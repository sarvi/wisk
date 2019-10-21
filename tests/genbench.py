#!/usr/bin/env python
# encoding: utf-8

import sys
import os.path
from random import Random
random = Random(0) # initialise with seed to have reproductible benches

HELP_USAGE = """Usage: generate_libs.py root libs classes internal external.
    root     - Root directory where to create libs.
    libs     - Number of libraries (libraries only depend on those with smaller numbers)
    classes  - Number of classes per library
    internal - Number of includes per file referring to that same library
    external - Number of includes per file pointing to other libraries

For example:
./genbench.py /tmp/build 200 100 15 5

To try the waf part, do:
waf configure build -p -j5

To test the autotools part, do:
touch README AUTHORS NEWS ChangeLog &&
autoreconf --install --symlink --verbose &&
mkdir autotools-build-dir &&
cd autotools-build-dir &&
../configure --disable-shared CXXFLAGS=-Wall &&
time make -j4 --silent &&
time make -j4 --silent
"""

def lib_name(i):
    return "lib_" + str(i)

def createHeader(name):
    filename = name + ".h"
    handle = open(filename, "w" )

    guard = name + '_h_'
    handle.write ('#ifndef ' + guard + '\n');
    handle.write ('#define ' + guard + '\n\n');

    handle.write ('class ' + name + ' {\n');
    handle.write ('public:\n');
    handle.write ('    ' + name + '();\n');
    handle.write ('    ~' + name + '();\n');
    handle.write ('};\n\n');

    handle.write ('#endif\n');


def createLibCPP(name, lib_number, classes_per_lib, internal_includes, external_includes):
    filename = name + ".cpp"
    handle = open(filename, "w" )

    header= name + ".h"
    handle.write ('#include "' + header + '"\n');

    includes = random.sample(range(classes_per_lib), internal_includes)
    for i in includes:
        handle.write ('#include "class_' + str(i) + '.h"\n')

    dep_list=[]
    if (lib_number > 0):
        handle.write ('// External Includes \n')
        includes = random.sample(range(classes_per_lib), external_includes)
        lib_list = range(lib_number)
        for i in includes:
            deplibnumber = random.choice(lib_list)
            libname = 'lib_' + str(deplibnumber)
            handle.write ('#include <' + libname + '/' + 'class_' + str(i) + '.h>\n')
            dep_list.append(deplibnumber)

    handle.write ('\n');
    handle.write (name + '::' + name + '() {}\n');
    handle.write (name + '::~' + name  + '() {}\n');
    return list(set(dep_list))


def createProgCPP(name, lib_number, classes_per_lib, internal_includes, external_includes):
    filename = name + ".cpp"
    handle = open(filename, "w" )
    handle.write ('#include <iostream>\n')

    handle.write ('// External Includes \n')
    dep_list=[]
    includes = random.sample(range(classes_per_lib), external_includes)
    lib_list = range(lib_number+1)
    for i in includes:
        deplibnumber = random.choice(lib_list)
        libname = 'lib_' + str(deplibnumber)
        handle.write ('#include <' + libname + '/' + 'class_' + str(i) + '.h>\n')
        dep_list.append(deplibnumber)

    handle.write ('\n');
    lib_invokes = ''.join([('class_' + str(i) + '();\n') for i in includes])
    handle.write ('''
int main()
{
std::cout<<"This is main %s_%d function\\n";
%s
}
        ''' % (name, lib_number, lib_invokes));
    return list(set(dep_list))


def createSConscript(lib_number, classes):
    handle = open("SConscript", "w");
    handle.write("Import('env')\n")
    handle.write('list = Split("""\n');
    for i in range(classes):
        handle.write('    class_' + str(i) + '.cpp\n')
    handle.write('    """)\n\n')
    handle.write('env.StaticLibrary("lib_' + str(lib_number) + '", list)\n\n')

def createLibCMakeLists(lib_number, classes, includes):
    handle = open("CMakeLists.txt", "w")
    handle.write("""add_library(lib_%s STATIC %s)\n""" % (str(lib_number), ' '.join(('class_%s' % str(i) for i in range(classes)))))
    handle.write("""add_library(libso_%s SHARED %s)\n""" % (str(lib_number), ' '.join(('class_%s' % str(i) for i in range(classes)))))
    for j in includes:
        handle.write("""target_link_libraries(libso_%s libso_%s)\n""" % (str(lib_number), str(j)))

def createProgCMakeLists(lib_number, classes, includes):
    handle = open("CMakeLists.txt", "w")
    for i in range(classes):
        handle.write("""add_executable(prog_%s_class_%s class_%s.cpp)\n""" % (str(lib_number), str(i), str(i)))
        handle.write("""target_link_libraries(prog_%s_class_%s libso_%s)\n""" % (str(lib_number), str(i), str(i)))
        for j in includes:
            handle.write("""target_link_libraries(prog_%s_class_%s libso_%s)\n""" % (str(lib_number), str(i), str(j)))

