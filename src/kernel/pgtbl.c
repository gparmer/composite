#include "include/shared/cos_types.h"
#include "include/captbl.h"
#include "include/pgtbl.h"
#include "include/cap_ops.h"
#include "include/liveness_tbl.h"
#include "include/retype_tbl.h"
#include "chal/chal_proto.h"

int
pgtbl_kmem_act(pgtbl_t pt, u32_t addr, unsigned long *kern_addr, unsigned long **pte_ret)
{
	return chal_pgtbl_kmem_act(pt, addr, kern_addr, pte_ret);
}

int
tlb_quiescence_check(u64_t timestamp)
{
	return chal_tlb_quiescence_check(timestamp);
}

int
cap_memactivate(struct captbl *ct, struct cap_pgtbl *pt, capid_t frame_cap, capid_t dest_pt, vaddr_t vaddr, vaddr_t order)
{
	return chal_cap_memactivate(ct, pt, frame_cap, dest_pt, vaddr, order);
}

int
pgtbl_activate(struct captbl *t, unsigned long cap, unsigned long capin, pgtbl_t pgtbl, u32_t lvl)
{
	return chal_pgtbl_activate(t, cap, capin, pgtbl, lvl);
}

int
pgtbl_deactivate(struct captbl *t, struct cap_captbl *dest_ct_cap, unsigned long capin,
                 livenessid_t lid, capid_t pgtbl_cap, capid_t cosframe_addr, const int root)
{
	return chal_pgtbl_deactivate(t, dest_ct_cap, capin, lid, pgtbl_cap, cosframe_addr, root);
}

int
pgtbl_mapping_add(pgtbl_t pt, unsigned long addr, unsigned long page, u32_t flags, u32_t order)
{
	return chal_pgtbl_mapping_add(pt, addr, page, flags, order);
}

int
pgtbl_cosframe_add(pgtbl_t pt, unsigned long addr, unsigned long page, u32_t flags, u32_t order)
{
	return chal_pgtbl_cosframe_add(pt, addr, page, flags, order);
}

int
pgtbl_mapping_mod(pgtbl_t pt, u32_t addr, u32_t flags, u32_t *prevflags)
{
	return chal_pgtbl_mapping_mod(pt, addr, flags, prevflags);
}

int
pgtbl_mapping_del(pgtbl_t pt, u32_t addr, u32_t liv_id)
{
	return chal_pgtbl_mapping_del(pt, addr, liv_id);
}

/* 
 * NOTE: This just removes the mapping. NO liveness tracking! TLB
 * flush should be taken care of separately (and carefully).
 */
int
pgtbl_mapping_del_direct(pgtbl_t pt, u32_t addr)
{
	return chal_pgtbl_mapping_del_direct(pt, addr);
}

int
pgtbl_mapping_scan(struct cap_pgtbl *pt)
{
	return chal_pgtbl_mapping_scan(pt);
}

void *
pgtbl_lkup_lvl(pgtbl_t pt, unsigned long addr, u32_t *flags, u32_t start_lvl, u32_t end_lvl)
{
	return chal_pgtbl_lkup_lvl(pt, addr, flags, start_lvl, end_lvl);
}

int
pgtbl_ispresent(u32_t flags)
{
	return chal_pgtbl_ispresent(flags);
}

unsigned long *
pgtbl_lkup(pgtbl_t pt, u32_t addr, u32_t *flags)
{
	return chal_pgtbl_lkup(pt, addr, flags);
}

unsigned long *
pgtbl_lkup_pte(pgtbl_t pt, u32_t addr, u32_t *flags)
{
	return chal_pgtbl_lkup_pte(pt, addr, flags);
}

unsigned long *
pgtbl_lkup_pgd(pgtbl_t pt, u32_t addr, u32_t *flags)
{
	return chal_pgtbl_lkup_pgd(pt, addr, flags);
}

int
pgtbl_get_cosframe(pgtbl_t pt, vaddr_t frame_addr, paddr_t *cosframe, vaddr_t *order)
{
	return chal_pgtbl_get_cosframe(pt, frame_addr, cosframe, order);
}

vaddr_t
pgtbl_translate(pgtbl_t pt, u32_t addr, u32_t *flags)
{
	return (vaddr_t)pgtbl_lkup(pt, addr, flags);
}

pgtbl_t
pgtbl_create(void *page, void *curr_pgtbl)
{
	return chal_pgtbl_create(page, curr_pgtbl);
}

int
pgtbl_quie_check(u32_t orig_v)
{
	return chal_pgtbl_quie_check(orig_v);
}

void
pgtbl_init_pte(void *pte)
{
	return chal_pgtbl_init_pte(pte);
}
