/**
 * fault_decode.c — see fault_decode.h.
 */
#include "fault_decode.h"

#include <string.h>

#include "minhook/hde/hde32.h"

/* The longest `FF /2` call: seg prefix, FF, ModRM, SIB, disp32. */
#define MAX_INDIRECT_CALL 7

const char *PmcRegName(int reg) {
    static const char *const names[8] = { "EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI" };
    return (reg >= 0 && reg < 8) ? names[reg] : "?";
}

int PmcDecodeOperand(const uint8_t *code, const uint32_t regs[8], PmcFaultOperand *out) {
    hde32s hs;
    unsigned len = hde32_disasm(code, &hs);
    uint32_t ea;

    memset(out, 0, sizeof(*out));
    out->base = PMC_REG_NONE;
    out->index = PMC_REG_NONE;
    if (hs.flags & F_ERROR) return 0;

    out->length = len;
    if (!(hs.flags & F_MODRM) || hs.modrm_mod == 3 || hs.p_67) return 1;

    out->memory = 1;
    out->segment = hs.p_seg == PREFIX_SEGMENT_FS || hs.p_seg == PREFIX_SEGMENT_GS;
    if (hs.flags & F_SIB) {
        if (hs.sib_index != 4) {
            out->index = hs.sib_index;
            out->scale = 1u << hs.sib_scale;
        }
        if (!(hs.modrm_mod == 0 && hs.sib_base == 5)) out->base = hs.sib_base;
    } else if (!(hs.modrm_mod == 0 && hs.modrm_rm == 5)) {
        out->base = hs.modrm_rm;
    }

    if (hs.flags & F_DISP8) out->disp = (int8_t)hs.disp.disp8;
    else if (hs.flags & F_DISP32) out->disp = (int32_t)hs.disp.disp32;

    ea = (uint32_t)out->disp;
    if (out->base != PMC_REG_NONE) ea += regs[out->base];
    if (out->index != PMC_REG_NONE) ea += regs[out->index] * out->scale;
    out->effective = ea;
    return 1;
}

int PmcFollowsCall(const uint8_t *ret, size_t before) {
    size_t n;

    if (before >= 5 && ret[-5] == 0xE8) return 1;
    for (n = 2; n <= MAX_INDIRECT_CALL && n <= before; n++) {
        hde32s hs;
        if (ret[-(ptrdiff_t)n] != 0xFF) continue;
        if (hde32_disasm(ret - n, &hs) == n && !(hs.flags & F_ERROR) &&
            (hs.modrm_reg == 2 || hs.modrm_reg == 3))
            return 1;
    }
    return 0;
}
