#include "include/shared/cos_types.h"
#include "include/captbl.h"
#include "include/cap_ops.h"

/*
 * Add the capability table to itself at cap.  This should really only
 * be used at boot time to create the initial bootable components.
 * This and page-table creation are the only special cases for booting
 * the system.
 */
int
captbl_activate_boot(struct captbl *t, unsigned long cap)
{
	struct cap_captbl *ctc;
	int                ret;

	ctc = (struct cap_captbl *)captbl_add(t, cap, CAP_CAPTBL, &ret);
	if (!ctc){ return ret;}
	ctc->captbl       = t; /* reference ourself! */
	ctc->lvl          = 0;
	ctc->h.type       = CAP_CAPTBL;
	ctc->refcnt_flags = 1;
	ctc->parent       = NULL;
	return 0;
}

int
captbl_activate(struct captbl *t, unsigned long cap, unsigned long capin, struct captbl *toadd, u32_t lvl)
{
	struct cap_captbl *ct;
	int                ret;

	ct = (struct cap_captbl *)__cap_capactivate_pre(t, cap, capin, CAP_CAPTBL, &ret);
	if (!unlikely(ct)) return ret;

	ct->captbl       = toadd;
	ct->lvl          = lvl;
	ct->refcnt_flags = 1;
	ct->parent       = NULL; /* new cap has no parent. only copied cap has. */

	__cap_capactivate_post(&ct->h, CAP_CAPTBL);

	return 0;
}

int
captbl_deactivate(struct captbl *t, struct cap_captbl *dest_ct_cap, unsigned long capin, livenessid_t lid,
                  capid_t pgtbl_cap, capid_t cosframe_addr, const int root)
{
	struct cap_header *deact_header;
	struct cap_captbl *deact_cap, *parent;

	unsigned long l, old_v = 0, *pte = NULL;
	int           ret;

	deact_header = captbl_lkup(dest_ct_cap->captbl, capin);
	if (!deact_header || deact_header->type != CAP_CAPTBL) cos_throw(err, -EINVAL);

	deact_cap = (struct cap_captbl *)deact_header;
	l         = deact_cap->refcnt_flags;

	//	assert(deact_cap->refcnt_flags & CAP_REFCNT_MAX);
	if (unlikely(!(l & CAP_REFCNT_MAX))) {
		printk("ERROR: >>>>>>>>>>> refcnt flags %x\n", (unsigned int)l);
		cos_throw(err, -EFAULT);
	}

	if ((l & CAP_REFCNT_MAX) != 1) {
		/* We need to deact children first! */
		cos_throw(err, -EINVAL);
	}

	parent = deact_cap->parent;
	if (parent == NULL) {
		if (!root) cos_throw(err, -EINVAL);
		/* Last reference to the captbl page. Require pgtbl
		 * and cos_frame cap to release the kmem page. */

		ret = kmem_deact_pre(deact_header, t, pgtbl_cap, cosframe_addr, &pte, &old_v);
		if (ret) cos_throw(err, ret);
	} else {
		/* more reference exists. Sanity check. */
		if (root) cos_throw(err, -EINVAL);
		assert(!pgtbl_cap && !cosframe_addr);
	}

	ret = cap_capdeactivate(dest_ct_cap, capin, CAP_CAPTBL, lid);
	if (ret) cos_throw(err, ret);

	if (cos_cas_32((u32_t *)&deact_cap->refcnt_flags, l, CAP_MEM_FROZEN_FLAG) != CAS_SUCCESS)
		cos_throw(err, -ECASFAIL);

	/* deactivation success. We should either release the
	 * page, or decrement parent cnt. */
	if (parent == NULL) {
		/* move the kmem to COSFRAME */
		ret = kmem_deact_post(pte, old_v);
		if (ret) {
			cos_faa((int *)&deact_cap->refcnt_flags, 1);
			cos_throw(err, ret);
		}
	} else {
		cos_faa((int *)&parent->refcnt_flags, -1);
	}

	return 0;
err:
	return ret;
}

