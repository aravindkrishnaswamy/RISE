//////////////////////////////////////////////////////////////////////
//
//  CompositeSPF.cpp - Implementation of the Composite SPF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 6, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CompositeSPF.h"
#include "../Interfaces/ILog.h"

#include <atomic>

using namespace RISE;
using namespace RISE::Implementation;

// A composite is the only SPF in the engine that can produce MORE exit rays
// than `ScatteredRayContainer::kCapacity`, because it is the only one that
// re-enters another SPF's Scatter() inside its own: every extra gap round trip
// adds another batch of exits.  `AddScatteredRay` answers false and DISCARDS
// the ray when it is full, so an unchecked add loses that energy with no
// diagnostic at all -- and the loss is not even colour-neutral, because the
// dropped rays are whichever the walk emitted LAST (with a dispersive top the
// per-channel loop runs R, G, B in order, so BLUE starves first).
//
// kCapacity is sized from the measured worst case at the scene-language
// DEFAULT recursion budgets (12; see the constant's comment in ISPF.h), so
// this path should not fire on a default-budget scene.  It still can on a
// deliberately deep one -- a dispersive top at max_recur 12 / per-type 6
// measured 30 attempted exits per Scatter -- which is exactly the case that
// must be audible rather than silent.
//
// Warn ONCE per process (the SplatFilm.cpp / Object.cpp log-once idiom): this
// sits inside the per-sample scatter loop, so an unthrottled warning would
// emit millions of lines and cost more than the render.
static void NoteCompositeExitRayDropped()
{
	static std::atomic<bool> warnedExitRayDropped{ false };
	bool expected = false;
	if( warnedExitRayDropped.compare_exchange_strong( expected, true ) ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"CompositeSPF:: the scattered-ray container filled (capacity %u) and at least one "
			"exiting ray was DROPPED -- that ray's energy is lost from the image, and because "
			"the walk emits per-channel lobes in R,G,B order the loss is biased toward blue.  "
			"This composite's random walk produces more exits than the container can hold; "
			"lower the material's recursion budgets (max_recur / max_reflection_recursion / "
			"max_refraction_recursion / max_diffuse_recursion / max_translucent_recursion), "
			"or drop the per-channel (dispersive) IOR on the top layer, which triples the "
			"number of lobes each interface emits.  Reported once per process.",
			ScatteredRayContainer::kCapacity );
	}
}

// composite_material's `thickness` has a parser default of 0.0 and no range
// check, so a negative value reaches us intact.  It goes straight into the
// Beer-Lambert exponent below, where a negative path length turns attenuation
// into GAIN -- exp(-extinction * negative) > 1, unbounded energy created on
// every gap crossing.  That was harmless only while the walk was broken and
// nothing ever crossed the gap; it is reachable now, so clamp it to 0 (no
// gap, no absorption) and say so rather than rendering an energy source.
// The comparison is written NEGATED (`!(thickness >= 0)`) rather than
// `thickness < 0` so that a NaN thickness -- which compares false against
// EVERYTHING, and would sail through `< 0` -- is clamped too.  A NaN path
// length poisons the Beer-Lambert exponent and every kray downstream of it.
static Scalar ClampCompositeThickness( const Scalar thickness )
{
	if( !( thickness >= 0 ) ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"CompositeSPF:: inter-layer thickness (%g) is not >= 0 -- a negative path length makes the Beer-Lambert term amplify rather than absorb, and a NaN poisons it -- clamping to 0",
			thickness );
		return 0;
	}
	return thickness;
}

CompositeSPF::CompositeSPF(
	const ISPF& top_,
	const ISPF& bottom_,
	const unsigned int max_recur_ ,
	const unsigned int max_reflection_recursion_,		// maximum level of reflection recursion
	const unsigned int max_refraction_recursion_,		// maximum level of refraction recursion
	const unsigned int max_diffuse_recursion_,			// maximum level of diffuse recursion
	const unsigned int max_translucent_recursion_,		// maximum level of translucent recursion
	const Scalar thickness_,							// thickness between the materials
	const IScalarPainter& extinction_					// extinction coefficient for absorption between layers (physical scalar)
	) :
  top( top_ ),
  bottom( bottom_ ),
  max_recur( max_recur_ ),
  max_reflection_recursion( max_reflection_recursion_ ),
  max_refraction_recursion( max_refraction_recursion_ ),
  max_diffuse_recursion( max_diffuse_recursion_ ),
  max_translucent_recursion( max_translucent_recursion_ ),
  thickness( ClampCompositeThickness( thickness_ ) ),
  extinction( extinction_ )
{
	top.addref();
	bottom.addref();
	extinction.addref();
}

