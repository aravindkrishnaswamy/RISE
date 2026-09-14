//////////////////////////////////////////////////////////////////////
//
//  GGXHeightCorrelatedEnergyLUTTest.cpp - DL-63 red proof.
//
//  DL-63 (docs/DL37_GGX_DIFFUSE_TRANSMISSION.md, docs/DEBT_LEDGER.md):
//  GGXBRDF/GGXSPF's single-scatter specular term is D * G2 / (4 cosWi
//  cosWo), where G2 is the Smith HEIGHT-CORRELATED masking-shadowing
//  function (MicrofacetUtils::GGX_G2/GGX_G2_Aniso, Heitz 2014 JCGT
//  3(2) Sec. 5.2).  But MicrofacetEnergyLUT.h's original E_ss_TABLE/
//  E_avg_TABLE (LookupEss/LookupEavg) were baked from a DIFFERENT
//  masking-shadowing model: tools/GenerateMicrofacetEnergyLUT.cpp's
//  VNDF-sampling estimator computed the per-sample weight as plain
//  G1(wo) -- which is the correct single-scatter-directional-albedo
//  estimator for the SEPARABLE model G = G1(wi)*G1(wo) (the G1(wi)
//  term cancels against the VNDF pdf's own G1(wi) factor), not for
//  height-correlated G2.  Using the separable-calibrated table to
//  compensate a height-correlated-G2 render under-estimates how much
//  energy the single-scatter term already carries, producing a
//  specular-only furnace GAIN: GGXDiffuseTransmissionTest's three
//  Schlick-F0=1 (pure specular, diffuse=0) configs read
//  {1.0772, 1.0432, 1.1467} instead of 1.0 (alpha=0.6 theta=80,
//  alpha=1 theta=60, alpha=1 theta=80).
//
//  This test independently verifies the calibration MISMATCH and the
//  FIX, using a completely separate implementation from both
//  tools/GenerateMicrofacetEnergyLUT.cpp (a standalone offline binary
//  with its own RNG and its own re-implementation of GGX math) and
//  MicrofacetEnergyLUT.h itself: it Monte-Carlo integrates the
//  directional albedo of the REAL, production `MicrofacetUtils::
//  GGX_Lambda`/`GGX_G2` height-correlated masking-shadowing functions
//  (the exact primitives GGXBRDF's single-scatter term calls), using
//  RISE's own `MicrofacetUtils::VNDF_Sample` for the proposal but an
//  independent `std::mt19937_64` RNG stream and a much larger sample
//  count than the LUT generator used, at the three previously-failing
//  (alpha, theta) configurations plus additional spot checks spanning
//  the alpha/cosTheta domain.  It asserts:
//
//    (a) `MicrofacetEnergyLUT::LookupEssG2` (the DL-63 fix) matches
//        this independent quadrature to within its Monte-Carlo
//        standard error -- the real regression pin.
//    (b) `MicrofacetEnergyLUT::LookupEss` (the pre-existing, UNCHANGED
//        separable-model table CookTorranceSPF/BRDF still use) does
//        NOT match the height-correlated quadrature by more than a
//        few standard errors at these configurations -- documents
//        that the two tables are calibrated to genuinely different
//        models, which is what makes routing GGXBRDF/GGXSPF/
//        CoatedBRDF to LookupEssG2 (not LookupEss) load-bearing.
//
//  Build (from project root):
//    c++ -arch arm64 -Isrc/Library -I/opt/homebrew/include
//        -O3 -ffast-math -fno-finite-math-only -funroll-loops -Wall -pedantic
//        -Wno-c++11-long-long -DCOLORS_RGB -DMERSENNE53
//        -DNO_TIFF_SUPPORT -DNO_EXR_SUPPORT -DRISE_ENABLE_MAILBOXING
//        -c tests/GGXHeightCorrelatedEnergyLUTTest.cpp
//        -o tests/GGXHeightCorrelatedEnergyLUTTest.o
//    c++ -arch arm64 -o tests/ggx_height_correlated_energy_lut_test
//        tests/GGXHeightCorrelatedEnergyLUTTest.o bin/librise.a
//        -L/opt/homebrew/lib -lpng -lz
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <cmath>
#include <string>
#include <sstream>
#include <random>
#include <algorithm>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/MicrofacetUtils.h"
#include "../src/Library/Utilities/MicrofacetEnergyLUT.h"
#include "../src/Library/Interfaces/ILog.h"

