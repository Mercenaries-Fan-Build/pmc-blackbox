/**
 * fault_decode.h — what a faulting instruction addressed, and whether a stack value follows a call.
 *
 * Both answers come from the bytes of the code itself, read with the hde32 decoder MinHook already
 * carries, so a crash record states the register an instruction used rather than the register whose
 * value happens to sit nearest the fault. Portable C, tested on the host by test_fault_decode.c.
 */
#ifndef PMC_FAULT_DECODE_H
#define PMC_FAULT_DECODE_H

#include <stddef.h>
#include <stdint.h>

/* Register numbers are the x86 encoding: EAX ECX EDX EBX ESP EBP ESI EDI. */
#define PMC_REG_NONE (-1)

typedef struct {
    unsigned length;     /* instruction length in bytes */
    int      memory;     /* 1 when the instruction has a ModRM memory operand */
    int      segment;    /* 1 when that operand has an FS or GS override, so `effective` is an offset */
    int      base;       /* register number or PMC_REG_NONE */
    int      index;      /* register number or PMC_REG_NONE */
    unsigned scale;      /* multiplier of index, 0 when there is none */
    int32_t  disp;
    uint32_t effective;  /* base + index * scale + disp */
} PmcFaultOperand;

/**
 * Decode the instruction at `code`. `regs` holds the eight registers by number; 16-bit addressing has
 * no memory operand reported. Returns 1 when the decoder accepted the bytes, 0 when it did not. The
 * caller guarantees 16 readable bytes at `code`.
 */
int PmcDecodeOperand(const uint8_t *code, const uint32_t regs[8], PmcFaultOperand *out);

/** The register's name, or "?" for anything that is not a register number. */
const char *PmcRegName(int reg);

/**
 * Whether the bytes before `ret` end in a call: `E8 rel32`, or `FF /2` or `FF /3` of any encoding. A
 * yes means the value can be a return address, not that it is a live frame: stale stack slots hold
 * return addresses too. `before` is how many bytes before `ret` are readable, and 16 bytes from
 * `ret` - 7 must be readable when `before` is at least 7.
 */
int PmcFollowsCall(const uint8_t *ret, size_t before);

#endif /* PMC_FAULT_DECODE_H */
