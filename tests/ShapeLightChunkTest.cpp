//////////////////////////////////////////////////////////////////////
//
//  ShapeLightChunkTest.cpp - Contract test for the `shape_light`
//  chunk: the one-chunk PHYSICAL area light for a CLOSED SOLID (a
//  sphere, an ellipsoid, a box or a cylinder), which the parser expands
//  into the canonical four-chunk chain (painter ->
//  lambertian_luminaire_material -> the shape's geometry ->
//  standard_object).
//
//  WHY THE CHUNK EXISTS: docs/agentic-redesign/83-staged-construction-
//  plan.md sec 11 -- `rect_light` closed the one-chunk gap for a PANEL,
//  and the first live run using it still turned every glowing creature
//  into an omni_light, because a jellyfish is not a rectangle and the
//  only one-chunk light with real area was.  `shape_light` is the same
//  mechanism for the shapes a glowing THING actually is.
//
//  A SEPARATE BINARY FROM RectLightChunkTest, deliberately.  The two
//  chunks are siblings but not variants: rect_light's whole hard part is
//  the corner winding that makes its ONE-SIDED emitter face `facing`,
//  and shape_light's whole point is that a closed solid needs no such
//  parameter.  Folding this into that file would bury the difference in
//  a shared fixture; kept apart, each file's math section IS the thing
//  its chunk had to get right.  (`make tests` discovers test sources by
//  wildcard, so a new file needs no build-system edit.)
//
//  WHAT THIS TEST OWNS (the parser MATH and the parser CONTRACT; the
//  agent-surface half is AgentChunkCrudTest's):
//
//    1. EXPANSION, PER SHAPE.  Each of the four shapes parses, all FOUR
//       entities land in their managers under the documented derived
//       names, and the derived geometry has the AREA the `size`
//       semantics promise -- a closed-form check per shape, so a
//       swapped argument (radius for height, width for depth) fails
//       here rather than as a mysteriously dim render.
//
//    2. THE OUTWARD-NORMAL PROOF -- the physics claim the descriptor
//       makes, and the reason there is no `facing`.  LambertianEmitter
//       returns black when Dot(out, N) <= 0, so "emits outward
//       everywhere and inward nowhere" is exactly "the surface normal
//       points away from the centre at every point".  The test asks the
//       DERIVED GEOMETRY ITSELF for normals at many sample points and
//       requires Dot(N, P) > 0 for every one (every shape is built
//       centred on its own origin, so P is the outward direction).  A
//       geometry that ever answered with an inward normal would emit
//       into its own interior and be dark on the outside there.
//
//    3. THE RENDER PROOF.  A sphere shape_light above a diffuse floor,
//       through the real integrator: the floor is LIT, and the light is
//       VISIBLE IN FRAME as an object (arc 81's visibility fact, which
//       for this chunk holds by construction -- it is an ordinary
//       standard_object).  Both are measured, not asserted: the frame
//       mean is compared against the same scene with the light removed,
//       and the emitter's own pixels are found by rendering it against
//       a floor-free scene where nothing else can contribute.
//
//    4. REJECTIONS.  Every required parameter missing; an undeclared
//       parameter (`power`, and `facing` -- the one a rect_light author
//       would reach for); exitance <= 0; an unknown `shape` value; the
//       wrong `size` arity for each shape; a non-positive `size`
//       component; and a derived-name collision on each of the four
//       names.  All must FAIL the load, loudly.
//
//    5. ROUND-TRIP.  A scene file containing shape_light loads, derives,
//       renders lit, and the Job's retained CST Document serializes back
//       byte-identically -- still containing `shape_light` and NOT the
//       expanded chain.  This is what save does (SaveEngine::Save is an
//       unconditional whole-Document Cst::SerializeCst).
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
// Scene plumbing -- RectLightChunkTest's, unchanged, so a difference
// between the two results is a difference between the two CHUNKS.
//////////////////////////////////////////////////////////////////////

std::string WriteTempScene( const std::string& tag, const std::string& body )
{
	const char* tmp = getenv( "TMPDIR" );
	std::string dir = tmp ? tmp : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pid[32];
	std::snprintf( pid, sizeof(pid), "%d", static_cast<int>( ::getpid() ) );
	const std::string path = dir + "rise_shape_light_" + tag + "_" + pid + ".RISEscene";
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
}
;

