/**
 * Copyright 2014 by Qi Wang, interwq@gwu.edu
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 *
 */

#ifndef RETYPE_TBL_H
#define RETYPE_TBL_H

#include "shared/cos_types.h"
#include "chal/shared/cos_config.h"
#include "chal/shared/util.h"
#include "chal/cpuid.h"
#include "chal.h"

typedef enum {
	RETYPETBL_UNTYPED  = 0, /* untyped physical frames. */
	RETYPETBL_USER     = 1,
	RETYPETBL_KERN     = 2,
	RETYPETBL_RETYPING = 3, /* ongoing retyping operation */
} mem_type_t;

#define RETYPE_ENT_TYPE_SZ COS_PGTBL_NUM_ORDER
#define RETYPE_ENT_REFCNT_SZ (sizeof(u32_t) * 8 - RETYPE_ENT_TYPE_SZ)
#define RETYPE_ENT_GLB_REFCNT_SZ (RETYPE_ENT_REFCNT_SZ / 2)
#define RETYPE_REFCNT_MAX ((1 << RETYPE_ENT_REFCNT_SZ) - 1)
#define RETYPE_REFCNT_GLB_MAX ((1 << RETYPE_ENT_GLB_REFCNT_SZ) - 1)

/* 
 * This maintains the information of a collection of physical pages
 * (of one core).
 */
struct retype_entry {
	union refcnt_atom {
		struct {
			/* 
			 * The ref_cnt is the # of mappings in user space for user
			 * memory, or # of mappings in kernel space for kernel memory.
			 */
			int ref_cnt : RETYPE_ENT_REFCNT_SZ;
			mem_type_t type : RETYPE_ENT_TYPE_SZ;
		} __attribute__((packed));
		u32_t v;
	} refcnt_atom;
	u32_t __pad; /* let's make the size power of 2. */
	/* 
	 * The timestamp of when the last unmapped happened. Used to
	 * track TLB quiescence when retype.
	 */
	u64_t last_unmap;
} __attribute__((packed));

/*
 * This maintains the information of a collection of physical pages
 * (globally).
 */
struct retype_entry_glb {
	union refcnt_atom_glb {
		struct {
			/* How many smaller subpages are typed as kernel? */
			int kernel_cnt : RETYPE_ENT_GLB_REFCNT_SZ;
			/* How many smaller subpages are typed as user-level? */
			int user_cnt : RETYPE_ENT_GLB_REFCNT_SZ;
			mem_type_t type : RETYPE_ENT_TYPE_SZ;
		} __attribute__((packed));
		u32_t v;
	} refcnt_atom;
	/* FIXME: Move this to core-local */
	/* How many kernel pages are used as kernel objects in this memory set? */
	u32_t kernel_ref;
} __attribute__((packed));

/* When getting kernel memory through Linux, the retype table is
 * partitioned into two parts for kmem and user memory. */
/* # of mem_sets in total. +1 to be safe. */
#define N_USER_MEM_SETS (COS_MAX_MEMORY / RETYPE_MEM_NPAGES + 1)
#define N_KERN_MEM_SETS (COS_KERNEL_MEMORY / RETYPE_MEM_NPAGES + 1)

#define N_MEM_SETS (N_USER_MEM_SETS + N_KERN_MEM_SETS)

/* FIXME:move these macros to architecture-specific parts */
#define N_RETYPE_SLOTS (N_MEM_SETS + N_MEM_SETS / 1024 + 1)
#define N_RETYPE_SLOTS_SZ (sizeof(struct retype_entry) * N_RETYPE_SLOTS)

/* per-cpu retype_info. Each core update this locally when doing
 * mapping/unmapping. */
struct retype_info {
	struct retype_entry mem_set[N_RETYPE_SLOTS];
	char                __pad[(N_RETYPE_SLOTS_SZ % CACHE_LINE == 0) ? 0 : (N_RETYPE_SLOTS_SZ % CACHE_LINE)];
} CACHE_ALIGNED;

extern struct retype_info retype_tbl[NUM_CPU];

/* global retype_info. We should only touch this when doing
 * retyping. NOT when doing mapping / unmapping. */
struct retype_info_glb {
	struct retype_entry_glb info;
	char  __pad[CACHE_LINE - sizeof(struct retype_entry_glb)];
} CACHE_ALIGNED;

extern struct retype_info_glb glb_retype_tbl[N_RETYPE_SLOTS];

#define COS_KMEM_BOUND (chal_kernel_mem_pa + PAGE_SIZE * COS_KERNEL_MEMORY)

/* physical address boundary check */
#define PA_BOUNDARY_CHECK()                                                                            \
	do {                                                                                           \
		if (unlikely(!(((unsigned long)pa >= COS_MEM_START) && ((unsigned long)pa < COS_MEM_BOUND))            \
			     && !(((unsigned long)pa >= chal_kernel_mem_pa) && ((unsigned long)pa < COS_KMEM_BOUND)))) \
			return -EINVAL;                                                                \
	} while (0)

/* get the index of the memory set. */
#define GET_MEM_IDX(pa)                                                                 \
	(((unsigned long)pa >= COS_MEM_START) ? (((unsigned long)(pa) - COS_MEM_START) / RETYPE_MEM_SIZE) \
	                              : (((unsigned long)(pa) - chal_kernel_mem_pa) / RETYPE_MEM_SIZE + N_USER_MEM_SETS))
/* The minimum/maximum page order - currently 4kB/1MB pages. This is fine because we don't support large pages on ARM */
#define MIN_PAGE_ORDER               12
#define MAX_PAGE_ORDER               22
#define NUM_PAGE_SIZES               2
/* get the memory set struct of the current cpu */
#define GET_RETYPE_POS(idx,order)    (((idx) >> ((order) - MIN_PAGE_ORDER)) + pos2base[order2pos[order]])
#define GET_RETYPE_ENTRY(idx, order) ((&(retype_tbl[get_cpuid()].mem_set[GET_RETYPE_POS(idx,order)])))
#define GET_RETYPE_CPU_ENTRY(cpu, idx, order) ((&(retype_tbl[cpu].mem_set[GET_RETYPE_POS(idx,order)])))
/* get the global memory set struct (used for retyping only). */
#define GET_GLB_RETYPE_ENTRY(idx, order) ((&(glb_retype_tbl[GET_RETYPE_POS(idx,order)].info)))

#define cos_throw(label, errno) \
	{                       \
		ret = (errno);  \
		goto label;     \
	}

static inline int
retypetbl_cas(u32_t *a, u32_t old, u32_t new)
{
	return cos_cas_32((u32_t *)a, old, new);
}

int retypetbl_retype2user(void *pa, u32_t order);
int retypetbl_retype2kern(void *pa, u32_t order);
int retypetbl_retype2frame(void *pa, u32_t order);

void retype_tbl_init(void);
int  retypetbl_ref(void *pa, u32_t order);
int  retypetbl_deref(void *pa, u32_t order);
int  retypetbl_kern_ref(void *pa, u32_t order);
int  retypetbl_kern_deref(void *pa, u32_t order);

#endif /* RETYPE_TBL_H */
