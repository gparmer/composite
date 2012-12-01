#include "thread.h"

#ifndef FPU_H
#define FPU_H

#define FPU_DISABLED 0x00000008

extern struct thread *last_used_fpu;

struct cos_fpu {
        unsigned int cwd; /* FPU Control Word*/
        unsigned int swd; /* FPU Status Word */
        unsigned int twd; /* FPU Tag Word */
        unsigned int fip; /* FPU IP Offset */
        unsigned int fcs; /* FPUIP Selector */
        unsigned int foo; /* FPU Operand Pointer Offset */
        unsigned int fos; /* FPU Operand Pointer Selector */

        /* 8*10 bytes for each FP-reg = 80 bytes: */
        unsigned int st_space[20]; /* 8 data registers */
	int status; /* used fpu */
	int saved_fpu;
};

inline void fsave(struct thread*);
inline void frstor(struct thread*);
inline void disable_fpu(void);
inline void enable_fpu(void);

unsigned int cos_read_cr0(void);
int save_fpu(struct thread *curr, struct thread *next);
int fpu_is_disabled(void);

#endif
