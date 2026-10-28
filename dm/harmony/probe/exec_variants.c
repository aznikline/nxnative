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

int main2(void);
int main(void) {
    try_("anon RW->RX", PROT_READ | PROT_WRITE, PROT_READ | PROT_EXEC, 0);
    try_("anon RWX direct", PROT_READ | PROT_WRITE | PROT_EXEC, 0, 0);
    try_("anon RX only", PROT_READ | PROT_EXEC, 0, 0);
    try_("anon EXEC only", PROT_EXEC, 0, 0);
    try_("file RX", PROT_READ | PROT_EXEC, 0, 1);
    try_("file RWX", PROT_READ | PROT_WRITE | PROT_EXEC, 0, 1);
    try_("file RW->RX", PROT_READ | PROT_WRITE, PROT_READ | PROT_EXEC, 1);
    return main2();
}

// ---- round 2: signed-bundle .so mappings (the only pages that carry an exec label on HarmonyOS) ----
// If any of these work, the loader can ship the rewritten image as a signed .so inside the HAP and
// map it read+execute — at MAP_FIXED if the kernel honors exec maps at a chosen address.
static void try_bundle(const char *name, const char *path, int prot, uint64_t fixed) {
    char out[160];
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("%-40s open errno %d\n", name, errno); return; }
    int flags = MAP_PRIVATE | (fixed ? MAP_FIXED : 0);
    void *pg = mmap((void *)fixed, 0x4000, prot, flags, fd, 0);
    if (pg == MAP_FAILED) snprintf(out, sizeof out, "mmap errno %d", errno);
    else { snprintf(out, sizeof out, "mapped at %p", pg); munmap(pg, 0x4000); }
    close(fd);
    printf("%-40s %s\n", name, out);
}

static void try_dlopen(const char *path) {
    void *h = dlopen(path, RTLD_NOW);
    printf("%-40s %s\n", path, h ? "dlopen ok" : dlerror());
}

int main2(void) {
    // candidates: the app's own signed libs — found at runtime via /proc/self/maps
    FILE *f = fopen("/proc/self/maps", "r"); char line[512], so[256] = "";
    while (f && fgets(line, sizeof line, f)) if (strstr(line, ".so") && strstr(line, "bundle")) { sscanf(line, "%*[^ ] %*[^ ] %*[^ ] %*[^ ] %*[^ ] %255s", so); break; }
    if (f) fclose(f);
    printf("bundle so found: %s\n", so[0] ? so : "(none — fill the path manually)");
    if (!so[0]) snprintf(so, sizeof so, "/data/app/el1/bundle.public/arm64/libdm.so");
    try_bundle("signed .so RX", so, PROT_READ | PROT_EXEC, 0);
    try_bundle("signed .so RWX", so, PROT_READ | PROT_WRITE | PROT_EXEC, 0);
    try_bundle("signed .so RW->RX mmap", so, PROT_READ | PROT_WRITE, 0);
    try_bundle("signed .so RX MAP_FIXED", so, PROT_READ | PROT_EXEC, 0x5000000000ULL);
    try_dlopen(so);
    return 0;
}
