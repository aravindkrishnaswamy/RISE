//////////////////////////////////////////////////////////////////////
//
//  ThinFilmSceneParseTest.cpp - Scene-language + public-API plumbing
//    test for thin-film GGX materials (Phase 2 piece C of the
//    thin-film-interference feature; see docs/THIN_FILM_INTERFERENCE.md
//    §4 / §7).
//
//    This piece wires `fresnel_mode thinfilm` + the `film_ior` /
//    `film_extinction` / `film_thickness` IScalarPainter slots through:
//
//      ggx_material chunk descriptor  (AsciiSceneParser.cpp)
//        -> Job::AddGGXMaterial        (Job.cpp; ResolveOrDiagnoseScalar
//                                       + ResolveFresnelMode + presence
//                                       contract)
//        -> RISE_API_CreateGGXMaterialThinFilm  (RISE_API.cpp; the
//                                       ABI-preserving sibling of the
//                                       frozen RISE_API_CreateGGXMaterial)
//        -> GGXMaterial / GGXBRDF / GGXSPF film slots  (P2-B, not touched
//                                       here).
//
//    The BSDF-level thin-film MATH is validated elsewhere (P1-A oracle,
//    P2-A/B unit tests).  This test owns the PLUMBING contract:
//
//      1. A scene with `ggx_material fresnel_mode thinfilm` + the three
//         film scalar_painters PARSES and registers a material whose BSDF
//         actually carries thin-film behaviour (verified by a direct
//         valueNM probe through the public factory).
//      2. The two parse-time diagnostics fire (the chunk fails to load,
//         so ParseAndLoadScene returns FALSE):
//           (a) cooktorrance_material + fresnel_mode thinfilm  (GGX-only)
//           (b) ggx_material fresnel_mode thinfilm WITHOUT film_thickness
//               (the P2-B BSDF dereferences the film slots in thinfilm
//                mode, so a missing slot is a hard parse error, not a
//                silent null-deref at render time).
//      3. ABI: the frozen RISE_API_CreateGGXMaterial symbol still
//         constructs a valid material (NULL film slots == pre-thin-film
//         behaviour), and the new thin-film factory plumbs the film slots
//         all the way into the BSDF (thickness-dependent reflectance).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <cstdio>
#include <cmath>
#include <string>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ISceneParser.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IBSDF.h"
#include "../src/Library/Materials/GGXMaterial.h"
#include "../src/Library/Materials/GGXBRDF.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/Color/Color.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int s_pass = 0;
	int s_fail = 0;

	void Check( bool ok, const char* what )
	{
		if( ok ) {
			++s_pass;
			std::cout << "  ok  : " << what << "\n";
		} else {
			++s_fail;
			std::cout << "  FAIL: " << what << "\n";
		}
	}

	// Write `body` to a uniquely-named temp .RISEscene file and return its
	// path.  Kept under the system temp dir so the test is hermetic and does
	// NOT depend on RISE_MEDIA_PATH (the scenes use inline scalar values, no
	// asset files).
	std::string WriteTempScene( const char* tag, const std::string& body )
	{
		std::string dir;
		const char* t = getenv( "TMPDIR" );
		if( t && t[0] ) {
			dir = t;
			if( dir.back() != '/' ) dir += '/';
		} else {
			dir = "/tmp/";
		}
		std::string path = dir + "rise_thinfilm_parse_" + tag + ".RISEscene";
		std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
		f << body;
		f.close();
		return path;
	}

	// Parse a scene file into a fresh Job.  Returns the ParseAndLoadScene
	// result; `job` is left holding whatever was loaded.
	bool ParseSceneFile( const std::string& path, Job& job )
	{
		ISceneParser* parser = 0;
		if( !RISE_API_CreateAsciiSceneParser( &parser, path.c_str() ) || !parser ) {
			return false;
		}
		parser->addref();
		const bool ok = parser->ParseAndLoadScene( job );
		parser->release();
		return ok;
	}

	// A minimal-but-complete thin-film GGX scene.  Inline scalar_painters
	// (no asset files) keep it hermetic.  `thickness` lets callers vary the
	// oxide thickness; `includeThickness` lets the missing-slot diagnostic
	// case drop film_thickness entirely.
	std::string ThinFilmGGXScene( double thickness, bool includeThickness )
	{
		std::string s;
		s += "RISE ASCII SCENE 6\n";
		s += "uniformcolor_painter\n{\nname rd\ncolor 0.0 0.0 0.0\n}\n";
		s += "uniformcolor_painter\n{\nname rs\ncolor 1.0 1.0 1.0\n}\n";
		s += "scalar_painter\n{\nname sub_n\nvalue 0.5\n}\n";			// Ti-ish substrate n
		s += "scalar_painter\n{\nname sub_k\nvalue 2.7\n}\n";			// Ti-ish substrate k
		s += "scalar_painter\n{\nname film_n\nvalue 2.5\n}\n";			// TiO2-ish film n
		s += "scalar_painter\n{\nname film_k\nvalue 0.0\n}\n";			// transparent film k
		char thk[64];
		std::snprintf( thk, sizeof(thk), "%.1f", thickness );
		s += std::string("scalar_painter\n{\nname film_t\nvalue ") + thk + "\n}\n";
		s += "scalar_painter\n{\nname rough\nvalue 0.1\n}\n";
		s += "ggx_material\n{\nname ti_heattint\n";
		s += "rd rd\nrs rs\nalphax rough\nalphay rough\n";
		s += "ior sub_n\nextinction sub_k\n";
		s += "fresnel_mode thinfilm\n";
		s += "film_ior film_n\nfilm_extinction film_k\n";
		if( includeThickness ) {
			s += "film_thickness film_t\n";
		}
		s += "}\n";
		return s;
	}

	// Build a RayIntersectionGeometric whose incident ray direction is
	// `rayDir`, surface normal +Z.  Mirrors the helper in GGXFresnelModeTest.
	RayIntersectionGeometric MakeRI( const Vector3& rayDir )
	{
		Ray inRay( Point3( 0, 0, 1 ), rayDir );
		RasterizerState rs = { 0, 0 };
		RayIntersectionGeometric ri( inRay, rs );
		ri.bHit = true;
		ri.range = 1.0;
		ri.ptIntersection = Point3( 0, 0, 0 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
		ri.ptCoord = Point2( 0.5, 0.5 );
		return ri;
	}

	// Sum |valueNM| over a few hero wavelengths at a near-normal half-vector
	// geometry — a scalar fingerprint of the BSDF's spectral response.  Two
	// thin-film stacks that differ only in film thickness produce different
	// fingerprints (interference depends on thickness), which proves the
	// thickness painter is plumbed through to the BSDF.
	double SpectralFingerprint( GGXBRDF* brdf )
	{
		// wi and the ray are arranged so the half-vector sits near the normal
		// (specular configuration), where the conductor Fresnel is replaced by
		// the thin-film reflectance.
		const Vector3 wi( 0.0, 0.0, 1.0 );
		RayIntersectionGeometric ri = MakeRI( Vector3( 0.0, 0.0, -1.0 ) );
		double acc = 0.0;
		const Scalar lambdas[] = { Scalar(450), Scalar(500), Scalar(550), Scalar(600), Scalar(650) };
		for( int i = 0; i < 5; ++i ) {
			const Scalar v = brdf->valueNM( wi, ri, lambdas[i] );
			acc += std::fabs( double(v) );
		}
		return acc;
	}
}

