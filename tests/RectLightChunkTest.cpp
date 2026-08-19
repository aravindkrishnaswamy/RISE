//////////////////////////////////////////////////////////////////////
//
//  RectLightChunkTest.cpp - Contract test for the `rect_light` chunk:
//  the one-chunk PHYSICAL area light that the parser expands into the
//  canonical four-chunk chain (painter -> lambertian_luminaire_material
//  -> clippedplane_geometry -> standard_object).
//
//  WHY THE CHUNK EXISTS: docs/agentic-redesign/83-staged-construction-
//  plan.md sec 9 -- three prompt-side mechanism families each failed to
//  get an agent to author an area light, because the category of the
//  task summons the category of the chunk.  `rect_light` IS a
//  lighting-category chunk AND an area light, so the reach lands on it.
//
//  WHAT THIS TEST OWNS (the parser MATH and the parser CONTRACT; the
//  agent-surface half is AgentChunkCrudTest's):
//
//    1. EXPANSION.  A minimal scene using rect_light parses, and all
//       FOUR entities land in their managers under the documented
//       names: `<name>` (object), `<name>__pnt`, `<name>__mat`,
//       `<name>__geo`.
//
//    2. THE WINDING PROOF -- the one piece of real math here.  The
//       panel must emit toward `facing` and nowhere else.  Two facts
//       make that true and both are load-bearing:
//         * LambertianEmitter::emittedRadiance returns black when
//           Dot(out, N) <= 0 (LambertianEmitter.cpp) -- emission is
//           one-sided about the surface normal.
//         * ClippedPlaneGeometry takes that normal from the corner
//           winding: N = normalize(Cross(ptb - pta, ptd - pta)) for a
//           planar parallelogram (ClippedPlaneGeometry.cpp header block
//           + IntersectRay).
//       So the test asks the DERIVED GEOMETRY ITSELF for its normal
//       (UniformRandomPoint) over a spread of `facing` directions --
//       including both world-up degeneracies where the in-plane basis
//       falls back -- and requires it to equal `facing` exactly (to
//       within FP).  A sign slip anywhere in the corner math fails
//       here, not three layers downstream in a noisy render.
//
//    3. THE RENDER PROOF.  Winding is necessary but not sufficient:
//       `doublesided` decides whether the back face is hit at all.  So
//       the same panel over the same diffuse floor is rendered twice --
//       facing DOWN (floor lit) and facing UP (floor dark) -- through
//       the real integrator.  The asymmetry is the sidedness contract.
//
//    4. REJECTIONS.  Every required parameter missing; an undeclared
//       parameter; exitance <= 0; a degenerate (zero) `facing`; a
//       mis-arity / non-positive `size`; and a derived-name collision
//       on each of the four names.  All must FAIL the load, loudly.
//
//    5. ROUND-TRIP.  The scene language keeps the COMPACT form: a scene
//       file containing rect_light loads, derives, renders lit, and the
//       Job's retained CST Document serializes back byte-identically --
//       still containing `rect_light`.  This is what save does
//       (SaveEngine::Save is an unconditional whole-Document
//       Cst::SerializeCst; see SaveEngine.h), so the compact form is
//       what a save writes.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>			// getpid()
#endif

#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IGeometry.h"
#include "../src/Library/Interfaces/IGeometryManager.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/RISE_API.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const std::string& name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

namespace {

//////////////////////////////////////////////////////////////////////
// Scene plumbing
//////////////////////////////////////////////////////////////////////

std::string WriteTempScene( const std::string& tag, const std::string& body )
{
	const char* tmp = getenv( "TMPDIR" );
	std::string dir = tmp ? tmp : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pid[32];
	std::snprintf( pid, sizeof(pid), "%d", static_cast<int>( ::getpid() ) );
	const std::string path = dir + "rise_rect_light_" + tag + "_" + pid + ".RISEscene";
	std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
	f << body;
	f.close();
	return path;
}

//! Load `body` (a v7 scene WITHOUT its header line) into `job`.  Returns the
//! load verdict -- DeriveToJob refuses-all when a chunk's Finalize fails, so a
//! rejection path shows up here as `false`.
bool ParseBodyInto( const std::string& tag, const std::string& body, IJobPriv& job )
{
	const std::string path = WriteTempScene( tag, "RISE ASCII SCENE 7\n" + body );
	const bool ok = job.LoadAsciiSceneViaCst( path.c_str() );
	remove( path.c_str() );
	return ok;
}

bool ParseBody( const std::string& tag, const std::string& body )
{
	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) return false;
	const bool ok = ParseBodyInto( tag, body, *job );
	safe_release( job );
	return ok;
}

