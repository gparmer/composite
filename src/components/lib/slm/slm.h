#ifndef SLM_H
#define SLM_H

#include <cos_component.h>
#include <cos_defkernel_api.h>
#include <ps.h>

/*
 * Simple state machine for each thread
 */
typedef enum {
	SLM_THD_FREE = 0,
	SLM_THD_BLOCKED,
	SLM_THD_WOKEN, /* if a race causes a wakeup before the thread's inevitable block */
	SLM_THD_RUNNABLE,
	SLM_THD_DYING,
} slm_thd_state_t;

static inline int
slm_state_is_runnable(slm_thd_state_t s)
{ return s == SLM_THD_RUNNABLE || s == SLM_THD_WOKEN; }

typedef enum {
	SLM_THD_PROPERTY_OWN_TCAP  = 1,      /* Thread owns a tcap */
	SLM_THD_PROPERTY_SEND      = (1<<1), /* use asnd to dispatch to this thread */
	SLM_THD_PROPERTY_SUSPENDED = (1<<2), /* suspended on a rcv capability? See note below. */
} slm_thd_property_t;

struct event_info {
	int         blocked; /* 1 - blocked. 0 - awake */
	cycles_t    cycles;  /* how many cycles the thread executed */
	tcap_time_t timeout;
};

struct slm_thd {
	/*
	 * rcv_suspended: Tracks the kernel state of the AEP threads for whether they're
	 *                suspended on cos_rcv or not.
	 *		  rcv_suspended is used only for threads that are AEPs (call cos_rcv).
	 *
	 * Activations of these AEP threads cannot be fully controlled by the
	 * scheduler and depends on the global quality of the TCap associated with this
	 * AEP at any point an `asnd` happens to this AEP.
	 *
	 * Therefore, this is really not a thread state that the scheduler controls.
	 * if a thread has rcv_suspended set, it doesn't mean that it isn't running.
	 * But if the thread uses any of `sl` block/yield, this should first be reset and
	 * the thread must be put back to run-queue before doing anything!!
	 *
	 * Another important detail is, when the scheduler receives a "unblocked" event, it
	 * resets this. If rcv_suspended == 0, then the scheduler does not modify the thread states!
	 * This is because, a thread could have run without the scheduler's knowledge through the
	 * tcap mechanism and may have eventually tried to block/acquire a lock/futex
	 * which would then block the thread at user-level. A kernel scheduling event
	 * should not cause it to change to RUNNABLE state and enter a critical section when
	 * it isn't it's turn!
	 *
	 * This is the strongest motivation towards not combining user-level and kernel-level
	 * thread states.
	 *
	 * To sum up:
	 * if rcv_suspended: A thread could "still" be calling block/yield and therefore be
	 *                   in a RUNNABLE/BLOCKED/BLOCKED_TIMEOUT states. It could also be
	 *                   woken up at the user-level if there is another high-prio thread
	 *                   and that gets to run before scheduler is activated up on this
	 *                   thread calls block/yield and it then wakes this thread up.
	 * if !rcv_suspended: A thread could be in any state and also be in cos_rcv.
	 *
	 * The only thing this captures is, "unblocking" a thread from cos_rcv or "blocking" it
	 * on cos_rcv from a scheduler's context. BLOCKing a thread when the scheduler processes
	 * a "blocked" kernel event, clears any prior thread states and sets it to be BLOCKED/BLOCKED_TIMEOUT.
	 */
	slm_thd_property_t properties;
	slm_thd_state_t    state;

	/*
	 * All of the capabilities and kernel-relevant information for
	 * the thread.
	 */
	tcap_t      tc;
	thdcap_t    thd;
	thdid_t     tid;
	arcvcap_t   rcv;
	asndcap_t   asnd;
	tcap_prio_t priority;

	/* Execution information retrieved by the scheduler thread */
	struct event_info event_info;
	struct ps_list    event_list; /* list of events for the scheduler end-point */
};

#include <slm_private.h>