//! What a render produced: the frame's mean channel value (composited over
//! black, AutoRasterizerTest's convention) and how many pixels carry a value
//! above `brightThreshold`.  `ok` false means the render did not run.
struct RenderStats
{
	bool   ok           = false;
	double mean         = 0.0;
	int    brightPixels = 0;
	int    pixelCount   = 0;
};

RenderStats RenderScene( const std::string& tag, const std::string& body,
                         double brightThreshold )
{
	RenderStats st;
	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) return st;
	if( !ParseBodyInto( tag, body, *job ) ) { safe_release( job ); return st; }

	job->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "shape_light test capture output" );
	job->GetRasterizer()->AddRasterizerOutput( pCap );

	if( job->Rasterize() && !pCap->pixels.empty() ) {
		double sum = 0.0;
		for( const RISEColor& c : pCap->pixels ) {
			const double cov = c.a;
			const double v   = ( c.base.r + c.base.g + c.base.b ) * cov / 3.0;
			sum += v;
			if( v > brightThreshold ) ++st.brightPixels;
		}
		st.pixelCount = static_cast<int>( pCap->pixels.size() );
		st.mean       = sum / double( st.pixelCount );
		st.ok         = true;
	}
	safe_release( pCap );
	safe_release( job );
	return st;
}

//////////////////////////////////////////////////////////////////////
// Scene fragments
//////////////////////////////////////////////////////////////////////

//! A shape_light chunk; any parameter may be omitted by passing null.
std::string ShapeLightChunk( const char* name,
                             const char* shape,
                             const char* center,
                             const char* size,
                             const char* exitance,
                             const char* color       = "1 1 1",
                             const char* orientation = 0 )
{
	std::string s = "shape_light\n{\n";
	if( name )        { s += "\tname\t\t";        s += name;        s += "\n"; }
	if( shape )       { s += "\tshape\t\t";       s += shape;       s += "\n"; }
	if( center )      { s += "\tcenter\t\t";      s += center;      s += "\n"; }
	if( size )        { s += "\tsize\t\t";        s += size;        s += "\n"; }
	if( orientation ) { s += "\torientation\t";   s += orientation; s += "\n"; }
	if( color )       { s += "\tcolor\t\t";       s += color;       s += "\n"; }
	if( exitance )    { s += "\texitance\t";      s += exitance;    s += "\n"; }
	s += "}\n";
	return s;
}

//! Camera, film, shader and integrator -- everything but the floor and the
//! light, so the two render fixtures below differ ONLY in what they contain.
std::string SceneShell( int samples )
{
	char buf[64];
	std::snprintf( buf, sizeof(buf), "%d", samples );
	// The shader is emitted BEFORE the rasterizer chunk: Job::Set*Rasterizer
	// resolves `defaultshader` during the rasterizer chunk's Finalize, so the
	// named shader must already be parsed.
	return
		"standard_shader\n{\n\tname\t\tglobal\n\tshaderop\tDefaultPathTracing\n}\n"
		"film\n{\n\twidth\t\t48\n\theight\t\t48\n}\n"
		"pinhole_camera\n{\n"
		"\tlocation\t\t0 3.0 6.0\n"
		"\tlookat\t\t\t0 1.5 0\n"
		"\tup\t\t\t\t0 1 0\n"
		"\tfov\t\t\t\t50.0\n"
		"}\n"
		"pathtracing_pel_rasterizer\n{\n"
		"\tsamples\t\t\t" + std::string( buf ) + "\n"
		"\tpixel_filter\tbox\n"
		"\tmax_diffuse_bounce\t2\n"
		// Raw integrator radiance: this test wants the emitter's own energy,
		// not a denoised estimate of it.
		"\toidn_denoise\tFALSE\n"
		"}\n";
}

std::string FloorChunks()
{
	return
		"uniformcolor_painter\n{\n\tname\t\tpnt_floor\n\tcolor\t\t0.7 0.7 0.7\n}\n"
		"lambertian_material\n{\n\tname\t\tfloor_mat\n\treflectance\tpnt_floor\n}\n"
		"clippedplane_geometry\n{\n"
		"\tname\t\tfloor_geo\n"
		"\tpta\t\t\t-6 0 -6\n"
		"\tptb\t\t\t 6 0 -6\n"
		"\tptc\t\t\t 6 0  6\n"
		"\tptd\t\t\t-6 0  6\n"
		"}\n"
		"standard_object\n{\n\tname\t\tfloor\n\tgeometry\tfloor_geo\n\tmaterial\tfloor_mat\n}\n";
}