def createLibMakefile(lib_number, classes):
    handle = open("Makefile", "w");
    handle.write ("""COMPILER = g++
INC = -I..
CCFLAGS = -g -Wall $(INC)
ARCHIVE = ar
DEPEND = makedepend
.SUFFIXES: .o .cpp

""")
    handle.write ("lib = lib_" + str(lib_number) + ".a\n")
    handle.write ("src = \\\n")
    for i in range(classes):
        handle.write('class_' + str(i) + '.cpp \\\n')
    handle.write ("""

objects = $(patsubst %.cpp, %.o, $(src))

all: depend $(lib)

$(lib): $(objects)
	$(ARCHIVE) cr $@ $^
	touch $@

.cpp.o:
	$(COMPILER) $(CCFLAGS) -c $<

clean:
	@rm $(objects) $(lib) 2> /dev/null

depend:
	@$(DEPEND) $(INC) $(src)

""")

def createLibJamFile(lib_number, classes):
    handle = open("Jamfile", "w")
    handle.write ("SubDir TOP lib_" + str(lib_number) + " ;\n\n")
    handle.write ("SubDirHdrs $(INCLUDES) ;\n\n")
    handle.write ("Library lib_" + str(lib_number) + " :\n")
    for i in range(classes):
        handle.write('    class_' + str(i) + '.cpp\n')
    handle.write ('    ;\n')

def createVCProjFile(lib_number, classes):
    handle = open("lib_" + str(lib_number) + ".vcproj", "w")
    handle.write("""<?xml version="1.0" encoding="Windows-1252"?>
<VisualStudioProject
	ProjectType="Visual C++"
	Version="7.10"
	Name=""" + '"' + lib_name(lib_number) + '"' + """
	ProjectGUID="{CF495178-8865-4D20-939D-AAA""" + str(lib_number) + """}"
	Keyword="Win32Proj">
	<Platforms>
		<Platform
			Name="Win32"/>
	</Platforms>
	<Configurations>
		<Configuration
			Name="Debug|Win32"
			OutputDirectory="Debug"
			IntermediateDirectory="Debug"
			ConfigurationType="4"
			CharacterSet="2">
			<Tool
				Name="VCCLCompilerTool"
				Optimization="0"
				PreprocessorDefinitions="WIN32;_DEBUG;_LIB"
                AdditionalIncludeDirectories=".."
				MinimalRebuild="TRUE"
				BasicRuntimeChecks="3"
				RuntimeLibrary="5"
				UsePrecompiledHeader="0"
				WarningLevel="3"
				Detect64BitPortabilityProblems="TRUE"
				DebugInformationFormat="4"/>
			<Tool
				Name="VCCustomBuildTool"/>
			<Tool
				Name="VCLibrarianTool"
				OutputFile="$(OutDir)/""" + lib_name(lib_number) + """.lib"/>
		</Configuration>
	</Configurations>
	<References>
	</References>
	<Files>
""")

    for i in range(classes):
        handle.write(r'  <File RelativePath=".\class_' + str(i) + '.cpp"/>\n')

    handle.write("""
	</Files>
	<Globals>
	</Globals>
</VisualStudioProject>
""")

def createLibrary(lib_number, classes, internal_includes, external_includes):
    name = "lib_" + str(lib_number)
    setDir(name)
    inc = []
    for i in range(classes):
        classname = "class_" + str(i)
        createHeader(classname)
        inc.extend(createLibCPP(classname, lib_number, classes, internal_includes, external_includes))
    inc=list(set(inc))
    createSConscript(lib_number, classes)
    createLibCMakeLists(lib_number, classes, inc)
    createLibMakefile(lib_number, classes)
    createAutotools(lib_number, classes)

    os.chdir("..")

def createProgram(lib_number, classes, internal_includes, external_includes):
    name = "prog_" + str(lib_number)
    setDir(name)
    inc = []
    for i in range(classes):
        classname = "class_" + str(i)
        inc.extend(createProgCPP(classname, lib_number, classes, internal_includes, external_includes))
    inc=list(set(inc))
#    createSConscript(lib_number, classes)
    createProgCMakeLists(lib_number, classes, inc)
#    createLibMakefile(lib_number, classes)
#    createAutotools(lib_number, classes)

    os.chdir("..")

