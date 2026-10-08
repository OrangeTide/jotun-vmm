/* x86_64_vmspace.c : per-process address spaces. See x86_64_vmspace.h. */
#include "x86_64_vmspace.h"

#include "string.h"

/* Physical frame allocator: a free list whose link lives in each free frame,
 * reached through phys_to_virt. */
static uint64_t free_head;              /* 0 means empty */
static int      physmap_on;             /* phys_to_virt uses the physmap once set */

/* The address space the page-fault handler maps into (the running process). */
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
    /* Thread high-to-low so low frames are handed out first (tidier dumps). */
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

/* Table-entry accessors over physmap. Each table is 512 uint64_t. */
static uint64_t *
tbl(uint64_t frame)
{
    return (uint64_t *)phys_to_virt(frame & ~0xFFFULL);
}

static uint64_t
pte_frame(uint64_t e)
{
    return e & 0x000FFFFFFFFFF000ULL;
}

vmspace_t
vmspace_new(void)
{
    uint64_t pml4 = phys_alloc();
    uint64_t pdpt = phys_alloc();
    uint64_t pd_low = phys_alloc();
    uint64_t pd_phys = phys_alloc();

    if (!pml4 || !pdpt || !pd_low || !pd_phys) {
        phys_free(pml4); phys_free(pdpt);
        phys_free(pd_low); phys_free(pd_phys);
        return 0;
    }

    tbl(pml4)[0] = pdpt | VM_P | VM_W | VM_U;
    tbl(pdpt)[0] = pd_low | VM_P | VM_W | VM_U;     /* [0, 1 GiB) */
    tbl(pdpt)[1] = pd_phys | VM_P | VM_W;           /* [1 GiB, 2 GiB): physmap */

    /* Everything in [0, 1 GiB) outside the user window is shared identity as
     * 2 MiB pages: the kernel, page tables, vcpu stacks, and the device MMIO
     * band (the 9P and SMP doorbells near 0x0a000000). Only the user window
     * [0x400000, 0x1C00000) is left unmapped, to be demand-paged per process. */
    uint64_t *pdl = tbl(pd_low);
    for (uint64_t i = 0; i < 512; i++) {
        uint64_t base = i << 21;
        if (base < VM_USER_LO || base >= VM_USER_HI)
            pdl[i] = base | VM_P | VM_W | VM_PS;
    }

    /* physmap: the first 32 MiB of RAM as sixteen 2 MiB pages at VM_PHYSMAP. */
    uint64_t *pdp = tbl(pd_phys);
    for (uint64_t i = 0; i < 16; i++)
        pdp[i] = (i << 21) | VM_P | VM_W | VM_PS;

    return pml4;
}

/* Walk to the PT entry for a user-window vaddr, allocating a PT if needed.
 * Returns a pointer to the entry, or NULL on allocation failure. */
static uint64_t *
walk_user(vmspace_t as, uint64_t vaddr, int create)
{
    uint64_t *pdpt = tbl(pte_frame(tbl(as)[0]));
    uint64_t *pd = tbl(pte_frame(pdpt[0]));
    uint64_t pdi = (vaddr >> 21) & 0x1FF;
    uint64_t e = pd[pdi];

    if (e & VM_PS)
        return NULL;                    /* a 2 MiB mapping sits here */
    if (!(e & VM_P)) {
        if (!create)
            return NULL;
        uint64_t pt = phys_alloc();
        if (!pt)
            return NULL;
        pd[pdi] = pt | VM_P | VM_W | VM_U;
        e = pd[pdi];
    }
    uint64_t *pt = tbl(pte_frame(e));
    return &pt[(vaddr >> 12) & 0x1FF];
}

int
vmspace_map(vmspace_t as, uint64_t vaddr, uint64_t frame, uint64_t flags)
{
    if (vaddr < VM_USER_LO || vaddr >= VM_USER_HI)
        return -1;
    uint64_t *e = walk_user(as, vaddr, 1);
    if (!e)
        return -1;
    *e = (frame & ~0xFFFULL) | (flags & 0xFFFULL) | VM_P;
    return 0;
}

uint64_t
vmspace_lookup(vmspace_t as, uint64_t vaddr)
{
    if (vaddr < VM_USER_LO || vaddr >= VM_USER_HI)
        return 0;
    uint64_t *e = walk_user(as, vaddr, 0);
    if (!e || !(*e & VM_P))
        return 0;
    return pte_frame(*e);
}

void
vmspace_switch(vmspace_t as)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(as) : "memory");
    physmap_on = 1;
    cur_vmspace = as;
}

/* The page-fault handler's C half (called from pf_isr). A not-present fault in
 * the running process's user window is demand-paged: allocate a zeroed frame,
 * map it, and report success so the instruction retries. Anything else is
 * fatal and reported by the caller. */
int
pf_handle(uint64_t cr2, uint64_t err)
{
    if (!cur_vmspace || (err & VM_P))           /* protection fault: not ours */
        return 0;
    if (cr2 < VM_USER_LO || cr2 >= VM_USER_HI)
        return 0;

    uint64_t f = phys_alloc();
    if (!f)
        return 0;
    if (vmspace_map(cur_vmspace, cr2 & ~0xFFFULL, f, VM_W | VM_U) != 0) {
        phys_free(f);
        return 0;
    }
    __asm__ volatile ("invlpg (%0)" : : "r"(cr2) : "memory");
    return 1;
}

/* Visit every present user-window PT entry, calling fn(vaddr, entry-ptr). */
static void
user_walk(vmspace_t as, void (*fn)(uint64_t, uint64_t *, void *), void *arg)
{
    uint64_t *pdpt = tbl(pte_frame(tbl(as)[0]));
    uint64_t *pd = tbl(pte_frame(pdpt[0]));

    for (uint64_t pdi = VM_USER_LO >> 21; pdi < (VM_USER_HI >> 21); pdi++) {
        uint64_t e = pd[pdi];
        if (!(e & VM_P) || (e & VM_PS))
            continue;
        uint64_t *pt = tbl(pte_frame(e));
        for (uint64_t pti = 0; pti < 512; pti++) {
            if (pt[pti] & VM_P) {
                uint64_t va = (pdi << 21) | (pti << 12);
                fn(va, &pt[pti], arg);
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

    /* Free the user-window page tables, then the top-level tables. The shared
     * kernel 2 MiB mappings own no per-process frames. */
    uint64_t *pdpt = tbl(pte_frame(tbl(as)[0]));
    uint64_t pd_lo = pte_frame(pdpt[0]);
    uint64_t *pd = tbl(pd_lo);
    for (uint64_t pdi = VM_USER_LO >> 21; pdi < (VM_USER_HI >> 21); pdi++) {
        if ((pd[pdi] & VM_P) && !(pd[pdi] & VM_PS))
            phys_free(pte_frame(pd[pdi]));
    }
    phys_free(pte_frame(pdpt[1]));      /* pd_phys */
    phys_free(pd_lo);
    phys_free(pte_frame(tbl(as)[0]));   /* pdpt */
    phys_free(as);                      /* pml4 */
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
    if (vmspace_map(c->dst, va, nf, *e & 0xFFFULL) != 0)
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
