include ../../../build/make/rise/Config.common
include ../../../build/make/rise/Config.specific

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
SHARED_EXT = dylib
SHARED_FLAGS = -dynamiclib
else
SHARED_EXT = so
SHARED_FLAGS = -shared
CXXFLAGS += -fPIC
endif

TARGET = rise_blender_bridge.$(SHARED_EXT)
SRCS = rise_blender_bridge.cpp
OBJS = $(SRCS:.cpp=.o)

CPPFLAGS += -I.
CXXFLAGS += -std=c++17

OPENVDB_ROOT ?=
TBB_ROOT ?=

ifeq ($(OPENVDB_ROOT),)
ifneq ($(wildcard /opt/homebrew/opt/openvdb/include/openvdb/openvdb.h),)
OPENVDB_ROOT := /opt/homebrew/opt/openvdb
else ifneq ($(wildcard $(HOME)/Development/blender-git/blender/lib/macos_arm64/openvdb/include/openvdb/openvdb.h),)
OPENVDB_ROOT := $(HOME)/Development/blender-git/blender/lib/macos_arm64/openvdb
endif
endif

ifneq ($(OPENVDB_ROOT),)
ifeq ($(TBB_ROOT),)
ifneq ($(wildcard /opt/homebrew/opt/tbb/lib/libtbb.dylib),)
TBB_ROOT := /opt/homebrew/opt/tbb
else ifneq ($(wildcard $(HOME)/Development/blender-git/blender/lib/macos_arm64/tbb/lib/libtbb.dylib),)
TBB_ROOT := $(HOME)/Development/blender-git/blender/lib/macos_arm64/tbb
endif
endif

CPPFLAGS += -DRISE_BLENDER_ENABLE_OPENVDB -I$(OPENVDB_ROOT)/include
LDLIBS += -L$(OPENVDB_ROOT)/lib -Wl,-rpath,$(OPENVDB_ROOT)/lib -lopenvdb
ifneq ($(TBB_ROOT),)
LDLIBS += -L$(TBB_ROOT)/lib -Wl,-rpath,$(TBB_ROOT)/lib -ltbb
endif
endif

all: $(TARGET)

.PHONY: rise_archive

rise_archive:
	@$(MAKE) -C ../../../build/make/rise librise

$(TARGET): rise_archive $(OBJS) ../../../bin/librise.a
	@echo Linking $@
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(SHARED_FLAGS) -o $@ $(OBJS) ../../../bin/librise.a $(LDLIBS)

.SUFFIXES:
.SUFFIXES: .cpp .o

.cpp.o:
	@echo Compiling: $<
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) -c $< -o $@

clean:
	@rm -f $(OBJS) $(TARGET)
