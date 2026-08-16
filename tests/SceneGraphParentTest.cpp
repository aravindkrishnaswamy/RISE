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
//         2-/3-cycles at runtime reparent; plus removal re-rooting its
//         children, including telling the light sampler when one emits.
//    G -- a LIVE edit re-composes the whole subtree, not only the node the
//         editor was handed.
//    H -- deleting a `parent` line actually DETACHES on an incremental apply.
//    I -- a CSG operand is refused at both ends of a parent link.
//    J -- the parent-invertibility test: a well-conditioned container at
//         x=1e7 is accepted at every distance decade, a collapsed scale is
//         not, and neither is a COMPOSED singular parent at depth 2.
//    K -- multi-child fan-out (the tree mechanism itself).
//    L -- a WORLD-space delta under a rotated, scaled parent, and the refusal
//         path: either the op applies exactly or nothing changes.
//    M -- the stored parent inverse IS the matrix the flag vouches for.
//    N -- the ACCEPT side of the bound: a world op through an ordinary
//         rotated, moderately anisotropic parent applies exactly.
//    O -- the CSG-operand refusal fires from a SCENE FILE, in the declaration
//         order the language actually forces.
//    P -- `csg_object`'s own `parent` end to end, and `parent none`.
//    Q -- a non-finite composed parent world is never behind a TRUE flag.
//    R -- descriptor integrity across EVERY chunk (an empty description is the
//         signature of a P() reference invalidated by a nested emplace_back).
//    S -- `rect_light` / `shape_light` compose against a parent.
//    T -- the container binding refusal holds at the IJob layer, which is the
//         one the non-CST hosts and the console command bind through.
//    V -- and on the AGENT commit path, which is where the derive's
//         drop-with-a-warning is invisible (a warning is not a diagnostic, so
//         the derivability gate passes and the agent is told `applied`) --
//         including chunk INSERT.
//    W -- but NOT on the history-replay paths: undo must be lossless and must
//         not wedge.  Two rounds gated the revert and both were wrong; W is
//         the sequence that proved it.
//    U -- and the UNDO path RESTORES rather than refusing: it replays a
//         document state that existed moments earlier, which the derive
//         already tolerates.
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
#include <set>
#include <cstdio>
#include <cmath>

#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IEmitter.h"
#include "../src/Library/Parsers/ChunkDescriptor.h"
#include "../src/Library/Parsers/ChunkParserRegistry.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Interfaces/IObjectPriv.h"
#include "../src/Library/Interfaces/IGeometry.h"
#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IEnumCallback.h"
#include "../src/Library/Utilities/BoundingBox.h"
#include "../src/Library/Utilities/FiniteMath.h"

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

