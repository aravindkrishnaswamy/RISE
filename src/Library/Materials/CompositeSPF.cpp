//////////////////////////////////////////////////////////////////////
//
//  CompositeSPF.cpp - Implementation of the Composite SPF and the
//    composite's layered BSDF.  See CompositeSPF.h for the DL-24 model
//    (DIRECT / COVERED / WALKER) and docs/DL24_COMPOSITE_ENERGY.md.
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

#include "../Utilities/GeometricUtilities.h"

#include <atomic>
#include <cstdint>
#include <cstring>

using namespace RISE;
using namespace RISE::Implementation;

// Before DL-24 (2026-09-28) a composite was the only SPF in the engine that
// could produce MORE exit rays than `ScatteredRayContainer::kCapacity`: its
// walk emitted every exit of every gap round trip into the caller's
// container, and a dispersive top reached 15 at the parser's default budgets
// and 30 at deep ones.  Since DL-24 `Scatter` emits AT MOST ONE ray per call
// (one branch of the DIRECT / COVERED / WALKER mixture, see CompositeSPF.h),
// and the walk's own per-layer containers receive one sub-SPF Scatter each
// (a dispersive DielectricSPF emits at most 6), so this path is not reachable
// from any layer SPF in the tree.  The check stays because `AddScatteredRay`
// answers false and DISCARDS the ray when full, and a future layer SPF that
// emits more than the capacity would otherwise lose energy with no
// diagnostic at all.
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
			"CompositeSPF:: the scattered-ray container filled (capacity %u) and a ray was "
			"DROPPED -- that ray's energy is lost from the image.  A composite emits at most one "
			"ray per Scatter, so this means a layer SPF emitted more lobes than the container "
			"holds.  Reported once per process.",
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

// A walk that reaches CompositeSPF::kMaxWalkEvents without Russian roulette
// ending it has either met a GENUINELY lossless trapping layer pair (e.g. a
// mirror facing down over an albedo-1 bottom: the energy it still carries
// can never leave the stack, so truncating it is exact in the limit), or --
// the case the DL-24 review found -- a stack whose IOR state is inconsistent:
// a NESTED composite walked from below with a stack that already holds the
// shared object key (a composite{dielectric/dielectric} as the TOP of another
// composite, or on a closed object seen from inside) leaves its two layers
// each reading the other side, and a ray ping-pongs between them under a
// total internal reflection that is not physical.  That energy is LOST, not
// merely trapped.  Either way the user deserves to hear about it once.
static void NoteCompositeWalkCapReached()
{
	static std::atomic<bool> warnedCap{ false };
	bool expected = false;
	if( warnedCap.compare_exchange_strong( expected, true ) ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"CompositeSPF:: a layer walk reached the %u-event safety cap still carrying energy, which "
			"is then dropped.  Either the two layers trap light losslessly (e.g. a downward-facing "
			"mirror top over an albedo-1 bottom), or -- more likely -- the stack's IOR state is "
			"inconsistent: a nested composite (a composite_material used as another composite's "
			"top, or a transmitting composite seen from inside a closed object) whose dielectric "
			"layers read each other's side and total-internally-reflect forever (DL-24 residual; "
			"energy is lost).  Reported once per process.",
			CompositeSPF::kMaxWalkEvents );
	}
}

static unsigned long long NextCompositeInstanceId()
{
	static std::atomic<unsigned long long> next{ 1 };
	return next.fetch_add( 1 );
}

CompositeSPF::CompositeSPF(
	const ISPF& top_,
	const ISPF& bottom_,
	const unsigned int max_recur_ ,
	const unsigned int max_reflection_recursion_,
	const unsigned int max_refraction_recursion_,
	const unsigned int max_diffuse_recursion_,
	const unsigned int max_translucent_recursion_,
	const Scalar thickness_,
	const IScalarPainter& extinction_,
	const IBSDF* pTopBSDF_,
	const IBSDF* pBottomBSDF_
	) :
  top( top_ ),
  bottom( bottom_ ),
  pTopBSDF( pTopBSDF_ ),
  pBottomBSDF( pBottomBSDF_ ),
  max_recur( max_recur_ ),
  max_reflection_recursion( max_reflection_recursion_ ),
  max_refraction_recursion( max_refraction_recursion_ ),
  max_diffuse_recursion( max_diffuse_recursion_ ),
  max_translucent_recursion( max_translucent_recursion_ ),
  thickness( ClampCompositeThickness( thickness_ ) ),
  extinction( extinction_ ),
  instanceId( NextCompositeInstanceId() )
{
	top.addref();
	bottom.addref();
	extinction.addref();
	if( pTopBSDF ) {
		pTopBSDF->addref();
	}
	if( pBottomBSDF ) {
		pBottomBSDF->addref();
	}
}

