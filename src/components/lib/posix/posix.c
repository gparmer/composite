#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <syscall.h>
#include <time.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <cos_component.h>
#include <cos_defkernel_api.h>
#include <llprint.h>
#include <memmgr.h>
/*
#include <sl.h>
#include <sl_lock.h>
#include <sl_thd.h>
*/

volatile int* null_ptr = NULL;
#define ABORT() do {int i = *null_ptr;} while(0)

#define SYSCALLS_NUM 378

typedef long (*cos_syscall_t)(long a, long b, long c, long d, long e, long f);
cos_syscall_t cos_syscalls[SYSCALLS_NUM];

static void
libc_syscall_override(cos_syscall_t fn, int syscall_num)
{
	printc("Overriding syscall %d\n", syscall_num);
	cos_syscalls[syscall_num] = fn;
}

//struct sl_lock stdout_lock = SL_LOCK_STATIC_INIT();

ssize_t
write_bytes_to_stdout(const char *buf, size_t count)
{
	size_t i;
	for(i = 0; i < count; i++) printc("%c", buf[i]);
	return count;
}

ssize_t
cos_write(int fd, const void *buf, size_t count)
{
	/* You shouldn't write to stdin anyway, so don't bother special casing it */
	if (fd == 1 || fd == 2) {
		//sl_lock_take(&stdout_lock);
		write_bytes_to_stdout((const char *) buf, count);
		//sl_lock_release(&stdout_lock);
		return count;
	} else {
		printc("fd: %d not supported!\n", fd);
		assert(0);
	}
}

ssize_t
cos_writev(int fd, const struct iovec *iov, int iovcnt)
{
	if (fd == 1 || fd == 2) {
		//sl_lock_take(&stdout_lock);
		int i;
		ssize_t ret = 0;
		for(i=0; i<iovcnt; i++) {
			ret += write_bytes_to_stdout((const void *)iov[i].iov_base, iov[i].iov_len);
		}
		//sl_lock_release(&stdout_lock);
		return ret;
	} else {
		printc("fd: %d not supported!\n", fd);
		assert(0);
	}
}

long
cos_ioctl(int fd, int request, void *data)
{
	/* musl libc does some ioctls to stdout, so just allow these to silently go through */
	if (fd == 1 || fd == 2) return 0;

	printc("ioctl on fd(%d) not implemented\n", fd);
	assert(0);
	return 0;
}

ssize_t
cos_brk(void *addr)
{
	printc("brk not implemented\n");
	/* Unsure if the below comment is accurate. We return 0, so doesn't brk "succeed"? */
	/* musl libc tries to use brk to expand heap in malloc. But if brk fails, it
	   turns to mmap. So this fake brk always fails, force musl libc to use mmap */
	return 0;
}

void *
cos_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	void *ret=0;

	if (addr != NULL) {
		printc("parameter void *addr is not supported!\n");
		errno = ENOTSUP;
		return MAP_FAILED;
	}
	if (fd != -1) {
		printc("file mapping is not supported!\n");
		errno = ENOTSUP;
		return MAP_FAILED;
	}

	//addr = (void *)cos_page_bump_allocn(&cos_defcompinfo_curr_get()->ci, length);
	addr = (void *)memmgr_heap_page_allocn(length / PAGE_SIZE);
	if (!addr){
		ret = (void *) -1;
	} else {
		ret = addr;
	}

	if (ret == (void *)-1) {  /* return value comes from man page */
		printc("mmap() failed!\n");
		/* This is a best guess about what went wrong */
		errno = ENOMEM;
	}
	return ret;
}

int
cos_munmap(void *start, size_t length)
{
	printc("munmap not implemented\n");
	errno = ENOSYS;
	return -1;
}

int
cos_madvise(void *start, size_t length, int advice)
{
	/* We don't do anything with the advice from madvise, but that isn't really a problem */
	return 0;
}

void *
cos_mremap(void *old_address, size_t old_size, size_t new_size, int flags)
{
	printc("mremap not implemented\n");
	errno = ENOSYS;
	return (void*) -1;
}

int
cos_rt_sigprocmask(int how, void* set, void* oldset, size_t sigsetsize)
{
	/* Musl uses this at thread create time */
	printc("rt_sigprocmask not implemented\n");
	errno = ENOSYS;
	return -1;
}

