/* arm64_vmspace.c : per-process address spaces on AArch64. See arm64_vmspace.h.
 *
 * Mirrors the x86 backend: a free-list frame allocator, a per-process top-level
 * table that identity-maps the kernel and MMIO and keeps a private user window,
 * and a physmap so the kernel can reach any frame by physical address. The
 * difference is the ARMv8 translation-table format and that the first switch
 * turns the MMU on (the guest boots with it off). */
#include "arm64_vmspace.h"

#include "string.h"

/* ARMv8 descriptor bits (4 KiB granule). */
#define D_BLOCK      0x1ULL                 /* L2 2 MiB block (bits[1:0]=0b01) */
#define D_TABLE      0x3ULL                 /* table, or L3 page (bits[1:0]=0b11) */
#define D_AF         (1ULL << 10)           /* access flag; 0 would fault */
#define D_SH_INNER   (3ULL << 8)            /* inner shareable */
#define D_ATTR_NORM  (0ULL << 2)            /* MAIR index 0: Normal WB */
#define D_ATTR_DEV   (1ULL << 2)            /* MAIR index 1: Device-nGnRE */
#define D_ADDR       0x0000FFFFFFFFF000ULL  /* output address field */

#define BLOCK_NORMAL (D_BLOCK | D_AF | D_SH_INNER | D_ATTR_NORM)
#define BLOCK_DEVICE (D_BLOCK | D_AF | D_ATTR_DEV)
#define PAGE_NORMAL  (D_TABLE | D_AF | D_SH_INNER | D_ATTR_NORM)

/* MAIR: attr0 Normal write-back RW-allocate, attr1 Device-nGnRE. */
#define MAIR_VALUE   0x00000000000004FFULL

/* TCR: 39-bit VA (T0SZ=25), 4 KiB TG0, walks inner-shareable write-back, TTBR1
 * walks disabled (EPD1), 40-bit intermediate PA. */
#define TCR_VALUE    (25ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) \
                      | (0ULL << 14) | (1ULL << 23) | (2ULL << 32))

/* Physical frame allocator: a free list threaded through the free frames. */
static uint64_t free_head;
static int      physmap_on;                 /* phys_to_virt uses the physmap once set */

/* The address space the fault handler maps into (the running process). */
vmspace_t cur_vmspace;

