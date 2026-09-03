//////////////////////////////////////////////////////////////////////
//
//  GeometryShadingTangentTest.cpp
//
//    Standalone regression test for docs/CLOTH_FABRIC_DESIGN.md
//    section 9.1, the UV-aligned mesh shading tangent: at every
//    geometry site that computes a genuine UV-derived `dpdu` (or
//    carries an authored glTF TANGENT), the hit now writes
//    `ri.vShadingTangent` / `ri.bHasShadingTangent` /
//    `ri.bShadingTangentFromGeometry` so `Object::IntersectRay` (and
//    `CSGObject::IntersectRay`) build the shading ONB's u-axis from a
//    COHERENT, geometry-defined direction instead of the arbitrary
//    per-triangle-discontinuous axis `OrthonormalBasis3D::CreateFromW`
//    would otherwise pick.
//
//    Write sites covered (docs/CLOTH_FABRIC_DESIGN.md section 9.1):
//      - TriangleMeshGeometryIndexedSpecializations.h: authored
//        TANGENT wins, UV Jacobian second, else nothing (byte-
//        identical legacy path).
//      - TriangleMeshGeometrySpecializations.h (non-indexed): UV
//        Jacobian only (no TANGENT accessor on this path).
//      - SphereGeometry / EllipsoidGeometry / TorusGeometry /
//        CylinderGeometry (both its capped and open-tube sites):
//        unconditional on `sd.valid`, no discontinuous fallback.
//      - ClippedPlaneGeometry: a NEW write (it never populated
//        `ri.derivatives` at all before this fix, and still does not
//        -- only the shading tangent is written).
//
//    Test oracle for the mesh/primitive (non-TANGENT) cases: since
//    the mesh sites project their UV-Jacobian `dpdu` onto the
//    OBJECT-space shading-normal tangent plane before writing it
//    (and the analytic primitives' `sd.dpdu` is already exactly
//    perpendicular to the object-space normal by construction --
//    standard differential geometry for a genuine surface
//    parameterisation), the object-space vector fed to
//    `vShadingTangent` is IDENTICAL to the one fed to
//    `ri.derivatives.dpdu`.  Both are promoted to world space by
//    `Object::IntersectRay` via the SAME forward matrix -- `dpdu` by
//    a bare `Transform` (Object.cpp ~832), `vShadingTangent` by
//    `Normalize(Transform(...))` followed by a projection onto the
//    WORLD-space normal plane inside the ONB-build branch (Object.cpp
//    ~699-772).  The identity `t . n == 0` (object space) implies
//    `(M t) . (M^-T n) == 0` (world space) for ANY invertible linear
//    map M -- t^T M^T M^-T n = t^T n = 0 -- so that world-space
//    projection is a no-op up to floating point, and
//        onb.u() == Normalize( ri.geometric.derivatives.dpdu )
//    is an EXACT oracle (not merely a proportionality) wherever
//    `ri.geometric.derivatives.valid` is true and the TANGENT branch
//    did not fire.  `ri.geometric.derivatives.dpdu`'s own correctness
//    is independently covered by GeometryUVRoundtripTest -- this test
//    checks that `vShadingTangent` FAITHFULLY TRACKS it, which is
//    exactly what section 9.1 prescribes.
//
//    For the mesh TANGENT-priority case the analogous cross-check
//    uses `ri.geometric.vTangent` (promoted the same way, tested
//    independently by the pre-existing glTF tangent-import path) as
//    ground truth.  For the two mesh "money assertion" cases and the
//    ClippedPlaneGeometry case (which never populates
//    `ri.derivatives`) the expected direction is instead computed
//    FROM SCRATCH from the known mesh/patch geometry and the object's
//    own transform matrix -- an independent derivation, not a
//    read-back from `ri` -- following the same idiom
//    HairTangentPlumbingTest.cpp uses for HairGeometry's fibre
//    tangent.
//
//    Coverage:
//      1. TestIndexedMeshUVTangent_Money -- UV-mapped indexed quad
//         mesh (both triangles), non-identity transform: onb.u()
//         equals the independently-computed world-transformed,
//         normal-plane-projected object-space dpdu (1,0,0).
//      2. TestIndexedMeshTangentPriority_Money -- same quad, this
//         time with an authored TANGENT array pointing (0,1,0)
//         (orthogonal to and distinct from dpdu): onb.u() must track
//         the TANGENT, not dpdu, AND must equal the promoted
//         ri.geometric.vTangent exactly.
//      3. TestIndexedMeshDegenerateUV_ByteIdentical -- degenerate
//         (collapsed) UV triangle, no TANGENT: bHasShadingTangent
//         stays false and the ONB is bit-for-bit whatever a fresh
//         CreateFromW(vNormal) call produces -- the legacy path,
//         untouched.
//      4. TestNonIndexedMeshUVTangent_Money -- non-indexed mesh
//         twin of case 1 (no TANGENT accessor on this path at all).
//      5. TestNonIndexedMeshDegenerateUV_ByteIdentical -- non-indexed
//         twin of case 3.
//      6. TestMirroredTransform_Mesh / TestMirroredTransform_Sphere
//         -- a `scale -1 1 1`-style mirrored (negative-determinant)
//         transform still yields a unit, normal-orthogonal tangent
//         and the same money-assertion oracle holds (the derivation
//         above never assumed a positive determinant).
//      7. TestAnalyticPrimitiveTangent -- Sphere / Ellipsoid / Torus
//         / capped-Cylinder (side wall AND cap hits) / open-Cylinder,
//         each driven by many random rays (same aim-at-bounding-
//         sphere-centre technique as GeometryUVRoundtripTest): every
//         hit with valid derivatives gets bHasShadingTangent == true
//         and onb.u() == Normalize(derivatives.dpdu).
//      8. TestClippedPlaneTangent_Money -- a planar `clippedplane_
//         geometry` quad (a NEW write site, no `ri.derivatives`
//         populated at all): onb.u() equals the independently
//         computed world-transformed, projected object-space
//         `ptb - pta` (the planar-parallelogram closed form of
//         `GeometricUtilities::BilinearTangentU`).
//      9. TestCsgNestedMeshMatchesUnnested -- the UV-mapped mesh as
//         a CSG_UNION operand (identity operand transform, real CSG-
//         level transform) versus the identical geometry+transform
//         wrapped directly in a plain Object (no CSG): onb.u() must
//         match, i.e. CSG nesting doesn't perturb the tangent.
//
//    Fix-round additions (docs/CLOTH_FABRIC_DESIGN.md 9.9 fix round,
//    reviews T1/T2):
//      10. Tests 6/7 (mirrored mesh / sphere) STRENGTHENED: the old
//          "right-handed" check (CheckOrthonormalRightHanded) is
//          tautologically true for ANY correctly-built ONB and cannot
//          detect a chirality/sign error, so it never actually
//          exercised the mirrored case's one real risk.  Both now
//          additionally check onb.v() AND overall handedness against
//          ComputeExpectedFrame -- an INDEPENDENT oracle (not calling
//          any OrthonormalBasis3D method) built from the hit's own
//          world-space normal, an independently-recovered world-space
//          tangent, and TransformHandedness(M) (a fresh determinant
//          computation off the raw Matrix4, not the code's own
//          m_tangentFrameSign).  This is the regression guard for the
//          P1 fix: OrthonormalBasis3D::FlipV() applied inside
//          Object::IntersectRay / CSGObject::IntersectRay's
//          bShadingTangentFromGeometry branch so a mirrored instance's
//          `tangent_rotation` rotates the same sense as an unmirrored
//          one.
//      11. TestMirroredTransform_Ellipsoid / _Cylinder / _Torus -- T1's
//          own ask ("a mesh, a sphere, a cylinder and a torus") plus
//          T2 P2: only Sphere had a mirrored-transform check before;
//          Ellipsoid/Cylinder/Torus's write sites were exercised only
//          under an IDENTITY transform (object space == world space,
//          so an object-vs-world-space promotion bug there was
//          undetectable).  Each new test checks BOTH a non-identity,
//          non-mirrored (rotation + non-uniform stretch, positive
//          determinant) transform AND a mirrored one, full (u, v)
//          frame, against ComputeExpectedFrame fed by the hit's own
//          (GeometryUVRoundtripTest-covered) `derivatives.dpdu`.
//      12. KNOWN, ACCEPTED GAP -- NOT implemented (T2 P2 #3, triaged in
//          the fix round, decision recorded here per the coordinator's
//          instruction): the existing degenerate-UV byte-identity test
//          (case 3 above) only ever exercises the `uvDet ~= 0` branch
//          of TriangleMeshGeometryIndexedSpecializations.h's
//          `useUVJacobian` gate.  The OTHER way to land in that branch
//          -- `thisTri.pCoords[k]` null, i.e. a mesh with NO texcoord
//          array at all (`AddTexCoord` never called) -- has NO test
//          here.  Investigated and deliberately NOT added: reaching
//          that state through the public Begin/Add.../Done API
//          requires `pCoords` (a `std::vector<Point2>`) to be
//          genuinely EMPTY while `DoneIndexedTriangles()`
//          unconditionally executes
//          `myTri.pCoords[i] = &pCoords[tri.iCoords[i]]` with no
//          empty-vector guard -- i.e. the only way to construct this
//          state through the public API is to index an empty
//          `std::vector`, which is UB (in practice, on this toolchain,
//          `&container[0]` on an empty container very likely resolves
//          to a null pointer without an actual load -- but that is an
//          implementation detail this test suite has no other
//          precedent for relying on, and it cannot be verified without
//          a build).  No production loader reaches this state either:
//          every in-tree loader (e.g. TriangleMeshLoaderPLY.cpp:838)
//          pushes a dummy `TexCoord(0,0)` and points every triangle's
//          `iCoords` at it rather than leaving `pCoords` empty, so the
//          null-pointer branch this file's `useUVJacobian` gate guards
//          against is, as far as this audit found, unreachable through
//          any currently-shipping construction path.  A mutated
//          `&&` -> `||` (or a dropped null check) on that branch would
//          not be caught by anything in this suite.  A safe test would
//          need a different construction path (e.g. `Deserialize()`
//          against a hand-built legacy-format byte stream with no
//          coord section) that was out of scope to build blind, with
//          no build access, in this round.
//      13. TestSDFHeightfieldCartesianDiskTangentPairing -- T2 P1: the
//          design doc's own named gate-1 obligation ("The
//          SDFGeometry-heightfield / cartesian_disk pairing
//          (Object.cpp:691-694) is re-checked explicitly; it is a
//          bucket-A case") had NO test at all.  Builds a heightfield
//          SDF and its `cartesian_disk_geometry` + `displaced_geometry`
//          twin from the SAME linear-gradient field (chosen so both
//          the SDF's legacy world-X-projection tangent and the mesh's
//          real per-triangle UV-Jacobian tangent are independently
//          hand-derivable in closed form) and checks the relationship
//          the doc predicts: a PURE one-axis slope (no cross term)
//          makes the two conventions coincide EXACTLY (both reduce to
//          the same formula algebraically); a genuine cross-slope
//          makes them provably, measurably diverge by a small amount
//          -- the bucket-A finding, quantified rather than assumed.
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