/***
 * The initialization procedure for the slm must include:
 *
 * ```c
 * void
 * cos_init() {
 *         t = thd_alloc(slm_idle, ...); // bypass the slm in allocation
 *         slm_init(t->thdcap, t->thdid);
 *         ...
 * }
 *
 * int
 * main(void) {
 *         ...
 *         slm_sched_loop(); // start processing the scheduler, never return
 * }
 * ```
 */

/* This is the idle function, and should be the function executed by the thread */
void slm_idle(void *);
/**
 * This function *must* be called as part of the scheduler
 * initialization, usually as part of `cos_init`. This assumes that
 * the calling thread will become the main scheduling thread, and that
 * it was created with the `defkernel` APIs.
 *
 * - @thd - a thread capability to the *idle thread* that is executing
 *     the `slm_idle` function
 * - @tid - the thread id of the idle thread
 */
void slm_init(thdcap_t thd, thdid_t tid);
/*
 * The initialization thread must execute this (post `slm_init`), and
 * this thread will become the scheduler notification thread that
 * polls the rcv end-point for other thread's activations and
 * suspensions.
 */
void slm_sched_loop(void);
void slm_sched_loop_nonblock(void);

int slm_thd_init(struct slm_thd *t, thdcap_t thd, thdid_t tid);

typedef enum {
	SLM_CS_NONE     = 0,
	SLM_CS_NOSPIN   = 1, /* return if we race with another thread */
	SLM_CS_SWITCHTO = 2, /* we should try and switch to the `switchto` argument */
	SLM_CS_SCHEDEVT = 4, /* return if there are pending scheduler notifications */
	SLM_CS_CHECK_TIMEOUT = 8, /* should we check for pending timeouts on exit? */
} slm_cs_flags_t;

/**
 * Try to enter into the critical section. There are few ways that
 * this can play out.
 *
 * 1. The lock is not owned by anyone, so we can take it! Use an
 *     atomic instruction to update the value.
 *
 * 2. The lock is owned by another thread. We will try and switch
 *     directly to that thread. This is subtly powerful: it enables us
 *     to implement priority inheritance *regardless which scheduling
 *     policy is used*.
 *
 * 3. We don't see someone owning the lock, *but* we race on updating
 *     the lock to us as the owner, and the other thread wins. Instead
 *     of trying to do option 2 here we return an error so that the
 *     surrounding context can manage the "retries".
 *
 * - @current - The currently active thread that is calling this
 *     function! Note that the liveness is awkward here. How do we
 *     know if the thread's structure is live at this point? Simple:
 *     we're currently executing the thread, so we simply need to
 *     ensure that the `slm_thd` for a thread is live as long as the
 *     thread is live...something that is fairly natural.

 * - @ret - `0` if we have the critical section, `1` if there was a
 *     race on setting the lock, and we lost, `-N` for errors on the
 *     kernel call to switch threads, including `-EAGAIN` if there was
 *     a race on the scheduler token, and `-EBUSY` if we are a
 *     scheduling thread, and there are pending scheduler events.
 */
static inline int
slm_cs_enter(struct slm_thd *current, slm_cs_flags_t flags)
{
	struct slm_cs  *cs;
	slm_cs_cached_t cached;
	sched_tok_t     tok;
	struct slm_thd  *owner;
	int             contended;

	assert(current);
	cs = &(slm_global()->lock);

	while (1) {
		tok    = cos_sched_sync();
		cached = __slm_cs_data(cs, &owner, &contended);

		if (unlikely(owner)) {
			int ret;

			ret = slm_cs_enter_contention(cs, cached, current, owner, contended, tok);
			/* does the caller want to be notified of pending scheduler events? */
			if (flags & SLM_CS_SCHEDEVT && ret == -EBUSY) return ret;
			assert(ret != -EBUSY); /* no scheduler thread should not see these events */

			if (flags & SLM_CS_NOSPIN) return ret;

			continue;
		}

		/* success! common case */
		if (likely(!__slm_cs_cas(cs, cached, current, 0))) return 0;
		if (flags & SLM_CS_NOSPIN) return 1;
	}
}

/*
 * Release the scheduler critical section, switch to the scheduler
 * thread if there is pending contention.
 *
 * Assume: the owner of the critical section is the same thread that
 * calls `slm_cs_exit`.
 */