void *
phys_to_virt(uint64_t p)
{
    return (void *)(uintptr_t)(physmap_on ? (VM_PHYSMAP + p) : p);
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

static uint64_t *
tbl(uint64_t frame)
{
    return (uint64_t *)phys_to_virt(frame & ~0xFFFULL);
}

static uint64_t
pte_frame(uint64_t e)
{
    return e & D_ADDR;
}

vmspace_t
vmspace_new(void)
{
    uint64_t l1 = phys_alloc();
    uint64_t l2_low = phys_alloc();
    uint64_t l2_phys = phys_alloc();

    if (!l1 || !l2_low || !l2_phys) {
        phys_free(l1); phys_free(l2_low); phys_free(l2_phys);
        return 0;
    }

    tbl(l1)[0] = l2_low | D_TABLE;          /* [0, 1 GiB) */
    tbl(l1)[1] = l2_phys | D_TABLE;         /* [1 GiB, 2 GiB): physmap */

    /* Low gigabyte: 2 MiB blocks everywhere outside the user window. RAM and the
     * low kernel are Normal; the device band (UART and doorbells at and above
     * 0x08000000) is Device so stores still trap to the host. The user window
     * is left invalid, to be filled with L3 pages on demand. */
    uint64_t *l2 = tbl(l2_low);
    for (uint64_t i = 0; i < 512; i++) {
        uint64_t base = i << 21;
        if (base >= VM_USER_LO && base < VM_USER_HI)
            continue;
        uint64_t attr = (base >= 0x08000000ULL) ? BLOCK_DEVICE : BLOCK_NORMAL;
        l2[i] = base | attr;
    }

    /* physmap: the first 32 MiB of RAM as sixteen 2 MiB blocks at VM_PHYSMAP. */
    uint64_t *lp = tbl(l2_phys);
    for (uint64_t i = 0; i < 16; i++)
        lp[i] = (i << 21) | BLOCK_NORMAL;

    return l1;
}

/* Walk to the L3 entry for a user-window vaddr, allocating an L3 table if
 * needed. Returns a pointer to the entry, or NULL on allocation failure. */
static uint64_t *
walk_user(vmspace_t as, uint64_t vaddr, int create)
{
    uint64_t *l2 = tbl(pte_frame(tbl(as)[0]));
    uint64_t l2i = (vaddr >> 21) & 0x1FF;
    uint64_t e = l2[l2i];

    if (e && (e & 0x3) == D_BLOCK)
        return NULL;                        /* a 2 MiB block sits here */
    if (!(e & 1)) {
        if (!create)
            return NULL;
        uint64_t l3 = phys_alloc();
        if (!l3)
            return NULL;
        l2[l2i] = l3 | D_TABLE;
        e = l2[l2i];
    }
    uint64_t *l3 = tbl(pte_frame(e));
    return &l3[(vaddr >> 12) & 0x1FF];
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
    *e = (frame & D_ADDR) | PAGE_NORMAL;
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

/* Turn the MMU on with as in TTBR0. Called once, on the first switch; the code
 * executing is identity-mapped, so it keeps running across the enable. */
static void
mmu_enable(vmspace_t as)
{
    uint64_t sctlr;
    __asm__ volatile ("msr mair_el1, %0" : : "r"(MAIR_VALUE));
    __asm__ volatile ("msr tcr_el1, %0"  : : "r"(TCR_VALUE));
    __asm__ volatile ("msr ttbr0_el1, %0" : : "r"(as));
    __asm__ volatile ("dsb ish; isb");
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 1ULL;                          /* M: enable the MMU (caches left off) */
    __asm__ volatile ("msr sctlr_el1, %0; isb" : : "r"(sctlr));
}

void
vmspace_switch(vmspace_t as)
{
    if (!physmap_on) {
        mmu_enable(as);
        physmap_on = 1;
    } else {
        __asm__ volatile ("msr ttbr0_el1, %0" : : "r"(as));
        __asm__ volatile ("dsb ish; tlbi vmalle1; dsb ish; isb");
    }
    cur_vmspace = as;
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

    uint64_t f = phys_alloc();
    if (!f)
        return 0;
    if (vmspace_map(cur_vmspace, far & ~0xFFFULL, f, 0) != 0) {
        phys_free(f);
        return 0;
    }
    __asm__ volatile ("dsb ishst; tlbi vmalle1; dsb ish; isb");
    return 1;
}

/* Visit every present user-window L3 entry, calling fn(vaddr, entry-ptr, arg). */
static void
user_walk(vmspace_t as, void (*fn)(uint64_t, uint64_t *, void *), void *arg)
{
    uint64_t *l2 = tbl(pte_frame(tbl(as)[0]));

    for (uint64_t l2i = VM_USER_LO >> 21; l2i < (VM_USER_HI >> 21); l2i++) {
        uint64_t e = l2[l2i];
        if (!(e & 1) || (e & 0x3) == D_BLOCK)
            continue;
        uint64_t *l3 = tbl(pte_frame(e));
        for (uint64_t i = 0; i < 512; i++) {
            if (l3[i] & 1) {
                uint64_t va = (l2i << 21) | (i << 12);
                fn(va, &l3[i], arg);
            }
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

    /* Free the user-window L3 tables, then the top-level tables. The shared 2
     * MiB block mappings own no per-process frames. */
    uint64_t *l1 = tbl(as);
    uint64_t l2_low = pte_frame(l1[0]);
    uint64_t *l2 = tbl(l2_low);
    for (uint64_t l2i = VM_USER_LO >> 21; l2i < (VM_USER_HI >> 21); l2i++) {
        if ((l2[l2i] & 1) && (l2[l2i] & 0x3) == D_TABLE)
            phys_free(pte_frame(l2[l2i]));
    }
    phys_free(pte_frame(l1[1]));            /* l2_phys */
    phys_free(l2_low);
    phys_free(as);                          /* l1 */
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
    memcpy(phys_to_virt(nf), phys_to_virt(pte_frame(*e)), VM_PAGE);
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
    return c.dst;
}
