#ifndef CHAL_PLAT_H
#define CHAL_PLAT_H

int chal_tlb_lockdown(unsigned long entryid, unsigned long vaddr, unsigned long paddr);
int chal_l1flush(void);
int chal_tlbstall(void);
int chal_tlbstall_recount(int a);
int chal_tlbflush(int a);

/* This flushes all levels of cache of the current logical CPU. */
static inline void
chal_flush_cache(void)
{
	asm volatile("wbinvd" : : : "memory");
}

static inline void
chal_flush_tlb_global(void)
{
}
static inline void
chal_remote_tlb_flush(int target_cpu)
{
}
/* This won't flush global TLB (pinned with PGE) entries. */
static inline void
chal_flush_tlb(void)
{
}

static inline void *
chal_pa2va(paddr_t address)
{
	return (void *)(address + COS_MEM_KERN_START_VA);
}

static inline paddr_t
chal_va2pa(void *address)
{
	return (paddr_t)((u32_t)address - COS_MEM_KERN_START_VA);
}

#endif /* CHAL_PLAT_H */
