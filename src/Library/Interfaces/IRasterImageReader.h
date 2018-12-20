//////////////////////////////////////////////////////////////////////
//
//  IRasterImageReader.h - Interface to a rasterimage reader, which is an
//  object capable to feeding data to a raster image
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

#ifndef IRASTERIMAGEREADER_
#define IRASTERIMAGEREADER_

#include "IReference.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/Color_Template.h"
#include "../Utilities/PEL.h"

namespace RISE
{
	//! To restore a raster image from a buffer
	class IRasterImageReader : public virtual IReference
	{
	protected:
		IRasterImageReader( ){};

	public:
		virtual ~IRasterImageReader( ){};

		//! Begins the read operation
		/// \return TRUE if reading has successfully begun, FALSE otherwise
		virtual bool BeginRead(
			unsigned int& width,								///< [out] Width of the image
			unsigned int& height								///< [out] Height of the image
			) = 0;

		//! Gets the next Color in a series of Colors
		virtual void ReadColor(	
			RISEColor& c,										///< [out] Color
			const unsigned int x,								///< [in] X co-ordinate of color
			const unsigned int y								///< [in] Y co-ordinate of color
			) = 0;

		//! Ends the read operation
		virtual void EndRead( ) = 0;
	};
}

#endif
