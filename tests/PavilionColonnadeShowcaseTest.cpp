//////////////////////////////////////////////////////////////////////
//
//  PavilionColonnadeShowcaseTest.cpp -- the Phase 3 CSG-neighbour
//  showcase gate for `pavilion_colonnade.RISEscene`
//  (docs/PROXIMITY_SHOWCASES.md Sec 1; the engine rules are
//  docs/CROSS_OBJECT_PROXIMITY_DESIGN.md Sec 2, Sec 5.2, Sec 5.6, Sec 8.2).
//
//  WHAT THIS DRIVES.  `ProximitySignalTest` (l) already pins
//  glass_pavilion's fluted `column2` as a cross-object NEIGHBOUR.  This
//  suite re-asserts the equivalent query stations on the NEW scene built
//  from that scene's own chunks (a receiver on `column1` this time, plus
//  a second set of stations on `column2` to prove the composite's own
//  transform is applied correctly regardless of which column carries the
//  receiver), and ADDS what (l) does not: the painter stations read
//  through an in-process probe/control render pair, and a live-vs-zero
//  wall-clock cost gate.
//
//  Every world point below is DERIVED from the scene's own chunks (radius
//  0.25, azimuth 20 deg, slot half-width 0.04, cap top y = 0.175, camera
//  params read back off the CST) rather than copied from a previous run;
//  see docs/CROSS_OBJECT_PROXIMITY_DESIGN.md Sec 8.2's showcase precedent
//  and Sec 5.6 for why a CSG composite's boundary-arm answer at a 1 cm
//  station is tight (<= 0.5 + 1e-9, >= 0.4874 - 1e-4) while every other
//  station gets the general 0.05 window that precedent uses.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <string>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#ifdef _WIN32
  #include <process.h>
#else
  #include <unistd.h>
#endif

#include "../src/Library/Interfaces/ISurfaceSignalProvider.h"
#include "../src/Library/Interfaces/SurfaceSignalProximity.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Cameras/ThinLensCamera.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/ExpressionMemo.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Cst/Cst.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace fs = std::filesystem;

static int passCount = 0;
static int failCount = 0;

