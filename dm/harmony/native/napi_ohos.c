// HarmonyOS App entry (NAPI; the ArkTS side is in dm/harmony/app/entry/src/main/ets) for nxnative:
// the platform glue is carried over from the d2r-mobile HarmonyOS port — logging (stderr -> pipe -> hilog + file),
// lifecycle dispatch, the runtime dir / env setup, the XComponent surface callbacks and the PROT_EXEC probe.
// Every game-specific hook of that port is dropped (dm_custom, derived tables, RomFS overlay, the naming dialog's
// name rules, the dm_tc_* touch state machine) — nxnative's contract is front.h + dm.h only.
//
// start(files, src, lang): files = the app's files dir; src = the imported game folder holding
//   RomFS/ (the game's romfs), out/ (patched modules + fixups/gates tables) and shaders/ (SPIR-V bundles).
#include "../../host/dm.h"
#include "../../host/front.h"
#include "../../host/hle.h"
#include "../../android/vulkan_present.h"
#include "../../android/lifecycle_dispatch.h"
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>
#include <napi/native_api.h>
#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>

void front_android_set_window(void *win, int w, int h);
void front_android_set_refresh(float hz);
int dm_main(int argc, char **argv);
static const char *ev_run_all(char *r, size_t n);

#define HLOG(lvl, tag, ...) do { char b_[1024]; snprintf(b_, sizeof b_, __VA_ARGS__); OH_LOG_Print(LOG_APP, lvl, 0xD2E0, tag, "%{public}s", b_); } while (0)

static char g_files[1024], g_src[1024], g_runtime[1100];
static FILE *g_logf;

// ---- lifecycle: the UI only enqueues; suspend/resume runs on a host thread ----
static dm_lifecycle_dispatch g_lifecycle;
static pthread_once_t g_lifecycle_once = PTHREAD_ONCE_INIT;
static int g_lifecycle_error;
static void lifecycle_graphics(int on) { dm_gfx_suspend(on); HLOG(LOG_INFO, "dm-lifecycle", "graphics suspend=%d", on); }
static void lifecycle_init(void) {
    dm_lifecycle_ops ops = {dm_focus_set, dm_audio_suspend, lifecycle_graphics};
    g_lifecycle_error = dm_lifecycle_start(&g_lifecycle, ops);
    if (!g_lifecycle_error) pthread_setname_np(g_lifecycle.thread, "dm-lifecycle");
}
static int lifecycle_ready(void) { pthread_once(&g_lifecycle_once, lifecycle_init); return !g_lifecycle_error; }

// ---- log: stderr -> pipe -> boot.log + hilog (tag dm) ----
static void *log_pump(void *arg) {
    int fd = (int)(intptr_t)arg; char buf[4096]; size_t have = 0; ssize_t k;
    while ((k = read(fd, buf + have, sizeof buf - 1 - have)) > 0) {
        have += (size_t)k; buf[have] = 0; char *s = buf, *nl;
        while ((nl = strchr(s, '\n'))) { *nl = 0; OH_LOG_Print(LOG_APP, LOG_INFO, 0xD2E0, "dm", "%{public}s", s); if (g_logf) { fputs(s, g_logf); fputc('\n', g_logf); fflush(g_logf); } s = nl + 1; }
        have = strlen(s); memmove(buf, s, have);
        if (have == sizeof buf - 1) { OH_LOG_Print(LOG_APP, LOG_INFO, 0xD2E0, "dm", "%{public}s", buf); if (g_logf) { fputs(buf, g_logf); fflush(g_logf); } have = 0; }
    }
    return NULL;
}
static void log_init(void) {
    static int initialized; if (initialized) return; initialized = 1;
    char p[1200]; snprintf(p, sizeof p, "%s/boot.log", g_runtime); g_logf = fopen(p, "a");
    int fds[2]; if (pipe(fds)) return;
    dup2(fds[1], 2); close(fds[1]); setvbuf(stderr, NULL, _IOLBF, 0);
    pthread_t th; if (!pthread_create(&th, NULL, log_pump, (void *)(intptr_t)fds[0])) pthread_detach(th);
}
static void mkdirs(const char *p) {
    char b[1100]; snprintf(b, sizeof b, "%s", p);
    for (char *s = b + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(b, 0700); *s = '/'; }
    mkdir(b, 0700);
}

