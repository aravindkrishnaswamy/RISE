//////////////////////////////////////////////////////////////////////
//
//  GrazingSnellFresnelTest.cpp - DL-58 direct boundary regressions.
//
//  At a matched-index boundary there is no optical interface.  In
//  particular, a tangent ray must remain a valid, unchanged transmitted
//  ray; treating a tiny non-negative Snell radicand as TIR drops valid
//  transport.  This test also keeps the cosine-based Fresnel helpers
//  aligned at that identity and at real (unequal-index) critical angles.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <iostream>

#include "../src/Library/Materials/FibreLobeMath.h"
#include "../src/Library/Utilities/ManifoldSolver.h"
#include "../src/Library/Utilities/Optics.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int gChecks = 0;
	int gFailures = 0;

	void Check( const bool condition, const char* const label )
	{
		++gChecks;
		if( !condition ) {
			std::cerr << "FAIL: " << label << std::endl;
			++gFailures;
		}
	}

	bool IsFiniteUnit( const Vector3& v, const Scalar tolerance = 1e-10 )
	{
		return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z ) &&
			std::fabs( Vector3Ops::Magnitude( v ) - Scalar(1) ) <= tolerance;
	}

	void CheckNear( const Scalar actual, const long double expected,
		const char* const label, const Scalar tolerance = 1e-10 )
	{
		++gChecks;
		const long double difference = std::fabs( (long double)actual - expected );
		if( !std::isfinite( actual ) || !std::isfinite( expected ) || difference > (long double)tolerance ) {
			std::cerr << "FAIL: " << label << " (got " << actual
				<< ", expected " << (double)expected << ")" << std::endl;
			++gFailures;
		}
	}

	// Scalar physical Fresnel reference for unmatched controls kept safely
	// away from the cancellation band. Long double may alias double on
	// this platform; matched-index expectations use the exact zero identity.
	long double PhysicalFresnel( const long double ni, const long double nt,
		const long double cosine )
	{
		const long double c = std::fabs( cosine );
		const long double sinT2 = (ni / nt) * (ni / nt) * (1.0L - c * c);
		if( sinT2 > 1.0L ) {
			return 1.0L;
		}
		const long double ct = std::sqrt( 1.0L - sinT2 );
		const long double rs = (ni * c - nt * ct) / (ni * c + nt * ct);
		const long double rp = (nt * c - ni * ct) / (nt * c + ni * ct);
		return 0.5L * (rs * rs + rp * rp);
	}

	Vector3 Incident( const Scalar cosine )
	{
		return Vector3( std::sqrt( Scalar(1) - cosine * cosine ), 0, -cosine );
	}

	void TestMatchedIndexGrazingSnell()
	{
		std::cout << "Matched-index grazing Snell" << std::endl;
		const Scalar grazing[] = { Scalar(1e-7), Scalar(1e-9), Scalar(0) };
		for( unsigned int i = 0; i < 3; ++i ) {
			const Vector3 before = Incident( grazing[i] );
			Vector3 transmitted = before;
			const bool refracted = Optics::CalculateRefractedRay(
				Vector3(0,0,1), Scalar(1.5), Scalar(1.5), transmitted );
			Check( refracted, "matched media preserve a grazing transmitted ray" );
			if( refracted ) {
				Check( IsFiniteUnit( transmitted ), "matched grazing transmission is finite and unit length" );
				CheckNear( transmitted.x, (long double)before.x, "matched grazing keeps tangential direction" );
				CheckNear( transmitted.y, (long double)before.y, "matched grazing keeps y direction" );
				CheckNear( transmitted.z, (long double)before.z, "matched grazing keeps normal direction" );
			}
		}
	}

	void TestDirectFresnelIdentity()
	{
		std::cout << "Matched-index direct Fresnel identity" << std::endl;
		const Scalar cosines[] = { Scalar(0), Scalar(1e-9), Scalar(-1e-9) };
		for( unsigned int i = 0; i < 3; ++i ) {
			const Scalar c = cosines[i];
			const Scalar fibre = RISE::FibreLobeMath::FrDielectric( c, Scalar(1) );
			const Scalar manifold = ManifoldSolver::ComputeDielectricFresnel( c, Scalar(1), Scalar(1) );
			Check( std::isfinite(fibre) && std::isfinite(manifold), "matched Fresnel results are finite" );
			CheckNear( fibre, 0.0L, "fibre signed-cosine matched identity is zero" );
			CheckNear( manifold, 0.0L, "manifold matched identity is zero" );
		}
	}

	void TestUnequalIndexControls()
	{
		std::cout << "Unequal-index Snell and Fresnel controls" << std::endl;
		const Scalar ni = Scalar(1.5);
		const Scalar nt = Scalar(1.0);

		// Normal incidence activates the ordinary Fresnel path independently
		// of the grazing cancellation this regression targets.
		CheckNear( ManifoldSolver::ComputeDielectricFresnel( Scalar(1), ni, nt ),
			PhysicalFresnel( 1.5L, 1.0L, 1.0L ), "manifold normal-incidence Fresnel" );
		CheckNear( RISE::FibreLobeMath::FrDielectric( Scalar(1), Scalar(1.5) ),
			PhysicalFresnel( 1.0L, 1.5L, 1.0L ), "fibre normal-incidence Fresnel" );

		// A nearby, real interface must still bend a grazing ray.  This is an
		// activity control: the matched-media pass above is not just a bypass.
		const Scalar nearNt = Scalar(1.5003);
		Vector3 transmitted = Incident( Scalar(1e-7) );
		Check( Optics::CalculateRefractedRay( Vector3(0,0,1), ni, nearNt, transmitted ),
			"nearby unequal media refract at grazing" );
		Check( IsFiniteUnit(transmitted), "nearby unequal grazing transmission is finite and unit" );
		Check( std::fabs( transmitted.z - Scalar(-1e-7) ) > Scalar(1e-11),
			"nearby unequal media actively change the transmitted direction" );

		const long double critical = std::sqrt( 1.0L - (1.0L/1.5L)*(1.0L/1.5L) );
		const Scalar transmittedSide = (Scalar)(critical + 1e-7L);
		const Scalar tirSide = (Scalar)(critical - 1e-7L);
		Check( PhysicalFresnel(1.5L,1.0L,(long double)transmittedSide) < 1.0L,
			"independent reference says offset above critical transmits" );
		CheckNear( ManifoldSolver::ComputeDielectricFresnel( transmittedSide, ni, nt ),
			PhysicalFresnel(1.5L,1.0L,(long double)transmittedSide), "manifold above-critical Fresnel" );
		CheckNear( RISE::FibreLobeMath::FrDielectric( -transmittedSide, Scalar(1.5) ),
			PhysicalFresnel(1.5L,1.0L,(long double)transmittedSide), "fibre above-critical Fresnel" );

		Vector3 refracted = Incident( transmittedSide );
		Check( Optics::CalculateRefractedRay(Vector3(0,0,1),ni,nt,refracted),
			"above-critical offset has a finite Snell transmission" );
		Check( IsFiniteUnit(refracted), "above-critical Snell direction is finite and unit" );
		Check( PhysicalFresnel(1.5L,1.0L,(long double)tirSide) == 1.0L,
			"independent reference says offset below critical is TIR" );
		CheckNear( ManifoldSolver::ComputeDielectricFresnel( tirSide, ni, nt ), 1.0L,
			"manifold below-critical offset is TIR" );
		CheckNear( RISE::FibreLobeMath::FrDielectric( -tirSide, Scalar(1.5) ), 1.0L,
			"fibre below-critical offset is TIR" );
		Vector3 tir = Incident( tirSide );
		Check( !Optics::CalculateRefractedRay(Vector3(0,0,1),ni,nt,tir),
			"below-critical offset rejects Snell transmission" );
	}
}

int main()
{
	TestMatchedIndexGrazingSnell();
	TestDirectFresnelIdentity();
	TestUnequalIndexControls();
	std::cout << "Checks: " << gChecks << "  Failures: " << gFailures << std::endl;
	return gFailures ? 1 : 0;
}
