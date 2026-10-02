//////////////////////////////////////////////////////////////////////
//
//  BDPTUtilities.h - Geometric and PDF measure conversion utilities
//  for BDPT.
//
//  BDPT requires frequent conversions between solid angle and area
//  PDF measures.  The relationship is (Veach thesis eq. 8.8):
//    pdfArea = pdfSolidAngle * |cos(theta)| / dist^2
//  where theta is the angle at the receiving vertex and dist is the
//  distance between vertices.
//
//  The geometric term G(x<->y) is the product of cosines at both
//  endpoints divided by the squared distance.  It appears in every
//  BDPT connection evaluation.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BDPT_UTILITIES_
#define BDPT_UTILITIES_

#include "Math3D/Math3D.h"
#include "../Shaders/BDPTVertex.h"
#include <vector>

namespace RISE
{
	namespace BDPTUtilities
	{
		//////////////////////////////////////////////////////////////
		// Medium distance-sampling streams (DL-283).
		//
		// The eye and light walks give every loop iteration its own
		// sampler stream (`EyeWalkStream` / `LightWalkStream` below --
		// `16 + depth` / `1 + depth` for shallow iterations, DL-286 --
		// 32 Sobol' dimensions each).  An
		// `IMedium::SampleDistance` call is the one consumer inside
		// that iteration whose draw count is OPEN-ENDED: heterogeneous
		// delta tracking draws one value per majorant-grid cell it
		// crosses plus two per tentative collision, up to
		// `IMedium::kMaxSampleDistanceDraws` (2048).  Drawn from the
		// vertex stream, a long free flight ran past the stream's 32
		// slots into the stream the NEXT iteration re-opens, so one
		// Sobol' dimension drove two decisions on one path (the PT
		// twin of this defect was DL-247's `PTVolumeWalkStream`).
		//
		// Under a fixed-budget sampler (`ISampler::
		// HasFixedDimensionBudget()`, i.e. SobolSampler) each distance
		// sample therefore draws from a BLOCK of
		// `kMediumDistanceStreamsPerEvent` streams of its own, wide
		// enough for the full draw bound (64 * 32 = 2048 dimensions),
		// keyed by (walk, loop iteration).  The caller re-opens its
		// vertex stream afterwards, so the vertex's own draws (phase,
		// roulette, BSDF) sit at the same slots whether or not a medium
		// was crossed.
		//
		// Layout, in streams (dimension = 32 * stream):
		//   eye   walk, iteration d:  8192 + 64 * d            d < 1024
		//   light walk, iteration d:  8192 + 64 * (1024 + d)   d < 1024
		// i.e. [8192, 139264), 4.46M dimensions at most.  Both walk
		// loops saturate their iteration count at
		// `kWalkIterationCap` (1024), which is what bounds `d`.  Every
		// other consumer of a BDPT/VCM sampler sits below 8192 except
		// the DEEP walk iterations (DL-286, `LightWalkStream` /
		// `EyeWalkStream` below, [139264, 141266)): film/light select 0,
		// shallow light walk 1..15, shallow eye walk 16..46, strategy
		// select 47, VCM NEE 49..3121 (48 + i, i >= 1), MLT film/lens
		// 2048 (PSSMLT only), thin-lens aperture 3322 -- and PT's own
		// volume walks (`PTVolumeWalkStream`, 4096..8191) never share a
		// sampler with these at all.  `tests/SobolDimensionBudgetTest.cpp`
		// Test G2 enumerates the whole map and asserts it is
		// collision-free.
		//
		// Every block lies past SobolSampler's 8192-dimension table, so
		// each draw is a wrapped dimension (an Owen-permuted index over
		// a table row).  Samplers WITHOUT a fixed budget are left on the
		// vertex stream: `IndependentSampler` ignores streams, and a
		// `PSSMLTSampler` (MLT) stream is an unbounded lane that cannot
		// spill.  Routing MLT through the blocks too was measured and
		// declined: no correctness gain, and +13 % user CPU on
		// scenes/Tests/MLT/mlt_deep_fog.RISEscene (its chains, not its
		// storage -- 128 events cost 128 extra-tier lanes), besides
		// needing PSSMLTSampler's 4096-stream sanity bound raised.
		//////////////////////////////////////////////////////////////
		static const unsigned int kWalkIterationCap = 1024;
		static const int kMediumDistanceStreamBase = 8192;
		static const int kMediumDistanceStreamsPerEvent = 64;

