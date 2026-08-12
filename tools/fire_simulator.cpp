//////////////////////////////////////////////////////////////////////
//
//  fire_simulator.cpp - Standalone Phase-C methane solver bring-up
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "fire_simulator_core.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::FireSim;

namespace
{
	void Usage()
	{
		std::printf("usage: fire_simulator --verify-records | --methane-bringup\n");
	}

	bool UniformState(
		const FireSimulationMethaneRecord& fuel,
		MethaneCellState& result,
		std::string& error
		)
	{
		result = MethaneCellState();
		result.temperatureK = 800.0;
		static const char* names[MethaneCarbon] = {
			"CH4", "O2", "N2", "CO2", "H2O", "CO"
		};
		double inverseMeanWeightSum = 0.0;
		for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
			result.constituent[species] = 0.8*fuel.AmbientMassFractions()[species]+
				0.2*fuel.InjectedMassFractions()[species];
			if( species < MethaneCarbon ) {
				const FireThermochemistrySpecies* property = fuel.FindSpecies(names[species]);
				if( !property ) return false;
				inverseMeanWeightSum += result.constituent[species]/
					property->molecularWeightKGPerKMol;
			}
		}
		const double scale = fuel.ThermodynamicPressurePa()/
			(8314.46261815324*result.temperatureK*inverseMeanWeightSum);
		for( double& density : result.constituent ) density *= scale;
		result.rhoTotalZ = 0.2*result.TotalDensity();
		return fuel.MixtureSensibleEnergyJPerM3(
			ThermochemicalDensities(result),result.temperatureK,
			result.sensibleEnergyJPerM3,&error);
	}
}

int main( const int argc, const char* const argv[] )
{
	if( argc != 2 ) { Usage(); return 2; }
	const FireSimulationMethaneRecord& fuel = FireSimulationMethaneRecord::PhysicalV1();
	const FireSimulationTransportRecord& transport = FireSimulationTransportRecord::OpenV1();
	const FireSimulationGasOpacityRecord& opacity =
		FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1();
	if( !fuel.IsValid() || !transport.IsValid() ||
		!opacity.IsValid() ) {
		std::fprintf(stderr,"fire_simulator: an adopted Phase-C record failed closed\n");
		return 1;
	}
	if( std::strcmp(argv[1],"--verify-records") == 0 ) {
		std::printf("methane=%s\ntransport=%s\ngas_opacity=%s\n",
			fuel.RecordId().c_str(),
			transport.RecordId().c_str(),opacity.RecordId().c_str());
		std::printf("predictive_qualified=%s blockers=%zu\n",
			fuel.IsPredictiveQualified() ? "true" : "false",
			fuel.PredictiveBlockers().size());
		return 0;
	}
	if( std::strcmp(argv[1],"--methane-bringup") != 0 ) { Usage(); return 2; }

	std::string error;
	MethaneCellState state;
	if( !UniformState(fuel,state,error) ) {
		std::fprintf(stderr,"fire_simulator: %s\n",error.c_str()); return 1;
	}
	const std::size_t count = 16;
	std::vector<ConservativeVector> conservative(count,ToConservativeVector(state));
	std::vector<double> momentum(count,state.GasDensity()*0.25);
	std::vector<ConservativeVector> source(count);
	PeriodicTransportConfig config;
	config.cellWidthM = 1.0/static_cast<double>(count);
	config.deltaTimeS = 0.01;
	config.ambientTemperatureK = 300.0;
	config.adiabaticTemperatureK = 2500.0;
	PeriodicProjectedHeunResult advanced;
	if( !AdvancePeriodicProjectedHeun(conservative,momentum,source,config,
		1.0e-11,false,fuel,fuel,transport,advanced,&error) ) {
		std::fprintf(stderr,"fire_simulator: %s\n",error.c_str()); return 1;
	}
	double maximumVelocityError = 0.0;
	for( const double velocity : advanced.velocityMPerS ) {
		maximumVelocityError = std::max(maximumVelocityError,std::fabs(velocity-0.25));
	}
	MethaneReactionStep sourceStep;
	sourceStep.deltaTimeS = 0.002;
	sourceStep.mixingTimeS = 0.02;
	sourceStep.primaryEligible = true;
	sourceStep.sootOxidationEnabled = true;
	MethaneSourcePacket reactionProbe;
	MethaneCellState postReactionProbe;
	GasExchangeEvaluation exchangeProbe;
	if( !BuildMethaneReactionPacket(state,fuel,sourceStep,reactionProbe,&error) ||
		!ApplySourcePacket(state,reactionProbe,fuel,postReactionProbe,&error) ||
		!EvaluateGasExchange(postReactionProbe,postReactionProbe.temperatureK,300.0,
			fuel,opacity,exchangeProbe,&error) ) {
		std::fprintf(stderr,"fire_simulator: %s\n",error.c_str()); return 1;
	}
	double radiativeFraction = 0.0;
	if( !fuel.ResolveRadiativeFraction("methane-bringup-v1",false,0.0,
		radiativeFraction,&error) ) {
		std::fprintf(stderr,"fire_simulator: %s\n",error.c_str()); return 1;
	}
	const double cellVolumeM3 = 0.001;
	const double heatReleaseW = exchangeProbe.exchangeWPerM3*cellVolumeM3/
		(2.0*radiativeFraction);
	std::vector<MethaneSourcePacket> frozenPacket;
	RadiationEscapeFactor escape;
	if( !BuildFrozenMethaneSourcePackets({state},{sourceStep},{cellVolumeM3},300.0,
		heatReleaseW,heatReleaseW,radiativeFraction,true,fuel,fuel,opacity,
		frozenPacket,escape,&error) ) {
		std::fprintf(stderr,"fire_simulator: %s\n",error.c_str()); return 1;
	}
	ConservativeVector sourceRate;
	sourceRate[0] = 0.0;
	for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
		sourceRate[1+species] = frozenPacket[0].constituentDelta[species]/sourceStep.deltaTimeS;
	}
	sourceRate[MethaneMassStateDimension] = frozenPacket[0].sensibleEnergyDeltaJPerM3/
		sourceStep.deltaTimeS;
	double expansionPerS = 0.0;
	if( !DivergenceFromDiscreteRate(ToConservativeVector(state),sourceRate,
		state.temperatureK,fuel,expansionPerS,&error) ) {
		std::fprintf(stderr,"fire_simulator: %s\n",error.c_str()); return 1;
	}
	const std::size_t openCells = 8;
	OpenMACProjection1DResult openExpansion;
	if( !ProjectPressureOpenMACVelocity1D(std::vector<double>(openCells,state.GasDensity()),
		std::vector<double>(openCells+1,0.0),std::vector<double>(openCells,expansionPerS),
		state.GasDensity(),0.01,sourceStep.deltaTimeS,1.0e-8,
		state.GasDensity()*1.0e-8,false,false,openExpansion,&error) ) {
		std::fprintf(stderr,"fire_simulator: %s\n",error.c_str()); return 1;
	}
	std::printf("record=%s cells=%zu r0=%zu r1=%zu r2=%zu max_free_stream_error=%.17g expansion=%.17g escape=%.17g open_head_residual=%.17g\n",
		fuel.RecordId().c_str(),count,advanced.r0.picardResidualPerS.size(),
		advanced.r1.picardResidualPerS.size(),advanced.r2.picardResidualPerS.size(),
		maximumVelocityError,expansionPerS,escape.accepted,
		openExpansion.maximumBoundaryHeadResidualPa);
	return maximumVelocityError <= 2.0e-14 && expansionPerS > 0.0 ? 0 : 1;
}
