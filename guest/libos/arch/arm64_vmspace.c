/* arm64_vmspace.c : per-process address spaces on AArch64. See arm64_vmspace.h.
 *
 * Mirrors the x86 backend: a free-list frame allocator, a per-process top-level
 * table that identity-maps the kernel and MMIO and keeps a private user window,
 * and a physmap so the kernel can reach any frame by physical address. The
 * differences are the ARMv8 translation-table format and that the first boot
 * turns the MMU on (the guest boots with it off).
 *
 * The translation granule is chosen at boot from what the CPU supports. Apple
 * Silicon only keeps the page-table walker coherent in 16 KiB units, so a 4 KiB
 * table (four to an Apple page) aliases its neighbours and the walker reads
 * stale descriptors; there the 16 KiB granule is used, which puts every table in
 * its own Apple page. A CPU that does not implement the (optional) 16 KiB
 * granule, such as a Cortex-A72, uses the 4 KiB granule instead, where the
 * walker is coherent at 4 KiB. Both use the same 36-bit VA and the same virtual
 * layout; only the table geometry differs:
 *
 *   16 KiB granule: 2-level walk from level 2. The root is the level-2 table,
 *       2048 entries of a 32 MiB block or a level-3 table; level-3 maps 16 KiB
 *       pages. The physmap is eight 32 MiB blocks in the root.
 *   4 KiB granule: 3-level walk from level 1. The root is the level-1 table,
 *       64 entries covering 1 GiB each; level-2 holds 2 MiB blocks or level-3
 *       tables; level-3 maps 4 KiB pages. The low 1 GiB (kernel, user window,
 *       device) hangs off L1[0]; the physmap is a single 1 GiB block at L1[1];
 *       the heap window hangs off L1[2].
 *
 * The frame allocator and every caller work in VM_PAGE (16 KiB) units regardless
 * of granule: on the 4 KiB granule a 16 KiB logical page is simply four adjacent
 * level-3 entries, so the Linux-compat page size stays 16 KiB on both. Only this
 * file knows the hardware page size.
 */
#include "arm64_vmspace.h"

#include "arch.h"
#include "mem.h"
#include "print.h"
#include "string.h"

/* ARMv8 descriptor bits. The block/table/page encodings and attribute indices
 * are the same at every level and for both granules; only the output-address
 * field width differs (set at boot in g_addr). */
#define D_BLOCK      0x1ULL                 /* block (bits[1:0]=0b01) */
#define D_TABLE      0x3ULL                 /* table, or L3 page (bits[1:0]=0b11) */
#define D_AF         (1ULL << 10)           /* access flag; 0 would fault */
#define D_NG         (1ULL << 11)           /* not-global: tagged by ASID */
#define D_SH_INNER   (3ULL << 8)            /* inner shareable */
#define D_ATTR_NORM  (0ULL << 2)            /* MAIR index 0: Normal WB */
#define D_ATTR_DEV   (1ULL << 2)            /* MAIR index 1: Device-nGnRE */
#define D_ATTR_NC    (2ULL << 2)            /* MAIR index 2: Normal non-cacheable */

#define BLOCK_NORMAL (D_BLOCK | D_AF | D_SH_INNER | D_ATTR_NORM)
#define BLOCK_DEVICE (D_BLOCK | D_AF | D_ATTR_DEV)
/* The physmap is mapped Normal non-cacheable: the kernel's physmap reads of a
 * page table then come from memory, coherent with what the hardware walker sees,
 * rather than from a stale cacheable line. */
#define BLOCK_PHYS   (D_BLOCK | D_AF | D_SH_INNER | D_ATTR_NC)
#define PAGE_NORMAL  (D_TABLE | D_AF | D_SH_INNER | D_ATTR_NORM)
/* A private user page: not-global, so it is tagged with the owning address
 * space's ASID and a stale entry from a previous space cannot be used after a
 * switch. The shared kernel identity, physmap and heap-window pages stay global
 * (no nG), since they map the same in every space. */
#define PAGE_USER    (PAGE_NORMAL | D_NG)

/* MAIR: attr0 Normal write-back RW-allocate, attr1 Device-nGnRE, attr2 Normal
 * non-cacheable. Same for both granules. */
#define MAIR_VALUE   0x00000000000444FFULL

