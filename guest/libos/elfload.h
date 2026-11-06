/* elfload.h : load an ELF64 program from an open 9P fid into guest memory. */
#ifndef LIBOS_ELFLOAD_H
#define LIBOS_ELFLOAD_H

#include <stdint.h>

/* What a load produced, for the auxv and the interpreter handoff. */
struct elf_info {
    uint64_t entry;         /* bias + e_entry, the absolute entry point */
    uint64_t bias;          /* load offset: 0 for ET_EXEC, chosen for ET_DYN */
    uint64_t phdr;          /* in-memory address of the program headers */
    uint64_t phentsize;     /* e_phentsize */
    uint64_t phnum;         /* e_phnum */
    uint64_t load_end;      /* highest mapped vaddr, to place the next object */
    int      has_interp;    /* a PT_INTERP segment was present */
    char     interp[128];   /* the PT_INTERP path string */
};

/* Load the PT_LOAD segments of the ELF open on fid, requiring every segment to
 * fall within [lo, hi). For ET_DYN (a PIE or a dynamic linker), the segments
 * are placed at bias_hint + p_vaddr; for ET_EXEC the vaddrs are absolute and
 * bias_hint is ignored. Fills out with the entry, bias, program-header address,
 * and any PT_INTERP path. Returns 0 on success, -1 on any error. The fid must
 * already be walked to the file and opened for reading. */
int elf_load_ex(uint32_t fid, uint64_t lo, uint64_t hi, uint64_t bias_hint,
                struct elf_info *out);

/* Load an ELF the old way: segments at their absolute p_vaddr within [lo, hi),
 * returning the entry address or 0 on error. A thin wrapper over elf_load_ex
 * for the native loader and the static non-PIE path. */
uint64_t elf_load_fid(uint32_t fid, uint64_t lo, uint64_t hi);

#endif /* LIBOS_ELFLOAD_H */