CompositeSPF::~CompositeSPF( )
{
	top.release();
	bottom.release();
	extinction.release();
}

// Beer-Lambert attenuation across ONE gap crossing, RGB path.
//
// The scalar painter's three components map 1:1 onto the R/G/B channels --
// the same shape `DielectricSPF` uses for its scalar `tau` / `ior`.  No
// colourspace conversion and no Jakob-Hanika uplift happen anywhere on this
// path, so an authored extinction of 8.0 arrives as 8.0 (see the member
// comment in CompositeSPF.h).
static RISEPel GapAttenuation(
	const IScalarPainter& extinction,
	const RayIntersectionGeometric& ri,
	const Scalar pathLength
	)
{
	const ScalarTriple e = extinction.GetValuesAt( ri );
	return ColorMath::exponential( RISEPel( e.v[0], e.v[1], e.v[2] ) * (-pathLength) );
}

// The slant distance a ray travels crossing the inter-layer gap.
//
// ONE function for all four recursion sites, and the reason it is a function
// at all: this length has TWO consumers that must agree -- it is the
// Beer-Lambert exponent's path length AND the distance the ray's origin is
// advanced.  Those two used to be written separately at each of the four
// sites, and they disagreed by exactly the 1/cos factor: the exponent used
// `thickness / cosTheta` while `Ray::Advance` moved only `thickness`.  Either
// layer can read its own absorption off `|ray.origin - ptIntersection|` --
// DielectricSPF's from-inside `tau^distance` (which is what the TOP layer
// applies on the return trip out of the gap), TranslucentSPF's and
// GenericHumanTissueSPF's `exp(-distance * extinction)` -- and every one of
// them was therefore handed the PERPENDICULAR crossing for a ray that had
// actually travelled the slant one, understating its own absorption by 1/cos.
// Deriving both from one value makes the disagreement unrepresentable.
//
// The clamped cosine, rather than `(cosTheta > NEARZERO) ? t/cosTheta : t`:
// that ternary substituted the SHORTEST possible crossing (`thickness`, the
// perpendicular one) for the most grazing rays, where the true slant distance
// runs the other way and diverges.  A cosine floor is continuous and monotone
// in cosTheta and keeps the limit on the correct side.
//
// kMinCosTheta is a genuine tuning knob (the quantity being guarded is not
// mathematically zero -- it is unbounded), so it is picked from what the two
// consumers can absorb rather than from FP noise:
//
//   * 1e-3 caps the crossing at 1000 x thickness.  For any physically
//     meaningful extinction that is already total absorption -- the shipped
//     composite_material scene's 8.0 over its 0.02 gap gives exp(-160) -- so
//     the cap does not truncate attenuation that would have been visible.
//   * It also caps the ORIGIN DISPLACEMENT at 1000 x thickness.  Without a
//     floor, a cosTheta of 1e-12 advances the ray 1e12 gap-thicknesses, which
//     is a nonsense position to hand a layer's Scatter() and, for a large
//     authored thickness, overflows outright.
//   * A cosine-weighted lobe puts P(cosTheta < 1e-3) = 1e-6 of its samples
//     inside the clamp, so no ordinary population is affected at all.
//
// The residual error is bounded and one-sided: below the floor a nearly
// transparent gap (extinction << 1/(1000 x thickness)) attenuates slightly
// LESS than the true unbounded slant path would. That is the price of
// bounding the displacement, and it is paid only by the 1-in-a-million ray
// that runs within 0.06 degrees of the slab plane.
const Scalar CompositeSPF::kMinCosTheta = 1e-3;

Scalar CompositeSPF::GapPathLength(
	const Vector3& dir,
	const Vector3& normal,
	const Scalar thickness
	)
{
	const Scalar cosTheta = fabs( Vector3Ops::Dot( dir, normal ) );
	return thickness / ( cosTheta > kMinCosTheta ? cosTheta : kMinCosTheta );
}

