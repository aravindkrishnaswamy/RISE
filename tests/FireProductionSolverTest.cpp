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
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
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

	std::string ReadText( const char* path )
	{
		std::ifstream input(path);
		std::ostringstream text;
		text << input.rdbuf();
		return text.str();
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
	const std::string expectedTablePackageId=
		"3e8f46503657cf137bb5e812a9090ae1bf09792c7f2e7b1222eb9e3b2b70ce55";
	if( package.TablePackageId()!=expectedTablePackageId )
		std::cerr << "Observed table package ID: " << package.TablePackageId() << '\n';
	Check(package.TablePackageId()==expectedTablePackageId,
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
			const double fractions[]={0.25,0.5,0.75};
			for( const double fraction : fractions ) {
				const double sample=(1.0-fraction)*table.temperatureK[i]+
					fraction*table.temperatureK[i+1u];
				double exactH=0.0,exactCp=0.0,compiledH=0.0,compiledCp=0.0;
				const FireThermochemistrySegment* segment=SegmentAt(*source,sample);
				Check(segment&&methane.SensibleEnthalpyJPerKG(table.speciesId.c_str(),sample,exactH,&error)&&
					methane.CpJPerKGK(table.speciesId.c_str(),sample,exactCp,&error)&&
					package.ThermochemistryValues(table.speciesId.c_str(),sample,compiledH,compiledCp,&error)&&
					std::fabs(compiledH-exactH)<=table.maximumEnthalpyErrorJPerKG&&
					std::fabs(compiledCp-exactCp)*0.5*(table.temperatureK[i+1u]-table.temperatureK[i])<=
						table.maximumCpIntegratedErrorJPerKG&&
					std::fabs(compiledH-exactH)<=segment->certifiedCpLowerJPerKGK*0.25,
					"production thermochemistry independent samples meet the certified gate");
			}
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
				const double fractions[]={0.125,0.375,0.625,0.875};
				for( const double gasFraction : fractions ) for( const double radiationFraction : fractions ) {
				const double gas=(1.0-gasFraction)*table.gasTemperatureK[g]+
					gasFraction*table.gasTemperatureK[g+1u];
				const double radiation=(1.0-radiationFraction)*table.radiationTemperatureK[r]+
					radiationFraction*table.radiationTemperatureK[r+1u];
				double exact=0.0,dGas=0.0,dRadiation=0.0,compiled=0.0;
				Check(opacity.PlanckMeanCrossSectionM2PerMolecule(table.speciesId.c_str(),gas,
					radiation,exact,dGas,dRadiation,&error)&&
					package.PlanckMeanM2PerMolecule(table.speciesId.c_str(),gas,radiation,compiled,&error)&&
					std::fabs(compiled-exact)<=table.maximumRelativeError*
						std::max(std::fabs(compiled),std::fabs(exact))&&
					std::fabs(compiled-exact)<=0.005*std::max(std::fabs(compiled),std::fabs(exact)),
					"production opacity independent stencil matches the certified record");
				}
			}
	}

	double h=11.0,cp=12.0,kappa=13.0;
	const double nan=std::numeric_limits<double>::quiet_NaN();
	const double infinity=std::numeric_limits<double>::infinity();
	Check(!package.ThermochemistryValues("CH4",methane.TemperatureMinK()-1.0,h,cp,&error)&&
		h==11.0&&cp==12.0&&!package.ThermochemistryValues("CH4",methane.TemperatureMaxK()+1.0,h,cp,&error)&&
		!package.ThermochemistryValues("CH4",nan,h,cp,&error)&&
		!package.ThermochemistryValues("CH4",infinity,h,cp,&error)&&
		!package.ThermochemistryValues("unknown",methane.TemperatureMinK(),h,cp,&error)&&
		h==11.0&&cp==12.0,
		"production thermochemistry rejects both bounds, nonfinite input, and unknown species");
	Check(!package.PlanckMeanM2PerMolecule("CO2",opacity.TemperatureMinK()-1.0,
		opacity.TemperatureMinK(),kappa,&error)&&kappa==13.0&&
		!package.PlanckMeanM2PerMolecule("CO2",opacity.TemperatureMaxK()+1.0,
			opacity.TemperatureMinK(),kappa,&error)&&
		!package.PlanckMeanM2PerMolecule("CO2",opacity.TemperatureMinK(),
			opacity.TemperatureMinK()-1.0,kappa,&error)&&
		!package.PlanckMeanM2PerMolecule("CO2",opacity.TemperatureMinK(),
			opacity.TemperatureMaxK()+1.0,kappa,&error)&&
		!package.PlanckMeanM2PerMolecule("CO2",nan,opacity.TemperatureMinK(),kappa,&error)&&
		!package.PlanckMeanM2PerMolecule("CO2",opacity.TemperatureMinK(),infinity,kappa,&error)&&
		!package.PlanckMeanM2PerMolecule("unknown",opacity.TemperatureMinK(),
			opacity.TemperatureMinK(),kappa,&error)&&kappa==13.0,
		"production opacity rejects every axis bound, nonfinite input, and unknown species");
	const FireProductionThermochemistryTable& ch4=package.Thermochemistry().front();
	const double aboveThermoKnot=std::nextafter(static_cast<double>(
		ch4.temperatureK[ch4.temperatureK.size()/2u]),infinity);
	const FireProductionOpacityTable& co2=package.Opacity().front();
	const double aboveGasKnot=std::nextafter(static_cast<double>(
		co2.gasTemperatureK[co2.gasTemperatureK.size()/2u]),infinity);
	const double aboveRadiationKnot=std::nextafter(static_cast<double>(
		co2.radiationTemperatureK[co2.radiationTemperatureK.size()/2u]),infinity);
	Check(package.ThermochemistryValues("CH4",aboveThermoKnot,h,cp,&error)&&
		package.PlanckMeanM2PerMolecule("CO2",aboveGasKnot,aboveRadiationKnot,kappa,&error),
		"in-domain values immediately above fp32 knots select the upper interval");

	FireProductionComputeCapability capability;
	Check(QueryFireProductionComputeCapability(capability),
		"production compute capability query completes structurally");
