//////////////////////////////////////////////////////////////////////
//
//  ProgressiveFilm.h - Per-pixel accumulation buffer for multi-pass
//    progressive rendering.  Stores running color sums, weights, and
//    Welford online variance state so that IntegratePixel can resume
//    across passes and converged pixels can be skipped.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 10, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PROGRESSIVE_FILM_
#define PROGRESSIVE_FILM_

#include "../Interfaces/IReference.h"
#include <vector>
#include <cstdint>

namespace RISE
{
	namespace Implementation
	{
		struct ProgressivePixel
		{
			RISEPel		colorSum;		///< Weighted color accumulator
			Scalar		weightSum;		///< Weight accumulator (for averaging)
			Scalar		alphaSum;		///< Alpha accumulator

			// Welford online variance state (luminance-based)
			Scalar		wMean;
			Scalar		wM2;
			uint32_t	wN;				///< Total sample count across all passes

			uint32_t	sampleIndex;	///< Next globalSampleIndex for Sobol continuity
			bool		converged;		///< Pixel has met convergence criterion

			ProgressivePixel() :
			  colorSum( 0, 0, 0 ),
			  weightSum( 0 ),
			  alphaSum( 0 ),
			  wMean( 0 ),
			  wM2( 0 ),
			  wN( 0 ),
			  sampleIndex( 0 ),
			  converged( false )
			{}
		};

		class ProgressiveFilm
		{
			std::vector<ProgressivePixel> pixels;
			unsigned int width;
			unsigned int height;

		public:
			ProgressiveFilm(
				const unsigned int w,
				const unsigned int h
				) :
			  width( w ),
			  height( h )
			{
				pixels.resize( w * h );
			}

			ProgressivePixel& Get( const unsigned int x, const unsigned int y )
			{
				return pixels[y * width + x];
			}

			const ProgressivePixel& Get( const unsigned int x, const unsigned int y ) const
			{
				return pixels[y * width + x];
			}

			bool IsConverged( const unsigned int x, const unsigned int y ) const
			{
				return pixels[y * width + x].converged;
			}

			/// Resolve the current progressive state into a display image.
			/// Writes colorSum/weightSum as the running average for each pixel.
			void Resolve( IRasterImage& target ) const
			{
				for( unsigned int y = 0; y < height; y++ ) {
					for( unsigned int x = 0; x < width; x++ ) {
						const ProgressivePixel& px = pixels[y * width + x];
						if( px.weightSum > 0 && px.alphaSum > 0 ) {
							const RISEPel avg = px.colorSum * (1.0 / px.alphaSum);
							target.SetPEL( x, y, RISEColor( avg, px.alphaSum / px.weightSum ) );
						}
					}
				}
			}

			bool IsPixelDone(
				const ProgressivePixel& px,
				const unsigned int targetSamples
				) const
			{
				return px.converged || (targetSamples > 0 && px.sampleIndex >= targetSamples);
			}

			/// Count the number of pixels that have either converged or hit
			/// the progressive sample budget.
			unsigned int CountDone( const unsigned int targetSamples ) const
			{
				unsigned int count = 0;
				for( size_t i = 0; i < pixels.size(); i++ ) {
					if( IsPixelDone( pixels[i], targetSamples ) ) count++;
				}
				return count;
			}

			/// Check if all pixels within a rectangular region have either
			/// converged or hit the progressive sample budget.
			bool IsTileDone( const Rect& rect, const unsigned int targetSamples ) const
			{
				for( unsigned int y = rect.top; y <= rect.bottom; y++ ) {
					for( unsigned int x = rect.left; x <= rect.right; x++ ) {
						if( !IsPixelDone( pixels[y * width + x], targetSamples ) ) {
							return false;
						}
					}
				}
				return true;
			}

			void Clear()
			{
				for( size_t i = 0; i < pixels.size(); i++ ) {
					pixels[i] = ProgressivePixel();
				}
			}
		};
	}
}

#endif