//////////////////////////////////////////////////////////////////////
// Render plumbing -- the same in-memory capture pattern
// AutoRasterizerTest / BDPTStrategyBalanceTest use (no file IO, no LDR
// encoding, so the test reads raw linear radiance).
//////////////////////////////////////////////////////////////////////

class CapturingRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
	unsigned int width;
	unsigned int height;

	CapturingRasterizerOutput() : width(0), height(0) {}

protected:
	virtual ~CapturingRasterizerOutput() {}

public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

	virtual void OutputImage( const IRasterImage& pImage, const Rect*, const unsigned int ) override
	{
		width  = pImage.GetWidth();
		height = pImage.GetHeight();
		pixels.resize( width * height );
		for( unsigned int y = 0; y < height; y++ )
			for( unsigned int x = 0; x < width; x++ )
				pixels[y * width + x] = pImage.GetPEL( x, y );
	}
};

//! Mean luminance-ish value over the frame (channel mean of base*coverage,
//! matching AutoRasterizerTest's composited-over-black convention).  -1 when
//! the render failed.
double RenderMeanRadiance( const std::string& tag, const std::string& body )
{
	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) return -1.0;
	if( !ParseBodyInto( tag, body, *job ) ) { safe_release( job ); return -1.0; }

	job->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "rect_light test capture output" );
	job->GetRasterizer()->AddRasterizerOutput( pCap );

	const bool rendered = job->Rasterize();
	double mean = -1.0;
	if( rendered && !pCap->pixels.empty() ) {
		double sum = 0.0;
		for( const RISEColor& c : pCap->pixels ) {
			const double cov = c.a;
			sum += ( c.base.r + c.base.g + c.base.b ) * cov / 3.0;
		}
		mean = sum / double( pCap->pixels.size() );
	}
	safe_release( pCap );
	safe_release( job );
	return mean;
}

//////////////////////////////////////////////////////////////////////
// Scene fragments
//////////////////////////////////////////////////////////////////////

//! A rect_light chunk with every parameter, overridable per test.
std::string RectLightChunk( const char* name,
                            const char* center,
                            const char* size,
                            const char* facing,
                            const char* color,
                            const char* exitance )
{
	std::string s = "rect_light\n{\n";
	if( name )     { s += "\tname\t\t"; s += name;     s += "\n"; }
	if( center )   { s += "\tcenter\t\t"; s += center;   s += "\n"; }
	if( size )     { s += "\tsize\t\t"; s += size;     s += "\n"; }
	if( facing )   { s += "\tfacing\t\t"; s += facing;   s += "\n"; }
	if( color )    { s += "\tcolor\t\t"; s += color;    s += "\n"; }
	if( exitance ) { s += "\texitance\t"; s += exitance; s += "\n"; }
	s += "}\n";
	return s;
}

