//////////////////////////////////////////////////////////////////////
//
//  ScalarPainterParserTest.cpp - Tests that the `scalar_painter`
//    scene chunk parses correctly and registers painters in the
//    job's IScalarPainterManager.
//
//  Phase 2 of the IScalarPainter refactor (see
//  docs/ISCALARPAINTER_REFACTOR.md).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Interfaces/IScalarPainterManager.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const char* name )
{
	if( cond ) passCount++;
	else {
		failCount++;
		std::cout << "  FAIL: " << name << std::endl;
	}
}

static bool ApproxEq( Scalar a, Scalar b, Scalar tol = Scalar( 1e-6 ) )
{
	return std::fabs( a - b ) <= tol;
}

static RayIntersectionGeometric MakeDummyRig()
{
	return RayIntersectionGeometric( Ray(), nullRasterizerState );
}

// Writes a scene file, loads it, returns the IJobPriv (caller releases).
// Returns nullptr if parse failed.  Pass scene body only; the wrapper
// prepends the version banner and writes the temp file.
static IJobPriv* LoadScene( const char* body, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof( path ),
		"/tmp/scalar_painter_test_%s_%d.RISEscene", tag, (int)::getpid() );
	std::ofstream ofs( path );
	if( !ofs.is_open() ) return nullptr;
	ofs << "RISE ASCII SCENE 6\n\n" << body;
	ofs.close();

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		std::remove( path );
		return nullptr;
	}
	const bool ok = pJob->LoadAsciiScene( path );
	std::remove( path );
	if( !ok ) {
		safe_release( pJob );
		return nullptr;
	}
	return pJob;
}

static void TestUniformScalarValue()
{
	std::cout << "TestUniformScalarValue" << std::endl;
	const char* scene =
		"scalar_painter\n"
		"{\n"
		"\tname my_ior\n"
		"\tvalue 1.5\n"
		"}\n";
	IJobPriv* pJob = LoadScene( scene, "uniform_value" );
	Check( pJob != nullptr, "uniform-value: scene loads" );
	if( !pJob ) return;

	IScalarPainter* p = pJob->GetScalarPainters()->GetItem( "my_ior" );
	Check( p != nullptr, "uniform-value: painter registered" );
	if( p ) {
		const auto ri = MakeDummyRig();
		Check( ApproxEq( p->GetValueAtNM( ri, 555 ), 1.5 ),
			"uniform-value: GetValueAtNM = 1.5" );
		Check( ! p->HasPerChannelVariation(),
			"uniform-value: !HasPerChannelVariation" );
	}
	safe_release( pJob );
}

static void TestRGBValues()
{
	std::cout << "TestRGBValues" << std::endl;
	const char* scene =
		"scalar_painter\n"
		"{\n"
		"\tname rgb_ior\n"
		"\tvalues 1.3 1.5 2.0\n"
		"}\n";
	IJobPriv* pJob = LoadScene( scene, "rgb_values" );
	Check( pJob != nullptr, "rgb-values: scene loads" );
	if( !pJob ) return;

	IScalarPainter* p = pJob->GetScalarPainters()->GetItem( "rgb_ior" );
	Check( p != nullptr, "rgb-values: painter registered" );
	if( p ) {
		const auto ri = MakeDummyRig();
		const auto t = p->GetValuesAt( ri );
		Check( ApproxEq( t.v[0], 1.3 ) && ApproxEq( t.v[1], 1.5 ) && ApproxEq( t.v[2], 2.0 ),
			"rgb-values: GetValuesAt = (1.3, 1.5, 2.0)" );
		Check( p->HasPerChannelVariation(),
			"rgb-values: HasPerChannelVariation" );
	}
	safe_release( pJob );
}

