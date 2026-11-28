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
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IRayCaster.h"		// kShadowWalkMaxCrossings (DL-330)
#include <vector>
#include <climits>

namespace RISE
{
	namespace BDPTUtilities
	{
		//////////////////////////////////////////////////////////////
		// Medium distance-sampling streams (DL-283).
		//
		// Under fixed-budget samplers (`HasFixedDimensionBudget()`, i.e.
		// SobolSampler), open-ended `IMedium::SampleDistance` calls (up to
		// `IMedium::kMaxSampleDistanceDraws` = 2048 draws) draw from a
		// dedicated 64-stream block (`64 * 32 = 2048` dims) per (walk, iter)
		// so they never overrun the vertex stream into the next iteration.
		// Callers re-open their vertex stream afterwards.
		// Layout (d < kWalkIterationCap = 1024):
		//   eye   walk: 8192 + 64 * d
		//   light walk: 8192 + 64 * (1024 + d)   -> [8192, 139264)
		// Unbounded-lane samplers (PSSMLTSampler, IndependentSampler) stay
		// on the vertex stream. Checked collision-free by
		// `tests/SobolDimensionBudgetTest.cpp`.
		//////////////////////////////////////////////////////////////
		static const unsigned int kWalkIterationCap = 1024;
		static const int kMediumDistanceStreamBase = 8192;
		static const int kMediumDistanceStreamsPerEvent = 64;

		enum WalkSide { eEyeWalk = 0, eLightWalk = 1 };

		//! First stream of the medium-distance block for `(side, depth)`
		//! (`depth` clamped to `< kWalkIterationCap`).
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
		// Per-iteration walk streams (DL-286; docs/DEBT_LEDGER.md).
		//
		// Eye and light walks share one sampler per BDPT/VCM/MLT sample and
		// must use disjoint streams at every iteration `d < kWalkIterationCap`:
		//   shallow light (d < 15):  1 + d             (1..15)
		//   shallow eye   (d < 31):  16 + d            (16..46)
		//   deep light    (d >= 15): base + (d - 15)           (1009 streams)
		//   deep eye      (d >= 31): base + 1009 + (d - 31)    ( 993 streams)
		// where `base` is:
		//   fixed-budget (SobolSampler): `kMediumDistanceStreamEnd` (139264)
		//   unbounded-lane (PSSMLT):     2049 (above lane 2048, below 4096 cap)
		// Checked collision-free by `tests/SobolDimensionBudgetTest.cpp`.
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
		// Medium vertex utilities (Veach Ch. 11; PBRT-v4 §16.3):
		//   G(surface <-> medium) = |cos_surface| / dist^2
		//   G(medium  <-> medium) = 1 / dist^2
		//   pdfArea (at medium)   = pdfSolidAngle * sigma_t / dist^2
		//////////////////////////////////////////////////////////////////

		//! Converts solid-angle PDF to generalized area measure at a medium vertex
		//! (sigma_t replaces |cos(theta)|).
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
		// Subsurface-jump path partition under depth caps (DL-317/375/380).
		//
		// BSSRDF / random-walk jumps relocate x_o -> entry (`isBSSRDFEntry`)
		// without a reverse relocation density, so eye- and light-sampled
		// jump families cannot be MIS'd against each other (MIS walks stop
		// at any entry). Both diffusion and random-walk entries are
		// connectible (`isConnectible`). A path belongs to the EYE family
		// iff the eye walk can generate a covering subpath within
		// `max_eye_depth` and `WalkIterationBudget(max_eye_depth,
		// max_volume_bounce)`, and to the LIGHT family otherwise.
		//
		// Surface hit count `D = S + L(k)` (minus 1 for a merge) counts
		// ray-reached SURFACE vertices (excluding BSSRDF entries, camera,
		// light roots, media, escapes); a jump pair counts 1 in either
		// direction, so D is path-invariant. Covering light tail `verts[w..k]`
		// requires `D - L(w-1)` eye surface hits plus remaining medium
		// vertices. The largest witness `w` of the last witness-bearing
		// jump dominates all earlier ones (`w = 0` = area hit / env escape).
		//////////////////////////////////////////////////////////////