// ---------------------------------------------------------------------------
//  THE TWO-STACK WALK
//
//  A composite is ONE IObject with TWO interfaces, and IORStack keys its
//  entries on `pCurrentObject` (see IORStack.h) -- a single pointer for the
//  whole material.  An entry the TOP layer pushed is therefore
//  indistinguishable from an entry belonging to the BOTTOM layer.  That is
//  what makes a single stack threaded through the walk unworkable, and it is
//  the reason this walk carries two.
//
//  The two stacks name the two media a walk step can be evaluated against:
//
//    outside_stack : the stack WITHOUT this object's entry -- the medium
//                    above the top interface.  For a from-above walk this is
//                    simply the stack Scatter() was entered with.
//    gap_stack     : the stack of the inter-layer gap -- WITH the entry the
//                    top interface pushed.  Initialised to the entry stack
//                    (a top layer that pushes nothing leaves the gap medium
//                    equal to the outer medium, which is correct) and updated
//                    the moment the top layer's own scattered ray tells us
//                    what it pushed.
//
//  EvalStack() below picks between them by the direction of the ray arriving
//  at the layer, and the rule is the same for both layers: a DOWN-going ray
//  is arriving from the medium above that layer (outside for the top layer,
//  the gap for the bottom layer -- and the bottom layer must read
//  "entering from outside", which is what the WITHOUT-entry stack gives it),
//  while an UP-going ray is arriving from inside the object and must see the
//  gap stack so that a dielectric top layer correctly takes its from-inside
//  branch and refracts OUT.
//
//  HISTORY -- the two failure modes this replaces, one at each interface:
//
//   1. Passing the entry (outside) stack everywhere killed the RETURN trip
//      through a dielectric TOP layer: the up-going ray arrived with an
//      OUTSIDE stack, containsCurrent() reported false, DielectricSPF took
//      its "entering from outside" branch, and BOTH lobes were culled -- the
//      transmission lobe by the hemisphere gate (an upward direction cannot
//      be a transmission when entering from above) and the Fresnel lobe by
//      the geometric-normal gate (reflecting an upward ray about -N points
//      down).  Every gap-crossing path died inside the walk, which made
//      `extinction` and `thickness` -- which only ever apply to gap-crossing
//      legs -- exactly inert.
//
//   2. Threading each scattered ray's own stack UNCONDITIONALLY fixed (1) but
//      broke the BOTTOM interface by the same shared-key confusion, one layer
//      down: the down-going ray carries the top's push, so a stack-sensitive
//      bottom layer (DielectricSPF, TranslucentSPF, PerfectRefractorSPF, the
//      subsurface shaders, a nested CompositeSPF) read containsCurrent()==true
//      for a ray physically ENTERING it and took its from-inside branch.  A
//      dielectric bottom lost both of its lobes to the same two gates as (1)
//      -- a black interface; a translucent bottom ran its exit branch and
//      POPPED the entry the top had pushed (find_and_destroy matches on the
//      shared key), so the up-going lobe reached the top with a popped stack
//      and was double-culled -- failure (1), re-introduced.
//
//  tests/CompositeExtinctionTest.cpp section 6 is the regression guard for
//  (2); sections 1-5 guard (1).
//
//  KNOWN SCOPE GAPS -- both reviewer-verified, both UNCHANGED from the
//  pre-two-stack baseline, and both deliberately left alone here:
//
//   (a) A walk ENTERED FROM BELOW (Scatter() called on an up-going ray, i.e.
//       the camera/photon is inside a closed composite volume) starts BOTH
//       stacks at the entry stack -- and that stack already contains this
//       object's entry, because the ray is inside it.  So on a subsequent
//       DOWN-going leg the bottom layer is handed a WITH-entry stack even
//       though `EvalStack` is selecting the nominally "outside" one, and a
//       stack-sensitive bottom reads containsCurrent()==true for a ray
//       entering it.  Fixing this needs a stack that can express "the outside
//       medium" independently of the entry stack, which IORStack's shared
//       per-IObject key cannot do; it is the same structural limit as (2)
//       above, and no shipped scene puts a camera inside a composite.
//
//   (b) The BOTTOM interface's incident IOR is the OUTSIDE medium's, not the
//       gap's.  `EvalStack` gives a down-going ray the without-entry stack so
//       the bottom classifies the crossing correctly (entering, not exiting) --
//       but that stack also carries the IOR the bottom will refract against.
//       For a dielectric top over a dielectric bottom the gap->bottom crossing
//       is therefore computed as 1.0 -> 1.33 rather than 1.5 -> 1.33.  Again
//       structural: IORStack cannot express "the gap's IOR WITHOUT this
//       object's entry", because the entry IS how the gap's IOR got recorded.
//       Classification correctness was chosen over Ni fidelity, which is also
//       exactly what the pre-fix down-leg did -- so this is not a regression,
//       and the section-6 numbers bake it in.
// ---------------------------------------------------------------------------

