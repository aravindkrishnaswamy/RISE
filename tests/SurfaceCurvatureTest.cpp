//////////////////////////////////////////////////////////////////////
//
//  SurfaceCurvatureTest.cpp - Phase-1 exit gate for the geometry-derived
//  shading signals arc (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md §13).
//
//  What this pins, in the order the design doc's exit gate lists it:
//
//    (a) the SIGN CONVENTION -- an outward-normal unit sphere reports
//        H = +1/r, positive = convex -- on BOTH an analytic sphere hit
//        and a smooth-shaded tessellated mesh sphere.  This is the one
//        thing the design doc insists must be pinned by a test rather
//        than asserted in a comment, because every downstream mask
//        (`clamp(curv,0,1)` = wear, `clamp(-curv,0,1)` = crevice) is
//        built on it.
//    (b) a FLAT surface reads zero -- both a genuinely flat one that
//        publishes derivatives (a capped cylinder's end cap) and a
//        primitive that publishes none at all (box), which must read 0
//        as an ABSENCE rather than produce a NaN.
//    (c) the SCALED-SPHERE transform: H_world == H_obj / s.  Doubles as
//        a regression guard for the 2026-08-29 quotient-rule fix
//        (commits 3f495c25..da92af3e), whose pre-fix bug gave H_obj/s².
//    (d) `curv` INVARIANCE across two instances of one geometry at
//        different world scales -- the whole point of the scaleHint
//        normalization.
//    (e) `curv` INVARIANCE under an applied normal-perturbing
//        modifier (relief) -- design doc §14
//        item 6, a REQUIREMENT: a wear mask must follow the form, not
//        the texture.
//    (f) DEGENERATE parameterizations return the invalid path, never a
//        NaN.
//    (g) the SDF family's DIRECT curvature (div n̂ / 2) ≈ 1/r, and the
//        CONSUMPTION GATE that keeps its ~18 extra field evaluations
//        per hit off every scene that doesn't ask for curvature.
//    (h) the whole thing END TO END through the painter API:
//        `scalar_painter { expression "curv" }` on a sphere.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <vector>
#include <string>

#include "../src/Library/Utilities/SurfaceCurvature.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/BoxGeometry.h"
#include "../src/Library/Geometry/CylinderGeometry.h"
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Modifiers/ReliefModifier.h"
#include "../src/Library/Painters/Function2DScalarPainter.h"
#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/Painters/ExpressionPainter.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const std::string& name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

static void CheckClose( Scalar got, Scalar want, Scalar tol, const std::string& name )
{
	if( std::fabs( got - want ) <= tol ) { ++passCount; }
	else {
		++failCount;
		std::cout.precision( 12 );
		std::cout << "  FAIL: " << name << "  got " << got << "  want " << want
			<< "  |d| " << std::fabs( got - want ) << "  (tol " << tol << ")" << std::endl;
	}
}

//======================================================================
// Helpers
//======================================================================

//! Fire a WORLD-space ray at an object.  RayIntersection has no default
//! constructor (it carries the ray), so the caller builds the record with
//! MkRI below and this only drives the cast.
static RayIntersection MkRI( const Point3& origin, const Vector3& dir )
{
	return RayIntersection( Ray( origin, dir ), nullRasterizerState );
}

static bool HitObject( const Object* obj, RayIntersection& ri )
{
	obj->IntersectRay( ri, RISE_INFINITY, true, true, false );
	return ri.geometric.bHit;
}

//! The curvature the expression VM would see at this hit -- i.e. exactly
//! what ExpressionPainter::BuildContext computes, expressed here in terms
//! of the same public helper so the test cannot drift from the painter and
//! still cannot merely restate it (test (h) drives the real painter).
static bool RecordH( const RayIntersectionGeometric& ri, Scalar& outH )
{
	if( ri.derivatives.curvatureValid ) { outH = ri.derivatives.curvature; return true; }
	if( ri.derivatives.valid ) {
		return SurfaceCurvature::MeanCurvatureFromDerivatives(
			ri.derivatives.dpdu, ri.derivatives.dpdv,
			ri.derivatives.dndu, ri.derivatives.dndv, outH );
	}
	return false;
}