		//! The loop budget of a BDPT/VCM/MLT subpath walk: one iteration
		//! per surface hit, medium scatter or escape, saturating at
		//! `kWalkIterationCap` (a `max_volume_bounce` of UINT_MAX means
		//! "unlimited" and must not wrap).  Shared by both generators and
		//! the DL-380 partition, which must agree with them exactly.
		inline unsigned int WalkIterationBudget(
			const unsigned int maxSurface,
			const unsigned int maxVolume
			)
		{
			return ( maxSurface >= kWalkIterationCap ||
				maxVolume > kWalkIterationCap - maxSurface ) ?
					kWalkIterationCap : maxSurface + maxVolume;
		}

		//! The eye walk's caps, as the partition needs them.
		struct EyeWalkCaps
		{
			unsigned int maxSurface;		///< max_eye_depth
			unsigned int maxIterations;		///< WalkIterationBudget( max_eye_depth, max_volume_bounce )
		};

		inline EyeWalkCaps MakeEyeWalkCaps(
			const unsigned int maxEyeDepth,
			const unsigned int maxVolumeBounce
			)
		{
			EyeWalkCaps c;
			c.maxSurface = maxEyeDepth;
			c.maxIterations = WalkIterationBudget( maxEyeDepth, maxVolumeBounce );
			return c;
		}

		//! Does a walk count `v` against its surface cap?  (See above.)
		inline bool CountsAsSurfaceHit( const BDPTVertex& v )
		{
			return v.type == BDPTVertex::SURFACE && !v.isBSSRDFEntry;
		}

		//////////////////////////////////////////////////////////////////
		// Joined-path per-type bounce caps (DL-471 / DL-481;
		// docs/SCENE_CONVENTIONS.md §8.10).
		//
		// For scattering vertices x_1..x_K from camera to light: counts
		// lobe types at x_1..x_{K-1}, and at x_K only if delta or no BSDF
		// (NEE and BSDF continuation to light at x_K are free). Medium
		// scatters do not count here; BSSRDF entries charge eRayTranslucent.
		//
		// Counted multi-type connection endpoints whose material implements
		// `HasConnectionTypeSplit` / `IBSDF::valueByScatterType` are priced
		// over allowed label combinations via `AllowedTypeProduct` (leaving
		// MIS untouched). Multi-type endpoints without a split are excluded
		// from the strategy and MIS (VCM falls back to s=0 + NEE).
		// Caps >= max_eye_depth + max_light_depth cannot bind and become UINT_MAX.
		//////////////////////////////////////////////////////////////////

		struct BounceTypeCaps
		{
			unsigned int	cap[4];		///< diffuse, glossy (reflection), transmission (refraction), translucent; UINT_MAX = cannot bind
			unsigned int	boundMask;	///< bit (1 << t) for every ScatRayType t whose cap can bind
			bool			active;		///< any cap can bind

			BounceTypeCaps() : boundMask( 0 ), active( false ) { cap[0] = cap[1] = cap[2] = cap[3] = UINT_MAX; }
		};

		/// The caps that can BIND on a path the walks generate (see above:
		/// N < max_eye_depth + max_light_depth); the others become UINT_MAX.
		inline BounceTypeCaps MakeBounceTypeCaps(
			const unsigned int maxDiffuse,
			const unsigned int maxGlossy,
			const unsigned int maxTransmission,
			const unsigned int maxTranslucent,
			const unsigned int maxEyeDepth,
			const unsigned int maxLightDepth
			)
		{
			const unsigned long long surfaces =
				static_cast<unsigned long long>( maxEyeDepth ) + static_cast<unsigned long long>( maxLightDepth );
			const unsigned int in[4] = { maxDiffuse, maxGlossy, maxTransmission, maxTranslucent };
			BounceTypeCaps c;
			for( int k = 0; k < 4; k++ ) {
				if( static_cast<unsigned long long>( in[k] ) < surfaces ) {
					c.cap[k] = in[k];
					c.boundMask |= 1u << ( k + 1 );
					c.active = true;
				}
			}
			return c;
		}

