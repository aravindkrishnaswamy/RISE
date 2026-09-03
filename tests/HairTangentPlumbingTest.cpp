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
//       operand A UNDER ITS OWN REAL (non-identity) rotation+stretch,
//       a never-hit operand B, and a rotation + translation + stretch
//       on the CSG object itself.  Exercises CSGObject::IntersectRay's
//       byte-duplicate ONB block AND the vShadingTangent write-back
//       fix (the field must already carry operand A's own Object-
//       level promotion by the time the CSG's own tail block reads
//       and further transforms it -- exactly the vTangent convention
//       CSGObject.cpp documents).  Operand A's transform is
//       deliberately non-identity (C2 round-1 review, P1): with an
//       identity operand transform, A's own write-back is a no-op and
//       this test cannot distinguish a correct write-back from a
//       deleted one.
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
//    7. TestCsgOfCsgComposesThreeLevelPromotion -- an inner CSGObject
//       (its own real, non-identity transform, wrapping the tangent-
//       bearing stub under YET ANOTHER real transform) as operand A of
//       an outer CSGObject with a third, different real transform.
//       (C2 round-1 review, P1(b)): the scenario CSGObject.cpp's own
//       write-back comment names by name ("a CSG nested one level
//       deeper needs THIS level's promotion applied to the field
//       itself") and that Test 5/6 alone cannot reach -- the middle
//       (inner CSG) level's write-back must fire for the outer CSG's
//       promotion to be correct.
//    8. TestObjectSingularTransformClearsSuppliedTangent -- an operand
//       stretch of (0,1,1) collapses the tangent's axis to exactly the
//       zero vector (C2 round-1 review, P3(5)).  Object::IntersectRay
//       must recognise the PROMOTED tangent itself (not just its
//       projection into the normal plane) as degenerate, clear
//       bHasShadingTangent, and fall back cleanly -- a "valid" flag
//       paired with a zero vector would otherwise be silently written
//       back and trap a further-nested CSG parent.
//
//  Given a HairGeometry hit, cases 1/2 do not need the degenerate
//  fallback (see case 4's derivation of why it is geometrically
//  unreachable there) -- the stub exists specifically to reach the
//  code paths a real curve hit cannot.
//
//  Residual wave 2 item A (docs/HAIR_FUR_DESIGN.md section 4.1,
//  2026-08-27) closed the one remaining gap this slice's own header
//  used to flag as open: `NormalMap::Modify` and `BumpMap::Modify`
//  used to rebuild the shading ONB with the unconditional
//  `CreateFromW(perturbed normal)` after perturbing it, silently
//  discarding whatever fiber tangent Object::IntersectRay had just
//  promoted into `ri.onb.u()`.  Cases 9-12 guard the fix:
//
//    9. TestNormalMapPreservesHairFiberTangent -- a real HairGeometry
//       hit (bHasShadingTangent == true) run through `NormalMap` with
//       a non-trivial tangent-space tilt.  Expected result computed
//       INDEPENDENTLY (same formula, written fresh in the test, not by
//       calling the modifier's own helpers): the perturbed normal from
//       the pre-modify {T=onb.u(), B=onb.v(), N=vNormal} frame, then
//       onb.u() projected into that new normal's plane and normalized
//       -- the CreateFromWU idiom GlintModifier already used.
//   10. TestNormalMapNonHairByteMatchesLegacy -- the identical tilt
//       through the identical modifier on a BoxGeometry hit
//       (bHasShadingTangent == false; docs/CLOTH_FABRIC_DESIGN.md 9.1
//       update -- SphereGeometry is no longer a valid control here, see
//       that test's own comment).  Golden value is a fresh,
//       independent `OrthonormalBasis3D::CreateFromW` call on the same
//       perturbed normal -- the fix's guard must be a true no-op here.
//   11. TestBumpMapPreservesHairFiberTangent -- same idea against
//       `BumpMap`, using a synthetic linear-gradient IFunction2D stub
//       (LinearGradientFunction2D below) so the finite-difference bump
//       is an exact, hand-computable constant regardless of `ptCoord`.
//   12. TestBumpMapNonHairByteMatchesLegacy -- BumpMap's non-hair
//       byte-match twin of case 10.
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
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/Geometry.h"
#include "../src/Library/Geometry/HairGeometry.h"
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Modifiers/BumpMap.h"
#include "../src/Library/Modifiers/NormalMap.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Painters/UniformColorPainter.h"

