//////////////////////////////////////////////////////////////////////
//
//  ObjectMirrorTest.cpp - doc 89 slice C: `mirror x|y|z` on `standard_object`.
//
//  A mirror REFLECTS a node across the plane through its OWN local origin
//  perpendicular to the named axis, composed INNERMOST:
//
//      world = parent.world * Position * Orientation * Stretch * Scale * Mirror
//
//  Innermost is the whole design.  It reflects the SHAPE in its own frame and
//  then places the reflected shape by the node's own transform -- so
//  `mirror x  position 3 0 0` puts the reflected copy AT +3 rather than moving
//  the un-reflected one to -3 -- and it means everything parented under a
//  mirrored node composes through the reflection exactly once.  That is the
//  headline use: `standard_object { name right_wing  source left_wing  mirror x
//  position ... }` reflects the whole cloned subtree.
//
//  WHAT THIS FILE PINS, and why each block is here rather than assumed:
//
//  [A matrix]     the composed world matrix equals the hand-composed product
//                 above with ZERO error -- built here from first principles
//                 out of Matrix4Ops, not read back from the thing under test.
//                 A wrong COMPOSITION POSITION (mirror outside the rotation,
//                 or on the transform stack, which left-multiplies) changes
//                 this product and nothing else in the suite would notice.
//
//  [B twin]       a mirrored TRIANGLE MESH shades identically to a
//                 HAND-REFLECTED twin mesh -- vertices reflected and winding
//                 fixed -- probed by real rays through Object::IntersectRay.
//                 Hit point, GEOMETRIC normal and SHADING normal must agree.
//                 Both meshes are built single-sided (`double_sided false`),
//                 so a handedness defect does not merely shade wrong: the
//                 probe MISSES, because a front-faces-only test rejects the
//                 face.  This is the block the "does winding need flipping?"
//                 question is answered by, and it answers it empirically
//                 rather than from the argument in the report.
//
//  [C emitter]    a mirrored EMITTER lights correctly.  Not `GetArea()` alone:
//                 the two calls the luminaire sampler actually makes are
//                 `GetArea()` (the pdfPosition denominator) and
//                 `UniformRandomPoint()` (the sample + its normal), and BOTH
//                 are checked against the hand-reflected twin, over a spread
//                 of sample points, with the normals additionally required to
//                 point OUTWARD.  `Object::GetArea` folds in |det|^(2/3) --
//                 the 2026-08-13 world-area Jacobian fix -- and a reflection
//                 has det < 0, so an unsigned `det^(2/3)` there would be NaN
//                 and a mirrored wing would light nothing at all.
//
//  [D scene]      the same, through the CST derive: a leaf mirror; `source` +
//                 `mirror` reflecting a whole SUBTREE with the grandchild
//                 reflected exactly ONCE; `count_u` + `mirror` mirroring every
//                 repetition; `mirror` on a geometry-less CONTAINER (children
//                 compose through it); and -- the IsInstanceOwnParam contract
//                 -- a `source` whose SOURCE carries a mirror does NOT inherit
//                 it, so an un-mirrored instance of a mirrored node comes out
//                 un-mirrored and a mirrored instance of one is not reflected
//                 twice.
//
//  [E refuse]     an axis that is not x / y / z is refused by NAME, changing
//                 nothing -- `mirror w`, and `mirror X`, since the descriptor
//                 advertises lower case only (the `lathe_geometry` axis
//                 precedent) and accepting a spelling the schema does not
//                 publish would let the two drift.
//
//  STYLE: a counted Check() + a non-zero exit, following CstSourceInstanceTest
//  and CSGObjectIdentityTest.  Deliberately NOT `assert`: MSVC Release carries
//  /DNDEBUG and run_all_tests.ps1 defaults to Release, where an inverted
//  assertion still prints "Passed!" and exits 0.
//
//////////////////////////////////////////////////////////////////////

#include "CstRenderEquivalence.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Interfaces/ITriangleMeshGeometry.h"
#include "../src/Library/Modifiers/NormalMap.h"                    // [B3]: the derivative-fallback tangent frame
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Interfaces/ILogPriv.h"                  // [D6]: capture the derive-time advisory
#include "../src/Library/SceneEditor/ChunkDescriptorRegistry.h" // [E]: DescriptorForKeyword (the published enum set)
#include "../src/Library/SceneEditor/ObjectIntrospection.h"      // [H]: the properties-panel read path
#include "../src/Library/SceneEditor/SceneEditController.h"   // [G]: the gizmo commit path
#include "../src/Library/SceneEditor/SceneEditor.h"           // [G]: Apply/Undo of a hand-built gizmo edit
#include "../src/Library/Utilities/Transformable.h"

#include <cmath>
#include <fstream>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::Cst;
using namespace risequiv;

// A minimal ILogPrinter that records every message containing `needle`.  Same shape
// (and the same reasoning) as CstSourceInstanceTest's: the derive-time `source` +
// `mirror` advisory is a WARNING, not a diagnostic -- both readings of that scene are
// legal, so it must not fail the derive -- and a warning is invisible to the `diags`
// bag every other assertion in this file reads.  Installed once and never removed;
// RemoveAllPrinters would also kill the default printers for everything after it.
class CapturingLogPrinter : public virtual RISE::ILogPrinter, public virtual RISE::Implementation::Reference
{
public:
	explicit CapturingLogPrinter( std::string needle ) : mNeedle( std::move( needle ) ) {}

	void Print( const RISE::LogEvent& event ) override
	{
		const std::string msg( event.szMessage );
		if( msg.find( mNeedle ) != std::string::npos ) {
			std::lock_guard<std::mutex> lk( mMutex );
			mMatches.push_back( msg );
		}
	}
	void Flush() override {}

	int MatchCount() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return static_cast<int>( mMatches.size() );
	}
	std::string LastMatch() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return mMatches.empty() ? std::string() : mMatches.back();
	}

protected:
	~CapturingLogPrinter() override {}

private:
	std::string                mNeedle;
	mutable std::mutex         mMutex;
	std::vector<std::string>   mMatches;
};

// The instancing advisory's capture point, installed in main() before any derive runs.
static CapturingLogPrinter* g_mirrorSourceWarn = 0;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

// The reflection matrix for a local axis, built HERE rather than read off the
// object under test -- an oracle that asked the implementation what it did
// would agree with any answer it gave.
static Matrix4 MirrorMx( int axis )
{
	Matrix4 m = Matrix4Ops::Identity();
	if( axis == 0 )      m._00 = -1;
	else if( axis == 1 ) m._11 = -1;
	else if( axis == 2 ) m._22 = -1;
	return m;
}

// Entry-by-entry matrix comparison.  `tol == 0` means EXACT: the expected
// product below is formed from the same Matrix4Ops multiplies in the same
// order, so any difference at all is a difference in the composition, not
// in floating point.
static bool MxEq( const Matrix4& a, const Matrix4& b, double tol, std::string* outWhere = nullptr )
{
	const Scalar* pa = &a._00;
	const Scalar* pb = &b._00;
	for( int k = 0; k < 16; ++k ) {
		const double d = std::fabs( (double)pa[k] - (double)pb[k] );
		if( !( d <= tol ) ) {
			if( outWhere ) {
				char buf[128];
				std::snprintf( buf, sizeof(buf), " (entry %d: got %.17g, want %.17g)", k, (double)pa[k], (double)pb[k] );
				*outWhere = buf;
			}
			return false;
		}
	}
	return true;
}

static bool VecClose( const Vector3& a, const Vector3& b, double tol )
{
	return std::fabs( a.x - b.x ) <= tol && std::fabs( a.y - b.y ) <= tol && std::fabs( a.z - b.z ) <= tol;
}

static bool PtClose( const Point3& a, const Point3& b, double tol )
{
	return std::fabs( a.x - b.x ) <= tol && std::fabs( a.y - b.y ) <= tol && std::fabs( a.z - b.z ) <= tol;
}

// One properties-panel row by name.  Returns false when the row is ABSENT, which
// is a different failure from a row that is present and wrong -- the descriptor
// drives the row list, so a missing row means the param stopped being published.
static bool RowValue( const std::vector<CameraProperty>& rows, const char* name, String& out )
{
	for( std::size_t i = 0; i < rows.size(); ++i ) {
		if( rows[i].name == String( name ) ) { out = rows[i].value; return true; }
	}
	return false;
}

//////////////////////////////////////////////////////////////////////
// [A] WORLD-MATRIX EXACTNESS
//////////////////////////////////////////////////////////////////////

static void PartA_WorldMatrixExactness()
{
	std::printf( "=== [A] the composed world matrix IS P * O * Stretch * Scale * Mirror ===\n" );

	Implementation::SphereGeometry* g = new Implementation::SphereGeometry( 1.0 );

	const double rx = 0.37, ry = -1.11, rz = 0.53;      // radians; deliberately not axis-aligned
	const Point3  pos( 3.0, 1.0, -2.0 );
	const Vector3 stretch( 2.0, 0.5, 1.25 );

	for( int axis = 0; axis < 3; ++axis ) {
		Implementation::Object* o = new Implementation::Object( g );
		o->SetPosition( pos );
		o->SetOrientation( Vector3( rx, ry, rz ) );
		o->SetStretch( stretch );
		Check( o->SetMirrorAxis( axis ), "A: SetMirrorAxis accepts a legal axis" );
		o->FinalizeTransformations();

		// HAND-COMPOSED FROM FIRST PRINCIPLES, in the order Transformable
		// documents.  `Matrix4Ops::Identity()` stands in for m_mxScale, which
		// the component path never sets (the scene language's `scale` is the
		// per-axis STRETCH); it is written out rather than dropped so the
		// product here is literally the documented one.
		const Matrix4 expect =
			Matrix4Ops::Translation( Vector3( pos.x, pos.y, pos.z ) )
			* ( Matrix4Ops::XRotation( rx ) * Matrix4Ops::YRotation( ry ) * Matrix4Ops::ZRotation( rz ) )
			* Matrix4Ops::Stretch( stretch )
			* Matrix4Ops::Identity()
			* MirrorMx( axis );

		std::string where;
		Check( MxEq( o->GetLocalTransformMatrix(), expect, 0.0, &where ),
		       std::string( "A: axis " ) + char( 'x' + axis ) + " -- local matrix is the exact hand-composed product" + where );
		Check( MxEq( o->GetFinalTransformMatrix(), expect, 0.0 ),
		       std::string( "A: axis " ) + char( 'x' + axis ) + " -- unparented, world == local" );

		// The mirror is INNERMOST, and this is what says so.  Composing it on
		// the OUTSIDE instead gives a different product for any non-trivial
		// position -- so if this passed while the assertion above failed, the
		// implementation put the mirror on the wrong side.
		const Matrix4 outside =
			MirrorMx( axis )
			* Matrix4Ops::Translation( Vector3( pos.x, pos.y, pos.z ) )
			* ( Matrix4Ops::XRotation( rx ) * Matrix4Ops::YRotation( ry ) * Matrix4Ops::ZRotation( rz ) )
			* Matrix4Ops::Stretch( stretch );
		Check( !MxEq( o->GetLocalTransformMatrix(), outside, 1e-12 ),
		       "A: (oracle sanity) the innermost product is NOT equal to the outermost one, so the test discriminates" );

		o->release();
	}

	// Parent composition: `world = parentWorld * local`, mirror included.
	{
		Implementation::Object* o = new Implementation::Object( g );
		o->SetPosition( Point3( 1, 0, 0 ) );
		o->SetMirrorAxis( 0 );
		const Matrix4 parentWorld = Matrix4Ops::Translation( Vector3( 0, 4, 0 ) ) * Matrix4Ops::YRotation( 0.7 );
		o->FinalizeTransformations( parentWorld );
		const Matrix4 expect = parentWorld
			* Matrix4Ops::Translation( Vector3( 1, 0, 0 ) )
			* Matrix4Ops::Identity() * Matrix4Ops::Identity() * Matrix4Ops::Identity()
			* MirrorMx( 0 );
		std::string where;
		Check( MxEq( o->GetFinalTransformMatrix(), expect, 0.0, &where ),
		       std::string( "A: parented -- world == parentWorld * (local with the mirror innermost)" ) + where );
		o->release();
	}

	// IDEMPOTENCE.  Finalizing twice must not compose the mirror twice -- the
	// 86 §3 bug class, which is exactly why the mirror is a MATRIX MEMBER and
	// not a transform-stack push (the stack never self-clears).
	{
		Implementation::Object* o = new Implementation::Object( g );
		o->SetPosition( Point3( 2, 0, 0 ) );
		o->SetMirrorAxis( 2 );
		o->FinalizeTransformations();
		const Matrix4 once = o->GetFinalTransformMatrix();
		o->FinalizeTransformations();
		o->FinalizeTransformations();
		Check( MxEq( o->GetFinalTransformMatrix(), once, 0.0 ),
		       "A: re-finalizing is idempotent -- the mirror composes exactly once, however many times finalize runs" );
		Check( Matrix4Ops::Determinant( once ) < 0.0,
		       "A: a mirrored transform has NEGATIVE determinant (the property every det<0 consumer must survive)" );
		o->release();
	}

	// ClearAllTransforms drops the mirror with the rest of the local transform:
	// an incremental re-apply of a chunk whose `mirror` line was DELETED must
	// come out un-reflected.
	{
		Implementation::Object* o = new Implementation::Object( g );
		o->SetMirrorAxis( 1 );
		o->FinalizeTransformations();
		Check( o->GetMirrorAxis() == 1, "A: the axis reads back" );
		o->ClearAllTransforms();
		Check( o->GetMirrorAxis() == -1, "A: ClearAllTransforms drops the mirror (a deleted `mirror` line must un-reflect)" );
		Check( MxEq( o->GetFinalTransformMatrix(), Matrix4Ops::Identity(), 0.0 ),
		       "A: ... and the composed matrix is back to identity" );
		o->release();
	}

	// Out-of-range axes are refused, changing nothing.
	{
		Implementation::Object* o = new Implementation::Object( g );
		o->SetMirrorAxis( 1 );
		Check( !o->SetMirrorAxis( 3 ),  "A: axis 3 is refused" );
		Check( !o->SetMirrorAxis( -2 ), "A: axis -2 is refused" );
		Check( o->GetMirrorAxis() == 1, "A: ... and a refused axis leaves the previous one in place" );
		o->release();
	}

	// The V2 transform snapshot carries the mirror -- editor undo restores a
	// mirrored pose as a mirrored pose.
	{
		Implementation::Object* o = new Implementation::Object( g );
		o->SetPosition( Point3( 5, 0, 0 ) );
		o->SetMirrorAxis( 0 );
		o->FinalizeTransformations();
		const TransformStateV2 snap = o->CaptureTransformStateV2();
		const Matrix4 before = o->GetFinalTransformMatrix();
		o->ClearAllTransforms();
		Check( o->GetMirrorAxis() == -1, "A: (setup) the mirror really was cleared before the restore" );
		Check( o->RestoreTransformStateV2( snap ), "A: the V2 snapshot restores" );
		Check( o->GetMirrorAxis() == 0 && MxEq( o->GetFinalTransformMatrix(), before, 0.0 ),
		       "A: the V2 snapshot round-trips the mirror (undo of an edit on a mirrored node)" );
		TransformStateV2 bad = snap;
		bad.mirrorAxis = 7;
		Check( !o->RestoreTransformStateV2( bad ),
		       "A: a snapshot with an out-of-range mirror axis is refused whole, not half-applied" );
		o->release();
	}

	// The SNAPSHOT clone (Scene::CreateSnapshot) carries it too.
	{
		Implementation::Object* o = new Implementation::Object( g );
		o->SetPosition( Point3( 0, 0, 7 ) );
		o->SetMirrorAxis( 2 );
		o->FinalizeTransformations();
		Implementation::Object* clone = o->CloneSnapshot();
		Check( clone && MxEq( clone->GetFinalTransformMatrix(), o->GetFinalTransformMatrix(), 0.0 ),
		       "A: CloneSnapshot reproduces the mirrored world matrix" );
		// The building block, not just the finalized matrix: without it the
		// clone's FIRST re-finalize would silently un-reflect it.
		if( clone ) {
			clone->FinalizeTransformations();
			Check( MxEq( clone->GetFinalTransformMatrix(), o->GetFinalTransformMatrix(), 0.0 ),
			       "A: ... and it survives the clone's own re-finalize (the building block was copied, not only the result)" );
			clone->release();
		}
		o->release();
	}

	g->release();
}