		/// Counter slot of a scatter type, -1 for a type no cap counts.
		inline int BounceTypeSlot( const unsigned int type )
		{
			return ( type >= ScatteredRay::eRayDiffuse && type <= ScatteredRay::eRayTranslucent ) ?
				static_cast<int>( type ) - 1 : -1;
		}

		struct BounceTypeCounts
		{
			unsigned int	n[4];

			BounceTypeCounts() { n[0] = n[1] = n[2] = n[3] = 0; }
			void Add( const unsigned int type )
			{
				const int k = BounceTypeSlot( type );
				if( k >= 0 ) n[k]++;
			}
			bool Within( const BounceTypeCaps& caps ) const
			{
				return n[0] <= caps.cap[0] && n[1] <= caps.cap[1] &&
					n[2] <= caps.cap[2] && n[3] <= caps.cap[3];
			}
		};

		enum TypeCapStatus
		{
			eTypeCapOK = 0,			///< within every cap
			eTypeCapOver,			///< some type over its cap: the path is not in the estimate
			eTypeCapNeedsSplit,		///< a counted connection endpoint has no defined type and no split (strategy excluded)
			eTypeCapSplit			///< DL-481: a counted endpoint has several types and a split; the caps allow only SOME of them, so the connection must be priced by AllowedTypeProduct
		};

		/// DL-481: the strategy prices the path (OK or Split); Over and
		/// NeedsSplit do not.
		inline bool TypeCapAdmits( const TypeCapStatus s )
		{
			return s == eTypeCapOK || s == eTypeCapSplit;
		}

		/// DL-481: an endpoint whose material has several possible
		/// non-delta lobe types, one of them capped, but declares a split
		/// of its connection value by lobe label
		/// (IMaterial::HasConnectionTypeSplit).
		inline bool EndpointHasTypeSplit( const BDPTVertex& v )
		{
			return v.type == BDPTVertex::SURFACE && !v.isBSSRDFEntry && v.pMaterial &&
				v.pMaterial->HasConnectionTypeSplit();
		}

		/// DL-481: what a strategy's per-type cap check leaves to price.
		/// `base` counts every counted vertex whose type is defined (its
		/// walk's label, or the endpoint material's single type); the up
		/// to two split endpoints (`light` = the light-side endpoint,
		/// `eye` = the eye-side one) each contribute exactly one of the
		/// types in their mask -- the label of the lobe that scatters
		/// there -- and the connection value is the sum over the allowed
		/// combinations of the per-label values (AllowedTypeProduct).
		struct TypeCapPlan
		{
			BounceTypeCounts	base;
			bool				splitLight;
			bool				splitEye;
			unsigned int		maskLight;
			unsigned int		maskEye;
			TypeCapPlan() : splitLight( false ), splitEye( false ), maskLight( 0 ), maskEye( 0 ) {}
		};

		/// Is `base` plus one count of type tl (0 = none) and one of te
		/// within the caps?
		inline bool TypeComboWithin( const BounceTypeCounts& base, const unsigned int tl, const unsigned int te,
			const BounceTypeCaps& caps )
		{
			BounceTypeCounts c = base;
			if( tl ) c.Add( tl );
			if( te ) c.Add( te );
			return c.Within( caps );
		}

		/// Resolve a plan whose endpoints are recorded: Over when no
		/// combination is allowed, OK when every one is (the aggregate
		/// value is then exact), Split otherwise.
		inline TypeCapStatus ResolveTypeCapPlan( const TypeCapPlan& plan, const BounceTypeCaps& caps )
		{
			if( !plan.base.Within( caps ) ) {
				return eTypeCapOver;
			}
			if( !plan.splitLight && !plan.splitEye ) {
				return eTypeCapOK;
			}
			bool any = false, all = true;
			for( unsigned int tl = 0; tl <= ScatteredRay::eRayTranslucent; tl++ ) {
				if( plan.splitLight ? !( plan.maskLight & ( 1u << tl ) ) : tl != 0 ) continue;
				for( unsigned int te = 0; te <= ScatteredRay::eRayTranslucent; te++ ) {
					if( plan.splitEye ? !( plan.maskEye & ( 1u << te ) ) : te != 0 ) continue;
					if( TypeComboWithin( plan.base, tl, te, caps ) ) any = true; else all = false;
				}
			}
			return !any ? eTypeCapOver : ( all ? eTypeCapOK : eTypeCapSplit );
		}

