/**
 * Copyright 2014 by Gabriel Parmer, gparmer@gwu.edu
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 *
 * This is a capability table implementation based on embedded radix
 * trees.
 */

#ifndef CAPTBL_H
#define CAPTBL_H

#include "shared/cos_errno.h"
#include "shared/cos_types.h"
#include "ertrie.h"
#include "liveness_tbl.h"
#include "shared/util.h"

#ifndef CACHELINE_SIZE
#define CACHELINE_SIZE 64
#define CACHELINE_ORDER 6
#endif

#define CAPTBL_DEPTH 2
#define CAPTBL_INTERNSZ (sizeof(int *))
#if defined(__x86_64__)
	#define CAPTBL_INTERN_ORD 8 /* log(PAGE_SIZE/(2*(CAPTBL_DEPTH-1)*CAPTBL_INTERNSZ)) */
#else
	#define CAPTBL_INTERN_ORD 9
#endif
#define CAPTBL_LEAFSZ (sizeof(struct cap_min))
#define CAPTBL_LEAF_ORD 7 /* log(PAGE_SIZE/(2*CAPTBL_LEAFSZ)) */

#ifdef CAP_FREE
#undef CAP_FREE
#endif

#ifndef EFAULT
#define EFAULT 14
#endif

static inline unsigned long
__captbl_cap2bytes(cap_t c)
{
	return 1 << (__captbl_cap2sz(c) + CAP_SZ_OFF);
}

typedef enum {
	CAP_FLAG_RO    = 1,
	CAP_FLAG_LOCAL = 1 << 1,
	CAP_FLAG_RCU   = 1 << 2,
} cap_flags_t;

#define CAP_HEAD_AMAP_SZ 4
#define CAP_HEAD_SZ_SZ 2
#define CAP_HEAD_FLAGS_SZ 3
#define CAP_HEAD_TYPE_SZ 7

/*
 * This is the header for each capability.  Includes information about
 * allowed operations (read/write for specific cores), the size of the
 * capabilities in a cache-line, and the type of the capability.
 */
struct cap_header {
	/* When we deactivate a cap entry, we set the liveness_id and
	 * change the type to quiescence. */
	u16_t liveness_id;
	/*
	 * Size is only populated on cache-line-aligned entries.
	 * Applies to all caps in that cache-line
	 */
	u8_t        amap : CAP_HEAD_AMAP_SZ; /* allocation map */
	cap_sz_t    size : CAP_HEAD_SZ_SZ;
	cap_flags_t flags : CAP_HEAD_FLAGS_SZ;
	cap_t       type : CAP_HEAD_TYPE_SZ;

	u8_t post[];
} __attribute__((packed));

struct cap_min {
	struct cap_header h;
	/* 1/4 of a cacheline minimal */
	char padding[(CACHELINE_SIZE / 4) - sizeof(struct cap_header)];
};

/* the 2 higher bits in refcnt are flags. */
#define CAP_MEM_REFCNT_SZ 30 /* 32 - 2 bits for flags */
#define CAP_REFCNT_MAX ((1 << CAP_MEM_REFCNT_SZ) - 1)
#define CAP_MEM_FROZEN_FLAG (1 << (CAP_MEM_REFCNT_SZ))
#define CAP_MEM_SCAN_FLAG (1 << (CAP_MEM_REFCNT_SZ + 1))

/* Capability structure to a capability table */
struct cap_captbl {
	struct cap_header  h;
	u32_t              refcnt_flags; /* includes refcnt and flags */
	struct captbl *    captbl;
	u32_t              lvl;       /* what level are the captbl nodes at? */
	struct cap_captbl *parent;    /* if !null, points to parent cap */
	u64_t              frozen_ts; /* timestamp when frozen is set. */
} __attribute__((packed));

static void *
__captbl_allocfn(void *d, int sz, int last_lvl)
{
	void **mem = d; /* really a pointer to a pointer */
	void * m   = *mem;

	/* dewarn */
	(void)last_lvl;

	assert(sz <= PAGE_SIZE / 2);
	*mem = NULL; /* NULL so we don't do mult allocs */

	return m;
}

#define CT_MSK(v, o) ((unsigned long)(v) & ~((1 << (o)) - 1))

static void
__captbl_init(struct ert_intern *a, int leaf)
{
	if (leaf) return;
	a->next = NULL;
}

static void
captbl_init(void *node, int leaf)
{
	int i;

	if (!leaf) {
		for (i = 0; i < 1 << CAPTBL_INTERN_ORD; i++) {
			struct ert_intern *a;
			a       = (struct ert_intern *)(((char *)node) + (i * CAPTBL_INTERNSZ));
			a->next = NULL;
		}
	} else {
		for (i = 0; i < 1 << CAPTBL_LEAF_ORD; i++) {
			struct cap_header *p = &(((struct cap_min *)node)[i].h);
			p->size              = CAP_SZ_64B;
			p->type              = CAP_FREE;
			p->amap              = 0;
			p->flags             = 0;
			p->liveness_id       = 0;
		}
	}
}

