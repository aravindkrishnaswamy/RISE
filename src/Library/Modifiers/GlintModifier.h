//////////////////////////////////////////////////////////////////////
//
//  GlintModifier.h - Discrete-facet glint normal modifier.
//
//  Models the sparse field of mirror-like micro-facets ("flecks" /
//  "spangle" / metallic flakes) that makes surfaces like grand-feu
//  enamel glaze, metallic car paint, snow, or glitter TWINKLE: as the
//  view/light sweeps, individual sub-pixel facets flash on and off.
//
//  A smooth continuous NDF (GGX) cannot produce discrete flecks, and
//  explicit micro-geometry at 10-100 um is both intractable (mesh
//  memory wall) and mis-leveled (sub-pixel structure belongs in
//  shading statistics).  This modifier is the shading-statistics
//  representation: an object-space cell hash places at most one
//  disc-shaped facet per cell (jittered centre + radius test +
//  per-cell existence probability, after Atanasov & Koylazov 2016,
//  "A practical stochastic algorithm for rendering mirror-like
//  flakes"); a hit landing on a facet has its shading normal replaced
//  by the facet's hash-seeded normal (a small Rayleigh-distributed
//  tilt about the smooth shading normal).
//
//  Because the perturbation happens at hit-production time (the
//  IRayIntersectionModifier hook fires in every transport path:
//  RayCaster, PT, BDPT, the photon tracers, SMS, SSS), the EXISTING
//  materials deliver the glints: DielectricSPF flashes its delta
//  reflection off the facet (glaze fleck), GGXSPF importance-samples
//  its VNDF around the perturbed frame (metal-flake fleck), and
//  value/Scatter/Pdf all see the same frame, so NEE-vs-BSDF MIS is
//  consistent by construction.  No new BRDF code.  See
//  docs/ENAMEL_SPARKLE_BRDF.md for the full design + ratification
//  record (including why this superseded a SparkleBRDF material).
//
//  Object-space hashing (`ptObjIntersec`) pins each facet to a fixed
//  surface location: flecks twinkle because the view/light moves,
//  never because the pattern swims under camera/object animation.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: July 3, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GLINT_MODIFIER_
#define GLINT_MODIFIER_

#include "../Interfaces/IRayIntersectionModifier.h"
#include "../Utilities/Reference.h"
#include <cmath>

namespace RISE
{
	namespace Implementation
	{
		//! Integer-hash helpers for the glint cell field.  Header-inline so
		//! the unit test exercises the SAME arithmetic the modifier uses
		//! (single source of truth; no test/impl drift).  Deterministic and
		//! platform-independent: integer mixing only.
		namespace GlintHash
		{
			//! 32-bit avalanche hash (Murmur3-finalizer variant).  Bijective
			//! on uint32, so chained applications yield decorrelated lanes.
			inline unsigned int HashU32( unsigned int x )
			{
				x ^= x >> 16;
				x *= 0x7feb352dU;
				x ^= x >> 15;
				x *= 0x846ca68bU;
				x ^= x >> 16;
				return x;
			}

			//! Hash a 3D integer cell coordinate + seed into a well-mixed
			//! 32-bit value.  Each axis is pre-mixed with a distinct odd
			//! 64-bit constant before folding into the avalanche so
			//! axis-aligned neighbours decorrelate (no lattice streaks).
			inline unsigned int HashCell( long long cx, long long cy, long long cz, unsigned int seed )
			{
				unsigned int h = seed * 0x9e3779b1U + 0x632be5abU;
				h = HashU32( h ^ (unsigned int)( (unsigned long long)cx * 0x9e3779b97f4a7c15ULL >> 11 ) );
				h = HashU32( h ^ (unsigned int)( (unsigned long long)cy * 0xc2b2ae3d27d4eb4fULL >> 11 ) );
				h = HashU32( h ^ (unsigned int)( (unsigned long long)cz * 0x165667b19e3779f9ULL >> 11 ) );
				return h;
			}