//! Smooth-shaded tessellated sphere, built through SphereGeometry's own
//! tessellator so the vertex normals are the analytic ones (a face-normal
//! mesh would give dndu = dndv = 0 identically and test nothing).  Same
//! construction GeometryUVRoundtripTest uses.
static TriangleMeshGeometryIndexed* BuildTessellatedSphereMesh( Scalar radius, unsigned int detail )
{
	SphereGeometry* g = new SphereGeometry( radius );
	IndexTriangleListType tris;
	VerticesListType verts;
	NormalsListType norms;
	TexCoordsListType coords;
	const bool ok = g->TessellateToMesh( tris, verts, norms, coords, detail );
	g->release();
	if( !ok ) return 0;

	TriangleMeshGeometryIndexed* pMesh = new TriangleMeshGeometryIndexed( true, false );
	pMesh->BeginIndexedTriangles();
	pMesh->AddVertices( verts );
	pMesh->AddNormals( norms );
	pMesh->AddTexCoords( coords );
	pMesh->AddIndexedTriangles( tris );
	pMesh->DoneIndexedTriangles();
	return pMesh;
}

//! A trivial non-constant IFunction2D for the relief test.  Non-constant
//! matters: a constant field has zero gradient, so the modifier would
//! leave vNormal untouched and test (e) would pass vacuously.
class RampField : public virtual IFunction2D, public virtual Reference
{
protected:
	virtual ~RampField() {}
public:
	Scalar Evaluate( const Scalar x, const Scalar y ) const override
	{
		return std::sin( Scalar( 12.0 ) * x ) * std::cos( Scalar( 9.0 ) * y );
	}
};

//! Compile one expression body with the full 3D context enabled -- the
//! same configuration expression_painter / scalar_painter{expression} use.
static bool CompileWithContext( const std::string& body, ExpressionProgram& out )
{
	ExpressionProgram::Builder b;
	b.EnableContextVars( true );
	return b.Finalize( body, out );
}

//======================================================================
// (a) Sign convention: outward-normal sphere -> H = +1/r
//======================================================================

static void TestAnalyticSphereSignAndMagnitude()
{
	std::cout << "(a) analytic sphere: H == +1/r, positive == convex" << std::endl;

	const Scalar r = 2.5;
	SphereGeometry* g = new SphereGeometry( r );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	// Deliberately OFF-AXIS: a ray down the polar axis lands on the
	// parameterization's singular point, where dpdu collapses and the
	// shape operator is legitimately undefined.
	RayIntersection ri = MkRI( Point3( 0.4, 0.7, 20.0 ), Vector3( 0, 0, -1 ) );
	const bool hit = HitObject( o, ri );
	Check( hit, "(a) analytic sphere hit" );
	Check( ri.geometric.derivatives.valid, "(a) analytic sphere publishes derivatives at intersection time" );

	Scalar H = 0;
	Check( RecordH( ri.geometric, H ), "(a) analytic sphere curvature well-defined" );
	Check( H > 0, "(a) analytic sphere H is POSITIVE (outward normal => convex)" );
	CheckClose( H, 1.0 / r, 1e-9, "(a) analytic sphere H == 1/r exactly (closed form)" );

	o->release();
}

static void TestMeshSphereSignAndMagnitude()
{
	std::cout << "(a) mesh sphere: H ~= +1/r, positive == convex" << std::endl;

	const Scalar r = 1.5;
	TriangleMeshGeometryIndexed* mesh = BuildTessellatedSphereMesh( r, 40 );
	Check( mesh != 0, "(a) tessellated sphere mesh built" );
	if( !mesh ) return;

	Object* o = new Object( mesh );
	mesh->release();
	o->FinalizeTransformations();

	RayIntersection ri = MkRI( Point3( 0.35, 0.22, 20.0 ), Vector3( 0, 0, -1 ) );
	const bool hit = HitObject( o, ri );
	Check( hit, "(a) mesh sphere hit" );
	Check( ri.geometric.derivatives.valid, "(a) mesh sphere publishes derivatives" );

	Scalar H = 0;
	Check( RecordH( ri.geometric, H ), "(a) mesh sphere curvature well-defined" );
	Check( H > 0, "(a) mesh sphere H is POSITIVE -- the mesh path shares the sign convention" );
	// Discretized: 5% is the same band GeometryUVRoundtripTest uses for
	// this construction at detail=40.
	CheckClose( H, 1.0 / r, 0.05 / r, "(a) mesh sphere H ~= 1/r" );

	o->release();
}

