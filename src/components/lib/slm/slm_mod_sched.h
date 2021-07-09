#ifndef SLM_POLICY_H
#define SLM_POLICY_H

/***
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

/* Thread allocation and indexing API */

struct slm_thd *slm_policy_thd_alloc(void);
int slm_policy_thd_dealloc(struct slm_thd *);
struct slm_thd *slm_policy_thd_lookup(thdid_t id);

/* Scheduling policy APIs */

#include <res_spec.h>

int slm_policy_block(struct slm_thd *t);
int slm_policy_wakeup(struct slm_thd *t);
struct slm_thd *slm_policy_schedule(void);

int slm_policy_thd_add(struct slm_thd *t);
int slm_policy_thd_modify(struct slm_thd *t, sched_param_t param);

#endif	/* SLM_POLICY_H */
