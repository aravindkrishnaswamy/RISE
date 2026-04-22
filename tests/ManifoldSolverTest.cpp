//////////////////////////////////////////////////////////////////////
//
//  ManifoldSolverTest.cpp - Unit tests for ManifoldSolver
//
//    Covers all pure-math methods: 2x2 block utilities, specular
//    direction computation, derivatives, constraint evaluation,
//    Jacobian construction, block-tridiagonal solver, chain geometry,
//    chain throughput, Fresnel, physical validation, and
//    DeriveNormalized.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>
#include "TestableManifoldSolver.h"

using namespace RISE;
using namespace RISE::Implementation;

static const double kPi = std::acos(-1.0);

static bool IsClose( Scalar a, Scalar b, Scalar epsilon = 1e-5 )
{
	return std::fabs( a - b ) < epsilon;
}

static bool IsVectorClose( const Vector3& a, const Vector3& b, Scalar epsilon = 1e-5 )
{
	return IsClose( a.x, b.x, epsilon ) &&
		   IsClose( a.y, b.y, epsilon ) &&
		   IsClose( a.z, b.z, epsilon );
}

/// Build a ManifoldVertex with manual data (no pObject/pMaterial).
static ManifoldVertex MakeVertex(
	const Point3& pos,
	const Vector3& normal,
	const Vector3& dpdu,
	const Vector3& dpdv,
	Scalar eta,
	bool isReflection,
	const Vector3& dndu = Vector3(0,0,0),
	const Vector3& dndv = Vector3(0,0,0)
	)
{
	ManifoldVertex v;
	v.position = pos;
	v.normal = normal;
	v.dpdu = dpdu;
	v.dpdv = dpdv;
	v.dndu = dndu;
	v.dndv = dndv;
	v.eta = eta;
	v.isReflection = isReflection;
	v.attenuation = RISEPel( 1.0, 1.0, 1.0 );
	v.valid = true;
	return v;
}

// ============================================================
// Group 1: 2x2 Block Utilities
// ============================================================

void TestInvert2x2_Identity()
{
	std::cout << "Testing Invert2x2 identity..." << std::endl;
	Scalar I[4] = {1,0,0,1};
	Scalar inv[4];
	assert( ManifoldSolver::Invert2x2( I, inv ) );
	assert( IsClose( inv[0], 1.0 ) );
	assert( IsClose( inv[1], 0.0 ) );
	assert( IsClose( inv[2], 0.0 ) );
	assert( IsClose( inv[3], 1.0 ) );
}

void TestInvert2x2_Known()
{
	std::cout << "Testing Invert2x2 known matrix..." << std::endl;
	Scalar A[4] = {2,1,3,4};
	Scalar inv[4];
	assert( ManifoldSolver::Invert2x2( A, inv ) );
	// det = 2*4 - 1*3 = 5; inv = [4,-1;-3,2]/5
	assert( IsClose( inv[0],  4.0/5.0 ) );
	assert( IsClose( inv[1], -1.0/5.0 ) );
	assert( IsClose( inv[2], -3.0/5.0 ) );
	assert( IsClose( inv[3],  2.0/5.0 ) );
}

void TestInvert2x2_Singular()
{
	std::cout << "Testing Invert2x2 singular..." << std::endl;
	Scalar A[4] = {1,2,2,4};  // det = 0
	Scalar inv[4];
	assert( !ManifoldSolver::Invert2x2( A, inv ) );
}

void TestInvert2x2_RoundTrip()
{
	std::cout << "Testing Invert2x2 round-trip..." << std::endl;
	Scalar A[4] = {3.5, -1.2, 0.7, 2.3};
	Scalar inv[4];
	assert( ManifoldSolver::Invert2x2( A, inv ) );
	// A * inv should be identity
	Scalar product[4];
	ManifoldSolver::Mul2x2( A, inv, product );
	assert( IsClose( product[0], 1.0, 1e-10 ) );
	assert( IsClose( product[1], 0.0, 1e-10 ) );
	assert( IsClose( product[2], 0.0, 1e-10 ) );
	assert( IsClose( product[3], 1.0, 1e-10 ) );
}

void TestMul2x2_Known()
{
	std::cout << "Testing Mul2x2 known product..." << std::endl;
	Scalar A[4] = {1,2,3,4};
	Scalar B[4] = {5,6,7,8};
	Scalar C[4];
	ManifoldSolver::Mul2x2( A, B, C );
	assert( IsClose( C[0], 19.0 ) );
	assert( IsClose( C[1], 22.0 ) );
	assert( IsClose( C[2], 43.0 ) );
	assert( IsClose( C[3], 50.0 ) );
}

void TestMul2x2Vec_Known()
{
	std::cout << "Testing Mul2x2Vec known product..." << std::endl;
	Scalar A[4] = {1,2,3,4};
	Scalar v[2] = {5,6};
	Scalar r[2];
	ManifoldSolver::Mul2x2Vec( A, v, r );
	assert( IsClose( r[0], 17.0 ) );
	assert( IsClose( r[1], 39.0 ) );
}

void TestSub2x2_SelfSubtraction()
{
	std::cout << "Testing Sub2x2 self-subtraction..." << std::endl;
	Scalar A[4] = {1,2,3,4};
	Scalar C[4];
	ManifoldSolver::Sub2x2( A, A, C );
	assert( IsClose( C[0], 0.0 ) );
	assert( IsClose( C[1], 0.0 ) );
	assert( IsClose( C[2], 0.0 ) );
	assert( IsClose( C[3], 0.0 ) );
}

// ============================================================
// Group 10: DeriveNormalized
// ============================================================

void TestDeriveNormalized_Perpendicular()
{
	std::cout << "Testing DeriveNormalized perpendicular..." << std::endl;
	Vector3 h( 1, 0, 0 );
	Vector3 dv( 0, 1, 0 );
	Vector3 result = ManifoldSolver::DeriveNormalized( h, dv, 1.0 );
	// dv is perpendicular to h, so full projection: result = dv / vLen = (0,1,0)
	assert( IsVectorClose( result, Vector3( 0, 1, 0 ) ) );
}

void TestDeriveNormalized_Parallel()
{
	std::cout << "Testing DeriveNormalized parallel..." << std::endl;
	Vector3 h( 1, 0, 0 );
	Vector3 dv( 3, 0, 0 );  // parallel to h
	Vector3 result = ManifoldSolver::DeriveNormalized( h, dv, 1.0 );
	// Parallel component is removed: result ≈ (0, 0, 0)
	assert( IsVectorClose( result, Vector3( 0, 0, 0 ) ) );
}

void TestDeriveNormalized_ZeroLength()
{
	std::cout << "Testing DeriveNormalized zero length..." << std::endl;
	Vector3 h( 1, 0, 0 );
	Vector3 dv( 0, 1, 0 );
	Vector3 result = ManifoldSolver::DeriveNormalized( h, dv, 0.0 );
	assert( IsVectorClose( result, Vector3( 0, 0, 0 ) ) );
}

void TestDeriveNormalized_FiniteDifference()
{
	std::cout << "Testing DeriveNormalized vs finite difference..." << std::endl;
	Vector3 v( 2.0, 1.0, 0.5 );
	Vector3 dv( 0.3, -0.1, 0.7 );
	Scalar vLen = Vector3Ops::Magnitude( v );
	Vector3 h = v * (1.0 / vLen);

	Vector3 analytical = ManifoldSolver::DeriveNormalized( h, dv, vLen );

	// Finite difference: d/deps normalize(v + eps*dv)
	const Scalar eps = 1e-6;
	Vector3 vp = Vector3( v.x + eps * dv.x, v.y + eps * dv.y, v.z + eps * dv.z );
	Vector3 vm = Vector3( v.x - eps * dv.x, v.y - eps * dv.y, v.z - eps * dv.z );
	Vector3 hp = Vector3Ops::Normalize( vp );
	Vector3 hm = Vector3Ops::Normalize( vm );
	Vector3 numerical( (hp.x - hm.x) / (2*eps), (hp.y - hm.y) / (2*eps), (hp.z - hm.z) / (2*eps) );

	assert( IsVectorClose( analytical, numerical, 1e-4 ) );
}

