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
#include <vector>

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

	// DL-77 P1 red proof (debt-ggx3): the anisotropic Kulla-Conty energy
	// table is baked with phi=0 meaning "aligned with the axis this
	// generator calls alphaX" -- so relabeling which physical roughness
	// is "X" and which is "Y" (a pure coordinate-frame choice) must leave
	// the ENERGY unchanged, provided the azimuth is relabeled to match:
	// LookupEssG2AnisoDirectional(cosTheta, localX, localY, alphaX,
	// alphaY) == LookupEssG2AnisoDirectional(cosTheta, localY, localX,
	// alphaY, alphaX) for the identical physical wi (swapping which axis
	// is "X" swaps localX/localY too).  At HEAD before the debt-ggx3 P1
	// fix, the table's azimuth READ (AnisoPhiIndex) ignored which of
	// alphaX,alphaY was larger while the table's azimuth BAKE always
	// treated the SMALLER alpha as the phi=0 axis -- so a caller passing
	// alphaX>alphaY (the swapped labelling) read a MIRRORED azimuth,
	// breaking this identity outright (see the two RunFurnaceBothLabels
	// configurations below, which independently reproduce the ledger's
	// cited furnace numbers on the unfixed code: 1.1699 and 0.7613).
	static bool TestRelabelSymmetry(
		const char* label, const Scalar alphaX, const Scalar alphaY,
		const double thetaDeg, const double phiDeg )
	{
		const double theta = thetaDeg * PI / 180.0;
		const double phi = phiDeg * PI / 180.0;
		const double phiSwapped = ( 90.0 - phiDeg ) * PI / 180.0;
		const Scalar cosTheta = std::cos( theta );
		const Scalar sinTheta = std::sin( theta );

		const Scalar localX = sinTheta * std::cos( phi );
		const Scalar localY = sinTheta * std::sin( phi );
		const Scalar v1 = MicrofacetEnergyLUT::LookupEssG2AnisoDirectional( cosTheta, localX, localY, alphaX, alphaY );

		const Scalar localX2 = sinTheta * std::cos( phiSwapped );
		const Scalar localY2 = sinTheta * std::sin( phiSwapped );
		const Scalar v2 = MicrofacetEnergyLUT::LookupEssG2AnisoDirectional( cosTheta, localX2, localY2, alphaY, alphaX );

		const double diff = std::fabs( (double)v1 - (double)v2 );
		// P3 (debt-ggx3 review round 3): the fix mirrors the canonical
		// alphaX<=alphaY half into the alphaX>alphaY half using the SAME
		// Monte-Carlo samples (see the DL-77 ledger row), so this identity
		// is exact by construction, not approximate -- measured diffs are
		// <=1.11e-16 (double-precision ULP noise) across all 7 rows below,
		// four orders of magnitude under the old 1e-6 tolerance. Tightened
		// to 1e-12 so a real regression (e.g. a re-introduced non-mirrored
		// bake) can't hide inside the gap.
		const bool passed = diff < 1e-12;

		std::ostringstream oss;
		oss << label << " Ess(aX=" << alphaX << ",aY=" << alphaY << ",phi=" << phiDeg << ")="
			<< std::fixed << std::setprecision(6) << v1
			<< " vs Ess(aX=" << alphaY << ",aY=" << alphaX << ",phi=" << (90.0-phiDeg) << ")="
			<< v2 << "  diff=" << std::scientific << diff;
		return Report( oss.str(), passed );
	}

	//////////////////////////////////////////////////////////////////
	// DL-86 red proof: LookupEssG2/LookupEssG2AnisoDirectional used to
	// flat-clamp cosTheta below the first LUT bin center
	// c0=0.5/LUT_SIZE~=0.0156 to that bin's own value, under-reading the
	// TRUE (still-rising, toward the Ess_G2(cosTheta=0)=1 boundary)
	// single-scatter directional albedo right at the grazing limit --
	// see MicrofacetEnergyLUT.h's LookupEssG2/LookupEssG2AnisoDirectional
	// comments and docs/DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md "DL-86" for
	// the fix (a one-sided extrapolation anchored at that exact
	// boundary).  This promotes the doc's brute-force-vs-lookup numbers
	// into GATING checks: every row below is RED on pre-fix master (the
	// flat clamp) and PASSES against the fixed lookup.
	//////////////////////////////////////////////////////////////////

	// Anisotropic twin of MonteCarloEssG2 above: independent VNDF
	// quadrature of the height-correlated directional albedo for a wi at
	// a specific tangent-space azimuth `phiWiDeg`, using
	// MicrofacetUtils::VNDF_Sample_Aniso/GGX_G2_Aniso/GGX_G1_Aniso -- the
	// exact primitives LookupEssG2AnisoDirectional's production callers
	// (GGXBRDF::value/valueNM, GGXSPF::Scatter/ScatterNM/Pdf/PdfNM) use
	// for their own Ess_i/Ess_o terms.
	static double MonteCarloEssG2AnisoDirectional( const Scalar alphaX, const Scalar alphaY, const Scalar cosWi, const double phiWiDeg, unsigned int numSamples, unsigned int seed, double& outStdErr )
	{
		OrthonormalBasis3D onb;
		onb.CreateFromW( Vector3( 0, 0, 1 ) );

		const Scalar sinWi = sqrt( r_max( Scalar(0), Scalar(1) - cosWi * cosWi ) );
		const double phiWi = phiWiDeg * PI / 180.0;
		const Vector3 wi( sinWi * cos(phiWi), sinWi * sin(phiWi), cosWi );

		const Scalar G1wi = MicrofacetUtils::GGX_G1_Aniso( alphaX, alphaY, wi );

		std::mt19937_64 rng( seed );
		std::uniform_real_distribution<double> uni( 0.0, 1.0 );

		double sum = 0.0;
		double sumSq = 0.0;
		unsigned int n = 0;
		for( unsigned int s = 0; s < numSamples; ++s )
		{
			const Scalar u1 = uni( rng );
			const Scalar u2 = uni( rng );
			const Vector3 m = MicrofacetUtils::VNDF_Sample_Aniso( wi, onb, alphaX, alphaY, u1, u2 );
			const Scalar wiDotM = Vector3Ops::Dot( wi, m );
			if( wiDotM <= 0 ) { ++n; continue; }

			const Vector3 wo = Vector3Ops::Normalize( m * ( 2.0 * wiDotM ) - wi );
			const Scalar cosWo = wo.z;
			double weight = 0.0;
			if( cosWo > 0 )
			{
				const Scalar G2 = MicrofacetUtils::GGX_G2_Aniso( alphaX, alphaY, wi, wo );
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

	// DL-86: SEPARABLE-model (G = G1(wi)*G1(wo)) twin of MonteCarloEssG2
	// above -- the model CookTorranceBRDF/SPF render with, and the one
	// E_ss_TABLE/LookupEss are calibrated to.  With VNDF sampling the
	// G1(wi) factor cancels against the proposal density, so the
	// per-sample weight is plain G1(wo) (see the generator's own comment
	// on the same identity).  LookupEss's grazing end-cap had no direct
	// coverage before DL-86: this test only ever drove LookupEss as a
	// NEGATIVE control against the height-correlated quadrature.
	static double MonteCarloEssSeparable( const Scalar alpha, const Scalar cosWi, unsigned int numSamples, unsigned int seed, double& outStdErr )
	{
		OrthonormalBasis3D onb;
		onb.CreateFromW( Vector3( 0, 0, 1 ) );

		const Scalar sinWi = sqrt( r_max( Scalar(0), Scalar(1) - cosWi * cosWi ) );
		const Vector3 wi( sinWi, 0, cosWi );

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
			if( cosWo > 0 ) weight = MicrofacetUtils::GGX_G1( alpha, cosWo );
			sum += weight;
			sumSq += weight * weight;
			++n;
		}

		const double mean = sum / n;
		const double variance = r_max( 0.0, sumSq / n - mean * mean );
		outStdErr = sqrt( variance / n );
		return mean;
	}

	static bool TestDL86SeparableEndCap( const char* label, const Scalar alpha, const double cosTheta, const double toleranceAbs, unsigned int seed )
	{
		double stdErr = 0.0;
		const double reference = MonteCarloEssSeparable( alpha, cosTheta, 20000000u, seed, stdErr );
		const double looked = MicrofacetEnergyLUT::LookupEss( cosTheta, alpha );

		const double tol = 8.0 * stdErr + toleranceAbs;
		const double diff = std::fabs( looked - reference );
		const bool passed = diff <= tol;

		std::ostringstream oss;
		oss << label << " LookupEss=" << std::fixed << std::setprecision(6) << looked
			<< " quadrature=" << reference << "+/-" << std::setprecision(6) << stdErr
			<< " diff=" << diff << " (" << (diff/reference*100.0) << "%) tol=" << tol;
		return Report( oss.str(), passed );
	}

	// `toleranceAbs` is PER-ALPHA, not one global floor.  Post-DL-86 the
	// cosTheta axis is resolved by a baked sub-grid whose own residual is
	// <=0.11% for any alpha the LUT's alpha axis actually resolves; what
	// remains at the low end is the ALPHA axis (DL-105): alpha<0.01
	// clamps to row 0 outright, and row 0 (alpha=0.01) to row 1
	// (alpha=0.0419) is a 4.2x ratio inside one interpolation cell.  Each
	// row below therefore carries the residual its own alpha really has,
	// measured, instead of one loose floor that would hide a regression
	// at the alphas where the fix is tight.
	static bool TestDL86IsotropicEndCap( const char* label, const Scalar alpha, const double cosTheta, const double toleranceAbs, unsigned int seed )
	{
		double stdErr = 0.0;
		const double reference = MonteCarloEssG2( alpha, cosTheta, 20000000u, seed, stdErr );
		const double looked = MicrofacetEnergyLUT::LookupEssG2( cosTheta, alpha );

		const double tol = 8.0 * stdErr + toleranceAbs;
		const double diff = std::fabs( looked - reference );
		const bool passed = diff <= tol;

		std::ostringstream oss;
		oss << label << " LookupEssG2=" << std::fixed << std::setprecision(6) << looked
			<< " quadrature=" << reference << "+/-" << std::setprecision(6) << stdErr
			<< " diff=" << diff << " (" << (diff/reference*100.0) << "%) tol=" << tol;
		return Report( oss.str(), passed );
	}

	static bool TestDL86AnisoDirectionalEndCap( const char* label, const Scalar alphaX, const Scalar alphaY, const double phiDeg, const double cosTheta, const double toleranceAbs, unsigned int seed )
	{
		double stdErr = 0.0;
		const double reference = MonteCarloEssG2AnisoDirectional( alphaX, alphaY, cosTheta, phiDeg, 20000000u, seed, stdErr );
		const Scalar sinWi = sqrt( r_max( Scalar(0), Scalar(1) - Scalar(cosTheta)*Scalar(cosTheta) ) );
		const double phi = phiDeg * PI / 180.0;
		const Scalar localX = sinWi * cos(phi), localY = sinWi * sin(phi);
		const double looked = MicrofacetEnergyLUT::LookupEssG2AnisoDirectional( cosTheta, localX, localY, alphaX, alphaY );

		// `toleranceAbs` is per-row, and what it bounds depends on which
		// axes the row interpolates.  At a NODE-EXACT (alphaX, alphaY,
		// phi) configuration cosTheta is the only interpolated axis, so
		// the row measures the DL-86 end-cap alone: <=0.338% relative
		// after the round-2 geometric refinement, over the whole probe
		// set down to cos=1e-4.  OFF-node the row also carries the aniso
		// grid's own interpolation error on up to three further axes --
		// <=0.57% when one alpha is off-node, 3-4% at the debt-ggx3
		// sweep's corner (off-node on all three) -- and that residual is
		// DL-105/DL-77's, not this end-cap's; it does not move with the
		// sub-grid.  See docs/DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md
		// "DL-86" for the full residual table.
		const double tol = 8.0 * stdErr + toleranceAbs;
		const double diff = std::fabs( looked - reference );
		const bool passed = diff <= tol;

		std::ostringstream oss;
		oss << label << " LookupEssG2AnisoDirectional=" << std::fixed << std::setprecision(6) << looked
			<< " quadrature=" << reference << "+/-" << std::setprecision(6) << stdErr
			<< " diff=" << diff << " (" << (diff/reference*100.0) << "%) tol=" << tol;
		return Report( oss.str(), passed );
	}

	//////////////////////////////////////////////////////////////////
	// DL-86 round 2: the H6 multiscatter-lobe sampler for the
	// ANISOTROPIC lobe.  SampleMSCosThetaG2Aniso inverts a piecewise-
	// linear reconstruction of the Ess row built by
	// MSLobeDetail::BuildSegmentsFromRowN, while MSPdfG2Aniso reports
	// (1-LookupEssG2Aniso(c))*c/(PI*Z).  Those are two DIFFERENT pieces
	// of code reading the SAME tables, so refining the grazing sub-grid
	// under one of them and not the other would silently break the
	// estimator's "the pdf describes what the sampler draws" invariant.
	// Both checks below are about the region the refinement touched.
	//////////////////////////////////////////////////////////////////

	// (a) The solid-angle density integrates to 1 over the hemisphere:
	//     2*PI * integral_0^1 MSPdfG2Aniso(c) dc == 1.  Composite
	//     Simpson, refined separately on [0,c0] (where the sub-grid
	//     lives, and where a uniform whole-range rule would put only a
	//     handful of nodes) and on [c0,1].
	static bool TestMSPdfAnisoNormalization( const char* label, const Scalar alphaX, const Scalar alphaY )
	{
		const Scalar Z = MicrofacetEnergyLUT::MSLobeZG2Aniso( alphaX, alphaY );
		const double c0 = 0.5 / 32.0;

		auto simpson = [&]( const double lo, const double hi, const int n ) -> double
		{
			const double h = ( hi - lo ) / double(n);
			double acc = 0.0;
			for( int i = 0; i <= n; ++i )
			{
				const double c = lo + double(i) * h;
				const double w = ( i == 0 || i == n ) ? 1.0 : ( ( i & 1 ) ? 4.0 : 2.0 );
				acc += w * MicrofacetEnergyLUT::MSPdfG2Aniso( c, alphaX, alphaY, Z );
			}
			return acc * h / 3.0;
		};

		// 2^16 intervals on [0,c0] resolves even the narrowest sub-grid
		// interval (c0/256) with ~256 nodes.
		const double integral = 2.0 * PI * ( simpson( 0.0, c0, 65536 ) + simpson( c0, 1.0, 262144 ) );
		const bool passed = std::fabs( integral - 1.0 ) <= 1e-4;

		std::ostringstream oss;
		oss << label << " 2PI*int MSPdfG2Aniso dc=" << std::fixed << std::setprecision(8) << integral
			<< " Z=" << Z;
		return Report( oss.str(), passed );
	}

	// (b) A histogram of SampleMSCosThetaG2Aniso draws against the
	//     cosTheta marginal 2*PI*MSPdfG2Aniso, binned on the DL-86
	//     sub-grid's own node intervals (plus one bin for everything
	//     above c0).  Gated where the expected count supports a test;
	//     the innermost bins carry mass ~ c^2 and are recorded instead.
	static bool TestMSSamplerAnisoHistogram( const char* label, const Scalar alphaX, const Scalar alphaY, const long numSamples, unsigned int seed )
	{
		const Scalar Z = MicrofacetEnergyLUT::MSLobeZG2Aniso( alphaX, alphaY );
		const double c0 = 0.5 / 32.0;

		// Bin edges, built HERE rather than read from the header, so the
		// sampler is not binned by its own node list: geometric octaves
		// c0/8 * 2^-j down to c0/2048, then the eight uniform c0/8
		// steps, then everything above c0 in one bin.
		std::vector<double> edges;
		edges.push_back( 0.0 );
		for( int j = 8; j >= 1; --j ) edges.push_back( ( c0 / 8.0 ) * std::pow( 2.0, -double(j) ) );
		for( int k = 1; k <= 8; ++k ) edges.push_back( double(k) * c0 / 8.0 );
		edges.push_back( 1.0 );

		const size_t nb = edges.size() - 1;
		std::vector<long> counts( nb, 0 );

		std::mt19937_64 rng( seed );
		std::uniform_real_distribution<double> uni( 0.0, 1.0 );
		for( long s = 0; s < numSamples; ++s )
		{
			// Argument order is (alphaX, alphaY, u1) -- the order
			// GGXSPF.cpp:387 calls it in.
			const Scalar c = MicrofacetEnergyLUT::SampleMSCosThetaG2Aniso( alphaX, alphaY, uni( rng ) );
			size_t b = 0;
			while( b + 1 < nb && c >= edges[b+1] ) ++b;
			counts[b]++;
		}

		bool passed = true;
		std::ostringstream detail;
		for( size_t b = 0; b < nb; ++b )
		{
			// Expected probability of this bin: Simpson on the marginal.
			const int n = 256;
			const double h = ( edges[b+1] - edges[b] ) / double(n);
			double acc = 0.0;
			for( int i = 0; i <= n; ++i )
			{
				const double c = edges[b] + double(i) * h;
				const double w = ( i == 0 || i == n ) ? 1.0 : ( ( i & 1 ) ? 4.0 : 2.0 );
				acc += w * 2.0 * PI * MicrofacetEnergyLUT::MSPdfG2Aniso( c, alphaX, alphaY, Z );
			}
			const double p = acc * h / 3.0;
			const double expected = p * double(numSamples);
			if( expected < 50.0 ) continue;			// recorded below, not gated
			const double sigma = sqrt( expected * ( 1.0 - p ) );
			const double z = ( double(counts[b]) - expected ) / sigma;
			if( std::fabs( z ) > 5.0 ) passed = false;
			detail << " [" << std::scientific << std::setprecision(2) << edges[b]
				<< "," << edges[b+1] << ") obs=" << std::fixed << std::setprecision(0) << double(counts[b])
				<< " exp=" << expected << " z=" << std::setprecision(2) << z;
		}

		std::ostringstream oss;
		oss << label << " sub-grid bins:" << detail.str();
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

	std::cout << "\n--- DL-77 P1: relabel-symmetry, LookupEssG2AnisoDirectional(aX,aY,phi) == (aY,aX,90-phi) ---\n";
	passed &= TestRelabelSymmetry( "(.9,.1) theta=70 az=0   (ledger red: 1.1699 furnace)",   0.9,  0.1,  70.0, 0.0 );
	passed &= TestRelabelSymmetry( "(.827,.09) theta=80 az=90 (ledger red: 0.7613 furnace)", 0.827, 0.09, 80.0, 90.0 );
	passed &= TestRelabelSymmetry( "(.5,.05) theta=80 az=0",                                 0.5,  0.05, 80.0, 0.0 );
	passed &= TestRelabelSymmetry( "(.3,.7) theta=45 az=30",                                 0.3,  0.7,  45.0, 30.0 );
	passed &= TestRelabelSymmetry( "(.05,.5) theta=60 az=60",                                0.05, 0.5,  60.0, 60.0 );
	passed &= TestRelabelSymmetry( "(.2,.8) theta=20 az=10",                                 0.2,  0.8,  20.0, 10.0 );
	passed &= TestRelabelSymmetry( "(.02,1.0) theta=60 az=0",                                0.02, 1.0,  60.0, 0.0 );

	std::cout << "\n--- DL-86: isotropic grazing end-cap, LookupEssG2 below c0=0.5/32~=0.0156 ---\n";
	{
		// Per-alpha tolerance (see TestDL86IsotropicEndCap's comment).
		// 0.004 is the tight band: the baked sub-grid's own residual at
		// these alphas is <=0.11% relative, so 0.004 absolute leaves
		// margin for Monte-Carlo noise without being able to pass either
		// pre-fix model (flat clamp: up to 9.9% relative; straight line
		// from the boundary: up to 5.7%).  The two loose rows name their
		// cause: alpha=0.02 sits inside the 4.2x-wide first alpha cell
		// and alpha=0.005 is BELOW the table's alpha range entirely and
		// clamps to row 0 -- both DL-105, neither a cosTheta-axis effect
		// (they do not move with SUB_SIZE).
		struct Row { double alpha; double tolAbs; };
		const Row rows[] = {
			{ 0.005, 0.060 },	// DL-105: alpha below the table range, clamps to row 0
			{ 0.01,  0.004 },
			{ 0.02,  0.025 },	// DL-105: first alpha cell spans 0.01 -> 0.0419
			{ 0.05,  0.004 },
			{ 0.3,   0.004 },
			{ 1.0,   0.004 },
		};
		// Every value here is strictly below c0=0.015625 -- the interval
		// DL-86 is about.  cos=0.03 (the first ORDINARY interpolation
		// span, between bin 0 and bin 1) is covered separately below:
		// the true curve has real structure there too, but that is the
		// main grid's own resolution (DL-105), untouched by this fix.
		const double coss[] = { 0.0001, 0.001, 0.002, 0.005, 0.008, 0.01, 0.0156 };
		unsigned int seed = 86001;
		for( const Row& r : rows )
		{
			for( double c : coss )
			{
				std::ostringstream label;
				label << "alpha=" << r.alpha << " cos=" << c;
				passed &= TestDL86IsotropicEndCap( label.str().c_str(), r.alpha, c, r.tolAbs, seed++ );
			}
		}
	}

	std::cout << "\n--- DL-86: isotropic grazing end-cap, SEPARABLE LookupEss (CookTorrance's table) ---\n";
	{
		// P1-4: the separable table's row 0 RISES with cosTheta (0.8947
		// at c0, 0.9731 at the next bin), so the bin0->bin1 secant an
		// earlier draft of this fix extrapolated with pointed AWAY from
		// the true cosTheta->0 limit and was WORSE than the flat clamp it
		// replaced (measured alpha=0.01 cos=0.002: truth 0.9174, flat
		// clamp 0.8947 = 2.5% low, secant 0.8606 = 6.2% low).  The baked
		// sub-grid plus the baked per-alpha boundary constant
		// (E_ss_LIMIT_TABLE) removes the guesswork entirely.
		struct Row { double alpha; double tolAbs; };
		const Row rows[] = {
			{ 0.005, 0.060 },	// DL-105, as above
			{ 0.01,  0.004 },
			{ 0.02,  0.025 },	// DL-105, as above
			{ 0.05,  0.004 },
			{ 0.3,   0.004 },
			{ 1.0,   0.004 },
		};
		// Every value here is strictly below c0=0.015625 -- the interval
		// DL-86 is about.  cos=0.03 (the first ORDINARY interpolation
		// span, between bin 0 and bin 1) is covered separately below:
		// the true curve has real structure there too, but that is the
		// main grid's own resolution (DL-105), untouched by this fix.
		const double coss[] = { 0.0001, 0.001, 0.002, 0.005, 0.008, 0.01, 0.0156 };
		unsigned int seed = 86501;
		for( const Row& r : rows )
		{
			for( double c : coss )
			{
				std::ostringstream label;
				label << "alpha=" << r.alpha << " cos=" << c;
				passed &= TestDL86SeparableEndCap( label.str().c_str(), r.alpha, c, r.tolAbs, seed++ );
			}
		}
	}

	std::cout << "\n--- DL-86 control: the first ORDINARY span (cos=0.03, above c0) is untouched by this fix ---\n";
	{
		// Above c0 nothing changed: same bilinear interpolation between
		// bin 0 and bin 1 as before DL-86.  These rows are recorded, with
		// the residual the MAIN grid actually has there (up to ~3.3%
		// relative at alpha=0.02, where the first alpha cell and the
		// first cosTheta span are both at their widest), so a future
		// change that "fixes" the end-cap by disturbing the interior
		// shows up here instead of hiding.  The tolerance is that
		// measured residual plus margin -- deliberately NOT the 0.004 the
		// below-c0 rows get, and tracked as DL-105, not DL-86.
		struct Row { double alpha; double tolAbs; };
		const Row rows[] = {
			{ 0.005, 0.060 },	// DL-105: below the table's alpha range
			{ 0.01,  0.040 },
			{ 0.02,  0.040 },
			{ 0.05,  0.040 },
			{ 0.3,   0.040 },
			{ 1.0,   0.040 },
		};
		unsigned int seed = 86701;
		for( const Row& r : rows )
		{
			std::ostringstream l1; l1 << "alpha=" << r.alpha << " cos=0.03 (G2)";
			passed &= TestDL86IsotropicEndCap( l1.str().c_str(), r.alpha, 0.03, r.tolAbs, seed++ );
			std::ostringstream l2; l2 << "alpha=" << r.alpha << " cos=0.03 (separable)";
			passed &= TestDL86SeparableEndCap( l2.str().c_str(), r.alpha, 0.03, r.tolAbs, seed++ );
		}
	}

	std::cout << "\n--- DL-86: anisotropic per-azimuth grazing end-cap, LookupEssG2AnisoDirectional ---\n";
	{
		// Tolerances here are per-GROUP and named, for the same reason as
		// the isotropic rows above: what is left after DL-86 is not the
		// cosTheta end-cap.
		//
		// kBelowC0Tol (the OFF-node rows, cosTheta < c0): measured
		// residual <=0.57% relative (worst absolute 0.00531, at
		// alphaX=0.05 -- which is off-node between the alpha nodes 0.01
		// and 0.0530, so what it measures is the alpha axis, DL-105),
		// against an independent 20M-sample per-azimuth quadrature.
		// 0.008 absolute is a real gate; pre-fix (flat clamp) the same
		// rows read up to 4.0% and would fail it.
		//
		// kNodeExactTol (the NODE-EXACT rows, where cosTheta is the only
		// interpolated axis and this end-cap is the only thing measured):
		// after the round-2 geometric refinement the worst residual over
		// the 8-value probe set at 8 node-exact configurations is 0.338%
		// relative / 0.00190 absolute, so 0.004 is the gate.  Round 1
		// read up to 5.91% / 0.0484 on the same rows -- 12x this band.
		//
		// kAboveC0Tol (cosTheta=0.03, the first ORDINARY span): unchanged
		// by DL-86 and left at the main grid's own residual, up to 2.43%
		// -- DL-105, recorded not gated tightly.
		//
		// kGridCornerTol: the debt-ggx3 sweep's own cited worst case sits
		// OFF-node on all three of alphaX (0.0361, between nodes 0.01 and
		// 0.0530), alphaY (0.9627, between 0.9570 and 1.0) and phi (5
		// degrees, between nodes 0 and 7.5), so what it measures is the
		// aniso grid's interpolation error, not the end-cap.  DL-86 makes
		// the value at every NODE right; the blend between nodes is
		// DL-105/DL-77's own tracked residual, and at this corner it is
		// 3.0-4.0% -- slightly WORSE than the straight-line model this
		// slice replaced happened to read there (2.1-2.4%), which is a
		// coincidence of that model's error pointing the same way as the
		// interpolation error, not evidence for it: the straight line is
		// 2-9x worse at every NODE-exact configuration below.
		const double kBelowC0Tol = 0.008;
		const double kNodeExactTol = 0.004;
		const double kAboveC0Tol = 0.035;
		const double kGridCornerTol = 0.045;

		struct Case { double aX, aY, phiDeg; };
		const Case cases[] = {
			{ 0.9, 0.1, 0.0 }, { 0.9, 0.1, 45.0 }, { 0.9, 0.1, 90.0 },
			{ 0.05, 0.5, 0.0 }, { 0.05, 0.5, 45.0 }, { 0.05, 0.5, 90.0 },
		};
		// DL-86 round 2: the probe set starts at 1e-4, not at 0.002.  The
		// round-1 set's smallest value (0.002) sat just ABOVE the first
		// baked sub-node (c0/8 = 1.953e-3), so the WHOLE first
		// sub-interval -- the one straight ramp from the exact
		// cosTheta->0 anchor to that node -- went unprobed, and it is the
		// interval this end-cap is least able to follow: for an
		// anisotropic pair the approach to 1 only begins at
		// cos << alphaX*sinTheta, so at alphaX=0.01 the true curve is
		// still at 0.735 where the isotropic alpha=0.01 curve has already
		// reached 0.958.  These 8 values put at least one probe in every
		// sub-interval of the refined grid (see ANISO_SUB_FINE in
		// MicrofacetEnergyLUT.h), down to the cos below which no
		// production shading direction lands (1e-4 is theta=89.994 deg).
		const double coss[] = { 0.0001, 0.00025, 0.0005, 0.001, 0.002, 0.005, 0.01, 0.0156 };
		unsigned int seed = 86101;
		for( const Case& c : cases )
		{
			for( double cosTheta : coss )
			{
				std::ostringstream label;
				label << "aX=" << c.aX << " aY=" << c.aY << " phi=" << c.phiDeg << " cos=" << cosTheta;
				passed &= TestDL86AnisoDirectionalEndCap( label.str().c_str(), c.aX, c.aY, c.phiDeg, cosTheta, kBelowC0Tol, seed++ );
			}
			std::ostringstream label;
			label << "aX=" << c.aX << " aY=" << c.aY << " phi=" << c.phiDeg << " cos=0.03 (above c0, control)";
			passed &= TestDL86AnisoDirectionalEndCap( label.str().c_str(), c.aX, c.aY, c.phiDeg, 0.03, kAboveC0Tol, seed++ );
		}

		// EXACT grid nodes on all three interpolated axes (alphaX, alphaY
		// at 0.01 + 0.99*k/23; phi at multiples of 7.5 degrees), so the
		// ONLY interpolation left is the cosTheta axis -- i.e. these rows
		// isolate exactly what DL-86 changed, with the aniso grid's own
		// resolution factored out.  This group did not exist before this
		// slice, which is why the aniso end-cap's residual was previously
		// only quotable at a configuration that confounds the two.
		//
		// (0.01,1.0,phi=0) and its axis-swapped twin (1.0,0.01,phi=90)
		// are the WORST configurations this end-cap has: the queried
		// azimuth is aligned with the 0.01 axis, so the masking of wi
		// only takes over at cos << 0.01, while the scattered lobe is
		// spread over the alpha=1.0 axis -- the pair the round-2 review
		// measured at +5.9% before the grid was refined.
		// (0.8709,0.0961,phi=90) is the node-exact stand-in for the
		// review's third named configuration, (0.9,0.1,phi=90): 0.9 and
		// 0.1 are NOT grid nodes (the alpha axis is 0.01+0.99k/23), so
		// (0.9,0.1) can only be probed off-node, which it is -- in the
		// `cases` group above, under that group's own tolerance.
		const Case nodeCases[] = {
			{ 0.01, 1.0, 0.0 }, { 0.01, 1.0, 45.0 }, { 0.01, 1.0, 90.0 },
			{ 1.0, 0.01, 0.0 }, { 1.0, 0.01, 90.0 },
			{ 0.01 + 0.99 * 20.0 / 23.0, 0.01 + 0.99 * 2.0 / 23.0, 90.0 },
			{ 0.01 + 0.99 * 5.0 / 23.0, 0.01 + 0.99 * 11.0 / 23.0, 7.5 },
			{ 0.01 + 0.99 * 22.0 / 23.0, 0.01 + 0.99 * 2.0 / 23.0, 82.5 },
		};
		for( const Case& c : nodeCases )
		{
			for( double cosTheta : coss )
			{
				std::ostringstream label;
				label << "node-exact aX=" << c.aX << " aY=" << c.aY << " phi=" << c.phiDeg << " cos=" << cosTheta;
				passed &= TestDL86AnisoDirectionalEndCap( label.str().c_str(), c.aX, c.aY, c.phiDeg, cosTheta, kNodeExactTol, seed++ );
			}
		}

		// The debt-ggx3 sweep's cited worst case: off-node on every axis
		// (see kGridCornerTol's comment).  Kept because the DL-77 record
		// quotes it, gated at the residual it really has.
		passed &= TestDL86AnisoDirectionalEndCap( "aX=0.0361 aY=0.9627 phi=5 cos=0.0024 (debt-ggx3 cited worst case, off-node on all 3 axes)", 0.0361, 0.9627, 5.0, 0.0024, kGridCornerTol, 86200 );
		passed &= TestDL86AnisoDirectionalEndCap( "aX=0.0361 aY=0.9627 phi=5 cos=0.005 (off-node on all 3 axes)",  0.0361, 0.9627, 5.0, 0.005,  kGridCornerTol, 86201 );
		passed &= TestDL86AnisoDirectionalEndCap( "aX=0.0361 aY=0.9627 phi=5 cos=0.01 (off-node on all 3 axes)",   0.0361, 0.9627, 5.0, 0.01,   kGridCornerTol, 86202 );
	}

	std::cout << "\n--- DL-86 round 2: the anisotropic H6 multiscatter lobe's sampler and pdf agree on the sub-grid ---\n";
	{
		// A CONSISTENCY PIN, not a red proof: this invariant holds
		// before and after the round-2 refinement (it held on the
		// uniform sub-grid too).  It exists because the refinement
		// changes the node list two independent pieces of code read --
		// MSLobeDetail::BuildSegmentsFromRowN, which
		// SampleMSCosThetaG2Aniso inverts, and LookupEssG2Aniso, which
		// MSPdfG2Aniso evaluates -- so a refinement applied to one and
		// not the other would leave the estimator reporting a density
		// that is not the sampler's.
		passed &= TestMSPdfAnisoNormalization( "aX=0.01 aY=1.0",  0.01, 1.0 );
		passed &= TestMSPdfAnisoNormalization( "aX=1.0 aY=0.01",  1.0,  0.01 );
		passed &= TestMSPdfAnisoNormalization( "aX=0.05 aY=0.5",  0.05, 0.5 );
		passed &= TestMSPdfAnisoNormalization( "aX=0.9 aY=0.1",   0.9,  0.1 );
		passed &= TestMSSamplerAnisoHistogram( "aX=0.01 aY=1.0 (40M draws)", 0.01, 1.0, 40000000L, 86301 );
	}

	std::cout << "\nGGXHeightCorrelatedEnergyLUTTest: " << checks << " checks, " << failures << " failures\n";
	std::cout << "=== " << ( passed ? "ALL TESTS PASSED" : "TESTS FAILED" ) << " ===\n";
	return passed ? 0 : 1;
}
