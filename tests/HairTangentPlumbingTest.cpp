//////////////////////////////////////////////////////////////////////
//
//  HairTangentPlumbingTest.cpp - Slice C2 of the hair / fur system
//    (docs/HAIR_FUR_DESIGN.md section 4.1): validates that
//    Object::IntersectRay and CSGObject::IntersectRay honour a
//    geometry-supplied fiber tangent (RayIntersectionGeometric::
//    vShadingTangent / bHasShadingTangent), which HairGeometry (C1)
//    writes but which, before this slice, sat unread -- every hair
//    hit built its shading ONB from the legacy, curve-unaware
//    world-X projection instead.
//
//  Coverage:
//
//    1. TestObjectHairThroughTransform -- a real HairGeometry (a
//       straight 2-control-point strand, exactly a line by the
//       reflected-phantom-endpoint Catmull-Rom convention documented
//       in HairGeometry.h) wrapped in an Object under a rotation +
//       translation + non-uniform scale.  The reported onb.u() must
//       align with the OBJECT-SPACE tangent forward-transformed by
//       the object's own final matrix (the exact idiom vTangent
//       uses) -- not the world-X projection.  onb.w() must equal the
//       reported world shading normal, and the ONB must be a
//       right-handed orthonormal frame.
//    2. TestObjectHairIdentity -- same strand, identity transform:
//       onb.u() must match the object-space tangent directly.
//    3. TestObjectLegacyGuardNoSuppliedTangent -- a geometry that
//       sets bShadingTangentFromGeometry WITHOUT bHasShadingTangent
//       (the pre-existing SDFGeometry heightfield-mode consumer,
//       reused verbatim from CsgSurfacePayloadTest's Test 10) must
//       still get the untouched legacy world-X projection.
//    4. TestObjectDegenerateSuppliedTangentFallsBack -- a synthetic
//       stub geometry (see StubTangentGeometry below) supplies a
//       tangent EXACTLY parallel to its normal.  HairGeometry itself
//       can never produce this (its tangent T is perpendicular to
//       its own shading normal Ncyl by construction, and that
//       orthogonality survives ANY invertible linear transform: if
//       n . t == 0 in object space then, for world normal
//       n' = (M^-T) n and world tangent t' = M t,
//       n' . t' = n^T M^-1 M t = n . t == 0 exactly) -- so a
//       hand-built stub is the only way to exercise Object::
//       IntersectRay's degeneracy fallback.  Must fall back cleanly
//       to the legacy world-X projection with no NaN anywhere in the
//       ONB.
//    5. TestCsgPassThroughHonoursSuppliedTangent -- the
//       StubTangentGeometry (real, non-degenerate tangent) as CSG
//       operand A, a never-hit operand B, and a rotation +
//       translation + stretch on the CSG object itself.  Exercises
//       CSGObject::IntersectRay's byte-duplicate ONB block AND the
//       vShadingTangent write-back fix (the field must already carry
//       operand A's own Object-level promotion by the time the CSG's
//       own tail block reads and further transforms it -- exactly
//       the vTangent convention CSGObject.cpp documents).
//    6. TestCsgAdoptSurfacePayloadCopiesSuppliedTangent -- a
//       CSG_INTERSECTION "outside both, A enters first, B enters
//       while inside A" boundary (mirrors CsgSurfacePayloadTest's
//       Test 1 nested-box scenario), where B is the tangent-bearing
//       stub and A is not.  `ri` starts life as A's whole-record copy
//       (bHasShadingTangent == false) and AdoptCsgSurfacePayload then
//       re-adopts B's geometric payload for the reported boundary --
//       this is the exact mixed-surface-payload gap found in review
//       while landing this slice: AdoptCsgSurfacePayload copied
//       bShadingTangentFromGeometry but NOT vShadingTangent /
//       bHasShadingTangent, so a boundary correctly flagged
//       "geometry-supplied" could still read A's stale (or absent)
//       tangent.  Regression guard for that fix.
//
//  Given a HairGeometry hit, cases 1/2 do not need the degenerate
//  fallback (see case 4's derivation of why it is geometrically
//  unreachable there) -- the stub exists specifically to reach the
//  code paths a real curve hit cannot.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <iostream>
#include <vector>