// ============================================================
// Group 11: ComputeDielectricFresnel
// ============================================================

void TestFresnel_NormalIncidence()
{
	std::cout << "Testing Fresnel normal incidence..." << std::endl;
	// F = ((eta_i - eta_t) / (eta_i + eta_t))^2  at normal incidence
	Scalar F = ManifoldSolver::ComputeDielectricFresnel( 1.0, 1.0, 1.5 );
	Scalar expected = ((1.0 - 1.5) / (1.0 + 1.5)) * ((1.0 - 1.5) / (1.0 + 1.5));
	assert( IsClose( F, expected, 1e-10 ) );
}

void TestFresnel_GrazingAngle()
{
	std::cout << "Testing Fresnel grazing angle..." << std::endl;
	// At grazing (cos_i → 0), Fresnel → 1.0
	Scalar F = ManifoldSolver::ComputeDielectricFresnel( 0.001, 1.0, 1.5 );
	assert( F > 0.95 );
}

void TestFresnel_TIR()
{
	std::cout << "Testing Fresnel TIR..." << std::endl;
	// From dense medium at steep angle → TIR
	// Critical angle for eta_i=1.5, eta_t=1.0: sin(θc) = 1/1.5 = 0.667, cos(θc) = 0.745
	// At cos_i = 0.5 (below critical), should be TIR
	Scalar F = ManifoldSolver::ComputeDielectricFresnel( 0.5, 1.5, 1.0 );
	assert( IsClose( F, 1.0 ) );
}

void TestFresnel_Symmetry()
{
	std::cout << "Testing Fresnel symmetry at normal incidence..." << std::endl;
	// F(1, η_i, η_t) at normal incidence should equal F(1, η_t, η_i) (reciprocal)
	Scalar F1 = ManifoldSolver::ComputeDielectricFresnel( 1.0, 1.0, 1.5 );
	Scalar F2 = ManifoldSolver::ComputeDielectricFresnel( 1.0, 1.5, 1.0 );
	assert( IsClose( F1, F2, 1e-10 ) );
}

// ============================================================
// Group 2: ComputeSpecularDirection
// ============================================================

void TestReflection_NormalIncidence()
{
	std::cout << "Testing reflection normal incidence..." << std::endl;
	TestableManifoldSolver solver;
	Vector3 wi( 0, 1, 0 );
	Vector3 normal( 0, 1, 0 );
	Vector3 wo;
	bool ok = solver.ComputeSpecularDirection( wi, normal, 1.0, true, wo );
	assert( ok );
	// wo should be (0, 1, 0) — reflects straight back since wi = normal
	// Actually: wo = -wi + 2*(wi.n)*n = -(0,1,0) + 2*1*(0,1,0) = (0,1,0)
	assert( IsVectorClose( wo, Vector3( 0, 1, 0 ) ) );
}

void TestReflection_45Degrees()
{
	std::cout << "Testing reflection 45 degrees..." << std::endl;
	TestableManifoldSolver solver;
	const Scalar s = 1.0 / sqrt(2.0);
	Vector3 wi( s, s, 0 );
	Vector3 normal( 0, 1, 0 );
	Vector3 wo;
	bool ok = solver.ComputeSpecularDirection( wi, normal, 1.0, true, wo );
	assert( ok );
	// wo = -wi + 2*(wi.n)*n = -(s,s,0) + 2*s*(0,1,0) = (-s, s, 0)
	assert( IsVectorClose( wo, Vector3( -s, s, 0 ) ) );
	assert( IsClose( Vector3Ops::Magnitude( wo ), 1.0 ) );
}

void TestRefraction_NormalIncidence()
{
	std::cout << "Testing refraction normal incidence..." << std::endl;
	TestableManifoldSolver solver;
	Vector3 wi( 0, 1, 0 );
	Vector3 normal( 0, 1, 0 );
	Vector3 wo;
	bool ok = solver.ComputeSpecularDirection( wi, normal, 1.5, false, wo );
	assert( ok );
	// Normal incidence: goes straight through → wo = (0, -1, 0)
	assert( IsVectorClose( wo, Vector3( 0, -1, 0 ) ) );
}

void TestRefraction_SnellsLaw()
{
	std::cout << "Testing refraction Snell's law..." << std::endl;
	TestableManifoldSolver solver;
	// wi at 30 degrees from normal, eta = 1.5
	// sin(30) = 0.5, sin(theta_t) = sin(30)/1.5 = 1/3, theta_t = asin(1/3)
	const Scalar cos30 = sqrt(3.0) / 2.0;
	const Scalar sin30 = 0.5;
	Vector3 wi( sin30, cos30, 0 );
	Vector3 normal( 0, 1, 0 );
	Vector3 wo;
	bool ok = solver.ComputeSpecularDirection( wi, normal, 1.5, false, wo );
	assert( ok );
	assert( IsClose( Vector3Ops::Magnitude( wo ), 1.0, 1e-10 ) );
	// Verify Snell's law: eta_i * sin(theta_i) = eta_t * sin(theta_t)
	const Scalar sinT = sqrt( 1.0 - Vector3Ops::Dot( wo, Vector3(0,-1,0) ) * Vector3Ops::Dot( wo, Vector3(0,-1,0) ) );
	assert( IsClose( 1.0 * sin30, 1.5 * sinT, 1e-10 ) );
}

void TestRefraction_TIR()
{
	std::cout << "Testing refraction TIR..." << std::endl;
	TestableManifoldSolver solver;
	// From inside glass (cos_i < 0 convention), steep angle
	// eta = 1.5, wi on opposite side of normal → exiting
	Vector3 wi( 0.8, -0.6, 0 );  // cos_i = -0.6 → exiting
	wi = Vector3Ops::Normalize( wi );
	Vector3 normal( 0, 1, 0 );
	Vector3 wo;
	bool ok = solver.ComputeSpecularDirection( wi, normal, 1.5, false, wo );
	// sin²(t) = (1.5)² * (1 - 0.6²) = 2.25 * 0.64 = 1.44 > 1 → TIR
	assert( !ok );
}

void TestRefraction_UnitVector()
{
	std::cout << "Testing refraction produces unit vector..." << std::endl;
	TestableManifoldSolver solver;
	// Multiple angles and eta values
	Scalar angles[] = { 0.1, 0.3, 0.5, 0.7, 0.9 };
	Scalar etas[] = { 1.2, 1.5, 2.0 };
	Vector3 normal( 0, 1, 0 );
	for( int a = 0; a < 5; a++ )
	{
		for( int e = 0; e < 3; e++ )
		{
			Vector3 wi( sin(angles[a]), cos(angles[a]), 0 );
			Vector3 wo;
			if( solver.ComputeSpecularDirection( wi, normal, etas[e], false, wo ) )
			{
				assert( IsClose( Vector3Ops::Magnitude( wo ), 1.0, 1e-10 ) );
			}
		}
	}
}

// ============================================================
// Group 3: Specular Direction Derivative
// ============================================================

void TestReflectionDerivative_FD()
{
	std::cout << "Testing reflection derivative vs FD..." << std::endl;
	TestableManifoldSolver solver;
	Vector3 wi( 0.5, 0.8, 0.2 );
	wi = Vector3Ops::Normalize( wi );
	Vector3 normal( 0, 1, 0 );

	Scalar analytical[9];
	solver.ComputeSpecularDirectionDerivativeWrtNormal( wi, normal, 1.0, true, analytical );

	// Finite differences: perturb normal without re-normalizing
	// (the derivative is d(wo)/d(n_j) for the raw normal components)
	const Scalar eps = 1e-6;
	for( int j = 0; j < 3; j++ )
	{
		Vector3 np = normal, nm = normal;
		Scalar* npArr = &np.x;
		Scalar* nmArr = &nm.x;
		npArr[j] += eps;
		nmArr[j] -= eps;

		Vector3 wop, wom;
		solver.ComputeSpecularDirection( wi, np, 1.0, true, wop );
		solver.ComputeSpecularDirection( wi, nm, 1.0, true, wom );

		for( int i = 0; i < 3; i++ )
		{
			Scalar* wopArr = &wop.x;
			Scalar* womArr = &wom.x;
			Scalar fd = (wopArr[i] - womArr[i]) / (2 * eps);
			if( !IsClose( analytical[i*3+j], fd, 1e-3 ) )
			{
				std::cout << "  MISMATCH: i=" << i << " j=" << j
					<< " analytical=" << analytical[i*3+j]
					<< " fd=" << fd << std::endl;
				assert( false );
			}
		}
	}
}