		enum WalkSide { eEyeWalk = 0, eLightWalk = 1 };

		//! First stream of the block a walk's distance sample at loop
		//! iteration `depth` draws from.  `depth` is clamped into the
		//! cap so a caller bug cannot run into a neighbouring layout;
		//! both walk loops already keep it below the cap.
		inline int MediumDistanceStream( const WalkSide side, const unsigned int depth )
		{
			const unsigned int d = depth < kWalkIterationCap ? depth : kWalkIterationCap - 1u;
			const unsigned int event = static_cast<unsigned int>( side ) * kWalkIterationCap + d;
			return kMediumDistanceStreamBase +
				static_cast<int>( event ) * kMediumDistanceStreamsPerEvent;
		}

		//! One past the last stream the medium-distance layout can reach.
		static const int kMediumDistanceStreamEnd = kMediumDistanceStreamBase +
			2 * static_cast<int>( kWalkIterationCap ) * kMediumDistanceStreamsPerEvent;

		//////////////////////////////////////////////////////////////
		// Per-iteration WALK streams (DL-286).
		//
		// Loop iteration `d` of the light walk opens one sampler stream
		// and iteration `d` of the eye walk another; everything a
		// vertex draws (phase / BSDF direction, lobe choice, roulette,
		// guiding) sits at fixed slots of that stream.  Both walks run
		// off ONE sampler per BDPT/VCM/MLT sample, so their streams
		// must be DISJOINT at every depth.  They used to be `1 + d` and
		// `16 + d`, laid out for 15 bounces: from light iteration 15 the
		// light walk re-opened the eye walk's streams (and the strategy
		// select, 47, at 46 and VCM's first NEE stream, 49, at 48; the
		// eye walk reached 47 at 31 and the NEE at 33).  Walk iterations
		// count medium scatters, so dense media reach that routinely,
		// and one Sobol' dimension (one PSSMLT primary sample) then
		// drove a light-vertex decision AND an eye-vertex decision of
		// ONE connected path -- a bias, not just a correlation: -12 % on
		// a light inside albedo-0.99 / sigma_t-8 fog under BDPT with
		// Sobol' and -13 % under MLT (docs/DL81_SOBOL_DIMENSION_PARITY.md
		// section 10, the DL-286 ledger row).
		//
		// The SHALLOW iterations keep their historical streams, so a
		// walk that never reaches light iteration 15 / eye iteration 31
		// draws exactly what it drew before (every medium-free shipped
		// scene; PSSMLT's legacy tier, streams < 49, is untouched):
		//   light iteration d < 15:  1 + d      (1..15)
		//   eye   iteration d < 31:  16 + d     (16..46)
		// DEEP iterations move to a block of their own, light first:
		//   light iteration d >= 15: base + (d - 15)            (1009 streams)
		//   eye   iteration d >= 31: base + 1009 + (d - 31)     ( 993 streams)
		// `d` < `kWalkIterationCap` (1024) always (both loops saturate).
		// The base depends on the sampler, because the two stream
		// spaces have different free regions:
		//   fixed-budget (SobolSampler, `HasFixedDimensionBudget()`):
		//     base 139264 = `kMediumDistanceStreamEnd`, i.e. streams
		//     [139264, 141266), past the DL-283 medium-distance blocks,
		//     in the wrap region (wraps 544..551).  Nothing below fits:
		//     VCM's per-eye-vertex NEE owns 49..3121 (48 + i).
		//   unbounded-lane (PSSMLTSampler, i.e. MLT; IndependentSampler
		//     ignores streams): base 2049, i.e. lanes [2049, 4051) --
		//     just above MLT's reserved film/lens/aperture lane 2048
		//     (`BDPTCameraUtilities::kPSSMLTFilmLensApertureStream`) and
		//     below PSSMLTSampler's 4096 stream sanity bound; lanes
		//     >= 49 live in its extra tier, so the lane numbers cost
		//     nothing.  VCM never drives a PSSMLTSampler, so its NEE
		//     range is irrelevant here.
		// `tests/SobolDimensionBudgetTest.cpp` Test G2 enumerates both
		// layouts from these functions and asserts them collision-free;
		// Test H asserts no light/eye shared dimension (Sobol') or
		// shared primary sample (PSSMLT) on the real generators.
		//////////////////////////////////////////////////////////////
		static const unsigned int kLightWalkShallowIterations = 15;
		static const unsigned int kEyeWalkShallowIterations = 31;
		static const int kDeepLightWalkStreams =
			static_cast<int>( kWalkIterationCap - kLightWalkShallowIterations );
		static const int kDeepEyeWalkStreams =
			static_cast<int>( kWalkIterationCap - kEyeWalkShallowIterations );
		static const int kDeepWalkStreamBaseFixedBudget = kMediumDistanceStreamEnd;
		static const int kDeepWalkStreamBaseUnboundedLanes = 2049;
		//! PSSMLTSampler's stream sanity bound (`kDefaultNumStreams`,
		//! protected there); the deep lanes must stay below it.
		//! `tests/PSSMLTStreamAliasingTest.cpp` checks the two agree.
		static const int kPSSMLTStreamSanityBound = 4096;
		static_assert( kDeepWalkStreamBaseUnboundedLanes + kDeepLightWalkStreams +
			kDeepEyeWalkStreams <= kPSSMLTStreamSanityBound,
			"DL-286: the deep walk lanes must fit under PSSMLTSampler's stream bound" );

