#ifndef CHAL_CONSTS_H
#define CHAL_CONSTS_H

#include "../chal_config.h"

struct pt_regs {
	u64_t ds;
	u64_t es;
	u64_t fs;
	u64_t gs;
	u64_t r15;
	u64_t r14;
	u64_t r13;
	u64_t r12;
	u64_t r11;
	u64_t r10;
	u64_t r9;
	u64_t r8;
	u64_t bx;
	u64_t cx;
	u64_t dx;
	u64_t si;
	u64_t di;
	u64_t bp;
	u64_t ax;	
	u64_t orig_ax;
	u64_t ip;
	u64_t cs;
	u64_t flags;
	u64_t sp;
	u64_t ss;
};

#define PAGE_MASK (~(PAGE_SIZE - 1))
#define PGD_SHIFT 39
#define PGD_RANGE (1LU << PGD_SHIFT)
#define PGD_SIZE PGD_RANGE
#define PGD_MASK (~(PGD_RANGE - 1))
#define PGD_PER_PTBL 1024

#define PGT3_SHIFT 30
#define PGT3_RANGE (1UL << PGT3_SHIFT)
#define PGT3_SIZE PGT3_RANGE
#define PGT3_MASK (~(PGT3_RANGE - 1))
#define PGT3_PER_PTBL 512
#endif /* CHAL_CONSTS_H */