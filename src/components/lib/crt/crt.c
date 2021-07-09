/**
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * Copyright 2019, The George Washington University
 * Author: Gabriel Parmer, gparmer@gwu.edu
 */

/**
 * This is the library of kernel manipulation functions that is
 * actually meant to be used directly by components that manage kernel
 * resources. It defines the component, sinv, rcv, thread, and asnd
 * structures, and the APIs in terms of those structures. It hides and
 * uses the cos_defkernel_api to manipulate the capability- and
 * page-table management.
 *
 * The naming scheme of many of these functions:
 *
 * - `crt_*_create` - Creates the resources for a given `*` resource
 *   in the current component.
 *
 * - `crt_*_create_in(crt_*, c, ...)` - Create the resources for a
 *   given `*` resource that is to created *inside* another component,
 *   `c`. This is often used to create threads and rcv endpoints in
 *   another component. This is paired with the `crt_*_alias_into`
 *   function to copy the capabilities into another component's
 *   capability tables.
 *
 * - `crt_*_create_with(crt_*, crt_*_resources, ...)` - Create a `*`
 *   resource using the resources passed into the function. This is
 *   often used when resources that constitute `*` have already been
 *   created by another component (e.g. the pgtbl and captbl for one
 *   of our child components).
 *
 * - `crt_*_alias_in(crt_*, c, crt_*_resources)` - Alias a
 *   resource `*`'s capabilities into the capability tables of a
 *   specific component `c`. Where `crt_*_resources` has non-zero
 *   capabilities, they are used as the location of the resources in
 *   `c`. In this way, this function also contains the functionality
 *   of a `_with` function as well. That structure is used to return
 *   the destination capabilities in `crt_*_resources`.
 */

#include <stdlib.h>

#include <cos_defkernel_api.h>
#include <cos_thd_init.h>
#include <cos_types.h>
#include <llprint.h>
#include <elf_loader.h>
#include <barrier.h>
#include <ps.h>
#include <initargs.h>

#include <crt.h>

#define CRT_REFCNT_INITVAL 1

static unsigned long nchkpt = 0;
static unsigned long ncomp = 1;

unsigned long
crt_ncomp()
{
	return ncomp;
}

unsigned long
crt_nchkpt()
{
	return nchkpt;
}

int
crt_chkpt_create(struct crt_chkpt *chkpt, struct crt_comp *c)
{
	char *mem;
	struct cos_compinfo *root_ci;

	chkpt->c = c;
	ps_faa(&nchkpt, 1);

	/* allocate space for saving the component's memory */
	root_ci = cos_compinfo_get(cos_defcompinfo_curr_get());
	mem = cos_page_bump_allocn(root_ci, c->tot_sz_mem);
	if (!mem) return -ENOMEM;

	chkpt->mem = mem;
	chkpt->tot_sz_mem = c->tot_sz_mem;

	memcpy(mem, c->mem, c->tot_sz_mem);	
	/* 
	 * TODO: capabilities aren't copied, so components that could modify their capabilities
	 * while running (schedulers/cap mgrs) shouldn't be checkpointed 
	 * TODO: copy dynamically allocated memory into the checkpoint
	 */

	return 0;
}

int
crt_chkpt_restore(struct crt_chkpt *chkpt, struct crt_comp *c)
{
	/* turning c, a terminated component, back into a chkpt */
	/* TODO: return memory to a previous saved state */
	return 0;
}

static inline int
crt_refcnt_alive(crt_refcnt_t *r)
{
	return *r > 0;
}

/*
 * Return 0 on success, non-zero on failure (i.e. calling this on a
 * component that is not already active)
 */
static inline void
crt_refcnt_take(crt_refcnt_t *r)
{
	assert(crt_refcnt_alive(r));
	if (ps_faa(r, 1) == 0) BUG();
}

/* return 1 if the last reference is released, 0 otherwise */
static inline int
crt_refcnt_release(crt_refcnt_t *r)
{
	assert(crt_refcnt_alive(r));
	if (ps_faa(r, -1) == 1) return 1;

	return 0;
}

/**
 * Alias a capability into another component, either into a fixed
 * capability, or into one that is bump-pointer allocated.
 *
 * - @cap is the capability to alias into `c`
 * - @type is the type of the capability
 * - @c is the component to alias into
 * - @retcap is set equal to the capability in `c`. If it is passed in
 *   as non-zero with the capability offset to use if we want to map
 *   into a fixed capability. Otherwise, it allocates a new capability
 *   using the normal bump-pointer.
 */
static int
crt_alias_alloc_helper(capid_t cap, cap_t type, struct crt_comp *c, capid_t *retcap)
{
	struct cos_defcompinfo *defci      = cos_defcompinfo_curr_get();
	struct cos_compinfo    *ci         = cos_compinfo_get(defci);
	struct cos_compinfo    *target_ci  = cos_compinfo_get(c->comp_res);
	int ret;

	if (*retcap) {
		/* If we need to expand out the captbls, do so */
		cos_comp_capfrontier_update(target_ci, round_up_to_pow2(*retcap + 1, 4));
		ret = cos_cap_cpy_at(target_ci, *retcap, ci, cap);
		assert(ret == 0);
	} else {
		*retcap = cos_cap_cpy(target_ci, ci, type, cap);
		assert(*retcap != 0);
	}

	return 0;
}


static int
crt_comp_init(struct crt_comp *c, char *name, compid_t id, void *elf_hdr, vaddr_t info)
{
	assert(c && name);

	memset(c, 0, sizeof(struct crt_comp));
	*c = (struct crt_comp) {
		.flags      = CRT_COMP_NONE,
		.name       = name,
		.id         = id,
		.elf_hdr    = elf_hdr,
		.entry_addr = elf_hdr ? elf_entry_addr(elf_hdr) : 0,
		.comp_res   = &c->comp_res_mem,
		.info       = info,
		.refcnt     = CRT_REFCNT_INITVAL,

		.init_state = CRT_COMP_INIT_PREINIT,
		.main_type  = INIT_MAIN_NONE,
		.init_core  = cos_cpuid(),
		.barrier    = SIMPLE_BARRIER_INITVAL
	};
	assert(!elf_hdr || c->entry_addr != 0); /* same as `if elf_hdr then c->entry_addr` */
	simple_barrier_init(&c->barrier, init_parallelism());

	ps_faa(&ncomp, 1);

	return 0;
}

