//////////////////////////////////////////////////////////////////////
//
//  SplatFilm.h - Thread-safe film buffer for accumulating
//  contributions that land at arbitrary pixel positions.
//
//  In BDPT, connection strategies where t==1 (light subpath
//  connects to camera) produce contributions at pixel positions
//  determined by projecting the light vertex onto the camera,
//  NOT the pixel currently being rendered.  These cannot be added
//  to the primary image during the per-pixel render pass because
//  the target pixel may be processed by a different thread.
//
//  SplatFilm provides a separate accumulation buffer with row-level
//  mutex locking so multiple render threads can splat concurrently.
//  After the render pass completes, Resolve() divides by the total
//  sample count and adds the result to the primary image.
//
//  Also used by MLTRasterizer for all contributions (since MLT
//  paths land at arbitrary pixels determined by the sample vector).
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
#include <cstdint>
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

			//! Batched commit.  Records must be sorted by pixelIndex
			//! (y*width + x).  Acquires each row's mutex exactly once.
			//! Used by ThreadLocalSplatBuffer to flush a whole tile
			//! of collected splats without per-splat mutex overhead.
			struct BatchRecord
			{
				uint32_t	pixelIndex;
				RISEPel		color;
			};
			void BatchCommit(
				const BatchRecord* records,
				std::size_t count
				);

			//! After rendering: resolves accumulated splats into the final image.
			//! Divides each pixel's accumulated color by the total number of samples.
			void Resolve(
				IRasterImage& target,					///< [in/out] Target image to receive resolved splats
				const Scalar sampleCount				///< [in] Total number of samples taken
				) const;

			//! Subtracts previously resolved splats from the image (inverse of Resolve).
			//! Used for progressive display: Resolve before intermediate output,
			//! Unresolve afterward to avoid double-counting.
			void Unresolve(
				IRasterImage& target,					///< [in/out] Target image to undo resolved splats
				const Scalar sampleCount				///< [in] Same sample count used in Resolve
				) const;

			//! Clears all accumulated splat data
			void Clear();

			//! Flush the calling thread's per-thread splat buffer (if
			//! it's bound to this film) into the shared accumulator.
			//! Called at tile boundaries and at end-of-pass to make
			//! sure no splats are orphaned in thread_local storage.
			void FlushCallingThreadBuffer();
		};
	}
}

#endif
