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
//    A -- exact composed world matrices down a 3-level chain, and the LOCAL
//         accessor still reporting the authored transform.
//    B -- the three FinalizeTransformations caches under a composed matrix:
//         world area (analytic), inverse-transpose (normal stays perpendicular
//         to the world tangent plane), bounding box.
//    C -- IDEMPOTENCE.  Re-composing, and re-deriving, must not compound.
//         This is the 86 §3 bug class: that design pushed the parent matrix
//         onto a transform stack that never self-clears, so every re-apply
//         squared in another factor.  87 passes the parent world as an
//         ARGUMENT so a re-compose cannot accumulate.
//    D -- container nodes (no `geometry`): world-invisible, unenumerated,
//         zero area, still composable and still parentable.
//    E -- refusals: forward reference, self-parent, unknown parent, and
//         2-/3-cycles at runtime reparent.
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
	// A normal is transformed by (M^-1)^T, not by M, precisely so it stays
	// perpendicular to the surface under anisotropic scaling.  With the
	// parent scaling x by 3 and y by 1, a sphere's world surface is an
	// ellipsoid and the two transforms differ.  The invariant checked is
	// geometric and does not restate the implementation: the returned world
	// normal must be perpendicular to BOTH world tangents.  Computing the
	// inverse-transpose from the un-composed local matrix breaks it.
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
			bool anyEvaluated = false, allPerp = true, allUnit = true;
			const Point2 uvs[4] = { Point2( 0.17, 0.31 ), Point2( 0.42, 0.63 ),
			                        Point2( 0.71, 0.22 ), Point2( 0.88, 0.79 ) };
			for( int i = 0; i < 4; ++i ) {
				Point3 wp; Vector3 wn, dpdu, dpdv, dndu, dndv;
				if( !ball->ComputeAnalyticalDerivatives( uvs[i], 0, wp, wn, dpdu, dpdv, dndu, dndv ) ) continue;
				anyEvaluated = true;
				const Scalar lu = Vector3Ops::Magnitude( dpdu );
				const Scalar lv = Vector3Ops::Magnitude( dpdv );
				if( lu > 1e-6 && !Close( Vector3Ops::Dot( wn, dpdu ) / lu, 0, 1e-7 ) ) allPerp = false;
				if( lv > 1e-6 && !Close( Vector3Ops::Dot( wn, dpdv ) / lv, 0, 1e-7 ) ) allPerp = false;
				if( !Close( Vector3Ops::Magnitude( wn ), 1, 1e-7 ) ) allUnit = false;
			}
			Check( anyEvaluated, "B2: the sphere answered the analytical-derivative query" );
			Check( allPerp, "B2: the world normal stays perpendicular to both world tangents under a "
			                "non-uniformly-scaled PARENT (inverse-transpose built from the COMPOSED matrix)" );
			Check( allUnit, "B2: the world normal is unit length" );

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
		Check( j->ComposeObjectHierarchy(), "E5: the removal moved the orphan" );
		perch = Obj( *j, "perch" );
		Check( perch && Close( Origin( perch->GetFinalTransformMatrix() ).x, 0 )
		             && Close( Origin( perch->GetFinalTransformMatrix() ).y, 2 ),
		       "E5: the orphan is re-rooted onto its own local transform (0,2,0)" );
		Check( !j->ComposeObjectHierarchy(),
		       "E5: a second compose is a clean no-op" );
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

	std::cout << "  " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
