//////////////////////////////////////////////////////////////////////
//
//  IORStackBehaviorTest.cpp - Locks in IORStack semantics across the
//    multi-volume scene topologies that surface every known edge case.
//
//    This is a regression guard, not an aspirational spec.  Each
//    scenario asserts the CURRENT behavior — including one documented
//    limitation (Scenario E) that IORStack.h's file-header call out.
//    If any of these assertions start firing, it means someone
//    changed the IORStack semantics; whoever makes the change is
//    responsible for updating this test alongside the code.
//
//    Scenarios covered:
//      A. Single closed glass sphere (canonical watertight case)
//         — CORRECT today; test asserts correctness
//      B. Nested different-material volumes (glass containing water)
//         — CORRECT today; test asserts correctness
//      C. Concentric same-material volumes (outer + inner both glass)
//         — CORRECT today; test asserts correctness
//      D. Multi-object same-medium slab (slab-from-planes)
//         — locally correct optically (Ni == Nt gives no refraction);
//           stack IS polluted, test documents that fact
//      E. Same-medium slab followed by a downstream refractor
//         — DOCUMENTED LIMITATION: stack pollution from D leaks the
//           slab's IOR into the downstream refractor's outer IOR.
//           Test ASSERTS the buggy value so that whoever lifts this
//           limitation in the future will see the assertion fire and
//           update the expectation to 1.0 (air).
//      F. Two disjoint objects same material (ray in-out-in-out)
//         — CORRECT today; test asserts correctness
//
//    These tests don't render anything; they exercise IORStack
//    push/pop/containsCurrent/top directly with stub object pointers
//    (distinct integer addresses cast to IObject*, never dereferenced)
//    to isolate the data-structure semantics from the rest of the
//    rendering pipeline.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>
#include <cassert>
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Interfaces/IObject.h"

using namespace RISE;

static int failed = 0;

#define EXPECT( cond, msg ) do { \
	if( !(cond) ) { \
		std::cout << "FAIL: " << __FILE__ << ":" << __LINE__ << " " << msg << std::endl; \
		failed++; \
	} \
} while(0)

#define EXPECT_NEAR( a, b, tol, msg ) do { \
	if( std::fabs((a) - (b)) > (tol) ) { \
		std::cout << "FAIL: " << __FILE__ << ":" << __LINE__ << " " << msg \
			<< " (got " << (a) << ", expected " << (b) << ")" << std::endl; \
		failed++; \
	} \
} while(0)

// IORStack keys on IObject* identity; we never dereference the pointers,
// so for unit-testing we cast distinct integer addresses to IObject*.
// Two distinct constant addresses give us two distinct "objects" without
// needing a real Object implementation.
static const IObject* FakeObj( uintptr_t id )
{
	return reinterpret_cast<const IObject*>( id );
}

// Simulate an SPF refraction step: given the current stack and the object
// being hit, returns (bEntering, outerIOR, innerIOR) as the PerfectRefractor
// SPF computes them.  Mutates the stack per the same logic.
struct RefractStep
{
	bool		bEntering;
	Scalar		Ni;	// "from" IOR passed to Optics
	Scalar		Nt;	// "to" IOR passed to Optics
};

static RefractStep StepEnterOrExit(
	IORStack& stack,
	const IObject* obj,
	Scalar matIOR
	)
{
	RefractStep r;
	stack.SetCurrentObject( obj );
	r.bEntering = !stack.containsCurrent();
	if( r.bEntering ) {
		r.Ni = stack.top();
		r.Nt = matIOR;
		stack.push( matIOR );
	} else {
		const Scalar innerIOR = matIOR;
		// Mirror the SPF: copy stack, pop, read new top for Nt
		IORStack afterPop( stack );
		afterPop.SetCurrentObject( obj );
		afterPop.pop();
		r.Ni = innerIOR;
		r.Nt = afterPop.top();
		stack.pop();	// real stack pop
	}
	return r;
}

//////////////////////////////////////////////////////////////////////

