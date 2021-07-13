/**
 * Copyright 2014 by Gabriel Parmer, gparmer@gwu.edu
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 */

#include "include/shared/cos_types.h"
#include "include/captbl.h"
#include "include/thd.h"
#include "include/chal/call_convention.h"
#include "include/ipi_cap.h"
#include "include/liveness_tbl.h"
#include "include/chal/cpuid.h"
#include "include/tcap.h"
#include "include/chal/defs.h"
#include "include/hw.h"
#include "include/chal/chal_proto.h"

#define COS_DEFAULT_RET_CAP 0

/*
 * TODO: switch to a dedicated TLB flush thread (in a separate
 * protection domain) to do this.
 */
void
tlb_mandatory_flush(void *arg)
{
	unsigned long long t;
	(void)arg;

	rdtscll(t);
	/* Order is important: get tsc before action. */
	chal_flush_tlb();
	/* But commit after. */
	tlb_quiescence[get_cpuid()].last_mandatory_flush = t;
}

#define MAX_LEN 512
extern char timer_detector[PAGE_SIZE] PAGE_ALIGNED;
static inline int
printfn(struct pt_regs *regs)
{
	char *str;
	int   len;
	char  kern_buf[MAX_LEN];

	str = (char *)(unsigned long)__userregs_get1(regs);
	len = __userregs_get2(regs);

	if (len < 1) goto done;
	if (len >= MAX_LEN) len = MAX_LEN - 1;
	memcpy(kern_buf, str, len);

	/*
	 * Hack of all hacks.  Manually request TLB flushes from
	 * user-level.  Should be replaced with invocations to async
	 * invocations instead.
	 */
	if (len >= 7) {
		if (kern_buf[0] == 'F' && kern_buf[1] == 'L' && kern_buf[2] == 'U' && kern_buf[3] == 'S'
		    && kern_buf[4] == 'H' && kern_buf[5] == '!') {
			int target_cpu = kern_buf[6];

			if (target_cpu == get_cpuid()) {
				tlb_mandatory_flush(NULL);
			} else {
				/* FIXME: avoid using this band-aid. */
				chal_remote_tlb_flush(target_cpu);
			}

			__userregs_set(regs, 0, __userregs_getsp(regs), __userregs_getip(regs));

			return 0;
		}
	}

	kern_buf[len] = '\0';
	printk("%s", kern_buf);
done:
	__userregs_set(regs, 0, __userregs_getsp(regs), __userregs_getip(regs));

	return 0;
}

void
kmem_unalloc(unsigned long *pte)
{
	/*
	 * Release the kmem page. We must be the only one has access
	 * to it.  It is most useful when newly allocated kernel
	 * memory must be quickly de-allocated due to an error
	 * elsewhere.
	 */
	unsigned long old = *pte;

	assert(chal_pgtbl_flag_exist(old, PGTBL_COSKMEM));
	retypetbl_deref((void *)(old & PGTBL_FRAME_MASK), PAGE_ORDER);
	*pte = chal_pgtbl_flag_clr(*pte, PGTBL_COSKMEM);
}

/*
 * The deact_pre / _post are used by kernel object deactivation:
 * cap_captbl, cap_pgtbl and thd.
 */
int
kmem_deact_pre(struct cap_header *ch, struct captbl *ct, capid_t pgtbl_cap, capid_t cosframe_addr,
               unsigned long **p_pte, unsigned long *v)
{
	struct cap_pgtbl *cap_pt;
	u32_t             flags, old_v, pa;
	u64_t             curr;
	int               ret;

	assert(ct && ch);
	if (!pgtbl_cap || !cosframe_addr) cos_throw(err, -EINVAL);

	cap_pt = (struct cap_pgtbl *)captbl_lkup(ct, pgtbl_cap);
	if (!CAP_TYPECHK(cap_pt, CAP_PGTBL)) cos_throw(err, -EINVAL);

	/* get the pte to the cos frame. */
	*p_pte = pgtbl_lkup_pte(cap_pt->pgtbl, cosframe_addr, &flags);
	if (!p_pte) cos_throw(err, -EINVAL);
	old_v = *v = **p_pte;

	pa = old_v & PGTBL_FRAME_MASK;
	 if (!chal_pgtbl_flag_exist(old_v, PGTBL_COSKMEM)) cos_throw(err, -EINVAL);
	 assert(!chal_pgtbl_flag_exist(old_v, PGTBL_QUIESCENCE)); 

	/* Scan the page to make sure there's nothing left. */
	if (ch->type == CAP_CAPTBL) {
		struct cap_captbl *deact_cap = (struct cap_captbl *)ch;
		void *             page      = deact_cap->captbl;
		u32_t              l         = deact_cap->refcnt_flags;

		if (chal_pa2va((paddr_t)pa) != page) cos_throw(err, -EINVAL);

		/* Require freeze memory and wait for quiescence
		 * first! */
		if (!(l & CAP_MEM_FROZEN_FLAG)) {
			cos_throw(err, -EQUIESCENCE);
		}
		/* Quiescence check! */
		if (deact_cap->lvl == 0) {
			/* top level has tlb quiescence period (due to
			 * the optimization to avoid current_component
			 * lookup on invocation path). */
			if (!tlb_quiescence_check(deact_cap->frozen_ts)) return -EQUIESCENCE;
		} else {
			/* other levels have kernel quiescence period. */
			rdtscll(curr);
			if (!QUIESCENCE_CHECK(curr, deact_cap->frozen_ts, KERN_QUIESCENCE_CYCLES)) return -EQUIESCENCE;
		}

		/* set the scan flag to avoid concurrent scanning. */
		if (cos_cas_32((u32_t*)&deact_cap->refcnt_flags, l, l | CAP_MEM_SCAN_FLAG) != CAS_SUCCESS)
			return -ECASFAIL;

		/*
		 * When gets here, we know quiescence has passed. and
		 * we are holding the scan lock.
		 */
		if (deact_cap->lvl < CAPTBL_DEPTH - 1) {
			ret = kmem_page_scan(page, PAGE_SIZE);
		} else {
			ret = captbl_kmem_scan(deact_cap);
		}

		if (ret) {
			/* unset scan and frozen bits. */
			cos_cas_32((u32_t *)&deact_cap->refcnt_flags, l | CAP_MEM_SCAN_FLAG,
			        l & ~(CAP_MEM_FROZEN_FLAG | CAP_MEM_SCAN_FLAG));
			cos_throw(err, ret);
		}
		cos_cas_32((u32_t *)&deact_cap->refcnt_flags, l | CAP_MEM_SCAN_FLAG, l);
	} else if (ch->type == CAP_PGTBL) {
		ret = chal_pgtbl_deact_pre(ch, pa);
		if (ret) cos_throw(err, ret);
	} else {
		/* currently only captbl and pgtbl pages need to be
		 * scanned before deactivation. */
		struct cap_thd *tc = (struct cap_thd *)ch;

		if (chal_pa2va((paddr_t)pa) != (void *)(tc->t)) cos_throw(err, -EINVAL);

		assert(ch->type == CAP_THD);
	}

	return 0;
err:

	return ret;
}

/* Updates the pte, deref the frame and zero out the page. */
int
kmem_deact_post(unsigned long *pte, unsigned long old_v)
{
	int   ret;
	unsigned long new_v;
	/* Unset coskmem bit. Release the kmem frame. */
	new_v = chal_pgtbl_flag_clr(old_v, PGTBL_COSKMEM);

	if (cos_cas(pte, old_v, new_v) != CAS_SUCCESS) cos_throw(err, -ECASFAIL);

	ret = retypetbl_kern_deref((void *)(old_v & PGTBL_FRAME_MASK), PAGE_ORDER);
	if (ret) {
		/* FIXME: handle this case? */
		cos_cas(pte, new_v, old_v);
		cos_throw(err, ret);
	}
	/* zero out the page to avoid info leaking. */
	memset(chal_pa2va((paddr_t)(old_v & PGTBL_FRAME_MASK)), 0, PAGE_SIZE);

	return 0;
err:
	return ret;
}