// Picks the stack a layer's Scatter() is evaluated against, by the direction
// of the arriving ray.  See the block comment above for the rule and why it
// is the same for both layers.
const IORStack& CompositeSPF::EvalStack(
	const RayIntersectionGeometric& ri,
	const IORStack& outside_stack,
	const IORStack& gap_stack
	)
{
	return ( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) <= 0 ) ? outside_stack : gap_stack;
}

// Returns the gap stack for the leg BELOW the top interface.
//
// A ScatteredRay that crossed a refracting interface carries its OWN stack
// (DielectricSPF pushes the medium it just entered / pops the one it left),
// and for a ray the TOP layer sent DOWNWARD that stack IS the gap medium.
// Rays that carry no stack of their own (every non-refracting lobe: diffuse,
// glossy, mirror) did not change medium, so the gap stack is unchanged.
//
// Deliberately NOT applied to the bottom->top direction: an up-going ray out
// of the bottom layer carries a stack describing ITS OWN crossing, not the
// gap it is about to travel through, and the gap medium never changes
// mid-walk.
//
// Lifetime: the returned reference aliases either the caller's stack or the
// ScatteredRay's, and the ScatteredRay lives in the ScatteredRayContainer that
// the enclosing loop iterates -- the recursion completes long before that
// container is destroyed.
const IORStack& CompositeSPF::GapStackBelowTop(
	const ScatteredRay& scat,
	const IORStack& gap_stack
	)
{
	return scat.ior_stack ? *scat.ior_stack : gap_stack;
}

bool CompositeSPF::ShouldScatteredRayBePropagated(
	const ScatteredRay::ScatRayType type,
	const unsigned int steps
	) const
{
	switch( type )
	{
	case ScatteredRay::eRayReflection:
		return (steps<max_reflection_recursion);
		break;
	case ScatteredRay::eRayRefraction:
		return (steps<max_refraction_recursion);
		break;
	case ScatteredRay::eRayDiffuse:
		return (steps<max_diffuse_recursion);
		break;
	case ScatteredRay::eRayTranslucent:
		return (steps<max_translucent_recursion);
		break;
	default:
		return false;
		break;
	};

	return false;
}

void CompositeSPF::ProcessTopLayer(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const RISEPel& importance,									///< [in] Importance from prevous pass
				ISampler& sampler,				///< Sampler for the MC process
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const unsigned int steps,									///< [in] Number of steps taken in the random walk process
				const IORStack& outside_stack,							///< [in] Stack of the medium above the top interface (no entry for this object)
				const IORStack& gap_stack								///< [in] Stack of the inter-layer gap (with the top's pushed entry)
				) const
{
	if( steps >= max_recur || ColorMath::MaxValue(importance) < NEARZERO ) {
		return;
	}

	ScatteredRayContainer scat_top;
	top.Scatter( ri, sampler, scat_top, EvalStack( ri, outside_stack, gap_stack ) );

	for( unsigned int i=0; i<scat_top.Count(); i++ )
	{
		// For each ray...
		if( Vector3Ops::Dot( scat_top[i].ray.Dir(), ri.onb.w() ) >= 0 ) {
			// Exits from the top, so its all good
			scat_top[i].kray = scat_top[i].kray * importance;
			if( !scattered.AddScatteredRay( scat_top[i] ) ) {
				NoteCompositeExitRayDropped();
			}
		} else {
			if( ShouldScatteredRayBePropagated( scat_top[i].type, steps ) ) {
				// We must pass it off to the bottom
				RayIntersectionGeometric my_ri(ri);
				my_ri.ray.origin = ri.ptIntersection;
				my_ri.ray.SetDir(Vector3Ops::Normalize(scat_top[i].ray.Dir()));

				// Apply Beer's law absorption through the layer.  The SAME slant
				// length is both the exponent's path and the distance the ray
				// actually travels -- see GapPathLength.
				const Scalar pathLength = GapPathLength( my_ri.ray.Dir(), ri.onb.w(), thickness );
				my_ri.ray.Advance( pathLength );
				const RISEPel attenuation = GapAttenuation( extinction, ri, pathLength );

				ProcessBottomLayer( my_ri, scat_top[i].kray*importance*attenuation, sampler, scattered, steps+1, outside_stack, GapStackBelowTop( scat_top[i], gap_stack ) );
			}
		}
	}
}

