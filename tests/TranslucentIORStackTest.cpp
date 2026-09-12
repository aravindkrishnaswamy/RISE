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
//      pushes per Scatter call) the same way.
//    Sub-test 3 -- ScatterNM: exercises the NM twin's single entry
//      push (~line 304) and its exit pop (~line 368).
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
	for( unsigned int i = 0; i < entryRays.Count(); i++ ) {
		if( entryRays[i].ior_stack != 0 ) {
			EXPECT( entryRays[i].type == ScatteredRay::eRayTranslucent, "aniso: entry ray with a non-null ior_stack is the translucent lobe" );
			const Scalar scale = RadianceEtaScale( entryStack, entryRays[i].ior_stack );
			EXPECT_NEAR( scale, 1.0, 1e-9, "aniso RGB entry RadianceEtaScale (per-channel loop)" );
			entryTranslucentChecked++;
			if( !exitInput ) {
				exitInput = new IORStack( *entryRays[i].ior_stack );
			}
		}
	}
	// The per-channel loop emits exactly one translucent ray per color
	// channel (three), unconditionally.
	EXPECT( entryTranslucentChecked == 3, "aniso: three translucent entry lobes (one per color channel) carried a stack" );
	EXPECT( exitInput != 0, "aniso: captured an entry output stack to chain into the exit call" );

	// --- Exit, chained from one of the entry call's own output stacks ---
	exitInput->SetCurrentObject( transObj );
	EXPECT( exitInput->containsCurrent(), "aniso: exit input stack (an entry output) contains the translucent object" );

	ScatteredRayContainer exitRays;
	spf->Scatter( ri, sampler, exitRays, *exitInput );

	int exitStackedChecked = 0;
	for( unsigned int i = 0; i < exitRays.Count(); i++ ) {
		if( exitRays[i].ior_stack != 0 ) {
			const Scalar scale = RadianceEtaScale( *exitInput, exitRays[i].ior_stack );
			EXPECT_NEAR( scale, 1.0, 1e-9, "aniso RGB exit RadianceEtaScale" );
			exitStackedChecked++;
		}
	}
	EXPECT( exitStackedChecked == 1, "aniso: exactly one exit lobe (the diffuse exit ray) carried a stack" );

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

int main()
{
	GlobalLog();

	TestIsotropicRGB();
	TestAnisotropicRGB();
	TestScatterNM();

	std::cout << std::endl;
	if( failed == 0 ) {
		std::cout << "ALL TESTS PASSED" << std::endl;
		return 0;
	} else {
		std::cout << failed << " CHECK(S) FAILED" << std::endl;
		return 1;
	}
}