using namespace RISE;

namespace
{
	static int checks = 0;
	static int failures = 0;

	static bool Report( const std::string& label, const bool passed )
	{
		std::cout << "  " << std::left << std::setw( 70 ) << label
			<< ( passed ? "PASS" : "FAIL" ) << "\n";
		++checks;
		if( !passed ) ++failures;
		return passed;
	}

	// Independent (from both the offline LUT generator AND
	// MicrofacetEnergyLUT.h) Monte-Carlo estimate of the
	// height-correlated-G2 single-scatter directional albedo
	// E_ss_G2(alpha, cosWi), using RISE's own production
	// MicrofacetUtils::VNDF_Sample / GGX_Lambda / GGX_G2 primitives --
	// the exact functions GGXBRDF's single-scatter term is built from --
	// with an independent std::mt19937_64 RNG stream.
	//
	// Weight derivation (VNDF importance sampling, F=1): the standard
	// identity f*cosWo/pdf(wo) = G2(wi,wo)/G1(wi) for a microfacet BRDF
	// f = D*G2/(4 cosWi cosWo) sampled by reflecting wi about a VNDF-
	// sampled micronormal m, since pdf(wo) = G1(wi)*D(m)/(4*cosWi) (the
	// wi.m factor cancels against the reflection Jacobian's 4*wi.m).
	static double MonteCarloEssG2( const Scalar alpha, const Scalar cosWi, unsigned int numSamples, unsigned int seed, double& outStdErr )
	{
		OrthonormalBasis3D onb;
		onb.CreateFromW( Vector3( 0, 0, 1 ) );

		const Scalar sinWi = sqrt( r_max( Scalar(0), Scalar(1) - cosWi * cosWi ) );
		const Vector3 wi( sinWi, 0, cosWi );

		const Scalar G1wi = 1.0 / ( 1.0 + MicrofacetUtils::GGX_Lambda( alpha, cosWi ) );

		std::mt19937_64 rng( seed );
		std::uniform_real_distribution<double> uni( 0.0, 1.0 );

		double sum = 0.0;
		double sumSq = 0.0;
		unsigned int n = 0;
		for( unsigned int s = 0; s < numSamples; ++s )
		{
			const Scalar u1 = uni( rng );
			const Scalar u2 = uni( rng );
			const Vector3 m = MicrofacetUtils::VNDF_Sample( wi, onb, alpha, u1, u2 );
			const Scalar wiDotM = Vector3Ops::Dot( wi, m );
			if( wiDotM <= 0 ) { ++n; continue; }

			const Vector3 wo = Vector3Ops::Normalize( m * ( 2.0 * wiDotM ) - wi );
			const Scalar cosWo = wo.z;
			double weight = 0.0;
			if( cosWo > 0 )
			{
				const Scalar G2 = MicrofacetUtils::GGX_G2( alpha, cosWi, cosWo );
				weight = G2 / G1wi;
			}
			sum += weight;
			sumSq += weight * weight;
			++n;
		}

		const double mean = sum / n;
		const double variance = r_max( 0.0, sumSq / n - mean * mean );
		outStdErr = sqrt( variance / n );
		return mean;
	}

