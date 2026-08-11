//////////////////////////////////////////////////////////////////////
//
//  CstLoadViaCstTest.cpp - P5 (save-as-CST) Slice 1.
//
//  Job::LoadAsciiSceneViaCst loads a scene by building the canonical CST
//  (ParseToCst), deriving the Scene from it (DeriveToJob), and RETAINING the
//  Document for later edit/save (Model-B: "Scene = derive(CST)").  This test
//  proves the CST load path derives a non-trivial Scene and retains the
//  Document, and pins the loader's ACCEPT / REFUSE contract: native v7 is
//  canonical, plain transitional v6 remains accepted, render-neutral `>`
//  directives are accepted, and retired legacy constructs, render-affecting
//  directives, unavailable authored encoders, and `> run` are refused.
//
//  Slice 6c-3b: the original legacy-vs-CST DumpJob equivalence arm was retired
//  with the rest of the legacy-parser oracle -- CstDeriveGoldenTest is the
//  standing CST-derive correctness net over the whole corpus.
//
//  Suite-safe: the accepted scenes are synthetic flat native forms (canonical
//  v7 plus the explicitly supported plain-v6 transition case) with no external
//  media, so the CST path loads them directly.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "CstRenderEquivalence.h"   // Job, DumpJob

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

using namespace RISE;
using namespace RISE::Implementation;
using namespace risequiv;

namespace
{
	int s_pass = 0, s_fail = 0;
	void Check( bool ok, const std::string& what ) { if( ok ) ++s_pass; else { ++s_fail; std::printf( "  FAIL: %s\n", what.c_str() ); } }

	// DumpJob of a fresh, empty Job -- the "derived nothing" sentinel.  A successful
	// accepted native load must produce a dump that differs from this.
	const std::string& EmptyJobDump()
	{
		static const std::string e = []{ Job* j = new Job(); std::string s = DumpJob( *j ); j->release(); return s; }();
		return e;
	}

	std::string TempPath( const char* name )
	{
		return ( std::filesystem::temp_directory_path() /
			( "cst_load_via_" + std::to_string( static_cast<long>( getpid() ) ) +
			  "_" + name ) ).string();
	}

	bool WriteTmp( const std::string& path, const std::string& text )
	{
		std::ofstream f( path );
		if( !f ) return false;
		f << text;
		return f.good();
	}

	// Load `scene` (written to `path`) via the CST path; assert it derives a non-trivial Scene
	// (DumpJob != the empty-Job dump) and RETAINS the canonical Document.  (Slice 6c-3b: the legacy-
	// parser arm that this originally compared against was retired -- CstDeriveGoldenTest is now the
	// CST-derive correctness net for the whole corpus, so the per-inline-scene legacy oracle here is
	// subsumed.  What stays UNIQUE to this test is the CST loader's ACCEPT/REFUSE contract below.)
	void Case( const char* label, const std::string& path, const std::string& scene )
	{
		if( !WriteTmp( path, scene ) ) { Check( false, std::string( label ) + ": write temp scene" ); return; }

		Job* jC = new Job();
		const bool okC = jC->LoadAsciiSceneViaCst( path.c_str() );

		Check( okC, std::string( label ) + ": LoadAsciiSceneViaCst succeeds" );
		if( okC )
			Check( DumpJob( *jC ) != EmptyJobDump(), std::string( label ) + ": CST load derives a non-trivial Scene" );
		Check( jC->GetCstDocument() != nullptr, std::string( label ) + ": CST load RETAINS the canonical Document" );

		jC->release();
		std::filesystem::remove( path );
	}

