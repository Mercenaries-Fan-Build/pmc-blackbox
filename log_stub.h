/**
 * log_stub.h — find the game's shared log stub in the running image, for any build.
 *
 * Every Lua `print`, `Debug.Printf` and ~700 stripped subsystem loggers point at one shared no-op
 * stub (`33 C0 C3` = xor eax,eax; ret). Its address differs per build — 0x006D5640 in v1.0 code,
 * 0x006AEF90 in v1.1 code — so it is found, not assumed: the base library's `luaL_Reg` table in
 * .rdata has a row `{ "print", <stub> }`. This is the same rule m2-sdk.dll uses
 * (mercs2_formats::lua_sites, `Site::Print`), so both hook the same address.
 *
 * Portable C with no Windows headers: the DLL passes its own mapped sections, the host test passes
 * sections read from an exe on disk.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* One section of a 32-bit image: its virtual address and its bytes as mapped. */
typedef struct {
    uint32_t va;
    const uint8_t *bytes;
    uint32_t size;
} LogStubSection;

/**
 * Find the shared log stub.
 *
 * Succeeds when exactly one 8-byte row in `rdata` is `{ pointer to a NUL-preceded "print\0" string in
 * rdata, pointer into text }`, and the function it names starts `33 C0 C3` (the stub) or `E9` (a
 * detour another module already installed on it). Then returns 1 and sets *stub_va to the function
 * and *row_va to the row.
 *
 * Otherwise returns 0 and writes the reason into why (always NUL-terminated when why_len > 0): no
 * such row, more than one, or a function whose bytes are not the stub. The caller must not hook
 * anything then.
 */
int FindLogStub(const LogStubSection *text, const LogStubSection *rdata,
                uint32_t *stub_va, uint32_t *row_va, char *why, size_t why_len);
