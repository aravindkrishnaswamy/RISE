//////////////////////////////////////////////////////////////////////
//
//  MISWeightsTest.cpp - Validates MIS weight functions from
//    MISWeights.h:
//
//    1. BalanceHeuristic basic values and symmetry
//    2. OptimalMIS2Weight with alpha=0.5 matches BalanceHeuristic
//    3. OptimalMIS2Weight with alpha=0 and alpha=1 edge cases
//    4. Two-technique weight sum property (w_a + w_b = 1)
//    5. PowerHeuristic still works correctly
//    6. Edge cases: both PDFs zero, one PDF zero, extreme ratios
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>
#include <iomanip>

#include "../src/Library/Utilities/MISWeights.h"
#include "../src/Library/Utilities/PathTransportUtilities.h"

using namespace RISE;

static const double TOL = 1e-10;

static bool ApproxEqual(
	double a,
	double b,
	double tol
	)
{
	return fabs( a - b ) < tol;
}

static int failCount = 0;
static int passCount = 0;

static void Check(
	bool condition,
	const char* testName
	)
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// Test 1: BalanceHeuristic basic values
//////////////////////////////////////////////////////////////////////
static void TestBalanceHeuristic()
{
	std::cout << "Test 1: BalanceHeuristic" << std::endl;

	// Equal PDFs -> 0.5
	Check( ApproxEqual( MISWeights::BalanceHeuristic( 1.0, 1.0 ), 0.5, TOL ),
		"equal PDFs -> 0.5" );

	// Known values
	Check( ApproxEqual( MISWeights::BalanceHeuristic( 3.0, 4.0 ), 3.0 / 7.0, TOL ),
		"3/(3+4)" );

	// Symmetry: w(a,b) + w(b,a) = 1
	double wa = MISWeights::BalanceHeuristic( 3.0, 4.0 );
	double wb = MISWeights::BalanceHeuristic( 4.0, 3.0 );
	Check( ApproxEqual( wa + wb, 1.0, TOL ),
		"symmetry: w(a,b) + w(b,a) = 1" );

	// One PDF zero
	Check( ApproxEqual( MISWeights::BalanceHeuristic( 5.0, 0.0 ), 1.0, TOL ),
		"one PDF zero -> 1" );
	Check( ApproxEqual( MISWeights::BalanceHeuristic( 0.0, 5.0 ), 0.0, TOL ),
		"own PDF zero -> 0" );

	// Both PDFs zero
	Check( ApproxEqual( MISWeights::BalanceHeuristic( 0.0, 0.0 ), 0.0, TOL ),
		"both PDFs zero -> 0" );
}