void CompositeSPF::ProcessBottomLayer(
		const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
		const RISEPel& importance,									///< [in] Importance from prevous pass
		ISampler& sampler,				///< Sampler for the MC process
		ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
		const unsigned int steps,									///< [in] Number of steps taken in the random walk process
		const IORStack& outside_stack,							///< [in] Stack of the medium above the top interface (no entry for this object)
		const IORStack& gap_stack								///< [in] Stack of the inter-layer gap (with the top's pushed entry)
		) const
{
	if( steps >= max_recur || ColorMath::MaxValue(importance) < NEARZERO ) {
		return;
	}

	ScatteredRayContainer scat_bottom;
	bottom.Scatter( ri, sampler, scat_bottom, EvalStack( ri, outside_stack, gap_stack ) );

	for( unsigned int i=0; i<scat_bottom.Count(); i++ )
	{
		// For each ray...
		if( Vector3Ops::Dot( scat_bottom[i].ray.Dir(), ri.onb.w() ) <= 0 ) {
			// Exits from the bottom, so its all good
			scat_bottom[i].kray = scat_bottom[i].kray * importance;
			if( !scattered.AddScatteredRay( scat_bottom[i] ) ) {
				NoteCompositeExitRayDropped();
			}
		} else {
			if( ShouldScatteredRayBePropagated( scat_bottom[i].type, steps ) ) {
				// We must pass it back to the top
				RayIntersectionGeometric my_ri(ri);
				my_ri.ray.origin = ri.ptIntersection;
				my_ri.ray.SetDir(Vector3Ops::Normalize(scat_bottom[i].ray.Dir()));

				// Apply Beer's law absorption through the layer.  The SAME slant
				// length is both the exponent's path and the distance the ray
				// actually travels -- see GapPathLength.
				const Scalar pathLength = GapPathLength( my_ri.ray.Dir(), ri.onb.w(), thickness );
				my_ri.ray.Advance( pathLength );
				const RISEPel attenuation = GapAttenuation( extinction, ri, pathLength );

				// gap_stack is passed through UNCHANGED -- see GapStackBelowTop.
				ProcessTopLayer( my_ri, scat_bottom[i].kray*importance*attenuation, sampler, scattered, steps+1, outside_stack, gap_stack );
			}
		}
	}
}

void CompositeSPF::ProcessTopLayerNM(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const Scalar importance,									///< [in] Importance from prevous pass
				ISampler& sampler,				///< Sampler for the MC process
				const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const unsigned int steps,									///< [in] Number of steps taken in the random walk process
				const IORStack& outside_stack,							///< [in] Stack of the medium above the top interface (no entry for this object)
				const IORStack& gap_stack								///< [in] Stack of the inter-layer gap (with the top's pushed entry)
				) const
{
	if( steps >= max_recur || importance < NEARZERO ) {
		return;
	}

	ScatteredRayContainer scat_top;
	top.ScatterNM( ri, sampler, nm, scat_top, EvalStack( ri, outside_stack, gap_stack ) );

	for( unsigned int i=0; i<scat_top.Count(); i++ )
	{
		// For each ray...
		if( Vector3Ops::Dot( scat_top[i].ray.Dir(), ri.onb.w() ) >= 0 ) {
			// Exits from the top, so its all good
			scat_top[i].krayNM *= importance;
			if( !scattered.AddScatteredRay( scat_top[i] ) ) {
				NoteCompositeExitRayDropped();
			}
		} else {
			if( ShouldScatteredRayBePropagated( scat_top[i].type, steps ) ) {
				// We must pass it off to the bottom
				RayIntersectionGeometric my_ri(ri);
				my_ri.ray.origin = ri.ptIntersection;
				my_ri.ray.SetDir(Vector3Ops::Normalize(scat_top[i].ray.Dir()));

				// Apply Beer's law absorption through the layer.  The SAME slant
				// length is both the exponent's path and the distance the ray
				// actually travels -- see GapPathLength.
				const Scalar pathLength = GapPathLength( my_ri.ray.Dir(), ri.onb.w(), thickness );
				my_ri.ray.Advance( pathLength );
				const Scalar extinctionNM = extinction.GetValueAtNM(ri, nm);
				const Scalar attenuation = exp( -extinctionNM * pathLength );

				ProcessBottomLayerNM( my_ri, scat_top[i].krayNM*importance*attenuation, sampler, nm, scattered, steps+1, outside_stack, GapStackBelowTop( scat_top[i], gap_stack ) );
			}
		}
	}
}