//! A minimal renderable scene: a grey Lambertian floor at y = 0 seen from
//! above, plus whatever light text the caller supplies.  Nothing else emits,
//! and there is no environment, so the frame is BLACK unless the panel lights
//! the floor.
std::string FloorScene( const std::string& lightText, int samples )
{
	char buf[64];
	std::snprintf( buf, sizeof(buf), "%d", samples );
	// The shader is emitted BEFORE the rasterizer chunk: Job::Set*Rasterizer
	// resolves `defaultshader` (default "global") during the rasterizer chunk's
	// Finalize, so the named shader must already be parsed.
	return
		"standard_shader\n{\n\tname\t\tglobal\n\tshaderop\tDefaultPathTracing\n}\n"
		"film\n{\n\twidth\t\t48\n\theight\t\t48\n}\n"
		"pinhole_camera\n{\n"
		"\tlocation\t\t0 3.0 6.0\n"
		"\tlookat\t\t\t0 0 0\n"
		"\tup\t\t\t\t0 1 0\n"
		"\tfov\t\t\t\t50.0\n"
		"}\n"
		"uniformcolor_painter\n{\n\tname\t\tpnt_floor\n\tcolor\t\t0.7 0.7 0.7\n}\n"
		"lambertian_material\n{\n\tname\t\tfloor_mat\n\treflectance\tpnt_floor\n}\n"
		"clippedplane_geometry\n{\n"
		"\tname\t\tfloor_geo\n"
		"\tpta\t\t\t-6 0 -6\n"
		"\tptb\t\t\t 6 0 -6\n"
		"\tptc\t\t\t 6 0  6\n"
		"\tptd\t\t\t-6 0  6\n"
		"}\n"
		"standard_object\n{\n\tname\t\tfloor\n\tgeometry\tfloor_geo\n\tmaterial\tfloor_mat\n}\n"
		+ lightText +
		"pathtracing_pel_rasterizer\n{\n"
		"\tsamples\t\t\t" + buf + "\n"
		"\tpixel_filter\tbox\n"
		"\tmax_diffuse_bounce\t2\n"
		// Raw integrator radiance: the sidedness gate wants the emitter's own
		// energy, not a denoised estimate of it.
		"\toidn_denoise\tFALSE\n"
		"}\n";
}

//////////////////////////////////////////////////////////////////////
// 1 + 2 -- expansion and the winding proof
//////////////////////////////////////////////////////////////////////

void TestExpansionAndNames()
{
	std::cout << "Test: rect_light expands into four named entities" << std::endl;

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }

	const bool ok = ParseBodyInto( "expand",
		RectLightChunk( "window_light", "0 4 0", "2 1", "0 -1 0", "1.0 0.95 0.85", "6000" ), *job );
	Check( ok, "expand: a full rect_light chunk parses" );

	Check( job->GetObjects()   && job->GetObjects()->GetItem( "window_light" ) != 0,
	       "expand: the OBJECT is named `window_light` (the chunk's own name)" );
	Check( job->GetPainters()  && job->GetPainters()->GetItem( "window_light__pnt" ) != 0,
	       "expand: the painter is named `window_light__pnt`" );
	Check( job->GetMaterials() && job->GetMaterials()->GetItem( "window_light__mat" ) != 0,
	       "expand: the luminaire material is named `window_light__mat`" );
	Check( job->GetGeometries() && job->GetGeometries()->GetItem( "window_light__geo" ) != 0,
	       "expand: the quad geometry is named `window_light__geo`" );

	// The panel's AREA must be width * height -- if the corner math scaled by
	// the wrong factor the exitance-per-unit-area contract would silently
	// deliver the wrong amount of light.
	const IGeometry* geo = job->GetGeometries() ? job->GetGeometries()->GetItem( "window_light__geo" ) : 0;
	if( geo ) {
		const Scalar area = geo->GetArea();
		Check( fabs( area - 2.0 ) < 1e-9,
		       "expand: `size 2 1` gives a panel of area 2 (got " + std::to_string( double(area) ) + ")" );
	} else {
		Check( false, "expand: geometry retrievable for the area check" );
	}

	safe_release( job );
}