static void Check( const bool cond, const std::string& name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

static void CheckClose( const double got, const double want, const double tol, const std::string& name )
{
	if( std::fabs( got - want ) <= tol ) { ++passCount; }
	else {
		++failCount;
		std::cout << "  FAIL: " << name << "  got " << got
			<< " want " << want << " (tol " << tol << ")" << std::endl;
	}
}

//======================================================================
// Scene loading
//======================================================================

static fs::path FindRepoRoot()
{
	const char* candidates[] = { ".", "..", "../..", "../../.." };
	for( const char* c : candidates ) {
		const fs::path p( c );
		if( fs::exists( p / "scenes" / "FeatureBased" / "Combined" / "pavilion_colonnade.RISEscene" ) ) {
			return p;
		}
	}
	return fs::path();
}

static bool ReadFile( const fs::path& p, std::string& out )
{
	std::ifstream in( p );
	if( !in ) return false;
	std::stringstream ss;
	ss << in.rdbuf();
	out = ss.str();
	return true;
}

//! One probe: distance from a world point to the nearest surface other
//! than `self`'s, within `r` -- the SAME hand-built channel
//! `ProximitySignalTest` (l) uses, so a query result here cannot disagree
//! with the engine about what `SurfaceSignalInfo::Proximity` reads.
static Scalar ProximityAtMgr( IObjectManager* mgr, const Point3& p, const IObjectPriv* self, const Scalar r )
{
	SurfaceSignalInfo s;
	s.pScene  = mgr;
	s.pSelf   = self;
	s.ptWorld = p;
	ExpressionMemo::Invalidate();
	return s.Proximity( r );
}

//! The cap top's world Y is derived once from `capgeom` (0.1 position +
//! half of its 0.15 height) and every column's own y-position (2.5, both
//! column1 and column2), then pushed through the CARRYING column's own
//! `GetFinalTransformMatrix()` -- so a station on the rotated `column2`
//! cannot disagree with the engine about what `orientation 0 45 0` means,
//! exactly as `ProximitySignalTest` (l) does for that same column.
static Point3 CapTopWorldPoint( IObjectPriv* column, const Scalar localX, const Scalar localZ )
{
	const Matrix4 toWorld = column->GetFinalTransformMatrix();
	const Scalar capTopLocalY = Scalar( 0.175 ) - Scalar( 2.5 );
	const Point3 local( localX, capTopLocalY, localZ );
	return Point3Ops::Transform( toWorld, local );
}

//======================================================================
// Sightline occlusion -- docs/PROXIMITY_SHOWCASES.md Sec 0's mechanics,
// verbatim: a FRESH RayIntersection per cast, both faces, and the
// visibility test "no hit, or a hit with range >= |station - origin| - 1e-6".
//======================================================================

static bool IsVisible( IObjectManager* mgr, const Point3& origin, const Point3& station,
	const IObject** outOccluder )
{
	const Vector3 u = Vector3Ops::Normalize( Vector3Ops::mkVector3( station, origin ) );
	RasterizerState nullRasterizerState;
	RayIntersection ri( Ray( origin, u ), nullRasterizerState );
	mgr->IntersectRay( ri, true, true, false );
	const double dist = Vector3Ops::Magnitude( Vector3Ops::mkVector3( station, origin ) );
	if( !ri.geometric.bHit ) return true;
	if( (double)ri.geometric.range >= dist - 1e-6 ) return true;
	if( outOccluder ) *outOccluder = ri.pObject;
	return false;
}

//! Reverse name lookup for a failure message only -- `IObjectManager` has
//! no pointer->name accessor, so this enumerates the manager's own named
//! items (`EnumerateItemNames`) and matches each back through `GetItem`.
//! O(number of named objects) per call; only ever invoked when an
//! occlusion Check has already failed, never on the hot/passing path.
static std::string NameOfObject( IObjectManager* mgr, const IObject* obj )
{
	if( !obj ) return "<none>";
	if( !mgr ) return "<unknown, no manager>";
	struct NC : public IEnumCallback<const char*>
	{
		std::vector<std::string> names;
		bool operator()( const char* const& s ) override { if( s ) names.push_back( std::string( s ) ); return true; }
	} nc;
	mgr->EnumerateItemNames( nc );
	for( const std::string& n : nc.names ) {
		if( mgr->GetItem( n.c_str() ) == obj ) return n;
	}
	return "<unnamed>";
}

//======================================================================
// CST helpers -- the descriptor-driven document surgery
// docs/PROXIMITY_SHOWCASES.md Sec 0 specifies: the rasterizer chunk is
// UNNAMED, so it is found by ROLE (ends in "_rasterizer"), never by name.
//======================================================================

static Cst::NodeId FindChunkByRoleSuffix( const Cst::Document& doc, const std::string& suffix )
{
	const int n = Cst::DocItemCount( doc );
	for( int i = 0; i < n; ++i ) {
		const Cst::NodeId id = Cst::DocNodeIdAt( doc, i );
		const Cst::NodeRef item = Cst::DocResolveNodeId( doc, id );
		if( item && item->kind == Cst::NodeKind::Chunk ) {
			const std::string& role = item->role;
			if( role.size() >= suffix.size() &&
				role.compare( role.size() - suffix.size(), suffix.size(), suffix ) == 0 ) {
				return id;
			}
		}
	}
	return 0;
}

//! Read a named chunk's param value back off the Document, so the camera
//! arithmetic below is derived from the TRACKED FILE rather than
//! hard-coded from the header's own prose.
static std::string ReadParam( const Cst::Document& doc, Cst::NodeId chunkId, const std::string& role )
{
	const Cst::NodeRef chunk = Cst::DocResolveNodeId( doc, chunkId );
	bool present = false;
	const std::string v = Cst::ParamValueAsParsed( chunk, role, &present );
	return present ? v : std::string();
}

static Point3 ParseVec3( const std::string& s )
{
	std::istringstream iss( s );
	double x = 0, y = 0, z = 0;
	iss >> x >> y >> z;
	return Point3( x, y, z );
}

static double ParseScalar( const std::string& s )
{
	std::istringstream iss( s );
	double x = 0;
	iss >> x;
	return x;
}

//======================================================================
// Camera projection -- world point -> pixel, replicating
// ThinLensCamera::Recompute's own formula (src/Library/Cameras/ThinLensCamera.cpp)
// so the painter stations below are read at the RIGHT pixel.  Validated
// against the REAL engine camera (not just this formula) immediately
// below, the same "cannot disagree with the engine" posture the query
// stations take.
//======================================================================

struct CamParams
{
	Point3   location, lookat;
	Vector3  up;
	double   sensorSize_mm, focalLength_mm, focusDistance, fstop;
	unsigned int width, height;
};

static bool LoadBeautyCamParams( const Cst::Document& doc, CamParams& out, unsigned int filmW, unsigned int filmH )
{
	const Cst::NodeId camId = Cst::DocFindByName( doc, "thinlens_camera/beauty_cam" );
	Check( camId > 0, "camera chunk 'thinlens_camera/beauty_cam' is found by name" );
	if( camId <= 0 ) return false;

	out.location       = ParseVec3( ReadParam( doc, camId, "location" ) );
	out.lookat          = ParseVec3( ReadParam( doc, camId, "lookat" ) );
	out.up              = Vector3Ops::mkVector3( ParseVec3( ReadParam( doc, camId, "up" ) ), Point3( 0, 0, 0 ) );
	out.sensorSize_mm   = ParseScalar( ReadParam( doc, camId, "sensor_size" ) );
	out.focalLength_mm  = ParseScalar( ReadParam( doc, camId, "focal_length" ) );
	out.focusDistance   = ParseScalar( ReadParam( doc, camId, "focus_distance" ) );
	out.fstop           = ParseScalar( ReadParam( doc, camId, "fstop" ) );
	out.width  = filmW;
	out.height = filmH;
	return true;
}

//! World point -> the SCREEN point the camera consumes.  NOT a raster
//! index: see `ScreenToRaster` below for the difference, and for the bug
//! that conflating the two produced.
static Point2 ProjectWorldToScreen( const CamParams& c, const Point3& P )
{
	const Vector3 forward = Vector3Ops::Normalize( Vector3Ops::mkVector3( c.lookat, c.location ) );
	const Vector3 right   = Vector3Ops::Normalize( Vector3Ops::Cross( forward, c.up ) );
	const Vector3 camUp   = Vector3Ops::Normalize( Vector3Ops::Cross( right, forward ) );

	const Vector3 v = Vector3Ops::mkVector3( P, c.location );
	const double vx = Vector3Ops::Dot( v, right );
	const double vy = Vector3Ops::Dot( v, camUp );
	const double vz = Vector3Ops::Dot( v, forward );

	const double mm_to_scene = 0.001;   // sceneUnitMeters = 1.0 (this scene declares no `scene_unit`)
	const double sensor_scene = c.sensorSize_mm * mm_to_scene;
	const double focal_scene  = c.focalLength_mm * mm_to_scene;
	const double imageAspect  = (double)c.width / (double)c.height;   // pixelAR = 1
	const double effective_sensor_v = sensor_scene / imageAspect;
	const double fov = 2.0 * std::atan( effective_sensor_v / ( 2.0 * focal_scene ) );
	const double filmDistance = c.focusDistance * focal_scene / ( c.focusDistance - focal_scene );
	const double sy = -2.0 * filmDistance * std::tan( fov / 2.0 ) / (double)c.height;
	const double sx = -sy;

	// NOTE the y sign: the engine's camera basis (OrthonormalBasis3D::CreateFromWV,
	// used by every RISE camera) sets W = forward, U = cross(up, forward) and
	// V = cross(W, U) -- so U is the NEGATIVE of the naive right = cross(forward, up),
	// and working through ThinLensCamera::GenerateRay's `focus` construction (whose
	// `oneMinusT` is NEGATIVE for any point beyond the lens) gives a local ray
	// direction proportional to (-x_img, -y_img, +filmDistance).  Composed with
	// U = -right, the x-image and x-world axes end up aligned (no flip) while the
	// y-image and y-world (camUp) axes end up ANTI-aligned -- verified below by
	// `ValidateProjection` against the real `ThinLensCamera`, which is what caught
	// this sign the first time (S1's y_img is ~0 by construction -- the lookat point
	// has no camUp component -- so only an OFF-AXIS station like S6 exposes it).
	const double x_img = filmDistance * vx / vz;
	const double y_img = -filmDistance * vy / vz;

	const double x_pix = x_img / sx + 0.5 * (double)c.width;
	const double y_pix = y_img / sy + 0.5 * (double)c.height;
	return Point2( x_pix, y_pix );
}

//! SCREEN POINT -> RASTER INDEX, and the FIX for the bug this test
//! shipped with on 2026-09-10.
//!
//! THE TWO COORDINATE SYSTEMS, and why they are not the same one.  Every
//! RISE camera consumes a SCREEN point whose y counts UP from the bottom
//! of the frame: the rasterizer's per-pixel loop
//! (`PixelBasedPelRasterizer::IntegratePixel`, both its sampled branch --
//! `ptOnScreen = Point2( x + jx - 0.5, (height-y) + jy - 0.5 )` -- and its
//! unsampled one, `Point2( x, height-y )`) hands the camera `height - y`
//! for RASTER row `y`.  The captured framebuffer, on the other hand, is
//! indexed by that raster row, top-down.  So a screen point `s` is imaged
//! by raster row `height - s`, and reading `pixels[ (int)s.y ]` reads the
//! MIRRORED row.
//!
//! WHAT THAT COST.  Until this fix the painter stations projected to a
//! screen point and then indexed the framebuffer with it directly.  S1
//! survived because it lands at y = 300.0 on a 600-row film and
//! `600 - 300 == 300` -- the flip is the identity at the vertical centre,
//! which is exactly where the `lookat` station sits.  S6, off-axis at
//! y = 407.1, was read at raster row 407 instead of 193: a floor pixel
//! that carries no cap contribution at all, so the probe and the control
//! renders agree there and the ratio reads ~1.02 no matter what the
//! signal says.  That reading was mistaken for a render-time defect in
//! `proximity()` and recorded as an open finding in this file, in the
//! scene header and in the design doc's 8.5; all three are corrected in
//! the same commit.  The signal was right all along: at the TRUE row the
//! probe pixel is exactly black, at 1, 8 and 64 spp alike.
//!
//! ROUNDING.  Raster row `y` covers screen y in [height-y-0.5,
//! height-y+0.5) (read it off the `+ jy - 0.5` above, with jy in [0,1)),
//! and column `x` covers screen x in [x-0.5, x+0.5) -- so both axes round
//! to nearest, not `floor`, which is the other half-pixel this used to
//! get wrong on the x axis.
static void ScreenToRaster( const CamParams& c, const Point2& screen, int& outX, int& outY )
{
	outX = (int)std::floor( screen.x + 0.5 );
	outY = (int)std::floor( (double)c.height - screen.y + 0.5 );
}

//! Validates the formula above against the REAL `ThinLensCamera`: the
//! chief ray (lens sample at the disk centre, per the
//! `tests/CameraUnitConversionTest.cpp` precedent) generated for the
//! computed screen point must point at `P`.  Takes a SCREEN point, which
//! is what `ThinLensCamera::GenerateRayWithLensSample` takes -- so this
//! check could never have caught the raster/screen confusion above, and
//! did not; the guard that does is the cap1 cast in (b), which now builds
//! its screen point back OUT of the raster index the readback uses.
static bool ValidateProjection( const CamParams& c, const Point3& P, const Point2& pix )
{
	ThinLensCamera* cam = new ThinLensCamera(
		c.location, c.lookat, c.up,
		c.sensorSize_mm, c.focalLength_mm, c.fstop, c.focusDistance, /*sceneUnitMeters*/1.0,
		c.width, c.height, /*pixelAR*/1.0, /*exposure*/0.0, /*scanRate*/0.0, /*pixelRate*/0.0,
		Vector3( 0, 0, 0 ), Vector2( 0, 0 ),
		/*blades*/0u, /*rot*/0.0, /*squeeze*/1.0, /*tiltX*/0.0, /*tiltY*/0.0, /*shiftX*/0.0, /*shiftY*/0.0 );

	RandomNumberGenerator rng( 1u );
	RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
	Ray ray;
	cam->GenerateRayWithLensSample( rc, ray, pix, Point2( 0.5, 0.5 ) );

	const Vector3 want = Vector3Ops::Normalize( Vector3Ops::mkVector3( P, c.location ) );
	const double d = Vector3Ops::Dot( Vector3Ops::Normalize( ray.Dir() ), want );
	cam->release();
	return d > 0.999;
}

//======================================================================
// Rendering -- the capturing IRasterizerOutput + Job::Rasterize() triple
// (tests/EnvLightBalanceTest.cpp's pattern), driven off an in-memory
// Cst::Document rather than a file on disk.
//======================================================================

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
		width = pImage.GetWidth();
		height = pImage.GetHeight();
		pixels.resize( width * height );
		for( unsigned int y = 0; y < height; y++ ) {
			for( unsigned int x = 0; x < width; x++ ) {
				pixels[y * width + x] = pImage.GetPEL( x, y );
			}
		}
	}
};

