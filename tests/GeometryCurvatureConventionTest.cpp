//////////////////////////////////////////////////////////////////////
//
//  GeometryCurvatureConventionTest.cpp - Verification suite for DL-13
//  (displaced-geometry curvature ambiguity).
//
//  Pins the curvature convention for displaced geometry:
//  - Ray hits and shading expressions evaluate the displaced surface
//    curvature by default (smoothing=0, what rays hit and wear masks see).
//  - Analytical queries via ComputeAnalyticalDerivatives support both:
//    * smoothing=0: displaced analytic surface curvature, verified
//      against closed form for uniform displacement and against a
//      finite-difference oracle for non-uniform displacement.
//    * smoothing=1: base surface curvature, verified against closed form.
//  - The descriptor for displaced_geometry explicitly documents this
//    convention.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <cmath>
#include <vector>
#include <string>

#include "../src/Library/Utilities/SurfaceCurvature.h"
#include "../src/Library/Geometry/DisplacedGeometry.h"
#include "../src/Library/Geometry/EllipsoidGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/SceneEditor/ChunkDescriptorRegistry.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Interfaces/IFunction2D.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK( cond, msg ) \
	do { \
		if( (cond) ) { \
			std::cout << "  PASS: " << (msg) << "\n"; \
			++g_passed; \
		} else { \
			std::cout << "  FAIL: " << (msg) << "\n"; \
			++g_failed; \
		} \
	} while(0)

class ConstantFunc2D : public IFunction2D, public Reference
{
public:
	Scalar m_val;
	ConstantFunc2D( Scalar v ) : m_val(v) {}
	Scalar Evaluate( const Scalar, const Scalar ) const override { return m_val; }
};

class SinusoidalFunc2D : public IFunction2D, public Reference
{
public:
	Scalar Evaluate( const Scalar u, const Scalar v ) const override
	{
		return sin( u * TWO_PI ) * sin( v * PI );
	}
};

// Test 1: Chunk descriptor documents curvature convention
static void TestDescriptorCurvatureConvention()
{
	std::cout << "Test 1: displaced_geometry chunk descriptor documents curvature convention...\n";

	const ChunkDescriptor* desc = DescriptorForKeyword( String( "displaced_geometry" ) );
	CHECK( desc != nullptr, "displaced_geometry descriptor exists" );
	if( !desc ) return;

	const bool hasCurvDoc = (desc->description.find( "Curvature convention:" ) != std::string::npos);
	CHECK( hasCurvDoc, "displaced_geometry descriptor documents curvature convention (smoothing=0 vs smoothing=1)" );
}