//////////////////////////////////////////////////////////////////////
// 1 -- expansion, per shape, with the size semantics checked as AREA
//////////////////////////////////////////////////////////////////////

void TestExpansionPerShape()
{
	std::cout << "Test: each shape expands into four named entities with the promised size" << std::endl;

	// `wantArea` is the closed-form surface area the documented `size`
	// semantics imply.  It is what makes this a check on the SEMANTICS rather
	// than on "some geometry got made": swap radius and height on the cylinder,
	// or width and depth on a non-cubic box, and the number moves.
	struct Case { const char* shape; const char* size; double wantArea; const char* why; };
	static const Case kCases[] = {
		{ "sphere",    "2",        4.0 * PI * 2.0 * 2.0,
		  "one radius -> 4*pi*r^2" },
		{ "box",       "2 3 4",    2.0 * ( 2.0*3.0 + 2.0*4.0 + 3.0*4.0 ),
		  "width height depth -> 2(wh + wd + hd)" },
		{ "cylinder",  "2 5",      2.0 * PI * 2.0 * 5.0 + 2.0 * PI * 2.0 * 2.0,
		  "radius height, CAPPED -> 2*pi*r*h + 2*pi*r^2" }
		// The ellipsoid's area has no elementary closed form (RISE uses a
		// Knud-Thomsen approximation), so its `size` semantics are checked
		// through the bounding box in TestEllipsoidRadii below instead of here.
	};

	for( std::size_t i = 0; i < sizeof(kCases)/sizeof(kCases[0]); ++i ) {
		IJobPriv* job = nullptr;
		if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }

		const std::string why = std::string( kCases[i].shape ) + " (" + kCases[i].why + ")";
		const bool ok = ParseBodyInto( "expand",
			ShapeLightChunk( "lamp", kCases[i].shape, "1 2 -3", kCases[i].size, "40" ), *job );
		Check( ok, "expand: a full shape_light parses -- " + why );

		Check( job->GetObjects()    && job->GetObjects()->GetItem( "lamp" ) != 0,
		       "expand: the OBJECT is named `lamp` (the chunk's own name) -- " + why );
		Check( job->GetPainters()   && job->GetPainters()->GetItem( "lamp__pnt" ) != 0,
		       "expand: the painter is named `lamp__pnt` -- " + why );
		Check( job->GetMaterials()  && job->GetMaterials()->GetItem( "lamp__mat" ) != 0,
		       "expand: the luminaire material is named `lamp__mat` -- " + why );
		Check( job->GetGeometries() && job->GetGeometries()->GetItem( "lamp__geo" ) != 0,
		       "expand: the solid geometry is named `lamp__geo` -- " + why );

		const IGeometry* geo = job->GetGeometries() ? job->GetGeometries()->GetItem( "lamp__geo" ) : 0;
		if( geo ) {
			const double area = double( geo->GetArea() );
			Check( fabs( area - kCases[i].wantArea ) < 1e-6 * kCases[i].wantArea,
			       "expand: `size " + std::string( kCases[i].size ) + "` gives area " +
			       std::to_string( kCases[i].wantArea ) + " -- " + why +
			       " (got " + std::to_string( area ) + ")" );
		} else {
			Check( false, "expand: geometry retrievable -- " + why );
		}
		safe_release( job );
	}
}

//! The ellipsoid's three numbers are its SEMI-AXES, in X Y Z order.  Checked
//! through the derived geometry's own bounding box, which is exact, rather
//! than through its area, which is an approximation.
void TestEllipsoidRadii()
{
	std::cout << "Test: `shape ellipsoid` reads `size` as three semi-axis radii, X Y Z" << std::endl;

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }

	const bool ok = ParseBodyInto( "ellipsoid",
		ShapeLightChunk( "orb", "ellipsoid", "0 0 0", "1 2 3", "40" ), *job );
	Check( ok, "ellipsoid: parses" );

	const IGeometry* geo = ( ok && job->GetGeometries() )
		? job->GetGeometries()->GetItem( "orb__geo" ) : 0;
	if( geo ) {
		const BoundingBox bb = geo->GenerateBoundingBox();
		Check( fabs( bb.ur.x - 1.0 ) < 1e-9 && fabs( bb.ll.x + 1.0 ) < 1e-9 &&
		       fabs( bb.ur.y - 2.0 ) < 1e-9 && fabs( bb.ll.y + 2.0 ) < 1e-9 &&
		       fabs( bb.ur.z - 3.0 ) < 1e-9 && fabs( bb.ll.z + 3.0 ) < 1e-9,
		       "ellipsoid: `size 1 2 3` gives semi-axes 1, 2, 3 on X, Y, Z -- an ordering "
		       "slip would put the long axis on the wrong world axis" );
	} else {
		Check( false, "ellipsoid: geometry retrievable" );
	}
	safe_release( job );
}