//! Ask the DERIVED geometry for its own normal and require it to equal
//! `facing`.  This is the winding proof: ClippedPlaneGeometry computes
//! N = normalize(Cross(dpdu, dpdv)) from the pta->ptb->ptc->ptd winding, and
//! LambertianEmitter emits only where Dot(out, N) > 0, so N == facing IS the
//! "emits toward facing" contract at the geometry layer.
void TestFacingBecomesTheNormal()
{
	std::cout << "Test: the derived quad's normal IS `facing` (winding proof)" << std::endl;

	struct Case { const char* text; double n[3]; const char* why; };
	static const Case kCases[] = {
		{ "0 -1 0",     {  0, -1,  0 }, "straight down (ceiling panel; world-up basis degenerate)" },
		{ "0 1 0",      {  0,  1,  0 }, "straight up (floor panel; the other degeneracy)" },
		{ "0 0 1",      {  0,  0,  1 }, "+Z" },
		{ "0 0 -1",     {  0,  0, -1 }, "-Z" },
		{ "1 0 0",      {  1,  0,  0 }, "+X" },
		{ "-1 0 0",     { -1,  0,  0 }, "-X" },
		{ "0 -3 0",     {  0, -1,  0 }, "non-unit down (normalisation)" },
		{ "1 -1 0",     {  0.70710678118654752, -0.70710678118654752, 0 }, "diagonal down-right" },
		{ "-2 -1 3",    { -0.53452248382484879, -0.26726124191242440, 0.80178372573727319 }, "oblique" }
	};

	for( std::size_t i = 0; i < sizeof(kCases)/sizeof(kCases[0]); ++i ) {
		IJobPriv* job = nullptr;
		if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }

		const std::string why = std::string( "facing " ) + kCases[i].text + " (" + kCases[i].why + ")";
		const bool ok = ParseBodyInto( "facing",
			RectLightChunk( "panel", "1 4 -2", "2 3", kCases[i].text, "1 1 1", "10" ), *job );
		Check( ok, "winding: parses -- " + why );

		const IGeometry* geo = ( ok && job->GetGeometries() )
			? job->GetGeometries()->GetItem( "panel__geo" ) : 0;
		if( geo ) {
			// Two (u, v) samples: for a planar parallelogram the normal is
			// constant, so both must agree with `facing`.
			bool allMatch = true;
			const Point3 prands[2] = { Point3( 0.25, 0.75, 0.0 ), Point3( 0.9, 0.1, 0.0 ) };
			for( int s = 0; s < 2; ++s ) {
				Point3 pt; Vector3 nrm; Point2 uv;
				geo->UniformRandomPoint( &pt, &nrm, &uv, prands[s] );
				for( int k = 0; k < 3; ++k ) {
					const double want = kCases[i].n[k];
					const double got  = ( k == 0 ? nrm.x : ( k == 1 ? nrm.y : nrm.z ) );
					if( fabs( want - got ) > 1e-9 ) allMatch = false;
				}
			}
			Check( allMatch, "winding: the quad's normal equals " + why );
		} else {
			Check( false, "winding: geometry retrievable -- " + why );
		}
		safe_release( job );
	}
}

//////////////////////////////////////////////////////////////////////
// 3 -- the render proof
//////////////////////////////////////////////////////////////////////

void TestSidednessRenders()
{
	std::cout << "Test: a rect_light facing DOWN lights the floor; facing UP it does not" << std::endl;

	const std::string down = FloorScene(
		RectLightChunk( "panel", "0 4 0", "2 2", "0 -1 0", "1 1 1", "40" ), 24 );
	const std::string up = FloorScene(
		RectLightChunk( "panel", "0 4 0", "2 2", "0 1 0", "1 1 1", "40" ), 24 );

	const double meanDown = RenderMeanRadiance( "down", down );
	const double meanUp   = RenderMeanRadiance( "up",   up );

	std::cout << "    facing 0 -1 0 (down) mean = " << meanDown << std::endl;
	std::cout << "    facing 0  1 0 (up)   mean = " << meanUp   << std::endl;

	Check( meanDown >= 0.0 && meanUp >= 0.0, "sidedness: both renders completed" );
	Check( meanDown > 1e-3, "sidedness: facing DOWN, the floor is lit" );
	// Facing up, the panel's emitting face points away from everything: the
	// floor gets nothing, and the camera (above the floor, below the panel)
	// sees the panel's DARK back face.  Some pixels can still carry the panel's
	// non-emissive black silhouette, so the gate is "essentially black", and
	// the ratio gate is what makes it a SIDEDNESS statement rather than a
	// brightness one.
	Check( meanUp < 1e-4, "sidedness: facing UP, the frame is essentially black" );
	Check( meanDown > 100.0 * ( meanUp > 0.0 ? meanUp : 1e-9 ) || meanUp <= 0.0,
	       "sidedness: down is >100x up (the two orientations are not the same light)" );
}

//////////////////////////////////////////////////////////////////////
// 4 -- rejections
//////////////////////////////////////////////////////////////////////

