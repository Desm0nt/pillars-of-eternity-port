/* F2: NintendoSDK plugin wrappers of the Switch main, implemented for Android.
 *
 * The game (GripDigital platform layer + InControl) reaches the SDK only through the "NintendoSDK plugin":
 * 192 tiny C wrappers at main+0x2496b00..0x2498438, each forwarding to one nn::* function.  The loader
 * branches a wrapper entry straight to the function listed in f2_nn_table.c, so every function here has
 * the WRAPPER's C ABI (what the generated P/Invoke code passes), not the nn::* C++ signature:
 *   - nn::Result                 -> uint32_t in w0 (0 = success; module = bits 0..8, description = bits 9..21)
 *   - Uid (2 x u64) by value     -> two consecutive x registers
 *   - UserHandle (3 x u64)       -> pointer (AAPCS64 composite > 16 bytes)
 *   - FileHandle/DirectoryHandle -> one x register (pointer sized)
 *   - `ref T`, arrays            -> pointer to the managed storage / first element (blittable types)
 *   - string                     -> char* UTF-8, marshalled by the caller
 * tools/nn/nn_abi.tsv has the row for every wrapper. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../f2_app.h"

typedef uint32_t nn_result_t;
#define NN_RESULT(module, desc) ((nn_result_t)((module) | ((uint32_t)(desc) << 9)))
#define NN_OK 0u
/* nn::fs (module 2) -- ranges checked by the game through nn.ErrorRange.Includes */
#define NN_FS_PATH_NOT_FOUND          NN_RESULT(2, 1)
#define NN_FS_PATH_ALREADY_EXISTS     NN_RESULT(2, 2)
#define NN_FS_TARGET_LOCKED           NN_RESULT(2, 7)
#define NN_FS_DIRECTORY_NOT_EMPTY     NN_RESULT(2, 8)
#define NN_FS_USABLE_SPACE_NOT_ENOUGH NN_RESULT(2, 30)
#define NN_FS_MOUNT_NAME_EXISTS       NN_RESULT(2, 60)
#define NN_FS_TARGET_NOT_FOUND        NN_RESULT(2, 1002)
#define NN_FS_OUT_OF_RANGE            NN_RESULT(2, 3005)
#define NN_FS_INVALID_ARGUMENT        NN_RESULT(2, 6001)
/* nn::account (module 124): network service account not available */
#define NN_ACCOUNT_NSA_UNAVAILABLE    NN_RESULT(124, 100)

typedef struct { uint64_t d0, d1; } nn_uid_t;
typedef struct { uint64_t d0, d1, context; } nn_user_handle_t;
extern const nn_uid_t f2_nn_uid;                 /* the one local user */

/* f2_nn_misc.c */
void f2_nn_config_load(void);                    /* reads <files dir>/f2_lang.txt, f2_platform.txt */
const char *f2_nn_language(void);                /* "ru" by default */
int f2_nn_report_switch_platform(void);          /* 1 = Application.platform says Switch (default) */

/* f2_nn_fs.c */
void f2_nn_fs_init(void);

/* exported helpers (version script exports f2_*): a later on-screen pad / Java side can feed the pad */
/* buttons = nn.hid.NpadButton bits (A 1, B 2, X 4, Y 8, StickL 0x10, StickR 0x20, L 0x40, R 0x80, ZL 0x100, ZR 0x200, Plus 0x400,
 * Minus 0x800, Left 0x1000, Up 0x2000, Right 0x4000, Down 0x8000); sticks -32767..32767, y up. Thread safe. */
__attribute__((visibility("default"))) void f2_nn_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
__attribute__((visibility("default"))) void f2_nn_set_language(const char *code);

/* optional, provided by the boot layer: folders the MANAGED code gets for Application.streamingAssetsPath /
 * Application.dataPath (must be real directories: the game reads them with System.IO; the Android player's own
 * answer is "jar:file://<apk>!/assets").  When the boot layer does not define them (or they return NULL),
 * f2_data_dir() + "/StreamingAssets" and f2_data_dir() are used; when f2_data_dir() is NULL too, no override. */
const char *f2_managed_streaming_assets_path(void) __attribute__((weak));
const char *f2_managed_data_path(void) __attribute__((weak));
