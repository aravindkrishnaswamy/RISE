//////////////////////////////////////////////////////////////////////
//
//  PEL.h - Defines an interface for a pixel element
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 24, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef _PEL_
#define _PEL_

namespace RISE
{
	template< typename T >
	struct RGBA_T
	{
		T r, g, b, a;
	};

	typedef RGBA_T<unsigned char> RGBA8;
	typedef RGBA_T<unsigned short> RGBA16;
}

#endif
