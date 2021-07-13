#ifndef CHAL_PGTBL_H
#define CHAL_PGTBL_H

/* These code below are for x86 specifically, only used in x86 chal */
typedef enum {
	X86_PGTBL_PRESENT    = 1,
	X86_PGTBL_WRITABLE   = 1 << 1,
	X86_PGTBL_USER       = 1 << 2,
	X86_PGTBL_WT         = 1 << 3, /* write-through caching */
	X86_PGTBL_NOCACHE    = 1 << 4, /* caching disabled */
	X86_PGTBL_ACCESSED   = 1 << 5,
	X86_PGTBL_MODIFIED   = 1 << 6,
	X86_PGTBL_SUPER      = 1 << 7, /* super-page (4MB on x86-32) */
	X86_PGTBL_GLOBAL     = 1 << 8,
	/* Composite defined bits next*/
	X86_PGTBL_COSFRAME   = 1 << 9,
	X86_PGTBL_COSKMEM    = 1 << 10, /* page activated as kernel object */
	X86_PGTBL_QUIESCENCE = 1 << 11,
	/* Flag bits done. */

	X86_PGTBL_USER_DEF   = X86_PGTBL_PRESENT | X86_PGTBL_USER | X86_PGTBL_ACCESSED | X86_PGTBL_MODIFIED | X86_PGTBL_WRITABLE,
	X86_PGTBL_INTERN_DEF = X86_PGTBL_USER_DEF,
} pgtbl_flags_x86_t;

/**
 * Use the passed in page, but make sure that we only use the passed
 * in page once.
 */
static inline void *
__pgtbl_a(void *d, int sz, int leaf)
{
	void **i = d, *p;

	(void)leaf;
	assert(sz == PAGE_SIZE);
	if (unlikely(!*i)) return NULL;
	p  = *i;
	*i = NULL;
	return p;
}
static struct ert_intern *
__pgtbl_get(struct ert_intern *a, void *accum, int isleaf)
{
	(void)isleaf;
	/* don't use | here as we only want the pte flags */
	*(u32_t *)accum = (((u32_t)(unsigned long)a->next) & PGTBL_FLAG_MASK);
	return chal_pa2va((paddr_t)((((u32_t)(unsigned long)a->next) & PGTBL_FRAME_MASK)));
}
static int
__pgtbl_isnull(struct ert_intern *a, void *accum, int isleaf)
{
	(void)isleaf;
	(void)accum;
	return !(((u32_t)(unsigned long)(a->next)) & (X86_PGTBL_PRESENT | X86_PGTBL_COSFRAME));
}
static int
__pgtbl_resolve(struct ert_intern *a, void *accum, int leaf, u32_t order, u32_t sz)
{
	(void)a;
	(void)leaf;
	(void)order;
	(void)sz;
	*(u32_t *)accum = (((u32_t)(unsigned long)a->next) & PGTBL_FLAG_MASK);
	return 1;
}
static void
__pgtbl_init(struct ert_intern *a, int isleaf)
{
	(void)isleaf;
	a->next = NULL;
}

/**
 * We only need to do mapping_add at boot time to add all physical
 * memory to the pgtbl of llboot. After that, we only need to do copy
 * from llboot pgtbl to other pgtbls. Thus, when adding to pgtbl, we
 * use physical addresses; when doing copy, we don't need to worry
 * about PA.
 */

/* v should include the desired flags */
static inline int
__pgtbl_setleaf(struct ert_intern *a, void *v)
{
	unsigned long new, old;
	old = (unsigned long)(a->next);
	new = (unsigned long)(v);

	if (!cos_cas((unsigned long *)a, old, new)) return -ECASFAIL;

	return 0;
}

/**
 * This takes an input parameter as the old value of the mapping. Only
 * update when the existing value matches.
 */
