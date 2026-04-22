//////////////////////////////////////////////////////////////////////
//
//  BDPTSMSSuppressionTest.cpp - Unit tests for the SMS cross-strategy
//    emission-suppression predicate used by BDPT's (s==0) branch.
//
//    BDPT's (s==0, t>0) strategy generates delta-through caustic paths
//    whenever the eye subpath BSDF-samples a delta lobe and terminates
//    at an emitter.  Those are the SAME paths SMS generates.  The
//    power-heuristic MIS walk skips delta vertices (both `vj.isDelta`
//    and `eyeVerts[j-1].isDelta` guards trigger a `continue`), so the
//    (s==0) strategy ends up with near-unit MIS weight for SDS-shaped
//    paths.  Meanwhile SMS adds with misWeight=1.0 (no cross-strategy
//    MIS).  Without suppression this double-counts caustic energy by
//    ~2x.
//
//    BDPTIntegrator::ShouldSuppressSMSOverlap implements PT's
//    `bPassedThroughSpecular && bHadNonSpecularShading` rule over the
//    eye subpath; this test validates its behaviour on the canonical
//    topologies.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <vector>
#include <cassert>

#include "../src/Library/Shaders/BDPTIntegrator.h"
#include "../src/Library/Shaders/BDPTVertex.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// Vertex construction helpers.  BDPTVertex's default ctor sets most
// fields to safe defaults (isDelta=false, isBSSRDFEntry=false); we
// only override what the predicate reads.
//////////////////////////////////////////////////////////////////////

static BDPTVertex Camera()
{
	BDPTVertex v;
	v.type = BDPTVertex::CAMERA;
	v.isDelta = false;
	v.isBSSRDFEntry = false;
	return v;
}

static BDPTVertex Diffuse()
{
	BDPTVertex v;
	v.type = BDPTVertex::SURFACE;
	v.isDelta = false;
	v.isBSSRDFEntry = false;
	return v;
}

static BDPTVertex DeltaSpec()
{
	BDPTVertex v;
	v.type = BDPTVertex::SURFACE;
	v.isDelta = true;
	v.isBSSRDFEntry = false;
	return v;
}

static BDPTVertex BSSRDFEntryNonDelta()
{
	// Diffusion-branch emergence (entryV.isDelta = false in
	// BDPTIntegrator.cpp:1906).
	BDPTVertex v;
	v.type = BDPTVertex::SURFACE;
	v.isDelta = false;
	v.isBSSRDFEntry = true;
	return v;
}

static BDPTVertex BSSRDFEntryDelta()
{
	// Random-walk-branch entry point (entryV.isDelta = true in
	// BDPTIntegrator.cpp:1993) — still a non-specular shading event
	// because isBSSRDFEntry overrides.
	BDPTVertex v;
	v.type = BDPTVertex::SURFACE;
	v.isDelta = true;
	v.isBSSRDFEntry = true;
	return v;
}

static BDPTVertex Emitter()
{
	BDPTVertex v;
	v.type = BDPTVertex::SURFACE;
	v.isDelta = false;
	v.isBSSRDFEntry = false;
	return v;
}

//////////////////////////////////////////////////////////////////////
// Tests
//////////////////////////////////////////////////////////////////////

void TestSuppress_TooShort_t2()
{
	std::cout << "Testing suppression: t<3 never suppresses (no delta chain can fit)..." << std::endl;
	// [camera, emitter] — t=2, no room for a shading point + delta chain.
	std::vector<BDPTVertex> eye{ Camera(), Emitter() };
	Check( !BDPTIntegrator::ShouldSuppressSMSOverlap( eye, 2 ),
		"t=2 should not suppress" );
}

void TestSuppress_TooShort_t1()
{
	std::cout << "Testing suppression: t==1 never suppresses..." << std::endl;
	std::vector<BDPTVertex> eye{ Camera() };
	Check( !BDPTIntegrator::ShouldSuppressSMSOverlap( eye, 1 ),
		"t=1 should not suppress" );
}

void TestSuppress_Empty()
{
	std::cout << "Testing suppression: t==0 never suppresses..." << std::endl;
	std::vector<BDPTVertex> eye;
	Check( !BDPTIntegrator::ShouldSuppressSMSOverlap( eye, 0 ),
		"t=0 should not suppress" );
}

void TestSuppress_DirectDiffuseHit()
{
	std::cout << "Testing suppression: camera->diffuse->emitter (direct hit, no SMS overlap)..." << std::endl;
	// [camera, diffuse, emitter] — one non-delta vertex, no delta chain.
	// Walk j=1 only: diffuse -> bSpec=false, bDif=true.  Final bSpec=false
	// -> no suppression.  This is the "direct lighting via BSDF sampling"
	// case; BDPT's (s==0) should contribute normally.
	std::vector<BDPTVertex> eye{ Camera(), Diffuse(), Emitter() };
	Check( !BDPTIntegrator::ShouldSuppressSMSOverlap( eye, 3 ),
		"camera->diffuse->emitter should not suppress" );
}