void CompositeSPF::ProcessBottomLayerNM(
		const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
		const Scalar importance,									///< [in] Importance from prevous pass
		ISampler& sampler,				///< Sampler for the MC process
		const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
		ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
		const unsigned int steps,									///< [in] Number of steps taken in the random walk process
		const IORStack& outside_stack,							///< [in] Stack of the medium above the top interface (no entry for this object)
		const IORStack& gap_stack								///< [in] Stack of the inter-layer gap (with the top's pushed entry)
		) const
{
	if( steps >= max_recur || importance < NEARZERO ) {
		return;
	}

	ScatteredRayContainer scat_bottom;
	bottom.ScatterNM( ri, sampler, nm, scat_bottom, EvalStack( ri, outside_stack, gap_stack ) );

	for( unsigned int i=0; i<scat_bottom.Count(); i++ )
	{
		// For each ray...
		if( Vector3Ops::Dot( scat_bottom[i].ray.Dir(), ri.onb.w() ) <= 0 ) {
			// Exits from the bottom, so its all good
			scat_bottom[i].krayNM *= importance;
			if( !scattered.AddScatteredRay( scat_bottom[i] ) ) {
				NoteCompositeExitRayDropped();
			}
		} else {
			if( ShouldScatteredRayBePropagated( scat_bottom[i].type, steps ) ) {
				// We must pass it back to the top
				RayIntersectionGeometric my_ri(ri);
				my_ri.ray.origin = ri.ptIntersection;
				my_ri.ray.SetDir(Vector3Ops::Normalize(scat_bottom[i].ray.Dir()));

				// Apply Beer's law absorption through the layer.  The SAME slant
				// length is both the exponent's path and the distance the ray
				// actually travels -- see GapPathLength.
				const Scalar pathLength = GapPathLength( my_ri.ray.Dir(), ri.onb.w(), thickness );
				my_ri.ray.Advance( pathLength );
				const Scalar extinctionNM = extinction.GetValueAtNM(ri, nm);
				const Scalar attenuation = exp( -extinctionNM * pathLength );

				// gap_stack is passed through UNCHANGED -- see GapStackBelowTop.
				ProcessTopLayerNM( my_ri, scat_bottom[i].krayNM*importance*attenuation, sampler, nm, scattered, steps+1, outside_stack, gap_stack );
			}
		}
	}
}

void CompositeSPF::Scatter(
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
			ISampler& sampler,				///< [in] Sampler
			ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
			const IORStack& ior_stack								///< [in/out] Index of refraction stack
			) const
{
	// We do a random walk process between the materials until the rays
	// either exit the bottom material from the bottom, or exit the
	// top material from the top
	// Both stacks start at the stack Scatter() was entered with: nothing has
	// crossed the top interface yet, so the gap medium is still the outer one.
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) <= 0 ) {
		ProcessTopLayer( ri, RISEPel(1,1,1), sampler, scattered, 0, ior_stack, ior_stack );
	} else {
		ProcessBottomLayer( ri, RISEPel(1,1,1), sampler, scattered, 0, ior_stack, ior_stack );
	}

	for( unsigned int i=0; i<scattered.Count(); i++ ) {
		// To account for thicknesses
		scattered[i].ray.origin = ri.ptIntersection;
	}
}

void CompositeSPF::ScatterNM(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	// We do a random walk process between the materials until the rays
	// either exit the bottom material from the bottom, or exit the
	// top material from the top
	// Both stacks start at the entry stack -- see Scatter().
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) <= 0 ) {
		ProcessTopLayerNM( ri, 1, sampler, nm, scattered, 0, ior_stack, ior_stack );
	} else {
		ProcessBottomLayerNM( ri, 1, sampler, nm, scattered, 0, ior_stack, ior_stack );
	}

	for( unsigned int i=0; i<scattered.Count(); i++ ) {
		// To account for thicknesses
		scattered[i].ray.origin = ri.ptIntersection;
	}
}

Scalar CompositeSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	// The composite SPF uses a random walk between top and bottom layers,
	// making exact lobe weights impractical to compute analytically.
	// Equal weighting is the best approximation for this material.
	const Scalar pdf_top = top.Pdf( ri, wo, ior_stack );
	const Scalar pdf_bottom = bottom.Pdf( ri, wo, ior_stack );
	return 0.5 * (pdf_top + pdf_bottom);
}

Scalar CompositeSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	// See Pdf() comment — random walk makes exact weights impractical.
	const Scalar pdf_top = top.PdfNM( ri, wo, nm, ior_stack );
	const Scalar pdf_bottom = bottom.PdfNM( ri, wo, nm, ior_stack );
	return 0.5 * (pdf_top + pdf_bottom);
}