// ---- ArkTS callbacks (thread-safe functions): naming dialog, boot failure ----
static napi_threadsafe_function g_show_input, g_boot_failed;
typedef struct { int64_t id; char *s[4]; int max_chars; } input_req;
static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t input_done = PTHREAD_COND_INITIALIZER;
static int64_t next_request, active_request;
static int pending, result, output_capacity;
static char *output;

static void call_show_input(napi_env env, napi_value js, void *ctx, void *data) {
    (void)ctx; input_req *r = data; if (!env) goto out;
    napi_value argv[6]; napi_create_int64(env, r->id, &argv[0]);
    for (int i = 0; i < 4; i++) napi_create_string_utf8(env, r->s[i] ? r->s[i] : "", NAPI_AUTO_LENGTH, &argv[1 + i]);
    napi_create_int32(env, r->max_chars, &argv[5]);
    napi_value undef; napi_get_undefined(env, &undef); napi_call_function(env, undef, js, 6, argv, NULL);
out:
    for (int i = 0; i < 4; i++) free(r->s[i]);
    free(r);
}
static void call_boot_failed(napi_env env, napi_value js, void *ctx, void *data) {
    (void)ctx; if (!env) return;
    napi_value code, undef; napi_create_int32(env, (int)(intptr_t)data, &code); napi_get_undefined(env, &undef);
    napi_call_function(env, undef, js, 1, &code, NULL);
}
void front_android_boot_failed(int code) {
    if (g_boot_failed) napi_call_threadsafe_function(g_boot_failed, (void *)(intptr_t)code, napi_tsfn_nonblocking);
}
// front.h's software-keyboard entry: the calling thread blocks until the ArkTS dialog resolves.
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!out || out_cap <= 0 || !g_show_input) return -1;
    input_req *r = calloc(1, sizeof *r); if (!r) return -1;
    const char *src[] = {title, sub, guide, initial};
    for (int i = 0; i < 4; i++) r->s[i] = strdup(src[i] ? src[i] : "");
    r->max_chars = max_chars;
    pthread_mutex_lock(&input_lock);
    if (active_request) { pthread_mutex_unlock(&input_lock); for (int i = 0; i < 4; i++) free(r->s[i]); free(r); return -1; }
    r->id = active_request = ++next_request;
    pending = 1; result = 0; output = out; output_capacity = out_cap; out[0] = 0;
    pthread_mutex_unlock(&input_lock);
    if (napi_call_threadsafe_function(g_show_input, r, napi_tsfn_blocking) != napi_ok) {
        pthread_mutex_lock(&input_lock); pending = 0; active_request = 0; pthread_mutex_unlock(&input_lock); return -1;
    }
    pthread_mutex_lock(&input_lock);
    while (pending) pthread_cond_wait(&input_done, &input_lock);
    int accepted = result; output = NULL; active_request = 0;
    pthread_mutex_unlock(&input_lock);
    return accepted;
}

static int get_str(napi_env env, napi_value v, char *buf, size_t cap) { size_t n = 0; return napi_get_value_string_utf8(env, v, buf, cap, &n) == napi_ok ? (int)n : -1; }
static napi_value ret_int(napi_env env, int v) { napi_value r; napi_create_int32(env, v, &r); return r; }
static napi_value undefined(napi_env env) { napi_value r; napi_get_undefined(env, &r); return r; }
#define ARGS(n) size_t argc = n; napi_value argv[n]; napi_get_cb_info(env, info, &argc, argv, NULL, NULL)

