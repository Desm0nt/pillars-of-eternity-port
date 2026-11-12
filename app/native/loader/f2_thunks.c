/* F2 thunks: veneers put an index into w17 and jump here. */
#include "f2_loader.h"
__attribute__((naked)) void f2_nn_thunk(void) { __asm__("mov x0, x17\n b f2_nn_call"); }
__attribute__((naked)) void f2_unbound_thunk(void) { __asm__("mov x0, x17\n b f2_unbound_import"); }