using namespace RISE;
using namespace RISE::Implementation;

// ============================================================
//  Harness
// ============================================================

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

namespace
{
	// No default tolerance: every call site below states its own eps
	// explicitly (P3 review -- money assertions want a tight 1e-9,
	// distinct from the looser 1e-6 a couple of sanity checks still
	// use), so a shared default would go unused and warn.
	bool Close( Scalar a, Scalar b, Scalar eps )
	{
		return std::fabs( a - b ) < eps;
	}

	bool VecClose( const Vector3& a, const Vector3& b, Scalar eps )
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

	//! Minimal IFunction2D stub for the BumpMap tests (cases 11/12):
	//! f(x,y) = a*x + b*y.  A linear gradient makes the central-
	//! difference bump BumpMap::Modify computes a HAND-COMPUTABLE
	//! constant (2*a*dWindow, 2*b*dWindow) independent of WHERE
	//! ptCoord happens to land -- unlike ConstantFunction2D (already
	//! included above), whose zero gradient would produce no
	//! perturbation at all and defeat the point of these tests.
	class LinearGradientFunction2D : public virtual IFunction2D, public virtual Reference
	{
	public:
		LinearGradientFunction2D( Scalar a_, Scalar b_ ) : a( a_ ), b( b_ ) {}
		Scalar Evaluate( const Scalar x, const Scalar y ) const override { return a * x + b * y; }
	protected:
		virtual ~LinearGradientFunction2D() {}
	private:
		Scalar a, b;
		LinearGradientFunction2D( const LinearGradientFunction2D& );
		LinearGradientFunction2D& operator=( const LinearGradientFunction2D& );
	};
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