/**
 * Initialize with a specified set of crt_comp_resources
 * (capabilities). This is most often used when a component has
 * *already* been created, and we want to create a crt_comp for
 * it. This happens within, for example, the capmgr.
 *
 * Arguments:
 * - @c the component to initialize.
 * - @name the name (not copied in this function) for debugging.
 * - @id the component's id.
 * - @r the capability resources (and frontiers)
 *
 * @return: 0 on success, != 0 on error.
 */
int
crt_comp_create_with(struct crt_comp *c, char *name, compid_t id, struct crt_comp_resources *r)
{
	assert(c && name && r);

	if (crt_comp_init(c, name, id, NULL, r->info)) BUG();

	cos_compinfo_init(cos_compinfo_get(c->comp_res),
			  r->ptc, r->ctc, r->compc, r->heap_ptr, r->captbl_frontier,
			  cos_compinfo_get(cos_defcompinfo_curr_get()));
	return 0;
}

/**
 * Creates the new component from the checkpoint
 *
 * Arguments:
 * - @c the new component to initialize
 * - @name the name (not copied in this function) for debugging.
 * - @id the component's id.
 * - @chkpt the checkpoint used to create c
 *
 * @return: 0 on success, != 0 on error.
 */
int 
crt_comp_create_from(struct crt_comp *c, char *name, compid_t id, struct crt_chkpt *chkpt)
{
	struct cos_compinfo *ci, *root_ci;
	struct cos_component_information *comp_info;
	unsigned long info_offset;
	size_t  ro_sz,   rw_sz, data_sz, bss_sz;
	char   *ro_src, *data_src, *mem;
	int     ret;
	vaddr_t	info = chkpt->c->info;

	assert(c && name);

	if (crt_comp_init(c, name, id, NULL, chkpt->c->info)) BUG();
	ci      = cos_compinfo_get(c->comp_res);
	root_ci = cos_compinfo_get(cos_defcompinfo_curr_get());

	/* overwrite */
	c->entry_addr = chkpt->c->entry_addr;
	c->ro_addr = chkpt->c->ro_addr;
	c->rw_addr = chkpt->c->rw_addr;

	/* re-work the sinvs with the right IDs */
	memcpy(c->sinvs, chkpt->c->sinvs, sizeof(c->sinvs));
	c->n_sinvs = chkpt->c->n_sinvs;
	for(u32_t i = 0; i < c->n_sinvs; i++) {
		struct crt_sinv inv = chkpt->c->sinvs[i];

		assert(inv.client->id == chkpt->c->id);
		c->sinvs[i].client = c;
		assert(inv.server->id != chkpt->c->id);
	}

	ret = cos_compinfo_alloc(ci, c->ro_addr, BOOT_CAPTBL_FREE, c->entry_addr, root_ci);
	assert(!ret);

	mem = cos_page_bump_allocn(root_ci, chkpt->tot_sz_mem);
	if (!mem) return -ENOMEM;
	c->mem = mem;
	c->tot_sz_mem = chkpt->tot_sz_mem;
	c->ro_sz = chkpt->c->ro_sz;

	memcpy(mem, chkpt->mem, round_up_to_page(chkpt->tot_sz_mem));

	info_offset = info - c->rw_addr;
	comp_info   = (struct cos_component_information *)(mem + round_up_to_page(c->ro_sz) + info_offset);
	comp_info->cos_this_spd_id = 0;
	assert(comp_info->cos_this_spd_id == 0);
	comp_info->cos_this_spd_id = id;

	/* FIXME: separate map of RO and RW */
	if (c->ro_addr != cos_mem_aliasn(ci, root_ci, (vaddr_t)mem, c->tot_sz_mem)) return -ENOMEM;

	/* FIXME: cos_time.h assumes we have access to this... */
	ret = cos_cap_cpy_at(ci, BOOT_CAPTBL_SELF_INITHW_BASE, root_ci, BOOT_CAPTBL_SELF_INITHW_BASE);
	assert(ret == 0);

	return 0;
}

/**
 * Create the component from the elf object including all the resource
 * tables, and memory.
 *
 * Notes:
 * - The capability tables in the generated component are empty.
 * - `name` is *not* copied, so it is borrowed from within
 *   `c`. Allocate/copy it manually if you can't guarantee it will
 *   stay alive.
 *
 * Arguments:
 * - @c the component to initialize.
 * - @name the name (not copied in this function) for debugging.
 * - @id the component's id.
 * - @elf_hdr the opaque pointer to the elf header
 * - @info is the address *within* the component to it's cos_comp_info.
 *
 * @return: 0 on success, != 0 on error.
 */
int
crt_comp_create(struct crt_comp *c, char *name, compid_t id, void *elf_hdr, vaddr_t info)
{
	struct cos_compinfo *ci, *root_ci;
	struct cos_component_information *comp_info;
	unsigned long info_offset;
	size_t  ro_sz,   rw_sz, data_sz, bss_sz, tot_sz;
	char   *ro_src, *data_src, *mem;
	int     ret;

	assert(c && name);

	if (crt_comp_init(c, name, id, elf_hdr, info)) BUG();
	ci      = cos_compinfo_get(c->comp_res);
	root_ci = cos_compinfo_get(cos_defcompinfo_curr_get());

	if (elf_load_info(c->elf_hdr, &c->ro_addr, &ro_sz, &ro_src, &c->rw_addr, &data_sz, &data_src, &bss_sz)) return -EINVAL;

	printc("\t\t elf obj: ro [0x%lx, 0x%lx), data [0x%lx, 0x%lx), bss [0x%lx, 0x%lx).\n",
	       c->ro_addr, c->ro_addr + ro_sz, c->rw_addr, c->rw_addr + data_sz, c->rw_addr + data_sz, c->rw_addr + data_sz + bss_sz);

	ret = cos_compinfo_alloc(ci, c->ro_addr, BOOT_CAPTBL_FREE, c->entry_addr, root_ci);
	assert(!ret);

	tot_sz = round_up_to_page(round_up_to_page(ro_sz) + data_sz + bss_sz);
	mem    = cos_page_bump_allocn(root_ci, tot_sz);
	if (!mem) return -ENOMEM;
	c->mem = mem;
	c->tot_sz_mem = tot_sz;
	c->ro_sz = ro_sz;

	memcpy(mem, ro_src, ro_sz);
	memcpy(mem + round_up_to_page(ro_sz), data_src, data_sz);
	memset(mem + round_up_to_page(ro_sz) + data_sz, 0, bss_sz);

	assert(info >= c->rw_addr && info < c->rw_addr + data_sz);
	info_offset = info - c->rw_addr;
	comp_info   = (struct cos_component_information *)(mem + round_up_to_page(ro_sz) + info_offset);
	assert(comp_info->cos_this_spd_id == 0);
	comp_info->cos_this_spd_id = id;

	c->n_sinvs = 0;
	memset(c->sinvs, 0, sizeof(c->sinvs));

	/* FIXME: separate map of RO and RW */
	if (c->ro_addr != cos_mem_aliasn(ci, root_ci, (vaddr_t)mem, tot_sz)) return -ENOMEM;

	/* FIXME: cos_time.h assumes we have access to this... */
	ret = cos_cap_cpy_at(ci, BOOT_CAPTBL_SELF_INITHW_BASE, root_ci, BOOT_CAPTBL_SELF_INITHW_BASE);
	assert(ret == 0);

	return 0;
}

