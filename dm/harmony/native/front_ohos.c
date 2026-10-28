// HarmonyOS front end (the HarmonyOS implementation of nxnative's front.h): the platform glue is carried over
// from the d2r-mobile HarmonyOS port (window bridging, display refresh tracking, thread QoS / pinning and the
// OpenGTX / FAST-Kit registration); every game-specific hook of that port (dm_tc_* touch controls, dm_ui_* HUD,
// front_game_res and friends) is dropped — nxnative's front contract is the ten calls in ../../host/front.h.
#include "../../host/dm.h"
#include "../../host/front.h"
#include "../../host/dm_input.h"
#include <math.h>
#include <stdatomic.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <sched.h>
#include <qos/qos.h>
#include <stdbool.h>
#include <window_manager/oh_display_manager.h>
#include <dlfcn.h>
#if __has_include(<graphics_game_sdk/opengtx_base.h>)
#include <graphics_game_sdk/opengtx_base.h>
#define DM_HAVE_OPENGTX 1
#else
#define DM_HAVE_OPENGTX 0   // this sysroot (API < 26) has no OpenGTX kit header: the whole block is off, as when the library is absent
#endif
#include <unistd.h>

static void *_Atomic g_window;
static _Atomic int g_screen_w, g_screen_h;
void front_android_set_window(void *win, int w, int h) { atomic_store(&g_window, win); if (w > 0 && h > 0) { atomic_store(&g_screen_w, w); atomic_store(&g_screen_h, h); } }

static _Atomic int g_refresh_millihz;
void front_android_set_refresh(float hz) {
    atomic_store(&g_refresh_millihz, isfinite(hz) && hz > 0 && hz < 1000 ? (int)lroundf(hz * 1000) : 0);
}
// The display-service refresh rate can change; on query failure retain the ArkTS startup snapshot or last valid value.
static uint32_t g_refresh_listener_index;
static bool g_refresh_listener_registered;
static bool g_refresh_register_warned, g_refresh_unregister_warned;
static _Atomic bool g_refresh_read_warned;
static _Atomic unsigned g_refresh_change_logs;

static void front_refresh_update(void) {
    uint32_t hz = 0;
    int r = OH_NativeDisplayManager_GetDefaultDisplayRefreshRate(&hz);
    if (r != DISPLAY_MANAGER_OK || hz == 0 || hz >= 1000) {
        if (!atomic_exchange(&g_refresh_read_warned, true))
            LOG("Display refresh query failed: rc=%d hz=%u; retaining cached refresh", r, hz);
        return;
    }
    int previous = atomic_load(&g_refresh_millihz);
    front_android_set_refresh((float)hz);
    if (previous != (int)hz * 1000 && atomic_fetch_add(&g_refresh_change_logs, 1) < 8)
        LOG("Display refresh updated: %.3f -> %u Hz", previous / 1000.0, hz);
}
static void front_refresh_changed(uint64_t display_id) { (void)display_id; front_refresh_update(); }
static void front_refresh_start(void) {
    if (!g_refresh_listener_registered) {
        int r = OH_NativeDisplayManager_RegisterDisplayChangeListener(front_refresh_changed, &g_refresh_listener_index);
        if (r == DISPLAY_MANAGER_OK) g_refresh_listener_registered = true;
        else if (!g_refresh_register_warned) { g_refresh_register_warned = true; LOG("Display refresh listener registration failed: rc=%d; using snapshot", r); }
    }
    front_refresh_update();
}
static void front_refresh_stop(void) {
    if (!g_refresh_listener_registered) return;
    int r = OH_NativeDisplayManager_UnregisterDisplayChangeListener(g_refresh_listener_index);
    if (r == DISPLAY_MANAGER_OK) g_refresh_listener_registered = false;
    else if (!g_refresh_unregister_warned) { g_refresh_unregister_warned = true; LOG("Display refresh listener removal failed: rc=%d", r); }
}

void front_pin_fast(const char *who);
void front_gtx_register(int who);
void front_gtx_scene(int scene);
void front_gtx_frame(void);

// ---- front.h contract ----

void *front_metal_layer(void) { return atomic_load(&g_window); }   // the OHNativeWindow* the Vulkan layer wraps into a VK_OHOS_surface

int front_screen_max_fps(void) { return atomic_load(&g_refresh_millihz) / 1000; }
double front_display_refresh(char *name, size_t n) {
    if (name && n) *name = 0;
    return atomic_load(&g_refresh_millihz) / 1000.0;
}
void front_screen_match(int fps) { (void)fps; }   // HarmonyOS does not let apps switch display modes

// front_text_input lives in napi_ohos.c (it needs the NAPI bridge to the ArkTS dialog).

void front_run(void (*guest)(void)) {
    if (front_metal_layer()) {
        front_refresh_start();
        front_pin_fast("game main thread");
        front_gtx_register(0);
    }
    guest();
    front_refresh_stop();
}

