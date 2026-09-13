//////////////////////////////////////////////////////////////////////
//
//  ManifoldNormalDerivativeTest.cpp - DL-59 normal-derivative guards.
//
//  The derivative routine returns d(rawWo)/d(n), while
//  ComputeSpecularDirection returns normalize(rawWo).  These tests compare
//  the appropriately normalized analytic derivative against central finite
//  differences of the actual direction routine under unit-normal
//  tangent perturbations.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <iostream>
#include <iomanip>
#include <vector>

#include "TestableManifoldSolver.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
struct Checks
{
	unsigned int count;
	unsigned int failures;

	Checks() : count( 0 ), failures( 0 ) {}

	void Check( bool condition, const char* label )
	{
		++count;
		if( !condition ) {
			++failures;
			std::cerr << "FAIL: " << label << std::endl;
		}
	}
};

bool IsFinite( Scalar value )
{
	// Materialize through volatile so this remains a real non-finite check in
	// optimized test builds as well as in debug builds.
	const volatile Scalar observed = value;
	return std::isfinite( observed );
}

bool IsFinite( const Vector3& v )
{
	return IsFinite( v.x ) && IsFinite( v.y ) && IsFinite( v.z );
}

Scalar DifferenceMagnitude( const Vector3& a, const Vector3& b )
{
	return Vector3Ops::Magnitude( Vector3( a.x - b.x, a.y - b.y, a.z - b.z ) );
}

Vector3 MatrixTimesVector( const Scalar matrix[9], const Vector3& v )
{
	return Vector3(
		matrix[0] * v.x + matrix[1] * v.y + matrix[2] * v.z,
		matrix[3] * v.x + matrix[4] * v.y + matrix[5] * v.z,
		matrix[6] * v.x + matrix[7] * v.y + matrix[8] * v.z );
}

// An independent raw Snell expression, used only to obtain the length and
// direction required by the normalization derivative.  It deliberately
// mirrors the physical entering/exiting convention rather than calling the
// derivative implementation under test.
bool PhysicalRawRefraction(
	const Vector3& wi,
	const Vector3& normal,
	Scalar eta,
	Vector3& rawWo )
{
	const Scalar signedCosI = Vector3Ops::Dot( wi, normal );
	const bool entering = signedCosI >= 0.0;
	const Vector3 orientedNormal = entering ? normal : Vector3( -normal.x, -normal.y, -normal.z );
	const Scalar etaRatio = entering ? 1.0 / eta : eta;
	const Scalar cosI = std::fabs( signedCosI );
	const Scalar sin2T = etaRatio * etaRatio * ( 1.0 - cosI * cosI );
	if( sin2T > 1.0 ) return false;

	const Scalar cosT = std::sqrt( std::fmax( 0.0, 1.0 - sin2T ) );
	rawWo = Vector3(
		-etaRatio * wi.x + ( etaRatio * cosI - cosT ) * orientedNormal.x,
		-etaRatio * wi.y + ( etaRatio * cosI - cosT ) * orientedNormal.y,
		-etaRatio * wi.z + ( etaRatio * cosI - cosT ) * orientedNormal.z );
	return true;
}

Vector3 NormalizedOutputDerivative(
	const Vector3& rawWo,
	const Scalar rawDerivative[9],
	const Vector3& normalTangent )
{
	const Scalar rawLength = Vector3Ops::Magnitude( rawWo );
	const Vector3 output = rawWo * ( 1.0 / rawLength );
	const Vector3 rawChange = MatrixTimesVector( rawDerivative, normalTangent );
	const Scalar radial = Vector3Ops::Dot( output, rawChange );
	return Vector3(
		( rawChange.x - output.x * radial ) / rawLength,
		( rawChange.y - output.y * radial ) / rawLength,
		( rawChange.z - output.z * radial ) / rawLength );
}

bool DirectionFiniteDifference(
	TestableManifoldSolver& solver,
	const Vector3& wi,
	const Vector3& normal,
	Scalar eta,
	bool isReflection,
	const Vector3& normalTangent,
	Scalar epsilon,
	Vector3& finiteDifference )
{
	const Vector3 plusNormal = Vector3Ops::Normalize( normal + normalTangent * epsilon );
	const Vector3 minusNormal = Vector3Ops::Normalize( normal - normalTangent * epsilon );
	Vector3 plusDirection, minusDirection;
	if( !solver.ComputeSpecularDirection( wi, plusNormal, eta, isReflection, plusDirection ) ||
		!solver.ComputeSpecularDirection( wi, minusNormal, eta, isReflection, minusDirection ) ) {
		return false;
	}
	finiteDifference = Vector3(
		( plusDirection.x - minusDirection.x ) / ( 2.0 * epsilon ),
		( plusDirection.y - minusDirection.y ) / ( 2.0 * epsilon ),
		( plusDirection.z - minusDirection.z ) / ( 2.0 * epsilon ) );
	return true;
}