CompositeSPF::~CompositeSPF( )
{
	top.release();
	bottom.release();
	extinction.release();
	if( pTopBSDF ) {
		pTopBSDF->release();
	}
	if( pBottomBSDF ) {
		pBottomBSDF->release();
	}
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
	// Public entry point: enforce the non-negative precondition here too
	// (negated compare so NaN also lands on 0), not only at the ctor clamp.
	if( !( thickness >= 0 ) ) {
		return 0;
	}
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


// Continuation of `type` out of the walk event at `steps`: the old
// ShouldScatteredRayBePropagated() allowed it iff `steps < budget(type)` AND
// the next event `steps + 1 < max_recur`, and silently DROPPED it otherwise.
// That drop is DL-24's energy loss (the internal Fresnel/TIR reflection at the
// top interface's underside is exactly such a continuation).  The same
// predicate now marks where Russian roulette -- with its survival
// probability divided back out -- may begin.
bool CompositeSPF::IsRouletteEligible(
	const ScatteredRay::ScatRayType type,
	const unsigned int steps
	) const
{
	if( steps + 1 >= max_recur ) {
		return true;
	}

	switch( type )
	{
	case ScatteredRay::eRayReflection:
		return !( steps < max_reflection_recursion );
	case ScatteredRay::eRayRefraction:
		return !( steps < max_refraction_recursion );
	case ScatteredRay::eRayDiffuse:
		return !( steps < max_diffuse_recursion );
	case ScatteredRay::eRayTranslucent:
		return !( steps < max_translucent_recursion );
	default:
		return true;
	}
}

const unsigned int CompositeSPF::kMaxWalkEvents = 256;

// ===========================================================================
//  DL-24 -- THE LAYERED TRANSPORT
//
//  See CompositeSPF.h for the three-class model (DIRECT / COVERED / WALKER).
//  Everything below is written once, generically over a "pipe" (RGB or a
//  single wavelength), so the RGB and NM twins cannot drift apart
//  (docs/skills/audit-by-bug-pattern.md's recurring RGB/NM-twin defect).
//
//  SELECTION.  Every sub-SPF emits ALL of its lobes into a container, and
//  `ScatteredRayContainer::RandomlySelect` picks one with probability
//  proportional to MaxValue(kray) (krayNM on the NM pipe), a lone ray with
//  probability 1.  Every selection here uses exactly that rule -- either
//  over the whole container (the "natural" selection a sub-SPF's own `Pdf`
//  describes, DL-67 Slice 0) or restricted to one side of the layer (a
//  CONDITIONAL selection of that rule), always dividing the realized
//  probability back out, so each walk is an unbiased estimator of the
//  transport it follows.
// ===========================================================================

namespace RISE
{
	namespace Implementation
	{
		struct CompositeSPFImpl
		{
			// -----------------------------------------------------------
			//  Hashing and the deterministic evaluator sampler.
			//
			//  The layered evaluator must be a DETERMINISTIC function of
			//  (incoming direction, outgoing direction, position): that is
			//  what makes an emitted covered ray's kray EXACTLY
			//  value(dir) * cos / Pdf(dir), and what makes its HWSS
			//  companion weight reconstructible from (ri, dir, nm).  The
			//  walk inside it is therefore driven by a PCG32 stream seeded
			//  from a hash of those arguments -- PBRT-v4's LayeredBxDF::f
			//  does the same (its RNG is seeded from Hash(wo), Hash(wi));
			//  the POSITION is added here so a flat surface under an
			//  orthographic view and a directional light does not reuse one
			//  estimate at every pixel.  The wavelength is deliberately NOT
			//  hashed: a companion wavelength then re-runs the SAME walk,
			//  so hero and companion estimates are correlated rather than
			//  independent.
			//
			//  Inputs are QUANTIZED (the low 20 of 52 mantissa bits
			//  dropped) before hashing, so a record rebuilt from stored
			//  vertex data a few ULPs away from the original (BDPT's
			//  companion replay) reproduces the same stream.
			// -----------------------------------------------------------
			static inline uint64_t SplitMix64( uint64_t x )
			{
				x += 0x9E3779B97F4A7C15ull;
				x = ( x ^ ( x >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
				x = ( x ^ ( x >> 27 ) ) * 0x94D049BB133111EBull;
				return x ^ ( x >> 31 );
			}

			static inline uint64_t QuantizedBits( Scalar v )
			{
				v = v + Scalar( 0 );			// -0.0 -> +0.0
				static_assert( sizeof( Scalar ) == sizeof( uint64_t ), "Scalar must be a 64-bit double" );
				uint64_t b;
				std::memcpy( &b, &v, sizeof( b ) );
				return b >> 20;
			}

			static inline uint64_t HashIn( uint64_t h, const Scalar v )
			{
				return SplitMix64( h ^ QuantizedBits( v ) );
			}

			static inline uint64_t HashVector( uint64_t h, const Vector3& v )
			{
				h = HashIn( h, v.x );
				h = HashIn( h, v.y );
				return HashIn( h, v.z );
			}

			static inline uint64_t HashPoint( uint64_t h, const Point3& p )
			{
				h = HashIn( h, p.x );
				h = HashIn( h, p.y );
				return HashIn( h, p.z );
			}

			static const uint64_t kSaltProbeA   = 0x5A17C0DE00000001ull;
			static const uint64_t kSaltProbeB   = 0x5A17C0DE00000002ull;
			static const uint64_t kSaltProbeC   = 0x5A17C0DE00000003ull;
			static const uint64_t kSaltEvaluate = 0x5A17C0DE00000004ull;
			static const uint64_t kSaltEvaluateExit = 0x5A17C0DE00000005ull;

			//! PCG32 (O'Neill 2014).  Local, stack-allocated, never shared
			//! across threads.
			class HashedSampler : public ISampler
			{
				uint64_t state;
				uint64_t inc;

				inline uint32_t Next()
				{
					const uint64_t old = state;
					state = old * 6364136223846793005ull + inc;
					const uint32_t xorshifted = (uint32_t)( ( ( old >> 18u ) ^ old ) >> 27u );
					const uint32_t rot = (uint32_t)( old >> 59u );
					return ( xorshifted >> rot ) | ( xorshifted << ( ( 0u - rot ) & 31u ) );
				}

			public:
				explicit HashedSampler( const uint64_t seed ) :
				  state( 0 ),
				  inc( ( SplitMix64( seed ) << 1u ) | 1u )
				{
					Next();
					state += seed;
					Next();
				}

				Scalar Get1D()
				{
					const uint64_t hi = Next();
					const uint64_t lo = Next();
					const uint64_t bits = ( ( hi << 21 ) ^ lo ) & ( ( uint64_t( 1 ) << 53 ) - 1 );
					return Scalar( bits ) * ( Scalar( 1 ) / Scalar( uint64_t( 1 ) << 53 ) );
				}

				Point2 Get2D()
				{
					const Scalar a = Get1D();
					const Scalar b = Get1D();
					return Point2( a, b );
				}
			};

			// -----------------------------------------------------------
			//  Pipes.
			// -----------------------------------------------------------
			struct PipeRGB
			{
				typedef RISEPel T;
				static const bool kNM = false;
				static T Zero() { return RISEPel( 0, 0, 0 ); }
				static T One() { return RISEPel( 1, 1, 1 ); }
				static T Kray( const ScatteredRay& r ) { return r.kray; }
				static void SetKray( ScatteredRay& r, const T& v ) { r.kray = v; }
				static Scalar Weight( const ScatteredRay& r ) { return ColorMath::MaxValue( r.kray ); }
				static Scalar MaxOf( const T& v ) { return ColorMath::MaxValue( v ); }
				static T Scaled( const T& v, const Scalar s ) { return v * s; }
				static T Mul( const T& a, const T& b ) { return a * b; }
				static void Scatter( const ISPF& spf, const RayIntersectionGeometric& ri, ISampler& s, const Scalar, ScatteredRayContainer& c, const IORStack& st )
				{
					spf.Scatter( ri, s, c, st );
				}
				static Scalar Pdf( const ISPF& spf, const RayIntersectionGeometric& ri, const Vector3& wo, const Scalar, const IORStack& st )
				{
					return spf.Pdf( ri, wo, st );
				}
				static T Value( const IBSDF& b, const Vector3& v, const RayIntersectionGeometric& ri, const Scalar, const IORStack* st )
				{
					return b.valueStateful( v, ri, st );
				}
				static T GapAtt( const IScalarPainter& ext, const RayIntersectionGeometric& ri, const Scalar, const Scalar pathLength )
				{
					return GapAttenuation( ext, ri, pathLength );
				}
			};

			struct PipeNM
			{
				typedef Scalar T;
				static const bool kNM = true;
				static T Zero() { return 0; }
				static T One() { return 1; }
				static T Kray( const ScatteredRay& r ) { return r.krayNM; }
				static void SetKray( ScatteredRay& r, const T& v ) { r.krayNM = v; }
				static Scalar Weight( const ScatteredRay& r ) { return r.krayNM; }
				static Scalar MaxOf( const T& v ) { return v; }
				static T Scaled( const T& v, const Scalar s ) { return v * s; }
				static T Mul( const T& a, const T& b ) { return a * b; }
				static void Scatter( const ISPF& spf, const RayIntersectionGeometric& ri, ISampler& s, const Scalar nm, ScatteredRayContainer& c, const IORStack& st )
				{
					spf.ScatterNM( ri, s, nm, c, st );
				}
				static Scalar Pdf( const ISPF& spf, const RayIntersectionGeometric& ri, const Vector3& wo, const Scalar nm, const IORStack& st )
				{
					return spf.PdfNM( ri, wo, nm, st );
				}
				static T Value( const IBSDF& b, const Vector3& v, const RayIntersectionGeometric& ri, const Scalar nm, const IORStack* st )
				{
					return b.valueStatefulNM( v, ri, nm, st );
				}
				static T GapAtt( const IScalarPainter& ext, const RayIntersectionGeometric& ri, const Scalar nm, const Scalar pathLength )
				{
					return exp( -ext.GetValueAtNM( ri, nm ) * pathLength );
				}
			};

			// -----------------------------------------------------------
			//  Geometry helpers.
			// -----------------------------------------------------------

			//! The composite is entered through its TOP when the ray arrives
			//! against the shading normal -- the convention the walk has
			//! always used.
			static inline bool EntryFromTop( const RayIntersectionGeometric& ri )
			{
				return Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) <= 0;
			}

			//! Ray-anchored geometric-horizon test (the tree-wide idiom:
			//! LambertianSPF, GGXSPF, ...): `w` must leave on the side the
			//! incoming ray arrived from.  Immune to a double-sided mesh's
			//! flipped normal by construction (it re-anchors to the ray).
			static inline bool PassesGeomGate( const RayIntersectionGeometric& ri, const Vector3& w )
			{
				const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar( 1e-12 ) )
					? ri.vGeomNormal : ri.onb.w();
				const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
				return Vector3Ops::Dot( w, geomN ) > 0;
			}

			//! The record a layer event is evaluated at: the same shading
			//! point, a ray along `dir` whose origin is advanced by the gap
			//! crossing's slant length so a layer that reads its own
			//! absorption off `|ray.origin - ptIntersection|` sees the
			//! distance actually travelled (see GapPathLength).
			static inline RayIntersectionGeometric LayerRecord(
				const RayIntersectionGeometric& ri,
				const Vector3& dir,
				const Scalar pathLength
				)
			{
				RayIntersectionGeometric r( ri );
				SetLayerRay( r, ri, dir, pathLength );
				return r;
			}

			//! In-place form of LayerRecord for the walks, which re-aim ONE
			//! record at every event instead of copying the (~1 KB) record
			//! each time: only the ray differs between a walk's events.
			static inline void SetLayerRay(
				RayIntersectionGeometric& r,
				const RayIntersectionGeometric& ri,
				const Vector3& dir,
				const Scalar pathLength
				)
			{
				r.ray.origin = ri.ptIntersection;
				r.ray.SetDir( dir );
				r.ray.Advance( pathLength );
			}

			//! The internal UP-going direction that a Snell refraction out
			//! through the top carries to the outside direction `w`, for a
			//! relative index eta = n_gap / n_outside.  False when no
			//! internal direction reaches `w` (eta < 1 beyond its critical
			//! angle).
			static inline bool InternalDirectionForExit(
				const Vector3& w,
				const Vector3& n,
				const Scalar eta,
				Vector3& u
				)
			{
				if( !( eta > 0 ) || eta == Scalar( 1 ) ) {
					u = w;
					return true;
				}
				const Scalar c = Vector3Ops::Dot( w, n );
				const Vector3 t = w - n * c;
				const Scalar sin2 = Vector3Ops::SquaredModulus( t ) / ( eta * eta );
				if( !( sin2 < 1 ) ) {
					return false;
				}
				u = Vector3Ops::Normalize( t * ( Scalar( 1 ) / eta ) + n * sqrt( Scalar( 1 ) - sin2 ) );
				return true;
			}

			//! Opaque identity key for the IOR stack of a STACKLESS
			//! evaluation (`IBSDF::value` without a stack).  `IORStack`
			//! only ever COMPARES its object keys (containsObject /
			//! find_and_destroy) and hands `topObject()` back to callers --
			//! it never dereferences them -- and this stack never leaves
			//! the evaluator, so any unique address serves as the key.
			static const IObject* StacklessKey()
			{
				static const char key = 0;
				return reinterpret_cast<const IObject*>( &key );
			}

			// -----------------------------------------------------------
			//  Selection (mirrors ScatteredRayContainer::RandomlySelect).
			// -----------------------------------------------------------

			//! Probability that the NATURAL selection over `c` lands in the
			//! subset `pred` selects.
			template<class P, class Pred>
			static Scalar SubsetMass( const ScatteredRayContainer& c, Pred pred )
			{
				const unsigned int count = c.Count();
				if( count == 0 ) {
					return 0;
				}
				if( count == 1 ) {
					return pred( c[0] ) ? Scalar( 1 ) : Scalar( 0 );
				}
				Scalar total = 0, sub = 0;
				for( unsigned int i = 0; i < count; i++ ) {
					const Scalar w = P::Weight( c[i] );
					total += w;
					if( pred( c[i] ) ) {
						sub += w;
					}
				}
				return ( total > NEARZERO ) ? ( sub / total ) : Scalar( 0 );
			}

			//! The natural selection CONDITIONED on `pred`: returns the index
			//! of the chosen ray (or -1) and its conditional probability.
			template<class P, class Pred>
			static int SelectSubset( const ScatteredRayContainer& c, Pred pred, const Scalar u, Scalar& q )
			{
				const unsigned int count = c.Count();
				q = 0;
				if( count == 0 ) {
					return -1;
				}
				if( count == 1 ) {
					if( pred( c[0] ) ) {
						q = 1;
						return 0;
					}
					return -1;
				}
				Scalar total = 0, sub = 0;
				for( unsigned int i = 0; i < count; i++ ) {
					const Scalar w = P::Weight( c[i] );
					total += w;
					if( pred( c[i] ) ) {
						sub += w;
					}
				}
				if( !( total > NEARZERO ) || !( sub > 0 ) ) {
					return -1;
				}
				const Scalar target = u * sub;
				Scalar acc = 0;
				int last = -1;
				for( unsigned int i = 0; i < count; i++ ) {
					if( !pred( c[i] ) ) {
						continue;
					}
					const Scalar w = P::Weight( c[i] );
					if( !( w > 0 ) ) {
						continue;
					}
					last = (int)i;
					acc += w;
					if( target < acc ) {
						q = w / sub;
						return (int)i;
					}
				}
				if( last >= 0 ) {
					q = P::Weight( c[last] ) / sub;
				}
				return last;
			}

			//! A THROUGHPUT-AWARE selection for the walks' own continuations,
			//! which make no density claim (the walker's emissions are
			//! delta-tagged; the evaluator is an estimate): any selection
			//! whose probabilities are divided back out is unbiased, and
			//! weighting by MaxOf(beta * kray) never spends a draw on a lobe
			//! the path cannot carry.  That matters for a DISPERSIVE top on
			//! the RGB pipe, whose per-channel rays each carry one channel:
			//! once a path has picked a channel, selecting by MaxValue(kray)
			//! alone would pick a dead channel two times in three.
			template<class P, class Pred>
			static int SelectCarried(
				const ScatteredRayContainer& c,
				Pred pred,
				const typename P::T& beta,
				const Scalar u,
				Scalar& q
				)
			{
				q = 0;
				Scalar sub = 0;
				for( unsigned int i = 0; i < c.Count(); i++ ) {
					if( pred( c[i] ) ) {
						const Scalar w = P::MaxOf( P::Mul( beta, P::Kray( c[i] ) ) );
						if( w > 0 ) {
							sub += w;
						}
					}
				}
				if( !( sub > 0 ) ) {
					return -1;
				}
				const Scalar target = u * sub;
				Scalar acc = 0;
				int last = -1;
				for( unsigned int i = 0; i < c.Count(); i++ ) {
					if( !pred( c[i] ) ) {
						continue;
					}
					const Scalar w = P::MaxOf( P::Mul( beta, P::Kray( c[i] ) ) );
					if( !( w > 0 ) ) {
						continue;
					}
					last = (int)i;
					acc += w;
					if( target < acc ) {
						q = w / sub;
						return (int)i;
					}
				}
				if( last >= 0 ) {
					q = P::MaxOf( P::Mul( beta, P::Kray( c[last] ) ) ) / sub;
				}
				return last;
			}

			// -----------------------------------------------------------
			//  Russian roulette, compensated.
			// -----------------------------------------------------------
			template<class P>
			static bool Roulette(
				const CompositeSPF& s,
				typename P::T& beta,
				const ScatteredRay::ScatRayType type,
				const unsigned int steps,
				ISampler& sampler
				)
			{
				if( !s.IsRouletteEligible( type, steps ) ) {
					return true;
				}
				const Scalar p = r_min( Scalar( 1 ), P::MaxOf( beta ) );
				if( !( p > 0 ) ) {
					return false;
				}
				if( p >= Scalar( 1 ) ) {
					return true;
				}
				if( sampler.Get1D() >= p ) {
					return false;
				}
				beta = P::Scaled( beta, Scalar( 1 ) / p );
				return true;
			}

			// -----------------------------------------------------------
			//  The PROBE -- a deterministic look at the top layer at this
			//  entry that decides the branch weights of Scatter.  Scatter
			//  and Pdf both run it from the same hash seeds, so they agree
			//  on the weights exactly.
			// -----------------------------------------------------------
			struct Probe
			{
				Scalar Qup;					// natural-selection mass of the top's up-going (direct) lobes
				Scalar Qdown;				// ... of its down-going (transmitted) lobes
				bool   det;					// the top DECLARES a deterministic up/down split (ISPF::SelectionMassIsDeterministic)
				bool   walkerPossible;		// a one-path look found transport the evaluator cannot price
			};

			template<class P>
			static Probe ComputeProbe(
				const CompositeSPF& s,
				const RayIntersectionGeometric& ri,
				const IORStack& outside,
				const Scalar nm
				)
			{
				const Vector3 n = ri.onb.w();
				auto isUp   = [&n]( const ScatteredRay& r ) { return Vector3Ops::Dot( r.ray.Dir(), n ) >= 0; };
				auto isDown = [&n]( const ScatteredRay& r ) { return Vector3Ops::Dot( r.ray.Dir(), n ) < 0; };

				const uint64_t seed = HashPoint( HashVector( 0x243F6A8885A308D3ull, ri.ray.Dir() ), ri.ptIntersection );

				ScatteredRayContainer c1;
				{
					HashedSampler hs( seed ^ kSaltProbeA );
					P::Scatter( s.top, ri, hs, nm, c1, outside );
				}
				const Scalar up1 = SubsetMass<P>( c1, isUp ),   dn1 = SubsetMass<P>( c1, isDown );

				// DETERMINISM IS DECLARED, NEVER INFERRED (DL-24 review P1-1).
				// An earlier revision compared two hashed draws and called
				// the top deterministic when they agreed.  For a SINGLE-EMIT
				// stochastic top (a nested composite, generic_human_tissue,
				// a thin weave) one draw's up mass is exactly 0 or 1, two
				// draws agree at least half the time, and AGGREGATE mode then
				// gave the DIRECT branch (or every down branch) probability
				// ZERO at that shading point -- transport never sampled, a
				// bias (tissue over white, 60 deg, position-jittered furnace:
				// 0.6250 against truth 1).  So only a top that DECLARES
				// `ISPF::SelectionMassIsDeterministic()` takes the aggregate
				// path; every other top runs PER-BRANCH mode, whose weights
				// need only be deterministic positive numbers (the floors in
				// MakeWeights keep every class reachable).  Two hashed probes
				// are averaged there purely to make those numbers closer to
				// the true split -- an efficiency choice, not a correctness
				// one.
				const bool declared = s.top.SelectionMassIsDeterministic();
				ScatteredRayContainer c2;
				if( !declared ) {
					HashedSampler hs( seed ^ kSaltProbeB );
					P::Scatter( s.top, ri, hs, nm, c2, outside );
				}
				const Scalar up2 = declared ? up1 : SubsetMass<P>( c2, isUp );
				const Scalar dn2 = declared ? dn1 : SubsetMass<P>( c2, isDown );

				Probe pr;
				pr.det = declared;
				pr.Qup   = pr.det ? up1 : Scalar( 0.5 ) * ( up1 + up2 );
				pr.Qdown = pr.det ? dn1 : Scalar( 0.5 ) * ( dn1 + dn2 );
				pr.walkerPossible = !s.HasLayeredValue();

				if( pr.walkerPossible || !( dn1 > 0 || dn2 > 0 ) ) {
					return pr;
				}

				// Follow ONE transmitted ray one round trip and see whether
				// the walk can reach anything the evaluator does not price
				// (a delta bottom lobe, transmission out through the bottom,
				// a non-delta event at a layer with no BSDF).  Efficiency
				// only: the walker branch keeps a floor share whatever this
				// says, so a miss here costs variance, never energy.
				const ScatteredRayContainer& src = ( dn1 > 0 ) ? c1 : c2;
				int best = -1;
				Scalar bestW = -1;
				for( unsigned int i = 0; i < src.Count(); i++ ) {
					if( isDown( src[i] ) && P::Weight( src[i] ) > bestW ) {
						bestW = P::Weight( src[i] );
						best = (int)i;
					}
				}
				if( best < 0 ) {
					return pr;
				}
				const Vector3 wd = Vector3Ops::Normalize( src[best].ray.Dir() );
				const IORStack gap( src[best].ior_stack ? *src[best].ior_stack : outside );
				const RayIntersectionGeometric rb = LayerRecord( ri, wd, CompositeSPF::GapPathLength( wd, n, s.thickness ) );

				HashedSampler hs( seed ^ kSaltProbeC );
				ScatteredRayContainer cb;
				P::Scatter( s.bottom, rb, hs, nm, cb, outside );
				int bestUp = -1;
				bestW = -1;
				for( unsigned int i = 0; i < cb.Count(); i++ ) {
					const ScatteredRay& r = cb[i];
					const bool up = Vector3Ops::Dot( r.ray.Dir(), n ) > 0;
					if( r.isDelta || !up || !s.pBottomBSDF ) {
						pr.walkerPossible = true;
					}
					if( up && P::Weight( r ) > bestW ) {
						bestW = P::Weight( r );
						bestUp = (int)i;
					}
				}
				if( pr.walkerPossible || bestUp < 0 ) {
					return pr;
				}
				const Vector3 wu = Vector3Ops::Normalize( cb[bestUp].ray.Dir() );
				const RayIntersectionGeometric rt = LayerRecord( ri, wu, CompositeSPF::GapPathLength( wu, n, s.thickness ) );
				ScatteredRayContainer ct;
				P::Scatter( s.top, rt, hs, nm, ct, gap );
				for( unsigned int i = 0; i < ct.Count(); i++ ) {
					const ScatteredRay& r = ct[i];
					if( Vector3Ops::Dot( r.ray.Dir(), n ) >= 0 && !r.isDelta && !s.pTopBSDF ) {
						pr.walkerPossible = true;
					}
				}
				return pr;
			}

			// -----------------------------------------------------------
			//  PROBE CACHE.  At one path vertex the SAME probe is needed by
			//  Scatter, by every NEE sample's MIS partner (Pdf), by the
			//  escape-side partner and by every HWSS companion's
			//  EvaluateLobeFNM.  The probe is a deterministic function of
			//  (composite, wavelength, record, IOR stack), so a small
			//  per-thread cache keyed on EXACTLY those inputs returns
			//  bit-identical results and changes nothing but cost.  Keyed on
			//  the composite's process-unique instanceId (never its address,
			//  which a later composite can reuse) and on the record fields a
			//  sub-SPF or its painters read at the shading point (direction,
			//  both points, both normals, the tangent, both UV charts, the
			//  glossy-filter width, the texture footprint, the IOR-stack
			//  state).  A miss only costs a recomputation; a hit requires
			//  every one of those to be bit-identical.
			// -----------------------------------------------------------
			static const int kProbeKeySize = 32;

			struct ProbeCacheEntry
			{
				bool                valid;
				unsigned long long  id;
				Scalar              nm;
				Scalar              key[kProbeKeySize];
				Probe               probe;
			};
			static const int kProbeCacheSize = 4;

			static inline void ProbeKey( const RayIntersectionGeometric& ri, const IORStack& st, Scalar key[kProbeKeySize] )
			{
				const Vector3 d = ri.ray.Dir();
				const Vector3 u = ri.onb.u();
				int i = 0;
				key[i++] = d.x;                     key[i++] = d.y;                     key[i++] = d.z;
				key[i++] = ri.ptIntersection.x;     key[i++] = ri.ptIntersection.y;     key[i++] = ri.ptIntersection.z;
				key[i++] = ri.ptObjIntersec.x;      key[i++] = ri.ptObjIntersec.y;      key[i++] = ri.ptObjIntersec.z;
				key[i++] = ri.vNormal.x;            key[i++] = ri.vNormal.y;            key[i++] = ri.vNormal.z;
				key[i++] = ri.vGeomNormal.x;        key[i++] = ri.vGeomNormal.y;        key[i++] = ri.vGeomNormal.z;
				key[i++] = u.x;                     key[i++] = u.y;                     key[i++] = u.z;
				key[i++] = ri.ptCoord.x;            key[i++] = ri.ptCoord.y;
				key[i++] = ri.ptCoord1.x;           key[i++] = ri.ptCoord1.y;
				key[i++] = ri.glossyFilterWidth;
				key[i++] = ri.txFootprint.worldWidth;
				key[i++] = ri.txFootprint.objectWidth;
				key[i++] = ri.txFootprint.dudx;     key[i++] = ri.txFootprint.dvdy;
				key[i++] = ( ri.txFootprint.valid ? Scalar( 1 ) : Scalar( 0 ) ) + ( ri.bGeomNormalOrientedToRay ? Scalar( 2 ) : Scalar( 0 ) );
				key[i++] = st.top();
				key[i++] = st.containsCurrent() ? Scalar( 1 ) : Scalar( 0 );
				key[i++] = ri.ambientIOR;
				key[i++] = ri.ray.origin.x;
			}

			template<class P>
			static Probe DoProbe(
				const CompositeSPF& s,
				const RayIntersectionGeometric& ri,
				const IORStack& outside,
				const Scalar nm
				)
			{
				thread_local ProbeCacheEntry cache[kProbeCacheSize] = {};
				thread_local int next = 0;
				Scalar key[kProbeKeySize];
				ProbeKey( ri, outside, key );
				for( int i = 0; i < kProbeCacheSize; i++ ) {
					const ProbeCacheEntry& e = cache[i];
					if( e.valid && e.id == s.instanceId && e.nm == nm &&
						std::memcmp( e.key, key, sizeof( key ) ) == 0 ) {
						return e.probe;
					}
				}
				const Probe pr = ComputeProbe<P>( s, ri, outside, nm );
				ProbeCacheEntry& e = cache[next];
				next = ( next + 1 ) % kProbeCacheSize;
				e.valid = true;
				e.id = s.instanceId;
				e.nm = nm;
				std::memcpy( e.key, key, sizeof( key ) );
				e.probe = pr;
				return pr;
			}

			// -----------------------------------------------------------
			//  Branch weights.
			//
			//    w1  DIRECT   the top's own up-going lobes, drawn by a
			//                 CONDITIONAL selection of the top's natural
			//                 rule.  w1 = Qup, so w1 * (conditional density)
			//                 == the top's own natural density top.Pdf.
			//    w2  COVERED  cosine-hemisphere proposal
			//    w3  COVERED  the bottom's own sampler at the OUTER record
			//                 (CoatedSPF's substrate proposal)
			//    w4  WALKER   single-path walk, delta-tagged emissions
			//
			//  w2 + w3 + w4 = Qdown.  AGGREGATE mode (every non-delta
			//  emission priced value*cos/Pdf, Pdf exact) requires Qup to be
			//  deterministic given the entry, which the TOP must DECLARE
			//  (`ISPF::SelectionMassIsDeterministic`: dielectric, perfect
			//  reflector/refractor, translucent).  Not inferred from
			//  samples (DL-24 review P1-1), and not assumed for a
			//  reflection-only top either: a GGX or Lambertian top can
			//  emit NOTHING on a random draw (a sample below the geometric
			//  horizon), which would read as Qup == 0.
			//  Otherwise PER-BRANCH mode: each branch prices only its own
			//  class, which is unbiased for ANY positive weights, and Pdf is
			//  an MIS partner rather than the exact density (legal by
			//  DL-74: MIS is unbiased for any deterministic weights that
			//  partition to one).
			// -----------------------------------------------------------
			struct Weights
			{
				Scalar w1, w2, w3, w4;
				bool   aggregate;
			};

			static const Scalar kShareFloor;
			static const Scalar kWalkerShareIfPossible;
			static const Scalar kWalkerShareFloor;

			static Weights MakeWeights( const CompositeSPF& s, const Probe& pr )
			{
				Weights W;
				W.w1 = W.w2 = W.w3 = W.w4 = 0;
				W.aggregate = pr.det;

				Scalar up = pr.Qup;
				Scalar dn = pr.Qdown;
				if( !pr.det ) {
					// A stochastic probe's zero is not evidence of zero mass:
					// keep both classes reachable.
					up = r_max( up, kShareFloor );
					dn = r_max( dn, kShareFloor );
					const Scalar t = up + dn;
					up /= t;
					dn /= t;
				}
				if( !( up + dn > 0 ) ) {
					return W;
				}
				W.w1 = up;
				if( dn > 0 ) {
					if( !s.HasLayeredValue() ) {
						W.w4 = dn;
					} else {
						const Scalar u = pr.walkerPossible ? kWalkerShareIfPossible : kWalkerShareFloor;
						W.w4 = dn * u;
						const Scalar e = dn - W.w4;
						if( s.pBottomBSDF ) {
							W.w2 = Scalar( 0.5 ) * e;
							W.w3 = Scalar( 0.5 ) * e;
						} else {
							W.w2 = e;
						}
					}
				}
				return W;
			}

			//! MakeWeights normalised to sum to one (it already does up to
			//! rounding; Scatter and Pdf must use the SAME numbers).
			static Weights NormalizedWeights( const CompositeSPF& s, const Probe& pr, bool& any )
			{
				Weights W = MakeWeights( s, pr );
				const Scalar total = W.w1 + W.w2 + W.w3 + W.w4;
				any = ( total > 0 );
				if( any ) {
					W.w1 /= total;
					W.w2 /= total;
					W.w3 /= total;
					W.w4 /= total;
				}
				return W;
			}

			//! Density of the NON-DELTA emissions at `w` (unit, entry side),
			//! given the probe and weights Scatter used.
			template<class P>
			static Scalar MixturePdf(
				const CompositeSPF& s,
				const RayIntersectionGeometric& ri,
				const Vector3& w,
				const IORStack& outside,
				const Scalar nm,
				const Probe& pr,
				const Weights& W,
				Scalar* pWalkedOnly
				)
			{
				const Scalar cosN = Vector3Ops::Dot( w, ri.onb.w() );
				Scalar walked = 0;
				if( W.w2 > 0 && PassesGeomGate( ri, w ) ) {
					walked += W.w2 * cosN * INV_PI;
				}
				if( W.w3 > 0 ) {
					walked += W.w3 * P::Pdf( s.bottom, ri, w, nm, outside );
				}
				if( pWalkedOnly ) {
					*pWalkedOnly = walked;
				}
				Scalar p = walked;
				// The DIRECT class's non-delta rays are emitted as such only
				// when the top has a BSDF to price them; otherwise they are
				// delta-tagged (see ScatterImpl) and carry no density here.
				if( W.w1 > 0 && pr.Qup > 0 && s.pTopBSDF ) {
					p += W.w1 * P::Pdf( s.top, ri, w, nm, outside ) / pr.Qup;
				}
				return p;
			}

			// -----------------------------------------------------------
			//  The COVERED-class evaluator: position-free MC layered BSDF
			//  (Guo et al. 2018 / PBRT-v4 LayeredBxDF::f), deterministic via
			//  its hash-seeded stream.  Returns f_walked(wi -> wOut)
			//  WITHOUT the outgoing cosine.
			//
			//  (a) At every bottom visit, CONNECT to wOut through the top's
			//      DELTA refraction: the internal direction ux that refracts
			//      to wOut is fixed by Snell, so the term is
			//          beta * f_bottom(ux) * W(ux) * Tr(ux) / eta^2
			//      with W the top's delta up-going kray from inside along ux
			//      (FORWARD evaluation, so a layer's own interior absorption
			//      -- DielectricSPF's tau^distance -- is included exactly as
			//      the walk applies it) and 1/eta^2 the solid-angle
			//      compression of the exit: n_out^2 cos dw_out ==
			//      n_gap^2 cos dw_in, i.e. integrating (a) against
			//      cos dw_out reproduces the walk's own bottom-then-exit
			//      step term for term.
			//  (b) At every top visit from below, the top's own BSDF for its
			//      NON-DELTA exit lobes.
			//  The walk continues by CONDITIONAL selection (reflections only
			//  at each layer); exits are accounted by (a)/(b), never
			//  sampled.  A delta-tagged top is priced as an IDEAL
			//  refraction by (a): DielectricSPF's finite-`scattering`
			//  transmission warp, which it itself tags delta, is not
			//  reproduced on the exit crossing (see the DL-24 doc).
			// -----------------------------------------------------------
			// -----------------------------------------------------------
			//  THE EVALUATOR'S WALK, built once per shading point.
			//
			//  The walk itself -- the entry transmission, every bottom
			//  and top visit, the throughputs -- does not depend on the
			//  exit direction: only the two CONNECTION terms do (term (a)
			//  through the top's delta transmission at the inverse-Snell
			//  direction of wOut, term (b) the top's BSDF at wOut).  So the
			//  walk is seeded from (wi, position) alone and recorded, and
			//  every exit query at the same shading point -- the emitted
			//  ray's own value, every NEE sample, every BDPT connection --
			//  re-evaluates only the connections against it (DL-24 review
			//  P2-3; +40 % render cost before).  Term (a)'s top transmission
			//  at ux is drawn from its own stream seeded by (wi, wOut,
			//  position), so `value` stays a deterministic function of its
			//  arguments.  Sharing one walk across a vertex's exits
			//  CORRELATES those estimates; each is still unbiased.
			// -----------------------------------------------------------
			template<class P>
			struct WalkPath
			{
				typedef typename P::T T;
				bool                   entered;
				IORStack               gap0;			//!< gap stack after the entry transmission (term (a)'s eta and top scatter)
				std::vector<T>         betaBot;		//!< throughput ARRIVING at each bottom visit
				std::vector<Vector3>   wBot;
				std::vector<Scalar>    LBot;
				std::vector<T>         betaTop;		//!< throughput ARRIVING at each top-underside visit
				std::vector<Vector3>   wTop;
				std::vector<Scalar>    LTop;
				std::vector<IORStack>  gapTop;		//!< gap stack the top sees at that visit
				WalkPath() : entered( false ), gap0( Scalar( 1 ) ) {}
				void Clear()
				{
					entered = false;
					betaBot.clear(); wBot.clear(); LBot.clear();
					betaTop.clear(); wTop.clear(); LTop.clear(); gapTop.clear();
				}
			};

			template<class P>
			static void BuildWalk(
				const CompositeSPF& s,
				const RayIntersectionGeometric& ri,
				const IORStack& outside,
				const Scalar nm,
				WalkPath<P>& path
				)
			{
				typedef typename P::T T;
				path.Clear();
				const Vector3 n = ri.onb.w();
				auto isUpBottom = [&n]( const ScatteredRay& r ) { return Vector3Ops::Dot( r.ray.Dir(), n ) > 0; };
				auto isDownTop  = [&n]( const ScatteredRay& r ) { return Vector3Ops::Dot( r.ray.Dir(), n ) < 0; };

				HashedSampler hs( HashPoint( HashVector( kSaltEvaluate, ri.ray.Dir() ), ri.ptIntersection ) );

				// Entry transmission.
				ScatteredRayContainer c0;
				P::Scatter( s.top, ri, hs, nm, c0, outside );
				Scalar q = 0;
				int k = SelectSubset<P>( c0, isDownTop, hs.Get1D(), q );
				if( k < 0 ) {
					return;
				}
				T beta = P::Scaled( P::Kray( c0[k] ), Scalar( 1 ) / q );
				Vector3 w = Vector3Ops::Normalize( c0[k].ray.Dir() );
				IORStack gap( c0[k].ior_stack ? *c0[k].ior_stack : outside );
				if( !Roulette<P>( s, beta, c0[k].type, 0, hs ) ) {
					return;
				}
				path.entered = true;
				path.gap0 = gap;

				// ONE record, re-aimed at every event (SetLayerRay).
				RayIntersectionGeometric rec( ri );

				unsigned int steps = 1;
				for( unsigned int ev = 0; ev < CompositeSPF::kMaxWalkEvents; ev += 2 )
				{
					// Down across the gap to the bottom.
					const Scalar Ld = CompositeSPF::GapPathLength( w, n, s.thickness );
					beta = P::Mul( beta, P::GapAtt( s.extinction, ri, nm, Ld ) );
					if( !( P::MaxOf( beta ) > 0 ) ) {
						return;
					}
					path.betaBot.push_back( beta );
					path.wBot.push_back( w );
					path.LBot.push_back( Ld );
					SetLayerRay( rec, ri, w, Ld );
					ScatteredRayContainer cb;
					P::Scatter( s.bottom, rec, hs, nm, cb, outside );
					k = SelectCarried<P>( cb, isUpBottom, beta, hs.Get1D(), q );
					if( k < 0 ) {
						return;
					}
					beta = P::Mul( beta, P::Scaled( P::Kray( cb[k] ), Scalar( 1 ) / q ) );
					w = Vector3Ops::Normalize( cb[k].ray.Dir() );
					if( !Roulette<P>( s, beta, cb[k].type, steps, hs ) ) {
						return;
					}
					steps++;

					// Up across the gap to the top's underside.
					const Scalar Lu = CompositeSPF::GapPathLength( w, n, s.thickness );
					beta = P::Mul( beta, P::GapAtt( s.extinction, ri, nm, Lu ) );
					if( !( P::MaxOf( beta ) > 0 ) ) {
						return;
					}
					path.betaTop.push_back( beta );
					path.wTop.push_back( w );
					path.LTop.push_back( Lu );
					path.gapTop.push_back( gap );
					SetLayerRay( rec, ri, w, Lu );
					ScatteredRayContainer ct;
					P::Scatter( s.top, rec, hs, nm, ct, gap );
					k = SelectCarried<P>( ct, isDownTop, beta, hs.Get1D(), q );
					if( k < 0 ) {
						return;
					}
					beta = P::Mul( beta, P::Scaled( P::Kray( ct[k] ), Scalar( 1 ) / q ) );
					w = Vector3Ops::Normalize( ct[k].ray.Dir() );
					if( ct[k].ior_stack ) {
						gap = *ct[k].ior_stack;
					}
					if( !Roulette<P>( s, beta, ct[k].type, steps, hs ) ) {
						return;
					}
					steps++;
				}
				if( P::MaxOf( beta ) > 0 ) {
					NoteCompositeWalkCapReached();
				}
			}

			//! The connection terms against a recorded walk.
			template<class P>
			static typename P::T EvaluateFromWalk(
				const CompositeSPF& s,
				const RayIntersectionGeometric& ri,
				const Vector3& wOut,
				const IORStack& outside,
				const Scalar nm,
				const WalkPath<P>& path
				)
			{
				typedef typename P::T T;
				T f = P::Zero();
				if( !path.entered ) {
					return f;
				}
				const Vector3 n = ri.onb.w();
				RayIntersectionGeometric rec( ri );

				// Term (a): every bottom visit connects to wOut through the
				// top's delta refraction at ux.
				if( s.pBottomBSDF && !path.betaBot.empty() ) {
					const Scalar nOut = outside.top();
					const Scalar nGap = path.gap0.top();
					const Scalar eta = ( nOut > 0 && nGap > 0 ) ? ( nGap / nOut ) : Scalar( 1 );
					Vector3 ux( 0, 0, 1 );
					if( InternalDirectionForExit( wOut, n, eta, ux ) ) {
						const Scalar Lx = CompositeSPF::GapPathLength( ux, n, s.thickness );
						SetLayerRay( rec, ri, ux, Lx );
						HashedSampler hx( HashPoint( HashVector( HashVector( kSaltEvaluateExit, ri.ray.Dir() ), wOut ), ri.ptIntersection ) );
						ScatteredRayContainer cx;
						P::Scatter( s.top, rec, hx, nm, cx, path.gap0 );
						T W = P::Zero();
						for( unsigned int i = 0; i < cx.Count(); i++ ) {
							if( cx[i].isDelta && Vector3Ops::Dot( cx[i].ray.Dir(), n ) >= 0 ) {
								W = W + P::Kray( cx[i] );
							}
						}
						if( P::MaxOf( W ) > 0 ) {
							const T aFactor = P::Scaled( P::Mul( W, P::GapAtt( s.extinction, ri, nm, Lx ) ), Scalar( 1 ) / ( eta * eta ) );
							for( size_t i = 0; i < path.betaBot.size(); i++ ) {
								SetLayerRay( rec, ri, path.wBot[i], path.LBot[i] );
								f = f + P::Mul( P::Mul( path.betaBot[i], P::Value( *s.pBottomBSDF, ux, rec, nm, &outside ) ), aFactor );
							}
						}
					}
				}

				// Term (b): every top-underside visit exits through the top's
				// own BSDF.
				if( s.pTopBSDF ) {
					for( size_t j = 0; j < path.betaTop.size(); j++ ) {
						SetLayerRay( rec, ri, path.wTop[j], path.LTop[j] );
						f = f + P::Mul( path.betaTop[j], P::Value( *s.pTopBSDF, wOut, rec, nm, &path.gapTop[j] ) );
					}
				}
				return f;
			}

			//! Per-thread cache of recorded walks, keyed exactly like the
			//! probe cache.  Only the OUTERMOST composite evaluation on a
			//! thread uses it: a nested composite's evaluation happens
			//! INSIDE the outer one's walk build / connection loop, and
			//! letting it touch the cache could evict the entry the outer
			//! evaluation is still reading.  Nested evaluations build a
			//! local walk instead.
			static thread_local int tEvaluateDepth;

			struct EvaluateDepthGuard
			{
				EvaluateDepthGuard() { ++tEvaluateDepth; }
				~EvaluateDepthGuard() { --tEvaluateDepth; }
			};

			template<class P>
			static typename P::T EvaluateWalked(
				const CompositeSPF& s,
				const RayIntersectionGeometric& ri,
				const Vector3& wOut,
				const IORStack& outside,
				const Scalar nm
				)
			{
				struct Entry
				{
					bool               valid;
					unsigned long long id;
					Scalar             nm;
					Scalar             key[kProbeKeySize];
					WalkPath<P>        path;
					Entry() : valid( false ), id( 0 ), nm( 0 ) {}
				};
				static const int kWalkCacheSize = 4;
				thread_local Entry cache[kWalkCacheSize];
				thread_local int next = 0;

				const bool outermost = ( tEvaluateDepth == 0 );
				EvaluateDepthGuard guard;
				if( !outermost ) {
					WalkPath<P> local;
					BuildWalk<P>( s, ri, outside, nm, local );
					return EvaluateFromWalk<P>( s, ri, wOut, outside, nm, local );
				}

				Scalar key[kProbeKeySize];
				ProbeKey( ri, outside, key );
				for( int i = 0; i < kWalkCacheSize; i++ ) {
					const Entry& e = cache[i];
					if( e.valid && e.id == s.instanceId && e.nm == nm &&
						std::memcmp( e.key, key, sizeof( key ) ) == 0 ) {
						return EvaluateFromWalk<P>( s, ri, wOut, outside, nm, e.path );
					}
				}
				Entry& e = cache[next];
				next = ( next + 1 ) % kWalkCacheSize;
				e.valid = false;
				BuildWalk<P>( s, ri, outside, nm, e.path );
				e.id = s.instanceId;
				e.nm = nm;
				std::memcpy( e.key, key, sizeof( key ) );
				e.valid = true;
				return EvaluateFromWalk<P>( s, ri, wOut, outside, nm, e.path );
			}

			//! Direct + covered non-delta response at `vLightIn`.
			template<class P>
			static void EvaluateLayeredParts(
				const CompositeSPF& s,
				const Vector3& vLightIn,
				const RayIntersectionGeometric& ri,
				const IORStack* pStack,
				const Scalar nm,
				typename P::T& direct,
				typename P::T& walked
				)
			{
				direct = P::Zero();
				walked = P::Zero();
				if( !EntryFromTop( ri ) ) {
					return;
				}
				const Vector3 w = Vector3Ops::Normalize( vLightIn );
				if( !( Vector3Ops::Dot( w, ri.onb.w() ) > 0 ) ) {
					return;
				}
				if( s.pTopBSDF ) {
					direct = P::Value( *s.pTopBSDF, vLightIn, ri, nm, pStack );
				}
				if( !s.HasLayeredValue() || !PassesGeomGate( ri, w ) ) {
					return;
				}
				if( pStack ) {
					walked = EvaluateWalked<P>( s, ri, w, *pStack, nm );
				} else {
					IORStack fabricated( ri.ambientIOR > 0 ? ri.ambientIOR : Scalar( 1 ) );
					fabricated.SetCurrentObject( StacklessKey() );
					walked = EvaluateWalked<P>( s, ri, w, fabricated, nm );
				}
			}

			// -----------------------------------------------------------
			//  The WALKER: an unbiased single-path random walk under the
			//  natural selection rule, compensated Russian roulette past
			//  the budgets.  From the top it emits only exits the
			//  evaluator does NOT price; from below it emits every exit.
			//  Every emission is delta-tagged (no NEE / connection prices
			//  it, so it carries no MIS partner).
			// -----------------------------------------------------------
			static void Emit( ScatteredRayContainer& scattered, ScatteredRay& r )
			{
				if( !scattered.AddScatteredRay( r ) ) {
					NoteCompositeExitRayDropped();
				}
			}

			template<class P>
			static void Walker(
				const CompositeSPF& s,
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				const Scalar nm,
				ScatteredRayContainer& scattered,
				const IORStack& outside,
				const Scalar weightScale,
				const bool fromTop
				)
			{
				typedef typename P::T T;
				const Vector3 n = ri.onb.w();
				IORStack gap( outside );
				T beta = P::Scaled( P::One(), weightScale );
				unsigned int steps = 0;
				bool atBottom = true;
				bool lastBottomEvaluable = false;
				RayIntersectionGeometric cur( ri );

				if( fromTop ) {
					auto isDownTop = [&n]( const ScatteredRay& r ) { return Vector3Ops::Dot( r.ray.Dir(), n ) < 0; };
					ScatteredRayContainer c0;
					P::Scatter( s.top, ri, sampler, nm, c0, outside );
					Scalar q = 0;
					const int k = SelectSubset<P>( c0, isDownTop, sampler.Get1D(), q );
					if( k < 0 ) {
						return;
					}
					beta = P::Mul( beta, P::Scaled( P::Kray( c0[k] ), Scalar( 1 ) / q ) );
					const Vector3 w = Vector3Ops::Normalize( c0[k].ray.Dir() );
					if( c0[k].ior_stack ) {
						gap = *c0[k].ior_stack;
					}
					if( !Roulette<P>( s, beta, c0[k].type, 0, sampler ) ) {
						return;
					}
					steps = 1;
					const Scalar L = CompositeSPF::GapPathLength( w, n, s.thickness );
					beta = P::Mul( beta, P::GapAtt( s.extinction, ri, nm, L ) );
					SetLayerRay( cur, ri, w, L );
				}

				for( unsigned int ev = 0; ev < CompositeSPF::kMaxWalkEvents; ev++ )
				{
					if( !( P::MaxOf( beta ) > 0 ) ) {
						return;
					}
					ScatteredRayContainer c;
					P::Scatter( atBottom ? s.bottom : s.top, cur, sampler, nm, c, CompositeSPF::EvalStack( cur, outside, gap ) );
					Scalar q = 0;
					auto any = []( const ScatteredRay& ) { return true; };
					const int kSel = SelectCarried<P>( c, any, beta, sampler.Get1D(), q );
					if( kSel < 0 || !( q > 0 ) ) {
						return;
					}
					ScatteredRay* r = &c[kSel];
					const Scalar cosN = Vector3Ops::Dot( r->ray.Dir(), n );

					if( atBottom ) {
						if( cosN <= 0 ) {
							// Out through the bottom: never priced by the
							// evaluator.
							P::SetKray( *r, P::Mul( beta, P::Scaled( P::Kray( *r ), Scalar( 1 ) / q ) ) );
							r->isDelta = true;
							Emit( scattered, *r );
							return;
						}
						lastBottomEvaluable = ( s.pBottomBSDF != 0 ) && !r->isDelta;
					} else {
						if( cosN >= 0 ) {
							// Out through the top.  Covered iff the evaluator
							// prices it: term (b) (non-delta exit, top has a
							// BSDF) or term (a) (delta exit right after a
							// non-delta bottom event with a BSDF).
							const bool covered = fromTop && (
								( !r->isDelta && s.pTopBSDF ) ||
								( r->isDelta && lastBottomEvaluable ) );
							if( !covered ) {
								P::SetKray( *r, P::Mul( beta, P::Scaled( P::Kray( *r ), Scalar( 1 ) / q ) ) );
								r->isDelta = true;
								Emit( scattered, *r );
							}
							return;
						}
						if( r->ior_stack ) {
							gap = *r->ior_stack;
						}
					}

					beta = P::Mul( beta, P::Scaled( P::Kray( *r ), Scalar( 1 ) / q ) );
					const Vector3 w = Vector3Ops::Normalize( r->ray.Dir() );
					if( !Roulette<P>( s, beta, r->type, steps, sampler ) ) {
						return;
					}
					steps++;
					const Scalar L = CompositeSPF::GapPathLength( w, n, s.thickness );
					beta = P::Mul( beta, P::GapAtt( s.extinction, ri, nm, L ) );
					SetLayerRay( cur, ri, w, L );
					atBottom = !atBottom;
				}
				if( P::MaxOf( beta ) > 0 ) {
					NoteCompositeWalkCapReached();
				}
			}

			//! Emits one COVERED / DIRECT-non-delta ray along `w`.
			template<class P>
			static void EmitNonDelta(
				const CompositeSPF& s,
				const RayIntersectionGeometric& ri,
				const Scalar nm,
				ScatteredRayContainer& scattered,
				const IORStack& outside,
				const Probe& pr,
				const Weights& W,
				const Vector3& w,
				const ScatteredRay::ScatRayType type,
				const bool walkedOnly
				)
			{
				typedef typename P::T T;
				Scalar walkedPdf = 0;
				const Scalar pdf = MixturePdf<P>( s, ri, w, outside, nm, pr, W, &walkedPdf );
				T direct, walked;
				EvaluateLayeredParts<P>( s, w, ri, &outside, nm, direct, walked );
				const Scalar cosO = fabs( Vector3Ops::Dot( w, ri.vNormal ) );

				ScatteredRay out;
				out.type = type;
				out.isDelta = false;
				out.ray.Set( ri.ptIntersection, w );
				if( walkedOnly ) {
					if( !( walkedPdf > 0 ) ) {
						return;
					}
					out.pdf = walkedPdf;
					P::SetKray( out, P::Scaled( walked, cosO / walkedPdf ) );
				} else {
					if( !( pdf > 0 ) ) {
						return;
					}
					out.pdf = pdf;
					P::SetKray( out, P::Scaled( direct + walked, cosO / pdf ) );
				}
				Emit( scattered, out );
			}

			template<class P>
			static void ScatterImpl(
				const CompositeSPF& s,
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				const Scalar nm,
				ScatteredRayContainer& scattered,
				const IORStack& ior_stack
				)
			{
				// Both stacks start at the stack Scatter() was entered with:
				// nothing has crossed the top interface yet, so the gap medium
				// is still the outer one.
				if( !EntryFromTop( ri ) ) {
					Walker<P>( s, ri, sampler, nm, scattered, ior_stack, Scalar( 1 ), false );
				} else {
					const Probe pr = DoProbe<P>( s, ri, ior_stack, nm );
					bool any = false;
					const Weights W = NormalizedWeights( s, pr, any );
					if( !any ) {
						return;
					}
					const Vector3 n = ri.onb.w();
					const Scalar u = sampler.Get1D();

					if( u < W.w1 ) {
						auto isUp = [&n]( const ScatteredRay& r ) { return Vector3Ops::Dot( r.ray.Dir(), n ) >= 0; };
						ScatteredRayContainer c;
						P::Scatter( s.top, ri, sampler, nm, c, ior_stack );
						Scalar q = 0;
						const int k = SelectSubset<P>( c, isUp, sampler.Get1D(), q );
						if( k >= 0 ) {
							ScatteredRay& r = c[k];
							if( r.isDelta || !W.aggregate || !s.pTopBSDF ) {
								// Priced by the top lobe's own estimator.  A
								// NON-delta lobe of a top with no BSDF (a
								// skin model, or an SPF-only composite built
								// without its layers' BSDFs) cannot be priced
								// by NEE either, so it is delta-tagged: no
								// technique partners it, exactly like the
								// WALKER class.
								P::SetKray( r, P::Scaled( P::Kray( r ), Scalar( 1 ) / ( q * W.w1 ) ) );
								if( !s.pTopBSDF ) {
									r.isDelta = true;
								}
								Emit( scattered, r );
							} else {
								EmitNonDelta<P>( s, ri, nm, scattered, ior_stack, pr, W,
									Vector3Ops::Normalize( r.ray.Dir() ), r.type, false );
							}
						}
					} else if( u < W.w1 + W.w2 ) {
						const Vector3 w = GeometricUtilities::CreateDiffuseVector( ri.onb, sampler.Get2D() );
						if( PassesGeomGate( ri, w ) ) {
							EmitNonDelta<P>( s, ri, nm, scattered, ior_stack, pr, W,
								w, ScatteredRay::eRayDiffuse, !W.aggregate );
						}
					} else if( u < W.w1 + W.w2 + W.w3 ) {
						ScatteredRayContainer c;
						P::Scatter( s.bottom, ri, sampler, nm, c, ior_stack );
						Scalar q = 0;
						const ScatteredRay* r = c.RandomlySelect( sampler.Get1D(), P::kNM, &q );
						if( r && !r->isDelta && Vector3Ops::Dot( r->ray.Dir(), n ) > 0 ) {
							EmitNonDelta<P>( s, ri, nm, scattered, ior_stack, pr, W,
								Vector3Ops::Normalize( r->ray.Dir() ), r->type, !W.aggregate );
						}
					} else {
						if( W.w4 > 0 ) {
							Walker<P>( s, ri, sampler, nm, scattered, ior_stack, Scalar( 1 ) / W.w4, true );
						}
					}
				}

				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					// To account for thicknesses
					scattered[i].ray.origin = ri.ptIntersection;
				}
			}

			template<class P>
			static Scalar PdfImpl(
				const CompositeSPF& s,
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const Scalar nm,
				const IORStack& ior_stack
				)
			{
				if( !EntryFromTop( ri ) ) {
					return 0;
				}
				const Vector3 w = Vector3Ops::Normalize( wo );
				if( !( Vector3Ops::Dot( w, ri.onb.w() ) > 0 ) ) {
					return 0;
				}
				const Probe pr = DoProbe<P>( s, ri, ior_stack, nm );
				bool any = false;
				const Weights W = NormalizedWeights( s, pr, any );
				if( !any ) {
					return 0;
				}
				return MixturePdf<P>( s, ri, w, ior_stack, nm, pr, W, 0 );
			}
		};

		thread_local int CompositeSPFImpl::tEvaluateDepth = 0;
		const Scalar CompositeSPFImpl::kShareFloor            = Scalar( 0.02 );
		const Scalar CompositeSPFImpl::kWalkerShareIfPossible = Scalar( 0.5 );
		const Scalar CompositeSPFImpl::kWalkerShareFloor      = Scalar( 0.05 );
	}
}

