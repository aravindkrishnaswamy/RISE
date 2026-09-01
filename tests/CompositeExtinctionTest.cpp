//////////////////////////////////////////////////////////////////////
//
//  CompositeExtinctionTest.cpp - Regression guard for composite_material's
//  inter-layer Beer-Lambert absorption (`extinction` + `thickness`).
//
//  WHAT THIS CATCHES (measured 2026-09-01)
//
//  Before the CompositeSPF::EffectiveStack fix, `extinction` and `thickness`
//  were COMPLETELY INERT: an env-lit dielectric-over-diffuse composite
//  rendered bit-near-identically for extinction 0.001 vs 1000, for thickness
//  0.0001 vs 50, and an asymmetric per-channel extinction (R=1000, G=1000,
//  B=0.001) rendered exactly R=G=B.
//
//  Root cause: CompositeSPF's random walk recursed with the ior_stack it was
//  HANDED rather than each scattered ray's OWN stack.  For the canonical
//  dielectric-top-over-Lambertian-bottom stack that killed the return trip:
//
//    steps=0  camera ray -> top DielectricSPF refracts DOWN; the scattered
//             ray's ior_stack has the dielectric pushed -- and the walk
//             discarded it.
//    steps=1  bottom Lambertian scatters UP (carries no stack of its own).
//    steps=2  ProcessTopLayer is re-entered with the ORIGINAL camera-side
//             stack and an UPWARD ray.  IORStack::containsCurrent() is false,
//             so DielectricSPF takes its "entering from outside" branch;
//             the transmission lobe is then culled by the hemisphere gate
//             (an upward direction cannot be a transmission when entering
//             from above) and the Fresnel lobe -- reflect(up, -N) -- points
//             DOWN and is culled by the geometric-normal gate.  The interface
//             emits NOTHING.
//
//  Every path that crossed the inter-layer gap therefore died inside the
//  walk.  Since `extinction` and `thickness` are applied ONLY to gap-crossing
//  legs, and the only surviving lobe (the first-interface Fresnel reflection)
//  never crosses the gap, both parameters were exactly inert -- which also
//  explains the exact R=G=B under an asymmetric extinction and the absence of
//  any blow-up at a negative thickness.
//
//  Empirical confirmation of the mechanism (an upward ray handed to a bare
//  DielectricSPF(ior 1.5) at the top interface, 20000 draws):
//    stack WITHOUT the dielectric pushed: 20000/20000 draws scattered NOTHING
//    stack WITH    the dielectric pushed: 0 empty; mean exiting kray 0.949
//
//  Tests 1-4 below ALL FAIL before the fix (1 collapses to the bare-Fresnel
//  level; 2-4 all become exact equalities because nothing responds).
//
//  Section 6 guards the SECOND half of the same story: the first fix threaded
//  each scattered ray's own stack UNCONDITIONALLY, which repaired the top
//  interface but broke the BOTTOM one -- because IORStack keys on the
//  IObject*, which both layers share.  The walk now carries two stacks.  See
//  the section-6 comment for the mechanism and the red-proof numbers.
//
//  This is an SPF-DIRECT unit test -- no scene, no rendering.  Fixture
//  construction mirrors tests/LayeredWhiteFurnaceTest.cpp.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <cmath>
#include <string>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/LambertianSPF.h"
#include "../src/Library/Materials/DielectricSPF.h"
#include "../src/Library/Materials/TranslucentSPF.h"
#include "../src/Library/Materials/CompositeSPF.h"

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

// Fixed seed => fully deterministic run.  MERSENNE53 is the configured
// backend (build/make/rise/Config.common), whose ctor takes a seed.
static const unsigned int kSeed    = 20260901u;
static const int          kSamples = 200000;

// Recursion budgets: the composite_material chunk's scene-language defaults,
// same values LayeredWhiteFurnaceTest uses.
static const unsigned int kMaxRecur      = 4;
static const unsigned int kMaxReflRecur  = 2;
static const unsigned int kMaxRefrRecur  = 2;
static const unsigned int kMaxDiffRecur  = 2;
static const unsigned int kMaxTransRecur = 2;

