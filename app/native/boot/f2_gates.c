/* F2: the one place where the modules' hooks into the game code and into the engine's icall registration meet.
 *   gates (native/loader/f2_loader.c gate_add): the loader calls f2_gate_table() while the game image is still writable; each module adds its own
 *   engine icalls: f2_diag.cpp calls f2_engine_icall() for every "Type::Method" libunity registers, so a module can keep the engine's function
 *   pointer (to call it) and / or register a full-name override ("Type::Method(args)") that the generated code resolves first
 * A module that is not linked in is simply skipped (weak references). */
#include <stdint.h>
#include <stddef.h>

typedef void (*f2_gate_hook)(uint64_t *regs);
typedef void (*f2_gate_add)(uint32_t off, uint32_t expect, f2_gate_hook hook, const char *what);

void f2_touch_gates(f2_gate_add add) __attribute__((weak));
void f2_msaa_gates(f2_gate_add add) __attribute__((weak));
void f2_texdiag_gates(f2_gate_add add) __attribute__((weak));
void f2_gate_table(f2_gate_add add)
{
    if (f2_touch_gates) f2_touch_gates(add);
    if (f2_msaa_gates) f2_msaa_gates(add);
    if (f2_texdiag_gates) f2_texdiag_gates(add);
}

void f2_screen_engine_icall(const char *name, void (*fn)(void)) __attribute__((weak));
void f2_touch_engine_icall(const char *name, void (*fn)(void)) __attribute__((weak));
void f2_msaa_engine_icall(const char *name, void (*fn)(void)) __attribute__((weak));
void f2_texdiag_engine_icall(const char *name, void (*fn)(void)) __attribute__((weak));
void f2_engine_icall(const char *name, void (*fn)(void))
{
    if (f2_screen_engine_icall) f2_screen_engine_icall(name, fn);
    if (f2_touch_engine_icall) f2_touch_engine_icall(name, fn);
    if (f2_msaa_engine_icall) f2_msaa_engine_icall(name, fn);
    if (f2_texdiag_engine_icall) f2_texdiag_engine_icall(name, fn);
}
