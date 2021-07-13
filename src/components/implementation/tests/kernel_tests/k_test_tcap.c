/*
 * Test measures Tcaps. It allocates a
 * It provides a desired execution time and it runs it
 * First test_timer progrmas the LAPIC and measures if it interrupted correctly
 * Then it assigns a budget through the TCAP and measures the interrupt timer
 * Then compares both by programming the LAPIC and then assigning a budget
 */

#include <stdint.h>
#include "kernel_tests.h"

struct results result_test_timer;
struct results result_budgets_single;
static struct perfdata result;

#define ARRAY_SIZE 10000
static cycles_t test_results[ARRAY_SIZE] = { 0 };

static void
spinner(void *d)
{
        while (1);
}

void
sched_events_clear(void)
{
        thdid_t     tid;
        int         blocked, rcvd;
        cycles_t    cycles, now;
        tcap_time_t timer, thd_timeout;

        while (cos_sched_rcv(BOOT_CAPTBL_SELF_INITRCV_CPU_BASE, RCV_ALL_PENDING, 0,
                                                 &rcvd, &tid, &blocked, &cycles, &thd_timeout) != 0);

}

void
test_timer(void)
{
        thdcap_t    tc;
        cycles_t    c = 0, p = 0;
        int         i, ret;
        cycles_t    s, e;
        thdid_t     tid;
        int         blocked, rcvd;
        cycles_t    cycles, now, utime;
        long long   time, mask;
        tcap_time_t timer, thd_timeout;

        tc = cos_thd_alloc(&booter_info, booter_info.comp_cap, spinner, NULL);

        perfdata_init(&result, "COS THD => COS_THD_SWITCH", test_results, ARRAY_SIZE);

        for (i = 0; i <= TEST_ITER; i++){
                rdtscll(now);
                timer = tcap_cyc2time(now + GRANULARITY * cyc_per_usec);
                cos_switch(tc, BOOT_CAPTBL_SELF_INITTCAP_CPU_BASE, 0, timer, BOOT_CAPTBL_SELF_INITRCV_CPU_BASE,
                           cos_sched_sync());
                p = c;
                rdtscll(c);
                time = (c - now - (cycles_t)(GRANULARITY * cyc_per_usec));
                mask = (time >> (sizeof(long long) * CHAR_BIT - 1));
                utime = (time + mask) ^ mask;

                if (i > 0) {
                        perfdata_add(&result, utime);

                        if (EXPECT_LLU_LT((long long unsigned)(c-now), (unsigned)(GRANULARITY * cyc_per_usec * MAX_THDS),
                                            "Timer: Failure on  MAX") ||
                                EXPECT_LLU_LT((unsigned)(GRANULARITY * cyc_per_usec * MIN_THDS), (long long unsigned)(c-now),
                                            "Timer: failure on MIN")) {
                                return;
                        }
                }
                sched_events_clear();
        }

        perfdata_calc(&result);
        results_save(&result_test_timer, &result);	

        /* Timer in past */
        c = 0, p = 0;

        rdtscll(c);
        timer = tcap_cyc2time(c - GRANULARITY * cyc_per_usec);
        cos_switch(tc, BOOT_CAPTBL_SELF_INITTCAP_CPU_BASE, 0, timer, BOOT_CAPTBL_SELF_INITRCV_CPU_BASE,
                    cos_sched_sync());
        p = c;
        rdtscll(c);

        if (EXPECT_LLU_LT((long long unsigned)(c-p), (unsigned)(GRANULARITY * cyc_per_usec), "Timer: Past")) {
                return;
        }

        sched_events_clear();

        /* Timer now */
        c = 0, p = 0;

        rdtscll(c);
        timer = tcap_cyc2time(c);
        cos_switch(tc, BOOT_CAPTBL_SELF_INITTCAP_CPU_BASE, 0, timer, BOOT_CAPTBL_SELF_INITRCV_CPU_BASE,
                    cos_sched_sync());
        p = c;
        rdtscll(c);

        if (EXPECT_LLU_LT((long long unsigned)(c-p), (unsigned)(GRANULARITY * cyc_per_usec), "Timer:  Now")) {
                return;
        }

        cos_sched_rcv(BOOT_CAPTBL_SELF_INITRCV_CPU_BASE, RCV_ALL_PENDING, 0,
                      &rcvd, &tid, &blocked, &cycles, &thd_timeout)
                        ;

        EXPECT_LLU_LT((long long unsigned)cycles, (long long unsigned)(c-p), "Timer => Cycles time");

        sched_events_clear();
        PRINTC("\t%s: \t\t\tSuccess\n", "One-Shot Timeout");
}

