//////////////////////////////////////////////////////////////////////
//
//  FireProductionSolverTest.cpp - production-fire P0 capability/tables
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Utilities/FireProductionCompute.h"
#include "../src/Library/Utilities/FireProductionTables.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

namespace
{
	int failures=0;

	void Check( bool condition, const char* message )
	{
		if( !condition ) {
			std::cerr << "FAIL: " << message << '\n';
			++failures;
		}
	}

	const RISE::FireThermochemistrySegment* SegmentAt(
		const RISE::FireThermochemistrySpecies& species, double temperatureK )
	{
		for( const RISE::FireThermochemistrySegment& segment : species.segments )
			if( temperatureK>=segment.temperatureMinK&&temperatureK<=segment.temperatureMaxK )
				return &segment;
		return 0;
	}
}

int main()
{
	using namespace RISE;
	const FireSimulationMethaneRecord& methane=FireSimulationMethaneRecord::PhysicalV1();
	const FireSimulationGasOpacityRecord& opacity=
		FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1();
	std::string error;
	FireProductionTablePackage package,repeated;
	Check(BuildFireProductionTablePackage(methane,opacity,package,&error),
		"record-derived production table package compiles");
	Check(BuildFireProductionTablePackage(methane,opacity,repeated,&error)&&
		package.CanonicalEnvelope()==repeated.CanonicalEnvelope()&&
		package.TablePackageId()==repeated.TablePackageId(),
		"production table compiler is byte deterministic");
	Check(package.TablePackageId()==
		"c4fbd663dcdfa97c1212fb451ca8710c9724ec21d0cdcfda0d017548878a9392",
		"production r83 table package digest matches the independently pinned fixture");

	RISECBOR64::Value envelope;RISECBOR64::Bytes payloadBytes;
	const RISECBOR64::Value* payload=0;const RISECBOR64::Value* id=0;
	Check(RISECBOR64::DecodeCanonical(package.CanonicalEnvelope(),envelope,&error)&&
		envelope.GetType()==RISECBOR64::Value::Map&&envelope.GetMap().size()==2u&&
		(payload=envelope.Find("payload"))!=0&&(id=envelope.Find("table_package_id"))!=0&&
		id->GetType()==RISECBOR64::Value::Text&&RISECBOR64::Encode(*payload,payloadBytes,&error)&&
		id->GetText()==RISECBOR64::SHA256Hex(payloadBytes)&&id->GetText()==package.TablePackageId(),
		"production table package has one canonical payload preimage");
	Check(payload&&payload->Find("thermochemistry_record_id")&&
		payload->Find("thermochemistry_record_id")->GetText()==methane.RecordId()&&
		payload->Find("gas_opacity_record_id")&&
		payload->Find("gas_opacity_record_id")->GetText()==opacity.RecordId(),
		"production table manifest binds both source record IDs");

	Check(package.Thermochemistry().size()==methane.SpeciesOrder().size(),
		"production thermochemistry table preserves record species order");
	for( std::size_t speciesIndex=0;speciesIndex<package.Thermochemistry().size();++speciesIndex ) {
		const FireProductionThermochemistryTable& table=package.Thermochemistry()[speciesIndex];
		const FireThermochemistrySpecies* source=methane.FindSpecies(table.speciesId.c_str());
		Check(source&&table.speciesId==methane.SpeciesOrder()[speciesIndex]&&
			table.temperatureK.size()==table.sensibleEnthalpyJPerKG.size()&&
			table.temperatureK.size()==table.cpJPerKGK.size()&&table.temperatureK.size()>=2u,
			"production thermochemistry table shape and order are exact");
		if( !source ) continue;
		for( std::size_t i=0;i+1u<table.temperatureK.size();++i ) {
			const double middle=0.5*(table.temperatureK[i]+table.temperatureK[i+1u]);
			double exactH=0.0,exactCp=0.0,compiledH=0.0,compiledCp=0.0;
			const FireThermochemistrySegment* segment=SegmentAt(*source,middle);
			Check(segment&&methane.SensibleEnthalpyJPerKG(table.speciesId.c_str(),middle,exactH,&error)&&
				methane.CpJPerKGK(table.speciesId.c_str(),middle,exactCp,&error)&&
				package.ThermochemistryValues(table.speciesId.c_str(),middle,compiledH,compiledCp,&error)&&
				std::fabs(compiledH-exactH)<=segment->certifiedCpLowerJPerKGK*0.25&&
				std::fabs(compiledCp-exactCp)*0.5*(table.temperatureK[i+1u]-table.temperatureK[i])<=
					segment->certifiedCpLowerJPerKGK*0.25,
				"production thermochemistry midpoint meets the derived quarter-kelvin gate");
		}
	}

	Check(package.Opacity().size()==2u&&package.Opacity()[0].speciesId=="CO2"&&
		package.Opacity()[1].speciesId=="H2O","production opacity table has the adopted species");
	for( const FireProductionOpacityTable& table : package.Opacity() ) {
		Check(table.maximumRelativeError<=0.005&&table.gasTemperatureK.size()>=2u&&
			table.radiationTemperatureK.size()>=2u,
			"production opacity compiler meets the derived half-percent gate");
		for( std::size_t g=0;g+1u<table.gasTemperatureK.size();++g )
			for( std::size_t r=0;r+1u<table.radiationTemperatureK.size();++r ) {
				const double gas=0.5*(table.gasTemperatureK[g]+table.gasTemperatureK[g+1u]);
				const double radiation=0.5*(table.radiationTemperatureK[r]+
					table.radiationTemperatureK[r+1u]);
				double exact=0.0,dGas=0.0,dRadiation=0.0,compiled=0.0;
				Check(opacity.PlanckMeanCrossSectionM2PerMolecule(table.speciesId.c_str(),gas,
					radiation,exact,dGas,dRadiation,&error)&&
					package.PlanckMeanM2PerMolecule(table.speciesId.c_str(),gas,radiation,compiled,&error)&&
					std::fabs(compiled-exact)<=0.005*std::max(std::fabs(compiled),std::fabs(exact)),
					"production opacity midpoint matches the certified record");
			}
	}

	double h=0.0,cp=0.0,kappa=0.0;
	Check(!package.ThermochemistryValues("CH4",methane.TemperatureMinK()-1.0,h,cp,&error)&&
		!package.PlanckMeanM2PerMolecule("CO2",opacity.TemperatureMaxK()+1.0,
			opacity.TemperatureMinK(),kappa,&error),
		"production table runtime rejects extrapolation");

	FireProductionComputeCapability capability;
	Check(QueryFireProductionComputeCapability(capability),
		"production compute capability query completes structurally");
#if defined(__APPLE__)
	if( !capability.available )
		std::cerr << "Metal capability detail: " << capability.structuredError << '\n';
	Check(capability.available&&capability.identityKernelPassed&&capability.backend=="metal"&&
		!capability.deviceName.empty()&&!capability.deviceFamily.empty()&&
		capability.registryId!=0u&&capability.maximumThreadsPerThreadgroup>=8u&&
		capability.structuredError.empty(),
		"Metal production capability compiles, dispatches, and verifies fp32 bytes");
#else
	Check(!capability.available&&!capability.identityKernelPassed&&capability.backend=="unavailable"&&
		!capability.structuredError.empty(),
		"non-Metal production capability reports honest unavailability");
#endif

	if( failures==0 ) {
		std::cout << "FireProductionSolverTest passed: table_package_id="
			<< package.TablePackageId() << "\n";
		return 0;
	}
	std::cerr << failures << " FireProductionSolverTest failure(s)\n";
	return 1;
}
