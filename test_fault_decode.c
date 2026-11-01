/**
 * test_fault_decode.c — host test of PmcDecodeOperand and PmcFollowsCall.
 *
 * The first cases are the two faulting instructions of the recorded E_OUTOFMEMORY crashes: the old
 * nearest-register guess named ECX for the first of them, whose bytes say EBP.
 *
 * Exit 0 when every case passes, 1 otherwise.
 */
#include "fault_decode.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int cond, const char *what) {
    printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
}

/* `n` instruction bytes, padded so the decoder always has 16 readable bytes. */
static int decode(const uint8_t *bytes, size_t n, const uint32_t regs[8], PmcFaultOperand *op) {
    uint8_t code[16];
    memset(code, 0x90, sizeof(code));
    memcpy(code, bytes, n);
    return PmcDecodeOperand(code, regs, op);
}

static void operands(void) {
    /*                     EAX EC  ED  EB  ESP     EBP     ESI         EDI */
    uint32_t regs[8] = { 0, 0, 0, 0, 0x03C7F5B0, 0, 0x0100A0BB, 0x31545844 };
    PmcFaultOperand op;

    {
        static const uint8_t b[] = { 0x8B, 0x45, 0x00 };   /* mov eax,[ebp+0], v1.1 0x85C999 */
        check(decode(b, sizeof(b), regs, &op) == 1 && op.length == 3 && op.memory == 1, "mov eax,[ebp+0] decodes");
        check(op.base == 5 && op.index == PMC_REG_NONE && op.disp == 0 && op.effective == 0,
              "its operand is [EBP+0] at 0, not ECX");
    }
    {
        static const uint8_t b[] = { 0x8B, 0x06 };         /* mov eax,[esi], v1.0 0x85C8D0 */
        regs[6] = 0;
        check(decode(b, sizeof(b), regs, &op) == 1 && op.base == 6 && op.effective == 0, "mov eax,[esi] names ESI");
    }
    {
        static const uint8_t b[] = { 0x83, 0x78, 0x10, 0x06 };   /* cmp dword [eax+0x10],6 */
        regs[0] = 0;
        check(decode(b, sizeof(b), regs, &op) == 1 && op.base == 0 && op.disp == 0x10 && op.effective == 0x10,
              "a disp8 is added");
    }
    {
        static const uint8_t b[] = { 0x8B, 0x44, 0x24, 0xF8 };   /* mov eax,[esp-8] */
        regs[4] = 0x1000;
        check(decode(b, sizeof(b), regs, &op) == 1 && op.base == 4 && op.index == PMC_REG_NONE &&
              op.disp == -8 && op.effective == 0xFF8, "a SIB with no index, and a negative disp8");
    }
    {
        static const uint8_t b[] = { 0x8B, 0x04, 0x8B };         /* mov eax,[ebx+ecx*4] */
        regs[3] = 0x2000; regs[1] = 3;
        check(decode(b, sizeof(b), regs, &op) == 1 && op.base == 3 && op.index == 1 && op.scale == 4 &&
              op.effective == 0x200C, "base plus scaled index");
    }
    {
        static const uint8_t b[] = { 0x8B, 0x04, 0x8D, 0x00, 0x10, 0x00, 0x00 };   /* mov eax,[ecx*4+0x1000] */
        check(decode(b, sizeof(b), regs, &op) == 1 && op.base == PMC_REG_NONE && op.index == 1 &&
              op.effective == 0x100C, "an index with no base");
    }
    {
        static const uint8_t b[] = { 0xA1, 0x74, 0xBD, 0xDF, 0x00 };   /* mov eax,[0xDFBD74], no ModRM */
        check(decode(b, sizeof(b), regs, &op) == 1 && op.memory == 0 && op.length == 5,
              "an instruction without a ModRM operand reports none");
    }
    {
        static const uint8_t b[] = { 0x8B, 0x05, 0x74, 0xBD, 0xDF, 0x00 };   /* mov eax,[0xDFBD74], ModRM */
        check(decode(b, sizeof(b), regs, &op) == 1 && op.memory == 1 && op.base == PMC_REG_NONE &&
              op.effective == 0xDFBD74, "an absolute operand has no base");
    }
    {
        static const uint8_t b[] = { 0x8B, 0xC1 };         /* mov eax,ecx */
        check(decode(b, sizeof(b), regs, &op) == 1 && op.memory == 0, "a register move has no memory operand");
    }
    {
        static const uint8_t b[] = { 0x64, 0x8B, 0x0D, 0x18, 0x00, 0x00, 0x00 };   /* mov ecx,fs:[0x18] */
        check(decode(b, sizeof(b), regs, &op) == 1 && op.memory == 1 && op.segment == 1, "an FS operand is marked");
    }
    {
        static const uint8_t b[] = { 0x0F, 0xFF };         /* no such instruction */
        check(decode(b, sizeof(b), regs, &op) == 0, "bytes the decoder refuses return 0");
    }
    check(strcmp(PmcRegName(5), "EBP") == 0 && strcmp(PmcRegName(PMC_REG_NONE), "?") == 0, "register names");
}