//======================================================================
// (b) Flat reads zero; a primitive that publishes nothing reads zero
//     as an ABSENCE (and never a NaN)
//======================================================================

static void TestFlatSurfacesReadZero()
{
	std::cout << "(b) flat surfaces read 0 / honest absence" << std::endl;

	// A CAPPED CYLINDER's end cap publishes real derivatives whose dndu /
	// dndv are identically zero -- a genuinely flat surface that IS
	// described, so H must come out exactly 0 rather than invalid.
	{
		CylinderGeometry* g = new CylinderGeometry( 'y', 1.0, 2.0, true );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();

		RayIntersection ri = MkRI( Point3( 0.3, 20.0, 0.2 ), Vector3( 0, -1, 0 ) );
		// Straight down onto the +Y cap, off the axis so we are on the cap
		// interior rather than its rim.
		const bool hit = HitObject( o, ri );
		Check( hit, "(b) cylinder cap hit" );
		Check( ri.geometric.derivatives.valid, "(b) cylinder cap publishes derivatives" );
		Scalar H = 1234.0;
		Check( RecordH( ri.geometric, H ), "(b) cylinder cap curvature well-defined" );
		CheckClose( H, 0.0, 1e-12, "(b) cylinder cap H == 0 (flat)" );

		// ...and the side wall of the SAME primitive is genuinely curved,
		// so the zero above is a property of the cap and not of the wiring.
		RayIntersection riSide = MkRI( Point3( 20.0, 0.0, 0.0 ), Vector3( -1, 0, 0 ) );
		const bool hitSide = HitObject( o, riSide );
		Check( hitSide, "(b) cylinder side-wall hit" );
		Scalar Hs = 0;
		Check( RecordH( riSide.geometric, Hs ), "(b) cylinder side-wall curvature well-defined" );
		// A cylinder of radius r has k1 = 1/r, k2 = 0 => H = 1/(2r).
		CheckClose( Hs, 0.5 / 1.0, 1e-9, "(b) cylinder side wall H == 1/(2r)" );
		Check( Hs > 0, "(b) cylinder side wall H POSITIVE (convex)" );

		o->release();
	}

	// A BOX publishes no derivatives at all -- correct, it is flat, but the
	// point here is the CONTRACT: valid stays false, curvatureValid stays
	// false, and every consumer reads the documented 0 rather than a NaN.
	{
		BoxGeometry* g = new BoxGeometry( 2.0, 2.0, 2.0 );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();

		RayIntersection ri = MkRI( Point3( 0.1, 0.2, 20.0 ), Vector3( 0, 0, -1 ) );
		const bool hit = HitObject( o, ri );
		Check( hit, "(b) box hit" );
		Check( !ri.geometric.derivatives.valid, "(b) box publishes NO derivatives (honest absence)" );
		Check( !ri.geometric.derivatives.curvatureValid, "(b) box publishes NO direct curvature" );
		Scalar H = 999.0;
		Check( !RecordH( ri.geometric, H ), "(b) box curvature reports UNAVAILABLE, not a fabricated value" );

		o->release();
	}
}

//======================================================================
// (c) Scaled sphere: H_world == H_obj / s
//======================================================================

