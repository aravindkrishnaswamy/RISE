##################################################################################################
#
#  Makefile for Realistic Image Synthesis Engine (R.I.S.E)
#  Author: Aravind Krishnaswamy
#  Date: September 6, 2002
#
#  Notes:  I recommend using GNU make 
#  
##################################################################################################

# Include the common settings among all the platforms
include Config.common

# Includes the platform dependent settings
# This file can be a synlink or a copy of one of platform specific configuration files
# I.E. Config.Linux, Config.Cygwin, Config.SGI, ... and so forth
include Config.specific

# Includes the file list
include Filelist

##############################################################################
#
# You should not need to change anything below this, except the convenience
# targets at the very end.
#

# Make the object list for the library itself
OBJLIB = $(SRCLIB:.cpp=.o)

# Make the object list for DRISE
OBJDRISE = $(SRCDRISE:.cpp=.o)

# Test source and output lists
PATHTESTS = ../../../tests/
PATHTESTDEST = $(PATHDEST)tests/
SRCTESTS = $(wildcard $(PATHTESTS)*.cpp)
OBJTESTS = $(SRCTESTS:.cpp=.o)

# Tool source and output lists. Tools are standalone CLI utilities
# (image diff, firefly inspection, etc.) that link against librise.a
# but are NOT part of the test suite — they require arguments and
# should not be invoked by run_all_tests.sh.
PATHTOOLS = ../../../tools/
PATHTOOLDEST = $(PATHDEST)tools/
SRCTOOLS = $(wildcard $(PATHTOOLS)*.cpp)
OBJTOOLS = $(SRCTOOLS:.cpp=.o)

# Source trees owned by this makefile. Sweep these for stale object files during clean.
# extlib/stb and extlib/cgltf are included because we compile vendored
# single-header libs inline (see Filelist SRCLIBEXTLIB); their .o files
# land next to the sources there.
CLEAN_OBJECT_DIRS = $(PATHLIBRARY) $(PATHSRCS) $(PATHSRCSDRISE) $(PATHTESTS) $(PATHTOOLS) ../../../extlib/stb ../../../extlib/cgltf

# Standalone entry points -- each .cpp is a one-off main() picked up by the
# default .cpp.o rule, so they don't appear in any SRC list above. They still
# need .d snippets emitted so header edits invalidate them.
OBJSTANDALONE = \
	$(PATHSRCS)commandconsole.o \
	$(PATHSRCS)risempi.o \
	$(PATHSRCS)meshconverter.o \
	$(PATHSRCS)imageconverter.o \
	$(PATHSRCS)biospecbsdfmaker.o \
	$(PATHSRCSDRISE)drise_server.o \
	$(PATHSRCSDRISE)drise_client.o \
	$(PATHSRCSDRISE)drise_submitter.o

# Every translation unit this Makefile builds. Header-dependency snippets are
# auto-emitted alongside each .o by -MMD (see CXXFLAGS_DEPS below) and pulled
# back in via the `-include $(ALL_DEPS)` near the bottom of this file. Result:
# editing a header invalidates exactly the .o files that included it -- no
# more `make clean` after touching IJob.h or any other interface header.
ALL_OBJS = $(OBJLIB) $(OBJDRISE) $(OBJTESTS) $(OBJTOOLS) $(OBJSTANDALONE)
ALL_DEPS = $(ALL_OBJS:.o=.d)

# -MMD: emit a Make-format header-dependency snippet alongside each .o,
#       excluding system headers (which never change).
# -MP : also emit phantom targets for each header so a deleted / renamed
#       header doesn't break the next build with a "no rule to make X" error.
#
# Default targets GCC / Clang. Config.specific can override with `=` (set to
# empty for compilers that don't recognise the flags, e.g. legacy MIPSPro CC
# or pre-3.0 g++ where -MP doesn't exist).
CXXFLAGS_DEPS ?= -MMD -MP

# Rules section
# Normally you would define target upstairs and build it here, but 
# in our case we're going to explicitly build the test programs

all : librise rise meshconverter imageconverter biospecbsdfmaker

drise : drise_server drise_client drise_submitter

librise : $(OBJLIB)
	@echo Creating archive
	@ar rcs $(PATHDEST)librise.a $(OBJLIB)

rise : $(OBJLIB) $(PATHSRCS)commandconsole.o
	@echo Linking $(PATHDEST)rise
	@$(CXX)	$(CPPFLAGS) -o $(PATHDEST)rise $(PATHSRCS)commandconsole.o $(OBJLIB) $(LDLIBS)

