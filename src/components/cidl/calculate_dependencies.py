#!/usr/bin/python2

import re
import sys
import os
import copy
import string

def error_out(message):
    sys.stderr.write("Error: " + sys.argv[0] + " " + message)
    sys.exit(-1);

if len(sys.argv) != 4:
    error_out("<sw_dir> <cos base> libdeps|ifdeps|libpaths|objpaths|incpaths|shallowlibdeps|shallowifdeps|shallowifexp: \n\tThe first argument must be the path to the directory of the component|interface|library. The second should be the base of the composite repo's component directory (src/components/). Both must end in a /.\n")

target_path = sys.argv[1]
# path to the composite repo
base_path   = sys.argv[2]
if base_path[-1] != '/' or target_path[-1] != '/':
    error_out(target_path + " " + base_path + " libdeps|ifdeps|libpaths|objpaths|incpaths|shallowlibdeps|shallowifdeps|shallowifexp: \n\tThe first argument must be the path to the directory of the component|interface|library. The second should be the base of the composite repo's component directory (src/components/). Both must end in a /.\n")

libs_path = base_path + "lib/"
ifs_path  = base_path + "interface/"

lib_str       = "LIBRARY_DEPENDENCIES"   # XXX where src/component/lib/XXX
if_dep_str    = "INTERFACE_DEPENDENCIES" # XXX where src/component/interface/XXX
if_exp_str    = "INTERFACE_EXPORTS"      # XXX where src/component/interface/XXX
lib_out_str   = "LIBRARY_OUTPUT"         # XXX where libXXX.a
bin_out_str   = "OBJECT_OUTPUT"          # XXX where XXX.lib.o
inc_paths_str = "INCLUDE_PATHS"          # all include paths relative to the library

# calculate the dependencies from this Makefile
def makefile_deps(path):
    try:
        mf = open(path+'/Makefile', 'r')
    except:
        return (False, [], [], [], [], [], [], [])

    lines    = mf.readlines()
    libdeps  = []
    ifdeps   = []
    ifexps   = []

    libout   = []
    libpaths = []
    objout   = []
    incpaths = []

    for line in lines:
        line           = string.rstrip(line)
        comment_filter = re.split("#", line) # filter out comments
        data           = re.split("=", comment_filter[0])

        if len(data) != 2:
            continue
        attr = data[0].strip()
        lst  = filter(lambda x: x != "", re.split(" ", data[1].strip()))
        if attr == lib_str:
            libdeps = lst
        if attr == if_dep_str:
            ifdeps += lst
        if attr == if_exp_str:
            ifexps += lst
        if attr == lib_out_str:
            libout   = map(lambda x: "-l" + str(x), lst)
            libpaths = map(lambda x: "-L" + str(path), lst)
        if attr == bin_out_str:
            objout   = map(lambda x: str(path) + "/" + str(x) + ".lib.o", lst)
        if attr == inc_paths_str:
            incpaths = map(lambda x: "-I" + str(path) + "/" + str(x), lst)

    return (True, libdeps, ifdeps, ifexps, libout, libpaths, objout, incpaths)

def makefile_dependencies(path):
    (success, libdeps, ifdeps, ifexps, libout, libpaths, objout, incpaths) = makefile_deps(path)
    return (success, libdeps, ifdeps, ifexps)

def makefile_paths(path):
    (success, libdeps, ifdeps, ifexps, libout, libpaths, objout, incpaths) = makefile_deps(path)
    return (success, libout, libpaths, objout, incpaths)

def unique(l):
    return list(set(l))

# filter out the redundant entries in the new dependencies WRT the
# original
def new_deps(orig, new):
    expanded = []
    uniq     = unique(new)
    for d in uniq:
        if d not in orig:
            expanded.append(d)
    return expanded

