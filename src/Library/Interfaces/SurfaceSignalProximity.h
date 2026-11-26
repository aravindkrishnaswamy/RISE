//////////////////////////////////////////////////////////////////////
//
//  SurfaceSignalProximity.h - The bodies of SurfaceSignalInfo::Proximity
//  and SurfaceSignalInfo::Interior, the CROSS-OBJECT PAIR of the
//  shading-signal family.
//
//  docs/CROSS_OBJECT_PROXIMITY_DESIGN.md.  `proximity(r)` answers "how
//  close is the nearest OTHER surface", which is the quantity contact
//  grime actually is -- dirt collecting where a nail rests on a plank,
//  dust where a wall meets a floor.  `interior(r)` (§5.6) is its SIGNED
//  sibling: "how deep inside another object is this point", which is what
//  burial looks like -- the sunk part of a nail, the embedded flank of a
//  stone in mortar.  A second builtin rather than a sign on the first, so
//  that together they cover the signed distance without a sign convention
//  an author has to remember.  Their three siblings
//  (occlusion / thickness / convexity) are SELF-signals: they ask the hit
//  geometry about its own shape and never see the rest of the scene.
//
//  WHY THIS IS A FILE OF ITS OWN, and not a handful more lines in
//  ISurfaceSignalProvider.h next to the struct it belongs to.  The bodies
//  call `IObjectManager::NearestOtherSurface` and
//  `IObjectManager::DeepestOtherContainment`, so they need
//  IObjectManager.h -- and IObjectManager.h reaches IObject.h, which
//  includes RayIntersection.h at its bottom, which includes
//  RayIntersectionGeometric.h, which includes ISurfaceSignalProvider.h.
//  An include there is therefore a cycle: whichever of the two a
//  translation unit names first, the other's guard is already set by the
//  time the body is compiled, and `IObjectManager` is incomplete.  This
//  header sits ABOVE both and may include them, which resolves it with
//  no forward-declaration tricks and no pointer laundering.
//
//  WHO MUST INCLUDE IT: any translation unit that CALLS Proximity or
//  Interior.  In the shipped tree that is exactly one -- the expression
//  VM's `CallFunc` in Painters/ExpressionEval.h -- plus the tests.  A unit
//  that merely names `SurfaceSignalInfo` needs nothing; omitting this
//  where it is needed is a link error, which is the loud kind.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SURFACE_SIGNAL_PROXIMITY_
#define SURFACE_SIGNAL_PROXIMITY_

