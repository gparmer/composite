.text;					
.globl __cosrt_upcall_entry;		
.type __cosrt_upcall_entry, @function;	
.align 16;				
__cosrt_upcall_entry:			
    mov $0, %rax
    mov $1, %rbx
    jmp .                   
