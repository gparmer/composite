/**
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * Copyright 2019, The George Washington University
 * Author: Gabriel Parmer, gparmer@gwu.edu
 */

#include <stdlib.h>
#include <limits.h>

#include <initargs.h>
#include <barrier.h>
#include <cos_kernel_api.h>
#include <cos_defkernel_api.h>
#include <crt.h>
#include <static_slab.h>

#include <init.h>
#include <addr.h>

#ifndef BOOTER_MAX_SINV
#define BOOTER_MAX_SINV 256
#endif
#ifndef BOOTER_MAX_SCHED
#define BOOTER_MAX_SCHED 1
#endif
#ifndef BOOTER_MAX_INITTHD
#define BOOTER_MAX_INITTHD (MAX_NUM_COMPS - BOOTER_MAX_SCHED)
#endif
#ifndef INITARGS_MAX_PATHNAME
#define INITARGS_MAX_PATHNAME 512
#endif
#ifndef BOOTER_CAPMGR_MB
#define BOOTER_CAPMGR_MB 64
#endif
#ifndef BOOTER_MAX_CHKPT
#define BOOTER_MAX_CHKPT 64
#endif

/* UNCOMMENT HERE FOR CHECKPOINT FUNCTIONALITY */
/* #ifndef ENABLE_CHKPT
 * #define ENABLE_CHKPT 1
 * #endif
 */

static struct crt_comp boot_comps[MAX_NUM_COMPS];
static const  compid_t sched_root_id  = 2;
static        long     boot_id_offset = -1;

SS_STATIC_SLAB(sinv,   struct crt_sinv,   BOOTER_MAX_SINV);
SS_STATIC_SLAB(thd,    struct crt_thd,    BOOTER_MAX_INITTHD);
SS_STATIC_SLAB(rcv,    struct crt_rcv,    BOOTER_MAX_SCHED);
SS_STATIC_SLAB(chkpt,  struct crt_chkpt,  BOOTER_MAX_CHKPT);

/*
 * Assumptions: the component with the lowest id *must* be the one
 * that is passed into this function first. You *can* pass in an id
 * that is higher than we have components. In that case, this will
 * return NULL.
 */
static struct crt_comp *
boot_comp_get(compid_t id)
{
	assert(id > 0 && id <= MAX_NUM_COMPS);

	if (boot_id_offset == -1) {
		boot_id_offset = id;
	}
	/* casts are OK as we know that boot_id_offset is > 0 now */
	assert((compid_t)boot_id_offset <= id);
	assert(boot_id_offset >= 0 && id >= (compid_t)boot_id_offset);

	return &boot_comps[id - boot_id_offset];
}

static struct crt_comp *
boot_comp_self(void)
{
	return boot_comp_get(cos_compid());
}

static void
boot_comp_set_idoffset(int off)
{
	boot_id_offset = off;
}