#include "ISurfaceSignalProvider.h"
#include "IObjectManager.h"
#include "../Utilities/OrthonormalBasis3D.h"
#include "../Utilities/Transformable.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace RISE
{
	//! THE SECOND AND THIRD POLICY BODIES beside `SignalQuery`, not further
	//! branches of it -- see `SurfaceSignalInfo::SignalKind`'s comment for
	//! why they cannot be merged (they have different preconditions:
	//! `SignalQuery` short-circuits on a null `pProvider`, and a
	//! `box_geometry` receiver has none, which is precisely the case these
	//! signals must answer for).
	//!
	//! All three DO share the L1 memo, through `MakeL1Key` and the same
	//! find-compute-insert shape, so a new key field reaches every one of
	//! them and none can be served another's entry (`fn` separates them).
	//!
	//! WHAT IS MEMOISED is the FINAL answer -- clamped, fallen back,
	//! guarded -- so a memo hit and a memo miss are indistinguishable by
	//! construction.  Like `SignalQuery`, this returns a stored `double`
	//! bit for bit on a hit and does no arithmetic of its own around the
	//! lookup, which is the property ExpressionMemo.h's FP contract
	//! requires of anything sitting inside the VM's arithmetic path.
	//!
	//! `bRadiusIsConstant` goes into the key as a constant FALSE.  There is
	//! no baked path behind this signal for a compile-time proof to feed,
	//! so every proximity entry carries the same value in that field and it
	//! never separates two of them -- `fn`, the radius and the hit do all
	//! the separating.  It is written explicitly rather than left to
	//! whatever a default would be, because an uninitialised bool in a key
	//! compared by `Equals` is a cache that misses at random.
	inline Scalar SurfaceSignalInfo::Proximity( const Scalar radiusWorld ) const
	{
		const ExpressionMemo::SignalKey key = MakeL1Key( eProximity, radiusWorld, false );

		Scalar memo = Scalar( 0 );
		if( ExpressionMemo::L1Find( key, memo ) ) return memo;

		Scalar out = NeutralProximity();

		// THE PRECONDITIONS, all four, and each is a real case rather than
		// defensive padding:
		//   pScene   0 on any record the object manager did not produce --
		//            the GUI's painter preview, a hand-built test record,
		//            a BSSRDF entry vertex, or an emitter record whose
		//            probe REFUSED.  Since the round-2 transport review of S3 that
		//            means an UNBOUNDED luminary, a self-hit floor wider than the
		//            luminary itself, or a second surface of the same luminary
		//            inside the standoff band AND FURTHER than the acceptance
		//            tolerance from the sampled point -- NOT "a concave luminary",
		//            which was the along-`vToLight` NEE probe's failure and went
		//            away when the probe was unified on the normal-aligned one
		//            (LightSampler::ProbeEmitterSurface).  A second surface INSIDE
		//            that tolerance is not on this list at all: it is ACCEPTED,
		//            stamping `pScene` with the neighbour's own live channel -- a
		//            silent, bounded read (at most one standoff, <= 0.5 % of `D`,
		//            away), not a neutral one -- and the beyond-tolerance refusal
		//            is unreachable for any luminary whose self-hit floor is
		//            <= 0.0089 * D (every analytic primitive; every SDF at
		//            `epsFrac` <= 0.00446, covering both the 5e-5 default and
		//            family E's 0.002).  Two record kinds LEFT
		//            this list on 2026-09-11
		//            (SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md): a
		//            BDPT / VCM / MLT rebuild, which forwards the stamp
		//            through PathVertexEval (§3), and an ACCEPTED emitter
		//            record, on which LightSampler::ProbeEmitterSurface
		//            stamps the triple from a real probe hit (§5).
		//   pSelf    0 on a MISS (nothing was hit, so there is no receiver
		//            and no self to exclude).
		//   radius   the runtime half of the parse-time literal check, for
		//            a COMPUTED radius that lands <= 0 or non-finite.
		//   ptWorld  a non-finite hit point cannot be measured from, and
		//            the manager would reject it anyway -- checked here so
		//            the neutral is reached without a call.
		if( pScene && pSelf && RadiusUsable( radiusWorld )
		 && RISE::IsFiniteDouble( static_cast<double>( ptWorld.x ) )
		 && RISE::IsFiniteDouble( static_cast<double>( ptWorld.y ) )
		 && RISE::IsFiniteDouble( static_cast<double>( ptWorld.z ) ) )
		{
			Scalar d = Scalar( 0 );
			if( pScene->NearestOtherSurface( ptWorld, pSelf, radiusWorld, d )
			 && RISE::IsFiniteDouble( static_cast<double>( d ) ) )
			{
				// `1 - d/r`: 1 at contact, falling linearly to 0 at the
				// radius.  The clamp is belt-and-braces -- the manager
				// already bounds `d` to **[0, r)**, zero INCLUDED
				// (interpenetration is contact: the solid families clamp
				// their signed field at zero) and `r` EXCLUDED (a candidate
				// is accepted only on `d < best`, and `best` starts at the
				// radius) -- and costs nothing, but it is what lets every
				// caller treat the result as [0,1] without knowing that.
				const Scalar v = Scalar( 1 ) - d / radiusWorld;
				out = ( v < Scalar( 0 ) ) ? Scalar( 0 ) : ( ( v > Scalar( 1 ) ) ? Scalar( 1 ) : v );
			}
		}

		ExpressionMemo::L1Insert( key, out );
		return out;
	}

	//! THE SIGNED SIBLING.  Same shape, same memo, same preconditions --
	//! and the same reason for every one of them, so read `Proximity`'s
	//! comments above for the argument; only the differences are noted
	//! here.
	//!
	//!   * The query is `DeepestOtherContainment`, whose running quantity
	//!     is a MAXIMUM over the objects that CONTAIN the point rather
	//!     than a minimum over the ones near it, and which has no
	//!     refusal diagnostic (every sheet family refuses containment
	//!     everywhere by design -- see its contract comment).
	//!   * The mapping is `depth / r`, not `1 - d/r`: 0 means "inside no
	//!     neighbour" and 1 means "at least `r` deep".  Both ends are the
	//!     do-nothing / fully-buried ends an author expects, and the
	//!     manager already bounds `depth` below by 0, so only the upper
	//!     clamp can bite -- it is written both ways anyway, for the same
	//!     belt-and-braces reason.
	//!   * `r` is MANDATORY and a WORLD LENGTH, with no `DynR` twin, for
	//!     the same reason `proximity` has none: there is no bake behind
	//!     this signal for a constant-radius proof to feed.
	//!
	//! WHAT IS MEMOISED is again the FINAL answer, so a hit and a miss are
	//! indistinguishable, and the body does no arithmetic around the
	//! lookup -- the FP contract ExpressionMemo.h requires of anything
	//! inside the VM's arithmetic path.
	inline Scalar SurfaceSignalInfo::Interior( const Scalar radiusWorld ) const
	{
		const ExpressionMemo::SignalKey key = MakeL1Key( eInterior, radiusWorld, false );

		Scalar memo = Scalar( 0 );
		if( ExpressionMemo::L1Find( key, memo ) ) return memo;

		Scalar out = NeutralInterior();

		if( pScene && pSelf && RadiusUsable( radiusWorld )
		 && RISE::IsFiniteDouble( static_cast<double>( ptWorld.x ) )
		 && RISE::IsFiniteDouble( static_cast<double>( ptWorld.y ) )
		 && RISE::IsFiniteDouble( static_cast<double>( ptWorld.z ) ) )
		{
			Scalar depth = Scalar( 0 );
			if( pScene->DeepestOtherContainment( ptWorld, pSelf, radiusWorld, depth )
			 && RISE::IsFiniteDouble( static_cast<double>( depth ) ) )
			{
				const Scalar v = depth / radiusWorld;
				out = ( v < Scalar( 0 ) ) ? Scalar( 0 ) : ( ( v > Scalar( 1 ) ) ? Scalar( 1 ) : v );
			}
		}

		ExpressionMemo::L1Insert( key, out );
		return out;
	}

	//! Ray-traced ambient occlusion (`osl_ao(distance, samples, normal, flags)`).
	//! Traces `samples` (clamped [1, 128]) cosine-weighted low-discrepancy
	//! (Hammersley + 2D Cranley-Patterson hash) hemisphere rays from the hit's
	//! centre world point (`ptWorld`), returning the unoccluded fraction in
	//! [0, 1].
	//!
	//! * Scene-wide (`only_local == false`): uses `pScene->IntersectShadowRay`
	//!   (honours `visible` + `casts_shadows`, evaluates no shaders, tests both
	//!   front and back faces so `inside == true` hits interior walls).
	//! * Local (`only_local == true`): uses `pSelf->IntersectRay_IntersectionOnly`
	//!   (ignores `casts_shadows`, matching Cycles `scene_intersect_local`,
	//!   Round 3 P2-4).
	//! * Relief taps: `ptWorld` is the centre hit's world point (untouched by
	//!   `ReliefModifier`), so `osl_ao` alone has zero spurious bump gradient
	//!   while `Color * osl_ao(...)` preserves Color's gradient (MIX §III.3).
	inline Scalar SurfaceSignalInfo::AmbientOcclusion(
		const Scalar distanceWorld,
		const int numSamples,
		const Vector3& customNormal,
		const int flags,
		const Vector3& shadingNormal,
		const Vector3& geomNormal,
		const bool geomOpposesArrival
		) const
	{
		// Per-thread re-entrancy guard (MIX §III.3): never recurse if a future
		// traversal path ever evaluated a painter during a shadow query.
		bool& inAO = SurfaceSignalInAmbientOcclusion();
		if( inAO ) {
			return Scalar( 1 );
		}
		struct ScopeGuard
		{
			bool& flag;
			explicit ScopeGuard( bool& f ) : flag( f ) { flag = true; }
			~ScopeGuard() { flag = false; }
		} guard( inAO );

		const bool bInside       = ( flags & 1 ) != 0;
		const bool bOnlyLocal    = ( flags & 2 ) != 0;
		const bool bCustomNormal = ( flags & 4 ) != 0;

		if( !pScene || !pSelf || numSamples < 1 ) {
			return Scalar( 1 );
		}
		if( !RISE::IsFiniteDouble( static_cast<double>( distanceWorld ) ) || !( distanceWorld > Scalar( 0 ) ) ) {
			return Scalar( 1 );
		}
		if( !RISE::IsFiniteDouble( static_cast<double>( ptWorld.x ) ) ||
			!RISE::IsFiniteDouble( static_cast<double>( ptWorld.y ) ) ||
			!RISE::IsFiniteDouble( static_cast<double>( ptWorld.z ) ) )
		{
			return Scalar( 1 );
		}

		const int N = ( numSamples > 128 ) ? 128 : numSamples;

		auto safeNorm = []( const Vector3& v ) -> Vector3 {
			const Scalar l2 = v.x * v.x + v.y * v.y + v.z * v.z;
			if( !( l2 > Scalar( 1e-24 ) ) || !RISE::IsFiniteDouble( static_cast<double>( l2 ) ) ) {
				return Vector3( 0, 0, 0 );
			}
			const Scalar inv = Scalar( 1 ) / std::sqrt( l2 );
			return Vector3( v.x * inv, v.y * inv, v.z * inv );
		};

		const Vector3 ngUnit = safeNorm( geomNormal );
		const bool hasNg = ( ngUnit.x != Scalar( 0 ) || ngUnit.y != Scalar( 0 ) || ngUnit.z != Scalar( 0 ) );

		Vector3 hemiN;
		if( bCustomNormal ) {
			hemiN = safeNorm( customNormal );
		} else {
			hemiN = safeNorm( shadingNormal );
			if( hasNg ) {
				// Orient default shading normal to the side the ray arrived from
				// (Cycles sd->N convention, MIX §III.3); fall back to geometric
				// face side if shadingNormal is degenerate.
				const Vector3 faceSide = geomOpposesArrival ? ngUnit : -ngUnit;
				if( hemiN.x == Scalar( 0 ) && hemiN.y == Scalar( 0 ) && hemiN.z == Scalar( 0 ) ) {
					hemiN = faceSide;
				} else if( Vector3Ops::Dot( hemiN, faceSide ) < Scalar( 0 ) ) {
					hemiN = -hemiN;
				}
			}
		}
		if( hemiN.x == Scalar( 0 ) && hemiN.y == Scalar( 0 ) && hemiN.z == Scalar( 0 ) ) {
			return Scalar( 1 );
		}
		if( bInside ) {
			hemiN = -hemiN;
		}

		// Offset ray origin slightly along the geometric normal oriented into
		// `hemiN` (falling back to the arrival side when customNormal is
		// near-tangent to the geometric plane, or `hemiN` when no geometric
		// normal is present).  Combined with `ScopedSelfPrimExclusion` for
		// indexed meshes, `1e-7 * posScale` clears `SURFACE_INTERSEC_ERROR`
		// cleanly without jumping over thin contact gaps at large coordinates.
		Vector3 offsetDir = hemiN;
		if( hasNg ) {
			const Scalar ngDot = Vector3Ops::Dot( ngUnit, hemiN );
			if( std::fabs( ngDot ) > Scalar( 1e-3 ) ) {
				offsetDir = ( ngDot > Scalar( 0 ) ) ? ngUnit : -ngUnit;
			} else {
				offsetDir = geomOpposesArrival ? ngUnit : -ngUnit;
			}
		}
		const Scalar posScale = Scalar( 1 ) + std::fabs( ptWorld.x ) + std::fabs( ptWorld.y ) + std::fabs( ptWorld.z );
		const Scalar eps = std::min( distanceWorld * Scalar( 1e-3 ), Scalar( 1e-7 ) * posScale );
		const Point3 rayOrigin = Point3Ops::mkPoint3( ptWorld, offsetDir * eps );
		// Cap rayMaxDist to prevent float narrowing / non-uniform scale overflow in BVH.
		const Scalar rayMaxDist = std::min( distanceWorld - eps, Scalar( 1e30 ) );
		if( !( rayMaxDist > Scalar( 0 ) ) ) {
			return Scalar( 1 );
		}

		// Per-thread 1-way AO cache inside ExpressionMemo::Tables (MIX §III.3):
		// eliminates 4x redundant ray-tracing inside ReliefModifier taps and
		// N-x reuse when one AO node feeds multiple material slots at the same
		// hit while staying trivially default-constructible and within the
		// 4,096 B per-thread ExpressionMemo TLS budget.
		ExpressionMemo::AOCacheKey aoKey;
		aoKey.pScene = (const void*)pScene;
		aoKey.pSelf  = (const void*)pSelf;
		aoKey.pProvider = (const void*)pProvider;
		aoKey.globalPoseEpoch = Implementation::Transformable::GlobalTransformPoseEpoch();
		aoKey.threadPoseEpoch = Implementation::Transformable::ThreadTransformPoseEpoch();
		aoKey.primId = primId;
		aoKey.numSamples = N;
		aoKey.flags = flags;
		aoKey.wx = (double)ptWorld.x; aoKey.wy = (double)ptWorld.y; aoKey.wz = (double)ptWorld.z;
		aoKey.ox = (double)ptObject.x; aoKey.oy = (double)ptObject.y; aoKey.oz = (double)ptObject.z;
		aoKey.time = (double)time;
		aoKey.dist = (double)distanceWorld;
		aoKey.hnx = (double)hemiN.x; aoKey.hny = (double)hemiN.y; aoKey.hnz = (double)hemiN.z;
		aoKey.odx = (double)offsetDir.x; aoKey.ody = (double)offsetDir.y; aoKey.odz = (double)offsetDir.z;
		Scalar cachedAO = Scalar( 0 );
		if( ExpressionMemo::AOFind( aoKey, cachedAO ) ) {
			return cachedAO;
		}

		OrthonormalBasis3D onb;
		onb.CreateFromW( hemiN );

		// Deterministic 2D Cranley-Patterson shift hashed from (ptWorld, ptObject,
		// time, flags) -- zero ASLR pointer dependence, and stays invariant across
		// ReliefModifier taps (which leave `signals` untouched).
		// Canonicalize -0.0 -> +0.0 on the raw integer bit pattern AFTER memcpy
		// so `-fno-signed-zeros` (`-ffast-math`) cannot elide the branch.
		auto canonBits = []( double x ) -> std::uint64_t {
			std::uint64_t u = 0;
			std::memcpy( &u, &x, sizeof( u ) );
			if( ( u & 0x7FFFFFFFFFFFFFFFULL ) == 0ULL ) u = 0ULL;
			return u;
		};
		auto mix64 = []( std::uint64_t z ) -> std::uint64_t {
			z = ( z ^ ( z >> 30 ) ) * 0xbf58476d1ce4e5b9ULL;
			z = ( z ^ ( z >> 27 ) ) * 0x94d049bb133111ebULL;
			return z ^ ( z >> 31 );
		};
		std::uint64_t h = 0x9e3779b97f4a7c15ULL ^ static_cast<std::uint64_t>( flags );
		h = mix64( h ^ canonBits( static_cast<double>( ptWorld.x ) ) );
		h = mix64( h ^ canonBits( static_cast<double>( ptWorld.y ) ) );
		h = mix64( h ^ canonBits( static_cast<double>( ptWorld.z ) ) );
		h = mix64( h ^ canonBits( static_cast<double>( ptObject.x ) ) );
		h = mix64( h ^ canonBits( static_cast<double>( ptObject.y ) ) );
		h = mix64( h ^ canonBits( static_cast<double>( ptObject.z ) ) );
		h = mix64( h ^ canonBits( static_cast<double>( time ) ) );
		const std::uint64_t h2 = mix64( h ^ 0x517cc1b727220a95ULL );
		const Scalar jitterX = static_cast<Scalar>( ( h >> 11 ) * ( 1.0 / 9007199254740992.0 ) );
		const Scalar jitterY = static_cast<Scalar>( ( h2 >> 11 ) * ( 1.0 / 9007199254740992.0 ) );

		// Exclude originating mesh triangle (pSelf, pProvider, primId) from
		// self-intersection so smooth-shaded coarse meshes and tilted custom
		// normals never self-shadow against their own face (Cycles ray.self.prim).
		const ScopedSelfPrimExclusion primExclusion( pSelf, pProvider, primId );

		int unoccluded = 0;
		const Scalar invN = Scalar( 1 ) / static_cast<Scalar>( N );
		for( int i = 0; i < N; ++i ) {
			Scalar u1 = ( static_cast<Scalar>( i ) + Scalar( 0.5 ) ) * invN + jitterX;
			if( u1 >= Scalar( 1 ) ) u1 -= Scalar( 1 );

			// Radical inverse base 2 (Van der Corput) for 2D Hammersley set
			std::uint32_t bits = static_cast<std::uint32_t>( i );
			bits = ( bits << 16u ) | ( bits >> 16u );
			bits = ( ( bits & 0x55555555u ) << 1u ) | ( ( bits & 0xAAAAAAAAu ) >> 1u );
			bits = ( ( bits & 0x33333333u ) << 2u ) | ( ( bits & 0xCCCCCCCCu ) >> 2u );
			bits = ( ( bits & 0x0F0F0F0Fu ) << 4u ) | ( ( bits & 0xF0F0F0F0u ) >> 4u );
			bits = ( ( bits & 0x00FF00FFu ) << 8u ) | ( ( bits & 0xFF00FF00u ) >> 8u );
			Scalar u2 = static_cast<Scalar>( bits ) * Scalar( 2.3283064365386963e-10 ) + jitterY;
			if( u2 >= Scalar( 1 ) ) u2 -= Scalar( 1 );

			const Scalar r = std::sqrt( u1 );
			const Scalar phi = TWO_PI * u2;
			const Scalar lx = r * std::cos( phi );
			const Scalar ly = r * std::sin( phi );
			const Scalar lz = std::sqrt( std::max( Scalar( 0 ), Scalar( 1 ) - u1 ) );

			const Vector3 dir = Vector3Ops::Normalize(
				onb.u() * lx + onb.v() * ly + onb.w() * lz );
			const Ray aoRay( rayOrigin, dir );
			const bool occluded = bOnlyLocal
				? pSelf->IntersectRay_IntersectionOnly( aoRay, rayMaxDist, true, true )
				: pScene->IntersectShadowRay( aoRay, rayMaxDist, true, true );
			if( !occluded ) {
				++unoccluded;
			}
		}

		const Scalar result = static_cast<Scalar>( unoccluded ) * invN;
		ExpressionMemo::AOInsert( aoKey, result );
		return result;
	}
}

#endif