def createCMakeLists(libs):
    handle = open("CMakeLists.txt", "w")
    handle.write("""project('profiling-test')
cmake_minimum_required(VERSION 2.8)

include_directories(${CMAKE_SOURCE_DIR})
""")

    for i in range(libs):
        handle.write("""add_subdirectory(lib_%s)\n""" % str(i))
    for i in range(libs):
        handle.write("""add_subdirectory(prog_%s)\n""" % str(i))

def createSConstruct(libs):
    handle = open("SConstruct", "w");
    handle.write("""env = Environment(CPPFLAGS=['-Wall'], CPPDEFINES=['LINUX'], CPPPATH=[Dir('#')])\n""")
    handle.write("""env.Decider('timestamp-newer')\n""")
    handle.write("""env.SetOption('implicit_cache', True)\n""")
    handle.write("""env.SourceCode('.', None)\n""")

    for i in range(libs):
        handle.write("""env.SConscript("lib_%s/SConscript", exports=['env'])\n""" % str(i))

def createFullMakefile(libs):
    handle = open("Makefile", "w")

    handle.write('subdirs = \\\n')
    for i in range(libs):
        handle.write('lib_' + str(i) + '\\\n')
    handle.write("""

all: $(subdirs)
	@for i in $(subdirs); do \
    $(MAKE) -C $$i all; done

clean:
	@for i in $(subdirs); do \
	(cd $$i; $(MAKE) clean); done

depend:
	@for i in $(subdirs); do \
	(cd $$i; $(MAKE) depend); done
""")

def createFullJamfile(libs):
    handle = open("Jamfile", "w")
    handle.write ("SubDir TOP ;\n\n")

    for i in range(libs):
        handle.write('SubInclude TOP ' + lib_name(i) + ' ;\n')

    handle = open("Jamrules", "w")
    handle.write('INCLUDES = $(TOP) ;\n')

WT = """#! /usr/bin/env python
# encoding: utf-8

VERSION = '0.0.2'
APPNAME = 'build_bench'
top  = '.'
out  = 'out'

def options(opt):
	opt.load('compiler_cxx')

def configure(conf):
	conf.load('compiler_cxx')

def build(bld):
	for i in range(%d):
		filez = ' '.join(['lib_%%d/class_%%d.cpp' %% (i, j) for j in range(%d)])
		bld.stlib(
			source = filez,
			target = 'lib_%%d' %% i,
			includes = '.', # include the top-level
		)
"""

def createWtop(libs, classes):
	f = open('wscript', 'w')
	f.write(WT % (libs, classes))
	f.close()

def createFullSolution(libs):
    handle = open("solution.sln", "w")
    handle.write("Microsoft Visual Studio Solution File, Format Version 8.00\n")

    for i in range(libs):
        project_name = lib_name(i) + '\\' + lib_name(i) + '.vcproj'
        handle.write('Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "' + lib_name(i) +
                      '", "' + project_name + '", "{CF495178-8865-4D20-939D-AAA' + str(i) + '}"\n')
        handle.write('EndProject\n')

def createAutotoolsTop(libs):
    handle = open("configure.ac", "w")
    handle.write('''\
AC_INIT([bench], [1.0.0])
AC_CONFIG_AUX_DIR([autotools-aux])
AM_INIT_AUTOMAKE([subdir-objects nostdinc no-define tar-pax dist-bzip2])
AC_PROG_CXX([gcc cl KCC CC cxx cc++ xlC aCC c++ g++ clang])
AM_PROG_LIBTOOL
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
''')

    handle = open("Makefile.am", "w")
    handle.write('''\
AM_CPPFLAGS = -I$(srcdir)
lib_LTLIBRARIES =
''')
    for i in range(libs): handle.write('include lib_%s/Makefile.am\n' % str(i))

def createAutotools(lib_number, classes):

    handle = open("Makefile.am", "w")
    handle.write('''\
lib_LTLIBRARIES += lib%s.la
lib%s_la_SOURCES =''' % (str(lib_number), str(lib_number)))
    for i in range(classes): handle.write(' lib_%s/class_%s.cpp' % (str(lib_number), str(i)))
    handle.write('\n')

def setDir(dir):
    if (not os.path.exists(dir)):
        os.mkdir(dir)
    os.chdir(dir)

def main(argv):
    if len(argv) != 6:
        print(HELP_USAGE)
        return

    root_dir = argv[1]
    libs = int(argv[2])
    classes = int(argv[3])
    internal_includes = int(argv[4])
    external_includes = int(argv[5])

    setDir(root_dir)
    for i in range(libs):
        createLibrary(i, classes, internal_includes, external_includes)
        createProgram(i, classes, internal_includes, external_includes)

    createSConstruct(libs)
    createCMakeLists(libs)
    createFullMakefile(libs)
    createWtop(libs, classes)
    createAutotoolsTop(libs)

if __name__ == "__main__":
    main( sys.argv )


