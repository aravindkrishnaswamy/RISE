//////////////////////////////////////////////////////////////////////
//
//  CstLoadViaCstTest.cpp - P5 (save-as-CST) Slice 1.
//
//  Job::LoadAsciiSceneViaCst loads a scene by building the canonical CST
//  (ParseToCst), deriving the Scene from it (DeriveToJob), and RETAINING the
//  Document for later edit/save (Model-B: "Scene = derive(CST)").  This test
//  proves the new load path produces a Scene byte-equivalent (DumpJob) to the
//  legacy Job::LoadAsciiScene, and that the Document is retained (and is NOT
//  retained by the legacy path).
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

using namespace RISE;
using namespace RISE::Implementation;
using namespace risequiv;

namespace
{
	int s_pass = 0, s_fail = 0;
	void Check( bool ok, const std::string& what ) { if( ok ) ++s_pass; else { ++s_fail; std::printf( "  FAIL: %s\n", what.c_str() ); } }

	bool WriteTmp( const char* path, const std::string& text )
	{
		std::ofstream f( path );
		if( !f ) return false;
		f << text;
		return f.good();
	}

	// Load `v7scene` (written to `path`) via BOTH the legacy parser and the CST path; assert the derived
	// Jobs are DumpJob-equivalent and that only the CST path retains the canonical Document.
	void Case( const char* label, const char* path, const std::string& v7scene )
	{
		if( !WriteTmp( path, v7scene ) ) { Check( false, std::string( label ) + ": write temp scene" ); return; }

		Job* jL = new Job();
		const bool okL = jL->LoadAsciiScene( path );
		Job* jC = new Job();
		const bool okC = jC->LoadAsciiSceneViaCst( path );

		Check( okL, std::string( label ) + ": legacy LoadAsciiScene succeeds" );
		Check( okC, std::string( label ) + ": LoadAsciiSceneViaCst succeeds" );
		if( okL && okC )
			Check( DumpJob( *jL ) == DumpJob( *jC ), std::string( label ) + ": CST-load Scene == legacy-load Scene (DumpJob)" );
		Check( jC->GetCstDocument() != nullptr, std::string( label ) + ": CST load RETAINS the canonical Document" );
		Check( jL->GetCstDocument() == nullptr, std::string( label ) + ": legacy load retains NO Document" );

		jL->release();
		jC->release();
		std::remove( path );
	}
}

int main()
{
	std::printf( "=== CstLoadViaCstTest (P5 Slice 1: load via the canonical CST) ===\n" );

	Case( "painter+material+geom+object+lights", "/tmp/cst_loadvia_1.RISEscene",
		"RISE ASCII SCENE 6\n"
		"uniformcolor_painter\n{\nname p\ncolor 0.8 0.2 0.2\n}\n"
		"lambertian_material\n{\nname m\nreflectance p\n}\n"
		"sphere_geometry\n{\nname g\nradius 1.5\n}\n"
		"standard_object\n{\nname o\ngeometry g\nmaterial m\nposition 0 0 0\n}\n"
		"directional_light\n{\nname key\npower 3.14\ncolor 1.0 0.96 0.90\ndirection 0.4 0.7 0.5\n}\n"
		"ambient_light\n{\nname amb\npower 0.2\ncolor 1 1 1\n}\n" );

	Case( "shared-material refs + comments", "/tmp/cst_loadvia_2.RISEscene",
		"RISE ASCII SCENE 6\n# two spheres sharing one material\n"
		"uniformcolor_painter\n{\nname pp\ncolor 0.1 0.6 0.9\n}\n"
		"lambertian_material\n{\nname mm\nreflectance pp\n}\n"
		"sphere_geometry\n{\nname g1\nradius 1\n}\n"
		"sphere_geometry\n{\nname g2\nradius 2\n}\n"
		"standard_object\n{\nname o1\ngeometry g1\nmaterial mm\nposition -1 0 0\n}\n"
		"standard_object\n{\nname o2\ngeometry g2\nmaterial mm\nposition 1 0 0\n}\n" );

	std::printf( "%d passed, %d failed.\n", s_pass, s_fail );
	return s_fail == 0 ? 0 : 1;
}
