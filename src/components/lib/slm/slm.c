#include <slm.h>
#include <slm_api.h>
#include <ps_list.h>

struct slm_global __slm_global[NUM_CPU];

struct slm_thd *
slm_thd_special(void)
{
	thdid_t me = cos_thdid();
	struct slm_global *g = slm_global();

	if (me == g->sched_thd.tid)     return &g->sched_thd;
	else if (me == g->idle_thd.tid) return &g->idle_thd;
	else                            return NULL;
}

int
slm_thd_init_internal(struct slm_thd *t, thdcap_t thd, thdid_t tid)
{
	struct cos_defcompinfo *defci     = cos_defcompinfo_curr_get();
	struct cos_aep_info    *sched_aep = cos_sched_aep_get(defci);
	int ret;

	memset(t, 0, sizeof(struct slm_thd));
	*t = (struct slm_thd) {
		.tc = sched_aep->tc,
		.thd = thd,
		.tid = tid,
		.state = SLM_THD_RUNNABLE,
		.priority = TCAP_PRIO_MIN,
		.properties = 0
	};
	ps_list_init(t, event_list);

	return 0;
}

int
slm_thd_init(struct slm_thd *t, thdcap_t thd, thdid_t tid)
{
	int ret;

	if ((ret = slm_thd_init_internal(t, thd, tid))) return ret;
	if ((ret = slm_timer_thd_init(t))) return ret;
	if ((ret = slm_sched_thd_init(t))) return ret;

	return 0;
}

int
slm_thd_deinit(struct slm_thd *t)
{ return 0; }


/*
 * These functions are removed from the inlined fast-paths of the
 * critical section (cs) code to save on code size/locality
 */
int
slm_cs_enter_contention(struct slm_cs *cs, slm_cs_cached_t cached, struct slm_thd *curr, struct slm_thd *owner, int contended, sched_tok_t tok)
{
	struct slm_global *g = slm_global();
	int ret;

	/* Set contention if it isn't yet set */
	if (!contended) {
		if (__slm_cs_cas(cs, cached, owner, 1)) return 1;
	}

	/* Switch to the owner of the critical section, with inheritance using our tcap/priority */
	ret = cos_defswitch(owner->thd, curr->priority, owner == &g->sched_thd ? TCAP_TIME_NIL : g->timeout_next, tok);
	if (ret) return ret;
	/* if we have an outdated token, then we want to use the same repeat loop, so return to that */

	return 1;
}

/* Return 1 if we need a retry, 0 otherwise */
int
slm_cs_exit_contention(struct slm_cs *cs, struct slm_thd *curr, slm_cs_cached_t cached, sched_tok_t tok)
{
	struct slm_thd *s = &slm_global()->sched_thd;
	int ret;

	if (__slm_cs_cas(cs, cached, NULL, 0)) return 1;
	/*
	 * We simplify here by simply switching to the scheduler
	 * thread to let it resolve the situation. Use our priority
	 * for its execution to avoid inversion.
	 */
	ret = cos_defswitch(s->thd, curr->priority, TCAP_TIME_NIL, tok);
	assert(ret != -EINVAL);

	return 0;
}

/***
 * Thread blocking and waking.
 */

/**
 * `slm_thd_wakeup_blked` assumes that `t` is in the `BLOCKED` state,
 * and wakes it up, telling the policy to insert it into the
 * runqueues.
 *
 * - @t - The thread to wakeup
 * - @return - `1` if it's already `RUNNABLE`.
 *             `0` if it was woken up in this call
 */
static inline int
slm_thd_wakeup_blked(struct slm_thd *t)
{
	assert(t);
	assert(slm_thd_normal(t));

	assert(t->state == SLM_THD_BLOCKED);
	t->state = SLM_THD_RUNNABLE;
	slm_sched_wakeup(t);
	t->properties &= ~SLM_THD_PROPERTY_SUSPENDED;

	return 0;
}

/*
 * - @t must be the current thread.
 * - @return: 1 if it's already WOKEN.
 *	    0 if it successfully blocked in this call.
 */
int
slm_thd_block(struct slm_thd *t)
{
	assert(t);
	assert(slm_thd_normal(t));

	if (unlikely(t->state == SLM_THD_WOKEN)) {
		assert(!(t->properties & SLM_THD_PROPERTY_SUSPENDED));
		t->state = SLM_THD_RUNNABLE;

		return 1;
	}
	assert(t->state == SLM_THD_RUNNABLE);

	/*
	 * reset SLM_THD_PROPERTY_SUSPENDED if the scheduler thinks
	 * "curr" was suspended on cos_rcv previously
	 */
	if (t->properties & SLM_THD_PROPERTY_SUSPENDED) {
		t->properties &= ~SLM_THD_PROPERTY_SUSPENDED;
		if (t->state == SLM_THD_BLOCKED) {
			slm_thd_wakeup_blked(t);
		}
	}

	assert(t->state == SLM_THD_RUNNABLE);
	t->state = SLM_THD_BLOCKED;
	slm_sched_block(t);

	return 0;
}

