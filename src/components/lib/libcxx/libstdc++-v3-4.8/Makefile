BUILDDIR := ../build/libstdc++
INCLUDE := -I../include
MUSLFLAG := -isystem ../../libc/musl-1.2.0/include
include Makefile.inc

EXCEPTION :=
OBJ_CC := basic_file_stdio.o codecvt_members.o collate_members.o ctype_configure_char.o ctype_members.o c_locale.o messages_members.o \
monetary_members.o numeric_members.o time_members.o

OBJ_CC += c++98/allocator-inst.o c++98/bitmap_allocator.o c++98/compatibility.o c++98/codecvt.o c++98/ctype.o c++98/globals_io.o c++98/ios.o \
c++98/ios_init.o c++98/ios-inst.o c++98/ios_locale.o c++98/iostream-inst.o c++98/istream.o c++98/istream-inst.o c++98/list.o c++98/locale.o \
c++98/locale_facets.o c++98/locale_init.o c++98/locale-inst.o c++98/localename.o c++98/misc-inst.o c++98/ostream-inst.o c++98/sstream-inst.o \
c++98/streambuf.o c++98/streambuf-inst.o c++98/tree.o

OBJ_CC += c++11/fstream-inst.o c++11/functexcept.o c++11/string-inst.o
#OBJ_CC += $(EXCEPTION)

vpath %.cc ./src
vpath %.cc ./config/io
vpath %.cc ./config/locale/generic
vpath %.cc ./config/os/generic

.PHONY: all clean

all: dir libstdc++.a

clean:
	rm -rf $(BUILDDIR) ../libstdc++.a c++11 c++98 *.o

$(OBJ_CC):%.o:%.cc
	@$(CXX) $< -o $@ -c $(CXXFLAGS)
dir:
	@mkdir -p c++11
	@mkdir -p c++98
	@mkdir -p $(BUILDDIR)

libstdc++.a: $(OBJ_CC)
	@$(AR) rcs $@ $^
	@$(MV) *.o $(BUILDDIR)
	@rm -rf $(BUILDDIR)/c++11
	@$(MV) -f c++11/ $(BUILDDIR)
	@rm -rf $(BUILDDIR)/c++98
	@$(MV) -f c++98/ $(BUILDDIR)
	@$(MV) $@ ../