void
crt_comp_captbl_frontier_update(struct crt_comp *c, capid_t capid)
{
	assert(c);

	cos_comp_capfrontier_update(cos_compinfo_get(c->comp_res), capid);
}

int
crt_booter_create(struct crt_comp *c, char *name, compid_t id, vaddr_t info)
{
	assert(c && name);

	*c = (struct crt_comp) {
		.flags      = CRT_COMP_BOOTER,
		.name       = name,
		.id         = id,
		.comp_res   = cos_defcompinfo_curr_get(),
		.info       = info,
		.refcnt     = CRT_REFCNT_INITVAL,

		.init_state = CRT_COMP_INIT_PREINIT,
		.main_type  = INIT_MAIN_NONE,
		.init_core  = cos_cpuid(),
		.barrier    = SIMPLE_BARRIER_INITVAL,
		.n_sinvs    = 0,
	};
	simple_barrier_init(&c->barrier, init_parallelism());

	return 0;
}

static int
crt_is_booter(struct crt_comp *c)
{
	assert(c);

	return c->flags & CRT_COMP_BOOTER;
}

thdcap_t
crt_comp_thdcap_get(struct crt_comp *c)
{
	thdcap_t ret = 0;

	assert(c);

	if (c->flags & CRT_COMP_INITIALIZE) {
		struct crt_thd *t = c->exec_ctxt.exec.thd;

		if (!t) return 0;

		ret = t->cap;
	} else if (c->flags & CRT_COMP_SCHED) {
		struct crt_rcv *r = c->exec_ctxt.exec.sched.sched_rcv;

		if (!r) return 0;

		ret = r->thd.cap;
	}

	return ret;
}

int
crt_comp_sched_delegate(struct crt_comp *child, struct crt_comp *self, tcap_prio_t prio, tcap_res_t res)
{
	struct cos_aep_info *sched_aep;

	assert(child && self);

	sched_aep = cos_sched_aep_get(self->comp_res);

	return cos_tcap_delegate(child->exec_ctxt.exec.sched.sched_asnd.asnd, sched_aep->tc, res, prio, TCAP_DELEG_YIELD);
}

/**
 * Alias the component, pgtbl, and/or captbl for `c` into `c_in`. Can
 * be placed into specific capability slots as specified in `res`. The
 * capabilities slots they are placed in populate `res`. The `flags`
 * control which of these resources to be aliased.
 */
int
crt_comp_alias_in(struct crt_comp *c, struct crt_comp *c_in, struct crt_comp_resources *res, crt_comp_alias_t flags)
{
	struct cos_compinfo *target_ci;

	assert(c && c_in && res);
	target_ci = cos_compinfo_get(c->comp_res);

	if (flags & CRT_COMP_ALIAS_COMP) {
		if (crt_alias_alloc_helper(target_ci->comp_cap, CAP_COMP, c_in, &res->compc)) BUG();
	}
	if (flags & CRT_COMP_ALIAS_PGTBL) {
		if (crt_alias_alloc_helper(target_ci->pgtbl_cap, CAP_PGTBL, c_in, &res->ptc)) BUG();
	}
	if (flags & CRT_COMP_ALIAS_CAPTBL) {
		if (crt_alias_alloc_helper(target_ci->captbl_cap, CAP_CAPTBL, c_in, &res->ctc)) BUG();
	}
	return 0;
}

int
crt_sinv_create(struct crt_sinv *sinv, char *name, struct crt_comp *server, struct crt_comp *client,
		vaddr_t c_fn_addr, vaddr_t c_ucap_addr, vaddr_t s_fn_addr)
{
	struct cos_compinfo *cli;
	struct cos_compinfo *srv;
	unsigned int ucap_off;
	struct usr_inv_cap *ucap;

	assert(sinv && name && server && client);

	cli = cos_compinfo_get(client->comp_res);
	srv = cos_compinfo_get(server->comp_res);

	assert(crt_refcnt_alive(&server->refcnt) && crt_refcnt_alive(&client->refcnt));
	crt_refcnt_take(&client->refcnt);
	crt_refcnt_take(&server->refcnt);

	assert(cli && cli->memsrc && srv && srv->memsrc && srv->comp_cap);
	assert(!crt_is_booter(client));

	*sinv = (struct crt_sinv) {
		.name        = name,
		.server      = server,
		.client      = client,
		.c_fn_addr   = c_fn_addr,
		.c_ucap_addr = c_ucap_addr,
		.s_fn_addr   = s_fn_addr
	};

	sinv->sinv_cap = cos_sinv_alloc(cli, srv->comp_cap, sinv->s_fn_addr, client->id);
	assert(sinv->sinv_cap);
	printc("sinv %s cap %ld\n", name, sinv->sinv_cap);

	/* poor-mans virtual address translation from client VAS -> our ptrs */
	assert(sinv->c_ucap_addr - sinv->client->ro_addr > 0);
	ucap_off = sinv->c_ucap_addr - sinv->client->ro_addr;
	ucap = (struct usr_inv_cap *)(sinv->client->mem + ucap_off);
	*ucap = (struct usr_inv_cap) {
		.invocation_fn = sinv->c_fn_addr,
		.cap_no        = sinv->sinv_cap,
		.data          = NULL
	};

	return 0;
}