struct exec_cluster {
        thdcap_t        tc;
        arcvcap_t   rc;
        tcap_t          tcc;
        cycles_t        cyc;
        asndcap_t   sc;                 /* send-cap to send to rc */
        tcap_prio_t prio;
        int                 xseq;           /* expected activation sequence number for this thread */
};

struct budget_test_data {
        /* p=parent, c=child, g=grand-child */
        struct exec_cluster p, c, g;
} bt[NUM_CPU], mbt[NUM_CPU];

static int
exec_cluster_alloc(struct exec_cluster *e, cos_thd_fn_t fn, void *d, arcvcap_t parentc)
{
        e->tcc = cos_tcap_alloc(&booter_info);
        if (EXPECT_LL_LT(1, e->tcc, "Cluster Allocation: TCAP ALLOC")) return -1;
        e->tc = cos_thd_alloc(&booter_info, booter_info.comp_cap, fn, d);
        if (EXPECT_LL_LT(1, e->tc, "Cluster Allocation: THD ALLOC")) return -1;
        e->rc = cos_arcv_alloc(&booter_info, e->tc, e->tcc, booter_info.comp_cap, parentc);
        if (EXPECT_LL_LT(1, e->rc, "Cluster Allocation: ARCV ALLOC")) return -1;
        e->sc = cos_asnd_alloc(&booter_info, e->rc, booter_info.captbl_cap);
        if (EXPECT_LL_LT(1, e->sc, "Cluster Allocation: ASND ALLOC")) return -1;

        e->cyc = 0;

        return 0;
}

static void
parent(void *d)
{
        assert(0);
}

static void
spinner_cyc(void *d)
{
        cycles_t *p = (cycles_t *)d;

        while (1) rdtscll(*p);
}

#define TIMER_TIME 100

void
test_2timers(void)
{
        int ret;
        cycles_t        s, e, timer;

        if (EXPECT_LL_NEQ(0, exec_cluster_alloc(&bt[cos_cpuid()].p, parent, &bt[cos_cpuid()].p,
                                                BOOT_CAPTBL_SELF_INITRCV_CPU_BASE), "TCAP v. Timer: Cannot Allocate")) {
                return;
        }
        if (EXPECT_LL_NEQ(0, exec_cluster_alloc(&bt[cos_cpuid()].c, spinner, &bt[cos_cpuid()].c,
                                                bt[cos_cpuid()].p.rc), "TCAP v. Timer: Cannot Allocate")) {
                return;
        }

        /* Timer > TCAP */

        ret = cos_tcap_transfer(bt[cos_cpuid()].c.rc, BOOT_CAPTBL_SELF_INITTCAP_CPU_BASE,
                                GRANULARITY * TIMER_TIME, TCAP_PRIO_MAX + 2);
        if (EXPECT_LL_NEQ(0, ret, "TCAP v. Timer : TCAP Transfer")) {
                return;
        }

        rdtscll(s);
        timer = tcap_cyc2time(s + GRANULARITY * cyc_per_usec);
        if (cos_switch(bt[cos_cpuid()].c.tc, bt[cos_cpuid()].c.tcc, TCAP_PRIO_MAX + 2,
                       timer, BOOT_CAPTBL_SELF_INITRCV_CPU_BASE, cos_sched_sync())) {
                EXPECT_LL_NEQ(0, 1, "TCAP v. Timer: COS Switch");
                return;
        }
        rdtscll(e);

        if (EXPECT_LLU_LT((long long unsigned)(e-s), (unsigned)(GRANULARITY * cyc_per_usec),
                                          "TCAP v. Timer: Timer > TCAP") ||
                EXPECT_LLU_LT((unsigned)(GRANULARITY * TIMER_TIME), (long long unsigned)(e-s),
                                          "TCAP v. Timer: Interreupt Under")) {
                return;
        }

        sched_events_clear();

        /* Timer < TCAP */

        ret = cos_tcap_transfer(bt[cos_cpuid()].c.rc, BOOT_CAPTBL_SELF_INITTCAP_CPU_BASE,
                                GRANULARITY * cyc_per_usec, TCAP_PRIO_MAX + 2);
        if (EXPECT_LL_NEQ(0, ret, "TCAP v. Timer: TCAP Transfer")) {
                return;
        }

        rdtscll(s);
        timer = tcap_cyc2time(s + GRANULARITY * TIMER_TIME);
        if (EXPECT_LL_NEQ(0, cos_switch(bt[cos_cpuid()].c.tc, bt[cos_cpuid()].c.tcc, TCAP_PRIO_MAX + 2, timer,
                                        BOOT_CAPTBL_SELF_INITRCV_CPU_BASE, cos_sched_sync()), "TCAP v. TImer: COS Switch")) {
                return;
        }

        rdtscll(e);

        if (EXPECT_LLU_LT((long long unsigned)(e-s), (unsigned)(GRANULARITY * cyc_per_usec),
                          "TCAP v. Timer: Timer < TCAP") ||
                EXPECT_LLU_LT((unsigned)(GRANULARITY * TIMER_TIME), (long long unsigned)(e-s),
                               "TCAP v. Timer: Interreupt Under")) {
                return;
        }

        sched_events_clear();
        PRINTC("\t%s: \t\tSuccess\n", "Timer => Timeout v. Budget");
}

