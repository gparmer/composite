/**
 * Copyright 2014 by Qi Wang, interwq@gwu.edu
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 *
 */

#include "include/retype_tbl.h"
#include "include/chal.h"
#include "include/chal/defs.h"
#include "include/shared/cos_types.h"
#include "include/shared/cos_errno.h"
#include "cc.h"
#include <assert.h>

/* The retype table walking structure - track our path through the pages of different granularity */
struct page_record {
	int inc_cnt;
	struct retype_entry *p;
	struct retype_entry_glb *p_glb;
};

/* The CPU-local data structures */
struct retype_info     retype_tbl[NUM_CPU] CACHE_ALIGNED;
struct retype_info_glb glb_retype_tbl[N_RETYPE_SLOTS] CACHE_ALIGNED;

/* The start positions of the tables - also architecture-specific */
const int pos2base[] = {0, N_MEM_SETS};

/* Currently this is x86 specific. Convert size orders to position */
const int order2pos[] = {COS_PGTBL_ORDER2POS};

/* Convert position to size orders */
const int pos2order[] = {COS_PGTBL_ORDERS};

#define POS(order)       (order2pos[order])
#define BASE(order)      (pos2base[POS(order)])
#define ORDER(pos)       (pos2order[pos])
#define SUPER(order)     (pos2order[SUPER_POS(order)])
#define SMALL(order)     (pos2order[SMALL_POS(order)])

/* This only does reference for kernel typed memory */
int
retypetbl_kern_ref(void *pa, u32_t order)
{
	struct retype_entry_glb* p_glb;
	int ret, idx;

	assert(pa); /* cannot be NULL: kernel image takes that space */
	PA_BOUNDARY_CHECK();

	idx = GET_MEM_IDX(pa);
	assert(idx < N_MEM_SETS);
	p_glb = GET_GLB_RETYPE_ENTRY(idx, order);
	/* Is this kernel memory? */
	if (p_glb->refcnt_atom.type != RETYPETBL_KERN) return -EINVAL;
	/* Atomic with FAA - necessary because of cluster counting */
	cos_faa((int*)&(p_glb->kernel_ref), 1);

	return 0;
}

int
retypetbl_kern_deref(void *pa, u32_t order)
{
	struct retype_entry_glb* p_glb;
	int ret, idx;

	assert(pa); /* cannot be NULL: kernel image takes that space */
	PA_BOUNDARY_CHECK();

	idx = GET_MEM_IDX(pa);
	assert(idx < N_MEM_SETS);

	p_glb = GET_GLB_RETYPE_ENTRY(idx, order);
	/* Is this kernel memory? */
	if (p_glb->refcnt_atom.type != RETYPETBL_KERN) return -EINVAL;
	/* Atomic with FAA - necessary because of cluster counting */
	cos_faa((int*)&(p_glb->kernel_ref), -1);

	return 0;
}

/* This will only track the number of user counts */
int
retypetbl_ref(void *pa, u32_t order)
{
	int idx, old_v;
	struct retype_entry *retype_entry;
	union refcnt_atom local_u;
	int found = 0;

	assert(pa); /* cannot be NULL: kernel image takes that space */
	PA_BOUNDARY_CHECK();

	idx = GET_MEM_IDX(pa);
	assert(idx < N_MEM_SETS);

	retype_entry = GET_RETYPE_ENTRY(idx, ORDER(0));
	old_v = local_u.v = retype_entry->refcnt_atom.v;
	if (local_u.ref_cnt == RETYPE_REFCNT_MAX) return -EOVERFLOW;
	/* Increase or decrease the core-local count with CAS */
	local_u.ref_cnt = local_u.ref_cnt + 1;
	cos_mem_fence();
	if (retypetbl_cas(&(retype_entry->refcnt_atom.v), old_v, local_u.v) != CAS_SUCCESS) return -ECASFAIL;

	return 0;
}

