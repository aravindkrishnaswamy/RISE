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

	RISECBOR64::Value ReplaceEnvelopeValue(
		const RISECBOR64::Value& record,
		const char* key,
		const char* value
		)
	{
		RISECBOR64::Value envelope = *record.Find(key);
		envelope = ReplaceMember(envelope,"value",RISECBOR64::Value::String(value));
		return ReplaceMember(record,key,envelope);
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

	RISECBOR64::Value ReplaceThermoSpeciesMember(
		const RISECBOR64::Value& record,
		const char* speciesId,
		const char* key,
		const RISECBOR64::Value& replacement
		)
	{
		RISECBOR64::Value::Values species = record.Find("species")->GetArray();
		for( auto& entry : species ) {
			const RISECBOR64::Value* id = entry.Find("species_id");
			if( id && id->GetText() == speciesId ) {
				entry = ReplaceMember(entry,key,replacement);
			}
		}
		return ReplaceMember(record,"species",RISECBOR64::Value::ArrayValue(species));
	}

	RISECBOR64::Value HugeFiniteThermoDomain( const RISECBOR64::Value& record )
	{
		RISECBOR64::Value::Values species = record.Find("species")->GetArray();
		for( auto& entry : species ) {
			const RISECBOR64::Value* id = entry.Find("species_id");
			if( !id || id->GetText() != "N2" ) continue;
			const double molecularWeight = 28.0134;
			RISECBOR64::Value model = *entry.Find("cp_hs_model");
			RISECBOR64::Value segment = model.Find("segments")->GetArray()[0];
			RISECBOR64::Value::Values coefficients(9,RISECBOR64::Value::Float(0.0));
			coefficients[2] = RISECBOR64::Value::Float(1.0);
			segment = ReplaceMember(segment,"temperature_min_K",RISECBOR64::Value::Float(200.0));
			segment = ReplaceMember(segment,"temperature_max_K",RISECBOR64::Value::Float(1.0e307));
			segment = ReplaceMember(segment,"coefficients",
				RISECBOR64::Value::ArrayValue(coefficients));
			segment = ReplaceMember(segment,"hs_offset_J_per_kg",RISECBOR64::Value::Float(
				-8314.46261815324*300.0/molecularWeight));
			segment = ReplaceMember(segment,"certified_cp_lower_J_per_kg_K",
				RISECBOR64::Value::Float(1.0));
			model = ReplaceMember(model,"temperature_domain_K",
				RISECBOR64::Value::ArrayValue({RISECBOR64::Value::Float(200.0),
					RISECBOR64::Value::Float(1.0e307)}));
			model = ReplaceMember(model,"segments",
				RISECBOR64::Value::ArrayValue({segment}));
			entry = ReplaceMember(entry,"cp_hs_model",model);
		}
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

	template<class Record>
	std::string RejectionError( const RISECBOR64::Value& value )
	{
		RISECBOR64::Bytes bytes;
		std::string error;
		Record record;
		if( !RISECBOR64::Encode(value,bytes,&error) ) return error;
		if( record.LoadCanonicalRecord(bytes,&error) ) return std::string();
		return error;
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
	const FireSimulationMethaneRecord& methane =
		FireSimulationMethaneRecord::PhysicalV1();
	const FireSimulationTransportRecord& transport =
		FireSimulationTransportRecord::OpenV1();
	const FireSimulationGasOpacityRecord& opacity =
		FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1();
	Check(thermo.IsValid(),"embedded thermochemistry record loads");
	Check(methane.IsValid(),"complete physical methane record loads");
	Check(transport.IsValid(),"embedded transport record loads");
	Check(opacity.IsValid(),"embedded HITEMP Planck-mean record loads");
	Check(!thermo.IsPredictiveQualified(),"open thermochemistry subset remains preview-only");
	Check(!methane.IsPredictiveQualified(),
		"physical methane substrate retains only its explicit predictive-label blockers");
	Check(methane.SpeciesOrder() == std::vector<std::string>({
		"CH4","O2","N2","CO2","H2O","CO","C(gr)"}),
		"methane record owns the exact r51 species order");
	Check(methane.ElementOrder() == std::vector<std::string>({"C","H","O","N"}),
		"methane record carries every conserved element");
	Check(methane.ConservativeReconstruction().declaredRank == 3 &&
		methane.ConservativeReconstruction().nullity == 5 &&
		methane.NonadvectiveFluxProjection().declaredRank == 4 &&
		methane.NonadvectiveFluxProjection().nullity == 4,
		"real methane r56 independent-row A and C carry their exact ranks");
	const FireAcceptedStateFeasibilityEnvelope& feasibility=
		methane.AcceptedStateFeasibilityEnvelope();
	Check(feasibility.derivedUnionFactorEpsilon64==2384.0&&
		feasibility.kappaEpsilon64==4096.0&&
		feasibility.remapFactorEpsilon32==256.0&&
		feasibility.composedForceFactorEpsilon32==64.0&&
		feasibility.projectionFactorEpsilon32==256.0&&
		feasibility.derivedUnionFactorEpsilon32==832.0&&
		feasibility.kappaEpsilon32==1024.0&&
		feasibility.limiterOutwardFactorEpsilon64+
		feasibility.rowAccumulationFactorEpsilon64+
		feasibility.nullspaceProjectionFactorEpsilon64+
		feasibility.sourcePacketFactorEpsilon64+
		feasibility.ledgerReductionFactorEpsilon64==
			feasibility.derivedUnionFactorEpsilon64,
		"methane record carries r60 plus the derived resident binary32 precision class");
	std::vector<double> physicalReactionDelta(1,0.0);
	physicalReactionDelta.insert(physicalReactionDelta.end(),
		methane.PrimaryReactionDelta().begin(),methane.PrimaryReactionDelta().end());
	std::vector<double> projectedReactionDelta;
	double projectionDisplacement = 0.0;
	Check(methane.ConservativeReconstruction().Project(
		physicalReactionDelta,projectedReactionDelta) &&
		projectedReactionDelta.size() == physicalReactionDelta.size(),
		"r56 physical reaction direction projects through the certified kernel");
	if( projectedReactionDelta.size() == physicalReactionDelta.size() ) {
		for( std::size_t index=0; index<physicalReactionDelta.size(); ++index ) {
			projectionDisplacement = std::max(projectionDisplacement,
				std::fabs(projectedReactionDelta[index]-physicalReactionDelta[index]));
		}
	}
	Check(projectionDisplacement <= 8.0*std::numeric_limits<double>::epsilon(),
		"r56 physical reaction projection is a rounding-level no-op, never a repair");
	Check(std::fabs(methane.LowerHeatingValueJPerKG()-50027364.88044851) < 0.1 &&
		std::fabs(methane.StoichiometricOxygenKGPerKGFuel()-3.989263492008084) < 1.0e-12,
		"methane LHV and oxygen coefficient derive from the pinned CEA formation data");
	double defaultRadiativeFraction = 0.0, overriddenRadiativeFraction = 0.0;
	double referencedSootDensity = 0.0;
	Check(methane.PilotTemperatureK() == 600.0 &&
		methane.AutoignitionTemperatureK() == 810.4 &&
		methane.SootOxidationTemperatureK() == 1300.0 &&
		methane.SootYieldKGPerKGFuel() == 0.0,
		"methane exposes the r52 operational constants by their distinct taxonomy");
	Check(methane.ResolveRadiativeFraction(0,false,0.0,defaultRadiativeFraction) &&
		defaultRadiativeFraction == 0.2 &&
		methane.ResolveRadiativeFraction(
			"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
			true,0.14,overriddenRadiativeFraction) && overriddenRadiativeFraction == 0.14 &&
		!methane.ResolveRadiativeFraction("not-a-case-record",true,0.14,
			overriddenRadiativeFraction),
		"radiative fraction defaults from fuel and overrides only through a hashed case record");
	Check(methane.ResolveSootDensityKGPerM3(FireOpticsPreset::PredictiveV1(),
		referencedSootDensity) && referencedSootDensity == 1800.0 &&
		!methane.ResolveSootDensityKGPerM3(FireOpticsPreset::SyntheticRegressionV1(),
			referencedSootDensity),
		"methane soot density resolves only through the adopted optics record identity");
	std::vector<double> arbitraryFlux = {0.13,-0.22,0.31,-0.17,0.19,-0.07,0.11,-0.09};
	std::vector<double> projectedFlux;
	Check(methane.NonadvectiveFluxProjection().Project(arbitraryFlux,projectedFlux),
		"certified methane N_C projects an arbitrary raw flux");
	if( projectedFlux.size() == 8 ) {
		const FireCertifiedNullspace& closure = methane.NonadvectiveFluxProjection();
		for( std::size_t row=0; row<closure.constraintRows; ++row ) {
			double residual = 0.0;
			for( std::size_t column=0; column<closure.stateDimension; ++column ) {
				residual += closure.constraintMatrix[row*closure.stateDimension+column]*
					projectedFlux[column];
			}
			Check(std::fabs(residual) <= 2.0e-14,
				"projected methane flux satisfies every A plus sum-J row");
		}
	}
	RISECBOR64::Value nearRankFixture, wrongSubspaceFixture;
	std::string fixtureError;
	const RISECBOR64::Bytes& nearRankBytes =
		FireSimulationSolverFixtureRecords::NearRankDeficientV1();
	const RISECBOR64::Bytes& wrongSubspaceBytes =
		FireSimulationSolverFixtureRecords::CorrectRankWrongSubspaceV1();
	Check(RISECBOR64::DecodeCanonical(nearRankBytes,nearRankFixture,&fixtureError) &&
		RISECBOR64::DecodeCanonical(wrongSubspaceBytes,wrongSubspaceFixture,&fixtureError),
		"synthetic solver RED records decode canonically");
	Check(nearRankFixture.Find("record_class") &&
		nearRankFixture.Find("record_class")->GetText() == "synthetic_verification_fixture" &&
		wrongSubspaceFixture.Find("record_class") &&
		wrongSubspaceFixture.Find("record_class")->GetText() == "synthetic_verification_fixture" &&
		RISECBOR64::SHA256Hex(nearRankBytes) != RISECBOR64::SHA256Hex(wrongSubspaceBytes),
		"contrived rank and wrong-subspace fixtures have distinct non-preset identities");
	FireSimulationMethaneRecord fixtureAsPreset;
	Check(!fixtureAsPreset.LoadCanonicalRecord(nearRankBytes) &&
		!fixtureAsPreset.LoadCanonicalRecord(wrongSubspaceBytes),
		"synthetic solver fixtures can never load as the physical methane preset");
	std::string nearRankRejection, wrongSubspaceRejection;
	Check(FireSimulationSolverFixtureRecords::RejectsCandidateCertificate(
		nearRankBytes,&nearRankRejection) &&
		FireSimulationSolverFixtureRecords::RejectsCandidateCertificate(
			wrongSubspaceBytes,&wrongSubspaceRejection),
		"synthetic RED candidates reach and fail the shared production certificate validator");
	double methaneCp = 0.0, methaneHs = 0.0, methaneRecoveredTemperature = 0.0;
	const std::vector<std::pair<std::string,double> > methaneMixture = {
		{"CH4",0.1},{"O2",0.2},{"N2",0.7}
	};
	const double orderedMethaneMixture[7]={0.1,0.2,0.7,0.0,0.0,0.0,0.0};
	double orderedMethaneCp[7]={};
	double methaneEnergy = 0.0,orderedMethaneEnergy=0.0,orderedMethaneTemperature=0.0;
	Check(methane.CpJPerKGK("CH4",1000.0,methaneCp) && methaneCp > 0.0 &&
		methane.CpBySpeciesOrderJPerKGK(1000.0,orderedMethaneCp,7) &&
		orderedMethaneCp[0]==methaneCp &&
		methane.SensibleEnthalpyJPerKG("CO2",1000.0,methaneHs) && methaneHs > 0.0 &&
		methane.MixtureSensibleEnergyJPerM3(methaneMixture,1000.0,methaneEnergy) &&
		methane.InvertMixtureTemperatureK(methaneMixture,methaneEnergy,
			methaneRecoveredTemperature) &&
		methane.MixtureSensibleEnergyBySpeciesOrderJPerM3(orderedMethaneMixture,7,
			1000.0,orderedMethaneEnergy) && orderedMethaneEnergy==methaneEnergy &&
		methane.InvertMixtureTemperatureBySpeciesOrderK(orderedMethaneMixture,7,
			orderedMethaneEnergy,orderedMethaneTemperature) &&
		orderedMethaneTemperature==methaneRecoveredTemperature &&
		std::fabs(methaneRecoveredTemperature-1000.0) < 1.0e-9,
		"the physical methane record's ordered and named cp, h_s and inversion paths agree bitwise");
	double namedMethaneMu=0.0,namedMethaneK=0.0,orderedMethaneMu=0.0,orderedMethaneK=0.0;
	Check(transport.MixtureViscosityPaS(methaneMixture,methane,1000.0,namedMethaneMu)&&
		transport.MixtureConductivityWPerMK(methaneMixture,methane,1000.0,namedMethaneK)&&
		transport.MixturePropertiesBySpeciesOrder(orderedMethaneMixture,6,methane,1000.0,
			orderedMethaneMu,orderedMethaneK)&&orderedMethaneMu==namedMethaneMu&&
		orderedMethaneK==namedMethaneK,
		"the methane transport record's ordered Wilke/WMS path is bitwise identical");
	Check(!transport.IsPredictiveQualified(),"transport fit uncertainty remains preview-only");
	Check(thermo.PredictiveBlockers().size() == 7,
		"thermochemistry exposes every unresolved licensed/estimation field");
	Check(transport.PredictiveBlockers().size() == 1,
		"transport exposes its unpublished-fit uncertainty blocker");
	Check(!thermo.RecordId().empty() && !transport.RecordId().empty(),
		"canonical records have content identities");
	Check(thermo.TemperatureMinK() == 300.0 && thermo.TemperatureMaxK() == 5000.0,
		"thermochemistry common domain is frozen");
	Check(transport.TemperatureMinK() == 300.0 && transport.TemperatureMaxK() == 3000.0,
		"transport common domain is frozen");
	Check(opacity.TemperatureMinK() == 300.0 && opacity.TemperatureMaxK() == 2500.0,
		"Planck-mean record exposes its certified continuous temperature domain");
	Check(opacity.FindSpecies("H2O") && opacity.FindSpecies("CO2") &&
		!opacity.FindSpecies("N2"),"Planck-mean record has the exact adopted species inventory");
	double opacityValue = 0.0, opacityGasDerivative = 0.0;
	double opacityRadiationDerivative = 0.0;
	Check(opacity.PlanckMeanCrossSectionM2PerMolecule("H2O",300.0,300.0,
		opacityValue,opacityGasDerivative,opacityRadiationDerivative) &&
		std::fabs(opacityValue-2.1374710395e-24) <= 2.0e-12*2.1374710395e-24,
		"H2O Planck-mean anchor matches the committed full HITEMP table");
	Check(opacity.PlanckMeanCrossSectionM2PerMolecule("CO2",300.0,300.0,
		opacityValue,opacityGasDerivative,opacityRadiationDerivative) &&
		std::fabs(opacityValue-1.0756050218e-24) <= 2.0e-12*1.0756050218e-24,
		"CO2 Planck-mean anchor matches the committed full HITEMP table");
	const double interiorGasTemperature = 923.25;
	const double interiorRadiationTemperature = 1476.75;
	Check(opacity.PlanckMeanCrossSectionM2PerMolecule("H2O",interiorGasTemperature,
		interiorRadiationTemperature,opacityValue,opacityGasDerivative,
		opacityRadiationDerivative),
		"Planck-mean record evaluates arbitrary interior solver temperatures");
	const double derivativeStepK = 1.0e-2;
	double plusValue = 0.0, minusValue = 0.0, derivativeScratch0 = 0.0;
	double derivativeScratch1 = 0.0;
	opacity.PlanckMeanCrossSectionM2PerMolecule("H2O",
		interiorGasTemperature+derivativeStepK,interiorRadiationTemperature,
		plusValue,derivativeScratch0,derivativeScratch1);
	opacity.PlanckMeanCrossSectionM2PerMolecule("H2O",
		interiorGasTemperature-derivativeStepK,interiorRadiationTemperature,
		minusValue,derivativeScratch0,derivativeScratch1);
	const double differencedGasDerivative =
		(plusValue-minusValue)/(2.0*derivativeStepK);
	Check(std::fabs(opacityGasDerivative-differencedGasDerivative) <=
		2.0e-8*std::max(std::fabs(opacityGasDerivative),1.0e-300),
		"analytic gas-temperature partial matches an independent central difference");
	opacity.PlanckMeanCrossSectionM2PerMolecule("H2O",interiorGasTemperature,
		interiorRadiationTemperature+derivativeStepK,plusValue,
		derivativeScratch0,derivativeScratch1);
	opacity.PlanckMeanCrossSectionM2PerMolecule("H2O",interiorGasTemperature,
		interiorRadiationTemperature-derivativeStepK,minusValue,
		derivativeScratch0,derivativeScratch1);
	const double differencedRadiationDerivative =
		(plusValue-minusValue)/(2.0*derivativeStepK);
	Check(std::fabs(opacityRadiationDerivative-differencedRadiationDerivative) <=
		2.0e-8*std::max(std::fabs(opacityRadiationDerivative),1.0e-300),
		"analytic radiation-temperature partial matches an independent central difference");
	double gasDerivativeMinimum = 0.0, gasDerivativeMaximum = 0.0;
	double radiationDerivativeMinimum = 0.0, radiationDerivativeMaximum = 0.0;
	Check(opacity.PlanckMeanDerivativeEnclosure("H2O",900.0,950.0,1450.0,1500.0,
		gasDerivativeMinimum,gasDerivativeMaximum,radiationDerivativeMinimum,
		radiationDerivativeMaximum) &&
		opacityGasDerivative >= gasDerivativeMinimum &&
		opacityGasDerivative <= gasDerivativeMaximum &&
		opacityRadiationDerivative >= radiationDerivativeMinimum &&
		opacityRadiationDerivative <= radiationDerivativeMaximum,
		"certified derivative enclosure contains the interior analytic partials");
	Check(!opacity.PlanckMeanCrossSectionM2PerMolecule("H2O",299.999,300.0,
		opacityValue,opacityGasDerivative,opacityRadiationDerivative) &&
		!opacity.PlanckMeanCrossSectionM2PerMolecule("CO2",2500.001,2500.0,
		opacityValue,opacityGasDerivative,opacityRadiationDerivative),
		"Planck-mean lookup rejects both sides of its certified domain");
	Check(transport.TurbulentPrandtl() == 0.7 && transport.TurbulentSchmidt() == 0.7,
		"turbulent transport constants come from the record");
	Check(transport.VremanCv() == 0.07 && transport.VremanCnu() == 0.1,
		"Vreman constants come from the record");
	Check(transport.ChemicalTimeS() == 1.0e-4 &&
		transport.CriticalFlameTemperatureK() == 1700.0,
		"reaction closure constants come from the record");
	const double zeroGradient[3][3] = {};
	const double directionalWidths[3] = {0.5,0.25,0.125};
	double eddyViscosity = -1.0;
	Check(transport.VremanEddyViscosityM2PerS(
		zeroGradient,directionalWidths,eddyViscosity) && eddyViscosity == 0.0,
		"Vreman zero-gradient rule returns zero without division");
	const double directionalGradient[3][3] = {
		{1.0,0.0,0.0},
		{0.0,2.0,0.0},
		{0.0,0.0,0.0}
	};
	Check(transport.VremanEddyViscosityM2PerS(
		directionalGradient,directionalWidths,eddyViscosity) &&
		NearRelative(eddyViscosity,0.07*std::sqrt(0.0625/5.0),1.0e-14),
		"directional linear gradient matches an independent Vreman evaluation");
	double molecularD = 0.0, sgsD = 0.0, totalD = 0.0;
	double effectiveMu = 0.0, effectiveK = 0.0;
	Check(transport.EffectiveTransport(1.8e-5,0.03,1.2,1000.0,eddyViscosity,false,
		molecularD,sgsD,totalD,effectiveMu,effectiveK) &&
		NearRelative(molecularD,0.03/1200.0,1.0e-14) &&
		NearRelative(sgsD,eddyViscosity/0.7,1.0e-14) &&
		NearRelative(totalD,molecularD+sgsD,1.0e-14) &&
		NearRelative(effectiveMu,1.8e-5+1.2*eddyViscosity,1.0e-14) &&
		NearRelative(effectiveK,0.03+1200.0*eddyViscosity/0.7,1.0e-14),
		"LES effective transport follows the record-owned relationships");
	Check(transport.EffectiveTransport(1.8e-5,0.03,1.2,1000.0,eddyViscosity,true,
		molecularD,sgsD,totalD,effectiveMu,effectiveK) && sgsD == 0.0 &&
		NearRelative(totalD,0.03/1200.0,1.0e-14) && effectiveMu == 1.8e-5 &&
		effectiveK == 0.03,
		"DNS retains molecular transport and zeros SGS transport");
	const FireThermochemistrySpecies* pentacosane = thermo.FindSpecies(
		"C25H52,n-pentacosane");
	Check(pentacosane &&
		pentacosane->molecularWeightKGPerKMol == 352.68038 &&
		pentacosane->formationEnthalpyJPerKMol == -560548000.0,
		"pentacosane uses the frozen C4-C8 CH2-increment extrapolation");
	Check(!thermo.FindSpecies("C6H10O5,levoglucosan"),
		"owner-gated levoglucosan vapor has no placeholder species values");
	double pentacosaneCp = 0.0, unused = 0.0;
	Check(thermo.CpJPerKGK("C25H52,n-pentacosane",200.0,pentacosaneCp) &&
		NearRelative(pentacosaneCp,1234.0330224466995,1.0e-12),
		"pentacosane low-temperature extension matches the frozen fit");
	Check(thermo.CpJPerKGK("C25H52,n-pentacosane",5000.0,pentacosaneCp) &&
		NearRelative(pentacosaneCp,4146.221423899239,1.0e-12),
		"pentacosane high-temperature endpoint matches the frozen fit");
	Check(!thermo.CpJPerKGK("C25H52,n-pentacosane",199.999,unused) &&
		!thermo.CpJPerKGK("C25H52,n-pentacosane",5000.001,unused),
		"pentacosane rejects both sides of its certified domain");
	Check(!thermo.CpJPerKGK("C6H10O5,levoglucosan",300.0,unused),
		"owner-gated levoglucosan vapor fails closed at lookup");
	const std::vector<std::pair<std::string,double> > waxOnly = {
		{"C25H52,n-pentacosane",1.0}
	};
	for( const double expectedTemperature : {200.0,250.0,5000.0} ) {
		double energy = 0.0, recovered = 0.0;
		Check(thermo.MixtureSensibleEnergyJPerM3(
			waxOnly,expectedTemperature,energy) &&
			thermo.InvertMixtureTemperatureK(waxOnly,energy,recovered) &&
			NearRelative(recovered,expectedTemperature,2.0e-13),
			"pentacosane dynamic-domain energy inversion round trips");
	}

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
	Check(thermo.CpJPerKGK("N2",200.0,unused),
		"NASA species retains its certified 200 K lower endpoint");
	Check(!thermo.CpJPerKGK("N2",199.999,unused),"cp rejects below-domain lookup");
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

	RISECBOR64::Value thermoValue, methaneValue, transportValue, opacityRecordValue;
	std::string error;
	Check(RISECBOR64::DecodeCanonical(thermo.RecordBytes(),thermoValue,&error),
		"thermochemistry record decodes canonically");
	Check(RISECBOR64::DecodeCanonical(methane.RecordBytes(),methaneValue,&error),
		"physical methane record decodes canonically");
	Check(RISECBOR64::DecodeCanonical(transport.RecordBytes(),transportValue,&error),
		"transport record decodes canonically");
	Check(RISECBOR64::DecodeCanonical(opacity.RecordBytes(),opacityRecordValue,&error),
		"Planck-mean record decodes canonically");
	RISECBOR64::Value encodedPentacosane;
	for( const auto& species : thermoValue.Find("species")->GetArray() ) {
		if( species.Find("species_id") &&
			species.Find("species_id")->GetText() == "C25H52,n-pentacosane" ) {
			encodedPentacosane = species;
		}
	}
	const RISECBOR64::Value* pentacosaneCertificate =
		encodedPentacosane.Find("assumption_bound_certificate");
	Check(pentacosaneCertificate &&
		pentacosaneCertificate->Find("source_species") &&
		pentacosaneCertificate->Find("source_species")->GetArray().size() == 5 &&
		pentacosaneCertificate->Find("maximum_cp_magnitude_J_per_kg_K") &&
		pentacosaneCertificate->Find("maximum_cp_magnitude_J_per_kg_K")->GetFloat() > 0.0,
		"pentacosane record carries its nonzero C4-C8 residual certificate");
	if( pentacosaneCertificate ) {
		RISECBOR64::Value mutatedCertificate = ReplaceMember(*pentacosaneCertificate,
			"added_CH2",RISECBOR64::Value::Float(16.0));
		Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceThermoSpeciesMember(
			thermoValue,"C25H52,n-pentacosane","assumption_bound_certificate",
			mutatedCertificate)),
			"pentacosane rejects a mutated CH2 extrapolation certificate");
		mutatedCertificate = RemoveMember(*pentacosaneCertificate,"corroboration_only");
		Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceThermoSpeciesMember(
			thermoValue,"C25H52,n-pentacosane","assumption_bound_certificate",
			mutatedCertificate)),
			"pentacosane requires the corroboration-only audit marker");
	}
	const RISECBOR64::Value pentacosaneMW = *encodedPentacosane.Find(
		"molecular_weight_kg_per_kmol");
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceThermoSpeciesMember(
		thermoValue,"C25H52,n-pentacosane","molecular_weight_kg_per_kmol",
		ReplaceMember(pentacosaneMW,"value",RISECBOR64::Value::Float(352.0)))),
		"pentacosane operational molecular weight is bound to its derivation");
	RISECBOR64::Value pentacosaneMWUncertainty = *pentacosaneMW.Find("uncertainty");
	pentacosaneMWUncertainty = ReplaceMember(pentacosaneMWUncertainty,"magnitude",
		RISECBOR64::Value::Float(0.0));
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceThermoSpeciesMember(
		thermoValue,"C25H52,n-pentacosane","molecular_weight_kg_per_kmol",
		ReplaceMember(pentacosaneMW,"uncertainty",pentacosaneMWUncertainty))),
		"pentacosane rejects a zeroed operational uncertainty magnitude");
	pentacosaneMWUncertainty = ReplaceMember(*pentacosaneMW.Find("uncertainty"),"basis",
		RISECBOR64::Value::String("mutated basis"));
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceThermoSpeciesMember(
		thermoValue,"C25H52,n-pentacosane","molecular_weight_kg_per_kmol",
		ReplaceMember(pentacosaneMW,"uncertainty",pentacosaneMWUncertainty))),
		"pentacosane binds its operational uncertainty basis");
	RISECBOR64::Value pentacosaneMWProvenance = *pentacosaneMW.Find("provenance");
	pentacosaneMWProvenance = ReplaceMember(pentacosaneMWProvenance,"citation",
		RISECBOR64::Value::String("mutated citation"));
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceThermoSpeciesMember(
		thermoValue,"C25H52,n-pentacosane","molecular_weight_kg_per_kmol",
		ReplaceMember(pentacosaneMW,"provenance",pentacosaneMWProvenance))),
		"pentacosane binds its operational CEA provenance");
	const RISECBOR64::Value pentacosaneFormation = *encodedPentacosane.Find(
		"formation_enthalpy_J_per_kmol_298p15K");
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceThermoSpeciesMember(
		thermoValue,"C25H52,n-pentacosane","formation_enthalpy_J_per_kmol_298p15K",
		ReplaceMember(pentacosaneFormation,"value",RISECBOR64::Value::Float(-1.0)))),
		"pentacosane formation enthalpy is bound to its derivation");
	RISECBOR64::Value pentacosaneModel = *encodedPentacosane.Find("cp_hs_model");
	RISECBOR64::Value pentacosaneMetadata = *pentacosaneModel.Find("table_metadata");
	RISECBOR64::Value pentacosaneModelUncertainty = *pentacosaneMetadata.Find("uncertainty");
	pentacosaneModelUncertainty = ReplaceMember(pentacosaneModelUncertainty,"magnitude",
		RISECBOR64::Value::Float(0.0));
	pentacosaneMetadata = ReplaceMember(pentacosaneMetadata,"uncertainty",
		pentacosaneModelUncertainty);
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceThermoSpeciesMember(
		thermoValue,"C25H52,n-pentacosane","cp_hs_model",
		ReplaceMember(pentacosaneModel,"table_metadata",pentacosaneMetadata))),
		"pentacosane cp model rejects a zeroed residual uncertainty");
	RISECBOR64::Value::Values pentacosaneSegments = pentacosaneModel.Find(
		"segments")->GetArray();
	RISECBOR64::Value::Values unusedNasaCoefficients = pentacosaneSegments[0].Find(
		"coefficients")->GetArray();
	unusedNasaCoefficients[8] = RISECBOR64::Value::Float(0.0);
	pentacosaneSegments[0] = ReplaceMember(pentacosaneSegments[0],"coefficients",
		RISECBOR64::Value::ArrayValue(unusedNasaCoefficients));
	pentacosaneModel = ReplaceMember(pentacosaneModel,"segments",
		RISECBOR64::Value::ArrayValue(pentacosaneSegments));
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceThermoSpeciesMember(
		thermoValue,"C25H52,n-pentacosane","cp_hs_model",pentacosaneModel)),
		"pentacosane binds even non-operational NASA-9 provenance coefficients");
	RISECBOR64::Value::Values withoutPentacosane;
	for( const auto& species : thermoValue.Find("species")->GetArray() ) {
		if( species.Find("species_id")->GetText() != "C25H52,n-pentacosane" ) {
			withoutPentacosane.push_back(species);
		}
	}
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceMember(thermoValue,
		"species",RISECBOR64::Value::ArrayValue(withoutPentacosane))),
		"the approved pentacosane species is mandatory in this record version");
	RISECBOR64::Value::Values missingStubs = thermoValue.Find(
		"missing_required_records")->GetArray();
	RISECBOR64::Value::Members placeholderStub = missingStubs[0].GetMap();
	placeholderStub.push_back(std::make_pair(std::string("cp_placeholder"),
		RISECBOR64::Value::Float(1.0)));
	missingStubs[0] = RISECBOR64::Value::MapValue(placeholderStub);
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceMember(thermoValue,
		"missing_required_records",RISECBOR64::Value::ArrayValue(missingStubs))),
		"owner-gated species stub rejects placeholder physical values");
	missingStubs = thermoValue.Find("missing_required_records")->GetArray();
	std::reverse(missingStubs.begin(),missingStubs.end());
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceMember(thermoValue,
		"missing_required_records",RISECBOR64::Value::ArrayValue(missingStubs))),
		"owner-gated missing-record stubs have a frozen role order");
	missingStubs = thermoValue.Find("missing_required_records")->GetArray();
	missingStubs[1] = ReplaceMember(missingStubs[1],"status",
		RISECBOR64::Value::String("available"));
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceMember(thermoValue,
		"missing_required_records",RISECBOR64::Value::ArrayValue(missingStubs))),
		"owner-gated condensed stub cannot claim availability");
	missingStubs = thermoValue.Find("missing_required_records")->GetArray();
	missingStubs[0] = ReplaceMember(missingStubs[0],"failure_policy",
		RISECBOR64::Value::String("use_placeholder"));
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceMember(thermoValue,
		"missing_required_records",RISECBOR64::Value::ArrayValue(missingStubs))),
		"owner-gated vapor stub cannot weaken fail-closed policy");
	const char* closureRelationshipFields[] = {
		"molecular_diffusivity_relationship",
		"sgs_diffusivity_relationship",
		"total_diffusivity_relationship",
		"effective_conductivity_relationship",
		"effective_viscosity_relationship",
		"shared_diffusivity_rule",
		"dns_sgs_rule",
		"vreman_alpha_relationship",
		"vreman_beta_relationship",
		"vreman_B_beta_relationship",
		"vreman_nu_sgs_relationship",
		"vreman_nonnegative_B_rule",
		"vreman_zero_denominator_rule"
	};
	for( const char* field : closureRelationshipFields ) {
		const RISECBOR64::Value mutated = ReplaceEnvelopeValue(
			transportValue,field,"mutated");
		Check(mutated.Find(field) && mutated.Find(field)->Find("value") &&
			mutated.Find(field)->Find("value")->GetText() == "mutated",
			"transport closure mutation fixture changes the intended field");
		RISECBOR64::Bytes mutatedBytes;
		std::string mutationError;
		FireSimulationTransportRecord mutatedRecord;
		const bool mutationEncoded = RISECBOR64::Encode(
			mutated,mutatedBytes,&mutationError);
		Check(mutationEncoded,"transport closure mutation encodes canonically");
		Check(mutationEncoded &&
			!mutatedRecord.LoadCanonicalRecord(mutatedBytes,&mutationError),
			"transport rejects a mutated closure relationship");
	}
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceMember(thermoValue,
		"predictive_blockers",RISECBOR64::Value::ArrayValue({}))),
		"preview record rejects an empty blocker list");
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceFirstThermoSegmentMember(
		thermoValue,"certified_cp_lower_J_per_kg_K",RISECBOR64::Value::Float(1.0e30))),
		"thermochemistry rejects a false cp certificate");
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceFirstThermoSegmentMember(
		thermoValue,"hs_offset_J_per_kg",RISECBOR64::Value::Float(1.0))),
		"thermochemistry rejects a broken h_s reference/continuity constraint");
	RISECBOR64::Value::Values overflowingCoefficients = thermoValue.Find("species")->
		GetArray()[0].Find("cp_hs_model")->Find("segments")->GetArray()[0].Find(
			"coefficients")->GetArray();
	overflowingCoefficients[6] = RISECBOR64::Value::Float(
		std::numeric_limits<double>::max());
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceFirstThermoSegmentMember(
		thermoValue,"coefficients",RISECBOR64::Value::ArrayValue(overflowingCoefficients))),
		"thermochemistry rejects finite coefficients with non-finite evaluated properties");
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
	const RISECBOR64::Value hugeDomain = HugeFiniteThermoDomain(thermoValue);
	double hugeDomainMaximum = 0.0;
	for( const auto& species : hugeDomain.Find("species")->GetArray() ) {
		if( species.Find("species_id")->GetText() == "N2" ) {
			hugeDomainMaximum = species.Find("cp_hs_model")->Find(
				"temperature_domain_K")->GetArray()[1].GetFloat();
		}
	}
	Check(hugeDomainMaximum == 1.0e307,
		"huge-domain mutation fixture changes the intended species endpoint");
	RISECBOR64::Bytes hugeDomainBytes;
	std::string hugeDomainError;
	FireSimulationThermochemistryRecord hugeDomainRecord;
	const bool hugeDomainEncoded = RISECBOR64::Encode(
		hugeDomain,hugeDomainBytes,&hugeDomainError);
	Check(hugeDomainEncoded,"huge-domain mutation encodes canonically");
	const bool hugeDomainRejected = hugeDomainEncoded &&
		!hugeDomainRecord.LoadCanonicalRecord(hugeDomainBytes,&hugeDomainError) &&
		!hugeDomainError.empty();
	Check(hugeDomainRejected,
		"thermochemistry rejects a finite huge domain whose endpoint enthalpy overflows");
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
	Check(Rejects<FireSimulationThermochemistryRecord>(ReplaceMember(thermoValue,"version",
		RISECBOR64::Value::String("2.0.0"))),
		"thermochemistry rejects an unsupported aggregate record version");
	Check(Rejects<FireSimulationTransportRecord>(ReplaceMember(transportValue,"version",
		RISECBOR64::Value::String("2.0.0"))),
		"transport rejects an unsupported aggregate record version");
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
	RISECBOR64::Value reconstruction = *methaneValue.Find("conservative_reconstruction_v1");
	RISECBOR64::Value exactConstraint = *reconstruction.Find("constraint_matrix");
	RISECBOR64::Value::Values constraintRows = exactConstraint.Find("entries")->GetArray();
	RISECBOR64::Value::Values firstConstraintRow = constraintRows[0].GetArray();
	firstConstraintRow[0] = ReplaceMember(firstConstraintRow[0],"numerator",
		RISECBOR64::Value::String("123456789"));
	constraintRows[0] = RISECBOR64::Value::ArrayValue(firstConstraintRow);
	exactConstraint = ReplaceMember(exactConstraint,"entries",
		RISECBOR64::Value::ArrayValue(constraintRows));
	reconstruction = ReplaceMember(reconstruction,"constraint_matrix",exactConstraint);
	const std::string falseRankError = RejectionError<FireSimulationMethaneRecord>(
		ReplaceMember(methaneValue,"conservative_reconstruction_v1",reconstruction));
	Check(falseRankError.find("not the adopted physical preset") != std::string::npos,
		"mutating physical A is rejected at the adopted-record identity boundary");
	RISECBOR64::Value fluxClosure = *methaneValue.Find("nonadvective_flux_projection_v1");
	RISECBOR64::Value numericalBasis = *fluxClosure.Find("orthonormal_nullspace");
	RISECBOR64::Value::Values basisRows = numericalBasis.Find("entries")->GetArray();
	RISECBOR64::Value::Values basisFirstRow = basisRows[0].GetArray();
	basisFirstRow[0] = RISECBOR64::Value::Float(basisFirstRow[0].GetFloat()+0.125);
	basisRows[0] = RISECBOR64::Value::ArrayValue(basisFirstRow);
	numericalBasis = ReplaceMember(numericalBasis,"entries",
		RISECBOR64::Value::ArrayValue(basisRows));
	fluxClosure = ReplaceMember(fluxClosure,"orthonormal_nullspace",numericalBasis);
	const std::string falseBasisError = RejectionError<FireSimulationMethaneRecord>(
		ReplaceMember(methaneValue,"nonadvective_flux_projection_v1",fluxClosure));
	Check(falseBasisError.find("not the adopted physical preset") != std::string::npos,
		"mutating N_C bytes is rejected at the adopted-record identity boundary");
	RISECBOR64::Value reaction = *methaneValue.Find("primary_reaction");
	RISECBOR64::Value::Values reactionDelta = reaction.Find(
		"constituent_delta_kg_per_kg_fuel")->GetArray();
	reactionDelta[3] = ReplaceMember(reactionDelta[3],"numerator",
		RISECBOR64::Value::String("1"));
	reaction = ReplaceMember(reaction,"constituent_delta_kg_per_kg_fuel",
		RISECBOR64::Value::ArrayValue(reactionDelta));
	const std::string falseBalanceError = RejectionError<FireSimulationMethaneRecord>(
		ReplaceMember(methaneValue,"primary_reaction",reaction));
	Check(falseBalanceError.find("not the adopted physical preset") != std::string::npos,
		"mutating a physical methane product coefficient fails the adopted identity");
	Check(Rejects<FireSimulationMethaneRecord>(ReplaceMember(methaneValue,"record_name",
		RISECBOR64::Value::String("recertified methane mutation"))),
		"semantically valid methane metadata mutation is rejected by adopted preset identity");
	FireSimulationMethaneRecord invalidatedMethane;
	Check(invalidatedMethane.LoadCanonicalRecord(methane.RecordBytes()),
		"reload regression starts from a valid methane record");
	Check(!invalidatedMethane.LoadCanonicalRecord(malformedBytes) &&
		!invalidatedMethane.IsValid() && invalidatedMethane.RecordBytes().empty(),
		"failed reload clears all previously valid methane state");
	Check(Rejects<FireSimulationGasOpacityRecord>(ReplaceMember(opacityRecordValue,
		"record_name",RISECBOR64::Value::String("recertified mutation"))),
		"Planck-mean record identity rejects a canonically recertified mutation");
	FireSimulationGasOpacityRecord invalidatedOpacity;
	Check(invalidatedOpacity.LoadCanonicalRecord(opacity.RecordBytes()),
		"reload regression starts from a valid Planck-mean record");
	Check(!invalidatedOpacity.LoadCanonicalRecord(malformedBytes) &&
		!invalidatedOpacity.IsValid() && invalidatedOpacity.RecordBytes().empty() &&
		!invalidatedOpacity.FindSpecies("H2O"),
		"failed reload clears all previously valid Planck-mean state");

	if( failures ) {
		std::printf("FireSimulationRecordsTest: %d failure(s)\n",failures);
		return 1;
	}
	std::printf("FireSimulationRecordsTest: all checks passed\n");
	return 0;
}
