//////////////////////////////////////////////////////////////////////
//
//  FireSimulationRecordsTest.cpp - Phase-C physical-record gates
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Utilities/FireSimulationRecords.h"
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

	bool NearRelative( const double actual, const double expected, const double relative )
	{
		return std::fabs(actual-expected) <= relative*std::max(1.0,std::fabs(expected));
	}

	RISECBOR64::Value ReplaceMember(
		const RISECBOR64::Value& map,
		const char* key,
		const RISECBOR64::Value& replacement
		)
	{
		RISECBOR64::Value::Members members = map.GetMap();
		for( auto& member : members ) {
			if( member.first == key ) member.second = replacement;
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

	RISECBOR64::Value ReplaceFirstThermoSegmentMember(
		const RISECBOR64::Value& record,
		const char* key,
		const RISECBOR64::Value& replacement
		)
	{
		RISECBOR64::Value::Values species = record.Find("species")->GetArray();
		RISECBOR64::Value model = *species[0].Find("cp_hs_model");
		RISECBOR64::Value::Values segments = model.Find("segments")->GetArray();
		segments[0] = ReplaceMember(segments[0],key,replacement);
		model = ReplaceMember(model,"segments",RISECBOR64::Value::ArrayValue(segments));
		species[0] = ReplaceMember(species[0],"cp_hs_model",model);
		return ReplaceMember(record,"species",RISECBOR64::Value::ArrayValue(species));
	}

	RISECBOR64::Value ReplaceFirstTransportPolicy(
		const RISECBOR64::Value& record,
		const char* policy
		)
	{
		RISECBOR64::Value::Values species = record.Find("species")->GetArray();
		RISECBOR64::Value model = *species[0].Find("viscosity_model");
		RISECBOR64::Value metadata = *model.Find("table_metadata");
		metadata = ReplaceMember(metadata,"out_of_domain_policy",RISECBOR64::Value::String(policy));
		model = ReplaceMember(model,"table_metadata",metadata);
		species[0] = ReplaceMember(species[0],"viscosity_model",model);
		return ReplaceMember(record,"species",RISECBOR64::Value::ArrayValue(species));
	}

	RISECBOR64::Value UnderstateFirstDerivativeEnclosure(
		const RISECBOR64::Value& record
		)
	{
		RISECBOR64::Value::Values species = record.Find("species")->GetArray();
		RISECBOR64::Value model = *species[0].Find("viscosity_model");
		RISECBOR64::Value interpolation = *model.Find("interpolation");
		RISECBOR64::Value::Values enclosures = interpolation.Find(
			"derivative_enclosures")->GetArray();
		RISECBOR64::Value::Values first = enclosures[0].GetArray();
		first[2] = RISECBOR64::Value::Float(0.0);
		first[3] = RISECBOR64::Value::Float(0.0);
		enclosures[0] = RISECBOR64::Value::ArrayValue(first);
		interpolation = ReplaceMember(interpolation,"derivative_enclosures",
			RISECBOR64::Value::ArrayValue(enclosures));
		model = ReplaceMember(model,"interpolation",interpolation);
		species[0] = ReplaceMember(species[0],"viscosity_model",model);
		return ReplaceMember(record,"species",RISECBOR64::Value::ArrayValue(species));
	}

	RISECBOR64::Value TwoKnotFirstTransportCurve(
		const RISECBOR64::Value& record
		)
	{
		RISECBOR64::Value::Values species = record.Find("species")->GetArray();
		RISECBOR64::Value model = *species[0].Find("viscosity_model");
		RISECBOR64::Value::Values rows = model.Find("rows")->GetArray();
		rows.resize(2);
		RISECBOR64::Value interpolation = *model.Find("interpolation");
		RISECBOR64::Value::Values slopes = interpolation.Find("slopes")->GetArray();
		RISECBOR64::Value::Values enclosures = interpolation.Find(
			"derivative_enclosures")->GetArray();
		slopes.resize(2);
		enclosures.resize(1);
		interpolation = ReplaceMember(interpolation,"slopes",
			RISECBOR64::Value::ArrayValue(slopes));
		interpolation = ReplaceMember(interpolation,"derivative_enclosures",
			RISECBOR64::Value::ArrayValue(enclosures));
		model = ReplaceMember(model,"rows",RISECBOR64::Value::ArrayValue(rows));
		model = ReplaceMember(model,"interpolation",interpolation);
		species[0] = ReplaceMember(species[0],"viscosity_model",model);
		return ReplaceMember(record,"species",RISECBOR64::Value::ArrayValue(species));
	}

	template<class Record>
	bool Rejects( const RISECBOR64::Value& value )
	{
		RISECBOR64::Bytes bytes;
		std::string error;
		Record record;
		return RISECBOR64::Encode(value,bytes,&error) &&
			!record.LoadCanonicalRecord(bytes,&error) && !error.empty();
	}

	double WilkePhi(
		const double propertyI,
		const double propertyJ,
		const double molecularWeightI,
		const double molecularWeightJ
		)
	{
		const double numerator = 1.0+std::sqrt(propertyI/propertyJ)*
			std::pow(molecularWeightJ/molecularWeightI,0.25);
		return numerator*numerator/std::sqrt(8.0*(1.0+molecularWeightI/molecularWeightJ));
	}

	double BinaryMixture(
		const double mass0, const double mass1,
		const double molecularWeight0, const double molecularWeight1,
		const double property0, const double property1,
		const double phiProperty0, const double phiProperty1
		)
	{
		const double mole0 = mass0/molecularWeight0;
		const double mole1 = mass1/molecularWeight1;
		const double x0 = mole0/(mole0+mole1);
		const double x1 = 1.0-x0;
		const double denominator0 = x0+x1*WilkePhi(
			phiProperty0,phiProperty1,molecularWeight0,molecularWeight1);
		const double denominator1 = x0*WilkePhi(
			phiProperty1,phiProperty0,molecularWeight1,molecularWeight0)+x1;
		return x0*property0/denominator0+x1*property1/denominator1;
	}
}

int main()
{
	const FireSimulationThermochemistryRecord& thermo =
		FireSimulationThermochemistryRecord::OpenSubsetV1();
	const FireSimulationTransportRecord& transport =
		FireSimulationTransportRecord::OpenV1();
	Check(thermo.IsValid(),"embedded thermochemistry record loads");
	Check(transport.IsValid(),"embedded transport record loads");
	Check(!thermo.IsPredictiveQualified(),"open thermochemistry subset remains preview-only");
	Check(!transport.IsPredictiveQualified(),"transport fit uncertainty remains preview-only");
	Check(thermo.PredictiveBlockers().size() == 9,
		"thermochemistry exposes every unresolved licensed/estimation field");
	Check(transport.PredictiveBlockers().size() == 1,
		"transport exposes its unpublished-fit uncertainty blocker");
	Check(!thermo.RecordId().empty() && !transport.RecordId().empty(),
		"canonical records have content identities");
	Check(thermo.TemperatureMinK() == 300.0 && thermo.TemperatureMaxK() == 5000.0,
		"thermochemistry common domain is frozen");
	Check(transport.TemperatureMinK() == 300.0 && transport.TemperatureMaxK() == 3000.0,
		"transport common domain is frozen");
	Check(transport.TurbulentPrandtl() == 0.7 && transport.TurbulentSchmidt() == 0.7,
		"turbulent transport constants come from the record");
	Check(transport.VremanCv() == 0.07 && transport.VremanCnu() == 0.1,
		"Vreman constants come from the record");
	Check(transport.ChemicalTimeS() == 1.0e-4 &&
		transport.CriticalFlameTemperatureK() == 1700.0,
		"reaction closure constants come from the record");

	struct CpAnchor { const char* id; double value; };
	const CpAnchor cpAnchors[] = {
		{"N2",1039.681805865883}, {"O2",918.3889267122223},
		{"CO2",845.7241586606975}, {"CH4",2229.1012890483366},
		{"C(gr)",715.3172165262429}
	};
	for( const CpAnchor& anchor : cpAnchors ) {
		double cp = 0.0, enthalpy = 1.0;
		Check(thermo.CpJPerKGK(anchor.id,300.0,cp),"cp anchor evaluates");
		Check(NearRelative(cp,anchor.value,1.0e-12),"cp anchor matches frozen NASA-9 input");
		Check(thermo.SensibleEnthalpyJPerKG(anchor.id,300.0,enthalpy),
			"reference sensible enthalpy evaluates");
		Check(std::fabs(enthalpy) <= 1.0e-8,"h_s(T_ref) is zero");
	}
	double unused = 0.0;
	Check(!thermo.CpJPerKGK("N2",299.999,unused),"cp rejects below-domain lookup");
	Check(!thermo.CpJPerKGK("N2",5000.001,unused),"cp rejects above-domain lookup");

	const std::vector<std::pair<std::string,double> > densities = {
		{"N2",0.8}, {"O2",0.2}, {"C(gr)",0.001}
	};
	for( const double expectedTemperature : {300.0,1000.0,5000.0} ) {
		double energy = 0.0, recovered = 0.0;
		Check(thermo.MixtureSensibleEnergyJPerM3(densities,expectedTemperature,energy),
			"mixture sensible energy evaluates");
		Check(thermo.InvertMixtureTemperatureK(densities,energy,recovered),
			"bracketed temperature inversion succeeds");
		Check(NearRelative(recovered,expectedTemperature,2.0e-13),
			"bracketed inversion recovers the source temperature");
	}
	double lowEnergy = 0.0, highEnergy = 0.0;
	thermo.MixtureSensibleEnergyJPerM3(densities,300.0,lowEnergy);
	thermo.MixtureSensibleEnergyJPerM3(densities,5000.0,highEnergy);
	Check(!thermo.InvertMixtureTemperatureK(densities,lowEnergy-1.0,unused),
		"temperature inversion rejects energy below its certified bracket");
	Check(!thermo.InvertMixtureTemperatureK(densities,highEnergy+1.0,unused),
		"temperature inversion rejects energy above its certified bracket");

	double n2Mu = 0.0, n2K = 0.0, o2Mu = 0.0, o2K = 0.0, waterMu = 0.0;
	Check(transport.ViscosityPaS("N2",300.0,n2Mu),"N2 viscosity evaluates");
	Check(transport.ConductivityWPerMK("N2",300.0,n2K),"N2 conductivity evaluates");
	Check(transport.ViscosityPaS("O2",300.0,o2Mu),"O2 viscosity evaluates");
	Check(transport.ConductivityWPerMK("O2",300.0,o2K),"O2 conductivity evaluates");
	Check(transport.ViscosityPaS("H2O",300.0,waterMu),
		"water-vapor transport covers the common lower bound");
	Check(NearRelative(n2Mu,1.8085469882167593e-5,1.0e-12) &&
		NearRelative(n2K,0.026450903656755405,1.0e-12),
		"transport anchors match the frozen GRI/Cantera evaluation");
	Check(!transport.ViscosityPaS("N2",299.999,unused),
		"transport rejects below-domain lookup");
	Check(!transport.ConductivityWPerMK("N2",3000.001,unused),
		"transport rejects above-domain lookup");

	const std::vector<std::pair<std::string,double> > pure = {{"N2",4.0}};
	double mixtureMu = 0.0, mixtureK = 0.0;
	Check(transport.MixtureViscosityPaS(pure,thermo,300.0,mixtureMu) &&
		NearRelative(mixtureMu,n2Mu,1.0e-14),"Wilke pure-component limit is exact");
	Check(transport.MixtureConductivityWPerMK(pure,thermo,300.0,mixtureK) &&
		NearRelative(mixtureK,n2K,1.0e-14),"WMS pure-component limit is exact");
	const std::vector<std::pair<std::string,double> > binary = {{"N2",0.7},{"O2",0.3}};
	const std::vector<std::pair<std::string,double> > permuted = {{"O2",3.0},{"N2",7.0}};
	double permutedMu = 0.0, permutedK = 0.0;
	Check(transport.MixtureViscosityPaS(binary,thermo,300.0,mixtureMu) &&
		transport.MixtureViscosityPaS(permuted,thermo,300.0,permutedMu) &&
		NearRelative(mixtureMu,permutedMu,1.0e-14),
		"Wilke mixing is permutation and scale invariant");
	Check(transport.MixtureConductivityWPerMK(binary,thermo,300.0,mixtureK) &&
		transport.MixtureConductivityWPerMK(permuted,thermo,300.0,permutedK) &&
		NearRelative(mixtureK,permutedK,1.0e-14),
		"WMS mixing is permutation and scale invariant");
	const FireThermochemistrySpecies* n2 = thermo.FindSpecies("N2");
	const FireThermochemistrySpecies* o2 = thermo.FindSpecies("O2");
	Check(n2 && o2,"transport mixture species have thermochemistry weights");
	if( n2 && o2 ) {
		const double expectedMu = BinaryMixture(0.7,0.3,n2->molecularWeightKGPerKMol,
			o2->molecularWeightKGPerKMol,n2Mu,o2Mu,n2Mu,o2Mu);
		const double expectedK = BinaryMixture(0.7,0.3,n2->molecularWeightKGPerKMol,
			o2->molecularWeightKGPerKMol,n2K,o2K,n2Mu,o2Mu);
		Check(NearRelative(mixtureMu,expectedMu,1.0e-14),
			"Wilke mixture matches an independent equation evaluation");
		Check(NearRelative(mixtureK,expectedK,1.0e-14),
			"WMS mixture matches an independent equation evaluation");
	}

	RISECBOR64::Value thermoValue, transportValue;
	std::string error;
	Check(RISECBOR64::DecodeCanonical(thermo.RecordBytes(),thermoValue,&error),
		"thermochemistry record decodes canonically");
	Check(RISECBOR64::DecodeCanonical(transport.RecordBytes(),transportValue,&error),
		"transport record decodes canonically");
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceMember(thermoValue,
		"predictive_blockers",RISECBOR64::Value::ArrayValue({}))),
		"preview record rejects an empty blocker list");
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceFirstThermoSegmentMember(
		thermoValue,"certified_cp_lower_J_per_kg_K",RISECBOR64::Value::Float(1.0e30))),
		"thermochemistry rejects a false cp certificate");
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceFirstThermoSegmentMember(
		thermoValue,"hs_offset_J_per_kg",RISECBOR64::Value::Float(1.0))),
		"thermochemistry rejects a broken h_s reference/continuity constraint");
	Check(Rejects<FireSimulationTransportRecord>(ReplaceFirstTransportPolicy(
		transportValue,"clamp")),"transport rejects a non-reject out-of-domain policy");
	Check(Rejects<FireSimulationTransportRecord>(UnderstateFirstDerivativeEnclosure(
		transportValue)),"transport rejects a false derivative enclosure");
	Check(Rejects<FireSimulationTransportRecord>(TwoKnotFirstTransportCurve(
		transportValue)),"two-knot transport input is handled without out-of-bounds access");
	RISECBOR64::Value extremeSegment = ReplaceFirstThermoSegmentMember(thermoValue,
		"temperature_min_K",RISECBOR64::Value::Float(1.0e16));
	extremeSegment = ReplaceFirstThermoSegmentMember(extremeSegment,
		"temperature_max_K",RISECBOR64::Value::Float(1.0e16+4.0));
	Check(Rejects<FireSimulationThermochemistryRecord>(extremeSegment),
		"extreme malformed segment rejects before certificate iteration");
	const std::vector<std::pair<std::string,double> > overflowingDensities = {
		{"N2",std::numeric_limits<double>::max()},
		{"O2",std::numeric_limits<double>::max()}
	};
	Check(!thermo.MixtureSensibleEnergyJPerM3(overflowingDensities,300.0,unused),
		"mixture energy rejects overflowing mass accumulation");
	FireSimulationThermochemistryRecord invalidatedThermo;
	Check(invalidatedThermo.LoadCanonicalRecord(thermo.RecordBytes()),
		"reload regression starts from a valid thermochemistry record");
	const RISECBOR64::Bytes malformedBytes(1,0xff);
	Check(!invalidatedThermo.LoadCanonicalRecord(malformedBytes) &&
		!invalidatedThermo.IsValid() && invalidatedThermo.RecordBytes().empty() &&
		!invalidatedThermo.FindSpecies("N2"),
		"failed reload clears all previously valid thermochemistry state");
	Check(!transport.MixtureViscosityPaS(binary,invalidatedThermo,300.0,unused),
		"transport rejects an invalid thermochemistry dependency");
	Check(Rejects<FireSimulationThermochemistryRecord>(RemoveMember(thermoValue,"version")),
		"thermochemistry requires an aggregate record version");
	RISECBOR64::Value referenceTemperature = *thermoValue.Find("reference_temperature_K");
	RISECBOR64::Value referenceUncertainty = *referenceTemperature.Find("uncertainty");
	referenceUncertainty = ReplaceMember(referenceUncertainty,"magnitude",
		RISECBOR64::Value::Bool(false));
	referenceTemperature = ReplaceMember(referenceTemperature,"uncertainty",referenceUncertainty);
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceMember(thermoValue,
		"reference_temperature_K",referenceTemperature)),
		"uncertainty magnitude type is fail-closed");
	FireSimulationTransportRecord invalidatedTransport;
	Check(invalidatedTransport.LoadCanonicalRecord(transport.RecordBytes()),
		"reload regression starts from a valid transport record");
	Check(!invalidatedTransport.LoadCanonicalRecord(malformedBytes) &&
		!invalidatedTransport.IsValid() && invalidatedTransport.RecordBytes().empty() &&
		!invalidatedTransport.FindSpecies("N2"),
		"failed reload clears all previously valid transport state");

	if( failures ) {
		std::printf("FireSimulationRecordsTest: %d failure(s)\n",failures);
		return 1;
	}
	std::printf("FireSimulationRecordsTest: all checks passed\n");
	return 0;
}