void CompositeSPF::Scatter(
			const RayIntersectionGeometric& ri,
			ISampler& sampler,
			ScatteredRayContainer& scattered,
			const IORStack& ior_stack
			) const
{
	CompositeSPFImpl::ScatterImpl<CompositeSPFImpl::PipeRGB>( *this, ri, sampler, Scalar( -1 ), scattered, ior_stack );
}

void CompositeSPF::ScatterNM(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const Scalar nm,
	ScatteredRayContainer& scattered,
	const IORStack& ior_stack
	) const
{
	CompositeSPFImpl::ScatterImpl<CompositeSPFImpl::PipeNM>( *this, ri, sampler, nm, scattered, ior_stack );
}

Scalar CompositeSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	return CompositeSPFImpl::PdfImpl<CompositeSPFImpl::PipeRGB>( *this, ri, wo, Scalar( -1 ), ior_stack );
}

Scalar CompositeSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	return CompositeSPFImpl::PdfImpl<CompositeSPFImpl::PipeNM>( *this, ri, wo, nm, ior_stack );
}

RISEPel CompositeSPF::EvaluateLayered(
	const Vector3& vLightIn,
	const RayIntersectionGeometric& ri,
	const IORStack* pStack
	) const
{
	RISEPel direct, walked;
	CompositeSPFImpl::EvaluateLayeredParts<CompositeSPFImpl::PipeRGB>( *this, vLightIn, ri, pStack, Scalar( -1 ), direct, walked );
	return direct + walked;
}

