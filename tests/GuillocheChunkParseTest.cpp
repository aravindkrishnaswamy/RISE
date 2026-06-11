//////////////////////////////////////////////////////////////////////
//
//  GuillocheChunkParseTest.cpp - Parse-level contract test for the
//  procedural guilloché chunks (the native replacements for the
//  Python bakers):
//
//    guilloche_dial_geometry   -> Job::AddGuillocheDialGeometry
//    guilloche_oxide_painter   -> Job::AddGuillocheOxideFunction2D
//    swept_band_geometry       -> Job::AddSweptBandGeometry
//    scalar_painter function2d + scale/bias (the affine form)
//
//  The field/mesh MATH is golden-tested elsewhere (GuillocheFieldTest,
//  ProceduralMeshTest); this test owns the PLUMBING contract:
//
//    1. A minimal scene using all three chunks + the scalar_painter
//       function2d affine form PARSES, and the named items land in the
//       right managers (geometry / function2d / scalar painter).
//    2. The rejection paths reject (ParseAndLoadScene == FALSE):
//       unknown enum strings, out-of-range field params, nan/inf and
//       non-numeric tokens (the dispatcher's TEXT-domain numeric
//       validation -- the build's -ffast-math erases value-domain
//       inf/NaN guards, so the token layer is the contract), too few
//       swept-band points, malformed point lines, degenerate stitch
//       parameters, and a function2d reference to a missing function.
//    3. Out-of-range mesh_n CLAMPS (parses fine) rather than rejects.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <string>
#include <cstdio>
#include <cstdlib>

#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/RISE_API.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const char* name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

namespace {

	std::string WriteTempScene( const std::string& tag, const std::string& body )
	{
		const char* tmp = getenv( "TMPDIR" );
		std::string dir = tmp ? tmp : "/tmp/";
		if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
		std::string path = dir + "rise_guilloche_parse_" + tag + ".RISEscene";
		std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
		f << body;
		f.close();
		return path;
	}

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

	// Parse an inline scene body; returns ParseAndLoadScene's verdict.
	// (Job is Reference-counted with a protected dtor -- heap + release.)
	bool ParseBody( const std::string& tag, const std::string& body, Job& job )
	{
		const std::string path = WriteTempScene( tag, "RISE ASCII SCENE 6\n" + body );
		const bool ok = ParseSceneFile( path, job );
		remove( path.c_str() );
		return ok;
	}

	bool ParseBody( const std::string& tag, const std::string& body )
	{
		Job* job = new Job();
		job->addref();
		const bool ok = ParseBody( tag, body, *job );
		job->release();
		return ok;
	}

	// Small chunks reused across cases (mesh_n tiny so the bake is instant).
	const char* kDial =
		"guilloche_dial_geometry\n{\n"
		"name dialg\n"
		"pattern lightning\n"
		"num_arms 11\n"
		"cell_mode select\n"
		"lightning_relief 0.6\n"
		"mesh_n 48\n"
		"}\n";
	const char* kOxide =
		"guilloche_oxide_painter\n{\n"
		"name oxfn\n"
		"metal nb\n"
		"falloff smooth\n"
		"torch_amount 0.4\n"
		"}\n";
	const char* kScalar =
		"scalar_painter\n{\n"
		"name oxthk\n"
		"function2d oxfn\n"
		"scale 13.0\n"
		"bias 24.5\n"
		"}\n";
	const char* kBand =
		"swept_band_geometry\n{\n"
		"name bandg\n"
		"point 24.0 -3.4\n"
		"point 43.0 -7.3\n"
		"point 70.0 -8.68\n"
		"point 104.0 -8.78\n"
		"n_len 40\n"
		"n_wid 8\n"
		"}\n";
	const char* kStitches =
		"swept_band_geometry\n{\n"
		"name stitchg\n"
		"point 24.0 -3.4\n"
		"point 43.0 -7.3\n"
		"point 104.0 -8.78\n"
		"emit_stitches TRUE\n"
		"}\n";

}

