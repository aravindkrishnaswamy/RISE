//////////////////////////////////////////////////////////////////////
//
//  TranslucentIORStackTest.cpp - Regression guard for debt 30's P2-1
//    fix: TranslucentSPF fabricating a medium change.
//
//  THE BUG THIS TEST GUARDS AGAINST
//
//    `translucent_material` has no `ior` parameter -- entering or
//    leaving it never changes the medium the ray is in (a lampshade
//    submerged in water is still surrounded by water on both sides
//    of the shade).  Despite that, TranslucentSPF::Scatter /
//    ScatterNM's three entry sites (the isotropic-N branch, the
//    per-RGB-channel loop for an anisotropic N, and the NM twin)
//    each did `trans.ior_stack->push( 1.0 )` -- a literal, unconditional
//    push of air's IOR, regardless of what medium the ray actually
//    arrived from.
//
//    `RISE::RadianceEtaScale` (Utilities/IORStack.h, debt 30) prices
//    every RADIANCE-mode medium change as `(eta_before/eta_after)^2`.
//    For a translucent object floating in water (eta 1.33) fed the
//    fabricated push(1.0), that reads the entry as a 1.33 -> 1.0
//    transition -- a 1.7689x throughput inflation on every translucent
//    object nested in any non-air medium -- when the physically
//    correct answer is "no transition at all" (scale exactly 1,
//    because translucent_material carries no IOR of its own).
//
//    Fix: push `ior_stack.top()` (the CURRENT medium) instead of the
//    literal `1.0`.  RadianceEtaScale then sees before==after and
//    returns exactly 1.  In air (top() == 1.0) this is bit-identical
//    to the old code, which is why the bug shipped unnoticed -- see
//    docs/REFRACTIVE_RADIANCE_SCALING.md section 2's "double
//    cancellation" writeup for the general shape of this trap.
//
//  RED-PROOF (recorded from a run against the pre-fix library; see the
//    fix commit message for the verbatim failing output)
//
//    Every "entry" assertion below failed with scale ~= 1.7689 (== n^2
//    at ior 1.33) instead of 1.0 before the fix.  The "exit" assertions
//    passed unmodified in both cases only when chained from a CORRECT
//    entry stack; chained from the pre-fix (buggy) entry stack, the
//    exit assertion ALSO failed, at scale ~= 0.565 (== 1/n^2) -- the
//    reciprocal error, because pop() always correctly reveals the
//    medium below the popped entry, so a wrong push shows up again,
//    inverted, the next time the object's own exit is priced against
//    the stack the buggy entry produced.
//
//  COVERAGE
//
//    Sub-test 1 -- isotropic Phong N (UniformScalarPainter): exercises
//      the single-ray RGB entry push (TranslucentSPF.cpp ~line 133)
//      and its matching exit pop (~line 238), chained so the exit
//      call's input stack is literally the entry call's output stack.
//    Sub-test 2 -- anisotropic Phong N (RGBScalarPainter, R != G != B):
//      exercises the per-channel-loop RGB entry push (~line 154, three
//      pushes per Scatter call) the same way.  It also red-proofs an
//      incidental bug found alongside debt 30 in the same loop: entry-side
//      `trans.kray[0] = p[0]` (TranslucentSPF.cpp ~line 154) wrote channel
//      0 on every iteration instead of `trans.kray[i] = p[i]`, so summed
//      across the three emitted rays, kray channels 1 and 2 were always
//      zero.  The fix mirrors the exit-side loop's `trans.kray[i] = p[i]`
//      (~line 222), which never had this bug.
//
//      Round 2 (review) found the EXIT side's sibling of that same bug:
//      the exit branch's per-channel loop (~line 230-231) did
//      `front.kray = 0; front.kray[i] = f[i]*(1-scat[i]);` INSIDE the
//      loop, but unlike `trans` (added to `scattered` once per iteration,
//      correctly), `front` -- the diffuse ray that actually leaves the
//      object -- is added ONCE, after the whole loop.  So every
//      iteration's `front.kray = 0` reset wiped out the previous
//      iteration's channel, leaving only the LAST channel (B, i=2)
//      non-zero on the ray that exits.  This sub-test's `scatFactor`
//      (0.3, uniform) and anisotropic `phongN` together force the exit
//      branch into the same per-channel loop, and assert the exit ray's
//      `kray` has all three channels equal to `f*(1-scat)` -- pre-fix,
//      channels 0 and 1 read exactly 0 (see the fix commit message for
//      the verbatim failing lines from this round's red-proof run).
//    Sub-test 3 -- ScatterNM: exercises the NM twin's single entry
//      push (~line 304) and its exit pop (~line 368).  ScatterNM has no
//      per-channel loop (it operates on one wavelength at a time), so
//      it carries no sibling of either RGB-side bug above.
//
//    Round 3 (fix worker, debt 30 review round 3) added two more:
//
//    Sub-test 2 (extended) -- C1: the entry-side per-channel loop
//      (~line 148-171) reuses ONE `trans` local across all three
//      iterations and `new`s a fresh `IORStack` each time.
//      `ScatteredRayContainer::AddScatteredRay` clears `delete_stack` to
//      false on the CALLER's local only AFTER a successful memcpy -- it
//      never touches the value memcpy'd INTO the stored copy.  Pre-fix,
//      only the first iteration's stored copy owned its stack;
//      iterations 2 and 3 stored copies with `delete_stack==false`
//      (carried over from iteration 1's post-add reset) paired with a
//      brand new allocation nothing else references -- two leaked
//      `IORStack`s per anisotropic-N entry `Scatter` call, on the
//      SUCCESS path (this is not an overflow case; see debt 31(b) in
//      docs/RENDERING_INTEGRATORS.md for the separate overflow-only
//      leak in the same shape).  Fix: re-arm `trans.delete_stack = true`
//      immediately before each `new IORStack`, so every stored copy
//      independently owns what it was given.  Guarded by asserting
//      `delete_stack == true` on every stored ray with a non-null
//      `ior_stack` in the entry loop.
//
//    Sub-test 4 -- C2: `Scatter`'s entry branch gated the front
//      (reflection) lobe on `front.kray[0] > 0` and the translucent lobe
//      on `trans.kray[0] > 0` -- channel 0 (R) alone, so a
//      reflectance/transmittance painter with zero red and non-zero
//      green/blue (e.g. (0, 0.5, 0.5)) emitted NEITHER lobe.  The exit
//      branch's equivalent gate already used
//      `ColorMath::MaxValue(front.kray) > 0`.  Fixed to match.
//
//    In every sub-test, EVERY scattered ray with a non-null ior_stack
//    (entry: the eRayTranslucent lobe; exit: the eRayDiffuse lobe that
//    actually leaves the object -- the exit's own possible
//    eRayTranslucent back-scatter lobe deliberately keeps ior_stack
//    null, "stays inside", and is skipped by the `if (!after) return 1`
//    branch of RadianceEtaScale itself) must read
//    RadianceEtaScale(stack-at-that-call, ray.ior_stack) == 1 exactly.
//
//  Build (from project root):
//    c++ -arch arm64 -Isrc/Library -I/opt/homebrew/include
//        -O3 -ffast-math -fno-finite-math-only -funroll-loops -Wall -pedantic
//        -Wno-c++11-long-long -DCOLORS_RGB -DMERSENNE53
//        -DNO_TIFF_SUPPORT -DNO_EXR_SUPPORT -DRISE_ENABLE_MAILBOXING
//        -c tests/TranslucentIORStackTest.cpp -o tests/TranslucentIORStackTest.o
//    c++ -arch arm64 -o bin/tests/TranslucentIORStackTest
//        tests/TranslucentIORStackTest.o bin/librise.a -L/opt/homebrew/lib -lpng -lz
//
//  Author: Aravind Krishnaswamy (RISE debt 30, fix worker round 1)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>

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
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Materials/TranslucentSPF.h"

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

