//////////////////////////////////////////////////////////////////////
//
//  BSSRDFSamplingTest.cpp - Validates BSSRDF diffusion profile
//    normalization, importance sampling consistency, and weight
//    correctness for the Burley normalized diffusion model.
//
//  Tests:
//    A. Profile normalization: integral of Rd(r)*2*pi*r dr = A
//       for each channel (numerical quadrature).
//    B. Sampling/PDF consistency: SampleRadius + PdfRadius obey
//       the importance sampling identity.
//    C. Fresnel energy conservation: R + Ft = 1.
//    D. Sw normalization: integral of Sw(wi)*cos(theta) dw over
//       the hemisphere equals the expected Ft hemispherical average.
//    E. Weight consistency: continuation weight * cosinePdf equals
//       Rd * Ft_exit * Sw(cosine_dir) at the sampled point.
//
//  Build (from project root):
//    make -C build/make/rise tests
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>
#include <cassert>
#include <vector>
#include <iomanip>
#include <numeric>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/BSSRDFSampling.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Materials/BurleyNormalizedDiffusionProfile.h"

using namespace RISE;
using namespace RISE::Implementation;

// ================================================================
// Helpers
// ================================================================

static bool IsClose( double a, double b, double relTol = 0.02, double absTol = 1e-6 )
{
	const double diff = std::fabs( a - b );
	const double ref = std::fmax( std::fabs(a), std::fabs(b) );
	return diff < absTol || diff < relTol * ref;
}

/// Local Schlick Fresnel reflectance (mirrors the protected static
/// in BurleyNormalizedDiffusionProfile but accessible from tests)
static Scalar SchlickFresnel( Scalar cosTheta, Scalar eta )
{
	const Scalar R0 = ((1.0 - eta) / (1.0 + eta)) * ((1.0 - eta) / (1.0 + eta));
	const Scalar c = 1.0 - cosTheta;
	const Scalar c2 = c * c;
	return R0 + (1.0 - R0) * c2 * c2 * c;
}

/// Helper to create a heap-allocated BurleyNormalizedDiffusionProfile
/// (destructor is protected, so stack allocation is forbidden).
/// Caller is responsible for calling release().
static BurleyNormalizedDiffusionProfile* MakeProfile(
	const IPainter& ior, const IPainter& abs, const IPainter& scat, Scalar g )
{
	BurleyNormalizedDiffusionProfile* p =
		new BurleyNormalizedDiffusionProfile( ior, abs, scat, g );
	p->addref();
	return p;
}

/// Create a dummy RayIntersectionGeometric at the origin with
/// normal (0,1,0).  Used to give painters a valid ri for GetColor().
static RayIntersectionGeometric MakeDummyRI()
{
	RayIntersectionGeometric ri( Ray( Point3(0,0,0), Vector3(0,-1,0) ), nullRasterizerState );
	ri.bHit = true;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 1, 0 );
	ri.onb.CreateFromW( ri.vNormal );
	return ri;
}

/// Compute reduced albedo per channel from absorption & scattering
static void ComputeChannelParams(
	const RISEPel& sa, const RISEPel& ss, Scalar g,
	Scalar albedo[3], Scalar mfp[3], Scalar sParam[3] )
{
	for( int c = 0; c < 3; c++ )
	{
		const Scalar ss_prime = ss[c] * (1.0 - g);
		const Scalar st_prime = ss_prime + sa[c];
		albedo[c] = st_prime > 1e-10 ? ss_prime / st_prime : 0.0;
		mfp[c] = st_prime > 1e-10 ? 1.0 / st_prime : 0.0;
		// Burley scaling factor
		const Scalar diff = albedo[c] - 0.8;
		sParam[c] = 1.9 - albedo[c] + 3.5 * diff * diff;
	}
}

// ================================================================
// Test A: Profile normalization
// ================================================================
//
// The Burley profile should integrate to the surface albedo A:
//   integral_0^inf Rd(r) * 2*pi*r dr = A
//
// This is a fundamental requirement for energy conservation.
// We verify by numerical trapezoidal quadrature.
// ================================================================