/* TCR, less the granule field: 36-bit VA (T0SZ=28), Non-cacheable Non-shareable
 * table walks (IRGN0 = ORGN0 = SH0 = 0), TTBR1 walks disabled (EPD1), 40-bit
 * intermediate PA. TG0 (bits[15:14]) is OR-ed in at boot: 0b10 for 16 KiB, 0b00
 * for 4 KiB. */
#define TCR_BASE     (28ULL | (1ULL << 23) | (2ULL << 32))

/* Largest table this build allocates statically (the 16 KiB granule's 2048
 * entries). The 4 KiB granule uses the first 512. */
#define TBL_MAX      2048

/* ---- translation geometry, chosen at boot ------------------------------- */
static uint64_t g_hwpage;       /* hardware page: 0x1000 (4K) or 0x4000 (16K) */
static unsigned g_pgshift;      /* log2 hardware page: 12 or 14 */
static unsigned g_l2shift;      /* 21 (4K, 2 MiB) or 25 (16K, 32 MiB) */
static unsigned g_l1shift;      /* 30 (4K, 1 GiB); unused on 16K */
static unsigned g_entries;      /* table entries: 512 (4K) or 2048 (16K) */
static uint64_t g_addr;         /* output-address field mask */
static uint64_t g_tcr;          /* TCR_EL1 value including TG0 */
static int      g_three;        /* 4K: a level-1 root above the level-2 (3 levels) */

#define L1I(va)   (((va) >> g_l1shift) & (g_entries - 1))
#define L2I(va)   (((va) >> g_l2shift) & (g_entries - 1))
#define L3I(va)   (((va) >> g_pgshift) & (g_entries - 1))
#define L2_BLK    (1ULL << g_l2shift)
#define FRAME_PTES (VM_PAGE / g_hwpage)     /* level-3 entries per 16 KiB frame */

/* Physical frame allocator: a free list threaded through the free frames. */
static uint64_t free_head;
static int      physmap_on;                 /* phys_to_virt uses the physmap once set */

/* The address space the fault handler maps into (the running process). */
vmspace_t cur_vmspace;

/* The stack guard band pf_handle refuses to back; see vmspace.h. */
uint64_t pf_guard_lo, pf_guard_hi;

/* The demand-grown kernel heap window. A run of level-3 tables (built once by
 * kheap_init) is shared by every address space. On the 4 KiB granule they hang
 * off one level-2 table (kheap_l2); on 16 KiB they sit directly in the root. */
#define KHEAP_WIN    0x10000000ULL          /* 256 MiB window */
static uint64_t     kheap_l3[128];          /* enough L3 tables for 256 MiB at 4K */
static uint64_t     kheap_l2;               /* 4K: the L2 holding the L3 tables */
static int          kheap_nl3;
static uint64_t     kheap_top;
static volatile int kheap_lock;
static int          kheap_live;

#define KHEAP_CHUNK  0x200000ULL            /* map at least 2 MiB per growth */

void *
phys_to_virt(uint64_t p)
{
    /* Only frames in the user-window physical range need the physmap: a process
     * map replaces the identity mapping of [VM_USER_LO, VM_USER_HI) with its
     * private user pages, so a frame there is unreachable by its physical address
     * and the physmap at VM_PHYSMAP reaches it instead. Everything else stays
     * identity-mapped in every map, so heap access does not depend on the
     * physmap. */
    if (physmap_on && p >= VM_USER_LO && p < VM_USER_HI)
        return (void *)(uintptr_t)(VM_PHYSMAP + p);
    return (void *)(uintptr_t)p;
}

void
phys_init(uint64_t lo, uint64_t hi)
{
    lo = (lo + VM_PAGE - 1) & ~(VM_PAGE - 1);
    hi &= ~(VM_PAGE - 1);
    free_head = 0;
    for (uint64_t f = hi; f >= lo + VM_PAGE; ) {
        f -= VM_PAGE;
        *(uint64_t *)phys_to_virt(f) = free_head;
        free_head = f;
    }
}

uint64_t
phys_alloc(void)
{
    uint64_t f = free_head;
    if (f == 0)
        return 0;
    free_head = *(uint64_t *)phys_to_virt(f);
    memset(phys_to_virt(f), 0, VM_PAGE);
    return f;
}