//////////////////////////////////////////////////////////////////////
// 2 -- the outward-normal proof (why there is no `facing`)
//////////////////////////////////////////////////////////////////////

//! Every shape must answer with a normal pointing AWAY from its centre at
//! every sampled point.  Composed with LambertianEmitter's one-sidedness
//! (black when Dot(out, N) <= 0) that IS the descriptor's claim: emits
//! outward everywhere, inward nowhere, no `facing` needed.
void TestNormalsPointOutward()
{
	std::cout << "Test: every shape's surface normal points OUTWARD (the no-facing proof)" << std::endl;

	struct Case { const char* shape; const char* size; };
	static const Case kCases[] = {
		{ "sphere",    "1.3"       },
		{ "ellipsoid", "0.7 2.1 1" },
		{ "box",       "2 3 4"     },
		{ "cylinder",  "1.5 4"     }
	};

	for( std::size_t i = 0; i < sizeof(kCases)/sizeof(kCases[0]); ++i ) {
		IJobPriv* job = nullptr;
		if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }

		const std::string shape = kCases[i].shape;
		const bool ok = ParseBodyInto( "normals",
			ShapeLightChunk( "s", shape.c_str(), "0 0 0", kCases[i].size, "10" ), *job );
		Check( ok, "outward: `shape " + shape + "` parses" );

		const IGeometry* geo = ( ok && job->GetGeometries() )
			? job->GetGeometries()->GetItem( "s__geo" ) : 0;
		if( !geo ) { Check( false, "outward: geometry retrievable -- " + shape ); safe_release( job ); continue; }

		// A deterministic sweep of the sample cube.  The cylinder's cap/wall
		// split and the box's face selection both read prand.z, so the sweep
		// walks all three axes rather than a plane -- 512 points, which hits
		// every face of the box and both caps of the cylinder many times over.
		bool allOutward = true;
		double worstDot = 1e300;
		for( int a = 0; a < 8 && allOutward; ++a )
		for( int b = 0; b < 8; ++b )
		for( int c = 0; c < 8; ++c ) {
			const Point3 prand( ( a + 0.5 ) / 8.0, ( b + 0.5 ) / 8.0, ( c + 0.5 ) / 8.0 );
			Point3 pt; Vector3 nrm; Point2 uv;
			geo->UniformRandomPoint( &pt, &nrm, &uv, prand );
			// The geometry is built centred on its own origin, so the vector
			// from the centre to the point IS the outward direction there.
			const Vector3 outward( pt.x, pt.y, pt.z );
			const double d = double( Vector3Ops::Dot( nrm, outward ) );
			if( d < worstDot ) worstDot = d;
			if( !( d > 0.0 ) ) allOutward = false;
		}
		Check( allOutward,
		       "outward: MONEY ASSERTION -- every sampled normal on `shape " + shape +
		       "` points away from the centre, so the solid emits outward everywhere and "
		       "inward nowhere; this is why the chunk has no `facing` (worst Dot(N, P) = " +
		       std::to_string( worstDot ) + ")" );
		safe_release( job );
	}
}

//////////////////////////////////////////////////////////////////////
// 3 -- the render proof: it lights the floor, and it is IN the frame
//////////////////////////////////////////////////////////////////////

