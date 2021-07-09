include Makefile.src

CC := gcc -m32
LD := ld -m elf_i386
AS := as --32 -g

INCPATH := ../../kernel/include
INCS    := -I$(INCPATH) -I$(INCPATH)/shared/
# This variable gets rid of the all the warnings on Ubuntu 20, but we should look into each...
TMPFLGS := -Wno-address-of-packed-member
CFLAGS  := -g3 -O3 -ffreestanding -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable $(TMPFLGS) -nostdinc -nostdlib -fno-pic -mno-red-zone $(INCS)
LDFLAGS := -nostdlib -fno-builtin -nostartfiles -nostdinc -nodefaultlibs

KERNEL := kernel.img
IMAGE  := cos.img

WARNINGS += -Wall
WARNINGS += -Wcast-align
WARNINGS += -Wformat=2
WARNINGS += -Winit-self
#WARNINGS += -Wmissing-declarations
#WARNINGS += -Wmissing-prototypes
WARNINGS += -Wnested-externs
WARNINGS += -Wno-system-headers
WARNINGS += -Wold-style-definition
WARNINGS += -Wredundant-decls
WARNINGS += -Wsign-compare
WARNINGS += -Wstrict-prototypes
WARNINGS += -Wundef
WARNINGS += -Wvolatile-register-var
WARNINGS += -Wwrite-strings

CFLAGS += $(WARNINGS)

OBJS += kernel.o
OBJS += gdt.o
OBJS += idt.o
OBJS += vm.o
OBJS += printk.o
OBJS += string.o
OBJS += vtxprintf.o
OBJS += tss.o
OBJS += user.o
OBJS += serial.o
OBJS += hpet.o
OBJS += chal.o
OBJS += boot_comp.o
OBJS += miniacpi.o
#OBJS += console.o
OBJS += vga.o
OBJS += exception.o
OBJS += lapic.o

COS_OBJ += pgtbl.o
COS_OBJ += retype_tbl.o
COS_OBJ += liveness_tbl.o
COS_OBJ += tcap.o
COS_OBJ += capinv.o
COS_OBJ += captbl.o

DEPS :=$(patsubst %.o, %.d, $(OBJS))

OBJS += $(COS_OBJ)

all: $(KERNEL)

$(KERNEL): linker.ld $(DEPS) $(OBJS) loader.o
	$(if $(CONSTRUCTOR_COMP), $(info |     [LD]   Linking in the constructor component $@))
	$(if $(CONSTRUCTOR_COMP), @cp $(CONSTRUCTOR_COMP) constructor; $(LD) -r -b binary constructor -o constructor.o; objcopy --rename-section .data=.initial_component constructor.o; rm constructor)
	$(if $(KERNEL_OUTPUT), $(info |     [LD]   Linking $@))
	$(if $(KERNEL_OUTPUT), $(LD) -T linker.ld loader.o entry.o $(OBJS) $(if $(CONSTRUCTOR_COMP), constructor.o) -o $@)
	$(if $(KERNEL_OUTPUT), $(info |     [CP]   Copying $@ into $(KERNEL_OUTPUT)))
	$(if $(KERNEL_OUTPUT), @cp $@ $(KERNEL_OUTPUT))

loader.o: loader.S entry.S
	$(info |     [AS]   Assembling $@)
	@$(CC) -c -I$(INCPATH) entry.S
	@$(CC) -c -I$(INCPATH) loader.S

%.d: %.c
	@$(CC) -M -MT $(patsubst %.d, %.o, $@) $(CFLAGS) $(LDFLAGS) $< -o $@

pgtbl.o: ../../kernel/pgtbl.c
	$(info |     [CC]   Compiling $@)
	@$(CC) $(CFLAGS) -c $< -o $@

tcap.o: ../../kernel/tcap.c
	$(info |     [CC]   Compiling $@)
	@$(CC) $(CFLAGS) -c $< -o $@

retype_tbl.o: ../../kernel/retype_tbl.c
	$(info |     [CC]   Compiling $@)
	@$(CC) $(CFLAGS) -c $< -o $@

liveness_tbl.o: ../../kernel/liveness_tbl.c
	$(info |     [CC]   Compiling $@)
	@$(CC) $(CFLAGS) -c $< -o $@

capinv.o: ../../kernel/capinv.c
	$(info |     [CC]   Compiling $@)
	@$(CC) $(CFLAGS) -c $< -o $@

captbl.o: ../../kernel/captbl.c
	$(info |     [CC]   Compiling $@)
	@$(CC) $(CFLAGS) -c $< -o $@


%.o: %.c
	$(info |     [CC]   Compiling $@)
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@rm -f *.d *.o $(KERNEL)