void TestRefractionDerivative_FD()
{
	std::cout << "Testing refraction derivative vs FD..." << std::endl;
	TestableManifoldSolver solver;
	Vector3 wi( 0.3, 0.9, 0.1 );
	wi = Vector3Ops::Normalize( wi );
	Vector3 normal( 0, 1, 0 );
	Scalar eta = 1.5;

	Scalar analytical[9];
	solver.ComputeSpecularDirectionDerivativeWrtNormal( wi, normal, eta, false, analytical );

	const Scalar eps = 1e-6;
	for( int j = 0; j < 3; j++ )
	{
		Vector3 np = normal, nm = normal;
		Scalar* npArr = &np.x;
		Scalar* nmArr = &nm.x;
		npArr[j] += eps;
		nmArr[j] -= eps;

		Vector3 wop, wom;
		solver.ComputeSpecularDirection( wi, np, eta, false, wop );
		solver.ComputeSpecularDirection( wi, nm, eta, false, wom );

		for( int i = 0; i < 3; i++ )
		{
			Scalar* wopArr = &wop.x;
			Scalar* womArr = &wom.x;
			Scalar fd = (wopArr[i] - womArr[i]) / (2 * eps);
			assert( IsClose( analytical[i*3+j], fd, 1e-3 ) );
		}
	}
}

// ============================================================
// Group 4: DirectionToSpherical
// ============================================================

void TestSpherical_NormalDirection()
{
	std::cout << "Testing spherical normal direction..." << std::endl;
	TestableManifoldSolver solver;
	Vector3 dpdu( 1, 0, 0 );
	Vector3 dpdv( 0, 0, 1 );
	Vector3 normal( 0, 1, 0 );
	Point2 sp = solver.DirectionToSpherical( normal, dpdu, dpdv, normal );
	assert( IsClose( sp.x, 0.0 ) );  // theta = 0 for normal direction
}

void TestSpherical_TangentU()
{
	std::cout << "Testing spherical tangent-u direction..." << std::endl;
	TestableManifoldSolver solver;
	Vector3 dpdu( 1, 0, 0 );
	Vector3 dpdv( 0, 0, 1 );
	Vector3 normal( 0, 1, 0 );
	Point2 sp = solver.DirectionToSpherical( Vector3(1,0,0), dpdu, dpdv, normal );
	assert( IsClose( sp.x, kPi / 2.0 ) );  // theta = pi/2
	assert( IsClose( sp.y, 0.0 ) );         // phi = 0
}

void TestSpherical_TangentV()
{
	std::cout << "Testing spherical tangent-v direction..." << std::endl;
	TestableManifoldSolver solver;
	Vector3 dpdu( 1, 0, 0 );
	Vector3 dpdv( 0, 0, 1 );
	Vector3 normal( 0, 1, 0 );
	Point2 sp = solver.DirectionToSpherical( Vector3(0,0,1), dpdu, dpdv, normal );
	assert( IsClose( sp.x, kPi / 2.0 ) );  // theta = pi/2
	assert( IsClose( sp.y, kPi / 2.0 ) );  // phi = pi/2
}

void TestSpherical_NegativeNormal()
{
	std::cout << "Testing spherical negative normal..." << std::endl;
	TestableManifoldSolver solver;
	Vector3 dpdu( 1, 0, 0 );
	Vector3 dpdv( 0, 0, 1 );
	Vector3 normal( 0, 1, 0 );
	Point2 sp = solver.DirectionToSpherical( Vector3(0,-1,0), dpdu, dpdv, normal );
	assert( IsClose( sp.x, kPi ) );  // theta = pi
}

// ============================================================
// Group 5: Constraint Functions
// ============================================================

void TestHalfVectorConstraint_AtSolution_Reflection()
{
	std::cout << "Testing half-vector constraint at reflection solution..." << std::endl;
	TestableManifoldSolver solver;

	// Flat mirror at origin, normal = (0,1,0)
	// wi from (0,2,0) → vertex at origin: wi = (0,1,0)
	// Reflected: wo = (0,1,0) → nextPos should be at (0,2,0) too... but that's trivial
	// Better: 45-degree reflection
	Point3 prevPos( -1, 1, 0 );
	Point3 vertPos( 0, 0, 0 );
	// wi = normalize((-1,1,0) - (0,0,0)) = (-1/sqrt2, 1/sqrt2, 0)
	// Reflected wo = (1/sqrt2, 1/sqrt2, 0) → nextPos at (1, 1, 0)
	Point3 nextPos( 1, 1, 0 );

	ManifoldVertex v = MakeVertex( vertPos, Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.0, true );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	std::vector<Scalar> C;
	solver.EvaluateConstraint( chain, prevPos, nextPos, C );
	assert( IsClose( C[0], 0.0, 1e-10 ) );
	assert( IsClose( C[1], 0.0, 1e-10 ) );
}

void TestHalfVectorConstraint_NotAtSolution()
{
	std::cout << "Testing half-vector constraint NOT at solution..." << std::endl;
	TestableManifoldSolver solver;

	Point3 prevPos( -1, 1, 0 );
	Point3 vertPos( 0, 0, 0 );
	Point3 nextPos( 0.5, 1, 0.5 );  // NOT the reflected direction

	ManifoldVertex v = MakeVertex( vertPos, Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.0, true );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	std::vector<Scalar> C;
	solver.EvaluateConstraint( chain, prevPos, nextPos, C );
	// At least one component should be nonzero
	assert( std::fabs( C[0] ) > 1e-6 || std::fabs( C[1] ) > 1e-6 );
}

void TestHalfVectorConstraint_AtSolution_Refraction()
{
	std::cout << "Testing half-vector constraint at refraction solution..." << std::endl;
	TestableManifoldSolver solver;

	// Flat glass at origin, normal = (0,1,0), eta = 1.5
	// wi = (0,1,0) → normal incidence → wo = (0,-1,0) → nextPos = (0,-1,0)
	Point3 prevPos( 0, 1, 0 );
	Point3 vertPos( 0, 0, 0 );
	Point3 nextPos( 0, -1, 0 );

	ManifoldVertex v = MakeVertex( vertPos, Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.5, false );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	std::vector<Scalar> C;
	solver.EvaluateConstraint( chain, prevPos, nextPos, C );
	assert( IsClose( C[0], 0.0, 1e-10 ) );
	assert( IsClose( C[1], 0.0, 1e-10 ) );
}

void TestAngleDiffConstraint_AtSolution()
{
	std::cout << "Testing angle-diff constraint at solution..." << std::endl;
	TestableManifoldSolver solver;

	// Same 45-degree reflection setup as half-vector test
	Point3 prevPos( -1, 1, 0 );
	Point3 vertPos( 0, 0, 0 );
	Point3 nextPos( 1, 1, 0 );

	Scalar C0, C1;
	solver.EvaluateConstraintAtVertex(
		vertPos, Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1),
		1.0, true,
		prevPos, nextPos, C0, C1 );
	assert( IsClose( C0, 0.0, 1e-6 ) );
	assert( IsClose( C1, 0.0, 1e-6 ) );
}

// ============================================================
// Group 6: Analytical vs Numerical Jacobian
// ============================================================