int
crt_sinv_alias_in(struct crt_sinv *s, struct crt_comp *c, struct crt_sinv_resources *res)
{
	assert(s && c && res);

	if (crt_alias_alloc_helper(s->sinv_cap, CAP_SINV, c, &res->sinv_cap)) BUG();

	return 0;
}

/**
 * # Notes on managing the complexity of the thread/rcv API
 *
 * There are five main types of threads that need to be created, thus
 * the complexity of this API. There are a few dimension in which
 * these threads are different:
 *
 * 1. Initialization thread or "thread on request"? Is the thread that
 *    executes the component's main initialization procedure, or is it
 *    one that will execute a requested function/closure? Note that
 *    there can be an initialization thread *per core*.
 * 2. Thread to be created in *us*, or in a *separate component*? In
 *    the former case, we use a higher-level API based on passing a
 *    closure for thread execution. In the latter case, we pass a
 *    closure id to be interpreted within the destination program.
 * 3. The thread is bound to a receive end-point, and associated with
 *    a tcap. This can be because 1. we're creating an asynchronous
 *    communication channel, or 2. because we're creating an initial
 *    thread in a child scheduler.
 * 4. Is the thread simply for normal execution in another component,
 *    or to be controlled by a scheduler other than us (thus requiring
 *    us to delegate its capability)?
 * 5. Threads and rcv end-points must sometimes be created from
 *    pre-existing capabilities. For example, if the loader
 *    auto-matically created some execution resources for the
 *    component, we must create the abstraction from the existing
 *    caps.
 *
 * Not all combinations of the variables in these dimensions make
 * sense. Most notably, it doesn't make sense to create an
 * initialization thread for ourselves. The differing arguments for
 * each variant include:
 *
 * - New thread's control flow initialization: is it a `(fn, data)` or
 *   `closure_id`?
 * - What is the tcap associated with a rcv end-point? Which is its
 *   scheduler tcap?
 * - Which component should the thread be created in? Us, or another?
 * - Do we return a thread or a rcv end-point?
 * - How do we access the thread id, and where do we return it?
 *
 * Thus, a complete set of arguments would include:
 *
 * - (fn, data)
 * - closure id
 * - component in which to create component
 * - receive end-point for scheduler
 * - receive end-point to use for tcap, or NULL (new tcap)
 *
 * We'd want to return:
 *
 * - crt thread structure (thdcap)
 * - receive structure (thdcap, tcap, rcvcap)
 * - the thread id
 * - errors
 *
 * An API based on this would be unreasonable, to put it lightly. (See
 * the proliferation within our previous APIs.) We don't have method
 * overloading, nor simple syntax for variants, so the API has to make
 * very specific trade-offs. We have a number of options:
 *
 * - We can split the API along any dimension. This is likely
 *   reasonable for "create thread in this component" versus, "other
 *   components", as the latter strongly corresponds to conventional
 *   APIs.
 * - We can detect if the thread we're creating a thread in is a
 *   scheduler. If it is, we can do the right thing (e.g. init thread
 *   creation turns into init async end-point creation). This implies
 *   that the component structure must track its scheduler
 *   asynchronous end-points. The scheduler's tcap will be used by
 *   default as the "parent" in the tcap hierarchy.
 * - We can pass arguments in as a structure with many fields. A
 *   separate API can configure the structure appropriately. This has
 *   the benefit of not raising the conceptual complexity of the main
 *   API, and only the "extra features" configured within the
 *   structure.
 *
 * Given these trade-offs, this API includes three variants:
 *
 * 1. `int crt_{thd|rcv}_create(struct crt_{thd|rcv} *, fn_t, void *, struct crt_rcv *parent)`
 *    which creates a thread/rcv end-point to execute in *this*
 *    component. `parent` is only in the `rcv` API and, if non-NULL,
 *    includes the tcap to use.
 * 2. `int crt_{thd|rcv}_create_in(struct crt_{thd|rcv} *, struct crt_comp *c, crt_closure_id_t id, struct crt_rcv *parent)`
 *    where `c` is the component in which to create execution, and
 *    `id` includes the closure id we should execute with in component
 *    `c`. `parent` is only passed in the rcv API, and has the `tcap`
 *    to use, or is NULL.
 * 3. `int crt_{thd|rcv}_create_with(struct crt_{thd|rcv} *, struct crt_exec_resources *r)`
 *    creates a thread using the given capability resources in `r`.
 */

int
crt_thd_create_with(struct crt_thd *t, struct crt_comp *c, struct crt_thd_resources *rs)
{
	assert(t && c && rs);

	*t = (struct crt_thd) {
		.cap = rs->cap,
		.c   = c
	};

	return 0;
}

/*
 * Create a new thread in the component @c in response to a request
 * to create the thread from that component (thus passing in the
 * requested @closure_id).
 */
int
crt_thd_create_in(struct crt_thd *t, struct crt_comp *c, thdclosure_index_t closure_id)
{
	struct cos_defcompinfo *defci = cos_defcompinfo_curr_get();
	struct cos_compinfo    *ci    = cos_compinfo_get(defci);
	struct cos_compinfo    *target_ci;
	struct cos_aep_info    *target_aep;
	thdcap_t thdcap;
	struct crt_thd_resources rs;

	assert(t && c);

	target_ci = cos_compinfo_get(c->comp_res);
	target_aep = cos_sched_aep_get(c->comp_res);

	assert(target_ci->comp_cap);
	if (closure_id == 0) {
		if(target_aep->thd != 0) return -1; /* should not allow double initialization */

		crt_refcnt_take(&c->refcnt);
		assert(target_ci->comp_cap);
		thdcap = target_aep->thd = cos_initthd_alloc(ci, target_ci->comp_cap);
		assert(target_aep->thd);
	} else {
		crt_refcnt_take(&c->refcnt);
		thdcap = cos_thd_alloc_ext(ci, target_ci->comp_cap, closure_id);
		assert(thdcap);
	}

	rs = (struct crt_thd_resources) { .cap = thdcap };
	if (crt_thd_create_with(t, c, &rs)) BUG();
	t->tid = cos_introspect(ci, thdcap, THD_GET_TID);

	return 0;
}