/*
 * Copy a capability from a location in one captbl/pgtbl to a location
 * in the other.  Fundamental operation used to delegate capabilities.
 * TODO: should limit the types of capabilities this works on.
 * The order is the power of 2 of the size of the (sub)page delegated.
 *
 */
static inline int
cap_cpy(struct captbl *t, capid_t cap_to, capid_t capin_to, capid_t cap_from, capid_t capin_from, vaddr_t order)
{
	struct cap_header *ctto, *ctfrom;
	int                sz, ret;
	cap_t              cap_type;

	ctfrom = captbl_lkup(t, cap_from);
	if (unlikely(!ctfrom)) return -ENOENT;

	cap_type = ctfrom->type;

	if (cap_type == CAP_CAPTBL) {
		u32_t old_v, l;
		cap_t type;

		ctfrom = captbl_lkup(((struct cap_captbl *)ctfrom)->captbl, capin_from);
		if (unlikely(!ctfrom)) return -ENOENT;

		type = ctfrom->type;
		sz   = __captbl_cap2bytes(type);

		ctto = __cap_capactivate_pre(t, cap_to, capin_to, type, &ret);
		if (!ctto) return -EINVAL;

		memcpy(ctto->post, ctfrom->post, sz - sizeof(struct cap_header));

		if (type == CAP_THD) {
			/* thd is per-core. refcnt in thd struct. */
			struct thread *thd = ((struct cap_thd *)ctfrom)->t;

			thd->refcnt++;
		} else if (type == CAP_CAPTBL) {
			struct cap_captbl *parent = (struct cap_captbl *)ctfrom;
			struct cap_captbl *child  = (struct cap_captbl *)ctto;

			old_v = l = parent->refcnt_flags;
			if (l & CAP_MEM_FROZEN_FLAG) return -EINVAL;
			if ((l & CAP_REFCNT_MAX) == CAP_REFCNT_MAX) return -EOVERFLOW;

			cos_cas_32((u32_t *)&(parent->refcnt_flags), old_v, l + 1);

			child->refcnt_flags = 1;
			child->parent       = parent;
		} else if (type == CAP_PGTBL) {
			struct cap_pgtbl *parent = (struct cap_pgtbl *)ctfrom;
			struct cap_pgtbl *child  = (struct cap_pgtbl *)ctto;

			old_v = l = parent->refcnt_flags;
			if (l & CAP_MEM_FROZEN_FLAG) return -EINVAL;
			if ((l & CAP_REFCNT_MAX) == CAP_REFCNT_MAX) return -EOVERFLOW;

			cos_cas_32((u32_t *)&(parent->refcnt_flags), old_v, l + 1);

			child->refcnt_flags = 1;
			child->parent       = parent;
		}
		__cap_capactivate_post(ctto, type);
	} else if (cap_type == CAP_PGTBL) {
		ret = chal_pgtbl_cpy(t, cap_to, capin_to, (struct cap_pgtbl*)ctfrom, capin_from, cap_type, order);
	} else {
		ret = -EINVAL;
	}

	return ret;
}

static inline int
cap_move(struct captbl *t, capid_t cap_to, capid_t capin_to, capid_t cap_from, capid_t capin_from)
{
	struct cap_header *ctto, *ctfrom;
	int                ret;
	cap_t              cap_type;

	ctfrom = captbl_lkup(t, cap_from);
	if (unlikely(!ctfrom)) return -ENOENT;

	cap_type = ctfrom->type;

	if (cap_type == CAP_CAPTBL) {
		/* no cap copy needed yet. */
		return -EPERM;
	} else if (cap_type == CAP_PGTBL) {
		unsigned long *f, old_v, *moveto, old_v_to;
		u32_t          flags;

		ctto = captbl_lkup(t, cap_to);
		if (unlikely(!ctto)) return -ENOENT;
		if (unlikely(ctto->type != cap_type)) return -EINVAL;
		if (unlikely(((struct cap_pgtbl *)ctto)->refcnt_flags & CAP_MEM_FROZEN_FLAG)) return -EINVAL;
		f = pgtbl_lkup_pte(((struct cap_pgtbl *)ctfrom)->pgtbl, capin_from, &flags);
		if (!f) return -ENOENT;
		old_v = *f;

		moveto = pgtbl_lkup_pte(((struct cap_pgtbl *)ctto)->pgtbl, capin_to, &flags);
		if (!moveto) return -ENOENT;
		old_v_to = *moveto;

		cos_mem_fence();
		if (!chal_pgtbl_flag_exist(old_v, PGTBL_COSFRAME)) return -EPERM;
		if (chal_pgtbl_flag_exist(old_v_to, PGTBL_COSFRAME | PGTBL_PRESENT)) return -EPERM;

		ret = pgtbl_quie_check(old_v_to);
		if (ret) return ret;

		/* valid to move. doing CAS next. */
		ret = cos_cas(f, old_v, 0);
		if (ret != CAS_SUCCESS) return -ECASFAIL;

		ret = cos_cas(moveto, old_v_to, old_v);
		if (ret != CAS_SUCCESS) {
			/*
			 * FIXME: reverse if the second cas fails. We
			 * should lock down the moveto slot first.
			 *
			 * We need to provide quiescence for the reuse
			 * of the old slot above so that it *cannot*
			 * be reused immediately.  Another option
			 * would be to have a "placeholder" in the
			 * slot so that it cannot be reused.
			 */
			return -ECASFAIL;
		}
		ret = 0;
	} else {
		ret = -EINVAL;
	}

	return ret;
}

static int
cap_thd_switch(struct pt_regs *regs, struct thread *curr, struct thread *next, struct comp_info *ci,
               struct cos_cpu_local_info *cos_info)
{
	struct next_thdinfo *nti     = &cos_info->next_ti;
	struct comp_info *   next_ci = &(next->invstk[next->invstk_top].comp_info);
	int                  preempt = 0;

	assert(next_ci && curr && next);
	assert(curr->cpuid == get_cpuid() && next->cpuid == get_cpuid());
	if (unlikely(curr == next)) return thd_switch_update(curr, regs, 1);

	/* FIXME: trigger fault for the next thread, for now, return error */
	if (unlikely(!ltbl_isalive(&next_ci->liveness))) {
		assert(!(curr->state & THD_STATE_PREEMPTED));
		__userregs_set(regs, -EFAULT, __userregs_getsp(regs), __userregs_getip(regs));
		return 0;
	}

	if (!(curr->state & THD_STATE_PREEMPTED)) {
		copy_gp_regs(regs, &curr->regs);
		__userregs_set(&curr->regs, 0, __userregs_getsp(&curr->regs), __userregs_getip(&curr->regs));
	} else {
		copy_all_regs(regs, &curr->regs);
	}

	thd_current_update(next, curr, cos_info);
	if (likely(ci->pgtblinfo.pgtbl != next_ci->pgtblinfo.pgtbl)) pgtbl_update(&next_ci->pgtblinfo);

	/* Not sure of the trade-off here: Branch cost vs. segment register update */
	if (next->tls != curr->tls) chal_tls_update(next->tls);

	preempt = thd_switch_update(next, &next->regs, 0);
	/* if switching to the preempted/awoken thread clear cpu local next_thdinfo */
	if (nti->thd && nti->thd == next) thd_next_thdinfo_update(cos_info, 0, 0, 0, 0);

	copy_all_regs(&next->regs, regs);

	return preempt;
}

static struct thread *
notify_parent(struct thread *rcv_thd, int send)
{
	struct thread *curr_notif = NULL, *prev_notif = NULL, *arcv_notif = NULL;
	int            depth = 0;

	/* hierarchical notifications - upto init (bounded by ARCV_NOTIF_DEPTH) */
	prev_notif = rcv_thd;
	curr_notif = arcv_notif = arcv_thd_notif(prev_notif);

	while (curr_notif && curr_notif != prev_notif) {
		assert(depth < ARCV_NOTIF_DEPTH);

		thd_rcvcap_evt_enqueue(curr_notif, prev_notif);
		if (!(curr_notif->state & THD_STATE_RCVING)) break;

		prev_notif = curr_notif;
		curr_notif = arcv_thd_notif(prev_notif);
		depth++;
	}

	return arcv_notif;
}