		//! Stream for light-walk loop iteration `depth` (DL-286).
		//! `bFixedBudget` is `ISampler::HasFixedDimensionBudget()`.
		inline int LightWalkStream( const unsigned int depth, const bool bFixedBudget )
		{
			if( depth < kLightWalkShallowIterations ) {
				return 1 + static_cast<int>( depth );
			}
			const unsigned int d = depth < kWalkIterationCap ? depth : kWalkIterationCap - 1u;
			const int base = bFixedBudget ?
				kDeepWalkStreamBaseFixedBudget : kDeepWalkStreamBaseUnboundedLanes;
			return base + static_cast<int>( d - kLightWalkShallowIterations );
		}

		//! Stream for eye-walk loop iteration `depth` (DL-286).
		inline int EyeWalkStream( const unsigned int depth, const bool bFixedBudget )
		{
			if( depth < kEyeWalkShallowIterations ) {
				return 16 + static_cast<int>( depth );
			}
			const unsigned int d = depth < kWalkIterationCap ? depth : kWalkIterationCap - 1u;
			const int base = bFixedBudget ?
				kDeepWalkStreamBaseFixedBudget : kDeepWalkStreamBaseUnboundedLanes;
			return base + kDeepLightWalkStreams + static_cast<int>( d - kEyeWalkShallowIterations );
		}

		//! One past the last deep-walk stream, per sampler kind.
		inline int DeepWalkStreamEnd( const bool bFixedBudget )
		{
			return ( bFixedBudget ? kDeepWalkStreamBaseFixedBudget : kDeepWalkStreamBaseUnboundedLanes ) +
				kDeepLightWalkStreams + kDeepEyeWalkStreams;
		}