void TestRejections()
{
	std::cout << "Test: rect_light rejection paths" << std::endl;

	// Baseline: the full chunk parses, so every rejection below is attributable
	// to the one thing it changed.
	Check( ParseBody( "ok", RectLightChunk( "L", "0 4 0", "2 1", "0 -1 0", "1 1 1", "6000" ) ),
	       "reject-baseline: the complete chunk parses" );

	// `color` is the only optional parameter.
	Check( ParseBody( "nocolor", RectLightChunk( "L", "0 4 0", "2 1", "0 -1 0", 0, "6000" ) ),
	       "reject: `color` is optional (defaults to 1 1 1)" );

	// Missing required parameters.
	Check( !ParseBody( "noname",   RectLightChunk( 0,   "0 4 0", "2 1", "0 -1 0", "1 1 1", "6000" ) ),
	       "reject: missing `name`" );
	Check( !ParseBody( "nocenter", RectLightChunk( "L", 0,       "2 1", "0 -1 0", "1 1 1", "6000" ) ),
	       "reject: missing `center`" );
	Check( !ParseBody( "nosize",   RectLightChunk( "L", "0 4 0", 0,     "0 -1 0", "1 1 1", "6000" ) ),
	       "reject: missing `size`" );
	Check( !ParseBody( "nofacing", RectLightChunk( "L", "0 4 0", "2 1", 0,        "1 1 1", "6000" ) ),
	       "reject: missing `facing`" );
	Check( !ParseBody( "noexit",   RectLightChunk( "L", "0 4 0", "2 1", "0 -1 0", "1 1 1", 0 ) ),
	       "reject: missing `exitance`" );

	// An undeclared parameter -- the descriptor IS the accepted set, so the
	// framework rejects this without the parser doing anything.  `power` is the
	// specific one to pin: the owner's decision is EXITANCE ONLY, so a scene
	// that writes `power` must fail rather than silently ignore it.
	Check( !ParseBody( "power",
		"rect_light\n{\n\tname\t\tL\n\tcenter\t\t0 4 0\n\tsize\t\t2 1\n"
		"\tfacing\t\t0 -1 0\n\texitance\t6000\n\tpower\t\t3.14\n}\n" ),
	       "reject: `power` is NOT a rect_light parameter (exitance only)" );
	Check( !ParseBody( "bogus",
		"rect_light\n{\n\tname\t\tL\n\tcenter\t\t0 4 0\n\tsize\t\t2 1\n"
		"\tfacing\t\t0 -1 0\n\texitance\t6000\n\tdoublesided\tTRUE\n}\n" ),
	       "reject: an undeclared parameter (`doublesided`)" );

	// exitance must be strictly positive.
	Check( !ParseBody( "exit0",   RectLightChunk( "L", "0 4 0", "2 1", "0 -1 0", "1 1 1", "0" ) ),
	       "reject: `exitance 0`" );
	Check( !ParseBody( "exitneg", RectLightChunk( "L", "0 4 0", "2 1", "0 -1 0", "1 1 1", "-5" ) ),
	       "reject: a negative `exitance`" );

	// Degenerate facing -- must fail loudly, not produce NaN corners.
	Check( !ParseBody( "facing0", RectLightChunk( "L", "0 4 0", "2 1", "0 0 0", "1 1 1", "6000" ) ),
	       "reject: a zero-length `facing`" );

	// size arity and positivity.
	Check( !ParseBody( "size1",   RectLightChunk( "L", "0 4 0", "2",     "0 -1 0", "1 1 1", "6000" ) ),
	       "reject: `size` with one number" );
	Check( !ParseBody( "size3",   RectLightChunk( "L", "0 4 0", "2 1 3", "0 -1 0", "1 1 1", "6000" ) ),
	       "reject: `size` with three numbers" );
	Check( !ParseBody( "size0",   RectLightChunk( "L", "0 4 0", "0 1",   "0 -1 0", "1 1 1", "6000" ) ),
	       "reject: a zero `size` component" );
	Check( !ParseBody( "sizeneg", RectLightChunk( "L", "0 4 0", "2 -1",  "0 -1 0", "1 1 1", "6000" ) ),
	       "reject: a negative `size` component" );

	// Derived-name collisions -- one per generated entity, each of which must
	// fail the load rather than silently win or lose.
	const std::string light = RectLightChunk( "L", "0 4 0", "2 1", "0 -1 0", "1 1 1", "6000" );
	Check( !ParseBody( "clash_pnt",
		"uniformcolor_painter\n{\n\tname\t\tL__pnt\n\tcolor\t\t1 0 0\n}\n" + light ),
	       "reject: an existing painter already named `L__pnt`" );
	Check( !ParseBody( "clash_mat",
		"uniformcolor_painter\n{\n\tname\t\tp\n\tcolor\t\t1 0 0\n}\n"
		"lambertian_material\n{\n\tname\t\tL__mat\n\treflectance\tp\n}\n" + light ),
	       "reject: an existing material already named `L__mat`" );
	Check( !ParseBody( "clash_geo",
		"sphere_geometry\n{\n\tname\t\tL__geo\n\tradius\t\t1\n}\n" + light ),
	       "reject: an existing geometry already named `L__geo`" );
	Check( !ParseBody( "clash_obj",
		"sphere_geometry\n{\n\tname\t\tg\n\tradius\t\t1\n}\n"
		"standard_object\n{\n\tname\t\tL\n\tgeometry\tg\n}\n" + light ),
	       "reject: an existing object already named `L`" );
	// Two rect_lights sharing a name collide on all four -- the ordinary
	// duplicate-name error, never a silent overwrite.
	Check( !ParseBody( "clash_self", light + light ),
	       "reject: two rect_lights with the same name" );
}