static void ScenarioA_SingleClosedSphere()
{
	std::cout << "A: Single closed glass sphere" << std::endl;
	IORStack stack( 1.0 );
	const IObject* glass = FakeObj( 0x1000 );

	auto step1 = StepEnterOrExit( stack, glass, 1.5 );	// Enter front
	EXPECT( step1.bEntering, "front: bEntering" );
	EXPECT_NEAR( step1.Ni, 1.0, 1e-12, "front: Ni=air" );
	EXPECT_NEAR( step1.Nt, 1.5, 1e-12, "front: Nt=glass" );

	auto step2 = StepEnterOrExit( stack, glass, 1.5 );	// Exit back
	EXPECT( !step2.bEntering, "back: bEntering=false" );
	EXPECT_NEAR( step2.Ni, 1.5, 1e-12, "back: Ni=glass" );
	EXPECT_NEAR( step2.Nt, 1.0, 1e-12, "back: Nt=air" );
}

static void ScenarioB_NestedDifferentMaterial()
{
	std::cout << "B: Nested different materials (glass sphere containing water sphere)" << std::endl;
	IORStack stack( 1.0 );
	const IObject* glass = FakeObj( 0x1000 );
	const IObject* water = FakeObj( 0x2000 );

	auto a = StepEnterOrExit( stack, glass, 1.5 );	// Enter outer glass
	EXPECT_NEAR( a.Nt, 1.5, 1e-12, "glass.front Nt" );

	auto b = StepEnterOrExit( stack, water, 1.33 );	// Enter inner water
	EXPECT( b.bEntering, "water.front bEntering" );
	EXPECT_NEAR( b.Ni, 1.5, 1e-12, "water.front Ni=glass" );
	EXPECT_NEAR( b.Nt, 1.33, 1e-12, "water.front Nt=water" );

	auto c = StepEnterOrExit( stack, water, 1.33 );	// Exit inner water
	EXPECT( !c.bEntering, "water.back bEntering=false" );
	EXPECT_NEAR( c.Ni, 1.33, 1e-12, "water.back Ni=water" );
	EXPECT_NEAR( c.Nt, 1.5, 1e-12, "water.back Nt=glass (back inside outer!)" );

	auto d = StepEnterOrExit( stack, glass, 1.5 );	// Exit outer glass
	EXPECT( !d.bEntering, "glass.back bEntering=false" );
	EXPECT_NEAR( d.Ni, 1.5, 1e-12, "glass.back Ni=glass" );
	EXPECT_NEAR( d.Nt, 1.0, 1e-12, "glass.back Nt=air" );
}

static void ScenarioC_ConcentricSameMaterial()
{
	std::cout << "C: Concentric same-material (outer and inner both glass)" << std::endl;
	IORStack stack( 1.0 );
	const IObject* glassA = FakeObj( 0x1000 );
	const IObject* glassB = FakeObj( 0x2000 );

	auto a = StepEnterOrExit( stack, glassA, 1.5 );
	EXPECT_NEAR( a.Nt, 1.5, 1e-12, "outer.front Nt" );

	auto b = StepEnterOrExit( stack, glassB, 1.5 );
	EXPECT( b.bEntering, "inner.front bEntering (object keying)" );
	EXPECT_NEAR( b.Ni, 1.5, 1e-12, "inner.front Ni=glass" );
	EXPECT_NEAR( b.Nt, 1.5, 1e-12, "inner.front Nt=glass (no optical boundary)" );

	auto c = StepEnterOrExit( stack, glassB, 1.5 );
	EXPECT( !c.bEntering, "inner.back bEntering=false" );
	EXPECT_NEAR( c.Ni, 1.5, 1e-12, "inner.back Ni=glass" );
	EXPECT_NEAR( c.Nt, 1.5, 1e-12, "inner.back Nt=glass (still in outer)" );

	auto d = StepEnterOrExit( stack, glassA, 1.5 );
	EXPECT( !d.bEntering, "outer.back bEntering=false" );
	EXPECT_NEAR( d.Nt, 1.0, 1e-12, "outer.back Nt=air" );
}

static void ScenarioD_MultiObjectSameMediumSlab()
{
	std::cout << "D: Multi-object slab (two planes, same glass material) — reported bug" << std::endl;
	IORStack stack( 1.0 );
	const IObject* topPlane = FakeObj( 0x1000 );
	const IObject* botPlane = FakeObj( 0x2000 );

	auto a = StepEnterOrExit( stack, topPlane, 2.2 );
	EXPECT( a.bEntering, "top: bEntering" );
	EXPECT_NEAR( a.Nt, 2.2, 1e-12, "top Nt=glass" );

	auto b = StepEnterOrExit( stack, botPlane, 2.2 );
	// Expected: bEntering=false (we are already inside glass volume)
	// Actual: bEntering=true (object-key doesn't know these share a medium)
	// The optical result is correct by accident (Ni=Nt=2.2 gives no refraction)
	// but the stack state is wrong: [air, topPlane, botPlane] after.
	std::cout << "  bot: bEntering=" << b.bEntering
		<< " (stack-pollution: both planes now on stack)" << std::endl;
	// We do NOT assert bEntering here — both values produce the same
	// refraction math (Ni==Nt).  What we DO assert is the consequence
	// for a downstream refractor (Scenario E).
}