int
cos_mprotect(void *addr, size_t len, int prot)
{
	/* Musl uses this at thread create time */
	printc("mprotect not implemented\n");
	return 0;
}

/* Thread related functions */
pid_t
cos_gettid(void)
{
	//return (pid_t) sl_thdid();
	return 0;
}

int
cos_tkill(int tid, int sig)
{
	if (sig == SIGABRT || sig == SIGKILL) {
		printc("Abort requested, complying...\n");
		ABORT();
	} else if (sig == SIGFPE) {
		printc("Floating point error, aborting...\n");
		ABORT();
	} else if (sig == SIGILL) {
		printc("Illegal instruction, aborting...\n");
		ABORT();
	} else if (sig == SIGINT) {
		printc("Terminal interrupt, aborting?\n");
		ABORT();
	} else {
		printc("Unknown signal %d\n", sig);
		assert(0);
	}

	assert(0);
	return 0;
}

static inline microsec_t
time_to_microsec(const struct timespec *t)
{
	time_t seconds          = t->tv_sec;
	long nano_seconds       = t->tv_nsec;
	microsec_t microseconds = seconds * 1000000 + nano_seconds / 1000;

	return microseconds;
}

long
cos_nanosleep(const struct timespec *req, struct timespec *rem)
{
	time_t remaining_seconds;
	long remaining_nano_seconds;
	microsec_t remaining_microseconds;
	cycles_t wakeup_deadline, wakeup_time;
	int completed_successfully;

	if (!req) {
		errno = EFAULT;
		return -1;
	}

	//wakeup_deadline        = sl_now() + sl_usec2cyc(time_to_microsec(req));
	wakeup_deadline = 0;
	//completed_successfully = sl_thd_block_timeout(0, wakeup_deadline);
	completed_successfully = 0;
	//wakeup_time   = sl_now();
	wakeup_time = 0;

	if (completed_successfully || wakeup_time > wakeup_deadline) {
		return 0;
	} else {
		errno = EINTR;
		if (rem) {
			//remaining_microseconds = sl_cyc2usec(wakeup_deadline - wakeup_time);
			remaining_microseconds = 0;
			remaining_seconds      = remaining_microseconds / 1000000;
			remaining_nano_seconds = (remaining_microseconds - remaining_seconds * 1000000) * 1000;
			*rem = (struct timespec) {
				.tv_sec = remaining_seconds,
				.tv_nsec = remaining_nano_seconds
			};
		}
		return -1;
	}
}


long
cos_set_tid_address(int *tidptr)
{
	/* Just do nothing for now and hope that works */
	return 0;
}

/* struct user_desc {
 *     int  		  entry_number; // Ignore
 *     unsigned int  base_addr; // Pass to cos thread mod
 *     unsigned int  limit; // Ignore
 *     unsigned int  seg_32bit:1;
 *     unsigned int  contents:2;
 *     unsigned int  read_exec_only:1;
 *     unsigned int  limit_in_pages:1;
 *     unsigned int  seg_not_present:1;
 *     unsigned int  useable:1;
 * };
 */

//void* backing_data[SL_MAX_NUM_THDS];
void* backing_data[1];

static void
setup_thread_area(struct sl_thd *thread, void* data)
{
	struct cos_compinfo *ci = cos_compinfo_get(cos_defcompinfo_curr_get());
	//thdid_t thdid = sl_thd_thdid(thread);
	thdid_t thdid = 0;

	backing_data[thdid] = data;

	//cos_thd_mod(ci, sl_thd_thdcap(thread), &backing_data[thdid]);
	cos_thd_mod(ci, 0, &backing_data[thdid]);
}

int
cos_set_thread_area(void* data)
{
	//setup_thread_area(sl_thd_curr(), data);
	setup_thread_area(0, data);
	return 0;
}

int
cos_clone(int (*func)(void *), void *stack, int flags, void *arg, pid_t *ptid, void *tls, pid_t *ctid)
{
	if (!func) {
		errno = EINVAL;
		return -1;
	}

	//struct sl_thd * thd = sl_thd_alloc((cos_thd_fn_t) func, arg);
	struct sl_thd * thd = 0;
	if (tls) {
		setup_thread_area(thd, tls);
	}
	//return sl_thd_thdid(thd);
	return 0;
}

