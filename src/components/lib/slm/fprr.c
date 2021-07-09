#include <slm.h>
#include <fprr.h>
#include <slm_api.h>
#include <cos_types.h>
#include <cos_component.h>

#define SLM_FPRR_NPRIOS         32
#define SLM_FPRR_PRIO_HIGHEST   TCAP_PRIO_MAX
#define SLM_FPRR_PRIO_LOWEST    SLM_FPRR_NPRIOS

#define SLM_FPRR_PERIOD_US_MIN  10000

struct ps_list_head threads[NUM_CPU][SLM_FPRR_NPRIOS] CACHE_ALIGNED;

/* No RR yet */
void
slm_sched_execution(struct slm_thd *t, cycles_t cycles)
{ return; }

struct slm_thd *
slm_sched_schedule(void)
{
	int i;
	struct slm_sched_thd *t;
	struct ps_list_head *prios = threads[cos_cpuid()];

	for (i = 0 ; i < SLM_FPRR_NPRIOS ; i++) {
		if (ps_list_head_empty(&prios[i])) continue;
		t = ps_list_head_first_d(&prios[i], struct slm_sched_thd);

		/*
		 * We want to move the selected thread to the back of the list.
		 * Otherwise fprr won't be truly round robin
		 */
		ps_list_rem_d(t);
		ps_list_head_append_d(&prios[i], t);

		return slm_thd_from_sched(t);
	}

	return NULL;
}

int
slm_sched_block(struct slm_thd *t)
{
	struct slm_sched_thd *p = slm_thd_sched_policy(t);

	ps_list_rem_d(p);

	return 0;
}

int
slm_sched_wakeup(struct slm_thd *t)
{
	struct slm_sched_thd *p = slm_thd_sched_policy(t);

	assert(ps_list_singleton_d(p));

	ps_list_head_append_d(&threads[cos_cpuid()][t->priority - 1], p);

	return 0;
}

void
slm_sched_yield(struct slm_thd *t, struct slm_thd *yield_to)
{
	struct slm_sched_thd *p = slm_thd_sched_policy(t);

	ps_list_rem_d(p);
	ps_list_head_append_d(&threads[cos_cpuid()][t->priority - 1], p);
}

int
slm_sched_thd_init(struct slm_thd *t)
{
	t->priority = SLM_FPRR_PRIO_LOWEST;
	ps_list_init_d(slm_thd_sched_policy(t));

	return 0;
}

void
slm_sched_thd_deinit(struct slm_thd *t)
{ ps_list_rem_d(slm_thd_sched_policy(t)); }

int
slm_sched_thd_modify(struct slm_thd *t, sched_param_type_t type, unsigned int v)
{
	struct slm_sched_thd *p = slm_thd_sched_policy(t);

	switch (type) {
	case SCHEDP_PRIO:
	{
		assert(v >= SLM_FPRR_PRIO_HIGHEST && v <= SLM_FPRR_PRIO_LOWEST);
		ps_list_rem_d(p); /* if we're already on a list, and we're updating priority */
		t->priority = v;
		ps_list_head_append_d(&threads[cos_cpuid()][t->priority - 1], p);

		break;
	}
	/* Only support priority, for now */
	default: assert(0);
	}

	return 0;
}

void
slm_sched_init(void)
{
	int i;

	memset(threads[cos_cpuid()], 0, sizeof(struct ps_list_head) * SLM_FPRR_NPRIOS);
	for (i = 0 ; i < SLM_FPRR_NPRIOS ; i++) {
		ps_list_head_init(&threads[cos_cpuid()][i]);
	}
}