int
retypetbl_deref(void *pa, u32_t order)
{
	struct page_record walk[NUM_PAGE_SIZES];
	int found = 0;
	int i, ret, idx;
	memset(walk, 0, sizeof(struct page_record) * NUM_PAGE_SIZES);
	assert(pa); /* cannot be NULL: kernel image takes that space */
	PA_BOUNDARY_CHECK();

	idx = GET_MEM_IDX(pa);
	assert(idx < N_MEM_SETS);

	/* See if it is kernel or user */
	for (i = POS(MAX_PAGE_ORDER); i >= POS(order); i--) {
		walk[i].p = GET_RETYPE_ENTRY(idx, ORDER(i));
		if (walk[i].p->refcnt_atom.type == RETYPETBL_KERN) cos_throw(err, -EPERM);
		if (walk[i].p->refcnt_atom.type == RETYPETBL_USER) found = 1;
		walk[i].p->refcnt_atom.v--;
		walk[i].inc_cnt = 1;
	}
	if (!found) cos_throw(err, -EPERM);

	rdtscll(walk[POS(order)].p->last_unmap);
	cos_mem_fence();
	
	return 0;
err:
	/* Prepare to unwind and quit */
	for ( ; i <= POS(MAX_PAGE_ORDER); i++) {
		if (walk[i].inc_cnt != 0) walk[i].p->refcnt_atom.v++;
	}
	return ret;
}

static inline int
atomic_type_swap(void* ptr, int old_type, int new_type, int clear)
{
	struct retype_entry temp, old_temp;

	old_temp.refcnt_atom.v = ((struct retype_entry*)(ptr))->refcnt_atom.v;
	if (clear != 0) {
		temp.refcnt_atom.v = 0;
	} else {
		temp.refcnt_atom.v = old_temp.refcnt_atom.v;
	}
	if (old_type != -1) old_temp.refcnt_atom.type = old_type;
	temp.refcnt_atom.type = new_type;

	return retypetbl_cas((u32_t*)ptr, old_temp.refcnt_atom.v, temp.refcnt_atom.v);
}

static inline int
mod_mem_type(void *pa, u32_t order, const mem_type_t type)
{
	struct page_record      walk[NUM_PAGE_SIZES];
	int                     i, ret, val;
	u32_t                   idx, old_type;
	struct retype_entry_glb temp;
	struct retype_entry_glb old_temp;
	struct retype_entry_glb *glb_retype_info;

	assert(pa); /* cannot be NULL: kernel image takes that space */
	assert(order == 12); /* only need/have to use 4kb page */
	memset(walk, 0, sizeof(walk));
	memset(&old_temp, 0, sizeof(old_temp));
	memset(&temp, 0, sizeof(temp));
	glb_retype_info = 0;
	PA_BOUNDARY_CHECK();

	idx = GET_MEM_IDX(pa);
	assert(idx < N_MEM_SETS);

	/* Kernel memory needs to be kernel accessible: pa2va returns null if it's not */
	if (type == RETYPETBL_KERN && chal_pa2va((paddr_t)pa) == NULL) return -EINVAL;

	/* Get the global slot */
    walk[POS(MAX_PAGE_ORDER)].p_glb = GET_GLB_RETYPE_ENTRY(idx, MAX_PAGE_ORDER);
	old_type = walk[POS(MAX_PAGE_ORDER)].p_glb->refcnt_atom.type;

	/* only can retype untyped mem sets. */
	if (unlikely(old_type != RETYPETBL_UNTYPED)) {
		if (old_type == type)
			return -EEXIST;
		else
			return -EPERM;
	}

	/* Repetitively add the record of a page set to a type, into the level one above it */ 
	for (i = POS(MAX_PAGE_ORDER) - 1; i >= POS(order); i--) {
	    walk[i].p_glb = GET_GLB_RETYPE_ENTRY(idx, ORDER(i));
		walk[i].inc_cnt = 0;
		/* Do we need to update the next highest page size count? */ 
		if (walk[i].p_glb->refcnt_atom.type == RETYPETBL_UNTYPED || 
		    (((type == RETYPETBL_USER) && (walk[i].p_glb->refcnt_atom.user_cnt == 0)) ||
		     ((type == RETYPETBL_KERN) && (walk[i].p_glb->refcnt_atom.kernel_cnt == 0)))) {
			old_temp.refcnt_atom.v = walk[i + 1].p_glb->refcnt_atom.v; 
			if (old_temp.refcnt_atom.type != RETYPETBL_UNTYPED) cos_throw(err, -EPERM); 
			temp.refcnt_atom.v = old_temp.refcnt_atom.v;
			if (type == RETYPETBL_USER) temp.refcnt_atom.user_cnt++; else temp.refcnt_atom.kernel_cnt++;
			/*
			 * Use CAS to update the word here in case there is a retype/retype race.
			 * If another retype retypes the parent, then the type check in this CAS
			 * will fail. This CAS makes sure that the parent will not be retyped into
			 * something else.
			 */
			if (retypetbl_cas((u32_t*)(walk[i + 1].p_glb), old_temp.refcnt_atom.v, temp.refcnt_atom.v) != CAS_SUCCESS) cos_throw(err, -ECASFAIL);
			/* This is a simple assignment, because this is the function's local variable */
			walk[i + 1].inc_cnt = 1;
		}
		if (walk[i].p_glb->refcnt_atom.type != RETYPETBL_UNTYPED) cos_throw(err, -ECASFAIL);
	}

	/* typed as the other type? - if there is a retype/retype race */
	if (((temp.refcnt_atom.type == RETYPETBL_KERN) && (type == RETYPETBL_USER)) ||
	    ((temp.refcnt_atom.type == RETYPETBL_USER) && (type == RETYPETBL_KERN))) cos_throw(err, -ECASFAIL);

	/* atomic update with CAS */
	ret = atomic_type_swap(walk[POS(order)].p_glb, RETYPETBL_UNTYPED, RETYPETBL_RETYPING, 1);
	if (ret != CAS_SUCCESS) cos_throw(err, -ECASFAIL);
	cos_mem_fence();

	/* 
	 * Set the retyping flag successfully. Now nobody else can
	 * change this memory set. Update the per-core retype entries
	 * next.
	 */
	for (i = 0; i < NUM_CPU; i++) GET_RETYPE_CPU_ENTRY(i, idx, order)->refcnt_atom.type = type;
	cos_mem_fence();

	/* Now commit the change to the global entry. */
	ret = atomic_type_swap(walk[POS(order)].p_glb, RETYPETBL_RETYPING, type, 1);
	assert(ret == CAS_SUCCESS);

	return 0;
err:
	/* Prepare to unwind and quit */
	if (type == RETYPETBL_USER) val = 1 << RETYPE_ENT_GLB_REFCNT_SZ; else val = 1;
	for ( ;i <= POS(MAX_PAGE_ORDER); i++) {
		if (walk[i].inc_cnt != 0) cos_faa((int*)walk[i].p_glb, -val);
	}

	return ret;
}