#if defined(__APPLE__)
	if( !capability.available )
		std::cerr << "Metal capability detail: " << capability.structuredError << '\n';
	Check(capability.available&&capability.identityKernelPassed&&capability.backend=="metal"&&
		!capability.deviceName.empty()&&!capability.deviceFamily.empty()&&
		capability.deviceFamily!="metal-family-unreported"&&
		capability.registryId!=0u&&capability.maximumThreadsPerThreadgroup>=8u&&
		capability.structuredError.empty(),
		"Metal production capability compiles, dispatches, and verifies fp32 bytes");
#if defined(__arm64__)
	Check(capability.unifiedMemory,"Apple-silicon Metal capability reports unified memory");
#endif
	std::vector<std::uint32_t> challenge(16u),returned;
	for( std::size_t i=0;i<challenge.size();++i )
		challenge[i]=static_cast<std::uint32_t>(package.TablePackageId()[i])*0x01010101u+
			static_cast<std::uint32_t>(i);
	Check(RunFireProductionComputeChallenge(challenge.data(),challenge.size(),returned,&error)&&
		returned.size()==challenge.size(),"Metal production challenge dispatch returns every word");
	for( std::size_t i=0;i<returned.size();++i )
		Check(returned[i]==(challenge[i]^(0x9e3779b9u+static_cast<std::uint32_t>(i)*0x85ebca6bu)),
			"Metal production challenge proves nonidentity device execution");
	const std::string metalSource=ReadText("src/Library/Utilities/FireProductionComputeMac.mm");
	Check(metalSource.find("newLibraryWithSource")!=std::string::npos&&
		metalSource.find("dispatchThreads")!=std::string::npos&&
		metalSource.find("[command commit]")!=std::string::npos&&
		metalSource.find("MTLCommandBufferStatusCompleted")!=std::string::npos&&
		metalSource.find("[output contents]")!=std::string::npos,
		"Metal capability source gate binds compilation, dispatch, completion, and returned bytes");
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