/**
 * Notify the appropriate end-points.
 * Return the thread that should be executed next.
 */
static struct thread *
notify_process(struct thread *rcv_thd, struct thread *thd, struct tcap *rcv_tcap, struct tcap *tcap,
               struct tcap **tcap_next, int yield)
{
	struct thread *next;

	notify_parent(rcv_thd, 1);

	/* The thread switch decision: */
	if (yield || tcap_higher_prio(rcv_tcap, tcap)) {
		next       = rcv_thd;
		*tcap_next = rcv_tcap;
	} else {
		next       = thd;
		*tcap_next = tcap;
	}

	return next;
}

/**
 * Process the send event, and notify the appropriate end-points.
 * Return the thread that should be executed next.
 */
static struct thread *
asnd_process(struct thread *rcv_thd, struct thread *thd, struct tcap *rcv_tcap, struct tcap *tcap,
             struct tcap **tcap_next, int yield, struct cos_cpu_local_info *cos_info)
{
	struct thread *next;

	thd_rcvcap_pending_inc(rcv_thd);
	next = notify_process(rcv_thd, thd, rcv_tcap, tcap, tcap_next, yield);

	/*
	 * FIXME: Need to revisit the preemption-stack functionality
	 *
	 * if (next == thd)
	 * 	tcap_wakeup(rcv_tcap, tcap_sched_info(rcv_tcap)->prio, 0, rcv_thd, cos_info);
	 * else
	 * 	thd_next_thdinfo_update(cos_info, thd, tcap, tcap_sched_info(tcap)->prio, 0);
	 */

	return next;
}

static int
cap_update(struct pt_regs *regs, struct thread *thd_curr, struct thread *thd_next, struct tcap *tc_curr,
           struct tcap *tc_next, tcap_time_t timeout, struct comp_info *ci, struct cos_cpu_local_info *cos_info,
           int timer_intr_context)
{
	struct thread *thd_sched;
	cycles_t       now;
	int            switch_away = 0;

	/* which tcap should we use?  is the current expended? */
	if (tcap_budgets_update(cos_info, thd_curr, tc_curr, &now)) {
		assert(!tcap_is_active(tc_curr) && tcap_expended(tc_curr));

		/*
		 * FIXME: child scheduler should abide by parent's timeouts.
		 * for now, we set timeout to 0 to use the budget of the tcap for timer interrupt programming.
		 */
		timeout = 0;
		if (timer_intr_context) tc_next= thd_rcvcap_tcap(thd_next);

		/* how about the scheduler's tcap? */
		if (tcap_expended(tc_next)) {
			/* finally...the active list */
			tc_next = tcap_active_next(cos_info);
			/* in active list?...better have budget */
			assert(tc_next && !tcap_expended(tc_next));
			/* and the next thread should be the scheduler of this tcap */
			thd_next    = thd_rcvcap_sched(tcap_rcvcap_thd(tc_next));
			switch_away = 1;
		}
	} else if (timer_intr_context) {
		/*
		 * If this is a timer interrupt and the current tcap has not been expended,
		 * then the timer interrupt is for a timeout request.
		 * choose the current thread's scheduler as next thread.
		 *
		 * Note: If the timer interrupt was indeed for a timeout but the current tcap
		 *       has expended, then budget expiry condition takes priority.
		 */
		if (thd_bound2rcvcap(thd_curr)
		    && thd_rcvcap_isreferenced(thd_curr)) thd_next = thd_rcvcap_sched(tcap_rcvcap_thd(tc_curr));
		else                                      thd_next = thd_scheduler(thd_curr);
		/* tc_next is tc_curr */
	}

	if (unlikely(switch_away)) notify_parent(thd_next, 1);

	/* update tcaps, and timers */
	tcap_timer_update(cos_info, tc_next, timeout, now);
	tcap_current_set(cos_info, tc_next);

	if (timer_intr_context) {
		/* update only tcap and return to curr thread */
		if (thd_next == thd_curr) return 1;
		thd_curr->state |= THD_STATE_PREEMPTED;
	}

	/* switch threads */
	return cap_thd_switch(regs, thd_curr, thd_next, ci, cos_info);
}

static int
cap_switch(struct pt_regs *regs, struct thread *curr, struct thread *next, struct tcap *next_tcap, tcap_time_t timeout,
           struct comp_info *ci, struct cos_cpu_local_info *cos_info)
{
	return cap_update(regs, curr, next, tcap_current(cos_info), next_tcap, timeout, ci, cos_info, 0);
}

static int
cap_sched_tok_validate(struct thread *rcvt, sched_tok_t usr_tok, struct comp_info *ci, struct cos_cpu_local_info *cos_info)
{
	assert(rcvt && usr_tok < ~0U);

	/* race-condition check for user-level thread switches */
	if (thd_rcvcap_get_counter(rcvt) > usr_tok) return -EAGAIN;
	thd_rcvcap_set_counter(rcvt, usr_tok);

	return 0;
}

static int
cap_thd_op(struct cap_thd *thd_cap, struct thread *thd, struct pt_regs *regs, struct comp_info *ci,
           struct cos_cpu_local_info *cos_info)
{
	struct thread *next        = thd_cap->t;
	capid_t        arcv        = (__userregs_get1(regs) << 16) >> 16;
	capid_t        tc          = __userregs_get1(regs) >> 16;
	u32_t          prio_higher = __userregs_get3(regs);
	u32_t          prio_lower  = __userregs_get2(regs);
	tcap_prio_t    prio        = (tcap_prio_t)(prio_higher >> 16) << 32 | (tcap_prio_t)prio_lower;
	sched_tok_t    usr_counter = (((sched_tok_t)__userregs_get3(regs) << 16) >> 16)
	                          | ((sched_tok_t)__userregs_getop(regs) << 16); /* op holds MSB of counter */
	tcap_time_t  timeout = (tcap_time_t)__userregs_get4(regs);
	struct tcap *tcap    = tcap_current(cos_info);
	int          ret;

	if (thd_cap->cpuid != get_cpuid() || thd_cap->cpuid != next->cpuid) return -EINVAL;

	if (arcv) {
		struct cap_arcv *arcv_cap;
		struct thread *  rcvt;

		arcv_cap = (struct cap_arcv *)captbl_lkup(ci->captbl, arcv);
		if (!CAP_TYPECHK_CORE(arcv_cap, CAP_ARCV)) return -EINVAL;
		rcvt = arcv_cap->thd;

		ret  = cap_sched_tok_validate(rcvt, usr_counter, ci, cos_info);
		if (ret) return ret;

		if (thd_rcvcap_pending(rcvt) > 0) {
			if (thd == rcvt) return -EBUSY;

			next = rcvt;
			/* tcap inheritance here...use the current tcap to process events */
			tc      = 0;
			timeout = TCAP_TIME_NIL;
		} else {
			thd_scheduler_set(next, rcvt);
		}
	} else {
		/* TODO: set current thread as it's scheduler? */
		thd_scheduler_set(next, thd);
	}

	if (tc) {
		struct cap_tcap *tcap_cap;

		tcap_cap = (struct cap_tcap *)captbl_lkup(ci->captbl, tc);
		if (!CAP_TYPECHK_CORE(tcap_cap, CAP_TCAP)) return -EINVAL;
		tcap = tcap_cap->tcap;
		if (!tcap_rcvcap_thd(tcap)) return -EINVAL;
		if (unlikely(!tcap_is_active(tcap))) return -EPERM;
	}

	ret = cap_switch(regs, thd, next, tcap, timeout, ci, cos_info);
	if (tc && tcap_current(cos_info) == tcap) tcap_setprio(tcap, prio);

	return ret;
}