#include "../src/Library/Geometry/ClippedPlaneGeometry.h"
#include "../src/Library/Geometry/CylinderGeometry.h"
#include "../src/Library/Geometry/DisplacedGeometry.h"
#include "../src/Library/Geometry/EllipsoidGeometry.h"
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/TorusGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Utilities/GeometricUtilities.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"

using namespace RISE;
using namespace RISE::Implementation;

// ============================================================
//  Harness
// ============================================================

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

namespace
{
	bool Close( Scalar a, Scalar b, Scalar eps )
	{
		return std::fabs( a - b ) < eps;
	}

	bool VecClose( const Vector3& a, const Vector3& b, Scalar eps )
	{
		return Close( a.x, b.x, eps ) && Close( a.y, b.y, eps ) && Close( a.z, b.z, eps );
	}

	//! Standard test-harness intersection call: reset the scratch record,
	//! stamp the ray, and run the full (front+back faces, exit info) query
	//! -- same convention as HairTangentPlumbingTest / CsgSurfacePayloadTest.
	void Hit( IObject* pObj, const Ray& r, RayIntersection& ri )
	{
		ri.geometric.bHit = false;
		ri.geometric.range = RISE_INFINITY;
		ri.geometric.range2 = RISE_INFINITY;
		ri.geometric.ray = r;
		pObj->IntersectRay( ri, RISE_INFINITY, true, true, true );
	}

