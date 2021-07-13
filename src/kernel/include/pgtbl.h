/**
 * Copyright 2014 by Gabriel Parmer, gparmer@gwu.edu
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 */

#ifndef PGTBL_H
#define PGTBL_H

#include "shared/cos_errno.h"
#include "ertrie.h"
#include "chal/shared/util.h"
#include "captbl.h"
#include "retype_tbl.h"
#include "liveness_tbl.h"
#include "chal/defs.h"
#include "chal/chal_proto.h"

#ifndef LINUX_TEST
#include "chal.h"
#endif

/* Generic page table flags */
typedef enum {
	PGTBL_PRESENT  = 1,
	PGTBL_WRITABLE = 1 << 1,
	PGTBL_USER     = 1 << 2,
	PGTBL_WT       = 1 << 3, /* write-through caching */
	PGTBL_NOCACHE  = 1 << 4, /* caching disabled */
	PGTBL_ACCESSED = 1 << 5,
	PGTBL_MODIFIED = 1 << 6,
	PGTBL_SUPER    = 1 << 7, /* super-page (4MB on x86-32) */
	PGTBL_GLOBAL   = 1 << 8,
	/* Composite defined bits next*/
	PGTBL_COSFRAME   = 1 << 9,
	PGTBL_COSKMEM    = 1 << 10, /* page activated as kernel object */
	PGTBL_QUIESCENCE = 1 << 11,
	/* Flag bits done. */
	PGTBL_USER_DEF   = PGTBL_PRESENT | PGTBL_USER | PGTBL_ACCESSED | PGTBL_MODIFIED | PGTBL_WRITABLE,
	PGTBL_INTERN_DEF = PGTBL_USER_DEF,
} pgtbl_flags_t;

struct tlb_quiescence {
	/* Updated by timer. */
	u64_t last_periodic_flush;
	/* Updated by tlb flush IPI. */
	u64_t last_mandatory_flush;
	/* cacheline size padding. */
	u8_t __padding[CACHE_LINE - 2 * sizeof(u64_t)];
} __attribute__((aligned(CACHE_LINE), packed));

extern struct tlb_quiescence tlb_quiescence[NUM_CPU] CACHE_ALIGNED;

int            tlb_quiescence_check(u64_t timestamp);
int            pgtbl_cosframe_add(pgtbl_t pt, unsigned long addr, unsigned long page, u32_t flags, u32_t order);
int            pgtbl_mapping_add(pgtbl_t pt, unsigned long addr, unsigned long page, u32_t flags, u32_t order);
int            pgtbl_mapping_mod(pgtbl_t pt, u32_t addr, u32_t flags, u32_t *prevflags);
int            pgtbl_mapping_del(pgtbl_t pt, u32_t addr, u32_t liv_id);
int            pgtbl_mapping_del_direct(pgtbl_t pt, u32_t addr);
void          *pgtbl_lkup_lvl(pgtbl_t pt, unsigned long addr, u32_t *flags, u32_t start_lvl, u32_t end_lvl);
int            pgtbl_ispresent(u32_t flags);
unsigned long *pgtbl_lkup(pgtbl_t pt, u32_t addr, u32_t *flags);
unsigned long *pgtbl_lkup_pte(pgtbl_t pt, u32_t addr, u32_t *flags);
unsigned long *pgtbl_lkup_pgd(pgtbl_t pt, u32_t addr, u32_t *flags);
int            pgtbl_get_cosframe(pgtbl_t pt, vaddr_t frame_addr, paddr_t *cosframe, vaddr_t *order);
vaddr_t        pgtbl_translate(pgtbl_t pt, u32_t addr, u32_t *flags);
pgtbl_t        pgtbl_create(void *page, void *curr_pgtbl);
int            pgtbl_activate(struct captbl *t, unsigned long cap, unsigned long capin, pgtbl_t pgtbl, u32_t lvl);
int            pgtbl_deactivate(struct captbl *t, struct cap_captbl *dest_ct_cap, unsigned long capin, livenessid_t lid,
                                capid_t pgtbl_cap, capid_t cosframe_addr, const int root);
int            pgtbl_mapping_scan(struct cap_pgtbl *pt);
int            pgtbl_quie_check(u32_t orig_v);
void           pgtbl_init_pte(void *pte);

static inline void
pgtbl_update(struct pgtbl_info *ptinfo)
{
	chal_pgtbl_update(ptinfo);
}

extern unsigned long __cr3_contents;

static void
pgtbl_init(void)
{
	assert(sizeof(struct cap_pgtbl) <= __captbl_cap2bytes(CAP_PGTBL));

	return;
}