void
slm_thd_block_cs(struct slm_thd *current)
{
	slm_cs_enter(current, SLM_CS_NONE);
	if (slm_thd_block(current)) {
		slm_cs_exit(current, SLM_CS_NONE);
	} else {
		slm_cs_exit_reschedule(current, NULL, SLM_CS_NONE);
	}

	return;
}

/*
 * This API is only used by the scheduling thread to block an AEP thread.
 * AEP thread scheduling events could be redundant.
 *
 * @return: 0 if it successfully blocked in this call.
 */
static inline int
slm_thd_sched_block(struct slm_thd *t)
{
	assert(t);
	assert(slm_state_is_runnable(t->state) || t->state == SLM_THD_BLOCKED);
	assert(slm_thd_normal(t));

	/*
	 * It is oddly possible that the thread is already blocked. It
	 * was previously blocked, yet woke due to an asynchronous
	 * activation. This code can execute *before* the scheduler
	 * thread sees the notification that it had been woken.
	 */
	if (likely(slm_state_is_runnable(t->state))) {
		slm_sched_block(t);
	}
	t->state       = SLM_THD_BLOCKED;
	t->properties |= SLM_THD_PROPERTY_SUSPENDED;

	return 0;
}


/*
 * This API is only used by the scheduling thread to wakeup an AEP thread.
 * AEP thread scheduling events could be redundant.
 *
 * @return: 1 if it's already WOKEN or RUNNABLE.
 *	    0 if it successfully blocked in this call.
 */
int
slm_thd_sched_wakeup(struct slm_thd *t)
{
	assert(t);

	/* not blocked on cos_rcv, so don't mess with user-level thread states */
	if (unlikely(!(t->properties & SLM_THD_PROPERTY_SUSPENDED))) return 1;
	t->properties &= ~SLM_THD_PROPERTY_SUSPENDED;
	/*
	 * If a thread was preempted and scheduler updated it to
	 * RUNNABLE status and if that AEP was activated again
	 * (perhaps by tcap preemption logic) and expired it's budget,
	 * it could result in the scheduler having a redundant WAKEUP
	 * event.
	 *
	 * Thread could be in WOKEN state: Perhaps the thread was
	 * blocked waiting for a lock and was woken up by another
	 * thread and and then scheduler sees some redundant wakeup
	 * event through "asnd" or "tcap budget expiry".
	 */
	if (unlikely(slm_state_is_runnable(t->state))) {
		t->state = SLM_THD_RUNNABLE;

		return 1;
	}

	assert(t->state == SLM_THD_BLOCKED);
	t->state = SLM_THD_RUNNABLE;
	slm_sched_wakeup(t);

	return 0;
}

/**
 * - @redundant - can we have redundant wakeups? Likely this only
 *   makes sense for redundant periodic wakeups.
 */
int
slm_thd_wakeup(struct slm_thd *t, int redundant)
{
	assert(t);

	if (unlikely(t->state == SLM_THD_RUNNABLE || (redundant && t->state == SLM_THD_WOKEN))) {
		/*
		 * We have an odd case. We have a thread that is
		 * runnable (and running!), but is being asked to wake
		 * up. How can this happen? There can be a race
		 * between a thread blocking itself, and another
		 * thread waking it up. If the thread is preempted
		 * before it blocks itself, it can be runnable, but
		 * the other thread can get activated and tries to
		 * wake it.
		 *
		 * t->state == SLM_THD_WOKEN? multiple wakeups?
		 */
		t->state = SLM_THD_WOKEN;

		return 1;
	}
	assert(t->state == SLM_THD_BLOCKED);

	return slm_thd_wakeup_blked(t);
}

void
slm_thd_wakeup_cs(struct slm_thd *curr, struct slm_thd *t)
{
	assert(t);

	slm_cs_enter(curr, SLM_CS_NONE);
	if (slm_thd_wakeup(t, 0)) {
		slm_cs_exit(curr, SLM_CS_NONE);
	} else {
		slm_cs_exit_reschedule(curr, NULL, SLM_CS_NONE);
	}

	return;
}

void
slm_thd_event_reset(struct slm_thd *t)
{
	memset(&t->event_info, 0, sizeof(struct event_info));
}