static void TestScaledSphereTransform()
{
	std::cout << "(c) scaled sphere: H_world == H_obj / s" << std::endl;

	const Scalar r = 2.0;
	const Scalar s = 2.0;

	Scalar H_identity = 0;
	{
		SphereGeometry* g = new SphereGeometry( r );
		Object* o = new Object( g );
		g->release();
		o->FinalizeTransformations();
		RayIntersection ri = MkRI( Point3( 0.4, 0.5, 20.0 ), Vector3( 0, 0, -1 ) );
		Check( HitObject( o, ri ), "(c) identity sphere hit" );
		Check( RecordH( ri.geometric, H_identity ), "(c) identity sphere H defined" );
		o->release();
	}

	Scalar H_scaled = 0;
	{
		SphereGeometry* g = new SphereGeometry( r );
		Object* o = new Object( g );
		g->release();
		o->SetScale( s );
		o->FinalizeTransformations();
		RayIntersection ri = MkRI( Point3( 0.4 * s, 0.5 * s, 40.0 ), Vector3( 0, 0, -1 ) );
		// The scaled sphere has world radius r*s; aim at the same RELATIVE
		// spot so we are comparing the same surface point.
		Check( HitObject( o, ri ), "(c) scaled sphere hit" );
		Check( RecordH( ri.geometric, H_scaled ), "(c) scaled sphere H defined" );
		o->release();
	}

	CheckClose( H_scaled, 1.0 / ( r * s ), 1e-9, "(c) scaled sphere H == 1/(r*s)" );
	const Scalar ratio = H_identity / H_scaled;
	CheckClose( ratio, s, 1e-9, "(c) H_identity / H_scaled == s (quotient-rule transform)" );
	Check( std::fabs( ratio - s * s ) > 1e-6,
		"(c) ratio is NOT the pre-2026-08-29 buggy s^2" );
}

//======================================================================
// (d) `curv` is invariant across instances at different world scales
//======================================================================

//! Drive the FULL painter path, which is where scaleHint is actually
//! consumed.  Returns the scalar `curv` an expression would read.
static bool CurvThroughPainter( const Object* obj, const Point3& origin, const Vector3& dir,
	const std::string& body, Scalar& outValue )
{
	ExpressionProgram prog = ExpressionProgram::Invalid();
	if( !CompileWithContext( body, prog ) ) return false;

	std::vector<ParamSpec> specs;
	ExpressionScalarPainter* painter = new ExpressionScalarPainter( prog, specs );

	RayIntersection ri = MkRI( origin, dir );
	const bool hit = HitObject( obj, ri );
	if( hit ) {
		outValue = painter->GetValuesAt( ri.geometric ).v[0];
	}
	painter->release();
	return hit;
}

static void TestCurvInvariantAcrossInstanceScales()
{
	std::cout << "(d) curv invariant across two instances of one geometry at different scales" << std::endl;

	const Scalar r = 1.25;
	// ONE shared geometry, two objects -- the instancing case scaleHint's
	// world-scale fold exists for.
	SphereGeometry* g = new SphereGeometry( r );

	Object* oA = new Object( g );
	oA->FinalizeTransformations();

	Object* oB = new Object( g );
	oB->SetScale( 4.0 );
	oB->FinalizeTransformations();
	g->release();

	Scalar curvA = 0, curvB = 0;
	Check( CurvThroughPainter( oA, Point3( 0.2, 0.3, 20.0 ), Vector3( 0, 0, -1 ), "curv", curvA ),
		"(d) instance A hit through painter" );
	Check( CurvThroughPainter( oB, Point3( 0.8, 1.2, 40.0 ), Vector3( 0, 0, -1 ), "curv", curvB ),
		"(d) instance B hit through painter" );

	Check( curvA > 0, "(d) curv positive on a convex sphere" );
	CheckClose( curvB, curvA, 1e-9, "(d) curv IDENTICAL at scale 1 and scale 4" );

	// And the raw variant is deliberately NOT invariant -- that is the whole
	// reason two variables exist.  Without this the check above could pass
	// on a `curv` that was accidentally wired to a constant.
	Scalar curvRA = 0, curvRB = 0;
	Check( CurvThroughPainter( oA, Point3( 0.2, 0.3, 20.0 ), Vector3( 0, 0, -1 ), "curvR", curvRA ),
		"(d) instance A curvR" );
	Check( CurvThroughPainter( oB, Point3( 0.8, 1.2, 40.0 ), Vector3( 0, 0, -1 ), "curvR", curvRB ),
		"(d) instance B curvR" );
	CheckClose( curvRA, 1.0 / r, 1e-9, "(d) curvR == 1/r at scale 1" );
	CheckClose( curvRB, 1.0 / ( r * 4.0 ), 1e-9, "(d) curvR == 1/(4r) at scale 4 -- raw stays physical" );

	oA->release();
	oB->release();
}