	// P2-2 (review finding): MonteCarloEssG2 above and
	// tools/GenerateMicrofacetEnergyLUT.cpp's generator both derive their
	// per-sample weight from the SAME VNDF-importance-sampling identity
	// (weight = G2(wi,wo)/G1(wi)) -- a wrong identity (e.g. a sign error
	// in how G2 folds into the VNDF pdf's own G1(wi) factor) would pass
	// both consistently.  This estimator is independent along every axis
	// the reviewer asked for: its own re-implementation of the GGX
	// distribution D, Smith Lambda, and height-correlated G2 (no call
	// into MicrofacetUtils or the offline generator's identical-by-
	// construction copies), its own std::mt19937_64 RNG stream, and
	// UNIFORM-hemisphere sampling of wo (pdf = 1/(2*pi), no VNDF at all).
	// It directly integrates the definition
	//   E_ss(alpha,cosWi) = integral_hemisphere D(m)*G2(wi,wo)/(4*cosWi) dwo
	// (the cosWo in "f*cosWo" cancels the cosWo in f's own 1/(4 cosWi
	// cosWo) denominator, leaving no cosWo dependence in the integrand
	// itself -- only in D(m)'s implicit dependence on wo through the half
	// vector m = normalize(wi+wo)), via
	//   E_ss = (2*pi / (4*cosWi*N)) * sum_i D(m_i)*G2(wi,wo_i)
	// A uniform-hemisphere proposal is a much worse importance sampler
	// than VNDF for peaked (low-alpha) configurations, so this is run at
	// moderate-to-high alpha only and at a high sample count.
	namespace IndependentGGX
	{
		static double D_Isotropic( double alpha, double cosThetaM )
		{
			if( cosThetaM <= 0 ) return 0.0;
			const double alpha2 = alpha * alpha;
			const double cos2 = cosThetaM * cosThetaM;
			const double denom = cos2 * ( alpha2 - 1.0 ) + 1.0;
			return alpha2 / ( PI * denom * denom );
		}

		static double Lambda( double alpha, double cosTheta )
		{
			if( cosTheta >= 1.0 - 1e-12 ) return 0.0;
			if( cosTheta <= 1e-12 ) return 1e10;
			const double cos2 = cosTheta * cosTheta;
			const double tan2 = ( 1.0 - cos2 ) / cos2;
			return 0.5 * ( -1.0 + sqrt( 1.0 + alpha * alpha * tan2 ) );
		}

		static double G2_HeightCorrelated( double alpha, double cosWi, double cosWo )
		{
			return 1.0 / ( 1.0 + Lambda( alpha, cosWi ) + Lambda( alpha, cosWo ) );
		}
	}

	static double UniformHemisphereEssG2( const Scalar alpha, const Scalar cosWi, unsigned int numSamples, unsigned int seed, double& outStdErr )
	{
		const double sinWi = sqrt( r_max( 0.0, 1.0 - double(cosWi) * double(cosWi) ) );

		std::mt19937_64 rng( seed );
		std::uniform_real_distribution<double> uni( 0.0, 1.0 );

		double sum = 0.0;
		double sumSq = 0.0;
		for( unsigned int s = 0; s < numSamples; ++s )
		{
			// Uniform SOLID-ANGLE sampling of the upper hemisphere:
			// cosThetaWo ~ Uniform(0,1), phi ~ Uniform(0,2*pi) -> pdf = 1/(2*pi).
			const double cosWo = uni( rng );
			const double sinWo = sqrt( r_max( 0.0, 1.0 - cosWo * cosWo ) );
			const double phi = TWO_PI * uni( rng );

			const double wix = sinWi, wiy = 0.0, wiz = double(cosWi);
			const double wox = sinWo * cos( phi ), woy = sinWo * sin( phi ), woz = cosWo;

			double mx = wix + wox, my = wiy + woy, mz = wiz + woz;
			const double mLen = sqrt( mx * mx + my * my + mz * mz );
			if( mLen < 1e-12 ) continue;
			mx /= mLen; my /= mLen; mz /= mLen;
			const double cosThetaM = mz;
			if( cosThetaM <= 0 ) continue;

			const double D = IndependentGGX::D_Isotropic( double(alpha), cosThetaM );
			const double G2 = IndependentGGX::G2_HeightCorrelated( double(alpha), double(cosWi), cosWo );
			const double weight = D * G2;
			sum += weight;
			sumSq += weight * weight;
		}

		const double meanWeight = sum / double(numSamples);
		const double varWeight = r_max( 0.0, sumSq / double(numSamples) - meanWeight * meanWeight );
		const double scale = TWO_PI / ( 4.0 * double(cosWi) );
		const double mean = scale * meanWeight;
		outStdErr = scale * sqrt( varWeight / double(numSamples) );
		return mean;
	}

