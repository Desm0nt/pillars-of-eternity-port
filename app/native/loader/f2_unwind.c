/* F2: make the unwinder (LLVM libunwind, linked statically into libil2cpp.so) see the Switch game image.
 * libunwind finds unwind tables with dl_iterate_phdr(); the image is mapped by our own loader, so the
 * dynamic linker does not know it.  libil2cpp.so is linked with -Wl,--wrap=dl_iterate_phdr and this
 * wrapper reports one extra "module" (PT_LOAD for the code + PT_GNU_EH_FRAME for main's .eh_frame_hdr)
 * before the real ones. */
#include <link.h>
#include <string.h>
#include <stdint.h>
#include "f2_loader.h"

int __real_dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data);

static ElfW(Phdr) s_phdr[2];
static struct dl_phdr_info s_info;
static int s_have;

void f2_unwind_register(uintptr_t base, size_t text_size, uintptr_t eh_frame_hdr, size_t eh_frame_hdr_size)
{
    memset(s_phdr, 0, sizeof(s_phdr));
    s_phdr[0].p_type = PT_LOAD;
    s_phdr[0].p_flags = PF_R | PF_X;
    s_phdr[0].p_vaddr = 0;
    s_phdr[0].p_memsz = s_phdr[0].p_filesz = text_size;
    s_phdr[1].p_type = PT_GNU_EH_FRAME;
    s_phdr[1].p_flags = PF_R;
    s_phdr[1].p_vaddr = eh_frame_hdr - base;
    s_phdr[1].p_memsz = s_phdr[1].p_filesz = eh_frame_hdr_size;
    memset(&s_info, 0, sizeof(s_info));
    s_info.dlpi_addr = base;
    s_info.dlpi_name = "main.f2";
    s_info.dlpi_phdr = s_phdr;
    s_info.dlpi_phnum = 2;
    s_info.dlpi_adds = 1;
    s_have = 1;
}

int __wrap_dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data)
{
    if (s_have) {
        struct dl_phdr_info copy = s_info;
        int r = cb(&copy, sizeof(copy), data);
        if (r)
            return r;
    }
    return __real_dl_iterate_phdr(cb, data);
}