void TestSphereLightsTheFloorAndIsVisible()
{
	std::cout << "Test: a sphere shape_light lights a floor, and is visible in frame" << std::endl;

	const std::string light =
		ShapeLightChunk( "bulb", "sphere", "0 3 0", "0.4", "300" );

	// (a) FLOOR + LIGHT vs FLOOR ALONE.  Nothing else emits and there is no
	//     environment, so the difference is entirely this chunk's doing.
	const RenderStats lit  = RenderScene( "lit",  SceneShell( 32 ) + FloorChunks() + light, 0.5 );
	const RenderStats dark = RenderScene( "dark", SceneShell( 32 ) + FloorChunks(),         0.5 );

	std::cout << "    floor + shape_light mean = " << lit.mean
	          << "   floor alone mean = " << dark.mean << std::endl;

	Check( lit.ok && dark.ok, "render: both renders completed" );
	Check( dark.mean <= 1e-9,
	       "render: the control frame (floor, no light) is black -- so the comparison below "
	       "is attributable to the shape_light and to nothing else" );
	Check( lit.mean > 1e-3,
	       "render: MONEY ASSERTION -- a sphere shape_light above the floor LIGHTS IT "
	       "(mean " + std::to_string( lit.mean ) + "). A closed solid needs no facing, and "
	       "this is the end-to-end proof that its outward emission reaches the scene" );

	// (b) THE LIGHT IS A VISIBLE OBJECT.  Arc 81's fact -- an emissive object is
	//     rendered like any other object, so the camera sees it -- which for
	//     this chunk holds BY CONSTRUCTION (it expands into a standard_object).
	//     Measured on a scene with NO floor, so the only thing that can put
	//     energy in the frame is the emitter's own surface.
	const RenderStats aloneLight = RenderScene( "alone", SceneShell( 8 ) + light, 1.0 );
	const RenderStats aloneEmpty = RenderScene( "empty", SceneShell( 8 ),        1.0 );

	std::cout << "    emitter alone: bright pixels = " << aloneLight.brightPixels
	          << " of " << aloneLight.pixelCount
	          << "   empty scene: " << aloneEmpty.brightPixels << std::endl;

	Check( aloneLight.ok && aloneEmpty.ok, "render: both visibility renders completed" );
	Check( aloneEmpty.brightPixels == 0,
	       "render: an empty scene has no bright pixels (the visibility control)" );
	Check( aloneLight.brightPixels > 0,
	       "render: MONEY ASSERTION -- the shape_light's own surface is IN THE FRAME (" +
	       std::to_string( aloneLight.brightPixels ) + " bright pixels with nothing else in "
	       "the scene). It is both a light and a thing the picture shows, which is the fact "
	       "arc 82 sec 8 recorded a model getting wrong about area lights" );
}

//////////////////////////////////////////////////////////////////////
// 4 -- rejections
//////////////////////////////////////////////////////////////////////