//////////////////////////////////////////////////////////////////////
// [B] HAND-REFLECTED TRIANGLE-MESH TWIN
//////////////////////////////////////////////////////////////////////

// Build a deliberately ASYMMETRIC, SINGLE-SIDED triangle mesh.  `flipX` bakes
// the HAND-REFLECTED twin: every vertex's x is negated AND two vertices are
// swapped, which is the winding fix a mesh author would make by hand.  Vertex
// normals are derived from the winding exactly as the RAW loader does
// (TriangleMeshLoaderRAW.cpp), so the twin's normals are produced by the
// geometry, never copied from the thing under test.
static ITriangleMeshGeometry* BuildWedge( bool flipX )
{
	ITriangleMeshGeometry* mesh = 0;
	RISE_API_CreateTriangleMeshGeometry( &mesh, /*double_sided*/ false );
	mesh->BeginTriangles();

	// A fan of four triangles over an asymmetric outline in the z = 0 plane,
	// tilted in z so the surface is not planar and the two halves in x differ.
	const double outline[5][3] = {
		{  0.10,  0.00,  0.00 },
		{  1.30,  0.20,  0.15 },
		{  1.05,  0.95, -0.10 },
		{  0.40,  1.20,  0.25 },
		{  0.05,  0.55, -0.05 }
	};
	const double apex[3] = { 0.55, 0.45, 0.90 };

	for( int i = 0; i < 4; ++i ) {
		Point3 a( apex[0], apex[1], apex[2] );
		Point3 b( outline[i][0],   outline[i][1],   outline[i][2] );
		Point3 c( outline[i+1][0], outline[i+1][1], outline[i+1][2] );
		if( flipX ) {
			a.x = -a.x; b.x = -b.x; c.x = -c.x;
			// WINDING FIX: reflecting the positions reverses the traversal
			// sense, so swap two vertices to put the face normal back where
			// the reflection of the original normal is.
			const Point3 t = b; b = c; c = t;
		}
		Triangle tri;
		tri.vertices[0] = a;
		tri.vertices[1] = b;
		tri.vertices[2] = c;
		tri.normals[0] = tri.normals[1] = tri.normals[2] = Vector3Ops::Normalize( Vector3Ops::Cross(
			Vector3Ops::mkVector3( tri.vertices[1], tri.vertices[0] ),
			Vector3Ops::mkVector3( tri.vertices[2], tri.vertices[0] ) ) );
		tri.coords[0] = Point2( 0, 0 );
		tri.coords[1] = Point2( 0, 1 );
		tri.coords[2] = Point2( 1, 1 );
		mesh->AddTriangle( tri );
	}

	mesh->DoneTriangles();
	return mesh;
}

