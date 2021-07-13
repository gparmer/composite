#ifndef UTIL_H
#define UTIL_H

#define CAS_SUCCESS 1
/*
 * Return values:
 * 0 on failure due to contention (*target != old)
 * 1 otherwise (*target == old -> *target = updated)
 */
static inline int
cos_cas_32(u32_t *target, u32_t old, u32_t updated)
{
	char z;
	__asm__ __volatile__("lock cmpxchgl %2, %0; setz %1"
	                     : "+m"(*target), "=a"(z)
	                     : "q"(updated), "a"(old)
	                     : "memory", "cc");
	return (int)z;
}

static inline int
cos_cas(unsigned long *target, unsigned long old, unsigned long updated)
{
	char z;
	#if defined(__x86_64__)
	__asm__ __volatile__("lock cmpxchgq %2, %0; setz %1"
	                     : "+m"(*target), "=a"(z)
	                     : "q"(updated), "a"(old)
	                     : "memory", "cc");
	#elif defined(__i386__)
	__asm__ __volatile__("lock cmpxchgl %2, %0; setz %1"
	                     : "+m"(*target), "=a"(z)
	                     : "q"(updated), "a"(old)
	                     : "memory", "cc");
	#endif 

	return (int)z;
}
/* Fetch-and-add implementation on x86. It returns the original value
 * before xaddl. */
static inline int
cos_faa(int *var, int value)
{
	__asm__ __volatile__("lock xaddl %%eax, %2;"
	                     : "=a"(value)           // Output
	                     : "a"(value), "m"(*var) // Input
	                     : "memory");
	return value;
}

/* x86 cpuid instruction barrier. */
static inline void
cos_inst_bar(void)
{
	int eax, edx, code = 0;
	(void)eax;
	(void)edx;
	(void)code;

	__asm__ __volatile__("cpuid" : "=a"(eax), "=d"(edx) : "a"(code) : "ecx", "ebx");

	return;
}

#ifndef rdtscll
#define rdtscll(val) __asm__ __volatile__("rdtsc" : "=A"(val))
#endif

#endif
