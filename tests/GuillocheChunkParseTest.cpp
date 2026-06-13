//////////////////////////////////////////////////////////////////////
//
//  GuillocheChunkParseTest.cpp - Parse-level contract test for the
//  procedural chunks (the native replacements for the Python bakers):
//
//    cartesian_disk_geometry   -> Job::AddCartesianDiskGeometry
//    expression_function2d     -> Job::AddExpressionFunction2D (the
//                                 in-scene math that supplanted the
//                                 guilloché dial + oxide bakers)
//    sweep_geometry            -> Job::AddSweepGeometry
//    path_instances_geometry   -> Job::AddPathInstancesGeometry
//    scalar_painter function2d + scale/bias (the affine form)
//    function2d_painter        (greyscale colour wrapper)
//
//  The expression MATH is golden-tested elsewhere (ExpressionFunction2DTest,
//  ThermalOxideExprTest, ProceduralMeshTest); this test owns the PLUMBING
//  contract:
//
//    1. A minimal scene using these chunks + the scalar_painter function2d
//       affine form PARSES, and the named items land in the right managers
//       (geometry / function2d / scalar painter).
//    2. The rejection paths reject (ParseAndLoadScene == FALSE): too few
//       profile/path points, malformed point lines, degenerate pitch,
//       missing instancer templates, a function2d reference to a missing
//       function, and malformed/under-specified expressions.  (The
//       dispatcher's TEXT-domain numeric validation -- the build's
//       -ffast-math erases value-domain inf/NaN guards, so the token layer
//       is the contract.)
//    3. Out-of-range mesh_n CLAMPS (parses fine) rather than rejects.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cmath>

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
		"cartesian_disk_geometry\n{\n"
		"name dialg\n"
		"radius 20.6\n"
		"mesh_n 48\n"
		"}\n";
	// A radial expression_function2d (the in-scene heat-tint shape that
	// replaced the guilloché oxide baker): rises centre -> rim, in [0,1].
	const char* kField =
		"expression_function2d\n{\n"
		"name oxfn\n"
		"param R 20.6\n"
		"def rho clamp(hypot((2*u-1)*R,(2*v-1)*R)/R,0,1)\n"
		"expr rho*rho\n"
		"}\n";
	const char* kScalar =
		"scalar_painter\n{\n"
		"name oxthk\n"
		"function2d oxfn\n"
		"scale 13.0\n"
		"bias 24.5\n"
		"}\n";
	const char* kSweep =
		"sweep_geometry\n{\n"
		"name bandg\n"
		"profile_point -2.0 0.5\n"
		"profile_point 2.0 0.5\n"
		"profile_point 2.0 -0.5\n"
		"profile_point -2.0 -0.5\n"
		"point 0 24.0 -3.4\n"
		"point 0 43.0 -7.3\n"
		"point 0 70.0 -8.68\n"
		"point 0 104.0 -8.78\n"
		"n_len 40\n"
		"end_scale_x 0.8\n"
		"}\n";
	const char* kCapsule =
		"sdf_geometry\n{\n"
		"name threadcap\n"
		"part capsule union 0  0 0 0  0 0 0  1 1 1  0.14 0.535 0  0\n"
		"}\n";
	const char* kInstances =
		"path_instances_geometry\n{\n"
		"name stitchg\n"
		"geometry threadcap\n"
		"point -10.5 24.3 -2.0\n"
		"point -9.8 43.2 -5.9\n"
		"point -8.4 104.0 -7.3\n"
		"pitch 2.4\n"
		"slant 16.0\n"
		"detail 12\n"
		"}\n";

}

static void TestHappyPath()
{
	std::cout << "Test 1: the procedural chunks + the function2d affine scalar_painter parse and register" << std::endl;
	Job* job = new Job();
	job->addref();
	const bool ok = ParseBody( "happy",
		std::string( kDial ) + kField + kScalar + kSweep + kCapsule + kInstances, *job );
	Check( ok, "scene parses" );
	IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
	Check( priv != 0, "IJobPriv available" );
	if( !priv ) { job->release(); return; }
	Check( priv->GetGeometries()->GetItem( "dialg" ) != 0,      "cartesian disk geometry registered" );
	Check( priv->GetGeometries()->GetItem( "bandg" ) != 0,      "sweep geometry registered" );
	Check( priv->GetGeometries()->GetItem( "stitchg" ) != 0,    "path-instances geometry registered" );
	Check( priv->GetFunction2Ds()->GetItem( "oxfn" ) != 0,      "expression function2d registered" );
	Check( priv->GetScalarPainters()->GetItem( "oxthk" ) != 0,  "affine function2d scalar painter registered" );
	// the field evaluates sanely through the registered function
	IFunction2D* f = priv->GetFunction2Ds()->GetItem( "oxfn" );
	const Scalar centre = f->Evaluate( 0.5, 0.5 );
	const Scalar rim    = f->Evaluate( 1.0, 0.5 );
	Check( centre >= 0.0 && centre <= 1.0 && rim >= 0.0 && rim <= 1.0, "field in [0,1]" );
	Check( rim > centre, "field increases centre -> rim" );
	job->release();
}

static void TestCartesianDiskValidation()
{
	std::cout << "Test 2: cartesian_disk_geometry validation (radius > 0, non-degenerate mesh)" << std::endl;
	Check( !ParseBody( "radius0",
		"cartesian_disk_geometry\n{\nname g\nradius 0\nmesh_n 16\n}\n" ), "radius 0 rejects" );
	Check( !ParseBody( "degenerate",
		"cartesian_disk_geometry\n{\nname g\nmesh_n 1\n}\n" ), "mesh_n 1 -> 2x2 grid all-outside -> degenerate rejects" );
}