#define BUDGET_TIME 100

static void
test_tcap_budgets_single(void)
{
        int         i;
        cycles_t    s = 0, e = 0;
        cycles_t    time, mask;
        int         ret;

        perfdata_init(&result, "Timer => Budget based", test_results, ARRAY_SIZE);

        if (EXPECT_LL_NEQ(0, exec_cluster_alloc(&bt[cos_cpuid()].p, parent, &bt[cos_cpuid()].p,
                          BOOT_CAPTBL_SELF_INITRCV_CPU_BASE), "Single Budget: Cannot Allocate") ||
                EXPECT_LL_NEQ(0, exec_cluster_alloc(&bt[cos_cpuid()].c, spinner, &bt[cos_cpuid()].c,
                              bt[cos_cpuid()].p.rc), "Single Budget: Cannot Allocate")) {
                return;
        }
        for (i = 1; i <= TEST_ITER; i++) {

                ret = cos_tcap_transfer(bt[cos_cpuid()].c.rc, BOOT_CAPTBL_SELF_INITTCAP_CPU_BASE,
                                        GRANULARITY * BUDGET_TIME, TCAP_PRIO_MAX + 2);
                if (EXPECT_LL_NEQ(0, ret, "Single Budget: TCAP Transfer")) {
                        return;
                }

                rdtscll(s);
                if (cos_switch(bt[cos_cpuid()].c.tc, bt[cos_cpuid()].c.tcc, TCAP_PRIO_MAX + 2, TCAP_TIME_NIL,
                               BOOT_CAPTBL_SELF_INITRCV_CPU_BASE, cos_sched_sync())){
                        EXPECT_LL_NEQ(0, 1, "Single Budget: COS Switch");
                        return;
                }
                rdtscll(e);

                if (i > 1) {
                        /* Performant absolute value function instead of branching */
                        time = (e - s - (GRANULARITY * BUDGET_TIME));
                        mask = (time >> (sizeof(cycles_t) * CHAR_BIT - 1));
                        time = (time + mask) ^ mask;

                        perfdata_add(&result, time);

                        if (EXPECT_LLU_LT((long long unsigned)(e-s), (unsigned)(GRANULARITY * BUDGET_TIME * MAX_THDS),
                                          "Single Budget: MAX Bound") ||
                                EXPECT_LLU_LT((unsigned)(GRANULARITY * BUDGET_TIME * MIN_THDS), (long long unsigned)(e-s),
                                               "Single Budget: MIN Bound")) {
                                return;
                        }
                }
                sched_events_clear();
        }

        perfdata_calc(&result);
        results_save(&result_budgets_single, &result);	

        PRINTC("\t%s: \t\t\tSuccess\n", "Timer => Budget based");
}

#define RATE_1 1600
#define RATE_2 800