	//! Checks {u,v,w} is unit + mutually orthogonal, with handedness
	//! matching `expectedHandedness` (+1 right-handed, -1 left-handed).
	//! T2 P2 fix-round finding: the plain "right-handed" form below
	//! (CheckOrthonormalRightHanded, still used by every call site that
	//! predates the fix round) is tautologically true for ANY
	//! correctly-built ONB -- CreateFromWU / CreateFromW always emit a
	//! right-handed triple by construction -- so it can only prove
	//! internal self-consistency, never catch a chirality/sign error.
	//! A mirrored-transform hit is DELIBERATELY left-handed after the
	//! P1 fix (OrthonormalBasis3D::FlipV, docs/CLOTH_FABRIC_DESIGN.md
	//! 9.9 fix round), so ITS test must assert -1 here, against an
	//! INDEPENDENTLY-computed expected value (ComputeExpectedFrame
	//! below), not merely "is this consistent with itself."
	void CheckOrthonormalFrame( const OrthonormalBasis3D& onb, Scalar expectedHandedness, const char* tag )
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
		std::snprintf( buf, sizeof(buf), "%s: handedness (u x v . w == %.0f)", tag, (double)expectedHandedness );
		Check( Close( Vector3Ops::Dot( Vector3Ops::Cross( onb.u(), onb.v() ), onb.w() ), expectedHandedness, 1e-6 ), buf );
	}

	//! Checks {u,v,w} is a right-handed orthonormal frame.  Kept for
	//! every pre-fix-round (non-mirrored) call site; delegates to
	//! CheckOrthonormalFrame with the tautologically-expected +1 (see
	//! that function's doc for why a mirrored hit must NOT use this
	//! overload).
	void CheckOrthonormalRightHanded( const OrthonormalBasis3D& onb, const char* tag )
	{
		CheckOrthonormalFrame( onb, Scalar( 1 ), tag );
	}

	//! Independent ground truth for the ONB's u-axis given an OBJECT-space
	//! candidate tangent direction, the object's own final transform, and
	//! the (already-promoted, world-space) shading normal.  This is
	//! EXACTLY the promotion + projection Object::IntersectRay performs
	//! in its bShadingTangentFromGeometry branch (Object.cpp ~699-772),
	//! reproduced here from scratch (not read back from `ri`) so the test
	//! is a real check on the mechanism, not a tautology.
	Vector3 ExpectedONBTangent( const Matrix4& M, const Vector3& objDir, const Vector3& n_world )
	{
		const Vector3 tWorld = Vector3Ops::Normalize( Vector3Ops::Transform( M, objDir ) );
		const Vector3 tProj = tWorld - n_world * Vector3Ops::Dot( n_world, tWorld );
		return Vector3Ops::Normalize( tProj );
	}

	//! Sign of det(M)'s upper-left 3x3 (the linear part) -- +1 for an
	//! orientation-preserving transform, -1 for a mirror (`scale -1 1 1`
	//! and similar).  Computed FRESH from the raw Matrix4 fields, not
	//! read from anything Object.cpp computes internally
	//! (m_tangentFrameSign has no public getter, and using it here
	//! would make the "oracle" just re-ask the code its own answer).
	Scalar TransformHandedness( const Matrix4& M )
	{
		const Scalar det3 =
			  M._00 * ( M._11 * M._22 - M._12 * M._21 )
			- M._01 * ( M._10 * M._22 - M._12 * M._20 )
			+ M._02 * ( M._10 * M._21 - M._11 * M._20 );
		return ( det3 < Scalar( 0 ) ) ? Scalar( -1 ) : Scalar( 1 );
	}

	//! T1 P1 fix-round regression guard's core oracle: the expected
	//! (u, v) pair for a shading ONB built from a WORLD-space candidate
	//! tangent `tWorldRaw` (need not be unit) and the hit's own
	//! world-space normal `n_world`, under a transform whose handedness
	//! is `handedness` (see TransformHandedness above).  `u` is the
	//! ordinary tangent-plane projection (no sign ambiguity -- proven
	//! sign-preserving under any invertible linear map by the
	//! t.n==0 => (Mt).(M^-Tn)==0 identity this file's header derives).
	//! `v` is cross(n_world, u) corrected by `handedness` -- EXACTLY the
	//! FlipV() correction Object::IntersectRay / CSGObject::IntersectRay
	//! apply, reproduced here with no call to any OrthonormalBasis3D
	//! method, so this is a real cross-check, not the ONB grading its
	//! own homework.
	struct ExpectedFrame { Vector3 u; Vector3 v; };

	ExpectedFrame ComputeExpectedFrameFromWorldTangent( Scalar handedness, const Vector3& tWorldRaw, const Vector3& n_world )
	{
		const Vector3 tWorld = Vector3Ops::Normalize( tWorldRaw );
		const Vector3 tProj = tWorld - n_world * Vector3Ops::Dot( n_world, tWorld );
		ExpectedFrame f;
		f.u = Vector3Ops::Normalize( tProj );
		f.v = Vector3Ops::Normalize( Vector3Ops::Cross( n_world, f.u ) ) * handedness;
		return f;
	}

	//! Convenience overload of the above for the mesh/clipped-plane cases,
	//! which have a clean OBJECT-space candidate tangent to forward-
	//! transform (mirrors ExpectedONBTangent's calling convention).
	ExpectedFrame ComputeExpectedFrame( const Matrix4& M, const Vector3& objDir, const Vector3& n_world )
	{
		return ComputeExpectedFrameFromWorldTangent(
			TransformHandedness( M ), Vector3Ops::Transform( M, objDir ), n_world );
	}

	// Deterministic LCG so the test is reproducible across runs / platforms.
	// (Same idiom as GeometryUVRoundtripTest.cpp.)
	struct LCG
	{
		unsigned long long state;
		explicit LCG( unsigned long long seed ) : state( seed ) {}

		Scalar next01()
		{
			state = state * 6364136223846793005ULL + 1442695040888963407ULL;
			return Scalar( (state >> 11) & ((1ULL << 53) - 1) ) / Scalar( 1ULL << 53 );
		}
	};

	//! Aim a ray at the geometry's own bounding-sphere centre from a random
	//! direction, with lateral jitter, so a broad sweep of the surface gets
	//! hit -- same technique as GeometryUVRoundtripTest::ShootHit, driven
	//! through an Object (assumed IDENTITY transform, so object space ==
	//! world space and the geometry's own bounding sphere can be used
	//! directly as the aiming sphere).
	bool ShootHitOnObject( Object* pObj, const IGeometry& g, LCG& rng, RayIntersection& ri )
	{
		Point3 sphereCenter;
		Scalar sphereRadius;
		g.GenerateBoundingSphere( sphereCenter, sphereRadius );
		if( !std::isfinite( sphereRadius ) || sphereRadius > 1e6 ) {
			sphereRadius = 1.0;
		}

		const Scalar u = rng.next01();
		const Scalar v = rng.next01();
		const Scalar costheta = 1.0 - 2.0 * u;
		const Scalar sintheta = std::sqrt( std::max( 0.0, 1.0 - costheta * costheta ) );
		const Scalar phi      = 2.0 * PI * v;
		const Vector3 originDir( sintheta * std::cos( phi ),
		                         sintheta * std::sin( phi ),
		                         costheta );

		Vector3 axisU;
		if( std::fabs( originDir.x ) < 0.9 ) {
			axisU = Vector3Ops::Normalize( Vector3Ops::Cross( originDir, Vector3( 1, 0, 0 ) ) );
		} else {
			axisU = Vector3Ops::Normalize( Vector3Ops::Cross( originDir, Vector3( 0, 1, 0 ) ) );
		}
		const Vector3 axisV = Vector3Ops::Cross( originDir, axisU );

		const Scalar j1 = (rng.next01() * 2.0) - 1.0;
		const Scalar j2 = (rng.next01() * 2.0) - 1.0;
		const Scalar jr = sphereRadius * 0.3 * std::sqrt( j1 * j1 + j2 * j2 );
		const Scalar jt = std::atan2( j2, j1 );
		const Vector3 lateral = axisU * (jr * std::cos( jt ))
		                      + axisV * (jr * std::sin( jt ));

		const Point3 target(
			sphereCenter.x + lateral.x,
			sphereCenter.y + lateral.y,
			sphereCenter.z + lateral.z );

		const Point3 origin(
			sphereCenter.x + 4.0 * sphereRadius * originDir.x,
			sphereCenter.y + 4.0 * sphereRadius * originDir.y,
			sphereCenter.z + 4.0 * sphereRadius * originDir.z );

		const Vector3 rayDir = Vector3Ops::Normalize( Vector3Ops::mkVector3( target, origin ) );

		Ray r( origin, rayDir );
		Hit( pObj, r, ri );
		return ri.geometric.bHit;
	}

	//////////////////////////////////////////////////////////////
	//  Mesh builders
	//////////////////////////////////////////////////////////////

	//! A unit quad in the z=0 plane, split into two triangles along the
	//! (0,0)-(1,1) diagonal: t1 = (v0,v1,v2) covers y<=x, t2 = (v0,v2,v3)
	//! covers y>=x.  `matchingUV` true gives UV == XY exactly (so the UV
	//! Jacobian is the identity and dpdu_obj is EXACTLY (1,0,0) on both
	//! triangles -- clean, hand-computable ground truth); false collapses
	//! every UV to (0,0) (a degenerate, zero-area UV triangle -- forces
	//! useUVJacobian == false).  `withTangent` adds a per-vertex TANGENT
	//! array (all four vertices share `tangentDir`, bitangentSign +1).
	TriangleMeshGeometryIndexed* BuildUnitQuadMeshIndexed(
		bool matchingUV, bool withTangent, const Vector3& tangentDir = Vector3( 0, 1, 0 ) )
	{
		TriangleMeshGeometryIndexed* mesh = new TriangleMeshGeometryIndexed( false, false );
		mesh->BeginIndexedTriangles();

		const Point3 verts[4] = {
			Point3( 0, 0, 0 ), Point3( 1, 0, 0 ), Point3( 1, 1, 0 ), Point3( 0, 1, 0 ) };
		const Vector3 n( 0, 0, 1 );
		const Point2 uvMatching[4] = {
			Point2( 0, 0 ), Point2( 1, 0 ), Point2( 1, 1 ), Point2( 0, 1 ) };

		for( int i = 0; i < 4; i++ ) {
			mesh->AddVertex( verts[i] );
			mesh->AddNormal( n );
			mesh->AddTexCoord( matchingUV ? uvMatching[i] : Point2( 0, 0 ) );
			if( withTangent ) {
				Tangent4 t;
				t.dir = tangentDir;
				t.bitangentSign = 1.0;
				mesh->AddTangent( t );
			}
		}

		IndexedTriangle t1, t2;
		t1.iVertices[0] = 0; t1.iVertices[1] = 1; t1.iVertices[2] = 2;
		t2.iVertices[0] = 0; t2.iVertices[1] = 2; t2.iVertices[2] = 3;
		for( int k = 0; k < 3; k++ ) {
			t1.iNormals[k] = t1.iVertices[k]; t1.iCoords[k] = t1.iVertices[k];
			t2.iNormals[k] = t2.iVertices[k]; t2.iCoords[k] = t2.iVertices[k];
		}
		mesh->AddIndexedTriangle( t1 );
		mesh->AddIndexedTriangle( t2 );
		mesh->DoneIndexedTriangles();
		return mesh;
	}

	//! Non-indexed twin of BuildUnitQuadMeshIndexed (no TANGENT path exists
	//! on this geometry family at all).
	TriangleMeshGeometry* BuildUnitQuadMeshNonIndexed( bool matchingUV )
	{
		TriangleMeshGeometry* mesh = new TriangleMeshGeometry( false );

		const Point3 verts[4] = {
			Point3( 0, 0, 0 ), Point3( 1, 0, 0 ), Point3( 1, 1, 0 ), Point3( 0, 1, 0 ) };
		const Vector3 n( 0, 0, 1 );
		const Point2 uvMatching[4] = {
			Point2( 0, 0 ), Point2( 1, 0 ), Point2( 1, 1 ), Point2( 0, 1 ) };
		const Point2 uvDegenerate[4] = { Point2(0,0), Point2(0,0), Point2(0,0), Point2(0,0) };
		const Point2* uv = matchingUV ? uvMatching : uvDegenerate;

		Triangle t1, t2;
		const int idx1[3] = { 0, 1, 2 };
		const int idx2[3] = { 0, 2, 3 };
		for( int k = 0; k < 3; k++ ) {
			t1.vertices[k] = verts[ idx1[k] ]; t1.normals[k] = n; t1.coords[k] = uv[ idx1[k] ];
			t2.vertices[k] = verts[ idx2[k] ]; t2.normals[k] = n; t2.coords[k] = uv[ idx2[k] ];
		}
		mesh->AddTriangle( t1 );
		mesh->AddTriangle( t2 );
		mesh->DoneTriangles();
		return mesh;
	}

	//! Minimal IFunction2D stub for the SDF-heightfield / cartesian_disk
	//! pairing test (T2 P1 #1): f(u,v) = a*u + b*v.  A LINEAR gradient
	//! (zero curvature) is deliberately chosen so BOTH the SDF's world-X-
	//! projection tangent AND the mesh's real per-triangle UV-Jacobian
	//! tangent are exactly hand-derivable in closed form (see the test's
	//! own comment for the derivation) -- unlike a curved field, where
	//! only the actual code output could be measured, not independently
	//! predicted.  Same idiom as HairTangentPlumbingTest.cpp's own
	//! LinearGradientFunction2D (a separate, file-local copy -- no
	//! shared header exists for it).
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
// Test 1 (money assertion): indexed mesh, UV Jacobian dpdu, both
// triangles, non-identity transform.
// ============================================================
static void TestIndexedMeshUVTangent_Money()
{
	std::cout << "Indexed mesh: UV-Jacobian dpdu drives the shading ONB (money assertion)..." << std::endl;

	TriangleMeshGeometryIndexed* g = BuildUnitQuadMeshIndexed( /*matchingUV=*/true, /*withTangent=*/false );
	Object* o = new Object( g );
	safe_release( g );

	o->SetOrientation( Vector3( 0.2, 0.6, -0.4 ) );
	o->TranslateObject( Vector3( 2, -1, 3 ) );
	o->SetStretch( Vector3( 1.5, 0.8, 1.2 ) );
	o->FinalizeTransformations();
	const Matrix4 M = o->GetFinalTransformMatrix();

	// Triangle 1 (y<=x): object-space point (0.3, 0.2, 0).
	// Triangle 2 (y>=x): object-space point (0.2, 0.3, 0).
	const Point3 objPts[2] = { Point3( 0.3, 0.2, 0 ), Point3( 0.2, 0.3, 0 ) };
	const char* tags[2] = { "Test1(tri1)", "Test1(tri2)" };

	for( int i = 0; i < 2; i++ ) {
		const Point3 objOrigin( objPts[i].x, objPts[i].y, 5.0 );
		const Vector3 objDir( 0, 0, -1 );
		const Point3 worldOrigin = Point3Ops::Transform( M, objOrigin );
		const Vector3 worldDir = Vector3Ops::Normalize( Vector3Ops::Transform( M, objDir ) );

		Ray r( worldOrigin, worldDir );
		RayIntersection ri( r, nullRasterizerState );
		Hit( o, r, ri );

		char buf[256];
		std::snprintf( buf, sizeof(buf), "%s: ray hits the mesh", tags[i] );
		Check( ri.geometric.bHit, buf );
		if( !ri.geometric.bHit ) continue;

		std::snprintf( buf, sizeof(buf), "%s: bShadingTangentFromGeometry set", tags[i] );
		Check( ri.geometric.bShadingTangentFromGeometry, buf );
		std::snprintf( buf, sizeof(buf), "%s: bHasShadingTangent set", tags[i] );
		Check( ri.geometric.bHasShadingTangent, buf );
		std::snprintf( buf, sizeof(buf), "%s: bHasTangent NOT set (no TANGENT array)", tags[i] );
		Check( !ri.geometric.bHasTangent, buf );

		const Vector3 expected = ExpectedONBTangent( M, Vector3( 1, 0, 0 ), ri.geometric.vNormal );
		std::snprintf( buf, sizeof(buf), "%s: MONEY ASSERTION -- onb.u() aligns with world-transformed UV dpdu", tags[i] );
		Check( VecClose( ri.geometric.onb.u(), expected, 1e-9 ), buf );

		std::snprintf( buf, sizeof(buf), "%s: onb.w() equals reported world shading normal", tags[i] );
		Check( VecClose( ri.geometric.onb.w(), ri.geometric.vNormal, 1e-9 ), buf );

		CheckOrthonormalRightHanded( ri.geometric.onb, tags[i] );

		// Cross-check against the independent GeometryUVRoundtripTest-
		// covered derivatives.dpdu (see file header derivation).
		if( ri.geometric.derivatives.valid ) {
			std::snprintf( buf, sizeof(buf), "%s: cross-check -- onb.u() == Normalize(derivatives.dpdu)", tags[i] );
			Check( VecClose( ri.geometric.onb.u(),
				Vector3Ops::Normalize( ri.geometric.derivatives.dpdu ), 1e-9 ), buf );
		}
	}

	o->release();
}