void TestRejections()
{
	std::cout << "Test: shape_light rejection paths" << std::endl;

	// Baseline: the complete chunk parses, so every rejection below is
	// attributable to the one thing it changed.
	Check( ParseBody( "ok", ShapeLightChunk( "L", "sphere", "0 4 0", "1", "600" ) ),
	       "reject-baseline: the complete chunk parses" );

	// The two optional parameters.
	Check( ParseBody( "nocolor", ShapeLightChunk( "L", "sphere", "0 4 0", "1", "600", 0 ) ),
	       "reject: `color` is optional (defaults to 1 1 1)" );
	Check( ParseBody( "orient",
		ShapeLightChunk( "L", "cylinder", "0 4 0", "1 3", "600", "1 1 1", "0 0 90" ) ),
	       "reject: `orientation` is optional and accepted (Euler degrees)" );

	// Missing required parameters.
	Check( !ParseBody( "noname",   ShapeLightChunk( 0,   "sphere", "0 4 0", "1", "600" ) ),
	       "reject: missing `name`" );
	Check( !ParseBody( "noshape",  ShapeLightChunk( "L", 0,        "0 4 0", "1", "600" ) ),
	       "reject: missing `shape`" );
	Check( !ParseBody( "nocenter", ShapeLightChunk( "L", "sphere", 0,       "1", "600" ) ),
	       "reject: missing `center`" );
	Check( !ParseBody( "nosize",   ShapeLightChunk( "L", "sphere", "0 4 0", 0,   "600" ) ),
	       "reject: missing `size`" );
	Check( !ParseBody( "noexit",   ShapeLightChunk( "L", "sphere", "0 4 0", "1", 0 ) ),
	       "reject: missing `exitance`" );

	// An UNKNOWN SHAPE must fail loudly rather than fall back to something.
	Check( !ParseBody( "badshape", ShapeLightChunk( "L", "torus",  "0 4 0", "1 1", "600" ) ),
	       "reject: MONEY ASSERTION -- an unknown `shape` (torus) fails the load; silently "
	       "picking a default would give an author a light of a shape they did not ask for" );
	Check( !ParseBody( "capshape", ShapeLightChunk( "L", "Sphere", "0 4 0", "1", "600" ) ),
	       "reject: `shape` is matched exactly -- `Sphere` is not `sphere`" );

	// Undeclared parameters.  `power` is the owner's exitance-only decision;
	// `facing` is the one a rect_light author would reach for, and a
	// shape_light that silently ignored it would be a lie about its physics.
	Check( !ParseBody( "power",
		"shape_light\n{\n\tname\t\tL\n\tshape\t\tsphere\n\tcenter\t\t0 4 0\n"
		"\tsize\t\t1\n\texitance\t600\n\tpower\t\t3.14\n}\n" ),
	       "reject: `power` is NOT a shape_light parameter (exitance only)" );
	Check( !ParseBody( "facing",
		"shape_light\n{\n\tname\t\tL\n\tshape\t\tsphere\n\tcenter\t\t0 4 0\n"
		"\tsize\t\t1\n\texitance\t600\n\tfacing\t\t0 -1 0\n}\n" ),
	       "reject: MONEY ASSERTION -- `facing` is NOT a shape_light parameter. A closed "
	       "solid emits outward everywhere, so accepting and ignoring the line would teach "
	       "an author that this chunk has a sidedness it does not have" );

	// exitance must be strictly positive.
	Check( !ParseBody( "exit0",   ShapeLightChunk( "L", "sphere", "0 4 0", "1", "0" ) ),
	       "reject: `exitance 0`" );
	Check( !ParseBody( "exitneg", ShapeLightChunk( "L", "sphere", "0 4 0", "1", "-5" ) ),
	       "reject: a negative `exitance`" );

	// SIZE ARITY, per shape -- one wrong-arity case on each side where the
	// shape has a neighbour that would accept it.
	Check( !ParseBody( "sph2",  ShapeLightChunk( "L", "sphere",    "0 4 0", "1 2",   "600" ) ),
	       "reject: `shape sphere` with two numbers in `size`" );
	Check( !ParseBody( "ell2",  ShapeLightChunk( "L", "ellipsoid", "0 4 0", "1 2",   "600" ) ),
	       "reject: `shape ellipsoid` with two numbers in `size`" );
	Check( !ParseBody( "ell4",  ShapeLightChunk( "L", "ellipsoid", "0 4 0", "1 2 3 4", "600" ) ),
	       "reject: `shape ellipsoid` with four numbers in `size`" );
	Check( !ParseBody( "box2",  ShapeLightChunk( "L", "box",       "0 4 0", "1 2",   "600" ) ),
	       "reject: `shape box` with two numbers in `size`" );
	Check( !ParseBody( "cyl1",  ShapeLightChunk( "L", "cylinder",  "0 4 0", "1",     "600" ) ),
	       "reject: `shape cylinder` with one number in `size`" );
	Check( !ParseBody( "cyl3",  ShapeLightChunk( "L", "cylinder",  "0 4 0", "1 2 3", "600" ) ),
	       "reject: `shape cylinder` with three numbers in `size`" );

	// SIZE POSITIVITY, including a zero buried in the middle of a triple --
	// checking only the first number would let that one through.
	Check( !ParseBody( "size0",   ShapeLightChunk( "L", "sphere", "0 4 0", "0",     "600" ) ),
	       "reject: a zero `size` on a sphere" );
	Check( !ParseBody( "sizeneg", ShapeLightChunk( "L", "sphere", "0 4 0", "-1",    "600" ) ),
	       "reject: a negative `size` on a sphere" );
	Check( !ParseBody( "boxmid0", ShapeLightChunk( "L", "box",    "0 4 0", "1 0 3", "600" ) ),
	       "reject: a zero in the MIDDLE of a box `size` -- every component is checked" );
	Check( !ParseBody( "cylneg",  ShapeLightChunk( "L", "cylinder", "0 4 0", "1 -3", "600" ) ),
	       "reject: a negative height on a cylinder `size`" );

	// Derived-name collisions -- one per generated entity, each of which must
	// fail the load rather than silently win or lose.
	const std::string light = ShapeLightChunk( "L", "sphere", "0 4 0", "1", "600" );
	Check( !ParseBody( "clash_pnt",
		"uniformcolor_painter\n{\n\tname\t\tL__pnt\n\tcolor\t\t1 0 0\n}\n" + light ),
	       "reject: an existing painter already named `L__pnt`" );
	Check( !ParseBody( "clash_mat",
		"uniformcolor_painter\n{\n\tname\t\tp\n\tcolor\t\t1 0 0\n}\n"
		"lambertian_material\n{\n\tname\t\tL__mat\n\treflectance\tp\n}\n" + light ),
	       "reject: an existing material already named `L__mat`" );
	Check( !ParseBody( "clash_geo",
		"box_geometry\n{\n\tname\t\tL__geo\n\twidth\t\t1\n}\n" + light ),
	       "reject: an existing geometry already named `L__geo`" );
	Check( !ParseBody( "clash_obj",
		"sphere_geometry\n{\n\tname\t\tg\n\tradius\t\t1\n}\n"
		"standard_object\n{\n\tname\t\tL\n\tgeometry\tg\n}\n" + light ),
	       "reject: an existing object already named `L`" );
	Check( !ParseBody( "clash_self", light + light ),
	       "reject: two shape_lights with the same name" );
	// ... and the CROSS-CHUNK collision, which exists because shape_light and
	// rect_light deliberately use the SAME three derived suffixes.
	Check( !ParseBody( "clash_rect",
		"rect_light\n{\n\tname\t\tL\n\tcenter\t\t0 8 0\n\tsize\t\t1 1\n"
		"\tfacing\t\t0 -1 0\n\texitance\t10\n}\n" + light ),
	       "reject: a rect_light named `L` collides with a shape_light named `L` -- the two "
	       "chunks share the `__pnt` / `__mat` / `__geo` suffixes on purpose, so a name "
	       "reused across them fails the load rather than half-overwriting" );
}

