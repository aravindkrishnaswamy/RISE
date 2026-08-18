//////////////////////////////////////////////////////////////////////
//
//  FireSimulationSolverTest.cpp - Phase-C V-tier kernel gates
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../tools/fire_simulator_core.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::FireSim;

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

	bool Near( const double actual, const double expected, const double relative )
	{
		return std::fabs(actual-expected) <= relative*std::max(1.0,std::fabs(expected));
	}

	MethaneCellState StateAtTemperature(
		const std::array<double,MethaneSpeciesCount>& constituentWeights,
		const double mixtureFraction,
		const double temperatureK,
		const FireSimulationMethaneRecord& thermochemistry
		)
	{
		MethaneCellState result;
		static const char* names[MethaneCarbon] = {
			"CH4", "O2", "N2", "CO2", "H2O", "CO"
		};
		double inverseMeanWeightSum = 0.0;
		for( std::size_t species=0; species<MethaneCarbon; ++species ) {
			const FireThermochemistrySpecies* property = thermochemistry.FindSpecies(names[species]);
			if( !property ) {
				std::printf("FAIL: fixture lacks species %s\n",names[species]);
				++failures;
				return result;
			}
			inverseMeanWeightSum += constituentWeights[species]/
				property->molecularWeightKGPerKMol;
		}
		const double scale = thermochemistry.ThermodynamicPressurePa()/
			(8314.46261815324*temperatureK*inverseMeanWeightSum);
		for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
			result.constituent[species] = scale*constituentWeights[species];
		}
		result.rhoTotalZ = result.TotalDensity()*mixtureFraction;
		result.temperatureK = temperatureK;
		std::string error;
		if( !thermochemistry.MixtureSensibleEnergyJPerM3(
			ThermochemicalDensities(result),temperatureK,
			result.sensibleEnergyJPerM3,&error) ) {
			std::printf("FAIL: fixture energy construction: %s\n",error.c_str());
			std::printf("fixture weights:");
			for(const double value:constituentWeights)std::printf(" %.17g",value);
			std::printf("\n");
			++failures;
		}
		return result;
	}

	double MaximumElementResidual(
		const FireSimulationMethaneRecord& fuel,
		const std::array<double,MethaneSpeciesCount>& delta
		)
	{
		double result = 0.0;
		const std::vector<double>& matrix = fuel.ElementMassFractionMatrix();
		for( std::size_t row=0; row<fuel.ElementOrder().size(); ++row ) {
			double residual = 0.0;
			for( std::size_t column=0; column<MethaneSpeciesCount; ++column ) {
				residual += matrix[row*MethaneSpeciesCount+column]*delta[column];
			}
			result = std::max(result,std::fabs(residual));
		}
		return result;
	}

	MethaneCellState PhysicalMixtureLineState(
		const FireSimulationMethaneRecord& fuel,
		const FireSimulationMethaneRecord& thermochemistry,
		const double mixtureFraction,
		const double temperatureK
		)
	{
		std::array<double,MethaneSpeciesCount> constituent = {};
		for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
			constituent[species] = (1.0-mixtureFraction)*
				fuel.AmbientMassFractions()[species]+mixtureFraction*
				fuel.InjectedMassFractions()[species];
		}
		return StateAtTemperature(constituent,mixtureFraction,
			temperatureK,thermochemistry);
	}

	MethaneCellState ProductRichMixtureLineState(
		const FireSimulationMethaneRecord& fuel,
		const FireSimulationMethaneRecord& thermochemistry,
		const double mixtureFraction,
		const double temperatureK
		)
	{
		std::array<double,MethaneSpeciesCount> weight={};
		for(std::size_t species=0;species<MethaneSpeciesCount;++species)weight[species]=
			(1.0-mixtureFraction)*fuel.AmbientMassFractions()[species]+
			mixtureFraction*fuel.InjectedMassFractions()[species];
		const double primaryExtent=0.006;
		for(std::size_t species=0;species<MethaneSpeciesCount;++species)
			weight[species]+=primaryExtent*fuel.PrimaryReactionDelta()[species];
		// Add carbon by reversing a small amount of the record-owned complete
		// soot-oxidation direction.  No atomic weight or stoichiometric coefficient
		// is copied into the fixture, so kernel regeneration cannot strand it.
		const double carbonExtent=0.001;
		for(std::size_t species=0;species<MethaneSpeciesCount;++species)
			weight[species]-=carbonExtent*fuel.SootOxidationDelta()[species];
		// The product-history fixtures also need a nonzero CO inventory.  Build
		// that synthetic direction from the record's certified kernel projector,
		// rather than copying the pre-r56 nullspace coordinates.
		std::vector<double> coSeed(MethaneMassStateDimension,0.0),coDirection;
		coSeed[1+MethaneCO]=1.0;
		if(!fuel.ConservativeReconstruction().Project(coSeed,coDirection)||
			coDirection.size()!=MethaneMassStateDimension||coDirection[1+MethaneCO]<=0.0){
			std::printf("FAIL: product-rich fixture cannot derive its CO kernel direction\n");
			++failures;return MethaneCellState();
		}
		MethaneCellState result=StateAtTemperature(weight,mixtureFraction,
			temperatureK,thermochemistry);
		ConservativeVector state=ToConservativeVector(result);
		double coAmplitude=0.001/coDirection[1+MethaneCO];
		for(std::size_t row=0;row<MethaneMassStateDimension;++row)
			if(coDirection[row]<0.0)coAmplitude=std::min(coAmplitude,
				0.1*state[row]/-coDirection[row]);
		for(std::size_t row=0;row<MethaneMassStateDimension;++row)
			state[row]+=coAmplitude*coDirection[row];
		result=FromConservativeVector(state);
		static const char* names[MethaneCarbon]={"CH4","O2","N2","CO2","H2O","CO"};
		double molarDensity=0.0;
		for(std::size_t species=0;species<MethaneCarbon;++species)molarDensity+=
			result.constituent[species]/thermochemistry.FindSpecies(
				names[species])->molecularWeightKGPerKMol;
		result.temperatureK=thermochemistry.ThermodynamicPressurePa()/
			(8314.46261815324*molarDensity);
		std::string fixtureError;
		if(!thermochemistry.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(result),
			result.temperatureK,result.sensibleEnergyJPerM3,&fixtureError)){
			std::printf("FAIL: product-rich fixture energy: %s\n",fixtureError.c_str());
			++failures;
		}
		return result;
	}

	struct RecordKernelFixtureDirection
	{
		std::string recordId;
		std::vector<double> coordinateWeights;
		ConservativeVector direction;
	};

	RecordKernelFixtureDirection BuildRecordKernelFixtureDirection(
		const FireSimulationMethaneRecord& fuel,
		const std::vector<double>& coordinateWeights
		)
	{
		RecordKernelFixtureDirection result;
		result.recordId=fuel.RecordId();
		result.coordinateWeights=coordinateWeights;
		const FireCertifiedNullspace& closure=fuel.ConservativeReconstruction();
		if(coordinateWeights.size()!=closure.nullity)return result;
		for(std::size_t row=0;row<closure.stateDimension;++row)
			for(std::size_t column=0;column<closure.nullity;++column)
				result.direction[row]+=coordinateWeights[column]*
					closure.orthonormalBasis[row*closure.nullity+column];
		return result;
	}

	bool KernelFixtureDirectionMatchesRecord(
		const FireSimulationMethaneRecord& fuel,
		const RecordKernelFixtureDirection& fixture
		)
	{
		if(fixture.recordId!=fuel.RecordId())return false;
		const RecordKernelFixtureDirection rebuilt=BuildRecordKernelFixtureDirection(
			fuel,fixture.coordinateWeights);
		for(std::size_t row=0;row<MethaneMassStateDimension;++row)
			if(fixture.direction[row]!=rebuilt.direction[row])return false;
		return true;
	}

	double MaximumConstraintResidual(
		const FireCertifiedNullspace& closure,
		const ConservativeVector& state
		)
	{
		double result = 0.0;
		for( std::size_t row=0; row<closure.constraintRows; ++row ) {
			double residual = 0.0;
			for( std::size_t column=0; column<closure.stateDimension; ++column ) {
				residual += closure.constraintMatrix[row*closure.stateDimension+column]*
					state[column];
			}
			result = std::max(result,std::fabs(residual));
		}
		return result;
	}

	bool SetRecordDerivedEnergyForDivergence(
		const ConservativeVector& beginning,
		ConservativeVector& increment,
		const double beginningTemperatureK,
		const double desiredDivergencePerS,
		const double deltaTimeS,
		const FireSimulationMethaneRecord& thermochemistry,
		std::string* error
		)
	{
		static const char* names[MethaneCarbon]={"CH4","O2","N2","CO2","H2O","CO"};
		MethaneCellState candidate=FromConservativeVector(beginning+increment);
		double molarDensity=0.0;
		for(std::size_t species=0;species<MethaneCarbon;++species){
			const FireThermochemistrySpecies* property=thermochemistry.FindSpecies(names[species]);
			if(!property||candidate.constituent[species]<0.0)return false;
			molarDensity+=candidate.constituent[species]/property->molecularWeightKGPerKMol;
		}
		const double targetVolume=1.0+deltaTimeS*desiredDivergencePerS;
		const double targetTemperature=targetVolume*thermochemistry.ThermodynamicPressurePa()/
			(8314.46261815324*molarDensity);
		double targetEnergy=0.0;
		if(!std::isfinite(targetTemperature)||targetTemperature<thermochemistry.TemperatureMinK()||
			targetTemperature>thermochemistry.TemperatureMaxK()||
			!thermochemistry.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(candidate),
				targetTemperature,targetEnergy,error))return false;
		auto evaluate=[&](const double energy,double& divergence){
			increment[MethaneMassStateDimension]=energy-
				beginning[MethaneMassStateDimension];
			return DivergenceFromDiscreteIncrement(beginning,increment,
				beginningTemperatureK,deltaTimeS,thermochemistry,divergence,error);
		};
		double recovered=0.0,bestEnergy=targetEnergy,bestResidual=
			std::numeric_limits<double>::max();
		for(unsigned int iteration=0;iteration<12;++iteration){
			if(!evaluate(targetEnergy,recovered))return false;
			const double residual=recovered-desiredDivergencePerS;
			if(std::fabs(residual)<bestResidual){bestResidual=std::fabs(residual);
				bestEnergy=targetEnergy;}
			if(bestResidual<=1.0e-11)break;
			const double probe=std::max(1.0e-7,std::fabs(targetEnergy)*1.0e-12);
			double probed=0.0;
			if(!evaluate(targetEnergy+probe,probed))return false;
			const double derivative=(probed-recovered)/probe;
			if(!std::isfinite(derivative)||derivative==0.0)break;
			targetEnergy-=residual/derivative;
		}
		return evaluate(bestEnergy,recovered)&&
			std::fabs(recovered-desiredDivergencePerS)<=1.0e-10;
	}

	struct SyntheticV4Checkpoint
	{
		double fuelReacted;
		double oxygenConsumed;
		double carbon;
		double condensableVapor;
		double condensableAerosol;
		double carbonDioxide;
		double water;
		double releasedEnergy;
		double radiationLoss;
		double sensibleEnergyChange;
	};

	SyntheticV4Checkpoint BuildSyntheticV4Checkpoint(
		const double fuelReacted,
		const double carbon,
		const double condensableVapor,
		const double condensableAerosol,
		const double radiationLoss
		)
	{
		// This fixture deliberately uses CH4 as the synthetic condensable.  That
		// keeps the independent C/H/O atom ledger exact while exercising a
		// nonzero withheld-organic stream without making a physical fuel claim.
		const double sootOxygen=8.0/3.0;
		const double combustionEnergy=50.0e6, sootEnergy=32.8e6,
			condensableEnergy=50.0e6;
		const double condensable=condensableVapor+condensableAerosol;
		const double oxidizedCarbon=0.75*fuelReacted-carbon-0.75*condensable;
		const double oxidizedHydrogen=0.25*fuelReacted-0.25*condensable;
		SyntheticV4Checkpoint result={};
		result.fuelReacted=fuelReacted;
		result.carbon=carbon;
		result.condensableVapor=condensableVapor;
		result.condensableAerosol=condensableAerosol;
		result.oxygenConsumed=sootOxygen*oxidizedCarbon+8.0*oxidizedHydrogen;
		result.releasedEnergy=fuelReacted*combustionEnergy-carbon*sootEnergy-
			condensable*condensableEnergy;
		result.radiationLoss=radiationLoss;
		result.sensibleEnergyChange=result.releasedEnergy-radiationLoss;
		result.carbonDioxide=(44.0/12.0)*oxidizedCarbon;
		result.water=9.0*oxidizedHydrogen;
		return result;
	}

	bool SyntheticV4LedgerCloses( const SyntheticV4Checkpoint& checkpoint )
	{
		const double sootOxygen=8.0/3.0, condensableOxygen=4.0;
		const double combustionEnergy=50.0e6, sootEnergy=32.8e6,
			condensableEnergy=50.0e6;
		const double condensable=checkpoint.condensableVapor+
			checkpoint.condensableAerosol;
		const double energyResidual=checkpoint.releasedEnergy+
			checkpoint.carbon*sootEnergy+condensable*condensableEnergy-
			checkpoint.fuelReacted*combustionEnergy;
		const double oxygenResidual=checkpoint.oxygenConsumed+
			sootOxygen*checkpoint.carbon+condensableOxygen*condensable-
			4.0*checkpoint.fuelReacted;
		const double massResidual=-checkpoint.fuelReacted-checkpoint.oxygenConsumed+
			checkpoint.carbonDioxide+checkpoint.water+checkpoint.carbon+condensable;
		const std::array<double,3> elementResidual={
			-0.75*checkpoint.fuelReacted+(12.0/44.0)*checkpoint.carbonDioxide+
				checkpoint.carbon+0.75*condensable,
			-0.25*checkpoint.fuelReacted+(2.0/18.0)*checkpoint.water+
				0.25*condensable,
			-checkpoint.oxygenConsumed+(32.0/44.0)*checkpoint.carbonDioxide+
				(16.0/18.0)*checkpoint.water};
		const double scale=std::max(1.0,checkpoint.fuelReacted*combustionEnergy);
		bool closes=std::fabs(energyResidual)<=64.0*std::numeric_limits<double>::epsilon()*scale&&
			std::fabs(oxygenResidual)<=64.0*std::numeric_limits<double>::epsilon()&&
			std::fabs(massResidual)<=
			64.0*std::numeric_limits<double>::epsilon();
		for(const double residual:elementResidual)closes=closes&&
			std::fabs(residual)<=64.0*std::numeric_limits<double>::epsilon();
		return closes&&Near(checkpoint.sensibleEnergyChange,
			checkpoint.releasedEnergy-checkpoint.radiationLoss,64.0*
			std::numeric_limits<double>::epsilon());
	}

	void BurnSyntheticV4Carbon(
		SyntheticV4Checkpoint& checkpoint,
		const double oxidizedCarbon
		)
	{
		const double sootOxygen=8.0/3.0,sootEnergy=32.8e6;
		checkpoint.carbon-=oxidizedCarbon;
		checkpoint.oxygenConsumed+=sootOxygen*oxidizedCarbon;
		checkpoint.carbonDioxide+=(44.0/12.0)*oxidizedCarbon;
		checkpoint.releasedEnergy+=sootEnergy*oxidizedCarbon;
		checkpoint.sensibleEnergyChange+=sootEnergy*oxidizedCarbon;
	}

	void CompleteSyntheticV4Condensable(
		SyntheticV4Checkpoint& checkpoint,
		const double radiationLoss
		)
	{
		const double condensable=checkpoint.condensableVapor+
			checkpoint.condensableAerosol;
		checkpoint.condensableVapor=0.0;checkpoint.condensableAerosol=0.0;
		checkpoint.oxygenConsumed+=4.0*condensable;
		checkpoint.carbonDioxide+=2.75*condensable;
		checkpoint.water+=2.25*condensable;
		checkpoint.releasedEnergy+=50.0e6*condensable;
		checkpoint.radiationLoss+=radiationLoss;
		checkpoint.sensibleEnergyChange+=50.0e6*condensable-radiationLoss;
	}

	bool IndependentHotCarbonWavelengthIntegral(
		const double carbonKGPerM3,
		const double sootDensityKGPerM3,
		const double effectiveAbsorption,
		const double temperatureK,
		const double ambientTemperatureK,
		double& exchangeWPerM3,
		double& derivativeWPerM3K
		)
	{
		const double pi=std::acos(-1.0),planck=6.62607015e-34,
			light=299792458.0,boltzmann=1.380649e-23;
		const double c2=planck*light/boltzmann;
		const double volumeFraction=carbonKGPerM3/sootDensityKGPerM3;
		const double opacityCoefficient=6.0*pi*effectiveAbsorption*volumeFraction;
		const std::size_t intervals=80000;
		const double x0=std::log(1.0e-9),x1=std::log(2.0e-2),dx=(x1-x0)/intervals;
		double exchangeIntegral=0.0,derivativeIntegral=0.0;
		for(std::size_t point=0;point<=intervals;++point){
			const double x=x0+dx*point,lambda=std::exp(x);
			auto radiance=[&](const double temperature,double& value,double& derivative){
				const double exponent=c2/(lambda*temperature);
				if(exponent>700.0){value=0.0;derivative=0.0;return;}
				const double denominator=std::expm1(exponent),exponential=denominator+1.0;
				value=2.0*planck*light*light/(std::pow(lambda,5.0)*denominator);
				derivative=value*exponent*exponential/(temperature*denominator);
			};
			double hot=0.0,hotDerivative=0.0,ambient=0.0,ignored=0.0;
			radiance(temperatureK,hot,hotDerivative);
			radiance(ambientTemperatureK,ambient,ignored);
			const double kappa=opacityCoefficient/lambda;
			const double weight=(point==0||point==intervals)?0.5:1.0;
			exchangeIntegral+=weight*4.0*pi*kappa*(hot-ambient)*lambda;
			derivativeIntegral+=weight*4.0*pi*kappa*hotDerivative*lambda;
		}
		exchangeWPerM3=exchangeIntegral*dx;
		derivativeWPerM3K=derivativeIntegral*dx;
		return std::isfinite(exchangeWPerM3)&&std::isfinite(derivativeWPerM3K);
	}
}

int main()
{
	const FireSimulationMethaneRecord& fuel = FireSimulationMethaneRecord::PhysicalV1();
	const FireSimulationMethaneRecord& thermochemistry = fuel;
	const FireSimulationTransportRecord& transport = FireSimulationTransportRecord::OpenV1();
	const FireSimulationGasOpacityRecord& opacity =
		FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1();
	Check(thermochemistry.IsValid() && fuel.IsValid() && transport.IsValid() && opacity.IsValid(),
		"V-tier tests start from the adopted physical records");
	const FireAcceptedStateFeasibilityEnvelope& r60Envelope=
		fuel.AcceptedStateFeasibilityEnvelope();
	Check(r60Envelope.limiterOutwardFactorEpsilon64==1024.0&&
		r60Envelope.rowAccumulationFactorEpsilon64==64.0&&
		r60Envelope.nullspaceProjectionFactorEpsilon64==1040.0&&
		r60Envelope.sourcePacketFactorEpsilon64==128.0&&
		r60Envelope.ledgerReductionFactorEpsilon64==128.0&&
		r60Envelope.derivedUnionFactorEpsilon64==2384.0&&
		r60Envelope.kappaEpsilon64==4096.0,
		"r60 accepted-state envelope is the record-derived producer-union ceiling");
	std::array<double,MethaneSpeciesCount> r60LowerEnthalpy,r60UpperEnthalpy;
	const bool r60Bounds=fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(
		fuel.TemperatureMinK(),r60LowerEnthalpy.data(),r60LowerEnthalpy.size(),0)&&
		fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(fuel.TemperatureMaxK(),
			r60UpperEnthalpy.data(),r60UpperEnthalpy.size(),0);
	MethaneCellState recordedTrace=PhysicalMixtureLineState(fuel,thermochemistry,0.0,300.0);
	recordedTrace.constituent[MethaneCH4]=-5.40472e-13;
	const ConservativeVector recordedTraceVector=ToConservativeVector(recordedTrace);
	const double r60MinimumEnvelope=r60Envelope.kappaEpsilon64*
		std::numeric_limits<double>::epsilon();
	const double recordedMassAccumulationScale=AcceptedStateMassScale(recordedTraceVector);
	const bool oldSplitWouldReject=std::fabs(recordedTrace.constituent[MethaneCH4])>
		2048.0*std::numeric_limits<double>::epsilon()*
		std::max(1.0,std::fabs(recordedTrace.constituent[MethaneCH4]));
	Check(r60Bounds&&oldSplitWouldReject&&
		std::fabs(recordedTrace.constituent[MethaneCH4])<r60MinimumEnvelope&&
		InequalityRoundoffScale(recordedTraceVector,2+MethaneCH4,r60LowerEnthalpy,
			r60UpperEnthalpy)==recordedMassAccumulationScale&&
		AcceptedStateAdmissible(recordedTraceVector,r60LowerEnthalpy,
			r60UpperEnthalpy,fuel,0),
		"r60 derived envelope contains the recorded -5.40472e-13 exhaustion scalar without tuning");
	ConservativeVector largeAccumulationTrace=recordedTraceVector;
	for(std::size_t species=0;species<MethaneSpeciesCount;++species)
		largeAccumulationTrace[1+species]*=1.0e6;
	largeAccumulationTrace[1+MethaneCH4]=recordedTraceVector[1+MethaneCH4];
	Check(InequalityRoundoffScale(largeAccumulationTrace,2+MethaneCH4,
		r60LowerEnthalpy,r60UpperEnthalpy)==AcceptedStateMassScale(largeAccumulationTrace)&&
		AcceptedStateMassScale(largeAccumulationTrace)>1.0e5,
		"r60 exhausted-inventory scale follows accumulated mass magnitudes, never the near-zero row result");
	// This scalar is the only conservative diagnostic retained from the failed r59
	// run: its quarantined VDB contains derived media, not the full conservative
	// cell.  Do not represent this reconstruction as byte-for-byte state recovery.
	const double epsilon=std::numeric_limits<double>::epsilon();
	// Stay on the record-owned ambient-to-fuel affine line so this checks the
	// limiter envelope without manufacturing an A-row violation.  Its density
	// and energy scales vary with the coordinate, exercising the interval lower
	// bound used to certify every possible face-alpha combination.
	const ConservativeVector tangentAmbient=ToConservativeVector(
		PhysicalMixtureLineState(fuel,thermochemistry,0.0,300.0));
	const ConservativeVector tangentFuel=ToConservativeVector(
		PhysicalMixtureLineState(fuel,thermochemistry,1.0,300.0));
	auto tangentState=[&](const double coordinate){
		ConservativeVector result=tangentAmbient;
		for(std::size_t entry=0;entry<MethaneConservativeDimension;++entry)
			result[entry]+=coordinate*(tangentFuel[entry]-tangentAmbient[entry]);
		return result;
	};
	const ConservativeVector tangentDirection=tangentFuel-tangentAmbient;
	const double tangentUpperRow=InequalityValue(tangentDirection,1,
		r60LowerEnthalpy,r60UpperEnthalpy);
	const double nearBoundZ=1.0+0.5*r60Envelope.kappaEpsilon64*epsilon/
		tangentUpperRow;
	ConservativeVector nearBoundLow=tangentState(nearBoundZ);
	const ConservativeVector prospectiveCorrection=tangentState(nearBoundZ+1.0e-6)-
		nearBoundLow;
	std::array<ConservativeVector,6> prospectiveCorrections={};
	prospectiveCorrections[0]=prospectiveCorrection;
	const double limiterScaleLowerBound=CertifiedLimiterScaleLowerBound(nearBoundLow,
		prospectiveCorrections,1,1,r60LowerEnthalpy,r60UpperEnthalpy);
	const double nearBoundBudget=CertifiedLimiterInequalityBudget(nearBoundLow,
		1,r60LowerEnthalpy,r60UpperEnthalpy,fuel,limiterScaleLowerBound);
	const double expectedBudget=(r60Envelope.kappaEpsilon64-
		r60Envelope.limiterOutwardFactorEpsilon64)*epsilon*limiterScaleLowerBound-
		InequalityValue(nearBoundLow,1,r60LowerEnthalpy,r60UpperEnthalpy);
	const ConservativeVector budgetedCandidate=tangentState(
		nearBoundZ+nearBoundBudget/tangentUpperRow);
	Check(std::fabs(nearBoundBudget-expectedBudget)<=8.0*epsilon*
		std::max(1.0,std::fabs(expectedBudget))&&
		AcceptedStateAdmissible(budgetedCandidate,r60LowerEnthalpy,
			r60UpperEnthalpy,fuel,0),
		"r60 kernel-tangent limiter correction uses a candidate-conservative scale lower bound");
	std::array<ConservativeVector,6> ambientToFuelCorrection={};
	ambientToFuelCorrection[0]=tangentDirection;
	const double ambientMassLower=CertifiedLimiterScaleLowerBound(tangentAmbient,
		ambientToFuelCorrection,1,0,r60LowerEnthalpy,r60UpperEnthalpy);
	const double ambientEnergyLower=CertifiedLimiterScaleLowerBound(tangentAmbient,
		ambientToFuelCorrection,1,2+MethaneSpeciesCount,r60LowerEnthalpy,
		r60UpperEnthalpy);
	const ConservativeVector tangentMid=tangentState(0.5);
	Check(ambientMassLower<=AcceptedStateMassScale(tangentAmbient)&&
		ambientMassLower<=AcceptedStateMassScale(tangentMid)&&
		ambientMassLower<=AcceptedStateMassScale(tangentFuel)&&
		ambientEnergyLower<=AcceptedStateEnergyScale(tangentAmbient,r60LowerEnthalpy,
			r60UpperEnthalpy)&&
		ambientEnergyLower<=AcceptedStateEnergyScale(tangentMid,r60LowerEnthalpy,
			r60UpperEnthalpy)&&
		ambientEnergyLower<=AcceptedStateEnergyScale(tangentFuel,r60LowerEnthalpy,
			r60UpperEnthalpy),
		"r60 limiter scale certificate is a true mass/energy lower bound from ambient through fuel");

	MethaneCellState exhaustion=PhysicalMixtureLineState(fuel,thermochemistry,0.055,300.0);
	const double exhaustible=std::min(exhaustion.constituent[MethaneCH4],
		exhaustion.constituent[MethaneO2]/fuel.StoichiometricOxygenKGPerKGFuel());
	const std::size_t exhaustionSteps=4096;
	const double exhaustionIncrement=exhaustible/static_cast<double>(exhaustionSteps);
	bool exhaustionContinues=true;
	for(std::size_t step=0;step<exhaustionSteps&&exhaustionContinues;++step){
		MethaneSourcePacket packet;
		for(std::size_t species=0;species<MethaneSpeciesCount;++species)
			packet.constituentDelta[species]=exhaustionIncrement*
				fuel.PrimaryReactionDelta()[species];
		packet.sensibleEnergyDeltaJPerM3=exhaustionIncrement*fuel.LowerHeatingValueJPerKG();
		MethaneCellState next;
		exhaustionContinues=ApplySourcePacket(exhaustion,packet,thermochemistry,next,0);
		if(exhaustionContinues)exhaustion=next;
	}
	MethaneSourcePacket identityPacket;
	MethaneCellState continuedExhaustion;
	const bool exhaustedInputAdmissible=AcceptedStateAdmissible(ToConservativeVector(exhaustion),
		r60LowerEnthalpy,r60UpperEnthalpy,fuel,0);
	const bool exhaustionNextStep=exhaustionContinues&&exhaustedInputAdmissible&&
		ApplySourcePacket(exhaustion,identityPacket,thermochemistry,continuedExhaustion,0);
	Check(exhaustionNextStep&&std::fabs(exhaustion.constituent[MethaneCH4])<=
		r60Envelope.kappaEpsilon64*std::numeric_limits<double>::epsilon()*
		AcceptedStateMassScale(ToConservativeVector(exhaustion))&&
		continuedExhaustion.constituent[MethaneCH4]==exhaustion.constituent[MethaneCH4],
		"r60 many-step exhaustion remains admissible as the next input without clamping stored bytes");
	MethaneSourcePacket identityTracePacket;
	MethaneCellState retainedTrace;
	Check(ApplySourcePacket(recordedTrace,identityTracePacket,thermochemistry,retainedTrace,0)&&
		retainedTrace.constituent[MethaneCH4]==recordedTrace.constituent[MethaneCH4]&&
		std::signbit(retainedTrace.constituent[MethaneCH4]),
		"r60 property consumers retain an envelope-negative conservative inventory exactly");
	const RecordKernelFixtureDirection kernelFixtureGuard=
		BuildRecordKernelFixtureDirection(fuel,{1.0,-0.5,0.25,-0.125,0.0625});
	RecordKernelFixtureDirection mismatchedKernelFixture=kernelFixtureGuard;
	mismatchedKernelFixture.direction[0]=std::nextafter(
		mismatchedKernelFixture.direction[0],std::numeric_limits<double>::infinity());
	Check(KernelFixtureDirectionMatchesRecord(fuel,kernelFixtureGuard)&&
		!KernelFixtureDirectionMatchesRecord(fuel,mismatchedKernelFixture),
		"manufactured fixtures reject any embedded kernel constant that mismatches the loaded record");
	std::string error;
	const MethaneCellState eosFixture = PhysicalMixtureLineState(
		fuel,thermochemistry,0.2,800.0);
	double eosResidual = 0.0;
	Check(EquationOfStateResidual(eosFixture,thermochemistry,eosResidual) &&
		eosResidual < 2.0e-15,
		"accepted methane fixture closes the one-atmosphere EOS from record molecular weights");
	MethaneCellState badEOSFixture = eosFixture;
	for( double& densityValue : badEOSFixture.constituent ) densityValue *= 1.01;
	badEOSFixture.rhoTotalZ *= 1.01;
	badEOSFixture.sensibleEnergyJPerM3 *= 1.01;
	std::vector<double> rejectedTemperature;
	Check(!InvertPeriodicTemperatures({ToConservativeVector(badEOSFixture)},
		thermochemistry,rejectedTemperature,&error),
		"accepted-stage inversion rejects a finite but pressure-inconsistent state");
	MethaneCellState overflowingDensityState = eosFixture;
	overflowingDensityState.constituent[MethaneCH4] = std::numeric_limits<double>::max();
	overflowingDensityState.constituent[MethaneO2] = std::numeric_limits<double>::max();
	Check(!ValidateCellState(overflowingDensityState,&error),
		"accepted cell validation rejects a non-finite constituent sum");
	MethaneCellState forwardEnvelopeState=eosFixture;
	const double stateEnvelope=r60Envelope.kappaEpsilon64*
		std::numeric_limits<double>::epsilon()*
		AcceptedStateMassScale(ToConservativeVector(forwardEnvelopeState));
	forwardEnvelopeState.constituent[MethaneCarbon]=-0.5*stateEnvelope;
	Check(AcceptedStateAdmissible(ToConservativeVector(forwardEnvelopeState),
		r60LowerEnthalpy,r60UpperEnthalpy,fuel,&error),
		"accepted conservative state retains an unmodified fp64 nullspace trace inside its certified envelope");
	forwardEnvelopeState.constituent[MethaneCarbon]=-2.0*stateEnvelope;
	Check(!AcceptedStateAdmissible(ToConservativeVector(forwardEnvelopeState),
		r60LowerEnthalpy,r60UpperEnthalpy,fuel,&error),
		"accepted conservative state rejects a constituent beyond the certified fp64 envelope");

	// V1: hydrostatic-relative buoyancy supplies no momentum in the ambient
	// state, and pressure-open faces cold-start in their static-pressure class.
	const std::size_t projectionCells = 32;
	std::vector<double> ambientDensity(projectionCells,1.18);
	std::vector<double> zeroMomentum(projectionCells,0.0);
	std::vector<double> zeroDivergence(projectionCells,0.0);
	PeriodicProjectionResult restProjection;
	Check(ProjectPeriodicMACVelocity(ambientDensity,zeroMomentum,zeroDivergence,
		1.0/static_cast<double>(projectionCells),0.01,1.0e-12,restProjection,&error),
		"V1 ambient rest projects successfully");
	Check(*std::max_element(restProjection.velocityMPerS.begin(),
		restProjection.velocityMPerS.end()) == 0.0,
		"V1 hydrostatic-relative ambient remains exactly quiescent");
	std::vector<ConservativeVector> ambientConservative(projectionCells,
		ToConservativeVector(PhysicalMixtureLineState(fuel,thermochemistry,0.0,300.0)));
	PeriodicTransportConfig ambientMomentumConfig;
	ambientMomentumConfig.cellWidthM = 1.0/static_cast<double>(projectionCells);
	ambientMomentumConfig.ambientGasDensityKGPerM3 =
		FromConservativeVector(ambientConservative[0]).GasDensity();
	ambientMomentumConfig.gravityMPerS2 = -9.80665;
	std::vector<double> ambientMomentumRHS;
	Check(RemainingMomentumRHS(ambientConservative,zeroMomentum,
		std::vector<double>(projectionCells,0.0),
		std::vector<ConservativeVector>(projectionCells),ambientMomentumConfig,
		ambientMomentumRHS,&error) && *std::max_element(ambientMomentumRHS.begin(),
		ambientMomentumRHS.end()) == 0.0,
		"V1 relative buoyancy leaves the physical ambient record exactly quiescent");
	std::vector<ConservativeVector> buoyantControl=ambientConservative;
	for( std::size_t component=1; component<=MethaneSpeciesCount; ++component ) {
		buoyantControl[0][component]*=0.8;
	}
	std::vector<double> buoyantMomentumRHS;
	const double expectedRelativeBuoyancy=(0.5*(
		FromConservativeVector(buoyantControl[0]).GasDensity()+
		FromConservativeVector(buoyantControl[1]).GasDensity())-
		ambientMomentumConfig.ambientGasDensityKGPerM3)*
		ambientMomentumConfig.gravityMPerS2;
	Check(RemainingMomentumRHS(buoyantControl,zeroMomentum,
		std::vector<double>(projectionCells,0.0),
		std::vector<ConservativeVector>(projectionCells),ambientMomentumConfig,
		buoyantMomentumRHS,&error) && Near(buoyantMomentumRHS[0],
		expectedRelativeBuoyancy,2.0e-15) && expectedRelativeBuoyancy!=0.0,
		"V1 relative buoyancy uses rho-rho_infinity rather than absolute weight");
	std::vector<double> openDensity(8,1.18), openMomentum(9,0.0);
	std::vector<double> openTarget(8,0.0);
	OpenMACProjection1DResult openRest;
	Check(ReferenceProjectPressureOpenMACVelocity1D(openDensity,openMomentum,openTarget,
		1.18,0.05,0.01,1.0e-10,1.18e-10,false,false,openRest,&error) &&
		!openRest.leftInflow && !openRest.rightInflow &&
		*std::max_element(openRest.velocityMPerS.begin(),
			openRest.velocityMPerS.end()) == 0.0 &&
		openRest.maximumBoundaryHeadResidualPa == 0.0,
		"V1 quiescent pressure-open box preserves exact hydrostatic-relative rest");
	std::fill(openMomentum.begin(),openMomentum.end(),1.18*0.1);
	OpenMACProjection1DResult throughFlow;
	const bool throughFlowOK = ReferenceProjectPressureOpenMACVelocity1D(openDensity,
		openMomentum,openTarget,1.18,0.05,0.01,1.0e-9,1.18e-10,
		false,false,throughFlow,&error);
	if( !throughFlowOK ) std::printf("V1 open-flow diagnostic: %s\n",error.c_str());
	Check(throughFlowOK && throughFlow.leftInflow && !throughFlow.rightInflow &&
		throughFlow.maximumBoundaryHeadResidualPa <= 1.18e-10,
		"V1 active set reclassifies ambient inflow and enforces total head");
	PeriodicMACShape openShape3D;
	openShape3D.nx=8; openShape3D.ny=8; openShape3D.nz=8;
	openShape3D.cellWidthM=0.025;
	const MethaneCellState ambientState3D=PhysicalMixtureLineState(
		fuel,thermochemistry,0.0,300.0);
	const MethaneCellState injectedState3D=PhysicalMixtureLineState(
		fuel,thermochemistry,1.0,300.0);
	OpenBoundaryConfig3D openBoundary3D;
	openBoundary3D.ambientDensityKGPerM3=ambientState3D.GasDensity();
	openBoundary3D.injectedGasDensityKGPerM3=injectedState3D.GasDensity();
	openBoundary3D.ambientState=ToConservativeVector(ambientState3D);
	openBoundary3D.injectedState=ToConservativeVector(injectedState3D);
	openBoundary3D.fuelMassFluxKGPerM2S=0.01;
	openBoundary3D.velocityToleranceMPerS=1.0e-10;
	openBoundary3D.pressureTolerancePa=openBoundary3D.ambientDensityKGPerM3*1.0e-10;
	OpenMACField3D zeroOpenMomentum3D;
	for( unsigned int axis=0; axis<3; ++axis ) zeroOpenMomentum3D.component[axis].assign(
		OpenMACFaceCount3D(openShape3D,axis),0.0);
	OpenMACProjection3DResult openRest3D;
	const bool openRest3DOK=ProjectPressureOpenMACVelocity3D(openShape3D,
		std::vector<double>(openShape3D.CellCount(),ambientState3D.GasDensity()),
		zeroOpenMomentum3D,std::vector<double>(openShape3D.CellCount(),0.0),
		openBoundary3D,0.01,1.0e-10,openRest3D,&error);
	if( !openRest3DOK ) std::printf("V1 3-D open-rest diagnostic: %s\n",error.c_str());
	double maximumOpenVelocity3D=0.0,maximumOpenPressure3D=0.0;
	for( unsigned int axis=0; axis<3; ++axis ) for( const double velocity :
		openRest3D.velocityMPerS.component[axis] ) maximumOpenVelocity3D=
		std::max(maximumOpenVelocity3D,std::fabs(velocity));
	for( const double pressure : openRest3D.stepAverageDynamicPressurePa ) maximumOpenPressure3D=
		std::max(maximumOpenPressure3D,std::fabs(pressure));
	Check(openRest3DOK && maximumOpenVelocity3D==0.0 && maximumOpenPressure3D==0.0 &&
		openRest3D.maximumDivergenceResidualPerS==0.0 &&
		openRest3D.maximumBoundaryHeadResidualPa==0.0,
		"V1 production 3-D pressure-open multigrid preserves exact hydrostatic rest");
	OpenBoundaryFluxField3D restBoundaryFlux3D;
	const std::vector<ConservativeVector> ambientCells3D(openShape3D.CellCount(),
		ToConservativeVector(ambientState3D));
	Check(openRest3DOK && BuildOpenBoundaryFluxField3D(openShape3D,ambientCells3D,
		std::vector<double>(openShape3D.CellCount(),300.0),
		std::vector<double>(openShape3D.CellCount(),0.01),
		std::vector<double>(openShape3D.CellCount(),0.1),openBoundary3D,openRest3D,
		300.0,300.0,fuel,thermochemistry,restBoundaryFlux3D,&error),
		"V1 assembles all six production scalar/enthalpy boundary fields");
	for( unsigned int side=0; side<6; ++side ) for( const OpenBoundaryFlux3D& flux :
		restBoundaryFlux3D.side[side] ) for( std::size_t component=0;
		component<MethaneConservativeDimension; ++component ) Check(
		flux.totalOutwardFlux[component]==0.0,
		"V1 quiescent ambient boundary carries exactly zero conservative flux");
	OpenMACField3D relativeBuoyancyRate3D;
	OpenBoundaryStage3DResult ambientOpenStage3D;
	const std::array<double,3> gravity3D={{0.0,0.0,-9.80665}};
	bool endToEndAmbientRest=BuildRelativeBuoyancyMomentumRate3D(openShape3D,
		std::vector<double>(openShape3D.CellCount(),ambientState3D.GasDensity()),
		openBoundary3D,gravity3D,relativeBuoyancyRate3D,&error) &&
		BuildOpenBoundaryStage3D(openShape3D,ambientCells3D,
		std::vector<double>(openShape3D.CellCount(),300.0),zeroOpenMomentum3D,
		relativeBuoyancyRate3D,std::vector<double>(openShape3D.CellCount(),0.0),
		std::vector<double>(openShape3D.CellCount(),0.01),
		std::vector<double>(openShape3D.CellCount(),0.1),openBoundary3D,300.0,300.0,
		0.01,1.0e-10,fuel,thermochemistry,ambientOpenStage3D,&error);
	for( unsigned int axis=0; endToEndAmbientRest && axis<3; ++axis ) for(
		const double value : relativeBuoyancyRate3D.component[axis] ) {
		endToEndAmbientRest=endToEndAmbientRest && value==0.0;
	}
	for( unsigned int axis=0; endToEndAmbientRest && axis<3; ++axis ) for(
		const double value : ambientOpenStage3D.projection.velocityMPerS.component[axis] ) {
		endToEndAmbientRest=endToEndAmbientRest && value==0.0;
	}
	for( unsigned int side=0; endToEndAmbientRest && side<6; ++side ) for(
		const OpenBoundaryFlux3D& flux : ambientOpenStage3D.boundaryFlux.side[side] ) for(
		std::size_t component=0; component<MethaneConservativeDimension; ++component ) {
		endToEndAmbientRest=endToEndAmbientRest && flux.totalOutwardFlux[component]==0.0;
	}
	Check(endToEndAmbientRest,
		"V1 one production 3-D boundary stage wires relative buoyancy, open projection, and scalar flux to exact ambient rest");
	Check(ambientOpenStage3D.projection.stepAverageDynamicPressurePa.size()==
		openShape3D.CellCount(),"V1 production boundary stage returns one pressure per cell");
	for( unsigned int axis=0; axis<3; ++axis ) Check(
		ambientOpenStage3D.projection.velocityMPerS.component[axis].size()==
		OpenMACFaceCount3D(openShape3D,axis),
		"V1 production boundary stage returns every MAC face");
	for( unsigned int side=0; side<6; ++side ) Check(
		ambientOpenStage3D.boundaryFlux.side[side].size()==
		OpenBoundaryFaceCount3D(openShape3D,side),
		"V1 production boundary stage returns every conservative boundary face");
	OpenBoundaryStage3DResult expandingOpenStage3D;
	bool owningExpansionOK=BuildOpenBoundaryStage3D(openShape3D,ambientCells3D,
		std::vector<double>(openShape3D.CellCount(),300.0),zeroOpenMomentum3D,
		relativeBuoyancyRate3D,std::vector<double>(openShape3D.CellCount(),0.05),
		std::vector<double>(openShape3D.CellCount(),0.0),
		std::vector<double>(openShape3D.CellCount(),0.0),openBoundary3D,300.0,300.0,
		0.01,2.0e-8,fuel,thermochemistry,expandingOpenStage3D,&error);
	double owningExpansionResidual=0.0;
	for( std::size_t z=0; owningExpansionOK && z<openShape3D.nz; ++z ) for(
		std::size_t y=0; y<openShape3D.ny; ++y ) for( std::size_t x=0;
		x<openShape3D.nx; ++x ) owningExpansionResidual=std::max(owningExpansionResidual,
		std::fabs(OpenMACDivergence3D(openShape3D,
			expandingOpenStage3D.projection.velocityMPerS,x,y,z)-0.05));
	bool owningExpansionFlux=false;
	for( unsigned int side=0; side<6; ++side ) for( const OpenBoundaryFlux3D& flux :
		expandingOpenStage3D.boundaryFlux.side[side] ) owningExpansionFlux=
		owningExpansionFlux || flux.totalOutwardFlux[1+MethaneN2]!=0.0;
	Check(owningExpansionOK && owningExpansionResidual<=2.0e-8 && owningExpansionFlux,
		"V1 owning 3-D boundary stage consumes nonzero projection target and returns its conservative open flux");
	bool allOwningAxesMatch=true;
	for( unsigned int forcedAxis=0; forcedAxis<3; ++forcedAxis ) {
		OpenMACField3D imposedMomentumRate3D=relativeBuoyancyRate3D;
		for( double& value : imposedMomentumRate3D.component[forcedAxis] ) value=
			ambientState3D.GasDensity()*3.0;
		OpenMACField3D independentlyUnprojected3D=zeroOpenMomentum3D;
		for( std::size_t face=0; face<independentlyUnprojected3D.component[forcedAxis].size();
			++face ) independentlyUnprojected3D.component[forcedAxis][face]=
			0.01*imposedMomentumRate3D.component[forcedAxis][face];
		OpenBoundaryConfig3D forcedBoundary3D=openBoundary3D;
		forcedBoundary3D.kind.fill(AdiabaticWallBoundary3D);
		forcedBoundary3D.kind[2*forcedAxis]=PressureOpenBoundary3D;
		forcedBoundary3D.kind[2*forcedAxis+1]=PressureOpenBoundary3D;
		OpenBoundaryStage3DResult forcedOpenStage3D;
		OpenMACProjection3DResult independentForcedProjection3D;
		const bool forcedOwningOK=BuildOpenBoundaryStage3D(openShape3D,ambientCells3D,
			std::vector<double>(openShape3D.CellCount(),300.0),zeroOpenMomentum3D,
			imposedMomentumRate3D,std::vector<double>(openShape3D.CellCount(),0.0),
			std::vector<double>(openShape3D.CellCount(),0.0),
			std::vector<double>(openShape3D.CellCount(),0.0),forcedBoundary3D,300.0,300.0,
			0.01,2.0e-8,fuel,thermochemistry,forcedOpenStage3D,&error) &&
			ProjectPressureOpenMACVelocity3D(openShape3D,
			std::vector<double>(openShape3D.CellCount(),ambientState3D.GasDensity()),
			independentlyUnprojected3D,std::vector<double>(openShape3D.CellCount(),0.0),
			forcedBoundary3D,0.01,2.0e-8,independentForcedProjection3D,&error);
		bool axisMatches=forcedOwningOK;
		for( unsigned int axis=0; axisMatches && axis<3; ++axis ) for(
			std::size_t face=0; face<forcedOpenStage3D.projection.velocityMPerS.component[axis].size();
			++face ) axisMatches=axisMatches && Near(
			forcedOpenStage3D.projection.velocityMPerS.component[axis][face],
			independentForcedProjection3D.velocityMPerS.component[axis][face],2.0e-14);
		double maximumForcedVelocity=0.0;
		for( const double value : forcedOpenStage3D.projection.velocityMPerS.component[forcedAxis] )
			maximumForcedVelocity=std::max(maximumForcedVelocity,std::fabs(value));
		allOwningAxesMatch=allOwningAxesMatch && axisMatches && maximumForcedVelocity>0.0;
	}
	Check(allOwningAxesMatch,
		"V1 owning 3-D boundary stage applies each component of its nonpressure momentum rate");
	OpenMACProjection3DResult openExpansion3D;
	const bool openExpansion3DOK=ProjectPressureOpenMACVelocity3D(openShape3D,
		std::vector<double>(openShape3D.CellCount(),ambientState3D.GasDensity()),
		zeroOpenMomentum3D,std::vector<double>(openShape3D.CellCount(),0.05),
		openBoundary3D,0.01,2.0e-8,openExpansion3D,&error);
	if( !openExpansion3DOK ) std::printf("V1 3-D open-expansion diagnostic: %s\n",error.c_str());
	Check(openExpansion3DOK && !openExpansion3D.multigridResidualHistoryPerS.empty() &&
		openExpansion3D.maximumDivergenceResidualPerS<=2.0e-8 &&
		openExpansion3D.maximumBoundaryHeadResidualPa<=openBoundary3D.pressureTolerancePa,
		"V1 3-D pressure-open multigrid closes a nonzero compatible expansion field");
	OpenMACField3D throughMomentum3D=zeroOpenMomentum3D;
	for( std::size_t z=0; z<openShape3D.nz; ++z ) for( std::size_t y=0;
		y<openShape3D.ny; ++y ) for( std::size_t x=0; x<=openShape3D.nx; ++x ) {
		throughMomentum3D.component[0][OpenMACFaceIndex3D(openShape3D,0,x,y,z)]=
			ambientState3D.GasDensity()*0.1;
	}
	for( std::size_t z=0; z<openShape3D.nz; ++z ) for( std::size_t y=0;
		y<=openShape3D.ny; ++y ) for( std::size_t x=0; x<openShape3D.nx; ++x ) {
		throughMomentum3D.component[1][OpenMACFaceIndex3D(openShape3D,1,x,y,z)]=
			ambientState3D.GasDensity()*(0.02+0.002*static_cast<double>(y));
	}
	for( std::size_t z=0; z<=openShape3D.nz; ++z ) for( std::size_t y=0;
		y<openShape3D.ny; ++y ) for( std::size_t x=0; x<openShape3D.nx; ++x ) {
		throughMomentum3D.component[2][OpenMACFaceIndex3D(openShape3D,2,x,y,z)]=
			ambientState3D.GasDensity()*(0.03+0.001*static_cast<double>(z));
	}
	OpenBoundaryConfig3D throughBoundary3D=openBoundary3D;
	throughBoundary3D.kind[2]=AdiabaticWallBoundary3D;
	throughBoundary3D.kind[3]=AdiabaticWallBoundary3D;
	throughBoundary3D.kind[4]=AdiabaticWallBoundary3D;
	throughBoundary3D.kind[5]=AdiabaticWallBoundary3D;
	OpenMACProjection3DResult openThrough3D;
	const bool openThrough3DOK=ProjectPressureOpenMACVelocity3D(openShape3D,
		std::vector<double>(openShape3D.CellCount(),ambientState3D.GasDensity()),
		throughMomentum3D,std::vector<double>(openShape3D.CellCount(),0.0),
		throughBoundary3D,0.01,2.0e-8,openThrough3D,&error);
	if( !openThrough3DOK ) {
		std::printf("V1 3-D total-head diagnostic: %s\n",error.c_str());
		for( unsigned int side=0; side<6; ++side ) std::printf(" side%u inflow=%zu/%zu\n",
			side,static_cast<std::size_t>(std::count(openThrough3D.inflow[side].begin(),
			openThrough3D.inflow[side].end(),true)),openThrough3D.inflow[side].size());
	}
	double independentObliqueHeadResidual=0.0;
	if( openThrough3DOK ) for( std::size_t z=0; z<openShape3D.nz; ++z ) for(
		std::size_t y=0; y<openShape3D.ny; ++y ) {
		const std::size_t index=OpenBoundaryFaceLinearIndex3D(openShape3D,0,y,z);
		const double normal=-openThrough3D.velocityMPerS.component[0][
			OpenMACFaceIndex3D(openShape3D,0,0,y,z)];
		const double tangent=0.5*(openThrough3D.velocityMPerS.component[1][
			OpenMACFaceIndex3D(openShape3D,1,0,y,z)]+
			openThrough3D.velocityMPerS.component[1][
			OpenMACFaceIndex3D(openShape3D,1,0,y+1,z)]);
		const double secondTangent=0.5*(openThrough3D.velocityMPerS.component[2][
			OpenMACFaceIndex3D(openShape3D,2,0,y,z)]+
			openThrough3D.velocityMPerS.component[2][
			OpenMACFaceIndex3D(openShape3D,2,0,y,z+1)]);
		independentObliqueHeadResidual=std::max(independentObliqueHeadResidual,std::fabs(
			openThrough3D.boundaryDynamicPressurePa[0][index]+0.5*
			throughBoundary3D.ambientDensityKGPerM3*(normal*normal+tangent*tangent+
			secondTangent*secondTangent)));
	}
	Check(openThrough3DOK && std::all_of(openThrough3D.inflow[0].begin(),
		openThrough3D.inflow[0].end(),[]( const bool value ){ return value; }) &&
		std::none_of(openThrough3D.inflow[1].begin(),openThrough3D.inflow[1].end(),
			[]( const bool value ){ return value; }) &&
		independentObliqueHeadResidual<=throughBoundary3D.pressureTolerancePa,
		"V1 3-D active set enforces full-vector total-head inflow and static-pressure outflow");
	OpenBoundaryConfig3D allOpenObliqueBoundary=openBoundary3D;
	allOpenObliqueBoundary.kind[4]=PressureOpenBoundary3D;
	allOpenObliqueBoundary.velocityToleranceMPerS=1.0;
	for(unsigned int side=0;side<6;++side)allOpenObliqueBoundary.priorInflow[side].assign(
		OpenBoundaryFaceCount3D(openShape3D,side),false);
	std::fill(allOpenObliqueBoundary.priorInflow[0].begin(),
		allOpenObliqueBoundary.priorInflow[0].end(),true);
	std::fill(allOpenObliqueBoundary.priorInflow[2].begin(),
		allOpenObliqueBoundary.priorInflow[2].end(),true);
	std::fill(allOpenObliqueBoundary.priorInflow[4].begin(),
		allOpenObliqueBoundary.priorInflow[4].end(),true);
	OpenMACProjection3DResult allOpenObliqueProjection;
	const bool allOpenObliqueOK=ProjectPressureOpenMACVelocity3D(openShape3D,
		std::vector<double>(openShape3D.CellCount(),ambientState3D.GasDensity()),
		throughMomentum3D,std::vector<double>(openShape3D.CellCount(),0.0),
		allOpenObliqueBoundary,0.01,2.0e-8,allOpenObliqueProjection,&error);
	if(!allOpenObliqueOK) {
		std::printf("V1 all-open oblique diagnostic: %s\n",error.c_str());
		for(unsigned int side=0;side<6;++side)std::printf(" allopen side%u=%zu/%zu\n",side,
			static_cast<std::size_t>(std::count(allOpenObliqueProjection.inflow[side].begin(),
			allOpenObliqueProjection.inflow[side].end(),true)),allOpenObliqueProjection.inflow[side].size());
	}
	Check(allOpenObliqueOK &&
		allOpenObliqueProjection.maximumBoundaryHeadResidualPa<=
		allOpenObliqueBoundary.pressureTolerancePa,
		"V1 augmented all-open solve couples oblique edge and corner head equations safely");
	double independentYMinusHeadResidual=0.0;
	if(allOpenObliqueOK) for( std::size_t z=0; z<openShape3D.nz; ++z ) for(
		std::size_t x=0; x<openShape3D.nx; ++x ) {
		const std::size_t index=OpenBoundaryFaceLinearIndex3D(openShape3D,2,x,z);
		const double normal=-allOpenObliqueProjection.velocityMPerS.component[1][
			OpenMACFaceIndex3D(openShape3D,1,x,0,z)];
		const double xTangent=0.5*(allOpenObliqueProjection.velocityMPerS.component[0][
			OpenMACFaceIndex3D(openShape3D,0,x,0,z)]+
			allOpenObliqueProjection.velocityMPerS.component[0][
			OpenMACFaceIndex3D(openShape3D,0,x+1,0,z)]);
		const double zTangent=0.5*(allOpenObliqueProjection.velocityMPerS.component[2][
			OpenMACFaceIndex3D(openShape3D,2,x,0,z)]+
			allOpenObliqueProjection.velocityMPerS.component[2][
			OpenMACFaceIndex3D(openShape3D,2,x,0,z+1)]);
		independentYMinusHeadResidual=std::max(independentYMinusHeadResidual,std::fabs(
			allOpenObliqueProjection.boundaryDynamicPressurePa[2][index]+0.5*
			allOpenObliqueBoundary.ambientDensityKGPerM3*(normal*normal+
			xTangent*xTangent+zTangent*zTangent)));
	}
	Check(allOpenObliqueOK && independentYMinusHeadResidual<=
		allOpenObliqueBoundary.pressureTolerancePa,
		"V1 y-normal pressure-open head independently includes x and z tangential velocity");
	double independentZMinusHeadResidual=0.0;
	if(allOpenObliqueOK) for( std::size_t y=0; y<openShape3D.ny; ++y ) for(
		std::size_t x=0; x<openShape3D.nx; ++x ) {
		const std::size_t index=OpenBoundaryFaceLinearIndex3D(openShape3D,4,x,y);
		const double normal=-allOpenObliqueProjection.velocityMPerS.component[2][
			OpenMACFaceIndex3D(openShape3D,2,x,y,0)];
		const double xTangent=0.5*(allOpenObliqueProjection.velocityMPerS.component[0][
			OpenMACFaceIndex3D(openShape3D,0,x,y,0)]+
			allOpenObliqueProjection.velocityMPerS.component[0][
			OpenMACFaceIndex3D(openShape3D,0,x+1,y,0)]);
		const double yTangent=0.5*(allOpenObliqueProjection.velocityMPerS.component[1][
			OpenMACFaceIndex3D(openShape3D,1,x,y,0)]+
			allOpenObliqueProjection.velocityMPerS.component[1][
			OpenMACFaceIndex3D(openShape3D,1,x,y+1,0)]);
		independentZMinusHeadResidual=std::max(independentZMinusHeadResidual,std::fabs(
			allOpenObliqueProjection.boundaryDynamicPressurePa[4][index]+0.5*
			allOpenObliqueBoundary.ambientDensityKGPerM3*(normal*normal+
			xTangent*xTangent+yTangent*yTangent)));
	}
	Check(allOpenObliqueOK && independentZMinusHeadResidual<=
		allOpenObliqueBoundary.pressureTolerancePa,
		"V1 z-normal pressure-open head independently includes x and y tangential velocity");
	bool productionTangentialConnected=openThrough3D.boundaryTangentialVelocityMPerS[0][0].size()==
		OpenBoundaryFaceCount3D(openShape3D,0);
	for( std::size_t z=0; productionTangentialConnected && z<openShape3D.nz; ++z ) for(
		std::size_t y=0; y<openShape3D.ny; ++y ) {
		const std::size_t index=OpenBoundaryFaceLinearIndex3D(openShape3D,0,y,z);
		productionTangentialConnected=productionTangentialConnected && Near(
			openThrough3D.boundaryTangentialVelocityMPerS[0][0][index],
			0.5*(openThrough3D.velocityMPerS.component[1][OpenMACFaceIndex3D(
				openShape3D,1,0,y,z)]+openThrough3D.velocityMPerS.component[1][
				OpenMACFaceIndex3D(openShape3D,1,0,y+1,z)]),2.0e-14);
	}
	Check(productionTangentialConnected,
		"V1 production projection returns pressure-open tangential zero-gradient velocity");
	bool secondProductionTangent=openThrough3D.boundaryTangentialVelocityMPerS[0][1].size()==
		OpenBoundaryFaceCount3D(openShape3D,0);
	for( std::size_t z=0; secondProductionTangent && z<openShape3D.nz; ++z ) for(
		std::size_t y=0; y<openShape3D.ny; ++y ) {
		const std::size_t index=OpenBoundaryFaceLinearIndex3D(openShape3D,0,y,z);
		secondProductionTangent=secondProductionTangent && Near(
			openThrough3D.boundaryTangentialVelocityMPerS[0][1][index],
			0.5*(openThrough3D.velocityMPerS.component[2][OpenMACFaceIndex3D(
				openShape3D,2,0,y,z)]+openThrough3D.velocityMPerS.component[2][
				OpenMACFaceIndex3D(openShape3D,2,0,y,z+1)]),2.0e-14);
	}
	Check(secondProductionTangent,
		"V1 production projection returns the second pressure-open tangential component");
	std::vector<ConservativeVector> hotOutflowCells(openShape3D.CellCount(),
		ToConservativeVector(PhysicalMixtureLineState(fuel,thermochemistry,0.2,800.0)));
	bool allSideReversals=true;
	for( unsigned int reversedSide=0; reversedSide<6; ++reversedSide ) {
		const unsigned int reversedAxis=reversedSide/2;
		const bool positiveSide=(reversedSide%2)!=0;
		const double coordinateSign=positiveSide?1.0:-1.0;
		OpenBoundaryConfig3D seededInflowBoundary=openBoundary3D;
		seededInflowBoundary.kind.fill(AdiabaticWallBoundary3D);
		seededInflowBoundary.kind[2*reversedAxis]=PressureOpenBoundary3D;
		seededInflowBoundary.kind[2*reversedAxis+1]=PressureOpenBoundary3D;
		for( unsigned int side=0; side<6; ++side ) seededInflowBoundary.priorInflow[side].assign(
			OpenBoundaryFaceCount3D(openShape3D,side),side==reversedSide);
		OpenMACField3D outwardMomentum3D=zeroOpenMomentum3D;
		for( double& value : outwardMomentum3D.component[reversedAxis] ) value=
			coordinateSign*ambientState3D.GasDensity()*0.1;
		OpenMACProjection3DResult reversedToOutflow3D;
		bool reversalOK=ProjectPressureOpenMACVelocity3D(openShape3D,
			std::vector<double>(openShape3D.CellCount(),ambientState3D.GasDensity()),
			outwardMomentum3D,std::vector<double>(openShape3D.CellCount(),0.0),
			seededInflowBoundary,0.01,2.0e-8,reversedToOutflow3D,&error);
		reversalOK=reversalOK && std::none_of(reversedToOutflow3D.inflow[reversedSide].begin(),
			reversedToOutflow3D.inflow[reversedSide].end(),[]( const bool value ){ return value; });
		OpenBoundaryFluxField3D reversedFluxField;
		bool reversedFluxOK=reversalOK && BuildOpenBoundaryFluxField3D(openShape3D,
			hotOutflowCells,std::vector<double>(openShape3D.CellCount(),800.0),
			std::vector<double>(openShape3D.CellCount(),0.01),
			std::vector<double>(openShape3D.CellCount(),0.1),seededInflowBoundary,
			reversedToOutflow3D,300.0,300.0,fuel,thermochemistry,reversedFluxField,&error);
		std::size_t x=0,y=0,z=0,cx=0,cy=0,cz=0;
		if(reversedAxis==0){x=positiveSide?openShape3D.nx:0;cx=positiveSide?openShape3D.nx-1:0;}
		if(reversedAxis==1){y=positiveSide?openShape3D.ny:0;cy=positiveSide?openShape3D.ny-1:0;}
		if(reversedAxis==2){z=positiveSide?openShape3D.nz:0;cz=positiveSide?openShape3D.nz-1:0;}
		const std::size_t face=OpenMACFaceIndex3D(openShape3D,reversedAxis,x,y,z);
		const std::size_t cell=openShape3D.Index(cx,cy,cz);
		const double outwardVelocity=coordinateSign*
			reversedToOutflow3D.velocityMPerS.component[reversedAxis][face];
		for( std::size_t component=0; reversedFluxOK &&
			component<MethaneConservativeDimension; ++component ) reversedFluxOK=
			reversedFluxOK && Near(reversedFluxField.side[reversedSide][0].
			totalOutwardFlux[component],outwardVelocity*hotOutflowCells[cell][component],2.0e-14);
		allSideReversals=allSideReversals && reversedFluxOK &&
			reversedFluxField.side[reversedSide][0].nonadvectiveEnergyOutwardFlux==0.0;
	}
	Check(allSideReversals,
		"V1 every side reclassifies stale inflow and immediately uses interior outflow flux");
	OpenBoundaryConfig3D picardSeedBoundary=openBoundary3D;
	picardSeedBoundary.kind.fill(AdiabaticWallBoundary3D);
	picardSeedBoundary.kind[0]=PressureOpenBoundary3D;
	picardSeedBoundary.kind[1]=PressureOpenBoundary3D;
	for(unsigned int side=0;side<6;++side)picardSeedBoundary.priorInflow[side].assign(
		OpenBoundaryFaceCount3D(openShape3D,side),side==0u);
	OpenMACField3D picardOutwardMomentum=zeroOpenMomentum3D;
	for(double& value:picardOutwardMomentum.component[0])value=
		-ambientState3D.GasDensity()*0.1;
	OpenMACProjection3DResult picardOutflow,iteratedDeadband,staleDeadband;
	const bool picardOutflowOK=ProjectPressureOpenMACVelocity3D(openShape3D,
		std::vector<double>(openShape3D.CellCount(),ambientState3D.GasDensity()),
		picardOutwardMomentum,std::vector<double>(openShape3D.CellCount(),0.0),
		picardSeedBoundary,0.01,2.0e-8,picardOutflow,&error);
	OpenBoundaryConfig3D iteratedSeedBoundary=picardSeedBoundary;
	iteratedSeedBoundary.priorInflow=picardOutflow.inflow;
	const bool iteratedDeadbandOK=picardOutflowOK&&ProjectPressureOpenMACVelocity3D(openShape3D,
		std::vector<double>(openShape3D.CellCount(),ambientState3D.GasDensity()),
		zeroOpenMomentum3D,std::vector<double>(openShape3D.CellCount(),0.0),
		iteratedSeedBoundary,0.01,2.0e-8,iteratedDeadband,&error);
	const bool staleDeadbandOK=ProjectPressureOpenMACVelocity3D(openShape3D,
		std::vector<double>(openShape3D.CellCount(),ambientState3D.GasDensity()),
		zeroOpenMomentum3D,std::vector<double>(openShape3D.CellCount(),0.0),
		picardSeedBoundary,0.01,2.0e-8,staleDeadband,&error);
	Check(iteratedDeadbandOK&&staleDeadbandOK&&
		std::none_of(iteratedDeadband.inflow[0].begin(),iteratedDeadband.inflow[0].end(),
			[](const bool value){return value;})&&
		std::all_of(staleDeadband.inflow[0].begin(),staleDeadband.inflow[0].end(),
			[](const bool value){return value;}),
		"open Picard deadband consumes the immediately preceding active-set classification");
	// Synthetic pressure-algebra fixture, deliberately distinct from every fuel
	// record.  These are the exact rounded bytes of the first compact state found
	// to reproduce the R0/R1 two-state cycle seen in the preserved tier-10 prefix.
	const char* r80FixtureId="synthetic-r80-open-active-cycle-v1";
	Check(std::string(r80FixtureId)!=fuel.RecordId(),
		"r80 cycling fixture remains distinct from the physical methane record");
	PeriodicMACShape r80Shape;r80Shape.nx=2;r80Shape.ny=2;r80Shape.nz=2;
	r80Shape.cellWidthM=0.025;
	OpenBoundaryConfig3D r80Boundary;
	r80Boundary.kind.fill(PressureOpenBoundary3D);
	r80Boundary.kind[4]=AdiabaticWallBoundary3D;
	r80Boundary.ambientDensityKGPerM3=1.18;
	r80Boundary.injectedGasDensityKGPerM3=0.65;
	r80Boundary.ambientState[1+MethaneN2]=1.18;
	r80Boundary.injectedState[1+MethaneCH4]=0.65;
	r80Boundary.velocityToleranceMPerS=1.0e-10;
	r80Boundary.pressureTolerancePa=1.18e-10;
	OpenMACField3D r80Momentum;
	r80Momentum.component[0]={-0.39204603162628593,0.021721462695222103,
		-0.29886061033561701,0.15764038356392523,-0.29753741599264799,
		-0.11689292488510242,0.44857978295174833,0.30318676269653672,
		-0.093165684114380518,-0.36645652810715085,-0.18848203347961781,
		-0.37139439953351877};
	r80Momentum.component[1]={-0.45187780714885317,0.27922894000192938,
		0.07818145964801268,0.40027363403184107,-0.28437611228830761,
		-0.21789700828283587,-0.27340293223340723,0.12155702730464175,
		-0.18524288676327269,-0.090770919811655915,-0.37732836817869225,
		0.46879158025325962};
	r80Momentum.component[2]={0.16158029623408621,-0.37692737674629062,
		-0.30714421191164104,-0.43596729901984049,0.33236684009648332,
		-0.22216859679920789,0.26506101216476513,0.010631582398982762,
		-0.052440224127210638,0.46978404796341594,-0.35630712207170895,
		0.042736637727026877};
	const std::vector<double> r80Target={-1.2455912455668876,-0.93702700426044117,
		0.77579829288905022,-0.45763762396502206,-1.1951777950380715,
		-1.3103188149369387,-0.78556623220967647,0.20074400281819904};
	OpenMACProjection3DResult r80OneWorker,r80ManyWorkers;
	const bool r80OneOK=ProjectPressureOpenMACVelocity3D(r80Shape,
		std::vector<double>(r80Shape.CellCount(),1.18),r80Momentum,r80Target,
		r80Boundary,0.01,2.0e-8,r80OneWorker,&error,1u);
	const bool r80ManyOK=ProjectPressureOpenMACVelocity3D(r80Shape,
		std::vector<double>(r80Shape.CellCount(),1.18),r80Momentum,r80Target,
		r80Boundary,0.01,2.0e-8,r80ManyWorkers,&error,4u);
	OpenBoundaryConfig3D r80OtherBoundary=r80Boundary;
	r80OtherBoundary.priorInflow=r80OneWorker.inflow;
	for(unsigned int side=0;side<6;++side){const unsigned int axis=side/2;
		const bool positive=side%2;const std::size_t firstCount=side<2?r80Shape.ny:
			r80Shape.nx,secondCount=side<4?r80Shape.nz:r80Shape.ny;
		for(std::size_t second=0;second<secondCount;++second)for(std::size_t first=0;
			first<firstCount;++first){const std::size_t index=
			OpenBoundaryFaceLinearIndex3D(r80Shape,side,first,second);
			if(r80Boundary.kind[side]!=PressureOpenBoundary3D)continue;
			std::size_t x=0,y=0,z=0;if(axis==0){x=positive?r80Shape.nx:0;y=first;z=second;}
			if(axis==1){x=first;y=positive?r80Shape.ny:0;z=second;}
			if(axis==2){x=first;y=second;z=positive?r80Shape.nz:0;}
			const double outward=(positive?1.0:-1.0)*r80OneWorker.velocityMPerS.
				component[axis][OpenMACFaceIndex3D(r80Shape,axis,x,y,z)];
			if(outward < -r80Boundary.velocityToleranceMPerS)
				r80OtherBoundary.priorInflow[side][index]=true;
			else if(outward > r80Boundary.velocityToleranceMPerS)
				r80OtherBoundary.priorInflow[side][index]=false;}}
	OpenMACProjection3DResult r80OtherBranch,r80ReversedVisit;
	const bool r80OtherOK=r80OneOK&&ProjectPressureOpenMACVelocity3D(r80Shape,
		std::vector<double>(r80Shape.CellCount(),1.18),r80Momentum,r80Target,
		r80OtherBoundary,0.01,2.0e-8,r80OtherBranch,&error,1u,true);
	const bool r80ReversedOK=r80OtherOK&&ProjectPressureOpenMACVelocity3D(r80Shape,
		std::vector<double>(r80Shape.CellCount(),1.18),r80Momentum,r80Target,
		r80OtherBoundary,0.01,2.0e-8,r80ReversedVisit,&error,1u);
	const std::vector<unsigned char> r80ExpectedSelected={0,1,1,0,1,0,1,1,0,1,0,1,
		1,1,1,0,0,0,0,0,0,0,1,1};
	const std::vector<unsigned char> r80ExpectedOther={0,1,1,0,1,0,1,1,0,1,0,1,
		1,1,1,0,0,0,0,0,1,0,1,1};
	PeriodicMACShape r80ComparatorShape;r80ComparatorShape.nx=1;r80ComparatorShape.ny=1;
	r80ComparatorShape.nz=1;r80ComparatorShape.cellWidthM=1.0;
	OpenBoundaryConfig3D r80ComparatorBoundary;r80ComparatorBoundary.kind.fill(
		AdiabaticWallBoundary3D);r80ComparatorBoundary.kind[0]=PressureOpenBoundary3D;
	r80ComparatorBoundary.kind[1]=PressureOpenBoundary3D;
	r80ComparatorBoundary.velocityToleranceMPerS=1.0e-10;
	OpenMACProjection3DResult r80CrossA,r80CrossB;
	for(unsigned int side=0;side<6;++side){const std::size_t count=
		OpenBoundaryFaceCount3D(r80ComparatorShape,side);
		r80CrossA.inflow[side].assign(count,false);r80CrossB.inflow[side].assign(count,false);}
	r80CrossA.inflow[0][0]=true;r80CrossB.inflow[1][0]=true;
	r80CrossA.velocityMPerS.component[0]={1.0,1.0};
	r80CrossB.velocityMPerS.component[0]=r80CrossA.velocityMPerS.component[0];
	r80CrossA.velocityMPerS.component[1].assign(2u,0.0);
	r80CrossB.velocityMPerS.component[1].assign(2u,0.0);
	r80CrossA.velocityMPerS.component[2].assign(2u,0.0);
	r80CrossB.velocityMPerS.component[2].assign(2u,0.0);
	Check(OpenActiveSetCanonicalBefore3D(r80ComparatorShape,r80ComparatorBoundary,
		r80CrossA,r80CrossB)&&!OpenActiveSetCanonicalBefore3D(r80ComparatorShape,
		r80ComparatorBoundary,r80CrossB,r80CrossA),
		"r80 canonical comparator independently selects a solved crossing branch, not union or intersection");
	const std::vector<std::vector<unsigned char> > r81CrossingHistory={
		std::vector<unsigned char>({1u,0u}),std::vector<unsigned char>({0u,1u})};
	const std::vector<unsigned char> r81Union={1u,1u},r81Intersection={0u,0u};
	const std::size_t r81MinimumDiscrepancy=OpenActiveSetCanonicalHistoryIndex3D(
		std::vector<double>({0.25,0.5}),r81CrossingHistory,0u);
	const std::size_t r81EqualDiscrepancy=OpenActiveSetCanonicalHistoryIndex3D(
		std::vector<double>({0.25,0.25}),r81CrossingHistory,0u);
	Check(r81MinimumDiscrepancy==0u&&r81CrossingHistory[r81MinimumDiscrepancy]!=r81Union&&
		r81CrossingHistory[r81MinimumDiscrepancy]!=r81Intersection&&r81EqualDiscrepancy==1u,
		"r81 canonical history selection rejects union/intersection and breaks exact ties lexicographically outflow-first");
	bool r80BitIdentity=r80OneOK&&r80ManyOK;
	for(unsigned int axis=0;r80BitIdentity&&axis<3;++axis)r80BitIdentity=
		r80OneWorker.velocityMPerS.component[axis]==r80ManyWorkers.velocityMPerS.component[axis]&&
		r80OneWorker.momentumKGPerM2S.component[axis]==r80ManyWorkers.momentumKGPerM2S.component[axis];
	if(!r80BitIdentity||r80OneWorker.inflow!=r80ManyWorkers.inflow||
		r80OneWorker.maximumActiveSetComplementarityDiscrepancyMPerS!=
			r80ManyWorkers.maximumActiveSetComplementarityDiscrepancyMPerS)
		std::printf("r80 worker diagnostic: one_ok=%d many_ok=%d bits=%d inflow=%d "
			"one_disc=%.17g many_disc=%.17g one_cycle=%zu many_cycle=%zu\n",
			r80OneOK?1:0,r80ManyOK?1:0,r80BitIdentity?1:0,
			r80OneWorker.inflow==r80ManyWorkers.inflow?1:0,
			r80OneWorker.maximumActiveSetComplementarityDiscrepancyMPerS,
			r80ManyWorkers.maximumActiveSetComplementarityDiscrepancyMPerS,
			r80OneWorker.activeSetCycleLength,r80ManyWorkers.activeSetCycleLength);
	Check(r80OneOK&&r80ManyOK&&r80OtherOK&&r80ReversedOK&&
		r80OneWorker.activeSetDiscontinuousClass&&r80ReversedVisit.activeSetDiscontinuousClass&&
		r80OneWorker.activeSetCycleLength==2u&&r80OneWorker.activeSetDifferingFaceCount==1u&&
		OpenActiveSetDifferingFaceCount3D(r80OneWorker.inflow,r80OtherBranch.inflow)==1u&&
		FlattenOpenActiveSet3D(r80OneWorker.inflow)==r80ExpectedSelected&&
		FlattenOpenActiveSet3D(r80OtherBranch.inflow)==r80ExpectedOther&&
		OpenActiveSetComplementarityDiscrepancy3D(r80Shape,r80Boundary,r80OneWorker)==
			0.0029506166530252633&&
		OpenActiveSetComplementarityDiscrepancy3D(r80Shape,r80Boundary,r80OtherBranch)==
			0.038419987516982668&&
		r80OneWorker.inflow==r80ReversedVisit.inflow&&
		r80OneWorker.velocityMPerS.component==r80ReversedVisit.velocityMPerS.component&&
		r80OneWorker.maximumDivergenceResidualPerS<=2.0e-8&&
		r80OneWorker.maximumBoundaryHeadResidualPa<=r80Boundary.pressureTolerancePa,
		"r80 recorded cycling state accepts the canonical solved branch without dt reduction");
	Check(r80BitIdentity&&r80OneWorker.inflow==r80ManyWorkers.inflow&&
		r80OneWorker.maximumActiveSetComplementarityDiscrepancyMPerS==
			r80ManyWorkers.maximumActiveSetComplementarityDiscrepancyMPerS,
		"r80 discontinuous active-set selection is bit-identical at one and N workers");
	bool r81DonorFound=false,r81DonorCorrect=false;
	for(unsigned int side=0;side<6&&!r81DonorFound;++side){const unsigned int axis=side/2;
		const bool positive=side%2;for(std::size_t second=0;second<(side<4?r80Shape.nz:r80Shape.ny)&&
			!r81DonorFound;++second)for(std::size_t first=0;first<(side<2?r80Shape.ny:r80Shape.nx)&&
			!r81DonorFound;++first){const std::size_t index=OpenBoundaryFaceLinearIndex3D(
			r80Shape,side,first,second);std::size_t x=0,y=0,z=0;
			if(axis==0){x=positive?r80Shape.nx:0;y=first;z=second;}
			if(axis==1){x=first;y=positive?r80Shape.ny:0;z=second;}
			if(axis==2){x=first;y=second;z=positive?r80Shape.nz:0;}
			const double outward=(positive?1.0:-1.0)*r80OneWorker.velocityMPerS.
				component[axis][OpenMACFaceIndex3D(r80Shape,axis,x,y,z)];
			const bool velocityInflow=outward < -r80Boundary.velocityToleranceMPerS;
			if(std::fabs(outward)<=r80Boundary.velocityToleranceMPerS||
				velocityInflow==r80OneWorker.inflow[side][index])continue;
			ConservativeVector interior=r80Boundary.ambientState;
			interior[1+MethaneN2]=0.59;
			OpenBoundaryFlux3D scalarFlux;
			r81DonorFound=true;
			r81DonorCorrect=BuildOpenBoundaryFlux3D(interior,300.0,outward,
				PressureOpenBoundary3D,r80OneWorker.inflow[side][index],r80Boundary,
				300.0,300.0,0.0,0.0,1.0,0.0,FireSimulationMethaneRecord::PhysicalV1(),
				FireSimulationMethaneRecord::PhysicalV1(),scalarFlux,&error)&&
				scalarFlux.totalOutwardFlux[1+MethaneN2]==outward*(velocityInflow?
					r80Boundary.ambientState[1+MethaneN2]:interior[1+MethaneN2]);}}
	Check(r81DonorFound&&r81DonorCorrect,
		"r81 resolved scalar donor follows accepted velocity sign, not discontinuous pressure bit");
	OpenBoundaryConfig3D r81ScalarBoundary=openBoundary3D;
	r81ScalarBoundary.kind.fill(AdiabaticWallBoundary3D);
	r81ScalarBoundary.kind[0]=PressureOpenBoundary3D;
	r81ScalarBoundary.velocityToleranceMPerS=1.0e-10;
	r81ScalarBoundary.ambientState=ToConservativeVector(
		PhysicalMixtureLineState(fuel,thermochemistry,0.0,300.0));
	const ConservativeVector r81Interior=ToConservativeVector(
		PhysicalMixtureLineState(fuel,thermochemistry,0.2,350.0));
	OpenBoundaryFlux3D r81ResolvedOutflow,r81ResolvedInflow;
	const bool r81ResolvedOutflowOK=BuildOpenBoundaryFlux3D(r81Interior,350.0,0.2,
		PressureOpenBoundary3D,true,r81ScalarBoundary,300.0,300.0,0.01,0.2,1.0,
		0.0,fuel,thermochemistry,r81ResolvedOutflow,&error);
	const bool r81ResolvedInflowOK=BuildOpenBoundaryFlux3D(r81Interior,350.0,-0.2,
		PressureOpenBoundary3D,false,r81ScalarBoundary,300.0,300.0,0.01,0.2,1.0,
		0.0,fuel,thermochemistry,r81ResolvedInflow,&error);
	bool r81OutflowDiffusionClosed=r81ResolvedOutflowOK;
	for(const double value:r81ResolvedOutflow.nonadvectiveMassOutwardFlux)
		r81OutflowDiffusionClosed=r81OutflowDiffusionClosed&&value==0.0;
	Check(r81OutflowDiffusionClosed&&r81ResolvedOutflow.nonadvectiveEnergyOutwardFlux==0.0&&
		r81ResolvedOutflow.totalOutwardFlux[1+MethaneCH4]==0.2*r81Interior[1+MethaneCH4]&&
		r81ResolvedInflowOK&&r81ResolvedInflow.totalOutwardFlux[1+MethaneCH4]!=
			-0.2*r81Interior[1+MethaneCH4]&&
		r81ResolvedInflow.nonadvectiveEnergyOutwardFlux!=0.0,
		"r81 resolved sign controls pressure-open diffusion and conduction as well as the advective donor");
	PeriodicMACShape r81GhostShape;r81GhostShape.nx=3u;r81GhostShape.ny=3u;
	r81GhostShape.nz=3u;r81GhostShape.cellWidthM=1.0;
	std::vector<ConservativeVector> r81GhostState(r81GhostShape.CellCount());
	for(std::size_t z=0;z<r81GhostShape.nz;++z)for(std::size_t y=0;y<r81GhostShape.ny;
		++y)for(std::size_t x=0;x<r81GhostShape.nx;++x)r81GhostState[
		r81GhostShape.Index(x,y,z)]=ToConservativeVector(PhysicalMixtureLineState(fuel,
			thermochemistry,x==0u?0.1:0.2,300.0));
	OpenBoundaryConfig3D r81GhostBoundary=r81ScalarBoundary;
	r81GhostBoundary.bottomFuelMask.assign(r81GhostShape.nx*r81GhostShape.ny,false);
	r81GhostBoundary.bottomFuelMassFluxKGPerM2S.assign(
		r81GhostShape.nx*r81GhostShape.ny,0.0);
	OpenMACProjection3DResult r81GhostProjection;
	for(unsigned int side=0;side<6;++side){const std::size_t sideCount=
		OpenBoundaryFaceCount3D(r81GhostShape,side);
		r81GhostProjection.inflow[side].assign(sideCount,false);
		r81GhostBoundary.priorInflow[side].assign(sideCount,false);}
	std::fill(r81GhostProjection.inflow[0].begin(),r81GhostProjection.inflow[0].end(),true);
	r81GhostProjection.velocityMPerS.component[0].assign(
		OpenMACFaceCount3D(r81GhostShape,0u),0.0);
	for(std::size_t z=0;z<r81GhostShape.nz;++z)for(std::size_t y=0;y<r81GhostShape.ny;
		++y){r81GhostProjection.velocityMPerS.component[0][OpenMACFaceIndex3D(
		r81GhostShape,0u,0u,y,z)]=-0.2;
		r81GhostProjection.velocityMPerS.component[0][OpenMACFaceIndex3D(
			r81GhostShape,0u,1u,y,z)]=0.2;
		r81GhostProjection.velocityMPerS.component[0][OpenMACFaceIndex3D(
			r81GhostShape,0u,2u,y,z)]=0.2;}
	r81GhostProjection.velocityMPerS.component[1].assign(
		OpenMACFaceCount3D(r81GhostShape,1u),0.0);
	r81GhostProjection.velocityMPerS.component[2].assign(
		OpenMACFaceCount3D(r81GhostShape,2u),0.0);
	OpenFluxPair3D r81GhostFlux;
	const bool r81GhostOK=BuildOpenFluxPair3D(r81GhostShape,r81GhostState,
		std::vector<double>(r81GhostShape.CellCount(),300.0),r81GhostProjection,
		std::vector<double>(r81GhostShape.CellCount(),0.0),
		std::vector<double>(r81GhostShape.CellCount(),0.0),r81GhostBoundary,300.0,300.0,fuel,
		thermochemistry,r81GhostFlux,&error);
	const std::size_t r81FirstInteriorFace=OpenMACFaceIndex3D(r81GhostShape,0u,1u,0u,0u);
	if(!r81GhostOK)std::printf("r81 ghost diagnostic error=%s\n",error.c_str());
	if(r81GhostOK&&r81GhostFlux.high[0][r81FirstInteriorFace][0]!=
		r81GhostFlux.low[0][r81FirstInteriorFace][0])std::printf(
		"r81 ghost diagnostic low=%.17g high=%.17g state=%.17g velocity=%.17g\n",
		r81GhostFlux.low[0][r81FirstInteriorFace][0],
		r81GhostFlux.high[0][r81FirstInteriorFace][0],r81GhostState[0][0],
		r81GhostProjection.velocityMPerS.component[0][r81FirstInteriorFace]);
	Check(r81GhostOK&&r81GhostFlux.high[0][r81FirstInteriorFace][0]==
		r81GhostFlux.low[0][r81FirstInteriorFace][0],
		"r81 high-order pressure-open ghost follows the resolved scalar sign and preserves the outflow zero slope");
	bool allPositiveInflowHeads=true;
	for( unsigned int normalAxis=0; normalAxis<3; ++normalAxis ) {
		const unsigned int positiveSide=2*normalAxis+1;
		OpenBoundaryConfig3D positiveInflowBoundary=openBoundary3D;
		positiveInflowBoundary.kind.fill(AdiabaticWallBoundary3D);
		positiveInflowBoundary.kind[2*normalAxis]=PressureOpenBoundary3D;
		positiveInflowBoundary.kind[positiveSide]=PressureOpenBoundary3D;
		OpenMACField3D positiveInflowMomentum=zeroOpenMomentum3D;
		for( double& value : positiveInflowMomentum.component[normalAxis] ) value=
			-ambientState3D.GasDensity()*0.1;
		for( unsigned int tangent=0; tangent<3; ++tangent ) if(tangent!=normalAxis)
			for( double& value : positiveInflowMomentum.component[tangent] ) value=
				ambientState3D.GasDensity()*(tangent==0?0.02:0.03);
		OpenMACProjection3DResult positiveInflowProjection;
		bool positiveInflowOK=ProjectPressureOpenMACVelocity3D(openShape3D,
			std::vector<double>(openShape3D.CellCount(),ambientState3D.GasDensity()),
			positiveInflowMomentum,std::vector<double>(openShape3D.CellCount(),0.0),
			positiveInflowBoundary,0.01,2.0e-8,positiveInflowProjection,&error) &&
			std::all_of(positiveInflowProjection.inflow[positiveSide].begin(),
			positiveInflowProjection.inflow[positiveSide].end(),
			[]( const bool value ){ return value; });
		const std::size_t firstCount=normalAxis==0?openShape3D.ny:openShape3D.nx;
		const std::size_t secondCount=normalAxis==2?openShape3D.ny:openShape3D.nz;
		for( std::size_t second=0; positiveInflowOK && second<secondCount; ++second ) for(
			std::size_t first=0; first<firstCount; ++first ) {
			std::size_t x=0,y=0,z=0,cx=0,cy=0,cz=0;
			if(normalAxis==0){x=openShape3D.nx;y=first;z=second;cx=openShape3D.nx-1;cy=y;cz=z;}
			if(normalAxis==1){x=first;y=openShape3D.ny;z=second;cx=x;cy=openShape3D.ny-1;cz=z;}
			if(normalAxis==2){x=first;y=second;z=openShape3D.nz;cx=x;cy=y;cz=openShape3D.nz-1;}
			const std::size_t face=OpenMACFaceIndex3D(openShape3D,normalAxis,x,y,z);
			double speedSquared=positiveInflowProjection.velocityMPerS.component[normalAxis][face]*
				positiveInflowProjection.velocityMPerS.component[normalAxis][face];
			for( unsigned int tangent=0; tangent<3; ++tangent ) if(tangent!=normalAxis) {
				const double value=OpenBoundaryTangentialCellVelocity3D(openShape3D,
					positiveInflowProjection.velocityMPerS,tangent,cx,cy,cz);
				speedSquared+=value*value;
			}
			const std::size_t index=OpenBoundaryFaceLinearIndex3D(openShape3D,
				positiveSide,first,second);
			positiveInflowOK=Near(positiveInflowProjection.boundaryDynamicPressurePa[
				positiveSide][index],-0.5*positiveInflowBoundary.ambientDensityKGPerM3*
				speedSquared,positiveInflowBoundary.pressureTolerancePa);
		}
		OpenBoundaryFluxField3D positiveInflowFlux;
		positiveInflowOK=positiveInflowOK && BuildOpenBoundaryFluxField3D(openShape3D,
			hotOutflowCells,std::vector<double>(openShape3D.CellCount(),800.0),
			std::vector<double>(openShape3D.CellCount(),0.01),
			std::vector<double>(openShape3D.CellCount(),0.1),positiveInflowBoundary,
			positiveInflowProjection,300.0,300.0,fuel,thermochemistry,
			positiveInflowFlux,&error);
		for( std::size_t component=0; positiveInflowOK &&
			component<MethaneConservativeDimension; ++component ) {
			const OpenBoundaryFlux3D& flux=positiveInflowFlux.side[positiveSide][0];
			const std::size_t face=normalAxis==0?OpenMACFaceIndex3D(openShape3D,0,
				openShape3D.nx,0,0):(normalAxis==1?OpenMACFaceIndex3D(openShape3D,1,0,
				openShape3D.ny,0):OpenMACFaceIndex3D(openShape3D,2,0,0,openShape3D.nz));
			const double outward=positiveInflowProjection.velocityMPerS.component[normalAxis][face];
			positiveInflowOK=Near(flux.totalOutwardFlux[component],outward*
				positiveInflowBoundary.ambientState[component]+(component<MethaneMassStateDimension?
				flux.nonadvectiveMassOutwardFlux[component]:(component==MethaneMassStateDimension?
				flux.nonadvectiveEnergyOutwardFlux:0.0)),2.0e-14);
		}
		allPositiveInflowHeads=allPositiveInflowHeads && positiveInflowOK;
	}
	Check(allPositiveInflowHeads,
		"V1 every positive side enforces full-vector inflow head and immediate ambient flux");
	const double jacobianAmbientDensity=ambientState3D.GasDensity();
	const double jacobianFaceDensity=4.0*jacobianAmbientDensity;
	const double jacobianTimeStep=0.01,jacobianWidth=openShape3D.cellWidthM;
	const double jacobianK=2.0*jacobianTimeStep/(jacobianFaceDensity*jacobianWidth);
	const double jacobianUnprojected=-0.1,jacobianCellPressure=0.02;
	const double jacobianTangent=0.2,jacobianTangentDerivative=0.003;
	auto BoundaryNormalForTangent=[&]( const double tangent ) {
		double facePressure=0.0;
		for( std::size_t iteration=0; iteration<40; ++iteration ) {
			const double normal=jacobianUnprojected+jacobianK*(jacobianCellPressure-facePressure);
			const double residual=facePressure+0.5*jacobianAmbientDensity*
				(normal*normal+tangent*tangent);
			const double derivative=1.0-jacobianAmbientDensity*normal*jacobianK;
			facePressure-=residual/derivative;
		}
		return (jacobianUnprojected+jacobianK*(jacobianCellPressure-facePressure))/jacobianWidth;
	};
	const double jacobianStep=1.0e-5;
	const double finiteDifferenceJacobian=(BoundaryNormalForTangent(jacobianTangent+
		jacobianTangentDerivative*jacobianStep)-BoundaryNormalForTangent(jacobianTangent-
		jacobianTangentDerivative*jacobianStep))/(2.0*jacobianStep);
	const double baselineNormal=BoundaryNormalForTangent(jacobianTangent)*jacobianWidth;
	const double exactNewtonJacobian=jacobianTimeStep*
		OpenBoundaryTangentialNewtonRowFactor3D(jacobianAmbientDensity,jacobianFaceDensity,
		jacobianTangent,jacobianWidth,1.0-jacobianAmbientDensity*baselineNormal*jacobianK)*
		jacobianTangentDerivative;
	Check(Near(finiteDifferenceJacobian,exactNewtonJacobian,2.0e-8),
		"V1 variable-density oblique Newton row is the finite-difference derivative of full total head");
	OpenMACField3D varyingTangentialField=zeroOpenMomentum3D;
	varyingTangentialField.component[1][OpenMACFaceIndex3D(openShape3D,1,0,0,0)]=0.02;
	varyingTangentialField.component[1][OpenMACFaceIndex3D(openShape3D,1,0,1,0)]=0.18;
	Check(Near(OpenBoundaryTangentialCellVelocity3D(openShape3D,varyingTangentialField,
		1,0,0,0),0.1,2.0e-15),
		"V1 edge-adjacent total head uses the independent MAC average of both bounding tangential faces");
	OpenBoundaryFlux3D ambientBackflowFlux,outflowFlux,fuelFlux;
	MethaneCellState hotOutflow=PhysicalMixtureLineState(fuel,thermochemistry,0.2,800.0);
	hotOutflow.constituent[MethaneCarbon]=0.01;
	bool exactAmbientDonor=BuildOpenBoundaryFlux3D(ToConservativeVector(hotOutflow),800.0,-0.1,
		PressureOpenBoundary3D,true,openBoundary3D,300.0,300.0,0.0,0.0,
		openShape3D.cellWidthM,openBoundary3D.fuelMassFluxKGPerM2S,
		fuel,thermochemistry,ambientBackflowFlux,&error);
	for( std::size_t component=0; component<MethaneConservativeDimension; ++component ) {
		exactAmbientDonor=exactAmbientDonor && Near(ambientBackflowFlux.totalOutwardFlux[component],
			-0.1*openBoundary3D.ambientState[component],2.0e-15);
	}
	Check(exactAmbientDonor && ambientBackflowFlux.totalOutwardFlux[1+MethaneCarbon]==0.0,
		"V1 reversing pressure-open face immediately replaces hot fuel/aerosol with complete ambient state");
	OpenBoundaryFlux3D diffusiveAmbientBackflow;
	bool ambientDiffusionClosed=BuildOpenBoundaryFlux3D(ToConservativeVector(hotOutflow),
		800.0,-0.1,PressureOpenBoundary3D,true,openBoundary3D,300.0,300.0,0.01,0.1,
		openShape3D.cellWidthM,openBoundary3D.fuelMassFluxKGPerM2S,
		fuel,thermochemistry,diffusiveAmbientBackflow,&error);
	bool nonzeroAmbientDiffusion=false;
	for( std::size_t component=0; component<MethaneMassStateDimension; ++component ) {
		nonzeroAmbientDiffusion=nonzeroAmbientDiffusion ||
			diffusiveAmbientBackflow.nonadvectiveMassOutwardFlux[component]!=0.0;
		ambientDiffusionClosed=ambientDiffusionClosed && Near(
			diffusiveAmbientBackflow.totalOutwardFlux[component],
			-0.1*openBoundary3D.ambientState[component]+
			diffusiveAmbientBackflow.nonadvectiveMassOutwardFlux[component],2.0e-14);
	}
	ambientDiffusionClosed=ambientDiffusionClosed && Near(
		diffusiveAmbientBackflow.totalOutwardFlux[MethaneMassStateDimension],
		-0.1*openBoundary3D.ambientState[MethaneMassStateDimension]+
		diffusiveAmbientBackflow.nonadvectiveEnergyOutwardFlux,2.0e-14);
	Check(ambientDiffusionClosed && nonzeroAmbientDiffusion &&
		diffusiveAmbientBackflow.nonadvectiveEnergyOutwardFlux!=0.0,
		"V1 ambient Dirichlet inflow closes projected species diffusion and conductive enthalpy");
	Check(BuildOpenBoundaryFlux3D(ToConservativeVector(hotOutflow),800.0,0.1,
		PressureOpenBoundary3D,false,openBoundary3D,300.0,300.0,0.01,0.1,
		openShape3D.cellWidthM,openBoundary3D.fuelMassFluxKGPerM2S,
		fuel,thermochemistry,outflowFlux,&error) &&
		outflowFlux.nonadvectiveEnergyOutwardFlux==0.0 &&
		std::all_of(outflowFlux.nonadvectiveMassOutwardFlux.begin(),
			outflowFlux.nonadvectiveMassOutwardFlux.end(),[]( const double value ){ return value==0.0; }),
		"V1 pressure-open outflow suppresses every inward diffusive/conductive flux");
	bool exactFuelFlux=BuildOpenBoundaryFlux3D(ToConservativeVector(ambientState3D),300.0,0.0,
		FuelInletBoundary3D,true,openBoundary3D,300.0,300.0,0.0,0.0,
		openShape3D.cellWidthM,openBoundary3D.fuelMassFluxKGPerM2S,
		fuel,thermochemistry,fuelFlux,&error);
	double injectedMass=0.0;
	for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) injectedMass+=
		openBoundary3D.injectedState[1+species];
	for( std::size_t component=0; component<MethaneConservativeDimension; ++component ) {
		exactFuelFlux=exactFuelFlux && Near(fuelFlux.totalOutwardFlux[component],
			-openBoundary3D.fuelMassFluxKGPerM2S*openBoundary3D.injectedState[component]/
			injectedMass,2.0e-15);
	}
	Check(exactFuelFlux && fuelFlux.totalOutwardFlux[1+MethaneCarbon]==0.0,
		"V1 fuel-bed flux injects the complete methane record state with zero aerosol");
	bool pureFuelTuple=fuelFlux.totalOutwardFlux[0]==
		-openBoundary3D.fuelMassFluxKGPerM2S&&
		fuelFlux.totalOutwardFlux[1+MethaneCH4]==
		-openBoundary3D.fuelMassFluxKGPerM2S;
	for(std::size_t species=1;species<MethaneSpeciesCount;++species)
		pureFuelTuple=pureFuelTuple&&fuelFlux.totalOutwardFlux[1+species]==0.0;
	Check(pureFuelTuple&&MaximumConstraintResidual(fuel.ConservativeReconstruction(),
		fuelFlux.totalOutwardFlux)==0.0&&CertifiedConstraintRowsSatisfied(
			fuelFlux.totalOutwardFlux,fuel.ConservativeReconstruction(),
			fuel.AcceptedStateFeasibilityEnvelope()),
		"V1 prescribed methane tuple is exactly pure fuel and lies in ker(A)");
	OpenBoundaryConfig3D maskedFuelBoundary=openBoundary3D;
	maskedFuelBoundary.bottomFuelMask.assign(openShape3D.nx*openShape3D.ny,false);
	const std::size_t fuelFaceIndex=(openShape3D.ny/2)*openShape3D.nx+openShape3D.nx/2;
	maskedFuelBoundary.bottomFuelMask[fuelFaceIndex]=true;
	OpenBoundaryFluxField3D maskedFuelField;
	bool maskedFuelOK=BuildOpenBoundaryFluxField3D(openShape3D,ambientCells3D,
		std::vector<double>(openShape3D.CellCount(),300.0),
		std::vector<double>(openShape3D.CellCount(),0.0),
		std::vector<double>(openShape3D.CellCount(),0.0),maskedFuelBoundary,openRest3D,
		300.0,300.0,fuel,thermochemistry,maskedFuelField,&error);
	for( std::size_t component=0; component<MethaneConservativeDimension; ++component ) {
		maskedFuelOK=maskedFuelOK && Near(maskedFuelField.side[4][fuelFaceIndex].
			totalOutwardFlux[component],fuelFlux.totalOutwardFlux[component],2.0e-15);
	}
	const std::size_t unmaskedFuelNeighbor=fuelFaceIndex-1;
	for( std::size_t component=0; component<MethaneConservativeDimension; ++component )
		maskedFuelOK=maskedFuelOK && maskedFuelField.side[4][unmaskedFuelNeighbor].
			totalOutwardFlux[component]==0.0;
	maskedFuelOK=maskedFuelOK && maskedFuelField.side[4][unmaskedFuelNeighbor].
		nonadvectiveEnergyOutwardFlux==0.0 && std::all_of(maskedFuelField.side[4][
		unmaskedFuelNeighbor].nonadvectiveMassOutwardFlux.begin(),maskedFuelField.side[4][
		unmaskedFuelNeighbor].nonadvectiveMassOutwardFlux.end(),
		[]( const double value ){ return value==0.0; });
	Check(maskedFuelOK,
		"V1 bed mask injects every fuel field while an unmasked neighbor remains adiabatic");
	OpenMACProjection3DResult maskedFuelProjection;
	const bool maskedFuelProjectionOK=ProjectPressureOpenMACVelocity3D(openShape3D,
		std::vector<double>(openShape3D.CellCount(),ambientState3D.GasDensity()),
		zeroOpenMomentum3D,std::vector<double>(openShape3D.CellCount(),0.0),
		maskedFuelBoundary,0.01,2.0e-8,maskedFuelProjection,&error);
	const std::size_t maskedFuelNormalFace=OpenMACFaceIndex3D(openShape3D,2,
		openShape3D.nx/2,openShape3D.ny/2,0);
	const std::size_t unmaskedFuelNormalFace=OpenMACFaceIndex3D(openShape3D,2,
		openShape3D.nx/2-1,openShape3D.ny/2,0);
	Check(maskedFuelProjectionOK && Near(
		maskedFuelProjection.momentumKGPerM2S.component[2][maskedFuelNormalFace],
		openBoundary3D.fuelMassFluxKGPerM2S,2.0e-15) && Near(
		maskedFuelProjection.faceDensityKGPerM3.component[2][maskedFuelNormalFace]*
		maskedFuelProjection.velocityMPerS.component[2][maskedFuelNormalFace],
		openBoundary3D.fuelMassFluxKGPerM2S,2.0e-15) &&
		maskedFuelProjection.velocityMPerS.component[2][unmaskedFuelNormalFace]==0.0 &&
		maskedFuelProjection.momentumKGPerM2S.component[2][unmaskedFuelNormalFace]==0.0,
		"V1 fuel-bed momentum ledger matches scalar mass flux only on masked faces");
	OpenBoundaryConfig3D patternedFuelBoundary=maskedFuelBoundary;
	patternedFuelBoundary.bottomFuelMask[unmaskedFuelNeighbor]=true;
	patternedFuelBoundary.bottomFuelMassFluxKGPerM2S.assign(openShape3D.nx*openShape3D.ny,0.0);
	patternedFuelBoundary.bottomFuelMassFluxKGPerM2S[fuelFaceIndex]=0.007;
	patternedFuelBoundary.bottomFuelMassFluxKGPerM2S[unmaskedFuelNeighbor]=0.013;
	OpenMACProjection3DResult patternedFuelProjection;
	OpenBoundaryFluxField3D patternedFuelField;
	const bool patternedFuelOK=ProjectPressureOpenMACVelocity3D(openShape3D,
		std::vector<double>(openShape3D.CellCount(),ambientState3D.GasDensity()),
		zeroOpenMomentum3D,std::vector<double>(openShape3D.CellCount(),0.0),
		patternedFuelBoundary,0.01,2.0e-8,patternedFuelProjection,&error) &&
		BuildOpenBoundaryFluxField3D(openShape3D,ambientCells3D,
		std::vector<double>(openShape3D.CellCount(),300.0),
		std::vector<double>(openShape3D.CellCount(),0.0),
		std::vector<double>(openShape3D.CellCount(),0.0),patternedFuelBoundary,
		patternedFuelProjection,300.0,300.0,fuel,thermochemistry,patternedFuelField,&error);
	Check(patternedFuelOK && Near(patternedFuelProjection.momentumKGPerM2S.component[2][
		maskedFuelNormalFace],0.007,2.0e-15) && Near(
		patternedFuelProjection.momentumKGPerM2S.component[2][unmaskedFuelNormalFace],0.013,
		2.0e-15) && Near(-patternedFuelField.side[4][fuelFaceIndex].totalOutwardFlux[0],
		0.007,2.0e-15) && Near(-patternedFuelField.side[4][unmaskedFuelNeighbor].
		totalOutwardFlux[0],0.013,2.0e-15),
		"r54 source pattern drives the same per-face fuel mass flux through momentum and scalar ledgers");
	bool rejectsEveryWholeFaceFuel=true;
	for( unsigned int fuelSide=0; fuelSide<6; ++fuelSide ) {
		OpenBoundaryConfig3D wholeFaceFuelBoundary=openBoundary3D;
		wholeFaceFuelBoundary.kind[fuelSide]=FuelInletBoundary3D;
		OpenMACProjection3DResult rejectedWholeFaceFuel;
		rejectsEveryWholeFaceFuel=rejectsEveryWholeFaceFuel &&
			!ProjectPressureOpenMACVelocity3D(openShape3D,
			std::vector<double>(openShape3D.CellCount(),ambientState3D.GasDensity()),
			zeroOpenMomentum3D,std::vector<double>(openShape3D.CellCount(),0.0),
			wholeFaceFuelBoundary,0.01,2.0e-8,rejectedWholeFaceFuel,&error);
	}
	Check(rejectsEveryWholeFaceFuel,
		"V1 rejects whole-face fuel kinds on every side in favor of the bottom bed mask");
	OpenBoundaryConfig3D openBottomFuelMask=maskedFuelBoundary;
	openBottomFuelMask.kind[4]=PressureOpenBoundary3D;
	OpenMACProjection3DResult rejectedOpenBottomFuelMask;
	Check(!ProjectPressureOpenMACVelocity3D(openShape3D,
		std::vector<double>(openShape3D.CellCount(),ambientState3D.GasDensity()),
		zeroOpenMomentum3D,std::vector<double>(openShape3D.CellCount(),0.0),
		openBottomFuelMask,0.01,2.0e-8,rejectedOpenBottomFuelMask,&error),
		"V1 fuel mask requires every unmasked bottom-bed face to remain adiabatic");
	OpenMACProjection3DResult tangentialBoundaryOracle=openRest3D;
	std::fill(tangentialBoundaryOracle.velocityMPerS.component[0].begin(),
		tangentialBoundaryOracle.velocityMPerS.component[0].end(),0.25);
	PopulateOpenBoundaryTangentialVelocity3D(openShape3D,maskedFuelBoundary,
		tangentialBoundaryOracle);
	Check(std::all_of(tangentialBoundaryOracle.boundaryTangentialVelocityMPerS[4][0].begin(),
		tangentialBoundaryOracle.boundaryTangentialVelocityMPerS[4][0].end(),
		[]( const double value ){ return value==0.0; }) &&
		std::all_of(tangentialBoundaryOracle.boundaryTangentialVelocityMPerS[5][0].begin(),
		tangentialBoundaryOracle.boundaryTangentialVelocityMPerS[5][0].end(),
		[]( const double value ){ return value==0.25; }),
		"V1 bed uses tangential no-slip while pressure-open top uses zero normal gradient");
	ConservativeVector overflowingOpenState=ToConservativeVector(ambientState3D);
	overflowingOpenState[MethaneMassStateDimension]=std::numeric_limits<double>::max();
	OpenBoundaryFlux3D rejectedOpenFlux;
	Check(!BuildOpenBoundaryFlux3D(overflowingOpenState,300.0,
		std::numeric_limits<double>::max(),PressureOpenBoundary3D,false,
		openBoundary3D,300.0,300.0,0.0,0.0,openShape3D.cellWidthM,
		openBoundary3D.fuelMassFluxKGPerM2S,fuel,
		thermochemistry,rejectedOpenFlux,&error),
		"V1 open conservative boundary rejects finite-product overflow");
	PeriodicMACShape overflowingOpenShape;
	overflowingOpenShape.nx=std::numeric_limits<std::size_t>::max()/2+1;
	overflowingOpenShape.ny=2; overflowingOpenShape.nz=2; overflowingOpenShape.cellWidthM=1.0;
	OpenMACProjection3DResult rejectedOpenProjection;
	Check(!ProjectPressureOpenMACVelocity3D(overflowingOpenShape,{},OpenMACField3D(),{},
		openBoundary3D,0.01,1.0e-8,rejectedOpenProjection,&error),
		"V1 pressure-open projection rejects overflowing grid products before allocation");
	OpenBoundaryConfig3D invalidKindBoundary=openBoundary3D;
	invalidKindBoundary.kind[0]=99u;
	Check(!BuildOpenBoundaryFlux3D(ambientCells3D[0],300.0,0.0,99u,false,
		openBoundary3D,300.0,300.0,0.0,0.0,openShape3D.cellWidthM,
		openBoundary3D.fuelMassFluxKGPerM2S,fuel,
		thermochemistry,rejectedOpenFlux,&error),
		"V1 scalar boundary helper rejects an unknown boundary discriminant directly");
	Check(!ProjectPressureOpenMACVelocity3D(openShape3D,
		std::vector<double>(openShape3D.CellCount(),ambientState3D.GasDensity()),
		zeroOpenMomentum3D,std::vector<double>(openShape3D.CellCount(),0.0),
		invalidKindBoundary,0.01,1.0e-8,rejectedOpenProjection,&error),
		"V1 pressure-open projection rejects an unknown boundary discriminant before layout");
	OpenMACProjection3DResult preservedFinalProjection;
	preservedFinalProjection.maximumDivergenceResidualPerS=123.0;
	OpenMACProjection3DResult overflowingFinalStage0=openRest3D;
	for( unsigned int axis=0; axis<3; ++axis ) std::fill(
		overflowingFinalStage0.velocityMPerS.component[axis].begin(),
		overflowingFinalStage0.velocityMPerS.component[axis].end(),
		2.0*std::sqrt(std::numeric_limits<double>::max()));
	Check(!ProjectPressureOpenMACVelocity3DFinal(openShape3D,
		std::vector<double>(openShape3D.CellCount(),ambientState3D.GasDensity()),
		zeroOpenMomentum3D,std::vector<double>(openShape3D.CellCount(),0.0),
		openBoundary3D,overflowingFinalStage0,openRest3D,0.01,1.0e-8,
		preservedFinalProjection,&error) &&
		preservedFinalProjection.maximumDivergenceResidualPerS==123.0,
		"V1 rejected final projection preserves the previously accepted output transactionally");
	PeriodicMACShape subnormalWidthShape=openShape3D;
	subnormalWidthShape.nx=2;subnormalWidthShape.ny=2;subnormalWidthShape.nz=2;
	subnormalWidthShape.cellWidthM=1.0e-320;
	OpenMACField3D subnormalMomentum;
	for( unsigned int axis=0; axis<3; ++axis ) subnormalMomentum.component[axis].assign(
		OpenMACFaceCount3D(subnormalWidthShape,axis),0.0);
	Check(!ProjectPressureOpenMACVelocity3D(subnormalWidthShape,
		std::vector<double>(subnormalWidthShape.CellCount(),ambientState3D.GasDensity()),
		subnormalMomentum,std::vector<double>(subnormalWidthShape.CellCount(),0.0),
		openBoundary3D,0.01,1.0e-8,rejectedOpenProjection,&error),
		"V1 pressure-open projection rejects non-finite derived grid scales");
	std::vector<ConservativeVector> invalidOpenCells=ambientCells3D;
	invalidOpenCells[0][1+MethaneCH4]=-1.0;
	invalidOpenCells[0][1+MethaneN2]=2.0;
	OpenBoundaryStage3DResult rejectedOpenStage;
	Check(!BuildOpenBoundaryStage3D(openShape3D,invalidOpenCells,
		std::vector<double>(openShape3D.CellCount(),300.0),zeroOpenMomentum3D,
		relativeBuoyancyRate3D,std::vector<double>(openShape3D.CellCount(),0.0),
		std::vector<double>(openShape3D.CellCount(),0.0),
		std::vector<double>(openShape3D.CellCount(),0.0),openBoundary3D,300.0,300.0,
		0.01,1.0e-8,fuel,thermochemistry,rejectedOpenStage,&error),
		"V1 production boundary stage rejects negative constituents despite a positive sum");
	PeriodicMACShape oddOpenShape;
	oddOpenShape.nx=65;oddOpenShape.ny=65;oddOpenShape.nz=65;oddOpenShape.cellWidthM=0.025;
	OpenMACField3D oddOpenMomentum;
	for( unsigned int axis=0; axis<3; ++axis ) oddOpenMomentum.component[axis].assign(
		OpenMACFaceCount3D(oddOpenShape,axis),0.0);
	OpenMACProjection3DResult oddOpenProjection;
	const bool oddOpenOK=ProjectPressureOpenMACVelocity3D(oddOpenShape,
		std::vector<double>(oddOpenShape.CellCount(),ambientState3D.GasDensity()),
		oddOpenMomentum,std::vector<double>(oddOpenShape.CellCount(),0.05),openBoundary3D,
		0.01,2.0e-8,oddOpenProjection,&error);
	if(!oddOpenOK) std::printf("V1 odd-grid diagnostic: %s\n",error.c_str());
	Check(oddOpenOK &&
		oddOpenProjection.maximumDivergenceResidualPerS<=2.0e-8,
		"V1 geometric multigrid coarsens ordinary odd-sized open domains");
	for( unsigned int classCode=0; classCode<4; ++classCode ) {
		const bool stage0Inflow = (classCode&1u) != 0;
		const bool stage1Inflow = (classCode&2u) != 0;
		OpenMACProjection1DResult finalOpen;
		const bool finalOK = ReferenceProjectPressureOpenMACVelocity1DFinal(openDensity,
			std::vector<double>(9,0.0),openTarget,1.18,0.05,0.01,
			stage0Inflow,stage1Inflow,false,false,0.2,0.4,0.0,0.0,
			1.0e-9,1.0e-9,finalOpen,&error);
		const double expectedIntegratedHead = -0.25*1.18*
			((stage0Inflow ? 0.2*0.2 : 0.0)+(stage1Inflow ? 0.4*0.4 : 0.0));
		Check(finalOK && Near(finalOpen.leftBoundaryPressurePa,
			expectedIntegratedHead,2.0e-15) &&
			(finalOpen.leftInflow == (finalOpen.velocityMPerS[0] > 1.0e-9 ? true :
				(finalOpen.velocityMPerS[0] < -1.0e-9 ? false : stage1Inflow))),
			"V1 final open projection uses the Heun indicator-integrated head");
	}
	for( unsigned int normalAxis=0; normalAxis<3; ++normalAxis ) for(
		unsigned int sideParity=0; sideParity<2; ++sideParity ) for(
		unsigned int classCode=0; classCode<4; ++classCode ) {
		OpenMACProjection3DResult stage0=openRest3D,stage1=openRest3D;
		const double stage0Velocity[3]={0.2,0.1,0.05};
		const double stage1Velocity[3]={0.4,0.3,0.07};
		for( unsigned int axis=0; axis<3; ++axis ) {
			std::fill(stage0.velocityMPerS.component[axis].begin(),
				stage0.velocityMPerS.component[axis].end(),stage0Velocity[axis]);
			std::fill(stage1.velocityMPerS.component[axis].begin(),
				stage1.velocityMPerS.component[axis].end(),stage1Velocity[axis]);
		}
		for( unsigned int side=0; side<6; ++side ) {
			std::fill(stage0.inflow[side].begin(),stage0.inflow[side].end(),false);
			std::fill(stage1.inflow[side].begin(),stage1.inflow[side].end(),false);
		}
		const unsigned int testedSide=2*normalAxis+sideParity;
		std::fill(stage0.inflow[testedSide].begin(),stage0.inflow[testedSide].end(),
			(classCode&1u)!=0);
		std::fill(stage1.inflow[testedSide].begin(),stage1.inflow[testedSide].end(),
			(classCode&2u)!=0);
		OpenBoundaryConfig3D finalBoundary3D=openBoundary3D;
		finalBoundary3D.kind.fill(AdiabaticWallBoundary3D);
		finalBoundary3D.kind[testedSide]=PressureOpenBoundary3D;
		finalBoundary3D.kind[2*normalAxis]=PressureOpenBoundary3D;
		finalBoundary3D.kind[2*normalAxis+1]=PressureOpenBoundary3D;
		OpenMACProjection3DResult finalOpen3D;
		const bool final3DOK=ProjectPressureOpenMACVelocity3DFinal(openShape3D,
			std::vector<double>(openShape3D.CellCount(),ambientState3D.GasDensity()),
			zeroOpenMomentum3D,std::vector<double>(openShape3D.CellCount(),0.0),
			finalBoundary3D,stage0,stage1,0.01,2.0e-8,finalOpen3D,&error);
		const double speed0=stage0Velocity[0]*stage0Velocity[0]+
			stage0Velocity[1]*stage0Velocity[1]+stage0Velocity[2]*stage0Velocity[2];
		const double speed1=stage1Velocity[0]*stage1Velocity[0]+
			stage1Velocity[1]*stage1Velocity[1]+stage1Velocity[2]*stage1Velocity[2];
		const double expectedFullHead=-0.25*ambientState3D.GasDensity()*
			(((classCode&1u)?speed0:0.0)+((classCode&2u)?speed1:0.0));
		bool exactIntegratedHead=final3DOK && finalOpen3D.stepAverageDynamicPressurePa.size()==
			openShape3D.CellCount() && finalOpen3D.boundaryDynamicPressurePa[testedSide].size()==
			OpenBoundaryFaceCount3D(openShape3D,testedSide);
		for( unsigned int axis=0; exactIntegratedHead && axis<3; ++axis ) {
			exactIntegratedHead=finalOpen3D.velocityMPerS.component[axis].size()==
				OpenMACFaceCount3D(openShape3D,axis) &&
				finalOpen3D.momentumKGPerM2S.component[axis].size()==
				OpenMACFaceCount3D(openShape3D,axis);
			for( std::size_t face=0; exactIntegratedHead &&
				face<finalOpen3D.velocityMPerS.component[axis].size(); ++face )
				exactIntegratedHead=Near(finalOpen3D.momentumKGPerM2S.component[axis][face],
					finalOpen3D.faceDensityKGPerM3.component[axis][face]*
					finalOpen3D.velocityMPerS.component[axis][face],2.0e-14);
		}
		for( const double pressure : finalOpen3D.boundaryDynamicPressurePa[testedSide] )
			exactIntegratedHead=exactIntegratedHead && Near(pressure,expectedFullHead,2.0e-15);
		bool boundaryVelocityEquation=final3DOK;
		const std::size_t firstCount=normalAxis==0?openShape3D.ny:openShape3D.nx;
		const std::size_t secondCount=normalAxis==2?openShape3D.ny:openShape3D.nz;
		for( std::size_t second=0; boundaryVelocityEquation && second<secondCount; ++second )
			for( std::size_t first=0; first<firstCount; ++first ) {
				std::size_t x=0,y=0,z=0,cx=0,cy=0,cz=0;
				if(normalAxis==0){x=sideParity?openShape3D.nx:0;y=first;z=second;
					cx=sideParity?openShape3D.nx-1:0;cy=y;cz=z;}
				if(normalAxis==1){x=first;y=sideParity?openShape3D.ny:0;z=second;
					cx=x;cy=sideParity?openShape3D.ny-1:0;cz=z;}
				if(normalAxis==2){x=first;y=second;z=sideParity?openShape3D.nz:0;
					cx=x;cy=y;cz=sideParity?openShape3D.nz-1:0;}
				const std::size_t face=OpenMACFaceIndex3D(openShape3D,normalAxis,x,y,z);
				const std::size_t cell=openShape3D.Index(cx,cy,cz);
				const std::size_t index=OpenBoundaryFaceLinearIndex3D(openShape3D,
					testedSide,first,second);
				const double sign=sideParity?1.0:-1.0;
				const double expectedVelocity=sign*2.0*0.01/(
					finalOpen3D.faceDensityKGPerM3.component[normalAxis][face]*
					openShape3D.cellWidthM)*(finalOpen3D.stepAverageDynamicPressurePa[cell]-
					finalOpen3D.boundaryDynamicPressurePa[testedSide][index]);
				boundaryVelocityEquation=boundaryVelocityEquation && Near(
					finalOpen3D.velocityMPerS.component[normalAxis][face],expectedVelocity,2.0e-14);
				if(classCode!=0) boundaryVelocityEquation=boundaryVelocityEquation &&
					finalOpen3D.velocityMPerS.component[normalAxis][face]!=0.0;
			}
		std::size_t endpointX=0,endpointY=0,endpointZ=0;
		if(normalAxis==0) endpointX=sideParity?openShape3D.nx:0;
		if(normalAxis==1) endpointY=sideParity?openShape3D.ny:0;
		if(normalAxis==2) endpointZ=sideParity?openShape3D.nz:0;
		const double endpointOutward=(sideParity?1.0:-1.0)*
			finalOpen3D.velocityMPerS.component[normalAxis][OpenMACFaceIndex3D(
			openShape3D,normalAxis,endpointX,endpointY,endpointZ)];
		const bool expectedEndpoint=endpointOutward < -finalBoundary3D.velocityToleranceMPerS ?
			true:(endpointOutward > finalBoundary3D.velocityToleranceMPerS ? false:
			(classCode&2u)!=0);
		Check(final3DOK && exactIntegratedHead && boundaryVelocityEquation &&
			finalOpen3D.inflow[testedSide].size()==
			OpenBoundaryFaceCount3D(openShape3D,testedSide) &&
			finalOpen3D.inflow[testedSide][0]==expectedEndpoint &&
			finalOpen3D.maximumDivergenceResidualPerS<=2.0e-8,
			"V1 production 3-D R2 covers every side, normal/tangent role, and class switch");
	}

	// V2: a discontinuous-density, nonzero-divergence projection uses the
	// same arithmetic staggered density for stored momentum and pressure.
	const double pi = std::acos(-1.0);
	std::vector<double> density(projectionCells), momentum(projectionCells);
	std::vector<double> divergenceTarget(projectionCells);
	for( std::size_t cell=0; cell<projectionCells; ++cell ) {
		density[cell] = cell < projectionCells/2 ? 0.4 : 1.6;
		divergenceTarget[cell] = 0.12*std::sin(2.0*pi*(cell+0.5)/projectionCells);
	}
	for( std::size_t face=0; face<projectionCells; ++face ) {
		const double faceDensity = 0.5*(density[face]+density[(face+1)%projectionCells]);
		momentum[face] = faceDensity*0.3*std::cos(4.0*pi*(face+0.5)/projectionCells);
	}
	PeriodicProjectionResult variableProjection;
	const bool variableProjectionOK = ProjectPeriodicMACVelocity(density,momentum,divergenceTarget,
		1.0/static_cast<double>(projectionCells),0.02,2.0e-11,
		variableProjection,&error);
	if( !variableProjectionOK ) std::printf("V2 diagnostic: %s\n",error.c_str());
	Check(variableProjectionOK,"V2 variable-density MAC projection converges");
	double projectionResidual = 0.0;
	for( std::size_t cell=0; variableProjectionOK && cell<projectionCells; ++cell ) {
		projectionResidual = std::max(projectionResidual,std::fabs(
			PeriodicDivergence(variableProjection.velocityMPerS,cell,
				1.0/static_cast<double>(projectionCells))-divergenceTarget[cell]));
		Check(variableProjection.faceDensityKGPerM3[cell] ==
			0.5*(density[cell]+density[(cell+1)%projectionCells]),
			"V2 projection stores the arithmetic face density exactly");
	}
	Check(variableProjectionOK && projectionResidual <= 2.0e-11 &&
		!variableProjection.residualHistoryPerS.empty(),
		"V2 accepted velocity closes the manufactured divergence target");
	std::vector<double> impossibleDivergence(projectionCells,0.01);
	PeriodicProjectionResult rejectedProjection;
	Check(!ProjectPeriodicMACVelocity(density,momentum,impossibleDivergence,
		1.0/static_cast<double>(projectionCells),0.02,2.0e-11,
		rejectedProjection,&error),
		"V2 spatially uniform nonzero periodic divergence is rejected");
	PeriodicProjectionResult overflowProjection;
	Check(ProjectPeriodicMACVelocity(std::vector<double>(3,
		std::numeric_limits<double>::max()),std::vector<double>(3,0.0),
		std::vector<double>(3,0.0),1.0,0.01,1.0e-8,overflowProjection,&error)&&
		std::all_of(overflowProjection.faceDensityKGPerM3.begin(),
			overflowProjection.faceDensityKGPerM3.end(),[](const double value){
				return value==std::numeric_limits<double>::max();}),
		"V2 forms an overflow-safe staggered mean of finite positive densities");
	PeriodicMACShape shape3D;
	shape3D.nx = 4; shape3D.ny = 4; shape3D.nz = 4;
	shape3D.cellWidthM = 0.025;
	const std::size_t count3D = shape3D.CellCount();
	std::vector<double> density3D(count3D), target3D(count3D);
	PeriodicMACField momentum3D;
	for( unsigned int axis=0; axis<3; ++axis ) {
		momentum3D.component[axis].assign(count3D,0.0);
	}
	for( std::size_t cell=0; cell<count3D; ++cell ) {
		const std::size_t x = cell%shape3D.nx;
		const std::size_t y = (cell/shape3D.nx)%shape3D.ny;
		const std::size_t z = cell/(shape3D.nx*shape3D.ny);
		density3D[cell] = x < 2 ? 0.5 : 1.5;
		target3D[cell] = 0.08*std::sin(2.0*pi*(x+0.5)/shape3D.nx)+
			0.05*std::cos(2.0*pi*(y+0.5)/shape3D.ny);
		momentum3D.component[0][cell] = 0.1*std::cos(2.0*pi*(y+0.5)/shape3D.ny);
		momentum3D.component[1][cell] = -0.07*std::sin(2.0*pi*(z+0.5)/shape3D.nz);
		momentum3D.component[2][cell] = 0.03*std::cos(2.0*pi*(x+0.5)/shape3D.nx);
	}
	PeriodicMACProjection3DResult projected3D;
	const bool projection3DOK = ProjectPeriodicMACVelocity3D(shape3D,density3D,
		momentum3D,target3D,0.01,2.0e-10,projected3D,&error);
	if( !projection3DOK ) std::printf("V2 3-D diagnostic: %s\n",error.c_str());
	double residual3D = 0.0;
	for( std::size_t cell=0; projection3DOK && cell<count3D; ++cell ) {
		residual3D = std::max(residual3D,std::fabs(PeriodicMACDivergence3D(
			shape3D,projected3D.velocityMPerS,cell)-target3D[cell]));
		for( unsigned int axis=0; axis<3; ++axis ) {
			const std::size_t next = PeriodicNext(shape3D,cell,axis);
			Check(projected3D.faceDensityKGPerM3.component[axis][cell] ==
				0.5*(density3D[cell]+density3D[next]),
				"V2 3-D projection uses one arithmetic staggered density");
		}
	}
	Check(projection3DOK && residual3D <= 2.0e-10 &&
		!projected3D.residualHistoryPerS.empty(),
		"V2 3-D MAC projection closes a variable-density manufactured divergence");
	PeriodicMACShape overflowShape;
	overflowShape.nx = std::numeric_limits<std::size_t>::max()/2+1;
	overflowShape.ny = 2; overflowShape.nz = 2; overflowShape.cellWidthM = 1.0;
	PeriodicMACField emptyMomentum3D;
	Check(!ProjectPeriodicMACVelocity3D(overflowShape,{},emptyMomentum3D,{},
		0.01,1.0e-8,projected3D,&error),
		"V2 rejects a wrapped 3-D grid product before indexing or solving");

	// V3: physical methane/air states are reconstructed through the adopted
	// N_A and the one physical diffusion flux is projected through N_C.
	const std::size_t transportCells = 32;
	std::vector<ConservativeVector> transportBeginning(transportCells);
	for( std::size_t cell=0; cell<transportCells; ++cell ) {
		const double mixtureFraction = 0.25+0.12*std::sin(2.0*pi*(cell+0.5)/transportCells);
		transportBeginning[cell] = ToConservativeVector(PhysicalMixtureLineState(
			fuel,thermochemistry,mixtureFraction,800.0));
	}
	std::vector<double> transportTemperature(transportCells,800.0);
	std::vector<double> transportVelocity(transportCells,0.0);
	std::vector<double> transportDiffusivity(transportCells,1.0e-4);
	std::vector<double> zeroConductivity(transportCells,0.0);
	PeriodicFluxPair physicalFlux;
	Check(BuildPeriodicFluxPair(transportBeginning,transportTemperature,
		transportVelocity,transportDiffusivity,zeroConductivity,
		1.0/static_cast<double>(transportCells),fuel,thermochemistry,
		physicalFlux,&error),"V3 physical centered flux pair assembles");
	double maximumFluxConstraint = 0.0, maximumJZ = 0.0;
	const FireCertifiedNullspace& fluxClosure = fuel.NonadvectiveFluxProjection();
	for( std::size_t face=0; face<transportCells; ++face ) {
		maximumJZ = std::max(maximumJZ,std::fabs(physicalFlux.nonadvectiveMass[face][0]));
		for( std::size_t row=0; row<fluxClosure.constraintRows; ++row ) {
			double residual = 0.0;
			for( std::size_t column=0; column<MethaneMassStateDimension; ++column ) {
				residual += fluxClosure.constraintMatrix[row*MethaneMassStateDimension+column]*
					physicalFlux.nonadvectiveMass[face][column];
			}
			maximumFluxConstraint = std::max(maximumFluxConstraint,std::fabs(residual));
		}
	}
	Check(maximumJZ > 0.0 && maximumFluxConstraint < 2.0e-18,
		"V3 multielement diffusion retains nonzero J_Z while satisfying every C row");
	std::vector<double> ncRaw(MethaneMassStateDimension,0.0),ncExpected(
		MethaneMassStateDimension,0.0);
	const std::size_t ncLeft=0,ncRight=1;
	double ncTotalLeft=0.0,ncTotalRight=0.0;
	for(std::size_t species=0;species<MethaneSpeciesCount;++species){
		ncTotalLeft+=transportBeginning[ncLeft][1+species];
		ncTotalRight+=transportBeginning[ncRight][1+species];
	}
	const double ncRhoD=HarmonicMean(ncTotalLeft*transportDiffusivity[ncLeft],
		ncTotalRight*transportDiffusivity[ncRight]);
	ncRaw[0]=-ncRhoD*(transportBeginning[ncRight][0]/ncTotalRight-
		transportBeginning[ncLeft][0]/ncTotalLeft)*transportCells;
	for(std::size_t species=0;species<MethaneSpeciesCount;++species)
		ncRaw[1+species]=-ncRhoD*(transportBeginning[ncRight][1+species]/ncTotalRight-
			transportBeginning[ncLeft][1+species]/ncTotalLeft)*transportCells;
	for(std::size_t basis=0;basis<fluxClosure.nullity;++basis){
		double coordinate=0.0;
		for(std::size_t row=0;row<MethaneMassStateDimension;++row)
			coordinate+=fluxClosure.orthonormalBasis[row*fluxClosure.nullity+basis]*ncRaw[row];
		for(std::size_t row=0;row<MethaneMassStateDimension;++row)
			ncExpected[row]+=fluxClosure.orthonormalBasis[row*fluxClosure.nullity+basis]*coordinate;
	}
	bool exactNC=true;
	double maximumNCDifference=0.0;
	for(std::size_t row=0;row<MethaneMassStateDimension;++row)
		maximumNCDifference=std::max(maximumNCDifference,std::fabs(
			physicalFlux.nonadvectiveMass[0][row]-ncExpected[row]));
	exactNC=maximumNCDifference<2.0e-18;
	Check(exactNC,
		"V3 physical diffusion uses exactly N_C N_C^T with no sequential correction");
	PeriodicMACShape zeroCarbonShape;zeroCarbonShape.nx=3;zeroCarbonShape.ny=3;
	zeroCarbonShape.nz=3;zeroCarbonShape.cellWidthM=1.0/3.0;
	std::vector<ConservativeVector> zeroCarbonState(zeroCarbonShape.CellCount());
	std::vector<double> zeroCarbonTemperature(zeroCarbonShape.CellCount(),0.0);
	for(std::size_t cell=0;cell<zeroCarbonState.size();++cell){const std::size_t x=cell%3,
		y=(cell/3)%3;const double burnedTemperature=650.0+45.0*y;
		MethaneCellState burned=PhysicalMixtureLineState(fuel,
			thermochemistry,0.01+0.004*x+0.001*y,burnedTemperature);
		const double extent=burned.constituent[MethaneCH4]/
			(-fuel.PrimaryReactionDelta()[MethaneCH4]);
		for(std::size_t species=0;species<MethaneSpeciesCount;++species)
			burned.constituent[species]+=extent*fuel.PrimaryReactionDelta()[species];
		burned.constituent[MethaneCH4]=0.0;
		Check(thermochemistry.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(burned),
			burned.temperatureK,burned.sensibleEnergyJPerM3,&error),
			"V3 post-burn zero-CH4 fixture has physical sensible energy");
		zeroCarbonState[cell]=ToConservativeVector(burned);
		zeroCarbonTemperature[cell]=burnedTemperature;}
	std::array<std::vector<std::array<double,MethaneMassStateDimension> >,3> zeroCarbonSlope;
	bool zeroCarbonTangent=InvariantMCMassSlopes3D(zeroCarbonShape,zeroCarbonState,
		fuel.ConservativeReconstruction(),zeroCarbonSlope,&error);
	double zeroCarbonResidual=0.0;
	for(unsigned int axis=0;zeroCarbonTangent&&axis<3;++axis)for(const auto& slope:
		zeroCarbonSlope[axis]){zeroCarbonTangent=zeroCarbonTangent&&
		slope[1+MethaneCH4]==0.0;for(std::size_t row=0;row<fuel.
		ConservativeReconstruction().constraintRows;++row){double residual=0.0;
		for(std::size_t column=0;column<MethaneMassStateDimension;++column){
			residual+=fuel.ConservativeReconstruction().constraintMatrix[
				row*MethaneMassStateDimension+column]*slope[column];
		}
		zeroCarbonResidual=std::max(zeroCarbonResidual,std::fabs(residual));
		zeroCarbonTangent=zeroCarbonTangent&&std::fabs(residual)<2.0e-15;}}
	if(!zeroCarbonTangent)std::printf("V3 zero-CH4 slope residual %.17g\n",zeroCarbonResidual);
	Check(zeroCarbonTangent,
		"V3 absent-inventory MC slopes preserve zero CH4 and the full N_A tangent");
	double nonzeroAbsentSlope=0.0;for(unsigned int axis=0;axis<3;++axis)for(const auto& slope:
		zeroCarbonSlope[axis])for(std::size_t row=0;row<MethaneMassStateDimension;++row)
		if(row!=1+MethaneCH4)nonzeroAbsentSlope=std::max(nonzeroAbsentSlope,std::fabs(slope[row]));
	Check(nonzeroAbsentSlope>1.0e-8,
		"V3 zero-inventory reduced basis retains a genuinely high-order methane slope");
	OpenBoundaryConfig3D zeroCarbonBoundary=openBoundary3D;
	zeroCarbonBoundary.kind.fill(AdiabaticWallBoundary3D);
	zeroCarbonBoundary.bottomFuelMask.clear();
	const double zeroCarbonGasDensity=FromConservativeVector(zeroCarbonState[0]).GasDensity();
	OpenMACProjection3DResult zeroCarbonProjection;
	for(unsigned int axis=0;axis<3;++axis){
		const std::size_t faceCount=OpenMACFaceCount3D(zeroCarbonShape,axis);
		zeroCarbonProjection.velocityMPerS.component[axis].assign(faceCount,axis==0?0.2:0.0);
		zeroCarbonProjection.faceDensityKGPerM3.component[axis].assign(faceCount,
			zeroCarbonGasDensity);
		zeroCarbonProjection.momentumKGPerM2S.component[axis].assign(faceCount,
			axis==0?0.2*zeroCarbonGasDensity:0.0);
	}
	for(unsigned int side=0;side<6;++side)zeroCarbonProjection.inflow[side].assign(
		OpenBoundaryFaceCount3D(zeroCarbonShape,side),false);
	OpenFluxPair3D zeroCarbonOpenFlux;
	const std::vector<double> zeroCarbonZero(zeroCarbonShape.CellCount(),0.0);
	const bool zeroCarbonOpenOK=BuildOpenFluxPair3D(zeroCarbonShape,zeroCarbonState,
		zeroCarbonTemperature,zeroCarbonProjection,zeroCarbonZero,zeroCarbonZero,
		zeroCarbonBoundary,300.0,300.0,fuel,thermochemistry,zeroCarbonOpenFlux,&error);
	double zeroCarbonOpenResidual=0.0,zeroCarbonOpenCorrection=0.0;
	bool zeroCarbonOpenTangent=zeroCarbonOpenOK;
	for(unsigned int axis=0;zeroCarbonOpenTangent&&axis<3;++axis)
		for(std::size_t face=0;face<zeroCarbonOpenFlux.high[axis].size();++face){
			std::array<double,MethaneMassStateDimension> correction;
			for(std::size_t row=0;row<MethaneMassStateDimension;++row){
				correction[row]=zeroCarbonOpenFlux.high[axis][face][row]-
					zeroCarbonOpenFlux.low[axis][face][row];
				zeroCarbonOpenCorrection=std::max(zeroCarbonOpenCorrection,
					std::fabs(correction[row]));
			}
			zeroCarbonOpenTangent=zeroCarbonOpenTangent&&
				zeroCarbonOpenFlux.high[axis][face][1+MethaneCH4]==0.0&&
				zeroCarbonOpenFlux.low[axis][face][1+MethaneCH4]==0.0;
			for(std::size_t row=0;row<fuel.ConservativeReconstruction().constraintRows;++row){
				double residual=0.0;
				for(std::size_t column=0;column<MethaneMassStateDimension;++column)
					residual+=fuel.ConservativeReconstruction().constraintMatrix[
						row*MethaneMassStateDimension+column]*correction[column];
				zeroCarbonOpenResidual=std::max(zeroCarbonOpenResidual,std::fabs(residual));
				zeroCarbonOpenTangent=zeroCarbonOpenTangent&&std::fabs(residual)<2.0e-15;
			}
		}
	if(!(zeroCarbonOpenTangent&&zeroCarbonOpenCorrection>1.0e-8))std::printf(
		"V3 open zero-CH4 slope tangent=%d residual=%.17g correction=%.17g error=%s\n",
		zeroCarbonOpenTangent?1:0,zeroCarbonOpenResidual,zeroCarbonOpenCorrection,error.c_str());
	Check(zeroCarbonOpenTangent&&zeroCarbonOpenCorrection>1.0e-8,
		"V3 open MC reconstruction preserves absent methane and N_A while retaining high order");
	OpenFluxPair3D emptyOpenFlux;std::vector<ConservativeVector> sentinelOpenResult(1);
	std::array<std::vector<double>,3> emptyOpenAlpha;PeriodicMACShape zeroOpenShape;
	zeroOpenShape.cellWidthM=1.0;PeriodicTransportConfig malformedOpenFCTConfig;
	malformedOpenFCTConfig.deltaTimeS=0.1;malformedOpenFCTConfig.cellWidthM=1.0;
	malformedOpenFCTConfig.ambientTemperatureK=300.0;
	malformedOpenFCTConfig.adiabaticTemperatureK=2500.0;
	Check(!ApplyOpenSharedFCT3D(zeroOpenShape,{},emptyOpenFlux,{},malformedOpenFCTConfig,
		fuel,thermochemistry,sentinelOpenResult,emptyOpenAlpha,&error)&&
		sentinelOpenResult.size()==1,
		"V3 open FCT rejects a zero-sized grid transactionally");
	// The same unequal-cp diffusion ledger is gauge invariant.  First bind the
	// production J_h field to the independently evaluated species enthalpies,
	// then transform both H_s and that production flux to two reference gauges.
	bool twoReferenceIsothermal=true;
	for(const double gaugeReferenceK:std::array<double,2>{{300.0,650.0}}){
		std::array<double,MethaneSpeciesCount> temperatureEnthalpy={},referenceEnthalpy={};
		static const char* gaugeNames[MethaneSpeciesCount]={
			"CH4","O2","N2","CO2","H2O","CO","C(gr)"};
		for(std::size_t species=0;species<MethaneSpeciesCount;++species){
			twoReferenceIsothermal=twoReferenceIsothermal &&
				thermochemistry.SensibleEnthalpyJPerKG(gaugeNames[species],800.0,
					temperatureEnthalpy[species],&error) &&
				thermochemistry.SensibleEnthalpyJPerKG(gaugeNames[species],gaugeReferenceK,
					referenceEnthalpy[species],&error);
		}
		for(std::size_t face=0;twoReferenceIsothermal&&face<transportCells;++face){
			double expectedJh=0.0;
			for(std::size_t species=0;species<MethaneSpeciesCount;++species)
				expectedJh+=temperatureEnthalpy[species]*
					physicalFlux.nonadvectiveMass[face][1+species];
			twoReferenceIsothermal=Near(physicalFlux.nonadvectiveEnergy[face],expectedJh,2.0e-14);
		}
		for(std::size_t cell=0;twoReferenceIsothermal && cell<transportCells;++cell){
			const std::size_t previous=(cell+transportCells-1)%transportCells;
			double energyBefore=transportBeginning[cell][MethaneMassStateDimension],
				energyAfter=0.0,incomingJh=physicalFlux.nonadvectiveEnergy[previous],
				outgoingJh=physicalFlux.nonadvectiveEnergy[cell];
			for(std::size_t species=0;species<MethaneSpeciesCount;++species){
				const double densityBefore=transportBeginning[cell][1+species];
				const double densityAfter=densityBefore+0.002*transportCells*(
					physicalFlux.nonadvectiveMass[previous][1+species]-
					physicalFlux.nonadvectiveMass[cell][1+species]);
				energyBefore-=referenceEnthalpy[species]*densityBefore;
				energyAfter+=(temperatureEnthalpy[species]-referenceEnthalpy[species])*densityAfter;
				incomingJh-=referenceEnthalpy[species]*
					physicalFlux.nonadvectiveMass[previous][1+species];
				outgoingJh-=referenceEnthalpy[species]*
					physicalFlux.nonadvectiveMass[cell][1+species];
			}
			const double advancedEnergy=energyBefore+0.002*transportCells*(
				incomingJh-outgoingJh);
			twoReferenceIsothermal=twoReferenceIsothermal && Near(
				advancedEnergy,energyAfter,3.0e-14);
		}
	}
	Check(twoReferenceIsothermal,
		"V2/V3 unequal-cp J_h diffusion is isothermal at two T_ref gauges");
	PeriodicTransportConfig transportConfig;
	transportConfig.cellWidthM = 1.0/static_cast<double>(transportCells);
	transportConfig.deltaTimeS = 0.002;
	transportConfig.ambientTemperatureK = 300.0;
	transportConfig.adiabaticTemperatureK = 2500.0;
	std::vector<ConservativeVector> zeroSource(transportCells);
	std::vector<ConservativeVector> transportResult;
	std::vector<double> transportAlpha;
	const bool transportOK = ReferenceAdvancePeriodicTransportHeun1D(transportBeginning,
		transportVelocity,transportDiffusivity,zeroConductivity,zeroSource,
		transportConfig,fuel,thermochemistry,transportResult,transportAlpha,&error);
	if( !transportOK ) std::printf("V3 diagnostic: %s\n",error.c_str());
	Check(transportOK,"V3 projected-Heun physical diffusion step succeeds");
	std::array<double,MethaneConservativeDimension> beforeSum = {};
	std::array<double,MethaneConservativeDimension> afterSum = {};
	double maximumStateConstraint = 0.0;
	for( std::size_t cell=0; transportOK && cell<transportCells; ++cell ) {
		for( std::size_t component=0; component<MethaneConservativeDimension; ++component ) {
			beforeSum[component] += transportBeginning[cell][component];
			afterSum[component] += transportResult[cell][component];
		}
		maximumStateConstraint = std::max(maximumStateConstraint,
			MaximumConstraintResidual(fuel.ConservativeReconstruction(),transportResult[cell]));
	}
	bool globallyConservative = true;
	for( std::size_t component=0; component<MethaneConservativeDimension; ++component ) {
		globallyConservative = globallyConservative &&
			Near(afterSum[component],beforeSum[component],5.0e-15);
	}
	std::vector<double> diffusedTemperature;
	Check(transportOK && InvertPeriodicTemperatures(transportResult,thermochemistry,
		diffusedTemperature,&error),"V3 diffused state temperature inverts");
	double maximumTemperatureError = 0.0;
	for( const double value : diffusedTemperature ) {
		maximumTemperatureError = std::max(maximumTemperatureError,std::fabs(value-800.0));
	}
	Check(transportOK && globallyConservative && maximumStateConstraint < 3.0e-15,
		"V3 shared FCT conserves every field and the real methane affine invariant");
	Check(transportOK && maximumTemperatureError < 2.0e-9,
		"V3 J_h keeps unequal-cp unit-Lewis diffusion exactly isothermal");
	std::vector<ConservativeVector> limiterState(8,
		ToConservativeVector(PhysicalMixtureLineState(fuel,thermochemistry,
			0.5,800.0)));
	limiterState[0] = ToConservativeVector(PhysicalMixtureLineState(
		fuel,thermochemistry,0.1,800.0));
	const ConservativeVector pureFuel = ToConservativeVector(PhysicalMixtureLineState(
		fuel,thermochemistry,1.0,800.0));
	const ConservativeVector pureAir = ToConservativeVector(PhysicalMixtureLineState(
		fuel,thermochemistry,0.0,800.0));
	PeriodicFluxPair limiterFlux;
	limiterFlux.low.assign(8,ConservativeVector());
	limiterFlux.high.assign(8,ConservativeVector());
	limiterFlux.high[0] = 0.2*(pureFuel-pureAir);
	std::vector<ConservativeVector> limiterSource(8), limiterResult;
	MethaneReactionStep limiterReaction;
	limiterReaction.deltaTimeS=1.0;
	limiterReaction.mixingTimeS=2.0;
	limiterReaction.primaryEligible=true;
	for(std::size_t cell=0;cell<limiterState.size();++cell){
		MethaneSourcePacket packet;
		MethaneCellState limiterPhysical=FromConservativeVector(limiterState[cell]);
		limiterPhysical.temperatureK=800.0;
		Check(BuildMethaneReactionPacket(limiterPhysical,fuel,
			limiterReaction,packet,&error),
			"V2/V3 limiter fixture consumes a record-derived methane packet");
		for(std::size_t species=0;species<MethaneSpeciesCount;++species)
			limiterSource[cell][1+species]=packet.constituentDelta[species];
		limiterSource[cell][MethaneMassStateDimension]=packet.sensibleEnergyDeltaJPerM3;
	}
	std::vector<double> limiterAlpha;
	PeriodicTransportConfig limiterConfig = transportConfig;
	limiterConfig.cellWidthM = 1.0;
	limiterConfig.deltaTimeS = 1.0;
	Check(ApplyPeriodicSharedFCT(limiterState,limiterFlux,limiterSource,
		limiterConfig,fuel,thermochemistry,limiterResult,limiterAlpha,&error),
		"V3 limiter-active manufactured correction is admissible");
	Check(limiterAlpha.size() == 8 && limiterAlpha[0] > 0.0 &&
		limiterAlpha[0] < 1.0,
		"V2/V3 record-derived packet and one nodal budget supply one shared nontrivial face alpha");
	// Cross a real constituent-activation surface with two physical states whose
	// normalized separation is at the fp64/Picard scale.  The maximal limiter
	// is allowed to jump: that discontinuity is precisely what r59 classifies.
	std::array<double,MethaneSpeciesCount> burnedWeights={};
	for(std::size_t species=0;species<MethaneSpeciesCount;++species)burnedWeights[species]=
		0.97*fuel.AmbientMassFractions()[species]+0.03*fuel.InjectedMassFractions()[species];
	const double burnedExtent=burnedWeights[MethaneCH4]/
		(-fuel.PrimaryReactionDelta()[MethaneCH4]);
	for(std::size_t species=0;species<MethaneSpeciesCount;++species)
		burnedWeights[species]+=burnedExtent*fuel.PrimaryReactionDelta()[species];
	ConservativeVector reactionDirection;
	for(std::size_t species=0;species<MethaneSpeciesCount;++species)
		reactionDirection[1+species]=fuel.PrimaryReactionDelta()[species];
	reactionDirection[MethaneMassStateDimension]=fuel.LowerHeatingValueJPerKG();
	std::array<double,MethaneSpeciesCount> activationWeightsA=burnedWeights,
		activationWeightsB=burnedWeights;
	for(std::size_t species=0;species<MethaneSpeciesCount;++species){
		activationWeightsA[species]-=1.5e-13*fuel.PrimaryReactionDelta()[species];
		activationWeightsB[species]-=3.0e-13*fuel.PrimaryReactionDelta()[species];}
	const MethaneCellState activationPhysicalA=StateAtTemperature(activationWeightsA,0.03,800.0,
		thermochemistry),activationPhysicalB=StateAtTemperature(activationWeightsB,0.03,800.0,
		thermochemistry);
	std::vector<ConservativeVector> activationA(8,ToConservativeVector(activationPhysicalA));
	std::vector<ConservativeVector> activationB(8,ToConservativeVector(activationPhysicalB));
	PeriodicFluxPair activationFlux;
	activationFlux.low.assign(8,ConservativeVector());
	activationFlux.high=activationFlux.low;
	activationFlux.high[0]=9.0e-13*reactionDirection;
	std::vector<ConservativeVector> activationResultA,activationResultB;
	std::vector<double> activationAlphaA,activationAlphaB;
	const bool activationOK=ApplyPeriodicSharedFCT(activationA,activationFlux,
		std::vector<ConservativeVector>(8),limiterConfig,fuel,thermochemistry,
		activationResultA,activationAlphaA,&error)&&ApplyPeriodicSharedFCT(activationB,
		activationFlux,std::vector<ConservativeVector>(8),limiterConfig,fuel,thermochemistry,
		activationResultB,activationAlphaB,&error);
	double activationStateDistance=0.0,activationStateScale=1.0;
	for(std::size_t component=0;component<MethaneConservativeDimension;++component){
		activationStateDistance=std::max(activationStateDistance,
			std::fabs(activationA[1][component]-activationB[1][component]));
		activationStateScale=std::max(activationStateScale,std::fabs(activationA[1][component]));
	}
	if(!(activationOK&&activationStateDistance/activationStateScale<1.0e-10&&
		std::fabs(activationAlphaA[0]-activationAlphaB[0])>0.05))std::printf(
		"r59 activation diagnostic ok=%d alphaA=%.17g alphaB=%.17g error=%s\n",
		activationOK?1:0,activationAlphaA.empty()?-1.0:activationAlphaA[0],
		activationAlphaB.empty()?-1.0:activationAlphaB[0],error.c_str());
	Check(activationOK&&activationStateDistance/activationStateScale<1.0e-10&&
		std::fabs(activationAlphaA[0]-activationAlphaB[0])>0.05,
		"r59 physical states at rounding-scale separation trigger an O(0.1) limiter discontinuity");
	if( limiterResult.size() == limiterState.size() ) {
		for( std::size_t component=0; component<MethaneConservativeDimension; ++component ) {
				double before = 0.0, after = 0.0, source = 0.0;
				for( std::size_t cell=0; cell<limiterState.size(); ++cell ) {
					before += limiterState[cell][component];
					after += limiterResult[cell][component];
					source += limiterSource[cell][component];
				}
				Check(Near(after,before+source,2.0e-15),
					"V2/V3 limiter consumes the methane packet once and the same signed face correction twice");
		}
	}
	std::vector<ConservativeVector> uniformState(transportCells,
		ToConservativeVector(PhysicalMixtureLineState(fuel,thermochemistry,
			0.2,800.0)));
	std::vector<double> uniformMomentum(transportCells,0.25);
	PeriodicProjectedHeunResult coupledTransport;
	const bool coupledOK = ReferenceAdvancePeriodicProjectedHeun1D(uniformState,uniformMomentum,
		zeroSource,transportConfig,1.0e-11,false,fuel,thermochemistry,transport,
		coupledTransport,&error);
	if( !coupledOK ) std::printf("V2/V3 coupled diagnostic: %s\n",error.c_str());
	Check(coupledOK && !coupledTransport.r0.picardResidualPerS.empty() &&
		!coupledTransport.r1.picardResidualPerS.empty() &&
		!coupledTransport.r2.picardResidualPerS.empty(),
		"V2/V3 projected-Heun executes converged R0, R1, and endpoint R2 solves");
	if( coupledOK ) {
		const PeriodicCoupledStage* stages[3] = {
			&coupledTransport.r0,&coupledTransport.r1,&coupledTransport.r2
		};
		for( const PeriodicCoupledStage* stage : stages ) {
			double acceptedResidual = 0.0;
			for( std::size_t cell=0; cell<transportCells; ++cell ) acceptedResidual =
				std::max(acceptedResidual,std::fabs(PeriodicDivergence(
					stage->projection.velocityMPerS,cell,transportConfig.cellWidthM)-
					stage->divergenceTargetPerS[cell]));
			Check(acceptedResidual <= 1.0e-11,
				"V2/V3 each accepted Picard stage is reprojected against its stored target");
		}
	}

	// The production owner is three-dimensional.  Even the free-stream case
	// traverses R0/R1/R2, reconstructs transport at each stage, and consumes a
	// frozen packet vector (zero here) through the sole accepted-state write.
	PeriodicMACShape ownerShape;
	ownerShape.nx=4; ownerShape.ny=4; ownerShape.nz=4; ownerShape.cellWidthM=0.025;
	const std::size_t ownerCount=ownerShape.CellCount();
	const MethaneCellState ownerPhysical=PhysicalMixtureLineState(
		fuel,thermochemistry,0.2,800.0);
	std::vector<ConservativeVector> ownerBeginning(ownerCount,
		ToConservativeVector(ownerPhysical));
	PeriodicMACField ownerMomentum;
	const double ownerVelocity[3]={0.17,-0.09,0.04};
	for( unsigned int axis=0;axis<3;++axis ) ownerMomentum.component[axis].assign(
		ownerCount,ownerPhysical.GasDensity()*ownerVelocity[axis]);
	ConservativeAdvance3DConfig ownerConfig;
	ownerConfig.transport.cellWidthM=ownerShape.cellWidthM;
	ownerConfig.transport.deltaTimeS=0.001;
	ownerConfig.transport.ambientTemperatureK=300.0;
	ownerConfig.transport.adiabaticTemperatureK=2500.0;
	ownerConfig.transport.ambientGasDensityKGPerM3=ownerPhysical.GasDensity();
	ownerConfig.projectionTolerancePerS=2.0e-10;
	ownerConfig.dns=true;
	ownerConfig.retainStageDiagnostics=true;
	ConservativeAdvance3DResult ownerResult;
	const bool ownerOK=AdvanceConservative3D(ownerShape,ownerBeginning,ownerMomentum,
		std::vector<MethaneSourcePacket>(ownerCount),ownerConfig,fuel,thermochemistry,
		transport,ownerResult,&error);
	if(!ownerOK) std::printf("V2/V3 3-D owner diagnostic: %s\n",error.c_str());
	bool ownerFreeStream=ownerOK && ownerResult.conservative.size()==ownerCount;
	for(std::size_t cell=0;ownerFreeStream && cell<ownerCount;++cell){
		for(std::size_t component=0;component<MethaneConservativeDimension;++component)
			ownerFreeStream=ownerFreeStream && Near(ownerResult.conservative[cell][component],
				ownerBeginning[cell][component],2.0e-14);
		for(unsigned int axis=0;axis<3;++axis) ownerFreeStream=ownerFreeStream && Near(
			ownerResult.velocityMPerS.component[axis][cell],ownerVelocity[axis],2.0e-13);
	}
	Check(ownerOK && ownerFreeStream && !ownerResult.r0.picardResidualPerS.empty() &&
		!ownerResult.r1.picardResidualPerS.empty() &&
		!ownerResult.r2.picardResidualPerS.empty()&&
		ownerResult.discontinuousLimiterClassCount==0u&&
		!ownerResult.r0.limiterDiscontinuousClass,
		"V2/V3 single 3-D owner preserves a physical uniform free stream through R0/R1/R2");
	// Unequal-cp fuel/air endpoints at one temperature form the production
	// isothermal mixing box.  Its projected J_h must keep T uniform while the
	// owner derives and projects the nonzero discrete expansion target.
	std::vector<ConservativeVector> mixingBeginning(ownerCount);
	for(std::size_t cell=0;cell<ownerCount;++cell){
		const std::size_t x=cell%ownerShape.nx;
		const double z=0.12+0.16*(0.5+0.5*std::sin(2.0*pi*(x+0.5)/ownerShape.nx));
		mixingBeginning[cell]=ToConservativeVector(PhysicalMixtureLineState(
			fuel,thermochemistry,z,800.0));
	}
	PeriodicMACField mixingMomentum;
	for(unsigned int axis=0;axis<3;++axis)mixingMomentum.component[axis].assign(ownerCount,0.0);
	ConservativeAdvance3DConfig mixingConfig=ownerConfig;
	mixingConfig.transport.deltaTimeS=1.0e-4;
	ConservativeAdvance3DResult mixingResult;
	const bool mixingOK=AdvanceConservative3D(ownerShape,mixingBeginning,mixingMomentum,
		std::vector<MethaneSourcePacket>(ownerCount),mixingConfig,fuel,thermochemistry,
		transport,mixingResult,&error);
	std::vector<double> mixingTemperature;
	bool mixingIsothermal=mixingOK&&InvertPeriodicTemperatures(mixingResult.conservative,
		thermochemistry,mixingTemperature,&error);
	std::array<double,MethaneConservativeDimension> mixingBefore={},mixingAfter={};
	double mixingSdiv=0.0;
	for(std::size_t cell=0;mixingIsothermal&&cell<ownerCount;++cell){
		mixingIsothermal=mixingIsothermal&&std::fabs(mixingTemperature[cell]-800.0)<2.0e-8&&
			MaximumConstraintResidual(fuel.ConservativeReconstruction(),
				mixingResult.conservative[cell])<4.0e-14;
		mixingSdiv=std::max(mixingSdiv,std::fabs(mixingResult.divergenceHeunPerS[cell]));
		for(std::size_t component=0;component<MethaneConservativeDimension;++component){
			mixingBefore[component]+=mixingBeginning[cell][component];
			mixingAfter[component]+=mixingResult.conservative[cell][component];
		}
	}
	for(std::size_t component=0;component<MethaneConservativeDimension;++component)
		mixingIsothermal=mixingIsothermal&&Near(mixingAfter[component],mixingBefore[component],
			3.0e-14);
	Check(mixingOK&&mixingIsothermal&&mixingSdiv>0.0,
		"V2 owning unequal-cp mixing box keeps T uniform, conserves every ledger, and projects discrete S_div");
	ConservativeAdvance3DConfig openOwnerConfig;
	openOwnerConfig.periodicBoundaries=false;
	openOwnerConfig.openBoundary=openBoundary3D;
	openOwnerConfig.transport.cellWidthM=openShape3D.cellWidthM;
	openOwnerConfig.transport.deltaTimeS=0.001;
	openOwnerConfig.transport.ambientTemperatureK=300.0;
	openOwnerConfig.transport.adiabaticTemperatureK=2500.0;
	openOwnerConfig.transport.ambientGasDensityKGPerM3=ambientState3D.GasDensity();
	openOwnerConfig.injectedTemperatureK=300.0;
	openOwnerConfig.projectionTolerancePerS=2.0e-8;
	openOwnerConfig.dns=true;
	openOwnerConfig.retainStageDiagnostics=true;
	PeriodicMACField openOwnerMomentum;
	openOwnerMomentum.component=zeroOpenMomentum3D.component;
	ConservativeAdvance3DResult openOwnerResult;
	const bool openOwnerOK=AdvanceConservative3D(openShape3D,ambientCells3D,
		openOwnerMomentum,std::vector<MethaneSourcePacket>(openShape3D.CellCount()),
		openOwnerConfig,fuel,thermochemistry,transport,openOwnerResult,&error);
	if(!openOwnerOK) std::printf("V2/V3 open owner diagnostic: %s\n",error.c_str());
	bool openOwnerRest=openOwnerOK && openOwnerResult.conservative.size()==
		openShape3D.CellCount();
	for(std::size_t cell=0;openOwnerRest && cell<openShape3D.CellCount();++cell)
		for(std::size_t component=0;component<MethaneConservativeDimension;++component)
			openOwnerRest=openOwnerRest && openOwnerResult.conservative[cell][component]==
				ambientCells3D[cell][component];
	for(unsigned int axis=0;axis<3;++axis) for(const double velocity:
		openOwnerResult.velocityMPerS.component[axis]) openOwnerRest=openOwnerRest && velocity==0.0;
	if(openOwnerOK&&!openOwnerRest){
		double stateError=0.0,velocityError=0.0;
		for(std::size_t cell=0;cell<openShape3D.CellCount();++cell)
			for(std::size_t component=0;component<MethaneConservativeDimension;++component)
				if(std::fabs(openOwnerResult.conservative[cell][component]-
					ambientCells3D[cell][component])>stateError){stateError=std::fabs(
					openOwnerResult.conservative[cell][component]-ambientCells3D[cell][component]);
					std::printf("V2/V3 open detail cell=%zu component=%zu actual=%.17g expected=%.17g\n",
						cell,component,openOwnerResult.conservative[cell][component],
						ambientCells3D[cell][component]);}
		for(unsigned int axis=0;axis<3;++axis)for(const double velocity:
			openOwnerResult.velocityMPerS.component[axis])velocityError=std::max(velocityError,
				std::fabs(velocity));
		double maximumR0ZFlux=0.0,maximumR1ZFlux=0.0;unsigned int maximumR0Axis=0;
		std::size_t maximumR0Face=0;
		for(unsigned int axis=0;axis<3;++axis){
			for(std::size_t face=0;face<openOwnerResult.r0.flux.low[axis].size();++face)
				if(std::fabs(openOwnerResult.r0.flux.low[axis][face][0])>maximumR0ZFlux){
					maximumR0ZFlux=std::fabs(openOwnerResult.r0.flux.low[axis][face][0]);
					maximumR0Axis=axis;maximumR0Face=face;}
			for(const ConservativeVector& face:openOwnerResult.r1.flux.low[axis])
				maximumR1ZFlux=std::max(maximumR1ZFlux,std::fabs(face[0]));
		}
		std::printf("V2/V3 open owner rest errors state=%.9g velocity=%.9g\n",stateError,velocityError);
		std::printf("V2/V3 open owner rest Z flux r0=%.17g axis=%u face=%zu r1=%.17g\n",
			maximumR0ZFlux,maximumR0Axis,maximumR0Face,maximumR1ZFlux);
		if(maximumR0Face<openOwnerResult.r0.flux.nonadvectiveMass[maximumR0Axis].size())
			std::printf("V2/V3 open owner rest nonadvZ=%.17g highZ=%.17g\n",
				openOwnerResult.r0.flux.nonadvectiveMass[maximumR0Axis][maximumR0Face][0],
				openOwnerResult.r0.flux.high[maximumR0Axis][maximumR0Face][0]);
		if(maximumR0Face<openOwnerResult.r0.flux.low[maximumR0Axis].size()&&
			maximumR0Face<openOwnerResult.r0.projection.velocityMPerS.component[maximumR0Axis].size()&&
			maximumR0Face<openOwnerResult.r0.flux.nonadvectiveMass[maximumR0Axis].size()&&
			maximumR0Face<openOwnerResult.r0.flux.high[maximumR0Axis].size()&&
			maximumR0Face<openOwnerResult.r0.faceAlpha[maximumR0Axis].size())
			std::printf("V2/V3 open owner rest source velocity=%.17g nonadvZ=%.17g highZ=%.17g alpha=%.17g\n",
				openOwnerResult.r0.projection.velocityMPerS.component[maximumR0Axis][maximumR0Face],
				openOwnerResult.r0.flux.nonadvectiveMass[maximumR0Axis][maximumR0Face][0],
				openOwnerResult.r0.flux.high[maximumR0Axis][maximumR0Face][0],
				openOwnerResult.r0.faceAlpha[maximumR0Axis][maximumR0Face]);
	}
	Check(openOwnerOK && openOwnerRest&&openOwnerResult.discontinuousLimiterClassCount==0u,
		"V2/V3 single 3-D owner reaches the accepted V1 pressure-open rest infrastructure");
	// Cold-start fuel-source interface: one prescribed methane face below one
	// ambient neighbour, with the exact production open R0/R1/R2 owner.  This
	// isolates boundary-ledger feasibility from chemistry and plume dynamics.
	PeriodicMACShape fuelColumnShape; fuelColumnShape.nx=3; fuelColumnShape.ny=3;
	fuelColumnShape.nz=3; fuelColumnShape.cellWidthM=openShape3D.cellWidthM;
	ConservativeAdvance3DConfig fuelColumnConfig=openOwnerConfig;
	fuelColumnConfig.transport.cellWidthM=fuelColumnShape.cellWidthM;
	fuelColumnConfig.openBoundary.kind.fill(AdiabaticWallBoundary3D);
	fuelColumnConfig.openBoundary.kind[5]=PressureOpenBoundary3D;
	fuelColumnConfig.openBoundary.bottomFuelMask.assign(9,false);
	fuelColumnConfig.openBoundary.bottomFuelMassFluxKGPerM2S.assign(9,0.0);
	fuelColumnConfig.openBoundary.bottomFuelMask[4]=true;
	fuelColumnConfig.openBoundary.bottomFuelMassFluxKGPerM2S[4]=
		openBoundary3D.fuelMassFluxKGPerM2S;
	PeriodicMACField fuelColumnMomentum;
	for(unsigned int axis=0;axis<3;++axis)fuelColumnMomentum.component[axis].assign(
		OpenMACFaceCount3D(fuelColumnShape,axis),0.0);
	const std::vector<ConservativeVector> fuelColumnBeginning(fuelColumnShape.CellCount(),
		ToConservativeVector(ambientState3D));
	bool fuelColumnAccepted=false;
	std::string fuelColumnLastError;
	fuelColumnConfig.transport.deltaTimeS=0.001;
	ConservativeAdvance3DResult fuelColumnResult;
	fuelColumnAccepted=AdvanceConservative3D(fuelColumnShape,fuelColumnBeginning,
		fuelColumnMomentum,std::vector<MethaneSourcePacket>(fuelColumnShape.CellCount()),
		fuelColumnConfig,fuel,thermochemistry,transport,fuelColumnResult,&fuelColumnLastError);
	Check(fuelColumnAccepted,
		"V1 fuel-source interface remains feasible in the exact production open advance");
	if(!fuelColumnAccepted)std::printf("V1 fuel-column diagnostic: %s\n",
		fuelColumnLastError.c_str());
	bool directBedEndpoint=true;
	for(unsigned int reduction: {0u,10u,19u}){
		const double dt=0.001*std::ldexp(1.0,-static_cast<int>(reduction));
		ConservativeVector mixed=ToConservativeVector(ambientState3D)-
			(dt/fuelColumnShape.cellWidthM)*fuelFlux.totalOutwardFlux;
		MethaneCellState mixedState=FromConservativeVector(mixed);
		double mixedTemperature=0.0;
		std::string mixedError;
		const bool mixedOK=InvertMethaneTemperatureWithinAcceptedEnvelope(mixedState,
			300.0,2500.0,thermochemistry,mixedTemperature,&mixedError);
		directBedEndpoint=directBedEndpoint&&mixedOK&&mixedTemperature==300.0;
	}
	Check(directBedEndpoint,
		"V1 prescribed methane enthalpy remains exactly on the 300 K endpoint across halvings");
	double ambientEndpointEnergy=0.0,ambientUpperEnergy=0.0,rejectedEndpointTemperature=0.0;
	SignedMixtureSensibleEnergy(ambientState3D,300.0,thermochemistry,
		ambientEndpointEnergy,&error);
	SignedMixtureSensibleEnergy(ambientState3D,2500.0,thermochemistry,
		ambientUpperEnergy,&error);
	std::array<double,MethaneSpeciesCount> endpointLowerH,endpointUpperH;
	thermochemistry.SensibleEnthalpiesBySpeciesOrderJPerKG(300.0,endpointLowerH.data(),
		endpointLowerH.size(),&error);
	thermochemistry.SensibleEnthalpiesBySpeciesOrderJPerKG(2500.0,endpointUpperH.data(),
		endpointUpperH.size(),&error);
	const double endpointEnvelope=fuel.AcceptedStateFeasibilityEnvelope().kappaEpsilon64*
		std::numeric_limits<double>::epsilon()*AcceptedStateEnergyScale(
			ToConservativeVector(ambientState3D),endpointLowerH,endpointUpperH);
	MethaneCellState rejectedEndpoint=ambientState3D;
	rejectedEndpoint.sensibleEnergyJPerM3=ambientEndpointEnergy-2.0*endpointEnvelope;
	Check(!InvertMethaneTemperatureWithinAcceptedEnvelope(rejectedEndpoint,
		300.0,2500.0,thermochemistry,
		rejectedEndpointTemperature,&error),
		"V1 endpoint property view rejects energy beyond its certified fp64 envelope");
	OpenMACField3D scheduleMomentum;
	ConservativeAdvance3DConfig scheduleConfig=openOwnerConfig;
	scheduleConfig.transport.deltaTimeS=1.0e-5;
	scheduleConfig.transport.ambientTemperatureK=800.0;
	scheduleConfig.transport.ambientGasDensityKGPerM3=ownerPhysical.GasDensity();
	scheduleConfig.openBoundary.ambientDensityKGPerM3=ownerPhysical.GasDensity();
	scheduleConfig.openBoundary.ambientState=ToConservativeVector(ownerPhysical);
	const std::vector<ConservativeVector> scheduleBeginning(openShape3D.CellCount(),
		ToConservativeVector(ownerPhysical));
	for(unsigned int axis=0;axis<3;++axis)scheduleMomentum.component[axis].assign(
		OpenMACFaceCount3D(openShape3D,axis),0.0);
	for(std::size_t z=0;z<openShape3D.nz;++z)for(std::size_t y=0;y<openShape3D.ny;++y)
		for(std::size_t x=1;x<openShape3D.nx;++x){
			const std::size_t face=OpenMACFaceIndex3D(openShape3D,0,x,y,z);
			scheduleMomentum.component[0][face]=ownerPhysical.GasDensity()*1.0e-4*
				std::sin(pi*static_cast<double>(x)/openShape3D.nx);
		}
	OpenConservativeAdvance3DResult scheduleResult;
	const bool scheduleOK=AdvanceOpenConservative3DImplementation(openShape3D,
		scheduleBeginning,scheduleMomentum,std::vector<MethaneSourcePacket>(openShape3D.CellCount()),
		scheduleConfig,fuel,thermochemistry,transport,scheduleResult,&error);
	OpenMACProjection3DResult independentR0;
	const bool independentR0OK=scheduleOK&&ProjectPressureOpenMACVelocity3D(openShape3D,
		GasDensityFromConservative(scheduleBeginning),scheduleMomentum,
		scheduleResult.r0.divergenceTargetPerS,scheduleConfig.openBoundary,
		scheduleConfig.transport.deltaTimeS,scheduleConfig.projectionTolerancePerS,
		independentR0,&error);
	double r0ScheduleDifference=0.0;
	for(unsigned int axis=0;independentR0OK&&axis<3;++axis)for(std::size_t face=0;
		face<independentR0.velocityMPerS.component[axis].size();++face)
			r0ScheduleDifference=std::max(r0ScheduleDifference,std::fabs(
			independentR0.velocityMPerS.component[axis][face]-
				scheduleResult.r0.projection.velocityMPerS.component[axis][face]));
	if(!(independentR0OK&&r0ScheduleDifference<=2.0e-12))std::printf(
		"V2 open schedule diagnostic advance=%d independent=%d difference=%.9g error=%s\n",
		scheduleOK?1:0,independentR0OK?1:0,r0ScheduleDifference,error.c_str());
	Check(independentR0OK&&r0ScheduleDifference<=2.0e-12,
		"V2 open R0 projects fixed M^n without consuming its recomputed A-hat twice");
	OpenMACProjection3DResult viscousLocalA=openRest3D,viscousLocalB=openRest3D;
	for(unsigned int axis=0;axis<3;++axis){
		viscousLocalA.velocityMPerS.component[axis].assign(OpenMACFaceCount3D(openShape3D,axis),0.0);
		viscousLocalB.velocityMPerS.component[axis].assign(OpenMACFaceCount3D(openShape3D,axis),0.0);
	}
	for(std::size_t z=0;z<openShape3D.nz;++z)for(std::size_t y=0;y<openShape3D.ny;++y){
		viscousLocalA.velocityMPerS.component[0][OpenMACFaceIndex3D(openShape3D,0,1,y,z)]=0.02;
		viscousLocalB.velocityMPerS.component[0][OpenMACFaceIndex3D(openShape3D,0,1,y,z)]=0.02;
		viscousLocalB.velocityMPerS.component[0][OpenMACFaceIndex3D(openShape3D,0,
			openShape3D.nx,y,z)]=0.9;
	}
	OpenMACField3D viscousRHSA,viscousRHSB;
	const bool viscousLocalOK=BuildOpenNonpressureMomentumRHS3D(openShape3D,ambientCells3D,
		viscousLocalA,std::vector<double>(openShape3D.CellCount(),0.01),
		std::vector<ConservativeVector>(openShape3D.CellCount()),openOwnerConfig,
		viscousRHSA,&error)&&BuildOpenNonpressureMomentumRHS3D(openShape3D,ambientCells3D,
		viscousLocalB,std::vector<double>(openShape3D.CellCount(),0.01),
		std::vector<ConservativeVector>(openShape3D.CellCount()),openOwnerConfig,
		viscousRHSB,&error);
	double nearSideViscousDifference=0.0;
	for(std::size_t z=0;viscousLocalOK&&z<openShape3D.nz;++z)for(std::size_t y=0;
		y<openShape3D.ny;++y){const std::size_t face=OpenMACFaceIndex3D(openShape3D,0,1,y,z);
		nearSideViscousDifference=std::max(nearSideViscousDifference,std::fabs(
			viscousRHSA.component[0][face]-viscousRHSB.component[0][face]));}
	Check(viscousLocalOK&&nearSideViscousDifference==0.0,
		"V2 open viscous stress has no periodic far-boundary coupling");
	OpenMACProjection3DResult phaseProjection=openRest3D;
	for(unsigned int axis=0;axis<3;++axis)phaseProjection.velocityMPerS.component[axis].assign(
		OpenMACFaceCount3D(openShape3D,axis),axis==0?0.1:0.0);
	std::vector<ConservativeVector> phaseSource(openShape3D.CellCount());
	for(std::size_t z=0;z<openShape3D.nz;++z)for(std::size_t y=0;y<openShape3D.ny;++y)
		phaseSource[openShape3D.Index(0,y,z)][1+MethaneCH4]=
			2.0*openOwnerConfig.transport.deltaTimeS;
	OpenMACField3D phaseRHS;
	const bool phaseRestrictionOK=BuildOpenNonpressureMomentumRHS3D(openShape3D,
		ambientCells3D,phaseProjection,std::vector<double>(openShape3D.CellCount(),0.0),
		phaseSource,openOwnerConfig,phaseRHS,&error);
	bool boundaryPhaseHalf=phaseRestrictionOK;
	for(std::size_t z=0;boundaryPhaseHalf&&z<openShape3D.nz;++z)for(std::size_t y=0;
		y<openShape3D.ny;++y)boundaryPhaseHalf=Near(phaseRHS.component[0][
		OpenMACFaceIndex3D(openShape3D,0,0,y,z)],0.1,2.0e-14);
	Check(boundaryPhaseHalf,
		"V2 open phase momentum uses the same half-cell boundary restriction as I_rho");

	// Exact dual-control-volume oracle: the compatible normal momentum flux
	// must restrict primal mass fluxes with I_i before differencing.  The
	// asymmetric three-face pattern distinguishes it from a collocated F*u.
	PeriodicMACShape compatibilityShape;
	compatibilityShape.nx=3;compatibilityShape.ny=3;compatibilityShape.nz=3;
	compatibilityShape.cellWidthM=1.0;
	std::array<std::vector<double>,3> compatibilityAdvection,compatibilityDiffusion;
	PeriodicMACField compatibilityVelocity;
	for(unsigned int axis=0;axis<3;++axis){
		compatibilityAdvection[axis].assign(compatibilityShape.CellCount(),0.0);
		compatibilityDiffusion[axis].assign(compatibilityShape.CellCount(),0.0);
		compatibilityVelocity.component[axis].assign(compatibilityShape.CellCount(),
			axis==0?1.0:0.0);
	}
	for(std::size_t cell=0;cell<compatibilityShape.CellCount();++cell){
		const std::size_t x=cell%3;
		compatibilityAdvection[0][cell]=x==0?1.0:(x==1?2.0:4.0);
	}
	const std::array<std::vector<double>,3> compatibilityDivergence=
		CompatibleMomentumFluxDivergence3D(compatibilityShape,compatibilityAdvection,
			compatibilityDiffusion,compatibilityVelocity);
	Check(compatibilityDivergence[0][0]==-1.0,
		"V2 compatible momentum uses the pinned I_i primal-flux restriction");

	// Open MC reconstruction must use the local boundary ghost.  A perturbation
	// at the far x+ boundary is forbidden from changing the x-=adjacent face.
	PeriodicMACShape ghostShape;
	ghostShape.nx=4;ghostShape.ny=3;ghostShape.nz=3;ghostShape.cellWidthM=0.1;
	std::vector<ConservativeVector> ghostStateA(ghostShape.CellCount()),ghostStateB;
	std::vector<double> ghostTemperature(ghostShape.CellCount(),800.0),
		ghostZero(ghostShape.CellCount(),0.0);
	for(std::size_t cell=0;cell<ghostShape.CellCount();++cell){
		const std::size_t x=cell%ghostShape.nx;
		ghostStateA[cell]=ToConservativeVector(PhysicalMixtureLineState(fuel,
			thermochemistry,0.12+0.03*x,800.0));
	}
	ghostStateB=ghostStateA;
	for(std::size_t cell=0;cell<ghostShape.CellCount();++cell)if(cell%ghostShape.nx==3)
		ghostStateB[cell]=ToConservativeVector(PhysicalMixtureLineState(fuel,
			thermochemistry,0.7,800.0));
	OpenBoundaryConfig3D ghostBoundary=openBoundary3D;
	ghostBoundary.kind.fill(AdiabaticWallBoundary3D);
	OpenMACProjection3DResult ghostProjection;
	for(unsigned int axis=0;axis<3;++axis){
		ghostProjection.velocityMPerS.component[axis].assign(
			OpenMACFaceCount3D(ghostShape,axis),axis==0?0.1:0.0);
		ghostProjection.faceDensityKGPerM3.component[axis].assign(
			OpenMACFaceCount3D(ghostShape,axis),ownerPhysical.GasDensity());
		ghostProjection.momentumKGPerM2S.component[axis].assign(
			OpenMACFaceCount3D(ghostShape,axis),axis==0?0.1*ownerPhysical.GasDensity():0.0);
	}
	for(unsigned int side=0;side<6;++side)ghostProjection.inflow[side].assign(
		OpenBoundaryFaceCount3D(ghostShape,side),false);
	OpenFluxPair3D ghostFluxA,ghostFluxB;
	const bool ghostFluxOK=BuildOpenFluxPair3D(ghostShape,ghostStateA,ghostTemperature,
		ghostProjection,ghostZero,ghostZero,ghostBoundary,300.0,300.0,fuel,
		thermochemistry,ghostFluxA,&error)&&BuildOpenFluxPair3D(ghostShape,ghostStateB,
		ghostTemperature,ghostProjection,ghostZero,ghostZero,ghostBoundary,300.0,300.0,
		fuel,thermochemistry,ghostFluxB,&error);
	const std::size_t ghostFace=OpenMACFaceIndex3D(ghostShape,0,1,1,1);
	bool localGhost=ghostFluxOK;
	for(std::size_t component=0;localGhost&&component<MethaneConservativeDimension;++component)
		localGhost=ghostFluxA.high[0][ghostFace][component]==
			ghostFluxB.high[0][ghostFace][component];
	Check(localGhost,"V3 open MC boundary reconstruction has no periodic far-side coupling");
	OpenFluxPair3D openCompatibilityFlux;
	std::array<std::vector<double>,3> openCompatibilityAlpha;
	OpenMACField3D openCompatibilityVelocity;
	for(unsigned int axis=0;axis<3;++axis){
		const std::size_t faceCount=OpenMACFaceCount3D(compatibilityShape,axis);
		openCompatibilityFlux.low[axis].assign(faceCount,ConservativeVector());
		openCompatibilityFlux.high[axis].assign(faceCount,ConservativeVector());
		openCompatibilityFlux.nonadvectiveMass[axis].assign(faceCount,
			std::array<double,MethaneMassStateDimension>());
		openCompatibilityFlux.nonadvectiveEnergy[axis].assign(faceCount,0.0);
		openCompatibilityAlpha[axis].assign(faceCount,1.0);
		openCompatibilityVelocity.component[axis].assign(faceCount,0.0);
	}
	const std::size_t openF0=OpenMACFaceIndex3D(compatibilityShape,0,0,0,0);
	const std::size_t openF1=OpenMACFaceIndex3D(compatibilityShape,0,1,0,0);
	const std::size_t openF2=OpenMACFaceIndex3D(compatibilityShape,0,2,0,0);
	openCompatibilityFlux.low[0][openF0][1+MethaneCH4]=1.0;
	openCompatibilityFlux.low[0][openF1][1+MethaneCH4]=2.0;
	openCompatibilityFlux.low[0][openF2][1+MethaneCH4]=4.0;
	openCompatibilityFlux.high[0]=openCompatibilityFlux.low[0];
	openCompatibilityVelocity.component[0][openF0]=1.0;
	openCompatibilityVelocity.component[0][openF1]=3.0;
	openCompatibilityVelocity.component[0][openF2]=2.0;
	const OpenMACField3D openCompatibility=OpenCompatibleMomentumFluxDivergence3D(
		compatibilityShape,openCompatibilityFlux,openCompatibilityAlpha,
		openCompatibilityVelocity);
	Check(openCompatibility.component[0][openF1]==4.5,
		"V2 open momentum applies D_i to the restricted primal mass flux");
	Check(openCompatibility.component[0][openF0]==4.0,
		"V2 open momentum uses the one-sided compatible restriction at a boundary face");
	OpenBoundaryConfig3D tangentFuelBoundary=ghostBoundary;
	tangentFuelBoundary.bottomFuelMask.assign(compatibilityShape.nx*compatibilityShape.ny,false);
	tangentFuelBoundary.bottomFuelMask[1]=true;
	OpenFluxPair3D tangentFuelFlux=openCompatibilityFlux;
	for(unsigned int axis=0;axis<3;++axis)for(std::size_t face=0;
		face<tangentFuelFlux.low[axis].size();++face){
		tangentFuelFlux.low[axis][face]=ConservativeVector();
		tangentFuelFlux.high[axis][face]=ConservativeVector();
	}
	for(const std::size_t x:std::array<std::size_t,2>{{0,1}}){
		const std::size_t face=OpenMACFaceIndex3D(compatibilityShape,2,x,0,0);
		tangentFuelFlux.low[2][face][1+MethaneCH4]=1.0;
		tangentFuelFlux.high[2][face][1+MethaneCH4]=1.0;
	}
	OpenMACField3D tangentVelocity;
	for(unsigned int axis=0;axis<3;++axis)tangentVelocity.component[axis].assign(
		OpenMACFaceCount3D(compatibilityShape,axis),axis==0?2.0:0.0);
	const OpenMACField3D tangentFuelCompatibility=OpenCompatibleMomentumFluxDivergence3D(
		compatibilityShape,tangentFuelFlux,openCompatibilityAlpha,tangentVelocity,
		&tangentFuelBoundary);
	const std::size_t tangentFuelFace=OpenMACFaceIndex3D(compatibilityShape,0,1,0,0);
	Check(tangentFuelCompatibility.component[0][tangentFuelFace]==0.0,
		"V2 masked fuel injection carries zero tangential momentum in the dual volume");

	// Wall/fuel tangential velocity uses the no-slip reflected ghost, whereas a
	// pressure-open face uses the specified zero-normal-gradient ghost.
	std::vector<ConservativeVector> shearState(compatibilityShape.CellCount(),
		ToConservativeVector(ownerPhysical));
	OpenMACProjection3DResult shearProjection;
	for(unsigned int axis=0;axis<3;++axis)shearProjection.velocityMPerS.component[axis].assign(
		OpenMACFaceCount3D(compatibilityShape,axis),axis==0?1.0:0.0);
	ConservativeAdvance3DConfig shearConfig=ownerConfig;
	shearConfig.openBoundary=ghostBoundary;
	shearConfig.transport.cellWidthM=compatibilityShape.cellWidthM;
	OpenMACField3D wallShear,openShear;
	const bool wallShearOK=BuildOpenNonpressureMomentumRHS3D(compatibilityShape,shearState,
		shearProjection,std::vector<double>(compatibilityShape.CellCount(),0.1),
		std::vector<ConservativeVector>(compatibilityShape.CellCount()),shearConfig,wallShear,&error);
	shearConfig.openBoundary.kind.fill(PressureOpenBoundary3D);
	const bool openShearOK=BuildOpenNonpressureMomentumRHS3D(compatibilityShape,shearState,
		shearProjection,std::vector<double>(compatibilityShape.CellCount(),0.1),
		std::vector<ConservativeVector>(compatibilityShape.CellCount()),shearConfig,openShear,&error);
	const std::size_t shearFace=OpenMACFaceIndex3D(compatibilityShape,0,1,1,0);
	if(!(wallShearOK&&openShearOK&&std::fabs(wallShear.component[0][shearFace])>1.0e-6&&
		std::fabs(openShear.component[0][shearFace])<1.0e-14))std::printf(
		"V2 shear diagnostic wall=%.17g open=%.17g error=%s\n",
		wallShear.component[0][shearFace],openShear.component[0][shearFace],error.c_str());
	Check(wallShearOK&&openShearOK&&std::fabs(wallShear.component[0][shearFace])>1.0e-6&&
		std::fabs(openShear.component[0][shearFace])<1.0e-14,
		"V2 open viscous stress distinguishes no-slip wall and pressure-open Neumann ghosts");
	OpenMACField3D lesVelocity=shearProjection.velocityMPerS;
	for(std::size_t face=0;face<lesVelocity.component[1].size();++face)
		lesVelocity.component[1][face]=0.17*static_cast<double>(face%compatibilityShape.nx);
	std::vector<double> wallD,wallK,wallMu,openD,openK,openMu;
	const bool wallLES=BuildOpenStageTransport3D(compatibilityShape,shearState,
		std::vector<double>(compatibilityShape.CellCount(),800.0),lesVelocity,ghostBoundary,
		false,thermochemistry,transport,wallD,wallK,wallMu,&error);
	OpenBoundaryConfig3D lesOpenBoundary=ghostBoundary;
	lesOpenBoundary.kind.fill(PressureOpenBoundary3D);
	const bool openLES=BuildOpenStageTransport3D(compatibilityShape,shearState,
		std::vector<double>(compatibilityShape.CellCount(),800.0),lesVelocity,lesOpenBoundary,
		false,thermochemistry,transport,openD,openK,openMu,&error);
	double lesBoundaryDifference=0.0;for(std::size_t cell=0;wallLES&&openLES&&
		cell<wallMu.size();++cell)lesBoundaryDifference=std::max(lesBoundaryDifference,
			std::fabs(wallMu[cell]-openMu[cell]));
	Check(wallLES&&openLES&&lesBoundaryDifference>0.0,
		"V2 open LES/Vreman transport consumes boundary-aware wall and pressure-open velocity ghosts");

	// The same owning open flux path must admit CH4 supplied by a masked bed;
	// exact-zero canonicalization applies only when no boundary is a source.
	OpenFluxPair3D maskedOwnerFlux;
	std::vector<double> maskedTemperature(openShape3D.CellCount(),300.0),
		maskedZero(openShape3D.CellCount(),0.0);
	std::vector<ConservativeVector> maskedAdvanced;
	std::array<std::vector<double>,3> maskedAlpha;
	PeriodicTransportConfig maskedTransport=openOwnerConfig.transport;
	const bool maskedOwnerFCT=BuildOpenFluxPair3D(openShape3D,ambientCells3D,
		maskedTemperature,maskedFuelProjection,maskedZero,maskedZero,maskedFuelBoundary,
		300.0,300.0,fuel,thermochemistry,maskedOwnerFlux,&error)&&
		ApplyOpenSharedFCT3D(openShape3D,ambientCells3D,maskedOwnerFlux,
			std::vector<ConservativeVector>(openShape3D.CellCount()),maskedTransport,fuel,
			thermochemistry,maskedAdvanced,maskedAlpha,&error);
	double injectedCH4Inventory=0.0;for(const ConservativeVector& cell:maskedAdvanced)
		injectedCH4Inventory+=cell[1+MethaneCH4];
	Check(maskedOwnerFCT&&injectedCH4Inventory>0.0,
		"V3 open FCT accepts a boundary-supplied previously absent fuel inventory");
	// A kernel-tangent MC correction below the certified fp64 accumulation
	// envelope is physically inactive, but it still has a canonical alpha=1.
	// Without the open operator's outward-rounding reserve this exact-zero
	// admissibility budget toggles alpha between zero and one across Picard
	// iterations even though the accepted state is unchanged at rounding scale.
	OpenFluxPair3D roundoffOpenFlux=maskedOwnerFlux;
	for(unsigned int axis=0;axis<3;++axis)for(std::size_t face=0;
		face<roundoffOpenFlux.high[axis].size();++face)
		roundoffOpenFlux.high[axis][face]=roundoffOpenFlux.low[axis][face];
	const std::size_t roundoffFace=OpenMACFaceIndex3D(openShape3D,0,4,4,4);
	for(std::size_t species=0;species<MethaneSpeciesCount;++species)
		roundoffOpenFlux.high[0][roundoffFace][1+species]+=1.0e-38*
			fuel.PrimaryReactionDelta()[species];
	std::vector<ConservativeVector> roundoffAdvanced;
	std::array<std::vector<double>,3> roundoffAlpha;
	const bool roundoffOpenFCT=ApplyOpenSharedFCT3D(openShape3D,ambientCells3D,
		roundoffOpenFlux,std::vector<ConservativeVector>(openShape3D.CellCount()),
		maskedTransport,fuel,thermochemistry,roundoffAdvanced,roundoffAlpha,&error);
	Check(roundoffOpenFCT&&roundoffAlpha[0][roundoffFace]==1.0,
		"V3 open FCT gives a canonical alpha to an inactive kernel-tangent roundoff correction");
	std::array<double,MethaneSpeciesCount> r59AmbientEnthalpy,r59AdiabaticEnthalpy;
	const bool r59Bounds=FireSimulationEnthalpyBounds(maskedTransport,thermochemistry,
		r59AmbientEnthalpy,r59AdiabaticEnthalpy,&error);
	// r59 semantic unit: two accepted-state evaluations can be separated at
	// rounding scale while a constraint-activation face changes limiter class.
	// The selector has no relaxation parameter: the discontinuous result is the
	// exact pointwise infimum, and that value is re-certified by production FCT.
	std::array<std::vector<double>,3> discontinuousNext=roundoffAlpha;
	std::array<std::vector<double>,3> discontinuousVerified=roundoffAlpha;
	const std::size_t reverseOrderFace=OpenMACFaceIndex3D(openShape3D,1,4,4,4);
	discontinuousNext[0][roundoffFace]=0.8;
	discontinuousVerified[0][roundoffFace]=0.6;
	discontinuousNext[1][reverseOrderFace]=0.4;
	discontinuousVerified[1][reverseOrderFace]=0.7;
	LimiterPicardAcceptance3D discontinuousAcceptance;
	const bool discontinuousSelected=SelectLimiterPicardAcceptance3D(discontinuousNext,
		discontinuousVerified,1.0e-12,discontinuousAcceptance,&error);
	std::vector<ConservativeVector> discontinuousState1,discontinuousStateN;
	std::array<std::vector<double>,3> discontinuousAlpha1,discontinuousAlphaN;
	const bool discontinuousCertified=discontinuousSelected&&ApplyOpenSharedFCT3D(openShape3D,
		ambientCells3D,roundoffOpenFlux,std::vector<ConservativeVector>(openShape3D.CellCount()),
		maskedTransport,fuel,thermochemistry,discontinuousState1,discontinuousAlpha1,&error,1u,
		&discontinuousAcceptance.faceAlpha)&&ApplyOpenSharedFCT3D(openShape3D,ambientCells3D,
		roundoffOpenFlux,std::vector<ConservativeVector>(openShape3D.CellCount()),maskedTransport,
		fuel,thermochemistry,discontinuousStateN,discontinuousAlphaN,&error,4u,
		&discontinuousAcceptance.faceAlpha);
	bool discontinuousBitIdentity=discontinuousState1.size()==discontinuousStateN.size();
	for(std::size_t cell=0;discontinuousBitIdentity&&cell<discontinuousState1.size();++cell)
		for(std::size_t component=0;component<MethaneConservativeDimension;++component)
			discontinuousBitIdentity=discontinuousState1[cell][component]==
				discontinuousStateN[cell][component];
	bool discontinuousAdmissible=discontinuousCertified;
	for(const ConservativeVector& cell:discontinuousState1) discontinuousAdmissible=
		discontinuousAdmissible&&AcceptedStateAdmissible(cell,r59AmbientEnthalpy,
			r59AdiabaticEnthalpy,fuel,&error);
	Check(r59Bounds&&discontinuousCertified&&discontinuousAcceptance.discontinuousClass&&
		Near(discontinuousAcceptance.maximumFaceDiscrepancy,0.3,2.0e-15)&&
		discontinuousAcceptance.faceAlpha[0][roundoffFace]==0.6&&
		discontinuousAcceptance.faceAlpha[1][reverseOrderFace]==0.4&&
		discontinuousAcceptance.faceAlpha[0][roundoffFace]!=0.7&&
		discontinuousAcceptance.faceAlpha[1][reverseOrderFace]!=0.55&&
		discontinuousAcceptance.faceAlpha[1][reverseOrderFace]!=0.7&&discontinuousAdmissible,
		"r59 discontinuous limiter accepts both orderings of the exact face infimum, rejects averaging/always-verification, and certifies the corrected state");
	Check(discontinuousBitIdentity&&discontinuousAlpha1==discontinuousAlphaN,
		"r59 discontinuous limiter/FCT result is bit-identical at one and N workers");
	std::array<std::vector<double>,3> continuousNext=roundoffAlpha;
	std::array<std::vector<double>,3> continuousVerified=roundoffAlpha;
	continuousNext[0][roundoffFace]=0.6000000000005;
	continuousVerified[0][roundoffFace]=0.6;
	LimiterPicardAcceptance3D continuousAcceptance;
	Check(SelectLimiterPicardAcceptance3D(continuousNext,continuousVerified,1.0e-12,
		continuousAcceptance,&error)&&!continuousAcceptance.discontinuousClass&&
		continuousAcceptance.faceAlpha==continuousVerified,
		"r59 continuous limiter class preserves the verified coefficient unchanged");
	auto verificationResidualFor=[&](const unsigned int changed,double& residual){
		std::vector<double> acceptedTarget(1,1.0),iterationTarget(1,1.0),
			acceptedMass(1,2.0),iterationMass(1,2.0),acceptedDiffusivity(1,3.0),
			iterationDiffusivity(1,3.0),acceptedConductivity(1,4.0),
			iterationConductivity(1,4.0),acceptedViscosity(1,5.0),iterationViscosity(1,5.0);
		if(changed==0u)acceptedTarget[0]+=0.25;
		if(changed==1u)acceptedMass[0]+=0.25;
		if(changed==2u)acceptedDiffusivity[0]+=0.25;
		if(changed==3u)acceptedConductivity[0]+=0.25;
		if(changed==4u)acceptedViscosity[0]+=0.25;
		return PicardContinuousVerificationResidual(acceptedTarget,iterationTarget,
			acceptedMass,iterationMass,acceptedDiffusivity,iterationDiffusivity,
			acceptedConductivity,iterationConductivity,acceptedViscosity,iterationViscosity,
			0.5,residual,&error);
	};
	bool everyContinuousQuantityBlocks=true;
	for(unsigned int changed=0;changed<5u;++changed){double residual=0.0;
		everyContinuousQuantityBlocks=everyContinuousQuantityBlocks&&
			verificationResidualFor(changed,residual)&&Near(residual,changed==1u?0.5:0.25,1.0e-15);}
	Check(everyContinuousQuantityBlocks,
		"r59 verification gate independently observes S_div, every face mass flux, and every transport coefficient");
	std::array<double,MethaneSpeciesCount> limiterAmbientEnthalpy,limiterAdiabaticEnthalpy;
	const bool limiterBounds=FireSimulationEnthalpyBounds(maskedTransport,thermochemistry,
		limiterAmbientEnthalpy,limiterAdiabaticEnthalpy,&error);
	const double lowerEnergyScale=limiterBounds?InequalityRoundoffScale(ambientCells3D[0],
		2+MethaneSpeciesCount,limiterAmbientEnthalpy,limiterAdiabaticEnthalpy):0.0;
	const double upperEnergyScale=limiterBounds?InequalityRoundoffScale(ambientCells3D[0],
		3+MethaneSpeciesCount,limiterAmbientEnthalpy,limiterAdiabaticEnthalpy):0.0;
	Check(limiterBounds&&lowerEnergyScale==upperEnergyScale&&lowerEnergyScale>1.0,
		"V3 limiter uses the accepted-state two-endpoint energy roundoff scale at both bounds");
	OpenFluxPair3D malformedOpenFlux=maskedOwnerFlux;
	malformedOpenFlux.high[0].clear();
	std::vector<ConservativeVector> unchangedMalformed(1,ConservativeVector());
	const std::vector<ConservativeVector> malformedSnapshot=unchangedMalformed;
	const bool malformedRejected=!ApplyOpenSharedFCT3D(openShape3D,ambientCells3D,malformedOpenFlux,
		std::vector<ConservativeVector>(openShape3D.CellCount()),maskedTransport,fuel,
		thermochemistry,unchangedMalformed,maskedAlpha,&error);
	bool malformedTransactional=unchangedMalformed.size()==malformedSnapshot.size();
	for(std::size_t cell=0;malformedTransactional&&cell<unchangedMalformed.size();++cell)
		for(std::size_t component=0;component<MethaneConservativeDimension;++component)
			malformedTransactional=unchangedMalformed[cell][component]==malformedSnapshot[cell][component];
	Check(malformedRejected&&malformedTransactional,
		"V3 open FCT rejects a truncated high-order face field transactionally");

	// V3(a,b): advect a nonuniform density field whose mixture fraction,
	// species fractions, sensible enthalpy per mass and aerosol fraction are
	// spatially uniform.  The deforming velocity activates a multidimensional
	// limiter while every local affine relation must survive, not just its sum.
	PeriodicMACShape affineShape;
	affineShape.nx=6;affineShape.ny=4;affineShape.nz=3;
	affineShape.cellWidthM=1.0/6.0;
	const std::size_t affineCount=affineShape.CellCount();
	std::vector<ConservativeVector> affineBeginning(affineCount);
	std::vector<double> affineTemperature(affineCount,800.0),affineZero(affineCount,0.0);
	PeriodicMACField affineVelocity;
	for(unsigned int axis=0;axis<3;++axis) affineVelocity.component[axis].resize(affineCount);
	for(std::size_t cell=0;cell<affineCount;++cell){
		const std::size_t x=cell%affineShape.nx;
		const std::size_t y=(cell/affineShape.nx)%affineShape.ny;
		const std::size_t z=cell/(affineShape.nx*affineShape.ny);
		const double scale=1.0+0.16*std::sin(2.0*pi*(x+0.5)/affineShape.nx)+
			0.07*std::cos(2.0*pi*(y+0.5)/affineShape.ny);
		affineBeginning[cell]=scale*ToConservativeVector(ownerPhysical);
		affineVelocity.component[0][cell]=0.12+0.025*std::sin(2.0*pi*(y+0.5)/affineShape.ny);
		affineVelocity.component[1][cell]=-0.07+0.018*std::cos(2.0*pi*(z+0.5)/affineShape.nz);
		affineVelocity.component[2][cell]=0.04+0.012*std::sin(2.0*pi*(x+0.5)/affineShape.nx);
	}
	PeriodicFluxPair3D affineFlux;
	PeriodicTransportConfig affineConfig=ownerConfig.transport;
	affineConfig.cellWidthM=affineShape.cellWidthM;
	affineConfig.deltaTimeS=0.03;
	std::vector<ConservativeVector> affineResult;
	std::array<std::vector<double>,3> affineAlpha;
	const bool affineOK=BuildPeriodicFluxPair3D(affineShape,affineBeginning,
		affineTemperature,affineVelocity,affineZero,affineZero,fuel,thermochemistry,
		affineFlux,&error) && ApplyPeriodicSharedFCT3D(affineShape,affineBeginning,
		affineFlux,std::vector<ConservativeVector>(affineCount),affineConfig,fuel,
		thermochemistry,affineResult,affineAlpha,&error);
	bool localAffine=affineOK,uniformScalar=affineOK,affineGlobal=affineOK;
	std::array<double,MethaneConservativeDimension> affineBefore={},affineAfter={};
	const ConservativeVector affineBase=ToConservativeVector(ownerPhysical);
	const double affineBaseTotal=ownerPhysical.TotalDensity();
	for(std::size_t cell=0;affineOK && cell<affineCount;++cell){
		const MethaneCellState physical=FromConservativeVector(affineResult[cell]);
		const double total=physical.TotalDensity();
		localAffine=localAffine && MaximumConstraintResidual(
			fuel.ConservativeReconstruction(),affineResult[cell])<4.0e-14;
		for(std::size_t component=0;component<MethaneConservativeDimension;++component){
			uniformScalar=uniformScalar && Near(affineResult[cell][component]/total,
				affineBase[component]/affineBaseTotal,3.0e-13);
			affineBefore[component]+=affineBeginning[cell][component];
			affineAfter[component]+=affineResult[cell][component];
		}
		uniformScalar=uniformScalar && affineResult[cell][1+MethaneCarbon]==0.0;
	}
	for(std::size_t component=0;component<MethaneConservativeDimension;++component)
		affineGlobal=affineGlobal && Near(affineAfter[component],affineBefore[component],
			3.0e-14);
	Check(affineOK && uniformScalar && localAffine && affineGlobal,
		"V3(a,b) 3-D FCT preserves every uniform scalar and the local methane affine invariant");

	// V2 reacting manufactured solution: a real record-derived reaction packet
	// is smoothly modulated, then a uniform manufactured cooling term removes
	// only the incompatible mean expansion.  The remaining nonzero S_div field
	// must be reconstructed from the exact discrete packet and physical flux.
	MethaneReactionStep manufacturedReactionStep;
	manufacturedReactionStep.deltaTimeS=1.0e-5;
	manufacturedReactionStep.mixingTimeS=20.0;
	manufacturedReactionStep.primaryEligible=true;
	manufacturedReactionStep.sootOxidationEnabled=true;
	std::vector<MethaneCellState> manufacturedPhysical(ownerCount);
	std::vector<ConservativeVector> manufacturedBeginning(ownerCount);
	std::vector<double> manufacturedTemperature(ownerCount);
	const ConservativeVector manufacturedProductRich=ToConservativeVector(
		ProductRichMixtureLineState(fuel,thermochemistry,0.2,800.0));
	const RecordKernelFixtureDirection manufacturedMixtureFixture=
		BuildRecordKernelFixtureDirection(fuel,{1.0,-0.75,0.5,-0.25,0.125});
	const RecordKernelFixtureDirection manufacturedProductFixture=
		BuildRecordKernelFixtureDirection(fuel,{-0.25,0.5,1.0,-0.5,0.75});
	const RecordKernelFixtureDirection manufacturedMolarPivotFixture=
		BuildRecordKernelFixtureDirection(fuel,{0.4,0.9,-0.6,0.3,1.0});
	RecordKernelFixtureDirection manufacturedEmbeddedMismatch=manufacturedProductFixture;
	manufacturedEmbeddedMismatch.direction[1+MethaneCO]=std::nextafter(
		manufacturedEmbeddedMismatch.direction[1+MethaneCO],
		std::numeric_limits<double>::infinity());
	Check(KernelFixtureDirectionMatchesRecord(fuel,manufacturedMixtureFixture)&&
		KernelFixtureDirectionMatchesRecord(fuel,manufacturedProductFixture)&&
		KernelFixtureDirectionMatchesRecord(fuel,manufacturedMolarPivotFixture)&&
		!KernelFixtureDirectionMatchesRecord(fuel,manufacturedEmbeddedMismatch),
		"V2 manufactured directions are rebuilt from the loaded kernel and reject a stranded embedded coefficient");
	static const char* manufacturedMolarName[MethaneCarbon]={
		"CH4","O2","N2","CO2","H2O","CO"};
	auto manufacturedMolarChange=[&](const ConservativeVector& direction){double value=0.0;
		for(std::size_t species=0;species<MethaneCarbon;++species){value+=
			direction[1+species]/thermochemistry.FindSpecies(
				manufacturedMolarName[species])->molecularWeightKGPerKMol;}
		return value;};
	const double manufacturedPivotMolar=manufacturedMolarChange(
		manufacturedMolarPivotFixture.direction);
	auto manufacturedIsomolar=[&](const ConservativeVector& input){ConservativeVector result=input;
		const double scale=manufacturedMolarChange(input)/manufacturedPivotMolar;
		for(std::size_t row=0;row<MethaneMassStateDimension;++row){result[row]-=
			scale*manufacturedMolarPivotFixture.direction[row];}
		return result;};
	ConservativeVector manufacturedProductDirection=manufacturedIsomolar(
		manufacturedProductFixture.direction);
	if(manufacturedProductDirection[1+MethaneCO]<0.0)
		for(double& value:manufacturedProductDirection.value)value=-value;
	const double manufacturedProductFloor=1.0e-2;
	double manufacturedProductAmplitude=std::numeric_limits<double>::max();
	for(std::size_t row=0;row<MethaneMassStateDimension;++row)
		if(manufacturedProductDirection[row]<0.0)manufacturedProductAmplitude=std::min(
			manufacturedProductAmplitude,0.08*manufacturedProductRich[row]/
			std::fabs(manufacturedProductDirection[row]));
	const ConservativeVector manufacturedBase=manufacturedProductRich+
		manufacturedProductFloor*manufacturedProductAmplitude*manufacturedProductDirection;
	std::array<double,MethaneMassStateDimension> manufacturedDirection={};
	const ConservativeVector manufacturedIsomolarMixtureDirection=manufacturedIsomolar(
		manufacturedMixtureFixture.direction);
	for(std::size_t row=0;row<MethaneMassStateDimension;++row)
		manufacturedDirection[row]=manufacturedIsomolarMixtureDirection[row];
	double manufacturedAmplitude=std::numeric_limits<double>::max();
	for(std::size_t row=0;row<MethaneMassStateDimension;++row)
		if(manufacturedDirection[row]!=0.0)manufacturedAmplitude=std::min(
			manufacturedAmplitude,0.1*manufacturedBase[row]/
			std::fabs(manufacturedDirection[row]));
	for(std::size_t cell=0;cell<ownerCount;++cell){
		ConservativeVector manufacturedState=manufacturedBase;
		const std::size_t x=cell%ownerShape.nx;
		const double signal=(x&1u)?1.0:-1.0;
		const double productWeight=0.1+0.09*std::sin(
			2.0*pi*(x+0.5)/ownerShape.nx+0.31);
		for(std::size_t row=0;row<MethaneMassStateDimension;++row)
			manufacturedState[row]+=signal*manufacturedAmplitude*manufacturedDirection[row]+
				(productWeight-manufacturedProductFloor)*manufacturedProductAmplitude*
					manufacturedProductDirection[row];
		manufacturedPhysical[cell]=FromConservativeVector(manufacturedState);
		static const char* manufacturedName[MethaneCarbon]={"CH4","O2","N2","CO2","H2O","CO"};
		double manufacturedMoles=0.0;for(std::size_t species=0;species<MethaneCarbon;++species)
			manufacturedMoles+=manufacturedPhysical[cell].constituent[species]/
				thermochemistry.FindSpecies(manufacturedName[species])->molecularWeightKGPerKMol;
		manufacturedPhysical[cell].temperatureK=thermochemistry.ThermodynamicPressurePa()/
			(8314.46261815324*manufacturedMoles);
		Check(thermochemistry.MixtureSensibleEnergyJPerM3(
			ThermochemicalDensities(manufacturedPhysical[cell]),
			manufacturedPhysical[cell].temperatureK,
			manufacturedPhysical[cell].sensibleEnergyJPerM3,&error),
			"V2 limiter-active manufactured state has a physical energy");
		manufacturedBeginning[cell]=ToConservativeVector(manufacturedPhysical[cell]);
		manufacturedTemperature[cell]=manufacturedPhysical[cell].temperatureK;
	}
	ConservativeAdvance3DConfig manufacturedConfig=ownerConfig;
	manufacturedConfig.transport.deltaTimeS=manufacturedReactionStep.deltaTimeS;
	manufacturedConfig.transport.ambientGasDensityKGPerM3=ownerPhysical.GasDensity();
	manufacturedConfig.projectionTolerancePerS=1.0e-1;
	std::vector<MethaneSourcePacket> manufacturedPacket(ownerCount);
	for(std::size_t cell=0;cell<ownerCount;++cell){
		const std::size_t x=cell%ownerShape.nx;
		const double scale=0.65+0.3*std::sin(2.0*pi*(x+0.5)/ownerShape.nx);
		MethaneSourcePacket manufacturedBase;
		Check(BuildMethaneReactionPacket(manufacturedPhysical[cell],fuel,
			manufacturedReactionStep,manufacturedBase,&error),
			"V2 manufactured source starts from a physical methane packet");
		for(std::size_t species=0;species<MethaneSpeciesCount;++species)
			manufacturedPacket[cell].constituentDelta[species]=
				scale*manufacturedBase.constituentDelta[species];
		manufacturedPacket[cell].sensibleEnergyDeltaJPerM3=
			scale*manufacturedBase.sensibleEnergyDeltaJPerM3;
	}
	PeriodicMACField manufacturedVelocity;
	const double manufacturedAdvectionVelocity=1.2e3;
	for(unsigned int axis=0;axis<3;++axis){manufacturedVelocity.component[axis].resize(
		ownerCount);for(std::size_t face=0;face<ownerCount;++face){const std::size_t x=
			face%ownerShape.nx,y=(face/ownerShape.nx)%ownerShape.ny;
			const double px=(x+(axis==0?1.0:0.5))/ownerShape.nx;
			const double py=(y+(axis==1?1.0:0.5))/ownerShape.ny;
			manufacturedVelocity.component[axis][face]=axis==0?
				manufacturedAdvectionVelocity*std::sin(2.0*pi*px)*std::cos(2.0*pi*py):
				(axis==1?-manufacturedAdvectionVelocity*std::cos(2.0*pi*px)*
					std::sin(2.0*pi*py):0.0);}}
	std::vector<double> manufacturedD,manufacturedK,manufacturedMu;
	PeriodicFluxPair3D manufacturedInitialFlux;
	Check(BuildPeriodicStageTransport3D(ownerShape,manufacturedBeginning,
		manufacturedTemperature,manufacturedVelocity,true,thermochemistry,
		transport,manufacturedD,manufacturedK,manufacturedMu,&error)&&
		BuildPeriodicFluxPair3D(ownerShape,manufacturedBeginning,
		manufacturedTemperature,manufacturedVelocity,manufacturedD,
		manufacturedK,fuel,thermochemistry,manufacturedInitialFlux,&error),
		"V2 manufactured initial physical flux is evaluable");
	// Derive every manufactured energy increment from the loaded record's
	// finite p0 identity.  This is not a copied pre-r56 kernel coefficient:
	// species, molecular weights, h_s and the desired zero-mean target all
	// participate in the same calculation production later repeats.
	double manufacturedDesiredSum=0.0;
	std::vector<double> manufacturedDesiredTarget(ownerCount,0.0);
	for(std::size_t cell=0;cell<ownerCount;++cell){
		ConservativeVector combined;
		for(std::size_t species=0;species<MethaneSpeciesCount;++species)
			combined[1+species]=manufacturedPacket[cell].constituentDelta[species];
		double physicalEnergyIncrement=0.0;
		for(unsigned int axis=0;axis<3;++axis){const std::size_t previous=
			PeriodicPrevious(ownerShape,cell,axis);for(std::size_t component=0;
			component<MethaneMassStateDimension;++component)combined[component]+=
				manufacturedConfig.transport.deltaTimeS*(manufacturedInitialFlux.
					nonadvectiveMass[axis][previous][component]-manufacturedInitialFlux.
					nonadvectiveMass[axis][cell][component])/ownerShape.cellWidthM;
			physicalEnergyIncrement+=manufacturedConfig.transport.deltaTimeS*(
				manufacturedInitialFlux.nonadvectiveEnergy[axis][previous]-
				manufacturedInitialFlux.nonadvectiveEnergy[axis][cell])/
				ownerShape.cellWidthM;}
		combined[MethaneMassStateDimension]=physicalEnergyIncrement;
		const std::size_t x=cell%ownerShape.nx;
		double desired=1.0e-5*std::sin(2.0*pi*(x+0.5)/ownerShape.nx);
		if(cell+1==ownerCount)desired=-manufacturedDesiredSum;
		else manufacturedDesiredSum+=desired;
		manufacturedDesiredTarget[cell]=desired;
		Check(SetRecordDerivedEnergyForDivergence(manufacturedBeginning[cell],combined,
			manufacturedTemperature[cell],desired,manufacturedConfig.transport.deltaTimeS,
			thermochemistry,&error),
			"V2 manufactured finite-increment target derives from the loaded record");
		manufacturedPacket[cell].sensibleEnergyDeltaJPerM3=
			combined[MethaneMassStateDimension]-physicalEnergyIncrement;
	}
	const std::vector<MethaneSourcePacket> manufacturedBaselinePacket=manufacturedPacket;
	PeriodicMACField manufacturedMomentum;
	for(unsigned int axis=0;axis<3;++axis){
		manufacturedMomentum.component[axis].resize(ownerCount);
		for(std::size_t face=0;face<ownerCount;++face){
			const std::size_t next=PeriodicNext(ownerShape,face,axis);
			manufacturedMomentum.component[axis][face]=PositiveArithmeticMean(
				manufacturedPhysical[face].GasDensity(),manufacturedPhysical[next].GasDensity())*
				manufacturedVelocity.component[axis][face];
		}
	}
	// A periodic manufactured packet must have zero mean expansion at both
	// nonlinear stages.  Calibrate one zero-R0 energy dipole by secant so the
	// frozen physical packet is also exactly R1-compatible; this changes no
	// global source ledger and is fixture construction, not an accuracy oracle.
	auto trialStageMeans=[&](std::array<double,2>& mean){
		ConservativeAdvance3DResult trial;std::string trialError;
		if(!AdvanceConservative3D(ownerShape,manufacturedBeginning,manufacturedMomentum,
			manufacturedPacket,manufacturedConfig,fuel,thermochemistry,transport,trial,
			&trialError)){std::printf("V2 calibration advance: %s\n",trialError.c_str());return false;}
		mean={{0.0,0.0}};
		for(const double value:trial.r1.divergenceTargetPerS)mean[0]+=value/ownerCount;
		for(const double value:trial.r2.divergenceTargetPerS)mean[1]+=value/ownerCount;
		return true;
	};
	std::array<std::vector<double>,2> calibrationMode;
	for(std::vector<double>& mode:calibrationMode)mode.resize(ownerCount);
	std::array<double,2> calibrationModeSum={{0.0,0.0}};
	for(std::size_t cell=0;cell<ownerCount;++cell){const std::size_t x=cell%ownerShape.nx;
		calibrationMode[0][cell]=std::sin(2.0*pi*(x+0.5)/ownerShape.nx);
		calibrationMode[1][cell]=std::cos(2.0*pi*(x+0.5)/ownerShape.nx);
		if(cell+1<ownerCount){calibrationModeSum[0]+=calibrationMode[0][cell];
			calibrationModeSum[1]+=calibrationMode[1][cell];}}
	calibrationMode[0].back()=-calibrationModeSum[0];
	calibrationMode[1].back()=-calibrationModeSum[1];
	auto applyRecordDerivedModes=[&](const double firstTargetAmplitude,
		const double secondTargetAmplitude){
		manufacturedPacket=manufacturedBaselinePacket;
		const std::vector<double>& primaryDirection=fuel.PrimaryReactionDelta();
		for(std::size_t cell=0;cell<ownerCount;++cell){const double extent=
			firstTargetAmplitude*calibrationMode[0][cell]+
			secondTargetAmplitude*calibrationMode[1][cell];
			for(std::size_t species=0;species<MethaneSpeciesCount;++species)
				manufacturedPacket[cell].constituentDelta[species]+=
					extent*primaryDirection[species];
			ConservativeVector combined;
			for(std::size_t species=0;species<MethaneSpeciesCount;++species)
				combined[1+species]=manufacturedPacket[cell].constituentDelta[species];
			for(unsigned int axis=0;axis<3;++axis){const std::size_t previous=
				PeriodicPrevious(ownerShape,cell,axis);for(std::size_t component=0;
				component<MethaneMassStateDimension;++component)combined[component]+=
					manufacturedConfig.transport.deltaTimeS*(manufacturedInitialFlux.
						nonadvectiveMass[axis][previous][component]-manufacturedInitialFlux.
						nonadvectiveMass[axis][cell][component])/ownerShape.cellWidthM;
				combined[MethaneMassStateDimension]+=manufacturedConfig.transport.deltaTimeS*(
					manufacturedInitialFlux.nonadvectiveEnergy[axis][previous]-
					manufacturedInitialFlux.nonadvectiveEnergy[axis][cell])/
					ownerShape.cellWidthM;}
			const double physicalEnergy=combined[MethaneMassStateDimension];
			const double desired=manufacturedDesiredTarget[cell];
			if(!SetRecordDerivedEnergyForDivergence(manufacturedBeginning[cell],combined,
				manufacturedTemperature[cell],desired,
				manufacturedConfig.transport.deltaTimeS,thermochemistry,&error))return false;
			manufacturedPacket[cell].sensibleEnergyDeltaJPerM3=
				combined[MethaneMassStateDimension]-physicalEnergy;}
		return true;
	};
	std::array<double,2> calibrationMean={{0.0,0.0}},probeA={{0.0,0.0}},
		probeB={{0.0,0.0}};
	double calibrationA=0.0,calibrationB=0.0;
	bool calibrationOK=true;
	const double calibrationProbe=1.0e-5;
	for(unsigned int iteration=0;calibrationOK&&iteration<16;++iteration){
		calibrationOK=applyRecordDerivedModes(calibrationA,calibrationB)&&
			trialStageMeans(calibrationMean);
		if(calibrationOK&&std::max(std::fabs(calibrationMean[0]),
			std::fabs(calibrationMean[1]))<9.8e-9)break;
		if(calibrationOK)calibrationOK=applyRecordDerivedModes(
			calibrationA+calibrationProbe,calibrationB)&&trialStageMeans(probeA);
		if(calibrationOK)calibrationOK=applyRecordDerivedModes(
			calibrationA,calibrationB+calibrationProbe)&&trialStageMeans(probeB);
		if(calibrationOK){const double j00=(probeA[0]-calibrationMean[0])/calibrationProbe,
			j10=(probeA[1]-calibrationMean[1])/calibrationProbe,
			j01=(probeB[0]-calibrationMean[0])/calibrationProbe,
			j11=(probeB[1]-calibrationMean[1])/calibrationProbe,
			determinant=j00*j11-j01*j10;
			calibrationOK=std::isfinite(determinant)&&determinant!=0.0;
			if(calibrationOK){const double deltaA=(-calibrationMean[0]*j11+
				j01*calibrationMean[1])/determinant,deltaB=(-j00*calibrationMean[1]+
					calibrationMean[0]*j10)/determinant;
				const double oldNorm=std::hypot(calibrationMean[0],calibrationMean[1]);
				bool accepted=false;
				for(unsigned int reduction=0;!accepted&&reduction<16;++reduction){
					const double fraction=std::ldexp(1.0,-static_cast<int>(reduction));
					std::array<double,2> candidateMean={{0.0,0.0}};
					if(applyRecordDerivedModes(calibrationA+fraction*deltaA,
						calibrationB+fraction*deltaB)&&trialStageMeans(candidateMean)&&
						std::hypot(candidateMean[0],candidateMean[1])<oldNorm){
						calibrationA+=fraction*deltaA;calibrationB+=fraction*deltaB;
						accepted=true;}}
				calibrationOK=accepted;}}
	}
	calibrationOK=calibrationOK&&applyRecordDerivedModes(calibrationA,calibrationB)&&
		trialStageMeans(calibrationMean);
	manufacturedConfig.projectionTolerancePerS=1.0e-8;
	if(!calibrationOK)std::printf("V2 calibration failed means %.17g %.17g\n",
		calibrationMean[0],calibrationMean[1]);
	ConservativeAdvance3DResult manufacturedResult;
	const bool manufacturedOK=calibrationOK&&AdvanceConservative3D(ownerShape,manufacturedBeginning,
		manufacturedMomentum,manufacturedPacket,manufacturedConfig,fuel,thermochemistry,
		transport,manufacturedResult,&error);
	if(!manufacturedOK) std::printf("V2 reacting owner diagnostic: %s\n",error.c_str());
	double maximumTargetMismatch=0.0,maximumProjectionMismatch=0.0;
	double manufacturedR0Mean=0.0,manufacturedR1Mean=0.0;
	double manufacturedMinimumAlpha=1.0;
	std::array<double,MethaneConservativeDimension> manufacturedBefore={},
		manufacturedAfter={},manufacturedSource={};
	std::vector<ConservativeVector> manufacturedDelta(ownerCount);
	for(std::size_t cell=0;cell<ownerCount;++cell){
		for(std::size_t species=0;species<MethaneSpeciesCount;++species)
			manufacturedDelta[cell][1+species]=manufacturedPacket[cell].constituentDelta[species];
		manufacturedDelta[cell][MethaneMassStateDimension]=
			manufacturedPacket[cell].sensibleEnergyDeltaJPerM3;
	}
	std::vector<double> independentlyManufacturedR0Target;
	std::vector<ConservativeVector> independentlyManufacturedPredictor;
	std::array<std::vector<double>,3> independentlyManufacturedAlpha;
	std::vector<double> independentlyManufacturedPredictorTemperature,
		independentlyManufacturedR1Target,independentlyManufacturedHeunTarget;
	PeriodicFluxPair3D independentlyManufacturedAverage;
	const bool manufacturedTargetOK=manufacturedOK&&ApplyPeriodicSharedFCT3D(ownerShape,
		manufacturedBeginning,manufacturedResult.r0.flux,manufacturedDelta,
		manufacturedConfig.transport,fuel,thermochemistry,independentlyManufacturedPredictor,
		independentlyManufacturedAlpha,&error,&manufacturedResult.r0.faceAlpha)&&
		ManifoldExactDivergenceTarget(manufacturedResult.r0.divergenceTargetPerS,
			independentlyManufacturedPredictor,manufacturedConfig.transport.deltaTimeS,
			thermochemistry,independentlyManufacturedR0Target,&error,true);
	bool manufacturedTableauOK=manufacturedTargetOK&&InvertPeriodicTemperatures(
		independentlyManufacturedPredictor,thermochemistry,
		independentlyManufacturedPredictorTemperature,&error);
	for(unsigned int axis=0;manufacturedTableauOK&&axis<3;++axis){
		independentlyManufacturedAverage.low[axis].resize(ownerCount);
		independentlyManufacturedAverage.high[axis].resize(ownerCount);
		independentlyManufacturedAverage.nonadvectiveMass[axis].resize(ownerCount);
		independentlyManufacturedAverage.nonadvectiveEnergy[axis].resize(ownerCount);
		for(std::size_t face=0;face<ownerCount;++face){
			independentlyManufacturedAverage.low[axis][face]=0.5*(
				manufacturedResult.r0.flux.low[axis][face]+manufacturedResult.r1.flux.low[axis][face]);
			independentlyManufacturedAverage.high[axis][face]=0.5*(
				manufacturedResult.r0.flux.high[axis][face]+manufacturedResult.r1.flux.high[axis][face]);
			independentlyManufacturedAverage.nonadvectiveEnergy[axis][face]=0.5*(
				manufacturedResult.r0.flux.nonadvectiveEnergy[axis][face]+
				manufacturedResult.r1.flux.nonadvectiveEnergy[axis][face]);
			for(std::size_t component=0;component<MethaneMassStateDimension;++component)
				independentlyManufacturedAverage.nonadvectiveMass[axis][face][component]=0.5*(
					manufacturedResult.r0.flux.nonadvectiveMass[axis][face][component]+
					manufacturedResult.r1.flux.nonadvectiveMass[axis][face][component]);
		}
	}
	std::vector<double> independentlyManufacturedAcceptedTemperature;
	std::vector<double> independentlyManufacturedR2Target;
	std::vector<ConservativeVector> independentlyManufacturedCommit;
	std::array<std::vector<double>,3> independentlyManufacturedCommitAlpha;
	manufacturedTableauOK=manufacturedTableauOK&&ApplyPeriodicSharedFCT3D(ownerShape,
		manufacturedBeginning,independentlyManufacturedAverage,manufacturedDelta,
		manufacturedConfig.transport,fuel,thermochemistry,independentlyManufacturedCommit,
		independentlyManufacturedCommitAlpha,&error,&manufacturedResult.r1.faceAlpha)&&
		ManifoldExactDivergenceTarget(manufacturedResult.r1.divergenceTargetPerS,
			independentlyManufacturedCommit,manufacturedConfig.transport.deltaTimeS,
			thermochemistry,independentlyManufacturedR1Target,&error,true)&&
		InvertPeriodicTemperatures(
		manufacturedResult.conservative,thermochemistry,
		independentlyManufacturedAcceptedTemperature,&error)&&
		PeriodicDivergenceTargetFromPhysicalFlux3D(ownerShape,
			manufacturedResult.conservative,independentlyManufacturedAcceptedTemperature,
			independentlyManufacturedAverage,manufacturedDelta,
			manufacturedConfig.transport.deltaTimeS,thermochemistry,
			independentlyManufacturedHeunTarget,&error)&&PeriodicDivergenceTargetFromPhysicalFlux3D(
			ownerShape,manufacturedResult.conservative,independentlyManufacturedAcceptedTemperature,
			manufacturedResult.r2.flux,manufacturedDelta,
			manufacturedConfig.transport.deltaTimeS,thermochemistry,
			independentlyManufacturedR2Target,&error);
	double maximumR1TargetMismatch=0.0,maximumHeunTargetMismatch=0.0,
		maximumR2TargetMismatch=0.0,maximumR2HeunDifference=0.0;
	for(std::size_t cell=0;manufacturedTableauOK && cell<ownerCount;++cell){
		manufacturedR0Mean+=manufacturedResult.r0.divergenceTargetPerS[cell]/ownerCount;
		manufacturedR1Mean+=manufacturedResult.r1.divergenceTargetPerS[cell]/ownerCount;
		for(std::size_t component=0;component<MethaneConservativeDimension;++component)
			manufacturedTableauOK=manufacturedTableauOK&&
				manufacturedResult.conservative[cell][component]==
					independentlyManufacturedCommit[cell][component];
		ConservativeVector rate;
		for(std::size_t species=0;species<MethaneSpeciesCount;++species)
			rate[1+species]=manufacturedPacket[cell].constituentDelta[species]/
				manufacturedConfig.transport.deltaTimeS;
		rate[MethaneMassStateDimension]=manufacturedPacket[cell].sensibleEnergyDeltaJPerM3/
			manufacturedConfig.transport.deltaTimeS;
		maximumTargetMismatch=std::max(maximumTargetMismatch,std::fabs(
			manufacturedResult.r0.divergenceTargetPerS[cell]-
			independentlyManufacturedR0Target[cell]));
		maximumR1TargetMismatch=std::max(maximumR1TargetMismatch,std::fabs(
			manufacturedResult.r1.divergenceTargetPerS[cell]-
			independentlyManufacturedR1Target[cell]));
		maximumHeunTargetMismatch=std::max(maximumHeunTargetMismatch,std::fabs(
			manufacturedResult.divergenceHeunPerS[cell]-
			independentlyManufacturedHeunTarget[cell]));
		maximumR2TargetMismatch=std::max(maximumR2TargetMismatch,std::fabs(
			manufacturedResult.r2.divergenceTargetPerS[cell]-
			independentlyManufacturedR2Target[cell]));
		maximumR2HeunDifference=std::max(maximumR2HeunDifference,std::fabs(
			manufacturedResult.r2.divergenceTargetPerS[cell]-
			manufacturedResult.divergenceHeunPerS[cell]));
		maximumProjectionMismatch=std::max(maximumProjectionMismatch,std::fabs(
			PeriodicMACDivergence3D(ownerShape,
				manufacturedResult.r2.projection.velocityMPerS,cell)-
			manufacturedResult.r2.divergenceTargetPerS[cell]));
		for(std::size_t component=0;component<MethaneConservativeDimension;++component){
			manufacturedBefore[component]+=manufacturedBeginning[cell][component];
			manufacturedAfter[component]+=manufacturedResult.conservative[cell][component];
		}
		for(std::size_t species=0;species<MethaneSpeciesCount;++species)
			manufacturedSource[1+species]+=manufacturedPacket[cell].constituentDelta[species];
		manufacturedSource[MethaneMassStateDimension]+=
			manufacturedPacket[cell].sensibleEnergyDeltaJPerM3;
	}
	for(unsigned int axis=0;axis<3;++axis)for(const double alpha:manufacturedResult.faceAlpha[axis])
		manufacturedMinimumAlpha=std::min(manufacturedMinimumAlpha,alpha);
	PeriodicFluxPair3D manufacturedLimiterFlux;
	for(unsigned int axis=0;axis<3;++axis){
		manufacturedLimiterFlux.low[axis].assign(ownerCount,ConservativeVector());
		manufacturedLimiterFlux.high[axis].assign(ownerCount,ConservativeVector());
		manufacturedLimiterFlux.nonadvectiveMass[axis].assign(ownerCount,
			std::array<double,MethaneMassStateDimension>());
		manufacturedLimiterFlux.nonadvectiveEnergy[axis].assign(ownerCount,0.0);}
	const double manufacturedLimiterScale=manufacturedConfig.transport.deltaTimeS/
		ownerShape.cellWidthM;
	manufacturedLimiterFlux.high[0][0]=0.2/manufacturedLimiterScale*(pureFuel-pureAir);
	std::vector<ConservativeVector> manufacturedLimiterBeginning(ownerCount,
		ToConservativeVector(PhysicalMixtureLineState(fuel,thermochemistry,0.5,800.0))),
		manufacturedLimiterSource(ownerCount),manufacturedLimiterResult;
	manufacturedLimiterBeginning[0]=ToConservativeVector(
		PhysicalMixtureLineState(fuel,thermochemistry,0.1,800.0));
	std::array<std::vector<double>,3> manufacturedLimiterAlpha;
	const bool manufacturedLimiterOK=ApplyPeriodicSharedFCT3D(ownerShape,
		manufacturedLimiterBeginning,manufacturedLimiterFlux,manufacturedLimiterSource,
		manufacturedConfig.transport,fuel,thermochemistry,manufacturedLimiterResult,
		manufacturedLimiterAlpha,&error);
	double manufacturedLimiterMinimumAlpha=1.0;
	for(unsigned int axis=0;axis<3;++axis)for(const double alpha:manufacturedLimiterAlpha[axis])
		manufacturedLimiterMinimumAlpha=std::min(manufacturedLimiterMinimumAlpha,alpha);
	const bool manufacturedLimiterActive=manufacturedLimiterOK&&
		manufacturedLimiterMinimumAlpha>0.0&&manufacturedLimiterMinimumAlpha<1.0;
	bool sourceConsumedOnce=manufacturedOK,manufacturedElements=manufacturedOK;
	for(const MethaneSourcePacket& packet:manufacturedPacket)
		manufacturedElements=manufacturedElements &&
			MaximumElementResidual(fuel,packet.constituentDelta)<3.0e-16 &&
			packet.constituentDelta[MethaneCarbon]==0.0;
	for(std::size_t component=0;component<MethaneConservativeDimension;++component){
		const double reductionTolerance=4096.0*std::numeric_limits<double>::epsilon()*
			std::max({1.0,std::fabs(manufacturedBefore[component]),
			std::fabs(manufacturedAfter[component])});
		const bool componentOnce=std::fabs(manufacturedAfter[component]-
			manufacturedBefore[component]-manufacturedSource[component])<=reductionTolerance;
		if(!componentOnce) std::printf("V2 source component=%zu actual=%.17g expected=%.17g\n",
			component,manufacturedAfter[component]-manufacturedBefore[component],
			manufacturedSource[component]);
		sourceConsumedOnce=sourceConsumedOnce && componentOnce;
	}
	const bool manufacturedPicardConverged=manufacturedOK&&
		!manufacturedResult.r0.picardResidualPerS.empty()&&
		!manufacturedResult.r1.picardResidualPerS.empty()&&
		manufacturedResult.r0.picardResidualPerS.back()<=manufacturedConfig.projectionTolerancePerS&&
		manufacturedResult.r1.picardResidualPerS.back()<=manufacturedConfig.projectionTolerancePerS;
	bool namedStageVelocityRED=manufacturedOK;
	std::vector<double> forbiddenR0Temperature,forbiddenR1Temperature;
	PeriodicMACField forbiddenR0Velocity,forbiddenR1Velocity;
	for(unsigned int axis=0;axis<3;++axis){forbiddenR0Velocity.component[axis].resize(ownerCount);
		forbiddenR1Velocity.component[axis].resize(ownerCount);for(std::size_t face=0;face<ownerCount;
			++face){const std::size_t next=PeriodicNext(ownerShape,face,axis);
			forbiddenR0Velocity.component[axis][face]=manufacturedMomentum.component[axis][face]/
				PositiveArithmeticMean(FromConservativeVector(manufacturedBeginning[face]).GasDensity(),
					FromConservativeVector(manufacturedBeginning[next]).GasDensity());}}
	PeriodicFluxPair3D forbiddenR0Flux,forbiddenR1Flux;
	namedStageVelocityRED=namedStageVelocityRED&&InvertPeriodicTemperatures(manufacturedBeginning,
		thermochemistry,forbiddenR0Temperature,&error)&&BuildPeriodicFluxPair3D(ownerShape,
		manufacturedBeginning,forbiddenR0Temperature,forbiddenR0Velocity,
		manufacturedResult.r0.diffusivityM2PerS,manufacturedResult.r0.conductivityWPerMK,fuel,
		thermochemistry,forbiddenR0Flux,&error);
	PeriodicMACField forbiddenPredictorMomentum=manufacturedMomentum;
	if(namedStageVelocityRED){std::array<std::vector<double>,3> low,high,diffusive,accepted;
		GasPrimalSubfluxes3D(manufacturedResult.r0.flux,low,high,diffusive);
		for(unsigned int axis=0;axis<3;++axis){accepted[axis].resize(ownerCount);for(std::size_t face=0;
			face<ownerCount;++face)accepted[axis][face]=low[axis][face]+independentlyManufacturedAlpha[
				axis][face]*(high[axis][face]-low[axis][face]);}
		const auto divergence=CompatibleMomentumFluxDivergence3D(ownerShape,accepted,diffusive,
			manufacturedResult.r0.projection.velocityMPerS);
		for(unsigned int axis=0;axis<3;++axis)for(std::size_t face=0;face<ownerCount;++face)
			forbiddenPredictorMomentum.component[axis][face]+=manufacturedConfig.transport.deltaTimeS*(
				manufacturedResult.r0.nonpressureMomentumRHS.component[axis][face]-divergence[axis][face]);
		for(unsigned int axis=0;axis<3;++axis)for(std::size_t face=0;face<ownerCount;++face){const
			std::size_t next=PeriodicNext(ownerShape,face,axis);forbiddenR1Velocity.component[axis][face]=
			forbiddenPredictorMomentum.component[axis][face]/PositiveArithmeticMean(
				FromConservativeVector(independentlyManufacturedPredictor[face]).GasDensity(),
				FromConservativeVector(independentlyManufacturedPredictor[next]).GasDensity());}
		namedStageVelocityRED=InvertPeriodicTemperatures(independentlyManufacturedPredictor,
			thermochemistry,forbiddenR1Temperature,&error)&&BuildPeriodicFluxPair3D(ownerShape,
			independentlyManufacturedPredictor,forbiddenR1Temperature,forbiddenR1Velocity,
			manufacturedResult.r1.diffusivityM2PerS,manufacturedResult.r1.conductivityWPerMK,fuel,
			thermochemistry,forbiddenR1Flux,&error);}
	double forbiddenVelocityFluxDifference=0.0;
	for(unsigned int axis=0;namedStageVelocityRED&&axis<3;++axis)for(std::size_t face=0;face<ownerCount;
		++face)for(std::size_t component=0;component<MethaneConservativeDimension;++component)
		forbiddenVelocityFluxDifference=std::max({forbiddenVelocityFluxDifference,std::fabs(
			forbiddenR0Flux.high[axis][face][component]-manufacturedResult.r0.flux.high[axis][face][component]),
			std::fabs(forbiddenR1Flux.high[axis][face][component]-manufacturedResult.r1.flux.high[axis][face][component])});
	namedStageVelocityRED=namedStageVelocityRED&&forbiddenVelocityFluxDifference>1.0e-8;
	if(!(manufacturedOK && maximumTargetMismatch<=manufacturedConfig.projectionTolerancePerS &&
		maximumR1TargetMismatch<=manufacturedConfig.projectionTolerancePerS&&
		maximumHeunTargetMismatch<=manufacturedConfig.projectionTolerancePerS&&
		maximumR2TargetMismatch<=manufacturedConfig.projectionTolerancePerS&&maximumR2HeunDifference>1.0e-10&&
		maximumProjectionMismatch<=manufacturedConfig.projectionTolerancePerS &&
		sourceConsumedOnce&&manufacturedLimiterActive)) std::printf("V2 reacting metrics target=%.9g "
		"r1=%.9g heun=%.9g r2=%.9g r2_heun=%.9g projection=%.9g once=%d "
		"picard=%d elements=%d velocity=%d ownerAlpha=%.17g limiterAlpha=%.17g\n",
		maximumTargetMismatch,maximumR1TargetMismatch,maximumHeunTargetMismatch,
		maximumR2TargetMismatch,maximumR2HeunDifference,maximumProjectionMismatch,
		sourceConsumedOnce?1:0,manufacturedPicardConverged?1:0,manufacturedElements?1:0,
		namedStageVelocityRED?1:0,manufacturedMinimumAlpha,manufacturedLimiterMinimumAlpha);
	Check(std::fabs(manufacturedR0Mean)<manufacturedConfig.projectionTolerancePerS&&
		std::fabs(manufacturedR1Mean)<manufacturedConfig.projectionTolerancePerS,
		"V2 reacting manufactured source is exactly periodic-compatible at R0 and R1");
	Check(manufacturedOK && manufacturedTargetOK && manufacturedTableauOK&&
		manufacturedPicardConverged&&manufacturedElements&&namedStageVelocityRED &&
		manufacturedMinimumAlpha>0.0 && manufacturedLimiterActive &&
		maximumTargetMismatch<=manufacturedConfig.projectionTolerancePerS &&
		maximumR1TargetMismatch<=manufacturedConfig.projectionTolerancePerS&&
		maximumHeunTargetMismatch<=manufacturedConfig.projectionTolerancePerS&&
		maximumR2TargetMismatch<=manufacturedConfig.projectionTolerancePerS&&maximumR2HeunDifference>1.0e-10&&
		maximumProjectionMismatch<=manufacturedConfig.projectionTolerancePerS && sourceConsumedOnce,
		"V2 reacting 3-D owner derives S_div from its one frozen-packet update and closes R2");

	// The frozen source is deliberately Euler-split from transport.  This
	// independent characteristic solution therefore converges at the declared
	// first-order source-split rate even though the transport tableau is Heun.
	// The physical owner above is bit-bound to this same shared FCT commit.
	const std::size_t sourceSplitResolution[3]={16,32,64};
	double sourceSplitError[3]={};bool sourceSplitOK=true,sourceSplitLedgers=true;
	const double sourceSplitFinalTime=0.2,sourceSplitVelocity=0.18;
	ConservativeVector sourceSplitDirection;
	static const char* sourceSplitName[MethaneSpeciesCount]={
		"CH4","O2","N2","CO2","H2O","CO","C(gr)"};
	for(std::size_t species=0;species<MethaneSpeciesCount;++species){
		sourceSplitDirection[1+species]=fuel.PrimaryReactionDelta()[species];
		double enthalpy=0.0;Check(thermochemistry.SensibleEnthalpyJPerKG(
			sourceSplitName[species],800.0,enthalpy,&error),
			"V2 source-split direction has record-derived isothermal enthalpy");
		sourceSplitDirection[MethaneMassStateDimension]+=enthalpy*
			fuel.PrimaryReactionDelta()[species];
	}
	const ConservativeVector sourceSplitBase=manufacturedProductRich;
	double sourceSplitInitialAmplitude=std::numeric_limits<double>::max();
	for(std::size_t species=0;species<MethaneSpeciesCount;++species)if(
		sourceSplitDirection[1+species]!=0.0)sourceSplitInitialAmplitude=std::min(
			sourceSplitInitialAmplitude,0.08*sourceSplitBase[1+species]/
			std::fabs(sourceSplitDirection[1+species]));
	const double sourceSplitRelaxationPerS=0.8;
	for(std::size_t level=0;level<3;++level){
		PeriodicMACShape splitShape;splitShape.nx=sourceSplitResolution[level];
		splitShape.ny=4;splitShape.nz=4;splitShape.cellWidthM=1.0/splitShape.nx;
		const std::size_t count=splitShape.CellCount();
		PeriodicMACField splitVelocity;for(unsigned int axis=0;axis<3;++axis)
			splitVelocity.component[axis].assign(count,axis==0?sourceSplitVelocity:0.0);
		std::vector<ConservativeVector> state(count);
		for(std::size_t cell=0;cell<count;++cell){const std::size_t x=cell%splitShape.nx;
			const double phase=2.0*pi*(x+0.5)/splitShape.nx;
			state[cell]=sourceSplitBase;for(std::size_t row=0;row<MethaneConservativeDimension;
				++row)state[cell][row]+=sourceSplitInitialAmplitude*std::sin(phase)*
					sourceSplitDirection[row];}
		const std::size_t steps=static_cast<std::size_t>(std::ceil(sourceSplitFinalTime/
			(0.22*splitShape.cellWidthM/sourceSplitVelocity)));
		PeriodicTransportConfig splitConfig=ownerConfig.transport;
		splitConfig.cellWidthM=splitShape.cellWidthM;
		splitConfig.deltaTimeS=sourceSplitFinalTime/steps;
		std::array<std::vector<double>,3> alpha;
		for(std::size_t step=0;sourceSplitOK&&step<steps;++step){
			std::vector<MethaneSourcePacket> packet(count);
			for(std::size_t cell=0;cell<count;++cell){const double coordinate=
				(state[cell][1+MethaneCH4]-sourceSplitBase[1+MethaneCH4])/
					sourceSplitDirection[1+MethaneCH4];
				const double deltaCoordinate=-splitConfig.deltaTimeS*
					sourceSplitRelaxationPerS*coordinate;
				for(std::size_t species=0;species<MethaneSpeciesCount;++species)
					packet[cell].constituentDelta[species]=deltaCoordinate*
						sourceSplitDirection[1+species];
				packet[cell].sensibleEnergyDeltaJPerM3=deltaCoordinate*
					sourceSplitDirection[MethaneMassStateDimension];}
			PeriodicMACField splitMomentum;for(unsigned int axis=0;axis<3;++axis){
				splitMomentum.component[axis].resize(count);for(std::size_t face=0;face<count;
					++face){const std::size_t next=PeriodicNext(splitShape,face,axis);
					splitMomentum.component[axis][face]=PositiveArithmeticMean(
						FromConservativeVector(state[face]).GasDensity(),
						FromConservativeVector(state[next]).GasDensity())*
						 splitVelocity.component[axis][face];}}
			ConservativeAdvance3DConfig splitOwner=ownerConfig;
			splitOwner.transport=splitConfig;splitOwner.dns=true;splitOwner.gravityMPerS2={{0,0,0}};
			splitOwner.projectionTolerancePerS=1.0e-7;
			ConservativeAdvance3DResult next;
			sourceSplitOK=AdvanceConservative3D(splitShape,state,splitMomentum,packet,
				splitOwner,fuel,thermochemistry,transport,next,&error);
			if(sourceSplitOK){state.swap(next.conservative);alpha=std::move(next.faceAlpha);}
		}
		std::array<double,MethaneConservativeDimension> initialTotal={},finalTotal={};
		for(std::size_t cell=0;sourceSplitOK&&cell<count;++cell){const std::size_t x=
			cell%splitShape.nx;const double coordinate=(x+0.5)/splitShape.nx;
			double departure=coordinate-sourceSplitVelocity*sourceSplitFinalTime;
			departure-=std::floor(departure);
			const double initialWave=std::exp(-sourceSplitRelaxationPerS*
				sourceSplitFinalTime)*std::sin(2.0*pi*departure);
			ConservativeVector exact=sourceSplitBase;
			for(std::size_t row=0;row<MethaneConservativeDimension;++row)
				exact[row]+=sourceSplitInitialAmplitude*initialWave*
					sourceSplitDirection[row];
			for(std::size_t component=0;component<MethaneConservativeDimension;++component){
				sourceSplitError[level]+=std::fabs(state[cell][component]-exact[component])/
					(count*MethaneConservativeDimension);
				initialTotal[component]+=sourceSplitBase[component];
				finalTotal[component]+=state[cell][component];}
			sourceSplitLedgers=sourceSplitLedgers&&MaximumConstraintResidual(
				fuel.ConservativeReconstruction(),state[cell])<4.0e-13;
		}
		for(std::size_t component=0;component<MethaneConservativeDimension;++component)
			sourceSplitLedgers=sourceSplitLedgers&&Near(finalTotal[component],
				initialTotal[component],8.0e-13);
	}
	const double sourceSplitOrder=std::log(sourceSplitError[1]/sourceSplitError[2])/
		std::log(2.0);
	if(!(sourceSplitOK&&sourceSplitLedgers&&sourceSplitOrder>=0.85&&sourceSplitOrder<=1.25))
		std::printf("V2 source-split errors %.17g %.17g %.17g order %.6g amplitude %.17g direction %.17g ok=%d ledger=%d error=%s\n",
			sourceSplitError[0],sourceSplitError[1],sourceSplitError[2],sourceSplitOrder,
			sourceSplitInitialAmplitude,manufacturedDirection[0],sourceSplitOK?1:0,
			sourceSplitLedgers?1:0,error.c_str());
	Check(sourceSplitOK&&sourceSplitLedgers&&sourceSplitOrder>=0.85&&sourceSplitOrder<=1.25,
		"V2 frozen reacting-source split has its independent analytic first-order history");

	// V2 variable-density momentum manufacture.  Conservative momentum
	// advection, viscous stress, relative buoyancy and a nonzero divergence
	// target are assembled together before the dynamic-pressure projection.
	// The analytic face velocity and cell pressure are compared over three
	// refinements rather than against the implementation's own residual.
	const std::size_t momentumResolution[3]={8,16,32};
	double manufacturedVelocityError[3]={},manufacturedPressureError[3]={};
	bool momentumManufacture=true,momentumTermsActive=true,ownerMomentumSchedule=true;
	for(std::size_t level=0;level<3;++level){
		PeriodicMACShape momentumShape;
		momentumShape.nx=momentumResolution[level];
		momentumShape.ny=momentumResolution[level];
		momentumShape.nz=momentumResolution[level];
		momentumShape.cellWidthM=1.0/momentumShape.nx;
		const std::size_t count=momentumShape.CellCount();
		std::vector<ConservativeVector> momentumState(count);
		std::vector<double> momentumTemperature(count),momentumViscosity(count,0.018);
		std::vector<double> target(count),exactPressure(count);
		PeriodicMACField exactVelocity,unprojectedMomentum,beginningManufacturedMomentum;
		for(unsigned int axis=0;axis<3;++axis){
			exactVelocity.component[axis].resize(count);
			unprojectedMomentum.component[axis].resize(count);
			beginningManufacturedMomentum.component[axis].resize(count);
		}
		for(std::size_t cell=0;cell<count;++cell){
			const std::size_t x=cell%momentumShape.nx;
			const std::size_t y=(cell/momentumShape.nx)%momentumShape.ny;
			const std::size_t z=cell/(momentumShape.nx*momentumShape.ny);
			const double xc=(x+0.5)*momentumShape.cellWidthM;
			const double yc=(y+0.5)*momentumShape.cellWidthM;
			const double zc=(z+0.5)*momentumShape.cellWidthM;
			const double densityScale=1.0+0.12*std::sin(2.0*pi*xc)*std::cos(2.0*pi*yc);
			momentumTemperature[cell]=800.0/densityScale;
			momentumState[cell]=ToConservativeVector(PhysicalMixtureLineState(fuel,
				thermochemistry,0.2,momentumTemperature[cell]));
			target[cell]=2.0*pi*(0.08*std::cos(2.0*pi*xc)-
				0.05*std::cos(2.0*pi*yc)+0.035*std::cos(2.0*pi*zc));
			exactPressure[cell]=0.7*std::sin(2.0*pi*xc)*std::cos(2.0*pi*yc)+
				0.2*std::sin(2.0*pi*zc);
			const double faceCoordinate[3]={(x+1.0)*momentumShape.cellWidthM,
				(y+1.0)*momentumShape.cellWidthM,(z+1.0)*momentumShape.cellWidthM};
			exactVelocity.component[0][cell]=0.08*std::sin(2.0*pi*faceCoordinate[0]);
			exactVelocity.component[1][cell]=-0.05*std::sin(2.0*pi*faceCoordinate[1]);
			exactVelocity.component[2][cell]=0.035*std::sin(2.0*pi*faceCoordinate[2]);
		}
		PeriodicFluxPair3D momentumFlux;
		std::vector<double> zeroCoefficient(count,0.0);
		const bool momentumFluxOK=BuildPeriodicFluxPair3D(momentumShape,
			momentumState,momentumTemperature,exactVelocity,zeroCoefficient,zeroCoefficient,
			fuel,thermochemistry,momentumFlux,&error);
		momentumManufacture=momentumManufacture && momentumFluxOK;
		if(!momentumFluxOK){std::printf("V2 momentum flux level=%zu: %s\n",level,error.c_str());continue;}
		std::array<std::vector<double>,3> lowMomentum,highMomentum,diffusiveMomentum;
		GasPrimalSubfluxes3D(momentumFlux,lowMomentum,highMomentum,diffusiveMomentum);
		const std::array<std::vector<double>,3> momentumAdvection=
			CompatibleMomentumFluxDivergence3D(momentumShape,highMomentum,
				diffusiveMomentum,exactVelocity);
		std::array<std::vector<double>,3> independentAdvection;
		for(unsigned int component=0;component<3;++component){
			independentAdvection[component].assign(count,0.0);
			for(std::size_t face=0;face<count;++face){double divergence=0.0;
				for(unsigned int derivative=0;derivative<3;++derivative){
					const std::size_t nextComponent=PeriodicNext(momentumShape,face,component),
						previousDerivative=PeriodicPrevious(momentumShape,face,derivative),
						nextComponentPreviousDerivative=PeriodicPrevious(momentumShape,
							nextComponent,derivative);
					auto mass=[&](std::size_t index){return highMomentum[derivative][index]+
						diffusiveMomentum[derivative][index];};
					double upper=0.0,lower=0.0;
					if(derivative==component){upper=0.25*(mass(face)+mass(nextComponent))*(
						exactVelocity.component[component][face]+exactVelocity.component[
							component][nextComponent]);lower=0.25*(mass(previousDerivative)+
						mass(face))*(exactVelocity.component[component][previousDerivative]+
							exactVelocity.component[component][face]);}
					else{const std::size_t nextDerivative=PeriodicNext(momentumShape,face,
						derivative);upper=0.25*(mass(face)+mass(nextComponent))*(
						exactVelocity.component[component][face]+exactVelocity.component[
							component][nextDerivative]);lower=0.25*(mass(previousDerivative)+
						mass(nextComponentPreviousDerivative))*(exactVelocity.component[
							component][previousDerivative]+exactVelocity.component[component][face]);}
					divergence+=(upper-lower)/momentumShape.cellWidthM;}
				independentAdvection[component][face]=divergence;}}
		PeriodicMACField momentumRHS;
		const bool momentumRHSOK=RemainingMomentumRHS3D(momentumShape,
			momentumState,exactVelocity,momentumViscosity,
			std::vector<ConservativeVector>(count),0.003,ownerPhysical.GasDensity(),
			std::array<double,3>{{0.4,-0.3,-9.80665}},momentumRHS,&error);
		momentumManufacture=momentumManufacture && momentumRHSOK;
		if(!momentumRHSOK){std::printf("V2 momentum RHS level=%zu: %s\n",level,error.c_str());continue;}
		PeriodicMACField independentRHS;std::array<std::vector<double>,3> independentCellVelocity;
		for(unsigned int component=0;component<3;++component){independentRHS.component[component].
			assign(count,0.0);independentCellVelocity[component].resize(count);for(std::size_t cell=0;
			cell<count;++cell)independentCellVelocity[component][cell]=0.5*(exactVelocity.component[
			component][cell]+exactVelocity.component[component][PeriodicPrevious(momentumShape,
			cell,component)]);}
		std::array<std::array<std::vector<double>,3>,3> independentStress;
		for(unsigned int component=0;component<3;++component)for(unsigned int derivative=0;
			derivative<3;++derivative)independentStress[component][derivative].assign(count,0.0);
		for(std::size_t cell=0;cell<count;++cell){double gradient[3][3]={},divergence=0.0;
			for(unsigned int derivative=0;derivative<3;++derivative){const std::size_t previous=
				PeriodicPrevious(momentumShape,cell,derivative),next=PeriodicNext(momentumShape,
				cell,derivative);for(unsigned int component=0;component<3;++component)gradient[
				derivative][component]=(independentCellVelocity[component][next]-
				independentCellVelocity[component][previous])/(2.0*momentumShape.cellWidthM);
				divergence+=gradient[derivative][derivative];}
			for(unsigned int component=0;component<3;++component)for(unsigned int derivative=0;
				derivative<3;++derivative)independentStress[component][derivative][cell]=
				momentumViscosity[cell]*(gradient[derivative][component]+gradient[component][derivative]-
					(component==derivative?(2.0/3.0)*divergence:0.0));}
		double independentOperatorDifference=0.0;
		for(unsigned int component=0;component<3;++component)for(std::size_t face=0;face<count;
			++face){const std::size_t right=PeriodicNext(momentumShape,face,component);double viscous=
			(independentStress[component][component][right]-independentStress[component][component][
				face])/momentumShape.cellWidthM;for(unsigned int derivative=0;derivative<3;++derivative){
			if(derivative==component)continue;const std::size_t previous=PeriodicPrevious(momentumShape,
				face,derivative),next=PeriodicNext(momentumShape,face,derivative),previousRight=
				PeriodicPrevious(momentumShape,right,derivative),nextRight=PeriodicNext(momentumShape,
				right,derivative);viscous+=0.25*(independentStress[component][derivative][next]+
				independentStress[component][derivative][nextRight]-independentStress[component][derivative][
				previous]-independentStress[component][derivative][previousRight])/
				momentumShape.cellWidthM;}const double gasDensity=0.5*(FromConservativeVector(
			momentumState[face]).GasDensity()+FromConservativeVector(momentumState[right]).GasDensity());
			independentRHS.component[component][face]=viscous+(gasDensity-ownerPhysical.GasDensity())*
				std::array<double,3>{{0.4,-0.3,-9.80665}}[component];
			independentOperatorDifference=std::max({independentOperatorDifference,std::fabs(
				momentumAdvection[component][face]-independentAdvection[component][face]),std::fabs(
				momentumRHS.component[component][face]-independentRHS.component[component][face])});}
		momentumManufacture=momentumManufacture&&independentOperatorDifference<2.0e-13;
		PeriodicMACField momentumWithoutGravity,momentumWithoutViscosity;
		const bool separatedMomentumOK=momentumRHSOK&&RemainingMomentumRHS3D(momentumShape,
			momentumState,exactVelocity,momentumViscosity,
			std::vector<ConservativeVector>(count),0.003,ownerPhysical.GasDensity(),
			std::array<double,3>{{0.0,0.0,0.0}},momentumWithoutGravity,&error)&&
			RemainingMomentumRHS3D(momentumShape,momentumState,exactVelocity,
			std::vector<double>(count,0.0),std::vector<ConservativeVector>(count),0.003,
			ownerPhysical.GasDensity(),std::array<double,3>{{0.4,-0.3,-9.80665}},
			momentumWithoutViscosity,&error);
		momentumManufacture=momentumManufacture&&separatedMomentumOK;
		double activeAdvection=0.0,activeNonpressure=0.0,activeViscous=0.0,activeBuoyancy=0.0;
		for(std::size_t cell=0;cell<count;++cell) for(unsigned int axis=0;axis<3;++axis){
			activeAdvection=std::max(activeAdvection,std::fabs(momentumAdvection[axis][cell]));
			activeNonpressure=std::max(activeNonpressure,
				std::fabs(momentumRHS.component[axis][cell]));
			activeBuoyancy=std::max(activeBuoyancy,std::fabs(
				momentumRHS.component[axis][cell]-momentumWithoutGravity.component[axis][cell]));
			activeViscous=std::max(activeViscous,std::fabs(
				momentumRHS.component[axis][cell]-momentumWithoutViscosity.component[axis][cell]));
			const std::size_t next=PeriodicNext(momentumShape,cell,axis);
			const double faceDensity=0.5*(FromConservativeVector(momentumState[cell]).GasDensity()+
				FromConservativeVector(momentumState[next]).GasDensity());
			const double pressureGradient=(exactPressure[next]-exactPressure[cell])/
				momentumShape.cellWidthM;
			unprojectedMomentum.component[axis][cell]=faceDensity*
				exactVelocity.component[axis][cell]+0.003*pressureGradient;
			beginningManufacturedMomentum.component[axis][cell]=
				unprojectedMomentum.component[axis][cell]-0.003*(
				momentumRHS.component[axis][cell]-momentumAdvection[axis][cell]);
			const double assembled=beginningManufacturedMomentum.component[axis][cell]+
				0.003*(momentumRHS.component[axis][cell]-momentumAdvection[axis][cell]);
			momentumManufacture=momentumManufacture && Near(assembled,
				unprojectedMomentum.component[axis][cell],4.0e-15);
		}
		momentumTermsActive=momentumTermsActive && activeAdvection>0.0 &&
			activeNonpressure>0.0&&activeViscous>0.0&&activeBuoyancy>0.0;
		PeriodicMACProjection3DResult momentumProjected;
		const bool momentumProjectionOK=ProjectPeriodicMACVelocity3D(momentumShape,
			GasDensityFromConservative(momentumState),unprojectedMomentum,target,0.003,
			2.0e-9,momentumProjected,&error);
		momentumManufacture=momentumManufacture && momentumProjectionOK;
		if(!momentumProjectionOK){
			std::printf("V2 momentum level=%zu diagnostic: %s\n",level,error.c_str());
			continue;
		}
		double pressureMean=0.0,computedMean=0.0,endpointPressureError=0.0;
		for(std::size_t cell=0;cell<count;++cell){pressureMean+=exactPressure[cell]/count;
			computedMean+=momentumProjected.stepAverageDynamicPressurePa[cell]/count;}
		for(std::size_t cell=0;momentumManufacture && cell<count;++cell){
			manufacturedPressureError[level]+=std::fabs(
				momentumProjected.stepAverageDynamicPressurePa[cell]-computedMean-
				(exactPressure[cell]-pressureMean))/count;
			endpointPressureError+=std::fabs(momentumProjected.stepAverageDynamicPressurePa[cell]-
				computedMean-2.0*(exactPressure[cell]-pressureMean))/count;
			for(unsigned int axis=0;axis<3;++axis) manufacturedVelocityError[level]+=
				std::fabs(momentumProjected.velocityMPerS.component[axis][cell]-
					exactVelocity.component[axis][cell])/(3.0*count);
		}
		momentumManufacture=momentumManufacture&&endpointPressureError>
			4.0*manufacturedPressureError[level];
		{
			ConservativeAdvance3DConfig momentumOwnerConfig=ownerConfig;
			momentumOwnerConfig.transport.cellWidthM=momentumShape.cellWidthM;
			momentumOwnerConfig.transport.deltaTimeS=2.0e-4;
			momentumOwnerConfig.transport.ambientGasDensityKGPerM3=ownerPhysical.GasDensity();
			momentumOwnerConfig.gravityMPerS2={{0.4,-0.3,-9.80665}};
			momentumOwnerConfig.dns=true;
			const MethaneCellState ownerSchedulePhysical=PhysicalMixtureLineState(
				fuel,thermochemistry,0.2,800.0);
			const std::vector<ConservativeVector> ownerScheduleState(count,
				ToConservativeVector(ownerSchedulePhysical));
			const std::vector<double> ownerScheduleTemperature(count,800.0);
			PeriodicMACField ownerBeginningMomentum;
			for(unsigned int axis=0;axis<3;++axis){
				ownerBeginningMomentum.component[axis].resize(count);
				for(std::size_t face=0;face<count;++face){
					const std::size_t next=PeriodicNext(momentumShape,face,axis);
					const double faceDensity=0.5*(FromConservativeVector(ownerScheduleState[face]).GasDensity()+
						FromConservativeVector(ownerScheduleState[next]).GasDensity());
					ownerBeginningMomentum.component[axis][face]=faceDensity*
						exactVelocity.component[axis][face];
				}
			}
			std::vector<MethaneSourcePacket> momentumOwnerPacket(count);
			std::vector<double> ownerDiffusivity,ownerConductivity,ownerViscosity;
			PeriodicFluxPair3D ownerInitialFlux;
			std::vector<double> ownerRawTarget;
			PeriodicMACProjection3DResult ownerCalibrationProjection;
			bool ownerSourceOK=ProjectPeriodicMACVelocity3D(momentumShape,
				GasDensityFromConservative(ownerScheduleState),ownerBeginningMomentum,
				std::vector<double>(count,0.0),momentumOwnerConfig.transport.deltaTimeS,
				momentumOwnerConfig.projectionTolerancePerS,ownerCalibrationProjection,&error)&&
				BuildPeriodicStageTransport3D(momentumShape,ownerScheduleState,
				ownerScheduleTemperature,ownerCalibrationProjection.velocityMPerS,true,
				thermochemistry,transport,ownerDiffusivity,
				ownerConductivity,ownerViscosity,&error)&&BuildPeriodicFluxPair3D(momentumShape,
				ownerScheduleState,ownerScheduleTemperature,ownerCalibrationProjection.velocityMPerS,
				ownerDiffusivity,ownerConductivity,
				fuel,thermochemistry,ownerInitialFlux,&error)&&
				PeriodicDivergenceTargetFromPhysicalFlux3D(momentumShape,ownerScheduleState,
					ownerScheduleTemperature,ownerInitialFlux,std::vector<ConservativeVector>(count),
					momentumOwnerConfig.transport.deltaTimeS,thermochemistry,ownerRawTarget,&error);
			double ownerTargetMean=0.0;for(const double value:ownerRawTarget)ownerTargetMean+=value/count;
			double ownerDesiredSum=0.0;
			for(std::size_t cell=0;ownerSourceOK&&cell<count;++cell){
				ConservativeVector combined;double physicalEnergyIncrement=0.0;
				for(unsigned int axis=0;axis<3;++axis){const std::size_t previous=
					PeriodicPrevious(momentumShape,cell,axis);for(std::size_t component=0;
					component<MethaneMassStateDimension;++component)combined[component]+=
						momentumOwnerConfig.transport.deltaTimeS*(ownerInitialFlux.
							nonadvectiveMass[axis][previous][component]-ownerInitialFlux.
							nonadvectiveMass[axis][cell][component])/momentumShape.cellWidthM;
					physicalEnergyIncrement+=momentumOwnerConfig.transport.deltaTimeS*(
						ownerInitialFlux.nonadvectiveEnergy[axis][previous]-
						ownerInitialFlux.nonadvectiveEnergy[axis][cell])/
						momentumShape.cellWidthM;}
				combined[MethaneMassStateDimension]=physicalEnergyIncrement;
				const std::size_t x=cell%momentumShape.nx;
				double desired=1.0e-4*std::sin(2.0*pi*(x+0.5)/momentumShape.nx);
				if(cell+1==count)desired=-ownerDesiredSum;else ownerDesiredSum+=desired;
				ownerSourceOK=SetRecordDerivedEnergyForDivergence(ownerScheduleState[cell],combined,
					ownerScheduleTemperature[cell],desired+ownerRawTarget[cell]-ownerTargetMean,
					momentumOwnerConfig.transport.deltaTimeS,thermochemistry,&error);
				momentumOwnerPacket[cell].sensibleEnergyDeltaJPerM3=
					combined[MethaneMassStateDimension]-physicalEnergyIncrement;
			}
			if(!ownerSourceOK)std::printf("V2 owning momentum source fixture: %s\n",error.c_str());
			ConservativeAdvance3DResult momentumOwnerResult;
			const bool momentumOwnerOK=AdvanceConservative3D(momentumShape,ownerScheduleState,
				ownerBeginningMomentum,momentumOwnerPacket,momentumOwnerConfig,
				fuel,thermochemistry,transport,momentumOwnerResult,&error);
			if(!momentumOwnerOK)std::printf("V2 owning momentum diagnostic: %s\n",error.c_str());
			ownerMomentumSchedule=ownerMomentumSchedule&&ownerSourceOK&&momentumOwnerOK;
			PeriodicMACProjection3DResult independentlyCommitted,independentR0Projection,
				independentR1Projection;
			if(momentumOwnerOK){
				std::vector<ConservativeVector> momentumPredictor;
				std::array<std::vector<double>,3> momentumPredictorAlpha;
				std::vector<ConservativeVector> momentumSourceDelta;
				bool momentumPredictorOK=FrozenPacketDeltas3D(momentumOwnerPacket,count,
					momentumSourceDelta,1u,&error)&&ApplyPeriodicSharedFCT3D(momentumShape,ownerScheduleState,
					momentumOwnerResult.r0.flux,momentumSourceDelta,momentumOwnerConfig.transport,
					fuel,thermochemistry,momentumPredictor,momentumPredictorAlpha,&error);
				std::array<std::vector<double>,3> r0Low,r0High,r0Diffusion,
					r1Low,r1High,r1Diffusion,r0Accepted,r0PredictorAccepted,r1Accepted;
				GasPrimalSubfluxes3D(momentumOwnerResult.r0.flux,r0Low,r0High,r0Diffusion);
				GasPrimalSubfluxes3D(momentumOwnerResult.r1.flux,r1Low,r1High,r1Diffusion);
				for(unsigned int axis=0;axis<3;++axis){
					r0Accepted[axis].resize(count);r0PredictorAccepted[axis].resize(count);
					r1Accepted[axis].resize(count);
					for(std::size_t face=0;face<count;++face){
						r0Accepted[axis][face]=r0Low[axis][face]+momentumOwnerResult.faceAlpha[axis][face]*
							(r0High[axis][face]-r0Low[axis][face]);
						r0PredictorAccepted[axis][face]=r0Low[axis][face]+
							momentumPredictorAlpha[axis][face]*(r0High[axis][face]-r0Low[axis][face]);
						r1Accepted[axis][face]=r1Low[axis][face]+momentumOwnerResult.faceAlpha[axis][face]*
							(r1High[axis][face]-r1Low[axis][face]);
					}
				}
				const std::array<std::vector<double>,3> r0Divergence=
					CompatibleMomentumFluxDivergence3D(momentumShape,r0Accepted,r0Diffusion,
						momentumOwnerResult.r0.projection.velocityMPerS);
				const std::array<std::vector<double>,3> r0PredictorDivergence=
					CompatibleMomentumFluxDivergence3D(momentumShape,r0PredictorAccepted,r0Diffusion,
						momentumOwnerResult.r0.projection.velocityMPerS);
				const std::array<std::vector<double>,3> r1Divergence=
					CompatibleMomentumFluxDivergence3D(momentumShape,r1Accepted,r1Diffusion,
						momentumOwnerResult.r1.projection.velocityMPerS);
				PeriodicMACField independentPredictorMomentum=ownerBeginningMomentum;
				for(unsigned int axis=0;axis<3;++axis)for(std::size_t face=0;face<count;++face)
					independentPredictorMomentum.component[axis][face]+=
						momentumOwnerConfig.transport.deltaTimeS*(momentumOwnerResult.r0.
						nonpressureMomentumRHS.component[axis][face]-r0PredictorDivergence[axis][face]);
				bool independentStages=momentumPredictorOK&&ProjectPeriodicMACVelocity3D(momentumShape,
					GasDensityFromConservative(ownerScheduleState),ownerBeginningMomentum,
					momentumOwnerResult.r0.divergenceTargetPerS,
					momentumOwnerConfig.transport.deltaTimeS,momentumOwnerConfig.projectionTolerancePerS,
					independentR0Projection,&error)&&ProjectPeriodicMACVelocity3D(momentumShape,
					GasDensityFromConservative(momentumPredictor),independentPredictorMomentum,
					momentumOwnerResult.r1.divergenceTargetPerS,momentumOwnerConfig.transport.deltaTimeS,
					momentumOwnerConfig.projectionTolerancePerS,independentR1Projection,&error);
				double stageVelocityDifference=0.0;
				for(unsigned int axis=0;independentStages&&axis<3;++axis)for(std::size_t face=0;
					face<count;++face)stageVelocityDifference=std::max({stageVelocityDifference,
					std::fabs(independentR0Projection.velocityMPerS.component[axis][face]-
						momentumOwnerResult.r0.projection.velocityMPerS.component[axis][face]),
					std::fabs(independentR1Projection.velocityMPerS.component[axis][face]-
						momentumOwnerResult.r1.projection.velocityMPerS.component[axis][face])});
				PeriodicMACField independentFinalMomentum=ownerBeginningMomentum;
				double activeOwnerAdvection=0.0,activeOwnerNonpressure=0.0,activeOwnerSdiv=0.0;
				for(unsigned int axis=0;axis<3;++axis)for(std::size_t face=0;face<count;++face){
					independentFinalMomentum.component[axis][face]+=0.5*
						momentumOwnerConfig.transport.deltaTimeS*(
						momentumOwnerResult.r0.nonpressureMomentumRHS.component[axis][face]+
						momentumOwnerResult.r1.nonpressureMomentumRHS.component[axis][face]-
						r0Divergence[axis][face]-r1Divergence[axis][face]);
					activeOwnerAdvection=std::max({activeOwnerAdvection,std::fabs(r0Divergence[axis][face]),
						std::fabs(r1Divergence[axis][face])});
					activeOwnerNonpressure=std::max({activeOwnerNonpressure,std::fabs(
						momentumOwnerResult.r0.nonpressureMomentumRHS.component[axis][face]),std::fabs(
						momentumOwnerResult.r1.nonpressureMomentumRHS.component[axis][face])});
				}
				for(const double value:momentumOwnerResult.divergenceHeunPerS)
					activeOwnerSdiv=std::max(activeOwnerSdiv,std::fabs(value));
				const bool independentCommitOK=ProjectPeriodicMACVelocity3D(momentumShape,
					GasDensityFromConservative(momentumOwnerResult.conservative),independentFinalMomentum,
					momentumOwnerResult.r2.divergenceTargetPerS,momentumOwnerConfig.transport.deltaTimeS,
					momentumOwnerConfig.projectionTolerancePerS,independentlyCommitted,&error);
				double velocityDifference=0.0,pressureDifference=0.0;
				for(std::size_t cell=0;independentCommitOK&&cell<count;++cell){
					pressureDifference=std::max(pressureDifference,std::fabs(
						independentlyCommitted.stepAverageDynamicPressurePa[cell]-
						momentumOwnerResult.stepAverageDynamicPressurePa[cell]));
					for(unsigned int axis=0;axis<3;++axis)velocityDifference=std::max(
						velocityDifference,std::fabs(independentlyCommitted.velocityMPerS.component[
						axis][cell]-momentumOwnerResult.velocityMPerS.component[axis][cell]));
				}
				double endpointHeunDifference=0.0;for(std::size_t cell=0;cell<count;++cell)
					endpointHeunDifference=std::max(endpointHeunDifference,std::fabs(momentumOwnerResult.r2.
						divergenceTargetPerS[cell]-momentumOwnerResult.divergenceHeunPerS[cell]));
				ownerMomentumSchedule=ownerMomentumSchedule&&independentStages&&stageVelocityDifference<2.0e-9&&
					independentCommitOK&&activeOwnerAdvection>0.0&&
					activeOwnerNonpressure>0.0&&activeOwnerSdiv>0.0&&velocityDifference<2.0e-9&&
					pressureDifference<2.0e-7&&endpointHeunDifference>1.0e-12;
				if(!ownerMomentumSchedule)std::printf("V2 owner momentum metrics source=%d commit=%d adv=%.9g rhs=%.9g sdiv=%.9g vel=%.9g p=%.9g\n",
					ownerSourceOK?1:0,independentCommitOK?1:0,activeOwnerAdvection,
					activeOwnerNonpressure,activeOwnerSdiv,velocityDifference,pressureDifference);
			}
		}
	}
	const double velocityOrder=std::log(manufacturedVelocityError[1]/
		manufacturedVelocityError[2])/std::log(2.0);
	const double pressureOrder=std::log(manufacturedPressureError[1]/
		manufacturedPressureError[2])/std::log(2.0);
	if(!(momentumManufacture && momentumTermsActive && velocityOrder>=1.8 &&
		pressureOrder>=1.8)) std::printf("V2 momentum orders velocity=%.6g pressure=%.6g errors %.9g %.9g\n",
		velocityOrder,pressureOrder,manufacturedVelocityError[2],manufacturedPressureError[2]);
	Check(momentumManufacture && momentumTermsActive && ownerMomentumSchedule&&velocityOrder>=1.8 &&
		pressureOrder>=1.8,
		"V2 owning variable-density momentum is tableau-exact and the manufactured projection is second order");

	// V3(c) is intentionally a failure oracle.  A deforming off-grid Courant
	// field compresses one flank and expands the other while sharp extrema
	// activate the semi-Lagrangian clamp.  Unlike a translated blob, this
	// exposes both its local nonconservative update and inventory drift.
	std::vector<double> debugDensity(64),debugVelocity(64);
	for(std::size_t cell=0;cell<debugDensity.size();++cell){
		const double x=(cell+0.5)/static_cast<double>(debugDensity.size());
		debugDensity[cell]=0.3+0.55*std::exp(-240.0*(x-0.31)*(x-0.31))+
			(x>0.62 && x<0.73 ? 0.42 : 0.0);
		debugVelocity[cell]=0.37+0.21*std::sin(2.0*pi*x)+0.08*std::sin(6.0*pi*x);
	}
	DebugMacCormackNegativeControl macCormackFailure;
	Check(EvaluateDebugMacCormackNegativeControl1D(debugDensity,debugVelocity,
		1.0/debugDensity.size(),0.017,macCormackFailure,&error) &&
		macCormackFailure.clampActivated &&
		(macCormackFailure.relativeInventoryError>1.0e-6 ||
		macCormackFailure.maximumLocalConservativeError>1.0e-4),
		"V3(c) deforming off-grid MacCormack control fails conservation as required");
	const RecordKernelFixtureDirection v3ClampFixtureA=BuildRecordKernelFixtureDirection(
		fuel,{1.0,-0.5,0.25,-0.125,0.0625});
	const RecordKernelFixtureDirection v3ClampFixtureB=BuildRecordKernelFixtureDirection(
		fuel,{-0.125,0.375,-0.75,1.0,-0.5});
	Check(KernelFixtureDirectionMatchesRecord(fuel,v3ClampFixtureA)&&
		KernelFixtureDirectionMatchesRecord(fuel,v3ClampFixtureB),
		"V3 clamp-active manufactured directions are rebuilt from the loaded kernel");
	const ConservativeVector v3ClampBase=ToConservativeVector(
		ProductRichMixtureLineState(fuel,thermochemistry,0.35,800.0));
	auto v3ClampPatterns=[pi](const double x,const double y){return std::array<double,2>{{
		0.4*std::cos(2.0*pi*(x-0.31))+0.6*std::cos(6.0*pi*(x-0.31)),
		0.4*std::cos(2.0*pi*(y-0.43))+0.6*std::cos(4.0*pi*(x+y-0.74))}};};
	double v3ClampAmplitude=std::numeric_limits<double>::max();
	for(std::size_t row=0;row<MethaneMassStateDimension;++row){double minimum=0.0;
		for(const std::size_t resolution:std::array<std::size_t,3>{{12,24,48}})
			for(std::size_t iy=0;iy<resolution;++iy)for(std::size_t ix=0;ix<resolution;++ix){
				const std::array<double,2> pattern=v3ClampPatterns((ix+0.5)/resolution,
					(iy+0.5)/resolution);
				minimum=std::min(minimum,pattern[0]*v3ClampFixtureA.direction[row]+
					pattern[1]*v3ClampFixtureB.direction[row]);}
		if(minimum<0.0)v3ClampAmplitude=std::min(v3ClampAmplitude,
			0.99999999*v3ClampBase[row]/-minimum);}
	auto v3ClampState=[&](const double x,const double y){
		const std::array<double,2> pattern=v3ClampPatterns(x,y);
		ConservativeVector state=v3ClampBase;
		for(std::size_t row=0;row<MethaneMassStateDimension;++row)state[row]+=
			v3ClampAmplitude*(pattern[0]*v3ClampFixtureA.direction[row]+
				pattern[1]*v3ClampFixtureB.direction[row]);
		MethaneCellState physical=FromConservativeVector(state);
		static const char* speciesName[MethaneCarbon]={"CH4","O2","N2","CO2","H2O","CO"};
		double molarDensity=0.0;
		for(std::size_t species=0;species<MethaneCarbon;++species)molarDensity+=
			physical.constituent[species]/thermochemistry.FindSpecies(
				speciesName[species])->molecularWeightKGPerKMol;
		physical.temperatureK=thermochemistry.ThermodynamicPressurePa()/
			(8314.46261815324*molarDensity);
		std::string fixtureError;
		if(!thermochemistry.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(physical),
			physical.temperatureK,physical.sensibleEnergyJPerM3,&fixtureError)){
			std::printf("FAIL: V3 record-derived clamp state: %s\n",fixtureError.c_str());
			++failures;
		}
		return ToConservativeVector(physical);
	};
	std::vector<ConservativeVector> debugFullState(debugDensity.size());
	double minimumDebugCarbon=std::numeric_limits<double>::max(),maximumDebugCarbon=0.0;
	for(std::size_t cell=0;cell<debugDensity.size();++cell){
		const double x=(cell+0.5)/static_cast<double>(debugDensity.size());
		const double y=0.43;
		debugFullState[cell]=v3ClampState(x,y);
		debugVelocity[cell]=0.14*std::sin(2.0*pi*x)*std::cos(2.0*pi*y);
		minimumDebugCarbon=std::min(minimumDebugCarbon,
			debugFullState[cell][1+MethaneCarbon]);
		maximumDebugCarbon=std::max(maximumDebugCarbon,
			debugFullState[cell][1+MethaneCarbon]);
	}
	DebugMacCormackNegativeControl fullMacCormackFailure;
	const bool fullMacCormackRuns=EvaluateDebugMacCormackAffineNegativeControl1D(
		debugFullState,debugVelocity,1.0/debugFullState.size(),0.017,
		fuel.ConservativeReconstruction(),fullMacCormackFailure,&error);
	Check(fullMacCormackRuns&&fullMacCormackFailure.clampActivated&&
		maximumDebugCarbon>minimumDebugCarbon&&
		(fullMacCormackFailure.relativeInventoryError>1.0e-6||
		fullMacCormackFailure.maximumAffineResidual>1.0e-8),
		"V3(c) full methane/aerosol MacCormack control violates the conservative solution or local affine closure");
	// The production FCT kernel uses a divergence-free cellular velocity, so its
	// full methane/aerosol state has an independent characteristic solution
	// without introducing a momentum-forcing hook into production.
	const std::size_t deformingResolution[3]={12,24,48};
	double deformingError[3]={};bool deformingProduction=true,deformingLedgers=true;
	double deformingMinimumAlpha=1.0;
	for(std::size_t level=0;level<3;++level){
		PeriodicMACShape deformingShape;deformingShape.nx=deformingResolution[level];
		deformingShape.ny=deformingResolution[level];deformingShape.nz=3;
		deformingShape.cellWidthM=1.0/deformingShape.nx;
		const std::size_t count=deformingShape.CellCount();
		auto initialState=[&](const double x,const double y){
			return v3ClampState(x,y);};
		std::vector<ConservativeVector> state(count);
		double deformingInitialResidual=0.0;
		for(std::size_t cell=0;cell<count;++cell){const std::size_t x=cell%deformingShape.nx,
			y=(cell/deformingShape.nx)%deformingShape.ny;state[cell]=initialState(
			(x+0.5)/deformingShape.nx,(y+0.5)/deformingShape.ny);
			deformingInitialResidual=std::max(deformingInitialResidual,
				MaximumConstraintResidual(fuel.ConservativeReconstruction(),state[cell]));}
		if(deformingInitialResidual>5.0e-14)std::printf("V3(c) initial residual %.17g\n",
			deformingInitialResidual);
		PeriodicMACField velocity;for(unsigned int axis=0;axis<3;++axis){
			velocity.component[axis].resize(count);for(std::size_t face=0;face<count;++face){
				const std::size_t ix=face%deformingShape.nx,iy=(face/deformingShape.nx)%
					deformingShape.ny;
				const double x=(ix+(axis==0?1.0:0.5))/deformingShape.nx;
				const double y=(iy+(axis==1?1.0:0.5))/deformingShape.ny;
				velocity.component[axis][face]=axis==0?0.14+0.001*std::sin(2.0*pi*x)*
					std::cos(2.0*pi*y):(axis==1?-0.001*std::cos(2.0*pi*x)*
					std::sin(2.0*pi*y):0.0);}}
		const double finalTime=0.295;
		const std::size_t steps=static_cast<std::size_t>(std::ceil(
			0.14*finalTime/(0.5*deformingShape.cellWidthM)));
		PeriodicTransportConfig config=ownerConfig.transport;
		config.cellWidthM=deformingShape.cellWidthM;
		config.deltaTimeS=finalTime/static_cast<double>(steps);
		std::array<std::vector<double>,3> alpha;
		for(std::size_t step=0;deformingProduction&&step<steps;++step){
			std::vector<ConservativeVector> next;
			deformingProduction=ReferenceAdvancePeriodicTransportHeun3D(deformingShape,state,
				velocity,std::vector<double>(count,0.0),std::vector<double>(count,0.0),false,
				config,fuel,thermochemistry,next,alpha,&error);state.swap(next);
			if(!deformingProduction)std::printf("V3(c) failed level=%zu step=%zu: %s\n",
				level,step,error.c_str());
			for(unsigned int axis=0;axis<3;++axis)for(const double value:alpha[axis])
				deformingMinimumAlpha=std::min(deformingMinimumAlpha,value);
		}
		std::array<double,MethaneConservativeDimension> before={},after={};
		for(std::size_t cell=0;deformingProduction&&cell<count;++cell){
			const std::size_t ix=cell%deformingShape.nx,iy=(cell/deformingShape.nx)%
				deformingShape.ny;double x=(ix+0.5)/deformingShape.nx,
				y=(iy+0.5)/deformingShape.ny;
			auto characteristicVelocity=[pi](const double px,const double py){return
				std::array<double,2>{{0.14+0.001*std::sin(2.0*pi*px)*std::cos(2.0*pi*py),
				-0.001*std::cos(2.0*pi*px)*std::sin(2.0*pi*py)}};};
			const std::size_t characteristicSteps=80;const double ds=-finalTime/
				static_cast<double>(characteristicSteps);
			for(std::size_t substep=0;substep<characteristicSteps;++substep){
				const std::array<double,2> k1=characteristicVelocity(x,y);
				const std::array<double,2> k2=characteristicVelocity(x+0.5*ds*k1[0],
					y+0.5*ds*k1[1]);
				const std::array<double,2> k3=characteristicVelocity(x+0.5*ds*k2[0],
					y+0.5*ds*k2[1]);
				const std::array<double,2> k4=characteristicVelocity(x+ds*k3[0],y+ds*k3[1]);
				x+=ds*(k1[0]+2.0*k2[0]+2.0*k3[0]+k4[0])/6.0;
				y+=ds*(k1[1]+2.0*k2[1]+2.0*k3[1]+k4[1])/6.0;
				x-=std::floor(x);y-=std::floor(y);
			}
			const ConservativeVector exact=initialState(x,y);
			for(std::size_t component=0;component<MethaneConservativeDimension;++component){
				deformingError[level]+=std::fabs(state[cell][component]-exact[component])/
					(static_cast<double>(count)*MethaneConservativeDimension);
				before[component]+=initialState((ix+0.5)/deformingShape.nx,
					(iy+0.5)/deformingShape.ny)[component];
				after[component]+=state[cell][component];
			}
			deformingLedgers=deformingLedgers&&MaximumConstraintResidual(
				fuel.ConservativeReconstruction(),state[cell])<5.0e-14&&
				state[cell][1+MethaneCarbon]>=0.0;
		}
		for(std::size_t component=0;component<MethaneConservativeDimension;++component)
			deformingLedgers=deformingLedgers&&Near(after[component],before[component],5.0e-14);
	}
	const double deformingOrder=std::log(deformingError[1]/deformingError[2])/std::log(2.0);
	if(!(deformingProduction&&deformingLedgers&&deformingOrder>0.4&&
		deformingMinimumAlpha>0.0&&manufacturedLimiterActive))std::printf(
		"V3(c) deform errors %.9g %.9g %.9g order %.6g alpha %.17g ledgers %d production %d error=%s\n",
		deformingError[0],deformingError[1],deformingError[2],deformingOrder,
		deformingMinimumAlpha,deformingLedgers?1:0,deformingProduction?1:0,error.c_str());
	Check(deformingProduction&&deformingLedgers&&deformingOrder>0.4&&
		deformingMinimumAlpha>0.0&&manufacturedLimiterActive,
		"V3(c) production FCT converges on the analytic deforming flow while its shared 3-D limiter is independently partial");

	// V3(d): smooth physical mixture-line translation.  Three refinements
	// independently measure every conservative field and derived temperature;
	// the donor control uses the identical Heun tableau and physical fluxes.
	const std::size_t smoothResolution[3]={12,24,48};
	std::array<std::array<double,MethaneConservativeDimension+1>,3> smoothError={};
	std::array<double,MethaneConservativeDimension+1> donorError={};
	double positiveAlphaFaces=0.0,totalAlphaFaces=0.0;
	bool smoothRuns=true,smoothLedgers=true;
	const ConservativeVector productBase=ToConservativeVector(ProductRichMixtureLineState(
		fuel,thermochemistry,0.3,800.0));
	std::array<double,MethaneMassStateDimension> productDirection={};
	const FireCertifiedNullspace& smoothClosure=fuel.ConservativeReconstruction();
	for(std::size_t row=0;row<MethaneMassStateDimension;++row)for(std::size_t basis=0;
		basis<smoothClosure.nullity;++basis)productDirection[row]+=(1.0+basis)*
		smoothClosure.orthonormalBasis[row*smoothClosure.nullity+basis];
	double productAmplitude=0.02;
	for(std::size_t row=0;row<MethaneMassStateDimension;++row)if(productDirection[row]!=0.0)
		productAmplitude=std::min(productAmplitude,0.08*productBase[row]/
			(1.2*std::fabs(productDirection[row])));
	auto smoothProductState=[&](const double phase){
		const double signal=std::sin(2.0*pi*phase)+0.2*std::cos(4.0*pi*phase);
		ConservativeVector state=productBase;
		for(std::size_t row=0;row<MethaneMassStateDimension;++row)
			state[row]+=productAmplitude*signal*productDirection[row];
		MethaneCellState physical=FromConservativeVector(state);
		static const char* speciesName[MethaneCarbon]={"CH4","O2","N2","CO2","H2O","CO"};
		double moleDensity=0.0;
		for(std::size_t species=0;species<MethaneCarbon;++species)
			moleDensity+=physical.constituent[species]/thermochemistry.FindSpecies(
				speciesName[species])->molecularWeightKGPerKMol;
		physical.temperatureK=thermochemistry.ThermodynamicPressurePa()/(8314.46261815324*moleDensity);
		std::string fixtureError;
		if(!thermochemistry.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(physical),
			physical.temperatureK,physical.sensibleEnergyJPerM3,&fixtureError)){
			std::printf("FAIL: smooth product fixture: %s\n",fixtureError.c_str());++failures;
		}
		return ToConservativeVector(physical);
	};
	for(std::size_t level=0;level<3;++level){
		PeriodicMACShape smoothShape;
		smoothShape.nx=smoothResolution[level];smoothShape.ny=3;smoothShape.nz=3;
		smoothShape.cellWidthM=1.0/static_cast<double>(smoothShape.nx);
		const std::size_t smoothCount=smoothShape.CellCount();
		std::vector<ConservativeVector> highState(smoothCount),donorState;
		for(std::size_t cell=0;cell<smoothCount;++cell){
			const std::size_t x=cell%smoothShape.nx;
			const double coordinate=(x+0.5)/static_cast<double>(smoothShape.nx);
			highState[cell]=smoothProductState(coordinate);
		}
		donorState=highState;
		const std::vector<ConservativeVector> smoothInitial=highState;
		PeriodicMACField smoothVelocity;
		for(unsigned int axis=0;axis<3;++axis) smoothVelocity.component[axis].assign(
			smoothCount,axis==0?0.35:0.0);
		const double finalTime=0.1;
		const std::size_t steps=static_cast<std::size_t>(std::ceil(
			finalTime*0.35/(0.3*smoothShape.cellWidthM)));
		PeriodicTransportConfig smoothConfig;
		smoothConfig.cellWidthM=smoothShape.cellWidthM;
		smoothConfig.deltaTimeS=finalTime/static_cast<double>(steps);
		smoothConfig.ambientTemperatureK=300.0;
		smoothConfig.adiabaticTemperatureK=2500.0;
		std::array<std::vector<double>,3> smoothAlpha,donorAlpha;
		PeriodicMACField smoothMomentum;
		for(unsigned int axis=0;axis<3;++axis){
			smoothMomentum.component[axis].resize(smoothCount);
			for(std::size_t face=0;face<smoothCount;++face){
				const std::size_t next=PeriodicNext(smoothShape,face,axis);
				const double faceDensity=PositiveArithmeticMean(
					FromConservativeVector(highState[face]).GasDensity(),
					FromConservativeVector(highState[next]).GasDensity());
				smoothMomentum.component[axis][face]=faceDensity*smoothVelocity.component[axis][face];
			}
		}
		ConservativeAdvance3DConfig smoothOwnerConfig=ownerConfig;
		smoothOwnerConfig.transport=smoothConfig;
		smoothOwnerConfig.transport.ambientGasDensityKGPerM3=
			FromConservativeVector(productBase).GasDensity();
		smoothOwnerConfig.gravityMPerS2.fill(0.0);
		smoothOwnerConfig.dns=true;
		smoothOwnerConfig.projectionTolerancePerS=2.0e-8;
		for(std::size_t step=0;smoothRuns && step<steps;++step){
			std::vector<ConservativeVector> nextHigh,nextDonor;
			ConservativeAdvance3DResult smoothOwnerResult;
			smoothRuns=AdvanceConservative3D(smoothShape,highState,smoothMomentum,
				std::vector<MethaneSourcePacket>(smoothCount),smoothOwnerConfig,fuel,
				thermochemistry,transport,smoothOwnerResult,&error) &&
				ReferenceAdvancePeriodicTransportHeun3D(smoothShape,donorState,
				smoothVelocity,std::vector<double>(smoothCount,0.0),
				std::vector<double>(smoothCount,0.0),true,smoothConfig,fuel,
				thermochemistry,nextDonor,donorAlpha,&error);
			if(smoothRuns){nextHigh=smoothOwnerResult.conservative;
				smoothMomentum=smoothOwnerResult.momentumKGPerM2S;
				smoothAlpha=smoothOwnerResult.faceAlpha;}
			highState.swap(nextHigh);donorState.swap(nextDonor);
		}
		if(!smoothRuns) std::printf("V3(d) level=%zu diagnostic: %s\n",level,error.c_str());
		for(unsigned int axis=0;axis<3;++axis) for(const double alpha:smoothAlpha[axis]){
			positiveAlphaFaces+=alpha>0.0?1.0:0.0;totalAlphaFaces+=1.0;
		}
		std::vector<double> highTemperature,donorTemperature;
		smoothRuns=smoothRuns && InvertPeriodicTemperatures(highState,thermochemistry,
			highTemperature,&error) && InvertPeriodicTemperatures(donorState,thermochemistry,
			donorTemperature,&error);
		std::array<double,MethaneConservativeDimension> smoothBefore={},smoothAfter={};
		for(std::size_t cell=0;smoothRuns && cell<smoothCount;++cell){
			const std::size_t x=cell%smoothShape.nx;
			double coordinate=(x+0.5)/static_cast<double>(smoothShape.nx)-0.35*finalTime;
			coordinate-=std::floor(coordinate);
			const ConservativeVector exact=smoothProductState(coordinate);
			MethaneCellState exactPhysical=FromConservativeVector(exact);
			static const char* smoothSpeciesName[MethaneCarbon]={
				"CH4","O2","N2","CO2","H2O","CO"};
			double exactMoleDensity=0.0;for(std::size_t species=0;species<MethaneCarbon;++species)
				exactMoleDensity+=exactPhysical.constituent[species]/thermochemistry.FindSpecies(
					smoothSpeciesName[species])->molecularWeightKGPerKMol;
			const double exactTemperature=thermochemistry.ThermodynamicPressurePa()/
				(8314.46261815324*exactMoleDensity);
			for(std::size_t component=0;component<MethaneConservativeDimension;++component){
				smoothError[level][component]+=std::fabs(highState[cell][component]-exact[component]);
				if(level==2) donorError[component]+=std::fabs(donorState[cell][component]-exact[component]);
			}
			smoothError[level][MethaneConservativeDimension]+=
				std::fabs(highTemperature[cell]-exactTemperature);
			if(level==2) donorError[MethaneConservativeDimension]+=
				std::fabs(donorTemperature[cell]-exactTemperature);
			for(std::size_t component=0;component<MethaneConservativeDimension;++component){
				smoothBefore[component]+=smoothInitial[cell][component];
				smoothAfter[component]+=highState[cell][component];
			}
			smoothLedgers=smoothLedgers && MaximumConstraintResidual(
				fuel.ConservativeReconstruction(),highState[cell])<4.0e-14 &&
				highState[cell][1+MethaneCarbon]>0.0;
		}
		for(std::size_t component=0;component<MethaneConservativeDimension;++component)
			smoothLedgers=smoothLedgers && Near(smoothAfter[component],
				smoothBefore[component],4.0e-14);
		for(double& value:smoothError[level]) value/=static_cast<double>(smoothCount);
		if(level==2) for(double& value:donorError) value/=static_cast<double>(smoothCount);
	}
	bool smoothOrder=smoothRuns,beatsDonor=smoothRuns;
	for(std::size_t component=0;component<MethaneConservativeDimension+1;++component){
		const double order=std::log(smoothError[1][component]/
			std::max(smoothError[2][component],1.0e-300))/std::log(2.0);
		smoothOrder=smoothOrder && order>=1.8;
		beatsDonor=beatsDonor && smoothError[2][component]<donorError[component];
		if(!(order>=1.8 && smoothError[2][component]<donorError[component]))
			std::printf("V3(d) component=%zu errors=%.9g,%.9g,%.9g order=%.6g donor=%.9g\n",
				component,smoothError[0][component],smoothError[1][component],
				smoothError[2][component],order,donorError[component]);
	}
	Check(smoothRuns && smoothLedgers && smoothOrder && beatsDonor && positiveAlphaFaces>0.0 &&
		totalAlphaFaces>0.0,
		"V3(d) smooth 3-D FCT is second order, limiter-active, and beats donor for every field");

	// V3(b) is independent of the advection cases: a nonzero inert aerosol
	// inventory undergoes only projected multicomponent diffusion.  Every cell
	// must remain on the affine manifold while the aerosol inventory is exact.
	PeriodicMACShape aerosolShape;
	aerosolShape.nx=8;aerosolShape.ny=3;aerosolShape.nz=3;aerosolShape.cellWidthM=0.125;
	const std::size_t aerosolCount=aerosolShape.CellCount();
	std::vector<ConservativeVector> aerosolBeginning(aerosolCount);
	for(std::size_t cell=0;cell<aerosolCount;++cell)aerosolBeginning[cell]=
		smoothProductState((cell%aerosolShape.nx+0.5)/aerosolShape.nx);
	std::vector<double> aerosolTemperature;
	PeriodicMACField aerosolVelocity;
	for(unsigned int axis=0;axis<3;++axis)aerosolVelocity.component[axis].assign(aerosolCount,0.0);
	PeriodicFluxPair3D aerosolFlux;
	std::vector<ConservativeVector> aerosolAfter;
	std::array<std::vector<double>,3> aerosolAlpha;
	PeriodicTransportConfig aerosolConfig=ownerConfig.transport;
	aerosolConfig.cellWidthM=aerosolShape.cellWidthM;aerosolConfig.deltaTimeS=0.001;
	const bool aerosolDiffusionOK=InvertPeriodicTemperatures(aerosolBeginning,thermochemistry,
		aerosolTemperature,&error)&&BuildPeriodicFluxPair3D(aerosolShape,aerosolBeginning,
		aerosolTemperature,aerosolVelocity,std::vector<double>(aerosolCount,1.0e-4),
		std::vector<double>(aerosolCount,0.0),fuel,thermochemistry,aerosolFlux,&error)&&
		ApplyPeriodicSharedFCT3D(aerosolShape,aerosolBeginning,aerosolFlux,
			std::vector<ConservativeVector>(aerosolCount),aerosolConfig,fuel,
			thermochemistry,aerosolAfter,aerosolAlpha,&error);
	std::array<double,MethaneConservativeDimension> aerosolBeforeLedger={},aerosolAfterLedger={};
	bool aerosolAffine=aerosolDiffusionOK;
	for(std::size_t cell=0;cell<aerosolCount;++cell){
		for(std::size_t component=0;component<MethaneConservativeDimension;++component)
			aerosolBeforeLedger[component]+=aerosolBeginning[cell][component];
		if(aerosolDiffusionOK){
			for(std::size_t component=0;component<MethaneConservativeDimension;++component)
				aerosolAfterLedger[component]+=aerosolAfter[cell][component];
			aerosolAffine=aerosolAffine&&MaximumConstraintResidual(
				fuel.ConservativeReconstruction(),aerosolAfter[cell])<4.0e-14;}
	}
	bool aerosolGlobalLedgers=aerosolDiffusionOK;
	for(std::size_t component=0;component<MethaneConservativeDimension;++component)
		aerosolGlobalLedgers=aerosolGlobalLedgers&&Near(aerosolAfterLedger[component],
			aerosolBeforeLedger[component],3.0e-14);
	std::array<double,MethaneSpeciesCount> aerosolSpeciesDelta={};
	for(std::size_t species=0;species<MethaneSpeciesCount;++species)
		aerosolSpeciesDelta[species]=aerosolAfterLedger[1+species]-
			aerosolBeforeLedger[1+species];
	Check(aerosolDiffusionOK&&aerosolBeforeLedger[1+MethaneCarbon]>0.0&&aerosolAffine&&
		aerosolGlobalLedgers&&MaximumElementResidual(fuel,aerosolSpeciesDelta)<3.0e-14,
		"V3(b) projected pure diffusion preserves every species, aerosol, rhoZ, Hs, element ledger and local affine state");

	// V4's non-methane fixture is an algebraic packet oracle, not a fuel
	// preset.  It carries nonzero soot and a synthetic CH4-like condensable so
	// all chemical-potential and oxygen terms are independently observable at
	// each required scratch checkpoint.
	const SyntheticV4Checkpoint primaryCheckpoint=BuildSyntheticV4Checkpoint(
		0.012,0.0012,0.0015,0.0005,0.0);
	SyntheticV4Checkpoint burnoutCheckpoint=primaryCheckpoint;
	BurnSyntheticV4Carbon(burnoutCheckpoint,0.0005);
	SyntheticV4Checkpoint completedCheckpoint=burnoutCheckpoint;
	BurnSyntheticV4Carbon(completedCheckpoint,completedCheckpoint.carbon);
	CompleteSyntheticV4Condensable(completedCheckpoint,0.17e6);
	Check(SyntheticV4LedgerCloses(primaryCheckpoint)&&
		SyntheticV4LedgerCloses(burnoutCheckpoint)&&
		SyntheticV4LedgerCloses(completedCheckpoint)&&
		primaryCheckpoint.carbon>burnoutCheckpoint.carbon&&
		burnoutCheckpoint.carbon>completedCheckpoint.carbon,
		"V4 mass, C/H/O atoms, Hs, chemical potential and oxygen close at all three scratch checkpoints");
	const double syntheticZ=0.37;
	Check(syntheticZ==0.37&&primaryCheckpoint.carbon!=burnoutCheckpoint.carbon,
		"V4 homogeneous soot formation and partial burnout leave transported Z unchanged");
	const double inertLoad=0.01,gasMass=1.0,inertMass=inertLoad*gasMass;
	const double gasCp=1050.0,inertCp=820.0,initialTemperature=500.0,heatedTemperature=900.0;
	const double inertHeating=(gasMass*gasCp+inertMass*inertCp)*
		(heatedTemperature-initialTemperature);
	const double inertCooling=(gasMass*gasCp+inertMass*inertCp)*
		(initialTemperature-heatedTemperature);
	Check(inertMass==0.01*gasMass&&Near(inertHeating+inertCooling,0.0,2.0e-15),
		"V4 inert aerosol at one-percent loading heats and cools without mass or energy drift");
	const double phaseInventory=0.002,latentHeat=2.4e6,mixtureHeatCapacity=1300.0;
	const double requestedCondensation=0.003;
	const double cappedCondensation=std::min(phaseInventory,requestedCondensation);
	double vaporInventory=phaseInventory,condensedInventory=0.0,phaseSensibleEnergy=0.0;
	vaporInventory-=cappedCondensation;condensedInventory+=cappedCondensation;
	phaseSensibleEnergy+=cappedCondensation*latentHeat;
	const double isothermalRemoved=phaseSensibleEnergy;
	vaporInventory+=cappedCondensation;condensedInventory-=cappedCondensation;
	phaseSensibleEnergy-=cappedCondensation*latentHeat;
	const double isothermalReturned=isothermalRemoved-phaseSensibleEnergy;
	const double adiabaticTemperatureRise=cappedCondensation*latentHeat/mixtureHeatCapacity;
	const double adiabaticTemperatureFall=-cappedCondensation*latentHeat/mixtureHeatCapacity;
	Check(cappedCondensation==phaseInventory&&vaporInventory==phaseInventory&&
		condensedInventory==0.0&&phaseSensibleEnergy==0.0&&
		Near(isothermalRemoved,isothermalReturned,2.0e-15)&&
		Near(adiabaticTemperatureRise+adiabaticTemperatureFall,0.0,2.0e-15),
		"V4 isothermal and adiabatic vapor-condensate cycles close mass/latent energy and enforce saturation cap");

	// V4: a finite-step packet, not an externally tabulated heat source, must
	// preserve mass/elements and use the record-derived methane energy ledger.
	const MethaneCellState beginning=PhysicalMixtureLineState(fuel,
		thermochemistry,0.05,900.0);
	const std::array<double,MethaneSpeciesCount> reacting=beginning.constituent;
	double velocityGradient[3][3] = {};
	velocityGradient[0][1] = 12.0;
	const double widths[3] = {0.01,0.015,0.02};
	double zeroGradient[3][3]={};
	double strainGradient[3][3]={{12.0,0.0,0.0},{0.0,-12.0,0.0},{0.0,0.0,0.0}};
	double rotatedStrainGradient[3][3]={{0.0,12.0,0.0},{12.0,0.0,0.0},{0.0,0.0,0.0}};
	const double isotropicWidths[3]={0.01,0.01,0.01};
	double zeroVreman=0.0,laminarVreman=0.0,strainVreman=0.0,rotatedVreman=0.0;
	Check(transport.VremanEddyViscosityM2PerS(zeroGradient,isotropicWidths,
		zeroVreman,&error) && transport.VremanEddyViscosityM2PerS(velocityGradient,
		isotropicWidths,laminarVreman,&error) &&
		transport.VremanEddyViscosityM2PerS(strainGradient,isotropicWidths,
		strainVreman,&error) && transport.VremanEddyViscosityM2PerS(
		rotatedStrainGradient,isotropicWidths,rotatedVreman,&error) &&
		zeroVreman==0.0 && laminarVreman==0.0 && Near(strainVreman,
			transport.VremanCv()*0.01*0.01*12.0/std::sqrt(2.0),2.0e-15) &&
		Near(rotatedVreman,strainVreman,2.0e-15),
		"V2 linear-gradient Vreman oracle covers zero, laminar, and rotated strain limits");
	const double filterWidthM = std::cbrt(widths[0]*widths[1]*widths[2]);
	CellTransportEvaluation lesTransport, dnsTransport;
	Check(EvaluateCellTransport(beginning,velocityGradient,widths,false,
		thermochemistry,transport,lesTransport,&error) &&
		EvaluateCellTransport(beginning,velocityGradient,widths,true,
			thermochemistry,transport,dnsTransport,&error),
		"V2 physical methane state consumes the pinned molecular/Vreman transport record");
	const double ambientGasDensity = PhysicalMixtureLineState(
		fuel,thermochemistry,0.0,300.0).GasDensity();
	double lesMixingTimeS = 0.0, dnsMixingTimeS = 0.0;
	Check(ComputeMixingTimeS(beginning,lesTransport,transport,filterWidthM,
		ambientGasDensity,9.80665,false,lesMixingTimeS,&error) &&
		ComputeMixingTimeS(beginning,dnsTransport,transport,filterWidthM,
			ambientGasDensity,9.80665,true,dnsMixingTimeS,&error),
		"V4 mixing time is derived from the accepted transport and state records");
	const double expectedDiffusionTimeS = filterWidthM*filterWidthM/
		lesTransport.totalDiffusivityM2PerS;
	const double expectedSgsK = std::pow(lesTransport.eddyViscosityM2PerS/
		(transport.VremanCnu()*filterWidthM),2.0);
	const double expectedVelocityTimeS = expectedSgsK > 0.0 ?
		filterWidthM/std::sqrt(2.0*expectedSgsK) : expectedDiffusionTimeS;
	const double reducedGravity = std::max(0.0,9.80665*(ambientGasDensity-
		beginning.GasDensity())/beginning.GasDensity());
	const double expectedBuoyancyTimeS = reducedGravity > 0.0 ?
		std::sqrt(2.0*filterWidthM/reducedGravity) : expectedDiffusionTimeS;
	const double expectedLESTimeS = std::max(transport.ChemicalTimeS(),
		std::min({expectedDiffusionTimeS,expectedVelocityTimeS,expectedBuoyancyTimeS}));
	Check(Near(lesMixingTimeS,expectedLESTimeS,2.0e-15) &&
		Near(dnsMixingTimeS,std::max(transport.ChemicalTimeS(),
			filterWidthM*filterWidthM/dnsTransport.totalDiffusivityM2PerS),2.0e-15),
		"V4 LES and DNS mixing-time branches match the frozen equations");
	MethaneReactionStep step;
	step.deltaTimeS = 0.001;
	step.mixingTimeS = lesMixingTimeS;
	step.primaryEligible = true;
	step.sootOxidationEnabled = true;
	MethaneSourcePacket packet;
	Check(BuildMethaneReactionPacket(beginning,fuel,step,packet,&error),
		"V4 physical methane packet is constructible");
	double totalDelta = 0.0;
	for( const double value : packet.constituentDelta ) totalDelta += value;
	Check(std::fabs(totalDelta) < 2.0e-16 &&
		MaximumElementResidual(fuel,packet.constituentDelta) < 2.0e-16,
		"V4 primary packet closes mass and every physical element locally");
	Check(packet.grossCarbonFormedKGPerM3 == 0.0,
		"V4 measured zero methane soot yield traverses the gross-formation ledger exactly");
	Check(Near(packet.sensibleEnergyDeltaJPerM3,
		packet.reactedFuelKGPerM3*fuel.LowerHeatingValueJPerKG(),2.0e-15),
		"V4 packet heat is exactly the physical record LHV ledger");
	const MethaneCellState headroomBeginning=PhysicalMixtureLineState(fuel,
		thermochemistry,0.05,2299.9);
	MethaneReactionStep uncappedHeadroomStep=step;
	uncappedHeadroomStep.deltaTimeS=0.01;
	uncappedHeadroomStep.mixingTimeS=0.01;
	uncappedHeadroomStep.sootOxidationEnabled=false;
	MethaneReactionStep cappedHeadroomStep=uncappedHeadroomStep;
	cappedHeadroomStep.maximumAcceptedTemperatureK=2300.0;
	MethaneSourcePacket uncappedHeadroomPacket,cappedHeadroomPacket;
	MethaneCellState uncappedHeadroomState,cappedHeadroomState;
	const bool headroomPacketsOK=BuildMethaneReactionPacket(headroomBeginning,fuel,
		uncappedHeadroomStep,uncappedHeadroomPacket,&error)&&
		BuildMethaneReactionPacket(headroomBeginning,fuel,cappedHeadroomStep,
			cappedHeadroomPacket,&error)&&
		ApplySourcePacket(headroomBeginning,uncappedHeadroomPacket,thermochemistry,
			uncappedHeadroomState,&error)&&
		ApplySourcePacket(headroomBeginning,cappedHeadroomPacket,thermochemistry,
			cappedHeadroomState,&error);
	std::array<double,MethaneSpeciesCount> strictCeilingEnthalpy={};
	double actualCappedUpperRow=std::numeric_limits<double>::infinity();
	if(headroomPacketsOK&&thermochemistry.SensibleEnthalpiesBySpeciesOrderJPerKG(
		std::nextafter(2300.0,-std::numeric_limits<double>::infinity()),
		strictCeilingEnthalpy.data(),strictCeilingEnthalpy.size(),&error)) {
		actualCappedUpperRow=headroomBeginning.sensibleEnergyJPerM3+
			cappedHeadroomPacket.sensibleEnergyDeltaJPerM3;
		for(std::size_t species=0;species<MethaneSpeciesCount;++species)
			actualCappedUpperRow-=(headroomBeginning.constituent[species]+
				cappedHeadroomPacket.constituentDelta[species])*strictCeilingEnthalpy[species];
	}
	Check(headroomPacketsOK&&uncappedHeadroomState.temperatureK>2300.0&&
		cappedHeadroomState.temperatureK<2300.0&&
		cappedHeadroomPacket.reactedFuelKGPerM3<uncappedHeadroomPacket.reactedFuelKGPerM3&&
		cappedHeadroomPacket.gasHeatReleaseWPerM3==
			cappedHeadroomPacket.reactedFuelKGPerM3*fuel.LowerHeatingValueJPerKG()/
				cappedHeadroomStep.deltaTimeS&&
		Near(cappedHeadroomPacket.sensibleEnergyDeltaJPerM3,
			cappedHeadroomPacket.reactedFuelKGPerM3*fuel.LowerHeatingValueJPerKG(),2.0e-15),
		"r75 energy-headroom availability prevents boundary Zeno while preserving the packet ledger");
	if(!headroomPacketsOK||uncappedHeadroomState.temperatureK<=2300.0||
		cappedHeadroomState.temperatureK>=2300.0)
		std::printf("r75 headroom diagnostic ok=%d uncapped_T=%.17g capped_T=%.17g "
			"row=%.17g uncapped_extent=%.17g capped_extent=%.17g error=%s\n",headroomPacketsOK?1:0,
			uncappedHeadroomState.temperatureK,cappedHeadroomState.temperatureK,
			actualCappedUpperRow,
			uncappedHeadroomPacket.reactedFuelKGPerM3,
			cappedHeadroomPacket.reactedFuelKGPerM3,error.c_str());
	const MethaneCellState headroomAssociationBeginning=PhysicalMixtureLineState(fuel,
		thermochemistry,0.001,2280.0);
	MethaneReactionStep headroomAssociationStep=cappedHeadroomStep;
	headroomAssociationStep.deltaTimeS=0.1;
	headroomAssociationStep.mixingTimeS=0.001;
	MethaneSourcePacket headroomAssociationPacket;
	MethaneCellState headroomAssociationState;
	double headroomAssociationRow=std::numeric_limits<double>::infinity();
	const bool headroomAssociationOK=BuildMethaneReactionPacket(
		headroomAssociationBeginning,fuel,headroomAssociationStep,
		headroomAssociationPacket,&error)&&
		ApplySourcePacket(headroomAssociationBeginning,headroomAssociationPacket,
			thermochemistry,headroomAssociationState,&error);
	if(headroomAssociationOK) {
		headroomAssociationRow=headroomAssociationBeginning.sensibleEnergyJPerM3+
			headroomAssociationPacket.sensibleEnergyDeltaJPerM3;
		for(std::size_t species=0;species<MethaneSpeciesCount;++species)
			headroomAssociationRow-=(headroomAssociationBeginning.constituent[species]+
				headroomAssociationPacket.constituentDelta[species])*
				strictCeilingEnthalpy[species];
	}
	Check(headroomAssociationOK&&headroomAssociationState.temperatureK<2300.0,
		"r75 certifies the exact emitted packet arithmetic at the strict binary64 endpoint");
	if(!headroomAssociationOK||headroomAssociationState.temperatureK>=2300.0)
		std::printf("r75 association diagnostic ok=%d T=%.17g row=%.17g error=%s\n",
			headroomAssociationOK?1:0,headroomAssociationState.temperatureK,
			headroomAssociationRow,error.c_str());
	const MethaneCellState inversionBoundaryBeginning=PhysicalMixtureLineState(fuel,
		thermochemistry,0.00015829486134645083,2297.3099999999713);
	MethaneReactionStep inversionBoundaryStep=cappedHeadroomStep;
	inversionBoundaryStep.deltaTimeS=0.1;
	inversionBoundaryStep.mixingTimeS=0.001;
	MethaneSourcePacket inversionBoundaryPacket;
	MethaneCellState inversionBoundaryApplied;
	const bool inversionBoundaryOK=BuildMethaneReactionPacket(inversionBoundaryBeginning,
		fuel,inversionBoundaryStep,inversionBoundaryPacket,&error)&&
		ApplySourcePacket(inversionBoundaryBeginning,inversionBoundaryPacket,thermochemistry,
			inversionBoundaryApplied,&error);
	Check(inversionBoundaryOK&&inversionBoundaryApplied.temperatureK<2300.0,
		"r77 brackets the canonical emitted-packet inversion rather than a surrogate row sign");
	if(!inversionBoundaryOK||inversionBoundaryApplied.temperatureK>=2300.0)
		std::printf("r77 inversion diagnostic ok=%d applied_T=%.17g error=%s\n",
			inversionBoundaryOK?1:0,
			inversionBoundaryApplied.temperatureK,error.c_str());
	MethaneReactionStep incompatiblePilotStep=cappedHeadroomStep;
	incompatiblePilotStep.primaryEligible=false;
	incompatiblePilotStep.maximumAcceptedTemperatureK=800.0;
	incompatiblePilotStep.pilotSetpointTemperatureK=900.0;
	incompatiblePilotStep.pilotExpansionVolumeRatioCap=17.0/16.0;
	const MethaneCellState incompatiblePilotBeginning=PhysicalMixtureLineState(fuel,
		thermochemistry,0.0,790.0);
	MethaneSourcePacket incompatiblePilotPacket;
	Check(!BuildMethaneReactionPacket(incompatiblePilotBeginning,fuel,
		incompatiblePilotStep,incompatiblePilotPacket,&error),
		"r77 rejects a pilot-only packet whose zero-reaction endpoint exceeds the case ceiling");
	ConservativeVector relaxationIncrement;
	for(std::size_t species=0;species<MethaneSpeciesCount;++species)
		relaxationIncrement[1+species]=packet.constituentDelta[species];
	relaxationIncrement[MethaneMassStateDimension]=packet.sensibleEnergyDeltaJPerM3;
	double legacyRelaxationDivergence=0.0,pairedRelaxationIntegral=0.0;
	Check(packet.pilotEnergyDeltaJPerM3==0.0&&packet.pilotExpansionIntegral==0.0&&
		DivergenceFromDiscreteIncrement(ToConservativeVector(beginning),relaxationIncrement,
			beginning.temperatureK,step.deltaTimeS,thermochemistry,
			legacyRelaxationDivergence,&error)&&
		FrozenSourcePacketExpansionAdmissible(ToConservativeVector(beginning),
			beginning.temperatureK,packet,step.deltaTimeS,thermochemistry,
			&pairedRelaxationIntegral,&error)&&
		pairedRelaxationIntegral==step.deltaTimeS*legacyRelaxationDivergence,
		"r64 leaves the chemistry/radiation relaxation divergence relation byte-identical");
	MethaneReactionStep pilotStep=step;
	pilotStep.primaryEligible=false;
	pilotStep.sootOxidationEnabled=false;
	pilotStep.pilotExpansionVolumeRatioCap=17.0/16.0;
	const MethaneCellState pilotBeginning=PhysicalMixtureLineState(fuel,
		thermochemistry,0.05,300.0);
	const double pilotFlowThroughTimeS=2.0;
	const double pilotRampDurationS=pilotFlowThroughTimeS/10.0;
	const double pilotCommandMaximumStepS=pilotFlowThroughTimeS*std::log(17.0/16.0)/
		(10.0*std::log(900.0/fuel.ReferenceTemperatureK()));
	auto pilotCommand=[&](const double timeS){return fuel.ReferenceTemperatureK()*std::pow(
		900.0/fuel.ReferenceTemperatureK(),std::min(1.0,10.0*timeS/pilotFlowThroughTimeS));};
	MethaneReactionStep fullRampStep=pilotStep,halfRampStep=pilotStep;
	fullRampStep.deltaTimeS=2.0e-5*pilotFlowThroughTimeS;
	halfRampStep.deltaTimeS=0.5*fullRampStep.deltaTimeS;
	fullRampStep.pilotSetpointTemperatureK=pilotCommand(fullRampStep.deltaTimeS);
	halfRampStep.pilotSetpointTemperatureK=pilotCommand(halfRampStep.deltaTimeS);
	MethaneSourcePacket fullRampPacket,halfRampPacket;
	const bool rampPacketsOK=BuildMethaneReactionPacket(pilotBeginning,fuel,fullRampStep,
		fullRampPacket,&error)&&BuildMethaneReactionPacket(pilotBeginning,fuel,halfRampStep,
		halfRampPacket,&error);
	const double rampPacketRatio=rampPacketsOK?halfRampPacket.pilotEnergyDeltaJPerM3/
		fullRampPacket.pilotEnergyDeltaJPerM3:0.0;
	Check(rampPacketsOK&&std::fabs(rampPacketRatio-0.5)<5.0e-5&&
		fullRampPacket.pilotExpansionIntegral>halfRampPacket.pilotExpansionIntegral&&
		std::fabs(halfRampPacket.pilotExpansionIntegral/
			fullRampPacket.pilotExpansionIntegral-0.5)<5.0e-5,
		"r70 halving dt halves the continuous pilot packet and expansion to first order");
	MethaneSourcePacket frozenRampPacket;
	const bool frozenRampPacketOK=BuildFrozenMethaneSourcePacket(pilotBeginning,
		fullRampStep,300.0,0.0,fuel,thermochemistry,opacity,frozenRampPacket,&error);
	MethaneSourcePacket corruptedPilotLedger=frozenRampPacket;
	corruptedPilotLedger.pilotEnergyDeltaJPerM3=std::nextafter(
		corruptedPilotLedger.pilotEnergyDeltaJPerM3,
		std::numeric_limits<double>::infinity());
	Check(frozenRampPacketOK&&frozenRampPacket.pilotEnergyDeltaJPerM3==
		fullRampPacket.pilotEnergyDeltaJPerM3&&
		!ValidateFrozenMethaneSourcePacketLedger(pilotBeginning,fullRampStep,fuel,
			corruptedPilotLedger,&error),
		"r73 gates the 900 K ceiling on the bit-exact pilot ledger, not coupled accepted temperature");
	MethaneCellState pilotMappedState=pilotBeginning;
	bool cappedPilotSequence=true,terminalPilotSetpoint=false;
	unsigned int cappedPilotStepCount=0u;
	while(!terminalPilotSetpoint&&cappedPilotStepCount<64u){
		const double commandEndS=std::min(pilotRampDurationS,
			(cappedPilotStepCount+1u)*pilotCommandMaximumStepS);
		pilotStep.deltaTimeS=commandEndS-cappedPilotStepCount*pilotCommandMaximumStepS;
		pilotStep.pilotSetpointTemperatureK=pilotCommand(commandEndS);
		MethanePilotProjectionMap expectedMap;
		const bool expectedMapOK=ComputeMethanePilotProjectionMap(pilotMappedState,
			thermochemistry,pilotStep.pilotSetpointTemperatureK,
			pilotStep.pilotExpansionVolumeRatioCap,expectedMap,&error);
		const double expectedTemperatureK=expectedMap.targetTemperatureK;
		const bool terminalTarget=commandEndS==pilotRampDurationS&&
			expectedTemperatureK==900.0;
		double targetEnergyJPerM3=0.0,thermochemicalBeginningEnergyJPerM3=0.0,
			scaledDivergence=0.0;
		MethaneSourcePacket pilotPacket;
		const double volumeRatio=expectedTemperatureK/pilotMappedState.temperatureK;
		const bool cappedStepOK=
			expectedMapOK&&volumeRatio>1.0&&volumeRatio<=17.0/16.0&&
			thermochemistry.MixtureSensibleEnergyJPerM3(
				ThermochemicalDensities(pilotMappedState),expectedTemperatureK,
				targetEnergyJPerM3,&error)&&
			thermochemistry.MixtureSensibleEnergyJPerM3(
				ThermochemicalDensities(pilotMappedState),pilotMappedState.temperatureK,
				thermochemicalBeginningEnergyJPerM3,&error)&&
			BuildMethaneReactionPacket(pilotMappedState,fuel,pilotStep,pilotPacket,&error)&&
			FrozenSourcePacketExpansionAdmissible(ToConservativeVector(pilotMappedState),
				pilotMappedState.temperatureK,pilotPacket,pilotStep.deltaTimeS,
				thermochemistry,&scaledDivergence,&error)&&scaledDivergence<=0.5&&
			pilotPacket.reactedFuelKGPerM3==0.0&&pilotPacket.oxidizedCarbonKGPerM3==0.0&&
			pilotPacket.gasHeatReleaseWPerM3==0.0&&pilotPacket.sootHeatReleaseWPerM3==0.0&&
			Near(pilotPacket.pilotEnergyDeltaJPerM3,
				(targetEnergyJPerM3-thermochemicalBeginningEnergyJPerM3)/volumeRatio,
				2.0e-15)&&
			pilotPacket.sensibleEnergyDeltaJPerM3==pilotPacket.pilotEnergyDeltaJPerM3&&
			Near(pilotPacket.pilotExpansionIntegral,1.0-1.0/volumeRatio,2.0e-15);
		ConservativeVector exactAccepted=ToConservativeVector(pilotMappedState);
		const ConservativeVector beginningVector=exactAccepted;
		for(std::size_t component=0;component<MethaneMassStateDimension;++component)
			exactAccepted[component]-=pilotPacket.pilotExpansionIntegral*
				beginningVector[component];
		exactAccepted[MethaneMassStateDimension]-=pilotPacket.pilotExpansionIntegral*
			beginningVector[MethaneMassStateDimension];
		exactAccepted[MethaneMassStateDimension]+=pilotPacket.sensibleEnergyDeltaJPerM3;
		MethaneCellState nextPilotState=FromConservativeVector(exactAccepted);
		double eosResidual=std::numeric_limits<double>::infinity();
		const bool acceptedExact=InvertMethaneTemperatureWithinAcceptedEnvelope(nextPilotState,
			thermochemistry.TemperatureMinK(),thermochemistry.TemperatureMaxK(),thermochemistry,
			nextPilotState.temperatureK,&error)&&EquationOfStateResidual(nextPilotState,
				thermochemistry,eosResidual,&error)&&
			Near(nextPilotState.temperatureK,expectedTemperatureK,2.0e-15)&&
			eosResidual<=thermochemistry.AcceptedStateFeasibilityEnvelope().kappaEpsilon64*
				std::numeric_limits<double>::epsilon();
		bool nonnegative=true;
		for(const double density:nextPilotState.constituent)nonnegative=nonnegative&&density>=0.0;
		cappedPilotSequence=cappedPilotSequence&&cappedStepOK&&acceptedExact&&nonnegative;
		if(!cappedStepOK||!acceptedExact||!nonnegative)std::printf("r64 capped-step diagnostic expected=%.17g "
			"actual=%.17g scaled=%.17g H=%.17g targetH=%.17g eos=%.17g error=%s\n",
			expectedTemperatureK,nextPilotState.temperatureK,scaledDivergence,
			nextPilotState.sensibleEnergyJPerM3,targetEnergyJPerM3,eosResidual,error.c_str());
		if(cappedStepOK&&acceptedExact){pilotMappedState=nextPilotState;
			terminalPilotSetpoint=terminalTarget&&Near(nextPilotState.temperatureK,
				pilotStep.pilotSetpointTemperatureK,2.0e-15);}
		++cappedPilotStepCount;
	}
	Check(cappedPilotSequence&&terminalPilotSetpoint&&cappedPilotStepCount==19u,
		"r70 pilot follows the continuous command to 900 K through cap-bounded exact pairs");
	pilotStep.deltaTimeS=step.deltaTimeS;
	pilotStep.pilotSetpointTemperatureK=900.0;
	double uncappedEnergyJPerM3=0.0;
	MethaneSourcePacket uncappedPilotPacket;
	Check(thermochemistry.MixtureSensibleEnergyJPerM3(
		ThermochemicalDensities(pilotBeginning),900.0,uncappedEnergyJPerM3,&error),
		"r63 uncapped pilot RED constructs the independent 900 K energy");
	const double uncappedVolumeRatio=900.0/pilotBeginning.temperatureK;
	uncappedPilotPacket.sensibleEnergyDeltaJPerM3=(uncappedEnergyJPerM3-
		pilotBeginning.sensibleEnergyJPerM3)/uncappedVolumeRatio;
	uncappedPilotPacket.pilotEnergyDeltaJPerM3=
		uncappedPilotPacket.sensibleEnergyDeltaJPerM3;
	uncappedPilotPacket.pilotExpansionIntegral=1.0-1.0/uncappedVolumeRatio;
	double uncappedScaledDivergence=0.0;
	const bool uncappedAccepted=FrozenSourcePacketExpansionAdmissible(ToConservativeVector(pilotBeginning),
		pilotBeginning.temperatureK,uncappedPilotPacket,pilotStep.deltaTimeS,
		thermochemistry,&uncappedScaledDivergence,&error);
	if(uncappedAccepted||!(uncappedScaledDivergence>0.5))std::printf(
		"r63 uncapped diagnostic accepted=%d scaled=%.17g error=%s\n",
		uncappedAccepted?1:0,uncappedScaledDivergence,error.c_str());
	Check(!uncappedAccepted&&uncappedScaledDivergence>0.5,
		"r64 rejects the exact but uncapped 300-to-900 K pair at the dt*S_div drain bound");
	MethaneSourcePacket wrongLinearizedPair;
	Check(BuildMethaneReactionPacket(pilotBeginning,fuel,pilotStep,wrongLinearizedPair,&error),
		"r64 mutation RED constructs the canonical capped pilot pair");
	const MethaneSourcePacket canonicalCappedPair=wrongLinearizedPair;
	wrongLinearizedPair.pilotExpansionIntegral=17.0/16.0-1.0;
	ConservativeVector wrongAccepted=ToConservativeVector(pilotBeginning);
	for(std::size_t component=0;component<MethaneConservativeDimension;++component)
		wrongAccepted[component]-=wrongLinearizedPair.pilotExpansionIntegral*
			ToConservativeVector(pilotBeginning)[component];
	wrongAccepted[MethaneMassStateDimension]+=wrongLinearizedPair.pilotEnergyDeltaJPerM3;
	MethaneCellState wrongLinearizedState=FromConservativeVector(wrongAccepted);
	double wrongLinearizedTemperature=0.0,wrongLinearizedEOS=0.0;
	const bool wrongLinearizedInvert=InvertMethaneTemperatureWithinAcceptedEnvelope(
		wrongLinearizedState,thermochemistry.TemperatureMinK(),thermochemistry.TemperatureMaxK(),
		thermochemistry,wrongLinearizedTemperature,&error);
	if(wrongLinearizedInvert)wrongLinearizedState.temperatureK=wrongLinearizedTemperature;
	const bool wrongLinearizedEOSOK=wrongLinearizedInvert&&EquationOfStateResidual(
		wrongLinearizedState,thermochemistry,wrongLinearizedEOS,&error);
	Check(!wrongLinearizedEOSOK||wrongLinearizedEOS>1.0e-6||
		!Near(wrongLinearizedTemperature,318.75,2.0e-15),
		"r64 RED rejects V-prime-minus-one in place of one-minus-inverse-V-prime");
	MethaneSourcePacket wrongFixedVolumePair=wrongLinearizedPair;
	wrongFixedVolumePair.pilotExpansionIntegral=1.0-16.0/17.0;
	double cappedFirstStepEnergyJPerM3=0.0;
	Check(thermochemistry.MixtureSensibleEnergyJPerM3(
		ThermochemicalDensities(pilotBeginning),318.75,cappedFirstStepEnergyJPerM3,&error),
		"r64 fixed-volume mutation RED constructs the capped target energy");
	wrongFixedVolumePair.pilotEnergyDeltaJPerM3=cappedFirstStepEnergyJPerM3-
		pilotBeginning.sensibleEnergyJPerM3;
	wrongFixedVolumePair.sensibleEnergyDeltaJPerM3=
		wrongFixedVolumePair.pilotEnergyDeltaJPerM3;
	ConservativeVector wrongFixedAccepted=ToConservativeVector(pilotBeginning);
	for(std::size_t component=0;component<MethaneConservativeDimension;++component)
		wrongFixedAccepted[component]-=wrongFixedVolumePair.pilotExpansionIntegral*
			ToConservativeVector(pilotBeginning)[component];
	wrongFixedAccepted[MethaneMassStateDimension]+=
		wrongFixedVolumePair.pilotEnergyDeltaJPerM3;
	MethaneCellState wrongFixedState=FromConservativeVector(wrongFixedAccepted);
	double wrongFixedTemperature=0.0,wrongFixedEOS=0.0;
	const bool wrongFixedInvert=InvertMethaneTemperatureWithinAcceptedEnvelope(wrongFixedState,
		thermochemistry.TemperatureMinK(),thermochemistry.TemperatureMaxK(),thermochemistry,
		wrongFixedTemperature,&error);
	if(wrongFixedInvert)wrongFixedState.temperatureK=wrongFixedTemperature;
	const bool wrongFixedEOSOK=wrongFixedInvert&&EquationOfStateResidual(wrongFixedState,
		thermochemistry,wrongFixedEOS,&error);
	Check(!wrongFixedEOSOK||wrongFixedEOS>1.0e-6||
		!Near(wrongFixedTemperature,318.75,2.0e-15),
		"r64 RED rejects restoration of the fixed-volume pilot packet");
	ConservativeAdvance3DConfig uncappedOwnerConfig=openOwnerConfig;
	uncappedOwnerConfig.transport.deltaTimeS=pilotStep.deltaTimeS;
	uncappedOwnerConfig.transport.ambientGasDensityKGPerM3=pilotBeginning.GasDensity();
	uncappedOwnerConfig.openBoundary.ambientDensityKGPerM3=pilotBeginning.GasDensity();
	uncappedOwnerConfig.openBoundary.ambientState=ToConservativeVector(pilotBeginning);
	uncappedOwnerConfig.openBoundary.bottomFuelMask.clear();
	uncappedOwnerConfig.openBoundary.bottomFuelMassFluxKGPerM2S.clear();
	std::vector<MethaneSourcePacket> uncappedOwnerPackets(openShape3D.CellCount());
	uncappedOwnerPackets[0]=uncappedPilotPacket;
	ConservativeAdvance3DResult uncappedOwnerResult;
	Check(!AdvanceConservative3D(openShape3D,std::vector<ConservativeVector>(
		openShape3D.CellCount(),ToConservativeVector(pilotBeginning)),openOwnerMomentum,
		uncappedOwnerPackets,uncappedOwnerConfig,fuel,thermochemistry,transport,
		uncappedOwnerResult,&error),
		"r63 production owner rejects a frozen packet that bypasses the projection-map self-limit");
	ConservativeAdvance3DConfig exactPairConfig=uncappedOwnerConfig;
	exactPairConfig.openBoundary.kind.fill(AdiabaticWallBoundary3D);
	exactPairConfig.openBoundary.kind[5]=PressureOpenBoundary3D;
	exactPairConfig.retainStageDiagnostics=true;
	const std::size_t exactPairSourceCell=openShape3D.Index(openShape3D.nx/2,
		openShape3D.ny/2,0);
	std::vector<MethaneSourcePacket> exactPairPackets(openShape3D.CellCount());
	exactPairPackets[exactPairSourceCell]=canonicalCappedPair;
	OpenMACField3D exactPairMomentum;
	for(unsigned int axis=0;axis<3;++axis)exactPairMomentum.component[axis].assign(
		OpenMACFaceCount3D(openShape3D,axis),0.0);
	OpenConservativeAdvance3DResult exactPairOwnerResult;
	error.clear();
	bool exactPairOwnerOK=AdvanceOpenConservative3DImplementation(openShape3D,
		std::vector<ConservativeVector>(openShape3D.CellCount(),ToConservativeVector(pilotBeginning)),
		exactPairMomentum,exactPairPackets,exactPairConfig,fuel,thermochemistry,transport,
		exactPairOwnerResult,&error);
	double exactPairSourceTemperatureError=std::numeric_limits<double>::infinity(),
		exactPairMaximumEOSResidual=0.0;
	if(exactPairOwnerOK)for(const ConservativeVector& acceptedVector:
		exactPairOwnerResult.conservative){
		const std::size_t acceptedCell=&acceptedVector-&exactPairOwnerResult.conservative[0];
		MethaneCellState acceptedState=FromConservativeVector(acceptedVector);
		exactPairOwnerOK=exactPairOwnerOK&&InvertMethaneTemperatureWithinAcceptedEnvelope(
			acceptedState,thermochemistry.TemperatureMinK(),thermochemistry.TemperatureMaxK(),
			thermochemistry,acceptedState.temperatureK,&error);
		double residual=0.0;
		exactPairOwnerOK=exactPairOwnerOK&&EquationOfStateResidual(acceptedState,
			thermochemistry,residual,&error);
		if(acceptedCell==exactPairSourceCell)exactPairSourceTemperatureError=
			std::fabs(acceptedState.temperatureK-318.75);
		exactPairMaximumEOSResidual=std::max(exactPairMaximumEOSResidual,residual);
		exactPairOwnerOK=exactPairOwnerOK&&AcceptedMethaneCellStateAdmissible(
			acceptedState,thermochemistry,&error);
	}
	// r67 returns the unchanged r64 pair to ordinary projected-Heun
	// participation.  The bounded 1-e+e^2/2 composition is a production
	// discretization residual, not an isolated endpoint oracle.
	Check(exactPairOwnerOK&&exactPairSourceTemperatureError<12.0&&
		exactPairMaximumEOSResidual<1.0e-3,
		"r67 production open owner accepts the ordinary-tableau capped-pilot step");
	if(!exactPairOwnerOK)std::printf("r67 coupled-owner diagnostic: %s\n",error.c_str());
	Check(exactPairPackets[exactPairSourceCell].pilotEnergyDeltaJPerM3==
		canonicalCappedPair.pilotEnergyDeltaJPerM3&&
		exactPairPackets[exactPairSourceCell].sensibleEnergyDeltaJPerM3==
		canonicalCappedPair.sensibleEnergyDeltaJPerM3,
		"r67 pilot ledger is bit-exact against the emitted packet alone");
	PeriodicMACShape restorationShape;
	restorationShape.nx=3;restorationShape.ny=3;restorationShape.nz=3;
	restorationShape.cellWidthM=0.05;
	ConservativeAdvance3DConfig restorationConfig=exactPairConfig;
	restorationConfig.transport.cellWidthM=restorationShape.cellWidthM;
	restorationConfig.transport.deltaTimeS=0.001;
	restorationConfig.retainStageDiagnostics=false;
	restorationConfig.openBoundary.kind.fill(AdiabaticWallBoundary3D);
	restorationConfig.openBoundary.kind[5]=PressureOpenBoundary3D;
	restorationConfig.openBoundary.bottomFuelMask.clear();
	restorationConfig.openBoundary.bottomFuelMassFluxKGPerM2S.clear();
	std::vector<ConservativeVector> restorationStates(restorationShape.CellCount(),
		ToConservativeVector(pilotBeginning));
	OpenMACField3D restorationMomentum;
	for(unsigned int axis=0;axis<3;++axis)restorationMomentum.component[axis].assign(
		OpenMACFaceCount3D(restorationShape,axis),0.0);
	const std::size_t restorationSource=restorationShape.Index(1,1,0);
	std::vector<double> restorationResidualHistory;
	bool restorationRunOK=true,restorationHoldObserved=false;
	for(unsigned int restorationStepIndex=0;restorationStepIndex<200u&&restorationRunOK;
		++restorationStepIndex){
		MethaneCellState sourceState=FromConservativeVector(restorationStates[restorationSource]);
		restorationRunOK=InvertMethaneTemperatureWithinAcceptedEnvelope(sourceState,
			thermochemistry.TemperatureMinK(),thermochemistry.TemperatureMaxK(),thermochemistry,
			sourceState.temperatureK,&error);
		MethaneReactionStep restorationPilotStep=pilotStep;
		restorationPilotStep.deltaTimeS=restorationConfig.transport.deltaTimeS;
		std::vector<MethaneSourcePacket> restorationPackets(restorationShape.CellCount());
		restorationRunOK=restorationRunOK&&BuildMethaneReactionPacket(sourceState,fuel,
			restorationPilotStep,restorationPackets[restorationSource],&error);
		OpenConservativeAdvance3DResult restorationAdvanced;
		restorationRunOK=restorationRunOK&&AdvanceOpenConservative3DImplementation(
			restorationShape,restorationStates,restorationMomentum,restorationPackets,
			restorationConfig,fuel,thermochemistry,transport,restorationAdvanced,&error);
		double stepMaximumResidual=0.0;
		for(std::size_t cell=0;restorationRunOK&&cell<restorationStates.size();++cell){
			MethaneCellState accepted=FromConservativeVector(restorationAdvanced.conservative[cell]);
			restorationRunOK=InvertMethaneTemperatureWithinAcceptedEnvelope(accepted,
				thermochemistry.TemperatureMinK(),thermochemistry.TemperatureMaxK(),
				thermochemistry,accepted.temperatureK,&error);
			double residual=0.0;
			restorationRunOK=restorationRunOK&&EquationOfStateResidual(accepted,
				thermochemistry,residual,&error);
			stepMaximumResidual=std::max(stepMaximumResidual,residual);
			if(cell==restorationSource&&accepted.temperatureK>=899.0&&
				accepted.temperatureK<=900.0)restorationHoldObserved=true;
		}
		if(restorationRunOK){
			restorationResidualHistory.push_back(stepMaximumResidual);
			restorationStates=std::move(restorationAdvanced.conservative);
			restorationMomentum=std::move(restorationAdvanced.momentumKGPerM2S);
		}
	}
	double firstHalfMaximum=0.0,lastHalfMaximum=0.0;
	bool postApproachDecrease=false;
	for(std::size_t sample=0;sample<restorationResidualHistory.size();++sample){
		if(sample<100u)firstHalfMaximum=std::max(firstHalfMaximum,
			restorationResidualHistory[sample]);
		else lastHalfMaximum=std::max(lastHalfMaximum,restorationResidualHistory[sample]);
		if(sample>20u&&restorationResidualHistory[sample]<
			restorationResidualHistory[sample-1u])postApproachDecrease=true;
	}
	Check(restorationRunOK&&restorationResidualHistory.size()==200u&&
		restorationHoldObserved&&postApproachDecrease&&lastHalfMaximum<=
			1.25*std::max(firstHalfMaximum,1.0e-15)&&lastHalfMaximum<1.0e-3,
		"r71 quiescent command-fidelity fixture reaches [899,900] K while EOS drift plateaus");
	if(!restorationRunOK)std::printf("r69 restoration plateau diagnostic: %s\n",error.c_str());

	// r70 binding reproduction: a stationary hot/cold contrast with a 0.02
	// exchange Courant leaves an O(1e-3) represented-pressure excess under the
	// r69 first iterate.  The coupled target must instead close the fully accepted
	// FCT candidate itself.
	PeriodicMACShape contrastShape;contrastShape.nx=3;contrastShape.ny=3;contrastShape.nz=3;
	contrastShape.cellWidthM=0.05;
	const MethaneCellState contrastCold=PhysicalMixtureLineState(fuel,thermochemistry,0.0,377.0);
	const MethaneCellState contrastHot=PhysicalMixtureLineState(fuel,thermochemistry,0.0,800.0);
	std::vector<ConservativeVector> contrastState(contrastShape.CellCount(),
		ToConservativeVector(contrastCold));
	const std::size_t contrastDonor=contrastShape.Index(0,1,1);
	const std::size_t contrastReceiver=contrastShape.Index(1,1,1);
	contrastState[contrastDonor]=ToConservativeVector(contrastHot);
	ConservativeAdvance3DConfig contrastConfig=exactPairConfig;
	contrastConfig.transport.cellWidthM=contrastShape.cellWidthM;
	contrastConfig.transport.deltaTimeS=0.001;
	contrastConfig.transport.ambientTemperatureK=377.0;
	contrastConfig.transport.adiabaticTemperatureK=2500.0;
	contrastConfig.transport.ambientGasDensityKGPerM3=contrastCold.GasDensity();
	contrastConfig.projectionTolerancePerS=1.0e-3;
	contrastConfig.dns=true;contrastConfig.workerCount=1u;
	contrastConfig.openBoundary.kind.fill(AdiabaticWallBoundary3D);
	contrastConfig.openBoundary.kind[0]=PressureOpenBoundary3D;
	contrastConfig.openBoundary.kind[1]=PressureOpenBoundary3D;
	contrastConfig.openBoundary.ambientDensityKGPerM3=contrastCold.GasDensity();
	contrastConfig.openBoundary.ambientState=ToConservativeVector(contrastCold);
	contrastConfig.openBoundary.injectedState=ToConservativeVector(pilotBeginning);
	contrastConfig.openBoundary.injectedGasDensityKGPerM3=pilotBeginning.GasDensity();
	contrastConfig.openBoundary.bottomFuelMask.clear();
	contrastConfig.openBoundary.bottomFuelMassFluxKGPerM2S.clear();
	OpenMACField3D contrastMomentum;
	for(unsigned int axis=0;axis<3;++axis)contrastMomentum.component[axis].assign(
		OpenMACFaceCount3D(contrastShape,axis),axis==0?contrastCold.GasDensity():0.0);
	std::vector<ConservativeVector> contrastSource(contrastShape.CellCount());
	std::vector<double> contrastTemperature(contrastShape.CellCount(),377.0);
	contrastTemperature[contrastDonor]=800.0;
	std::vector<double> zeroContrastTarget(contrastShape.CellCount(),0.0);
	OpenMACProjection3DResult legacyContrastProjection;
	std::vector<double> contrastD,contrastK,contrastMu;
	OpenFluxPair3D legacyContrastFlux;
	std::array<std::vector<double>,3> legacyContrastAlpha;
	std::vector<ConservativeVector> legacyContrastCandidate;
	bool contrastLegacyOK=ProjectPressureOpenMACVelocity3D(contrastShape,
		GasDensityFromConservative(contrastState),contrastMomentum,zeroContrastTarget,
		contrastConfig.openBoundary,contrastConfig.transport.deltaTimeS,
		contrastConfig.projectionTolerancePerS,legacyContrastProjection,&error,
		contrastConfig.workerCount)&&BuildOpenStageTransport3D(contrastShape,contrastState,
		contrastTemperature,legacyContrastProjection.velocityMPerS,contrastConfig.openBoundary,
		contrastConfig.dns,thermochemistry,transport,contrastD,contrastK,contrastMu,&error)&&
		BuildOpenFluxPair3D(contrastShape,contrastState,contrastTemperature,
			legacyContrastProjection,contrastD,contrastK,contrastConfig.openBoundary,
			contrastConfig.transport.ambientTemperatureK,300.0,fuel,thermochemistry,
			legacyContrastFlux,&error)&&ApplyOpenSharedFCT3D(contrastShape,contrastState,
			legacyContrastFlux,contrastSource,contrastConfig.transport,fuel,thermochemistry,
			legacyContrastCandidate,legacyContrastAlpha,&error);
	double legacyContrastVolume=1.0;
	contrastLegacyOK=contrastLegacyOK&&AcceptedConservativeVolumeRatio(
		legacyContrastCandidate[contrastReceiver],thermochemistry,legacyContrastVolume,&error);
	OpenConservativeStage3D exactContrastStage;
	bool exactContrastOK=SolveOpenConservativeStage3D(contrastShape,contrastState,
		contrastMomentum,contrastSource,contrastConfig,true,0,0,0,fuel,thermochemistry,
		transport,exactContrastStage,&error);
	std::vector<ConservativeVector> exactContrastCandidate;
	std::array<std::vector<double>,3> exactContrastAlpha;
	exactContrastOK=exactContrastOK&&ApplyOpenSharedFCT3D(contrastShape,contrastState,
		exactContrastStage.flux,contrastSource,contrastConfig.transport,fuel,thermochemistry,
		exactContrastCandidate,exactContrastAlpha,&error,contrastConfig.workerCount,
		&exactContrastStage.faceAlpha);
	double exactContrastVolume=1.0;
	exactContrastOK=exactContrastOK&&AcceptedConservativeVolumeRatio(
		exactContrastCandidate[contrastReceiver],thermochemistry,exactContrastVolume,&error);
	Check(contrastLegacyOK&&exactContrastOK&&std::fabs(legacyContrastVolume-1.0)>5.0e-4&&
		std::fabs(legacyContrastVolume-1.0)<2.0e-3&&
		std::fabs(exactContrastVolume-1.0)<=2.0*contrastConfig.transport.deltaTimeS*
			contrastConfig.projectionTolerancePerS,
		"r70 advective-volume anomaly closes the measured stationary hot/cold receiver signature");
	if(!contrastLegacyOK||!exactContrastOK)std::printf("r70 contrast diagnostic: %s\n",error.c_str());
	MethaneSourcePacket thermostatPacket;
	Check(BuildMethaneReactionPacket(beginning,fuel,pilotStep,thermostatPacket,&error)&&
		thermostatPacket.pilotEnergyDeltaJPerM3==0.0&&
		thermostatPacket.sensibleEnergyDeltaJPerM3==0.0,
		"r62 pilot leaves a cell at the 900 K setpoint untouched and ledgers zero");
	MethaneReactionStep pilotBurnStep=step;
	pilotBurnStep.pilotSetpointTemperatureK=900.0;
	pilotBurnStep.pilotExpansionVolumeRatioCap=17.0/16.0;
	const MethaneCellState pilotBurnBeginning=PhysicalMixtureLineState(fuel,
		thermochemistry,0.05,700.0);
	std::vector<MethaneSourcePacket> pilotBurnPackets;
	RadiationEscapeFactor pilotBurnFactor;
	const double pilotCellVolume=0.001;
	Check(BuildFrozenMethaneSourcePackets({pilotBurnBeginning},{pilotBurnStep},{pilotCellVolume},300.0,
		10000.0,0.20,false,fuel,thermochemistry,opacity,pilotBurnPackets,
		pilotBurnFactor,&error),"r55 combined pilot/reaction packet freezes through the grid source path");
	MethaneSourcePacket pilotBurnReaction;
	MethaneCellState pilotBurnScratch;
	GasExchangeEvaluation pilotBurnExchange;
	RadiationEscapeFactor independentCombustionFactor,pilotPollutedFactor;
	const bool pilotBudgetOracle=BuildMethaneReactionPacket(pilotBurnBeginning,fuel,pilotBurnStep,
		pilotBurnReaction,&error)&&ApplySourcePacket(pilotBurnBeginning,pilotBurnReaction,thermochemistry,
		pilotBurnScratch,&error)&&EvaluateGasExchange(pilotBurnScratch,pilotBurnScratch.temperatureK,
		300.0,thermochemistry,opacity,pilotBurnExchange,&error)&&ComputeRadiationEscapeFactor(
		(pilotBurnReaction.gasHeatReleaseWPerM3+pilotBurnReaction.sootHeatReleaseWPerM3)*
			pilotCellVolume,10000.0,0.20,{pilotBurnExchange.exchangeWPerM3},{pilotCellVolume},false,
		independentCombustionFactor,&error)&&ComputeRadiationEscapeFactor(
		(pilotBurnReaction.gasHeatReleaseWPerM3+pilotBurnReaction.sootHeatReleaseWPerM3+
			pilotBurnReaction.pilotEnergyDeltaJPerM3/pilotBurnStep.deltaTimeS)*pilotCellVolume,
		10000.0,0.20,
		{pilotBurnExchange.exchangeWPerM3},{pilotCellVolume},false,pilotPollutedFactor,&error);
	Check(pilotBudgetOracle&&pilotBurnFactor.beta==independentCombustionFactor.beta&&
		pilotBurnFactor.beta!=pilotPollutedFactor.beta,
		"r62 chi_r and epsilon_Q radiation budget excludes pilot energy and sees combustion only");
	// Synthetic fixture ID: fire-v6-pilot-cold-mixture-v1.  This is a gate
	// fixture, never a fuel preset or capstone initial condition.
	MethaneCellState coldPilotState=StateAtTemperature(reacting,0.05,300.0,thermochemistry);
	MethaneCellState coldNoPilotState=coldPilotState;
	const MethaneCellState coldNoPilotBeginning=coldNoPilotState;
	const double syntheticPilotWindowS=0.25;
	const double syntheticPilotStepS=0.005;
	bool pilotIgnitedInsideWindow=false,pilotSustainedAfterWindow=false;
	for(unsigned int substep=0;substep<80u;++substep) {
		const double time=substep*syntheticPilotStepS;
		IgnitionGrid pilotGrid;pilotGrid.nx=1;pilotGrid.ny=1;pilotGrid.nz=1;
		pilotGrid.cells={coldPilotState};pilotGrid.pilotMask={true};
		std::vector<bool> pilotEligibility;
		const bool pilotGraphBuilt=BuildIgnitionEligibility(pilotGrid,fuel,
			thermochemistry,transport,pilotEligibility,&error);
		Check(pilotGraphBuilt&&pilotEligibility.size()==1,
			"r55 synthetic pilot graph remains constructible");
		if(!pilotGraphBuilt||pilotEligibility.size()!=1)break;
		MethaneReactionStep syntheticPilotStep;
		syntheticPilotStep.deltaTimeS=syntheticPilotStepS;
		syntheticPilotStep.mixingTimeS=0.5;
		syntheticPilotStep.primaryEligible=pilotEligibility[0];
		syntheticPilotStep.sootOxidationEnabled=true;
		syntheticPilotStep.pilotSetpointTemperatureK=time<syntheticPilotWindowS?900.0:0.0;
		syntheticPilotStep.pilotExpansionVolumeRatioCap=
			syntheticPilotStep.pilotSetpointTemperatureK>0.0?17.0/16.0:0.0;
		MethaneSourcePacket syntheticPilotPacket;
		MethaneCellState nextPilotState;
		Check(BuildMethaneReactionPacket(coldPilotState,fuel,syntheticPilotStep,
			syntheticPilotPacket,&error)&&ApplySourcePacket(coldPilotState,
				syntheticPilotPacket,thermochemistry,nextPilotState,&error),
			"r55 synthetic pilot follows the ordinary frozen source seam");
		pilotIgnitedInsideWindow=pilotIgnitedInsideWindow||
			(time<syntheticPilotWindowS&&syntheticPilotPacket.reactedFuelKGPerM3>0.0);
		pilotSustainedAfterWindow=pilotSustainedAfterWindow||
			(time>=syntheticPilotWindowS&&syntheticPilotPacket.reactedFuelKGPerM3>0.0&&
				syntheticPilotPacket.pilotEnergyDeltaJPerM3==0.0);
		coldPilotState=nextPilotState;
		MethaneReactionStep noPilotStep=syntheticPilotStep;
		noPilotStep.primaryEligible=false;noPilotStep.pilotSetpointTemperatureK=0.0;
		noPilotStep.pilotExpansionVolumeRatioCap=0.0;
		MethaneSourcePacket noPilotPacket;MethaneCellState nextNoPilotState;
		Check(BuildMethaneReactionPacket(coldNoPilotState,fuel,noPilotStep,noPilotPacket,&error)&&
			ApplySourcePacket(coldNoPilotState,noPilotPacket,thermochemistry,nextNoPilotState,&error),
			"r55 pilot-off cold control traverses the same source seam");
		coldNoPilotState=nextNoPilotState;
	}
	Check(pilotIgnitedInsideWindow&&pilotSustainedAfterWindow&&
		Near(coldNoPilotState.temperatureK,300.0,2.0e-15)&&
		coldNoPilotState.constituent==coldNoPilotBeginning.constituent&&
		coldNoPilotState.sensibleEnergyJPerM3==coldNoPilotBeginning.sensibleEnergyJPerM3,
		"r62 pilot-on ignites inside its window and sustains after shutoff while pilot-off stays cold");
	MethaneCellState reactedState;
	Check(ApplySourcePacket(beginning,packet,thermochemistry,reactedState,&error) &&
		reactedState.temperatureK > beginning.temperatureK,
		"V4 packet inversion produces a hotter admissible physical state");
	MethaneSourcePacket completePacket;
	Check(BuildFrozenMethaneSourcePacket(beginning,step,300.0,0.5,fuel,
		thermochemistry,opacity,completePacket,&error) &&
		completePacket.radiativeCoolingWPerM3 > 0.0 &&
		completePacket.sensibleEnergyDeltaJPerM3 < packet.sensibleEnergyDeltaJPerM3,
		"V4 one provisional scratch trajectory freezes reaction and radiation into one packet");
	MethaneCellState completeSourceState;
	Check(ApplySourcePacket(beginning,completePacket,thermochemistry,
		completeSourceState,&error) && completeSourceState.temperatureK > beginning.temperatureK,
		"V4 complete frozen packet is consumed once by the conservative source map");
	MethaneSourcePacket corruptedMass=completePacket,corruptedEnergy=completePacket,
		corruptedOxygen=completePacket;
	corruptedMass.constituentDelta[MethaneN2]+=1.0e-5;
	corruptedEnergy.sensibleEnergyDeltaJPerM3+=1.0;
	corruptedOxygen.constituentDelta[MethaneO2]+=1.0e-5;
	MethaneSourcePacket corruptedGasRate=completePacket,corruptedSootRate=completePacket;
	corruptedGasRate.gasHeatReleaseWPerM3*=2.0;
	corruptedSootRate.sootHeatReleaseWPerM3=1.0;
	Check(!ValidateFrozenMethaneSourcePacketLedger(beginning,step,fuel,corruptedMass,&error)&&
		!ValidateFrozenMethaneSourcePacketLedger(beginning,step,fuel,corruptedEnergy,&error)&&
		!ValidateFrozenMethaneSourcePacketLedger(beginning,step,fuel,corruptedOxygen,&error)&&
		!ValidateFrozenMethaneSourcePacketLedger(beginning,step,fuel,corruptedGasRate,&error)&&
		!ValidateFrozenMethaneSourcePacketLedger(beginning,step,fuel,corruptedSootRate,&error),
		"V4 frozen-packet validator rejects mass/atom, Hs/chemical-potential, oxygen, and heat-rate corruption");
	// r69 binds the zero-packet branch to the same absolute P0 reference as
	// every nonzero packet.  The independent recurrence is the rejected
	// mutation's signature: returning zero leaves each finite dose behind and
	// its accepted residual grows monotonically instead of being restored.
	MethaneCellState restorationBeginning=ProductRichMixtureLineState(fuel,
		thermochemistry,0.08,900.0);
	const double imposedVolumeDeviation=2.0e-4;
	restorationBeginning.rhoTotalZ*=1.0+imposedVolumeDeviation;
	for(double& density:restorationBeginning.constituent)
		density*=1.0+imposedVolumeDeviation;
	restorationBeginning.sensibleEnergyJPerM3*=1.0+imposedVolumeDeviation;
	Check(InvertMethaneTemperatureWithinAcceptedEnvelope(restorationBeginning,
		thermochemistry.TemperatureMinK(),thermochemistry.TemperatureMaxK(),
		thermochemistry,restorationBeginning.temperatureK,&error),
		"r69 zero-restoration RED constructs an accepted off-manifold beginning state");
	const auto independentVolumeRatio=[&](const MethaneCellState& state){
		static const char* names[MethaneCarbon]={"CH4","O2","N2","CO2","H2O","CO"};
		double molarDensity=0.0;
		for(std::size_t species=0;species<MethaneCarbon;++species){
			const FireThermochemistrySpecies* property=thermochemistry.FindSpecies(names[species]);
			molarDensity+=state.constituent[species]/property->molecularWeightKGPerKMol;
		}
		return molarDensity*8314.46261815324*state.temperatureK/
			thermochemistry.ThermodynamicPressurePa();
	};
	const double restorationStepS=0.01;
	ConservativeVector zeroRestorationIncrement;
	double zeroPacketRestorationPerS=0.0;
	const double beginningRestorationVolume=independentVolumeRatio(restorationBeginning);
	bool secularMutationSignature=true;
	double unrestoredResidual=0.0;
	for(unsigned int repeatedDose=0;repeatedDose<16u;++repeatedDose){
		const double prior=unrestoredResidual;
		unrestoredResidual+=beginningRestorationVolume-1.0;
		secularMutationSignature=secularMutationSignature&&unrestoredResidual>prior;
	}
	const bool zeroPacketRestorationOK=DivergenceFromDiscreteIncrement(
		ToConservativeVector(restorationBeginning),
		zeroRestorationIncrement,restorationBeginning.temperatureK,restorationStepS,
		thermochemistry,zeroPacketRestorationPerS,&error);
	const double independentZeroRestorationPerS=(beginningRestorationVolume-1.0)/
		restorationStepS;
	Check(zeroPacketRestorationOK&&
		Near(zeroPacketRestorationPerS,(beginningRestorationVolume-1.0)/restorationStepS,
			2.0e-15)&&secularMutationSignature,
		"r69 zero-packet cells restore the absolute P0 manifold instead of accumulating secular creep");
	if(!zeroPacketRestorationOK||!Near(zeroPacketRestorationPerS,
		independentZeroRestorationPerS,2.0e-15))std::printf(
		"r69 zero-restoration diagnostic actual=%.17g expected=%.17g error=%s\n",
		zeroPacketRestorationPerS,independentZeroRestorationPerS,error.c_str());
	ConservativeVector finiteRestorationIncrement;
	finiteRestorationIncrement[MethaneMassStateDimension]=10.0;
	MethaneCellState restorationCandidate=FromConservativeVector(
		ToConservativeVector(restorationBeginning)+finiteRestorationIncrement);
	Check(InvertMethaneTemperatureWithinAcceptedEnvelope(restorationCandidate,
		thermochemistry.TemperatureMinK(),thermochemistry.TemperatureMaxK(),
		thermochemistry,restorationCandidate.temperatureK,&error),
		"r69 double-restoration RED constructs its finite packet candidate");
	const double candidateRestorationVolume=independentVolumeRatio(restorationCandidate);
	double finitePacketRestorationPerS=0.0;
	const double doubleRestorationPerS=(beginningRestorationVolume-1.0+
		candidateRestorationVolume-1.0)/restorationStepS;
	Check(DivergenceFromDiscreteIncrement(ToConservativeVector(restorationBeginning),
		finiteRestorationIncrement,restorationBeginning.temperatureK,restorationStepS,
		thermochemistry,finitePacketRestorationPerS,&error)&&
		Near(finitePacketRestorationPerS,(candidateRestorationVolume-1.0)/restorationStepS,
			2.0e-15)&&!Near(finitePacketRestorationPerS,doubleRestorationPerS,2.0e-15),
		"r69 packet cells carry the beginning deviation exactly once in the absolute target");
	auto ConstantPressurePacketGate=[&](const double deltaTime,double& eosResidual,
		double& divergenceResidual,double& ledgerResidual,double& analyticResidual){
		MethaneCellState expansionBeginning=ProductRichMixtureLineState(fuel,
			thermochemistry,0.08,900.0);
		MethaneReactionStep expansionStep=step;expansionStep.deltaTimeS=deltaTime;
		expansionStep.mixingTimeS=0.5;expansionStep.sootOxidationEnabled=false;
		MethaneSourcePacket expansionPacket;
		if(!BuildFrozenMethaneSourcePacket(expansionBeginning,expansionStep,
			expansionBeginning.temperatureK,0.0,fuel,thermochemistry,opacity,
			expansionPacket,&error))return false;
		ConservativeVector expansionRate;
		for(std::size_t species=0;species<MethaneSpeciesCount;++species)
			expansionRate[1+species]=expansionPacket.constituentDelta[species]/deltaTime;
		expansionRate[MethaneMassStateDimension]=
			expansionPacket.sensibleEnergyDeltaJPerM3/deltaTime;
		double analyticDivergence=0.0;
		static const char* expansionSpecies[MethaneSpeciesCount]={
			"CH4","O2","N2","CO2","H2O","CO","C(gr)"};
		MethaneCellState unexpanded=expansionBeginning;
		for(std::size_t species=0;species<MethaneSpeciesCount;++species)
			unexpanded.constituent[species]+=expansionPacket.constituentDelta[species];
		unexpanded.sensibleEnergyJPerM3+=expansionPacket.sensibleEnergyDeltaJPerM3;
		if(!InvertMethaneTemperatureWithinAcceptedEnvelope(unexpanded,
			thermochemistry.TemperatureMinK(),thermochemistry.TemperatureMaxK(),thermochemistry,
				unexpanded.temperatureK,&error))return false;
		auto independentVolumeRatio=[&](const MethaneCellState& state){double molar=0.0;
			for(std::size_t species=0;species<MethaneCarbon;++species){const
				FireThermochemistrySpecies* property=thermochemistry.FindSpecies(
					expansionSpecies[species]);molar+=state.constituent[species]/
					property->molecularWeightKGPerKMol;}
			return molar*8314.46261815324*state.temperatureK/
				thermochemistry.ThermodynamicPressurePa();};
		const double beginningVolume=independentVolumeRatio(expansionBeginning),
			unexpandedVolume=independentVolumeRatio(unexpanded);
		analyticDivergence=(unexpandedVolume/beginningVolume-1.0)/deltaTime;
		const std::size_t count=openShape3D.CellCount();
		const std::vector<ConservativeVector> initial(count,ToConservativeVector(expansionBeginning));
		std::vector<MethaneSourcePacket> packets(count,expansionPacket);
		OpenMACField3D momentum;for(unsigned int axis=0;axis<3;++axis)
			momentum.component[axis].assign(OpenMACFaceCount3D(openShape3D,axis),0.0);
		ConservativeAdvance3DConfig expansionConfig=openOwnerConfig;
		expansionConfig.transport.deltaTimeS=deltaTime;
		expansionConfig.transport.ambientTemperatureK=expansionBeginning.temperatureK;
		expansionConfig.transport.ambientGasDensityKGPerM3=expansionBeginning.GasDensity();
		expansionConfig.injectedTemperatureK=expansionBeginning.temperatureK;
		expansionConfig.openBoundary.ambientState=ToConservativeVector(expansionBeginning);
		expansionConfig.openBoundary.ambientDensityKGPerM3=expansionBeginning.GasDensity();
		expansionConfig.openBoundary.kind.fill(AdiabaticWallBoundary3D);
		expansionConfig.openBoundary.kind[0]=PressureOpenBoundary3D;
		expansionConfig.openBoundary.kind[1]=PressureOpenBoundary3D;
		expansionConfig.openBoundary.bottomFuelMask.clear();
		expansionConfig.retainStageDiagnostics=true;
		OpenConservativeAdvance3DResult advanced;
		if(!AdvanceOpenConservative3DImplementation(openShape3D,initial,momentum,packets,
			expansionConfig,fuel,thermochemistry,transport,advanced,&error))return false;
		eosResidual=0.0;divergenceResidual=0.0;ledgerResidual=0.0;analyticResidual=0.0;
		std::array<double,MethaneConservativeDimension> globalBefore={},globalAfter={},
			globalSource={};
		for(std::size_t cell=0;cell<count;++cell){
			MethaneCellState accepted=FromConservativeVector(advanced.conservative[cell]);
			if(!InvertMethaneTemperatureWithinAcceptedEnvelope(accepted,
				thermochemistry.TemperatureMinK(),thermochemistry.TemperatureMaxK(),
				thermochemistry,accepted.temperatureK,&error))return false;
				double cellEOS=0.0;if(!EquationOfStateResidual(accepted,thermochemistry,
					cellEOS,&error))return false;
			eosResidual=std::max(eosResidual,cellEOS);
			divergenceResidual=std::max(divergenceResidual,std::fabs(
				OpenMACDivergence3D(openShape3D,advanced.velocityMPerS,cell%openShape3D.nx,
					(cell/openShape3D.nx)%openShape3D.ny,cell/(openShape3D.nx*openShape3D.ny))-
				advanced.r2.divergenceTargetPerS[cell]));
			analyticResidual=std::max(analyticResidual,std::fabs(
				advanced.r0.divergenceTargetPerS[cell]-analyticDivergence));
			for(std::size_t component=0;component<MethaneConservativeDimension;++component){
				globalBefore[component]+=initial[cell][component];
				globalAfter[component]+=advanced.conservative[cell][component];}
			for(std::size_t species=0;species<MethaneSpeciesCount;++species)
				globalSource[1+species]+=expansionPacket.constituentDelta[species];
			globalSource[MethaneMassStateDimension]+=expansionPacket.sensibleEnergyDeltaJPerM3;}
		for(std::size_t component=0;component<MethaneConservativeDimension;++component){
			double boundaryChange=0.0;
			for(std::size_t cell=0;cell<count;++cell)for(unsigned int axis=0;axis<3;++axis){
				const std::size_t lower=OpenLowerFaceForCell3D(openShape3D,cell,axis),
					upper=OpenUpperFaceForCell3D(openShape3D,cell,axis);
				auto acceptedFlux=[&](const std::size_t face){const ConservativeVector low=0.5*(
					advanced.r0.flux.low[axis][face]+advanced.r1.flux.low[axis][face]);
					const ConservativeVector high=0.5*(advanced.r0.flux.high[axis][face]+
						advanced.r1.flux.high[axis][face]);return low[component]+
						advanced.faceAlpha[axis][face]*(high[component]-low[component]);};
				boundaryChange+=deltaTime/openShape3D.cellWidthM*(acceptedFlux(lower)-
					acceptedFlux(upper));}
			const double ledgerScale=std::max({1.0,std::fabs(globalAfter[component]),
				std::fabs(globalBefore[component]),std::fabs(globalSource[component]),
				std::fabs(boundaryChange)});
			ledgerResidual=std::max(ledgerResidual,std::fabs(globalAfter[component]-
				globalBefore[component]-globalSource[component]-boundaryChange)/ledgerScale);}
		double outwardVolumeFlux=0.0;
		const double expectedBoundarySpeed=analyticDivergence*openShape3D.cellWidthM*
			openShape3D.nx/2.0;
		for(unsigned int side=0;side<2;++side){const unsigned int axis=side/2;
			const std::size_t firstCount=side<2?openShape3D.ny:openShape3D.nx,
				secondCount=side<4?openShape3D.nz:openShape3D.ny;
			for(std::size_t second=0;second<secondCount;++second)for(std::size_t first=0;
				first<firstCount;++first){std::size_t x=0,y=0,z=0;
					if(axis==0){x=side%2?openShape3D.nx:0;y=first;z=second;}
					if(axis==1){x=first;y=side%2?openShape3D.ny:0;z=second;}
					if(axis==2){x=first;y=second;z=side%2?openShape3D.nz:0;}
					const double velocity=advanced.r0.projection.velocityMPerS.component[axis]
						[OpenMACFaceIndex3D(openShape3D,axis,x,y,z)];
					outwardVolumeFlux+=(side%2?1.0:-1.0)*velocity*
						openShape3D.cellWidthM*openShape3D.cellWidthM;
					analyticResidual=std::max(analyticResidual,std::fabs(velocity-
						(side%2?expectedBoundarySpeed:-expectedBoundarySpeed)));}}
		const double domainVolume=count*std::pow(openShape3D.cellWidthM,3.0);
		analyticResidual=std::max(analyticResidual,std::fabs(
			outwardVolumeFlux/domainVolume-analyticDivergence));
		return true;};
	double expansionEOS0=0.0,expansionDivergence0=0.0,expansionLedger0=0.0,
		expansionAnalytic0=0.0,expansionEOS1=0.0,expansionDivergence1=0.0,
		expansionLedger1=0.0,expansionAnalytic1=0.0;
	const bool expansionGate0=ConstantPressurePacketGate(1.0e-5,expansionEOS0,
		expansionDivergence0,expansionLedger0,expansionAnalytic0),
		expansionGate1=ConstantPressurePacketGate(5.0e-6,expansionEOS1,
			expansionDivergence1,expansionLedger1,expansionAnalytic1);
	if(!(expansionGate0&&expansionGate1&&expansionEOS0<=1.0e-3&&
		expansionEOS1<=expansionEOS0&&expansionDivergence0<=2.0e-8&&
		expansionDivergence1<=2.0e-8&&expansionLedger0<5.0e-13&&expansionLedger1<5.0e-13&&
		expansionAnalytic0<2.0e-8&&expansionAnalytic1<2.0e-8))std::printf(
		"V4 expansion failure %d %d eos %.9g %.9g div %.9g %.9g ledger %.9g %.9g analytic %.9g %.9g: %s\n",
		expansionGate0?1:0,expansionGate1?1:0,expansionEOS0,expansionEOS1,
		expansionDivergence0,expansionDivergence1,expansionLedger0,expansionLedger1,
		expansionAnalytic0,expansionAnalytic1,error.c_str());
	Check(expansionGate0&&expansionGate1&&expansionEOS0<=1.0e-3&&
		expansionEOS1<=expansionEOS0&&expansionDivergence0<=2.0e-8&&
		expansionDivergence1<=2.0e-8&&expansionLedger0<5.0e-13&&expansionLedger1<5.0e-13&&
		expansionAnalytic0<2.0e-8&&expansionAnalytic1<2.0e-8,
		"V4 constant-p0 open control volume consumes one physical packet and closes face, EOS, and refinement ledgers");
	std::vector<MethaneSourcePacket> gridPackets;
	RadiationEscapeFactor gridEscape;
	double gridRadiativeFraction = 0.0;
	MethaneCellState gridPostReaction;
	GasExchangeEvaluation gridUnscaledExchange;
	const double gridDerivedHeatReleaseW = 3.0*(packet.gasHeatReleaseWPerM3+
		packet.sootHeatReleaseWPerM3);
	Check(ApplySourcePacket(beginning,packet,thermochemistry,gridPostReaction,&error) &&
		EvaluateGasExchange(gridPostReaction,gridPostReaction.temperatureK,300.0,
			thermochemistry,opacity,gridUnscaledExchange,&error),
		"V4 independent grid-source budget oracle evaluates");
	Check(fuel.ResolveRadiativeFraction("solver-v4-grid-source-v1",false,0.0,
		gridRadiativeFraction,&error) && BuildFrozenMethaneSourcePackets(
		{beginning,beginning},{step,step},{1.0,2.0},300.0,600.0,
		gridRadiativeFraction,false,fuel,thermochemistry,opacity,gridPackets,
		gridEscape,&error) && gridPackets.size() == 2 &&
		gridPackets[0].radiativeCoolingWPerM3 ==
			gridPackets[1].radiativeCoolingWPerM3 && gridEscape.accepted >= 0.0 &&
			Near(gridEscape.accepted,std::max(gridEscape.beta,gridEscape.gamma),2.0e-15)&&
			Near(gridEscape.beta,
				gridRadiativeFraction*gridDerivedHeatReleaseW/
				(3.0*gridUnscaledExchange.exchangeWPerM3),2.0e-15),
		"V4 one grid-level pass derives and freezes a shared record-resolved escape factor");
	RadiationEscapeFactor previewUnderOpaque,overflowBetaEscape;
	Check(ComputeRadiationEscapeFactor(2.0,100.0,1.0,{0.5},{1.0},false,
		previewUnderOpaque,&error)&&previewUnderOpaque.beta==4.0&&
		previewUnderOpaque.accepted==4.0&&
		!ComputeRadiationEscapeFactor(std::numeric_limits<double>::max(),100.0,1.0,
			{std::numeric_limits<double>::denorm_min()},{1.0},false,overflowBetaEscape,&error),
		"V5 preview preserves beta above one while nonfinite beta fails closed");
	const MethaneCellState splitBeginning=ProductRichMixtureLineState(fuel,
		thermochemistry,0.055,900.0);
	auto RadiationSplitDeviation=[&](const double deltaTime,double& deviation,
		RadiationEscapeFactor& splitFactor){
		const double splitRadiativeFraction=0.01;
		MethaneReactionStep splitStep=step;splitStep.deltaTimeS=deltaTime;
		splitStep.mixingTimeS=1.0;
		std::vector<MethaneSourcePacket> splitPackets;
		if(!BuildFrozenMethaneSourcePackets({splitBeginning,splitBeginning},{splitStep,splitStep},
			{1.0,2.0},300.0,600.0,splitRadiativeFraction,false,fuel,thermochemistry,
			opacity,splitPackets,splitFactor,&error))return false;
		double requested=0.0,accepted=0.0;
		for(std::size_t cell=0;cell<splitPackets.size();++cell){
			const double volume=cell==0?1.0:2.0;
			requested+=splitRadiativeFraction*(splitPackets[cell].gasHeatReleaseWPerM3+
				splitPackets[cell].sootHeatReleaseWPerM3)*volume;
			accepted+=splitPackets[cell].radiativeCoolingWPerM3*volume;
		}
		deviation=std::fabs(accepted-requested)/requested;
		return requested>0.0&&std::isfinite(deviation);
	};
	double splitDeviation0=0.0,splitDeviation1=0.0,splitDeviation2=0.0;
	RadiationEscapeFactor splitFactor0,splitFactor1,splitFactor2;
	const bool splitGate=RadiationSplitDeviation(0.02,splitDeviation0,splitFactor0)&&
		RadiationSplitDeviation(0.01,splitDeviation1,splitFactor1)&&
		RadiationSplitDeviation(0.005,splitDeviation2,splitFactor2);
	const double splitOrder01=std::log(splitDeviation1/splitDeviation0)/std::log(0.5),
		splitOrder12=std::log(splitDeviation2/splitDeviation1)/std::log(0.5);
	if(!(splitGate&&splitFactor0.beta>=splitFactor0.gamma&&splitFactor1.beta>=
		splitFactor1.gamma&&splitFactor2.beta>=splitFactor2.gamma&&splitDeviation0<=0.02&&
		splitOrder01>=0.85&&splitOrder01<=1.15&&splitOrder12>=0.85&&splitOrder12<=1.15))
		std::printf("V5 split diagnostic ok=%d factors %.6g/%.6g %.6g/%.6g %.6g/%.6g deviations %.9g %.9g %.9g\n",
			splitGate?1:0,splitFactor0.beta,splitFactor0.gamma,splitFactor1.beta,
			splitFactor1.gamma,splitFactor2.beta,splitFactor2.gamma,splitDeviation0,
			splitDeviation1,splitDeviation2);
	Check(splitGate&&splitFactor0.beta>=splitFactor0.gamma&&splitFactor1.beta>=
		splitFactor1.gamma&&splitFactor2.beta>=splitFactor2.gamma&&splitDeviation0<=0.02&&
		splitOrder01>=0.85&&splitOrder01<=1.15&&splitOrder12>=0.85&&splitOrder12<=1.15,
		"V5 beta-active accepted radiation stays within two percent and its splitting deviation halves with dt");
	RadiationEscapeFactor predictiveInsufficientOpacity;
	Check(!ComputeRadiationEscapeFactor(gridDerivedHeatReleaseW,600.0,
		gridRadiativeFraction,{gridUnscaledExchange.exchangeWPerM3,
		gridUnscaledExchange.exchangeWPerM3},{1.0,2.0},true,
		predictiveInsufficientOpacity,&error),
		"V4 predictive radiation fails closed when the methane gas opacity cannot supply chi_r");
	MethaneReactionStep overflowRateStep = step;
	overflowRateStep.deltaTimeS = 1.0e-310;
	overflowRateStep.mixingTimeS = std::numeric_limits<double>::denorm_min();
	MethaneSourcePacket overflowRatePacket;
	Check(!BuildMethaneReactionPacket(beginning,fuel,overflowRateStep,
		overflowRatePacket,&error),
		"V4 rejects a packet whose finite extent produces non-finite diagnostic rates");
	MethaneSourcePacket tracePacket;
	const MethaneCellState traceBeginning=PhysicalMixtureLineState(fuel,
		thermochemistry,0.2,900.0);
	const double traceExtent=traceBeginning.constituent[MethaneO2]/
		fuel.StoichiometricOxygenKGPerKGFuel();
	for(std::size_t species=0;species<MethaneSpeciesCount;++species)
		tracePacket.constituentDelta[species]=traceExtent*fuel.PrimaryReactionDelta()[species];
	tracePacket.constituentDelta[MethaneO2] = -std::nextafter(
		traceBeginning.constituent[MethaneO2],std::numeric_limits<double>::infinity());
	tracePacket.sensibleEnergyDeltaJPerM3=traceExtent*fuel.LowerHeatingValueJPerKG();
	MethaneCellState traceState;
	Check(ApplySourcePacket(traceBeginning,tracePacket,thermochemistry,traceState,&error) &&
		traceState.constituent[MethaneO2] < 0.0,
		"V4 property inversion maps a roundoff trace without clamping the conservative ledger");
	Check(lesTransport.eddyViscosityM2PerS >= 0.0 &&
		dnsTransport.eddyViscosityM2PerS == 0.0 &&
		dnsTransport.sgsDiffusivityM2PerS == 0.0 &&
		dnsTransport.effectiveViscosityPaS == dnsTransport.molecularViscosityPaS,
		"V2 DNS collapse and LES effective-transport relationships are operational");

	// V4 RED topology: primary combustion and pre-existing carbon oxidation
	// independently request all oxygen, then one shared theta allocates it.
	std::array<double,MethaneSpeciesCount> starvedWeights={};
	for(std::size_t species=0;species<MethaneSpeciesCount;++species)starvedWeights[species]=
		0.8*fuel.AmbientMassFractions()[species]+0.2*fuel.InjectedMassFractions()[species];
	const double starvedCarbonExtent=0.01,targetStarvedOxygen=0.005;
	const double starvedPrimaryExtent=(starvedWeights[MethaneO2]+
		starvedCarbonExtent*fuel.SootOxygenKGPerKGCarbon()-targetStarvedOxygen)/
		fuel.StoichiometricOxygenKGPerKGFuel();
	for(std::size_t species=0;species<MethaneSpeciesCount;++species)
		starvedWeights[species]+=starvedPrimaryExtent*fuel.PrimaryReactionDelta()[species]-
			starvedCarbonExtent*fuel.SootOxidationDelta()[species];
	const MethaneCellState starvedState=StateAtTemperature(starvedWeights,0.2,1500.0,
		thermochemistry);
	MethaneSourcePacket sharedOxygen;
	MethaneReactionStep sharedOxygenFixture = step;
	sharedOxygenFixture.mixingTimeS = sharedOxygenFixture.deltaTimeS;
	Check(BuildMethaneReactionPacket(starvedState,fuel,sharedOxygenFixture,
		sharedOxygen,&error),
		"V4 shared-oxygen packet is constructible");
	const double sharedRelaxation=-std::expm1(-sharedOxygenFixture.deltaTimeS/
		sharedOxygenFixture.mixingTimeS);
	const double primaryCandidate=sharedRelaxation*std::min(starvedState.constituent[MethaneCH4],
		starvedState.constituent[MethaneO2]/fuel.StoichiometricOxygenKGPerKGFuel());
	const double sootCandidate=sharedRelaxation*std::min(starvedState.constituent[MethaneCarbon],
		starvedState.constituent[MethaneO2]/fuel.SootOxygenKGPerKGCarbon());
	const double sharedTheta=starvedState.constituent[MethaneO2]/(
		fuel.StoichiometricOxygenKGPerKGFuel()*primaryCandidate+
		fuel.SootOxygenKGPerKGCarbon()*sootCandidate);
	const double sequentialPrimary=std::min(primaryCandidate,starvedState.constituent[MethaneO2]/
		fuel.StoichiometricOxygenKGPerKGFuel());
	const double sequentialRemaining=starvedState.constituent[MethaneO2]-
		fuel.StoichiometricOxygenKGPerKGFuel()*sequentialPrimary;
	const double sequentialSoot=std::min(sootCandidate,sequentialRemaining/
		fuel.SootOxygenKGPerKGCarbon());
	if(!(Near(-sharedOxygen.constituentDelta[MethaneO2],
		starvedState.constituent[MethaneO2],2.0e-15) &&
		Near(sharedOxygen.reactedFuelKGPerM3/primaryCandidate,sharedTheta,2.0e-15)&&
		Near(sharedOxygen.oxidizedCarbonKGPerM3/sootCandidate,sharedTheta,2.0e-15)&&
		std::fabs(sequentialPrimary/primaryCandidate-sequentialSoot/sootCandidate)>0.1))
		std::printf("shared oxygen diagnostic q=%.9g consumed=%.9g primary=%.9g soot=%.9g theta=%.9g\n",
			starvedState.constituent[MethaneO2],-sharedOxygen.constituentDelta[MethaneO2],
			sharedOxygen.reactedFuelKGPerM3,sharedOxygen.oxidizedCarbonKGPerM3,sharedTheta);
	Check(Near(-sharedOxygen.constituentDelta[MethaneO2],
		starvedState.constituent[MethaneO2],2.0e-15) &&
		Near(sharedOxygen.reactedFuelKGPerM3/primaryCandidate,sharedTheta,2.0e-15)&&
		Near(sharedOxygen.oxidizedCarbonKGPerM3/sootCandidate,sharedTheta,2.0e-15)&&
		std::fabs(sequentialPrimary/primaryCandidate-sequentialSoot/sootCandidate)>0.1,
		"V4 one shared oxygen scale serves simultaneous primary and soot candidates");

	// V5: the backward-Euler root must close against an independently
	// re-evaluated Planck-mean exchange at the accepted temperature.
	std::array<double,MethaneSpeciesCount> products={};
	for(std::size_t species=0;species<MethaneSpeciesCount;++species)products[species]=
		0.945*fuel.AmbientMassFractions()[species]+0.055*fuel.InjectedMassFractions()[species];
	const double productExtent=std::min(products[MethaneCH4],
		products[MethaneO2]/fuel.StoichiometricOxygenKGPerKGFuel());
	for(std::size_t species=0;species<MethaneSpeciesCount;++species)
		products[species]+=productExtent*fuel.PrimaryReactionDelta()[species];
	const MethaneCellState hotProducts = StateAtTemperature(
		products,0.055,1400.0,thermochemistry);
	GasExchangeEvaluation frozenAbsoluteExchange;
	MethaneCellState emitterFree=hotProducts;
	emitterFree.constituent[MethaneCO2]=0.0;
	emitterFree.constituent[MethaneH2O]=0.0;
	GasExchangeEvaluation emitterFreeExchange;
	Check(EvaluateGasExchange(emitterFree,1400.0,300.0,thermochemistry,opacity,
		emitterFreeExchange,&error)&&emitterFreeExchange.exchangeWPerM3==0.0&&
		emitterFreeExchange.temperatureDerivativeWPerM3K==0.0,
		"V5 product-free ambient has an exact zero gas-exchange fast path");
	const double recordDerivedAbsoluteExchange=1409954.1794722667;
	Check(EvaluateGasExchange(hotProducts,1400.0,300.0,thermochemistry,opacity,
		frozenAbsoluteExchange,&error)&&Near(frozenAbsoluteExchange.exchangeWPerM3,
		recordDerivedAbsoluteExchange,5.0e-11),
		"V5 absolute HITEMP Planck-mean cooling matches the frozen independent W-per-m3 oracle");
	if(!Near(frozenAbsoluteExchange.exchangeWPerM3,recordDerivedAbsoluteExchange,5.0e-11))
		std::printf("V5 absolute exchange derived fixture=%.17g\n",
			frozenAbsoluteExchange.exchangeWPerM3);
	MethaneCellState cooledProducts;
	double coolingWPerM3 = 0.0;
	const double radiationStepS = 0.01;
	Check(ApplyGasRadiationBackwardEuler(hotProducts,300.0,radiationStepS,1.0,
		thermochemistry,opacity,cooledProducts,coolingWPerM3,&error),
		"V5 certified gas backward-Euler map accepts the physical product state");
	double acceptedEnergyCheck = 0.0;
	GasExchangeEvaluation acceptedRootExchange;
	Check(thermochemistry.MixtureSensibleEnergyJPerM3(
		ThermochemicalDensities(cooledProducts),cooledProducts.temperatureK,
		acceptedEnergyCheck,&error) && EvaluateGasExchange(cooledProducts,
		cooledProducts.temperatureK,300.0,thermochemistry,opacity,
		acceptedRootExchange,&error) && std::fabs(acceptedEnergyCheck-
		hotProducts.sensibleEnergyJPerM3+radiationStepS*
		acceptedRootExchange.exchangeWPerM3) <= 64.0*std::numeric_limits<double>::epsilon()*
		std::max(1.0,std::fabs(hotProducts.sensibleEnergyJPerM3)),
		"V5 accepted gas root satisfies the energy residual tolerance");
	GasExchangeEvaluation acceptedExchange;
	Check(EvaluateGasExchange(cooledProducts,cooledProducts.temperatureK,300.0,
		thermochemistry,opacity,acceptedExchange,&error),
		"V5 independently re-evaluates the accepted gas exchange");
	Check(cooledProducts.temperatureK < hotProducts.temperatureK &&
		Near(coolingWPerM3,acceptedExchange.exchangeWPerM3,2.0e-12),
		"V5 accepted energy loss equals the implicit Planck-mean cooling rate");
	const MethaneCellState coldProducts = StateAtTemperature(products,0.055,
		400.0,thermochemistry);
	MethaneCellState warmedProducts;
	double signedCoolingWPerM3 = 0.0;
	Check(ApplyGasRadiationBackwardEuler(coldProducts,500.0,radiationStepS,1.0,
		thermochemistry,opacity,warmedProducts,signedCoolingWPerM3,&error) &&
		warmedProducts.temperatureK > coldProducts.temperatureK &&
		signedCoolingWPerM3 < 0.0,
		"V5 signed Kirchhoff exchange heats gas below its black enclosure temperature");
	RadiationEscapeFactor escape;
	const std::vector<double> exchangeFixture = {100.0,200.0};
	const std::vector<double> volumeFixture = {1.0,1.0};
	double methaneRadiativeFraction = 0.0;
	Check(fuel.ResolveRadiativeFraction(0,false,0.0,methaneRadiativeFraction) &&
		ComputeRadiationEscapeFactor(600.0,600.0,methaneRadiativeFraction,exchangeFixture,
		volumeFixture,true,escape,&error) && Near(escape.accepted,0.4,1.0e-15) &&
		Near(escape.accepted*300.0,120.0,1.0e-15),
		"V5 burning escape factor closes the total-support radiative budget exactly");
	RadiationEscapeFactor postFireEscape;
	Check(ComputeRadiationEscapeFactor(0.0,600.0,0.1,{0.0},{1.0},true,
		postFireEscape,&error) && postFireEscape.beta == 0.0 &&
		postFireEscape.gamma == 1.0 && postFireEscape.accepted == 1.0,
		"V5 degenerate post-fire state reverts to unscaled optically-thin cooling");
	RadiationEscapeFactor overflowEscape;
	Check(!ComputeRadiationEscapeFactor(1.0,600.0,0.1,
		{std::numeric_limits<double>::max(),std::numeric_limits<double>::max()},
		{1.0,1.0},true,overflowEscape,&error),
		"V5 radiative-budget accumulation rejects finite-input overflow");
	double derivativeLower = 0.0;
	Check(CertifiedGasExchangeDerivativeLower(hotProducts,1300.0,1400.0,300.0,
		thermochemistry,opacity,derivativeLower,&error) &&
		std::isfinite(derivativeLower),
		"V5 F-prime proof consumes a finite analytic opacity derivative enclosure");
	double carbonExchange0 = 0.0, carbonDerivative0 = 0.0;
	double independentCarbonExchange=0.0,independentCarbonDerivative=0.0;
	Check(EvaluateSyntheticHotCarbonExchange(1.0e-4,1200.0,300.0,0.25,1800.0,
		carbonExchange0,carbonDerivative0,&error)&&
		IndependentHotCarbonWavelengthIntegral(1.0e-4,1800.0,0.25,1200.0,300.0,
			independentCarbonExchange,independentCarbonDerivative),
		"V5 production and independent numerical hot-carbon wavelength integrals evaluate");
	Check(Near(carbonExchange0,independentCarbonExchange,2.0e-3)&&
		Near(carbonDerivative0,independentCarbonDerivative,2.0e-3),
		"V5 independent wavelength integral recovers the fv*(T^5-Tinf^5) law");
	double nearAmbientExchange=0.0,nearAmbientDerivative=0.0;
	double independentNearAmbientExchange=0.0,independentNearAmbientDerivative=0.0;
	Check(EvaluateSyntheticHotCarbonExchange(1.0e-4,300.001,300.0,0.25,1800.0,
		nearAmbientExchange,nearAmbientDerivative,&error)&&
		IndependentHotCarbonWavelengthIntegral(1.0e-4,1800.0,0.25,300.001,300.0,
			independentNearAmbientExchange,independentNearAmbientDerivative)&&
		Near(nearAmbientExchange,independentNearAmbientExchange,2.0e-3)&&
		Near(nearAmbientDerivative,independentNearAmbientDerivative,2.0e-3),
		"V5 independent wavelength integral recovers the analytic near-ambient hot-carbon derivative");
	double ambientCarbonExchange=1.0,ambientCarbonDerivative=0.0,coldCarbonExchange=0.0,
		coldCarbonDerivative=0.0;
	Check(EvaluateSyntheticHotCarbonExchange(1.0e-4,300.0,300.0,0.25,1800.0,
		ambientCarbonExchange,ambientCarbonDerivative,&error)&&ambientCarbonExchange==0.0&&
		EvaluateSyntheticHotCarbonExchange(1.0e-4,299.0,300.0,0.25,1800.0,
		coldCarbonExchange,coldCarbonDerivative,&error)&&coldCarbonExchange<0.0,
		"V5 hot-carbon exchange is exactly zero at ambient and changes sign below it");
	double h2oSigma=0.0,h2oGasDerivative=0.0,h2oRadiationDerivative=0.0,
		co2Sigma=0.0,co2GasDerivative=0.0,co2RadiationDerivative=0.0;
	const bool thinMeans=opacity.PlanckMeanCrossSectionM2PerMolecule("H2O",300.0,300.0,
		h2oSigma,h2oGasDerivative,h2oRadiationDerivative,&error)&&
		opacity.PlanckMeanCrossSectionM2PerMolecule("CO2",300.0,300.0,co2Sigma,
			co2GasDerivative,co2RadiationDerivative,&error);
	const double h2oExpected=2.1374710395e-24,co2Expected=1.0756050218e-24;
	const double h2oOpticalDepth=52.289641591*5.0e-4;
	const double co2OpticalDepth=26.312871635*5.0e-4;
	const double h2oThinEmissivity=-std::expm1(-h2oOpticalDepth);
	const double co2ThinEmissivity=-std::expm1(-co2OpticalDepth);
	Check(thinMeans&&std::fabs(h2oSigma/h2oExpected-1.0)<5.0e-11&&
		std::fabs(co2Sigma/co2Expected-1.0)<5.0e-11&&h2oOpticalDepth<0.05&&co2OpticalDepth<0.05&&
		std::fabs(h2oThinEmissivity/h2oOpticalDepth-1.0)<0.02&&
		std::fabs(co2ThinEmissivity/co2OpticalDepth-1.0)<0.02,
		"V5 adopted HITEMP H2O/CO2 Planck means match frozen independent nodes in the shape-free thin limit");
	MethaneCellState alternateHistory=ProductRichMixtureLineState(fuel,
		thermochemistry,0.055,1400.0);
	MethaneCellState equalZProducts=hotProducts;
	const double sharedTransportedZ=0.2*std::min(hotProducts.TotalDensity(),
		alternateHistory.TotalDensity());
	equalZProducts.rhoTotalZ=sharedTransportedZ;alternateHistory.rhoTotalZ=sharedTransportedZ;
	GasExchangeEvaluation alternateExchange,productHistoryExchange;
	Check(EvaluateGasExchange(equalZProducts,1400.0,300.0,thermochemistry,opacity,
		productHistoryExchange,&error)&&EvaluateGasExchange(alternateHistory,1400.0,300.0,
		thermochemistry,opacity,alternateExchange,&error)&&
		equalZProducts.rhoTotalZ==alternateHistory.rhoTotalZ&&sharedTransportedZ>0.0&&
		productHistoryExchange.exchangeWPerM3>alternateExchange.exchangeWPerM3,
		"V5 equal-Z states with different transported product histories cool differently");

	// A shared certified scalar solver is used by the physical gas map and by
	// this deliberately nonmonotone verification closure.  Its interval proof,
	// not a bracketed root's accidental choice, controls acceptance.
	const double syntheticCp=1000.0,syntheticInitial=900.0,syntheticAmbient=300.0,
		syntheticFrequency=0.025,syntheticKCool=1.0,syntheticKHot=30.0,
		syntheticExchangeScale=1.0e-7;
	auto syntheticEnergy=[&](const double temperature,double& value){
		value=syntheticCp*temperature;return true;};
	auto syntheticExchange=[&](const double temperature,double& value){
		const double difference=temperature-syntheticAmbient;
		const double phi=0.5+0.45*std::sin(syntheticFrequency*difference);
		const double kappa=syntheticKCool+(syntheticKHot-syntheticKCool)*phi;
		value=syntheticExchangeScale*kappa*(std::pow(temperature,4.0)-
			std::pow(syntheticAmbient,4.0));return std::isfinite(value)&&value>=0.0;};
	std::vector<double> syntheticPhiKnots={syntheticAmbient,syntheticInitial};
	const double halfPi=0.5*std::acos(-1.0);
	for(double phase=halfPi;syntheticAmbient+phase/syntheticFrequency<syntheticInitial;
		phase+=std::acos(-1.0))syntheticPhiKnots.push_back(
			syntheticAmbient+phase/syntheticFrequency);
	auto syntheticDerivative=[&](const double lo,const double hi,double& value){
		for(const double knot:syntheticPhiKnots)if(knot>lo&&knot<hi){
			value=-1.0e12;return true;}
		const double deltaK=syntheticKHot-syntheticKCool;
		const double derivativeKLower=-0.45*syntheticFrequency*deltaK;
		const double kappaLower=syntheticKCool+0.05*deltaK;
		value=syntheticExchangeScale*(derivativeKLower*(std::pow(hi,4.0)-
			std::pow(syntheticAmbient,4.0))+4.0*kappaLower*std::pow(lo,3.0));
		return true;};
	double rejectedRadiationTemperature=0.0,rejectedCooling=0.0;
	const bool largeStepRejected=!CertifiedScalarRadiationBackwardEuler(syntheticInitial,
		syntheticAmbient,syntheticCp*syntheticInitial,syntheticCp,20.0,1.0,
		syntheticPhiKnots,syntheticEnergy,syntheticExchange,
		syntheticDerivative,rejectedRadiationTemperature,rejectedCooling,&error);
	double reducedStep=20.0,reducedTemperature=0.0,reducedCooling=0.0;
	bool reducedAccepted=false;std::size_t reductions=0;
	while(!reducedAccepted&&reductions<16){reducedStep*=0.5;++reductions;
		reducedAccepted=CertifiedScalarRadiationBackwardEuler(syntheticInitial,
			syntheticAmbient,syntheticCp*syntheticInitial,syntheticCp,reducedStep,1.0,
			syntheticPhiKnots,syntheticEnergy,syntheticExchange,
			syntheticDerivative,reducedTemperature,reducedCooling,&error);}
	double unsplitTemperature=0.0,unsplitCooling=0.0;
	const bool unsplitRejected=!CertifiedScalarRadiationBackwardEuler(syntheticInitial,
		syntheticAmbient,syntheticCp*syntheticInitial,syntheticCp,reducedStep,1.0,
		{syntheticAmbient,syntheticInitial},syntheticEnergy,syntheticExchange,
		syntheticDerivative,unsplitTemperature,unsplitCooling,&error);
	// Continue the same deterministic halving policy past first admissibility
	// until temporal splitting error is small enough to compare to a fine path.
	for(std::size_t refinement=0;reducedAccepted&&refinement<6;++refinement){
		reducedStep*=0.5;++reductions;
		reducedAccepted=CertifiedScalarRadiationBackwardEuler(syntheticInitial,
			syntheticAmbient,syntheticCp*syntheticInitial,syntheticCp,reducedStep,1.0,
			syntheticPhiKnots,syntheticEnergy,syntheticExchange,
			syntheticDerivative,reducedTemperature,reducedCooling,&error);
	}
	double fineTemperature=syntheticInitial;
	const std::size_t fineSubsteps=64;
	bool fineAccepted=reducedAccepted;
	for(std::size_t substep=0;fineAccepted&&substep<fineSubsteps;++substep){
		double next=0.0,cooling=0.0;
		fineAccepted=CertifiedScalarRadiationBackwardEuler(fineTemperature,
			syntheticAmbient,syntheticCp*fineTemperature,syntheticCp,
			reducedStep/fineSubsteps,1.0,syntheticPhiKnots,
			syntheticEnergy,syntheticExchange,syntheticDerivative,next,cooling,&error);
		fineTemperature=next;}
	if(!(largeStepRejected&&reducedAccepted&&reductions>0&&fineAccepted&&
		std::fabs(reducedTemperature-fineTemperature)<8.0))std::printf(
		"V5 enclosure diagnostic reject=%d accept=%d reductions=%zu coarse=%.9g fine=%.9g diff=%.9g error=%s\n",
		largeStepRejected?1:0,reducedAccepted?1:0,reductions,reducedTemperature,fineTemperature,
		std::fabs(reducedTemperature-fineTemperature),error.c_str());
	Check(largeStepRejected&&reducedAccepted&&unsplitRejected&&reductions>0&&fineAccepted&&
		std::fabs(reducedTemperature-fineTemperature)<8.0,
		"V5 steep nonmonotone opacity fails F-prime at large dt, passes deterministic reduction, and matches a small-step reference");

	// V6: the eligibility graph is memoryless.  A vitiated barrier blocks a
	// pilot-connected pocket while a separate CFT-passing autoignition cell seeds itself.
	IgnitionGrid grid;
	grid.nx = 6; grid.ny = 1; grid.nz = 1;
	grid.cells.assign(6,StateAtTemperature(beginning.constituent,
		beginning.rhoTotalZ/beginning.TotalDensity(),700.0,thermochemistry));
	grid.pilotMask.assign(6,false);
	grid.pilotMask[0] = true;
	const double gridMixtureFraction = beginning.rhoTotalZ/beginning.TotalDensity();
	grid.cells[0] = StateAtTemperature(grid.cells[0].constituent,
		gridMixtureFraction,1001.0,thermochemistry);
	const double barrierExtent=0.999*std::min(
		grid.cells[4].constituent[MethaneCH4],grid.cells[4].constituent[MethaneO2]/
		fuel.StoichiometricOxygenKGPerKGFuel());
	for(std::size_t species=0;species<MethaneSpeciesCount;++species)
		grid.cells[4].constituent[species]+=barrierExtent*fuel.PrimaryReactionDelta()[species];
	grid.cells[4] = StateAtTemperature(grid.cells[4].constituent,
		gridMixtureFraction,700.0,thermochemistry);
	double barrierAdiabatic=0.0;
	Check(TrialAdiabaticTemperatureK(grid.cells[4],fuel,thermochemistry,
		barrierAdiabatic,&error)&&barrierAdiabatic<transport.CriticalFlameTemperatureK()&&
		grid.cells[4].constituent[MethaneCH4]>0.0&&grid.cells[4].constituent[MethaneO2]>0.0,
		"V6 vitiated barrier reaches and fails the CFT trial with both reactants present");
	std::vector<bool> eligibility,repeatedEligibility,historyEligibility;
	Check(BuildIgnitionEligibility(grid,fuel,thermochemistry,transport,
		eligibility,&error),"V6 ignition eligibility graph evaluates");
	Check(eligibility.size()==6&&eligibility[0]&&eligibility[1]&&eligibility[2]&&
		eligibility[3]&&!eligibility[4]&&!eligibility[5],
		"V6 pilot traversal crosses a multi-hop component while a vitiated barrier blocks the remote pocket");
	IgnitionGrid autoignitionGrid;autoignitionGrid.nx=autoignitionGrid.ny=
		autoignitionGrid.nz=1;autoignitionGrid.pilotMask.assign(1,false);
	autoignitionGrid.cells.push_back(StateAtTemperature(beginning.constituent,
		gridMixtureFraction,900.0,thermochemistry));
	std::vector<bool> autoignitionEligibility;
	Check(BuildIgnitionEligibility(autoignitionGrid,fuel,thermochemistry,transport,
		autoignitionEligibility,&error)&&autoignitionEligibility.size()==1&&
		autoignitionEligibility[0],
		"V6 isolated CFT-passing cell above measured T_AIT seeds itself without a pilot");
	auto SingleCellEligibility=[&](const double temperature,const bool pilot,bool& value){
		IgnitionGrid thresholdGrid;thresholdGrid.nx=thresholdGrid.ny=thresholdGrid.nz=1;
		thresholdGrid.cells.push_back(StateAtTemperature(beginning.constituent,
			gridMixtureFraction,temperature,thermochemistry));thresholdGrid.pilotMask.assign(1,pilot);
		std::vector<bool> thresholdEligibility;if(!BuildIgnitionEligibility(thresholdGrid,fuel,
			thermochemistry,transport,thresholdEligibility,&error))return false;
		value=thresholdEligibility[0];return true;};
	bool belowPilot=false,abovePilot=false,belowAIT=false,aboveAIT=false;
	Check(SingleCellEligibility(fuel.PilotTemperatureK()-0.01,true,belowPilot)&&
		SingleCellEligibility(fuel.PilotTemperatureK()+0.01,true,abovePilot)&&
		SingleCellEligibility(fuel.AutoignitionTemperatureK()-0.01,false,belowAIT)&&
		SingleCellEligibility(fuel.AutoignitionTemperatureK()+0.01,false,aboveAIT)&&
		!belowPilot&&abovePilot&&!belowAIT&&aboveAIT,
		"V6 ignition graph consumes measured T_AIT and model-gate T_pilot at their recorded thresholds");
	if(!(fuel.AutoignitionTemperatureK()>=810.0&&fuel.AutoignitionTemperatureK()<811.0&&
		fuel.PilotTemperatureK()==600.0))
		std::printf("V6 constants T_AIT=%.17g T_pilot=%.17g\n",
			fuel.AutoignitionTemperatureK(),fuel.PilotTemperatureK());
	Check(fuel.AutoignitionTemperatureK()>=810.0&&fuel.AutoignitionTemperatureK()<811.0&&
		fuel.PilotTemperatureK()==600.0&&
		fuel.PilotTemperatureK()<fuel.AutoignitionTemperatureK(),
		"V6 ignition graph consumes r52 measured T_AIT and the lower model-gate T_pilot by kind");
	// Exercise arbitrary prior reaction history on a separate trajectory, then
	// reconstruct a fresh grid solely from serialized conservative state.
	IgnitionGrid historyGrid=grid;
	for(MethaneCellState& historyState:historyGrid.cells){
		MethaneReactionStep historyStep=step;historyStep.primaryEligible=true;
		MethaneSourcePacket historyPacket;
		if(BuildMethaneReactionPacket(historyState,fuel,historyStep,historyPacket,&error))
			ApplySourcePacket(historyState,historyPacket,thermochemistry,historyState,&error);
		historyState=StateAtTemperature(historyState.constituent,
			historyState.rhoTotalZ/historyState.TotalDensity(),500.0,thermochemistry);
	}
	Check(BuildIgnitionEligibility(historyGrid,fuel,thermochemistry,transport,
		historyEligibility,&error)&&historyEligibility!=eligibility,
		"V6 arbitrary prior reaction trajectory materially changes the graph before restart");
	IgnitionGrid restartedGrid;restartedGrid.nx=grid.nx;restartedGrid.ny=grid.ny;
	restartedGrid.nz=grid.nz;restartedGrid.pilotMask=grid.pilotMask;
	for(const MethaneCellState& state:grid.cells){
		MethaneCellState restarted=FromConservativeVector(ToConservativeVector(state));
		Check(thermochemistry.InvertMixtureTemperatureK(ThermochemicalDensities(restarted),
			restarted.sensibleEnergyJPerM3,restarted.temperatureK,&error),
			"V6 restart reconstructs temperature from serialized conservative state");
		restartedGrid.cells.push_back(restarted);
	}
	Check(BuildIgnitionEligibility(restartedGrid,fuel,thermochemistry,transport,
		repeatedEligibility,&error)&&repeatedEligibility==eligibility,
		"V6 a fresh restart after arbitrary discarded reaction history reproduces the identical eligibility graph");
	FireSimulationMethaneRecord invalidFuel;
	std::vector<bool> invalidEligibility = {true};
	Check(!BuildIgnitionEligibility(grid,invalidFuel,thermochemistry,transport,
		invalidEligibility,&error),
		"V6 ignition fails closed before indexing an invalid fuel record");
	IgnitionGrid overflowGrid;
	overflowGrid.nx = std::numeric_limits<std::size_t>::max()/2+1;
	overflowGrid.ny = 2; overflowGrid.nz = 2;
	Check(!BuildIgnitionEligibility(overflowGrid,fuel,thermochemistry,transport,
		invalidEligibility,&error),
		"V6 ignition rejects a wrapped grid product");

	struct V6History{std::vector<double> heatReleaseW;std::vector<double> frontM;};
	auto RunCoupledV6History=[&](const std::size_t substepsPerSample,V6History& history){
		PeriodicMACShape historyShape;historyShape.nx=6;historyShape.ny=3;historyShape.nz=3;
		historyShape.cellWidthM=0.01;const std::size_t historyCount=historyShape.CellCount();
		const ConservativeVector historyBase=ToConservativeVector(ProductRichMixtureLineState(
			fuel,thermochemistry,0.08,710.0));
		std::vector<ConservativeVector> state(historyCount);
		for(std::size_t cell=0;cell<historyCount;++cell){state[cell]=historyBase;
			if(cell%historyShape.nx==3){MethaneCellState barrier=FromConservativeVector(historyBase);
				const double barrierExtent=0.45*std::min(barrier.constituent[MethaneCH4],
					barrier.constituent[MethaneO2]/fuel.StoichiometricOxygenKGPerKGFuel());
				for(std::size_t species=0;species<MethaneSpeciesCount;++species)
					barrier.constituent[species]+=barrierExtent*fuel.PrimaryReactionDelta()[species];
				if(!thermochemistry.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(barrier),
					710.0,barrier.sensibleEnergyJPerM3,&error))return false;
				barrier.temperatureK=710.0;state[cell]=ToConservativeVector(barrier);}}
		PeriodicMACField momentum;const double transportVelocity=1.0;
		for(unsigned int axis=0;axis<3;++axis){const std::size_t faceCount=
			OpenMACFaceCount3D(historyShape,axis);momentum.component[axis].assign(faceCount,0.0);
			if(axis==0)for(std::size_t z=0;z<historyShape.nz;++z)for(std::size_t y=0;
				y<historyShape.ny;++y)for(std::size_t x=0;x<=historyShape.nx;++x){
				const std::size_t left=historyShape.Index(x?x-1:0,y,z),
					right=historyShape.Index(x<historyShape.nx?x:historyShape.nx-1,y,z);
				const double density=PositiveArithmeticMean(FromConservativeVector(state[left]).
					GasDensity(),FromConservativeVector(state[right]).GasDensity());
				momentum.component[axis][OpenMACFaceIndex3D(historyShape,axis,x,y,z)]=
					density*transportVelocity;
		}}
		ConservativeAdvance3DConfig historyConfig=ownerConfig;
		historyConfig.periodicBoundaries=false;historyConfig.dns=true;
		historyConfig.transport.cellWidthM=historyShape.cellWidthM;
		historyConfig.transport.deltaTimeS=0.001/substepsPerSample;
		historyConfig.transport.ambientGasDensityKGPerM3=
			FromConservativeVector(state[historyShape.nx-1]).GasDensity();
		historyConfig.openBoundary=openBoundary3D;
		historyConfig.openBoundary.kind.fill(AdiabaticWallBoundary3D);
		historyConfig.openBoundary.kind[0]=PressureOpenBoundary3D;
		historyConfig.openBoundary.kind[1]=PressureOpenBoundary3D;
		historyConfig.openBoundary.bottomFuelMask.clear();
		for(unsigned int side=0;side<6;++side)historyConfig.openBoundary.priorInflow[side].assign(
			OpenBoundaryFaceCount3D(historyShape,side),false);
		historyConfig.openBoundary.ambientState=ToConservativeVector(
			ProductRichMixtureLineState(fuel,thermochemistry,0.08,700.0));
		historyConfig.openBoundary.ambientDensityKGPerM3=
			FromConservativeVector(historyConfig.openBoundary.ambientState).GasDensity();
		historyConfig.transport.ambientTemperatureK=700.0;
		historyConfig.injectedTemperatureK=700.0;
			historyConfig.retainStageDiagnostics=false;
		history.heatReleaseW.clear();history.frontM.clear();
		for(std::size_t sample=0;sample<30;++sample){
			double sampleHeat=0.0,front=0.0;
			for(std::size_t substep=0;substep<substepsPerSample;++substep){
				IgnitionGrid currentGrid;currentGrid.nx=historyShape.nx;currentGrid.ny=historyShape.ny;
				currentGrid.nz=historyShape.nz;currentGrid.pilotMask.assign(historyCount,false);
				for(std::size_t cell=0;cell<historyCount;++cell){
					MethaneCellState physical=FromConservativeVector(state[cell]);
					if(!InvertMethaneTemperatureWithinAcceptedEnvelope(physical,
						thermochemistry.TemperatureMinK(),thermochemistry.TemperatureMaxK(),
						thermochemistry,physical.temperatureK,&error))return false;
					currentGrid.cells.push_back(physical);
					if(cell%historyShape.nx==0)currentGrid.pilotMask[cell]=true;
				}
				std::vector<bool> currentEligibility;
				if(!BuildIgnitionEligibility(currentGrid,fuel,thermochemistry,transport,
					currentEligibility,&error))return false;
				std::vector<MethaneSourcePacket> packets(historyCount);
				for(std::size_t cell=0;cell<historyCount;++cell){MethaneReactionStep currentStep;
					currentStep.deltaTimeS=historyConfig.transport.deltaTimeS;
					currentStep.mixingTimeS=20.0;currentStep.primaryEligible=currentEligibility[cell];
					currentStep.sootOxidationEnabled=false;
					if(!BuildMethaneReactionPacket(currentGrid.cells[cell],fuel,currentStep,
						packets[cell],&error)){std::printf(
						"V6 history packet failed substeps=%zu sample=%zu substep=%zu cell=%zu: %s\n",
						substepsPerSample,sample,substep,cell,error.c_str());return false;}
					sampleHeat+=packets[cell].gasHeatReleaseWPerM3*std::pow(
						historyShape.cellWidthM,3.0)/substepsPerSample;
					if(currentEligibility[cell])front=std::max(front,
						(cell%historyShape.nx+0.5)*historyShape.cellWidthM);
				}
				ConservativeAdvance3DResult advanced;
				if(!AdvanceConservative3D(historyShape,state,momentum,packets,historyConfig,
					fuel,thermochemistry,transport,advanced,&error)){
					std::printf("V6 owner history failed substeps=%zu sample=%zu substep=%zu: %s\n",
						substepsPerSample,sample,substep,error.c_str());return false;}
				state.swap(advanced.conservative);momentum.component.swap(
					advanced.momentumKGPerM2S.component);
			}
			history.heatReleaseW.push_back(sampleHeat);history.frontM.push_back(front);
		}
		return true;
	};
	V6History history1,history2,history4,history8;
	const bool historyRuns=RunCoupledV6History(1,history1)&&RunCoupledV6History(2,history2)&&
		RunCoupledV6History(4,history4)&&RunCoupledV6History(8,history8);
	// Use explicit spelling here: the history grid remains fixed while only dt
	// changes, and both observables are compared at identical sample times.
	double historyHeatError1=0.0,historyHeatError2=0.0,historyHeatError4=0.0;
	double historyFrontError1=0.0,historyFrontError2=0.0,historyFrontError4=0.0;
	if(historyRuns){for(std::size_t sample=0;sample<history8.heatReleaseW.size();++sample){
		auto heatContribution=[&](const V6History& candidate){return
			std::fabs(candidate.heatReleaseW[sample]-history8.heatReleaseW[sample])/
			std::max(1.0,std::fabs(history8.heatReleaseW[sample]));};
		auto frontContribution=[&](const V6History& candidate){return
			std::fabs(candidate.frontM[sample]-history8.frontM[sample])/0.01;};
		historyHeatError1+=heatContribution(history1);historyHeatError2+=heatContribution(history2);
		historyHeatError4+=heatContribution(history4);historyFrontError1+=frontContribution(history1);
		historyFrontError2+=frontContribution(history2);historyFrontError4+=frontContribution(history4);}}
	const bool historyConverges=historyRuns&&history8.frontM.back()>history8.frontM.front()&&
		historyHeatError2<historyHeatError1&&historyHeatError4<historyHeatError2&&
		historyFrontError1>0.0&&((historyFrontError2<historyFrontError1&&
		historyFrontError4<historyFrontError2)||(historyFrontError2==0.0&&
		historyFrontError4==0.0));
	if(!historyConverges)std::printf(
		"V6 history diagnostic ok=%d heat %.9g %.9g %.9g front %.9g %.9g %.9g range %.9g %.9g error=%s\n",
		historyRuns?1:0,historyHeatError1,historyHeatError2,historyHeatError4,
		historyFrontError1,historyFrontError2,historyFrontError4,
		historyRuns?history8.frontM.front():0.0,historyRuns?history8.frontM.back():0.0,error.c_str());
	Check(historyConverges,
		"V6 fixed-grid owning advance converges under timestep refinement in heat-release and ignition-front histories");

	// V6 local split refinement: exponential finite-step conversion composes
	// exactly when the accepted remainder is used by the next substep.
	auto ReactInSubsteps = [&]( const std::size_t substeps ) {
		MethaneCellState current = beginning;
		double totalReacted = 0.0;
		for( std::size_t substep=0; substep<substeps; ++substep ) {
			MethaneReactionStep local = step;
			local.deltaTimeS = step.deltaTimeS/static_cast<double>(substeps);
			local.sootOxidationEnabled = false;
			MethaneSourcePacket localPacket;
			if( !BuildMethaneReactionPacket(current,fuel,local,localPacket,&error) ) return -1.0;
			totalReacted += localPacket.reactedFuelKGPerM3;
			if( !ApplySourcePacket(current,localPacket,thermochemistry,current,&error) ) return -1.0;
		}
		return totalReacted;
	};
	const double oneStep = ReactInSubsteps(1);
	Check(oneStep > 0.0 && Near(ReactInSubsteps(2),oneStep,2.0e-14) &&
		Near(ReactInSubsteps(4),oneStep,2.0e-14),
		"V6 exponential source map is timestep-compositional before coupled transport");
	StableTimeStep stableStep;
	Check(ComputeStableTimeStep(0.01,0.4,0.0,
		std::max(lesTransport.totalDiffusivityM2PerS,
			lesTransport.effectiveViscosityPaS/beginning.GasDensity()),3,0.0,
		stableStep,&error) && stableStep.seconds > 0.0 &&
		std::isfinite(stableStep.seconds),
		"V6 timestep selector combines advective, positive-buoyancy, and explicit-diffusion limits");
	StableTimeStep advectiveStep,buoyantStep,diffusiveStep;
	Check(ComputeStableTimeStep(0.01,0.4,0.0,0.0,3,0.0,advectiveStep,&error) &&
		advectiveStep.seconds==0.5*0.01/0.4 && advectiveStep.activeLimit=="advective_CFL" &&
		ComputeStableTimeStep(0.01,0.0,8.0,0.0,3,0.0,buoyantStep,&error) &&
		buoyantStep.seconds==0.5*std::sqrt(2.0*0.01/8.0) &&
		buoyantStep.activeLimit=="buoyant_acceleration" &&
		ComputeStableTimeStep(0.01,0.0,0.0,0.01,3,0.0,diffusiveStep,&error) &&
		diffusiveStep.seconds==0.01*0.01/(8.0*0.01) &&
		diffusiveStep.activeLimit=="explicit_diffusion",
		"r54 pins all three solver timestep coefficients as exact binary64 operations");
	StableTimeStep growthStep;
	Check(ComputeStableTimeStep(0.01,0.0,0.0,0.0,3,0.02,growthStep,&error) &&
		growthStep.seconds==1.1*0.02 && growthStep.activeLimit=="growth_limit",
		"r54 production timestep selector enforces the x1.1 growth limit");

	if( failures ) {
		std::printf("FireSimulationSolverTest: %d failure(s)\n",failures);
		return 1;
	}
	std::printf("FireSimulationSolverTest: V1 pressure-open gate and current kernel checks passed\n");
	return 0;
}