static inline int
__pgtbl_update_leaf(struct ert_intern *a, void *v, u64_t old)
{
	u64_t new;

	new = (u64_t)(v);
	if (!cos_cas((unsigned long *)a, old, new)) return -ECASFAIL;

	return 0;
}

/* Note:  We're just using pre-defined default flags for internal (pgd) entries */
static int
__pgtbl_set(struct ert_intern *a, void *v, void *accum, int isleaf)
{
	unsigned long old, new;
	(void)accum;
	assert(!isleaf);

	old = (unsigned long)a->next;
	new = (unsigned long)chal_va2pa((void *)((unsigned long)v & PGTBL_FRAME_MASK)) | X86_PGTBL_INTERN_DEF;

	if (!cos_cas((unsigned long *)&a->next, old, new)) return -ECASFAIL;

	return 0;
}

static inline void *
__pgtbl_getleaf(struct ert_intern *a, void *accum)
{
	if (unlikely(!a)) return NULL;
	return __pgtbl_get(a, accum, 1);
}

ERT_CREATE(__pgtbl, pgtbl, PGTBL_DEPTH, PGTBL_ENTRY_ORDER, sizeof(int *), PGTBL_ENTRY_ORDER, sizeof(int *), NULL, __pgtbl_init,
           __pgtbl_get, __pgtbl_isnull, __pgtbl_set, __pgtbl_a, __pgtbl_setleaf, __pgtbl_getleaf, __pgtbl_resolve);

static pgtbl_t
pgtbl_alloc(void *page)
{
	return (pgtbl_t)((unsigned long)__pgtbl_alloc(&page) & PGTBL_FRAME_MASK);
}

static int
pgtbl_intern_expand(pgtbl_t pt, u64_t addr, void *pte, u32_t flags)
{
	unsigned long accum = (unsigned long)flags;
	int           ret;

	/* NOTE: flags currently ignored. */

	assert(pt);
	assert((PGTBL_FLAG_MASK & (u64_t)pte) == 0);
	assert((PGTBL_FRAME_MASK & flags) == 0);

	if (!pte) return -EINVAL;
	ret = __pgtbl_expandn(pt, (unsigned long)(addr >> PGTBL_PAGEIDX_SHIFT), PGTBL_DEPTH, &accum, &pte, NULL);
	if (!ret && pte) return -EEXIST; /* no need to expand */
	assert(!(ret && !pte));          /* error and used memory??? */

	return ret;
}

/**
 * FIXME: If these need to return a physical address, we should do a
 * va2pa before returning
 */
static void *
pgtbl_intern_prune(pgtbl_t pt, u64_t addr)
{
	unsigned long accum = 0, *pgd;
	void *        page;

	assert(pt);
	assert((PGTBL_FLAG_MASK & (u64_t)addr) == 0);

	pgd = __pgtbl_lkupan((pgtbl_t)((u64_t)pt | X86_PGTBL_PRESENT), (u64_t)addr >> PGTBL_PAGEIDX_SHIFT, 1, &accum);
	if (!pgd) return NULL;
	page  = __pgtbl_get((struct ert_intern *)pgd, &accum, 0);
	accum = 0;

	if (__pgtbl_set((struct ert_intern *)pgd, NULL, &accum, 0)) return NULL;

	return page;
}

/* FIXME:  these pgd functions should be replaced with lookup_lvl functions (see below). Consider deleting this because we have lkup now */
static void *
pgtbl_get_pgd(pgtbl_t pt, u64_t addr)
{
	unsigned long accum = 0;

	assert(pt);
	return __pgtbl_lkupan((pgtbl_t)((u64_t)pt | X86_PGTBL_PRESENT), (u64_t)addr >> PGTBL_PAGEIDX_SHIFT, 1, &accum);
}

static int
pgtbl_check_pgd_absent(pgtbl_t pt, u32_t addr)
{
	return __pgtbl_isnull(pgtbl_get_pgd(pt, (u32_t)addr), 0, 0);
}
#endif /* CHAL_PGTBL_H */