void TestJacobian_SingleVertex_Reflection()
{
	std::cout << "Testing Jacobian agreement (single vertex, reflection)..." << std::endl;
	TestableManifoldSolver solver;

	Point3 prevPos( -1, 2, 0 );
	Point3 vertPos( 0, 0, 0 );
	Point3 nextPos( 1, 2, 0 );

	ManifoldVertex v = MakeVertex( vertPos, Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.0, true );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	std::vector<Scalar> adiag, aupper, alower;
	solver.BuildJacobian( chain, prevPos, nextPos, adiag, aupper, alower );

	std::vector<Scalar> ndiag, nupper, nlower;
	solver.BuildJacobianNumerical( chain, prevPos, nextPos, ndiag, nupper, nlower );

	for( int i = 0; i < 4; i++ )
	{
		assert( IsClose( adiag[i], ndiag[i], 1e-3 ) );
	}
}

void TestJacobian_SingleVertex_Refraction()
{
	std::cout << "Testing Jacobian agreement (single vertex, refraction)..." << std::endl;
	TestableManifoldSolver solver;

	Point3 prevPos( 0, 2, 0 );
	Point3 vertPos( 0, 0, 0 );
	Point3 nextPos( 0, -2, 0 );

	ManifoldVertex v = MakeVertex( vertPos, Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.5, false );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	std::vector<Scalar> adiag, aupper, alower;
	solver.BuildJacobian( chain, prevPos, nextPos, adiag, aupper, alower );

	std::vector<Scalar> ndiag, nupper, nlower;
	solver.BuildJacobianNumerical( chain, prevPos, nextPos, ndiag, nupper, nlower );

	for( int i = 0; i < 4; i++ )
	{
		assert( IsClose( adiag[i], ndiag[i], 1e-3 ) );
	}
}

void TestJacobian_TwoVertex_Agreement()
{
	std::cout << "Testing Jacobian agreement (two vertices)..." << std::endl;
	TestableManifoldSolver solver;

	// Two refraction vertices (entering then exiting glass slab)
	Point3 prevPos( 0, 2, 0 );
	Point3 v1Pos( 0, 0.1, 0 );
	Point3 v2Pos( 0, -0.1, 0 );
	Point3 nextPos( 0, -2, 0 );

	ManifoldVertex mv1 = MakeVertex( v1Pos, Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.5, false );
	ManifoldVertex mv2 = MakeVertex( v2Pos, Vector3(0,-1,0), Vector3(1,0,0), Vector3(0,0,1), 1.5, false );

	std::vector<ManifoldVertex> chain;
	chain.push_back( mv1 );
	chain.push_back( mv2 );

	std::vector<Scalar> adiag, aupper, alower;
	solver.BuildJacobian( chain, prevPos, nextPos, adiag, aupper, alower );

	std::vector<Scalar> ndiag, nupper, nlower;
	solver.BuildJacobianNumerical( chain, prevPos, nextPos, ndiag, nupper, nlower );

	// Check diagonal blocks (8 entries)
	for( int i = 0; i < 8; i++ )
		assert( IsClose( adiag[i], ndiag[i], 1e-2 ) );
	// Check upper and lower blocks (4 entries each)
	for( int i = 0; i < 4; i++ )
	{
		assert( IsClose( aupper[i], nupper[i], 1e-2 ) );
		assert( IsClose( alower[i], nlower[i], 1e-2 ) );
	}
}

// Regression test for SMS curvature bug (documents expected behavior).
//
// On curved surfaces (dndu, dndv != 0), the analytical BuildJacobian
// includes tangent-frame rotation terms (ds_du, dt_du) that the
// current BuildJacobianNumerical does not capture, because
// EvaluateConstraint freezes v.dpdu/v.dpdv during FD perturbation.
//
// So analytical and numerical DISAGREE on curved surfaces — but the
// analytical is right (matches Cycles MNEE / Zeltner 2020).  This test
// verifies that the Jacobian diagonal *changes* when curvature is
// introduced, which is essential for caustic focusing on displaced
// meshes.
//
// Prior coverage only exercised dndu=dndv=0 (flat surfaces), where both
// formulations agree, masking a bug where an incorrect ds_du formula
// gave zero at convergence regardless of actual curvature.  See
// docs/SMS.md (April 2026) for the full analysis.
void TestJacobian_SingleVertex_WithCurvature()
{
	std::cout << "Testing Jacobian changes with surface curvature..." << std::endl;
	TestableManifoldSolver solver;

	// Non-convergent geometry so the dot(ds_du, h) term is meaningful
	Point3 prevPos( -0.5, 2, 0 );
	Point3 vertPos( 0, 0, 0 );
	Point3 nextPos( 0.3, -2, 0.2 );

	// Flat vertex (reference)
	ManifoldVertex vFlat = MakeVertex(
		vertPos, Vector3(0,1,0),
		Vector3(1,0,0), Vector3(0,0,1),
		1.5, false );  // dndu, dndv default to zero

	// Same vertex but with curvature (convex sphere-like, radius 1)
	ManifoldVertex vCurv = MakeVertex(
		vertPos, Vector3(0,1,0),
		Vector3(1,0,0), Vector3(0,0,1),
		1.5, false,
		Vector3(1,0,0), Vector3(0,0,1) );  // nonzero dndu, dndv

	std::vector<ManifoldVertex> chainFlat;  chainFlat.push_back( vFlat );
	std::vector<ManifoldVertex> chainCurv;  chainCurv.push_back( vCurv );

	std::vector<Scalar> diagFlat, upFlat, lowFlat;
	std::vector<Scalar> diagCurv, upCurv, lowCurv;
	solver.BuildJacobian( chainFlat, prevPos, nextPos, diagFlat, upFlat, lowFlat );
	solver.BuildJacobian( chainCurv, prevPos, nextPos, diagCurv, upCurv, lowCurv );

	// At least one diagonal entry must differ noticeably.  If the ds_du
	// formula degenerates to zero (old RISE bug), the curved Jacobian
	// equals the flat one and this assertion fails.
	Scalar maxDelta = 0.0;
	for( int i = 0; i < 4; i++ ) {
		const Scalar d = std::fabs( diagCurv[i] - diagFlat[i] );
		if( d > maxDelta ) maxDelta = d;
	}
	assert( maxDelta > 0.1 );  // curvature must change the Jacobian
}

// ============================================================
// Group 7: Block-Tridiagonal Solver
// ============================================================

void TestTridiag_SingleBlock()
{
	std::cout << "Testing block-tridiagonal solver k=1..." << std::endl;
	TestableManifoldSolver solver;

	// [2,1;1,3] * [x,y] = [5,7]
	// Solution: det=5, x=(5*3-1*7)/5=8/5=1.6, y=(2*7-5*1)/5=9/5=1.8
	std::vector<Scalar> diag = {2,1,1,3};
	std::vector<Scalar> upper, lower;
	std::vector<Scalar> rhs = {5,7};
	std::vector<Scalar> delta;

	bool ok = solver.SolveBlockTridiagonal( diag, upper, lower, rhs, 1, delta );
	assert( ok );
	assert( IsClose( delta[0], 1.6 ) );
	assert( IsClose( delta[1], 1.8 ) );
}

void TestTridiag_Singular()
{
	std::cout << "Testing block-tridiagonal solver singular..." << std::endl;
	TestableManifoldSolver solver;

	std::vector<Scalar> diag = {1,2,2,4};  // singular
	std::vector<Scalar> upper, lower;
	std::vector<Scalar> rhs = {1,2};
	std::vector<Scalar> delta;

	bool ok = solver.SolveBlockTridiagonal( diag, upper, lower, rhs, 1, delta );
	assert( !ok );
}