static void TestSellmeier()
{
	std::cout << "TestSellmeier" << std::endl;
	// BK7 published coefficients.
	const char* scene =
		"scalar_painter\n"
		"{\n"
		"\tname bk7_ior\n"
		"\tsellmeier 1.03961212 0.231792344 1.01046945 0.00600069867 0.0200179144 103.560653\n"
		"}\n";
	IJobPriv* pJob = LoadScene( scene, "sellmeier" );
	Check( pJob != nullptr, "sellmeier: scene loads" );
	if( !pJob ) return;

	IScalarPainter* p = pJob->GetScalarPainters()->GetItem( "bk7_ior" );
	Check( p != nullptr, "sellmeier: painter registered" );
	if( p ) {
		const auto ri = MakeDummyRig();
		Check( ApproxEq( p->GetValueAtNM( ri, 587.6 ), 1.5168, 1e-3 ),
			"sellmeier: BK7 d-line ≈ 1.5168" );
	}
	safe_release( pJob );
}

static void TestPolynomial()
{
	std::cout << "TestPolynomial" << std::endl;
	// f(λ) = 1 + 0.001·λ.
	const char* scene =
		"scalar_painter\n"
		"{\n"
		"\tname linear_func\n"
		"\tpolynomial 1.0 0.001\n"
		"}\n";
	IJobPriv* pJob = LoadScene( scene, "polynomial" );
	Check( pJob != nullptr, "polynomial: scene loads" );
	if( !pJob ) return;

	IScalarPainter* p = pJob->GetScalarPainters()->GetItem( "linear_func" );
	Check( p != nullptr, "polynomial: painter registered" );
	if( p ) {
		const auto ri = MakeDummyRig();
		Check( ApproxEq( p->GetValueAtNM( ri, 500 ), 1.5 ),
			"polynomial: f(500) = 1.5" );
	}
	safe_release( pJob );
}

static void TestPiecewiseLinearFile()
{
	std::cout << "TestPiecewiseLinearFile" << std::endl;
	// Create a 2-column file alongside the scene.
	char path[512];
	std::snprintf( path, sizeof( path ),
		"/tmp/scalar_painter_test_pwl_%d.ior", (int)::getpid() );
	std::ofstream f( path );
	f << "380 1.10\n720 1.45\n";
	f.close();

	std::ostringstream scene;
	scene << "scalar_painter\n{\n\tname pwl_ior\n\tfile " << path << "\n}\n";

	IJobPriv* pJob = LoadScene( scene.str().c_str(), "piecewise" );
	Check( pJob != nullptr, "piecewise: scene loads" );
	if( pJob ) {
		IScalarPainter* p = pJob->GetScalarPainters()->GetItem( "pwl_ior" );
		Check( p != nullptr, "piecewise: painter registered" );
		if( p ) {
			const auto ri = MakeDummyRig();
			Check( ApproxEq( p->GetValueAtNM( ri, 380 ), 1.10 ),
				"piecewise: 380 → 1.10" );
			Check( ApproxEq( p->GetValueAtNM( ri, 720 ), 1.45 ),
				"piecewise: 720 → 1.45" );
			Check( ApproxEq( p->GetValueAtNM( ri, 550 ), 1.275 ),
				"piecewise: 550 → 1.275 (midpoint)" );
		}
		safe_release( pJob );
	}
	std::remove( path );
}

static void TestScaledComposition()
{
	std::cout << "TestScaledComposition" << std::endl;
	const char* scene =
		"scalar_painter\n"
		"{\n"
		"\tname base\n"
		"\tvalue 2.0\n"
		"}\n"
		"\n"
		"scalar_painter\n"
		"{\n"
		"\tname half\n"
		"\tbase base\n"
		"\tscale 0.5\n"
		"}\n";
	IJobPriv* pJob = LoadScene( scene, "scaled" );
	Check( pJob != nullptr, "scaled-compose: scene loads" );
	if( !pJob ) return;

	IScalarPainter* p = pJob->GetScalarPainters()->GetItem( "half" );
	Check( p != nullptr, "scaled-compose: painter registered" );
	if( p ) {
		const auto ri = MakeDummyRig();
		Check( ApproxEq( p->GetValueAtNM( ri, 555 ), 1.0 ),
			"scaled-compose: 2.0 × 0.5 = 1.0" );
	}
	safe_release( pJob );
}