// epsilon=1e-6 balances double-precision central-difference roundoff
// (roughly epsilon_machine/epsilon) against the O(epsilon^2) truncation term.
// These well-conditioned, non-grazing fixtures therefore need only 2e-6;
// no tolerance is shared with small-cosine or TIR cases.
const Scalar kDerivativeEpsilon = 1.0e-6;
const Scalar kDerivativeTolerance = 2.0e-6;

void CheckDerivativeCase(
	Checks& checks,
	const char* label,
	const Vector3& wi,
	const Vector3& normal,
	Scalar eta,
	bool isReflection,
	bool expectActivity )
{
	TestableManifoldSolver solver;
	const Vector3 tangentX = Vector3Ops::Normalize( Vector3( 1.0, 0.0, 0.0 ) - normal * Vector3Ops::Dot( normal, Vector3( 1.0, 0.0, 0.0 ) ) );
	const Vector3 tangentY = Vector3Ops::Normalize( Vector3Ops::Cross( normal, tangentX ) );
	const Vector3 tangents[2] = { tangentX, tangentY };

	Vector3 rawWo;
	bool rawOk = true;
	if( !isReflection ) rawOk = PhysicalRawRefraction( wi, normal, eta, rawWo );
	else {
		const Scalar d = Vector3Ops::Dot( wi, normal );
		rawWo = Vector3( -wi.x + 2.0 * d * normal.x,
			-wi.y + 2.0 * d * normal.y, -wi.z + 2.0 * d * normal.z );
	}
	checks.Check( rawOk && IsFinite( rawWo ) && Vector3Ops::Magnitude( rawWo ) > 0.0, label );
	if( !rawOk || !IsFinite( rawWo ) || Vector3Ops::Magnitude( rawWo ) <= 0.0 ) return;

	Scalar derivative[9];
	solver.ComputeSpecularDirectionDerivativeWrtNormal( wi, normal, eta, isReflection, derivative );
	for( unsigned int i = 0; i < 9; ++i ) checks.Check( IsFinite( derivative[i] ), label );

	for( unsigned int t = 0; t < 2; ++t )
	{
		Vector3 numerical;
		const bool fdOk = DirectionFiniteDifference( solver, wi, normal, eta,
			isReflection, tangents[t], kDerivativeEpsilon, numerical );
		const Vector3 analytical = NormalizedOutputDerivative( rawWo, derivative, tangents[t] );
		checks.Check( fdOk && IsFinite( numerical ) && IsFinite( analytical ), label );
		if( !fdOk || !IsFinite( numerical ) || !IsFinite( analytical ) ) continue;

		const Scalar activity = Vector3Ops::Magnitude( numerical );
		checks.Check( expectActivity ? activity > 1.0e-4 : activity < kDerivativeTolerance, label );
		const Scalar error = DifferenceMagnitude( analytical, numerical );
		std::cout << "DERIVATIVE " << label << " tangent=" << t
			<< " FD=(" << numerical.x << "," << numerical.y << "," << numerical.z
			<< ") analytical=(" << analytical.x << "," << analytical.y << "," << analytical.z
			<< ") error=" << error << std::endl;
		checks.Check( error < kDerivativeTolerance, label );
	}
}