// ============================================================
// Test 2 (money assertion): authored TANGENT wins over dpdu.
// ============================================================
static void TestIndexedMeshTangentPriority_Money()
{
	std::cout << "Indexed mesh: authored TANGENT wins over UV-Jacobian dpdu (money assertion)..." << std::endl;

	const Vector3 tangentDir( 0, 1, 0 );	// orthogonal to dpdu=(1,0,0), and to n=(0,0,1)
	TriangleMeshGeometryIndexed* g = BuildUnitQuadMeshIndexed( /*matchingUV=*/true, /*withTangent=*/true, tangentDir );
	Object* o = new Object( g );
	safe_release( g );

	o->SetOrientation( Vector3( 0.2, 0.6, -0.4 ) );
	o->TranslateObject( Vector3( 2, -1, 3 ) );
	o->SetStretch( Vector3( 1.5, 0.8, 1.2 ) );
	o->FinalizeTransformations();
	const Matrix4 M = o->GetFinalTransformMatrix();

	const Point3 objOrigin( 0.3, 0.2, 5.0 );
	const Vector3 objDir( 0, 0, -1 );
	const Point3 worldOrigin = Point3Ops::Transform( M, objOrigin );
	const Vector3 worldDir = Vector3Ops::Normalize( Vector3Ops::Transform( M, objDir ) );

	Ray r( worldOrigin, worldDir );
	RayIntersection ri( r, nullRasterizerState );
	Hit( o, r, ri );

	Check( ri.geometric.bHit, "Test2: ray hits the mesh" );
	Check( ri.geometric.bHasTangent, "Test2: bHasTangent set (TANGENT array present)" );
	Check( ri.geometric.bShadingTangentFromGeometry, "Test2: bShadingTangentFromGeometry set" );
	Check( ri.geometric.bHasShadingTangent, "Test2: bHasShadingTangent set" );

	const Vector3 expectedFromTangent = ExpectedONBTangent( M, tangentDir, ri.geometric.vNormal );
	const Vector3 expectedFromDpdu    = ExpectedONBTangent( M, Vector3( 1, 0, 0 ), ri.geometric.vNormal );

	// Sanity: the two candidate directions really are distinct here.
	Check( std::fabs( Vector3Ops::Dot( expectedFromTangent, expectedFromDpdu ) ) < 0.999,
		"Test2: (sanity) TANGENT direction is NOT the dpdu direction here" );

	Check( VecClose( ri.geometric.onb.u(), expectedFromTangent, 1e-9 ),
		"Test2: MONEY ASSERTION -- onb.u() tracks the authored TANGENT, not dpdu" );
	Check( !VecClose( ri.geometric.onb.u(), expectedFromDpdu, 1e-6 ),
		"Test2: (sanity) onb.u() does NOT match the dpdu-only expectation" );

	// vTangent is promoted (Normalize(Transform(M, ...))) by the exact
	// same formula as the ONB branch on the exact same object-space
	// source vector -- and (0,1,0) is exactly orthogonal to the object-
	// space normal (0,0,1), so the world-space projection inside the ONB
	// branch is a no-op (same identity as the file header derivation).
	// onb.u() must therefore equal the promoted vTangent exactly.
	Check( VecClose( ri.geometric.onb.u(), ri.geometric.vTangent, 1e-9 ),
		"Test2: onb.u() equals the promoted ri.geometric.vTangent exactly" );

	CheckOrthonormalRightHanded( ri.geometric.onb, "Test2" );

	o->release();
}

// ============================================================
// Test 3: degenerate UV, no TANGENT -- byte-identical legacy path.
// ============================================================
static void TestIndexedMeshDegenerateUV_ByteIdentical()
{
	std::cout << "Indexed mesh: degenerate UV triangle leaves the legacy CreateFromW path untouched..." << std::endl;

	TriangleMeshGeometryIndexed* g = BuildUnitQuadMeshIndexed( /*matchingUV=*/false, /*withTangent=*/false );
	Object* o = new Object( g );
	safe_release( g );
	o->SetOrientation( Vector3( 0.2, 0.6, -0.4 ) );
	o->TranslateObject( Vector3( 2, -1, 3 ) );
	o->SetStretch( Vector3( 1.5, 0.8, 1.2 ) );
	o->FinalizeTransformations();
	const Matrix4 M = o->GetFinalTransformMatrix();

	const Point3 objOrigin( 0.3, 0.2, 5.0 );
	const Vector3 objDir( 0, 0, -1 );
	const Point3 worldOrigin = Point3Ops::Transform( M, objOrigin );
	const Vector3 worldDir = Vector3Ops::Normalize( Vector3Ops::Transform( M, objDir ) );

	Ray r( worldOrigin, worldDir );
	RayIntersection ri( r, nullRasterizerState );
	Hit( o, r, ri );

	Check( ri.geometric.bHit, "Test3: ray hits the mesh" );
	Check( !ri.geometric.bShadingTangentFromGeometry, "Test3: bShadingTangentFromGeometry stays FALSE (degenerate UV)" );
	Check( !ri.geometric.bHasShadingTangent, "Test3: bHasShadingTangent stays FALSE" );

	// Independent ground truth: a fresh CreateFromW on the SAME reported
	// world normal.  Not read back from ri.geometric.onb.
	OrthonormalBasis3D golden;
	golden.CreateFromW( ri.geometric.vNormal );

	Check( VecClose( ri.geometric.onb.u(), golden.u(), 1e-12 ), "Test3: onb.u() bit-identical to fresh CreateFromW" );
	Check( VecClose( ri.geometric.onb.v(), golden.v(), 1e-12 ), "Test3: onb.v() bit-identical to fresh CreateFromW" );
	Check( VecClose( ri.geometric.onb.w(), golden.w(), 1e-12 ), "Test3: onb.w() bit-identical to fresh CreateFromW" );

	o->release();
}

// ============================================================
// Test 4 (money assertion): non-indexed mesh twin of Test 1.
// ============================================================
static void TestNonIndexedMeshUVTangent_Money()
{
	std::cout << "Non-indexed mesh: UV-Jacobian dpdu drives the shading ONB (money assertion)..." << std::endl;

	TriangleMeshGeometry* g = BuildUnitQuadMeshNonIndexed( /*matchingUV=*/true );
	Object* o = new Object( g );
	safe_release( g );

	o->SetOrientation( Vector3( -0.3, 0.5, 0.9 ) );
	o->TranslateObject( Vector3( -4, 6, 1 ) );
	o->SetStretch( Vector3( 2.0, 1.4, 0.7 ) );
	o->FinalizeTransformations();
	const Matrix4 M = o->GetFinalTransformMatrix();

	const Point3 objPts[2] = { Point3( 0.3, 0.2, 0 ), Point3( 0.2, 0.3, 0 ) };
	const char* tags[2] = { "Test4(tri1)", "Test4(tri2)" };

	for( int i = 0; i < 2; i++ ) {
		const Point3 objOrigin( objPts[i].x, objPts[i].y, 5.0 );
		const Vector3 objDir( 0, 0, -1 );
		const Point3 worldOrigin = Point3Ops::Transform( M, objOrigin );
		const Vector3 worldDir = Vector3Ops::Normalize( Vector3Ops::Transform( M, objDir ) );

		Ray r( worldOrigin, worldDir );
		RayIntersection ri( r, nullRasterizerState );
		Hit( o, r, ri );

		char buf[256];
		std::snprintf( buf, sizeof(buf), "%s: ray hits the mesh", tags[i] );
		Check( ri.geometric.bHit, buf );
		if( !ri.geometric.bHit ) continue;

		std::snprintf( buf, sizeof(buf), "%s: bHasShadingTangent set", tags[i] );
		Check( ri.geometric.bHasShadingTangent, buf );

		const Vector3 expected = ExpectedONBTangent( M, Vector3( 1, 0, 0 ), ri.geometric.vNormal );
		std::snprintf( buf, sizeof(buf), "%s: MONEY ASSERTION -- onb.u() aligns with world-transformed UV dpdu", tags[i] );
		Check( VecClose( ri.geometric.onb.u(), expected, 1e-9 ), buf );

		CheckOrthonormalRightHanded( ri.geometric.onb, tags[i] );
	}

	o->release();
}

