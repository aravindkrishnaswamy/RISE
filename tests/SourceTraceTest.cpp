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
}   // anonymous namespace

int main()
{
	std::printf( "=== SourceTraceTest ===\n" );
	TestForwardParamSpans();
	TestHonestRefusal();
	TestReverse();

	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