static inline void
slm_thd_event_enqueue(struct slm_thd *t, int blocked, cycles_t cycles, tcap_time_t timeout)
{
	struct slm_global *g = slm_global();

	if (ps_list_singleton(t, event_list)) ps_list_head_append(&g->event_head, t, event_list);

	t->event_info.blocked = blocked;
	t->event_info.cycles += cycles;
	t->event_info.timeout = timeout;
}

static inline void
slm_thd_event_dequeue(struct slm_thd *t, int *blocked, cycles_t *cycles, tcap_time_t *timeout)
{
	ps_list_rem(t, event_list);

	*blocked = t->event_info.blocked;
	*cycles  = t->event_info.cycles;
	*timeout = t->event_info.timeout;
	slm_thd_event_reset(t);
}

/*
 * `slm_cs_enter_sched` enter into scheduler cs from the scheduler
 * thread context, and if we cannot take the critical section because
 * there are pending scheduler events (for this thread to get from the
 * kernel), then return as such.
 *
 * - @ret: returns -EBUSY if sched thread has events to process and
 *     cannot switch threads, 0 otherwise.
 */
static inline int
slm_cs_enter_sched(void)
{
	int ret;

	while ((ret = slm_cs_enter(&slm_global()->sched_thd, SLM_CS_NOSPIN))) {
		/* We don't want to retry if we have pending events to handle */
		if (ret == -EBUSY) break;
	}

	return ret;
}

/*
 * Do a few things: 1. call schedule to find the next thread to run,
 * 2. release the critical section (note this will cause visual
 * asymmetries in your code if you call slm_cs_enter before this
 * function), and 3. switch to the given thread. It hides some races,
 * and details that would make this difficult to write repetitively.
 *
 * Preconditions: if synchronization is required with code before
 * calling this, you must call slm_cs_enter before-hand (this is likely
 * a typical case).
 *
 * Return: the return value from cos_switch.  The caller must handle
 * this value correctly.
 *
 * A common use-case is:
 *
 * slm_cs_enter(...);
 * scheduling_stuff()
 * slm_cs_exit_reschedule(...);
 *
 * ...which correctly handles any race-conditions on thread selection and
 * dispatch.
 */
int
slm_cs_exit_reschedule(struct slm_thd *curr, struct slm_thd *to, slm_cs_flags_t flags)
{
	struct cos_defcompinfo *dci = cos_defcompinfo_curr_get();
	struct cos_compinfo    *ci  = &dci->ci;
	struct slm_thd         *t;
	struct slm_global      *g   = slm_global();
	sched_tok_t             tok;
	int                     ret;

	tok  = cos_sched_sync();
	if (flags & SLM_CS_CHECK_TIMEOUT && g->timer_set) {
		cycles_t now;
		s64_t    diff;

		now  = slm_now();
		diff = (s64_t)(g->timer_next - now);
		/* Do we need to recompute the timer? */
		if (diff <= 0) {
			g->timer_set = 0;
			/* The timer policy will likely reset the timer */
			slm_timer_expire(now);
		}
	}

	/*
	 * We're going to exit the critical section, then try and
	 * switch to the thread. Given this, once we exit, we can't
	 * trust t's memory as it could be deallocated/modified, so
	 * cache it locally. If these values are out of date, the
	 * scheduler synchronization tok will catch it. This is a
	 * little twitchy and subtle, so lets put it in a function,
	 * here.
	 */
	if (unlikely(to)) {
		t = to;
		if (!slm_state_is_runnable(t->state)) to = NULL;
	}
	if (likely(!to)) {
		t = slm_sched_schedule();
		if (unlikely(!t)) t = &g->idle_thd;
	}

	assert(slm_state_is_runnable(t->state));
	slm_cs_exit(NULL, flags);

	ret = slm_thd_activate(curr, t, tok, 0);
	/* Assuming only the single tcap with infinite budget...should not get EPERM */
	assert(ret != -EPERM);

	return ret;
}