void
phys_free(uint64_t frame)
{
    if (frame == 0)
        return;
    *(uint64_t *)phys_to_virt(frame) = free_head;
    free_head = frame;
}

/* Map a table frame, or a vmspace handle, to its kernel pointer. g_addr masks off
 * both the low bits and the ASID a vmspace handle carries in bits[55:48], so this
 * accepts a bare frame or a vmspace_t alike. */
static uint64_t *
tbl(uint64_t frame)
{
    return (uint64_t *)phys_to_virt(frame & g_addr);
}

static uint64_t
pte_frame(uint64_t e)
{
    return e & g_addr;
}

/* The 16 KiB frame a level-3 entry belongs to (its output masked to the frame
 * unit). On the 4 KiB granule the first of a frame's four entries recovers the
 * frame; on 16 KiB the entry's output is already the frame. */
static uint64_t
pte_frame16(uint64_t e)
{
    return pte_frame(e) & ~(VM_PAGE - 1);
}

/* ASID allocator: a bitmap of the 255 usable ASIDs (0 is the boot map). */
static uint64_t asid_used[4] = { 1 };

static uint64_t
asid_alloc(void)
{
    for (int i = 0; i < 256; i++) {
        if (!(asid_used[i >> 6] & (1ULL << (i & 63)))) {
            asid_used[i >> 6] |= (1ULL << (i & 63));
            return (uint64_t)i;
        }
    }
    return 0;
}

static void
asid_free(uint64_t asid)
{
    if (asid == 0)
        return;
    asid_used[asid >> 6] &= ~(1ULL << (asid & 63));
    __asm__ volatile ("dsb ish; tlbi aside1is, %0; dsb ish; isb"
                      : : "r"(asid << 48) : "memory");
}

/* Publish a translation-table write to the hardware table walker. */
static inline void
pte_sync(void *entry)
{
    __asm__ volatile ("dc civac, %0" : : "r"(entry) : "memory");
    __asm__ volatile ("dsb ish" ::: "memory");
}

/* Clean a whole freshly built table frame to the PoC (see pte_sync). */
static void
tbl_clean(uint64_t frame)
{
    uint8_t *p = (uint8_t *)phys_to_virt(frame & ~(VM_PAGE - 1));
    for (uint64_t off = 0; off < VM_PAGE; off += 64)
        __asm__ volatile ("dc civac, %0" : : "r"(p + off) : "memory");
    __asm__ volatile ("dsb ish");
}

/* ---- the user-window level-2 table -------------------------------------- */
/* The whole user window (and, on 4K, the low-GiB kernel/device mappings) lives
 * under one level-2 table. On 16 KiB the root is that table; on 4 KiB it hangs
 * off L1[0], allocated on demand. Returns NULL only on a create failure. */
static uint64_t *
user_l2(vmspace_t as, int create)
{
    uint64_t *r = tbl(as);
    if (!g_three)
        return r;                           /* the root is the level-2 table */
    if (!(r[0] & 1)) {
        if (!create)
            return NULL;
        uint64_t l2 = phys_alloc();
        if (!l2)
            return NULL;
        r[0] = l2 | D_TABLE;
        pte_sync(&r[0]);
    }
    return tbl(pte_frame(r[0]));
}

/* Walk to the level-3 entry for a hardware-page-aligned user VA, allocating the
 * level-3 table if needed. */
static uint64_t *
walk_hw(vmspace_t as, uint64_t va, int create)
{
    uint64_t *l2 = user_l2(as, create);
    if (!l2)
        return NULL;
    uint64_t bi = L2I(va);
    uint64_t e = l2[bi];
    if (e && (e & 0x3) == D_BLOCK)
        return NULL;                        /* a block sits here (kernel/device) */
    if (!(e & 1)) {
        if (!create)
            return NULL;
        uint64_t l3 = phys_alloc();
        if (!l3)
            return NULL;
        l2[bi] = l3 | D_TABLE;
        pte_sync(&l2[bi]);
        e = l2[bi];
    }
    uint64_t *l3 = tbl(pte_frame(e));
    return &l3[L3I(va)];
}