static inline CFORCEINLINE void *
__captbl_getleaf(struct ert_intern *a, void *accum)
{
	unsigned long      off, mask;
	struct cap_header *h, *c;
	/* dewarn */
	(void)accum;

	/*
	 * fastpath: lets make everything statically computable by
	 * telling the compiler the capability size.
	 */
	h = (struct cap_header *)CT_MSK(a, CACHELINE_ORDER);
	if (likely(h->size == __captbl_cap2sz(CAP_SINV))) {
		c   = (struct cap_header *)CT_MSK(a, __captbl_cap2sz(CAP_SINV) + CAP_SZ_OFF);
		off = (struct cap_min *)c - (struct cap_min *)h; /* ptr math */
		if (likely(h->amap & (1 << off))) return c;
	}

	/*
	 * This requires explanation.  We want to avoid a conditional
	 * to check if this slot in the allocation map for the cache
	 * line is free or not.
	 */
	c    = (struct cap_header *)CT_MSK(a, h->size + CAP_SZ_OFF);
	off  = (struct cap_min *)c - (struct cap_min *)h;    /* ptr math */
	mask = (h->amap & (1 << off)) >> off;                /* 0 or 1, depending */
	mask--;                                              /* 0 or 0xFFFF... */
	c = (struct cap_header *)((unsigned long)c & ~mask); /* NULL, or the address */

	return c;
}

static inline int
__captbl_setleaf(struct ert_intern *a, void *v)
{
	(void)a;
	(void)v;
	assert(0);
	return -1;
}

static inline CFORCEINLINE struct ert_intern *
__captbl_get(struct ert_intern *a, void *accum, int leaf)
{
	(void)accum;
	(void)leaf;
	return a->next;
}

#define CT_DEFINITVAL NULL
ERT_CREATE(__captbl, captbl, CAPTBL_DEPTH, CAPTBL_INTERN_ORD, CAPTBL_INTERNSZ, CAPTBL_LEAF_ORD, CAPTBL_LEAFSZ,
           CT_DEFINITVAL, __captbl_init, __captbl_get, ert_defisnull, ert_defset, __captbl_allocfn, __captbl_setleaf,
           __captbl_getleaf, ert_defresolve);

static struct captbl *
captbl_alloc(void *page)
{
	return __captbl_alloc(&page);
}

static inline int
__captbl_header_validate(struct cap_header *h, cap_sz_t sz)
{
	cap_sz_t mask;
	/* compiler should optimize away the branches here */
	switch (sz) {
	case CAP_SZ_16B:
		mask = CAP_MASK_16B;
		break;
	case CAP_SZ_32B:
		mask = CAP_MASK_32B;
		break;
	case CAP_SZ_64B:
		mask = CAP_MASK_64B;
		break;
	default:
		mask = 0;
		break;
	}

	if (unlikely(sz != h->size)) return 1;
	return h->amap & ~mask;
}

static inline void *
captbl_lkup_lvl(struct captbl *t, capid_t cap, u32_t start_lvl, u32_t end_lvl)
{
	cap &= __captbl_maxid() - 1; /* Assume: 2s complement math */
	return __captbl_lkupani(t, cap, start_lvl, end_lvl, NULL);
}

/*
 * This function is the fast-path used for capability lookup in the
 * invocation path.
 */
static inline struct cap_header *
captbl_lkup(struct captbl *t, capid_t cap)
{
	return captbl_lkup_lvl(t, cap, 0, CAPTBL_DEPTH + 1);
}

static inline int
__captbl_store_32(u32_t *addr, u32_t new, u32_t old)
{
	if (!cos_cas_32(addr, old, new)){ return -1;}

	return 0;
}

#define CTSTORE(a, n, o) \
	({                            \
		int tmp_ret = 0;     \
		u32_t *tmp_n = n;   \
		u32_t *tmp_o = o;   \
		tmp_ret = __captbl_store_32((u32_t *)a, *tmp_n, *tmp_o); \
		tmp_ret; \
	})

#define cos_throw(label, errno) \
	{                       \
		ret = (errno);  \
		goto label;     \
	}

//#include <stdio.h>

