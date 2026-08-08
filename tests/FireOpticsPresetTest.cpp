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
#include <iostream>
#include <limits>
#include <string>
#include <vector>

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

	RISECBOR64::Value RemoveMember(
		const RISECBOR64::Value& map,
		const char* key
		)
	{
		RISECBOR64::Value::Members members;
		for( const auto& member : map.GetMap() ) {
			if( member.first != key ) members.push_back(member);
		}
		return RISECBOR64::Value::MapValue(members);
	}

	std::vector<double> PCHIPSlopes(
		const std::vector<double>& wavelengths,
		const std::vector<double>& values
		)
	{
		const std::size_t count = wavelengths.size();
		std::vector<double> intervals(count-1), secants(count-1), slopes(count,0.0);
		for( std::size_t i=0; i+1<count; ++i ) {
			intervals[i] = wavelengths[i+1]-wavelengths[i];
			secants[i] = (values[i+1]-values[i])/intervals[i];
		}
		for( std::size_t i=1; i+1<count; ++i ) {
			if( secants[i-1]*secants[i] > 0.0 ) {
				const double w1 = 2.0*intervals[i]+intervals[i-1];
				const double w2 = intervals[i]+2.0*intervals[i-1];
				slopes[i] = (w1+w2)/(w1/secants[i-1]+w2/secants[i]);
			}
		}
		auto endpoint = []( const double h0, const double h1,
			const double d0, const double d1 ) {
			double value = ((2.0*h0+h1)*d0-h0*d1)/(h0+h1);
			if( value*d0 <= 0.0 ) return 0.0;
			if( d0*d1 < 0.0 && std::fabs(value) > std::fabs(3.0*d0) ) {
				return 3.0*d0;
			}
			return value;
		};
		slopes.front() = endpoint(intervals[0],intervals[1],secants[0],secants[1]);
		slopes.back() = endpoint(intervals[count-2],intervals[count-3],
			secants[count-2],secants[count-3]);
		return slopes;
	}

	void DerivativeExtrema(
		const double x0, const double x1, const double y0, const double y1,
		const double slope0, const double slope1, double& minimum, double& maximum
		)
	{
		const double h = x1-x0;
		const double a = 2.0*y0-2.0*y1+h*(slope0+slope1);
		const double b = -3.0*y0+3.0*y1-h*(2.0*slope0+slope1);
		const double c = h*slope0;
		auto derivative = [a,b,c,h]( const double u ) {
			return (3.0*a*u*u+2.0*b*u+c)/h;
		};
		minimum = std::min(derivative(0.0),derivative(1.0));
		maximum = std::max(derivative(0.0),derivative(1.0));
		if( a != 0.0 ) {
			const double vertex = -b/(3.0*a);
			if( vertex > 0.0 && vertex < 1.0 ) {
				minimum = std::min(minimum,derivative(vertex));
				maximum = std::max(maximum,derivative(vertex));
			}
		}
	}

	RISECBOR64::Value InterpolationValue(
		const std::vector<double>& wavelengths,
		const std::vector<double>& values
		)
	{
		using RISECBOR64::Value;
		const std::vector<double> slopes = PCHIPSlopes(wavelengths,values);
		Value::Values encodedSlopes, encodedEnclosures;
		for( std::size_t i=0; i<slopes.size(); ++i ) {
			encodedSlopes.push_back(Value::Float(slopes[i]));
		}
		for( std::size_t i=0; i+1<wavelengths.size(); ++i ) {
			double minimum, maximum;
			DerivativeExtrema(wavelengths[i],wavelengths[i+1],values[i],values[i+1],
				slopes[i],slopes[i+1],minimum,maximum);
			encodedEnclosures.push_back(Value::ArrayValue({
				Value::Float(wavelengths[i]), Value::Float(wavelengths[i+1]),
				Value::Float(std::nextafter(minimum,-std::numeric_limits<double>::infinity())),
				Value::Float(std::nextafter(maximum,std::numeric_limits<double>::infinity()))
			}));
		}
		return Value::MapValue({
			{ "derivative_enclosures", Value::ArrayValue(encodedEnclosures) },
			{ "slopes", Value::ArrayValue(encodedSlopes) }
		});
	}

	RISECBOR64::Value WideFiniteInterpolationValue(
		const std::vector<double>& wavelengths,
		const std::vector<double>& values
		)
	{
		using RISECBOR64::Value;
		Value::Values slopes, enclosures;
		const std::vector<double> computedSlopes = PCHIPSlopes(wavelengths,values);
		for( std::size_t i=0; i<computedSlopes.size(); ++i ) {
			slopes.push_back(Value::Float(computedSlopes[i]));
			if( i+1 < wavelengths.size() ) {
				enclosures.push_back(Value::ArrayValue({
					Value::Float(wavelengths[i]), Value::Float(wavelengths[i+1]),
					Value::Float(-1.0e308), Value::Float(1.0e308)
				}));
			}
		}
		return Value::MapValue({
			{ "derivative_enclosures", Value::ArrayValue(enclosures) },
			{ "slopes", Value::ArrayValue(slopes) }
		});
	}

	RISECBOR64::Value ReplaceSpectrumColumnWithWideEnclosure(
		const RISECBOR64::Value& record,
		const char* sectionKey,
		const std::size_t valueColumn,
		const std::vector<double>& replacements,
		const char* interpolationKey
		)
	{
		using RISECBOR64::Value;
		const Value* section = record.Find(sectionKey);
		const Value* rowsValue = section ? section->Find("rows") : 0;
		if( !section || !rowsValue ||
			replacements.size() != rowsValue->GetArray().size() ) return Value();
		Value::Values rows = rowsValue->GetArray();
		std::vector<double> wavelengths, values;
		for( std::size_t i=0; i<rows.size(); ++i ) {
			Value& row = rows[i];
			Value::Values cells = row.GetArray();
			wavelengths.push_back(cells[0].GetFloat());
			values.push_back(replacements[i]);
			cells[valueColumn] = Value::Float(replacements[i]);
			row = Value::ArrayValue(cells);
		}
		Value changedSection = ReplaceMember(*section,"rows",Value::ArrayValue(rows));
		changedSection = ReplaceMember(changedSection,interpolationKey,
			WideFiniteInterpolationValue(wavelengths,values));
		return ReplaceMember(record,sectionKey,changedSection);
	}

	RISECBOR64::Value ReplaceSpectrumCell(
		const RISECBOR64::Value& record, const char* sectionKey,
		const std::size_t rowIndex, const std::size_t valueColumn,
		const double replacement, const char* interpolationKey
		)
	{
		using RISECBOR64::Value;
		const Value* section = record.Find(sectionKey);
		const Value* rowsValue = section ? section->Find("rows") : 0;
		if( !section || !rowsValue || rowIndex >= rowsValue->GetArray().size() ) {
			return Value();
		}
		Value::Values rows = rowsValue->GetArray();
		Value::Values changedRow = rows[rowIndex].GetArray();
		changedRow[valueColumn] = Value::Float(replacement);
		rows[rowIndex] = Value::ArrayValue(changedRow);
		Value changedSection = ReplaceMember(*section,"rows",Value::ArrayValue(rows));
		if( interpolationKey ) {
			std::vector<double> wavelengths, values;
			for( std::size_t i=0; i<rows.size(); ++i ) {
				wavelengths.push_back(rows[i].GetArray()[0].GetFloat());
				values.push_back(rows[i].GetArray()[valueColumn].GetFloat());
			}
			changedSection = ReplaceMember(changedSection,interpolationKey,
				InterpolationValue(wavelengths,values));
		}
		return ReplaceMember(record,sectionKey,changedSection);
	}

	RISECBOR64::Value ReplaceOperationalPhiBand(
		const RISECBOR64::Value& record,
		const double minimumK,
		const double maximumK
		)
	{
		using RISECBOR64::Value;
		Value changedRecord = record;
		const char* sectionKeys[] = { "hot_soot", "cool_carbon" };
		const Value band = Value::ArrayValue({
			Value::Float(minimumK), Value::Float(maximumK) });
		for( std::size_t i=0; i<sizeof(sectionKeys)/sizeof(sectionKeys[0]); ++i ) {
			const Value* section = changedRecord.Find(sectionKeys[i]);
			const Value* phi = section ? section->Find("phi_T_partition") : 0;
			if( !section || !phi ) {
				return Value();
			}
			const Value changedPhi = ReplaceMember(
				*phi, "hot_fraction_temperature_band_K", band );
			changedRecord = ReplaceMember(changedRecord, sectionKeys[i],
				ReplaceMember(*section, "phi_T_partition", changedPhi));
		}
		return changedRecord;
	}

	RISECBOR64::Value ReplaceSourcePhiBand(
		const RISECBOR64::Value& record,
		const double minimumK,
		const double maximumK
		)
	{
		using RISECBOR64::Value;
		const Value* sourceRecords = record.Find("source_records");
		if( !sourceRecords ) return Value();
		Value changedSources = *sourceRecords;
		const char* sourceKeys[] = { "hot_soot", "cool_carbon" };
		const Value band = Value::ArrayValue({
			Value::Float(minimumK), Value::Float(maximumK) });
		for( std::size_t i=0; i<sizeof(sourceKeys)/sizeof(sourceKeys[0]); ++i ) {
			const Value* source = changedSources.Find(sourceKeys[i]);
			const Value* phi = source ? source->Find("phi_T_partition") : 0;
			if( !source || !phi ) return Value();
			const Value changedPhi = ReplaceMember(
				*phi,"hot_fraction_temperature_band_K",band );
			changedSources = ReplaceMember(changedSources,sourceKeys[i],
				ReplaceMember(*source,"phi_T_partition",changedPhi));
		}
		return ReplaceMember(record,"source_records",changedSources);
	}

	RISECBOR64::Value ReplaceTextArrayElement(
		const RISECBOR64::Value& map,
		const char* key,
		const std::size_t index,
		const char* replacement
		)
	{
		using RISECBOR64::Value;
		const Value* array = map.Find(key);
		if( !array || array->GetType() != Value::Array ||
			index >= array->GetArray().size() ) return Value();
		Value::Values changed = array->GetArray();
		changed[index] = Value::String(replacement);
		return ReplaceMember(map,key,Value::ArrayValue(changed));
	}

	RISECBOR64::Value RemoveSyntheticFixtureEnvelopeMember(
		const RISECBOR64::Value& record,
		const char* groupKey,
		const char* valueKey,
		const char* memberKey
		)
	{
		using RISECBOR64::Value;
		const Value* sources = record.Find("source_records");
		const Value* fixtures = sources ? sources->Find("synthetic_fixtures") : 0;
		const Value* values = fixtures ? fixtures->Find("values") : 0;
		const Value* group = groupKey && values ? values->Find(groupKey) : values;
		const Value* envelope = group ? group->Find(valueKey) : 0;
		if( !sources || !fixtures || !values || !group || !envelope ) return Value();
		const Value changedGroup = ReplaceMember(*group,valueKey,
			RemoveMember(*envelope,memberKey));
		const Value changedValues = groupKey ?
			ReplaceMember(*values,groupKey,changedGroup) : changedGroup;
		const Value changedFixtures = ReplaceMember(*fixtures,"values",changedValues);
		return ReplaceMember(record,"source_records",
			ReplaceMember(*sources,"synthetic_fixtures",changedFixtures));
	}

	RISECBOR64::Value ReplaceFloatArrayElement(
		const RISECBOR64::Value& map,
		const char* key,
		const std::size_t index,
		const double replacement
		)
	{
		using RISECBOR64::Value;
		const Value* array = map.Find(key);
		if( !array || array->GetType() != Value::Array ||
			index >= array->GetArray().size() ) {
			return Value();
		}
		Value::Values changed = array->GetArray();
		changed[index] = Value::Float(replacement);
		return ReplaceMember(map,key,Value::ArrayValue(changed));
	}

	bool RejectsWith(
		const RISECBOR64::Value& record,
		const char* expectedError
		)
	{
		RISECBOR64::Bytes bytes;
		std::string error;
		FireOpticsPreset rejected;
		const bool matches = RISECBOR64::Encode(record,bytes,&error) &&
			!rejected.LoadCanonicalRecord(bytes,&error) &&
			error.find(expectedError) != std::string::npos;
		if( !matches ) {
			std::cerr << "Mutation expected '" << expectedError << "', got '"
				<< error << "'\n";
		}
		return matches;
	}

	RISECBOR64::Value MemberOrNull(
		const RISECBOR64::Value& map,
		const char* key
		)
	{
		const RISECBOR64::Value* value = map.Find(key);
		return value ? *value : RISECBOR64::Value();
	}

	RISECBOR64::Value PhiOperations( const RISECBOR64::Value& section )
	{
		const RISECBOR64::Value* phi = section.Find("phi_T_partition");
		if( !phi ) return RISECBOR64::Value();
		return RISECBOR64::Value::MapValue({
			{ "form", MemberOrNull(*phi,"form") },
			{ "hot_fraction_temperature_band_K",
				MemberOrNull(*phi,"hot_fraction_temperature_band_K") }
		});
	}

	RISECBOR64::Value PredictiveOperationalProjection(
		const RISECBOR64::Value& record
		)
	{
		using RISECBOR64::Value;
		const Value* effective = record.Find("effective_absorption");
		const Value* hot = record.Find("hot_soot");
		const Value* cool = record.Find("cool_carbon");
		const Value* condensed = record.Find("condensed_organics");
		if( !effective || !hot || !cool || !condensed ) return Value();
		return Value::MapValue({
			{ "condensed_organics", Value::MapValue({
				{ "applicability", MemberOrNull(*condensed,"applicability") },
				{ "columns", MemberOrNull(*condensed,"columns") },
				{ "domain_nm", MemberOrNull(*condensed,"domain_nm") },
				{ "extinction_angstrom_exponent_450_633",
					MemberOrNull(*condensed,"extinction_angstrom_exponent_450_633") },
				{ "g_interpolation", MemberOrNull(*condensed,"g_interpolation") },
				{ "ir_closure_status", MemberOrNull(*condensed,"ir_closure_status") },
				{ "k_ext_interpolation", MemberOrNull(*condensed,"k_ext_interpolation") },
				{ "omega_interpolation", MemberOrNull(*condensed,"omega_interpolation") },
				{ "predictive_reason_code", MemberOrNull(*condensed,"predictive_reason_code") },
				{ "rows", MemberOrNull(*condensed,"rows") }
			}) },
			{ "cool_carbon", Value::MapValue({
				{ "certified_domain_nm", MemberOrNull(*cool,"certified_domain_nm") },
				{ "g_633nm", MemberOrNull(*cool,"g_633nm") },
				{ "k_m_extinction_633nm_m2_per_g",
					MemberOrNull(*cool,"k_m_extinction_633nm_m2_per_g") },
				{ "n_spectral_exponent", MemberOrNull(*cool,"n_spectral_exponent") },
				{ "n_supported_range", MemberOrNull(*cool,"n_supported_range") },
				{ "omega_633nm", MemberOrNull(*cool,"omega_633nm") },
				{ "out_of_domain_policy", MemberOrNull(*cool,"out_of_domain_policy") },
				{ "phi_T_partition", PhiOperations(*cool) }
			}) },
			{ "effective_absorption", Value::MapValue({
				{ "columns", MemberOrNull(*effective,"columns") },
				{ "domain_nm", MemberOrNull(*effective,"domain_nm") },
				{ "mac_interpolation", MemberOrNull(*effective,"mac_interpolation") },
				{ "normative_quantity", MemberOrNull(*effective,"normative_quantity") },
				{ "pinned_density_g_cm3", MemberOrNull(*effective,"pinned_density_g_cm3") },
				{ "rows", MemberOrNull(*effective,"rows") }
			}) },
			{ "hot_soot", Value::MapValue({
				{ "columns", MemberOrNull(*hot,"columns") },
				{ "domain_nm", MemberOrNull(*hot,"domain_nm") },
				{ "g_interpolation", MemberOrNull(*hot,"g_interpolation") },
				{ "omega_interpolation", MemberOrNull(*hot,"omega_interpolation") },
				{ "phi_T_partition", PhiOperations(*hot) },
				{ "rows", MemberOrNull(*hot,"rows") }
			}) },
			{ "interpolation", MemberOrNull(record,"interpolation") },
			{ "record_class", MemberOrNull(record,"record_class") }
		});
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
		"c3999bcbaecf8a029fc57a9dd36e2c27c13801522531c60e950d03c8c61a5dfc" &&
		synthetic.RecordId() ==
		"59a3fccbc868d985465522728648cc31f2bf82ba90f71ab48b9ba1be71ebf830",
		"the frozen v1 record IDs are pinned" );
	Check( RISECBOR64::SHA256Hex(predictive.RecordBytes()) == predictive.RecordId(),
		"the record ID hashes the exact canonical record bytes" );
	RISECBOR64::Value oldOperationalRecord;
	std::string oldOperationalError;
	RISECBOR64::Bytes oldOperationalBytes;
	const bool oldOperationalDecoded = RISECBOR64::DecodeCanonical(
		predictive.RecordBytes(),oldOperationalRecord,&oldOperationalError );
	const bool oldOperationalEncoded = oldOperationalDecoded && RISECBOR64::Encode(
		PredictiveOperationalProjection(oldOperationalRecord),oldOperationalBytes,
		&oldOperationalError );
	Check( oldOperationalEncoded &&
		RISECBOR64::SHA256Hex(oldOperationalBytes) ==
			"8e68d6da455f0af89e2334d162aa05944a581753c56cf8a90f45c295dc7ad44c",
		"metadata regeneration preserves the previous canonical operational payload" );

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
	Check( predictive.CoolCarbonDomainMinNM() == 380.0 &&
		predictive.CoolCarbonDomainMaxNM() == 780.0 &&
		predictive.SupportsWavelengthRange(380.0,780.0) &&
		!predictive.SupportsWavelengthRange(379.999,780.0) &&
		!predictive.SupportsWavelengthRange(380.0,780.001),
		"cool carbon certifies the exact renderer band and rejects either overrun" );
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
	RISECBOR64::Value decodedExplicit;
	std::string explicitDecodeError;
	const bool explicitDecoded = RISECBOR64::DecodeCanonical(
		originalDensity.RecordBytes(),decodedExplicit,&explicitDecodeError );
	const RISECBOR64::Value* explicitSources = explicitDecoded ?
		decodedExplicit.Find("source_records") : 0;
	const RISECBOR64::Value* explicitSource = explicitSources ?
		explicitSources->Find("explicit_synthetic_fixtures") : 0;
	const RISECBOR64::Value* explicitValues = explicitSource ?
		explicitSource->Find("values") : 0;
	const RISECBOR64::Value* explicitDensity = explicitValues ?
		explicitValues->Find("density_g_cm3") : 0;
	Check( explicitDecoded && decodedExplicit.Find("provenance_schema") &&
		explicitDensity && explicitDensity->Find("value") &&
		explicitDensity->Find("uncertainty") && explicitDensity->Find("provenance") &&
		explicitDensity->Find("applicability"),
		"explicit synthetic records carry the complete hashed metadata contract" );
	if( explicitSources && explicitSource ) {
		const RISECBOR64::Value changedSource = ReplaceMember(*explicitSource,
			"schema_version",RISECBOR64::Value::String("unknown-version"));
		Check( RejectsWith(ReplaceMember(decodedExplicit,"source_records",
			ReplaceMember(*explicitSources,"explicit_synthetic_fixtures",changedSource)),
			"source record header does not match its frozen component"),
			"explicit synthetic source schema version is pinned" );
	}
	Check( !FireOpticsPreset::CreateExplicitSyntheticFixture(
		0.26,1800.0,1.0,0.50,8.7,1.2,0.60,0.60,
		3.298,0.50,0.90,0.70).IsValid(),
		"synthetic records outside physical optical ranges are rejected" );

	RISECBOR64::Value decodedSynthetic;
	std::string mutationError;
	Check( RISECBOR64::DecodeCanonical(synthetic.RecordBytes(),decodedSynthetic,
		&mutationError), "the synthetic record decodes for semantic mutation tests" );
	const RISECBOR64::Value* syntheticSources = decodedSynthetic.Find("source_records");
	const RISECBOR64::Value* fixtureSource = syntheticSources ?
		syntheticSources->Find("synthetic_fixtures") : 0;
	const RISECBOR64::Value* fixtureValues = fixtureSource ?
		fixtureSource->Find("values") : 0;
	const RISECBOR64::Value* fixtureEffective = fixtureValues ?
		fixtureValues->Find("E_eff_fixture") : 0;
	Check( decodedSynthetic.Find("version") && decodedSynthetic.Find("record_status") &&
		fixtureEffective && fixtureEffective->Find("value") &&
		fixtureEffective->Find("uncertainty") && fixtureEffective->Find("provenance") &&
		fixtureEffective->Find("applicability"),
		"the synthetic source carries aggregate and per-value metadata envelopes" );
	if( syntheticSources && fixtureSource ) {
		const RISECBOR64::Value changedFixture = ReplaceMember(*fixtureSource,
			"record_kind",RISECBOR64::Value::String("unknown-fixture-kind"));
		Check( RejectsWith(ReplaceMember(decodedSynthetic,"source_records",
			ReplaceMember(*syntheticSources,"synthetic_fixtures",changedFixture)),
			"source record header does not match its frozen component"),
			"frozen synthetic fixture source kind is pinned" );
	}
	struct FixtureScalarBinding
	{
		const char* group;
		const char* key;
	};
	const FixtureScalarBinding fixtureBindings[] = {
		{ 0, "E_eff_fixture" },
		{ "hot_soot", "omega" },
		{ "hot_soot", "g" },
		{ "fresh_smoke_cool_carbon", "n_exponent" },
		{ "fresh_smoke_cool_carbon", "omega" },
		{ "fresh_smoke_cool_carbon", "g" },
		{ "organic_droplets_condensed", "n_exponent" },
		{ "organic_droplets_condensed", "omega" },
		{ "organic_droplets_condensed", "g" }
	};
	for( const FixtureScalarBinding& binding : fixtureBindings ) {
		Check( RejectsWith(RemoveSyntheticFixtureEnvelopeMember(decodedSynthetic,
			binding.group,binding.key,"provenance"),
			"synthetic operational provenance is incomplete"),
			"each consumed synthetic scalar remains bound to its frozen metadata envelope" );
	}
	const RISECBOR64::Value* syntheticEffective =
		decodedSynthetic.Find("effective_absorption");
	const RISECBOR64::Value* syntheticCool = decodedSynthetic.Find("cool_carbon");
	const RISECBOR64::Value* syntheticCondensed =
		decodedSynthetic.Find("condensed_organics");
	if( syntheticEffective && syntheticCool && syntheticCondensed ) {
		Check( RejectsWith(ReplaceMember(decodedSynthetic,"effective_absorption",
			ReplaceMember(*syntheticEffective,"pinned_density_g_cm3",
				RISECBOR64::Value::Float(1.81))),
			"synthetic scalar differs from its source record"),
			"the synthetic density scalar remains bound to its predictive source envelope" );
		Check( RejectsWith(ReplaceMember(decodedSynthetic,"cool_carbon",
			ReplaceMember(*syntheticCool,"k_m_extinction_633nm_m2_per_g",
				RISECBOR64::Value::Float(8.8))),
			"synthetic scalar differs from its source record"),
			"the synthetic cool-carbon extinction remains bound to its source envelope" );
		Check( RejectsWith(ReplaceMember(decodedSynthetic,"condensed_organics",
			ReplaceMember(*syntheticCondensed,"k_m_extinction_633nm_m2_per_g",
				RISECBOR64::Value::Float(3.4))),
			"synthetic scalar differs from its source record"),
			"the synthetic condensed extinction remains bound to its source table" );
	} else {
		Check(false,"synthetic operational sections exist for scalar binding tests");
	}
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
		Check( RejectsWith(divergentRecord,
			"hot-soot and cool-carbon phi_T_partition fields differ"),
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
	RISECBOR64::Value decodedPredictive;
	Check( RISECBOR64::DecodeCanonical(predictive.RecordBytes(),decodedPredictive,
		&mutationError), "the predictive record decodes for policy mutation tests" );
	const RISECBOR64::Value* provenanceSchema = decodedPredictive.Find("provenance_schema");
	const RISECBOR64::Value* sourceRecords = decodedPredictive.Find("source_records");
	const RISECBOR64::Value* aggregateVersion = decodedPredictive.Find("version");
	const RISECBOR64::Value* aggregateStatus = decodedPredictive.Find("record_status");
	const RISECBOR64::Value* componentStatuses =
		decodedPredictive.Find("component_record_statuses");
	Check( provenanceSchema && sourceRecords && aggregateVersion && aggregateStatus &&
		componentStatuses && provenanceSchema->Find("uncertainty_kind_enum") &&
		provenanceSchema->Find("table_metadata_granularity") &&
		provenanceSchema->Find("hashed_payload_required_fields") &&
		sourceRecords->Find("effective_absorption") && sourceRecords->Find("hot_soot") &&
		sourceRecords->Find("cool_carbon") && sourceRecords->Find("condensed_organics"),
		"the canonical payload hashes aggregate metadata, schema draft-2, and every source" );
	if( provenanceSchema ) {
		Check( RejectsWith(ReplaceMember(decodedPredictive,"provenance_schema",
			ReplaceMember(*provenanceSchema,"schema_version",
				RISECBOR64::Value::String("draft-1"))),
			"canonical provenance schema is not draft-2"),
			"load rejects a pre-envelope provenance schema" );
		Check( RejectsWith(ReplaceMember(decodedPredictive,"provenance_schema",
			RemoveMember(*provenanceSchema,"hashed_payload_required_fields")),
			"does not match pinned draft-2"),
			"load rejects a schema missing its hashed-payload field contract" );
		Check( RejectsWith(ReplaceMember(decodedPredictive,"provenance_schema",
			ReplaceMember(*provenanceSchema,"table_metadata_granularity",
				RISECBOR64::Value::String("per-cell"))),
			"does not match pinned draft-2"),
			"load rejects a schema that claims per-cell metadata" );
		const RISECBOR64::Value* uncertaintyKinds =
			provenanceSchema->Find("uncertainty_kind_enum");
		if( uncertaintyKinds ) {
			Check( RejectsWith(ReplaceMember(decodedPredictive,"provenance_schema",
				ReplaceMember(*provenanceSchema,"uncertainty_kind_enum",
					ReplaceMember(*uncertaintyKinds,"synthetic_exact",
						RISECBOR64::Value::String("changed definition")))),
				"does not match pinned draft-2"),
				"load rejects a changed pinned uncertainty-kind definition" );
		}
	}
	Check( RejectsWith(RemoveMember(
		ReplaceMember(decodedPredictive,"record_name",
			RISECBOR64::Value::String("renamed-predictive-record")),
		"source_records"),"unsupported fire-optics record name/class combination"),
		"renaming a predictive record cannot bypass its provenance contract" );
	if( sourceRecords ) {
		const char* sourceKeys[] = {
			"effective_absorption", "hot_soot", "cool_carbon", "condensed_organics" };
		const char* headerKeys[] = { "schema_version", "record_kind", "record_name" };
		for( const char* sourceKey : sourceKeys ) {
			const RISECBOR64::Value* source = sourceRecords->Find(sourceKey);
			if( !source ) {
				Check(false,"predictive source record exists for header mutations");
				continue;
			}
			for( const char* headerKey : headerKeys ) {
				const RISECBOR64::Value changedSource = ReplaceMember(*source,headerKey,
					RISECBOR64::Value::String("unknown-v1-header"));
				const RISECBOR64::Value changedSources = ReplaceMember(
					*sourceRecords,sourceKey,changedSource);
				const std::string message = std::string("load pins ")+sourceKey+
					" source "+headerKey;
				Check( RejectsWith(ReplaceMember(decodedPredictive,"source_records",
					changedSources),"source record header does not match its frozen component"),
					message.c_str() );
			}
		}
		const RISECBOR64::Value* sourceEffective =
			sourceRecords->Find("effective_absorption");
		const RISECBOR64::Value* sourceEffectiveTable = sourceEffective ?
			sourceEffective->Find("table") : 0;
		if( sourceEffective && sourceEffectiveTable ) {
			Check( RejectsWith(ReplaceMember(decodedPredictive,"source_records",
				ReplaceMember(*sourceRecords,"effective_absorption",
					ReplaceMember(*sourceEffective,"normative_quantity",
						RISECBOR64::Value::String("E_eff_lambda")))),
				"operational effective absorption differs from its source record"),
				"the effective source normative quantity remains bound to the operation" );
			Check( RejectsWith(ReplaceMember(decodedPredictive,"source_records",
				ReplaceMember(*sourceRecords,"effective_absorption",
					RemoveMember(*sourceEffective,"quantity_semantics"))),
				"quantity_semantics"),
				"the effective source requires its record-level quantity semantics" );
			const RISECBOR64::Value* sourceEffectiveMetadata =
				sourceEffectiveTable->Find("table_metadata");
			const RISECBOR64::Value* operationalEffective =
				decodedPredictive.Find("effective_absorption");
			const RISECBOR64::Value* operationalEffectiveMetadata =
				operationalEffective ? operationalEffective->Find("table_metadata") : 0;
			const RISECBOR64::Value changedTable = ReplaceMember(*sourceEffectiveTable,
				"table_metadata",RISECBOR64::Value::MapValue({}));
			const RISECBOR64::Value changedEffective = ReplaceMember(*sourceEffective,
				"table",changedTable);
			const RISECBOR64::Value changedSources = ReplaceMember(*sourceRecords,
				"effective_absorption",changedEffective);
			Check( RejectsWith(ReplaceMember(decodedPredictive,"source_records",
				changedSources),"source table metadata is incomplete"),
				"load rejects a source table with an empty metadata envelope" );
			if( sourceEffectiveMetadata && operationalEffective &&
				operationalEffectiveMetadata ) {
				const RISECBOR64::Value changedMetadata = ReplaceMember(
					*operationalEffectiveMetadata,"provenance",
					RISECBOR64::Value::String("coordinated metadata identity change"));
				Check( RejectsWith(ReplaceMember(decodedPredictive,
					"effective_absorption",ReplaceMember(*operationalEffective,
						"table_metadata",changedMetadata)),
					"operational effective-absorption table metadata differs"),
					"operational effective-absorption metadata cannot detach from its source" );
				const RISECBOR64::Value changedSourceTable = ReplaceMember(
					*sourceEffectiveTable,"table_metadata",changedMetadata);
				const RISECBOR64::Value coordinated = ReplaceMember(
					ReplaceMember(decodedPredictive,"source_records",
						ReplaceMember(*sourceRecords,"effective_absorption",
							ReplaceMember(*sourceEffective,"table",changedSourceTable))),
					"effective_absorption",ReplaceMember(*operationalEffective,
						"table_metadata",changedMetadata));
				RISECBOR64::Bytes coordinatedBytes;
				FireOpticsPreset coordinatedPreset;
				Check( RISECBOR64::Encode(coordinated,coordinatedBytes,&mutationError) &&
					coordinatedPreset.LoadCanonicalRecord(coordinatedBytes,&mutationError) &&
					coordinatedPreset.RecordId() != predictive.RecordId() &&
					HasReason(coordinatedPreset.EvaluateFidelity(false,false,false),
						"qualified_record_override"),
					"a coordinated complete table-metadata change remains loadable with a new ID" );
			} else {
				Check(false,"effective table metadata is present on source and operation");
			}
			const RISECBOR64::Value changedColumns = ReplaceTextArrayElement(
				*sourceEffectiveTable,"columns",1,"E_eff_unit_unspecified");
			const RISECBOR64::Value changedColumnEffective = ReplaceMember(
				*sourceEffective,"table",changedColumns);
			Check( RejectsWith(ReplaceMember(decodedPredictive,"source_records",
				ReplaceMember(*sourceRecords,"effective_absorption",
					changedColumnEffective)),
				"source effective-absorption table columns do not match"),
				"load rejects changed source effective-absorption column semantics" );
			const RISECBOR64::Value* sourceDefinition =
				sourceEffective->Find("definition");
			const RISECBOR64::Value* sourceDensity = sourceDefinition ?
				sourceDefinition->Find("pinned_density_g_cm3") : 0;
			if( sourceDefinition && sourceDensity ) {
				const char* envelopeMembers[] = { "provenance", "uncertainty" };
				for( const char* member : envelopeMembers ) {
					const RISECBOR64::Value changedDefinition = ReplaceMember(
						*sourceDefinition,"pinned_density_g_cm3",
						RemoveMember(*sourceDensity,member));
					const RISECBOR64::Value changedEffectiveEnvelope = ReplaceMember(
						*sourceEffective,"definition",changedDefinition);
					Check( RejectsWith(ReplaceMember(decodedPredictive,"source_records",
						ReplaceMember(*sourceRecords,"effective_absorption",
							changedEffectiveEnvelope)),
						"source scalar metadata envelope is incomplete"),
						"the pinned-density source requires provenance and uncertainty metadata" );
				}
				const RISECBOR64::Value* uncertainty = sourceDensity->Find("uncertainty");
				if( uncertainty ) {
					const RISECBOR64::Value wrongKind = ReplaceMember(*uncertainty,"kind",
						RISECBOR64::Value::String("expanded_95"));
					const RISECBOR64::Value malformedMagnitude = ReplaceMember(*uncertainty,
						"magnitude",RISECBOR64::Value::String("unknown"));
					for( const RISECBOR64::Value& changedUncertainty :
						{ wrongKind, malformedMagnitude } ) {
						const RISECBOR64::Value changedDefinition = ReplaceMember(
							*sourceDefinition,"pinned_density_g_cm3",
							ReplaceMember(*sourceDensity,"uncertainty",changedUncertainty));
						const RISECBOR64::Value changedEffectiveEnvelope = ReplaceMember(
							*sourceEffective,"definition",changedDefinition);
						Check( RejectsWith(ReplaceMember(decodedPredictive,"source_records",
							ReplaceMember(*sourceRecords,"effective_absorption",
								changedEffectiveEnvelope)),
							changedUncertainty.Find("kind") &&
								changedUncertainty.Find("kind")->GetText() == "expanded_95" ?
								"uncertainty kind does not match" :
								"uncertainty magnitude is malformed"),
							"the pinned-density uncertainty kind and shape are frozen" );
					}
				} else {
					Check(false,"effective-absorption density uncertainty is present");
				}
			} else {
				Check(false,"effective-absorption density envelope is present");
			}
		} else {
			Check(false,"effective-absorption source table metadata is present");
		}
		const RISECBOR64::Value* sourceHot = sourceRecords->Find("hot_soot");
		const RISECBOR64::Value* sourceHotPhi = sourceHot ?
			sourceHot->Find("phi_T_partition") : 0;
		const RISECBOR64::Value* sourceHotComputed = sourceHot ?
			sourceHot->Find("computed_outputs") : 0;
		const RISECBOR64::Value* sourceHotTable = sourceHotComputed ?
			sourceHotComputed->Find("spectral_young_dp30_N50") : 0;
		if( sourceHot && sourceHotComputed && sourceHotTable ) {
			const RISECBOR64::Value changedTable = ReplaceTextArrayElement(
				*sourceHotTable,"columns",2,"g_unitless_unpinned");
			const RISECBOR64::Value changedComputed = ReplaceMember(*sourceHotComputed,
				"spectral_young_dp30_N50",changedTable);
			const RISECBOR64::Value changedHot = ReplaceMember(*sourceHot,
				"computed_outputs",changedComputed);
			Check( RejectsWith(ReplaceMember(decodedPredictive,"source_records",
				ReplaceMember(*sourceRecords,"hot_soot",changedHot)),
				"source hot-soot table columns do not match"),
				"load rejects changed source hot-soot column semantics" );
			const RISECBOR64::Value* sourceHotMetadata = sourceHotComputed->Find(
				"spectral_young_dp30_N50_metadata");
			const RISECBOR64::Value* operationalHot = decodedPredictive.Find("hot_soot");
			const RISECBOR64::Value* operationalHotMetadata = operationalHot ?
				operationalHot->Find("table_metadata") : 0;
			if( sourceHotMetadata && operationalHot && operationalHotMetadata ) {
				const RISECBOR64::Value changedMetadata = ReplaceMember(
					*operationalHotMetadata,"provenance",
					RISECBOR64::Value::String("detached hot table provenance"));
				Check( RejectsWith(ReplaceMember(decodedPredictive,"hot_soot",
					ReplaceMember(*operationalHot,"table_metadata",changedMetadata)),
					"operational hot-soot table metadata differs"),
					"operational hot-soot metadata cannot detach from its source" );
				const RISECBOR64::Value changedAdoption = ReplaceMember(
					*operationalHotMetadata,"adoption_ruling",RISECBOR64::Value::MapValue({}));
				Check( RejectsWith(ReplaceMember(decodedPredictive,"hot_soot",
					ReplaceMember(*operationalHot,"table_metadata",changedAdoption)),
					"operational hot-soot table metadata differs"),
					"hot-soot adopted-550 metadata remains source-bound" );
			} else {
				Check(false,"hot-soot table metadata is present on source and operation");
			}
		} else {
			Check(false,"hot-soot source table is present");
		}
		if( sourceHot && sourceHotPhi ) {
			const RISECBOR64::Value changedSourcePhi = ReplaceMember(*sourceHotPhi,
				"hot_fraction_temperature_band_K",
				RISECBOR64::Value::ArrayValue({
					RISECBOR64::Value::Unsigned(701),
					RISECBOR64::Value::Unsigned(900) }));
			const RISECBOR64::Value changedSourceHot = ReplaceMember(*sourceHot,
				"phi_T_partition",changedSourcePhi);
			const RISECBOR64::Value changedSources = ReplaceMember(*sourceRecords,
				"hot_soot",changedSourceHot);
			Check( RejectsWith(ReplaceMember(decodedPredictive,"source_records",
				changedSources),"source phi_T_partition fields differ"),
				"load rejects source-record-only phi_T divergence" );
		} else {
			Check(false,"hot-soot source phi_T metadata is present");
		}
		const RISECBOR64::Value* sourceCool = sourceRecords->Find("cool_carbon");
		const RISECBOR64::Value* sourceValues = sourceCool ? sourceCool->Find("values") : 0;
		const RISECBOR64::Value* sourceOmega = sourceValues ?
			sourceValues->Find("omega_633nm") : 0;
		const RISECBOR64::Value* sourceOmegaProvenance = sourceOmega ?
			sourceOmega->Find("provenance") : 0;
		if( sourceCool && sourceValues ) {
			const char* coolEnvelopeKeys[] = {
				"k_m_extinction_633nm_m2_per_g", "MAC_absorption_550nm_m2_per_g",
				"density_g_cm3", "omega_633nm",
				"g_asymmetry", "n_spectral_exponent" };
			const char* envelopeMembers[] = { "provenance", "uncertainty" };
			for( const char* key : coolEnvelopeKeys ) {
				const RISECBOR64::Value* envelope = sourceValues->Find(key);
				if( !envelope ) {
					Check(false,"cool-carbon scalar envelope is present");
					continue;
				}
				for( const char* member : envelopeMembers ) {
					const RISECBOR64::Value changedValues = ReplaceMember(*sourceValues,
						key,RemoveMember(*envelope,member));
					const RISECBOR64::Value changedCool = ReplaceMember(*sourceCool,
						"values",changedValues);
					Check( RejectsWith(ReplaceMember(decodedPredictive,"source_records",
						ReplaceMember(*sourceRecords,"cool_carbon",changedCool)),
						"source scalar metadata envelope is incomplete"),
						"each cool-carbon source scalar requires provenance and uncertainty" );
				}
				const RISECBOR64::Value* uncertainty = envelope->Find("uncertainty");
				if( !uncertainty ) {
					Check(false,"cool-carbon scalar uncertainty is present");
					continue;
				}
				const RISECBOR64::Value wrongKind = ReplaceMember(*uncertainty,"kind",
					RISECBOR64::Value::String("synthetic_exact"));
				const RISECBOR64::Value malformedMagnitude = ReplaceMember(*uncertainty,
					"magnitude",RISECBOR64::Value::String("unknown"));
				const RISECBOR64::Value mutations[] = { wrongKind, malformedMagnitude };
				const char* errors[] = {
					"uncertainty kind does not match", "uncertainty magnitude is malformed" };
				for( std::size_t i=0; i<2; ++i ) {
					const RISECBOR64::Value changedValues = ReplaceMember(*sourceValues,key,
						ReplaceMember(*envelope,"uncertainty",mutations[i]));
					const RISECBOR64::Value changedCool = ReplaceMember(*sourceCool,
						"values",changedValues);
					Check( RejectsWith(ReplaceMember(decodedPredictive,"source_records",
						ReplaceMember(*sourceRecords,"cool_carbon",changedCool)),errors[i]),
						"each cool-carbon uncertainty kind and magnitude shape are frozen" );
				}
			}
		} else {
			Check(false,"cool-carbon source scalar envelopes are present");
		}
		if( sourceCool && sourceValues && sourceOmega && sourceOmegaProvenance ) {
			const RISECBOR64::Value changedOmega = ReplaceMember(*sourceOmega,
				"provenance",ReplaceMember(*sourceOmegaProvenance,"access",
					RISECBOR64::Value::String(
						"distinct access provenance for identity mutation")));
			const RISECBOR64::Value changedValues = ReplaceMember(*sourceValues,
				"omega_633nm",changedOmega);
			const RISECBOR64::Value changedCool = ReplaceMember(*sourceCool,
				"values",changedValues);
			const RISECBOR64::Value changedSources = ReplaceMember(*sourceRecords,
				"cool_carbon",changedCool);
			const RISECBOR64::Value changedRecord = ReplaceMember(decodedPredictive,
				"source_records",changedSources);
			RISECBOR64::Bytes changedBytes;
			FireOpticsPreset changedProvenance;
			Check( RISECBOR64::Encode(changedRecord,changedBytes,&mutationError) &&
				changedProvenance.LoadCanonicalRecord(changedBytes,&mutationError) &&
				changedProvenance.RecordId() != predictive.RecordId(),
				"a provenance-only change changes the canonical record identity" );
		} else {
			Check(false,"cool-carbon per-value provenance is present in the hashed payload");
		}
		const RISECBOR64::Value* sourceCondensed =
			sourceRecords->Find("condensed_organics");
		const RISECBOR64::Value* sourceCondensedComputed = sourceCondensed ?
			sourceCondensed->Find("computed_outputs_full_mie") : 0;
		const RISECBOR64::Value* sourceCondensedTable = sourceCondensedComputed ?
			sourceCondensedComputed->Find("table") : 0;
		if( sourceCondensed && sourceCondensedComputed && sourceCondensedTable ) {
			const RISECBOR64::Value changedTable = ReplaceTextArrayElement(
				*sourceCondensedTable,"columns",1,"k_ext_units_missing");
			const RISECBOR64::Value changedComputed = ReplaceMember(
				*sourceCondensedComputed,"table",changedTable);
			const RISECBOR64::Value changedCondensed = ReplaceMember(
				*sourceCondensed,"computed_outputs_full_mie",changedComputed);
			Check( RejectsWith(ReplaceMember(decodedPredictive,"source_records",
				ReplaceMember(*sourceRecords,"condensed_organics",changedCondensed)),
				"source condensed-organics table columns do not match"),
				"load rejects changed source condensed-organics column semantics" );
			const RISECBOR64::Value* sourceCondensedMetadata =
				sourceCondensedComputed->Find("table_metadata");
			const RISECBOR64::Value* operationalCondensed =
				decodedPredictive.Find("condensed_organics");
			const RISECBOR64::Value* operationalCondensedMetadata =
				operationalCondensed ? operationalCondensed->Find("table_metadata") : 0;
			if( operationalCondensed ) {
				const RISECBOR64::Value changedComputedExponent = ReplaceMember(
					*sourceCondensedComputed,"extinction_angstrom_exponent_450_633",
					RISECBOR64::Value::Float(1.79));
				const RISECBOR64::Value changedSource = ReplaceMember(*sourceCondensed,
					"computed_outputs_full_mie",changedComputedExponent);
				const RISECBOR64::Value changedOperational = ReplaceMember(
					*operationalCondensed,"extinction_angstrom_exponent_450_633",
					RISECBOR64::Value::Float(1.79));
				const RISECBOR64::Value changedRecord = ReplaceMember(
					ReplaceMember(decodedPredictive,"source_records",
						ReplaceMember(*sourceRecords,"condensed_organics",changedSource)),
					"condensed_organics",changedOperational);
				Check( RejectsWith(changedRecord,
					"extinction exponent disagrees with its table"),
					"coordinated condensed exponent mutation remains table-bound" );

				const std::vector<double> hugeValues = {
					1.0e308, 1.0e-308, 1.0e308, 1.0e-308, 1.0e308 };
				RISECBOR64::Value hugeRecord = ReplaceSpectrumColumnWithWideEnclosure(
					decodedPredictive,"condensed_organics",1,hugeValues,
					"k_ext_interpolation");
				const RISECBOR64::Value* hugeOperational =
					hugeRecord.Find("condensed_organics");
				const RISECBOR64::Value* hugeRows = hugeOperational ?
					hugeOperational->Find("rows") : 0;
				if( hugeOperational && hugeRows ) {
					const RISECBOR64::Value hugeOperationalCopy = *hugeOperational;
					const RISECBOR64::Value hugeSourceTable = ReplaceMember(
						*sourceCondensedTable,"rows",*hugeRows);
					RISECBOR64::Value hugeSourceComputed = ReplaceMember(
						*sourceCondensedComputed,"table",hugeSourceTable);
					hugeSourceComputed = ReplaceMember(hugeSourceComputed,
						"extinction_angstrom_exponent_450_633",
						RISECBOR64::Value::Float(0.0));
					const RISECBOR64::Value hugeSource = ReplaceMember(*sourceCondensed,
						"computed_outputs_full_mie",hugeSourceComputed);
					hugeRecord = ReplaceMember(hugeRecord,"source_records",
						ReplaceMember(*sourceRecords,"condensed_organics",hugeSource));
					hugeRecord = ReplaceMember(hugeRecord,"condensed_organics",
						ReplaceMember(hugeOperationalCopy,
							"extinction_angstrom_exponent_450_633",
							RISECBOR64::Value::Float(0.0)));
					Check( RejectsWith(hugeRecord,
						"derivative enclosure computation is non-finite"),
						"finite huge knots cannot bypass derivative-enclosure validation" );
				} else {
					Check(false,"huge-knot derivative mutation fixture is constructible");
				}
			}
			if( sourceCondensedMetadata && operationalCondensed &&
				operationalCondensedMetadata ) {
				const RISECBOR64::Value changedMetadata = ReplaceMember(
					*operationalCondensedMetadata,"provenance",
					RISECBOR64::Value::String("detached condensed table provenance"));
				Check( RejectsWith(ReplaceMember(decodedPredictive,"condensed_organics",
					ReplaceMember(*operationalCondensed,"table_metadata",changedMetadata)),
					"operational condensed-organics table metadata differs"),
					"operational condensed-organics metadata cannot detach from its source" );
			} else {
				Check(false,"condensed table metadata is present on source and operation");
			}
			const RISECBOR64::Value* mieInputs = sourceCondensed->Find("mie_inputs");
			const RISECBOR64::Value* brownCarbonAAE = mieInputs ?
				mieInputs->Find("brown_carbon_AAE") : 0;
			if( mieInputs && brownCarbonAAE ) {
				const char* envelopeMembers[] = { "provenance", "uncertainty" };
				for( const char* member : envelopeMembers ) {
					const RISECBOR64::Value changedInputs = ReplaceMember(*mieInputs,
						"brown_carbon_AAE",RemoveMember(*brownCarbonAAE,member));
					const RISECBOR64::Value changedSource = ReplaceMember(*sourceCondensed,
						"mie_inputs",changedInputs);
					Check( RejectsWith(ReplaceMember(decodedPredictive,"source_records",
						ReplaceMember(*sourceRecords,"condensed_organics",changedSource)),
						"source scalar metadata envelope is incomplete"),
						"brown-carbon AAE requires provenance and uncertainty metadata" );
				}
				const RISECBOR64::Value* uncertainty = brownCarbonAAE->Find("uncertainty");
				if( uncertainty ) {
					const RISECBOR64::Value mutations[] = {
						ReplaceMember(*uncertainty,"kind",
							RISECBOR64::Value::String("measured_1sigma")),
						ReplaceMember(*uncertainty,"magnitude",
							RISECBOR64::Value::String("unknown")) };
					const char* errors[] = {
						"uncertainty kind does not match", "uncertainty magnitude is malformed" };
					for( std::size_t i=0; i<2; ++i ) {
						const RISECBOR64::Value changedInputs = ReplaceMember(*mieInputs,
							"brown_carbon_AAE",ReplaceMember(*brownCarbonAAE,
								"uncertainty",mutations[i]));
						const RISECBOR64::Value changedSource = ReplaceMember(*sourceCondensed,
							"mie_inputs",changedInputs);
						Check( RejectsWith(ReplaceMember(decodedPredictive,"source_records",
							ReplaceMember(*sourceRecords,"condensed_organics",changedSource)),
							errors[i]),
							"brown-carbon AAE uncertainty kind and shape are frozen" );
					}
				} else {
					Check(false,"brown-carbon AAE uncertainty is present");
				}
			} else {
				Check(false,"brown-carbon AAE source envelope is present");
			}
		} else {
			Check(false,"condensed-organics source table is present");
		}
	}
	const RISECBOR64::Value* cool = decodedPredictive.Find("cool_carbon");
	if( cool ) {
		const RISECBOR64::Value changedCool = ReplaceMember(*cool,
			"out_of_domain_policy", RISECBOR64::Value::String("extrapolate") );
		const RISECBOR64::Value changedRecord = ReplaceMember(decodedPredictive,
			"cool_carbon", changedCool );
		RISECBOR64::Bytes changedBytes;
		FireOpticsPreset rejected;
		Check( RISECBOR64::Encode(changedRecord,changedBytes,&mutationError) &&
			!rejected.LoadCanonicalRecord(changedBytes,&mutationError),
			"load rejects a cool-carbon policy that permits extrapolation" );
	}

	const RISECBOR64::Value* predictiveEffective =
		decodedPredictive.Find("effective_absorption");
	const RISECBOR64::Value* predictiveHot = decodedPredictive.Find("hot_soot");
	const RISECBOR64::Value* predictiveCondensed =
		decodedPredictive.Find("condensed_organics");
	if( predictiveEffective && predictiveHot && cool && predictiveCondensed ) {
		Check( RejectsWith(ReplaceMember(decodedPredictive,"hot_soot",
			ReplaceFloatArrayElement(*predictiveHot,"domain_nm",0,381.0)),
			"hot-soot certified domain differs"),
			"a hot-soot certified-domain lower-bound mutation is rejected" );
		Check( RejectsWith(ReplaceMember(decodedPredictive,"hot_soot",
			ReplaceFloatArrayElement(*predictiveHot,"domain_nm",1,779.0)),
			"hot-soot certified domain differs"),
			"a hot-soot certified-domain upper-bound mutation is rejected" );
		Check( RejectsWith(ReplaceMember(decodedPredictive,"condensed_organics",
			ReplaceFloatArrayElement(*predictiveCondensed,"domain_nm",0,381.0)),
			"condensed-organics certified domain differs"),
			"a condensed-organics certified-domain lower-bound mutation is rejected" );
		Check( RejectsWith(ReplaceMember(decodedPredictive,"condensed_organics",
			ReplaceFloatArrayElement(*predictiveCondensed,"domain_nm",1,779.0)),
			"condensed-organics certified domain differs"),
			"a condensed-organics certified-domain upper-bound mutation is rejected" );
		Check( RejectsWith(ReplaceMember(decodedPredictive,"effective_absorption",
			ReplaceTextArrayElement(*predictiveEffective,"columns",2,"MAC_kg_per_m2")),
			"operational effective-absorption table columns do not match"),
			"load rejects changed operational effective-absorption column semantics" );
		Check( RejectsWith(ReplaceMember(decodedPredictive,"hot_soot",
			ReplaceTextArrayElement(*predictiveHot,"columns",1,"albedo")),
			"operational hot-soot table columns do not match"),
			"load rejects changed operational hot-soot column semantics" );
		Check( RejectsWith(ReplaceMember(decodedPredictive,"condensed_organics",
			ReplaceTextArrayElement(*predictiveCondensed,"columns",3,"phase_g")),
			"operational condensed-organics table columns do not match"),
			"load rejects changed operational condensed-organics column semantics" );
		Check( RejectsWith(ReplaceSourcePhiBand(decodedPredictive,701.0,900.0),
			"operational phi_T_partition differs from its source record"),
			"a coordinated source-only phi(T) mutation cannot detach operations from provenance" );
		Check( RejectsWith(ReplaceSourcePhiBand(
			ReplaceOperationalPhiBand(decodedPredictive,701.0,900.0),701.0,900.0),
			"predictive v1 phi(T) band gate failed"),
			"a lower phi(T) edge mutation reaches the pinned-band gate" );
		Check( RejectsWith(ReplaceSourcePhiBand(
			ReplaceOperationalPhiBand(decodedPredictive,700.0,899.0),700.0,899.0),
			"predictive v1 phi(T) band gate failed"),
			"an upper phi(T) edge mutation reaches the pinned-band gate" );
		Check( RejectsWith(ReplaceMember(decodedPredictive,"effective_absorption",
			ReplaceMember(*predictiveEffective,"pinned_density_g_cm3",
				RISECBOR64::Value::Float(1.81))),"density gate failed"),
			"a density mutation reaches the specific load-time density gate" );
		const RISECBOR64::Value* effectiveRows = predictiveEffective->Find("rows");
		if( effectiveRows ) {
			const double mac550 = effectiveRows->GetArray()[34].GetArray()[2].GetFloat();
			Check( RejectsWith(ReplaceSpectrumCell(decodedPredictive,
				"effective_absorption",34,2,mac550+0.02,"mac_interpolation"),
				"MAC(550 nm) gate failed"),
				"a MAC-550 mutation reaches its specific load-time gate" );
			const double mac630 = effectiveRows->GetArray()[50].GetArray()[2].GetFloat();
			Check( RejectsWith(ReplaceSpectrumCell(decodedPredictive,
				"effective_absorption",50,2,mac630+0.10,"mac_interpolation"),
				"MAC(632.8 nm) gate failed"),
				"a MAC-632.8 mutation reaches its specific load-time gate" );
			const double effective380 =
				effectiveRows->GetArray().front().GetArray()[1].GetFloat();
			Check( RejectsWith(ReplaceSpectrumCell(decodedPredictive,
				"effective_absorption",0,1,effective380*1.01,0),
				"E_eff shape gate failed"),
				"an E_eff endpoint mutation reaches the specific shape gate" );
			const double interiorEffective =
				effectiveRows->GetArray()[20].GetArray()[1].GetFloat();
			Check( RejectsWith(ReplaceSpectrumCell(decodedPredictive,
				"effective_absorption",20,1,interiorEffective+0.001,0),
				"E_eff row disagrees with normative MAC"),
				"an interior E_eff mutation reaches the normative-MAC consistency gate" );
			const double sourceBoundE =
				effectiveRows->GetArray()[20].GetArray()[1].GetFloat();
			const double sourceBoundMAC =
				effectiveRows->GetArray()[20].GetArray()[2].GetFloat();
			RISECBOR64::Value changedEffectiveRecord = ReplaceSpectrumCell(
				decodedPredictive,"effective_absorption",20,1,sourceBoundE*1.001,0);
			changedEffectiveRecord = ReplaceSpectrumCell(changedEffectiveRecord,
				"effective_absorption",20,2,sourceBoundMAC*1.001,"mac_interpolation");
			Check( RejectsWith(changedEffectiveRecord,
				"operational effective absorption differs from its source record"),
				"coherent non-anchor E/MAC drift cannot detach operations from provenance" );
			const double mac380 =
				effectiveRows->GetArray().front().GetArray()[2].GetFloat();
			Check( RejectsWith(ReplaceSpectrumCell(decodedPredictive,
				"effective_absorption",0,2,mac380*1.01,"mac_interpolation"),
				"visible AAE gate failed"),
				"a MAC endpoint mutation reaches the specific visible-AAE gate" );
		}
		const RISECBOR64::Value* macInterpolation =
			predictiveEffective->Find("mac_interpolation");
		if( macInterpolation ) {
			const RISECBOR64::Value* slopes = macInterpolation->Find("slopes");
			const RISECBOR64::Value* enclosures =
				macInterpolation->Find("derivative_enclosures");
			if( slopes && enclosures ) {
				RISECBOR64::Value::Values changedSlopes = slopes->GetArray();
				changedSlopes[10] = RISECBOR64::Value::Float(
					changedSlopes[10].GetFloat()+0.01);
				const RISECBOR64::Value changedInterpolation = ReplaceMember(
					*macInterpolation,"slopes",
					RISECBOR64::Value::ArrayValue(changedSlopes));
				Check( RejectsWith(ReplaceMember(decodedPredictive,"effective_absorption",
					ReplaceMember(*predictiveEffective,"mac_interpolation",
						changedInterpolation)),"PCHIP slope does not match"),
					"a slope mutation reaches the specific PCHIP load-time gate" );

				RISECBOR64::Value::Values changedEnclosures = enclosures->GetArray();
				RISECBOR64::Value::Values firstEnclosure =
					changedEnclosures.front().GetArray();
				firstEnclosure[3] = firstEnclosure[2];
				changedEnclosures.front() =
					RISECBOR64::Value::ArrayValue(firstEnclosure);
				const RISECBOR64::Value narrowInterpolation = ReplaceMember(
					*macInterpolation,"derivative_enclosures",
					RISECBOR64::Value::ArrayValue(changedEnclosures));
				Check( RejectsWith(ReplaceMember(decodedPredictive,"effective_absorption",
					ReplaceMember(*predictiveEffective,"mac_interpolation",
						narrowInterpolation)),"derivative is outside its enclosure"),
					"a one-bound mutation reaches the derivative-enclosure gate" );
			}
		}
		const RISECBOR64::Value* hotRows = predictiveHot->Find("rows");
		if( hotRows ) {
			const double hotOmega550 = hotRows->GetArray()[2].GetArray()[1].GetFloat();
			Check( RejectsWith(ReplaceSpectrumCell(decodedPredictive,"hot_soot",2,1,
				hotOmega550+0.01,"omega_interpolation"),"hot-soot gate failed"),
				"a hot-soot omega mutation reaches its specific load-time gate" );
			const double hotG550 = hotRows->GetArray()[2].GetArray()[2].GetFloat();
			Check( RejectsWith(ReplaceSpectrumCell(decodedPredictive,"hot_soot",2,2,
				hotG550+0.01,"g_interpolation"),"hot-soot gate failed"),
				"a hot-soot anchor mutation reaches its specific load-time gate" );
			const double hotNonAnchor = hotRows->GetArray()[1].GetArray()[1].GetFloat();
			Check( RejectsWith(ReplaceSpectrumCell(decodedPredictive,"hot_soot",1,1,
				hotNonAnchor+0.001,"omega_interpolation"),
				"operational hot-soot table differs from its source record"),
				"a non-anchor hot-soot mutation cannot detach operations from provenance" );
		}
		Check( RejectsWith(ReplaceMember(decodedPredictive,"cool_carbon",
			ReplaceMember(*cool,"n_spectral_exponent",RISECBOR64::Value::Float(1.1))),
			"operational cool-carbon values differ from their source record"),
			"an in-range cool exponent mutation cannot detach operations from provenance" );
		Check( RejectsWith(ReplaceMember(decodedPredictive,"cool_carbon",
			ReplaceMember(*cool,"k_m_extinction_633nm_m2_per_g",
				RISECBOR64::Value::Float(8.8))),"cool-carbon gate failed"),
			"a cool-carbon extinction mutation reaches its specific load-time gate" );
		Check( RejectsWith(ReplaceMember(decodedPredictive,"cool_carbon",
			ReplaceMember(*cool,"omega_633nm",RISECBOR64::Value::Float(0.26))),
			"cool-carbon gate failed"),
			"a cool-carbon omega mutation reaches its specific load-time gate" );
		Check( RejectsWith(ReplaceMember(decodedPredictive,"cool_carbon",
			ReplaceMember(*cool,"g_633nm",RISECBOR64::Value::Float(0.59))),
			"cool-carbon gate failed"),
			"a cool-carbon anchor mutation reaches its specific load-time gate" );
		Check( RejectsWith(ReplaceMember(decodedPredictive,"cool_carbon",
			ReplaceFloatArrayElement(*cool,"n_supported_range",0,0.9)),
			"cool-carbon exponent range is not frozen v1"),
			"a cool-carbon exponent-range lower bound mutation is rejected" );
		Check( RejectsWith(ReplaceMember(decodedPredictive,"cool_carbon",
			ReplaceFloatArrayElement(*cool,"n_supported_range",1,1.3)),
			"cool-carbon exponent range is not frozen v1"),
			"a cool-carbon exponent-range upper bound mutation is rejected" );
		Check( RejectsWith(ReplaceMember(decodedPredictive,"cool_carbon",
			ReplaceFloatArrayElement(*cool,"certified_domain_nm",0,381.0)),
			"cool-carbon certified domain is not frozen v1"),
			"a cool-carbon certified-domain lower bound mutation is rejected" );
		Check( RejectsWith(ReplaceMember(decodedPredictive,"cool_carbon",
			ReplaceFloatArrayElement(*cool,"certified_domain_nm",1,779.0)),
			"cool-carbon certified domain is not frozen v1"),
			"a cool-carbon certified-domain upper bound mutation is rejected" );
		Check( RejectsWith(ReplaceMember(decodedPredictive,"condensed_organics",
			ReplaceMember(*predictiveCondensed,"ir_closure_status",
				RISECBOR64::Value::String("open"))),"condensed-organics gate failed"),
			"a condensed-organics status mutation reaches its specific load-time gate" );
		const RISECBOR64::Value* condensedRows = predictiveCondensed->Find("rows");
		if( condensedRows ) {
			const double condensedNonAnchor =
				condensedRows->GetArray()[1].GetArray()[1].GetFloat();
			Check( RejectsWith(ReplaceSpectrumCell(decodedPredictive,
				"condensed_organics",1,1,condensedNonAnchor+0.001,
				"k_ext_interpolation"),
				"operational condensed-organics values differ from their source record"),
				"a non-anchor condensed mutation cannot detach operations from provenance" );
		}
	} else {
		Check(false,"predictive record sections exist for numeric mutation tests");
	}

	const FireFidelityEvaluation preview = predictive.EvaluateFidelity(false,true,false);
	Check( !preview.predictiveAllowed && preview.renderFidelityStatus == "preview" &&
		HasReason(preview,"requested_preview") && HasReason(preview,"producer_unqualified") &&
		HasReason(preview,"chem_none_unqualified") &&
		HasReason(preview,"condensed_organics_ir_unclosed") &&
		!HasReason(preview,"qualified_record_override") &&
		!HasReason(preview,"table_domain_exceeded") &&
		std::is_sorted(preview.reasonCodes.begin(),preview.reasonCodes.end()) &&
		std::adjacent_find(preview.reasonCodes.begin(),preview.reasonCodes.end()) ==
			preview.reasonCodes.end(),
		"preview fidelity reasons are specific, unique, and sorted" );
	const FireFidelityEvaluation predictiveAttempt = predictive.EvaluateFidelity(true,true,true);
	Check( !predictiveAttempt.predictiveAllowed &&
		HasReason(predictiveAttempt,"missing_chem_record") &&
		HasReason(predictiveAttempt,"condensed_organics_ir_unclosed") &&
		!HasReason(predictiveAttempt,"table_domain_exceeded") &&
		!HasReason(predictiveAttempt,"requested_preview"),
		"predictive preflight fails for chem and nonzero condensed inventory" );

	if( failures ) {
		std::printf("%d fire optical preset test(s) failed\n",failures);
		return 1;
	}
	std::printf("All fire optical preset tests passed\n");
	return 0;
}
