#include "assert.h"
#include "kernel.h"
#include "boot_comp.h"
#include "chal_cpu.h"
#include "chal/chal_proto.h"
#include "chal_pgtbl.h"
#include "mem_layout.h"
#include "string.h"
#include <pgtbl.h>
#include <thd.h>
#include <component.h>
#include <inv.h>
#include <hw.h>
#include <shared/elf_loader.h>

extern u8_t *boot_comp_pgd;

void *thd_mem[NUM_CPU], *tcap_mem[NUM_CPU];
struct captbl *glb_boot_ct;

/* FIXME:  loops to create threads/tcaps/rcv caps per core. */
static void
kern_boot_thd(struct captbl *ct, void *thd_mem, void *tcap_mem, const cpuid_t cpu_id)
{
	struct cos_cpu_local_info *cos_info = cos_cpu_local_info();
	struct thread *            t        = thd_mem;
	struct tcap *              tc       = tcap_mem;
	tcap_res_t                 expended;
	int                        ret;
	//struct cap_pgtbl *         cap_pt;
	//pgtbl_t                    pgtbl;
	struct cap_comp *          cap_comp;

	assert(cpu_id >= 0);
	assert(cos_info->cpuid == (u32_t)cpu_id);
	assert(sizeof(struct cos_cpu_local_info) == STK_INFO_SZ);
	memset(cos_info, 0, sizeof(struct cos_cpu_local_info));
	cos_info->cpuid          = cpu_id;
	cos_info->invstk_top     = 0;
	cos_info->overflow_check = 0xDEADBEEF;
	ret = thd_activate(ct, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_INITTHD_BASE_CPU(cpu_id), thd_mem, BOOT_CAPTBL_SELF_COMP, 0);
	assert(!ret);

	tcap_active_init(cos_info);
	ret = tcap_activate(ct, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_INITTCAP_BASE_CPU(cpu_id), tcap_mem);
	assert(!ret);

	tc->budget.cycles = TCAP_RES_INF; /* Chronos's got all the time in the world */
	tc->perm_prio     = 0;
	tcap_setprio(tc, 0);                              /* Chronos gets preempted by no one! */
	list_enqueue(&cos_info->tcaps, &tc->active_list); /* Chronos on the TCap active list */
	cos_info->tcap_uid  = 1;
	cos_info->cycles    = tsc();
	cos_info->curr_tcap = tc;
	thd_next_thdinfo_update(cos_info, 0, 0, 0, 0);

	thd_current_update(t, t, cos_info);
	thd_scheduler_set(t, t);

	ret = arcv_activate(ct, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_INITRCV_BASE_CPU(cpu_id), BOOT_CAPTBL_SELF_COMP,
	                    BOOT_CAPTBL_SELF_INITTHD_BASE_CPU(cpu_id), BOOT_CAPTBL_SELF_INITTCAP_BASE_CPU(cpu_id), 0, 1);
	assert(!ret);

	/* switching to boot component's PGD using component capability */
	cap_comp = (struct cap_comp *)captbl_lkup(ct, BOOT_CAPTBL_SELF_COMP);
	if (!cap_comp || !CAP_TYPECHK(cap_comp, CAP_COMP)) assert(0);
	assert(cap_comp->info.pgtblinfo.pgtbl);
	pgtbl_update(&cap_comp->info.pgtblinfo);


	printk("\tCreating initial threads, tcaps, and rcv end-points in boot-component.\n");
}

static vaddr_t
mem_bootc_entry(void)
{
	return elf_entry_addr((struct elf_hdr *)mem_bootc_start());
}

static int
boot_nptes(unsigned long vaddr_start, unsigned long sz, int lvl)
{
	/* 
	* Need the start address for edge cases
	* 4 lvl page tables, each lvl is: 0, 1, 2, 3
	* will return the number of lvl-th page tables needed.
	*/	
	assert(lvl > 0);
	assert(sz > 0);
	unsigned long end_addr = (vaddr_start + sz - 1) >> PAGE_ORDER;
	vaddr_start = vaddr_start >> PAGE_ORDER;
	return ((end_addr >> ((4 - lvl) * 9)) - (vaddr_start >> ((4 - lvl) * 9)) + 1);
}

