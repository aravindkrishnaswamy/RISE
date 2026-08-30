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
#include "../Utilities/SurfaceCurvature.h"
#include "ILog.h"
#include <atomic>

namespace RISE
{
	struct SurfaceSignalInfo;

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

		SurfaceSignalInfo() :
		pProvider( 0 ), ptObject( 0, 0, 0 ), nObject( 0, 0, 0 ),
		primId( -1 ), baryA( 0 ), baryB( 0 )
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
		//!
		//! `bRadiusIsConstant` is the caller's PROOF that the radius is a
		//! compile-time constant -- see ISurfaceSignalProvider's own doc for
		//! why a bake cannot work without it.  It has no default: a caller
		//! that cannot prove it must say so and take the neutral fallback on
		//! the baked families, rather than inherit a silently optimistic one.
		Scalar Occlusion( const Scalar radiusFraction, const bool bRadiusIsConstant ) const
		{
			Scalar v = Scalar( 0 );
			if( pProvider && RadiusUsable( radiusFraction ) &&
			    pProvider->ComputeOcclusion( *this, radiusFraction, bRadiusIsConstant, v ) &&
			    RISE::IsFiniteDouble( static_cast<double>( v ) ) ) {
				return ( v < Scalar( 0 ) ) ? Scalar( 0 ) : ( ( v > Scalar( 1 ) ) ? Scalar( 1 ) : v );
			}
			return NeutralOcclusion();
		}

		//! Thickness at this hit, or NeutralThickness() when there is no
		//! provider, the radius is unusable, or the provider refuses.
		//! Always finite, always in [0,1].
		Scalar Thickness( const Scalar radiusFraction, const bool bRadiusIsConstant ) const
		{
			Scalar v = Scalar( 0 );
			if( pProvider && RadiusUsable( radiusFraction ) &&
			    pProvider->ComputeThickness( *this, radiusFraction, bRadiusIsConstant, v ) &&
			    RISE::IsFiniteDouble( static_cast<double>( v ) ) ) {
				return ( v < Scalar( 0 ) ) ? Scalar( 0 ) : ( ( v > Scalar( 1 ) ) ? Scalar( 1 ) : v );
			}
			return NeutralThickness();
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
	//! two relaxed atomic loads (SurfaceCurvatureDemand and
	//! SurfaceSignalDemand cover `curv` and `occlusion`/`thickness`
	//! respectively -- an expression can key on either family without
	//! calling the other), gated so a scene that never mentions any of
	//! the three signals costs nothing beyond the two loads.
	//!
	//! `pLog` may be null (defensive; every call site has a live log in
	//! practice) -- a null log means "cannot report," not "nothing to
	//! report," so the check is skipped silently rather than crashing.
	inline void WarnIfNonPTRenderHasLiveSignalConsumer( ILog* pLog, const char* familyName )
	{
		if( !pLog || !familyName ) return;
		if( SurfaceCurvatureDemand::Any() || SurfaceSignalDemand::Any() ) {
			pLog->PrintEx( eLog_Warning,
				"%s:: curv/occlusion/thickness evaluate as neutral in parts of BDPT/VCM/MLT "
				"transport; PT renders them fully -- see docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md",
				familyName );
		}
	}
}

#endif
