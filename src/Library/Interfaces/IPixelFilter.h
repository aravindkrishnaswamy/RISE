//////////////////////////////////////////////////////////////////////
//
//  IPixelFilter.h - Interface for pixel filters.  Pixel filters 
//  are what samplers will call with canonical samples, in return
//  the filter will return where within the pixel to take
//  a particular sample.  
//
//  NOTE: Pixel filters assume their input (usually from ISampling2D)
//  object produces samples in a canonical space [0,1].
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 4, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef IPIXELFILTER_
#define IPIXELFILTER_

#include "IReference.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/RandomNumbers.h"

namespace RISE
{
	//! A pixel filter warps samples in a canonical (box) filter 
	//! to samples in various other filter geometries
	class IPixelFilter : public virtual IReference
	{
	protected:
		IPixelFilter(){};
		virtual ~IPixelFilter(){};

	public:
		//! Warps some 2D value
		/// \return The weight of the warped sample
		virtual Scalar warp( 
			const RandomNumberGenerator& random,///< [in] Random number generator
			const Point2& canonical,			///< [in] Value to warp
			Point2& warped						///< [in] Warped value
			) const = 0;

		//! Warps some value as a point on a virtual screen
		/// \return The weight of the warped sample
		virtual Scalar warpOnScreen( 
			const RandomNumberGenerator& random,///< [in] Random number generator
			const Point2& canonical,			///< [in] Value to warp
			Point2& warped,						///< [in] Warped value
			const unsigned int x,				///< [in] Screen x value
			const unsigned int y				///< [in] Screen y value
			) const = 0;
	};
}

#endif
