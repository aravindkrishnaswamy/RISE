//////////////////////////////////////////////////////////////////////
//
//  ShelfBunnyShowcaseTest.cpp -- docs/PROXIMITY_SHOWCASES.md 3
//  (`shelf_bunny`): mesh-to-mesh and mesh-to-plank `proximity(0.02)`
//  contact, staged as a picture over
//  scenes/FeatureBased/Textures/shelf_bunny.RISEscene.
//
//  Follows the (g)/(l) precedents this spec names: `MeshClosestPointTest`
//  (g) for loading a tracked scene through the CST path and re-deriving
//  its placement numbers from the mesh assets themselves (never trusting
//  the scene file's own literals), and `EnvLightBalanceTest`'s capturing
//  `IRasterizerOutput` for the in-process probe/control renders.
//
//  SECTIONS:
//    (a) GEOMETRY, re-derived from the bunny/dragon assets independently
//        of the scene file: the bunny's y-offset, the dragon's onto-the-
//        bunny placement, the bunny's footprint clearance from the wall,
//        and the M3 direction (the one candidate whose minimum 3D vertex
//        distance exceeds 25 mm AND whose camera sightline is clear).
//    (b) QUERY STATIONS M1-M5 against the loaded (tracked) scene via
//        `SurfaceSignalInfo::Proximity`, `self` = the shelf for M1-M3/M5,
//        the bunny for M4.
//    (c) SHADOW RAYS + n.L at M1p/M1q/M3, cast against the loaded scene.
//    (d) PAINTER STATIONS: an in-process PROBE copy (`expr` swapped to
//        `vec3(dust,dust,dust)`) and CONTROL copy (`vec3(1,1,1)`), both
//        path-traced at 512 spp with `oidn_denoise FALSE` forced on the
//        rasterizer chunk and every non-receiver object repointed to a
//        black Lambertian, read back at the stations' projected pixels.
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
#include <filesystem>

#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/SurfaceSignalProximity.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Utilities/ExpressionMemo.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/Cameras/ThinLensCamera.h"
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

static void CheckClose( const Scalar got, const Scalar want, const Scalar tol, const std::string& name )
{
	if( std::fabs( (double)( got - want ) ) <= (double)tol ) { ++passCount; }
	else {
		++failCount;
		std::cout << "  FAIL: " << name << "  got " << (double)got
			<< " want " << (double)want << " (tol " << (double)tol << ")" << std::endl;
	}
}

//======================================================================
// Repo / asset location
//======================================================================

static fs::path FindRepoRoot()
{
	const char* candidates[] = { ".", "..", "../..", "../../.." };
	for( const char* c : candidates ) {
		const fs::path p( c );
		if( fs::exists( p / "models" / "risemesh" / "bunny.risemesh" ) ) {
			return p;
		}
	}
	return fs::path();
}

//! Same deserialize path `risemesh_geometry` uses (Job::AddRISEMeshTriangleMeshGeometry's
//! load-into-memory branch) -- see MeshClosestPointTest's twin helper.
static TriangleMeshGeometryIndexed* LoadRiseMesh( const fs::path& file )
{
	IMemoryBuffer* pBuffer = 0;
	RISE_API_CreateMemoryBufferFromFile( &pBuffer, file.string().c_str() );
	if( !pBuffer || pBuffer->Size() == 0 ) {
		if( pBuffer ) pBuffer->release();
		return 0;
	}
	TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
	mesh->addref();
	mesh->Deserialize( *pBuffer );
	pBuffer->release();
	if( mesh->numPoints() == 0 ) {
		mesh->release();
		return 0;
	}
	return mesh;
}

//======================================================================
// THE SCENE'S OWN LITERALS -- kept in lockstep with the header of
// scenes/FeatureBased/Textures/shelf_bunny.RISEscene, and re-derived from
// the assets themselves in section (a) below (never trusted blind).
//======================================================================

static const Scalar kBunnyY      = Scalar( -0.0329874 );
static const Scalar kDragonScale = Scalar( 0.35 );
static const Scalar kDragonX     = Scalar( -0.0318315 );
static const Scalar kDragonY     = Scalar( 0.135862595 );
static const Scalar kDragonZ     = Scalar( -0.014760295 );
static const Scalar kRadius      = Scalar( 0.02 );		// proximity(0.02)

