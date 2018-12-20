//////////////////////////////////////////////////////////////////////
//
//  IRasterImage.h - Interface of a raster image
//                
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 9, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IRASTERIMAGE_
#define IRASTERIMAGE_

#include "IReference.h"
#include "IRasterImageReader.h"
#include "IRasterImageWriter.h"
#include <stdlib.h>

namespace RISE
{
	//! Describes a rectangle on a raster image
	struct Rect
	{
		unsigned int top, left, bottom, right;
		Rect( unsigned int t, unsigned int l, unsigned int b, unsigned int r ) : 
		top( t ), left( l ), bottom( b ), right( r ) {}
	};

	//! A basic raster image, or an image or an array of pixels
	class IRasterImage : public virtual IReference
	{
	protected:
		IRasterImage(){};

	public:
		virtual ~IRasterImage(){};

		/// \return The PEL
		virtual	RISEColor GetPEL( 
			const unsigned int x,								///< [in] X co-ordinate of pixel
			const unsigned int y								///< [in] Scanline of pixel
			) const = 0;

		//! Sets the PEL
		virtual	void SetPEL( 
			const unsigned int x,								///< [in] X co-ordinate of pixel
			const unsigned int y,								///< [in] Scanline of pixel
			const RISEColor& p									///< [in] Color to set pixel to
			) = 0;

		//! Clears the raster image
		virtual	void Clear( 
			const RISEColor& c,									///< [in] Color to clear raster image to
			const Rect* rc										///< [in] Region to clear
			) = 0;

		//! Dumps the raster image
		virtual void DumpImage( 
			IRasterImageWriter* pWriter							///< [in] Writer to write out to
			) const = 0;

		//! Loads an image from the reader
		virtual void LoadImage( 
			IRasterImageReader* pReader							///< [in] Reader to read from
			) = 0;

		/// \return The width of the raster image in pixels
		virtual unsigned int GetWidth() const = 0;

		/// \return The height of the raster image in pixels
		virtual unsigned int GetHeight() const = 0;
	};
}

#endif