static void
boot_pgtbl_expand(struct captbl *ct, capid_t pgdcap, capid_t ptecap, const char *label,
		  unsigned long user_vaddr, unsigned long range)
{
	int               ret;
	u8_t *            ptes;
	unsigned int      nptes = 0, lvl = 0, i;
	struct cap_pgtbl *pte_cap, *pgd_cap;

	if (pgtbl_activate(ct, BOOT_CAPTBL_SELF_CT, ptecap, NULL, 1)) assert(0);
	for (lvl=1; lvl < 4; lvl++){
		nptes = boot_nptes(user_vaddr, range, lvl);
		ptes  = mem_boot_alloc(nptes);
		assert(ptes);

		printk("\tCreating %d %d-lvl %s pages from [%x,%x) for v_addr [%x,%x).\n", nptes, lvl ,label,
	       ptes, ptes + nptes * PAGE_SIZE, user_vaddr, user_vaddr + range);

		/*
		* Note the use of NULL here.  We aren't actually adding a PTE
		* currently.  This is a hack and only used on boot-up.  We'll
		* reuse this capability entry to create _multiple_ ptes.  We
		* won't create captbl entries for each of them, so they
		* cannot be aliased/removed later.  The only adverse
		* side-effect I can think of from this is that we cannot
		* reclaim all of the boot-time memory, but that is so far
		* into the future, I don't think we care.
		*/
		pte_cap = (struct cap_pgtbl *)captbl_lkup(ct, ptecap);
		assert(pte_cap);
		/* Hook in the PTEs */
		for (i = 0; i < nptes; i++) {
			u8_t *  p  = ptes + i * PAGE_SIZE;
			paddr_t pf = chal_va2pa(p);

			pgtbl_init_pte(p);
			pte_cap->pgtbl = (pgtbl_t)p;
			pte_cap->lvl = lvl;
			/* hook the pte into the boot component's page tables */
			ret = cap_cons(ct, pgdcap, ptecap, (capid_t)(user_vaddr + i * (1 << (12 + (4 - lvl) * 9))));
			assert(!ret);
		}
	}
	
	return;
}

int
boot_pgtbl_mappings_add(struct captbl *ct, capid_t pgdcap, capid_t ptecap, const char *label, void *kern_vaddr,
                        unsigned long user_vaddr, unsigned int range, int uvm)
{
	int               ret;
	u8_t *            ptes;
	unsigned long int      nptes = 0, i;
	struct cap_pgtbl *pte_cap, *pgd_cap;
	pgtbl_t           pgtbl;

	pgd_cap = (struct cap_pgtbl *)captbl_lkup(ct, pgdcap);
	if (!pgd_cap || !CAP_TYPECHK(pgd_cap, CAP_PGTBL)) assert(0);
	pgtbl = (pgtbl_t)pgd_cap->pgtbl;

	printk("\tMapping in %s (@ [0x%p, 0x%p))\n", label, user_vaddr, user_vaddr + range);
	/* Map in the actual memory. */
	for (i = 0; i < round_up_to_page(range) / PAGE_SIZE; i++) {
		u8_t *  p     = kern_vaddr + i * PAGE_SIZE;
		paddr_t pf    = chal_va2pa(p);
		unsigned long   mapat = (unsigned long)user_vaddr + i * PAGE_SIZE;
		u32_t flags = 0;

		if (uvm && pgtbl_mapping_add(pgtbl, mapat, pf, X86_PGTBL_USER_DEF, PAGE_ORDER)) assert(0);
		if (!uvm && pgtbl_cosframe_add(pgtbl, mapat, pf, X86_PGTBL_COSFRAME, PAGE_ORDER)) assert(0);
		assert(pf == (*(unsigned long*)chal_pgtbl_lkup_lvl((pgtbl_t)(pgtbl), mapat, &flags, 0, 4) & 0xfffffffffffff000));
	}

	return 0;
}

static int
boot_map(struct captbl *ct, capid_t pgdcap, capid_t ptecap, const char *label, void *kern_vaddr,
	 unsigned long user_vaddr, unsigned int range, int user)
{
	return boot_pgtbl_mappings_add(ct, pgdcap, ptecap, label, kern_vaddr, user_vaddr, range, user);
}