// ============================================================
//  Test 1: thin-film ggx_material parses + registers a material
//          whose BSDF carries thin-film behaviour.
// ============================================================
static void TestThinFilmParsesAndWires()
{
	std::cout << "\n[1] ggx_material fresnel_mode thinfilm parses + registers\n";

	const std::string path = WriteTempScene( "ok", ThinFilmGGXScene( 120.0, /*includeThickness*/ true ) );
	Job* job = new Job();
	job->addref();

	const bool parsed = ParseSceneFile( path, *job );
	Check( parsed, "scene with thinfilm ggx_material + 3 film painters parses (ParseAndLoadScene == true)" );

	// Pull the registered material back out and confirm it has a BSDF.
	IMaterial* mat = job->GetMaterials() ? job->GetMaterials()->GetItem( "ti_heattint" ) : 0;
	Check( mat != 0, "material `ti_heattint` was registered" );
	if( mat ) {
		IBSDF* bsdf = mat->GetBSDF();
		Check( bsdf != 0, "registered thinfilm material exposes a BSDF" );
		GGXBRDF* ggx = dynamic_cast<GGXBRDF*>( bsdf );
		Check( ggx != 0, "the BSDF is a GGXBRDF (thinfilm routes through GGX)" );
	}

	job->release();
	std::remove( path.c_str() );
}