// Shelf: box 0.60 x 0.025 x 0.24 at (0, -0.0125, 0) -> top y = 0,
// x in [-0.30, 0.30], z in [-0.12, 0.12].
static const Scalar kShelfXMin = Scalar( -0.30 );
static const Scalar kShelfXMax = Scalar(  0.30 );
static const Scalar kShelfZMin = Scalar( -0.12 );
static const Scalar kShelfZMax = Scalar(  0.12 );
// Wall front face.
static const Scalar kWallFrontZ = Scalar( -0.12 );

//======================================================================
// (a) GEOMETRY -- re-derived from the assets, independent of the scene
//======================================================================

struct GeometryFacts
{
	Point3	contactWorld;			// bunny's lowest vertex, in world space
	Point3	bunnyTopWorld;			// bunny's highest vertex, in world space
	BoundingBox bunnyBBoxWorld;
	Point3	m3Station;				// the DERIVED M3 direction's station point
	Scalar	m3ClearMM[4];			// measured clearances, +x -x +z -z (mm)
	int		m3DirIndex;				// 0=+x 1=-x 2=+z 3=-z
};

static bool DeriveGeometry( const fs::path& root, GeometryFacts& out )
{
	TriangleMeshGeometryIndexed* bunny  = LoadRiseMesh( root / "models" / "risemesh" / "bunny.risemesh" );
	TriangleMeshGeometryIndexed* dragon = LoadRiseMesh( root / "models" / "risemesh" / "dragon_small.risemesh" );
	Check( bunny != 0,  "(a) bunny.risemesh loads" );
	Check( dragon != 0, "(a) dragon_small.risemesh loads" );
	if( !bunny || !dragon ) {
		if( bunny ) bunny->release();
		if( dragon ) dragon->release();
		return false;
	}

	const BoundingBox bbB = bunny->GenerateBoundingBox();

	// The bunny's lowest vertex touches the shelf top (y = 0): Y_b = -bbox floor.
	const Scalar derivedBunnyY = -bbB.ll.y;
	CheckClose( derivedBunnyY, kBunnyY, Scalar( 1e-6 ),
		"(a) MONEY: bunny y-offset re-derived from the ASSET matches the scene header's number" );

	// Extreme VERTICES (not bbox corners -- a bbox corner is not a point ON the mesh).
	Point3 bLowest( 0, RISE_INFINITY, 0 ), bHighest( 0, -RISE_INFINITY, 0 );
	{
		const VerticesListType& vs = bunny->getVertices();
		for( std::size_t i = 0; i < vs.size(); ++i ) {
			if( vs[i].y < bLowest.y )  bLowest  = vs[i];
			if( vs[i].y > bHighest.y ) bHighest = vs[i];
		}
	}
	out.contactWorld = Point3( bLowest.x, bLowest.y + kBunnyY, bLowest.z );
	CheckClose( out.contactWorld.y, Scalar( 0 ), Scalar( 1e-12 ),
		"(a) ...and that vertex lands EXACTLY on the shelf top y = 0" );

	out.bunnyTopWorld = Point3( bHighest.x, bHighest.y + kBunnyY, bHighest.z );

	// Dragon: lowest vertex lands on the bunny's highest, in world space.
	Point3 dLowest( 0, RISE_INFINITY, 0 );
	{
		const VerticesListType& vs = dragon->getVertices();
		for( std::size_t i = 0; i < vs.size(); ++i ) if( vs[i].y < dLowest.y ) dLowest = vs[i];
	}
	const Scalar derivedDragonX = out.bunnyTopWorld.x - kDragonScale * dLowest.x;
	const Scalar derivedDragonY = out.bunnyTopWorld.y - kDragonScale * dLowest.y;
	const Scalar derivedDragonZ = out.bunnyTopWorld.z - kDragonScale * dLowest.z;
	CheckClose( derivedDragonX, kDragonX, Scalar( 1e-6 ), "(a) MONEY: dragon x re-derived from the ASSETS matches the header" );
	CheckClose( derivedDragonY, kDragonY, Scalar( 1e-6 ), "(a) MONEY: ...its y too (lowest vertex on the bunny's highest)" );
	CheckClose( derivedDragonZ, kDragonZ, Scalar( 1e-6 ), "(a) MONEY: ...and its z" );

	// Bunny world bounding box: inside the shelf, and > 2 cm clear of the wall.
	out.bunnyBBoxWorld = BoundingBox(
		Point3( bbB.ll.x, bbB.ll.y + kBunnyY, bbB.ll.z ),
		Point3( bbB.ur.x, bbB.ur.y + kBunnyY, bbB.ur.z ) );
	Check( out.bunnyBBoxWorld.ll.x >= kShelfXMin && out.bunnyBBoxWorld.ur.x <= kShelfXMax,
		"(a) MONEY: the bunny's world bbox X stays inside the shelf's extents" );
	Check( out.bunnyBBoxWorld.ll.z >= kShelfZMin && out.bunnyBBoxWorld.ur.z <= kShelfZMax,
		"(a) MONEY: the bunny's world bbox Z stays inside the shelf's extents" );
	const Scalar wallClearance = out.bunnyBBoxWorld.ll.z - kWallFrontZ;
	std::cout << "  (a) bunny-to-wall clearance (world bbox min z - wall front z) = "
		<< (double)wallClearance << " m" << std::endl;
	Check( wallClearance > Scalar( 0.02 ),
		"(a) MONEY: the bunny's footprint is > 2 cm clear of the wall" );

	// M3: 5 cm from the contact vertex in each of +-x, +-z, minimum 3D distance
	// to any BUNNY vertex, brute force over the vertex array (a triangle
	// interior can be nearer than any vertex, but the vertex sweep is the
	// margin the spec uses as its guard).
	const Scalar r5cm = Scalar( 0.05 );
	Point3 candidates[4] = {
		Point3( out.contactWorld.x + r5cm, 0, out.contactWorld.z ),
		Point3( out.contactWorld.x - r5cm, 0, out.contactWorld.z ),
		Point3( out.contactWorld.x, 0, out.contactWorld.z + r5cm ),
		Point3( out.contactWorld.x, 0, out.contactWorld.z - r5cm ) };
	const char* candNames[4] = { "+x", "-x", "+z", "-z" };
	{
		const VerticesListType& vs = bunny->getVertices();
		for( int d = 0; d < 4; ++d ) {
			double best = 1e18;
			for( std::size_t i = 0; i < vs.size(); ++i ) {
				const Point3 w( vs[i].x, vs[i].y + kBunnyY, vs[i].z );
				const double dx = (double)( w.x - candidates[d].x );
				const double dy = (double)( w.y - candidates[d].y );
				const double dz = (double)( w.z - candidates[d].z );
				const double dist = std::sqrt( dx*dx + dy*dy + dz*dz );
				if( dist < best ) best = dist;
			}
			out.m3ClearMM[d] = Scalar( best * 1000.0 );
			std::cout << "  (a) M3 candidate " << candNames[d] << ": min vertex distance = "
				<< best * 1000.0 << " mm" << std::endl;
		}
	}
	// Exactly one candidate clears 25 mm in this scene (asserted, not assumed):
	// the spec's prediction is -x at ~33 mm, the rest well under.
	int clearCount = 0, clearIdx = -1;
	for( int d = 0; d < 4; ++d ) {
		if( out.m3ClearMM[d] > Scalar( 25.0 ) ) { ++clearCount; clearIdx = d; }
	}
	Check( clearCount == 1, "(a) MONEY: exactly one M3 candidate clears the 25 mm vertex-distance guard" );
	out.m3DirIndex = clearIdx;
	if( clearIdx >= 0 ) out.m3Station = candidates[clearIdx];

	bunny->release();
	dragon->release();
	return true;
}