// ============================================================
// Test 5: non-indexed mesh twin of Test 3.
// ============================================================
static void TestNonIndexedMeshDegenerateUV_ByteIdentical()
{
	std::cout << "Non-indexed mesh: degenerate UV triangle leaves the legacy CreateFromW path untouched..." << std::endl;

	TriangleMeshGeometry* g = BuildUnitQuadMeshNonIndexed( /*matchingUV=*/false );
	Object* o = new Object( g );
	safe_release( g );
	o->SetOrientation( Vector3( -0.3, 0.5, 0.9 ) );
	o->TranslateObject( Vector3( -4, 6, 1 ) );
	o->SetStretch( Vector3( 2.0, 1.4, 0.7 ) );
	o->FinalizeTransformations();
	const Matrix4 M = o->GetFinalTransformMatrix();

	const Point3 objOrigin( 0.3, 0.2, 5.0 );
	const Vector3 objDir( 0, 0, -1 );
	const Point3 worldOrigin = Point3Ops::Transform( M, objOrigin );
	const Vector3 worldDir = Vector3Ops::Normalize( Vector3Ops::Transform( M, objDir ) );

	Ray r( worldOrigin, worldDir );
	RayIntersection ri( r, nullRasterizerState );
	Hit( o, r, ri );

	Check( ri.geometric.bHit, "Test5: ray hits the mesh" );
	Check( !ri.geometric.bHasShadingTangent, "Test5: bHasShadingTangent stays FALSE" );

	OrthonormalBasis3D golden;
	golden.CreateFromW( ri.geometric.vNormal );
	Check( VecClose( ri.geometric.onb.u(), golden.u(), 1e-12 ), "Test5: onb.u() bit-identical to fresh CreateFromW" );
	Check( VecClose( ri.geometric.onb.w(), golden.w(), 1e-12 ), "Test5: onb.w() bit-identical to fresh CreateFromW" );

	o->release();
}

// ============================================================
// Test 6: mirrored transform (negative determinant) -- mesh.
// ============================================================
static void TestMirroredTransform_Mesh()
{
	std::cout << "Indexed mesh: mirrored (scale -1 1 1) transform still yields a valid tangent..." << std::endl;

	TriangleMeshGeometryIndexed* g = BuildUnitQuadMeshIndexed( /*matchingUV=*/true, /*withTangent=*/false );
	Object* o = new Object( g );
	safe_release( g );

	o->SetOrientation( Vector3( 0.1, 0.4, 0.2 ) );
	o->TranslateObject( Vector3( 1, 2, -3 ) );
	o->SetStretch( Vector3( -1.0, 1.0, 1.0 ) );	// mirrored: negative determinant
	o->FinalizeTransformations();
	const Matrix4 M = o->GetFinalTransformMatrix();

	const Point3 objOrigin( 0.3, 0.2, 5.0 );
	const Vector3 objDir( 0, 0, -1 );
	const Point3 worldOrigin = Point3Ops::Transform( M, objOrigin );
	const Vector3 worldDir = Vector3Ops::Normalize( Vector3Ops::Transform( M, objDir ) );

	Ray r( worldOrigin, worldDir );
	RayIntersection ri( r, nullRasterizerState );
	Hit( o, r, ri );

	Check( ri.geometric.bHit, "Test6: ray hits the mirrored mesh" );
	Check( ri.geometric.bHasShadingTangent, "Test6: bHasShadingTangent set under a mirrored transform" );

	const Vector3 expected = ExpectedONBTangent( M, Vector3( 1, 0, 0 ), ri.geometric.vNormal );
	Check( VecClose( ri.geometric.onb.u(), expected, 1e-9 ),
		"Test6: MONEY ASSERTION -- onb.u() still matches the independent formula under mirroring" );

	// T1 P1 fix-round STRENGTHENING: onb.u() alone cannot catch a V-sign
	// (chirality) error -- see this file's header derivation.  Check the
	// full frame against the independent ComputeExpectedFrame oracle,
	// including handedness == -1 (this transform is mirrored).
	const ExpectedFrame expectedFrame = ComputeExpectedFrame( M, Vector3( 1, 0, 0 ), ri.geometric.vNormal );
	Check( VecClose( ri.geometric.onb.v(), expectedFrame.v, 1e-9 ),
		"Test6: MONEY ASSERTION -- onb.v() matches the independent FlipV-corrected formula under mirroring" );
	CheckOrthonormalFrame( ri.geometric.onb, Scalar( -1 ), "Test6" );

	o->release();
}

// ============================================================
// Test 7: mirrored transform -- analytic primitive (sphere).
// ============================================================
static void TestMirroredTransform_Sphere()
{
	std::cout << "Sphere: mirrored (scale -1 1 1) transform still yields a valid tangent..." << std::endl;

	SphereGeometry* g = new SphereGeometry( 1.0 );
	Object* o = new Object( g );
	safe_release( g );
	o->SetOrientation( Vector3( 0.3, -0.2, 0.5 ) );
	o->TranslateObject( Vector3( 5, -5, 2 ) );
	o->SetStretch( Vector3( -1.0, 1.0, 1.0 ) );
	o->FinalizeTransformations();

	// Object-space ray hitting a generic (non-pole) point: origin (2,0,0),
	// dir (-1,0,0) crosses the unit sphere at (1,0,0), away from the
	// Y-axis pole singularity this primitive's UV uses.
	const Matrix4 M = o->GetFinalTransformMatrix();
	const Point3 objOrigin( 2, 0, 0 );
	const Vector3 objDir( -1, 0, 0 );
	const Point3 worldOrigin = Point3Ops::Transform( M, objOrigin );
	const Vector3 worldDir = Vector3Ops::Normalize( Vector3Ops::Transform( M, objDir ) );

	Ray r( worldOrigin, worldDir );
	RayIntersection ri( r, nullRasterizerState );
	Hit( o, r, ri );

	Check( ri.geometric.bHit, "Test7: ray hits the mirrored sphere" );
	Check( ri.geometric.bHasShadingTangent, "Test7: bHasShadingTangent set under a mirrored transform" );
	Check( ri.geometric.derivatives.valid, "Test7: derivatives.valid set" );
	if( ri.geometric.derivatives.valid ) {
		Check( VecClose( ri.geometric.onb.u(),
			Vector3Ops::Normalize( ri.geometric.derivatives.dpdu ), 1e-7 ),
			"Test7: MONEY ASSERTION -- onb.u() == Normalize(derivatives.dpdu) under mirroring" );

		// T1 P1 fix-round STRENGTHENING: full-frame check via the
		// INDEPENDENT oracle, using `derivatives.dpdu` itself (world-
		// space, GeometryUVRoundtripTest-covered) as the "world-space
		// tangent from the transform" -- not any OrthonormalBasis3D
		// method -- exactly what T1/T2 asked for.
		const ExpectedFrame expectedFrame = ComputeExpectedFrameFromWorldTangent(
			TransformHandedness( M ), ri.geometric.derivatives.dpdu, ri.geometric.vNormal );
		Check( VecClose( ri.geometric.onb.v(), expectedFrame.v, 1e-7 ),
			"Test7: MONEY ASSERTION -- onb.v() matches the independent FlipV-corrected formula under mirroring" );
		CheckOrthonormalFrame( ri.geometric.onb, Scalar( -1 ), "Test7" );
	}

	o->release();
}