// ============================================================
//  Test 2(a): cooktorrance_material + fresnel_mode thinfilm is REJECTED.
// ============================================================
static void TestCookTorranceThinFilmRejected()
{
	std::cout << "\n[2a] cooktorrance_material + fresnel_mode thinfilm is rejected\n";

	std::string s;
	s += "RISE ASCII SCENE 6\n";
	s += "uniformcolor_painter\n{\nname rd\ncolor 0.2 0.2 0.2\n}\n";
	s += "uniformcolor_painter\n{\nname rs\ncolor 0.8 0.8 0.8\n}\n";
	s += "scalar_painter\n{\nname facets\nvalue 0.15\n}\n";
	s += "scalar_painter\n{\nname ctn\nvalue 2.45\n}\n";
	s += "scalar_painter\n{\nname ctk\nvalue 1.0\n}\n";
	s += "cooktorrance_material\n{\nname ct_bad\n";
	s += "rd rd\nrs rs\nfacets facets\nior ctn\nextinction ctk\n";
	s += "fresnel_mode thinfilm\n";			// GGX-only — must be rejected
	s += "}\n";

	const std::string path = WriteTempScene( "ct_thinfilm", s );
	Job* job = new Job();
	job->addref();

	const bool parsed = ParseSceneFile( path, *job );
	Check( !parsed, "cooktorrance_material with fresnel_mode thinfilm fails to parse (diagnostic fired)" );

	IMaterial* mat = job->GetMaterials() ? job->GetMaterials()->GetItem( "ct_bad" ) : 0;
	Check( mat == 0, "rejected cooktorrance material was NOT registered" );

	job->release();
	std::remove( path.c_str() );
}

// ============================================================
//  Test 2(b): ggx_material fresnel_mode thinfilm WITHOUT film_thickness
//             is REJECTED (presence contract).
// ============================================================
static void TestThinFilmMissingThicknessRejected()
{
	std::cout << "\n[2b] ggx_material fresnel_mode thinfilm WITHOUT film_thickness is rejected\n";

	const std::string path = WriteTempScene( "missing_thk", ThinFilmGGXScene( 0.0, /*includeThickness*/ false ) );
	Job* job = new Job();
	job->addref();

	const bool parsed = ParseSceneFile( path, *job );
	Check( !parsed, "thinfilm ggx_material missing film_thickness fails to parse (presence contract fired)" );

	IMaterial* mat = job->GetMaterials() ? job->GetMaterials()->GetItem( "ti_heattint" ) : 0;
	Check( mat == 0, "incomplete thinfilm material was NOT registered" );

	job->release();
	std::remove( path.c_str() );
}