//======================================================================
// (e) `curv` is invariant under an applied normal-perturbing modifier
//     (design doc §14.6)
//======================================================================

static void TestCurvInvariantUnderNormalPerturbation()
{
	std::cout << "(e) curv invariant under an applied relief modifier" << std::endl;

	const Scalar r = 2.0;
	SphereGeometry* g = new SphereGeometry( r );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	// The UV-domain relief modifier over the ramp -- the shape the removed
	// `bumpmap_modifier` chunk now lowers to (2026-09-06, design 7.5), with
	// its amplitude fold applied to the (0.75, 0.01, normalize FALSE) this
	// case used to pass: S' = -0.75*2*0.01 = -0.015.  What matters to (e) is
	// only that the shading normal MOVES, which the assertion below pins.
	RampField* field = new RampField();
	Function2DScalarPainter* height = new Function2DScalarPainter( field );
	field->release();
	ReliefModifier* bump = new ReliefModifier( *height, -0.015, ReliefDomain::UV, 0.01 );
	height->release();

	ExpressionProgram prog = ExpressionProgram::Invalid();
	Check( CompileWithContext( "curv", prog ), "(e) `curv` compiles" );
	std::vector<ParamSpec> specs;
	ExpressionScalarPainter* painter = new ExpressionScalarPainter( prog, specs );

	RayIntersection ri = MkRI( Point3( 0.4, 0.6, 20.0 ), Vector3( 0, 0, -1 ) );
	Check( HitObject( o, ri ), "(e) sphere hit" );

	const Scalar before = painter->GetValuesAt( ri.geometric ).v[0];
	const Vector3 nBefore = ri.geometric.vNormal;

	// Apply the modifier exactly as RayCaster does, in place, before
	// material shading.
	bump->Modify( ri.geometric );

	const Vector3 nAfter = ri.geometric.vNormal;
	const Scalar after = painter->GetValuesAt( ri.geometric ).v[0];

	// The modifier must have actually MOVED the shading normal, or the
	// invariance below is vacuous.
	const Scalar normalMove = std::fabs( nBefore.x - nAfter.x )
		+ std::fabs( nBefore.y - nAfter.y ) + std::fabs( nBefore.z - nAfter.z );
	Check( normalMove > 1e-6, "(e) the relief modifier genuinely perturbed vNormal (non-vacuous)" );

	CheckClose( after, before, 1e-12, "(e) curv UNCHANGED by the relief modifier" );
	Check( before > 0, "(e) curv still reads convex after the perturbation" );

	painter->release();
	bump->release();
	o->release();
}

//======================================================================
// (f) Degenerate parameterizations: invalid, never NaN
//======================================================================

