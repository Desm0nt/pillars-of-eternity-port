/* F2 loader -- see f2_loader.h.
 *
 * Image layout in memory (offsets == addresses of the Switch main, build 68976c70...):
 *   [0, F2_BSS_START)            text + rodata + data, read from main.f2img
 *   [F2_BSS_START, F2_BSS_END)   bss (zero)
 *   [F2_KEY_PAGE_OFF, +0x4000)   key page of the nx2android x18 stubs (slot offsets)
 *   [F2_PATCH_OFF, +PATCH_SIZE)  x18 stubs (from main.f2img)
 *   [veneers, +F2_VENEER_SIZE)   our veneers: runtime redirects, nn stubs, unbound imports
 *
 * Steps: map -> RELA relocations (RELATIVE / ABS64 / GLOB_DAT / JUMP_SLOT; imports bound by name) ->
 * redirect every runtime entry the game code calls to our libil2cpp -> nn plugin wrappers to stubs ->
 * poison pointer-reachable Switch runtime entries -> alias typeinfo of Il2CppExceptionWrapper ->
 * protect -> register .eh_frame_hdr with the unwinder.  Registration of the game's
 * Il2CppCodeRegistration / MetadataRegistration and of the GC roots happens from Runtime::Init. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <elf.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include "f2_loader.h"
#include "../f2_app.h"
#include "f2_gen_gameinfo.h"
#include "f2_gen_image.h"

void f2_register_switch_icalls(void);
void *f2_diag_substitute(void *target) __attribute__((weak));

#define F2_VENEER_SIZE 0x20000UL
#define PAGE_UP(x) (((x) + 0xffffUL) & ~0xffffUL)

static uint8_t *s_base;
static size_t s_total;
static uint8_t *s_veneer, *s_veneer_end;
static char s_path[512];
static const char **s_import_names;
static unsigned s_import_name_count;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

uintptr_t f2_game_base(void) { return (uintptr_t)s_base; }

void f2_set_image_path(const char *path)
{
    strncpy(s_path, path, sizeof(s_path) - 1);
}

static uint32_t enc_b(uintptr_t from, uintptr_t to)
{
    intptr_t d = (intptr_t)to - (intptr_t)from;
    if (d < -(1L << 27) || d >= (1L << 27)) { f2_log("F2: branch out of range %p -> %p", (void *)from, (void *)to); abort(); }
    return 0x14000000u | (uint32_t)((d >> 2) & 0x3ffffff);
}

/* ldr x16, #8 ; br x16 ; .quad target */
static uint8_t *veneer_jump(void *target)
{
    uint32_t *v = (uint32_t *)s_veneer;
    if (s_veneer + 16 > s_veneer_end) { f2_log("F2: veneer area full"); abort(); }
    v[0] = 0x58000050u; v[1] = 0xd61f0200u;
    memcpy(v + 2, &target, 8);
    s_veneer += 16;
    return (uint8_t *)v;
}

/* movz w17, #index ; ldr x16, #8 ; br x16 ; .quad target   (index < 65536) */
static uint8_t *veneer_indexed(unsigned index, void *target)
{
    uint32_t *v = (uint32_t *)s_veneer;
    if (s_veneer + 24 > s_veneer_end) { f2_log("F2: veneer area full"); abort(); }
    v[0] = 0x52800011u | ((index & 0xffff) << 5); v[1] = 0x58000050u; v[2] = 0xd61f0200u;
    memcpy(v + 3, &target, 8);
    s_veneer += 24;
    return (uint8_t *)v;
}

static void redirect(uint32_t off, uint8_t *veneer)
{
    uint32_t w = enc_b((uintptr_t)(s_base + off), (uintptr_t)veneer);
    memcpy(s_base + off, &w, 4);
}

/* ---- gates: C hooks at chosen instructions of the game code (boot/f2_touch.c ...).  The instruction at main+off is replaced by a
 * branch to a stub in the veneer area: save x0..x30, call hook(regs) (it may change them), restore, run the displaced instruction,
 * branch back to main+off+4.  The displaced instruction must not depend on its own address (the caller states the word it expects;
 * a mismatch leaves the code alone).  x16 is the only register the stub itself needs and it is restored as well; FP / SIMD registers
 * are NOT saved (use gates at function entries without float arguments or right after a call).  Installed while the image is still
 * writable and no game code runs. */
