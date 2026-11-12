/* F2 glue: the few places where the loader needs C++ names of the runtime. */
#include "il2cpp-config.h"
#include "il2cpp-class-internals.h"
#include "il2cpp-object-internals.h"
#include "vm/MetadataCache.h"
#include "gc/GarbageCollector.h"
#include "f2_loader.h"

extern "C" void f2_glue_register_codegen(void *codeReg, void *metaReg, void *options)
{
    il2cpp::vm::MetadataCache::Register((const Il2CppCodeRegistration *)codeReg,
        (const Il2CppMetadataRegistration *)metaReg, (const Il2CppCodeGenOptions *)options);
}

extern "C" void f2_glue_gc_add_root(void *start, size_t size)
{
    il2cpp::gc::GarbageCollector::RegisterRoot((char *)start, size);
}

/* typeinfo object of Il2CppExceptionWrapper as our runtime throws/catches it (linkonce_odr, one per DSO) */
extern const char f2_ti_wrapper[] __asm__("_ZTI22Il2CppExceptionWrapper");
extern "C" const void *f2_glue_exception_typeinfo(void)
{
    return f2_ti_wrapper;
}

/* keeps the typeinfo alive in this translation unit as well */
extern "C" int f2_glue_selftest_throw(void)
{
    try { throw Il2CppExceptionWrapper(NULL); }
    catch (Il2CppExceptionWrapper &e) { return e.ex == NULL; }
    return 0;
}

/* ---- test support (used by harness/host_main.c only) ---- */
#include "vm/Runtime.h"
#include "vm/Class.h"
#include "vm/Exception.h"
#include "vm/Image.h"
extern "C" int f2_glue_image_class_count(const Il2CppImage *image) { return (int)image->typeCount; }
extern "C" Il2CppClass *f2_glue_image_class(const Il2CppImage *image, int index)
{
    return il2cpp::vm::MetadataCache::GetTypeInfoFromTypeDefinitionIndex(image->typeStart + index);
}
/* 0 = no cctor, 1 = cctor ran, 2 = managed exception (its class name copied to msg) */
extern "C" int f2_glue_try_class_init(Il2CppClass *klass, char *msg, int n)
{
    try {
        il2cpp::vm::Class::Init(klass);
        if (!klass->has_cctor) return 0;
        il2cpp::vm::Runtime::ClassInit(klass);
        return 1;
    } catch (Il2CppExceptionWrapper &e) {
        Il2CppException *ex = e.ex;
        while (ex && ex->inner_ex) ex = ex->inner_ex;
        Il2CppClass *k = ex ? ((Il2CppObject *)ex)->klass : NULL;
        snprintf(msg, n, "%s", k ? k->name : "?");
        return 2;
    }
}
