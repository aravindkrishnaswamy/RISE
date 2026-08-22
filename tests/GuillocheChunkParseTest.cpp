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
//    lathe_geometry            -> Job::AddLatheGeometry (arc-85 C3
//                                 surface of revolution)
//    sdf_geometry `part`       -> SDFGeometry::ParsePartLines (arc-85 C6
//                                 `superellipsoid` primitive)
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
//    2. The rejection paths reject (load == FALSE): too few
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
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cmath>

#include "../src/Library/Job.h"
// arc-85 C3 fix round: asserting the FINALIZE DIAGNOSTIC (not just the
// verdict) needs the CST derive's `diagnostics` out-param -- the load-a-file
// helper below reports a bool only.
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/RISE_API.h"
// C2 fix round (2026-08-14), Fix 2: real-parser coverage for the
// profile_circle/profile_rect conveniences needs the concrete mesh type's
// getVertices()/getFaces() accessors -- ITriangleMeshGeometryIndexed alone
// only exposes numPoints().
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
// expression_function2d/expression_painter unification (doc 88 sect. 7
// decision 5): both now route through the SAME shared helper
// (BuildExpressionProgramFromChunkFields, ExpressionPainter.h) with
// enableContextVars=false/autoRegisterSeed=false for the legacy UV-only
// surface -- TestUnifiedEngineEquivalence below proves the chunk-parsed
// painter's output equals a direct ExpressionProgram::Builder compile of
// the identical body, param-for-param, def-for-def.
#include "../src/Library/Painters/ExpressionEval.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const char* name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}
static void CheckClose( Scalar got, Scalar want, Scalar tol, const char* name )
{
	if( std::fabs( got - want ) <= tol ) { ++passCount; }
	else {
		++failCount;
		std::cout.precision( 12 );
		std::cout << "  FAIL: " << name << "  got " << got << "  want " << want << "  |d| " << std::fabs(got-want) << std::endl;
	}
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
		// Model-B P5 Slice 6c-3b: load via the canonical CST path (native-v7).
		// The rejection paths (too-few points etc. -> false) hold: DeriveToJob
		// refuses-all on a chunk whose descriptor validation fails.
		return job.LoadAsciiSceneViaCst( path.c_str() );
	}

	// Parse an inline scene body; returns the load verdict.
	// (Job is Reference-counted with a protected dtor -- heap + release.)
	bool ParseBody( const std::string& tag, const std::string& body, Job& job )
	{
		const std::string path = WriteTempScene( tag, "RISE ASCII SCENE 7\n" + body );
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

	// Derive an inline body through the CST and return its DIAGNOSTICS,
	// joined.  DeriveToJob prefers the specific reason a Finalize (however
	// deep the call stack) leaves in g_cstFinalizeDiagSink over its generic
	// "<keyword>: apply failed (e.g. unresolved reference); see log" -- so
	// this is how a factory-level refusal's own message is pinned.
	std::string DeriveDiagnostics( const std::string& body )
	{
		Job* job = new Job();
		job->addref();
		RISE::Cst::Document doc = RISE::Cst::ParseToCst( "RISE ASCII SCENE 7\n" + body );
		std::vector<std::string> diags;
		RISE::Cst::DeriveToJob( doc, *job, &diags );
		job->release();
		std::string joined;
		for( std::size_t i = 0; i < diags.size(); ++i ) {
			joined += diags[i];
			joined += "\n";
		}
		return joined;
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
		"point_width 0.7\n"
		"point_width 0.85\n"
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
		{ "pw_toomany",   "sweep_geometry\n{\nname b\nprofile_point -1 0\nprofile_point 1 0\nprofile_point 0 1\npoint 0 0 0\npoint 0 0 10\npoint_width 0.7\npoint_width 0.8\npoint_width 0.9\n}\n",
		  "more point_width than path points rejects" },
		{ "pw_badnum",    "sweep_geometry\n{\nname b\nprofile_point -1 0\nprofile_point 1 0\nprofile_point 0 1\npoint 0 0 0\npoint 0 0 10\npoint_width abc\n}\n",
		  "malformed point_width rejects" },
		{ "pw_nonpos",    "sweep_geometry\n{\nname b\nprofile_point -1 0\nprofile_point 1 0\nprofile_point 0 1\npoint 0 0 0\npoint 0 0 10\npoint_width 0\n}\n",
		  "point_width 0 rejects" },
		// C2 slice: profile_circle / profile_rect mutual exclusion + validation,
		// and path_closed structural validation.
		{ "prof_pt_circ",  "sweep_geometry\n{\nname b\nprofile_point -1 0\nprofile_point 1 0\nprofile_point 0 1\nprofile_circle 1.0\npoint 0 0 0\npoint 0 0 10\n}\n",
		  "profile_point + profile_circle together rejects (mutually exclusive)" },
		{ "circ_rect",     "sweep_geometry\n{\nname b\nprofile_circle 1.0\nprofile_rect 2 2\npoint 0 0 0\npoint 0 0 10\n}\n",
		  "profile_circle + profile_rect together rejects (mutually exclusive)" },
		{ "circ_bad_r",    "sweep_geometry\n{\nname b\nprofile_circle 0\npoint 0 0 0\npoint 0 0 10\n}\n",
		  "profile_circle radius <= 0 rejects" },
		{ "rect_r_toobig", "sweep_geometry\n{\nname b\nprofile_rect 2 2 5\npoint 0 0 0\npoint 0 0 10\n}\n",
		  "profile_rect corner radius > min(w,h)/2 rejects" },
		{ "closed_2pts",   "sweep_geometry\n{\nname b\nprofile_circle 1.0\npoint 0 0 0\npoint 0 0 10\npath_closed TRUE\n}\n",
		  "path_closed with only 2 path points rejects" },
		{ "closed_dup",    "sweep_geometry\n{\nname b\nprofile_circle 1.0\npoint 0 0 0\npoint 5 5 0\npoint 0 0 0\npath_closed TRUE\n}\n",
		  "path_closed with the first and last point authored coincident rejects" },
		{ "closed_scale",  "sweep_geometry\n{\nname b\nprofile_circle 1.0\npoint 0 0 0\npoint 5 5 0\npoint 10 0 0\npath_closed TRUE\nend_scale_x 0.5\n}\n",
		  "path_closed with end_scale_x != 1.0 rejects" },
		{ "closed_cap",    "sweep_geometry\n{\nname b\nprofile_circle 1.0\npoint 0 0 0\npoint 5 5 0\npoint 10 0 0\npath_closed TRUE\ncap_start TRUE\n}\n",
		  "path_closed with cap_start explicitly TRUE rejects" },
		// C2 fix round: point_scale (Fix 1) mirrors point_width's own
		// validation rows exactly -- repeatable, <= path point count,
		// exactly one number per line, > 0.
		{ "ps_toomany",   "sweep_geometry\n{\nname b\nprofile_point -1 0\nprofile_point 1 0\nprofile_point 0 1\npoint 0 0 0\npoint 0 0 10\npoint_scale 0.7\npoint_scale 0.8\npoint_scale 0.9\n}\n",
		  "more point_scale than path points rejects" },
		{ "ps_badnum",    "sweep_geometry\n{\nname b\nprofile_point -1 0\nprofile_point 1 0\nprofile_point 0 1\npoint 0 0 0\npoint 0 0 10\npoint_scale abc\n}\n",
		  "malformed point_scale rejects" },
		{ "ps_nonpos",    "sweep_geometry\n{\nname b\nprofile_point -1 0\nprofile_point 1 0\nprofile_point 0 1\npoint 0 0 0\npoint 0 0 10\npoint_scale 0\n}\n",
		  "point_scale 0 rejects" },
		// C2 fix round, Fix 4/5 (comment corrected 2026-08-14): NaN is
		// actually rejected by the TOKEN-level finiteness gate
		// (AllTokensAreFiniteNumbers, checked before the sscanf/range-check
		// even runs) -- "nan" is not a finite-number token, so this row
		// never reaches the range check at all.  The range check's own
		// negated-idiom form `!( r >= 0.0 && r <= halfMin )` (vs. the old
		// `r < 0 || r > halfMin`, which passes NaN through both branches)
		// is still correct and kept as DEFENSE IN DEPTH for any future
		// caller that reaches Finalize with a NaN past the token gate.
		{ "rect_r_nan",   "sweep_geometry\n{\nname b\nprofile_rect 2 2 nan\npoint 0 0 0\npoint 0 0 10\n}\n",
		  "profile_rect corner radius nan rejects" },
		// C2 fix round, Fix 5: profile_circle / profile_rect have OPTIONAL
		// trailing fields, so a bare sscanf conversion count can't tell
		// "the optional field was omitted" from "garbage is glued onto the
		// last field sscanf could parse" -- `1.0abc` stops the radius %lf
		// at `1.0`, leaving `abc` for the `n` %lf to fail on, and sscanf
		// halts there instead of erroring (so the "n" field just silently
		// defaults, dropping "abc" on the floor).  Same trap for
		// profile_rect's optional corner radius.
		{ "circ_trailing", "sweep_geometry\n{\nname b\nprofile_circle 1.0abc\npoint 0 0 0\npoint 0 0 10\n}\n",
		  "profile_circle radius with glued trailing garbage rejects" },
		{ "rect_trailing", "sweep_geometry\n{\nname b\nprofile_rect 2 2extra\npoint 0 0 0\npoint 0 0 10\n}\n",
		  "profile_rect height with glued trailing garbage rejects" },
	};
	for( size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); ++i ) {
		Check( !ParseBody( rows[i].tag, rows[i].body ), rows[i].what );
	}
}

