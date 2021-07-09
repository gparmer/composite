/**
 * Copyright 2007 by Gabriel Parmer, gabep1@cs.bu.edu
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 */
#include <stdio.h>

#include <sys/auxv.h>

#include <consts.h>
#include <cos_component.h>
#include <cos_debug.h>
#include <cos_kernel_api.h>

#include <init.h>

#include <ps.h>

CWEAKSYMB int cos_sched_notifications;

/*
 * __crt_main is just used to identify if the user has defined their
 * own main (thus overriding the weak place-holder below)
 */
static int
__crt_main(void)
{
	return 0;
}
COS_FN_WEAKALIAS(main, __crt_main);

CWEAKSYMB void
cos_init()
{
	return;
}

static void
__crt_parallel_main(coreid_t cid, int init_core, int ncores)
{
	return;
}
COS_FN_WEAKALIAS(parallel_main, __crt_parallel_main);

void
__crt_cos_parallel_init(coreid_t cid, int init_core, int ncores)
{
	return;
}
COS_FN_WEAKALIAS(cos_parallel_init, __crt_cos_parallel_init);

int
cos_main_defined(void)
{
	return __crt_main != main;
}

/*
 * Invoke the user's main. Only makes sense to call this if there is
 * an actual main function defined. TODO: handle passing arguments
 * properly.
 */
int
cos_main(void)
{
	assert(cos_main_defined());

	return main();
}

/* Intended to be implement by libraries */
CWEAKSYMB void
pre_syscall_default_setup()
{
}

/* Intended to be overriden by components */
CWEAKSYMB void
pre_syscall_setup()
{
	pre_syscall_default_setup();
}

CWEAKSYMB void
syscall_emulation_setup()
{
}

CWEAKSYMB long
cos_syscall_handler(int syscall_num, long a, long b, long c, long d, long e, long f, long g)
{
	printc("Default syscall handler called (syscall: %d, first arg: %ld), faulting!", syscall_num, a);
	assert(0);
	return 0;
}

__attribute__((regparm(1))) long
__cos_syscall(int syscall_num, long a, long b, long c, long d, long e, long f, long g)
{
	return cos_syscall_handler(syscall_num, a, b, c, d, e, f, g);
}

CWEAKSYMB void
libc_initialization_handler()
{
}

/* TODO: Make this a weak symbol (currently doing so makes this fail) */
void __init_libc(char **envp, char *pn);

void
libc_init()
{
	/* The construction of this is:
	 * evn1, env2, ..., NULL, auxv_n1, auxv_1, auxv_n2, auxv_2 ..., NULL
	 * TODO: Figure out a way to set AT_HWCAP / AT_SYSINFO
	 */
	static char *envp[] = {
                               "USER=composite_user",
                               "LANG=en_US.UTF-8",
                               "HOME=/home/composite_user",
                               "LOGNAME=composite_user",
                               NULL,
                               (char *)AT_PAGESZ,
                               (char *)PAGE_SIZE, /* Page size */
                               (char *)AT_UID,
                               (char *)1000, /* User id */
                               (char *)AT_EUID,
                               (char *)1000, /* Effective user id */
                               (char *)AT_GID,
                               (char *)1000, /* Group id */
                               (char *)AT_EGID,
                               (char *)1000, /* Effective group id */
                               (char *)AT_SECURE,
                               (char *)0, /* Whether the program is being run under sudo */
                               NULL
	};
	char *program_name = "composite component";
	__init_libc(envp, program_name);
}

CWEAKSYMB void
cos_upcall_exec(void *arg)
{
}

CWEAKSYMB int
cos_async_inv(struct usr_inv_cap *ucap, int *params)
{
	return 0;
}

CWEAKSYMB int
cos_thd_entry_static(u32_t idx)
{
	assert(0);
	return 0;
}

const char *cos_print_str[PRINT_LEVEL_MAX] = {
	"ERR:",
	"WARN:",
	"DBG:",
};