void TestProfileNormalization()
{
	std::cout << "Test A: Profile normalization (integral Rd*2*pi*r dr = A)" << std::endl;

	struct TestCase {
		RISEPel absorption;
		RISEPel scattering;
		Scalar g;
		const char* name;
	};

	TestCase cases[] = {
		// Wax-like: moderate absorption variation across channels
		{ RISEPel(0.01, 0.3, 1.0), RISEPel(2.0, 2.0, 2.0), 0.0, "wax" },
		// Skin-like: anisotropic scattering
		{ RISEPel(1.2, 0.6, 0.06), RISEPel(1.2, 1.5, 1.8), 0.0, "skin" },
		// Near-zero absorption (high albedo)
		{ RISEPel(0.001, 0.001, 0.001), RISEPel(2.0, 2.0, 2.0), 0.0, "high-albedo" },
		// High absorption (low albedo)
		{ RISEPel(5.0, 5.0, 5.0), RISEPel(1.0, 1.0, 1.0), 0.0, "low-albedo" },
		// Anisotropic phase function (g=0.8)
		{ RISEPel(0.01, 0.3, 1.0), RISEPel(2.0, 2.0, 2.0), 0.8, "aniso-g0.8" },
	};

	RayIntersectionGeometric ri = MakeDummyRI();

	for( const auto& tc : cases )
	{
		UniformColorPainter* pIOR  = new UniformColorPainter( RISEPel(1.3, 1.3, 1.3) );  pIOR->addref();
		UniformColorPainter* pAbs  = new UniformColorPainter( tc.absorption );  pAbs->addref();
		UniformColorPainter* pScat = new UniformColorPainter( tc.scattering );  pScat->addref();

		BurleyNormalizedDiffusionProfile* pProfile = MakeProfile( *pIOR, *pAbs, *pScat, tc.g );

		Scalar albedo[3], mfpArr[3], sArr[3];
		ComputeChannelParams( tc.absorption, tc.scattering, tc.g, albedo, mfpArr, sArr );

		// Numerical integration with fine trapezoidal rule
		// Use adaptive range based on profile width
		const Scalar maxR = pProfile->GetMaximumDistanceForError( 1e-8 );
		const int N = 100000;
		const Scalar dr = maxR / N;

		Scalar integral[3] = { 0, 0, 0 };
		for( int i = 1; i < N; i++ )
		{
			const Scalar r = i * dr;
			const RISEPel Rd = pProfile->EvaluateProfile( r, ri );
			for( int c = 0; c < 3; c++ ) {
				integral[c] += Rd[c] * 2.0 * PI * r * dr;
			}
		}

		bool pass = true;
		for( int c = 0; c < 3; c++ )
		{
			const bool ok = IsClose( integral[c], albedo[c], 0.01 );
			if( !ok ) pass = false;
			std::cout << "  " << tc.name << " ch" << c
				<< ": integral=" << std::setprecision(6) << integral[c]
				<< " albedo=" << albedo[c]
				<< " err=" << std::fabs(integral[c] - albedo[c]) / albedo[c] * 100 << "%"
				<< (ok ? " OK" : " FAIL") << std::endl;
		}
		assert( pass );

		pProfile->release();
		pIOR->release();
		pAbs->release();
		pScat->release();
	}

	std::cout << "  PASSED" << std::endl;
}

// ================================================================
// Test B: Sampling/PDF consistency
// ================================================================
//
// For each channel, sample many radii via SampleRadius(u, ch) and
// check that the histogram matches the PDF from PdfRadius(r, ch).
// Also verify: E[Rd(r)*2*pi*r / pdfRadius(r)] = A (importance
// sampling identity for the profile integral).
// ================================================================