static inline struct cap_arcv *
__cap_asnd_to_arcv(struct cap_asnd *asnd)
{
	struct cap_arcv *arcv;

	if (unlikely(!ltbl_isalive(&(asnd->comp_info.liveness)))) return NULL;
	arcv = (struct cap_arcv *)captbl_lkup(asnd->comp_info.captbl, asnd->arcv_capid);
	if (unlikely(!CAP_TYPECHK(arcv, CAP_ARCV))) return NULL;
	/* FIXME: check arcv epoch + liveness */

	return arcv;
}

int
cap_ipi_process(struct pt_regs *regs)
{
	struct cos_cpu_local_info  *cos_info = cos_cpu_local_info();
	struct IPI_receiving_rings *receiver_rings;
	struct xcore_ring 	   *ring;
	struct ipi_cap_data 	    data;
	struct cap_arcv 	   *arcv;
	struct thread 		   *thd_curr, *thd_next;
	struct tcap 		   *tcap_curr, *tcap_next;
	struct comp_info 	   *ci;
	int                         i, scan_base;
	unsigned long               ip, sp;

	thd_curr       = thd_next = thd_current(cos_info);
	receiver_rings = &IPI_cap_dest[get_cpuid()];
	tcap_curr      = tcap_next = tcap_current(cos_info);
	ci             = thd_invstk_current(thd_curr, &ip, &sp, cos_info);
	assert(ci && ci->captbl);

	scan_base = receiver_rings->start;
	receiver_rings->start = (receiver_rings->start + 1) % NUM_CPU;

	/* We need to scan the entire buffer once. */
	for (i = 0; i < NUM_CPU; i++) {
		struct thread *rcvthd  = NULL;
		struct tcap   *rcvtcap = NULL;

		ring = &receiver_rings->IPI_source[(scan_base + i) % NUM_CPU];

		if (ring->sender == ring->receiver) continue;
		while ((cos_ipi_ring_dequeue(ring, &data)) != 0) {
			arcv = cos_ipi_arcv_get(&data);
			assert(arcv);

			rcvthd  = arcv->thd;
			rcvtcap = rcvthd->rcvcap.rcvcap_tcap;
			assert(rcvthd && rcvtcap);

			/*
			 * tcap_higher_prio (partial-order qualities) check for "highest" prio so far and the next
			 * thread in the ring (dequeued item).
			 */
			thd_next = asnd_process(rcvthd, thd_next, rcvtcap, tcap_next, &tcap_next, 0, cos_info);
		}
	}

	if (thd_next == thd_curr) return 1;
	thd_curr->state |= THD_STATE_PREEMPTED;

	return cap_switch(regs, thd_curr, thd_next, tcap_next, TCAP_TIME_NIL, ci, cos_info);
}

static int
cap_asnd_op(struct cap_asnd *asnd, struct thread *thd, struct pt_regs *regs, struct comp_info *ci,
            struct cos_cpu_local_info *cos_info)
{
	int              curr_cpu = get_cpuid();
	capid_t          srcv     = __userregs_get1(regs);
	sched_tok_t      usr_tok  = __userregs_get2(regs);
	tcap_time_t      timeout  = __userregs_get3(regs);
	int              yield    = __userregs_get4(regs);
	struct cap_arcv *arcv;
	struct thread *  rcv_thd, *next;
	struct tcap *    rcv_tcap, *tcap, *tcap_next;
	int              ret;

	assert(asnd->arcv_capid);
	/* IPI notification to another core */
	if (asnd->arcv_cpuid != curr_cpu) {
		/* ignore yield flag */
		assert(!srcv);

		ret = cos_cap_send_ipi(asnd->arcv_cpuid, asnd);
		/* special handling for IPI send */
		if (likely(ret == 0)) __userregs_set(regs, 0, __userregs_getsp(regs), __userregs_getip(regs));

		return ret;
	}

	arcv = __cap_asnd_to_arcv(asnd);
	if (unlikely(!arcv)) return -EINVAL;

	rcv_thd  = arcv->thd;
	tcap     = tcap_current(cos_info);
	rcv_tcap = rcv_thd->rcvcap.rcvcap_tcap;
	assert(rcv_tcap && tcap);

	if (srcv) {
		struct cap_arcv *srcv_cap;
		struct thread   *rcvt;

		srcv_cap = (struct cap_arcv *)captbl_lkup(ci->captbl, srcv);
		if (!CAP_TYPECHK_CORE(srcv_cap, CAP_ARCV)) return -EINVAL;
		rcvt = srcv_cap->thd;

		ret  = cap_sched_tok_validate(rcvt, usr_tok, ci, cos_info);
		if (ret) return ret;

		if (thd_rcvcap_pending(rcvt) > 0) {
			if (thd == rcvt) return -EBUSY;

			next = rcvt;
			/* tcap inheritance here...use the current tcap to process events */
			tcap_next = tcap;
			timeout   = TCAP_TIME_NIL;
			goto done;
		}

		if (unlikely(tcap_expended(rcv_tcap))) return -EPERM;
		yield = 1; /* scheduling child thread, so yield to it. */

		thd_scheduler_set(rcv_thd, rcvt);
		/* FIXME: child component to abide by the parent's timeout */
	}

	next = asnd_process(rcv_thd, thd, rcv_tcap, tcap, &tcap_next, yield, cos_info);

done:
	return cap_switch(regs, thd, next, tcap_next, timeout, ci, cos_info);
}

int
cap_hw_asnd(struct cap_asnd *asnd, struct pt_regs *regs)
{
	int                        curr_cpu = get_cpuid();
	struct cap_arcv *          arcv;
	struct cos_cpu_local_info *cos_info;
	struct thread *            rcv_thd, *next, *thd;
	struct tcap *              rcv_tcap, *tcap, *tcap_next;
	struct comp_info *         ci;
	unsigned long              ip, sp;

	if (!CAP_TYPECHK(asnd, CAP_ASND)) return 1;
	assert(asnd->arcv_capid);

	/* IPI notification to another core */
	if (asnd->arcv_cpuid != curr_cpu) {
		cos_cap_send_ipi(asnd->arcv_cpuid, asnd);
		return 1;
	}

	arcv = __cap_asnd_to_arcv(asnd);
	if (unlikely(!arcv)) return 1;

	cos_info = cos_cpu_local_info();
	assert(cos_info);
	thd  = thd_current(cos_info);
	tcap = tcap_current(cos_info);
	assert(thd);
	ci = thd_invstk_current(thd, &ip, &sp, cos_info);
	assert(ci && ci->captbl);
	assert(!(thd->state & THD_STATE_PREEMPTED));
	rcv_thd  = arcv->thd;
	rcv_tcap = rcv_thd->rcvcap.rcvcap_tcap;
	assert(rcv_tcap && tcap);

	next = asnd_process(rcv_thd, thd, rcv_tcap, tcap, &tcap_next, 0, cos_info);
	if (next == thd) return 1;
	thd->state |= THD_STATE_PREEMPTED;

	return cap_switch(regs, thd, next, tcap_next, TCAP_TIME_NIL, ci, cos_info);
}

int
expended_process(struct pt_regs *regs, struct thread *thd_curr, struct comp_info *ci,
                 struct cos_cpu_local_info *cos_info, int timer_intr_context)
{
	struct thread *thd_next;
	struct tcap *  tc_curr, *tc_next;

	tc_curr = tc_next = tcap_current(cos_info);
	assert(tc_curr);
	/* get the scheduler thread */
	thd_next = thd_rcvcap_sched(tcap_rcvcap_thd(tc_curr));
	assert(thd_next && thd_bound2rcvcap(thd_next) && thd_rcvcap_isreferenced(thd_next));

	return cap_update(regs, thd_curr, thd_next, tc_curr, tc_next, TCAP_TIME_NIL, ci, cos_info, timer_intr_context);
}

/**
 * This is the main function for maintaining time, budgets, expiring
 * tcaps, and sending timer notifications to scheduler threads.
 *
 * This function:
 * 1. gets all of the current state (thread/tcap/component)
 * 2. finds who should be activated from the current tcap (which scheduler thread)
 * 3. finds an active tcap if the current tcap and the scheduler's tcap are out of budget
 * 4. notifies the scheduler of the thread's execution
 * 5. switch tcap and thread context
 */