/**
 * Create a new thread in this component to execute fn with the
 * corresponding data.
 *
 * - @t the thread structure to be populated
 * - @self the crt_comp that represents us
 * - @fn/@data the function to be invoked, passed specific data.
 * - @return `0` if successful, `<0` otherwise
 */
int
crt_thd_create(struct crt_thd *t, struct crt_comp *self, crt_thd_fn_t fn, void *data)
{
	int      idx = cos_thd_init_alloc(fn, data);
	thdcap_t ret;

	assert(t && self);
	if (idx < 1) return 0;
	ret = crt_thd_create_in(t, self, idx);
	if (ret < 0) cos_thd_init_free(idx);

	return ret;
}

int
crt_thd_alias_in(struct crt_thd *t, struct crt_comp *c, struct crt_thd_resources *res)
{
	assert(t && c && res);

	if (crt_alias_alloc_helper(t->cap, CAP_THD, c, &res->cap)) BUG();

	return 0;
}

int
crt_rcv_create_with(struct crt_rcv *r, struct crt_comp *c, struct crt_rcv_resources *rs)
{
	assert(r && c && rs);

	*r = (struct crt_rcv) {
		.thd = (struct crt_thd) {
			.cap = rs->thd,
			.c   = c
		},
		.local_aep = (struct cos_aep_info) {
			.tc   = rs->tc,
			.thd  = rs->thd,
			.tid  = rs->tid,
			.rcv  = rs->rcv,
			.fn   = NULL,
			.data = NULL
		},
		.c         = c,
		.refcnt    = CRT_REFCNT_INITVAL
	};
	r->aep = &r->local_aep;

	return 0;
}

/**
 * Create a new receive end-point in a specific component, complete
 * with an executable thread. We explicitly specify which component is
 * to act as its scheduler (i.e. which component has the scheduler rcv
 * end-point), and if this rcv resource should have its own,
 * independent tcap, or inherit the one from the scheduler.
 *
 * - @r the rcv structure to be populated.
 * - @c the component in which the rcv should be created.
 * - @sched the scheduler component, or NULL if it is "us".
 * - @id the thread closure that contains fn/data in c.
 * - @flags if we should inherit the schedulers tcap, and other options
 *
 * Note: the capabilities returned from here are valid only within the
 * current capability-tables. Should you want to allow the rcv cap to
 * be used in `c`, you must copy the capabilities accordingly.
 */
int
crt_rcv_create_in(struct crt_rcv *r, struct crt_comp *c, struct crt_rcv *sched, thdclosure_index_t closure_id, crt_rcv_flags_t flags)
{
	struct cos_defcompinfo *defci      = cos_defcompinfo_curr_get();
	struct cos_compinfo    *ci         = cos_compinfo_get(defci);
	struct cos_compinfo    *target_ci  = cos_compinfo_get(c->comp_res);
	struct cos_aep_info    *sched_aep;
	struct crt_rcv_resources res;

	tcap_t    tcap;
	thdcap_t  thdcap;
	arcvcap_t rcvcap;

	assert(r && c);

	if (sched) {
		sched_aep = sched->aep;
	} else {
		sched_aep = cos_sched_aep_get(defci);
	}

	/* Note that this increases the component's reference count */
	crt_refcnt_take(&c->refcnt);
	assert(target_ci->comp_cap);
	if (closure_id == 0) {
		thdcap = cos_initthd_alloc(cos_compinfo_get(defci), target_ci->comp_cap);
	} else {
		thdcap = cos_thd_alloc_ext(cos_compinfo_get(defci), target_ci->comp_cap, closure_id);
	}
	assert(thdcap);

	/* Allocate the necessary kernel resources */
	tcap   = cos_tcap_alloc(ci);
	assert(tcap);
	rcvcap = cos_arcv_alloc(ci, thdcap, tcap, target_ci->comp_cap, sched_aep->rcv);
	assert(rcvcap);

	res = (struct crt_rcv_resources) {
		.tc   = tcap,
		.thd  = thdcap,
		.tid  = 0,
		.rcv  = rcvcap,
	};
	if (crt_rcv_create_with(r, c, &res)) BUG();

	if (sched) crt_refcnt_take(&sched->refcnt);

	return 0;
}

/**
 * Create a new rcv end-point in this component to execute fn with the
 * corresponding data.
 *
 * - @r the rcv structure to be populated
 * - @self the crt_comp that represents us
 * - @fn/@data the function to be invoked, passed specific data.
 */
int
crt_rcv_create(struct crt_rcv *r, struct crt_comp *self, crt_thd_fn_t fn, void *data)
{
	int      idx = cos_thd_init_alloc(fn, data);
	thdcap_t ret;

	assert(r && self);

	if (idx < 1) return 0;
	ret = crt_rcv_create_in(r, self, NULL, idx, CRT_RCV_TCAP_INHERIT);
	if (!ret) cos_thd_init_free(idx);
	/* As this rcv is in this component, we need this reference to make an asnd */
	r->child_rcv = r->local_aep.rcv;

	return ret;
}

/**
 * Make aliases for a receive end-point into a component. As with all
 * alias functions, initialize res capabilities to ask the function to
 * alias at those specific capability slots.
 *
 * - @flags controls which parts of the rcv end-point are
 *   aliased. `CRT_RCV_ALIAS_RCV`, `CRT_RCV_ALIAS_THD`, and
 *   `CRT_RCV_ALIAS_TCAP` enable the corresponding resources to be
 *   aliased.
 */
int
crt_rcv_alias_in(struct crt_rcv *r, struct crt_comp *c, struct crt_rcv_resources *res, crt_rcv_alias_t flags)
{
	assert(r && c && res);

	if (flags & CRT_RCV_ALIAS_RCV) {
		if (crt_alias_alloc_helper(r->aep->rcv, CAP_ARCV, c, &res->rcv)) BUG();
		/*
		 * Creating a snd requires that we have the rcv cap
		 * within the owning components captbl
		 */
		r->child_rcv = res->rcv;
	}
	if (flags & CRT_RCV_ALIAS_THD) {
		if (crt_alias_alloc_helper(r->aep->thd, CAP_THD, c, &res->thd)) BUG();
	}
	if (flags & CRT_RCV_ALIAS_TCAP) {
		if (crt_alias_alloc_helper(r->aep->tc, CAP_TCAP, c, &res->tc)) BUG();
	}

	return 0;
}