		/// DL-481: the connection value of a Split strategy -- the sum over
		/// the allowed label combinations of the split endpoints' per-label
		/// values (IBSDF::valueByScatterType, area measure).  `fl` / `fe`
		/// are read only for a split endpoint; the caller multiplies the
		/// result in place of the split endpoints' aggregate values (an
		/// unsplit endpoint keeps its aggregate value, its type is in
		/// `base`).
		template<class V>
		inline V AllowedTypeProduct( const TypeCapPlan& plan, const BounceTypeCaps& caps, const V fl[5], const V fe[5] )
		{
			V sum = V( Scalar( 0 ) );
			for( unsigned int tl = 0; tl <= ScatteredRay::eRayTranslucent; tl++ ) {
				if( plan.splitLight ? !( plan.maskLight & ( 1u << tl ) ) : tl != 0 ) continue;
				for( unsigned int te = 0; te <= ScatteredRay::eRayTranslucent; te++ ) {
					if( plan.splitEye ? !( plan.maskEye & ( 1u << te ) ) : te != 0 ) continue;
					if( !TypeComboWithin( plan.base, tl, te, caps ) ) continue;
					const V a = plan.splitLight ? fl[tl] : V( Scalar( 1 ) );
					const V b = plan.splitEye ? fe[te] : V( Scalar( 1 ) );
					sum = sum + a * b;
				}
			}
			return sum;
		}

		/// The type a CONNECTION ENDPOINT contributes when it is counted.
		/// From the material's possible non-delta lobe types
		/// (IMaterial::ConnectionScatterTypes): none capped -> counts nothing;
		/// a single possible type that is capped -> that type; otherwise false.
		/// Ordinary SSS reflection has its own label; jumps are charged separately.
		/// Medium vertices, subsurface entries and roots contribute no type.
		/// A property of the material and the caps only, never of a sampled
		/// lobe, so every strategy of a path agrees on it.
		inline bool EndpointBounceType( const BDPTVertex& v, const BounceTypeCaps& caps, unsigned int& type )
		{
			type = 0;
			if( v.type != BDPTVertex::SURFACE || v.isBSSRDFEntry || !v.pMaterial ) {
				return true;
			}
			const unsigned int mask = v.pMaterial->ConnectionScatterTypes();
			if( ( mask & caps.boundMask ) == 0 ) {
				return true;
			}
			if( ( mask & ( mask - 1u ) ) != 0 ) {
				return false;
			}
			for( unsigned int t = ScatteredRay::eRayDiffuse; t <= ScatteredRay::eRayTranslucent; t++ ) {
				if( mask == ( 1u << t ) ) {
					type = t;
				}
			}
			return true;
		}

		/// Does a counted connection endpoint at `v` need a per-lobe-type
		/// split the material does not provide?  (False when no cap is
		/// set.)  A MATERIAL property of a connectible surface vertex --
		/// never of its sampled lobe -- so every strategy of a path agrees
		/// on it.
		inline bool EndpointNeedsTypeSplit( const BDPTVertex& v, const BounceTypeCaps& caps )
		{
			if( !caps.active || !v.isConnectible ) {
				return false;
			}
			unsigned int type = 0;
			return !EndpointBounceType( v, caps, type ) && !EndpointHasTypeSplit( v );
		}

		/// VCM (DL-471): does any of verts[from .. to] (path positions >= 2,
		/// i.e. counted) need a type split?  VCM's recurrence MIS cannot drop
		/// the individual strategies at such a vertex, so a path that has one
		/// is estimated by the s = 0 and s = 1 strategies alone, which never
		/// have a counted endpoint (VCMIntegrator.cpp, "DL-471").
		inline bool AnyNeedsTypeSplit(
			const BDPTVertex* verts,
			const unsigned int from,
			const unsigned int to,
			const BounceTypeCaps& caps
			)
		{
			if( !caps.active ) {
				return false;
			}
			for( unsigned int q = from; q <= to; q++ ) {
				if( EndpointNeedsTypeSplit( verts[q], caps ) ) {
					return true;
				}
			}
			return false;
		}