//////////////////////////////////////////////////////////////////////
// Test 2: OptimalMIS2Weight matches balance heuristic at alpha=0.5
//////////////////////////////////////////////////////////////////////
static void TestOptimalMatchesBalance()
{
	std::cout << "Test 2: OptimalMIS2Weight with alpha=0.5" << std::endl;

	double pdfs[][2] = {
		{1.0, 1.0}, {3.0, 4.0}, {0.1, 10.0}, {100.0, 0.01}, {0.0, 5.0}, {5.0, 0.0}
	};

	for( int i = 0; i < 6; i++ ) {
		double pa = pdfs[i][0];
		double pb = pdfs[i][1];
		double balance = MISWeights::BalanceHeuristic( pa, pb );
		double optimal = MISWeights::OptimalMIS2Weight( pa, pb, 0.5 );
		char name[128];
		snprintf( name, sizeof(name), "alpha=0.5 matches balance: pa=%.2f pb=%.2f", pa, pb );
		Check( ApproxEqual( balance, optimal, TOL ), name );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 3: OptimalMIS2Weight edge cases (alpha=0, alpha=1)
//////////////////////////////////////////////////////////////////////
static void TestOptimalEdgeCases()
{
	std::cout << "Test 3: OptimalMIS2Weight alpha edge cases" << std::endl;

	// alpha=1: fully trust technique A -> w = 1 when pa > 0
	Check( ApproxEqual( MISWeights::OptimalMIS2Weight( 3.0, 4.0, 1.0 ), 1.0, TOL ),
		"alpha=1 -> w=1 when pa>0" );

	// alpha=0: fully trust technique B -> w = 0
	Check( ApproxEqual( MISWeights::OptimalMIS2Weight( 3.0, 4.0, 0.0 ), 0.0, TOL ),
		"alpha=0 -> w=0" );

	// Both PDFs zero with any alpha
	Check( ApproxEqual( MISWeights::OptimalMIS2Weight( 0.0, 0.0, 0.7 ), 0.0, TOL ),
		"both zero -> 0" );

	// alpha=0 and pb=0 -> both sides zero -> 0
	Check( ApproxEqual( MISWeights::OptimalMIS2Weight( 3.0, 0.0, 0.0 ), 0.0, TOL ),
		"alpha=0 pb=0 -> 0" );
}

//////////////////////////////////////////////////////////////////////
// Test 4: Two-technique weight sum property
//////////////////////////////////////////////////////////////////////
static void TestWeightSum()
{
	std::cout << "Test 4: Weight sum property" << std::endl;

	double alphas[] = { 0.1, 0.25, 0.5, 0.75, 0.9 };
	double pdfs[][2] = { {1.0, 1.0}, {3.0, 4.0}, {0.1, 10.0}, {100.0, 0.01} };

	for( int a = 0; a < 5; a++ ) {
		for( int p = 0; p < 4; p++ ) {
			double pa = pdfs[p][0];
			double pb = pdfs[p][1];
			double alpha = alphas[a];
			// w_a with alpha + w_b with (1-alpha) should sum to 1
			double wa = MISWeights::OptimalMIS2Weight( pa, pb, alpha );
			double wb = MISWeights::OptimalMIS2Weight( pb, pa, 1.0 - alpha );
			char name[128];
			snprintf( name, sizeof(name), "sum=1: alpha=%.2f pa=%.2f pb=%.2f (sum=%.12f)", alpha, pa, pb, wa + wb );
			Check( ApproxEqual( wa + wb, 1.0, TOL ), name );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// Test 5: PowerHeuristic cross-check
//////////////////////////////////////////////////////////////////////
static void TestPowerHeuristic()
{
	std::cout << "Test 5: PowerHeuristic cross-check" << std::endl;

	using PathTransportUtilities::PowerHeuristic;

	// Known value: 3^2 / (3^2 + 4^2) = 9/25
	Check( ApproxEqual( PowerHeuristic( 3.0, 4.0 ), 9.0 / 25.0, TOL ),
		"power(3,4) = 9/25" );

	// Symmetry: sum = 1
	double wa = PowerHeuristic( 3.0, 4.0 );
	double wb = PowerHeuristic( 4.0, 3.0 );
	Check( ApproxEqual( wa + wb, 1.0, TOL ),
		"power symmetry sum = 1" );

	// Equal PDFs -> 0.5
	Check( ApproxEqual( PowerHeuristic( 5.0, 5.0 ), 0.5, TOL ),
		"power equal PDFs -> 0.5" );

	// Power heuristic concentrates more weight than balance
	double powerW = PowerHeuristic( 10.0, 1.0 );
	double balanceW = MISWeights::BalanceHeuristic( 10.0, 1.0 );
	Check( powerW > balanceW,
		"power concentrates more than balance when pa >> pb" );
}

//////////////////////////////////////////////////////////////////////
// Test 6: Extreme ratio edge cases
//////////////////////////////////////////////////////////////////////
static void TestExtremeRatios()
{
	std::cout << "Test 6: Extreme ratio edge cases" << std::endl;

	// Very large ratio
	double w = MISWeights::OptimalMIS2Weight( 1e10, 1e-10, 0.5 );
	Check( w > 0.999, "extreme ratio: dominant technique -> near 1" );
	Check( w <= 1.0, "weight <= 1" );

	// Very small alpha with large pa
	w = MISWeights::OptimalMIS2Weight( 1e10, 1.0, 0.001 );
	Check( w >= 0 && w <= 1.0, "small alpha large pa -> valid range" );

	// Verify no NaN or Inf
	w = MISWeights::BalanceHeuristic( 1e300, 1e-300 );
	Check( std::isfinite( w ), "extreme values: no NaN/Inf" );
}

static void TestNMNoEventSurvivalWeight()
{
	std::cout << "Test 7: NM no-event survival cancellation" << std::endl;
	using namespace PathTransportUtilities;
	Check(NMNoEventSurvivalWeight(1.0)==1.0,
		"pure delta tracking has exact unit no-event weight");
	Check(NMNoEventSurvivalWeight(0.5)==2.0,
		"the delta-tracking half of the mixture has exact weight two");
	Check(NMNoEventSurvivalWeight(0.0)==0.0 &&
		NMNoEventSurvivalWeight(-0.5)==0.0,
		"nonpositive technique mass fails closed");
}

//////////////////////////////////////////////////////////////////////
// Test 7: one-sample guiding mixture support and null-sample semantics
//////////////////////////////////////////////////////////////////////
static void TestGuidingSelectedMixturePdf()
{
	std::cout << "Test 8: GuidingSelectedMixturePdf" << std::endl;

	using PathTransportUtilities::GuidingSelectedMixturePdf;

	Check( ApproxEqual(
		GuidingSelectedMixturePdf( 0.25, 0.0, 0.2, false ), 0.15, TOL ),
		"phase-selected direction retains phase support when guide PDF is zero" );
	Check( GuidingSelectedMixturePdf( 0.25, 0.0, 0.2, true ) == 0,
		"guide-selected zero-density result is an explicit null sample" );
	Check( ApproxEqual(
		GuidingSelectedMixturePdf( 0.25, 0.4, 0.2, true ), 0.25, TOL ),
		"guide-selected valid result uses the actual mixture density" );
	Check( GuidingSelectedMixturePdf( 0.25, -0.1, 0.2, false ) == 0,
		"negative guide density fails closed" );

	const Scalar tinyDensity = GuidingSelectedMixturePdf(
		1.0 - 5.0e-12, 0.0, 0.1, false );
	Check( tinyDensity > 0 && tinyDensity < NEARZERO,
		"near-unit guide mixture produces legal sub-NEARZERO phase support" );
	Check( PathTransportUtilities::IsPositiveFiniteDensity( tinyDensity ),
		"every finite positive density remains in estimator support" );
	Check( !PathTransportUtilities::IsPositiveFiniteDensity( 0.0 ),
		"exact zero density is outside support" );
}

//////////////////////////////////////////////////////////////////////
// Test 8: RIS selection preserves exact finite-positive support
//////////////////////////////////////////////////////////////////////
static void TestGuidingRISTinyPositiveSupport()
{
	std::cout << "Test 9: GuidingRIS tiny-positive support" << std::endl;

	PathTransportUtilities::GuidingRISCandidate<Scalar> candidates[2] = {};
	candidates[0].risTarget = 2.0e-20;
	candidates[0].risWeight = 2.0e-20;
	candidates[1].risTarget = 1.0e-20;
	candidates[1].risWeight = 1.0e-20;
	Scalar effectivePdf = 0;
	const unsigned int selected =
		PathTransportUtilities::GuidingRISSelectCandidate(
			candidates,2,0.9,effectivePdf);
	Check( selected==1,
		"sub-NEARZERO RIS weights retain their relative selection mass" );
	Check( PathTransportUtilities::IsPositiveFiniteDensity(effectivePdf),
		"sub-NEARZERO RIS weights produce a positive finite density" );
	Check( ApproxEqual(effectivePdf,2.0/3.0,TOL),
		"scaled RIS selection preserves the exact effective density ratio" );

	candidates[0].risTarget = 2.0e300;
	candidates[0].risWeight = 2.0e300;
	candidates[1].risTarget = 1.0e300;
	candidates[1].risWeight = 1.0e300;
	effectivePdf = 0;
	const unsigned int largeSelected =
		PathTransportUtilities::GuidingRISSelectCandidate(
			candidates,2,0.9,effectivePdf);
	Check( largeSelected==1 &&
		PathTransportUtilities::IsPositiveFiniteDensity(effectivePdf) &&
		ApproxEqual(effectivePdf,2.0/3.0,TOL),
		"scaled RIS selection avoids overflow without changing the density" );

	candidates[0].risTarget = 0.0;
	candidates[0].risWeight = 0.0;
	candidates[1].risTarget = 0.25;
	candidates[1].risWeight = 0.5;
	effectivePdf = 0.0;
	const unsigned int zeroBoundarySelected =
		PathTransportUtilities::GuidingRISSelectCandidate(
			candidates,2,0.0,effectivePdf);
	Check(zeroBoundarySelected==1 &&
		PathTransportUtilities::IsPositiveFiniteDensity(effectivePdf),
		"xi zero skips a leading zero-weight RIS atom and selects supported mass");
}

//////////////////////////////////////////////////////////////////////
// Test 9: Phase-B thermal-volume NEE/march family partition
//////////////////////////////////////////////////////////////////////
static void TestVolumeEmissionFamilyPartition()
{
	std::cout << "Test 10: VolumeEmission family partition" << std::endl;

	const Scalar pV = 3.0;
	const Scalar pMarch = 4.0;
	const Scalar wNee = MISWeights::VolumeEmissionNEEFamilyWeight(pV,pMarch);
	const Scalar wMarch = MISWeights::VolumeEmissionMarchFamilyWeight(
		pMarch,pV,true,false);
	Check(ApproxEqual(wNee,9.0/25.0,TOL),
		"NEE family gets pV^2/(pV^2+pMarch^2)");
	Check(ApproxEqual(wMarch,16.0/25.0,TOL),
		"march family gets pMarch^2/(pMarch^2+pV^2)");
	Check(ApproxEqual(wNee+wMarch,1.0,TOL),
		"competing NEE and march family weights sum to one");

	Check(MISWeights::VolumeEmissionMarchFamilyWeight(
		pMarch,pV,false,false)==1.0,
		"camera or NEE-disabled march keeps weight one");
	Check(MISWeights::VolumeEmissionMarchFamilyWeight(
		pMarch,pV,true,true)==1.0,
		"sampled singular continuation keeps march weight one");
	Check(MISWeights::VolumeEmissionNEEFamilyWeight(pV,0.0)==1.0,
		"structurally absent march gives NEE weight one");

	const Scalar density = MISWeights::VolumeEmissionMarchDensity(
		0.25,0.5,2.0,0.125);
	Check(ApproxEqual(density,0.00390625,TOL),
		"march density includes direction, p_t, inverse-square, and chain survival");
	Check(MISWeights::VolumeEmissionMarchDensity(0.25,0.5,2.0,0.0)==0.0,
		"zero boundary survival removes march support");
	Check(ApproxEqual(MISWeights::VolumeEmissionMarchDensity(
		0.25,1.0e200,1.0e200,1.0),2.5e-201,2.5e-211),
		"march density preserves a representable result when distance squared overflows");
	Check(ApproxEqual(MISWeights::VolumeEmissionMarchDensity(
		1.0e200,1.0e200,1.0e200,1.0),1.0,1e-12),
		"march density cancels overflowing numerator and denominator scales");
	Check(ApproxEqual(MISWeights::VolumeEmissionMarchDensity(
		1.0e-200,1.0e-200,1.0e-200,1.0),1.0,1e-12),
		"march density cancels underflowing numerator and denominator scales");

	const Scalar extremeNee = MISWeights::VolumeEmissionNEEFamilyWeight(
		1.0e300,1.0e-300);
	const Scalar extremeMarch = MISWeights::VolumeEmissionMarchFamilyWeight(
		1.0e-300,1.0e300,true,false);
	Check(std::isfinite(extremeNee) && extremeNee==1.0,
		"NEE family weight stays finite for extreme density ratios");
	Check(std::isfinite(extremeMarch) && extremeMarch==0.0,
		"march family weight stays finite for extreme density ratios");

	const size_t legacyRayStateSize = 88;
	Check(sizeof(IRayCaster::RAY_STATE)==legacyRayStateSize,
		"volume MIS state does not alter the public RAY_STATE ABI");
	const VolumeEmissionSegmentState defaultState =
		CurrentVolumeEmissionSegmentState();
	Check(!defaultState.competitionAvailable && !defaultState.continuationSingular,
		"unscoped camera or unsupported segment gets weight-one state");
	const VolumeEmissionSegmentState outerState(true,false);
	{
		const VolumeEmissionSegmentStateScope outerScope(outerState);
		const VolumeEmissionSegmentState currentOuter =
			CurrentVolumeEmissionSegmentState();
		Check(currentOuter.competitionAvailable && !currentOuter.continuationSingular,
			"request-local scope propagates the two-bit segment state");
		const VolumeEmissionSegmentState innerState(false,true);
		{
			const VolumeEmissionSegmentStateScope innerScope(innerState);
			const VolumeEmissionSegmentState currentInner =
				CurrentVolumeEmissionSegmentState();
			Check(!currentInner.competitionAvailable && currentInner.continuationSingular,
				"nested recursive segment scope overrides its parent");
		}
		const VolumeEmissionSegmentState restoredOuter =
			CurrentVolumeEmissionSegmentState();
		Check(restoredOuter.competitionAvailable && !restoredOuter.continuationSingular,
			"nested recursive segment scope restores its parent exactly");
	}
	const VolumeEmissionSegmentState restoredDefault =
		CurrentVolumeEmissionSegmentState();
	Check(!restoredDefault.competitionAvailable && !restoredDefault.continuationSingular,
		"completed request-local scope restores camera defaults");
	const VolumeEmissionSegmentState proposalState(true,false,0,0.25,0.0);
	const VolumeEmissionSegmentState oneBoundary =
		AdvanceVolumeEmissionSegmentState(proposalState,0.5,1.25);
	const VolumeEmissionSegmentState twoBoundaries =
		AdvanceVolumeEmissionSegmentState(oneBoundary,0.5,2.75);
	Check(ApproxEqual(twoBoundaries.directionPdf,0.25,TOL) &&
		ApproxEqual(exp(twoBoundaries.logBoundarySurvival),0.25,TOL) &&
		ApproxEqual(twoBoundaries.distanceOffset,4.0,TOL),
		"null-boundary advancement preserves p_omega and accumulates no-event atoms and distance");
	const Scalar logDensity = MISWeights::VolumeEmissionMarchDensityFromLogSurvival(
		twoBoundaries.directionPdf,0.5,2.0,twoBoundaries.logBoundarySurvival);
	const Scalar productDensity = MISWeights::VolumeEmissionMarchDensity(
		0.25,0.5,2.0,0.25);
	Check(ApproxEqual(logDensity,productDensity,TOL),
		"log-space boundary survival gives the pinned p_omega p_t product over r squared");
	const Scalar collisionDensity =
		MISWeights::VolumeEmissionMarchDensityAtCollision(
			twoBoundaries,0.5,2.0);
	const Scalar expectedCollisionDensity = MISWeights::VolumeEmissionMarchDensity(
		0.25,0.5,6.0,0.25);
	Check(ApproxEqual(collisionDensity,expectedCollisionDensity,TOL),
		"collision density uses distance from the originating vertex across all boundaries");
	const MISWeights::LogDensity logCollisionDensity =
		MISWeights::VolumeEmissionMarchLogDensityAtCollision(
			twoBoundaries,log(0.5),2.0);
	Check(logCollisionDensity.hasSupport &&
		ApproxEqual(exp(logCollisionDensity.value),expectedCollisionDensity,TOL),
		"log-distance collision density equals the ordinary representable formulation");
	const Scalar pVAtCollision = 0.125;
	const Scalar weightedMarch =
		MISWeights::VolumeEmissionMarchFamilyWeightFromLogDensities(
			logCollisionDensity,MISWeights::MakeLogDensity(pVAtCollision),true,false);
	const Scalar weightedNee = MISWeights::VolumeEmissionFamilyWeightFromLogDensities(
		MISWeights::MakeLogDensity(pVAtCollision),logCollisionDensity);
	Check(ApproxEqual(weightedMarch+weightedNee,1.0,TOL),
		"collision-consumed march density is complementary to the labeled NEE density");
	const Scalar commonTinyMarch =
		MISWeights::VolumeEmissionMarchFamilyWeightFromLogDensities(
			MISWeights::LogDensity(true,-1400.0),
			MISWeights::LogDensity(true,-1400.0),true,false);
	Check(ApproxEqual(commonTinyMarch,0.5,TOL),
		"common-scale densities remain balanced after both ordinary values underflow");
	const MISWeights::LogDensity tinyMixture =
		MISWeights::EqualMixtureLogDensity(
			MISWeights::LogDensity(true,-1400.0),
			MISWeights::LogDensity(true,-1401.0));
	const Scalar expectedTinyMixture = -1400.0 + log1p(exp(-1.0)) - log(2.0);
	Check(tinyMixture.hasSupport &&
		ApproxEqual(tinyMixture.value,expectedTinyMixture,1e-13),
		"equal distance mixture retains its log density after both terms underflow");
	VolumeEmissionSegmentState deepChain = proposalState;
	for(unsigned int i=0;i<2000;++i) {
		deepChain = AdvanceVolumeEmissionSegmentState(deepChain,0.5,1.0);
	}
	Check(std::isfinite(deepChain.logBoundarySurvival) &&
		ApproxEqual(deepChain.logBoundarySurvival,2000.0*log(0.5),1e-9) &&
		deepChain.distanceOffset==2000.0,
		"long null-boundary chains retain every no-event atom and complete path length");
	const VolumeEmissionSegmentState impossibleBoundary =
		AdvanceVolumeEmissionSegmentState(proposalState,0.0,1.0);
	Check(MISWeights::VolumeEmissionMarchDensityFromLogSurvival(
		impossibleBoundary.directionPdf,0.5,2.0,
		impossibleBoundary.logBoundarySurvival)==0.0,
		"zero-probability boundary survival removes march support");
	Check(!MISWeights::VolumeEmissionMarchLogDensityAtCollision(
		impossibleBoundary,log(0.5),1.0).hasSupport,
		"zero-probability boundary survival has no log-domain march support");
	{
		const VolumeEmissionSegmentStateScope temporaryScope(
			VolumeEmissionSegmentState(true,true) );
		const VolumeEmissionSegmentState copiedTemporary =
			CurrentVolumeEmissionSegmentState();
		Check(copiedTemporary.competitionAvailable &&
			copiedTemporary.continuationSingular,
			"scope owns an immutable copy when constructed from a temporary");
	}
}

//////////////////////////////////////////////////////////////////////
// Main
//////////////////////////////////////////////////////////////////////
int main()
{
	std::cout << "=== MIS Weights Test ===" << std::endl;

	TestBalanceHeuristic();
	TestOptimalMatchesBalance();
	TestOptimalEdgeCases();
	TestWeightSum();
	TestPowerHeuristic();
	TestExtremeRatios();
	TestNMNoEventSurvivalWeight();
	TestGuidingSelectedMixturePdf();
	TestGuidingRISTinyPositiveSupport();
	TestVolumeEmissionFamilyPartition();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	if( failCount > 0 ) {
		std::cout << "*** TEST SUITE FAILED ***" << std::endl;
		return 1;
	}

	std::cout << "All tests passed." << std::endl;
	return 0;
}