static void TestMultiplyComposition()
{
	std::cout << "TestMultiplyComposition" << std::endl;
	const char* scene =
		"scalar_painter\n"
		"{\n"
		"\tname a\n"
		"\tvalue 3.0\n"
		"}\n"
		"\n"
		"scalar_painter\n"
		"{\n"
		"\tname b\n"
		"\tvalue 4.0\n"
		"}\n"
		"\n"
		"scalar_painter\n"
		"{\n"
		"\tname twelve\n"
		"\tmultiply a b\n"
		"}\n";
	IJobPriv* pJob = LoadScene( scene, "multiply" );
	Check( pJob != nullptr, "multiply: scene loads" );
	if( !pJob ) return;

	IScalarPainter* p = pJob->GetScalarPainters()->GetItem( "twelve" );
	Check( p != nullptr, "multiply: painter registered" );
	if( p ) {
		const auto ri = MakeDummyRig();
		Check( ApproxEq( p->GetValueAtNM( ri, 555 ), 12.0 ),
			"multiply: 3 × 4 = 12" );
	}
	safe_release( pJob );
}

static void TestRejectMissingForm()
{
	std::cout << "TestRejectMissingForm" << std::endl;
	const char* scene =
		"scalar_painter\n"
		"{\n"
		"\tname missing_form\n"
		"}\n";
	IJobPriv* pJob = LoadScene( scene, "missing_form" );
	Check( pJob == nullptr, "missing-form: scene REJECTED" );
	if( pJob ) safe_release( pJob );
}

static void TestRejectMultipleForms()
{
	std::cout << "TestRejectMultipleForms" << std::endl;
	const char* scene =
		"scalar_painter\n"
		"{\n"
		"\tname conflict\n"
		"\tvalue 1.5\n"
		"\tvalues 1.3 1.5 2.0\n"
		"}\n";
	IJobPriv* pJob = LoadScene( scene, "multi_form" );
	Check( pJob == nullptr, "multi-form: scene REJECTED" );
	if( pJob ) safe_release( pJob );
}

static void TestRejectUnderspecifiedValues()
{
	std::cout << "TestRejectUnderspecifiedValues" << std::endl;
	// `values 1.5` (one component) must be rejected — silently
	// zero-filling to (1.5, 0, 0) is a footgun.
	const char* scene =
		"scalar_painter\n"
		"{\n"
		"\tname under\n"
		"\tvalues 1.5\n"
		"}\n";
	IJobPriv* pJob = LoadScene( scene, "under_values" );
	Check( pJob == nullptr, "under-values: 1-component values REJECTED" );
	if( pJob ) safe_release( pJob );
}

static void TestRejectPolynomialGarbage()
{
	std::cout << "TestRejectPolynomialGarbage" << std::endl;
	// `polynomial 1.0 0.001 oops` must be rejected; silent
	// truncation would let typos through.
	const char* scene =
		"scalar_painter\n"
		"{\n"
		"\tname poly_bad\n"
		"\tpolynomial 1.0 0.001 oops\n"
		"}\n";
	IJobPriv* pJob = LoadScene( scene, "poly_garbage" );
	Check( pJob == nullptr, "polynomial-trailing-garbage REJECTED" );
	if( pJob ) safe_release( pJob );
}

int main()
{
	std::cout << "ScalarPainterParserTest" << std::endl;
	TestUniformScalarValue();
	TestRGBValues();
	TestSellmeier();
	TestPolynomial();
	TestPiecewiseLinearFile();
	TestScaledComposition();
	TestMultiplyComposition();
	TestRejectMissingForm();
	TestRejectMultipleForms();
	TestRejectUnderspecifiedValues();
	TestRejectPolynomialGarbage();
	std::cout << "\nResults: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