// ---- thread QoS / pinning (from the d2r port; HarmonyOS-generic) ----
void front_pin_keep(unsigned mask);
void front_pin_fast(const char *who) {
    { const char *q = getenv("DM_QOS"); if (!(q && *q == '0')) { int r = OH_QoS_SetThreadQoS(QOS_USER_INTERACTIVE); LOG("QoS: %s (tid %d) -> USER_INTERACTIVE, rc %d", who, (int)gettid(), r); } }
    const char *e = getenv("DM_PIN_FAST"); unsigned mask = 0;
    if (!e || !*e || (*e == '0' && !e[1])) return;
    mask = (unsigned)strtoul(e, NULL, 16);
    if (!mask) { long best = 0, all = 0;
        for (int c = 0; c < 32; c++) { char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c); FILE *f = fopen(p, "r"); if (!f) continue;
            long v = 0; if (fscanf(f, "%ld", &v) == 1 && v > 0) { all |= 1L << c; if (v > best) { best = v; mask = 0; } if (v == best) mask |= 1u << c; } fclose(f); }
        if ((long)mask == all) mask = 0; }
    if (!mask) return;
    cpu_set_t set; CPU_ZERO(&set); for (int c = 0; c < 32; c++) if (mask >> c & 1) CPU_SET(c, &set);
    int r = sched_setaffinity(0, sizeof set, &set);
    LOG("pin: %s (tid %d) -> mask %#x, rc %d", who, (int)gettid(), mask, r);
    static __thread unsigned kept; kept = r ? 0 : mask;
    front_pin_keep(kept);
}
// The system (QoS / resource scheduling) rewrites the thread's allowed cores: check once per frame and re-pin if changed.
static __thread unsigned g_pin_mask; static __thread unsigned g_pin_drifts;
void front_pin_keep(unsigned mask) { g_pin_mask = mask; }
void front_pin_check(void) {
    if (!g_pin_mask) return;
    cpu_set_t cur; CPU_ZERO(&cur); if (sched_getaffinity(0, sizeof cur, &cur)) return;
    unsigned have = 0; for (int c = 0; c < 32; c++) if (CPU_ISSET(c, &cur)) have |= 1u << c;
    if (have == g_pin_mask) return;
    cpu_set_t set; CPU_ZERO(&set); for (int c = 0; c < 32; c++) if (g_pin_mask >> c & 1) CPU_SET(c, &set);
    int r = sched_setaffinity(0, sizeof set, &set);
    if (++g_pin_drifts <= 5 || !(g_pin_drifts & 1023)) LOG("pin: tid %d allowed cores became %#x (drift %u), re-pin %#x rc %d", (int)gettid(), have, g_pin_drifts, g_pin_mask, r);
}

