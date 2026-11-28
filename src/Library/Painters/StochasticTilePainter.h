//////////////////////////////////////////////////////////////////////
//
//  StochasticTilePainter.h - Hex-tiling with histogram-preserving
//  blending over a tileable source painter (Heitz & Neyret 2018;
//  Burley 2019; default blend_gamma = 7.0 per Mikkelsen 2022).
//
//  Pipeline (UV-only via ri.ptCoord; wrap in mapping_painter for
//  world-space/triplanar tiling):
//    1. Map ptCoord * tile_scale onto an equilateral-triangle lattice
//       (dual is hexagonal) and compute 3 vertex IDs + barycentric weights.
//    2. Hash each vertex ID (NoiseCore::WorleyHashCell) to a UV offset
//       in [0,1)^2 (offset-only, no rotation).
//    3. Sharpen weights: w_i' = w_i^blend_gamma, normalized to sum 1.
//    4. Variance-preserving blend around parser-supplied `mean` (mu):
//           out = mu + (sum_i w_i' * (x_i - mu)) / sqrt(sum_i w_i'^2)
//       Because sum w_i' == 1 and w_i' >= 0, sqrt(sum w_i'^2) lies in
//       [1/sqrt(3), 1] (no zero-division guard needed).
//
//  Invariants:
//    - GetColor, GetColorNM, GetRadiance*, GetSpectrum, and GetAlpha all
//      share ComputeHexTiling() so RGB/spectral/alpha paths stay synced.
//    - `mean` is uplifted once at construction (albedo + illuminant).
//    - GetAlpha uses a plain convex blend (no variance restore) to stay
//      within [0,1].
//    - txFootprint is invalidated on offset sample copies.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-08-20
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef STOCHASTIC_TILE_PAINTER_
#define STOCHASTIC_TILE_PAINTER_

#include "Painter.h"
#include "../Utilities/Color/RGBSpectra.h"
#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	namespace Implementation
	{
		class StochasticTilePainter : public Painter
		{
		protected:
			const IPainter&			source;
			const Scalar				tileScale;
			const unsigned int			seed;
			const RISEPel				mean;
			const RGBAlbedoSpectrum		meanSpec;		//!< mean uplifted once (albedo, for GetColorNM)
			const RGBIlluminantSpectrum	meanRadSpec;	//!< mean uplifted once (illuminant, for GetRadianceNM)
			const RISEPel				meanRad;		//!< non-negative RGB twin of meanRadSpec (for GetRadianceColor)
			const Scalar				blendGamma;

			virtual ~StochasticTilePainter();

			//! Computes the 3 sharpened, normalized barycentric weights (sum == 1)
			//! and hash-offset sample UVs for `coord`.
			void ComputeHexTiling( const Point2& coord, Scalar w[3], Point2 offsetUV[3] ) const;

		public:
			StochasticTilePainter(
				const IPainter&		source_,
				const Scalar		tileScale_,
				const unsigned int	seed_,
				const RISEPel&		mean_,
				const Scalar		blendGamma_ );

			StochasticTilePainter( const StochasticTilePainter& ) = delete;
			StochasticTilePainter& operator=( const StochasticTilePainter& ) = delete;

			RISEPel			GetColor( const RayIntersectionGeometric& ri ) const;
			Scalar			GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;
			//! Forwards directly to source.GetRadianceNM (about meanRadSpec) to preserve physical SPDs.
			Scalar			GetRadianceNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;
			RISEPel			GetRadianceColor( const RayIntersectionGeometric& ri ) const;
			SpectralPacket	GetSpectrum( const RayIntersectionGeometric& ri ) const;
			Scalar			GetAlpha( const RayIntersectionGeometric& ri ) const;

			IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
			void SetIntermediateValue( const IKeyframeParameter& ) {}
			void RegenerateData() {}
			bool IsSpectrallyDefined() const { return source.IsSpectrallyDefined(); }
			unsigned int GetPathDependence() const { return source.GetPathDependence(); }
		};
	}
}

#endif
