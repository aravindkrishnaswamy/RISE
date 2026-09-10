//////////////////////////////////////////////////////////////////////
//
//  ISurfaceSignalProvider.h - THE single dispatch channel from the
//  expression VM to the per-hit shading signals it calls.
//
//  Phase 2 of docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md (§6.1); `convexity`
//  and occlusion's re-basing onto the planar reference are
//  docs/OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md.
//
//  Two things live here because they are the two halves of ONE feature --
//  "who can answer a query about this hit" and "what does a particular hit
//  hand that answerer":
//
//    RISE::ISurfaceSignalProvider   the geometry-side interface.
//    RISE::SurfaceSignalInfo        the per-hit record field, plus the
//                                   five honest-fallback wrappers every
//                                   consumer should call instead of
//                                   dereferencing the provider itself.
//
//  THREE OF THE FIVE ARE SELF-SIGNALS -- occlusion, thickness and
//  convexity ask the hit geometry about ITS OWN surface, through
//  ISurfaceSignalProvider, and never see the rest of the scene.  The
//  other two, `proximity` and `interior`, are CROSS-OBJECT: they ask the
//  object manager how close the nearest OTHER surface is, and how deep
//  inside another object this point lies
//  (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md).  They ride the same record
//  field and the same L1 memo, but they have no provider and they are
//  DECLARED here and DEFINED in SurfaceSignalProximity.h -- see the
//  forward declarations below for the include cycle that forces the
//  split.
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
#include "../Utilities/SurfaceCurvature.h"
#include "../Utilities/ExpressionMemo.h"
#include "ILog.h"
#include <atomic>

namespace RISE
{
	struct SurfaceSignalInfo;
	//! The CROSS-OBJECT half of the channel (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md
	//! §5.1).  FORWARD-DECLARED, never included: IObjectManager.h reaches
	//! IObject.h, which includes RayIntersection.h at its bottom, which
	//! includes RayIntersectionGeometric.h, which includes THIS file -- so a
	//! real include here is a cycle that leaves IObjectManager incomplete for
	//! whichever of the two a translation unit names first.  That is why
	//! `SurfaceSignalInfo::Proximity` is DECLARED here and DEFINED in
	//! SurfaceSignalProximity.h, which sits above both and may include them.
	class IObjectManager;
	class IObject;

	//! A geometry that can answer per-hit AMBIENT-OCCLUSION, THICKNESS and
	//! CONVEXITY queries about ITS OWN surface.
	//!
	//! WHY AN INTERFACE RATHER THAN AN SDF METHOD.  The builtins
	//! `occlusion(radius)` / `thickness(radius)` / `convexity(radius)` are
	//! meant to mean the same thing on every geometry family.  Phase 2
	//! answers them live, from the distance field, on the SDF family
	//! (`sdf_geometry`, `skeleton_geometry`); Phase 3 answers them for
	//! triangle meshes out of a per-vertex BAKE.  Both arrive through THIS interface, so the VM has
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
	//!   * every output is dimensionless [0,1], so the results are
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
	//! `bRadiusIsConstant` IS THE BAKED FAMILY'S PRECONDITION, and it is
	//! carried on the call rather than inferred, because it CANNOT be
	//! inferred: by the time a `Scalar` radius reaches a provider, "0.05
	//! written in the scene text" and "the current value of `fbm(P)*0.1`"
	//! are the same bit pattern.  A live estimator does not care -- the SDF
	//! family answers any radius per hit and ignores this flag.  A BAKE
	//! does: it commits to one radius, so a computed radius would either
	//! be silently answered at the wrong scale (design doc §7.1 forbids
	//! exactly that) or bake a fresh table per distinct value, which is
	//! unbounded work AND makes the render depend on which radius happened
	//! to arrive first -- a reproducibility break, not just a slowdown.
	//! The expression compiler proves literalness per CALL SITE and emits
	//! that proof into the instruction (ExpressionEval.h's
	//! kFnOcclusion / kFnOcclusionDynR pair), so the answer is the same on
	//! every thread and every run.  FALSE never means "provably not
	//! constant"; it means "not proven constant here", which the baked path
	//! must treat as dynamic and refuse.
	//!
	//! THREAD SAFETY.  Every render thread calls these concurrently on one
	//! shared provider, so an implementation must be safe under that and
	//! must never mutate anything another thread can observe as scene state
	//! (docs/ARCHITECTURE.md's scene-immutability rule).  Two shapes
	//! satisfy that, and RISE ships both:
	//!   * PURE (the SDF family) -- a `const` function of the geometry's
	//!     immutable state and the arguments; no locks, no caches, no rays.
	//!   * LAZILY BAKED (the indexed-mesh family) -- a `mutable` cache
	//!     behind an `RMutex`, find-or-build serialized, the built table
	//!     immutable from then on.  This is the sanctioned
	//!     ARCHITECTURE.md §Known Exceptions pattern (the SSS point-set
	//!     precedent), adopted here for the same structural reason: a mesh
	//!     bake is far too expensive to run for a preview that will never
	//!     shade a material, so it must not exist until something reads it
	//!     (design doc §7.3).  An implementation that takes this route owes
	//!     the full discipline: lock across the whole find-or-build, a
	//!     null-sentinel cached on failure so a failed build is not retried
	//!     per sample, and a DETERMINISTIC build (no wall-clock seeding, no
	//!     dependence on which thread won the race).
	//!
	//! SCENE ACCESS remains forbidden in both shapes: a provider may trace
	//! against its OWN primitives (that is what a self-occlusion bake is)
	//! and must never reach the scene, which is what keeps these signals
	//! object-local and their invalidation free (§8, §7.2).
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
		//!
		//! THE CONTRACT, restated 2026-09-06
		//! (docs/OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md): this is the
		//! **fraction of the outward hemisphere from which a ray escapes a
		//! distance `radiusFraction` without re-entering the solid**,
		//! cosine-weighted -- classic ambient occlusion.  Two consequences
		//! an implementation MUST honour, because scenes are written
		//! against them:
		//!   * a locally PLANAR surface reads EXACTLY 1, and so does every
		//!     CONVEX feature (every outward ray escapes).  Occlusion must
		//!     never report a residual on a convex edge; that side of the
		//!     range belongs to ComputeConvexity.  Note the reference costs
		//!     nothing to hit: it is not a normalisation, it is what "every
		//!     ray escapes" comes out at.
		//!   * on a wedge of empty opening `alpha`, a point on one of its
		//!     faces reads `sin^2(alpha/2)`; in a spherical pocket of
		//!     radius rho, `1 - (R/2rho)^2`.
		//!
		//! It is a VISIBILITY measure, not a volume one, and that
		//! distinction is load-bearing rather than pedantic: the fraction
		//! of the query BALL lying outside the solid satisfies both closed
		//! forms above and still reads a narrow slot as OPEN, because the
		//! ball reaches up out of the slot into air that no ray can
		//! actually reach.  See the referenced document for the render that
		//! settled it.
		//! \return TRUE and writes outValue, or FALSE (outValue untouched)
		//!         when this provider cannot answer for this radius.
		virtual bool ComputeOcclusion(
			const SurfaceSignalInfo& hit,	///< [in] the hit, in THIS provider's own object space (see below)
			const Scalar radiusFraction,	///< [in] query radius as a fraction of the geometry's characteristic size
			const bool bRadiusIsConstant,	///< [in] did the compiler PROVE this radius a compile-time constant? (see below)
			Scalar& outValue				///< [out] occlusion in [0,1], 1 = unoccluded
			) const = 0;