static void PartB_HandReflectedTwin()
{
	std::printf( "=== [B] a mirrored mesh == a hand-reflected twin mesh (rays, hits and normals) ===\n" );

	ITriangleMeshGeometry* meshA = BuildWedge( false );
	ITriangleMeshGeometry* meshB = BuildWedge( true );

	// The object under test: the ORIGINAL mesh, reflected by the transform.
	Implementation::Object* mirrored = new Implementation::Object( meshA );
	Check( mirrored->SetMirrorAxis( 0 ), "B: mirror x set on the transform-reflected object" );
	mirrored->FinalizeTransformations();

	// The oracle: the HAND-REFLECTED mesh, with NO transform at all.
	Implementation::Object* twin = new Implementation::Object( meshB );
	twin->FinalizeTransformations();

	// The control: the original mesh, un-mirrored.  Its hits must NOT match
	// the twin, or the two probes below would agree for a trivial reason.
	Implementation::Object* plain = new Implementation::Object( meshA );
	plain->FinalizeTransformations();

	int probesHit = 0, matched = 0, controlDiffered = 0;
	int normalsOutwardish = 0;

	// Fire a grid of rays at the reflected wedge from +z.  Every ray that hits
	// the twin must hit the mirrored object at the same point with the same
	// normals -- and the front-faces-only flags mean a handedness defect shows
	// up as a MISS, not merely as a shading difference.
	for( int iy = 0; iy < 9; ++iy ) {
		for( int ix = 0; ix < 9; ++ix ) {
			const double x = -1.25 + ix * 0.14;
			const double y =  0.06 + iy * 0.13;
			const Ray ray( Point3( x, y, 4.0 ), Vector3( 0, 0, -1 ) );

			RayIntersection riT( ray, nullRasterizerState );
			twin->IntersectRay( riT, RISE_INFINITY, /*front*/ true, /*back*/ false, false );
			if( !riT.geometric.bHit ) continue;
			++probesHit;

			RayIntersection riM( ray, nullRasterizerState );
			mirrored->IntersectRay( riM, RISE_INFINITY, true, false, false );

			const bool ok = riM.geometric.bHit
				&& PtClose( riM.geometric.ptIntersection, riT.geometric.ptIntersection, 1e-9 )
				&& VecClose( riM.geometric.vGeomNormal, riT.geometric.vGeomNormal, 1e-9 )
				&& VecClose( riM.geometric.vNormal,     riT.geometric.vNormal,     1e-9 );
			if( ok ) ++matched;
			else if( probesHit - matched <= 3 ) {
				char buf[256];
				std::snprintf( buf, sizeof(buf),
					"B: probe (%.3f, %.3f) -- mirrored %s, twin hit at (%.6f %.6f %.6f) n=(%.6f %.6f %.6f)",
					x, y, riM.geometric.bHit ? "hit but differs" : "MISSED (handedness / winding)",
					(double)riT.geometric.ptIntersection.x, (double)riT.geometric.ptIntersection.y,
					(double)riT.geometric.ptIntersection.z,
					(double)riT.geometric.vGeomNormal.x, (double)riT.geometric.vGeomNormal.y,
					(double)riT.geometric.vGeomNormal.z );
				Check( false, buf );
			}

			// OUTWARD: the wedge's faces are wound so their normals point away
			// from the apex-to-outline body, i.e. they carry a POSITIVE z
			// component on this side.  The reflection about x cannot change
			// that -- if the inverse-transpose path had dropped a sign, the
			// mirrored object's normals would point into the surface.
			if( riM.geometric.bHit && riM.geometric.vGeomNormal.z > 0.0 ) ++normalsOutwardish;

			RayIntersection riP( ray, nullRasterizerState );
			plain->IntersectRay( riP, RISE_INFINITY, true, false, false );
			if( !riP.geometric.bHit
			 || !PtClose( riP.geometric.ptIntersection, riT.geometric.ptIntersection, 1e-9 ) ) ++controlDiffered;
		}
	}

	Check( probesHit >= 20, "B: (control) the probe grid actually hits the twin enough times to mean something" );
	Check( matched == probesHit,
	       "B: EVERY probe that hits the hand-reflected twin hits the mirrored object at the same point, "
	       "with the same geometric AND shading normals" );
	Check( normalsOutwardish == probesHit,
	       "B: the mirrored object's geometric normals point OUTWARD (toward the probe), not into the surface" );
	Check( controlDiffered == probesHit,
	       "B: (oracle sanity) the UN-mirrored original disagrees with the twin on every probe, so the match above is not vacuous" );

	// Back-face probing from the far side: the mirrored object must present a
	// BACK face there, exactly as the twin does.  A winding/handedness defect
	// that merely swapped the two sides would pass the front-face block above
	// only by accident; this closes that.
	//
	// PROBE PLACEMENT IS LOAD-BEARING, and this block used to get it wrong.  It
	// fired a SINGLE ray from (-0.55, 0.45), which is BIT-EXACTLY the mirrored
	// fan APEX -- the one point where all four triangles meet.  A ray through a
	// shared vertex hits all four faces at the same real-world point, but the
	// two objects reach that point through DIFFERENT tie mechanisms, verified
	// by rebuilding each mesh with its four triangles in reversed insertion
	// order and checking whether the reported face changes:
	//   - the TWIN's four candidate t's are a bit-identical tie -- reversing
	//     insertion order changes which face it reports -- so its winner is
	//     whichever face closest-hit tie-breaking visits FIRST
	//     (`h.dRange < ri.range` is strict, so an equal-but-later candidate
	//     never displaces it); the twin's different coordinates (two vertices
	//     swapped) just mean its BVH happens to visit a different face first.
	//   - the MIRRORED object's four candidates are NOT bit-identical -- one is
	//     a genuine, if minute, numeric minimum -- reversing insertion order
	//     reports the SAME winning face either way, so its answer is a real
	//     (if tiny) decision, not an arbitrary tie-break.
	// Both mechanisms produce legitimate normals: at a vertex shared by four
	// non-coplanar faces the geometric normal is genuinely four-valued, and no
	// intersector can make two differently-built meshes -- or, for the twin,
	// two different traversal orders of the SAME mesh -- agree on which face
	// owns it.  The front-face grid above escapes the same trap only by
	// rounding luck (its cell `-1.25 + 5*0.14` lands 1.1e-16 short of -0.55).
	// The probes below are therefore placed strictly INTERIOR to each of the
	// four fan triangles -- off every vertex and every shared edge -- which
	// also turns one probe into four and gives every face back-face coverage
	// instead of just one.
	{
		// x is already in mirrored space; each row is inside a different face.
		static const double kBackProbes[4][2] = {
			{ -0.65, 0.22 },   // interior of fan triangle 0
			{ -0.95, 0.55 },   // interior of fan triangle 1
			{ -0.68, 0.87 },   // interior of fan triangle 2
			{ -0.33, 0.73 }    // interior of fan triangle 3
		};

		int backControl = 0, backMatched = 0, frontOnlyMissed = 0;
		for( int k = 0; k < 4; ++k ) {
			const Ray ray( Point3( kBackProbes[k][0], kBackProbes[k][1], -4.0 ), Vector3( 0, 0, 1 ) );

			RayIntersection riT( ray, nullRasterizerState );
			twin->IntersectRay( riT, RISE_INFINITY, /*front*/ false, /*back*/ true, false );
			RayIntersection riM( ray, nullRasterizerState );
			mirrored->IntersectRay( riM, RISE_INFINITY, false, true, false );
			if( riT.geometric.bHit ) ++backControl;
			if( riM.geometric.bHit == riT.geometric.bHit
			 && ( !riT.geometric.bHit
			      || ( PtClose( riM.geometric.ptIntersection, riT.geometric.ptIntersection, 1e-9 )
			           && VecClose( riM.geometric.vGeomNormal, riT.geometric.vGeomNormal, 1e-9 ) ) ) ) {
				++backMatched;
			}

			// The other half of "classification survives": with only FRONT faces
			// accepted, this same ray -- which sees the surface from behind --
			// must MISS on both.  An implementation that merely swapped the two
			// sides would pass the back-face compare above and fail here.
			RayIntersection riTf( ray, nullRasterizerState );
			twin->IntersectRay( riTf, RISE_INFINITY, /*front*/ true, /*back*/ false, false );
			RayIntersection riMf( ray, nullRasterizerState );
			mirrored->IntersectRay( riMf, RISE_INFINITY, true, false, false );
			if( !riTf.geometric.bHit && !riMf.geometric.bHit ) ++frontOnlyMissed;
		}

		Check( backControl == 4, "B: (control) the twin presents a back face to a ray from -z, on all four faces" );
		Check( backMatched == 4,
		       "B: front/back face classification survives the reflection (same side, same normal, as the twin)" );
		Check( frontOnlyMissed == 4,
		       "B: ... and a front-faces-only ray from the BACK side misses on the mirrored object exactly as on the twin" );
	}

	// The same property in its DEGENERACY-FREE form, which the twin oracle above
	// cannot express: the mirrored object must reproduce the UN-mirrored original's
	// hit, exactly reflected -- same hit/miss under each acceptance flag, and
	// normals equal after negating x.  Both sides run against the SAME mesh and the
	// SAME BVH, and -- because the mirror here is exactly diag(-1,1,1) and the probe
	// x is negated to match -- they reduce to the BIT-IDENTICAL object-space ray, so
	// every tie is broken identically by construction rather than by luck.  The
	// comparison is therefore exact (1e-12, not the 1e-9 the twin comparison needs)
	// at every GRID point of the surface -- see the dedicated apex probe below for
	// the one point the grid itself cannot reach.  What is left to differ is the
	// transform layer's own work: the acceptance-flag pass-through, and the SIGN
	// the published normal picks up under this det<0 transform.
	//
	// NOTE what this block does NOT pin down.  Object::IntersectRay publishes
	// normalize(M^-T n_obj) -- the inverse-transpose promotion.  But for a PURE
	// AXIS MIRROR, M = diag(-1,1,1) IS its own inverse-transpose (diagonal, and
	// every entry is its own reciprocal: -1 and 1 are both self-inverse), so
	// normalize(M^-T n_obj) and normalize(M n_obj) are the SAME vector here --
	// this block cannot tell "the normal was promoted by M^-T" apart from "by M
	// itself".  All it actually pins down is the published normal's SIGN under
	// reflection.  The probe that follows this one composes the mirror with a
	// NON-uniform scale, where M != M^-T, and is the one that actually
	// discriminates the two formulas.  This block is still the assertion that
	// catches the failure mode the block above is named for -- an object-space
	// winding test left uncorrected under a negative-determinant transform would
	// make the mirrored object miss where the original hits -- and unlike the
	// twin comparison it cannot be perturbed by a shared-vertex tie-break.
	{
		// SIBLING (audit-by-bug-pattern): Object::IntersectRay_IntersectionOnly --
		// the SHADOW-ray path -- transforms the ray by the same m_mxInvFinalTrans
		// and hands bHitFrontFaces/bHitBackFaces straight through to the geometry
		// in exactly the same way, so it carries the same question and is swept
		// here alongside the full path.  (CSGObject::IntersectRay is NOT a sibling:
		// it discards both flags and always probes its operands with `true, true`
		// because interval arithmetic needs both sides -- and a csg_object is
		// refused a mirror outright, see [F].)
		const int kN = 25;	// probes per axis
		int probed = 0, agreed = 0, shadowAgreed = 0, swept = 0;
		for( int iy = 0; iy < kN; ++iy ) {
			for( int ix = 0; ix < kN; ++ix ) {
				const double x = -1.30 + ix * ( 1.30 / ( kN - 1 ) );
				const double y =  0.00 + iy * ( 1.25 / ( kN - 1 ) );
				for( int side = 0; side < 2; ++side ) {
					++swept;
					const double  z     = side ? -4.0 : 4.0;
					const Vector3 dir   = side ? Vector3( 0, 0, 1 ) : Vector3( 0, 0, -1 );
					const bool    front = !side, back = !!side;

					const Ray rayM( Point3(  x, y, z ), dir );
					RayIntersection riM( rayM, nullRasterizerState );
					mirrored->IntersectRay( riM, RISE_INFINITY, front, back, false );
					// The un-mirrored original, probed at the reflected x.
					const Ray rayP( Point3( -x, y, z ), dir );
					RayIntersection riP( rayP, nullRasterizerState );
					plain->IntersectRay( riP, RISE_INFINITY, front, back, false );

					// The shadow-ray sibling, on the same two rays.
					if( mirrored->IntersectRay_IntersectionOnly( rayM, RISE_INFINITY, front, back )
					 == plain->IntersectRay_IntersectionOnly( rayP, RISE_INFINITY, front, back ) ) {
						++shadowAgreed;
					}

					if( !riM.geometric.bHit && !riP.geometric.bHit ) continue;
					++probed;
					if( riM.geometric.bHit != riP.geometric.bHit ) continue;

					const Point3  pRef( -riP.geometric.ptIntersection.x,
					                     riP.geometric.ptIntersection.y,
					                     riP.geometric.ptIntersection.z );
					const Vector3 gRef( -riP.geometric.vGeomNormal.x,
					                     riP.geometric.vGeomNormal.y,
					                     riP.geometric.vGeomNormal.z );
					const Vector3 nRef( -riP.geometric.vNormal.x,
					                     riP.geometric.vNormal.y,
					                     riP.geometric.vNormal.z );
					if( PtClose( riM.geometric.ptIntersection, pRef, 1e-12 )
					 && VecClose( riM.geometric.vGeomNormal, gRef, 1e-12 )
					 && VecClose( riM.geometric.vNormal,     nRef, 1e-12 ) ) {
						++agreed;
					}
				}
			}
		}
		Check( probed >= 200, "B: (control) the reflected-original sweep actually reaches the surface" );
		Check( swept == kN * kN * 2 && shadowAgreed == swept,
		       "B: the SHADOW-ray path (IntersectRay_IntersectionOnly) accepts/rejects identically on the mirrored "
		       "object and the reflected original, under both acceptance flags" );
		Check( agreed == probed,
		       "B: the mirrored object reproduces the UN-mirrored original exactly reflected -- hit/miss under BOTH "
		       "acceptance flags, geometric and shading normals -- at every GRID probe" );

		// EXPLICIT APEX PROBE.  "at every GRID probe" above is not "at every point
		// of the surface" -- the 25x25 grid's steps (1.30/24 and 1.25/24) don't
		// divide the apex's 0.55 / 0.45 offsets evenly, so no grid cell lands on
		// it (checked: the nearest cell is a fraction of a step away on both
		// axes).  That is exactly the point this whole block exists to cover, so
		// probe it directly, under both flag settings, with the same ray
		// construction the sweep above uses per side.
		int apexProbed = 0, apexAgreed = 0;
		for( int side = 0; side < 2; ++side ) {
			const double  z     = side ? -4.0 : 4.0;
			const Vector3 dir   = side ? Vector3( 0, 0, 1 ) : Vector3( 0, 0, -1 );
			const bool    front = !side, back = !!side;

			const Ray rayM( Point3( -0.55, 0.45, z ), dir );
			RayIntersection riM( rayM, nullRasterizerState );
			mirrored->IntersectRay( riM, RISE_INFINITY, front, back, false );
			const Ray rayP( Point3( 0.55, 0.45, z ), dir );
			RayIntersection riP( rayP, nullRasterizerState );
			plain->IntersectRay( riP, RISE_INFINITY, front, back, false );

			if( riM.geometric.bHit && riP.geometric.bHit ) {
				++apexProbed;
				const Point3  pRef( -riP.geometric.ptIntersection.x,
				                     riP.geometric.ptIntersection.y,
				                     riP.geometric.ptIntersection.z );
				const Vector3 gRef( -riP.geometric.vGeomNormal.x,
				                     riP.geometric.vGeomNormal.y,
				                     riP.geometric.vGeomNormal.z );
				const Vector3 nRef( -riP.geometric.vNormal.x,
				                     riP.geometric.vNormal.y,
				                     riP.geometric.vNormal.z );
				if( PtClose( riM.geometric.ptIntersection, pRef, 1e-12 )
				 && VecClose( riM.geometric.vGeomNormal, gRef, 1e-12 )
				 && VecClose( riM.geometric.vNormal,     nRef, 1e-12 ) ) {
					++apexAgreed;
				}
			}
		}
		Check( apexProbed == 2, "B: (control) the explicit apex probe hits on both the front side and the back side" );
		Check( apexAgreed == 2,
		       "B: ... and AT THE FAN APEX ITSELF -- the one point the grid above cannot reach -- the mirrored object "
		       "still reproduces the UN-mirrored original exactly reflected, under both acceptance flags" );
	}

	// ADJOINT DISCRIMINATOR.  The block above pins the published normal's SIGN
	// under a pure axis mirror, but cannot separate normalize(M^-T n_obj) from
	// normalize(M n_obj) because M = diag(-1,1,1) is its own inverse-transpose.
	// Compose the mirror with a NON-UNIFORM stretch so that stops being true:
	// M = Stretch(1,2,1) * Mirror(x) = diag(-1,2,1), whose inverse-transpose is
	// diag(-1,0.5,1) -- a genuinely different matrix.  The published geometric
	// normal must equal normalize(M^-T n_obj) and must NOT equal normalize(M n_obj).
	{
		Implementation::Object* adj = new Implementation::Object( meshA );
		Check( adj->SetMirrorAxis( 0 ), "B: (adjoint probe) mirror x set" );
		adj->SetStretch( Vector3( 1, 2, 1 ) );
		adj->FinalizeTransformations();

		const Matrix4 M     = adj->GetFinalTransformMatrix();
		const Matrix4 MInvT = Matrix4Ops::Transpose( Matrix4Ops::Inverse( M ) );
		Check( !MxEq( M, MInvT, 1e-9 ),
		       "B: (control) mirror + non-uniform stretch really does have M != M^-T, or this probe is vacuous" );

		// An object-space point strictly interior to fan triangle 0 (barycentric
		// combination of its three vertices, weights summing to 1, all > 0), and
		// that triangle's own OBJECT-SPACE geometric normal -- both computed
		// exactly as BuildWedge does for face 0 (apex, outline[0], outline[1]),
		// independent of the hit-testing machinery above.
		const Point3  pA( 0.55, 0.45, 0.90 );   // apex
		const Point3  pB( 0.10, 0.00, 0.00 );   // outline[0]
		const Point3  pC( 1.30, 0.20, 0.15 );   // outline[1]
		const Point3  pObj( 0.5 * pA.x + 0.3 * pB.x + 0.2 * pC.x,
		                     0.5 * pA.y + 0.3 * pB.y + 0.2 * pC.y,
		                     0.5 * pA.z + 0.3 * pB.z + 0.2 * pC.z );
		const Vector3 nObj = Vector3Ops::Normalize( Vector3Ops::Cross(
			Vector3Ops::mkVector3( pB, pA ), Vector3Ops::mkVector3( pC, pA ) ) );

		const Vector3 nExpected = Vector3Ops::Normalize( Vector3Ops::Transform( MInvT, nObj ) );
		const Vector3 nWrong    = Vector3Ops::Normalize( Vector3Ops::Transform( M,     nObj ) );
		Check( !VecClose( nExpected, nWrong, 1e-6 ),
		       "B: (control) the two candidate formulas really do disagree here, or the probe below is vacuous" );

		const Point3 pWorld = Point3Ops::Transform( M, pObj );
		const Ray ray( Point3( pWorld.x, pWorld.y, 4.0 ), Vector3( 0, 0, -1 ) );
		RayIntersection ri( ray, nullRasterizerState );
		adj->IntersectRay( ri, RISE_INFINITY, true, false, false );

		Check( ri.geometric.bHit, "B: (control) the adjoint probe's ray actually hits the mirror+stretch object" );
		if( ri.geometric.bHit ) {
			Check( VecClose( ri.geometric.vGeomNormal, nExpected, 1e-12 ),
			       "B: mirror + non-uniform stretch -- the published geometric normal equals normalize(M^-T n_obj)" );
			Check( !VecClose( ri.geometric.vGeomNormal, nWrong, 1e-6 ),
			       "B: ... and does NOT equal normalize(M n_obj) -- the probe that actually discriminates the "
			       "inverse-transpose promotion from the forward matrix" );
		}
		adj->release();
	}

	mirrored->release();
	twin->release();
	plain->release();
	meshA->release();
	meshB->release();
}