int
retypetbl_retype2user(void *pa, u32_t order)
{
	return mod_mem_type(pa, order, RETYPETBL_USER);
}

int
retypetbl_retype2kern(void *pa, u32_t order)
{
	return mod_mem_type(pa, order, RETYPETBL_KERN);
}

/* implemented in pgtbl.c */
int tlb_quiescence_check(u64_t unmap_time);

/* Retype something back to frame */
int
retypetbl_retype2frame(void *pa, u32_t order)
{
	struct retype_entry_glb *glb_retype_info;
	struct retype_entry     *retype_info;
	union refcnt_atom       local_u;
	u32_t                   old_v, idx;
	u64_t                   last_unmap;
	int                     i, j, cpu, ret, sum;
	mem_type_t              old_type;

	PA_BOUNDARY_CHECK();

	idx = GET_MEM_IDX(pa);
	assert(idx < N_MEM_SETS);

	/* Exact page granularity */
	glb_retype_info = GET_GLB_RETYPE_ENTRY(idx, order);
	old_type        = glb_retype_info->refcnt_atom.type;
	/* Only can untype typed mem sets */
	if (unlikely(old_type != RETYPETBL_USER && old_type != RETYPETBL_KERN)) return -EPERM;

	/* lock down the memory set we are going to untype */
	ret = atomic_type_swap(glb_retype_info, -1, RETYPETBL_RETYPING, 0);
	if (ret != CAS_SUCCESS) return ret;

	for (i = 0; i < NUM_CPU; i++) {
		retype_info = GET_RETYPE_CPU_ENTRY(i, idx, order);
		/* See if we can successfully lock it */
		if (atomic_type_swap(retype_info, -1, RETYPETBL_RETYPING, 0) != CAS_SUCCESS) cos_throw(err, -EINVAL);
	}

	/* Now all of them are frozen. add up the counts to see this still mapped somewhere */
	last_unmap = 0;
	sum = 0;
	for (cpu = 0; cpu < NUM_CPU; cpu++) {
		sum += GET_RETYPE_CPU_ENTRY(cpu, idx, order)->refcnt_atom.ref_cnt;
		/* for tlb quiescence check */
		if (last_unmap < GET_RETYPE_CPU_ENTRY(cpu, idx, order)->last_unmap) {
			last_unmap = GET_RETYPE_CPU_ENTRY(cpu, idx, order)->last_unmap;
		}
	}

	/* Only can retype when there's no more mapping */
	if (sum != 0) cos_throw(err, -EINVAL);

	/* 
	 * Before retype any type of memory back to untyped, we need
	 * to make sure TLB quiescence has been achieved after the
	 * last unmapping of any pages in this memory set
	 */
	if (!tlb_quiescence_check(last_unmap)) cos_throw(err, -EQUIESCENCE);
	cos_mem_fence();

	/* Repetitively add the record of a page set to a type, into the level one above it */
	if (old_type == RETYPETBL_USER) sum = (1 << RETYPE_ENT_GLB_REFCNT_SZ); else sum = 1;
	for (j = POS(MAX_PAGE_ORDER); j > POS(order); j--) {
		glb_retype_info = GET_GLB_RETYPE_ENTRY(idx, ORDER(j));
		/* The following is atomic with FAA */
		cos_faa((int*)glb_retype_info, -sum);
	}

	/* Update all the per-cpu variables one by one */
	for (cpu = 0; cpu < NUM_CPU; cpu++) {
		retype_info = GET_RETYPE_CPU_ENTRY(cpu, idx, order);
		retype_info->refcnt_atom.v = 0;
	}
	assert(atomic_type_swap(glb_retype_info, RETYPETBL_RETYPING, RETYPETBL_UNTYPED, 0) == CAS_SUCCESS);

	return 0;
err:
	/* restore per-core retype entries. (is this necessary?) */
	/* cpu is the one we tried to modify but fail. So we need to
	 * restore [0, cpu-1]. */
	for (i--; i >= 0; i--) {
		retype_info = GET_RETYPE_CPU_ENTRY(i, idx, order);
		atomic_type_swap(retype_info, -1, old_type, 0);
	}
	/* Restore the global entry to old type before return:
	 * only us can change it. */
	assert(atomic_type_swap(glb_retype_info, RETYPETBL_RETYPING, old_type, 0) == CAS_SUCCESS);

	return ret;
}

