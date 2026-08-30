//////////////////////////////////////////////////////////////////////
//
//  ISurfaceSignalProvider.h - THE single dispatch channel from the
//  expression VM to a geometry's own per-hit shading signals
//  (occlusion / thickness).
//
//  Phase 2 of docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md (§6.1).
//
//  Two things live here because they are the two halves of ONE feature
//  -- "who can answer an occlusion / thickness query" and "what does a
//  particular hit hand that answerer":
//
//    RISE::ISurfaceSignalProvider   the geometry-side interface.
//    RISE::SurfaceSignalInfo        the per-hit record field, plus the
//                                   two honest-fallback wrappers every
//                                   consumer should call instead of
//                                   dereferencing the provider itself.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef I_SURFACE_SIGNAL_PROVIDER_
#define I_SURFACE_SIGNAL_PROVIDER_

#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/FiniteMath.h"

namespace RISE
{
	//! A geometry that can answer per-hit AMBIENT-OCCLUSION and THICKNESS
	//! queries about ITS OWN surface.
	//!
	//! WHY AN INTERFACE RATHER THAN AN SDF METHOD.  The two builtins
	//! `occlusion(radius)` / `thickness(radius)` are meant to mean the same
	//! thing on every geometry family.  Phase 2 answers them live, from the
	//! distance field, on the SDF family (`sdf_geometry`,
	//! `skeleton_geometry`); Phase 3 answers them for triangle meshes out of
	//! a per-vertex BAKE.  Both arrive through THIS interface, so the VM has
	//! exactly one call shape and there is no second mechanism to keep in
	//! sync (design doc §6.1, §7.1).  That is what makes "the same builtin
	//! names on meshes and SDFs" a portability claim rather than a naming
	//! coincidence.
	//!
	//! THE INTERFACE DOES NOT ASSUME A LIVE FIELD.  Every method returns a
	//! bool and may REFUSE (return false, leaving `outValue` untouched) --
	//! which is precisely what a bake-backed provider must do when the
	//! requested radius is not the baked one (§7.1's mismatch contract),
	//! and what any provider does for a nonsensical radius.  Refusal is a
	//! first-class answer, not an error: the caller substitutes the
	//! signal's documented NEUTRAL value (see SurfaceSignalInfo below).
	//!
	//! COORDINATE SPACE.  Queries are posed in the GEOMETRY'S OWN object
	//! space -- the space its `IntersectRay` works in, before
	//! `Object::IntersectRay` lifts anything to world.  Two consequences,
	//! both deliberate:
	//!   * `radiusFraction` is DIMENSIONLESS (a fraction of the geometry's
	//!     own characteristic size, see below), so no length ever has to
	//!     cross the transform boundary;
	//!   * both outputs are dimensionless [0,1], so the results are
	//!     invariant under the object's transform for free -- two
	//!     instances of one geometry at different world scales read the
	//!     SAME occlusion at corresponding points, with no `scaleHint`
	//!     fold required (contrast `curvR`, which is 1/length and needs
	//!     one).
	//!
	//! `radiusFraction` is a FRACTION OF THE GEOMETRY'S CHARACTERISTIC
	//! SIZE -- the same bounding-box-diagonal convention `scaleHint` uses
	//! (SurfaceCurvature::ScaleHintFromBoundingBox).  `occlusion(0.05)`
	//! reads as "5 % of the object", not "5 world units", which is the only
	//! scene-scale-independent thing it could mean (design doc §9).
	//!
	//! THREAD SAFETY.  Implementations MUST be pure `const` functions of
	//! the geometry's immutable state and the arguments: no locks, no
	//! caches, no rays, no scene access.  Every render thread calls these
	//! concurrently on one shared provider (docs/ARCHITECTURE.md's
	//! scene-immutability rule).
	//!
	//! LIFETIME.  Providers are NOT reference-counted through this
	//! interface: a provider IS its geometry (SDFGeometry implements it on
	//! itself), and a geometry always outlives the hit record that names
	//! it.  A geometry must therefore only ever stamp `this` -- never a
	//! pointer to anything shorter-lived -- into
	//! `RayIntersectionGeometric::signals`.
	class ISurfaceSignalProvider
	{
	public:
		virtual ~ISurfaceSignalProvider() {}

		//! AMBIENT OCCLUSION / cavity exposure at `ptObject`, in [0,1],
		//! with **1 = fully unoccluded** (design doc §9; this is the
		//! universal convention -- Blender, Substance and V-Ray all call
		//! white "unoccluded").  Self-occlusion only: the query sees the
		//! hit geometry's own surface, nothing else in the scene (§8).
		//! \return TRUE and writes outValue, or FALSE (outValue untouched)
		//!         when this provider cannot answer for this radius.
		virtual bool ComputeOcclusion(
			const Point3& ptObject,			///< [in] hit point, geometry object space
			const Vector3& nObject,			///< [in] OUTWARD unit normal there, same space
			const Scalar radiusFraction,	///< [in] query radius as a fraction of the geometry's characteristic size
			Scalar& outValue				///< [out] occlusion in [0,1], 1 = unoccluded
			) const = 0;

		//! THICKNESS of the solid behind `ptObject`, in [0,1], NORMALIZED
		//! BY THE QUERY RADIUS, with **1 = thick** (Substance's
		//! Thickness-from-Mesh convention: black = thin, white = thick).
		//! A slab of width w queried at radius R reads `min(w/R, 1)`.
		//! \return TRUE and writes outValue, or FALSE (outValue untouched)
		//!         when this provider cannot answer for this radius.
		virtual bool ComputeThickness(
			const Point3& ptObject,			///< [in] hit point, geometry object space
			const Vector3& nObject,			///< [in] OUTWARD unit normal there, same space
			const Scalar radiusFraction,	///< [in] query radius as a fraction of the geometry's characteristic size
			Scalar& outValue				///< [out] thickness in [0,1], 1 = thick
			) const = 0;
	};

