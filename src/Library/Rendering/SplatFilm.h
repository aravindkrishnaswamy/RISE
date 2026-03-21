//////////////////////////////////////////////////////////////////////
//
//  SplatFilm.h - Thread-safe film buffer for accumulating splat
//  contributions from s<=1 BDPT strategies.  Uses row-level
//  mutexes so that multiple threads can splat to different
//  scanlines concurrently.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SPLAT_FILM_
#define SPLAT_FILM_

#include "../Interfaces/IRasterImage.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/Color_Template.h"
#include "../Utilities/Threads/Threads.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class SplatFilm : public virtual Reference
		{
		protected:

			struct SplatPixel
			{
				RISEPel		color;
				Scalar		weight;

				SplatPixel() :
				color( RISEPel( 0, 0, 0 ) ),
				weight( 0 )
				{
				}
			};

			unsigned int				width;
			unsigned int				height;
			std::vector<SplatPixel>		pixels;
			std::vector<RMutex*>		rowMutexes;		///< One mutex per scanline for concurrent access

			virtual ~SplatFilm();

		public:
			SplatFilm(
				const unsigned int w,					///< [in] Width of the film in pixels
				const unsigned int h					///< [in] Height of the film in pixels
				);

			//! Thread-safe: adds a contribution to the pixel at (x,y)
			void Splat(
				const unsigned int x,					///< [in] X co-ordinate of pixel
				const unsigned int y,					///< [in] Scanline of pixel
				const RISEPel& contribution				///< [in] Color contribution to accumulate
				);

			//! After rendering: resolves accumulated splats into the final image.
			//! Divides each pixel's accumulated color by the total number of samples.
			void Resolve(
				IRasterImage& target,					///< [in/out] Target image to receive resolved splats
				const Scalar sampleCount				///< [in] Total number of samples taken
				) const;

			//! Clears all accumulated splat data
			void Clear();
		};
	}
}

#endif
