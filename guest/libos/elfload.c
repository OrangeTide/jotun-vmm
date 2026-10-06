/* elfload.c : minimal ELF64 program loader, reading over 9P.
 *
 * Reads the header and program headers from the open fid, then copies each
 * PT_LOAD segment to its virtual address and zeroes the trailing bss. No
 * relocation: programs are linked at a fixed base (JOTUN_PROG_BASE), so they
 * load in place. */
#include "elfload.h"

#include "p9.h"

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

#define PT_LOAD     1
#define ELFCLASS64  2
#define ELFDATA2LSB 1

/* Read exactly len bytes at offset into dst, looping over short reads. Returns
 * the number of bytes read, which is len on success. */
static uint64_t
read_full(uint32_t fid, uint64_t offset, void *dst, uint64_t len)
{
    uint8_t *p = dst;
    uint64_t done = 0;

    while (done < len) {
        uint32_t want = (uint32_t)(len - done);
        long n = p9_read(fid, offset + done, p + done, want);
        if (n <= 0)
            break;
        done += (uint64_t)n;
    }
    return done;
}

uint64_t
elf_load_fid(uint32_t fid, uint64_t lo, uint64_t hi)
{
    Elf64_Ehdr eh;

    if (read_full(fid, 0, &eh, sizeof eh) != sizeof eh)
        return 0;
    if (eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' ||
        eh.e_ident[2] != 'L' || eh.e_ident[3] != 'F')
        return 0;
    if (eh.e_ident[4] != ELFCLASS64 || eh.e_ident[5] != ELFDATA2LSB)
        return 0;
    if (eh.e_phentsize < sizeof(Elf64_Phdr))
        return 0;

    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        Elf64_Phdr ph;
        uint64_t off = eh.e_phoff + (uint64_t)i * eh.e_phentsize;
        if (read_full(fid, off, &ph, sizeof ph) != sizeof ph)
            return 0;
        if (ph.p_type != PT_LOAD)
            continue;

        /* The whole segment, including bss, must fit the allowed window. */
        if (ph.p_vaddr < lo || ph.p_memsz > hi - ph.p_vaddr ||
            ph.p_filesz > ph.p_memsz)
            return 0;

        uint8_t *dst = (uint8_t *)(uintptr_t)ph.p_vaddr;
        if (read_full(fid, ph.p_offset, dst, ph.p_filesz) != ph.p_filesz)
            return 0;
        for (uint64_t b = ph.p_filesz; b < ph.p_memsz; b++)
            dst[b] = 0;
    }

    if (eh.e_entry < lo || eh.e_entry >= hi)
        return 0;
    return eh.e_entry;
}
