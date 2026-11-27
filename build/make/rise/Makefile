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
# Eval-harness E4: the library source list may contain an ObjC++ .mm (the
# macOS platform-TLS leg, OS-gated in Filelist).  Map BOTH .cpp and .mm to
# .o so the .mm's object lands in the library + gets a tracked .d.
OBJLIB = $(patsubst %.mm,%.o,$(SRCLIB:.cpp=.o))

# r108 calibration derivation must execute in a process that cannot dispatch
# Metal.  The dedicated link keeps the ordinary CPU library objects, replaces
# all four production Metal legs with their honest unsupported stubs, and
# removes the Metal framework itself.  The post-link symbol/framework audit is
# part of the target, not an optional source-hygiene test.
ifdef IS_MACOS_BUILD
FIREPRODUCTIONNOMETALSTUBS = \
	$(PATHLIBRARY)Utilities/FireProductionComputeUnsupported.o \
	$(PATHLIBRARY)Utilities/FireProductionAdvectionUnsupported.o \
	$(PATHLIBRARY)Utilities/FireProductionProjectionUnsupported.o \
	$(PATHLIBRARY)Utilities/FireProductionForceUnsupported.o
OBJLIB_NOMETAL = $(filter-out \
	$(PATHLIBRARY)Utilities/FireProductionComputeMac.o \
	$(PATHLIBRARY)Utilities/FireProductionAdvectionMac.o \
	$(PATHLIBRARY)Utilities/FireProductionProjectionMac.o \
	$(PATHLIBRARY)Utilities/FireProductionForceMac.o,$(OBJLIB)) \
	$(FIREPRODUCTIONNOMETALSTUBS)
LDLIBS_NOMETAL = $(subst -framework Metal,,$(LDLIBS))
else
FIREPRODUCTIONNOMETALSTUBS =
OBJLIB_NOMETAL = $(OBJLIB)
LDLIBS_NOMETAL = $(LDLIBS)
endif

# Make the object list for DRISE
OBJDRISE = $(SRCDRISE:.cpp=.o)