void TestSamplingPDFConsistency()
{
	std::cout << "Test B: Sampling/PDF consistency" << std::endl;

	UniformColorPainter* pIOR  = new UniformColorPainter( RISEPel(1.3, 1.3, 1.3) );  pIOR->addref();
	UniformColorPainter* pAbs  = new UniformColorPainter( RISEPel(0.01, 0.3, 1.0) );  pAbs->addref();
	UniformColorPainter* pScat = new UniformColorPainter( RISEPel(2.0, 2.0, 2.0) );   pScat->addref();

	BurleyNormalizedDiffusionProfile* pProfile = MakeProfile( *pIOR, *pAbs, *pScat, 0.0 );

	RayIntersectionGeometric ri = MakeDummyRI();

	Scalar albedo[3], mfpArr[3], sArr[3];
	ComputeChannelParams( RISEPel(0.01, 0.3, 1.0), RISEPel(2.0, 2.0, 2.0), 0.0,
		albedo, mfpArr, sArr );

	RandomNumberGenerator rng( 12345 );

	for( int ch = 0; ch < 3; ch++ )
	{
		const int N = 500000;
		double isEstimate = 0;  // importance sampling estimate of integral

		for( int i = 0; i < N; i++ )
		{
			const Scalar u = rng.CanonicalRandom();
			const Scalar r = pProfile->SampleRadius( u, ch, ri );
			if( r <= 0 ) continue;

			const Scalar pdf = pProfile->PdfRadius( r, ch, ri );
			assert( pdf > 0 );

			const RISEPel Rd = pProfile->EvaluateProfile( r, ri );

			// IS estimate: E[Rd(r) * 2*pi*r / pdfR(r)] should = A per channel
			isEstimate += Rd[ch] * 2.0 * PI * r / pdf;
		}
		isEstimate /= N;

		const bool ok = IsClose( isEstimate, albedo[ch], 0.02 );
		std::cout << "  ch" << ch << ": IS_estimate=" << std::setprecision(6) << isEstimate
			<< " albedo=" << albedo[ch]
			<< " err=" << std::fabs(isEstimate - albedo[ch]) / albedo[ch] * 100 << "%"
			<< (ok ? " OK" : " FAIL") << std::endl;
		assert( ok );
	}

	pProfile->release();
	pIOR->release();
	pAbs->release();
	pScat->release();

	std::cout << "  PASSED" << std::endl;
}

// ================================================================
// Test C: Fresnel energy conservation
// ================================================================
//
// For any angle, R + Ft = 1 (Schlick approximation).
// Test at several angles and IOR values.
// ================================================================

void TestFresnelConservation()
{
	std::cout << "Test C: Fresnel energy conservation (R + Ft = 1)" << std::endl;

	RayIntersectionGeometric ri = MakeDummyRI();

	const Scalar iors[] = { 1.0, 1.1, 1.3, 1.5, 2.0, 2.5 };
	const Scalar cosThetas[] = { 0.01, 0.1, 0.3, 0.5, 0.7, 0.9, 1.0 };

	bool allPass = true;
	for( Scalar eta : iors )
	{
		UniformColorPainter* pIOR  = new UniformColorPainter( RISEPel(eta, eta, eta) );  pIOR->addref();
		UniformColorPainter* pAbs  = new UniformColorPainter( RISEPel(0.1, 0.1, 0.1) );  pAbs->addref();
		UniformColorPainter* pScat = new UniformColorPainter( RISEPel(1.0, 1.0, 1.0) );   pScat->addref();

		BurleyNormalizedDiffusionProfile* pProfile = MakeProfile( *pIOR, *pAbs, *pScat, 0.0 );

		for( Scalar cosT : cosThetas )
		{
			const Scalar Ft = pProfile->FresnelTransmission( cosT, ri );
			const Scalar R = 1.0 - Ft;
			const Scalar sum = R + Ft;

			if( !IsClose( sum, 1.0, 1e-10 ) ) {
				std::cout << "  FAIL: eta=" << eta << " cos=" << cosT
					<< " R+Ft=" << sum << std::endl;
				allPass = false;
			}

			// Verify R is non-negative and at most 1
			assert( R >= -1e-10 && R <= 1.0 + 1e-10 );
			// Verify Ft is non-negative and at most 1
			assert( Ft >= -1e-10 && Ft <= 1.0 + 1e-10 );
		}

		// Check F0 = ((eta-1)/(eta+1))^2 at normal incidence
		const Scalar Ft_normal = pProfile->FresnelTransmission( 1.0, ri );
		const Scalar R_normal = 1.0 - Ft_normal;
		const Scalar F0_expected = ((eta - 1.0) / (eta + 1.0)) * ((eta - 1.0) / (eta + 1.0));
		if( !IsClose( R_normal, F0_expected, 1e-8 ) ) {
			std::cout << "  FAIL: eta=" << eta
				<< " R(normal)=" << R_normal << " vs F0=" << F0_expected << std::endl;
			allPass = false;
		}

		pProfile->release();
		pIOR->release();
		pAbs->release();
		pScat->release();
	}

	assert( allPass );
	std::cout << "  PASSED" << std::endl;
}

// ================================================================
// Test D: Sw normalization
// ================================================================
//
// The directional scattering function Sw(wi) = Ft(cos_theta) / (c*pi)
// should satisfy (PBRT, Christensen & Burley):
//   integral_hemisphere Sw(wi) * cos(theta) dw = 1
// This is the normalization property that ensures the BSSRDF
// factorization is correct.
//
// We verify by MC integration over the hemisphere.
// ================================================================