typedef void (*f2_gate_hook)(uint64_t *regs);
void f2_gate_table(void (*add)(uint32_t off, uint32_t expect, f2_gate_hook hook, const char *what)) __attribute__((weak));
static unsigned s_gates;
static void gate_add(uint32_t off, uint32_t expect, f2_gate_hook hook, const char *what)
{
    uint32_t cur;
    memcpy(&cur, s_base + off, 4);
    if (cur != expect) { f2_log("F2: [RED] gate %s at main+0x%x: word is %08x, expected %08x: NOT installed", what, off, cur, expect); return; }
    if (s_veneer + 4 * 44 + 8 > s_veneer_end) { f2_log("F2: [RED] gate %s: veneer area full", what); return; }
    uint32_t *v = (uint32_t *)s_veneer, *w = v;
    *w++ = 0xd10483ffu;                                                              /* sub sp, sp, #0x120 */
    for (unsigned r = 0; r < 30; r += 2) *w++ = 0xa9000000u | ((r / 2 * 2) << 15) | ((r + 1) << 10) | (31u << 5) | r;   /* stp xr, xr+1, [sp, #8r] */
    *w++ = 0xf9007bfeu;                                                              /* str x30, [sp, #240] */
    *w++ = 0x910003e0u;                                                              /* mov x0, sp */
    uint32_t *ldr = w++;                                                             /* ldr x16, hook literal */
    *w++ = 0xd63f0200u;                                                              /* blr x16 */
    for (unsigned r = 0; r < 30; r += 2) *w++ = 0xa9400000u | ((r / 2 * 2) << 15) | ((r + 1) << 10) | (31u << 5) | r;   /* ldp */
    *w++ = 0xf9407bfeu;                                                              /* ldr x30, [sp, #240] */
    *w++ = 0x910483ffu;                                                              /* add sp, sp, #0x120 */
    *w++ = cur;                                                                      /* the displaced instruction */
    *w = enc_b((uintptr_t)w, (uintptr_t)(s_base + off + 4)); w++;                    /* b back */
    if ((uintptr_t)w & 7) *w++ = 0xd503201fu;                                        /* nop: align the literal */
    *ldr = 0x58000010u | ((uint32_t)(w - ldr) << 5);
    memcpy(w, &hook, 8); w += 2;
    s_veneer = (uint8_t *)w;
    redirect(off, (uint8_t *)v);
    s_gates++;
    f2_log("F2: gate %s at main+0x%x", what, off);
}