risempi : $(OBJLIB) $(PATHSRCS)risempi.o
	@echo Linking $(PATHDEST)risempi
	@$(CXX)	$(CPPFLAGS) -o $(PATHDEST)risempi $(PATHSRCS)risempi.o $(OBJLIB) $(LDLIBS) -lmpi -lmpi++

meshconverter : $(OBJLIB) $(PATHSRCS)meshconverter.o
	@echo Linking $(PATHDEST)meshconverter
	@$(CXX) $(CPPFLAGS) -o $(PATHDEST)meshconverter $(PATHSRCS)meshconverter.o $(OBJLIB) $(LDLIBS) 

biospecbsdfmaker : $(OBJLIB) $(PATHSRCS)biospecbsdfmaker.o
	@echo Linking $(PATHDEST)biospecbsdfmaker
	@$(CXX) $(CPPFLAGS) -o $(PATHDEST)biospecbsdfmaker $(PATHSRCS)biospecbsdfmaker.o $(OBJLIB) $(LDLIBS) 

imageconverter : $(OBJLIB) $(PATHSRCS)imageconverter.o
	@echo Linking $(PATHDEST)imageconverter
	@$(CXX) $(CPPFLAGS) -o $(PATHDEST)imageconverter $(PATHSRCS)imageconverter.o $(OBJLIB) $(LDLIBS) 


# HDRVarianceTest -- standalone variance-measurement tool (K-trial EXR
# per-pixel variance + RMSE-vs-reference; source tools/HDRVarianceTest.cpp).
# Mirrors the imageconverter link pattern but lands in bin/tools/.  Needs
# OpenEXR at link time, which $(LDLIBS) already carries via $(LDLIBS_EXR)
# (Config.OSX).  Dedicated target so variance work does not depend on the
# other tools/ sources compiling -- unlike the catch-all `tools` target.
HDRVarianceTest : $(OBJLIB) $(PATHTOOLS)HDRVarianceTest.o
	@mkdir -p $(PATHTOOLDEST)
	@echo Linking $(PATHTOOLDEST)HDRVarianceTest
	@$(CXX) $(CPPFLAGS) -o $(PATHTOOLDEST)HDRVarianceTest $(PATHTOOLS)HDRVarianceTest.o $(OBJLIB) $(LDLIBS)

# MigrateScenesV6toV7 -- the canonical v6->v7 scene migrator tool (Phase A of the parser cutover;
# docs/agentic-redesign/61-...).  Wraps the shared tools/CstMigrator.h, same pattern as HDRVarianceTest.
MigrateScenesV6toV7 : $(OBJLIB) $(PATHTOOLS)MigrateScenesV6toV7.o
	@mkdir -p $(PATHTOOLDEST)
	@echo Linking $(PATHTOOLDEST)MigrateScenesV6toV7
	@$(CXX) $(CPPFLAGS) -o $(PATHTOOLDEST)MigrateScenesV6toV7 $(PATHTOOLS)MigrateScenesV6toV7.o $(OBJLIB) $(LDLIBS)
drise_server : $(OBJLIB) $(OBJDRISE) $(PATHSRCSDRISE)drise_server.o
	@echo Linking $(PATHDEST)drise_server
	@$(CXX) $(CPPFLAGS) -o $(PATHDEST)drise_server $(PATHSRCSDRISE)drise_server.o $(OBJLIB) $(OBJDRISE) $(LDLIBS) 

drise_client : $(OBJLIB) $(OBJDRISE) $(PATHSRCSDRISE)drise_client.o
	@echo Linking $(PATHDEST)drise_client
	@$(CXX) $(CPPFLAGS) -o $(PATHDEST)drise_client $(PATHSRCSDRISE)drise_client.o $(OBJLIB) $(OBJDRISE) $(LDLIBS) 

drise_submitter : $(OBJLIB) $(OBJDRISE) $(PATHSRCSDRISE)drise_submitter.o
	@echo Linking $(PATHDEST)drise_submitter
	@$(CXX) $(CPPFLAGS) -o $(PATHDEST)drise_submitter $(PATHSRCSDRISE)drise_submitter.o $(OBJLIB) $(OBJDRISE) $(LDLIBS) 

# Build all tests.  Prerequisite-driven so `make -jN tests` compiles and
# LINKS tests in parallel: each test binary is its own target via the
# $(PATHTESTDEST)% pattern rule below, and make schedules them across -j
# slots.  (The previous version was ONE recipe containing a shell for-loop,
# which compiled and linked all tests sequentially regardless of -j — after
# any library change, ~131 serial links against the full $(OBJLIB) took tens
# of minutes on a single core while looking like "the tests are slow".)
# A broken test stops a plain `make tests` at that target with make's own
# diagnostics; use `make -k -jN tests` to keep building past failures (CI
# still gets a non-zero exit either way).  run_all_tests.sh isolates and
# logs individual build failures itself.
TESTBINS = $(addprefix $(PATHTESTDEST),$(notdir $(SRCTESTS:.cpp=)))