static int failed = 0;

#define EXPECT( cond, msg ) do { \
	if( !(cond) ) { \
		std::cout << "FAIL: " << __FILE__ << ":" << __LINE__ << " " << msg << std::endl; \
		failed++; \
	} \
} while(0)

#define EXPECT_NEAR( a, b, tol, msg ) do { \
	if( std::fabs((double)(a) - (double)(b)) > (tol) ) { \
		std::cout << "FAIL: " << __FILE__ << ":" << __LINE__ << " " << msg \
			<< " (got " << (a) << ", expected " << (b) << ")" << std::endl; \
		failed++; \
	} else { \
		std::cout << "  ok: " << msg << " = " << (a) << std::endl; \
	} \
} while(0)

// Water's IOR, matching the docs/REFRACTIVE_RADIANCE_SCALING.md worked
// example throughout.
static const Scalar kWaterIOR = 1.33;

// A synthetic intersection: incoming ray travels straight down (-Z) and
// hits a surface at the origin whose shading normal is +Z.  Mirrors
// tests/SPFPdfConsistencyTest.cpp's MakeIntersection helper.
static RayIntersectionGeometric MakeIntersection()
{
	Ray inRay( Point3(0, 0, 1.0), Vector3(0, 0, -1.0) );
	RasterizerState rs = {0, 0};
	RayIntersectionGeometric ri( inRay, rs );

	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = Point3(0, 0, 0);
	ri.vNormal = Vector3(0, 0, 1);
	ri.onb.CreateFromW( Vector3(0, 0, 1) );
	ri.ptCoord = Point2(0.5, 0.5);
	// ri.vGeomNormal is left at its default (zero vector), which is the
	// production convention for geometry that doesn't populate a
	// separate geometric normal -- TranslucentSPF falls back to
	// ri.onb.w() in that case (see the SquaredModulus gate at the top
	// of Scatter/ScatterNM).

	return ri;
}