static void ScenarioE_SlabPollutionLeaksToNextRefractor()
{
	std::cout << "E: Slab pollution leaks into next refractor's Ni" << std::endl;
	IORStack stack( 1.0 );
	const IObject* topPlane = FakeObj( 0x1000 );
	const IObject* botPlane = FakeObj( 0x2000 );
	const IObject* nextGlassSphere = FakeObj( 0x3000 );

	StepEnterOrExit( stack, topPlane, 2.2 );	// glass slab top
	StepEnterOrExit( stack, botPlane, 2.2 );	// glass slab bot (stack pollution)

	// Now the ray has exited the slab into air but stack thinks
	// we are still inside the glass volume.
	auto c = StepEnterOrExit( stack, nextGlassSphere, 1.5 );
	std::cout << "  after-slab refractor: Ni=" << c.Ni
		<< " (CORRECT would be 1.0 air; ACTUAL is " << c.Ni << ")" << std::endl;

	// DOCUMENTED LIMITATION (see IORStack.h file-header comment):
	//   Physically-correct Ni for the downstream refractor after the
	//   slab is 1.0 (air, since slab-from-planes is just two
	//   interfaces in air with no real glass volume between them).
	//   Current object-pointer keying reports Ni = 2.2 instead,
	//   because the stack wrongly retains one entry per plane
	//   traversed.
	//
	// This assertion ASSERTS THE BUGGY VALUE ON PURPOSE, as a
	// regression guard.  If a future change to IORStack fixes the
	// limitation, this assertion will start failing — at which point
	// the fix author should flip the expected value to 1.0, delete
	// this comment block, and add a separate test (or amend Scenario
	// D) that asserts the stack ends up clean after a same-medium
	// slab.
	EXPECT_NEAR( c.Ni, 2.2, 1e-12,
		"DOCUMENTED LIMITATION: next refractor's outer IOR is polluted to glass; "
		"physically-correct value is air (1.0).  See IORStack.h file-header." );
}

static void ScenarioF_DisjointObjectsSameMaterial()
{
	std::cout << "F: Two disjoint spheres, same glass material (ray in-out-in-out)" << std::endl;
	IORStack stack( 1.0 );
	const IObject* glassA = FakeObj( 0x1000 );
	const IObject* glassB = FakeObj( 0x2000 );

	auto a = StepEnterOrExit( stack, glassA, 1.5 );
	EXPECT_NEAR( a.Nt, 1.5, 1e-12, "A.front Nt" );

	auto b = StepEnterOrExit( stack, glassA, 1.5 );
	EXPECT( !b.bEntering, "A.back bEntering=false" );
	EXPECT_NEAR( b.Nt, 1.0, 1e-12, "A.back Nt=air" );

	auto c = StepEnterOrExit( stack, glassB, 1.5 );
	EXPECT( c.bEntering, "B.front bEntering" );
	EXPECT_NEAR( c.Ni, 1.0, 1e-12, "B.front Ni=air (stack cleaned)" );

	auto d = StepEnterOrExit( stack, glassB, 1.5 );
	EXPECT( !d.bEntering, "B.back bEntering=false" );
	EXPECT_NEAR( d.Nt, 1.0, 1e-12, "B.back Nt=air" );
}

int main()
{
	std::cout << "=== IORStackBehaviorTest ===" << std::endl;

	ScenarioA_SingleClosedSphere();
	ScenarioB_NestedDifferentMaterial();
	ScenarioC_ConcentricSameMaterial();
	ScenarioD_MultiObjectSameMediumSlab();
	ScenarioE_SlabPollutionLeaksToNextRefractor();
	ScenarioF_DisjointObjectsSameMaterial();

	std::cout << std::endl;
	if( failed == 0 ) {
		std::cout << "All tests passed." << std::endl;
		return 0;
	}
	std::cout << failed << " test(s) failed." << std::endl;
	return 1;
}
