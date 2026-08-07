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
		return RISECBOR64::Encode(record,bytes,&error) &&
			!rejected.LoadCanonicalRecord(bytes,&error) &&
			error.find(expectedError) != std::string::npos;
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
		"ad002cef185d055cb24e63c913e8fcf9db384cf5586a9703a5401117d4d15c87" &&
		synthetic.RecordId() ==
		"e4d939b940468df2b29b80cb4de26e51a4d47ce776cb7f67dfce33023bdcbf80",
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
		const RISECBOR64::Value* sourceEffective =
			sourceRecords->Find("effective_absorption");
		const RISECBOR64::Value* sourceEffectiveTable = sourceEffective ?
			sourceEffective->Find("table") : 0;
		if( sourceEffective && sourceEffectiveTable ) {
			const RISECBOR64::Value changedTable = ReplaceMember(*sourceEffectiveTable,
				"table_metadata",RISECBOR64::Value::MapValue({}));
			const RISECBOR64::Value changedEffective = ReplaceMember(*sourceEffective,
				"table",changedTable);
			const RISECBOR64::Value changedSources = ReplaceMember(*sourceRecords,
				"effective_absorption",changedEffective);
			Check( RejectsWith(ReplaceMember(decodedPredictive,"source_records",
				changedSources),"source table metadata is incomplete"),
				"load rejects a source table with an empty metadata envelope" );
		} else {
			Check(false,"effective-absorption source table metadata is present");
		}
		const RISECBOR64::Value* sourceHot = sourceRecords->Find("hot_soot");
		const RISECBOR64::Value* sourceHotPhi = sourceHot ?
			sourceHot->Find("phi_T_partition") : 0;
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
		if( sourceCool && sourceValues && sourceOmega ) {
			const RISECBOR64::Value changedOmega = ReplaceMember(*sourceOmega,"source",
				RISECBOR64::Value::String("distinct provenance for identity mutation"));
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
		Check( RejectsWith(ReplaceOperationalPhiBand(decodedPredictive,701.0,900.0),
			"predictive v1 phi(T) band gate failed"),
			"a lower phi(T) edge mutation reaches the pinned-band gate" );
		Check( RejectsWith(ReplaceOperationalPhiBand(decodedPredictive,700.0,899.0),
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