void TestSuppress_SDSCausticPath()
{
	std::cout << "Testing suppression: camera->diffuse->delta->delta->emitter (SMS domain)..." << std::endl;
	// [camera, diffuse, delta, delta, emitter] — the canonical SMS
	// topology.  Walk: j=1 diffuse -> bSpec=false bDif=true; j=2 delta
	// -> bSpec=true; j=3 delta -> bSpec=true.  Final: bSpec && bDif ->
	// SUPPRESS.
	std::vector<BDPTVertex> eye{
		Camera(), Diffuse(), DeltaSpec(), DeltaSpec(), Emitter()
	};
	Check( BDPTIntegrator::ShouldSuppressSMSOverlap( eye, 5 ),
		"camera->diffuse->delta->delta->emitter must suppress" );
}

void TestSuppress_MinimalSDS()
{
	std::cout << "Testing suppression: camera->diffuse->delta->emitter (minimal SDS k=1)..." << std::endl;
	// k=1 SMS path (single refraction slab).  Walk: j=1 diffuse ->
	// bSpec=false bDif=true; j=2 delta -> bSpec=true.  Final: suppress.
	std::vector<BDPTVertex> eye{
		Camera(), Diffuse(), DeltaSpec(), Emitter()
	};
	Check( BDPTIntegrator::ShouldSuppressSMSOverlap( eye, 4 ),
		"camera->diffuse->delta->emitter must suppress" );
}

void TestSuppress_PureSpecular_NoSMS()
{
	std::cout << "Testing suppression: camera->delta->delta->emitter (no non-specular shading)..." << std::endl;
	// Pure specular camera-to-light path (e.g. direct glass view of
	// sun).  SMS has no shading point to anchor from, so no overlap.
	// Walk: j=1 delta -> bSpec=true; j=2 delta -> bSpec=true; bDif
	// never set.  No suppression.
	std::vector<BDPTVertex> eye{
		Camera(), DeltaSpec(), DeltaSpec(), Emitter()
	};
	Check( !BDPTIntegrator::ShouldSuppressSMSOverlap( eye, 4 ),
		"pure specular (no diffuse) must not suppress" );
}

void TestSuppress_DiffuseAfterDelta()
{
	std::cout << "Testing suppression: camera->delta->diffuse->emitter (reset after non-delta)..." << std::endl;
	// Eye enters glass (delta), then hits the far wall (diffuse), and
	// the diffuse surface happens to be the emitter.  This is NEE via
	// BSDF sampling, not an SMS caustic.  Walk: j=1 delta -> bSpec=true;
	// j=2 diffuse -> bSpec=false (reset), bDif=true.  Final:
	// bSpec=false -> no suppression.
	std::vector<BDPTVertex> eye{
		Camera(), DeltaSpec(), Diffuse(), Emitter()
	};
	Check( !BDPTIntegrator::ShouldSuppressSMSOverlap( eye, 4 ),
		"camera->delta->diffuse->emitter must not suppress (bSpec resets)" );
}

void TestSuppress_DiffuseAfterDeltaChain()
{
	std::cout << "Testing suppression: camera->diffuse->delta->delta->diffuse->emitter..." << std::endl;
	// Path re-diffuses after the specular chain.  Walk: j=1 diffuse ->
	// bSpec=false bDif=true; j=2 delta -> bSpec=true; j=3 delta ->
	// bSpec=true; j=4 diffuse -> bSpec=false (reset), bDif=true (still).
	// Final: bSpec=false -> no suppression.  BDPT's (s==0) and SMS
	// don't overlap on this topology — the diffuse-after-chain means
	// the LAST scatter was non-delta, so SMS's anchor is at v4 not v1
	// and would produce a different path.
	std::vector<BDPTVertex> eye{
		Camera(), Diffuse(), DeltaSpec(), DeltaSpec(), Diffuse(), Emitter()
	};
	Check( !BDPTIntegrator::ShouldSuppressSMSOverlap( eye, 6 ),
		"diffuse-after-delta-chain must not suppress" );
}

void TestSuppress_BSSRDFEmergence_Nondelta()
{
	std::cout << "Testing suppression: camera->BSSRDFEntry(delta=false)->delta->emitter..." << std::endl;
	// Subsurface emergence from the diffusion branch: entryV.isDelta
	// is already false, so this is trivially non-delta shading.  The
	// subsequent specular chain then sets bSpec=true.  Suppress.
	std::vector<BDPTVertex> eye{
		Camera(), BSSRDFEntryNonDelta(), DeltaSpec(), Emitter()
	};
	Check( BDPTIntegrator::ShouldSuppressSMSOverlap( eye, 4 ),
		"BSSRDF diffusion-branch emergence + delta chain must suppress" );
}

