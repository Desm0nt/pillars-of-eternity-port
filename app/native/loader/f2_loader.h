/* F2 loader: maps the IL2CPP-generated game code of the Switch `main` into our own libil2cpp.so
 * (Unity 2018.1.4f1 runtime built for Android arm64) and wires it to that runtime. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../f2_app.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint32_t off; void *target; } f2_redirect_t;
typedef struct { uint32_t off; const char *name; uint64_t ret; } f2_nn_entry_t;
typedef struct { const char *name; void *ptr; } f2_import_t;

/* generated (tools/gen_tables.py) */
extern const f2_redirect_t f2_redirects[];
extern const unsigned f2_redirect_count;
extern const uint32_t f2_poison[];
extern const unsigned f2_poison_count;
extern const uint32_t f2_typeinfo_slots[];
extern const unsigned f2_typeinfo_slot_count;
extern const f2_nn_entry_t f2_nn_entries[];
extern const unsigned f2_nn_entry_count;
/* f2_imports.c */
extern const f2_import_t f2_imports[];
extern const unsigned f2_import_count;

/* public */
int f2_load_game(const char *image_path);      /* 0 = ok; idempotent */
void f2_set_image_path(const char *path);      /* optional: before il2cpp_init */
uintptr_t f2_game_base(void);
void f2_runtime_pre_init(void);                /* called from Runtime::Init (source patch) */
void f2_runtime_gc_ready(void);                /* called from Runtime::Init after GC init */
void f2_thread_enter(void);                    /* called from Thread::Initialize */

/* f2_unwind.c */
void f2_unwind_register(uintptr_t base, size_t text_size, uintptr_t eh_frame_hdr, size_t eh_frame_hdr_size);
/* f2_glue.cpp */
void f2_glue_register_codegen(void *codeReg, void *metaReg, void *options);
void f2_glue_gc_add_root(void *start, size_t size);
const void *f2_glue_exception_typeinfo(void);
/* f2_thunks.S */
void f2_nn_thunk(void);
void f2_unbound_thunk(void);
uint64_t f2_nn_call(uint64_t index);
void f2_unbound_import(uint64_t index);
void f2_no_return(void);

#ifdef __cplusplus
}
#endif