		//! THICKNESS of the solid behind `ptObject`, in [0,1], NORMALIZED
		//! BY THE QUERY RADIUS, with **1 = thick** (Substance's
		//! Thickness-from-Mesh convention: black = thin, white = thick).
		//! A slab of width w queried at radius R reads `min(w/R, 1)`.
		//! \return TRUE and writes outValue, or FALSE (outValue untouched)
		//!         when this provider cannot answer for this radius.
		virtual bool ComputeThickness(
			const SurfaceSignalInfo& hit,	///< [in] the hit, in THIS provider's own object space
			const Scalar radiusFraction,	///< [in] query radius as a fraction of the geometry's characteristic size
			const bool bRadiusIsConstant,	///< [in] did the compiler PROVE this radius a compile-time constant?
			Scalar& outValue				///< [out] thickness in [0,1], 1 = thick
			) const = 0;

		//! RADIUS-SAMPLED CONVEXITY at `ptObject`, in [0,1], with **0 = flat
		//! OR concave** and **1 = a knife edge at that radius**
		//! (docs/OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md).
		//!
		//! THE OTHER SIDE OF `ComputeOcclusion`'s RANGE, and that is why it
		//! lives on this interface rather than beside `curv`: occlusion
		//! reports how much LESS open than a flat surface this one is, and
		//! this reports how much MORE.  Neither is ever a residual of the
		//! other -- a flat face and every convex edge read occlusion
		//! exactly 1, a flat face and every cavity read convexity exactly
		//! 0 -- so an author composes them without a threshold between.
		//!
		//! The contract is `clamp(2A - 1, 0, 1)` for `A` the fraction of
		//! the query BALL lying outside the solid (exactly 1/2 on a plane).
		//! Volume, not solid angle, and deliberately unlike occlusion's:
		//! from a point on a sphere of ANY radius the solid subtends
		//! exactly a hemisphere, so a directional measure reads 0 on every
		//! smooth body, where the ball reads `3R/(8*rho)` -- "this bead is
		//! proud of its surroundings at scale R", which is what an
		//! edge-wear mask wants.  On WEDGES -- edges, creases, corners --
		//! the two measures agree exactly, which is where both are mostly
		//! used.
		//!
		//! Every value has a fixed geometric meaning on every object at
		//! every scene scale, which is exactly what `curv` (an unbounded
		//! differential quantity whose useful thresholds are per-object)
		//! cannot offer: **0.5 IS a 90-degree arris, 0.75 IS a three-face
		//! corner**, on a 3 mm nail and on a 30 m wall alike.
		//!
		//! Self-signal only, same as the other two: the query sees this
		//! geometry's own surface and nothing else in the scene.
		//! \return TRUE and writes outValue, or FALSE (outValue untouched)
		//!         when this provider cannot answer for this radius.
		virtual bool ComputeConvexity(
			const SurfaceSignalInfo& hit,	///< [in] the hit, in THIS provider's own object space
			const Scalar radiusFraction,	///< [in] query radius as a fraction of the geometry's characteristic size
			const bool bRadiusIsConstant,	///< [in] did the compiler PROVE this radius a compile-time constant?
			Scalar& outValue				///< [out] convexity in [0,1], 0 = flat/concave
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

		//! WHICH PRIMITIVE was hit, as an index into the provider's own
		//! primitive array -- a triangle index for the indexed-mesh family;
		//! -1 (the default) for a provider that answers POSITIONALLY and has
		//! no primitives to index, which is the SDF family.
		//!
		//! WHY THE RECORD CARRIES THIS AT ALL, when design doc §7.1's
		//! original sketch said "interpolate at intersection time": that
		//! sketch predates §7.3's decision to build mesh bakes LAZILY, and
		//! the two cannot both hold.  Intersection happens BEFORE shading,
		//! and the lazy bake does not exist until shading first asks for it
		//! -- so at intersection time there is, by construction, nothing to
		//! interpolate on the first query.  What survives from §7.1 is the
		//! part that was actually load-bearing: the barycentric arithmetic
		//! belongs to the intersector's frame, not the provider's.  So the
		//! intersector stamps WHERE the hit is on the mesh (this int and the
		//! two weights below -- cheaper than the interpolation it replaces)
		//! and the provider interpolates ON DEMAND, after its find-or-build.
		//! Same data, one indirection later, and the draft-mode guarantee
		//! survives.
		int								primId;
		//! Barycentric weights of the hit inside `primId`, in the convention
		//! the mesh intersector already uses for normals / UVs / vertex
		//! colours: value = v0 + (v1-v0)*baryA + (v2-v0)*baryB.  Meaningless
		//! (and untouched) when primId < 0.
		Scalar							baryA;
		Scalar							baryB;

		//! IS THE COMPOSITE'S SOLID THE COMPLEMENT OF THE PROVIDER'S OWN?
		//! FALSE (the default, and the case for every non-CSG hit) means
		//! "solid" means the same thing to the composite and to the
		//! provider.  TRUE means a CSG_SUBTRACTION has credited this
		//! surface to the SUBTRAHEND: the wall the viewer is standing in
		//! front of belongs to operand B, but the empty region is B's
		//! INTERIOR and the solid is B's exterior -- exactly inverted.
		//!
		//! WHY THIS EXISTS SEPARATELY FROM THE `nObject` FLIP beside it.
		//! Those four CSG sites already negate `nObject`, and that flip is
		//! still load-bearing -- for `thickness`, which MARCHES along -n
		//! and would otherwise march into the wrong side.  But occlusion
		//! and convexity are no longer directional: since 2026-09-06 they
		//! are two clamps of a ball ACCESSIBILITY that reads only the SIGN
		//! of the field around the hit, and a sign is not something a
		//! normal flip can invert.  Under the retired normal-line
		//! estimator the flip did double duty (it was sign-blind, so
		//! reversing the tap direction was the same as reversing the
		//! sense); it cannot any more, so the sense travels explicitly.
		//!
		//! It TOGGLES rather than sets, so nested subtractions compose.
		//!
		//! HONOURED BY THE SDF FAMILY ONLY.  A baked mesh table is
		//! computed over the mesh's own solid and cannot be complemented
		//! after the fact (its occlusion bake is a cosine-hemisphere
		//! integral, already clamped), so the mesh provider ignores this
		//! flag -- which is the same behaviour a subtracted mesh operand
		//! had before the flag existed, not a new gap.
		bool							bComplementedField;

		//! THE CROSS-OBJECT HALF OF THE CHANNEL
		//! (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.1 and §5.6).  Everything above
		//! this line describes the hit's OWN surface and is stamped by the
		//! geometry that owns it, in that geometry's object space.  These
		//! four describe WHERE IN THE SCENE that hit is, which is what a
		//! cross-object query needs and what no object-space field can say.
		//!
		//! Non-owning, non-refcounted back-pointers, with the same lifetime
		//! argument `pProvider` already makes: a manager and an object
		//! outlive every record they stamp (the scene is immutable for the
		//! duration of a pass, and a record never outlives the pass).
		//!
		//! STAMPED IN EXACTLY ONE PLACE for the first three --
		//! `ObjectManager::IntersectRay( RayIntersection&, ... )`, on the
		//! WINNING record after traversal, with `pSelf` copied from
		//! `ri.pObject` so the two identities cannot disagree.  The
		//! invariant that makes the stamp survive to the painter is that
		//! nothing assigns `signals` after that function returns;
		//! SourceHygieneTest pins the write-site set at file granularity.
		//!
		//! ZERO IS THE HONEST ABSENCE for all five: a record rebuilt by
		//! `PathVertexEval::PopulateRIGFromVertex` (BDPT / VCM / MLT), a
		//! hit found by something other than the object manager, or a
		//! hand-built test record carries `pScene == 0` and reads the
		//! neutral 0 from `Proximity` and from `Interior` -- the same
		//! disclosed gap the other three signals already have on those
		//! integrator families.
		const IObjectManager*			pScene;		//!< the manager that found this hit; 0 = none
		const IObject*					pSelf;		//!< == ri.pObject: the object the hit belongs to
		Point3							ptWorld;	//!< the hit in WORLD space (== ri.ptIntersection)
		//! The evaluating painter's own `m_time`, stamped by
		//! `ExpressionPainter::BuildContext` on its COPY of this struct (the
		//! object manager does not know a painter's time, and the VM's
		//! `CallFunc` has no time parameter).  It is here rather than only in
		//! `ExprEvalContext::time` because the cross-object answer can move
		//! when a NEIGHBOUR moves while the receiver's own hit is unchanged
		//! -- the one way this signal's memo-staleness differs from the other
		//! three (§5.4).  `ExpressionScalarPainter::BuildContext` stamps 0:
		//! that pipe deliberately does not expose `time` at all.
		Scalar							time;

		SurfaceSignalInfo() :
		pProvider( 0 ), ptObject( 0, 0, 0 ), nObject( 0, 0, 0 ),
		primId( -1 ), baryA( 0 ), baryB( 0 ), bComplementedField( false ),
		pScene( 0 ), pSelf( 0 ), ptWorld( 0, 0, 0 ), time( 0 )
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

		//! THE NEUTRAL CONVEXITY: 0 == flat.  Argued from the use case, and
		//! it lands on the OPPOSITE end of the range from the other two --
		//! which is the point, not an inconsistency.  All three neutrals
		//! are "the do-nothing end": a convexity mask exists to WEAR THE
		//! EDGES (`convexity(r)` drives paint chipping, polish, rust-free
		//! ridges), so an absent signal must read FLAT, leaving an
		//! unsupported geometry untouched.  The neutral 1 ("knife edge
		//! everywhere") would wear every surface in the frame and read as a
		//! feature rather than as an absence.
		static Scalar NeutralConvexity() { return Scalar( 0 ); }

		//! THE NEUTRAL PROXIMITY: 0 == nothing within the radius.  The
		//! do-nothing end again, and argued the same way: `proximity(r)`
		//! exists to COLLECT CONTACT GRIME -- dirt where a nail meets a
		//! plank, dust where a wall meets a floor -- so an absent signal
		//! must read "no neighbour here" and paint nothing.  The neutral 1
		//! ("touching everywhere") would grime the whole frame and read as
		//! a feature rather than as an absence.  Reached whenever the
		//! channel carries no scene or no self object (a hit rebuilt by
		//! BDPT/VCM/MLT, a hand-built record, a preview), the radius or the
		//! point is unusable, or every candidate refused.
		static Scalar NeutralProximity() { return Scalar( 0 ); }

		//! THE NEUTRAL INTERIOR: 0 == inside no neighbour.  The same
		//! argument as `proximity`'s, one step further in: `interior(r)`
		//! exists to paint what BURIAL does -- the sunk part of a nail, the
		//! embedded flank of a stone in mortar -- so an absent signal must
		//! read "buried in nothing" and paint nothing.  Reached whenever
		//! the channel carries no scene or no self, the radius or the point
		//! is unusable, or no candidate contained the point (which every
		//! SHEET family answers by refusing, at every point, by design).
		static Scalar NeutralInterior() { return Scalar( 0 ); }

		//! Is `radiusFraction` a usable query radius at all?  Must be
		//! finite and strictly positive; a zero or negative radius has no
		//! meaning for either signal (a literal one is rejected at PARSE
		//! time -- this is the runtime half, for a computed radius).
		static bool RadiusUsable( const Scalar radiusFraction )
		{
			return RISE::IsFiniteDouble( static_cast<double>( radiusFraction ) )
				&& radiusFraction > Scalar( 0 );
		}

		//! WHICH signal is being asked for -- the discriminator the three
		//! self-signal wrappers below hand to their shared implementation,
		//! and part of the L1 memo key (they have DIFFERENT neutrals, so
		//! sharing an entry between them would be a wrong answer, not a
		//! stale one).
		//! `eProximity` and `eInterior` are NOT further branches of
		//! `SignalQuery` -- they are the discriminators the TWO CROSS-OBJECT
		//! policy bodies, `Proximity` and `Interior`, stamp into the SAME L1
		//! table so none of them can collide there.  They have to be
		//! separate bodies rather than further cases because they have
		//! DIFFERENT PRECONDITIONS: `SignalQuery` is gated on `pProvider`,
		//! and a receiver like a `box_geometry` has none at all (it would
		//! short-circuit to the neutral before the scene was ever asked),
		//! while the cross-object pair is gated on `pScene && pSelf` and
		//! does not care whether the hit surface publishes signals of its
		//! own.
		enum SignalKind { eOcclusion = 0, eThickness = 1, eConvexity = 2, eProximity = 3, eInterior = 4 };

		//! THE L1 MEMO KEY for this hit -- every field of this struct, so a
		//! new field added above and NOT added here is a silent wrong
		//! render.  Kept next to the fields it copies so the omission is
		//! at least visible; the part that is CHECKED rather than merely
		//! visible is ExpressionMemoTest's "(i) real-mesh key fields",
		//! which drives a live TriangleMeshGeometryIndexed bake -- the
		//! provider that reads ONLY (primId, baryA, baryB) -- and fails if
		//! any of the three leaves this function.  Add a field, add a
		//! separation check there too.
		ExpressionMemo::SignalHitKey MemoHitKey() const
		{
			ExpressionMemo::SignalHitKey k;
			k.pProvider = (const void*)pProvider;
			k.ptx = (double)ptObject.x; k.pty = (double)ptObject.y; k.ptz = (double)ptObject.z;
			k.nx  = (double)nObject.x;  k.ny  = (double)nObject.y;  k.nz  = (double)nObject.z;
			k.baryA = (double)baryA;    k.baryB = (double)baryB;
			k.primId = primId;
			k.bComplementedField = bComplementedField;
			// The cross-object half (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md
			// §5.1).  `pScene`/`pSelf` go in as opaque addresses -- the memo
			// never dereferences them; what it needs is only "is this the
			// same scene, asked about the same object".  A manager or object
			// freed and reallocated at the same address between passes is
			// covered by the generation counter, exactly as `pProvider` is.
			k.pScene = (const void*)pScene;
			k.pSelf  = (const void*)pSelf;
			k.wx = (double)ptWorld.x; k.wy = (double)ptWorld.y; k.wz = (double)ptWorld.z;
			k.time = (double)time;
			return k;
		}

		//! THE ONE PLACE the three wrappers' shared policy lives -- the
		//! null-provider / unusable-radius / refusal fallbacks, the
		//! finiteness test and the [0,1] clamp -- and therefore the one
		//! place the L1 memo can sit without splitting that policy in
		//! three.  What is memoised is the FINAL, already-clamped,
		//! already-fallen-back answer, so a memo hit and a memo miss are
		//! indistinguishable by construction.
		//!
		//! SOUNDNESS.  The providers are pure const functions of their
		//! geometry's immutable state and these arguments -- the SDF
		//! family analytically (its per-hit sample-set rotation is a hash
		//! of `ptObject`, not an RNG draw), the mesh family through a
		//! build-once table.  So the answer is a function of (provider,
		//! every field of this record, fn, radius, constant-radius proof),
		//! which is exactly the key -- plus the provider's own state,
		//! which the generation counter covers (ExpressionMemo.h).
		//! THE ONE L1 KEY CONSTRUCTOR, shared by `SignalQuery` and by
		//! `Proximity` / `Interior` (SurfaceSignalProximity.h) -- and the
		//! thing that keeps three policy bodies from drifting on memo
		//! POLICY while they legitimately differ on preconditions.
		//!
		//! WHAT SHARING THIS BUYS, exactly, since it is a small function
		//! and the claim should be no bigger than it is:
		//!   * A field added to `SurfaceSignalInfo` and to `MemoHitKey`
		//!     reaches BOTH bodies' keys automatically.  That is the drift
		//!     that would actually be a wrong render, and it is now
		//!     impossible to introduce in one body and forget in the other.
		//!   * The three bodies stamp the SAME `fn` field from the SAME
		//!     enum, so an entry filled by one can never be served to
		//!     another -- `eProximity` is 3, `eInterior` is 4, and nothing
		//!     else is either.
		//! What it does NOT buy, and does not try to: the find/insert
		//! CALLS still appear once per body.  They must, because what is
		//! memoised is each body's FINAL, already-clamped, already-fallen-
		//! back answer -- which is what makes a hit and a miss
		//! indistinguishable -- and that value only exists inside the body
		//! that computed it.
		ExpressionMemo::SignalKey MakeL1Key( const SignalKind fn, const Scalar radius, const bool bRadiusIsConstant ) const
		{
			ExpressionMemo::SignalKey key;
			key.hit = MemoHitKey();
			key.radius = (double)radius;
			key.fn = (int)fn;
			key.bRadiusIsConstant = bRadiusIsConstant;
			return key;
		}

		//! CROSS-OBJECT PROXIMITY at this hit -- `clamp(1 - d/r, 0, 1)` for
		//! `d` the shortest distance to any OTHER world-visible,
		//! non-emissive object's surface, and `r` a WORLD LENGTH.
		//! 1 = touching, 0 = nothing within `r`.  Always finite, always in
		//! [0,1].  docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §2.
		//!
		//! Takes no `bRadiusIsConstant`: there is no bake behind this
		//! signal, so a computed radius costs exactly what a literal one
		//! does and no proof has to travel.
		//!
		//! DECLARED HERE, DEFINED IN SurfaceSignalProximity.h, because the
		//! body has to call through `IObjectManager` and including that
		//! header from this one is a cycle (see the forward declarations at
		//! the top of this file).  A translation unit that CALLS this must
		//! include SurfaceSignalProximity.h; one that only names the struct
		//! need not.
		Scalar Proximity( const Scalar radiusWorld ) const;

		//! CROSS-OBJECT INTERIOR at this hit -- `clamp(depth / r, 0, 1)`
		//! for `depth` the largest inside-depth LOWER bound over every
		//! OTHER world-visible, non-emissive object that CONTAINS this
		//! point, and `r` a WORLD LENGTH.  0 = inside no neighbour,
		//! 1 = at least `r` deep.  Always finite, always in [0,1].
		//! docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.6.
		//!
		//! THE SIGNED SIBLING of `Proximity`, as a second builtin rather
		//! than a sign on the first: together the two cover the signed
		//! distance without a sign convention an author has to remember.
		//!
		//! Takes no `bRadiusIsConstant`, for the same reason `Proximity`
		//! does not: there is no bake behind either.
		//!
		//! DECLARED HERE, DEFINED IN SurfaceSignalProximity.h, for the same
		//! include cycle.
		Scalar Interior( const Scalar radiusWorld ) const;

		Scalar SignalQuery( const SignalKind fn, const Scalar radiusFraction, const bool bRadiusIsConstant ) const
		{
			const ExpressionMemo::SignalKey key = MakeL1Key( fn, radiusFraction, bRadiusIsConstant );

			Scalar memo = Scalar( 0 );
			if( ExpressionMemo::L1Find( key, memo ) ) return memo;

			const Scalar neutral = ( fn == eOcclusion ) ? NeutralOcclusion()
			                     : ( fn == eThickness ) ? NeutralThickness()
			                                            : NeutralConvexity();
			Scalar out = neutral;
			Scalar v = Scalar( 0 );
			if( pProvider && RadiusUsable( radiusFraction ) ) {
				const bool answered = ( fn == eOcclusion ) ? pProvider->ComputeOcclusion( *this, radiusFraction, bRadiusIsConstant, v )
				                    : ( fn == eThickness ) ? pProvider->ComputeThickness( *this, radiusFraction, bRadiusIsConstant, v )
				                                           : pProvider->ComputeConvexity( *this, radiusFraction, bRadiusIsConstant, v );
				if( answered && RISE::IsFiniteDouble( static_cast<double>( v ) ) ) {
					out = ( v < Scalar( 0 ) ) ? Scalar( 0 ) : ( ( v > Scalar( 1 ) ) ? Scalar( 1 ) : v );
				}
			}
			ExpressionMemo::L1Insert( key, out );
			return out;
		}

		//! Occlusion at this hit, or NeutralOcclusion() when there is no
		//! provider, the radius is unusable, or the provider refuses.
		//! Always finite, always in [0,1].
		//!
		//! `bRadiusIsConstant` is the caller's PROOF that the radius is a
		//! compile-time constant -- see ISurfaceSignalProvider's own doc for
		//! why a bake cannot work without it.  It has no default: a caller
		//! that cannot prove it must say so and take the neutral fallback on
		//! the baked families, rather than inherit a silently optimistic one.
		Scalar Occlusion( const Scalar radiusFraction, const bool bRadiusIsConstant ) const
		{
			return SignalQuery( eOcclusion, radiusFraction, bRadiusIsConstant );
		}

		//! Thickness at this hit, or NeutralThickness() when there is no
		//! provider, the radius is unusable, or the provider refuses.
		//! Always finite, always in [0,1].
		Scalar Thickness( const Scalar radiusFraction, const bool bRadiusIsConstant ) const
		{
			return SignalQuery( eThickness, radiusFraction, bRadiusIsConstant );
		}

		//! Convexity at this hit, or NeutralConvexity() when there is no
		//! provider, the radius is unusable, or the provider refuses.
		//! Always finite, always in [0,1].
		Scalar Convexity( const Scalar radiusFraction, const bool bRadiusIsConstant ) const
		{
			return SignalQuery( eConvexity, radiusFraction, bRadiusIsConstant );
		}
	};