#include "../src/Library/Functions/ConstantFunctions.h"
#include "../src/Library/Geometry/Geometry.h"
#include "../src/Library/Geometry/HairGeometry.h"
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Objects/Object.h"

using namespace RISE;
using namespace RISE::Implementation;

// ============================================================
//  Harness
// ============================================================

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

namespace
{
	const Scalar kEps = 1e-6;

	bool Close( Scalar a, Scalar b, Scalar eps = kEps )
	{
		return std::fabs( a - b ) < eps;
	}

	bool VecClose( const Vector3& a, const Vector3& b, Scalar eps = kEps )
	{
		return Close( a.x, b.x, eps ) && Close( a.y, b.y, eps ) && Close( a.z, b.z, eps );
	}

	bool VecFinite( const Vector3& v )
	{
		return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
	}

	//! Standard test-harness intersection call: reset the scratch record,
	//! stamp the ray, and run the full (front+back faces, exit info) query
	//! -- same convention as CsgSurfacePayloadTest's Hit() helper.
	void Hit( IObject* pObj, const Ray& r, RayIntersection& ri )
	{
		ri.geometric.bHit = false;
		ri.geometric.range = RISE_INFINITY;
		ri.geometric.range2 = RISE_INFINITY;
		ri.geometric.ray = r;
		pObj->IntersectRay( ri, RISE_INFINITY, true, true, true );
	}

	//! Checks {u,v,w} is a right-handed orthonormal frame.
	void CheckOrthonormalRightHanded( const OrthonormalBasis3D& onb, const char* tag )
	{
		char buf[256];
		std::snprintf( buf, sizeof(buf), "%s: onb.u() is unit", tag );
		Check( Close( Vector3Ops::Magnitude( onb.u() ), 1.0, 1e-9 ), buf );
		std::snprintf( buf, sizeof(buf), "%s: onb.v() is unit", tag );
		Check( Close( Vector3Ops::Magnitude( onb.v() ), 1.0, 1e-9 ), buf );
		std::snprintf( buf, sizeof(buf), "%s: onb.w() is unit", tag );
		Check( Close( Vector3Ops::Magnitude( onb.w() ), 1.0, 1e-9 ), buf );
		std::snprintf( buf, sizeof(buf), "%s: u . v ~ 0", tag );
		Check( Close( Vector3Ops::Dot( onb.u(), onb.v() ), 0.0, 1e-9 ), buf );
		std::snprintf( buf, sizeof(buf), "%s: u . w ~ 0", tag );
		Check( Close( Vector3Ops::Dot( onb.u(), onb.w() ), 0.0, 1e-9 ), buf );
		std::snprintf( buf, sizeof(buf), "%s: v . w ~ 0", tag );
		Check( Close( Vector3Ops::Dot( onb.v(), onb.w() ), 0.0, 1e-9 ), buf );
		std::snprintf( buf, sizeof(buf), "%s: right-handed (u x v ~ w)", tag );
		Check( VecClose( Vector3Ops::Cross( onb.u(), onb.v() ), onb.w(), 1e-6 ), buf );
	}

	//////////////////////////////////////////////////////////////
	//
	//  StubTangentGeometry -- a minimal hand-built IGeometry (via
	//  the Geometry convenience base) that reports a synthetic hit
	//  with a CALLER-CHOSEN normal and (optionally) a CALLER-CHOSEN
	//  supplied shading tangent, at a CALLER-CHOSEN pair of ray
	//  parameters (entry/exit).  Exists to reach two code paths a
	//  real HairGeometry hit cannot:
	//    (a) a supplied tangent that is degenerate (parallel to the
	//        normal) -- geometrically unreachable from a real curve
	//        hit, see the file header's orthogonality argument;
	//    (b) a nested CSG boundary scenario with a KNOWN, exact
	//        entry/exit range pair, so the CSG algebra branch under
	//        test is picked deterministically instead of depending
	//        on incidental curve/BVH numerics.
	//
	//  Deliberately not geometrically self-consistent (the ray is
	//  otherwise ignored beyond the entry/exit ranges reported) --
	//  this is a plumbing/flag test, not a geometric-validity test.
	//////////////////////////////////////////////////////////////
	class StubTangentGeometry : public virtual Geometry
	{
	public:
		StubTangentGeometry(
			const Vector3& objNormal,
			bool bShadingTangentFromGeometry_,
			bool bHasShadingTangent_,
			const Vector3& objShadingTangent,
			Scalar tEnter_ = 1.0,
			Scalar tExit_ = 1.0
			) :
		n( Vector3Ops::Normalize( objNormal ) ),
		bFromGeom( bShadingTangentFromGeometry_ ),
		bHasTangent( bHasShadingTangent_ ),
		tangent( objShadingTangent ),
		tEnter( tEnter_ ),
		tExit( tExit_ )
		{
		}