static void
comps_init(void)
{
	struct initargs comps, curr;
	struct initargs_iter i;
	int cont, ret, j;
	int comp_idx = 0;

	/*
	 * Assume: our component id is the lowest of the ids for all
	 * components we are set to create, and that we get it from
	 * mkimg.
	 */
	if (cos_compid_uninitialized()) {
		int booter_id;

		booter_id = atoi(args_get("compid"));
		cos_compid_set(booter_id);
	}
	boot_comp_set_idoffset(cos_compid());

	ret = args_get_entry("components", &comps);
	assert(!ret);
	printc("Components (%d):\n", args_len(&comps));
	for (cont = args_iter(&comps, &i, &curr) ; cont ; cont = args_iter_next(&i, &curr)) {
		struct crt_comp *comp;
		void *elf_hdr;
		int   keylen;
		compid_t id = atoi(args_key(&curr, &keylen));
		char *name  = args_get_from("img", &curr);
		vaddr_t info = atol(args_get_from("info", &curr));
		const char *root = "binaries/";
		int   len  = strlen(root);
		char  path[INITARGS_MAX_PATHNAME];

		printc("%s: %lu\n", name, id);

		assert(id < MAX_NUM_COMPS && id > 0 && name);

		memset(path, 0, INITARGS_MAX_PATHNAME);
		strncat(path, root, len + 1);
		assert(path[len] == '\0');
		strncat(path, name, INITARGS_MAX_PATHNAME - len);
		assert(path[INITARGS_MAX_PATHNAME - 1] == '\0'); /* no truncation allowed */

		comp = boot_comp_get(id);
		assert(comp);
		elf_hdr = (void *)args_get(path);

		if (id == cos_compid()) {
			int ret;

			/* booter should not have an elf object */
			assert(!elf_hdr);
			ret = crt_booter_create(comp, name, id, info);
			assert(ret == 0);
		} else {
			assert(elf_hdr);
			if (crt_comp_create(comp, name, id, elf_hdr, info)) {
				printc("Error constructing the resource tables and image of component %s.\n", comp->name);
				BUG();
			}	
		}
		assert(comp->refcnt != 0);
	}

	ret = args_get_entry("execute", &comps);
	assert(!ret);
	printc("Execution schedule:\n");
	for (cont = args_iter(&comps, &i, &curr) ; cont ; cont = args_iter_next(&i, &curr)) {
		struct crt_comp     *comp;
		int      keylen;
		compid_t id        = atoi(args_key(&curr, &keylen));
		char    *exec_type = args_value(&curr);
		struct crt_comp_exec_context ctxt = { 0 };

		assert(exec_type);
		assert(id != cos_compid());
		comp = boot_comp_get(id);
		assert(comp);

		if (!strcmp(exec_type, "sched")) {
			struct crt_rcv *r = ss_rcv_alloc();

			assert(r);
			if (crt_comp_exec(comp, crt_comp_exec_sched_init(&ctxt, r))) BUG();
			ss_rcv_activate(r);
			printc("\tCreated scheduling execution for %ld\n", id);
		} else if (!strcmp(exec_type, "init")) {
			struct crt_thd *t = ss_thd_alloc();

			assert(t);
			if (crt_comp_exec(comp, crt_comp_exec_thd_init(&ctxt, t))) BUG();
			ss_thd_activate(t);
			printc("\tCreated thread for %ld\n", id);
		} else {
			printc("Error: Found unknown execution schedule type %s.\n", exec_type);
			BUG();
		}

		comp->init_state = CRT_COMP_INIT_COS_INIT;
	}

	/* perform any necessary captbl delegations */
	ret = args_get_entry("captbl_delegations", &comps);
	assert(!ret);
	printc("Capability table delegations (%d capability managers):\n", args_len(&comps));
	/*
	 * for now, assume only one capmgr (allocating untyped memory
	 * gets complex here otherwise)
	 */
	for (cont = args_iter(&comps, &i, &curr) ; cont ; cont = args_iter_next(&i, &curr)) {
		struct crt_comp *c;
		struct initargs curr_inner;
		struct initargs_iter i_inner;
		int keylen, cont2;
		compid_t capmgr_id;
		struct crt_comp_resources comp_res = { 0 };
		crt_comp_alias_t alias_flags = 0;
		struct crt_comp *target = NULL;

		capmgr_id = atoi(args_key(&curr, &keylen));
		c = boot_comp_get(capmgr_id);
		assert(c);

		printc("\tCapmgr %ld:\n", capmgr_id);
		/* This assumes that all capabilities for a given component are *contiguous* */
		for (cont2 = args_iter(&curr, &i_inner, &curr_inner) ; cont2 ; cont2 = args_iter_next(&i_inner, &curr_inner)) {
			char    *type      = args_get_from("type", &curr_inner);
			capid_t  capno     = atoi(args_key(&curr_inner, &keylen));
			compid_t target_id = atoi(args_get_from("target", &curr_inner));

			/* If we've moved on to the next component, commit the changes for the previous */
			if (target && target != boot_comp_get(target_id)) {
				if (crt_comp_alias_in(target, c, &comp_res, alias_flags)) BUG();

				memset(&comp_res, 0, sizeof(struct crt_comp_resources));
				alias_flags = 0;
			}
			target = boot_comp_get(target_id);
			assert(target);

			printc("\t\tCapability #%ld: %s for component %ld\n", capno, type, target_id);
			if (!strcmp(type, "pgtbl")) {
				comp_res.ptc = capno;
				alias_flags |= CRT_COMP_ALIAS_PGTBL;
			} else if (!strcmp(type, "captbl")) {
				comp_res.ctc = capno;
				alias_flags |= CRT_COMP_ALIAS_CAPTBL;
			} else if (!strcmp(type, "comp")) {
				comp_res.compc = capno;
				alias_flags |= CRT_COMP_ALIAS_COMP;
			} else {
				BUG();
			}
		}
		if (crt_comp_alias_in(target, c, &comp_res, alias_flags)) BUG();
	}

	/*
	 * *No static capability slot allocations after this point.*
	 *
	 * Past this point, dynamic allocations are made into the
	 * capability tables of the components. Thus any statically
	 * allocated capability ids can be disrupted by these
	 * bump-pointer allocations if they are used past this
	 * point. All of the capability manager static capabilities
	 * should be done already.
	 *
	 * The exception is that capability ids under BOOT_CAPTLB_FREE
	 * are reserved as is, so they can be allocated to regardless.
	 */

	/*
	 * Create the synchronous invocations for the component. This
	 * has to go *after* the updating of the capability frontier
	 * for the hard-coded capabilities as we don't want to use up
	 * those slots for the synchronous invocations.
	 */
	ret = args_get_entry("sinvs", &comps);
	assert(!ret);
	printc("Synchronous invocations (%d):\n", args_len(&comps));
	for (cont = args_iter(&comps, &i, &curr) ; cont ; cont = args_iter_next(&i, &curr)) {
		struct crt_sinv *sinv;
		int serv_id = atoi(args_get_from("server", &curr));
		int cli_id  = atoi(args_get_from("client", &curr));
		struct crt_comp *serv = boot_comp_get(serv_id);
		struct crt_comp *cli = boot_comp_get(cli_id);

		sinv = ss_sinv_alloc();
		assert(sinv);
		crt_sinv_create(sinv, args_get_from("name", &curr), boot_comp_get(serv_id), boot_comp_get(cli_id),
				strtoul(args_get_from("c_fn_addr", &curr), NULL, 10), strtoul(args_get_from("c_ucap_addr", &curr), NULL, 10),
				strtoul(args_get_from("s_fn_addr", &curr), NULL, 10));
		ss_sinv_activate(sinv);
		printc("\t%s (%lu->%lu):\tclient_fn @ 0x%lx, client_ucap @ 0x%lx, server_fn @ 0x%lx\n",
		       sinv->name, sinv->client->id, sinv->server->id, sinv->c_fn_addr, sinv->c_ucap_addr, sinv->s_fn_addr);
	#ifdef ENABLE_CHKPT
		assert(serv->n_sinvs < CRT_COMP_SINVS_LEN);
		serv->sinvs[serv->n_sinvs] = *sinv;
		serv->n_sinvs++;
		assert(cli->n_sinvs < CRT_COMP_SINVS_LEN);
		cli->sinvs[cli->n_sinvs] = *sinv;
		cli->n_sinvs++;
	#endif /* ENABLE_CHKPT */
	}
	
	/*
	 * Delegate the untyped memory to the capmgr. This should go
	 * *after* all allocations that use untyped memory, so that we
	 * can delegate away the rest of our memory. FIXME: this might
	 * not be the cause currently, and we rely on a few untyped
	 * regions (after the ones we delegate to the capmgr) for the
	 * parallel thread allocations.
	 */
	ret = args_get_entry("captbl_delegations", &comps);
	assert(!ret);
	for (cont = args_iter(&comps, &i, &curr) ; cont ; cont = args_iter_next(&i, &curr)) {
		struct crt_comp *c;
		int keylen;
		struct crt_comp_exec_context ctxt = { 0 };

		c = boot_comp_get(atoi(args_key(&curr, &keylen)));
		assert(c);

		/* TODO: generalize. Give the capmgr 64MB for now. */
		if (crt_comp_exec(c, crt_comp_exec_capmgr_init(&ctxt, BOOTER_CAPMGR_MB * 1024 * 1024))) BUG();
	}

	printc("Kernel resources created, booting components!\n");

	return;
}