Scalar CompositeSPF::EvaluateLayeredNM(
	const Vector3& vLightIn,
	const RayIntersectionGeometric& ri,
	const Scalar nm,
	const IORStack* pStack
	) const
{
	Scalar direct = 0, walked = 0;
	CompositeSPFImpl::EvaluateLayeredParts<CompositeSPFImpl::PipeNM>( *this, vLightIn, ri, pStack, nm, direct, walked );
	return direct + walked;
}

Scalar CompositeSPF::EvaluateLobeFNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType /*rayType*/,
	Scalar nm,
	const IORStack& ior_stack
	) const
{
	// Every non-delta emission in AGGREGATE mode is priced
	// value(dir) * cos / Pdf(dir) with the deterministic layered value, so
	// the companion weight is exactly valueNM(dir; nm) * cos / pdfHero --
	// the ISPF default's 6-argument EvaluateKrayNM supplies the cos and the
	// division.  PER-BRANCH mode (a top whose selection is itself random)
	// prices each branch's own class instead, which this cannot recover.
	if( !CompositeSPFImpl::EntryFromTop( ri ) ) {
		return -1;
	}
	const CompositeSPFImpl::Probe pr = CompositeSPFImpl::DoProbe<CompositeSPFImpl::PipeNM>( *this, ri, ior_stack, nm );
	if( !pr.det ) {
		return -1;
	}
	return EvaluateLayeredNM( outDir, ri, nm, &ior_stack );
}