int
vmspace_map(vmspace_t as, uint64_t vaddr, uint64_t frame, uint64_t flags)
{
    (void)flags;                            /* every user page is RW at EL1 */
    if (vaddr < VM_USER_LO || vaddr >= VM_USER_HI)
        return -1;
    /* A 16 KiB logical frame is one level-3 entry on the 16 KiB granule and four
     * adjacent entries on the 4 KiB granule. */
    for (unsigned k = 0; k < FRAME_PTES; k++) {
        uint64_t *e = walk_hw(as, vaddr + (uint64_t)k * g_hwpage, 1);
        if (!e)
            return -1;
        *e = ((frame + (uint64_t)k * g_hwpage) & g_addr) | PAGE_USER;
        pte_sync(e);
    }
    return 0;
}

uint64_t
vmspace_lookup(vmspace_t as, uint64_t vaddr)
{
    if (vaddr < VM_USER_LO || vaddr >= VM_USER_HI)
        return 0;
    uint64_t *e = walk_hw(as, vaddr, 0);
    if (!e || !(*e & 1))
        return 0;
    return pte_frame16(*e);
}

int
vmspace_unmap(vmspace_t as, uint64_t vaddr)
{
    if (vaddr < VM_USER_LO || vaddr >= VM_USER_HI)
        return 0;
    uint64_t *first = walk_hw(as, vaddr, 0);
    if (!first || !(*first & 1))
        return 0;
    phys_free(pte_frame16(*first));
    for (unsigned k = 0; k < FRAME_PTES; k++) {
        uint64_t *e = walk_hw(as, vaddr + (uint64_t)k * g_hwpage, 0);
        if (e) {
            *e = 0;
            pte_sync(e);
        }
    }
    if ((as & g_addr) == (cur_vmspace & g_addr))
        __asm__ volatile ("dsb ishst; tlbi vmalle1is; dsb ish; isb");
    return 1;
}

/* Turn the MMU on with as in TTBR0. The executing code is identity-mapped, so it
 * keeps running across the enable. */
static void
mmu_enable(vmspace_t as)
{
    uint64_t sctlr;
    __asm__ volatile ("msr mair_el1, %0" : : "r"(MAIR_VALUE));
    __asm__ volatile ("msr tcr_el1, %0"  : : "r"(g_tcr));
    __asm__ volatile ("msr ttbr0_el1, %0" : : "r"(as));
    __asm__ volatile ("dsb ish; isb");
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    /* M: enable the MMU. C and I: enable the data and instruction caches. The
     * caches are not optional on Apple Silicon: it only implements the exclusive
     * monitor behind the atomics for cacheable Normal memory, so with the data
     * cache off the first heap lock faults. RAM is mapped Normal write-back. */
    sctlr |= (1ULL << 0) | (1ULL << 2) | (1ULL << 12);
    __asm__ volatile ("msr sctlr_el1, %0; isb" : : "r"(sctlr));
}

/* Fill a level-3 table's entries for [va_lo, va_hi) with identity Normal pages.
 * Used by the 16 KiB path to carve the kernel identity back into the two root
 * blocks that are otherwise handed to the user window. */
static void
l3_identity(uint64_t l3, uint64_t va_lo, uint64_t va_hi)
{
    uint64_t *p = tbl(l3);
    for (uint64_t va = va_lo; va < va_hi; va += g_hwpage)
        p[L3I(va)] = (va & g_addr) | PAGE_NORMAL;
}

/* 4 KiB path: fill the low-GiB level-2 table with the shared mappings, leaving
 * the user window invalid for demand paging. Covers [0,1 GiB): the kernel image
 * and heap band as Normal 2 MiB blocks, the band up to the physmap as Device. */
static void
fill_low_l2(uint64_t l2)
{
    uint64_t *p = tbl(l2);
    for (uint64_t va = 0; va < VM_USER_LO; va += L2_BLK)
        p[L2I(va)] = (va & g_addr) | BLOCK_NORMAL;          /* kernel image */
    for (uint64_t va = VM_KHEAP_LO; va < VM_RAM_SIZE; va += L2_BLK)
        p[L2I(va)] = (va & g_addr) | BLOCK_NORMAL;          /* heap band + stacks */
    for (uint64_t va = VM_RAM_SIZE; va < VM_PHYSMAP; va += L2_BLK)
        p[L2I(va)] = (va & g_addr) | BLOCK_DEVICE;          /* device MMIO */
}

