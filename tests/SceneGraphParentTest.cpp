//////////////////////////////////////////////////////////////////////
//
//  SceneGraphParentTest.cpp - docs/agentic-redesign/87-recursive-scene-graph.md
//    step 1: `standard_object` gains a `parent` link and an OPTIONAL
//    `geometry`, derive records the links, and a tail walk bakes
//    `world = parent.world * local` into the flat render list.
//
//  The oracles here are analytic wherever they can be.  A three-level chain
//  with a translation, a scale and a second translation has an exact closed
//  form, and so do the three caches that hang off the composed matrix -- the
//  world-area Jacobian in particular, which is `|det|^(2/3)` and is what an
//  emitter's position pdf divides by.  That case is not decoration: the
//  equivalent bug on a FLAT object (GetArea ignoring the transform's area
//  scaling) shipped and lit a `scale 2` emitter at 0.39x, and hierarchy
//  reintroduces exactly the same hazard one level up.
//
//  Cases:
//    A -- exact composed world matrices down a 3-level chain, a ROTATED
//         parent (which is what pins composition ORDER), and the LOCAL
//         accessor still reporting the authored transform.
//    B -- the three FinalizeTransformations caches under a composed matrix:
//         world area (|det|^(2/3), analytic), inverse-transpose (the world
//         normal against a closed-form ellipsoid normal), bounding box.
//    C -- IDEMPOTENCE.  Re-composing, and re-deriving, must not compound.
//         This is the 86 §3 bug class: that design pushed the parent matrix
//         onto a transform stack that never self-clears, so every re-apply
//         squared in another factor.  87 passes the parent world as an
//         ARGUMENT so a re-compose cannot accumulate.
//    D -- container nodes (no `geometry`): world-invisible, unenumerated,
//         zero area, still composable and still parentable.
//    E -- refusals: forward reference, self-parent, unknown parent, and
//         2-/3-cycles at runtime reparent.
//    G -- a LIVE edit re-composes the whole subtree, not only the node the
//         editor was handed.
//    H -- deleting a `parent` line actually DETACHES on an incremental apply.
//    I -- a CSG operand is refused at both ends of a parent link.
//    J -- the parent-invertibility test: a well-conditioned container at
//         x=1e7 is accepted at every distance decade, a collapsed scale is
//         not, and neither is a COMPOSED singular parent at depth 2.
//    K -- multi-child fan-out (the tree mechanism itself).
//    L -- a WORLD-space delta under a rotated, scaled parent.
//    F -- the editor commits the LOCAL matrix to the CST, so a gizmo edit on
//         a PARENTED object round-trips through a re-derive exactly.  Under
//         86 the analogous commit wrote the composed matrix and squared the
//         group transform on every drag.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <string>
#include <cstdio>
#include <cmath>

#include "../src/Library/Job.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Interfaces/IObjectPriv.h"
#include "../src/Library/Interfaces/IGeometry.h"
#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IEnumCallback.h"
#include "../src/Library/Utilities/BoundingBox.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0, failCount = 0;
static void Check( bool c, const char* n )
{
	if( c ) { ++passCount; }
	else    { ++failCount; std::cout << "  FAIL: " << n << std::endl; }
}

static bool Close( Scalar a, Scalar b, Scalar eps = 1e-9 )
{
	return std::fabs( static_cast<double>( a ) - static_cast<double>( b ) ) <= static_cast<double>( eps );
}

//! Exact (bit-for-bit) matrix equality.  Used only where the contract really is
//! exactness -- idempotence of a re-compose, which must reproduce the same
//! products in the same order.  A tolerance there would hide a slow drift.
static bool Mat4Exact( const Matrix4& a, const Matrix4& b )
{
	const Scalar* pa = &a._00;
	const Scalar* pb = &b._00;
	for( int i = 0; i < 16; ++i ) if( pa[i] != pb[i] ) return false;
	return true;
}

static bool Mat4Close( const Matrix4& a, const Matrix4& b, Scalar eps = 1e-9 )
{
	const Scalar* pa = &a._00;
	const Scalar* pb = &b._00;
	for( int i = 0; i < 16; ++i ) if( !Close( pa[i], pb[i], eps ) ) return false;
	return true;
}

static IObjectPriv* Obj( Job& j, const char* name )
{
	const IScene* s = j.GetScene();
	if( !s ) return 0;
	const IObjectManager* om = s->GetObjects();
	return om ? om->GetItem( name ) : 0;
}

//! Origin of a matrix (its translation column), which for a composed transform
//! is where the node's local origin lands in the world.
static Point3 Origin( const Matrix4& m ) { return Point3( m._30, m._31, m._32 ); }

static void WriteScene( const char* path, const std::string& body )
{
	std::ofstream o( path );
	o << "RISE ASCII SCENE 7\n"
	     "film\n{\nwidth 32\nheight 24\n}\n"
	     "pinhole_camera\n{\nname cam\nlocation 0 0 10\nlookat 0 0 0\n}\n"
	     "uniformcolor_painter\n{\nname p\ncolor 1 1 1\n}\n"
	     "lambertian_material\n{\nname m\nreflectance p\n}\n"
	     "sphere_geometry\n{\nname g\nradius 1\n}\n"
	     // ellipsoid_geometry is the unit-sphere stand-in that answers
	     // ComputeAnalyticalDerivatives (sphere_geometry does not), which case
	     // B2 needs to read a world normal and its world tangents.
	     "ellipsoid_geometry\n{\nname ge\nradii 1 1 1\n}\n"
	  << body;
}

//! Counts the objects the world-visible enumeration actually hands out.  This
//! is the SAME callback surface the nine render-side consumers use (area
//! lights, both light-sampler scans, the SMS caster scan, the auto-rasterizer's
//! integrator choice, BDPT setup, the two extent scans, the editor), so a
//! container leaking into it here would leak into all of them.
struct CountVisible : public IEnumCallback<IObject>
{
	int n = 0;
	bool operator()( const IObject& ) override { ++n; return true; }
};