static void TestDegenerateParameterization()
{
	std::cout << "(f) degenerate parameterizations return invalid, never NaN" << std::endl;

	Scalar H = 1234.5;
	const Scalar sentinel = H;

	// Collapsed tangent (a pole).
	Check( !SurfaceCurvature::MeanCurvatureFromDerivatives(
		Vector3( 0, 0, 0 ), Vector3( 0, 1, 0 ), Vector3( 1, 0, 0 ), Vector3( 0, 1, 0 ), H ),
		"(f) zero dpdu -> invalid" );
	Check( H == sentinel, "(f) outH untouched on the zero-dpdu path" );

	// Parallel tangents (a pinched chart): E*G - F*F == 0.
	Check( !SurfaceCurvature::MeanCurvatureFromDerivatives(
		Vector3( 2, 0, 0 ), Vector3( 3, 0, 0 ), Vector3( 0, 1, 0 ), Vector3( 0, 1, 0 ), H ),
		"(f) parallel dpdu/dpdv -> invalid" );

	// NEARLY parallel -- below the relative gate, which is the case an
	// absolute NEARZERO compare would wrongly wave through on a large
	// object.  1e-9 of relative skew leaves det/(E*G) ~ 1e-18 << 1e-12.
	Check( !SurfaceCurvature::MeanCurvatureFromDerivatives(
		Vector3( 1000, 0, 0 ), Vector3( 1000, 1e-6, 0 ), Vector3( 0, 1, 0 ), Vector3( 0, 1, 0 ), H ),
		"(f) near-parallel tangents on a LARGE object -> invalid (relative gate)" );

	// The same relative skew on a WELL-CONDITIONED frame must still be
	// accepted, or the gate would be rejecting legitimate geometry.
	Check( SurfaceCurvature::MeanCurvatureFromDerivatives(
		Vector3( 1, 0, 0 ), Vector3( 0, 1, 0 ), Vector3( 1, 0, 0 ), Vector3( 0, 1, 0 ), H ),
		"(f) orthonormal frame accepted" );
	CheckClose( H, 1.0, 1e-12, "(f) orthonormal frame H == 1" );

	// A NaN anywhere in the inputs must come out as "invalid", not as a
	// NaN handed to a painter.
	const Scalar nan = std::numeric_limits<Scalar>::quiet_NaN();
	H = sentinel;
	Check( !SurfaceCurvature::MeanCurvatureFromDerivatives(
		Vector3( 1, 0, 0 ), Vector3( 0, 1, 0 ), Vector3( nan, 0, 0 ), Vector3( 0, 1, 0 ), H ),
		"(f) NaN in dndu -> invalid, not a NaN result" );
	Check( H == sentinel, "(f) outH untouched on the NaN path" );
}

//======================================================================
// (g) SDF direct curvature + the consumption gate
//======================================================================

static void TestSdfDirectCurvatureAndGate()
{
	std::cout << "(g) SDF direct curvature and the consumption gate" << std::endl;

	const Scalar R = 3.0;
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart(
		SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), R, 0, 0, 0 ) );

	SDFGeometry* g = new SDFGeometry( parts, 512, Scalar( 1e-5 ) );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	// --- THE GATE, closed.  No expression in this process is asking for
	//     curvature right now, so the SDF must not have paid for it.
	Check( !SurfaceCurvatureDemand::Any(),
		"(g) demand counter starts at zero (no live curvature-reading painter)" );
	{
		RayIntersection ri = MkRI( Point3( 0.5, 0.6, 30.0 ), Vector3( 0, 0, -1 ) );
		Check( HitObject( o, ri ), "(g) gated SDF hit" );
		Check( !ri.geometric.derivatives.curvatureValid,
			"(g) GATE CLOSED: SDF skipped the curvature FD entirely" );
		CheckClose( ri.geometric.derivatives.scaleHint, 1.0, 1e-12,
			"(g) GATE CLOSED: scaleHint left at its default too" );
	}

	// --- THE GATE, open.  A live registration is exactly what an
	//     ExpressionScalarPainter compiled from "curv" installs.
	{
		SurfaceCurvatureDemand::Registration demand( true );
		Check( SurfaceCurvatureDemand::Any(), "(g) demand counter opens the gate" );

		RayIntersection ri = MkRI( Point3( 0.5, 0.6, 30.0 ), Vector3( 0, 0, -1 ) );
		Check( HitObject( o, ri ), "(g) ungated SDF hit" );
		Check( ri.geometric.derivatives.curvatureValid, "(g) GATE OPEN: SDF published direct curvature" );
		Check( !ri.geometric.derivatives.valid,
			"(g) SDF leaves dpdu/dpdv invalid -- an implicit surface has no natural (u,v)" );

		Scalar H = 0;
		Check( RecordH( ri.geometric, H ), "(g) SDF curvature readable" );
		Check( H > 0, "(g) SDF sphere H POSITIVE (outward gradient normal => convex)" );
		// Finite-difference stencil on a sphere-traced surface: 2% is
		// generous but still an order of magnitude tighter than any wrong
		// answer (e.g. a missing /2 would be 100% off).
		CheckClose( H, 1.0 / R, 0.02 / R, "(g) SDF sphere H ~= 1/R (div n_hat / 2)" );

		// A missing factor of 2 is the single most likely wiring slip here,
		// so exclude it explicitly.
		Check( std::fabs( H - 2.0 / R ) > 0.1 / R, "(g) H is div/2, NOT div" );
	}

	Check( !SurfaceCurvatureDemand::Any(), "(g) demand released with the registration (RAII)" );
	o->release();
}