//! The scene's light-topology generation.  The ray caster compares this across
//! an AttachScene to decide whether to rebuild its light samplers, so it is the
//! observable for "was the sampler told the luminary set moved".
static unsigned int SceneLightGenerationForTest( const IScene* scene )
{
	const RISE::Implementation::Scene* concrete =
		dynamic_cast<const RISE::Implementation::Scene*>( scene );
	return concrete ? concrete->GetLightTopologyGeneration() : 0u;
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
			// `a` carries a NON-IDENTITY transform deliberately: with `a` at the
			// origin, "c parented to a" and "c detached" both land at x=1 and
			// the detach assertion below is vacuous.
			"standard_object\n{\nname a\ngeometry g\nmaterial m\nposition 20 0 0\n}\n"
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
		Check( c && Close( Origin( c->GetFinalTransformMatrix() ).x, 22 ),
		       "E3: the refused reparents left the chain intact (c is still at x=22)" );
		Check( !j->ComposeObjectHierarchy(),
		       "E3: nothing moved -- the refusals changed no link" );

		// A LEGAL reparent works, and detaching restores the root pose.
		Check( j->SetObjectParent( "c", "a" ), "E3: reparenting c under a is allowed" );
		Check( j->ComposeObjectHierarchy(), "E3: the legal reparent moved something" );
		Check( c && Close( Origin( c->GetFinalTransformMatrix() ).x, 21 ),
		       "E3: c now composes against a, so it sits at x=21" );
		Check( j->SetObjectParent( "c", 0 ), "E3: detaching c is allowed" );
		Check( j->ComposeObjectHierarchy(), "E3: the detach MOVED c" );
		Check( c && Close( Origin( c->GetFinalTransformMatrix() ).x, 1 ),
		       "E3: a detached c falls back to its own local transform, x=1 not 21" );
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
	{
		// E6 -- a removed PARENT moves its children, and a moved child may be
		// an EMITTER.  No per-object check on the REMOVED object can see that:
		// the emitter is not the object being removed.  The ray caster rebuilds
		// its light samplers off the scene's light-topology generation, so if
		// that does not advance, NEE keeps emitting from a position where no
		// geometry is any more.  Third site to need this argument, after the
		// incremental apply and the live edit.
		const char* sR2 = "sg_parent_remove_emitter.RISEscene";
		WriteScene( sR2,
			"uniformcolor_painter\n{\nname pe\ncolor 1 1 1\n}\n"
			"lambertian_luminaire_material\n{\nname glow\nexitance pe\nscale 4\n}\n"
			"standard_object\n{\nname rig\nposition 5 0 0\n}\n"
			"standard_object\n{\nname bulb\nparent rig\ngeometry g\nmaterial glow\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sR2 ), "E6: emissive-child scene loads" );
		IObjectPriv* bulb = Obj( *j, "bulb" );
		Check( bulb && Close( Origin( bulb->GetFinalTransformMatrix() ).x, 5 ),
		       "E6: the emitter starts composed at x=5" );
		Check( bulb && bulb->GetMaterial() && bulb->GetMaterial()->GetEmitter(),
		       "E6: (sanity) the child really is an emitter" );
		const unsigned int genBefore = SceneLightGenerationForTest( j->GetScene() );
		Check( j->RemoveObject( "rig" ), "E6: the NON-emissive parent is removed" );
		bulb = Obj( *j, "bulb" );
		Check( bulb && Close( Origin( bulb->GetFinalTransformMatrix() ).x, 0 ),
		       "E6: the emitter moved to x=0 when its parent went away" );
		Check( SceneLightGenerationForTest( j->GetScene() ) != genBefore,
		       "E6: the light-topology generation ADVANCED -- a reused ray caster rebuilds its sampler "
		       "instead of emitting from where the bulb used to be" );
		j->release();
		std::remove( sR2 );
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
		// NESTED CSG still works.  Checked BEFORE `cut` is parented, because a
		// parented composite is itself refused as an operand (below) -- and
		// note "no geometry" alone must NOT be the operand/container test,
		// since a CSGObject has none either.
		const IScene* scI = j->GetScene();
		const IObjectManager* omI = scI ? scI->GetObjects() : 0;
		Check( omI && omI->GetItem( "nested" ) == 0, "I: `nested` not yet present" );
		RadianceMapConfig noMapI;
		const double zeroI[3] = { 0, 0, 0 };
		Check( j->AddCSGObject( "nested", "cut", "opA", 0, "m", 0, 0, noMapI, zeroI, zeroI, true, true ),
		       "I: a csg_object may take another csg_object as an operand (nested CSG still works)" );
		Check( !j->AddCSGObject( "bad", "perch2", "opA", 0, "m", 0, 0, noMapI, zeroI, zeroI, true, true ),
		       "I: a CONTAINER is refused as a CSG operand (it is a transform, not a shape)" );

		// The csg_object ITSELF is a world-visible node and IS parentable.
		// Use the OUTERMOST composite: `cut` is now an operand of `nested`, so
		// it is correctly refused for the same reason opA was.
		Check( !j->SetObjectParent( "cut", "perch2" ),
		       "I: a composite CONSUMED as an operand is refused, like any other operand" );
		Check( j->SetObjectParent( "nested", "perch2" ),
		       "I: the outermost csg_object is an ordinary scene-graph node" );
		// And that link is AUTHORABLE, so it can persist: a runtime-only link
		// would be silently reverted by the next full re-derive.
		bool csgHasParent = false;
		{
			const std::vector<ChunkParserEntry> parsers = CreateAllChunkParsers();
			for( size_t e = 0; e < parsers.size(); ++e ) {
				if( !parsers[e].parser ) continue;
				const ChunkDescriptor& d = parsers[e].parser->Describe();
				if( d.keyword != "csg_object" ) continue;
				for( size_t k = 0; k < d.parameters.size(); ++k ) {
					if( d.parameters[k].name == "parent" ) csgHasParent = true;
				}
			}
		}
		Check( csgHasParent,
		       "I: and `csg_object` declares a `parent` param, so that link can be written to the "
		       "document rather than living only in memory" );
		// A composite that IS parented cannot then be consumed as an operand:
		// an operand's matrix is read in its composite's frame, so the world
		// parent would be applied in the wrong space.
		Check( !j->AddCSGObject( "nested2", "nested", "opB", 0, "m", 0, 0, noMapI, zeroI, zeroI, true, true ),
		       "I: a PARENTED composite is refused as an operand -- its matrix would be read in the "
		       "outer composite's frame, applying the world parent in the wrong space" );
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
	// near-singular linear part would sail through.  The committed test
	// NORMALISES the linear part and then asks two questions of it: is the
	// computed inverse actually an inverse (a rank test that works at rank 1 as
	// well as rank 2), and is it well enough conditioned to conjugate through.
	// Transformable.cpp records the four formulations tried and rejected before
	// it, each with the case that defeats it.
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
		// DEPTH-2 degeneracy, at RANK 1.  A collapsed GRANDPARENT composes into a
		// grandchild's parent world as a matrix whose true determinant is zero
		// but whose COMPUTED determinant is a rounding residue, so
		// Matrix4Ops::Inverse does not return its input -- it returns an
		// adjugate/det "inverse" that is not an inverse of anything.
		//
		// RANK 1 specifically, not rank 2: for a rank-1 matrix EVERY 2x2 cofactor
		// is zero in exact arithmetic, so the determinant and the whole adjugate
		// are residue of the same order and their quotient is O(1).  A guard that
		// forms a condition number from that quotient sees ~20 and accepts -- it
		// is dividing noise by noise.  A guard that asks "is this computed
		// inverse actually an inverse" sees a residual of O(1) and refuses.
		// Measured before the fix: 80%% of composed rank-1 parents accepted, 0%%
		// of rank-2.  The guard worked exactly one rank too shallow, so this
		// case uses `scale 0 0 1`, not `scale 0 1 1`.
		const char* sL = "sg_parent_deep_degenerate.RISEscene";
		WriteScene( sL,
			"standard_object\n{\nname flat_gp\norientation -94 16 -47\nposition 1 1.26 -4.3\nscale 0 0 1\n}\n"
			"standard_object\n{\nname mid_p\nparent flat_gp\norientation 147 -11 18\nposition -3 2.2 0.4\n"
			"scale 1.739 1.312 2.611\n}\n"
			"standard_object\n{\nname grandkid\nparent mid_p\ngeometry g\nmaterial m\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sL ), "J: depth-2 degenerate scene loads" );
		IObjectPriv* gk = Obj( *j, "grandkid" );
		Check( gk && !gk->IsParentWorldInvertible(),
		       "J: a COMPOSED RANK-1 parent is caught at depth 2 -- the case where the determinant AND "
		       "the whole adjugate are rounding residue, so their quotient looks well-conditioned" );
		j->release();
		std::remove( sL );
	}
	{
		// EXTREME ANISOTROPY.  Nothing is singular here -- a ground-plane
		// container at `scale 1e5 1e-5 1e5` has determinant 1e5 and renders
		// perfectly well -- but conjugating a world delta through a frame whose
		// axis scales differ by 1e10 keeps almost none of double's digits, so
		// the world-space editor op is refused rather than silently mangled.
		// The moderate case one thousand times gentler is accepted, so this is
		// a bound and not a blanket ban on non-uniform scale.
		const char* sP = "sg_parent_aniso_extreme.RISEscene";
		WriteScene( sP,
			"standard_object\n{\nname slab\nscale 100000 0.00001 100000\n}\n"
			"standard_object\n{\nname onslab\nparent slab\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname mild\nscale 1000 0.001 1\n}\n"
			"standard_object\n{\nname onmild\nparent mild\ngeometry g\nmaterial m\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sP ), "J: anisotropy scene loads" );
		IObjectPriv* onslab = Obj( *j, "onslab" );
		IObjectPriv* onmild = Obj( *j, "onmild" );
		Check( onslab && !onslab->IsParentWorldInvertible(),
		       "J: an ancestor with a ~1e10 axis-scale spread is refused as a change of frame" );
		Check( onmild && onmild->IsParentWorldInvertible(),
		       "J: a ~1e6 spread is still accepted -- this is a conditioning bound, not a ban on "
		       "non-uniform scale" );
		// Both still RENDER: the refusal is about editing, not about visibility.
		Check( onslab && onslab->IsWorldVisible() && onslab->GetArea() > 0,
		       "J: the ill-conditioned subtree is still perfectly renderable" );
		j->release();
		std::remove( sP );
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
	{
		// L2 -- the op REFUSES, and leaves the transform untouched, when the
		// conjugation cannot be done.  The predicate that used to be the only
		// gate here has been wrong four times in review, so PushWorldOp_ now
		// verifies its own result -- `parentWorld * localOp == worldOp *
		// parentWorld` -- rather than trusting a prediction about the parent.
		// This case pins the invariant that actually matters: EITHER the op
		// applies exactly, OR nothing changes.  There is no third outcome where
		// a wrong matrix reaches the object.
		const char* sQ = "sg_parent_refuse.RISEscene";
		WriteScene( sQ,
			"standard_object\n{\nname squashed\nscale 0 0 1\n}\n"
			"standard_object\n{\nname doomed\nparent squashed\ngeometry g\nmaterial m\nposition 0 1 0\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sQ ), "L2: degenerate-parent scene loads" );
		IObjectPriv* doomed = Obj( *j, "doomed" );
		Check( doomed != 0, "L2: child registered" );
		if( doomed ) {
			const Matrix4 beforeLocal = doomed->GetLocalTransformMatrix();
			SceneEditController c( *j, 0 );
			c.SetSelection( Cat::Object, String( "doomed" ) );
			Check( !c.ForTest_TranslateSelectedObjectWorld( 3, 0, 0 ),
			       "L2: the world-space op is REFUSED under a collapsed parent" );
			IObjectPriv* after = Obj( *j, "doomed" );
			Check( after && Mat4Exact( after->GetLocalTransformMatrix(), beforeLocal ),
			       "L2: and the object's LOCAL transform is byte-identical -- a refused op mutates "
			       "nothing, so no corrupt matrix can reach the scene document" );
		}
		j->release();
		std::remove( sQ );
	}

	// =================================================================
	// M -- the STORED inverse must be the one the flag vouches for.
	//
	// The guard validates a Frobenius-NORMALISED 3x3; the inverse handed to
	// WorldToLocal must be built from those same quantities, not from the raw
	// 4x4 adjugate/determinant.  A uniform `scale 1e-120` is perfectly
	// conditioned and its 4x4 determinant is 1e-360, i.e. zero -- and
	// Matrix4Ops::Inverse RETURNS ITS INPUT at zero determinant, so the flag
	// would say "invertible" while the stored matrix was P itself.  Validating
	// one matrix and storing another is the shape of that bug.
	//
	// The published contract is that WorldToLocal is usable when the flag is
	// true, so that is what this asserts, at both ends of the exponent range.
	// =================================================================
	{
		// The exponents deliberately straddle where a FROBENIUS normaliser
		// breaks.  Squaring 1e-180 underflows to exactly 0 and squaring 1e180
		// overflows to inf, so a Frobenius normaliser refuses both -- while the
		// frames are perfectly conditioned uniform scales.  The committed
		// normaliser is max|entry|, which cannot do either.  The outermost two
		// exponents are what pin that choice; the inner ones would pass under
		// either.
		const char* kScales[8] = { "1e-180", "1e-170", "1e-120", "1e-40",
		                           "1e40", "1e120", "1e170", "1e180" };
		for( int i = 0; i < 8; ++i ) {
			const char* sT = "sg_parent_tiny.RISEscene";
			WriteScene( sT,
				// A TRANSLATION as well as the scale, deliberately: with a pure
				// diagonal parent the inverse's translation column is
				// -(L^-1 * 0) = 0 and the half of the un-normalisation that BOTH
				// fix rounds patched is never evaluated at all.
				std::string( "standard_object\n{\nname microscale\nposition 3 -7 11\nscale " ) + kScales[i] + " " +
				kScales[i] + " " + kScales[i] + "\n}\n"
				"standard_object\n{\nname speckle\nparent microscale\ngeometry g\nmaterial m\n}\n" );
			Job* j = new Job();
			const bool loaded = j->LoadAsciiSceneViaCst( sT );
			IObjectPriv* sp = loaded ? Obj( *j, "speckle" ) : 0;
			const bool flag = sp && sp->IsParentWorldInvertible();
			Check( flag, "M: a uniform scale is well-conditioned at any exponent, so the flag is TRUE" );
			if( flag ) {
				// The contract: P^-1 * P == I.  Round-trip a known matrix
				// through WorldToLocal and back through the parent world.
				const Matrix4 P = sp->GetParentWorldTransformMatrix();
				const Matrix4 probe = Matrix4Ops::Translation( Vector3( 2, -3, 5 ) );
				const Matrix4 back = P * sp->WorldToLocal( probe );
				Check( Mat4Close( back, probe, 1e-9 ),
				       "M: and WorldToLocal really inverts it -- the STORED inverse is the matrix the "
				       "flag vouches for, not a raw 4x4 inverse whose determinant under/overflowed" );
			}
			j->release();
			std::remove( sT );
		}
	}

	// =================================================================
	// N -- a world op through an ACCEPTED but non-trivially conditioned
	// parent must apply EXACTLY.  Case L uses a parent with condition number
	// ~1 and case L2 a rank-1 one; without something in between, tightening
	// either threshold would pass every assertion in this file while making
	// children of any ordinary rotated, moderately anisotropic container
	// undraggable.  This pins the accept side of the bound.
	//
	// It also drives the OTHER op routed through the conjugation, the
	// pivot-carrying rotate, which nothing else exercises under a parent.
	// =================================================================
	{
		const char* sU = "sg_parent_midcond.RISEscene";
		WriteScene( sU,
			"standard_object\n{\nname bench\nscale 400 0.05 90\norientation 0 35 0\nposition 12 3 -7\n}\n"
			"standard_object\n{\nname widget\nparent bench\ngeometry g\nmaterial m\nposition 0.5 1 0\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sU ), "N: mid-conditioned parent scene loads" );
		IObjectPriv* w = Obj( *j, "widget" );
		Check( w && w->IsParentWorldInvertible(),
		       "N: an ordinary rotated, moderately anisotropic container is ACCEPTED" );
		if( w ) {
			const Point3 p0 = Origin( w->GetFinalTransformMatrix() );
			SceneEditController c( *j, 0 );
			c.SetSelection( Cat::Object, String( "widget" ) );
			Check( c.ForTest_TranslateSelectedObjectWorld( -4, 2.5, 1 ),
			       "N: the world-space op is ACCEPTED, not refused" );
			IObjectPriv* after = Obj( *j, "widget" );
			const Point3 p1 = after ? Origin( after->GetFinalTransformMatrix() ) : Point3( 0, 0, 0 );
			Check( after && Close( p1.x - p0.x, -4, 1e-6 )
			             && Close( p1.y - p0.y, 2.5, 1e-6 )
			             && Close( p1.z - p0.z, 1, 1e-6 ),
			       "N: and it moved by EXACTLY the world delta through that frame" );
		}
		j->release();
		std::remove( sU );
	}

	// =================================================================
	// O -- the CSG operand refusal must fire FROM A SCENE FILE.
	//
	// ObjectManager::SetObjectParent identifies an operand as "hidden and not
	// a container", but an operand is only HIDDEN once its csg_object is
	// applied -- which declare-before-use forces to come AFTER the operand's
	// own chunk.  So from scene text the child-side gate can never fire, and
	// the reciprocal check has to live where operand-ness is actually decided.
	// The render consequence is silent: CSGObject::IntersectRay reads the
	// operand's matrix as CSG-LOCAL, so a world parent moves the operand
	// somewhere nobody asked for and the boolean quietly changes shape.
	// =================================================================
	{
		const char* sV = "sg_parent_csg_authored.RISEscene";
		WriteScene( sV,
			"standard_object\n{\nname mount\nposition 3 0 0\n}\n"
			"standard_object\n{\nname pA\ngeometry g\nmaterial m\nparent mount\n}\n"
			"standard_object\n{\nname pB\ngeometry g\nmaterial m\nposition 0.5 0 0\n}\n"
			"csg_object\n{\nname carved\nobja pA\nobjb pB\noperation union\nmaterial m\n}\n" );
		Job* j = new Job();
		Check( !j->LoadAsciiSceneViaCst( sV ),
		       "O: a scene that parents an object and THEN makes it a CSG operand is refused at load" );
		j->release();
		std::remove( sV );
	}
	{
		// The other direction: something parented TO an object that later
		// becomes an operand.
		const char* sW = "sg_parent_csg_authored2.RISEscene";
		WriteScene( sW,
			"standard_object\n{\nname qA\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname rider\ngeometry g\nmaterial m\nparent qA\nposition 0 1 0\n}\n"
			"standard_object\n{\nname qB\ngeometry g\nmaterial m\nposition 0.5 0 0\n}\n"
			"csg_object\n{\nname welded\nobja qA\nobjb qB\noperation union\nmaterial m\n}\n" );
		Job* j = new Job();
		Check( !j->LoadAsciiSceneViaCst( sW ),
		       "O: and a scene that parents something TO an object which then becomes an operand" );
		j->release();
		std::remove( sW );
	}
	{
		// P -- `csg_object`'s own `parent`, end to end from a scene file, and
		// `parent none` as the explicit detach form.  Case I only checks that
		// the descriptor DECLARES the param.
		const char* sX = "sg_parent_csg_parented.RISEscene";
		WriteScene( sX,
			"standard_object\n{\nname rig2\nposition 3 1 -2\n}\n"
			"standard_object\n{\nname sA\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname sB\ngeometry g\nmaterial m\nposition 0.5 0 0\n}\n"
			"csg_object\n{\nname gadget\nobja sA\nobjb sB\noperation union\nmaterial m\n"
			"parent rig2\nposition 0.5 -0.25 0.75\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sX ), "P: a parented csg_object loads" );
		IObjectPriv* gadget = Obj( *j, "gadget" );
		Check( gadget && Close( Origin( gadget->GetFinalTransformMatrix() ).x, 3.5, 1e-9 )
		              && Close( Origin( gadget->GetFinalTransformMatrix() ).y, 0.75, 1e-9 )
		              && Close( Origin( gadget->GetFinalTransformMatrix() ).z, -1.25, 1e-9 ),
		       "P: the composite composes against its parent -- world (3.5, 0.75, -1.25)" );
		j->release();
		std::remove( sX );
	}
	{
		const char* sY = "sg_parent_none.RISEscene";
		WriteScene( sY,
			"standard_object\n{\nname holder\nposition 8 0 0\n}\n"
			"standard_object\n{\nname freed\ngeometry g\nmaterial m\nparent none\nposition 0 1 0\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sY ), "P: `parent none` loads" );
		IObjectPriv* freed = Obj( *j, "freed" );
		Check( freed && Close( Origin( freed->GetFinalTransformMatrix() ).x, 0 ),
		       "P: `parent none` is the explicit no-parent sentinel, matching `geometry none`" );
		j->release();
		std::remove( sY );
	}

	// =================================================================
	// Q -- a NON-FINITE parent world must not sit behind a TRUE flag.
	//
	// The translation column is not part of the linear part and cannot make an
	// affine map singular -- but it IS fed into the stored inverse's own
	// translation, so an infinite one produces a NaN "inverse".  An earlier
	// revision checked finiteness over the 3x3 only and did exactly that.  No
	// non-finite literal is needed to reach it: the parser rejects those, and
	// composition produces one anyway.
	// =================================================================
	{
		const char* sZ = "sg_parent_overflow.RISEscene";
		WriteScene( sZ,
			"standard_object\n{\nname gp\nscale 1e150 1e150 1e150\n}\n"
			"standard_object\n{\nname midz\nparent gp\nscale 1e-150 1e-150 1e-150\nposition 1e200 0 0\n}\n"
			"standard_object\n{\nname kidz\nparent midz\ngeometry g\nmaterial m\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sZ ), "Q: overflow-composing scene loads" );
		IObjectPriv* kidz = Obj( *j, "kidz" );
		Check( kidz != 0, "Q: grandchild registered" );
		if( kidz ) {
			const Matrix4 P = kidz->GetParentWorldTransformMatrix();
			const bool parentFinite = IsFiniteDouble( static_cast<double>( P._30 ) )
			                       && IsFiniteDouble( static_cast<double>( P._31 ) )
			                       && IsFiniteDouble( static_cast<double>( P._32 ) );
			Check( !parentFinite,
			       "Q: (sanity) the composed parent world really does have a non-finite translation" );
			Check( !kidz->IsParentWorldInvertible(),
			       "Q: and the flag is FALSE -- a non-finite translation cannot sit behind a TRUE flag, "
			       "because it is what the stored inverse's own translation is built from" );
		}
		j->release();
		std::remove( sZ );
	}
	{
		// Q2 -- the OUTPUT side.  The un-normalisation runs AFTER both tests:
		// L^-1 = Lh^-1/frob blows up for a tiny `frob`, and -L^-1 t multiplies
		// that by the translation.  Both are finite INPUTS composing to a
		// non-finite INVERSE, so checking the input alone leaves a TRUE flag
		// fronting a NaN matrix -- the same "validate one thing, store another"
		// shape this guard has already been bitten by once.
		const char* sZ2 = "sg_parent_invoverflow.RISEscene";
		WriteScene( sZ2,
			"standard_object\n{\nname faroff\nposition 1e200 0 0\n}\n"
			"standard_object\n{\nname shrunk\nparent faroff\nscale 1e-150 1e-150 1e-150\n}\n"
			"standard_object\n{\nname tip\nparent shrunk\ngeometry g\nmaterial m\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sZ2 ), "Q2: inverse-overflow scene loads" );
		IObjectPriv* tip = Obj( *j, "tip" );
		Check( tip != 0, "Q2: grandchild registered" );
		if( tip ) {
			const Matrix4 P = tip->GetParentWorldTransformMatrix();
			bool inputFinite = true;
			{
				const Scalar* pp = &P._00;
				for( int k = 0; k < 16; ++k ) {
					if( !IsFiniteDouble( static_cast<double>( pp[k] ) ) ) { inputFinite = false; break; }
				}
			}
			Check( inputFinite, "Q2: (sanity) the parent world itself is entirely FINITE" );
			// Whatever the verdict, the stored inverse must never be non-finite
			// behind a TRUE flag.
			if( tip->IsParentWorldInvertible() ) {
				const Matrix4 back = P * tip->WorldToLocal( Matrix4Ops::Translation( Vector3( 2, -3, 5 ) ) );
				const Scalar* b2 = &back._00;
				bool finiteOut = true;
				for( int k = 0; k < 16; ++k ) {
					if( !IsFiniteDouble( static_cast<double>( b2[k] ) ) ) { finiteOut = false; break; }
				}
				Check( finiteOut,
				       "Q2: a TRUE flag never fronts a non-finite inverse -- the un-normalisation's own "
				       "result is checked, not just its inputs" );
			} else {
				Check( true, "Q2: refused, which is also a correct answer for an inverse that overflows" );
			}
		}
		j->release();
		std::remove( sZ2 );
	}

	// =================================================================
	// R -- DESCRIPTOR INTEGRITY.  Nothing in this file looked at descriptor
	// TEXT, and that is how a heap use-after-free shipped through 156 green
	// assertions: a `parent` entry spliced INSIDE a still-open `name` block
	// made P()'s emplace_back reallocate, so the outer `p` reference dangled
	// and the description that followed was written into freed memory.  The
	// observable is a description that comes back EMPTY, which is also a real
	// regression on its own -- SchemaGen emits it to the agent and the panel
	// shows it.
	//
	// Checked for EVERY registered chunk, not just the ones this arc touched:
	// the bug is a C++ idiom trap, not a fact about parenting, and the next
	// person to add a param anywhere can fall into it.
	// =================================================================
	{
		const std::vector<ChunkParserEntry> parsers = CreateAllChunkParsers();
		Check( parsers.size() > 100, "R: the parser registry loaded" );
		int emptyDesc = 0, emptyName = 0;
		std::string firstBad;
		for( size_t e = 0; e < parsers.size(); ++e ) {
			if( !parsers[e].parser ) continue;
			const ChunkDescriptor& d = parsers[e].parser->Describe();
			for( size_t k = 0; k < d.parameters.size(); ++k ) {
				if( d.parameters[k].name.empty() ) {
					++emptyName;
					if( firstBad.empty() ) firstBad = d.keyword + " (a parameter has no name)";
				} else if( d.parameters[k].description.empty() ) {
					++emptyDesc;
					if( firstBad.empty() ) firstBad = d.keyword + "." + d.parameters[k].name;
				}
			}
		}
		if( emptyDesc || emptyName ) {
			std::cout << "    (R: first offender: " << firstBad << ")" << std::endl;
		}
		Check( emptyName == 0, "R: every declared parameter has a name" );
		Check( emptyDesc == 0,
		       "R: every declared parameter has a non-empty description -- an empty one is the "
		       "signature of a P() reference invalidated by a nested emplace_back" );

		// An empty description is the signature only when the post-realloc
		// write lands on `description`.  The SAME splice one line lower is the
		// same use-after-free and leaves the check above green, so also assert
		// the SHAPE invariants that the later fields have to satisfy: an Enum
		// must offer values, a Reference must name categories, and no chunk may
		// declare a parameter name twice.  These widen the check past
		// `description` to the fields most P() blocks set after it; they are NOT
		// exhaustive -- `required`, `defaultValueHint` and a corrupted `kind` on
		// a parameter that is neither Enum nor Reference would still slip
		// through.  The structurally complete check is a source-level lint for a
		// second P() inside an open block; this is the runtime approximation.
		int badEnum = 0, badRef = 0, dupName = 0;
		std::string firstShapeBad;
		for( size_t e = 0; e < parsers.size(); ++e ) {
			if( !parsers[e].parser ) continue;
			const ChunkDescriptor& d = parsers[e].parser->Describe();
			std::set<std::string> seen;
			for( size_t k = 0; k < d.parameters.size(); ++k ) {
				const ParameterDescriptor& pd = d.parameters[k];
				if( pd.kind == ValueKind::Enum && pd.enumValues.empty() ) {
					++badEnum;
					if( firstShapeBad.empty() ) firstShapeBad = d.keyword + "." + pd.name + " (Enum, no values)";
				}
				if( pd.kind == ValueKind::Reference && pd.referenceCategories.empty() ) {
					++badRef;
					if( firstShapeBad.empty() ) firstShapeBad = d.keyword + "." + pd.name + " (Reference, no categories)";
				}
				if( !pd.name.empty() && !seen.insert( pd.name ).second ) {
					++dupName;
					if( firstShapeBad.empty() ) firstShapeBad = d.keyword + "." + pd.name + " (declared twice)";
				}
			}
		}
		if( badEnum || badRef || dupName ) {
			std::cout << "    (R: first shape offender: " << firstShapeBad << ")" << std::endl;
		}
		Check( badEnum == 0, "R: every Enum parameter offers enumValues" );
		Check( badRef  == 0, "R: every Reference parameter names referenceCategories" );
		Check( dupName == 0, "R: no chunk declares the same parameter name twice" );

		// The sweep above is a CORRUPTION check: it catches a `parent` entry
		// that got mangled, not one that was never declared.  Deleting the
		// declaration outright leaves every aggregate above green (the chunk
		// simply has one parameter fewer), so name the four chunks that must
		// carry it and assert the entry is present AND well-formed.  A missing
		// declaration would also break cases A / I / P / S at scene-load time,
		// but those report it as "the scene failed to parse", which is a long
		// way from "someone deleted a descriptor entry".
		{
			const char* kParentChunks[] = { "standard_object", "csg_object", "rect_light", "shape_light" };
			for( size_t n = 0; n < sizeof(kParentChunks)/sizeof(kParentChunks[0]); ++n ) {
				const ParameterDescriptor* found = 0;
				for( size_t e = 0; e < parsers.size() && !found; ++e ) {
					if( !parsers[e].parser ) continue;
					const ChunkDescriptor& d = parsers[e].parser->Describe();
					if( d.keyword != kParentChunks[n] ) continue;
					for( size_t k = 0; k < d.parameters.size(); ++k ) {
						if( d.parameters[k].name == "parent" ) { found = &d.parameters[k]; break; }
					}
				}
				const std::string who = std::string( "R: `" ) + kParentChunks[n] + "` declares a `parent` parameter";
				Check( found != 0, who.c_str() );
				if( found ) {
					Check( found->kind == ValueKind::Reference,
					       ( who + ", as an object Reference" ).c_str() );
					Check( !found->referenceCategories.empty()
					    && found->referenceCategories[0] == ChunkCategory::Object,
					       ( who + ", resolving against the Object category" ).c_str() );
					Check( !found->description.empty(),
					       ( who + ", with a non-empty description" ).c_str() );
				}
			}
		}
	}

	// =================================================================
	// S -- the one-chunk area lights are scene-graph nodes too.
	// =================================================================
	{
		const char* sS2 = "sg_parent_lights.RISEscene";
		WriteScene( sS2,
			"standard_object\n{\nname gantry\nposition 6 4 0\n}\n"
			"rect_light\n{\nname panel\nparent gantry\ncenter 0 1 0\nsize 2 1\nfacing 0 -1 0\n"
			"exitance 40\ncolor 1 1 1\n}\n"
			"shape_light\n{\nname bulb2\nparent gantry\nshape sphere\ncenter 0 -1 0\nsize 0.3\n"
			"exitance 15\ncolor 1 1 1\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sS2 ), "S: parented area lights load" );
		IObjectPriv* panel = Obj( *j, "panel" );
		IObjectPriv* bulb2 = Obj( *j, "bulb2" );
		// The two chunks place their geometry DIFFERENTLY, which is worth
		// pinning: `shape_light` passes `center` to the object as its position,
		// so its transform origin composes; `rect_light` bakes `center` into
		// the clipped-plane's four CORNERS and gives the object an identity
		// transform.  So the rect_light's own origin is the parent's origin,
		// and the panel's placement shows up in its world BOUNDING BOX -- which
		// is the observable that actually matters, and the one that proves the
		// parent transform reached the geometry.
		Check( panel && Close( Origin( panel->GetFinalTransformMatrix() ).x, 6 )
		             && Close( Origin( panel->GetFinalTransformMatrix() ).y, 4 ),
		       "S: a rect_light's object origin is its parent's origin (it bakes `center` into geometry)" );
		if( panel ) {
			const BoundingBox pb = panel->getBoundingBox();
			const Scalar cx = ( pb.ll.x + pb.ur.x ) * Scalar( 0.5 );
			const Scalar cy = ( pb.ll.y + pb.ur.y ) * Scalar( 0.5 );
			Check( Close( cx, 6, 1e-6 ) && Close( cy, 5, 1e-6 ),
			       "S: and its world bounding box is centred at (6,5,0) -- the parent transform reached "
			       "the baked geometry" );
		}
		Check( bulb2 && Close( Origin( bulb2->GetFinalTransformMatrix() ).x, 6 )
		             && Close( Origin( bulb2->GetFinalTransformMatrix() ).y, 3 ),
		       "S: and so does a shape_light -- world (6,3,0)" );
		// They are still EMITTERS after being parented.
		Check( panel && panel->GetMaterial() && panel->GetMaterial()->GetEmitter(),
		       "S: the parented rect_light is still an emitter" );
		j->release();
		std::remove( sS2 );
	}

	// =================================================================
	// T -- the container binding refusal must hold at the IJob layer too.
	//
	// Three layers can bind a surface slot to an object: the DERIVE
	// (DropContainerSurfaceBindings_), the EDITOR (ApplyObjectOpForward and the
	// CST pre-routing gate), and the public IJob setters.  The first two were
	// gated in earlier rounds; the third is the API the non-CST embedding binds
	// through, and the shipped console command `modify object <name> material
	// <name2>` routes straight to it.  Ungated, "a container never carries a
	// material" -- the premise the agent's non-sampling-emitter audit is
	// written against -- was true of AddObject and false of SetObjectMaterial,
	// which is how a world-invisible, zero-area EMITTER could be created with
	// no diagnostic at all.
	// =================================================================
	{
		const char* sT2 = "sg_parent_ijobbind.RISEscene";
		WriteScene( sT2,
			"uniformcolor_painter\n{\nname pe2\ncolor 1 1 1\n}\n"
			"lambertian_luminaire_material\n{\nname glow2\nexitance pe2\nscale 4\n}\n"
			"standard_object\n{\nname hub3\nposition 2 0 0\n}\n"
			"standard_object\n{\nname shape3\ngeometry g\nmaterial m\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sT2 ), "T: scene loads" );
		IObjectPriv* hub3 = Obj( *j, "hub3" );
		Check( hub3 && hub3->GetGeometry() == 0 && hub3->GetMaterial() == 0,
		       "T: the container starts with no geometry and no material" );

		Check( !j->SetObjectMaterial( "hub3", "glow2" ),
		       "T: IJob::SetObjectMaterial REFUSES a container" );
		Check( hub3 && hub3->GetMaterial() == 0,
		       "T: and nothing was bound -- no world-invisible zero-area emitter was created" );

		// The same call on a real shape still works, so the refusal is
		// container-specific rather than a blanket breakage.
		Check( j->SetObjectMaterial( "shape3", "glow2" ),
		       "T: an object WITH geometry still binds normally" );
		IObjectPriv* shape3 = Obj( *j, "shape3" );
		Check( shape3 && shape3->GetMaterial() && shape3->GetMaterial()->GetEmitter(),
		       "T: and that binding really took effect" );
		j->release();
		std::remove( sT2 );
	}

	// =================================================================
	// U -- the container rule on the UNDO path.
	//
	// ApplyRevertMutation restores a CAPTURED prior binding, so it is a fourth
	// place a material can land on an object -- and the forward gates cannot
	// stop it, because the capture predates them.  Reachable when an edit
	// sequence turns a leaf into a container between the bind and the undo.
	//
	// The gate has to sit ABOVE the CST routing: that routing RETURNS, so a
	// check placed after it is dead on every retained-CST scene, which is the
	// dominant GUI/agent edit model.  This case therefore drives a CST-loaded
	// scene deliberately -- an API-built one would pass either way.
	// =================================================================
	{
		const char* sU2 = "sg_parent_undo_bind.RISEscene";
		WriteScene( sU2,
			"scene_variant\n{\nname night\n}\n"
			"uniformcolor_painter\n{\nname p3\ncolor 1 0 0\n}\n"
			"lambertian_material\n{\nname m3\nreflectance p3\n}\n"
			"standard_object\n{\nname morph\ngeometry g\nmaterial m\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sU2 ), "U: scene loads (retained CST document)" );
		Check( j->HasRetainedCstDocument(),
		       "U: (sanity) the scene really does have a retained CST Document, so the CST routing arm "
		       "is the one under test" );

		SceneEditController c( *j, 0 );
		c.SetSelection( Cat::Object, String( "morph" ) );
		// 1. Bind a different material while the object is still a LEAF.
		Check( c.SetPropertyForCategory( Cat::Object, String( "material" ), String( "m3" ) ),
		       "U: the material edit applies while the object still has geometry" );
		// 2. Turn it into a CONTAINER by removing its `geometry` line.  This has
		//    to go through the Job primitive: the editor's own edit vocabulary
		//    has no "clear geometry" op (SceneEdit::SetObjectGeometry only ever
		//    binds a NAMED geometry), so a container can only be made by
		//    removing the param from the Document.
		const int rc = j->ApplyCstParamRemoveChecked( "morph", "standard_object", "geometry", 0 );
		Check( rc != 0, "U: the geometry removal is accepted" );
		// A code >=2 removal is a D2: it ClearAll'd and rebuilt the Scene and
		// every manager, so the editor's BORROWED mScene / manager pointers now
		// dangle.  Production never has to think about this because the
		// removal goes through SceneEditor::RouteCstParamRemove_, which calls
		// RebindToJob_ on >=2.  This test calls the Job primitive underneath
		// that, so it owes the same rebind by hand -- otherwise the very next
		// FindObject reads a freed Scene (observed: a jump through a garbage
		// vptr, layout-dependent, so it survived instrumentation).
		{
			IJobPriv* jp = dynamic_cast<IJobPriv*>( static_cast<IJob*>( j ) );
			Check( jp != 0, "U: (sanity) the Job exposes its private interface for the rebind" );
			if( jp ) {
				if( IScenePriv* sc = jp->GetScene() ) c.Editor().RebindScene( *sc );
				c.Editor().SetMaterialManager( jp->GetMaterials() );
				c.Editor().SetShaderManager( jp->GetShaders() );
				c.Editor().SetPainterManager( jp->GetPainters() );
				c.Editor().SetScalarPainterManager( jp->GetScalarPainters() );
			}
		}
		IObjectPriv* morph = Obj( *j, "morph" );
		Check( morph && morph->GetGeometry() == 0,
		       "U: the object is now a container" );
		// 3. UNDO the material edit.  It MUST SUCCEED.  The captured prior value
		//    is a real material name and the object it lands on is now a
		//    container -- and that is fine: undo RESTORES a document state that
		//    existed moments earlier, one the derive already tolerates (warn,
		//    drop the binding, carry on), which is the contract for every scene
		//    file ever authored.  Refusing it would wedge the undo stack, since
		//    a refused revert is pushed back and re-fails forever.
		c.Undo();
		morph = Obj( *j, "morph" );
		Check( morph && morph->GetGeometry() == 0,
		       "U: (sanity) still a container after the undo" );
		Check( morph && morph->GetMaterial() == 0,
		       "U: the derive still drops a container's material -- the LIVE object carries none" );

		auto ChunkOf = []( const RISE::Cst::Document& d, const char* objName ) -> std::string {
			const std::string text = RISE::Cst::SerializeCst( d );
			const std::string key  = std::string( "name " ) + objName;
			size_t at = text.find( key );
			while( at != std::string::npos ) {
				const size_t after = at + key.size();
				if( after >= text.size() || text[after] == '\n' || text[after] == '\r' ) break;
				at = text.find( key, after );
			}
			if( at == std::string::npos ) return std::string();
			const size_t end = text.find( "\n}", at );
			return text.substr( at, ( end == std::string::npos ? text.size() : end ) - at );
		};
		auto ParamValue = []( const std::string& chunk, const char* key ) -> std::string {
			const std::string needle = std::string( "\n" ) + key + " ";
			std::string out;
			size_t p = chunk.find( needle );
			while( p != std::string::npos ) {
				const size_t vs = p + needle.size();
				const size_t ve = chunk.find( '\n', vs );
				out = chunk.substr( vs, ( ve == std::string::npos ? chunk.size() : ve ) - vs );
				p = chunk.find( needle, vs );
			}
			return out;
		};

		// THE DECIDING OBSERVABLE IS THE DOCUMENT.  Live state cannot tell a
		// refused undo from an accepted one here -- the derive drops a
		// container's material either way -- so read the text and require the
		// AUTHORED value back.  A refused undo would leave `m3` standing.
		const RISE::Cst::Document* doc = j->GetCstDocument();
		Check( doc != 0, "U: the Document is retained" );
		if( doc ) {
			const std::string chunk = ChunkOf( *doc, "morph" );
			Check( !chunk.empty(), "U: found the object's chunk in the Document" );
			const std::string mat = ParamValue( chunk, "material" );
			if( mat != "m" ) std::cout << "    (U: chunk text was:\n" << chunk << "\n)" << std::endl;
			Check( mat == "m",
			       "U: the undo RESTORED the authored binding -- it is not refused, and the undo stack "
			       "is not wedged" );
		}

		// REDO is a history replay too, and Redo SHARES ApplyForwardMutation with
		// the creation path -- so without an explicit "this is a replay" flag the
		// forward container gate fires here and wedges the redo stack.  Escapable
		// (any new edit clears redo) where the undo wedge is not, but the pair has
		// to be symmetric or the block comment claiming it is, is a lie.
		c.Redo();
		{
			const RISE::Cst::Document* doc3 = j->GetCstDocument();
			Check( doc3 != 0, "U: (sanity) the Document survived the redo" );
			if( doc3 ) {
				const std::string mat3 = ParamValue( ChunkOf( *doc3, "morph" ), "material" );
				Check( mat3 == "m3",
				       "U: the REDO re-applied the edit onto the container -- history replay is exempt "
				       "on both directions, not just Undo" );
			}
		}

		// And the CLEAR is still allowed on a container, so a stale binding CAN
		// be removed.  Refusing this would refuse the one edit that fixes the
		// warning every later derive emits.
		c.SetSelection( Cat::Object, String( "morph" ) );
		Check( c.SetPropertyForCategory( Cat::Object, String( "material" ), String( "none" ) ),
		       "U: an explicit CLEAR is accepted on a container, so a stale binding can be removed" );
		{
			const RISE::Cst::Document* doc2 = j->GetCstDocument();
			Check( doc2 != 0, "U: (sanity) the Document survived the clear" );
			if( doc2 ) {
				const std::string mat2 = ParamValue( ChunkOf( *doc2, "morph" ), "material" );
				Check( mat2 != "m",
				       "U: and the stale binding is gone from the Document" );
			}
		}
		j->release();
		std::remove( sU2 );
	}

	// =================================================================
	// V -- the container rule on the AGENT commit path.
	//
	// The fourth and last layer that can write a binding into the Document.
	// It matters more than the count suggests: this is the path an agent
	// actually edits through, and it is the one where the derive's "drop it
	// with a warning and report success" is INVISIBLE -- a log warning is not
	// a diagnostic, so the full-derivability gate passes, the head commits,
	// and the agent is told `applied`.
	//
	// Three assertions, and the third is what stops the case being vacuous:
	// a BIND on a container is refused, a CLEAR on a container is accepted,
	// and a BIND on a LEAF is still accepted (so the gate is discriminating,
	// not just refusing every material edit).
	// =================================================================
	{
		const char* sV = "sg_parent_agent_bind.RISEscene";
		WriteScene( sV,
			"uniformcolor_painter\n{\nname pv\ncolor 0 1 0\n}\n"
			"lambertian_material\n{\nname mv\nreflectance pv\n}\n"
			"lambertian_material\n{\nname mv2\nreflectance pv\n}\n"
			"sphere_geometry\n{\nname gv\nradius 1\n}\n"
			"standard_object\n{\nname hollow\nposition 0 2 0\nmaterial mv\n}\n"
			// `geometry none` is the OTHER spelling of "container".  A gate that
			// tested only for the PRESENCE of a `geometry` line would call this
			// object a leaf while every other layer calls it a container.
			"standard_object\n{\nname sentinel\ngeometry none\nposition 0 4 0\n}\n"
			"standard_object\n{\nname solid\ngeometry gv\nparent hollow\n}\n"
			"standard_object\n{\nname cutA\ngeometry gv\n}\n"
			"standard_object\n{\nname cutB\ngeometry gv\nposition 0.5 0 0\n}\n"
			"csg_object\n{\nname carved\nobja cutA\nobjb cutB\noperation subtraction\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sV ), "V: scene loads" );
		SceneEditController c( *j, 0 );

		auto ChunkOf = []( const RISE::Cst::Document& d, const char* objName ) -> std::string {
			const std::string text = RISE::Cst::SerializeCst( d );
			const std::string key  = std::string( "name " ) + objName;
			size_t at = text.find( key );
			while( at != std::string::npos ) {
				const size_t after = at + key.size();
				if( after >= text.size() || text[after] == '\n' || text[after] == '\r' ) break;
				at = text.find( key, after );
			}
			if( at == std::string::npos ) return std::string();
			const size_t end = text.find( "\n}", at );
			return text.substr( at, ( end == std::string::npos ? text.size() : end ) - at );
		};
		auto ParamValue = []( const std::string& chunk, const char* key ) -> std::string {
			const std::string needle = std::string( "\n" ) + key + " ";
			std::string out;
			size_t p = chunk.find( needle );
			while( p != std::string::npos ) {
				const size_t vs = p + needle.size();
				const size_t ve = chunk.find( '\n', vs );
				out = chunk.substr( vs, ( ve == std::string::npos ? chunk.size() : ve ) - vs );
				p = chunk.find( needle, vs );
			}
			return out;
		};

		// 1. BIND onto the container -> refused, and the Document is untouched.
		{
			const SceneEditController::AgentCommitResult r = c.ApplyAgentParamEdit(
				String( "hollow" ), String( "standard_object" ),
				String( "material" ), String( "mv2" ), /*baseVersionOrNull*/ 0 );
			Check( !r.applied, "V: the agent's BIND onto a container is refused" );
			Check( r.status == String( "rejected" ),
			       "V: ... reported as `rejected`, not silently applied" );
			Check( std::string( r.message.c_str() ).find( "container" ) != std::string::npos,
			       "V: ... with a message that names the cause" );
			const RISE::Cst::Document* d = j->GetCstDocument();
			Check( d != 0, "V: (sanity) the Document is retained" );
			if( d ) {
				Check( ParamValue( ChunkOf( *d, "hollow" ), "material" ) != "mv2",
				       "V: and the refused bind wrote nothing -- the container's chunk did not gain it" );
			}
		}

		// 1b. Same, on the `geometry none` spelling.
		{
			const SceneEditController::AgentCommitResult r = c.ApplyAgentParamEdit(
				String( "sentinel" ), String( "standard_object" ),
				String( "material" ), String( "mv2" ), /*baseVersionOrNull*/ 0 );
			Check( !r.applied,
			       "V: `geometry none` is a container too -- the BIND is refused there as well" );
			Check( r.status == String( "rejected" ),
			       "V: ... as a `rejected`, not a resolution failure" );
			Check( std::string( r.message.c_str() ).find( "container" ) != std::string::npos,
			       "V: ... naming the container as the cause, so this cannot pass because the chunk "
			       "merely failed to resolve" );
			const RISE::Cst::Document* d = j->GetCstDocument();
			Check( d != 0, "V: (sanity) the Document is retained" );
			if( d ) {
				Check( ParamValue( ChunkOf( *d, "sentinel" ), "material" ) != "mv2",
				       "V: ... and wrote nothing" );
			}
		}

		// 2. The SAME edit onto the LEAF child is accepted.  Without this the
		//    case would pass against a gate that refuses every material edit.
		{
			const SceneEditController::AgentCommitResult r = c.ApplyAgentParamEdit(
				String( "solid" ), String( "standard_object" ),
				String( "material" ), String( "mv2" ), /*baseVersionOrNull*/ 0 );
			Check( r.applied, "V: the same bind onto a LEAF object IS accepted" );
			const RISE::Cst::Document* d = j->GetCstDocument();
			Check( d != 0, "V: (sanity) the Document is retained" );
			if( d ) {
				Check( ParamValue( ChunkOf( *d, "solid" ), "material" ) == "mv2",
				       "V: ... and landed in the Document" );
			}
		}

		// 3. A `csg_object` is NOT a container -- it takes its shape from its
		//    operands, so it names no `geometry` either, and a container test
		//    that keyed on "no geometry param" alone would refuse every
		//    material edit on every composite in the scene.  The role check is
		//    what separates them, and this is what holds it in place.
		{
			const SceneEditController::AgentCommitResult r = c.ApplyAgentParamEdit(
				String( "carved" ), String( "csg_object" ),
				String( "material" ), String( "mv2" ), /*baseVersionOrNull*/ 0 );
			Check( r.applied,
			       "V: a bind onto a `csg_object` IS accepted -- it names no `geometry`, but it is a "
			       "composite shape, not a container" );
			const RISE::Cst::Document* d = j->GetCstDocument();
			Check( d != 0, "V: (sanity) the Document is retained" );
			if( d ) {
				Check( ParamValue( ChunkOf( *d, "carved" ), "material" ) == "mv2",
				       "V: ... and landed in the Document" );
			}
		}

		// 4. A CLEAR on the container is accepted, so the stale `material mv`
		//    the scene authored can actually be removed.
		{
			const SceneEditController::AgentCommitResult r = c.ApplyAgentParamEdit(
				String( "hollow" ), String( "standard_object" ),
				String( "material" ), String( "none" ), /*baseVersionOrNull*/ 0 );
			Check( r.applied, "V: an explicit CLEAR on the container IS accepted" );
			const RISE::Cst::Document* d = j->GetCstDocument();
			Check( d != 0, "V: (sanity) the Document is retained" );
			if( d ) {
				Check( ParamValue( ChunkOf( *d, "hollow" ), "material" ) != "mv",
				       "V: ... and the stale binding is gone from the Document" );
			}
		}

		// 5. UNDOING that permitted clear RESTORES the binding, and must.  The
		//    forward gate exists to stop the agent CREATING this state; undo is
		//    a history replay of a state that already existed, and gating it
		//    wedges the stack -- a refused revert is pushed back onto the undo
		//    stack and re-fails on every later Cmd-Z, stranding everything
		//    older.  See the block comment above SceneEditor::ApplyRevertMutation.
		{
			c.Undo();
			const RISE::Cst::Document* d = j->GetCstDocument();
			Check( d != 0, "V: (sanity) the Document survived the undo" );
			if( d ) {
				Check( ParamValue( ChunkOf( *d, "hollow" ), "material" ) == "mv",
				       "V: undoing the permitted clear RESTORED the prior binding -- history replay is "
				       "exempt from the creation gate, so the undo stack cannot wedge" );
			}
		}

		// 6. INSERTING a whole container chunk that names a binding is refused
		//    too.  This is the entry point the param-edit gate can never see --
		//    and writing a whole object chunk is the most likely way an agent
		//    produces this shape in the first place.
		{
			const SceneEditController::AgentCommitResult r = c.ApplyAgentInsertChunk(
				String( "standard_object\n{\nname ghost\nposition 0 8 0\nmaterial mv\n}\n" ),
				/*baseVersionOrNull*/ 0 );
			Check( !r.applied, "V: inserting a container chunk that names a material is refused" );
			const RISE::Cst::Document* d = j->GetCstDocument();
			Check( d != 0, "V: (sanity) the Document is retained" );
			if( d ) {
				Check( ChunkOf( *d, "ghost" ).empty(),
				       "V: ... and nothing was inserted" );
			}
		}
		// 6b. LAST OCCURRENCE WINS, because that is what the parser does
		//     (ParseStateBag::SetSingle is an unconditional overwrite, and
		//     nothing refuses a duplicated non-repeatable param).  Reading the
		//     FIRST occurrence instead makes the gate a two-line bypass in one
		//     direction and a false refusal in the other -- so both are here.
		{
			const SceneEditController::AgentCommitResult r = c.ApplyAgentInsertChunk(
				String( "standard_object\n{\nname dupe\nmaterial none\nmaterial mv\n}\n" ),
				/*baseVersionOrNull*/ 0 );
			Check( !r.applied,
			       "V: a container whose LAST `material` is a real bind is refused, even though its "
			       "first reads as a clear" );
			const RISE::Cst::Document* d = j->GetCstDocument();
			Check( d != 0, "V: (sanity) the Document is retained" );
			if( d ) Check( ChunkOf( *d, "dupe" ).empty(), "V: ... and nothing was inserted" );
		}
		{
			const SceneEditController::AgentCommitResult r = c.ApplyAgentInsertChunk(
				String( "standard_object\n{\nname undupe\ngeometry none\ngeometry gv\nmaterial mv\n}\n" ),
				/*baseVersionOrNull*/ 0 );
			Check( r.applied,
			       "V: and one whose LAST `geometry` resolves is a LEAF, so its material is accepted -- "
			       "reading the first would have refused a perfectly ordinary object" );
			const RISE::Cst::Document* d = j->GetCstDocument();
			Check( d != 0, "V: (sanity) the Document is retained" );
			if( d ) Check( ParamValue( ChunkOf( *d, "undupe" ), "material" ) == "mv",
			               "V: ... and it landed" );
		}

		// The SAME chunk with a geometry is accepted -- the insert gate is about
		// the container-ness, not about naming a material.
		{
			const SceneEditController::AgentCommitResult r = c.ApplyAgentInsertChunk(
				String( "standard_object\n{\nname real\ngeometry gv\nposition 0 8 0\nmaterial mv\n}\n" ),
				/*baseVersionOrNull*/ 0 );
			Check( r.applied, "V: the same chunk WITH a geometry inserts fine" );
			const RISE::Cst::Document* d = j->GetCstDocument();
			Check( d != 0, "V: (sanity) the Document is retained" );
			if( d ) {
				Check( ParamValue( ChunkOf( *d, "real" ), "material" ) == "mv",
				       "V: ... carrying its material" );
			}
		}

		j->release();
		std::remove( sV );
	}

	// =================================================================
	// W -- UNDO IS LOSSLESS AND CANNOT WEDGE, on the agent path.
	//
	// This is the sequence that killed the gate two rounds put on the revert
	// path.  Both agent edits are permitted; both are the ones the design
	// intends to permit.  What must hold at the end is that TWO Cmd-Zs return
	// the scene to exactly what was authored -- geometry back, material back,
	// and the material LIVE again now that the object is a leaf.
	//
	// Under the gated revert, step 4's first undo was refused, PopForUndo
	// pushed its record back, and every later undo re-popped and re-failed:
	// step 3 and everything older became permanently unreachable.
	// =================================================================
	{
		const char* sW = "sg_parent_agent_undo_lossless.RISEscene";
		WriteScene( sW,
			"uniformcolor_painter\n{\nname pw\ncolor 0 0 1\n}\n"
			"lambertian_material\n{\nname mw\nreflectance pw\n}\n"
			"sphere_geometry\n{\nname gw\nradius 1\n}\n"
			"standard_object\n{\nname swap\ngeometry gw\nmaterial mw\n}\n" );
		Job* j = new Job();
		Check( j->LoadAsciiSceneViaCst( sW ), "W: scene loads" );
		SceneEditController c( *j, 0 );

		auto ChunkOf = []( const RISE::Cst::Document& d, const char* objName ) -> std::string {
			const std::string text = RISE::Cst::SerializeCst( d );
			const std::string key  = std::string( "name " ) + objName;
			size_t at = text.find( key );
			while( at != std::string::npos ) {
				const size_t after = at + key.size();
				if( after >= text.size() || text[after] == '\n' || text[after] == '\r' ) break;
				at = text.find( key, after );
			}
			if( at == std::string::npos ) return std::string();
			const size_t end = text.find( "\n}", at );
			return text.substr( at, ( end == std::string::npos ? text.size() : end ) - at );
		};
		auto ParamValue = []( const std::string& chunk, const char* key ) -> std::string {
			const std::string needle = std::string( "\n" ) + key + " ";
			std::string out;
			size_t p = chunk.find( needle );
			while( p != std::string::npos ) {
				const size_t vs = p + needle.size();
				const size_t ve = chunk.find( '\n', vs );
				out = chunk.substr( vs, ( ve == std::string::npos ? chunk.size() : ve ) - vs );
				p = chunk.find( needle, vs );
			}
			return out;
		};

		// Positive control FIRST: the object starts as a leaf carrying `mw`.
		{
			IObjectPriv* sw = Obj( *j, "swap" );
			Check( sw && sw->GetGeometry() != 0 && sw->GetMaterial() != 0,
			       "W: (baseline) the authored object is a leaf with a live material" );
		}
		// 1. Make it a container.  `geometry` is not a surface binding, so this
		//    is permitted -- and it is how an agent reaches the state at all.
		{
			const SceneEditController::AgentCommitResult r = c.ApplyAgentParamEdit(
				String( "swap" ), String( "standard_object" ),
				String( "geometry" ), String( "none" ), /*baseVersionOrNull*/ 0 );
			Check( r.applied, "W: `geometry none` applies -- it is not a surface binding" );
			IObjectPriv* sw = Obj( *j, "swap" );
			Check( sw && sw->GetGeometry() == 0, "W: the object is now a container" );
		}
		// 2. Clear the now-stale material.  Permitted by the CLEAR carve-out.
		{
			const SceneEditController::AgentCommitResult r = c.ApplyAgentParamEdit(
				String( "swap" ), String( "standard_object" ),
				String( "material" ), String( "none" ), /*baseVersionOrNull*/ 0 );
			Check( r.applied, "W: clearing the stale binding off the container is accepted" );
		}
		// 3. Cmd-Z, Cmd-Z.  Back to exactly what was authored.
		c.Undo();
		c.Undo();
		{
			const RISE::Cst::Document* d = j->GetCstDocument();
			Check( d != 0, "W: (sanity) the Document survived two undos" );
			if( d ) {
				const std::string chunk = ChunkOf( *d, "swap" );
				const std::string geom = ParamValue( chunk, "geometry" );
				const std::string mat  = ParamValue( chunk, "material" );
				if( geom != "gw" || mat != "mw" )
					std::cout << "    (W: chunk text was:\n" << chunk << "\n)" << std::endl;
				Check( geom == "gw", "W: the second undo restored the `geometry` -- the stack did NOT wedge" );
				Check( mat  == "mw", "W: and the first undo restored the `material`" );
			}
			IObjectPriv* sw = Obj( *j, "swap" );
			Check( sw && sw->GetGeometry() != 0,
			       "W: the LIVE object is a leaf again" );
			Check( sw && sw->GetMaterial() != 0,
			       "W: ... carrying its material again -- undo was LOSSLESS.  A revert that skipped "
			       "the binding write to keep the container clean would leave this null forever" );
		}
		j->release();
		std::remove( sW );
	}

	std::cout << "  " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