		//! Convert a solid-angle PDF at `from` to the canonical
		//! measure stored at `to`.
		//!
		//! For ordinary surface / medium / camera destinations this
		//! is the standard SA -> area Jacobian (cos / dist^2 for
		//! surface, sigma_t / dist^2 for medium, 1/dist^2 for
		//! camera).  For infinite-area / environment-light
		//! destinations the conversion is the IDENTITY — the env
		//! vertex's pdfFwd / pdfRev are stored in solid-angle
		//! measure (sr^-1) directly, matching PBRT-v4 §15.5.2
		//! `ConvertDensity`. This helper converts angular sampling at
		//! a finite source. A parallel environment emission instead
		//! requires its conditional projected-disc target density;
		//! BDPT's generators install that density explicitly.
		/// \return PDF in the destination's canonical measure.
		inline Scalar ConvertDensity(
			const Scalar pdfSolidAngle,				///< [in] PDF in SA measure at `from` [1/sr]
			const BDPTVertex& from,					///< [in] Source vertex (where pdfSolidAngle is parameterised)
			const BDPTVertex& to					///< [in] Destination vertex (whose measure we convert TO)
			)
		{
			if( to.IsInfiniteLight() ) {
				// Env vertex stores SA-measure pdfs directly — skip
				// the area-Jacobian to match PBRT-v4's first-class
				// infinite-light vertex treatment.
				return pdfSolidAngle;
			}
			const Vector3 d = Vector3Ops::mkVector3( to.position, from.position );
			const Scalar distSq = Vector3Ops::SquaredModulus( d );
			if( distSq < 1e-20 ) {
				return 0;
			}
			if( to.type == BDPTVertex::MEDIUM ) {
				// Medium destination: sigma_t replaces |cos|.
				return pdfSolidAngle * to.sigma_t_scalar / distSq;
			}
			if( to.type == BDPTVertex::CAMERA ) {
				// Camera destination: implicit cos=1 (camera-pinhole
				// directional reparameterisation handled upstream).
				return pdfSolidAngle / distSq;
			}
			const Scalar invDist = Scalar( 1 ) / sqrt( distSq );
			const Vector3 dHat = d * invDist;
			const Scalar absCos = fabs( Vector3Ops::Dot( to.geomNormal, dHat ) );
			return pdfSolidAngle * absCos / distSq;
		}

		//! Computes the geometric term G(x <-> y) between two surface points.
		//! G = |cos(theta_x)| * |cos(theta_y)| / ||x - y||^2
		//! where theta_x is the angle between normal at x and the direction toward y,
		//! and theta_y is the angle between normal at y and the direction toward x.
		/// \return The geometric term value
		inline Scalar GeometricTerm(
			const Point3& p1,						///< [in] Position of first vertex
			const Vector3& n1,						///< [in] Normal at first vertex
			const Point3& p2,						///< [in] Position of second vertex
			const Vector3& n2						///< [in] Normal at second vertex
			)
		{
			Vector3 d = Vector3Ops::mkVector3( p2, p1 );
			const Scalar distSq = Vector3Ops::SquaredModulus( d );

			if( distSq < 1e-20 ) {
				return 0;
			}

			const Scalar invDist = 1.0 / sqrt( distSq );
			d = d * invDist;

			const Scalar cosTheta1 = fabs( Vector3Ops::Dot( n1, d ) );
			const Scalar cosTheta2 = fabs( Vector3Ops::Dot( n2, -d ) );

			return (cosTheta1 * cosTheta2) / distSq;
		}

		//! Converts a PDF from solid angle measure to area measure.
		//! pdfArea = pdfSolidAngle * |cos(theta)| / dist^2
		/// \return PDF in area measure
		inline Scalar SolidAngleToArea(
			const Scalar pdfSolidAngle,				///< [in] PDF in solid angle measure [1/sr]
			const Scalar absCosTheta,				///< [in] |cos(theta)| at the receiving vertex
			const Scalar distSquared				///< [in] Squared distance between the two vertices
			)
		{
			if( distSquared < 1e-20 ) {
				return 0;
			}
			return pdfSolidAngle * absCosTheta / distSquared;
		}

		//! Converts a PDF from area measure to solid angle measure.
		//! pdfSolidAngle = pdfArea * dist^2 / |cos(theta)|
		/// \return PDF in solid angle measure
		inline Scalar AreaToSolidAngle(
			const Scalar pdfArea,					///< [in] PDF in area measure
			const Scalar absCosTheta,				///< [in] |cos(theta)| at the receiving vertex
			const Scalar distSquared				///< [in] Squared distance between the two vertices
			)
		{
			if( absCosTheta < 1e-20 ) {
				return 0;
			}
			return pdfArea * distSquared / absCosTheta;
		}

