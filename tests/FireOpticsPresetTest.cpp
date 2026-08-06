//////////////////////////////////////////////////////////////////////
//
//  FireOpticsPresetTest.cpp - Frozen fire optical record gates
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Utilities/FireOptics.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace RISE;

namespace
{
	int failures = 0;

	void Check( const bool condition, const char* message )
	{
		if( !condition ) {
			std::printf("FAIL: %s\n",message);
			++failures;
		}
	}

	bool Near( const double actual, const double expected, const double tolerance )
	{
		return std::fabs(actual-expected) <= tolerance;
	}

	bool HasReason( const FireFidelityEvaluation& evaluation, const char* reason )
	{
		return std::find(evaluation.reasonCodes.begin(), evaluation.reasonCodes.end(),
			std::string(reason)) != evaluation.reasonCodes.end();
	}
}

int main()
{
	std::printf("=== Fire optical preset record tests ===\n");

	const FireOpticsPreset& predictive = FireOpticsPreset::PredictiveV1();
	const FireOpticsPreset& synthetic = FireOpticsPreset::SyntheticRegressionV1();
	Check( predictive.IsValid() && !predictive.IsSynthetic(),
		"the embedded predictive record validates" );
	Check( synthetic.IsValid() && synthetic.IsSynthetic(),
		"the embedded regression fixture validates as explicitly synthetic" );
	Check( predictive.RecordId().size() == 64 && synthetic.RecordId().size() == 64 &&
		predictive.RecordId() != synthetic.RecordId(),
		"predictive and synthetic records have distinct SHA-256 identities" );
	Check( RISECBOR64::SHA256Hex(predictive.RecordBytes()) == predictive.RecordId(),
		"the record ID hashes the exact canonical record bytes" );

	Check( predictive.SootDensityGCM3() == 1.8,
		"the pinned density is part of the loaded predictive payload" );
	Check( Near(predictive.MAC(550.0),8.0,1.0e-12) &&
		Near(predictive.MAC(632.8),6.647,5.0e-4),
		"MAC magnitude anchors pass" );
	Check( Near(predictive.EffectiveAbsorption(380.0)/
		predictive.EffectiveAbsorption(780.0),1.311,5.0e-4),
		"effective-absorption visible shape anchor passes" );
	const double aae = -std::log(predictive.MAC(380.0)/predictive.MAC(780.0)) /
		std::log(380.0/780.0);
	Check( Near(aae,1.377,5.0e-4),
		"MAC visible Angstrom exponent anchor passes" );
	Check( Near(predictive.HotAlbedo(550.0),0.10,2.0e-3) &&
		Near(predictive.HotG(550.0),0.22,1.0e-12),
		"predictive hot-soot 550 nm values are frozen" );
	Check( Near(predictive.CoolExtinctionMass(633.0),8.7,1.0e-12) &&
		Near(predictive.CoolAlbedo(633.0),0.25,1.0e-12) &&
		Near(predictive.CoolG(633.0),0.58,1.0e-12),
		"predictive cool-carbon anchor values are frozen" );
	Check( Near(predictive.CondensedExtinctionMass(380.0),7.63,1.0e-12) &&
		Near(predictive.CondensedExtinctionMass(780.0),2.097,1.0e-12) &&
		predictive.CondensedIRClosureStatus() == "blocked" &&
		predictive.CondensedApplicability().find("fresh") != std::string::npos &&
		predictive.CondensedApplicability().find("flaming") != std::string::npos,
		"condensed-organic table, domain statement, and blocked IR closure are retained" );

	const double volumeFraction = 1.0e-3/predictive.SootDensityKgM3();
	Check( Near(volumeFraction,5.555555555555556e-7,1.0e-21),
		"1 g/m3 at phi=1 converts to the pinned soot volume fraction" );
	Check( Near(predictive.HotAbsorptionMass(550.0),predictive.MAC(550.0),1.0e-12),
		"the 6 pi E f_v/lambda coefficient form recovers normative MAC" );

	const DifferentiableSpectrum& mac = predictive.MACSpectrum();
	bool derivativesEnclosed = true;
	for( std::size_t segment=0; segment<mac.DerivativeEnclosures().size(); segment++ ) {
		const SpectralDerivativeEnclosure& enclosure =
			mac.DerivativeEnclosures()[segment];
		for( unsigned int sample=0; sample<=256; sample++ ) {
			const double wavelength = enclosure.wavelengthMinNM+
				(enclosure.wavelengthMaxNM-enclosure.wavelengthMinNM)*
				static_cast<double>(sample)/256.0;
			const double derivative = mac.Derivative(wavelength);
			derivativesEnclosed = derivativesEnclosed &&
				derivative >= enclosure.derivativeMin &&
				derivative <= enclosure.derivativeMax;
		}
	}
	Check( derivativesEnclosed,
		"analytic PCHIP derivative enclosures contain every sampled derivative" );
	bool derivativeContinuous = true;
	for( std::size_t knot=1; knot+1<mac.Wavelengths().size(); knot++ ) {
		const double wavelength = mac.Wavelengths()[knot];
		const double epsilon = 1.0e-6;
		derivativeContinuous = derivativeContinuous &&
			Near(mac.Derivative(wavelength-epsilon),mac.Derivative(wavelength+epsilon),1.0e-8) &&
			Near(mac.Derivative(wavelength),mac.Slopes()[knot],1.0e-12);
	}
	Check( derivativeContinuous,
		"PCHIP interpolation is derivative-continuous at every interior knot" );

	FireOpticsPreset changedDensity = FireOpticsPreset::CreateExplicitSyntheticFixture(
		0.26, 1801.0, 0.10, 0.50, 8.7, 1.2, 0.60, 0.60,
		3.298, 0.50, 0.90, 0.70 );
	FireOpticsPreset originalDensity = FireOpticsPreset::CreateExplicitSyntheticFixture(
		0.26, 1800.0, 0.10, 0.50, 8.7, 1.2, 0.60, 0.60,
		3.298, 0.50, 0.90, 0.70 );
	Check( changedDensity.IsValid() && originalDensity.IsValid() &&
		changedDensity.RecordId() != originalDensity.RecordId(),
		"changing pinned density changes the canonical record identity" );
	Check( originalDensity.RecordId() != synthetic.RecordId(),
		"legacy loose-value fixtures remain distinct from the frozen fixture record" );

	const FireFidelityEvaluation preview = predictive.EvaluateFidelity(false,true,false);
	Check( !preview.predictiveAllowed && preview.renderFidelityStatus == "preview" &&
		HasReason(preview,"requested_preview") && HasReason(preview,"producer_unqualified") &&
		HasReason(preview,"chem_none_unqualified") &&
		HasReason(preview,"condensed_organics_ir_unclosed") &&
		std::is_sorted(preview.reasonCodes.begin(),preview.reasonCodes.end()),
		"preview fidelity reasons are specific, unique, and sorted" );
	const FireFidelityEvaluation predictiveAttempt = predictive.EvaluateFidelity(true,true,true);
	Check( !predictiveAttempt.predictiveAllowed &&
		HasReason(predictiveAttempt,"missing_chem_record") &&
		HasReason(predictiveAttempt,"condensed_organics_ir_unclosed") &&
		!HasReason(predictiveAttempt,"requested_preview"),
		"predictive preflight fails for chem and nonzero condensed inventory" );

	if( failures ) {
		std::printf("%d fire optical preset test(s) failed\n",failures);
		return 1;
	}
	std::printf("All fire optical preset tests passed\n");
	return 0;
}
