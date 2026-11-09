/* arm64_vmspace.c : per-process address spaces on AArch64. See arm64_vmspace.h.
 *
 * Mirrors the x86 backend: a free-list frame allocator, a per-process top-level
 * table that identity-maps the kernel and MMIO and keeps a private user window,
 * and a physmap so the kernel can reach any frame by physical address. The
 * differences are the ARMv8 translation-table format, that the first boot turns
 * the MMU on (the guest boots with it off), and a 16 KiB granule so every table
 * and every user page is a full Apple-Silicon page (see the header for why).
 *
 * 16 KiB granule, 36-bit VA: the walk starts at level 2. A level-2 table has
 * 2048 entries, each a 32 MiB block or a pointer to a level-3 table; a level-3
 * table has 2048 entries, each a 16 KiB page. The root table is the level-2
 * table. */
#include "arm64_vmspace.h"

#include "arch.h"
#include "mem.h"
#include "print.h"
#include "string.h"

/* ARMv8 descriptor bits (16 KiB granule). */
#define D_BLOCK      0x1ULL                 /* L2 32 MiB block (bits[1:0]=0b01) */
#define D_TABLE      0x3ULL                 /* table, or L3 page (bits[1:0]=0b11) */
#define D_AF         (1ULL << 10)           /* access flag; 0 would fault */
#define D_NG         (1ULL << 11)           /* not-global: tagged by ASID */
#define D_SH_INNER   (3ULL << 8)            /* inner shareable */
#define D_ATTR_NORM  (0ULL << 2)            /* MAIR index 0: Normal WB */
#define D_ATTR_DEV   (1ULL << 2)            /* MAIR index 1: Device-nGnRE */
#define D_ATTR_NC    (2ULL << 2)            /* MAIR index 2: Normal non-cacheable */
#define D_ADDR       0x0000FFFFFFFFC000ULL  /* output address field, bits[47:14] */

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
 * (PAGE_NORMAL, no nG), since they map the same in every space. */
#define PAGE_USER    (PAGE_NORMAL | D_NG)

/* Table geometry. L2 entries span 32 MiB; L3 entries span 16 KiB; both tables
 * hold 2048 entries. */
#define L2_SHIFT     25
#define L3_SHIFT     14
#define TBL_ENTRIES  2048
#define L2I(va)      (((va) >> L2_SHIFT) & (TBL_ENTRIES - 1))
#define L3I(va)      (((va) >> L3_SHIFT) & (TBL_ENTRIES - 1))
#define L2_BLOCK     (1ULL << L2_SHIFT)     /* 32 MiB */

/* The index in the root (level-2) table of the first and last blocks that hold
 * part of the user window, and the physmap and heap-window block ranges. */
#define RAM_BLOCKS   (VM_RAM_SIZE / L2_BLOCK)        /* 8 blocks of RAM */
#define USER_LO_BLK  (VM_USER_LO >> L2_SHIFT)        /* 0: kernel + user */
#define USER_HI_BLK  ((VM_USER_HI - 1) >> L2_SHIFT)  /* 7: user + heap/stacks */
#define PHYSMAP_BLK  (VM_PHYSMAP >> L2_SHIFT)        /* 32 */
#define KHEAP_BLK    (VM_KHEAP_VA >> L2_SHIFT)       /* 64 */
#define KHEAP_NL3    8                               /* heap window: 8*32 = 256 MiB */

/* MAIR: attr0 Normal write-back RW-allocate, attr1 Device-nGnRE. */
#define MAIR_VALUE   0x00000000000444FFULL

/* TCR: 36-bit VA (T0SZ=28) so the walk starts at level 2, 16 KiB TG0 (0b10),
 * Non-cacheable Non-shareable table walks (IRGN0 = ORGN0 = SH0 = 0), TTBR1 walks
 * disabled (EPD1), 40-bit intermediate PA. The walker reads descriptors straight
 * from memory and every descriptor write is cleaned to the Point of Coherency
 * with pte_sync, so the walker shares one view of the tables with the kernel's
 * physmap accesses. */
#define TCR_VALUE    (28ULL | (0ULL << 8) | (0ULL << 10) | (0ULL << 12) \
                      | (2ULL << 14) | (1ULL << 23) | (2ULL << 32))