		void IntersectRay( RayIntersectionGeometric& ri, const bool, const bool, const bool bComputeExitInfo ) const override
		{
			ri.bHit = true;
			ri.range = tEnter;
			ri.range2 = bComputeExitInfo ? tExit : tEnter;
			ri.vNormal = n;
			ri.vGeomNormal = n;
			ri.vNormal2 = n;
			ri.vGeomNormal2 = n;
			ri.ptCoord = Point2( 0, 0 );
			ri.bShadingTangentFromGeometry = bFromGeom;
			if( bHasTangent ) {
				ri.vShadingTangent = tangent;
				ri.bHasShadingTangent = true;
			}
		}

		bool IntersectRay_IntersectionOnly( const Ray&, const Scalar, const bool, const bool ) const override
		{
			return true;
		}

		void GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const override
		{
			ptCenter = Point3( 0, 0, 0 );
			radius = 1e6;
		}

		BoundingBox GenerateBoundingBox() const override
		{
			return BoundingBox();	// default ctor = infinite box
		}

		bool DoPreHitTest() const override { return false; }
		bool CanBeAreaLight() const override { return false; }
		bool CanTessellate() const override { return false; }

		void UniformRandomPoint( Point3*, Vector3*, Point2*, const Point3& ) const override {}
		Scalar GetArea() const override { return 0; }

		IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) override { return 0; }
		void SetIntermediateValue( const IKeyframeParameter& ) override {}
		void RegenerateData() override {}

	protected:
		virtual ~StubTangentGeometry() {}

	private:
		Vector3	n;
		bool	bFromGeom;
		bool	bHasTangent;
		Vector3	tangent;
		Scalar	tEnter;
		Scalar	tExit;

		StubTangentGeometry( const StubTangentGeometry& );
		StubTangentGeometry& operator=( const StubTangentGeometry& );
	};

	//! Builds a single straight 2-control-point strand from `root` to
	//! `tip`.  Per HairGeometry.h's documented Catmull-Rom reflected-
	//! phantom-endpoint convention, a 2-CP strand is EXACTLY a straight
	//! line, so its object-space tangent is the CONSTANT
	//! normalize(tip - root) at every point along it -- no curve
	//! evaluation needed to know the ground truth.
	HairGeometry* MakeStraightStrandGeometry( const Point3& root, const Point3& tip, Scalar width )
	{
		HairGeometry::StrandDesc d;
		d.controlPoints.push_back( root );
		d.controlPoints.push_back( tip );
		d.rootWidth = width;
		d.tipWidth = width;
		d.rootUV = Point2( 0.25, 0.75 );
		std::vector<HairGeometry::StrandDesc> descs;
		descs.push_back( d );
		return new HairGeometry( descs );
	}
}