static inline struct cap_header *
captbl_add(struct captbl *t, capid_t cap, cap_t type, int *retval)
{
	struct cap_header *p, *h;
	struct cap_header  l, o;
	u64_t              curr_ts, past_ts;
	int                ret = 0, off;
	cap_sz_t           sz  = __captbl_cap2sz(type);

	if (unlikely(sz == CAP_SZ_ERR)) cos_throw(err, -EINVAL);
	if (unlikely(cap >= __captbl_maxid())) cos_throw(err, -EINVAL);

	p = __captbl_lkupan(t, cap, CAPTBL_DEPTH, NULL);
	if (unlikely(!p)) cos_throw(err, -EPERM);

	h = (struct cap_header *)CT_MSK(p, CACHELINE_ORDER);
	l = o = *h;
	if (unlikely(l.flags & CAP_FLAG_RO)) cos_throw(err, -EPERM);

	off = (struct cap_min *)p - (struct cap_min *)h; /* ptr math */
	assert(off >= 0 && off < CAP_HEAD_AMAP_SZ);
	/* already allocated? */
	if (unlikely(l.amap & (1 << off))) cos_throw(err, -EEXIST);
	if (unlikely((l.amap && (l.size != sz)))) cos_throw(err, -EEXIST);

	l.amap |= 1 << off;

	/* Quiescence check: either check the entire cacheline if
	 * needed, or a single entry. */
	if (l.type == CAP_QUIESCENCE && l.size != sz) {
		/* FIXME: when false sharing happens, other cores
		 * could already changed the size and type of the
		 * cacheline. */

		/* The entire cacheline has been deactivated
		 * before. We need to make sure all entries in the
		 * cacheline has reached quiescence before re-size. */
		int                i, n_ent, ent_size;
		struct cap_header *header_i;
		assert(l.size);
		ent_size = 1 << (l.size + CAP_SZ_OFF);

		rdtscll(curr_ts);
		header_i = h;
		n_ent    = CACHELINE_SIZE >> (l.size + CAP_SZ_OFF);
		for (i = 0; i < n_ent; i++) {
			assert((void *)header_i < ((void *)h + CACHELINE_SIZE));

			/* non_zero liv_id means deactivation happened. */
			if (header_i->liveness_id && header_i->type == CAP_QUIESCENCE) {
				if (ltbl_get_timestamp(header_i->liveness_id, &past_ts)) cos_throw(err, -EFAULT);
				/* quiescence period for cap entries
				 * is the worst-case in kernel
				 * execution time. */
				if (!QUIESCENCE_CHECK(curr_ts, past_ts, KERN_QUIESCENCE_CYCLES))
					cos_throw(err, -EQUIESCENCE);
			}

			header_i = (void *)header_i + ent_size; /* get next header */
		}
	} else {
		/* check only the current single entry */
		if (p->liveness_id && p->type == CAP_QUIESCENCE) {
			/* means a deactivation on this cap entry happened
			 * before. */
			rdtscll(curr_ts);
			if (ltbl_get_timestamp(p->liveness_id, &past_ts)) {
				cos_throw(err, -EFAULT);
			}
			if (!QUIESCENCE_CHECK(curr_ts, past_ts, KERN_QUIESCENCE_CYCLES)) cos_throw(err, -EQUIESCENCE);
		}
	}

	if (l.size != sz) l.size = sz;
	if (unlikely(__captbl_header_validate(&l, sz))){ cos_throw(err, -EINVAL);}

	/* FIXME: we should _not_ do this here.  This should be done
	 * in step 3 of the protocol for setting capabilities, not 1 */
	if (p == h) {
		l.type        = type;
		l.liveness_id = 0;
	}
	if (CTSTORE(h, (u32_t *)&l,(u32_t *)&o)) cos_throw(err, -EEXIST); /* commit */

	/* FIXME: same as above */
	if (p != h) {
		p->type        = type;
		p->liveness_id = 0;
	}

	/* FIXME: same as above! */
	//	assert(p == __captbl_lkupan(t, cap, CAPTBL_DEPTH+1, NULL));
	*retval = ret;

	return p;
err:
	*retval = ret;
	return NULL;
}