static void
slm_sched_loop_intern(int non_block)
{
	struct slm_global *g = slm_global();
	rcv_flags_t      rfl = (non_block ? RCV_NON_BLOCKING : 0) | RCV_ALL_PENDING;
	struct slm_thd   *us = &g->sched_thd;

	/* Only the scheduler thread should call this function. */
	assert(cos_thdid() == us->tid);

	while (1) {
		int pending;

		do {
			thdid_t        tid;
			int            blocked, rcvd;
			cycles_t       cycles;
			tcap_time_t    thd_timeout;
			struct slm_thd *t = NULL, *tn = NULL;

			/*
			 * Here we retrieve the kernel scheduler
			 * events. These include threads being asnded
			 * (thus activating), rcving (thus
			 * suspending), and generally executing (thus
			 * consuming cycles of computation). This is a
			 * rcv, so it may suspend the calling thread
			 * if `non_block` is `0`.
			 *
			 * Important that this is *not* in the CS due
			 * to the potential blocking.
			 */
			pending = cos_sched_rcv(us->rcv, rfl, g->timeout_next, &rcvd, &tid, &blocked, &cycles, &thd_timeout);
			if (!tid) goto pending_events;

			/*
			 * FIXME: kernel should pass an untyped
			 * pointer back here that we can use instead
			 * of the tid. This is the only place where
			 * slm requires the thread id -> thread
			 * mapping ;-(
			 */
			t = slm_thd_lookup(tid);
			assert(t);
			/* don't report the idle thread or a freed thread */
			if (unlikely(t == &g->idle_thd || t->state == SLM_THD_FREE)) goto pending_events;

			/*
			 * Failure to take the CS because 1. another
			 * thread is holding it and 2. switching to
			 * that thread cannot succeed because
			 * scheduler has pending events which will
			 * prevent the switch to the CS holder. This
			 * can cause the event just received to be
			 * dropped. Thus, to avoid dropping events,
			 * add the events to the scheduler event list
			 * and processing all the pending events after
			 * the scheduler can successfully take the
			 * lock.
			 *
			 * TODO: Better would be to update the kernel
			 * to enable a flag that ignores pending
			 * events on a dispatch request. This would
			 * allow the scheduler thread to switch to the
			 * CS holder, and switch back when the CS
			 * holder releases the CS (thus allowing the
			 * events to be processed at that point.
			 */
			slm_thd_event_enqueue(t, blocked, cycles, thd_timeout);

pending_events:
			/* No events? make a scheduling decision */
			if (ps_list_head_empty(&g->event_head)) break;

			/*
			 * Receiving scheduler notifications is not in critical section mainly for
			 * 1. scheduler thread can often be blocked in rcv, which can add to
			 *    interrupt execution or even AEP thread execution overheads.
			 * 2. scheduler events are not acting on the slm_thd or the policy structures, so
			 *    having finer grained locks around the code that modifies slm_thd states is better.
			 *
			 * Thus we process the events now, with the CS taken.
			 */
			if (slm_cs_enter(us, SLM_CS_SCHEDEVT)) continue;

			ps_list_foreach_del(&g->event_head, t, tn, event_list) {
				/* remove the event from the list and get event info */
				slm_thd_event_dequeue(t, &blocked, &cycles, &thd_timeout);

				/* outdated event for a freed thread */
				if (t->state == SLM_THD_FREE) continue;

				/* Notify the policy that some execution has happened. */
				slm_sched_execution(t, cycles);

				if (blocked) {
					assert(cycles);
					slm_thd_sched_block(t);
				} else {
					slm_thd_sched_wakeup(t);
				}
			}

			slm_cs_exit(us, SLM_CS_NONE);
		} while (pending > 0);

		if (slm_cs_enter_sched()) continue;
		/* If switch returns an inconsistency, we retry anyway */
		if (slm_cs_exit_reschedule(us, NULL, SLM_CS_CHECK_TIMEOUT)) assert(0);
	}
}

void
slm_sched_loop(void)
{
	slm_sched_loop_intern(0);
}

void
slm_sched_loop_nonblock(void)
{
	slm_sched_loop_intern(1);
}

void
slm_idle(void *d)
{
	/* TODO: go through the initialization protocol for the threads... */

	while (1) ;

	BUG();
}

void
slm_init(thdcap_t thd, thdid_t tid)
{
	struct slm_global *g = slm_global();
	struct slm_thd *s    = &g->sched_thd;
	struct slm_thd *i    = &g->idle_thd;
	struct cos_defcompinfo *defci;
	struct cos_aep_info    *sched_aep;

	defci = cos_defcompinfo_curr_get();
	sched_aep = cos_sched_aep_get(defci);

	*s = (struct slm_thd) {
		.properties = 0,
		.state = SLM_THD_RUNNABLE,
		.tc  = sched_aep->tc,
		.thd = sched_aep->thd,
		.tid = sched_aep->tid,
		.rcv = sched_aep->rcv,
		.priority = TCAP_PRIO_MAX
	};
	ps_list_init(s, event_list);
	assert(s->tid == cos_thdid());

	*i = (struct slm_thd) {
		.properties = 0,
		.state = SLM_THD_RUNNABLE,
		.tc = sched_aep->tc,
		.thd = thd,
		.tid = tid,
		.rcv = 0,
		.priority = TCAP_PRIO_MIN
	};
	ps_list_init(i, event_list);

	ps_list_head_init(&g->event_head);
	g->cyc_per_usec = cos_hw_cycles_per_usec(BOOT_CAPTBL_SELF_INITHW_BASE);
	g->lock.owner_contention = 0;

	slm_sched_init();
	slm_timer_init();
}