	// Assert LoadAsciiSceneViaCst REFUSES a scene expected to be rejected for
	// the reason named by its label (returns false, retains no Document).
	void RefuseCase( const char* label, const std::string& path, const std::string& scene )
	{
		if( !WriteTmp( path, scene ) ) { Check( false, std::string( label ) + ": write temp scene" ); return; }
		Job* j = new Job();
		const bool ok = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( !ok, std::string( label ) + ": LoadAsciiSceneViaCst REFUSES (returns false)" );
		Check( j->GetCstDocument() == nullptr, std::string( label ) + ": no Document retained on refusal" );
		j->release();
		std::filesystem::remove( path );
	}

	// Assert LoadAsciiSceneViaCst ACCEPTS a scene carrying a render-NEUTRAL `>` directive (`> echo` / `> set
	// accelerator` -- the migrator passes those through; DeriveToJob skips them render-neutrally).  (Render-
	// AFFECTING `> modify` / `> set <other>` are REFUSED -- see the RefuseCases below.)
	void AcceptCase( const char* label, const std::string& path, const std::string& scene )
	{
		if( !WriteTmp( path, scene ) ) { Check( false, std::string( label ) + ": write temp scene" ); return; }
		Job* j = new Job();
		const bool ok = j->LoadAsciiSceneViaCst( path.c_str() );
		Check( ok, std::string( label ) + ": LoadAsciiSceneViaCst ACCEPTS (render-neutral > directive not false-rejected)" );
		Check( j->GetCstDocument() != nullptr, std::string( label ) + ": Document retained" );
		j->release();
		std::filesystem::remove( path );
	}
}

