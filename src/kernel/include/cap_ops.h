#ifndef CAP_OPS
#define CAP_OPS

#include "captbl.h"
#include "pgtbl.h"
#include "liveness_tbl.h"
#include "chal/chal_proto.h"

/*
 * Capability-table, capability operations for activation and
 * deactivation.
 */

static inline struct cap_header *
__cap_capactivate_pre(struct captbl *t, capid_t cap, capid_t capin, cap_t type, int *retval)
{
	struct cap_captbl *ct;
	struct cap_header *h = NULL;
	int                ret;

	ct = (struct cap_captbl *)captbl_lkup(t, cap);
	if (unlikely(!ct || ct->h.type != CAP_CAPTBL)) cos_throw(err, -EINVAL);
	if (unlikely(ct->refcnt_flags & CAP_MEM_FROZEN_FLAG)) cos_throw(err, -EINVAL);

	h = captbl_add(ct->captbl, capin, type, &ret);
err:
	*retval = ret;
	return h;
}

/* commit the activation */
static inline int
__cap_capactivate_post(struct cap_header *h, cap_t type)
{
	/*
	 * FIXME: should be atomic on a word including the amap and
	 * poly, mostly likely atomic add on the type/poly (they are
	 * contiguous) to avoid any conflicts at this point on amap
	 * modifications.
	 *
	 * Should also be a store barrier here as well to make sure
	 * the modifications to the capability body are committed
	 * before finally activating the cap.
	 */

	/* u32_t old_v, new_v; */
	/* struct cap_header *local; */

	cos_mem_fence();

	/* FIXME: the following is done in captbl_add now, which is
	 * wrong. */
	/*
	        new_v = old_v = *((u32_t *)h);

	        local = (struct cap_header *)&new_v;
	        local->type = type;

	        if (unlikely(!cos_cas((unsigned long *)h, old_v, new_v))) return -ECASFAIL;
	*/

	return 0;
}

static inline int
cap_capdeactivate(struct cap_captbl *ct, capid_t capin, cap_t type, livenessid_t lid)
{
	if (unlikely(!ct)) return -ENOENT;
	if (unlikely(ct->h.type != CAP_CAPTBL)) return -EINVAL;

	return captbl_del(ct->captbl, capin, type, lid);
}

static inline int
cap_kmem_activate(struct captbl *t, capid_t cap, unsigned long addr, unsigned long *kern_addr, unsigned long **pte_ret)
{
	int               ret;
	struct cap_pgtbl *pgtblc;

	assert(t);
	pgtblc = (struct cap_pgtbl *)captbl_lkup(t, cap);

	if (unlikely(!pgtblc)) return -ENOENT;
	if (unlikely(pgtblc->h.type != CAP_PGTBL || pgtblc->lvl != 0)) return -EINVAL;
	if ((ret = pgtbl_kmem_act(pgtblc->pgtbl, addr & PGTBL_FRAME_MASK, (unsigned long *)kern_addr, pte_ret)))
		return ret;

	return 0;
}

/*
 * Construction and deconstruction of the capability and page tables
 * from separate capability/page-table trees that are subtrees for
 * different levels.
 */

static inline int
cap_cons(struct captbl *t, capid_t capto, capid_t capsub, capid_t expandid)
{
	/*
	 * Note: here we're relying on the fact that cap_captbl has an
	 * identical layout to cap_pgtbl.
	 */
	struct cap_captbl *ct, *ctsub;
	u32_t              depth;
	cap_t              cap_type;
	int                ret = 0;

	if (unlikely(capto == capsub)) return -EINVAL;
	ct = (struct cap_captbl *)captbl_lkup(t, capto);
	if (unlikely(!ct)) return -ENOENT;
	cap_type = ct->h.type;
	if (unlikely(cap_type != CAP_CAPTBL && cap_type != CAP_PGTBL)) return -EINVAL;
	ctsub = (struct cap_captbl *)captbl_lkup(t, capsub);
	if (unlikely(!ctsub)) return -ENOENT;
	if (unlikely(ctsub->h.type != cap_type)) return -EINVAL;

	depth = ctsub->lvl;
	if (depth == 0) return -EINVAL; /* subtree must not have a root */

	assert(ct->captbl);
	if (cap_type == CAP_CAPTBL) {
		ret = captbl_cons(ct, ctsub, expandid);
	} else {
		ret = chal_pgtbl_cons(ct, ctsub, expandid, depth);
	}

	return ret;
}