		/// Per-type cap status of strategy (s, t): the path is
		/// lightVerts[0 .. s-1] followed by eyeVerts[t-1 .. 0] (BDPT's
		/// MISWeight numbering; lightVerts is not read when s <= 1 and
		/// eyeVerts[0], the camera, is never read).  Its position q from the
		/// light root is the x_{K+1-q} above: q = 1 is x_K.  Interior
		/// vertices count the type their walk counted (`capType`), x_K is
		/// free unless its own scatter was a delta lobe or it has no BSDF,
		/// and the two connection endpoints count their material's type
		/// unless they ARE x_K.
		inline TypeCapStatus JoinedTypeCapPlan(
			const BDPTVertex* lightVerts,
			const BDPTVertex* eyeVerts,
			const unsigned int s,
			const unsigned int t,
			const BounceTypeCaps& caps,
			TypeCapPlan& plan
			)
		{
			plan = TypeCapPlan();
			if( !caps.active || s + t < 3 ) {
				return eTypeCapOK;
			}
			const unsigned int n = s + t;
			for( unsigned int q = 1; q + 1 < n; q++ )
			{
				const BDPTVertex& v = ( q < s ) ? lightVerts[q] : eyeVerts[n - 1 - q];
				const bool endpoint = s >= 1 && ( q + 1 == s || q == s );
				// The relocation edge is counted even when its angular exit is free.
				if( v.isBSSRDFEntry ) plan.base.Add( ScatteredRay::eRayTranslucent );
				if( q == 1 ) {
					if( endpoint || ( !v.isDelta && v.isConnectible ) ) {
						continue;
					}
					plan.base.Add( v.capType );
					continue;
				}
				if( endpoint ) {
					unsigned int type = 0;
					if( !EndpointBounceType( v, caps, type ) ) {
						// DL-481: several possible types, one of them capped.
						if( !EndpointHasTypeSplit( v ) ) {
							return eTypeCapNeedsSplit;
						}
						if( q + 1 == s ) {
							plan.splitLight = true;
							plan.maskLight = v.pMaterial->ConnectionScatterTypes();
						} else {
							plan.splitEye = true;
							plan.maskEye = v.pMaterial->ConnectionScatterTypes();
						}
						continue;
					}
					plan.base.Add( type );
				} else {
					plan.base.Add( v.capType );
				}
			}
			return ResolveTypeCapPlan( plan, caps );
		}

		/// The status alone (see JoinedTypeCapPlan; Split admits the
		/// strategy, priced by AllowedTypeProduct).
		inline TypeCapStatus JoinedTypeCapStatus(
			const BDPTVertex* lightVerts,
			const BDPTVertex* eyeVerts,
			const unsigned int s,
			const unsigned int t,
			const BounceTypeCaps& caps
			)
		{
			TypeCapPlan plan;
			return JoinedTypeCapPlan( lightVerts, eyeVerts, s, t, caps, plan );
		}

		/// Light-subpath prefix counts for a stored light vertex: the types
		/// the light walk counted at lightVerts[1 .. k-1] (its free first
		/// vertex already uncounted), i.e. everything a MERGE at
		/// lightVerts[k] keeps of the light subpath.  Saturates at 255 (a
		/// count above every finite cap the walk could have reached).
		inline void LightPrefixTypeCounts(
			const BDPTVertex* lightVerts,
			const unsigned int k,
			unsigned char out[4]
			)
		{
			BounceTypeCounts c;
			for( unsigned int q = 1; q < k; q++ ) {
				c.Add( lightVerts[q].capType );
				if( lightVerts[q].isBSSRDFEntry ) c.Add( ScatteredRay::eRayTranslucent );
			}
			if( k > 0 && lightVerts[k].isBSSRDFEntry ) c.Add( ScatteredRay::eRayTranslucent );
			for( int j = 0; j < 4; j++ ) {
				out[j] = static_cast<unsigned char>( c.n[j] > 255u ? 255u : c.n[j] );
			}
		}