// ============================================================
// Test 1: HairGeometry through Object under rotation + translation
//         + non-uniform scale -- onb.u() must be the world-
//         transformed OBJECT-SPACE tangent (vTangent's own idiom),
//         not the legacy world-X projection.
// ============================================================
static void TestObjectHairThroughTransform()
{
	std::cout << "Object: hair fiber tangent through rotation+translation+stretch..." << std::endl;

	const Point3 root( 0, 0, -0.05 );
	const Point3 tip( 0, 0, 0.05 );
	const Vector3 objTangent = Vector3Ops::Normalize( Vector3Ops::mkVector3( tip, root ) );	// (0,0,1)

	HairGeometry* g = MakeStraightStrandGeometry( root, tip, 0.02 );
	Object* o = new Object( g );
	safe_release( g );

	o->SetOrientation( Vector3( 0.3, 1.1, -0.7 ) );
	o->TranslateObject( Vector3( 5, -2, 3 ) );
	o->SetStretch( Vector3( 1.7, 0.6, 2.4 ) );		// non-uniform scale
	o->FinalizeTransformations();

	const Matrix4 M = o->GetFinalTransformMatrix();

	// Object-space ray that crosses the strand's centerline (h = 0)
	// exactly at its arc-length midpoint (u = 0.5, object-space point
	// (0,0,0)): origin (0.5,0,0), direction -X.  Forward-transform BOTH
	// through the object's own final matrix to get the equivalent
	// WORLD-space ray Object::IntersectRay expects (it inverse-
	// transforms internally, recovering this exact object-space ray).
	const Point3 objOrigin( 0.5, 0, 0 );
	const Vector3 objDir( -1, 0, 0 );
	const Point3 worldOrigin = Point3Ops::Transform( M, objOrigin );
	const Vector3 worldDir = Vector3Ops::Normalize( Vector3Ops::Transform( M, objDir ) );

	Ray r( worldOrigin, worldDir );
	RayIntersection ri( r, nullRasterizerState );
	Hit( o, r, ri );

	Check( ri.geometric.bHit, "Test1: ray hits the transformed strand" );
	Check( ri.geometric.bShadingTangentFromGeometry, "Test1: bShadingTangentFromGeometry set" );
	Check( ri.geometric.bHasShadingTangent, "Test1: bHasShadingTangent set" );

	// Ground truth: forward-transform the OBJECT-SPACE tangent through
	// the object's own matrix, exactly the vTangent idiom (Object.cpp,
	// the bHasTangent block) -- NOT the world-X projection.
	const Vector3 expectedTangent = Vector3Ops::Normalize( Vector3Ops::Transform( M, objTangent ) );

	Check( Vector3Ops::Dot( ri.geometric.onb.u(), expectedTangent ) > 0.999,
		"Test1: MONEY ASSERTION -- onb.u() aligns with the world-transformed curve tangent" );

	// Sanity: this really discriminates against the legacy world-X
	// projection (would only coincide with expectedTangent by fluke).
	const Vector3 n = ri.geometric.vNormal;
	Vector3 legacyT( 1.0 - n.x*n.x, -n.x*n.y, -n.x*n.z );
	if( Vector3Ops::SquaredModulus( legacyT ) < 1e-12 ) {
		legacyT = Vector3( -n.y*n.x, 1.0 - n.y*n.y, -n.y*n.z );
	}
	legacyT = Vector3Ops::Normalize( legacyT );
	Check( std::fabs( Vector3Ops::Dot( expectedTangent, legacyT ) ) < 0.999,
		"Test1: (sanity) world-transformed curve tangent is NOT the legacy world-X projection here" );

	Check( VecClose( ri.geometric.onb.w(), ri.geometric.vNormal, 1e-9 ),
		"Test1: onb.w() equals the reported world shading normal" );

	CheckOrthonormalRightHanded( ri.geometric.onb, "Test1" );

	o->release();
}

// ============================================================
// Test 2: identity transform -- onb.u() must match the OBJECT-space
//         tangent directly (world space == object space here).
// ============================================================
static void TestObjectHairIdentity()
{
	std::cout << "Object: hair fiber tangent under identity transform..." << std::endl;

	const Point3 root( 0, 0, -0.05 );
	const Point3 tip( 0, 0, 0.05 );
	const Vector3 objTangent = Vector3Ops::Normalize( Vector3Ops::mkVector3( tip, root ) );	// (0,0,1)

	HairGeometry* g = MakeStraightStrandGeometry( root, tip, 0.02 );
	Object* o = new Object( g );
	safe_release( g );
	o->FinalizeTransformations();

	Ray r( Point3( 0.5, 0, 0 ), Vector3( -1, 0, 0 ) );
	RayIntersection ri( r, nullRasterizerState );
	Hit( o, r, ri );

	Check( ri.geometric.bHit, "Test2: ray hits the strand" );
	Check( ri.geometric.bHasShadingTangent, "Test2: bHasShadingTangent set" );
	Check( Vector3Ops::Dot( ri.geometric.onb.u(), objTangent ) > 0.999,
		"Test2: identity transform -- onb.u() matches the object-space tangent directly" );
	CheckOrthonormalRightHanded( ri.geometric.onb, "Test2" );

	o->release();
}