// Test 2: Uniform radial displacement on sphere (closed form)
// Base sphere radius R=2.0 -> H_base = 1/2.0 = 0.5.
// Displacement delta = 0.5 -> displaced sphere radius 2.5 -> H_disp = 1/2.5 = 0.4.
static void TestUniformDisplacedSphereClosedForm()
{
	std::cout << "Test 2: Uniformly displaced sphere matches closed-form curvatures...\n";

	const Scalar R = 2.0;
	const Scalar delta = 0.5;
	EllipsoidGeometry* base = new EllipsoidGeometry( Vector3( R, R, R ) );
	ConstantFunc2D* func = new ConstantFunc2D( 1.0 );

	DisplacedGeometry* disp = new DisplacedGeometry(
		base,
		32,
		func,
		delta,
		false,
		false,
		true
	);

	const Point2 testPoints[] = {
		Point2( 0.2, 0.3 ),
		Point2( 0.35, 0.65 ),
		Point2( 0.72, 0.24 ),
		Point2( 0.81, 0.77 )
	};

	for( size_t i = 0; i < 4; ++i ) {
		const Point2& uv = testPoints[i];
		Point3 P;
		Vector3 N, dpdu, dpdv, dndu, dndv;
		Scalar H = 0.0;

		// smoothing = 1.0 (base surface)
		bool ok = disp->ComputeAnalyticalDerivatives( uv, 1.0, P, N, dpdu, dpdv, dndu, dndv );
		CHECK( ok, "smoothing=1 ComputeAnalyticalDerivatives succeeded" );
		if( ok ) {
			bool curvOk = SurfaceCurvature::MeanCurvatureFromDerivatives( dpdu, dpdv, dndu, dndv, H );
			CHECK( curvOk, "smoothing=1 MeanCurvatureFromDerivatives succeeded" );
			const Scalar err = std::fabs( H - (1.0 / R) );
			CHECK( err < 1e-5, "smoothing=1 curvature matches base sphere closed form (0.5)" );
		}

		// smoothing = 0.0 (displaced surface)
		ok = disp->ComputeAnalyticalDerivatives( uv, 0.0, P, N, dpdu, dpdv, dndu, dndv );
		CHECK( ok, "smoothing=0 ComputeAnalyticalDerivatives succeeded" );
		if( ok ) {
			bool curvOk = SurfaceCurvature::MeanCurvatureFromDerivatives( dpdu, dpdv, dndu, dndv, H );
			CHECK( curvOk, "smoothing=0 MeanCurvatureFromDerivatives succeeded" );
			const Scalar expectedH = 1.0 / (R + delta);
			const Scalar err = std::fabs( H - expectedH );
			CHECK( err < 1e-5, "smoothing=0 curvature matches displaced sphere closed form (0.4)" );
		}

		// Intermediate smoothing s=0.5 -> effScale = delta * 0.5 = 0.25 -> R_eff = 2.25 -> H = 1/2.25
		ok = disp->ComputeAnalyticalDerivatives( uv, 0.5, P, N, dpdu, dpdv, dndu, dndv );
		if( ok ) {
			bool curvOk = SurfaceCurvature::MeanCurvatureFromDerivatives( dpdu, dpdv, dndu, dndv, H );
			CHECK( curvOk, "smoothing=0.5 MeanCurvatureFromDerivatives succeeded" );
			const Scalar expectedH = 1.0 / (R + delta * 0.5);
			const Scalar err = std::fabs( H - expectedH );
			CHECK( err < 1e-5, "smoothing=0.5 curvature matches intermediate sphere closed form" );
		}
	}

	safe_release( disp );
	safe_release( func );
	safe_release( base );
}

