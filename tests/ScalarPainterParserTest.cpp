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

static void TestRejectInlineScalarOverflow()
{
	std::cout << "TestRejectInlineScalarOverflow" << std::endl;
	// A material scalar resolved via Job::ResolveScalarPainterArg with an inline
	// numeric value that OVERFLOWS (strtod -> HUGE_VAL, errno==ERANGE) must be
	// REJECTED loudly, not silently bound as an inf painter.
	const char* ovf1 =
		"dielectric_material\n{\n\tname ovf_single\n\ttau 1.0\n\tior 1e999\n\tscattering 100000\n}\n";
	IJobPriv* j1 = LoadScene( ovf1, "ovf_single" );
	Check( j1 == nullptr, "inline scalar overflow (ior 1e999) REJECTED" );
	if( j1 ) safe_release( j1 );

	const char* ovf3 =
		"dielectric_material\n{\n\tname ovf_triple\n\ttau 1.0\n\tior 1 1 1e999\n\tscattering 100000\n}\n";
	IJobPriv* j3 = LoadScene( ovf3, "ovf_triple" );
	Check( j3 == nullptr, "inline scalar overflow (ior 1 1 1e999) REJECTED" );
	if( j3 ) safe_release( j3 );

	// Non-finite SPELLINGS (inf / nan) also bypass the descriptor's
	// AllTokensAreFiniteNumbers gate (Reference-kind slots) and reach
	// ResolveScalarPainterArg; strtod succeeds on them WITHOUT errno==ERANGE, so
	// the string-layer spelling check must reject them too.
	const char* inf1 =
		"dielectric_material\n{\n\tname inf_single\n\ttau 1.0\n\tior inf\n\tscattering 100000\n}\n";
	IJobPriv* ji = LoadScene( inf1, "inf_single" );
	Check( ji == nullptr, "inline scalar inf (ior inf) REJECTED" );
	if( ji ) safe_release( ji );

	const char* nan3 =
		"dielectric_material\n{\n\tname nan_triple\n\ttau 1.0\n\tior 1 1 nan\n\tscattering 100000\n}\n";
	IJobPriv* jn = LoadScene( nan3, "nan_triple" );
	Check( jn == nullptr, "inline scalar nan (ior 1 1 nan) REJECTED" );
	if( jn ) safe_release( jn );

	// Sibling: SSS `g` / `roughness` are Reference-kind too but were parsed via
	// plain atof() (no ERANGE, no nan/inf detection) -- now routed through the
	// same string-layer ParseFiniteScalarLiteral check.
	const char* sssG =
		"subsurfacescattering_material\n{\n\tname sss_g\n\tior 1.4\n\tabsorption 0.1\n\tscattering 1.0\n\tg inf\n\troughness 0.0\n}\n";
	IJobPriv* jg = LoadScene( sssG, "sss_g" );
	Check( jg == nullptr, "SSS `g inf` REJECTED (atof sibling)" );
	if( jg ) safe_release( jg );

	const char* sssR =
		"subsurfacescattering_material\n{\n\tname sss_r\n\tior 1.4\n\tabsorption 0.1\n\tscattering 1.0\n\tg 0.0\n\troughness 1e999\n}\n";
	IJobPriv* jr = LoadScene( sssR, "sss_r" );
	Check( jr == nullptr, "SSS `roughness 1e999` REJECTED (atof sibling)" );
	if( jr ) safe_release( jr );

	// Control: a finite inline ior still loads (fix must not reject good input).
	const char* okm =
		"dielectric_material\n{\n\tname ok_mat\n\ttau 1.0\n\tior 1.55\n\tscattering 100000\n}\n";
	IJobPriv* jok = LoadScene( okm, "ok_mat" );
	Check( jok != nullptr, "finite inline ior (1.55) still loads" );
	if( jok ) safe_release( jok );

	// Control: a finite SSS with g/roughness still loads.
	const char* sssOk =
		"subsurfacescattering_material\n{\n\tname sss_ok\n\tior 1.4\n\tabsorption 0.1\n\tscattering 1.0\n\tg 0.5\n\troughness 0.2\n}\n";
	IJobPriv* jso = LoadScene( sssOk, "sss_ok" );
	Check( jso != nullptr, "finite SSS (g 0.5, roughness 0.2) still loads" );
	if( jso ) safe_release( jso );
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
	TestRejectInlineScalarOverflow();
	std::cout << "\nResults: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