// ============================================================
// Test 7b/7c (T2 P2 #4): Ellipsoid / Cylinder under a non-identity,
// non-mirrored transform AND a mirrored one -- before this, only
// Sphere had ANY non-identity-transform check among the analytic
// primitives, so an object-vs-world-space promotion bug specific to
// Ellipsoid/Torus/Cylinder's write sites (EllipsoidGeometry.cpp:189-
// 195, CylinderGeometry.cpp:390-399/528-534) was undetectable by this
// file.
// ============================================================
static void CheckPrimitiveTangentUnderTransform(
	IGeometry* g, const char* tag,
	const Vector3& orientation, const Vector3& translate, const Vector3& stretch,
	const Point3& objOrigin, const Vector3& objDir,
	Scalar expectedHandedness )
{
	Object* o = new Object( g );
	safe_release( g );
	o->SetOrientation( orientation );
	o->TranslateObject( translate );
	o->SetStretch( stretch );
	o->FinalizeTransformations();
	const Matrix4 M = o->GetFinalTransformMatrix();

	const Point3 worldOrigin = Point3Ops::Transform( M, objOrigin );
	const Vector3 worldDir = Vector3Ops::Normalize( Vector3Ops::Transform( M, objDir ) );

	Ray r( worldOrigin, worldDir );
	RayIntersection ri( r, nullRasterizerState );
	Hit( o, r, ri );

	char buf[256];
	std::snprintf( buf, sizeof(buf), "%s: ray hits", tag );
	Check( ri.geometric.bHit, buf );
	std::snprintf( buf, sizeof(buf), "%s: bHasShadingTangent set", tag );
	Check( ri.geometric.bHasShadingTangent, buf );
	std::snprintf( buf, sizeof(buf), "%s: derivatives.valid set", tag );
	Check( ri.geometric.derivatives.valid, buf );

	if( ri.geometric.bHit && ri.geometric.derivatives.valid ) {
		std::snprintf( buf, sizeof(buf), "%s: MONEY ASSERTION -- onb.u() == Normalize(derivatives.dpdu)", tag );
		Check( VecClose( ri.geometric.onb.u(),
			Vector3Ops::Normalize( ri.geometric.derivatives.dpdu ), 1e-7 ), buf );

		const Scalar measuredHandedness = TransformHandedness( M );
		std::snprintf( buf, sizeof(buf), "%s: (sanity) transform handedness matches the scenario's expectation", tag );
		Check( Close( measuredHandedness, expectedHandedness, 1e-9 ), buf );

		const ExpectedFrame expectedFrame = ComputeExpectedFrameFromWorldTangent(
			measuredHandedness, ri.geometric.derivatives.dpdu, ri.geometric.vNormal );
		std::snprintf( buf, sizeof(buf), "%s: MONEY ASSERTION -- onb.v() matches the independent FlipV-corrected formula", tag );
		Check( VecClose( ri.geometric.onb.v(), expectedFrame.v, 1e-7 ), buf );

		std::snprintf( buf, sizeof(buf), "%s (frame)", tag );
		CheckOrthonormalFrame( ri.geometric.onb, expectedHandedness, buf );
	}

	o->release();
}

static void TestMirroredTransform_Ellipsoid()
{
	std::cout << "Ellipsoid: non-identity and mirrored transforms both yield a valid tangent..." << std::endl;

	// Object-space ray hitting a generic (non-pole) point: origin
	// (2,0,0), dir (-1,0,0) crosses the ellipsoid (radii 1.0,1.3,0.8)
	// at (1.0,0,0), away from the Y-axis pole this primitive's UV uses
	// (same convention as SphereGeometry).
	const Point3 objOrigin( 2, 0, 0 );
	const Vector3 objDir( -1, 0, 0 );

	// Non-identity, NON-mirrored (positive determinant): rotation +
	// non-uniform stretch.
	CheckPrimitiveTangentUnderTransform(
		new EllipsoidGeometry( Vector3( 1.0, 1.3, 0.8 ) ), "Test7b(rotated)",
		Vector3( 0.4, -0.6, 0.3 ), Vector3( 3, -2, 4 ), Vector3( 1.6, 0.9, 2.1 ),
		objOrigin, objDir, Scalar( 1 ) );

	// Mirrored (negative determinant).
	CheckPrimitiveTangentUnderTransform(
		new EllipsoidGeometry( Vector3( 1.0, 1.3, 0.8 ) ), "Test7b(mirrored)",
		Vector3( 0.3, -0.2, 0.5 ), Vector3( 5, -5, 2 ), Vector3( -1.0, 1.0, 1.0 ),
		objOrigin, objDir, Scalar( -1 ) );
}

static void TestMirroredTransform_Cylinder()
{
	std::cout << "Cylinder: non-identity and mirrored transforms both yield a valid tangent..." << std::endl;

	// Object-space ray hitting the side wall (not a cap): the cylinder
	// spans axial range [-height/2, height/2] = [-1,1] for height=2.0;
	// z=0.3 is safely interior.  Origin (2,0,0.3), dir (-1,0,0) crosses
	// the radius-0.5 side wall at (0.5,0,0.3).
	const Point3 objOrigin( 2, 0, 0.3 );
	const Vector3 objDir( -1, 0, 0 );

	CheckPrimitiveTangentUnderTransform(
		new CylinderGeometry( 'z', 0.5, 2.0, /*capped=*/true ), "Test7c(rotated)",
		Vector3( 0.5, 0.2, -0.4 ), Vector3( -2, 1, 3 ), Vector3( 1.3, 2.2, 0.7 ),
		objOrigin, objDir, Scalar( 1 ) );

	CheckPrimitiveTangentUnderTransform(
		new CylinderGeometry( 'z', 0.5, 2.0, /*capped=*/true ), "Test7c(mirrored)",
		Vector3( 0.1, 0.4, 0.2 ), Vector3( 1, 2, -3 ), Vector3( -1.0, 1.0, 1.0 ),
		objOrigin, objDir, Scalar( -1 ) );
}

static void TestMirroredTransform_Torus()
{
	std::cout << "Torus: non-identity and mirrored transforms both yield a valid tangent..." << std::endl;

	// Object-space ray hitting the outer equator (U=0, V=0 in
	// TorusGeometry::TessellateToMesh's own convention -- ring in the
	// XZ plane, tube axis Y): pos = ((R+r)cosU, r*sinV, (R+r)sinU) is
	// (R+r, 0, 0) = (1.3, 0, 0) for R=1.0, r=0.3.  No pole/singularity
	// exists anywhere on a torus, but this point matches
	// GeometryUVRoundtripTest.cpp's own documented parameterisation
	// exactly, so it is a known-good, non-accidental hit.
	const Point3 objOrigin( 3, 0, 0 );
	const Vector3 objDir( -1, 0, 0 );

	CheckPrimitiveTangentUnderTransform(
		new TorusGeometry( 1.0, 0.3 ), "Test7d(rotated)",
		Vector3( 0.2, 0.5, -0.3 ), Vector3( 2, -3, 1 ), Vector3( 0.8, 1.7, 2.4 ),
		objOrigin, objDir, Scalar( 1 ) );

	CheckPrimitiveTangentUnderTransform(
		new TorusGeometry( 1.0, 0.3 ), "Test7d(mirrored)",
		Vector3( -0.4, 0.1, 0.6 ), Vector3( -1, 4, -2 ), Vector3( 1.0, -1.0, 1.0 ),
		objOrigin, objDir, Scalar( -1 ) );
}

// ============================================================
// Test 8: analytic primitives -- broad random-ray sweep.
// ============================================================
static void CheckAnalyticPrimitiveTangent( IGeometry* g, const char* tag, unsigned long long seed, int numSamples = 300 )
{
	Object* o = new Object( g );
	safe_release( g );
	o->FinalizeTransformations();	// identity transform

	LCG rng( seed );
	int nChecked = 0;
	for( int i = 0; i < numSamples; i++ ) {
		RayIntersection ri( Ray( Point3(0,0,0), Vector3(1,0,0) ), nullRasterizerState );
		if( !ShootHitOnObject( o, *o->GetGeometry(), rng, ri ) ) continue;
		if( !ri.geometric.derivatives.valid ) continue;

		char buf[256];
		std::snprintf( buf, sizeof(buf), "%s: bShadingTangentFromGeometry set (sample %d)", tag, i );
		Check( ri.geometric.bShadingTangentFromGeometry, buf );
		std::snprintf( buf, sizeof(buf), "%s: bHasShadingTangent set (sample %d)", tag, i );
		Check( ri.geometric.bHasShadingTangent, buf );

		const Vector3 expected = Vector3Ops::Normalize( ri.geometric.derivatives.dpdu );
		std::snprintf( buf, sizeof(buf), "%s: onb.u() == Normalize(derivatives.dpdu) (sample %d)", tag, i );
		Check( VecClose( ri.geometric.onb.u(), expected, 1e-7 ), buf );

		std::snprintf( buf, sizeof(buf), "%s: orthonormal right-handed (sample %d)", tag, i );
		CheckOrthonormalRightHanded( ri.geometric.onb, buf );

		nChecked++;
	}

	char buf[256];
	std::snprintf( buf, sizeof(buf), "%s: enough samples actually hit + had valid derivatives", tag );
	Check( nChecked > numSamples / 4, buf );

	o->release();
}

static void TestAnalyticPrimitiveTangent()
{
	std::cout << "Analytic primitives: dpdu drives the shading ONB across a random-ray sweep..." << std::endl;

	CheckAnalyticPrimitiveTangent( new SphereGeometry( 1.0 ), "Sphere", 12345ULL );
	CheckAnalyticPrimitiveTangent( new EllipsoidGeometry( Vector3( 1.0, 1.3, 0.8 ) ), "Ellipsoid", 23456ULL );
	CheckAnalyticPrimitiveTangent( new TorusGeometry( 1.0, 0.3 ), "Torus", 34567ULL );
	// Capped cylinder: random sweep hits BOTH the side wall and the caps,
	// exercising CylinderGeometry.cpp's two ComputeSurfaceDerivatives call
	// sites (the capped-path block and its cap-vs-wall dndu/dndv split).
	CheckAnalyticPrimitiveTangent( new CylinderGeometry( 'z', 0.5, 2.0, /*capped=*/true ), "Cylinder(capped)", 45678ULL );
	// Open (uncapped) cylinder: exercises the "open-tube path" site.
	CheckAnalyticPrimitiveTangent( new CylinderGeometry( 'z', 0.5, 2.0, /*capped=*/false ), "Cylinder(open)", 56789ULL );
}