		//////////////////////////////////////////////////////////////////
		// Medium vertex utilities
		//
		// For medium scatter vertices (no surface orientation), the
		// geometric coupling and PDF measure conversions differ from
		// surface vertices:
		//
		//   Geometric coupling term G(x <-> y):
		//     surface <-> surface:  |cos_x| * |cos_y| / dist^2
		//     surface <-> medium:   |cos_surface| / dist^2
		//     medium  <-> medium:   1 / dist^2
		//
		//   Medium vertices have no surface normal, so no cosine factor
		//   appears.  The 1/dist^2 term is the inverse-square law for
		//   point-to-point radiance transport in free space.
		//
		//   For the solid-angle-to-area PDF conversion at medium
		//   vertices, sigma_t replaces |cos(theta)| (Veach thesis
		//   Ch. 11; PBRT v4 Section 16.3):
		//     pdfArea = pdfSolidAngle * sigma_t / dist^2
		//////////////////////////////////////////////////////////////////

		//! Converts a solid angle PDF to area measure at a medium vertex.
		//! sigma_t at the scatter point replaces |cos(theta)| because the
		//! free-flight sampling probability density is proportional to
		//! sigma_t, while surface "acceptance" is proportional to the
		//! projected area (|cos|/dist^2).
		/// \return PDF in generalized area measure
		inline Scalar SolidAngleToAreaMedium(
			const Scalar pdfSolidAngle,				///< [in] PDF in solid angle measure [1/sr]
			const Scalar sigma_t,					///< [in] Extinction coefficient at scatter point
			const Scalar distSquared				///< [in] Squared distance between vertices
			)
		{
			if( distSquared < 1e-20 ) {
				return 0;
			}
			return pdfSolidAngle * sigma_t / distSquared;
		}

		//! Geometric term between a surface vertex and a medium vertex.
		//! G = |cos(theta_surface)| / ||p_surface - p_medium||^2
		//! Only the surface side contributes a cosine factor.
		/// \return The geometric term value
		inline Scalar GeometricTermSurfaceMedium(
			const Point3& pSurface,					///< [in] Position of surface vertex
			const Vector3& nSurface,				///< [in] Normal at surface vertex
			const Point3& pMedium					///< [in] Position of medium vertex
			)
		{
			Vector3 d = Vector3Ops::mkVector3( pMedium, pSurface );
			const Scalar distSq = Vector3Ops::SquaredModulus( d );

			if( distSq < 1e-20 ) {
				return 0;
			}

			const Scalar invDist = 1.0 / sqrt( distSq );
			d = d * invDist;

			const Scalar cosTheta = fabs( Vector3Ops::Dot( nSurface, d ) );

			return cosTheta / distSq;
		}

		//! Geometric term between two medium vertices.
		//! G = 1 / ||p1 - p2||^2
		//! Neither side has a surface orientation.
		/// \return The geometric term value
		inline Scalar GeometricTermMediumMedium(
			const Point3& p1,						///< [in] Position of first medium vertex
			const Point3& p2						///< [in] Position of second medium vertex
			)
		{
			const Scalar distSq = Vector3Ops::SquaredModulus(
				Vector3Ops::mkVector3( p2, p1 ) );

			if( distSq < 1e-20 ) {
				return 0;
			}

			return 1.0 / distSq;
		}

