//////////////////////////////////////////////////////////////////////
//
//  IRasterImageWriter.h - Interface to a raster image writer, which is an
//  object capable to writing a raster image somewhere.
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

#ifndef IRASTERIMAGEWRITER_
#define IRASTERIMAGEWRITER_

#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/Color_Template.h"
#include "../Utilities/PEL.h"

namespace RISE
{
	//! To flush a raster image to a buffer
	class IRasterImageWriter : public virtual IReference
	{
	protected:
		IRasterImageWriter( ){};

	public:
		virtual ~IRasterImageWriter( ){};

		//! Begins the write operation
		virtual void BeginWrite( 
			const unsigned int width,					///< [in] Width of image
			const unsigned int height					///< [in] Height of image
			) = 0;

		//! Writes the next color is a series of colors
		virtual void WriteColor( 
			const RISEColor& c,							///< [in] Color to be written
			const unsigned int x,						///< [in] X co-ordinate of color
			const unsigned int y						///< [in] Y co-ordinate of color
			) = 0;

		//! Ends the write operation
		virtual void EndWrite( ) = 0;
	};
}

#endif
