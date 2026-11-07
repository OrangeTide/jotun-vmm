/* elfload.c : minimal ELF64 program loader, reading over 9P.
 *
 * Reads the header and program headers from the open fid, then copies each
 * PT_LOAD segment to its virtual address and zeroes the trailing bss. An
 * ET_EXEC loads at its fixed p_vaddr with no relocation. An ET_DYN (a PIE or a
 * dynamic linker) loads at a caller-chosen bias; the dynamic linker then does
 * the relocation in userspace. */
#include "elfload.h"

#include "p9.h"
#include "string.h"

/* Where the ELF bytes come from: an open 9P fid, or an in-memory image (a
 * program built in the writable overlay, which has no 9P fid). Exactly one of
 * the two is used, chosen by whether mem is set. */
struct elf_src {
    uint32_t             fid;   /* 9P fid, used when mem is NULL */
    const unsigned char *mem;   /* in-memory image, or NULL to read over 9P */
    uint64_t             len;   /* mem length in bytes */
};

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
#define PT_INTERP   3
#define ET_EXEC     2
#define ET_DYN      3
#define ELFCLASS64  2
#define ELFDATA2LSB 1

/* Read exactly len bytes at offset into dst, looping over short reads (9P) or
 * copying from the in-memory image. Returns the number of bytes read, which is
 * len on success; a short count means the source ended early. */
static uint64_t
read_full(const struct elf_src *src, uint64_t offset, void *dst, uint64_t len)
{
    if (src->mem) {
        if (offset >= src->len)
            return 0;
        uint64_t avail = src->len - offset;
        uint64_t n = len < avail ? len : avail;
        memcpy(dst, src->mem + offset, n);
        return n;
    }

    uint8_t *p = dst;
    uint64_t done = 0;
    while (done < len) {
        uint32_t want = (uint32_t)(len - done);
        long n = p9_read(src->fid, offset + done, p + done, want);
        if (n <= 0)
            break;
        done += (uint64_t)n;
    }
    return done;
}

static int
elf_load_src(const struct elf_src *src, uint64_t lo, uint64_t hi,
             uint64_t bias_hint, struct elf_info *out)
{
    Elf64_Ehdr eh;

    if (read_full(src, 0, &eh, sizeof eh) != sizeof eh)
        return -1;
    if (eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' ||
        eh.e_ident[2] != 'L' || eh.e_ident[3] != 'F')
        return -1;
    if (eh.e_ident[4] != ELFCLASS64 || eh.e_ident[5] != ELFDATA2LSB)
        return -1;
    if (eh.e_phentsize < sizeof(Elf64_Phdr))
        return -1;
    if (eh.e_type != ET_EXEC && eh.e_type != ET_DYN)
        return -1;

    /* ET_EXEC carries absolute vaddrs; ET_DYN is relocatable, so apply a bias. */
    uint64_t bias = (eh.e_type == ET_EXEC) ? 0 : bias_hint;

    out->bias = bias;
    out->entry = bias + eh.e_entry;
    out->phentsize = eh.e_phentsize;
    out->phnum = eh.e_phnum;
    out->phdr = 0;
    out->load_end = 0;
    out->has_interp = 0;
    out->interp[0] = 0;

    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        Elf64_Phdr ph;
        uint64_t off = eh.e_phoff + (uint64_t)i * eh.e_phentsize;
        if (read_full(src, off, &ph, sizeof ph) != sizeof ph)
            return -1;

        if (ph.p_type == PT_INTERP) {
            uint64_t n = ph.p_filesz;
            if (n == 0 || n > sizeof out->interp)
                return -1;
            if (read_full(src, ph.p_offset, out->interp, n) != n)
                return -1;
            out->interp[n - 1] = 0;         /* the stored path is NUL-terminated */
            out->has_interp = 1;
            continue;
        }
        if (ph.p_type != PT_LOAD)
            continue;
        if (ph.p_filesz > ph.p_memsz)
            return -1;

        uint64_t va = bias + ph.p_vaddr;
        /* The whole segment, including bss, must fit the allowed window. */
        if (va < lo || ph.p_memsz > hi - va)
            return -1;

        uint8_t *dst = (uint8_t *)(uintptr_t)va;
        if (read_full(src, ph.p_offset, dst, ph.p_filesz) != ph.p_filesz)
            return -1;
        for (uint64_t b = ph.p_filesz; b < ph.p_memsz; b++)
            dst[b] = 0;

        if (va + ph.p_memsz > out->load_end)
            out->load_end = va + ph.p_memsz;
        /* The program headers live in the LOAD segment that spans e_phoff. */
        if (ph.p_offset <= eh.e_phoff &&
            eh.e_phoff < ph.p_offset + ph.p_filesz)
            out->phdr = va + (eh.e_phoff - ph.p_offset);
    }

    if (out->entry < lo || out->entry >= hi)
        return -1;
    return 0;
}

int
elf_load_ex(uint32_t fid, uint64_t lo, uint64_t hi, uint64_t bias_hint,
            struct elf_info *out)
{
    struct elf_src src = { .fid = fid, .mem = 0, .len = 0 };
    return elf_load_src(&src, lo, hi, bias_hint, out);
}

int
elf_load_mem(const void *data, uint64_t len, uint64_t lo, uint64_t hi,
             uint64_t bias_hint, struct elf_info *out)
{
    struct elf_src src = { .fid = 0, .mem = (const unsigned char *)data,
                           .len = len };
    return elf_load_src(&src, lo, hi, bias_hint, out);
}

uint64_t
elf_load_fid(uint32_t fid, uint64_t lo, uint64_t hi)
{
    struct elf_info info;

    if (elf_load_ex(fid, lo, hi, 0, &info) != 0)
        return 0;
    return info.entry;
}
