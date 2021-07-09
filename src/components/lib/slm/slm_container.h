#ifndef SLM_CONTAINER_H
#define SLM_CONTAINER_H

/**
 * This file is only to be included *after* you include the
 * definitions of the all three types of blocks. This simply enables
 * indexing and conversion between the different aspects of the
 * thread. In short, this file is *not* self-contained, thus in no way
 * useful on its own.
 */

struct slm_thd_container {
	struct slm_thd thd;
	struct slm_sched_thd sched;
	struct slm_timer_thd timer;
	struct crt_thd resources;
};

struct slm_timer_thd *
slm_thd_timer_policy(struct slm_thd *t)
{
	return ps_container(t, struct slm_thd_container, thd)->timer;
}

struct slm_sched_thd *
slm_thd_sched_policy(struct slm_thd *t)
{
	return ps_container(t, struct slm_thd_container, thd)->sched;
}

struct slm_thd *
slm_thd_from_timer(struct slm_timer_thd *t)
{
	return ps_container(t, struct slm_thd_container, timer)->thd;
}

struct slm_thd *
slm_thd_from_sched(struct slm_sched_thd *t)
{
	return ps_container(t, struct slm_thd_container, sched)->thd;
}

#endif