// ---- OpenGTX / FAST-Kit (game acceleration): generic, loaded at run time and skipped when unsupported ----
#if DM_HAVE_OPENGTX
static struct {
    void *lib; OpenGTX_Context *ctx; int main_tid, render_tid, key_tids[5], nkeys, active, scene;
    OpenGTX_Context *(*create)(OpenGTX_DeviceInfoCallback);
    OpenGTX_ErrorCode (*config)(OpenGTX_Context *, const OpenGTX_ConfigDescription *);
    OpenGTX_ErrorCode (*activate)(OpenGTX_Context *);
    OpenGTX_ErrorCode (*scene_info)(OpenGTX_Context *, const OpenGTX_GameSceneInfo *);
    OpenGTX_ErrorCode (*frame_info)(OpenGTX_Context *, const OpenGTX_FrameRenderInfo *);
} g_gtx;
static pthread_mutex_t g_gtx_mu = PTHREAD_MUTEX_INITIALIZER;
static void gtx_temp(OpenGTX_TempLevel level) { LOG("OpenGTX: device temp level %d", (int)level); }
static void gtx_try_activate(void) {
    if (g_gtx.active || !g_gtx.main_tid || !g_gtx.render_tid) return;
    const char *e = getenv("DM_OPENGTX"); if (e && *e == '0') return;
    g_gtx.active = -1;
    g_gtx.lib = dlopen("libopengtx.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_gtx.lib) { LOG("OpenGTX: lib not found (%s), skipped", dlerror()); return; }
    g_gtx.create = (OpenGTX_Context *(*)(OpenGTX_DeviceInfoCallback))dlsym(g_gtx.lib, "HMS_OpenGTX_CreateContext");
    g_gtx.config = (OpenGTX_ErrorCode (*)(OpenGTX_Context *, const OpenGTX_ConfigDescription *))dlsym(g_gtx.lib, "HMS_OpenGTX_SetConfiguration");
    g_gtx.activate = (OpenGTX_ErrorCode (*)(OpenGTX_Context *))dlsym(g_gtx.lib, "HMS_OpenGTX_Activate");
    g_gtx.scene_info = (OpenGTX_ErrorCode (*)(OpenGTX_Context *, const OpenGTX_GameSceneInfo *))dlsym(g_gtx.lib, "HMS_OpenGTX_DispatchGameSceneInfo");
    g_gtx.frame_info = (OpenGTX_ErrorCode (*)(OpenGTX_Context *, const OpenGTX_FrameRenderInfo *))dlsym(g_gtx.lib, "HMS_OpenGTX_DispatchFrameRenderInfo");
    if (!g_gtx.create || !g_gtx.config || !g_gtx.activate) { LOG("OpenGTX: incomplete API, skipped"); return; }
    g_gtx.ctx = g_gtx.create(gtx_temp);
    if (!g_gtx.ctx) { LOG("OpenGTX: unsupported device (NULL context), skipped"); return; }
    OpenGTX_ConfigDescription c; memset(&c, 0, sizeof c);
    c.mode = SCENE_MODE; c.targetFPS = 60;
    c.packageName = (char *)"com.nxnative.game"; c.appVersion = (char *)"1.0.0";
    c.engineType = OTHERS_ENGINE; c.engineVersion = (char *)"dm"; c.gameType = RPG; c.pictureQualityMaxLevel = FHD;
    int dm = atomic_load(&g_screen_w), sh = atomic_load(&g_screen_h);
    c.resolutionMaxValue.height = sh > dm ? dm : sh; c.resolutionMaxValue.width = sh > dm ? sh : dm;
    if (c.resolutionMaxValue.height < 360) { c.resolutionMaxValue.height = 1320; c.resolutionMaxValue.width = 2120; }
    c.gameMainThreadId = g_gtx.main_tid; c.gameRenderThreadId = g_gtx.render_tid;
    for (int i = 0; i < 5; i++) c.gameKeyThreadIds[i] = i < g_gtx.nkeys ? g_gtx.key_tids[i] : 0;
    c.vulkanSupport = true;
    OpenGTX_ErrorCode r1 = g_gtx.config(g_gtx.ctx, &c), r2 = r1 == OPENGTX_SUCCESS ? g_gtx.activate(g_gtx.ctx) : r1;
    LOG("OpenGTX: main %d render %d key %d, target 60 FPS, %dx%d -> config %d activate %d", g_gtx.main_tid, g_gtx.render_tid, g_gtx.nkeys,
        c.resolutionMaxValue.width, c.resolutionMaxValue.height, (int)r1, (int)r2);
    if (r2 == OPENGTX_SUCCESS) g_gtx.active = 1;
}
// who: 0 game main thread, 1 render (translation) thread, 2 other key threads (present / audio)
void front_gtx_register(int who) {
    pthread_mutex_lock(&g_gtx_mu);
    int tid = (int)gettid();
    if (who == 0) g_gtx.main_tid = tid; else if (who == 1) g_gtx.render_tid = tid; else if (g_gtx.nkeys < 5) g_gtx.key_tids[g_gtx.nkeys++] = tid;
    gtx_try_activate();
    pthread_mutex_unlock(&g_gtx_mu);
}
// scene: 1 in a run 2 loading 3 front-end pages
void front_gtx_scene(int scene) {
    if (g_gtx.active != 1 || !g_gtx.scene_info || scene == g_gtx.scene) return;
    pthread_mutex_lock(&g_gtx_mu);
    g_gtx.scene = scene;
    OpenGTX_GameSceneInfo si; memset(&si, 0, sizeof si);
    si.sceneID = scene == 1 ? PLAYING : scene == 2 ? LOADING : GAME_INTERFACE;
    si.description = (char *)(scene == 1 ? "in run" : scene == 2 ? "loading" : "menus");
    si.recommendFPS = 60; si.minFPS = 30; si.maxFPS = 60;
    int dm = atomic_load(&g_screen_w), sh = atomic_load(&g_screen_h);
    si.resolutionCurValue.height = sh > dm ? dm : sh; si.resolutionCurValue.width = sh > dm ? sh : dm;
    OpenGTX_ErrorCode r = g_gtx.scene_info(g_gtx.ctx, &si);
    LOG("OpenGTX: scene -> %s, rc %d", si.description, (int)r);
    pthread_mutex_unlock(&g_gtx_mu);
}
void front_gtx_frame(void) {
    if (g_gtx.active != 1 || !g_gtx.frame_info) return;
    OpenGTX_FrameRenderInfo fi; memset(&fi, 0, sizeof fi); g_gtx.frame_info(g_gtx.ctx, &fi);
}
#else
void front_gtx_register(int who) { (void)who; }
void front_gtx_scene(int scene) { (void)scene; }
void front_gtx_frame(void) {}
#endif