Scalar CompositeSPF::EvaluateKrayNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& ior_stack
	) const
{
	// Reached for DELTA rays (the HWSS ladders pass pdfHero = -1 for them).
	// A DIRECT delta ray is the top's own delta up-going REFLECTION,
	// emitted with kray_r / q (q its natural selection probability, since
	// w1 == Qup in AGGREGATE mode).  Reconstruct it from the top at `nm`,
	// using the selection probability at `nm` too -- exact whenever the
	// top's lobe selection is wavelength-independent (a non-dispersive
	// dielectric: F / F).  For a DISPERSIVE top the emitted weight is
	// kray_hero / q_hero and the right companion weight is
	// kray_nm / q_hero, which this cannot form (the hero wavelength is not
	// an argument), so a dispersive top is approximate here (DL-221).
	//
	// DL-24 review P1-2: a WALKER ray must decline, and its DIRECTION
	// cannot tell it apart -- a composite is a parallel slab, so every
	// all-delta walker path that leaves through the top exits exactly
	// along the mirror direction of the entry.  Its TYPE can: a walker
	// ray leaves the top from INSIDE the stack, i.e. it arrived at the
	// top travelling upward, and a reflection returns a ray to the side it
	// came from, so an up-going ray out of the top from inside is a
	// TRANSMISSION, never a reflection.  The DIRECT delta ray is the only
	// up-going eRayReflection this SPF emits in AGGREGATE mode, so only
	// that type, matched against a top lobe of the same type, is
	// reconstructed.  PER-BRANCH mode prices the direct ray as
	// kray_r / (q * w1) with a floored w1 that is not Qup, so it declines
	// outright.
	if( rayType != ScatteredRay::eRayReflection ) {
		return -1;
	}
	if( !CompositeSPFImpl::EntryFromTop( ri ) ) {
		return -1;
	}
	const Vector3 n = ri.onb.w();
	const Vector3 d = Vector3Ops::Normalize( outDir );
	if( !( Vector3Ops::Dot( d, n ) >= 0 ) ) {
		return -1;
	}
	{
		const CompositeSPFImpl::Probe pr = CompositeSPFImpl::DoProbe<CompositeSPFImpl::PipeNM>( *this, ri, ior_stack, nm );
		if( !pr.det ) {
			return -1;
		}
	}

	const uint64_t seed = CompositeSPFImpl::HashPoint(
		CompositeSPFImpl::HashVector( 0x243F6A8885A308D3ull, ri.ray.Dir() ), ri.ptIntersection );
	ScatteredRayContainer c;
	{
		CompositeSPFImpl::HashedSampler hs( seed ^ CompositeSPFImpl::kSaltProbeA );
		top.ScatterNM( ri, hs, nm, c, ior_stack );
	}
	const unsigned int count = c.Count();
	Scalar total = 0;
	for( unsigned int i = 0; i < count; i++ ) {
		total += c[i].krayNM;
	}
	Scalar result = -1;
	for( unsigned int i = 0; i < count; i++ ) {
		if( !c[i].isDelta || c[i].type != ScatteredRay::eRayReflection ) {
			continue;
		}
		const Vector3 ci = Vector3Ops::Normalize( c[i].ray.Dir() );
		if( Scalar( 1 ) - Vector3Ops::Dot( ci, d ) > Scalar( 1e-9 ) ) {
			continue;
		}
		const Scalar q = ( count == 1 ) ? Scalar( 1 ) : ( ( total > NEARZERO ) ? c[i].krayNM / total : Scalar( 0 ) );
		if( q > 0 ) {
			result = ( result < 0 ? Scalar( 0 ) : result ) + c[i].krayNM / q;
		}
	}
	return result;
}