	static bool TestUniformHemisphereIndependentQuadrature(
		const char* label, const Scalar alpha, const Scalar cosWi, unsigned int seed )
	{
		double stdErr = 0.0;
		const double reference = UniformHemisphereEssG2( alpha, cosWi, 20000000u, seed, stdErr );
		const double looked = MicrofacetEnergyLUT::LookupEssG2( cosWi, alpha );

		// Generous MC band: uniform-hemisphere sampling has much higher
		// variance than VNDF importance sampling for this integrand, so
		// this needs a wide multiple of its own (larger) standard error
		// plus the same small absolute floor the other quadrature checks
		// in this file use, rather than the reviewer's illustrative
		// figures (which came from a different sample count/seed).
		const double tol = 8.0 * stdErr + 0.003;
		const double diff = std::fabs( looked - reference );
		const bool passed = diff <= tol;

		std::ostringstream oss;
		oss << label << " LookupEssG2=" << std::fixed << std::setprecision(5) << looked
			<< " uniformHemisphere=" << reference << "+/-" << std::setprecision(5) << stdErr
			<< " diff=" << diff << " tol=" << tol;
		return Report( oss.str(), passed );
	}

	static bool TestLookupEssG2MatchesIndependentQuadrature(
		const char* label, const Scalar alpha, const double thetaDeg, unsigned int seed )
	{
		const double theta = thetaDeg * PI / 180.0;
		const Scalar cosWi = std::cos( theta );

		double stdErr = 0.0;
		const double reference = MonteCarloEssG2( alpha, cosWi, 4000000u, seed, stdErr );
		const double looked = MicrofacetEnergyLUT::LookupEssG2( cosWi, alpha );

		// 8-sigma bound (generous: this is a bilinear-interpolated 32x32
		// LUT compared against a fresh, independently-seeded 4M-sample
		// quadrature -- not bit-identical, but must agree far inside
		// statistical noise).
		const double tol = 8.0 * stdErr + 0.003;
		const double diff = std::fabs( looked - reference );
		const bool passed = diff <= tol;

		std::ostringstream oss;
		oss << label << " LookupEssG2=" << std::fixed << std::setprecision(5) << looked
			<< " quadrature=" << reference << "+/-" << std::setprecision(5) << stdErr
			<< " diff=" << diff << " tol=" << tol;
		return Report( oss.str(), passed );
	}

	static bool TestLookupEssSeparableModelDiffersFromG2(
		const char* label, const Scalar alpha, const double thetaDeg, unsigned int seed )
	{
		const double theta = thetaDeg * PI / 180.0;
		const Scalar cosWi = std::cos( theta );

		double stdErr = 0.0;
		const double reference = MonteCarloEssG2( alpha, cosWi, 4000000u, seed, stdErr );
		const double lookedSeparable = MicrofacetEnergyLUT::LookupEss( cosWi, alpha );

		// The separable-model table must be MEASURABLY different from
		// the height-correlated quadrature -- documents that the two
		// TABLES are calibrated to different models.  Note what this
		// does NOT cover: this function never constructs or calls
		// GGXBRDF/GGXSPF, so it cannot detect a regression that routes
		// production code back to LookupEss (that is
		// GGXDiffuseTransmissionTest's job, which exercises the real
		// GGXBRDF).  This check only guards against the LookupEss/
		// LookupEssG2 table DATA becoming numerically indistinguishable
		// (e.g. a copy-paste that baked E_ss_TABLE_G2 from the same
		// separable estimator as E_ss_TABLE), which would silently make
		// GGXBRDF's new G2 lookup calls a no-op change.
		const double tol = 8.0 * stdErr + 0.003;
		const double diff = std::fabs( lookedSeparable - reference );
		const bool passed = diff > tol;
		// Reported for diagnostic clarity -- how many standard errors the
		// divergence actually is, versus the ratio to this check's pass
		// threshold (two different, easily-conflated numbers; see this
		// function's history in docs/DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md
		// "DL-63" for a prior mix-up between them).
		const double sigmaMultiple = ( stdErr > 0.0 ) ? diff / stdErr : 0.0;
		const double toleranceRatio = ( tol > 0.0 ) ? diff / tol : 0.0;

		std::ostringstream oss;
		oss << label << " LookupEss(separable)=" << std::fixed << std::setprecision(5) << lookedSeparable
			<< " G2-quadrature=" << reference << "+/-" << std::setprecision(5) << stdErr
			<< " diff=" << diff << " (" << std::setprecision(1) << sigmaMultiple
			<< " sigma, " << toleranceRatio << "x tol=" << std::setprecision(5) << tol << ")";
		return Report( oss.str(), passed );
	}

