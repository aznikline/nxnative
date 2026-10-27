#!/usr/bin/env python3
"""读取宿主 Makefile 的源清单，输出 CMake 变量；不处理任何游戏内容。
Read source lists from the host Makefile into CMake variables; no game content.
"""
import argparse
from pathlib import Path
import re


def source_lists(makefile):
    import glob
    src, libraries = [], []
    root = makefile.parent
    def expand(text):
        # literal files plus $(wildcard pat) globs (nxnative uses one-line SRC=/GFX= lists; d2r uses multi-line)
        out = []
        for w in re.findall(r"\$\(wildcard ([^)]+)\)|(\S+)", text):
            pat, lit = w
            if pat:
                out += sorted(glob.glob(str(root / pat)))
            elif lit and lit.endswith(('.c', '.S')):
                out.append(str(root / lit))
        return out
    for line in makefile.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^(SRC|GFX)\s*=", line)
        if m:
            text = line.split('=', 1)[1].strip().rstrip("\\").strip()
            cont = line.rstrip().endswith("\\")
            for p in expand(text):
                st = Path(p).stem
                if st in ("main",) or st.endswith(("_mac", "_metal")) or p.endswith((".m", ".mm")):
                    continue
                src.append(Path(p).resolve())
            while cont:
                pass   # nxnative lists are single-line; multi-line continuation handled by glob anyway
                break
        if line.startswith("LIBDEFLATE="):
            libraries = [(root / s).resolve() for s in re.findall(r"[A-Za-z0-9_./-]+\.c", line)]
    if not src:
        raise ValueError("Host Makefile source lists changed; update the Android parser")
    return src, libraries


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("makefile", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source, libraries = source_lists(args.makefile)
    text = ""
    for name, paths in (("HOST_SRCS", source), ("LIBDEFLATE_SRCS", libraries)):
        text += "set(" + name + "\n" + "\n".join('  "' + p.as_posix() + '"' for p in paths) + "\n)\n"
    args.output.write_text(text, encoding="utf-8", newline="\n")
