/* generated: imports of the game image bound by name (sorted for bsearch) */
#include <stdlib.h>
#include "f2_loader.h"
extern char f2i__ZdlPv[] __asm__("_ZdlPv");
extern char f2i__Znwm[] __asm__("_Znwm");
extern char f2i__ZdaPv[] __asm__("_ZdaPv");
extern char f2i__Znam[] __asm__("_Znam");
extern char f2i__Unwind_Resume[] __asm__("_Unwind_Resume");
extern char f2i___cxa_begin_catch[] __asm__("__cxa_begin_catch");
extern char f2i___cxa_end_catch[] __asm__("__cxa_end_catch");
extern char f2i___cxa_allocate_exception[] __asm__("__cxa_allocate_exception");
extern char f2i___cxa_throw[] __asm__("__cxa_throw");
extern char f2i___cxa_pure_virtual[] __asm__("__cxa_pure_virtual");
extern char f2i___gxx_personality_v0[] __asm__("__gxx_personality_v0");
extern char f2i__ZSt9terminatev[] __asm__("_ZSt9terminatev");
extern char f2i_memcpy[] __asm__("memcpy");
extern char f2i_memmove[] __asm__("memmove");
extern char f2i_memset[] __asm__("memset");
extern char f2i_memcmp[] __asm__("memcmp");
extern char f2i_strlen[] __asm__("strlen");
extern char f2i_strcmp[] __asm__("strcmp");
extern char f2i_strncmp[] __asm__("strncmp");
extern char f2i_bsearch[] __asm__("bsearch");
extern char f2i_qsort[] __asm__("qsort");
extern char f2i_abort[] __asm__("abort");
extern char f2i_modf[] __asm__("modf");
extern char f2i_log[] __asm__("log");
extern char f2i_sin[] __asm__("sin");
extern char f2i_cos[] __asm__("cos");
extern char f2i_acos[] __asm__("acos");
extern char f2i_asin[] __asm__("asin");
extern char f2i_atan[] __asm__("atan");
extern char f2i_atan2[] __asm__("atan2");
extern char f2i_exp[] __asm__("exp");
extern char f2i_log10[] __asm__("log10");
extern char f2i_pow[] __asm__("pow");
extern char f2i_sqrt[] __asm__("sqrt");
extern char f2i_exp2f[] __asm__("exp2f");
extern char f2i_sqrtf[] __asm__("sqrtf");
extern char f2i_cosf[] __asm__("cosf");
extern char f2i_sinf[] __asm__("sinf");
extern char f2i_tanf[] __asm__("tanf");
extern char f2i_fmodf[] __asm__("fmodf");
extern char f2i_powf[] __asm__("powf");
extern char f2i_acosf[] __asm__("acosf");
extern char f2i_asinf[] __asm__("asinf");
extern char f2i_atanf[] __asm__("atanf");
extern char f2i_atan2f[] __asm__("atan2f");
extern char f2i_expf[] __asm__("expf");
extern char f2i_fmod[] __asm__("fmod");
extern char f2i_logf[] __asm__("logf");
extern char f2i_floor[] __asm__("floor");
extern char f2i_ceil[] __asm__("ceil");
extern char f2i_floorf[] __asm__("floorf");
extern char f2i_ceilf[] __asm__("ceilf");
extern char f2i_tan[] __asm__("tan");
extern char f2i_round[] __asm__("round");
extern char f2i_roundf[] __asm__("roundf");
extern char f2i_log2[] __asm__("log2");
extern char f2i_log2f[] __asm__("log2f");
extern char f2i_log10f[] __asm__("log10f");
extern char f2i_fabs[] __asm__("fabs");
extern char f2i_fabsf[] __asm__("fabsf");
extern char f2i__ZTVN10__cxxabiv117__class_type_infoE[] __asm__("_ZTVN10__cxxabiv117__class_type_infoE");
extern char f2i__ZTVN10__cxxabiv120__si_class_type_infoE[] __asm__("_ZTVN10__cxxabiv120__si_class_type_infoE");
extern char f2i__ZTVN10__cxxabiv121__vmi_class_type_infoE[] __asm__("_ZTVN10__cxxabiv121__vmi_class_type_infoE");
extern void *f2_assert_fail_ptr(void);
static void f2_assert_fail_shim(const char *e, const char *fl, int l, const char *fn) { ((void (*)(const char *, const char *, int, const char *))f2_assert_fail_ptr())(e, fl, l, fn); }
static void f2_throw_length_error(void) { f2_log("F2: std::length_error from game code"); abort(); }
const f2_import_t f2_imports[] = {
    {"_Unwind_Resume", (void *)f2i__Unwind_Resume},
    {"_ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv", (void *)f2_throw_length_error},
    {"_ZSt9terminatev", (void *)f2i__ZSt9terminatev},
    {"_ZTVN10__cxxabiv117__class_type_infoE", (void *)f2i__ZTVN10__cxxabiv117__class_type_infoE},
    {"_ZTVN10__cxxabiv120__si_class_type_infoE", (void *)f2i__ZTVN10__cxxabiv120__si_class_type_infoE},
    {"_ZTVN10__cxxabiv121__vmi_class_type_infoE", (void *)f2i__ZTVN10__cxxabiv121__vmi_class_type_infoE},
    {"_ZdaPv", (void *)f2i__ZdaPv},
    {"_ZdlPv", (void *)f2i__ZdlPv},
    {"_Znam", (void *)f2i__Znam},
    {"_Znwm", (void *)f2i__Znwm},
    {"__assert_fail", (void *)f2_assert_fail_shim},
    {"__cxa_allocate_exception", (void *)f2i___cxa_allocate_exception},
    {"__cxa_begin_catch", (void *)f2i___cxa_begin_catch},
    {"__cxa_end_catch", (void *)f2i___cxa_end_catch},
    {"__cxa_pure_virtual", (void *)f2i___cxa_pure_virtual},
    {"__cxa_throw", (void *)f2i___cxa_throw},
    {"__gxx_personality_v0", (void *)f2i___gxx_personality_v0},
    {"abort", (void *)f2i_abort},
    {"acos", (void *)f2i_acos},
    {"acosf", (void *)f2i_acosf},
    {"asin", (void *)f2i_asin},
    {"asinf", (void *)f2i_asinf},
    {"atan", (void *)f2i_atan},
    {"atan2", (void *)f2i_atan2},
    {"atan2f", (void *)f2i_atan2f},
    {"atanf", (void *)f2i_atanf},
    {"bsearch", (void *)f2i_bsearch},
    {"ceil", (void *)f2i_ceil},
    {"ceilf", (void *)f2i_ceilf},
    {"cos", (void *)f2i_cos},
    {"cosf", (void *)f2i_cosf},
    {"exp", (void *)f2i_exp},
    {"exp2f", (void *)f2i_exp2f},
    {"expf", (void *)f2i_expf},
    {"fabs", (void *)f2i_fabs},
    {"fabsf", (void *)f2i_fabsf},
    {"floor", (void *)f2i_floor},
    {"floorf", (void *)f2i_floorf},
    {"fmod", (void *)f2i_fmod},
    {"fmodf", (void *)f2i_fmodf},
    {"log", (void *)f2i_log},
    {"log10", (void *)f2i_log10},
    {"log10f", (void *)f2i_log10f},
    {"log2", (void *)f2i_log2},
    {"log2f", (void *)f2i_log2f},
    {"logf", (void *)f2i_logf},
    {"memcmp", (void *)f2i_memcmp},
    {"memcpy", (void *)f2i_memcpy},
    {"memmove", (void *)f2i_memmove},
    {"memset", (void *)f2i_memset},
    {"modf", (void *)f2i_modf},
    {"pow", (void *)f2i_pow},
    {"powf", (void *)f2i_powf},
    {"qsort", (void *)f2i_qsort},
    {"round", (void *)f2i_round},
    {"roundf", (void *)f2i_roundf},
    {"sin", (void *)f2i_sin},
    {"sinf", (void *)f2i_sinf},
    {"sqrt", (void *)f2i_sqrt},
    {"sqrtf", (void *)f2i_sqrtf},
    {"strcmp", (void *)f2i_strcmp},
    {"strlen", (void *)f2i_strlen},
    {"strncmp", (void *)f2i_strncmp},
    {"tan", (void *)f2i_tan},
    {"tanf", (void *)f2i_tanf},
};
const unsigned f2_import_count = 65;