void TestDirectionDerivativeConventions( Checks& checks )
{
	const Vector3 z( 0.0, 0.0, 1.0 );

	// DL-59 regression: at normal incidence these have tangential derivatives
	// -1/3 (air -> eta=1.5) and -1/2 (eta=1.5 -> air), respectively.
	CheckDerivativeCase( checks, "entry normal refraction", z, z, 1.5, false, true );
	CheckDerivativeCase( checks, "exit normal refraction", Vector3( 0.0, 0.0, -1.0 ), z, 1.5, false, true );

	CheckDerivativeCase( checks, "entry oblique refraction",
		Vector3Ops::Normalize( Vector3( 0.31, -0.22, 0.925 ) ), z, 1.5, false, true );
	CheckDerivativeCase( checks, "exit oblique refraction",
		Vector3Ops::Normalize( Vector3( -0.28, 0.19, -0.94 ) ), z, 1.2, false, true );

	// Exact matched-index contract: transmission is -wi and its normal
	// derivative vanishes for both interface orientations.
	CheckDerivativeCase( checks, "matched eta entry identity",
		Vector3Ops::Normalize( Vector3( 0.3, -0.2, 0.93 ) ), z, 1.0, false, false );
	CheckDerivativeCase( checks, "matched eta exit identity",
		Vector3Ops::Normalize( Vector3( 0.3, -0.2, -0.93 ) ), z, 1.0, false, false );

	// Reflection is a convention-independent control for the same normalized
	// normal perturbation and derivative correction.
	CheckDerivativeCase( checks, "reflection normal-derivative control",
		Vector3Ops::Normalize( Vector3( 0.31, -0.27, 0.91 ) ), z, 1.5, true, true );
}

void TestNormalIncidenceClosedForm( Checks& checks )
{
	TestableManifoldSolver solver;
	const Vector3 z( 0.0, 0.0, 1.0 );
	const Vector3 x( 1.0, 0.0, 0.0 );
	const Vector3 expectedEntry( -1.0 / 3.0, 0.0, 0.0 );
	const Vector3 expectedExit( -0.5, 0.0, 0.0 );
	Vector3 actualEntry, actualExit;
	const bool entryOk = DirectionFiniteDifference( solver, z, z, 1.5, false,
		x, kDerivativeEpsilon, actualEntry );
	const bool exitOk = DirectionFiniteDifference( solver, Vector3( 0.0, 0.0, -1.0 ),
		z, 1.5, false, x, kDerivativeEpsilon, actualExit );

	// These are closed-form normal-incidence Snell derivatives.  They make
	// the ratio and exiting-normal convention visible even if a future change
	// accidentally makes both the implementation and a derivative helper agree.
	checks.Check( entryOk && IsFinite( expectedEntry ) && IsFinite( actualEntry ),
		"entry normal closed-form derivative is finite" );
	checks.Check( exitOk && IsFinite( expectedExit ) && IsFinite( actualExit ),
		"exit normal closed-form derivative is finite" );
	if( entryOk && IsFinite( actualEntry ) ) {
		checks.Check( DifferenceMagnitude( expectedEntry, actualEntry ) < kDerivativeTolerance,
			"entry normal derivative is -1/3" );
	}
	if( exitOk && IsFinite( actualExit ) ) {
		checks.Check( DifferenceMagnitude( expectedExit, actualExit ) < kDerivativeTolerance,
			"exit normal derivative is -1/2" );
	}
}

void TestPhysicalDirectionControls( Checks& checks )
{
	TestableManifoldSolver solver;
	const Vector3 z( 0.0, 0.0, 1.0 );
	const Vector3 entering = Vector3Ops::Normalize( Vector3( 0.42, 0.0, 0.91 ) );
	const Vector3 exiting = Vector3Ops::Normalize( Vector3( 0.42, 0.0, -0.91 ) );
	Vector3 direction, raw;

	checks.Check( solver.ComputeSpecularDirection( entering, z, 1.5, false, direction ) &&
		PhysicalRawRefraction( entering, z, 1.5, raw ) && IsFinite( direction ) &&
		std::fabs( Vector3Ops::Magnitude( direction ) - 1.0 ) < 1.0e-12 &&
		DifferenceMagnitude( direction, Vector3Ops::Normalize(raw) ) < 1.0e-12,
		"physically valid entering transmission" );
	checks.Check( solver.ComputeSpecularDirection( exiting, z, 1.2, false, direction ) &&
		PhysicalRawRefraction( exiting, z, 1.2, raw ) && IsFinite( direction ) &&
		std::fabs( Vector3Ops::Magnitude( direction ) - 1.0 ) < 1.0e-12 &&
		DifferenceMagnitude( direction, Vector3Ops::Normalize(raw) ) < 1.0e-12,
		"physically valid exiting transmission" );

	const Vector3 tirWi = Vector3Ops::Normalize( Vector3( 0.8, 0.0, -0.6 ) );
	checks.Check( !solver.ComputeSpecularDirection( tirWi, z, 1.5, false, direction ) &&
		!PhysicalRawRefraction( tirWi, z, 1.5, raw ), "physical TIR control" );
}