	//! DIAGNOSTIC-ONLY consumption gate for `occlusion()` / `thickness()`,
	//! mirroring SurfaceCurvatureDemand's mechanism (SurfaceCurvature.h)
	//! exactly, for exactly the reason Phase 1's counter was thread-safe:
	//! an atomic mutated only at painter construction/destruction (scene
	//! build/teardown), loaded relaxed from render threads.
	//!
	//! UNLIKE SurfaceCurvatureDemand, this gate does NOT control the
	//! per-hit provider install -- SurfaceSignalInfo's own doc (above) and
	//! design doc §13 item 2 already settled that the stamp is
	//! unconditional (a pointer plus six scalars, cheaper to always write
	//! than to gate).  Its ONLY consumer is the Phase-2 fix-round
	//! containment diagnostic (design doc §14 item 11): when a BDPT/VCM/
	//! MLT-family render begins, `Any()` answers "does any live compiled
	//! expression call occlusion()/thickness() anywhere in the process",
	//! so the rasterizer can emit one GlobalLog warning naming the
	//! disclosed neutral-signal gap on those integrator families, without
	//! doing a real scene-wide painter walk that does not exist (same
	//! argument as SurfaceCurvatureDemand's own doc comment).  A false
	//! positive here (a scene from a DIFFERENT job in the same process
	//! still holding a signal-reading painter alive) means one spurious
	//! warning line, never a wrong render -- the conservative direction
	//! for a diagnostic.
	namespace SurfaceSignalDemand
	{
		//! The single counter.  A function-local static inside an inline
		//! function has exactly one instance across all translation units.
		inline std::atomic<int>& Counter()
		{
			static std::atomic<int> counter( 0 );
			return counter;
		}

