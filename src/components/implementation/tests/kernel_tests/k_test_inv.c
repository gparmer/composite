/*
 * Test allocates a tcap_invocation anf proceeds to invoke it
 * It additionally measures the roundtrip time
 */

#include <stdint.h>
#include "kernel_tests.h"

static int      failure = 0;
static struct perfdata result;
struct results  result_sinv;

#define ARRAY_SIZE 10000
static cycles_t test_results[ARRAY_SIZE] = { 0 };

int
test_serverfn(int a, int b, int c)
{
        return 0xDEADBEEF;
}

extern void *__inv_test_serverfn(int a, int b, int c);

static inline int
call_cap_mb(u32_t cap_no, int arg1, int arg2, int arg3)
{
        int ret;

        /*
         * Which stack should we use for this invocation?  Simple, use
         * this stack, at the current sp.  This is essentially a
         * function call into another component, with odd calling
         * conventions.
         */
        cap_no = (cap_no + 1) << COS_CAPABILITY_OFFSET;

#if defined(__x86__)
        __asm__ __volatile__("pushl %%ebp\n\t"
                             "movl %%esp, %%ebp\n\t"
                             "movl %%esp, %%edx\n\t"
                             "movl $1f, %%ecx\n\t"
                             "sysenter\n\t"
                             "1:\n\t"
                             "popl %%ebp"
                             : "=a"(ret)
                             : "a"(cap_no), "b"(arg1), "S"(arg2), "D"(arg3)
                             : "memory", "cc", "ecx", "edx");

#elif defined(__arm__)

	__asm__ __volatile__("ldr r1,%[_cap_no]\n\t"
			     "ldr r2,%[_arg1]\n\t"
			     "ldr r3,%[_arg2]\n\t"
			     "ldr r4,%[_arg3]\n\t"
			     "mov r5, #0\n\t"
			     "svc #0x00\n\t"
			     "str r0, %[_ret]\n\t"
			     : [ _ret ] "=m"(ret)
			     : [ _cap_no ] "m"(cap_no), [ _arg1 ] "m"(arg1), [ _arg2 ] "m"(arg2), [ _arg3 ] "m"(arg3)
			     : "memory", "cc", "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "lr");

#else 
	assert(0);
#endif
        return ret;
}

void
test_inv(void)
{
        compcap_t        cc;
        sinvcap_t        ic;
        unsigned int r;
        int                  i;
        cycles_t         start_cycles = 0LL, end_cycles = 0LL;

        perfdata_init(&result, "SINV", test_results, ARRAY_SIZE);

        cc = cos_comp_alloc(&booter_info, booter_info.captbl_cap, booter_info.pgtbl_cap, (vaddr_t)NULL);
        if (EXPECT_LL_LT(1, cc, "Invocation: Cannot Allocate")) return;
        ic = cos_sinv_alloc(&booter_info, cc, (vaddr_t)__inv_test_serverfn, 0xdead);
        if (EXPECT_LL_LT(1, ic, "Invocation: Cannot Allocate")) return;

        r = call_cap_mb(ic, 1, 2, 3);
        if (EXPECT_LLU_NEQ(0xDEADBEEF, r, "Test Invocation")) return;
	perfcntr_init(); /* 32bit counter, so resetting before every benchmark */

        for (i = 0; i < ITER; i++) {
                start_cycles = ps_tsc();
                call_cap_mb(ic, 1, 2, 3);
                end_cycles = ps_tsc();

                perfdata_add(&result, end_cycles - start_cycles);
        }

        perfdata_calc(&result);
	results_save(&result_sinv, &result);

        CHECK_STATUS_FLAG();
        PRINTC("\t%s: \t\tSuccess\n", "Synchronous Invocations");
        EXIT_FN();
}
