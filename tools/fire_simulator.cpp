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

	bool MixtureLineState(
		const FireSimulationMethaneRecord& fuel,
		const double mixtureFraction,
		const double temperatureK,
		MethaneCellState& result,
		std::string& error
		)
	{
		result = MethaneCellState();
		result.temperatureK = temperatureK;
		static const char* names[MethaneCarbon] = {
			"CH4", "O2", "N2", "CO2", "H2O", "CO"
		};
		double inverseMeanWeightSum = 0.0;
		for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
			result.constituent[species] = (1.0-mixtureFraction)*
				fuel.AmbientMassFractions()[species]+mixtureFraction*
				fuel.InjectedMassFractions()[species];
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
		result.rhoTotalZ = mixtureFraction*result.TotalDensity();
		return fuel.MixtureSensibleEnergyJPerM3(
			ThermochemicalDensities(result),result.temperatureK,
			result.sensibleEnergyJPerM3,&error);
	}

	bool UniformState(
		const FireSimulationMethaneRecord& fuel,
		MethaneCellState& result,
		std::string& error
		)
	{
		return MixtureLineState(fuel,0.2,800.0,result,error);
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
	PeriodicMACShape ownerShape;
	ownerShape.nx=4; ownerShape.ny=4; ownerShape.nz=4; ownerShape.cellWidthM=0.025;
	const std::size_t count = ownerShape.CellCount();
	std::vector<ConservativeVector> conservative(count,ToConservativeVector(state));
	PeriodicMACField momentum;
	for( unsigned int axis=0; axis<3; ++axis ) momentum.component[axis].assign(count,
		state.GasDensity()*(axis==0 ? 0.25 : 0.0));
	ConservativeAdvance3DConfig config;
	config.transport.cellWidthM = ownerShape.cellWidthM;
	config.transport.deltaTimeS = 0.001;
	config.transport.ambientTemperatureK = 300.0;
	config.transport.adiabaticTemperatureK = 2500.0;
	config.transport.ambientGasDensityKGPerM3=state.GasDensity();
	config.projectionTolerancePerS=2.0e-10;
	config.dns=true;
	ConservativeAdvance3DResult advanced;
	if( !AdvanceConservative3D(ownerShape,conservative,momentum,
		std::vector<MethaneSourcePacket>(count),config,fuel,fuel,transport,
		advanced,&error) ) {
		std::fprintf(stderr,"fire_simulator: %s\n",error.c_str()); return 1;
	}
	double maximumVelocityError = 0.0;
	for( const double velocity : advanced.velocityMPerS.component[0] ) {
		maximumVelocityError = std::max(maximumVelocityError,std::fabs(velocity-0.25));
	}
	MethaneReactionStep sourceStep;
	sourceStep.deltaTimeS = 0.0001;
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
	std::vector<MethaneSourcePacket> frozenPacket;
	RadiationEscapeFactor escape;
	if( !BuildFrozenMethaneSourcePackets({state},{sourceStep},{cellVolumeM3},300.0,
		reactionProbe.gasHeatReleaseWPerM3*cellVolumeM3,radiativeFraction,
		fuel.IsPredictiveQualified(),fuel,fuel,opacity,
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
	PeriodicMACShape openShape;
	openShape.nx=8; openShape.ny=8; openShape.nz=8; openShape.cellWidthM=0.01;
	MethaneCellState ambientState,injectedState;
	if( !MixtureLineState(fuel,0.0,300.0,ambientState,error) ||
		!MixtureLineState(fuel,1.0,300.0,injectedState,error) ) {
		std::fprintf(stderr,"fire_simulator: %s\n",error.c_str()); return 1;
	}
	OpenBoundaryConfig3D openBoundary;
	openBoundary.ambientState=ToConservativeVector(ambientState);
	openBoundary.injectedState=ToConservativeVector(injectedState);
	openBoundary.ambientDensityKGPerM3=ambientState.GasDensity();
	openBoundary.injectedGasDensityKGPerM3=injectedState.GasDensity();
	openBoundary.velocityToleranceMPerS=1.0e-8;
	openBoundary.pressureTolerancePa=ambientState.GasDensity()*1.0e-8;
	PeriodicMACField openMomentum;
	for( unsigned int axis=0; axis<3; ++axis ) openMomentum.component[axis].assign(
		OpenMACFaceCount3D(openShape,axis),0.0);
	ConservativeAdvance3DConfig openAdvanceConfig;
	openAdvanceConfig.periodicBoundaries=false;
	openAdvanceConfig.openBoundary=openBoundary;
	openAdvanceConfig.transport.cellWidthM=openShape.cellWidthM;
	openAdvanceConfig.transport.deltaTimeS=sourceStep.deltaTimeS;
	openAdvanceConfig.transport.ambientTemperatureK=300.0;
	openAdvanceConfig.transport.adiabaticTemperatureK=2500.0;
	openAdvanceConfig.transport.ambientGasDensityKGPerM3=ambientState.GasDensity();
	openAdvanceConfig.injectedTemperatureK=300.0;
	openAdvanceConfig.projectionTolerancePerS=1.0e-8;
	openAdvanceConfig.dns=true;
	ConservativeAdvance3DResult openExpansion;
	if( !AdvanceConservative3D(openShape,
		std::vector<ConservativeVector>(openShape.CellCount(),ToConservativeVector(state)),
		openMomentum,std::vector<MethaneSourcePacket>(openShape.CellCount(),frozenPacket[0]),
		openAdvanceConfig,fuel,fuel,transport,openExpansion,&error) ) {
		std::fprintf(stderr,"fire_simulator: %s\n",error.c_str()); return 1;
	}
	std::printf("record=%s cells=%zu r0=%zu r1=%zu r2=%zu max_free_stream_error=%.17g expansion=%.17g escape=%.17g open_head_residual=%.17g\n",
		fuel.RecordId().c_str(),count,advanced.r0.picardResidualPerS.size(),
		advanced.r1.picardResidualPerS.size(),advanced.r2.picardResidualPerS.size(),
		maximumVelocityError,expansionPerS,escape.accepted,
		openExpansion.maximumBoundaryHeadResidualPa);
	return maximumVelocityError <= 2.0e-14 && expansionPerS > 0.0 ? 0 : 1;
}