		//////////////////////////////////////////////////////////////
		// Subsurface jumps partition paths BY PATH between the two
		// jump families (DL-317 in VCM; shared with BDPT/MLT since
		// DL-375).  A BSSRDF / random-walk event relocates the path
		// from the hit where it went in (`x_o`, marked delta) to a
		// sampled ENTRY vertex (`isBSSRDFEntry`); the relocation is not
		// an edge and its reverse density is never evaluated, so the
		// eye-sampled and light-sampled jump families estimate the
		// SAME integral with nothing to MIS them against each other.
		// A path belongs to the EYE-sampled family whenever that family
		// has a strategy for it, and to the light-sampled family only
		// when it has none; within the owning family the MIS walks stop
		// at the jump (VCM: zero running quantities at the entry; BDPT:
		// `MISWeight` breaks at an entry).
		//
		// Since DL-375 BOTH entry kinds are connectible: a diffusion
		// entry prices Sw with its profile's Fresnel, a random-walk
		// entry with the exact dielectric transmission
		// (`RandomWalkEntryBSDF`, PT's own NEE adapter); the MIS
		// density of either is the cosine exit its continuation is
		// sampled from.  So the delta-lit random-walk class that
		// DL-317 had to KEEP in the light family (a point light
		// feeding a random-walk entry) is now eye-coverable through NEE
		// at the eye's entry, and the predicate below -- which reads
		// the entry's `isConnectible` -- cuts it.
		//////////////////////////////////////////////////////////////

		/// Does the EYE-sampled family have a strategy for a path whose
		/// jump the light walk sampled at `verts[p] -> verts[p+1]` (the
		/// hit where the light went in, then the entry)?  The eye would
		/// arrive at the entry's point, jump to verts[p] (its own entry
		/// vertex there -- connectible exactly when verts[p+1] is), and
		/// must split the light-side segment verts[s..p] somewhere;
		/// verts[s] is the light root (s == 0) or the previous KEPT
		/// light entry.  The strategies, with the eye covering
		/// verts[j..p] and the light verts[s..j-1]:
		///   s=0 (eye hits the root): root not delta;
		///   merge at verts[j] (VCM only): merging live, verts[j] a
		///     non-delta surface;
		///   NEE / connection at the edge (j-1, j): verts[j] non-delta
		///     and connectible, and verts[j-1] the root (NEE reaches any
		///     light) or a non-delta connectible vertex (a kept entry is
		///     one).
		/// "Non-delta" is the path's own scatter at that vertex (the
		/// light walk's sampled lobe), so the predicate is a function of
		/// the path.
		inline bool LightSegmentEyeCoverable(
			const std::vector<BDPTVertex>& verts,
			const std::size_t s,
			const std::size_t p,
			const bool mergingActive
			)
		{
			if( p + 1 >= verts.size() || p <= s ) {
				return true;	// malformed: keep the cut (conservative, never double counts)
			}
			if( s == 0 && verts[0].type == BDPTVertex::LIGHT && !verts[0].isDelta ) {
				return true;
			}
			for( std::size_t j = s + 1; j <= p; j++ ) {
				const BDPTVertex& b = verts[j];
				const bool bUsable = ( j == p )
					? verts[p + 1].isConnectible
					: ( b.isConnectible && !b.isDelta );
				if( !bUsable ) {
					continue;
				}
				if( mergingActive && b.type == BDPTVertex::SURFACE ) {
					return true;
				}
				const BDPTVertex& a = verts[j - 1];
				if( j - 1 == 0 && a.type == BDPTVertex::LIGHT ) {
					return true;
				}
				if( a.isConnectible && !a.isDelta ) {
					return true;
				}
			}
			return false;
		}

		/// The number of leading light-subpath vertices usable as a
		/// strategy endpoint.  Walks the light-side jumps in order; the
		/// first one the eye family can cover ends the usable subpath
		/// (nothing at or past it is a strategy).  A jump the eye family
		/// cannot cover is KEPT -- the light-sampled family is then the
		/// only estimator of those paths -- and the next segment starts
		/// at its entry.  The hit where the light walk went INTO the
		/// material is always usable: it is an ordinary arrival; only
		/// its continuation was the jump.
		inline std::size_t UsableLightSubpathLength(
			const std::vector<BDPTVertex>& lightVerts,
			const bool mergingActive
			)
		{
			std::size_t segmentStart = 0;
			for( std::size_t i = 1; i < lightVerts.size(); i++ ) {
				if( !lightVerts[i].isBSSRDFEntry ) {
					continue;
				}
				if( LightSegmentEyeCoverable( lightVerts, segmentStart, i - 1, mergingActive ) ) {
					return i;
				}
				segmentStart = i;
			}
			return lightVerts.size();
		}
	}
}

#endif