static inline void
slm_cs_exit(struct slm_thd *switchto, slm_cs_flags_t flags)
{
	int ret = 1;
	struct slm_cs *cs = &(slm_global()->lock);

	while (ret != 0) {
		int             contention;
		sched_tok_t     tok;
		slm_cs_cached_t cached;
		struct slm_thd *current;

		tok    = cos_sched_sync();
		cached = __slm_cs_data(cs, &current, &contention);
		/* Another thread attempted to enter the critical section */
		if (unlikely(contention)) {
			if (!slm_cs_exit_contention(cs, current, cached, tok)) return;

			continue; /* we woke up, try again */
		}

		/* The common case: release lock, no-one waiting for it */
		ret = __slm_cs_cas(cs, cached, 0, 0);
		if (flags & SLM_CS_NOSPIN) return;
		if (flags & SLM_CS_SCHEDEVT && ret == -EBUSY) return;
	}

	return;
}

int slm_cs_exit_reschedule(struct slm_thd *curr, struct slm_thd *switch_to, slm_cs_flags_t flags);

/**
 * `slm_switch_to` attempts to perform scheduler bypass and switch
 * directly to the specified thread. This is pretty unsafe, in
 * general. There is no critical section, and it bypasses all
 * scheduling decisions. As we bypass scheduling decisions, we must
 * decide with which priority we want to execute: the current
 * thread's, or the next's. Thus, the `inherit_prio` value selects if
 * the switched-to thread inherits this thread's priority.
 *
 * This function is mainly useful in defining scheduling fast-paths
 * for communication between different threads. Though Composite IPC
 * is very fast, IPC between threads has the overhead of component
 * invocation, scheduling, and dispatch. Slite gets rid of the latter,
 * and this effectively gets rid of the scheduling overhead.
 *
 * *Synchronization*: `tok` is passed as an argument enabling some
 * limited synchronization as the surrounding context can access
 * data-structures after taken the token, and the thread switch will
 * be prevented if context switches happened in the mean-time.
 *
 * - `curr` - the current thread, only used when inheriting priority
 * - `to` - thread to switch to
 * - `tok` - the scheduler token, passed from the caller
 * - `inherit_prio` - should `to` inherit `curr`'s priority?
 */
static inline int
slm_switch_to(struct slm_thd *curr, struct slm_thd *to, sched_tok_t tok, int inherit_prio)
{
	if (to->state != SLM_THD_RUNNABLE) return 1;

	return slm_thd_activate(curr, to, tok, inherit_prio);
}

int slm_thd_block(struct slm_thd *t);
int slm_thd_wakeup(struct slm_thd *t, int redundant);

/***
 * The `slm` time API. Unfortunately, three times are used in the
 * system:
 *
 * 1. cycles which are the finest-granularity, and are accessed with
 *    the least overhead (using direct instructions).
 * 2. microseconds (usec), which are an intuitive time unit with which
 *    users specify time. Relatively expensive to convert to and from
 *    (requiring general multiplication and division.
 * 3. tcap "ticks" which are some multiple of a cycle which are quick
 *    to convert to and from.
 *
 * `slm` simply tries to hide tcap times, and interfaces with timer
 * policy using only cycles. The conversion functions to and from
 * microseconds are provided here for that policy to use, should it
 * need it.
 */

static inline cycles_t
slm_now(void)
{
	return ps_tsc();
}

static inline microsec_t
slm_cyc2usec(cycles_t cyc)
{
	return cyc / slm_global()->cyc_per_usec;
}

static inline cycles_t
slm_usec2cyc(microsec_t usec)
{
	return usec * slm_global()->cyc_per_usec;
}

/***
 * Simple timeout API to enable the timer module to set and remove
 * timeouts. Must be used with the scheduler's critical section taken.
 */
static inline void
slm_timeout_set(cycles_t timeout)
{
	struct slm_global *g = slm_global();

	g->timeout_next = tcap_cyc2time(timeout);
	g->timer_next   = timeout;
	g->timer_set    = 1;
}

static inline void
slm_timeout_clear(void)
{
	slm_global()->timer_set = 0;
}

#endif	/* SLM_H */