			//! Map a hash to canonical [0,1) using the top 24 bits (the
			//! best-mixed); exactly representable in a double, never 1.0.
			inline Scalar U01( unsigned int h )
			{
				return (Scalar)( h >> 8 ) * ( Scalar(1.0) / Scalar(16777216.0) );
			}

			//! Fold a (already floor()ed, possibly offset) cell coordinate
			//! into a bounded int64 so the double->integer conversion is
			//! well-defined for hostile coordinates/densities.  The fold
			//! period 2^40 dwarfs any real cell count; the hash mixes low
			//! bits regardless, so the period is invisible in practice.
			inline long long FoldToCellIndex( double flooredCoord )
			{
				const double LARGE = 1099511627776.0;	// 2^40
				double f = flooredCoord - LARGE * std::floor( flooredCoord / LARGE );
				return (long long)f;
			}
		}

		//! The facet (if any) covering a given object-space point, in
		//! frame-independent form: identity (centre in scaled cell space)
		//! plus the hash-seeded tilt angles.  `Modify` expresses the tilt
		//! in the hit's local shading frame; the unit test consumes this
		//! struct directly to validate placement/statistics without
		//! constructing full intersections.
		struct GlintFacet
		{
			bool	found;
			Vector3	centreQ;		///< facet centre in q-space (scaled+shifted object space x density)
			Scalar	theta;			///< tilt polar angle from the smooth normal (radians, safety-clamped)
			Scalar	phi;			///< tilt azimuth about the smooth normal (radians)

			GlintFacet() : found( false ), centreQ( 0, 0, 0 ), theta( 0 ), phi( 0 ) {}
		};

		class GlintModifier :
			public virtual IRayIntersectionModifier,
			public virtual Reference
		{
		protected:
			virtual ~GlintModifier( );

			Scalar		density;		///< cells per object-space unit (facet pitch = 1/density units)
			Scalar		coverage;		///< per-cell facet existence probability, clamped [0,1]
			Scalar		fill;			///< facet disc radius as fraction of the half-cell, clamped <= 1; <= 0 is inert
			Scalar		spreadRad;		///< facet tilt Rayleigh scale (stored radians; authored degrees)
			Vector3		vScale;			///< anisotropic cell stretch (Worley3DPainter convention)
			Vector3		vShift;			///< cell-space offset (Worley3DPainter convention)
			unsigned int seed;			///< hash seed (distinct fleck fields on otherwise identical objects)

		public:
			GlintModifier(
				const Scalar density_,			///< [in] cells per object-space unit; <= 0 makes the modifier inert
				const Scalar coverage_,			///< [in] per-cell facet existence probability [0,1]
				const Scalar fill_,				///< [in] facet disc radius fraction of the half-cell (0,1]; <= 0 makes the modifier inert
				const Scalar spreadDeg_,		///< [in] facet tilt Rayleigh scale in DEGREES; <= 0 makes the modifier inert
				const Vector3& vScale_,			///< [in] anisotropic cell stretch (1,1,1 = isotropic)
				const Vector3& vShift_,			///< [in] cell-space offset
				const unsigned int seed_		///< [in] hash seed
				);

			//! Replaces the shading normal + ONB with the facet frame when
			//! the hit lands on a facet; leaves the intersection untouched
			//! otherwise (and whenever the safety guards reject the tilt).
			void Modify( RayIntersectionGeometric& ri ) const;

			//! The facet covering `objPt` (object space), or found=false.
			//! Deterministic pure function of (objPt, parameters) — this is
			//! the object-space-stability contract, exposed for the unit
			//! test.  Searches the 3x3x3 cell neighbourhood so facet discs
			//! never clip at cell boundaries (radius <= half-cell); when
			//! discs from adjacent cells overlap, the nearest centre wins.
			GlintFacet FindFacet( const Point3& objPt ) const;
		};
	}
}

#endif