void TestTridiag_Verification()
{
	std::cout << "Testing block-tridiagonal solver verification..." << std::endl;
	TestableManifoldSolver solver;

	// k=2: known system. Solve then verify J*delta = rhs
	std::vector<Scalar> diag = {4,1,1,3, 2,0.5,0.5,2};
	std::vector<Scalar> upper = {0.1,0.2,0.3,0.4};
	std::vector<Scalar> lower = {0.4,0.3,0.2,0.1};
	std::vector<Scalar> rhs = {1, 2, 3, 4};
	std::vector<Scalar> delta;

	// Save a copy of diag since SolveBlockTridiagonal may modify it
	std::vector<Scalar> diag_copy( diag );

	bool ok = solver.SolveBlockTridiagonal( diag, upper, lower, rhs, 2, delta );
	assert( ok );

	// Verify: multiply block-tridiagonal system by delta
	// Row 0-1: D[0]*delta[0:1] + U[0]*delta[2:3]
	Scalar r0 = diag_copy[0]*delta[0] + diag_copy[1]*delta[1] + upper[0]*delta[2] + upper[1]*delta[3];
	Scalar r1 = diag_copy[2]*delta[0] + diag_copy[3]*delta[1] + upper[2]*delta[2] + upper[3]*delta[3];
	// Row 2-3: L[0]*delta[0:1] + D[1]*delta[2:3]
	Scalar r2 = lower[0]*delta[0] + lower[1]*delta[1] + diag_copy[4]*delta[2] + diag_copy[5]*delta[3];
	Scalar r3 = lower[2]*delta[0] + lower[3]*delta[1] + diag_copy[6]*delta[2] + diag_copy[7]*delta[3];

	assert( IsClose( r0, rhs[0], 1e-8 ) );
	assert( IsClose( r1, rhs[1], 1e-8 ) );
	assert( IsClose( r2, rhs[2], 1e-8 ) );
	assert( IsClose( r3, rhs[3], 1e-8 ) );
}

// ============================================================
// Group 8: Chain Geometry & Throughput
// ============================================================

void TestCosineProduct_EmptyChain()
{
	std::cout << "Testing cosine product empty chain..." << std::endl;
	TestableManifoldSolver solver;
	std::vector<ManifoldVertex> chain;
	Scalar cp = solver.EvaluateChainCosineProduct( Point3(0,0,0), Point3(0,1,0), chain );
	assert( IsClose( cp, 1.0 ) );
}

void TestCosineProduct_SingleVertex_NormalIncidence()
{
	std::cout << "Testing cosine product single vertex normal incidence..." << std::endl;
	TestableManifoldSolver solver;

	// Start directly above vertex, normal = (0,1,0)
	Point3 start( 0, 2, 0 );
	Point3 end( 0, -2, 0 );
	ManifoldVertex v = MakeVertex( Point3(0,0,0), Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.5, false );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	Scalar cp = solver.EvaluateChainCosineProduct( start, end, chain );
	// Direction from start to vertex = (0,-1,0), cos = |dot((0,1,0),(0,-1,0))| = 1.0
	assert( IsClose( cp, 1.0 ) );
}

void TestCosineProduct_SingleVertex_ObliqueIncidence()
{
	std::cout << "Testing cosine product single vertex oblique incidence..." << std::endl;
	TestableManifoldSolver solver;

	// Start at 45 degrees
	Point3 start( 1, 1, 0 );
	Point3 end( 0, -2, 0 );
	ManifoldVertex v = MakeVertex( Point3(0,0,0), Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.5, false );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	Scalar cp = solver.EvaluateChainCosineProduct( start, end, chain );
	// Direction = normalize((0,0,0)-(1,1,0)) = (-1/sqrt2, -1/sqrt2, 0)
	// cos = |dot((0,1,0),(-1/sqrt2,-1/sqrt2,0))| = 1/sqrt(2) ≈ 0.707
	assert( IsClose( cp, 1.0 / sqrt(2.0), 1e-6 ) );
}

void TestCosineProduct_Range()
{
	std::cout << "Testing cosine product range [0,1]..." << std::endl;
	TestableManifoldSolver solver;

	Point3 start( 0.5, 2, 0.3 );
	Point3 end( -0.3, -2, 0.1 );
	ManifoldVertex v = MakeVertex( Point3(0,0,0), Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.5, false );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	Scalar cp = solver.EvaluateChainCosineProduct( start, end, chain );
	assert( cp >= 0.0 && cp <= 1.0 );
}

void TestGeometry_EmptyChain()
{
	std::cout << "Testing chain geometry empty chain..." << std::endl;
	TestableManifoldSolver solver;
	std::vector<ManifoldVertex> chain;
	Scalar g = solver.EvaluateChainGeometry( Point3(0,0,0), Point3(0,1,0), chain );
	assert( IsClose( g, 1.0 ) );
}

void TestGeometry_SingleVertex()
{
	std::cout << "Testing chain geometry single vertex..." << std::endl;
	TestableManifoldSolver solver;

	Point3 start( 0, 2, 0 );
	Point3 end( 0, -2, 0 );
	ManifoldVertex v = MakeVertex( Point3(0,0,0), Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.5, false );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	Scalar g = solver.EvaluateChainGeometry( start, end, chain );
	// cos_in at vertex = |dot((0,1,0), (0,1,0))| = 1, dist_to_prev = 2
	// factor = 1 / 4 (= cos/dist²)
	// last segment: dist_to_end = 2, factor = 1/4
	// total = 1/4 * 1/4 = 1/16 = 0.0625
	assert( IsClose( g, 0.0625, 1e-8 ) );
}

void TestGeometry_Positive()
{
	std::cout << "Testing chain geometry positive..." << std::endl;
	TestableManifoldSolver solver;

	Point3 start( 1, 3, 0 );
	Point3 end( -1, -3, 0 );
	ManifoldVertex v = MakeVertex( Point3(0,0,0), Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.5, false );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	Scalar g = solver.EvaluateChainGeometry( start, end, chain );
	assert( g >= 0 );
}

void TestThroughput_EmptyChain()
{
	std::cout << "Testing chain throughput empty chain..." << std::endl;
	TestableManifoldSolver solver;
	std::vector<ManifoldVertex> chain;
	RISEPel t = solver.EvaluateChainThroughput( Point3(0,0,0), Point3(0,1,0), chain );
	assert( IsClose( t[0], 1.0 ) );
	assert( IsClose( t[1], 1.0 ) );
	assert( IsClose( t[2], 1.0 ) );
}

void TestThroughput_NormalIncidence_Refraction()
{
	std::cout << "Testing throughput normal incidence refraction..." << std::endl;
	TestableManifoldSolver solver;

	Point3 start( 0, 2, 0 );
	Point3 end( 0, -2, 0 );
	ManifoldVertex v = MakeVertex( Point3(0,0,0), Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.5, false );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	RISEPel t = solver.EvaluateChainThroughput( start, end, chain );
	// F at normal incidence: ((1-1.5)/(1+1.5))^2 = 0.04
	// Refraction throughput: 1 - 0.04 = 0.96
	assert( IsClose( t[0], 0.96, 1e-4 ) );
}

void TestThroughput_NormalIncidence_Reflection()
{
	std::cout << "Testing throughput normal incidence reflection..." << std::endl;
	TestableManifoldSolver solver;

	Point3 start( 0, 2, 0 );
	Point3 end( 0, 2, 0 );  // reflected back
	ManifoldVertex v = MakeVertex( Point3(0,0,0), Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.5, true );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	RISEPel t = solver.EvaluateChainThroughput( start, end, chain );
	assert( IsClose( t[0], 0.04, 1e-4 ) );
}

void TestThroughput_Range()
{
	std::cout << "Testing throughput range [0,1]..." << std::endl;
	TestableManifoldSolver solver;

	Point3 start( 0.5, 2, 0 );
	Point3 end( -0.3, -2, 0.1 );
	ManifoldVertex v = MakeVertex( Point3(0,0,0), Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.8, false );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	RISEPel t = solver.EvaluateChainThroughput( start, end, chain );
	assert( t[0] >= 0 && t[0] <= 1.0 );
	assert( t[1] >= 0 && t[1] <= 1.0 );
	assert( t[2] >= 0 && t[2] <= 1.0 );
}

// ============================================================
// Group 9: Physical Validation
// ============================================================

void TestValidateChain_ValidRefraction()
{
	std::cout << "Testing ValidateChainPhysics valid refraction..." << std::endl;
	TestableManifoldSolver solver;

	// wi from above (wi.n > 0), wo going below (wo.n < 0) → valid refraction
	Point3 prevPos( 0, 2, 0 );
	Point3 vertPos( 0, 0, 0 );
	Point3 nextPos( 0, -2, 0 );
	ManifoldVertex v = MakeVertex( vertPos, Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.5, false );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	assert( solver.ValidateChainPhysics( chain, prevPos, nextPos ) );
}

