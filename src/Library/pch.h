//////////////////////////////////////////////////////////////////////
//
//  pch.h - Precompiled header for RISE
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 8, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_LIBRARY_PCH_
#define RISE_LIBRARY_PCH_
#ifdef USE_PRECOMPILED_HEADER_

//
// The precompiled header is orgainzed
//


//
// Platform and compiler dependent options
// In case of the Win32 and MSVC, we turn off
// some idiotic warnings that VC gives us
//
#ifdef WIN32
#pragma warning( disable : 4512 )		// disables warning about not being able to generate an assignment operator (.NET 2003)
#pragma warning( disable : 4250 )		// disables silly virtual inheritance warning
#pragma warning( disable : 4344 )		// disables warning about explicit template argument passed to template function

#define WIN32_LEAN_AND_MEAN		// Exclude rarely-used stuff from Windows headers
#define _WIN32_WINNT 0x0400		// require NT4 or greater for TryEnterCriticalSection
#include <windows.h>

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
#include <float.h>

#ifndef NO_PNG_SUPPORT
	#include <png.h>
#endif

#ifndef NO_TIFF_SUPPORT
	#include <tiffio.h>
#endif

// STL stuff
#include <algorithm>
#include <deque>
#include <functional>
#include <vector>
#include <list>
#include <map>


#include "Version.h"				// <--  Very important!


//
// Commonly used RISE interfaces
//
#include "Interfaces/IFunction1D.h"
#include "Interfaces/IFunction2D.h"
#include "Interfaces/IGeometry.h"
#include "Interfaces/ILog.h"
#include "Interfaces/ILogPriv.h"
#include "Interfaces/IReference.h"
#include "Interfaces/IRasterImage.h"


//
// All essential RISE utilitary stuff
//
#include "Utilities/Math3D/Math3D.h"
#include "Utilities/Color/Color.h"
#include "Utilities/Color/Color_Template.h"
#include "Utilities/Color/ColorUtils.h"
#include "Utilities/Threads/Threads.h"
#include "Utilities/BoundingBox.h"
#include "Utilities/Optics.h"
#include "Utilities/OrthonormalBasis3D.h"
#include "Utilities/RandomNumbers.h"
#include "Utilities/Reference.h"
#include "Utilities/RString.h"
#include "Utilities/stl_utils.h"


//
// Functions
//
#include "Functions/ConstantFunctions.h"

//
// Geometry
//
#include "Polygon.h"
#include "Intersection/RayIntersection.h"

#endif // USE_PRECOMPILED_HEADER_
#endif // RISE_LIBRARY_PCH_



