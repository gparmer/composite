#include "chal/shared/cos_config.h"
#include "chal_asm_inc.h"
#include "multiboot2.h"

/* multiboot2 header must be contained completely within the first 32768 bytes of the OS image, and must be 64-bit aligned */
.section .text
multiboot_start:

    /* must have multiboot2 header field */
    .long MULTIBOOT2_HEADER_MAGIC
    .long MULTIBOOT_ARCHITECTURE_I386
    .long multiboot_end - multiboot_start
    .long -(MULTIBOOT2_HEADER_MAGIC + MULTIBOOT_ARCHITECTURE_I386 + (multiboot_end - multiboot_start))

    /* end tag of multiboot2 */
    .short MULTIBOOT_HEADER_TAG_END
    .short 0
    .long 8

multiboot_end:

.set STACKSIZE, 0x1000
.comm stack, (STACKSIZE * NUM_CPU) , 0x1000

/* 
 * grub enters 32-bit mode, so your first section should use 32-bit code.
 * jmp to 64 bit yourself from here.
 */
.code32
.global entry_point
entry_point:
    /* save multiboot2 args first */
    mov %eax, Multiboot_magic
    mov %ebx, Multiboot_tags

    /* now enable SSE */
    mov %cr0, %eax
    and $0xFFFB, %ax	
    or $0x2, %ax	
    mov %eax, %cr0
    mov %cr4, %eax
    or $(3 << 9), %ax	
    mov %eax, %cr4
    
    /* repare to enter long(64-bit) mode */

    /* close paging first */
    mov %cr0, %eax                                   
    and $0b1111111111111111111111111111111, %eax   
    mov %eax, %cr0                                

    /* test if 1G huge page is supported */
    /* some cpus don't support 1G huge page, we enable this by set qemu -cpu max */
    mov $0x80000001, %eax
    cpuid
    bt $26, %edx
    jc huge_page_enabled
    /* TODO: support for cpus which don't have 1G paging scheme here */
    jmp .

huge_page_enabled:
    /* we have to set up a temparaily page scheme to before enter long mode */
    /* we use 1G paging scheme, so only to set P4_table, P3_table */
    mov $P3_table, %eax
    or $0x01, %eax
    mov %eax, P4_table
    mov %eax, (P4_table+0x08*256)
    
    /* set kernel paging space */
    movl  $0x00000083, P3_table /* 0-1 GB vm -> 0-1 GB pm */

    /* set %cr3 to the physical address of P4_table */
    mov $P4_table, %eax
    mov %eax, %cr3

    /* enable PAE-paging by setting the PAE-bit in %cr4 */
    mov %cr4, %eax
    or $(1 << 5), %eax
    mov %eax, %cr4

    /* set the long mode bit in the EFER MSR */
    mov $0xC0000080, %ecx
    rdmsr
    or $(1 << 8), %eax
    wrmsr

    /* enable paging and protected mode in cr0 */
    mov %cr0, %eax
    or $(1 << 31), %eax
    mov %eax, %cr0

    /* load GDT so we can use selectors within them */
    mov $Gdt_ptr, %eax
    lgdt (%eax)

    /* need a jump to flush cs selector */
.code64
    mov $entry_64, %rax
    pushq $0x08
    pushq %rax
    lretq


entry_64:
    
    cli
    /* Flush data selectors */
    mov $0x10, %rax	
    mov %rax, %ds
    mov %rax, %es
    mov %rax, %fs
    mov %rax, %gs
    mov %rax, %ss

    /* set up a kernel stack for kmain */
    movabs $(stack + STACKSIZE - STK_INFO_OFF), %rsp
    xor %rsi, %rsi
    xor %rdi, %rdi
    movl Multiboot_magic, %esi
    movl Multiboot_tags, %edi
    movabs $COS_MEM_KERN_START_VA, %rax
    add %rax, %rdi

    movabs $kmain, %rax
    pushq $0x08
    pushq %rax
    lretq
    jmp .

.align 8
Multiboot_magic:
    .fill 1, 4, 0
Multiboot_tags:
    .fill 1, 4, 0

/* a simple temparaily GDT */
.align 8
.global Gdt_table
Gdt_table:
    .quad	0x0000000000000000	// null descriptor
    .quad	0x0020980000000000	// kernel code descriptor
    .quad 	0x0000920000000000	// kernel data descriptor
Gdt_end:

Gdt_ptr:	
    .word	Gdt_end - Gdt_table - 1
    .quad	Gdt_table

/* temparaily page directory/tables */
/* need to be 4k aligned */
.align 4096

.global P4_table
P4_table:
    .fill 512, 8, 0

.global P3_table
P3_table:
    .fill 512, 8, 0