	// P2-1 (review finding, replaces the old TestKullaContyIdentityHoldsForG2
	// below): that check computed total = Ess_G2 + (1-Ess_G2)*F_ms at
	// Schlick F0=1, where ComputeFms(F_avg=1, *) == 1 IDENTICALLY for any
	// Eavg argument (denom = 1 - 1*(1-Eavg) = Eavg, so F_ms = 1*1*Eavg/Eavg
	// = 1) -- so `total` collapsed to Ess_G2 + (1-Ess_G2)*1 == 1 by pure
	// algebra, for ANY table content whatsoever (a corrupted or all-zero
	// E_ss_TABLE_G2/E_avg_TABLE_G2 would still pass).  The reported
	// "diff=0.000000e+00" was the tell.  Two real checks replace it.

	// (a) E_ss/E_avg cross-table consistency.  tools/GenerateMicrofacetEnergyLUT.cpp
	// derives each E_avg_TABLE_G2[ai] from that SAME row's E_ss_TABLE_G2[ai][*]
	// via a plain midpoint-rule discretization over the LUT's own 32 cell
	// centers: `E_avg_G2[ai] = 2 * sum_k E_ss_G2[ai][k] * mu_k * (1/LUT_SIZE)`
	// (see the generator's per-alpha loop and this header's own "E_avg(alpha)
	// = 2 * integral_0^1 E_ss(alpha, mu) * mu d_mu" comment on E_avg_TABLE).
	// This re-derives that SAME sum directly from the CHECKED-IN
	// E_ss_TABLE_G2 values (not fresh Monte-Carlo samples), blended across
	// adjacent alpha rows with EXACTLY LookupEssG2/LookupEavgG2's own
	// (ai0, ai1, af) mapping.  Because the sum is linear, blending the rows
	// first and then summing is algebraically identical to summing each
	// row first and then blending -- so this must reproduce LookupEavgG2
	// (alpha) to within the table's own printed precision (8 significant
	// decimals per entry, ~5e-9 absolute per term) if E_avg_TABLE_G2 was
	// genuinely baked from E_ss_TABLE_G2's own values, and would NOT if
	// E_avg_TABLE_G2 were stale, mis-indexed, or copy-pasted from the
	// separable table.  (MSLobeZG2's segment-exact integral was
	// considered instead but rejected: it uses a DIFFERENT, piecewise-
	// linear-with-flat-end-caps interpolation model built for exact CDF
	// inversion, not the generator's plain midpoint rule -- comparing
	// against it mixes two distinct quadrature schemes and was measured
	// to disagree by up to ~5e-5, which is quadrature-scheme error, not a
	// table defect.)
	static bool TestEssEavgConsistencyG2( const char* label, const Scalar alpha )
	{
		const int LS = MicrofacetEnergyLUT::LUT_SIZE;
		const Scalar a = r_max( 0.0, r_min( 1.0, (alpha - Scalar(0.01)) / Scalar(0.99) ) ) * Scalar(LS - 1);
		const int ai0 = (int)a;
		const int ai1 = r_min( ai0 + 1, LS - 1 );
		const Scalar af = a - ai0;

		double integral = 0.0;
		for( int k = 0; k < LS; k++ )
		{
			const double mu = ( k + 0.5 ) / double(LS);
			const double dmu = 1.0 / double(LS);
			const double essBlended = double(1 - af) * double( MicrofacetEnergyLUT::E_ss_TABLE_G2[ai0][k] )
				+ double(af) * double( MicrofacetEnergyLUT::E_ss_TABLE_G2[ai1][k] );
			integral += essBlended * mu * dmu;
		}
		const double lhs = 2.0 * integral;
		const double rhs = double( MicrofacetEnergyLUT::LookupEavgG2( alpha ) );
		const double diff = std::fabs( lhs - rhs );
		const bool passed = diff < 1e-6;

		std::ostringstream oss;
		oss << label << " 2*sum(EssG2*mu*dmu)=" << std::fixed << std::setprecision(8) << lhs
			<< " LookupEavgG2=" << rhs << " diff=" << std::scientific << diff;
		return Report( oss.str(), passed );
	}

