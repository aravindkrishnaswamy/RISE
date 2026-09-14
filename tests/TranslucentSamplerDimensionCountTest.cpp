//////////////////////////////////////////////////////////////////////
//
//  TranslucentSamplerDimensionCountTest.cpp - Regression guard for
//    P1 (DL-45 exact-remap follow-up, 2026-09-13): `TranslucentSPF`'s diffuse-exit sampler
//    (`SampleValidDiffuseExit`, TranslucentSPF.cpp) used to rejection-
//    sample up to 32 times against the geometric-horizon constraint,
//    drawing 2 sampler dimensions per attempt -- a VARIABLE number of
//    draws per `Scatter()`/`ScatterNM()` call, depending on how many
//    attempts it took to land a geometrically-valid direction.
//
//  WHY THIS MATTERS
//
//    `ISampler::HasFixedDimensionBudget()` (ISampler.h) is true for
//    `SobolSampler`, which PT passes into `pSPF->Scatter` directly
//    (`PathTracingIntegrator.cpp`) -- a QMC sampler that partitions
//    dimensions into fixed-size per-bounce phases so every bounce's
//    Russian-roulette / light-selection draws start at the same
//    dimension offset regardless of what earlier draws in the bounce
//    consumed.  A rejection loop that sometimes draws 2 dimensions and
//    sometimes draws 4, 6, ... up to 64 shifts every LATER Get1D() call
//    in that bounce's phase in a way correlated with the tilt angle
//    (more tilt -> more rejections -> more dimensions consumed) -- at
//    60-degree tilt, 25% of exit scatters drew >=4 dimensions instead
//    of 2 (matching this fixture's own tilt and measured rate below),
//    and >=16 rejections in a row crossed into the NEXT bounce's phase
//    entirely.
//
//  THE FIX
//
//    `SampleValidDiffuseExit` now draws EXACTLY 2 canonical samples
//    every call (an exact closed-form Malley's-method remap onto the
//    geometrically-valid region -- no rejection, no second sampler) --
//    see the long derivation comment in TranslucentSPF.cpp.  This test
//    counts the ACTUAL number of `ISampler::Get1D()`/`Get2D()` calls
//    a real `Scatter()`/`ScatterNM()` call makes (via a counting
//    `ISampler` wrapper around a real `IndependentSampler`) and asserts
//    it is exactly 2.
//
//    P3 (review round 3): the count is asserted UNCONDITIONALLY, on
//    every call rather than only on calls that emitted an exit lobe --
//    the earlier `if(hasExit)` guard left the VANISH path (where the old
//    rejection loop burned its whole attempt budget, up to 64
//    dimensions, and emitted nothing) entirely unchecked.  The sweep
//    covers 0 / 60 / 179 / 180 degrees of shading-vs-outward tilt; 179
//    and 180 are the configurations that sat at or below
//    `kExitVanishThreshold` before P1 oriented the exit frame outward
//    (180 is a double-sided mesh's exit hit).  Post-P1, P(valid) >= 0.5
//    at every geometry, so the test also asserts an exit lobe IS emitted
//    on every trial at all four tilts.
//
//  RED-PROOF (recorded from a run against the pre-P1-fix library; see
//    the fix commit message for the verbatim failing output)
//
//    At 60-degree tilt, extinction=0, scattering=0 (isolating the exit
//    branch: no backscatter draws to confound the count), a real
//    `RandomNumberGenerator`-backed run over many trials shows a REAL
//    fraction of `Scatter()`/`ScatterNM()` calls consuming more than 2
//    dimensions (4, 6, ... up to 64) -- this test's `EXPECT(dims == 2)`
//    assertion fails on that fraction pre-fix, and passes unconditionally
//    post-fix.
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `translucent`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/TranslucentSPF.h"

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

static int failed = 0;
static int checks = 0;