int main()
{
	std::printf( "=== CstLoadViaCstTest (P5 Slice 1: load via the canonical CST) ===\n" );

	// Slice 6e header-bump back-compat pair: the reader accepts BOTH the post-cutover `7` header and the
	// transitional `6` header (the CST is version-agnostic).  Prove both load so a re-header can't break
	// un-migrated user scenes, and so a re-headered corpus scene keeps loading.
	Case( "back-compat: SCENE 7 header loads (post-cutover default)", TempPath("v7.RISEscene"),
		"RISE ASCII SCENE 7\nsphere_geometry\n{\nname sg7\nradius 1\n}\n" );
	Case( "back-compat: SCENE 6 header still loads (transitional)", TempPath("v6.RISEscene"),
		"RISE ASCII SCENE 6\nsphere_geometry\n{\nname sg6\nradius 1\n}\n" );

	Case( "painter+material+geom+object+lights", TempPath("1.RISEscene"),
		"RISE ASCII SCENE 7\n"
		"uniformcolor_painter\n{\nname p\ncolor 0.8 0.2 0.2\n}\n"
		"lambertian_material\n{\nname m\nreflectance p\n}\n"
		"sphere_geometry\n{\nname g\nradius 1.5\n}\n"
		"standard_object\n{\nname o\ngeometry g\nmaterial m\nposition 0 0 0\n}\n"
		"directional_light\n{\nname key\npower 3.14\ncolor 1.0 0.96 0.90\ndirection 0.4 0.7 0.5\n}\n"
		"ambient_light\n{\nname amb\npower 0.2\ncolor 1 1 1\n}\n" );

	Case( "shared-material refs + comments", TempPath("2.RISEscene"),
		"RISE ASCII SCENE 6\n# two spheres sharing one material\n"
		"uniformcolor_painter\n{\nname pp\ncolor 0.1 0.6 0.9\n}\n"
		"lambertian_material\n{\nname mm\nreflectance pp\n}\n"
		"sphere_geometry\n{\nname g1\nradius 1\n}\n"
		"sphere_geometry\n{\nname g2\nradius 2\n}\n"
		"standard_object\n{\nname o1\ngeometry g1\nmaterial mm\nposition -1 0 0\n}\n"
		"standard_object\n{\nname o2\ngeometry g2\nmaterial mm\nposition 1 0 0\n}\n" );

	// P1 fix: non-native-v7 input is REFUSED (not silently mis-derived -- e.g. a 3-iteration FOR -> 1 body).
	RefuseCase( "v6 FOR loop refused", TempPath("for.RISEscene"),
		"RISE ASCII SCENE 6\nFOR i 0 1 2\nsphere_geometry\n{\nname s\nradius 1\n}\nENDFOR\n" );
	RefuseCase( "v6 `> run` directive refused", TempPath("run.RISEscene"),
		"RISE ASCII SCENE 6\n> run somewhere/palette.RISEscript\n" );
	RefuseCase( "missing version header refused", TempPath("nohdr.RISEscene"),
		"sphere_geometry\n{\nname s\nradius 1\n}\n" );

	// P1 (round-4 fix): a RENDER-AFFECTING `>` directive must be REFUSED -- CST-load silently drops every `>`
	// line, so a `> modify` / `> set <other>` scene would mis-render (DumpJob is blind).  The migrator must
	// convert these to v7 chunks (or, for `> modify`, the light-configurations feature) before they CST-load.
	RefuseCase( "render-affecting `> modify` refused", TempPath("modify.RISEscene"),
		"RISE ASCII SCENE 6\nsphere_geometry\n{\nname s\nradius 1\n}\n> modify object s material glow\n" );
	RefuseCase( "render-affecting `> set light_rr_threshold` refused", TempPath("rr.RISEscene"),
		"RISE ASCII SCENE 6\n> set light_rr_threshold 0.5\nsphere_geometry\n{\nname s\nradius 1\n}\n" );

	// A MIGRATED scene retains render-NEUTRAL `>` directives (`> echo`, `> set accelerator`); these MUST be
	// accepted, not false-rejected (else ~185 corpus scenes break).  (Round-4: render-AFFECTING `> modify` /
	// `> set <other>` are refused instead -- the RefuseCases above.)
	AcceptCase( "> set accelerator accepted", TempPath("set.RISEscene"),
		"RISE ASCII SCENE 6\n> set accelerator B 10 8\nsphere_geometry\n{\nname s\nradius 1\n}\n" );
	AcceptCase( "> echo accepted", TempPath("echo.RISEscene"),
		"RISE ASCII SCENE 6\n> echo loading the scene\nsphere_geometry\n{\nname s\nradius 1\n}\n" );

	const auto encoderScene = []( const char* type ) {
		const char* colorSpace = std::string(type) == "EXR" ?
			"Rec709RGB_Linear" : "sRGB";
		return std::string(
			"RISE ASCII SCENE 7\n"
			"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n"
			"pixelpel_rasterizer\n{\nsamples 1\n}\n"
			"file_rasterizeroutput\n{\npattern cst_encoder_gate\ntype " ) +
			type + "\nbpp 8\ncolor_space " + colorSpace + "\n}\n";
	};
#ifdef NO_PNG_SUPPORT
	RefuseCase( "authored PNG rejects when encoder is unavailable",
		TempPath("no_png_encoder.RISEscene"), encoderScene("PNG") );
#else
	AcceptCase( "authored PNG loads when encoder is available",
		TempPath("png_encoder.RISEscene"), encoderScene("PNG") );
#endif
#ifdef NO_TIFF_SUPPORT
	RefuseCase( "authored TIFF rejects when encoder is unavailable",
		TempPath("no_tiff_encoder.RISEscene"), encoderScene("TIFF") );
#else
	AcceptCase( "authored TIFF loads when encoder is available",
		TempPath("tiff_encoder.RISEscene"), encoderScene("TIFF") );
#endif
#ifdef NO_EXR_SUPPORT
	RefuseCase( "authored EXR rejects when encoder is unavailable",
		TempPath("no_exr_encoder.RISEscene"), encoderScene("EXR") );
#else
	AcceptCase( "authored EXR loads when encoder is available",
		TempPath("exr_encoder.RISEscene"), encoderScene("EXR") );
#endif

	// Slice 6c-3a: LoadAsciiSceneAuto is now CST-ONLY.  A plain transitional-v6 scene loads via the CST path and RETAINS
	// the canonical Document; a legacy-streaming (unmigrated) scene HARD-FAILS (returns false, NO Document) instead
	// of falling back to the legacy loader.  RED-PROVE: before this change Auto fell back to legacy on the
	// non-native branch, so the FOR-loop scene below would have legacy-loaded and returned TRUE -- the
	// `Check( !okAutoBad, ... )` assertion would flip to a FAIL.  (Restore the legacy fallback in
	// LoadAsciiSceneAuto and this assertion fails; that is the proof it exercises the new hard-fail.)
	{
		const std::string pOk  = TempPath("auto_native.RISEscene");
		const std::string pBad = TempPath("auto_nonnative.RISEscene");
		const bool wroteOk = WriteTmp( pOk,
			"RISE ASCII SCENE 6\nsphere_geometry\n{\nname sg\nradius 1\n}\n" );
		const bool wroteBad = WriteTmp( pBad,
			"RISE ASCII SCENE 6\nFOR i 0 1 2\nsphere_geometry\n{\nname s\nradius 1\n}\nENDFOR\n" );
		Check( wroteOk, "Auto CST-only: transitional-v6 temp scene was written" );
		Check( wroteBad, "Auto CST-only: legacy-streaming temp scene was written" );

		// Plain transitional v6 -> Auto succeeds + retains the CST Document.
		if( wroteOk ) {
			Job* j = new Job();
			const bool okAuto = j->LoadAsciiSceneAuto( pOk.c_str() );
			Check( okAuto, "Auto CST-only: transitional-v6 scene loads via Auto (returns true)" );
			Check( j->HasRetainedCstDocument(), "Auto CST-only: transitional-v6 Auto-load RETAINS the CST Document" );
			j->release();
			std::filesystem::remove( pOk );
		}

		// Unmigrated FOR/ENDFOR -> Auto HARD-FAILS, retains NO Document, does NOT legacy-fall-back.
		if( wroteBad ) {
			Job* j = new Job();
			const bool okAutoBad = j->LoadAsciiSceneAuto( pBad.c_str() );
			Check( !okAutoBad, "Auto CST-only: legacy-streaming scene HARD-FAILS (no legacy fallback)" );
			Check( !j->HasRetainedCstDocument(), "Auto CST-only: legacy-streaming failure retains NO Document" );
			j->release();
			std::filesystem::remove( pBad );
		}
	}

	// P2 fix: load-once -- re-loading into a live Job is refused (Document/Scene desync otherwise).
	// Use a DIFFERENT, otherwise-valid 2nd scene (distinct name) so ONLY the load-once guard can refuse it --
	// loading the same file twice would mask a neutered guard behind the duplicate-name hard error.
	{
		const std::string pa = TempPath("reload_a.RISEscene");
		const std::string pb = TempPath("reload_b.RISEscene");
		const bool wroteA = WriteTmp( pa,
			"RISE ASCII SCENE 6\nsphere_geometry\n{\nname sa\nradius 1\n}\n" );
		const bool wroteB = WriteTmp( pb,
			"RISE ASCII SCENE 6\nsphere_geometry\n{\nname sb\nradius 2\n}\n" );
		Check( wroteA, "reload: first temp scene was written" );
		Check( wroteB, "reload: second temp scene was written" );
		if( wroteA && wroteB ) {
			Job* j = new Job();
			const bool ok1 = j->LoadAsciiSceneViaCst( pa.c_str() );
			const bool ok2 = j->LoadAsciiSceneViaCst( pb.c_str() );   // distinct valid scene -> only load-once can refuse it
			Check( ok1, "reload: first load succeeds" );
			Check( !ok2, "reload: second load REFUSED by load-once (not masked by a dup-name error)" );
			j->release();
		}
		std::filesystem::remove( pa );
		std::filesystem::remove( pb );
	}

	std::printf( "%d passed, %d failed.\n", s_pass, s_fail );
	return s_fail == 0 ? 0 : 1;
}