static int
boot_elf_process(struct captbl *ct, capid_t pgdcap, capid_t ptecap, const char *label, void *kern_vaddr,
		 unsigned int range)
{
	struct elf_contig_mem s[3] = {};
	unsigned long  bss_sz;	   /* derived from the section information for .data */
	unsigned int   bss_pages;
	unsigned int   bss_offset; /* offset into the last page that includes .data information */
	unsigned char *bss;	   /* memory array for the last bit of .data and .bss */

	/* RO + Code */
	if (elf_contig_mem(kern_vaddr, 0, &s[0])) assert(0);
	assert(s[0].objsz == s[0].sz && s[0].access == ELF_PH_CODE);

	/* Data + BSS */
	if (elf_contig_mem(kern_vaddr, 1, &s[1])) assert(0);
	assert(s[1].access == ELF_PH_RW);
	/* the data should immediately follow code */
	assert(round_up_to_page(s[0].vstart + s[0].sz) == s[1].vstart);
	/* should be page aligned so that we can map it directly */
	assert(round_to_page(s[0].mem) == (unsigned long)s[0].mem);

	/* allocate bss memory, including the last sub-page of .data (assumes .data is page-aligned) */
	assert(s[1].vstart % PAGE_SIZE == 0);
	bss_pages  = (round_up_to_page(s[1].sz) - round_to_page(s[1].objsz))/PAGE_SIZE;
	bss_sz     = (round_up_to_page(s[1].sz) - s[1].objsz);
	bss_offset = s[1].objsz % PAGE_SIZE; //round_up_to_page(s[1].sz) - round_to_page(s[1].objsz);
	bss = mem_boot_alloc(bss_pages);
	assert(bss);

	memcpy(bss, s[1].mem + round_to_page(s[1].objsz), bss_offset);
	memset(bss + bss_offset, 0, bss_sz);

	/* Assume there are no more sections */
	if (elf_contig_mem(kern_vaddr, 2, &s[2]) != 1) assert(0);


	/* We have the elf information, time to do the virtual memory operations... */
	boot_pgtbl_expand(ct, pgdcap, ptecap, label, s[0].vstart, round_up_to_page(s[0].sz) + s[1].sz);

	/* Map in the sections separately */
	if (boot_map(ct, pgdcap, ptecap, label, s[0].mem, s[0].vstart, s[0].objsz, 1)) return -1;
	if (boot_map(ct, pgdcap, ptecap, label, s[1].mem, s[1].vstart, round_to_page(s[1].objsz), 1)) return -1;
	if (boot_map(ct, pgdcap, ptecap, label, bss, s[1].vstart + round_to_page(s[1].objsz), bss_sz, 1)) return -1;

	printk("\tBooter elf object: RO - [%x, %x), RW - [%x, %x)\n",
	       s[0].vstart, s[0].vstart + s[0].objsz, s[1].vstart, s[1].vstart + s[1].sz);

	return 0;
}

