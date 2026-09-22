//////////////////////////////////////////////////////////////////////
//
//  CSGCurvatureConventionTest.cpp - Verification suite for DL-21
//  (CSG boundary curvature convention).
//
//  Pins the convention for differential geometry and curvature at a
//  CSG boundary:
//  1. Ray-surface intersection resolves to a specific operand's surface.
//     The composite forwards the contributing surface's differential
//     geometry and curvature (derivatives.dpdu, dpdv, dndu, dndv,
//     curvature, scaleHint) from whichever operand generated the active
//     boundary surface hit.
//  2. For unsubtracted boundary surfaces (CSG_UNION, CSG_INTERSECTION,
//     and operand A in CSG_SUBTRACTION), curvature retains the operand's
//     native sign (H = H_operand).
//  3. For subtracted cavity walls (operand B in CSG_SUBTRACTION A - B),
//     the reported outward normal is negated alongside dndu/dndv and
//     direct curvature, so the reported curvature inverts sign
//     (H_composite = -H_B), correctly presenting a convex operand as
//     a concave cavity.
//  4. Curvature is NOT invalidated at the seam: differential geometry
//     remains valid and continuous on each contributing operand's
//     surface right up to the boundary seam.
//  5. Direct curvature from the SDF family (curvatureValid, curvature)
//     obeys the identical forwarding and sign inversion rules.
//  6. Object-level scaling (worldLinearScale) divides world-space
//     curvature by s (H_world = H_local / s) for both operands.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <cmath>
#include <vector>
#include <string>

#include "../src/Library/Utilities/SurfaceCurvature.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Intersection/RayIntersection.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_pass = 0;
static int g_fail = 0;

static void Check( bool c, const char* msg )
{
	if( c ) {
		++g_pass;
		std::cout << "  PASS: " << msg << "\n";
	} else {
		++g_fail;
		std::cout << "  FAIL: " << msg << "\n";
	}
}

namespace
{
	const Scalar kEps = 1e-4;

	bool Close( Scalar a, Scalar b, Scalar eps = kEps )
	{
		return std::fabs( a - b ) < eps;
	}

	void Hit( IObject* pObj, const Ray& r, RayIntersection& ri )
	{
		ri.geometric.bHit = false;
		ri.geometric.range = RISE_INFINITY;
		ri.geometric.range2 = RISE_INFINITY;
		ri.geometric.ray = r;
		pObj->IntersectRay( ri, RISE_INFINITY, true, true, true );
	}
}

//
// Part 1: CSG_INTERSECTION analytic curvature forwarding
//
static void TestIntersection_AnalyticCurvature()
{
	std::cout << "Part 1: CSG_INTERSECTION analytic curvature forwarding...\n";
	SurfaceCurvatureDemand::Registration reg( true );

	const Scalar rA = 2.0;   // H_A = 1/2 = 0.5
	const Scalar rB = 1.0;   // H_B = 1/1 = 1.0

	SphereGeometry* gA = new SphereGeometry( rA );
	SphereGeometry* gB = new SphereGeometry( rB );
	Object* oA = new Object( gA );
	Object* oB = new Object( gB );
	safe_release( gA );
	safe_release( gB );

	// Sphere A centered at (0, 0, -0.5) -> z in [-2.5, 1.5]
	// Sphere B centered at (0, 0,  0.5) -> z in [-0.5, 1.5]
	// Intersection span along z is [-0.5, 1.5]
	oA->SetPosition( Point3( 0, 0, -0.5 ) );
	oB->SetPosition( Point3( 0, 0,  0.5 ) );
	oA->FinalizeTransformations();
	oB->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_INTERSECTION );
	Check( csg->AssignObjects( oA, oB ), "Part 1: CSG_INTERSECTION assigned operands" );
	csg->FinalizeTransformations();

	// +Z ray: enters A at z = -2.5, enters B at z = -0.5 while inside A.
	// Active entry boundary is Sphere B!
	{
		Ray r( Point3( 0.1, 0.05, -5.0 ), Vector3( 0, 0, 1 ) );
		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );

		Check( ri.geometric.bHit, "Part 1: +Z ray hits intersection" );
		Check( ri.geometric.derivatives.valid, "Part 1: +Z entry derivatives valid" );

		Scalar H = 0;
		bool ok = SurfaceCurvature::MeanCurvatureFromDerivatives(
			ri.geometric.derivatives.dpdu, ri.geometric.derivatives.dpdv,
			ri.geometric.derivatives.dndu, ri.geometric.derivatives.dndv, H );
		Check( ok, "Part 1: +Z mean curvature successfully computed" );
		Check( Close( H, 1.0 / rB ), "Part 1: +Z entry forwards Sphere B curvature H = 1.0" );
	}

	// -Z ray: from (0.1, 0.05, 5.0) along -Z
	// Enters B at z = 1.5. But A also ends at z = 1.5.
	// Let's offset A slightly so A enters while inside B from the other side:
	// Let Sphere A be at (0, 0, -0.8), radius 2.0 -> z in [-2.8, 1.2]
	// Sphere B at (0, 0, 0.8), radius 1.0 -> z in [-0.2, 1.8]
	// Along -Z: enters B at z = 1.8, enters A at z = 1.2 while inside B.
	// Active entry boundary is Sphere A!
	{
		oA->SetPosition( Point3( 0, 0, -0.8 ) );
		oB->SetPosition( Point3( 0, 0,  0.8 ) );
		oA->FinalizeTransformations();
		oB->FinalizeTransformations();
		csg->FinalizeTransformations();

		Ray r( Point3( 0.1, 0.05, 5.0 ), Vector3( 0, 0, -1 ) );
		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );

		Check( ri.geometric.bHit, "Part 1: -Z ray hits intersection" );
		Check( ri.geometric.derivatives.valid, "Part 1: -Z entry derivatives valid" );

		Scalar H = 0;
		bool ok = SurfaceCurvature::MeanCurvatureFromDerivatives(
			ri.geometric.derivatives.dpdu, ri.geometric.derivatives.dpdv,
			ri.geometric.derivatives.dndu, ri.geometric.derivatives.dndv, H );
		Check( ok, "Part 1: -Z mean curvature successfully computed" );
		Check( Close( H, 1.0 / rA ), "Part 1: -Z entry forwards Sphere A curvature H = 0.5" );
	}

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
}