//////////////////////////////////////////////////////////////////////
//  CompositeBSDF
//////////////////////////////////////////////////////////////////////

CompositeBSDF::CompositeBSDF( const CompositeSPF& spf_, const IBSDF* pAlbedoSource_ ) :
  spf( spf_ ),
  pAlbedoSource( pAlbedoSource_ )
{
	spf.addref();
	if( pAlbedoSource ) {
		pAlbedoSource->addref();
	}
}

CompositeBSDF::~CompositeBSDF()
{
	spf.release();
	if( pAlbedoSource ) {
		pAlbedoSource->release();
	}
}

RISEPel CompositeBSDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	return spf.EvaluateLayered( vLightIn, ri, 0 );
}

Scalar CompositeBSDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	return spf.EvaluateLayeredNM( vLightIn, ri, nm, 0 );
}

RISEPel CompositeBSDF::valueStateful( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const IORStack* pIORStack ) const
{
	return spf.EvaluateLayered( vLightIn, ri, pIORStack );
}

Scalar CompositeBSDF::valueStatefulNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm, const IORStack* pIORStack ) const
{
	return spf.EvaluateLayeredNM( vLightIn, ri, nm, pIORStack );
}

RISEPel CompositeBSDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// OIDN albedo AOV only: the layer whose BSDF the composite used to
	// present (top, else bottom), as before DL-24.
	return pAlbedoSource ? pAlbedoSource->albedo( ri ) : RISEPel( 1, 1, 1 );
}