def resolve_deps(current, libdeps, ifdeps, expifs):
    alllibdeps = []
    allifdeps  = []
    allifexps  = []

    # Calculate the list of new dependencies from the previous set
    def expand_deps(path, deps, depname):
        libdeps_unchecked = []
        ifdeps_unchecked  = []

        for l in deps:
            (success, ls, ifs, exps) = makefile_dependencies(path + l)
            if not success:
                error_out("\n"+
                          "\tCould not find Makefile for dependency "+l+" from "+current+".\n"+
                          "\tMost likely your "+depname+" includes an incorrect entry: "+l+".\n"+
                          "\tIgnoring "+l+" entry.\n")

            nlibs = new_deps(libdeps_unchecked, new_deps(alllibdeps, ls))
            if len(nlibs) > 0:
                libdeps_unchecked += nlibs
            nifs  = new_deps(ifdeps_unchecked, new_deps(allifdeps, ifs))
            if len(nifs) > 0:
                ifdeps_unchecked  += nifs
        return (libdeps_unchecked, ifdeps_unchecked)

    libdeps_unchecked = copy.copy(libdeps)
    ifdeps_unchecked  = copy.copy(ifdeps)
    ifexps_unchecked  = copy.copy(expifs)

    while len(libdeps_unchecked) != 0 or len(ifdeps_unchecked) != 0 or len(ifexps_unchecked) != 0:
        (newlibdeps1, newifdeps1) = expand_deps(libs_path, libdeps_unchecked, lib_str)
        (newlibdeps2, newifdeps2) = expand_deps(ifs_path,  ifdeps_unchecked,  if_dep_str)
        (x,           newifdeps3) = expand_deps(ifs_path,  ifexps_unchecked,  if_exp_str)

        alllibdeps += libdeps_unchecked
        allifdeps  += ifdeps_unchecked
        allifexps  += ifexps_unchecked
        libdeps_unchecked = unique(newlibdeps1 + newlibdeps2)
        ifdeps_unchecked  = unique(newifdeps1 + newifdeps2)
        ifexps_unchecked  = unique(newifdeps3)

    return (alllibdeps, allifdeps, allifexps)

def gen_paths(libdeps, ifdeps, expifs):
    libincs  = []
    libpaths = []
    binpaths = []
    incpaths = []

    def gather_paths(path, li, lp, bp, ip):
        (success, l_libincs, l_libpaths, l_binpaths, l_incpaths) = makefile_paths(path)
        if not success:
            error_out("INTERNAL ERROR:\n\tCould not find Makefile in directory "+ path + ".\n")
        li += l_libincs
        lp += l_libpaths
        bp += l_binpaths
        ip += l_incpaths

    for l in libdeps:
        gather_paths(libs_path + l, libincs, libpaths, binpaths, incpaths);
    for i in ifdeps:
        gather_paths(ifs_path  + i, libincs, libpaths, binpaths, incpaths);
    # exports should only be -I included, and we should not use their libraries or binaries
    for e in expifs:
        gather_paths(ifs_path  + e, [], [], [], incpaths);

    return (unique(libincs), unique(libpaths), unique(binpaths), unique(incpaths))

# The current directory should have the Makefile of the build unit
(success, shallow_ls, shallow_ifs, shallow_exps) = makefile_dependencies(target_path)
if not success:
    error_out("Could not find Makefile in " + target_path + ").\n")

(libs, ifs, exps) = resolve_deps(target_path, shallow_ls, shallow_ifs, shallow_exps)

(libincs, libpaths, binpaths, incpaths) = gen_paths(libs, ifs, exps)
select_cmd = {
    "libdeps":libs,
    "ifdeps":ifs,
    "libinc":libincs,
    "libpaths":libpaths,
    "objpaths":binpaths,
    "incpaths":incpaths,
    "shallowlibdeps":shallow_ls,
    "shallowifdeps":shallow_ifs,
    "shallowifexps":shallow_exps,
    }
try:
    out = select_cmd.get(sys.argv[3])
    if None == out:
        out = []
except:
    error_out(sys.argv[1] + " " + sys.argv[2] +" libdeps|ifdeps|libinc|libpaths|objpaths|incpaths|shallowlibdeps|shallowifdeps|shallowifexp: \n\tsecond argument ("+sys.argv[2]+") not one of the two options.\n")

output = ""
for o in out:
    output += " " + str(o).strip()
print output.strip()
