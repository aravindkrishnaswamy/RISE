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
	ofs << "RISE ASCII SCENE 7\n\n" << body;
	ofs.close();

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		std::remove( path );
		return nullptr;
	}
	const bool ok = pJob->LoadAsciiSceneViaCst( path );
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

	// Sibling: GGX tangent_rotation (and the twin ggx_emissive / PBR anisotropy_
	// rotation) synthesise a uniform-colour painter from atof() -- an inline
	// non-finite value was accepted.  Now guarded up front with the same helper.
	const char* ggxCol = "uniformcolor_painter\n{\n\tname col\n\tcolor 0.5 0.5 0.5\n}\n";
	const char* ggxInf =
		"uniformcolor_painter\n{\n\tname col\n\tcolor 0.5 0.5 0.5\n}\n"
		"ggx_material\n{\n\tname ggx_inf\n\trd col\n\trs col\n\talphax 0.1\n\talphay 0.1\n\tior 0.15\n\textinction 3.5\n\ttangent_rotation inf\n}\n";
	IJobPriv* jgi = LoadScene( ggxInf, "ggx_inf" );
	Check( jgi == nullptr, "GGX `tangent_rotation inf` REJECTED" );
	if( jgi ) safe_release( jgi );

	const char* ggxOk =
		"uniformcolor_painter\n{\n\tname col\n\tcolor 0.5 0.5 0.5\n}\n"
		"ggx_material\n{\n\tname ggx_ok\n\trd col\n\trs col\n\talphax 0.1\n\talphay 0.1\n\tior 0.15\n\textinction 3.5\n\ttangent_rotation 0.5\n}\n";
	IJobPriv* jgo = LoadScene( ggxOk, "ggx_ok" );
	Check( jgo != nullptr, "GGX finite tangent_rotation (0.5) still loads" );
	if( jgo ) safe_release( jgo );
	(void)ggxCol;

	// PBR-MR scalar FACTORS (metallic / roughness / specular_factor /
	// anisotropy_factor) also fall back to atof() in resolveOrSynth -- an inline
	// non-finite value synthesised a non-finite uniform-colour painter.
	const char* pbrInf =
		"uniformcolor_painter\n{\n\tname bc\n\tcolor 0.8 0.8 0.8\n}\n"
		"pbr_metallic_roughness_material\n{\n\tname pbr_inf\n\tbase_color bc\n\tmetallic 0.0\n\troughness inf\n}\n";
	IJobPriv* jp = LoadScene( pbrInf, "pbr_inf" );
	Check( jp == nullptr, "PBR-MR `roughness inf` REJECTED" );
	if( jp ) safe_release( jp );

	const char* pbrOk =
		"uniformcolor_painter\n{\n\tname bc\n\tcolor 0.8 0.8 0.8\n}\n"
		"pbr_metallic_roughness_material\n{\n\tname pbr_ok\n\tbase_color bc\n\tmetallic 0.0\n\troughness 0.3\n}\n";
	IJobPriv* jpo = LoadScene( pbrOk, "pbr_ok" );
	Check( jpo != nullptr, "PBR-MR finite roughness (0.3) still loads" );
	if( jpo ) safe_release( jpo );

	// Light-shaderop sibling: arealight_shaderop `N` (Phong/directionality
	// exponent) is Reference-kind and falls back to atof() -> non-finite exponent.
	const char* alInf =
		"uniformcolor_painter\n{\n\tname em\n\tcolor 1 1 1\n}\n"
		"arealight_shaderop\n{\n\tname al_inf\n\temission em\n\tlocation 0 0 5\n\tmake_dir 0 0 0\n\tsamples 4\n\twidth 1\n\theight 1\n\tpower 10\n\tN inf\n\tcache false\n}\n";
	IJobPriv* jal = LoadScene( alInf, "al_inf" );
	Check( jal == nullptr, "arealight_shaderop `N inf` REJECTED" );
	if( jal ) safe_release( jal );

	const char* alOk =
		"uniformcolor_painter\n{\n\tname em\n\tcolor 1 1 1\n}\n"
		"arealight_shaderop\n{\n\tname al_ok\n\temission em\n\tlocation 0 0 5\n\tmake_dir 0 0 0\n\tsamples 4\n\twidth 1\n\theight 1\n\tpower 10\n\tN 1.0\n\tcache false\n}\n";
	IJobPriv* jalo = LoadScene( alOk, "al_ok" );
	Check( jalo != nullptr, "arealight_shaderop finite N (1.0) still loads" );
	if( jalo ) safe_release( jalo );

	// A painter whose NAME merely has a nan/inf PREFIX (inflection_map, nan_mask)
	// must NOT be rejected by the non-finite guard -- strtod matches the prefix but
	// the token isn't a whole number, so it reaches the painter lookup and resolves.
	const char* pnPrefix =
		"uniformcolor_painter\n{\n\tname col\n\tcolor 0.5 0.5 0.5\n}\n"
		"uniformcolor_painter\n{\n\tname inflection_map\n\tcolor 0.1 0.1 0.1\n}\n"
		"ggx_material\n{\n\tname ggx_pn\n\trd col\n\trs col\n\talphax 0.1\n\talphay 0.1\n\tior 0.15\n\textinction 3.5\n\ttangent_rotation inflection_map\n}\n";
	IJobPriv* jpn = LoadScene( pnPrefix, "ggx_pn" );
	Check( jpn != nullptr, "painter named `inflection_map` (inf-prefix) NOT wrongly rejected" );
	if( jpn ) safe_release( jpn );

	// randomwalk_sss_material max_bounces (Reference-kind) was atoi()'d, so inf /
	// nan / misspelled silently became 0.  Now validated as a non-negative integer.
	const char* rwInf =
		"randomwalk_sss_material\n{\n\tname rw_inf\n\tior 1.4\n\tabsorption 0.1\n\tscattering 1.0\n\tg 0.0\n\troughness 0.0\n\tmax_bounces inf\n}\n";
	IJobPriv* jri = LoadScene( rwInf, "rw_inf" );
	Check( jri == nullptr, "randomwalk `max_bounces inf` REJECTED" );
	if( jri ) safe_release( jri );

	const char* rwOk =
		"randomwalk_sss_material\n{\n\tname rw_ok\n\tior 1.4\n\tabsorption 0.1\n\tscattering 1.0\n\tg 0.0\n\troughness 0.0\n\tmax_bounces 32\n}\n";
	IJobPriv* jro = LoadScene( rwOk, "rw_ok" );
	Check( jro != nullptr, "randomwalk finite max_bounces (32) still loads" );
	if( jro ) safe_release( jro );

	// Non-finite followed by a # comment or junk must STILL be caught (inline
	// comments are tolerated syntax; atof would yield inf).  Inline (unregistered)
	// tangent_rotation on a lookup-first slot.
	const char* rotComment =
		"uniformcolor_painter\n{\n\tname col\n\tcolor 0.5 0.5 0.5\n}\n"
		"ggx_material\n{\n\tname ggx_c\n\trd col\n\trs col\n\talphax 0.1\n\talphay 0.1\n\tior 0.15\n\textinction 3.5\n\ttangent_rotation inf # deg\n}\n";
	IJobPriv* jrc = LoadScene( rotComment, "ggx_c" );
	Check( jrc == nullptr, "GGX `tangent_rotation inf # comment` REJECTED" );
	if( jrc ) safe_release( jrc );

	const char* rotJunk =
		"uniformcolor_painter\n{\n\tname col\n\tcolor 0.5 0.5 0.5\n}\n"
		"ggx_material\n{\n\tname ggx_j\n\trd col\n\trs col\n\talphax 0.1\n\talphay 0.1\n\tior 0.15\n\textinction 3.5\n\ttangent_rotation 1e999x\n}\n";
	IJobPriv* jrj = LoadScene( rotJunk, "ggx_j" );
	Check( jrj == nullptr, "GGX `tangent_rotation 1e999x` REJECTED" );
	if( jrj ) safe_release( jrj );

	// max_bounces must reject a partial token (32junk) and a value that overflows
	// unsigned int (4294967296 > UINT_MAX), not silently truncate/wrap.
	const char* mbJunk =
		"randomwalk_sss_material\n{\n\tname rw_j\n\tior 1.4\n\tabsorption 0.1\n\tscattering 1.0\n\tg 0.0\n\troughness 0.0\n\tmax_bounces 32junk\n}\n";
	IJobPriv* jmj = LoadScene( mbJunk, "rw_j" );
	Check( jmj == nullptr, "randomwalk `max_bounces 32junk` REJECTED" );
	if( jmj ) safe_release( jmj );

	const char* mbBig =
		"randomwalk_sss_material\n{\n\tname rw_big\n\tior 1.4\n\tabsorption 0.1\n\tscattering 1.0\n\tg 0.0\n\troughness 0.0\n\tmax_bounces 4294967296\n}\n";
	IJobPriv* jmb = LoadScene( mbBig, "rw_big" );
	Check( jmb == nullptr, "randomwalk `max_bounces 4294967296` (>UINT_MAX) REJECTED" );
	if( jmb ) safe_release( jmb );

	// UNDERFLOW (5e-400) sets ERANGE but is a FINITE 0.0 -- it must NOT be rejected
	// as non-finite (only overflow is).
	const char* ufOk =
		"randomwalk_sss_material\n{\n\tname rw_uf\n\tior 1.4\n\tabsorption 0.1\n\tscattering 1.0\n\tg 5e-400\n\troughness 0.0\n\tmax_bounces 32\n}\n";
	IJobPriv* juf = LoadScene( ufOk, "rw_uf" );
	Check( juf != nullptr, "underflow `g 5e-400` (finite 0) NOT rejected" );
	if( juf ) safe_release( juf );

	// OVERFLOW with trailing junk that itself contains `e-` (1e999e-5): strtod
	// consumes `1e999` (overflow, atof -> +inf); the underflow scan must look only
	// at the consumed token, not the trailing `e-5`, so this is still REJECTED.
	const char* ofJunk =
		"uniformcolor_painter\n{\n\tname col\n\tcolor 0.5 0.5 0.5\n}\n"
		"ggx_material\n{\n\tname ggx_of\n\trd col\n\trs col\n\talphax 0.1\n\talphay 0.1\n\tior 0.15\n\textinction 3.5\n\ttangent_rotation 1e999e-5\n}\n";
	IJobPriv* jof = LoadScene( ofJunk, "ggx_of" );
	Check( jof == nullptr, "overflow-with-junk `1e999e-5` REJECTED (scan bounded to token)" );
	if( jof ) safe_release( jof );

	// FINITE-BUT-INVALID junk (atof would silently coerce): a finite numeric prefix
	// with a trailing unit (`0.5rad`->0.5) and a mistyped painter name that resolves
	// to no painter (`roughnes_tex`->0.0) must both be REJECTED, not coerced.
	const char* rotUnit =
		"uniformcolor_painter\n{\n\tname col\n\tcolor 0.5 0.5 0.5\n}\n"
		"ggx_material\n{\n\tname ggx_ru\n\trd col\n\trs col\n\talphax 0.1\n\talphay 0.1\n\tior 0.15\n\textinction 3.5\n\ttangent_rotation 0.5rad\n}\n";
	IJobPriv* jru = LoadScene( rotUnit, "ggx_ru" );
	Check( jru == nullptr, "GGX `tangent_rotation 0.5rad` (finite+unit junk) REJECTED" );
	if( jru ) safe_release( jru );

	const char* rotTypo =
		"uniformcolor_painter\n{\n\tname col\n\tcolor 0.5 0.5 0.5\n}\n"
		"ggx_material\n{\n\tname ggx_rt\n\trd col\n\trs col\n\talphax 0.1\n\talphay 0.1\n\tior 0.15\n\textinction 3.5\n\ttangent_rotation roughnes_tex\n}\n";
	IJobPriv* jrt = LoadScene( rotTypo, "ggx_rt" );
	Check( jrt == nullptr, "GGX `tangent_rotation roughnes_tex` (unregistered name) REJECTED" );
	if( jrt ) safe_release( jrt );

	// SSS g/roughness are BAKED scalars (plain doubles through the ctor).  Junk
	// (`nope`) is rejected, and so is a REGISTERED painter name: the old
	// lookup-first tolerance passed validation and then silently atof'd the
	// painter NAME to g = 0.0 -- silent physical drift, now a loud refusal.
	const char* sssJunk =
		"subsurfacescattering_material\n{\n\tname sss_j\n\tior 1.4\n\tabsorption 0.1\n\tscattering 1.0\n\tg nope\n\troughness 0.0\n}\n";
	IJobPriv* jsj = LoadScene( sssJunk, "sss_j" );
	Check( jsj == nullptr, "SSS `g nope` (not a number) REJECTED" );
	if( jsj ) safe_release( jsj );

	const char* sssName =
		"scalar_painter\n{\n\tname pnt_zero\n\tvalue 0.0\n}\n"
		"subsurfacescattering_material\n{\n\tname sss_n\n\tior 1.4\n\tabsorption 0.1\n\tscattering 1.0\n\tg pnt_zero\n\troughness 0.0\n}\n";
	IJobPriv* jsn = LoadScene( sssName, "sss_n" );
	Check( jsn == nullptr, "SSS `g pnt_zero` (painter name on a BAKED scalar field) REJECTED -- no silent atof-to-0" );
	if( jsn ) safe_release( jsn );

	// C99 hex-float exponents: a hex UNDERFLOW (`0x1p-5000` -> finite 0) must load,
	// a hex OVERFLOW (`0x1p5000` -> +inf) must be rejected -- the ERANGE scan reads
	// the `p-`/`p` binary exponent, not just decimal `e-`.
	const char* hexUf =
		"subsurfacescattering_material\n{\n\tname sss_hu\n\tior 1.4\n\tabsorption 0.1\n\tscattering 1.0\n\tg 0x1p-5000\n\troughness 0.0\n}\n";
	IJobPriv* jhu = LoadScene( hexUf, "sss_hu" );
	Check( jhu != nullptr, "SSS hex-underflow `g 0x1p-5000` (finite 0) NOT rejected" );
	if( jhu ) safe_release( jhu );

	const char* hexOf =
		"subsurfacescattering_material\n{\n\tname sss_ho\n\tior 1.4\n\tabsorption 0.1\n\tscattering 1.0\n\tg 0x1p5000\n\troughness 0.0\n}\n";
	IJobPriv* jho = LoadScene( hexOf, "sss_ho" );
	Check( jho == nullptr, "SSS hex-overflow `g 0x1p5000` (+inf) REJECTED" );
	if( jho ) safe_release( jho );
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
