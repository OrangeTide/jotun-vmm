/* elf.h : minimal 32-bit ELF loader into a flat guest-RAM buffer */

#ifndef ELF_H
#define ELF_H

#include <stddef.h>
#include <stdint.h>

typedef struct elf_image {
    uint32_t entry;         /* guest entry point (e_entry) */
    uint32_t lo;            /* lowest loaded guest address */
    uint32_t hi;            /* one past the highest loaded guest address */
    int nsegments;          /* number of PT_LOAD segments mapped */
} elf_image;

/* Load every PT_LOAD segment of the little-endian ELF32 file at `path` into
 * `ram`, a flat buffer whose byte 0 is guest-physical address 0 and which is
 * `ram_size` bytes long. Segment p_paddr values index directly into `ram`.
 *
 * Returns 0 on success with *img filled in, or -1 on failure with a message on
 * stderr. */
int elf_load_file(const char *path, uint8_t *ram, size_t ram_size,
                  elf_image *img);

#endif /* ELF_H */
