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
#include "../Utilities/Color/ColorUtils.h"
#include <vector>
#include <cstdint>

namespace RISE
{
	namespace Implementation
	{
		// Experiment with alignas(64) to kill false sharing was
		// found to REGRESS performance significantly (VCM: 28s →
		// 46s, PT: 9.3s → 13.9s) — likely because the extra 8
		// bytes per pixel pushes working set beyond L2 for typical
		// scenes, while the actual false-sharing was lower than
		// predicted since Morton-ordered tiles already keep threads
		// on separate regions.  Leave natural layout.
		struct ProgressivePixel
		{
			// PBRT-v4 RGBFilm-style storage: per-pixel accumulator is in
			// CIE XYZ tristimulus (a linear color space, no gamut),
			// converted to RISEPel ROMM RGB exactly once at Resolve.
			// Eliminates the per-sample chromaticity gamut clip
			// (MoveXYZIntoROMMRGBGamut, fired by the implicit
			// RISEPel(XYZPel) conversion) that systematically suppressed
			// out-of-gamut spectral-locus contributions toward white.
			// For RGB rasterizers: implicit XYZPel(const ROMMRGBPel&)
			// at write is a lossless 3x3 matrix multiply (no clip);
			// implicit RISEPel(const XYZPel&) at read does fire the
			// chromaticity clip, but for in-gamut RGB samples the clip
			// is a no-op, so RGB rasterizers behave identically.
			XYZPel		colorSum;		///< Weighted XYZ accumulator (linear, no clip)
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
			/// XYZ -> RISEPel conversion (standard `ColorUtils::XYZtoRec709RGB`
			/// post Stage-B colour-space migration: D65→D65, matrix-only, no
			/// Bradford adapt) happens here, exactly once per pixel.  The
			/// implicit RISEPel(XYZPel) constructor in RISEColor's argument
			/// list is the dispatch point.  Earlier ROMM-era revisions
			/// dispatched to a matrix-only `IntegratorXYZtoROMMRGB` for
			/// spectral rasterizers; that path was eliminated by
			/// Stage A of the colour-space migration (`IntegratorXYZto*`
			/// no longer exists).
			///
			/// When `showMap=true && targetSamples>0`, writes a grayscale
			/// heatmap of `sampleIndex / targetSamples` in place of beauty.
			/// This is the **only** place adaptive-sample-map visualisation
			/// can take effect in progressive-rendering mode — the per-pixel
			/// `cret` return path in IntegratePixel is overwritten by this
			/// resolve step, so the heatmap must be re-derived from the
			/// progressive-film state here.
			void Resolve( IRasterImage& target, bool showMap = false, unsigned int targetSamples = 0 ) const
			{
				const bool emitMap = showMap && targetSamples > 0;
				const Scalar invTarget = emitMap ? 1.0 / Scalar( targetSamples ) : 0.0;
				for( unsigned int y = 0; y < height; y++ ) {
					for( unsigned int x = 0; x < width; x++ ) {
						const ProgressivePixel& px = pixels[y * width + x];
						if( emitMap ) {
							const Scalar t = Scalar( px.sampleIndex ) * invTarget;
							target.SetPEL( x, y, RISEColor( RISEPel( t, t, t ), 1.0 ) );
						} else if( px.weightSum > 0 && px.alphaSum > 0 ) {
							const XYZPel avgXYZ = px.colorSum * (1.0 / px.alphaSum);
							target.SetPEL( x, y, RISEColor( RISEPel( avgXYZ ), px.alphaSum / px.weightSum ) );
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
