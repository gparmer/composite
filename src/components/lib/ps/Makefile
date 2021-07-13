# Required variables used to drive the compilation process. It is OK
# for many of these to be empty.
#
# The library names associated with .a files output that are linked
# (via, for example, -lps) into dependents. This list should be
# "ps" for output files such as libps.a.
LIBRARY_OUTPUT = ps
# The .o files that are mandatorily linked into dependents. This is
# rarely used, and only when normal .a linking rules will avoid
# linking some necessary objects. This list is of names (for example,
# ps) which will generate ps.lib.o. Do NOT include the list of .o
# files here. Please note that using this list is *very rare* and
# should only be used when the .a support above is not appropriate.
OBJECT_OUTPUT =
# The path within this directory that holds the .h files for
# dependents to compile with (./ by default). Will be fed into the -I
# compiler arguments.
INCLUDE_PATHS = ps/
# The interfaces this component is dependent on for compilation (this
# is a list of directory names in interface/)
INTERFACE_DEPENDENCIES =
# The library dependencies this component is reliant on for
# compilation/linking (this is a list of directory names in lib/)
LIBRARY_DEPENDENCIES = component
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
## include Makefile.lib

## Type 2, external library: If you need to specialize the Makefile
## for an external library, you can add the external code as a
## subdirectory, and drive its compilation, and integration with the
## system using a specialized Makefile. The Makefile must generate
## lib$(LIBRARY_OUTPUT).a and $(OBJECT_OUTPUT).lib.o, and have all of
## the necessary include paths in $(INCLUDE_PATHS).
##
## To access the Composite Makefile definitions, use the following. An
## example of a Makefile written in this way is in `ps/`.

include Makefile.src Makefile.comp Makefile.dependencies

.PHONY: all clean init distclean config clean_lib armv7a_config i386_config
CINC_ENV = $(CINC) $(DEPENDENCY_INCPATH)
CFLAGS   = $(CINC_ENV)
export CINC_ENV
export CFLAGS

all: config
	$(info Building ps library...)
	make -s -C ps all
	@cp ps/libps.a .

clean_lib:
	@rm -f libps.a
	-make -C ps clean

# need to make sure the ps_plat.h is always available
clean: clean_lib config

armv7a_config:
	$(info Configuring the ps library)
	@cd ps; ./configure cos armv7a general; cd ..

i386_config:
	$(info Configuring the ps library)
	@cd ps; ./configure cos x86 general; cd ..

config: $(PLATFORM)_config
	make -s -C ps config

init: clean config
distclean: clean
