#ifndef SLM_API_H
#define SLM_API_H

#include <slm.h>

/***
 * This file encodes the main means of communication that bind the
 * different modules of `slm` together. The core modules are:
 *
 * 1. *slm* - the scheduler "core" logic that provides critical
 *     sections, and coordinates thread state transitions, and
 *     interacts with the nuances of the kernel's API.
 * 2. *timer policy* - which is the timer policy and drives the timeout APIs
 *     of `slm`, and tracks all timeouts, and their association with
 *     threads.
 * 3. *scheduler policy* - which drives the scheduling decisions of
 *     the system by defining the scheduler policy.
 * 4. *slm adapter* - the highest-level API that allocates and tracks
 *     threads, and coordinates the coordination between the previous
 *     three modules. This API bridges the higher level (e.g. `sched`)
 *     API to the slm.
 */

/* These should be defined in their own policy files. */
struct slm_timer_thd;
struct slm_sched_thd;

/***
 * The adapter layer coordinating API must define the following. These
 * functions are used by the various modules to get access to their
 * own structures that should be stored along with the `slm_thd`
 * structure.
 */
struct slm_timer_thd *slm_thd_timer_policy(struct slm_thd *t);
struct slm_sched_thd *slm_thd_sched_policy(struct slm_thd *t);
struct slm_thd *slm_thd_from_timer(struct slm_timer_thd *t);
struct slm_thd *slm_thd_from_sched(struct slm_sched_thd *t);
struct slm_thd *slm_thd_lookup(thdid_t id);

/***
 * This is the timer API. Timer policy must define all of these
 * functions that are used by the `slm` to expire timeouts (via
 * `slm_timer_expire`), and for the adapter to create and cancel
 * timers.
 */

/**
 * This function is triggered by the `slm` core when a timeout
 * expires. This function (and others) will likely want to use the
 * `slm_timeout_set` (and `slm_timeout_clear`) function to set the
 * timer as this value is what will eventually result in this function
 * being called.
 *
 * - @now - the current cycle count, passed here to avoid the costs of
 *          retrieving it from hardware registers (e.g. 30 cycles on
 *          x86)
 */
void slm_timer_expire(cycles_t now);

/**
 * The API that the scheduler uses to interact with slm. Initialize
 * and remove threads, add specific thread timeouts, remove them,
 * etc... `cancel` cancels an active timeout for this thread.
 */
int  slm_timer_thd_init(struct slm_thd *t);
void slm_timer_thd_deinit(struct slm_thd *t);

int slm_timer_add(struct slm_thd *t, cycles_t absolute_cyc);
int slm_timer_cancel(struct slm_thd *t);

int slm_timer_init(void);

/***
 * The API that the scheduling policy must implement.
 *
 * Module specifications:
 *
 * - The thread allocation and indexing interfaces that allocate the
 *   thread representation in memory (thus it should be paired with
 *   the policy if the policy needs additional fields), and converts
 *   between thread ids, and that memory representation.
 * - Scheduling policy hooks to implement the policy, and implements
 *   the thread state machine going through initialization, setting up
 *   parameters (modification), execution, blocking, and waking to
 *   execute again, and removal from the system.
 * - The low-level resource allocation APIs that are either backed by
 *   an in-component mechanism (e.g. crt), or via invocation of other
 *   components that have the privileges to do so.
 *
 * These do not need to be implemented by the same body of code, but
 * do need to be compiled into the binary in some manner.
 */

#include <res_spec.h>

void slm_sched_init(void);
int  slm_sched_thd_init(struct slm_thd *t);
void slm_sched_thd_deinit(struct slm_thd *t);
int  slm_sched_thd_modify(struct slm_thd *t, sched_param_type_t param, unsigned int v);

/* remove the thread from consideration for execution */
int slm_sched_block(struct slm_thd *t);
/* add the thread for consideration for execution */
int slm_sched_wakeup(struct slm_thd *t);
void slm_sched_yield(struct slm_thd *t, struct slm_thd *yield_to);

/*
 * Return the next thread (e.g. highest priority thread) that should
 * execute.
 */
struct slm_thd *slm_sched_schedule(void);
/* Some amount of execution for thread t has elapsed */
void slm_sched_execution(struct slm_thd *t, cycles_t cycles);

#endif	/* SLM_API_H */