void
retype_tbl_init(void)
{
	int i, j;

	/* Alignment & size checks! */
	assert(sizeof(struct retype_info) % CACHE_LINE == 0);
	assert(sizeof(struct retype_info_glb) % CACHE_LINE == 0);
	assert(sizeof(retype_tbl) % CACHE_LINE == 0);
	assert(sizeof(glb_retype_tbl) % CACHE_LINE == 0);
	assert((unsigned long)retype_tbl % CACHE_LINE == 0);
	assert((unsigned long)glb_retype_tbl % CACHE_LINE == 0);

	assert(sizeof(union refcnt_atom) == sizeof(u32_t));
	assert(RETYPE_ENT_TYPE_SZ + RETYPE_ENT_REFCNT_SZ == 32);
	assert(CACHE_LINE % sizeof(struct retype_entry) == 0);

	for (i = 0; i < NUM_CPU; i++) {
		for (j = 0; j < N_RETYPE_SLOTS; j++) {
			retype_tbl[i].mem_set[j].refcnt_atom.type    = RETYPETBL_UNTYPED;
			retype_tbl[i].mem_set[j].refcnt_atom.ref_cnt = 0;
			retype_tbl[i].mem_set[j].last_unmap          = 0;
		}
	}

	for (i = 0; i < N_RETYPE_SLOTS; i++) {
		glb_retype_tbl[i].info.refcnt_atom.type = RETYPETBL_UNTYPED;
		glb_retype_tbl[i].info.refcnt_atom.user_cnt = 0;
		glb_retype_tbl[i].info.refcnt_atom.kernel_cnt = 0;
		glb_retype_tbl[i].info.kernel_ref = 0;
	}

	cos_mem_fence();

	return;
}