static napi_value js_set_callbacks(napi_env env, napi_callback_info info) {
    ARGS(2); napi_value name;
    napi_create_string_utf8(env, "dmShowInput", NAPI_AUTO_LENGTH, &name);
    napi_create_threadsafe_function(env, argv[0], NULL, name, 0, 1, NULL, NULL, NULL, call_show_input, &g_show_input);
    napi_create_string_utf8(env, "dmBootFailed", NAPI_AUTO_LENGTH, &name);
    napi_create_threadsafe_function(env, argv[1], NULL, name, 0, 1, NULL, NULL, NULL, call_boot_failed, &g_boot_failed);
    napi_unref_threadsafe_function(env, g_show_input); napi_unref_threadsafe_function(env, g_boot_failed);
    return undefined(env);
}
static napi_value js_text_result(napi_env env, napi_callback_info info) {
    ARGS(3); int64_t id = 0; bool accepted = false; napi_get_value_int64(env, argv[0], &id); napi_get_value_bool(env, argv[1], &accepted);
    pthread_mutex_lock(&input_lock);
    if (pending && active_request == id) {   // stale callbacks never write into a new request
        result = accepted && get_str(env, argv[2], output, (size_t)output_capacity) >= 0;
        pending = 0; pthread_cond_signal(&input_done);
        HLOG(LOG_INFO, "dm-input", "completed id=%lld accepted=%d", (long long)id, result);
    }
    pthread_mutex_unlock(&input_lock);
    return undefined(env);
}

// The rewritten modules of this game, beside out/. Silksong: main + rtld + sdk + subsdk0 (per-game lists come from the build step later).
static const char *g_modules[] = {"main", "rtld", "sdk", "subsdk0"};

static void *host_thread(void *arg) {
    (void)arg;
    char out[1200]; snprintf(out, sizeof out, "%s/out", g_runtime);
    char shaders[1200], error[512] = {0}; snprintf(shaders, sizeof shaders, "%s/shaders", g_runtime);
    int graphics_rc = dm_android_vk_initialize(shaders, error, sizeof error);
    if (graphics_rc) { LOG("boot: Vulkan initialization failed %d: %s", graphics_rc, error); front_android_boot_failed(-6); return NULL; }
    char *argv[16]; int argc = 0;
    argv[argc++] = (char *)"dmhost"; argv[argc++] = out;
    for (unsigned i = 0; i < sizeof g_modules / sizeof g_modules[0]; i++) argv[argc++] = (char *)g_modules[i];
    argv[argc] = NULL;
    LOG("boot: dm_main %s (%d modules)", out, argc - 2);
    int rc = dm_main(argc, argv);
    LOG("boot: dm_main returned %d", rc);
    front_android_boot_failed(-7);
    return NULL;
}