// The TANGENT-FRAME half of handedness.  `Object::m_tangentFrameSign` exists so
// an imported per-vertex TANGENT.w still rebuilds the right bitangent under an
// orientation-REVERSING transform, and a mirror is exactly that -- so this is
// the pre-existing det<0 consumer the mirror hands its first named-parameter
// caller.
//
// The invariant asserted is not "the sign is -1" (that would restate the
// implementation) but the thing the sign exists to preserve:
//
//     cross( N_world, T_world ) * bitangentSign  ==  Mirror * ( cross( N_obj, T_obj ) * w )
//
// i.e. the world bitangent the NormalMap modifier rebuilds is the REFLECTION of
// the object-space one.  Drop the `*= m_tangentFrameSign` in
// Object::IntersectRay and this comes out negated.
static void PartB2_TangentHandedness()
{
	std::printf( "=== [B2] a mirrored transform flips the imported bitangent sign back ===\n" );

	ITriangleMeshGeometryIndexed* meshRaw = 0;
	RISE_API_CreateTriangleMeshGeometryIndexed( &meshRaw, /*double_sided*/ false, /*face_normals*/ false );
	ITriangleMeshGeometryIndexed3* mesh = dynamic_cast<ITriangleMeshGeometryIndexed3*>( meshRaw );
	Check( mesh != 0, "B2: the indexed mesh exposes the v3 (tangent) storage interface" );
	if( !mesh ) { if( meshRaw ) meshRaw->release(); return; }

	// One triangle in the z = 0 plane, wound so its face normal is +z.
	mesh->BeginIndexedTriangles();
	mesh->AddVertex( Vertex( 0, 0, 0 ) );
	mesh->AddVertex( Vertex( 1, 0, 0 ) );
	mesh->AddVertex( Vertex( 0, 1, 0 ) );
	for( int k = 0; k < 3; ++k ) mesh->AddNormal( Normal( 0, 0, 1 ) );
	mesh->AddTexCoord( TexCoord( 0, 0 ) );
	mesh->AddTexCoord( TexCoord( 1, 0 ) );
	mesh->AddTexCoord( TexCoord( 0, 1 ) );
	Tangent4 t;
	t.dir = Vector3( 1, 0, 0 );
	t.bitangentSign = 1.0;
	for( int k = 0; k < 3; ++k ) mesh->AddTangent( t );
	IndexedTriangle it;
	for( int k = 0; k < 3; ++k ) { it.iVertices[k] = k; it.iNormals[k] = k; it.iCoords[k] = k; }
	mesh->AddIndexedTriangle( it );
	mesh->DoneIndexedTriangles();

	// The object-space bitangent the imported frame describes.
	const Vector3 bitangentObj = Vector3Ops::Cross( Vector3( 0, 0, 1 ), Vector3( 1, 0, 0 ) );   // = (0, 1, 0), w = +1

	Implementation::Object* plain = new Implementation::Object( meshRaw );
	plain->FinalizeTransformations();
	Implementation::Object* mirrored = new Implementation::Object( meshRaw );
	mirrored->SetMirrorAxis( 0 );
	mirrored->FinalizeTransformations();

	// Two probes: the un-mirrored triangle lives at x in [0,1], the mirrored one
	// at x in [-1,0], so each ray is aimed at its own object.  The assertion
	// compares the INVARIANT, not the ray.
	RayIntersection riP( Ray( Point3(  0.25, 0.25, 3 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
	plain->IntersectRay( riP, RISE_INFINITY, true, false, false );
	RayIntersection riM( Ray( Point3( -0.25, 0.25, 3 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
	mirrored->IntersectRay( riM, RISE_INFINITY, true, false, false );

	Check( riP.geometric.bHit && riP.geometric.bHasTangent, "B2: (control) the un-mirrored probe hits and carries a tangent" );
	Check( riM.geometric.bHit && riM.geometric.bHasTangent, "B2: the mirrored probe hits and carries a tangent" );

	if( riP.geometric.bHit && riM.geometric.bHit ) {
		const Vector3 bP = Vector3Ops::Cross( riP.geometric.vNormal,
			Vector3Ops::Normalize( riP.geometric.vTangent ) ) * riP.geometric.bitangentSign;
		const Vector3 bM = Vector3Ops::Cross( riM.geometric.vNormal,
			Vector3Ops::Normalize( riM.geometric.vTangent ) ) * riM.geometric.bitangentSign;

		Check( VecClose( bP, bitangentObj, 1e-9 ),
		       "B2: (control) with no transform the rebuilt world bitangent IS the object-space one" );
		char buf[192];
		std::snprintf( buf, sizeof(buf),
			"B2: the mirrored world bitangent is the REFLECTION of the object-space one -- want (%.6f %.6f %.6f), got (%.6f %.6f %.6f)",
			-bitangentObj.x, bitangentObj.y, bitangentObj.z, (double)bM.x, (double)bM.y, (double)bM.z );
		Check( VecClose( bM, Vector3( -bitangentObj.x, bitangentObj.y, bitangentObj.z ), 1e-9 ), buf );
		Check( riM.geometric.bitangentSign * riP.geometric.bitangentSign < 0.0,
		       "B2: ... which it achieves by FLIPPING the imported TANGENT.w, not by leaving it alone" );
	}

	plain->release();
	mirrored->release();
	meshRaw->release();
}

//////////////////////////////////////////////////////////////////////
// [B3] THE DERIVATIVE FALLBACK -- NO IMPORTED TANGENT
//////////////////////////////////////////////////////////////////////

// [B2] pins the path an asset with an authored TANGENT accessor takes.  The path
// a LATHE, SWEEP or SKIN bake takes is the OTHER one: those geometries populate
// UV surface derivatives but ship no TANGENT, so NormalMap builds its frame from
// `dpdu` and derives the bitangent as `cross(N, T)`.
//
// That cross product is exactly what a negative determinant flips: `N` comes
// through the inverse-transpose while `dpdu` comes through the forward matrix, so
// under a reflection it points opposite the transformed original bitangent.  The
// handedness sign that fixes it used to be folded into `bitangentSign` only inside
// the has-a-tangent branch, and consumed only inside the has-a-tangent branch --
// so a mirrored, normal-mapped asset with no TANGENT rendered with its green
// channel inverted.  Pre-existing for `scale -1 1 1`; this slice makes reflection
// a one-token op on exactly that asset class.
//
// Probed through the REAL modifier, not through the sign alone: the sign is only
// half the fix (Object::IntersectRay folds it in; NormalMap has to USE it), and an
// assertion on the sign would stay green with the second half reverted.
static void PartB3_DerivativeFallbackHandedness()
{
	std::printf( "=== [B3] a mirrored NORMAL-MAPPED hit with NO imported tangent ===\n" );

	ITriangleMeshGeometryIndexed* mesh = 0;
	RISE_API_CreateTriangleMeshGeometryIndexed( &mesh, /*double_sided*/ false, /*face_normals*/ false );
	Check( mesh != 0, "B3: (setup) the indexed mesh is created" );
	if( !mesh ) return;

	// The same +z triangle as [B2], with TEXCOORDs (so derivatives are populated)
	// and DELIBERATELY NO TANGENT accessor.
	mesh->BeginIndexedTriangles();
	mesh->AddVertex( Vertex( 0, 0, 0 ) );
	mesh->AddVertex( Vertex( 1, 0, 0 ) );
	mesh->AddVertex( Vertex( 0, 1, 0 ) );
	for( int k = 0; k < 3; ++k ) mesh->AddNormal( Normal( 0, 0, 1 ) );
	mesh->AddTexCoord( TexCoord( 0, 0 ) );
	mesh->AddTexCoord( TexCoord( 1, 0 ) );
	mesh->AddTexCoord( TexCoord( 0, 1 ) );
	IndexedTriangle it;
	for( int k = 0; k < 3; ++k ) { it.iVertices[k] = k; it.iNormals[k] = k; it.iCoords[k] = k; }
	mesh->AddIndexedTriangle( it );
	mesh->DoneIndexedTriangles();

	// A normal map that leans hard along +V, so the BITANGENT carries most of the
	// perturbation and a flipped one is unmissable.  Uniform, so the assertion
	// cannot be satisfied by a UV-lookup coincidence.
	IPainter* nmPainter = 0;
	RISE_API_CreateUniformColorPainter( &nmPainter, RISEPel( 0.7, 0.9, 1.0 ) );
	Check( nmPainter != 0, "B3: (setup) the normal-map painter is created" );
	if( !nmPainter ) { mesh->release(); return; }
	Implementation::NormalMap* modifier = new Implementation::NormalMap( *nmPainter, 1.0 );

	Implementation::Object* plain = new Implementation::Object( mesh );
	plain->FinalizeTransformations();
	Implementation::Object* mirrored = new Implementation::Object( mesh );
	mirrored->SetMirrorAxis( 0 );
	mirrored->FinalizeTransformations();

	RayIntersection riP( Ray( Point3(  0.25, 0.25, 3 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
	plain->IntersectRay( riP, RISE_INFINITY, true, false, false );
	RayIntersection riM( Ray( Point3( -0.25, 0.25, 3 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
	mirrored->IntersectRay( riM, RISE_INFINITY, true, false, false );

	Check( riP.geometric.bHit && riM.geometric.bHit, "B3: both probes hit" );
	Check( riP.geometric.bHit && !riP.geometric.bHasTangent && riP.geometric.derivatives.valid,
	       "B3: (premise) the hit carries NO imported tangent but DOES carry UV derivatives -- the lathe / "
	       "sweep / skin bake case" );

	if( riP.geometric.bHit && riM.geometric.bHit ) {
		const Vector3 nP0 = riP.geometric.vNormal;
		modifier->Modify( riP.geometric );
		modifier->Modify( riM.geometric );
		const Vector3 nP = riP.geometric.vNormal;
		const Vector3 nM = riM.geometric.vNormal;

		char buf[256];
		std::snprintf( buf, sizeof(buf), "B3: (control) the un-mirrored perturbed normal leans off the geometric "
			"one along the BITANGENT, so the assertion below is live (perturbed y %.6f, geometric y %.6f)",
			(double)nP.y, (double)nP0.y );
		Check( std::fabs( (double)nP.y ) > 0.2, buf );

		char buf2[352];
		std::snprintf( buf2, sizeof(buf2), "B3: the MIRRORED perturbed normal is the exact reflection of the "
			"un-mirrored one -- want (%.6f %.6f %.6f), got (%.6f %.6f %.6f).  A y-component of the WRONG SIGN is "
			"the inverted green channel: cross(N, dpdu) flips under a reflection and nothing put it back",
			-(double)nP.x, (double)nP.y, (double)nP.z, (double)nM.x, (double)nM.y, (double)nM.z );
		Check( VecClose( nM, Vector3( -nP.x, nP.y, nP.z ), 1e-9 ), buf2 );
	}

	modifier->release();
	nmPainter->release();
	plain->release();
	mirrored->release();
	mesh->release();
}

//////////////////////////////////////////////////////////////////////
// [C] A MIRRORED EMITTER LIGHTS CORRECTLY
//////////////////////////////////////////////////////////////////////

static void PartC_EmitterSamplingPath()
{
	std::printf( "=== [C] the luminaire-sampling path on a mirrored emitter ===\n" );

	ITriangleMeshGeometry* meshA = BuildWedge( false );
	ITriangleMeshGeometry* meshB = BuildWedge( true );

	Implementation::Object* mirrored = new Implementation::Object( meshA );
	mirrored->SetMirrorAxis( 0 );
	mirrored->FinalizeTransformations();

	Implementation::Object* twin = new Implementation::Object( meshB );
	twin->FinalizeTransformations();

	// (1) pdfPosition's denominator.  |det| of a reflection is 1, so the area
	//     is unchanged -- but only because GetArea takes the ABSOLUTE value.
	//     A signed det^(2/3) would be NaN here and the emitter would vanish
	//     from every light-selection table it appears in.
	const double aM = (double)mirrored->GetArea();
	const double aT = (double)twin->GetArea();
	Check( aT > 0.0, "C: (control) the twin emitter has positive area" );
	Check( aM == aM && aM > 0.0, "C: the MIRRORED emitter's area is finite and positive (not NaN from a signed det)" );
	{
		char buf[160];
		std::snprintf( buf, sizeof(buf), "C: mirrored area == hand-reflected twin area (got %.17g vs %.17g)", aM, aT );
		Check( std::fabs( aM - aT ) <= 1e-12 * aT, buf );
	}

	// A mirror COMPOSED WITH A SCALE still gets the right Jacobian: det = -s^3,
	// |det|^(2/3) = s^2, so the area scales by s^2 exactly as an un-mirrored
	// scale would.  This is the case where an unsigned/absolute-value slip is
	// visible as a wrong NUMBER rather than as a NaN.
	{
		Implementation::Object* scaled = new Implementation::Object( meshA );
		scaled->SetMirrorAxis( 0 );
		scaled->SetScale( 3.0 );
		scaled->FinalizeTransformations();
		const double got = (double)scaled->GetArea();
		char buf[160];
		std::snprintf( buf, sizeof(buf), "C: mirror + `scale 3` scales the emitter area by 9 (got %.17g, want %.17g)", got, 9.0 * aT );
		Check( std::fabs( got - 9.0 * aT ) <= 1e-9 * aT, buf );
		scaled->release();
	}

	// MIRROR + NON-UNIFORM SCALE.  The uniform case above cannot separate a
	// correct |det|^(2/3) from a hard-coded s^2, and it is the case where the
	// reflection's sign has the least room to show: det = -s^3 and |det|^(2/3)
	// is s^2 either way.  With `stretch 2 3 4` the determinant is -24 and the
	// contract is |det|^(2/3) = 24^(2/3), a number no accidental formulation
	// (mean of the axes, product of two of them, the largest squared) reproduces.
	//
	// KNOWN DIVERGENCE, DELIBERATELY NOT ASSERTED HERE.  Under a NON-UNIFORM
	// scale `UniformRandomPoint` pushes the sample normal through the FORWARD
	// matrix (Object.cpp) while `IntersectRay` pushes the hit normal through the
	// INVERSE-TRANSPOSE, and those disagree the moment the scale stops being
	// uniform.  That is a pre-existing wrongness of the sampler under non-uniform
	// scale, entirely independent of `mirror` (it reproduces with `stretch 2 3 4`
	// and no mirror at all), and fixing it belongs with the world-area Jacobian's
	// own "exactness needs per-geometry integration" note -- NOT here.  What IS
	// mirror-specific, and is asserted, is that the mirrored sampler agrees
	// EXACTLY with the same-stretch un-mirrored one under reflection: point and
	// normal both, whatever convention that normal is computed in.
	{
		const Vector3 stretch( 2, 3, 4 );
		Implementation::Object* mirroredStretched = new Implementation::Object( meshA );
		mirroredStretched->SetMirrorAxis( 0 );
		mirroredStretched->SetStretch( stretch );
		mirroredStretched->FinalizeTransformations();

		Implementation::Object* plainStretched = new Implementation::Object( meshA );
		plainStretched->SetStretch( stretch );
		plainStretched->FinalizeTransformations();

		const double wantScale = std::pow( 24.0, 2.0 / 3.0 );
		const double gotArea   = (double)mirroredStretched->GetArea();
		const double plainArea = (double)plainStretched->GetArea();
		char buf[224];
		std::snprintf( buf, sizeof(buf),
			"C: mirror + `stretch 2 3 4` scales the emitter area by |det|^(2/3) = 24^(2/3) = %.17g "
			"(got %.17g, want %.17g)", wantScale, gotArea, wantScale * aT );
		Check( std::fabs( gotArea - wantScale * aT ) <= 1e-9 * aT, buf );
		Check( gotArea == gotArea && gotArea > 0.0,
		       "C: ... and it is finite and positive -- a SIGNED det^(2/3) would be NaN at det = -24" );
		Check( std::fabs( gotArea - plainArea ) <= 1e-12 * plainArea,
		       "C: ... and identical to the same stretch WITHOUT the mirror (|det| discards the sign)" );

		int pts = 0, norms = 0;
		for( int i = 0; i < 32; ++i ) {
			const Point3 prand( ( ( i * 7 ) % 32 + 0.5 ) / 32.0,
			                    ( ( i * 23 ) % 32 + 0.5 ) / 32.0,
			                    ( ( i * 11 ) % 32 + 0.5 ) / 32.0 );
			Point3 pM, pP; Vector3 nM, nP; Point2 uvM, uvP;
			mirroredStretched->UniformRandomPoint( &pM, &nM, &uvM, prand );
			plainStretched->UniformRandomPoint( &pP, &nP, &uvP, prand );
			if( PtClose( pM, Point3( -pP.x, pP.y, pP.z ), 1e-12 ) ) ++pts;
			if( VecClose( nM, Vector3Ops::Normalize( Vector3( -nP.x, nP.y, nP.z ) ), 1e-12 ) ) ++norms;
		}
		Check( pts == 32,
		       "C: under a NON-UNIFORM scale every mirrored sample point is still the exact reflection "
		       "of the same-stretch un-mirrored one" );
		Check( norms == 32, "C: ... and so is every sample normal" );

		plainStretched->release();
		mirroredStretched->release();
	}

	// (2) the SAMPLE and its NORMAL.  These are the other two things the
	//     luminaire sampler reads, and a wrong normal on a mirrored emitter is
	//     a wing that emits INWARD -- an emitter that lights nothing, which the
	//     area check alone would never notice.
	//
	//     NOT a pointwise `prand`-for-`prand` compare against the twin, and the
	//     reason is a property of the ORACLE, not of the feature: the twin's
	//     winding fix swaps two vertices of every triangle, which permutes that
	//     triangle's barycentric parameterisation -- so one `prand` lands on
	//     corresponding SURFACES but not on corresponding POINTS, and a
	//     pointwise assertion would be red for a reason that has nothing to do
	//     with the mirror.  What must hold, and is asserted instead, is
	//     stronger than a coordinate coincidence:
	//       (a) the sample is the exact reflection of the UN-mirrored object's
	//           sample for the same `prand`, and so is its normal;
	//       (b) the sampled point actually LIES ON the hand-reflected twin's
	//           surface, verified by firing a ray back down the reported
	//           normal and requiring a FRONT-face hit at that same point.
	//     (b) is what makes this the luminaire path rather than arithmetic: a
	//     sample that missed the twin, or that reported an inward normal, is a
	//     shadow ray the NEE estimator would trace into the emitter's own back.
	Implementation::Object* plain = new Implementation::Object( meshA );
	plain->FinalizeTransformations();

	int samples = 0, reflectedPoint = 0, reflectedNormal = 0, onTwin = 0, outward = 0;
	for( int i = 0; i < 64; ++i ) {
		// A deterministic, non-degenerate spread over the unit cube.
		const double a = ( ( i * 7 ) % 64 + 0.5 ) / 64.0;
		const double b = ( ( i * 23 ) % 64 + 0.5 ) / 64.0;
		const double c = ( ( i * 11 ) % 64 + 0.5 ) / 64.0;
		const Point3 prand( a, b, c );

		Point3 pM, pP; Vector3 nM, nP; Point2 uvM, uvP;
		mirrored->UniformRandomPoint( &pM, &nM, &uvM, prand );
		plain->UniformRandomPoint( &pP, &nP, &uvP, prand );
		++samples;

		// (a) exact reflection of the un-mirrored sample.
		if( PtClose( pM, Point3( -pP.x, pP.y, pP.z ), 1e-12 ) ) ++reflectedPoint;
		if( VecClose( nM, Vector3Ops::Normalize( Vector3( -nP.x, nP.y, nP.z ) ), 1e-12 ) ) ++reflectedNormal;

		// (b) the point is on the hand-reflected twin's surface, front side.
		const Ray probe( Point3( pM.x + nM.x * 0.01, pM.y + nM.y * 0.01, pM.z + nM.z * 0.01 ),
		                 Vector3( -nM.x, -nM.y, -nM.z ) );
		RayIntersection riT( probe, nullRasterizerState );
		twin->IntersectRay( riT, 1.0, /*front*/ true, /*back*/ false, false );
		if( riT.geometric.bHit && PtClose( riT.geometric.ptIntersection, pM, 1e-6 ) ) ++onTwin;

		// OUTWARD, geometrically: the wedge is a fan whose faces all carry a
		// positive z normal component (see PartB).  Reflecting about x leaves
		// that alone.
		if( nM.z > 0.0 ) ++outward;
	}
	Check( samples == 64, "C: (control) the sampler was exercised" );
	Check( reflectedPoint == samples,
	       "C: every UniformRandomPoint sample on the mirrored emitter is the exact reflection of the un-mirrored one" );
	Check( reflectedNormal == samples,
	       "C: ... and so is the normal the sampler hands the NEE estimator" );
	Check( onTwin == samples,
	       "C: ... and every sampled point LIES ON the hand-reflected twin's surface, on its FRONT side "
	       "(a ray fired back down the reported normal hits it there)" );
	Check( outward == samples,
	       "C: ... so a mirrored emitter emits away from its surface, not into it" );

	plain->release();

	mirrored->release();
	twin->release();
	meshA->release();
	meshB->release();
}

//////////////////////////////////////////////////////////////////////
// [D] THROUGH THE SCENE LANGUAGE
//////////////////////////////////////////////////////////////////////

static const std::string HDR = "RISE ASCII SCENE 7\n";

static std::string Scene( const std::string& body )
{
	return HDR
		+ "sphere_geometry\n{\nname geo\nradius 0.25\n}\n"
		+ "uniformcolor_painter\n{\nname p\ncolor 0.5 0.5 0.5\n}\n"
		+ "lambertian_material\n{\nname m\nreflectance p\n}\n"
		+ body;
}

static Job* DeriveJob( const std::string& scene, std::vector<std::string>* outDiags = nullptr )
{
	Job* j = new Job();
	Document d = ParseToCst( scene );
	std::vector<std::string> diags;
	DeriveToJob( d, *j, &diags );
	if( outDiags ) *outDiags = diags;
	return j;
}

static IObject* Obj( Job* j, const char* name )
{
	return ( j && j->GetObjects() ) ? j->GetObjects()->GetItem( name ) : 0;
}

// An object's world matrix, or identity-with-a-marker when it does not exist
// (so a missing object can never accidentally satisfy an assertion).
static bool WorldMx( Job* j, const char* name, Matrix4& out )
{
	IObject* o = Obj( j, name );
	if( !o ) return false;
	out = o->GetFinalTransformMatrix();
	return true;
}

static void PartD_SceneLanguage()
{
	std::printf( "=== [D] mirror through the CST derive: leaf, subtree, counts, container, inheritance ===\n" );

	const Matrix4 Fx = MirrorMx( 0 );
	const Matrix4 Fy = MirrorMx( 1 );

	// ---- D1: a LEAF mirror composes innermost, so `position` still places
	//          the reflected shape at the positive coordinate it names.
	{
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene(
			"standard_object\n{\nname L\ngeometry geo\nmaterial m\nposition 3 1 -2\nmirror x\n}\n" ), &diags );
		Matrix4 got;
		const Matrix4 expect = Matrix4Ops::Translation( Vector3( 3, 1, -2 ) )
			* Matrix4Ops::Identity() * Matrix4Ops::Identity() * Matrix4Ops::Identity() * Fx;
		std::string where;
		Check( diags.empty(), "D1: a leaf `mirror x` derives clean" );
		Check( WorldMx( j, "L", got ) && MxEq( got, expect, 0.0, &where ),
		       std::string( "D1: leaf world matrix == T(3,1,-2) * Mirror(x)" ) + where );
		j->release();
	}

	// ---- D2: `source` + `mirror` reflects the WHOLE cloned subtree, and a
	//          grandchild is reflected EXACTLY ONCE.
	{
		const std::string body =
			"standard_object\n{\nname root\nposition 0 0 0\n}\n"
			"standard_object\n{\nname arm\nparent root\ngeometry geo\nmaterial m\nposition 2 0 0\n}\n"
			"standard_object\n{\nname hand\nparent arm\ngeometry geo\nmaterial m\nposition 1 0.5 0\n}\n"
			"standard_object\n{\nname mroot\nsource root\nmirror x\n}\n";
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( body ), &diags );
		Check( diags.empty(), "D2: `source` + `mirror` on a container derives clean" );

		Matrix4 arm, hand, marm, mhand, mroot;
		const bool got = WorldMx( j, "arm", arm ) && WorldMx( j, "hand", hand )
		              && WorldMx( j, "mroot", mroot )
		              && WorldMx( j, "mroot.arm", marm ) && WorldMx( j, "mroot.hand", mhand );
		Check( got, "D2: the clone root and both clones exist (mroot, mroot.arm, mroot.hand)" );
		if( got ) {
			std::string where;
			Check( MxEq( mroot, Fx, 0.0, &where ),
			       std::string( "D2: the CONTAINER clone's own world matrix is the bare reflection" ) + where );
			Check( MxEq( marm, Matrix4( Fx * arm ), 0.0, &where ),
			       std::string( "D2: every clone vertex position is the reflection of its source counterpart "
			                    "-- clone.world == Mirror * source.world (child)" ) + where );
			Check( MxEq( mhand, Matrix4( Fx * hand ), 0.0, &where ),
			       std::string( "D2: ... and for the GRANDCHILD, i.e. reflected exactly ONCE, not once per level" ) + where );
			// The discriminating number: double-reflection would put the
			// grandchild at x = -1, single at x = -3.
			char buf[160];
			std::snprintf( buf, sizeof(buf), "D2: grandchild world x is -3 (single reflection), not -1 (double) -- got %.17g",
				(double)mhand._30 );
			Check( std::fabs( (double)mhand._30 - ( -3.0 ) ) < 1e-12, buf );
			Check( Matrix4Ops::Determinant( mhand ) < 0.0,
			       "D2: the grandchild clone's world transform is orientation-REVERSING (it inherited the reflection)" );
		}
		j->release();
	}

	// ---- D3: `count_u` + `mirror` mirrors EVERY repetition.
	{
		const std::string body =
			"standard_object\n{\nname unit\ngeometry geo\nmaterial m\nposition 1 0 0\n}\n"
			"standard_object\n{\nname R\nsource unit\ncount_u 3\nmirror x\nposition expr(i*4) 0 0\n}\n";
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( body ), &diags );
		Check( diags.empty(), "D3: `count_u` + `mirror` derives clean" );
		int ok = 0;
		for( int i = 0; i < 3; ++i ) {
			char nm[32];
			std::snprintf( nm, sizeof(nm), "R[%d,0]", i );
			Matrix4 got;
			const Matrix4 expect = Matrix4Ops::Translation( Vector3( i * 4.0, 0, 0 ) )
				* Matrix4Ops::Identity() * Matrix4Ops::Identity() * Matrix4Ops::Identity() * Fx;
			if( WorldMx( j, nm, got ) && MxEq( got, expect, 0.0 ) ) ++ok;
		}
		Check( ok == 3, "D3: all three repetitions are mirrored, each at its own per-instance position" );
		j->release();
	}

	// ---- D4: `mirror` is an INSTANCE-OWN param -- never inherited through
	//          `source`.  Both directions.
	{
		const std::string body =
			"standard_object\n{\nname W\ngeometry geo\nmaterial m\nposition 2 0 0\nmirror x\n}\n"
			"standard_object\n{\nname plainCopy\nsource W\nposition 5 0 0\n}\n"
			"standard_object\n{\nname yCopy\nsource W\nposition 5 0 0\nmirror y\n}\n";
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( body ), &diags );
		Check( diags.empty(), "D4: instancing a mirrored source derives clean" );

		Matrix4 plain, ycopy;
		const Matrix4 expectPlain = Matrix4Ops::Translation( Vector3( 5, 0, 0 ) )
			* Matrix4Ops::Identity() * Matrix4Ops::Identity() * Matrix4Ops::Identity() * Matrix4Ops::Identity();
		const Matrix4 expectY = Matrix4Ops::Translation( Vector3( 5, 0, 0 ) )
			* Matrix4Ops::Identity() * Matrix4Ops::Identity() * Matrix4Ops::Identity() * Fy;
		std::string where;
		Check( WorldMx( j, "plainCopy", plain ) && MxEq( plain, expectPlain, 0.0, &where ),
		       std::string( "D4: an un-mirrored instance of a MIRRORED source is NOT reflected "
		                    "(`mirror` is dropped with position/orientation/scale)" ) + where );
		Check( WorldMx( j, "yCopy", ycopy ) && MxEq( ycopy, expectY, 0.0, &where ),
		       std::string( "D4: an instance's OWN `mirror y` replaces the source's `mirror x` "
		                    "-- it does not compose with it (which would be a double mirror)" ) + where );
		j->release();
	}

	// ---- D5: `mirror` on a geometry-less CONTAINER is LEGAL, and a child
	//          that carries its OWN mirror composes through the parent's --
	//          it is not handed the parent's a second time.
	{
		const std::string body =
			"standard_object\n{\nname box_root\nposition 0 0 0\nmirror x\n}\n"
			"standard_object\n{\nname kid\nparent box_root\ngeometry geo\nmaterial m\nposition 1 0.5 0\nmirror x\n}\n";
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( body ), &diags );
		Check( diags.empty(), "D5: `mirror` on a CONTAINER derives clean (a container IS a transform)" );

		Matrix4 kid;
		// world = parent.world * kidLocal = Fx * ( T(1,.5,0) * Fx )
		const Matrix4 expect = Matrix4( Fx * Matrix4( Matrix4Ops::Translation( Vector3( 1, 0.5, 0 ) )
			* Matrix4Ops::Identity() * Matrix4Ops::Identity() * Matrix4Ops::Identity() * Fx ) );
		std::string where;
		Check( WorldMx( j, "kid", kid ) && MxEq( kid, expect, 0.0, &where ),
		       std::string( "D5: a child composes through the container's reflection ONCE, then applies its own" ) + where );
		Check( WorldMx( j, "kid", kid ) && Matrix4Ops::Determinant( kid ) > 0.0,
		       "D5: two reflections cancel -- the child's own `mirror x` under a mirrored parent is orientation-PRESERVING" );
		char buf[160];
		std::snprintf( buf, sizeof(buf), "D5: ... and its world position is (-1, 0.5, 0) -- got (%.17g %.17g %.17g)",
			(double)kid._30, (double)kid._31, (double)kid._32 );
		Check( std::fabs( (double)kid._30 + 1.0 ) < 1e-12 && std::fabs( (double)kid._31 - 0.5 ) < 1e-12, buf );
		j->release();
	}

	// ---- D6: a MIRRORED SOURCE ROOT.  D4 pins the parameter mechanics on a
	//          LEAF; this pins what those mechanics MEAN for a real assembly,
	//          because the answer is the opposite of what "copy" suggests and
	//          the docs now say so explicitly.
	//
	//          `mirror` is instance-own, so the clone root is built WITHOUT the
	//          source root's reflection while the subtree is rebuilt from its
	//          members' own chunks.  Therefore:
	//            * `source mroot`            -> the clone is the source's MIRROR IMAGE
	//            * `source mroot  mirror x`  -> the clone is an EXACT DUPLICATE
	//          Asserted RELATIVE TO EACH CLONE'S OWN ROOT, which is the only frame
	//          in which "copy" or "mirror image" means anything -- the roots sit at
	//          different world positions by construction.
	{
		const int warnBefore = g_mirrorSourceWarn ? g_mirrorSourceWarn->MatchCount() : -1;
		const std::string body =
			"standard_object\n{\nname mroot\nposition 0 0 0\nmirror x\n}\n"
			"standard_object\n{\nname mkid\nparent mroot\ngeometry geo\nmaterial m\nposition 2 0 0\n}\n"
			"standard_object\n{\nname copyPlain\nsource mroot\nposition 6 0 0\n}\n"
			"standard_object\n{\nname copyExact\nsource mroot\nposition 6 0 0\nmirror x\n}\n";
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( body ), &diags );
		Check( diags.empty(), "D6: instancing a MIRRORED ROOT derives clean -- both readings are legal scenes" );

		Matrix4 srcKid, plainKid, exactKid;
		const bool got = WorldMx( j, "mkid", srcKid )
		              && WorldMx( j, "copyPlain.mkid", plainKid )
		              && WorldMx( j, "copyExact.mkid", exactKid );
		Check( got, "D6: the source child and both clone children exist" );
		if( got ) {
			// The source's own child: reflected through its parent, so x = -2.
			char buf[224];
			std::snprintf( buf, sizeof(buf), "D6: (premise) the SOURCE child sits at x = -2 -- it composes through "
				"its parent's `mirror x` (got %.17g)", (double)srcKid._30 );
			Check( std::fabs( (double)srcKid._30 + 2.0 ) < 1e-12, buf );

			// The plain clone: root's mirror DROPPED, so its child is at +2
			// relative to the clone root (world 6 + 2 = 8) and its transform is
			// orientation-PRESERVING.  That is the mirror image of the source.
			std::snprintf( buf, sizeof(buf), "D6: a PLAIN `source` clone of a mirrored root is the source's MIRROR "
				"IMAGE -- its child sits at +2 from the clone root (world x = 8), where the source's sits at -2 "
				"(got world x %.17g)", (double)plainKid._30 );
			Check( std::fabs( (double)plainKid._30 - 8.0 ) < 1e-12, buf );
			Check( Matrix4Ops::Determinant( plainKid ) > 0.0,
			       "D6: ... and it is orientation-PRESERVING, where the source's child is reversing" );

			// The `mirror x` clone: an EXACT duplicate, placed at the clone
			// root's position.  Equality with `T(6,0,0) * source.world` is the
			// strongest form of "same shape, moved" -- the source root is at the
			// origin, so its world frame IS its own frame.
			std::string where;
			Check( MxEq( exactKid, Matrix4( Matrix4Ops::Translation( Vector3( 6, 0, 0 ) ) * srcKid ), 1e-12, &where ),
			       std::string( "D6: repeating the source's OWN `mirror x` on the instance reproduces it EXACTLY "
			                    "-- clone.world == T(clonePos) * source.world for every subtree member" ) + where );
			Check( Matrix4Ops::Determinant( exactKid ) < 0.0,
			       "D6: ... including its handedness" );
		}

		// THE ADVISORY.  It must fire for `copyPlain` (source mirrored, instance
		// not) and NOT for `copyExact`, so exactly ONE new message across a scene
		// that contains both -- a count, not a presence check, because "warns on
		// everything" is as wrong as "warns on nothing".
		if( g_mirrorSourceWarn ) {
			const int fired = g_mirrorSourceWarn->MatchCount() - warnBefore;
			char buf[160];
			std::snprintf( buf, sizeof(buf), "D6: the derive warns EXACTLY ONCE -- for the plain clone, not the "
				"`mirror x` one (fired %d times)", fired );
			Check( fired == 1, buf );
			const std::string msg = g_mirrorSourceWarn->LastMatch();
			Check( msg.find( "copyPlain" ) != std::string::npos,
			       "D6: ... and the warning NAMES the instancing chunk to fix" );
			Check( msg.find( "Add `mirror x` here" ) != std::string::npos,
			       "D6: ... and spells the one-token fix, with the source's own axis" );
		}
		j->release();
	}
}

//////////////////////////////////////////////////////////////////////
// [E] REFUSALS
//////////////////////////////////////////////////////////////////////

static void PartE_Refusals()
{
	std::printf( "=== [E] an axis that is not x / y / z is refused BY NAME ===\n" );

	struct Case { const char* value; const char* what; };
	const Case cases[] = {
		{ "w",  "E: `mirror w` is refused" },
		{ "X",  "E: `mirror X` is refused -- the descriptor advertises lower case only" },
		{ "xy", "E: `mirror xy` is refused" },
		{ "0",  "E: `mirror 0` is refused (an axis is a name here, not an index)" }
	};
	for( const Case& c : cases ) {
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene( std::string(
			"standard_object\n{\nname L\ngeometry geo\nmaterial m\nmirror " ) + c.value + "\n}\n" ), &diags );
		std::string all;
		for( std::size_t i = 0; i < diags.size(); ++i ) { all += diags[i]; all += "\n"; }
		Check( !diags.empty(), c.what );
		Check( all.find( "must be `x`, `y` or `z`" ) != std::string::npos,
		       std::string( c.what ) + " -- and the diagnostic NAMES the accepted set" );
		Check( all.find( "mirror" ) != std::string::npos,
		       std::string( c.what ) + " -- and names the parameter, so the author knows which line to fix" );
		j->release();
	}

	// `mirror none` is the explicit "no mirror" spelling and must derive.
	{
		std::vector<std::string> diags;
		Job* j = DeriveJob( Scene(
			"standard_object\n{\nname L\ngeometry geo\nmaterial m\nposition 2 0 0\nmirror none\n}\n" ), &diags );
		Matrix4 got;
		Check( diags.empty(), "E: `mirror none` is the explicit clear and derives clean" );
		Check( WorldMx( j, "L", got ) && Matrix4Ops::Determinant( got ) > 0.0,
		       "E: ... and leaves the object un-reflected" );
		j->release();
	}

	// THE PUBLISHED SET MUST BE THE ACCEPTED SET.  `enumValues` is what the
	// syntax highlighter, the schema the agent reads, and the panel's preset
	// list all show; the four refusals above and the `mirror none` case below
	// them are what the parser actually does.  A published set that omits a
	// spelling the parser accepts is the same drift class the descriptor-driven
	// parsers exist to make structurally impossible -- so assert the two agree
	// on `none`, the one value that is accepted but is not an axis.
	{
		const ChunkDescriptor* desc = DescriptorForKeyword( String( "standard_object" ) );
		Check( desc != 0, "E: (setup) the standard_object descriptor is registered" );
		bool found = false, hasNone = false, hasX = false;
		if( desc ) {
			for( std::size_t i = 0; i < desc->parameters.size(); ++i ) {
				if( desc->parameters[i].name != "mirror" ) continue;
				found = true;
				for( std::size_t k = 0; k < desc->parameters[i].enumValues.size(); ++k ) {
					if( desc->parameters[i].enumValues[k] == "none" ) hasNone = true;
					if( desc->parameters[i].enumValues[k] == "x" )    hasX    = true;
				}
			}
		}
		Check( found, "E: the descriptor declares a `mirror` parameter" );
		Check( hasX, "E: ... whose enumValues publish the axes" );
		Check( hasNone, "E: ... AND `none`, which the parser accepts (above) and Job::SetObjectMirror decodes" );
	}
}

//////////////////////////////////////////////////////////////////////
// [F] THE RUNTIME IJob SURFACE
//////////////////////////////////////////////////////////////////////

// `IJob::SetObjectMirror` is what the parser calls, and it is also public API
// surface (the Blender / PRISE embedding, the console) with no derive behind
// it.  Its contract is deliberately SetObjectParent's and not
// SetObjectPosition's: it finalizes THIS node and invalidates, but it does NOT
// walk the hierarchy -- the parser calls it once per object chunk, so a walk
// per call would be O(N^2) over a derive (measured 1.10 s vs 0.07 s on a
// 1500-mirrored-child scene).  The descendants are composed by the caller's own
// walk: the derive's tail (proved by [D]) or, for an API caller, the per-frame
// `RebakeHierarchy()` inside ObjectManager::Prepare that every render and pick
// goes through.  `Job::ComposeObjectHierarchy()` is that same walk, reachable
// from a test.
static void PartF_RuntimeApi()
{
	std::printf( "=== [F] IJob::SetObjectMirror -- the runtime / API entry point ===\n" );

	const Matrix4 Fx = MirrorMx( 0 );
	const std::string body =
		"standard_object\n{\nname root\nposition 1 0 0\n}\n"
		"standard_object\n{\nname kid\nparent root\ngeometry geo\nmaterial m\nposition 2 0.5 0\n}\n";
	std::vector<std::string> diags;
	Job* j = DeriveJob( Scene( body ), &diags );
	Check( diags.empty(), "F: (setup) the un-mirrored parented scene derives clean" );

	Matrix4 rootBefore, kidBefore;
	Check( WorldMx( j, "root", rootBefore ) && WorldMx( j, "kid", kidBefore ),
	       "F: (setup) both nodes resolve" );

	// A BAD AXIS CHANGES NOTHING -- refuse-before-mutate.
	Check( !j->SetObjectMirror( "root", "w" ), "F: an illegal axis is refused" );
	Check( !j->SetObjectMirror( "no_such_object", "x" ), "F: an unknown object is refused" );
	Matrix4 rootAfterRefusal;
	Check( WorldMx( j, "root", rootAfterRefusal ) && MxEq( rootAfterRefusal, rootBefore, 0.0 ),
	       "F: ... and a refused call leaves the transform untouched" );

	// SET.  The node itself is reflected IMMEDIATELY (it is finalized in the
	// call); the subtree follows the caller's walk.
	Check( j->SetObjectMirror( "root", "x" ), "F: `x` is accepted" );
	Matrix4 rootMirrored;
	std::string where;
	Check( WorldMx( j, "root", rootMirrored ) && MxEq( rootMirrored, Matrix4( rootBefore * Fx ), 0.0, &where ),
	       std::string( "F: the node's own world matrix is reflected in the call itself" ) + where );

	j->ComposeObjectHierarchy();   // what a derive's tail and Prepare's RebakeHierarchy both do
	Matrix4 kidAfter;
	Check( WorldMx( j, "kid", kidAfter )
	       && MxEq( kidAfter, Matrix4( Matrix4( rootBefore * Fx ) * Matrix4Ops::Translation( Vector3( 2, 0.5, 0 ) ) ), 1e-12 ),
	       "F: the DESCENDANT composes through the reflection once the caller's hierarchy walk runs" );

	// IDEMPOTENT: setting the same axis again is a no-op, not a second
	// reflection.  This is the early-return in Job::SetObjectMirror, and it is
	// what keeps the parser's unconditional per-chunk call cheap.
	Check( j->SetObjectMirror( "root", "x" ), "F: re-setting the same axis succeeds" );
	Matrix4 rootAgain;
	Check( WorldMx( j, "root", rootAgain ) && MxEq( rootAgain, rootMirrored, 0.0 ),
	       "F: ... and does NOT reflect a second time" );

	// CLEAR, both spellings.
	Check( j->SetObjectMirror( "root", "none" ), "F: `none` clears the mirror" );
	Matrix4 rootCleared;
	Check( WorldMx( j, "root", rootCleared ) && MxEq( rootCleared, rootBefore, 0.0 ),
	       "F: ... returning the node to its un-reflected transform" );
	Check( j->SetObjectMirror( "root", "z" ) && j->SetObjectMirror( "root", 0 ),
	       "F: a NULL axis also clears" );
	Check( WorldMx( j, "root", rootCleared ) && MxEq( rootCleared, rootBefore, 0.0 ),
	       "F: ... to the same un-reflected transform" );

	j->release();

	// A csg_object is REFUSED BY NAME.  Unreachable from a scene file -- the
	// `csg_object` grammar declares no `mirror` param, so the descriptor-driven
	// parser rejects the line first -- but IJob is public API and the console can
	// name any object, and a CSGObject IS a Transformable, so nothing in the call
	// itself would stop it.  What stops it is that a composite's transform is saved
	// as `position` + `orientation` (csg_object has no `matrix` param), and neither
	// can express a reflection: the mirror would RENDER and could never be written
	// back, diverging the live scene from its file.
	{
		const std::string body =
			"standard_object\n{\nname opA\ngeometry geo\nmaterial m\n}\n"
			"standard_object\n{\nname opB\ngeometry geo\nmaterial m\nposition 0.2 0 0\n}\n"
			"csg_object\n{\nname comp\nobja opA\nobjb opB\noperation union\nmaterial m\n}\n";
		std::vector<std::string> diags;
		Job* jc = DeriveJob( Scene( body ), &diags );
		Check( diags.empty(), "F: (setup) the csg fixture derives clean" );
		Matrix4 compBefore;
		Check( WorldMx( jc, "comp", compBefore ), "F: (setup) the composite resolves" );
		Check( !jc->SetObjectMirror( "comp", "x" ),
		       "F: a csg_object is REFUSED a mirror -- its transform is saved as position + orientation, "
		       "which cannot express a reflection" );
		Matrix4 compAfter;
		Check( WorldMx( jc, "comp", compAfter ) && MxEq( compAfter, compBefore, 0.0 ),
		       "F: ... and the refusal changed nothing" );
		Check( jc->SetObjectMirror( "comp", "none" ),
		       "F: ... while CLEARING is still accepted -- `none` is the state it is already in, so nothing "
		       "unsaveable can be reached through it" );
		jc->release();
	}
}

//////////////////////////////////////////////////////////////////////
// [G] THE GIZMO COMMIT
//////////////////////////////////////////////////////////////////////

// A GUI transform gesture on a `standard_object` commits by writing the object's
// LOCAL matrix into the chunk's `matrix` param
// (SceneEditor::CommitPendingCstObjectTransforms, kind 1).  `mirror` is NOT in
// standard_object's matrix > quaternion > orientation precedence chain -- it
// composes with whichever of them wins, and ApplyCstObjectMatrixEdit
// deliberately does not strip it -- so a committed matrix that still contained
// the reflection would be re-multiplied by the surviving `mirror` line on the
// next derive and the object would come back reflected TWICE.
//
// Pinned by DETERMINANT, not by a coordinate: a double reflection is
// orientation-PRESERVING, so `det > 0` after the round-trip is the exact
// signature of the bug, and it cannot be satisfied by any correct answer.
static void PartG_GizmoCommitRoundTrip()
{
	std::printf( "=== [G] a gizmo drag on a mirrored object commits without double-reflecting it ===\n" );

	using Cat = SceneEditController::Category;

	const std::string scene = Scene(
		"standard_object\n{\nname mo\ngeometry geo\nmaterial m\nposition 1 0 0\nmirror x\n}\n" );
	const char* path = "objectmirror_gizmo.RISEscene";
	{ std::ofstream o( path, std::ios::binary ); o << scene; }

	Job* j = new Job();
	const bool loaded = j->LoadAsciiSceneViaCst( path );
	Check( loaded, "G: the mirrored fixture loads with a retained CST head" );
	if( loaded ) {
		Matrix4 before;
		Check( WorldMx( j, "mo", before ) && Matrix4Ops::Determinant( before ) < 0.0,
		       "G: (premise) the object starts out REFLECTED" );

		SceneEditController c( *j, 0 );
		c.SetSelection( Cat::Object, String( "mo" ) );
		const bool moved = c.SetPropertyForCategory( Cat::Object, String( "position" ), String( "5 0 0" ) );
		Check( moved, "G: a translate through the controller is accepted on a mirrored standard_object" );

		Matrix4 after;
		const bool got = WorldMx( j, "mo", after );
		const Matrix4 expect = Matrix4Ops::Translation( Vector3( 5, 0, 0 ) )
			* Matrix4Ops::Identity() * Matrix4Ops::Identity() * Matrix4Ops::Identity() * MirrorMx( 0 );
		char buf[192];
		std::snprintf( buf, sizeof(buf),
			"G: after the commit RE-DERIVES, the object is still reflected exactly ONCE (det %.6g must stay negative; "
			"a second reflection would make it positive)", got ? (double)Matrix4Ops::Determinant( after ) : 0.0 );
		Check( got && Matrix4Ops::Determinant( after ) < 0.0, buf );
		std::string where;
		Check( got && MxEq( after, expect, 1e-9, &where ),
		       std::string( "G: ... and it landed at the committed position with the mirror intact" ) + where );

		// ---- G2: the ORIENTATION commit, and the NO-OP form of it.
		//
		// The READ path un-applies the mirror before decomposing, because
		// DecomposeFinalAffine builds a PROPER frame and would otherwise report a
		// phantom `orientation 0 180 0` for an object that carries no rotation at
		// all.  The WRITE path has to decompose the SAME matrix, or the most
		// innocuous gesture in the panel -- open the row, press Enter on the value
		// it is already showing -- REWRITES the object: the phantom comes back as a
		// real 180-degree y-rotation and the reflection is consumed paying for it.
		//
		// So: read the row, commit it verbatim, and require the world matrix to be
		// BYTE-STABLE.  A no-op that is not a no-op is invisible to every assertion
		// that checks a value against an expectation, because the value it produces
		// IS internally consistent -- it is just not the object the author had.
		{
			String shownOrientation;
			const IObject* o = Obj( j, "mo" );
			Check( o != 0, "G2: (setup) the object resolves before the read" );
			if( o ) {
				const std::vector<CameraProperty> rows =
					ObjectIntrospection::Inspect( String( "mo" ), *o, 0, 0, j );
				Check( RowValue( rows, "orientation", shownOrientation ),
				       "G2: (setup) the panel has an `orientation` row" );
			}
			double ox = 9, oy = 9, oz = 9;
			Check( std::sscanf( shownOrientation.c_str(), "%lf %lf %lf", &ox, &oy, &oz ) == 3,
			       "G2: (setup) the row parses as three numbers" );
			char buf[224];
			std::snprintf( buf, sizeof(buf), "G2: the panel shows NO rotation on an unrotated `mirror x` object "
				"(got `%s`) -- a phantom 0 180 0 is what a PROPER-frame decomposition of the un-corrected local "
				"matrix reports", shownOrientation.c_str() );
			Check( std::fabs( ox ) < 1e-9 && std::fabs( oy ) < 1e-9 && std::fabs( oz ) < 1e-9, buf );

			Matrix4 preNoop;
			Check( WorldMx( j, "mo", preNoop ), "G2: (setup) the pre-commit matrix reads back" );
			const bool committed =
				c.SetPropertyForCategory( Cat::Object, String( "orientation" ), shownOrientation );
			Check( committed, "G2: committing the orientation the panel just showed is accepted" );
			Matrix4 postNoop;
			const bool gotNoop = WorldMx( j, "mo", postNoop );
			std::string whereN;
			Check( gotNoop && MxEq( postNoop, preNoop, 1e-9, &whereN ),
			       std::string( "G2: ... and it is a genuine NO-OP -- the world matrix round-trips through the "
			                    "commit and re-derive unchanged" ) + whereN );
			std::snprintf( buf, sizeof(buf), "G2: ... including its HANDEDNESS: the object is still reflected "
				"exactly once (det %.6g must stay negative; un-reflecting it flips the sign)",
				gotNoop ? (double)Matrix4Ops::Determinant( postNoop ) : 0.0 );
			Check( gotNoop && Matrix4Ops::Determinant( postNoop ) < 0.0, buf );
		}

		// ---- G3: a REAL scale commit.  `SetAbsoluteStretch_` reads the current
		//          local matrix, strips its scale, and multiplies the requested one
		//          in.  Reading the un-corrected matrix carries an M into the base,
		//          and M * Stretch * M == Stretch for a diagonal stretch -- so the
		//          two reflections cancel and the object comes back UN-mirrored at
		//          det +2 instead of -2.  Pinned by the determinant's SIGN and by
		//          its MAGNITUDE: the sign catches the cancellation, the magnitude
		//          catches a mirror applied on the wrong side of the scale.
		{
			const bool scaled = c.SetPropertyForCategory( Cat::Object, String( "scale" ), String( "2 1 1" ) );
			Check( scaled, "G3: a `scale 2 1 1` commit is accepted on a mirrored object" );
			Matrix4 afterScale;
			const bool gotS = WorldMx( j, "mo", afterScale );
			const double det = gotS ? (double)Matrix4Ops::Determinant( afterScale ) : 0.0;
			char buf[224];
			std::snprintf( buf, sizeof(buf), "G3: `scale 2 1 1` on a mirrored object lands at det = -2 "
				"(got %.17g); +2 is the mirror cancelling itself against the one finalize re-appends", det );
			Check( gotS && std::fabs( det + 2.0 ) < 1e-9, buf );
			const Matrix4 expectScale = Matrix4Ops::Translation( Vector3( 5, 0, 0 ) )
				* Matrix4Ops::Stretch( Vector3( 2, 1, 1 ) ) * MirrorMx( 0 );
			std::string whereS;
			Check( gotS && MxEq( afterScale, expectScale, 1e-9, &whereS ),
			       std::string( "G3: ... and the matrix is exactly T(5,0,0) * Stretch(2,1,1) * Mirror(x)" ) + whereS );
		}

		// ---- G4: the GIZMO's scale drag, and the UNDO of it.
		//
		// `ScaleObjectFromAnchor` is the one transform op CaptureForApply does NOT
		// snapshot (its `prevTransform` is the controller's drag-start anchor, which
		// must survive every per-frame Apply), so `hasTransformState` is false and
		// its undo lands in RestoreObjectTransform's FALLBACK -- the branch that
		// calls ClearAllTransforms, which now also clears the mirror.  Restoring the
		// baked-in anchor there brings the POSE back while leaving `mirrorAxis` at
		// none, and the next commit then writes a reflection with no `mirror` line
		// beside it.  So assert both halves: the matrix AND the axis.
		{
			IObjectPriv* op = j->GetObjects() ? j->GetObjects()->GetItem( "mo" ) : 0;
			Check( op != 0, "G4: (setup) the object resolves as IObjectPriv" );
			if( op ) {
				const Matrix4 anchorLocal = op->GetLocalTransformMatrix();
				Matrix4 preDrag;
				Check( WorldMx( j, "mo", preDrag ), "G4: (setup) the pre-drag world matrix reads back" );
				const double preDet = (double)Matrix4Ops::Determinant( preDrag );

				SceneEdit drag;
				drag.op            = SceneEdit::ScaleObjectFromAnchor;
				drag.objectName    = String( "mo" );
				drag.prevTransform = anchorLocal;                 // what the controller captures at drag start
				drag.v3a           = Vector3( 2, 2, 2 );
				Check( c.Editor().Apply( drag ), "G4: a gizmo scale drag applies on a mirrored object" );

				Matrix4 dragged;
				const bool gotD = WorldMx( j, "mo", dragged );
				const double dragDet = gotD ? (double)Matrix4Ops::Determinant( dragged ) : 0.0;
				char buf[224];
				std::snprintf( buf, sizeof(buf), "G4: a uniform x2 drag multiplies the determinant by 8 and KEEPS "
					"its sign (%.17g -> %.17g; want %.17g)", preDet, dragDet, preDet * 8.0 );
				Check( gotD && std::fabs( dragDet - preDet * 8.0 ) < 1e-9 * 8.0 * std::fabs( preDet ), buf );

				Check( c.Editor().Undo(), "G4: the drag undoes" );
				Matrix4 undone;
				const bool gotU = WorldMx( j, "mo", undone );
				std::string whereU;
				Check( gotU && MxEq( undone, preDrag, 1e-9, &whereU ),
				       std::string( "G4: ... restoring the drag-start pose" ) + whereU );
				const Implementation::Transformable* tf =
					dynamic_cast<const Implementation::Transformable*>( Obj( j, "mo" ) );
				std::snprintf( buf, sizeof(buf), "G4: ... AND the authored mirror axis (got %d, want 0) -- the "
					"fallback restore clears the mirror before pushing an anchor that already contains it",
					tf ? tf->GetMirrorAxis() : -99 );
				Check( tf && tf->GetMirrorAxis() == 0, buf );
			}
		}

		// ---- G5: the `mirror` PROPERTY ROW itself.  The panel has read it back
		//          since the slice landed; without a writer arm in
		//          SceneEditController::SetPropertyInner_ it offered an edit that
		//          could only be refused.
		{
			Check( c.SetPropertyForCategory( Cat::Object, String( "mirror" ), String( "y" ) ),
			       "G5: committing `mirror y` through the property panel is accepted" );
			Matrix4 my;
			const bool gotY = WorldMx( j, "mo", my );
			Check( gotY && Matrix4Ops::Determinant( my ) < 0.0,
			       "G5: ... and the object is still reflected" );
			Check( gotY && (double)my._11 < 0.0 && (double)my._00 > 0.0,
			       "G5: ... about the Y axis now, not X" );

			Check( c.SetPropertyForCategory( Cat::Object, String( "mirror" ), String( "none" ) ),
			       "G5: `none` clears it through the same row" );
			Matrix4 mn;
			Check( WorldMx( j, "mo", mn ) && Matrix4Ops::Determinant( mn ) > 0.0,
			       "G5: ... leaving the object un-reflected" );

			Check( !c.SetPropertyForCategory( Cat::Object, String( "mirror" ), String( "q" ) ),
			       "G5: an axis the descriptor does not publish is REFUSED, not silently mapped" );
		}
	}
	j->release();
	std::remove( path );
}

//////////////////////////////////////////////////////////////////////
// [H] THE PROPERTIES-PANEL READ PATH
//////////////////////////////////////////////////////////////////////

// The introspection layer decomposes the object's LOCAL matrix into the
// position / orientation / scale rows the panel shows.  A reflection has no
// place in a PROPER rotation frame, so decomposing a mirrored object's matrix
// without un-applying the mirror first reports a rotation the author never
// wrote -- and the panel then offers to commit it.  [G2] pins that the commit
// is a no-op; this pins what the panel SHOWS in the first place, which is the
// half a user reads before deciding anything.
static void PartH_IntrospectionReadBack()
{
	std::printf( "=== [H] the properties panel reads a mirrored object without a phantom rotation ===\n" );

	std::vector<std::string> diags;
	Job* j = DeriveJob( Scene(
		"standard_object\n{\nname mo\ngeometry geo\nmaterial m\nposition 1 2 3\nmirror x\n}\n" ), &diags );
	Check( diags.empty(), "H: (setup) the fixture derives clean" );

	const IObject* o = Obj( j, "mo" );
	Check( o != 0, "H: (setup) the object resolves" );
	if( o ) {
		const std::vector<CameraProperty> rows =
			ObjectIntrospection::Inspect( String( "mo" ), *o, 0, 0, j );

		String orientation, position, mirror;
		Check( RowValue( rows, "orientation", orientation ), "H: the panel has an `orientation` row" );
		Check( RowValue( rows, "position", position ),       "H: the panel has a `position` row" );

		double ox = 9, oy = 9, oz = 9;
		char buf[224];
		std::snprintf( buf, sizeof(buf), "H: an UNROTATED `mirror x` object reads back orientation 0 0 0, not the "
			"phantom 0 180 0 a proper-frame decomposition of the un-corrected matrix produces (got `%s`)",
			orientation.c_str() );
		Check( std::sscanf( orientation.c_str(), "%lf %lf %lf", &ox, &oy, &oz ) == 3
		    && std::fabs( ox ) < 1e-9 && std::fabs( oy ) < 1e-9 && std::fabs( oz ) < 1e-9, buf );

		double px = 9, py = 9, pz = 9;
		std::snprintf( buf, sizeof(buf), "H: ... and the position row is the AUTHORED 1 2 3 (got `%s`) -- "
			"un-applying the mirror must not disturb the translation", position.c_str() );
		Check( std::sscanf( position.c_str(), "%lf %lf %lf", &px, &py, &pz ) == 3
		    && std::fabs( px - 1.0 ) < 1e-9 && std::fabs( py - 2.0 ) < 1e-9 && std::fabs( pz - 3.0 ) < 1e-9, buf );

		// The MIRROR row itself: the axis is the one thing the decomposition
		// deliberately cannot show, so it has to be surfaced on its own.
		const bool hasMirror = RowValue( rows, "mirror", mirror );
		Check( hasMirror, "H: the panel exposes a `mirror` row" );
		Check( hasMirror && mirror == String( "x" ),
		       std::string( "H: ... reading back the AUTHORED axis (got `" ) + mirror.c_str() + "`)" );
	}
	j->release();
}

int main()
{
	std::printf( "=== ObjectMirrorTest: doc 89 slice C, `mirror x|y|z` on standard_object ===\n" );

	// Installed for the whole run, BEFORE the first derive: [D6] counts the
	// derive-time `source` + `mirror` advisory, which is a warning (both readings
	// of that scene are legal) and so never reaches the `diags` bag.
	{
		CapturingLogPrinter* owned = new CapturingLogPrinter( "will be the MIRROR IMAGE of what you see" );
		RISE::GlobalLogPriv()->AddPrinter( owned );
		g_mirrorSourceWarn = owned;      // AddPrinter addref'd it; keep a raw read handle
		safe_release( owned );           // drop OUR construction ref (safe_release nulls its arg)
	}

	PartA_WorldMatrixExactness();
	PartB_HandReflectedTwin();
	PartB2_TangentHandedness();
	PartB3_DerivativeFallbackHandedness();
	PartC_EmitterSamplingPath();
	PartD_SceneLanguage();
	PartE_Refusals();
	PartF_RuntimeApi();
	PartG_GizmoCommitRoundTrip();
	PartH_IntrospectionReadBack();

	std::printf( "\n%d passed, %d failed.\n", g_pass, g_fail );
	if( g_fail ) {
		std::printf( "FAILED\n" );
		return 1;
	}
	std::printf( "All ObjectMirrorTest checks passed!\n" );
	return 0;
}