static void TestProfileConveniencesAndClosedLoop()
{
	std::cout << "Test 3b: profile_circle / profile_rect conveniences + path_closed -- happy paths" << std::endl;
	Job* job = new Job();
	job->addref();
	const bool ok = ParseBody( "conveniences",
		// profile_circle
		"sweep_geometry\n{\nname circg\nprofile_circle 2.0 16\npoint 0 0 0\npoint 0 0 10\n}\n"
		// profile_rect, sharp
		"sweep_geometry\n{\nname rectg\nprofile_rect 2.0 1.0\npoint 0 0 0\npoint 0 0 10\n}\n"
		// profile_rect, rounded
		"sweep_geometry\n{\nname rrectg\nprofile_rect 2.0 1.0 0.3\npoint 0 0 0\npoint 0 0 10\n}\n"
		// path_closed loop (circular profile, non-coincident first/last point)
		"sweep_geometry\n{\nname loopg\nprofile_circle 1.0\npoint 0 0 0\npoint 5 5 0\npoint 10 0 0\npath_closed TRUE\n}\n"
		// C2 fix round, Fix 1: point_scale -- a per-station UNIFORM (both
		// axes) taper, composed multiplicatively with point_width (x only)
		// and end_scale -- on an OPEN path, exercising it alongside
		// point_width so both tracks resolve in the same chunk.
		"sweep_geometry\n{\nname scaleg\nprofile_circle 1.0\npoint 0 0 0\npoint 0 5 2\npoint 0 8 6\n"
		"point_width 1.0\npoint_width 0.7\npoint_width 0.5\n"
		"point_scale 1.0\npoint_scale 0.8\npoint_scale 0.4\n}\n"
		// point_scale on a CLOSED loop (periodic sampling) -- point_scale
		// stays legal there even though end_scale must not (a loop has no
		// end to taper toward, but per-station scale is still periodic).
		"sweep_geometry\n{\nname scaleloopg\nprofile_circle 1.0\npoint 0 0 0\npoint 5 5 0\npoint 10 0 0\npath_closed TRUE\n"
		"point_scale 1.0\npoint_scale 0.6\npoint_scale 0.8\n}\n",
		*job );
	Check( ok, "profile_circle / profile_rect / path_closed / point_scale scene parses" );
	IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
	Check( priv != 0, "IJobPriv available" );
	if( priv ) {
		Check( priv->GetGeometries()->GetItem( "circg" ) != 0,  "profile_circle sweep registered" );
		Check( priv->GetGeometries()->GetItem( "rectg" ) != 0,  "profile_rect (sharp) sweep registered" );
		Check( priv->GetGeometries()->GetItem( "rrectg" ) != 0, "profile_rect (rounded) sweep registered" );
		Check( priv->GetGeometries()->GetItem( "loopg" ) != 0,  "path_closed loop sweep registered" );
		Check( priv->GetGeometries()->GetItem( "scaleg" ) != 0, "point_scale x point_width (open path) sweep registered" );
		Check( priv->GetGeometries()->GetItem( "scaleloopg" ) != 0, "point_scale on a closed loop sweep registered" );
	}
	job->release();
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

// C2 fix round (2026-08-14), Fix 2: real-parser coverage for the profile
// conveniences.  Test 3b above only asserts the geometry REGISTERED; the
// parser's own profile_rect corner-arc tessellation + conditional dedup
// (ChunkParserRegistry.cpp's `profile_rect` branch, ~ line 5766) had zero
// coverage through the actual chunk-parse path -- ProceduralMeshTest's
// coverage of that same expansion algebra is a hand-copied re-derivation
// of the formula, which validates itself, not the parser.  Every fixture
// here uses a straight 2-point path along +Z with `n_len 2` (the minimum
// the descriptor accepts) and caps off, so the station count is exactly 3
// (segs=1, per=max(2,2)=2, out=segs*per+1=3) and ring 0 is exactly the
// first NP vertices in mesh order -- deterministic, no re-derivation of
// BuildPathFrames needed.
static void TestProfileConvenienceRealParserCoverage()
{
	std::cout << "Test 3c: profile_rect / profile_circle conveniences -- REAL PARSER vertex-count + ring-integrity coverage" << std::endl;

	auto ringCheck = [&]( const TriangleMeshGeometryIndexed* mesh, unsigned int NP, const char* label ) {
		// no two CONSECUTIVE ring-0 points (cyclically) are coincident, and
		// the ring's signed area (shoelace, in the path's local x/y plane --
		// the path here is a straight +Z line with no frame_hint, so
		// BuildPathFrames picks B[0] = world +X (the axis-tie-break falls to
		// x first) and N[0] = cross(B[0], T[0]) = world -Y; the mesh vertex
		// x/y are therefore (px, -ph), NOT (px, ph) directly) is NEGATIVE --
		// i.e. still CCW in the profile's OWN (px, ph) frame after the
		// parser's dedup (matching the descriptor's documented convention,
		// CCW = outward normals), which appears CW once embedded because
		// cross(B, N) = -T always (B _|_ T, so B x (B x T) = -T) -- the
		// sweep's (B, N) basis is structurally left-handed relative to T,
		// not a fixture-specific quirk of this path.
		bool noCoincident = true;
		Scalar area2 = 0;
		for( unsigned int k = 0; k < NP; ++k ) {
			const unsigned int k1 = ( k + 1 ) % NP;
			const Vertex& p0 = mesh->getVertices()[k];
			const Vertex& p1 = mesh->getVertices()[k1];
			const Scalar dx = p1.x - p0.x, dy = p1.y - p0.y;
			if( std::fabs( dx ) < 1e-9 && std::fabs( dy ) < 1e-9 ) noCoincident = false;
			area2 += p0.x * p1.y - p1.x * p0.y;
		}
		std::string coincLabel = std::string( label ) + ": ring 0 has no consecutive coincident points";
		std::string areaLabel  = std::string( label ) + ": ring 0 has negative embedded shoelace area (CCW in the profile's own frame, mirrored by B x N = -T)";
		Check( noCoincident, coincLabel.c_str() );
		Check( area2 < 0, areaLabel.c_str() );
	};

	// profile_rect 4 2 1: r == min(w,h)/2 (a capsule -- the two collapsed
	// sides are along the SHORTER dimension), NP = 18 pins the conditional
	// per-corner dedup (4 corners * 5 raw samples = 20, minus the two
	// coincident boundaries where adjacent corners share a centre).
	{
		Job* job = new Job();
		job->addref();
		const bool ok = ParseBody( "prof_rect_18",
			"sweep_geometry\n{\nname r18\nprofile_rect 4 2 1\npoint 0 0 0\npoint 0 0 10\nn_len 2\ncap_start FALSE\ncap_end FALSE\n}\n", *job );
		Check( ok, "profile_rect 4 2 1 parses" );
		IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
		if( priv ) {
			IGeometry* g = priv->GetGeometries()->GetItem( "r18" );
			const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( g );
			Check( mesh != 0, "profile_rect 4 2 1: concrete mesh type available" );
			if( mesh ) {
				const unsigned int NP = 18;
				Check( mesh->numPoints() == 3 * NP,
					"profile_rect 4 2 1: MONEY ASSERTION: total vertex count pins NP == 18 through the REAL parser" );
				ringCheck( mesh, NP, "profile_rect 4 2 1" );
			}
		} else {
			Check( false, "IJobPriv available" );
		}
		job->release();
	}

	// profile_rect 2 2 1: r == min(w,h)/2 AND w == h -- the full-collapse
	// case (all four corners share one centre, a circle from 4 quarter
	// arcs).  NP = 16 pins BOTH the 3 consecutive-boundary dedups AND the
	// separate cyclic wrap-around dedup (first/last point coincide too).
	{
		Job* job = new Job();
		job->addref();
		const bool ok = ParseBody( "prof_rect_16",
			"sweep_geometry\n{\nname r16\nprofile_rect 2 2 1\npoint 0 0 0\npoint 0 0 10\nn_len 2\ncap_start FALSE\ncap_end FALSE\n}\n", *job );
		Check( ok, "profile_rect 2 2 1 parses" );
		IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
		if( priv ) {
			IGeometry* g = priv->GetGeometries()->GetItem( "r16" );
			const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( g );
			Check( mesh != 0, "profile_rect 2 2 1: concrete mesh type available" );
			if( mesh ) {
				const unsigned int NP = 16;
				Check( mesh->numPoints() == 3 * NP,
					"profile_rect 2 2 1: MONEY ASSERTION: total vertex count pins NP == 16 (dedup + cyclic-wrap dedup) through the REAL parser" );
				ringCheck( mesh, NP, "profile_rect 2 2 1" );
			}
		} else {
			Check( false, "IJobPriv available" );
		}
		job->release();
	}

	// profile_circle 2.0 (no n): NP = 24 pins the descriptor's documented
	// default n (24, clamped 3..512) through the real parser -- a deleted
	// default or a changed clamp would silently drift this without a
	// parse-path assertion.
	{
		Job* job = new Job();
		job->addref();
		const bool ok = ParseBody( "prof_circ_24",
			"sweep_geometry\n{\nname c24\nprofile_circle 2.0\npoint 0 0 0\npoint 0 0 10\nn_len 2\ncap_start FALSE\ncap_end FALSE\n}\n", *job );
		Check( ok, "profile_circle 2.0 (no n) parses" );
		IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
		if( priv ) {
			IGeometry* g = priv->GetGeometries()->GetItem( "c24" );
			const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( g );
			Check( mesh != 0, "profile_circle 2.0: concrete mesh type available" );
			if( mesh ) {
				const unsigned int NP = 24;
				Check( mesh->numPoints() == 3 * NP,
					"profile_circle 2.0: MONEY ASSERTION: total vertex count pins the default n == 24 through the REAL parser" );
				ringCheck( mesh, NP, "profile_circle 2.0" );
			}
		} else {
			Check( false, "IJobPriv available" );
		}
		job->release();
	}
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

// arc-85 C3 (2026-08-18): `lathe_geometry` -- the surface-of-revolution
// chunk.  The MESH is proven from first principles in ProceduralMeshTest
// (cylinder / cone / sphere identities, exact angular span, pole
// collapse, outward winding by ray probe); this owns the PARSE contract:
// every documented validation refusal, the clamp that is a warning rather
// than a refusal, and one money assertion pinning the emitted vertex
// count through the REAL parser so a silently-changed default or a
// dropped pole collapse cannot slip past the chunk layer.
static void TestLatheChunk()
{
	std::cout << "Test 6: lathe_geometry -- parse-level plumbing, validation, and the pole-collapse vertex count" << std::endl;

	// (a) happy path: a vase (both ends on the axis), a partial-sweep
	// cutaway, an explicit axis, and smooth FALSE all register.
	{
		Job* job = new Job();
		job->addref();
		const bool ok = ParseBody( "lathe_happy",
			"lathe_geometry\n{\nname vaseg\n"
			"profile_point 0.00 0.00\nprofile_point 0.35 0.05\nprofile_point 0.42 0.30\n"
			"profile_point 0.18 0.72\nprofile_point 0.22 0.90\nprofile_point 0.00 0.94\n"
			"axis y\nsweep_degrees 360\nn_radial 48\nsmooth TRUE\n}\n"
			"lathe_geometry\n{\nname cutg\n"
			"profile_point 0.5 0\nprofile_point 0.5 2\n"
			"sweep_degrees 90\nn_radial 12\n}\n"
			"lathe_geometry\n{\nname axzg\n"
			"profile_point 0.5 0\nprofile_point 0.5 2\naxis z\n}\n"
			"lathe_geometry\n{\nname facetg\n"
			"profile_point 0.5 0\nprofile_point 0.8 1\nprofile_point 0.5 2\nsmooth FALSE\n}\n",
			*job );
		Check( ok, "lathe_geometry happy-path scene parses" );
		IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
		Check( priv != 0, "IJobPriv available" );
		if( priv ) {
			Check( priv->GetGeometries()->GetItem( "vaseg" )  != 0, "lathe vase registered" );
			Check( priv->GetGeometries()->GetItem( "cutg" )   != 0, "lathe partial-sweep cutaway registered" );
			Check( priv->GetGeometries()->GetItem( "axzg" )   != 0, "lathe with axis z registered" );
			Check( priv->GetGeometries()->GetItem( "facetg" ) != 0, "lathe with smooth FALSE registered" );
		}
		job->release();
	}

	// (b) MONEY ASSERTION through the REAL parser: a vase profile with both
	// endpoints on the axis collapses each to ONE vertex, so the count is
	// 2 + rings*n_radial -- NOT rows*n_radial (poles not collapsed) and NOT
	// rows*(n_radial+1) (a duplicated 360-degree seam column).  n_radial is
	// authored explicitly so the assertion does not silently follow a
	// changed default.
	{
		Job* job = new Job();
		job->addref();
		const bool ok = ParseBody( "lathe_counts",
			"lathe_geometry\n{\nname lc\n"
			"profile_point 0 0\nprofile_point 1 1\nprofile_point 1 2\nprofile_point 0 3\n"
			"n_radial 8\n}\n", *job );
		Check( ok, "lathe vertex-count fixture parses" );
		IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
		if( priv ) {
			IGeometry* g = priv->GetGeometries()->GetItem( "lc" );
			const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( g );
			Check( mesh != 0, "lathe count fixture: concrete mesh type available" );
			if( mesh ) {
				Check( mesh->numPoints() == 2 + 2 * 8,
					"lathe: MONEY ASSERTION -- 2 pole vertices + 2 rings x n_radial 8, through the REAL parser "
					"(not 4*8 with uncollapsed poles, not 4*9 with a duplicated seam column)" );
				Check( mesh->getFaces().size() == (size_t)( 8 + 2 * 8 + 8 ),
					"lathe: two pole fans of n_radial plus one full band of 2*n_radial" );
			}
		} else {
			Check( false, "IJobPriv available" );
		}
		job->release();
	}

	// (c) n_radial CLAMPS (a warning, not a refusal) -- and the clamp is
	// observable in the emitted vertex count.
	{
		Job* job = new Job();
		job->addref();
		const bool ok = ParseBody( "lathe_clamp",
			"lathe_geometry\n{\nname lcl\nprofile_point 1 0\nprofile_point 1 1\nn_radial 1\n}\n", *job );
		Check( ok, "lathe n_radial 1 CLAMPS (parses) rather than rejecting" );
		IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
		if( priv ) {
			IGeometry* g = priv->GetGeometries()->GetItem( "lcl" );
			const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( g );
			Check( mesh && mesh->numPoints() == 6, "lathe n_radial 1 clamped to the minimum 3 columns" );
		}
		job->release();
	}

	// (d) every documented refusal.
	struct Row { const char* tag; const char* body; const char* what; };
	const Row rows[] = {
		{ "lathe_one_pt",   "lathe_geometry\n{\nname l\nprofile_point 1 0\n}\n",
		  "lathe: a single profile_point rejects (need at least 2)" },
		{ "lathe_no_pt",    "lathe_geometry\n{\nname l\nn_radial 16\n}\n",
		  "lathe: no profile_point at all rejects" },
		{ "lathe_arity3",   "lathe_geometry\n{\nname l\nprofile_point 1 0 5\nprofile_point 1 1\n}\n",
		  "lathe: a 3-number profile_point rejects (wrong arity)" },
		{ "lathe_arity1",   "lathe_geometry\n{\nname l\nprofile_point 1\nprofile_point 1 1\n}\n",
		  "lathe: a 1-number profile_point rejects (wrong arity)" },
		{ "lathe_nonnum",   "lathe_geometry\n{\nname l\nprofile_point 1 abc\nprofile_point 1 1\n}\n",
		  "lathe: a non-numeric profile_point rejects" },
		{ "lathe_trailing", "lathe_geometry\n{\nname l\nprofile_point 0.35abc 0\nprofile_point 1 1\n}\n",
		  "lathe: a profile_point with glued trailing garbage rejects (sscanf would silently truncate it)" },
		{ "lathe_nan",      "lathe_geometry\n{\nname l\nprofile_point nan 0\nprofile_point 1 1\n}\n",
		  "lathe: a nan profile_point rejects at the TOKEN layer" },
		{ "lathe_inf",      "lathe_geometry\n{\nname l\nprofile_point 1 inf\nprofile_point 1 1\n}\n",
		  "lathe: an inf profile_point rejects at the TOKEN layer" },
		{ "lathe_negr",     "lathe_geometry\n{\nname l\nprofile_point -1 0\nprofile_point 1 1\n}\n",
		  "lathe: a NEGATIVE radius rejects (the profile lives in a half-plane)" },
		{ "lathe_allaxis",  "lathe_geometry\n{\nname l\nprofile_point 0 0\nprofile_point 0 1\nprofile_point 0 2\n}\n",
		  "lathe: an ALL-on-axis profile rejects (zero area)" },
		{ "lathe_sweep0",   "lathe_geometry\n{\nname l\nprofile_point 1 0\nprofile_point 1 1\nsweep_degrees 0\n}\n",
		  "lathe: sweep_degrees 0 rejects" },
		{ "lathe_sweepneg", "lathe_geometry\n{\nname l\nprofile_point 1 0\nprofile_point 1 1\nsweep_degrees -90\n}\n",
		  "lathe: a negative sweep_degrees rejects" },
		{ "lathe_sweep361", "lathe_geometry\n{\nname l\nprofile_point 1 0\nprofile_point 1 1\nsweep_degrees 361\n}\n",
		  "lathe: sweep_degrees > 360 rejects" },
		{ "lathe_sweepnan", "lathe_geometry\n{\nname l\nprofile_point 1 0\nprofile_point 1 1\nsweep_degrees nan\n}\n",
		  "lathe: a nan sweep_degrees rejects at the dispatcher's numeric ValueKind gate" },
		{ "lathe_badaxis",  "lathe_geometry\n{\nname l\nprofile_point 1 0\nprofile_point 1 1\naxis w\n}\n",
		  "lathe: an axis that is not x/y/z rejects" },
		{ "lathe_upaxis",   "lathe_geometry\n{\nname l\nprofile_point 1 0\nprofile_point 1 1\naxis Y\n}\n",
		  "lathe: an UPPER-CASE axis rejects -- the descriptor's enumValues {x,y,z} IS the accepted set, so autocomplete/read_schema cannot understate what parses" },
		{ "lathe_zeroarea", "lathe_geometry\n{\nname l\nprofile_point 5 3\nprofile_point 5 3\nprofile_point 5 3\n}\n",
		  "lathe: a profile of identical points rejects (every segment is zero-length -- it revolves to zero area)" },
		{ "lathe_unknown",  "lathe_geometry\n{\nname l\nprofile_point 1 0\nprofile_point 1 1\nsweep_deg 90\n}\n",
		  "lathe: an undeclared parameter name rejects (descriptor is the accepted set)" },
	};
	for( size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); ++i ) {
		Check( !ParseBody( rows[i].tag, rows[i].body ), rows[i].what );
	}

	// (d2) FACTORY-LEVEL refusals must reach the CST DIAGNOSTIC, not fold
	// into the generic "apply failed (e.g. unresolved reference); see log"
	// -- which is actively misleading here, since no reference is involved.
	// Three identical profile_point lines pass every check the chunk parser
	// itself makes and die inside the factory, so this is the reachable
	// representative of that whole class (the >4096 cap, the ear-clip
	// failure and the vertex budget travel the same channel).
	{
		const std::string diag = DeriveDiagnostics(
			"lathe_geometry\n{\nname zerog\nprofile_point 5 3\nprofile_point 5 3\nprofile_point 5 3\n}\n" );
		Check( diag.find( "zero surface area" ) != std::string::npos,
			"lathe: MONEY ASSERTION -- a FACTORY-level refusal's own reason reaches the CST diagnostic" );
		Check( diag.find( "zerog" ) != std::string::npos,
			"lathe: the factory-level diagnostic names the geometry it refused" );
		Check( diag.find( "unresolved reference" ) == std::string::npos,
			"lathe: the factory-level refusal does NOT fold into the generic unresolved-reference message" );
	}

	// (e) a zero-length profile segment (the documented duplicate-a-point
	// hard-edge idiom) is ACCEPTED -- and must not emit a zero-area band.
	{
		Job* job = new Job();
		job->addref();
		const bool ok = ParseBody( "lathe_dup",
			"lathe_geometry\n{\nname ldup\n"
			"profile_point 1 0\nprofile_point 1 1\nprofile_point 1 1\nprofile_point 0.6 2\n"
			"n_radial 8\n}\n", *job );
		Check( ok, "lathe: a duplicated profile_point (hard-edge idiom) is ACCEPTED" );
		IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
		if( priv ) {
			IGeometry* g = priv->GetGeometries()->GetItem( "ldup" );
			const TriangleMeshGeometryIndexed* mesh = dynamic_cast<TriangleMeshGeometryIndexed*>( g );
			Check( mesh && mesh->numPoints() == 4 * 8, "lathe hard edge: all four rows still emitted" );
			// the zero-length segment contributes NO band, so 2 bands x 2*8
			Check( mesh && mesh->getFaces().size() == (size_t)( 2 * 2 * 8 ),
				"lathe hard edge: MONEY ASSERTION -- the zero-length segment emits NO band (no zero-area triangles)" );
		}
		job->release();
	}
}

//! arc-85 C6: the `superellipsoid` SDF part token, through the REAL chunk
//! parser.  `sdf_geometry` forwards `part` lines verbatim to
//! SDFGeometry::ParsePartLines, so a new primitive needs no chunk change --
//! which is exactly the claim worth a parse-level test rather than a unit
//! test on the grammar function alone.  Three rows: the token is accepted and
//! the geometry registers; out-of-range exponents CLAMP-AND-WARN rather than
//! rejecting the scene (the field clamps unconditionally anyway, because
//! `part<i>.size` is keyframable); a malformed line still hard-fails.
static void TestSDFSuperellipsoidPartLines()
{
	std::cout << "Test 9: sdf_geometry `superellipsoid` part lines (arc-85 C6)" << std::endl;

	Job* job = new Job();
	job->addref();
	const bool ok = ParseBody( "superell",
		// the continuum, plus a composed pair: exponents inside range, a
		// non-uniform per-part scale carrying the ellipsoidal proportions,
		// and a superellipsoid smin-blended onto a sphere
		"sdf_geometry\n{\nname cushiong\n"
		"part superellipsoid union 0  0 0 0  0 0 0  1.6 1.0 0.9  1.0 0.45 0.6  0\n}\n"
		"sdf_geometry\n{\nname blendg\n"
		"part sphere union 0  0 0 0  0 0 0  1 1 1  1.0 0 0  0\n"
		"part superellipsoid smin 0.5  1.4 0 0  0 15 0  1 1 1  0.8 0.3 1.0  0\n}\n"
		"sdf_geometry\n{\nname octag\n"
		"part superellipsoid union 0  0 0 0  0 0 0  1 1 1  1.0 2.0 2.0  0\n}\n",
		*job );
	Check( ok, "superellipsoid part lines parse through sdf_geometry" );
	IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
	Check( priv != 0, "IJobPriv available" );
	if( priv ) {
		Check( priv->GetGeometries()->GetItem( "cushiong" ) != 0, "superellipsoid sdf geometry registered" );
		Check( priv->GetGeometries()->GetItem( "blendg" ) != 0,   "superellipsoid smin sphere registered" );
		Check( priv->GetGeometries()->GetItem( "octag" ) != 0,    "e1 = e2 = 2 (octahedron) registered" );
	}
	job->release();

	// Out of range on EITHER exponent: warns on the console, clamps, and the
	// scene still LOADS.  (Rejecting would fail an entire scene over a
	// taste-level authoring slip, and the field clamps regardless.)
	Check( ParseBody( "superell_hi",
		"sdf_geometry\n{\nname g\npart superellipsoid union 0  0 0 0  0 0 0  1 1 1  1.0 6.0 1.0  0\n}\n" ),
		"exponent above the supported range CLAMPS (loads, does not reject)" );
	Check( ParseBody( "superell_lo",
		"sdf_geometry\n{\nname g\npart superellipsoid union 0  0 0 0  0 0 0  1 1 1  1.0 1.0 0.001  0\n}\n" ),
		"exponent below the supported range CLAMPS (loads, does not reject)" );

	// ...and the grammar is no softer for the new token than for the old ones.
	Check( !ParseBody( "superell_short",
		"sdf_geometry\n{\nname g\npart superellipsoid union 0  0 0 0  0 0 0  1 1 1  1.0 1.0\n}\n" ),
		"short superellipsoid part line (14 tokens) still rejects" );
	Check( !ParseBody( "superell_trail",
		"sdf_geometry\n{\nname g\npart superellipsoid union 0  0 0 0  0 0 0  1 1 1  1.0 1.0 1.0  0  9\n}\n" ),
		"trailing token on a superellipsoid part line still rejects" );
	Check( !ParseBody( "superell_typo",
		"sdf_geometry\n{\nname g\npart superelipsoid union 0  0 0 0  0 0 0  1 1 1  1.0 1.0 1.0  0\n}\n" ),
		"a misspelled primitive token still rejects (no silent fallback to sphere)" );
	Check( !ParseBody( "superell_firstop",
		"sdf_geometry\n{\nname g\npart superellipsoid subtract 0  0 0 0  0 0 0  1 1 1  1.0 1.0 1.0  0\n}\n" ),
		"a leading `subtract` still rejects for the new primitive too" );
}

//! expression_function2d / expression_painter unification (doc 88 sect. 7
//! decision 5): ExpressionFunction2DPainterAsciiChunkParser::Finalize was
//! rewritten to call the SAME shared helper
//! (Implementation::BuildExpressionProgramFromChunkFields, also used by
//! Job::AddExpressionPainter and the scalar_painter{expression} chunk)
//! instead of hand-rolling its own param/def parsing.  Two pins:
//!
//!   1. Equivalence: a legacy body with param + def + expr, loaded through
//!      the REAL chunk parser (exercising the rewritten Finalize), must
//!      evaluate identically to the SAME body compiled directly through
//!      ExpressionProgram::Builder with EnableContextVars(false) and no
//!      auto-registered `seed` -- i.e. exactly the enableContextVars=false,
//!      autoRegisterSeed=false arguments the chunk parser now passes.
//!   2. Legacy-chunk-still-parses: the scene load itself must succeed and
//!      register the function2d under its name -- a regression in the
//!      shared helper's plumbing (wrong flag, wrong param grammar) would
//!      show up here as either a parse failure or a value mismatch.
static void TestUnifiedEngineEquivalence()
{
	std::cout << "Test 10: expression_function2d/expression_painter unification -- chunk-parsed == direct Builder compile" << std::endl;

	// A body deliberately shaped like the in-tree scenes' usage: a param,
	// a def referencing that param, and a u/v-only final expression.  Every
	// `param` line here is the plain `<name> <number>` form used across all
	// 10 in-tree scene files (38 expression_function2d chunk instances)
	// that reference expression_function2d.
	const char* kName = "unif_eq";
	const char* kBody =
		"expression_function2d\n{\n"
		"name unif_eq\n"
		"param R 20.6\n"
		"param k 6.0\n"
		"def rho clamp(hypot((2*u-1)*R,(2*v-1)*R)/R,0,1)\n"
		"expr 0.5+0.5*sin(k*rho)+rho*rho\n"
		"}\n";

	// The frozen UV-only contract (doc 88 sect. 7 decision 5): a body
	// referencing a full-context variable must still hard-reject through
	// the REAL chunk parser, end to end -- pins enableContextVars=false at
	// the parser boundary, not just at the Builder level (already covered
	// by ExpressionFunction2DTest.cpp's Test 1).
	Check( !ParseBody( "unif_ctxvar", "expression_function2d\n{\nname e\nexpr time*2+1\n}\n" ),
		"context var `time` still rejects through the chunk parser (frozen UV-only contract)" );

	// Sibling half of the frozen contract: autoRegisterSeed=false means
	// expression_function2d never auto-registers a `seed` param the way
	// expression_painter does -- a body referencing `seed` with no
	// explicit `param seed <v>` line of its own must hard-reject as an
	// unknown variable, not silently resolve.  A seed-free body (the one
	// above) can't distinguish this flag either way, so it needs its own
	// pin -- without this, a future flip of ONLY autoRegisterSeed would
	// sail through every other check in this test.
	Check( !ParseBody( "unif_noseed", "expression_function2d\n{\nname e\nexpr seed+u*v\n}\n" ),
		"unqualified `seed` still rejects through the chunk parser (no auto-registered seed param)" );

	Job* job = new Job();
	job->addref();
	const bool ok = ParseBody( "unif_eq", kBody, *job );
	Check( ok, "legacy expression_function2d chunk (param+def+expr) still parses through the unified helper" );
	IJobPriv* priv = dynamic_cast<IJobPriv*>( job );
	Check( priv != 0, "IJobPriv available" );
	if( !ok || !priv ) { job->release(); return; }
	IFunction2D* chunkParsed = priv->GetFunction2Ds()->GetItem( kName );
	Check( chunkParsed != 0, "expression function2d registered under its name" );

	// The identical body compiled directly, matching exactly what the
	// rewritten Finalize now does: EnableContextVars(false), no
	// AddParam("seed", ...) auto-registration.
	ExpressionProgram direct = ExpressionProgram::Invalid();
	{
		ExpressionProgram::Builder b;
		b.EnableContextVars( false );
		Check( b.AddParam( "R", 20.6 ), "direct compile: AddParam R" );
		Check( b.AddParam( "k", 6.0 ), "direct compile: AddParam k" );
		Check( b.AddDef( "rho", "clamp(hypot((2*u-1)*R,(2*v-1)*R)/R,0,1)" ), "direct compile: AddDef rho" );
		Check( b.Finalize( "0.5+0.5*sin(k*rho)+rho*rho", direct ), "direct compile: Finalize" );
	}
	Check( direct.IsValid(), "direct-compiled program is valid" );

	if( chunkParsed && direct.IsValid() ) {
		static const Scalar kGrid[][2] = {
			{ 0.0, 0.0 }, { 1.0, 1.0 }, { 0.5, 0.5 }, { 0.31, 0.17 },
			{ 0.05, 0.93 }, { 0.72, 0.24 }, { 1.0, 0.0 }, { 0.0, 1.0 }
		};
		for( size_t i = 0; i < sizeof(kGrid)/sizeof(kGrid[0]); ++i ) {
			const Scalar u = kGrid[i][0], v = kGrid[i][1];
			char label[96];
			snprintf( label, sizeof(label), "chunk-parsed == direct compile at (u=%.2f,v=%.2f)", (double)u, (double)v );
			CheckClose( chunkParsed->Evaluate( u, v ), direct.Eval( u, v ), Scalar(1e-12), label );
		}
	}
	job->release();
}

int main( int, char** )
{
	std::cout << "GuillocheChunkParseTest -- parse-level plumbing for the procedural chunks" << std::endl << std::endl;
	TestHappyPath();
	TestCartesianDiskValidation();
	TestRejections();
	TestProfileConveniencesAndClosedLoop();
	TestProfileConvenienceRealParserCoverage();
	TestFunction2DColorPainter();
	TestExpressionAndDisplacement();
	TestLatheChunk();
	TestSDFSuperellipsoidPartLines();
	TestUnifiedEngineEquivalence();
	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
