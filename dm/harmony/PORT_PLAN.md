# nxnative → HarmonyOS 移植工作分解（Silksong 首目标）

## 已从 d2r-mobile 搬入的层
- `dm/harmony/`：ArkTS app 壳 + native (front_ohos.c 341 / napi_ohos.c 403 / touch_ohos.c 517 / gameperf_ohos.c 108) + android_shim + probe + tools
- `dm/android/`：compat（mach/GCD/CommonCrypto 薄 shim）+ tools（asm_elf.py / host_sources.py）+ vulkan_present.cpp + overlay_validate.c
- `dm/gfx/vulkan/`：xbg 引擎（~8k 行）+ nvn_vk.c（mtl_* 契约实现，和 nvn_mtl.m 同构）

## 需要适配的接口差异
- front.h：nxnative 比 d2r 少 front_game_res / front_text_field / front_touch / front_save_failed / front_load_kickstart；多 front_screen_max_fps / front_screen_match → front_ohos.c 需裁剪+补
- dm.h / dm_input.h：两边同族不同代，编译时逐个缺口对齐
- touch C 层：d2r 复用 dm/ios/Sources/dm_touch_*.c（D2R 专属技能 UI），Silksong 无此物 → 触控走基础 touch_ohos 或先禁触控用键鼠

## 需要新写/移植的
- patch_nso.py DM_TARGET=ohos（d2r 的 musl TSD 布局 tpidr_el0−272 移植到 nxnative 新版改写器）
- DM_GEN_DIR 生成：d2r 的 vault/HUD 头文件是 D2R 专属，Silksong 侧需要空实现或通用生成器
- shader：dmsh_vk.py（d2r 有）把 NVN shader 翻 SPIR-V；nxnative 的 dmsh_xlate_bin 是 MSL-only → 要么接 d2r 的 SPIR-V 翻译器，要么引擎吃 nvn_vk 的 SPIR-V 路径需对齐 nxnative 的 shader_map 格式

## 目标设备：HarmonyOS API 26 真机（另一台机器构建部署）

## 接口差距扫描结果（2026-10-24 晚，实扫非估）

### Apple API 缺口（.c 文件全部）：
| 文件 | 缺口 | 处置 |
|---|---|---|
| hle_audio.c | dispatch_semaphore, notify_ | 鸿蒙音频后端本身要重写(OHAudio)，连带解决 |
| hle_fs.c | clonefile(已有shim), notify_ | notify 部分用 inotify 或内部信号 |
| hle_os.c / hle_time.c / hle_nvn.c / nvn_dirty.c | mach_absolute_time | compat/mach/mach_time.h 已提供 |
| loader.c / hle_nvn.c | CC_SHA | CommonCrypto shim 已提供 |
| main.c | sysctl | sysconf / /proc 读取 |
| probe.c | mach_vm | 诊断工具，__OHOS__ 分支关掉 |

### 结构性差异：
- nxnative 前端契约只 10 个函数（比 d2r 简单）：front_run / front_metal_layer / front_screen_max_fps / front_screen_match / front_text_input / dm_host_preload_start / dm_touch_set / front_save_failed
- d2r 的 front_ohos/napi_ohos/touch_ohos 重度绑 D2R 专属 API（dm_tc_*/dm_ui_*/dm_bossrush/dm_derived/dm_custom）→ 重写为 nxnative 版，保留平台胶水（XComponent/刷新率/生命周期/N-API启动），剥掉 D2R 扩展
- nvn_vk.c + xbg engine：mtl_* 契约同构，可对接；头文件 include 路径要对齐（../../host/dm.h 等在 nxnative 树中结构相同）
- .m ObjC 文件（nvn_mtl/nvn_refl/front_mac/memstat/app_mac）在鸿蒙不编，由 nvn_vk.c + front_ohos.c 替代

### patch_nso.py：
nxnative 版无 DM_TARGET —— 需要把 d2r 的 musl TSD 布局（tpidr_el0−272）+ tsd 槽映射移植过来；同时保留我们的 unrolled-switch 修复

### DM_GEN_DIR：
d2r 的 vault/HUD/native_art 表是 D2R 专属 —— Silksong 用 stub/通用版；dm_touch_native_art.c 等按需替换

## 2026-10-25 进展：骨架落地，接缝闭合（harmony-port 分支）

### 已完成
- patch_nso.py DM_TARGET=ohos：musl TSD（tpidr_el0−272）emit_tp 移植，apple 产物逐字节一致，ohos 出 49,817 条 musl 序列、0 残留 tpidrro
- front_ohos.c：nxnative 10 函数契约重写（窗口桥/刷新率/QoS绑核/OpenGTX/FAST-Kit 保留，D2R 全剥）
- napi_ohos.c：重写（log泵/生命周期/env 默认值/execProbe/XComponent 保留；dm_custom/derived/overlay/touch 全剥），Silksong 4 模块清单
- CMakeLists.txt：去掉 DM_GEN_DIR/dm_touch_native_art/overlay_contract 门禁
- nvn_vk.c：mtl_* 面闭合（22 个存根——统计/时序/贴图登记存零，深度/原点/外贴图标记 TODO）
- compat：dispatch_semaphore→sem_t shim
- main.c：sysctl KERN_PROC_PID → /proc/self/stat (__OHOS__)
- probe.c：mach_vm_region/remap → __OHOS__ 分支（in-place unprotect 退化实现）
- dm_fault_hook_register / dm_guest_write_prep_register：nxnative 已有 ✓
- os_unfair：仅产物里有，源码无使用，非缺口

### 剩余（按优先级）
1. hle_audio.c：dispatch_semaphore 已 shim，但音频输出还是 CoreAudio→要 OHAudio 后端（或先静音跑通图形）
2. ArkTS app 壳：dm/harmony/app/ 是 D2R 的界面（IosOverlay/HUD/技能键），Silksong 要简化版壳（XComponent + start() + 控制器输入桥 dm_pad_set）
3. 输入：目前只有 NAPI touch stub——真机需要手柄/键盘→dm_pad_set 桥（物理手柄走 HID API 或 UI 按键映射）
4. shader SPIR-V：Silksong 的 MSL shader cache 要经 dmsh_vk.py 出 .xgs 包——nxnative dmsh_xlate_bin 是 MSL-only，先搬 d2r 的 SPIR-V 翻译器或适配
5. runtime pack：RomFS + out/ + shaders/ 的 staging 布局（napi_ohos.c 已按 <src>/RomFS|out|shaders 定约）
6. 真机首编：让另一台机器 cmake -B build -DCMAKE_TOOLCHAIN_FILE=ohos.cmake 试编 dmcore+dmplatform+libdm，收报错清单
