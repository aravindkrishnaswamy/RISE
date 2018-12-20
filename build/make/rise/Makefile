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
	@rm -rf $(PATHDEST)rise $(PATHDEST)risempi $(PATHDEST)librise $(OBJS) $(OBJMPI) $(OBJLIB) 
	@rm -rf $(OBJDRISE) $(PATHSRCSDRISE)drise_server.o $(PATHSRCSDRISE)drise_client.o $(PATHSRCSDRISE)drise_submitter.o
	@rm -rf $(PATHSRCS)commandconsole.o $(PATHSRCS)meshconverter.o $(PATHSRCS)imageconverter.o
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