#define FUTEX_WAIT		0
#define FUTEX_WAKE		1
#define FUTEX_FD		2
#define FUTEX_REQUEUE		3
#define FUTEX_CMP_REQUEUE	4
#define FUTEX_WAKE_OP		5
#define FUTEX_LOCK_PI		6
#define FUTEX_UNLOCK_PI		7
#define FUTEX_TRYLOCK_PI	8
#define FUTEX_WAIT_BITSET	9

#define FUTEX_PRIVATE 128

#define FUTEX_CLOCK_REALTIME 256

struct futex_data
{
	int *uaddr;
	void* waiters;
};

struct futex_waiter
{
	thdid_t thdid;
	void* list;
};

#define FUTEX_COUNT 20
struct futex_data futexes[FUTEX_COUNT];

struct futex_data *
lookup_futex(int *uaddr)
{
	assert(0);
}

/* TODO: Cleanup empty futexes */

//struct sl_lock futex_lock = SL_LOCK_STATIC_INIT();

/*
 * precondition: futex_lock is taken
 */
int
cos_futex_wait(struct futex_data *futex, int *uaddr, int val, const struct timespec *timeout)
{
	return 0;
}

int cos_futex_wake(struct futex_data *futex, int wakeup_count)
{
	return 0;
}

int
cos_futex(int *uaddr, int op, int val,
          const struct timespec *timeout, /* or: uint32_t val2 */
		  int *uaddr2, int val3)
{
	return 0;
}


void
pre_syscall_default_setup()
{
	printc("pre_syscall_default_setup\n");

	struct cos_defcompinfo *defci = cos_defcompinfo_curr_get();
	struct cos_compinfo    *ci    = cos_compinfo_get(defci);

	cos_defcompinfo_init();
	cos_meminfo_init(&(ci->mi), BOOT_MEM_KM_BASE, COS_MEM_KERN_PA_SZ, BOOT_CAPTBL_SELF_UNTYPED_PT);
	//sl_init(SL_MIN_PERIOD_US);
}

void
syscall_emulation_setup(void)
{
	printc("syscall_emulation_setup\n");

	int i;
	for (i = 0; i < SYSCALLS_NUM; i++) {
		cos_syscalls[i] = 0;
	}

	libc_syscall_override((cos_syscall_t)cos_write, __NR_write);
	libc_syscall_override((cos_syscall_t)cos_writev, __NR_writev);
	libc_syscall_override((cos_syscall_t)cos_ioctl, __NR_ioctl);
	libc_syscall_override((cos_syscall_t)cos_brk, __NR_brk);
	libc_syscall_override((cos_syscall_t)cos_mmap, __NR_mmap);
	libc_syscall_override((cos_syscall_t)cos_mmap, __NR_mmap2);
	libc_syscall_override((cos_syscall_t)cos_munmap, __NR_munmap);
	libc_syscall_override((cos_syscall_t)cos_madvise, __NR_madvise);
	libc_syscall_override((cos_syscall_t)cos_mremap, __NR_mremap);

	libc_syscall_override((cos_syscall_t)cos_nanosleep, __NR_nanosleep);

	libc_syscall_override((cos_syscall_t)cos_rt_sigprocmask, __NR_rt_sigprocmask);
	libc_syscall_override((cos_syscall_t)cos_mprotect, __NR_mprotect);

	libc_syscall_override((cos_syscall_t)cos_gettid, __NR_gettid);
	libc_syscall_override((cos_syscall_t)cos_tkill, __NR_tkill);
	libc_syscall_override((cos_syscall_t)cos_set_thread_area, __NR_set_thread_area);
	libc_syscall_override((cos_syscall_t)cos_set_tid_address, __NR_set_tid_address);
	libc_syscall_override((cos_syscall_t)cos_clone, __NR_clone);
	libc_syscall_override((cos_syscall_t)cos_futex, __NR_futex);
}

long
cos_syscall_handler(int syscall_num, long a, long b, long c, long d, long e, long f)
{
	assert(syscall_num <= SYSCALLS_NUM);
	/* printc("Making syscall %d\n", syscall_num); */
	if (!cos_syscalls[syscall_num]){
		printc("WARNING: Component %ld calling unimplemented system call %d\n", cos_spd_id(), syscall_num);
		assert(0);
		return 0;
	} else {
		return cos_syscalls[syscall_num](a, b, c, d, e, f);
	}
}

void
libc_initialization_handler()
{
	printc("libc_init\n");
	libc_init();
}