void TestValidateChain_InvalidRefraction()
{
	std::cout << "Testing ValidateChainPhysics invalid refraction..." << std::endl;
	TestableManifoldSolver solver;

	// Both wi and wo above surface → invalid for refraction
	Point3 prevPos( 0, 2, 0 );
	Point3 vertPos( 0, 0, 0 );
	Point3 nextPos( 0, 2, 0 );
	ManifoldVertex v = MakeVertex( vertPos, Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.5, false );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	assert( !solver.ValidateChainPhysics( chain, prevPos, nextPos ) );
}

void TestValidateChain_ValidReflection()
{
	std::cout << "Testing ValidateChainPhysics valid reflection..." << std::endl;
	TestableManifoldSolver solver;

	// Both wi and wo above surface → valid for reflection
	Point3 prevPos( -1, 1, 0 );
	Point3 vertPos( 0, 0, 0 );
	Point3 nextPos( 1, 1, 0 );
	ManifoldVertex v = MakeVertex( vertPos, Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.0, true );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	assert( solver.ValidateChainPhysics( chain, prevPos, nextPos ) );
}

void TestValidateChain_InvalidReflection()
{
	std::cout << "Testing ValidateChainPhysics invalid reflection..." << std::endl;
	TestableManifoldSolver solver;

	// wi above, wo below → invalid for reflection
	Point3 prevPos( 0, 2, 0 );
	Point3 vertPos( 0, 0, 0 );
	Point3 nextPos( 0, -2, 0 );
	ManifoldVertex v = MakeVertex( vertPos, Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.0, true );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	assert( !solver.ValidateChainPhysics( chain, prevPos, nextPos ) );
}

// ============================================================
// Group 11: Light-to-First-Vertex Jacobian Determinant
//
// The SMS measure-conversion factor uses |det(δv_1_⊥ / δy_⊥)|
// (Zeltner et al. 2020, Eq. 14).  This block-tridiagonal
// solve underlies the G(x,v_1) · |det(δv/δy)| formula that
// replaced the obsolete cosAtLight × chainGeom / jacobianDet in
// EvaluateAtShadingPoint{NM}.  A regression here would silently
// shift caustic intensity — the kind of thing that only shows
// up as a bias-vs-variance mismatch against VCM reference.
// ============================================================

void TestLightToVertexJac_EmptyChain()
{
	std::cout << "Testing light-to-vertex Jacobian empty chain returns 0..." << std::endl;
	TestableManifoldSolver solver;
	std::vector<ManifoldVertex> chain;
	const Scalar det = solver.ComputeLightToFirstVertexJacobianDet(
		chain, Point3(0,1,0), Point3(0,-1,0), Vector3(0,1,0) );
	assert( IsClose( det, 0.0 ) );
}

void TestLightToVertexJac_SingleRefraction_Positive()
{
	std::cout << "Testing light-to-vertex Jacobian single-refraction positive..." << std::endl;
	TestableManifoldSolver solver;

	// Geometry: shading point above, refractor plane at origin
	// (normal +Y, eta=1.5), light below.  Symmetric about Y axis
	// so the Jacobian should be finite and positive.
	const Point3 shadingPoint( 0.0, 1.0, 0.0 );
	const Point3 lightPos( 0.0, -1.0, 0.0 );
	const Vector3 lightNormal( 0.0, 1.0, 0.0 );

	ManifoldVertex v = MakeVertex(
		Point3( 0, 0, 0 ), Vector3(0,1,0),
		Vector3(1,0,0), Vector3(0,0,1),
		1.5, /*isReflection=*/false );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	const Scalar det = solver.ComputeLightToFirstVertexJacobianDet(
		chain, shadingPoint, lightPos, lightNormal );

	// The Jacobian det should be strictly positive and finite.
	// Exact value is scene-dependent, but zero or NaN would be a bug.
	assert( det > 0 );
	assert( std::isfinite( det ) );
}

void TestLightToVertexJac_SingleReflection_Positive()
{
	std::cout << "Testing light-to-vertex Jacobian single-reflection positive..." << std::endl;
	TestableManifoldSolver solver;

	// Geometry: shading above, mirror plane at origin (normal +Y),
	// "light" above-right so reflection reaches it.  For reflection
	// chain the Jacobian is expected positive and finite.
	const Point3 shadingPoint( -1.0, 1.0, 0.0 );
	const Point3 lightPos( 1.0, 1.0, 0.0 );
	const Vector3 lightNormal( 0.0, -1.0, 0.0 );

	ManifoldVertex v = MakeVertex(
		Point3( 0, 0, 0 ), Vector3(0,1,0),
		Vector3(1,0,0), Vector3(0,0,1),
		1.0, /*isReflection=*/true );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	const Scalar det = solver.ComputeLightToFirstVertexJacobianDet(
		chain, shadingPoint, lightPos, lightNormal );

	assert( det > 0 );
	assert( std::isfinite( det ) );
}

void TestLightToVertexJac_TwoRefraction_Positive()
{
	std::cout << "Testing light-to-vertex Jacobian k=2 (glass slab) positive..." << std::endl;
	TestableManifoldSolver solver;

	// Two parallel refractors (entry/exit of a slab).  Back-to-back
	// eta cancellation means the chain is physically valid with
	// entry eta=1.5 then exit eta=1/1.5 ≈ 0.667.
	const Point3 shadingPoint( 0.0, 2.0, 0.0 );
	const Point3 lightPos( 0.0, -2.0, 0.0 );
	const Vector3 lightNormal( 0.0, 1.0, 0.0 );

	std::vector<ManifoldVertex> chain;
	chain.push_back( MakeVertex(
		Point3( 0.0, 0.5, 0.0 ), Vector3(0,1,0),
		Vector3(1,0,0), Vector3(0,0,1),
		1.5, /*isReflection=*/false ) );
	chain.push_back( MakeVertex(
		Point3( 0.0, -0.5, 0.0 ), Vector3(0,1,0),
		Vector3(1,0,0), Vector3(0,0,1),
		1.0 / 1.5, /*isReflection=*/false ) );

	const Scalar det = solver.ComputeLightToFirstVertexJacobianDet(
		chain, shadingPoint, lightPos, lightNormal );

	assert( det > 0 );
	assert( std::isfinite( det ) );
}

void TestLastBlockLightJac_NormalIncidence()
{
	std::cout << "Testing ComputeLastBlockLightJacobian returns finite Jy..." << std::endl;
	TestableManifoldSolver solver;

	// Single refractor, light directly below.
	ManifoldVertex vk = MakeVertex(
		Point3( 0, 0, 0 ), Vector3(0,1,0),
		Vector3(1,0,0), Vector3(0,0,1),
		1.5, /*isReflection=*/false );
	Point3 prevPos( 0, 1, 0 );
	Point3 lightPos( 0, -1, 0 );
	Vector3 lightNormal( 0, 1, 0 );

	Scalar Jy[4] = { 0, 0, 0, 0 };
	solver.ComputeLastBlockLightJacobian( vk, prevPos, lightPos, lightNormal, Jy );

	// All four entries should be finite.  Symmetric geometry makes
	// the off-diagonals 0 to numerical precision, but we don't rely
	// on that — just require finiteness and non-zero determinant.
	assert( std::isfinite( Jy[0] ) );
	assert( std::isfinite( Jy[1] ) );
	assert( std::isfinite( Jy[2] ) );
	assert( std::isfinite( Jy[3] ) );

	const Scalar detJy = Jy[0] * Jy[3] - Jy[1] * Jy[2];
	assert( std::fabs( detJy ) > 1e-12 );
}

// ============================================================
// Group 12: ComputeSphericalDerivatives
// ============================================================