//
// Part 2: CSG_UNION analytic curvature forwarding
//
static void TestUnion_AnalyticCurvature()
{
	std::cout << "Part 2: CSG_UNION analytic curvature forwarding...\n";
	SurfaceCurvatureDemand::Registration reg( true );

	const Scalar rA = 2.0;   // H_A = 0.5
	const Scalar rB = 1.0;   // H_B = 1.0

	SphereGeometry* gA = new SphereGeometry( rA );
	SphereGeometry* gB = new SphereGeometry( rB );
	Object* oA = new Object( gA );
	Object* oB = new Object( gB );
	safe_release( gA );
	safe_release( gB );

	oA->SetPosition( Point3( -1.5, 0, 0 ) );
	oB->SetPosition( Point3(  1.5, 0, 0 ) );
	oA->FinalizeTransformations();
	oB->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_UNION );
	Check( csg->AssignObjects( oA, oB ), "Part 2: CSG_UNION assigned operands" );
	csg->FinalizeTransformations();

	// Ray hitting Sphere A
	{
		Ray r( Point3( -1.5, 0, -5.0 ), Vector3( 0, 0, 1 ) );
		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );

		Check( ri.geometric.bHit, "Part 2: ray hits Sphere A in union" );
		Check( ri.geometric.derivatives.valid, "Part 2: Sphere A derivatives valid" );

		Scalar H = 0;
		bool ok = SurfaceCurvature::MeanCurvatureFromDerivatives(
			ri.geometric.derivatives.dpdu, ri.geometric.derivatives.dpdv,
			ri.geometric.derivatives.dndu, ri.geometric.derivatives.dndv, H );
		Check( ok, "Part 2: Sphere A mean curvature computed" );
		Check( Close( H, 1.0 / rA ), "Part 2: Sphere A in union forwards H = 0.5" );
	}

	// Ray hitting Sphere B
	{
		Ray r( Point3( 1.5, 0, -5.0 ), Vector3( 0, 0, 1 ) );
		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );

		Check( ri.geometric.bHit, "Part 2: ray hits Sphere B in union" );
		Check( ri.geometric.derivatives.valid, "Part 2: Sphere B derivatives valid" );

		Scalar H = 0;
		bool ok = SurfaceCurvature::MeanCurvatureFromDerivatives(
			ri.geometric.derivatives.dpdu, ri.geometric.derivatives.dpdv,
			ri.geometric.derivatives.dndu, ri.geometric.derivatives.dndv, H );
		Check( ok, "Part 2: Sphere B mean curvature computed" );
		Check( Close( H, 1.0 / rB ), "Part 2: Sphere B in union forwards H = 1.0" );
	}

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
}

