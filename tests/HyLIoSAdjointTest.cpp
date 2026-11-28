//////////////////////////////////////////////////////////////////////
//
//  HyLIoSAdjointTest.cpp - P4/P7 reciprocity, PDF/sampling unbiasedness,
//    smooth-boundary delta-NEE refusal, and energy-conservation tests
//    for `rise_reciprocal_boundary_v1` (HyLIoSReciprocalBoundary).
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <iostream>
#include <string>

#include "../src/Library/Materials/HyLIoS/HyLIoSReciprocalBoundary.h"
#include "../src/Library/Utilities/RandomNumbers.h"

using namespace RISE::HyLIoS;

namespace {

int gFailures = 0;

void Check( const bool cond, const std::string& msg )
{
	if( !cond ) {
		++gFailures;
		std::cerr << "FAIL: " << msg << "\n";
	}
}

void Near( const double actual, const double expected, const double tol, const std::string& msg )
{
	if( !std::isfinite( actual ) || std::fabs( actual - expected ) > tol ) {
		++gFailures;
		std::cerr << "FAIL: " << msg << " (actual=" << actual << ", expected=" << expected
				  << ", tol=" << tol << ")\n";
	}
}

SlabVector Norm( const double x, const double y, const double z )
{
	const double l = std::sqrt( x * x + y * y + z * z );
	return { x / l, y / l, z / l };
}

void TestReflectionAndTransmissionReciprocity()
{
	const double etaExt = 1.0;
	const double etaInt = 1.45;
	const HyLIoSReciprocalBoundary b( 0.30, etaExt, etaInt );
	Check( b.AllowsDeltaLightNEE(), "Rough reciprocal boundary allows delta light NEE" );

	// 1. Reflection reciprocity: K_r(wi, wo) == K_r(wo, wi)
	const SlabVector dirsUpper[4] = {
		Norm( 0.2, 0.1, 0.95 ),
		Norm( -0.5, 0.3, 0.75 ),
		Norm( 0.1, -0.7, 0.55 ),
		Norm( -0.6, -0.5, 0.35 )
	};
	for( int i = 0; i < 4; ++i ) {
		for( int j = 0; j < 4; ++j ) {
			const double f1 = b.EvalReflection( dirsUpper[i], dirsUpper[j] );
			const double f2 = b.EvalReflection( dirsUpper[j], dirsUpper[i] );
			Check( f1 > 0.0, "Positive external reflection K_r" );
			Near( f1, f2, 1e-12 * std::max( 1.0, f1 ), "Reflection reciprocity K_r(wi,wo) == K_r(wo,wi)" );
		}
	}

	// 2. Transmission adjoint reciprocity:
	//    eta_ext^2 * K_t(wi_ext -> wo_int) == eta_int^2 * K_t(wo_int -> wi_ext)
	const SlabVector dirsLower[4] = {
		Norm( 0.1, -0.2, -0.96 ),
		Norm( -0.3, -0.2, -0.88 ),
		Norm( 0.25, 0.15, -0.90 ),
		Norm( -0.15, 0.35, -0.85 )
	};
	for( int i = 0; i < 4; ++i ) {
		for( int j = 0; j < 4; ++j ) {
			const double fExtToInt = b.EvalTransmission( dirsUpper[i], dirsLower[j] );
			const double fIntToExt = b.EvalTransmission( dirsLower[j], dirsUpper[i] );
			if( fExtToInt > 0.0 || fIntToExt > 0.0 ) {
				const double lhs = etaExt * etaExt * fExtToInt;
				const double rhs = etaInt * etaInt * fIntToExt;
				Near( lhs, rhs, 1e-11 * std::max( 1.0, lhs ),
					"Transmission adjoint reciprocity eta_ext^2 * f(ext->int) == eta_int^2 * f(int->ext)" );
			}
		}
	}
}

void TestSmoothBoundaryRefusalAndSamplingConsistency()
{
	const HyLIoSReciprocalBoundary smooth( 0.0, 1.0, 1.45 );
	Check( smooth.IsSmooth(), "alpha == 0 is smooth" );
	Check( !smooth.AllowsDeltaLightNEE(), "Smooth boundary refuses delta light NEE" );

	// Rough boundary: verify SampleReflection & SampleTransmission weight == f * |cos| / pdf
	// and SampleFull energy conservation (mean radiance transfer under unit white furnace <= 1).
	const HyLIoSReciprocalBoundary rough( 0.25, 1.0, 1.45 );
	RISE::RandomNumberGenerator rng( 2026u );
	const SlabVector wo = Norm( 0.3, -0.2, 0.92 );

	double reflMean = 0.0;
	double fullPowerMean = 0.0;
	const int N = 4000;
	for( int s = 0; s < N; ++s ) {
		const double u1 = rng.CanonicalRandom();
		const double u2 = rng.CanonicalRandom();
		const ReciprocalBoundarySample rs = rough.SampleReflection( wo, u1, u2 );
		if( rs.valid ) {
			const double f = rough.EvalReflection( rs.direction, wo );
			const double pdf = rough.PdfReflection( rs.direction, wo );
			Near( rs.pdf, pdf, 1e-12, "SampleReflection pdf matches PdfReflection" );
			Near( rs.weight, f * std::fabs( rs.direction.z ) / pdf, 1e-11, "SampleReflection weight == f*cos/pdf" );
			reflMean += rs.weight;
		}

		const double ub = rng.CanonicalRandom();
		const double u3 = rng.CanonicalRandom();
		const double u4 = rng.CanonicalRandom();
		const ReciprocalBoundarySample fs = rough.SampleFull( wo, ub, u3, u4 );
		if( fs.valid ) {
			if( fs.isReflection ) {
				Near( fs.pdf, rough.PdfReflectionBranch( fs.direction, wo ), 1e-12,
					"SampleFull reflection pdf == PdfReflectionBranch" );
			} else {
				Near( fs.pdf, rough.PdfTransmissionBranch( fs.direction, wo ), 1e-12,
					"SampleFull transmission pdf == PdfTransmissionBranch" );
			}
			// Power-domain white furnace (undo radiance (etaO/etaI)^2 for transmission branch)
			const double powerWeight = fs.isReflection ? fs.weight : ( fs.weight * ( 1.45 * 1.45 ) );
			fullPowerMean += powerWeight;
		}
	}
	reflMean /= N;
	fullPowerMean /= N;
	Check( reflMean > 0.02 && reflMean < 0.15, "Fresnel-weighted reflection directional albedo in [0.02, 0.15]" );
	Check( fullPowerMean > 0.85 && fullPowerMean <= 1.02, "Single-scattering rough dielectric white-furnace <= 1.0" );
}

} // namespace

int main()
{
	TestReflectionAndTransmissionReciprocity();
	TestSmoothBoundaryRefusalAndSamplingConsistency();

	if( gFailures == 0 ) {
		std::cout << "HyLIoSAdjointTest: ALL PASSED\n";
		return 0;
	}
	std::cerr << "HyLIoSAdjointTest: " << gFailures << " FAILURES\n";
	return 1;
}
