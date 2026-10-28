// 信号现场（ucontext_t）里的寄存器：苹果与 Linux（安卓）布局不同，宿主一律经这几个宏读写。
// UC_ESR：出错时的 ESR_EL1（缺页的 WnR 位 = 写访问）；Linux 上它在 __reserved 的扩展记录链里（ESR_MAGIC），没有时为 0。
// Registers in the signal context (ucontext_t): the layouts differ between Apple and Linux (Android), so the host always reads and writes them through these macros.
// UC_ESR: ESR_EL1 at the time of the fault (the WnR bit of a page fault = write access); on Linux it is in the extension record chain in __reserved (ESR_MAGIC), 0 when absent.
#pragma once
#include <stdint.h>
#include <sys/ucontext.h>
#ifdef __APPLE__
#define UC_PC(uc) ((uc)->uc_mcontext->__ss.__pc)
#define UC_LR(uc) ((uc)->uc_mcontext->__ss.__lr)
#define UC_SP(uc) ((uc)->uc_mcontext->__ss.__sp)
#define UC_FP(uc) ((uc)->uc_mcontext->__ss.__fp)
#define UC_X(uc, i) ((uc)->uc_mcontext->__ss.__x[i])
static inline uint64_t UC_ESR(const ucontext_t *uc) { return uc->uc_mcontext->__es.__esr; }
#else
#define UC_PC(uc) ((uc)->uc_mcontext.pc)
#define UC_LR(uc) ((uc)->uc_mcontext.regs[30])
#define UC_SP(uc) ((uc)->uc_mcontext.sp)
#define UC_FP(uc) ((uc)->uc_mcontext.regs[29])
#define UC_X(uc, i) ((uc)->uc_mcontext.regs[i])
static inline uint64_t UC_ESR(const ucontext_t *uc) {
    const uint8_t *p = (const uint8_t *)uc->uc_mcontext.__reserved, *end = p + sizeof uc->uc_mcontext.__reserved;
    while (p + 8 <= end) {
        uint32_t magic = *(const uint32_t *)p, size = *(const uint32_t *)(p + 4);
        if (!magic || size < 8) break;
        if (magic == 0x45535201u && size >= 16) return *(const uint64_t *)(p + 8);   // ESR_MAGIC
        p += size;
    }
    return 0;
}
#endif
#ifdef __APPLE__
#define UC_PSTATE(uc) ((uc)->uc_mcontext->__ss.__cpsr)
#define UC_FPSIMD_V(uc) ((uc)->uc_mcontext->__ns.__v)
#define UC_FPSR(uc) ((uc)->uc_mcontext->__ns.__fpsr)
#define UC_FPCR(uc) ((uc)->uc_mcontext->__ns.__fpcr)
#else
// Vector state (v regs / fpsr / fpcr): musl keeps it in the __reserved extension chain (FPSIMD record, magic 0x46508001)
typedef struct { uint32_t fpsr, fpcr, pad[2]; __uint128_t v[32]; } dm_vregs_t;
static inline const dm_vregs_t *UC_FPSIMD(const ucontext_t *uc) {
    const uint8_t *p = (const uint8_t *)uc->uc_mcontext.__reserved, *end = p + sizeof uc->uc_mcontext.__reserved;
    while (p + 8 <= end) {
        uint32_t magic = *(const uint32_t *)p, size = *(const uint32_t *)(p + 4);
        if (!magic || size < 8) break;
        if (magic == 0x46508001u && size >= 16) return (const dm_vregs_t *)(p + 8);
        p += size;
    }
    return NULL;
}
#define UC_VREGS(uc) (UC_FPSIMD(uc) ? UC_FPSIMD(uc)->v : NULL)
#define UC_FPSR(uc) (UC_FPSIMD(uc) ? UC_FPSIMD(uc)->fpsr : 0)
#define UC_FPCR(uc) (UC_FPSIMD(uc) ? UC_FPSIMD(uc)->fpcr : 0)
#define UC_PSTATE(uc) ((uc)->uc_mcontext.pstate)
#endif