cos_print_level_t cos_print_level   = PRINT_ERROR;
int               cos_print_lvl_str = 0;

CWEAKSYMB void
cos_print_level_set(cos_print_level_t lvl, int print_str)
{
	cos_print_level   = lvl;
	cos_print_lvl_str = print_str;
}

/*
 * Cos thread creation data structures.
 */
struct __thd_init_data __thd_init_data[COS_THD_INIT_REGION_SIZE] CACHE_ALIGNED;

static void
cos_thd_entry_exec(u32_t idx)
{
	void (*fn)(void *);
	void *data;

	fn   = __thd_init_data[idx].fn;
	data = __thd_init_data[idx].data;
	/* and release the entry... might need a barrier here. */
	__thd_init_data[idx].data = NULL;
	__thd_init_data[idx].fn   = NULL;

	(fn)(data);
}

static void
start_execution(coreid_t cid, int init_core, int ncores)
{
	init_main_t main_type = INIT_MAIN_NONE;
	/* is there a user-defined parallel init? */
	const int parallel_init = cos_parallel_init != __crt_cos_parallel_init;
	int ret = 0;
	int main_time = 0;
	static volatile int initialization_completed = 0;

	/* are parallel/regular main user-defined? */
	if (parallel_main != __crt_parallel_main) {
		main_type = INIT_MAIN_PARALLEL;
	} else if (cos_main_defined()) {
		/* only execute main if parallel_main doesn't exist */
		main_type = INIT_MAIN_SINGLE;
	}

	/* single-core initialization */
	if (initialization_completed == 0) {
		if (init_core) {
			cos_init();
			/* continue only if there is no user-defined main, or parallel exec */
			COS_EXTERN_INV(init_done)(parallel_init, main_type);
			assert(parallel_init || main_type != INIT_MAIN_NONE);
		}

		/* Parallel initialization */
		COS_EXTERN_INV(init_parallel_await_init)();
		if (parallel_init) {
			cos_parallel_init(cid, init_core, init_parallelism());
		}
		/* All initialization completed here, go onto main execution */
		initialization_completed = 1;
		COS_EXTERN_INV(init_done)(0, main_type);
	}
	/* No main? we shouldn't have continued here... */
	assert(main_type != INIT_MAIN_NONE);
	assert(main_type == INIT_MAIN_PARALLEL || (main_type == INIT_MAIN_SINGLE && init_core));

	/* Execute the main: either parallel or normal */
	if (main_type == INIT_MAIN_PARALLEL) {
		parallel_main(cid, init_core, init_parallelism());
	} else {
		/* Only the initial core should execute main */
		assert(init_core && main_type == INIT_MAIN_SINGLE);
		ret = cos_main();
	}
	COS_EXTERN_INV(init_exit)(ret);

	/* with the previous exits, we should never get here */
	BUG();
}