static inline int
captbl_del(struct captbl *t, capid_t cap, cap_t type, livenessid_t lid)
{
	struct cap_header *p, *h;
	struct cap_header  l, o;
	int                ret = 0, off;

	if (unlikely(cap >= __captbl_maxid())) cos_throw(err, -EINVAL);
	p = __captbl_lkupan(t, cap, CAPTBL_DEPTH, NULL);

	if (unlikely(!p)) cos_throw(err, -EPERM);
	if (p != __captbl_getleaf((void *)p, NULL)) cos_throw(err, -EINVAL);
	if (p->type != type) cos_throw(err, -EINVAL);

	h   = (struct cap_header *)CT_MSK(p, CACHELINE_ORDER);
	off = (struct cap_min *)p - (struct cap_min *)h;
	assert(off >= 0 && off < CAP_HEAD_AMAP_SZ);
	l = o = *h;

	/* Do we want RO to prevent deletions? */
	if (unlikely(l.flags & CAP_FLAG_RO)) cos_throw(err, -EPERM);
	if (unlikely(!(l.amap & (1 << off)))) cos_throw(err, -ENOENT);

	/* Update timestamp first. */
	ret = ltbl_timestamp_update(lid);

	if (unlikely(ret)) cos_throw(err, ret);

	if (h == p) {
		l.liveness_id = lid;
		l.type        = CAP_QUIESCENCE;
	} else {
		p->liveness_id = lid;
		p->type        = CAP_QUIESCENCE;
	}
	cos_mem_fence();

	/* new map, removing the current allocation */
	l.amap &= (~(1 << off)) & ((1 << CAP_HEAD_AMAP_SZ) - 1);
	if (l.amap == 0) {
		/* no active allocations... */
		/* we can't change l.size yet. still need it to check
		 * quiescence. */
		l.type = CAP_QUIESCENCE;
	}

	if (CTSTORE(h, (u32_t *)&l, (u32_t *)&o)) cos_throw(err, -EEXIST); /* commit */
err:
	return ret;
}

static inline u32_t
captbl_maxdepth(void)
{
	return __captbl_maxdepth();
}

/*
 * Extend a capability table up to a depth using the memory passed in
 * via memctxt.  Returns a negative value on error, a positive value
 * if the table is extended, but the depth specified was too deep
 * (requiring more memory), and zero on unqualified success.
 */
static inline int
captbl_expand(struct captbl *t, capid_t cap, u32_t depth, void *memctxt)
{
	int ret;

	if (unlikely(cap >= __captbl_maxid() || depth > captbl_maxdepth())) return -EINVAL;
	ret = __captbl_expandn(t, cap, depth, NULL, &memctxt, NULL);
	if (unlikely(memctxt)) return -EEXIST;
	if (unlikely(ret)) return 1; /* extended successfully, but incorrect depth value */

	return 0;
}

/*
 * Prune off part of the capability tree at the specified depth along
 * the path to the specified capability number.  depth == 1 will prune
 * off all but the root.
 */
static void *
captbl_prune(struct captbl *t, capid_t cap, u32_t depth, int *retval)
{
	unsigned long *intern, p, new;
	int            ret = 0;

	if (unlikely(cap >= __captbl_maxid() || depth >= captbl_maxdepth())) cos_throw(err, -EINVAL);
	intern = __captbl_lkupan(t, cap, depth, NULL);
	if (unlikely(!intern)) cos_throw(err, -EPERM);
	p   = *intern;
	new = (unsigned long)CT_DEFINITVAL;
	if (CTSTORE(intern, (u32_t*)&new, (u32_t *)&p)) cos_throw(err, -EEXIST); /* commit */
done:
	*retval = ret;
	return (void *)p;
err:
	p = (unsigned long)NULL;
	goto done;
}

static struct captbl *
captbl_create(void *page)
{
	struct captbl *ct;
	int            ret;

	assert(page);
	ct = captbl_alloc(page);
	assert(ct);
	/*
	 * replace hard-coded sizes with calculations based on captbl
	 * depth, and intern and leaf sizes/orders
	 */
	captbl_init(page, 0);
	captbl_init(&((char *)page)[PAGE_SIZE / 2], 1);
	ret = captbl_expand(ct, 0, captbl_maxdepth(), &((char *)page)[PAGE_SIZE / 2]);
	assert(!ret);

	return ct;
}

int captbl_activate(struct captbl *t, capid_t cap, capid_t capin, struct captbl *toadd, u32_t lvl);
int captbl_deactivate(struct captbl *t, struct cap_captbl *dest_ct_cap, unsigned long capin, livenessid_t lid,
                      capid_t pgtbl_cap, capid_t cosframe_addr, const int root);
int captbl_activate_boot(struct captbl *t, unsigned long cap);

int captbl_cons(struct cap_captbl *target_ct, struct cap_captbl *cons_cap, capid_t cons_addr);
int captbl_decons(struct cap_header *head, struct cap_header *sub, capid_t pruneid, unsigned long lvl);
int captbl_kmem_scan(struct cap_captbl *cap);

static void
cap_init(void)
{
	assert(sizeof(struct cap_captbl) <= __captbl_cap2bytes(CAP_CAPTBL));
	assert(((1 << CAPTBL_LEAF_ORD) * CAPTBL_LEAFSZ + CAPTBL_INTERNSZ * (1 << CAPTBL_INTERN_ORD)) == PAGE_SIZE);
	assert(CAPTBL_EXPAND_SZ == 1 << CAPTBL_LEAF_ORD);
}

#endif /* CAPTBL_H */