/* elfload.h : load an ELF64 program from an open 9P fid into guest memory. */
#ifndef LIBOS_ELFLOAD_H
#define LIBOS_ELFLOAD_H

#include <stdint.h>

/* Load the PT_LOAD segments of the ELF open on fid, requiring every segment to
 * fall within [lo, hi). Returns the entry address, or 0 on any error. The fid
 * must already be walked to the file and opened for reading. */
uint64_t elf_load_fid(uint32_t fid, uint64_t lo, uint64_t hi);

#endif /* LIBOS_ELFLOAD_H */