// Test 3: Non-uniform sinusoidal displacement vs Finite-Difference Oracle
static void TestNonUniformDisplacementOracle()
{
	std::cout << "Test 3: Non-uniform displacement vs finite-difference oracle...\n";

	const Scalar R = 1.0;
	const Scalar dispScale = 0.08;
	EllipsoidGeometry* base = new EllipsoidGeometry( Vector3( R, R, R ) );
	SinusoidalFunc2D* func = new SinusoidalFunc2D();

	DisplacedGeometry* disp = new DisplacedGeometry(
		base,
		32,
		func,
		dispScale,
		false,
		false,
		false // no seam fold for smooth differentiation
	);

	// Evaluate displaced surface position analytically:
	auto evalDisplaced = [&]( const Point2& uv ) -> Point3 {
		Point3 Pb;
		Vector3 Nb, dua, dva, dnua, dnva;
		base->ComputeAnalyticalDerivatives( uv, 0.0, Pb, Nb, dua, dva, dnua, dnva );
		const Scalar f = func->Evaluate( uv.x, uv.y );
		return Point3Ops::mkPoint3( Pb, Nb * (dispScale * f) );
	};

	// Finite-difference oracle for (dpdu, dpdv, dndu, dndv)
	auto evalOracleCurvature = [&]( const Point2& uv ) -> Scalar {
		const Scalar eps = 1e-4;
		const Point3 Pu_plus  = evalDisplaced( Point2( uv.x + eps, uv.y ) );
		const Point3 Pu_minus = evalDisplaced( Point2( uv.x - eps, uv.y ) );
		const Point3 Pv_plus  = evalDisplaced( Point2( uv.x, uv.y + eps ) );
		const Point3 Pv_minus = evalDisplaced( Point2( uv.x, uv.y - eps ) );

		const Vector3 dpdu = Vector3( Pu_plus.x - Pu_minus.x, Pu_plus.y - Pu_minus.y, Pu_plus.z - Pu_minus.z ) * (1.0 / (2.0 * eps));
		const Vector3 dpdv = Vector3( Pv_plus.x - Pv_minus.x, Pv_plus.y - Pv_minus.y, Pv_plus.z - Pv_minus.z ) * (1.0 / (2.0 * eps));

		auto evalNormal = [&]( const Point2& p ) -> Vector3 {
			const Point3 pup = evalDisplaced( Point2( p.x + eps, p.y ) );
			const Point3 pum = evalDisplaced( Point2( p.x - eps, p.y ) );
			const Point3 pvp = evalDisplaced( Point2( p.x, p.y + eps ) );
			const Point3 pvm = evalDisplaced( Point2( p.x, p.y - eps ) );
			Vector3 tu = Vector3( pup.x - pum.x, pup.y - pum.y, pup.z - pum.z ) * (1.0 / (2.0 * eps));
			Vector3 tv = Vector3( pvp.x - pvm.x, pvp.y - pvm.y, pvp.z - pvm.z ) * (1.0 / (2.0 * eps));
			Vector3 n = Vector3Ops::Normalize( Vector3Ops::Cross( tu, tv ) );
			// Orient outward (dot with position > 0)
			Point3 pos = evalDisplaced( p );
			if( Vector3Ops::Dot( n, Vector3( pos.x, pos.y, pos.z ) ) < 0 ) {
				n = n * -1.0;
			}
			return n;
		};

		const Vector3 Nu_plus  = evalNormal( Point2( uv.x + eps, uv.y ) );
		const Vector3 Nu_minus = evalNormal( Point2( uv.x - eps, uv.y ) );
		const Vector3 Nv_plus  = evalNormal( Point2( uv.x, uv.y + eps ) );
		const Vector3 Nv_minus = evalNormal( Point2( uv.x, uv.y - eps ) );

		const Vector3 dndu = (Nu_plus - Nu_minus) * (1.0 / (2.0 * eps));
		const Vector3 dndv = (Nv_plus - Nv_minus) * (1.0 / (2.0 * eps));

		Scalar Horacle = 0.0;
		SurfaceCurvature::MeanCurvatureFromDerivatives( dpdu, dpdv, dndu, dndv, Horacle );
		return Horacle;
	};

	const Point2 testPoints[] = {
		Point2( 0.22, 0.35 ),
		Point2( 0.65, 0.40 ),
		Point2( 0.30, 0.70 )
	};

	for( size_t i = 0; i < 3; ++i ) {
		const Point2& uv = testPoints[i];
		Point3 P;
		Vector3 N, dpdu, dpdv, dndu, dndv;
		Scalar H_disp = 0.0;
		Scalar H_base = 0.0;

		// smoothing = 1.0 (base)
		disp->ComputeAnalyticalDerivatives( uv, 1.0, P, N, dpdu, dpdv, dndu, dndv );
		SurfaceCurvature::MeanCurvatureFromDerivatives( dpdu, dpdv, dndu, dndv, H_base );
		CHECK( std::fabs( H_base - 1.0 ) < 1e-5, "smoothing=1 gives base sphere curvature (1.0)" );

		// smoothing = 0.0 (displaced)
		disp->ComputeAnalyticalDerivatives( uv, 0.0, P, N, dpdu, dpdv, dndu, dndv );
		SurfaceCurvature::MeanCurvatureFromDerivatives( dpdu, dpdv, dndu, dndv, H_disp );

		const Scalar H_oracle = evalOracleCurvature( uv );
		const Scalar relErr = std::fabs( H_disp - H_oracle ) / std::fabs( H_oracle );
		CHECK( relErr < 0.01, "smoothing=0 matches finite-difference oracle within 1%" );
		CHECK( std::fabs( H_disp - H_base ) > 0.001, "displaced curvature differs from base curvature" );
	}

	safe_release( disp );
	safe_release( func );
	safe_release( base );
}

