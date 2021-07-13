/**
 * Copyright 2012 by Qi Wang, interwq@gwu.edu; Gabriel Parmer,
 * gparmer@gwu.edu

 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 */

#ifndef CPUID_H
#define CPUID_H

#include "../../../kernel/include/shared/consts.h"
#include "../../../kernel/include/shared/cos_types.h"
#include "../../../kernel/include/asm_ipc_defs.h"
#include "../../../kernel/include/list.h"
#include "../chal_asm_inc.h"

static inline cycles_t
tsc(void)
{
	unsigned long long ret;

	__asm__ __volatile__("rdtsc" : "=A"(ret));

	return ret;
}

/*
 * This is the data structure embedded in the cos_cpu_local_info (next_ti) that
 * contains information for the thread that was either preempted or woken up
 * and is eligible to be scheduled instead of the current thread's scheduler
 * upon RCV syscall. This is mainly to reduce the number of context switches
 * to schedule the thread that is deemed eligible by the scheduler.
 */
struct next_thdinfo {
	void *      thd;
	void *      tc;
	tcap_prio_t prio;
	tcap_res_t  budget;
};

struct cos_cpu_local_info {
	/*
	 * orig_sysenter_esp SHOULD be the first variable here. The
	 * sysenter interposition path gets tss from it.
	 */
	unsigned long *orig_sysenter_esp; /* Points to the end of the tss struct in Linux. */
	/***********************************************/
	/* info saved in kernel stack for fast access. */
	unsigned long cpuid;
	void *        curr_thd;
	void *        curr_tcap;
	struct list   tcaps;
	tcap_uid_t    tcap_uid;
	tcap_prio_t   tcap_prio;
	cycles_t      cycles;
	cycles_t      next_timer;
	/*
	 * cache the stk_top index to save a cacheline access on
	 * inv/ret. Could use a struct here if need to cache multiple
	 * things. (e.g. captbl, etc)
	 */
	int           invstk_top;
	unsigned long epoch;
	/***********************************************/
	/*
	 * Since this struct resides at the lowest address of the
	 * kernel stack page (along with thread_info struct of
	 * Linux), we store 0xDEADBEEF to detect overflow.
	 */
	unsigned long overflow_check;
	/* next - preempted/awoken thread information */
	struct next_thdinfo next_ti;
};

static inline struct cos_cpu_local_info *
cos_cpu_local_info(void)
{
	int local;

	return (struct cos_cpu_local_info *)(round_up_to_page((unsigned long)&local) - STK_INFO_OFF);
}

static inline int
get_cpuid(void)
{
#if NUM_CPU == 1
	return 0;
#endif

	return cos_cpu_local_info()->cpuid;
}

#endif /* CPUID_H */