// ============================================================
// Test 9 (money assertion): ClippedPlaneGeometry -- a NEW write
// site (never populated ri.derivatives before or after this fix).
// ============================================================
static void TestClippedPlaneTangent_Money()
{
	std::cout << "ClippedPlaneGeometry: analytic UV tangent drives the shading ONB (money assertion)..." << std::endl;

	// Planar rectangle (parallelogram: D = c11-c10-c01+c00 == 0), so
	// BilinearTangentU is the CONSTANT ptb-pta regardless of v.
	Point3 vP[4] = { Point3(0,0,0), Point3(1,0,0), Point3(1,1,0), Point3(0,1,0) };
	ClippedPlaneGeometry* g = new ClippedPlaneGeometry( vP, /*bDoubleSided_=*/true );
	Object* o = new Object( g );
	safe_release( g );

	o->SetOrientation( Vector3( 0.4, -0.3, 0.6 ) );
	o->TranslateObject( Vector3( -2, 3, 5 ) );
	o->SetStretch( Vector3( 1.3, 0.9, 1.7 ) );
	o->FinalizeTransformations();
	const Matrix4 M = o->GetFinalTransformMatrix();

	const Point3 objOrigin( 0.5, 0.5, 5.0 );
	const Vector3 objDir( 0, 0, -1 );
	const Point3 worldOrigin = Point3Ops::Transform( M, objOrigin );
	const Vector3 worldDir = Vector3Ops::Normalize( Vector3Ops::Transform( M, objDir ) );

	Ray r( worldOrigin, worldDir );
	RayIntersection ri( r, nullRasterizerState );
	Hit( o, r, ri );

	Check( ri.geometric.bHit, "Test9: ray hits the clipped plane" );
	Check( ri.geometric.bShadingTangentFromGeometry, "Test9: bShadingTangentFromGeometry set" );
	Check( ri.geometric.bHasShadingTangent, "Test9: bHasShadingTangent set" );
	Check( !ri.geometric.derivatives.valid,
		"Test9: (per design) ri.derivatives itself is still NEVER populated by this geometry" );

	const Vector3 objDpdu = Vector3Ops::mkVector3( vP[1], vP[0] );	// ptb - pta = (1,0,0)
	const Vector3 expected = ExpectedONBTangent( M, objDpdu, ri.geometric.vNormal );
	Check( VecClose( ri.geometric.onb.u(), expected, 1e-9 ),
		"Test9: MONEY ASSERTION -- onb.u() aligns with the world-transformed analytic UV tangent" );

	CheckOrthonormalRightHanded( ri.geometric.onb, "Test9" );

	o->release();
}

// ============================================================
// Test 10: CSG-nested mesh gets the same tangent as un-nested.
// ============================================================
static void TestCsgNestedMeshMatchesUnnested()
{
	std::cout << "CSGObject: nested mesh gets the same shading tangent as the un-nested Object..." << std::endl;

	const Point3 objOrigin( 0.3, 0.2, 5.0 );
	const Vector3 objDir( 0, 0, -1 );

	// -------- Plain Object, geometry carries the transform directly. --------
	TriangleMeshGeometryIndexed* gPlain = BuildUnitQuadMeshIndexed( /*matchingUV=*/true, /*withTangent=*/false );
	Object* oPlain = new Object( gPlain );
	safe_release( gPlain );
	oPlain->SetOrientation( Vector3( 0.5, -0.3, 0.8 ) );
	oPlain->TranslateObject( Vector3( -3, 8, 1 ) );
	oPlain->SetStretch( Vector3( 2.0, 0.5, 1.3 ) );
	oPlain->FinalizeTransformations();
	const Matrix4 Mplain = oPlain->GetFinalTransformMatrix();

	{
		const Point3 worldOrigin = Point3Ops::Transform( Mplain, objOrigin );
		const Vector3 worldDir = Vector3Ops::Normalize( Vector3Ops::Transform( Mplain, objDir ) );
		Ray r( worldOrigin, worldDir );
		RayIntersection riPlain( r, nullRasterizerState );
		Hit( oPlain, r, riPlain );
		Check( riPlain.geometric.bHit, "Test10: plain Object hits the mesh" );
		Check( riPlain.geometric.bHasShadingTangent, "Test10: plain Object bHasShadingTangent set" );

		// -------- Same geometry+transform, but wrapped as a CSG operand: --------
		// operand A = identical mesh at IDENTITY transform, operand B = a
		// far-away never-hit sphere, and the CSG composite itself carries
		// the transform `Mplain` used above.
		TriangleMeshGeometryIndexed* gCsg = BuildUnitQuadMeshIndexed( /*matchingUV=*/true, /*withTangent=*/false );
		Object* opA = new Object( gCsg );
		safe_release( gCsg );
		opA->FinalizeTransformations();	// identity

		SphereGeometry* gB = new SphereGeometry( 1.0 );
		Object* opB = new Object( gB );
		safe_release( gB );
		opB->SetPosition( Point3( 100000, 100000, 100000 ) );
		opB->FinalizeTransformations();

		CSGObject* csg = new CSGObject( CSG_UNION );
		const bool assigned = csg->AssignObjects( opA, opB );
		Check( assigned, "Test10: composite takes A(mesh)/B(far sphere) operands" );

		csg->SetOrientation( Vector3( 0.5, -0.3, 0.8 ) );
		csg->TranslateObject( Vector3( -3, 8, 1 ) );
		csg->SetStretch( Vector3( 2.0, 0.5, 1.3 ) );
		csg->FinalizeTransformations();
		const Matrix4 Mcsg = csg->GetFinalTransformMatrix();
		Check( Close( Mcsg._00, Mplain._00, 1e-9 ) && Close( Mcsg._11, Mplain._11, 1e-9 )
			&& Close( Mcsg._22, Mplain._22, 1e-9 ) && Close( Mcsg._03, Mplain._03, 1e-9 ),
			"Test10: (sanity) CSG's own final matrix matches the plain Object's" );

		const Point3 worldOriginCsg = Point3Ops::Transform( Mcsg, objOrigin );
		const Vector3 worldDirCsg = Vector3Ops::Normalize( Vector3Ops::Transform( Mcsg, objDir ) );
		Ray rCsg( worldOriginCsg, worldDirCsg );
		RayIntersection riCsg( rCsg, nullRasterizerState );
		Hit( csg, rCsg, riCsg );

		Check( riCsg.geometric.bHit, "Test10: CSG composite hits the nested mesh" );
		Check( riCsg.geometric.bHasShadingTangent, "Test10: CSG composite bHasShadingTangent set" );
		Check( VecClose( riCsg.geometric.onb.u(), riPlain.geometric.onb.u(), 1e-9 ),
			"Test10: MONEY ASSERTION -- CSG-nested mesh's onb.u() equals the un-nested Object's" );

		CheckOrthonormalRightHanded( riCsg.geometric.onb, "Test10" );

		safe_release( csg );
		safe_release( opA );
		safe_release( opB );
	}

	safe_release( oPlain );
}