//////////////////////////////////////////////////////////////////////
// 5 -- round-trip
//////////////////////////////////////////////////////////////////////

void TestRoundTrip()
{
	std::cout << "Test: a shape_light scene round-trips as the COMPACT form" << std::endl;

	const std::string body = SceneShell( 16 ) + FloorChunks() +
		ShapeLightChunk( "bulb_light", "sphere", "0 3 0", "0.4", "300", "1.0 0.92 0.8" );
	const std::string text = "RISE ASCII SCENE 7\n" + body;
	const std::string path = WriteTempScene( "roundtrip", text );

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); remove( path.c_str() ); return; }

	const bool loaded = job->LoadAsciiSceneViaCst( path.c_str() );
	Check( loaded, "roundtrip: the scene file loads" );
	Check( job->GetObjects() && job->GetObjects()->GetItem( "bulb_light" ) != 0,
	       "roundtrip: the shape_light derived into an object" );

	// SaveEngine::Save is an unconditional whole-Document Cst::SerializeCst on
	// the Job's retained CST Document (SaveEngine.h), so serializing that
	// Document here IS what a save writes to disk.
	Check( job->HasRetainedCstDocument(),
	       "roundtrip: the Job retains a CST Document (what save serializes)" );
	const RISE::Cst::Document* doc = job->GetCstDocument();
	if( doc ) {
		const std::string saved = RISE::Cst::SerializeCst( *doc );
		Check( saved.find( "shape_light" ) != std::string::npos,
		       "roundtrip: the saved text still contains `shape_light` (the compact form persists)" );
		Check( saved.find( "lambertian_luminaire_material" ) == std::string::npos &&
		       saved.find( "sphere_geometry" ) == std::string::npos,
		       "roundtrip: the save does NOT write out the expanded four-chunk chain" );
		Check( saved == text, "roundtrip: an unedited save is byte-identical to the source file" );
	} else {
		Check( false, "roundtrip: CST Document retrievable" );
	}

	safe_release( job );
	remove( path.c_str() );

	// And the same scene actually renders LIT -- a document that round-trips
	// but does not light anything would pass every check above.
	const RenderStats st = RenderScene( "roundtrip_render", body, 0.5 );
	std::cout << "    round-trip scene mean = " << st.mean << std::endl;
	Check( st.ok && st.mean > 1e-3, "roundtrip: the scene renders lit" );
}

} // anonymous namespace

int main()
{
	std::cout << "=== ShapeLightChunkTest ===" << std::endl;

	TestExpansionPerShape();
	TestEllipsoidRadii();
	TestNormalsPointOutward();
	TestRejections();
	TestSphereLightsTheFloorAndIsVisible();
	TestRoundTrip();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << "  Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
