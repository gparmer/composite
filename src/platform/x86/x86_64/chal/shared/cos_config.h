#ifndef COS_CONFIG_H
#define COS_CONFIG_H

/***
 * Memory information.  Included are four sections:
 * - physical memory information (PA)
 * - virtual memory addresses (VA)
 * - resource table addresses to index memory resources (RTA)
 * - the maximum number of pages of the different types of memory.
 *   See BOOT_* in cos_types.h for this information
 *
 * These exist separately for kernel-accessible memory that can be
 * typed as either kernel or user virtual memory, and
 * kernel-inaccessible memory that can only be used as user virtual
 * memory.
 */

#include "cpu_ghz.h"

#define NUM_CPU 1
#define NUM_CPU_BMP_BYTES ((NUM_CPU + 7) / 8)
#define NUM_CPU_BMP_WORDS ((NUM_CPU_BMP_BYTES + 3) / 4)
/* 
 * FIXME: The macro to set a portion of memory of the booter to super pages - 
 * should be dynamically passed from kernel to userlevel!
 */
#define NUM_SUPERPAGES           139 
#define MAX_USABLE_MEMORY        1700
/* FIXME: This is a hack - was 0xD800000, now expanded to 1200MB */
#define EXTRA_MEMORY             ((MAX_USABLE_MEMORY - 512) << 20)
#define EXTRA_SUPERPAGES         ((MAX_USABLE_MEMORY - 808) / 4)
#define TOTAL_SUPERPAGES         (NUM_SUPERPAGES + EXTRA_SUPERPAGES - 1)

/*
 * 1 MB, note that this is not the PA of kernel-usable memory, instead
 * it is the PA of the kernel.  If you change this, update the kernel
 * linker script (.ld) as well.
 */
#define COS_MEM_KERN_PA (0x00100000)
#define COS_MEM_KERN_PA_ORDER (29)
#define COS_MEM_KERN_PA_SZ ((1 << COS_MEM_KERN_PA_ORDER) - (1<<26)) /* FIXME: Need a way to get physical memory size from kernel. Cannot use a hardcoded value, actual memory could be much lower! */

#define COS_MEM_COMP_START_VA ((1 << 30) + (1 << 22)) /* 1GB + 4MB (a relic) */
#define COS_MEM_KERN_HIGH_ADDR_VA_PGD_MASK 0x0000ff8000000000
#define COS_MEM_KERN_START_VA (0xffff800000000000) // COS_MEM_KERN_PA     /* currently, we don't do kernel relocation */

#define COS_HW_MMIO_MAX_SZ (1 << 27) /* Assuming a MAX of 128MB for MMIO. */
#define COS_PHYMEM_MAX_SZ ((1 << 30) - (1 << 22) - COS_HW_MMIO_MAX_SZ) /* 1GB - 4MB - MMIO sz */

#define COS_PHYMEM_END_PA ((1 << 30) - COS_HW_MMIO_MAX_SZ) /* Maximum usable physical memory */

#define BOOT_COMP_MAX_SZ (1 << 24) /* 16 MB for the booter component */

#define CPU_TIMER_FREQ 100 // set in your linux .config

#define RUNTIME 3 // seconds

/* The kernel quiescence period = WCET in Kernel + WCET of a CAS. */
#define KERN_QUIESCENCE_PERIOD_US 500
#define KERN_QUIESCENCE_CYCLES (KERN_QUIESCENCE_PERIOD_US * 4000)
#define TLB_QUIESCENCE_CYCLES (4000 * 1000 * (1000 / CPU_TIMER_FREQ))

// After how many seconds should schedulers print out their information?
#define SCHED_PRINTOUT_PERIOD 100000
#define COMPONENT_ASSERTIONS 1 // activate assertions in components?

//#define FPU_ENABLED
#define FPU_SUPPORT_FXSR 1 /* >0 : CPU supports FXSR. */

/* the CPU that does initialization for Composite */
#define INIT_CORE 0
#define NUM_CPU_COS (NUM_CPU > 1 ? NUM_CPU - 1 : 1)

/* Composite user memory uses physical memory above this. */
#define COS_MEM_START COS_MEM_KERN_PA

/* NUM_CPU_SOCKETS defined in cpu_ghz.h. The information is used for
 * intelligent IPI distribution. */
#define NUM_CORE_PER_SOCKET (NUM_CPU / NUM_CPU_SOCKETS)

// cos kernel settings
#define COS_PRINT_MEASUREMENTS 1
#define COS_PRINT_SCHED_EVENTS 1
#define COS_ASSERTIONS_ACTIVE 1

/*** Console and output options ***/
/*
 * Notes: If you are using composite as high priority and no idle to
 * linux, then the shell output will not appear until the Composite
 * system has exited.  Thus, you will want to make the memory size
 * large enough to buffer _all_ output.  Note that currently
 * COS_PRINT_MEM_SZ should not exceed around (1024*1024*3).
 *
 * If you have COS_PRINT_SHELL, you _will not see output_ unless you
 * run
 * $~/transfer/print
 * after
 * # make
 * but before the runscript.
 */
/* print out to the shell? */
#define COS_PRINT_SHELL 1
/* how much should we buffer before sending an event to the shell? */
#define COS_PRINT_BUF_SZ 128
/* how large should the shared memory region be that will buffer print data? */
#define COS_PRINT_MEM_SZ (4096)

/* print out to dmesg? */
/* #define COS_PRINT_DMESG 1 */

/**
 * Configuration to enable/disable functionality in Kernel.
 */
#define ENABLE_VGA
#define ENABLE_SERIAL

#define COS_PGTBL_ORDER_PTE 12
#define COS_PGTBL_ORDER_PGD 22

/* Page sizes */
#define COS_PGTBL_NUM_ORDER      2
#define COS_PGTBL_ORDERS         COS_PGTBL_ORDER_PTE, COS_PGTBL_ORDER_PGD
#define COS_PGTBL_ORDER2POS /* 0/1B    1/2B    2/4B    3/8B   4/16B   5/32B   6/64B  7/128B  8/256B  9/512B */ \
				-1,     -1,     -1,     -1,     -1,     -1,     -1,     -1,     -1,     -1, \
		  	    /* 10/1K   11/2K   12/4K   13/8K  14/16K  15/32K  16/64K 17/128K 18/256K 19/512K */ \
		  		-1,     -1,      0,     -1,     -1,     -1,     -1,     -1,     -1,     -1, \
		  	    /* 20/1M   21/2M   22/4M   23/8M  24/16M  25/32M  26/64M 27/128M 28/256M 29/512M */ \
		  		-1,     -1,      1,     -1,     -1,     -1,     -1,     -1,     -1,     -1, \
  		  	    /* 30/1G   31/2G */ \
		  		-1,     -1 \

#endif /* COS_CONFIG_H */