static void TestDemandRegistrationFollowsTheProgram()
{
	std::cout << "(g) demand registration keys off the COMPILED program" << std::endl;

	ExpressionProgram plain  = ExpressionProgram::Invalid();
	ExpressionProgram curvy  = ExpressionProgram::Invalid();
	ExpressionProgram viaDef = ExpressionProgram::Invalid();
	Check( CompileWithContext( "u * v + P.x", plain ), "(g) plain body compiles" );
	Check( CompileWithContext( "clamp(-curv, 0, 1)", curvy ), "(g) curvature body compiles" );

	Check( !plain.UsesSurfaceCurvature(), "(g) a body that never says curv does not demand it" );
	Check( curvy.UsesSurfaceCurvature(), "(g) a body that says curv demands it" );
	Check( curvy.UsesContextVar( ExpressionProgram::kContextSlotCurv ), "(g) curv slot recorded" );
	Check( !curvy.UsesContextVar( ExpressionProgram::kContextSlotCurvR ), "(g) curvR slot NOT recorded" );

	// A `def` stage counts -- the mask is accumulated across every body the
	// Builder compiled, not just the final expression.
	{
		ExpressionProgram::Builder b;
		b.EnableContextVars( true );
		Check( b.AddDef( "wear", "clamp(curv, 0, 1)" ), "(g) def compiles" );
		Check( b.Finalize( "wear * 0.5", viaDef ), "(g) program with a curv-reading def compiles" );
		Check( viaDef.UsesSurfaceCurvature(), "(g) a curv reference inside a `def` still demands it" );
	}

	// And a user `param` named `curv` SHADOWS the context var, so it must
	// not raise demand -- the mask is set only where the context slot is
	// actually emitted.
	{
		ExpressionProgram shadowed = ExpressionProgram::Invalid();
		ExpressionProgram::Builder b;
		b.EnableContextVars( true );
		Check( b.AddParam( "curv", 0.5 ), "(g) `param curv` accepted (shadowing is load-bearing back-compat)" );
		Check( b.Finalize( "curv * 2", shadowed ), "(g) shadowed program compiles" );
		Check( !shadowed.UsesSurfaceCurvature(),
			"(g) a user param named `curv` shadows the context var and raises NO demand" );
	}

	// The painters are what actually hold the demand in production.
	{
		Check( !SurfaceCurvatureDemand::Any(), "(g) counter clear before painter construction" );
		std::vector<ParamSpec> specs;
		ExpressionScalarPainter* p1 = new ExpressionScalarPainter( plain, specs );
		Check( !SurfaceCurvatureDemand::Any(), "(g) a plain painter raises no demand" );
		ExpressionScalarPainter* p2 = new ExpressionScalarPainter( curvy, specs );
		Check( SurfaceCurvatureDemand::Any(), "(g) a curvature painter raises demand" );
		p2->release();
		Check( !SurfaceCurvatureDemand::Any(), "(g) demand drops when that painter dies" );
		p1->release();
	}
}

//======================================================================
// (h) End to end through the painter API
//======================================================================