int
captbl_cons(struct cap_captbl *target_ct, struct cap_captbl *cons_cap, capid_t cons_addr)
{
	int   ret;
	u32_t l;
	void *captbl_mem;

	if (target_ct->h.type != CAP_CAPTBL || target_ct->lvl != 0) cos_throw(err, -EINVAL);
	if (cons_cap->h.type != CAP_CAPTBL || cons_cap->lvl != 1) cos_throw(err, -EINVAL);
	captbl_mem = (void *)cons_cap->captbl;
	l          = cons_cap->refcnt_flags;
	if ((l & CAP_MEM_FROZEN_FLAG) || (target_ct->refcnt_flags & CAP_MEM_FROZEN_FLAG)) cos_throw(err, -EINVAL);
	if ((l & CAP_REFCNT_MAX) == CAP_REFCNT_MAX) cos_throw(err, -EOVERFLOW);

	/* increment refcnt */
	if (cos_cas_32((u32_t *)&(cons_cap->refcnt_flags), l, l + 1) != CAS_SUCCESS) cos_throw(err, -ECASFAIL);

	/*
	 * FIXME: we are expanding the entire page to
	 * two of the locations. Do we want separate
	 * calls for them?
	 */
	ret = captbl_expand(target_ct->captbl, cons_addr, captbl_maxdepth(), captbl_mem);
	if (ret) {
		printk("first captbl_expand @ %d returns %d\n", cons_addr, ret);
		cos_faa((int *)&cons_cap->refcnt_flags, -1);
		cos_throw(err, ret);
	}

	ret = captbl_expand(target_ct->captbl, cons_addr + (PAGE_SIZE / (2 * CAPTBL_LEAFSZ)), captbl_maxdepth(),
	                    &((char *)captbl_mem)[PAGE_SIZE / 2]);
	if (ret) {
		printk("second captbl_expand returns %d\n", ret);
		/* Rewind. */
		captbl_expand(target_ct->captbl, cons_addr, captbl_maxdepth(), NULL);
		cos_faa((int *)&cons_cap->refcnt_flags, -1);
		cos_throw(err, ret);
	}

	return 0;
err:
	return ret;
}


int
captbl_decons(struct cap_header *head, struct cap_header *sub, capid_t pruneid, unsigned long lvl)
{
	unsigned long *    intern, old_v;
	u32_t l;
	struct cap_captbl *ct = (struct cap_captbl *)head;

	if (lvl <= ct->lvl) return -EINVAL;
	intern = captbl_lkup_lvl(ct->captbl, pruneid, ct->lvl, lvl);
	if (!intern) return -ENOENT;
	old_v = *intern;

	if (old_v == 0) return -ENOENT; /* return an error here? */
	/* commit; note that 0 is "no entry" in both pgtbl and captbl */
	if (cos_cas(intern, old_v, 0) != CAS_SUCCESS) return -ECASFAIL;

	/* FIXME: we are removing two half pages for captbl. */
	intern = captbl_lkup_lvl(ct->captbl, pruneid + (PAGE_SIZE / 2 / CAPTBL_LEAFSZ), ct->lvl, lvl);
	if (!intern) return -ENOENT;

	old_v = *intern;
	if (old_v == 0) return -ENOENT; /* return an error here? */
	/* commit; note that 0 is "no entry" in both pgtbl and captbl */
	if (cos_cas(intern, old_v, 0) != CAS_SUCCESS) return -ECASFAIL;

	/* decrement the refcnt */
	ct = (struct cap_captbl *)sub;
	l = ct->refcnt_flags;
	if (l & CAP_MEM_FROZEN_FLAG) return -EINVAL;
	cos_faa((int *)&(ct->refcnt_flags), -1);

	return 0;
}

static int
captbl_leaflvl_scan(struct captbl *ct)
{
	unsigned int i, ret;
	u64_t        curr_ts, past_ts;

	/* going through each cacheline. */
	for (i = 0; i < ((1 << CAPTBL_LEAF_ORD) * CAPTBL_LEAFSZ) / CACHELINE_SIZE; i++) {
		int                j, n_ent, ent_size;
		struct cap_header *h, *header_i, l;

		/* header of this cacheline. */
		h = captbl_lkup_lvl(ct, i * (CACHELINE_SIZE / CAPTBL_LEAFSZ), CAPTBL_DEPTH - 1, CAPTBL_DEPTH);

		l = *h;
		if (unlikely(l.amap)) cos_throw(err, -EINVAL);
		ent_size = 1 << (l.size + CAP_SZ_OFF);

		rdtscll(curr_ts);
		header_i = h;
		n_ent    = CACHELINE_SIZE >> (l.size + CAP_SZ_OFF) ;

		for (j = 0; j < n_ent; j++) {
			assert((void *)header_i < ((void *)h + CACHELINE_SIZE));

			/* non_zero liv_id means deactivation happened. */
			if (header_i->type == CAP_QUIESCENCE && header_i->liveness_id) {
				if (ltbl_get_timestamp(header_i->liveness_id, &past_ts)) cos_throw(err, -EFAULT);
				if (!QUIESCENCE_CHECK(curr_ts, past_ts, KERN_QUIESCENCE_CYCLES))
					cos_throw(err, -EQUIESCENCE);
			}

			header_i = (void *)header_i + ent_size; /* get next header */
		}
	}

	return 0;
err:
	return ret;
}

int
captbl_kmem_scan(struct cap_captbl *cap)
{
	/* This scans the leaf level of the captbl. We need to go
	 * through all cap entries and verify quiescence. */
	int            ret;
	struct captbl *ct = cap->captbl;
	assert(cap->lvl == CAPTBL_DEPTH - 1);

	/* each level of captbl is half page. */
	ret = captbl_leaflvl_scan(ct);
	if (ret) return ret;
	ret = captbl_leaflvl_scan((struct captbl *)((char *)ct + PAGE_SIZE / 2));
	if (ret) return ret;

	return 0;
}
