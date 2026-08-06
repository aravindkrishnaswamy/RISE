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

	RISECBOR64::Value ReplaceMember(
		const RISECBOR64::Value& map,
		const char* key,
		const RISECBOR64::Value& replacement
		)
	{
		RISECBOR64::Value::Members members = map.GetMap();
		for( RISECBOR64::Value::Members::iterator member=members.begin();
			member != members.end(); ++member ) {
			if( member->first == key ) {
				member->second = replacement;
			}
		}
		return RISECBOR64::Value::MapValue(members);
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
	Check( predictive.RecordId() ==
		"c66304c80d438c2653cffcda1a11d7e53b309e8d85a5d97ee22b1b74be931f70" &&
		synthetic.RecordId() ==
		"b1f177756bbe960fd2a618ac50ff6a35519f288b50c9c1f712482b2e36b204ae",
		"the frozen v1 record IDs are pinned" );
	Check( RISECBOR64::SHA256Hex(predictive.RecordBytes()) == predictive.RecordId(),
		"the record ID hashes the exact canonical record bytes" );

	Check( predictive.SootDensityGCM3() == 1.8,
		"the pinned density is part of the loaded predictive payload" );
	Check( predictive.HotFractionMinK() == 700.0 &&
		predictive.HotFractionMaxK() == 900.0 &&
		predictive.HotFraction(700.0) == 0.0 &&
		predictive.HotFraction(800.0) == 0.5 &&
		predictive.HotFraction(900.0) == 1.0,
		"the versioned optical record owns the hot/cool temperature band" );
	Check( Near(predictive.HotFraction(750.0),0.15625,1.0e-15) &&
		Near(predictive.HotFraction(850.0),0.84375,1.0e-15),
		"the temperature partition is cubic-Hermite smoothstep" );
	const double phiStep = 1.0e-4;
	Check( Near((predictive.HotFraction(700.0+phiStep)-
		predictive.HotFraction(700.0))/phiStep,0.0,1.0e-6) &&
		Near((predictive.HotFraction(900.0)-
		predictive.HotFraction(900.0-phiStep))/phiStep,0.0,1.0e-6),
		"the cubic-Hermite temperature partition is C1 at both clamps" );
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
	Check( Near(predictive.HotAlbedo(550.0),0.10,1.0e-12) &&
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
	bool derivedEffectiveAbsorptionContinuous = true;
	for( std::size_t knot=1; knot+1<mac.Wavelengths().size(); knot++ ) {
		const double wavelength = mac.Wavelengths()[knot];
		const double epsilon = 1.0e-6;
		derivedEffectiveAbsorptionContinuous = derivedEffectiveAbsorptionContinuous &&
			Near(predictive.EffectiveAbsorptionDerivative(wavelength-epsilon),
				predictive.EffectiveAbsorptionDerivative(wavelength+epsilon),1.0e-8);
	}
	Check( derivedEffectiveAbsorptionContinuous,
		"derived E_eff interpolation is derivative-continuous at every MAC knot" );
	bool conservativeBounds = true;
	for( unsigned int sample=0; sample<=4096; sample++ ) {
		const double wavelength = 380.0+400.0*static_cast<double>(sample)/4096.0;
		conservativeBounds = conservativeBounds &&
			predictive.MaximumExtinctionMassVisible() >=
				predictive.HotExtinctionMass(wavelength) &&
			predictive.MaximumExtinctionMassVisible() >=
				predictive.CoolExtinctionMass(wavelength) &&
			predictive.MaximumExtinctionMassVisible() >=
				predictive.CondensedExtinctionMass(wavelength) &&
			predictive.MaximumHotAbsorptionMassVisible() >=
				predictive.HotAbsorptionMass(wavelength) &&
			predictive.MaximumCoolAbsorptionMassVisible() >=
				predictive.CoolExtinctionMass(wavelength)*
					(1.0-predictive.CoolAlbedo(wavelength)) &&
			predictive.MaximumCondensedAbsorptionMassVisible() >=
				predictive.CondensedExtinctionMass(wavelength)*
					(1.0-predictive.CondensedAlbedo(wavelength));
	}
	Check( conservativeBounds,
		"visible extinction and absorption bounds enclose interpolated spectra" );

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
	Check( !FireOpticsPreset::CreateExplicitSyntheticFixture(
		0.26,1800.0,1.0,0.50,8.7,1.2,0.60,0.60,
		3.298,0.50,0.90,0.70).IsValid(),
		"synthetic records outside physical optical ranges are rejected" );

	RISECBOR64::Value decodedSynthetic;
	std::string mutationError;
	Check( RISECBOR64::DecodeCanonical(synthetic.RecordBytes(),decodedSynthetic,
		&mutationError), "the synthetic record decodes for semantic mutation tests" );
	const RISECBOR64::Value* hot = decodedSynthetic.Find("hot_soot");
	const RISECBOR64::Value* hotPhi = hot ? hot->Find("phi_T_partition") : 0;
	if( hot && hotPhi ) {
		const RISECBOR64::Value changedBand = RISECBOR64::Value::ArrayValue({
			RISECBOR64::Value::Float(701.0), RISECBOR64::Value::Float(900.0) });
		const RISECBOR64::Value changedPhi = ReplaceMember(
			*hotPhi, "hot_fraction_temperature_band_K", changedBand );
		const RISECBOR64::Value changedHot = ReplaceMember(
			*hot, "phi_T_partition", changedPhi );
		const RISECBOR64::Value divergentRecord = ReplaceMember(
			decodedSynthetic, "hot_soot", changedHot );
		RISECBOR64::Bytes divergentBytes;
		FireOpticsPreset rejected;
		Check( RISECBOR64::Encode(divergentRecord,divergentBytes,&mutationError) &&
			!rejected.LoadCanonicalRecord(divergentBytes,&mutationError),
			"load rejects divergent hot/cool phi_T_partition records" );
	} else {
		Check(false,"synthetic record contains both hot-soot phi fields");
	}
	const RISECBOR64::Value* condensed = decodedSynthetic.Find("condensed_organics");
	if( condensed ) {
		const RISECBOR64::Value changedCondensed = ReplaceMember(*condensed,
			"predictive_reason_code", RISECBOR64::Value::String("arbitrary_text") );
		const RISECBOR64::Value changedRecord = ReplaceMember(decodedSynthetic,
			"condensed_organics", changedCondensed );
		RISECBOR64::Bytes changedBytes;
		FireOpticsPreset rejected;
		Check( RISECBOR64::Encode(changedRecord,changedBytes,&mutationError) &&
			!rejected.LoadCanonicalRecord(changedBytes,&mutationError),
			"load rejects a free-form condensed-organics predictive reason" );
	}

	const FireFidelityEvaluation preview = predictive.EvaluateFidelity(false,true,false);
	Check( !preview.predictiveAllowed && preview.renderFidelityStatus == "preview" &&
		HasReason(preview,"requested_preview") && HasReason(preview,"producer_unqualified") &&
		HasReason(preview,"chem_none_unqualified") &&
		HasReason(preview,"condensed_organics_ir_unclosed") &&
		HasReason(preview,"table_domain_exceeded") &&
		std::is_sorted(preview.reasonCodes.begin(),preview.reasonCodes.end()),
		"preview fidelity reasons are specific, unique, and sorted" );
	const FireFidelityEvaluation predictiveAttempt = predictive.EvaluateFidelity(true,true,true);
	Check( !predictiveAttempt.predictiveAllowed &&
		HasReason(predictiveAttempt,"missing_chem_record") &&
		HasReason(predictiveAttempt,"condensed_organics_ir_unclosed") &&
		HasReason(predictiveAttempt,"table_domain_exceeded") &&
		!HasReason(predictiveAttempt,"requested_preview"),
		"predictive preflight fails for chem and nonzero condensed inventory" );

	if( failures ) {
		std::printf("%d fire optical preset test(s) failed\n",failures);
		return 1;
	}
	std::printf("All fire optical preset tests passed\n");
	return 0;
}