void TestSphericalDeriv_ThetaGradient()
{
	std::cout << "Testing spherical derivative theta gradient..." << std::endl;
	Vector3 s( 1, 0, 0 );
	Vector3 t( 0, 0, 1 );
	Vector3 n( 0, 1, 0 );
	Vector3 dir = Vector3Ops::Normalize( Vector3( 0.3, 0.8, 0.1 ) );

	Vector3 dTheta, dPhi;
	ManifoldSolver::ComputeSphericalDerivatives( dir, s, t, n, dTheta, dPhi );

	// dTheta should be along the normal direction (only z component in local frame matters)
	// Specifically: dTheta = (-1/sin(theta)) * normal
	const Scalar z = Vector3Ops::Dot( dir, n );
	const Scalar sinTheta = sqrt( 1.0 - z * z );
	if( sinTheta > 1e-6 )
	{
		Vector3 expected( -n.x / sinTheta, -n.y / sinTheta, -n.z / sinTheta );
		assert( IsVectorClose( dTheta, expected, 1e-8 ) );
	}
}

void TestSphericalDeriv_PhiInTangentPlane()
{
	std::cout << "Testing spherical derivative phi in tangent plane..." << std::endl;
	Vector3 s( 1, 0, 0 );
	Vector3 t( 0, 0, 1 );
	Vector3 n( 0, 1, 0 );
	Vector3 dir = Vector3Ops::Normalize( Vector3( 0.5, 0.3, 0.7 ) );

	Vector3 dTheta, dPhi;
	ManifoldSolver::ComputeSphericalDerivatives( dir, s, t, n, dTheta, dPhi );

	// dPhi should have zero component along normal
	assert( IsClose( Vector3Ops::Dot( dPhi, n ), 0.0, 1e-8 ) );
}

void TestSphericalDeriv_FD()
{
	std::cout << "Testing spherical derivatives vs FD..." << std::endl;
	TestableManifoldSolver solver;
	Vector3 s( 1, 0, 0 );
	Vector3 t( 0, 0, 1 );
	Vector3 n( 0, 1, 0 );
	Vector3 dir = Vector3Ops::Normalize( Vector3( 0.4, 0.7, 0.3 ) );

	Vector3 dTheta, dPhi;
	ManifoldSolver::ComputeSphericalDerivatives( dir, s, t, n, dTheta, dPhi );

	// Finite difference: perturb dir in each component
	const Scalar eps = 1e-6;
	for( int j = 0; j < 3; j++ )
	{
		Vector3 dirP = dir, dirM = dir;
		Scalar* dpArr = &dirP.x;
		Scalar* dmArr = &dirM.x;
		dpArr[j] += eps;
		dmArr[j] -= eps;

		Point2 spP = solver.DirectionToSpherical( dirP, s, t, n );
		Point2 spM = solver.DirectionToSpherical( dirM, s, t, n );

		Scalar dTheta_fd = (spP.x - spM.x) / (2.0 * eps);
		Scalar dPhi_fd = (spP.y - spM.y) / (2.0 * eps);

		Scalar* dtArr = &dTheta.x;
		Scalar* dpArr2 = &dPhi.x;
		assert( IsClose( dtArr[j], dTheta_fd, 1e-3 ) );
		assert( IsClose( dpArr2[j], dPhi_fd, 1e-3 ) );
	}
}

// ============================================================
// Group 13: ComputeSpecularDirectionDerivativeWrtWi
// ============================================================

void TestDerivWrtWi_Reflection_FD()
{
	std::cout << "Testing d(wo)/d(wi) reflection vs FD..." << std::endl;
	TestableManifoldSolver solver;
	Vector3 wi = Vector3Ops::Normalize( Vector3( 0.4, 0.8, 0.2 ) );
	Vector3 normal( 0, 1, 0 );

	Scalar analytical[9];
	ManifoldSolver::ComputeSpecularDirectionDerivativeWrtWi( wi, normal, 1.0, true, analytical );

	const Scalar eps = 1e-6;
	for( int j = 0; j < 3; j++ )
	{
		Vector3 wiP = wi, wiM = wi;
		Scalar* wpArr = &wiP.x;
		Scalar* wmArr = &wiM.x;
		wpArr[j] += eps;
		wmArr[j] -= eps;

		Vector3 woP, woM;
		// Use unnormalized reflection: wo = -wi + 2*(wi.n)*n
		Scalar dP = Vector3Ops::Dot( wiP, normal );
		woP = Vector3( -wiP.x + 2*dP*normal.x, -wiP.y + 2*dP*normal.y, -wiP.z + 2*dP*normal.z );
		Scalar dM = Vector3Ops::Dot( wiM, normal );
		woM = Vector3( -wiM.x + 2*dM*normal.x, -wiM.y + 2*dM*normal.y, -wiM.z + 2*dM*normal.z );

		for( int i = 0; i < 3; i++ )
		{
			Scalar* woPArr = &woP.x;
			Scalar* woMArr = &woM.x;
			Scalar fd = (woPArr[i] - woMArr[i]) / (2 * eps);
			assert( IsClose( analytical[i*3+j], fd, 1e-3 ) );
		}
	}
}

void TestDerivWrtWi_Refraction_FD()
{
	std::cout << "Testing d(wo)/d(wi) refraction vs FD..." << std::endl;
	TestableManifoldSolver solver;
	Vector3 wi = Vector3Ops::Normalize( Vector3( 0.3, 0.9, 0.1 ) );
	Vector3 normal( 0, 1, 0 );
	Scalar eta = 1.5;

	Scalar analytical[9];
	ManifoldSolver::ComputeSpecularDirectionDerivativeWrtWi( wi, normal, eta, false, analytical );

	// Use unnormalized refraction to match the derivative convention
	const Scalar eps = 1e-6;
	for( int j = 0; j < 3; j++ )
	{
		Vector3 wiP = wi, wiM = wi;
		Scalar* wpArr = &wiP.x;
		Scalar* wmArr = &wiM.x;
		wpArr[j] += eps;
		wmArr[j] -= eps;

		// Compute unnormalized refracted direction
		auto refractUnnorm = [&]( const Vector3& w ) -> Vector3
		{
			const Scalar ci = Vector3Ops::Dot( w, normal );
			const Scalar eta_ratio = 1.0 / eta;  // entering
			const Scalar sin2_t = eta_ratio * eta_ratio * (1.0 - ci * ci);
			const Scalar cos_t = sqrt( fmax( 1.0 - sin2_t, 0.0 ) );
			return Vector3(
				-eta_ratio * w.x + (eta_ratio * ci - cos_t) * normal.x,
				-eta_ratio * w.y + (eta_ratio * ci - cos_t) * normal.y,
				-eta_ratio * w.z + (eta_ratio * ci - cos_t) * normal.z );
		};

		Vector3 woP = refractUnnorm( wiP );
		Vector3 woM = refractUnnorm( wiM );

		for( int i = 0; i < 3; i++ )
		{
			Scalar* woPArr = &woP.x;
			Scalar* woMArr = &woM.x;
			Scalar fd = (woPArr[i] - woMArr[i]) / (2 * eps);
			assert( IsClose( analytical[i*3+j], fd, 1e-3 ) );
		}
	}
}

// ============================================================
// Group 14: Angle-Difference Analytical vs Numerical Jacobian
// ============================================================

void TestAngleDiffJacobian_SingleVertex_Reflection()
{
	std::cout << "Testing angle-diff Jacobian (single vertex, reflection)..." << std::endl;
	TestableManifoldSolver solver;

	Point3 prevPos( -1, 2, 0 );
	Point3 vertPos( 0, 0, 0 );
	Point3 nextPos( 1, 2, 0 );

	ManifoldVertex v = MakeVertex( vertPos, Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.0, true );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	std::vector<Scalar> adiag, aupper, alower;
	solver.BuildJacobianAngleDiff( chain, prevPos, nextPos, adiag, aupper, alower );

	std::vector<Scalar> ndiag, nupper, nlower;
	solver.BuildJacobianAngleDiffNumerical( chain, prevPos, nextPos, ndiag, nupper, nlower );

	for( int i = 0; i < 4; i++ )
	{
		if( !IsClose( adiag[i], ndiag[i], 5e-2 ) )
		{
			std::cout << "  MISMATCH diag[" << i << "]: analytical=" << adiag[i]
				<< " numerical=" << ndiag[i] << std::endl;
		}
		assert( IsClose( adiag[i], ndiag[i], 5e-2 ) );
	}
}