ManifoldVertex MakeCurvedVertex(
	const Vector3& normal,
	const Vector3& dpdu,
	const Vector3& dpdv,
	Scalar eta )
{
	ManifoldVertex vertex;
	vertex.position = Point3( 0.0, 0.0, 0.0 );
	vertex.normal = normal;
	vertex.geomNormal = normal;
	vertex.dpdu = dpdu;
	vertex.dpdv = dpdv;
	// A locally spherical, curved frame: the normal changes tangentially in
	// both parameter directions.  The fixture intentionally stays away from
	// spherical-coordinate poles.
	vertex.dndu = dpdu * 0.35;
	vertex.dndv = dpdv * 0.35;
	vertex.eta = eta;
	vertex.isReflection = false;
	vertex.valid = true;
	return vertex;
}

void CheckCurvedAngleDiffJacobian(
	Checks& checks, const char* label, const Vector3& wi, Scalar eta )
{
	TestableManifoldSolver solver;
	const Vector3 normal = Vector3Ops::Normalize( Vector3( 0.32, -0.27, 0.91 ) );
	const Vector3 dpdu = Vector3Ops::Normalize( Vector3( normal.z, 0.0, -normal.x ) );
	const Vector3 dpdv = Vector3Ops::Normalize( Vector3Ops::Cross( normal, dpdu ) );
	ManifoldVertex vertex = MakeCurvedVertex( normal, dpdu, dpdv, eta );

	Vector3 wo;
	const bool directionOk = solver.ComputeSpecularDirection( wi, normal, eta, false, wo );
	checks.Check( directionOk && IsFinite( wo ), label );
	if( !directionOk || !IsFinite( wo ) ) return;

	std::vector<ManifoldVertex> chain( 1, vertex );
	const Point3 start = Point3Ops::mkPoint3( vertex.position, wi * 1.7 );
	const Point3 end = Point3Ops::mkPoint3( vertex.position, wo * 1.9 );
	std::vector<Scalar> analyticalDiag, analyticalUpper, analyticalLower;
	std::vector<Scalar> numericalDiag, numericalUpper, numericalLower;
	solver.BuildJacobianAngleDiff( chain, start, end,
		analyticalDiag, analyticalUpper, analyticalLower );
	solver.BuildJacobianAngleDiffNumerical( chain, start, end,
		numericalDiag, numericalUpper, numericalLower );

	checks.Check( analyticalDiag.size() == 4 && numericalDiag.size() == 4, label );
	if( analyticalDiag.size() != 4 || numericalDiag.size() != 4 ) return;
	Scalar numericalActivity = 0;
	for( unsigned int i = 0; i < 4; ++i ) {
		numericalActivity += std::fabs(numericalDiag[i]);
		std::cout << "CURVED " << label << " entry=" << i
			<< " analytical=" << analyticalDiag[i] << " numerical=" << numericalDiag[i] << std::endl;
		checks.Check( IsFinite( analyticalDiag[i] ) && IsFinite( numericalDiag[i] ), label );
		// The numerical builder uses eps=1e-5.  This non-pole fixture has
		// order-one geometry; the direct-case error budget also accommodates
		// its central-difference truncation and roundoff.
		checks.Check( std::fabs( analyticalDiag[i] - numericalDiag[i] ) < kDerivativeTolerance, label );
	}
	checks.Check( IsFinite(numericalActivity) && numericalActivity > 0.1,
		"curved numerical Jacobian has positive activity" );
}

void TestCurvedAngleDiffJacobians( Checks& checks )
{
	const Vector3 normal = Vector3Ops::Normalize( Vector3( 0.32, -0.27, 0.91 ) );
	const Vector3 tangent = Vector3Ops::Normalize( Vector3( normal.z, 0.0, -normal.x ) );
	CheckCurvedAngleDiffJacobian( checks, "curved angle-diff entry",
		Vector3Ops::Normalize( normal * 0.87 + tangent * 0.49 ), 1.5 );
	CheckCurvedAngleDiffJacobian( checks, "curved angle-diff exit",
		Vector3Ops::Normalize( normal * -0.94 + tangent * 0.34 ), 1.2 );
}
}

int main()
{
	std::cout << std::setprecision(17);
	Checks checks;
	TestDirectionDerivativeConventions( checks );
	TestNormalIncidenceClosedForm( checks );
	TestPhysicalDirectionControls( checks );
	TestCurvedAngleDiffJacobians( checks );

	std::cout << "Checks: " << checks.count << " Failures: " << checks.failures << std::endl;
	return checks.failures == 0 ? 0 : 1;
}