/* `bytes` then a NOP sled, returning the address just after `bytes`. */
static const uint8_t *after(uint8_t *buf, size_t size, const uint8_t *bytes, size_t n) {
    memset(buf, 0x90, size);
    memcpy(buf + 8, bytes, n);
    return buf + 8 + n;
}

static void calls(void) {
    uint8_t buf[48];

    {
        static const uint8_t b[] = { 0xE8, 0x11, 0x22, 0x33, 0x00 };   /* call rel32 */
        check(PmcFollowsCall(after(buf, sizeof(buf), b, sizeof(b)), 13) == 1, "call rel32");
    }
    {
        static const uint8_t b[] = { 0xFF, 0xD2 };                     /* call edx */
        check(PmcFollowsCall(after(buf, sizeof(buf), b, sizeof(b)), 10) == 1, "call edx");
    }
    {
        static const uint8_t b[] = { 0xFF, 0x50, 0x5C };               /* call [eax+0x5c], CreateTexture */
        check(PmcFollowsCall(after(buf, sizeof(buf), b, sizeof(b)), 11) == 1, "call [eax+disp8]");
    }
    {
        static const uint8_t b[] = { 0xFF, 0x15, 0x00, 0x10, 0x40, 0x00 };   /* call [0x401000] */
        check(PmcFollowsCall(after(buf, sizeof(buf), b, sizeof(b)), 14) == 1, "call [abs]");
    }
    {
        static const uint8_t b[] = { 0xFF, 0x54, 0x24, 0x10 };         /* call [esp+0x10] */
        check(PmcFollowsCall(after(buf, sizeof(buf), b, sizeof(b)), 12) == 1, "call [esp+disp8]");
    }
    {
        static const uint8_t b[] = { 0xFF, 0xE2 };                     /* jmp edx */
        check(PmcFollowsCall(after(buf, sizeof(buf), b, sizeof(b)), 10) == 0, "jmp edx is not a call");
    }
    {
        static const uint8_t b[] = { 0xFF, 0x70, 0x10 };               /* push [eax+0x10] */
        check(PmcFollowsCall(after(buf, sizeof(buf), b, sizeof(b)), 11) == 0, "push [mem] is not a call");
    }
    {
        static const uint8_t b[] = { 0x8B, 0x45, 0x00 };               /* plain data move */
        check(PmcFollowsCall(after(buf, sizeof(buf), b, sizeof(b)), 11) == 0, "no call before the value");
    }
    {
        static const uint8_t b[] = { 0xE8, 0x11, 0x22, 0x33, 0x00 };
        check(PmcFollowsCall(after(buf, sizeof(buf), b, sizeof(b)), 4) == 0, "too few readable bytes before it");
    }
}

int main(void) {
    operands();
    calls();
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
