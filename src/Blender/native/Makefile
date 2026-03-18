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

all: $(TARGET)

$(TARGET): $(OBJS) ../../../bin/librise.a
	@echo Linking $@
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) $(SHARED_FLAGS) -o $@ $(OBJS) ../../../bin/librise.a $(LDLIBS)

.SUFFIXES:
.SUFFIXES: .cpp .o

.cpp.o:
	@echo Compiling: $<
	@$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXARCHFLAGS) $(CXXLIBSETTINGS) $(DEFS) -c $< -o $@

clean:
	@rm -f $(OBJS) $(TARGET)

