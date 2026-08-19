//////////////////////////////////////////////////////////////////////
//
//  FireProductionSolverTest.cpp - production-fire P0 capability/tables
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Utilities/FireProductionCompute.h"
#include "../src/Library/Utilities/FireProductionAdvection.h"
#include "../src/Library/Utilities/FireProductionTables.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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

	std::size_t CountSubstring( const std::string& text, const std::string& needle )
	{
		std::size_t count=0u,position=0u;
		while( (position=text.find(needle,position))!=std::string::npos ) {
			++count;position+=needle.size();
		}
		return count;
	}

	std::size_t RemapValueIndex( const RISE::FireProductionRemapRequest& request,
		std::size_t component, std::size_t line, std::size_t cell )
	{
		return (component*request.lineCount+line)*request.lineLength+cell;
	}

	std::size_t RemapFluxIndex( const RISE::FireProductionRemapRequest& request,
		std::size_t component, std::size_t line, std::size_t face )
	{
		return (component*request.lineCount+line)*(request.lineLength+1u)+face;
	}

	RISE::FireProductionRemapRequest PeriodicRequest( std::size_t cells,
		std::size_t components, float courant )
	{
		RISE::FireProductionRemapRequest request;
		request.lineLength=cells;
		request.lineCount=1u;
		request.componentCount=components;
		request.cellWidthM=1.0f/static_cast<float>(cells);
		request.timeStepS=courant*request.cellWidthM;
		request.boundary=RISE::FireProductionRemapPeriodic;
		request.values.resize(cells*components);
		request.faceVelocityMPerS.assign(cells+1u,1.0f);
		request.ambientValues.assign(components,0.0f);
		return request;
	}

	bool NearFloat( float a, float b, float relative=2.0e-5f )
	{
		return std::isfinite(a)&&std::isfinite(b)&&
			std::fabs(a-b)<=relative*std::max(1.0f,std::max(std::fabs(a),std::fabs(b)));
	}

	bool SameRemapWithin( const RISE::FireProductionRemapResult& cpu,
		const RISE::FireProductionRemapResult& gpu, float relative )
	{
		if( cpu.updatedValues.size()!=gpu.updatedValues.size()||
			cpu.faceFluxes.size()!=gpu.faceFluxes.size()||
			cpu.sharedLimiterAlpha.size()!=gpu.sharedLimiterAlpha.size() ) return false;
		for( std::size_t i=0;i<cpu.updatedValues.size();++i )
			if( !NearFloat(cpu.updatedValues[i],gpu.updatedValues[i],relative) ) return false;
		for( std::size_t i=0;i<cpu.faceFluxes.size();++i )
			if( !NearFloat(cpu.faceFluxes[i],gpu.faceFluxes[i],relative) ) return false;
		for( std::size_t i=0;i<cpu.sharedLimiterAlpha.size();++i )
			if( !NearFloat(cpu.sharedLimiterAlpha[i],gpu.sharedLimiterAlpha[i],relative) ) return false;
		return true;
	}

	double SmoothPeriodicError( std::size_t cells, RISE::FireProductionRemapResult* output )
	{
		const double pi=std::acos(-1.0);
		const double shift=0.2;
		RISE::FireProductionRemapRequest request=PeriodicRequest(cells,1u,
			static_cast<float>(shift*static_cast<double>(cells)));
		for( std::size_t cell=0;cell<cells;++cell ) {
			const double left=static_cast<double>(cell)/static_cast<double>(cells);
			const double right=static_cast<double>(cell+1u)/static_cast<double>(cells);
			request.values[cell]=static_cast<float>(2.0+(std::cos(2.0*pi*left)-
				std::cos(2.0*pi*right))/(2.0*pi*(right-left)));
		}
		RISE::FireProductionRemapResult result;
		std::string error;
		if( !RISE::RemapFireProductionCPU(request,result,&error) )
			return std::numeric_limits<double>::max();
		double l1=0.0;
		for( std::size_t cell=0;cell<cells;++cell ) {
			const double left=static_cast<double>(cell)/static_cast<double>(cells)-shift;
			const double right=static_cast<double>(cell+1u)/static_cast<double>(cells)-shift;
			const double exact=2.0+(std::cos(2.0*pi*left)-std::cos(2.0*pi*right))/
				(2.0*pi/static_cast<double>(cells));
			l1+=std::fabs(static_cast<double>(result.updatedValues[cell])-exact);
		}
		if( output ) *output=result;
		return l1/static_cast<double>(cells);
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

	FireProductionRemapRequest constant=PeriodicRequest(32u,3u,0.3f);
	constant.values.assign(constant.values.size(),0.1f);
	FireProductionRemapResult constantCPU;
	const bool constantRemapped=RemapFireProductionCPU(constant,constantCPU,&error);
	const bool constantExact=constantRemapped&&
		std::all_of(constantCPU.updatedValues.begin(),constantCPU.updatedValues.end(),
			[](float value){return value==0.1f;});
	if( !constantExact&&constantRemapped ) {
		auto range=std::minmax_element(constantCPU.updatedValues.begin(),constantCPU.updatedValues.end());
		std::cerr << std::hexfloat << "Observed constant remap range: " << *range.first << ", "
			<< *range.second << std::defaultfloat << '\n';
		for( std::size_t i=0;i<constantCPU.updatedValues.size();++i )
			if( constantCPU.updatedValues[i]!=0.1f ) {
				std::cerr << "First constant mismatch index " << i << " value " << std::hexfloat
					<< constantCPU.updatedValues[i] << std::defaultfloat << '\n';break;
			}
		auto alphaRange=std::minmax_element(constantCPU.sharedLimiterAlpha.begin(),
			constantCPU.sharedLimiterAlpha.end());
		std::cerr << "Constant alpha range: " << *alphaRange.first << ", " << *alphaRange.second << '\n';
	}
	Check(constantExact,
		"production remap preserves an ordinary binary32 periodic constant exactly");

	FireProductionRemapRequest latePrefix=PeriodicRequest(1024u,1u,1.0e-6f);
	latePrefix.cellWidthM=1.0f;latePrefix.timeStepS=1.0e-6f;
	latePrefix.values.assign(latePrefix.values.size(),1.0e8f);
	FireProductionRemapResult latePrefixCPU;
	Check(RemapFireProductionCPU(latePrefix,latePrefixCPU,&error)&&
		latePrefixCPU.faceFluxes[RemapFluxIndex(latePrefix,0u,0u,1023u)]>0.0f&&
		std::all_of(latePrefixCPU.updatedValues.begin(),latePrefixCPU.updatedValues.end(),
			[](float value){return value==1.0e8f;}),
		"local fractional integral survives a tiny late-cell sweep without prefix cancellation");
	FireProductionRemapRequest subUlpSweep=PeriodicRequest(9u,1u,0x1p-25f);
	subUlpSweep.cellWidthM=1.0f;subUlpSweep.timeStepS=0x1p-25f;
	subUlpSweep.values.assign(subUlpSweep.values.size(),0.1f);
	FireProductionRemapResult subUlpSweepCPU;
	Check(RemapFireProductionCPU(subUlpSweep,subUlpSweepCPU,&error)&&
		subUlpSweepCPU.faceFluxes.front()>0.0f&&
		std::all_of(subUlpSweepCPU.updatedValues.begin(),subUlpSweepCPU.updatedValues.end(),
			[](float value){return value==0.1f;}),
		"sub-ulp trailing sweep advances without constructing the rounded value one-minus-C");
	for( const std::pair<std::size_t,float>& largeCourant : {
		std::make_pair(std::size_t(5u),0x1.fff832p+24f),
		std::make_pair(std::size_t(7u),0x1.110bb6p+62f)} ) {
		FireProductionRemapRequest large=PeriodicRequest(largeCourant.first,1u,
			largeCourant.second);
		large.cellWidthM=1.0f;large.timeStepS=largeCourant.second;
		large.values.assign(large.values.size(),0.1f);
		FireProductionRemapResult largeCPU;
		Check(RemapFireProductionCPU(large,largeCPU,&error)&&
			std::all_of(largeCPU.updatedValues.begin(),largeCPU.updatedValues.end(),
				[](float value){return value==0.1f;}),
			"large finite periodic Courant has a bounded canonical quotient and remainder");
	}

	FireProductionRemapRequest seam=PeriodicRequest(7u,1u,0.1f);
	for( std::size_t cell=0;cell<seam.lineLength;++cell )
		seam.values[cell]=0.1f+0.137f*static_cast<float>(cell);
	FireProductionRemapResult seamPositive,seamNegative;
	const bool seamPositiveOK=RemapFireProductionCPU(seam,seamPositive,&error);
	std::fill(seam.faceVelocityMPerS.begin(),seam.faceVelocityMPerS.end(),-1.0f);
	const bool seamNegativeOK=RemapFireProductionCPU(seam,seamNegative,&error);
	Check(seamPositiveOK&&seamNegativeOK&&
		seamPositive.faceFluxes.front()==seamPositive.faceFluxes.back()&&
		seamNegative.faceFluxes.front()==seamNegative.faceFluxes.back(),
		"non-power-of-two periodic lines publish one exact seam for both velocity signs");
	FireProductionRemapRequest unequalSeam=seam;
	unequalSeam.faceVelocityMPerS.back()=-2.0f;
	Check(!ValidateFireProductionRemapRequest(unequalSeam,&error),
		"periodic remap rejects a multi-valued seam velocity");

	FireProductionRemapRequest blelloch=PeriodicRequest(8u,1u,8.0f);
	blelloch.values={1.0e8f,1.0f,-1.0e8f,1.0f,1.0e8f,1.0f,-1.0e8f,1.0f};
	FireProductionRemapResult blellochCPU;
	Check(RemapFireProductionCPU(blelloch,blellochCPU,&error)&&
		blellochCPU.faceFluxes.front()==0.0f&&
		blellochCPU.faceFluxes.front()==blellochCPU.faceFluxes.back(),
		"whole-domain transport uses the pinned cancellation-sensitive Blelloch root");

	for( const float signedCourant : {0.75f,2.25f,70.25f,-2.25f} ) {
		FireProductionRemapRequest pulse=PeriodicRequest(64u,2u,std::fabs(signedCourant));
		if( signedCourant<0.0f )
			std::fill(pulse.faceVelocityMPerS.begin(),pulse.faceVelocityMPerS.end(),-1.0f);
		for( std::size_t cell=16u;cell<32u;++cell ) {
			pulse.values[RemapValueIndex(pulse,0u,0u,cell)]=1.0f;
			pulse.values[RemapValueIndex(pulse,1u,0u,cell)]=0.5f;
		}
		FireProductionRemapResult pulseCPU;
		Check(RemapFireProductionCPU(pulse,pulseCPU,&error),
			"periodic pulse remaps below, above, negative, and beyond-domain Courant numbers");
		for( std::size_t component=0;component<2u;++component ) {
			float before=0.0f,after=0.0f;
			for( std::size_t cell=0;cell<pulse.lineLength;++cell ) {
				before+=pulse.values[RemapValueIndex(pulse,component,0u,cell)];
				after+=pulseCPU.updatedValues[RemapValueIndex(pulse,component,0u,cell)];
			}
			Check(before==after,"periodic arbitrary-Courant pulse conserves each component");
		}
	}

	FireProductionRemapResult smooth64Result;
	const double smooth16=SmoothPeriodicError(16u,0);
	const double smooth32=SmoothPeriodicError(32u,0);
	const double smooth64=SmoothPeriodicError(64u,&smooth64Result);
	const double order16To32=std::log(smooth16/smooth32)/std::log(2.0);
	const double order32To64=std::log(smooth32/smooth64)/std::log(2.0);
	if( !(order16To32>=1.8&&order32To64>=1.8) )
		std::cerr << "Observed production remap orders: " << order16To32 << ", "
			<< order32To64 << " errors: " << smooth16 << ", " << smooth32 << ", "
			<< smooth64 << '\n';
	Check(order16To32>=1.8&&order32To64>=1.8,
		"smooth periodic production remap has at least 1.8 observed order");

	FireProductionRemapRequest affine=PeriodicRequest(48u,3u,1.35f);
	for( std::size_t cell=0;cell<affine.lineLength;++cell ) {
		const float first=1.0f+0.4f*std::sin(static_cast<float>(2.0*std::acos(-1.0)*
			static_cast<double>(cell)/static_cast<double>(affine.lineLength)));
		const float second=0.6f+(cell>=11u&&cell<19u?0.7f:0.0f)+
			0.1f*std::cos(static_cast<float>(6.0*std::acos(-1.0)*
			static_cast<double>(cell)/static_cast<double>(affine.lineLength)));
		affine.values[RemapValueIndex(affine,0u,0u,cell)]=first;
		affine.values[RemapValueIndex(affine,1u,0u,cell)]=second;
		affine.values[RemapValueIndex(affine,2u,0u,cell)]=first+second;
	}
	FireProductionRemapResult affineCPU;
	Check(RemapFireProductionCPU(affine,affineCPU,&error),
		"common-limiter affine tuple remaps");
	float maximumAffineResidual=0.0f;
	for( std::size_t cell=0;cell<affine.lineLength;++cell )
		maximumAffineResidual=std::max(maximumAffineResidual,std::fabs(
			affineCPU.updatedValues[RemapValueIndex(affine,2u,0u,cell)]-
			(affineCPU.updatedValues[RemapValueIndex(affine,0u,0u,cell)]+
			 affineCPU.updatedValues[RemapValueIndex(affine,1u,0u,cell)])));
	if( maximumAffineResidual>2.0e-5f )
		std::cerr << "Observed common-weight affine residual: " << maximumAffineResidual << '\n';
	Check(maximumAffineResidual<=2.0e-5f,
		"one tuple limiter preserves a cancellation-sensitive affine constituent row");

	FireProductionRemapRequest open;
	open.lineLength=8u;open.lineCount=1u;open.componentCount=2u;
	open.cellWidthM=1.0f;open.timeStepS=0.5f;
	open.boundary=FireProductionRemapPressureOpen;
	open.values.assign(16u,2.0f);open.faceVelocityMPerS.assign(9u,0.0f);
	open.faceVelocityMPerS.front()=1.0f;open.faceVelocityMPerS.back()=1.0f;
	open.ambientValues={5.0f,7.0f};
	FireProductionRemapResult openCPU;
	Check(RemapFireProductionCPU(open,openCPU,&error)&&
		openCPU.faceFluxes[RemapFluxIndex(open,0u,0u,0u)]==2.5f&&
		openCPU.faceFluxes[RemapFluxIndex(open,0u,0u,8u)]==1.0f&&
		openCPU.faceFluxes[RemapFluxIndex(open,1u,0u,0u)]==3.5f&&
		openCPU.faceFluxes[RemapFluxIndex(open,1u,0u,8u)]==1.0f,
		"pressure-open remap uses ambient inflow and nearest-interior outflow");
	FireProductionRemapRequest openNegative=open;
	openNegative.faceVelocityMPerS.front()=-1.0f;
	openNegative.faceVelocityMPerS.back()=-1.0f;
	FireProductionRemapResult openNegativeCPU;
	Check(RemapFireProductionCPU(openNegative,openNegativeCPU,&error)&&
		openNegativeCPU.faceFluxes[RemapFluxIndex(openNegative,0u,0u,0u)]==-1.0f&&
		openNegativeCPU.faceFluxes[RemapFluxIndex(openNegative,0u,0u,8u)]==-2.5f&&
		openNegativeCPU.faceFluxes[RemapFluxIndex(openNegative,1u,0u,0u)]==-1.0f&&
		openNegativeCPU.faceFluxes[RemapFluxIndex(openNegative,1u,0u,8u)]==-3.5f,
		"pressure-open remap reverses ambient and interior roles under negative velocity");
	FireProductionRemapRequest wall=open;wall.boundary=FireProductionRemapWall;
	FireProductionRemapResult wallCPU;
	const bool wallRemapped=RemapFireProductionCPU(wall,wallCPU,&error);
	bool everyWallFluxZero=wallRemapped;
	for( std::size_t component=0;component<wall.componentCount;++component )
		everyWallFluxZero=everyWallFluxZero&&
			wallCPU.faceFluxes[RemapFluxIndex(wall,component,0u,0u)]==0.0f&&
			wallCPU.faceFluxes[RemapFluxIndex(wall,component,0u,wall.lineLength)]==0.0f;
	Check(everyWallFluxZero,
		"wall production remap has exact zero boundary flux");
	FireProductionRemapRequest wallNegative=openNegative;
	wallNegative.boundary=FireProductionRemapWall;
	FireProductionRemapResult wallNegativeCPU;
	const bool wallNegativeRemapped=RemapFireProductionCPU(wallNegative,wallNegativeCPU,&error);
	bool everyNegativeWallFluxZero=wallNegativeRemapped;
	for( std::size_t component=0;component<wallNegative.componentCount;++component )
		everyNegativeWallFluxZero=everyNegativeWallFluxZero&&
			wallNegativeCPU.faceFluxes[RemapFluxIndex(wallNegative,component,0u,0u)]==0.0f&&
			wallNegativeCPU.faceFluxes[RemapFluxIndex(wallNegative,component,0u,
				wallNegative.lineLength)]==0.0f;
	Check(everyNegativeWallFluxZero,
		"wall production remap has exact zero boundary flux under negative velocity");
	FireProductionRemapRequest wallToOpen=open;
	wallToOpen.asymmetricBoundaries=true;
	wallToOpen.lowerBoundary=FireProductionRemapWall;
	wallToOpen.upperBoundary=FireProductionRemapPressureOpen;
	FireProductionRemapResult wallToOpenCPU;
	Check(RemapFireProductionCPU(wallToOpen,wallToOpenCPU,&error)&&
		wallToOpenCPU.faceFluxes[RemapFluxIndex(wallToOpen,0u,0u,0u)]==0.0f&&
		wallToOpenCPU.faceFluxes[RemapFluxIndex(wallToOpen,0u,0u,8u)]==1.0f&&
		wallToOpenCPU.faceFluxes[RemapFluxIndex(wallToOpen,1u,0u,0u)]==0.0f&&
		wallToOpenCPU.faceFluxes[RemapFluxIndex(wallToOpen,1u,0u,8u)]==1.0f,
		"asymmetric remap applies a lower wall and upper pressure-open outflow independently");
	FireProductionRemapRequest openToWall=open;
	openToWall.asymmetricBoundaries=true;
	openToWall.lowerBoundary=FireProductionRemapPressureOpen;
	openToWall.upperBoundary=FireProductionRemapWall;
	FireProductionRemapResult openToWallCPU;
	Check(RemapFireProductionCPU(openToWall,openToWallCPU,&error)&&
		openToWallCPU.faceFluxes[RemapFluxIndex(openToWall,0u,0u,0u)]==2.5f&&
		openToWallCPU.faceFluxes[RemapFluxIndex(openToWall,0u,0u,8u)]==0.0f&&
		openToWallCPU.faceFluxes[RemapFluxIndex(openToWall,1u,0u,0u)]==3.5f&&
		openToWallCPU.faceFluxes[RemapFluxIndex(openToWall,1u,0u,8u)]==0.0f,
		"asymmetric remap applies lower pressure-open inflow and an upper wall independently");
	FireProductionRemapRequest invalidPeriodicPair=open;
	invalidPeriodicPair.asymmetricBoundaries=true;
	invalidPeriodicPair.lowerBoundary=FireProductionRemapPeriodic;
	invalidPeriodicPair.upperBoundary=FireProductionRemapWall;
	Check(!ValidateFireProductionRemapRequest(invalidPeriodicPair,&error)&&
		error.find("boundary pairing")!=std::string::npos,
		"asymmetric remap rejects an unpaired periodic boundary");
	FireProductionRemapRequest folded=open;
	folded.timeStepS=0.75f;
	folded.faceVelocityMPerS.assign(9u,0.0f);
	folded.faceVelocityMPerS[3u]=-1.0f;
	folded.faceVelocityMPerS[4u]=1.0f;
	FireProductionRemapResult foldedResult;foldedResult.updatedValues.push_back(9.0f);
	Check(!RemapFireProductionCPU(folded,foldedResult,&error)&&
		foldedResult.updatedValues.empty()&&error.find("folded")!=std::string::npos,
		"production remap rejects a crossed departure map instead of draining a donor twice");
	FireProductionRemapRequest roundedFold;
	roundedFold.lineLength=4u;roundedFold.lineCount=1u;roundedFold.componentCount=1u;
	roundedFold.cellWidthM=0x1.a0e166p+72f;roundedFold.timeStepS=0x1.34a348p+38f;
	roundedFold.boundary=FireProductionRemapPressureOpen;
	roundedFold.values.assign(4u,1.0e-30f);roundedFold.ambientValues.assign(1u,1.0e-30f);
	roundedFold.faceVelocityMPerS={0x1.cdfcecp+57f,0x1.cdfceep+57f,
		0x1.cdfceep+57f,0x1.cdfceep+57f,0x1.cdfceep+57f};
	Check(!ValidateFireProductionRemapRequest(roundedFold,&error)&&
		error.find("folded")!=std::string::npos,
		"fold admission evaluates the same rounded binary32 Courants as the kernels");
	FireProductionRemapRequest cancellationFold;
	cancellationFold.lineLength=4u;cancellationFold.lineCount=1u;
	cancellationFold.componentCount=1u;cancellationFold.cellWidthM=1.0f;
	cancellationFold.timeStepS=1.0f;cancellationFold.boundary=FireProductionRemapPressureOpen;
	cancellationFold.values.assign(4u,0x1p-100f);
	cancellationFold.ambientValues.assign(1u,0x1p-100f);
	cancellationFold.faceVelocityMPerS={0x1.000002p+24f,0x1.000002p+24f,
		0x1.000002p+24f,0x1.000004p+24f,0x1.000004p+24f};
	Check(!ValidateFireProductionRemapRequest(cancellationFold,&error)&&
		error.find("folded")!=std::string::npos,
		"fold admission preserves unit face separation after rounded large Courants");
	FireProductionRemapRequest invalid=constant;
	invalid.values[0]=std::numeric_limits<float>::quiet_NaN();
	FireProductionRemapResult invalidResult;invalidResult.updatedValues.push_back(9.0f);
	Check(!RemapFireProductionCPU(invalid,invalidResult,&error)&&
		invalidResult.updatedValues.empty()&&!error.empty(),
		"production remap rejects nonfinite state without returning partial output");
	FireProductionRemapRequest finiteOverflow=constant;
	finiteOverflow.values.assign(finiteOverflow.values.size(),
		std::numeric_limits<float>::max());
	FireProductionRemapResult finiteOverflowResult;
	Check(!RemapFireProductionCPU(finiteOverflow,finiteOverflowResult,&error)&&
		finiteOverflowResult.updatedValues.empty()&&!error.empty(),
		"finite input that overflows derived arithmetic fails without published output");
	FireProductionRemapRequest overflow=constant;
	overflow.lineCount=std::numeric_limits<std::size_t>::max();
	Check(!ValidateFireProductionRemapRequest(overflow,&error)&&!error.empty(),
		"production remap rejects overflowing dimensions before indexing or allocation");
	FireProductionRemapRequest excessiveWorkingSet;
	excessiveWorkingSet.lineLength=1024u;excessiveWorkingSet.lineCount=70000u;
	excessiveWorkingSet.componentCount=1u;excessiveWorkingSet.cellWidthM=1.0f;
	excessiveWorkingSet.timeStepS=0.0f;excessiveWorkingSet.boundary=FireProductionRemapPeriodic;
	Check(!ValidateFireProductionRemapRequest(excessiveWorkingSet,&error)&&
		error.find("two GiB")!=std::string::npos,
		"production remap accounts for every Metal buffer before the two-GiB admission gate");
	FireProductionRemapRequest admittedWorkingSet=excessiveWorkingSet;
	admittedWorkingSet.lineCount=65000u;
	Check(!ValidateFireProductionRemapRequest(admittedWorkingSet,&error)&&
		error.find("two GiB")==std::string::npos,
		"working-set RED straddles the complete two-GiB allocation boundary");
	FireProductionRemapRequest finalBytesBoundary;
	finalBytesBoundary.lineLength=4u;finalBytesBoundary.lineCount=1672495u;
	finalBytesBoundary.componentCount=12u;finalBytesBoundary.cellWidthM=1.0f;
	finalBytesBoundary.timeStepS=0.0f;
	finalBytesBoundary.boundary=FireProductionRemapPeriodic;
	Check(!ValidateFireProductionRemapRequest(finalBytesBoundary,&error)&&
		error.find("two GiB")!=std::string::npos,
		"two-GiB admission counts the ambient tuple and parameter bytes at the final float");
	FireProductionRemapRequest finalBytesBelow=finalBytesBoundary;
	finalBytesBelow.lineCount-=1u;
	Check(!ValidateFireProductionRemapRequest(finalBytesBelow,&error)&&
		error.find("two GiB")==std::string::npos,
		"final-byte resource fixture has a discriminating below-bound companion");

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
	FireProductionRemapResult constantGPU,constantGPURepeated,openGPU,openNegativeGPU,wallGPU,
		wallNegativeGPU,wallToOpenGPU,openToWallGPU,
		smoothGPU,affineGPU,latePrefixGPU,subUlpSweepGPU,blellochGPU;
	const bool constantMetal=RemapFireProductionMetal(constant,constantGPU,&error);
	if( !constantMetal ) std::cerr << "Metal remap detail: " << error << '\n';
	const bool repeatedMetal=constantMetal&&RemapFireProductionMetal(constant,constantGPURepeated,&error);
	if( constantMetal&&repeatedMetal&& !SameRemapWithin(constantCPU,constantGPU,2.0e-5f) ) {
		for( std::size_t i=0;i<constantCPU.updatedValues.size();++i )
			if( constantCPU.updatedValues[i]!=constantGPU.updatedValues[i] ) {
				std::cerr << "Constant CPU/GPU state mismatch " << i << ": " << std::hexfloat <<
					constantCPU.updatedValues[i] << " vs " << constantGPU.updatedValues[i] <<
					std::defaultfloat << '\n';break;
			}
		for( std::size_t i=0;i<constantCPU.faceFluxes.size();++i )
			if( constantCPU.faceFluxes[i]!=constantGPU.faceFluxes[i] ) {
				std::cerr << "Constant CPU/GPU flux mismatch " << i << ": " << std::hexfloat <<
					constantCPU.faceFluxes[i] << " vs " << constantGPU.faceFluxes[i] <<
					std::defaultfloat << '\n';break;
			}
		for( std::size_t i=0;i<constantCPU.sharedLimiterAlpha.size();++i )
			if( !NearFloat(constantCPU.sharedLimiterAlpha[i],
				constantGPU.sharedLimiterAlpha[i],2.0e-5f) ) {
				std::cerr << "Constant CPU/GPU alpha mismatch " << i << ": " << std::hexfloat <<
					constantCPU.sharedLimiterAlpha[i] << " vs " <<
					constantGPU.sharedLimiterAlpha[i] << std::defaultfloat << '\n';break;
			}
	}
	Check(constantMetal&&repeatedMetal&&
		constantGPU.updatedValues==constantGPURepeated.updatedValues&&
		constantGPU.faceFluxes==constantGPURepeated.faceFluxes&&
		constantGPU.sharedLimiterAlpha==constantGPURepeated.sharedLimiterAlpha&&
		SameRemapWithin(constantCPU,constantGPU,2.0e-5f),
		"Metal remap is same-device byte deterministic and matches the fp32 oracle");
	const bool openMetal=RemapFireProductionMetal(open,openGPU,&error);
	if( !openMetal ) std::cerr << "Metal open-remap detail: " << error << '\n';
	const bool wallMetal=RemapFireProductionMetal(wall,wallGPU,&error);
	Check(openMetal&&SameRemapWithin(openCPU,openGPU,2.0e-5f)&&
		RemapFireProductionMetal(openNegative,openNegativeGPU,&error)&&
		SameRemapWithin(openNegativeCPU,openNegativeGPU,2.0e-5f)&&
		wallMetal&&SameRemapWithin(wallCPU,wallGPU,2.0e-5f)&&
		RemapFireProductionMetal(wallNegative,wallNegativeGPU,&error)&&
		SameRemapWithin(wallNegativeCPU,wallNegativeGPU,2.0e-5f)&&
		RemapFireProductionMetal(wallToOpen,wallToOpenGPU,&error)&&
		SameRemapWithin(wallToOpenCPU,wallToOpenGPU,2.0e-5f)&&
		RemapFireProductionMetal(openToWall,openToWallGPU,&error)&&
		SameRemapWithin(openToWallCPU,openToWallGPU,2.0e-5f),
		"Metal symmetric and asymmetric pressure-open/wall fluxes match the fp32 oracle");
	const bool affineMetal=RemapFireProductionMetal(affine,affineGPU,&error);
	float maximumGPUAffineResidual=0.0f;
	if( affineMetal ) for( std::size_t cell=0;cell<affine.lineLength;++cell )
		maximumGPUAffineResidual=std::max(maximumGPUAffineResidual,std::fabs(
			affineGPU.updatedValues[RemapValueIndex(affine,2u,0u,cell)]-
			(affineGPU.updatedValues[RemapValueIndex(affine,0u,0u,cell)]+
			 affineGPU.updatedValues[RemapValueIndex(affine,1u,0u,cell)])));
	Check(affineMetal&&SameRemapWithin(affineCPU,affineGPU,3.0e-5f)&&
		maximumGPUAffineResidual<=3.0e-5f,
		"Metal applies one shared tuple limiter to a cancellation-sensitive affine row");
	Check(RemapFireProductionMetal(latePrefix,latePrefixGPU,&error)&&
		SameRemapWithin(latePrefixCPU,latePrefixGPU,3.0e-5f)&&
		RemapFireProductionMetal(subUlpSweep,subUlpSweepGPU,&error)&&
		subUlpSweepGPU.faceFluxes.front()>0.0f&&
		subUlpSweepGPU.faceFluxes.front()==subUlpSweepCPU.faceFluxes.front()&&
		SameRemapWithin(subUlpSweepCPU,subUlpSweepGPU,3.0e-5f)&&
		RemapFireProductionMetal(blelloch,blellochGPU,&error)&&
		blellochGPU.faceFluxes.front()==0.0f&&
		blellochGPU.faceFluxes.front()==blellochGPU.faceFluxes.back(),
		"Metal local integration and Blelloch cancellation topology match their oracle fixtures");
	FireProductionRemapRequest largeMetalRequest=PeriodicRequest(7u,1u,0x1.110bb6p+62f);
	largeMetalRequest.cellWidthM=1.0f;largeMetalRequest.timeStepS=0x1.110bb6p+62f;
	largeMetalRequest.values.assign(7u,0.1f);
	FireProductionRemapResult largeMetalCPU,largeMetalGPU;
	Check(RemapFireProductionCPU(largeMetalRequest,largeMetalCPU,&error)&&
		RemapFireProductionMetal(largeMetalRequest,largeMetalGPU,&error)&&
		SameRemapWithin(largeMetalCPU,largeMetalGPU,3.0e-5f),
		"Metal bounded quotient and remainder handle a large finite periodic Courant");
	FireProductionRemapResult finiteOverflowGPU;finiteOverflowGPU.updatedValues.push_back(4.0f);
	Check(!RemapFireProductionMetal(finiteOverflow,finiteOverflowGPU,&error)&&
		finiteOverflowGPU.updatedValues.empty()&&!error.empty(),
		"Metal rejects nonfinite derived output without publishing partial state");
	FireProductionRemapRequest smoothRequest=PeriodicRequest(64u,1u,12.8f);
	const double pi=std::acos(-1.0);
	for( std::size_t cell=0;cell<smoothRequest.lineLength;++cell ) {
		const double left=static_cast<double>(cell)/64.0;
		const double right=static_cast<double>(cell+1u)/64.0;
		smoothRequest.values[cell]=static_cast<float>(2.0+(std::cos(2.0*pi*left)-
			std::cos(2.0*pi*right))/(2.0*pi*(right-left)));
	}
	FireProductionRemapResult smoothGPURepeated;
	const bool smoothMetal=RemapFireProductionMetal(smoothRequest,smoothGPU,&error);
	const bool smoothMetalRepeated=smoothMetal&&
		RemapFireProductionMetal(smoothRequest,smoothGPURepeated,&error);
	Check(smoothMetal&&smoothMetalRepeated&&smoothGPU.updatedValues==smoothGPURepeated.updatedValues&&
		smoothGPU.faceFluxes==smoothGPURepeated.faceFluxes&&
		smoothGPU.sharedLimiterAlpha==smoothGPURepeated.sharedLimiterAlpha&&
		SameRemapWithin(smooth64Result,smoothGPU,3.0e-5f),
		"nontrivial Metal remap is byte deterministic and matches the fp32 oracle");

	FireProductionRemapRequest tier10;
	tier10.lineLength=129u;tier10.lineCount=7568u;tier10.componentCount=8u;
	tier10.cellWidthM=0.3f/128.0f;tier10.timeStepS=1.0f/480.0f;
	tier10.boundary=FireProductionRemapPeriodic;
	tier10.values.resize(tier10.componentCount*tier10.lineCount*tier10.lineLength);
	tier10.faceVelocityMPerS.resize(tier10.lineCount*(tier10.lineLength+1u));
	tier10.ambientValues.assign(tier10.componentCount,0.0f);
	for( std::size_t index=0;index<tier10.values.size();++index )
		tier10.values[index]=1.0f+static_cast<float>(index%97u)*(1.0f/256.0f);
	for( std::size_t index=0;index<tier10.faceVelocityMPerS.size();++index )
		tier10.faceVelocityMPerS[index]=0.2f+static_cast<float>(index%11u)*(1.0f/64.0f);
	for( std::size_t line=0;line<tier10.lineCount;++line )
		tier10.faceVelocityMPerS[line*(tier10.lineLength+1u)+tier10.lineLength]=
			tier10.faceVelocityMPerS[line*(tier10.lineLength+1u)];
	std::vector<double> elapsed,wallElapsed;
	FireProductionRemapResult tier10GPU;
	for( unsigned run=0;run<6u;++run ) {
		const std::chrono::steady_clock::time_point beginning=std::chrono::steady_clock::now();
		const bool remapped=RemapFireProductionMetal(tier10,tier10GPU,&error);
		const double wallMS=std::chrono::duration<double,std::milli>(
			std::chrono::steady_clock::now()-beginning).count();
		if( !remapped ) { std::cerr << "Metal tier-10 remap detail: " << error << '\n';break; }
		if( run>0u ) {elapsed.push_back(tier10GPU.deviceElapsedMS);wallElapsed.push_back(wallMS);}
	}
	std::sort(elapsed.begin(),elapsed.end());
	std::sort(wallElapsed.begin(),wallElapsed.end());
	const double p95=elapsed.empty()?std::numeric_limits<double>::infinity():elapsed.back();
	const double wallP95=wallElapsed.empty()?std::numeric_limits<double>::infinity():wallElapsed.back();
	std::cout << "Production P1 tier-10-shaped remap device_p95_ms=" << p95 <<
		" wall_p95_ms=" << wallP95 << '\n';
	Check(elapsed.size()==5u&&wallElapsed.size()==5u&&std::isfinite(p95)&&p95>0.0&&
		std::isfinite(wallP95)&&wallP95>0.0&&p95<=wallP95+1.0&&p95<=45.0&&
		wallP95<=45.0,
		"tier-10-shaped Metal remap meets the 45 ms p95 allocation");
	const std::string metalSource=ReadText("src/Library/Utilities/FireProductionComputeMac.mm");
	Check(metalSource.find("newLibraryWithSource")!=std::string::npos&&
		metalSource.find("dispatchThreads")!=std::string::npos&&
		metalSource.find("[command commit]")!=std::string::npos&&
		metalSource.find("MTLCommandBufferStatusCompleted")!=std::string::npos&&
		metalSource.find("[output contents]")!=std::string::npos,
		"Metal capability source gate binds compilation, dispatch, completion, and returned bytes");
	const std::string advectionMetalSource=ReadText(
		"src/Library/Utilities/FireProductionAdvectionMac.mm");
	const std::string makeRules=ReadText("build/make/rise/Makefile");
	const std::string xcodeProject=ReadText("build/XCode/rise/rise.xcodeproj/project.pbxproj");
	const std::string androidRules=ReadText("build/cmake/rise-android/CMakeLists.txt");
	const std::string visualStudioProject=ReadText("build/VS2022/Library/Library.vcxproj");
	Check(advectionMetalSource.find("MTLMathModeSafe")!=std::string::npos&&
		advectionMetalSource.find("MTLMathModeFast")==std::string::npos&&
		advectionMetalSource.find("fast::")==std::string::npos&&
		advectionMetalSource.find("atomic_")==std::string::npos&&
		advectionMetalSource.find("simd_")==std::string::npos&&
		advectionMetalSource.find("RemapFireProductionCPU")==std::string::npos&&
		advectionMetalSource.find("makePipeline(\"reconstruct\")")!=std::string::npos&&
		advectionMetalSource.find("makePipeline(\"scan_lines\")")!=std::string::npos&&
		advectionMetalSource.find("makePipeline(\"face_flux\")")!=std::string::npos&&
		advectionMetalSource.find("makePipeline(\"update_cells\")")!=std::string::npos&&
		advectionMetalSource.find("[command commit]")!=std::string::npos&&
		advectionMetalSource.find("MTLCommandBufferStatusCompleted")!=std::string::npos&&
		advectionMetalSource.find("[updated contents]")!=std::string::npos&&
		advectionMetalSource.find("GPUStartTime")!=std::string::npos&&
		advectionMetalSource.find("GPUEndTime")!=std::string::npos&&
		advectionMetalSource.find("activeFaces=periodic?p.n:faces")!=std::string::npos&&
		advectionMetalSource.find("p.lowerBoundary==2u")!=std::string::npos&&
		advectionMetalSource.find("p.upperBoundary==2u")!=std::string::npos&&
		advectionMetalSource.find("flux[base+p.n]=flux[base]")!=std::string::npos&&
		makeRules.find("-fno-fast-math -ffp-contract=off")!=std::string::npos&&
		CountSubstring(xcodeProject,
			"FireProductionAdvection.cpp in Sources */ = {isa = PBXBuildFile; fileRef = "
			"FA84000131FF000100000007 /* FireProductionAdvection.cpp */; settings = "
			"{COMPILER_FLAGS = \"-fno-fast-math -ffp-contract=off\"; }; }")==2u&&
		androidRules.find("-fno-fast-math;-ffp-contract=off")!=std::string::npos&&
		visualStudioProject.find("<FloatingPointModel>Strict</FloatingPointModel>")!=
			std::string::npos,
		"production remap source binds safe math, four real kernels, device output, and strict CPU builds");
#else
	Check(!capability.available&&!capability.identityKernelPassed&&capability.backend=="unavailable"&&
		!capability.structuredError.empty(),
		"non-Metal production capability reports honest unavailability");
	const std::uint32_t unsupportedInput[]={0x12345678u,0x9abcdef0u};
	std::vector<std::uint32_t> unsupportedOutput(2u,0xfeedfaceu);
	error.clear();
	Check(!RunFireProductionComputeChallenge(unsupportedInput,2u,unsupportedOutput,&error)&&
		unsupportedOutput.empty()&&!error.empty(),
		"non-Metal challenge fails explicitly without a silent CPU fallback");
	FireProductionRemapResult unsupportedRemap;unsupportedRemap.updatedValues.push_back(3.0f);
	error.clear();
	Check(!RemapFireProductionMetal(constant,unsupportedRemap,&error)&&
		unsupportedRemap.updatedValues.empty()&&!error.empty(),
		"non-Metal production remap fails explicitly without a silent CPU fallback");
#endif

	if( failures==0 ) {
		std::cout << "FireProductionSolverTest passed: table_package_id="
			<< package.TablePackageId() << "\n";
		return 0;
	}
	std::cerr << failures << " FireProductionSolverTest failure(s)\n";
	return 1;
}