// Test 4: Ray hit on baked displaced mesh reflects displaced curvature
static void TestRayHitReflectsDisplacedCurvature()
{
	std::cout << "Test 4: Ray hit on baked mesh reflects displaced curvature (smoothing=0)...\n";

	const Scalar R = 2.0;
	const Scalar delta = 0.5;
	EllipsoidGeometry* base = new EllipsoidGeometry( Vector3( R, R, R ) );
	ConstantFunc2D* func = new ConstantFunc2D( 1.0 );

	DisplacedGeometry* disp = new DisplacedGeometry(
		base,
		64, // fine tessellation
		func,
		delta,
		false,
		false,
		true
	);

	disp->Realize();

	Object* obj = new Object( disp );

	// Cast ray toward displaced sphere along +Z axis hit at (0, 0, 2.5)
	Ray ray( Point3( 0, 0, 5.0 ), Vector3( 0, 0, -1.0 ) );
	RayIntersection ri( ray, nullRasterizerState );

	obj->IntersectRay( ri, INFINITY, true, false, false );
	CHECK( ri.geometric.bHit, "Ray hits displaced mesh" );

	if( ri.geometric.bHit ) {
		// Hit position should be at z ≈ 2.5 (displaced radius)
		CHECK( std::fabs( ri.geometric.ptIntersection.z - 2.5 ) < 0.05, "Hit position is on displaced surface (z ≈ 2.5)" );
		CHECK( ri.geometric.derivatives.valid, "Hit derivatives are valid" );

		Scalar H = 0.0;
		bool curvOk = SurfaceCurvature::MeanCurvatureFromDerivatives(
			ri.geometric.derivatives.dpdu,
			ri.geometric.derivatives.dpdv,
			ri.geometric.derivatives.dndu,
			ri.geometric.derivatives.dndv,
			H
		);
		CHECK( curvOk, "MeanCurvatureFromDerivatives succeeded on ray hit" );
		// Displaced curvature should be ~ 1/2.5 = 0.4, NOT base curvature 1/2.0 = 0.5
		const Scalar expectedH = 1.0 / (R + delta);
		const Scalar baseH = 1.0 / R;
		CHECK( std::fabs( H - expectedH ) < 0.05, "Ray hit curvature reflects displaced surface (0.4), not base (0.5)" );
		CHECK( std::fabs( H - baseH ) > 0.04, "Ray hit curvature significantly differs from base sphere curvature" );
	}

	safe_release( obj );
	safe_release( disp );
	safe_release( func );
	safe_release( base );
}

// Test 5: Base geometry accessor
static void TestBaseGeometryAccessor()
{
	std::cout << "Test 5: Base geometry accessor GetBaseGeometry()...\n";

	EllipsoidGeometry* base = new EllipsoidGeometry( Vector3( 2.0, 2.0, 2.0 ) );
	ConstantFunc2D* func = new ConstantFunc2D( 1.0 );

	DisplacedGeometry* disp = new DisplacedGeometry(
		base,
		16,
		func,
		0.2,
		false,
		false,
		true
	);

	CHECK( disp->GetBaseGeometry() == base, "GetBaseGeometry() returns base geometry pointer" );

	// Query base directly via GetBaseGeometry
	Point3 P;
	Vector3 N, dpdu, dpdv, dndu, dndv;
	const bool ok = disp->GetBaseGeometry()->ComputeAnalyticalDerivatives(
		Point2( 0.25, 0.5 ), 0.0, P, N, dpdu, dpdv, dndu, dndv );
	CHECK( ok, "base ComputeAnalyticalDerivatives via GetBaseGeometry succeeded" );
	if( ok ) {
		Scalar H = 0.0;
		SurfaceCurvature::MeanCurvatureFromDerivatives( dpdu, dpdv, dndu, dndv, H );
		CHECK( std::fabs( H - 0.5 ) < 1e-5, "base curvature via GetBaseGeometry matches base sphere closed form (0.5)" );
	}

	safe_release( disp );
	safe_release( func );
	safe_release( base );
}

int main()
{
	std::cout << "=== GeometryCurvatureConventionTest (DL-13) ===\n";

	TestDescriptorCurvatureConvention();
	TestUniformDisplacedSphereClosedForm();
	TestNonUniformDisplacementOracle();
	TestRayHitReflectsDisplacedCurvature();
	TestBaseGeometryAccessor();

	std::cout << "\nResults: " << g_passed << " passed, " << g_failed << " failed.\n";
	return g_failed == 0 ? 0 : 1;
}