	// (b) Furnace-style check at F0=0.9, where F_ms != 1 (ComputeFms is a
	// genuine, non-collapsing function of Eavg at this F_avg), so `total`
	// actually depends on the table contents instead of being forced to a
	// fixed point by ComputeFms's own algebra.  There is no simple
	// independent closed-form target for `total` itself at F0<1 (Schlick
	// reflectance varies with angle in the real renderer, unlike the F0=1
	// case where it is angle-independent by construction) -- but
	// ComputeFms(F_avg<=1, Eavg in [0,1]) provably satisfies F_ms <= F_avg
	// (denom = 1-F_avg*(1-Eavg) >= F_avg*Eavg for F_avg<=1), so
	// total = F0*Ess_i + (1-Ess_i)*F_ms is provably bounded in [0, F0] for
	// ANY table content in range -- a genuine energy-conservation bound,
	// not a tautology, that a corrupted Eavg/Ess table (e.g. producing a
	// value outside [0,1]) can violate.  Combined with the explicit
	// F_ms < 1 assertion (documenting the check is not vacuously sitting
	// at the F0=1 fixed point), this keeps the F0=0.9 configuration's
	// actual numbers visible without asserting a value nobody can derive.
	static bool TestFurnaceStyleAtF0Point9(
		const char* label, const Scalar alpha, const double thetaDeg )
	{
		const double theta = thetaDeg * PI / 180.0;
		const Scalar cosWi = std::cos( theta );
		const Scalar F0 = Scalar(0.9);

		const Scalar Eavg = MicrofacetEnergyLUT::LookupEavgG2( alpha );
		const Scalar Ess_i = MicrofacetEnergyLUT::LookupEssG2( cosWi, alpha );
		const Scalar F_ms = MicrofacetEnergyLUT::ComputeFms<Scalar>( F0, Eavg );
		const Scalar total = F0 * Ess_i + ( Scalar(1) - Ess_i ) * F_ms;

		const bool fmsNontrivial = F_ms < Scalar( 0.999 );
		const bool boundOk = ( total >= Scalar(-1e-9) ) && ( total <= F0 + Scalar(1e-9) );
		const bool passed = fmsNontrivial && boundOk;

		std::ostringstream oss;
		oss << label << " Ess_i=" << std::fixed << std::setprecision(6) << Ess_i
			<< " Eavg=" << Eavg << " F_ms=" << F_ms << " total=" << total
			<< " (F0=" << F0 << ", bound [0," << F0 << "])";
		return Report( oss.str(), passed );
	}
}

