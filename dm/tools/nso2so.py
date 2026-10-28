#!/usr/bin/env python3
# nso2so.py: pack a rewritten module (patch_nso.py output) as a real AArch64 ELF shared object, so the
# HarmonyOS app can run it through dlopen -- the only executable-memory path the sandbox grants
# (measured on device 2026-10-28: anon mprotect PROT_EXEC EINVAL, file RX EACCES, bundle .so via the
# system linker only). Layout mirrors loader.c's map_image: [patch area][gap][image text RX][ro R][data+bss RW].
# All 530k RELR/RELA/JMPREL relocations stay for the host to apply (their targets all land in the RW data
# segment -- verified on Silksong main), so the .so needs no dynamic relocations of its own.
# Usage: nso2so.py <module.patched.bin> <module.fixups.tsv> <out.so>
import os, struct, sys

PAGE = 0x4000
ET_DYN, EM_AARCH64, EV_CURRENT = 3, 183, 1
PT_LOAD, PT_DYNAMIC = 1, 2
PF_X, PF_W, PF_R = 1, 2, 4


def fixups(path):
    kv = {}
    for ln in open(path):
        if ln.startswith('# '):
            p = ln[2:].strip().split('\t')
            if len(p) == 2:
                try: kv[p[0]] = int(p[1], 0)
                except ValueError: pass   # BUILD_ID is a hex string, not a number
    return kv