// ============================================================
// Test 3: legacy guard.  bShadingTangentFromGeometry WITHOUT
//         bHasShadingTangent (SDFGeometry heightfield mode, the
//         pre-existing consumer) still gets the untouched legacy
//         world-X projection -- reused verbatim from
//         CsgSurfacePayloadTest's Test 10 (P2 item 2) control case.
// ============================================================
static void TestObjectLegacyGuardNoSuppliedTangent()
{
	std::cout << "Object: legacy world-X path preserved when bHasShadingTangent is unset..." << std::endl;

	ConstantFunction2D* field = new ConstantFunction2D( 0.0 );
	SDFGeometry* g = new SDFGeometry( field, 2.0, 0.0, 256, 0.0, 8 );
	safe_release( field );

	Object* o = new Object( g );
	safe_release( g );
	o->FinalizeTransformations();

	Ray r( Point3( 0.3, 0.2, 5.0 ), Vector3( 0, 0, -1 ) );
	RayIntersection ri( r, nullRasterizerState );
	Hit( o, r, ri );

	Check( ri.geometric.bHit, "Test3: ray hits the flat heightfield SDF" );
	Check( ri.geometric.bShadingTangentFromGeometry, "Test3: bShadingTangentFromGeometry set (SDFGeometry heightfield)" );
	Check( !ri.geometric.bHasShadingTangent, "Test3: bHasShadingTangent NOT set (legacy consumer never sets it)" );

	// Known closed-form legacy result for an exact +Z normal: world-X
	// projected into the normal plane is world-X itself.
	Check( VecClose( ri.geometric.onb.u(), Vector3( 1, 0, 0 ) ),
		"Test3: onb.u() is the legacy world-X projection, byte-identical to before this slice" );

	o->release();
}

// ============================================================
// Test 4: degenerate supplied tangent (parallel to the normal) --
//         falls back cleanly to the legacy world-X projection, no
//         NaN anywhere in the ONB.  Uses the stub because a real
//         HairGeometry hit can never produce this (see file header).
// ============================================================
static void TestObjectDegenerateSuppliedTangentFallsBack()
{
	std::cout << "Object: degenerate supplied tangent (parallel to normal) falls back cleanly..." << std::endl;

	const Vector3 n( 0, 0, 1 );
	StubTangentGeometry* g = new StubTangentGeometry(
		n, /*bShadingTangentFromGeometry=*/true, /*bHasShadingTangent=*/true,
		/*objShadingTangent=*/Vector3( 0, 0, 5 ),	// parallel to n (non-unit, to also prove the degeneracy check isn't fooled by magnitude)
		1.0, 1.0 );
	Object* o = new Object( g );
	safe_release( g );
	o->FinalizeTransformations();	// identity: world normal/tangent equal object-space values

	Ray r( Point3( 0, 0, -1 ), Vector3( 0, 0, 1 ) );
	RayIntersection ri( r, nullRasterizerState );
	Hit( o, r, ri );

	Check( ri.geometric.bHit, "Test4: ray hits the stub" );
	Check( ri.geometric.bHasShadingTangent, "Test4: bHasShadingTangent set (stub supplied one)" );

	Check( VecFinite( ri.geometric.onb.u() ), "Test4: onb.u() is finite (no NaN)" );
	Check( VecFinite( ri.geometric.onb.v() ), "Test4: onb.v() is finite (no NaN)" );
	Check( VecFinite( ri.geometric.onb.w() ), "Test4: onb.w() is finite (no NaN)" );

	// Falls back to the SAME legacy world-X projection Test 3 pins:
	// n = (0,0,1) here too, so onb.u() should land on world-X.
	Check( VecClose( ri.geometric.onb.u(), Vector3( 1, 0, 0 ) ),
		"Test4: degenerate supplied tangent falls back to the legacy world-X projection" );

	CheckOrthonormalRightHanded( ri.geometric.onb, "Test4" );

	o->release();
}

