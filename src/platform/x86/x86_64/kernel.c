#include "assert.h"
#include "kernel.h"
#include "multiboot2.h"
#include "string.h"
#include "boot_comp.h"
#include "mem_layout.h"
#include "chal_cpu.h"

#include <captbl.h>
#include <retype_tbl.h>
#include <component.h>
#include <thd.h>
#include <chal_plat.h>

#define ADDR_STR_LEN 8

boot_state_t initialization_state = INIT_BOOTED;

void
boot_state_transition(boot_state_t from, boot_state_t to)
{
	assert(initialization_state == from);
	assert((to - from) == 1); /* transitions must be linear */
	initialization_state = to;
}

struct mem_layout glb_memlayout;
volatile int cores_ready[NUM_CPU];

extern u8_t end; /* from the linker script */

#define MEM_KB_ONLY(x) (((x) & ((1 << 20) - 1)) >> 10)
#define MEM_MB_ONLY(x) ((x) >> 20)

/*
 * X86_64-FIXME:
 * Change these two external variable into non-external
 */
extern u8_t _binary_constructor_start, _binary_constructor_end;

void
kern_memory_setup(u64_t mboot_addr, u64_t mboot_magic)
{
	unsigned int i = 0, wastage = 0;
	struct multiboot_tag *tag;

	glb_memlayout.allocs_avail = 1;

	if (mboot_magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
		die("multiboot magic not correct\n");
	}

	if (mboot_addr & 7)
    {
	  die("mboot unligned mbi\n");
    }

	glb_memlayout.kern_end = &end;
	assert((u64_t)&end % RETYPE_MEM_NPAGES * PAGE_SIZE == 0);

	printk("Initial component found:\n");
	/* These values have to be higher-half addresses */
	glb_memlayout.mod_start = &_binary_constructor_start;
	glb_memlayout.mod_end   = &_binary_constructor_end;

	/* FIXME: set mod start and end temporarily in order to test */
	//glb_memlayout.mod_start = &end + 1024;
	//glb_memlayout.mod_end   = glb_memlayout.mod_start + 1024;
	glb_memlayout.kern_boot_heap = mem_boot_start();
	printk("kenrel mem layout:\n");
	printk("\t- [%p, %p, %p)\n", glb_memlayout.mod_start, glb_memlayout.mod_end, glb_memlayout.kern_boot_heap);

	u32_t size = *(u32_t *)mboot_addr;
	if (size <= 0) {
		die("not found tag!\n");
	}

	size = 0;

	printk("Memory regions:\n\n");
	for (tag = (struct multiboot_tag *) (mboot_addr + 8);
       tag->type != MULTIBOOT_TAG_TYPE_END;
       tag = (struct multiboot_tag *) ((multiboot_uint8_t *) tag 
                                       + ((tag->size + 7) & ~7)))
    {
      switch (tag->type)
        {
        case MULTIBOOT_TAG_TYPE_MMAP:
          {
            multiboot_memory_map_t *mmap;
            for (mmap = ((struct multiboot_tag_mmap *) tag)->entries;
                 (multiboot_uint8_t *) mmap < (multiboot_uint8_t *) tag + tag->size;
                 mmap = (multiboot_memory_map_t *) ((unsigned long) mmap + ((struct multiboot_tag_mmap *) tag)->entry_size)){
					u8_t * mod_end  = glb_memlayout.mod_end;
					u8_t * mem_addr = chal_pa2va((paddr_t)mmap->addr);
					u64_t mem_len  = (mmap->len > COS_PHYMEM_MAX_SZ ? COS_PHYMEM_MAX_SZ : mmap->len); /* maximum allowed */
					printk("\t- %d (%s): [%08llx, %08llx) sz = %ldMB + %ldKB\n", i, mmap->type == 1 ? "Available" : "Reserved ", mmap->addr,
		       			mmap->addr + mmap->len, MEM_MB_ONLY((u64_t)mmap->len), MEM_KB_ONLY((u64_t)mmap->len));

					if (mmap->addr > COS_PHYMEM_END_PA || mmap->addr + mem_len > COS_PHYMEM_END_PA) continue;
					/* is this the memory region we'll use for component memory? */
					if (mmap->type == 1 && mod_end >= mem_addr && mod_end < (mem_addr + mem_len)) {
						u64_t sz = (mem_addr + mem_len) - mod_end;

						glb_memlayout.kmem_end = mem_addr + mem_len;
						printk("\t  memory usable at boot time: %lx (%ld MB + %ld KB)\n", sz, MEM_MB_ONLY(sz),
							MEM_KB_ONLY(sz));
					}
					i++;
				 }
          	break;
		  }

        }
    }

	/* FIXME: check memory layout vs. the multiboot memory regions... */

	/* Validate the memory layout. */
	assert(mem_kern_end() <= mem_bootc_start());
	assert(mem_bootc_end() <= mem_boot_start());
	assert(mem_boot_start() >= mem_kmem_start());
	assert(mem_kmem_start() == mem_bootc_start());
	assert(mem_kmem_end() >= mem_boot_end());
	assert(mem_utmem_start() >= mem_kmem_start());
	assert(mem_utmem_start() >= mem_boot_end());
	assert(mem_utmem_end() <= mem_kmem_end());

	wastage += mem_boot_start() - mem_bootc_end();

	printk("\tAmount of wasted memory due to layout is %u MB + 0x%x B\n", MEM_MB_ONLY(wastage), wastage & ((1 << 20) - 1));
	assert(STK_INFO_SZ == sizeof(struct cos_cpu_local_info));
}

