//////////////////////////////////////////////////////////////////////
//
//  pch.h - Precompiled header for DRISE
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 24, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DRISE_PCH_
#define DRISE_PCH_
#ifdef USE_PRECOMPILED_HEADER_

//
// The precompiled header is orgainzed
//


//
// Platform and compiler dependent options
// In case of the Win32 and MSVC, we turn off
// some idiotic warnings that VC6 gives us
//
#ifdef WIN32
#pragma warning( disable : 4503 )		// disables warning about decorated names being truncated
#pragma warning( disable : 4786 )		// disables warning about 255 truncation in PDB
#pragma warning( disable : 4250 )		// disables silly virtual inheritance warning
#endif

//
// First all the platform independent system files
//
#include <iostream>
#include <fstream>
#include <math.h>
#include <memory.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

// STL stuff
#include <algorithm>
#include <deque>
#include <functional>
#include <vector>
#include <list>
#include <map>


#include "../Library/RISE_API.h"
#include "../Library/Interfaces/IReference.h"

//
// All essential RISE utilitary stuff
//
#include "../Library/Utilities/Threads/Threads.h"
#include "../Library/Utilities/Communications/ClientSocketCommunicator.h"


#endif // USE_PRECOMPILED_HEADER_
#endif // DRISE_PCH_