	Check( VecClose( ri.geometric.onb.u(), expectedTangent, 1e-9 ),
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
	Check( VecClose( ri.geometric.onb.u(), objTangent, 1e-9 ),
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
	Check( VecClose( ri.geometric.onb.u(), Vector3( 1, 0, 0 ), 1e-9 ),
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
	Check( VecClose( ri.geometric.onb.u(), Vector3( 1, 0, 0 ), 1e-9 ),
		"Test4: degenerate supplied tangent falls back to the legacy world-X projection" );

	CheckOrthonormalRightHanded( ri.geometric.onb, "Test4" );

	o->release();
}

// ============================================================
// Test 5: CSGObject's byte-duplicate ONB block, exercised via the
//         simple pass-through path (never-hit second operand,
//         mirrors CsgSurfacePayloadTest's Test 10 pattern) PLUS the
//         vShadingTangent write-back fix -- operand A itself carries
//         a REAL (non-identity) rotation+stretch, so operand A's own
//         Object::IntersectRay must ACTUALLY promote and write back
//         vShadingTangent into ITS parent (the CSG's local) frame for
//         the CSG's own tail block to then finish the promotion to
//         true world space.  With an identity operand transform this
//         write-back is a no-op and the test cannot tell a correct
//         write-back from a deleted one (P1, C2 round-1 review) --
//         the non-identity operand transform is what makes the
//         write-back load-bearing for this test.
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
	// Real, non-identity operand transform (P1 fix): orthogonality
	// between the stub's object-space normal and tangent survives ANY
	// invertible linear map (same proof as the file header's Test 4
	// derivation, applied one level earlier), so this introduces no
	// degeneracy -- it only makes operand A's own promotion, and its
	// write-back into vShadingTangent, actually do something.
	opA->SetOrientation( Vector3( 0.5, -0.3, 0.8 ) );
	opA->SetStretch( Vector3( 3.0, 1.0, 0.5 ) );
	opA->FinalizeTransformations();
	const Matrix4 Mopa = opA->GetFinalTransformMatrix();

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

	// Ray in the CSG's local frame is (origin (0,0,-1), dir +Z).  The
	// stub geometry (see its class comment) reports a hit at a fixed
	// local ray-parameter range regardless of the actual ray, so this
	// still lands on A's synthetic plane through operand A's now-real
	// transform; forward-transform through the CSG's own matrix to get
	// the equivalent world ray (CSGObject::IntersectRay inverse-
	// transforms internally, same convention as Object::IntersectRay).
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

	// Ground truth composed INDEPENDENTLY from the two known matrices,
	// one level at a time -- operand A's own promotion, then the CSG's.
	// Normalize commutes with a linear map up to a positive scalar (the
	// normalizing divisor), so composing with an intermediate
	// normalize (as the production code does at each level) or without
	// one (as here) gives the identical final DIRECTION -- one final
	// normalize is sufficient ground truth.
	const Vector3 expectedTangent = Vector3Ops::Normalize(
		Vector3Ops::Transform( M, Vector3Ops::Transform( Mopa, objTangent ) ) );
	Check( VecClose( ri.geometric.onb.u(), expectedTangent, 1e-9 ),
		"Test5: MONEY ASSERTION -- CSG composite onb.u() aligns with the doubly-promoted (A then CSG) fiber tangent" );
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
	Check( VecClose( ri.geometric.vShadingTangent, objTangentB, 1e-9 ),
		"Test6: MONEY ASSERTION -- vShadingTangent adopted from B, not stale/absent" );

	// And the composite's own ONB actually used it (identity CSG
	// transform, so world == object space here).
	Check( VecClose( ri.geometric.onb.u(), objTangentB, 1e-9 ),
		"Test6: composite onb.u() reflects B's adopted tangent" );

	safe_release( csg );
	safe_release( opA );
	safe_release( opB );
}

// ============================================================
// Test 7: CSG-of-CSG (P1, C2 round-1 review) -- an inner CSGObject
//         with a REAL non-identity transform is operand A of an
//         outer CSGObject with a DIFFERENT non-identity transform.
//         This is the scenario CSGObject.cpp's own write-back
//         comment names explicitly ("a CSG nested one level deeper
//         needs THIS level's promotion applied to the field itself")
//         and that Test 5/6 cannot reach (both use an identity CSG-
//         or-operand transform on the level whose write-back would
//         otherwise be dead).  Three promotions must compose:
//         innermost Object -> inner CSG -> outer CSG.
// ============================================================
static void TestCsgOfCsgComposesThreeLevelPromotion()
{
	std::cout << "CSGObject: CSG-of-CSG composes a three-level fiber tangent promotion..." << std::endl;

	const Vector3 objNormal( 0, 0, 1 );
	const Vector3 objTangent( 1, 0, 0 );	// perpendicular to objNormal

	// Level 0: the tangent-bearing stub, wrapped in an Object with its
	// own real transform.
	StubTangentGeometry* gLeaf = new StubTangentGeometry(
		objNormal, /*bShadingTangentFromGeometry=*/true, /*bHasShadingTangent=*/true,
		objTangent, /*tEnter=*/1.0, /*tExit=*/1000.0 );
	Object* opLeaf = new Object( gLeaf );
	safe_release( gLeaf );
	opLeaf->SetOrientation( Vector3( 0.6, 0.1, -0.4 ) );
	opLeaf->SetStretch( Vector3( 1.4, 2.2, 0.7 ) );
	opLeaf->FinalizeTransformations();
	const Matrix4 Mleaf = opLeaf->GetFinalTransformMatrix();

	// Never-hit far operand for the INNER CSG.
	SphereGeometry* gInnerB = new SphereGeometry( 1.0 );
	Object* opInnerB = new Object( gInnerB );
	safe_release( gInnerB );
	opInnerB->SetPosition( Point3( 100000, 100000, 100000 ) );
	opInnerB->FinalizeTransformations();

	// Level 1: inner CSG, its own real (different) transform.
	CSGObject* innerCsg = new CSGObject( CSG_UNION );
	Check( innerCsg->AssignObjects( opLeaf, opInnerB ), "Test7: inner composite takes A(stub)/B(far sphere) operands" );
	innerCsg->SetOrientation( Vector3( -0.2, 0.9, 0.3 ) );
	innerCsg->SetStretch( Vector3( 0.8, 1.6, 2.5 ) );
	innerCsg->FinalizeTransformations();
	const Matrix4 Minner = innerCsg->GetFinalTransformMatrix();

	// Never-hit far operand for the OUTER CSG.
	SphereGeometry* gOuterB = new SphereGeometry( 1.0 );
	Object* opOuterB = new Object( gOuterB );
	safe_release( gOuterB );
	opOuterB->SetPosition( Point3( -100000, -100000, -100000 ) );
	opOuterB->FinalizeTransformations();

	// Level 2: outer CSG, wrapping the inner CSG as operand A, with yet
	// another real (different again) transform.
	CSGObject* outerCsg = new CSGObject( CSG_UNION );
	Check( outerCsg->AssignObjects( innerCsg, opOuterB ), "Test7: outer composite takes A(inner CSG)/B(far sphere) operands" );
	outerCsg->SetOrientation( Vector3( 1.1, -0.5, 0.2 ) );
	outerCsg->TranslateObject( Vector3( 4, -6, 2 ) );
	outerCsg->SetStretch( Vector3( 2.1, 0.9, 1.3 ) );
	outerCsg->FinalizeTransformations();
	const Matrix4 Mouter = outerCsg->GetFinalTransformMatrix();

	// Ray in the OUTER CSG's local frame; the stub reports a hit at a
	// fixed local ray-parameter range regardless of the actual ray (see
	// StubTangentGeometry's class comment), so this reaches the leaf
	// through both nested transforms.  Forward-transform through the
	// outer CSG's own matrix to get the equivalent world ray.
	const Point3 localOrigin( 0, 0, -1 );
	const Vector3 localDir( 0, 0, 1 );
	const Point3 worldOrigin = Point3Ops::Transform( Mouter, localOrigin );
	const Vector3 worldDir = Vector3Ops::Normalize( Vector3Ops::Transform( Mouter, localDir ) );

	Ray r( worldOrigin, worldDir );
	RayIntersection ri( r, nullRasterizerState );
	Hit( outerCsg, r, ri );

	Check( ri.geometric.bHit, "Test7: ray hits the CSG-of-CSG composite" );
	Check( ri.geometric.bShadingTangentFromGeometry, "Test7: bShadingTangentFromGeometry set" );
	Check( ri.geometric.bHasShadingTangent, "Test7: bHasShadingTangent set" );

	// Ground truth: three promotions composed INDEPENDENTLY from the
	// three known matrices, one final normalize (see Test 5's comment
	// for why an intermediate normalize per level doesn't change the
	// final direction).
	const Vector3 expectedTangent = Vector3Ops::Normalize(
		Vector3Ops::Transform( Mouter, Vector3Ops::Transform( Minner, Vector3Ops::Transform( Mleaf, objTangent ) ) ) );
	Check( VecClose( ri.geometric.onb.u(), expectedTangent, 1e-9 ),
		"Test7: MONEY ASSERTION -- onb.u() aligns with the three-level (leaf, inner CSG, outer CSG) composed tangent" );
	Check( VecClose( ri.geometric.onb.w(), ri.geometric.vNormal, 1e-9 ),
		"Test7: onb.w() equals the reported world shading normal" );

	CheckOrthonormalRightHanded( ri.geometric.onb, "Test7" );

	safe_release( outerCsg );
	safe_release( innerCsg );
	safe_release( opLeaf );
	safe_release( opInnerB );
	safe_release( opOuterB );
}

// ============================================================
// Test 8: singular operand transform (P3.5) -- a stretch that
//         collapses one axis to zero (0,1,1) zeroes out the promoted
//         tangent entirely (objTangent is along the collapsed X axis)
//         wherever the tangent isn't perpendicular to the collapsed
//         axis.  Object::IntersectRay must recognise the promoted
//         tangent itself (not just its projection) as degenerate,
//         clear bHasShadingTangent, and fall back to a clean
//         (non-NaN) legacy ONB -- a "valid" flag paired with a zero
//         vector would otherwise be a trap for CSGObject's own
//         nesting promotion.
// ============================================================
static void TestObjectSingularTransformClearsSuppliedTangent()
{
	std::cout << "Object: singular operand transform clears bHasShadingTangent, no NaN..." << std::endl;

	const Vector3 objNormal( 0, 0, 1 );
	const Vector3 objTangent( 1, 0, 0 );	// perpendicular to objNormal; lies EXACTLY along the axis about to be collapsed

	StubTangentGeometry* g = new StubTangentGeometry(
		objNormal, /*bShadingTangentFromGeometry=*/true, /*bHasShadingTangent=*/true,
		objTangent, 1.0, 1.0 );
	Object* o = new Object( g );
	safe_release( g );
	o->SetStretch( Vector3( 0, 1, 1 ) );	// collapses the X axis -- objTangent maps to the zero vector
	o->FinalizeTransformations();

	Ray r( Point3( 0, 0, -1 ), Vector3( 0, 0, 1 ) );
	RayIntersection ri( r, nullRasterizerState );
	Hit( o, r, ri );

	Check( ri.geometric.bHit, "Test8: ray hits the stub" );
	Check( !ri.geometric.bHasShadingTangent,
		"Test8: MONEY ASSERTION -- bHasShadingTangent cleared after a singular-transform collapse" );

	Check( VecFinite( ri.geometric.onb.u() ), "Test8: onb.u() is finite (no NaN)" );
	Check( VecFinite( ri.geometric.onb.v() ), "Test8: onb.v() is finite (no NaN)" );
	Check( VecFinite( ri.geometric.onb.w() ), "Test8: onb.w() is finite (no NaN)" );

	// The collapsed transform still maps the +Z object normal to +Z
	// world normal (unaffected axis), so this lands on the SAME known
	// legacy world-X fallback Tests 3/4 pin.
	Check( VecClose( ri.geometric.onb.u(), Vector3( 1, 0, 0 ), 1e-9 ),
		"Test8: falls back cleanly to the legacy world-X projection" );

	CheckOrthonormalRightHanded( ri.geometric.onb, "Test8" );

	o->release();
}

// ============================================================
// Test 9: NormalMap through a real hair hit -- residual wave 2 item A.
//         onb.u() must stay fiber-aligned (projected into the
//         perturbed normal's plane), not replaced by an arbitrary
//         canonical-axis tangent.  Expected value computed
//         INDEPENDENTLY of the modifier's own CreateFromWU call.
// ============================================================
static void TestNormalMapPreservesHairFiberTangent()
{
	std::cout << "NormalMap: preserves the fiber tangent on a hair hit..." << std::endl;

	const Point3 root( 0, 0, -0.05 );
	const Point3 tip( 0, 0, 0.05 );
	HairGeometry* g = MakeStraightStrandGeometry( root, tip, 0.02 );
	Object* o = new Object( g );
	safe_release( g );
	o->FinalizeTransformations();

	Ray r( Point3( 0.5, 0, 0 ), Vector3( -1, 0, 0 ) );
	RayIntersection ri( r, nullRasterizerState );
	Hit( o, r, ri );

	Check( ri.geometric.bHit, "Test9: ray hits the strand" );
	Check( ri.geometric.bHasShadingTangent, "Test9: bHasShadingTangent set (precondition)" );

	// Snapshot the pre-modify frame: HairGeometry sets neither
	// bHasTangent nor derivatives.valid, so NormalMap's last-ditch
	// fallback (T=onb.u(), B=onb.v()) is exactly what fires -- these
	// ARE the values the modifier will read.
	const Vector3 oldU = ri.geometric.onb.u();
	const Vector3 oldV = ri.geometric.onb.v();
	const Vector3 oldN = ri.geometric.vNormal;

	// r=0.75,g=0.35 -> nx=0.5, ny=-0.3, dScale=1 -> nz=sqrt(1-0.25-0.09).
	UniformColorPainter* pPainter = new UniformColorPainter( RISEPel( 0.75, 0.35, 1.0 ) );
	NormalMap* pMod = new NormalMap( *pPainter, 1.0 );
	safe_release( pPainter );

	const Scalar nx = 0.5, ny = -0.3;
	const Scalar nz = std::sqrt( 1.0 - nx*nx - ny*ny );
	const Vector3 expectedNormal = Vector3Ops::Normalize( oldU * nx + oldV * ny + oldN * nz );
	const Vector3 uProj = oldU - expectedNormal * Vector3Ops::Dot( oldU, expectedNormal );
	Check( Vector3Ops::SquaredModulus( uProj ) > 1e-12, "Test9: (sanity) projection is non-degenerate" );
	const Vector3 expectedU = Vector3Ops::Normalize( uProj );

	pMod->Modify( ri.geometric );
	safe_release( pMod );

	Check( VecClose( ri.geometric.vNormal, expectedNormal, 1e-9 ),
		"Test9: (sanity) perturbed normal matches the independently-computed value" );
	Check( VecClose( ri.geometric.onb.u(), expectedU, 1e-9 ),
		"Test9: MONEY ASSERTION -- onb.u() stays fiber-aligned (projected into the perturbed normal's plane)" );
	Check( VecClose( ri.geometric.onb.w(), ri.geometric.vNormal, 1e-9 ),
		"Test9: onb.w() equals the perturbed normal" );
	CheckOrthonormalRightHanded( ri.geometric.onb, "Test9" );

	o->release();
}

// ============================================================
// Test 10: NormalMap through a non-hair hit -- byte-matches the
//          pre-fix (unconditional CreateFromW) behaviour.  Golden
//          value is a fresh, independent CreateFromW call.
// ============================================================
static void TestNormalMapNonHairByteMatchesLegacy()
{
	std::cout << "NormalMap: non-hair hit byte-matches legacy CreateFromW rebuild..." << std::endl;

	// docs/CLOTH_FABRIC_DESIGN.md 9.1 regression-sweep note: this control
	// used to be a SphereGeometry hit, on the assumption that a sphere
	// never supplies a shading tangent.  Section 9.1 gave SphereGeometry
	// (among other analytic primitives) a REAL geometry-supplied tangent
	// from its own closed-form dpdu, so a sphere hit no longer satisfies
	// this test's precondition.  BoxGeometry is not one of that fix's
	// eight write sites and still takes the legacy CreateFromW path
	// unconditionally -- it is the control this test actually needs.
	BoxGeometry* g = new BoxGeometry( 2.0, 2.0, 2.0 );
	Object* o = new Object( g );
	safe_release( g );
	o->FinalizeTransformations();

	Ray r( Point3( 0, 0, 5 ), Vector3( 0, 0, -1 ) );
	RayIntersection ri( r, nullRasterizerState );
	Hit( o, r, ri );

	Check( ri.geometric.bHit, "Test10: ray hits the box" );
	Check( !ri.geometric.bHasShadingTangent, "Test10: (precondition) box hit carries no supplied tangent" );

	const Vector3 oldU = ri.geometric.onb.u();
	const Vector3 oldV = ri.geometric.onb.v();
	const Vector3 oldN = ri.geometric.vNormal;

	UniformColorPainter* pPainter = new UniformColorPainter( RISEPel( 0.75, 0.35, 1.0 ) );
	NormalMap* pMod = new NormalMap( *pPainter, 1.0 );
	safe_release( pPainter );

	const Scalar nx = 0.5, ny = -0.3;
	const Scalar nz = std::sqrt( 1.0 - nx*nx - ny*ny );
	const Vector3 expectedNormal = Vector3Ops::Normalize( oldU * nx + oldV * ny + oldN * nz );

	pMod->Modify( ri.geometric );
	safe_release( pMod );

	// Golden: an independent CreateFromW call on the same perturbed
	// normal -- the legacy, unconditional rebuild this hit must still
	// take because bHasShadingTangent is false.
	OrthonormalBasis3D golden;
	golden.CreateFromW( expectedNormal );

	Check( VecClose( ri.geometric.onb.u(), golden.u(), 1e-9 ), "Test10: onb.u() byte-matches the legacy CreateFromW golden" );
	Check( VecClose( ri.geometric.onb.v(), golden.v(), 1e-9 ), "Test10: onb.v() byte-matches the legacy CreateFromW golden" );
	Check( VecClose( ri.geometric.onb.w(), golden.w(), 1e-9 ), "Test10: onb.w() byte-matches the legacy CreateFromW golden" );

	o->release();
}

// ============================================================
// Test 11: BumpMap through a real hair hit -- same guard as Test 9,
//          via BumpMap's finite-difference perturbation instead of
//          NormalMap's tangent-space decode.
// ============================================================
static void TestBumpMapPreservesHairFiberTangent()
{
	std::cout << "BumpMap: preserves the fiber tangent on a hair hit..." << std::endl;

	const Point3 root( 0, 0, -0.05 );
	const Point3 tip( 0, 0, 0.05 );
	HairGeometry* g = MakeStraightStrandGeometry( root, tip, 0.02 );
	Object* o = new Object( g );
	safe_release( g );
	o->FinalizeTransformations();

	Ray r( Point3( 0.5, 0, 0 ), Vector3( -1, 0, 0 ) );
	RayIntersection ri( r, nullRasterizerState );
	Hit( o, r, ri );

	Check( ri.geometric.bHit, "Test11: ray hits the strand" );
	Check( ri.geometric.bHasShadingTangent, "Test11: bHasShadingTangent set (precondition)" );

	const Vector3 oldU = ri.geometric.onb.u();
	const Vector3 oldV = ri.geometric.onb.v();
	const Vector3 oldN = ri.geometric.vNormal;

	// a=0.4, b=-0.3, dScale=1, dWindow=0.05, no gradient normalization
	// -> bumpU = 2*a*dWindow = 0.04, bumpV = 2*b*dWindow = -0.03
	// (the y-term/x-term cancel exactly in each central difference
	// because the function is linear -- see the class comment).
	LinearGradientFunction2D* pFunc = new LinearGradientFunction2D( 0.4, -0.3 );
	BumpMap* pMod = new BumpMap( *pFunc, 1.0, 0.05, false );
	safe_release( pFunc );

	const Scalar bumpU = 0.04, bumpV = -0.03;
	const Vector3 expectedNormal = Vector3Ops::Normalize( oldN + oldU * bumpU + oldV * bumpV );
	const Vector3 uProj = oldU - expectedNormal * Vector3Ops::Dot( oldU, expectedNormal );
	Check( Vector3Ops::SquaredModulus( uProj ) > 1e-12, "Test11: (sanity) projection is non-degenerate" );
	const Vector3 expectedU = Vector3Ops::Normalize( uProj );

	pMod->Modify( ri.geometric );
	safe_release( pMod );

	Check( VecClose( ri.geometric.vNormal, expectedNormal, 1e-9 ),
		"Test11: (sanity) perturbed normal matches the independently-computed value" );
	Check( VecClose( ri.geometric.onb.u(), expectedU, 1e-9 ),
		"Test11: MONEY ASSERTION -- onb.u() stays fiber-aligned (projected into the perturbed normal's plane)" );
	Check( VecClose( ri.geometric.onb.w(), ri.geometric.vNormal, 1e-9 ),
		"Test11: onb.w() equals the perturbed normal" );
	CheckOrthonormalRightHanded( ri.geometric.onb, "Test11" );

	o->release();
}

// ============================================================
// Test 12: BumpMap through a non-hair hit -- byte-matches the pre-fix
//          (unconditional CreateFromW) behaviour.
// ============================================================
static void TestBumpMapNonHairByteMatchesLegacy()
{
	std::cout << "BumpMap: non-hair hit byte-matches legacy CreateFromW rebuild..." << std::endl;

	// See TestNormalMapNonHairByteMatchesLegacy's comment: BoxGeometry,
	// not SphereGeometry, is the control docs/CLOTH_FABRIC_DESIGN.md 9.1
	// leaves on the legacy CreateFromW path.
	BoxGeometry* g = new BoxGeometry( 2.0, 2.0, 2.0 );
	Object* o = new Object( g );
	safe_release( g );
	o->FinalizeTransformations();

	Ray r( Point3( 0, 0, 5 ), Vector3( 0, 0, -1 ) );
	RayIntersection ri( r, nullRasterizerState );
	Hit( o, r, ri );

	Check( ri.geometric.bHit, "Test12: ray hits the box" );
	Check( !ri.geometric.bHasShadingTangent, "Test12: (precondition) box hit carries no supplied tangent" );

	const Vector3 oldU = ri.geometric.onb.u();
	const Vector3 oldV = ri.geometric.onb.v();
	const Vector3 oldN = ri.geometric.vNormal;

	LinearGradientFunction2D* pFunc = new LinearGradientFunction2D( 0.4, -0.3 );
	BumpMap* pMod = new BumpMap( *pFunc, 1.0, 0.05, false );
	safe_release( pFunc );

	const Scalar bumpU = 0.04, bumpV = -0.03;
	const Vector3 expectedNormal = Vector3Ops::Normalize( oldN + oldU * bumpU + oldV * bumpV );

	pMod->Modify( ri.geometric );
	safe_release( pMod );

	OrthonormalBasis3D golden;
	golden.CreateFromW( expectedNormal );

	Check( VecClose( ri.geometric.onb.u(), golden.u(), 1e-9 ), "Test12: onb.u() byte-matches the legacy CreateFromW golden" );
	Check( VecClose( ri.geometric.onb.v(), golden.v(), 1e-9 ), "Test12: onb.v() byte-matches the legacy CreateFromW golden" );
	Check( VecClose( ri.geometric.onb.w(), golden.w(), 1e-9 ), "Test12: onb.w() byte-matches the legacy CreateFromW golden" );

	o->release();
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
	TestCsgOfCsgComposesThreeLevelPromotion();
	TestObjectSingularTransformClearsSuppliedTangent();
	TestNormalMapPreservesHairFiberTangent();
	TestNormalMapNonHairByteMatchesLegacy();
	TestBumpMapPreservesHairFiberTangent();
	TestBumpMapNonHairByteMatchesLegacy();

	std::cout << std::endl << g_pass << " passed, " << g_fail << " failed." << std::endl;
	return g_fail == 0 ? 0 : 1;
}