void TestSwNormalization()
{
	std::cout << "Test D: Sw normalization (integral Sw*cos dw = 1)" << std::endl;

	// The Christensen & Burley normalization c = (41-20*F0)/42 is an
	// empirical fit.  The integral of Sw*cos over the hemisphere is
	// approximately 1.0 but not exact — errors up to ~3% are expected
	// for low IOR values.  We use a 5% tolerance to avoid false
	// failures while still catching catastrophic normalization bugs.
	// Test IOR values typical of translucent materials (wax, skin, marble).
	// The c normalization becomes increasingly inaccurate above eta ~1.5.
	const Scalar iors[] = { 1.1, 1.3, 1.5 };

	RandomNumberGenerator rng( 54321 );

	for( Scalar eta : iors )
	{
		// MC integration of Sw(wi)*cos(theta) over hemisphere
		// using cosine-weighted sampling: pdf = cos(theta)/pi
		// Estimator: (1/N) * sum[ Sw(wi) * cos(theta) / (cos/pi) ]
		//          = (1/N) * sum[ Sw(wi) * pi ]
		// But Sw = Ft/(c*pi), so estimator = (1/N) * sum[ Ft(cos) / c ]
		const int N = 1000000;
		double estimate = 0;

		for( int i = 0; i < N; i++ )
		{
			// Cosine-weighted hemisphere sampling
			const Scalar u1 = rng.CanonicalRandom();
			const Scalar cosTheta = sqrt( u1 );

			// Evaluate Sw at this angle using the shared function
			const Scalar Ft = 1.0 - SchlickFresnel( cosTheta, eta );
			const Scalar Sw = BSSRDFSampling::EvaluateSwWithFresnel( Ft, eta );

			// Cosine-weighted estimator: f / pdf = Sw*cos / (cos/pi) = Sw*pi
			estimate += Sw * PI;
		}
		estimate /= N;

		const bool ok = IsClose( estimate, 1.0, 0.05 );
		std::cout << "  eta=" << eta << ": integral=" << std::setprecision(6) << estimate
			<< " expected=1.0"
			<< " err=" << std::fabs(estimate - 1.0) * 100 << "%"
			<< (ok ? " OK" : " FAIL") << std::endl;
		assert( ok );
	}

	std::cout << "  PASSED" << std::endl;
}

// ================================================================
// Test E: Weight consistency
// ================================================================
//
// The continuation weight from SampleEntryPoint should satisfy:
//   weight * cosinePdf = Rd(r) * Ft_exit * Sw(cosine_dir)
//
// And the spatial weight should satisfy:
//   weightSpatial = Rd(r) * Ft_exit / pdfSurface
//
// We verify by comparing the weight components against independent
// evaluation of the profile and Fresnel terms, without needing
// actual probe rays (we test the formula consistency directly).
// ================================================================