// ============================================================
// Test 5: CSGObject's byte-duplicate ONB block, exercised via the
//         simple pass-through path (never-hit second operand,
//         mirrors CsgSurfacePayloadTest's Test 10 pattern) PLUS the
//         vShadingTangent write-back fix -- operand A's own
//         Object::IntersectRay must leave vShadingTangent promoted
//         to ITS parent frame (identity here) so the CSG's own tail
//         block, under a REAL rotation+translation+stretch, finishes
//         the promotion to true world space.
// ============================================================
static void TestCsgPassThroughHonoursSuppliedTangent()
{
	std::cout << "CSGObject: pass-through tail block honours a supplied fiber tangent..." << std::endl;

	const Vector3 objNormal( 0, 0, 1 );
	const Vector3 objTangent( 1, 0, 0 );	// perpendicular to objNormal

	StubTangentGeometry* gA = new StubTangentGeometry(
		objNormal, /*bShadingTangentFromGeometry=*/true, /*bHasShadingTangent=*/true,
		objTangent, /*tEnter=*/1.0, /*tExit=*/1000.0 );
	Object* opA = new Object( gA );
	safe_release( gA );
	opA->FinalizeTransformations();		// identity -- operand's own promotion is a no-op

	// Far-away, never-hit second operand (same trick as
	// CsgSurfacePayloadTest's Test 8/9/10).
	SphereGeometry* gB = new SphereGeometry( 1.0 );
	Object* opB = new Object( gB );
	safe_release( gB );
	opB->SetPosition( Point3( 100000, 100000, 100000 ) );
	opB->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_UNION );
	const bool assigned = csg->AssignObjects( opA, opB );
	Check( assigned, "Test5: composite takes A(stub)/B(far sphere) operands" );

	csg->SetOrientation( Vector3( -0.4, 0.2, 0.9 ) );
	csg->TranslateObject( Vector3( -3, 8, 1 ) );
	csg->SetStretch( Vector3( 2.0, 0.5, 1.3 ) );
	csg->FinalizeTransformations();

	const Matrix4 M = csg->GetFinalTransformMatrix();

	// Ray in the CSG's local frame is (origin (0,0,-1), dir +Z) to hit
	// A's synthetic plane at local t=1 (object space (0,0,0)); forward-
	// transform through the CSG's own matrix to get the equivalent
	// world ray (CSGObject::IntersectRay inverse-transforms internally,
	// same convention as Object::IntersectRay).
	const Point3 localOrigin( 0, 0, -1 );
	const Vector3 localDir( 0, 0, 1 );
	const Point3 worldOrigin = Point3Ops::Transform( M, localOrigin );
	const Vector3 worldDir = Vector3Ops::Normalize( Vector3Ops::Transform( M, localDir ) );

	Ray r( worldOrigin, worldDir );
	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );

	Check( ri.geometric.bHit, "Test5: ray hits the composite (A's synthetic plane)" );
	Check( ri.geometric.bShadingTangentFromGeometry, "Test5: bShadingTangentFromGeometry set" );
	Check( ri.geometric.bHasShadingTangent, "Test5: bHasShadingTangent set" );

	const Vector3 expectedTangent = Vector3Ops::Normalize( Vector3Ops::Transform( M, objTangent ) );
	Check( Vector3Ops::Dot( ri.geometric.onb.u(), expectedTangent ) > 0.999,
		"Test5: MONEY ASSERTION -- CSG composite onb.u() aligns with the CSG-transformed fiber tangent" );
	Check( VecClose( ri.geometric.onb.w(), ri.geometric.vNormal, 1e-9 ),
		"Test5: onb.w() equals the reported world shading normal" );

	CheckOrthonormalRightHanded( ri.geometric.onb, "Test5" );

	safe_release( csg );
	safe_release( opA );
	safe_release( opB );
}