int main()
{
	std::cout << "=== GGX Height-Correlated Energy LUT Test (DL-63) ===\n";
	GlobalLog();

	bool passed = true;

	std::cout << "\n--- DL-63: LookupEssG2 matches an independent height-correlated-G2 quadrature ---\n";
	// The three previously-failing GGXDiffuseTransmissionTest configs:
	passed &= TestLookupEssG2MatchesIndependentQuadrature( "alpha=0.6 theta=80 (was 1.0772)", 0.6, 80.0, 4001 );
	passed &= TestLookupEssG2MatchesIndependentQuadrature( "alpha=1.0 theta=60 (was 1.0432)", 1.0, 60.0, 4002 );
	passed &= TestLookupEssG2MatchesIndependentQuadrature( "alpha=1.0 theta=80 (was 1.1467)", 1.0, 80.0, 4003 );
	// Additional spot checks spanning the domain.
	passed &= TestLookupEssG2MatchesIndependentQuadrature( "alpha=0.02 theta=0",  0.02, 0.0,  4004 );
	passed &= TestLookupEssG2MatchesIndependentQuadrature( "alpha=0.16 theta=45", 0.16, 45.0, 4005 );
	passed &= TestLookupEssG2MatchesIndependentQuadrature( "alpha=0.3 theta=30",  0.3,  30.0, 4006 );
	passed &= TestLookupEssG2MatchesIndependentQuadrature( "alpha=0.8 theta=20",  0.8,  20.0, 4007 );

	std::cout << "\n--- DL-63: LookupEss (separable, unchanged) diverges from the G2 model ---\n";
	passed &= TestLookupEssSeparableModelDiffersFromG2( "alpha=0.6 theta=80", 0.6, 80.0, 5001 );
	passed &= TestLookupEssSeparableModelDiffersFromG2( "alpha=1.0 theta=60", 1.0, 60.0, 5002 );
	passed &= TestLookupEssSeparableModelDiffersFromG2( "alpha=1.0 theta=80", 1.0, 80.0, 5003 );

	std::cout << "\n--- P2-1(a): E_ss_G2/E_avg_G2 exact-integral cross-table consistency ---\n";
	passed &= TestEssEavgConsistencyG2( "alpha=0.05", 0.05 );
	passed &= TestEssEavgConsistencyG2( "alpha=0.16", 0.16 );
	passed &= TestEssEavgConsistencyG2( "alpha=0.3",  0.3  );
	passed &= TestEssEavgConsistencyG2( "alpha=0.6",  0.6  );
	passed &= TestEssEavgConsistencyG2( "alpha=0.808", 0.808 );
	passed &= TestEssEavgConsistencyG2( "alpha=1.0",  1.0  );

	std::cout << "\n--- P2-1(b): furnace-style bound at F0=0.9 (Fms != 1, depends on the table) ---\n";
	passed &= TestFurnaceStyleAtF0Point9( "alpha=0.6 theta=80", 0.6, 80.0 );
	passed &= TestFurnaceStyleAtF0Point9( "alpha=1.0 theta=60", 1.0, 60.0 );
	passed &= TestFurnaceStyleAtF0Point9( "alpha=1.0 theta=80", 1.0, 80.0 );
	passed &= TestFurnaceStyleAtF0Point9( "alpha=0.02 theta=0", 0.02, 0.0 );

	std::cout << "\n--- P2-2: uniform-hemisphere quadrature, own D/Lambda/G2 + own RNG (no VNDF) ---\n";
	passed &= TestUniformHemisphereIndependentQuadrature( "alpha=1.0 mu=0.0156",  1.0,   0.0156, 6001 );
	passed &= TestUniformHemisphereIndependentQuadrature( "alpha=0.649 mu=0.1719", 0.649, 0.1719, 6002 );
	passed &= TestUniformHemisphereIndependentQuadrature( "alpha=0.808 mu=0.4844", 0.808, 0.4844, 6003 );

	std::cout << "\nGGXHeightCorrelatedEnergyLUTTest: " << checks << " checks, " << failures << " failures\n";
	std::cout << "=== " << ( passed ? "ALL TESTS PASSED" : "TESTS FAILED" ) << " ===\n";
	return passed ? 0 : 1;
}