int
timer_process(struct pt_regs *regs)
{
	struct cos_cpu_local_info *cos_info;
	struct thread *            thd_curr;
	struct comp_info *         comp;
	unsigned long              ip, sp;
	cycles_t                   now;

	cos_info = cos_cpu_local_info();
	assert(cos_info);
	thd_curr = thd_current(cos_info);
	assert(thd_curr && thd_curr->cpuid == get_cpuid());
	comp = thd_invstk_current(thd_curr, &ip, &sp, cos_info);
	assert(comp);

	return expended_process(regs, thd_curr, comp, cos_info, 1);
}

static int
cap_arcv_op(struct cap_arcv *arcv, struct thread *thd, struct pt_regs *regs, struct comp_info *ci,
            struct cos_cpu_local_info *cos_info)
{
	struct thread *      next;
	struct tcap *        tc_next     = tcap_current(cos_info);
	struct next_thdinfo *nti         = &cos_info->next_ti;
	rcv_flags_t          rflags      = __userregs_get1(regs);
	tcap_time_t          swtimeout   = TCAP_TIME_NIL;
	tcap_time_t          timeout     = __userregs_get2(regs);
	int                  all_pending = (!!(rflags & RCV_ALL_PENDING));

	if (unlikely(arcv->thd != thd || arcv->cpuid != get_cpuid())) return -EINVAL;

	/* deliver pending notifications? */
	if (thd_rcvcap_pending(thd)) {
		__userregs_set(regs, 0, __userregs_getsp(regs), __userregs_getip(regs));
		thd_rcvcap_all_pending_set(thd, all_pending);
		thd_rcvcap_pending_deliver(thd, regs);

		return 0;
	} else if (rflags & RCV_NON_BLOCKING) {
		__userregs_set(regs, 0, __userregs_getsp(regs), __userregs_getip(regs));
		__userregs_setretvals(regs, -EAGAIN, 0, 0, 0);

		return 0;
	}
	__userregs_setretvals(regs, 0, 0, 0, 0);

	next = notify_parent(thd, 0);
	/* TODO: should we continue tcap-inheritence policy in this case? */
	if (unlikely(tc_next != thd_rcvcap_tcap(thd))) tc_next = thd_rcvcap_tcap(thd);

	/* if preempted/awoken thread is waiting, switch to that */
	if (nti->thd) {
		assert(nti->tc);

		next    = nti->thd;
		tc_next = nti->tc;
		tcap_setprio(nti->tc, nti->prio);
		if (nti->budget) {
			/* convert budget to timeout */
			cycles_t now;
			rdtscll(now);
			swtimeout = tcap_cyc2time(now + nti->budget);
		}
		thd_next_thdinfo_update(cos_info, 0, 0, 0, 0);
	}

	/* FIXME:  for now, lets just ignore this path...need to plumb tcaps into it */
	thd->interrupted_thread = NULL;
	if (thd->interrupted_thread) {
		next = thd->interrupted_thread;
		assert(next->state & THD_STATE_PREEMPTED);
		thd->interrupted_thread = NULL;
		assert(0); /* need to take care of the tcap as well */
	}

	if (likely(thd != next)) {
		assert(!(thd->state & THD_STATE_PREEMPTED));
		thd->state |= THD_STATE_RCVING;
		thd_rcvcap_all_pending_set(thd, all_pending);
		thd->timeout = timeout;
	}

	return cap_switch(regs, thd, next, tc_next, swtimeout, ci, cos_info);
}

static int
cap_introspect(struct captbl *ct, capid_t capid, u32_t op, unsigned long *retval)
{
	struct cap_header *ch = captbl_lkup(ct, capid);

	if (unlikely(!ch)) return -EINVAL;

	switch (ch->type) {
	case CAP_THD:
		return thd_introspect(((struct cap_thd *)ch)->t, op, retval);
	case CAP_TCAP:
		return tcap_introspect(((struct cap_tcap *)ch)->tcap, op, retval);
	case CAP_ARCV:
		return arcv_introspect(((struct cap_arcv *)ch), op, retval);
	default:
		return -EINVAL;
	}
}

#define ENABLE_KERNEL_PRINT

static int composite_syscall_slowpath(struct pt_regs *regs, int *thd_switch);

COS_SYSCALL __attribute__((section("__ipc_entry"))) int
composite_syscall_handler(struct pt_regs *regs)
{
	struct cap_header *ch;
	struct comp_info * ci;
	struct thread *    thd;
	capid_t            cap;
	unsigned long      ip, sp;

	/*
	 * We lookup this struct (which is on stack) only once, and
	 * pass it into other functions to avoid redundant lookups.
	 */
	struct cos_cpu_local_info *cos_info   = cos_cpu_local_info();
	int                        ret        = -ENOENT;
	int                        thd_switch = 0;

	cap = __userregs_getcap(regs);
	thd = thd_current(cos_info);

	/* fast path: invocation return (avoiding captbl accesses) */
	if (cap == COS_DEFAULT_RET_CAP) {
		/* No need to lookup captbl */
		sret_ret(thd, regs, cos_info);
		return 0;
	}

	/* FIXME: use a cap for print */
	if (unlikely(cap == PRINT_CAP_TEMP)) {
		printfn(regs);
		return 0;
	}

	ci = thd_invstk_current(thd, &ip, &sp, cos_info);
	assert(ci && ci->captbl);

	/*
	 * We don't check the liveness of the current component
	 * because it's guaranteed by component quiescence period,
	 * which is at timer tick granularity.
	 */
	ch = captbl_lkup(ci->captbl, cap);
	if (unlikely(!ch)) {
		printk("cos: cap %d not found!\n", (int)cap);
		cos_throw(done, 0);
	}
	/* fastpath: invocation */
	if (likely(ch->type == CAP_SINV)) {
		sinv_call(thd, (struct cap_sinv *)ch, regs, cos_info);
		return 0;
	}

	/*
	 * Some less common, but still optimized cases:
	 * thread dispatch, asnd and arcv operations.
	 */
	switch (ch->type) {
	case CAP_THD:
		ret = cap_thd_op((struct cap_thd *)ch, thd, regs, ci, cos_info);
		if (ret < 0) cos_throw(done, ret);
		return ret;
	case CAP_ASND:
		ret = cap_asnd_op((struct cap_asnd *)ch, thd, regs, ci, cos_info);
		if (ret < 0) cos_throw(done, ret);
		return ret;
	case CAP_ARCV:
		ret = cap_arcv_op((struct cap_arcv *)ch, thd, regs, ci, cos_info);
		if (ret < 0) cos_throw(done, ret);
		return ret;
	default:
		break;
	}

	/* slowpath restbl (captbl and pgtbl) operations */
	ret = composite_syscall_slowpath(regs, &thd_switch);
	if (ret < 0) cos_throw(done, ret);

	if (thd_switch) return ret;
done:
	/*
	 * Note: we need to return ret to user-level (e.g. as a return
	 * value), which is not the return value of this function.
	 * Thus the level of indirection here.
	 */
	__userregs_set(regs, ret, __userregs_getsp(regs), __userregs_getip(regs));

	return 0;
}

/*
 * slowpath: other capability operations, most of which
 * involve updating the resource tables.
 */