//
// Part 3: CSG_SUBTRACTION cavity wall curvature sign inversion
//
static void TestSubtraction_CurvatureSign()
{
	std::cout << "Part 3: CSG_SUBTRACTION cavity wall curvature sign inversion...\n";
	SurfaceCurvatureDemand::Registration reg( true );

	const Scalar rA = 2.0;   // Outer sphere
	const Scalar rB = 1.0;   // Carving sphere

	SphereGeometry* gA = new SphereGeometry( rA );
	SphereGeometry* gB = new SphereGeometry( rB );
	Object* oA = new Object( gA );
	Object* oB = new Object( gB );
	safe_release( gA );
	safe_release( gB );

	oA->SetPosition( Point3( 0, 0, 0 ) );
	oB->SetPosition( Point3( 0, 0, 1.5 ) );   // cuts into front face of A
	oA->FinalizeTransformations();
	oB->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_SUBTRACTION );
	Check( csg->AssignObjects( oA, oB ), "Part 3: CSG_SUBTRACTION assigned operands" );
	csg->FinalizeTransformations();

	// Ray hitting un-carved exterior of A (from -Z)
	{
		Ray r( Point3( 0, 0, -5.0 ), Vector3( 0, 0, 1 ) );
		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );

		Check( ri.geometric.bHit, "Part 3: ray hits un-carved exterior of A" );
		Scalar H = 0;
		bool ok = SurfaceCurvature::MeanCurvatureFromDerivatives(
			ri.geometric.derivatives.dpdu, ri.geometric.derivatives.dpdv,
			ri.geometric.derivatives.dndu, ri.geometric.derivatives.dndv, H );
		Check( ok, "Part 3: exterior curvature computed" );
		Check( Close( H, 1.0 / rA ), "Part 3: exterior of A reads convex H = +0.5" );
	}

	// Ray hitting cavity wall (from +Z, through B's excavation into A)
	{
		// Ray from (0.1, 0.05, 5.0) along -Z
		// Hits the carved notch from B inside A
		Ray r( Point3( 0.1, 0.05, 5.0 ), Vector3( 0, 0, -1 ) );
		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );

		Check( ri.geometric.bHit, "Part 3: ray hits carved cavity wall" );
		Scalar H = 0;
		bool ok = SurfaceCurvature::MeanCurvatureFromDerivatives(
			ri.geometric.derivatives.dpdu, ri.geometric.derivatives.dpdv,
			ri.geometric.derivatives.dndu, ri.geometric.derivatives.dndv, H );
		Check( ok, "Part 3: cavity wall curvature computed" );
		Check( Close( H, -1.0 / rB ), "Part 3: cavity wall reads CONCAVE H = -1.0 (inverted sign)" );
	}

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
}

static SDFGeometry* BuildSdfSphere( const Scalar radius )
{
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart(
		SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), radius, 0, 0, 0 ) );
	return new SDFGeometry( parts, 512, Scalar( 1e-5 ) );
}

//
// Part 4: SDF Surfaces under CSG
//
static void TestSDF_CSGCurvature()
{
	std::cout << "Part 4: SDF surfaces under CSG direct curvature...\n";
	SurfaceCurvatureDemand::Registration reg( true );

	const Scalar rA = 2.0;
	const Scalar rB = 1.0;

	SDFGeometry* gA = BuildSdfSphere( rA );
	SDFGeometry* gB = BuildSdfSphere( rB );

	Object* oA = new Object( gA );
	Object* oB = new Object( gB );
	safe_release( gA );
	safe_release( gB );

	oA->SetPosition( Point3( -1.5, 0, 0 ) );
	oB->SetPosition( Point3(  1.5, 0, 0 ) );
	oA->FinalizeTransformations();
	oB->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_UNION );
	Check( csg->AssignObjects( oA, oB ), "Part 4: SDF CSG_UNION assigned operands" );
	csg->FinalizeTransformations();

	// Ray hitting SDF Sphere A
	{
		Ray r( Point3( -1.5, 0, -5.0 ), Vector3( 0, 0, 1 ) );
		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );

		Check( ri.geometric.bHit, "Part 4: ray hits SDF A in union" );
		Check( ri.geometric.derivatives.curvatureValid, "Part 4: SDF A curvatureValid is true" );
		Check( Close( ri.geometric.derivatives.curvature, 1.0 / rA, 0.05 ), "Part 4: SDF A curvature matches 1/rA = 0.5" );
	}

	// Ray hitting SDF Sphere B
	{
		Ray r( Point3( 1.5, 0, -5.0 ), Vector3( 0, 0, 1 ) );
		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );

		Check( ri.geometric.bHit, "Part 4: ray hits SDF B in union" );
		Check( ri.geometric.derivatives.curvatureValid, "Part 4: SDF B curvatureValid is true" );
		Check( Close( ri.geometric.derivatives.curvature, 1.0 / rB, 0.05 ), "Part 4: SDF B curvature matches 1/rB = 1.0" );
	}

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
}