static void
test_tcap_budgets_multi(void)
{
        int i;

        if(EXPECT_LL_NEQ(0, exec_cluster_alloc(&mbt[cos_cpuid()].p, spinner_cyc, &(mbt[cos_cpuid()].p.cyc),
                                         BOOT_CAPTBL_SELF_INITRCV_CPU_BASE), "Multi Budget: Cannot Allocate") ||
           EXPECT_LL_NEQ(0, exec_cluster_alloc(&mbt[cos_cpuid()].c, spinner_cyc, &(mbt[cos_cpuid()].c.cyc),
                                         mbt[cos_cpuid()].p.rc), "Multi Budget: Cannot Allocate") ||
           EXPECT_LL_NEQ(0, exec_cluster_alloc(&mbt[cos_cpuid()].g, spinner_cyc, &(mbt[cos_cpuid()].g.cyc),
                                         mbt[cos_cpuid()].c.rc), "Multi Budget: Cannot allocate")) {
                return;
        }

        for (i = 1; i <= TEST_ITER; i++) {
                tcap_res_t  res;
                thdid_t     tid;
                int         blocked;
                cycles_t    cycles, s, e;
                tcap_time_t thd_timeout;

		(void)e;
                                        /* test both increasing budgets and constant budgets */
                if (i > (TEST_ITER/2))
                        res = GRANULARITY * RATE_1;
                else
                        res = i * GRANULARITY * RATE_2;

                if (EXPECT_LL_NEQ(0, cos_tcap_transfer(mbt[cos_cpuid()].p.rc, BOOT_CAPTBL_SELF_INITTCAP_CPU_BASE,
                                  res, TCAP_PRIO_MAX + 2), "Multi Budget: TCAP Transfer") ||
                        EXPECT_LL_NEQ(0, cos_tcap_transfer(mbt[cos_cpuid()].c.rc, mbt[cos_cpuid()].p.tcc, res / 2,
                                      TCAP_PRIO_MAX + 2), "Multi Budget: TCAP Transfer") ||
                        EXPECT_LL_NEQ(0, cos_tcap_transfer(mbt[cos_cpuid()].g.rc, mbt[cos_cpuid()].c.tcc, res / 4,
                                      TCAP_PRIO_MAX + 2), "Multi Budget: TCAP Transfer")) {
                        return;
                }

                mbt[cos_cpuid()].p.cyc = mbt[cos_cpuid()].c.cyc = mbt[cos_cpuid()].g.cyc = 0;
                rdtscll(s);
                if (cos_switch(mbt[cos_cpuid()].g.tc, mbt[cos_cpuid()].g.tcc, TCAP_PRIO_MAX + 2, TCAP_TIME_NIL,
                               BOOT_CAPTBL_SELF_INITRCV_CPU_BASE, cos_sched_sync())) {
                        EXPECT_LL_NEQ(0, 1, "Multi Budget: COS Switch");
                        return;
                }
                rdtscll(e);

                cos_sched_rcv(BOOT_CAPTBL_SELF_INITRCV_CPU_BASE, 0, 0, NULL, &tid, &blocked, &cycles, &thd_timeout);

                if ( i > 1) {

                        /* To measure time of execution, we need a min time
                         * as well as a max time to determine
                         * if the interrupt happened when it was supposed to
                         * thus MAX bound and MIN bound
                         * MAX_THDS and MIN_THDS are #defined to give it some flexibility
                         * from the user
                         */

                        if (EXPECT_LLU_LT((mbt[cos_cpuid()].g.cyc - s), (res / 4 * MAX_THDS), "Multi Budget: G")       ||
                            EXPECT_LLU_LT(mbt[cos_cpuid()].g.cyc - s, res / 4 * MAX_THDS, "Multi Budget: G MAX Bound") ||
                            EXPECT_LLU_LT(res / 4 * MIN_THDS, mbt[cos_cpuid()].g.cyc - s, "Multi Budget: G MIN Bound") ||
                            EXPECT_LLU_LT(mbt[cos_cpuid()].c.cyc - s, res / 2 * MAX_THDS, "Multi Budget: C MAX Bound") ||
                            EXPECT_LLU_LT(res / 2 * MIN_THDS, mbt[cos_cpuid()].c.cyc - s, "Multi Budget: C MIN Bound") ||
                            EXPECT_LLU_LT(mbt[cos_cpuid()].p.cyc - s, res * MAX_THDS, "Multi Budget: P MAX Bound")     ||
                            EXPECT_LLU_LT(res * MIN_THDS, mbt[cos_cpuid()].p.cyc - s, "Multi Budget: P MIN BOund")) {
                            return;
                        }
                }
        }
        PRINTC("\t%s: \t\tSuccess\n", "Timer => Hierarchical Budget");
}

void
test_tcap_budgets(void)
{
        /* single-level budgets test */
        test_tcap_budgets_single();

        /* multi-level budgets test */
        test_tcap_budgets_multi();
}
