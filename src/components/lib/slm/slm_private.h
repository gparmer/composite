#ifndef SLM_PRIVATE_H
#define SLM_PRIVATE_H

#include <cos_component.h>
#include <ps.h>

typedef unsigned long slm_cs_cached_t;
/* Critical section (cs) API to protect scheduler data-structures */
struct slm_cs {
	unsigned long owner_contention;
};

/*
 * - @cs - critical section
 * - @cached - the cached value of the cs
 * - @curr - current thread
 * - @owner - the thread that owns the cs
 * - @contended - {0, 1} previously contended or not
 * - @tok - scheduler synchronization token for cos_defswitch
 *
 * @ret:
 *     (Caller of this function should retry for a non-zero return value.)
 *     1 for cas failure or after successful thread switch to thread that owns the lock.
 *     -ve from cos_defswitch failure, allowing caller for ex: the scheduler thread to
 *     check if it was -EBUSY to first recieve pending notifications before retrying lock.
 */
int slm_cs_enter_contention(struct slm_cs *cs, slm_cs_cached_t cached, struct slm_thd *curr, struct slm_thd *owner, int contended, sched_tok_t tok);
/*
 * - @cs - the critical section
 * - @curr - the current thread (releasing the cs)
 * - @cached - cached copy the critical section value
 * - @tok: scheduler synchronization token
 *
 * @ret: returns 1 if we need a retry, 0 otherwise
 */
int slm_cs_exit_contention(struct slm_cs *cs, struct slm_thd *curr, slm_cs_cached_t cached, sched_tok_t tok);


/**
 * Get the metadata about the critical section (CS). This includes the
 * owner thread (or `NULL`), and if the CS has been contended by another
 * thread.
 *
 * - @cs - the critical section
 * - @thd - the *returned* owner thread or `NULL`
 * - @contention - {`0`, `1`} depending on if there is contention
 * - @ret - a *cached* version of the critical section metadata to be passed into `__slm_cs_cas`.
 */
static inline slm_cs_cached_t
__slm_cs_data(struct slm_cs *cs, struct slm_thd **thd, int *contention)
{
	unsigned long oc = ps_load(&cs->owner_contention);
	/* least significant bit signifies contention */
	*thd        = (struct slm_thd *)(oc & (~0UL << 1));
	*contention = oc & 1;

	return oc;
}

/**
 * Update the critical section atomically using compare-and-swap.
 * `cached` was returned from `__slm_cs_data`.
 *
 * - @cs - the critical section
 * - @cached - the previous value of the critical section
 * - @thd - thread owning the critical section or NULL
 * - @contention - must be in {`0`, `1`} -- denotes if we want the
 *                 contention bit to be set.
 * - @ret - `0` on success to update the cs, `1` on failure
 */
static inline int
__slm_cs_cas(struct slm_cs *cs, slm_cs_cached_t cached, struct slm_thd *thd, int contention)
{
	return !ps_cas(&cs->owner_contention, (unsigned long)cached, ((unsigned long)thd | !!contention));
}

struct slm_global {
	struct slm_cs lock;

	struct slm_thd sched_thd;
	struct slm_thd idle_thd;

	int         cyc_per_usec;
	int         timer_set; 	  /* is the timer set? */
	cycles_t    timer_next;	  /* ...what is it set to? */
	tcap_time_t timeout_next; /* ...and what is the tcap representation? */

	struct ps_list_head event_head; /* all pending events for sched end-point */
} CACHE_ALIGNED;

/*
 * Simply retrieve this core's global data-structures.
 */
static inline struct slm_global *
slm_global(void)
{
	extern struct slm_global __slm_global[NUM_CPU];

	return &__slm_global[cos_coreid()];
}

/*
 * Return if the given thread is normal, i.e. not the idle thread nor
 * the scheduler thread.
 */
static inline int
slm_thd_normal(struct slm_thd *t)
{
	struct slm_global *g = slm_global();

	return t != &g->idle_thd && t != &g->sched_thd;
}

/*
 * If the current thread is the scheduler or idle thread, return that
 * slm_thd. That thread should generally never be used
 */
struct slm_thd *slm_thd_special(void);

static inline int
slm_thd_activate(struct slm_thd *curr, struct slm_thd *t, sched_tok_t tok, int inherit_prio)
{
	struct cos_defcompinfo *dci = cos_defcompinfo_curr_get();
	struct cos_compinfo    *ci  = &dci->ci;
	struct slm_global      *g   = slm_global();
	tcap_prio_t             prio;
	tcap_time_t             timeout;
	int                     ret = 0;

	if (unlikely(t == &g->sched_thd)) {
		timeout = TCAP_TIME_NIL;
		prio    = curr->priority;
	} else {
		timeout = g->timeout_next;
		prio = inherit_prio ? curr->priority : t->priority;
	}

	if (unlikely(t->properties & SLM_THD_PROPERTY_SEND)) {
		return cos_sched_asnd(t->asnd, g->timeout_next, g->sched_thd.rcv, tok);
	} else if (unlikely(t->properties & SLM_THD_PROPERTY_OWN_TCAP)) {
		return cos_switch(t->thd, t->tc, prio, timeout, g->sched_thd.rcv, tok);
	}

	ret = cos_defswitch(t->thd, prio, timeout, tok);

	if (unlikely(ret == -EPERM && !slm_thd_normal(t))) {
		/*
		 * Attempting to activate scheduler thread or idle
		 * thread failed for no budget in it's tcap. Force
		 * switch to the scheduler with current tcap.
		 */
		ret = cos_switch(g->sched_thd.thd, 0, curr->priority, TCAP_TIME_NIL, g->sched_thd.rcv, tok);
	}

	return ret;
}

#endif	/* SLM_PRIVATE_H */
