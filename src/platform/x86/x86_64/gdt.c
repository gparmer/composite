/* Based on code from Pintos. See LICENSE.pintos for licensing information */

#include "kernel.h"
#include "tss.h"
#include "chal_asm_inc.h"

struct gdt_aligned {
        u64_t seg_descs[SEL_CNT];
} __attribute__((aligned(CACHE_LINE)));
static volatile struct gdt_aligned gdt[NUM_CPU];

/* GDT helpers. */
static u64_t make_code_desc(int dpl);
static u64_t make_data_desc(int dpl);
static void make_tss_desc(volatile u64_t * tss_desc, u64_t tss_addr);
static void make_gdtr_operand(u8_t gdtr_addr[10], u16_t limit, u64_t base);
static void flash_selectors(void);

void
chal_tls_update(vaddr_t addr)
{
	int cpu_id = get_cpuid();
	/* X86_64-FIXME: commented temporarily because currently I do not set up tls*/
	//gdt[cpu_id].seg_descs[SEL_UGSEG / sizeof *gdt[cpu_id].seg_descs] = make_data_desc_at(3, (u32_t)addr);
	/* force the reload of the segment cache */
	asm volatile("movl %0, %%gs" : : "q"(SEL_UGSEG));
}

/*
 * Sets up a proper GDT.  The bootstrap loader's GDT didn't include
 * user-mode selectors or a TSS, but we need both now.
 */
void
gdt_init(const cpuid_t cpu_id)
{
	u8_t gdtr_operand[10];

	/* Initialize GDT. */
	gdt[cpu_id].seg_descs[SEL_NULL / sizeof *gdt[cpu_id].seg_descs]  = 0;
	gdt[cpu_id].seg_descs[SEL_KCSEG / sizeof *gdt[cpu_id].seg_descs] = make_code_desc(0);
	gdt[cpu_id].seg_descs[SEL_KDSEG / sizeof *gdt[cpu_id].seg_descs] = make_data_desc(0);

	gdt[cpu_id].seg_descs[SEL_UDSEG / sizeof *gdt[cpu_id].seg_descs] = make_data_desc(3);
	gdt[cpu_id].seg_descs[SEL_UCSEG / sizeof *gdt[cpu_id].seg_descs] = make_code_desc(3);
	make_tss_desc(&(gdt[cpu_id].seg_descs[SEL_TSS / sizeof *gdt[cpu_id].seg_descs]), (u64_t)&tss[cpu_id]);
	gdt[cpu_id].seg_descs[SEL_UGSEG / sizeof *gdt[cpu_id].seg_descs] = make_data_desc(3);

	/*
	 * Load GDTR, TR.  See [IA32-v3a] 2.4.1 "Global Descriptor
	 * Table Register (GDTR)", 2.4.4 "Task Register (TR)", and
	 * 6.2.4 "Task Register".
	 */
	make_gdtr_operand(gdtr_operand, sizeof gdt[cpu_id] - 1 ,(u64_t)(gdt + cpu_id));
	asm volatile("lgdt %0" : : "m"(gdtr_operand));
	/* 
	 * Be careful to have the correct tss structure in memory and in gdt,
	 * qemu does not check tss validity, but real machine does check 
	 */
	asm volatile("ltr %w0" : : "r"(SEL_TSS));
	flash_selectors();
}

/* System segment or code/data segment? */
enum seg_class
{
	CLS_SYSTEM    = 0, /* System segment. */
	CLS_CODE_DATA = 1  /* Code or data segment. */
};

/* Limit has byte or 4 kB page granularity? */
enum seg_granularity
{
	GRAN_BYTE = 0, /* Limit has 1-byte granularity. */
	GRAN_PAGE = 1  /* Limit has 4 kB granularity. */
};


static void
make_gdtr_operand(u8_t gdtr_addr[10], u16_t limit, u64_t base)
{
	*((u16_t*)(&gdtr_addr[0])) = limit;
	*((u64_t*)(&gdtr_addr[2])) = base;
}

static u64_t
make_code_desc(int dpl)
{
	/* 
	 * make sure to init variables before use, you don't know 
	 * whether the value comes from registers or memory, if it does 
	 * comes from memory, value in that address could be random, 
	 * thus you need to init that memory first. 	
	 */
	u32_t e0 = 0, e1 = 0;

	e1 = ((0)
	      | (10 << 8)       /* Segment type. */
	      | (1 << 12)       /* 0=system, 1=code/data. */
	      | (dpl << 13)     /* Descriptor privilege. */
	      | (1 << 15)       /* Present. */
		  | (1 << 21)       /* L bit. */
	      | (0 << 22));       /* D/B bit. */

	return e0 | ((u64_t)e1 << 32);
}


static u64_t
make_data_desc(int dpl)
{
	/* 
	 * make sure to init variables before use, you don't know 
	 * whether the value comes from registers or memory, if it does 
	 * comes from memory, value in that address could be random, 
	 * thus you need to init that memory first. 	
	 */
	u32_t e0 = 0, e1 = 0;

	e1 = ((0)
	      | (2 << 8)        /* Segment type. */
	      | (1 << 12)       /* 0=system, 1=code/data. */
	      | (dpl << 13)     /* Descriptor privilege. */
	      | (1 << 15));     /* Present. */

	return e0 | ((u64_t)e1 << 32);
}

static void
make_tss_desc(volatile u64_t * tss_desc, u64_t tss_addr)
{
	/* 
	 * make sure to init variables before use, you don't know 
	 * whether the value comes from registers or memory, if it does 
	 * comes from memory, value in that address could be random, 
	 * thus you need to init that memory first. 	
	 */
	u32_t e0 = 0, e1 = 0;
	u64_t e2 = (tss_addr >> 32) & 0x00000000ffffffff;
	u32_t base = (u32_t)tss_addr;
	int type = 9;
	u32_t limit = 0x67;
	e0 = (( limit & 0xffff) 		/* Limit 15:0. */
	      | (base << 16));			/* Base 15:0. */

	e1 = (((base >> 16) & 0xff)		/* Base 23:16. */
	      | (type << 8)				/* Segment type. */
	      | (0 << 12)				/* 0=system, 1=code/data. */
	      | (0 << 13)				/* Descriptor privilege. */
	      | (1 << 15)             	/* Present. */
	      | (limit & 0xf0000)     	/* Limit 16:19. */
	      | (base & 0xff000000)); 	/* Base 31:24. */

	*tss_desc = (e0 | ((u64_t)e1 << 32));	
	*(tss_desc + 1) = e2;
}

static void
flash_selectors(void)
{
	__asm__ __volatile__("mov %0, %%rax   \n\t"   
						 "mov %%rax, %%ds  \n\t"
				 		 "mov %%rax, %%ss     \n\t"
						 "mov $0, %%rax    \n\t"
						 "mov %%rax, %%es	  \n\t"
						 "mov %%rax, %%fs     \n\t"   
						 "mov %%rax, %%gs     \n\t"
						 "mov %1, %%rax	  \n\t"
						 "pushq %%rax	  \n\t"
						 "movabs $label_1, %%rax \n\t"
						 "pushq %%rax     \n\t"
						 "lretq    \n\t"
						 ".global label_1\n\t"
						 "label_1:\n\t"
						:                  
						:"i"(SEL_KDSEG), "i"(SEL_KCSEG)                         
						:"%rax");                   

}