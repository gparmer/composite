/**
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * Copyright 2018, The George Washington University
 * Author: Phani Gadepalli, phanikishoreg@gwu.edu
 */

#include <sl.h>
#include <res_spec.h>
#include <hypercall.h>
#include <sched_info.h>

#define FIXED_PRIO 1

u32_t cycs_per_usec = 0;

/* using raw kernel api. this api from capmgr cannot be linked to or used */
thdcap_t
capmgr_thd_retrieve_next(spdid_t child, thdid_t *tid)
{
	assert(0);
}

void
sched_child_init(struct sched_childinfo *schedci)
{
	assert(schedci);

	schedci->initthd = sl_thd_initaep_alloc_dcb(sched_child_defci_get(schedci), NULL, schedci->flags & COMP_FLAG_SCHED, schedci->flags & COMP_FLAG_SCHED ? 1 : 0, 0, 0, 0, 0);

	assert(schedci->initthd);
	sl_thd_param_set(schedci->initthd, sched_param_pack(SCHEDP_PRIO, FIXED_PRIO));
}

thdid_t
sched_child_thd_create(struct sched_childinfo *schedci, thdclosure_index_t idx)
{
	struct sl_thd *t = sl_thd_aep_alloc_ext_dcb(sched_child_defci_get(schedci), NULL, idx, 0, 0, 0, 0, 0, 0, 0, NULL);

	return t ? sl_thd_thdid(t) : 0;
}

thdid_t
sched_child_aep_create(struct sched_childinfo *schedci, thdclosure_index_t idx, int owntc, cos_channelkey_t key, microsec_t ipiwin, u32_t ipimax, arcvcap_t *extrcv)
{
	struct sl_thd *t = sl_thd_aep_alloc_ext_dcb(sched_child_defci_get(schedci), NULL, idx, 1, owntc, key, 0, 0, 0, 0, extrcv);

	return t ? sl_thd_thdid(t) : 0;
}

void
cos_init(void)
{
	struct cos_defcompinfo *defci = cos_defcompinfo_curr_get();
	struct cos_compinfo    *ci    = cos_compinfo_get(defci);
	static unsigned long first = NUM_CPU + 1, init_done[NUM_CPU] = { 0 };
	static u32_t cpubmp[NUM_CPU_BMP_WORDS] = { 0 };
	int i;

	PRINTLOG(PRINT_DEBUG, "CPU cycles per sec: %u\n", cos_hw_cycles_per_usec(BOOT_CAPTBL_SELF_INITHW_BASE));

	if (ps_cas(&first, NUM_CPU + 1, cos_cpuid())) {
		cos_meminfo_init(&(ci->mi), BOOT_MEM_KM_BASE, COS_MEM_KERN_PA_SZ, BOOT_CAPTBL_SELF_UNTYPED_PT);
		cos_defcompinfo_init();
		cos_init_args_cpubmp(cpubmp);
	} else {
		while (!ps_load(&init_done[first])) ;

		cos_defcompinfo_sched_init();
	}
	ps_faa(&init_done[cos_cpuid()], 1);

	/* make sure the INITTHD of the scheduler is created on all cores.. for cross-core sl initialization to work! */
	for (i = 0; i < NUM_CPU; i++) {
		if (!bitmap_check(cpubmp, i)) continue;

		while (!ps_load(&init_done[i])) ;
	}

	sl_init_corebmp(SL_MIN_PERIOD_US, cpubmp);
	sched_childinfo_init_raw();
	hypercall_comp_init_done();

	sl_sched_loop_nonblock();

	PRINTLOG(PRINT_ERROR, "Should never have reached this point!!!\n");
	assert(0);
}
