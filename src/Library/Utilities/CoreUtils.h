//////////////////////////////////////////////////////////////////////
//
//  CoreUtils.h - Core utilities, this is stuff I kinda didn't know
//  where to put
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 9, 2002
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef STR_CONCAT
	#ifndef STR_CONCAT_HELPER
		#define STR_CONCAT_HELPER(a,b) a ## b
	#endif

	#define STR_CONCAT(a,b) STR_CONCAT_HELPER(a,b)
#endif

#ifndef MAKECOLORSTR
	#define MAKECOLORSTR( x ) STR_CONCAT( COLORSPACE_, x )
#endif

#ifdef COLORS_RGB
	#define COLORSPACE RGB
#elif COLORS_XYZ
	#define COLORSPACE XYZ
#else
	#define COLORSPACE RGB
#endif

#define COLORSPACE_RGB 1
#define COLORSPACE_XYZ 2