tests : $(TESTBINS)
	@echo "============================================================"
	@echo "tests: all built successfully"

# Single-test build target. Used by run_all_tests.sh to drive per-test
# build progress with quiet, formatted output. Real-file target so make
# can skip up-to-date binaries; phony alias gives the caller a stable name.
# .SECONDARY keeps the binary after make satisfies the build-test/% alias —
# without it, make treats the binary as an intermediate and auto-deletes it.
.SECONDARY:

build-test/% : $(PATHTESTDEST)%
	@:

# Compile and link split into two rules so a library-only change relinks
# WITHOUT recompiling every test translation unit (the .d dependency files
# emitted by $(CXXFLAGS_DEPS) keep the .o rebuilds header-accurate).
$(PATHTESTS)%.o : $(PATHTESTS)%.cpp
	@echo Compiling: $<
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHTESTDEST)% : $(PATHTESTS)%.o $(OBJLIB)
	@mkdir -p $(PATHTESTDEST)
	@echo Linking $@
	@$(CXX) $(CPPFLAGS) -o $@ $(PATHTESTS)$*.o $(OBJLIB) $(LDLIBS)

# Build all tools. Mirrors the `tests` target — same loop-and-continue
# pattern so one broken tool doesn't block the rest.
tools : $(OBJLIB)
	@mkdir -p $(PATHTOOLDEST)
	@fail_count=0; failed_list=""; \
	for tool_src in $(SRCTOOLS); do \
		tool_name=`basename $$tool_src .cpp`; \
		echo Compiling: $$tool_src; \
		if ! $(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $$tool_src -o $(PATHTOOLS)$$tool_name.o; then \
			fail_count=$$((fail_count + 1)); failed_list="$$failed_list $$tool_name(compile)"; \
			continue; \
		fi; \
		echo Linking $(PATHTOOLDEST)$$tool_name; \
		if ! $(CXX) $(CPPFLAGS) -o $(PATHTOOLDEST)$$tool_name $(PATHTOOLS)$$tool_name.o $(OBJLIB) $(LDLIBS); then \
			fail_count=$$((fail_count + 1)); failed_list="$$failed_list $$tool_name(link)"; \
		fi; \
	done; \
	echo "============================================================"; \
	if [ $$fail_count -ne 0 ]; then \
		echo "tools: $$fail_count tool(s) failed to build:$$failed_list"; \
		exit 1; \
	fi; \
	echo "tools: all built successfully"

# Build only the library .o files needed to link tests. Used by
# run_all_tests.sh as a one-shot library-build phase before per-test builds.
test-objs : $(OBJLIB)
	@:

# Build and run all tests
test : tests
	@echo Running all tests
	@sh ../../../run_all_tests.sh

.SUFFIXES:
.SUFFIXES: .cpp .o

.cpp.o:
	@echo Compiling: $<
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

# automatically generate source file dependencies

depend:
	@echo Generating dependency list
	@$(CXX) -M $(CPPFLAGS) -M $(CXXFLAGS) $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(SRCS) $(SRCLIB) $(SRCSDRISECLIENT) $(SRCSDRISESERVER) $(SRCSPRISEWORKER) $(SRCSPRISESCHEDULER) > deps

# This section is for various "convenience" targets.

install:
	@echo Creating folder: $(PATHDEST) and ../../../rendered
	@mkdir $(PATHDEST)
	@mkdir ../../../rendered

files:
	@echo Makefile $(HDRS) $(SRCLIB) $(SRCS)

clean:
	@echo Removing all objects and build targets
	@rm -rf $(PATHDEST)rise $(PATHDEST)risempi $(PATHDEST)librise.a $(PATHDEST)meshconverter $(PATHDEST)imageconverter $(PATHDEST)biospecbsdfmaker $(PATHDEST)drise_server $(PATHDEST)drise_client $(PATHDEST)drise_submitter $(OBJS) $(OBJMPI) $(OBJLIB)
	@rm -rf $(OBJDRISE) $(PATHSRCSDRISE)drise_server.o $(PATHSRCSDRISE)drise_client.o $(PATHSRCSDRISE)drise_submitter.o
	@rm -rf $(PATHSRCS)commandconsole.o $(PATHSRCS)meshconverter.o $(PATHSRCS)imageconverter.o $(PATHSRCS)biospecbsdfmaker.o
	@rm -rf $(OBJTESTS) $(PATHTESTDEST)
	@rm -rf $(OBJTOOLS) $(PATHTOOLDEST)
	@find $(CLEAN_OBJECT_DIRS) -type f \( -name '*.o' -o -name '*.d' \) -exec rm -f {} \;
	@rm -rf $(PATHSRCS)ii_files
	@rm -rf $(PATHLIBRARY)Animation/ii_files
	@rm -rf $(PATHLIBRARY)Cameras/ii_files
	@rm -rf $(PATHLIBRARY)Functions/ii_files
	@rm -rf $(PATHLIBRARY)Geometry/ii_files
	@rm -rf $(PATHLIBRARY)Interfaces/ii_files
	@rm -rf $(PATHLIBRARY)Intersection/ii_files
	@rm -rf $(PATHLIBRARY)Lights/ii_files
	@rm -rf $(PATHLIBRARY)Managers/ii_files
	@rm -rf $(PATHLIBRARY)Materials/ii_files
	@rm -rf $(PATHLIBRARY)Modifiers/ii_files
	@rm -rf $(PATHLIBRARY)Noise/ii_files
	@rm -rf $(PATHLIBRARY)Objects/ii_files
	@rm -rf $(PATHLIBRARY)Painters/ii_files
	@rm -rf $(PATHLIBRARY)PhotonMapping/ii_files
	@rm -rf $(PATHLIBRARY)Rendering/ii_files
	@rm -rf $(PATHLIBRARY)Cameras/ii_files
	@rm -rf $(PATHLIBRARY)Parsers/ii_files
	@rm -rf $(PATHLIBRARY)Sampling/ii_files
	@rm -rf $(PATHLIBRARY)Surfaces/ii_files
	@rm -rf $(PATHLIBRARY)Utilities/ii_files
	@rm -rf $(PATHLIBRARY)Utilities/Color/ii_files
	@rm -rf $(PATHLIBRARY)Utilities/Communications/ii_files
	@rm -rf $(PATHLIBRARY)Utilities/Log/ii_files
	@rm -rf $(PATHLIBRARY)Utilities/Math3D/ii_files
	@rm -rf $(PATHLIBRARY)Utilities/Threads/ii_files
	

# include the automatically generated source file dependencies
#include deps

# An object present WITHOUT its .d sidecar has no recorded header
# prerequisites: it was compiled by an older make (pre -MMD), orphaned when a
# .d was deleted, or carried in from another tree.  It therefore looks up to
# date against its lone .cpp prerequisite even after a header it includes has
# changed.  The test and standalone binaries link the object list directly
# ($(OBJLIB) + the per-binary .o), and compile/link are separate rules, so such
# a stale object is RELINKED in place when a library-header change rebuilds the
# library half: a stale vtable at best, an "Undefined symbols" link failure on
# any signature change at worst (the exact failure mode this tracking exists to
# prevent).  The `-include` below cannot help -- it only wires up deps a .d
# already records.  So force every object whose .d is currently missing to
# recompile, which regenerates an accurate .d via -MMD; from then on `-include`
# keeps it header-accurate.  (Objects with neither .o nor .d are swept up too --
# harmless: they compile regardless of the forced prerequisite.)  Deliberately
# existence-based, not mtime-based: clang emits the .d a few hundred microseconds
# AFTER the .o, so a plain `%.o : %.d` prerequisite would relink-loop forever
# under make >= 4.x (sub-second mtimes).  In steady state (every .d present) the
# list is empty and the rule vanishes -- zero effect on normal incremental
# builds.
#
# Gated on CXXFLAGS_DEPS being non-empty.  The legacy Config.SGI / MIPSPro
# toolchains disable dependency generation (CXXFLAGS_DEPS =), so they never emit
# a .d; without this gate EVERY object would look "missing a .d" and recompile
# on every invocation.  With deps off the build relies on `make clean` after a
# header edit as it always has, so suppressing the force-rule there is correct.
OBJS_MISSING_DEP := $(filter-out $(patsubst %.d,%.o,$(wildcard $(ALL_DEPS))),$(ALL_OBJS))
ifneq ($(strip $(CXXFLAGS_DEPS)),)
ifneq ($(OBJS_MISSING_DEP),)
$(OBJS_MISSING_DEP) : FORCE_DEP_REGEN
endif
endif
FORCE_DEP_REGEN :
.PHONY: FORCE_DEP_REGEN

# Pull in the per-translation-unit header dependency snippets emitted by
# -MMD. The leading dash silences the include for missing files, which is
# the normal state on a fresh tree (no .d files exist yet) -- after the
# first compile the snippets land next to their .o files and subsequent
# builds see exactly which TUs each header touches.
-include $(ALL_DEPS)