/*
 * We only support a single checkpoint directly above the existing components.
 * At this point we assume capability managers and schedulers will not be checkpointed
 */
void 
chkpt_comp_init(struct crt_comp *comp, struct crt_chkpt *chkpt, char *name)
{
#ifdef ENABLE_CHKPT
	/* create the component */
	void *elf_hdr;
	int   keylen;
	compid_t id;
	const char *root = "binaries/";
	int   len  = strlen(root);
	char  path[INITARGS_MAX_PATHNAME];
	struct crt_comp_exec_context ctxt = { 0 };
	struct crt_thd *t;

	id = crt_ncomp() + 1;
	assert(id < MAX_NUM_COMPS && id > 0 && name);

	assert(len < INITARGS_MAX_PATHNAME);
	memset(path, 0, INITARGS_MAX_PATHNAME);
	strncat(path, root, len);
	assert(path[len] == '\0');
	strncat(path, name, INITARGS_MAX_PATHNAME - len);
	assert(path[INITARGS_MAX_PATHNAME - 1] == '\0'); /* no truncation allowed */

	if (id == cos_compid()) {
		/* this should never happen */
		assert(0);
	} else {
		if (crt_comp_create_from(comp, name, id, chkpt)) {
			printc("Error constructing the resource tables and image of component %s.\n", comp->name);
			BUG();
		}
	}
	assert(comp->refcnt != 0);

	t = ss_thd_alloc();
	assert(t);

	if (crt_comp_exec(comp, crt_comp_exec_thd_init(&ctxt, t))) BUG();
	ss_thd_activate(t);
	comp->init_state = CRT_COMP_INIT_COS_INIT;

	/* create the sinvs */
	for (u32_t i = 0 ; i < comp->n_sinvs ; i++) {
		struct crt_sinv *sinv;
		int serv_id = comp->sinvs[i].server->id;
		int cli_id  = comp->sinvs[i].client->id;

		sinv = ss_sinv_alloc();
		assert(sinv);
		crt_sinv_create(sinv, comp->sinvs[i].name, comp->sinvs[i].server, comp->sinvs[i].client,
			comp->sinvs[i].c_fn_addr, comp->sinvs[i].c_ucap_addr, comp->sinvs[i].s_fn_addr);
		ss_sinv_activate(sinv);
		printc("\t(chkpt) sinv: %s (%lu->%lu):\tclient_fn @ 0x%lx, client_ucap @ 0x%lx, server_fn @ 0x%lx\n",
			sinv->name, sinv->client->id, sinv->server->id, sinv->c_fn_addr, sinv->c_ucap_addr, sinv->s_fn_addr);	
	}
#endif /* ENABLE_CHKPT */

}

