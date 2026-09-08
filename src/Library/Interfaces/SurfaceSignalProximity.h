//////////////////////////////////////////////////////////////////////
//
//  SurfaceSignalProximity.h - The body of SurfaceSignalInfo::Proximity,
//  the CROSS-OBJECT half of the shading-signal family.
//
//  docs/CROSS_OBJECT_PROXIMITY_DESIGN.md.  `proximity(r)` answers "how
//  close is the nearest OTHER surface", which is the quantity contact
//  grime actually is -- dirt collecting where a nail rests on a plank,
//  dust where a wall meets a floor.  Its three siblings
//  (occlusion / thickness / convexity) are SELF-signals: they ask the hit
//  geometry about its own shape and never see the rest of the scene.
//
//  WHY THIS IS A FILE OF ITS OWN, and not four more lines in
//  ISurfaceSignalProvider.h next to the struct it belongs to.  The body
//  calls `IObjectManager::NearestOtherSurface`, so it needs
//  IObjectManager.h -- and IObjectManager.h reaches IObject.h, which
//  includes RayIntersection.h at its bottom, which includes
//  RayIntersectionGeometric.h, which includes ISurfaceSignalProvider.h.
//  An include there is therefore a cycle: whichever of the two a
//  translation unit names first, the other's guard is already set by the
//  time the body is compiled, and `IObjectManager` is incomplete.  This
//  header sits ABOVE both and may include them, which resolves it with
//  no forward-declaration tricks and no pointer laundering.
//
//  WHO MUST INCLUDE IT: any translation unit that CALLS Proximity.  In
//  the shipped tree that is exactly one -- the expression VM's
//  `CallFunc` in Painters/ExpressionEval.h -- plus the tests.  A unit
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

namespace RISE
{
	//! THE SECOND POLICY BODY beside `SignalQuery`, not a fourth branch of
	//! it -- see `SurfaceSignalInfo::SignalKind`'s comment for why the two
	//! cannot be merged (they have different preconditions: `SignalQuery`
	//! short-circuits on a null `pProvider`, and a `box_geometry` receiver
	//! has none, which is precisely the case this signal must answer for).
	//!
	//! They DO share the L1 memo, through `MakeL1Key` and the same
	//! find-compute-insert shape, so a new key field reaches both and
	//! neither can be served the other's entry (`fn` separates them).
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
		//            a BDPT/VCM/MLT rebuild, the GUI's painter preview, a
		//            hand-built test record.
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
				// already bounds `d` to (0, r] -- and costs nothing, but it
				// is what lets every caller treat the result as [0,1]
				// without knowing that.
				const Scalar v = Scalar( 1 ) - d / radiusWorld;
				out = ( v < Scalar( 0 ) ) ? Scalar( 0 ) : ( ( v > Scalar( 1 ) ) ? Scalar( 1 ) : v );
			}
		}

		ExpressionMemo::L1Insert( key, out );
		return out;
	}
}

#endif
