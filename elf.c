/* elf.c : minimal 32-bit ELF loader into a flat guest-RAM buffer */

#include "elf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EI_NIDENT   16
#define ELFCLASS32  1
#define ELFDATA2LSB 1
#define ET_EXEC     2
#define PT_LOAD     1

/* The guest is x86, so the image is little-endian; these readers assume it. */
static uint16_t
rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t
rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A guest image is small and read once, so slurping the whole file keeps the
 * header walking free of seek bookkeeping. */
static uint8_t *
slurp(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f) {
        fprintf(stderr, "elf: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fprintf(stderr, "elf: cannot seek %s\n", path);
        return NULL;
    }
    n = ftell(f);
    rewind(f);
    if (n < EI_NIDENT) {
        fclose(f);
        fprintf(stderr, "elf: %s is too small to be an ELF file\n", path);
        return NULL;
    }
    buf = malloc((size_t)n);
    if (!buf) {
        fclose(f);
        fprintf(stderr, "elf: out of memory reading %s\n", path);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        fprintf(stderr, "elf: short read on %s\n", path);
        return NULL;
    }
    fclose(f);
    *size = n;
    return buf;
}

int
elf_load_file(const char *path, uint8_t *ram, size_t ram_size, elf_image *img)
{
    long size;
    uint8_t *b = slurp(path, &size);
    uint32_t phoff, phentsize, phnum, i;

    if (!b)
        return -1;

    if (size < 52 || memcmp(b, "\177ELF", 4) != 0) {
        fprintf(stderr, "elf: %s is not an ELF file\n", path);
        goto fail;
    }
    if (b[4] != ELFCLASS32 || b[5] != ELFDATA2LSB) {
        fprintf(stderr, "elf: %s is not a little-endian ELF32 image\n", path);
        goto fail;
    }
    if (rd16(b + 16) != ET_EXEC) {
        fprintf(stderr, "elf: %s is not an executable image\n", path);
        goto fail;
    }

    memset(img, 0, sizeof(*img));
    img->entry = rd32(b + 24);
    img->lo = 0xFFFFFFFFu;
    img->hi = 0;

    phoff = rd32(b + 28);
    phentsize = rd16(b + 42);
    phnum = rd16(b + 44);
    if (phnum == 0 || phoff == 0) {
        fprintf(stderr, "elf: %s has no program headers\n", path);
        goto fail;
    }

    for (i = 0; i < phnum; i++) {
        const uint8_t *ph = b + phoff + i * phentsize;
        uint32_t type, offset, paddr, filesz, memsz;

        if ((long)(phoff + (i + 1) * phentsize) > size) {
            fprintf(stderr, "elf: %s: program headers past end of file\n",
                    path);
            goto fail;
        }
        type = rd32(ph + 0);
        if (type != PT_LOAD)
            continue;
        offset = rd32(ph + 4);
        paddr = rd32(ph + 12);  /* p_paddr: physical load address */
        filesz = rd32(ph + 16);
        memsz = rd32(ph + 20);

        if ((long)(offset + filesz) > size) {
            fprintf(stderr, "elf: %s: segment %u past end of file\n", path, i);
            goto fail;
        }
        if (paddr + memsz > ram_size || paddr + memsz < paddr) {
            fprintf(stderr, "elf: %s: segment %u does not fit in guest RAM "
                    "(0x%x + 0x%x > 0x%zx)\n",
                    path, i, paddr, memsz, ram_size);
            goto fail;
        }

        memcpy(ram + paddr, b + offset, filesz);
        if (memsz > filesz)
            memset(ram + paddr + filesz, 0, memsz - filesz);

        if (paddr < img->lo)
            img->lo = paddr;
        if (paddr + memsz > img->hi)
            img->hi = paddr + memsz;
        img->nsegments++;
    }

    if (img->nsegments == 0) {
        fprintf(stderr, "elf: %s has no loadable segments\n", path);
        goto fail;
    }

    free(b);
    return 0;

fail:
    free(b);
    return -1;
}