int
crt_asnd_create(struct crt_asnd *s, struct crt_rcv *r)
{
	struct cos_compinfo    *ci = cos_compinfo_get(cos_defcompinfo_curr_get());
	struct cos_compinfo    *target_ci;
	asndcap_t ascap;

	assert(s && r && r->thd.c && r->thd.c->comp_res);
	assert(r->aep && r->aep->rcv);
	assert(r->child_rcv); 	/* must create in this the current component, or alias first */
	target_ci = cos_compinfo_get(r->thd.c->comp_res);
	assert(target_ci->captbl_cap);
	crt_refcnt_take(&r->refcnt);

	assert(r->aep->rcv && target_ci->captbl_cap);
	ascap = cos_asnd_alloc(ci, r->child_rcv, target_ci->captbl_cap);
	assert(ascap);

	*s = (struct crt_asnd) {
		.asnd = ascap,
		.rcv  = r
	};

	return 0;
}

int
crt_asnd_alias_in(struct crt_asnd *s, struct crt_comp *c, struct crt_asnd_resources *res)
{
	assert(s && c && res);

	if (crt_alias_alloc_helper(s->asnd, CAP_ASND, c, &res->asnd)) BUG();

	return 0;
}

/**
 * Compositional API to configure execution information for a
 * component. Once a component has been created, we often want to
 * create execution in it, and it might need a new thread, or a rcv
 * end-point if it is a scheduler. This API enables initialization
 * data to be added to a context by composing these calls. Finally,
 * they are passed into `crt_comp_exec` to initialize the execution.
 * Scheduler init, and capmgr init can be performed across two
 * `crt_comp_exec` calls.
 *
 * These are the valid compositions:
 *
 * - `crt_comp_exec_capmgr_init(crt_comp_exec_sched_init(&c, r), N)`
 * - `crt_comp_exec_thd_init(&c, t)`
 * - `crt_comp_exec_sched_init(&c, r)`
 *
 * None of these functions return NULL, and assert out if invalid
 * arguments are passed in. Most usage patterns (statically allocated
 * `crt_comp_exec_context` on the stack) will bomb out
 * deterministically if the API is misused, and avoiding the output
 * checks encourages code simplicity.
 */
struct crt_comp_exec_context *
crt_comp_exec_sched_init(struct crt_comp_exec_context *ctxt, struct crt_rcv *r)
{
	assert(ctxt && r);
	assert(!(ctxt->flags & CRT_COMP_INITIALIZE));

	ctxt->flags |= CRT_COMP_SCHED;
	ctxt->exec.sched.sched_rcv = r;

	return ctxt;
}

struct crt_comp_exec_context *
crt_comp_exec_thd_init(struct crt_comp_exec_context *ctxt, struct crt_thd *t)
{
	assert(ctxt && t);
	assert(!(ctxt->flags & CRT_COMP_SCHED));

	ctxt->flags |= CRT_COMP_INITIALIZE;
	ctxt->exec.thd = t;

	return ctxt;
}

struct crt_comp_exec_context *
crt_comp_exec_capmgr_init(struct crt_comp_exec_context *ctxt, size_t untyped_memsz)
{
	assert(ctxt);
	assert(!(ctxt->flags & CRT_COMP_INITIALIZE)); /* capmgrs cannot be normal threads */

	ctxt->flags |= CRT_COMP_CAPMGR;
	ctxt->memsz = untyped_memsz;

	return ctxt;
}

/*
 * `crt_comp_exec_schedthd` can return `NULL` if the component was not
 * initialized as a scheduler. `crt_comp_exec_thd` can return NULL if
 * the component was *not* initialized for execution at all.
 *
 * Note that being a scheduler implies that it has a thread. Being a
 * capability manager implies being a scheduler.
 */
struct crt_rcv *
crt_comp_exec_schedthd(struct crt_comp *comp)
{
	assert(comp);
	assert((comp->exec_ctxt.flags & CRT_COMP_SCHED) && comp->exec_ctxt.exec.sched.sched_rcv != NULL);

	return comp->exec_ctxt.exec.sched.sched_rcv;
}

struct crt_thd *
crt_comp_exec_thd(struct crt_comp *comp)
{
	assert(comp);
	assert((comp->exec_ctxt.flags & (CRT_COMP_INITIALIZE | CRT_COMP_SCHED)));

	return comp->exec_ctxt.exec.thd;
}

/**
 * Modify the component to make it a scheduler and/or a capability
 * manager. This allocates the initial execution and capabilities into
 * the target component. For a scheduler, it creates the rcv endpoint,
 * thread, and tcap for the scheduler. For now, it also maps in the
 * component's capability table. For a capability manager, it does
 * that *and* maps in the page-table and component structure of the
 * component itself. In addition, for a capability manager, a `memsz`
 * amount of untyped memory is mapped into a pgtbl that is mapped into
 * the component.
 *
 * - @c the component to upgrade
 * - @flags in CRT_COMP_SCHED and/or CRT_COMP_CAPMGR
 * - @memsz is the amount of untyped memory to map into the capmgr
 *   (not relevant if you don't pass in CRT_COMP_CAPMGR)
 * - @ret 0 on success
 */