#define EXPECT( cond, msg ) do { \
	checks++; \
	if( !(cond) ) { \
		std::cout << "FAIL: " << __FILE__ << ":" << __LINE__ << " " << msg << std::endl; \
		failed++; \
	} \
} while(0)

namespace
{
	//! Wraps a real ISampler and counts every canonical dimension drawn
	//! through it (Get1D -> 1, Get2D -> 2), forwarding to the inner
	//! sampler so the actual VALUES drawn are unaffected -- only the
	//! draw COUNT is observed.  `Reset()` zeroes the counter between
	//! calls under test so each `Scatter()`/`ScatterNM()` invocation's
	//! own consumption is isolated.
	class CountingSampler : public ISampler
	{
		ISampler& inner;
		int dims;
	public:
		explicit CountingSampler( ISampler& inner_ ) : inner( inner_ ), dims( 0 ) {}
		void Reset() { dims = 0; }
		int Dims() const { return dims; }

		Scalar Get1D() override { dims += 1; return inner.Get1D(); }
		Point2 Get2D() override { dims += 2; return inner.Get2D(); }
		void StartStream( int streamIndex ) override { inner.StartStream( streamIndex ); }
		bool HasFixedDimensionBudget() const override { return true; }
	};

	// True outward direction, matching TranslucentTiltedExitTest.cpp's
	// fixture convention: vGeomNormal fixed at (0,0,1), shading normal
	// tilted off it in the XZ plane.
	const Vector3 kTrueOutward( 0, 0, 1 );

	RayIntersectionGeometric MakeTiltedExitIntersection( Scalar tiltDeg )
	{
		const Scalar tiltRad = tiltDeg * PI / 180.0;
		const Vector3 n( sin(tiltRad), 0, cos(tiltRad) );

		Ray inRay( Point3(0,0,-1), Vector3(0,0,1) );
		RasterizerState rs = {0,0};
		RayIntersectionGeometric ri( inRay, rs );

		ri.bHit = true;
		ri.range = 1.0;
		ri.ptIntersection = Point3(0,0,0);
		ri.vNormal = n;
		ri.onb.CreateFromW( n );
		ri.vGeomNormal = kTrueOutward;
		ri.ptCoord = Point2(0.5,0.5);

		return ri;
	}

	IORStack MakeInsideStack( const IObject* obj )
	{
		IORStack stack( 1.0 );
		stack.SetCurrentObject( obj );
		stack.push( 1.33 );
		stack.SetCurrentObject( obj );
		return stack;
	}
}

