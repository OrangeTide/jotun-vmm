/* boot.c : read the host-supplied boot parameter block.
 *
 * The block lives at a fixed guest-physical address (JOTUN_BOOT_BASE). The
 * argument strings are NUL-terminated and concatenated in blob[]; boot_init
 * indexes them once so boot_argv is a simple lookup. */
#include "boot.h"

#include <stddef.h>

#include "jotun_boot.h"

#define MAX_ARGV    32

static const struct jotun_bootinfo *info;
static const char *argv[MAX_ARGV];
static int argc;

int
boot_init(void)
{
    const struct jotun_bootinfo *b = (const struct jotun_bootinfo *)(uintptr_t)JOTUN_BOOT_BASE;

    if (b->magic != JOTUN_BOOT_MAGIC || b->version != JOTUN_BOOT_VERSION)
        return -1;

    info = b;

    /* Walk the blob, recording the start of each of the first argc strings.
     * blob_len bounds the walk so a malformed block cannot run off the page. */
    uint32_t n = b->argc;
    if (n > MAX_ARGV)
        n = MAX_ARGV;

    uint32_t off = 0;
    for (uint32_t i = 0; i < n && off < b->blob_len; i++) {
        argv[i] = (const char *)&b->blob[off];
        argc++;
        while (off < b->blob_len && b->blob[off] != '\0')
            off++;
        off++;      /* step past the terminator */
    }
    return 0;
}

uint64_t
boot_ram_size(void)
{
    return info ? info->ram_size : 0;
}

int
boot_argc(void)
{
    return argc;
}

const char *
boot_argv(int i)
{
    if (i < 0 || i >= argc)
        return NULL;
    return argv[i];
}