int
crt_comp_exec(struct crt_comp *c, struct crt_comp_exec_context *ctxt)
{
	struct cos_defcompinfo *defci      = cos_defcompinfo_curr_get();
	struct cos_compinfo    *ci         = cos_compinfo_get(defci);
	struct cos_compinfo    *target_ci  = cos_compinfo_get(c->comp_res);
	struct cos_aep_info    *target_aep = cos_sched_aep_get(c->comp_res);
	struct crt_comp_resources compres;
	int ret;

	assert(c && ctxt);

	/* Should only be called if initialization is necessary */
	assert(target_ci->comp_cap);
	if (ctxt->flags & CRT_COMP_CAPMGR && !(c->flags & CRT_COMP_SCHED)) ctxt->flags |= CRT_COMP_SCHED;
	assert(!(ctxt->flags & CRT_COMP_INITIALIZE) || !(ctxt->flags & CRT_COMP_SCHED)); /* choose one */
	assert((c->flags & ctxt->flags) == 0);

	if (ctxt->flags & CRT_COMP_INITIALIZE) {
		assert(!(c->flags & (CRT_COMP_CAPMGR | CRT_COMP_SCHED)) && ctxt->exec.thd);

		c->exec_ctxt = *ctxt;
		c->flags     = ctxt->flags;

		if (crt_thd_create_in(ctxt->exec.thd, c, 0)) BUG();

		return 0;
	}

	if (ctxt->flags & CRT_COMP_SCHED) {
		struct crt_rcv_resources rcvres;
		struct crt_rcv *r;

		assert(!(c->flags & CRT_COMP_SCHED) && ctxt->exec.sched.sched_rcv);
		r = ctxt->exec.sched.sched_rcv;

		if (crt_rcv_create_in(r, c, NULL, 0, 0)) BUG();

		rcvres = (struct crt_rcv_resources) {
			.tc  = BOOT_CAPTBL_SELF_INITTCAP_CPU_BASE,
			.thd = BOOT_CAPTBL_SELF_INITTHD_CPU_BASE,
			.rcv = BOOT_CAPTBL_SELF_INITRCV_CPU_BASE,
		};
		if (crt_rcv_alias_in(r, c, &rcvres, CRT_RCV_ALIAS_RCV | CRT_RCV_ALIAS_THD | CRT_RCV_ALIAS_TCAP)) BUG();

		*target_aep = r->local_aep; /* update the component's structures */
		assert(target_aep->thd && target_aep->tc && target_aep->rcv);

		/*
		 * FIXME:
		 * This is an ugly hack to allow components to do cos_introspect()
		 * - to get thdid
		 * - to get budget on tcap
		 * - other introspect uses
		 *
		 * I don't know a way to get away from this for now!
		 * If it were just thdid, capmgr could have returned the thdids!
		 */
		compres = (struct crt_comp_resources) {
			.ctc = BOOT_CAPTBL_SELF_CT
		};
		if (crt_comp_alias_in(c, c, &compres, CRT_COMP_ALIAS_CAPTBL)) BUG();
		/*
		 * FIXME: should subset the permissions for this
		 * around time management. This should be added back
		 * in and removed above
		 */
		/* ret = cos_cap_cpy_at(target_ci, BOOT_CAPTBL_SELF_INITHW_BASE, ci, BOOT_CAPTBL_SELF_INITHW_BASE); */
		/* assert(ret == 0); */

		/* Update the component's structure */
		c->exec_ctxt.exec.sched.sched_rcv = r;
		/* Make an asnd capability to the child so that we can do tcap delegations */
		if (crt_asnd_create(&c->exec_ctxt.exec.sched.sched_asnd, r)) BUG();

		c->flags |= CRT_COMP_SCHED;
	}
	/* fall-through here */
	if (ctxt->flags & CRT_COMP_CAPMGR) {
		pgtblcap_t utpt;

		assert((c->flags & CRT_COMP_SCHED) && !(c->flags & (CRT_COMP_INITIALIZE | CRT_COMP_CAPMGR)));

		/* assume CT is already mapped in from sched_create */
		compres = (struct crt_comp_resources) {
			.ptc   = BOOT_CAPTBL_SELF_PT,
			.compc = BOOT_CAPTBL_SELF_COMP
		};
		if (crt_comp_alias_in(c, c, &compres, CRT_COMP_ALIAS_PGTBL | CRT_COMP_ALIAS_COMP)) BUG();

		/* Set up the untyped memory in the new component */
		utpt = cos_pgtbl_alloc(ci);
		assert(utpt);
		cos_meminfo_init(&(target_ci->mi), BOOT_MEM_KM_BASE, ctxt->memsz, utpt);
		cos_meminfo_alloc(target_ci, BOOT_MEM_KM_BASE, ctxt->memsz);
		ret = cos_cap_cpy_at(target_ci, BOOT_CAPTBL_SELF_UNTYPED_PT, ci, target_ci->mi.pgtbl_cap);
		assert(ret == 0);

		c->exec_ctxt.memsz = ctxt->memsz;
		c->flags |= CRT_COMP_CAPMGR;
	}

	return 0;
}

void *
crt_page_allocn(struct crt_comp *c, u32_t n_pages)
{
	assert(c);

	return cos_page_bump_allocn(cos_compinfo_get(c->comp_res), n_pages * PAGE_SIZE);
}

int
crt_page_aliasn_in(void *pages, u32_t n_pages, struct crt_comp *self, struct crt_comp *c_in, vaddr_t *map_addr)
{
	*map_addr = cos_mem_aliasn(cos_compinfo_get(c_in->comp_res), cos_compinfo_get(self->comp_res), (vaddr_t)pages, n_pages * PAGE_SIZE);
	if (!*map_addr) return -EINVAL;

	return 0;
}

/*
 * The functions to automate much of the component initialization
 * logic follow.
 */
