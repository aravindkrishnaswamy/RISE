//////////////////////////////////////////////////////////////////////
//
//  CstLoadViaCstTest.cpp - P5 (save-as-CST) Slice 1.
//
//  Job::LoadAsciiSceneViaCst loads a scene by building the canonical CST
//  (ParseToCst), deriving the Scene from it (DeriveToJob), and RETAINING the
//  Document for later edit/save (Model-B: "Scene = derive(CST)").  This test
//  proves the CST load path derives a non-trivial Scene and retains the
//  Document, and pins the loader's ACCEPT / REFUSE contract (native-v7 accepted,
//  render-neutral `>` directives accepted, render-affecting directives + FOR /
//  `> run` refused).
//
//  Slice 6c-3b: the original legacy-vs-CST DumpJob equivalence arm was retired
//  with the rest of the legacy-parser oracle -- CstDeriveGoldenTest is the
//  standing CST-derive correctness net over the whole corpus.
//
//  Suite-safe: the scenes are synthetic NATIVE-v7 forms (flat -- no
//  $()/DEFINE/FOR/`> run`) with no external media, so the CST path (which does
//  NOT Migrate -- the v6 corpus is converted offline in plan Slice 2) loads
//  them directly.  Slice 2 extends the equivalence to the whole (converted)
//  corpus through the live load path.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "CstRenderEquivalence.h"   // Job, DumpJob

#include <cstdio>
#include <fstream>
#include <string>
#include "../src/Library/Interfaces/IShaderManager.h"
#include "../src/Library/Utilities/PiecewiseLinearFunction.h"
#include "TestTempDir.h"

using namespace RISE;
using namespace RISE::Implementation;
using namespace risequiv;

namespace
{
	int s_pass = 0, s_fail = 0;
	void Check( bool ok, const std::string& what ) { if( ok ) ++s_pass; else { ++s_fail; std::printf( "  FAIL: %s\n", what.c_str() ); } }

	// DumpJob of a fresh, empty Job -- the "derived nothing" sentinel.  A successful
	// native-v7 load must produce a dump that differs from this.
	const std::string& EmptyJobDump()
	{
		static const std::string e = []{ Job* j = new Job(); std::string s = DumpJob( *j ); j->release(); return s; }();
		return e;
	}

	bool WriteTmp( const char* path, const std::string& text )
	{
		std::ofstream f( path );
		if( !f ) return false;
		f << text;
		return f.good();
	}

	// Load `v7scene` (written to `path`) via the CST path; assert it derives a non-trivial Scene
	// (DumpJob != the empty-Job dump) and RETAINS the canonical Document.  (Slice 6c-3b: the legacy-
	// parser arm that this originally compared against was retired -- CstDeriveGoldenTest is now the
	// CST-derive correctness net for the whole corpus, so the per-inline-scene legacy oracle here is
	// subsumed.  What stays UNIQUE to this test is the CST loader's ACCEPT/REFUSE contract below.)
	void Case( const char* label, const char* path, const std::string& v7scene )
	{
		if( !WriteTmp( path, v7scene ) ) { Check( false, std::string( label ) + ": write temp scene" ); return; }

		Job* jC = new Job();
		const bool okC = jC->LoadAsciiSceneViaCst( path );

		Check( okC, std::string( label ) + ": LoadAsciiSceneViaCst succeeds" );
		if( okC )
			Check( DumpJob( *jC ) != EmptyJobDump(), std::string( label ) + ": CST load derives a non-trivial Scene" );
		Check( jC->GetCstDocument() != nullptr, std::string( label ) + ": CST load RETAINS the canonical Document" );

		jC->release();
		std::remove( path );
	}

	// Assert LoadAsciiSceneViaCst REFUSES a non-native-v7 scene (returns false, retains no Document).
	void RefuseCase( const char* label, const char* path, const std::string& scene )
	{
		if( !WriteTmp( path, scene ) ) { Check( false, std::string( label ) + ": write temp scene" ); return; }
		Job* j = new Job();
		const bool ok = j->LoadAsciiSceneViaCst( path );
		Check( !ok, std::string( label ) + ": LoadAsciiSceneViaCst REFUSES (returns false)" );
		Check( j->GetCstDocument() == nullptr, std::string( label ) + ": no Document retained on refusal" );
		j->release();
		std::remove( path );
	}