/* Physical frame allocator: a free list threaded through the free frames. */
static uint64_t free_head;
static int      physmap_on;                 /* phys_to_virt uses the physmap once set */

/* The address space the fault handler maps into (the running process). */
vmspace_t cur_vmspace;

/* The stack guard band pf_handle refuses to back; see vmspace.h. Disabled until
 * the Linux-compat loader sets it. */
uint64_t pf_guard_lo, pf_guard_hi;

/* The demand-grown kernel heap window. A run of KHEAP_NL3 level-3 tables (built
 * once by kheap_init) is installed in every address space; kheap_top is the next
 * unmapped window VA. kheap_nl3 is 0 until kheap_init runs, which keeps
 * vmspace_new and the grower inert on the native boot path that never sets the
 * window up. */
static uint64_t     kheap_l3[KHEAP_NL3];
static int          kheap_nl3;
static uint64_t     kheap_top;
static volatile int kheap_lock;
static int          kheap_live;     /* a process map (with the window) is active */

#define KHEAP_CHUNK  0x200000ULL        /* map at least 2 MiB per growth */

void *
phys_to_virt(uint64_t p)
{
    /* Only frames in the user-window physical range need the physmap: a process
     * map replaces the identity mapping of [VM_USER_LO, VM_USER_HI) with its
     * private user pages, so a frame there (a page table or a user page the
     * kernel must touch) is unreachable by its physical address and the physmap
     * at VM_PHYSMAP reaches it instead. Everything else, the low kernel below the
     * window and the heap band and vcpu stacks above it, stays identity-mapped in
     * every map, so it is reached by its physical address directly. Keeping the
     * heap on the identity map means heap access does not depend on the physmap. */
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

/* Map a table frame, or a vmspace handle, to its kernel pointer. D_ADDR masks
 * off both the low bits and the ASID a vmspace handle carries in bits[55:48], so
 * this accepts a bare frame or a vmspace_t alike. */
static uint64_t *
tbl(uint64_t frame)
{
    return (uint64_t *)phys_to_virt(frame & D_ADDR);
}

/* ASID allocator: a bitmap of the 255 usable ASIDs (0 is the boot map). Each
 * address space keeps one stable ASID for its whole life, carried in its handle,
 * so its not-global user entries stay valid across the many TTBR0 reloads the
 * scheduler does. LINUX_NPROC is well under 255, so the bitmap never fills. */
static uint64_t asid_used[4] = { 1 };       /* ASID 0 reserved for the boot map */

static uint64_t
asid_alloc(void)
{
    for (int i = 0; i < 256; i++) {
        if (!(asid_used[i >> 6] & (1ULL << (i & 63)))) {
            asid_used[i >> 6] |= (1ULL << (i & 63));
            return (uint64_t)i;
        }
    }
    return 0;                               /* full (cannot happen): share ASID 0 */
}

static void
asid_free(uint64_t asid)
{
    if (asid == 0)
        return;
    asid_used[asid >> 6] &= ~(1ULL << (asid & 63));
    /* Drop this ASID's TLB entries so the next space to reuse it starts clean. */
    __asm__ volatile ("dsb ish; tlbi aside1is, %0; dsb ish; isb"
                      : : "r"(asid << 48) : "memory");
}

/* Publish a translation-table write to the hardware table walker: clean the
 * entry's cache line to the Point of Coherency and order it with a store
 * barrier. Every path that writes a descriptor routes through here (or tbl_clean
 * for a whole freshly built table). */
static inline void
pte_sync(void *entry)
{
    __asm__ volatile ("dc civac, %0" : : "r"(entry) : "memory");
    __asm__ volatile ("dsb ish" ::: "memory");
}

/* Clean a whole freshly built 16 KiB table frame to the PoC (see pte_sync). */
static void
tbl_clean(uint64_t frame)
{
    uint8_t *p = (uint8_t *)phys_to_virt(frame & ~(VM_PAGE - 1));
    for (uint64_t off = 0; off < VM_PAGE; off += 64)
        __asm__ volatile ("dc civac, %0" : : "r"(p + off) : "memory");
    __asm__ volatile ("dsb ish");
}

static uint64_t
pte_frame(uint64_t e)
{
    return e & D_ADDR;
}

/* Fill a level-3 table's entries for [va_lo, va_hi) with identity Normal pages.
 * Used to carve the kernel identity mapping back into the two root blocks that
 * are otherwise handed to the user window. */
static void
l3_identity(uint64_t l3, uint64_t va_lo, uint64_t va_hi)
{
    uint64_t *p = tbl(l3);
    for (uint64_t va = va_lo; va < va_hi; va += VM_PAGE)
        p[L3I(va)] = (va & D_ADDR) | PAGE_NORMAL;
}

vmspace_t
vmspace_new(void)
{
    uint64_t root = phys_alloc();           /* the level-2 (root) table */
    uint64_t l3_lo = phys_alloc();          /* block USER_LO_BLK: kernel + user */
    uint64_t l3_hi = phys_alloc();          /* block USER_HI_BLK: user + heap/stacks */

    if (!root || !l3_lo || !l3_hi) {
        phys_free(root); phys_free(l3_lo); phys_free(l3_hi);
        return 0;
    }

    uint64_t *r = tbl(root);

    /* The two straddle blocks become level-3 tables: the kernel identity part is
     * filled in, the user part is left invalid for demand paging. */
    l3_identity(l3_lo, 0, VM_USER_LO);              /* [0, 4 MiB): kernel image */
    l3_identity(l3_hi, VM_KHEAP_LO, VM_RAM_SIZE);   /* [240, 256 MiB): heap + stacks */
    r[USER_LO_BLK] = l3_lo | D_TABLE;
    r[USER_HI_BLK] = l3_hi | D_TABLE;

    /* The user-window blocks fully inside the window get their level-3 tables on
     * demand (walk_user); their root entries stay invalid here. The blocks
     * between RAM top and the physmap are Device so stray MMIO still traps. */
    for (uint64_t b = RAM_BLOCKS; b < PHYSMAP_BLK; b++)
        r[b] = (b << L2_SHIFT) | BLOCK_DEVICE;

    /* physmap: all of RAM as 32 MiB blocks sitting directly in the root table, so
     * the kernel reaches any frame by physical address regardless of TTBR0. No
     * second-level table means nothing here can go stale for the walker. */
    for (uint64_t i = 0; i < RAM_BLOCKS; i++)
        r[PHYSMAP_BLK + i] = (i << L2_SHIFT) | BLOCK_PHYS;

    /* The demand-grown heap window: a run of shared level-3 tables. */
    for (int k = 0; k < kheap_nl3; k++)
        r[KHEAP_BLK + k] = kheap_l3[k] | D_TABLE;

    tbl_clean(root);
    tbl_clean(l3_lo);
    tbl_clean(l3_hi);
    /* Carry this space's ASID in the handle, so every vmspace_switch to it uses
     * the same ASID and its not-global user entries survive the scheduler's TTBR0
     * reloads. tbl() masks the ASID off to recover the table address. */
    return root | (asid_alloc() << 48);
}

/* Walk to the L3 entry for a user-window vaddr, allocating an L3 table if
 * needed. Returns a pointer to the entry, or NULL on allocation failure. The two
 * straddle blocks already have their L3 tables (built in vmspace_new); the
 * blocks fully inside the window get theirs here on first touch. */
static uint64_t *
walk_user(vmspace_t as, uint64_t vaddr, int create)
{
    uint64_t *r = tbl(as);
    uint64_t bi = L2I(vaddr);
    uint64_t e = r[bi];

    if (e && (e & 0x3) == D_BLOCK)
        return NULL;                        /* a 32 MiB block sits here */
    if (!(e & 1)) {
        if (!create)
            return NULL;
        uint64_t l3 = phys_alloc();
        if (!l3)
            return NULL;
        r[bi] = l3 | D_TABLE;
        pte_sync(&r[bi]);                   /* publish the new L3 table to the walker */
        e = r[bi];
    }
    uint64_t *l3 = tbl(pte_frame(e));
    return &l3[L3I(vaddr)];
}

int
vmspace_map(vmspace_t as, uint64_t vaddr, uint64_t frame, uint64_t flags)
{
    (void)flags;                            /* every user page is RW at EL1 */
    if (vaddr < VM_USER_LO || vaddr >= VM_USER_HI)
        return -1;
    uint64_t *e = walk_user(as, vaddr, 1);
    if (!e)
        return -1;
    *e = (frame & D_ADDR) | PAGE_USER;
    pte_sync(e);                            /* publish the mapping to the walker */
    return 0;
}

uint64_t
vmspace_lookup(vmspace_t as, uint64_t vaddr)
{
    if (vaddr < VM_USER_LO || vaddr >= VM_USER_HI)
        return 0;
    uint64_t *e = walk_user(as, vaddr, 0);
    if (!e || !(*e & 1))
        return 0;
    return pte_frame(*e);
}

/* Turn the MMU on with as in TTBR0. The code executing is identity-mapped, so it
 * keeps running across the enable. */
static void
mmu_enable(vmspace_t as)
{
    uint64_t sctlr;
    __asm__ volatile ("msr mair_el1, %0" : : "r"(MAIR_VALUE));
    __asm__ volatile ("msr tcr_el1, %0"  : : "r"(TCR_VALUE));
    __asm__ volatile ("msr ttbr0_el1, %0" : : "r"(as));
    __asm__ volatile ("dsb ish; isb");
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    /* M: enable the MMU. C and I: enable the data and instruction caches. The
     * caches are not optional here: Apple Silicon only implements the exclusive
     * monitor (ldaxr/stxr, which back every atomic) for cacheable Normal memory,
     * so with the data cache off the first heap lock faults with an "unsupported
     * atomic" data abort. The boot and process maps mark RAM Normal write-back. */
    sctlr |= (1ULL << 0) | (1ULL << 2) | (1ULL << 12);
    __asm__ volatile ("msr sctlr_el1, %0; isb" : : "r"(sctlr));
}

/* Boot-time translation tables: a level-2 (root) table built like a process map
 * but with no user window. The two straddle blocks are level-3 tables carrying
 * only the kernel identity (the image low down, the heap band and stacks up
 * high); the blocks that would be the user window are left unmapped, the band up
 * to the physmap is Device, and the physmap sits in the root. Crucially the boot
 * map creates no global mapping of the user-window VAs: a global boot entry
 * there would shadow a process's not-global user pages after the switch (a global
 * TLB entry matches every ASID), which is exactly the kind of stale translation
 * the ASID scheme exists to avoid. The guest boots with the MMU off, where the
 * exclusive instructions behind the atomics fault, so the MMU must come up before
 * the first heap lock. The tables live in the kernel image (identity-mapped by
 * themselves), so no allocator is needed. */
static uint64_t boot_root[TBL_ENTRIES] __attribute__((aligned(0x4000)));
static uint64_t boot_l3_lo[TBL_ENTRIES] __attribute__((aligned(0x4000)));
static uint64_t boot_l3_hi[TBL_ENTRIES] __attribute__((aligned(0x4000)));

void
arch_mmu_init(void)
{
    /* Every cpu runs this; they all write the same descriptors (a benign race)
     * and then enable translation on themselves. The boot map carries the
     * physmap, and the physmap is marked live from here on, so the kernel reaches
     * every frame through one virtual address from the very start. That keeps the
     * frame pool coherent: the free list is written and read through one virtual
     * address rather than two aliases the caches would not keep in step. */
    for (uint64_t va = 0; va < VM_USER_LO; va += VM_PAGE)
        boot_l3_lo[L3I(va)] = (va & D_ADDR) | PAGE_NORMAL;      /* kernel image */
    for (uint64_t va = VM_KHEAP_LO; va < VM_RAM_SIZE; va += VM_PAGE)
        boot_l3_hi[L3I(va)] = (va & D_ADDR) | PAGE_NORMAL;      /* heap + stacks */
    boot_root[USER_LO_BLK] = (uint64_t)(uintptr_t)boot_l3_lo | D_TABLE;
    boot_root[USER_HI_BLK] = (uint64_t)(uintptr_t)boot_l3_hi | D_TABLE;
    for (uint64_t b = RAM_BLOCKS; b < PHYSMAP_BLK; b++)
        boot_root[b] = (b << L2_SHIFT) | BLOCK_DEVICE;
    for (uint64_t i = 0; i < RAM_BLOCKS; i++)
        boot_root[PHYSMAP_BLK + i] = (i << L2_SHIFT) | BLOCK_PHYS;
    __asm__ volatile ("dsb ish");
    mmu_enable((vmspace_t)(uintptr_t)boot_root);
    physmap_on = 1;                     /* the physmap is live from boot */
}

void
vmspace_switch(vmspace_t as)
{
    /* The MMU is already on (arch_mmu_init at boot), so this just repoints TTBR0
     * at the process map. The handle already carries the space's stable ASID in
     * bits[55:48], so no TLB flush is needed: the not-global user entries are
     * tagged by ASID and the space being left cannot be confused with this one.
     * cur_vmspace keeps the whole handle (ASID included); tbl() masks it off for
     * the software table walks. */
    __asm__ volatile ("msr ttbr0_el1, %0" : : "r"(as));
    __asm__ volatile ("isb");
    kheap_live = 1;                     /* this map carries the kheap window */
    cur_vmspace = as;
}

/* Map one zeroed pool frame at window VA va (page aligned, >= VM_KHEAP_VA),
 * into the shared heap-window level-3 tables. Returns 0, or -1 if the pool is
 * empty or the window is exhausted. The caller holds kheap_lock. */
static int
kheap_map_page(uint64_t va)
{
    uint64_t bi = L2I(va);
    if (bi < (uint64_t)KHEAP_BLK || bi >= (uint64_t)(KHEAP_BLK + kheap_nl3))
        return -1;                      /* past the end of the window */
    uint64_t *l3 = tbl(kheap_l3[bi - KHEAP_BLK]);
    uint64_t f = phys_alloc();
    if (!f)
        return -1;
    l3[L3I(va)] = (f & D_ADDR) | PAGE_NORMAL;
    pte_sync(&l3[L3I(va)]);
    return 0;
}

/* The allocator's grower: map at least need bytes of fresh pool frames into the
 * window, contiguous with what is already mapped, and hand the span to the
 * allocator. Returns the bytes added, or 0 when it cannot satisfy need (the
 * window is unreachable until the MMU is on, or the pool is empty). */
static size_t
kheap_grow(size_t need)
{
    if (!kheap_live)            /* the window is unmapped until the first process switch */
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
        __asm__ volatile ("dsb ish; isb");  /* publish the new entries to the table walker */

    __atomic_store_n(&kheap_lock, 0, __ATOMIC_RELEASE);
    arch_irq_restore(irq);

    if (done == 0)
        return 0;
    memadd((void *)(uintptr_t)base, done);
    return done < need ? 0 : done;      /* mapped, but short of the request */
}

void
kheap_init(void)
{
    for (int k = 0; k < KHEAP_NL3; k++) {
        uint64_t l3 = phys_alloc();
        if (!l3)
            break;                      /* short window; the bootstrap band stands */
        kheap_l3[k] = l3;
        kheap_nl3++;
    }
    if (!kheap_nl3)
        return;
    kheap_top = VM_KHEAP_VA;
    mem_set_grower(kheap_grow);

    /* The window's tables are installed in every process by vmspace_new. The
     * guest boots with the MMU off and no translation tables to patch, so the
     * window comes alive at the first switch; the grower stays idle until then. */
}

int
pf_handle(uint64_t far, uint64_t esr)
{
    uint32_t dfsc = (uint32_t)(esr & 0x3f);

    /* Only a translation fault (page not mapped) in the running process's user
     * window is ours. DFSC 0b0001LL is a translation fault at level LL. */
    if (!cur_vmspace || dfsc < 0x04 || dfsc > 0x07)
        return 0;
    if (far < VM_USER_LO || far >= VM_USER_HI)
        return 0;
    if (far >= pf_guard_lo && far < pf_guard_hi)  /* stack guard: a fault here is overflow */
        return 0;

    uint64_t f = phys_alloc();
    if (!f)
        return 0;
    if (vmspace_map(cur_vmspace, far & ~(VM_PAGE - 1), f, 0) != 0) {
        phys_free(f);
        return 0;
    }
    __asm__ volatile ("dsb ishst; tlbi vmalle1is; dsb ish; isb");
    return 1;
}

int
vmspace_unmap(vmspace_t as, uint64_t vaddr)
{
    if (vaddr < VM_USER_LO || vaddr >= VM_USER_HI)
        return 0;
    uint64_t *e = walk_user(as, vaddr, 0);
    if (!e || !(*e & 1))
        return 0;
    phys_free(pte_frame(*e));
    *e = 0;
    pte_sync(e);                            /* publish the clear to the walker */
    if (as == cur_vmspace)
        __asm__ volatile ("dsb ishst; tlbi vmalle1is; dsb ish; isb");
    return 1;
}

/* Visit every present user-window L3 entry, calling fn(vaddr, entry-ptr, arg).
 * Only VAs inside the user window are visited, so the kernel identity pages that
 * share the two straddle blocks' L3 tables are left untouched. */
static void
user_walk(vmspace_t as, void (*fn)(uint64_t, uint64_t *, void *), void *arg)
{
    uint64_t *r = tbl(as);

    for (uint64_t bi = USER_LO_BLK; bi <= USER_HI_BLK; bi++) {
        uint64_t e = r[bi];
        if (!(e & 1) || (e & 0x3) == D_BLOCK)
            continue;
        uint64_t *l3 = tbl(pte_frame(e));
        for (uint64_t i = 0; i < TBL_ENTRIES; i++) {
            if (!(l3[i] & 1))
                continue;
            uint64_t va = (bi << L2_SHIFT) | (i << L3_SHIFT);
            if (va < VM_USER_LO || va >= VM_USER_HI)
                continue;               /* kernel identity page in a straddle block */
            fn(va, &l3[i], arg);
        }
    }
}

static void
free_one(uint64_t va, uint64_t *e, void *arg)
{
    (void)va;
    (void)arg;
    phys_free(pte_frame(*e));
    *e = 0;
}

void
vmspace_destroy(vmspace_t as)
{
    if (!as)
        return;
    user_walk(as, free_one, NULL);

    /* Free the user-window level-3 tables (the two straddle blocks always have
     * one; the interior blocks have one only if they were ever touched), then the
     * root table. The physmap and device blocks are leaf descriptors that own no
     * per-process frames, and the heap-window tables are shared. */
    uint64_t *r = tbl(as);
    for (uint64_t bi = USER_LO_BLK; bi <= USER_HI_BLK; bi++) {
        if ((r[bi] & 1) && (r[bi] & 0x3) == D_TABLE)
            phys_free(pte_frame(r[bi]));
    }
    asid_free((as >> 48) & 0xFF);
    phys_free(as & D_ADDR);                  /* the root table (handle minus ASID) */
}

struct clone_ctx {
    vmspace_t dst;
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
    memcpy(dstp, phys_to_virt(pte_frame(*e)), VM_PAGE);
    /* A copied page may hold code the child will execute. The instruction cache
     * is not coherent with the data writes of the memcpy, so clean the fresh page
     * to the Point of Unification and invalidate the instruction cache for it.
     * Both operate on the page's physical address (the instruction cache is
     * physically tagged), so invalidating by this physmap address also covers the
     * child's fetch through its own user mapping, and the parent's instruction
     * cache is left untouched. */
    for (uint64_t off = 0; off < VM_PAGE; off += 64)
        __asm__ volatile ("dc cvau, %0" : : "r"(dstp + off) : "memory");
    __asm__ volatile ("dsb ish" ::: "memory");
    for (uint64_t off = 0; off < VM_PAGE; off += 64)
        __asm__ volatile ("ic ivau, %0" : : "r"(dstp + off) : "memory");
    if (vmspace_map(c->dst, va, nf, 0) != 0)
        c->failed = 1;
}

vmspace_t
vmspace_clone(vmspace_t src)
{
    struct clone_ctx c = { vmspace_new(), 0 };

    if (!c.dst)
        return 0;
    user_walk(src, clone_one, &c);
    if (c.failed) {
        vmspace_destroy(c.dst);
        return 0;
    }
    /* clone_one already cleaned and invalidated each copied page's caches by
     * physical address; order those maintenance ops before the child runs. */
    __asm__ volatile ("dsb ish; isb" ::: "memory");
    return c.dst;
}