static StubObject* g_stub = 0;
static int         g_failures = 0;

// ============================================================
//  Fixture — same synthetic intersection as
//  LayeredWhiteFurnaceTest::MakeIntersection.
// ============================================================

static RayIntersectionGeometric MakeIntersection( const double thetaRad )
{
	const double sinT = std::sin( thetaRad );
	const double cosT = std::cos( thetaRad );

	const Vector3 inDir( sinT, 0, -cosT );
	const Ray     inRay( Point3( sinT, 0, 1.0 ), inDir );
	const RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );

	ri.bHit           = true;
	ri.range          = 1.0 / cosT;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal        = Vector3( 0, 0, 1 );
	ri.vGeomNormal    = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
	ri.ptCoord        = Point2( 0.5, 0.5 );

	return ri;
}

// ============================================================
//  Measurement
//
//  Aggregates the kray of every UP-exiting scattered ray over
//  kSamples draws.  Splits by ray type because the two
//  populations answer different questions:
//
//   * eRayReflection is (for this stack) the first-interface
//     Fresnel lobe.  It NEVER crosses the inter-layer gap, so it
//     is by construction insensitive to extinction/thickness --
//     it is the "floor" the broken walk was stuck at.
//   * eRayRefraction can ONLY be produced by the return trip out
//     through the top interface, i.e. by a path that crossed the
//     gap twice.  It is the direct gap-crossing signal and the
//     only population extinction/thickness can touch.
// ============================================================

struct Measurement
{
	double total   = 0;		// mean per-draw sum of max-channel kray, up-exiting
	double fresnel = 0;		// ... restricted to eRayReflection
	double crossed = 0;		// ... restricted to eRayRefraction (gap-crossing)
	double r = 0, g = 0, b = 0;	// mean per-draw per-channel sums, up-exiting
	long   nCrossed = 0;	// count of gap-crossing rays
	double down    = 0;		// mean per-draw sum of max-channel kray, DOWN-exiting
	long   nDown   = 0;		// count of down-exiting rays
};

static Measurement Measure( const ISPF& spf, const double thetaRad )
{
	RayIntersectionGeometric ri = MakeIntersection( thetaRad );
	RandomNumberGenerator rng( kSeed );
	IndependentSampler    sampler( rng );
	IORStack              iorStack = MakeTestIORStack( g_stub );

	const Vector3 normal = ri.onb.w();
	Measurement m;

	for( int i = 0; i < kSamples; ++i )
	{
		ScatteredRayContainer scattered;
		spf.Scatter( ri, sampler, scattered, iorStack );

		for( unsigned int j = 0; j < scattered.Count(); ++j )
		{
			const ScatteredRay& s = scattered[j];
			const Vector3 wo = Vector3Ops::Normalize( s.ray.Dir() );

			const double k = ColorMath::MaxValue( s.kray );
			if( !( k >= 0 && k < 1e6 ) ) continue;				// NaN / inf guard

			if( Vector3Ops::Dot( wo, normal ) <= 0 ) {
				// Transmitted THROUGH the stack and out the bottom.  Zero for a
				// Lambertian substrate; the primary signal for a transmissive
				// one (section 6).
				m.down += k;
				m.nDown++;
				continue;
			}

			m.total += k;
			m.r += s.kray[0];
			m.g += s.kray[1];
			m.b += s.kray[2];

			if( s.type == ScatteredRay::eRayReflection ) {
				m.fresnel += k;
			} else if( s.type == ScatteredRay::eRayRefraction ) {
				m.crossed += k;
				m.nCrossed++;
			}
		}
	}

	const double inv = 1.0 / double( kSamples );
	m.total *= inv;  m.fresnel *= inv;  m.crossed *= inv;
	m.r *= inv;  m.g *= inv;  m.b *= inv;
	m.down *= inv;
	return m;
}