	//! The per-hit half of the channel: what `RayIntersectionGeometric`
	//! carries so an expression evaluated at that hit can pose a query.
	//!
	//! Stamped by the geometry inside its OWN `IntersectRay`, in its own
	//! object space, and then deliberately left alone by every transform
	//! layer above it (`Object::IntersectRay`, `CSGObject::IntersectRay`)
	//! -- see the space discussion on ISurfaceSignalProvider.  A CSG
	//! composite therefore reports the INNERMOST geometry's own frame,
	//! which is exactly the frame that geometry's provider expects.
	//!
	//! NOT GATED BY A DEMAND COUNTER, and that is a deliberate difference
	//! from Phase 1's `curv` (SurfaceCurvatureDemand).  Curvature is a
	//! CONTEXT VARIABLE -- it must be computed before the painter runs, and
	//! on the SDF family computing it costs ~18 extra field evaluations per
	//! hit, so it needs an up-front consumption predicate.  This is a
	//! pointer plus six scalars with NO field evaluation, stamped on a path
	//! that just finished a sphere trace: the cost is below measurement
	//! noise, while a gate would add a real failure mode (a closed gate
	//! silently degrades a live `occlusion()` call to its fallback).  The
	//! EXPENSIVE half -- the estimators themselves -- is lazy by
	//! construction: it runs only when an expression actually calls the
	//! builtin (design doc §6.2), which is where the "costs nothing when
	//! unused" guarantee actually comes from.
	//!
	//! STALENESS.  This field follows exactly the protocol `derivatives`
	//! already relies on: intersection candidates are evaluated into FRESH
	//! records and copied back whole on a win (ObjectManager::
	//! RayElementIntersection, the BVH / BSP node code), and CSG's
	//! boundary-attribution helper `AdoptCsgSurfacePayload` re-adopts it
	//! alongside `derivatives` / `ptObjIntersec` when the algebra credits
	//! the reported surface to the other operand.  A geometry with no
	//! provider simply never stamps, and the null pointer reads as the
	//! honest "this surface publishes no signals".
	struct SurfaceSignalInfo
	{
		//! Non-owning, non-refcounted back-pointer to the geometry that can
		//! answer for this hit; 0 = this surface publishes no signals.
		const ISurfaceSignalProvider*	pProvider;
		//! Hit point in the PROVIDER's object space.
		Point3							ptObject;
		//! Outward unit normal at that point, same space.
		Vector3							nObject;

		SurfaceSignalInfo() :
		pProvider( 0 ), ptObject( 0, 0, 0 ), nObject( 0, 0, 0 )
		{
		}

		//! THE NEUTRAL OCCLUSION: 1 == unoccluded.  "No evidence of
		//! occlusion" and "measured to be unoccluded" agree here, which is
		//! what makes 1 the honest absence value as well as the range end.
		static Scalar NeutralOcclusion() { return Scalar( 1 ); }

		//! THE NEUTRAL THICKNESS: 1 == thick.  Argued from the use case,
		//! not from symmetry.  Thickness masks exist to KEY THIN REGIONS --
		//! `1 - thickness(r)` drives translucency, subsurface tint, wax
		//! glow, edge scatter.  An absent signal must therefore read THICK,
		//! so an unbaked mesh or an unanswerable radius lights NOTHING up;
		//! the neutral 0 ("thin") would set the whole object glowing and
		//! read as a feature rather than as an absence.  Same reasoning
		//! that puts `fw` at 0 (= "no filter", i.e. do nothing) rather than
		//! at some plausible-looking width.
		static Scalar NeutralThickness() { return Scalar( 1 ); }

		//! Is `radiusFraction` a usable query radius at all?  Must be
		//! finite and strictly positive; a zero or negative radius has no
		//! meaning for either signal (a literal one is rejected at PARSE
		//! time -- this is the runtime half, for a computed radius).
		static bool RadiusUsable( const Scalar radiusFraction )
		{
			return RISE::IsFiniteDouble( static_cast<double>( radiusFraction ) )
				&& radiusFraction > Scalar( 0 );
		}

		//! Occlusion at this hit, or NeutralOcclusion() when there is no
		//! provider, the radius is unusable, or the provider refuses.
		//! Always finite, always in [0,1].
		Scalar Occlusion( const Scalar radiusFraction ) const
		{
			Scalar v = Scalar( 0 );
			if( pProvider && RadiusUsable( radiusFraction ) &&
			    pProvider->ComputeOcclusion( ptObject, nObject, radiusFraction, v ) &&
			    RISE::IsFiniteDouble( static_cast<double>( v ) ) ) {
				return ( v < Scalar( 0 ) ) ? Scalar( 0 ) : ( ( v > Scalar( 1 ) ) ? Scalar( 1 ) : v );
			}
			return NeutralOcclusion();
		}

		//! Thickness at this hit, or NeutralThickness() when there is no
		//! provider, the radius is unusable, or the provider refuses.
		//! Always finite, always in [0,1].
		Scalar Thickness( const Scalar radiusFraction ) const
		{
			Scalar v = Scalar( 0 );
			if( pProvider && RadiusUsable( radiusFraction ) &&
			    pProvider->ComputeThickness( ptObject, nObject, radiusFraction, v ) &&
			    RISE::IsFiniteDouble( static_cast<double>( v ) ) ) {
				return ( v < Scalar( 0 ) ) ? Scalar( 0 ) : ( ( v > Scalar( 1 ) ) ? Scalar( 1 ) : v );
			}
			return NeutralThickness();
		}
	};
}

#endif