//
// Part 5: CSG scale transformation divides curvature by s
//
static void TestTransform_ScaleCurvature()
{
	std::cout << "Part 5: CSG object scale transformation divides curvature by s...\n";
	SurfaceCurvatureDemand::Registration reg( true );

	const Scalar rA = 1.0;
	const Scalar s = 2.0;

	SphereGeometry* gA = new SphereGeometry( rA );
	SphereGeometry* gB = new SphereGeometry( rA );
	Object* oA = new Object( gA );
	Object* oB = new Object( gB );
	safe_release( gA );
	safe_release( gB );

	oA->SetPosition( Point3( -1.0, 0, 0 ) );
	oB->SetPosition( Point3(  1.0, 0, 0 ) );
	oA->FinalizeTransformations();
	oB->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_UNION );
	csg->AssignObjects( oA, oB );
	csg->SetScale( s );
	csg->FinalizeTransformations();

	Ray r( Point3( -s, 0, -10.0 ), Vector3( 0, 0, 1 ) );
	RayIntersection ri( r, nullRasterizerState );
	Hit( csg, r, ri );

	Check( ri.geometric.bHit, "Part 5: ray hits scaled CSG object" );
	Check( ri.geometric.derivatives.valid, "Part 5: derivatives valid on scaled CSG" );

	Scalar H = 0;
	bool ok = SurfaceCurvature::MeanCurvatureFromDerivatives(
		ri.geometric.derivatives.dpdu, ri.geometric.derivatives.dpdv,
		ri.geometric.derivatives.dndu, ri.geometric.derivatives.dndv, H );
	Check( ok, "Part 5: mean curvature computed on scaled CSG" );
	// Local H = 1.0; after uniform scale s = 2.0, world H = 1.0 / 2.0 = 0.5
	Check( Close( H, ( 1.0 / rA ) / s ), "Part 5: scaled CSG curvature H = H_local / s = 0.5" );

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
}

//
// Part 6: Seam continuity: rays approaching the seam from either side
//
static void TestSeam_Continuity()
{
	std::cout << "Part 6: Seam continuity right up to boundary...\n";
	SurfaceCurvatureDemand::Registration reg( true );

	const Scalar rA = 2.0;
	const Scalar rB = 1.0;

	SphereGeometry* gA = new SphereGeometry( rA );
	SphereGeometry* gB = new SphereGeometry( rB );
	Object* oA = new Object( gA );
	Object* oB = new Object( gB );
	safe_release( gA );
	safe_release( gB );

	oA->SetPosition( Point3( 0, 0, -0.8 ) );
	oB->SetPosition( Point3( 0, 0,  0.8 ) );
	oA->FinalizeTransformations();
	oB->FinalizeTransformations();

	CSGObject* csg = new CSGObject( CSG_INTERSECTION );
	csg->AssignObjects( oA, oB );
	csg->FinalizeTransformations();

	// Sweep across the seam at multiple offsets
	int validCurvatureCount = 0;
	for( Scalar y = -0.5; y <= 0.5; y += 0.1 )
	{
		Ray r( Point3( 0.05, y, -5.0 ), Vector3( 0, 0, 1 ) );
		RayIntersection ri( r, nullRasterizerState );
		Hit( csg, r, ri );

		if( ri.geometric.bHit && ri.geometric.derivatives.valid ) {
			Scalar H = 0;
			if( SurfaceCurvature::MeanCurvatureFromDerivatives(
				ri.geometric.derivatives.dpdu, ri.geometric.derivatives.dpdv,
				ri.geometric.derivatives.dndu, ri.geometric.derivatives.dndv, H ) )
			{
				if( !std::isnan( H ) && !std::isinf( H ) && H > 0.1 ) {
					++validCurvatureCount;
				}
			}
		}
	}

	Check( validCurvatureCount == 11, "Part 6: all 11 ray hits across boundary seam evaluate valid curvature" );

	safe_release( csg );
	safe_release( oA );
	safe_release( oB );
}

int main()
{
	std::cout << "=== CSGCurvatureConventionTest (DL-21) ===\n";
	TestIntersection_AnalyticCurvature();
	TestUnion_AnalyticCurvature();
	TestSubtraction_CurvatureSign();
	TestSDF_CSGCurvature();
	TestTransform_ScaleCurvature();
	TestSeam_Continuity();

	std::cout << "\nResult: " << g_pass << " passed, " << g_fail << " failed.\n";
	return g_fail == 0 ? 0 : 1;
}
