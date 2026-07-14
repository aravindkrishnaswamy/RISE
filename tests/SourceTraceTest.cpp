//////////////////////////////////////////////////////////////////////
//
//  SourceTraceTest.cpp - Source traceability: tie any UI element back to
//    its span in the scene file, and the reverse (text -> UI element).
//    Generalizes EntitySourceLocation to param granularity + the
//    Film/Rasterizer/Environment singletons, keyed by the SAME
//    (Category, name, param) address the CST edit path uses.
//
//    S1  Param-granular forward: ResolveSourceSpan(cat,name,param) returns
//        a byte range whose substring of SerializeCst(doc) is EXACTLY the
//        param's `role value…` text — for a named painter's `color`, a
//        material whole-chunk, the Film singleton's `width`, the active
//        Rasterizer's `radiance_scale` (the Environment binding), and a
//        bound painter's `file` (the Environment HDRI, reached by the
//        widget naming the painter).
//    S2  Honest refusal: an unresolvable entity name or a missing param
//        returns false with present=false.
//    S3  Reverse: SourceRefAtByteOffset(offset) recovers (cat,name,param)
//        for an offset landing on a param and on a chunk header; and a
//        forward->reverse round-trip agrees.  Inter-chunk trivia -> false.
//
//  Self-contained: inline native-v7 scene, no render, nullptr rasterizer.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/SceneEditor/SceneEditController.h"

using namespace RISE;
using namespace RISE::Implementation;
using Category = SceneEditController::Category;
using SourceSpan = SceneEditController::SourceSpan;

namespace
{
	int g_pass = 0, g_fail = 0;
	void Check( bool c, const std::string& what )
	{
		if( c ) { ++g_pass; std::printf( "  ok  : %s\n", what.c_str() ); }
		else    { ++g_fail; std::printf( "  FAIL: %s\n", what.c_str() ); }
	}

	std::string TempPath( const char* n )
	{
		const char* b = std::getenv( "TMPDIR" );
		std::string d = b ? b : "/tmp";
		if( !d.empty() && d.back() != '/' ) d += '/';
		return d + n;
	}

