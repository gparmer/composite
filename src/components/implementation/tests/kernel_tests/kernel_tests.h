#ifndef KERNEL_TESTS_H
#define KERNEL_TESTS_H

#include <stdio.h>
#include <string.h>

#include <cos_debug.h>
#include <llprint.h>

#undef assert
/* On assert, immediately switch to the "exit" thread */
#define assert(node)                                                        \
        do {                                                                \
                if (unlikely(!(node))) {                                    \
                        debug_print("assert error in @ ");                  \
                        cos_thd_switch(termthd[cos_cpuid()]);               \
                }                                                           \
        } while (0)

#define EXIT_FN()                                                           \
                exit_fn: return;

#define CHECK_STATUS_FLAG()                                                 \
        do {                                                                \
                if (failure) {                                              \
                        goto exit_fn;                                       \
                }                                                           \
        } while (0)

#include <cos_component.h>
#include <cos_kernel_api.h>
#include <perfdata.h>
#include <cos_ubench.h>

#define PERF
#define ITER 10000
#define TEST_NTHDS 5
#define CHAR_BIT 8
#define TEST_ITER 16

#define THD_ARG 666             /* Thread Argument to pass */
#define NUM_TEST 16             /* Iterator NUM */
#define MAX_THDS 4              /* Max Threshold Multiplier */
#define MIN_THDS 0.5            /* Min Threshold Multiplier */
#define GRANULARITY 1000        /* Granularity */

#define TEST_NPAGES (1024 * 2)  /* Testing with 8MB for now */

extern unsigned int cyc_per_usec;

extern struct cos_compinfo booter_info;
extern thdcap_t         termthd[]; /* switch to this to shutdown */
extern unsigned long    tls_test[][TEST_NTHDS];
extern unsigned long    thd_test[TEST_NTHDS];
extern int              num, den, count;

struct results {
        long long unsigned avg;
        long long unsigned max;
        long long unsigned min;
        long long unsigned sd;
        int                sz;
        long long unsigned p90tile;
        long long unsigned p95tile;
        long long unsigned p99tile;
};

static inline void
results_save(struct results *r, struct perfdata *p)
{
        r->avg = perfdata_avg(p);
        r->max = perfdata_max(p);
        r->min = perfdata_min(p);
        r->sz = perfdata_sz(p);
        r->sd = perfdata_sd(p);
        r->p90tile = perfdata_90ptile(p);
        r->p95tile = perfdata_95ptile(p);
        r->p99tile = perfdata_99ptile(p);
}

static inline void
results_split_print(struct results *r, const char *testname)
{
        long unsigned avg, avg_h;
        long unsigned max, max_h;
        long unsigned min, min_h;
        long unsigned sd, sd_h;
        int           sz;
        long unsigned p90tile, p90_h;
        long unsigned p95tile, p95_h;
        long unsigned p99tile, p99_h;	

	avg = r->avg & 0xffffffff;
	avg_h = r->avg >> 32;
	max = r->max & 0xffffffff;
	max_h = r->max >> 32;
	min = r->min & 0xffffffff;
	min_h = r->min >> 32;
	sd = r->sd & 0xffffffff;
	sd_h = r->sd >> 32;
	sz = r->sz;
	p90tile = r->p90tile & 0xffffffff;
	p90_h = r->p90tile >> 32;
	p95tile = r->p95tile & 0xffffffff;
	p95_h = r->p95tile >> 32;
	p99tile = r->p99tile & 0xffffffff;
	p99_h = r->p99tile >> 32;
	
	PRINTC("%s\n", testname);
	PRINTC("\t\tAvg:%lu (%lu), Max:%lu (%lu), Min:%lu (%lu), Iters: %d\n", avg, avg_h, max, max_h, min, min_h, sz);
	PRINTC("\t\tSD:%lu (%lu), 90%%:%lu (%lu), 95%%:%lu (%lu), 99%%:%lu (%lu)\n", sd, sd_h, p90tile, p90_h, p95tile, p95_h, p99tile, p99_h);
}

static inline void
results_print(struct results *r, const char *testname)
{
	PRINTC("%s\n", testname);
	PRINTC("\t\tAvg:%llu, Max:%llu, Min:%llu, Iters: %d\n", r->avg, r->max, r->min, r->sz);
	PRINTC("\t\tSD:%llu, 90%%:%llu, 95%%:%llu, 99%%:%llu\n", r->sd, r->p90tile, r->p95tile, r->p99tile);
}

#if defined(__x86__)
static unsigned long
tls_get(size_t off)
{
        unsigned long val;

        __asm__ __volatile__("movl %%gs:(%1), %0" : "=r"(val) : "r"(off) :);

        return val;
}

static void
tls_set(size_t off, unsigned long val)
{
        __asm__ __volatile__("movl %0, %%gs:(%1)" : : "r"(val), "r"(off) : "memory");
}
#endif

extern void test_run_perf_kernel(void);
extern void test_timer(void);
extern void test_tcap_budgets(void);
extern void test_2timers(void);
extern void test_thds(void);
extern void test_mem_alloc(void);
extern void test_async_endpoints(void);
extern void test_inv(void);
extern void test_captbl_expands(void);

#endif /* KERNEL_TESTS_H */