// Builds an IORStack whose top is `mediumIOR`, with `pTransObj` NOT yet
// on the stack (so containsCurrent() is false once pTransObj is made
// current) -- i.e. the state immediately BEFORE the ray enters the
// translucent object, having already been travelling through some
// enclosing medium (water, most commonly).
static IORStack MakeEnteringStack( const IObject* pWaterObj, const IObject* pTransObj, Scalar mediumIOR )
{
	IORStack stack( 1.0 );					// environment = air
	stack.SetCurrentObject( pWaterObj );
	stack.push( mediumIOR );				// now inside the enclosing medium
	stack.SetCurrentObject( pTransObj );	// about to hit the translucent object
	return stack;
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 1: isotropic Phong N -- single-ray RGB entry/exit push
//////////////////////////////////////////////////////////////////////
static void TestIsotropicRGB()
{
	std::cout << "Sub-test 1: isotropic RGB (single-ray entry/exit)" << std::endl;

	UniformColorPainter* front = new UniformColorPainter( RISEPel(0.2, 0.2, 0.2) );  front->addref();
	UniformColorPainter* trans = new UniformColorPainter( RISEPel(0.4, 0.4, 0.4) );  trans->addref();
	UniformScalarPainter* extinction = new UniformScalarPainter( 0.1 );  extinction->addref();
	UniformScalarPainter* phongN     = new UniformScalarPainter( 10.0 ); phongN->addref();
	UniformScalarPainter* scatFactor = new UniformScalarPainter( 0.3 );  scatFactor->addref();

	TranslucentSPF* spf = new TranslucentSPF( *front, *trans, *extinction, *phongN, *scatFactor );
	spf->addref();

	StubObject* waterObj = new StubObject();  waterObj->addref();
	StubObject* transObj = new StubObject();  transObj->addref();

	RayIntersectionGeometric ri = MakeIntersection();
	RandomNumberGenerator rng;
	IndependentSampler sampler( rng );

	// --- Entry ---
	IORStack entryStack = MakeEnteringStack( waterObj, transObj, kWaterIOR );
	EXPECT( !entryStack.containsCurrent(), "iso: entry stack does not yet contain the translucent object" );

	ScatteredRayContainer entryRays;
	spf->Scatter( ri, sampler, entryRays, entryStack );

	int entryTranslucentChecked = 0;
	IORStack* exitInput = 0;
	for( unsigned int i = 0; i < entryRays.Count(); i++ ) {
		if( entryRays[i].ior_stack != 0 ) {
			EXPECT( entryRays[i].type == ScatteredRay::eRayTranslucent, "iso: entry ray with a non-null ior_stack is the translucent lobe" );
			const Scalar scale = RadianceEtaScale( entryStack, entryRays[i].ior_stack );
			EXPECT_NEAR( scale, 1.0, 1e-9, "iso RGB entry RadianceEtaScale" );
			entryTranslucentChecked++;
			if( !exitInput ) {
				exitInput = new IORStack( *entryRays[i].ior_stack );
			}
		}
	}
	EXPECT( entryTranslucentChecked == 1, "iso: exactly one translucent entry lobe carried a stack" );
	EXPECT( exitInput != 0, "iso: captured an entry output stack to chain into the exit call" );

	// --- Exit, chained from the entry call's own output stack ---
	exitInput->SetCurrentObject( transObj );
	EXPECT( exitInput->containsCurrent(), "iso: exit input stack (the entry's output) contains the translucent object" );

	ScatteredRayContainer exitRays;
	spf->Scatter( ri, sampler, exitRays, *exitInput );

	int exitStackedChecked = 0;
	for( unsigned int i = 0; i < exitRays.Count(); i++ ) {
		if( exitRays[i].ior_stack != 0 ) {
			const Scalar scale = RadianceEtaScale( *exitInput, exitRays[i].ior_stack );
			EXPECT_NEAR( scale, 1.0, 1e-9, "iso RGB exit RadianceEtaScale" );
			exitStackedChecked++;
		}
	}
	EXPECT( exitStackedChecked == 1, "iso: exactly one exit lobe (the diffuse exit ray) carried a stack" );

	delete exitInput;
	spf->release();
	transObj->release();
	waterObj->release();
	scatFactor->release();
	phongN->release();
	extinction->release();
	trans->release();
	front->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 2: anisotropic Phong N -- per-channel-loop RGB entry/exit
//////////////////////////////////////////////////////////////////////
static void TestAnisotropicRGB()
{
	std::cout << "Sub-test 2: anisotropic RGB (per-channel-loop entry/exit)" << std::endl;

	UniformColorPainter* front = new UniformColorPainter( RISEPel(0.2, 0.2, 0.2) );  front->addref();
	UniformColorPainter* trans = new UniformColorPainter( RISEPel(0.4, 0.4, 0.4) );  trans->addref();
	UniformScalarPainter* extinction = new UniformScalarPainter( 0.1 );  extinction->addref();
	// R != G != B forces TranslucentSPF's per-channel loop branch
	// (Scatter/ScatterNM's `(Nfactor[0]==Nfactor[1]) && (Nfactor[1]==Nfactor[2])`
	// gate), which is a second, textually distinct push(1.0) site.
	RGBScalarPainter* phongN = new RGBScalarPainter( 5.0, 10.0, 15.0 );  phongN->addref();
	UniformScalarPainter* scatFactor = new UniformScalarPainter( 0.3 );  scatFactor->addref();

	TranslucentSPF* spf = new TranslucentSPF( *front, *trans, *extinction, *phongN, *scatFactor );
	spf->addref();

	StubObject* waterObj = new StubObject();  waterObj->addref();
	StubObject* transObj = new StubObject();  transObj->addref();

	RayIntersectionGeometric ri = MakeIntersection();
	RandomNumberGenerator rng;
	IndependentSampler sampler( rng );

	// --- Entry ---
	IORStack entryStack = MakeEnteringStack( waterObj, transObj, kWaterIOR );
	EXPECT( !entryStack.containsCurrent(), "aniso: entry stack does not yet contain the translucent object" );

	ScatteredRayContainer entryRays;
	spf->Scatter( ri, sampler, entryRays, entryStack );

	int entryTranslucentChecked = 0;
	IORStack* exitInput = 0;
	// RED-PROOF for the incidental per-channel `kray` bug found alongside
	// debt 30 (TranslucentSPF.cpp ~line 154, entry-side per-channel loop):
	// each of the three emitted eRayTranslucent rays should carry ONE
	// live channel (trans.kray[i] = p[i], the rest left at the loop's
	// `trans.kray = 0` reset), so summing kray across all three rays must
	// reconstruct the untouched `pTrans->GetColor(ri)` value exactly --
	// each channel contributed by exactly one ray.  Before the fix, every
	// iteration wrote `trans.kray[0] = p[0]` regardless of `i`, so the
	// sum's channels 1 and 2 stayed at zero (dead lobes) while channel 0
	// was triple-counted.
	RISEPel krayEntrySum(0,0,0);
	for( unsigned int i = 0; i < entryRays.Count(); i++ ) {
		if( entryRays[i].ior_stack != 0 ) {
			EXPECT( entryRays[i].type == ScatteredRay::eRayTranslucent, "aniso: entry ray with a non-null ior_stack is the translucent lobe" );
			const Scalar scale = RadianceEtaScale( entryStack, entryRays[i].ior_stack );
			EXPECT_NEAR( scale, 1.0, 1e-9, "aniso RGB entry RadianceEtaScale (per-channel loop)" );
			// RED-PROOF for C1 (review round 3): the per-channel loop reuses
			// ONE `trans` local across all three iterations and allocates a
			// FRESH `IORStack` each time.  `AddScatteredRay` clears
			// `delete_stack` to false on the CALLER's local only AFTER a
			// successful memcpy -- it never touches the value that gets
			// copied INTO the stored ray, which is whatever `delete_stack`
			// reads at call time.  Pre-fix, only i==0 (the freshly
			// constructed `trans`, delete_stack==true from the ctor) stored
			// a copy that owns its stack; i==1 and i==2 stored copies with
			// delete_stack==false (carried over from i==0's post-add reset)
			// paired with a BRAND NEW allocation only that copy references
			// -- two leaked `IORStack`s per call.  Every stored ray with a
			// non-null `ior_stack` must independently own it.
			EXPECT( entryRays[i].delete_stack == true, "aniso: entry ray with a non-null ior_stack owns its stack (delete_stack==true) -- C1 leak guard" );
			entryTranslucentChecked++;
			krayEntrySum[0] += entryRays[i].kray[0];
			krayEntrySum[1] += entryRays[i].kray[1];
			krayEntrySum[2] += entryRays[i].kray[2];
			if( !exitInput ) {
				exitInput = new IORStack( *entryRays[i].ior_stack );
			}
		}
	}
	// The per-channel loop emits exactly one translucent ray per color
	// channel (three), unconditionally.
	EXPECT( entryTranslucentChecked == 3, "aniso: three translucent entry lobes (one per color channel) carried a stack" );
	EXPECT( exitInput != 0, "aniso: captured an entry output stack to chain into the exit call" );

	// pTrans is UniformColorPainter(0.4, 0.4, 0.4) -- the value the
	// per-channel loop reads into `p` before zeroing trans.kray.
	const RISEPel expectedKrayEntrySum(0.4, 0.4, 0.4);
	EXPECT( krayEntrySum[0] > 0, "aniso: entry kray sum channel 0 (R) is non-zero" );
	EXPECT( krayEntrySum[1] > 0, "aniso: entry kray sum channel 1 (G) is non-zero -- catches trans.kray[0]=p[0] typo" );
	EXPECT( krayEntrySum[2] > 0, "aniso: entry kray sum channel 2 (B) is non-zero -- catches trans.kray[0]=p[0] typo" );
	EXPECT_NEAR( krayEntrySum[0], expectedKrayEntrySum[0], 1e-9, "aniso: entry kray sum channel 0 (R) matches single-branch kray" );
	EXPECT_NEAR( krayEntrySum[1], expectedKrayEntrySum[1], 1e-9, "aniso: entry kray sum channel 1 (G) matches single-branch kray" );
	EXPECT_NEAR( krayEntrySum[2], expectedKrayEntrySum[2], 1e-9, "aniso: entry kray sum channel 2 (B) matches single-branch kray" );

	// --- Exit, chained from one of the entry call's own output stacks ---
	exitInput->SetCurrentObject( transObj );
	EXPECT( exitInput->containsCurrent(), "aniso: exit input stack (an entry output) contains the translucent object" );

	ScatteredRayContainer exitRays;
	spf->Scatter( ri, sampler, exitRays, *exitInput );

	// RED-PROOF for the sibling exit-side `kray` bug (TranslucentSPF.cpp
	// ~line 230-231, the exit branch's per-channel loop; fixed alongside
	// entry-side sub-test 2's bug in this round).  `scatFactor` (0.3,
	// uniform) makes the exit branch's "multiple scatter back" arm run,
	// and the anisotropic `phongN` (5, 10, 15) forces its per-channel
	// loop -- same gate as the entry side, mirrored on the exit path.
	// That loop does `front.kray = 0; front.kray[i] = f[i]*(1-scat[i]);`
	// INSIDE the per-channel iteration, but `front` (the exit diffuse
	// ray) is added to `scattered` only ONCE, after the loop -- so the
	// reset on every iteration except the last (i=2) wiped channels 0
	// and 1 back to zero on the ray that actually leaves the object.
	// Fixed: zero `front.kray` once, before the loop, and only assign
	// (never re-zero) the i'th channel inside it.
	//
	// Expected value: extinction 0.1 over distance 1 (ray origin (0,0,1)
	// to hit point origin) gives `front.kray` (before scattering) =
	// exp(-0.1) on every channel; the multi-scatter arm's `scat` is the
	// uniform 0.3 painter (channel-flat even though N is not), so the
	// exit ray's expected kray is exp(-0.1)*(1-0.3) on ALL THREE
	// channels equally -- the buggy code left channels 0 and 1 at
	// exactly 0 instead.
	const Scalar expectedExitKray = std::exp( -1.0 * 0.1 ) * ( 1.0 - 0.3 );
	RISEPel exitFrontKray(0,0,0);
	int exitFrontKrayCaptured = 0;

	int exitStackedChecked = 0;
	for( unsigned int i = 0; i < exitRays.Count(); i++ ) {
		if( exitRays[i].ior_stack != 0 ) {
			const Scalar scale = RadianceEtaScale( *exitInput, exitRays[i].ior_stack );
			EXPECT_NEAR( scale, 1.0, 1e-9, "aniso RGB exit RadianceEtaScale" );
			exitStackedChecked++;
			exitFrontKray = exitRays[i].kray;
			exitFrontKrayCaptured++;
		}
	}
	EXPECT( exitStackedChecked == 1, "aniso: exactly one exit lobe (the diffuse exit ray) carried a stack" );
	EXPECT( exitFrontKrayCaptured == 1, "aniso: captured the exit diffuse ray's kray" );
	EXPECT( exitFrontKray[0] > 0, "aniso: exit front kray channel 0 (R) is non-zero -- catches the per-iteration front.kray=0 reset bug" );
	EXPECT( exitFrontKray[1] > 0, "aniso: exit front kray channel 1 (G) is non-zero -- catches the per-iteration front.kray=0 reset bug" );
	EXPECT( exitFrontKray[2] > 0, "aniso: exit front kray channel 2 (B) is non-zero -- catches the per-iteration front.kray=0 reset bug" );
	EXPECT_NEAR( exitFrontKray[0], expectedExitKray, 1e-9, "aniso: exit front kray channel 0 (R) matches f*(1-scat)" );
	EXPECT_NEAR( exitFrontKray[1], expectedExitKray, 1e-9, "aniso: exit front kray channel 1 (G) matches f*(1-scat)" );
	EXPECT_NEAR( exitFrontKray[2], expectedExitKray, 1e-9, "aniso: exit front kray channel 2 (B) matches f*(1-scat)" );

	delete exitInput;
	spf->release();
	transObj->release();
	waterObj->release();
	scatFactor->release();
	phongN->release();
	extinction->release();
	trans->release();
	front->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 3: ScatterNM -- the spectral twin's single entry/exit push
//////////////////////////////////////////////////////////////////////
static void TestScatterNM()
{
	std::cout << "Sub-test 3: ScatterNM (spectral twin, entry/exit)" << std::endl;

	UniformColorPainter* front = new UniformColorPainter( RISEPel(0.2, 0.2, 0.2) );  front->addref();
	UniformColorPainter* trans = new UniformColorPainter( RISEPel(0.4, 0.4, 0.4) );  trans->addref();
	UniformScalarPainter* extinction = new UniformScalarPainter( 0.1 );  extinction->addref();
	UniformScalarPainter* phongN     = new UniformScalarPainter( 10.0 ); phongN->addref();
	UniformScalarPainter* scatFactor = new UniformScalarPainter( 0.3 );  scatFactor->addref();

	TranslucentSPF* spf = new TranslucentSPF( *front, *trans, *extinction, *phongN, *scatFactor );
	spf->addref();

	StubObject* waterObj = new StubObject();  waterObj->addref();
	StubObject* transObj = new StubObject();  transObj->addref();

	RayIntersectionGeometric ri = MakeIntersection();
	RandomNumberGenerator rng;
	IndependentSampler sampler( rng );
	const Scalar nm = 550.0;

	// --- Entry ---
	IORStack entryStack = MakeEnteringStack( waterObj, transObj, kWaterIOR );
	EXPECT( !entryStack.containsCurrent(), "NM: entry stack does not yet contain the translucent object" );

	ScatteredRayContainer entryRays;
	spf->ScatterNM( ri, sampler, nm, entryRays, entryStack );

	int entryTranslucentChecked = 0;
	IORStack* exitInput = 0;
	for( unsigned int i = 0; i < entryRays.Count(); i++ ) {
		if( entryRays[i].ior_stack != 0 ) {
			EXPECT( entryRays[i].type == ScatteredRay::eRayTranslucent, "NM: entry ray with a non-null ior_stack is the translucent lobe" );
			const Scalar scale = RadianceEtaScale( entryStack, entryRays[i].ior_stack );
			EXPECT_NEAR( scale, 1.0, 1e-9, "NM entry RadianceEtaScale" );
			entryTranslucentChecked++;
			if( !exitInput ) {
				exitInput = new IORStack( *entryRays[i].ior_stack );
			}
		}
	}
	EXPECT( entryTranslucentChecked == 1, "NM: exactly one translucent entry lobe carried a stack" );
	EXPECT( exitInput != 0, "NM: captured an entry output stack to chain into the exit call" );

	// --- Exit, chained from the entry call's own output stack ---
	exitInput->SetCurrentObject( transObj );
	EXPECT( exitInput->containsCurrent(), "NM: exit input stack (the entry's output) contains the translucent object" );

	ScatteredRayContainer exitRays;
	spf->ScatterNM( ri, sampler, nm, exitRays, *exitInput );

	int exitStackedChecked = 0;
	for( unsigned int i = 0; i < exitRays.Count(); i++ ) {
		if( exitRays[i].ior_stack != 0 ) {
			const Scalar scale = RadianceEtaScale( *exitInput, exitRays[i].ior_stack );
			EXPECT_NEAR( scale, 1.0, 1e-9, "NM exit RadianceEtaScale" );
			exitStackedChecked++;
		}
	}
	EXPECT( exitStackedChecked == 1, "NM: exactly one exit lobe (the diffuse exit ray) carried a stack" );

	delete exitInput;
	spf->release();
	transObj->release();
	waterObj->release();
	scatFactor->release();
	phongN->release();
	extinction->release();
	trans->release();
	front->release();
}

//////////////////////////////////////////////////////////////////////
//  Sub-test 4: channel-0-only lobe gates (review round 3, C2)
//
//  `Scatter`'s entry branch gated the front (reflection) lobe on
//  `front.kray[0] > 0` and the translucent lobe on `trans.kray[0] > 0`
//  -- channel 0 (R) alone.  A reflectance/transmittance painter with
//  zero red and non-zero green/blue, e.g. (0, 0.5, 0.5), read both
//  gates as false and emitted NEITHER lobe at all: not a partial
//  channel loss, a total one, since both gates guard the entire `if`
//  block that samples and adds the ray.  The exit branch's equivalent
//  gate (TranslucentSPF.cpp ~line 182) already used
//  `ColorMath::MaxValue(front.kray) > 0`, so this was an entry-side-only
//  asymmetry.  Fix: both entry gates now use MaxValue, matching the
//  exit branch.
//////////////////////////////////////////////////////////////////////
static void TestChannelZeroGate()
{
	std::cout << "Sub-test 4: channel-0-only lobe gate (front.kray[0]>0 / trans.kray[0]>0)" << std::endl;

	// Zero in channel 0 (R), non-zero in channels 1/2 (G, B) -- isotropic N
	// so both painters exercise the single-ray branch (the loop's own
	// per-channel bug is C1, covered by sub-test 2).
	UniformColorPainter* front = new UniformColorPainter( RISEPel(0.0, 0.5, 0.5) );  front->addref();
	UniformColorPainter* trans = new UniformColorPainter( RISEPel(0.0, 0.5, 0.5) );  trans->addref();
	UniformScalarPainter* extinction = new UniformScalarPainter( 0.1 );  extinction->addref();
	UniformScalarPainter* phongN     = new UniformScalarPainter( 10.0 ); phongN->addref();
	UniformScalarPainter* scatFactor = new UniformScalarPainter( 0.3 );  scatFactor->addref();

	TranslucentSPF* spf = new TranslucentSPF( *front, *trans, *extinction, *phongN, *scatFactor );
	spf->addref();

	StubObject* waterObj = new StubObject();  waterObj->addref();
	StubObject* transObj = new StubObject();  transObj->addref();

	RayIntersectionGeometric ri = MakeIntersection();
	RandomNumberGenerator rng;
	IndependentSampler sampler( rng );

	IORStack entryStack = MakeEnteringStack( waterObj, transObj, kWaterIOR );
	ScatteredRayContainer entryRays;
	spf->Scatter( ri, sampler, entryRays, entryStack );

	bool sawFront = false, sawTrans = false;
	for( unsigned int i = 0; i < entryRays.Count(); i++ ) {
		if( entryRays[i].type == ScatteredRay::eRayDiffuse ) {
			sawFront = true;
			EXPECT( entryRays[i].kray[1] > 0, "channel-gate: front lobe channel 1 (G) non-zero" );
			EXPECT( entryRays[i].kray[2] > 0, "channel-gate: front lobe channel 2 (B) non-zero" );
		}
		if( entryRays[i].type == ScatteredRay::eRayTranslucent ) {
			sawTrans = true;
			EXPECT( entryRays[i].kray[1] > 0, "channel-gate: translucent lobe channel 1 (G) non-zero" );
			EXPECT( entryRays[i].kray[2] > 0, "channel-gate: translucent lobe channel 2 (B) non-zero" );
		}
	}
	EXPECT( sawFront, "channel-gate: front (reflection) lobe was emitted despite kray[0]==0 -- catches the channel-0-only gate" );
	EXPECT( sawTrans, "channel-gate: translucent lobe was emitted despite kray[0]==0 -- catches the channel-0-only gate" );

	spf->release();
	transObj->release();
	waterObj->release();
	scatFactor->release();
	phongN->release();
	extinction->release();
	trans->release();
	front->release();
}

#include "TranslucentGuidedStackProbe.h"

int main()
{
	GlobalLog();

	TestIsotropicRGB();
	TestAnisotropicRGB();
	TestScatterNM();
	TestChannelZeroGate();
	GuidedStackProbe::Run();

	std::cout << std::endl;
	if( failed == 0 ) {
		std::cout << "ALL TESTS PASSED" << std::endl;
		return 0;
	} else {
		std::cout << failed << " CHECK(S) FAILED" << std::endl;
		return 1;
	}
}