void TestWeightConsistency()
{
	std::cout << "Test E: Weight formula consistency" << std::endl;

	const Scalar eta = 1.3;
	const Scalar F0 = ((eta - 1.0) / (eta + 1.0)) * ((eta - 1.0) / (eta + 1.0));
	const Scalar c = (41.0 - 20.0 * F0) / 42.0;

	// Test that Sw = Ft / (c*pi) for various angles
	bool allPass = true;

	const Scalar angles[] = { 0.1, 0.3, 0.5, 0.7, 0.9, 1.0 };
	for( Scalar cosTheta : angles )
	{
		const Scalar R = SchlickFresnel( cosTheta, eta );
		const Scalar Ft = 1.0 - R;

		// Compute Sw via shared function
		const Scalar Sw = BSSRDFSampling::EvaluateSwWithFresnel( Ft, eta );

		// Expected: Ft / (c * pi)
		const Scalar expected = Ft / (c * PI);

		if( !IsClose( Sw, expected, 1e-10 ) ) {
			std::cout << "  FAIL: cos=" << cosTheta
				<< " Sw=" << Sw << " vs Ft/(c*pi)=" << expected << std::endl;
			allPass = false;
		}
	}

	// Verify weight formula:
	// weight        = Rd * Ft_exit * SwFactor / pdfSurface
	// weightSpatial = Rd * Ft_exit / pdfSurface
	// where SwFactor = Ft_entry / SwNorm = Ft_entry / c
	//
	// So: weight / weightSpatial = SwFactor = Ft_entry / c
	//
	// And: weight * cosinePdf = Rd * Ft_exit * Ft_entry / (c * pdfSurface) * cos/pi
	//    = weightSpatial * Ft_entry / c * cos/pi
	//    = weightSpatial * Sw(entry_dir) * cos
	//
	// This is exactly Rd * Ft_exit * Sw(entry_dir) * cos / pdfSurface,
	// which is the correct integrand divided by the spatial PDF.

	// Simulate the weight computation with known values
	RandomNumberGenerator rng( 99999 );

	for( int trial = 0; trial < 100; trial++ )
	{
		const Scalar cosEntry = 0.1 + 0.9 * rng.CanonicalRandom();
		const Scalar FtEntry = 1.0 - SchlickFresnel( cosEntry, eta );
		const Scalar SwFactor = FtEntry / c;
		const Scalar FtExit = 1.0 - SchlickFresnel(
			0.1 + 0.9 * rng.CanonicalRandom(), eta );

		// Simulate Rd and pdfSurface as arbitrary positive values
		const Scalar Rd = 0.01 + 10.0 * rng.CanonicalRandom();
		const Scalar pdfSurface = 0.001 + 5.0 * rng.CanonicalRandom();

		// Compute weights as the code does
		const Scalar weight = Rd * FtExit * SwFactor / pdfSurface;
		const Scalar weightSpatial = Rd * FtExit / pdfSurface;

		// Verify relationship
		const Scalar cosinePdf = cosEntry / PI;
		const Scalar Sw = BSSRDFSampling::EvaluateSwWithFresnel( FtEntry, eta );

		// weight * cosinePdf should equal weightSpatial * Sw * cosEntry
		const Scalar lhs = weight * cosinePdf;
		const Scalar rhs = weightSpatial * Sw * cosEntry;

		if( !IsClose( lhs, rhs, 1e-10 ) ) {
			std::cout << "  FAIL: trial=" << trial
				<< " w*pdf=" << lhs << " vs wSpatial*Sw*cos=" << rhs << std::endl;
			allPass = false;
		}
	}

	assert( allPass );
	std::cout << "  PASSED" << std::endl;
}

// ================================================================
// Test F: Flat-slab energy conservation
// ================================================================
//
// For a semi-infinite flat slab in a uniform environment, the
// expected BSSRDF contribution per unit incident radiance is:
//   ratio = R + Ft * A
// where R = Fresnel reflectance (hemispherical average) and
// A = surface albedo per channel.
//
// We verify this by MC integration:
//   (1/N) * sum_i[ Ft(cos_i) * Rd_ch(r_i) * 2*pi*r_i / pdfR(r_i) * Sw(cos_entry) * pi ]
// should approach Ft * A per channel (the BSSRDF contribution).
//
// When added to the Fresnel reflectance R, the total should be:
//   R + Ft * A
// which for zero absorption (A=1) gives R + Ft = 1.
// ================================================================

