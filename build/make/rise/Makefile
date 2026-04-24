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
CLEAN_OBJECT_DIRS = $(PATHLIBRARY) $(PATHSRCS) $(PATHSRCSDRISE) $(PATHTESTS) $(PATHTOOLS)

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

drise_server : $(OBJLIB) $(OBJDRISE) $(PATHSRCSDRISE)drise_server.o
	@echo Linking $(PATHDEST)drise_server
	@$(CXX) $(CPPFLAGS) -o $(PATHDEST)drise_server $(PATHSRCSDRISE)drise_server.o $(OBJLIB) $(OBJDRISE) $(LDLIBS) 

drise_client : $(OBJLIB) $(OBJDRISE) $(PATHSRCSDRISE)drise_client.o
	@echo Linking $(PATHDEST)drise_client
	@$(CXX) $(CPPFLAGS) -o $(PATHDEST)drise_client $(PATHSRCSDRISE)drise_client.o $(OBJLIB) $(OBJDRISE) $(LDLIBS) 

drise_submitter : $(OBJLIB) $(OBJDRISE) $(PATHSRCSDRISE)drise_submitter.o
	@echo Linking $(PATHDEST)drise_submitter
	@$(CXX) $(CPPFLAGS) -o $(PATHDEST)drise_submitter $(PATHSRCSDRISE)drise_submitter.o $(OBJLIB) $(OBJDRISE) $(LDLIBS) 

# Build all tests. A broken test must not prevent the others from building, so
# the loop continues past compile/link failures and reports a per-test tally at
# the end. The target still exits non-zero if any test failed to build, so CI
# preserves its "broken build" signal — but every test that *can* build does.
tests : $(OBJLIB)
	@mkdir -p $(PATHTESTDEST)
	@fail_count=0; failed_list=""; \
	for test_src in $(SRCTESTS); do \
		test_name=`basename $$test_src .cpp`; \
		echo Compiling: $$test_src; \
		if ! $(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) -c $$test_src -o $(PATHTESTS)$$test_name.o; then \
			fail_count=$$((fail_count + 1)); failed_list="$$failed_list $$test_name(compile)"; \
			continue; \
		fi; \
		echo Linking $(PATHTESTDEST)$$test_name; \
		if ! $(CXX) $(CPPFLAGS) -o $(PATHTESTDEST)$$test_name $(PATHTESTS)$$test_name.o $(OBJLIB) $(LDLIBS); then \
			fail_count=$$((fail_count + 1)); failed_list="$$failed_list $$test_name(link)"; \
		fi; \
	done; \
	echo "============================================================"; \
	if [ $$fail_count -ne 0 ]; then \
		echo "tests: $$fail_count test(s) failed to build:$$failed_list"; \
		exit 1; \
	fi; \
	echo "tests: all built successfully"

# Single-test build target. Used by run_all_tests.sh to drive per-test
# build progress with quiet, formatted output. Real-file target so make
# can skip up-to-date binaries; phony alias gives the caller a stable name.
# .SECONDARY keeps the binary after make satisfies the build-test/% alias —
# without it, make treats the binary as an intermediate and auto-deletes it.
.SECONDARY:

build-test/% : $(PATHTESTDEST)%
	@:

$(PATHTESTDEST)% : $(PATHTESTS)%.cpp $(OBJLIB)
	@mkdir -p $(PATHTESTDEST)
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) -c $< -o $(PATHTESTS)$*.o
	@$(CXX) $(CPPFLAGS) -o $@ $(PATHTESTS)$*.o $(OBJLIB) $(LDLIBS)

# Build all tools. Mirrors the `tests` target — same loop-and-continue
# pattern so one broken tool doesn't block the rest.
tools : $(OBJLIB)
	@mkdir -p $(PATHTOOLDEST)
	@fail_count=0; failed_list=""; \
	for tool_src in $(SRCTOOLS); do \
		tool_name=`basename $$tool_src .cpp`; \
		echo Compiling: $$tool_src; \
		if ! $(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) -c $$tool_src -o $(PATHTOOLS)$$tool_name.o; then \
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
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) -c $< -o $@

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
	@find $(CLEAN_OBJECT_DIRS) -type f -name '*.o' -exec rm -f {} \;
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
