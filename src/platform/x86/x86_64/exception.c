#include <pgtbl.h>
#include <thd.h>

#include "kernel.h"
#include "string.h"
#include "isr.h"
#include "chal_cpu.h"

int
div_by_zero_err_fault_handler(struct pt_regs *regs)
{
	print_pt_regs(regs);
	die("FAULT: Divide by Zero Error\n\n");

	return 1;
}

int
debug_trap_handler(struct pt_regs *regs)
{
	print_pt_regs(regs);
	die("TRAP: Debug\n");

	return 1;
}

int
breakpoint_trap_handler(struct pt_regs *regs)
{
	print_pt_regs(regs);
	die("TRAP: Breakpoint\n");

	return 1;
}

int
overflow_trap_handler(struct pt_regs *regs)
{
	print_pt_regs(regs);
	die("TRAP: Overflow\n");

	return 1;
}

int
bound_range_exceed_fault_handler(struct pt_regs *regs)
{
	print_pt_regs(regs);
	die("FAULT: Bound Range Exceeded\n");

	return 1;
}

int
invalid_opcode_fault_handler(struct pt_regs *regs)
{
	print_pt_regs(regs);
	die("FAULT: Invalid opcode\n");

	return 1;
}

int
device_not_avail_fault_handler(struct pt_regs *regs)
{
	print_pt_regs(regs);
	die("FAULT: Device Not Available\n");

	return 1;
}

int
double_fault_abort_handler(struct pt_regs *regs)
{
	print_pt_regs(regs);
	die("ABORT: Double Fault\n");

	return 1;
}

int
invalid_tss_fault_handler(struct pt_regs *regs)
{
	print_pt_regs(regs);
	die("FAULT: Invalid TSS\n");

	return 1;
}

int
seg_not_present_fault_handler(struct pt_regs *regs)
{
	print_pt_regs(regs);
	die("FAULT: Segment Not Present\n");

	return 1;
}

int
stack_seg_fault_handler(struct pt_regs *regs)
{
	print_pt_regs(regs);
	die("FAULT: Stack Segment Fault\n");

	return 1;
}

int
gen_protect_fault_handler(struct pt_regs *regs)
{
	print_pt_regs(regs);
	die("FAULT: General Protection Fault\n");

	return 1;
}

int
page_fault_handler(struct pt_regs *regs)
{
	u64_t                      fault_addr = 0, errcode = 0, rip = 0;
	struct cos_cpu_local_info *ci    = cos_cpu_local_info();
	thdid_t                    thdid = thd_current(ci)->tid;

	print_pt_regs(regs);
	fault_addr = chal_cpu_fault_vaddr(regs);
	errcode    = chal_cpu_fault_errcode(regs);
	rip        = chal_cpu_fault_ip(regs);

	die("FAULT: Page Fault in thd %d (%s %s %s %s %s) @ 0x%p, ip 0x%p\n", thdid,
	    errcode & PGTBL_PRESENT ? "present" : "not-present",
	    errcode & PGTBL_WRITABLE ? "write-fault" : "read-fault", errcode & PGTBL_USER ? "user-mode" : "system",
	    errcode & PGTBL_WT ? "reserved" : "", errcode & PGTBL_NOCACHE ? "instruction-fetch" : "", fault_addr, rip);

	return 1;
}

int
x87_float_pt_except_fault_handler(struct pt_regs *regs)
{
	print_pt_regs(regs);
	die("FAULT: x87 Floating-point Exception\n");

	return 1;
}

int
align_check_fault_handler(struct pt_regs *regs)
{
	print_pt_regs(regs);
	die("FAULT: Alignment Check\n");

	return 1;
}

int
machine_check_abort_handler(struct pt_regs *regs)
{
	print_pt_regs(regs);
	die("ABORT: Machine Check\n");

	return 1;
}

int
smid_float_pt_except_fault_handler(struct pt_regs *regs)
{
	print_pt_regs(regs);
	die("FAULT: SMID Floating-point Exception\n");

	return 1;
}

int
virtualization_except_fault_handler(struct pt_regs *regs)
{
	print_pt_regs(regs);
	die("FAULT: Virtualization Exception\n");

	return 1;
}

int
security_except_fault_handler(struct pt_regs *regs)
{
	print_pt_regs(regs);
	die("FAULT: Security Exception\n");

	return 1;
}