// ============================================================
// Test 11 (T2 P1 #1): the design doc's explicitly named gate-1
// obligation -- "The SDFGeometry-heightfield / cartesian_disk pairing
// (Object.cpp:691-694) is re-checked explicitly; it is a bucket-A
// case" -- had NO test anywhere before this.
//
// Builds a heightfield SDF (z = scale*field(u,v) over
// [-R,R]^2, u=(x+R)/2R) and its cartesian_disk_geometry +
// displaced_geometry twin (the SAME formula: RISE_API_
// CreateCartesianDiskGeometry's flat +Z-normal grid, displaced along
// its own normal by scale*field(u,v) -- GeometricUtilities::
// ApplyDisplacementMapToObject) from the SAME field, at the SAME
// (x, y) point, and compares the two independently-derivable
// shading tangents.
//
// Closed-form derivation (field(u,v) = a*u + b*v, so the height
// surface is the exact plane z = gx*x + gy*y + c with
// gx = scale*a/(2R), gy = scale*b/(2R)):
//
//   SDF tangent (legacy world-X projection, Object.cpp's
//   bShadingTangentFromGeometry-without-bHasShadingTangent branch):
//     n = normalize(-gx, -gy, 1)
//     u_sdf = normalize( worldX - n*(n . worldX) )
//
//   Mesh tangent (this fix's real per-triangle dpdu = d/du of the
//   plane, u aligned to x by the cartesian_disk's own UV convention):
//     rawDpdu = (1, 0, gx)            -- already EXACTLY in-plane:
//                                          n . rawDpdu = -gx + gx = 0
//     u_mesh = normalize( rawDpdu )   -- the projection is a no-op
//
//   These two formulas are ALGEBRAICALLY IDENTICAL when gy == 0 (a
//   pure one-axis slope: the world-X projection then has zero
//   y-component too, by inspection of the n . worldX = -gx term and
//   the cross term -gx*gy/|n|^2 vanishing) and MEASURABLY DIVERGE
//   (by that same -gx*gy/|n|^2 y-component) whenever the field has a
//   genuine cross-slope (gy != 0) -- exactly the "differs by design"
//   outcome docs/CLOTH_FABRIC_DESIGN.md 9.1 predicts (the mesh
//   follows its own real per-triangle slope; the SDF's legacy
//   fallback only ever knows about world-X).
// ============================================================
static void CheckSDFCartesianDiskPairing(
	Scalar a, Scalar b, Scalar scale, const char* tag, bool expectExactMatch )
{
	const Scalar R = 2.0;
	const Point3 objOrigin( 0.5, 0.3, 5.0 );
	const Vector3 objDir( 0, 0, -1 );

	// ---- SDF heightfield ----
	LinearGradientFunction2D* fieldSdf = new LinearGradientFunction2D( a, b );
	SDFGeometry* gSdf = new SDFGeometry( fieldSdf, R, scale, 256, 0.0, 64 );
	safe_release( fieldSdf );
	Object* oSdf = new Object( gSdf );
	safe_release( gSdf );
	oSdf->FinalizeTransformations();	// identity

	RayIntersection riSdf( Ray( objOrigin, objDir ), nullRasterizerState );
	Hit( oSdf, Ray( objOrigin, objDir ), riSdf );

	char buf[256];
	std::snprintf( buf, sizeof(buf), "%s: SDF ray hits the heightfield", tag );
	Check( riSdf.geometric.bHit, buf );
	std::snprintf( buf, sizeof(buf), "%s: SDF bShadingTangentFromGeometry set", tag );
	Check( riSdf.geometric.bShadingTangentFromGeometry, buf );
	std::snprintf( buf, sizeof(buf), "%s: SDF bHasShadingTangent NOT set (legacy world-X branch)", tag );
	Check( !riSdf.geometric.bHasShadingTangent, buf );

	// ---- cartesian_disk + displaced_geometry twin ----
	ITriangleMeshGeometryIndexed* pBase = 0;
	const bool baseOk = RISE_API_CreateCartesianDiskGeometry( &pBase, R, /*meshN=*/20 );
	Check( baseOk && pBase != 0, "SDF/mesh pairing: cartesian_disk base built" );

	LinearGradientFunction2D* fieldMesh = new LinearGradientFunction2D( a, b );
	DisplacedGeometry* gDisp = new DisplacedGeometry(
		pBase, /*detail=*/0, fieldMesh, scale,
		/*bDoubleSided=*/false, /*bUseFaceNormals=*/false, /*bSeamFold=*/false );
	safe_release( pBase );
	safe_release( fieldMesh );
	gDisp->Realize();

	Object* oMesh = new Object( gDisp );
	safe_release( gDisp );
	oMesh->FinalizeTransformations();	// identity

	RayIntersection riMesh( Ray( objOrigin, objDir ), nullRasterizerState );
	Hit( oMesh, Ray( objOrigin, objDir ), riMesh );

	std::snprintf( buf, sizeof(buf), "%s: mesh ray hits the displaced disk", tag );
	Check( riMesh.geometric.bHit, buf );
	std::snprintf( buf, sizeof(buf), "%s: mesh bHasShadingTangent set (real UV Jacobian)", tag );
	Check( riMesh.geometric.bHasShadingTangent, buf );

	if( !riSdf.geometric.bHit || !riMesh.geometric.bHit ) {
		safe_release( oSdf );
		safe_release( oMesh );
		return;
	}

	// ---- independent closed-form ground truth ----
	const Scalar gx = scale * a / ( 2.0 * R );
	const Scalar gy = scale * b / ( 2.0 * R );
	const Vector3 n = Vector3Ops::Normalize( Vector3( -gx, -gy, 1.0 ) );
	const Vector3 worldX( 1, 0, 0 );
	const Vector3 expectedSdfU = Vector3Ops::Normalize(
		worldX - n * Vector3Ops::Dot( n, worldX ) );
	const Vector3 rawDpdu( 1.0, 0.0, gx );
	const Vector3 expectedMeshU = Vector3Ops::Normalize( rawDpdu );

	// Sanity: rawDpdu is already exactly in the analytic tangent plane
	// (n . rawDpdu == 0), so the mesh formula's projection step is a
	// no-op -- confirms the derivation above, not just the code.
	std::snprintf( buf, sizeof(buf), "%s: (derivation sanity) rawDpdu already in-plane", tag );
	Check( Close( Vector3Ops::Dot( n, rawDpdu ), 0.0, 1e-9 ), buf );

	// ---- code vs. independent closed form ----
	std::snprintf( buf, sizeof(buf), "%s: SDF onb.u() matches the independent world-X-projection formula", tag );
	Check( VecClose( riSdf.geometric.onb.u(), expectedSdfU, 1e-5 ), buf );
	std::snprintf( buf, sizeof(buf), "%s: mesh onb.u() matches the independent per-triangle-dpdu formula", tag );
	Check( VecClose( riMesh.geometric.onb.u(), expectedMeshU, 1e-6 ), buf );

	// ---- the actual gate-1 pairing question ----
	const Scalar cosAngle = Vector3Ops::Dot( expectedSdfU, expectedMeshU );
	if( expectExactMatch ) {
		std::snprintf( buf, sizeof(buf),
			"%s: MONEY ASSERTION -- pure one-axis slope (gy=0): SDF and mesh tangents AGREE to tolerance", tag );
		Check( VecClose( riSdf.geometric.onb.u(), riMesh.geometric.onb.u(), 1e-4 ), buf );
	} else {
		// Bucket-A finding, quantified rather than assumed: a genuine
		// cross-slope makes the two conventions provably diverge (the
		// -gx*gy/|n|^2 term above is nonzero).  Independently computed
		// for THIS test's exact (a, b, scale) via the same closed form:
		// cosAngle ~= 0.99659 (~4.7 degrees) -- comfortably above both
		// per-vector tolerances used above (1e-5 / 1e-6), so this is a
		// real, measured divergence, not noise; and comfortably a SMALL
		// one (not a wild blow-up), consistent with
		// docs/CLOTH_FABRIC_DESIGN.md 9.1's "differs by design" framing.
		std::snprintf( buf, sizeof(buf),
			"%s: bucket-A finding -- cross-slope makes SDF and mesh tangents MEASURABLY DIVERGE (not noise)", tag );
		Check( cosAngle < 0.999, buf );
		std::snprintf( buf, sizeof(buf),
			"%s: bucket-A finding -- but the divergence stays SMALL for this gentle a cross-slope (~4.7 degrees)", tag );
		Check( cosAngle > 0.99, buf );
	}

	safe_release( oSdf );
	safe_release( oMesh );
}

static void TestSDFHeightfieldCartesianDiskTangentPairing()
{
	std::cout << "SDFGeometry heightfield vs. cartesian_disk + displaced_geometry: gate-1 tangent pairing..." << std::endl;

	// Pure one-axis slope (b=0, gy=0): the two conventions must
	// algebraically coincide -- "agree to a tolerance at matched
	// points", per T2's literal ask.
	CheckSDFCartesianDiskPairing( 0.4, 0.0, 0.6, "Test11(pure-x-slope)", /*expectExactMatch=*/true );

	// Genuine cross-slope (a, b both nonzero): the mesh's real
	// per-triangle dpdu and the SDF's legacy world-X projection
	// measurably diverge -- documented, quantified bucket-A finding.
	// scale=3.0 (not 0.6) so gx=gy=0.3 gives a divergence
	// (~4.7 degrees, cosAngle ~= 0.9966) comfortably clear of this
	// test's own 1e-5/1e-6 per-vector floating-point tolerances --
	// field(u,v)=0.4u+0.4v stays within [0, 0.8], so height =
	// scale*field stays within [0, 2.4], safely inside SDFGeometry's
	// assumed heightfield bbox z-range [0, scale] = [0, 3.0].
	CheckSDFCartesianDiskPairing( 0.4, 0.4, 3.0, "Test11(cross-slope)", /*expectExactMatch=*/false );
}

// ============================================================
int main()
{
	std::cout << "==================================================" << std::endl;
	std::cout << " GeometryShadingTangentTest" << std::endl;
	std::cout << " docs/CLOTH_FABRIC_DESIGN.md section 9.1: the" << std::endl;
	std::cout << " UV-aligned mesh/primitive shading tangent." << std::endl;
	std::cout << "==================================================" << std::endl;

	TestIndexedMeshUVTangent_Money();
	TestIndexedMeshTangentPriority_Money();
	TestIndexedMeshDegenerateUV_ByteIdentical();
	TestNonIndexedMeshUVTangent_Money();
	TestNonIndexedMeshDegenerateUV_ByteIdentical();
	TestMirroredTransform_Mesh();
	TestMirroredTransform_Sphere();
	TestMirroredTransform_Ellipsoid();
	TestMirroredTransform_Cylinder();
	TestMirroredTransform_Torus();
	TestAnalyticPrimitiveTangent();
	TestClippedPlaneTangent_Money();
	TestCsgNestedMeshMatchesUnnested();
	TestSDFHeightfieldCartesianDiskTangentPairing();

	std::cout << "==================================================" << std::endl;
	std::cout << " " << g_pass << " passed, " << g_fail << " failed" << std::endl;
	std::cout << "==================================================" << std::endl;

	return g_fail == 0 ? 0 : 1;
}