static void TestExpressionEndToEnd()
{
	std::cout << "(h) scalar_painter { expression \"curv\" } end to end on a sphere" << std::endl;

	const Scalar r = 2.0;
	SphereGeometry* g = new SphereGeometry( r );
	Object* o = new Object( g );
	g->release();
	o->FinalizeTransformations();

	// scaleHint for a sphere is its bbox diagonal, 2r*sqrt(3); H is 1/r; so
	// curv is 2*sqrt(3) INDEPENDENT of r -- which is exactly the "O(1) at
	// object scale" property the normalization exists to give.
	const Scalar expectedCurv = 2.0 * std::sqrt( 3.0 );

	Scalar curv = 0;
	Check( CurvThroughPainter( o, Point3( 0.3, 0.4, 20.0 ), Vector3( 0, 0, -1 ), "curv", curv ),
		"(h) painter evaluated at a sphere hit" );
	CheckClose( curv, expectedCurv, 1e-9, "(h) curv == 2*sqrt(3) on any sphere" );

	// Composition: the two documented mask idioms must behave.
	Scalar wear = 0, crevice = 0;
	Check( CurvThroughPainter( o, Point3( 0.3, 0.4, 20.0 ), Vector3( 0, 0, -1 ), "clamp(curv,0,1)", wear ),
		"(h) wear mask evaluated" );
	Check( CurvThroughPainter( o, Point3( 0.3, 0.4, 20.0 ), Vector3( 0, 0, -1 ), "clamp(-curv,0,1)", crevice ),
		"(h) crevice mask evaluated" );
	CheckClose( wear, 1.0, 1e-12, "(h) clamp(curv,0,1) saturates on a convex sphere" );
	CheckClose( crevice, 0.0, 1e-12, "(h) clamp(-curv,0,1) is 0 on a convex sphere" );

	// A radius change must NOT move `curv` (scale-free) but MUST move
	// `curvR` (physical).
	{
		SphereGeometry* g2 = new SphereGeometry( 10.0 );
		Object* o2 = new Object( g2 );
		g2->release();
		o2->FinalizeTransformations();
		Scalar curv2 = 0, curvR2 = 0;
		Check( CurvThroughPainter( o2, Point3( 1.0, 1.5, 40.0 ), Vector3( 0, 0, -1 ), "curv", curv2 ),
			"(h) larger sphere curv" );
		Check( CurvThroughPainter( o2, Point3( 1.0, 1.5, 40.0 ), Vector3( 0, 0, -1 ), "curvR", curvR2 ),
			"(h) larger sphere curvR" );
		CheckClose( curv2, expectedCurv, 1e-9, "(h) curv is radius-INDEPENDENT" );
		CheckClose( curvR2, 0.1, 1e-9, "(h) curvR == 1/10 on a radius-10 sphere" );
		o2->release();
	}

	// A body that never mentions curvature must be byte-unaffected.
	{
		Scalar plain = 0;
		Check( CurvThroughPainter( o, Point3( 0.3, 0.4, 20.0 ), Vector3( 0, 0, -1 ), "0.25", plain ),
			"(h) constant body evaluated" );
		CheckClose( plain, 0.25, 1e-15, "(h) a body with no curvature reference is unchanged" );
	}

	// expression_function2d's frozen UV-only surface must NOT see the new
	// names (design doc §14 item 7): context vars off => `curv` is an
	// ordinary unknown identifier and a hard compile error.
	{
		ExpressionProgram::Builder b;	// EnableContextVars deliberately NOT called
		ExpressionProgram out = ExpressionProgram::Invalid();
		Check( !b.Finalize( "curv", out ),
			"(h) `curv` is a compile ERROR without EnableContextVars (expression_function2d stays frozen)" );
		Check( b.Error().find( "unknown variable" ) != std::string::npos,
			"(h) ...and the diagnostic is the ordinary unknown-variable one" );
	}

	o->release();
}

//======================================================================

int main()
{
	std::cout << "SurfaceCurvatureTest -- geometry-derived shading signals, Phase 1" << std::endl << std::endl;

	TestAnalyticSphereSignAndMagnitude();
	TestMeshSphereSignAndMagnitude();
	TestFlatSurfacesReadZero();
	TestScaledSphereTransform();
	TestCurvInvariantAcrossInstanceScales();
	TestCurvInvariantUnderNormalPerturbation();
	TestDegenerateParameterization();
	TestSdfDirectCurvatureAndGate();
	TestDemandRegistrationFollowsTheProgram();
	TestExpressionEndToEnd();

	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
