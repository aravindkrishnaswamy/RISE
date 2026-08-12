//////////////////////////////////////////////////////////////////////
//
//  fire_simulator_core.h - Standalone Phase-C solver kernels
//
//  This header is intentionally owned by tools/: the simulator is an
//  offline executable and is not linked into the renderer.
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_FIRE_SIMULATOR_CORE_
#define RISE_FIRE_SIMULATOR_CORE_

#include "../src/Library/Utilities/FireSimulationRecords.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <queue>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace RISE
{
	namespace FireSim
	{
		enum MethaneSpeciesIndex
		{
			MethaneCH4 = 0,
			MethaneO2 = 1,
			MethaneN2 = 2,
			MethaneCO2 = 3,
			MethaneH2O = 4,
			MethaneCO = 5,
			MethaneCarbon = 6,
			MethaneSpeciesCount = 7
		};

		static const std::size_t MethaneMassStateDimension = 1+MethaneSpeciesCount;
		static const std::size_t MethaneConservativeDimension =
			MethaneMassStateDimension+1;

		struct ConservativeVector
		{
			std::array<double,MethaneConservativeDimension> value;
			ConservativeVector() { value.fill(0.0); }
			double& operator[]( const std::size_t index ) { return value[index]; }
			double operator[]( const std::size_t index ) const { return value[index]; }
		};

		inline ConservativeVector operator+(
			const ConservativeVector& first,
			const ConservativeVector& second
			)
		{
			ConservativeVector result;
			for( std::size_t index=0; index<MethaneConservativeDimension; ++index ) {
				result[index] = first[index]+second[index];
			}
			return result;
		}

		inline ConservativeVector operator-(
			const ConservativeVector& first,
			const ConservativeVector& second
			)
		{
			ConservativeVector result;
			for( std::size_t index=0; index<MethaneConservativeDimension; ++index ) {
				result[index] = first[index]-second[index];
			}
			return result;
		}

		inline ConservativeVector operator*(
			const double scale,
			const ConservativeVector& input
			)
		{
			ConservativeVector result;
			for( std::size_t index=0; index<MethaneConservativeDimension; ++index ) {
				result[index] = scale*input[index];
			}
			return result;
		}

		inline bool Fail( std::string* error, const std::string& message )
		{
			if( error ) *error = message;
			return false;
		}

		struct MethaneCellState
		{
			double rhoTotalZ;
			std::array<double,MethaneSpeciesCount> constituent;
			double sensibleEnergyJPerM3;
			double temperatureK;

			MethaneCellState() : rhoTotalZ(0.0), sensibleEnergyJPerM3(0.0),
				temperatureK(0.0)
			{
				constituent.fill(0.0);
			}

			double GasDensity() const
			{
				double result = 0.0;
				for( std::size_t index=0; index<MethaneCarbon; ++index ) {
					result += constituent[index];
				}
				return result;
			}

			double TotalDensity() const
			{
				return GasDensity()+constituent[MethaneCarbon];
			}
		};

		inline ConservativeVector ToConservativeVector( const MethaneCellState& state )
		{
			ConservativeVector result;
			result[0] = state.rhoTotalZ;
			for( std::size_t index=0; index<MethaneSpeciesCount; ++index ) {
				result[1+index] = state.constituent[index];
			}
			result[MethaneMassStateDimension] = state.sensibleEnergyJPerM3;
			return result;
		}

		inline MethaneCellState FromConservativeVector( const ConservativeVector& input )
		{
			MethaneCellState result;
			result.rhoTotalZ = input[0];
			for( std::size_t index=0; index<MethaneSpeciesCount; ++index ) {
				result.constituent[index] = input[1+index];
			}
			result.sensibleEnergyJPerM3 = input[MethaneMassStateDimension];
			return result;
		}

		inline std::vector<std::pair<std::string,double> > ThermochemicalDensities(
			const MethaneCellState& state
			)
		{
			static const char* names[MethaneSpeciesCount] = {
				"CH4", "O2", "N2", "CO2", "H2O", "CO", "C(gr)"
			};
			std::vector<std::pair<std::string,double> > result;
			for( std::size_t index=0; index<MethaneSpeciesCount; ++index ) {
				result.push_back(std::make_pair(std::string(names[index]),state.constituent[index]));
			}
			return result;
		}

		inline bool ThermochemicalDensitiesWithinForwardEnvelope(
			const MethaneCellState& state,
			std::vector<std::pair<std::string,double> >& result,
			std::string* error = 0
			)
		{
			static const char* names[MethaneSpeciesCount] = {
				"CH4", "O2", "N2", "CO2", "H2O", "CO", "C(gr)"
			};
			double scale = 1.0;
			for( const double density : state.constituent ) {
				if( !std::isfinite(density) ) {
					return Fail(error,"fire solver thermochemical property view is non-finite");
				}
				scale = std::max(scale,std::fabs(density));
			}
			const double tolerance = 2048.0*std::numeric_limits<double>::epsilon()*scale;
			result.clear();
			for( std::size_t index=0; index<MethaneSpeciesCount; ++index ) {
				if( state.constituent[index] < -tolerance ) {
					return Fail(error,"fire solver constituent exceeds the certified fp64 forward envelope");
				}
				// The conservative state is not modified.  Only the thermochemical
				// property view maps a sign-roundoff trace to the boundary value.
				result.push_back(std::make_pair(std::string(names[index]),
					std::max(0.0,state.constituent[index])));
			}
			return true;
		}

		inline bool ValidateCellState(
			const MethaneCellState& state,
			std::string* error = 0
			)
		{
			if( !std::isfinite(state.rhoTotalZ) || state.rhoTotalZ < 0.0 ||
				!std::isfinite(state.sensibleEnergyJPerM3) ||
				!std::isfinite(state.temperatureK) || state.temperatureK <= 0.0 ) {
				return Fail(error,"fire solver cell contains a non-finite or negative primary field");
			}
			for( const double density : state.constituent ) {
				if( !std::isfinite(density) || density < 0.0 ) {
					return Fail(error,"fire solver cell contains a negative constituent density");
				}
			}
			const double total = state.TotalDensity();
			return (total > 0.0 && state.rhoTotalZ <= total) ||
				Fail(error,"fire solver mixture fraction is outside [0,1]");
		}

		inline bool EquationOfStateResidual(
			const MethaneCellState& state,
			const FireSimulationMethaneRecord& thermochemistry,
			double& result,
			std::string* error = 0
			)
		{
			static const char* names[MethaneCarbon] = {
				"CH4", "O2", "N2", "CO2", "H2O", "CO"
			};
			if( !thermochemistry.IsValid() || !std::isfinite(state.temperatureK) ||
				state.temperatureK <= 0.0 || !std::isfinite(state.rhoTotalZ) ||
				state.rhoTotalZ < 0.0 ) return Fail(error,"fire solver EOS state is invalid");
			std::vector<std::pair<std::string,double> > propertyDensities;
			if( !ThermochemicalDensitiesWithinForwardEnvelope(state,propertyDensities,error) ) {
				return false;
			}
			double gasDensity = 0.0;
			double molarDensityKMolPerM3 = 0.0;
			for( std::size_t species=0; species<MethaneCarbon; ++species ) {
				const FireThermochemistrySpecies* property =
					thermochemistry.FindSpecies(names[species]);
				if( !property ) return Fail(error,"fire solver EOS lacks a gas species");
				gasDensity += propertyDensities[species].second;
				molarDensityKMolPerM3 += propertyDensities[species].second/
					property->molecularWeightKGPerKMol;
			}
			if( gasDensity <= 0.0 || molarDensityKMolPerM3 <= 0.0 ) {
				return Fail(error,"fire solver EOS has no positive gas density");
			}
			const double meanWeightKGPerKMol = gasDensity/molarDensityKMolPerM3;
			const double representedPressure = gasDensity*8314.46261815324*
				state.temperatureK/meanWeightKGPerKMol;
			result = std::fabs(representedPressure/thermochemistry.ThermodynamicPressurePa()-1.0);
			return std::isfinite(result) || Fail(error,"fire solver EOS residual overflowed");
		}

		struct CellTransportEvaluation
		{
			double gasCpJPerKGK;
			double molecularViscosityPaS;
			double molecularConductivityWPerMK;
			double eddyViscosityM2PerS;
			double molecularDiffusivityM2PerS;
			double sgsDiffusivityM2PerS;
			double totalDiffusivityM2PerS;
			double effectiveViscosityPaS;
			double effectiveConductivityWPerMK;
			CellTransportEvaluation() : gasCpJPerKGK(0.0), molecularViscosityPaS(0.0),
				molecularConductivityWPerMK(0.0), eddyViscosityM2PerS(0.0),
				molecularDiffusivityM2PerS(0.0), sgsDiffusivityM2PerS(0.0),
				totalDiffusivityM2PerS(0.0), effectiveViscosityPaS(0.0),
				effectiveConductivityWPerMK(0.0) {}
		};

		inline bool EvaluateCellTransport(
			const MethaneCellState& state,
			const double velocityGradientPerS[3][3],
			const double directionalWidthsM[3],
			const bool dns,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			CellTransportEvaluation& result,
			std::string* error = 0
			)
		{
			static const char* names[MethaneCarbon] = {
				"CH4", "O2", "N2", "CO2", "H2O", "CO"
			};
			if( !ValidateCellState(state,error) || !thermochemistry.IsValid() ||
				!transport.IsValid() ) return false;
			const double gasDensity = state.GasDensity();
			if( gasDensity <= 0.0 ) return Fail(error,"fire solver transport has no gas mass");
			std::vector<std::pair<std::string,double> > massFractions;
			result = CellTransportEvaluation();
			for( std::size_t species=0; species<MethaneCarbon; ++species ) {
				const double fraction = state.constituent[species]/gasDensity;
				massFractions.push_back(std::make_pair(std::string(names[species]),fraction));
				double cp = 0.0;
				if( !thermochemistry.CpJPerKGK(names[species],state.temperatureK,cp,error) ) return false;
				result.gasCpJPerKGK += fraction*cp;
			}
			if( !transport.MixtureViscosityPaS(massFractions,thermochemistry,
				state.temperatureK,result.molecularViscosityPaS,error) ||
				!transport.MixtureConductivityWPerMK(massFractions,thermochemistry,
					state.temperatureK,result.molecularConductivityWPerMK,error) ) return false;
			if( dns ) {
				result.eddyViscosityM2PerS = 0.0;
			} else if( !transport.VremanEddyViscosityM2PerS(velocityGradientPerS,
				directionalWidthsM,result.eddyViscosityM2PerS,error) ) return false;
			return transport.EffectiveTransport(result.molecularViscosityPaS,
				result.molecularConductivityWPerMK,gasDensity,result.gasCpJPerKGK,
				result.eddyViscosityM2PerS,dns,result.molecularDiffusivityM2PerS,
				result.sgsDiffusivityM2PerS,result.totalDiffusivityM2PerS,
				result.effectiveViscosityPaS,result.effectiveConductivityWPerMK,error);
		}

		inline bool ComputeMixingTimeS(
			const MethaneCellState& state,
			const CellTransportEvaluation& evaluation,
			const FireSimulationTransportRecord& transport,
			const double filterWidthM,
			const double ambientGasDensityKGPerM3,
			const double gravityMagnitudeMPerS2,
			const bool dns,
			double& result,
			std::string* error = 0
			)
		{
			if( !transport.IsValid() || !ValidateCellState(state,error) ||
				!std::isfinite(filterWidthM) || filterWidthM <= 0.0 ||
				!std::isfinite(ambientGasDensityKGPerM3) || ambientGasDensityKGPerM3 <= 0.0 ||
				!std::isfinite(gravityMagnitudeMPerS2) || gravityMagnitudeMPerS2 < 0.0 ||
				!std::isfinite(evaluation.totalDiffusivityM2PerS) ||
				evaluation.totalDiffusivityM2PerS <= 0.0 ) {
				return Fail(error,"fire solver mixing-time inputs are outside their domain");
			}
			const double tauDiff = filterWidthM*filterWidthM/evaluation.totalDiffusivityM2PerS;
			double fastest = tauDiff;
			if( !dns ) {
				if( evaluation.eddyViscosityM2PerS > 0.0 ) {
					const double kSgs = std::pow(evaluation.eddyViscosityM2PerS/
						(transport.VremanCnu()*filterWidthM),2.0);
					fastest = std::min(fastest,filterWidthM/std::sqrt(2.0*kSgs));
				}
				const double gasDensity = state.GasDensity();
				const double reducedGravity = std::max(0.0,gravityMagnitudeMPerS2*
					(ambientGasDensityKGPerM3-gasDensity)/gasDensity);
				if( reducedGravity > 0.0 ) fastest = std::min(fastest,
					std::sqrt(2.0*filterWidthM/reducedGravity));
			}
			result = std::max(transport.ChemicalTimeS(),fastest);
			return (result > 0.0 && std::isfinite(result)) ||
				Fail(error,"fire solver mixing time is invalid");
		}

		struct StableTimeStep
		{
			double seconds;
			std::string activeLimit;
			StableTimeStep() : seconds(0.0) {}
		};

		inline bool ComputeStableTimeStep(
			const double cellWidthM,
			const double maximumVelocityMPerS,
			const double maximumPositiveReducedGravityMPerS2,
			const double maximumKinematicTransportM2PerS,
			const unsigned int dimensions,
			StableTimeStep& result,
			std::string* error = 0
			)
		{
			if( !std::isfinite(cellWidthM) || cellWidthM <= 0.0 ||
				!std::isfinite(maximumVelocityMPerS) || maximumVelocityMPerS < 0.0 ||
				!std::isfinite(maximumPositiveReducedGravityMPerS2) ||
				maximumPositiveReducedGravityMPerS2 < 0.0 ||
				!std::isfinite(maximumKinematicTransportM2PerS) ||
				maximumKinematicTransportM2PerS < 0.0 || dimensions == 0 || dimensions > 3 ) {
				return Fail(error,"fire solver timestep inputs are invalid");
			}
			result.seconds = std::numeric_limits<double>::infinity();
			result.activeLimit = "unbounded_static_state";
			auto accept = [&]( const double candidate, const char* label ) {
				if( candidate < result.seconds ) {
					result.seconds = candidate;
					result.activeLimit = label;
				}
			};
			if( maximumVelocityMPerS > 0.0 ) {
				accept(0.8*cellWidthM/maximumVelocityMPerS,"advective_CFL");
			}
			if( maximumPositiveReducedGravityMPerS2 > 0.0 ) {
				accept(std::sqrt(2.0*cellWidthM/maximumPositiveReducedGravityMPerS2),
					"buoyant_acceleration");
			}
			if( maximumKinematicTransportM2PerS > 0.0 ) {
				accept(0.45*cellWidthM*cellWidthM/(static_cast<double>(dimensions)*
					maximumKinematicTransportM2PerS),"explicit_diffusion");
			}
			return (result.seconds > 0.0 && !std::isnan(result.seconds)) ||
				Fail(error,"fire solver timestep selection failed");
		}

		struct MethaneSourcePacket
		{
			std::array<double,MethaneSpeciesCount> constituentDelta;
			double sensibleEnergyDeltaJPerM3;
			double reactedFuelKGPerM3;
			double oxidizedCarbonKGPerM3;
			double grossCarbonFormedKGPerM3;
			double gasHeatReleaseWPerM3;
			double sootHeatReleaseWPerM3;
			double radiativeCoolingWPerM3;

			MethaneSourcePacket() : sensibleEnergyDeltaJPerM3(0.0),
				reactedFuelKGPerM3(0.0), oxidizedCarbonKGPerM3(0.0),
				grossCarbonFormedKGPerM3(0.0),
				gasHeatReleaseWPerM3(0.0), sootHeatReleaseWPerM3(0.0),
				radiativeCoolingWPerM3(0.0)
			{
				constituentDelta.fill(0.0);
			}
		};

		struct MethaneReactionStep
		{
			double deltaTimeS;
			double mixingTimeS;
			bool primaryEligible;
			bool sootOxidationEnabled;

			MethaneReactionStep() : deltaTimeS(0.0), mixingTimeS(0.0),
				primaryEligible(false),
				sootOxidationEnabled(false) {}
		};

		inline bool BuildMethaneReactionPacket(
			const MethaneCellState& beginning,
			const FireSimulationMethaneRecord& fuel,
			const MethaneReactionStep& step,
			MethaneSourcePacket& packet,
			std::string* error = 0
			)
		{
			packet = MethaneSourcePacket();
			if( !fuel.IsValid() || !ValidateCellState(beginning,error) ||
				!std::isfinite(step.deltaTimeS) || step.deltaTimeS <= 0.0 ||
				!std::isfinite(step.mixingTimeS) || step.mixingTimeS <= 0.0 ) {
				return Fail(error,"fire solver reaction step is outside its physical domain");
			}
			const double relaxation = -std::expm1(-step.deltaTimeS/step.mixingTimeS);
			const double oxygen = beginning.constituent[MethaneO2];
			const double primaryCandidate = step.primaryEligible ? relaxation*std::min(
				beginning.constituent[MethaneCH4],
				oxygen/fuel.StoichiometricOxygenKGPerKGFuel()) : 0.0;
			const bool oxidizes = step.sootOxidationEnabled &&
				beginning.temperatureK > fuel.SootOxidationTemperatureK();
			const double sootCandidate = oxidizes ? relaxation*std::min(
				beginning.constituent[MethaneCarbon],
				oxygen/fuel.SootOxygenKGPerKGCarbon()) : 0.0;
			const double oxygenDemand = fuel.StoichiometricOxygenKGPerKGFuel()*primaryCandidate+
				fuel.SootOxygenKGPerKGCarbon()*sootCandidate;
			const double theta = oxygenDemand > 0.0 ? std::min(1.0,oxygen/oxygenDemand) : 1.0;
			const double reacted = theta*primaryCandidate;
			const double oxidized = theta*sootCandidate;
			packet.reactedFuelKGPerM3 = reacted;
			packet.oxidizedCarbonKGPerM3 = oxidized;
			packet.grossCarbonFormedKGPerM3 = reacted*fuel.SootYieldKGPerKGFuel();
			const std::vector<double>& primary = fuel.PrimaryReactionDelta();
			if( primary.size() != MethaneSpeciesCount ) {
				return Fail(error,"fire solver methane reaction vector has the wrong dimension");
			}
			for( std::size_t index=0; index<MethaneSpeciesCount; ++index ) {
				packet.constituentDelta[index] = reacted*primary[index];
			}
			packet.constituentDelta[MethaneCarbon] += packet.grossCarbonFormedKGPerM3;
			packet.constituentDelta[MethaneCarbon] -= oxidized;
			packet.constituentDelta[MethaneO2] -=
				fuel.SootOxygenKGPerKGCarbon()*oxidized;
			packet.constituentDelta[MethaneCO2] +=
				fuel.SootCO2KGPerKGCarbon()*oxidized;
			packet.sensibleEnergyDeltaJPerM3 =
				reacted*fuel.LowerHeatingValueJPerKG()+
				oxidized*fuel.SootHeatReleaseJPerKGCarbon();
			packet.gasHeatReleaseWPerM3 = reacted*fuel.LowerHeatingValueJPerKG()/step.deltaTimeS;
			packet.sootHeatReleaseWPerM3 = oxidized*fuel.SootHeatReleaseJPerKGCarbon()/step.deltaTimeS;
			for( std::size_t index=0; index<MethaneSpeciesCount; ++index ) {
				if( !std::isfinite(packet.constituentDelta[index]) ) {
					return Fail(error,"fire solver reaction packet contains a non-finite constituent delta");
				}
				if( beginning.constituent[index]+packet.constituentDelta[index] <
					-64.0*std::numeric_limits<double>::epsilon()*
					std::max(1.0,beginning.constituent[index]) ) {
					return Fail(error,"fire solver shared oxygen allocation produced a negative inventory");
				}
			}
			return (std::isfinite(packet.sensibleEnergyDeltaJPerM3) &&
				std::isfinite(packet.reactedFuelKGPerM3) &&
				std::isfinite(packet.oxidizedCarbonKGPerM3) &&
				std::isfinite(packet.grossCarbonFormedKGPerM3) &&
				std::isfinite(packet.gasHeatReleaseWPerM3) &&
				std::isfinite(packet.sootHeatReleaseWPerM3)) ||
				Fail(error,"fire solver reaction packet overflowed");
		}

		inline bool ApplySourcePacket(
			const MethaneCellState& beginning,
			const MethaneSourcePacket& packet,
			const FireSimulationMethaneRecord& thermochemistry,
			MethaneCellState& result,
			std::string* error = 0
			)
		{
			MethaneCellState candidate = beginning;
			for( std::size_t index=0; index<MethaneSpeciesCount; ++index ) {
				candidate.constituent[index] += packet.constituentDelta[index];
			}
			candidate.sensibleEnergyJPerM3 += packet.sensibleEnergyDeltaJPerM3;
			std::vector<std::pair<std::string,double> > propertyDensities;
			if( !ThermochemicalDensitiesWithinForwardEnvelope(candidate,propertyDensities,error) ) {
				return false;
			}
			if( !thermochemistry.InvertMixtureTemperatureK(
				propertyDensities,candidate.sensibleEnergyJPerM3,
				candidate.temperatureK,error) ) return false;
			const double total = candidate.TotalDensity();
			return (std::isfinite(total) && total > 0.0 &&
				std::isfinite(candidate.rhoTotalZ) && candidate.rhoTotalZ >= 0.0 &&
				candidate.rhoTotalZ <= total && std::isfinite(candidate.temperatureK) &&
				candidate.temperatureK > 0.0 && ((result=candidate),true)) ||
				Fail(error,"fire solver source packet produced an invalid accepted state");
		}

		inline double MCScalarSlope( const double backward, const double forward )
		{
			if( backward*forward <= 0.0 ) return 0.0;
			const double centered = 0.5*(backward+forward);
			const double sign = centered < 0.0 ? -1.0 : 1.0;
			return sign*std::min(std::fabs(centered),
				2.0*std::min(std::fabs(backward),std::fabs(forward)));
		}

		inline bool InvariantMCMassSlopes(
			const std::vector<ConservativeVector>& cells,
			const FireCertifiedNullspace& reconstruction,
			std::vector<std::array<double,MethaneMassStateDimension> >& slopes,
			std::string* error = 0
			)
		{
			const std::size_t count = cells.size();
			if( count < 3 || reconstruction.stateDimension != MethaneMassStateDimension ||
				reconstruction.orthonormalBasis.size() !=
				reconstruction.stateDimension*reconstruction.nullity ) {
				return Fail(error,"fire solver invariant reconstruction dimensions are invalid");
			}
			std::vector<std::vector<double> > coordinate(count,
				std::vector<double>(reconstruction.nullity,0.0));
			for( std::size_t cell=0; cell<count; ++cell ) {
				for( std::size_t basis=0; basis<reconstruction.nullity; ++basis ) {
					for( std::size_t row=0; row<MethaneMassStateDimension; ++row ) {
						coordinate[cell][basis] += reconstruction.orthonormalBasis[
							row*reconstruction.nullity+basis]*cells[cell][row];
					}
				}
			}
			slopes.assign(count,std::array<double,MethaneMassStateDimension>());
			for( std::size_t cell=0; cell<count; ++cell ) {
				slopes[cell].fill(0.0);
				const std::size_t previous = (cell+count-1)%count;
				const std::size_t next = (cell+1)%count;
				for( std::size_t basis=0; basis<reconstruction.nullity; ++basis ) {
					const double slope = MCScalarSlope(
						coordinate[cell][basis]-coordinate[previous][basis],
						coordinate[next][basis]-coordinate[cell][basis]);
					for( std::size_t row=0; row<MethaneMassStateDimension; ++row ) {
						slopes[cell][row] += reconstruction.orthonormalBasis[
							row*reconstruction.nullity+basis]*slope;
					}
				}
			}
			return true;
		}

		struct PeriodicTransportConfig
		{
			double cellWidthM;
			double deltaTimeS;
			double ambientTemperatureK;
			double adiabaticTemperatureK;
			double ambientGasDensityKGPerM3;
			double gravityMPerS2;
			PeriodicTransportConfig() : cellWidthM(0.0), deltaTimeS(0.0),
				ambientTemperatureK(0.0), adiabaticTemperatureK(0.0),
				ambientGasDensityKGPerM3(0.0), gravityMPerS2(0.0) {}
		};

		struct PeriodicFluxPair
		{
			std::vector<ConservativeVector> low;
			std::vector<ConservativeVector> high;
			std::vector<std::array<double,MethaneMassStateDimension> > nonadvectiveMass;
			std::vector<double> nonadvectiveEnergy;
		};

		inline double HarmonicMean( const double first, const double second )
		{
			if( first <= 0.0 || second <= 0.0 ) return 0.0;
			return 2.0*first*second/(first+second);
		}

		inline bool BuildPeriodicFluxPair(
			const std::vector<ConservativeVector>& cells,
			const std::vector<double>& temperatureK,
			const std::vector<double>& faceVelocityMPerS,
			const std::vector<double>& diffusivityM2PerS,
			const std::vector<double>& conductivityWPerMK,
			const double cellWidthM,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			PeriodicFluxPair& result,
			std::string* error = 0
			)
		{
			const std::size_t count = cells.size();
			if( count < 3 || temperatureK.size() != count ||
				faceVelocityMPerS.size() != count || diffusivityM2PerS.size() != count ||
				conductivityWPerMK.size() != count || !std::isfinite(cellWidthM) ||
				cellWidthM <= 0.0 ) {
				return Fail(error,"fire solver periodic transport arrays are malformed");
			}
			std::vector<std::array<double,MethaneMassStateDimension> > massSlope;
			if( !InvariantMCMassSlopes(cells,fuel.ConservativeReconstruction(),
				massSlope,error) ) return false;
			std::vector<double> energySlope(count,0.0);
			for( std::size_t cell=0; cell<count; ++cell ) {
				const std::size_t previous = (cell+count-1)%count;
				const std::size_t next = (cell+1)%count;
				energySlope[cell] = MCScalarSlope(
					cells[cell][MethaneMassStateDimension]-cells[previous][MethaneMassStateDimension],
					cells[next][MethaneMassStateDimension]-cells[cell][MethaneMassStateDimension]);
			}
			result.low.assign(count,ConservativeVector());
			result.high.assign(count,ConservativeVector());
			result.nonadvectiveMass.assign(count,
				std::array<double,MethaneMassStateDimension>());
			result.nonadvectiveEnergy.assign(count,0.0);
			for( std::size_t face=0; face<count; ++face ) {
				const std::size_t left = face;
				const std::size_t right = (face+1)%count;
				double totalLeft = 0.0, totalRight = 0.0;
				for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
					totalLeft += cells[left][1+species];
					totalRight += cells[right][1+species];
				}
				if( totalLeft <= 0.0 || totalRight <= 0.0 ||
					!std::isfinite(temperatureK[left]) || !std::isfinite(temperatureK[right]) ||
					!std::isfinite(faceVelocityMPerS[face]) || diffusivityM2PerS[left] < 0.0 ||
					diffusivityM2PerS[right] < 0.0 || conductivityWPerMK[left] < 0.0 ||
					conductivityWPerMK[right] < 0.0 ) {
					return Fail(error,"fire solver periodic transport state is outside its domain");
				}
				const double rhoD = HarmonicMean(totalLeft*diffusivityM2PerS[left],
					totalRight*diffusivityM2PerS[right]);
				std::vector<double> raw(MethaneMassStateDimension,0.0), projected;
				raw[0] = -rhoD*(cells[right][0]/totalRight-cells[left][0]/totalLeft)/cellWidthM;
				for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
					raw[1+species] = -rhoD*(cells[right][1+species]/totalRight-
						cells[left][1+species]/totalLeft)/cellWidthM;
				}
				if( !fuel.NonadvectiveFluxProjection().Project(raw,projected,error) ||
					projected.size() != MethaneMassStateDimension ) return false;
				for( std::size_t index=0; index<MethaneMassStateDimension; ++index ) {
					result.nonadvectiveMass[face][index] = projected[index];
				}
				const double faceTemperature = 0.5*(temperatureK[left]+temperatureK[right]);
				double enthalpyFlux = 0.0;
				static const char* names[MethaneSpeciesCount] = {
					"CH4", "O2", "N2", "CO2", "H2O", "CO", "C(gr)"
				};
				for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
					double sensibleEnthalpy = 0.0;
					if( !thermochemistry.SensibleEnthalpyJPerKG(names[species],
						faceTemperature,sensibleEnthalpy,error) ) return false;
					enthalpyFlux += sensibleEnthalpy*projected[1+species];
				}
				const double conductivity = HarmonicMean(conductivityWPerMK[left],
					conductivityWPerMK[right]);
				const double nonadvectiveEnergy = enthalpyFlux-conductivity*
					(temperatureK[right]-temperatureK[left])/cellWidthM;
				result.nonadvectiveEnergy[face] = nonadvectiveEnergy;
				const std::size_t donor = faceVelocityMPerS[face] >= 0.0 ? left : right;
				for( std::size_t index=0; index<MethaneMassStateDimension; ++index ) {
					const double highFaceValue = faceVelocityMPerS[face] >= 0.0 ?
						cells[left][index]+0.5*massSlope[left][index] :
						cells[right][index]-0.5*massSlope[right][index];
					result.low[face][index] = faceVelocityMPerS[face]*cells[donor][index]+
						projected[index];
					result.high[face][index] = faceVelocityMPerS[face]*highFaceValue+
						projected[index];
				}
				const double highEnergy = faceVelocityMPerS[face] >= 0.0 ?
					cells[left][MethaneMassStateDimension]+0.5*energySlope[left] :
					cells[right][MethaneMassStateDimension]-0.5*energySlope[right];
				result.low[face][MethaneMassStateDimension] = faceVelocityMPerS[face]*
					cells[donor][MethaneMassStateDimension]+nonadvectiveEnergy;
				result.high[face][MethaneMassStateDimension] = faceVelocityMPerS[face]*
					highEnergy+nonadvectiveEnergy;
			}
			return true;
		}

		inline bool ConservativeStateFeasible(
			const ConservativeVector& state,
			const std::array<double,MethaneSpeciesCount>& ambientEnthalpy,
			const std::array<double,MethaneSpeciesCount>& adiabaticEnthalpy,
			const double tolerance
			)
		{
			double total = 0.0, lowerEnergy = 0.0, upperEnergy = 0.0;
			for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
				if( state[1+species] < -tolerance ) return false;
				total += state[1+species];
				lowerEnergy += state[1+species]*ambientEnthalpy[species];
				upperEnergy += state[1+species]*adiabaticEnthalpy[species];
			}
			return total > 0.0 && state[0] >= -tolerance && state[0] <= total+tolerance &&
				state[MethaneMassStateDimension] >= lowerEnergy-tolerance &&
				state[MethaneMassStateDimension] <= upperEnergy+tolerance;
		}

		inline bool FireSimulationEnthalpyBounds(
			const PeriodicTransportConfig& config,
			const FireSimulationMethaneRecord& thermochemistry,
			std::array<double,MethaneSpeciesCount>& ambient,
			std::array<double,MethaneSpeciesCount>& adiabatic,
			std::string* error = 0
			)
		{
			static const char* names[MethaneSpeciesCount] = {
				"CH4", "O2", "N2", "CO2", "H2O", "CO", "C(gr)"
			};
			if( config.ambientTemperatureK >= config.adiabaticTemperatureK ) {
				return Fail(error,"fire solver FCT energy interval is invalid");
			}
			for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
				if( !thermochemistry.SensibleEnthalpyJPerKG(names[species],
					config.ambientTemperatureK,ambient[species],error) ||
					!thermochemistry.SensibleEnthalpyJPerKG(names[species],
						config.adiabaticTemperatureK,adiabatic[species],error) ) return false;
			}
			return true;
		}

		inline double InequalityValue(
			const ConservativeVector& state,
			const std::size_t inequality,
			const std::array<double,MethaneSpeciesCount>& ambientEnthalpy,
			const std::array<double,MethaneSpeciesCount>& adiabaticEnthalpy
			)
		{
			if( inequality == 0 ) return -state[0];
			if( inequality == 1 ) {
				double result = state[0];
				for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
					result -= state[1+species];
				}
				return result;
			}
			if( inequality < 2+MethaneSpeciesCount ) return -state[inequality-1];
			if( inequality == 2+MethaneSpeciesCount ) {
				double result = -state[MethaneMassStateDimension];
				for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
					result += ambientEnthalpy[species]*state[1+species];
				}
				return result;
			}
			double result = state[MethaneMassStateDimension];
			for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
				result -= adiabaticEnthalpy[species]*state[1+species];
			}
			return result;
		}

		inline bool CertifiedMassConstraintSatisfied(
			const ConservativeVector& state,
			const FireCertifiedNullspace& closure
			)
		{
			double matrixNorm = 0.0, stateNorm = 0.0, residualNorm = 0.0;
			for( std::size_t column=0; column<closure.stateDimension; ++column ) {
				stateNorm = std::max(stateNorm,std::fabs(state[column]));
			}
			for( std::size_t row=0; row<closure.constraintRows; ++row ) {
				double rowNorm = 0.0, residual = 0.0;
				for( std::size_t column=0; column<closure.stateDimension; ++column ) {
					const double coefficient = closure.constraintMatrix[
						row*closure.stateDimension+column];
					rowNorm += std::fabs(coefficient);
					residual += coefficient*state[column];
				}
				matrixNorm = std::max(matrixNorm,rowNorm);
				residualNorm = std::max(residualNorm,std::fabs(residual));
			}
			const double bound = 4096.0*std::numeric_limits<double>::epsilon()*
				std::max(1.0,matrixNorm*stateNorm);
			return residualNorm <= bound;
		}

		inline bool ApplyPeriodicSharedFCT(
			const std::vector<ConservativeVector>& beginning,
			const PeriodicFluxPair& flux,
			const std::vector<ConservativeVector>& sourcePerS,
			const PeriodicTransportConfig& config,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			std::vector<ConservativeVector>& result,
			std::vector<double>& faceAlpha,
			std::string* error = 0
			)
		{
			const std::size_t count = beginning.size();
			if( count < 3 || flux.low.size() != count || flux.high.size() != count ||
				sourcePerS.size() != count || !std::isfinite(config.deltaTimeS) ||
				config.deltaTimeS <= 0.0 || !std::isfinite(config.cellWidthM) ||
				config.cellWidthM <= 0.0 ) {
				return Fail(error,"fire solver FCT input is malformed");
			}
			std::array<double,MethaneSpeciesCount> ambientEnthalpy, adiabaticEnthalpy;
			if( !FireSimulationEnthalpyBounds(config,thermochemistry,
				ambientEnthalpy,adiabaticEnthalpy,error) ) return false;
			std::vector<ConservativeVector> low(count), leftCorrection(count), rightCorrection(count);
			const double scale = config.deltaTimeS/config.cellWidthM;
			for( std::size_t cell=0; cell<count; ++cell ) {
				if( !CertifiedMassConstraintSatisfied(beginning[cell],
					fuel.ConservativeReconstruction()) ) {
					return Fail(error,"fire solver FCT input violates the certified physical affine invariant");
				}
				const std::size_t leftFace = (cell+count-1)%count;
				const std::size_t rightFace = cell;
				low[cell] = beginning[cell]+config.deltaTimeS*sourcePerS[cell]+
					scale*(flux.low[leftFace]-flux.low[rightFace]);
				leftCorrection[cell] = scale*(flux.high[leftFace]-flux.low[leftFace]);
				rightCorrection[cell] = -scale*(flux.high[rightFace]-flux.low[rightFace]);
				const double tolerance = 256.0*std::numeric_limits<double>::epsilon()*
					std::max(1.0,std::fabs(low[cell][MethaneMassStateDimension]));
				if( !ConservativeStateFeasible(low[cell],ambientEnthalpy,
					adiabaticEnthalpy,tolerance) ) {
					return Fail(error,"fire solver low-order FCT state is infeasible");
				}
				if( !CertifiedMassConstraintSatisfied(low[cell],
					fuel.ConservativeReconstruction()) ) {
					return Fail(error,"fire solver low-order FCT state violates the certified affine invariant");
				}
			}
			const std::size_t inequalityCount = 4+MethaneSpeciesCount;
			std::vector<std::vector<double> > ratio(count,
				std::vector<double>(inequalityCount,1.0));
			for( std::size_t cell=0; cell<count; ++cell ) {
				for( std::size_t inequality=0; inequality<inequalityCount; ++inequality ) {
					const double budget = std::max(0.0,-InequalityValue(low[cell],inequality,
						ambientEnthalpy,adiabaticEnthalpy));
					const double leftUse = InequalityValue(leftCorrection[cell],inequality,
						ambientEnthalpy,adiabaticEnthalpy);
					const double rightUse = InequalityValue(rightCorrection[cell],inequality,
						ambientEnthalpy,adiabaticEnthalpy);
					const double requested = std::max(0.0,leftUse)+std::max(0.0,rightUse);
					ratio[cell][inequality] = requested > 0.0 ?
						std::min(1.0,budget/requested) : 1.0;
				}
			}
			faceAlpha.assign(count,1.0);
			for( std::size_t face=0; face<count; ++face ) {
				const std::size_t left = face;
				const std::size_t right = (face+1)%count;
				for( std::size_t inequality=0; inequality<inequalityCount; ++inequality ) {
					if( InequalityValue(rightCorrection[left],inequality,
						ambientEnthalpy,adiabaticEnthalpy) > 0.0 ) {
						faceAlpha[face] = std::min(faceAlpha[face],ratio[left][inequality]);
					}
					if( InequalityValue(leftCorrection[right],inequality,
						ambientEnthalpy,adiabaticEnthalpy) > 0.0 ) {
						faceAlpha[face] = std::min(faceAlpha[face],ratio[right][inequality]);
					}
				}
			}
			result = low;
			for( std::size_t face=0; face<count; ++face ) {
				const std::size_t right = (face+1)%count;
				const ConservativeVector correction = scale*faceAlpha[face]*
					(flux.high[face]-flux.low[face]);
				result[face] = result[face]-correction;
				result[right] = result[right]+correction;
			}
			for( std::size_t cell=0; cell<count; ++cell ) {
				const double tolerance = 1024.0*std::numeric_limits<double>::epsilon()*
					std::max(1.0,std::fabs(result[cell][MethaneMassStateDimension]));
				if( !ConservativeStateFeasible(result[cell],ambientEnthalpy,
					adiabaticEnthalpy,tolerance) ) {
					return Fail(error,"fire solver shared FCT result violates a nodal budget");
				}
				if( !CertifiedMassConstraintSatisfied(result[cell],
					fuel.ConservativeReconstruction()) ) {
					return Fail(error,"fire solver shared FCT result violates the certified affine invariant");
				}
			}
			return true;
		}

		inline bool InvertPeriodicTemperatures(
			const std::vector<ConservativeVector>& cells,
			const FireSimulationMethaneRecord& thermochemistry,
			std::vector<double>& temperatureK,
			std::string* error = 0
			)
		{
			temperatureK.assign(cells.size(),0.0);
			for( std::size_t cell=0; cell<cells.size(); ++cell ) {
				MethaneCellState state = FromConservativeVector(cells[cell]);
				std::string inversionError;
				std::vector<std::pair<std::string,double> > propertyDensities;
				if( !ThermochemicalDensitiesWithinForwardEnvelope(state,propertyDensities,
					&inversionError) ||
					!thermochemistry.InvertMixtureTemperatureK(propertyDensities,
					state.sensibleEnergyJPerM3,temperatureK[cell],&inversionError) ) {
					std::ostringstream message;
					message << "fire solver cell " << cell << " temperature inversion failed: "
						<< inversionError << "; constituents=";
					for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
						message << (species ? "," : "") << state.constituent[species];
					}
					return Fail(error,message.str());
				}
				state.temperatureK = temperatureK[cell];
				double equationOfStateResidual = 0.0;
				if( !EquationOfStateResidual(state,thermochemistry,
					equationOfStateResidual,&inversionError) ||
					equationOfStateResidual > 1.0e-3 ) {
					std::ostringstream message;
					message << "fire solver cell " << cell
						<< " violates the accepted-state EOS gate: residual="
						<< equationOfStateResidual << "; " << inversionError;
					return Fail(error,message.str());
				}
			}
			return true;
		}

		inline bool DivergenceFromDiscreteRate(
			const ConservativeVector& stateVector,
			const ConservativeVector& nonadvectiveAndSourceRate,
			const double temperatureK,
			const FireSimulationMethaneRecord& thermochemistry,
			double& result,
			std::string* error = 0
			)
		{
			static const char* names[MethaneSpeciesCount] = {
				"CH4", "O2", "N2", "CO2", "H2O", "CO", "C(gr)"
			};
			const MethaneCellState state = FromConservativeVector(stateVector);
			double gasDensity = 0.0, inverseMeanWeightSum = 0.0;
			for( std::size_t species=0; species<MethaneCarbon; ++species ) {
				const FireThermochemistrySpecies* property = thermochemistry.FindSpecies(names[species]);
				if( !property ) return Fail(error,"fire solver divergence identity lacks a gas species");
				const double density = std::max(0.0,state.constituent[species]);
				gasDensity += density;
				inverseMeanWeightSum += density/property->molecularWeightKGPerKMol;
			}
			if( gasDensity <= 0.0 || inverseMeanWeightSum <= 0.0 ||
				!std::isfinite(temperatureK) || temperatureK <= 0.0 ) {
				return Fail(error,"fire solver divergence identity has an invalid gas state");
			}
			const double meanWeight = gasDensity/inverseMeanWeightSum;
			double heatCapacity = 0.0;
			std::array<double,MethaneSpeciesCount> enthalpy = {};
			for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
				double cp = 0.0;
				if( !thermochemistry.CpJPerKGK(names[species],temperatureK,cp,error) ||
					!thermochemistry.SensibleEnthalpyJPerKG(names[species],temperatureK,
						enthalpy[species],error) ) return false;
				heatCapacity += std::max(0.0,state.constituent[species])*cp;
			}
			if( heatCapacity <= 0.0 || !std::isfinite(heatCapacity) ) {
				return Fail(error,"fire solver divergence identity lacks positive C_T");
			}
			const double heatCapacityTemperature = heatCapacity*temperatureK;
			result = nonadvectiveAndSourceRate[MethaneMassStateDimension]/
				heatCapacityTemperature;
			for( std::size_t species=0; species<MethaneCarbon; ++species ) {
				const FireThermochemistrySpecies* property = thermochemistry.FindSpecies(names[species]);
				result += (meanWeight/(gasDensity*property->molecularWeightKGPerKMol)-
					enthalpy[species]/heatCapacityTemperature)*
					nonadvectiveAndSourceRate[1+species];
			}
			result -= enthalpy[MethaneCarbon]/heatCapacityTemperature*
				nonadvectiveAndSourceRate[1+MethaneCarbon];
			return std::isfinite(result) ||
				Fail(error,"fire solver divergence identity overflowed");
		}

		inline bool PeriodicDivergenceTargetFromPhysicalFlux(
			const std::vector<ConservativeVector>& state,
			const std::vector<double>& temperatureK,
			const PeriodicFluxPair& flux,
			const std::vector<ConservativeVector>& frozenSourcePerS,
			const double cellWidthM,
			const FireSimulationMethaneRecord& thermochemistry,
			std::vector<double>& result,
			std::string* error = 0
			)
		{
			const std::size_t count = state.size();
			if( count < 3 || temperatureK.size() != count || flux.nonadvectiveMass.size() != count ||
				flux.nonadvectiveEnergy.size() != count || frozenSourcePerS.size() != count ) {
				return Fail(error,"fire solver divergence target arrays are malformed");
			}
			result.assign(count,0.0);
			for( std::size_t cell=0; cell<count; ++cell ) {
				const std::size_t leftFace = (cell+count-1)%count;
				ConservativeVector rate = frozenSourcePerS[cell];
				for( std::size_t component=0; component<MethaneMassStateDimension; ++component ) {
					rate[component] += (flux.nonadvectiveMass[leftFace][component]-
						flux.nonadvectiveMass[cell][component])/cellWidthM;
				}
				rate[MethaneMassStateDimension] += (flux.nonadvectiveEnergy[leftFace]-
					flux.nonadvectiveEnergy[cell])/cellWidthM;
				if( !DivergenceFromDiscreteRate(state[cell],rate,temperatureK[cell],
					thermochemistry,result[cell],error) ) return false;
			}
			return true;
		}

		inline bool AdvancePeriodicTransportHeun(
			const std::vector<ConservativeVector>& beginning,
			const std::vector<double>& faceVelocityMPerS,
			const std::vector<double>& diffusivityM2PerS,
			const std::vector<double>& conductivityWPerMK,
			const std::vector<ConservativeVector>& frozenSourcePerS,
			const PeriodicTransportConfig& config,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			std::vector<ConservativeVector>& result,
			std::vector<double>& acceptedAlpha,
			std::string* error = 0
			)
		{
			std::vector<double> temperature0, temperature1;
			if( !InvertPeriodicTemperatures(beginning,thermochemistry,temperature0,error) ) return false;
			PeriodicFluxPair flux0;
			if( !BuildPeriodicFluxPair(beginning,temperature0,faceVelocityMPerS,
				diffusivityM2PerS,conductivityWPerMK,config.cellWidthM,fuel,
				thermochemistry,flux0,error) ) return false;
			std::vector<ConservativeVector> predictor;
			std::vector<double> predictorAlpha;
			if( !ApplyPeriodicSharedFCT(beginning,flux0,frozenSourcePerS,config,
				fuel,thermochemistry,predictor,predictorAlpha,error) ||
				!InvertPeriodicTemperatures(predictor,thermochemistry,temperature1,error) ) return false;
			PeriodicFluxPair flux1;
			if( !BuildPeriodicFluxPair(predictor,temperature1,faceVelocityMPerS,
				diffusivityM2PerS,conductivityWPerMK,config.cellWidthM,fuel,
				thermochemistry,flux1,error) ) return false;
			PeriodicFluxPair averaged;
			averaged.low.resize(beginning.size());
			averaged.high.resize(beginning.size());
			averaged.nonadvectiveMass.resize(beginning.size());
			averaged.nonadvectiveEnergy.resize(beginning.size());
			for( std::size_t face=0; face<beginning.size(); ++face ) {
				averaged.low[face] = 0.5*(flux0.low[face]+flux1.low[face]);
				averaged.high[face] = 0.5*(flux0.high[face]+flux1.high[face]);
				averaged.nonadvectiveEnergy[face] = 0.5*(
					flux0.nonadvectiveEnergy[face]+flux1.nonadvectiveEnergy[face]);
				for( std::size_t component=0; component<MethaneMassStateDimension; ++component ) {
					averaged.nonadvectiveMass[face][component] = 0.5*(
						flux0.nonadvectiveMass[face][component]+
						flux1.nonadvectiveMass[face][component]);
				}
			}
			return ApplyPeriodicSharedFCT(beginning,averaged,frozenSourcePerS,config,
				fuel,thermochemistry,result,acceptedAlpha,error);
		}

		struct PeriodicProjectionResult
		{
			std::vector<double> faceDensityKGPerM3;
			std::vector<double> velocityMPerS;
			std::vector<double> momentumKGPerM2S;
			std::vector<double> pressureImpulsePa;
			std::vector<double> residualHistoryPerS;
		};

		struct PeriodicMACShape
		{
			std::size_t nx, ny, nz;
			double cellWidthM;
			PeriodicMACShape() : nx(0), ny(0), nz(0), cellWidthM(0.0) {}
			std::size_t CellCount() const { return nx*ny*nz; }
			std::size_t Index( const std::size_t x, const std::size_t y,
				const std::size_t z ) const { return (z*ny+y)*nx+x; }
		};

		struct PeriodicMACField
		{
			std::array<std::vector<double>,3> component;
		};

		struct PeriodicMACProjection3DResult
		{
			PeriodicMACField faceDensityKGPerM3;
			PeriodicMACField velocityMPerS;
			PeriodicMACField momentumKGPerM2S;
			std::vector<double> stepAverageDynamicPressurePa;
			std::vector<double> residualHistoryPerS;
		};

		inline double Dot( const std::vector<double>& first,
			const std::vector<double>& second );
		inline void RemoveMean( std::vector<double>& values );

		inline std::size_t PeriodicPrevious(
			const PeriodicMACShape& shape,
			const std::size_t cell,
			const unsigned int axis
			)
		{
			const std::size_t x = cell%shape.nx;
			const std::size_t y = (cell/shape.nx)%shape.ny;
			const std::size_t z = cell/(shape.nx*shape.ny);
			if( axis == 0 ) return shape.Index((x+shape.nx-1)%shape.nx,y,z);
			if( axis == 1 ) return shape.Index(x,(y+shape.ny-1)%shape.ny,z);
			return shape.Index(x,y,(z+shape.nz-1)%shape.nz);
		}

		inline std::size_t PeriodicNext(
			const PeriodicMACShape& shape,
			const std::size_t cell,
			const unsigned int axis
			)
		{
			const std::size_t x = cell%shape.nx;
			const std::size_t y = (cell/shape.nx)%shape.ny;
			const std::size_t z = cell/(shape.nx*shape.ny);
			if( axis == 0 ) return shape.Index((x+1)%shape.nx,y,z);
			if( axis == 1 ) return shape.Index(x,(y+1)%shape.ny,z);
			return shape.Index(x,y,(z+1)%shape.nz);
		}

		inline double PeriodicMACDivergence3D(
			const PeriodicMACShape& shape,
			const PeriodicMACField& field,
			const std::size_t cell
			)
		{
			double result = 0.0;
			for( unsigned int axis=0; axis<3; ++axis ) {
				result += (field.component[axis][cell]-field.component[axis][
					PeriodicPrevious(shape,cell,axis)])/shape.cellWidthM;
			}
			return result;
		}

		inline void ApplyNegativePeriodicMACPoisson3D(
			const PeriodicMACShape& shape,
			const PeriodicMACField& inverseFaceDensity,
			const std::vector<double>& input,
			std::vector<double>& output
			)
		{
			const std::size_t count = shape.CellCount();
			const double inverseWidth2 = 1.0/(shape.cellWidthM*shape.cellWidthM);
			output.assign(count,0.0);
			for( std::size_t cell=0; cell<count; ++cell ) {
				for( unsigned int axis=0; axis<3; ++axis ) {
					const std::size_t previous = PeriodicPrevious(shape,cell,axis);
					const std::size_t next = PeriodicNext(shape,cell,axis);
					output[cell] += inverseWidth2*(inverseFaceDensity.component[axis][cell]*
						(input[cell]-input[next])+inverseFaceDensity.component[axis][previous]*
						(input[cell]-input[previous]));
				}
			}
		}

		inline void SmoothPeriodicMACPoisson3D(
			const PeriodicMACShape& shape,
			const PeriodicMACField& inverseFaceDensity,
			const std::vector<double>& rightHandSide,
			const std::size_t iterations,
			std::vector<double>& solution
			)
		{
			const std::size_t count = shape.CellCount();
			const double inverseWidth2 = 1.0/(shape.cellWidthM*shape.cellWidthM);
			std::vector<double> applied, next(count,0.0);
			for( std::size_t iteration=0; iteration<iterations; ++iteration ) {
				ApplyNegativePeriodicMACPoisson3D(shape,inverseFaceDensity,solution,applied);
				for( std::size_t cell=0; cell<count; ++cell ) {
					double diagonal = 0.0;
					for( unsigned int axis=0; axis<3; ++axis ) diagonal += inverseWidth2*(
						inverseFaceDensity.component[axis][cell]+inverseFaceDensity.component[axis][
							PeriodicPrevious(shape,cell,axis)]);
					next[cell] = solution[cell]+(2.0/3.0)*(rightHandSide[cell]-applied[cell])/diagonal;
				}
				solution.swap(next);
				RemoveMean(solution);
			}
		}

		inline bool CoarsenPeriodicMACLevel(
			const PeriodicMACShape& fineShape,
			const PeriodicMACField& fineCoefficient,
			PeriodicMACShape& coarseShape,
			PeriodicMACField& coarseCoefficient
			)
		{
			if( fineShape.nx%2 || fineShape.ny%2 || fineShape.nz%2 ||
				fineShape.nx < 4 || fineShape.ny < 4 || fineShape.nz < 4 ) return false;
			coarseShape.nx = fineShape.nx/2;
			coarseShape.ny = fineShape.ny/2;
			coarseShape.nz = fineShape.nz/2;
			coarseShape.cellWidthM = 2.0*fineShape.cellWidthM;
			for( unsigned int axis=0; axis<3; ++axis ) {
				coarseCoefficient.component[axis].assign(coarseShape.CellCount(),0.0);
			}
			for( std::size_t z=0; z<coarseShape.nz; ++z ) for( std::size_t y=0;
				y<coarseShape.ny; ++y ) for( std::size_t x=0; x<coarseShape.nx; ++x ) {
				const std::size_t coarse = coarseShape.Index(x,y,z);
				for( unsigned int axis=0; axis<3; ++axis ) {
					double sum = 0.0;
					for( std::size_t first=0; first<2; ++first ) for( std::size_t second=0;
						second<2; ++second ) {
						std::size_t fx=2*x, fy=2*y, fz=2*z;
						if( axis == 0 ) { fx += 1; fy += first; fz += second; }
						if( axis == 1 ) { fy += 1; fx += first; fz += second; }
						if( axis == 2 ) { fz += 1; fx += first; fy += second; }
						sum += fineCoefficient.component[axis][fineShape.Index(fx,fy,fz)];
					}
					coarseCoefficient.component[axis][coarse] = 0.25*sum;
				}
			}
			return true;
		}

		inline void RestrictPeriodicResidual3D(
			const PeriodicMACShape& fineShape,
			const PeriodicMACShape& coarseShape,
			const std::vector<double>& fine,
			std::vector<double>& coarse
			)
		{
			coarse.assign(coarseShape.CellCount(),0.0);
			for( std::size_t z=0; z<coarseShape.nz; ++z ) for( std::size_t y=0;
				y<coarseShape.ny; ++y ) for( std::size_t x=0; x<coarseShape.nx; ++x ) {
				double sum = 0.0;
				for( std::size_t dz=0; dz<2; ++dz ) for( std::size_t dy=0; dy<2; ++dy )
					for( std::size_t dx=0; dx<2; ++dx ) sum += fine[fineShape.Index(
						2*x+dx,2*y+dy,2*z+dz)];
				coarse[coarseShape.Index(x,y,z)] = 0.125*sum;
			}
			RemoveMean(coarse);
		}

		inline void ProlongPeriodicCorrection3D(
			const PeriodicMACShape& fineShape,
			const PeriodicMACShape& coarseShape,
			const std::vector<double>& coarse,
			std::vector<double>& fine
			)
		{
			for( std::size_t z=0; z<fineShape.nz; ++z ) for( std::size_t y=0;
				y<fineShape.ny; ++y ) for( std::size_t x=0; x<fineShape.nx; ++x ) {
				const std::size_t cx=x/2, cy=y/2, cz=z/2;
				const double tx = (x%2)*0.5, ty = (y%2)*0.5, tz = (z%2)*0.5;
				double value = 0.0;
				for( std::size_t dz=0; dz<2; ++dz ) for( std::size_t dy=0; dy<2; ++dy )
					for( std::size_t dx=0; dx<2; ++dx ) value +=
						(dx ? tx : 1.0-tx)*(dy ? ty : 1.0-ty)*(dz ? tz : 1.0-tz)*
						coarse[coarseShape.Index((cx+dx)%coarseShape.nx,
							(cy+dy)%coarseShape.ny,(cz+dz)%coarseShape.nz)];
				fine[fineShape.Index(x,y,z)] += value;
			}
			RemoveMean(fine);
		}

		inline void PeriodicMACMultigridVCycle3D(
			const PeriodicMACShape& shape,
			const PeriodicMACField& coefficient,
			const std::vector<double>& rightHandSide,
			std::vector<double>& solution
			)
		{
			PeriodicMACShape coarseShape;
			PeriodicMACField coarseCoefficient;
			if( !CoarsenPeriodicMACLevel(shape,coefficient,coarseShape,coarseCoefficient) ) {
				SmoothPeriodicMACPoisson3D(shape,coefficient,rightHandSide,40,solution);
				return;
			}
			SmoothPeriodicMACPoisson3D(shape,coefficient,rightHandSide,4,solution);
			std::vector<double> applied, residual(shape.CellCount(),0.0), coarseRight;
			ApplyNegativePeriodicMACPoisson3D(shape,coefficient,solution,applied);
			for( std::size_t cell=0; cell<shape.CellCount(); ++cell ) {
				residual[cell] = rightHandSide[cell]-applied[cell];
			}
			RestrictPeriodicResidual3D(shape,coarseShape,residual,coarseRight);
			std::vector<double> coarseCorrection(coarseShape.CellCount(),0.0);
			PeriodicMACMultigridVCycle3D(coarseShape,coarseCoefficient,coarseRight,
				coarseCorrection);
			ProlongPeriodicCorrection3D(shape,coarseShape,coarseCorrection,solution);
			SmoothPeriodicMACPoisson3D(shape,coefficient,rightHandSide,4,solution);
		}

		inline bool ProjectPeriodicMACVelocity3D(
			const PeriodicMACShape& shape,
			const std::vector<double>& gasDensityKGPerM3,
			const PeriodicMACField& unprojectedMomentumKGPerM2S,
			const std::vector<double>& divergenceTargetPerS,
			const double deltaTimeS,
			const double absoluteTolerancePerS,
			PeriodicMACProjection3DResult& result,
			std::string* error = 0
			)
		{
			if( shape.nx < 2 || shape.ny < 2 || shape.nz < 2 ||
				shape.nx > std::numeric_limits<std::size_t>::max()/shape.ny ||
				shape.nx*shape.ny > std::numeric_limits<std::size_t>::max()/shape.nz ||
				!std::isfinite(shape.cellWidthM) || shape.cellWidthM <= 0.0 ||
				!std::isfinite(deltaTimeS) || deltaTimeS <= 0.0 ||
				!std::isfinite(absoluteTolerancePerS) || absoluteTolerancePerS <= 0.0 ) {
				return Fail(error,"fire solver 3-D periodic MAC projection input is malformed");
			}
			const std::size_t count = shape.CellCount();
			if( gasDensityKGPerM3.size() != count || divergenceTargetPerS.size() != count ) {
				return Fail(error,"fire solver 3-D periodic MAC projection array shape is invalid");
			}
			for( unsigned int axis=0; axis<3; ++axis ) {
				if( unprojectedMomentumKGPerM2S.component[axis].size() != count ) {
					return Fail(error,"fire solver 3-D periodic MAC momentum shape is invalid");
				}
				result.faceDensityKGPerM3.component[axis].assign(count,0.0);
				result.velocityMPerS.component[axis].assign(count,0.0);
				result.momentumKGPerM2S.component[axis] =
					unprojectedMomentumKGPerM2S.component[axis];
			}
			PeriodicMACField inverseFaceDensity;
			for( unsigned int axis=0; axis<3; ++axis ) {
				inverseFaceDensity.component[axis].assign(count,0.0);
			}
			double targetMean = 0.0, targetMaximum = 0.0;
			for( std::size_t cell=0; cell<count; ++cell ) {
				if( !std::isfinite(gasDensityKGPerM3[cell]) || gasDensityKGPerM3[cell] <= 0.0 ||
					!std::isfinite(divergenceTargetPerS[cell]) ) {
					return Fail(error,"fire solver 3-D periodic MAC state is invalid");
				}
				targetMean += divergenceTargetPerS[cell];
				targetMaximum = std::max(targetMaximum,std::fabs(divergenceTargetPerS[cell]));
				for( unsigned int axis=0; axis<3; ++axis ) {
					const std::size_t next = PeriodicNext(shape,cell,axis);
					const double faceDensity = 0.5*(gasDensityKGPerM3[cell]+
						gasDensityKGPerM3[next]);
					const double momentum = unprojectedMomentumKGPerM2S.component[axis][cell];
					if( !std::isfinite(momentum) ) {
						return Fail(error,"fire solver 3-D periodic MAC momentum is non-finite");
					}
					result.faceDensityKGPerM3.component[axis][cell] = faceDensity;
					inverseFaceDensity.component[axis][cell] = 1.0/faceDensity;
					result.velocityMPerS.component[axis][cell] = momentum/faceDensity;
				}
			}
			targetMean /= static_cast<double>(count);
			if( std::fabs(targetMean) > 128.0*std::numeric_limits<double>::epsilon()*
				std::max(1.0,targetMaximum) ) {
				return Fail(error,"fire solver 3-D periodic divergence target violates compatibility");
			}
			std::vector<double> rightHandSide(count,0.0);
			for( std::size_t cell=0; cell<count; ++cell ) {
				rightHandSide[cell] = -(PeriodicMACDivergence3D(shape,
					result.velocityMPerS,cell)-divergenceTargetPerS[cell])/deltaTimeS;
			}
			RemoveMean(rightHandSide);
			result.stepAverageDynamicPressurePa.assign(count,0.0);
			const double pressureTolerance = absoluteTolerancePerS/deltaTimeS;
			result.residualHistoryPerS.clear();
			std::vector<double> applied, residual(count,0.0);
			double residualNorm = std::numeric_limits<double>::infinity();
			for( std::size_t cycle=0; cycle<128 && residualNorm > pressureTolerance; ++cycle ) {
				PeriodicMACMultigridVCycle3D(shape,inverseFaceDensity,rightHandSide,
					result.stepAverageDynamicPressurePa);
				ApplyNegativePeriodicMACPoisson3D(shape,inverseFaceDensity,
					result.stepAverageDynamicPressurePa,applied);
				double residualSquared = 0.0;
				for( std::size_t cell=0; cell<count; ++cell ) {
					residual[cell] = rightHandSide[cell]-applied[cell];
					residualSquared += residual[cell]*residual[cell];
				}
				RemoveMean(residual);
				residualNorm = std::sqrt(residualSquared);
				result.residualHistoryPerS.push_back(deltaTimeS*
					residualNorm/std::sqrt(static_cast<double>(count)));
				if( !std::isfinite(residualNorm) ) return Fail(error,
					"fire solver 3-D geometric multigrid residual overflowed");
			}
			if( residualNorm > pressureTolerance ) {
				return Fail(error,"fire solver 3-D periodic pressure solve did not converge");
			}
			for( std::size_t cell=0; cell<count; ++cell ) {
				for( unsigned int axis=0; axis<3; ++axis ) {
					const std::size_t next = PeriodicNext(shape,cell,axis);
					const double gradient = (result.stepAverageDynamicPressurePa[next]-
						result.stepAverageDynamicPressurePa[cell])/shape.cellWidthM;
					result.momentumKGPerM2S.component[axis][cell] -= deltaTimeS*gradient;
					result.velocityMPerS.component[axis][cell] =
						result.momentumKGPerM2S.component[axis][cell]/
						result.faceDensityKGPerM3.component[axis][cell];
				}
			}
			double maximumResidual = 0.0;
			for( std::size_t cell=0; cell<count; ++cell ) {
				maximumResidual = std::max(maximumResidual,std::fabs(
					PeriodicMACDivergence3D(shape,result.velocityMPerS,cell)-
					divergenceTargetPerS[cell]));
			}
			return maximumResidual <= absoluteTolerancePerS ||
				Fail(error,"fire solver accepted 3-D velocity misses its divergence target");
		}

		inline double PeriodicDivergence(
			const std::vector<double>& faceValue,
			const std::size_t cell,
			const double cellWidthM
			)
		{
			const std::size_t count = faceValue.size();
			return (faceValue[cell]-faceValue[(cell+count-1)%count])/cellWidthM;
		}

		inline void ApplyNegativePeriodicPoisson(
			const std::vector<double>& inverseFaceDensity,
			const std::vector<double>& input,
			const double cellWidthM,
			std::vector<double>& output
			)
		{
			const std::size_t count = input.size();
			const double inverseWidth2 = 1.0/(cellWidthM*cellWidthM);
			output.assign(count,0.0);
			for( std::size_t cell=0; cell<count; ++cell ) {
				const std::size_t previous = (cell+count-1)%count;
				const std::size_t next = (cell+1)%count;
				output[cell] = inverseWidth2*(
					inverseFaceDensity[cell]*(input[cell]-input[next])+
					inverseFaceDensity[previous]*(input[cell]-input[previous]));
			}
		}

		inline double Dot( const std::vector<double>& first, const std::vector<double>& second )
		{
			double result = 0.0;
			for( std::size_t index=0; index<first.size(); ++index ) result += first[index]*second[index];
			return result;
		}

		inline void RemoveMean( std::vector<double>& values )
		{
			double mean = 0.0;
			for( const double value : values ) mean += value;
			mean /= static_cast<double>(values.size());
			for( double& value : values ) value -= mean;
		}

		inline bool ProjectPeriodicMACVelocity(
			const std::vector<double>& gasDensityKGPerM3,
			const std::vector<double>& unprojectedMomentumKGPerM2S,
			const std::vector<double>& divergenceTargetPerS,
			const double cellWidthM,
			const double deltaTimeS,
			const double absoluteTolerancePerS,
			PeriodicProjectionResult& result,
			std::string* error = 0
			)
		{
			const std::size_t count = gasDensityKGPerM3.size();
			if( count < 3 || unprojectedMomentumKGPerM2S.size() != count ||
				divergenceTargetPerS.size() != count || !std::isfinite(cellWidthM) ||
				cellWidthM <= 0.0 || !std::isfinite(deltaTimeS) || deltaTimeS <= 0.0 ||
				!std::isfinite(absoluteTolerancePerS) || absoluteTolerancePerS <= 0.0 ) {
				return Fail(error,"fire solver periodic MAC projection input is malformed");
			}
			double targetMean = 0.0, targetMaximum = 0.0;
			for( std::size_t cell=0; cell<count; ++cell ) {
				if( !std::isfinite(gasDensityKGPerM3[cell]) || gasDensityKGPerM3[cell] <= 0.0 ||
					!std::isfinite(unprojectedMomentumKGPerM2S[cell]) ||
					!std::isfinite(divergenceTargetPerS[cell]) ) {
					return Fail(error,"fire solver periodic MAC projection state is invalid");
				}
				targetMean += divergenceTargetPerS[cell];
				targetMaximum = std::max(targetMaximum,std::fabs(divergenceTargetPerS[cell]));
			}
			targetMean /= static_cast<double>(count);
			const double compatibilityTolerance = 64.0*std::numeric_limits<double>::epsilon()*
				std::max(1.0,targetMaximum);
			if( std::fabs(targetMean) > compatibilityTolerance ) {
				std::ostringstream message;
				message << "fire solver periodic divergence target violates the zero-integral compatibility condition: "
					<< targetMean;
				return Fail(error,message.str());
			}
			result.faceDensityKGPerM3.assign(count,0.0);
			result.velocityMPerS.assign(count,0.0);
			std::vector<double> inverseFaceDensity(count,0.0), rightHandSide(count,0.0);
			for( std::size_t face=0; face<count; ++face ) {
				const std::size_t right = (face+1)%count;
				result.faceDensityKGPerM3[face] = 0.5*(gasDensityKGPerM3[face]+
					gasDensityKGPerM3[right]);
				inverseFaceDensity[face] = 1.0/result.faceDensityKGPerM3[face];
				result.velocityMPerS[face] = unprojectedMomentumKGPerM2S[face]*
					inverseFaceDensity[face];
			}
			for( std::size_t cell=0; cell<count; ++cell ) {
				rightHandSide[cell] = -(PeriodicDivergence(result.velocityMPerS,cell,
					cellWidthM)-divergenceTargetPerS[cell])/deltaTimeS;
			}
			RemoveMean(rightHandSide);
			result.pressureImpulsePa.assign(count,0.0);
			std::vector<double> residual = rightHandSide;
			std::vector<double> direction = residual, operatorDirection;
			double residualSquared = Dot(residual,residual);
			result.residualHistoryPerS.clear();
			const double rightNorm = std::sqrt(residualSquared);
			const double pressureTolerance = absoluteTolerancePerS/deltaTimeS;
			for( std::size_t iteration=0; iteration<8*count &&
				std::sqrt(residualSquared) > pressureTolerance;
				++iteration ) {
				ApplyNegativePeriodicPoisson(inverseFaceDensity,direction,cellWidthM,
					operatorDirection);
				const double denominator = Dot(direction,operatorDirection);
				if( !std::isfinite(denominator) || denominator <= 0.0 ) {
					return Fail(error,"fire solver periodic pressure operator lost positive definiteness");
				}
				const double alpha = residualSquared/denominator;
				for( std::size_t cell=0; cell<count; ++cell ) {
					result.pressureImpulsePa[cell] += alpha*direction[cell];
					residual[cell] -= alpha*operatorDirection[cell];
				}
				RemoveMean(result.pressureImpulsePa);
				RemoveMean(residual);
				const double nextResidualSquared = Dot(residual,residual);
				result.residualHistoryPerS.push_back(deltaTimeS*
					std::sqrt(nextResidualSquared/static_cast<double>(count)));
				if( nextResidualSquared == 0.0 ) { residualSquared = 0.0; break; }
				const double beta = nextResidualSquared/residualSquared;
				for( std::size_t cell=0; cell<count; ++cell ) {
					direction[cell] = residual[cell]+beta*direction[cell];
				}
				RemoveMean(direction);
				residualSquared = nextResidualSquared;
			}
			if( rightNorm > 0.0 && std::sqrt(residualSquared) >
				pressureTolerance ) {
				return Fail(error,"fire solver periodic pressure solve did not converge");
			}
			result.momentumKGPerM2S = unprojectedMomentumKGPerM2S;
			for( std::size_t face=0; face<count; ++face ) {
				const std::size_t right = (face+1)%count;
				const double gradient = (result.pressureImpulsePa[right]-
					result.pressureImpulsePa[face])/cellWidthM;
				result.momentumKGPerM2S[face] -= deltaTimeS*gradient;
				result.velocityMPerS[face] = result.momentumKGPerM2S[face]/
					result.faceDensityKGPerM3[face];
			}
			double maximumResidual = 0.0;
			for( std::size_t cell=0; cell<count; ++cell ) {
				maximumResidual = std::max(maximumResidual,std::fabs(
					PeriodicDivergence(result.velocityMPerS,cell,cellWidthM)-
					divergenceTargetPerS[cell]));
			}
			if( maximumResidual > absoluteTolerancePerS ) {
				std::ostringstream message;
				message << "fire solver accepted periodic velocity misses its divergence target: "
					<< maximumResidual << " > " << absoluteTolerancePerS;
				return Fail(error,message.str());
			}
			return true;
		}

		struct OpenMACProjection1DResult
		{
			std::vector<double> faceDensityKGPerM3;
			std::vector<double> velocityMPerS;
			std::vector<double> momentumKGPerM2S;
			std::vector<double> dynamicPressurePa;
			bool leftInflow;
			bool rightInflow;
			double maximumDivergenceResidualPerS;
			double maximumBoundaryHeadResidualPa;
			double leftBoundaryPressurePa;
			double rightBoundaryPressurePa;
			std::vector<double> nonlinearResidualHistory;
			OpenMACProjection1DResult() : leftInflow(false), rightInflow(false),
				maximumDivergenceResidualPerS(0.0),maximumBoundaryHeadResidualPa(0.0),
				leftBoundaryPressurePa(0.0),rightBoundaryPressurePa(0.0) {}
		};

		inline bool SolveDenseLinearSystem(
			std::vector<double> matrix,
			std::vector<double> rightHandSide,
			std::vector<double>& result
			)
		{
			const std::size_t count = rightHandSide.size();
			if( matrix.size() != count*count ) return false;
			for( std::size_t pivot=0; pivot<count; ++pivot ) {
				std::size_t selected = pivot;
				for( std::size_t row=pivot+1; row<count; ++row ) {
					if( std::fabs(matrix[row*count+pivot]) >
						std::fabs(matrix[selected*count+pivot]) ) selected = row;
				}
				if( !std::isfinite(matrix[selected*count+pivot]) ||
					matrix[selected*count+pivot] == 0.0 ) return false;
				if( selected != pivot ) {
					for( std::size_t column=pivot; column<count; ++column ) {
						std::swap(matrix[pivot*count+column],matrix[selected*count+column]);
					}
					std::swap(rightHandSide[pivot],rightHandSide[selected]);
				}
				const double diagonal = matrix[pivot*count+pivot];
				for( std::size_t row=pivot+1; row<count; ++row ) {
					const double factor = matrix[row*count+pivot]/diagonal;
					matrix[row*count+pivot] = 0.0;
					for( std::size_t column=pivot+1; column<count; ++column ) {
						matrix[row*count+column] -= factor*matrix[pivot*count+column];
					}
					rightHandSide[row] -= factor*rightHandSide[pivot];
				}
			}
			result.assign(count,0.0);
			for( std::size_t reverse=0; reverse<count; ++reverse ) {
				const std::size_t row = count-1-reverse;
				double value = rightHandSide[row];
				for( std::size_t column=row+1; column<count; ++column ) {
					value -= matrix[row*count+column]*result[column];
				}
				result[row] = value/matrix[row*count+row];
				if( !std::isfinite(result[row]) ) return false;
			}
			return true;
		}

		inline bool ProjectPressureOpenMACVelocity1D(
			const std::vector<double>& gasDensityKGPerM3,
			const std::vector<double>& unprojectedMomentumKGPerM2S,
			const std::vector<double>& divergenceTargetPerS,
			const double ambientDensityKGPerM3,
			const double cellWidthM,
			const double deltaTimeS,
			const double velocityToleranceMPerS,
			const double pressureTolerancePa,
			const bool seedLeftInflow,
			const bool seedRightInflow,
			OpenMACProjection1DResult& result,
			std::string* error = 0
			)
		{
			const std::size_t cells = gasDensityKGPerM3.size();
			const std::size_t unknowns = cells+2;
			if( cells < 2 || unprojectedMomentumKGPerM2S.size() != cells+1 ||
				divergenceTargetPerS.size() != cells ||
				!std::isfinite(ambientDensityKGPerM3) || ambientDensityKGPerM3 <= 0.0 ||
				!std::isfinite(cellWidthM) || cellWidthM <= 0.0 ||
				!std::isfinite(deltaTimeS) || deltaTimeS <= 0.0 ||
				!std::isfinite(velocityToleranceMPerS) || velocityToleranceMPerS < 0.0 ||
				!std::isfinite(pressureTolerancePa) || pressureTolerancePa <= 0.0 ) {
				return Fail(error,"fire solver pressure-open projection input is malformed");
			}
			result = OpenMACProjection1DResult();
			result.leftInflow = seedLeftInflow;
			result.rightInflow = seedRightInflow;
			result.faceDensityKGPerM3.assign(cells+1,0.0);
			for( std::size_t cell=0; cell<cells; ++cell ) {
				if( !std::isfinite(gasDensityKGPerM3[cell]) || gasDensityKGPerM3[cell] <= 0.0 ||
					!std::isfinite(divergenceTargetPerS[cell]) ) {
					return Fail(error,"fire solver pressure-open cell is invalid");
				}
			}
			for( const double momentum : unprojectedMomentumKGPerM2S ) {
				if( !std::isfinite(momentum) ) return Fail(error,"fire solver pressure-open momentum is invalid");
			}
			result.faceDensityKGPerM3[0] = 0.5*(ambientDensityKGPerM3+gasDensityKGPerM3[0]);
			for( std::size_t face=1; face<cells; ++face ) {
				result.faceDensityKGPerM3[face] = 0.5*(gasDensityKGPerM3[face-1]+gasDensityKGPerM3[face]);
			}
			result.faceDensityKGPerM3[cells] = 0.5*(gasDensityKGPerM3[cells-1]+ambientDensityKGPerM3);
			std::vector<double> pressure(unknowns,0.0);
			std::vector<unsigned int> seen;
			for( std::size_t activeIteration=0; activeIteration<8; ++activeIteration ) {
				const unsigned int activeCode = (result.leftInflow ? 1u : 0u) |
					(result.rightInflow ? 2u : 0u);
				if( std::find(seen.begin(),seen.end(),activeCode) != seen.end() ) {
					return Fail(error,"fire solver pressure-open active set cycled");
				}
				seen.push_back(activeCode);
				auto combinedResidual = [&]( const std::vector<double>& candidate ) {
					std::vector<double> velocity(cells+1,0.0);
					for( std::size_t face=0; face<=cells; ++face ) {
						velocity[face] = unprojectedMomentumKGPerM2S[face]/
							result.faceDensityKGPerM3[face];
						if( face == 0 ) velocity[face] -= 2.0*deltaTimeS*
							(candidate[0]-candidate[cells])/
							(result.faceDensityKGPerM3[face]*cellWidthM);
						else if( face == cells ) velocity[face] -= 2.0*deltaTimeS*
							(candidate[cells+1]-candidate[cells-1])/
							(result.faceDensityKGPerM3[face]*cellWidthM);
						else velocity[face] -= deltaTimeS*(candidate[face]-candidate[face-1])/
							(result.faceDensityKGPerM3[face]*cellWidthM);
					}
					double divergenceNorm = 0.0;
					for( std::size_t cell=0; cell<cells; ++cell ) divergenceNorm =
						std::max(divergenceNorm,std::fabs((velocity[cell+1]-velocity[cell])/
							cellWidthM-divergenceTargetPerS[cell]));
					const double leftHead = candidate[cells]+(result.leftInflow ?
						0.5*ambientDensityKGPerM3*velocity[0]*velocity[0] : 0.0);
					const double rightHead = candidate[cells+1]+(result.rightInflow ?
						0.5*ambientDensityKGPerM3*velocity[cells]*velocity[cells] : 0.0);
					return std::max(divergenceNorm*cellWidthM/
						std::max(velocityToleranceMPerS,1.0e-300),
						std::max(std::fabs(leftHead),std::fabs(rightHead))/pressureTolerancePa);
				};
				bool newtonConverged = false;
				for( std::size_t newton=0; newton<40; ++newton ) {
					std::vector<double> velocity(cells+1,0.0), residual(unknowns,0.0);
					std::vector<double> derivative((cells+1)*unknowns,0.0);
					for( std::size_t face=0; face<=cells; ++face ) {
						velocity[face] = unprojectedMomentumKGPerM2S[face]/
							result.faceDensityKGPerM3[face];
						if( face == 0 ) {
							const double factor = 2.0*deltaTimeS/
								(result.faceDensityKGPerM3[face]*cellWidthM);
							velocity[face] -= factor*(pressure[0]-pressure[cells]);
							derivative[face*unknowns+0] = -factor;
							derivative[face*unknowns+cells] = factor;
						} else if( face == cells ) {
							const double factor = 2.0*deltaTimeS/
								(result.faceDensityKGPerM3[face]*cellWidthM);
							velocity[face] -= factor*(pressure[cells+1]-pressure[cells-1]);
							derivative[face*unknowns+cells+1] = -factor;
							derivative[face*unknowns+cells-1] = factor;
						} else {
							const double factor = deltaTimeS/
								(result.faceDensityKGPerM3[face]*cellWidthM);
							velocity[face] -= factor*(pressure[face]-pressure[face-1]);
							derivative[face*unknowns+face] = -factor;
							derivative[face*unknowns+face-1] = factor;
						}
					}
					std::vector<double> jacobian(unknowns*unknowns,0.0);
					for( std::size_t cell=0; cell<cells; ++cell ) {
						residual[cell] = (velocity[cell+1]-velocity[cell])/cellWidthM-
							divergenceTargetPerS[cell];
						for( std::size_t column=0; column<unknowns; ++column ) {
							jacobian[cell*unknowns+column] = (derivative[(cell+1)*unknowns+column]-
								derivative[cell*unknowns+column])/cellWidthM;
						}
					}
					const std::size_t leftRow = cells, rightRow = cells+1;
					residual[leftRow] = pressure[cells];
					residual[rightRow] = pressure[cells+1];
					jacobian[leftRow*unknowns+cells] = 1.0;
					jacobian[rightRow*unknowns+cells+1] = 1.0;
					if( result.leftInflow ) {
						residual[leftRow] += 0.5*ambientDensityKGPerM3*velocity[0]*velocity[0];
						for( std::size_t column=0; column<unknowns; ++column ) {
							jacobian[leftRow*unknowns+column] += ambientDensityKGPerM3*
								velocity[0]*derivative[column];
						}
					}
					if( result.rightInflow ) {
						residual[rightRow] += 0.5*ambientDensityKGPerM3*
							velocity[cells]*velocity[cells];
						for( std::size_t column=0; column<unknowns; ++column ) {
							jacobian[rightRow*unknowns+column] += ambientDensityKGPerM3*
								velocity[cells]*derivative[cells*unknowns+column];
						}
					}
					double divergenceNorm = 0.0;
					for( std::size_t row=0; row<cells; ++row ) divergenceNorm =
						std::max(divergenceNorm,std::fabs(residual[row]));
					const double headNorm = std::max(std::fabs(residual[leftRow]),
						std::fabs(residual[rightRow]));
					const double norm = std::max(divergenceNorm*cellWidthM/
						std::max(velocityToleranceMPerS,1.0e-300),headNorm/pressureTolerancePa);
					result.nonlinearResidualHistory.push_back(norm);
					if( divergenceNorm <= velocityToleranceMPerS/cellWidthM &&
						headNorm <= pressureTolerancePa ) { newtonConverged = true; break; }
					for( double& value : residual ) value = -value;
					std::vector<double> update;
					if( !SolveDenseLinearSystem(jacobian,residual,update) ) {
						return Fail(error,"fire solver pressure-open Newton system is singular");
					}
					for( const double value : update ) if( !std::isfinite(value) ) {
						return Fail(error,"fire solver pressure-open Newton update overflowed");
					}
					double damping = 1.0;
					std::vector<double> trial(unknowns,0.0);
					double trialNorm = std::numeric_limits<double>::infinity();
					while( damping >= std::ldexp(1.0,-20) ) {
						for( std::size_t column=0; column<unknowns; ++column ) {
							trial[column] = pressure[column]+damping*update[column];
						}
						trialNorm = combinedResidual(trial);
						if( std::isfinite(trialNorm) && trialNorm < norm ) break;
						damping *= 0.5;
					}
					if( !std::isfinite(trialNorm) || trialNorm >= norm ) return Fail(error,
						"fire solver pressure-open Newton line search failed");
					pressure.swap(trial);
				}
				if( !newtonConverged ) return Fail(error,
					"fire solver pressure-open damped Newton iteration did not converge");
				result.dynamicPressurePa.assign(pressure.begin(),pressure.begin()+cells);
				result.velocityMPerS.assign(cells+1,0.0);
				result.momentumKGPerM2S.assign(cells+1,0.0);
				for( std::size_t face=0; face<=cells; ++face ) {
					double gradient = 0.0;
					if( face == 0 ) gradient = 2.0*(pressure[0]-pressure[cells])/cellWidthM;
					else if( face == cells ) gradient = 2.0*(pressure[cells+1]-pressure[cells-1])/cellWidthM;
					else gradient = (pressure[face]-pressure[face-1])/cellWidthM;
					result.momentumKGPerM2S[face] = unprojectedMomentumKGPerM2S[face]-deltaTimeS*gradient;
					result.velocityMPerS[face] = result.momentumKGPerM2S[face]/result.faceDensityKGPerM3[face];
				}
				const double leftOutward = -result.velocityMPerS[0];
				const double rightOutward = result.velocityMPerS[cells];
				const bool nextLeft = leftOutward < -velocityToleranceMPerS ? true :
					(leftOutward > velocityToleranceMPerS ? false : result.leftInflow);
				const bool nextRight = rightOutward < -velocityToleranceMPerS ? true :
					(rightOutward > velocityToleranceMPerS ? false : result.rightInflow);
				if( nextLeft == result.leftInflow && nextRight == result.rightInflow ) {
					result.maximumDivergenceResidualPerS = 0.0;
					for( std::size_t cell=0; cell<cells; ++cell ) result.maximumDivergenceResidualPerS =
						std::max(result.maximumDivergenceResidualPerS,std::fabs((result.velocityMPerS[cell+1]-
							result.velocityMPerS[cell])/cellWidthM-divergenceTargetPerS[cell]));
					result.maximumBoundaryHeadResidualPa = std::max(
						std::fabs(pressure[cells]+(result.leftInflow ? 0.5*ambientDensityKGPerM3*
							result.velocityMPerS[0]*result.velocityMPerS[0] : 0.0)),
						std::fabs(pressure[cells+1]+(result.rightInflow ? 0.5*ambientDensityKGPerM3*
							result.velocityMPerS[cells]*result.velocityMPerS[cells] : 0.0)));
					result.leftBoundaryPressurePa = pressure[cells];
					result.rightBoundaryPressurePa = pressure[cells+1];
					return result.maximumDivergenceResidualPerS <= velocityToleranceMPerS/cellWidthM &&
						result.maximumBoundaryHeadResidualPa <= pressureTolerancePa;
				}
				result.leftInflow = nextLeft;
				result.rightInflow = nextRight;
			}
			return Fail(error,"fire solver pressure-open active set did not converge");
		}

		inline bool ProjectPressureOpenMACVelocity1DFinal(
			const std::vector<double>& gasDensityKGPerM3,
			const std::vector<double>& unprojectedMomentumKGPerM2S,
			const std::vector<double>& divergenceTargetPerS,
			const double ambientDensityKGPerM3,
			const double cellWidthM,
			const double deltaTimeS,
			const bool leftStage0Inflow,
			const bool leftStage1Inflow,
			const bool rightStage0Inflow,
			const bool rightStage1Inflow,
			const double leftStage0VelocityMPerS,
			const double leftStage1VelocityMPerS,
			const double rightStage0VelocityMPerS,
			const double rightStage1VelocityMPerS,
			const double velocityToleranceMPerS,
			const double absoluteTolerancePerS,
			OpenMACProjection1DResult& result,
			std::string* error = 0
			)
		{
			const std::size_t cells = gasDensityKGPerM3.size();
			if( cells < 2 || unprojectedMomentumKGPerM2S.size() != cells+1 ||
				divergenceTargetPerS.size() != cells || !std::isfinite(ambientDensityKGPerM3) ||
				ambientDensityKGPerM3 <= 0.0 || !std::isfinite(cellWidthM) || cellWidthM <= 0.0 ||
				!std::isfinite(deltaTimeS) || deltaTimeS <= 0.0 ||
				!std::isfinite(velocityToleranceMPerS) || velocityToleranceMPerS < 0.0 ||
				!std::isfinite(absoluteTolerancePerS) || absoluteTolerancePerS <= 0.0 ) {
				return Fail(error,"fire solver final pressure-open projection input is malformed");
			}
			const double stageVelocity[4] = {leftStage0VelocityMPerS,leftStage1VelocityMPerS,
				rightStage0VelocityMPerS,rightStage1VelocityMPerS};
			for( const double velocity : stageVelocity ) if( !std::isfinite(velocity) ) {
				return Fail(error,"fire solver final pressure-open stage velocity is non-finite");
			}
			for( std::size_t cell=0; cell<cells; ++cell ) {
				if( !std::isfinite(gasDensityKGPerM3[cell]) || gasDensityKGPerM3[cell] <= 0.0 ||
					!std::isfinite(divergenceTargetPerS[cell]) ) {
					return Fail(error,"fire solver final pressure-open cell is invalid");
				}
			}
			for( const double momentum : unprojectedMomentumKGPerM2S ) {
				if( !std::isfinite(momentum) ) return Fail(error,
					"fire solver final pressure-open momentum is invalid");
			}
			const double leftPressure = -0.25*ambientDensityKGPerM3*
				((leftStage0Inflow ? leftStage0VelocityMPerS*leftStage0VelocityMPerS : 0.0)+
				 (leftStage1Inflow ? leftStage1VelocityMPerS*leftStage1VelocityMPerS : 0.0));
			const double rightPressure = -0.25*ambientDensityKGPerM3*
				((rightStage0Inflow ? rightStage0VelocityMPerS*rightStage0VelocityMPerS : 0.0)+
				 (rightStage1Inflow ? rightStage1VelocityMPerS*rightStage1VelocityMPerS : 0.0));
			result = OpenMACProjection1DResult();
			result.leftBoundaryPressurePa = leftPressure;
			result.rightBoundaryPressurePa = rightPressure;
			result.faceDensityKGPerM3.assign(cells+1,0.0);
			result.faceDensityKGPerM3[0] = 0.5*(ambientDensityKGPerM3+gasDensityKGPerM3[0]);
			for( std::size_t face=1; face<cells; ++face ) result.faceDensityKGPerM3[face] =
				0.5*(gasDensityKGPerM3[face-1]+gasDensityKGPerM3[face]);
			result.faceDensityKGPerM3[cells] = 0.5*(gasDensityKGPerM3[cells-1]+ambientDensityKGPerM3);
			std::vector<double> zeroPressure(cells,0.0), zeroVelocity(cells+1,0.0);
			auto velocityFromPressure = [&]( const std::vector<double>& pressure,
				std::vector<double>& velocity ) {
				velocity.assign(cells+1,0.0);
				for( std::size_t face=0; face<=cells; ++face ) {
					double gradient = face == 0 ? 2.0*(pressure[0]-leftPressure)/cellWidthM :
						(face == cells ? 2.0*(rightPressure-pressure[cells-1])/cellWidthM :
						(pressure[face]-pressure[face-1])/cellWidthM);
					velocity[face] = (unprojectedMomentumKGPerM2S[face]-deltaTimeS*gradient)/
						result.faceDensityKGPerM3[face];
				}
			};
			velocityFromPressure(zeroPressure,zeroVelocity);
			std::vector<double> matrix(cells*cells,0.0), rightHandSide(cells,0.0);
			for( std::size_t row=0; row<cells; ++row ) rightHandSide[row] =
				divergenceTargetPerS[row]-(zeroVelocity[row+1]-zeroVelocity[row])/cellWidthM;
			for( std::size_t column=0; column<cells; ++column ) {
				std::vector<double> basis(cells,0.0), basisVelocity(cells+1,0.0);
				basis[column] = 1.0;
				velocityFromPressure(basis,basisVelocity);
				for( std::size_t row=0; row<cells; ++row ) matrix[row*cells+column] =
					((basisVelocity[row+1]-basisVelocity[row])-
					 (zeroVelocity[row+1]-zeroVelocity[row]))/cellWidthM;
			}
			if( !SolveDenseLinearSystem(matrix,rightHandSide,result.dynamicPressurePa) ) {
				return Fail(error,"fire solver final pressure-open linear system is singular");
			}
			velocityFromPressure(result.dynamicPressurePa,result.velocityMPerS);
			result.momentumKGPerM2S.assign(cells+1,0.0);
			for( std::size_t face=0; face<=cells; ++face ) result.momentumKGPerM2S[face] =
				result.faceDensityKGPerM3[face]*result.velocityMPerS[face];
			result.maximumDivergenceResidualPerS = 0.0;
			for( std::size_t cell=0; cell<cells; ++cell ) result.maximumDivergenceResidualPerS =
				std::max(result.maximumDivergenceResidualPerS,std::fabs((result.velocityMPerS[cell+1]-
					result.velocityMPerS[cell])/cellWidthM-divergenceTargetPerS[cell]));
			result.maximumBoundaryHeadResidualPa = 0.0;
			result.leftInflow = result.velocityMPerS[0] > velocityToleranceMPerS ? true :
				(result.velocityMPerS[0] < -velocityToleranceMPerS ? false : leftStage1Inflow);
			result.rightInflow = result.velocityMPerS[cells] < -velocityToleranceMPerS ? true :
				(result.velocityMPerS[cells] > velocityToleranceMPerS ? false : rightStage1Inflow);
			return result.maximumDivergenceResidualPerS <= absoluteTolerancePerS ||
				Fail(error,"fire solver final pressure-open projection misses its divergence target");
		}

		inline std::vector<double> GasDensityFromConservative(
			const std::vector<ConservativeVector>& cells
			)
		{
			std::vector<double> result(cells.size(),0.0);
			for( std::size_t cell=0; cell<cells.size(); ++cell ) {
				for( std::size_t species=0; species<MethaneCarbon; ++species ) {
					result[cell] += cells[cell][1+species];
				}
			}
			return result;
		}

		inline void GasPrimalSubfluxes(
			const PeriodicFluxPair& flux,
			std::vector<double>& lowAdvection,
			std::vector<double>& highAdvection,
			std::vector<double>& physicalDiffusion
			)
		{
			const std::size_t count = flux.low.size();
			lowAdvection.assign(count,0.0);
			highAdvection.assign(count,0.0);
			physicalDiffusion.assign(count,0.0);
			for( std::size_t face=0; face<count; ++face ) {
				for( std::size_t species=0; species<MethaneCarbon; ++species ) {
					physicalDiffusion[face] += flux.nonadvectiveMass[face][1+species];
					lowAdvection[face] += flux.low[face][1+species]-
						flux.nonadvectiveMass[face][1+species];
					highAdvection[face] += flux.high[face][1+species]-
						flux.nonadvectiveMass[face][1+species];
				}
			}
		}

		inline std::vector<double> CompatibleMomentumFluxDivergence(
			const std::vector<double>& acceptedGasAdvection,
			const std::vector<double>& physicalGasDiffusion,
			const std::vector<double>& faceVelocity,
			const double cellWidthM
			)
		{
			const std::size_t count = faceVelocity.size();
			std::vector<double> dualFlux(count,0.0), result(count,0.0);
			for( std::size_t center=0; center<count; ++center ) {
				const std::size_t previous = (center+count-1)%count;
				const double restrictedMass = 0.5*(
					acceptedGasAdvection[previous]+acceptedGasAdvection[center]+
					physicalGasDiffusion[previous]+physicalGasDiffusion[center]);
				dualFlux[center] = restrictedMass*0.5*(
					faceVelocity[previous]+faceVelocity[center]);
			}
			for( std::size_t face=0; face<count; ++face ) {
				result[face] = (dualFlux[(face+1)%count]-dualFlux[face])/cellWidthM;
			}
			return result;
		}

		inline bool RemainingMomentumRHS(
			const std::vector<ConservativeVector>& state,
			const std::vector<double>& faceVelocity,
			const std::vector<double>& dynamicViscosityPaS,
			const std::vector<ConservativeVector>& frozenSourcePerS,
			const PeriodicTransportConfig& config,
			std::vector<double>& result,
			std::string* error = 0
			)
		{
			const std::size_t count = state.size();
			if( count < 3 || faceVelocity.size() != count ||
				dynamicViscosityPaS.size() != count || frozenSourcePerS.size() != count ||
				!std::isfinite(config.gravityMPerS2) ||
				!std::isfinite(config.ambientGasDensityKGPerM3) ||
				config.ambientGasDensityKGPerM3 < 0.0 ) {
				return Fail(error,"fire solver nonpressure momentum inputs are malformed");
			}
			std::vector<double> stress(count,0.0);
			for( std::size_t cell=0; cell<count; ++cell ) {
				if( !std::isfinite(dynamicViscosityPaS[cell]) ||
					dynamicViscosityPaS[cell] < 0.0 || !std::isfinite(faceVelocity[cell]) ) {
					return Fail(error,"fire solver momentum transport coefficient is invalid");
				}
				const std::size_t leftFace = (cell+count-1)%count;
				stress[cell] = (4.0/3.0)*dynamicViscosityPaS[cell]*
					(faceVelocity[cell]-faceVelocity[leftFace])/config.cellWidthM;
			}
			result.assign(count,0.0);
			for( std::size_t face=0; face<count; ++face ) {
				const std::size_t right = (face+1)%count;
				const double gasDensity = 0.5*(
					FromConservativeVector(state[face]).GasDensity()+
					FromConservativeVector(state[right]).GasDensity());
				double phaseRateLeft = 0.0, phaseRateRight = 0.0;
				for( std::size_t species=0; species<MethaneCarbon; ++species ) {
					phaseRateLeft += frozenSourcePerS[face][1+species];
					phaseRateRight += frozenSourcePerS[right][1+species];
				}
				const double phaseRate = 0.5*(phaseRateLeft+phaseRateRight);
				result[face] = (stress[right]-stress[face])/config.cellWidthM+
					(gasDensity-config.ambientGasDensityKGPerM3)*config.gravityMPerS2+
					faceVelocity[face]*phaseRate;
				if( !std::isfinite(result[face]) ) {
					return Fail(error,"fire solver nonpressure momentum RHS overflowed");
				}
			}
			return true;
		}

		struct PeriodicCoupledStage
		{
			PeriodicFluxPair flux;
			PeriodicProjectionResult projection;
			std::vector<double> nonpressureMomentumRHS;
			std::vector<double> faceAlpha;
			std::vector<double> divergenceTargetPerS;
			std::vector<double> picardResidualPerS;
			std::vector<double> diffusivityM2PerS;
			std::vector<double> conductivityWPerMK;
			std::vector<double> dynamicViscosityPaS;
		};

		inline bool BuildPeriodicStageTransport(
			const std::vector<ConservativeVector>& state,
			const std::vector<double>& temperatureK,
			const std::vector<double>& faceVelocityMPerS,
			const double cellWidthM,
			const bool dns,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			std::vector<double>& diffusivityM2PerS,
			std::vector<double>& conductivityWPerMK,
			std::vector<double>& dynamicViscosityPaS,
			std::string* error = 0
			)
		{
			const std::size_t count = state.size();
			if( count < 3 || temperatureK.size() != count ||
				faceVelocityMPerS.size() != count || !transport.IsValid() ||
				!std::isfinite(cellWidthM) || cellWidthM <= 0.0 ) {
				return Fail(error,"fire solver stage transport input is malformed");
			}
			diffusivityM2PerS.assign(count,0.0);
			conductivityWPerMK.assign(count,0.0);
			dynamicViscosityPaS.assign(count,0.0);
			const double widths[3] = {cellWidthM,cellWidthM,cellWidthM};
			for( std::size_t cell=0; cell<count; ++cell ) {
				MethaneCellState physical = FromConservativeVector(state[cell]);
				physical.temperatureK = temperatureK[cell];
				double gradient[3][3] = {};
				gradient[0][0] = (faceVelocityMPerS[cell]-
					faceVelocityMPerS[(cell+count-1)%count])/cellWidthM;
				CellTransportEvaluation evaluation;
				if( !EvaluateCellTransport(physical,gradient,widths,dns,thermochemistry,
					transport,evaluation,error) ) return false;
				diffusivityM2PerS[cell] = evaluation.totalDiffusivityM2PerS;
				conductivityWPerMK[cell] = evaluation.effectiveConductivityWPerMK;
				dynamicViscosityPaS[cell] = evaluation.effectiveViscosityPaS;
			}
			return true;
		}

		inline bool SolvePeriodicCoupledStage(
			const std::vector<ConservativeVector>& state,
			const std::vector<double>& unprojectedMomentum,
			const std::vector<ConservativeVector>& frozenSourcePerS,
			const PeriodicTransportConfig& config,
			const double projectionTolerancePerS,
			const bool solvePredictorLimiter,
			const bool dns,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			PeriodicCoupledStage& result,
			std::string* error = 0
			)
		{
			std::vector<double> temperature;
			if( !InvertPeriodicTemperatures(state,thermochemistry,temperature,error) ) return false;
			std::vector<double> target(state.size(),0.0), priorMassFlux(state.size(),0.0);
			std::vector<double> priorAlpha(state.size(),0.0);
			std::vector<double> priorDiffusivity(state.size(),0.0),
				priorConductivity(state.size(),0.0),priorViscosity(state.size(),0.0);
			result.picardResidualPerS.clear();
			for( std::size_t iteration=0; iteration<64; ++iteration ) {
				PeriodicProjectionResult projection;
				if( !ProjectPeriodicMACVelocity(GasDensityFromConservative(state),
					unprojectedMomentum,target,config.cellWidthM,config.deltaTimeS,
					projectionTolerancePerS,projection,error) ) return false;
				std::vector<double> diffusivityM2PerS, conductivityWPerMK, dynamicViscosityPaS;
				if( !BuildPeriodicStageTransport(state,temperature,projection.velocityMPerS,
					config.cellWidthM,dns,thermochemistry,transport,diffusivityM2PerS,
					conductivityWPerMK,dynamicViscosityPaS,error) ) return false;
				PeriodicFluxPair flux;
				if( !BuildPeriodicFluxPair(state,temperature,projection.velocityMPerS,
					diffusivityM2PerS,conductivityWPerMK,config.cellWidthM,fuel,
					thermochemistry,flux,error) ) return false;
				std::vector<double> nextAlpha;
				if( solvePredictorLimiter ) {
					std::vector<ConservativeVector> predictor;
					if( !ApplyPeriodicSharedFCT(state,flux,frozenSourcePerS,config,fuel,
						thermochemistry,predictor,nextAlpha,error) ) return false;
				}
				std::vector<double> nextTarget;
				if( !PeriodicDivergenceTargetFromPhysicalFlux(state,temperature,flux,
					frozenSourcePerS,config.cellWidthM,thermochemistry,nextTarget,error) ) return false;
				double residual = 0.0, massFluxResidual = 0.0, alphaResidual = 0.0,
					coefficientResidual = 0.0;
				for( std::size_t cell=0; cell<state.size(); ++cell ) {
					residual = std::max(residual,std::fabs(nextTarget[cell]-target[cell]));
					const double massFlux = projection.faceDensityKGPerM3[cell]*
						projection.velocityMPerS[cell];
					if( iteration ) massFluxResidual = std::max(massFluxResidual,
						std::fabs(massFlux-priorMassFlux[cell]));
					priorMassFlux[cell] = massFlux;
					if( solvePredictorLimiter && iteration ) alphaResidual = std::max(
						alphaResidual,std::fabs(nextAlpha[cell]-priorAlpha[cell]));
					if( iteration ) coefficientResidual = std::max({coefficientResidual,
						std::fabs(diffusivityM2PerS[cell]-priorDiffusivity[cell]),
						std::fabs(conductivityWPerMK[cell]-priorConductivity[cell]),
						std::fabs(dynamicViscosityPaS[cell]-priorViscosity[cell])});
				}
				if( solvePredictorLimiter ) priorAlpha = nextAlpha;
				priorDiffusivity = diffusivityM2PerS;
				priorConductivity = conductivityWPerMK;
				priorViscosity = dynamicViscosityPaS;
				result.picardResidualPerS.push_back(std::max({residual,massFluxResidual/
					std::max(config.cellWidthM,1.0e-300),alphaResidual,coefficientResidual}));
				target = nextTarget;
				result.flux = flux;
				result.projection = projection;
				result.divergenceTargetPerS = target;
				result.faceAlpha = nextAlpha;
				result.diffusivityM2PerS = diffusivityM2PerS;
				result.conductivityWPerMK = conductivityWPerMK;
				result.dynamicViscosityPaS = dynamicViscosityPaS;
				if( !RemainingMomentumRHS(state,projection.velocityMPerS,
					dynamicViscosityPaS,frozenSourcePerS,config,
					result.nonpressureMomentumRHS,error) ) return false;
				if( residual <= projectionTolerancePerS &&
					(!iteration || (massFluxResidual/config.cellWidthM <= projectionTolerancePerS &&
						alphaResidual <= projectionTolerancePerS &&
						coefficientResidual <= projectionTolerancePerS)) ) {
					if( iteration ) {
						PeriodicProjectionResult acceptedProjection;
						if( !ProjectPeriodicMACVelocity(GasDensityFromConservative(state),
							unprojectedMomentum,target,config.cellWidthM,config.deltaTimeS,
							projectionTolerancePerS,acceptedProjection,error) ) return false;
						result.projection = acceptedProjection;
						return true;
					}
				}
			}
			return Fail(error,"fire solver periodic coupled Picard stage did not converge");
		}

		struct PeriodicProjectedHeunResult
		{
			std::vector<ConservativeVector> conservative;
			std::vector<double> momentumKGPerM2S;
			std::vector<double> velocityMPerS;
			std::vector<double> stepAveragePressurePa;
			std::vector<double> faceAlpha;
			std::vector<double> divergenceHeunPerS;
			PeriodicCoupledStage r0;
			PeriodicCoupledStage r1;
			PeriodicCoupledStage r2;
		};

		inline bool AdvancePeriodicProjectedHeun(
			const std::vector<ConservativeVector>& beginning,
			const std::vector<double>& beginningMomentum,
			const std::vector<ConservativeVector>& frozenSourcePerS,
			const PeriodicTransportConfig& config,
			const double projectionTolerancePerS,
			const bool dns,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			PeriodicProjectedHeunResult& result,
			std::string* error = 0
			)
		{
			const std::size_t count = beginning.size();
			if( beginningMomentum.size() != count ) {
				return Fail(error,"fire solver projected-Heun momentum dimension is invalid");
			}
			if( !SolvePeriodicCoupledStage(beginning,beginningMomentum,frozenSourcePerS,
				config,projectionTolerancePerS,true,dns,fuel,thermochemistry,transport,
				result.r0,error) ) return false;
			std::vector<ConservativeVector> predictor;
			std::vector<double> predictorAlpha;
			if( !ApplyPeriodicSharedFCT(beginning,result.r0.flux,frozenSourcePerS,
				config,fuel,thermochemistry,predictor,predictorAlpha,error) ) return false;
			std::vector<double> low0, high0, diffusion0;
			GasPrimalSubfluxes(result.r0.flux,low0,high0,diffusion0);
			std::vector<double> predictorAcceptedGas(count,0.0);
			for( std::size_t face=0; face<count; ++face ) {
				predictorAcceptedGas[face] = low0[face]+predictorAlpha[face]*
					(high0[face]-low0[face]);
			}
			std::vector<double> predictorMomentumDivergence = CompatibleMomentumFluxDivergence(
				predictorAcceptedGas,diffusion0,result.r0.projection.velocityMPerS,
				config.cellWidthM);
			std::vector<double> predictorMomentum(count,0.0);
			for( std::size_t face=0; face<count; ++face ) {
				predictorMomentum[face] = beginningMomentum[face]+config.deltaTimeS*(
					result.r0.nonpressureMomentumRHS[face]-predictorMomentumDivergence[face]);
			}
			if( !SolvePeriodicCoupledStage(predictor,predictorMomentum,frozenSourcePerS,
				config,projectionTolerancePerS,false,dns,fuel,thermochemistry,transport,
				result.r1,error) ) return false;
			PeriodicFluxPair averaged;
			averaged.low.resize(count); averaged.high.resize(count);
			averaged.nonadvectiveMass.resize(count); averaged.nonadvectiveEnergy.resize(count);
			for( std::size_t face=0; face<count; ++face ) {
				averaged.low[face] = 0.5*(result.r0.flux.low[face]+result.r1.flux.low[face]);
				averaged.high[face] = 0.5*(result.r0.flux.high[face]+result.r1.flux.high[face]);
				averaged.nonadvectiveEnergy[face] = 0.5*(result.r0.flux.nonadvectiveEnergy[face]+
					result.r1.flux.nonadvectiveEnergy[face]);
				for( std::size_t component=0; component<MethaneMassStateDimension; ++component ) {
					averaged.nonadvectiveMass[face][component] = 0.5*(
						result.r0.flux.nonadvectiveMass[face][component]+
						result.r1.flux.nonadvectiveMass[face][component]);
				}
			}
			if( !ApplyPeriodicSharedFCT(beginning,averaged,frozenSourcePerS,config,
				fuel,thermochemistry,result.conservative,result.faceAlpha,error) ) return false;
			std::vector<double> heunTemperature;
			if( !InvertPeriodicTemperatures(result.conservative,thermochemistry,
				heunTemperature,error) || !PeriodicDivergenceTargetFromPhysicalFlux(
					result.conservative,heunTemperature,averaged,frozenSourcePerS,
					config.cellWidthM,thermochemistry,result.divergenceHeunPerS,error) ) return false;
			std::vector<double> low1, high1, diffusion1;
			GasPrimalSubfluxes(result.r1.flux,low1,high1,diffusion1);
			std::vector<double> accepted0(count), accepted1(count);
			for( std::size_t face=0; face<count; ++face ) {
				accepted0[face] = low0[face]+result.faceAlpha[face]*(high0[face]-low0[face]);
				accepted1[face] = low1[face]+result.faceAlpha[face]*(high1[face]-low1[face]);
			}
			const std::vector<double> divergence0 = CompatibleMomentumFluxDivergence(
				accepted0,diffusion0,result.r0.projection.velocityMPerS,config.cellWidthM);
			const std::vector<double> divergence1 = CompatibleMomentumFluxDivergence(
				accepted1,diffusion1,result.r1.projection.velocityMPerS,config.cellWidthM);
			std::vector<double> heunMomentum(count,0.0);
			for( std::size_t face=0; face<count; ++face ) {
				heunMomentum[face] = beginningMomentum[face]+0.5*config.deltaTimeS*(
					result.r0.nonpressureMomentumRHS[face]+
					result.r1.nonpressureMomentumRHS[face]-divergence0[face]-divergence1[face]);
			}
			if( !SolvePeriodicCoupledStage(result.conservative,heunMomentum,frozenSourcePerS,
				config,projectionTolerancePerS,false,dns,fuel,thermochemistry,transport,
				result.r2,error) ) return false;
			result.momentumKGPerM2S = result.r2.projection.momentumKGPerM2S;
			result.velocityMPerS = result.r2.projection.velocityMPerS;
			result.stepAveragePressurePa = result.r2.projection.pressureImpulsePa;
			return true;
		}

		inline bool TrialAdiabaticTemperatureK(
			const MethaneCellState& beginning,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			double& result,
			std::string* error = 0
			)
		{
			if( !fuel.IsValid() || !thermochemistry.IsValid() ||
				fuel.PrimaryReactionDelta().size() != MethaneSpeciesCount ||
				!ValidateCellState(beginning,error) ) {
				return Fail(error,"fire solver trial adiabatic state lacks valid records");
			}
			const double extent = std::min(beginning.constituent[MethaneCH4],
				beginning.constituent[MethaneO2]/fuel.StoichiometricOxygenKGPerKGFuel());
			MethaneCellState trial = beginning;
			const std::vector<double>& delta = fuel.PrimaryReactionDelta();
			for( std::size_t index=0; index<MethaneSpeciesCount; ++index ) {
				trial.constituent[index] += extent*delta[index];
			}
			trial.sensibleEnergyJPerM3 += extent*fuel.LowerHeatingValueJPerKG();
			return thermochemistry.InvertMixtureTemperatureK(
				ThermochemicalDensities(trial),trial.sensibleEnergyJPerM3,result,error) &&
				(std::isfinite(result) || Fail(error,"fire solver trial adiabatic temperature is non-finite"));
		}

		struct IgnitionGrid
		{
			std::size_t nx, ny, nz;
			std::vector<MethaneCellState> cells;
			std::vector<bool> pilotMask;
		};

		inline bool BuildIgnitionEligibility(
			const IgnitionGrid& grid,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			std::vector<bool>& eligible,
			std::string* error = 0
			)
		{
			if( grid.nx == 0 || grid.ny == 0 || grid.nz == 0 ||
				grid.nx > std::numeric_limits<std::size_t>::max()/grid.ny ||
				grid.nx*grid.ny > std::numeric_limits<std::size_t>::max()/grid.nz ||
				!fuel.IsValid() || !thermochemistry.IsValid() ||
				!transport.IsValid() ) {
				return Fail(error,"fire solver ignition graph input is malformed");
			}
			const std::size_t plane = grid.nx*grid.ny;
			const std::size_t count = plane*grid.nz;
			if(
				grid.cells.size() != count || grid.pilotMask.size() != count ||
				count == 0 ) {
				return Fail(error,"fire solver ignition graph input is malformed");
			}
			std::vector<bool> vertex(count,false), seed(count,false);
			for( std::size_t index=0; index<count; ++index ) {
				const MethaneCellState& state = grid.cells[index];
				if( !ValidateCellState(state,error) ) return false;
				if( state.constituent[MethaneCH4] <= 0.0 ||
					state.constituent[MethaneO2] <= 0.0 ||
					state.temperatureK <= fuel.PilotTemperatureK() ) continue;
				double adiabatic = 0.0;
				if( !TrialAdiabaticTemperatureK(state,fuel,thermochemistry,adiabatic,error) ) return false;
				vertex[index] = adiabatic >= transport.CriticalFlameTemperatureK();
				seed[index] = vertex[index] && (grid.pilotMask[index] ||
					state.temperatureK > fuel.AutoignitionTemperatureK());
			}
			eligible.assign(count,false);
			std::queue<std::size_t> pending;
			for( std::size_t index=0; index<count; ++index ) if( seed[index] ) {
				eligible[index] = true; pending.push(index);
			}
			while( !pending.empty() ) {
				const std::size_t index = pending.front(); pending.pop();
				const std::size_t x = index%grid.nx;
				const std::size_t y = (index/grid.nx)%grid.ny;
				const std::size_t z = index/plane;
				const std::size_t neighbors[6] = {
					x ? index-1 : count, x+1<grid.nx ? index+1 : count,
					y ? index-grid.nx : count, y+1<grid.ny ? index+grid.nx : count,
					z ? index-plane : count, z+1<grid.nz ? index+plane : count
				};
				for( const std::size_t neighbor : neighbors ) {
					if( neighbor < count && vertex[neighbor] && !eligible[neighbor] ) {
						eligible[neighbor] = true; pending.push(neighbor);
					}
				}
			}
			return true;
		}

		struct GasExchangeEvaluation
		{
			double exchangeWPerM3;
			double temperatureDerivativeWPerM3K;
			GasExchangeEvaluation() : exchangeWPerM3(0.0),
				temperatureDerivativeWPerM3K(0.0) {}
		};

		struct RadiationEscapeFactor
		{
			double beta;
			double gamma;
			double accepted;
			RadiationEscapeFactor() : beta(0.0), gamma(0.0), accepted(0.0) {}
		};

		inline bool ComputeRadiationEscapeFactor(
			const double totalHeatReleaseW,
			const double nominalPeakHeatReleaseW,
			const double radiativeFraction,
			const std::vector<double>& unscaledExchangeWPerM3,
			const std::vector<double>& cellVolumeM3,
			const bool predictive,
			RadiationEscapeFactor& result,
			std::string* error = 0
			)
		{
			if( !std::isfinite(totalHeatReleaseW) || totalHeatReleaseW < 0.0 ||
				!std::isfinite(nominalPeakHeatReleaseW) || nominalPeakHeatReleaseW <= 0.0 ||
				!std::isfinite(radiativeFraction) || radiativeFraction < 0.0 ||
				radiativeFraction > 1.0 || unscaledExchangeWPerM3.empty() ||
				unscaledExchangeWPerM3.size() != cellVolumeM3.size() ) {
				return Fail(error,"fire solver radiation budget input is malformed");
			}
			double exchangeIntegralW = 0.0;
			for( std::size_t cell=0; cell<unscaledExchangeWPerM3.size(); ++cell ) {
				if( !std::isfinite(unscaledExchangeWPerM3[cell]) ||
					!std::isfinite(cellVolumeM3[cell]) ||
					cellVolumeM3[cell] <= 0.0 ) {
					return Fail(error,"fire solver radiation budget contains an invalid cell");
				}
				const double contribution = unscaledExchangeWPerM3[cell]*cellVolumeM3[cell];
				if( !std::isfinite(contribution) ||
					!std::isfinite(exchangeIntegralW+contribution) ) {
					return Fail(error,"fire solver radiation budget accumulation overflowed");
				}
				exchangeIntegralW += contribution;
			}
			result = RadiationEscapeFactor();
			if( totalHeatReleaseW > 0.0 ) {
				if( exchangeIntegralW <= 0.0 ) {
					return Fail(error,"fire solver burning radiation budget has no modeled opacity");
				}
				result.beta = radiativeFraction*totalHeatReleaseW/exchangeIntegralW;
				if( predictive && result.beta > 1.0 ) {
					return Fail(error,"fire solver predictive opacity cannot supply the requested radiative fraction");
				}
			}
			result.gamma = std::max(0.0,std::min(1.0,1.0-totalHeatReleaseW/
				(0.01*nominalPeakHeatReleaseW)));
			result.accepted = std::max(result.beta,result.gamma);
			return std::isfinite(result.accepted) ||
				Fail(error,"fire solver radiation escape factor overflowed");
		}

		inline bool EvaluateSyntheticHotCarbonExchange(
			const double carbonKGPerM3,
			const double temperatureK,
			const double ambientTemperatureK,
			const double effectiveAbsorption,
			const double sootDensityKGPerM3,
			double& exchangeWPerM3,
			double& temperatureDerivativeWPerM3K,
			std::string* error = 0
			)
		{
			const double sigmaSB = 5.670374419e-8;
			const double secondRadiationConstantMK = 0.014387768775039337;
			if( !std::isfinite(carbonKGPerM3) || carbonKGPerM3 < 0.0 ||
				!std::isfinite(temperatureK) || temperatureK <= 0.0 ||
				!std::isfinite(ambientTemperatureK) || ambientTemperatureK <= 0.0 ||
				!std::isfinite(effectiveAbsorption) || effectiveAbsorption < 0.0 ||
				!std::isfinite(sootDensityKGPerM3) || sootDensityKGPerM3 <= 0.0 ) {
				return Fail(error,"synthetic hot-carbon radiation fixture is invalid");
			}
			const double volumeFraction = carbonKGPerM3/sootDensityKGPerM3;
			const double c0 = 6.0*std::acos(-1.0)*effectiveAbsorption;
			const double coefficient = 4.0*sigmaSB*3.83*c0*volumeFraction/
				secondRadiationConstantMK;
			const double temperature2 = temperatureK*temperatureK;
			const double temperature4 = temperature2*temperature2;
			const double ambient2 = ambientTemperatureK*ambientTemperatureK;
			const double ambient4 = ambient2*ambient2;
			exchangeWPerM3 = coefficient*(temperature4*temperatureK-
				ambient4*ambientTemperatureK);
			temperatureDerivativeWPerM3K = 5.0*coefficient*temperature4;
			return (std::isfinite(exchangeWPerM3) &&
				std::isfinite(temperatureDerivativeWPerM3K)) ||
				Fail(error,"synthetic hot-carbon radiation fixture overflowed");
		}

		inline bool EvaluateGasExchange(
			const MethaneCellState& state,
			const double temperatureK,
			const double ambientTemperatureK,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationGasOpacityRecord& opacity,
			GasExchangeEvaluation& result,
			std::string* error = 0
			)
		{
			const double sigmaSB = 5.670374419e-8;
			const double avogadro = 6.02214076e23;
			if( !opacity.IsValid() || !thermochemistry.IsValid() ||
				!std::isfinite(temperatureK) || !std::isfinite(ambientTemperatureK) ||
				temperatureK <= 0.0 || ambientTemperatureK <= 0.0 ) {
				return Fail(error,"fire solver gas exchange state is outside its record domain");
			}
			result = GasExchangeEvaluation();
			const char* ids[2] = {"CO2", "H2O"};
			const std::size_t indices[2] = {MethaneCO2,MethaneH2O};
			for( std::size_t speciesIndex=0; speciesIndex<2; ++speciesIndex ) {
				const FireThermochemistrySpecies* species = thermochemistry.FindSpecies(ids[speciesIndex]);
				if( !species ) return Fail(error,"fire solver gas opacity species lacks thermochemistry");
				const double moleculesPerM3 = state.constituent[indices[speciesIndex]]/
					species->molecularWeightKGPerKMol*1000.0*avogadro;
				double hot = 0.0, hotGasDerivative = 0.0, hotRadiationDerivative = 0.0;
				double ambient = 0.0, ambientGasDerivative = 0.0;
				double ignoredDerivative = 0.0;
				if( !opacity.PlanckMeanCrossSectionM2PerMolecule(ids[speciesIndex],
					temperatureK,temperatureK,hot,hotGasDerivative,hotRadiationDerivative,error) ||
					!opacity.PlanckMeanCrossSectionM2PerMolecule(ids[speciesIndex],
						temperatureK,ambientTemperatureK,ambient,ambientGasDerivative,
						ignoredDerivative,error) ) return false;
				const double hotT4 = temperatureK*temperatureK*temperatureK*temperatureK;
				const double ambientT4 = ambientTemperatureK*ambientTemperatureK*
					ambientTemperatureK*ambientTemperatureK;
				result.exchangeWPerM3 += 4.0*sigmaSB*moleculesPerM3*
					(hot*hotT4-ambient*ambientT4);
				result.temperatureDerivativeWPerM3K += 4.0*sigmaSB*moleculesPerM3*
					((hotGasDerivative+hotRadiationDerivative)*hotT4+
					4.0*hot*temperatureK*temperatureK*temperatureK-
					ambientGasDerivative*ambientT4);
			}
			return (std::isfinite(result.exchangeWPerM3) &&
				std::isfinite(result.temperatureDerivativeWPerM3K)) ||
				Fail(error,"fire solver gas exchange evaluation overflowed");
		}

		inline double MixtureCertifiedCpLowerJPerM3K(
			const MethaneCellState& state,
			const double lowerK,
			const double upperK,
			const FireSimulationMethaneRecord& thermochemistry
			)
		{
			static const char* names[MethaneSpeciesCount] = {
				"CH4", "O2", "N2", "CO2", "H2O", "CO", "C(gr)"
			};
			double result = 0.0;
			for( std::size_t index=0; index<MethaneSpeciesCount; ++index ) {
				const FireThermochemistrySpecies* species = thermochemistry.FindSpecies(names[index]);
				if( !species ) return -1.0;
				double speciesLower = std::numeric_limits<double>::infinity();
				for( const FireThermochemistrySegment& segment : species->segments ) {
					if( segment.temperatureMaxK >= lowerK && segment.temperatureMinK <= upperK ) {
						speciesLower = std::min(speciesLower,segment.certifiedCpLowerJPerKGK);
					}
				}
				if( !std::isfinite(speciesLower) ) return -1.0;
				result += state.constituent[index]*speciesLower;
			}
			return result;
		}

		inline double IntervalProductLower(
			const double firstMinimum,
			const double firstMaximum,
			const double secondMinimum,
			const double secondMaximum
			)
		{
			return std::min(std::min(firstMinimum*secondMinimum,
				firstMinimum*secondMaximum),std::min(firstMaximum*secondMinimum,
				firstMaximum*secondMaximum));
		}

		inline bool CertifiedGasExchangeDerivativeLower(
			const MethaneCellState& state,
			const double lowerK,
			const double upperK,
			const double ambientTemperatureK,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationGasOpacityRecord& opacity,
			double& result,
			std::string* error = 0
			)
		{
			const double sigmaSB = 5.670374419e-8;
			const double avogadro = 6.02214076e23;
			if( lowerK <= 0.0 || upperK < lowerK ) {
				return Fail(error,"fire solver gas-exchange derivative interval is invalid");
			}
			result = 0.0;
			const char* ids[2] = {"CO2", "H2O"};
			const std::size_t indices[2] = {MethaneCO2,MethaneH2O};
			const double midpoint = 0.5*(lowerK+upperK);
			const double radius = 0.5*(upperK-lowerK);
			const double lower2 = lowerK*lowerK;
			const double upper2 = upperK*upperK;
			const double lower3 = lower2*lowerK;
			const double lower4 = lower2*lower2;
			const double upper4 = upper2*upper2;
			const double ambient2 = ambientTemperatureK*ambientTemperatureK;
			const double ambient4 = ambient2*ambient2;
			for( std::size_t speciesIndex=0; speciesIndex<2; ++speciesIndex ) {
				const FireThermochemistrySpecies* species = thermochemistry.FindSpecies(ids[speciesIndex]);
				if( !species ) return Fail(error,"fire solver gas opacity species lacks thermochemistry");
				const double moleculesPerM3 = state.constituent[indices[speciesIndex]]/
					species->molecularWeightKGPerKMol*1000.0*avogadro;
				double hot = 0.0, hotGasPoint = 0.0, hotRadiationPoint = 0.0;
				double ambient = 0.0, ambientGasPoint = 0.0, ambientRadiationPoint = 0.0;
				if( !opacity.PlanckMeanCrossSectionM2PerMolecule(ids[speciesIndex],
					midpoint,midpoint,hot,hotGasPoint,hotRadiationPoint,error) ||
					!opacity.PlanckMeanCrossSectionM2PerMolecule(ids[speciesIndex],
						midpoint,ambientTemperatureK,ambient,ambientGasPoint,
						ambientRadiationPoint,error) ) return false;
				double hotGasMinimum = 0.0, hotGasMaximum = 0.0;
				double hotRadiationMinimum = 0.0, hotRadiationMaximum = 0.0;
				double ambientGasMinimum = 0.0, ambientGasMaximum = 0.0;
				double ambientRadiationMinimum = 0.0, ambientRadiationMaximum = 0.0;
				if( !opacity.PlanckMeanDerivativeEnclosure(ids[speciesIndex],
					lowerK,upperK,lowerK,upperK,hotGasMinimum,hotGasMaximum,
					hotRadiationMinimum,hotRadiationMaximum,error) ||
					!opacity.PlanckMeanDerivativeEnclosure(ids[speciesIndex],
						lowerK,upperK,ambientTemperatureK,ambientTemperatureK,
						ambientGasMinimum,ambientGasMaximum,ambientRadiationMinimum,
						ambientRadiationMaximum,error) ) return false;
				const double hotDerivativeMinimum = hotGasMinimum+hotRadiationMinimum;
				const double hotDerivativeMaximum = hotGasMaximum+hotRadiationMaximum;
				const double hotLipschitz = std::max(std::fabs(hotDerivativeMinimum),
					std::fabs(hotDerivativeMaximum));
				const double hotMinimum = std::max(0.0,hot-hotLipschitz*radius);
				const double hotTermLower = IntervalProductLower(hotDerivativeMinimum,
					hotDerivativeMaximum,lower4,upper4)+4.0*hotMinimum*lower3;
				const double speciesLower = 4.0*sigmaSB*moleculesPerM3*
					(hotTermLower-ambientGasMaximum*ambient4);
				if( !std::isfinite(speciesLower) ) {
					return Fail(error,"fire solver gas-exchange derivative enclosure overflowed");
				}
				result += speciesLower;
			}
			return std::isfinite(result) ||
				Fail(error,"fire solver gas-exchange derivative sum overflowed");
		}

		inline bool ApplyGasRadiationBackwardEuler(
			const MethaneCellState& preRadiation,
			const double ambientTemperatureK,
			const double deltaTimeS,
			const double escapeFactor,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationGasOpacityRecord& opacity,
			MethaneCellState& result,
			double& acceptedCoolingWPerM3,
			std::string* error = 0
			)
		{
			if( !ValidateCellState(preRadiation,error) || !std::isfinite(deltaTimeS) ||
				deltaTimeS <= 0.0 || !std::isfinite(escapeFactor) || escapeFactor < 0.0 ||
				escapeFactor > 1.0 || ambientTemperatureK < opacity.TemperatureMinK() ||
				ambientTemperatureK > opacity.TemperatureMaxK() ||
				preRadiation.temperatureK < opacity.TemperatureMinK() ||
				preRadiation.temperatureK > opacity.TemperatureMaxK() ) {
				return Fail(error,"fire solver radiation map is outside its certified domain");
			}
			if( preRadiation.temperatureK == ambientTemperatureK || escapeFactor == 0.0 ) {
				result = preRadiation; acceptedCoolingWPerM3 = 0.0; return true;
			}
			const double lower = std::min(ambientTemperatureK,preRadiation.temperatureK);
			const double upper = std::max(ambientTemperatureK,preRadiation.temperatureK);
			const double cpLower = MixtureCertifiedCpLowerJPerM3K(
				preRadiation,lower,upper,thermochemistry);
			if( cpLower <= 0.0 ) return Fail(error,"fire solver radiation map lacks a positive C_T bound");
			const FireGasOpacitySpecies* co2 = opacity.FindSpecies("CO2");
			const FireGasOpacitySpecies* h2o = opacity.FindSpecies("H2O");
			if( !co2 || !h2o ) return Fail(error,"fire solver radiation record is incomplete");
			std::vector<double> knots = {lower,upper};
			for( const double value : co2->gasTemperatureAxisK ) if( value > lower && value < upper ) knots.push_back(value);
			for( const double value : co2->radiationTemperatureAxisK ) if( value > lower && value < upper ) knots.push_back(value);
			for( const double value : h2o->gasTemperatureAxisK ) if( value > lower && value < upper ) knots.push_back(value);
			for( const double value : h2o->radiationTemperatureAxisK ) if( value > lower && value < upper ) knots.push_back(value);
			std::sort(knots.begin(),knots.end());
			knots.erase(std::unique(knots.begin(),knots.end()),knots.end());
			for( std::size_t interval=0; interval+1<knots.size(); ++interval ) {
				const double lo = knots[interval], hi = knots[interval+1];
				double derivativeLower = 0.0;
				if( !CertifiedGasExchangeDerivativeLower(preRadiation,lo,hi,
					ambientTemperatureK,thermochemistry,opacity,derivativeLower,error) ) return false;
				if( cpLower+deltaTimeS*escapeFactor*derivativeLower <= 0.0 ) {
					return Fail(error,"fire solver radiation F-prime enclosure is not strictly positive");
				}
			}
			auto residual = [&]( const double temperature, double& value ) {
				double energy = 0.0;
				if( !thermochemistry.MixtureSensibleEnergyJPerM3(
					ThermochemicalDensities(preRadiation),temperature,energy,error) ) return false;
				GasExchangeEvaluation exchange;
				if( !EvaluateGasExchange(preRadiation,temperature,ambientTemperatureK,
					thermochemistry,opacity,exchange,error) ) return false;
				value = energy-preRadiation.sensibleEnergyJPerM3+
					deltaTimeS*escapeFactor*exchange.exchangeWPerM3;
				return std::isfinite(value);
			};
			double fLower = 0.0, fUpper = 0.0;
			if( !residual(lower,fLower) || !residual(upper,fUpper) || fLower > 0.0 || fUpper < 0.0 ) {
				return Fail(error,"fire solver radiation map lacks its certified endpoint sign change");
			}
			double lo = lower, hi = upper;
			for( std::size_t iteration=0; iteration<160; ++iteration ) {
				const double midpoint = 0.5*(lo+hi);
				double value = 0.0;
				if( !residual(midpoint,value) ) return false;
				if( value > 0.0 ) hi = midpoint; else lo = midpoint;
				if( hi-lo <= 8.0*std::numeric_limits<double>::epsilon()*
					std::max(1.0,midpoint) ) break;
			}
			MethaneCellState candidate = preRadiation;
			candidate.temperatureK = 0.5*(lo+hi);
			double finalResidual = 0.0;
			if( !residual(candidate.temperatureK,finalResidual) ) return false;
			if( !thermochemistry.MixtureSensibleEnergyJPerM3(
				ThermochemicalDensities(candidate),candidate.temperatureK,
				candidate.sensibleEnergyJPerM3,error) ) return false;
			acceptedCoolingWPerM3 = (preRadiation.sensibleEnergyJPerM3-
				candidate.sensibleEnergyJPerM3)/deltaTimeS;
			const double energyScale = std::max({1.0,
				std::fabs(preRadiation.sensibleEnergyJPerM3),
				std::fabs(candidate.sensibleEnergyJPerM3)});
			if( std::fabs(finalResidual) > 64.0*std::numeric_limits<double>::epsilon()*
				energyScale ) return Fail(error,"fire solver radiation root misses its energy residual tolerance");
			if( !std::isfinite(acceptedCoolingWPerM3) ) return Fail(error,
				"fire solver accepted radiative exchange is invalid");
			result = candidate;
			return true;
		}

		inline bool BuildFrozenMethaneSourcePacket(
			const MethaneCellState& beginning,
			const MethaneReactionStep& reactionStep,
			const double ambientTemperatureK,
			const double escapeFactor,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationGasOpacityRecord& opacity,
			MethaneSourcePacket& result,
			std::string* error = 0
			)
		{
			MethaneSourcePacket reaction;
			if( !BuildMethaneReactionPacket(beginning,fuel,reactionStep,reaction,error) ) return false;
			MethaneCellState postReaction;
			if( !ApplySourcePacket(beginning,reaction,thermochemistry,postReaction,error) ) return false;
			MethaneCellState finalScratch;
			double signedCoolingWPerM3 = 0.0;
			if( !ApplyGasRadiationBackwardEuler(postReaction,ambientTemperatureK,
				reactionStep.deltaTimeS,escapeFactor,thermochemistry,opacity,
				finalScratch,signedCoolingWPerM3,error) ) return false;
			result = reaction;
			for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
				result.constituentDelta[species] = finalScratch.constituent[species]-
					beginning.constituent[species];
			}
			result.sensibleEnergyDeltaJPerM3 = finalScratch.sensibleEnergyJPerM3-
				beginning.sensibleEnergyJPerM3;
			result.radiativeCoolingWPerM3 = signedCoolingWPerM3;
			double massResidual = 0.0;
			for( const double delta : result.constituentDelta ) massResidual += delta;
			double elementResidual = 0.0;
			const std::vector<double>& element = fuel.ElementMassFractionMatrix();
			for( std::size_t row=0; row<fuel.ElementOrder().size(); ++row ) {
				double residual = 0.0;
				for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
					residual += element[row*MethaneSpeciesCount+species]*
						result.constituentDelta[species];
				}
				elementResidual = std::max(elementResidual,std::fabs(residual));
			}
			const double scale = std::max(1.0,beginning.TotalDensity());
			const double tolerance = 4096.0*std::numeric_limits<double>::epsilon()*scale;
			const double expectedEnergy = reaction.sensibleEnergyDeltaJPerM3-
				reactionStep.deltaTimeS*signedCoolingWPerM3;
			const double energyTolerance = 4096.0*std::numeric_limits<double>::epsilon()*
				std::max(1.0,std::fabs(expectedEnergy));
			return (std::fabs(massResidual) <= tolerance && elementResidual <= tolerance &&
				std::fabs(result.sensibleEnergyDeltaJPerM3-expectedEnergy) <= energyTolerance) ||
				Fail(error,"fire solver frozen source packet failed its mass/element/energy ledger");
		}

		inline bool BuildFrozenMethaneSourcePackets(
			const std::vector<MethaneCellState>& beginning,
			const std::vector<MethaneReactionStep>& reactionStep,
			const std::vector<double>& cellVolumeM3,
			const double ambientTemperatureK,
			const double totalHeatReleaseW,
			const double nominalPeakHeatReleaseW,
			const double radiativeFraction,
			const bool predictive,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationGasOpacityRecord& opacity,
			std::vector<MethaneSourcePacket>& result,
			RadiationEscapeFactor& factor,
			std::string* error = 0
			)
		{
			const std::size_t count = beginning.size();
			if( count == 0 || reactionStep.size() != count || cellVolumeM3.size() != count ) {
				return Fail(error,"fire solver grid source-packet arrays are malformed");
			}
			std::vector<double> unscaledExchange(count,0.0);
			for( std::size_t cell=0; cell<count; ++cell ) {
				MethaneSourcePacket reaction;
				MethaneCellState postReaction;
				if( !BuildMethaneReactionPacket(beginning[cell],fuel,reactionStep[cell],
					reaction,error) || !ApplySourcePacket(beginning[cell],reaction,
					thermochemistry,postReaction,error) ) return false;
				GasExchangeEvaluation exchange;
				if( !EvaluateGasExchange(postReaction,postReaction.temperatureK,
					ambientTemperatureK,thermochemistry,opacity,exchange,error) ) return false;
				unscaledExchange[cell] = exchange.exchangeWPerM3;
			}
			if( !ComputeRadiationEscapeFactor(totalHeatReleaseW,nominalPeakHeatReleaseW,
				radiativeFraction,unscaledExchange,cellVolumeM3,predictive,factor,error) ) return false;
			result.assign(count,MethaneSourcePacket());
			for( std::size_t cell=0; cell<count; ++cell ) {
				if( !BuildFrozenMethaneSourcePacket(beginning[cell],reactionStep[cell],
					ambientTemperatureK,factor.accepted,fuel,thermochemistry,opacity,
					result[cell],error) ) return false;
			}
			return true;
		}
	}
}

#endif