		/// Does a light subpath have a vertex needing a type split at
		/// positions 2 .. k-1 -- what a MERGE at lightVerts[k] keeps of it
		/// (stored on the LightVertex)?
		inline bool LightPrefixNeedsTypeSplit( const BDPTVertex* lightVerts, const unsigned int k, const BounceTypeCaps& caps )
		{
			return k >= 3 && AnyNeedsTypeSplit( lightVerts, 2, k - 1, caps );
		}

		/// Per-type cap status of a MERGE of eye vertex eyeVerts[i] with
		/// the stored light vertex lightVerts[k] (prefix counts from
		/// LightPrefixTypeCounts): the path is camera, eyeVerts[1 .. i]
		/// (the merge point, priced by the EYE vertex's BSDF), then
		/// lightVerts[k-1 .. 1] and the root.  The merge point is x_K, and
		/// free, when k == 1.
		inline TypeCapStatus MergeTypeCapPlan(
			const BDPTVertex* eyeVerts,
			const unsigned int i,
			const unsigned char lightPrefix[4],
			const unsigned int k,
			const BounceTypeCaps& caps,
			TypeCapPlan& plan
			)
		{
			plan = TypeCapPlan();
			if( !caps.active ) {
				return eTypeCapOK;
			}
			for( int j = 0; j < 4; j++ ) {
				plan.base.n[j] = lightPrefix[j];
			}
			for( unsigned int q = 1; q < i; q++ ) {
				plan.base.Add( eyeVerts[q].capType );
				if( eyeVerts[q].isBSSRDFEntry ) plan.base.Add( ScatteredRay::eRayTranslucent );
			}
			if( i > 0 && eyeVerts[i].isBSSRDFEntry ) plan.base.Add( ScatteredRay::eRayTranslucent );
			if( k >= 2 ) {
				unsigned int type = 0;
				if( !EndpointBounceType( eyeVerts[i], caps, type ) ) {
					// DL-481: the merge point is priced by the EYE vertex's
					// BSDF -- the eye-side endpoint of the plan.
					if( !EndpointHasTypeSplit( eyeVerts[i] ) ) {
						return eTypeCapNeedsSplit;
					}
					plan.splitEye = true;
					plan.maskEye = eyeVerts[i].pMaterial->ConnectionScatterTypes();
				} else {
					plan.base.Add( type );
				}
			}
			return ResolveTypeCapPlan( plan, caps );
		}

		inline TypeCapStatus MergeTypeCapStatus(
			const BDPTVertex* eyeVerts,
			const unsigned int i,
			const unsigned char lightPrefix[4],
			const unsigned int k,
			const BounceTypeCaps& caps
			)
		{
			TypeCapPlan plan;
			return MergeTypeCapPlan( eyeVerts, i, lightPrefix, k, caps, plan );
		}

		/// DL-330 (review P1).  Is verts[1..j-1] a straight chain of delta
		/// PASS-THROUGHS (thin-weave gap draws) from a delta-position light
		/// root verts[0] to verts[j]?  That is exactly the light-side shape
		/// BDPT's see-through s = 1 connection reaches: an eye vertex at
		/// verts[j] connects to the root through those gaps
		/// (`CastShadowRayAutoSampled`, which crosses at most
		/// kShadowWalkMaxCrossings = 31 surfaces).  The chain is
		/// a function of the path: each gap draw continues the incoming ray
		/// undeviated, so the segment directions must all equal
		/// root -> verts[j].
		/// Does `out` continue `in` undeviated?  The straightness test
		/// DeltaPassThroughChainToRoot applies to every segment of a gap
		/// chain, shared with the light walk's gap-draw recording
		/// (BDPTVertex::passThroughProb) so the two sides cannot disagree
		/// on which delta draws are pass-throughs.  Unnormalized inputs.
		inline bool IsStraightContinuation( const Vector3& in, const Vector3& out )
		{
			const Scalar l2 = Vector3Ops::SquaredModulus( in ) * Vector3Ops::SquaredModulus( out );
			if( !( l2 > 0 ) ) {
				return false;
			}
			return Vector3Ops::Dot( in, out ) >= ( Scalar( 1 ) - Scalar( 1e-9 ) ) * sqrt( l2 );
		}

