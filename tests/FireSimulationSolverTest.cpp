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
	std::vector<double> openDensity(8,1.18), openMomentum(9,0.0);
	std::vector<double> openTarget(8,0.0);
	OpenMACProjection1DResult openRest;
	Check(ProjectPressureOpenMACVelocity1D(openDensity,openMomentum,openTarget,
		1.18,0.05,0.01,1.0e-10,1.18e-10,false,false,openRest,&error) &&
		!openRest.leftInflow && !openRest.rightInflow &&
		*std::max_element(openRest.velocityMPerS.begin(),
			openRest.velocityMPerS.end()) == 0.0 &&
		openRest.maximumBoundaryHeadResidualPa == 0.0,
		"V1 quiescent pressure-open box preserves exact hydrostatic-relative rest");
	std::fill(openMomentum.begin(),openMomentum.end(),1.18*0.1);
	OpenMACProjection1DResult throughFlow;
	const bool throughFlowOK = ProjectPressureOpenMACVelocity1D(openDensity,
		openMomentum,openTarget,1.18,0.05,0.01,1.0e-9,1.18e-10,
		false,false,throughFlow,&error);
	if( !throughFlowOK ) std::printf("V1 open-flow diagnostic: %s\n",error.c_str());
	Check(throughFlowOK && throughFlow.leftInflow && !throughFlow.rightInflow &&
		throughFlow.maximumBoundaryHeadResidualPa <= 1.18e-10,
		"V1 active set reclassifies ambient inflow and enforces total head");
	for( unsigned int classCode=0; classCode<4; ++classCode ) {
		const bool stage0Inflow = (classCode&1u) != 0;
		const bool stage1Inflow = (classCode&2u) != 0;
		OpenMACProjection1DResult finalOpen;
		const bool finalOK = ProjectPressureOpenMACVelocity1DFinal(openDensity,
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
	PeriodicTransportConfig transportConfig;
	transportConfig.cellWidthM = 1.0/static_cast<double>(transportCells);
	transportConfig.deltaTimeS = 0.002;
	transportConfig.ambientTemperatureK = 300.0;
	transportConfig.adiabaticTemperatureK = 2500.0;
	std::vector<ConservativeVector> zeroSource(transportCells);
	std::vector<ConservativeVector> transportResult;
	std::vector<double> transportAlpha;
	const bool transportOK = AdvancePeriodicTransportHeun(transportBeginning,
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
	const bool coupledOK = AdvancePeriodicProjectedHeun(uniformState,uniformMomentum,
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
	Check(fuel.ResolveRadiativeFraction("solver-v4-grid-source-v1",false,0.0,
		gridRadiativeFraction,&error) && BuildFrozenMethaneSourcePackets(
		{beginning,beginning},{step,step},{1.0,2.0},300.0,600.0,600.0,
		gridRadiativeFraction,true,fuel,thermochemistry,opacity,gridPackets,
		gridEscape,&error) && gridPackets.size() == 2 &&
		gridPackets[0].radiativeCoolingWPerM3 ==
			gridPackets[1].radiativeCoolingWPerM3 && gridEscape.accepted > 0.0,
		"V4 one grid-level pass derives and freezes a shared record-resolved escape factor");
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
	std::printf("FireSimulationSolverTest: V1-V6 kernel checks passed\n");
	return 0;
}