void TestSuppress_BSSRDFEmergence_DeltaFlag()
{
	std::cout << "Testing suppression: camera->BSSRDFEntry(delta=true)->delta->emitter..." << std::endl;
	// Random-walk-branch BSSRDF entry records isDelta=true (PT-side
	// convention for the MIS weight computation).  The isBSSRDFEntry
	// override forces treatAsNonDelta=true here so that the emergent
	// path correctly resets bSpec and sets bDif — matching the PT
	// handoff rs2.smsPassedThroughSpecular=false,
	// rs2.smsHadNonSpecularShading=true.  Suppress.
	std::vector<BDPTVertex> eye{
		Camera(), BSSRDFEntryDelta(), DeltaSpec(), Emitter()
	};
	Check( BDPTIntegrator::ShouldSuppressSMSOverlap( eye, 4 ),
		"BSSRDF random-walk-branch (isDelta=true) + delta chain must still suppress "
		"via isBSSRDFEntry override" );
}

void TestSuppress_LongDeltaChain()
{
	std::cout << "Testing suppression: camera->diffuse->5x delta->emitter..." << std::endl;
	// Deep SMS chain (e.g. multi-surface refractor).  All the deltas
	// keep bSpec latched at true; bDif was set at j=1.  Suppress.
	std::vector<BDPTVertex> eye;
	eye.push_back( Camera() );
	eye.push_back( Diffuse() );
	for( int i = 0; i < 5; ++i ) eye.push_back( DeltaSpec() );
	eye.push_back( Emitter() );
	Check( BDPTIntegrator::ShouldSuppressSMSOverlap( eye, static_cast<unsigned int>(eye.size()) ),
		"camera->diffuse->5*delta->emitter must suppress" );
}

void TestSuppress_BoundsGuard_tExceedsSize()
{
	std::cout << "Testing suppression: t > eyeVerts.size() returns false (bounds guard)..." << std::endl;
	// Defensive guard — caller must not pass an out-of-range t, but
	// if it does the predicate should refuse to suppress rather than
	// read past the vector.
	std::vector<BDPTVertex> eye{ Camera(), Diffuse(), DeltaSpec(), Emitter() };
	Check( !BDPTIntegrator::ShouldSuppressSMSOverlap( eye, 99 ),
		"out-of-range t must return false" );
}

void TestSuppress_EmitterAdjacentToDiffuse()
{
	std::cout << "Testing suppression: camera->diffuse->emitter vs camera->delta->diffuse->emitter..." << std::endl;
	// Sanity check: the predicate reads eyeVerts[1 .. t-2], so the
	// emitter's own isDelta flag doesn't participate.  Verify by
	// toggling the emitter to delta — should still not suppress in
	// the non-SMS-topology case.
	std::vector<BDPTVertex> eye1{ Camera(), Diffuse(), Emitter() };
	BDPTVertex deltaEmitter = Emitter();
	deltaEmitter.isDelta = true;
	std::vector<BDPTVertex> eye2{ Camera(), Diffuse(), deltaEmitter };
	Check( !BDPTIntegrator::ShouldSuppressSMSOverlap( eye1, 3 ),
		"camera->diffuse->emitter (non-delta emitter) must not suppress" );
	Check( !BDPTIntegrator::ShouldSuppressSMSOverlap( eye2, 3 ),
		"emitter.isDelta flag must not affect the predicate" );
}

int main()
{
	std::cout << std::endl;
	std::cout << "========================================" << std::endl;
	std::cout << "  BDPT SMS Suppression Tests" << std::endl;
	std::cout << "========================================" << std::endl;
	std::cout << std::endl;

	TestSuppress_Empty();
	TestSuppress_TooShort_t1();
	TestSuppress_TooShort_t2();
	TestSuppress_DirectDiffuseHit();
	TestSuppress_MinimalSDS();
	TestSuppress_SDSCausticPath();
	TestSuppress_PureSpecular_NoSMS();
	TestSuppress_DiffuseAfterDelta();
	TestSuppress_DiffuseAfterDeltaChain();
	TestSuppress_BSSRDFEmergence_Nondelta();
	TestSuppress_BSSRDFEmergence_DeltaFlag();
	TestSuppress_LongDeltaChain();
	TestSuppress_BoundsGuard_tExceedsSize();
	TestSuppress_EmitterAdjacentToDiffuse();

	std::cout << std::endl;
	std::cout << "========================================" << std::endl;
	std::cout << "  Passed: " << passCount << "   Failed: " << failCount << std::endl;
	std::cout << "========================================" << std::endl;
	std::cout << std::endl;

	return (failCount == 0) ? 0 : 1;
}
