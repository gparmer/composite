#include <chal/shared/cos_config.h>
#include "chal_asm_inc.h"

.set MULTIBOOT_PAGE_ALIGN,  1<<0
.set MULTIBOOT_MEMINFO,     1<<1
.set MULTIBOOT_AOUT_KLUDGE, 1<<16
.set MULTIBOOT_MAGIC,       0x1BADB002
.set MULTIBOOT_FLAGS,       MULTIBOOT_PAGE_ALIGN | MULTIBOOT_MEMINFO
.set MULTIBOOT_CHECKSUM,    -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

.section .multiboot
.align 4
.long MULTIBOOT_MAGIC
.long MULTIBOOT_FLAGS
.long MULTIBOOT_CHECKSUM

.set STACKSIZE, 0x1000
.comm stack, (STACKSIZE * NUM_CPU), 0x1000

.globl loader
loader:
	/* X86_64-FIXME: commented temparaily */
	/* mov 	$(stack + STACKSIZE - STK_INFO_OFF), %esp */
	movl	%eax, %ecx
	/* X86_64-FIXME: movl %cr4, %eax -> movq %cr4, %rax */
	movq    %cr4, %rax
	orl     $((1<<4) | (1<<7)), %eax
	/* X86_64-FIXME: movl %eax, %cr4 -> movq  %rax, %cr4 */
	movq    %rax, %cr4
	/* X86_64-FIXME: movl $(boot_comp_pgd-COS_MEM_KERN_START_VA), %eax -> movq  $(boot_comp_pgd-COS_MEM_KERN_START_VA), %rax*/
	movq    $(boot_comp_pgd-COS_MEM_KERN_START_VA), %rax
	/* X86_64-FIXME: movl %eax, %cr3 -> movq  %rax, %cr3 */
	movq    %rax, %cr3

	# Turn on paging.
	/* X86_64-FIXME: movl %cr0, %eax -> movq  %cr0, %rax */
	movq    %cr0, %rax
	orl     $(1<<31), %eax
	/* X86_64-FIXME: movl %eax, %cr0 ->  movq %rax, %cr0 */
	movq    %rax, %cr0
	cli
	/* X86_64-FIXME: pushl %exx -> pushq %rxx*/
	pushq %rsp
	pushq %rcx
	pushq %rbx
	/* X86_64-FIXME: pushl $0 -> pushq %0 */ 
	pushq $0  /* empty return value as we're jmping, not calling */
	/* X86_64-FIXME: commented temparaily */
	/* mov $kmain, %eax */
	/* X86_64-FIXME: jmp *%eax -> jmp *%rax */ 
	jmp *%rax

.text

/***
The following SMP boot code is from xv6, thus:

Copyright (c) 2006-2009 Frans Kaashoek, Robert Morris, Russ Cox,
                        Massachusetts Institute of Technology

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
***/

#define SEG_NULLASM                                             \
        .word 0, 0;                                             \
        .byte 0, 0, 0, 0

// The 0xC0 means the limit is in 4096-byte units
// and (for executable segments) 32-bit mode.
#define SEG_ASM(type,base,lim)                                  \
        .word (((lim) >> 12) & 0xffff), ((base) & 0xffff);      \
        .byte (((base) >> 16) & 0xff), (0x90 | (type)),         \
                (0xC0 | (((lim) >> 28) & 0xf)), (((base) >> 24) & 0xff)

#define STA_X     0x8       // Executable segment
#define STA_E     0x4       // Expand down (non-executable segments)
#define STA_C     0x4       // Conforming code segment (executable only)
#define STA_W     0x2       // Writeable (non-executable segments)
#define STA_R     0x2       // Readable (executable segments)
#define STA_A     0x1       // Accessed

#define CR0_PE          0x00000001      // Protection Enable
#define CR0_WP          0x00010000      // Write Protect
#define CR0_PG          0x80000000      // Paging

/*
 * All of this code is compiled as part of the kernel, but will be
 * dynamically copied into SMP_BOOT_PATCH_ADDR.  Given that the code
 * is linked to another set of addresses (in the kernel), we need
 * to manually relocate many of the addresses here.  Thus, these
 * macros.
 */
#define RELOCATE_ADDR(a)    a - smppatchstart + SMP_BOOT_PATCH_ADDR
/* when the code segment is = SMP_BOOT_PATCH_ADDR, use this: */
#define RELOCATE_ADDR_CS(a) a - smppatchstart

.align 4096
.code16
.globl smppatchstart
smppatchstart:
.globl smploader
smploader:
	cli

	movw    %cs, %ax
	movw    %ax, %ds
	lgdt    RELOCATE_ADDR_CS(smpgdtdesc) /* using cs as ds: idea taken from Quest */
	xorw    %ax, %ax
	movw    %ax, %ds

	movl    %cr0, %eax
	orl     $CR0_PE, %eax
	movl    %eax, %cr0

	/* ljmpl    $(SEL_KCSEG), $RELOCATE_ADDR(smpstart32) */

.align 0x10
.code32
smpstart32:
	movw    $(SEL_KDSEG), %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    $0, %ax
	movw    %ax, %fs
	movw    %ax, %gs

	/* Turn on page size extension for 4Mbyte pages */
	movl    %cr4, %eax
	orl     $((1<<4) | (1<<7)), %eax
	movl    %eax, %cr4

	movl    $(boot_ap_pgd - COS_MEM_KERN_START_VA), %eax
	movl    %eax, %cr3

	/* Turn on paging. */
	movl    %cr0, %eax
	orl     $(CR0_PE|CR0_PG|CR0_WP), %eax
	movl    %eax, %cr0

	/* Switch to the stack allocated by startothers() */
	movl    $RELOCATE_ADDR(smpstack), %esp
	movl    (%esp), %esp
	/* X86_64-FIXME: commented temparaily */
	/* pushl   $spin */
	/* X86_64-FIXME: commented temparaily */
	/* mov     $smp_kmain, %eax */
	jmp     *%eax
spin:
	jmp     spin

# setup an initial gdt that will be overridden in C code
.p2align 2
smpgdt:
  SEG_NULLASM
  SEG_ASM(STA_X|STA_R, 0, 0xffffffff)
  SEG_ASM(STA_W, 0, 0xffffffff)

smpgdtdesc:
  .word   (smpgdtdesc - smpgdt - 1)
  .long   RELOCATE_ADDR(smpgdt)

.global smpstack
smpstack:
.long 0xDEADBEEF

.globl smppatchend
smppatchend:
	nop

/*
http://wiki.osdev.org/Shutdown

http://stackoverflow.com/questions/21463908/x86-instructions-to-power-off-computer-in-real-mode
http://stackoverflow.com/questions/678458/shutdown-the-computer-using-assembly
http://stackoverflow.com/questions/3145569/how-to-power-down-the-computer-from-a-freestanding-environment
*/

.code16
.globl shutdown_apm
shutdown_apm:
mov $0x5301, %ax
xor %bx, %bx
int $0x15

/* Try to set apm version (to 1.2). */
mov $0x530e, %ax
xor %bx, %bx
mov $0x0102, %cx
int $0x15

/* Turn off the system. */
mov $0x5307, %ax
mov $0x0001, %bx
mov $0x0003, %cx
int $0x15

.org 510
.word 0xaa55