static void *find_import(const char *name)
{
    unsigned lo = 0, hi = f2_import_count;       /* table is sorted by name */
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(name, f2_imports[mid].name);
        if (!c) return f2_imports[mid].ptr;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

void f2_unbound_import(uint64_t index)
{
    f2_log("F2: FATAL: game code called unbound import #%u '%s'", (unsigned)index,
           index < s_import_name_count ? s_import_names[index] : "?");
    abort();
}

uint64_t f2_nn_call(uint64_t index)
{
    static uint8_t seen[1024];
    if (index < f2_nn_entry_count) {
        if (index < sizeof(seen) && !seen[index]) {
            seen[index] = 1;
            f2_log("F2: nn stub (constant) %s -> %llu, first call from main+0x%lx", f2_nn_entries[index].name,
                   (unsigned long long)f2_nn_entries[index].ret, (unsigned long)((uintptr_t)__builtin_return_address(0) - (uintptr_t)s_base));
        }
        return f2_nn_entries[index].ret;
    }
    return 0;
}

void f2_no_return(void)
{
    f2_log("F2: FATAL: il2cpp_codegen_no_return reached");
    abort();
}

static void assert_fail_shim(const char *expr, const char *file, int line, const char *func)
{
    f2_log("F2: game assert failed: %s (%s:%d %s)", expr, file, line, func);
    abort();
}
void *f2_assert_fail_ptr(void) { return (void *)assert_fail_shim; }

static int relocate(void)
{
    const Elf64_Dyn *dyn = (const Elf64_Dyn *)(s_base + F2_DYNAMIC);
    const Elf64_Rela *rela = NULL, *jmprel = NULL;
    size_t relasz = 0, pltrelsz = 0, strsz = 0;
    const Elf64_Sym *symtab = NULL;
    const char *strtab = NULL;
    const uint32_t *hash = NULL;
    for (; dyn->d_tag != DT_NULL; dyn++) {
        switch (dyn->d_tag) {
        case DT_RELA: rela = (const Elf64_Rela *)(s_base + dyn->d_un.d_ptr); break;
        case DT_RELASZ: relasz = dyn->d_un.d_val; break;
        case DT_JMPREL: jmprel = (const Elf64_Rela *)(s_base + dyn->d_un.d_ptr); break;
        case DT_PLTRELSZ: pltrelsz = dyn->d_un.d_val; break;
        case DT_SYMTAB: symtab = (const Elf64_Sym *)(s_base + dyn->d_un.d_ptr); break;
        case DT_STRTAB: strtab = (const char *)(s_base + dyn->d_un.d_ptr); break;
        case DT_STRSZ: strsz = dyn->d_un.d_val; break;
        case DT_HASH: hash = (const uint32_t *)(s_base + dyn->d_un.d_ptr); break;
        }
    }
    if (!rela || !symtab || !strtab || !hash) { f2_log("F2: bad dynamic section"); return -1; }
    (void)strsz;
    unsigned nsym = hash[1];
    void **symaddr = (void **)calloc(nsym, sizeof(void *));
    s_import_names = (const char **)calloc(nsym, sizeof(char *));
    s_import_name_count = nsym;
    unsigned bound = 0, unbound = 0;
    for (unsigned i = 1; i < nsym; i++) {
        const Elf64_Sym *s = &symtab[i];
        const char *name = strtab + s->st_name;
        s_import_names[i] = name;
        if (s->st_shndx != SHN_UNDEF) { symaddr[i] = s_base + s->st_value; continue; }
        void *p = find_import(name);
        if (p) { symaddr[i] = p; bound++; }
        else { symaddr[i] = NULL; unbound++; }
    }
    size_t n = relasz / sizeof(Elf64_Rela), nrel = 0, nabs = 0;
    for (size_t i = 0; i < n; i++) {
        const Elf64_Rela *r = &rela[i];
        uint32_t type = ELF64_R_TYPE(r->r_info), sym = ELF64_R_SYM(r->r_info);
        uint64_t *where = (uint64_t *)(s_base + r->r_offset);
        switch (type) {
        case R_AARCH64_RELATIVE: *where = (uint64_t)s_base + r->r_addend; nrel++; break;
        case R_AARCH64_ABS64:
        case R_AARCH64_GLOB_DAT:
            /* data imports nobody in the game code needs stay NULL (weak-like); functions get a trap veneer */
            *where = symaddr[sym] ? (uint64_t)symaddr[sym] + r->r_addend : 0; nabs++; break;
        default: f2_log("F2: unsupported relocation type %u", type); return -1;
        }
    }
    n = pltrelsz / sizeof(Elf64_Rela);
    for (size_t i = 0; i < n; i++) {
        const Elf64_Rela *r = &jmprel[i];
        uint32_t sym = ELF64_R_SYM(r->r_info);
        uint64_t *where = (uint64_t *)(s_base + r->r_offset);
        void *p = symaddr[sym];
        if (!p) p = veneer_indexed(sym, (void *)f2_unbound_thunk);
        *where = (uint64_t)p;
    }
    f2_log("F2: relocations: %zu relative, %zu symbolic, %zu jump slots; imports bound %u, unbound %u (trap)",
           nrel, nabs, n, bound, unbound);
    free(symaddr);
    return 0;
}

int f2_load_game(const char *image_path)
{
    pthread_mutex_lock(&s_lock);
    if (s_base) { pthread_mutex_unlock(&s_lock); return 0; }
    if (!image_path || !*image_path) image_path = s_path[0] ? s_path : getenv("F2_GAME_IMAGE");
    static char beside[600];
    if (!image_path) {
        /* default on Android: the image ships as lib/arm64-v8a/libf2game.so, i.e. next to libil2cpp.so */
        Dl_info di;
        if (dladdr((void *)f2_load_game, &di) && di.dli_fname) {
            const char *slash = strrchr(di.dli_fname, '/');
            if (slash && (size_t)(slash - di.dli_fname) < sizeof(beside) - 20) {
                memcpy(beside, di.dli_fname, slash - di.dli_fname + 1);
                strcpy(beside + (slash - di.dli_fname) + 1, "libf2game.so");
                if (access(beside, R_OK) == 0) image_path = beside;
            }
        }
    }
    /* clean APK (tools/build_apk.py without --dev-full): the image is not shipped in the APK at all. GameFolder.findDataRoot
     * (java/.../GameFolder.java) + DmActivity.writeFront wrote game_image=<data root>/game/libf2game.so into f2_front.txt when
     * that folder is complete; setup_f2.py's "layout" step put the image there. A file under Download is noexec, but f2_load_game
     * only pread()s it into an anonymous mapping below and mprotects THAT executable afterwards, so noexec storage is fine. */
    if (!image_path && f2_game_image_path()) {
        if (access(f2_game_image_path(), R_OK) == 0) image_path = f2_game_image_path();
        else f2_log("F2: [RED] game_image=%s (f2_front.txt) is not readable: %s", f2_game_image_path(), strerror(errno));
    }
    static char infiles[600];
    if (!image_path && f2_files_dir()) {
        /* fallback when the installer did not extract the (non-ELF) image next to the libraries: push it by hand to <files dir>/libf2game.so */
        snprintf(infiles, sizeof infiles, "%s/libf2game.so", f2_files_dir());
        if (access(infiles, R_OK) == 0) image_path = infiles;
    }
    if (!image_path) {
        /* nothing worked: say exactly where we looked, the same [RED] style f2_paths.c already uses for a missing data folder */
        f2_log("F2: [RED] game image not found anywhere: not beside the libraries (%s), not game_image= from f2_front.txt (%s), "
               "not %s -- copy the setup folder's game/libf2game.so to the phone, or rebuild with the dev-full APK option",
               beside[0] ? beside : "<lib dir>/libf2game.so", f2_game_image_path() ? f2_game_image_path() : "(not set)",
               infiles[0] ? infiles : "<files dir>/libf2game.so");
        image_path = "main.f2img";
    }
    int fd = open(image_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { f2_log("F2: cannot open game image %s: %s", image_path, strerror(errno)); pthread_mutex_unlock(&s_lock); return -1; }
    /* The patch area's length comes from the FILE: the image is made on the player's PC by the setup (tools/setup_f2.py) from the porter's
     * output, and its tail (stubs of the other route's gates, never called here) grows with the port's profile: 0xaf030 in the image the
     * first builds carried, 0xaf0f0 from the profile of 11.11.2026 19:49.  F2_PATCH_SIZE is only the fallback when the size cannot be read. */
    size_t patch_size = F2_PATCH_SIZE;
    struct stat ist;
    if (fstat(fd, &ist) == 0 && (size_t)ist.st_size > F2_FILE_IMAGE_SIZE && (size_t)ist.st_size - F2_FILE_IMAGE_SIZE <= (16u << 20))
        patch_size = (size_t)ist.st_size - F2_FILE_IMAGE_SIZE;
    else f2_log("F2: [RED] game image %s: unexpected size, using the built-in patch area size 0x%lx", image_path, (unsigned long)F2_PATCH_SIZE);
    if (patch_size != F2_PATCH_SIZE) f2_log("F2: game image patch area 0x%zx bytes (this build was made with 0x%lx): taken from the file", patch_size, (unsigned long)F2_PATCH_SIZE);
    size_t veneer_off = PAGE_UP(F2_PATCH_OFF + patch_size);
    s_total = veneer_off + F2_VENEER_SIZE;
    uint8_t *base = (uint8_t *)mmap(NULL, s_total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { f2_log("F2: mmap %zu failed", s_total); close(fd); pthread_mutex_unlock(&s_lock); return -1; }
    size_t got = 0;
    while (got < F2_FILE_IMAGE_SIZE) {
        ssize_t r = pread(fd, base + got, F2_FILE_IMAGE_SIZE - got, got);
        if (r <= 0) { f2_log("F2: short read of image"); close(fd); pthread_mutex_unlock(&s_lock); return -1; }
        got += r;
    }
    got = 0;
    while (got < patch_size) {
        ssize_t r = pread(fd, base + F2_PATCH_OFF + got, patch_size - got, F2_FILE_IMAGE_SIZE + got);
        if (r <= 0) { f2_log("F2: short read of patch area"); close(fd); pthread_mutex_unlock(&s_lock); return -1; }
        got += r;
    }
    close(fd);
    s_base = base;
    s_veneer = base + veneer_off; s_veneer_end = s_veneer + F2_VENEER_SIZE;
    if (memcmp(base + 8, "MOD0", 4) != 0) f2_log("F2: warning: MOD0 not found where expected");

    if (relocate() != 0) { pthread_mutex_unlock(&s_lock); return -1; }

    /* runtime entry points -> our libil2cpp */
    unsigned nsub = 0;
    for (unsigned i = 0; i < f2_redirect_count; i++) {
        void *t = f2_redirects[i].target;
        if (f2_diag_substitute) {            /* diagnostics wrappers for Exception::Raise, InternalCalls::Resolve ... (boot/f2_diag.cpp) */
            void *w = f2_diag_substitute(t);
            if (w != t) { t = w; nsub++; }
        }
        redirect(f2_redirects[i].off, veneer_jump(t));
    }
    /* NintendoSDK plugin wrappers (P/Invoke "__Internal") -> stubs */
    unsigned nimpl = 0;
    for (unsigned i = 0; i < f2_nn_entry_count; i++) {
        void *impl = NULL;
        if (&f2_nn_impl_count)
            for (unsigned k = 0; k < f2_nn_impl_count; k++)
                if (f2_nn_impls[k].off == f2_nn_entries[i].off) { impl = f2_nn_impls[k].fn; break; }
        if (impl) { redirect(f2_nn_entries[i].off, veneer_jump(impl)); nimpl++; }
        else redirect(f2_nn_entries[i].off, veneer_indexed(i, (void *)f2_nn_thunk));
    }
    f2_log("F2: nn plugin wrappers: %u implemented, %u constant stubs", nimpl, f2_nn_entry_count - nimpl);
    /* Switch runtime entries reachable only through data pointers: trap instead of running Horizon code */
    for (unsigned i = 0; i < f2_poison_count; i++) {
        uint32_t brk = 0xd4200000u | (0xf2u << 5);
        memcpy(base + f2_poison[i], &brk, 4);
    }
    /* one type_info identity for Il2CppExceptionWrapper: every slot of main that held the address of its own
     * typeinfo (GOT entry used by throw sites, LSDA type-table references) now points at ours */
    const void *ti = f2_glue_exception_typeinfo();
    for (unsigned i = 0; i < f2_typeinfo_slot_count; i++)
        memcpy(base + f2_typeinfo_slots[i], &ti, 8);
    /* ...and main's own typeinfo object gets our name pointer, so a direct (pc-relative) reference to it still
     * compares equal (libc++abi compares type names: by pointer or by strcmp) */
    memcpy(base + F2_TYPEINFO_MAIN + 8, (const char *)ti + 8, 8);
    /* x18 stubs: slot K18 lives at offset 0 of the per-thread block (see f2_thread_enter) */
    memset(base + F2_KEY_PAGE_OFF, 0, 64);

    if (f2_gate_table) f2_gate_table(gate_add);
    if (mprotect(base, F2_TEXT_END, PROT_READ | PROT_EXEC) != 0 ||
        mprotect(base + F2_PATCH_OFF, s_total - F2_PATCH_OFF, PROT_READ | PROT_EXEC) != 0)
        f2_log("F2: mprotect failed: %s", strerror(errno));
    __builtin___clear_cache((char *)base, (char *)base + F2_TEXT_END);
    __builtin___clear_cache((char *)base + F2_PATCH_OFF, (char *)base + s_total);

    f2_unwind_register((uintptr_t)base, F2_TEXT_END, (uintptr_t)base + F2_EH_FRAME_HDR, F2_EH_FRAME_HDR_SIZE);
    f2_thread_enter();
    f2_log("F2: [M09] game image %s mapped at %p (%zu MB), %u runtime redirects (%u with diagnostics wrappers), %u nn stubs, %u poisoned entries",
           image_path, base, s_total >> 20, f2_redirect_count, nsub, f2_nn_entry_count, f2_poison_count);
    pthread_mutex_unlock(&s_lock);
    return 0;
}

void f2_runtime_pre_init(void)
{
    if (f2_load_game(NULL) != 0) { f2_log("F2: FATAL: game image not loaded"); abort(); }
    f2_glue_register_codegen(s_base + F2_CODE_REGISTRATION, s_base + F2_METADATA_REGISTRATION, s_base + F2_CODEGEN_OPTIONS);
}

void f2_runtime_gc_ready(void)
{
    /* conservative roots: data + bss of the image (metadata usage variables, method-init flags, C++ statics) */
    f2_glue_gc_add_root(s_base + F2_GAME_DATA_ROOT_START, F2_GAME_DATA_ROOT_END - F2_GAME_DATA_ROOT_START);
    f2_register_switch_icalls();
}

/* nx2android's x18 stubs keep the "virtual x18" in a per-thread block reached through bionic's
 * TLS_SLOT_APP (tpidr_el0[2]); Android reserves the real x18 (shadow call stack). */
void f2_thread_enter(void)
{
    void **tls;
    __asm__("mrs %0, tpidr_el0" : "=r"(tls));
    if (tls && !tls[2])
        tls[2] = calloc(1, 64);
}
