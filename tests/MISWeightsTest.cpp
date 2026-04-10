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
	Check( w == w && w != 1.0 / 0.0, "extreme values: no NaN/Inf" );
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
