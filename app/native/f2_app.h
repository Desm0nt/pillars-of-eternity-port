/* F2 app: interface shared by the loader (native/loader), the boot / hook / diagnostics layer (native/boot)
 * and the NintendoSDK plugin implementations (native/nn).  Everything here lives in libil2cpp.so. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- logging (native/boot/f2_log.c): logcat tag "F2" + <files dir>/f2_log.txt, line buffered, thread safe ---- */
void f2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* numbered milestone line "F2: [M<id>] ..." -- the HANDOVER lists them in order */
void f2_milestone(int id, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* log once per call site */
#define F2_ONCE(...) do { static int f2_once_; if (!__atomic_exchange_n(&f2_once_, 1, __ATOMIC_RELAXED)) f2_log(__VA_ARGS__); } while (0)

/* ---- paths (native/boot/f2_paths.c) ---- */
const char *f2_files_dir(void);    /* app-specific external files dir, e.g. /storage/emulated/0/Android/data/<package>/files */
const char *f2_save_dir(void);     /* <files dir>/save   (nn.fs save data mount) */
const char *f2_data_dir(void);     /* shared, read-only game Data folder (the 15 GB tree) */
const char *f2_overlay_dir(void);  /* overlay Data folder (files that replace / add to f2_data_dir) */
const char *f2_game_image_path(void); /* explicit game_image= path (clean APK: setup_f2.py's <data root>/game/libf2game.so), or NULL */

/* ---- NintendoSDK plugin wrappers (native/nn) ----
 * off = address of the plugin wrapper inside the Switch main (column 1 of work/f2/data/nn_pinvoke.tsv).
 * The loader branches that entry straight to fn (arguments stay in the AAPCS64 registers the generated
 * P/Invoke code put them in).  Entries without an implementation keep the constant-return stub. */
typedef struct { uint32_t off; void *fn; const char *name; } f2_nn_impl_t;
extern const f2_nn_impl_t f2_nn_impls[] __attribute__((weak));
extern const unsigned f2_nn_impl_count __attribute__((weak));
void f2_nn_init(void) __attribute__((weak));   /* once, from Runtime::Init after the GC is ready */

/* ---- game image ---- */
uintptr_t f2_game_base(void);
/* nearest managed method / nn wrapper for an address inside the game image (0 if unknown) */
int f2_describe_game_addr(uintptr_t addr, char *out, size_t n);

#ifdef __cplusplus
}
#endif
