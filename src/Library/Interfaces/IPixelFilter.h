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

		//! Evaluates the filter at an offset (dx, dy) from the pixel center.
		//! Used by film-based reconstruction to splat contributions to
		//! neighboring pixels within the filter's support.
		/// \return The filter weight at the given offset
		virtual Scalar EvaluateFilter(
			const Scalar dx,					///< [in] Horizontal offset from pixel center
			const Scalar dy						///< [in] Vertical offset from pixel center
			) const { return 0; }

		//! Returns the half-width and half-height of the filter's support
		//! region in pixels.  A filter with support [-2,2] returns 2.0.
		virtual void GetFilterSupport(
			Scalar& halfWidth,					///< [out] Half-width of support in pixels
			Scalar& halfHeight					///< [out] Half-height of support in pixels
			) const { halfWidth = 0.5; halfHeight = 0.5; }
	};
}

#endif