void
crt_compinit_execute(comp_get_fn_t comp_get)
{
	struct initargs comps, curr;
	struct initargs_iter i;
	int cont;
	int ret;

	/* Initialize components in order of the pre-computed schedule from mkimg */
	ret = args_get_entry("execute", &comps);
	assert(!ret);
	for (cont = args_iter(&comps, &i, &curr) ; cont ; cont = args_iter_next(&i, &curr)) {
		struct crt_comp  *comp;
		int      keylen;
		compid_t id        = atoi(args_key(&curr, &keylen));
		char    *exec_type = args_value(&curr);
		int      initcore, ret;
		thdcap_t thdcap;

		comp     = comp_get(id);
		assert(comp);
		initcore = comp->init_core == cos_cpuid();
		assert(comp->init_state = CRT_COMP_INIT_COS_INIT);
		thdcap   = crt_comp_thdcap_get(comp);

		if (initcore) {
			printc("Initializing component %lu (executing cos_init).\n", comp->id);
		} else {
			/* wait for the init core's thread to initialize */
			while (ps_load(&comp->init_state) == CRT_COMP_INIT_COS_INIT) ;
			if (ps_load(&comp->init_state) != CRT_COMP_INIT_PAR_INIT) continue;

			/* Lazily allocate parallel threads only when they are required. */
			if (!thdcap) {
				assert(0);
				/* ret = crt_thd_init_create(comp); */
				assert(ret == 0);
				thdcap = crt_comp_thdcap_get(comp);
			}
		}
		assert(thdcap);

		if (comp->flags & CRT_COMP_SCHED) {
			if (crt_comp_sched_delegate(comp, comp_get(cos_compid()), TCAP_PRIO_MAX, TCAP_RES_INF)) BUG();
		} else {
			if ((ret = cos_defswitch(thdcap, TCAP_PRIO_MAX, TCAP_RES_INF, cos_sched_sync()))) {
				printc("Switch failure on thdcap %ld, with ret %d\n", thdcap, ret);
				BUG();
			}
		}
		assert(comp->init_state > CRT_COMP_INIT_PAR_INIT);
	}

	/*
	 * Initialization of components (parallel or sequential)
	 * complete. Execute the main in components, FIFO
	 */
	/* Initialize components in order of the pre-computed schedule from mkimg */
	ret = args_get_entry("execute", &comps);
	assert(!ret);
	for (cont = args_iter(&comps, &i, &curr) ; cont ; cont = args_iter_next(&i, &curr)) {
		struct crt_comp  *comp;
		int      keylen;
		compid_t id        = atoi(args_key(&curr, &keylen));
		char    *exec_type = args_value(&curr);
		int initcore;
		thdcap_t thdcap;

		comp = comp_get(id);
		assert(comp);
		initcore = comp->init_core == cos_cpuid();
		thdcap   = crt_comp_thdcap_get(comp);

		/* wait for the initcore to change the state... */
		while (ps_load(&comp->init_state) == CRT_COMP_INIT_COS_INIT || ps_load(&comp->init_state) == CRT_COMP_INIT_PAR_INIT) ;
		/* If we don't need to continue persistent computation... */
		if (ps_load(&comp->init_state) == CRT_COMP_INIT_PASSIVE ||
		    (comp->main_type == INIT_MAIN_SINGLE && !initcore)) continue;

		if (initcore) {
			assert(thdcap);
			printc("Switching to main in component %lu.\n", comp->id);
		} else if (!thdcap) {
			assert(0); /* FIXME: Update once the single core is working */
			/* ret = crt_thd_init_create(comp); */
			/* assert(ret == 0); */
			/* thdcap = crt_comp_create_in(comp); */
			/* assert(thdcap); */
		}

		if (comp->flags & CRT_COMP_SCHED) {
			struct cos_defcompinfo *defci     = comp->comp_res;
			struct cos_aep_info    *child_aep = cos_sched_aep_get(defci);

			if (cos_switch(thdcap, child_aep->tc, TCAP_PRIO_MAX, TCAP_RES_INF, child_aep->rcv, cos_sched_sync())) BUG();
		} else {
			if (cos_defswitch(thdcap, TCAP_PRIO_MAX, TCAP_RES_INF, cos_sched_sync())) BUG();
		}
	}

	cos_hw_shutdown(BOOT_CAPTBL_SELF_INITHW_BASE);
	while (1) ;

	BUG();

	return;
}

void
crt_compinit_done(struct crt_comp *c, int parallel_init, init_main_t main_type)
{
	assert(c->id != cos_compid());
	assert(c && ps_load(&c->init_state) > CRT_COMP_INIT_PREINIT);

	switch (ps_load(&c->init_state)) {
	case CRT_COMP_INIT_COS_INIT: {
		c->main_type = main_type;

		if (parallel_init) {
			/* This will activate any parallel threads */
			ps_store(&c->init_state, CRT_COMP_INIT_PAR_INIT);
			return; /* we're continuing with initialization, return! */
		}

		if (c->main_type == INIT_MAIN_NONE) ps_store(&c->init_state, CRT_COMP_INIT_PASSIVE);
		else                                ps_store(&c->init_state, CRT_COMP_INIT_MAIN);

		break;
	}
	case CRT_COMP_INIT_PAR_INIT: {
		simple_barrier(&c->barrier);
		if (c->init_core != cos_cpuid()) break;

		if (c->main_type == INIT_MAIN_NONE) ps_store(&c->init_state, CRT_COMP_INIT_PASSIVE);
		else                                ps_store(&c->init_state, CRT_COMP_INIT_MAIN);

		break;
	}
	default:
		/*
		 * We only get here if the client defined cos_init and
		 * main, thus skipped parallel_init, and *already*
		 * notified the system that we're executing main.
		 */
		assert(c->init_state == CRT_COMP_INIT_MAIN || CRT_COMP_INIT_PASSIVE);
		return;
	}

	if (c->init_core == cos_cpuid()) {
		printc("Component %lu initialization complete%s.\n", c->id, (c->main_type > 0 ? ", awaiting main execution": ""));
	}

	/* switch back to the booter's thread in execute() */
	if (cos_defswitch(BOOT_CAPTBL_SELF_INITTHD_CPU_BASE, TCAP_PRIO_MAX, TCAP_RES_INF, cos_sched_sync())) BUG();

	assert(c->init_state != CRT_COMP_INIT_PASSIVE);
	assert(c->init_state != CRT_COMP_INIT_COS_INIT && c->init_state != CRT_COMP_INIT_PAR_INIT);

	if (c->init_state == CRT_COMP_INIT_MAIN && c->init_core == cos_cpuid()) {
		printc("Executing main in component %lu.\n", c->id);
	}

	return;
}

void
crt_compinit_exit(struct crt_comp *c, int retval)
{
	assert(c->id != cos_compid());
	/*
	 * TODO: should likely wait to do this until the exit comes
	 * from all cores.
	 */
	if (c->init_core == cos_cpuid()) {
		c->init_state = CRT_COMP_INIT_TERM;
		printc("Component %lu has terminated with error code %d on core %lu.\n",
		       c->id, retval, cos_cpuid());
	}

	/* switch back to the booter's thread in execute */
	if (cos_defswitch(BOOT_CAPTBL_SELF_INITTHD_CPU_BASE, TCAP_PRIO_MAX, TCAP_RES_INF, cos_sched_sync())) BUG();
	BUG();
	while (1) ;
}