void
kmain(u64_t mboot_addr, u64_t mboot_magic)
{

#define MAX(X, Y) ((X) > (Y) ? (X) : (Y))

	u64_t max;
	tss_init(INIT_CORE);
	idt_init(INIT_CORE);
	gdt_init(INIT_CORE);
	
	#ifdef ENABLE_SERIAL
	serial_init();
	#endif

	#ifdef ENABLE_CONSOLE
	console_init();
	#endif

	#ifdef ENABLE_VGA
	vga_init();
	#endif

	boot_state_transition(INIT_BOOTED, INIT_CPU);
	max =MAX((u64_t)chal_va2pa((void*)mboot_addr), (u64_t)(chal_va2pa(&end)));

	kern_paging_map_init((void *)(max));
	kern_memory_setup(mboot_addr, mboot_magic);
	boot_state_transition(INIT_CPU, INIT_MEM_MAP);

	chal_init();
	cap_init();
	ltbl_init();
	retype_tbl_init();
	comp_init();
	thd_init();
	boot_state_transition(INIT_MEM_MAP, INIT_DATA_STRUCT);

	paging_init();
	boot_state_transition(INIT_DATA_STRUCT, INIT_UT_MEM);

	acpi_init();
	lapic_init();
	timer_init();
	boot_state_transition(INIT_UT_MEM, INIT_KMEM);

	kern_boot_comp(INIT_CORE);

	smp_init(cores_ready);
	cores_ready[INIT_CORE] = 1;

	kern_boot_upcall();

	/* should not get here... */
	khalt();
}

void
smp_kmain(void)
{
	volatile cpuid_t cpu_id = get_cpuid();
	struct cos_cpu_local_info *cos_info = cos_cpu_local_info();

	printk("Initializing CPU %d\n", cpu_id);
	tss_init(cpu_id);
	gdt_init(cpu_id);
	idt_init(cpu_id);

	chal_cpu_init();
	kern_boot_comp(cpu_id);
	lapic_init();

	printk("New CPU %d Booted\n", cpu_id);
	cores_ready[cpu_id] = 1;
	/* waiting for all cored booted */
	while(cores_ready[INIT_CORE] == 0);

	kern_boot_upcall();

	while(1) ;
}

extern void shutdown_apm(void);
extern void outw(unsigned short __val, unsigned short __port);

void
khalt(void)
{
	static int method = 0;

	if (method == 0) printk("Shutting down...\n");
	/*
	 * Use the case statement as we shutdown in the fault handler,
	 * thus faults on shutdown require that we bypass faulty
	 * shutdown handlers.
	 */
	switch(method) {
	case 0:
		method++;
		printk("\ttry acpi");
		acpi_shutdown();
		printk("...FAILED\n");
	case 1:
		method++;
		printk("\ttry apm");
		shutdown_apm();
		printk("...FAILED\n");
	case 2:
		method++;
		printk("\t...try emulator magic");
		outw(0xB004, 0x0 | 0x2000);
		printk("...FAILED\n");
	}
	/* last resort */
	printk("\t...spinning\n");
	while (1) ;
}