void
kern_boot_comp(const cpuid_t cpu_id)
{
	int            ret = 0, nkmemptes = 0;
	unsigned int   i;
	u8_t *         boot_comp_captbl;
	pgtbl_t        pgtbl     = (pgtbl_t)chal_va2pa(&boot_comp_pgd), boot_vm_pgd;
	u32_t          hw_bitmap = ~0;

	assert(cpu_id >= 0);
	if (NUM_CPU > 1 && cpu_id > 0) {
		assert(glb_boot_ct);
		chal_cpu_pgtbl_activate(pgtbl);
		kern_boot_thd(glb_boot_ct, thd_mem[cpu_id], tcap_mem[cpu_id], cpu_id);
		return;
	}

	printk("Setting up the booter component.\n");

	boot_comp_captbl = mem_boot_alloc(BOOT_CAPTBL_NPAGES);
	assert(boot_comp_captbl);
	glb_boot_ct = captbl_create(boot_comp_captbl);
	assert(glb_boot_ct);

	/* expand the captbl to use multiple pages. */
	for (i = PAGE_SIZE; i < BOOT_CAPTBL_NPAGES * PAGE_SIZE; i += PAGE_SIZE) {
		captbl_init(boot_comp_captbl + i, 1);
		ret = captbl_expand(glb_boot_ct, (i - PAGE_SIZE / 2) / CAPTBL_LEAFSZ, captbl_maxdepth(), boot_comp_captbl + i);
		assert(!ret);
		captbl_init(boot_comp_captbl + PAGE_SIZE + PAGE_SIZE / 2, 1);
		ret = captbl_expand(glb_boot_ct, i / CAPTBL_LEAFSZ, captbl_maxdepth(), boot_comp_captbl + i + PAGE_SIZE / 2);
		assert(!ret);
	}

	for (i = 0; i < NUM_CPU; i++) {
		thd_mem[i]  = mem_boot_alloc(1);
		assert(thd_mem[i]);
		tcap_mem[i] = mem_boot_alloc(1);
		assert(tcap_mem[i]);
		assert(thd_mem[i] && tcap_mem[i]);
	}

	if (captbl_activate_boot(glb_boot_ct, BOOT_CAPTBL_SELF_CT)) assert(0);
	if (sret_activate(glb_boot_ct, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SRET)) assert(0);

	hw_asndcap_init();
	if (hw_activate(glb_boot_ct, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_INITHW_BASE, hw_bitmap)) assert(0);

	/*
	 * separate pgd for boot component virtual memory
	 */
	boot_vm_pgd = (pgtbl_t)mem_boot_alloc(1);
	assert(boot_vm_pgd);
	memcpy((void *)boot_vm_pgd + KERNEL_PGD_REGION_OFFSET, (void *)(&boot_comp_pgd) + KERNEL_PGD_REGION_OFFSET,
	       KERNEL_PGD_REGION_SIZE);
	if (pgtbl_activate(glb_boot_ct, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_PT, (pgtbl_t)chal_va2pa(boot_vm_pgd), 0)) assert(0);

	/* Map in the virtual memory */
	ret = boot_elf_process(glb_boot_ct, BOOT_CAPTBL_SELF_PT, BOOT_CAPTBL_BOOTVM_PTE, "booter VM",
			       mem_bootc_start(), mem_bootc_end() - mem_bootc_start());
	assert(ret == 0);

	/*
	 * Map in the untyped memory.  This is more complicated as we
	 * need to use the untyped memory itself for the PTEs.
	 *
	 * Chicken and egg problem here: We want to know how many PTEs
	 * to allocate based on how much untyped memory there is.
	 * Once the PTEs are allocated, we might require fewer address
	 * range, thus less PTEs.  This _must_ be the last allocation.
	 * The bump pointer modifies this allocation.
	 *
	 * Need to account for the pages that will be allocated as
	 * PTEs
	 */
	if (pgtbl_activate(glb_boot_ct, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_UNTYPED_PT, pgtbl, 0)) assert(0);


	for (int lvl=1; lvl < 4; lvl++){
		nkmemptes += boot_nptes(BOOT_MEM_KM_BASE, mem_utmem_end() - mem_boot_end(), lvl);
	}

	//nkmemptes = boot_nptes(mem_utmem_end() - mem_boot_end());
	boot_pgtbl_expand(glb_boot_ct, BOOT_CAPTBL_SELF_UNTYPED_PT, BOOT_CAPTBL_KM_PTE, "untyped memory",
			  BOOT_MEM_KM_BASE, mem_utmem_end() - mem_boot_nalloc_end(nkmemptes));
	ret = boot_pgtbl_mappings_add(glb_boot_ct, BOOT_CAPTBL_SELF_UNTYPED_PT, BOOT_CAPTBL_KM_PTE, "untyped memory",
                                      mem_boot_nalloc_end(nkmemptes), BOOT_MEM_KM_BASE,
                                      mem_utmem_end() - mem_boot_nalloc_end(nkmemptes), 0);
	assert(ret == 0);

	printk("\tCapability table and page-table created.\n");

	/* Shut off further bump allocations */
	glb_memlayout.allocs_avail = 0;

	if (comp_activate(glb_boot_ct, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_COMP, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_PT, 0,
	                  mem_bootc_entry()))
		assert(0);
	printk("\tCreated boot component structure from page-table and capability-table.\n");

	kern_boot_thd(glb_boot_ct, thd_mem[cpu_id], tcap_mem[cpu_id], cpu_id);

	printk("\tBoot component initialization complete.\n");
}

void
kern_boot_upcall(void)
{
	void *entry = (void *)mem_bootc_entry();
	u32_t flags = 0;
	void *p;

	assert(get_cpuid() >= 0);
	/* only print complete msg for BSP */
	if (get_cpuid() == 0) {
		printk("Upcall into boot component at ip 0x%p for cpu: %d with tid: %d\n", entry, get_cpuid(), thd_current(cos_cpu_local_info())->tid);
		printk("------------------[ Kernel boot complete ]------------------\n");
	}

	chal_user_upcall(entry, thd_current(cos_cpu_local_info())->tid, get_cpuid());
	assert(0); /* should never get here! */
}