int main()
{
	std::cout << "SceneGraphParentTest" << std::endl;
	using Cat = SceneEditController::Category;

	// =================================================================
	// A -- exact composed world transforms, and LOCAL vs WORLD.
	//
	//   root   container, position 10 0 0
	//   mid    parent root,  position 0 5 0, scale 2 2 2
	//   leaf   parent mid,   position 1 0 0
	//
	// Closed form (local = P * O * Stretch * Scale; world = parent.world * local):
	//   root.world = T(10,0,0)
	//   mid.world  = T(10,0,0) * T(0,5,0) * S(2)          -> origin (10,5,0), linear 2I
	//   leaf.world = mid.world * T(1,0,0)                 -> origin (12,5,0), linear 2I
	//
	// The leaf's origin is the load-bearing number: 12, not 11.  Getting 11
	// means the parent's SCALE was not applied to the child's translation,
	// i.e. the composition was done additively or in the wrong order.
	// =================================================================
	const char* sA = "sg_parent_chain.RISEscene";
	{
		WriteScene( sA,
			"standard_object\n{\nname root\nposition 10 0 0\n}\n"
			"standard_object\n{\nname mid\nparent root\ngeometry g\nmaterial m\nposition 0 5 0\nscale 2 2 2\n}\n"
			"standard_object\n{\nname leaf\nparent mid\ngeometry g\nmaterial m\nposition 1 0 0\n}\n" );

		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sA ), "A: parented scene loads" );

		IObjectPriv* root = Obj( *j, "root" );
		IObjectPriv* mid  = Obj( *j, "mid" );
		IObjectPriv* leaf = Obj( *j, "leaf" );
		Check( root && mid && leaf, "A: all three nodes registered" );

		if( root && mid && leaf ) {
			const Matrix4 rootW = root->GetFinalTransformMatrix();
			const Matrix4 midW  = mid->GetFinalTransformMatrix();
			const Matrix4 leafW = leaf->GetFinalTransformMatrix();

			Check( Close( Origin( rootW ).x, 10 ) && Close( Origin( rootW ).y, 0 ) && Close( Origin( rootW ).z, 0 ),
			       "A1: root world origin is (10,0,0)" );

			Check( Close( Origin( midW ).x, 10 ) && Close( Origin( midW ).y, 5 ) && Close( Origin( midW ).z, 0 ),
			       "A2: mid world origin is (10,5,0) -- parent translation composed" );
			Check( Close( midW._00, 2 ) && Close( midW._11, 2 ) && Close( midW._22, 2 ),
			       "A2: mid world linear part is 2I" );

			Check( Close( Origin( leafW ).x, 12 ) && Close( Origin( leafW ).y, 5 ) && Close( Origin( leafW ).z, 0 ),
			       "A3: leaf world origin is (12,5,0) -- the PARENT'S SCALE acts on the child's translation" );
			Check( Close( leafW._00, 2 ) && Close( leafW._11, 2 ) && Close( leafW._22, 2 ),
			       "A3: leaf world linear part inherits the parent scale" );

			// The LOCAL accessor must still report what the chunk authored.
			const Matrix4 leafL = leaf->GetLocalTransformMatrix();
			Check( Close( Origin( leafL ).x, 1 ) && Close( Origin( leafL ).y, 0 ) && Close( Origin( leafL ).z, 0 ),
			       "A4: leaf LOCAL origin is its authored (1,0,0), not the composed (12,5,0)" );
			Check( Close( leafL._00, 1 ) && Close( leafL._11, 1 ) && Close( leafL._22, 1 ),
			       "A4: leaf LOCAL linear part carries no parent scale" );
			Check( Mat4Close( leaf->GetParentWorldTransformMatrix(), midW ),
			       "A4: leaf's recorded parent world IS mid's world matrix" );
			Check( Mat4Close( root->GetLocalTransformMatrix(), rootW ),
			       "A4: an unparented node's LOCAL and WORLD matrices are identical" );

			// =========================================================
			// B -- the caches FinalizeTransformations hangs off the
			//      composed matrix.
			//
			// B1 (world area).  The leaf is a unit sphere: object area 4*pi.
			// Its composed linear part is 2I, det = 8, so the world-area
			// factor |det|^(2/3) = 4 and GetArea() must be 16*pi.  If the
			// area scale were computed from the LOCAL matrix (det 1) it
			// would report 4*pi -- a 4x error straight into every
			// pdfPosition = 1/GetArea() consumer (NEE, BDPT/VCM InitLight,
			// photon power normalization, SSS dipole sampling).
			// =========================================================
			const Scalar kPi = Scalar( 3.14159265358979323846 );
			Check( Close( leaf->GetArea(), 16 * kPi, 1e-6 ),
			       "B1: leaf world area is 16*pi -- |det(COMPOSED)|^(2/3), not the local 4*pi" );
			Check( Close( mid->GetArea(), 16 * kPi, 1e-6 ),
			       "B1: mid world area is 16*pi (its own scale 2)" );

			// B3 (bounding box).  A unit sphere scaled 2 about world (12,5,0)
			// spans [10,14] x [3,7] x [-2,2].
			const BoundingBox bb = leaf->getBoundingBox();
			Check( Close( bb.ll.x, 10, 1e-6 ) && Close( bb.ur.x, 14, 1e-6 )
			    && Close( bb.ll.y,  3, 1e-6 ) && Close( bb.ur.y,  7, 1e-6 ),
			       "B3: leaf world bounding box reflects the composed transform" );

			// =========================================================
			// C -- IDEMPOTENCE (the 86 §3 bug class).
			// =========================================================
			Matrix4 before[3] = { rootW, midW, leafW };
			for( int k = 0; k < 5; ++k ) j->ComposeObjectHierarchy();
			Check( Mat4Exact( root->GetFinalTransformMatrix(), before[0] )
			    && Mat4Exact( mid->GetFinalTransformMatrix(),  before[1] )
			    && Mat4Exact( leaf->GetFinalTransformMatrix(), before[2] ),
			       "C1: five extra composes change NOTHING (composition is an argument, not a stack push)" );
			Check( !j->ComposeObjectHierarchy(),
			       "C1: a no-op compose reports 'nothing moved' (the incremental apply's spatial gate)" );

			// A full re-derive of the retained Document must reproduce the same
			// world matrices.  Under 86 this is where the squaring showed up.
			Check( j->RederiveCstWithVariant( "none" ), "C2: re-derive succeeds" );
			IObjectPriv* leaf2 = Obj( *j, "leaf" );
			Check( leaf2 && Mat4Close( leaf2->GetFinalTransformMatrix(), before[2] ),
			       "C2: re-derive reproduces the SAME leaf world matrix (no compounding)" );
		}
		j->release();
	}

	// =================================================================
	// A5 -- a ROTATED parent.  Every other case here uses a translation-only
	// or axis-aligned-scale parent, and those cannot distinguish a whole family
	// of composition mistakes: composing the parent's rotation on the wrong
	// side of the child's scale, transposing the linear part, or rotating the
	// child's translation but not its basis.  With `spin` at 90 degrees about
	// Z and a child one unit out along +X:
	//   child world origin = Rz(90) * (1,0,0) = (0,1,0)
	//   child world X basis = Rz(90) * (1,0,0) = (0,1,0)
	//   child world Y basis = Rz(90) * (0,1,0) = (-1,0,0)
	// =================================================================
	const char* sRot = "sg_parent_rotated.RISEscene";
	{
		WriteScene( sRot,
			"standard_object\n{\nname spin\norientation 0 0 90\n}\n"
			"standard_object\n{\nname flag\nparent spin\ngeometry g\nmaterial m\nposition 1 0 0\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sRot ), "A5: rotated-parent scene loads" );
		IObjectPriv* flag = Obj( *j, "flag" );
		Check( flag != 0, "A5: child registered" );
		if( flag ) {
			const Matrix4 w = flag->GetFinalTransformMatrix();
			Check( Close( Origin( w ).x, 0, 1e-12 ) && Close( Origin( w ).y, 1, 1e-12 )
			    && Close( Origin( w ).z, 0, 1e-12 ),
			       "A5: a 90-degree parent rotation carries the child's +X offset onto world +Y" );
			// Column 0 of the world matrix is the image of the local +X axis.
			Check( Close( w._00, 0, 1e-12 ) && Close( w._01, 1, 1e-12 ) && Close( w._02, 0, 1e-12 ),
			       "A5: the child's BASIS is rotated too, not just its origin" );
			Check( Close( w._10, -1, 1e-12 ) && Close( w._11, 0, 1e-12 ) && Close( w._12, 0, 1e-12 ),
			       "A5: the child's +Y basis lands on world -X" );
			Check( Close( flag->GetLocalTransformMatrix()._00, 1, 1e-12 ),
			       "A5: the child's own LOCAL basis is still unrotated" );
		}
		j->release();
		std::remove( sRot );
	}

	// =================================================================
	// D -- container nodes.
	// =================================================================
	{
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sA ), "D: parented scene loads" );
		IObjectPriv* root = Obj( *j, "root" );
		Check( root != 0, "D: the geometry-less container node IS registered as an object" );
		if( root ) {
			Check( root->GetGeometry() == 0, "D1: a container carries no geometry" );
			Check( !root->IsWorldVisible(),  "D1: a container is world-INVISIBLE" );
			Check( Close( root->GetArea(), 0 ), "D1: a container has zero area (never a luminaire)" );
		}
		CountVisible cv;
		const IScene* sc = j->GetScene();
		if( sc && sc->GetObjects() ) sc->GetObjects()->EnumerateObjects( cv );
		Check( cv.n == 2,
		       "D2: the world-visible enumeration hands out the 2 real shapes and NOT the container" );
		j->release();
	}
	std::remove( sA );

	// =================================================================
	// B2 -- inverse-transpose under a NON-UNIFORMLY scaled parent.
	//
	// A normal is transformed by (M^-1)^T, not by M, so it stays perpendicular
	// to the surface under anisotropic scaling.  With the parent scaling x by
	// 3, a unit-sphere child's world surface is the ellipsoid
	// (px/3)^2 + py^2 + pz^2 = 1, whose outward normal at a surface point p is
	// exactly normalize( px/9, py, pz ).  THAT is the oracle: a closed form
	// that mentions only the world POSITION, and therefore cannot be satisfied
	// by an inverse-transpose built from the wrong matrix.
	//
	// NOT an oracle, and the reason this comment is long: "the world normal is
	// perpendicular to the world tangents" is an ALGEBRAIC IDENTITY here, not
	// a test.  ComputeAnalyticalDerivatives transforms the tangents by M and
	// the normal by (M^-1)^T, and
	//     ((M^-1)^T n) . (M t) = n^T M^-1 M t = n . t = 0
	// for ANY invertible M -- the composed one, the un-composed local one, or
	// any other.  A perpendicularity check here is self-consistent by
	// construction and discriminates nothing.  An earlier revision of this
	// file asserted exactly that and claimed it caught the local-matrix bug;
	// it did not, and the step-1 commit message repeated the claim.
	// =================================================================
	const char* sB = "sg_parent_aniso.RISEscene";
	{
		WriteScene( sB,
			"standard_object\n{\nname squash\nscale 3 1 1\n}\n"
			"standard_object\n{\nname ball\nparent squash\ngeometry ge\nmaterial m\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sB ), "B2: anisotropic-parent scene loads" );
		IObjectPriv* ball = Obj( *j, "ball" );
		Check( ball != 0, "B2: child registered" );
		if( ball ) {
			int evaluated = 0;
			bool allMatchAnalytic = true, allUnit = true, anyDiscriminating = false;
			Scalar worstErr = 0;
			const Point2 uvs[4] = { Point2( 0.17, 0.31 ), Point2( 0.42, 0.63 ),
			                        Point2( 0.71, 0.22 ), Point2( 0.88, 0.79 ) };
			for( int i = 0; i < 4; ++i ) {
				Point3 wp; Vector3 wn, dpdu, dpdv, dndu, dndv;
				if( !ball->ComputeAnalyticalDerivatives( uvs[i], 0, wp, wn, dpdu, dpdv, dndu, dndv ) ) continue;
				++evaluated;
				if( !Close( Vector3Ops::Magnitude( wn ), 1, 1e-7 ) ) allUnit = false;

				// Closed-form ellipsoid normal at the RETURNED world point.
				const Vector3 expect = Vector3Ops::Normalize(
					Vector3( wp.x / Scalar( 9 ), wp.y, wp.z ) );
				// The inward/outward sense is the geometry's business, not this
				// test's -- compare up to sign.
				const Scalar d = Scalar( std::fabs( static_cast<double>( Vector3Ops::Dot( wn, expect ) ) ) );
				if( 1 - d > worstErr ) worstErr = 1 - d;
				if( !Close( d, 1, 1e-7 ) ) allMatchAnalytic = false;

				// Guard against a degenerate sample set: on the x=0 or yz=0
				// axes the WRONG (local-matrix) normal coincides with the right
				// one, so at least one sample must be off-axis enough to tell
				// them apart.  normalize(px/3,py,pz) is what the wrong answer
				// would be; require it to differ measurably somewhere.
				const Vector3 wrong = Vector3Ops::Normalize(
					Vector3( wp.x / Scalar( 3 ), wp.y, wp.z ) );
				if( std::fabs( static_cast<double>( Vector3Ops::Dot( expect, wrong ) ) ) < 0.999 ) {
					anyDiscriminating = true;
				}
			}
			Check( evaluated == 4, "B2: the ellipsoid answered all four analytical-derivative queries" );
			Check( allUnit, "B2: the world normal is unit length" );
			Check( anyDiscriminating,
			       "B2: at least one sample is off-axis enough that a local-matrix normal would differ "
			       "(otherwise the next assertion would be vacuous)" );
			if( !allMatchAnalytic ) {
				std::cout << "    (B2 worst |1 - dot(n, analytic)| = "
				          << static_cast<double>( worstErr ) << ")" << std::endl;
			}
			Check( allMatchAnalytic,
			       "B2: the world normal equals the closed-form ellipsoid normal normalize(px/9, py, pz) -- "
			       "the inverse-transpose is built from the COMPOSED matrix, not the local one" );

			// Area under a non-uniform parent.  The composed determinant is 3
			// (the child is identity, the parent scales x by 3), so the world
			// area must be the OBJECT-space area times |det|^(2/3) = 3^(2/3).
			// Taken against the geometry's own reported area rather than a
			// hardcoded 4*pi, so this pins the JACOBIAN -- the thing hierarchy
			// can get wrong -- and not the ellipsoid area formula.
			const Scalar objArea = ball->GetGeometry() ? ball->GetGeometry()->GetArea() : Scalar( 0 );
			Check( objArea > 0, "B2: the geometry reports a positive object-space area" );
			Check( objArea > 0 && Close( ball->GetArea(), objArea * std::pow( 3.0, 2.0 / 3.0 ), 1e-6 ),
			       "B2: world area = object area * |det(COMPOSED)|^(2/3)" );
		}
		j->release();
	}
	std::remove( sB );

	// =================================================================
	// E -- refusals.  Each must leave the scene UNCHANGED, not half-linked.
	// =================================================================
	{
		// E1: forward reference.  `parent` resolves by immediate manager
		// lookup, exactly like standard_shader's `shaderop`, so naming a
		// not-yet-declared object must fail the load rather than silently
		// dropping the link.  This is also what makes a parse-time cycle
		// impossible: a cycle needs a link back to something undeclared.
		const char* sF = "sg_parent_forward.RISEscene";
		WriteScene( sF,
			"standard_object\n{\nname child\nparent later\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname later\nposition 1 0 0\n}\n" );
		Job* j = new Job();
		Check( !j->LoadAsciiSceneViaCst( sF ),
		       "E1: a FORWARD `parent` reference is refused (declare-before-use)" );
		j->release();
		std::remove( sF );
	}
	{
		const char* sS = "sg_parent_self.RISEscene";
		WriteScene( sS, "standard_object\n{\nname me\nparent me\ngeometry g\nmaterial m\n}\n" );
		Job* j = new Job();
		Check( !j->LoadAsciiSceneViaCst( sS ), "E2: an object cannot be its own parent" );
		j->release();
		std::remove( sS );
	}
	{
		// E3/E4: runtime reparent guards.  These are the ones parse-order
		// cannot cover, because both endpoints already exist.
		const char* sC = "sg_parent_cycle.RISEscene";
		WriteScene( sC,
			"standard_object\n{\nname a\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname b\nparent a\ngeometry g\nmaterial m\nposition 1 0 0\n}\n"
			"standard_object\n{\nname c\nparent b\ngeometry g\nmaterial m\nposition 1 0 0\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sC ), "E3: a->b->c chain loads" );

		Check( !j->SetObjectParent( "a", "b" ),
		       "E3: parenting a to its own CHILD is refused (2-cycle)" );
		Check( !j->SetObjectParent( "a", "c" ),
		       "E3: parenting a to its own GRANDCHILD is refused (3-cycle)" );
		Check( !j->SetObjectParent( "a", "nosuch" ),
		       "E4: parenting to an unregistered object is refused" );
		Check( !j->SetObjectParent( "nosuch", "a" ),
		       "E4: parenting an unregistered object is refused" );

		// A refused reparent must not have half-applied: c still hangs off b,
		// so it still sits at (2,0,0), and a further compose is a no-op.
		IObjectPriv* c = Obj( *j, "c" );
		Check( c && Close( Origin( c->GetFinalTransformMatrix() ).x, 2 ),
		       "E3: the refused reparents left the chain intact (c is still at x=2)" );
		Check( !j->ComposeObjectHierarchy(),
		       "E3: nothing moved -- the refusals changed no link" );

		// A LEGAL reparent works, and detaching restores the root pose.
		Check( j->SetObjectParent( "c", "a" ), "E3: reparenting c under a is allowed" );
		Check( j->ComposeObjectHierarchy(), "E3: the legal reparent moved something" );
		Check( c && Close( Origin( c->GetFinalTransformMatrix() ).x, 1 ),
		       "E3: c now composes against a, so it sits at x=1" );
		Check( j->SetObjectParent( "c", 0 ), "E3: detaching c is allowed" );
		j->ComposeObjectHierarchy();
		Check( c && Close( Origin( c->GetFinalTransformMatrix() ).x, 1 ),
		       "E3: a detached c falls back to its own local transform" );
		j->release();
		std::remove( sC );
	}
	{
		// E5: removing a PARENT re-roots its children.  Left to dangle, the
		// link would make every later compose warn, and the child's world
		// transform would depend on a node that no longer exists.
		const char* sR = "sg_parent_remove.RISEscene";
		WriteScene( sR,
			"standard_object\n{\nname base\nposition 7 0 0\n}\n"
			"standard_object\n{\nname perch\nparent base\ngeometry g\nmaterial m\nposition 0 2 0\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sR ), "E5: scene loads" );
		IObjectPriv* perch = Obj( *j, "perch" );
		Check( perch && Close( Origin( perch->GetFinalTransformMatrix() ).x, 7 ),
		       "E5: perch starts composed at x=7" );
		Check( j->RemoveObject( "base" ), "E5: the parent is removed" );
		// RemoveObject alone must leave the orphan CORRECT -- no explicit
		// compose.  Nothing else would do it on this path: Job::RemoveObject
		// does not compose, and the per-frame re-bake is 87 step 2.  The
		// `remove object` console command reaches exactly here, so an orphan
		// left holding its old composed pose would render there indefinitely.
		perch = Obj( *j, "perch" );
		Check( perch && Close( Origin( perch->GetFinalTransformMatrix() ).x, 0 )
		             && Close( Origin( perch->GetFinalTransformMatrix() ).y, 2 ),
		       "E5: the orphan is re-rooted onto its own local transform (0,2,0) BY THE REMOVAL ITSELF" );
		Check( !j->ComposeObjectHierarchy(),
		       "E5: and a compose afterwards finds nothing left to move" );
		// The link must be RETIRED, not left dangling: a dangling link makes
		// every later compose take the fallback path and warn, and keeps the
		// authored graph describing an object that no longer exists.
		const IScene* scR = j->GetScene();
		const IObjectManager* omR = scR ? scR->GetObjects() : 0;
		Check( omR && std::string( omR->GetObjectParent( "perch" ) ).empty(),
		       "E5: the orphan's parent link is RETIRED, not left dangling at the removed name" );
		j->release();
		std::remove( sR );
	}

	// =================================================================
	// F -- the editor commits the LOCAL matrix.
	//
	// Move a PARENTED child through the transform panel, then force a full
	// re-derive of the retained Document.  If the commit wrote the composed
	// WORLD matrix, the re-derive would apply the parent's transform a
	// SECOND time and the child would jump.  86 §3 records exactly this
	// failure (there it compounded without bound, because that design
	// composed onto a transform stack that never self-clears).
	// =================================================================
	{
		const char* sE = "sg_parent_commit.RISEscene";
		WriteScene( sE,
			"scene_variant\n{\nname night\n}\n"
			"standard_object\n{\nname hub\nposition 4 0 0\n}\n"
			"standard_object\n{\nname arm\nparent hub\ngeometry g\nmaterial m\nposition 0 1 0\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sE ), "F: parented scene with a variant loads" );
		IObjectPriv* arm = Obj( *j, "arm" );
		Check( arm && Close( Origin( arm->GetFinalTransformMatrix() ).x, 4 )
		           && Close( Origin( arm->GetFinalTransformMatrix() ).y, 1 ),
		       "F: arm starts composed at (4,1,0)" );

		SceneEditController c( *j, 0 );
		c.SetSelection( Cat::Object, String( "arm" ) );
		// The panel edits the chunk's own `position` param, so it is a LOCAL
		// absolute setter: (0,3,0) local under a hub at x=4 -> world (4,3,0).
		Check( c.SetPropertyForCategory( Cat::Object, String( "position" ), String( "0 3 0" ) ),
		       "F: panel position edit applies" );
		arm = Obj( *j, "arm" );
		Matrix4 moved = arm ? arm->GetFinalTransformMatrix() : Matrix4Ops::Identity();
		Check( arm && Close( Origin( moved ).x, 4 ) && Close( Origin( moved ).y, 3 ),
		       "F: after the LOCAL position edit the arm is at world (4,3,0)" );

		// Force the commit + a full re-derive by editing a material.
		c.SetSelection( Cat::Material, String( "m" ) );
		Check( c.SetPropertyForCategory( Cat::Material, String( "reflectance" ), String( "p" ) ),
		       "F: material edit applies (forces a full re-derive)" );
		Check( j->RederiveCstWithVariant( "none" ), "F: re-derive succeeds" );
		IObjectPriv* arm2 = Obj( *j, "arm" );
		Check( arm2 && Mat4Close( arm2->GetFinalTransformMatrix(), moved, 1e-9 ),
		       "F: the arm's world transform SURVIVES the re-derive unchanged -- the commit wrote the "
		       "LOCAL matrix, so the parent was not applied twice" );
		j->release();
		std::remove( sE );
	}

	// =================================================================
	// G -- a LIVE edit re-composes the SUBTREE, not just the edited node.
	//
	// SceneEditor::RunObjectInvariantChain finalizes the object it was handed.
	// Under hierarchy that is not enough: every descendant is still holding the
	// parent world it was last composed against, plus the three caches derived
	// from it -- and the same function then invalidates the TLAS, which would
	// rebuild from those stale boxes.
	//
	// Built through the API, NOT loaded from a scene file, deliberately: with a
	// retained CST Document an object transform edit commits and re-derives at
	// the edit boundary, and the re-derive's own compose would mask the bug.
	// The un-masked case is real and reachable -- a mid-drag gizmo frame, an
	// API/Blender/PRISE host, or any scene whose commit is refused -- and it is
	// the one where the subtree stays detached for the rest of the session.
	// =================================================================
	{
		Job* j = new Job();
		const double white[3] = { 1, 1, 1 };
		Check( j->AddUniformColorPainter( "p", white, "sRGB" ), "G: painter added" );
		Check( j->AddLambertianMaterial( "m", "p" ), "G: material added" );
		Check( j->AddSphereGeometry( "g", 1.0 ), "G: geometry added" );

		const double zero[3] = { 0, 0, 0 };
		const double one[3]  = { 1, 1, 1 };
		const double up[3]   = { 0, 1, 0 };
		RadianceMapConfig noMap;
		Check( j->AddObject( "hub2", 0, 0, 0, 0, noMap, zero, zero, one, true, true ),
		       "G: container added through the API (null geometry)" );
		Check( j->AddObject( "kid", "g", "m", 0, 0, noMap, up, zero, one, true, true ),
		       "G: child added" );
		Check( j->SetObjectParent( "kid", "hub2" ), "G: link recorded" );
		j->ComposeObjectHierarchy();

		IObjectPriv* kid = Obj( *j, "kid" );
		Check( kid && Close( Origin( kid->GetFinalTransformMatrix() ).x, 0 )
		           && Close( Origin( kid->GetFinalTransformMatrix() ).y, 1 ),
		       "G: kid starts at world (0,1,0)" );

		SceneEditController c( *j, 0 );
		c.SetSelection( Cat::Object, String( "hub2" ) );
		Check( c.SetPropertyForCategory( Cat::Object, String( "position" ), String( "6 0 0" ) ),
		       "G: the PARENT's position edit applies" );
		kid = Obj( *j, "kid" );
		Check( kid && Close( Origin( kid->GetFinalTransformMatrix() ).x, 6 )
		           && Close( Origin( kid->GetFinalTransformMatrix() ).y, 1 ),
		       "G: the CHILD followed its parent to world (6,1,0) -- the live edit re-composed the subtree" );
		j->release();
	}

	// =================================================================
	// H -- REMOVING the `parent` line detaches.  The object chunk's Finalize
	// runs again on an incremental re-apply, and if it only calls
	// SetObjectParent when a parent is PRESENT, the old link survives an edit
	// that deleted it: the object keeps rendering under a parent its own chunk
	// no longer names, until a save and reload silently move it.
	// =================================================================
	{
		const char* sH = "sg_parent_detach.RISEscene";
		WriteScene( sH,
			"standard_object\n{\nname anchor\nposition 9 0 0\n}\n"
			"standard_object\n{\nname hanger\nparent anchor\ngeometry g\nmaterial m\nposition 0 2 0\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sH ), "H: scene loads" );
		IObjectPriv* hanger = Obj( *j, "hanger" );
		Check( hanger && Close( Origin( hanger->GetFinalTransformMatrix() ).x, 9 ),
		       "H: hanger starts composed at x=9" );

		// Remove the `parent` param through the ordinary incremental edit path.
		// The 0/1/2/3 contract: 1 = INCREMENTAL apply, 2 = full re-derive (clean),
		// 3 = full re-derive that DIAGNOSED (a failure), 0 = refused.  `!= 0`
		// would accept 3, and would also let a regression that pushed this edit
		// onto the full-re-derive fallback pass silently -- and a full re-derive
		// builds a fresh ObjectManager, which drops every link for free and
		// makes the detach assertions below vacuous.  This case exists to pin
		// the INCREMENTAL detach, so it pins exactly that code.
		const int rc = j->ApplyCstParamRemoveChecked( "hanger", "standard_object", "parent", 0 );
		Check( rc == 1, "H: the `parent` param removal goes through the INCREMENTAL apply (rc == 1)" );
		hanger = Obj( *j, "hanger" );
		Check( hanger && Close( Origin( hanger->GetFinalTransformMatrix() ).x, 0 )
		              && Close( Origin( hanger->GetFinalTransformMatrix() ).y, 2 ),
		       "H: removing the `parent` line DETACHES -- the object falls back to its own (0,2,0)" );
		const IScene* scH = j->GetScene();
		const IObjectManager* omH = scH ? scH->GetObjects() : 0;
		Check( omH && std::string( omH->GetObjectParent( "hanger" ) ).empty(),
		       "H: and the authored graph no longer records the link" );
		j->release();
		std::remove( sH );
	}

	// =================================================================
	// I -- a CSG OPERAND is not a scene-graph node.  Its transform is
	// interpreted in its csg_object's frame, not the world's, so parenting in
	// either direction would place things in a frame nobody asked for.
	// =================================================================
	{
		const char* sI = "sg_parent_csg.RISEscene";
		WriteScene( sI,
			"standard_object\n{\nname opA\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname opB\ngeometry g\nmaterial m\nposition 0.5 0 0\n}\n"
			"csg_object\n{\nname cut\nobja opA\nobjb opB\noperation union\nmaterial m\n}\n"
			"standard_object\n{\nname perch2\nposition 3 0 0\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sI ), "I: csg scene loads" );
		Check( !j->SetObjectParent( "opA", "perch2" ),
		       "I: a CSG operand cannot take a parent" );
		Check( !j->SetObjectParent( "perch2", "opB" ),
		       "I: nothing can be parented TO a CSG operand" );
		// The csg_object ITSELF is a world-visible node and IS parentable.
		Check( j->SetObjectParent( "cut", "perch2" ),
		       "I: the csg_object itself is an ordinary scene-graph node" );
		// And a container is still refused as a CSG operand -- but "no
		// geometry" alone must NOT be the test, because a CSGObject has none
		// either and NESTED CSG (an inner composite as an operand) is a
		// supported construction the corpus actually uses.
		const IScene* scI = j->GetScene();
		const IObjectManager* omI = scI ? scI->GetObjects() : 0;
		Check( omI && omI->GetItem( "nested" ) == 0, "I: `nested` not yet present" );
		RadianceMapConfig noMapI;
		const double zeroI[3] = { 0, 0, 0 };
		Check( j->AddCSGObject( "nested", "cut", "opA", 0, "m", 0, 0, noMapI, zeroI, zeroI, true, true ),
		       "I: a csg_object may take another csg_object as an operand (nested CSG still works)" );
		Check( !j->AddCSGObject( "bad", "perch2", "opA", 0, "m", 0, 0, noMapI, zeroI, zeroI, true, true ),
		       "I: a CONTAINER is refused as a CSG operand (it is a transform, not a shape)" );
		j->release();
		std::remove( sI );
	}

	// =================================================================
	// J -- the parent-invertibility test must be SCALE-INVARIANT.
	//
	// It exists to catch the one documented failure mode of Matrix4Ops::Inverse
	// (it returns its INPUT at zero determinant), and it decides whether a
	// world-space editor op can be expressed in the node's local frame.  An
	// ABSOLUTE epsilon on |P*P^-1 - I| gets it wrong in both directions,
	// because the residual in an affine matrix's translation column grows like
	// ||t||*eps: a perfectly well-conditioned container far from the origin
	// would be declared singular (and its children's world-space edits then
	// composed with an extra parent factor -- a teleport), while a tiny
	// near-singular linear part would sail through.  The committed test is the
	// componentwise backward error, which is exactly scale-invariant.
	// =================================================================
	{
		// Swept over five decades rather than pinned to one distance: the
		// property under test is SCALE-INVARIANCE, and any single distance is
		// just a threshold this machine's arithmetic happens to sit under.
		const char* kDistances[5] = { "1000", "100000", "10000000", "1000000000", "100000000000" };
		for( int d = 0; d < 5; ++d ) {
			const char* sJ = "sg_parent_farfield.RISEscene";
			WriteScene( sJ,
				std::string( "standard_object\n{\nname faraway\nposition " ) + kDistances[d] +
				" 0 0\norientation 0 0 40\n}\n"
				"standard_object\n{\nname speck\nparent faraway\ngeometry g\nmaterial m\nposition 0 1 0\n}\n" );
			Job* j = new Job();
			const bool loaded = j->LoadAsciiSceneViaCst( sJ );
			IObjectPriv* speck = loaded ? Obj( *j, "speck" ) : 0;
			if( !( speck && speck->IsParentWorldInvertible() ) ) {
				std::cout << "    (J: rejected at distance " << kDistances[d] << ")" << std::endl;
			}
			Check( speck && speck->IsParentWorldInvertible(),
			       "J: a rotated container remains INVERTIBLE at every distance decade "
			       "(an absolute residual epsilon rejects the far ones)" );
			j->release();
			std::remove( sJ );
		}
	}
	{
		// DEPTH-2 degeneracy.  A flattened GRANDPARENT composes into a
		// grandchild's parent world as a matrix whose true determinant is zero
		// but whose COMPUTED determinant is a rounding residue (~1e-17), not
		// exactly 0 -- so Matrix4Ops::Inverse does not return its input, it
		// returns an adjugate/det "inverse" with entries ~1e16.  A residual or
		// backward-error test accepts that (the backward error of a garbage
		// inverse is small by construction); a conditioning test does not.
		// This is the depth hierarchy ADDS, so a guard that only works at
		// depth 1 is a guard that only works on the scenes 87 did not enable.
		const char* sL = "sg_parent_deep_degenerate.RISEscene";
		WriteScene( sL,
			"standard_object\n{\nname flat_gp\norientation -94 16 -47\nposition 1 1.26 -4.3\nscale 0 1 1\n}\n"
			"standard_object\n{\nname mid_p\nparent flat_gp\norientation 147 -11 18\nposition -3 2.2 0.4\n"
			"scale 1.739 1.312 2.611\n}\n"
			"standard_object\n{\nname grandkid\nparent mid_p\ngeometry g\nmaterial m\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sL ), "J: depth-2 degenerate scene loads" );
		IObjectPriv* gk = Obj( *j, "grandkid" );
		Check( gk && !gk->IsParentWorldInvertible(),
		       "J: a COMPOSED singular parent is caught at depth 2, where its determinant is a rounding "
		       "residue rather than exactly zero" );
		j->release();
		std::remove( sL );
	}
	{
		// The genuinely degenerate case must still be caught.
		const char* sK = "sg_parent_degenerate.RISEscene";
		WriteScene( sK,
			"standard_object\n{\nname flattened\nscale 0 1 1\n}\n"
			"standard_object\n{\nname onit\nparent flattened\ngeometry g\nmaterial m\nposition 0 1 0\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sK ), "J: degenerate-parent scene loads" );
		IObjectPriv* onit = Obj( *j, "onit" );
		Check( onit && !onit->IsParentWorldInvertible(),
		       "J: a collapsed parent scale IS reported non-invertible" );
		Check( onit && Close( onit->GetArea(), 0 ),
		       "J: and the child's world area collapses to zero, so it is never area-sampled" );
		j->release();
		std::remove( sK );
	}

	// =================================================================
	// K -- MULTI-CHILD FAN-OUT.  Every other case in this file gives each node
	// at most ONE child, which means the actual tree mechanism -- the
	// per-parent child list, its ordering, and the walk that descends into all
	// of it -- has no assertion behind it.  An implementation that composed
	// only childrenOf[parent][0] would pass every one of them.  This is the
	// thing 87 adds over 86, so it gets its own case.
	// =================================================================
	{
		const char* sM = "sg_parent_fanout.RISEscene";
		WriteScene( sM,
			"standard_object\n{\nname trunk\nposition 0 5 0\nscale 2 2 2\n}\n"
			"standard_object\n{\nname limb_a\nparent trunk\ngeometry g\nmaterial m\nposition 1 0 0\n}\n"
			"standard_object\n{\nname limb_b\nparent trunk\ngeometry g\nmaterial m\nposition 0 1 0\n}\n"
			"standard_object\n{\nname limb_c\nparent trunk\ngeometry g\nmaterial m\nposition 0 0 1\n}\n"
			"standard_object\n{\nname twig\nparent limb_c\ngeometry g\nmaterial m\nposition 0 0 1\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sM ), "K: fan-out scene loads" );
		IObjectPriv* a1 = Obj( *j, "limb_a" );
		IObjectPriv* b1 = Obj( *j, "limb_b" );
		IObjectPriv* c1 = Obj( *j, "limb_c" );
		IObjectPriv* tw = Obj( *j, "twig" );
		Check( a1 && b1 && c1 && tw, "K: all four descendants registered" );
		if( a1 && b1 && c1 && tw ) {
			// trunk.world = T(0,5,0)*S(2).  Each limb is one unit out along a
			// different local axis, so each lands 2 units out along that axis.
			Check( Close( Origin( a1->GetFinalTransformMatrix() ).x, 2 )
			    && Close( Origin( a1->GetFinalTransformMatrix() ).y, 5 ),
			       "K: sibling 1 of 3 composed" );
			Check( Close( Origin( b1->GetFinalTransformMatrix() ).y, 7 )
			    && Close( Origin( b1->GetFinalTransformMatrix() ).x, 0 ),
			       "K: sibling 2 of 3 composed -- not just the first child" );
			Check( Close( Origin( c1->GetFinalTransformMatrix() ).z, 2 )
			    && Close( Origin( c1->GetFinalTransformMatrix() ).y, 5 ),
			       "K: sibling 3 of 3 composed" );
			// And the walk descends past a sibling: twig hangs off the LAST limb.
			Check( Close( Origin( tw->GetFinalTransformMatrix() ).z, 4 )
			    && Close( Origin( tw->GetFinalTransformMatrix() ).y, 5 ),
			       "K: a grandchild under the THIRD sibling composes (the walk descends past siblings)" );
		}
		j->release();
		std::remove( sM );
	}

	// =================================================================
	// L -- the WORLD-SPACE delta path under a NON-IDENTITY parent.  The
	// absolute setters (cases F, G) are local-space and never touch
	// PushWorldOp_'s conjugation, so without this the one piece of math 87
	// singles out -- `parentWorld^-1 * worldOp * parentWorld` -- ships with no
	// regression coverage at all, and a swapped multiply order or an inverted
	// WorldToLocal would be invisible.
	//
	// The parent is rotated 90 degrees about Z and scaled 2, so its frame is
	// genuinely different from the world's: a WORLD +X translation of 3 must
	// move the child by 3 along WORLD +X regardless.
	// =================================================================
	{
		const char* sN = "sg_parent_worldop.RISEscene";
		WriteScene( sN,
			"standard_object\n{\nname turret\norientation 0 0 90\nscale 2 2 2\nposition 1 0 0\n}\n"
			"standard_object\n{\nname barrel\nparent turret\ngeometry g\nmaterial m\nposition 1 0 0\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sN ), "L: rotated+scaled-parent scene loads" );
		IObjectPriv* barrel = Obj( *j, "barrel" );
		Check( barrel != 0, "L: child registered" );
		if( barrel ) {
			const Matrix4 before = barrel->GetFinalTransformMatrix();
			const Point3 p0 = Origin( before );
			Check( barrel->IsParentWorldInvertible(), "L: the parent frame is invertible" );

			SceneEditController c( *j, 0 );
			c.SetSelection( Cat::Object, String( "barrel" ) );
			// The gizmo's translate drag is a WORLD delta.  Drive the same op
			// the drag produces, directly.
			Check( c.ForTest_TranslateSelectedObjectWorld( 3, 0, 0 ),
			       "L: a WORLD-space translate op applies" );
			IObjectPriv* after = Obj( *j, "barrel" );
			const Point3 p1 = after ? Origin( after->GetFinalTransformMatrix() ) : Point3( 0, 0, 0 );
			Check( after && Close( p1.x - p0.x, 3, 1e-9 )
			             && Close( p1.y - p0.y, 0, 1e-9 )
			             && Close( p1.z - p0.z, 0, 1e-9 ),
			       "L: the child moved by exactly the WORLD delta, not the parent-rotated/scaled one" );
			// The parent's own frame is unchanged by a child edit.
			IObjectPriv* turret = Obj( *j, "turret" );
			Check( turret && Close( Origin( turret->GetFinalTransformMatrix() ).x, 1 ),
			       "L: the parent did not move" );
		}
		j->release();
		std::remove( sN );
	}

	std::cout << "  " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
