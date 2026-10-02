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
#include <optional>
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
//  THE STACK CONVENTION (DL-341, 2026-10-02)
//
//  A composite is ONE IObject with TWO interfaces, and IORStack keys its
//  entries on `pCurrentObject` -- one pointer for the whole material.  The
//  convention is DERIVED from what the equivalent pair of SEPARATE surfaces
//  would do: two coincident interfaces with the same orientation (both
//  fronts on the shading-normal side), the top's material above the bottom's.
//  Under RISE's stack rules (a closed solid; or two open sheets under the
//  DL-345 face rule, which reads the same) a ray that crosses both from above
//  is INSIDE both, in the medium the bottom layer defines; one that leaves
//  upward through both is back outside.  So, for an object O:
//
//    OUT    the stack without O's entry -- the medium above the top
//           (OutsideOf: the entry stack with O popped if it held it);
//    GAP    OUT plus what the TOP pushed crossing down (O at the gap's
//           index; nothing for a top that does not push);
//    BELOW  GAP plus what the BOTTOM pushed crossing down.
//
//  The shared key is what used to make one threaded stack unworkable (the
//  top's push read as the bottom's, so a stack-sensitive bottom took its
//  from-inside branch -- the two failure modes the pre-DL-341 two-stack
//  walk was built around, guarded by CompositeExtinctionTest section 6).
//  The walk now gives the BOTTOM layer its own key, `BottomKey(s)` (an
//  address inside this composite instance, never dereferenced), and the top
//  keeps O.  With two keys ONE internal stack is threaded through the whole
//  walk and refreshed after EVERY crossing in either direction: the top sees
//  "inside" exactly when the ray is in the gap or below, the bottom exactly
//  when the ray is below, and each refracts from the index of the medium the
//  ray is actually in (the bottom now refracts from the GAP's index, not the
//  outside one -- the pre-DL-341 "scope gap (b)": a glass/glass stack
//  refracted 1.0 -> 1.5 twice).
//
//  ENTRY SIDE is the GEOMETRIC one, in the composite's own frame (the
//  geometric normal oriented into the shading normal's hemisphere, so a
//  double-sided mesh that flips both normals together keeps its old frame):
//    * from above, agreeing with the shading normal -- the DIRECT / COVERED /
//      WALKER mixture, starting at OUT;
//    * from above but BEHIND a tilted shading normal (`d . n_s > 0`) -- a
//      natural walk that starts at the TOP, every exit delta-tagged (the
//      layered evaluator does not price it, so `value` and `Pdf` are 0
//      there);
//    * from below -- a natural walk that starts at the BOTTOM from BELOW:
//      OUT, plus the GAP entry the top would push (read off one hashed
//      from-above Scatter of the top, GapStackForBelow), plus the bottom's
//      key at the index of the medium the ray is in (the entry stack's top).
//  Every emitted ray carries the EXTERNAL form of the stack it ends in
//  (ToExternal): OUT for an exit up through the top, OUT plus O at the
//  BELOW medium's index for an exit down through the bottom -- so a
//  radiance consumer's eta^2 factor (RadianceEtaScale) sees the medium the
//  ray really entered, and a ray that leaves upward from inside pops O.
//
//  What this convention says about a TRANSMITTING composite on an OPEN
//  sheet (CompositeEnergyConservationTest D3): the ray that crossed both
//  layers is inside the bottom's medium, exactly as below a single open
//  `dielectric_material` sheet, so a glass/glass composite renders like the
//  pair of separate glass sheets it stands for (0.467 in a white env furnace
//  at normal incidence, F + (1 - F) / eta^2) -- not 1.  A white environment
//  of radiance 1 seen inside a medium of index 1.5 is not an equilibrium
//  (that would be n^2 = 2.25), so "truth 1" was never the expectation there.
// ---------------------------------------------------------------------------


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
			//  what makes an emitted covered ray's kray equal
			//  value(dir) * cos / Pdf(dir) (up to rounding), and what makes
			//  its HWSS companion weight reconstructible from (ri, dir, nm)
			//  in the aggregate mode.  The WALK is driven by a PCG32 stream
			//  seeded from a hash of (incoming direction, position) ONLY --
			//  it is recorded once per shading point and shared by every
			//  exit query there (see WalkPath below) -- and term (a)'s top
			//  transmission draws from a second stream seeded by (incoming,
			//  outgoing, position).  PBRT-v4's LayeredBxDF::f seeds from
			//  Hash(wo), Hash(wi); the POSITION is added here so a flat
			//  surface under an orthographic view and a directional light
			//  does not reuse one estimate at every pixel.  The wavelength is deliberately NOT
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
			static const uint64_t kSaltGapBelow = 0x5A17C0DE00000006ull;
			static const int      kGapBelowDraws    = 16;
			static const int      kPerBranchProbes  = 8;

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

			//! DL-341: the side the ray arrives from is the GEOMETRIC one,
			//! read in the composite's own frame -- the geometric normal
			//! oriented into the shading normal's hemisphere, so a
			//! double-sided mesh (which flips both normals together) keeps
			//! the frame it always had, and only the wedge between a TILTED
			//! shading normal and the true surface changes classification.
			//! Hair's ray-derived normal carries no side: the shading normal
			//! stands in.
			static inline bool EnteredFromAbove( const RayIntersectionGeometric& ri )
			{
				const Vector3 n = ri.onb.w();
				Vector3 g = ri.HasTrueGeomSide() ? ri.vGeomNormal : n;
				if( !( Vector3Ops::SquaredModulus( g ) > Scalar( 1e-12 ) ) ) {
					g = n;
				}
				if( Vector3Ops::Dot( g, n ) < 0 ) {
					g = -g;
				}
				return Vector3Ops::Dot( ri.ray.Dir(), g ) <= 0;
			}

			//! The entries the DIRECT / COVERED / WALKER mixture (and so the
			//! layered evaluator and `Pdf`) handles: from above AND against
			//! the shading normal.  Everything else is walked naturally and
			//! delta-tagged.
			static inline bool CoveredEntry( const RayIntersectionGeometric& ri )
			{
				return EnteredFromAbove( ri ) && Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) <= 0;
			}

			// -----------------------------------------------------------
			//  DL-341 stack helpers -- see "THE STACK CONVENTION" above.
			// -----------------------------------------------------------

			//! The bottom layer's own key: an address inside THIS composite
			//! instance (so a nested composite's key never collides with
			//! its parent's), compared by IORStack and never dereferenced.
			static inline const IObject* BottomKey( const CompositeSPF& s )
			{
				return reinterpret_cast<const IObject*>( &s.instanceId );
			}

			static inline IORStack Keyed( const IORStack& st, const IObject* key )
			{
				IORStack c( st );
				c.SetCurrentObject( key );
				return c;
			}

			//! A stack handed back to the TOP layer: re-keyed to O (a ray
			//! the bottom reflected carries the bottom's key).
			static inline IORStack KeyedTop( const IORStack& st, const IORStack& out )
			{
				IORStack c( st );
				if( out.currentObject() ) {
					c.SetCurrentObject( out.currentObject() );
				}
				return c;
			}

			//! OUT: the entry stack without this object's entry.
			static inline IORStack OutsideOf( const IORStack& S )
			{
				IORStack out( S );
				if( S.currentObject() && S.containsCurrent() ) {
					out.pop();
				}
				return out;
			}

			//! The stack a walk ENTERED FROM ABOVE starts from: the entry
			//! stack itself.  It normally does not hold O (that IS the
			//! medium above the top).  When it does -- an inconsistent
			//! state for a geometric from-above entry, e.g. a surface the
			//! stack crossed into without ever crossing back -- the stack is
			//! trusted as the pre-DL-341 walk trusted it: the top is handed
			//! it unpopped and takes its own from-inside branch (DL-341
			//! review round 1: popping it there turned the double-sided
			//! closed-mesh exit into an entry).  Only a FROM-BELOW walk pops
			//! O (OutsideOf).
			static inline const IORStack& OutRef( const IORStack& S, std::optional<IORStack>& )
			{
				return S;
			}

			//! The EXTERNAL form of the internal stack a ray leaves the
			//! walk with: BELOW (the bottom key innermost) becomes OUT plus
			//! O at the below medium's index; anything else (OUT after an
			//! exit up through the top; GAP after a non-pushing bottom) is
			//! already in external form.  `out` carries O as its current
			//! object.
			static IORStack* ToExternal( const CompositeSPF& s, const IORStack& internalSt, const IORStack& out )
			{
				IORStack* p = 0;
				if( internalSt.topObject() == BottomKey( s ) ) {
					p = new IORStack( out );
					if( out.currentObject() ) {
						p->push( internalSt.top() );
					}
				} else {
					p = new IORStack( internalSt );
					if( out.currentObject() ) {
						p->SetCurrentObject( out.currentObject() );
					}
				}
				GlobalLog()->PrintNew( p, __FILE__, __LINE__, "ior stack" );
				return p;
			}

			//! Hands `r` (owned by a layer's container) the external stack.
			static inline void SetExternalStack( const CompositeSPF& s, ScatteredRay& r, const IORStack& internalSt, const IORStack& out )
			{
				IORStack* p = ToExternal( s, internalSt, out );
				if( r.delete_stack ) {
					safe_delete( r.ior_stack );
				}
				r.ior_stack = p;
				r.delete_stack = true;
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
				const IORStack& entry,
				const Scalar nm
				)
			{
				std::optional<IORStack> outStore;
				const IORStack& outside = OutRef( entry, outStore );
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
				// `ISPF::SelectionMassIsDeterministic( ri, nm )` takes the aggregate
				// path; every other top runs PER-BRANCH mode, whose weights
				// need only be deterministic positive numbers (the floors in
				// MakeWeights keep every class reachable).  Several hashed
				// probes are averaged there purely to make those numbers
				// closer to the true split -- an efficiency choice, not a
				// correctness one: with two probes a single-emit top whose
				// true down share is 0.3 read "no down mass" at 49 % of
				// shading points, flooring the walker's share to 1 % and
				// weighting its rare straight-through exits ~100x (a
				// heavy-tailed estimator a BDPT light-tracing splat did not
				// converge within 1024 spp on DL-05's composite-of-weaves
				// row).  kPerBranchProbes = 8 takes that to 5.8 %.
				const bool declared = s.top.SelectionMassIsDeterministic( ri, nm );
				Scalar upSum = up1, dnSum = dn1;
				int nProbes = 1;
				uint64_t downSalt = 0;
				bool haveDownSalt = false;
				if( !declared ) {
					for( int i = 1; i < kPerBranchProbes; i++ ) {
						const uint64_t salt = kSaltProbeB + uint64_t( i ) * 0x9E3779B97F4A7C15ull;
						ScatteredRayContainer ci;
						HashedSampler hs( seed ^ salt );
						P::Scatter( s.top, ri, hs, nm, ci, outside );
						const Scalar dnI = SubsetMass<P>( ci, isDown );
						upSum += SubsetMass<P>( ci, isUp );
						dnSum += dnI;
						nProbes++;
						if( !haveDownSalt && dnI > 0 ) {
							downSalt = salt;
							haveDownSalt = true;
						}
					}
				}

				Probe pr;
				pr.det = declared;
				pr.Qup   = upSum / Scalar( nProbes );
				pr.Qdown = dnSum / Scalar( nProbes );
				pr.walkerPossible = !s.HasLayeredValue();

				// The capability probe below follows ONE transmitted ray:
				// the first probe draw that had one.
				ScatteredRayContainer c2;
				if( !( dn1 > 0 ) && haveDownSalt ) {
					HashedSampler hs( seed ^ downSalt );
					P::Scatter( s.top, ri, hs, nm, c2, outside );
				}
				const Scalar dn2 = SubsetMass<P>( c2, isDown );

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
				P::Scatter( s.bottom, rb, hs, nm, cb, Keyed( gap, BottomKey( s ) ) );
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
				P::Scatter( s.top, rt, hs, nm, ct, cb[bestUp].ior_stack ? KeyedTop( *cb[bestUp].ior_stack, outside ) : gap );
				for( unsigned int i = 0; i < ct.Count(); i++ ) {
					const ScatteredRay& r = ct[i];
					if( Vector3Ops::Dot( r.ray.Dir(), n ) >= 0 &&
						( ( !r.isDelta && !s.pTopBSDF ) || ( r.isDelta && !s.top.DeltaTransmissionIsRefraction() ) ) ) {
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
			//  PER RECORD (`ISPF::SelectionMassIsDeterministic(ri, nm)`:
			//  perfect reflector/refractor and translucent always; a
			//  dielectric only when its transmission warp is off or its
			//  shading normal equals its geometric normal -- the warp is
			//  clipped to the GEOMETRIC side, so under a tilted shading
			//  normal the wedge between the two planes moves mass across
			//  the shading plane at random; DL-24 review round 2 P1-A,
			//  CompositeEnergyConservationTest section T).  Not inferred from
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
			static const Scalar kAdjointCosineShare;		// DL-297: cosine share of the adjoint warp draw under a tilted shading normal

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
			//      DELTA transmission out of the gap.  For an IDEAL delta the
			//      internal direction u that refracts to wOut is fixed by
			//      Snell, and the term is
			//          beta * f_bottom(u) * W(u) * Tr(u) / eta^2
			//      with W the top's delta up-going kray from inside along u
			//      (FORWARD evaluation, so a layer's own interior absorption
			//      -- DielectricSPF's tau^distance -- is included exactly as
			//      the walk applies it) and 1/eta^2 the solid-angle
			//      compression of the exit: n_out^2 cos dw_out ==
			//      n_gap^2 cos dw_in.
			//      DL-297 (2026-10-02): a top whose delta-tagged transmission
			//      is WARPED (DielectricSPF with a finite `scattering`, a
			//      clipped Phong cos^N lobe about the Snell direction t)
			//      lands at wOut with density q(wOut | u), so
			//          f(wOut) cos(wOut) = INT dt cos(t)/eta^2 g(u(t)) q(wOut | u(t))
			//      (g = beta f_bottom W Tr; u(t) the inverse Snell map).  One
			//      sample of t from the ADJOINT warp p(t) -- the same cos^N
			//      lobe about wOut, clipped to the exit hemisphere -- gives
			//          beta f_bottom(u) W(u) Tr(u) / eta^2 * q cos(t) / ( p(t) cos(wOut) ),
			//      which reduces to the ideal term when q and p are deltas.
			//      q comes from the top itself (ISPF::DeltaTransmissionWarpPdf),
			//      so it is the density of the draw its Scatter makes, clip
			//      and DL-111 re-derivation included.
			//  (b) At every top visit from below, the top's own BSDF for its
			//      NON-DELTA exit lobes.
			//  The walk continues by CONDITIONAL selection (reflections only
			//  at each layer); exits are accounted by (a)/(b), never
			//  sampled.  A Henyey-Greenstein warp (which also keeps a delta
			//  part) and a per-channel RGB warp have no single-density form
			//  and are still priced as ideal (DL-297's residual).  A top
			//  whose delta-tagged transmissions are not refractions at all
			//  (a nested composite: they are whole walks) has no term (a);
			//  the walker carries that class (DL-341).
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
			//  P2-3; +40 % render cost before).  Term (a)'s draws at the
			//  exit come from their own stream seeded by (wi, wOut,
			//  position), so `value` stays a deterministic function of its
			//  arguments.  Sharing one walk across a vertex's exits
			//  CORRELATES those estimates; each is still unbiased.
			//
			//  STACKS (DL-341): the walk threads ONE internal stack, the
			//  bottom keyed by BottomKey(s), the top by O.  The bottom is
			//  therefore evaluated against the GAP medium (its own key not
			//  yet pushed), the top from below against the gap with O in it.
			// -----------------------------------------------------------
			template<class P>
			struct WalkPath
			{
				typedef typename P::T T;
				bool                   entered;
				IORStack               out;			//!< OUT (the medium above the top; term (a)'s n_out)
				IORStack               gap0;			//!< gap stack after the entry transmission (term (a)'s eta and top scatter)
				std::vector<T>         betaBot;		//!< throughput ARRIVING at each bottom visit
				std::vector<Vector3>   wBot;
				std::vector<Scalar>    LBot;
				std::vector<IORStack>  gapBot;		//!< stack the bottom sees at that visit (bottom-keyed)
				std::vector<T>         betaTop;		//!< throughput ARRIVING at each top-underside visit
				std::vector<Vector3>   wTop;
				std::vector<Scalar>    LTop;
				std::vector<IORStack>  gapTop;		//!< gap stack the top sees at that visit
				WalkPath() : entered( false ), out( Scalar( 1 ) ), gap0( Scalar( 1 ) ) {}
				void Clear()
				{
					entered = false;
					betaBot.clear(); wBot.clear(); LBot.clear(); gapBot.clear();
					betaTop.clear(); wTop.clear(); LTop.clear(); gapTop.clear();
				}
			};

			template<class P>
			static void BuildWalk(
				const CompositeSPF& s,
				const RayIntersectionGeometric& ri,
				const IORStack& entry,
				const Scalar nm,
				WalkPath<P>& path
				)
			{
				typedef typename P::T T;
				path.Clear();
				const Vector3 n = ri.onb.w();
				auto isUpBottom = [&n]( const ScatteredRay& r ) { return Vector3Ops::Dot( r.ray.Dir(), n ) > 0; };
				auto isDownTop  = [&n]( const ScatteredRay& r ) { return Vector3Ops::Dot( r.ray.Dir(), n ) < 0; };

				path.out = entry;		// from above only (OutRef)
				const IORStack& outside = path.out;
				const IObject* const kB = BottomKey( s );

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
					path.gapBot.push_back( Keyed( gap, kB ) );
					SetLayerRay( rec, ri, w, Ld );
					ScatteredRayContainer cb;
					P::Scatter( s.bottom, rec, hs, nm, cb, path.gapBot.back() );
					k = SelectCarried<P>( cb, isUpBottom, beta, hs.Get1D(), q );
					if( k < 0 ) {
						return;
					}
					beta = P::Mul( beta, P::Scaled( P::Kray( cb[k] ), Scalar( 1 ) / q ) );
					w = Vector3Ops::Normalize( cb[k].ray.Dir() );
					if( cb[k].ior_stack ) {
						gap = KeyedTop( *cb[k].ior_stack, outside );
					}
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

			//! DL-297: one draw of the ADJOINT of a cos^N warp -- the lobe
			//! about `w`, clipped to the exit hemisphere `t . n > 0` (the
			//! Snell image of any internal direction lies there) -- and
			//! its exact density.  Where the shading normal is TILTED off
			//! the geometric one the forward warp may be re-derived about
			//! the true surface (DL-111), so its support is not guaranteed
			//! to lie inside this lobe's: a cosine-hemisphere component
			//! (`kAdjointCosineShare`) then keeps every exit direction
			//! reachable.  Returns false (no sample) on a degenerate arc.
			static bool SampleAdjointWarp(
				const Vector3& w,
				const Vector3& n,
				const Scalar N,
				const bool tilted,
				const OrthonormalBasis3D& onb,
				ISampler& smp,
				Vector3& t,
				Scalar& pdf
				)
			{
				const Scalar eps = tilted ? kAdjointCosineShare : Scalar( 0 );
				const Scalar uSel = smp.Get1D();
				const Point2 u2 = smp.Get2D();
				Scalar pPhong = 0;
				bool havePhong = false;
				if( uSel < eps ) {
					t = GeometricUtilities::CreateDiffuseVector( onb, u2 );
				} else {
					// The lobe's own draw: polar cosine u^(1/(N+1)), azimuth
					// on the valid arc -- its density is known from the draw.
					const Scalar c = r_min( Scalar( 1 ), pow( u2.x, Scalar( 1 ) / ( N + Scalar( 1 ) ) ) );
					Scalar half = PI;
					t = ( c < Scalar( 1 ) ) ? GeometricUtilities::PerturbClipped( w, acos( c ), n, u2.y, &half ) : w;
					if( !( half > 0 ) ) {
						return false;
					}
					pPhong = ( N + Scalar( 1 ) ) * pow( c, N ) / ( Scalar( 2 ) * half );
					havePhong = true;
				}
				t = Vector3Ops::Normalize( t );
				const Scalar cn = Vector3Ops::Dot( t, n );
				if( !( cn > 0 ) ) {
					return false;
				}
				if( !havePhong ) {
					// A cosine draw: evaluate the Phong component at it.
					const Scalar ct = Vector3Ops::Dot( t, w );
					if( ct > 0 ) {
						Scalar half = PI;
						if( ct < Scalar( 1 ) ) {
							GeometricUtilities::PerturbClipped( w, acos( ct ), n, Scalar( 0.5 ), &half );
						}
						if( half > 0 ) {
							pPhong = ( N + Scalar( 1 ) ) * pow( ct, N ) / ( Scalar( 2 ) * half );
						}
					}
				}
				pdf = ( Scalar( 1 ) - eps ) * pPhong + eps * cn * INV_PI;
				return pdf > 0;
			}

			//! True when the shading normal is tilted off the geometric one
			//! (the DielectricSPF::SelectionMassIsDeterministic test).
			static inline bool ShadingTilted( const RayIntersectionGeometric& ri )
			{
				if( !ri.HasTrueGeomSide() || !( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar( 1e-12 ) ) ) {
					return false;
				}
				const Scalar c = fabs( Vector3Ops::Dot( Vector3Ops::Normalize( ri.vGeomNormal ), ri.onb.w() ) );
				return c < Scalar( 1 ) - Scalar( 1e-12 );
			}

			//! The connection terms against a recorded walk.
			template<class P>
			static typename P::T EvaluateFromWalk(
				const CompositeSPF& s,
				const RayIntersectionGeometric& ri,
				const Vector3& wOut,
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
				// top's delta transmission at u.
				if( s.pBottomBSDF && !path.betaBot.empty() && s.top.DeltaTransmissionIsRefraction() ) {
					const Scalar nOut = path.out.top();
					const Scalar nGap = path.gap0.top();
					const Scalar eta = ( nOut > 0 && nGap > 0 ) ? ( nGap / nOut ) : Scalar( 1 );
					HashedSampler hx( HashPoint( HashVector( HashVector( kSaltEvaluateExit, ri.ray.Dir() ), wOut ), ri.ptIntersection ) );

					// DL-297: a WARPED delta transmission is connected
					// through an adjoint draw of its warp (see above).
					const Scalar warpN = s.top.DeltaTransmissionWarpExponent( ri, nm );
					Vector3 t = wOut;
					Scalar pT = 0;
					bool ok = true;
					if( warpN >= 0 ) {
						ok = SampleAdjointWarp( wOut, n, warpN, ShadingTilted( ri ), ri.onb, hx, t, pT );
					}
					Vector3 ux( 0, 0, 1 );
					if( ok && InternalDirectionForExit( t, n, eta, ux ) ) {
						const Scalar Lx = CompositeSPF::GapPathLength( ux, n, s.thickness );
						SetLayerRay( rec, ri, ux, Lx );
						ScatteredRayContainer cx;
						P::Scatter( s.top, rec, hx, nm, cx, path.gap0 );
						T W = P::Zero();
						Scalar adj = 1;
						if( warpN >= 0 ) {
							// The transmission's weight wherever its warp
							// happened to land on this draw (its density is
							// q, below): identified by type, not direction.
							for( unsigned int i = 0; i < cx.Count(); i++ ) {
								if( cx[i].isDelta && cx[i].type == ScatteredRay::eRayRefraction ) {
									W = W + P::Kray( cx[i] );
								}
							}
							const Scalar q = s.top.DeltaTransmissionWarpPdf( rec, wOut, nm, path.gap0 );
							const Scalar cOut = Vector3Ops::Dot( wOut, n );
							adj = ( q > 0 && cOut > 0 ) ? q * Vector3Ops::Dot( t, n ) / ( pT * cOut ) : Scalar( 0 );
						} else {
							for( unsigned int i = 0; i < cx.Count(); i++ ) {
								if( cx[i].isDelta && Vector3Ops::Dot( cx[i].ray.Dir(), n ) >= 0 ) {
									W = W + P::Kray( cx[i] );
								}
							}
						}
						if( P::MaxOf( W ) > 0 && adj > 0 ) {
							const T aFactor = P::Scaled( P::Mul( W, P::GapAtt( s.extinction, ri, nm, Lx ) ), adj / ( eta * eta ) );
							for( size_t i = 0; i < path.betaBot.size(); i++ ) {
								SetLayerRay( rec, ri, path.wBot[i], path.LBot[i] );
								f = f + P::Mul( P::Mul( path.betaBot[i], P::Value( *s.pBottomBSDF, ux, rec, nm, &path.gapBot[i] ) ), aFactor );
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
					return EvaluateFromWalk<P>( s, ri, wOut, nm, local );
				}

				Scalar key[kProbeKeySize];
				ProbeKey( ri, outside, key );
				for( int i = 0; i < kWalkCacheSize; i++ ) {
					const Entry& e = cache[i];
					if( e.valid && e.id == s.instanceId && e.nm == nm &&
						std::memcmp( e.key, key, sizeof( key ) ) == 0 ) {
						return EvaluateFromWalk<P>( s, ri, wOut, nm, e.path );
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
				return EvaluateFromWalk<P>( s, ri, wOut, nm, e.path );
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
				if( !CoveredEntry( ri ) ) {
					return;
				}
				const Vector3 w = Vector3Ops::Normalize( vLightIn );
				if( !( Vector3Ops::Dot( w, ri.onb.w() ) > 0 ) ) {
					return;
				}
				if( s.pTopBSDF ) {
					std::optional<IORStack> outStore;
					direct = P::Value( *s.pTopBSDF, vLightIn, ri, nm, pStack ? &OutRef( *pStack, outStore ) : pStack );
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

			//! Where a walker starts (DL-341).
			enum WalkStart
			{
				eStartCoveredTop,		//!< the WALKER branch of a covered entry: conditional entry transmission, covered exits not emitted
				eStartNaturalTop,		//!< from above but behind a tilted shading normal: natural walk from the top, every exit emitted
				eStartNaturalBottom		//!< from below: natural walk from the bottom, every exit emitted
			};

			//! DL-341: the GAP stack a walk entered from BELOW starts its
			//! return trip into -- OUT plus whatever the top pushes when it
			//! transmits a ray DOWN (read off a hashed from-above Scatter of
			//! the top at normal incidence; a top that pushes nothing leaves
			//! the gap at OUT).  The bottom's exit out of the BELOW medium
			//! refracts into this index, and the top's own exit pops it.
			template<class P>
			static IORStack GapStackForBelow(
				const CompositeSPF& s,
				const RayIntersectionGeometric& ri,
				const IORStack& out,
				const Scalar nm
				)
			{
				if( !out.currentObject() ) {
					return out;
				}
				const Vector3 n = ri.onb.w();
				RayIntersectionGeometric rp( ri );
				rp.ray.origin = ri.ptIntersection;
				rp.ray.SetDir( n );
				rp.ray.Advance( Scalar( 1 ) );
				rp.ray.SetDir( -n );
				// Every multi-emit top (dielectric, perfect refractor,
				// translucent: all lobes emitted per Scatter) shows its
				// transmission on the FIRST draw at normal incidence, so the
				// answer is a deterministic function of the record; a top
				// that DECLARES a deterministic split and transmits nothing
				// on that draw never pushes, and stops there too.  Only a
				// single-emit stochastic top (a nested composite, tissue) is
				// SAMPLED: up to kGapBelowDraws hashed draws, so a top that
				// transmits with probability p misses with (1-p)^16 (a
				// nested glass/glass composite: p ~ 0.92, ~1e-17).
				const uint64_t seed = HashPoint( kSaltGapBelow, ri.ptIntersection );
				const bool declared = s.top.SelectionMassIsDeterministic( rp, nm );
				for( int i = 0; i < kGapBelowDraws; i++ ) {
					HashedSampler hs( seed + uint64_t( i ) * 0x9E3779B97F4A7C15ull );
					ScatteredRayContainer c;
					P::Scatter( s.top, rp, hs, nm, c, out );
					int best = -1;
					Scalar bestW = -1;
					for( unsigned int j = 0; j < c.Count(); j++ ) {
						if( c[j].ior_stack && Vector3Ops::Dot( c[j].ray.Dir(), n ) < 0 &&
							c[j].ior_stack->containsCurrent() && P::Weight( c[j] ) > bestW ) {
							bestW = P::Weight( c[j] );
							best = (int)j;
						}
					}
					if( best >= 0 ) {
						return KeyedTop( *c[best].ior_stack, out );
					}
					if( declared || c.Count() > 1 ) {
						break;		// multi-emit / declared: this draw is the answer
					}
				}
				return out;
			}

			template<class P>
			static void Walker(
				const CompositeSPF& s,
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				const Scalar nm,
				ScatteredRayContainer& scattered,
				const IORStack& entry,
				const Scalar weightScale,
				const WalkStart start
				)
			{
				typedef typename P::T T;
				const Vector3 n = ri.onb.w();
				const IObject* const kB = BottomKey( s );
				// OUT: popped only for a walk from below (OutRef).
				const IORStack out = ( start == eStartNaturalBottom ) ? OutsideOf( entry ) : entry;

				// ONE internal stack (DL-341): the bottom is called with its
				// own key, the top with O; refreshed after every crossing.
				IORStack st( out );
				T beta = P::Scaled( P::One(), weightScale );
				unsigned int steps = 0;
				bool atBottom = false;
				bool lastBottomEvaluable = false;
				RayIntersectionGeometric cur( ri );

				if( start == eStartCoveredTop ) {
					auto isDownTop = [&n]( const ScatteredRay& r ) { return Vector3Ops::Dot( r.ray.Dir(), n ) < 0; };
					ScatteredRayContainer c0;
					P::Scatter( s.top, ri, sampler, nm, c0, out );
					Scalar q = 0;
					const int k = SelectSubset<P>( c0, isDownTop, sampler.Get1D(), q );
					if( k < 0 ) {
						return;
					}
					beta = P::Mul( beta, P::Scaled( P::Kray( c0[k] ), Scalar( 1 ) / q ) );
					const Vector3 w = Vector3Ops::Normalize( c0[k].ray.Dir() );
					if( c0[k].ior_stack ) {
						st = *c0[k].ior_stack;
					}
					if( !Roulette<P>( s, beta, c0[k].type, 0, sampler ) ) {
						return;
					}
					steps = 1;
					const Scalar L = CompositeSPF::GapPathLength( w, n, s.thickness );
					beta = P::Mul( beta, P::GapAtt( s.extinction, ri, nm, L ) );
					SetLayerRay( cur, ri, w, L );
					atBottom = true;
				} else if( start == eStartNaturalBottom ) {
					// From below: the ray is in the BELOW medium (the
					// entry stack's top -- O's index when the stack holds
					// O, the medium the ray is in when it does not).
					st = GapStackForBelow<P>( s, ri, out, nm );
					st.SetCurrentObject( kB );
					st.push( entry.top() );
					atBottom = true;
				}

				for( unsigned int ev = 0; ev < CompositeSPF::kMaxWalkEvents; ev++ )
				{
					if( !( P::MaxOf( beta ) > 0 ) ) {
						return;
					}
					ScatteredRayContainer c;
					// Re-key the one internal stack in place (no copy): the
					// layer reads it through a const reference and pushes or
					// pops only on its own copy.
					if( atBottom ) {
						st.SetCurrentObject( kB );
					} else if( out.currentObject() ) {
						st.SetCurrentObject( out.currentObject() );
					}
					P::Scatter( atBottom ? s.bottom : s.top, cur, sampler, nm, c, st );
					Scalar q = 0;
					auto any = []( const ScatteredRay& ) { return true; };
					const int kSel = SelectCarried<P>( c, any, beta, sampler.Get1D(), q );
					if( kSel < 0 || !( q > 0 ) ) {
						return;
					}
					ScatteredRay* r = &c[kSel];
					const Scalar cosN = Vector3Ops::Dot( r->ray.Dir(), n );
					const IORStack after( r->ior_stack ? *r->ior_stack : st );

					if( atBottom ) {
						if( cosN <= 0 ) {
							// Out through the bottom: never priced by the
							// evaluator.
							P::SetKray( *r, P::Mul( beta, P::Scaled( P::Kray( *r ), Scalar( 1 ) / q ) ) );
							r->isDelta = true;
							SetExternalStack( s, *r, after, out );
							Emit( scattered, *r );
							return;
						}
						lastBottomEvaluable = ( s.pBottomBSDF != 0 ) && !r->isDelta;
					} else {
						if( cosN >= 0 ) {
							// Out through the top.  Covered iff the evaluator
							// prices it: term (b) (non-delta exit, top has a
							// BSDF) or term (a) (delta exit right after a
							// non-delta bottom event with a BSDF, through a
							// top whose delta transmission is a refraction --
							// ISPF::DeltaTransmissionIsRefraction; a nested
							// composite's exits are whole walks, DL-341).
							const bool covered = ( start == eStartCoveredTop ) && (
								( !r->isDelta && s.pTopBSDF ) ||
								( r->isDelta && lastBottomEvaluable && s.top.DeltaTransmissionIsRefraction() ) );
							if( !covered ) {
								P::SetKray( *r, P::Mul( beta, P::Scaled( P::Kray( *r ), Scalar( 1 ) / q ) ) );
								r->isDelta = true;
								SetExternalStack( s, *r, after, out );
								Emit( scattered, *r );
							}
							return;
						}
					}
					st = after;

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
				const IORStack& entry,
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
				EvaluateLayeredParts<P>( s, w, ri, &entry, nm, direct, walked );
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
				// DL-341: the entry side is the GEOMETRIC one (EnteredFromAbove);
				// an arrival from below, or from above but behind a tilted
				// shading normal, is walked naturally from the layer it
				// meets first and delta-tagged (the evaluator does not price
				// it).  See "THE STACK CONVENTION".
				if( !EnteredFromAbove( ri ) ) {
					Walker<P>( s, ri, sampler, nm, scattered, ior_stack, Scalar( 1 ), eStartNaturalBottom );
				} else if( !CoveredEntry( ri ) ) {
					Walker<P>( s, ri, sampler, nm, scattered, ior_stack, Scalar( 1 ), eStartNaturalTop );
				} else {
					std::optional<IORStack> outStore;
					const IORStack& outside = OutRef( ior_stack, outStore );
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
						P::Scatter( s.top, ri, sampler, nm, c, outside );
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
								EmitNonDelta<P>( s, ri, nm, scattered, ior_stack, outside, pr, W,
									Vector3Ops::Normalize( r.ray.Dir() ), r.type, false );
							}
						}
					} else if( u < W.w1 + W.w2 ) {
						const Vector3 w = GeometricUtilities::CreateDiffuseVector( ri.onb, sampler.Get2D() );
						if( PassesGeomGate( ri, w ) ) {
							EmitNonDelta<P>( s, ri, nm, scattered, ior_stack, outside, pr, W,
								w, ScatteredRay::eRayDiffuse, !W.aggregate );
						}
					} else if( u < W.w1 + W.w2 + W.w3 ) {
						ScatteredRayContainer c;
						P::Scatter( s.bottom, ri, sampler, nm, c, outside );
						Scalar q = 0;
						const ScatteredRay* r = c.RandomlySelect( sampler.Get1D(), P::kNM, &q );
						if( r && !r->isDelta && Vector3Ops::Dot( r->ray.Dir(), n ) > 0 ) {
							EmitNonDelta<P>( s, ri, nm, scattered, ior_stack, outside, pr, W,
								Vector3Ops::Normalize( r->ray.Dir() ), r->type, !W.aggregate );
						}
					} else {
						if( W.w4 > 0 ) {
							Walker<P>( s, ri, sampler, nm, scattered, ior_stack, Scalar( 1 ) / W.w4, eStartCoveredTop );
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
				if( !CoveredEntry( ri ) ) {
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
				std::optional<IORStack> outStore;
				return MixturePdf<P>( s, ri, w, OutRef( ior_stack, outStore ), nm, pr, W, 0 );
			}
		};

		thread_local int CompositeSPFImpl::tEvaluateDepth = 0;
		const Scalar CompositeSPFImpl::kShareFloor            = Scalar( 0.02 );
		const Scalar CompositeSPFImpl::kWalkerShareIfPossible = Scalar( 0.5 );
		const Scalar CompositeSPFImpl::kWalkerShareFloor      = Scalar( 0.05 );
		const Scalar CompositeSPFImpl::kAdjointCosineShare    = Scalar( 0.1 );
	}
}

// DL-05.  Scatter's walk reaches a straight exit only as first-layer
// pass-through -> gap crossing -> second-layer pass-through (any other lobe
// turns the ray, and a turned ray never re-aligns with the incoming
// direction).  The first layer is chosen exactly as Scatter chooses it
// (DL-341: by the GEOMETRIC side, EnteredFromAbove -- top from above, bottom
// from below).  A first-layer pass-through that already leaves on its own
// side of the shading plane (from above: up-going, the edge-on `d == 0` case
// or a ray behind a tilted shading normal; from below: down-going, the
// mirror case) is an immediate exit of the walk, so it is the whole answer.
// Otherwise it is emitted only by the WALKER, as a delta-tagged exit through
// the far layer, with weight beta = (1/w4) * (kray1/q1) * Beer * (kray2/q2)
// on a draw of probability w4 * q1 * q2 -- expectation t1 * Beer * t2 for
// ANY branch weights.
//
// DL-24 re-derivation of the gates.  The pre-DL-24 walk DROPPED a
// continuation past `max_recur` or a per-type budget, so this used to return
// 0 when `max_recur < 2` or the step-0 refraction gate refused.  Since DL-24
// the budgets are only Russian-roulette ONSETS with the survival
// compensated (IsRouletteEligible / Roulette), and the walk's only hard
// stop is the 256-event safety cap, which a two-event straight path never
// reaches -- so no budget changes the EXPECTED straight-through weight, and
// the gates are gone.  So is the old importance floor on the attenuation:
// the walker stops only at an exactly zero throughput.
	//! DL-345: a composite's layers live INSIDE one thin surface, and its
	//! walk keys every internal layer crossing on the IOR stack it threads
	//! (DL-341, "THE STACK CONVENTION" above).  The open-sheet FACE
	//! rule the transmissive SPFs apply to a provably open sheet
	//! (IORStackSeeding::ResolveOpenSheetCrossing) would reinterpret those
	//! internal crossings -- an up-going walk ray meets the top from the
	//! sheet's back -- so every layer call sees the record WITHOUT that
	//! certification: the composite keeps its pre-DL-345 containment
	//! semantics on a clipped plane as on any other surface.  Copies only
	//! when the flag is set.
	//!
	//! DL-341 review round 1 (2026-10-02): the entry side is the TRUE
	//! geometric one.  A double-sided mesh / Bezier patch flips BOTH normals
	//! toward the ray (`bGeomNormalOrientedToRay`), so a hit from INSIDE a
	//! closed solid presented the composite's TOP to the ray and was walked
	//! as an entry from above: OUT popped O, the dielectric top refracted
	//! 1.0 -> 1.5 instead of 1.5 -> 1.0, and the exit claimed to be still
	//! inside (a closed double-sided glass/glass mesh box read 0.467 in a
	//! white furnace).  Such a record is UNFLIPPED here -- geometric normal
	//! back to the true outward one (DL-70 `UnflippedGeomNormal()`), the
	//! shading normal and frame oriented into its hemisphere -- so the
	//! walk sees the solid's true sides exactly as a single-sided mesh
	//! does.
	//!
	//! WHEN TO UNFLIP is decided by the WALK'S STACK, not by a geometry
	//! certificate (review rounds 2 and 3): a flipped record is unflipped
	//! only when the caller's IOR stack already holds this object -- the
	//! ray really is inside, it crossed in earlier.  `bOpenSheet` cannot
	//! decide it (on an indexed mesh it means NOT CERTIFIED watertight, and
	//! one T-junction un-certifies a closed box: round 3 read 0.466 there),
	//! and `BezierPatchGeometry` never sets it at all (DL-220), so a single
	//! open patch read as closed.  With the stack rule an open sheet hit on
	//! its back with no prior crossing keeps the flipped frame and presents
	//! its top on both faces, as the base did; a provably open sheet
	//! (`bProvablyNoInterior`) is never unflipped; hair's ray-derived normal
	//! has no true side.  BDPT / VCM reprice a connection on a record
	//! rebuilt by PathVertexEval::PopulateRIGFromVertex (which replays the
	//! surface-identity flags, BDPTVertex) against the stack
	//! BuildVertexIORStack rebuilds from the vertex's own `insideObject` --
	//! the same two inputs Scatter decided from, so the same frame.
	//!
	//! A STACKLESS caller (`IBSDF::value` without a stack, and
	//! DeltaPassThroughTransmittance, which has none) is never unflipped:
	//! it sees the reported frame, as before DL-341.  (For the straight
	//! pass-through the order of the two layers does not change the
	//! product `t1 * Beer * t2`.)  A camera or light INSIDE a closed
	//! composite is not seeded (DL-407), so its first inside hits keep the
	//! flipped frame too.
	static inline const RayIntersectionGeometric& CompositeLayerFrame(
		const RayIntersectionGeometric& ri,
		std::optional<RayIntersectionGeometric>& store,
		const IORStack* pStack
		)
	{
		const bool unflip = ri.bGeomNormalOrientedToRay && ri.HasTrueGeomSide() && !ri.bProvablyNoInterior &&
			pStack && pStack->currentObject() && pStack->containsCurrent();
		if( !ri.bProvablyNoInterior && !unflip ) {
			return ri;
		}
		store.emplace( ri );
		store->bProvablyNoInterior = false;
		if( unflip ) {
			store->vGeomNormal = ri.UnflippedGeomNormal();
			store->bGeomNormalOrientedToRay = false;
			if( Vector3Ops::Dot( store->vNormal, store->vGeomNormal ) < 0 ) {
				store->vNormal = -store->vNormal;
				store->onb.FlipW();
			}
		}
		return *store;
	}

RISEPel CompositeSPF::DeltaPassThroughTransmittance(
	const RayIntersectionGeometric& riIn
	) const
{
	std::optional<RayIntersectionGeometric> layerStore;
	const RayIntersectionGeometric& ri = CompositeLayerFrame( riIn, layerStore, 0 );

	const Vector3 dir = ri.ray.Dir();
	const Scalar  d   = Vector3Ops::Dot( dir, ri.onb.w() );
	const bool fromAbove = CompositeSPFImpl::EnteredFromAbove( ri );
	const ISPF& first  = fromAbove ? top : bottom;
	const ISPF& second = fromAbove ? bottom : top;

	const RISEPel t1 = first.DeltaPassThroughTransmittance( ri );
	if( !( ColorMath::MaxValue( t1 ) > 0 ) ) {
		return RISEPel( 0, 0, 0 );
	}
	if( ( fromAbove && d >= 0 ) || ( !fromAbove && d <= 0 ) ) {
		return t1;
	}

	RayIntersectionGeometric my_ri( ri );
	my_ri.ray.origin = ri.ptIntersection;
	my_ri.ray.SetDir( Vector3Ops::Normalize( dir ) );
	const Scalar pathLength = GapPathLength( my_ri.ray.Dir(), ri.onb.w(), thickness );
	my_ri.ray.Advance( pathLength );
	const RISEPel attenuation = GapAttenuation( extinction, ri, pathLength );
	if( !( ColorMath::MaxValue( attenuation ) > 0 ) ) {
		return RISEPel( 0, 0, 0 );
	}
	return t1 * attenuation * second.DeltaPassThroughTransmittance( my_ri );
}

Scalar CompositeSPF::DeltaPassThroughTransmittanceNM(
	const RayIntersectionGeometric& riIn,
	const Scalar nm
	) const
{
	std::optional<RayIntersectionGeometric> layerStore;
	const RayIntersectionGeometric& ri = CompositeLayerFrame( riIn, layerStore, 0 );

	const Vector3 dir = ri.ray.Dir();
	const Scalar  d   = Vector3Ops::Dot( dir, ri.onb.w() );
	const bool fromAbove = CompositeSPFImpl::EnteredFromAbove( ri );
	const ISPF& first  = fromAbove ? top : bottom;
	const ISPF& second = fromAbove ? bottom : top;

	const Scalar t1 = first.DeltaPassThroughTransmittanceNM( ri, nm );
	if( !( t1 > 0 ) ) {
		return 0;
	}
	if( ( fromAbove && d >= 0 ) || ( !fromAbove && d <= 0 ) ) {
		return t1;
	}

	RayIntersectionGeometric my_ri( ri );
	my_ri.ray.origin = ri.ptIntersection;
	my_ri.ray.SetDir( Vector3Ops::Normalize( dir ) );
	const Scalar pathLength = GapPathLength( my_ri.ray.Dir(), ri.onb.w(), thickness );
	my_ri.ray.Advance( pathLength );
	const Scalar attenuation = exp( -extinction.GetValueAtNM( ri, nm ) * pathLength );
	if( !( attenuation > 0 ) ) {
		return 0;
	}
	return t1 * attenuation * second.DeltaPassThroughTransmittanceNM( my_ri, nm );
}

void CompositeSPF::Scatter(
			const RayIntersectionGeometric& riIn,
			ISampler& sampler,
			ScatteredRayContainer& scattered,
			const IORStack& ior_stack
			) const
{
	std::optional<RayIntersectionGeometric> layerStore;
	const RayIntersectionGeometric& ri = CompositeLayerFrame( riIn, layerStore, &ior_stack );

	CompositeSPFImpl::ScatterImpl<CompositeSPFImpl::PipeRGB>( *this, ri, sampler, Scalar( -1 ), scattered, ior_stack );
}

void CompositeSPF::ScatterNM(
	const RayIntersectionGeometric& riIn,
	ISampler& sampler,
	const Scalar nm,
	ScatteredRayContainer& scattered,
	const IORStack& ior_stack
	) const
{
	std::optional<RayIntersectionGeometric> layerStore;
	const RayIntersectionGeometric& ri = CompositeLayerFrame( riIn, layerStore, &ior_stack );

	CompositeSPFImpl::ScatterImpl<CompositeSPFImpl::PipeNM>( *this, ri, sampler, nm, scattered, ior_stack );
}

Scalar CompositeSPF::Pdf(
	const RayIntersectionGeometric& riIn,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	std::optional<RayIntersectionGeometric> layerStore;
	const RayIntersectionGeometric& ri = CompositeLayerFrame( riIn, layerStore, &ior_stack );

	return CompositeSPFImpl::PdfImpl<CompositeSPFImpl::PipeRGB>( *this, ri, wo, Scalar( -1 ), ior_stack );
}

Scalar CompositeSPF::PdfNM(
	const RayIntersectionGeometric& riIn,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	std::optional<RayIntersectionGeometric> layerStore;
	const RayIntersectionGeometric& ri = CompositeLayerFrame( riIn, layerStore, &ior_stack );

	return CompositeSPFImpl::PdfImpl<CompositeSPFImpl::PipeNM>( *this, ri, wo, nm, ior_stack );
}

RISEPel CompositeSPF::EvaluateLayered(
	const Vector3& vLightIn,
	const RayIntersectionGeometric& riIn,
	const IORStack* pStack
	) const
{
	std::optional<RayIntersectionGeometric> layerStore;
	const RayIntersectionGeometric& ri = CompositeLayerFrame( riIn, layerStore, pStack );

	RISEPel direct, walked;
	CompositeSPFImpl::EvaluateLayeredParts<CompositeSPFImpl::PipeRGB>( *this, vLightIn, ri, pStack, Scalar( -1 ), direct, walked );
	return direct + walked;
}

Scalar CompositeSPF::EvaluateLayeredNM(
	const Vector3& vLightIn,
	const RayIntersectionGeometric& riIn,
	const Scalar nm,
	const IORStack* pStack
	) const
{
	std::optional<RayIntersectionGeometric> layerStore;
	const RayIntersectionGeometric& ri = CompositeLayerFrame( riIn, layerStore, pStack );

	Scalar direct = 0, walked = 0;
	CompositeSPFImpl::EvaluateLayeredParts<CompositeSPFImpl::PipeNM>( *this, vLightIn, ri, pStack, nm, direct, walked );
	return direct + walked;
}

Scalar CompositeSPF::EvaluateLobeFNM(
	const RayIntersectionGeometric& riIn,
	const Vector3& outDir,
	ScatteredRay::ScatRayType /*rayType*/,
	Scalar nm,
	const IORStack& ior_stack
	) const
{
	std::optional<RayIntersectionGeometric> layerStore;
	const RayIntersectionGeometric& ri = CompositeLayerFrame( riIn, layerStore, &ior_stack );

	// Every non-delta emission in AGGREGATE mode is priced
	// value(dir) * cos / Pdf(dir) with the deterministic layered value, so
	// the companion weight is exactly valueNM(dir; nm) * cos / pdfHero --
	// the ISPF default's 6-argument EvaluateKrayNM supplies the cos and the
	// division.  PER-BRANCH mode (a top whose selection is itself random)
	// prices each branch's own class instead, which this cannot recover.
	if( !CompositeSPFImpl::CoveredEntry( ri ) ) {
		return -1;
	}
	const CompositeSPFImpl::Probe pr = CompositeSPFImpl::DoProbe<CompositeSPFImpl::PipeNM>( *this, ri, ior_stack, nm );
	if( !pr.det ) {
		return -1;
	}
	return EvaluateLayeredNM( outDir, ri, nm, &ior_stack );
}

Scalar CompositeSPF::EvaluateKrayNM(
	const RayIntersectionGeometric& riIn,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& ior_stack
	) const
{
	std::optional<RayIntersectionGeometric> layerStore;
	const RayIntersectionGeometric& ri = CompositeLayerFrame( riIn, layerStore, &ior_stack );

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
	if( !CompositeSPFImpl::CoveredEntry( ri ) ) {
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
		std::optional<IORStack> outStore;
		top.ScatterNM( ri, hs, nm, c, CompositeSPFImpl::OutRef( ior_stack, outStore ) );
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