static void TestHappyPath()
{
	std::cout << "Test 1: all three chunks + the function2d affine scalar_painter parse and register" << std::endl;
	Job* job = new Job();
	job->addref();
	const bool ok = ParseBody( "happy",
		std::string( kDial ) + kOxide + kScalar + kBand + kStitches, *job );
	Check( ok, "scene parses" );
	IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
	Check( priv != 0, "IJobPriv available" );
	if( !priv ) { job->release(); return; }
	Check( priv->GetGeometries()->GetItem( "dialg" ) != 0,      "dial geometry registered" );
	Check( priv->GetGeometries()->GetItem( "bandg" ) != 0,      "band geometry registered" );
	Check( priv->GetGeometries()->GetItem( "stitchg" ) != 0,    "stitch geometry registered" );
	Check( priv->GetFunction2Ds()->GetItem( "oxfn" ) != 0,      "oxide function2d registered" );
	Check( priv->GetScalarPainters()->GetItem( "oxthk" ) != 0,  "affine function2d scalar painter registered" );
	// the oxide painter evaluates sanely through the registered function
	IFunction2D* f = priv->GetFunction2Ds()->GetItem( "oxfn" );
	const Scalar centre = f->Evaluate( 0.5, 0.5 );
	const Scalar rim    = f->Evaluate( 1.0, 0.5 );
	Check( centre >= 0.0 && centre <= 1.0 && rim >= 0.0 && rim <= 1.0, "dose in [0,1]" );
	Check( rim > centre, "dose increases centre -> rim" );
	job->release();
}

static void TestClamps()
{
	std::cout << "Test 2: out-of-range mesh_n clamps (parses), never rejects" << std::endl;
	Check( ParseBody( "clamp_lo",
		"guilloche_dial_geometry\n{\nname g\nmesh_n 1\n}\n" ), "mesh_n 1 clamps to 8 (floor matches the SDF sampling_detail precedent; a 2x2 grid has every corner outside the dial circle)" );
}

static void TestRejections()
{
	std::cout << "Test 3: rejection paths" << std::endl;
	struct Row { const char* tag; const char* body; const char* what; };
	const Row rows[] = {
		{ "bad_pattern",  "guilloche_dial_geometry\n{\nname g\npattern zigzag\nmesh_n 16\n}\n",
		  "unknown pattern enum rejects" },
		{ "bad_metal",    "guilloche_oxide_painter\n{\nname f\nmetal woof\nactivation_ea 160000\n}\n",
		  "unknown metal rejects even with explicit activation_ea" },
		{ "bad_falloff",  "guilloche_oxide_painter\n{\nname f\nfalloff sideways\n}\n",
		  "unknown falloff enum rejects" },
		{ "arms_zero",    "guilloche_dial_geometry\n{\nname g\nnum_arms 0\nmesh_n 16\n}\n",
		  "num_arms 0 rejects" },
		{ "arms_huge",    "guilloche_dial_geometry\n{\nname g\nnum_arms 1000\nmesh_n 16\n}\n",
		  "num_arms 1000 rejects (cap 256)" },
		{ "cell_zero",    "guilloche_dial_geometry\n{\nname g\ncell 0\nmesh_n 16\n}\n",
		  "cell 0 rejects" },
		{ "radius_nan",   "guilloche_dial_geometry\n{\nname g\nradius nan\nmesh_n 16\n}\n",
		  "radius nan rejects (text-domain)" },
		{ "disp_nan",     "guilloche_dial_geometry\n{\nname g\ndisp nan\nmesh_n 16\n}\n",
		  "disp nan rejects (text-domain; factory has no disp guard)" },
		{ "disp_inf",     "guilloche_dial_geometry\n{\nname g\ndisp inf\nmesh_n 16\n}\n",
		  "disp inf rejects (text-domain)" },
		{ "disp_text",    "guilloche_dial_geometry\n{\nname g\ndisp abc\nmesh_n 16\n}\n",
		  "non-numeric double rejects (text-domain)" },
		{ "ea_huge",      "guilloche_oxide_painter\n{\nname f\nactivation_ea 5e7\n}\n",
		  "activation_ea above 1e6 J/mol rejects" },
		{ "one_point",    "swept_band_geometry\n{\nname b\npoint 24.0 -3.4\n}\n",
		  "single control point rejects" },
		{ "bad_point",    "swept_band_geometry\n{\nname b\npoint 24.0 -3.4\npoint 43.0 abc\n}\n",
		  "malformed point line rejects" },
		{ "stitch_zero",  "swept_band_geometry\n{\nname b\npoint 24.0 -3.4\npoint 104.0 -8.78\nstitch_r 0\nemit_stitches TRUE\n}\n",
		  "stitch_r 0 with emit_stitches rejects" },
		{ "missing_fn",   "scalar_painter\n{\nname s\nfunction2d nosuch\n}\n",
		  "function2d reference to missing function rejects" },
	};
	for( size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); ++i ) {
		Check( !ParseBody( rows[i].tag, rows[i].body ), rows[i].what );
	}
}

int main( int, char** )
{
	std::cout << "GuillocheChunkParseTest -- parse-level plumbing for the procedural chunks" << std::endl << std::endl;
	TestHappyPath();
	TestClamps();
	TestRejections();
	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