// start(files, src, lang) -> number of resource files found (>0) or a negative error code
static napi_value js_start(napi_env env, napi_callback_info info) {
    ARGS(3); static int started; if (started) return ret_int(env, 0);
    char lg[64] = {0};
    get_str(env, argv[0], g_files, sizeof g_files); get_str(env, argv[1], g_src, sizeof g_src); get_str(env, argv[2], lg, sizeof lg);
    snprintf(g_runtime, sizeof g_runtime, "%s/dm", g_files); mkdirs(g_runtime);
    log_init();
    LOG("boot: HarmonyOS files %s, game data %s, system language %s", g_files, g_src, lg);
    {   // diagnostics: max frequency of each core (to find the big cores for thread pinning)
        char line[512] = {0}; int at = 0;
        for (int c = 0; c < 16; c++) { char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c); FILE *f = fopen(p, "r"); if (!f) continue;
            long khz = 0; if (fscanf(f, "%ld", &khz) == 1) at += snprintf(line + at, sizeof line - at, " %d:%ld", c, khz / 1000); fclose(f); }
        LOG("boot: CPU core max MHz%s", at ? line : " (unreadable)");
        char ev[512]; ev_run_all(ev, sizeof ev);
    }
    char romfs[1100], save[1100], temp[1100], runtime_link[1200];
    snprintf(romfs, sizeof romfs, "%s/RomFS", g_src);
    snprintf(save, sizeof save, "%s/save", g_files); snprintf(temp, sizeof temp, "%s/temp", g_files);
    mkdirs(save); mkdirs(temp);
    // the imported game folder also carries out/ and shaders/; point the runtime layout at it (no copy, same-partition rename is left to the staging step)
    snprintf(runtime_link, sizeof runtime_link, "%s/out", g_src);
    char out_dir[1200]; snprintf(out_dir, sizeof out_dir, "%s/out", g_runtime);
    mkdir(out_dir, 0700);   // ok if it exists
    setenv("DM_ROMFS", romfs, 0); setenv("DM_SAVE", save, 0); setenv("DM_TEMP", temp, 0);
    // debug: dm/env, one KEY=VALUE per line, applied before the defaults below
    { char ef[1200], ln[512]; snprintf(ef, sizeof ef, "%s/env", g_runtime); FILE *f = fopen(ef, "rb");
      while (f && fgets(ln, sizeof ln, f)) { ln[strcspn(ln, "\r\n")] = 0; char *eq = strchr(ln, '='); if (ln[0] == '#' || !eq || eq == ln) continue; *eq = 0; setenv(ln, eq + 1, 1); LOG("boot: env %s=%s", ln, eq + 1); }
      if (f) fclose(f); }
    // HarmonyOS defaults (Maleoon best practice, as the d2r port measured; files/dm/env still overrides)
    setenv("DM_3D_SCALE_GPU", "1", 0);
    setenv("DM_NVN_SYNC", "0", 0);            // shader translation on its own thread
    setenv("DM_GFX_DEFER_EVENTS", "2", 0);
    setenv("DM_GFX_READ_PREFLIGHT", "0", 0);
    setenv("DM_GFX_READBACK_CACHED", "1", 0); // readback buffers in host-cached memory (Maleoon)
    setenv("DM_GFX_PIPE_MEMO", "1", 0); setenv("DM_GFX_QUAD_DERIVATIVES", "1", 0); setenv("DM_GFX_WARP32", "1", 0); setenv("DM_GFX_NAN_MINMAX", "1", 0);
    setenv("DM_GFX_NATIVE_BC", "0", 0);       // Maleoon has no BC textures: software decode
    setenv("DM_GFX_NOLOAD", "1", 0);
    setenv("DM_PACE_ADAPT", "1", 0);          // adaptive present tier
    { char mk[1200]; snprintf(mk, sizeof mk, "%s/nvn-async", g_runtime); FILE *f = fopen(mk, "rb"); if (f) { setenv("DM_NVN_SYNC", fgetc(f) == '0' ? "1" : "0", 1); fclose(f); } }
    if (!getenv("DM_LANG") && *lg) setenv("DM_LANG", lg, 0);

    // required runtime files: every declared module's patched.bin + fixups + gates under <src>/out
    int n = 0;
    for (unsigned i = 0; i < sizeof g_modules / sizeof g_modules[0]; i++) {
        char required[1300];
        snprintf(required, sizeof required, "%s/%s.patched.bin", runtime_link, g_modules[i]);
        if (access(required, R_OK)) { LOG("boot: missing %s", required); return ret_int(env, -3); }
        snprintf(required, sizeof required, "%s/%s.fixups.tsv", runtime_link, g_modules[i]);
        if (access(required, R_OK)) { LOG("boot: missing %s", required); return ret_int(env, -3); }
        snprintf(required, sizeof required, "%s/%s.gates.tsv", runtime_link, g_modules[i]);
        if (access(required, R_OK)) { LOG("boot: missing %s", required); return ret_int(env, -3); }
        n++;
    }
    { char required[1300]; snprintf(required, sizeof required, "%s/shaders", g_src);
      DIR *sh = opendir(required); int have_shader = 0;
      if (sh) { struct dirent *e; while ((e = readdir(sh))) { if (strstr(e->d_name, ".xgs")) { have_shader = 1; break; } } closedir(sh); }
      if (!have_shader) { LOG("boot: no shader bundles in %s", required); return ret_int(env, -4); } }
    // the module dir dm_main reads is the imported folder's out/ — expose it through the runtime layout
    setenv("DM_MODULE_DIR", runtime_link, 0);

    if (!lifecycle_ready()) return ret_int(env, -8);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8u << 20);
    pthread_t th; int rc = pthread_create(&th, &a, host_thread, NULL); pthread_attr_destroy(&a);
    if (!rc) { started = 1; pthread_detach(th); }
    return ret_int(env, rc ? -1 : n);
}