static void TestRejections()
{
	std::cout << "Test 3: rejection paths" << std::endl;
	struct Row { const char* tag; const char* body; const char* what; };
	const Row rows[] = {
		{ "two_prof",     "sweep_geometry\n{\nname b\nprofile_point -1 0\nprofile_point 1 0\npoint 0 0 0\npoint 0 0 10\n}\n",
		  "fewer than 3 profile points rejects" },
		{ "one_point",    "sweep_geometry\n{\nname b\nprofile_point -1 0\nprofile_point 1 0\nprofile_point 0 1\npoint 0 0 0\n}\n",
		  "single path point rejects" },
		{ "bad_point",    "sweep_geometry\n{\nname b\nprofile_point -1 0\nprofile_point 1 0\nprofile_point 0 1\npoint 0 0 0\npoint 0 43.0 abc\n}\n",
		  "malformed path point rejects" },
		{ "bad_scale",    "sweep_geometry\n{\nname b\nprofile_point -1 0\nprofile_point 1 0\nprofile_point 0 1\npoint 0 0 0\npoint 0 0 10\nend_scale_x 0\n}\n",
		  "end_scale_x 0 rejects" },
		{ "no_template",  "path_instances_geometry\n{\nname p\ngeometry nosuchgeom\npoint 0 0 0\npoint 0 0 10\n}\n",
		  "missing template geometry rejects" },
		{ "pitch_zero",   "sdf_geometry\n{\nname cap2\npart sphere union 0  0 0 0  0 0 0  1 1 1  0.5 0 0  0\n}\npath_instances_geometry\n{\nname p\ngeometry cap2\npoint 0 0 0\npoint 0 0 10\npitch 0\n}\n",
		  "pitch 0 rejects" },
		{ "missing_fn",   "scalar_painter\n{\nname s\nfunction2d nosuch\n}\n",
		  "function2d reference to missing function rejects" },
	};
	for( size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); ++i ) {
		Check( !ParseBody( rows[i].tag, rows[i].body ), rows[i].what );
	}
}

static void TestFunction2DColorPainter()
{
	std::cout << "Test 4: function2d_painter (greyscale colour wrapper over an expression field)" << std::endl;
	Job* job = new Job();
	job->addref();
	const char* body =
		"expression_function2d\n{\nname spallf\nparam R 20.6\ndef rho clamp(hypot((2*u-1)*R,(2*v-1)*R)/R,0,1)\nexpr smoothstep(0.6,0.9,rho)\n}\n"
		"function2d_painter\n{\nname spallcol\nfunction2d spallf\n}\n";
	const bool ok = ParseBody( "fn2dcol", body, *job );
	Check( ok, "expression + function2d_painter parse" );
	IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
	if( priv ) {
		Check( priv->GetFunction2Ds()->GetItem( "spallf" ) != 0,   "expression function2d registered" );
		Check( priv->GetPainters()->GetItem( "spallcol" ) != 0,    "function2d_painter colour registered" );
	} else {
		Check( false, "IJobPriv available" );
	}
	job->release();

	// rejection: a function2d_painter referencing a missing source
	Check( !ParseBody( "missing_fn2d", "function2d_painter\n{\nname p\nfunction2d nope\n}\n" ),
		"function2d_painter missing source rejects" );
}

static void TestExpressionAndDisplacement()
{
	std::cout << "Test 5: expression_function2d + cartesian_disk + displaced(uv_seam_fold FALSE)" << std::endl;
	Job* job = new Job();
	job->addref();
	const char* body =
		"expression_function2d\n{\nname ripple\nparam k 6.0\ndef rr hypot(2*u-1,2*v-1)\nexpr 0.5+0.5*sin(k*rr)\n}\n"
		"cartesian_disk_geometry\n{\nname base\nradius 10\nmesh_n 24\n}\n"
		"displaced_geometry\n{\nname relief\nbase_geometry base\ndisplacement ripple\ndisp_scale 0.5\ndetail 1\nuv_seam_fold FALSE\n}\n";
	const bool ok = ParseBody( "expr", body, *job );
	Check( ok, "expression + cartesian disk + displaced parse" );
	IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
	if( priv ) {
		Check( priv->GetPainters()->GetItem( "ripple" ) != 0,    "expression painter registered (colour)" );
		Check( priv->GetFunction2Ds()->GetItem( "ripple" ) != 0, "expression painter registered (function2d)" );
		Check( priv->GetGeometries()->GetItem( "base" ) != 0,    "cartesian disk registered" );
		Check( priv->GetGeometries()->GetItem( "relief" ) != 0,  "displaced geometry registered" );
		IFunction2D* f = priv->GetFunction2Ds()->GetItem( "ripple" );
		Check( f && std::fabs( f->Evaluate( 0.5, 0.5 ) - 0.5 ) < 1e-9, "expression evaluates at centre" );
	} else {
		Check( false, "IJobPriv available" );
	}
	job->release();

	// rejection: a bad expression (unknown variable) must fail the parse
	Check( !ParseBody( "bad_expr", "expression_function2d\n{\nname e\nexpr u + nope\n}\n" ),
		"unknown variable in expr rejects" );
	// rejection: missing final expr
	Check( !ParseBody( "no_expr", "expression_function2d\n{\nname e\nparam k 1\n}\n" ),
		"missing expr rejects" );
}

int main( int, char** )
{
	std::cout << "GuillocheChunkParseTest -- parse-level plumbing for the procedural chunks" << std::endl << std::endl;
	TestHappyPath();
	TestCartesianDiskValidation();
	TestRejections();
	TestFunction2DColorPainter();
	TestExpressionAndDisplacement();
	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
