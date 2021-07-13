#ifndef CHAL_CONFIG_H
#define CHAL_CONFIG_H

#ifndef COS_BASE_TYPES
#define COS_BASE_TYPES
typedef unsigned char      u8_t;
typedef unsigned short int u16_t;
typedef unsigned int       u32_t;
typedef unsigned long long u64_t;
typedef signed char        s8_t;
typedef signed short int   s16_t;
typedef signed int         s32_t;
typedef signed long long   s64_t;
#endif

typedef enum {
	HW_PERIODIC = 32, /* periodic timer interrupt */
	HW_KEYBOARD,      /* keyboard interrupt */
	HW_ID3,
	HW_ID4,
	HW_SERIAL, /* serial interrupt */
	HW_ID6,
	HW_ID7,
	HW_ID8,
	HW_ONESHOT, /* onetime timer interrupt */
	HW_ID10,
	HW_ID11,
	HW_ID12,
	HW_ID13,
	HW_ID14,
	HW_ID15,
	HW_ID16,
	HW_ID17,
	HW_ID18,
	HW_ID19,
	HW_ID20,
	HW_ID21,
	HW_ID22,
	HW_ID23,
	HW_ID24,
	HW_ID25,
	HW_ID26,
	HW_ID27,
	HW_ID28,
	HW_ID29,
	HW_ID30,
	HW_ID31,
	HW_LAPIC_SPURIOUS,
	HW_LAPIC_IPI_ASND    = 254, /* ipi interrupt for asnd */
	HW_LAPIC_TIMER = 255, /* Local APIC TSC-DEADLINE mode - Timer interrupts */
} hwid_t;

typedef struct {
	volatile unsigned int counter;
} atomic_t;

#define LOCK_PREFIX_HERE                  \
	".pushsection .smp_locks,\"a\"\n" \
	".balign 4\n"                     \
	".long 671f - .\n" /* offset */   \
	".popsection\n"                   \
	"671:"

#define LOCK_PREFIX LOCK_PREFIX_HERE "\n\tlock; "

static inline void
atomic_inc(atomic_t *v)
{
	__asm__ __volatile__(LOCK_PREFIX "incl %0" : "+m"(v->counter));
}

static inline void
atomic_dec(atomic_t *v)
{
	__asm__ __volatile__(LOCK_PREFIX "decl %0" : "+m"(v->counter));
}

static inline void
cos_mem_fence(void)
{
	__asm__ __volatile__("mfence" ::: "memory");
}

/* 256 entries. can be increased if necessary */
#define COS_THD_INIT_REGION_SIZE (1 << 8)
// Static entries are after the dynamic allocated entries
#define COS_STATIC_THD_ENTRY(i) ((i + COS_THD_INIT_REGION_SIZE + 1))

#define COS_SYSCALL __attribute__((regparm(0)))
#define KERNEL_PGD_REGION_OFFSET (PAGE_SIZE - PAGE_SIZE / 2)
#define KERNEL_PGD_REGION_SIZE (PAGE_SIZE / 2)

#endif /* CHAL_CONFIG_H */