vmspace_t
vmspace_new(void)
{
    uint64_t root = phys_alloc();
    if (!root)
        return 0;
    uint64_t *r = tbl(root);

    if (g_three) {
        /* 4 KiB granule: a level-1 root. L1[0] is the low-GiB level-2 table
         * (kernel, user window, device); L1[physmap] is a single 1 GiB block
         * mapping RAM non-cacheably; L1[heap] points at the shared heap level-2
         * table. */
        uint64_t l2 = phys_alloc();
        if (!l2) {
            phys_free(root);
            return 0;
        }
        fill_low_l2(l2);
        r[0] = l2 | D_TABLE;
        r[L1I(VM_PHYSMAP)] = (0ULL & g_addr) | BLOCK_PHYS;  /* 1 GiB block, PA 0 */
        if (kheap_l2)
            r[L1I(VM_KHEAP_VA)] = kheap_l2 | D_TABLE;
        tbl_clean(l2);
    } else {
        /* 16 KiB granule: the root is the level-2 table. The two blocks that
         * straddle the user window (the first, holding the kernel image, and the
         * last, holding the heap band and stacks) become level-3 tables; the
         * interior user blocks are demand-filled. The band up to the physmap is
         * Device; the physmap is eight 32 MiB blocks in the root; the heap window
         * is a run of shared level-3 tables. */
        uint64_t l3_lo = phys_alloc();
        uint64_t l3_hi = phys_alloc();
        if (!l3_lo || !l3_hi) {
            phys_free(root); phys_free(l3_lo); phys_free(l3_hi);
            return 0;
        }
        l3_identity(l3_lo, 0, VM_USER_LO);
        l3_identity(l3_hi, VM_KHEAP_LO, VM_RAM_SIZE);
        r[L2I(VM_USER_LO)] = l3_lo | D_TABLE;
        r[L2I(VM_USER_HI - 1)] = l3_hi | D_TABLE;
        for (uint64_t va = VM_RAM_SIZE; va < VM_PHYSMAP; va += L2_BLK)
            r[L2I(va)] = (va & g_addr) | BLOCK_DEVICE;
        for (uint64_t off = 0; off < VM_RAM_SIZE; off += L2_BLK)
            r[L2I(VM_PHYSMAP + off)] = (off & g_addr) | BLOCK_PHYS;
        for (int k = 0; k < kheap_nl3; k++)
            r[L2I(VM_KHEAP_VA) + k] = kheap_l3[k] | D_TABLE;
        tbl_clean(l3_lo);
        tbl_clean(l3_hi);
    }

    tbl_clean(root);
    /* Carry this space's ASID in the handle so every switch to it uses the same
     * ASID and its not-global user entries survive the scheduler's TTBR0 reloads;
     * tbl() masks the ASID off to recover the table address. */
    return root | (asid_alloc() << 48);
}

/* Boot-time translation tables, built like a process map but with no user
 * window, and no global mapping of the user-window VAs (a global boot entry
 * there would shadow a process's not-global user pages after the switch). The
 * tables live in the kernel image (identity-mapped by themselves), so no
 * allocator is needed. Sized for the larger (16 KiB) geometry. */
static uint64_t boot_root[TBL_MAX] __attribute__((aligned(0x4000)));
static uint64_t boot_ta[TBL_MAX]   __attribute__((aligned(0x4000)));
static uint64_t boot_tb[TBL_MAX]   __attribute__((aligned(0x4000)));

/* Detect the granule the CPU supports and set the geometry. 16 KiB is preferred
 * where available (Apple Silicon needs it for walker coherency); otherwise 4 KiB.
 * ID_AA64MMFR0_EL1.TGran16 (bits[23:20]) is 0 when 16 KiB is not implemented. */
