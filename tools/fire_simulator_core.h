//////////////////////////////////////////////////////////////////////
//
//  fire_simulator_core.h - Standalone Phase-C solver kernels
//
//  The offline solver remains assembled in tools/.  Canonical methane source
//  arithmetic is shared from src/Library/Utilities/FireProductionSourceKernel.h;
//  tools no longer owns a second source-map implementation.
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_FIRE_SIMULATOR_CORE_
#define RISE_FIRE_SIMULATOR_CORE_

#include "../src/Library/Utilities/FireSimulationRecords.h"
#include "../src/Library/Utilities/FireCase.h"
#include "../src/Library/Utilities/FireProductionTransport.h"
#include "../src/Library/Utilities/FireProductionSourceKernel.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <functional>
#include <exception>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace RISE
{
	namespace FireSim
	{

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
			FireStateProducerPrecision producerPrecision;
			PeriodicTransportConfig() : cellWidthM(0.0), deltaTimeS(0.0),
				ambientTemperatureK(0.0), adiabaticTemperatureK(0.0),
				ambientGasDensityKGPerM3(0.0), gravityMPerS2(0.0),
				producerPrecision(FireStateProducerPrecision::Binary64) {}
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

		inline double InequalityRoundoffScale(
			const ConservativeVector& state,
			const std::size_t inequality,
			const std::array<double,MethaneSpeciesCount>& ambientEnthalpy,
			const std::array<double,MethaneSpeciesCount>& adiabaticEnthalpy
			)
		{
			return inequality<2+MethaneSpeciesCount?AcceptedStateMassScale(state):
				AcceptedStateEnergyScale(state,ambientEnthalpy,adiabaticEnthalpy);
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

		inline double CertifiedLimiterInequalityBudget(
			const ConservativeVector& low,
			const std::size_t inequality,
			const std::array<double,MethaneSpeciesCount>& ambientEnthalpy,
			const std::array<double,MethaneSpeciesCount>& adiabaticEnthalpy,
			const FireSimulationMethaneRecord& fuel,
			const double rowScaleLowerBound
			)
		{
			const FireAcceptedStateFeasibilityEnvelope& envelope=
				fuel.AcceptedStateFeasibilityEnvelope();
			if(!std::isfinite(rowScaleLowerBound)||rowScaleLowerBound<1.0)return 0.0;
			const double epsilon=std::numeric_limits<double>::epsilon();
			const double admissible=envelope.kappaEpsilon64*epsilon*rowScaleLowerBound;
			const double assemblyReserve=envelope.limiterOutwardFactorEpsilon64*
				epsilon*rowScaleLowerBound;
			return std::max(0.0,admissible-assemblyReserve-
				InequalityValue(low,inequality,ambientEnthalpy,adiabaticEnthalpy));
		}

		inline double CertifiedLimiterScaleLowerBound(
			const ConservativeVector& low,
			const std::array<ConservativeVector,6>& correction,
			const std::size_t correctionCount,
			const std::size_t inequality,
			const std::array<double,MethaneSpeciesCount>& ambientEnthalpy,
			const std::array<double,MethaneSpeciesCount>& adiabaticEnthalpy
			)
		{
			auto componentMinimumAbsolute=[&](const std::size_t component){
				double lower=low[component],upper=low[component];
				for(std::size_t direction=0;direction<correctionCount;++direction){
					const double delta=correction[direction][component];
					if(delta<0.0)lower+=delta;else upper+=delta;
				}
				return lower<=0.0&&upper>=0.0?0.0:
					std::min(std::fabs(lower),std::fabs(upper));
			};
			if(inequality<2+MethaneSpeciesCount){
				double result=componentMinimumAbsolute(0);
				for(std::size_t species=0;species<MethaneSpeciesCount;++species)
					result+=componentMinimumAbsolute(1+species);
				return std::max(1.0,result);
			}
			double result=componentMinimumAbsolute(MethaneMassStateDimension);
			for(std::size_t species=0;species<MethaneSpeciesCount;++species)
				result+=(std::fabs(ambientEnthalpy[species])+
					std::fabs(adiabaticEnthalpy[species]))*
					componentMinimumAbsolute(1+species);
			return std::max(1.0,result);
		}

		inline bool CertifiedConstraintRowsSatisfied(
			const ConservativeVector& state,
			const FireCertifiedNullspace& closure,
			const FireAcceptedStateFeasibilityEnvelope& envelope,
			const FireStateProducerPrecision producerPrecision
			)
		{
			const double roundoffFactor=AcceptedStateRoundoffFactor(envelope,producerPrecision);
			if(!(roundoffFactor>0.0)||!std::isfinite(roundoffFactor)||
				closure.stateDimension>
				MethaneMassStateDimension)return false;
			for( std::size_t row=0; row<closure.constraintRows; ++row ) {
				double scale = 0.0, residual = 0.0;
				for( std::size_t column=0; column<closure.stateDimension; ++column ) {
					const double coefficient = closure.constraintMatrix[
						row*closure.stateDimension+column];
					const double term=coefficient*state[column];
					scale+=std::fabs(term);residual+=term;
				}
				const double bound=roundoffFactor*std::max(1.0,scale);
				if(!std::isfinite(residual)||std::fabs(residual)>bound)return false;
			}
			return true;
		}

		inline bool CertifiedConstraintRowsSatisfied(const ConservativeVector& state,
			const FireCertifiedNullspace& closure,
			const FireAcceptedStateFeasibilityEnvelope& envelope)
		{
			return CertifiedConstraintRowsSatisfied(state,closure,envelope,
				FireStateProducerPrecision::Binary64);
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
			std::string* error = 0,
			const std::vector<double>* acceptedFaceAlpha = 0
			)
		{
			const std::size_t count = beginning.size();
			if( count < 3 || flux.low.size() != count || flux.high.size() != count ||
				sourcePerS.size() != count || !std::isfinite(config.deltaTimeS) ||
				config.deltaTimeS <= 0.0 || !std::isfinite(config.cellWidthM) ||
				config.cellWidthM <= 0.0 ) {
				return Fail(error,"fire solver FCT input is malformed");
			}
			if(config.producerPrecision==FireStateProducerPrecision::Binary32)
				for(const ConservativeVector& source:sourcePerS)
					if(!CertifiedBinary32ZeroSource(source))return Fail(error,
						"fire solver binary32 source producer is not yet certified");
			std::array<double,MethaneSpeciesCount> ambientEnthalpy, adiabaticEnthalpy;
			if( !FireSimulationEnthalpyBounds(config,thermochemistry,
				ambientEnthalpy,adiabaticEnthalpy,error) ) return false;
			std::vector<ConservativeVector> low(count), leftCorrection(count), rightCorrection(count);
			const double scale = config.deltaTimeS/config.cellWidthM;
			for( std::size_t cell=0; cell<count; ++cell ) {
				if(!AcceptedStateAdmissible(beginning[cell],ambientEnthalpy,
					adiabaticEnthalpy,fuel,config.producerPrecision,error))return false;
				const std::size_t leftFace = (cell+count-1)%count;
				const std::size_t rightFace = cell;
				low[cell] = beginning[cell]+config.deltaTimeS*sourcePerS[cell]+
					scale*(flux.low[leftFace]-flux.low[rightFace]);
				leftCorrection[cell] = scale*(flux.high[leftFace]-flux.low[leftFace]);
				rightCorrection[cell] = -scale*(flux.high[rightFace]-flux.low[rightFace]);
				if(!AcceptedStateAdmissible(low[cell],ambientEnthalpy,
					adiabaticEnthalpy,fuel,config.producerPrecision,error))return false;
			}
			const std::size_t inequalityCount = 4+MethaneSpeciesCount;
			std::vector<std::vector<double> > ratio(count,
				std::vector<double>(inequalityCount,1.0));
			for( std::size_t cell=0; cell<count; ++cell ) {
				std::array<ConservativeVector,6> limiterCorrection={};
				limiterCorrection[0]=leftCorrection[cell];
				limiterCorrection[1]=rightCorrection[cell];
				for( std::size_t inequality=0; inequality<inequalityCount; ++inequality ) {
					const double scaleLowerBound=CertifiedLimiterScaleLowerBound(low[cell],
						limiterCorrection,2,inequality,ambientEnthalpy,adiabaticEnthalpy);
					const double budget=CertifiedLimiterInequalityBudget(low[cell],inequality,
						ambientEnthalpy,adiabaticEnthalpy,fuel,scaleLowerBound);
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
			if(acceptedFaceAlpha){
				if(acceptedFaceAlpha->size()!=faceAlpha.size()) return Fail(error,
					"fire solver accepted limiter shape is invalid");
				for(std::size_t face=0;face<faceAlpha.size();++face){
					const double accepted=(*acceptedFaceAlpha)[face];
					if(!std::isfinite(accepted)||accepted<0.0||accepted>faceAlpha[face])
						return Fail(error,"fire solver accepted limiter exceeds its certificate");
				}
				faceAlpha=*acceptedFaceAlpha;
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
				if(!AcceptedStateAdmissible(result[cell],ambientEnthalpy,
					adiabaticEnthalpy,fuel,config.producerPrecision,error))return false;
			}
			return true;
		}

		inline bool InvertPeriodicTemperaturesWithinBounds(
			const std::vector<ConservativeVector>& cells,
			const FireSimulationMethaneRecord& thermochemistry,
			const double lowerTemperatureK,
			const double upperTemperatureK,
			const FireStateProducerPrecision producerPrecision,
			std::vector<double>& temperatureK,
			std::string* error = 0,
			const unsigned int workerCount = 1u,
			const bool enforceAbsoluteReferencePressureGate = true
			)
		{
			FireProfileIncrement(FireProfile().invertTCalls);
			FireProfileScopedNs profileTimer(FireProfile().nsInvertT);
			std::vector<double> candidate(cells.size(),0.0);
			if(cells.empty()) { temperatureK.clear();return true; }
			const unsigned int workers=std::max(1u,std::min(workerCount,
				static_cast<unsigned int>(cells.size())));
			FireProfileIncrement(FireProfile().spawnCycles);
			FireProfileIncrement(FireProfile().spawnThreads,workers);
			const std::size_t noFailure=std::numeric_limits<std::size_t>::max();
			std::vector<std::size_t> failureCell(workers,noFailure);
			std::vector<std::string> failureMessage(workers);
			std::vector<std::thread> threads;
			for(unsigned int worker=0;worker<workers;++worker) threads.emplace_back([&,worker]() {
				const std::size_t first=cells.size()*worker/workers;
				const std::size_t last=cells.size()*(worker+1u)/workers;
				for(std::size_t cell=first;cell<last;++cell) {
				MethaneCellState state = FromConservativeVector(cells[cell],producerPrecision);
				std::string inversionError;
				if(!InvertMethaneTemperatureWithinAcceptedEnvelope(state,lowerTemperatureK,
					upperTemperatureK,thermochemistry,producerPrecision,candidate[cell],
					&inversionError)) {
					std::ostringstream message;
					message << "fire solver cell " << cell << " temperature inversion failed: "
						<< inversionError << "; constituents=";
					for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
						message << (species ? "," : "") << state.constituent[species];
					}
					failureCell[worker]=cell;failureMessage[worker]=message.str();break;
				}
				state.temperatureK = candidate[cell];
				double equationOfStateResidual = 0.0;
				if( enforceAbsoluteReferencePressureGate &&
					(!EquationOfStateResidual(state,thermochemistry,producerPrecision,
						equationOfStateResidual,&inversionError) ||
					 equationOfStateResidual > 1.0e-3) ) {
					std::ostringstream message;
					message << "fire solver cell " << cell
						<< " violates the accepted-state EOS gate: residual="
						<< equationOfStateResidual << "; " << inversionError << "; constituents=";
					for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
						message << (species ? "," : "") << state.constituent[species];
					}
					failureCell[worker]=cell;failureMessage[worker]=message.str();break;
				}
				}
			});
			for(std::thread& thread:threads) thread.join();
			std::size_t firstFailure=noFailure;unsigned int failedWorker=0u;
			for(unsigned int worker=0;worker<workers;++worker) if(failureCell[worker]<firstFailure) {
				firstFailure=failureCell[worker];failedWorker=worker;
			}
			if(firstFailure!=noFailure) return Fail(error,failureMessage[failedWorker]);
			temperatureK.swap(candidate);
			return true;
		}

		inline bool InvertPeriodicTemperaturesWithinBounds(
			const std::vector<ConservativeVector>& cells,
			const FireSimulationMethaneRecord& thermochemistry,
			const double lowerTemperatureK,const double upperTemperatureK,
			std::vector<double>& temperatureK,std::string* error=0,
			const unsigned int workerCount=1u)
		{
			return InvertPeriodicTemperaturesWithinBounds(cells,thermochemistry,
				lowerTemperatureK,upperTemperatureK,FireStateProducerPrecision::Binary64,
				temperatureK,error,workerCount);
		}

		inline bool InvertPeriodicTemperatures(
			const std::vector<ConservativeVector>& cells,
			const FireSimulationMethaneRecord& thermochemistry,
			std::vector<double>& temperatureK,
			std::string* error = 0
			)
		{
			return InvertPeriodicTemperaturesWithinBounds(cells,thermochemistry,
				thermochemistry.TemperatureMinK(),thermochemistry.TemperatureMaxK(),
				temperatureK,error);
		}

		inline bool PeriodicDivergenceTargetFromPhysicalFlux(
			const std::vector<ConservativeVector>& state,
			const std::vector<double>& temperatureK,
			const PeriodicFluxPair& flux,
			const std::vector<ConservativeVector>& frozenSourcePerS,
			const double cellWidthM,
			const double deltaTimeS,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireStateProducerPrecision producerPrecision,
			std::vector<double>& result,
			std::string* error = 0
			)
		{
			const std::size_t count = state.size();
			if( count < 3 || temperatureK.size() != count || flux.nonadvectiveMass.size() != count ||
				flux.nonadvectiveEnergy.size() != count || frozenSourcePerS.size() != count ||
				!std::isfinite(deltaTimeS) || deltaTimeS <= 0.0 ) {
				return Fail(error,"fire solver divergence target arrays are malformed");
			}
			result.assign(count,0.0);
			for( std::size_t cell=0; cell<count; ++cell ) {
				const std::size_t leftFace = (cell+count-1)%count;
				ConservativeVector increment = deltaTimeS*frozenSourcePerS[cell];
				for( std::size_t component=0; component<MethaneMassStateDimension; ++component ) {
					increment[component] += deltaTimeS*(flux.nonadvectiveMass[leftFace][component]-
						flux.nonadvectiveMass[cell][component])/cellWidthM;
				}
				increment[MethaneMassStateDimension] += deltaTimeS*(flux.nonadvectiveEnergy[leftFace]-
					flux.nonadvectiveEnergy[cell])/cellWidthM;
				if( !DivergenceFromDiscreteIncrement(state[cell],increment,temperatureK[cell],
					deltaTimeS,thermochemistry,producerPrecision,result[cell],error) ) return false;
			}
			return true;
		}

		inline bool PeriodicDivergenceTargetFromPhysicalFlux(
			const std::vector<ConservativeVector>& state,
			const std::vector<double>& temperatureK,const PeriodicFluxPair& flux,
			const std::vector<ConservativeVector>& frozenSourcePerS,const double cellWidthM,
			const double deltaTimeS,const FireSimulationMethaneRecord& thermochemistry,
			std::vector<double>& result,std::string* error=0)
		{
			return PeriodicDivergenceTargetFromPhysicalFlux(state,temperatureK,flux,
				frozenSourcePerS,cellWidthM,deltaTimeS,thermochemistry,
				FireStateProducerPrecision::Binary64,result,error);
		}

		// Test-only one-dimensional reference.  Production state is owned only by
		// AdvanceConservative3D below.
		inline bool ReferenceAdvancePeriodicTransportHeun1D(
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
			if( !InvertPeriodicTemperaturesWithinBounds(beginning,thermochemistry,
				config.ambientTemperatureK,config.adiabaticTemperatureK,
				config.producerPrecision,temperature0,error) ) return false;
			PeriodicFluxPair flux0;
			if( !BuildPeriodicFluxPair(beginning,temperature0,faceVelocityMPerS,
				diffusivityM2PerS,conductivityWPerMK,config.cellWidthM,fuel,
				thermochemistry,flux0,error) ) return false;
			std::vector<ConservativeVector> predictor;
			std::vector<double> predictorAlpha;
			if( !ApplyPeriodicSharedFCT(beginning,flux0,frozenSourcePerS,config,
				fuel,thermochemistry,predictor,predictorAlpha,error) ||
				!InvertPeriodicTemperaturesWithinBounds(predictor,thermochemistry,
					config.ambientTemperatureK,config.adiabaticTemperatureK,
					config.producerPrecision,temperature1,error) ) return false;
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

		enum OpenBoundaryKind3D
		{
			PressureOpenBoundary3D,
			AdiabaticWallBoundary3D,
			FuelInletBoundary3D
		};

		struct OpenMACField3D
		{
			std::array<std::vector<double>,3> component;
		};

		struct OpenBoundaryConfig3D
		{
			std::array<unsigned int,6> kind;
			std::array<std::vector<bool>,6> priorInflow;
			std::vector<bool> bottomFuelMask;
			std::vector<double> bottomFuelMassFluxKGPerM2S;
			ConservativeVector ambientState;
			ConservativeVector injectedState;
			double ambientDensityKGPerM3;
			double injectedGasDensityKGPerM3;
			double fuelMassFluxKGPerM2S;
			double velocityToleranceMPerS;
			double pressureTolerancePa;
			OpenBoundaryConfig3D() : ambientDensityKGPerM3(0.0),
				injectedGasDensityKGPerM3(0.0),fuelMassFluxKGPerM2S(0.0),
				velocityToleranceMPerS(0.0),pressureTolerancePa(0.0)
			{
				kind.fill(PressureOpenBoundary3D);
				kind[4] = AdiabaticWallBoundary3D;
			}
		};

		inline double OpenBoundaryFuelMassFluxKGPerM2S(
			const OpenBoundaryConfig3D& boundary,
			const unsigned int side,
			const std::size_t boundaryIndex )
		{
			return side==4u && !boundary.bottomFuelMassFluxKGPerM2S.empty() ?
				boundary.bottomFuelMassFluxKGPerM2S[boundaryIndex] :
				boundary.fuelMassFluxKGPerM2S;
		}

		struct OpenMACProjection3DResult
		{
			OpenMACField3D faceDensityKGPerM3;
			OpenMACField3D velocityMPerS;
			OpenMACField3D momentumKGPerM2S;
			std::vector<double> stepAverageDynamicPressurePa;
			std::array<std::vector<bool>,6> inflow;
			std::array<std::vector<double>,6> boundaryDynamicPressurePa;
			std::array<std::array<std::vector<double>,2>,6>
				boundaryTangentialVelocityMPerS;
			double maximumDivergenceResidualPerS;
			double maximumPreProjectionDivergenceResidualPerS;
			double maximumBoundaryHeadResidualPa;
			double maximumActiveSetComplementarityDiscrepancyMPerS;
			std::size_t activeSetCycleLength;
			std::size_t activeSetDifferingFaceCount;
			bool activeSetDiscontinuousClass;
			std::vector<double> nonlinearResidualHistory;
			std::vector<double> multigridResidualHistoryPerS;
			OpenMACProjection3DResult() : maximumDivergenceResidualPerS(0.0),
				maximumPreProjectionDivergenceResidualPerS(0.0),
				maximumBoundaryHeadResidualPa(0.0),
				maximumActiveSetComplementarityDiscrepancyMPerS(0.0),
				activeSetCycleLength(0u),activeSetDifferingFaceCount(0u),
				activeSetDiscontinuousClass(false) {}
		};

		inline std::vector<unsigned char> FlattenOpenActiveSet3D(
			const std::array<std::vector<bool>,6>& inflow )
		{
			std::vector<unsigned char> result;
			for( unsigned int side=0; side<6; ++side ) for( const bool value :
				inflow[side] ) result.push_back(value?1u:0u);
			return result;
		}

		inline std::size_t OpenActiveSetDifferingFaceCount3D(
			const std::array<std::vector<bool>,6>& first,
			const std::array<std::vector<bool>,6>& second )
		{
			std::size_t result=0u;
			for( unsigned int side=0; side<6; ++side ) for( std::size_t face=0;
				face<first[side].size() && face<second[side].size(); ++face )
				result+=first[side][face]!=second[side][face]?1u:0u;
			return result;
		}

		struct OpenBoundaryFlux3D
		{
			ConservativeVector totalOutwardFlux;
			std::array<double,MethaneMassStateDimension> nonadvectiveMassOutwardFlux;
			double nonadvectiveEnergyOutwardFlux;
			OpenBoundaryFlux3D() : nonadvectiveEnergyOutwardFlux(0.0)
			{
				nonadvectiveMassOutwardFlux.fill(0.0);
			}
		};

		inline bool OpenScalarInflow3D(
			const double outwardVelocityMPerS,
			const bool pressureInflow,
			const double velocityToleranceMPerS )
		{
			if(outwardVelocityMPerS < -velocityToleranceMPerS)return true;
			if(outwardVelocityMPerS > velocityToleranceMPerS)return false;
			return pressureInflow;
		}

		inline bool BuildOpenBoundaryFlux3D(
			const ConservativeVector& interior,
			const double interiorTemperatureK,
			const double outwardVelocityMPerS,
			const unsigned int kind,
			const bool inflow,
			const OpenBoundaryConfig3D& boundary,
			const double ambientTemperatureK,
			const double injectedTemperatureK,
			const double rhoDiffusivityKGPerMS,
			const double conductivityWPerMK,
			const double cellWidthM,
			const double fuelMassFluxKGPerM2S,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			OpenBoundaryFlux3D& result,
			std::string* error = 0
			)
		{
			result = OpenBoundaryFlux3D();
			if( (kind!=PressureOpenBoundary3D && kind!=AdiabaticWallBoundary3D &&
				kind!=FuelInletBoundary3D) || !std::isfinite(outwardVelocityMPerS) ||
				!std::isfinite(interiorTemperatureK) || interiorTemperatureK <= 0.0 ||
				!std::isfinite(ambientTemperatureK) || ambientTemperatureK <= 0.0 ||
				!std::isfinite(injectedTemperatureK) || injectedTemperatureK <= 0.0 ||
				!std::isfinite(rhoDiffusivityKGPerMS) || rhoDiffusivityKGPerMS < 0.0 ||
				!std::isfinite(conductivityWPerMK) || conductivityWPerMK < 0.0 ||
				!std::isfinite(cellWidthM) || cellWidthM <= 0.0 ||
				!std::isfinite(fuelMassFluxKGPerM2S) || fuelMassFluxKGPerM2S < 0.0 ||
				!fuel.IsValid() || !thermochemistry.IsValid() ) {
				return Fail(error,"fire solver open scalar-boundary input is invalid");
			}
			for( std::size_t component=0; component<MethaneConservativeDimension; ++component ) {
				if( !std::isfinite(interior[component]) ||
					!std::isfinite(boundary.ambientState[component]) ||
					!std::isfinite(boundary.injectedState[component]) ) return Fail(error,
					"fire solver open scalar-boundary state is non-finite");
			}
			if( kind == AdiabaticWallBoundary3D ) return true;
			if( kind == FuelInletBoundary3D ) {
				double injectedTotal = 0.0;
				for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
					injectedTotal += boundary.injectedState[1+species];
				}
				if( injectedTotal <= 0.0 || !std::isfinite(injectedTotal) ) return Fail(error,
					"fire solver injected conservative ghost is invalid");
				for( std::size_t component=0; component<MethaneMassStateDimension; ++component ) {
					result.totalOutwardFlux[component] = -fuelMassFluxKGPerM2S*
						boundary.injectedState[component]/injectedTotal;
					if( !std::isfinite(result.totalOutwardFlux[component]) ) return Fail(error,
						"fire solver fuel-bed constituent flux overflowed");
				}
				result.totalOutwardFlux[MethaneMassStateDimension] =
					-fuelMassFluxKGPerM2S*
					boundary.injectedState[MethaneMassStateDimension]/injectedTotal;
				if( !std::isfinite(result.totalOutwardFlux[MethaneMassStateDimension]) ) {
					return Fail(error,"fire solver fuel-bed enthalpy flux overflowed");
				}
				// A prescribed bed tuple is not the ordinary interior MAC donor flux.
				// The pressure solve already carries outwardVelocity*interior as its
				// volume displacement; only the difference between that carrier and
				// the imposed pure-fuel tuple belongs in the physical-flux divergence
				// identity.  Keeping the full imposed tuple in low/high preserves the
				// exact one-sided conservative ledger.
				for( std::size_t component=0; component<MethaneMassStateDimension; ++component ) {
					result.nonadvectiveMassOutwardFlux[component] =
						result.totalOutwardFlux[component]-outwardVelocityMPerS*interior[component];
				}
				result.nonadvectiveEnergyOutwardFlux =
					result.totalOutwardFlux[MethaneMassStateDimension]-
					outwardVelocityMPerS*interior[MethaneMassStateDimension];
				return true;
			}
			const bool scalarInflow=kind==PressureOpenBoundary3D?
				OpenScalarInflow3D(outwardVelocityMPerS,inflow,
					boundary.velocityToleranceMPerS):inflow;
			const ConservativeVector& donor = scalarInflow ? boundary.ambientState : interior;
			for( std::size_t component=0; component<MethaneConservativeDimension; ++component ) {
				result.totalOutwardFlux[component] = outwardVelocityMPerS*donor[component];
				if( !std::isfinite(result.totalOutwardFlux[component]) ) return Fail(error,
					"fire solver open advective flux overflowed");
			}
			if( !scalarInflow ) return true; // zero-gradient outflow suppresses every inward diffusive flux.
			bool identicalAmbient=true;
			for( std::size_t component=0; component<MethaneConservativeDimension; ++component ) {
				identicalAmbient=identicalAmbient && interior[component]==boundary.ambientState[component];
			}
			// Identical conservative ambient bytes define the same thermodynamic
			// state.  Do not manufacture a conductive flux from the inversion's
			// last-bit temperature representation.
			if( identicalAmbient ) return true;
			double interiorTotal = 0.0, ambientTotal = 0.0;
			for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
				interiorTotal += interior[1+species];
				ambientTotal += boundary.ambientState[1+species];
			}
			if( interiorTotal <= 0.0 || ambientTotal <= 0.0 ) return Fail(error,
				"fire solver ambient Dirichlet ghost has no mass");
			std::vector<double> raw(MethaneMassStateDimension,0.0),projected;
			raw[0] = -rhoDiffusivityKGPerMS*(boundary.ambientState[0]/ambientTotal-
				interior[0]/interiorTotal)/(0.5*cellWidthM);
			for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) raw[1+species] =
				-rhoDiffusivityKGPerMS*(boundary.ambientState[1+species]/ambientTotal-
					interior[1+species]/interiorTotal)/(0.5*cellWidthM);
			if( !fuel.NonadvectiveFluxProjection().Project(raw,projected,error) ||
				projected.size()!=MethaneMassStateDimension ) return false;
			static const char* names[MethaneSpeciesCount] = {
				"CH4","O2","N2","CO2","H2O","CO","C(gr)"
			};
			const double faceTemperature=0.5*(interiorTemperatureK+ambientTemperatureK);
			for( std::size_t component=0; component<MethaneMassStateDimension; ++component ) {
				result.nonadvectiveMassOutwardFlux[component]=projected[component];
				result.totalOutwardFlux[component]+=projected[component];
			}
			for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
				double enthalpy=0.0;
				if( !thermochemistry.SensibleEnthalpyJPerKG(names[species],faceTemperature,
					enthalpy,error) ) return false;
				const double contribution=enthalpy*projected[1+species];
				if( !std::isfinite(contribution) || !std::isfinite(
					result.nonadvectiveEnergyOutwardFlux+contribution) ) return Fail(error,
					"fire solver open species-enthalpy flux overflowed");
				result.nonadvectiveEnergyOutwardFlux+=contribution;
			}
			const double conductive=conductivityWPerMK*
				(ambientTemperatureK-interiorTemperatureK)/(0.5*cellWidthM);
			if( !std::isfinite(conductive) || !std::isfinite(
				result.nonadvectiveEnergyOutwardFlux-conductive) ) return Fail(error,
				"fire solver open conductive flux overflowed");
			result.nonadvectiveEnergyOutwardFlux-=conductive;
			const double totalEnergy=result.totalOutwardFlux[MethaneMassStateDimension]+
				result.nonadvectiveEnergyOutwardFlux;
			if( !std::isfinite(totalEnergy) ) return Fail(error,
				"fire solver open total enthalpy flux overflowed");
			result.totalOutwardFlux[MethaneMassStateDimension]=totalEnergy;
			return true;
		}

		inline std::size_t OpenMACFaceCount3D(
			const PeriodicMACShape& shape,
			const unsigned int axis
			)
		{
			if( axis == 0 ) return (shape.nx+1)*shape.ny*shape.nz;
			if( axis == 1 ) return shape.nx*(shape.ny+1)*shape.nz;
			return shape.nx*shape.ny*(shape.nz+1);
		}

		inline std::size_t OpenMACFaceIndex3D(
			const PeriodicMACShape& shape,
			const unsigned int axis,
			const std::size_t x,
			const std::size_t y,
			const std::size_t z
			)
		{
			if( axis == 0 ) return (z*shape.ny+y)*(shape.nx+1)+x;
			if( axis == 1 ) return (z*(shape.ny+1)+y)*shape.nx+x;
			return (z*shape.ny+y)*shape.nx+x;
		}

		inline std::size_t OpenBoundaryFaceCount3D(
			const PeriodicMACShape& shape,
			const unsigned int side
			)
		{
			if( side < 2 ) return shape.ny*shape.nz;
			if( side < 4 ) return shape.nx*shape.nz;
			return shape.nx*shape.ny;
		}

		inline std::size_t OpenBoundaryFaceLinearIndex3D(
			const PeriodicMACShape& shape,
			const unsigned int side,
			const std::size_t first,
			const std::size_t second
			)
		{
			if( side < 2 ) return second*shape.ny+first;
			if( side < 4 ) return second*shape.nx+first;
			return second*shape.nx+first;
		}

		inline double OpenActiveSetComplementarityDiscrepancy3D(
			const PeriodicMACShape& shape,
			const OpenBoundaryConfig3D& boundary,
			const OpenMACProjection3DResult& projection )
		{
			double result=0.0;
			for( unsigned int side=0; side<6; ++side ) {
				const unsigned int axis=side/2; const bool positive=side%2;
				const std::size_t firstCount=side<2?shape.ny:shape.nx;
				const std::size_t secondCount=side<4?shape.nz:shape.ny;
				for( std::size_t second=0; second<secondCount; ++second ) for(
					std::size_t first=0; first<firstCount; ++first ) {
					const std::size_t index=OpenBoundaryFaceLinearIndex3D(shape,side,
						first,second);
					unsigned int kind=boundary.kind[side];
					if( side==4u && !boundary.bottomFuelMask.empty() &&
						boundary.bottomFuelMask[index] ) kind=FuelInletBoundary3D;
					if( kind!=PressureOpenBoundary3D ) continue;
					std::size_t x=0,y=0,z=0;
					if(axis==0){x=positive?shape.nx:0;y=first;z=second;}
					if(axis==1){x=first;y=positive?shape.ny:0;z=second;}
					if(axis==2){x=first;y=second;z=positive?shape.nz:0;}
					const double outward=(positive?1.0:-1.0)*projection.velocityMPerS.
						component[axis][OpenMACFaceIndex3D(shape,axis,x,y,z)];
					const double discrepancy=projection.inflow[side][index]?
						std::max(0.0,outward-boundary.velocityToleranceMPerS):
						std::max(0.0,-outward-boundary.velocityToleranceMPerS);
					result=std::max(result,discrepancy);
				}
			}
			return result;
		}

		inline bool OpenActiveSetCanonicalBefore3D(
			const PeriodicMACShape& shape,
			const OpenBoundaryConfig3D& boundary,
			const OpenMACProjection3DResult& first,
			const OpenMACProjection3DResult& second )
		{
			const double firstDiscrepancy=OpenActiveSetComplementarityDiscrepancy3D(
				shape,boundary,first);
			const double secondDiscrepancy=OpenActiveSetComplementarityDiscrepancy3D(
				shape,boundary,second);
			if(firstDiscrepancy!=secondDiscrepancy)
				return firstDiscrepancy<secondDiscrepancy;
			return FlattenOpenActiveSet3D(first.inflow)<
				FlattenOpenActiveSet3D(second.inflow);
		}

		inline std::size_t OpenActiveSetCanonicalHistoryIndex3D(
			const std::vector<double>& discrepancy,
			const std::vector<std::vector<unsigned char> >& bits,
			const std::size_t first )
		{
			std::size_t selected=first;
			for(std::size_t i=first+1u;i<bits.size();++i)if(
				discrepancy[i]<discrepancy[selected]||
				(discrepancy[i]==discrepancy[selected]&&bits[i]<bits[selected]))selected=i;
			return selected;
		}

		inline bool ObserveOpenActiveSetHistory3D(
			std::vector<std::array<std::vector<bool>,6> >& history,
			const std::array<std::vector<bool>,6>& classification,
			std::vector<std::array<std::vector<bool>,6> >& provedCycle )
		{
			const auto repeated=std::find(history.begin(),history.end(),classification);
			if(repeated==history.end()){history.push_back(classification);return false;}
			provedCycle.assign(repeated,history.end());
			return true;
		}

		struct OpenBoundaryFluxField3D
		{
			std::array<std::vector<OpenBoundaryFlux3D>,6> side;
		};

		inline bool ValidateOpenBoundaryConfig3D(
			const PeriodicMACShape& shape,
			const OpenBoundaryConfig3D& boundary,
			std::string* error
			);

		inline bool BuildOpenBoundaryFluxField3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& cellState,
			const std::vector<double>& temperatureK,
			const std::vector<double>& rhoDiffusivityKGPerMS,
			const std::vector<double>& conductivityWPerMK,
			const OpenBoundaryConfig3D& boundary,
			const OpenMACProjection3DResult& projection,
			const double ambientTemperatureK,
			const double injectedTemperatureK,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			OpenBoundaryFluxField3D& result,
			std::string* error = 0
			)
		{
			if( !ValidateOpenBoundaryConfig3D(shape,boundary,error) ) return false;
			const std::size_t count=shape.CellCount();
			if( cellState.size()!=count || temperatureK.size()!=count ||
				rhoDiffusivityKGPerMS.size()!=count || conductivityWPerMK.size()!=count ) return Fail(error,
				"fire solver 3-D boundary-flux field shape is invalid");
			for( unsigned int side=0; side<6; ++side ) {
				const unsigned int axis=side/2;
				const bool positive=side%2;
				const std::size_t firstCount=side<2?shape.ny:shape.nx;
				const std::size_t secondCount=side<4?shape.nz:shape.ny;
				const std::size_t faceCount=OpenBoundaryFaceCount3D(shape,side);
				if( projection.inflow[side].size()!=faceCount ||
					projection.velocityMPerS.component[axis].size()!=OpenMACFaceCount3D(shape,axis) ) {
					return Fail(error,"fire solver 3-D boundary-flux projection shape is invalid");
				}
				result.side[side].assign(faceCount,OpenBoundaryFlux3D());
				for( std::size_t second=0; second<secondCount; ++second ) for(
					std::size_t first=0; first<firstCount; ++first ) {
					std::size_t x=0,y=0,z=0;
					if(axis==0){x=positive?shape.nx:0;y=first;z=second;}
					if(axis==1){x=first;y=positive?shape.ny:0;z=second;}
					if(axis==2){x=first;y=second;z=positive?shape.nz:0;}
					const std::size_t cx=axis==0?(positive?shape.nx-1:0):x;
					const std::size_t cy=axis==1?(positive?shape.ny-1:0):y;
					const std::size_t cz=axis==2?(positive?shape.nz-1:0):z;
					const std::size_t cell=shape.Index(cx,cy,cz);
					const std::size_t boundaryIndex=OpenBoundaryFaceLinearIndex3D(
						shape,side,first,second);
					unsigned int kind=boundary.kind[side];
					if(side==4 && !boundary.bottomFuelMask.empty() &&
						boundary.bottomFuelMask[boundaryIndex]) kind=FuelInletBoundary3D;
					const std::size_t face=OpenMACFaceIndex3D(shape,axis,x,y,z);
					const double outwardVelocity=(positive?1.0:-1.0)*
						projection.velocityMPerS.component[axis][face];
					if( !BuildOpenBoundaryFlux3D(cellState[cell],temperatureK[cell],
						outwardVelocity,kind,projection.inflow[side][boundaryIndex],boundary,
						ambientTemperatureK,injectedTemperatureK,rhoDiffusivityKGPerMS[cell],
						conductivityWPerMK[cell],shape.cellWidthM,
						OpenBoundaryFuelMassFluxKGPerM2S(boundary,side,boundaryIndex),fuel,thermochemistry,
						result.side[side][boundaryIndex],error) ) return false;
				}
			}
			return true;
		}

		struct OpenPressureOperator3D
		{
			OpenMACField3D faceCoefficient;
			std::vector<std::vector<std::pair<std::size_t,double> > > rowExtra;
		};

		inline void ApplyOpenPressureOperator3D(
			const PeriodicMACShape& shape,
			const OpenPressureOperator3D& pressureOperator,
			const std::vector<double>& input,
			std::vector<double>& output,
			const unsigned int workerCount = 1u
			)
		{
			const double inverseWidth2 = 1.0/(shape.cellWidthM*shape.cellWidthM);
			output.assign(shape.CellCount(),0.0);
			const auto applySlice=[&](const std::size_t z) { for( std::size_t y=0;
				y<shape.ny; ++y ) for( std::size_t x=0; x<shape.nx; ++x ) {
				const std::size_t cell = shape.Index(x,y,z);
				const std::size_t xLeft = OpenMACFaceIndex3D(shape,0,x,y,z);
				const std::size_t xRight = OpenMACFaceIndex3D(shape,0,x+1,y,z);
				const std::size_t yLeft = OpenMACFaceIndex3D(shape,1,x,y,z);
				const std::size_t yRight = OpenMACFaceIndex3D(shape,1,x,y+1,z);
				const std::size_t zLeft = OpenMACFaceIndex3D(shape,2,x,y,z);
				const std::size_t zRight = OpenMACFaceIndex3D(shape,2,x,y,z+1);
				const double centre = input[cell];
				output[cell] = inverseWidth2*(
					pressureOperator.faceCoefficient.component[0][xLeft]*
						(centre-(x ? input[shape.Index(x-1,y,z)] : 0.0))+
					pressureOperator.faceCoefficient.component[0][xRight]*
						(centre-(x+1<shape.nx ? input[shape.Index(x+1,y,z)] : 0.0))+
					pressureOperator.faceCoefficient.component[1][yLeft]*
						(centre-(y ? input[shape.Index(x,y-1,z)] : 0.0))+
					pressureOperator.faceCoefficient.component[1][yRight]*
						(centre-(y+1<shape.ny ? input[shape.Index(x,y+1,z)] : 0.0))+
					pressureOperator.faceCoefficient.component[2][zLeft]*
						(centre-(z ? input[shape.Index(x,y,z-1)] : 0.0))+
					pressureOperator.faceCoefficient.component[2][zRight]*
						(centre-(z+1<shape.nz ? input[shape.Index(x,y,z+1)] : 0.0)));
				if( cell<pressureOperator.rowExtra.size() ) for( const std::pair<
					std::size_t,double>& entry : pressureOperator.rowExtra[cell] ) {
					output[cell]+=entry.second*input[entry.first];
				}
			}};
			ParallelFireSlices(shape.nz,workerCount,applySlice);
		}

		inline void BuildOpenPressureDiagonal3D(
			const PeriodicMACShape& shape,
			const OpenPressureOperator3D& pressureOperator,
			std::vector<double>& diagonal,
			const unsigned int workerCount
			)
		{
			const double inverseWidth2 = 1.0/(shape.cellWidthM*shape.cellWidthM);
			diagonal.assign(shape.CellCount(),0.0);
			const auto buildDiagonalSlice=[&](const std::size_t z) { for( std::size_t y=0;
				y<shape.ny; ++y ) for( std::size_t x=0; x<shape.nx; ++x ) {
				const std::size_t cell = shape.Index(x,y,z);
				diagonal[cell] += pressureOperator.faceCoefficient.component[0][
					OpenMACFaceIndex3D(shape,0,x,y,z)];
				diagonal[cell] += pressureOperator.faceCoefficient.component[0][
					OpenMACFaceIndex3D(shape,0,x+1,y,z)];
				diagonal[cell] += pressureOperator.faceCoefficient.component[1][
					OpenMACFaceIndex3D(shape,1,x,y,z)];
				diagonal[cell] += pressureOperator.faceCoefficient.component[1][
					OpenMACFaceIndex3D(shape,1,x,y+1,z)];
				diagonal[cell] += pressureOperator.faceCoefficient.component[2][
					OpenMACFaceIndex3D(shape,2,x,y,z)];
				diagonal[cell] += pressureOperator.faceCoefficient.component[2][
					OpenMACFaceIndex3D(shape,2,x,y,z+1)];
				diagonal[cell] *= inverseWidth2;
				if( cell<pressureOperator.rowExtra.size() ) for( const std::pair<
					std::size_t,double>& entry : pressureOperator.rowExtra[cell] ) {
					if( entry.first==cell ) diagonal[cell]+=entry.second;
				}
			}};
			ParallelFireSlices(shape.nz,workerCount,buildDiagonalSlice);
		}

		inline void SmoothOpenPressureOperator3D(
			const PeriodicMACShape& shape,
			const OpenPressureOperator3D& pressureOperator,
			const std::vector<double>& rightHandSide,
			const std::size_t iterations,
			std::vector<double>& solution,
			const unsigned int workerCount = 1u,
			const std::vector<double>* preparedDiagonal = 0
			)
		{
			FireProfileIncrement(FireProfile().smoothSweeps,iterations);
			std::vector<double> computedDiagonal;
			if(!preparedDiagonal)BuildOpenPressureDiagonal3D(shape,pressureOperator,
				computedDiagonal,workerCount);
			const std::vector<double>& diagonal=preparedDiagonal?*preparedDiagonal:computedDiagonal;
			std::vector<double> applied,next(shape.CellCount(),0.0);
			for( std::size_t iteration=0; iteration<iterations; ++iteration ) {
				ApplyOpenPressureOperator3D(shape,pressureOperator,solution,applied,workerCount);
				const auto updateSlice=[&](const std::size_t z) { for( std::size_t y=0;
					y<shape.ny; ++y ) for( std::size_t x=0; x<shape.nx; ++x ) {
					const std::size_t cell = shape.Index(x,y,z);
					next[cell] = solution[cell]+(2.0/3.0)*
						(rightHandSide[cell]-applied[cell])/diagonal[cell];
				}};
				ParallelFireSlices(shape.nz,workerCount,updateSlice);
				solution.swap(next);
			}
		}

		inline bool CoarsenOpenPressureOperator3D(
			const PeriodicMACShape& fineShape,
			const OpenPressureOperator3D& fineOperator,
			PeriodicMACShape& coarseShape,
			OpenPressureOperator3D& coarseOperator
			)
		{
			if( fineShape.nx < 4 || fineShape.ny < 4 || fineShape.nz < 4 ) return false;
			coarseShape.nx = (fineShape.nx+1)/2;
			coarseShape.ny = (fineShape.ny+1)/2;
			coarseShape.nz = (fineShape.nz+1)/2;
			coarseShape.cellWidthM = 2.0*fineShape.cellWidthM;
			coarseOperator.rowExtra.assign(coarseShape.CellCount(),
				std::vector<std::pair<std::size_t,double> >());
			for( unsigned int axis=0; axis<3; ++axis ) coarseOperator.faceCoefficient.
				component[axis].assign(OpenMACFaceCount3D(coarseShape,axis),0.0);
			for( unsigned int axis=0; axis<3; ++axis ) {
				const std::size_t normalCount = axis == 0 ? coarseShape.nx+1 :
					(axis == 1 ? coarseShape.ny+1 : coarseShape.nz+1);
				const std::size_t firstCount = axis == 0 ? coarseShape.ny : coarseShape.nx;
				const std::size_t secondCount = axis == 2 ? coarseShape.ny : coarseShape.nz;
				for( std::size_t second=0; second<secondCount; ++second ) for(
					std::size_t first=0; first<firstCount; ++first ) for(
					std::size_t normal=0; normal<normalCount; ++normal ) {
					double sum = 0.0;
					double sampleCount=0.0;
					for( std::size_t b=0; b<2; ++b ) for( std::size_t a=0; a<2; ++a ) {
						std::size_t x=0,y=0,z=0;
						if( axis == 0 ) { x=std::min(2*normal,fineShape.nx);
							y=2*first+a; z=2*second+b; }
						if( axis == 1 ) { x=2*first+a;
							y=std::min(2*normal,fineShape.ny); z=2*second+b; }
						if( axis == 2 ) { x=2*first+a; y=2*second+b;
							z=std::min(2*normal,fineShape.nz); }
						if( x>(axis==0?fineShape.nx:fineShape.nx-1) ||
							y>(axis==1?fineShape.ny:fineShape.ny-1) ||
							z>(axis==2?fineShape.nz:fineShape.nz-1) ) continue;
						sum += fineOperator.faceCoefficient.component[axis][
							OpenMACFaceIndex3D(fineShape,axis,x,y,z)];
						sampleCount+=1.0;
					}
					std::size_t x=0,y=0,z=0;
					if( axis == 0 ) { x=normal; y=first; z=second; }
					if( axis == 1 ) { x=first; y=normal; z=second; }
					if( axis == 2 ) { x=first; y=second; z=normal; }
					coarseOperator.faceCoefficient.component[axis][
						OpenMACFaceIndex3D(coarseShape,axis,x,y,z)] = sum/sampleCount;
				}
			}
			return true;
		}

		inline void RestrictOpenResidual3D(
			const PeriodicMACShape& fineShape,
			const PeriodicMACShape& coarseShape,
			const std::vector<double>& fine,
			std::vector<double>& coarse,
			const unsigned int workerCount = 1u
			)
		{
			coarse.assign(coarseShape.CellCount(),0.0);
			const auto restrictSlice=[&](const std::size_t z){for( std::size_t y=0;
				y<coarseShape.ny; ++y ) for( std::size_t x=0; x<coarseShape.nx; ++x ) {
				double sum = 0.0, sampleCount=0.0;
				for( std::size_t dz=0; dz<2; ++dz ) for( std::size_t dy=0; dy<2; ++dy )
					for( std::size_t dx=0; dx<2; ++dx ) if( 2*x+dx<fineShape.nx &&
						2*y+dy<fineShape.ny && 2*z+dz<fineShape.nz ) {
						sum += fine[fineShape.Index(2*x+dx,2*y+dy,2*z+dz)];
						sampleCount+=1.0;
					}
				coarse[coarseShape.Index(x,y,z)] = sum/sampleCount;
			}};
			ParallelFireSlices(coarseShape.nz,workerCount,restrictSlice);
		}

		inline void ProlongOpenCorrection3D(
			const PeriodicMACShape& fineShape,
			const PeriodicMACShape& coarseShape,
			const std::vector<double>& coarse,
			std::vector<double>& fine,
			const unsigned int workerCount = 1u
			)
		{
			const auto prolongSlice=[&](const std::size_t z){for( std::size_t y=0;
				y<fineShape.ny; ++y ) for( std::size_t x=0; x<fineShape.nx; ++x ) {
				const std::size_t cx=x/2, cy=y/2, cz=z/2;
				const double tx=(x%2)*0.5,ty=(y%2)*0.5,tz=(z%2)*0.5;
				double value = 0.0;
				for( std::size_t dz=0; dz<2; ++dz ) for( std::size_t dy=0; dy<2; ++dy )
					for( std::size_t dx=0; dx<2; ++dx ) {
						const std::size_t qx=std::min(cx+dx,coarseShape.nx-1);
						const std::size_t qy=std::min(cy+dy,coarseShape.ny-1);
						const std::size_t qz=std::min(cz+dz,coarseShape.nz-1);
						value += (dx?tx:1.0-tx)*(dy?ty:1.0-ty)*(dz?tz:1.0-tz)*
							coarse[coarseShape.Index(qx,qy,qz)];
					}
				fine[fineShape.Index(x,y,z)] += value;
			}};
			ParallelFireSlices(fineShape.nz,workerCount,prolongSlice);
		}

		inline void OpenPressureMultigridVCycle3D(
			const PeriodicMACShape& shape,
			const OpenPressureOperator3D& pressureOperator,
			const std::vector<double>& rightHandSide,
			std::vector<double>& solution,
			const unsigned int workerCount = 1u
			)
		{
			FireProfileIncrement(FireProfile().vcycles);
			PeriodicMACShape coarseShape;
			OpenPressureOperator3D coarseOperator;
			FireProfileIncrement(FireProfile().coarsenCalls);
			bool coarsened=false;
			{FireProfileScopedNs profileTimer(FireProfile().nsMGCoarsen);
				coarsened=CoarsenOpenPressureOperator3D(shape,pressureOperator,coarseShape,
					coarseOperator);}
			if(!coarsened) {
				SmoothOpenPressureOperator3D(shape,pressureOperator,rightHandSide,60,solution,workerCount);
				return;
			}
			SmoothOpenPressureOperator3D(shape,pressureOperator,rightHandSide,4,solution,workerCount);
			std::vector<double> applied,residual(shape.CellCount(),0.0),coarseRight;
			ApplyOpenPressureOperator3D(shape,pressureOperator,solution,applied,workerCount);
			for( std::size_t cell=0; cell<shape.CellCount(); ++cell ) residual[cell] =
				rightHandSide[cell]-applied[cell];
			RestrictOpenResidual3D(shape,coarseShape,residual,coarseRight,workerCount);
			std::vector<double> coarseCorrection(coarseShape.CellCount(),0.0);
			OpenPressureMultigridVCycle3D(coarseShape,coarseOperator,coarseRight,
				coarseCorrection,1u);
			ProlongOpenCorrection3D(shape,coarseShape,coarseCorrection,solution,workerCount);
			SmoothOpenPressureOperator3D(shape,pressureOperator,rightHandSide,4,solution,workerCount);
		}

		inline void OpenPressureMultigridHierarchyVCycle3D(
			const std::vector<PeriodicMACShape>& shape,
			const std::vector<OpenPressureOperator3D>& pressureOperator,
			const std::vector<std::vector<double> >& diagonal,
			const std::size_t level,
			const std::vector<double>& rightHandSide,
			std::vector<double>& solution,
			const unsigned int workerCount = 1u
			)
		{
			if(level==0u)FireProfileIncrement(FireProfile().vcycles);
			if(level+1u==shape.size()) {
				SmoothOpenPressureOperator3D(shape[level],pressureOperator[level],
					rightHandSide,60,solution,workerCount,&diagonal[level]);
				return;
			}
			SmoothOpenPressureOperator3D(shape[level],pressureOperator[level],
				rightHandSide,4,solution,workerCount,&diagonal[level]);
			std::vector<double> applied,residual(shape[level].CellCount(),0.0),coarseRight;
			ApplyOpenPressureOperator3D(shape[level],pressureOperator[level],solution,applied,workerCount);
			for(std::size_t cell=0;cell<shape[level].CellCount();++cell) residual[cell]=
				rightHandSide[cell]-applied[cell];
			RestrictOpenResidual3D(shape[level],shape[level+1u],residual,coarseRight,workerCount);
			std::vector<double> coarseCorrection(shape[level+1u].CellCount(),0.0);
			OpenPressureMultigridHierarchyVCycle3D(shape,pressureOperator,diagonal,level+1u,
				coarseRight,coarseCorrection,1u);
			ProlongOpenCorrection3D(shape[level],shape[level+1u],coarseCorrection,solution,workerCount);
			SmoothOpenPressureOperator3D(shape[level],pressureOperator[level],
				rightHandSide,4,solution,workerCount,&diagonal[level]);
		}

		inline double OpenVectorDot3D(
			const std::vector<double>& first,
			const std::vector<double>& second
			)
		{
			double result=0.0;
			for( std::size_t i=0; i<first.size(); ++i ) result+=first[i]*second[i];
			return result;
		}

		inline bool SolveOpenPressureMultigrid3D(
			const PeriodicMACShape& shape,
			const OpenPressureOperator3D& pressureOperator,
			const std::vector<double>& rightHandSide,
			const double tolerance,
			std::vector<double>& solution,
			std::vector<double>& residualHistory,
			const unsigned int workerCount = 1u
			)
		{
			FireProfileIncrement(FireProfile().mgSolves);
			const std::size_t count=shape.CellCount();
			std::vector<PeriodicMACShape> shapeHierarchy(1u,shape);
			std::vector<OpenPressureOperator3D> operatorHierarchy(1u,pressureOperator);
			for(;;){PeriodicMACShape coarseShape;OpenPressureOperator3D coarseOperator;
				FireProfileIncrement(FireProfile().coarsenCalls);
				bool coarsened=false;{FireProfileScopedNs profileTimer(FireProfile().nsMGCoarsen);
					coarsened=CoarsenOpenPressureOperator3D(shapeHierarchy.back(),
						operatorHierarchy.back(),coarseShape,coarseOperator);}
				if(!coarsened)break;shapeHierarchy.push_back(coarseShape);
				operatorHierarchy.push_back(std::move(coarseOperator));}
			std::vector<std::vector<double> > diagonalHierarchy(operatorHierarchy.size());
			for(std::size_t level=0;level<operatorHierarchy.size();++level)
				BuildOpenPressureDiagonal3D(shapeHierarchy[level],operatorHierarchy[level],
					diagonalHierarchy[level],level?1u:workerCount);
			solution.assign(count,0.0);
			std::vector<double> applied;
			for( std::size_t cycle=0; cycle<128; ++cycle ) {
				FireProfileIncrement(FireProfile().mgOuterIters);
				OpenPressureMultigridHierarchyVCycle3D(shapeHierarchy,operatorHierarchy,
					diagonalHierarchy,0u,rightHandSide,solution,workerCount);
				ApplyOpenPressureOperator3D(shape,pressureOperator,solution,applied,workerCount);
				double maximum=0.0;
				for( std::size_t i=0; i<count; ++i ) maximum=std::max(maximum,
					std::fabs(rightHandSide[i]-applied[i]));
				residualHistory.push_back(maximum);
				if(maximum<=tolerance) return true;
			}
			solution.assign(count,0.0);
			std::vector<double> r=rightHandSide,rHat=r,p(count,0.0),v(count,0.0),
				s(count,0.0),t(count,0.0),pHat,sHat;
			double rhoPrevious=1.0,alpha=1.0,omega=1.0;
			for( std::size_t iteration=0; iteration<256; ++iteration ) {
				double maximum=0.0;
				for( const double value : r ) maximum=std::max(maximum,std::fabs(value));
				residualHistory.push_back(maximum);
				if( maximum<=tolerance ) return true;
				const double rho=OpenVectorDot3D(rHat,r);
				if( !std::isfinite(rho) || rho==0.0 ) return false;
				const double beta=(rho/rhoPrevious)*(alpha/omega);
				if( !std::isfinite(beta) ) return false;
				for( std::size_t i=0; i<count; ++i ) p[i]=r[i]+beta*(p[i]-omega*v[i]);
				pHat.assign(count,0.0);
				OpenPressureMultigridHierarchyVCycle3D(shapeHierarchy,operatorHierarchy,
					diagonalHierarchy,0u,p,pHat,workerCount);
				ApplyOpenPressureOperator3D(shape,pressureOperator,pHat,v,workerCount);
				const double denominator=OpenVectorDot3D(rHat,v);
				if( !std::isfinite(denominator) || denominator==0.0 ) return false;
				alpha=rho/denominator;
				for( std::size_t i=0; i<count; ++i ) s[i]=r[i]-alpha*v[i];
				double sMaximum=0.0;
				for( const double value : s ) sMaximum=std::max(sMaximum,std::fabs(value));
				if( sMaximum<=tolerance ) {
					for( std::size_t i=0; i<count; ++i ) solution[i]+=alpha*pHat[i];
					residualHistory.push_back(sMaximum);
					return true;
				}
				sHat.assign(count,0.0);
				OpenPressureMultigridHierarchyVCycle3D(shapeHierarchy,operatorHierarchy,
					diagonalHierarchy,0u,s,sHat,workerCount);
				ApplyOpenPressureOperator3D(shape,pressureOperator,sHat,t,workerCount);
				const double tSquared=OpenVectorDot3D(t,t);
				if( !std::isfinite(tSquared) || tSquared==0.0 ) return false;
				omega=OpenVectorDot3D(t,s)/tSquared;
				if( !std::isfinite(omega) || omega==0.0 ) return false;
				for( std::size_t i=0; i<count; ++i ) {
					solution[i]+=alpha*pHat[i]+omega*sHat[i];
					r[i]=s[i]-omega*t[i];
					if( !std::isfinite(solution[i]) || !std::isfinite(r[i]) ) return false;
				}
				rhoPrevious=rho;
			}
			return false;
		}

		inline bool ValidateOpenBoundaryConfig3D(
			const PeriodicMACShape& shape,
			const OpenBoundaryConfig3D& boundary,
			std::string* error = 0
			)
		{
			const std::size_t maximum=std::numeric_limits<std::size_t>::max();
			if( shape.nx<2 || shape.ny<2 || shape.nz<2 ||
				shape.nx>maximum/shape.ny || shape.nx*shape.ny>maximum/shape.nz ||
				shape.nx+1>maximum/shape.ny || (shape.nx+1)*shape.ny>maximum/shape.nz ||
				shape.ny+1>maximum/shape.nx || shape.nx*(shape.ny+1)>maximum/shape.nz ||
				shape.nz+1>maximum/shape.nx || shape.nx*(shape.nz+1)>maximum/shape.ny ||
				!std::isfinite(shape.cellWidthM) || shape.cellWidthM<=0.0 ||
				!std::isfinite(boundary.ambientDensityKGPerM3) ||
				boundary.ambientDensityKGPerM3 <= 0.0 ||
				!std::isfinite(boundary.injectedGasDensityKGPerM3) ||
				boundary.injectedGasDensityKGPerM3 <= 0.0 ||
				!std::isfinite(boundary.fuelMassFluxKGPerM2S) ||
				boundary.fuelMassFluxKGPerM2S < 0.0 ||
				!std::isfinite(boundary.velocityToleranceMPerS) ||
				boundary.velocityToleranceMPerS < 0.0 ||
				!std::isfinite(boundary.pressureTolerancePa) ||
				boundary.pressureTolerancePa <= 0.0 ) {
				return Fail(error,"fire solver 3-D open-boundary constants are invalid");
			}
			double ambientMass=0.0,injectedMass=0.0,ambientGasMass=0.0,injectedGasMass=0.0;
			for( std::size_t component=0; component<MethaneConservativeDimension; ++component ) {
				if( !std::isfinite(boundary.ambientState[component]) ||
					!std::isfinite(boundary.injectedState[component]) ) return Fail(error,
					"fire solver 3-D open-boundary state is non-finite");
			}
			for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
				if( boundary.ambientState[1+species]<0.0 || boundary.injectedState[1+species]<0.0 ) {
					return Fail(error,"fire solver 3-D open-boundary constituent is negative");
				}
				ambientMass+=boundary.ambientState[1+species];
				injectedMass+=boundary.injectedState[1+species];
				if( species<MethaneCarbon ) {
					ambientGasMass+=boundary.ambientState[1+species];
					injectedGasMass+=boundary.injectedState[1+species];
				}
			}
			if( !std::isfinite(ambientMass) || !std::isfinite(injectedMass) ||
				ambientMass<=0.0 || injectedMass<=0.0 || boundary.ambientState[0]<0.0 ||
				boundary.injectedState[0]<0.0 || boundary.ambientState[0]>ambientMass ||
				boundary.injectedState[0]>injectedMass ||
				ambientGasMass!=boundary.ambientDensityKGPerM3 ||
				injectedGasMass!=boundary.injectedGasDensityKGPerM3 ) return Fail(error,
				"fire solver 3-D open-boundary state has invalid mass");
			for( unsigned int side=0; side<6; ++side ) {
				if( boundary.kind[side]!=PressureOpenBoundary3D &&
					boundary.kind[side]!=AdiabaticWallBoundary3D &&
					boundary.kind[side]!=FuelInletBoundary3D ) return Fail(error,
					"fire solver 3-D boundary kind is invalid");
				if( boundary.kind[side]==FuelInletBoundary3D ) return Fail(error,
					"fire solver 3-D fuel inlet must be selected by the bottom bed mask");
				const std::size_t expected = OpenBoundaryFaceCount3D(shape,side);
				if( !boundary.priorInflow[side].empty() &&
					boundary.priorInflow[side].size() != expected ) {
					return Fail(error,"fire solver 3-D open-boundary class shape is invalid");
				}
			}
			if( !boundary.bottomFuelMask.empty() &&
				boundary.bottomFuelMask.size() != shape.nx*shape.ny ) {
				return Fail(error,"fire solver 3-D fuel-mask shape is invalid");
			}
			if( !boundary.bottomFuelMassFluxKGPerM2S.empty() &&
				(boundary.bottomFuelMassFluxKGPerM2S.size()!=shape.nx*shape.ny ||
				 boundary.bottomFuelMask.size()!=shape.nx*shape.ny) ) return Fail(error,
				"fire solver 3-D fuel-flux pattern shape is invalid");
			for( std::size_t face=0;face<boundary.bottomFuelMassFluxKGPerM2S.size();++face ) {
				if( !std::isfinite(boundary.bottomFuelMassFluxKGPerM2S[face]) ||
					boundary.bottomFuelMassFluxKGPerM2S[face]<0.0 ||
					(!boundary.bottomFuelMask[face] && boundary.bottomFuelMassFluxKGPerM2S[face]!=0.0) )
					return Fail(error,"fire solver 3-D fuel-flux pattern is invalid");
			}
			if( !boundary.bottomFuelMask.empty() &&
				boundary.kind[4]!=AdiabaticWallBoundary3D ) return Fail(error,
				"fire solver 3-D fuel mask requires an otherwise adiabatic bottom bed");
			return true;
		}

		inline double OpenMACDivergence3D(
			const PeriodicMACShape& shape,
			const OpenMACField3D& velocity,
			const std::size_t x,
			const std::size_t y,
			const std::size_t z
			)
		{
			return (
				velocity.component[0][OpenMACFaceIndex3D(shape,0,x+1,y,z)]-
				velocity.component[0][OpenMACFaceIndex3D(shape,0,x,y,z)]+
				velocity.component[1][OpenMACFaceIndex3D(shape,1,x,y+1,z)]-
				velocity.component[1][OpenMACFaceIndex3D(shape,1,x,y,z)]+
				velocity.component[2][OpenMACFaceIndex3D(shape,2,x,y,z+1)]-
				velocity.component[2][OpenMACFaceIndex3D(shape,2,x,y,z)])/
				shape.cellWidthM;
		}

		inline double OpenBoundaryTangentialCellVelocity3D(
			const PeriodicMACShape& shape,
			const OpenMACField3D& velocity,
			const unsigned int component,
			const std::size_t x,
			const std::size_t y,
			const std::size_t z
			)
		{
			const std::size_t coordinate=component==0?x:(component==1?y:z);
			std::size_t first=coordinate,second=coordinate+1;
			std::size_t firstX=x,firstY=y,firstZ=z;
			std::size_t secondX=x,secondY=y,secondZ=z;
			if(component==0){firstX=first;secondX=second;}
			if(component==1){firstY=first;secondY=second;}
			if(component==2){firstZ=first;secondZ=second;}
			return 0.5*(velocity.component[component][OpenMACFaceIndex3D(shape,
				component,firstX,firstY,firstZ)]+velocity.component[component][
				OpenMACFaceIndex3D(shape,component,secondX,secondY,secondZ)]);
		}

		inline void AppendOpenBoundaryTangentialDerivative3D(
			const PeriodicMACShape& shape,
			const OpenMACField3D& faceDensityKGPerM3,
			const unsigned int component,
			const std::size_t x,
			const std::size_t y,
			const std::size_t z,
			const double deltaTimeS,
			std::vector<std::pair<std::size_t,double> >& derivative
			)
		{
			const std::size_t coordinate=component==0?x:(component==1?y:z);
			std::size_t first=coordinate,second=coordinate+1;
			const std::size_t normal[2]={first,second};
			for( unsigned int sample=0; sample<2; ++sample ) {
				std::size_t fx=x,fy=y,fz=z;
				if(component==0)fx=normal[sample];
				if(component==1)fy=normal[sample];
				if(component==2)fz=normal[sample];
				const std::size_t face=OpenMACFaceIndex3D(shape,component,fx,fy,fz);
				const double scale=0.5*deltaTimeS/(faceDensityKGPerM3.component[
					component][face]*shape.cellWidthM);
				std::size_t lowerX=x,lowerY=y,lowerZ=z;
				std::size_t upperX=x,upperY=y,upperZ=z;
				if(component==0){lowerX=normal[sample]-1;upperX=normal[sample];}
				if(component==1){lowerY=normal[sample]-1;upperY=normal[sample];}
				if(component==2){lowerZ=normal[sample]-1;upperZ=normal[sample];}
				derivative.push_back(std::make_pair(shape.Index(lowerX,lowerY,lowerZ),scale));
				derivative.push_back(std::make_pair(shape.Index(upperX,upperY,upperZ),-scale));
			}
		}

		inline double OpenBoundaryTangentialNewtonRowFactor3D(
			const double ambientDensityKGPerM3,
			const double normalFaceDensityKGPerM3,
			const double tangentialVelocityMPerS,
			const double cellWidthM,
			const double normalImplicitDenominator
			)
		{
			return 2.0*ambientDensityKGPerM3*tangentialVelocityMPerS/
				(normalFaceDensityKGPerM3*cellWidthM*cellWidthM*
				normalImplicitDenominator);
		}

		inline void PopulateOpenBoundaryTangentialVelocity3D(
			const PeriodicMACShape& shape,
			const OpenBoundaryConfig3D& boundary,
			OpenMACProjection3DResult& result
			)
		{
			for( unsigned int side=0; side<6; ++side ) {
				const unsigned int axis=side/2;
				const bool positive=side%2;
				const unsigned int tangent[2]={(axis+1)%3,(axis+2)%3};
				const std::size_t firstCount=side<2?shape.ny:shape.nx;
				const std::size_t secondCount=side<4?shape.nz:shape.ny;
				const std::size_t count=OpenBoundaryFaceCount3D(shape,side);
				result.boundaryTangentialVelocityMPerS[side][0].assign(count,0.0);
				result.boundaryTangentialVelocityMPerS[side][1].assign(count,0.0);
				for( std::size_t second=0; second<secondCount; ++second ) for(
					std::size_t first=0; first<firstCount; ++first ) {
					std::size_t x=0,y=0,z=0;
					if(axis==0){x=positive?shape.nx-1:0;y=first;z=second;}
					if(axis==1){x=first;y=positive?shape.ny-1:0;z=second;}
					if(axis==2){x=first;y=second;z=positive?shape.nz-1:0;}
					const std::size_t index=OpenBoundaryFaceLinearIndex3D(shape,side,first,second);
					unsigned int kind=boundary.kind[side];
					if( side==4 && !boundary.bottomFuelMask.empty() &&
						boundary.bottomFuelMask[index] ) kind=FuelInletBoundary3D;
					if( kind==PressureOpenBoundary3D ) for( unsigned int local=0; local<2;
						++local ) result.boundaryTangentialVelocityMPerS[side][local][index]=
						OpenBoundaryTangentialCellVelocity3D(shape,result.velocityMPerS,
							tangent[local],x,y,z);
				}
			}
		}

		inline bool ReferenceProjectPressureOpenMACVelocity3DEliminated(
			const PeriodicMACShape& shape,
			const std::vector<double>& gasDensityKGPerM3,
			const OpenMACField3D& unprojectedMomentumKGPerM2S,
			const std::vector<double>& divergenceTargetPerS,
			const OpenBoundaryConfig3D& boundary,
			const double deltaTimeS,
			const double absoluteTolerancePerS,
			OpenMACProjection3DResult& result,
			std::string* error = 0
			)
		{
			if( shape.nx < 2 || shape.ny < 2 || shape.nz < 2 ||
				shape.nx > std::numeric_limits<std::size_t>::max()/shape.ny ||
				shape.nx*shape.ny > std::numeric_limits<std::size_t>::max()/shape.nz ||
				!std::isfinite(shape.cellWidthM) || shape.cellWidthM <= 0.0 ||
				!std::isfinite(deltaTimeS) || deltaTimeS <= 0.0 ||
				!std::isfinite(absoluteTolerancePerS) || absoluteTolerancePerS <= 0.0 ||
				!std::isfinite(1.0/shape.cellWidthM) ||
				!std::isfinite(1.0/(shape.cellWidthM*shape.cellWidthM)) ||
				!ValidateOpenBoundaryConfig3D(shape,boundary,error) ) {
				return Fail(error,"fire solver 3-D pressure-open projection input is malformed");
			}
			const std::size_t cellCount = shape.CellCount();
			if( gasDensityKGPerM3.size() != cellCount ||
				divergenceTargetPerS.size() != cellCount ) {
				return Fail(error,"fire solver 3-D pressure-open cell shape is invalid");
			}
			result = OpenMACProjection3DResult();
			for( unsigned int axis=0; axis<3; ++axis ) {
				const std::size_t faceCount = OpenMACFaceCount3D(shape,axis);
				if( unprojectedMomentumKGPerM2S.component[axis].size() != faceCount ) {
					return Fail(error,"fire solver 3-D pressure-open momentum shape is invalid");
				}
				result.faceDensityKGPerM3.component[axis].assign(faceCount,0.0);
				result.velocityMPerS.component[axis].assign(faceCount,0.0);
				result.momentumKGPerM2S.component[axis].assign(faceCount,0.0);
			}
			for( std::size_t cell=0; cell<cellCount; ++cell ) {
				if( !std::isfinite(gasDensityKGPerM3[cell]) || gasDensityKGPerM3[cell] <= 0.0 ||
					!std::isfinite(divergenceTargetPerS[cell]) ) {
					return Fail(error,"fire solver 3-D pressure-open cell is invalid");
				}
			}
			for( unsigned int axis=0; axis<3; ++axis ) for( const double momentum :
				unprojectedMomentumKGPerM2S.component[axis] ) if( !std::isfinite(momentum) ) {
				return Fail(error,"fire solver 3-D pressure-open momentum is non-finite");
			}

			for( unsigned int side=0; side<6; ++side ) {
				const std::size_t count = OpenBoundaryFaceCount3D(shape,side);
				result.inflow[side] = boundary.priorInflow[side].empty() ?
					std::vector<bool>(count,false) : boundary.priorInflow[side];
				result.boundaryDynamicPressurePa[side].assign(count,0.0);
			}

			// One arithmetic density owns momentum storage and every pressure coefficient.
			for( std::size_t z=0; z<shape.nz; ++z ) for( std::size_t y=0;
				y<shape.ny; ++y ) for( std::size_t x=0; x<=shape.nx; ++x ) {
				const std::size_t face = OpenMACFaceIndex3D(shape,0,x,y,z);
				const double first = x ? gasDensityKGPerM3[shape.Index(x-1,y,z)] :
					boundary.ambientDensityKGPerM3;
				const double second = x<shape.nx ? gasDensityKGPerM3[shape.Index(x,y,z)] :
					boundary.ambientDensityKGPerM3;
				result.faceDensityKGPerM3.component[0][face] = 0.5*first+0.5*second;
			}
			for( std::size_t z=0; z<shape.nz; ++z ) for( std::size_t y=0;
				y<=shape.ny; ++y ) for( std::size_t x=0; x<shape.nx; ++x ) {
				const std::size_t face = OpenMACFaceIndex3D(shape,1,x,y,z);
				const double first = y ? gasDensityKGPerM3[shape.Index(x,y-1,z)] :
					boundary.ambientDensityKGPerM3;
				const double second = y<shape.ny ? gasDensityKGPerM3[shape.Index(x,y,z)] :
					boundary.ambientDensityKGPerM3;
				result.faceDensityKGPerM3.component[1][face] = 0.5*first+0.5*second;
			}
			for( std::size_t z=0; z<=shape.nz; ++z ) for( std::size_t y=0;
				y<shape.ny; ++y ) for( std::size_t x=0; x<shape.nx; ++x ) {
				const std::size_t face = OpenMACFaceIndex3D(shape,2,x,y,z);
				const bool fuel = z==0 && !boundary.bottomFuelMask.empty() &&
					boundary.bottomFuelMask[y*shape.nx+x];
				const double outside = fuel ? boundary.injectedGasDensityKGPerM3 :
					boundary.ambientDensityKGPerM3;
				const double first = z ? gasDensityKGPerM3[shape.Index(x,y,z-1)] : outside;
				const double second = z<shape.nz ? gasDensityKGPerM3[shape.Index(x,y,z)] : outside;
				result.faceDensityKGPerM3.component[2][face] = 0.5*first+0.5*second;
			}
			for( unsigned int axis=0; axis<3; ++axis ) for( const double density :
				result.faceDensityKGPerM3.component[axis] ) if( !std::isfinite(density) || density <= 0.0 ) {
				return Fail(error,"fire solver 3-D pressure-open face density overflowed");
			}

			std::vector<double> pressure(cellCount,0.0);
			std::vector<std::vector<unsigned char> > activeHistory;
			for( std::size_t activeIteration=0; activeIteration<16; ++activeIteration ) {
				std::vector<unsigned char> activeState;
				for( unsigned int side=0; side<6; ++side ) for( const bool inflow :
					result.inflow[side] ) activeState.push_back(inflow?1u:0u);
				if( std::find(activeHistory.begin(),activeHistory.end(),activeState) !=
					activeHistory.end() ) return Fail(error,
					"fire solver 3-D pressure-open active set cycled");
				activeHistory.push_back(activeState);

				bool nonlinearConverged = false;
				bool evaluatingLineSearch = false;
				std::vector<double> lineSearchBasePressure,lineSearchCorrection;
				double lineSearchBaseResidual=0.0,lineSearchDamping=1.0;
				for( std::size_t nonlinear=0; nonlinear<40; ++nonlinear ) {
					OpenPressureOperator3D pressureOperator;
					pressureOperator.rowExtra.assign(cellCount,
						std::vector<std::pair<std::size_t,double> >());
					for( unsigned int axis=0; axis<3; ++axis ) pressureOperator.faceCoefficient.
						component[axis].assign(OpenMACFaceCount3D(shape,axis),0.0);
					for( unsigned int axis=0; axis<3; ++axis ) {
						result.velocityMPerS.component[axis].assign(OpenMACFaceCount3D(shape,axis),0.0);
					}
					// Interior faces use the same adjoint D/G pair as the multigrid operator.
					for( std::size_t z=0; z<shape.nz; ++z ) for( std::size_t y=0;
						y<shape.ny; ++y ) for( std::size_t x=1; x<shape.nx; ++x ) {
						const std::size_t face=OpenMACFaceIndex3D(shape,0,x,y,z);
						const double inverseDensity=1.0/result.faceDensityKGPerM3.component[0][face];
						result.velocityMPerS.component[0][face]=
							unprojectedMomentumKGPerM2S.component[0][face]*inverseDensity-
							deltaTimeS*inverseDensity*(pressure[shape.Index(x,y,z)]-
							pressure[shape.Index(x-1,y,z)])/shape.cellWidthM;
						pressureOperator.faceCoefficient.component[0][face]=inverseDensity;
						if( !std::isfinite(result.velocityMPerS.component[0][face]) ) return Fail(
							error,"fire solver 3-D pressure-open x velocity overflowed");
					}
					for( std::size_t z=0; z<shape.nz; ++z ) for( std::size_t y=1;
						y<shape.ny; ++y ) for( std::size_t x=0; x<shape.nx; ++x ) {
						const std::size_t face=OpenMACFaceIndex3D(shape,1,x,y,z);
						const double inverseDensity=1.0/result.faceDensityKGPerM3.component[1][face];
						result.velocityMPerS.component[1][face]=
							unprojectedMomentumKGPerM2S.component[1][face]*inverseDensity-
							deltaTimeS*inverseDensity*(pressure[shape.Index(x,y,z)]-
							pressure[shape.Index(x,y-1,z)])/shape.cellWidthM;
						pressureOperator.faceCoefficient.component[1][face]=inverseDensity;
						if( !std::isfinite(result.velocityMPerS.component[1][face]) ) return Fail(
							error,"fire solver 3-D pressure-open y velocity overflowed");
					}
					for( std::size_t z=1; z<shape.nz; ++z ) for( std::size_t y=0;
						y<shape.ny; ++y ) for( std::size_t x=0; x<shape.nx; ++x ) {
						const std::size_t face=OpenMACFaceIndex3D(shape,2,x,y,z);
						const double inverseDensity=1.0/result.faceDensityKGPerM3.component[2][face];
						result.velocityMPerS.component[2][face]=
							unprojectedMomentumKGPerM2S.component[2][face]*inverseDensity-
							deltaTimeS*inverseDensity*(pressure[shape.Index(x,y,z)]-
							pressure[shape.Index(x,y,z-1)])/shape.cellWidthM;
						pressureOperator.faceCoefficient.component[2][face]=inverseDensity;
						if( !std::isfinite(result.velocityMPerS.component[2][face]) ) return Fail(
							error,"fire solver 3-D pressure-open z velocity overflowed");
					}

					double maximumHeadResidual = 0.0;
					// Fix prescribed normal velocities before any pressure-open face evaluates
					// its full tangential speed; boundary traversal order must not affect head.
					for( unsigned int side=0; side<6; ++side ) {
						const unsigned int axis=side/2; const bool positive=side%2;
						const std::size_t firstCount=side<2?shape.ny:shape.nx;
						const std::size_t secondCount=side<4?shape.nz:shape.ny;
						for( std::size_t second=0; second<secondCount; ++second ) for(
							std::size_t first=0; first<firstCount; ++first ) {
							const std::size_t index=OpenBoundaryFaceLinearIndex3D(shape,side,first,second);
							unsigned int kind=boundary.kind[side];
							if(side==4 && !boundary.bottomFuelMask.empty() &&
								boundary.bottomFuelMask[index]) kind=FuelInletBoundary3D;
							if(kind==PressureOpenBoundary3D) continue;
							std::size_t x=0,y=0,z=0;
							if(axis==0){x=positive?shape.nx:0;y=first;z=second;}
							if(axis==1){x=first;y=positive?shape.ny:0;z=second;}
							if(axis==2){x=first;y=second;z=positive?shape.nz:0;}
							const std::size_t face=OpenMACFaceIndex3D(shape,axis,x,y,z);
							result.velocityMPerS.component[axis][face]=kind==AdiabaticWallBoundary3D?
								0.0:((positive?-1.0:1.0)*OpenBoundaryFuelMassFluxKGPerM2S(
									boundary,side,index)/
								boundary.injectedGasDensityKGPerM3);
						}
					}
					for( unsigned int side=0; side<6; ++side ) {
						const unsigned int axis=side/2;
						const bool positive=side%2;
						const std::size_t firstCount=side<2?shape.ny:shape.nx;
						const std::size_t secondCount=side<4?shape.nz:shape.ny;
						for( std::size_t second=0; second<secondCount; ++second ) for(
							std::size_t first=0; first<firstCount; ++first ) {
							std::size_t x=0,y=0,z=0;
							if( axis==0 ) { x=positive?shape.nx:0; y=first; z=second; }
							if( axis==1 ) { x=first; y=positive?shape.ny:0; z=second; }
							if( axis==2 ) { x=first; y=second; z=positive?shape.nz:0; }
							const std::size_t face=OpenMACFaceIndex3D(shape,axis,x,y,z);
							const std::size_t cx=axis==0?(positive?shape.nx-1:0):x;
							const std::size_t cy=axis==1?(positive?shape.ny-1:0):y;
							const std::size_t cz=axis==2?(positive?shape.nz-1:0):z;
							const std::size_t cell=shape.Index(cx,cy,cz);
							const std::size_t boundaryIndex=OpenBoundaryFaceLinearIndex3D(
								shape,side,first,second);
							unsigned int kind=boundary.kind[side];
							if( side==4 && !boundary.bottomFuelMask.empty() &&
								boundary.bottomFuelMask[boundaryIndex] ) kind=FuelInletBoundary3D;
							const double sign=positive?1.0:-1.0;
							if( kind==AdiabaticWallBoundary3D ) {
								result.velocityMPerS.component[axis][face]=0.0;
								continue;
							}
							if( kind==FuelInletBoundary3D ) {
								const double fuelFlux=OpenBoundaryFuelMassFluxKGPerM2S(
									boundary,side,boundaryIndex);
								result.velocityMPerS.component[axis][face]=sign<0.0?
									fuelFlux/boundary.injectedGasDensityKGPerM3:
									-fuelFlux/boundary.injectedGasDensityKGPerM3;
								continue;
							}
							const double density=result.faceDensityKGPerM3.component[axis][face];
							const double outwardUnprojected=sign*
								unprojectedMomentumKGPerM2S.component[axis][face]/density;
							const double k=2.0*deltaTimeS/(density*shape.cellWidthM);
							if( !std::isfinite(k) ) return Fail(error,
								"fire solver 3-D pressure-open boundary scale overflowed");
							double facePressure=0.0,outwardVelocity=outwardUnprojected+k*pressure[cell];
							double tangentialSquared=0.0;
							std::array<double,3> faceVelocity = {{0.0,0.0,0.0}};
							faceVelocity[axis]=sign*outwardVelocity;
							for( unsigned int tangent=0; tangent<3; ++tangent ) if(tangent!=axis) {
								faceVelocity[tangent]=OpenBoundaryTangentialCellVelocity3D(shape,
									result.velocityMPerS,tangent,cx,cy,cz);
								tangentialSquared+=faceVelocity[tangent]*faceVelocity[tangent];
							}
							if( result.inflow[side][boundaryIndex] ) {
								for( std::size_t local=0; local<30; ++local ) {
									outwardVelocity=outwardUnprojected+k*(pressure[cell]-facePressure);
									const double localResidual=facePressure+0.5*
										boundary.ambientDensityKGPerM3*(outwardVelocity*outwardVelocity+
										tangentialSquared);
									const double derivative=1.0-boundary.ambientDensityKGPerM3*
										outwardVelocity*k;
									if( !std::isfinite(derivative) || derivative<=0.0 ) return Fail(error,
										"fire solver 3-D incoming total-head branch is singular");
									if( std::fabs(localResidual)<=boundary.pressureTolerancePa ) break;
									facePressure-=localResidual/derivative;
								}
								outwardVelocity=outwardUnprojected+k*(pressure[cell]-facePressure);
								maximumHeadResidual=std::max(maximumHeadResidual,std::fabs(facePressure+
									0.5*boundary.ambientDensityKGPerM3*(outwardVelocity*outwardVelocity+
									tangentialSquared)));
								const double denominator=1.0-boundary.ambientDensityKGPerM3*
									outwardVelocity*k;
								pressureOperator.faceCoefficient.component[axis][face]=
									2.0/density/denominator;
								for( unsigned int tangent=0; tangent<3; ++tangent ) if(tangent!=axis) {
									std::vector<std::pair<std::size_t,double> > derivative;
									AppendOpenBoundaryTangentialDerivative3D(shape,
										result.faceDensityKGPerM3,tangent,cx,cy,cz,deltaTimeS,derivative);
									const double factor=OpenBoundaryTangentialNewtonRowFactor3D(
										boundary.ambientDensityKGPerM3,density,faceVelocity[tangent],
										shape.cellWidthM,denominator);
									for( const std::pair<std::size_t,double>& entry : derivative ) {
										pressureOperator.rowExtra[cell].push_back(std::make_pair(
											entry.first,factor*entry.second));
									}
								}
							} else {
								pressureOperator.faceCoefficient.component[axis][face]=2.0/density;
							}
							result.boundaryDynamicPressurePa[side][boundaryIndex]=facePressure;
							result.velocityMPerS.component[axis][face]=sign*outwardVelocity;
						}
					}

					std::vector<double> nonlinearResidual(cellCount,0.0),rightHandSide(cellCount,0.0);
					double maximumDivergence=0.0;
					for( std::size_t z=0; z<shape.nz; ++z ) for( std::size_t y=0;
						y<shape.ny; ++y ) for( std::size_t x=0; x<shape.nx; ++x ) {
						const std::size_t cell=shape.Index(x,y,z);
						nonlinearResidual[cell]=OpenMACDivergence3D(shape,
							result.velocityMPerS,x,y,z)-divergenceTargetPerS[cell];
						if( !std::isfinite(nonlinearResidual[cell]) ) return Fail(error,
							"fire solver 3-D pressure-open divergence overflowed");
						maximumDivergence=std::max(maximumDivergence,
							std::fabs(nonlinearResidual[cell]));
						rightHandSide[cell]=-nonlinearResidual[cell]/deltaTimeS;
					}
					const double combined=std::max(maximumDivergence/absoluteTolerancePerS,
						maximumHeadResidual/boundary.pressureTolerancePa);
					result.nonlinearResidualHistory.push_back(combined);
					if( maximumDivergence<=absoluteTolerancePerS &&
						maximumHeadResidual<=boundary.pressureTolerancePa ) {
						nonlinearConverged=true;
						result.maximumDivergenceResidualPerS=maximumDivergence;
						result.maximumBoundaryHeadResidualPa=maximumHeadResidual;
						break;
					}
					if( evaluatingLineSearch ) {
						if( combined < lineSearchBaseResidual*(1.0-1.0e-4*lineSearchDamping) ) {
							evaluatingLineSearch=false;
						} else {
							lineSearchDamping*=0.5;
							if( lineSearchDamping < 1.0/1024.0 ) return Fail(error,
								"fire solver 3-D open Newton line search did not decrease the residual");
							for( std::size_t cell=0; cell<cellCount; ++cell ) pressure[cell]=
								lineSearchBasePressure[cell]+lineSearchDamping*lineSearchCorrection[cell];
							continue;
						}
					}
					std::vector<double> correction,linearHistory;
					if( !SolveOpenPressureMultigrid3D(shape,pressureOperator,rightHandSide,
						absoluteTolerancePerS/deltaTimeS,correction,linearHistory) ) return Fail(error,
						"fire solver 3-D open multigrid did not converge");
					for( const double value : linearHistory ) result.multigridResidualHistoryPerS.
						push_back(deltaTimeS*value);
					for( std::size_t cell=0; cell<cellCount; ++cell ) {
						if( !std::isfinite(correction[cell]) ) return Fail(error,
							"fire solver 3-D open Newton correction overflowed");
					}
					lineSearchBasePressure=pressure;
					lineSearchCorrection=correction;
					lineSearchBaseResidual=combined;
					lineSearchDamping=1.0;
					for( std::size_t cell=0; cell<cellCount; ++cell ) pressure[cell]+=
						lineSearchCorrection[cell];
					evaluatingLineSearch=true;
				}
				if( !nonlinearConverged ) return Fail(error,
					"fire solver 3-D pressure-open nonlinear solve did not converge");

				bool changed=false;
				for( unsigned int side=0; side<6; ++side ) {
					if( boundary.kind[side]!=PressureOpenBoundary3D ) continue;
					const unsigned int axis=side/2;
					const bool positive=side%2;
					const std::size_t firstCount=side<2?shape.ny:shape.nx;
					const std::size_t secondCount=side<4?shape.nz:shape.ny;
					for( std::size_t second=0; second<secondCount; ++second ) for(
						std::size_t first=0; first<firstCount; ++first ) {
						std::size_t x=0,y=0,z=0;
						if(axis==0){x=positive?shape.nx:0;y=first;z=second;}
						if(axis==1){x=first;y=positive?shape.ny:0;z=second;}
						if(axis==2){x=first;y=second;z=positive?shape.nz:0;}
						const std::size_t face=OpenMACFaceIndex3D(shape,axis,x,y,z);
						const double outward=(positive?1.0:-1.0)*result.velocityMPerS.component[axis][face];
						const std::size_t index=OpenBoundaryFaceLinearIndex3D(shape,side,first,second);
						const bool next=outward < -boundary.velocityToleranceMPerS ? true :
							(outward > boundary.velocityToleranceMPerS ? false : result.inflow[side][index]);
						changed=changed || next!=result.inflow[side][index];
						result.inflow[side][index]=next;
					}
				}
				if( !changed ) {
					result.stepAverageDynamicPressurePa=pressure;
					for( unsigned int axis=0; axis<3; ++axis ) for( std::size_t face=0;
						face<result.velocityMPerS.component[axis].size(); ++face ) {
						result.momentumKGPerM2S.component[axis][face]=
							result.faceDensityKGPerM3.component[axis][face]*
							result.velocityMPerS.component[axis][face];
						if( !std::isfinite(result.momentumKGPerM2S.component[axis][face]) ) return Fail(
							error,"fire solver 3-D pressure-open momentum overflowed");
					}
					PopulateOpenBoundaryTangentialVelocity3D(shape,boundary,result);
					return true;
				}
			}
			return Fail(error,"fire solver 3-D pressure-open active set did not converge");
		}

		struct OpenAugmentedPressureLayout3D
		{
			std::array<std::vector<std::size_t>,6> boundaryUnknown;
			std::size_t unknownCount;
			OpenAugmentedPressureLayout3D() : unknownCount(0) {}
		};

		inline void BuildOpenAugmentedPressureLayout3D(
			const PeriodicMACShape& shape,
			const OpenBoundaryConfig3D& boundary,
			OpenAugmentedPressureLayout3D& layout
			)
		{
			layout.unknownCount=shape.CellCount();
			for( unsigned int side=0; side<6; ++side ) {
				const std::size_t count=OpenBoundaryFaceCount3D(shape,side);
				layout.boundaryUnknown[side].assign(count,
					std::numeric_limits<std::size_t>::max());
				for( std::size_t index=0; index<count; ++index ) {
					unsigned int kind=boundary.kind[side];
					if(side==4 && !boundary.bottomFuelMask.empty() &&
						boundary.bottomFuelMask[index]) kind=FuelInletBoundary3D;
					if(kind==PressureOpenBoundary3D) layout.boundaryUnknown[side][index]=
						layout.unknownCount++;
				}
			}
		}

		template<typename ApplyOperator,typename ApplyPreconditioner>
		inline bool SolveOpenAugmentedBiCGStab(
			const std::vector<double>& rightHandSide,
			const double tolerance,
			const ApplyOperator& applyOperator,
			const ApplyPreconditioner& applyPreconditioner,
			std::vector<double>& solution,
			const unsigned int workerCount
			)
		{
			const std::size_t count=rightHandSide.size();
			const unsigned int workers=std::max(1u,std::min(workerCount,
				static_cast<unsigned int>(std::min<std::size_t>(count,
					std::numeric_limits<unsigned int>::max()))));
			auto parallelMaximum=[&](const std::vector<double>& values){
				std::vector<double> local(workers,0.0);
				ParallelFireSlices(workers,workers,[&](const std::size_t worker){
					const std::size_t first=count*worker/workers;
					const std::size_t last=count*(worker+1u)/workers;
					for(std::size_t i=first;i<last;++i)local[worker]=
						std::max(local[worker],std::fabs(values[i]));});
				double maximum=0.0;for(const double value:local)maximum=std::max(maximum,value);
				return maximum;
			};
			solution.assign(count,0.0);
			std::vector<double> r=rightHandSide,rHat=r,p(count,0.0),v(count,0.0),
				s(count,0.0),t(count,0.0),pHat,sHat;
			double rhoPrevious=1.0,alpha=1.0,omega=1.0;
			static const std::size_t maximumIterations=28u;
			for( std::size_t iteration=0; iteration<maximumIterations; ++iteration ) {
				const double maximum=parallelMaximum(r);
				if(maximum<=tolerance) return true;
				const double rho=OpenVectorDot3D(rHat,r);
				if(!std::isfinite(rho) || rho==0.0) return false;
				const double beta=(rho/rhoPrevious)*(alpha/omega);
				if(!std::isfinite(beta)) return false;
				ParallelFireSlices(count,workerCount,[&](const std::size_t i){
					p[i]=r[i]+beta*(p[i]-omega*v[i]);});
				if(!applyPreconditioner(p,pHat) || !applyOperator(pHat,v)) return false;
				const double denominator=OpenVectorDot3D(rHat,v);
				if(!std::isfinite(denominator) || denominator==0.0) return false;
				alpha=rho/denominator;
				ParallelFireSlices(count,workerCount,[&](const std::size_t i){
					s[i]=r[i]-alpha*v[i];});
				const double sMaximum=parallelMaximum(s);
				if(sMaximum<=tolerance){ParallelFireSlices(count,workerCount,
					[&](const std::size_t i){solution[i]+=alpha*pHat[i];});
					return true;}
				if(!applyPreconditioner(s,sHat) || !applyOperator(sHat,t))return false;
				const double tSquared=OpenVectorDot3D(t,t);
				if(!std::isfinite(tSquared) || tSquared==0.0)return false;
				omega=OpenVectorDot3D(t,s)/tSquared;
				if(!std::isfinite(omega) || omega==0.0)return false;
				std::atomic<bool> finite(true);
				ParallelFireSlices(count,workerCount,[&](const std::size_t i){
					solution[i]+=alpha*pHat[i]+omega*sHat[i];r[i]=s[i]-omega*t[i];
					if(!std::isfinite(solution[i])||!std::isfinite(r[i]))
						finite.store(false,std::memory_order_relaxed);});
				if(!finite.load(std::memory_order_relaxed))return false;
				rhoPrevious=rho;
			}
			return false;
		}

		inline bool ProjectPressureOpenMACVelocity3D(
			const PeriodicMACShape& shape,
			const std::vector<double>& gasDensityKGPerM3,
			const OpenMACField3D& unprojectedMomentumKGPerM2S,
			const std::vector<double>& divergenceTargetPerS,
			const OpenBoundaryConfig3D& boundary,
			const double deltaTimeS,
			const double absoluteTolerancePerS,
			OpenMACProjection3DResult& result,
			std::string* error = 0,
			const unsigned int workerCount = 1u,
			const bool fixedActiveSet = false
			)
		{
			FireProfileIncrement(FireProfile().projCalls);
			FireProfileScopedNs profileTimer(FireProfile().nsProjection);
			if(!ValidateOpenBoundaryConfig3D(shape,boundary,error) ||
				gasDensityKGPerM3.size()!=shape.CellCount() ||
				divergenceTargetPerS.size()!=shape.CellCount() ||
				!std::isfinite(deltaTimeS) || deltaTimeS<=0.0 ||
				!std::isfinite(absoluteTolerancePerS) || absoluteTolerancePerS<=0.0 ||
				!std::isfinite(1.0/shape.cellWidthM) ||
				!std::isfinite(1.0/(shape.cellWidthM*shape.cellWidthM))) return Fail(error,
				"fire solver augmented 3-D pressure-open input is malformed");
			const std::size_t cellCount=shape.CellCount();
			OpenMACProjection3DResult candidate;
			for(std::size_t cell=0;cell<cellCount;++cell)if(!std::isfinite(
				gasDensityKGPerM3[cell]) || gasDensityKGPerM3[cell]<=0.0 ||
				!std::isfinite(divergenceTargetPerS[cell]))return Fail(error,
				"fire solver augmented 3-D pressure-open cell is invalid");
			for(unsigned int axis=0;axis<3;++axis){const std::size_t faceCount=
				OpenMACFaceCount3D(shape,axis);if(unprojectedMomentumKGPerM2S.component[axis].
				size()!=faceCount)return Fail(error,"fire solver augmented 3-D face shape is invalid");
				candidate.faceDensityKGPerM3.component[axis].assign(faceCount,0.0);
				candidate.velocityMPerS.component[axis].assign(faceCount,0.0);
				candidate.momentumKGPerM2S.component[axis].assign(faceCount,0.0);
				for(const double value:unprojectedMomentumKGPerM2S.component[axis])if(
					!std::isfinite(value))return Fail(error,"fire solver augmented momentum is non-finite");}
			for(unsigned int side=0;side<6;++side){const std::size_t count=
				OpenBoundaryFaceCount3D(shape,side);candidate.inflow[side]=
				boundary.priorInflow[side].empty()?std::vector<bool>(count,false):
				boundary.priorInflow[side];candidate.boundaryDynamicPressurePa[side].assign(count,0.0);}
			for(std::size_t z=0;z<shape.nz;++z)for(std::size_t y=0;y<shape.ny;++y)
				for(std::size_t x=0;x<=shape.nx;++x){const std::size_t face=
				OpenMACFaceIndex3D(shape,0,x,y,z);const double a=x?gasDensityKGPerM3[
				shape.Index(x-1,y,z)]:boundary.ambientDensityKGPerM3;const double b=x<shape.nx?
				gasDensityKGPerM3[shape.Index(x,y,z)]:boundary.ambientDensityKGPerM3;
				candidate.faceDensityKGPerM3.component[0][face]=0.5*a+0.5*b;}
			for(std::size_t z=0;z<shape.nz;++z)for(std::size_t y=0;y<=shape.ny;++y)
				for(std::size_t x=0;x<shape.nx;++x){const std::size_t face=
				OpenMACFaceIndex3D(shape,1,x,y,z);const double a=y?gasDensityKGPerM3[
				shape.Index(x,y-1,z)]:boundary.ambientDensityKGPerM3;const double b=y<shape.ny?
				gasDensityKGPerM3[shape.Index(x,y,z)]:boundary.ambientDensityKGPerM3;
				candidate.faceDensityKGPerM3.component[1][face]=0.5*a+0.5*b;}
			for(std::size_t z=0;z<=shape.nz;++z)for(std::size_t y=0;y<shape.ny;++y)
				for(std::size_t x=0;x<shape.nx;++x){const std::size_t face=
				OpenMACFaceIndex3D(shape,2,x,y,z);const bool fuel=z==0 &&
				!boundary.bottomFuelMask.empty()&&boundary.bottomFuelMask[y*shape.nx+x];
				const double outside=fuel?boundary.injectedGasDensityKGPerM3:
				boundary.ambientDensityKGPerM3;const double a=z?gasDensityKGPerM3[
				shape.Index(x,y,z-1)]:outside;const double b=z<shape.nz?gasDensityKGPerM3[
				shape.Index(x,y,z)]:outside;candidate.faceDensityKGPerM3.component[2][face]=
				0.5*a+0.5*b;}
			for(unsigned int axis=0;axis<3;++axis)for(const double density:
				candidate.faceDensityKGPerM3.component[axis])if(!std::isfinite(density)||density<=0.0)
				return Fail(error,"fire solver augmented face density overflowed");

			OpenAugmentedPressureLayout3D layout;BuildOpenAugmentedPressureLayout3D(shape,
				boundary,layout);
			OpenPressureOperator3D preconditionerOperator;
			preconditionerOperator.rowExtra.assign(cellCount,
				std::vector<std::pair<std::size_t,double> >());
			for( unsigned int axis=0; axis<3; ++axis ) preconditionerOperator.
				faceCoefficient.component[axis].assign(OpenMACFaceCount3D(shape,axis),0.0);
			for( std::size_t z=0; z<shape.nz; ++z ) for( std::size_t y=0; y<shape.ny;
				++y ) for( std::size_t x=1; x<shape.nx; ++x ) {
				const std::size_t face=OpenMACFaceIndex3D(shape,0,x,y,z);
				preconditionerOperator.faceCoefficient.component[0][face]=1.0/
					candidate.faceDensityKGPerM3.component[0][face];
			}
			for( std::size_t z=0; z<shape.nz; ++z ) for( std::size_t y=1; y<shape.ny;
				++y ) for( std::size_t x=0; x<shape.nx; ++x ) {
				const std::size_t face=OpenMACFaceIndex3D(shape,1,x,y,z);
				preconditionerOperator.faceCoefficient.component[1][face]=1.0/
					candidate.faceDensityKGPerM3.component[1][face];
			}
			for( std::size_t z=1; z<shape.nz; ++z ) for( std::size_t y=0; y<shape.ny;
				++y ) for( std::size_t x=0; x<shape.nx; ++x ) {
				const std::size_t face=OpenMACFaceIndex3D(shape,2,x,y,z);
				preconditionerOperator.faceCoefficient.component[2][face]=1.0/
					candidate.faceDensityKGPerM3.component[2][face];
			}
			for( unsigned int side=0; side<6; ++side ) {
				const unsigned int axis=side/2; const bool positive=side%2;
				const std::size_t firstCount=side<2?shape.ny:shape.nx;
				const std::size_t secondCount=side<4?shape.nz:shape.ny;
				for( std::size_t second=0; second<secondCount; ++second ) for(
					std::size_t first=0; first<firstCount; ++first ) {
					const std::size_t index=OpenBoundaryFaceLinearIndex3D(shape,side,first,second);
					if( layout.boundaryUnknown[side][index]==
						std::numeric_limits<std::size_t>::max() ) continue;
					std::size_t x=0,y=0,z=0;
					if(axis==0){x=positive?shape.nx:0;y=first;z=second;}
					if(axis==1){x=first;y=positive?shape.ny:0;z=second;}
					if(axis==2){x=first;y=second;z=positive?shape.nz:0;}
					const std::size_t face=OpenMACFaceIndex3D(shape,axis,x,y,z);
					preconditionerOperator.faceCoefficient.component[axis][face]=2.0/
						candidate.faceDensityKGPerM3.component[axis][face];
				}
			}
			std::vector<PeriodicMACShape> preconditionerShape(1u,shape);
			std::vector<OpenPressureOperator3D> preconditionerHierarchy;
			preconditionerHierarchy.push_back(std::move(preconditionerOperator));
			for(;;) {
				PeriodicMACShape coarseShape;
				OpenPressureOperator3D coarseOperator;
				if(!CoarsenOpenPressureOperator3D(preconditionerShape.back(),
					preconditionerHierarchy.back(),coarseShape,coarseOperator)) break;
				preconditionerShape.push_back(coarseShape);
				preconditionerHierarchy.push_back(std::move(coarseOperator));
			}
			std::vector<std::vector<double> > preconditionerDiagonal(
				preconditionerHierarchy.size());
			for(std::size_t level=0;level<preconditionerHierarchy.size();++level)
				BuildOpenPressureDiagonal3D(preconditionerShape[level],
					preconditionerHierarchy[level],preconditionerDiagonal[level],
					level?1u:workerCount);
			std::vector<double> unknown(layout.unknownCount,0.0);
			std::vector<std::vector<unsigned char> > activeHistory;
			std::vector<std::array<std::vector<bool>,6> > activeSetHistory;
			std::vector<double> activeDiscrepancyHistory;
			auto evaluateVelocity=[&](const std::vector<double>& value,OpenMACField3D& velocity){
				for(unsigned int axis=0;axis<3;++axis)velocity.component[axis].assign(
					OpenMACFaceCount3D(shape,axis),0.0);
				ParallelFireSlices(shape.nz,workerCount,[&](const std::size_t z){for(std::size_t y=0;
					y<shape.ny;++y)for(std::size_t x=1;x<shape.nx;++x){const std::size_t f=OpenMACFaceIndex3D(shape,0,x,y,z);
					velocity.component[0][f]=unprojectedMomentumKGPerM2S.component[0][f]/
					candidate.faceDensityKGPerM3.component[0][f]-deltaTimeS*(value[shape.Index(x,y,z)]-
					value[shape.Index(x-1,y,z)])/(candidate.faceDensityKGPerM3.component[0][f]*shape.cellWidthM);}});
				ParallelFireSlices(shape.nz,workerCount,[&](const std::size_t z){for(std::size_t y=1;
					y<shape.ny;++y)for(std::size_t x=0;x<shape.nx;++x){const std::size_t f=OpenMACFaceIndex3D(shape,1,x,y,z);
					velocity.component[1][f]=unprojectedMomentumKGPerM2S.component[1][f]/
					candidate.faceDensityKGPerM3.component[1][f]-deltaTimeS*(value[shape.Index(x,y,z)]-
					value[shape.Index(x,y-1,z)])/(candidate.faceDensityKGPerM3.component[1][f]*shape.cellWidthM);}});
				ParallelFireSlices(shape.nz,workerCount,[&](const std::size_t z){if(!z)return;for(std::size_t y=0;
					y<shape.ny;++y)for(std::size_t x=0;x<shape.nx;++x){const std::size_t f=OpenMACFaceIndex3D(shape,2,x,y,z);
					velocity.component[2][f]=unprojectedMomentumKGPerM2S.component[2][f]/
					candidate.faceDensityKGPerM3.component[2][f]-deltaTimeS*(value[shape.Index(x,y,z)]-
					value[shape.Index(x,y,z-1)])/(candidate.faceDensityKGPerM3.component[2][f]*shape.cellWidthM);}});
				for(unsigned int side=0;side<6;++side){const unsigned int axis=side/2;const bool positive=side%2;
					const std::size_t firstCount=side<2?shape.ny:shape.nx,secondCount=side<4?shape.nz:shape.ny;
					for(std::size_t second=0;second<secondCount;++second)for(std::size_t first=0;first<firstCount;++first){
						std::size_t x=0,y=0,z=0;if(axis==0){x=positive?shape.nx:0;y=first;z=second;}
						if(axis==1){x=first;y=positive?shape.ny:0;z=second;}if(axis==2){x=first;y=second;z=positive?shape.nz:0;}
						const std::size_t index=OpenBoundaryFaceLinearIndex3D(shape,side,first,second);
						unsigned int kind=boundary.kind[side];
						if(side==4&&!boundary.bottomFuelMask.empty()&&
							boundary.bottomFuelMask[index]) kind=FuelInletBoundary3D;
						const std::size_t f=
						OpenMACFaceIndex3D(shape,axis,x,y,z);const double sign=positive?1.0:-1.0;
						if(kind==AdiabaticWallBoundary3D){velocity.component[axis][f]=0.0;continue;}
						if(kind==FuelInletBoundary3D){velocity.component[axis][f]=(positive?-1.0:1.0)*
						OpenBoundaryFuelMassFluxKGPerM2S(boundary,side,index)/
						candidate.faceDensityKGPerM3.component[axis][f];continue;}
						const std::size_t cx=axis==0?(positive?shape.nx-1:0):x,cy=axis==1?
						(positive?shape.ny-1:0):y,cz=axis==2?(positive?shape.nz-1:0):z;
						const std::size_t cell=shape.Index(cx,cy,cz),bindex=layout.boundaryUnknown[side][index];
						const double density=candidate.faceDensityKGPerM3.component[axis][f];
						const double outward=sign*unprojectedMomentumKGPerM2S.component[axis][f]/density+
						2.0*deltaTimeS*(value[cell]-value[bindex])/(density*shape.cellWidthM);
						velocity.component[axis][f]=sign*outward;}}
				for(unsigned int axis=0;axis<3;++axis) for(const double v:
					velocity.component[axis]) {
					if(!std::isfinite(v)) return false;
				}
				return true;
			};
			auto evaluateResidual=[&](const std::vector<double>& value,OpenMACField3D& velocity,
				std::vector<double>& residual){if(!evaluateVelocity(value,velocity))return false;
				residual.assign(layout.unknownCount,0.0);ParallelFireSlices(shape.nz,workerCount,
				[&](const std::size_t z){for(std::size_t y=0;y<shape.ny;++y)for(std::size_t x=0;
				x<shape.nx;++x){const std::size_t cell=shape.Index(x,y,z);residual[cell]=
				OpenMACDivergence3D(shape,velocity,x,y,z)-divergenceTargetPerS[cell];}});
				for(unsigned int side=0;side<6;++side){const unsigned int axis=side/2;const bool positive=side%2;
				const std::size_t firstCount=side<2?shape.ny:shape.nx,secondCount=side<4?shape.nz:shape.ny;
				for(std::size_t second=0;second<secondCount;++second)for(std::size_t first=0;first<firstCount;++first){
				const std::size_t index=OpenBoundaryFaceLinearIndex3D(shape,side,first,second),u=
				layout.boundaryUnknown[side][index];if(u==std::numeric_limits<std::size_t>::max())continue;
				std::size_t x=0,y=0,z=0;if(axis==0){x=positive?shape.nx:0;y=first;z=second;}
				if(axis==1){x=first;y=positive?shape.ny:0;z=second;}if(axis==2){x=first;y=second;z=positive?shape.nz:0;}
				double speed=velocity.component[axis][OpenMACFaceIndex3D(shape,axis,x,y,z)];speed*=speed;
				const std::size_t cx=axis==0?(positive?shape.nx-1:0):x,cy=axis==1?(positive?shape.ny-1:0):y,
				cz=axis==2?(positive?shape.nz-1:0):z;for(unsigned int tangent=0;tangent<3;++tangent)if(tangent!=axis){
				const double tv=OpenBoundaryTangentialCellVelocity3D(shape,velocity,tangent,cx,cy,cz);speed+=tv*tv;}
				residual[u]=value[u]+(candidate.inflow[side][index]?0.5*boundary.ambientDensityKGPerM3*speed:0.0);}}
				for(const double v:residual)if(!std::isfinite(v))return false;return true;};

			auto publishCandidate=[&](){
				candidate.stepAverageDynamicPressurePa.assign(unknown.begin(),
					unknown.begin()+cellCount);
				for(unsigned int side=0;side<6;++side)for(std::size_t i=0;
					i<layout.boundaryUnknown[side].size();++i){const std::size_t u=
					layout.boundaryUnknown[side][i];if(u!=std::numeric_limits<std::size_t>::max())
					candidate.boundaryDynamicPressurePa[side][i]=unknown[u];}
				for(unsigned int axis=0;axis<3;++axis)for(std::size_t f=0;
					f<candidate.velocityMPerS.component[axis].size();++f){
					candidate.momentumKGPerM2S.component[axis][f]=candidate.
						faceDensityKGPerM3.component[axis][f]*candidate.velocityMPerS.
						component[axis][f];
					if(!std::isfinite(candidate.momentumKGPerM2S.component[axis][f]))
						return Fail(error,"fire solver augmented momentum overflowed");}
				PopulateOpenBoundaryTangentialVelocity3D(shape,boundary,candidate);
				result=candidate;return true;
			};

			for(std::size_t active=0;active<(fixedActiveSet?1u:16u);++active){
				const std::vector<unsigned char> activeState=FlattenOpenActiveSet3D(
					candidate.inflow);
				const std::vector<std::vector<unsigned char> >::const_iterator repeated=
					std::find(activeHistory.begin(),activeHistory.end(),activeState);
				if(repeated!=activeHistory.end()) {
					const std::size_t cycleFirst=static_cast<std::size_t>(repeated-
						activeHistory.begin());
					const std::size_t selected=OpenActiveSetCanonicalHistoryIndex3D(
						activeDiscrepancyHistory,activeHistory,cycleFirst);
					OpenBoundaryConfig3D frozenBoundary=boundary;
					frozenBoundary.priorInflow=activeSetHistory[selected];
					OpenMACProjection3DResult frozen;
					if(!ProjectPressureOpenMACVelocity3D(shape,gasDensityKGPerM3,
						unprojectedMomentumKGPerM2S,divergenceTargetPerS,frozenBoundary,
						deltaTimeS,absoluteTolerancePerS,frozen,error,1u,true))return false;
					frozen.activeSetDiscontinuousClass=true;
					frozen.activeSetCycleLength=activeHistory.size()-cycleFirst;
					for(std::size_t face=0;face<activeHistory[selected].size();++face){
						bool differs=false;
						for(std::size_t i=cycleFirst;i<activeHistory.size();++i)
							differs=differs||activeHistory[i][face]!=activeHistory[selected][face];
						frozen.activeSetDifferingFaceCount+=differs?1u:0u;
					}
					frozen.maximumActiveSetComplementarityDiscrepancyMPerS=
						OpenActiveSetComplementarityDiscrepancy3D(shape,boundary,frozen);
					result=std::move(frozen);return true;
				}
				activeHistory.push_back(activeState);
				activeSetHistory.push_back(candidate.inflow);
				bool converged=false;for(std::size_t nonlinear=0;nonlinear<12;++nonlinear){OpenMACField3D velocity;
					std::vector<double> residual;
					if(!evaluateResidual(unknown,velocity,residual)) return Fail(error,
						"fire solver augmented residual overflowed");
					double maximumDivergence=0.0,maximumHead=0.0;
					for(std::size_t i=0;i<cellCount;++i)maximumDivergence=std::max(maximumDivergence,std::fabs(residual[i]));
					if(candidate.nonlinearResidualHistory.empty())
						candidate.maximumPreProjectionDivergenceResidualPerS=maximumDivergence;
					for(std::size_t i=cellCount;i<layout.unknownCount;++i)maximumHead=std::max(maximumHead,std::fabs(residual[i]));
					const double norm=std::max(maximumDivergence/absoluteTolerancePerS,maximumHead/boundary.pressureTolerancePa);
					candidate.nonlinearResidualHistory.push_back(norm);if(maximumDivergence<=absoluteTolerancePerS &&
					maximumHead<=boundary.pressureTolerancePa){candidate.velocityMPerS=velocity;candidate.maximumDivergenceResidualPerS=
					maximumDivergence;candidate.maximumBoundaryHeadResidualPa=maximumHead;converged=true;break;}
					OpenMACField3D affineVelocity;
					std::vector<double> zeroDirection(layout.unknownCount,0.0);
					if(!evaluateVelocity(zeroDirection,affineVelocity)) return Fail(error,
						"fire solver augmented affine velocity overflowed");
					auto applyJ=[&](const std::vector<double>& direction,std::vector<double>& output){
						OpenMACField3D dVelocity;if(!evaluateVelocity(direction,dVelocity))return false;
						// evaluateVelocity is affine; remove the unprojected/prescribed constant.
						for(unsigned int axis=0;axis<3;++axis)
						for(std::size_t f=0;f<dVelocity.component[axis].size();++f)dVelocity.component[axis][f]-=affineVelocity.component[axis][f];
						output.assign(layout.unknownCount,0.0);ParallelFireSlices(shape.nz,workerCount,
						[&](const std::size_t z){for(std::size_t y=0;y<shape.ny;++y)for(std::size_t x=0;
						x<shape.nx;++x)output[shape.Index(x,y,z)]=OpenMACDivergence3D(shape,dVelocity,x,y,z);});
						for(unsigned int side=0;side<6;++side){const unsigned int axis=side/2;const bool positive=side%2;
						const std::size_t firstCount=side<2?shape.ny:shape.nx,secondCount=side<4?shape.nz:shape.ny;
						for(std::size_t second=0;second<secondCount;++second)for(std::size_t first=0;first<firstCount;++first){
						const std::size_t index=OpenBoundaryFaceLinearIndex3D(shape,side,first,second),u=layout.boundaryUnknown[side][index];
						if(u==std::numeric_limits<std::size_t>::max())continue;output[u]=direction[u];if(!candidate.inflow[side][index])continue;
						std::size_t x=0,y=0,z=0;if(axis==0){x=positive?shape.nx:0;y=first;z=second;}if(axis==1){x=first;y=positive?shape.ny:0;z=second;}
						if(axis==2){x=first;y=second;z=positive?shape.nz:0;}const std::size_t cx=axis==0?(positive?shape.nx-1:0):x,
						cy=axis==1?(positive?shape.ny-1:0):y,cz=axis==2?(positive?shape.nz-1:0):z;
						const std::size_t f=OpenMACFaceIndex3D(shape,axis,x,y,z);double dot=velocity.component[axis][f]*dVelocity.component[axis][f];
						for(unsigned int tangent=0;tangent<3;++tangent)if(tangent!=axis)dot+=OpenBoundaryTangentialCellVelocity3D(shape,
						velocity,tangent,cx,cy,cz)*OpenBoundaryTangentialCellVelocity3D(shape,dVelocity,tangent,cx,cy,cz);
						output[u]+=boundary.ambientDensityKGPerM3*dot;}}return true;};
					auto precondition=[&](const std::vector<double>& rhs,
						std::vector<double>& output) {
						output.assign(layout.unknownCount,0.0);
						std::vector<double> cellRight(rhs.begin(),rhs.begin()+cellCount);
						std::vector<double> cellSolution(cellCount,0.0);
						OpenPressureMultigridHierarchyVCycle3D(preconditionerShape,
							preconditionerHierarchy,preconditionerDiagonal,0u,cellRight,
							cellSolution,workerCount);
						for( std::size_t cell=0; cell<cellCount; ++cell ) output[cell]=
							cellSolution[cell]/deltaTimeS;
						for( std::size_t i=cellCount; i<layout.unknownCount; ++i ) {
							output[i]=rhs[i];
						}
						return true;
					};
					std::vector<double> rhs=residual,update;for(double& v:rhs)v=-v;
					const double linearTolerance=std::min(absoluteTolerancePerS,boundary.pressureTolerancePa)*0.1;
					if(!SolveOpenAugmentedBiCGStab(rhs,linearTolerance,applyJ,precondition,update,
						workerCount)) {
						return Fail(error,"fire solver augmented Newton system did not converge");
					}
					double damping=1.0;bool accepted=false;
					candidate.multigridResidualHistoryPerS.push_back(norm);
					while(damping>=1.0/1024.0){std::vector<double> trial=unknown,trialResidual;OpenMACField3D trialVelocity;
						for(std::size_t i=0;i<trial.size();++i)trial[i]+=damping*update[i];if(evaluateResidual(trial,trialVelocity,trialResidual)){
						double td=0.0,th=0.0;for(std::size_t i=0;i<cellCount;++i)td=std::max(td,std::fabs(trialResidual[i]));
						for(std::size_t i=cellCount;i<trial.size();++i)th=std::max(th,std::fabs(trialResidual[i]));
						if(std::max(td/absoluteTolerancePerS,th/boundary.pressureTolerancePa)<norm){unknown.swap(trial);accepted=true;break;}}
						damping*=0.5;}if(!accepted)return Fail(error,"fire solver augmented Newton line search failed");}
				if(!converged)return Fail(error,"fire solver augmented nonlinear solve did not converge");
				activeDiscrepancyHistory.push_back(OpenActiveSetComplementarityDiscrepancy3D(
					shape,boundary,candidate));
				if(fixedActiveSet)return publishCandidate();
				bool changed=false;
				for(unsigned int side=0;side<6;++side){const unsigned int axis=side/2;const bool positive=side%2;
				const std::size_t firstCount=side<2?shape.ny:shape.nx,secondCount=side<4?shape.nz:shape.ny;
				for(std::size_t second=0;second<secondCount;++second)for(std::size_t first=0;first<firstCount;++first){
				const std::size_t index=OpenBoundaryFaceLinearIndex3D(shape,side,first,second);
				if(layout.boundaryUnknown[side][index]==std::numeric_limits<std::size_t>::max()) continue;
				std::size_t x=0,y=0,z=0;if(axis==0){x=positive?shape.nx:0;y=first;z=second;}
				if(axis==1){x=first;y=positive?shape.ny:0;z=second;}if(axis==2){x=first;y=second;z=positive?shape.nz:0;}
				const double outward=(positive?1.0:-1.0)*candidate.velocityMPerS.component[axis][OpenMACFaceIndex3D(shape,axis,x,y,z)];
				const bool next=outward < -boundary.velocityToleranceMPerS?true:(outward>boundary.velocityToleranceMPerS?false:
				candidate.inflow[side][index]);changed=changed||next!=candidate.inflow[side][index];candidate.inflow[side][index]=next;}}
				if(!changed)return publishCandidate();}
			return Fail(error,"fire solver augmented active set did not converge");
		}

		inline bool ProjectPressureOpenMACVelocity3DFinal(
			const PeriodicMACShape& shape,
			const std::vector<double>& gasDensityKGPerM3,
			const OpenMACField3D& unprojectedMomentumKGPerM2S,
			const std::vector<double>& divergenceTargetPerS,
			const OpenBoundaryConfig3D& boundary,
			const OpenMACProjection3DResult& stage0,
			const OpenMACProjection3DResult& stage1,
			const double deltaTimeS,
			const double absoluteTolerancePerS,
			OpenMACProjection3DResult& publishedResult,
			std::string* error = 0,
			const unsigned int workerCount = 1u
			)
		{
			FireProfileIncrement(FireProfile().projCalls);
			FireProfileScopedNs profileTimer(FireProfile().nsProjection);
			if( !ValidateOpenBoundaryConfig3D(shape,boundary,error) ||
				gasDensityKGPerM3.size()!=shape.CellCount() ||
				divergenceTargetPerS.size()!=shape.CellCount() ||
				!std::isfinite(deltaTimeS) || deltaTimeS<=0.0 ||
				!std::isfinite(absoluteTolerancePerS) || absoluteTolerancePerS<=0.0 ) {
				return Fail(error,"fire solver final 3-D pressure-open input is malformed");
			}
			const std::size_t cellCount=shape.CellCount();
			OpenMACProjection3DResult result;
			for( std::size_t cell=0; cell<cellCount; ++cell ) if(
				!std::isfinite(gasDensityKGPerM3[cell]) || gasDensityKGPerM3[cell]<=0.0 ||
				!std::isfinite(divergenceTargetPerS[cell]) ) return Fail(error,
				"fire solver final 3-D pressure-open cell is invalid");
			for( unsigned int axis=0; axis<3; ++axis ) {
				const std::size_t count=OpenMACFaceCount3D(shape,axis);
				if( unprojectedMomentumKGPerM2S.component[axis].size()!=count ||
					stage0.velocityMPerS.component[axis].size()!=count ||
					stage1.velocityMPerS.component[axis].size()!=count ) return Fail(error,
					"fire solver final 3-D pressure-open face shape is invalid");
				result.faceDensityKGPerM3.component[axis].assign(count,0.0);
				result.velocityMPerS.component[axis].assign(count,0.0);
				result.momentumKGPerM2S.component[axis].assign(count,0.0);
				for( const double value : unprojectedMomentumKGPerM2S.component[axis] ) if(
					!std::isfinite(value) ) return Fail(error,
					"fire solver final 3-D pressure-open momentum is non-finite");
				for( const double value : stage0.velocityMPerS.component[axis] ) if(
					!std::isfinite(value) ) return Fail(error,
					"fire solver final 3-D stage-0 velocity is non-finite");
				for( const double value : stage1.velocityMPerS.component[axis] ) if(
					!std::isfinite(value) ) return Fail(error,
					"fire solver final 3-D stage-1 velocity is non-finite");
			}
			for( unsigned int side=0; side<6; ++side ) {
				const std::size_t count=OpenBoundaryFaceCount3D(shape,side);
				if( stage0.inflow[side].size()!=count || stage1.inflow[side].size()!=count ) {
					return Fail(error,"fire solver final 3-D pressure-open class shape is invalid");
				}
				result.inflow[side].assign(count,false);
				result.boundaryDynamicPressurePa[side].assign(count,0.0);
			}

			for( std::size_t z=0; z<shape.nz; ++z ) for( std::size_t y=0;
				y<shape.ny; ++y ) for( std::size_t x=0; x<=shape.nx; ++x ) {
				const std::size_t face=OpenMACFaceIndex3D(shape,0,x,y,z);
				const double first=x?gasDensityKGPerM3[shape.Index(x-1,y,z)]:
					boundary.ambientDensityKGPerM3;
				const double second=x<shape.nx?gasDensityKGPerM3[shape.Index(x,y,z)]:
					boundary.ambientDensityKGPerM3;
				result.faceDensityKGPerM3.component[0][face]=0.5*first+0.5*second;
			}
			for( std::size_t z=0; z<shape.nz; ++z ) for( std::size_t y=0;
				y<=shape.ny; ++y ) for( std::size_t x=0; x<shape.nx; ++x ) {
				const std::size_t face=OpenMACFaceIndex3D(shape,1,x,y,z);
				const double first=y?gasDensityKGPerM3[shape.Index(x,y-1,z)]:
					boundary.ambientDensityKGPerM3;
				const double second=y<shape.ny?gasDensityKGPerM3[shape.Index(x,y,z)]:
					boundary.ambientDensityKGPerM3;
				result.faceDensityKGPerM3.component[1][face]=0.5*first+0.5*second;
			}
			for( std::size_t z=0; z<=shape.nz; ++z ) for( std::size_t y=0;
				y<shape.ny; ++y ) for( std::size_t x=0; x<shape.nx; ++x ) {
				const std::size_t face=OpenMACFaceIndex3D(shape,2,x,y,z);
				const bool fuel=z==0 && !boundary.bottomFuelMask.empty() &&
					boundary.bottomFuelMask[y*shape.nx+x];
				const double outside=fuel?boundary.injectedGasDensityKGPerM3:
					boundary.ambientDensityKGPerM3;
				const double first=z?gasDensityKGPerM3[shape.Index(x,y,z-1)]:outside;
				const double second=z<shape.nz?gasDensityKGPerM3[shape.Index(x,y,z)]:outside;
				result.faceDensityKGPerM3.component[2][face]=0.5*first+0.5*second;
			}
			for( unsigned int axis=0; axis<3; ++axis ) for( const double density :
				result.faceDensityKGPerM3.component[axis] ) if( !std::isfinite(density) ||
				density<=0.0 ) return Fail(error,
				"fire solver final 3-D pressure-open face density overflowed");

			for( unsigned int side=0; side<6; ++side ) {
				const unsigned int axis=side/2;
				const bool positive=side%2;
				const std::size_t firstCount=side<2?shape.ny:shape.nx;
				const std::size_t secondCount=side<4?shape.nz:shape.ny;
				for( std::size_t second=0; second<secondCount; ++second ) for(
					std::size_t first=0; first<firstCount; ++first ) {
					const std::size_t index=OpenBoundaryFaceLinearIndex3D(shape,side,first,second);
					unsigned int kind=boundary.kind[side];
					if( side==4 && !boundary.bottomFuelMask.empty() &&
						boundary.bottomFuelMask[index] ) kind=FuelInletBoundary3D;
					if( kind!=PressureOpenBoundary3D ) continue;
					std::size_t x=0,y=0,z=0,cx=0,cy=0,cz=0;
					if(axis==0){x=positive?shape.nx:0;y=first;z=second;
						cx=positive?shape.nx-1:0;cy=y;cz=z;}
					if(axis==1){x=first;y=positive?shape.ny:0;z=second;
						cx=x;cy=positive?shape.ny-1:0;cz=z;}
					if(axis==2){x=first;y=second;z=positive?shape.nz:0;
						cx=x;cy=y;cz=positive?shape.nz-1:0;}
					const std::size_t face=OpenMACFaceIndex3D(shape,axis,x,y,z);
					double speed0=stage0.velocityMPerS.component[axis][face]*
						stage0.velocityMPerS.component[axis][face];
					double speed1=stage1.velocityMPerS.component[axis][face]*
						stage1.velocityMPerS.component[axis][face];
					for( unsigned int tangent=0; tangent<3; ++tangent ) if(tangent!=axis) {
						const double value0=OpenBoundaryTangentialCellVelocity3D(shape,
							stage0.velocityMPerS,tangent,cx,cy,cz);
						const double value1=OpenBoundaryTangentialCellVelocity3D(shape,
							stage1.velocityMPerS,tangent,cx,cy,cz);
						speed0+=value0*value0; speed1+=value1*value1;
					}
					if( !std::isfinite(speed0) || !std::isfinite(speed1) ) return Fail(error,
						"fire solver final 3-D stage head overflowed");
					result.boundaryDynamicPressurePa[side][index]=
						-0.25*boundary.ambientDensityKGPerM3*((stage0.inflow[side][index]?
						speed0:0.0)+(stage1.inflow[side][index]?speed1:0.0));
					if( !std::isfinite(result.boundaryDynamicPressurePa[side][index]) ) return Fail(
						error,"fire solver final 3-D integrated head overflowed");
				}
			}

			OpenPressureOperator3D pressureOperator;
			pressureOperator.rowExtra.assign(cellCount,
				std::vector<std::pair<std::size_t,double> >());
			for( unsigned int axis=0; axis<3; ++axis ) pressureOperator.faceCoefficient.
				component[axis].assign(OpenMACFaceCount3D(shape,axis),0.0);
			std::vector<double> pressure(cellCount,0.0);
			auto evaluate=[&]() {
				for( unsigned int axis=0; axis<3; ++axis ) result.velocityMPerS.component[axis].
					assign(OpenMACFaceCount3D(shape,axis),0.0);
				for( std::size_t z=0; z<shape.nz; ++z ) for( std::size_t y=0;
					y<shape.ny; ++y ) for( std::size_t x=1; x<shape.nx; ++x ) {
					const std::size_t face=OpenMACFaceIndex3D(shape,0,x,y,z);
					const double inverse=1.0/result.faceDensityKGPerM3.component[0][face];
					result.velocityMPerS.component[0][face]=unprojectedMomentumKGPerM2S.
						component[0][face]*inverse-deltaTimeS*inverse*(pressure[
						shape.Index(x,y,z)]-pressure[shape.Index(x-1,y,z)])/shape.cellWidthM;
					pressureOperator.faceCoefficient.component[0][face]=inverse;
				}
				for( std::size_t z=0; z<shape.nz; ++z ) for( std::size_t y=1;
					y<shape.ny; ++y ) for( std::size_t x=0; x<shape.nx; ++x ) {
					const std::size_t face=OpenMACFaceIndex3D(shape,1,x,y,z);
					const double inverse=1.0/result.faceDensityKGPerM3.component[1][face];
					result.velocityMPerS.component[1][face]=unprojectedMomentumKGPerM2S.
						component[1][face]*inverse-deltaTimeS*inverse*(pressure[
						shape.Index(x,y,z)]-pressure[shape.Index(x,y-1,z)])/shape.cellWidthM;
					pressureOperator.faceCoefficient.component[1][face]=inverse;
				}
				for( std::size_t z=1; z<shape.nz; ++z ) for( std::size_t y=0;
					y<shape.ny; ++y ) for( std::size_t x=0; x<shape.nx; ++x ) {
					const std::size_t face=OpenMACFaceIndex3D(shape,2,x,y,z);
					const double inverse=1.0/result.faceDensityKGPerM3.component[2][face];
					result.velocityMPerS.component[2][face]=unprojectedMomentumKGPerM2S.
						component[2][face]*inverse-deltaTimeS*inverse*(pressure[
						shape.Index(x,y,z)]-pressure[shape.Index(x,y,z-1)])/shape.cellWidthM;
					pressureOperator.faceCoefficient.component[2][face]=inverse;
				}
				for( unsigned int side=0; side<6; ++side ) {
					const unsigned int axis=side/2; const bool positive=side%2;
					const std::size_t firstCount=side<2?shape.ny:shape.nx;
					const std::size_t secondCount=side<4?shape.nz:shape.ny;
					for( std::size_t second=0; second<secondCount; ++second ) for(
						std::size_t first=0; first<firstCount; ++first ) {
						std::size_t x=0,y=0,z=0;
						if(axis==0){x=positive?shape.nx:0;y=first;z=second;}
						if(axis==1){x=first;y=positive?shape.ny:0;z=second;}
						if(axis==2){x=first;y=second;z=positive?shape.nz:0;}
						const std::size_t face=OpenMACFaceIndex3D(shape,axis,x,y,z);
						const std::size_t cx=axis==0?(positive?shape.nx-1:0):x;
						const std::size_t cy=axis==1?(positive?shape.ny-1:0):y;
						const std::size_t cz=axis==2?(positive?shape.nz-1:0):z;
						const std::size_t cell=shape.Index(cx,cy,cz);
						const std::size_t index=OpenBoundaryFaceLinearIndex3D(shape,side,first,second);
						unsigned int kind=boundary.kind[side];
						if(side==4 && !boundary.bottomFuelMask.empty() &&
							boundary.bottomFuelMask[index]) kind=FuelInletBoundary3D;
						const double sign=positive?1.0:-1.0;
						if(kind==AdiabaticWallBoundary3D){result.velocityMPerS.component[axis][face]=0.0;
							continue;}
						if(kind==FuelInletBoundary3D){result.velocityMPerS.component[axis][face]=
							-sign*OpenBoundaryFuelMassFluxKGPerM2S(boundary,side,index)/
							result.faceDensityKGPerM3.component[axis][face];continue;}
						const double density=result.faceDensityKGPerM3.component[axis][face];
						const double outwardUnprojected=sign*unprojectedMomentumKGPerM2S.
							component[axis][face]/density;
						const double k=2.0*deltaTimeS/(density*shape.cellWidthM);
						const double outward=outwardUnprojected+k*(pressure[cell]-
							result.boundaryDynamicPressurePa[side][index]);
						result.velocityMPerS.component[axis][face]=sign*outward;
						pressureOperator.faceCoefficient.component[axis][face]=2.0/density;
					}
				}
				for( unsigned int axis=0; axis<3; ++axis ) for( const double value :
					result.velocityMPerS.component[axis] ) if( !std::isfinite(value) ) return false;
				return true;
			};
			if( !evaluate() ) return Fail(error,"fire solver final 3-D velocity overflowed");
			for( std::size_t outer=0; outer<4; ++outer ) {
				std::vector<double> residual(cellCount,0.0),rightHandSide(cellCount,0.0);
				double maximum=0.0;
				for( std::size_t z=0; z<shape.nz; ++z ) for( std::size_t y=0;
					y<shape.ny; ++y ) for( std::size_t x=0; x<shape.nx; ++x ) {
					const std::size_t cell=shape.Index(x,y,z);
					residual[cell]=OpenMACDivergence3D(shape,result.velocityMPerS,x,y,z)-
						divergenceTargetPerS[cell];
					if( !std::isfinite(residual[cell]) ) return Fail(error,
						"fire solver final 3-D divergence overflowed");
					maximum=std::max(maximum,std::fabs(residual[cell]));
					rightHandSide[cell]=-residual[cell]/deltaTimeS;
				}
				if( outer==0u ) result.maximumPreProjectionDivergenceResidualPerS=maximum;
				if(maximum<=absoluteTolerancePerS){result.maximumDivergenceResidualPerS=maximum;break;}
				std::vector<double> correction,linearHistory;
				if( !SolveOpenPressureMultigrid3D(shape,pressureOperator,rightHandSide,
					absoluteTolerancePerS/deltaTimeS,correction,linearHistory,workerCount) ) return Fail(error,
					"fire solver final 3-D open multigrid did not converge");
				for( const double value : linearHistory ) result.multigridResidualHistoryPerS.
					push_back(deltaTimeS*value);
				for( std::size_t cell=0; cell<cellCount; ++cell ) pressure[cell]+=correction[cell];
				if( !evaluate() ) return Fail(error,"fire solver final 3-D velocity overflowed");
				if( outer==3 ) return Fail(error,
					"fire solver final 3-D open projection misses its divergence target");
			}
			result.stepAverageDynamicPressurePa=pressure;
			result.maximumBoundaryHeadResidualPa=0.0;
			for( unsigned int side=0; side<6; ++side ) {
				const unsigned int axis=side/2; const bool positive=side%2;
				const std::size_t firstCount=side<2?shape.ny:shape.nx;
				const std::size_t secondCount=side<4?shape.nz:shape.ny;
				for( std::size_t second=0; second<secondCount; ++second ) for(
					std::size_t first=0; first<firstCount; ++first ) {
					const std::size_t index=OpenBoundaryFaceLinearIndex3D(shape,side,first,second);
					if(boundary.kind[side]!=PressureOpenBoundary3D){result.inflow[side][index]=false;continue;}
					std::size_t x=0,y=0,z=0;
					if(axis==0){x=positive?shape.nx:0;y=first;z=second;}
					if(axis==1){x=first;y=positive?shape.ny:0;z=second;}
					if(axis==2){x=first;y=second;z=positive?shape.nz:0;}
					const double outward=(positive?1.0:-1.0)*result.velocityMPerS.component[axis][
						OpenMACFaceIndex3D(shape,axis,x,y,z)];
					result.inflow[side][index]=outward < -boundary.velocityToleranceMPerS ? true :
						(outward > boundary.velocityToleranceMPerS ? false : stage1.inflow[side][index]);
				}
			}
			for( unsigned int axis=0; axis<3; ++axis ) for( std::size_t face=0;
				face<result.velocityMPerS.component[axis].size(); ++face ) {
				result.momentumKGPerM2S.component[axis][face]=
					result.faceDensityKGPerM3.component[axis][face]*result.velocityMPerS.component[axis][face];
				if(!std::isfinite(result.momentumKGPerM2S.component[axis][face])) return Fail(error,
					"fire solver final 3-D open momentum overflowed");
			}
			PopulateOpenBoundaryTangentialVelocity3D(shape,boundary,result);
			publishedResult=result;
			return true;
		}

		struct OpenBoundaryStage3DResult
		{
			OpenMACProjection3DResult projection;
			OpenBoundaryFluxField3D boundaryFlux;
		};

		inline bool BuildRelativeBuoyancyMomentumRate3D(
			const PeriodicMACShape& shape,
			const std::vector<double>& gasDensityKGPerM3,
			const OpenBoundaryConfig3D& boundary,
			const std::array<double,3>& gravityMPerS2,
			OpenMACField3D& result,
			std::string* error = 0
			)
		{
			if( !ValidateOpenBoundaryConfig3D(shape,boundary,error) ||
				gasDensityKGPerM3.size()!=shape.CellCount() ) return Fail(error,
				"fire solver 3-D relative-buoyancy input is malformed");
			for( const double value : gravityMPerS2 ) if( !std::isfinite(value) ) return Fail(
				error,"fire solver 3-D gravity is non-finite");
			for( const double density : gasDensityKGPerM3 ) if( !std::isfinite(density) ||
				density<=0.0 ) return Fail(error,"fire solver 3-D buoyancy density is invalid");
			for( unsigned int axis=0; axis<3; ++axis ) result.component[axis].assign(
				OpenMACFaceCount3D(shape,axis),0.0);
			for( unsigned int axis=0; axis<3; ++axis ) {
				const std::size_t normalCount=axis==0?shape.nx+1:(axis==1?shape.ny+1:shape.nz+1);
				const std::size_t firstCount=axis==0?shape.ny:shape.nx;
				const std::size_t secondCount=axis==2?shape.ny:shape.nz;
				for( std::size_t second=0; second<secondCount; ++second ) for(
					std::size_t first=0; first<firstCount; ++first ) for(
					std::size_t normal=0; normal<normalCount; ++normal ) {
					std::size_t x=0,y=0,z=0;
					if(axis==0){x=normal;y=first;z=second;}
					if(axis==1){x=first;y=normal;z=second;}
					if(axis==2){x=first;y=second;z=normal;}
					const bool lowerBoundary=normal==0,upperBoundary=normal+1==normalCount;
					double lower=boundary.ambientDensityKGPerM3;
					double upper=boundary.ambientDensityKGPerM3;
					if(!lowerBoundary){const std::size_t cx=axis==0?normal-1:x;
						const std::size_t cy=axis==1?normal-1:y;
						const std::size_t cz=axis==2?normal-1:z;
						lower=gasDensityKGPerM3[shape.Index(cx,cy,cz)];}
					if(!upperBoundary){const std::size_t cx=axis==0?normal:x;
						const std::size_t cy=axis==1?normal:y;
						const std::size_t cz=axis==2?normal:z;
						upper=gasDensityKGPerM3[shape.Index(cx,cy,cz)];}
					if(axis==2 && lowerBoundary && !boundary.bottomFuelMask.empty() &&
						boundary.bottomFuelMask[y*shape.nx+x]) lower=
						boundary.injectedGasDensityKGPerM3;
					const double faceDensity=0.5*lower+0.5*upper;
					const double value=(faceDensity-boundary.ambientDensityKGPerM3)*
						gravityMPerS2[axis];
					if(!std::isfinite(value)) return Fail(error,
						"fire solver 3-D relative buoyancy overflowed");
					result.component[axis][OpenMACFaceIndex3D(shape,axis,x,y,z)]=value;
				}
			}
			return true;
		}

		inline bool BuildOpenBoundaryStage3D(
			const PeriodicMACShape& shape,
			const std::vector<ConservativeVector>& cellState,
			const std::vector<double>& temperatureK,
			const OpenMACField3D& beginningMomentumKGPerM2S,
			const OpenMACField3D& nonpressureMomentumRateKGPerM2S2,
			const std::vector<double>& divergenceTargetPerS,
			const std::vector<double>& rhoDiffusivityKGPerMS,
			const std::vector<double>& conductivityWPerMK,
			const OpenBoundaryConfig3D& boundary,
			const double ambientTemperatureK,
			const double injectedTemperatureK,
			const double deltaTimeS,
			const double absoluteTolerancePerS,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireStateProducerPrecision producerPrecision,
			OpenBoundaryStage3DResult& result,
			std::string* error = 0
			)
		{
			if( !ValidateOpenBoundaryConfig3D(shape,boundary,error) ) return false;
			const std::size_t count=shape.CellCount();
			if( cellState.size()!=count || temperatureK.size()!=count ||
				divergenceTargetPerS.size()!=count || rhoDiffusivityKGPerMS.size()!=count ||
				conductivityWPerMK.size()!=count || !std::isfinite(deltaTimeS) ||
				deltaTimeS<=0.0 ) return Fail(error,
				"fire solver 3-D open boundary-stage arrays are malformed");
			std::array<double,MethaneSpeciesCount> lowerEnthalpy,upperEnthalpy;
			if(!thermochemistry.SensibleEnthalpiesBySpeciesOrderJPerKG(
				thermochemistry.TemperatureMinK(),lowerEnthalpy.data(),lowerEnthalpy.size(),error)||
				!thermochemistry.SensibleEnthalpiesBySpeciesOrderJPerKG(
					thermochemistry.TemperatureMaxK(),upperEnthalpy.data(),upperEnthalpy.size(),error))
				return false;
			for( std::size_t cell=0; cell<count; ++cell ) {
				MethaneCellState physical=FromConservativeVector(cellState[cell],producerPrecision);
				physical.temperatureK=temperatureK[cell];
				if( !ValidateCellState(physical,error) ||
					!AcceptedStateAdmissible(cellState[cell],lowerEnthalpy,upperEnthalpy,
						thermochemistry,producerPrecision,error) ||
					!std::isfinite(rhoDiffusivityKGPerMS[cell]) ||
					rhoDiffusivityKGPerMS[cell]<0.0 ||
					!std::isfinite(conductivityWPerMK[cell]) || conductivityWPerMK[cell]<0.0 ) {
					return Fail(error,"fire solver 3-D open boundary-stage cell is invalid");
				}
			}
			OpenMACField3D unprojected;
			for( unsigned int axis=0; axis<3; ++axis ) {
				const std::size_t faceCount=OpenMACFaceCount3D(shape,axis);
				if( beginningMomentumKGPerM2S.component[axis].size()!=faceCount ||
					nonpressureMomentumRateKGPerM2S2.component[axis].size()!=faceCount ) return Fail(
					error,"fire solver 3-D open boundary-stage face shape is invalid");
				unprojected.component[axis].assign(faceCount,0.0);
				for( std::size_t face=0; face<faceCount; ++face ) {
					const double value=beginningMomentumKGPerM2S.component[axis][face]+
						deltaTimeS*nonpressureMomentumRateKGPerM2S2.component[axis][face];
					if(!std::isfinite(value)) return Fail(error,
						"fire solver 3-D open boundary-stage momentum overflowed");
					unprojected.component[axis][face]=value;
				}
			}
			OpenBoundaryStage3DResult candidate;
			std::vector<double> gasDensity(count,0.0);
			for( std::size_t cell=0; cell<count; ++cell ) for( std::size_t species=0;
				species<MethaneCarbon; ++species ) {
				gasDensity[cell]+=cellState[cell][1+species];
			}
			if( !ProjectPressureOpenMACVelocity3D(shape,gasDensity,
				unprojected,divergenceTargetPerS,boundary,deltaTimeS,absoluteTolerancePerS,
				candidate.projection,error) || !BuildOpenBoundaryFluxField3D(shape,cellState,
				temperatureK,rhoDiffusivityKGPerMS,conductivityWPerMK,boundary,
				candidate.projection,ambientTemperatureK,injectedTemperatureK,fuel,
				thermochemistry,candidate.boundaryFlux,error) ) return false;
			result=candidate;
			return true;
		}

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
					const double faceDensity = PositiveArithmeticMean(gasDensityKGPerM3[cell],
						gasDensityKGPerM3[next]);
					const double momentum = unprojectedMomentumKGPerM2S.component[axis][cell];
					if( !std::isfinite(faceDensity) || faceDensity<=0.0 || !std::isfinite(momentum) ) {
						return Fail(error,"fire solver 3-D periodic MAC momentum is non-finite");
					}
					result.faceDensityKGPerM3.component[axis][cell] = faceDensity;
					inverseFaceDensity.component[axis][cell] = 1.0/faceDensity;
					result.velocityMPerS.component[axis][cell] = momentum/faceDensity;
				}
			}
			targetMean /= static_cast<double>(count);
			if( std::fabs(targetMean) > std::max(absoluteTolerancePerS,
				128.0*std::numeric_limits<double>::epsilon()*
					std::max(1.0,targetMaximum)) ) {
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
				result.faceDensityKGPerM3[face] = PositiveArithmeticMean(
					gasDensityKGPerM3[face],gasDensityKGPerM3[right]);
				if( !std::isfinite(result.faceDensityKGPerM3[face]) ||
					result.faceDensityKGPerM3[face] <= 0.0 ) return Fail(error,
					"fire solver periodic staggered density overflowed");
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

		// Independent small-grid reference used only by the verification tier.
		// The production pressure-open path is ProjectPressureOpenMACVelocity3D.
		inline bool ReferenceProjectPressureOpenMACVelocity1D(
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

		inline bool ReferenceProjectPressureOpenMACVelocity1DFinal(
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

		inline bool PicardContinuousVerificationResidual(
			const std::vector<double>& acceptedTarget,
			const std::vector<double>& iterationTarget,
			const std::vector<double>& acceptedFaceMass,
			const std::vector<double>& iterationFaceMass,
			const std::vector<double>& acceptedDiffusivity,
			const std::vector<double>& iterationDiffusivity,
			const std::vector<double>& acceptedConductivity,
			const std::vector<double>& iterationConductivity,
			const std::vector<double>& acceptedViscosity,
			const std::vector<double>& iterationViscosity,
			const double cellWidthM,
			double& result,
			std::string* error=0)
		{
			if(acceptedTarget.size()!=iterationTarget.size()||
				acceptedFaceMass.size()!=iterationFaceMass.size()||
				acceptedDiffusivity.size()!=iterationDiffusivity.size()||
				acceptedConductivity.size()!=iterationConductivity.size()||
				acceptedViscosity.size()!=iterationViscosity.size()||
				!std::isfinite(cellWidthM)||cellWidthM<=0.0) return Fail(error,
					"fire solver Picard verification shape is invalid");
			double candidate=0.0;
			auto compare=[&](const std::vector<double>& accepted,
				const std::vector<double>& iteration,const double scale){
				for(std::size_t i=0;i<accepted.size();++i){
					if(!std::isfinite(accepted[i])||!std::isfinite(iteration[i]))return false;
					candidate=std::max(candidate,std::fabs(accepted[i]-iteration[i])*scale);
				}
				return true;
			};
			if(!compare(acceptedTarget,iterationTarget,1.0)||
				!compare(acceptedFaceMass,iterationFaceMass,1.0/cellWidthM)||
				!compare(acceptedDiffusivity,iterationDiffusivity,1.0)||
				!compare(acceptedConductivity,iterationConductivity,1.0)||
				!compare(acceptedViscosity,iterationViscosity,1.0)) return Fail(error,
					"fire solver Picard verification value is nonfinite");
			result=candidate;
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
			double maximumLimiterClassDiscrepancy=0.0;
			bool limiterDiscontinuousClass=false;
		};

		inline bool SelectLimiterPicardAcceptance(
			const std::vector<double>& acceptingIteration,
			const std::vector<double>& verification,
			const double projectionTolerance,
			std::vector<double>& accepted,
			double& maximumDiscrepancy,
			bool& discontinuousClass,
			std::string* error=0)
		{
			if(acceptingIteration.size()!=verification.size()||
				!std::isfinite(projectionTolerance)||projectionTolerance<0.0)
				return Fail(error,"fire solver limiter acceptance input is malformed");
			maximumDiscrepancy=0.0;
			for(std::size_t face=0;face<verification.size();++face){
				const double next=acceptingIteration[face],verified=verification[face];
				if(!std::isfinite(next)||!std::isfinite(verified)||next<0.0||next>1.0||
					verified<0.0||verified>1.0) return Fail(error,
						"fire solver limiter acceptance coefficient is invalid");
				maximumDiscrepancy=std::max(maximumDiscrepancy,std::fabs(next-verified));
			}
			discontinuousClass=maximumDiscrepancy>projectionTolerance;
			accepted.resize(verification.size());
			for(std::size_t face=0;face<verification.size();++face) accepted[face]=
				discontinuousClass?std::min(acceptingIteration[face],verification[face]):
					verification[face];
			return true;
		}

		inline bool BuildPeriodicStageTransport(
			const std::vector<ConservativeVector>& state,
			const std::vector<double>& temperatureK,
			const std::vector<double>& faceVelocityMPerS,
			const double cellWidthM,
			const bool dns,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			const FireStateProducerPrecision producerPrecision,
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
				MethaneCellState physical = FromConservativeVector(state[cell],producerPrecision);
				physical.temperatureK = temperatureK[cell];
				double gradient[3][3] = {};
				gradient[0][0] = (faceVelocityMPerS[cell]-
					faceVelocityMPerS[(cell+count-1)%count])/cellWidthM;
				CellTransportEvaluation evaluation;
				if( !EvaluateCellTransport(physical,gradient,widths,dns,thermochemistry,
					transport,producerPrecision,evaluation,error) ) return false;
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
			std::string* error = 0,
			const std::vector<ConservativeVector>* heunBeginning = 0,
			const PeriodicFluxPair* firstStageFlux = 0,
			const bool scalarAcceptanceStage = true
			)
		{
			std::vector<double> temperature;
			if( !InvertPeriodicTemperaturesWithinBounds(state,thermochemistry,
				config.ambientTemperatureK,config.adiabaticTemperatureK,
				config.producerPrecision,temperature,error) ) return false;
			std::vector<double> target(state.size(),0.0), priorMassFlux(state.size(),0.0);
			std::vector<double> priorDiffusivity(state.size(),0.0),
				priorConductivity(state.size(),0.0),priorViscosity(state.size(),0.0);
			if((heunBeginning==0)!=(firstStageFlux==0)||
				(heunBeginning&&heunBeginning->size()!=state.size()))return Fail(error,
					"fire solver periodic manifold candidate context is malformed");
			auto acceptedCandidate=[&](const PeriodicFluxPair& secondFlux,
				std::vector<ConservativeVector>& candidate,std::vector<double>& alpha)->bool{
				if(!heunBeginning)return ApplyPeriodicSharedFCT(state,secondFlux,frozenSourcePerS,
					config,fuel,thermochemistry,candidate,alpha,error);
				PeriodicFluxPair averaged;const std::size_t count=state.size();
				averaged.low.resize(count);averaged.high.resize(count);
				averaged.nonadvectiveMass.resize(count);averaged.nonadvectiveEnergy.resize(count);
				for(std::size_t face=0;face<count;++face){
					averaged.low[face]=0.5*(firstStageFlux->low[face]+secondFlux.low[face]);
					averaged.high[face]=0.5*(firstStageFlux->high[face]+secondFlux.high[face]);
					averaged.nonadvectiveEnergy[face]=0.5*(
						firstStageFlux->nonadvectiveEnergy[face]+secondFlux.nonadvectiveEnergy[face]);
					for(std::size_t component=0;component<MethaneMassStateDimension;++component)
						averaged.nonadvectiveMass[face][component]=0.5*(
							firstStageFlux->nonadvectiveMass[face][component]+
							secondFlux.nonadvectiveMass[face][component]);
				}
				return ApplyPeriodicSharedFCT(*heunBeginning,averaged,frozenSourcePerS,
					config,fuel,thermochemistry,candidate,alpha,error);
			};
			result.picardResidualPerS.clear();
			for( std::size_t iteration=0; iteration<64; ++iteration ) {
				PeriodicProjectionResult projection;
				if( !ProjectPeriodicMACVelocity(GasDensityFromConservative(state),
					unprojectedMomentum,target,config.cellWidthM,config.deltaTimeS,
					projectionTolerancePerS,projection,error) ) return false;
				std::vector<double> diffusivityM2PerS, conductivityWPerMK, dynamicViscosityPaS;
				if( !BuildPeriodicStageTransport(state,temperature,projection.velocityMPerS,
					config.cellWidthM,dns,thermochemistry,transport,config.producerPrecision,
					diffusivityM2PerS,
					conductivityWPerMK,dynamicViscosityPaS,error) ) return false;
				PeriodicFluxPair flux;
				if( !BuildPeriodicFluxPair(state,temperature,projection.velocityMPerS,
					diffusivityM2PerS,conductivityWPerMK,config.cellWidthM,fuel,
					thermochemistry,flux,error) ) return false;
				std::vector<double> nextAlpha;
				std::vector<ConservativeVector> stageCandidate;
				if(scalarAcceptanceStage&&!acceptedCandidate(flux,stageCandidate,nextAlpha))return false;
				std::vector<double> nextTarget;
				if(iteration==0u||!scalarAcceptanceStage){
					if( !PeriodicDivergenceTargetFromPhysicalFlux(state,temperature,flux,
						frozenSourcePerS,config.cellWidthM,config.deltaTimeS,thermochemistry,
						config.producerPrecision,nextTarget,error) ) return false;
				}else if(!ManifoldExactDivergenceTarget(target,stageCandidate,
					config.deltaTimeS,thermochemistry,config.producerPrecision,nextTarget,error,true))
					return false;
				double residual = 0.0, massFluxResidual = 0.0,
					coefficientResidual = 0.0;
				for( std::size_t cell=0; cell<state.size(); ++cell ) {
					residual = std::max(residual,std::fabs(nextTarget[cell]-target[cell]));
					const double massFlux = projection.faceDensityKGPerM3[cell]*
						projection.velocityMPerS[cell];
					if( iteration ) massFluxResidual = std::max(massFluxResidual,
						std::fabs(massFlux-priorMassFlux[cell]));
					priorMassFlux[cell] = massFlux;
					if( iteration ) coefficientResidual = std::max({coefficientResidual,
						std::fabs(diffusivityM2PerS[cell]-priorDiffusivity[cell]),
						std::fabs(conductivityWPerMK[cell]-priorConductivity[cell]),
						std::fabs(dynamicViscosityPaS[cell]-priorViscosity[cell])});
				}
				priorDiffusivity = diffusivityM2PerS;
				priorConductivity = conductivityWPerMK;
				priorViscosity = dynamicViscosityPaS;
				result.picardResidualPerS.push_back(std::max({residual,massFluxResidual/
					std::max(config.cellWidthM,1.0e-300),coefficientResidual}));
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
						coefficientResidual <= projectionTolerancePerS)) ) {
					if( iteration ) {
						PeriodicProjectionResult acceptedProjection;
						if( !ProjectPeriodicMACVelocity(GasDensityFromConservative(state),
							unprojectedMomentum,target,config.cellWidthM,config.deltaTimeS,
							projectionTolerancePerS,acceptedProjection,error) ) return false;
						std::vector<double> acceptedDiffusivity,acceptedConductivity,acceptedViscosity;
						if(!BuildPeriodicStageTransport(state,temperature,acceptedProjection.velocityMPerS,
							config.cellWidthM,dns,thermochemistry,transport,config.producerPrecision,
							acceptedDiffusivity,
							acceptedConductivity,acceptedViscosity,error)) return false;
						PeriodicFluxPair acceptedFlux;
						if(!BuildPeriodicFluxPair(state,temperature,acceptedProjection.velocityMPerS,
							acceptedDiffusivity,acceptedConductivity,config.cellWidthM,fuel,
							thermochemistry,acceptedFlux,error)) return false;
						std::vector<ConservativeVector> verifiedCandidate;
						std::vector<double> verifiedAlpha,verifiedTarget;
						if(scalarAcceptanceStage){
							if(!acceptedCandidate(acceptedFlux,verifiedCandidate,verifiedAlpha)||
								!ManifoldExactDivergenceTarget(target,verifiedCandidate,
									config.deltaTimeS,thermochemistry,config.producerPrecision,
									verifiedTarget,error,true))return false;
						}else if(!PeriodicDivergenceTargetFromPhysicalFlux(state,temperature,
							acceptedFlux,frozenSourcePerS,config.cellWidthM,config.deltaTimeS,
							thermochemistry,config.producerPrecision,verifiedTarget,error))return false;
						double verification=0.0;
						if(!PicardContinuousVerificationResidual(verifiedTarget,target,
							acceptedProjection.momentumKGPerM2S,projection.momentumKGPerM2S,
							acceptedDiffusivity,diffusivityM2PerS,acceptedConductivity,
							conductivityWPerMK,acceptedViscosity,dynamicViscosityPaS,
							config.cellWidthM,verification,error)) return false;
						if(verification>projectionTolerancePerS){target=verifiedTarget;continue;}
						if(solvePredictorLimiter){
							if(!SelectLimiterPicardAcceptance(nextAlpha,verifiedAlpha,
								projectionTolerancePerS,result.faceAlpha,
								result.maximumLimiterClassDiscrepancy,
								result.limiterDiscontinuousClass,error)) return false;
							std::vector<ConservativeVector> certifiedPredictor;
							std::vector<double> certifiedAlpha;
							if(!ApplyPeriodicSharedFCT(state,acceptedFlux,frozenSourcePerS,config,fuel,
								thermochemistry,certifiedPredictor,certifiedAlpha,error,
								&result.faceAlpha)) return false;
							std::vector<double> certifiedTarget;
							if(!ManifoldExactDivergenceTarget(target,certifiedPredictor,
								config.deltaTimeS,thermochemistry,config.producerPrecision,
								certifiedTarget,error,true))return false;
							double certifiedResidual=0.0;
							for(std::size_t cell=0;cell<target.size();++cell)certifiedResidual=std::max(
								certifiedResidual,std::fabs(certifiedTarget[cell]-target[cell]));
							if(certifiedResidual>projectionTolerancePerS){target=certifiedTarget;continue;}
						}else result.faceAlpha=verifiedAlpha;
						result.projection=acceptedProjection;
						result.flux=acceptedFlux;
						result.divergenceTargetPerS=verifiedTarget;
						result.diffusivityM2PerS=acceptedDiffusivity;
						result.conductivityWPerMK=acceptedConductivity;
						result.dynamicViscosityPaS=acceptedViscosity;
						if(!RemainingMomentumRHS(state,acceptedProjection.velocityMPerS,
							acceptedViscosity,frozenSourcePerS,config,
							result.nonpressureMomentumRHS,error)) return false;
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

		// Test-only one-dimensional reference retained for cross-checks.
		inline bool ReferenceAdvancePeriodicProjectedHeun1D(
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
				config,fuel,thermochemistry,predictor,predictorAlpha,error,
				&result.r0.faceAlpha) ) return false;
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
				result.r1,error,&beginning,&result.r0.flux,true) ) return false;
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
				fuel,thermochemistry,result.conservative,result.faceAlpha,error,
				&result.r1.faceAlpha) ) return false;
			std::vector<double> heunTemperature;
			if( !InvertPeriodicTemperaturesWithinBounds(result.conservative,thermochemistry,
				config.ambientTemperatureK,config.adiabaticTemperatureK,
				config.producerPrecision,heunTemperature,error) ||
				!PeriodicDivergenceTargetFromPhysicalFlux(
				result.conservative,heunTemperature,averaged,frozenSourcePerS,
				config.cellWidthM,config.deltaTimeS,thermochemistry,
				config.producerPrecision,result.divergenceHeunPerS,error) ) return false;
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
				result.r2,error,0,0,false) ) return false;
			result.momentumKGPerM2S = result.r2.projection.momentumKGPerM2S;
			result.velocityMPerS = result.r2.projection.velocityMPerS;
			result.stepAveragePressurePa = result.r2.projection.pressureImpulsePa;
			return true;
		}

		// The production conservative owner is kept in a separate header so the
		// pressure/open-boundary kernels above remain reviewable.  This include is
		// intentionally inside RISE::FireSim; the included file is a namespace
		// fragment and cannot be used as an independent implementation path.
#include "fire_simulator_3d_advance.h"
	}
}

#endif