	// Assert LoadAsciiSceneViaCst ACCEPTS a scene carrying a render-NEUTRAL `>` directive (`> echo` / `> set
	// accelerator` -- the migrator passes those through; DeriveToJob skips them render-neutrally).  (Render-
	// AFFECTING `> modify` / `> set <other>` are REFUSED -- see the RefuseCases below.)
	void AcceptCase( const char* label, const char* path, const std::string& scene )
	{
		if( !WriteTmp( path, scene ) ) { Check( false, std::string( label ) + ": write temp scene" ); return; }
		Job* j = new Job();
		const bool ok = j->LoadAsciiSceneViaCst( path );
		Check( ok, std::string( label ) + ": LoadAsciiSceneViaCst ACCEPTS (render-neutral > directive not false-rejected)" );
		Check( j->GetCstDocument() != nullptr, std::string( label ) + ": Document retained" );
		j->release();
		std::remove( path );
	}
}

int main()
{
	std::printf( "=== CstLoadViaCstTest (P5 Slice 1: load via the canonical CST) ===\n" );

	// Slice 6e header-bump back-compat pair: the reader accepts BOTH the post-cutover `7` header and the
	// transitional `6` header (the CST is version-agnostic).  Prove both load so a re-header can't break
	// un-migrated user scenes, and so a re-headered corpus scene keeps loading.
	Case( "back-compat: SCENE 7 header loads (post-cutover default)", RISE::TestSupport::ScratchPath( "cst_loadvia_v7.RISEscene" ).c_str(),
		"RISE ASCII SCENE 7\nsphere_geometry\n{\nname sg7\nradius 1\n}\n" );
	Case( "back-compat: SCENE 6 header still loads (transitional)", RISE::TestSupport::ScratchPath( "cst_loadvia_v6.RISEscene" ).c_str(),
		"RISE ASCII SCENE 6\nsphere_geometry\n{\nname sg6\nradius 1\n}\n" );

	Case( "painter+material+geom+object+lights", RISE::TestSupport::ScratchPath( "cst_loadvia_1.RISEscene" ).c_str(),
		"RISE ASCII SCENE 7\n"
		"uniformcolor_painter\n{\nname p\ncolor 0.8 0.2 0.2\n}\n"
		"lambertian_material\n{\nname m\nreflectance p\n}\n"
		"sphere_geometry\n{\nname g\nradius 1.5\n}\n"
		"standard_object\n{\nname o\ngeometry g\nmaterial m\nposition 0 0 0\n}\n"
		"directional_light\n{\nname key\npower 3.14\ncolor 1.0 0.96 0.90\ndirection 0.4 0.7 0.5\n}\n"
		"ambient_light\n{\nname amb\npower 0.2\ncolor 1 1 1\n}\n" );

	Case( "shared-material refs + comments", RISE::TestSupport::ScratchPath( "cst_loadvia_2.RISEscene" ).c_str(),
		"RISE ASCII SCENE 6\n# two spheres sharing one material\n"
		"uniformcolor_painter\n{\nname pp\ncolor 0.1 0.6 0.9\n}\n"
		"lambertian_material\n{\nname mm\nreflectance pp\n}\n"
		"sphere_geometry\n{\nname g1\nradius 1\n}\n"
		"sphere_geometry\n{\nname g2\nradius 2\n}\n"
		"standard_object\n{\nname o1\ngeometry g1\nmaterial mm\nposition -1 0 0\n}\n"
		"standard_object\n{\nname o2\ngeometry g2\nmaterial mm\nposition 1 0 0\n}\n" );

	// P1 fix: non-native-v7 input is REFUSED (not silently mis-derived -- e.g. a 3-iteration FOR -> 1 body).
	RefuseCase( "v6 FOR loop refused", RISE::TestSupport::ScratchPath( "cst_loadvia_for.RISEscene" ).c_str(),
		"RISE ASCII SCENE 6\nFOR i 0 1 2\nsphere_geometry\n{\nname s\nradius 1\n}\nENDFOR\n" );
	RefuseCase( "v6 `> run` directive refused", RISE::TestSupport::ScratchPath( "cst_loadvia_run.RISEscene" ).c_str(),
		"RISE ASCII SCENE 6\n> run somewhere/palette.RISEscript\n" );
	RefuseCase( "missing version header refused", RISE::TestSupport::ScratchPath( "cst_loadvia_nohdr.RISEscene" ).c_str(),
		"sphere_geometry\n{\nname s\nradius 1\n}\n" );

	// P1 (round-4 fix): a RENDER-AFFECTING `>` directive must be REFUSED -- CST-load silently drops every `>`
	// line, so a `> modify` / `> set <other>` scene would mis-render (DumpJob is blind).  The migrator must
	// convert these to v7 chunks (or, for `> modify`, the light-configurations feature) before they CST-load.
	RefuseCase( "render-affecting `> modify` refused", RISE::TestSupport::ScratchPath( "cst_loadvia_modify.RISEscene" ).c_str(),
		"RISE ASCII SCENE 6\nsphere_geometry\n{\nname s\nradius 1\n}\n> modify object s material glow\n" );
	RefuseCase( "render-affecting `> set light_rr_threshold` refused", RISE::TestSupport::ScratchPath( "cst_loadvia_rr.RISEscene" ).c_str(),
		"RISE ASCII SCENE 6\n> set light_rr_threshold 0.5\nsphere_geometry\n{\nname s\nradius 1\n}\n" );

	// A MIGRATED scene retains render-NEUTRAL `>` directives (`> echo`, `> set accelerator`); these MUST be
	// accepted, not false-rejected (else ~185 corpus scenes break).  (Round-4: render-AFFECTING `> modify` /
	// `> set <other>` are refused instead -- the RefuseCases above.)
	AcceptCase( "> set accelerator accepted", RISE::TestSupport::ScratchPath( "cst_loadvia_set.RISEscene" ).c_str(),
		"RISE ASCII SCENE 6\n> set accelerator B 10 8\nsphere_geometry\n{\nname s\nradius 1\n}\n" );
	AcceptCase( "> echo accepted", RISE::TestSupport::ScratchPath( "cst_loadvia_echo.RISEscene" ).c_str(),
		"RISE ASCII SCENE 6\n> echo loading the scene\nsphere_geometry\n{\nname s\nradius 1\n}\n" );

	// Slice 6c-3a: LoadAsciiSceneAuto is now CST-ONLY.  A native-v7 scene loads via the CST path and RETAINS
	// the canonical Document; a non-native (un-migrated) scene HARD-FAILS (returns false, NO Document) instead
	// of falling back to the legacy loader.  RED-PROVE: before this change Auto fell back to legacy on the
	// non-native branch, so the FOR-loop scene below would have legacy-loaded and returned TRUE -- the
	// `Check( !okAutoBad, ... )` assertion would flip to a FAIL.  (Restore the legacy fallback in
	// LoadAsciiSceneAuto and this assertion fails; that is the proof it exercises the new hard-fail.)
	{
		const std::string pOkStr  = RISE::TestSupport::ScratchPath( "cst_auto_native.RISEscene" );
		const char* pOk  = pOkStr.c_str();
		const std::string pBadStr = RISE::TestSupport::ScratchPath( "cst_auto_nonnative.RISEscene" );
		const char* pBad = pBadStr.c_str();

		// Native-v7 -> Auto succeeds + retains the CST Document.
		if( WriteTmp( pOk, "RISE ASCII SCENE 6\nsphere_geometry\n{\nname sg\nradius 1\n}\n" ) ) {
			Job* j = new Job();
			const bool okAuto = j->LoadAsciiSceneAuto( pOk );
			Check( okAuto, "Auto CST-only: native-v7 scene loads via Auto (returns true)" );
			Check( j->HasRetainedCstDocument(), "Auto CST-only: native-v7 Auto-load RETAINS the CST Document" );
			j->release();
			std::remove( pOk );
		}

		// Non-native (un-migrated FOR/ENDFOR) -> Auto HARD-FAILS, retains NO Document, does NOT legacy-fall-back.
		if( WriteTmp( pBad, "RISE ASCII SCENE 6\nFOR i 0 1 2\nsphere_geometry\n{\nname s\nradius 1\n}\nENDFOR\n" ) ) {
			Job* j = new Job();
			const bool okAutoBad = j->LoadAsciiSceneAuto( pBad );
			Check( !okAutoBad, "Auto CST-only: non-native scene HARD-FAILS via Auto (returns false, no legacy fallback)" );
			Check( !j->HasRetainedCstDocument(), "Auto CST-only: non-native Auto-fail retains NO Document" );
			j->release();
			std::remove( pBad );
		}
	}

	// P2 fix: load-once -- re-loading into a live Job is refused (Document/Scene desync otherwise).
	// Use a DIFFERENT, otherwise-valid 2nd scene (distinct name) so ONLY the load-once guard can refuse it --
	// loading the same file twice would mask a neutered guard behind the duplicate-name hard error.
	{
		const std::string paStr = RISE::TestSupport::ScratchPath( "cst_loadvia_reload_a.RISEscene" );
		const char* pa = paStr.c_str();
		const std::string pbStr = RISE::TestSupport::ScratchPath( "cst_loadvia_reload_b.RISEscene" );
		const char* pb = pbStr.c_str();
		if( WriteTmp( pa, "RISE ASCII SCENE 6\nsphere_geometry\n{\nname sa\nradius 1\n}\n" ) &&
		    WriteTmp( pb, "RISE ASCII SCENE 6\nsphere_geometry\n{\nname sb\nradius 2\n}\n" ) ) {
			Job* j = new Job();
			const bool ok1 = j->LoadAsciiSceneViaCst( pa );
			const bool ok2 = j->LoadAsciiSceneViaCst( pb );   // distinct valid scene -> only load-once can refuse it
			Check( ok1, "reload: first load succeeds" );
			Check( !ok2, "reload: second load REFUSED by load-once (not masked by a dup-name error)" );
			j->release();
			std::remove( pa ); std::remove( pb );
		}
	}

	{
		const std::string scenePath = RISE::TestSupport::ScratchPath( "cst_tuple_records.RISEscene" );
		const std::string rawPath = RISE::TestSupport::ScratchPath( "cst_raw_records.rawmesh" );
		const std::string header = "RISE ASCII SCENE 7\n";
		const std::string shader = "advanced_shader\n{\nname tuple_shader\n";
		const std::string sphere = "sphere_geometry\n{\nname tuple_sphere\nradius 1\n}\n";
		for( const char* entries : { "shaderop DefaultEmission\n", "" } ) {
			Check( WriteTmp( scenePath.c_str(), header + shader + entries + "}\n" + sphere ),
				"write advanced shader tuple fixture" );
			auto* job = new Job();
			Check( job->LoadAsciiSceneViaCst( scenePath.c_str() ) &&
				job->GetShaders()->GetItem( "tuple_shader" ) != nullptr,
				*entries ? "advanced shader shorthand registers" : "empty advanced shader registers" );
			job->release();
			std::remove( scenePath.c_str() );
		}
		RefuseCase( "invalid optional depth", scenePath.c_str(),
			header + shader + "shaderop DefaultEmission invalid\n}\n" );
		RefuseCase( "long shader-op name", scenePath.c_str(),
			header + shader + "shaderop " + std::string( 600, 'x' ) + "\n}\n" );
		RefuseCase( "incomplete two-dimensional function row", scenePath.c_str(),
			header + "piecewise_linear_function2d\n{\nname bad_row\ncp 0\n}\n" );
		const std::string rawScene = header + "rawmesh_geometry\n{\nname raw_records\nfile " + rawPath + "\n}\n";
		Check( WriteTmp( rawPath.c_str(), "tri 0 0 0 1 0 0 0 1 0\n" ), "write valid RAW fixture" );
		Case( "valid RAW triangle", scenePath.c_str(), rawScene );
		Check( WriteTmp( rawPath.c_str(), "tri 0 0 0 1 0 0 0 1 0\ntri 0 0\n" ), "write incomplete RAW fixture" );
		RefuseCase( "incomplete RAW record after a valid triangle", scenePath.c_str(), rawScene );
		const std::string raw2Scene = header + "rawmesh2_geometry\n{\nname raw2_records\nfile " + rawPath + "\n}\n";
		const std::string raw2Vertices =
			"3 1\nv 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\n";
		Check( WriteTmp( rawPath.c_str(), raw2Vertices + "t 0 1 2\n" ), "write valid RAW2 fixture" );
		Case( "valid RAW2 triangle", scenePath.c_str(), raw2Scene );
		Check( WriteTmp( rawPath.c_str(), raw2Vertices + "t 0 1\n" ), "write incomplete RAW2 fixture" );
		RefuseCase( "incomplete RAW2 face", scenePath.c_str(), raw2Scene );
		Check( WriteTmp( rawPath.c_str(), "" ), "write empty RAW2 fixture" );
		RefuseCase( "missing RAW2 header", scenePath.c_str(), raw2Scene );
		std::remove( rawPath.c_str() );
	}

	{
		auto* row = new PiecewiseLinearFunction1D();
		{
			PiecewiseLinearFunction2D grid;
			grid.addControlPoint( 0, row );
			grid.addControlPoint( 1, row );
			Check( row->refcount() == 3, "2D function retains each row occurrence" );
			PiecewiseLinearFunction2D copy( grid );
			Check( row->refcount() == 5, "copied 2D function owns its row references" );
			grid.clearControlPoints();
			Check( row->refcount() == 3, "clearing 2D function releases only its own row references" );
		}
		Check( row->refcount() == 1, "destroying the 2D function releases its remaining row references" );
		row->release();

		IShaderOp* op = nullptr;
		if( RISE_API_CreateEmissionShaderOp( &op ) && op ) {
			IShader* standard = nullptr;
			IShader* advanced = nullptr;
			const bool madeStandard = RISE_API_CreateStandardShader( &standard, { op } );
			const bool madeAdvanced = RISE_API_CreateAdvancedShader( &advanced, { op }, { 0 }, { 1 }, "+" );
			Check( madeStandard && madeAdvanced && op->refcount() == 3, "both shader chains retain their shader op" );
			safe_release( standard );
			safe_release( advanced );
			Check( op->refcount() == 1, "destroying shader chains releases their shader-op references" );
			op->release();
		} else {
			Check( false, "create shader op for shader ownership test" );
		}
	}

	std::printf( "%d passed, %d failed.\n", s_pass, s_fail );
	return s_fail == 0 ? 0 : 1;
}