/*
 * FIXME: Next version will probably need to provide the capability to
 * the inner node of the captbl/pgtbl so that we can maintain proper
 * reference counting.
 */
static inline int
cap_decons(struct captbl *t, capid_t cap, capid_t capsub, capid_t pruneid, unsigned long lvl)
{
	/* capsub is the cap_cap for sub level to be pruned. We need
	 * to decrement ref_cnt correctly for the kernel page. */
	struct cap_header *head, *sub;
	unsigned long *    intern, old_v;

	head = (struct cap_header *)captbl_lkup(t, cap);
	sub  = (struct cap_header *)captbl_lkup(t, capsub);
	if (unlikely(!head || !sub)) return -ENOENT;
	if (unlikely(head->type != sub->type)) return -EPERM;

	if (head->type == CAP_CAPTBL) {
		return captbl_decons(head, sub, pruneid, lvl);
	} else if (head->type == CAP_PGTBL) {
		return chal_pgtbl_decons(head, sub, pruneid, lvl);
	} 

	return -EINVAL;
}

static int
cap_kmem_freeze(struct captbl *t, capid_t target_cap)
{
	struct cap_header *ch;
	u32_t              l;
	int                ret;

	ch = captbl_lkup(t, target_cap);
	if (!ch) return -EINVAL;

	/* Only memory for captbl and pgtbl needs to be frozen before
	 * deactivation. */
	if (ch->type == CAP_CAPTBL) {
		struct cap_captbl *ct = (struct cap_captbl *)ch;
		l                     = ct->refcnt_flags;

		if ((l & CAP_REFCNT_MAX) > 1 || l & CAP_MEM_FROZEN_FLAG) return -EINVAL;

		rdtscll(ct->frozen_ts);
		ret = cos_cas_32((u32_t *)&ct->refcnt_flags, l, l | CAP_MEM_FROZEN_FLAG);
		if (ret != CAS_SUCCESS) return -ECASFAIL;
	} else if (ch->type == CAP_PGTBL) {
		struct cap_pgtbl *pt = (struct cap_pgtbl *)ch;
		l                    = pt->refcnt_flags;
		if ((l & CAP_REFCNT_MAX) > 1 || l & CAP_MEM_FROZEN_FLAG) return -EINVAL;

		rdtscll(pt->frozen_ts);
		ret = cos_cas_32((u32_t *)&pt->refcnt_flags, l, l | CAP_MEM_FROZEN_FLAG);
		if (ret != CAS_SUCCESS) return -ECASFAIL;
	} else {
		return -EINVAL;
	}

	return 0;
}

static int
kmem_page_scan(void *obj_vaddr, const int size)
{
	/* For non-leaf level captbl / pgtbl. entries are all pointers
	 * in these cases. */
	unsigned int i;
	void *       addr = obj_vaddr;

	for (i = 0; i < size / sizeof(void *); i++) {
		if (*(unsigned long *)addr != 0) return -EINVAL;
		addr++;
	}

	return 0;
}

int kmem_deact_pre(struct cap_header *ch, struct captbl *ct, capid_t pgtbl_cap, capid_t cosframe_addr,
                   unsigned long **p_pte, unsigned long *v);
int kmem_deact_post(unsigned long *pte, unsigned long old_v);

#endif /* CAP_OPS */