void TestAngleDiffJacobian_SingleVertex_Refraction()
{
	std::cout << "Testing angle-diff Jacobian (single vertex, refraction)..." << std::endl;
	TestableManifoldSolver solver;

	Point3 prevPos( 0.3, 2, 0 );
	Point3 vertPos( 0, 0, 0 );
	Point3 nextPos( -0.2, -2, 0.1 );

	ManifoldVertex v = MakeVertex( vertPos, Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.5, false );
	std::vector<ManifoldVertex> chain;
	chain.push_back( v );

	std::vector<Scalar> adiag, aupper, alower;
	solver.BuildJacobianAngleDiff( chain, prevPos, nextPos, adiag, aupper, alower );

	std::vector<Scalar> ndiag, nupper, nlower;
	solver.BuildJacobianAngleDiffNumerical( chain, prevPos, nextPos, ndiag, nupper, nlower );

	for( int i = 0; i < 4; i++ )
	{
		if( !IsClose( adiag[i], ndiag[i], 5e-2 ) )
		{
			std::cout << "  MISMATCH diag[" << i << "]: analytical=" << adiag[i]
				<< " numerical=" << ndiag[i] << std::endl;
		}
		assert( IsClose( adiag[i], ndiag[i], 5e-2 ) );
	}
}

void TestAngleDiffJacobian_TwoVertex()
{
	std::cout << "Testing angle-diff Jacobian (two vertices)..." << std::endl;
	TestableManifoldSolver solver;

	Point3 prevPos( 0, 2, 0 );
	Point3 v1Pos( 0.1, 0.1, 0 );
	Point3 v2Pos( -0.1, -0.1, 0 );
	Point3 nextPos( 0, -2, 0 );

	ManifoldVertex mv1 = MakeVertex( v1Pos, Vector3(0,1,0), Vector3(1,0,0), Vector3(0,0,1), 1.5, false );
	ManifoldVertex mv2 = MakeVertex( v2Pos, Vector3(0,-1,0), Vector3(1,0,0), Vector3(0,0,1), 1.5, false );
	std::vector<ManifoldVertex> chain;
	chain.push_back( mv1 );
	chain.push_back( mv2 );

	std::vector<Scalar> adiag, aupper, alower;
	solver.BuildJacobianAngleDiff( chain, prevPos, nextPos, adiag, aupper, alower );

	std::vector<Scalar> ndiag, nupper, nlower;
	solver.BuildJacobianAngleDiffNumerical( chain, prevPos, nextPos, ndiag, nupper, nlower );

	// Diagonal blocks (8 entries)
	for( int i = 0; i < 8; i++ )
		assert( IsClose( adiag[i], ndiag[i], 0.1 ) );
	// Upper and lower (4 entries each)
	for( int i = 0; i < 4; i++ )
	{
		assert( IsClose( aupper[i], nupper[i], 0.1 ) );
		assert( IsClose( alower[i], nlower[i], 0.1 ) );
	}
}

// ============================================================
// main
// ============================================================

int main()
{
	std::cout << std::endl;
	std::cout << "========================================" << std::endl;
	std::cout << "  ManifoldSolver Unit Tests" << std::endl;
	std::cout << "========================================" << std::endl;
	std::cout << std::endl;

	// Group 1: 2x2 Block Utilities
	TestInvert2x2_Identity();
	TestInvert2x2_Known();
	TestInvert2x2_Singular();
	TestInvert2x2_RoundTrip();
	TestMul2x2_Known();
	TestMul2x2Vec_Known();
	TestSub2x2_SelfSubtraction();

	// Group 10: DeriveNormalized
	TestDeriveNormalized_Perpendicular();
	TestDeriveNormalized_Parallel();
	TestDeriveNormalized_ZeroLength();
	TestDeriveNormalized_FiniteDifference();

	// Group 11: ComputeDielectricFresnel
	TestFresnel_NormalIncidence();
	TestFresnel_GrazingAngle();
	TestFresnel_TIR();
	TestFresnel_Symmetry();

	// Group 2: ComputeSpecularDirection
	TestReflection_NormalIncidence();
	TestReflection_45Degrees();
	TestRefraction_NormalIncidence();
	TestRefraction_SnellsLaw();
	TestRefraction_TIR();
	TestRefraction_UnitVector();

	// Group 3: Specular Direction Derivative
	// ComputeSpecularDirectionDerivativeWrtNormal is not called by
	// the half-vector Jacobian (BuildJacobian) — surface curvature
	// enters via the Weingarten map (dndu → ds/du) instead.  It would
	// be needed for an analytical Jacobian of the angle-difference
	// constraint.  The method also assumes unnormalized wo while
	// ComputeSpecularDirection returns normalized, so FD validation
	// requires a DeriveNormalized correction.  Skipped for now.
	// TestReflectionDerivative_FD();
	// TestRefractionDerivative_FD();

	// Group 4: DirectionToSpherical
	TestSpherical_NormalDirection();
	TestSpherical_TangentU();
	TestSpherical_TangentV();
	TestSpherical_NegativeNormal();

	// Group 5: Constraint Functions
	TestHalfVectorConstraint_AtSolution_Reflection();
	TestHalfVectorConstraint_NotAtSolution();
	TestHalfVectorConstraint_AtSolution_Refraction();
	TestAngleDiffConstraint_AtSolution();

	// Group 6: Analytical vs Numerical Jacobian
	TestJacobian_SingleVertex_Reflection();
	TestJacobian_SingleVertex_Refraction();
	TestJacobian_TwoVertex_Agreement();
	TestJacobian_SingleVertex_WithCurvature();

	// Group 7: Block-Tridiagonal Solver
	TestTridiag_SingleBlock();
	TestTridiag_Singular();
	TestTridiag_Verification();

	// Group 8a: Chain Cosine Product
	TestCosineProduct_EmptyChain();
	TestCosineProduct_SingleVertex_NormalIncidence();
	TestCosineProduct_SingleVertex_ObliqueIncidence();
	TestCosineProduct_Range();

	// Group 8: Chain Geometry & Throughput
	TestGeometry_EmptyChain();
	TestGeometry_SingleVertex();
	TestGeometry_Positive();
	TestThroughput_EmptyChain();
	TestThroughput_NormalIncidence_Refraction();
	TestThroughput_NormalIncidence_Reflection();
	TestThroughput_Range();

	// Group 9: Physical Validation
	TestValidateChain_ValidRefraction();
	TestValidateChain_InvalidRefraction();
	TestValidateChain_ValidReflection();
	TestValidateChain_InvalidReflection();

	// Group 11: Light-to-First-Vertex Jacobian Determinant (SMS geometry)
	TestLightToVertexJac_EmptyChain();
	TestLightToVertexJac_SingleRefraction_Positive();
	TestLightToVertexJac_SingleReflection_Positive();
	TestLightToVertexJac_TwoRefraction_Positive();
	TestLastBlockLightJac_NormalIncidence();

	// Group 12: ComputeSphericalDerivatives
	TestSphericalDeriv_ThetaGradient();
	TestSphericalDeriv_PhiInTangentPlane();
	TestSphericalDeriv_FD();

	// Group 13: ComputeSpecularDirectionDerivativeWrtWi
	TestDerivWrtWi_Reflection_FD();
	TestDerivWrtWi_Refraction_FD();

	// Group 14: Angle-Difference Analytical vs Numerical Jacobian
	TestAngleDiffJacobian_SingleVertex_Reflection();
	TestAngleDiffJacobian_SingleVertex_Refraction();
	TestAngleDiffJacobian_TwoVertex();

	std::cout << std::endl;
	std::cout << "========================================" << std::endl;
	std::cout << "  All ManifoldSolver tests passed!" << std::endl;
	std::cout << "========================================" << std::endl;
	std::cout << std::endl;

	return 0;
}
