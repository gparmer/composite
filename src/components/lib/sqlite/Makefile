# Required variables used to drive the compilation process. It is OK
# for many of these to be empty.
#
# The library names associated with .a files output that are linked
# (via, for example, -lsqlite) into dependents. This list should be
# "sqlite" for output files such as libsqlite.a.
LIBRARY_OUTPUT = sqlite3
# The .o files that are mandatorily linked into dependents. This is
# rarely used, and only when normal .a linking rules will avoid
# linking some necessary objects. This list is of names (for example,
# sqlite) which will generate sqlite.lib.o. Do NOT include the list of .o
# files here. Please note that using this list is *very rare* and
# should only be used when the .a support above is not appropriate.
OBJECT_OUTPUT = 
# The path within this directory that holds the .h files for
# dependents to compile with (./ by default). Will be fed into the -I
# compiler arguments.
INCLUDE_PATHS = sqlite/
# The interfaces this component is dependent on for compilation (this
# is a list of directory names in interface/)
INTERFACE_DEPENDENCIES =
# The library dependencies this component is reliant on for
# compilation/linking (this is a list of directory names in lib/)
LIBRARY_DEPENDENCIES = component posix libc
# Note: Both the interface and library dependencies should be
# *minimal*. That is to say that removing a dependency should cause
# the build to fail. The build system does not validate this
# minimality; that's on you!

# There are two different *types* of Makefiles for libraries.
# 1. Those that are Composite-specific, and simply need an easy way to
#    compile and itegrate their code.
# 2. Those that aim to integrate external libraries into
#    Composite. These focus on "driving" the build process of the
#    external library, then pulling out the resulting files and
#    directories. These need to be flexible as all libraries are
#    different.

# Type 1, Composite library: This is the default Makefile for
# libraries written for composite. Get rid of this if you require a
# custom Makefile (e.g. if you use an existing
# (non-composite-specific) library. An example of this is `kernel`.
# include Makefile.lib

## Type 2, external library: If you need to specialize the Makefile
## for an external library, you can add the external code as a
## subdirectory, and drive its compilation, and integration with the
## system using a specialized Makefile. The Makefile must generate
## lib$(LIBRARY_OUTPUT).a and $(OBJECT_OUTPUT).lib.o, and have all of
## the necessary include paths in $(INCLUDE_PATHS).
##
## To access the Composite Makefile definitions, use the following. An
## example of a Makefile written in this way is in `ps/`.
#
# include Makefile.src Makefile.comp Makefile.dependencies
# .PHONY: all clean init distclean
## Fill these out with your implementation
# all:
# clean:
#
## Default rules:
# init: clean all
# distclean: clean

include Makefile.src Makefile.comp Makefile.dependencies

.PHONY: all clean init distclean config clean_lib
CINC_ENV = $(CINC) $(DEPENDENCY_INCPATH)
CFLAGS   = $(CINC_ENV)
export CINC_ENV
export CFLAGS

all:
	$(info Building sqlite library...)
	make -s -C sqlite libsqlite3.a
	@cp sqlite/libsqlite3.a .

clean_lib:
	make -s -C sqlite clean

clean: clean_lib config

config:
	$(info Configuring the sqlite library)
	make -s -C sqlite sqlite3.h

init: clean config
distclean: clean