// ============================================================
// Test 6: AdoptCsgSurfacePayload regression -- CSG_INTERSECTION,
//         "outside both, A enters first, B enters while inside A"
//         (mirrors CsgSurfacePayloadTest's Test 1 nested-box
//         scenario).  `ri` starts as A's whole-record copy
//         (bHasShadingTangent == false, A never sets it), then
//         AdoptCsgSurfacePayload re-adopts B's geometric payload
//         because the reported entry boundary is wholly B's.  Before
//         the fix, AdoptCsgSurfacePayload copied
//         bShadingTangentFromGeometry but not vShadingTangent /
//         bHasShadingTangent, so the composite would report
//         bShadingTangentFromGeometry == true (B's) paired with
//         bHasShadingTangent == false (A's stale value) and a stale
//         vShadingTangent -- exactly the "mixed-surface" corruption
//         AdoptCsgSurfacePayload exists to prevent for every other
//         per-surface field.
// ============================================================
static void TestCsgAdoptSurfacePayloadCopiesSuppliedTangent()
{
	std::cout << "CSGObject: AdoptCsgSurfacePayload copies the supplied fiber tangent..." << std::endl;

	const Vector3 objNormal( 0, 0, 1 );
	const Vector3 objTangentB( 1, 0, 0 );	// perpendicular to objNormal

	// A: no supplied tangent at all (matches a plain box/sphere
	// operand) -- wide interval [1, 1000].
	StubTangentGeometry* gA = new StubTangentGeometry(
		objNormal, /*bShadingTangentFromGeometry=*/false, /*bHasShadingTangent=*/false,
		Vector3( 0, 0, 0 ), /*tEnter=*/1.0, /*tExit=*/1000.0 );
	Object* opA = new Object( gA );
	safe_release( gA );
	opA->FinalizeTransformations();

	// B: real supplied tangent -- narrow interval [40, 60], fully
	// nested inside A's [1, 1000] (triggers the "A enters first, B
	// enters while inside A" branch: A.range <= B.range <= A.range2).
	StubTangentGeometry* gB = new StubTangentGeometry(
		objNormal, /*bShadingTangentFromGeometry=*/true, /*bHasShadingTangent=*/true,
		objTangentB, /*tEnter=*/40.0, /*tExit=*/60.0 );
	Object* opB = new Object( gB );
	safe_release( gB );
	opB->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_INTERSECTION );
	const bool assigned = csg->AssignObjects( opA, opB );
	Check( assigned, "Test6: composite takes A(no tangent)/B(tangent) operands" );
	csg->FinalizeTransformations();		// identity CSG transform -- isolates the AdoptCsgSurfacePayload fix

	Ray r( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );	// entry/exit ranges are stub-fixed; ray direction just needs to be a unit vector
	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );

	Check( ri.geometric.bHit, "Test6: ray hits the composite" );
	Check( Close( ri.geometric.range, 40.0, 1e-6 ), "Test6: (control) composite entry is B's range (40)" );
	Check( ri.geometric.bShadingTangentFromGeometry, "Test6: bShadingTangentFromGeometry adopted from B" );
	Check( ri.geometric.bHasShadingTangent,
		"Test6: MONEY ASSERTION -- bHasShadingTangent adopted from B (previously stayed A's stale FALSE)" );
	Check( VecClose( ri.geometric.vShadingTangent, objTangentB ),
		"Test6: MONEY ASSERTION -- vShadingTangent adopted from B, not stale/absent" );

	// And the composite's own ONB actually used it (identity CSG
	// transform, so world == object space here).
	Check( Vector3Ops::Dot( ri.geometric.onb.u(), objTangentB ) > 0.999,
		"Test6: composite onb.u() reflects B's adopted tangent" );

	safe_release( csg );
	safe_release( opA );
	safe_release( opB );
}

// ============================================================
//  main
// ============================================================
int main()
{
	std::cout << "===== HairTangentPlumbingTest (Slice C2) =====" << std::endl << std::endl;

	TestObjectHairThroughTransform();
	TestObjectHairIdentity();
	TestObjectLegacyGuardNoSuppliedTangent();
	TestObjectDegenerateSuppliedTangentFallsBack();
	TestCsgPassThroughHonoursSuppliedTangent();
	TestCsgAdoptSurfacePayloadCopiesSuppliedTangent();

	std::cout << std::endl << g_pass << " passed, " << g_fail << " failed." << std::endl;
	return g_fail == 0 ? 0 : 1;
}
