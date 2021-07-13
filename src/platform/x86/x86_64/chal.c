#include <chal.h>
#include <shared/cos_types.h>
#include "kernel.h"
#include "mem_layout.h"
#include "chal_cpu.h"

u32_t        free_thd_id;
char         timer_detector[PAGE_SIZE] PAGE_ALIGNED;
extern void *cos_kmem, *cos_kmem_base;
u32_t        chal_msr_mhz = 0;

paddr_t chal_kernel_mem_pa;

void *
chal_alloc_kern_mem(int order)
{
	return mem_kmem_start();
}

void
chal_free_kern_mem(void *mem, int order)
{
}

int
chal_attempt_arcv(struct cap_arcv *arcv)
{
	return 0;
}

void
chal_send_ipi(int cpu_id)
{
	lapic_asnd_ipi_send(cpu_id);

	return;
}

void
chal_khalt(void)
{
	khalt();
}

int
chal_tlb_lockdown(unsigned long entryid, unsigned long vaddr, unsigned long paddr)
{
	return 0;
}

int
chal_l1flush(void)
{
	/* TODO */
	return 0;
}

int
chal_tlbflush(int a)
{
	/* TODO */
	return 0;
}

int
chal_tlbstall(void)
{
	/* TODO */
	return 0;
}

int
chal_tlbstall_recount(int a)
{
	/* TODO */
	return 0;
}

void
chal_init(void)
{
	u32_t a, b, c, d;
	u32_t vendor[4];
	char *v = (char *)&vendor[0];
	int   apicid, i;

	printk("Processor information:\n");
	chal_cpuid(0, &a, &b, &c, &d);
	vendor[0] = b;
	vendor[1] = d;
	vendor[2] = c;
	vendor[3] = 0;
	printk("\tVendor: %s\n", (char *)vendor);

	chal_cpuid(0x80000000, &a, &b, &c, &d);
	/* processor brand string is supported? */
	if (a > 0x80000004) {
		u32_t name[13];

		chal_cpuid(0x80000002, &name[0], &name[1], &name[2], &name[3]);
		chal_cpuid(0x80000003, &name[4], &name[5], &name[6], &name[7]);
		chal_cpuid(0x80000004, &name[8], &name[9], &name[10], &name[11]);
		name[12] = 0;

		printk("\tBrand string %s\n", (char *)name);
	}

	printk("\tFeatures [");
	chal_cpuid(1, &a, &b, &c, &d);
	if (d & (1 << 4)) printk("rdtsc ");
	if (d & (1 << 0)) printk("fpu ");
	if (d & (1 << 3)) printk("superpages ");
	if (d & (1 << 11)) printk("sysenter/exit ");
	if (d & (1 << 19)) printk("clflush ");
	if (d & (1 << 22)) printk("acpi ");
	if (d & (1 << 9)) printk("apic ");
	if (c & (1 << 21)) printk("x2apic ");
	if (c & (1 << 24)) printk("tsc-deadline ");
	chal_cpuid(0x80000001, &a, &b, &c, &d);
	if (d & (1 << 27)) printk("rdtscp ");
	chal_cpuid(0x80000007, &a, &b, &c, &d);
	if (d & (1 << 8)) printk("invariant_tsc ");
	printk("]\n");

	chal_cpuid(0x16, &a, &b, &c, &d);
	/* FIXME: need to do cpuid twice to get frequency, don't know why  */
	chal_cpuid(0x16, &a, &b, &c, &d);
	a = (a << 16) >> 16;
	if (a) {
		printk("\tCPUID base frequency: %d (* 1Mhz)\n", a);
		printk("\tCPUID max  frequency: %d (* 1Mhz)\n", (b << 16) >> 16);
	}

	/* FIXME: cannot get platform info on qemu */
	readmsr(MSR_PLATFORM_INFO, &a, &b);
	a = (a >> 8) & ((1<<7)-1);
	if (a) {
		printk("\tMSR Frequency: %d (* 100Mhz)\n", a);
		chal_msr_mhz = a * 100;
	}

	free_thd_id = 1;

	chal_kernel_mem_pa = chal_va2pa(mem_kmem_start());
}