		inline bool DeltaPassThroughChainToRoot(
			const std::vector<BDPTVertex>& verts,
			const std::size_t j
			)
		{
			if( j < 2 || j - 1 > kShadowWalkMaxCrossings || j >= verts.size() ) {
				return false;
			}
			const BDPTVertex& root = verts[0];
			if( root.type != BDPTVertex::LIGHT || !root.isDelta || !root.pLight || root.pEnvLight ) {
				return false;
			}
			const Vector3 d0 = Vector3Ops::Normalize(
				Vector3Ops::mkVector3( verts[j].position, root.position ) );
			for( std::size_t i = 1; i <= j; i++ ) {
				if( i < j ) {
					const BDPTVertex& v = verts[i];
					if( v.type != BDPTVertex::SURFACE || !v.isDelta || !v.pMaterial ||
						!v.pMaterial->HasDeltaPassThrough() ) {
						return false;
					}
				}
				const Vector3 di = Vector3Ops::Normalize(
					Vector3Ops::mkVector3( verts[i].position, verts[i - 1].position ) );
				if( Vector3Ops::Dot( di, d0 ) < Scalar( 1 ) - Scalar( 1e-9 ) ) {
					return false;
				}
			}
			return true;
		}

		/// Largest split index `w` where the eye family has a strategy for a
		/// light-walk jump `verts[p] -> verts[p+1]` along segment `verts[s..p]`
		/// (ignoring depth caps), or -1 if none exists:
		///   w = 0: eye hits non-delta root (`s == 0`);
		///   merge at `verts[w]`: `mergingActive` and non-delta surface;
		///   connection/NEE on `(w-1, w)`: `verts[w]` connectible non-delta and
		///     `verts[w-1]` is root, connectible non-delta, or (with
		///     `seeThroughNEE` and `s == 0`) a `DeltaPassThroughChainToRoot`.
		inline int LightSegmentEyeWitness(
			const std::vector<BDPTVertex>& verts,
			const std::size_t s,
			const std::size_t p,
			const bool mergingActive,
			const bool seeThroughNEE = false
			)
		{
			if( p + 1 >= verts.size() || p <= s ) {
				return -1;	// malformed: no entry pair inside the segment
			}
			for( std::size_t j = p; j > s; j-- ) {
				const BDPTVertex& b = verts[j];
				const bool bUsable = ( j == p )
					? verts[p + 1].isConnectible
					: ( b.isConnectible && !b.isDelta );
				if( !bUsable ) {
					continue;
				}
				if( mergingActive && b.type == BDPTVertex::SURFACE ) {
					return static_cast<int>( j );
				}
				const BDPTVertex& a = verts[j - 1];
				if( j - 1 == 0 && a.type == BDPTVertex::LIGHT ) {
					return static_cast<int>( j );
				}
				if( a.isConnectible && !a.isDelta ) {
					return static_cast<int>( j );
				}
				if( seeThroughNEE && s == 0 && DeltaPassThroughChainToRoot( verts, j ) ) {
					return static_cast<int>( j );
				}
			}
			if( s == 0 && verts[0].type == BDPTVertex::LIGHT && !verts[0].isDelta ) {
				return 0;
			}
			return -1;
		}

		/// The dominant eye-coverable light-side jump at or before a
		/// light vertex (see the DL-380 block above), reduced to what the
		/// cap test needs.
		struct LightJumpCover
		{
			bool			exists;			///< some jump in the prefix has a witness
			int				surfaceBefore;	///< L(w-1); -1 for an area-light root hit (the hit counts)
			unsigned int	volumeBefore;	///< medium vertices in verts[0..w-1]
			bool			escape;			///< the eye escapes to an environment root (one iteration)

			LightJumpCover() : exists( false ), surfaceBefore( 0 ), volumeBefore( 0 ), escape( false ) {}
		};