def main(bin_path, fx_path, out_path):
    img = open(bin_path, 'rb').read()
    fx = fixups(fx_path)
    patch_off = fx['PATCH_OFF']
    text_end = (fx['TEXT_END'] + PAGE - 1) & ~(PAGE - 1)
    ro_off, data_off = fx['RO_OFF'], fx['DATA_OFF']
    file_len = fx.get('FILE_IMAGE_LEN', len(img))
    if patch_off >= 0:
        # embedded patch area: one image at vaddr 0; patch pages sit at patch_off inside it
        image_size = fx.get('IMAGE_SIZE', (len(img) + PAGE - 1) & ~(PAGE - 1))
        pre, file_img, patch = 0, img, b''
        patch_len = 0
    else:
        image_size = fx['IMAGE_SIZE']
        file_len = fx['FILE_IMAGE_LEN']
        pre = -patch_off                       # patch area sits before the image base
        file_img, patch = img[:file_len], img[file_len:]
    patch_len = (len(patch) + PAGE - 1) & ~(PAGE - 1)
    if patch_len > pre: sys.exit(f'patch area {patch_len:#x} > pre-base gap {pre:#x}')

    # vaddr layout: patch [0, patch_len) RX ; gap ; image at vaddr pre (+0 text RX, +ro R, +data RW+bss)
    # file layout:  [ehdr+phdrs @ 0x4000][patch @ 0x8000][text][ro][data]
    eh = 0x40
    foff = PAGE                               # first data chunk at file offset 0x4000 (page aligned)
    chunks = []                               # (vaddr, filesz, memsz, flags, filedata)
    def put(vaddr, data, memsz, flags):
        nonlocal foff
        # OHOS ldso requires p_offset == p_vaddr (mod page) or dlopen fails (measured on device)
        foff += (vaddr - foff) % PAGE
        chunks.append((vaddr, foff, len(data), memsz, flags, data))
        foff += len(data)
        foff = (foff + PAGE - 1) & ~(PAGE - 1)

    if patch_len: put(0, patch + b'\0' * (patch_len - len(patch)), patch_len, PF_R | PF_X)
    put(pre, file_img[:text_end], text_end, PF_R | PF_X)
    # the ro range may start inside the text range's last page (16K kernel pages cannot express a
    # sub-page R region inside an RX one); the overlapping bytes are already mapped by the RX chunk
    # (same file_img data, R is a subset of RX), so emit the ro chunk only for the tail beyond text.
    if data_off > text_end:
        put(pre + max(ro_off, text_end), file_img[max(ro_off, text_end):data_off],
            data_off - max(ro_off, text_end), PF_R)
    if patch_off >= 0:
        # embedded patch: data up to the patch pages is RW, the patch pages themselves RX
        # (loader.c mprotects [patch_off, end) read+execute; the key page is always below patch_off)
        pal = patch_off & ~(PAGE - 1)
        put(pre + data_off, file_img[data_off:pal], pal - data_off, PF_R | PF_W)
        put(pre + pal, file_img[pal:file_len], image_size - pal, PF_R | PF_X)
    else:
        put(pre + data_off, file_img[data_off:file_len], image_size - data_off, PF_R | PF_W)

    # dynamic segment + minimal symbol tables: OHOS ldso rejects a .so whose dynamic has no
    # SYMTAB/STRTAB/HASH|GNU_HASH (measured on device: dlopen fails with "No error information").
    # Emit a trailing RW page holding the dynamic array followed by empty tables
    # (one null Elf64_Sym, one "\0" string, empty sysv+gnu hash -- all consistent for zero symbols).
    dyn_vaddr = (pre + image_size + PAGE - 1) & ~(PAGE - 1)
    tab = 0x80                                  # tables sit at +0x80 inside the page, after the dyn array
    sysv_h = struct.pack('<IIII', 1, 1, 0, 0)   # nbucket=1 nchain=1 bucket[0]=0 chain[0]=0
    gnu_h = struct.pack('<IIIIQI', 1, 1, 1, 0, 0, 0)  # nbuckets=1 symoff=1 bloom_sz=1 shift=0 bloom[0]=0 buckets[0]=0
    sym0 = b'\0' * 24                          # one null Elf64_Sym
    str0 = b'\0'
    dyn = b''.join(struct.pack('<QQ', t, v) for t, v in [
        (0x6ffffef5, dyn_vaddr + tab),                       # DT_GNU_HASH
        (4, dyn_vaddr + tab + len(gnu_h)),                   # DT_HASH
        (6, dyn_vaddr + tab + len(gnu_h) + len(sysv_h)),     # DT_SYMTAB
        (0xb, 24),                                           # DT_SYMENT
        (5, dyn_vaddr + tab + len(gnu_h) + len(sysv_h) + len(sym0)),  # DT_STRTAB
        (0xa, 1),                                            # DT_STRSZ
        (0, 0)])                                             # DT_NULL
    tail = dyn + b'\0' * (tab - len(dyn)) + gnu_h + sysv_h + sym0 + str0
    put(dyn_vaddr, tail, len(tail), PF_R | PF_W)
    dyn_off = chunks[-1][1]

    nph = len(chunks) + 1
    phdrs = b''
    for vaddr, off, fsz, msz, fl, _ in chunks:
        phdrs += struct.pack('<IIQQQQQQ', PT_LOAD, fl, off, vaddr, vaddr, fsz, msz, PAGE)
    phdrs += struct.pack('<IIQQQQQQ', PT_DYNAMIC, PF_R | PF_W, dyn_off, dyn_vaddr, dyn_vaddr, len(dyn), len(dyn), 8)

    # OHOS ldso rejects section-less shared objects (measured on device: dlopen EINVAL), so emit a
    # minimal section table after all chunk data: NULL + one PROGBITS name + shstrtab.
    ehdr = struct.pack('<16sHHIQQQIHHHHHH',
        b'\x7fELF\x02\x01\x01' + b'\0' * 9, ET_DYN, EM_AARCH64, EV_CURRENT,
        0, eh, 0, 0, eh, 56, nph, 64, 3, 2)
    out = bytearray()
    out += ehdr + phdrs
    out += b'\0' * (chunks[0][1] - len(out))
    for vaddr, off, fsz, msz, fl, data in chunks:
        out += b'\0' * (off - len(out)); out += data
    shstr = b'\0.dm\0.shstrtab\0'
    sh_off = len(out)
    out += b'\0' * 64
    out += struct.pack('<IIQQQQIIQQ', 1, 1, 0, 0, sh_off + 3 * 64 + 1, 3, 0, 0, 1, 0)
    out += struct.pack('<IIQQQQIIQQ', 5, 3, 0, 0, sh_off + 3 * 64, len(shstr), 0, 0, 1, 0)
    out += shstr
    # e_shoff points into the trailing area; patch the header field at 0x28
    struct.pack_into('<Q', out, 0x28, sh_off)
    with open(out_path, 'wb') as f: f.write(bytes(out))
    total_v = pre + image_size
    print(f'{os.path.basename(out_path)}: loads={len(chunks)} patch@{0:#x} base@{pre:#x} image {image_size:#x} '
          f'(text {text_end:#x} ro {data_off - ro_off:#x} data+bss {image_size - data_off:#x}) '
          f'reservation {total_v:#x}')


if __name__ == '__main__':
    main(*sys.argv[1:4])