CWEAKSYMB void
cos_upcall_fn(upcall_type_t t, void *arg1, void *arg2, void *arg3)
{
	static int first = 1;

	/*
	 * There should be no concurrency at initialization (the init
	 * interface ensures this), so atomic operations aren't
	 * required here to update first.
	 */
	if (first) {
		first = 0;

		/*
		 * Use the heap pointer as calculated from the linker
		 * script *if* the loader doesn't pass in its own
		 * value.
		 */
		if (__cosrt_comp_info.cos_heap_ptr == 0) {
			extern const vaddr_t __crt_static_heap_ptr;

			__cosrt_comp_info.cos_heap_ptr = round_up_to_page((vaddr_t)&__crt_static_heap_ptr);
		}

		cos_print_level_set(PRINT_DEBUG, 1);
		/* The syscall enumlator might need something to be setup before it can work */
		pre_syscall_setup();
		/* libc needs syscall emulation to work */
		syscall_emulation_setup();
		/* With all that setup, we can invoke the libc_initialization_handler */
		libc_initialization_handler();

		constructors_execute();
	}

	/*
	 * if it's the first component.. wait for timer calibration.
	 * NOTE: for "fork"ing components and not updating "spdid"s, this call will just fail and should be fine.
	 */
	if (cos_compid_uninitialized()) { /* we must be in the initial booter! */
		cos_hw_cycles_per_usec(BOOT_CAPTBL_SELF_INITHW_BASE);
	}

	switch (t) {
        /* New thread creation method passes in this option. */
	case COS_UPCALL_THD_CREATE:
	{
		/* A new thread is created in this comp. */

		/* arg1 is the thread init data. 0 means
		 * bootstrap. */
		if (arg1 == 0) {
			static unsigned long first_core = 1;

			start_execution(cos_coreid(), ps_cas(&first_core, 1, 0), init_parallelism());

		} else {
			u32_t idx = (int)arg1 - 1;
			if (idx >= COS_THD_INIT_REGION_SIZE) {
				/* This means static defined entry */
				cos_thd_entry_static(idx - COS_THD_INIT_REGION_SIZE);
			} else {
				/* Execute dynamic allocated entry. */
				cos_thd_entry_exec(idx);
			}
		}
		break;
	}
	default:
		/* fault! */
		assert(0);
	}
	assert(0); 		/* should *not* return from threads */

	return;
}

CWEAKSYMB void *
cos_get_vas_page(void)
{
	char *h;
	long  r;
	do {
		h = cos_get_heap_ptr();
		r = (long)h + PAGE_SIZE;
	} while (cos_cmpxchg(&__cosrt_comp_info.cos_heap_ptr, (long)h, r) != r);
	return h;
}

CWEAKSYMB void
cos_release_vas_page(void *p)
{
	cos_set_heap_ptr_conditional(p + PAGE_SIZE, p);
}

extern const vaddr_t cos_atomic_cmpxchg, cos_atomic_cmpxchg_end, cos_atomic_user1, cos_atomic_user1_end,
  cos_atomic_user2, cos_atomic_user2_end, cos_atomic_user3, cos_atomic_user3_end, cos_atomic_user4,
  cos_atomic_user4_end;
extern const vaddr_t __cosrt_upcall_entry;

extern const vaddr_t cos_ainv_entry;

CWEAKSYMB vaddr_t ST_user_caps;

/*
 * Much of this is either initialized at load time, or passed to the
 * loader though this structure.
 */
struct cos_component_information __cosrt_comp_info =
        { .cos_this_spd_id         = 0,
	  .cos_heap_ptr            = 0,
	  .cos_heap_limit          = 0,
	  .cos_stacks.freelists[0] = {.freelist = 0, .thd_id = 0},
	  .cos_upcall_entry        = (vaddr_t)&__cosrt_upcall_entry,
	  .cos_async_inv_entry     = (vaddr_t)&cos_ainv_entry,
	  .cos_user_caps           = (vaddr_t)&ST_user_caps,
	  .cos_ras  = {{.start = (vaddr_t)&cos_atomic_cmpxchg, .end = (vaddr_t)&cos_atomic_cmpxchg_end},
		       {.start = (vaddr_t)&cos_atomic_user1, .end = (vaddr_t)&cos_atomic_user1_end},
		       {.start = (vaddr_t)&cos_atomic_user2, .end = (vaddr_t)&cos_atomic_user2_end},
		       {.start = (vaddr_t)&cos_atomic_user3, .end = (vaddr_t)&cos_atomic_user3_end},
		       {.start = (vaddr_t)&cos_atomic_user4, .end = (vaddr_t)&cos_atomic_user4_end}},
	  .cos_poly = {0,}
	};

CWEAKSYMB long _binary_tar_binary_start = 0;
CWEAKSYMB long _binary_tar_binary_end = 0;

char *
cos_initargs_tar(void)
{
	/* Tar files are at least one record, which is 512 bytes */
	if (_binary_tar_binary_end - _binary_tar_binary_start < 512) return NULL;
	return (char *)_binary_tar_binary_start;
}