static void PrintMeasurement( const std::string& label, const Measurement& m )
{
	std::cout << "  " << std::left << std::setw( 40 ) << label
	          << std::fixed << std::setprecision( 5 )
	          << " total=" << std::setw( 8 ) << m.total
	          << " fresnel=" << std::setw( 8 ) << m.fresnel
	          << " crossed=" << std::setw( 8 ) << m.crossed
	          << "  RGB=(" << m.r << ", " << m.g << ", " << m.b << ")"
	          << "  nCrossed=" << m.nCrossed
	          << "  down=" << m.down << " (n=" << m.nDown << ")\n";
}

// Spectral (NM) twin of Measure().  The NM walk is a separate code path
// (ProcessTopLayerNM / ProcessBottomLayerNM) that carried the SAME bug, so it
// needs its own check -- see docs/skills/audit-by-bug-pattern.md.
struct MeasurementNM
{
	double total   = 0;
	double crossed = 0;
	long   nCrossed = 0;
};

static MeasurementNM MeasureNM( const ISPF& spf, const double thetaRad, const Scalar nm )
{
	RayIntersectionGeometric ri = MakeIntersection( thetaRad );
	RandomNumberGenerator rng( kSeed );
	IndependentSampler    sampler( rng );
	IORStack              iorStack = MakeTestIORStack( g_stub );

	const Vector3 normal = ri.onb.w();
	MeasurementNM m;

	for( int i = 0; i < kSamples; ++i )
	{
		ScatteredRayContainer scattered;
		spf.ScatterNM( ri, sampler, nm, scattered, iorStack );

		for( unsigned int j = 0; j < scattered.Count(); ++j )
		{
			const ScatteredRay& s = scattered[j];
			if( Vector3Ops::Dot( Vector3Ops::Normalize( s.ray.Dir() ), normal ) <= 0 ) continue;
			const double k = s.krayNM;
			if( !( k >= 0 && k < 1e6 ) ) continue;

			m.total += k;
			if( s.type == ScatteredRay::eRayRefraction ) {
				m.crossed += k;
				m.nCrossed++;
			}
		}
	}

	const double inv = 1.0 / double( kSamples );
	m.total *= inv;  m.crossed *= inv;
	return m;
}

static void Check( const bool ok, const std::string& what )
{
	std::cout << "    " << ( ok ? "PASS" : "FAIL" ) << "  " << what << "\n";
	if( !ok ) g_failures++;
}

// ============================================================