// ============================================================
//  Test 3: ABI + film-slot plumbing through the public factory.
//          - the frozen RISE_API_CreateGGXMaterial still builds a material
//          - RISE_API_CreateGGXMaterialThinFilm plumbs the film slots into
//            the BSDF, so two stacks differing only in film thickness give
//            different spectral fingerprints (and both differ from the
//            conductor-mode control).
// ============================================================
static void TestApiFactoryPlumbsFilmSlots()
{
	std::cout << "\n[3] RISE_API thin-film factory plumbs film slots into the BSDF\n";

	UniformColorPainter*  rd     = new UniformColorPainter( RISEPel( 0.0, 0.0, 0.0 ) ); rd->addref();
	UniformColorPainter*  rs     = new UniformColorPainter( RISEPel( 1.0, 1.0, 1.0 ) ); rs->addref();
	UniformScalarPainter* rough  = new UniformScalarPainter( 0.1 ); rough->addref();
	UniformScalarPainter* subN   = new UniformScalarPainter( 0.5 ); subN->addref();
	UniformScalarPainter* subK   = new UniformScalarPainter( 2.7 ); subK->addref();
	UniformScalarPainter* filmN  = new UniformScalarPainter( 2.5 ); filmN->addref();
	UniformScalarPainter* filmK  = new UniformScalarPainter( 0.0 ); filmK->addref();
	UniformScalarPainter* filmT1 = new UniformScalarPainter( 100.0 ); filmT1->addref();
	UniformScalarPainter* filmT2 = new UniformScalarPainter( 300.0 ); filmT2->addref();

	// (a) Frozen ABI symbol still constructs a valid material (NULL films).
	IMaterial* matLegacy = 0;
	const bool okLegacy = RISE_API_CreateGGXMaterial(
		&matLegacy, *rd, *rs, *rough, *rough, *subN, *subK, eFresnelConductor, nullptr );
	Check( okLegacy && matLegacy != 0, "frozen RISE_API_CreateGGXMaterial still builds a material" );

	// Conductor-mode control through the new thin-film factory (films = NULL).
	IMaterial* matCond = 0;
	RISE_API_CreateGGXMaterialThinFilm(
		&matCond, *rd, *rs, *rough, *rough, *subN, *subK,
		eFresnelConductor, nullptr, nullptr, nullptr, nullptr );

	// Two thin-film materials differing ONLY in film thickness.
	IMaterial* matThin1 = 0;
	RISE_API_CreateGGXMaterialThinFilm(
		&matThin1, *rd, *rs, *rough, *rough, *subN, *subK,
		eFresnelThinFilmConductor, nullptr, filmN, filmK, filmT1 );
	IMaterial* matThin2 = 0;
	RISE_API_CreateGGXMaterialThinFilm(
		&matThin2, *rd, *rs, *rough, *rough, *subN, *subK,
		eFresnelThinFilmConductor, nullptr, filmN, filmK, filmT2 );

	Check( matCond && matThin1 && matThin2, "thin-film factory built conductor + 2 thinfilm materials" );

	if( matCond && matThin1 && matThin2 ) {
		GGXBRDF* bCond  = dynamic_cast<GGXBRDF*>( matCond->GetBSDF() );
		GGXBRDF* bThin1 = dynamic_cast<GGXBRDF*>( matThin1->GetBSDF() );
		GGXBRDF* bThin2 = dynamic_cast<GGXBRDF*>( matThin2->GetBSDF() );
		Check( bCond && bThin1 && bThin2, "all three BSDFs downcast to GGXBRDF" );

		if( bCond && bThin1 && bThin2 ) {
			const double fCond  = SpectralFingerprint( bCond );
			const double fThin1 = SpectralFingerprint( bThin1 );
			const double fThin2 = SpectralFingerprint( bThin2 );
			std::cout << "    fingerprint conductor=" << fCond
					  << "  thinfilm(100nm)=" << fThin1
					  << "  thinfilm(300nm)=" << fThin2 << "\n";

			// thickness is plumbed: changing only film_thickness changes the
			// BSDF output.  A robust, scale-free comparison.
			const double dThick = std::fabs( fThin1 - fThin2 );
			const double scaleThk = std::max( std::fabs( fThin1 ), std::fabs( fThin2 ) ) + 1e-12;
			Check( dThick / scaleThk > 1e-3,
				"film_thickness is plumbed: 100nm vs 300nm give different BSDF response" );

			// the film slots actually change the result vs a bare conductor.
			const double dMode = std::fabs( fThin1 - fCond );
			const double scaleMode = std::max( std::fabs( fThin1 ), std::fabs( fCond ) ) + 1e-12;
			Check( dMode / scaleMode > 1e-3,
				"thinfilm BSDF differs from conductor BSDF (film slots take effect)" );
		}
	}

	if( matLegacy ) matLegacy->release();
	if( matCond )   matCond->release();
	if( matThin1 )  matThin1->release();
	if( matThin2 )  matThin2->release();
	rd->release(); rs->release(); rough->release();
	subN->release(); subK->release();
	filmN->release(); filmK->release(); filmT1->release(); filmT2->release();
}

