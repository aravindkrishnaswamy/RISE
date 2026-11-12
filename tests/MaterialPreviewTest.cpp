//////////////////////////////////////////////////////////////////////
//
//  MaterialPreviewTest.cpp - Standalone headless test suite for the
//    3D studio shaderball MaterialPreview engine (Slice 5), spectral
//    material differentiators D1-D3 (Slice 6), SceneEditController +
//    RISE_API C-ABI preview/preset wrappers, and the shared cross-
//    platform interactive material-preview viewport pipeline
//    (CreateInteractiveMaterialPreviewPipeline: 1-SPP live drag +
//    multi-SPP pointer-up polish pass).
//
//  Self-contained: no RISE_MEDIA_PATH required; inline native-v7
//  scenes written to per-process temp files; fast-tier (<1 s).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
	#include <process.h>
	#define rise_test_getpid _getpid
#else
	#include <unistd.h>
	#define rise_test_getpid getpid
#endif

#include "../src/Library/Agent/InMemoryRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRayCaster.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Rendering/InteractivePelRasterizer.h"
#include "../src/Library/SceneEditor/MaterialPreview.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Utilities/FiniteMath.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int g_pass = 0;
	int g_fail = 0;

	void Check( bool c, const char* label )
	{
		if( c ) {
			++g_pass;
		} else {
			++g_fail;
			std::cout << "  FAIL: " << label << std::endl;
		}
	}

	std::string TempPath( const char* stem )
	{
		std::error_code ec;
		std::filesystem::path dir = std::filesystem::temp_directory_path( ec );
		if( ec || dir.empty() ) {
			const char* envTmp = std::getenv( "TMPDIR" );
			dir = ( envTmp && *envTmp ) ? std::filesystem::path( envTmp ) : std::filesystem::path( "." );
		}
		char buf[256];
		std::snprintf( buf, sizeof( buf ), "rise_matprev_%d_%s", (int)rise_test_getpid(), stem );
		return ( dir / buf ).string();
	}

	Job* LoadSceneFromText( const char* text, const std::string& path )
	{
		{
			std::ofstream o( path.c_str(), std::ios::binary );
			o << text;
		}
		Job* pJob = new Job();
		if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
			pJob->release();
			std::remove( path.c_str() );
			return nullptr;
		}
		return pJob;
	}

	static const char* const kShaderballFixture =
		"RISE ASCII SCENE 7\n"
		"uniformcolor_painter\n{\n\tname pnt_blue\n\tcolor 0.2 0.4 0.7\n}\n"
		"uniformcolor_painter\n{\n\tname pnt_white\n\tcolor 1.0 1.0 1.0\n}\n"
		"uniformcolor_painter\n{\n\tname pnt_black\n\tcolor 0.0 0.0 0.0\n}\n"
		"uniformcolor_painter\n{\n\tname pnt_gold\n\tcolor 0.95 0.72 0.25\n}\n"
		"scalar_painter\n{\n\tname sp_alpha\n\tvalue 0.08\n}\n"
		"scalar_painter\n{\n\tname sp_ior_15\n\tvalue 1.5\n}\n"
		"scalar_painter\n{\n\tname sp_ior_gold\n\tvalue 0.27\n}\n"
		"scalar_painter\n{\n\tname sp_ext_gold\n\tvalue 2.95\n}\n"
		"scalar_painter\n{\n\tname sp_ext_22\n\tvalue 2.2\n}\n"
		"scalar_painter\n{\n\tname sp_film_ior\n\tvalue 1.38\n}\n"
		"scalar_painter\n{\n\tname sp_thick_380\n\tvalue 380\n}\n"
		"scalar_painter\n{\n\tname sp_thick_520\n\tvalue 520\n}\n"
		"scalar_painter\n{\n\tname sp_tau\n\tvalue 0.9999\n}\n"
		"scalar_painter\n{\n\tname sp_sellmeier\n\tsellmeier 1.03961212 0.231792344 1.01046945 0.00600069867 0.0200179144 103.560653\n}\n"
		"scalar_painter\n{\n\tname sp_sss_ior\n\tvalue 1.38\n}\n"
		"scalar_painter\n{\n\tname sp_sss_abs\n\tvalues 0.01 1.20 5.00\n}\n"
		"scalar_painter\n{\n\tname sp_sss_scat\n\tvalues 2.40 1.20 0.25\n}\n"
		"lambertian_material\n{\n\tname mat_lambert\n\treflectance pnt_blue\n}\n"
		"ggx_material\n{\n\tname mat_conductor\n\trd pnt_black\n\trs pnt_gold\n\talphax sp_alpha\n\talphay sp_alpha\n\tfresnel_mode conductor\n\tior sp_ior_gold\n\textinction sp_ext_gold\n}\n"
		"ggx_material\n{\n\tname mat_thinfilm_380\n\trd pnt_black\n\trs pnt_white\n\talphax sp_alpha\n\talphay sp_alpha\n\tfresnel_mode thinfilm\n\tior sp_ior_15\n\textinction sp_ext_22\n\tfilm_ior sp_film_ior\n\tfilm_thickness sp_thick_380\n}\n"
		"ggx_material\n{\n\tname mat_thinfilm_520\n\trd pnt_black\n\trs pnt_white\n\talphax sp_alpha\n\talphay sp_alpha\n\tfresnel_mode thinfilm\n\tior sp_ior_15\n\textinction sp_ext_22\n\tfilm_ior sp_film_ior\n\tfilm_thickness sp_thick_520\n}\n"
		"dielectric_material\n{\n\tname mat_glass\n\ttau sp_tau\n\tior sp_sellmeier\n\tscattering sp_alpha\n}\n"
		"randomwalk_sss_material\n{\n\tname mat_sss\n\tior sp_sss_ior\n\tabsorption sp_sss_abs\n\tscattering sp_sss_scat\n\tg 0.75\n\troughness 0.15\n\tmax_bounces 32\n}\n"
		"lambertian_luminaire_material\n{\n\tname mat_lum\n\texitance pnt_white\n\tscale 5.0\n\tmaterial none\n}\n"
		"sphere_geometry\n{\n\tname geom_s\n\tradius 1.0\n}\n"
		"standard_object\n{\n\tname obj_s\n\tgeometry geom_s\n\tmaterial mat_lambert\n}\n";

	static const char* const kInteractivePreviewScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
		"film\n{\n\twidth 48\n\theight 36\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 0.35 3.4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 42.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_clay\n\tcolor 0.72 0.32 0.22\n}\n"
		"uniformcolor_painter\n{\n\tname pnt_metal\n\tcolor 0.92 0.78 0.35\n}\n"
		"uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor 0.55 0.55 0.58\n}\n"
		"uniformcolor_painter\n{\n\tname pnt_black\n\tcolor 0.0 0.0 0.0\n}\n"
		"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n"
		"scalar_painter\n{\n\tname sp_rough\n\tvalue 0.12\n}\n"
		"scalar_painter\n{\n\tname sp_ior\n\tvalue 0.35\n}\n"
		"scalar_painter\n{\n\tname sp_ext\n\tvalue 2.8\n}\n"
		"lambertian_material\n{\n\tname mat_clay\n\treflectance pnt_clay\n}\n"
		"ggx_material\n{\n\tname mat_metal\n\trd pnt_black\n\trs pnt_metal\n\talphax sp_rough\n\talphay sp_rough\n\tfresnel_mode conductor\n\tior sp_ior\n\textinction sp_ext\n}\n"
		"lambertian_material\n{\n\tname mat_floor\n\treflectance pnt_floor\n}\n"
		"lambertian_luminaire_material\n{\n\tname mat_key\n\texitance pnt_emit\n\tscale 18.0\n\tmaterial none\n}\n"
		"sphere_geometry\n{\n\tname sph_left\n\tradius 0.55\n}\n"
		"sphere_geometry\n{\n\tname sph_right\n\tradius 0.55\n}\n"
		"clippedplane_geometry\n{\n\tname floor_quad\n\tpta -3.0 -0.55 -3.0\n\tptb -3.0 -0.55 3.0\n\tptc 3.0 -0.55 3.0\n\tptd 3.0 -0.55 -3.0\n}\n"
		"clippedplane_geometry\n{\n\tname key_quad\n\tpta -0.8 2.2 1.8\n\tptb 0.8 2.2 1.8\n\tptc 0.8 1.4 1.8\n\tptd -0.8 1.4 1.8\n}\n"
		"standard_object\n{\n\tname obj_left\n\tgeometry sph_left\n\tposition -0.68 0 0\n\tmaterial mat_clay\n}\n"
		"standard_object\n{\n\tname obj_right\n\tgeometry sph_right\n\tposition 0.68 0 0\n\tmaterial mat_metal\n}\n"
		"standard_object\n{\n\tname obj_floor\n\tgeometry floor_quad\n\tmaterial mat_floor\n}\n"
		"standard_object\n{\n\tname obj_key\n\tgeometry key_quad\n\tmaterial mat_key\n}\n";

	// ------------------------------------------------------------------
	// 1. Direct 3D shaderball engine across material families, refusals,
	//    and sample-budget decimation
	// ------------------------------------------------------------------
	void TestShaderballFamiliesAndRefusals()
	{
		std::cout << "1. MaterialPreview::RenderMaterialPreview across material families, refusals, and budget decimation..." << std::endl;
		const std::string path = TempPath( "shaderball.RISEscene" );
		Job* j = LoadSceneFromText( kShaderballFixture, path );
		Check( j != nullptr, "shaderball fixture loads via CST" );
		if( !j ) return;

		const unsigned int W = 36, H = 36;

		// (a) Lambertian 3D directional studio shading + determinism
		const MaterialPreview::Result lam1 = MaterialPreview::RenderMaterialPreview( *j, String( "mat_lambert" ), W, H );
		const MaterialPreview::Result lam2 = MaterialPreview::RenderMaterialPreview( *j, String( "mat_lambert" ), W, H );
		Check( lam1.status == MaterialPreview::Status::Ok, "lambertian_material shaderball succeeds" );
		Check( lam1.width == W && lam1.height == H && lam1.rgba.size() == (std::size_t)W * H * 4,
		       "lambertian_material output matches W*H*4" );
		Check( lam1.rgba == lam2.rgba, "lambertian_material shaderball is deterministic" );

		if( lam1.status == MaterialPreview::Status::Ok ) {
			const std::size_t idxKey  = ( (std::size_t)12 * W + 12 ) * 4;
			const std::size_t idxFill = ( (std::size_t)24 * W + 24 ) * 4;
			const int lumaKey  = (int)lam1.rgba[idxKey]  + (int)lam1.rgba[idxKey+1]  + (int)lam1.rgba[idxKey+2];
			const int lumaFill = (int)lam1.rgba[idxFill] + (int)lam1.rgba[idxFill+1] + (int)lam1.rgba[idxFill+2];
			Check( lumaKey > lumaFill, "key-lit upper-left hemisphere is brighter than shadow hemisphere" );
			Check( lam1.rgba[idxKey+2] > lam1.rgba[idxKey+1] && lam1.rgba[idxKey+1] > lam1.rgba[idxKey+0],
			       "blue-dominant reflectance (0.2, 0.4, 0.7) preserved on lit sphere" );
			Check( lam1.rgba[idxKey+3] == 255, "alpha channel is opaque 255" );
		}

		// (b) GGX conductor
		const MaterialPreview::Result cond = MaterialPreview::RenderMaterialPreview( *j, String( "mat_conductor" ), W, H );
		Check( cond.status == MaterialPreview::Status::Ok, "ggx_material conductor shaderball succeeds" );
		if( cond.status == MaterialPreview::Status::Ok && lam1.status == MaterialPreview::Status::Ok ) {
			Check( cond.rgba != lam1.rgba, "conductor GGX shaderball differs from diffuse shaderball" );
		}

		// (c) GGX thin-film spectral interference
		const MaterialPreview::Result tf380 = MaterialPreview::RenderMaterialPreview( *j, String( "mat_thinfilm_380" ), W, H );
		const MaterialPreview::Result tf520 = MaterialPreview::RenderMaterialPreview( *j, String( "mat_thinfilm_520" ), W, H );
		Check( tf380.status == MaterialPreview::Status::Ok && tf520.status == MaterialPreview::Status::Ok,
		       "ggx_material thinfilm (380nm & 520nm) shaderballs succeed" );
		if( tf380.status == MaterialPreview::Status::Ok && tf520.status == MaterialPreview::Status::Ok ) {
			Check( tf380.rgba != tf520.rgba,
			       "12-wavelength spectral quadrature distinguishes 380nm vs 520nm thin-film thickness" );
		}

		// (d) Sellmeier dielectric glass refraction
		const MaterialPreview::Result glass = MaterialPreview::RenderMaterialPreview( *j, String( "mat_glass" ), W, H );
		Check( glass.status == MaterialPreview::Status::Ok, "dielectric_material Sellmeier glass shaderball succeeds" );
		if( glass.status == MaterialPreview::Status::Ok && lam1.status == MaterialPreview::Status::Ok ) {
			Check( glass.rgba != lam1.rgba, "refractive glass shaderball differs from opaque diffuse shaderball" );
		}

		// (e) Random-walk SSS wrap preview
		const MaterialPreview::Result sss = MaterialPreview::RenderMaterialPreview( *j, String( "mat_sss" ), W, H );
		Check( sss.status == MaterialPreview::Status::Ok, "randomwalk_sss_material shaderball succeeds" );
		if( sss.status == MaterialPreview::Status::Ok ) {
			const std::size_t cIdx = ( (std::size_t)( H / 2 ) * W + ( W / 2 ) ) * 4;
			// Absorption is much lower in red than blue (0.01 vs 5.00, equal scattering 3.0)
			Check( sss.rgba[cIdx+0] > sss.rgba[cIdx+2],
			       "SSS wrap preview reflects lower red absorption / stronger red scattering" );
		}

		// (f) Emissive luminaire
		const MaterialPreview::Result lum = MaterialPreview::RenderMaterialPreview( *j, String( "mat_lum" ), W, H );
		Check( lum.status == MaterialPreview::Status::Ok, "lambertian_luminaire_material shaderball succeeds" );
		if( lum.status == MaterialPreview::Status::Ok ) {
			const std::size_t cIdx = ( (std::size_t)( H / 2 ) * W + ( W / 2 ) ) * 4;
			Check( lum.rgba[cIdx] > 200 && lum.rgba[cIdx+1] > 200 && lum.rgba[cIdx+2] > 200,
			       "emissive luminaire renders bright white at sphere center" );
		}

		// (g) Sample-budget decimation + upscale (128*128 = 16384 > kMaxSampleBudget = 9216)
		const unsigned int bigW = 128, bigH = 128;
		Check( (std::size_t)bigW * bigH > MaterialPreview::kMaxSampleBudget,
		       "128x128 exceeds kMaxSampleBudget so decimated upscale path is exercised" );
		const MaterialPreview::Result decimated = MaterialPreview::RenderMaterialPreview( *j, String( "mat_lambert" ), bigW, bigH );
		Check( decimated.status == MaterialPreview::Status::Ok &&
		       decimated.width == bigW && decimated.height == bigH &&
		       decimated.rgba.size() == (std::size_t)bigW * bigH * 4,
		       "over-budget request decimates internally and upscales cleanly to requested 128x128" );

		// (h) Refusals
		const MaterialPreview::Result emptyName = MaterialPreview::RenderMaterialPreview( *j, String( "" ), W, H );
		Check( emptyName.status == MaterialPreview::Status::Refused && !emptyName.refusalReason.empty(),
		       "empty material name refuses with reason" );
		const MaterialPreview::Result nullMat = MaterialPreview::RenderMaterialPreview( *j, String( "none" ), W, H );
		Check( nullMat.status == MaterialPreview::Status::Refused && !nullMat.refusalReason.empty(),
		       "'none' material refuses with reason" );
		const MaterialPreview::Result unknownMat = MaterialPreview::RenderMaterialPreview( *j, String( "does_not_exist" ), W, H );
		Check( unknownMat.status == MaterialPreview::Status::Refused && !unknownMat.refusalReason.empty(),
		       "unresolved material refuses with reason" );
		const MaterialPreview::Result zeroW = MaterialPreview::RenderMaterialPreview( *j, String( "mat_lambert" ), 0, H );
		const MaterialPreview::Result zeroH = MaterialPreview::RenderMaterialPreview( *j, String( "mat_lambert" ), W, 0 );
		Check( zeroW.status == MaterialPreview::Status::Refused && zeroH.status == MaterialPreview::Status::Refused,
		       "zero width or height refuses" );
		const MaterialPreview::Result overW = MaterialPreview::RenderMaterialPreview( *j, String( "mat_lambert" ), MaterialPreview::kMaxDim + 1, H );
		const MaterialPreview::Result overH = MaterialPreview::RenderMaterialPreview( *j, String( "mat_lambert" ), W, MaterialPreview::kMaxDim + 1 );
		Check( overW.status == MaterialPreview::Status::Refused && overH.status == MaterialPreview::Status::Refused,
		       "dimensions exceeding kMaxDim refuse" );

		j->release();
		std::remove( path.c_str() );
	}

	// ------------------------------------------------------------------
	// 2. Direct Spectral Differentiator helpers (D1/D2/D3) & C-ABI
	// ------------------------------------------------------------------
	void TestSpectralHelpersAndControllerCAbi()
	{
		std::cout << "2. Spectral differentiators (D1 Blackbody/Sellmeier, D2 Measured Metals, D3 Thin-Film) & C-ABI..." << std::endl;
		const std::string path = TempPath( "spectral_cabi.RISEscene" );
		Job* j = LoadSceneFromText( kShaderballFixture, path );
		Check( j != nullptr, "fixture loads for spectral & C-ABI checks" );
		if( !j ) return;

		// Direct D1 Blackbody strip + evaluation + refusal
		const MaterialPreview::Result bb = MaterialPreview::RenderBlackbodyKelvinStrip( 64, 6, 1000.0, 12000.0 );
		Check( bb.status == MaterialPreview::Status::Ok && bb.rgba.size() == 64u * 6u * 4u,
		       "RenderBlackbodyKelvinStrip succeeds" );
		const MaterialPreview::Result bbBad = MaterialPreview::RenderBlackbodyKelvinStrip( 64, 6, 8000.0, 2000.0 );
		Check( bbBad.status == MaterialPreview::Status::Refused,
		       "RenderBlackbodyKelvinStrip refuses inverted minK >= maxK" );

		double peak5500 = 0.0;
		unsigned char rgba1800[4] = {0}, rgba10000[4] = {0};
		Check( MaterialPreview::EvaluateBlackbodyKelvin( 1800.0, nullptr, rgba1800 ) &&
		       MaterialPreview::EvaluateBlackbodyKelvin( 5500.0, &peak5500, nullptr ) &&
		       MaterialPreview::EvaluateBlackbodyKelvin( 10000.0, nullptr, rgba10000 ),
		       "EvaluateBlackbodyKelvin succeeds for 1800K, 5500K, 10000K" );
		Check( std::fabs( peak5500 - 526.8676 ) < 0.1, "Wien peak at 5500K matches ~526.87 nm" );
		Check( rgba1800[0] > rgba1800[2] && rgba10000[2] > rgba10000[0],
		       "1800K is warm (R > B) and 10000K is cool (B > R)" );
		Check( !MaterialPreview::EvaluateBlackbodyKelvin( 0.0, &peak5500, rgba1800 ),
		       "EvaluateBlackbodyKelvin refuses <= 0 K" );

		// Direct D1 Sellmeier presets + metrics + malformed refusal
		const char* const kGlassPresets[] = { "N-BK7", "SF11", "Diamond", "FusedSilica" };
		for( const char* preset : kGlassPresets ) {
			std::string coeffs;
			double nd = 0.0, vd = 0.0;
			const bool ok = MaterialPreview::LookupSellmeierGlassPreset( String( preset ), coeffs, &nd, &vd );
			Check( ok && !coeffs.empty() && RISE::IsFiniteDouble( nd ) && nd > 1.4 && RISE::IsFiniteDouble( vd ) && vd > 10.0,
			       preset );
		}
		double badNd = 0.0, badVd = 0.0;
		{
			std::string badCoeffs;
			Check( !MaterialPreview::LookupSellmeierGlassPreset( String( "NotAGlass" ), badCoeffs, &badNd, &badVd ),
			       "unknown glass preset refuses" );
		}
		Check( !MaterialPreview::EvaluateSellmeierMetrics( String( "1.0 2.0" ), &badNd, &badVd ),
		       "EvaluateSellmeierMetrics refuses incomplete tuple" );

		// Direct D2 Measured metal presets + unknown refusal
		const char* const kMetals[] = { "Au", "Ag", "Cu", "Al", "Cr", "Ti" };
		for( const char* metal : kMetals ) {
			std::string fn, fk, pn, pk;
			unsigned char sw[4] = {0};
			const bool ok = MaterialPreview::LookupMeasuredMetalPreset( String( metal ), fn, fk, pn, pk, sw );
			Check( ok && !fn.empty() && !fk.empty() && !pn.empty() && !pk.empty() && sw[3] == 255,
			       metal );
		}
		{
			std::string fn, fk, pn, pk;
			unsigned char sw[4] = {0};
			Check( !MaterialPreview::LookupMeasuredMetalPreset( String( "Unobtainium" ), fn, fk, pn, pk, sw ),
			       "LookupMeasuredMetalPreset refuses unknown metal id" );
		}

		// Direct D3 Thin-film strip + single-angle evaluation
		double dNm = 0.0, fIor = 0.0;
		const MaterialPreview::Result tfStrip = MaterialPreview::RenderThinFilmAngleStrip( *j, String( "mat_thinfilm_380" ), 48, 6, &dNm, &fIor );
		Check( tfStrip.status == MaterialPreview::Status::Ok && std::fabs( dNm - 380.0 ) < 1e-4 && std::fabs( fIor - 1.38 ) < 1e-4,
		       "RenderThinFilmAngleStrip succeeds and reports live thickness=380nm, film_ior=1.38" );
		unsigned char tf0[4] = {0}, tf75[4] = {0};
		Check( MaterialPreview::EvaluateThinFilmAtAngle( *j, String( "mat_thinfilm_380" ), 0.0, tf0 ) &&
		       MaterialPreview::EvaluateThinFilmAtAngle( *j, String( "mat_thinfilm_380" ), 75.0, tf75 ),
		       "EvaluateThinFilmAtAngle succeeds at 0 deg and 75 deg" );
		Check( tf0[0] != tf75[0] || tf0[1] != tf75[1] || tf0[2] != tf75[2],
		       "thin-film reflectance shifts colour from normal to grazing incidence" );

		// SceneEditController C-ABI preview + preset round-trip
		SceneEditController* ctrl = nullptr;
		Check( RISE_API_CreateSceneEditController( j, nullptr, &ctrl ) && ctrl != nullptr,
		       "RISE_API_CreateSceneEditController succeeds" );
		if( ctrl ) {
			std::vector<unsigned char> cabiBall( 28 * 28 * 4, 0 );
			Check( RISE_API_SceneEditController_MaterialPreview( ctrl, "mat_conductor", 28, 28, cabiBall.data() ),
			       "RISE_API_SceneEditController_MaterialPreview renders mat_conductor" );
			Check( !RISE_API_SceneEditController_MaterialPreview( ctrl, "none", 28, 28, cabiBall.data() ),
			       "RISE_API_SceneEditController_MaterialPreview refuses 'none'" );

			ctrl->SetSelection( SceneEditController::Category::Material, String( "mat_conductor" ) );
			Check( RISE_API_SceneEditController_ApplySpectralPreset(
				ctrl, static_cast<int>( SceneEditController::Category::Material ), "Au" ),
				"ApplySpectralPreset('Au') succeeds on mat_conductor" );
			Check( ctrl->Undo() && ctrl->Redo(), "Undo and Redo of ApplySpectralPreset('Au') succeed" );

			RISE_API_DestroySceneEditController( ctrl );
		}

		j->release();
		std::remove( path.c_str() );
	}

	// ------------------------------------------------------------------
	// 3. Shared headless InteractivePelRasterizer material-preview pipeline
	//    (CreateInteractiveMaterialPreviewPipeline: 1-SPP live pass + 4-SPP polish)
	// ------------------------------------------------------------------
	void TestInteractiveMaterialPreviewPipeline()
	{
		std::cout << "3. CreateInteractiveMaterialPreviewPipeline headless 1-SPP live pass + 4-SPP polish pass..." << std::endl;
		const std::string path = TempPath( "interactive_preview.RISEscene" );
		Job* j = LoadSceneFromText( kInteractivePreviewScene, path );
		Check( j != nullptr, "interactive preview scene loads via CST" );
		if( !j ) return;

		const IScenePriv* scene = j->GetScene();
		Check( scene != nullptr, "Job exposes IScenePriv" );
		if( !scene ) {
			j->release();
			std::remove( path.c_str() );
			return;
		}

		IRasterizer* pRast = nullptr;
		IRayCaster*  pPreviewCaster = nullptr;
		IRayCaster*  pPolishCaster  = nullptr;
		Check( CreateInteractiveMaterialPreviewPipeline( &pRast, &pPreviewCaster, &pPolishCaster ) &&
		       pRast != nullptr && pPreviewCaster != nullptr && pPolishCaster != nullptr,
		       "CreateInteractiveMaterialPreviewPipeline constructs rasterizer + preview & polish casters" );

		InteractivePelRasterizer* pInteractive = dynamic_cast<InteractivePelRasterizer*>( pRast );
		Check( pInteractive != nullptr, "returned rasterizer is an InteractivePelRasterizer" );

		if( pRast && pInteractive ) {
			pInteractive->SetPreviewDenoiseMode( InteractivePelRasterizer::PreviewDenoise_Off );
			Agent::InMemoryRasterizerOutput* sink = new Agent::InMemoryRasterizerOutput();
			pRast->AddRasterizerOutput( sink );

			// Pass A: 1-SPP live drag preview pass
			pInteractive->SetIdleMode( false );
			pInteractive->SetSampleCount( 1 );
			std::srand( 4242u );
			pRast->RasterizeScene( *scene, 0, nullptr );

			Check( sink->HasImage() && sink->Width() == 48 && sink->Height() == 36,
			       "1-SPP live preview pass produces 48x36 frame" );

			bool liveAllFinite = true;
			double liveMeanR = 0.0, liveMeanG = 0.0, liveMeanB = 0.0;
			sink->MeanChannels( liveMeanR, liveMeanG, liveMeanB );
			for( unsigned int y = 0; y < sink->Height() && liveAllFinite; ++y ) {
				for( unsigned int x = 0; x < sink->Width(); ++x ) {
					RISEColor px;
					if( !sink->GetPixelColor( x, y, px ) ||
					    !RISE::IsFiniteDouble( px.base[0] ) ||
					    !RISE::IsFiniteDouble( px.base[1] ) ||
					    !RISE::IsFiniteDouble( px.base[2] ) ||
					    !RISE::IsFiniteDouble( px.a ) ||
					    px.base[0] < 0.0 || px.base[1] < 0.0 || px.base[2] < 0.0 ) {
						liveAllFinite = false;
						break;
					}
				}
			}
			Check( liveAllFinite, "1-SPP live preview frame contains only finite non-negative linear pixels" );
			Check( liveMeanR > 0.01 && liveMeanG > 0.01 && liveMeanB > 0.01,
			       "1-SPP live preview frame has non-zero studio shading across scene" );

			// Left clay sphere (warm R > B) vs right gold conductor sphere
			RISEColor leftPx, rightPx;
			Check( sink->GetPixelColor( 14, 18, leftPx ) && sink->GetPixelColor( 33, 18, rightPx ),
			       "sampled left clay sphere and right metal sphere pixels" );
			Check( leftPx.base[0] > leftPx.base[2] && rightPx.base[0] > rightPx.base[2],
			       "both warm clay and gold conductor spheres shade with R > B" );

			// Pass B: 4-SPP pointer-up polish pass (swaps to pPolishCaster)
			pInteractive->SetSampleCount( 4 );
			std::srand( 4243u );
			pRast->RasterizeScene( *scene, 0, nullptr );

			bool polishAllFinite = true;
			double polishMeanR = 0.0, polishMeanG = 0.0, polishMeanB = 0.0;
			sink->MeanChannels( polishMeanR, polishMeanG, polishMeanB );
			for( unsigned int y = 0; y < sink->Height() && polishAllFinite; ++y ) {
				for( unsigned int x = 0; x < sink->Width(); ++x ) {
					RISEColor px;
					if( !sink->GetPixelColor( x, y, px ) ||
					    !RISE::IsFiniteDouble( px.base[0] ) ||
					    !RISE::IsFiniteDouble( px.base[1] ) ||
					    !RISE::IsFiniteDouble( px.base[2] ) ||
					    !RISE::IsFiniteDouble( px.a ) ||
					    px.base[0] < 0.0 || px.base[1] < 0.0 || px.base[2] < 0.0 ) {
						polishAllFinite = false;
						break;
					}
				}
			}
			Check( polishAllFinite, "4-SPP polish pass frame contains only finite non-negative linear pixels" );
			Check( polishMeanR > 0.01 && polishMeanG > 0.01 && polishMeanB > 0.01,
			       "4-SPP polish pass frame produces valid studio-lit image" );

			// Reset back to 1-SPP live caster cleanly
			pInteractive->SetSampleCount( 1 );
			safe_release( sink );
		}

		safe_release( pPreviewCaster );
		safe_release( pPolishCaster );
		safe_release( pRast );
		j->release();
		std::remove( path.c_str() );
	}
}

int main()
{
	std::cout << "=== MaterialPreviewTest ===" << std::endl;
	TestShaderballFamiliesAndRefusals();
	TestSpectralHelpersAndControllerCAbi();
	TestInteractiveMaterialPreviewPipeline();

	std::cout << "\n" << g_pass << " passed, " << g_fail << " failed" << std::endl;
	return g_fail == 0 ? 0 : 1;
}