extern void kmem_unalloc(unsigned long *pte);
int cap_memactivate(struct captbl *ct, struct cap_pgtbl *pt, capid_t frame_cap, capid_t dest_pt, vaddr_t vaddr, vaddr_t order);
int pgtbl_kmem_act(pgtbl_t pt, u32_t addr, unsigned long *kern_addr, unsigned long **pte);

/* Chal related function prototypes */
/* Do flag transformations */
unsigned long  chal_pgtbl_flag_add(unsigned long input, pgtbl_flags_t flags);
unsigned long  chal_pgtbl_flag_clr(unsigned long input, pgtbl_flags_t flags);
unsigned long  chal_pgtbl_flag_exist(unsigned long input, pgtbl_flags_t flags);
unsigned long  chal_pgtbl_flag_all(unsigned long input, pgtbl_flags_t flags);
unsigned long  chal_pgtbl_frame(unsigned long input);
unsigned long  chal_pgtbl_flag(unsigned long input);

int            chal_pgtbl_kmem_act(pgtbl_t pt, unsigned long addr, unsigned long *kern_addr, unsigned long **pte_ret);
int            chal_tlb_quiescence_check(u64_t timestamp);
int            chal_cap_memactivate(struct captbl *ct, struct cap_pgtbl *pt, capid_t frame_cap, capid_t dest_pt, vaddr_t vaddr, vaddr_t order);
int            chal_pgtbl_activate(struct captbl *t, unsigned long cap, unsigned long capin, pgtbl_t pgtbl, u32_t lvl);
int            chal_pgtbl_deactivate(struct captbl *t, struct cap_captbl *dest_ct_cap, unsigned long capin,
                                     livenessid_t lid, capid_t pgtbl_cap, capid_t cosframe_addr, const int root);

int            chal_pgtbl_mapping_add(pgtbl_t pt, unsigned long addr, unsigned long page, u32_t flags, u32_t order);
int            chal_pgtbl_cosframe_add(pgtbl_t pt, unsigned long addr, unsigned long page, u32_t flags, u32_t order);
/* This function updates flags of an existing mapping. */
int            chal_pgtbl_mapping_mod(pgtbl_t pt, unsigned long addr, u32_t flags, u32_t *prevflags);
int            chal_pgtbl_mapping_del(pgtbl_t pt, unsigned long addr, u32_t liv_id);
int            chal_pgtbl_mapping_del_direct(pgtbl_t pt, u32_t addr);
int            chal_pgtbl_mapping_scan(struct cap_pgtbl *pt);
void          *chal_pgtbl_lkup_lvl(pgtbl_t pt, unsigned long addr, u32_t *flags, u32_t start_lvl, u32_t end_lvl);
int            chal_pgtbl_ispresent(u32_t flags);
unsigned long *chal_pgtbl_lkup(pgtbl_t pt, u32_t addr, u32_t *flags);
unsigned long *chal_pgtbl_lkup_pte(pgtbl_t pt, u32_t addr, u32_t *flags);
unsigned long *chal_pgtbl_lkup_pgd(pgtbl_t pt, u32_t addr, u32_t *flags);
int            chal_pgtbl_get_cosframe(pgtbl_t pt, vaddr_t frame_addr, paddr_t *cosframe, vaddr_t *order);
pgtbl_t        chal_pgtbl_create(void *page, void *curr_pgtbl);
int            chal_pgtbl_quie_check(u32_t orig_v);
void           chal_pgtbl_init_pte(void *pte);

/* Creation of the table object - not to be confused with activation of cap */
int            chal_pgtbl_pgtblactivate(struct captbl *ct, capid_t cap, capid_t pt_entry, capid_t pgtbl_cap, vaddr_t kmem_cap, capid_t pgtbl_order);
/* Deactivate */
int            chal_pgtbl_deact_pre(struct cap_header *ch, u32_t pa);
/* Page mapping */
int            chal_pgtbl_cpy(struct captbl *t, capid_t cap_to, capid_t capin_to, struct cap_pgtbl *ctfrom, capid_t capin_from, cap_t cap_type, vaddr_t order);
/* Cons & decons functions */
int            chal_pgtbl_cons(struct cap_captbl *ct, struct cap_captbl *ctsub, capid_t expandid, unsigned long depth);
int            chal_pgtbl_decons(struct cap_header *head, struct cap_header *sub, capid_t pruneid, unsigned long lvl);
/* Introspection */
int            chal_pgtbl_introspect(struct cap_header *ch, vaddr_t addr);

#endif /* PGTBL_H */