//////////////////////////////////////////////////////////////////////
//  Test G: the thin-film API factory REJECTS thinfilm mode with a null
//  film_ior or film_thickness (no sensible default; the BSDF derefs both
//  unconditionally), and ACCEPTS a null film_extinction (k=0 default).
//  Regression for review round-2 P2: the scene path is guarded by Job's
//  presence contract, but RISE_API_CreateGGXMaterialThinFilm (the direct-
//  API path) was not -> the identical null-deref crash class.
//////////////////////////////////////////////////////////////////////
static void TestApiRejectsNullFilm()
{
	std::cout << "\n[4] RISE_API thin-film factory rejects null film_ior/film_thickness\n";
	
	UniformColorPainter*  rd    = new UniformColorPainter( RISEPel( 0.0, 0.0, 0.0 ) ); rd->addref();
	UniformColorPainter*  rs    = new UniformColorPainter( RISEPel( 1.0, 1.0, 1.0 ) ); rs->addref();
	UniformScalarPainter* rough = new UniformScalarPainter( 0.1 ); rough->addref();
	UniformScalarPainter* subN  = new UniformScalarPainter( 2.7 ); subN->addref();
	UniformScalarPainter* subK  = new UniformScalarPainter( 3.0 ); subK->addref();
	UniformScalarPainter* filmN = new UniformScalarPainter( 2.5 ); filmN->addref();
	UniformScalarPainter* filmT = new UniformScalarPainter( 90.0 ); filmT->addref();
	
	// (a) thinfilm + null film_ior -> reject (false, no material, no crash).
	IMaterial* m1 = 0;
	const bool ok1 = RISE_API_CreateGGXMaterialThinFilm(
		&m1, *rd, *rs, *rough, *rough, *subN, *subK,
		eFresnelThinFilmConductor, nullptr, /*film_ior=*/nullptr, nullptr, filmT );
	Check( !ok1 && m1 == 0, "thinfilm + null film_ior rejected (false, no material, no crash)" );
	
	// (b) thinfilm + null film_thickness -> reject.
	IMaterial* m2 = 0;
	const bool ok2 = RISE_API_CreateGGXMaterialThinFilm(
		&m2, *rd, *rs, *rough, *rough, *subN, *subK,
		eFresnelThinFilmConductor, nullptr, filmN, nullptr, /*film_thickness=*/nullptr );
	Check( !ok2 && m2 == 0, "thinfilm + null film_thickness rejected (false, no material, no crash)" );
	
	// (c) thinfilm + null film_extinction (k=0 default) -> ACCEPTED.
	IMaterial* m3 = 0;
	const bool ok3 = RISE_API_CreateGGXMaterialThinFilm(
		&m3, *rd, *rs, *rough, *rough, *subN, *subK,
		eFresnelThinFilmConductor, nullptr, filmN, /*film_extinction=*/nullptr, filmT );
	Check( ok3 && m3 != 0, "thinfilm + null film_extinction (k=0 default) accepted" );
	
	if( m3 ) m3->release();
	rd->release(); rs->release(); rough->release();
	subN->release(); subK->release(); filmN->release(); filmT->release();
}

int main()
{
	std::cout << "=== ThinFilmSceneParseTest -- scene-language + API plumbing ===\n";
	GlobalLog();	// initialize the global log

	TestThinFilmParsesAndWires();
	TestCookTorranceThinFilmRejected();
	TestThinFilmMissingThicknessRejected();
	TestApiFactoryPlumbsFilmSlots();
	TestApiRejectsNullFilm();

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