		/// Can the eye walk generate a subpath with `eyeSurface` surface
		/// hits and `eyeVolume` medium scatters (plus an escape)?
		inline bool EyeWalkCanGenerate(
			const long long eyeSurface,
			const long long eyeVolume,
			const bool escape,
			const EyeWalkCaps& caps
			)
		{
			return eyeSurface <= static_cast<long long>( caps.maxSurface ) &&
				eyeSurface + eyeVolume + ( escape ? 1 : 0 ) <= static_cast<long long>( caps.maxIterations );
		}

		/// Can the eye walk generate the subpath that covers a jump with
		/// witness `c`, on a path of eye-walk surface count
		/// `pathSurface` (D) and `pathVolume` medium vertices?  True means
		/// the EYE family owns the path and the light family must not
		/// count it.
		inline bool EyeFamilyCovers(
			const LightJumpCover& c,
			const unsigned int pathSurface,
			const unsigned int pathVolume,
			const EyeWalkCaps& caps
			)
		{
			if( !c.exists ) {
				return false;
			}
			return EyeWalkCanGenerate(
				static_cast<long long>( pathSurface ) - c.surfaceBefore,
				static_cast<long long>( pathVolume ) - static_cast<long long>( c.volumeBefore ),
				c.escape, caps );
		}

		/// Per light subpath: L(k) and the dominant cover at every vertex.
		/// Reused through thread-local scratch by BDPT and VCM, so `Build`
		/// never reallocates once warm.
		class LightJumpPartition
		{
		public:
			LightJumpPartition() : anyJump( false ) {}

			void Build(
				const std::vector<BDPTVertex>& verts,
				const bool mergingActive,
				const bool seeThroughNEE = false	///< DL-330: see LightSegmentEyeWitness
				)
			{
				const std::size_t n = verts.size();
				surfaceCount.assign( n, 0u );
				cover.assign( n, LightJumpCover() );
				anyJump = false;
				unsigned int L = 0;
				std::size_t segmentStart = 0;
				LightJumpCover running;
				for( std::size_t i = 0; i < n; i++ ) {
					if( i > 0 && CountsAsSurfaceHit( verts[i] ) ) {
						L++;
					}
					surfaceCount[i] = L;
					if( i > 0 && verts[i].isBSSRDFEntry ) {
						anyJump = true;
						const int w = LightSegmentEyeWitness( verts, segmentStart, i - 1, mergingActive, seeThroughNEE );
						if( w >= 1 ) {
							running.exists = true;
							running.surfaceBefore = static_cast<int>( surfaceCount[w - 1] );
							running.volumeBefore = verts[w - 1].volumeBounces;
							running.escape = false;
						} else if( w == 0 && !running.exists ) {
							running.exists = true;
							running.surfaceBefore = verts[0].pEnvLight ? 0 : -1;
							running.volumeBefore = 0;
							running.escape = verts[0].pEnvLight != 0;
						}
						segmentStart = i;
					}
					cover[i] = running;
				}
			}

			bool AnyJump() const { return anyJump; }

			//! L(k): light-walk surface count of verts[1..k].
			unsigned int SurfaceCount( const std::size_t k ) const { return surfaceCount[k]; }

			//! Dominant cover of the jumps whose entry is at or before k.
			const LightJumpCover& Cover( const std::size_t k ) const { return cover[k]; }

			/// Does the LIGHT family keep the path whose light part ends at
			/// verts[k] (a connection or splat, or -- `merge` -- a merge AT
			/// verts[k], which shares that vertex with the eye part)?
			/// `eyeSurface` is the eye part's own eye-walk surface count,
			/// `pathVolume` the path's medium-vertex total.
			bool Keeps(
				const std::size_t k,
				const unsigned int eyeSurface,
				const unsigned int pathVolume,
				const EyeWalkCaps& caps,
				const bool merge = false
				) const
			{
				if( !anyJump ) {
					return true;
				}
				const unsigned int D = eyeSurface + surfaceCount[k] - ( merge && surfaceCount[k] > 0 ? 1u : 0u );
				return !EyeFamilyCovers( cover[k], D, pathVolume, caps );
			}

		private:
			std::vector<unsigned int>	surfaceCount;
			std::vector<LightJumpCover>	cover;
			bool						anyJump;
		};
	}
}

#endif