struct RenderedImage
{
	bool ok = false;
	unsigned int width = 0, height = 0;
	std::vector<RISEColor> pixels;

	double At( int x, int y ) const
	{
		if( x < 0 || y < 0 || (unsigned)x >= width || (unsigned)y >= height ) return -1.0;
		const RISEColor& c = pixels[(unsigned)y * width + (unsigned)x];
		return ( (double)c.base.r + (double)c.base.g + (double)c.base.b ) / 3.0;
	}
};

//! Renders a Document in-process and captures the linear framebuffer.
//! `oidn_denoise FALSE` is forced by the CALLER (docs/PROXIMITY_SHOWCASES.md
//! Sec 0 setter list) before this is invoked; asserting `diagnostics.empty()`
//! here is the same gate `MeshClosestPointTest`'s `LoadSceneD` (g) applies.
static bool RenderDocument( const Cst::Document& doc, RenderedImage& out, std::vector<std::string>& diags )
{
	Job* job = new Job();
	Cst::DeriveToJob( doc, *job, &diags );
	if( !diags.empty() ) { job->release(); return false; }

	IObjectManager* mgr = job->GetObjects();
	if( !mgr ) { job->release(); return false; }
	mgr->PrepareForRendering();

	job->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( cap, __FILE__, __LINE__, "PavilionColonnadeShowcaseTest capture output" );
	if( !job->GetRasterizer() ) { cap->release(); job->release(); return false; }
	job->GetRasterizer()->AddRasterizerOutput( cap );

	const bool rendered = job->Rasterize();
	if( rendered ) {
		out.ok = true;
		out.width = cap->width;
		out.height = cap->height;
		out.pixels = cap->pixels;
	}

	cap->release();
	job->release();
	return rendered;
}