static void RunTilt( Scalar tiltDeg )
{
	std::cout << "  tilt=" << tiltDeg << " deg" << std::endl;

	StubObject* obj = new StubObject();  obj->addref();

	UniformColorPainter* front = new UniformColorPainter( RISEPel(0.3,0.3,0.3) );  front->addref();
	UniformColorPainter* trans = new UniformColorPainter( RISEPel(0.3,0.3,0.3) );  trans->addref();
	UniformScalarPainter* ext = new UniformScalarPainter( 0.0 );  ext->addref();
	UniformScalarPainter* phongN = new UniformScalarPainter( 1.0 );  phongN->addref();
	UniformScalarPainter* scat = new UniformScalarPainter( 0.0 );  scat->addref();
	TranslucentSPF* pSpf = new TranslucentSPF( *front, *trans, *ext, *phongN, *scat );
	pSpf->addref();
	TranslucentSPF& spf = *pSpf;

	RayIntersectionGeometric ri = MakeTiltedExitIntersection( tiltDeg );
	IORStack stack = MakeInsideStack( obj );

	RandomNumberGenerator rng( 909090 );
	IndependentSampler inner( rng );
	CountingSampler counting( inner );

	const int kTrials = 8192;
	int rgbExitCount = 0, rgbNotTwo = 0;
	int nmExitCount = 0, nmNotTwo = 0;

	for( int i = 0; i < kTrials; i++ ) {
		ScatteredRayContainer scattered;
		counting.Reset();
		spf.Scatter( ri, counting, scattered, stack );
		for( unsigned int j = 0; j < scattered.Count(); j++ ) {
			if( scattered[j].type == ScatteredRay::eRayDiffuse ) rgbExitCount++;
		}
		// UNCONDITIONAL (P3, review round 3): the draw count is the
		// invariant, not "the draw count on calls that happened to emit
		// something".  Asserting it only when an exit lobe came out left
		// the vanish path -- the one case where the old rejection loop
		// could burn the most dimensions -- entirely unchecked.
		if( counting.Dims() != 2 ) rgbNotTwo++;
		EXPECT( counting.Dims() == 2, "RGB Scatter() draws exactly 2 sampler dimensions, exit lobe or not" );
	}

	for( int i = 0; i < kTrials; i++ ) {
		ScatteredRayContainer scattered;
		counting.Reset();
		spf.ScatterNM( ri, counting, 550.0, scattered, stack );
		for( unsigned int j = 0; j < scattered.Count(); j++ ) {
			if( scattered[j].type == ScatteredRay::eRayDiffuse ) nmExitCount++;
		}
		if( counting.Dims() != 2 ) nmNotTwo++;
		EXPECT( counting.Dims() == 2, "NM ScatterNM() draws exactly 2 sampler dimensions, exit lobe or not" );
	}

	std::cout << "    RGB: " << rgbExitCount << "/" << kTrials << " calls emitted an exit; "
		<< rgbNotTwo << " calls drew other than 2 dimensions" << std::endl;
	std::cout << "    NM:  " << nmExitCount << "/" << kTrials << " calls emitted an exit; "
		<< nmNotTwo << " calls drew other than 2 dimensions" << std::endl;

	// Post-P1 the exit lobe's frame is oriented outward before sampling,
	// so P(valid) >= 0.5 at EVERY geometry and the vanish branch can no
	// longer be reached from a production call -- including the
	// 180-degree case (a double-sided mesh's exit hit), which before P1
	// gave P(valid)=0 and emitted nothing at all.
	EXPECT( rgbExitCount == kTrials, "RGB fixture emits an exit lobe on every trial (vanish path unreachable post-P1)" );
	EXPECT( nmExitCount == kTrials, "NM fixture emits an exit lobe on every trial (vanish path unreachable post-P1)" );

	obj->release();
	pSpf->release();
	front->release();
	trans->release();
	ext->release();
	phongN->release();
	scat->release();
}

static void TestExactlyTwoDrawsPerScatterCall()
{
	std::cout << "Sub-test: exactly 2 sampler dimensions per Scatter()/ScatterNM() call (P1)" << std::endl;

	// 60 deg is DL-45's own fixture tilt (P(valid)=0.75, where the old
	// rejection loop drew >2 dimensions 25% of the time).  179 and 180
	// are the configurations that used to sit AT or BELOW
	// kExitVanishThreshold before P1 oriented the exit frame outward --
	// the vanish path, where a rejection loop burned its full attempt
	// budget (up to 64 dimensions) and emitted nothing.  0 is the
	// identity-remap control.
	const Scalar tilts[] = { 0.0, 60.0, 179.0, 180.0 };
	for( int i = 0; i < 4; i++ ) {
		RunTilt( tilts[i] );
	}
}

int main()
{
	GlobalLog();

	std::cout << "TranslucentSamplerDimensionCountTest: P1 fixed-dimension-budget draw count" << std::endl;

	TestExactlyTwoDrawsPerScatterCall();

	std::cout << std::endl;
	std::cout << "TranslucentSamplerDimensionCountTest: " << checks << " checks, " << failed << " failures" << std::endl;
	if( failed == 0 ) {
		std::cout << "ALL TESTS PASSED" << std::endl;
		return 0;
	}
	std::cout << failed << " CHECK(S) FAILED" << std::endl;
	return 1;
}
