// PROT_EXEC policy variants probe for HarmonyOS (run inside the app domain or hdc shell).
// Prints which memory-executable paths the kernel allows.
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

static void try_(const char *name, int prot_first, int prot_then, int use_file) {
    char out[160];
    void *pg;
    if (use_file) {
        int fd = open("/data/local/tmp/ev_code.bin", O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (fd < 0) { printf("%s: open errno %d\n", name, errno); return; }
        uint32_t code[2] = {0x52800540, 0xd65f03c0};   // mov w0,#42; ret
        write(fd, code, 8); fsync(fd);
        pg = mmap(NULL, 0x4000, prot_first, MAP_PRIVATE, fd, 0);
    } else {
        pg = mmap(NULL, 0x4000, prot_first, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    if (pg == MAP_FAILED) { snprintf(out, sizeof out, "mmap errno %d", errno); }
    else {
        if (!use_file) { uint32_t *p = pg; p[0] = 0x52800540; p[1] = 0xd65f03c0; __builtin___clear_cache((char *)p, (char *)(p + 2)); }
        if (prot_then && mprotect(pg, 0x4000, prot_then)) snprintf(out, sizeof out, "mprotect errno %d", errno);
        else { int v = ((int (*)(void))pg)(); snprintf(out, sizeof out, v == 42 ? "ok" : "bad %d", v); }
        munmap(pg, 0x4000);
    }
    if (use_file) unlink("/data/local/tmp/ev_code.bin");
    printf("%-34s %s\n", name, out);
}

int main(void) {
    try_("anon RW->RX", PROT_READ | PROT_WRITE, PROT_READ | PROT_EXEC, 0);
    try_("anon RWX direct", PROT_READ | PROT_WRITE | PROT_EXEC, 0, 0);
    try_("anon RX only", PROT_READ | PROT_EXEC, 0, 0);
    try_("anon EXEC only", PROT_EXEC, 0, 0);
    try_("file RX", PROT_READ | PROT_EXEC, 0, 1);
    try_("file RWX", PROT_READ | PROT_WRITE | PROT_EXEC, 0, 1);
    try_("file RW->RX", PROT_READ | PROT_WRITE, PROT_READ | PROT_EXEC, 1);
    return 0;
}