# Test source and output lists
PATHTESTS = ../../../tests/
PATHTESTDEST = $(PATHDEST)tests/
SRCTESTS = $(wildcard $(PATHTESTS)*.cpp)
OBJTESTS = $(SRCTESTS:.cpp=.o)
FIREPRODUCTIONFP64SOURCES = $(wildcard $(PATHTESTS)fire_production_fp64/*.cpp)
FIREPRODUCTIONFP64OBJECTS = $(FIREPRODUCTIONFP64SOURCES:.cpp=.o)
FIREPRODUCTIONTRACESOURCES = $(wildcard $(PATHTESTS)fire_production_trace/*.cpp)
FIREPRODUCTIONTRACEOBJECTS = $(FIREPRODUCTIONTRACESOURCES:.cpp=.o)
ifdef IS_MACOS_BUILD
FIREPREVIEWLIBS = -framework ImageIO -framework CoreGraphics
else
FIREPREVIEWLIBS =
endif
FIREPRODUCTIONFP64ORIGINALS = \
	$(PATHLIBRARY)Utilities/FireProductionAdvection.h \
	$(PATHLIBRARY)Utilities/FireProductionAdvection.cpp \
	$(PATHLIBRARY)Utilities/FireProductionProjection.h \
	$(PATHLIBRARY)Utilities/FireProductionProjection.cpp \
	$(PATHLIBRARY)Utilities/FireProductionTransport.h \
	$(PATHLIBRARY)Utilities/FireProductionTransport.cpp \
	$(PATHLIBRARY)Utilities/FireProductionForce.h \
	$(PATHLIBRARY)Utilities/FireProductionForce.cpp

# Tool source and output lists. Tools are standalone CLI utilities
# (image diff, firefly inspection, etc.) that link against librise.a
# but are NOT part of the test suite — they require arguments and
# should not be invoked by run_all_tests.sh.
PATHTOOLS = ../../../tools/
PATHTOOLDEST = $(PATHDEST)tools/
# fire_gas_opacity_native.cpp is a quarantined shared-library backend with a
# C ABI, not a standalone executable (and intentionally has no main()).
SRCTOOLS = $(filter-out $(PATHTOOLS)fire_gas_opacity_native.cpp,$(wildcard $(PATHTOOLS)*.cpp))
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
ALL_OBJS = $(OBJLIB) $(FIREPRODUCTIONNOMETALSTUBS) $(FIREPRODUCTIONFP64OBJECTS) \
	$(FIREPRODUCTIONTRACEOBJECTS) $(OBJDRISE) $(OBJTESTS) $(OBJTOOLS) $(OBJSTANDALONE) \
	$(OBJTESTLIB)
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

RENDERER_BUILD_HEADER = RendererBuildIdentity.generated.h
RENDERER_FP_CONTRACTION = $(if $(findstring -ffast-math,$(CXXFLAGS)),fast,compiler_default)
RENDERER_OPTIMIZATION = $(if $(findstring -O3,$(CXXFLAGS)),O3,$(if $(findstring -O2,$(CXXFLAGS)),O2,$(if $(findstring -O1,$(CXXFLAGS)),O1,$(if $(findstring -Os,$(CXXFLAGS)),Os,$(if $(findstring -Oz,$(CXXFLAGS)),Oz,compiler_default)))))
RENDERER_LTO = $(if $(findstring -flto=thin,$(CXXFLAGS)),thin,$(if $(findstring -flto,$(CXXFLAGS)),full,off))
CPPFLAGS += -I$(CURDIR)

# The single-letter flags are MAKEFLAGS's first word, or MAKEFLAGS starts
# with a space when there are none (the GNU make manual's idiom; later words
# can be -Oline, -I<dir> or VAR=value on make 4.x).  -n is a dry run and -t
# only touches, so neither regenerates the build identity below nor records
# the test link mode: a -t after a mode switch must not make objects compiled
# in the old mode look current.  -q regenerates the build identity, so its
# answer is about the current source state; with a test goal it also records
# the test link mode (deleting the test objects and binaries after a switch,
# as below).
# GNU make 3.81 (macOS /usr/bin/make) instead puts --long options first and
# the single-letter flags after them as -x words; 4.x's later -x words can be
# -Oline or -I<dir>, so only 3.81 reads them.
ifeq ($(MAKE_VERSION),3.81)
MAKE_FLAG_WORD = $(firstword -$(MAKEFLAGS)) $(filter-out --%,$(filter -%,$(MAKEFLAGS)))
else
MAKE_FLAG_WORD = $(firstword -$(MAKEFLAGS))
endif
MAKE_DRY_RUN = $(findstring n,$(MAKE_FLAG_WORD))$(findstring t,$(MAKE_FLAG_WORD))
MAKE_SKIP_GENERATED = $(MAKE_DRY_RUN)$(filter clean,$(MAKECMDGOALS))

# The build identity (source revision, dirty-state hash, FP / optimization /
# LTO modes) is regenerated when make reads this file, not by a phony rule:
# the generator rewrites the header only when its content changes, so Job.o
# recompiles exactly when the identity moves, and `make -q` can still say a
# target is up to date (a phony prerequisite would make every target that
# reaches Job.o look out of date to -q).  A dry run, -t and clean leave it
# alone.
RENDERER_IDENTITY_GEN = python3 ../../../tools/generate_renderer_build_identity_header.py \
	--repo-root ../../.. \
	--fp-contraction $(RENDERER_FP_CONTRACTION) \
	--optimization-mode $(RENDERER_OPTIMIZATION)
ifeq ($(MAKE_SKIP_GENERATED),)
RENDERER_IDENTITY_STATUS := $(shell $(RENDERER_IDENTITY_GEN) --output $(RENDERER_BUILD_HEADER) --lto-mode $(RENDERER_LTO) || echo failed)
ifneq ($(RENDERER_IDENTITY_STATUS),)
$(error generate_renderer_build_identity_header.py failed for $(RENDERER_BUILD_HEADER))
endif
endif

$(PATHLIBRARY)Job.o : $(RENDERER_BUILD_HEADER)
# Only when the header is missing: a dry run in a fresh tree, or after the
# clean of a `make clean all`.
$(RENDERER_BUILD_HEADER) :
	@$(RENDERER_IDENTITY_GEN) --output $@ --lto-mode $(RENDERER_LTO)

# Rules section
# Normally you would define target upstairs and build it here, but 
# in our case we're going to explicitly build the test programs

all : librise rise meshconverter imageconverter biospecbsdfmaker fire-simulator

drise : drise_server drise_client drise_submitter

LIBRISE_A = $(PATHDEST)librise.a
ARCHIVE_BUILD_DEFS = Makefile Filelist Config.common Config.specific
ifdef IS_MACOS_BUILD
ARCHIVE_LINK_LIBRISE = -Wl,-dead_strip $(LIBRISE_A)
else
ARCHIVE_LINK_LIBRISE = -Wl,--start-group $(LIBRISE_A) -Wl,--end-group
endif

librise : $(LIBRISE_A)

# Test binaries link against their own copy of the library, compiled WITHOUT
# link-time optimisation, whenever the platform config builds the library with
# -flto (macOS).  With -flto every one of the ~530 test links re-ran LTO code
# generation over the whole library slice it pulled in (~50 s of one core
# each): a clean `make -j10 tests` took 2098 s, and so did relinking every test
# after a one-line library change.  Measured now (2026-11-18, 10-core M-series,
# both libraries, 523 non-LTO + 8 LTO test links): a clean `make -j10 tests`
# takes 235 s, and rebuilding the tests after touching one library source
# 63-65 s, most of it the eight LTO links below (the non-LTO relinks alone
# took 18 s before those eight were added).  After the fire-solver merge
# (eight LTO tests at the time, plus three fire binaries that link the LTO objects
# directly) a clean `make -j10 tests` took 242 s here, where FireSequenceTest
# and its oracle do not build (they need OpenVDB).
# Costs: same -O3 / -ffast-math flags but no cross-TU inlining (what the Linux
# build has always done), measured ~0-9 % slower test runs (+3 % on a
# six-test sample); and a library edit compiles twice, once per library
# (about 50 s more for a full library rebuild).  The shipped
# binaries (`make all`, Xcode) are unaffected.  (The TESTS_LINK_LTO tests
# below, and the three fire binaries with their own link rules, also need the
# LTO library, so `make tests` builds both.)
#
# TESTS_LINK_LTO keeps the old LTO build for the tests whose results depend
# on LTO code generation: SSSNearUnityFresnelTest (last-bit compensated-
# arithmetic oracle sensitive to cross-TU FMA), FireSimulationSolverTest (an
# r80 recorded cycling-state replay), and FireProductionSolverTest (a pinned
# r83 table-package digest).  All other tests link against the non-LTO
# librise_test.a archive.
# `make TEST_LTO=1 tests` links every test against the LTO library instead,
# for checking something that depends on LTO code generation (run the suite
# that way with `TEST_LTO=1 ./run_all_tests.sh`).  The mode is recorded in
# $(TEST_MODE_STAMP), so the next make that builds tests after a switch
# rebuilds them; dry runs leave the record alone (but see the stamp rule for
# -t), and `clean` deletes it.
TESTS_LINK_LTO = SSSNearUnityFresnelTest FireSimulationSolverTest FireProductionSolverTest

ifneq ($(filter -flto%,$(CPPFLAGS) $(CXXFLAGS)),)
ifeq ($(filter-out 0,$(strip $(TEST_LTO))),)
TESTLIB_SPLIT = 1
endif
endif

ifdef TESTLIB_SPLIT
TESTOBJ_DIR = $(PATHDEST)testobj/
TEST_IDENTITY_DIR = $(TESTOBJ_DIR)identity/
TEST_RENDERER_BUILD_HEADER = $(TEST_IDENTITY_DIR)$(RENDERER_BUILD_HEADER)
TEST_CPPFLAGS = -I$(TEST_IDENTITY_DIR) $(filter-out -flto -flto=%,$(CPPFLAGS))
TEST_CXXFLAGS = $(filter-out -flto -flto=%,$(CXXFLAGS))
OBJTESTLIB = $(patsubst ../../../%,$(TESTOBJ_DIR)%,$(OBJLIB))
LIBRISE_TEST_A = $(PATHDEST)librise_test.a
ifdef IS_MACOS_BUILD
ARCHIVE_LINK_LIBRISE_TEST = -Wl,-dead_strip $(LIBRISE_TEST_A)
else
ARCHIVE_LINK_LIBRISE_TEST = -Wl,--start-group $(LIBRISE_TEST_A) -Wl,--end-group
endif
TEST_LINK_MODE = split
LTO_TESTS = $(TESTS_LINK_LTO)

$(LIBRISE_TEST_A) : $(OBJTESTLIB) $(ARCHIVE_BUILD_DEFS)
	@mkdir -p $(PATHDEST)
	@echo Creating test archive
	@rm -f $(LIBRISE_TEST_A)
	@ar rcs $(LIBRISE_TEST_A) $(OBJTESTLIB)

$(TESTOBJ_DIR)%.o : ../../../%.cpp
	@mkdir -p $(dir $@)
	@echo Compiling for tests: $<
	@$(CXX) $(TEST_CPPFLAGS) $(TEST_CXXFLAGS) $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(TESTOBJ_DIR)%.o : ../../../%.mm
	@mkdir -p $(dir $@)
	@echo "Compiling for tests (ObjC++): $<"
	@$(CXX) $(TEST_CPPFLAGS) $(TEST_CXXFLAGS) $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -x objective-c++ -fobjc-arc -c $< -o $@

# Library files with their own compile flags (see the per-file rules near the
# end of this file) keep them in the test copy: the fire production operators,
# the simulation solvers, and the Cycles procedural ports (OslNoise, OslVoronoi,
# OslColor, OslMath, OslVector, OslShaderNodes) compile without fast-math or FP
# contraction (FireCase's rule only drops LTO, which the test copy does anyway).
TEST_SAFE_FP32_OBJS = $(addprefix $(TESTOBJ_DIR)src/Library/Utilities/, \
	FireProductionAdvection.o FireProductionProjection.o \
	FireProductionTransport.o FireProductionSource.o FireProductionForce.o \
	OslNoise.o OslVoronoi.o OslColor.o OslMath.o OslVector.o OslShaderNodes.o)
# M2 track B: the fire run driver moved out of tests/FireSequenceTest.cpp.
TEST_SAFE_FP32_OBJS += $(addprefix $(TESTOBJ_DIR)src/Library/Simulation/Fire/, \
	FireRunCheckpoint.o FireRunFrames.o FireRunDriver.o FireRun.o FireSolver.o)
# M3a: the cloth solver (strict IEEE arithmetic for reproducible checkpoints and evidence).
TEST_SAFE_FP32_OBJS += $(addprefix $(TESTOBJ_DIR)src/Library/Simulation/Cloth/, \
	ClothMaterial.o ClothLinearAlgebra.o ClothModel.o ClothParallel.o ClothContact.o ClothValidation.o ClothSolver.o)
# HYLIOS compensated geometry, certified overlap and normalized surface laws require strict arithmetic.
TEST_SAFE_FP32_OBJS += $(addprefix $(TESTOBJ_DIR)src/Library/Materials/HyLIoS/, \
	HyLIoSParticleGeometry.o HyLIoSParticleOverlap.o HyLIoSComplexLayout.o HyLIoSSurfaceBoundary.o HyLIoSLocalBake.o HyLIoSSkinMaterial.o HyperspectralRadiometricRasterizer.o)
# Compensated beauty moments preserve binary64 error-free transforms.
TEST_SAFE_FP32_OBJS += $(TESTOBJ_DIR)src/Library/Rendering/BeautyMoments.o
TEST_SAFE_FP32_MM_OBJS = $(addprefix $(TESTOBJ_DIR)src/Library/Utilities/, \
	FireProductionAdvectionMac.o FireProductionProjectionMac.o \
	FireProductionForceMac.o)

$(filter $(OBJTESTLIB),$(TEST_SAFE_FP32_OBJS)) : $(TESTOBJ_DIR)%.o : ../../../%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling for tests (safe fp32): $<"
	@$(CXX) $(TEST_CPPFLAGS) $(filter-out -ffast-math,$(TEST_CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(filter $(OBJTESTLIB),$(TEST_SAFE_FP32_MM_OBJS)) : $(TESTOBJ_DIR)%.o : ../../../%.mm
	@mkdir -p $(dir $@)
	@echo "Compiling for tests (safe fp32 ObjC++): $<"
	@$(CXX) $(TEST_CPPFLAGS) $(filter-out -ffast-math,$(TEST_CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -x objective-c++ -fobjc-arc -c $< -o $@

# The test copy's Job.o records its own build identity: the same source state
# with LTO off (TEST_CPPFLAGS puts this directory ahead of -I$(CURDIR)).
ifeq ($(MAKE_SKIP_GENERATED),)
TEST_RENDERER_IDENTITY_STATUS := $(shell $(RENDERER_IDENTITY_GEN) --output $(TEST_RENDERER_BUILD_HEADER) --lto-mode off || echo failed)
ifneq ($(TEST_RENDERER_IDENTITY_STATUS),)
$(error generate_renderer_build_identity_header.py failed for $(TEST_RENDERER_BUILD_HEADER))
endif
endif
$(TEST_RENDERER_BUILD_HEADER) :
	@$(RENDERER_IDENTITY_GEN) --output $@ --lto-mode off

$(TESTOBJ_DIR)src/Library/Job.o : $(TEST_RENDERER_BUILD_HEADER)
else
TEST_CPPFLAGS = $(CPPFLAGS)
TEST_CXXFLAGS = $(CXXFLAGS)
OBJTESTLIB =
LIBRISE_TEST_A = $(LIBRISE_A)
ARCHIVE_LINK_LIBRISE_TEST = $(ARCHIVE_LINK_LIBRISE)
TEST_LINK_MODE = $(if $(filter -flto%,$(CPPFLAGS) $(CXXFLAGS)),lto,plain)
LTO_TESTS =
endif

# Per-test flags: an LTO_TESTS test compiles and links exactly as before.
TEST_CPPFLAGS_FOR = $(if $(filter $(1),$(LTO_TESTS)),$(CPPFLAGS),$(TEST_CPPFLAGS))
TEST_CXXFLAGS_FOR = $(if $(filter $(1),$(LTO_TESTS)),$(CXXFLAGS),$(TEST_CXXFLAGS))
TEST_ARCHIVE_FOR = $(if $(filter $(1),$(LTO_TESTS)),$(ARCHIVE_LINK_LIBRISE),$(ARCHIVE_LINK_LIBRISE_TEST))

# The test link mode in use, rewritten only when it changes.  Every
# pattern-built test object and binary depends on it; a switch is acted on by
# the next make with a test goal (below), which deletes them.
TEST_MODE_STAMP = $(PATHDEST).test-link-mode
# Only a make that builds tests records the mode; any other (`make all`, say,
# beside a suite running in the other mode) leaves the record and the test
# binaries alone.  A mode change also deletes the test objects and binaries:
# make 3.81 compares whole-second mtimes, so an object or binary made in the
# same second as the new record would otherwise count as up to date.
# Every goal that builds test objects or binaries must be listed here,
# including aliases of `tests` such as `test`.
TEST_GOALS = $(filter test tests build-test/% $(PATHTESTDEST)% $(PATHTESTS)%,$(MAKECMDGOALS))
ifeq ($(MAKE_SKIP_GENERATED),)
ifneq ($(TEST_GOALS),)
$(shell mkdir -p $(PATHDEST) && { [ "`cat $(TEST_MODE_STAMP) 2>/dev/null`" = "$(TEST_LINK_MODE) $(LTO_TESTS)" ] || { rm -rf $(OBJTESTS) $(PATHTESTDEST); echo "$(TEST_LINK_MODE) $(LTO_TESTS)" > $(TEST_MODE_STAMP); }; })
endif
endif
# Only when the record is missing, e.g. after the clean of `make clean tests`;
# `make -n` prints this and `make -t` touches an empty record only when
# something below it is out of date; the next real make with a test goal
# replaces an empty record (deleting the test objects and binaries).
$(TEST_MODE_STAMP) :
	@mkdir -p $(PATHDEST) && echo "$(TEST_LINK_MODE) $(LTO_TESTS)" > $@

# Depend on $(ARCHIVE_BUILD_DEFS) in addition to $(OBJLIB) so removing a
# translation unit from Filelist (or changing platform gates in Config.*)
# invalidates and recreates the archive rather than leaving removed members
# inside a stale librise.a.
$(LIBRISE_A) : $(OBJLIB) $(ARCHIVE_BUILD_DEFS)
	@mkdir -p $(PATHDEST)
	@echo Creating archive
	@rm -f $(LIBRISE_A)
	@ar rcs $(LIBRISE_A) $(OBJLIB)

rise : $(OBJLIB) $(PATHSRCS)commandconsole.o | $(PATHTOOLDEST)simulation_cache_import $(PATHTOOLDEST)rise_sim
	@echo Linking $(PATHDEST)rise
	@$(CXX)	$(CPPFLAGS) -o $(PATHDEST)rise $(PATHSRCS)commandconsole.o $(OBJLIB) $(LDLIBS)

fire-simulator : $(OBJLIB) $(PATHTOOLS)fire_simulator.o
	@mkdir -p $(PATHTOOLDEST)
	@echo Linking $(PATHTOOLDEST)fire_simulator
	@$(CXX) $(CPPFLAGS) -o $(PATHTOOLDEST)fire_simulator \
		$(PATHTOOLS)fire_simulator.o $(OBJLIB) $(LDLIBS)

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
TESTBINS = $(addprefix $(PATHTESTDEST),$(notdir $(SRCTESTS:.cpp=))) \
	$(PATHTESTDEST)FireProductionCalibrationOracle

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
$(PATHTESTS)fire_production_fp64/%.o : $(PATHTESTS)fire_production_fp64/%.cpp
	@echo Compiling strict fp64 calibration mirror: $<
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHTESTS)fire_production_trace/%.o : $(PATHTESTS)fire_production_trace/%.cpp
	@echo Compiling strict roundoff trace: $<
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHTESTS)FireProductionCalibrationTest.o : $(PATHTESTS)FireProductionCalibrationTest.cpp
	@echo Compiling strict calibration gate: $<
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHTESTS)FireSequenceTest.o : $(PATHTESTS)FireSequenceTest.cpp
	@echo Compiling strict calibration owner: $<
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

# The internal geometry-sum regression includes its strict implementation TU.
$(PATHTESTS)HyLIoSGeometrySumTest.o : $(PATHTESTS)HyLIoSGeometrySumTest.cpp $(TEST_MODE_STAMP)
	@echo "Compiling strict geometry sum regression: $<"
	@$(CXX) $(call TEST_CPPFLAGS_FOR,HyLIoSGeometrySumTest) $(filter-out -ffast-math,$(call TEST_CXXFLAGS_FOR,HyLIoSGeometrySumTest)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHTESTS)ImageReaderOverflowTest.o: CPPFLAGS += -DRISE_TEST_SOURCE_ROOT='"$(abspath ../../..)"'

$(PATHTESTS)%.o : $(PATHTESTS)%.cpp $(TEST_MODE_STAMP)
	@echo Compiling: $<
	@$(CXX) $(call TEST_CPPFLAGS_FOR,$*) $(call TEST_CXXFLAGS_FOR,$*) $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

.PHONY: check-fire-production-fp64 check-fire-production-trace
check-fire-production-fp64 : ../../../tools/generate_fire_production_fp64_mirror.py $(FIREPRODUCTIONFP64ORIGINALS) $(FIREPRODUCTIONFP64SOURCES)
	@python3 ../../../tools/generate_fire_production_fp64_mirror.py --check

check-fire-production-trace : ../../../tools/generate_fire_production_roundoff_trace.py $(FIREPRODUCTIONFP64ORIGINALS) $(FIREPRODUCTIONTRACESOURCES)
	@python3 ../../../tools/generate_fire_production_roundoff_trace.py --check

$(PATHTESTDEST)FireProductionCalibrationTest : check-fire-production-fp64 check-fire-production-trace $(PATHTESTS)FireProductionCalibrationTest.o $(FIREPRODUCTIONFP64OBJECTS) $(FIREPRODUCTIONTRACEOBJECTS) $(OBJLIB)
	@mkdir -p $(PATHTESTDEST)
	@echo Linking $@
	@$(CXX) $(CPPFLAGS) -o $@ $(PATHTESTS)FireProductionCalibrationTest.o $(FIREPRODUCTIONFP64OBJECTS) $(FIREPRODUCTIONTRACEOBJECTS) $(OBJLIB) $(LDLIBS)

$(PATHTESTDEST)FireSequenceTest : check-fire-production-fp64 check-fire-production-trace $(PATHTESTS)FireSequenceTest.o $(FIREPRODUCTIONFP64OBJECTS) $(FIREPRODUCTIONTRACEOBJECTS) $(OBJLIB) | $(PATHTOOLDEST)simulation_cache_import
	@mkdir -p $(PATHTESTDEST)
	@echo Linking $@
	@$(CXX) $(CPPFLAGS) -o $@ $(PATHTESTS)FireSequenceTest.o $(FIREPRODUCTIONFP64OBJECTS) $(FIREPRODUCTIONTRACEOBJECTS) $(OBJLIB) $(LDLIBS) $(FIREPREVIEWLIBS)

$(PATHTESTDEST)FireProductionCalibrationOracle : check-fire-production-fp64 check-fire-production-trace $(PATHTESTS)FireSequenceTest.o $(FIREPRODUCTIONFP64OBJECTS) $(FIREPRODUCTIONTRACEOBJECTS) $(OBJLIB_NOMETAL)
	@mkdir -p $(PATHTESTDEST)
	@echo Linking no-Metal calibration owner: $@
	@$(CXX) $(CPPFLAGS) -o $@ $(PATHTESTS)FireSequenceTest.o $(FIREPRODUCTIONFP64OBJECTS) $(FIREPRODUCTIONTRACEOBJECTS) $(OBJLIB_NOMETAL) $(LDLIBS_NOMETAL) $(FIREPREVIEWLIBS)
ifdef IS_MACOS_BUILD
	@if otool -L $@ | grep -q 'Metal.framework'; then \
		echo 'calibration owner unexpectedly links Metal.framework'; rm -f $@; exit 1; fi
	@if nm $@ | grep -q 'ProjectFireProductionMetalImpl'; then \
		echo 'calibration owner unexpectedly contains a production Metal implementation'; rm -f $@; exit 1; fi
endif

$(PATHTESTDEST)% : $(PATHTESTS)%.o $(LIBRISE_TEST_A) $(TEST_MODE_STAMP) | $(PATHTOOLDEST)simulation_cache_import
	@mkdir -p $(PATHTESTDEST)
	@echo Linking $@
	@$(CXX) $(call TEST_CPPFLAGS_FOR,$*) -o $@ $(PATHTESTS)$*.o $(call TEST_ARCHIVE_FOR,$*) $(LDLIBS)

# The LTO_TESTS link only the LTO library (a static pattern rule wins over
# the pattern rule above, so they do not wait on, or relink for, the test
# library).
ifneq ($(strip $(LTO_TESTS)),)
$(addprefix $(PATHTESTDEST),$(LTO_TESTS)) : $(PATHTESTDEST)% : $(PATHTESTS)%.o $(LIBRISE_A) $(TEST_MODE_STAMP) | $(PATHTOOLDEST)simulation_cache_import
	@mkdir -p $(PATHTESTDEST)
	@echo Linking $@
	@$(CXX) $(CPPFLAGS) -o $@ $(PATHTESTS)$*.o $(ARCHIVE_LINK_LIBRISE) $(LDLIBS)
endif

# Targeted diagnostic builds (for reproducible certification audits/timings).
build-tool/% : $(PATHTOOLDEST)%
	@:

$(PATHTOOLDEST)% : $(PATHTOOLS)%.o $(LIBRISE_A)
	@mkdir -p $(PATHTOOLDEST)
	@$(CXX) $(CPPFLAGS) -o $@ $(PATHTOOLS)$*.o $(ARCHIVE_LINK_LIBRISE) $(LDLIBS)

# Build all tools. Mirrors the `tests` target — same loop-and-continue
# pattern so one broken tool doesn't block the rest.
tools : $(LIBRISE_A)
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
		if ! $(CXX) $(CPPFLAGS) -o $(PATHTOOLDEST)$$tool_name $(PATHTOOLS)$$tool_name.o $(ARCHIVE_LINK_LIBRISE) $(LDLIBS); then \
			fail_count=$$((fail_count + 1)); failed_list="$$failed_list $$tool_name(link)"; \
		fi; \
	done; \
	echo "============================================================"; \
	if [ $$fail_count -ne 0 ]; then \
		echo "tools: $$fail_count tool(s) failed to build:$$failed_list"; \
		exit 1; \
	fi; \
	echo "tools: all built successfully"

# Build the library .o files and static archive needed to link tests (the
# non-LTO test copy where the platform splits them, see LIBRISE_TEST_A). Used by
# run_all_tests.sh as a one-shot library-build phase before per-test builds.
test-objs : $(LIBRISE_TEST_A)
	@:

# Build and run all tests (`test` is in TEST_GOALS, so it records the test
# link mode like `tests`).
test : tests
	@echo Running all tests
	@sh ../../../run_all_tests.sh

.SUFFIXES:
.SUFFIXES: .cpp .o

# r270 the case record derives identity-bearing floats (grid, source
# pattern, flame height).  Keep its historical fast-math codegen but out of
# link-time optimization, so edits to the executables that link it cannot
# re-optimize the derivation and silently move canonical case IDs.
$(PATHLIBRARY)Utilities/FireCase.o : $(PATHLIBRARY)Utilities/FireCase.cpp
	@echo "Compiling (no LTO): $<"
	@$(CXX) $(filter-out -flto,$(CPPFLAGS)) $(filter-out -flto,$(CXXFLAGS)) $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

# The Cycles noise, Voronoi, colour, math and vector ports reproduce Cycles'
# scalar path as strict IEEE single precision; fast-math's contraction would
# change their results (OslNoise.cpp, OslVoronoi.cpp, OslColor.cpp, OslMath.cpp
# and OslVector.cpp refuse to compile with __FAST_MATH__ defined).
# Compensated microscopic geometry must not reassociate error-free sums.
$(PATHLIBRARY)Materials/HyLIoS/HyLIoSParticleGeometry.o : $(PATHLIBRARY)Materials/HyLIoS/HyLIoSParticleGeometry.cpp
	@echo "Compiling (strict particle geometry): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

# Certified overlap bounds must retain outward-rounded operations.
$(PATHLIBRARY)Materials/HyLIoS/HyLIoSParticleOverlap.o : $(PATHLIBRARY)Materials/HyLIoS/HyLIoSParticleOverlap.cpp
	@echo "Compiling (strict overlap): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHLIBRARY)Materials/HyLIoS/HyLIoSComplexLayout.o : $(PATHLIBRARY)Materials/HyLIoS/HyLIoSComplexLayout.cpp
	@echo "Compiling (strict complex geometry): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHLIBRARY)Materials/HyLIoS/HyLIoSSurfaceBoundary.o : $(PATHLIBRARY)Materials/HyLIoS/HyLIoSSurfaceBoundary.cpp
	@echo "Compiling (strict surface boundary): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHLIBRARY)Materials/HyLIoS/HyLIoSLocalBake.o : $(PATHLIBRARY)Materials/HyLIoS/HyLIoSLocalBake.cpp
	@echo "Compiling (strict HYLIOS bake): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHLIBRARY)Materials/HyLIoS/HyLIoSSkinMaterial.o : $(PATHLIBRARY)Materials/HyLIoS/HyLIoSSkinMaterial.cpp
	@echo "Compiling (strict HYLIOS skin material): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHLIBRARY)Rendering/HyperspectralRadiometricRasterizer.o : $(PATHLIBRARY)Rendering/HyperspectralRadiometricRasterizer.cpp
	@echo "Compiling (strict hyperspectral radiometric): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

# Compensated beauty moments must not reassociate error-free transforms.
$(PATHLIBRARY)Rendering/BeautyMoments.o : $(PATHLIBRARY)Rendering/BeautyMoments.cpp
	@echo "Compiling (strict beauty moments): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHLIBRARY)Utilities/OslNoise.o : $(PATHLIBRARY)Utilities/OslNoise.cpp
	@echo "Compiling (safe fp32): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHLIBRARY)Utilities/OslVoronoi.o : $(PATHLIBRARY)Utilities/OslVoronoi.cpp
	@echo "Compiling (safe fp32): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHLIBRARY)Utilities/OslColor.o : $(PATHLIBRARY)Utilities/OslColor.cpp
	@echo "Compiling (safe fp32): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHLIBRARY)Utilities/OslMath.o : $(PATHLIBRARY)Utilities/OslMath.cpp
	@echo "Compiling (safe fp32): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHLIBRARY)Utilities/OslVector.o : $(PATHLIBRARY)Utilities/OslVector.cpp
	@echo "Compiling (safe fp32): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHLIBRARY)Utilities/OslShaderNodes.o : $(PATHLIBRARY)Utilities/OslShaderNodes.cpp
	@echo "Compiling (safe fp32): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

# r84 production remap arithmetic is a recorded binary32 operator.  It must
# not inherit the renderer's reassociation/contract flags.
$(PATHLIBRARY)Utilities/FireProductionAdvection.o : $(PATHLIBRARY)Utilities/FireProductionAdvection.cpp
	@echo "Compiling (safe fp32): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHLIBRARY)Utilities/FireProductionProjection.o : $(PATHLIBRARY)Utilities/FireProductionProjection.cpp
	@echo "Compiling (safe fp32): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHLIBRARY)Utilities/FireProductionTransport.o : $(PATHLIBRARY)Utilities/FireProductionTransport.cpp
	@echo "Compiling (safe fp32): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHLIBRARY)Utilities/FireProductionSource.o : $(PATHLIBRARY)Utilities/FireProductionSource.cpp
	@echo "Compiling (safe fp32): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHLIBRARY)Utilities/FireProductionForce.o : $(PATHLIBRARY)Utilities/FireProductionForce.cpp
	@echo "Compiling (safe fp32): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

$(PATHLIBRARY)Utilities/FireProductionAdvectionMac.o : $(PATHLIBRARY)Utilities/FireProductionAdvectionMac.mm
	@echo "Compiling (safe fp32 ObjC++): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -x objective-c++ -fobjc-arc -c $< -o $@

$(PATHLIBRARY)Utilities/FireProductionProjectionMac.o : $(PATHLIBRARY)Utilities/FireProductionProjectionMac.mm
	@echo "Compiling (safe fp32 ObjC++): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -x objective-c++ -fobjc-arc -c $< -o $@

$(PATHLIBRARY)Utilities/FireProductionForceMac.o : $(PATHLIBRARY)Utilities/FireProductionForceMac.mm
	@echo "Compiling (safe fp32 ObjC++): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -x objective-c++ -fobjc-arc -c $< -o $@

# M2 track B: the production fire driver moved out of tests/FireSequenceTest.cpp
# keeps that translation unit's flags exactly (no fast-math, no FP contraction),
# so its accepted states and frames stay byte-identical.
$(PATHLIBRARY)Simulation/Fire/FireRunCheckpoint.o : $(PATHLIBRARY)Simulation/Fire/FireRunCheckpoint.cpp
	@echo "Compiling (strict fire driver): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@
$(PATHLIBRARY)Simulation/Fire/FireRunFrames.o : $(PATHLIBRARY)Simulation/Fire/FireRunFrames.cpp
	@echo "Compiling (strict fire driver): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@
$(PATHLIBRARY)Simulation/Fire/FireRunDriver.o : $(PATHLIBRARY)Simulation/Fire/FireRunDriver.cpp
	@echo "Compiling (strict fire driver): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@
$(PATHLIBRARY)Simulation/Fire/FireRun.o : $(PATHLIBRARY)Simulation/Fire/FireRun.cpp
	@echo "Compiling (strict fire driver): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@
# M2 track C: the fire solver adapter shares the driver's frame-clock arithmetic.
$(PATHLIBRARY)Simulation/Fire/FireSolver.o : $(PATHLIBRARY)Simulation/Fire/FireSolver.cpp
	@echo "Compiling (strict fire driver): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@
# M3a: the cloth solver runs without fast-math or FP contraction, so its
# checkpoints, frames and evidence digests do not depend on the compiler's
# reassociation choices.
CLOTH_STRICT_OBJS = $(addprefix $(PATHLIBRARY)Simulation/Cloth/, ClothMaterial.o ClothLinearAlgebra.o ClothModel.o ClothParallel.o ClothContact.o ClothValidation.o ClothSolver.o)
$(CLOTH_STRICT_OBJS) : $(PATHLIBRARY)Simulation/Cloth/%.o : $(PATHLIBRARY)Simulation/Cloth/%.cpp
	@echo "Compiling (strict cloth solver): $<"
	@$(CXX) $(CPPFLAGS) $(filter-out -ffast-math,$(CXXFLAGS)) -fno-fast-math -ffp-contract=off $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

.cpp.o:
	@echo Compiling: $<
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -c $< -o $@

# Eval-harness E4: ObjC++ (.mm) compile rule for the macOS platform-TLS leg
# (TlsTransportMac.mm -- NSURLSession).  Additive: does NOT touch the .cpp.o
# rule or the -MMD/-MP .d machinery above (both are extension-generic).  Only
# reached on macOS, where Filelist gates the single .mm into SRCLIB; the Linux
# build never has a .mm in SRCLIB, so this rule is simply never invoked there.
# -x objective-c++ selects the ObjC++ front end; -fobjc-arc lets ARC own the
# Foundation object lifetimes (Foundation itself is linked via Config.OSX's
# LDLIBS -framework Foundation).
%.o : %.mm
	@echo "Compiling (ObjC++): $<"
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(CXXFLAGS_DEPS) -x objective-c++ -fobjc-arc -c $< -o $@

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
	@rm -rf $(OBJTESTS) $(PATHTESTDEST) $(PATHDEST)testobj $(PATHDEST)librise_test.a $(TEST_MODE_STAMP)
	@rm -rf $(OBJTOOLS) $(PATHTOOLDEST)
	@find $(CLEAN_OBJECT_DIRS) -type f \( -name '*.o' -o -name '*.d' \) -exec rm -f {} \;
	@rm -rf $(PATHSRCS)ii_files
	@rm -f $(RENDERER_BUILD_HEADER)
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
# changed.  Test and tool binaries link a library archive + the per-binary .o (and
# standalone binaries link $(OBJLIB) + the per-binary .o), and compile/link are
# separate rules, so such
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
