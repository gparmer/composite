BUILDDIR := ../../build/libsupc++
INCLUDE := -I../../include
MUSLFLAG := -isystem ../../../libc/musl-1.2.0/include
include ../Makefile.inc

EXCEPTION := eh_alloc.o eh_arm.o eh_aux_runtime.o eh_call.o eh_catch.o eh_exception.o eh_throw.o \
eh_personality.o eh_term_handler.o eh_terminate.o eh_type.o eh_unex_handler.o eh_globals.o eh_ptr.o
OBJ_CC := del_op.o del_opnt.o del_opv.o del_opvnt.o new_handler.o new_op.o guard.o nested_exception.o \
new_opnt.o new_opv.o new_opvnt.o tinfo2.o tinfo.o vterminate.o demangle.o vec.o pure.o cxx_atexit.o fake_unwind.o
OBJ_CC += $(EXCEPTION)

OBJ_CC_GCC48 := array_type_info.o atexit_arm.o bad_alloc.o bad_cast.o bad_typeid.o class_type_info.o \
dyncast.o enum_type_info.o function_type_info.o fundamental_type_info.o pbase_type_info.o \
pmem_type_info.o pointer_type_info.o si_class_type_info.o vmi_class_type_info.o guard_error.o hash_bytes.o

.PHONY: all clean

all: libsupc++.a

clean:
	rm -rf $(BUILDDIR) ../../libsupc++.a *.o

$(OBJ_CC) $(OBJ_CC_GCC48):%.o:%.cc
	@$(CXX) $< -o $@ -c $(CXXFLAGS)

libsupc++.a:$(OBJ_CC) $(OBJ_CC_GCC48)
	@$(AR) rcs $@ $^
	@mkdir -p $(BUILDDIR)
	@$(MV) *.o $(BUILDDIR)
	@$(MV) $@ ../../