int main()
{
	GlobalLog();

	g_stub = new StubObject();
	g_stub->addref();

	UniformColorPainter*  white = new UniformColorPainter( RISEPel( 1, 1, 1 ) );  white->addref();
	UniformScalarPainter* sTau  = new UniformScalarPainter( 1.0 );  sTau->addref();
	UniformScalarPainter* sIor  = new UniformScalarPainter( 1.5 );  sIor->addref();
	// scattering = 1e6 is the DELTA (clear) transmission end of DielectricSPF's
	// scattering knob -- 0.0 would mean maximally DIFFUSE transmission (see
	// docs: "dielectric `scattering 0.0` = maximally DIFFUSE, not off").  A
	// clear coat keeps the gap-crossing population clean and the geometry of
	// the Beer-Lambert path length interpretable.
	UniformScalarPainter* sScat = new UniformScalarPainter( 1000000.0 );  sScat->addref();

	// Extinction painters.  E_LO is "effectively transparent" rather than
	// exactly zero so that the zero case is not special-cased anywhere.
	UniformColorPainter* extLo   = new UniformColorPainter( RISEPel( 0.001, 0.001, 0.001 ) );  extLo->addref();
	UniformColorPainter* extHi   = new UniformColorPainter( RISEPel( 50.0, 50.0, 50.0 ) );     extHi->addref();
	UniformColorPainter* extAsym = new UniformColorPainter( RISEPel( 50.0, 50.0, 0.001 ) );    extAsym->addref();
	UniformColorPainter* extMid  = new UniformColorPainter( RISEPel( 5.0, 5.0, 5.0 ) );        extMid->addref();

	LambertianSPF*  lambertian = new LambertianSPF( *white );  lambertian->addref();
	DielectricSPF*  dielectric = new DielectricSPF( *sTau, *sIor, *sScat, /*hg*/ false );  dielectric->addref();

	std::cout << "\n================================================================\n";
	std::cout << "  CompositeSPF inter-layer extinction / thickness regression\n";
	std::cout << "================================================================\n";
	std::cout << "  Stack: DielectricSPF(ior 1.5, clear) over LambertianSPF(white)\n";
	std::cout << "  Samples per config: " << kSamples << "   seed: " << kSeed << "\n\n";

	// Reference: the bare top layer on its own.  Its albedo IS the
	// Fresnel-reflection-only level the broken walk was pinned to.
	const Measurement bare = Measure( *dielectric, 0.0 );
	PrintMeasurement( "bare dielectric (Fresnel-only floor)", bare );
	std::cout << "\n";

	// ------------------------------------------------------------
	// 1. Walk liveness: with a near-transparent gap the composite must
	//    emit gap-crossing rays, and its aggregate must sit far above
	//    the Fresnel-only floor.
	//
	//    PRE-FIX: total == bare.total to 5 decimals and nCrossed == 0.
	// ------------------------------------------------------------
	std::cout << "1. Walk liveness (extinction 0.001, thickness 0.02)\n";
	CompositeSPF* compLo = new CompositeSPF(
		*dielectric, *lambertian, kMaxRecur, kMaxReflRecur, kMaxRefrRecur,
		kMaxDiffRecur, kMaxTransRecur, 0.02, *extLo );
	compLo->addref();
	const Measurement lo = Measure( *compLo, 0.0 );
	PrintMeasurement( "composite ext=0.001 thick=0.02", lo );

	Check( lo.nCrossed > kSamples / 10,
	       "gap-crossing rays exist (nCrossed > 10% of draws)" );
	// Measured post-fix: total 0.42, floor 0.04 -> ratio ~10.6x.  The gate is
	// set at 4x: comfortably above MC noise, and a regression that loses the
	// return trip drops the ratio to exactly 1.0.
	Check( lo.total > 4.0 * bare.total,
	       "aggregate exiting kray exceeds 4x the Fresnel-only floor" );
	// The Fresnel lobe itself must be unchanged -- it never crosses the gap,
	// so a fix that "recovers energy" by inflating the reflection instead of
	// reviving the transmission would be caught here.
	Check( std::fabs( lo.fresnel - bare.total ) < 0.01,
	       "first-interface Fresnel lobe unchanged vs the bare top layer" );

	// ------------------------------------------------------------
	// 2. Extinction attenuates the gap-crossing population.
	//
	//    Beer-Lambert over two crossings of a 0.02-thick gap at
	//    extinction 50 gives roughly exp(-50 * 2 * 0.02 / cos) -- a
	//    ~7-9x drop once the actual (cosine-spread) path lengths are
	//    accounted for.  The band below is derived from the measured
	//    post-fix ratio (0.04604 / 0.38569 = 0.1194), widened to
	//    [0.05, 0.25] so it tracks the physics rather than one seed.
	//
	//    PRE-FIX: ratio == 1.000 exactly.
	// ------------------------------------------------------------
	std::cout << "\n2. Extinction attenuates (0.001 vs 50 at thickness 0.02)\n";
	CompositeSPF* compHi = new CompositeSPF(
		*dielectric, *lambertian, kMaxRecur, kMaxReflRecur, kMaxRefrRecur,
		kMaxDiffRecur, kMaxTransRecur, 0.02, *extHi );
	compHi->addref();
	const Measurement hi = Measure( *compHi, 0.0 );
	PrintMeasurement( "composite ext=50 thick=0.02", hi );

	const double extRatio = ( lo.crossed > 0 ) ? hi.crossed / lo.crossed : 1.0;
	std::cout << "    crossed(ext=50) / crossed(ext=0.001) = "
	          << std::fixed << std::setprecision( 4 ) << extRatio << "\n";
	Check( extRatio > 0.05 && extRatio < 0.25,
	       "gap-crossing energy attenuated into the measured Beer-Lambert band [0.05, 0.25]" );
	Check( hi.total < lo.total,
	       "total exiting energy strictly lower at the higher extinction" );

	// ------------------------------------------------------------
	// 3. Per-channel asymmetry.  extinction = (50, 50, 0.001) must
	//    leave BLUE far brighter than RED/GREEN.
	//
	//    PRE-FIX: R == G == B exactly.
	// ------------------------------------------------------------
	std::cout << "\n3. Per-channel asymmetry (extinction R=50 G=50 B=0.001)\n";
	CompositeSPF* compAsym = new CompositeSPF(
		*dielectric, *lambertian, kMaxRecur, kMaxReflRecur, kMaxRefrRecur,
		kMaxDiffRecur, kMaxTransRecur, 0.02, *extAsym );
	compAsym->addref();
	const Measurement asym = Measure( *compAsym, 0.0 );
	PrintMeasurement( "composite ext=(50,50,0.001) thick=0.02", asym );

	Check( asym.b > 3.0 * asym.r && asym.b > 3.0 * asym.g,
	       "blue channel more than 3x red and green" );
	Check( std::fabs( asym.r - asym.g ) < 1e-9,
	       "the two equally-extinguished channels stay exactly equal" );

	// ------------------------------------------------------------
	// 4. Thickness scales the attenuation at a fixed extinction.
	//
	//    PRE-FIX: identical for every thickness.
	// ------------------------------------------------------------
	std::cout << "\n4. Thickness scales attenuation (extinction 5, thickness 0.01 vs 0.1)\n";
	CompositeSPF* compThin = new CompositeSPF(
		*dielectric, *lambertian, kMaxRecur, kMaxReflRecur, kMaxRefrRecur,
		kMaxDiffRecur, kMaxTransRecur, 0.01, *extMid );
	compThin->addref();
	CompositeSPF* compThick = new CompositeSPF(
		*dielectric, *lambertian, kMaxRecur, kMaxReflRecur, kMaxRefrRecur,
		kMaxDiffRecur, kMaxTransRecur, 0.10, *extMid );
	compThick->addref();

	const Measurement thin  = Measure( *compThin, 0.0 );
	const Measurement thick = Measure( *compThick, 0.0 );
	PrintMeasurement( "composite ext=5 thick=0.01", thin );
	PrintMeasurement( "composite ext=5 thick=0.10", thick );

	const double thickRatio = ( thin.crossed > 0 ) ? thick.crossed / thin.crossed : 1.0;
	std::cout << "    crossed(t=0.10) / crossed(t=0.01) = "
	          << std::fixed << std::setprecision( 4 ) << thickRatio << "\n";
	// Measured post-fix: 0.3848 (0.13348 / 0.34690); the analytic
	// exp(-5 * 2 * (0.10 - 0.01) / cos) with a cosine-spread mean path is
	// ~0.28.  Band widened to [0.20, 0.60].
	Check( thickRatio > 0.20 && thickRatio < 0.60,
	       "10x thickness attenuates into the measured band [0.20, 0.60]" );
	Check( thick.total < thin.total,
	       "total exiting energy strictly lower at the greater thickness" );

	// ------------------------------------------------------------
	// 5. Spectral (NM) twin.  ProcessTopLayerNM / ProcessBottomLayerNM are a
	//    separate code path that carried the identical bug, so the NM walk
	//    gets its own liveness + attenuation check rather than being assumed
	//    fixed by the RGB result.
	//
	//    PRE-FIX: nCrossed == 0 and the two extinctions tie exactly.
	// ------------------------------------------------------------
	std::cout << "\n5. Spectral (NM) walk at 550 nm\n";
	const MeasurementNM nmLo = MeasureNM( *compLo, 0.0, 550.0 );
	const MeasurementNM nmHi = MeasureNM( *compHi, 0.0, 550.0 );
	std::cout << "  " << std::left << std::setw( 40 ) << "NM ext=0.001 thick=0.02"
	          << std::fixed << std::setprecision( 5 )
	          << " total=" << nmLo.total << " crossed=" << nmLo.crossed
	          << " nCrossed=" << nmLo.nCrossed << "\n";
	std::cout << "  " << std::left << std::setw( 40 ) << "NM ext=50    thick=0.02"
	          << " total=" << nmHi.total << " crossed=" << nmHi.crossed
	          << " nCrossed=" << nmHi.nCrossed << "\n";

	const double nmRatio = ( nmLo.crossed > 0 ) ? nmHi.crossed / nmLo.crossed : 1.0;
	std::cout << "    NM crossed(ext=50) / crossed(ext=0.001) = "
	          << std::fixed << std::setprecision( 4 ) << nmRatio << "\n";

	Check( nmLo.nCrossed > kSamples / 10,
	       "NM walk emits gap-crossing rays" );
	// Deliberately only a MONOTONICITY check, not a Beer-Lambert band.
	//
	// SECOND, INDEPENDENT BUG found while writing this test (2026-09-01, NOT
	// fixed here): CompositeSPF's `extinction` is typed `IPainter`, so the
	// spectral walk reads it through IPainter::GetColorNM.  For the ordinary
	// `uniformcolor_painter` that means RGBAlbedoSpectrum::FromRGB -- the
	// Jakob-Hanika ALBEDO uplift, which is bounded to [0,1] by construction.
	// An extinction of 50 therefore arrives at the NM walk as ~1.0, and the
	// measured spectral attenuation here is 0.958 where the RGB walk gives
	// 0.119.  Every extinction above ~1 is silently clamped in every spectral
	// rasterizer.  This is exactly the failure class docs/ISCALARPAINTER_REFACTOR.md
	// exists for: extinction is a PHYSICAL SCALAR coefficient, so the slot
	// wants `IScalarPainter` (which never goes through JH uplift), not
	// `IPainter`.  Retyping it touches the CompositeSPF ctor, RISE_API,
	// Job::AddCompositeMaterial, the composite_material chunk descriptor, and
	// needs a scene migration -- out of scope for the walk fix, so the check
	// below asserts only what IS true today: extinction is LIVE on the NM path
	// (pre-fix the two were an EXACT tie at zero) and pushes energy DOWN.  It
	// stays valid if and when the IScalarPainter retyping lands.
	Check( nmHi.crossed < nmLo.crossed * 0.999,
	       "NM gap-crossing energy responds to extinction (strictly lower at ext=50)" );

	// ------------------------------------------------------------
	// 6. STACK-SENSITIVE BOTTOM LAYER.
	//
	//    Sections 1-5 all use a Lambertian substrate, which ignores the IOR
	//    stack entirely -- so they are blind to WHICH stack the walk hands
	//    each layer.  This section uses substrates that DO read the stack.
	//
	//    IORStack keys its entries on `pCurrentObject` (IORStack.h), which is
	//    ONE pointer for the whole composite: the top layer's push is
	//    indistinguishable from an entry belonging to the bottom layer.  So a
	//    walk that threads the top's pushed stack straight down into the
	//    bottom makes the bottom read containsCurrent()==true for a ray that
	//    is physically ENTERING it from the gap above, and it takes its
	//    from-inside branch.  The fix threads TWO stacks (see
	//    CompositeSPF::EvalStack): `outside` (without this object's entry) and
	//    `gap` (with it), and evaluates every DOWN-going ray against `outside`
	//    and every UP-going ray against `gap`.
	//
	//    RED-PROOF (measured 2026-09-01 against the single-stack walk, i.e.
	//    the state right after the EffectiveStack commit 24888c67):
	//
	//      dielectric(1.5) over dielectric(1.33)
	//        single-stack : down=0.00000 (n=0)      total=0.04000  nCrossed=0
	//        two-stack    : down=0.94072 (n=200000) total=0.05849  nCrossed=200000
	//        (total=0.04000 pre-fix is EXACTLY the bare-top Fresnel floor: the
	//         bottom interface emitted nothing at all and 96% of the incident
	//         energy simply vanished inside the walk.)
	//
	//      dielectric(1.5) over translucent
	//        single-stack : nCrossed=0       total=0.04000
	//        two-stack    : nCrossed=89203   total=0.19556
	//
	//    Sections 1-5 are BYTE-IDENTICAL across the two (same seed, same
	//    sampler consumption): with a Lambertian substrate the two stacks
	//    coincide everywhere it matters, which is why those sections could not
	//    see this bug.
	//
	//    Mechanism of the single-stack failure, dielectric over dielectric:
	//    the bottom takes bFromInside, so its transmission lobe is killed by
	//    the `Dot(dir, w) >= NEARZERO` gate (DielectricSPF.cpp) -- a downward
	//    refraction cannot be an exit -- and its Fresnel lobe is killed by the
	//    geometric-normal gate.  BOTH lobes vanish: a black interface.
	//    Translucent bottom: TranslucentSPF's exit branch POPS, and since the
	//    key is shared it destroys the entry the TOP pushed; the up-going lobe
	//    then reaches the top interface with a popped stack and is
	//    double-culled -- exactly the failure 24888c67 fixed, re-introduced one
	//    layer down.  This is the shipped-scene case
	//    scenes/Tests/Materials/composite_material.RISEscene `mat_double_composite`.
	// ------------------------------------------------------------
	std::cout << "\n6. Stack-sensitive bottom layer (the shared-IObject-key trap)\n";

	UniformScalarPainter* sIorB = new UniformScalarPainter( 1.33 );  sIorB->addref();
	DielectricSPF* dielectricBot = new DielectricSPF( *sTau, *sIorB, *sScat, /*hg*/ false );
	dielectricBot->addref();

	CompositeSPF* compDD = new CompositeSPF(
		*dielectric, *dielectricBot, kMaxRecur, kMaxReflRecur, kMaxRefrRecur,
		kMaxDiffRecur, kMaxTransRecur, 0.02, *extLo );
	compDD->addref();

	const Measurement dd = Measure( *compDD, 0.0 );
	PrintMeasurement( "composite dielectric/dielectric", dd );

	// 6a. The bottom interface must actually transmit.  This is the direct
	//     signal: on the single-stack walk it is EXACTLY zero.
	Check( dd.nDown > kSamples / 10,
	       "bottom interface transmits (nDown > 10% of draws)" );
	Check( dd.down > 0.30,
	       "down-exiting transmission carries substantial energy (>0.30, measured 0.94)" );

	// 6b. Some of the light reflected off the BOTTOM interface must find its
	//     way back out through the top.  On the single-stack walk the bottom
	//     emits nothing at all, so the up-exiting total collapses to exactly
	//     the bare first-interface Fresnel level.
	Check( dd.total > bare.total * 1.10,
	       "up-exiting energy exceeds the bare-top Fresnel-only floor by >10%" );

	// 6c. Translucent bottom -- the mat_double_composite shape.  The
	//     up-exiting eRayRefraction population can ONLY be produced by a ray
	//     that went down through the gap, scattered off the substrate, came
	//     back up and refracted out of the top interface.
	UniformColorPainter*  tFront = new UniformColorPainter( RISEPel( 0.4, 0.4, 0.4 ) );  tFront->addref();
	UniformColorPainter*  tTrans = new UniformColorPainter( RISEPel( 0.6, 0.6, 0.6 ) );  tTrans->addref();
	UniformScalarPainter* tExt   = new UniformScalarPainter( 0.0 );    tExt->addref();
	UniformScalarPainter* tN     = new UniformScalarPainter( 100.0 );  tN->addref();
	UniformScalarPainter* tScat  = new UniformScalarPainter( 0.5 );    tScat->addref();

	TranslucentSPF* translucent = new TranslucentSPF( *tFront, *tTrans, *tExt, *tN, *tScat );
	translucent->addref();

	CompositeSPF* compDT = new CompositeSPF(
		*dielectric, *translucent, kMaxRecur, kMaxReflRecur, kMaxRefrRecur,
		kMaxDiffRecur, kMaxTransRecur, 0.02, *extLo );
	compDT->addref();

	const Measurement dt = Measure( *compDT, 0.0 );
	PrintMeasurement( "composite dielectric/translucent", dt );

	Check( dt.nCrossed > kSamples / 20,
	       "translucent substrate returns light out through the top (nCrossed > 5% of draws)" );

	// ------------------------------------------------------------
	// 7. Negative thickness must not become an energy source.
	//
	//    `thickness` reaches CompositeSPF straight from the scene file (parser
	//    default 0.0, no range check), and it lands in the Beer-Lambert
	//    exponent as a path length.  A NEGATIVE one flips attenuation into
	//    GAIN -- exp(-extinction * negative) > 1 -- once per gap crossing,
	//    which the live walk now performs.  The constructor clamps it to 0
	//    (with a warning), so a negative thickness must be
	//    indistinguishable from zero rather than brighter than either.
	// ------------------------------------------------------------
	std::cout << "\n7. Negative thickness is clamped (extinction 5, thickness -0.10 vs 0.0)\n";
	CompositeSPF* compZero = new CompositeSPF(
		*dielectric, *lambertian, kMaxRecur, kMaxReflRecur, kMaxRefrRecur,
		kMaxDiffRecur, kMaxTransRecur, 0.0, *extMid );
	compZero->addref();
	CompositeSPF* compNeg = new CompositeSPF(
		*dielectric, *lambertian, kMaxRecur, kMaxReflRecur, kMaxRefrRecur,
		kMaxDiffRecur, kMaxTransRecur, -0.10, *extMid );
	compNeg->addref();

	const Measurement zeroT = Measure( *compZero, 0.0 );
	const Measurement negT  = Measure( *compNeg,  0.0 );
	PrintMeasurement( "composite ext=5 thick= 0.00", zeroT );
	PrintMeasurement( "composite ext=5 thick=-0.10", negT );

	Check( std::fabs( negT.total - zeroT.total ) < 1e-9,
	       "negative thickness renders exactly as thickness 0" );
	// The standalone no-gain statement, independent of the clamp target: an
	// UNCLAMPED -0.10 at extinction 5 amplifies each crossing by exp(0.5) or
	// more, so this trips long before the equality above goes stale.  The 1%
	// slack absorbs the fact that the reference (`lo`) has a real, if tiny,
	// extinction of 0.001 rather than exactly zero.
	Check( negT.crossed <= lo.crossed * 1.01,
	       "no energy gain: gap-crossing energy stays at the transparent-gap level" );

	compNeg->release();
	compZero->release();
	compDT->release();
	translucent->release();
	tScat->release();
	tN->release();
	tExt->release();
	tTrans->release();
	tFront->release();
	compDD->release();
	dielectricBot->release();
	sIorB->release();

	compThick->release();
	compThin->release();
	compAsym->release();
	compHi->release();
	compLo->release();
	dielectric->release();
	lambertian->release();
	extMid->release();
	extAsym->release();
	extHi->release();
	extLo->release();
	sScat->release();
	sIor->release();
	sTau->release();
	white->release();
	g_stub->release();

	std::cout << "\n================================================================\n";
	if( g_failures ) {
		std::cout << "  " << g_failures << " check(s) FAILED\n";
	} else {
		std::cout << "  All checks passed\n";
	}
	std::cout << "================================================================\n";

	return g_failures ? 1 : 0;
}