// 0 unfocused, 1 background, 2 foreground, 3 active
static napi_value js_focus(napi_env env, napi_callback_info info) {
    ARGS(1); int32_t ev = 0; napi_get_value_int32(env, argv[0], &ev);
    if (lifecycle_ready()) dm_lifecycle_post(&g_lifecycle, ev);
    return undefined(env);
}
static napi_value js_refresh(napi_env env, napi_callback_info info) { ARGS(1); double hz = 0; napi_get_value_double(env, argv[0], &hz); front_android_set_refresh((float)hz); return undefined(env); }
// touchEvent / touchScreen / touchTick: the game's own touchscreen input is not wired yet
// (Silksong is a controller game; the d2r touch state machine is not carried over). Kept as harmless stubs so the ArkTS shell need not change.
static napi_value js_touch_event(napi_env env, napi_callback_info info) { (void)env; (void)info; return undefined(env); }
static napi_value js_touch_screen(napi_env env, napi_callback_info info) { (void)env; (void)info; return undefined(env); }
static napi_value js_touch_tick(napi_env env, napi_callback_info info) { (void)info; return ret_int(env, 0); }

// execProbe(): as loader.c does (write code into an anonymous page, then mprotect it read+execute), run two
// instructions; returns "ok" or the failing step and errno. Whether the system allows executable memory under
// release signing is confirmed on device with this; the result is shown on the start / error page.
#include <sys/mman.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
static sigjmp_buf ev_jb;
static void ev_segv(int s) { (void)s; siglongjmp(ev_jb, 1); }
static int ev_try(const char *name, int prot_first, int prot_then, int use_file, char *out, size_t on) {
    void *pg;
    if (use_file) {
        static const char *dirs[] = { "/data/storage/el2/base/haps/entry/files/ev_code.bin", "./ev_code.bin" };
        int fd = -1;
        for (unsigned i = 0; i < 2 && fd < 0; i++) fd = open(dirs[i], O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (fd < 0) { snprintf(out, on, "%s: open errno %d", name, errno); return 0; }
        uint32_t code[2] = {0x52800540, 0xd65f03c0};   // mov w0,#42; ret
        write(fd, code, 8); fsync(fd);
        pg = mmap(NULL, 0x4000, prot_first, MAP_PRIVATE, fd, 0);
        close(fd);
    } else {
        pg = mmap(NULL, 0x4000, prot_first, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    if (pg == MAP_FAILED) snprintf(out, on, "%s: mmap errno %d", name, errno);
    else {
        if (!use_file) { uint32_t *p = pg; p[0] = 0x52800540; p[1] = 0xd65f03c0; __builtin___clear_cache((char *)p, (char *)(p + 2)); }
        if (prot_then && mprotect(pg, 0x4000, prot_then)) snprintf(out, on, "%s: mprotect errno %d", name, errno);
        else {
            struct sigaction sa = {0}, old; sa.sa_handler = ev_segv; sigemptyset(&sa.sa_mask);
            sigaction(SIGSEGV, &sa, &old);
            int v = sigsetjmp(ev_jb, 1) ? -999 : ((int (*)(void))pg)();
            sigaction(SIGSEGV, &old, NULL);
            if (v == -999) snprintf(out, on, "%s: exec fault (SIGSEGV)", name);
            else snprintf(out, on, v == 42 ? "%s: ok" : "%s: bad result %d", name, v);
        }
        munmap(pg, 0x4000);
    }
    if (use_file) { unlink("/data/storage/el2/base/haps/entry/files/ev_code.bin"); unlink("./ev_code.bin"); }
    return 1;
}
static const char *ev_run_all(char *r, size_t n) {
    char *p = r; size_t left = n;
    struct { const char *name; int first, then, file; } v[] = {
        {"anon RW->RX", PROT_READ | PROT_WRITE, PROT_READ | PROT_EXEC, 0},
        {"anon RWX direct", PROT_READ | PROT_WRITE | PROT_EXEC, 0, 0},
        {"anon RX only", PROT_READ | PROT_EXEC, 0, 0},
        {"anon EXEC only", PROT_EXEC, 0, 0},
        {"file RX", PROT_READ | PROT_EXEC, 0, 1},
        {"file RWX", PROT_READ | PROT_WRITE | PROT_EXEC, 0, 1},
        {"file RW->RX", PROT_READ | PROT_WRITE, PROT_READ | PROT_EXEC, 1},
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        char line[128]; if (!ev_try(v[i].name, v[i].first, v[i].then, v[i].file, line, sizeof line)) break;
        int k = snprintf(p, left, "%s%s", line, i + 1 < sizeof v / sizeof v[0] ? "; " : ""); p += k; left -= k;
        LOG("boot: exec memory probe %s", line);
    }
    return r;
}
static napi_value js_exec_probe(napi_env env, napi_callback_info info) {
    (void)info; char r[512];
    napi_value out; napi_create_string_utf8(env, ev_run_all(r, sizeof r), NAPI_AUTO_LENGTH, &out); return out;
}
// Debug FPS: presented frame count (nvn_vk.c mtl_present_count); ArkTS shows the per-second difference
uint64_t mtl_present_count(void);
static napi_value js_present_count(napi_env env, napi_callback_info info) {
    (void)info; napi_value r; napi_create_double(env, (double)mtl_present_count(), &r); return r;
}
// ---- XComponent window callbacks -> presenter ----
static void surface_set(OH_NativeXComponent *c, void *window) {
    uint64_t w = 0, h = 0;
    if (window) OH_NativeXComponent_GetXComponentSize(c, window, &w, &h);
    dm_android_vk_set_window((ANativeWindow *)window);
    front_android_set_window(window, (int)w, (int)h);
    LOG("front: surface %s %llux%llu", window ? "ready" : "gone", (unsigned long long)w, (unsigned long long)h);
}
static void on_created(OH_NativeXComponent *c, void *window) { surface_set(c, window); }
static void on_changed(OH_NativeXComponent *c, void *window) { surface_set(c, window); }
static void on_destroyed(OH_NativeXComponent *c, void *window) { (void)window; surface_set(c, NULL); }
static void on_touch(OH_NativeXComponent *c, void *window) { (void)c; (void)window; }
static OH_NativeXComponent_Callback g_xcb = {on_created, on_changed, on_destroyed, on_touch};

static napi_value init(napi_env env, napi_value exports) {
    napi_property_descriptor d[] = {
        {"start", NULL, js_start, NULL, NULL, NULL, napi_default, NULL},
        {"focus", NULL, js_focus, NULL, NULL, NULL, napi_default, NULL},
        {"refresh", NULL, js_refresh, NULL, NULL, NULL, napi_default, NULL},
        {"touchEvent", NULL, js_touch_event, NULL, NULL, NULL, napi_default, NULL},
        {"touchScreen", NULL, js_touch_screen, NULL, NULL, NULL, napi_default, NULL},
        {"touchTick", NULL, js_touch_tick, NULL, NULL, NULL, napi_default, NULL},
        {"execProbe", NULL, js_exec_probe, NULL, NULL, NULL, napi_default, NULL},
        {"presentCount", NULL, js_present_count, NULL, NULL, NULL, napi_default, NULL},
        {"setCallbacks", NULL, js_set_callbacks, NULL, NULL, NULL, napi_default, NULL},
        {"textResult", NULL, js_text_result, NULL, NULL, NULL, napi_default, NULL},
    };
    napi_define_properties(env, exports, sizeof d / sizeof d[0], d);
    // When an XComponent loads this library, exports carries the native component
    napi_value xobj; OH_NativeXComponent *xc = NULL;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &xobj) == napi_ok && napi_unwrap(env, xobj, (void **)&xc) == napi_ok && xc)
        OH_NativeXComponent_RegisterCallback(xc, &g_xcb);
    return exports;
}
static napi_module g_module = {1, 0, NULL, init, "dm", NULL, {0}};
__attribute__((constructor)) static void register_module(void) { napi_module_register(&g_module); }
