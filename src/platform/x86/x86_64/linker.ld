/*
 * TODO: integrate into the build system to avoid redefining the
 * constants using "gcc -P -E ldscript.c -o ldscript.ld"
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY (entry_point)
phys = 0x00100000;  		/* This should be COS_MEM_KERN_PA */
SECTIONS {

    . = phys;

    .boottext : AT(phys) ALIGN(8)
    {
       start.o(.text)
    }

    boot_end = .;

    . = 0xffff800000000000 + boot_end;		/* This should be COS_MEM_KERN_START_VA + COS_MEM_KERN_PA */

    .text : AT(boot_end) {
        code = .;
        *(.text)
        *(.rodata)
        . = ALIGN(4096);
        *(__ipc_entry)
        . = ALIGN(4096);
    }

    .data : AT(boot_end + (data - code)) {
        data = .;
        *(.data)
        . = ALIGN(4096);
    }

    .bss : AT(boot_end + (bss - code)) {
        bss = .;
        *(.bss)
	. = ALIGN(4096);
    }

    /*
     * We allocate all large allocations after the normal .bss and .data so that data
     * that is needed in the intial kernel image (referred to by a single 4MB super-page)
     */
    .largebss : AT(boot_end + (largebss - code)) {
    	largebss = .;
        *(.largebss)
	. = ALIGN(4096);
    }
    end = .;		/* Unfortunately, this variable will take up the whole page. */
    . = ALIGN(4096);

    .initial_component : AT(boot_end + (initial_component - code)) {
        initial_component = .;
	*(.initial_component)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.gnu.build-id)
    }
}