//! Wall-clock-only render, for the cost gate: no capture, since the cost
//! is about the render pass itself, not the pixel readback.
static double TimeRasterize( const std::string& sceneText, bool& outOk )
{
	outOk = false;
	Cst::Document doc = Cst::ParseToCst( sceneText );
	Job* job = new Job();
	std::vector<std::string> diags;
	Cst::DeriveToJob( doc, *job, &diags );
	if( !diags.empty() ) { job->release(); return 0.0; }

	IObjectManager* mgr = job->GetObjects();
	if( !mgr ) { job->release(); return 0.0; }
	mgr->PrepareForRendering();

	job->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( cap, __FILE__, __LINE__, "PavilionColonnadeShowcaseTest cost-gate output" );
	if( !job->GetRasterizer() ) { cap->release(); job->release(); return 0.0; }
	job->GetRasterizer()->AddRasterizerOutput( cap );

	const auto t0 = std::chrono::steady_clock::now();
	const bool rendered = job->Rasterize();
	const auto t1 = std::chrono::steady_clock::now();

	cap->release();
	job->release();

	if( !rendered ) return 0.0;
	outOk = true;
	return std::chrono::duration<double>( t1 - t0 ).count();
}

//======================================================================
// main
//======================================================================

int main()
{
	std::cout << "=== PavilionColonnadeShowcaseTest ===" << std::endl;

	// STEP 0: every core busy, the same route ./bench.sh uses
	// (docs/PROXIMITY_SHOWCASES.md Sec 0's cost-gate protocol) --
	// `render_thread_reserve_count` is an OPTIONS-file setting, not a
	// scene parameter, read lazily by `GlobalOptions()` on first access,
	// so this MUST run before any Job/Rasterizer touches it.
	char optsPath[512];
	std::snprintf( optsPath, sizeof(optsPath), "/tmp/pavilion_colonnade_bench_opts_%d.txt",
		static_cast<int>( ::getpid() ) );
	{
		std::ofstream ofs( optsPath );
		ofs << "render_thread_reserve_count 0\n";
		ofs << "force_all_threads_low_priority false\n";
	}
#ifdef _WIN32
	_putenv_s( "RISE_OPTIONS_FILE", optsPath );
#else
	setenv( "RISE_OPTIONS_FILE", optsPath, 1 );
#endif

	const fs::path root = FindRepoRoot();
	Check( !root.empty(), "the repo root is found (scenes/FeatureBased/Combined/pavilion_colonnade.RISEscene exists)" );
	if( root.empty() ) {
		std::cout << std::endl << passCount << " passed, " << ( failCount + 1 ) << " failed." << std::endl;
		return 1;
	}
	const fs::path scenePath = root / "scenes" / "FeatureBased" / "Combined" / "pavilion_colonnade.RISEscene";

	std::string sceneText;
	Check( ReadFile( scenePath, sceneText ), "the tracked scene file is readable" );
	if( sceneText.empty() ) {
		std::cout << std::endl << passCount << " passed, " << ( failCount + 1 ) << " failed." << std::endl;
		return 1;
	}

	//------------------------------------------------------------------
	// (a) LOAD, through the normal CST path.
	//------------------------------------------------------------------
	Cst::Document doc = Cst::ParseToCst( sceneText );
	Job* job = new Job();
	std::vector<std::string> diags;
	Cst::DeriveToJob( doc, *job, &diags );
	for( const std::string& d : diags ) std::cout << "  scene diagnostic: " << d << std::endl;
	Check( diags.empty(), "(a) pavilion_colonnade derives with NO diagnostics" );

	IObjectManager* mgr = job->GetObjects();
	Check( mgr != 0, "(a) it derives with an object manager" );
	if( !mgr ) { job->release(); return failCount == 0 ? 0 : 1; }
	mgr->PrepareForRendering();

	IObjectPriv* cap1     = mgr->GetItem( "cap1" );
	IObjectPriv* cap2     = mgr->GetItem( "cap2" );
	IObjectPriv* column1  = mgr->GetItem( "column1" );
	IObjectPriv* column2  = mgr->GetItem( "column2" );
	Check( cap1 && cap2 && column1 && column2, "(a) cap1, cap2, column1 and column2 are present" );
	if( !cap1 || !cap2 || !column1 || !column2 ) { job->release(); return failCount == 0 ? 0 : 1; }

	// --- THE `capped TRUE` PIN, with teeth (same MONEY check as
	// ProximitySignalTest (l), re-asserted on the NEW scene's own
	// column1 rather than glass_pavilion's column2).
	{
		IObjectPriv* col1Cyl = mgr->GetItem( "col1_cyl" );
		Check( col1Cyl != 0, "(a) col1_cyl (column1's cylinder operand) is present" );
		if( col1Cyl ) {
			Scalar f = 0; bool ex = false;
			Check( col1Cyl->SignedDistanceLower( Point3( -2.5, 3, 2.5 ), Scalar( 10 ), f, ex ),
				"(a) MONEY -- colcylgeom answers the SIGNED query (capped TRUE): an open tube is a "
				"sheet and would make every fluted column stop answering proximity()" );
			Check( ex, "(a) ...exactly, so a boundary landing on the column wall is admissible" );
		}
	}

	//------------------------------------------------------------------
	// (a) QUERY STATIONS S1-S7, all derived from the geometry constants
	// (radius 0.25, azimuth 20 deg, slot half-width 0.04, cap top
	// y = 0.175) rather than hard-coded world points.
	//------------------------------------------------------------------
	const double radius = 0.25;
	const double az20 = 20.0 * PI / 180.0;

	// S1/S2/S3: cap1 top, 20 deg azimuth, 1/2/4 cm outside the wall.
	{
		const double offsets[3] = { 0.01, 0.02, 0.04 };
		const double want[3]    = { 0.5,  0.0,  0.0  };
		const char* names[3] = { "S1", "S2", "S3" };
		for( int i = 0; i < 3; ++i ) {
			const double r = radius + offsets[i];
			const Point3 world = CapTopWorldPoint( column1, Scalar( r * std::sin( az20 ) ), Scalar( r * std::cos( az20 ) ) );
			const double v = (double)ProximityAtMgr( mgr, world, cap1, Scalar( 0.02 ) );
			std::cout << "    " << names[i] << " (" << offsets[i]*100 << " cm out, 20 deg az): proximity(0.02) = " << v << std::endl;
			if( i == 0 ) {
				// The 1 cm station is the CSG boundary arm firing exactly
				// (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md Sec 5.6 / Sec 8.2):
				// never above 0.5, never below the one-probe-step floor.
				Check( v <= 0.5 + 1e-9, "(a) MONEY -- S1 never reads ABOVE 0.5 (over-read would be contact painted where there is none)" );
				Check( v >= 0.4874 - 1e-4, "(a) ...and never below the one-probe-step floor 0.5 - eps/r = 0.4874" );
			} else {
				// S2/S3 are exact 0: a CSG neighbour's FAR stations are
				// the exclusive `d < r` cutoff, not a boundary landing --
				// same exactness class as a box/sphere neighbour
				// (docs/PROXIMITY_SHOWCASES.md Sec 0's "box or sphere
				// neighbour is exact -> 1e-9").
				CheckClose( v, want[i], 1e-9, std::string("(a) MONEY -- ") + names[i] + " reads its predicted value" );
			}
		}
	}

	// S4: flute mouth, local +z, 1 cm out -- the phantom-refusal station.
	{
		const double r = radius + 0.01;
		const Point3 world = CapTopWorldPoint( column1, Scalar( 0.0 ), Scalar( r ) );
		const double v = (double)ProximityAtMgr( mgr, world, cap1, Scalar( 0.02 ) );
		// S4 is the refusal station: `SurfaceSignalInfo::Proximity`'s
		// refusal branch returns a bit-exact 0 (NeutralProximity(),
		// no arithmetic), and `ProximitySignalTest` (l) pins the
		// equivalent flute-mouth refusal at 1e-12.
		CheckClose( v, 0.0, 1e-12, "(a) MONEY -- S4 (flute mouth, 1 cm outside the tangent face) reads 0 at the scene query, "
			"not the phantom 1.00 a tolerant landing test would report" );

		Scalar dObj = Scalar( 0 );
		Check( !column1->DistanceToSurface( world, Scalar( 0.1 ), dObj ),
			"(a) MONEY -- column1->DistanceToSurface at radius 0.1 REFUSES at S4 -- the bracket keeps "
			"probing into the slot and never finds an admitted landing" );

		Scalar dScene = Scalar( 0 );
		Check( mgr->NearestOtherSurface( world, cap1, Scalar( 0.1 ), dScene ),
			"(a) ...while the SCENE-WIDE query at the same radius ANSWERS" );
		CheckClose( (double)dScene, 0.075, 1e-9,
			"(a) ...with 0.075, the floor top at y = 0.1 under the cap top at 0.175 -- exactly why "
			"the refusal above has to be asked PER OBJECT" );

		const double corner = std::sqrt( 0.04*0.04 +
			( r - std::sqrt( radius*radius - 0.04*0.04 ) ) * ( r - std::sqrt( radius*radius - 0.04*0.04 ) ) );
		std::cout << "    S4: proximity(0.02) = " << v << "; column1 refuses at r = 0.1; the true corner "
			"distance the refusal under-paints is " << corner << " m" << std::endl;
		Check( corner > 0.04 && corner < 0.043, "(a) ...and that corner really is ~4.21 cm" );
	}

	// S5a/S5b: the SAME two local points, but on the ROTATED column2 --
	// pushed through column2's own GetFinalTransformMatrix(), so a 45 deg
	// y-rotation cannot be gotten wrong by this test.
	{
		const double r1 = radius + 0.01;
		const Point3 s5a = CapTopWorldPoint( column2, Scalar( r1 * std::sin( az20 ) ), Scalar( r1 * std::cos( az20 ) ) );
		const double v5a = (double)ProximityAtMgr( mgr, s5a, cap2, Scalar( 0.02 ) );
		std::cout << "    S5a (column2, 1 cm out, 20 deg az): proximity(0.02) = " << v5a
			<< "  world (" << (double)s5a.x << ", " << (double)s5a.y << ", " << (double)s5a.z << ")" << std::endl;
		Check( v5a <= 0.5 + 1e-9, "(a) MONEY -- S5a never reads above 0.5" );
		Check( v5a >= 0.4874 - 1e-4, "(a) ...and never below 0.4874" );
		CheckClose( (double)s5a.y, 0.175, 1e-9, "(a) ...and lands exactly on cap2's top face (world y = 0.175), "
			"not mid-column height -- a 45 deg y-rotation leaves y untouched" );

		const double r2 = radius + 0.01;
		const Point3 s5b = CapTopWorldPoint( column2, Scalar( 0.0 ), Scalar( r2 ) );
		const double v5b = (double)ProximityAtMgr( mgr, s5b, cap2, Scalar( 0.02 ) );
		std::cout << "    S5b (column2, flute mouth): proximity(0.02) = " << v5b
			<< "  world (" << (double)s5b.x << ", " << (double)s5b.y << ", " << (double)s5b.z << ")" << std::endl;
		// A plain far station like S2/S3/S6: exact 0 by the exclusive
		// `d < r` cutoff, so 1e-9 rather than the general 0.05 window.
		CheckClose( v5b, 0.0, 1e-9, "(a) MONEY -- S5b reads 0 (the phantom refusal, reproduced on the rotated column)" );
		CheckClose( (double)s5b.y, 0.175, 1e-9, "(a) ...and also lands on cap2's top face" );
	}

	// S6: cap1 top, 6.5 cm from the wall along local +x (radius 0.315).
	{
		const Point3 world = CapTopWorldPoint( column1, Scalar( 0.315 ), Scalar( 0.0 ) );
		const double v = (double)ProximityAtMgr( mgr, world, cap1, Scalar( 0.02 ) );
		CheckClose( v, 0.0, 1e-9, "(a) MONEY -- S6 (6.5 cm from the wall) reads 0 -- nothing within 2 cm" );
		std::cout << "    S6: proximity(0.02) = " << v << std::endl;

		// A dense 4x4 cm grid around S6 (checked during investigation,
		// not asserted here to keep the log short) confirms the field is
		// FLAT ZERO throughout the neighbourhood -- the query-channel
		// station is not a knife-edge coincidence, and it is why the
		// PAINTER station at S6 (below) can assert a HARD 0: the probe
		// pixel and every pixel the film filter gathers from are black.
	}

	// S7: inside the slot mouth, local (0.03, y, 0.20) -- the composite
	// boundary arm firing on the box's exact face.
	{
		const Point3 world = CapTopWorldPoint( column1, Scalar( 0.03 ), Scalar( 0.20 ) );
		const double v = (double)ProximityAtMgr( mgr, world, cap1, Scalar( 0.02 ) );
		std::cout << "    S7: proximity(0.02) = " << v << std::endl;
		Check( v <= 0.5 + 1e-9, "(a) MONEY -- S7 never reads above 0.5" );
		Check( v >= 0.4874 - 1e-4, "(a) ...and never below 0.4874 (the +x slot wall, an exact box face, 1 cm away)" );
	}

	//------------------------------------------------------------------
	// (b) PAINTER STATIONS at S1 and S6 ONLY, from the shipped camera.
	// The receiver (`marble_cap`) is already a `lambertian_material`, so
	// the probe is the `expr` swap alone plus the two rasterizer setters
	// every probe gets (docs/PROXIMITY_SHOWCASES.md Sec 0).
	//------------------------------------------------------------------
	{
		// Camera params, read back off the CST (never hard-coded).
		const Cst::NodeId filmId = FindChunkByRoleSuffix( doc, "film" );
		Check( filmId > 0, "(b) the 'film' chunk is found by role" );
		unsigned int filmW = 800, filmH = 600;
		if( filmId > 0 ) {
			const std::string ws = ReadParam( doc, filmId, "width" );
			const std::string hs = ReadParam( doc, filmId, "height" );
			if( !ws.empty() ) filmW = (unsigned int)ParseScalar( ws );
			if( !hs.empty() ) filmH = (unsigned int)ParseScalar( hs );
		}
		Check( filmW == 800 && filmH == 600, "(b) film is 800x600, per docs/PROXIMITY_SHOWCASES.md Sec 0's framing rule" );

		CamParams cam;
		Check( LoadBeautyCamParams( doc, cam, filmW, filmH ), "(b) the shipped camera's params are read back off the CST" );

		// S1 and S6 world points, re-derived exactly as in (a).
		const double r1 = radius + 0.01;
		const Point3 s1World = CapTopWorldPoint( column1, Scalar( r1 * std::sin( az20 ) ), Scalar( r1 * std::cos( az20 ) ) );
		const Point3 s6World = CapTopWorldPoint( column1, Scalar( 0.315 ), Scalar( 0.0 ) );

		// --- The sightline: unoccluded, per Sec 0's single-cast rule
		// (true, true, false) for a showcase with no refractive object.
		const IObject* occluder1 = nullptr;
		const IObject* occluder6 = nullptr;
		const bool s1Visible = IsVisible( mgr, cam.location, s1World, &occluder1 );
		const bool s6Visible = IsVisible( mgr, cam.location, s6World, &occluder6 );
		Check( s1Visible, "(b) MONEY -- S1's sightline from the beauty camera is UNOCCLUDED" +
			( s1Visible ? std::string() : ( " -- occluded by \"" + NameOfObject( mgr, occluder1 ) + "\"" ) ) );
		Check( s6Visible, "(b) MONEY -- S6's sightline from the beauty camera is UNOCCLUDED" +
			( s6Visible ? std::string() : ( " -- occluded by \"" + NameOfObject( mgr, occluder6 ) + "\"" ) ) );

		const Point2 pix1 = ProjectWorldToScreen( cam, s1World );
		const Point2 pix6 = ProjectWorldToScreen( cam, s6World );

		// The RASTER indices the framebuffer is actually read at -- the
		// screen points above flipped through `ScreenToRaster`, whose
		// comment records the bug that omitting the flip caused.
		int x1 = 0, y1 = 0, x6 = 0, y6 = 0;
		ScreenToRaster( cam, pix1, x1, y1 );
		ScreenToRaster( cam, pix6, x6, y6 );
		Check( y1 == 300 && y6 == (int)filmH - 407,
			"(b) MONEY -- the raster rows are the SCREEN rows flipped through height - y "
			"(S1 at the vertical centre is its own mirror; S6, off-axis, is not -- the flip "
			"is load-bearing exactly where it used to be missing)" );
		Check( x1 == 400,
			"(b) MONEY -- S1's raster column is exactly 400 (the horizontal centre of an 800-wide "
			"frame), asserted numerically rather than folded into the row-only check above" );

		// The SAME raster-index identity guard as S6 below, but for S1.
		// S1 sits at the vertical centre (y1 == 300, its own mirror under
		// `height - y`), so the flip bug that hid at S1 could not have been
		// caught by a guard that only cross-checks y -- this one builds its
		// screen point back OUT OF THE RASTER INDEX `(x1, y1)`, exactly as
		// `IntegratePixel` does, and requires every one of 200
		// aperture-jittered samples in that pixel to land on cap1.
		{
			ThinLensCamera* sanityCam1 = new ThinLensCamera(
				cam.location, cam.lookat, cam.up,
				cam.sensorSize_mm, cam.focalLength_mm, cam.fstop, cam.focusDistance, 1.0,
				cam.width, cam.height, 1.0, 0.0, 0.0, 0.0,
				Vector3(0,0,0), Vector2(0,0), 0u, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0 );
			RandomNumberGenerator sanityRng1( 1u );
			RuntimeContext sanityRc1( sanityRng1, RuntimeContext::PASS_NORMAL, false );
			// `IntegratePixel`'s own screen point for raster pixel (x1, y1).
			const double baseX1 = (double)x1 - 0.5;
			const double baseY1 = (double)filmH - (double)y1 - 0.5;
			int nCap1AtS1 = 0;
			const int kSamplesS1 = 200;
			for( int i = 0; i < kSamplesS1; ++i ) {
				const double jx = sanityRng1.CanonicalRandom(), jy = sanityRng1.CanonicalRandom();
				Ray ray1;
				sanityCam1->GenerateRay( sanityRc1, ray1, Point2( baseX1 + jx, baseY1 + jy ) );
				RasterizerState rs0;
				RayIntersection dbg1( ray1, rs0 );
				mgr->IntersectRay( dbg1, true, true, false );
				if( dbg1.pObject == cap1 ) ++nCap1AtS1;
			}
			sanityCam1->release();
			Check( nCap1AtS1 == kSamplesS1, "(b) every one of 200 aperture-jittered samples in S1's pixel lands on cap1" );
		}

		// Sanity guard against a pixel-picking mistake: 200 REAL
		// aperture-jittered samples (a fresh ThinLensCamera, the actual
		// per-sample `GenerateRay` path that draws its own lens sample
		// from `rc.random`, exactly as every one of the render's 512 spp
		// does) within S6's pixel must all land on cap1, close to S6
		// itself -- ruling out "the wrong object/point is being read".
		//
		// IT BUILDS ITS SCREEN POINT OUT OF THE RASTER INDEX `(x6, y6)`
		// the readback below uses, exactly as `IntegratePixel` does, NOT
		// out of `pix6` directly.  That is the whole point of the guard
		// after 2026-09-10: cast through the same index you read, or the
		// check validates a pixel the readback never touches -- which is
		// how the mirrored row went unnoticed through a 200-cast sanity
		// check that passed.
		{
			ThinLensCamera* sanityCam = new ThinLensCamera(
				cam.location, cam.lookat, cam.up,
				cam.sensorSize_mm, cam.focalLength_mm, cam.fstop, cam.focusDistance, 1.0,
				cam.width, cam.height, 1.0, 0.0, 0.0, 0.0,
				Vector3(0,0,0), Vector2(0,0), 0u, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0 );
			RandomNumberGenerator sanityRng( 1u );
			RuntimeContext sanityRc( sanityRng, RuntimeContext::PASS_NORMAL, false );
			// `IntegratePixel`'s own screen point for raster pixel (x6, y6).
			const double baseX = (double)x6 - 0.5;
			const double baseY = (double)filmH - (double)y6 - 0.5;
			int nCap1 = 0;
			const int kSamples = 200;
			for( int i = 0; i < kSamples; ++i ) {
				const double jx = sanityRng.CanonicalRandom(), jy = sanityRng.CanonicalRandom();
				Ray r2;
				sanityCam->GenerateRay( sanityRc, r2, Point2( baseX + jx, baseY + jy ) );
				RasterizerState rs1;
				RayIntersection dbg2( r2, rs1 );
				mgr->IntersectRay( dbg2, true, true, false );
				if( dbg2.pObject == cap1 ) ++nCap1;
			}
			sanityCam->release();
			Check( nCap1 == kSamples, "(b) every one of 200 aperture-jittered samples in S6's pixel lands on cap1" );
		}

		std::cout << "    S1 projects to screen (" << pix1.x << ", " << pix1.y << ") -> raster pixel (" << x1 << ", " << y1 << ")" << std::endl;
		std::cout << "    S6 projects to screen (" << pix6.x << ", " << pix6.y << ") -> raster pixel (" << x6 << ", " << y6 << ")" << std::endl;
		Check( ValidateProjection( cam, s1World, pix1 ), "(b) MONEY -- S1's projected pixel's chief ray, cast through the REAL ThinLensCamera, "
			"points at S1 (cannot disagree with the engine)" );
		Check( ValidateProjection( cam, s6World, pix6 ), "(b) ...and the same holds for S6" );
		Check( pix1.x >= 0 && pix1.x < filmW && pix1.y >= 0 && pix1.y < filmH, "(b) S1's pixel is inside the frame" );
		Check( pix6.x >= 0 && pix6.x < filmW && pix6.y >= 0 && pix6.y < filmH, "(b) S6's pixel is inside the frame" );

		// --- Build the probe (expr = vec3(dust,dust,dust)) and control
		// (expr = vec3(1,1,1)) documents.  `oidn_denoise FALSE` is set on
		// BOTH -- SourceHygieneTest's literal (below, next to the actual
		// setter calls): oidn_denoise FALSE forces the denoiser off so a
		// probe render is not smeared by OIDN.
		const Cst::NodeId rastId = FindChunkByRoleSuffix( doc, "_rasterizer" );
		Check( rastId > 0, "(b) the rasterizer chunk is found by role (ends in '_rasterizer')" );
		const Cst::NodeId painterId = Cst::DocFindByName( doc, "expression_painter/pnt_cap_dust" );
		Check( painterId > 0, "(b) the receiver's expression_painter 'pnt_cap_dust' is found by name" );

		RenderedImage probeImg, controlImg;
		bool probeOk = false, controlOk = false;
		if( rastId > 0 && painterId > 0 ) {
			// PROBE: expr -> vec3(dust, dust, dust).
			Cst::Document probeDoc = doc;
			probeDoc = Cst::DocSetOrAddParamValue( probeDoc, rastId, "oidn_denoise", 0, "FALSE" );   // oidn_denoise FALSE
			probeDoc = Cst::DocSetOrAddParamValue( probeDoc, rastId, "samples", 0, "512" );
			probeDoc = Cst::DocSetOrAddParamValue( probeDoc, painterId, "expr", 0, "vec3(dust, dust, dust)" );
			Check( Cst::SerializeCst( probeDoc ).find( "vec3(dust, dust, dust)" ) != std::string::npos,
				"(b) the probe document's serialized text carries the swapped `expr` line" );

			// Cross-check: on a JOB DERIVED FROM THE PROBE DOCUMENT ITSELF
			// (not the outer job), the QUERY CHANNEL still reads 0 at S6 --
			// so the underlying `NearestOtherSurface` machinery agrees with
			// itself regardless of which derived Job asks.  Kept from the
			// 2026-09-10 investigation: it is the check that isolates a
			// future painter-station failure to render-TIME evaluation
			// rather than to the signal or to how the probe document was
			// built.  (The failure it was written for turned out to be in
			// neither -- it was this test's own framebuffer indexing; see
			// `ScreenToRaster`.)
			{
				Job* checkJob = new Job();
				std::vector<std::string> checkDiags;
				Cst::DeriveToJob( probeDoc, *checkJob, &checkDiags );
				IObjectManager* checkMgr = checkJob->GetObjects();
				if( checkMgr ) {
					checkMgr->PrepareForRendering();
					IObjectPriv* checkCap1 = checkMgr->GetItem( "cap1" );
					if( checkCap1 ) {
						const double checkV = (double)ProximityAtMgr( checkMgr, s6World, checkCap1, Scalar( 0.02 ) );
						CheckClose( checkV, 0.0, 1e-9,
							"(b) MONEY -- the PROBE document's own derived job still reads proximity(0.02) = 0 at S6 "
							"through the query channel (the signal layer is not the source of the painter-station "
							"finding below)" );
					}
				}
				checkJob->release();
			}
			std::vector<std::string> probeDiags;
			probeOk = RenderDocument( probeDoc, probeImg, probeDiags );
			for( const std::string& d : probeDiags ) std::cout << "  probe diagnostic: " << d << std::endl;
			Check( probeDiags.empty(), "(b) the PROBE copy derives with NO diagnostics" );

			// CONTROL: expr -> vec3(1, 1, 1).
			Cst::Document controlDoc = doc;
			controlDoc = Cst::DocSetOrAddParamValue( controlDoc, rastId, "oidn_denoise", 0, "FALSE" );   // oidn_denoise FALSE
			controlDoc = Cst::DocSetOrAddParamValue( controlDoc, rastId, "samples", 0, "512" );
			controlDoc = Cst::DocSetOrAddParamValue( controlDoc, painterId, "expr", 0, "vec3(1, 1, 1)" );
			std::vector<std::string> controlDiags;
			controlOk = RenderDocument( controlDoc, controlImg, controlDiags );
			for( const std::string& d : controlDiags ) std::cout << "  control diagnostic: " << d << std::endl;
			Check( controlDiags.empty(), "(b) the CONTROL copy derives with NO diagnostics" );
		}
		Check( probeOk && controlOk, "(b) both the probe and control copies render" );

		if( probeOk && controlOk ) {
			// (x1,y1) / (x6,y6) are the RASTER indices computed above by
			// `ScreenToRaster` -- read its comment before touching them.
			const double p1 = probeImg.At( x1, y1 ), c1 = controlImg.At( x1, y1 );
			const double p6 = probeImg.At( x6, y6 ), c6 = controlImg.At( x6, y6 );

			Check( c1 > 0.0, "(b) MONEY -- S1's CONTROL pixel is non-zero (asserted before dividing)" );
			Check( c6 > 0.0, "(b) MONEY -- S6's CONTROL pixel is non-zero" );

			if( c1 > 0.0 ) {
				const double ratio1 = p1 / c1;
				std::cout << "    S1 painter ratio: probe=" << p1 << " control=" << c1 << " ratio=" << ratio1 << std::endl;
				CheckClose( ratio1, 0.5, 0.15, "(b) MONEY -- S1's painter ratio matches the predicted 0.5 within the coarse-footprint band" );
			}
			if( c6 > 0.0 ) {
				const double ratio6 = p6 / c6;
				std::cout << "    S6 painter ratio: probe=" << p6 << " control=" << c6 << " ratio=" << ratio6 << std::endl;
				// The SPEC's value (docs/PROXIMITY_SHOWCASES.md Sec 1: 0
				// within 0.08).  It reads a hard 0 rather than a
				// near-0 -- `dust` is exactly 0 across the whole
				// neighbourhood 6.5 cm from the wall, so the probe pixel is
				// exactly black and the filtered film has nothing but zeros
				// to gather from its neighbours either.  Between 2026-09-10
				// and this commit this line asserted ~0.97 instead, because
				// the readback was indexing the framebuffer with a SCREEN
				// row: see `ScreenToRaster` above.
				CheckClose( ratio6, 0.0, 0.08,
					"(b) MONEY -- S6's painter ratio matches the predicted 0 (nothing within 2 cm)" );
			}
		}
	}

	job->release();

	//------------------------------------------------------------------
	// (c) COST: live vs `def dust 0`, 64 spp, pixelpel_rasterizer.  The
	// expression VM has no constant folding, so the rest of the program
	// still executes and the delta isolates the signal call and its L1
	// memo lookup (docs/PROXIMITY_SHOWCASES.md Sec 0).
	//------------------------------------------------------------------
	{
		const std::string from = "dust proximity(0.02)";
		const std::string to   = "dust 0";
		const std::size_t pos = sceneText.find( from );
		Check( pos != std::string::npos, "(c) the 'def dust proximity(0.02)' line is found for the cost gate's text substitution" );

		std::string zeroText = sceneText;
		if( pos != std::string::npos ) zeroText.replace( pos, from.size(), to );
		Check( zeroText.find( from ) == std::string::npos,
			"(c) MONEY -- the substituted line is gone from the zeroed copy (only the `def dust` line changed)" );
		Check( zeroText.find( to ) != std::string::npos, "(c) ...replaced with `dust 0`" );

		// 2 warm-ups, discarded.
		bool ok = false;
		TimeRasterize( sceneText, ok );
		TimeRasterize( zeroText, ok );

		std::vector<double> liveTimes, zeroTimes;
		const int kPairs = 3;
		for( int i = 0; i < kPairs; ++i ) {
			bool okLive = false, okZero = false;
			const double tLive = TimeRasterize( sceneText, okLive );
			const double tZero = TimeRasterize( zeroText, okZero );
			Check( okLive && okZero, "(c) cost-gate pair " + std::to_string(i) + " both rendered" );
			if( okLive && okZero ) {
				liveTimes.push_back( tLive );
				zeroTimes.push_back( tZero );
				std::cout << "    pair " << i << ": live=" << tLive << "s  def-0=" << tZero << "s" << std::endl;
			}
		}

		if( !liveTimes.empty() ) {
			double liveSum = 0, zeroSum = 0;
			for( double v : liveTimes ) liveSum += v;
			for( double v : zeroTimes ) zeroSum += v;
			const double liveMean = liveSum / liveTimes.size();
			const double zeroMean = zeroSum / zeroTimes.size();
			const double ratio = zeroMean > 0 ? liveMean / zeroMean : -1.0;
			std::cout << "    COST: live mean=" << liveMean << "s  def-0 mean=" << zeroMean
				<< "s  ratio=" << ratio << "x (target <= 1.15x)" << std::endl;
			Check( ratio > 0 && ratio <= 1.15, "(c) live/def-0 ratio is bounded (recorded in the scene header; target <= 1.15x)" );
		}
	}

	::remove( optsPath );

	std::cout << std::endl << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