	Job* LoadScene( const std::string& text, const std::string& path )
	{
		{ std::ofstream o( path.c_str(), std::ios::binary ); o << text; }
		Job* pJob = new Job();
		if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) { pJob->release(); std::remove( path.c_str() ); return nullptr; }
		return pJob;
	}

	std::string Serialized( Job& job )
	{
		const RISE::Cst::Document* d = job.GetCstDocument();
		return d ? RISE::Cst::SerializeCst( *d ) : std::string();
	}

	// The scene file text — note exact param strings we assert on below.
	const char* const kScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_col\ncolor 0.5 0.5 0.5\n}\n\n"
		"hdr_painter\n{\nname pnt_env\nfile /tmp/x.hdr\n}\n\n"
		"lambertian_material\n{\nname mat\nreflectance pnt_col\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 8\npixel_filter box\noidn_denoise false\n"
		"radiance_map pnt_env\nradiance_scale 2\n}\n\n"
		"film\n{\nwidth 32\nheight 24\n}\n\n"
		"pinhole_camera\n{\nname cam\nlocation 0 0 5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n";

	// Assert the (offset,length) span extracts EXACTLY `expected` from the doc text.
	void CheckSpanText( SceneEditController& ctrl, Job& job, Category cat, const char* name,
	                    const char* param, const char* expected )
	{
		SourceSpan sp;
		const bool ok = ctrl.ResolveSourceSpan( cat, String( name ), String( param ), 0, sp );
		Check( ok && sp.present, std::string( "resolve " ) + name + "." + param );
		if( !ok ) return;
		const std::string full = Serialized( job );
		Check( sp.byteOffset + sp.byteLength <= full.size(), std::string( "span in bounds for " ) + param );
		const std::string got = full.substr( sp.byteOffset, sp.byteLength );
		Check( got == expected,
			std::string( "span text for " ) + name + "." + param + " == `" + expected + "` (got `" + got + "`)" );
		// Line is 1-based and consistent with the offset's newline count.
		unsigned int nl = 1; for( std::uint64_t i = 0; i < sp.byteOffset; ++i ) if( full[i] == '\n' ) ++nl;
		Check( sp.line == nl, std::string( "line matches newline count for " ) + param );
	}

	void TestForwardParamSpans()
	{
		std::printf( "S1: param-granular forward spans (exact text)...\n" );
		const std::string tmp = TempPath( "srctrace_s1.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) return;
		SceneEditController ctrl( *pJob, nullptr );

		// Named painter param.
		CheckSpanText( ctrl, *pJob, Category::Painter, "pnt_col", "color", "color 0.5 0.5 0.5" );
		// Film singleton param (no name).
		CheckSpanText( ctrl, *pJob, Category::Film, "", "width", "width 32" );
		// Active Rasterizer = the Environment binding params.
		CheckSpanText( ctrl, *pJob, Category::Rasterizer, "", "radiance_scale", "radiance_scale 2" );
		CheckSpanText( ctrl, *pJob, Category::Rasterizer, "", "radiance_map", "radiance_map pnt_env" );
		// Environment HDRI file lives on the bound painter (the widget names it).
		CheckSpanText( ctrl, *pJob, Category::Painter, "pnt_env", "file", "file /tmp/x.hdr" );

		// Whole-chunk (empty param): offset lands at the chunk keyword, length 0.
		{
			SourceSpan sp;
			Check( ctrl.ResolveSourceSpan( Category::Material, String( "mat" ), String(), 0, sp ) && sp.present,
				"resolve whole material chunk" );
			const std::string full = Serialized( *pJob );
			Check( full.compare( sp.byteOffset, 19, "lambertian_material" ) == 0,
				"whole-chunk offset lands at `lambertian_material`" );
			Check( sp.byteLength == 0, "whole-chunk span has length 0" );
		}

		pJob->release();
		std::remove( tmp.c_str() );
	}

	void TestHonestRefusal()
	{
		std::printf( "S2: honest refusal (bad entity / bad param)...\n" );
		const std::string tmp = TempPath( "srctrace_s2.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) return;
		SceneEditController ctrl( *pJob, nullptr );

		SourceSpan sp;
		Check( !ctrl.ResolveSourceSpan( Category::Material, String( "nonexistent" ), String(), 0, sp ),
			"unresolvable entity name -> false" );
		Check( !sp.present, "present stays false on refusal" );
		Check( !ctrl.ResolveSourceSpan( Category::Painter, String( "pnt_col" ), String( "bogus" ), 0, sp ),
			"missing param -> false" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	void TestReverse()
	{
		std::printf( "S3: reverse (text offset -> UI ref) + round-trip...\n" );
		const std::string tmp = TempPath( "srctrace_s3.RISEscene" );
		Job* pJob = LoadScene( kScene, tmp );
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) return;
		SceneEditController ctrl( *pJob, nullptr );

		// Forward -> reverse round-trip on the painter color param.
		SourceSpan sp;
		Check( ctrl.ResolveSourceSpan( Category::Painter, String( "pnt_col" ), String( "color" ), 0, sp ),
			"forward color span" );
		Category cat; String name, param;
		Check( ctrl.SourceRefAtByteOffset( sp.byteOffset, cat, name, param ), "reverse at color offset" );
		Check( cat == Category::Painter, "reverse recovers Painter" );
		Check( std::string( name.c_str() ) == "pnt_col", "reverse recovers name pnt_col" );
		Check( std::string( param.c_str() ) == "color", "reverse recovers param color" );

		// Reverse on the Rasterizer's radiance_scale (Environment binding) round-trips.
		Check( ctrl.ResolveSourceSpan( Category::Rasterizer, String(), String( "radiance_scale" ), 0, sp ),
			"forward radiance_scale span" );
		Check( ctrl.SourceRefAtByteOffset( sp.byteOffset, cat, name, param )
			&& cat == Category::Rasterizer && std::string( param.c_str() ) == "radiance_scale",
			"reverse recovers Rasterizer.radiance_scale" );

		// Offset inside the material chunk header (before any param) -> Material, no param.
		{
			SourceSpan mchunk;
			Check( ctrl.ResolveSourceSpan( Category::Material, String( "mat" ), String(), 0, mchunk ),
				"forward material chunk" );
			Category c2; String n2, p2;
			// +2 bytes past the chunk start = still inside the keyword `lambertian_material`.
			Check( ctrl.SourceRefAtByteOffset( mchunk.byteOffset + 2, c2, n2, p2 ), "reverse at chunk header" );
			Check( c2 == Category::Material && std::string( n2.c_str() ) == "mat",
				"reverse recovers Material `mat` from a header offset" );
		}

		// An offset in inter-chunk trivia (the blank line between chunks) is not an
		// addressable element -> false.  Byte 0 is the "RISE ASCII SCENE 7" header
		// line, which is a non-entity leaf.
		{
			Category c3; String n3, p3;
			const bool r = ctrl.SourceRefAtByteOffset( 0, c3, n3, p3 );
			Check( !r || c3 == Category::None, "offset 0 (header) is not an addressable entity" );
		}

		pJob->release();
		std::remove( tmp.c_str() );
	}
	// Scene exercising the review-fix cases: a non-*_painter-suffix Painter entity
	// (expression_function2d) with a REPEATED `def` param, and TWO rasterizer chunks.
	const char* const kFixScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"expression_function2d\n{\nname fn\ndef a 1\ndef b 2\nexpr a+b\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 8\npixel_filter box\noidn_denoise false\n}\n\n"
		"bdpt_pel_rasterizer\n{\nsamples 4\nmax_eye_depth 8\nmax_light_depth 8\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 16\nheight 12\n}\n\n"
		"pinhole_camera\n{\nname cam\nlocation 0 0 5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n";

	void TestReviewFixes()
	{
		std::printf( "S4: review fixes — registry-classifier P1, multi-rasterizer P2a, occ P2b...\n" );
		const std::string tmp = TempPath( "srctrace_s4.RISEscene" );
		Job* pJob = LoadScene( kFixScene, tmp );
		Check( pJob != nullptr, "fixture loads (two rasterizers + function2d)" );
		if( !pJob ) return;
		SceneEditController ctrl( *pJob, nullptr );
		const std::string full = Serialized( *pJob );

		// P1: expression_function2d (does NOT end in _painter) is a registry Painter
		// entity — forward resolves it by name, and reverse now recovers Painter.
		SourceSpan fnExpr;
		Check( ctrl.ResolveSourceSpan( Category::Painter, String( "fn" ), String( "expr" ), 0, fnExpr ),
			"forward fn.expr (function2d addressable as Painter by name)" );
		Category c; String n, p; int occ = -1;
		Check( ctrl.SourceRefAtByteOffset( fnExpr.byteOffset, c, n, p, &occ ), "reverse at fn.expr" );
		Check( c == Category::Painter && std::string( n.c_str() ) == "fn",
			"P1: expression_function2d reverses to Painter `fn` (registry classifier, not a suffix)" );
		Check( std::string( p.c_str() ) == "expr", "reverse recovers param expr" );

		// P2b: the repeated `def` param — occ addresses the right one, and reverse
		// into the 2nd occurrence recovers occ=1.
		SourceSpan def0, def1;
		Check( ctrl.ResolveSourceSpan( Category::Painter, String( "fn" ), String( "def" ), 0, def0 )
			&& full.substr( def0.byteOffset, def0.byteLength ) == "def a 1", "forward def occ=0 == `def a 1`" );
		Check( ctrl.ResolveSourceSpan( Category::Painter, String( "fn" ), String( "def" ), 1, def1 )
			&& full.substr( def1.byteOffset, def1.byteLength ) == "def b 2", "forward def occ=1 == `def b 2`" );
		Check( ctrl.SourceRefAtByteOffset( def1.byteOffset, c, n, p, &occ )
			&& std::string( p.c_str() ) == "def" && occ == 1,
			"P2b: reverse into the 2nd def recovers occ=1" );
		Check( ctrl.SourceRefAtByteOffset( def0.byteOffset, c, n, p, &occ ) && occ == 0,
			"reverse into the 1st def recovers occ=0" );

		// P2a: multi-rasterizer round-trip.  Reverse into a rasterizer chunk yields
		// (Rasterizer, KIND); forward that kind resolves the SAME chunk (regardless
		// of which rasterizer is active).
		const size_t bdptPos = full.find( "bdpt_pel_rasterizer" );
		Check( bdptPos != std::string::npos, "scene text has a bdpt_pel_rasterizer chunk" );
		Category rc; String rn, rp;
		Check( ctrl.SourceRefAtByteOffset( bdptPos + 2, rc, rn, rp ), "reverse into the bdpt rasterizer chunk" );
		Check( rc == Category::Rasterizer && std::string( rn.c_str() ) == "bdpt_pel_rasterizer",
			"P2a: rasterizer reverse yields the KIND as name" );
		SourceSpan rfwd;
		Check( ctrl.ResolveSourceSpan( Category::Rasterizer, rn, String(), 0, rfwd ),
			"forward the reversed rasterizer kind" );
		Check( rfwd.byteOffset == bdptPos,
			"P2a: rasterizer round-trip resolves the SAME chunk (not the active one)" );

		pJob->release();
		std::remove( tmp.c_str() );
	}
	// Non-entity chunks that carry an entity ChunkCategory (parse grouping) but are
	// NOT UI-addressable: scene_options (Camera), piecewise_linear_function (Function).
	const char* const kNonEntityScene =
		"RISE ASCII SCENE 7\n"
		"scene_options\n{\nscene_unit 1.0\n}\n\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"piecewise_linear_function\n{\nname plf\ncp 400 1.0\ncp 500 2.0\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 8\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 16\nheight 12\n}\n\n"
		"pinhole_camera\n{\nname cam\nlocation 0 0 5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n";

	void TestNonEntityExclusions()
	{
		std::printf( "S5: non-entity chunks with an entity ChunkCategory reverse to false...\n" );
		const std::string tmp = TempPath( "srctrace_s5.RISEscene" );
		Job* pJob = LoadScene( kNonEntityScene, tmp );
		Check( pJob != nullptr, "fixture loads" );
		if( !pJob ) return;
		SceneEditController ctrl( *pJob, nullptr );
		const std::string full = Serialized( *pJob );

		Category c; String n, p;

		// scene_options is ChunkCategory::Camera + unnamed -> would mis-classify as
		// Camera and (via the camera unique-fallback) resolve the actual camera chunk.
		// The self-consistency check rejects it (forward doesn't round-trip to itself).
		const size_t soPos = full.find( "scene_options" );
		Check( soPos != std::string::npos, "scene has a scene_options chunk" );
		Check( !ctrl.SourceRefAtByteOffset( soPos + 2, c, n, p ),
			"reverse into scene_options -> false (not the camera)" );

		// piecewise_linear_function is ChunkCategory::Function -> Category::None
		// (registers into the Function manager, not the Painter union).
		const size_t plfPos = full.find( "piecewise_linear_function" );
		Check( plfPos != std::string::npos, "scene has a piecewise_linear_function chunk" );
		Check( !ctrl.SourceRefAtByteOffset( plfPos + 2, c, n, p ),
			"reverse into piecewise_linear_function -> false (Function, not a UI Painter)" );

		// Control: the REAL camera still round-trips (the self-consistency check does
		// not over-reject legitimate entities).
		const size_t camPos = full.find( "pinhole_camera" );
		Check( camPos != std::string::npos, "scene has a pinhole_camera chunk" );
		Check( ctrl.SourceRefAtByteOffset( camPos + 2, c, n, p )
			&& c == Category::Camera && std::string( n.c_str() ) == "cam",
			"reverse into the real camera -> (Camera, cam)" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	// The two final-verification cases: light_rr_threshold (ChunkCategory::Rasterizer
	// config, defeats a self-consistency guard) + a real scene_variant colliding by
	// name with an active_scene_variant selector.
	const char* const kConfigCollideScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"light_rr_threshold\n{\nthreshold 0.5\n}\n\n"
		"scene_variant\n{\nname night\n}\n\n"
		"active_scene_variant\n{\nname night\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 8\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 16\nheight 12\n}\n\n"
		"pinhole_camera\n{\nname cam\nlocation 0 0 5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n";

	void TestConfigAndVariantCollision()
	{
		std::printf( "S6: light_rr_threshold reject + scene_variant not over-rejected by the collision...\n" );
		const std::string tmp = TempPath( "srctrace_s6.RISEscene" );
		Job* pJob = LoadScene( kConfigCollideScene, tmp );
		Check( pJob != nullptr, "fixture loads (light_rr_threshold + variant collision)" );
		if( !pJob ) return;
		SceneEditController ctrl( *pJob, nullptr );
		const std::string full = Serialized( *pJob );
		Category c; String n, p;

		// P2-a: light_rr_threshold is a ChunkCategory::Rasterizer scene setting, NOT a
		// rasterizer -> its role doesn't end in _rasterizer -> not addressable.
		const size_t lrtPos = full.find( "light_rr_threshold" );
		Check( lrtPos != std::string::npos, "scene has a light_rr_threshold chunk" );
		Check( !ctrl.SourceRefAtByteOffset( lrtPos + 2, c, n, p ),
			"reverse into light_rr_threshold -> false (config, not a rasterizer entity)" );

		// P2-b: the REAL scene_variant `night` must NOT be over-rejected even though an
		// active_scene_variant `night` selector collides by name.  (The enum check
		// doesn't call the ambiguous forward resolver, so no over-rejection.)
		const size_t svPos = full.find( "scene_variant\n" );   // the declaration (not active_scene_variant)
		Check( svPos != std::string::npos, "scene has a scene_variant chunk" );
		Check( ctrl.SourceRefAtByteOffset( svPos + 2, c, n, p )
			&& c == Category::SceneVariant && std::string( n.c_str() ) == "night",
			"reverse into the real scene_variant -> (SceneVariant, night), not over-rejected" );

		pJob->release();
		std::remove( tmp.c_str() );
	}

	// override_object: a legacy MODIFIER whose `name` references an existing object.
	const char* const kOverrideScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"uniformcolor_painter\n{\nname pnt\ncolor 0.5 0.5 0.5\n}\n\n"
		"lambertian_material\n{\nname mat\nreflectance pnt\n}\n\n"
		"sphere_geometry\n{\nname sph\nradius 1\n}\n\n"
		"standard_object\n{\nname ball\ngeometry sph\nmaterial mat\n}\n\n"
		"override_object\n{\nname ball\nposition 1 0 0\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 8\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 16\nheight 12\n}\n\n"
		"pinhole_camera\n{\nname cam\nlocation 0 0 5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n";

	void TestOverrideObjectExclusion()
	{
		std::printf( "S7: override_object (references an object, isn't one) reverses to false...\n" );
		const std::string tmp = TempPath( "srctrace_s7.RISEscene" );
		Job* pJob = LoadScene( kOverrideScene, tmp );
		Check( pJob != nullptr, "fixture loads (standard_object + override_object)" );
		if( !pJob ) return;
		SceneEditController ctrl( *pJob, nullptr );
		const std::string full = Serialized( *pJob );
		Category c; String n, p;

		const size_t ovPos = full.find( "override_object" );
		Check( ovPos != std::string::npos, "scene has an override_object chunk" );
		Check( !ctrl.SourceRefAtByteOffset( ovPos + 2, c, n, p ),
			"reverse into override_object -> false (references `ball`, isn't the entity)" );

		// Control: the real standard_object `ball` still reverses to (Object, ball).
		const size_t soPos = full.find( "standard_object" );
		Check( soPos != std::string::npos, "scene has a standard_object chunk" );
		Check( ctrl.SourceRefAtByteOffset( soPos + 2, c, n, p )
			&& c == Category::Object && std::string( n.c_str() ) == "ball",
			"reverse into standard_object -> (Object, ball)" );

		pJob->release();
		std::remove( tmp.c_str() );
	}
}   // anonymous namespace

int main()
{
	std::printf( "=== SourceTraceTest ===\n" );
	TestForwardParamSpans();
	TestHonestRefusal();
	TestReverse();
	TestReviewFixes();
	TestNonEntityExclusions();
	TestConfigAndVariantCollision();
	TestOverrideObjectExclusion();

	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