static void
geom_detect(void)
{
    uint64_t mmfr0;
    __asm__ volatile ("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    unsigned tgran16 = (unsigned)((mmfr0 >> 20) & 0xf);

    if (tgran16 != 0) {
        g_hwpage = 0x4000; g_pgshift = 14; g_l2shift = 25;
        g_entries = 2048; g_addr = 0x0000FFFFFFFFC000ULL;
        g_tcr = TCR_BASE | (2ULL << 14);        /* TG0 = 0b10 */
        g_three = 0;
    } else {
        g_hwpage = 0x1000; g_pgshift = 12; g_l2shift = 21; g_l1shift = 30;
        g_entries = 512; g_addr = 0x0000FFFFFFFFF000ULL;
        g_tcr = TCR_BASE | (0ULL << 14);        /* TG0 = 0b00 */
        g_three = 1;
    }
}

void
arch_mmu_init(void)
{
    /* Every cpu runs this; they all write the same descriptors (a benign race)
     * and enable translation on themselves. The physmap is marked live from here
     * on, so the kernel reaches every frame through one virtual address from the
     * start, keeping the frame pool coherent. */
    geom_detect();

    if (g_three) {
        /* 4 KiB: L1 root, boot_ta is the low-GiB level-2 (kernel + device, no
         * user window), boot_tb unused; physmap is a 1 GiB L1 block. */
        for (uint64_t va = 0; va < VM_USER_LO; va += L2_BLK)
            boot_ta[L2I(va)] = (va & g_addr) | BLOCK_NORMAL;
        for (uint64_t va = VM_KHEAP_LO; va < VM_RAM_SIZE; va += L2_BLK)
            boot_ta[L2I(va)] = (va & g_addr) | BLOCK_NORMAL;
        for (uint64_t va = VM_RAM_SIZE; va < VM_PHYSMAP; va += L2_BLK)
            boot_ta[L2I(va)] = (va & g_addr) | BLOCK_DEVICE;
        boot_root[0] = (uint64_t)(uintptr_t)boot_ta | D_TABLE;
        boot_root[L1I(VM_PHYSMAP)] = (0ULL & g_addr) | BLOCK_PHYS;
    } else {
        /* 16 KiB: the root is the level-2 table; boot_ta/boot_tb are the two
         * straddle level-3 tables carrying only the kernel identity. */
        for (uint64_t va = 0; va < VM_USER_LO; va += g_hwpage)
            boot_ta[L3I(va)] = (va & g_addr) | PAGE_NORMAL;
        for (uint64_t va = VM_KHEAP_LO; va < VM_RAM_SIZE; va += g_hwpage)
            boot_tb[L3I(va)] = (va & g_addr) | PAGE_NORMAL;
        boot_root[L2I(VM_USER_LO)] = (uint64_t)(uintptr_t)boot_ta | D_TABLE;
        boot_root[L2I(VM_USER_HI - 1)] = (uint64_t)(uintptr_t)boot_tb | D_TABLE;
        for (uint64_t va = VM_RAM_SIZE; va < VM_PHYSMAP; va += L2_BLK)
            boot_root[L2I(va)] = (va & g_addr) | BLOCK_DEVICE;
        for (uint64_t off = 0; off < VM_RAM_SIZE; off += L2_BLK)
            boot_root[L2I(VM_PHYSMAP + off)] = (off & g_addr) | BLOCK_PHYS;
    }

    __asm__ volatile ("dsb ish");
    mmu_enable((vmspace_t)(uintptr_t)boot_root);
    physmap_on = 1;
}

void
vmspace_switch(vmspace_t as)
{
    /* The MMU is already on, so this just repoints TTBR0 at the process map. The
     * handle carries the space's stable ASID in bits[55:48], so no TLB flush is
     * needed: the not-global user entries are tagged by ASID. */
    __asm__ volatile ("msr ttbr0_el1, %0" : : "r"(as));
    __asm__ volatile ("isb");
    kheap_live = 1;
    cur_vmspace = as;
}

/* Map one zeroed pool frame at window VA va (page aligned, >= VM_KHEAP_VA) into
 * the shared heap-window level-3 tables. Returns 0, or -1 if the pool is empty or
 * the window is exhausted. The caller holds kheap_lock. */
static int
kheap_map_page(uint64_t va)
{
    uint64_t idx = (va - VM_KHEAP_VA) >> g_l2shift;
    if (idx >= (uint64_t)kheap_nl3)
        return -1;
    uint64_t *l3 = tbl(kheap_l3[idx]);
    uint64_t f = phys_alloc();
    if (!f)
        return -1;
    /* A 16 KiB frame is one or four level-3 entries. */
    for (unsigned k = 0; k < FRAME_PTES; k++) {
        uint64_t sva = va + (uint64_t)k * g_hwpage;
        l3[L3I(sva)] = ((f + (uint64_t)k * g_hwpage) & g_addr) | PAGE_NORMAL;
        pte_sync(&l3[L3I(sva)]);
    }
    return 0;
}

static size_t
kheap_grow(size_t need)
{
    if (!kheap_live)
        return 0;

    size_t want = need < KHEAP_CHUNK ? KHEAP_CHUNK : need;
    want = (want + VM_PAGE - 1) & ~(VM_PAGE - 1);

    unsigned long irq = arch_irq_save();
    while (__atomic_exchange_n(&kheap_lock, 1, __ATOMIC_ACQUIRE))
        arch_cpu_relax();

    uint64_t base = kheap_top;
    size_t done = 0;
    while (done < want) {
        if (kheap_map_page(kheap_top) != 0)
            break;
        kheap_top += VM_PAGE;
        done += VM_PAGE;
    }
    if (done)
        __asm__ volatile ("dsb ish; isb");

    __atomic_store_n(&kheap_lock, 0, __ATOMIC_RELEASE);
    arch_irq_restore(irq);

    if (done == 0)
        return 0;
    memadd((void *)(uintptr_t)base, done);
    return done < need ? 0 : done;
}

void
kheap_init(void)
{
    /* One level-3 table covers (g_entries << g_pgshift) of VA: 32 MiB at 16 KiB,
     * 2 MiB at 4 KiB. Allocate enough to span the window. On 4 KiB they hang off
     * one level-2 table installed in every process at L1[heap]. */
    uint64_t l3_span = (uint64_t)g_entries << g_pgshift;
    int want = (int)(KHEAP_WIN / l3_span);
    if (want > (int)(sizeof kheap_l3 / sizeof kheap_l3[0]))
        want = (int)(sizeof kheap_l3 / sizeof kheap_l3[0]);

    if (g_three) {
        kheap_l2 = phys_alloc();
        if (!kheap_l2)
            return;
    }
    uint64_t *l2p = g_three ? tbl(kheap_l2) : NULL;

    for (int k = 0; k < want; k++) {
        uint64_t l3 = phys_alloc();
        if (!l3)
            break;
        kheap_l3[k] = l3;
        if (g_three)
            l2p[k] = l3 | D_TABLE;          /* L2[k] -> the k-th L3 table */
        kheap_nl3++;
    }
    if (!kheap_nl3)
        return;
    if (g_three)
        tbl_clean(kheap_l2);
    kheap_top = VM_KHEAP_VA;
    mem_set_grower(kheap_grow);

    /* The window's tables are installed in every process by vmspace_new. The
     * guest boots with the MMU off, so the window comes alive at the first switch;
     * the grower stays idle until then. */
}

int
pf_handle(uint64_t far, uint64_t esr)
{
    uint32_t dfsc = (uint32_t)(esr & 0x3f);

    /* Only a translation fault (DFSC 0b0001LL) in the running process's user
     * window is ours. */
    if (!cur_vmspace || dfsc < 0x04 || dfsc > 0x07)
        return 0;
    if (far < VM_USER_LO || far >= VM_USER_HI)
        return 0;
    if (far >= pf_guard_lo && far < pf_guard_hi)
        return 0;

    uint64_t f = phys_alloc();
    if (!f)
        return 0;
    /* Back the whole 16 KiB logical page containing the fault. */
    if (vmspace_map(cur_vmspace, far & ~(VM_PAGE - 1), f, 0) != 0) {
        phys_free(f);
        return 0;
    }
    __asm__ volatile ("dsb ishst; tlbi vmalle1is; dsb ish; isb");
    return 1;
}

/* Visit every present user-window 16 KiB page, calling fn(vaddr, first-L3-entry,
 * arg). Steps the level-3 table by a frame (one or four entries) and skips VAs
 * outside the user window (kernel identity that shares a straddle block on 16K). */
static void
user_walk(vmspace_t as, void (*fn)(uint64_t, uint64_t *, void *), void *arg)
{
    uint64_t *l2 = user_l2(as, 0);
    if (!l2)
        return;
    uint64_t lo = L2I(VM_USER_LO);
    uint64_t hi = L2I(VM_USER_HI - 1);
    for (uint64_t bi = lo; bi <= hi; bi++) {
        uint64_t e = l2[bi];
        if (!(e & 1) || (e & 0x3) == D_BLOCK)
            continue;
        uint64_t *l3 = tbl(pte_frame(e));
        for (uint64_t i = 0; i < g_entries; i += FRAME_PTES) {
            if (!(l3[i] & 1))
                continue;
            uint64_t va = (bi << g_l2shift) | (i << g_pgshift);
            if (va < VM_USER_LO || va >= VM_USER_HI)
                continue;
            fn(va, &l3[i], arg);
        }
    }
}

static void
free_one(uint64_t va, uint64_t *e, void *arg)
{
    (void)va;
    (void)arg;
    phys_free(pte_frame16(*e));
    *e = 0;
}

void
vmspace_destroy(vmspace_t as)
{
    if (!as)
        return;
    user_walk(as, free_one, NULL);

    /* Free the user-window level-3 tables, then the per-process level-2 table
     * (4 KiB) or nothing extra (16 KiB, where the root is the level-2), then the
     * root. The physmap, device blocks and heap-window tables own no per-process
     * frames or are shared. */
    uint64_t *l2 = user_l2(as, 0);
    if (l2) {
        uint64_t lo = L2I(VM_USER_LO), hi = L2I(VM_USER_HI - 1);
        for (uint64_t bi = lo; bi <= hi; bi++) {
            if ((l2[bi] & 1) && (l2[bi] & 0x3) == D_TABLE)
                phys_free(pte_frame(l2[bi]));
        }
    }
    if (g_three) {
        uint64_t *r = tbl(as);
        if (r[0] & 1)
            phys_free(pte_frame(r[0]));      /* the low-GiB level-2 table */
    }
    asid_free((as >> 48) & 0xFF);
    phys_free(as & g_addr);                  /* the root table */
}

struct clone_ctx {
    vmspace_t dst;
    int       src_active;       /* the source space is the running one (fork) */
    int       failed;
};

static void
clone_one(uint64_t va, uint64_t *e, void *arg)
{
    struct clone_ctx *c = arg;

    if (c->failed)
        return;
    uint64_t nf = phys_alloc();
    if (!nf) {
        c->failed = 1;
        return;
    }
    uint8_t *dstp = phys_to_virt(nf);
    /* The copy below writes the child page through the non-cacheable physmap,
     * straight to RAM. A previous owner of this physical frame may have left
     * cache lines over it: a stale clean line would shadow the copy for the
     * child's cacheable mapping, and a stale dirty line could later evict and
     * clobber it. Drop them first, by physical address (the caches are physically
     * tagged). No cacheable alias of the frame exists yet (the child is mapped
     * below), so none can reappear in between. */
    for (uint64_t off = 0; off < VM_PAGE; off += 64)
        __asm__ volatile ("dc ivac, %0" : : "r"(dstp + off) : "memory");
    __asm__ volatile ("dsb ish" ::: "memory");
    /* Read the source from the parent's own cacheable user mapping, which is the
     * running address space during fork, so the copy sees data the parent has
     * written but not yet cleaned to the point of coherency. A read through the
     * non-cacheable physmap would miss those dirty cache lines and copy stale RAM.
     * Only fall back to the physmap if the source space is not the running one. */
    const void *srcp = c->src_active
                     ? (const void *)(uintptr_t)va
                     : (const void *)phys_to_virt(pte_frame16(*e));
    memcpy(dstp, srcp, VM_PAGE);
    __asm__ volatile ("dsb ish" ::: "memory");
    /* The copied page may hold code the child will execute, so make the
     * instruction cache refetch it from memory. */
    for (uint64_t off = 0; off < VM_PAGE; off += 64)
        __asm__ volatile ("ic ivau, %0" : : "r"(dstp + off) : "memory");
    __asm__ volatile ("dsb ish; isb" ::: "memory");
    if (vmspace_map(c->dst, va, nf, 0) != 0)
        c->failed = 1;
}

vmspace_t
vmspace_clone(vmspace_t src)
{
    struct clone_ctx c = { vmspace_new(), 0, 0 };

    if (!c.dst)
        return 0;
    c.src_active = ((src & g_addr) == (cur_vmspace & g_addr));
    user_walk(src, clone_one, &c);
    if (c.failed) {
        vmspace_destroy(c.dst);
        return 0;
    }
    __asm__ volatile ("dsb ish; isb" ::: "memory");
    return c.dst;
}
