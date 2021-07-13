#include "string.h"
#include "kernel.h"

#define PRINTK_BUFFER 1024
#define MAX_HANDLERS 5

static void (*printk_handlers[MAX_HANDLERS])(const char *);
static unsigned num_handlers = 0;

int
printk_register_handler(void (*handler)(const char *))
{
	if (handler == NULL || num_handlers > (sizeof(printk_handlers) / sizeof(printk_handlers[0]))) return -1;
	printk_handlers[num_handlers++] = handler;
	return 0;
}

void
printk(const char *fmt, ...)
{
	char         buffer[PRINTK_BUFFER];
	va_list      args;
	unsigned int l, i;

	va_start(args, fmt);
	l = vsprintf(buffer, fmt, args);
	va_end(args);

	for (i = 0; i < num_handlers; i++) {
		printk_handlers[i](buffer);
	}
}

void
print_pt_regs(struct pt_regs *regs)
{
        PRINTK("Register hexdump (0x%p):\n", regs);
        PRINTK("General->   RAX: 0x%p, RBX: 0x%p, RCX: 0x%p, RDX: 0x%p\n",
	       regs->ax, regs->bx, regs->cx, regs->dx);
		PRINTK("General->   R8: 0x%p, R9: 0x%p, R10: 0x%p, R11: 0x%p\n",
	       regs->r8, regs->r9, regs->r10, regs->r11);
		PRINTK("General->   R12: 0x%p, R13: 0x%p, R14: 0x%p, R15: 0x%p\n",
	       regs->r12, regs->r13, regs->r14, regs->r15);

        PRINTK("Segment->   CS: 0x%x, DS: 0x%x, ES: 0x%x, FS: 0x%x, GS: 0x%x, SS: 0x%x\n",
	       regs->cs, regs->ds, regs->es, regs->fs, regs->gs, regs->ss);
        PRINTK("Index->     RSI: 0x%p, RDI: 0x%p, RIP: 0x%p, RSP: 0x%p, RBP: 0x%p\n",
	       regs->si, regs->di, regs->ip, regs->sp, regs->bp);
        PRINTK("Indicator-> RFLAGS: 0x%p\n", regs->flags);
        PRINTK("(Exception Error Code-> ORIG_AX: 0x%x)\n", regs->orig_ax);
}