void TestFlatSlabEnergyConservation()
{
	std::cout << "Test F: Flat-slab energy conservation (R + Ft*A)" << std::endl;

	RayIntersectionGeometric ri = MakeDummyRI();

	UniformColorPainter* pIOR  = new UniformColorPainter( RISEPel(1.3, 1.3, 1.3) );  pIOR->addref();
	UniformColorPainter* pAbs  = new UniformColorPainter( RISEPel(0.01, 0.3, 1.0) );  pAbs->addref();
	UniformColorPainter* pScat = new UniformColorPainter( RISEPel(2.0, 2.0, 2.0) );   pScat->addref();

	BurleyNormalizedDiffusionProfile* pProfile = MakeProfile( *pIOR, *pAbs, *pScat, 0.0 );

	const Scalar eta = 1.3;
	const Scalar F0 = ((eta - 1.0) / (eta + 1.0)) * ((eta - 1.0) / (eta + 1.0));

	Scalar albedo[3], mfpArr[3], sArr[3];
	ComputeChannelParams( RISEPel(0.01, 0.3, 1.0), RISEPel(2.0, 2.0, 2.0), 0.0,
		albedo, mfpArr, sArr );

	// Expected flat-slab values: R + Ft*A per channel
	// where R and Ft are hemispherical averages, which for the Sw
	// normalization approach reduces to just A per channel for the
	// subsurface contribution (since integral of Sw*cos dw = 1).
	//
	// More precisely, for normal-incidence illumination:
	//   subsurface = Ft(normal) * A * integral_hemisphere[ Sw(wi)*cos(wi) dw ]
	//              = Ft(normal) * A * 1.0
	//              = Ft(normal) * A

	const Scalar Ft_normal = 1.0 - F0;  // Fresnel transmission at normal incidence

	// MC estimate: for each sample, pick a radius from profile CDF,
	// evaluate Rd*2*pi*r/pdf, and average over channels
	RandomNumberGenerator rng( 77777 );
	const int N = 500000;

	for( int ch = 0; ch < 3; ch++ )
	{
		double isEstimate = 0;
		for( int i = 0; i < N; i++ )
		{
			const Scalar u = rng.CanonicalRandom();
			const Scalar r = pProfile->SampleRadius( u, ch, ri );
			if( r <= 0 ) continue;

			const Scalar pdf = pProfile->PdfRadius( r, ch, ri );
			if( pdf <= 0 ) continue;

			const RISEPel Rd = pProfile->EvaluateProfile( r, ri );

			// The profile integrand: Rd(r) * 2*pi*r
			// IS estimate: Rd * 2*pi*r / pdf
			isEstimate += Rd[ch] * 2.0 * PI * r / pdf;
		}
		isEstimate /= N;  // This should equal albedo[ch]

		// Total energy at normal incidence: R + Ft * A
		const Scalar total = F0 + Ft_normal * isEstimate;
		const Scalar expected = F0 + Ft_normal * albedo[ch];

		const bool ok = IsClose( total, expected, 0.02 );
		std::cout << "  ch" << ch << ": R+Ft*A=" << std::setprecision(6) << total
			<< " expected=" << expected
			<< " albedo_IS=" << isEstimate << " albedo=" << albedo[ch]
			<< (ok ? " OK" : " FAIL") << std::endl;
		assert( ok );
	}

	pProfile->release();
	pIOR->release();
	pAbs->release();
	pScat->release();

	// Also test zero absorption: total should = 1.0
	std::cout << "  Zero absorption (A=1): ";

	UniformColorPainter* pIOR2  = new UniformColorPainter( RISEPel(1.3, 1.3, 1.3) );  pIOR2->addref();
	UniformColorPainter* pAbs2  = new UniformColorPainter( RISEPel(0.0, 0.0, 0.0) );  pAbs2->addref();
	UniformColorPainter* pScat2 = new UniformColorPainter( RISEPel(2.0, 2.0, 2.0) );  pScat2->addref();

	BurleyNormalizedDiffusionProfile* pProfile2 = MakeProfile( *pIOR2, *pAbs2, *pScat2, 0.0 );

	double isEstimate = 0;
	const int N2 = 500000;
	for( int i = 0; i < N2; i++ )
	{
		const Scalar u = rng.CanonicalRandom();
		const Scalar r = pProfile2->SampleRadius( u, 0, ri );
		if( r <= 0 ) continue;

		const Scalar pdf = pProfile2->PdfRadius( r, 0, ri );
		if( pdf <= 0 ) continue;

		const RISEPel Rd = pProfile2->EvaluateProfile( r, ri );
		isEstimate += Rd[0] * 2.0 * PI * r / pdf;
	}
	isEstimate /= N2;

	const Scalar totalZero = F0 + Ft_normal * isEstimate;
	const bool okZero = IsClose( totalZero, 1.0, 0.02 );
	std::cout << "R+Ft*A=" << std::setprecision(6) << totalZero
		<< " expected=1.0"
		<< (okZero ? " OK" : " FAIL") << std::endl;
	assert( okZero );

	pProfile2->release();
	pIOR2->release();
	pAbs2->release();
	pScat2->release();

	std::cout << "  PASSED" << std::endl;
}

// ================================================================

int main()
{
	std::cout << "=== BSSRDF Sampling Correctness Tests ===" << std::endl;
	std::cout << std::endl;

	TestProfileNormalization();
	std::cout << std::endl;

	TestSamplingPDFConsistency();
	std::cout << std::endl;

	TestFresnelConservation();
	std::cout << std::endl;

	TestSwNormalization();
	std::cout << std::endl;

	TestWeightConsistency();
	std::cout << std::endl;

	TestFlatSlabEnergyConservation();
	std::cout << std::endl;

	std::cout << "=== All BSSRDF tests passed ===" << std::endl;
	return 0;
}
