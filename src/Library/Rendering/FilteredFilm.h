//////////////////////////////////////////////////////////////////////
//
//  FilteredFilm.h - Thread-safe film buffer for accumulating
//  filter-weighted sample contributions across pixel boundaries.
//
//  Unlike SplatFilm (which accumulates unit-weighted contributions
//  at arbitrary pixel locations), FilteredFilm evaluates the pixel
//  filter at each receiving pixel's offset from the sample position.
//  This correctly handles reconstruction filters with negative lobes
//  (e.g. Mitchell-Netravali, Lanczos) by spreading each sample's
//  contribution to all pixels within the filter's support.
//
//  After the render pass, Resolve() computes the final pixel value
//  as colorSum / weightSum, which is the proper filtered result.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 10, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FILTERED_FILM_
#define FILTERED_FILM_

#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IPixelFilter.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/Color_Template.h"
#include "../Utilities/Threads/Threads.h"
#include <vector>
#include <cmath>

namespace RISE
{
	namespace Implementation
	{
		class FilteredFilm : public virtual Reference
		{
		protected:

			struct FilteredPixel
			{
				RISEPel		colorSum;
				Scalar		weightSum;

				FilteredPixel() :
				colorSum( RISEPel( 0, 0, 0 ) ),
				weightSum( 0 )
				{
				}
			};

			unsigned int				width;
			unsigned int				height;
			std::vector<FilteredPixel>	pixels;
			std::vector<RMutex*>		rowMutexes;		///< One mutex per scanline for concurrent access

			virtual ~FilteredFilm();

		public:
			FilteredFilm(
				const unsigned int w,					///< [in] Width of the film in pixels
				const unsigned int h					///< [in] Height of the film in pixels
				);

			//! Thread-safe: splats a sample contribution to all pixels within
			//! the filter's support around the given screen position.
			void Splat(
				const Scalar screenX,					///< [in] Sample screen X position
				const Scalar screenY,					///< [in] Sample screen Y position
				const RISEPel& color,					///< [in] Sample radiance
				const IPixelFilter& filter				///< [in] Pixel filter for weight evaluation
				);

			//! After rendering: resolves accumulated contributions into the final image.
			//! Each pixel's value is colorSum / weightSum.
			void Resolve(
				IRasterImage& target					///< [out] Target image to receive resolved result
				) const;

			//! Subtracts previously resolved values from the image (inverse of Resolve).
			//! Used for progressive display.
			void Unresolve(
				IRasterImage& target					///< [in/out] Target image to undo resolved values
				) const;

			//! Clears all accumulated data
			void Clear();
		};
	}
}

#endif