unsigned long
addr_get(compid_t id, addr_t type)
{
	compid_t client = (compid_t)cos_inv_token();
	struct crt_comp *c, *target;
	struct cos_compinfo *ci;

	c = boot_comp_get(client);
	/* only capmgrs should be allowed to call this... */
	assert(c);
	if (!(c->flags & CRT_COMP_CAPMGR)) return 0;

	if (id <= 0 || client > MAX_NUM_COMPS) return 0;
	target = boot_comp_get(id);
	assert(target);

	ci = cos_compinfo_get(target->comp_res);

	switch (type) {
	case ADDR_CAPTBL_FRONTIER:
		return ci->cap_frontier;
	case ADDR_HEAP_FRONTIER:
		return ci->vas_frontier;
	default:
		return 0;
	}
}

static void
booter_init(void)
{
	struct cos_compinfo *boot_info = cos_compinfo_get(cos_defcompinfo_curr_get());

	cos_meminfo_init(&(boot_info->mi), BOOT_MEM_KM_BASE, COS_MEM_KERN_PA_SZ, BOOT_CAPTBL_SELF_UNTYPED_PT);
	cos_defcompinfo_init();

	cos_hw_cycles_per_usec(BOOT_CAPTBL_SELF_INITHW_BASE);
}

void
execute(void)
{
	crt_compinit_execute(boot_comp_get);
}

void
init_done_chkpt(struct crt_comp *c)
{
	struct crt_comp  *new_comp = boot_comp_get(crt_ncomp() + 1);
	struct crt_chkpt *chkpt;
	thdcap_t          thdcap;
	int               ret;
	char              name[INITARGS_MAX_PATHNAME];
	char             *prefix = "chkpt_";
	int               prefix_sz = strlen("chkpt_");

	if (c->id == cos_compid()) {
	 	/* don't allow chkpnts of the booter */
	 	BUG();
	}

	assert(INITARGS_MAX_PATHNAME > prefix_sz + strlen(c->name));
	memcpy(name, prefix, prefix_sz + 1);
	strncat(name, c->name, INITARGS_MAX_PATHNAME - prefix_sz - 1); 
	c->name[INITARGS_MAX_PATHNAME - 1] = '\0';
	
	/* completed all initialization */
	if (c->init_state >= CRT_COMP_INIT_MAIN) {
		if (crt_nchkpt() > BOOTER_MAX_CHKPT) {
			BUG();
		}

		chkpt = ss_chkpt_alloc();
		if (crt_chkpt_create(chkpt, c) != 0) {
			BUG();
		}

		ss_chkpt_activate(chkpt);
		chkpt_comp_init(new_comp, chkpt, name);
		thdcap = crt_comp_thdcap_get(new_comp);
		assert(thdcap);
		if ((ret = cos_defswitch(thdcap, TCAP_PRIO_MAX, TCAP_RES_INF, cos_sched_sync()))) {
			printc("Switch failure on thdcap %ld, with ret %d\n", thdcap, ret);
			BUG();
		}
	}

	return;
}

void
init_done(int parallel_init, init_main_t main_type)
{
	compid_t client = (compid_t)cos_inv_token();
	struct crt_comp    *c;

	assert(client > 0 && client <= MAX_NUM_COMPS);
	c = boot_comp_get(client);

	crt_compinit_done(c, parallel_init, main_type);

#ifdef ENABLE_CHKPT
	init_done_chkpt(c);
#endif /* ENABLE_CHKPT */
	return;

}


void
init_exit(int retval)
{
	compid_t client = (compid_t)cos_inv_token();
	struct crt_comp *c;

	assert(client > 0 && client <= MAX_NUM_COMPS);
	c = boot_comp_get(client);
	assert(c);

	crt_compinit_exit(c, retval);

	/* TODO: recycle back to a chkpt via chkpt_restore() */

	while (1) ;
}

void
cos_init(void)
{
	booter_init();
	comps_init();
}

void
parallel_main(coreid_t cid)
{
	execute();
}