		//! Is any live consumer asking for occlusion()/thickness() anywhere
		//! in its compiled program?  One relaxed atomic load.
		inline bool Any()
		{
			return Counter().load( std::memory_order_relaxed ) > 0;
		}

		//! RAII demand reference -- same shape as
		//! SurfaceCurvatureDemand::Registration.  `active=false` constructs
		//! an inert one at zero cost.
		class Registration
		{
		public:
			explicit Registration( bool active = false ) : m_active( active )
			{
				if( m_active ) Counter().fetch_add( 1, std::memory_order_relaxed );
			}
			Registration( const Registration& other ) : m_active( other.m_active )
			{
				if( m_active ) Counter().fetch_add( 1, std::memory_order_relaxed );
			}
			Registration& operator=( const Registration& other )
			{
				if( this != &other ) {
					if( other.m_active ) Counter().fetch_add( 1, std::memory_order_relaxed );
					if( m_active ) Counter().fetch_sub( 1, std::memory_order_relaxed );
					m_active = other.m_active;
				}
				return *this;
			}
			~Registration()
			{
				if( m_active ) Counter().fetch_sub( 1, std::memory_order_relaxed );
			}
			bool IsActive() const { return m_active; }

		private:
			bool m_active;
		};
	}

	//! CONSUMPTION GATE for the CROSS-OBJECT PAIR -- registered when the
	//! compiled program calls `proximity()` OR `interior()`
	//! (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.6's `UsesCrossObject()`),
	//! so an `interior`-only scene still gets the eager snapshot.  THE NAME
	//! IS KEPT because it is a public-ish symbol with call sites in two
	//! painters and a test; what it gates is stated here rather than
	//! inferred from the name.  Unlike SurfaceSignalDemand above, this one
	//! is NOT diagnostic-only: it gates real per-pass and per-ray work.
	//!
	//! WHY THE CROSS-OBJECT PAIR NEEDS ITS OWN COUNTER when the other
	//! signals share one.
	//! The three self-signals cost nothing until called: their provider is
	//! a pointer already on the record and their estimators are lazy.
	//! `proximity` is the first signal with a SCENE-LEVEL prerequisite --
	//! `ObjectManager`'s world-AABB snapshot, which is an allocation plus a
	//! `getBoundingBox()` per object (eight corners through a matrix, each)
	//! and which `ObjectManager::IntersectRay` was checking on EVERY ray to
	//! keep alive.  That is a load, a compare and a heap indirection on the
	//! hottest path in the renderer, paid by every scene in the tree
	//! including the overwhelming majority that never mention the builtin.
	//!
	//! So the eager build in `PrepareForRendering` and the per-ray
	//! `EnsureBoxSnapshot()` are both gated on `Any()`, which is one
	//! relaxed atomic load. `NearestOtherSurface` still builds LAZILY under
	//! the tree mutex when asked with no snapshot, so correctness never
	//! depends on the counter -- only cost does.
	//!
	//! THE TRADE, stated: a consumer that reaches `NearestOtherSurface`
	//! WITHOUT registering -- a hypothetical direct caller, or a test --
	//! pays the whole snapshot build under the lock on its first call
	//! rather than having it ready. That is a one-time cost on a path that
	//! by construction has no registered painter driving it, and it is the
	//! right direction: the alternative is charging every scene in the tree
	//! for a query nobody made.
	//!
	//! Same mechanism and same thread-safety argument as SurfaceCurvature's
	//! and SurfaceSignalDemand's: an atomic mutated only at painter
	//! construction/destruction (scene build/teardown), loaded relaxed from
	//! render threads. THE COUNTER IS PROCESS-WIDE, though, not per-scene --
	//! same conservatism as SurfaceCurvature/SurfaceSignalDemand, but here it
	//! is not merely diagnostic.  A false positive (a painter from another
	//! job in the same process still alive) does not cost "one snapshot
	//! build" and stop there: it costs that build PLUS a live, unconditional
	//! `EnsureBoxSnapshot()` count-check on every ray for every
	//! `ObjectManager` in the process for the rest of the render -- the very
	//! per-ray cost this whole mechanism exists to avoid, paid by a scene
	//! that never mentions `proximity()` because some unrelated job's
	//! painter happens to still be alive. Never a wrong render, only a
	//! process-wide, render-duration cost regression.
	namespace ProximityDemand
	{
		//! The single counter.  A function-local static inside an inline
		//! function has exactly one instance across all translation units.
		inline std::atomic<int>& Counter()
		{
			static std::atomic<int> counter( 0 );
			return counter;
		}