static int __attribute__((noinline)) composite_syscall_slowpath(struct pt_regs *regs, int *thd_switch)
{
	struct cap_header *        ch;
	struct comp_info *         ci;
	struct captbl *            ct;
	struct thread *            thd;
	capid_t                    cap, capin;
	syscall_op_t               op;
	int                        ret      = -ENOENT;
	struct cos_cpu_local_info *cos_info = cos_cpu_local_info();
	unsigned long              ip, sp;

	/*
	 * These variables are:
	 *
	 * ct:     the current captbl
	 * cap:    main capability looking up into the captbl
	 * capin:  capability to lookup *inside* that reftbl
	 * ch:     the header of cap in captbl
	 * op_cap: ch casted as captbl
	 * op:     operation to perform on the capability
	 */

	thd   = thd_current(cos_info);
	cap   = __userregs_getcap(regs);
	capin = __userregs_get1(regs);

	ci = thd_invstk_current(thd, &ip, &sp, cos_info);
	assert(ci && ci->captbl);
	ct = ci->captbl;

	ch = captbl_lkup(ct, cap);
	assert(ch);
	op = __userregs_getop(regs);

	switch (ch->type) {
	case CAP_CAPTBL: {
		struct cap_captbl *op_cap = (struct cap_captbl *)ch;

		/*
		 * FIXME: make sure that the lvl of the pgtbl makes
		 * sense for the op.
		 */
		switch (op) {
		case CAPTBL_OP_CAPTBLACTIVATE: {
			capid_t newcaptbl_cap = __userregs_get1(regs);
			capid_t pgtbl_cap     = __userregs_get2(regs);
			vaddr_t kmem_cap      = __userregs_get3(regs);
			int     captbl_lvl    = __userregs_get4(regs);

			struct captbl *newct;
			unsigned long *pte       = NULL;
			vaddr_t        kmem_addr = 0;

			ret = cap_kmem_activate(ct, pgtbl_cap, kmem_cap, (unsigned long *)&kmem_addr, &pte);
			if (unlikely(ret)) cos_throw(err, ret);
			assert(kmem_addr && pte);

			if (captbl_lvl == 0) {
				newct = captbl_create((void *)kmem_addr);
				assert(newct);
			} else {
				captbl_init((void *)kmem_addr, 1);
				captbl_init((void *)(kmem_addr + PAGE_SIZE / 2), 1);
			}

			ret = captbl_activate(ct, cap, newcaptbl_cap, (struct captbl *)kmem_addr, captbl_lvl);
			if (ret) kmem_unalloc(pte);

			break;
		}
		case CAPTBL_OP_CAPTBLDEACTIVATE: {
			livenessid_t lid = __userregs_get2(regs);

			ret = captbl_deactivate(ct, op_cap, capin, lid, 0, 0, 0);

			break;
		}
		case CAPTBL_OP_CAPTBLDEACTIVATE_ROOT: {
			livenessid_t lid           = __userregs_get2(regs);
			capid_t      pgtbl_cap     = __userregs_get3(regs);
			capid_t      cosframe_addr = __userregs_get4(regs);

			ret = captbl_deactivate(ct, op_cap, capin, lid, pgtbl_cap, cosframe_addr, 1);

			break;
		}
		case CAPTBL_OP_PGTBLACTIVATE: {
			capid_t pt_entry  = __userregs_get1(regs);
			capid_t pgtbl_cap = __userregs_get2(regs);
			vaddr_t kmem_cap  = __userregs_get3(regs);
			capid_t pgtbl_order = __userregs_get4(regs);
			/* FIXME: change lvl to order */
			ret = chal_pgtbl_pgtblactivate(ct, cap, pt_entry, pgtbl_cap, kmem_cap, pgtbl_order);

			break;
		}
		case CAPTBL_OP_PGTBLDEACTIVATE: {
			livenessid_t lid = __userregs_get2(regs);

			ret = pgtbl_deactivate(ct, op_cap, capin, lid, 0, 0, 0);

			break;
		}
		case CAPTBL_OP_PGTBLDEACTIVATE_ROOT: {
			livenessid_t lid           = __userregs_get2(regs);
			capid_t      pgtbl_cap     = __userregs_get3(regs);
			capid_t      cosframe_addr = __userregs_get4(regs);

			ret = pgtbl_deactivate(ct, op_cap, capin, lid, pgtbl_cap, cosframe_addr, 1);

			break;
		}
		case CAPTBL_OP_THDACTIVATE: {
			thdclosure_index_t init_data  = __userregs_get1(regs) >> 16;
			capid_t thd_cap               = __userregs_get1(regs) & 0xFFFF;
			capid_t pgtbl_cap             = __userregs_get2(regs);
			capid_t pgtbl_addr            = __userregs_get3(regs);
			capid_t compcap               = __userregs_get4(regs);

			struct thread *thd;
			unsigned long *pte = NULL;

			ret = cap_kmem_activate(ct, pgtbl_cap, pgtbl_addr, (unsigned long *)&thd, &pte);
			if (unlikely(ret)) cos_throw(err, ret);
			assert(thd && pte);

			/* ret is returned by the overall function */
			ret = thd_activate(ct, cap, thd_cap, thd, compcap, init_data);
			if (ret) kmem_unalloc(pte);

			break;
		}
		case CAPTBL_OP_TCAP_ACTIVATE: {
			capid_t        tcap_cap   = __userregs_get1(regs) >> 16;
			capid_t        pgtbl_cap  = (__userregs_get1(regs) << 16) >> 16;
			capid_t        pgtbl_addr = __userregs_get2(regs);
			struct tcap *  tcap;
			unsigned long *pte = NULL;

			ret = cap_kmem_activate(ct, pgtbl_cap, pgtbl_addr, (unsigned long *)&tcap, &pte);
			if (unlikely(ret)) cos_throw(err, ret);

			ret = tcap_activate(ct, cap, tcap_cap, tcap);
			if (ret) kmem_unalloc(pte);

			break;
		}
		case CAPTBL_OP_THDDEACTIVATE: {
			livenessid_t lid = __userregs_get2(regs);

			ret = thd_deactivate(ct, op_cap, capin, lid, 0, 0, 0);
			break;
		}
		case CAPTBL_OP_THDTLSSET: {
			capid_t thd_cap = __userregs_get1(regs);
			vaddr_t tlsaddr = __userregs_get2(regs);

			assert(op_cap->captbl);
			if (thd_tls_set(op_cap->captbl, thd_cap, tlsaddr, thd)) cos_throw(err, -EINVAL);
			break;
		}
		case CAPTBL_OP_THDDEACTIVATE_ROOT: {
			livenessid_t lid           = __userregs_get2(regs);
			capid_t      pgtbl_cap     = __userregs_get3(regs);
			capid_t      cosframe_addr = __userregs_get4(regs);

			ret = thd_deactivate(ct, op_cap, capin, lid, pgtbl_cap, cosframe_addr, 1);
			break;
		}
		case CAPTBL_OP_CAPKMEM_FREEZE: {
			capid_t freeze_cap = capin;

			ret = cap_kmem_freeze(op_cap->captbl, freeze_cap);
			break;
		}
		case CAPTBL_OP_COMPACTIVATE: {
			capid_t      captbl_cap = __userregs_get2(regs) >> 16;
			capid_t      pgtbl_cap  = __userregs_get2(regs) & 0xFFFF;
			livenessid_t lid        = __userregs_get3(regs);
			vaddr_t      entry_addr = __userregs_get4(regs);

			ret = comp_activate(ct, cap, capin, captbl_cap, pgtbl_cap, lid, entry_addr);
			break;
		}
		case CAPTBL_OP_COMPDEACTIVATE: {
			livenessid_t lid = __userregs_get2(regs);

			ret = comp_deactivate(op_cap, capin, lid);
			break;
		}
		case CAPTBL_OP_SINVACTIVATE: {
			capid_t    dest_comp_cap = __userregs_get2(regs);
			vaddr_t    entry_addr    = __userregs_get3(regs);
			invtoken_t token         = __userregs_get4(regs);

			ret = sinv_activate(ct, cap, capin, dest_comp_cap, entry_addr, token);
			break;
		}
		case CAPTBL_OP_SINVDEACTIVATE: {
			livenessid_t lid = __userregs_get2(regs);

			ret = sinv_deactivate(op_cap, capin, lid);
			break;
		}
		case CAPTBL_OP_SRETACTIVATE: {
			ret = -EINVAL;
			break;
		}
		case CAPTBL_OP_SRETDEACTIVATE: {
			livenessid_t lid = __userregs_get2(regs);

			ret = sret_deactivate(op_cap, capin, lid);
			break;
		}
		case CAPTBL_OP_ASNDACTIVATE: {
			capid_t rcv_captbl = __userregs_get2(regs);
			capid_t rcv_cap    = __userregs_get3(regs);

			ret = asnd_activate(ct, cap, capin, rcv_captbl, rcv_cap);
			break;
		}
		case CAPTBL_OP_ASNDDEACTIVATE: {
			livenessid_t lid = __userregs_get2(regs);

			ret = asnd_deactivate(op_cap, capin, lid);
			break;
		}
		case CAPTBL_OP_ARCVACTIVATE: {
			capid_t thd_cap  = (__userregs_get2(regs) << 16) >> 16;
			capid_t tcap_cap = __userregs_get2(regs) >> 16;
			capid_t comp_cap = __userregs_get3(regs);
			capid_t arcv_cap = __userregs_get4(regs);

			ret = arcv_activate(ct, cap, capin, comp_cap, thd_cap, tcap_cap, arcv_cap, 0);
			break;
		}
		case CAPTBL_OP_ARCVDEACTIVATE: {
			livenessid_t lid = __userregs_get2(regs);

			ret = arcv_deactivate(op_cap, capin, lid);
			break;
		}
		case CAPTBL_OP_CPY: {
			capid_t from_captbl = cap;
			capid_t from_cap    = __userregs_get1(regs);
			capid_t dest_captbl = __userregs_get2(regs);
			capid_t dest_cap    = __userregs_get3(regs);

			ret = cap_cpy(ct, dest_captbl, dest_cap, from_captbl, from_cap, 0);
			break;
		}
		case CAPTBL_OP_CONS: {
			capid_t cons_addr = __userregs_get2(regs);

			ret = cap_cons(ct, cap, capin, cons_addr);
			break;
		}
		case CAPTBL_OP_DECONS: {
			capid_t decons_addr = __userregs_get2(regs);
			capid_t lvl         = __userregs_get3(regs);

			/* FIXME: adding liveness id here. */

			ret = cap_decons(ct, cap, capin, decons_addr, lvl);

			break;
		}
		case CAPTBL_OP_INTROSPECT: {
			struct captbl *ctin   = op_cap->captbl;
			unsigned long  retval = 0;
			u32_t          op     = __userregs_get2(regs);
			assert(ctin);

			ret = cap_introspect(ctin, capin, op, &retval);
			if (!ret) ret= retval;

			break;
		}
		case CAPTBL_OP_HW_ACTIVATE: {
			u32_t bitmap = __userregs_get2(regs);

			ret = hw_activate(ct, cap, capin, bitmap);
			break;
		}
		case CAPTBL_OP_HW_DEACTIVATE: {
			livenessid_t lid = __userregs_get2(regs);

			ret = hw_deactivate(op_cap, capin, lid);
			break;
		}
		default:
			goto err;
		}
		break;
	}
	case CAP_PGTBL: {
		/* pgtbl_t pt = ((struct cap_pgtbl *)ch)->pgtbl; */
		capid_t pt = cap;

		switch (op) {
		case CAPTBL_OP_CPY: {
			capid_t source_pt   = pt;
			vaddr_t source_addr = __userregs_get1(regs);
			capid_t dest_pt     = __userregs_get2(regs);
			vaddr_t dest_addr   = __userregs_get3(regs);
			vaddr_t order       = __userregs_get4(regs);

			ret = cap_cpy(ct, dest_pt, dest_addr, source_pt, source_addr, order);
			break;
		}
		case CAPTBL_OP_MEMMOVE: {
			/* Moves a mem frame to another pgtbl. Used to
			 * grant frames to memory management
			 * components.  */
			capid_t source_pt   = pt;
			vaddr_t source_addr = __userregs_get1(regs);
			capid_t dest_pt     = __userregs_get2(regs);
			vaddr_t dest_addr   = __userregs_get3(regs);

			ret = cap_move(ct, dest_pt, dest_addr, source_pt, source_addr);

			break;
		}
		case CAPTBL_OP_CONS: {
			vaddr_t pte_cap   = __userregs_get1(regs);
			vaddr_t cons_addr = __userregs_get2(regs);

			ret = cap_cons(ct, pt, pte_cap, cons_addr);

			break;
		}
		case CAPTBL_OP_DECONS: {
			capid_t decons_addr = __userregs_get2(regs);
			capid_t lvl         = __userregs_get3(regs);

			ret = cap_decons(ct, cap, capin, decons_addr, lvl);

			break;
		}
		case CAPTBL_OP_MEMACTIVATE: {
			/* This takes cosframe as input and constructs mapping in pgtbl. */
			capid_t frame_cap = __userregs_get1(regs);
			capid_t dest_pt   = __userregs_get2(regs);
			vaddr_t vaddr     = __userregs_get3(regs);
			vaddr_t order     = __userregs_get4(regs);

			ret = cap_memactivate(ct, (struct cap_pgtbl *)ch, frame_cap, dest_pt, vaddr, order);

			break;
		}
		case CAPTBL_OP_MEMDEACTIVATE: {
			vaddr_t      addr = __userregs_get1(regs);
			livenessid_t lid  = __userregs_get2(regs);

			if (((struct cap_pgtbl *)ch)->lvl) cos_throw(err, -EINVAL);

			ret = pgtbl_mapping_del(((struct cap_pgtbl *)ch)->pgtbl, addr, lid);

			break;
		}
		case CAPTBL_OP_MEM_RETYPE2USER: {
			vaddr_t frame_addr = __userregs_get1(regs);
			paddr_t frame;
			vaddr_t order;

			ret = pgtbl_get_cosframe(((struct cap_pgtbl *)ch)->pgtbl, frame_addr, &frame, &order);
			if (ret) cos_throw(err, ret);

			if (__userregs_get2(regs) != 0) order = __userregs_get2(regs);
			ret = retypetbl_retype2user((void *)frame, order);

			break;
		}
		case CAPTBL_OP_MEM_RETYPE2KERN: {
			vaddr_t frame_addr = __userregs_get1(regs);
			paddr_t frame;
			vaddr_t order;

			ret = pgtbl_get_cosframe(((struct cap_pgtbl *)ch)->pgtbl, frame_addr, &frame, &order);

			if (ret) cos_throw(err, ret);

			if (__userregs_get2(regs) != 0) order = __userregs_get2(regs);
			ret = retypetbl_retype2kern((void *)frame, order);

			break;
		}
		case CAPTBL_OP_MEM_RETYPE2FRAME: {
			vaddr_t frame_addr = __userregs_get1(regs);
			paddr_t frame;
			vaddr_t order;

			ret = pgtbl_get_cosframe(((struct cap_pgtbl *)ch)->pgtbl, frame_addr, &frame, &order);
			if (ret) cos_throw(err, ret);

			if (__userregs_get2(regs) != 0) order = __userregs_get2(regs);
			ret = retypetbl_retype2frame((void *)frame, order);

			break;
		}
		case CAPTBL_OP_INTROSPECT: {
			vaddr_t        addr = __userregs_get1(regs);

			ret = chal_pgtbl_introspect(ch, addr);
			break;
		}
		/* case CAPTBL_OP_MAPPING_MOD: */
		default:
			goto err;
		}
		break;
	}
	case CAP_SRET: {
		/*
		 * We usually don't have sret cap as we have 0 as the
		 * default return cap.
		 */
		sret_ret(thd, regs, cos_info);
		return 0;
	}
	case CAP_TCAP: {
		/* TODO: Validate that all tcaps are on the same core */
		switch (op) {
		case CAPTBL_OP_TCAP_TRANSFER: {
			capid_t          tcpdst      = __userregs_get1(regs);
			tcap_res_t       res         = __userregs_get2(regs);
			u32_t            prio_higher = __userregs_get3(regs);
			u32_t            prio_lower  = __userregs_get4(regs);
			tcap_prio_t      prio        = (tcap_prio_t)prio_higher << 32 | (tcap_prio_t)prio_lower;
			struct cap_tcap *tcapsrc     = (struct cap_tcap *)ch;
			struct cap_arcv *rcv;
			struct thread *  rthd;
			struct tcap *    tc;

			rcv = (struct cap_arcv *)captbl_lkup(ci->captbl, tcpdst);
			if (!CAP_TYPECHK_CORE(rcv, CAP_ARCV)) cos_throw(err, -EINVAL);

			rthd = rcv->thd;
			tc   = rthd->rcvcap.rcvcap_tcap;
			assert(rthd && tc);

			ret = tcap_delegate(tc, tcapsrc->tcap, res, prio);
			if (unlikely(ret)) cos_throw(err, -EINVAL);

			if (tcap_expended(tcap_current(cos_info))) {
				ret = expended_process(regs, thd, ci, cos_info, 0);
				if (unlikely(ret < 0)) cos_throw(err, ret);

				*thd_switch = 1;
			}

			break;
		}
		case CAPTBL_OP_TCAP_DELEGATE: {
			capid_t          asnd_cap    = __userregs_get1(regs);
			long long        res         = __userregs_get2(regs);
			u32_t            prio_higher = __userregs_get3(regs);
			u32_t            prio_lower  = __userregs_get4(regs);
			struct cap_tcap *tcapsrc     = (struct cap_tcap *)ch;
			struct cap_arcv *arcv;
			struct cap_asnd *asnd;
			struct thread *  rthd;
			struct tcap *    tcapdst, *tcap_next;
			struct thread *  n;
			tcap_prio_t      prio;
			int              yield;

			/* highest-order bit is dispatch flag */
			yield       = prio_higher >> ((sizeof(prio_higher) * 8) - 1);
			prio_higher = (prio_higher << 1) >> 1;
			prio        = (tcap_prio_t)prio_higher << 32 | (tcap_prio_t)prio_lower;
			asnd        = (struct cap_asnd *)captbl_lkup(ci->captbl, asnd_cap);
			if (unlikely(!CAP_TYPECHK(asnd, CAP_ASND))) cos_throw(err, -EINVAL);

			arcv = __cap_asnd_to_arcv(asnd);
			if (unlikely(!arcv)) cos_throw(err, -EINVAL);

			rthd    = arcv->thd;
			tcapdst = rthd->rcvcap.rcvcap_tcap;
			assert(rthd && tcapdst);

			ret = tcap_delegate(tcapdst, tcapsrc->tcap, res, prio);
			if (unlikely(ret)) cos_throw(err, -EINVAL);

			n = asnd_process(rthd, thd, tcapdst, tcap_current(cos_info), &tcap_next, yield, cos_info);
			if (n != thd) {
				/*
				 * FIXME: set scheduler for rcv thread with DELEG_YIELD and
				 *        when we have room for sched_rcv with this API
				 *
				 *        Also, scheduling token validation!
				 */
				ret = cap_switch(regs, thd, n, tcap_next, TCAP_TIME_NIL, ci, cos_info);
				if (unlikely(ret < 0)) cos_throw(err, ret);

				*thd_switch = 1;
			} else if (tcap_expended(tcap_current(cos_info))) {
				ret = expended_process(regs, thd, ci, cos_info, 0);
				if (unlikely(ret < 0)) cos_throw(err, ret);

				*thd_switch = 1;
			}

			break;
		}
		case CAPTBL_OP_TCAP_MERGE: {
			capid_t          tcaprem = __userregs_get1(regs);
			struct cap_tcap *tcapdst = (struct cap_tcap *)ch;
			struct cap_tcap *tcaprm;

			tcaprm = (struct cap_tcap *)captbl_lkup(ci->captbl, tcaprem);
			if (!CAP_TYPECHK_CORE(tcaprm, CAP_TCAP)) cos_throw(err, -EINVAL);

			ret = tcap_merge(tcapdst->tcap, tcaprm->tcap);
			if (unlikely(ret)) cos_throw(err, -ENOENT);

			break;
		}
		case CAPTBL_OP_TCAP_WAKEUP: {
			struct cap_thd * thdwkup;
			struct cap_tcap *tcapwkup    = (struct cap_tcap *)ch;
			capid_t          thdcap      = __userregs_get1(regs);
			u32_t            prio_higher = __userregs_get2(regs);
			u32_t            prio_lower  = __userregs_get3(regs);
			tcap_prio_t      prio        = (tcap_prio_t)prio_higher << 32 | (tcap_prio_t)prio_lower;
			tcap_res_t       budget      = __userregs_get4(regs);

			thdwkup = (struct cap_thd *)captbl_lkup(ci->captbl, thdcap);
			if (!CAP_TYPECHK_CORE(thdwkup, CAP_THD)) return -EINVAL;

			ret = tcap_wakeup(tcapwkup->tcap, prio, budget, thdwkup->t, cos_info);
			if (unlikely(ret)) cos_throw(err, -EINVAL);

			break;
		}
		default:
			goto err;
		}
	}
	case CAP_HW: {
		switch (op) {
		case CAPTBL_OP_HW_ATTACH: {
			struct cap_arcv *rcvc;
			hwid_t           hwid   = __userregs_get1(regs);
			capid_t          rcvcap = __userregs_get2(regs);

			rcvc = (struct cap_arcv *)captbl_lkup(ci->captbl, rcvcap);
			if (!CAP_TYPECHK(rcvc, CAP_ARCV)) cos_throw(err, -EINVAL);

			ret = hw_attach_rcvcap((struct cap_hw *)ch, hwid, rcvc, rcvcap);
			break;
		}
		case CAPTBL_OP_HW_DETACH: {
			hwid_t hwid = __userregs_get1(regs);

			ret = hw_detach_rcvcap((struct cap_hw *)ch, hwid);
			break;
		}
		case CAPTBL_OP_HW_MAP: {
			capid_t           ptcap = __userregs_get1(regs);
			vaddr_t           va    = __userregs_get2(regs);
			paddr_t           pa    = __userregs_get3(regs);
			struct cap_pgtbl *ptc;
			unsigned long *   pte;
			u32_t             flags;

			/*
			 * FIXME: This is broken.  It should only be
			 * used on physical address that are not
			 * accessible through the normal mechanisms
			 * (i.e. on physical apertures presented by
			 * PCI).
			 */
			ptc = (struct cap_pgtbl *)captbl_lkup(ci->captbl, ptcap);
			if (!CAP_TYPECHK(ptc, CAP_PGTBL)) cos_throw(err, -EINVAL);

			pte = pgtbl_lkup_pte(ptc->pgtbl, va, &flags);
			if (!pte) cos_throw(err, -EINVAL);
			if (*pte & PGTBL_FRAME_MASK) cos_throw(err, -ENOENT);
			*pte = chal_pgtbl_flag_add(chal_pgtbl_frame(pa), PGTBL_USER_DEF);

			ret = 0;
			break;
		}
		case CAPTBL_OP_HW_CYC_USEC: {
			ret = chal_cyc_usec();
			break;
		}
		case CAPTBL_OP_HW_CYC_THRESH: {
			ret = (int)chal_cyc_thresh();
			break;
		}
		case CAPTBL_OP_HW_SHUTDOWN: {
			chal_khalt();
			ret = 0;
			break;
		}
		case CAPTBL_OP_HW_TLB_LOCKDOWN: {
			unsigned long entryid = __userregs_get1(regs);
			unsigned long vaddr = __userregs_get2(regs);
			unsigned long paddr = __userregs_get3(regs);
			ret = chal_tlb_lockdown(entryid, vaddr, paddr);
			break;
		}
		case CAPTBL_OP_HW_L1FLUSH: {
			ret = chal_l1flush();
			break;
		}
		case CAPTBL_OP_HW_TLBFLUSH: {
			ret = chal_tlbflush(0);
			break;
		}
		case CAPTBL_OP_HW_TLBSTALL: {
			ret = chal_tlbstall();
			break;
		}
		case CAPTBL_OP_HW_TLBSTALL_RECOUNT: {
			ret = chal_tlbstall_recount(0);
			break;
		}
		default:
			goto err;
		}
		break;
	}
	default:
		break;
	}
err:
	return ret;
}