//======================================================================
// SCENE LOADING (the tracked file, via the canonical CST path -- as
// MeshClosestPointTest's LoadSceneD does).
//======================================================================

struct SceneHandle
{
	Job*			job = 0;
	IObjectManager*	mgr = 0;
	IObjectPriv*	shelf = 0;
	IObjectPriv*	bunny = 0;
	IObjectPriv*	dragon = 0;
	IObjectPriv*	wall = 0;
};

static std::string ReadFile( const fs::path& p )
{
	std::ifstream in( p );
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

static bool LoadTrackedScene( const fs::path& root, SceneHandle& out )
{
	const fs::path scenePath = root / "scenes" / "FeatureBased" / "Textures" / "shelf_bunny.RISEscene";
	const std::string text = ReadFile( scenePath );
	if( text.empty() ) return false;

	Cst::Document doc = Cst::ParseToCst( text );
	out.job = new Job();
	std::vector<std::string> diags;
	Cst::DeriveToJob( doc, *out.job, &diags );
	for( std::size_t i = 0; i < diags.size(); ++i ) std::cout << "  scene diagnostic: " << diags[i] << std::endl;
	Check( diags.empty(), "(b) tracked scene derives with NO diagnostics" );

	out.mgr = out.job->GetObjects();
	if( !out.mgr ) return false;
	out.mgr->PrepareForRendering();
	out.shelf  = out.mgr->GetItem( "shelf" );
	out.bunny  = out.mgr->GetItem( "bunny" );
	out.dragon = out.mgr->GetItem( "dragon" );
	out.wall   = out.mgr->GetItem( "wall" );
	return out.shelf && out.bunny && out.dragon && out.wall;
}

static Scalar ProxAt( const SceneHandle& s, const Point3& p, const IObjectPriv* self, const Scalar r )
{
	SurfaceSignalInfo si;
	si.pScene  = s.mgr;
	si.pSelf   = self;
	si.ptWorld = p;
	ExpressionMemo::Invalidate();
	return si.Proximity( r );
}

//======================================================================
// (c) OCCLUSION -- the single-cast protocol (docs/PROXIMITY_SHOWCASES.md
// 0): a FRESH RayIntersection per cast, VISIBLE iff no hit or the hit's
// range >= |station - origin| - 1e-6.
//======================================================================

static bool IsVisible( const IObjectManager* mgr, const Point3& origin, const Point3& station, const IObject** outOccluder )
{
	const Vector3 u = Vector3Ops::Normalize( Vector3Ops::mkVector3( station, origin ) );
	RayIntersection ri( Ray( origin, u ), nullRasterizerState );
	mgr->IntersectRay( ri, true, true, false );
	const Scalar dist = Point3Ops::Distance( station, origin );
	if( outOccluder ) *outOccluder = ri.pObject;
	if( !ri.pObject ) return true;
	return ri.geometric.range >= dist - Scalar( 1e-6 );
}

//======================================================================
// (d) PAINTER STATIONS -- capturing output + CST probe/control renders.
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

//! Find the first top-level Chunk item in a freshly-parsed one-chunk
//! Document -- the same idiom Job::ApplyCstInsertCameraChunk uses to pull
//! a NodeRef out of a hand-built chunk-text Document.
static Cst::NodeRef FirstChunkItem( const Cst::Document& d )
{
	const int n = Cst::DocItemCount( d );
	for( int i = 0; i < n; ++i ) {
		const Cst::NodeRef it = Cst::DocResolveNodeId( d, Cst::DocNodeIdAt( d, i ) );
		if( it && it->kind == Cst::NodeKind::Chunk ) return it;
	}
	return Cst::NodeRef();
}

static Cst::NodeRef NewlineLeaf()
{
	Cst::Document d = Cst::ParseToCst( std::string( "\n" ) );
	return Cst::DocResolveNodeId( d, Cst::DocNodeIdAt( d, 0 ) );
}

//! Build one probe/control copy of the tracked scene's Document: swap
//! p_shelf's `expr`, black out every non-receiver OBJECT's `material`
//! (wall, bunny, dragon -- the shelf IS the receiver here and is never
//! blacked), force `oidn_denoise FALSE` + `samples 512` on the
//! rasterizer chunk.  Mirrors docs/PROXIMITY_SHOWCASES.md 0's recipe:
//! the inserted black material's three-leaf splice
//! ([leadSep][chunk][trailSep]) goes BEFORE the first object chunk
//! (`standard_object/shelf`) so it derives before anything names it.
static Cst::Document BuildProbeDocument( const Cst::Document& baseDoc, const std::string& exprValue )
{
	Cst::Document doc = baseDoc;

	// 1. Insert the shared black material before the FIRST object chunk.
	const Cst::NodeId shelfObjId = Cst::DocFindByName( doc, "standard_object/shelf" );
	Check( shelfObjId > 0, "(d) DocFindByName(standard_object/shelf) resolves" );
	Cst::NodeRef shelfObjItem;
	const int shelfIdx = Cst::DocIndexOfNodeId( doc, shelfObjId, &shelfObjItem );
	Check( shelfIdx >= 0, "(d) DocIndexOfNodeId(shelf) resolves" );

	Cst::Document blackChunkDoc = Cst::ParseToCst( std::string(
		"lambertian_material\n{\nname\tprobe_black\n}\n" ) );
	Cst::NodeRef blackChunkItem = FirstChunkItem( blackChunkDoc );
	Check( (bool)blackChunkItem, "(d) the black material chunk text parses" );
	Cst::NodeRef lead = NewlineLeaf();
	Cst::NodeRef trail = NewlineLeaf();

	doc = Cst::DocInsertItem( doc, shelfIdx,     lead );
	doc = Cst::DocInsertItem( doc, shelfIdx + 1, blackChunkItem );
	doc = Cst::DocInsertItem( doc, shelfIdx + 2, trail );

	// 2. Repoint wall / bunny / dragon's `material` to the black one.
	//    NodeId-based setters -- unaffected by the index shift above.
	for( const char* objName : { "wall", "bunny", "dragon" } ) {
		const std::string path = std::string( "standard_object/" ) + objName;
		const Cst::NodeId id = Cst::DocFindByName( doc, path );
		Check( id > 0, std::string( "(d) DocFindByName(" ) + path + ") resolves" );
		doc = Cst::DocSetOrAddParamValue( doc, id, "material", 0, "probe_black" );
	}

	// 3. Swap p_shelf's `expr`.
	{
		const Cst::NodeId id = Cst::DocFindByName( doc, "expression_painter/p_shelf" );
		Check( id > 0, "(d) DocFindByName(expression_painter/p_shelf) resolves" );
		doc = Cst::DocSetOrAddParamValue( doc, id, "expr", 0, exprValue );
	}

	// 4. Rasterizer chunk is UNNAMED -- find it by role (ends in "_rasterizer").
	{
		Cst::NodeId rastId = 0;
		const int n = Cst::DocItemCount( doc );
		for( int i = 0; i < n; ++i ) {
			const Cst::NodeId nid = Cst::DocNodeIdAt( doc, i );
			const Cst::NodeRef it = Cst::DocResolveNodeId( doc, nid );
			if( !it || it->kind != Cst::NodeKind::Chunk ) continue;
			const std::string& role = it->role;
			if( role.size() > 11 && role.compare( role.size() - 11, 11, "_rasterizer" ) == 0 ) {
				rastId = nid;
				break;
			}
		}
		Check( rastId > 0, "(d) the rasterizer chunk is found by role" );
		// oidn_denoise FALSE forced on both probe copies (SourceHygieneTest
		// looks for this literal contiguous substring in the file):
		// oidn_denoise FALSE
		doc = Cst::DocSetOrAddParamValue( doc, rastId, "oidn_denoise", 0, "FALSE" );
		doc = Cst::DocSetOrAddParamValue( doc, rastId, "samples", 0, "512" );
	}

	return doc;
}

struct RenderResult
{
	bool ok = false;
	unsigned int width = 0, height = 0;
	std::vector<RISEColor> pixels;
	Point3 cameraLocation;
	Job* job = 0;			// caller releases
};

static RenderResult RenderDocument( const Cst::Document& doc )
{
	RenderResult r;
	r.job = new Job();
	std::vector<std::string> diags;
	Cst::DeriveToJob( doc, *r.job, &diags );
	for( std::size_t i = 0; i < diags.size(); ++i ) std::cout << "  probe diagnostic: " << diags[i] << std::endl;
	Check( diags.empty(), "(d) probe/control copy derives with NO diagnostics" );
	if( !diags.empty() ) return r;

	r.job->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
	cap->addref();
	r.job->GetRasterizer()->AddRasterizerOutput( cap );

	const bool rendered = r.job->Rasterize();
	Check( rendered, "(d) probe/control copy renders" );

	r.width  = cap->width;
	r.height = cap->height;
	r.pixels = cap->pixels;
	if( r.job->GetScene() && r.job->GetScene()->GetCamera() ) {
		r.cameraLocation = r.job->GetScene()->GetCamera()->GetLocation();
	}
	r.ok = rendered;
	cap->release();
	return r;
}

//! Invert the camera's pixel->direction mapping numerically (2D Newton on
//! the 3D direction residual), rather than hand-deriving filmDistance /
//! sensor-scale analytically -- this calls the SAME GenerateRayWithLensSample
//! the renderer itself uses (lens sample fixed at the disk centre = the
//! deterministic pinhole chief ray), so it cannot disagree with what was
//! actually rendered.
static bool ProjectToPixel( const ThinLensCamera* cam, unsigned int width, unsigned int height,
	const Point3& worldStation, double& outPx, double& outPy )
{
	if( !cam ) return false;
	RandomNumberGenerator rng( 1u );
	RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
	const Point2 lensCenter( 0.5, 0.5 );

	auto dirAt = [&]( double px, double py ) -> Vector3 {
		Ray r;
		cam->GenerateRayWithLensSample( rc, r, Point2( px, py ), lensCenter );
		return Vector3Ops::Normalize( r.Dir() );
	};

	const Vector3 target = Vector3Ops::Normalize( Vector3Ops::mkVector3( worldStation, cam->GetLocation() ) );

	double px = width * 0.5, py = height * 0.5;
	const double eps = 1.0;	// finite-difference step, in pixels
	for( int iter = 0; iter < 6; ++iter ) {
		const Vector3 f0 = dirAt( px, py );
		const Vector3 residual = target - f0;
		const Vector3 dfdx = ( dirAt( px + eps, py ) - dirAt( px - eps, py ) ) * ( 1.0 / ( 2.0 * eps ) );
		const Vector3 dfdy = ( dirAt( px, py + eps ) - dirAt( px, py - eps ) ) * ( 1.0 / ( 2.0 * eps ) );

		const double a11 = Vector3Ops::Dot( dfdx, dfdx );
		const double a12 = Vector3Ops::Dot( dfdx, dfdy );
		const double a22 = Vector3Ops::Dot( dfdy, dfdy );
		const double b1  = Vector3Ops::Dot( dfdx, residual );
		const double b2  = Vector3Ops::Dot( dfdy, residual );

		const double det = a11 * a22 - a12 * a12;
		if( std::fabs( det ) < 1e-18 ) break;
		const double dpx = (  a22 * b1 - a12 * b2 ) / det;
		const double dpy = ( -a12 * b1 + a11 * b2 ) / det;
		px += dpx;
		py += dpy;
		if( std::fabs( dpx ) < 1e-6 && std::fabs( dpy ) < 1e-6 ) break;
	}
	// GenerateRay's own pixel space has y increasing UPWARD in world terms
	// (a HIGHER world point converges to a LARGER film-space py here,
	// empirically verified: y=0.01 -> py~203, y=0.15 -> py~494), while the
	// captured image array (`IRasterImage::GetPEL`, the same order the PNG
	// encoder writes) is row-0-at-the-TOP like every raster convention --
	// so the row index for ARRAY LOOKUP is the vertical flip of the film
	// coordinate this function's Newton solve converges on.
	outPx = px;
	outPy = (double)height - py;
	return true;
}

static bool SamplePixel( const RenderResult& r, double px, double py, double& outR, double& outG, double& outB )
{
	const long ix = (long)std::lround( px );
	const long iy = (long)std::lround( py );
	if( ix < 0 || iy < 0 || (unsigned long)ix >= r.width || (unsigned long)iy >= r.height ) return false;
	const RISEColor& c = r.pixels[ (unsigned long)iy * r.width + (unsigned long)ix ];
	outR = (double)c.base.r; outG = (double)c.base.g; outB = (double)c.base.b;
	return true;
}

//======================================================================

int main( int, char** )
{
	std::cout << "ShelfBunnyShowcaseTest -- docs/PROXIMITY_SHOWCASES.md 3 (shelf_bunny)" << std::endl;

	const fs::path root = FindRepoRoot();
	Check( !root.empty(), "repo root found (models/risemesh/bunny.risemesh reachable)" );
	if( root.empty() ) {
		std::cout << "TOTAL: " << passCount << " passed, " << failCount << " failed" << std::endl;
		return failCount == 0 ? 0 : 1;
	}

	//------------------------------------------------------------
	// (a) Geometry, re-derived from the assets.
	//------------------------------------------------------------
	GeometryFacts geo;
	const bool geoOk = DeriveGeometry( root, geo );
	Check( geoOk, "(a) geometry derivation completed" );

	//------------------------------------------------------------
	// (b) Load the tracked scene; query stations.
	//------------------------------------------------------------
	SceneHandle scene;
	const bool sceneOk = LoadTrackedScene( root, scene );
	Check( sceneOk, "(b) tracked scene loads and exposes shelf/bunny/dragon/wall" );

	Point3 m1p, m1q, m3;
	if( geoOk && sceneOk ) {
		// M1: shelf top, 1 mm outside the contact vertex's xz, four directions.
		const Scalar mm = Scalar( 0.001 );
		const Vector3 outs[4] = { Vector3(1,0,0), Vector3(-1,0,0), Vector3(0,0,1), Vector3(0,0,-1) };
		const char* dirNames[4] = { "+x", "-x", "+z", "-z" };
		Scalar worstM1 = Scalar( 1 );
		for( int i = 0; i < 4; ++i ) {
			const Point3 p( geo.contactWorld.x + outs[i].x * mm, Scalar(0), geo.contactWorld.z + outs[i].z * mm );
			const Scalar v = ProxAt( scene, p, scene.shelf, kRadius );
			std::cout << "  M1 " << dirNames[i] << " 1mm: " << (double)v << std::endl;
			if( v < worstM1 ) worstM1 = v;
		}
		Check( worstM1 >= Scalar( 0.9 ), "M1 MONEY: shelf reads >= 0.9 at 1 mm outside the bunny's contact vertex" );

		// M2: same four directions at 2 mm.
		Scalar worstM2 = Scalar( 1 );
		for( int i = 0; i < 4; ++i ) {
			const Point3 p( geo.contactWorld.x + outs[i].x * 2 * mm, Scalar(0), geo.contactWorld.z + outs[i].z * 2 * mm );
			const Scalar v = ProxAt( scene, p, scene.shelf, kRadius );
			if( v < worstM2 ) worstM2 = v;
		}
		std::cout << "  M2 worst of four at 2mm: " << (double)worstM2 << std::endl;
		Check( worstM2 >= Scalar( 0.9 ), "M2 MONEY: shelf reads >= 0.9 at 2 mm outside" );

		// M3: the derived direction, 5 cm out -- predicted 0 (outside the 2 cm radius).
		Check( geo.m3DirIndex >= 0, "M3 direction was derived" );
		if( geo.m3DirIndex >= 0 ) {
			m3 = geo.m3Station;
			const Scalar v = ProxAt( scene, m3, scene.shelf, kRadius );
			std::cout << "  M3 query (5cm, chosen direction): " << (double)v << std::endl;
			Check( v == Scalar( 0 ), "M3 MONEY: shelf reads exactly 0 at 5 cm (outside the 2 cm radius)" );
			// The chosen direction's camera sightline must ALSO be clear (the
			// spec's second half of the direction-derivation rule) -- checked
			// below in section (c), once the camera location is in scope.
		}

		// M4: bunny's highest vertex (self = bunny) -- the dragon sits there, distance 0.
		{
			const Scalar top = ProxAt( scene, geo.bunnyTopWorld, scene.bunny, kRadius );
			std::cout << "  M4 at the shared vertex (self=bunny): " << (double)top << std::endl;
			Check( top >= Scalar( 0.999 ), "M4 MONEY: at the shared vertex, self=bunny reads the dragon at distance 0" );

			const Point3 below( geo.bunnyTopWorld.x, geo.bunnyTopWorld.y - Scalar( 0.005 ), geo.bunnyTopWorld.z );
			const Scalar belowV = ProxAt( scene, below, scene.bunny, kRadius );
			std::cout << "  M4 5mm below (self=bunny): " << (double)belowV << "  (closed form 0.75)" << std::endl;
			CheckClose( belowV, Scalar( 0.75 ), Scalar( 1e-4 ),
				"M4 MONEY: 5mm below the shared vertex reads the dragon's own lowest vertex, closed form 0.75" );
		}

		// M5: shelf top near the wall -- box neighbour, exact closed form.
		{
			const Point3 pNear( Scalar( 0.20 ), Scalar( 0 ), Scalar( -0.11 ) );
			const Scalar vNear = ProxAt( scene, pNear, scene.shelf, kRadius );
			std::cout << "  M5 z=-0.11 (1cm from wall): " << (double)vNear << "  (closed form 0.5)" << std::endl;
			CheckClose( vNear, Scalar( 0.5 ), Scalar( 1e-9 ), "M5 MONEY: 1cm from the wall reads EXACTLY 0.5 (box neighbour, exact)" );

			const Point3 pFar( Scalar( 0.20 ), Scalar( 0 ), Scalar( -0.09 ) );
			const Scalar vFar = ProxAt( scene, pFar, scene.shelf, kRadius );
			std::cout << "  M5 z=-0.09 (3cm from wall): " << (double)vFar << std::endl;
			Check( vFar == Scalar( 0 ), "M5 ...and EXACTLY 0 at 3 cm (outside the radius)" );
		}

		// Painter-station points (query values recorded for the ratio comparison below).
		m1p = Point3( geo.contactWorld.x - Scalar( 0.028 ), Scalar(0), geo.contactWorld.z );
		m1q = Point3( geo.contactWorld.x, Scalar(0), geo.contactWorld.z + Scalar( 0.030 ) );
	}

	const Scalar m1pQuery = sceneOk ? ProxAt( scene, m1p, scene.shelf, kRadius ) : Scalar( 0 );
	const Scalar m1qQuery = sceneOk ? ProxAt( scene, m1q, scene.shelf, kRadius ) : Scalar( 0 );
	const Scalar m3Query  = ( sceneOk && geo.m3DirIndex >= 0 ) ? ProxAt( scene, m3, scene.shelf, kRadius ) : Scalar( 0 );
	std::cout << "  M1p query = " << (double)m1pQuery << "   M1q query = " << (double)m1qQuery
		<< "   M3 query = " << (double)m3Query << std::endl;

	//------------------------------------------------------------
	// (c) Shadow rays + n.L, and camera sightlines, against the loaded scene.
	//------------------------------------------------------------
	if( sceneOk ) {
		const Point3 keyPos( -0.35, 0.45, 0.25 );
		const Point3 stations[3]  = { m1p, m1q, m3 };
		const char*  stationNames[3] = { "M1p", "M1q", "M3" };
		for( int i = 0; i < 3; ++i ) {
			const IObject* occluder = 0;
			const bool visible = IsVisible( scene.mgr, stations[i], keyPos, &occluder );
			Check( visible, std::string( "shadow ray to the key from " ) + stationNames[i] + " is clear" );
			const Vector3 toLight = Vector3Ops::Normalize( Vector3Ops::mkVector3( keyPos, stations[i] ) );
			const Scalar nDotL = toLight.y;	// shelf top normal is (0,1,0)
			std::cout << "  n.L at " << stationNames[i] << " = " << (double)nDotL << std::endl;
			Check( nDotL > Scalar( 0 ), std::string( "n.L at " ) + stationNames[i] + " is positive (lit)" );
		}

		// The camera sightline to M1p/M1q/M3 -- required for the painter
		// stations to be valid reads (and to confirm the M3 direction's
		// second clause: an unoccluded sightline).
		const Point3 camLoc( 0.22, 0.25, 0.42 );
		for( int i = 0; i < 3; ++i ) {
			const IObject* occluder = 0;
			const bool visible = IsVisible( scene.mgr, camLoc, stations[i], &occluder );
			Check( visible, std::string( "camera sightline to " ) + stationNames[i] + " is clear" );
		}
	}

	//------------------------------------------------------------
	// (d) Painter stations: probe / control renders.
	//------------------------------------------------------------
	if( sceneOk ) {
		const fs::path scenePath = root / "scenes" / "FeatureBased" / "Textures" / "shelf_bunny.RISEscene";
		const std::string text = ReadFile( scenePath );
		Cst::Document baseDoc = Cst::ParseToCst( text );

		Cst::Document probeDoc   = BuildProbeDocument( baseDoc, "vec3(dust, dust, dust)" );
		Cst::Document controlDoc = BuildProbeDocument( baseDoc, "vec3(1, 1, 1)" );

		RenderResult probe   = RenderDocument( probeDoc );
		RenderResult control = RenderDocument( controlDoc );
		Check( probe.ok && control.ok, "(d) both probe and control copies rendered" );

		if( probe.ok && control.ok ) {
			const ThinLensCamera* cam = dynamic_cast<const ThinLensCamera*>(
				control.job->GetScene() ? control.job->GetScene()->GetCamera() : 0 );
			Check( cam != 0, "(d) the beauty camera resolves as a ThinLensCamera" );

			struct Station { const char* name; Point3 p; Scalar queryVal; Scalar tol; };
			Station stations[3] = {
				{ "M1p", m1p, m1pQuery, Scalar( 0.15 ) },
				{ "M1q", m1q, m1qQuery, Scalar( 0.15 ) },
				{ "M3",  m3,  m3Query,  Scalar( 0.08 ) },
			};

			for( int i = 0; i < 3; ++i ) {
				if( cam == 0 ) break;
				double px = 0, py = 0;
				ProjectToPixel( cam, control.width, control.height, stations[i].p, px, py );
				std::cout << "  " << stations[i].name << " projects to pixel (" << px << ", " << py << ")" << std::endl;

				double pr=0,pg=0,pb=0, cr=0,cg=0,cb=0;
				const bool gotP = SamplePixel( probe,   px, py, pr, pg, pb );
				const bool gotC = SamplePixel( control, px, py, cr, cg, cb );
				Check( gotP && gotC, std::string( "(d) " ) + stations[i].name + " projects inside the frame" );
				if( gotP && gotC ) {
					const double probeMean   = ( pr + pg + pb ) / 3.0;
					const double controlMean = ( cr + cg + cb ) / 3.0;
					Check( controlMean > 0.0, std::string( "(d) " ) + stations[i].name + " control pixel is non-zero" );
					if( controlMean > 0.0 ) {
						const double ratio = probeMean / controlMean;
						std::cout << "  " << stations[i].name << " rendered ratio = " << ratio
							<< "   query = " << (double)stations[i].queryVal << std::endl;
						CheckClose( Scalar( ratio ), stations[i].queryVal, stations[i].tol,
							std::string( "(d) MONEY: " ) + stations[i].name + " rendered ratio matches the query value" );
					}
				}
			}
		}
		if( probe.job )   probe.job->release();
		if( control.job ) control.job->release();
	}

	if( scene.job ) scene.job->release();

	std::cout << "TOTAL: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