		//! Does any live compiled program call EITHER cross-object signal --
		//! `proximity()` or `interior()`?  One relaxed
		//! atomic load.
		inline bool Any()
		{
			return Counter().load( std::memory_order_relaxed ) > 0;
		}

		//! RAII demand reference -- same shape as
		//! SurfaceSignalDemand::Registration.
		class Registration
		{
		public:
			explicit Registration( bool active = false ) : m_active( active )
			{
				if( m_active ) Counter().fetch_add( 1, std::memory_order_relaxed );
			}
			Registration( const Registration& other ) : m_active( other.m_active )
			{
				if( m_active ) Counter().fetch_add( 1, std::memory_order_relaxed );
			}
			Registration& operator=( const Registration& other )
			{
				if( this != &other ) {
					if( other.m_active ) Counter().fetch_add( 1, std::memory_order_relaxed );
					if( m_active ) Counter().fetch_sub( 1, std::memory_order_relaxed );
					m_active = other.m_active;
				}
				return *this;
			}
			~Registration()
			{
				if( m_active ) Counter().fetch_sub( 1, std::memory_order_relaxed );
			}
			bool IsActive() const { return m_active; }

		private:
			bool m_active;
		};
	}

	//! Containment diagnostic for the geometry-derived shading signals'
	//! disclosed BDPT/VCM/MLT gap (design doc §13 Phase-2 "Known residual",
	//! §14 item 11): `PathVertexEval.h`'s `PopulateRIGFromVertex` -- and
	//! every evaluation downstream of it (forward-walk throughput
	//! re-pricing, NEE/connections, MIS reverse-pdf, OpenPGL guiding RIS,
	//! HWSS companion evals, VCM merges, and MLT which drives BDPT's own
	//! machinery) -- carries neither `derivatives` nor `signals`, so
	//! `curv`, `occlusion` and `thickness` read their neutral fallback at
	//! those sites.  PT is unaffected and evaluates all three fully.
	//!
	//! Call once from each BDPT/VCM/MLT-family rasterizer's own
	//! pre-render hook (never per pixel or per sample) -- see
	//! BDPTPelRasterizer::PreRenderSetup, BDPTSpectralRasterizer::
	//! PreRenderSetup, VCMRasterizerBase::PreRenderSetup, and
	//! MLTRasterizer::RenderFrameOfMLT for the four call sites.  Cheap:
	//! two relaxed atomic loads.  `SurfaceCurvatureDemand` covers `curv`
	//! alone; `SurfaceSignalDemand` is built from a compiled program's
	//! `UsesSurfaceSignals()` (`!m_signalCalls.empty()`,
	//! ExpressionPainter.h), which fires for ALL FIVE of the other
	//! signals -- `occlusion`, `thickness`, `convexity`, and the two
	//! cross-object ones, `proximity` and `interior` -- so the two
	//! counters between them cover all SIX, not three: an expression
	//! calling only `proximity()`, say, still trips `SurfaceSignalDemand`
	//! and is caught by the warning below exactly as an `occlusion()`
	//! caller is.  Gated so a scene that never mentions any of the six
	//! costs nothing beyond the two loads.
	//!
	//! `pLog` may be null (defensive; every call site has a live log in
	//! practice) -- a null log means "cannot report," not "nothing to
	//! report," so the check is skipped silently rather than crashing.
	inline void WarnIfNonPTRenderHasLiveSignalConsumer( ILog* pLog, const char* familyName )
	{
		if( !pLog || !familyName ) return;
		if( SurfaceCurvatureDemand::Any() || SurfaceSignalDemand::Any() ) {
			// NAMES ALL SIX.  The message used to say
			// "curv/occlusion/thickness", which had already drifted when
			// `convexity` shipped, drifted again when `proximity` did, and
			// again when `interior` joined it -- and a warning that does
			// not name the signal an author is actually using reads as
			// being about somebody else's problem.  The two CROSS-OBJECT
			// ones are the sharpest here: their neutrals mean "no contact
			// anywhere in the scene" and "buried in nothing", so a grime
			// mask simply stops painting.
			pLog->PrintEx( eLog_Warning,
				"%s:: curv/occlusion/thickness/convexity/proximity/interior evaluate as neutral "
				"in parts of BDPT/VCM/MLT transport; PT renders them fully -- see "
				"docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md and, for proximity and interior, "
				"docs/CROSS_OBJECT_PROXIMITY_DESIGN.md",
				familyName );
		}
	}
}

#endif
