//////////////////////////////////////////////////////////////////////
//
//  ScatterPainter.h - Texture-bombing painter: stamps `source` over
//  `background` on a jittered square UV lattice (`ptCoord * cell_scale`).
//
//  Algorithm (UV-only; wrap in mapping_painter for triplanar/world-space):
//    1. Each lattice cell rolls `probability` and draws jittered center
//       (stays inside its owning cell via +/- 0.5 * jitter_position),
//       rotation (+/- jitter_rotation deg), and scale in
//       stamp_scale * [1 - jitter_scale, 1 + jitter_scale] via
//       NoiseCore::WorleyHashCell.
//    2. FindStamp() searches the 3x3 cell neighbourhood. Parse-time check
//       `stamp_scale * (1 + jitter_scale) <= sqrt(2)` bounds the
//       circumradius to <= 1 cell, guaranteeing 3x3 is sufficient.
//    3. Nearest covering stamp center wins (row-major tie-break).
//    4. Straight-alpha Porter-Duff "over" compositing (Porter & Duff 1984):
//           out_a     = fg_a + bg_a * (1 - fg_a)
//           out_color = (fg * fg_a + bg * bg_a * (1 - fg_a)) / out_a
//
//  Invariants:
//    - All Get* methods share Resolve() so RGB/spectral/alpha agree on
//      the winning stamp; txFootprint is invalidated on the stamp-local ri2.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-08-20
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SCATTER_PAINTER_
#define SCATTER_PAINTER_

#include "Painter.h"
#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	namespace Implementation
	{
		class ScatterPainter : public Painter
		{
		protected:
			const IPainter&			source;			// the stamp
			const IPainter&			background;
			const Scalar				cellScale;
			const Scalar				stampScale;
			const Scalar				jitterPosition;
			const Scalar				jitterRotationDeg;
			const Scalar				jitterScale;
			const Scalar				probability;
			const unsigned int			seed;

			virtual ~ScatterPainter();

			//! 3x3 neighbourhood search for the nearest covering stamp; outputs
			//! localUV in [0,1]^2 on true.
			bool FindStamp( const Point2& coord, Point2& localUV ) const;

			//! Shared per-hit stamp resolution (ptCoord = localUV, txFootprint
			//! invalidated when found == true).
			struct HitInfo
			{
				bool						found;
				Scalar						alpha;
				RayIntersectionGeometric	ri2;
				explicit HitInfo( const RayIntersectionGeometric& base ) : found( false ), alpha( 0 ), ri2( base ) {}
			};
			HitInfo Resolve( const RayIntersectionGeometric& ri ) const;

		public:
			ScatterPainter(
				const IPainter&		source_,
				const IPainter&		background_,
				const Scalar		cellScale_,
				const Scalar		stampScale_,
				const Scalar		jitterPosition_,
				const Scalar		jitterRotationDeg_,
				const Scalar		jitterScale_,
				const Scalar		probability_,
				const unsigned int	seed_ );

			ScatterPainter( const ScatterPainter& ) = delete;
			ScatterPainter& operator=( const ScatterPainter& ) = delete;

			RISEPel			GetColor( const RayIntersectionGeometric& ri ) const;
			Scalar			GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;
			//! Coverage-weighted forward to source/background GetRadianceNM (preserves physical SPDs).
			Scalar			GetRadianceNM( const RayIntersectionGeometric& ri, const Scalar nm ) const;
			RISEPel			GetRadianceColor( const RayIntersectionGeometric& ri ) const;
			SpectralPacket	GetSpectrum( const RayIntersectionGeometric& ri ) const;
			Scalar			GetAlpha( const RayIntersectionGeometric& ri ) const;

			IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
			void SetIntermediateValue( const IKeyframeParameter& ) {}
			void RegenerateData() {}
			bool IsSpectrallyDefined() const { return source.IsSpectrallyDefined() || background.IsSpectrallyDefined(); }
			unsigned int GetPathDependence() const { return source.GetPathDependence() | background.GetPathDependence(); }
		};
	}
}

#endif