//////////////////////////////////////////////////////////////////////
// 5 -- round-trip
//////////////////////////////////////////////////////////////////////

void TestRoundTrip()
{
	std::cout << "Test: a rect_light scene round-trips as the COMPACT form" << std::endl;

	const std::string body = FloorScene(
		RectLightChunk( "window_light", "0 4 0", "2 1", "0 -1 0", "1.0 0.95 0.85", "300" ), 16 );
	const std::string text = "RISE ASCII SCENE 7\n" + body;
	const std::string path = WriteTempScene( "roundtrip", text );

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); remove( path.c_str() ); return; }

	const bool loaded = job->LoadAsciiSceneViaCst( path.c_str() );
	Check( loaded, "roundtrip: the scene file loads" );
	Check( job->GetObjects() && job->GetObjects()->GetItem( "window_light" ) != 0,
	       "roundtrip: the rect_light derived into an object" );

	// SaveEngine::Save is an unconditional whole-Document Cst::SerializeCst on
	// the Job's retained CST Document (SaveEngine.h), so serializing that
	// Document here IS what a save writes to disk.
	Check( job->HasRetainedCstDocument(), "roundtrip: the Job retains a CST Document (what save serializes)" );
	const RISE::Cst::Document* doc = job->GetCstDocument();
	if( doc ) {
		const std::string saved = RISE::Cst::SerializeCst( *doc );
		Check( saved.find( "rect_light" ) != std::string::npos,
		       "roundtrip: the saved text still contains `rect_light` (the compact form persists)" );
		Check( saved.find( "lambertian_luminaire_material" ) == std::string::npos,
		       "roundtrip: the save does NOT write out the expanded four-chunk chain" );
		Check( saved == text, "roundtrip: an unedited save is byte-identical to the source file" );
	} else {
		Check( false, "roundtrip: CST Document retrievable" );
	}

	safe_release( job );
	remove( path.c_str() );

	// And the same scene actually renders LIT -- a document that round-trips
	// but does not light anything would pass every check above.
	const double mean = RenderMeanRadiance( "roundtrip_render", body );
	std::cout << "    round-trip scene mean = " << mean << std::endl;
	Check( mean > 1e-3, "roundtrip: the scene renders lit" );
}

} // anonymous namespace

int main()
{
	std::cout << "=== RectLightChunkTest ===" << std::endl;

	TestExpansionAndNames();
	TestFacingBecomesTheNormal();
	TestRejections();
	TestSidednessRenders();
	TestRoundTrip();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << "  Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
