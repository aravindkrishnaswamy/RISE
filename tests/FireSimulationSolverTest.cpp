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
		openShape3D.cellWidthM,fuel,thermochemistry,ambientBackflowFlux,&error);
	for( std::size_t component=0; component<MethaneConservativeDimension; ++component ) {
		exactAmbientDonor=exactAmbientDonor && Near(ambientBackflowFlux.totalOutwardFlux[component],
			-0.1*openBoundary3D.ambientState[component],2.0e-15);
	}
	Check(exactAmbientDonor && ambientBackflowFlux.totalOutwardFlux[1+MethaneCarbon]==0.0,
		"V1 reversing pressure-open face immediately replaces hot fuel/aerosol with complete ambient state");
	OpenBoundaryFlux3D diffusiveAmbientBackflow;
	bool ambientDiffusionClosed=BuildOpenBoundaryFlux3D(ToConservativeVector(hotOutflow),
		800.0,-0.1,PressureOpenBoundary3D,true,openBoundary3D,300.0,300.0,0.01,0.1,
		openShape3D.cellWidthM,fuel,thermochemistry,diffusiveAmbientBackflow,&error);
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
		openShape3D.cellWidthM,fuel,thermochemistry,outflowFlux,&error) &&
		outflowFlux.nonadvectiveEnergyOutwardFlux==0.0 &&
		std::all_of(outflowFlux.nonadvectiveMassOutwardFlux.begin(),
			outflowFlux.nonadvectiveMassOutwardFlux.end(),[]( const double value ){ return value==0.0; }),
		"V1 pressure-open outflow suppresses every inward diffusive/conductive flux");
	bool exactFuelFlux=BuildOpenBoundaryFlux3D(ToConservativeVector(ambientState3D),300.0,0.0,
		FuelInletBoundary3D,true,openBoundary3D,300.0,300.0,0.0,0.0,
		openShape3D.cellWidthM,fuel,thermochemistry,fuelFlux,&error);
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
		openBoundary3D,300.0,300.0,0.0,0.0,openShape3D.cellWidthM,fuel,
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
		openBoundary3D,300.0,300.0,0.0,0.0,openShape3D.cellWidthM,fuel,
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
	Check(!ProjectPeriodicMACVelocity(std::vector<double>(3,
		std::numeric_limits<double>::max()),std::vector<double>(3,0.0),
		std::vector<double>(3,0.0),1.0,0.01,1.0e-8,overflowProjection,&error),
		"V2 rejects finite cell densities whose staggered arithmetic mean overflows");
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
	// The same unequal-cp diffusion ledger is gauge invariant.  Re-form the
	// sensible enthalpy and J_h ledgers at two distinct reference temperatures;
	// at uniform T each must close exactly after the projected species update.
	bool twoReferenceIsothermal=true;
	for(const double gaugeReferenceK:std::array<double,2>{{300.0,650.0}}){
		std::array<double,MethaneSpeciesCount> gaugeEnthalpy={};
		static const char* gaugeNames[MethaneSpeciesCount]={
			"CH4","O2","N2","CO2","H2O","CO","C(gr)"};
		for(std::size_t species=0;species<MethaneSpeciesCount;++species){
			double atTemperature=0.0,atReference=0.0;
			twoReferenceIsothermal=twoReferenceIsothermal &&
				thermochemistry.SensibleEnthalpyJPerKG(gaugeNames[species],800.0,
					atTemperature,&error) &&
				thermochemistry.SensibleEnthalpyJPerKG(gaugeNames[species],gaugeReferenceK,
					atReference,&error);
			gaugeEnthalpy[species]=atTemperature-atReference;
		}
		for(std::size_t cell=0;twoReferenceIsothermal && cell<transportCells;++cell){
			const std::size_t previous=(cell+transportCells-1)%transportCells;
			double energyBefore=0.0,energyAfter=0.0;
			for(std::size_t species=0;species<MethaneSpeciesCount;++species){
				const double densityBefore=transportBeginning[cell][1+species];
				const double densityAfter=densityBefore+0.002*transportCells*(
					physicalFlux.nonadvectiveMass[previous][1+species]-
					physicalFlux.nonadvectiveMass[cell][1+species]);
				energyBefore+=gaugeEnthalpy[species]*densityBefore;
				energyAfter+=gaugeEnthalpy[species]*densityAfter;
			}
			double incomingJh=0.0,outgoingJh=0.0;
			for(std::size_t species=0;species<MethaneSpeciesCount;++species){
				incomingJh+=gaugeEnthalpy[species]*
					physicalFlux.nonadvectiveMass[previous][1+species];
				outgoingJh+=gaugeEnthalpy[species]*
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
	std::vector<double> limiterAlpha;
	PeriodicTransportConfig limiterConfig = transportConfig;
	limiterConfig.cellWidthM = 1.0;
	limiterConfig.deltaTimeS = 1.0;
	Check(ApplyPeriodicSharedFCT(limiterState,limiterFlux,limiterSource,
		limiterConfig,fuel,thermochemistry,limiterResult,limiterAlpha,&error),
		"V3 limiter-active manufactured correction is admissible");
	const double expectedLimiterAlpha = limiterState[0][0]/
		(0.2*(pureFuel[0]-pureAir[0]));
	Check(limiterAlpha.size() == 8 && limiterAlpha[0] > 0.0 &&
		limiterAlpha[0] < 1.0 &&
		Near(limiterAlpha[0],expectedLimiterAlpha,2.0e-14),
		"V3 one nodal Z budget supplies one shared nontrivial face alpha");
	if( limiterResult.size() == limiterState.size() ) {
		for( std::size_t component=0; component<MethaneConservativeDimension; ++component ) {
			double before = 0.0, after = 0.0;
			for( std::size_t cell=0; cell<limiterState.size(); ++cell ) {
				before += limiterState[cell][component];
				after += limiterResult[cell][component];
			}
			Check(Near(after,before,2.0e-15),
				"V3 limiter uses the same signed face correction in both cells");
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
		!ownerResult.r2.picardResidualPerS.empty(),
		"V2/V3 single 3-D owner preserves a physical uniform free stream through R0/R1/R2");
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
		std::printf("V2/V3 open owner rest errors state=%.9g velocity=%.9g\n",stateError,velocityError);
	}
	Check(openOwnerOK && openOwnerRest,
		"V2/V3 single 3-D owner reaches the accepted V1 pressure-open rest infrastructure");

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
	manufacturedReactionStep.deltaTimeS=ownerConfig.transport.deltaTimeS;
	manufacturedReactionStep.mixingTimeS=2.0e4;
	manufacturedReactionStep.primaryEligible=true;
	manufacturedReactionStep.sootOxidationEnabled=true;
	const MethaneCellState manufacturedCell=PhysicalMixtureLineState(
		fuel,thermochemistry,0.2,800.0);
	std::vector<MethaneCellState> manufacturedPhysical(ownerCount,manufacturedCell);
	std::vector<ConservativeVector> manufacturedBeginning(ownerCount,
		ToConservativeVector(manufacturedCell));
	ConservativeAdvance3DConfig manufacturedConfig=ownerConfig;
	manufacturedConfig.transport.ambientGasDensityKGPerM3=ownerPhysical.GasDensity();
	std::vector<MethaneSourcePacket> manufacturedPacket(ownerCount);
	std::vector<double> rawManufacturedTarget(ownerCount,0.0),
		energyExpansionCoefficient(ownerCount,0.0);
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
		ConservativeVector rate;
		for(std::size_t species=0;species<MethaneSpeciesCount;++species)
			rate[1+species]=manufacturedPacket[cell].constituentDelta[species]/
				ownerConfig.transport.deltaTimeS;
		rate[MethaneMassStateDimension]=manufacturedPacket[cell].sensibleEnergyDeltaJPerM3/
			ownerConfig.transport.deltaTimeS;
		Check(DivergenceFromDiscreteRate(manufacturedBeginning[cell],rate,800.0,thermochemistry,
			rawManufacturedTarget[cell],&error),"V2 manufactured packet divergence is evaluable");
		ConservativeVector unitEnergyRate;
		unitEnergyRate[MethaneMassStateDimension]=1.0;
		Check(DivergenceFromDiscreteRate(manufacturedBeginning[cell],unitEnergyRate,800.0,
			thermochemistry,energyExpansionCoefficient[cell],&error) &&
			energyExpansionCoefficient[cell]>0.0,
			"V2 manufactured cooling coefficient follows the full divergence identity");
	}
	double desiredSum=0.0;
	for(std::size_t cell=0;cell<ownerCount;++cell){
		const std::size_t x=cell%ownerShape.nx;
		double desired=1.0e-5*std::sin(2.0*pi*(x+0.5)/ownerShape.nx);
		if(cell+1==ownerCount) desired=-desiredSum; else desiredSum+=desired;
		manufacturedPacket[cell].sensibleEnergyDeltaJPerM3+=(desired-
			rawManufacturedTarget[cell])/energyExpansionCoefficient[cell]*
			ownerConfig.transport.deltaTimeS;
	}
	// One-cell residual cancellation makes the periodic compatibility identity
	// exact in the same fp64 evaluation used by the production target builder.
	for(unsigned int correction=0;correction<3;++correction){
		double sum=0.0;
		for(std::size_t cell=0;cell<ownerCount;++cell){
			ConservativeVector rate;
			for(std::size_t species=0;species<MethaneSpeciesCount;++species)
				rate[1+species]=manufacturedPacket[cell].constituentDelta[species]/
					ownerConfig.transport.deltaTimeS;
			rate[MethaneMassStateDimension]=manufacturedPacket[cell].sensibleEnergyDeltaJPerM3/
				ownerConfig.transport.deltaTimeS;
			double evaluated=0.0;
			DivergenceFromDiscreteRate(manufacturedBeginning[cell],rate,800.0,thermochemistry,
				evaluated,&error);
			sum+=evaluated;
		}
		manufacturedPacket.back().sensibleEnergyDeltaJPerM3-=sum/
			energyExpansionCoefficient.back()*ownerConfig.transport.deltaTimeS;
	}
	PeriodicMACField manufacturedMomentum;
	for(unsigned int axis=0;axis<3;++axis) manufacturedMomentum.component[axis].assign(
		ownerCount,0.0);
	ConservativeAdvance3DResult manufacturedResult;
	const bool manufacturedOK=AdvanceConservative3D(ownerShape,manufacturedBeginning,
		manufacturedMomentum,manufacturedPacket,manufacturedConfig,fuel,thermochemistry,
		transport,manufacturedResult,&error);
	if(!manufacturedOK) std::printf("V2 reacting owner diagnostic: %s\n",error.c_str());
	double maximumTargetMismatch=0.0,maximumProjectionMismatch=0.0;
	std::array<double,MethaneConservativeDimension> manufacturedBefore={},
		manufacturedAfter={},manufacturedSource={};
	for(std::size_t cell=0;manufacturedOK && cell<ownerCount;++cell){
		ConservativeVector rate;
		for(std::size_t species=0;species<MethaneSpeciesCount;++species)
			rate[1+species]=manufacturedPacket[cell].constituentDelta[species]/
				ownerConfig.transport.deltaTimeS;
		rate[MethaneMassStateDimension]=manufacturedPacket[cell].sensibleEnergyDeltaJPerM3/
			ownerConfig.transport.deltaTimeS;
		double exactTarget=0.0;
		DivergenceFromDiscreteRate(manufacturedBeginning[cell],rate,800.0,thermochemistry,
			exactTarget,&error);
		maximumTargetMismatch=std::max(maximumTargetMismatch,std::fabs(
			manufacturedResult.r0.divergenceTargetPerS[cell]-exactTarget));
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
	if(!(manufacturedOK && maximumTargetMismatch<2.0e-11 &&
		maximumProjectionMismatch<=manufacturedConfig.projectionTolerancePerS &&
		sourceConsumedOnce)) std::printf("V2 reacting metrics target=%.9g projection=%.9g once=%d\n",
		maximumTargetMismatch,maximumProjectionMismatch,sourceConsumedOnce?1:0);
	Check(manufacturedOK && manufacturedElements && maximumTargetMismatch<2.0e-11 &&
		maximumProjectionMismatch<=manufacturedConfig.projectionTolerancePerS && sourceConsumedOnce,
		"V2 reacting 3-D owner derives S_div from its one frozen-packet update and closes R2");

	// V2 variable-density momentum manufacture.  Conservative momentum
	// advection, viscous stress, relative buoyancy and a nonzero divergence
	// target are assembled together before the dynamic-pressure projection.
	// The analytic face velocity and cell pressure are compared over three
	// refinements rather than against the implementation's own residual.
	const std::size_t momentumResolution[3]={8,16,32};
	double manufacturedVelocityError[3]={},manufacturedPressureError[3]={};
	bool momentumManufacture=true,momentumTermsActive=true;
	for(std::size_t level=0;level<3;++level){
		PeriodicMACShape momentumShape;
		momentumShape.nx=momentumResolution[level];
		momentumShape.ny=momentumResolution[level];
		momentumShape.nz=momentumResolution[level];
		momentumShape.cellWidthM=1.0/momentumShape.nx;
		const std::size_t count=momentumShape.CellCount();
		std::vector<ConservativeVector> momentumState(count);
		std::vector<double> momentumTemperature(count,800.0),momentumViscosity(count,0.018);
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
			momentumState[cell]=densityScale*ToConservativeVector(ownerPhysical);
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
		PeriodicMACField momentumRHS;
		const bool momentumRHSOK=RemainingMomentumRHS3D(momentumShape,
			momentumState,exactVelocity,momentumViscosity,
			std::vector<ConservativeVector>(count),0.003,ownerPhysical.GasDensity(),
			std::array<double,3>{{0.4,-0.3,-9.80665}},momentumRHS,&error);
		momentumManufacture=momentumManufacture && momentumRHSOK;
		if(!momentumRHSOK){std::printf("V2 momentum RHS level=%zu: %s\n",level,error.c_str());continue;}
		double activeAdvection=0.0,activeNonpressure=0.0;
		for(std::size_t cell=0;cell<count;++cell) for(unsigned int axis=0;axis<3;++axis){
			activeAdvection=std::max(activeAdvection,std::fabs(momentumAdvection[axis][cell]));
			activeNonpressure=std::max(activeNonpressure,
				std::fabs(momentumRHS.component[axis][cell]));
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
		momentumTermsActive=momentumTermsActive && activeAdvection>0.0 && activeNonpressure>0.0;
		PeriodicMACProjection3DResult momentumProjected;
		const bool momentumProjectionOK=ProjectPeriodicMACVelocity3D(momentumShape,
			GasDensityFromConservative(momentumState),unprojectedMomentum,target,0.003,
			2.0e-9,momentumProjected,&error);
		momentumManufacture=momentumManufacture && momentumProjectionOK;
		if(!momentumProjectionOK){
			std::printf("V2 momentum level=%zu diagnostic: %s\n",level,error.c_str());
			continue;
		}
		double pressureMean=0.0,computedMean=0.0;
		for(std::size_t cell=0;cell<count;++cell){pressureMean+=exactPressure[cell]/count;
			computedMean+=momentumProjected.stepAverageDynamicPressurePa[cell]/count;}
		for(std::size_t cell=0;momentumManufacture && cell<count;++cell){
			manufacturedPressureError[level]+=std::fabs(
				momentumProjected.stepAverageDynamicPressurePa[cell]-computedMean-
				(exactPressure[cell]-pressureMean))/count;
			for(unsigned int axis=0;axis<3;++axis) manufacturedVelocityError[level]+=
				std::fabs(momentumProjected.velocityMPerS.component[axis][cell]-
				exactVelocity.component[axis][cell])/(3.0*count);
		}
	}
	const double velocityOrder=std::log(manufacturedVelocityError[1]/
		manufacturedVelocityError[2])/std::log(2.0);
	const double pressureOrder=std::log(manufacturedPressureError[1]/
		manufacturedPressureError[2])/std::log(2.0);
	if(!(momentumManufacture && momentumTermsActive && velocityOrder>=1.8 &&
		pressureOrder>=1.8)) std::printf("V2 momentum orders velocity=%.6g pressure=%.6g errors %.9g %.9g\n",
		velocityOrder,pressureOrder,manufacturedVelocityError[2],manufacturedPressureError[2]);
	Check(momentumManufacture && momentumTermsActive && velocityOrder>=1.8 &&
		pressureOrder>=1.8,
		"V2 variable-density momentum manufacture is second order with all RHS terms active");

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

	// V3(d): smooth physical mixture-line translation.  Three refinements
	// independently measure every conservative field and derived temperature;
	// the donor control uses the identical Heun tableau and physical fluxes.
	const std::size_t smoothResolution[3]={12,24,48};
	std::array<std::array<double,MethaneConservativeDimension+1>,3> smoothError={};
	std::array<double,MethaneConservativeDimension+1> donorError={};
	double positiveAlphaFaces=0.0,totalAlphaFaces=0.0;
	bool smoothRuns=true,smoothLedgers=true;
	for(std::size_t level=0;level<3;++level){
		PeriodicMACShape smoothShape;
		smoothShape.nx=smoothResolution[level];smoothShape.ny=3;smoothShape.nz=3;
		smoothShape.cellWidthM=1.0/static_cast<double>(smoothShape.nx);
		const std::size_t smoothCount=smoothShape.CellCount();
		std::vector<ConservativeVector> highState(smoothCount),donorState;
		for(std::size_t cell=0;cell<smoothCount;++cell){
			const std::size_t x=cell%smoothShape.nx;
			const double coordinate=(x+0.5)/static_cast<double>(smoothShape.nx);
			const double z=0.20+0.025*std::sin(2.0*pi*coordinate)+
				0.006*std::cos(4.0*pi*coordinate);
			const double temperature=800.0+8.0*std::cos(2.0*pi*coordinate)-
				2.0*std::sin(4.0*pi*coordinate);
			highState[cell]=ToConservativeVector(PhysicalMixtureLineState(
				fuel,thermochemistry,z,temperature));
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
		for(std::size_t step=0;smoothRuns && step<steps;++step){
			std::vector<ConservativeVector> nextHigh,nextDonor;
			smoothRuns=ReferenceAdvancePeriodicTransportHeun3D(smoothShape,highState,
				smoothVelocity,std::vector<double>(smoothCount,0.0),
				std::vector<double>(smoothCount,0.0),false,smoothConfig,fuel,
				thermochemistry,nextHigh,smoothAlpha,&error) &&
				ReferenceAdvancePeriodicTransportHeun3D(smoothShape,donorState,
				smoothVelocity,std::vector<double>(smoothCount,0.0),
				std::vector<double>(smoothCount,0.0),true,smoothConfig,fuel,
				thermochemistry,nextDonor,donorAlpha,&error);
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
			const double z=0.20+0.025*std::sin(2.0*pi*coordinate)+
				0.006*std::cos(4.0*pi*coordinate);
			const double exactTemperature=800.0+8.0*std::cos(2.0*pi*coordinate)-
				2.0*std::sin(4.0*pi*coordinate);
			const ConservativeVector exact=ToConservativeVector(PhysicalMixtureLineState(
				fuel,thermochemistry,z,exactTemperature));
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
				highState[cell][1+MethaneCarbon]==0.0;
		}
		for(std::size_t component=0;component<MethaneConservativeDimension;++component)
			smoothLedgers=smoothLedgers && Near(smoothAfter[component],
				smoothBefore[component],4.0e-14);
		for(double& value:smoothError[level]) value/=static_cast<double>(smoothCount);
		if(level==2) for(double& value:donorError) value/=static_cast<double>(smoothCount);
	}
	bool smoothOrder=smoothRuns,beatsDonor=smoothRuns;
	for(std::size_t component=0;component<MethaneConservativeDimension+1;++component){
		const bool exactZero=smoothError[0][component]==0.0 &&
			smoothError[1][component]==0.0 && smoothError[2][component]==0.0 &&
			donorError[component]==0.0;
		const double order=exactZero?std::numeric_limits<double>::infinity():
			std::log(smoothError[1][component]/
			std::max(smoothError[2][component],1.0e-300))/std::log(2.0);
		smoothOrder=smoothOrder && (exactZero || order>=1.8);
		beatsDonor=beatsDonor && (exactZero ||
			smoothError[2][component]<donorError[component]);
		if(!(exactZero || (order>=1.8 && smoothError[2][component]<donorError[component])))
			std::printf("V3(d) component=%zu errors=%.9g,%.9g,%.9g order=%.6g donor=%.9g\n",
				component,smoothError[0][component],smoothError[1][component],
				smoothError[2][component],order,donorError[component]);
	}
	Check(smoothRuns && smoothLedgers && smoothOrder && beatsDonor && positiveAlphaFaces>0.0 &&
		totalAlphaFaces>0.0,
		"V3(d) smooth 3-D FCT is second order, limiter-active, and beats donor for every field");

	// V4: a finite-step packet, not an externally tabulated heat source, must
	// preserve mass/elements and use the record-derived methane energy ledger.
	std::array<double,MethaneSpeciesCount> reacting = {};
	reacting[MethaneCH4] = 0.05;
	reacting[MethaneO2] = 0.25;
	reacting[MethaneN2] = 0.70;
	const MethaneCellState beginning = StateAtTemperature(
		reacting,0.05,900.0,thermochemistry);
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
	step.deltaTimeS = 0.02;
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
			gridEscape.accepted <= 1.0 && Near(gridEscape.beta,
				gridRadiativeFraction*gridDerivedHeatReleaseW/
				(3.0*gridUnscaledExchange.exchangeWPerM3),2.0e-15),
		"V4 one grid-level pass derives and freezes a shared record-resolved escape factor");
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
	tracePacket.constituentDelta[MethaneO2] = -std::nextafter(
		beginning.constituent[MethaneO2],std::numeric_limits<double>::infinity());
	MethaneCellState traceState;
	Check(ApplySourcePacket(beginning,tracePacket,thermochemistry,traceState,&error) &&
		traceState.constituent[MethaneO2] < 0.0,
		"V4 property inversion maps a roundoff trace without clamping the conservative ledger");
	Check(lesTransport.eddyViscosityM2PerS >= 0.0 &&
		dnsTransport.eddyViscosityM2PerS == 0.0 &&
		dnsTransport.sgsDiffusivityM2PerS == 0.0 &&
		dnsTransport.effectiveViscosityPaS == dnsTransport.molecularViscosityPaS,
		"V2 DNS collapse and LES effective-transport relationships are operational");

	// V4 RED topology: primary combustion and pre-existing carbon oxidation
	// independently request all oxygen, then one shared theta allocates it.
	std::array<double,MethaneSpeciesCount> starved = {};
	starved[MethaneCH4] = 0.2;
	starved[MethaneO2] = 0.005;
	starved[MethaneN2] = 0.5;
	starved[MethaneCarbon] = 0.2;
	const MethaneCellState starvedState = StateAtTemperature(
		starved,0.2,1500.0,thermochemistry);
	MethaneSourcePacket sharedOxygen;
	MethaneReactionStep sharedOxygenFixture = step;
	sharedOxygenFixture.mixingTimeS = sharedOxygenFixture.deltaTimeS;
	Check(BuildMethaneReactionPacket(starvedState,fuel,sharedOxygenFixture,
		sharedOxygen,&error),
		"V4 shared-oxygen packet is constructible");
	Check(Near(-sharedOxygen.constituentDelta[MethaneO2],
		starvedState.constituent[MethaneO2],2.0e-15) &&
		sharedOxygen.reactedFuelKGPerM3 > 0.0 &&
		sharedOxygen.oxidizedCarbonKGPerM3 > 0.0,
		"V4 one shared oxygen scale serves simultaneous primary and soot candidates");

	// V5: the backward-Euler root must close against an independently
	// re-evaluated Planck-mean exchange at the accepted temperature.
	std::array<double,MethaneSpeciesCount> products = {};
	products[MethaneN2] = 0.70;
	products[MethaneCO2] = 0.18;
	products[MethaneH2O] = 0.12;
	const MethaneCellState hotProducts = StateAtTemperature(
		products,0.0,1400.0,thermochemistry);
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
	const MethaneCellState coldProducts = StateAtTemperature(products,0.0,
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
	double carbonExchange1 = 0.0, carbonDerivative1 = 0.0;
	Check(EvaluateSyntheticHotCarbonExchange(1.0e-4,1200.0,300.0,0.25,1800.0,
		carbonExchange0,carbonDerivative0,&error) &&
		EvaluateSyntheticHotCarbonExchange(1.0e-4,1500.0,300.0,0.25,1800.0,
			carbonExchange1,carbonDerivative1,&error),
		"V5 distinct synthetic hot-carbon fixture evaluates");
	const double ambient5 = std::pow(300.0,5.0);
	Check(Near(carbonExchange0/(std::pow(1200.0,5.0)-ambient5),
		carbonExchange1/(std::pow(1500.0,5.0)-ambient5),2.0e-15) &&
		Near(carbonDerivative0,5.0*carbonExchange0*std::pow(1200.0,4.0)/
			(std::pow(1200.0,5.0)-ambient5),2.0e-15),
		"V5 hot-carbon oracle follows the required fv*(T^5-Tinf^5) law");

	// V6: the eligibility graph is memoryless.  A vitiated barrier blocks a
	// pilot-connected pocket while a separate CFT-passing autoignition cell seeds itself.
	IgnitionGrid grid;
	grid.nx = 5; grid.ny = 1; grid.nz = 1;
	grid.cells.assign(5,beginning);
	grid.pilotMask.assign(5,false);
	grid.pilotMask[0] = true;
	const double gridMixtureFraction = beginning.rhoTotalZ/beginning.TotalDensity();
	grid.cells[0] = StateAtTemperature(grid.cells[0].constituent,
		gridMixtureFraction,1001.0,thermochemistry);
	grid.cells[2].constituent[MethaneO2] = 0.0;
	grid.cells[2] = StateAtTemperature(grid.cells[2].constituent,
		gridMixtureFraction,700.0,thermochemistry);
	grid.cells[4] = StateAtTemperature(grid.cells[4].constituent,
		gridMixtureFraction,1300.0,thermochemistry);
	std::vector<bool> eligibility, repeatedEligibility;
	Check(BuildIgnitionEligibility(grid,fuel,thermochemistry,transport,
		eligibility,&error),"V6 ignition eligibility graph evaluates");
	Check(eligibility.size() == 5 && eligibility[0] && eligibility[1] &&
		!eligibility[2] && eligibility[3] && eligibility[4],
		"V6 vitiated barrier blocks the pilot while isolated autoignition seeds its remote pocket");
	Check(BuildIgnitionEligibility(grid,fuel,thermochemistry,transport,
		repeatedEligibility,&error) && repeatedEligibility == eligibility,
		"V6 eligibility depends only on the current conservative state");
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
			lesTransport.effectiveViscosityPaS/beginning.GasDensity()),3,
		stableStep,&error) && stableStep.seconds > 0.0 &&
		std::isfinite(stableStep.seconds),
		"V6 timestep selector combines advective, positive-buoyancy, and explicit-diffusion limits");

	if( failures ) {
		std::printf("FireSimulationSolverTest: %d failure(s)\n",failures);
		return 1;
	}
	std::printf("FireSimulationSolverTest: V1 pressure-open gate and current kernel checks passed\n");
	return 0;
}
