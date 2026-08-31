//////////////////////////////////////////////////////////////////////
//
// FireProductionSourceKernel.h - shared canonical methane source arithmetic
//
// License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_FIRE_PRODUCTION_SOURCE_KERNEL_
#define RISE_FIRE_PRODUCTION_SOURCE_KERNEL_

#include "FireSimulationRecords.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
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
		struct FireProfileCounters
		{
			std::atomic<unsigned long long> sliceCalls{0},sliceSerialCalls{0},sliceThreads{0};
			std::atomic<unsigned long long> spawnCycles{0},spawnThreads{0};
			std::atomic<unsigned long long> vcycles{0},coarsenCalls{0},smoothSweeps{0};
			std::atomic<unsigned long long> mgSolves{0},mgOuterIters{0},projCalls{0};
			std::atomic<unsigned long long> picardIters{0},stageCalls{0},fctCalls{0};
			std::atomic<unsigned long long> fluxPairCalls{0},transportCalls{0},invertTCalls{0};
			std::atomic<unsigned long long> nsProjection{0},nsTransport{0},nsRHS{0};
			std::atomic<unsigned long long> nsFluxPair{0},nsFCT{0},nsTarget{0};
			std::atomic<unsigned long long> nsInvertT{0},nsMGCoarsen{0};
		};
		inline bool FireProfileEnabled()
		{
			static const bool enabled=std::getenv("RISE_FIRE_PROFILE")!=nullptr;
			return enabled;
		}
		inline FireProfileCounters& FireProfile()
		{
			static FireProfileCounters counters;return counters;
		}
		inline void FireProfileIncrement(std::atomic<unsigned long long>& counter,
			const unsigned long long amount=1u)
		{
			if(FireProfileEnabled())counter.fetch_add(amount,std::memory_order_relaxed);
		}
		class FireProfileScopedNs
		{
		public:
			explicit FireProfileScopedNs(std::atomic<unsigned long long>& sink):sink_(sink),
				active_(FireProfileEnabled()),begin_(std::chrono::steady_clock::now()) {}
			~FireProfileScopedNs()
			{
				if(active_)sink_.fetch_add(static_cast<unsigned long long>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now()-begin_).count()),std::memory_order_relaxed);
			}
		private:
			std::atomic<unsigned long long>& sink_;bool active_;
			std::chrono::steady_clock::time_point begin_;
		};
		inline void FireProfileReportAndReset(const char* label)
		{
			if(!FireProfileEnabled())return;
			FireProfileCounters& c=FireProfile();
			std::fprintf(stderr,"FIREPROF %s slice_calls=%llu slice_serial=%llu slice_threads=%llu "
				"spawn_cycles=%llu spawn_threads=%llu vcycles=%llu coarsen=%llu smooth_sweeps=%llu "
				"mg_solves=%llu mg_outer=%llu proj=%llu picard=%llu stages=%llu fct=%llu "
				"fluxpair=%llu transport=%llu invertT=%llu ms_proj=%.3f ms_transport=%.3f "
				"ms_rhs=%.3f ms_fluxpair=%.3f ms_fct=%.3f ms_target=%.3f ms_invertT=%.3f "
				"ms_mg_coarsen=%.3f\n",label,
				c.sliceCalls.exchange(0),c.sliceSerialCalls.exchange(0),c.sliceThreads.exchange(0),
				c.spawnCycles.exchange(0),c.spawnThreads.exchange(0),c.vcycles.exchange(0),
				c.coarsenCalls.exchange(0),c.smoothSweeps.exchange(0),c.mgSolves.exchange(0),
				c.mgOuterIters.exchange(0),c.projCalls.exchange(0),c.picardIters.exchange(0),
				c.stageCalls.exchange(0),c.fctCalls.exchange(0),c.fluxPairCalls.exchange(0),
				c.transportCalls.exchange(0),c.invertTCalls.exchange(0),
				c.nsProjection.exchange(0)*1.0e-6,c.nsTransport.exchange(0)*1.0e-6,
				c.nsRHS.exchange(0)*1.0e-6,c.nsFluxPair.exchange(0)*1.0e-6,
				c.nsFCT.exchange(0)*1.0e-6,c.nsTarget.exchange(0)*1.0e-6,
				c.nsInvertT.exchange(0)*1.0e-6,c.nsMGCoarsen.exchange(0)*1.0e-6);
		}

		class PersistentFireWorkerPool
		{
		public:
			PersistentFireWorkerPool():generation_(0u),activeWorkers_(0u),finishedWorkers_(0u),
				stopping_(false){}
			~PersistentFireWorkerPool()
			{
				{std::lock_guard<std::mutex> lock(mutex_);stopping_=true;++generation_;}
				workAvailable_.notify_all();
				for(std::thread& worker:workers_)worker.join();
			}
			void Run(const unsigned int workerCount,const std::function<void(unsigned int)>& task)
			{
				std::lock_guard<std::mutex> submitLock(submitMutex_);
				if(workerCount>workers_.size()){
					std::uint64_t initialGeneration=0u;
					{std::lock_guard<std::mutex> lock(mutex_);initialGeneration=generation_;}
					const std::size_t previous=workers_.size();workers_.reserve(workerCount);
					struct StartupGate
					{
						std::mutex mutex;std::condition_variable condition;
						bool released=false,abort=false;
					};
					const std::shared_ptr<StartupGate> gate=std::make_shared<StartupGate>();
					std::vector<std::thread> pending;pending.reserve(workerCount-previous);
					try {
						for(std::size_t worker=previous;worker<workerCount;++worker){
							pending.emplace_back([this,worker,initialGeneration,gate](){
								{std::unique_lock<std::mutex> lock(gate->mutex);
									gate->condition.wait(lock,[&](){return gate->released;});
									if(gate->abort)return;}
								WorkerLoop(static_cast<unsigned int>(worker),initialGeneration);});
						}
					} catch(...) {
						{std::lock_guard<std::mutex> lock(gate->mutex);
							gate->abort=true;gate->released=true;}
						gate->condition.notify_all();
						for(std::thread& worker:pending)worker.join();
						throw;
					}
					for(std::thread& worker:pending)workers_.push_back(std::move(worker));
					{std::lock_guard<std::mutex> lock(gate->mutex);gate->released=true;}
					gate->condition.notify_all();
					FireProfileIncrement(FireProfile().spawnCycles);
					FireProfileIncrement(FireProfile().spawnThreads,workerCount-previous);
				}
				{std::lock_guard<std::mutex> lock(mutex_);task_=task;activeWorkers_=workerCount;
					finishedWorkers_=0u;taskException_=std::exception_ptr();++generation_;}
				workAvailable_.notify_all();
				std::unique_lock<std::mutex> lock(mutex_);
				workFinished_.wait(lock,[this](){return finishedWorkers_==activeWorkers_;});
				const std::exception_ptr taskException=taskException_;
				task_=std::function<void(unsigned int)>();activeWorkers_=0u;
				lock.unlock();if(taskException)std::rethrow_exception(taskException);
			}
		private:
			void WorkerLoop(const unsigned int workerIndex,std::uint64_t observedGeneration)
			{
				for(;;){
					std::function<void(unsigned int)> task;unsigned int active=0u;
					{std::unique_lock<std::mutex> lock(mutex_);
						workAvailable_.wait(lock,[&](){return stopping_||generation_!=observedGeneration;});
						if(stopping_)return;observedGeneration=generation_;active=activeWorkers_;
						if(workerIndex<active)task=task_;}
					if(workerIndex>=active)continue;
					try { task(workerIndex); }
					catch(...) { std::lock_guard<std::mutex> lock(mutex_);
						if(!taskException_)taskException_=std::current_exception(); }
					{std::lock_guard<std::mutex> lock(mutex_);++finishedWorkers_;
						if(finishedWorkers_==activeWorkers_)workFinished_.notify_one();}
				}
			}
			std::vector<std::thread> workers_;std::mutex mutex_,submitMutex_;
			std::condition_variable workAvailable_,workFinished_;
			std::function<void(unsigned int)> task_;
			std::exception_ptr taskException_;
			std::uint64_t generation_;unsigned int activeWorkers_,finishedWorkers_;bool stopping_;
		};
		inline PersistentFireWorkerPool& FireWorkerPool()
		{
			static PersistentFireWorkerPool pool;return pool;
		}
		inline unsigned int FireWorkerCapacity()
		{
			const unsigned int topology=std::thread::hardware_concurrency();
			return std::max(1u,topology);
		}
		template<typename SliceFunction>
		inline void ParallelFireSlices(
			const std::size_t sliceCount,
			const unsigned int workerCount,
			const SliceFunction& function )
		{
			const unsigned int workers=std::max(1u,std::min(workerCount,
				std::min(static_cast<unsigned int>(sliceCount),FireWorkerCapacity())));
			if(workers==1u){FireProfileIncrement(FireProfile().sliceSerialCalls);
				for(std::size_t slice=0;slice<sliceCount;++slice)function(slice);return;}
			FireProfileIncrement(FireProfile().sliceCalls);
			FireProfileIncrement(FireProfile().sliceThreads,workers);
			FireWorkerPool().Run(workers,[&](const unsigned int worker){
				const std::size_t first=sliceCount*worker/workers;
				const std::size_t last=sliceCount*(worker+1u)/workers;
				for(std::size_t slice=first;slice<last;++slice)function(slice);
			});
		}
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

		inline double PositiveArithmeticMean( const double first, const double second )
		{
			const double larger=std::max(first,second);
			const double smaller=std::min(first,second);
			return larger*(0.5+0.5*(smaller/larger));
		}

		struct MethaneCellState
		{
			double rhoTotalZ;
			std::array<double,MethaneSpeciesCount> constituent;
			double sensibleEnergyJPerM3;
			double temperatureK;
			FireStateProducerPrecision producerPrecision;

			MethaneCellState() : rhoTotalZ(0.0), sensibleEnergyJPerM3(0.0),
				temperatureK(0.0),producerPrecision(FireStateProducerPrecision::Binary64)
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

		inline MethaneCellState FromConservativeVector(const ConservativeVector& input,
			const FireStateProducerPrecision producerPrecision=FireStateProducerPrecision::Binary64)
		{
			MethaneCellState result;
			result.producerPrecision=producerPrecision;
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

		inline double AcceptedStateMassScale(const ConservativeVector& state)
		{
			double result=std::fabs(state[0]);
			for(std::size_t species=0;species<MethaneSpeciesCount;++species)
				result+=std::fabs(state[1+species]);
			return std::max(1.0,result);
		}

		inline double AcceptedStateEnergyScale(const ConservativeVector& state,
			const std::array<double,MethaneSpeciesCount>& lowerEnthalpy,
			const std::array<double,MethaneSpeciesCount>& upperEnthalpy)
		{
			double result=std::fabs(state[MethaneMassStateDimension]);
			for(std::size_t species=0;species<MethaneSpeciesCount;++species){
				result+=std::fabs(lowerEnthalpy[species]*state[1+species]);
				result+=std::fabs(upperEnthalpy[species]*state[1+species]);
			}
			return std::max(1.0,result);
		}

		inline bool PositivePartThermochemicalDensitiesOrdered(
			const MethaneCellState& state,
			std::array<double,MethaneSpeciesCount>& result,
			std::string* error=0 )
		{
			for(const double density:state.constituent) {
				if(!std::isfinite(density)) return Fail(error,
					"fire solver thermochemical density is non-finite");
			}
			for(std::size_t species=0;species<MethaneSpeciesCount;++species)
				result[species]=std::max(0.0,state.constituent[species]);
			return true;
		}

		inline bool SignedMixtureSensibleEnergy(const MethaneCellState& state,
			const double temperatureK,const FireSimulationMethaneRecord& thermochemistry,
			double& result,std::string* error=0)
		{
			std::array<double,MethaneSpeciesCount> enthalpy;
			if(!thermochemistry.SensibleEnthalpiesBySpeciesOrderJPerKG(temperatureK,
				enthalpy.data(),enthalpy.size(),error))return false;
			result=0.0;for(std::size_t species=0;species<MethaneSpeciesCount;++species)
				result+=state.constituent[species]*enthalpy[species];
			return std::isfinite(result)||Fail(error,
				"fire solver signed sensible-energy evaluation overflowed");
		}

		inline bool AcceptedStateAdmissible(const ConservativeVector& state,
			const std::array<double,MethaneSpeciesCount>& ambientEnthalpy,
			const std::array<double,MethaneSpeciesCount>& adiabaticEnthalpy,
			const FireSimulationMethaneRecord& fuel,
			const FireStateProducerPrecision producerPrecision,std::string* error);
		inline bool AcceptedStateAdmissible(const ConservativeVector& state,
			const std::array<double,MethaneSpeciesCount>& ambientEnthalpy,
			const std::array<double,MethaneSpeciesCount>& adiabaticEnthalpy,
			const FireSimulationMethaneRecord& fuel,std::string* error);
		inline double AcceptedStateRoundoffFactor(
			const FireAcceptedStateFeasibilityEnvelope& envelope,
			const FireStateProducerPrecision producerPrecision)
		{
			if(producerPrecision==FireStateProducerPrecision::Binary64)
				return envelope.kappaEpsilon64*std::numeric_limits<double>::epsilon();
			if(producerPrecision==FireStateProducerPrecision::Binary32)
				return envelope.kappaEpsilon32*std::numeric_limits<float>::epsilon();
			return 0.0;
		}
		inline bool AcceptedMethaneCellStateAdmissible(const MethaneCellState& state,
			const FireSimulationMethaneRecord& fuel,
			const FireStateProducerPrecision producerPrecision,std::string* error=0);
		inline double MixtureCertifiedCpLowerJPerM3K(const MethaneCellState& state,
			const double lowerK,const double upperK,
			const FireSimulationMethaneRecord& thermochemistry);

		inline bool InvertMethaneTemperatureWithinAcceptedEnvelope(
			const MethaneCellState& state,
			const double lowerTemperatureK,
			const double upperTemperatureK,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireStateProducerPrecision producerPrecision,
			double& result,
			std::string* error=0 )
		{
			if(producerPrecision!=state.producerPrecision)return Fail(error,
				"fire solver temperature inversion precision metadata mismatch");
			if(!std::isfinite(lowerTemperatureK)||!std::isfinite(upperTemperatureK)||
				lowerTemperatureK>=upperTemperatureK) return Fail(error,
					"fire solver thermochemical inversion bounds are invalid");
			std::array<double,MethaneSpeciesCount> lowerEnthalpy,upperEnthalpy;
			if(!thermochemistry.SensibleEnthalpiesBySpeciesOrderJPerKG(lowerTemperatureK,
				lowerEnthalpy.data(),lowerEnthalpy.size(),error)||
				!thermochemistry.SensibleEnthalpiesBySpeciesOrderJPerKG(upperTemperatureK,
					upperEnthalpy.data(),upperEnthalpy.size(),error)||
				!AcceptedStateAdmissible(ToConservativeVector(state),lowerEnthalpy,
					upperEnthalpy,thermochemistry,producerPrecision,error))return false;
			double lowerEnergy=0.0,upperEnergy=0.0;
			if(!SignedMixtureSensibleEnergy(state,lowerTemperatureK,thermochemistry,
				lowerEnergy,error)||!SignedMixtureSensibleEnergy(state,upperTemperatureK,
				thermochemistry,upperEnergy,error))return false;
			if(!(MixtureCertifiedCpLowerJPerM3K(state,lowerTemperatureK,
				upperTemperatureK,thermochemistry)>0.0))return Fail(error,
				"fire solver signed inversion lacks a positive certified heat capacity");
			const FireAcceptedStateFeasibilityEnvelope& envelope=
				thermochemistry.AcceptedStateFeasibilityEnvelope();
			const double tolerance=AcceptedStateRoundoffFactor(envelope,producerPrecision)*
				AcceptedStateEnergyScale(ToConservativeVector(state),lowerEnthalpy,upperEnthalpy);
			if(!(tolerance>0.0)||!std::isfinite(tolerance))return Fail(error,
				"fire solver temperature inversion producer precision is invalid");
			if(state.sensibleEnergyJPerM3<=lowerEnergy+tolerance){result=lowerTemperatureK;return true;}
			if(state.sensibleEnergyJPerM3>=upperEnergy-tolerance){result=upperTemperatureK;return true;}
			double lower=lowerTemperatureK,upper=upperTemperatureK;
			for(unsigned int iteration=0;iteration<96;++iteration){
				const double midpoint=0.5*(lower+upper);double midpointEnergy=0.0;
				if(midpoint==lower||midpoint==upper){result=midpoint;return true;}
				if(!SignedMixtureSensibleEnergy(state,midpoint,thermochemistry,
					midpointEnergy,error))return false;
				if(midpointEnergy<state.sensibleEnergyJPerM3)lower=midpoint;else upper=midpoint;
			}
			result=0.5*(lower+upper);return std::isfinite(result)||Fail(error,
				"fire solver signed temperature inversion is non-finite");
		}

		inline bool InvertMethaneTemperatureWithinAcceptedEnvelope(
			const MethaneCellState& state,const double lowerTemperatureK,
			const double upperTemperatureK,
			const FireSimulationMethaneRecord& thermochemistry,double& result,
			std::string* error=0)
		{
			return InvertMethaneTemperatureWithinAcceptedEnvelope(state,lowerTemperatureK,
				upperTemperatureK,thermochemistry,state.producerPrecision,
				result,error);
		}

		inline bool ValidateCellState(
			const MethaneCellState& state,
			std::string* error = 0
			)
		{
			if( !std::isfinite(state.rhoTotalZ) || !std::isfinite(state.sensibleEnergyJPerM3) ||
				!std::isfinite(state.temperatureK) || state.temperatureK <= 0.0 ) {
				return Fail(error,"fire solver cell contains a non-finite or negative primary field");
			}
			for(const double density:state.constituent) {
				if(!std::isfinite(density))return Fail(error,
					"fire solver cell contains a non-finite constituent density");
			}
			const double total = state.TotalDensity();
			return (std::isfinite(total) && total > 0.0) ||
				Fail(error,"fire solver cell has no finite positive total density");
		}

		inline bool EquationOfStateResidual(
			const MethaneCellState& state,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireStateProducerPrecision producerPrecision,
			double& result,
			std::string* error = 0
			)
		{
			if(producerPrecision!=state.producerPrecision)return Fail(error,
				"fire solver EOS precision metadata mismatch");
			static const char* names[MethaneCarbon] = {
				"CH4", "O2", "N2", "CO2", "H2O", "CO"
			};
			if( !thermochemistry.IsValid() || !std::isfinite(state.temperatureK) ||
				state.temperatureK <= 0.0 || !std::isfinite(state.rhoTotalZ) ) return Fail(error,
				"fire solver EOS state is invalid");
			std::array<double,MethaneSpeciesCount> lowerEnthalpy,upperEnthalpy;
			if(!thermochemistry.SensibleEnthalpiesBySpeciesOrderJPerKG(
				thermochemistry.TemperatureMinK(),lowerEnthalpy.data(),lowerEnthalpy.size(),error)||
				!thermochemistry.SensibleEnthalpiesBySpeciesOrderJPerKG(
					thermochemistry.TemperatureMaxK(),upperEnthalpy.data(),upperEnthalpy.size(),error)||
				!AcceptedStateAdmissible(ToConservativeVector(state),lowerEnthalpy,
					upperEnthalpy,thermochemistry,producerPrecision,error))return false;
			double gasDensity = 0.0;
			double molarDensityKMolPerM3 = 0.0;
			std::array<double,MethaneSpeciesCount> propertyDensities;
			if(!PositivePartThermochemicalDensitiesOrdered(state,propertyDensities,error))return false;
			for( std::size_t species=0; species<MethaneCarbon; ++species ) {
				const FireThermochemistrySpecies* property =
					thermochemistry.FindSpecies(names[species]);
				if( !property ) return Fail(error,"fire solver EOS lacks a gas species");
				gasDensity += propertyDensities[species];
				molarDensityKMolPerM3 += propertyDensities[species]/
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

		inline bool EquationOfStateResidual(const MethaneCellState& state,
			const FireSimulationMethaneRecord& thermochemistry,double& result,
			std::string* error=0)
		{
			return EquationOfStateResidual(state,thermochemistry,
				state.producerPrecision,result,error);
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

		struct CellMolecularTransportEvaluation
		{
			double gasDensityKGPerM3;
			double gasCpJPerKGK;
			double molecularViscosityPaS;
			double molecularConductivityWPerMK;
			CellMolecularTransportEvaluation():gasDensityKGPerM3(0.0),gasCpJPerKGK(0.0),
				molecularViscosityPaS(0.0),molecularConductivityWPerMK(0.0){}
		};

		inline bool EvaluateCellMolecularTransport(
			const MethaneCellState& state,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			const FireStateProducerPrecision producerPrecision,
			CellMolecularTransportEvaluation& result,
			std::string* error = 0
			)
		{
			if(producerPrecision!=state.producerPrecision)return Fail(error,
				"fire solver molecular transport precision metadata mismatch");
			if(!ValidateCellState(state,error)||
				!AcceptedMethaneCellStateAdmissible(state,thermochemistry,producerPrecision,error)||
				!thermochemistry.IsValid()||!transport.IsValid())return false;
			std::array<double,MethaneSpeciesCount> propertyDensities;
			if(!PositivePartThermochemicalDensitiesOrdered(state,propertyDensities,error))return false;
			result=CellMolecularTransportEvaluation();
			for(std::size_t species=0;species<MethaneCarbon;++species)
				result.gasDensityKGPerM3+=propertyDensities[species];
			if(result.gasDensityKGPerM3<=0.0)return Fail(error,
				"fire solver transport has no gas mass");
			std::array<double,MethaneCarbon> massFractions;
			std::array<double,MethaneSpeciesCount> speciesCp;
			if(!thermochemistry.CpBySpeciesOrderJPerKGK(state.temperatureK,speciesCp.data(),
				speciesCp.size(),error))return false;
			for(std::size_t species=0;species<MethaneCarbon;++species){
				massFractions[species]=propertyDensities[species]/result.gasDensityKGPerM3;
				result.gasCpJPerKGK+=massFractions[species]*speciesCp[species];}
			return transport.MixturePropertiesBySpeciesOrder(massFractions.data(),
				massFractions.size(),thermochemistry,state.temperatureK,
				result.molecularViscosityPaS,result.molecularConductivityWPerMK,error);
		}

		inline bool EvaluateCellMolecularTransport(const MethaneCellState& state,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			CellMolecularTransportEvaluation& result,std::string* error=0)
		{
			return EvaluateCellMolecularTransport(state,thermochemistry,transport,
				state.producerPrecision,result,error);
		}

		inline bool EvaluateCellTransportFromMolecular(
			const CellMolecularTransportEvaluation& molecular,
			const double velocityGradientPerS[3][3],
			const double directionalWidthsM[3],
			const bool dns,
			const FireSimulationTransportRecord& transport,
			CellTransportEvaluation& result,
			std::string* error = 0
			)
		{
			result=CellTransportEvaluation();
			result.gasCpJPerKGK=molecular.gasCpJPerKGK;
			result.molecularViscosityPaS=molecular.molecularViscosityPaS;
			result.molecularConductivityWPerMK=molecular.molecularConductivityWPerMK;
			if(dns)result.eddyViscosityM2PerS=0.0;
			else if(!transport.VremanEddyViscosityM2PerS(velocityGradientPerS,
				directionalWidthsM,result.eddyViscosityM2PerS,error))return false;
			return transport.EffectiveTransport(result.molecularViscosityPaS,
				result.molecularConductivityWPerMK,molecular.gasDensityKGPerM3,
				result.gasCpJPerKGK,result.eddyViscosityM2PerS,dns,
				result.molecularDiffusivityM2PerS,result.sgsDiffusivityM2PerS,
				result.totalDiffusivityM2PerS,result.effectiveViscosityPaS,
				result.effectiveConductivityWPerMK,error);
		}

		inline bool EvaluateCellTransport(
			const MethaneCellState& state,
			const double velocityGradientPerS[3][3],
			const double directionalWidthsM[3],
			const bool dns,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			const FireStateProducerPrecision producerPrecision,
			CellTransportEvaluation& result,
			std::string* error = 0
			)
		{
			CellMolecularTransportEvaluation molecular;
			return EvaluateCellMolecularTransport(state,thermochemistry,transport,
				producerPrecision,molecular,error)&&
				EvaluateCellTransportFromMolecular(molecular,velocityGradientPerS,directionalWidthsM,
					dns,transport,result,error);
		}

		inline bool EvaluateCellTransport(const MethaneCellState& state,
			const double velocityGradientPerS[3][3],const double directionalWidthsM[3],
			const bool dns,const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationTransportRecord& transport,
			CellTransportEvaluation& result,std::string* error=0)
		{
			return EvaluateCellTransport(state,velocityGradientPerS,directionalWidthsM,dns,
				thermochemistry,transport,state.producerPrecision,result,error);
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
			const double previousStepS,
			StableTimeStep& result,
			std::string* error = 0
			)
		{
			if( !std::isfinite(cellWidthM) || cellWidthM <= 0.0 ||
				!std::isfinite(maximumVelocityMPerS) || maximumVelocityMPerS < 0.0 ||
				!std::isfinite(maximumPositiveReducedGravityMPerS2) ||
				maximumPositiveReducedGravityMPerS2 < 0.0 ||
				!std::isfinite(maximumKinematicTransportM2PerS) ||
				maximumKinematicTransportM2PerS < 0.0 || dimensions == 0 || dimensions > 3 ||
				(!std::isfinite(previousStepS) && previousStepS != 0.0) || previousStepS < 0.0 ) {
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
				accept(0.5*cellWidthM/maximumVelocityMPerS,"advective_CFL");
			}
			if( maximumPositiveReducedGravityMPerS2 > 0.0 ) {
				accept(0.5*std::sqrt(2.0*cellWidthM/maximumPositiveReducedGravityMPerS2),
					"buoyant_acceleration");
			}
			if( maximumKinematicTransportM2PerS > 0.0 ) {
				accept(cellWidthM*cellWidthM/(8.0*maximumKinematicTransportM2PerS),
					"explicit_diffusion");
			}
			if( previousStepS > 0.0 ) accept(1.1*previousStepS,"growth_limit");
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
			double pilotEnergyDeltaJPerM3;
			double pilotExpansionIntegral;
			double radiativeCoolingWPerM3;

			MethaneSourcePacket() : sensibleEnergyDeltaJPerM3(0.0),
				reactedFuelKGPerM3(0.0), oxidizedCarbonKGPerM3(0.0),
				grossCarbonFormedKGPerM3(0.0),
				gasHeatReleaseWPerM3(0.0), sootHeatReleaseWPerM3(0.0),
				pilotEnergyDeltaJPerM3(0.0),
				pilotExpansionIntegral(0.0),
				radiativeCoolingWPerM3(0.0)
			{
				constituentDelta.fill(0.0);
			}
		};

		inline bool CertifiedPositiveZeroSourceValue(const double value)
		{
			return value==0.0&&!std::signbit(value);
		}

		inline bool CertifiedBinary32ZeroSource(const ConservativeVector& source)
		{
			for(std::size_t component=0;component<MethaneConservativeDimension;++component)
				if(!CertifiedPositiveZeroSourceValue(source[component]))return false;
			return true;
		}

	inline bool CertifiedBinary32ZeroSourcePacket(const MethaneSourcePacket& packet)
		{
			for(const double value:packet.constituentDelta)
				if(!CertifiedPositiveZeroSourceValue(value))return false;
			return CertifiedPositiveZeroSourceValue(packet.sensibleEnergyDeltaJPerM3)&&
				CertifiedPositiveZeroSourceValue(packet.reactedFuelKGPerM3)&&
				CertifiedPositiveZeroSourceValue(packet.oxidizedCarbonKGPerM3)&&
				CertifiedPositiveZeroSourceValue(packet.grossCarbonFormedKGPerM3)&&
				CertifiedPositiveZeroSourceValue(packet.gasHeatReleaseWPerM3)&&
				CertifiedPositiveZeroSourceValue(packet.sootHeatReleaseWPerM3)&&
				CertifiedPositiveZeroSourceValue(packet.pilotEnergyDeltaJPerM3)&&
				CertifiedPositiveZeroSourceValue(packet.pilotExpansionIntegral)&&
			CertifiedPositiveZeroSourceValue(packet.radiativeCoolingWPerM3);
	}

	inline bool CertifiedBinary32SourceDelta(const ConservativeVector& source,
		const FireSimulationMethaneRecord& fuel)
	{
		std::array<double,MethaneMassStateDimension> represented={{}};
		if(!CertifiedPositiveZeroSourceValue(source[0])||!std::isfinite(
			source[MethaneMassStateDimension])||static_cast<double>(static_cast<float>(
			source[MethaneMassStateDimension]))!=source[MethaneMassStateDimension]||
			(source[MethaneMassStateDimension]==0.0&&
			!CertifiedPositiveZeroSourceValue(source[MethaneMassStateDimension])))return false;
		double massResidual=0.0,massScale=0.0;
		for(std::size_t species=0;species<MethaneSpeciesCount;++species){
			const double value=source[1u+species];
			if(!std::isfinite(value)||static_cast<double>(static_cast<float>(value))!=value||
				(value==0.0&&!CertifiedPositiveZeroSourceValue(value)))
				return false;
			represented[1u+species]=value;massResidual+=value;massScale+=std::fabs(value);
		}
		const double factor=fuel.AcceptedStateFeasibilityEnvelope().
			sourcePacketFactorEpsilon32*std::numeric_limits<float>::epsilon();
		if(!(factor>0.0)||std::fabs(massResidual)>factor*std::max(1.0,massScale))return false;
		const FireCertifiedNullspace& closure=fuel.ConservativeReconstruction();
		if(closure.stateDimension!=MethaneMassStateDimension||
			closure.constraintMatrix.size()!=closure.constraintRows*closure.stateDimension)
			return false;
		for(std::size_t row=0;row<closure.constraintRows;++row){
			double residual=0.0,scale=0.0;
			for(std::size_t column=0;column<closure.stateDimension;++column){
				const double term=closure.constraintMatrix[row*closure.stateDimension+column]*
					represented[column];
				residual+=term;scale+=std::fabs(term);
			}
			if(!std::isfinite(residual)||std::fabs(residual)>factor*std::max(1.0,scale))
				return false;
		}
		return true;
	}

	inline bool CertifiedBinary32SourcePacket(const MethaneSourcePacket& packet,
		const FireSimulationMethaneRecord& fuel)
	{
		ConservativeVector represented{};
		for(std::size_t species=0;species<MethaneSpeciesCount;++species){
			const double value=packet.constituentDelta[species];
			represented[1u+species]=value;
		}
		represented[MethaneMassStateDimension]=packet.sensibleEnergyDeltaJPerM3;
		// The production source-map authority covers the bytes consumed by the
		// conservative update.  Reaction/radiation diagnostics are certified by
		// the frozen-packet ledger, but are not resident source inputs.
		return CertifiedBinary32SourceDelta(represented,fuel);
	}

	inline void RepresentMethaneSourcePacketBinary32(MethaneSourcePacket& packet)
	{
		auto represented=[](const double value){
			const double result=static_cast<double>(static_cast<float>(value));
			return result==0.0?std::fabs(result):result;
		};
		for(double& delta:packet.constituentDelta)delta=represented(delta);
		packet.sensibleEnergyDeltaJPerM3=represented(packet.sensibleEnergyDeltaJPerM3);
	}

		struct MethaneReactionStep
		{
			double deltaTimeS;
			double mixingTimeS;
			double maximumAcceptedTemperatureK;
			double pilotSetpointTemperatureK;
			double pilotExpansionVolumeRatioCap;
			bool primaryEligible;
			bool sootOxidationEnabled;

			MethaneReactionStep() : deltaTimeS(0.0), mixingTimeS(0.0),
				maximumAcceptedTemperatureK(0.0),
				pilotSetpointTemperatureK(0.0),
				pilotExpansionVolumeRatioCap(0.0),
				primaryEligible(false),
				sootOxidationEnabled(false) {}
		};

		struct MethanePilotProjectionMap
		{
			double targetTemperatureK=0.0;
			double volumeRatio=1.0;
			double sensibleEnergyDeltaJPerM3=0.0;
			double expansionIntegral=0.0;
		};

		inline bool ComputeMethanePilotProjectionMap(
			const MethaneCellState& beginning,
			const FireSimulationMethaneRecord& thermochemistry,
			const double setpointTemperatureK,
			const double expansionVolumeRatioCap,
			MethanePilotProjectionMap& result,
			std::string* error = 0
			)
		{
			result=MethanePilotProjectionMap();
			result.targetTemperatureK=beginning.temperatureK;
			if(!std::isfinite(setpointTemperatureK)||setpointTemperatureK<0.0||
				!std::isfinite(expansionVolumeRatioCap)||expansionVolumeRatioCap<0.0)
				return Fail(error,"fire solver pilot setpoint is invalid");
			if(setpointTemperatureK==0.0||beginning.temperatureK>=setpointTemperatureK)
				return true;
			if(expansionVolumeRatioCap!=17.0/16.0)
				return Fail(error,"fire solver pilot expansion cap is not canonical");
			if(setpointTemperatureK<thermochemistry.TemperatureMinK()||
				setpointTemperatureK>thermochemistry.TemperatureMaxK())
				return Fail(error,"fire solver pilot setpoint is outside thermochemistry");
			double cappedTemperatureK=std::min(setpointTemperatureK,
				beginning.temperatureK*expansionVolumeRatioCap);
			while(cappedTemperatureK/beginning.temperatureK>expansionVolumeRatioCap)
				cappedTemperatureK=std::nextafter(cappedTemperatureK,beginning.temperatureK);
			double beginningEnergyJPerM3=0.0,setpointEnergyJPerM3=0.0;
			if(!SignedMixtureSensibleEnergy(beginning,beginning.temperatureK,
				thermochemistry,beginningEnergyJPerM3,error)||
				!SignedMixtureSensibleEnergy(beginning,cappedTemperatureK,
					thermochemistry,setpointEnergyJPerM3,error))return false;
			result.targetTemperatureK=cappedTemperatureK;
			result.volumeRatio=cappedTemperatureK/beginning.temperatureK;
			result.sensibleEnergyDeltaJPerM3=(setpointEnergyJPerM3-
				beginningEnergyJPerM3)/result.volumeRatio;
			result.expansionIntegral=1.0-1.0/result.volumeRatio;
			return (std::isfinite(result.sensibleEnergyDeltaJPerM3)&&
				result.sensibleEnergyDeltaJPerM3>=0.0&&
				std::isfinite(result.expansionIntegral)&&result.expansionIntegral>=0.0&&
				result.expansionIntegral<=0.5)||
				Fail(error,"fire solver pilot energy increment is invalid");
		}

		inline bool ComputeMethanePilotEnergyDeltaJPerM3(
			const MethaneCellState& beginning,
			const FireSimulationMethaneRecord& thermochemistry,
			const double setpointTemperatureK,
			const double expansionVolumeRatioCap,
			double& result,
			std::string* error = 0
			)
		{
			MethanePilotProjectionMap map;
			if(!ComputeMethanePilotProjectionMap(beginning,thermochemistry,
				setpointTemperatureK,expansionVolumeRatioCap,map,error))return false;
			result=map.sensibleEnergyDeltaJPerM3;return true;
		}

#if defined(_MSC_VER)
		__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
		__attribute__((noinline))
#endif
	inline bool CanonicalApplySourcePacket(
			const MethaneCellState& beginning,
			const MethaneSourcePacket& packet,
			const FireSimulationMethaneRecord& thermochemistry,
			MethaneCellState& result,
			std::string* error = 0
			)
		{
			std::array<double,MethaneSpeciesCount> lowerEnthalpy,upperEnthalpy;
			if(!thermochemistry.SensibleEnthalpiesBySpeciesOrderJPerKG(
				thermochemistry.TemperatureMinK(),lowerEnthalpy.data(),lowerEnthalpy.size(),error)||
				!thermochemistry.SensibleEnthalpiesBySpeciesOrderJPerKG(
					thermochemistry.TemperatureMaxK(),upperEnthalpy.data(),upperEnthalpy.size(),error)||
				!AcceptedStateAdmissible(ToConservativeVector(beginning),lowerEnthalpy,
					upperEnthalpy,thermochemistry,beginning.producerPrecision,error))return false;
		const bool binary32=beginning.producerPrecision==FireStateProducerPrecision::Binary32;
		if(binary32&&!CertifiedBinary32SourcePacket(packet,thermochemistry)){
			std::ostringstream message;
			message<<"fire solver binary32 source packet lacks its precision-class certificate: dU="
				<<packet.sensibleEnergyDeltaJPerM3<<" drho=";
			for(const double value:packet.constituentDelta)message<<value<<',';
			return Fail(error,message.str());
		}
		bool identity=binary32?static_cast<float>(packet.sensibleEnergyDeltaJPerM3)==0.0f:
			packet.sensibleEnergyDeltaJPerM3==0.0;
		for(const double delta:packet.constituentDelta)identity=identity&&
			(binary32?static_cast<float>(delta)==0.0f:delta==0.0);
			if(identity) {
				if(!ValidateCellState(beginning,error))return false;
				result=beginning;
				return true;
			}
		MethaneCellState candidate=beginning;
		for(std::size_t index=0;index<MethaneSpeciesCount;++index){
			if(binary32)candidate.constituent[index]=static_cast<double>(
				static_cast<float>(beginning.constituent[index])+
				static_cast<float>(packet.constituentDelta[index]));
			else candidate.constituent[index]+=packet.constituentDelta[index];
		}
		if(binary32)candidate.sensibleEnergyJPerM3=static_cast<double>(
			static_cast<float>(beginning.sensibleEnergyJPerM3)+
			static_cast<float>(packet.sensibleEnergyDeltaJPerM3));
		else candidate.sensibleEnergyJPerM3+=packet.sensibleEnergyDeltaJPerM3;
			if(!InvertMethaneTemperatureWithinAcceptedEnvelope(candidate,
				thermochemistry.TemperatureMinK(),thermochemistry.TemperatureMaxK(),thermochemistry,
				candidate.temperatureK,error)||!ValidateCellState(candidate,error))return false;
			result=candidate;
			return true;
		}

		inline bool BuildMethaneReactionPacket(
			const MethaneCellState& beginning,
			const FireSimulationMethaneRecord& fuel,
			const MethaneReactionStep& step,
			MethaneSourcePacket& packet,
			std::string* error = 0,
			MethaneCellState* acceptedEndpoint = 0
			)
		{
			packet = MethaneSourcePacket();
			if( !fuel.IsValid() || !ValidateCellState(beginning,error) ||
				!std::isfinite(step.deltaTimeS) || step.deltaTimeS <= 0.0 ||
				!std::isfinite(step.mixingTimeS) || step.mixingTimeS <= 0.0 ||
				!std::isfinite(step.maximumAcceptedTemperatureK) ||
				step.maximumAcceptedTemperatureK < 0.0 ||
				!std::isfinite(step.pilotSetpointTemperatureK) ||
				step.pilotSetpointTemperatureK < 0.0 ||
				!std::isfinite(step.pilotExpansionVolumeRatioCap) ||
				step.pilotExpansionVolumeRatioCap < 0.0 ) {
				return Fail(error,"fire solver reaction step is outside its physical domain");
			}
			const double maximumAcceptedTemperatureK=step.maximumAcceptedTemperatureK>0.0?
				step.maximumAcceptedTemperatureK:fuel.TemperatureMaxK();
			if(maximumAcceptedTemperatureK<fuel.TemperatureMinK()||
				maximumAcceptedTemperatureK>fuel.TemperatureMaxK())
				return Fail(error,"fire solver reaction energy ceiling is outside thermochemistry");
			const double strictMaximumAcceptedTemperatureK=std::nextafter(
				maximumAcceptedTemperatureK,-std::numeric_limits<double>::infinity());
			std::array<double,MethaneSpeciesCount> lowerEnthalpy,ceilingEnthalpy;
			if(!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(fuel.TemperatureMinK(),
				lowerEnthalpy.data(),lowerEnthalpy.size(),error)||
				!fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(strictMaximumAcceptedTemperatureK,
					ceilingEnthalpy.data(),ceilingEnthalpy.size(),error)||
				!AcceptedStateAdmissible(ToConservativeVector(beginning),lowerEnthalpy,
					ceilingEnthalpy,fuel,beginning.producerPrecision,error))return false;
			const double relaxation = -std::expm1(-step.deltaTimeS/step.mixingTimeS);
			const double oxygen = std::max(0.0,beginning.constituent[MethaneO2]);
			const double primaryCandidate = step.primaryEligible ? relaxation*std::min(
				std::max(0.0,beginning.constituent[MethaneCH4]),
				oxygen/fuel.StoichiometricOxygenKGPerKGFuel()) : 0.0;
			const bool oxidizes = step.sootOxidationEnabled &&
				beginning.temperatureK > fuel.SootOxidationTemperatureK();
			const double sootCandidate = oxidizes ? relaxation*std::min(
				std::max(0.0,beginning.constituent[MethaneCarbon]),
				oxygen/fuel.SootOxygenKGPerKGCarbon()) : 0.0;
			const double oxygenDemand = fuel.StoichiometricOxygenKGPerKGFuel()*primaryCandidate+
				fuel.SootOxygenKGPerKGCarbon()*sootCandidate;
			const double theta = oxygenDemand > 0.0 ? std::min(1.0,oxygen/oxygenDemand) : 1.0;
			const double fullReacted = theta*primaryCandidate;
			const double fullOxidized = theta*sootCandidate;
			MethanePilotProjectionMap pilotMap;
			if(!ComputeMethanePilotProjectionMap(beginning,fuel,
				step.pilotSetpointTemperatureK,step.pilotExpansionVolumeRatioCap,
				pilotMap,error))return false;
			const std::vector<double>& primary = fuel.PrimaryReactionDelta();
			if( primary.size() != MethaneSpeciesCount ) {
				return Fail(error,"fire solver methane reaction vector has the wrong dimension");
			}
			auto packetAtExtent=[&](const double extent,MethaneSourcePacket& candidate){
				candidate=MethaneSourcePacket();
				const double reacted=extent==1.0?fullReacted:extent*fullReacted;
				const double oxidized=extent==1.0?fullOxidized:extent*fullOxidized;
				candidate.reactedFuelKGPerM3=reacted;
				candidate.oxidizedCarbonKGPerM3=oxidized;
				candidate.grossCarbonFormedKGPerM3=reacted*fuel.SootYieldKGPerKGFuel();
				for(std::size_t index=0;index<MethaneSpeciesCount;++index)
					candidate.constituentDelta[index]=reacted*primary[index];
				candidate.constituentDelta[MethaneCarbon]+=
					candidate.grossCarbonFormedKGPerM3-oxidized;
				candidate.constituentDelta[MethaneO2]-=
					fuel.SootOxygenKGPerKGCarbon()*oxidized;
				candidate.constituentDelta[MethaneCO2]+=
					fuel.SootCO2KGPerKGCarbon()*oxidized;
				candidate.sensibleEnergyDeltaJPerM3=
					reacted*fuel.LowerHeatingValueJPerKG()+
					oxidized*fuel.SootHeatReleaseJPerKGCarbon()+
					pilotMap.sensibleEnergyDeltaJPerM3;
				candidate.gasHeatReleaseWPerM3=
					reacted*fuel.LowerHeatingValueJPerKG()/step.deltaTimeS;
				candidate.sootHeatReleaseWPerM3=
					oxidized*fuel.SootHeatReleaseJPerKGCarbon()/step.deltaTimeS;
				candidate.pilotEnergyDeltaJPerM3=pilotMap.sensibleEnergyDeltaJPerM3;
				candidate.pilotExpansionIntegral=pilotMap.expansionIntegral;
				if(beginning.producerPrecision==FireStateProducerPrecision::Binary32)
					RepresentMethaneSourcePacketBinary32(candidate);
			};
			std::string strictCandidateError;
			auto strictCandidate=[&](const MethaneSourcePacket& candidate,
				MethaneCellState& endpoint){
				strictCandidateError.clear();
				return CanonicalApplySourcePacket(beginning,candidate,fuel,endpoint,
					&strictCandidateError)&&endpoint.temperatureK<maximumAcceptedTemperatureK;
			};
			MethaneSourcePacket fullPacket;
			MethaneCellState fullEndpoint;
			packetAtExtent(1.0,fullPacket);
			MethaneCellState selectedEndpoint;
			if(strictCandidate(fullPacket,fullEndpoint)) {
				packet=fullPacket;
				selectedEndpoint=fullEndpoint;
			}
			else {
				MethaneSourcePacket lowerPacket;
				MethaneCellState lowerEndpoint;
				packetAtExtent(0.0,lowerPacket);
				if(!strictCandidate(lowerPacket,lowerEndpoint))return Fail(error,
					"fire solver combined pilot packet has no strict source headroom: "+
					strictCandidateError);
				static_assert(sizeof(double)==sizeof(std::uint64_t),
					"fire solver reaction extent requires binary64 storage");
				static_assert(std::numeric_limits<double>::is_iec559,
					"fire solver reaction extent requires IEEE-754 binary64 ordering");
				auto extentBits=[](const double value){
					std::uint64_t bits=0;
					std::memcpy(&bits,&value,sizeof(bits));
					return bits;
				};
				auto extentFromBits=[](const std::uint64_t bits){
					double value=0.0;
					std::memcpy(&value,&bits,sizeof(value));
					return value;
				};
				std::uint64_t lowerBits=extentBits(0.0),upperBits=extentBits(1.0);
				while(upperBits-lowerBits>1u) {
					const std::uint64_t midpointBits=lowerBits+(upperBits-lowerBits)/2u;
					MethaneSourcePacket midpointPacket;
					MethaneCellState midpointEndpoint;
					packetAtExtent(extentFromBits(midpointBits),midpointPacket);
					if(strictCandidate(midpointPacket,midpointEndpoint)) {
						lowerBits=midpointBits;
						lowerPacket=midpointPacket;
						lowerEndpoint=midpointEndpoint;
					} else upperBits=midpointBits;
				}
				packet=lowerPacket;
				selectedEndpoint=lowerEndpoint;
			}
			for( std::size_t index=0; index<MethaneSpeciesCount; ++index ) {
				if( !std::isfinite(packet.constituentDelta[index]) ) {
					return Fail(error,"fire solver reaction packet contains a non-finite constituent delta");
				}
			}
			const bool finitePacket=std::isfinite(packet.sensibleEnergyDeltaJPerM3) &&
				std::isfinite(packet.reactedFuelKGPerM3) &&
				std::isfinite(packet.oxidizedCarbonKGPerM3) &&
				std::isfinite(packet.grossCarbonFormedKGPerM3) &&
				std::isfinite(packet.gasHeatReleaseWPerM3) &&
				std::isfinite(packet.sootHeatReleaseWPerM3) &&
				std::isfinite(packet.pilotEnergyDeltaJPerM3) &&
				std::isfinite(packet.pilotExpansionIntegral);
			if(!finitePacket)return Fail(error,"fire solver reaction packet overflowed");
			if(acceptedEndpoint)*acceptedEndpoint=selectedEndpoint;
			return true;
		}

		inline bool ApplySourcePacket(
			const MethaneCellState& beginning,
			const MethaneSourcePacket& packet,
			const FireSimulationMethaneRecord& thermochemistry,
			MethaneCellState& result,
			std::string* error = 0
			)
		{
			return CanonicalApplySourcePacket(beginning,packet,thermochemistry,result,error);
		}

		inline bool AcceptedStateAdmissible(
			const ConservativeVector& state,
			const std::array<double,MethaneSpeciesCount>& ambientEnthalpy,
			const std::array<double,MethaneSpeciesCount>& adiabaticEnthalpy,
			const FireSimulationMethaneRecord& fuel,
			const FireStateProducerPrecision producerPrecision,
			std::string* error=0 )
		{
			return fuel.AcceptedConservativeStateAdmissibleByComponentOrder(
				state.value.data(),state.value.size(),ambientEnthalpy.data(),adiabaticEnthalpy.data(),
				ambientEnthalpy.size(),producerPrecision,error);
		}

		inline bool AcceptedMethaneCellStateAdmissible(
			const MethaneCellState& state,
			const FireSimulationMethaneRecord& fuel,
			const FireStateProducerPrecision producerPrecision,
			std::string* error )
		{
			if(producerPrecision!=state.producerPrecision)return Fail(error,
				"fire solver accepted-state precision metadata mismatch");
			std::array<double,MethaneSpeciesCount> lowerEnthalpy,upperEnthalpy;
			return fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(fuel.TemperatureMinK(),
				lowerEnthalpy.data(),lowerEnthalpy.size(),error)&&
				fuel.SensibleEnthalpiesBySpeciesOrderJPerKG(fuel.TemperatureMaxK(),
					upperEnthalpy.data(),upperEnthalpy.size(),error)&&
				AcceptedStateAdmissible(ToConservativeVector(state),lowerEnthalpy,
					upperEnthalpy,fuel,producerPrecision,error);
		}

		inline bool AcceptedStateAdmissible(const ConservativeVector& state,
			const std::array<double,MethaneSpeciesCount>& ambientEnthalpy,
			const std::array<double,MethaneSpeciesCount>& adiabaticEnthalpy,
			const FireSimulationMethaneRecord& fuel,std::string* error)
		{
			return AcceptedStateAdmissible(state,ambientEnthalpy,adiabaticEnthalpy,fuel,
				FireStateProducerPrecision::Binary64,error);
		}

		inline bool AcceptedMethaneCellStateAdmissible(const MethaneCellState& state,
			const FireSimulationMethaneRecord& fuel,std::string* error=0)
		{
			return AcceptedMethaneCellStateAdmissible(state,fuel,
				state.producerPrecision,error);
		}
		inline bool DivergenceFromDiscreteRate(
			const ConservativeVector& stateVector,
			const ConservativeVector& nonadvectiveAndSourceRate,
			const double temperatureK,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireStateProducerPrecision producerPrecision,
			double& result,
			std::string* error = 0
			)
		{
			static const char* names[MethaneSpeciesCount] = {
				"CH4", "O2", "N2", "CO2", "H2O", "CO", "C(gr)"
			};
			const MethaneCellState state = FromConservativeVector(stateVector,producerPrecision);
			std::array<double,MethaneSpeciesCount> lowerEnthalpy,upperEnthalpy;
			if(!thermochemistry.SensibleEnthalpiesBySpeciesOrderJPerKG(
				thermochemistry.TemperatureMinK(),lowerEnthalpy.data(),lowerEnthalpy.size(),error)||
				!thermochemistry.SensibleEnthalpiesBySpeciesOrderJPerKG(
					thermochemistry.TemperatureMaxK(),upperEnthalpy.data(),upperEnthalpy.size(),error)||
				!AcceptedStateAdmissible(stateVector,lowerEnthalpy,upperEnthalpy,
					thermochemistry,producerPrecision,error))return false;
			std::array<double,MethaneSpeciesCount> propertyDensities;
			if(!PositivePartThermochemicalDensitiesOrdered(state,propertyDensities,error))return false;
			double gasDensity = 0.0, inverseMeanWeightSum = 0.0;
			for( std::size_t species=0; species<MethaneCarbon; ++species ) {
				const FireThermochemistrySpecies* property = thermochemistry.FindSpecies(names[species]);
				if( !property ) return Fail(error,"fire solver divergence identity lacks a gas species");
				const double density = propertyDensities[species];
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
				heatCapacity += propertyDensities[species]*cp;
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

		inline bool DivergenceFromDiscreteRate(const ConservativeVector& stateVector,
			const ConservativeVector& nonadvectiveAndSourceRate,const double temperatureK,
			const FireSimulationMethaneRecord& thermochemistry,double& result,
			std::string* error=0)
		{
			return DivergenceFromDiscreteRate(stateVector,nonadvectiveAndSourceRate,
				temperatureK,thermochemistry,FireStateProducerPrecision::Binary64,result,error);
		}

		inline bool AcceptedConservativeVolumeRatio(
			const ConservativeVector& stateVector,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireStateProducerPrecision producerPrecision,
			double& ratio,
			std::string* error = 0
			)
		{
			static const char* names[MethaneCarbon]={"CH4","O2","N2","CO2","H2O","CO"};
			MethaneCellState state=FromConservativeVector(stateVector,producerPrecision);
			double temperatureK=0.0;
			if(!InvertMethaneTemperatureWithinAcceptedEnvelope(state,
				thermochemistry.TemperatureMinK(),thermochemistry.TemperatureMaxK(),
				thermochemistry,temperatureK,error))return false;
			std::array<double,MethaneSpeciesCount> propertyDensities;
			if(!PositivePartThermochemicalDensitiesOrdered(state,propertyDensities,error))return false;
			double molarDensity=0.0;
			for(std::size_t species=0;species<MethaneCarbon;++species){
				const FireThermochemistrySpecies* property=thermochemistry.FindSpecies(names[species]);
				if(!property)return Fail(error,"fire solver finite-increment divergence lacks a gas species");
				molarDensity+=propertyDensities[species]/property->molecularWeightKGPerKMol;
			}
			ratio=molarDensity*8314.46261815324*temperatureK/
				thermochemistry.ThermodynamicPressurePa();
			return (std::isfinite(ratio)&&ratio>0.0)||Fail(error,
				"fire solver finite-increment volume ratio is invalid");
		}

		inline bool AcceptedConservativeVolumeRatio(const ConservativeVector& stateVector,
			const FireSimulationMethaneRecord& thermochemistry,double& ratio,
			std::string* error=0)
		{
			return AcceptedConservativeVolumeRatio(stateVector,thermochemistry,
				FireStateProducerPrecision::Binary64,ratio,error);
		}

		inline bool DivergenceFromDiscreteIncrement(
			const ConservativeVector& stateVector,
			const ConservativeVector& nonadvectiveAndSourceIncrement,
			const double temperatureK,
			const double deltaTimeS,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireStateProducerPrecision producerPrecision,
			double& result,
			std::string* error = 0
			)
		{
			if(!std::isfinite(deltaTimeS)||deltaTimeS<=0.0||
				!std::isfinite(temperatureK)||temperatureK<=0.0)
				return Fail(error,"fire solver finite-increment divergence input is invalid");
			for(std::size_t component=0;component<MethaneConservativeDimension;++component){
				const double value=nonadvectiveAndSourceIncrement[component];
				if(!std::isfinite(value))return Fail(error,
					"fire solver finite-increment divergence input is non-finite");
			}
			ConservativeVector candidateVector=stateVector+nonadvectiveAndSourceIncrement;
			double candidateVolume=0.0;
			if(!AcceptedConservativeVolumeRatio(candidateVector,thermochemistry,producerPrecision,
				candidateVolume,error))return false;
			// The constrained cell represents one fixed Eulerian volume.  Refer the
			// finite update to that p0 manifold, rather than preserving a prior
			// accepted-state residual; the next coupled advection then removes its
			// own fp/splitting residual instead of accumulating it step by step.
			result=(candidateVolume-1.0)/deltaTimeS;
			return std::isfinite(result)||Fail(error,
				"fire solver finite-increment divergence overflowed");
		}

		inline bool DivergenceFromDiscreteIncrement(const ConservativeVector& stateVector,
			const ConservativeVector& nonadvectiveAndSourceIncrement,const double temperatureK,
			const double deltaTimeS,const FireSimulationMethaneRecord& thermochemistry,
			double& result,std::string* error=0)
		{
			return DivergenceFromDiscreteIncrement(stateVector,nonadvectiveAndSourceIncrement,
				temperatureK,deltaTimeS,thermochemistry,FireStateProducerPrecision::Binary64,
				result,error);
		}

		inline bool ManifoldExactDivergenceTarget(
			const std::vector<double>& currentTargetPerS,
			const std::vector<ConservativeVector>& acceptedCandidate,
			const double deltaTimeS,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireStateProducerPrecision producerPrecision,
			std::vector<double>& result,
			std::string* error = 0,
			const bool enforcePeriodicCompatibility = false
			)
		{
			FireProfileScopedNs profileTimer(FireProfile().nsTarget);
			if(currentTargetPerS.size()!=acceptedCandidate.size()||
				!std::isfinite(deltaTimeS)||deltaTimeS<=0.0)return Fail(error,
					"fire solver manifold-exact target input is malformed");
			result.resize(acceptedCandidate.size());
			for(std::size_t cell=0;cell<acceptedCandidate.size();++cell){
				double volumeRatio=0.0;
				if(!AcceptedConservativeVolumeRatio(acceptedCandidate[cell],thermochemistry,
					producerPrecision,
					volumeRatio,error))return false;
				result[cell]=currentTargetPerS[cell]+(volumeRatio-1.0)/deltaTimeS;
				if(!std::isfinite(result[cell]))return Fail(error,
					"fire solver manifold-exact target overflowed");
			}
			if(enforcePeriodicCompatibility&&!result.empty()){
				double mean=0.0;
				for(const double value:result)mean+=value;
				mean/=static_cast<double>(result.size());
				for(double& value:result)value-=mean;
			}
			return true;
		}

		inline bool ManifoldExactDivergenceTarget(
			const std::vector<double>& currentTargetPerS,
			const std::vector<ConservativeVector>& acceptedCandidate,const double deltaTimeS,
			const FireSimulationMethaneRecord& thermochemistry,std::vector<double>& result,
			std::string* error=0,const bool enforcePeriodicCompatibility=false)
		{
			return ManifoldExactDivergenceTarget(currentTargetPerS,acceptedCandidate,deltaTimeS,
				thermochemistry,FireStateProducerPrecision::Binary64,result,error,
				enforcePeriodicCompatibility);
		}

		inline bool FrozenSourcePacketExpansionAdmissible(
			const ConservativeVector& beginning,
			const double beginningTemperatureK,
			const MethaneSourcePacket& packet,
			const double deltaTimeS,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireStateProducerPrecision producerPrecision,
			double* scaledDivergence = 0,
			std::string* error = 0
			)
		{
			ConservativeVector increment;
			for(std::size_t species=0;species<MethaneSpeciesCount;++species)
				increment[1+species]=packet.constituentDelta[species];
			increment[MethaneMassStateDimension]=packet.sensibleEnergyDeltaJPerM3-
				packet.pilotEnergyDeltaJPerM3;
			double divergencePerS=0.0;
			if(!DivergenceFromDiscreteIncrement(beginning,increment,beginningTemperatureK,
				deltaTimeS,thermochemistry,producerPrecision,divergencePerS,error))return false;
			const double scaled=deltaTimeS*divergencePerS+packet.pilotExpansionIntegral;
			if(scaledDivergence)*scaledDivergence=scaled;
			if(!std::isfinite(scaled)||scaled>0.5){
				std::ostringstream message;
				message<<"fire solver frozen source packet exceeds the expansion bound: dt*S_div="
					<<scaled<<" limit=0.5";
				return Fail(error,message.str());
			}
			return true;
		}

		inline bool FrozenSourcePacketExpansionAdmissible(const ConservativeVector& beginning,
			const double beginningTemperatureK,const MethaneSourcePacket& packet,
			const double deltaTimeS,const FireSimulationMethaneRecord& thermochemistry,
			double* scaledDivergence=0,std::string* error=0)
		{
			return FrozenSourcePacketExpansionAdmissible(beginning,beginningTemperatureK,packet,
				deltaTimeS,thermochemistry,FireStateProducerPrecision::Binary64,
				scaledDivergence,error);
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
			const double extent = std::min(std::max(0.0,beginning.constituent[MethaneCH4]),
				std::max(0.0,beginning.constituent[MethaneO2])/
					fuel.StoichiometricOxygenKGPerKGFuel());
			MethaneCellState trial = beginning;
			const std::vector<double>& delta = fuel.PrimaryReactionDelta();
			for( std::size_t index=0; index<MethaneSpeciesCount; ++index ) {
				trial.constituent[index] += extent*delta[index];
			}
			trial.sensibleEnergyJPerM3 += extent*fuel.LowerHeatingValueJPerKG();
			return InvertMethaneTemperatureWithinAcceptedEnvelope(trial,
				thermochemistry.TemperatureMinK(),thermochemistry.TemperatureMaxK(),
				thermochemistry,result,error) &&
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
				if( !ValidateCellState(state,error) ||
					!AcceptedMethaneCellStateAdmissible(state,thermochemistry,error) ) return false;
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
				if(!std::isfinite(result.beta))return Fail(error,
					"fire solver radiation escape factor overflowed");
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
			std::array<double,MethaneSpeciesCount> propertyDensities;
			if(!PositivePartThermochemicalDensitiesOrdered(state,propertyDensities,error))return false;
			const char* ids[2] = {"CO2", "H2O"};
			const std::size_t indices[2] = {MethaneCO2,MethaneH2O};
			for( std::size_t speciesIndex=0; speciesIndex<2; ++speciesIndex ) {
				// The cold ambient occupies almost the entire plume lattice and
				// contains neither modeled emitter.  Its contribution is exactly
				// zero, so do not perform four table interpolations per empty cell.
				if( propertyDensities[indices[speciesIndex]] == 0.0 ) continue;
				const FireThermochemistrySpecies* species = thermochemistry.FindSpecies(ids[speciesIndex]);
				if( !species ) return Fail(error,"fire solver gas opacity species lacks thermochemistry");
				const double moleculesPerM3 = propertyDensities[indices[speciesIndex]]/
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
				double speciesUpper = 0.0;
				for( const FireThermochemistrySegment& segment : species->segments ) {
					if( segment.temperatureMaxK >= lowerK && segment.temperatureMinK <= upperK ) {
						speciesLower = std::min(speciesLower,segment.certifiedCpLowerJPerKGK);
						const double boundedLower=std::max(lowerK,segment.temperatureMinK);
						const double boundedUpper=std::min(upperK,segment.temperatureMaxK);
						double absolutePolynomial=
							std::fabs(segment.coefficients[0])/(boundedLower*boundedLower)+
							std::fabs(segment.coefficients[1])/boundedLower+
							std::fabs(segment.coefficients[2]);
						double power=boundedUpper;
						for(std::size_t coefficient=3;coefficient<7;++coefficient){
							absolutePolynomial+=std::fabs(segment.coefficients[coefficient])*power;
							power*=boundedUpper;
						}
						speciesUpper=std::max(speciesUpper,absolutePolynomial*8314.46261815324/
							species->molecularWeightKGPerKMol);
					}
				}
				if( !std::isfinite(speciesLower)||!std::isfinite(speciesUpper) ) return -1.0;
				result += state.constituent[index]>=0.0?
					state.constituent[index]*speciesLower:
					state.constituent[index]*speciesUpper;
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

		inline double IntervalProductUpper(
			const double firstMinimum,const double firstMaximum,
			const double secondMinimum,const double secondMaximum)
		{
			return std::max(std::max(firstMinimum*secondMinimum,
				firstMinimum*secondMaximum),std::max(firstMaximum*secondMinimum,
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
			const double upper3 = upper2*upperK;
			const double lower4 = lower2*lower2;
			const double upper4 = upper2*upper2;
			const double ambient2 = ambientTemperatureK*ambientTemperatureK;
			const double ambient4 = ambient2*ambient2;
			std::array<double,MethaneSpeciesCount> propertyDensities;
			if(!PositivePartThermochemicalDensitiesOrdered(state,propertyDensities,error))return false;
			for( std::size_t speciesIndex=0; speciesIndex<2; ++speciesIndex ) {
				const FireThermochemistrySpecies* species = thermochemistry.FindSpecies(ids[speciesIndex]);
				if( !species ) return Fail(error,"fire solver gas opacity species lacks thermochemistry");
				const double moleculesPerM3 = propertyDensities[indices[speciesIndex]]/
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
				const double hotMaximum = std::max(0.0,hot+hotLipschitz*radius);
				const double hotTermLower = IntervalProductLower(hotDerivativeMinimum,
					hotDerivativeMaximum,lower4,upper4)+4.0*hotMinimum*lower3;
				const double hotTermUpper = IntervalProductUpper(hotDerivativeMinimum,
					hotDerivativeMaximum,lower4,upper4)+4.0*hotMaximum*upper3;
				const double innerLower=hotTermLower-ambientGasMaximum*ambient4;
				const double innerUpper=hotTermUpper-ambientGasMinimum*ambient4;
				const double speciesLower = 4.0*sigmaSB*(moleculesPerM3>=0.0?
					moleculesPerM3*innerLower:moleculesPerM3*innerUpper);
				if( !std::isfinite(speciesLower) ) {
					return Fail(error,"fire solver gas-exchange derivative enclosure overflowed");
				}
				result += speciesLower;
			}
			return std::isfinite(result) ||
				Fail(error,"fire solver gas-exchange derivative sum overflowed");
		}

		template<class EnergyFunction,class ExchangeFunction,class DerivativeLowerFunction>
		inline bool CertifiedScalarRadiationBackwardEuler(
			const double initialTemperatureK,
			const double ambientTemperatureK,
			const double initialEnergyJPerM3,
			const double heatCapacityLowerJPerM3K,
			const double deltaTimeS,
			const double escapeFactor,
			std::vector<double> knots,
			const EnergyFunction& energy,
			const ExchangeFunction& exchange,
			const DerivativeLowerFunction& derivativeLower,
			double& acceptedTemperatureK,
			double& acceptedCoolingWPerM3,
			std::string* error = 0
			)
		{
			if(!std::isfinite(initialTemperatureK)||!std::isfinite(ambientTemperatureK)||
				!std::isfinite(initialEnergyJPerM3)||!std::isfinite(heatCapacityLowerJPerM3K)||
				heatCapacityLowerJPerM3K<=0.0||!std::isfinite(deltaTimeS)||deltaTimeS<=0.0||
				!std::isfinite(escapeFactor)||escapeFactor<0.0){
				return Fail(error,"fire solver certified scalar radiation input is malformed");
			}
			const double lower=std::min(initialTemperatureK,ambientTemperatureK);
			const double upper=std::max(initialTemperatureK,ambientTemperatureK);
			knots.push_back(lower);knots.push_back(upper);
			std::sort(knots.begin(),knots.end());
			knots.erase(std::remove_if(knots.begin(),knots.end(),[&](const double value){
				return !std::isfinite(value)||value<lower||value>upper;
			}),knots.end());
			knots.erase(std::unique(knots.begin(),knots.end()),knots.end());
			if(knots.size()<2)return Fail(error,"fire solver certified scalar radiation lacks a bracket");
			for(std::size_t interval=0;interval+1<knots.size();++interval){
				double bound=0.0;
				if(!derivativeLower(knots[interval],knots[interval+1],bound)||
					!std::isfinite(bound)||heatCapacityLowerJPerM3K+
					deltaTimeS*escapeFactor*bound<=0.0){
					return Fail(error,"fire solver radiation F-prime enclosure is not strictly positive");
				}
			}
			auto residual=[&](const double temperature,double& value){
				double sensible=0.0,radiative=0.0;
				if(!energy(temperature,sensible)||!exchange(temperature,radiative))return false;
				// At T=T* the sensible difference is the exact algebraic zero.
				// Re-evaluating and subtracting two referenced enthalpies can flip the
				// endpoint sign in the near-T_ref limit and destroy a valid bracket.
				const double sensibleDelta=temperature==initialTemperatureK ? 0.0 :
					sensible-initialEnergyJPerM3;
				value=sensibleDelta+deltaTimeS*escapeFactor*radiative;
				return std::isfinite(value);
			};
			double fLower=0.0,fUpper=0.0;
			if(!residual(lower,fLower)||!residual(upper,fUpper))return false;
			const double endpointScale=std::max({1.0,std::fabs(initialEnergyJPerM3),
				heatCapacityLowerJPerM3K*std::max(std::fabs(lower),std::fabs(upper))});
			const double endpointEnvelope=64.0*std::numeric_limits<double>::epsilon()*endpointScale;
			if(std::fabs(fLower)<=endpointEnvelope)fLower=0.0;
			if(std::fabs(fUpper)<=endpointEnvelope)fUpper=0.0;
			if(fLower>0.0||fUpper<0.0){
				std::ostringstream message;
				message << "fire solver radiation map lacks its certified endpoint sign change: lower="
					<< lower << " upper=" << upper << " f_lower=" << fLower << " f_upper=" << fUpper
					<< " initial_T=" << initialTemperatureK << " initial_E=" << initialEnergyJPerM3;
				return Fail(error,message.str());
			}
			double lo=lower,hi=upper;
			for(std::size_t iteration=0;iteration<160;++iteration){
				const double midpoint=0.5*(lo+hi);double value=0.0;
				if(!residual(midpoint,value))return false;
				if(value>0.0)hi=midpoint;else lo=midpoint;
				if(hi-lo<=8.0*std::numeric_limits<double>::epsilon()*
					std::max(1.0,midpoint))break;
			}
			const double midpoint=0.5*(lo+hi);
			double fLo=0.0,fHi=0.0,fMid=0.0;
			if(!residual(lo,fLo)||!residual(hi,fHi)||!residual(midpoint,fMid))return false;
			acceptedTemperatureK=midpoint;double finalResidual=fMid;
			if(std::fabs(fLo)<std::fabs(finalResidual)) {
				acceptedTemperatureK=lo;finalResidual=fLo;
			}
			if(std::fabs(fHi)<std::fabs(finalResidual)) {
				acceptedTemperatureK=hi;finalResidual=fHi;
			}
			double finalEnergy=0.0;
			if(!energy(acceptedTemperatureK,finalEnergy))return false;
			// Sensible enthalpy is referenced to T_ref and can therefore be near
			// zero even though each polynomial term being subtracted is O(C_T*T).
			// Use that operation's natural scale for the fp64 inversion residual;
			// scaling only by the referenced result makes a representable root
			// impossible in the cold, near-T_ref limit.
			const double energyScale=std::max({1.0,std::fabs(initialEnergyJPerM3),
				std::fabs(finalEnergy),heatCapacityLowerJPerM3K*
					std::max(std::fabs(initialTemperatureK),std::fabs(ambientTemperatureK))});
			if(std::fabs(finalResidual)>64.0*std::numeric_limits<double>::epsilon()*energyScale){
				std::ostringstream message;
				message << "fire solver radiation root misses its energy residual tolerance: residual="
					<< finalResidual << ", scale=" << energyScale << ", T="
					<< acceptedTemperatureK << ", bracket=" << lo << "," << hi;
				return Fail(error,message.str());
			}
			acceptedCoolingWPerM3=(initialEnergyJPerM3-finalEnergy)/deltaTimeS;
			return (std::isfinite(acceptedTemperatureK)&&std::isfinite(acceptedCoolingWPerM3))||
				Fail(error,"fire solver certified scalar radiation result overflowed");
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
				ambientTemperatureK < opacity.TemperatureMinK() ||
				ambientTemperatureK > opacity.TemperatureMaxK() ) {
				return Fail(error,"fire solver radiation map is outside its certified domain");
			}
			double radiationTemperatureK=0.0;
			if(!InvertMethaneTemperatureWithinAcceptedEnvelope(preRadiation,
				opacity.TemperatureMinK(),opacity.TemperatureMaxK(),thermochemistry,
				radiationTemperatureK,error))return false;
			if( radiationTemperatureK == ambientTemperatureK || escapeFactor == 0.0 ) {
				result = preRadiation; acceptedCoolingWPerM3 = 0.0; return true;
			}
			const double lower = std::min(ambientTemperatureK,radiationTemperatureK);
			const double upper = std::max(ambientTemperatureK,radiationTemperatureK);
			const double cpLower = MixtureCertifiedCpLowerJPerM3K(
				preRadiation,lower,upper,thermochemistry);
			if( cpLower <= 0.0 ) return Fail(error,"fire solver radiation map lacks a positive C_T bound");
			double ambientEnergy=0.0;
			if(!SignedMixtureSensibleEnergy(preRadiation,ambientTemperatureK,
				thermochemistry,ambientEnergy,error))return false;
			const double representableEnergyEnvelope=64.0*std::numeric_limits<double>::epsilon()*
				std::max({1.0,std::fabs(ambientEnergy),std::fabs(preRadiation.sensibleEnergyJPerM3),
					cpLower*std::max(std::fabs(ambientTemperatureK),
						std::fabs(radiationTemperatureK))});
			if(std::fabs(preRadiation.sensibleEnergyJPerM3-ambientEnergy)<=
				representableEnergyEnvelope) {
				// The conservative energy is retained.  Only the derived temperature
				// separation is below one fp64 thermochemical resolution element, so
				// no nonzero radiative exchange can be certified yet.
				result=preRadiation;acceptedCoolingWPerM3=0.0;return true;
			}
			if(preRadiation.sensibleEnergyJPerM3>=ambientEnergy&&
				radiationTemperatureK<=ambientTemperatureK) {
				// Conservative energy is the authoritative source state.  A derived
				// temperature rounded to the ambient endpoint cannot justify a
				// cooling sign; defer the sink until inversion resolves T*>T_inf.
				result=preRadiation;acceptedCoolingWPerM3=0.0;return true;
			}
			const FireGasOpacitySpecies* co2 = opacity.FindSpecies("CO2");
			const FireGasOpacitySpecies* h2o = opacity.FindSpecies("H2O");
			if( !co2 || !h2o ) return Fail(error,"fire solver radiation record is incomplete");
			std::vector<double> knots = {lower,upper};
			for( const double value : co2->gasTemperatureAxisK ) if( value > lower && value < upper ) knots.push_back(value);
			for( const double value : co2->radiationTemperatureAxisK ) if( value > lower && value < upper ) knots.push_back(value);
			for( const double value : h2o->gasTemperatureAxisK ) if( value > lower && value < upper ) knots.push_back(value);
			for( const double value : h2o->radiationTemperatureAxisK ) if( value > lower && value < upper ) knots.push_back(value);
			auto energy=[&](const double temperature,double& value){
				return SignedMixtureSensibleEnergy(preRadiation,temperature,
					thermochemistry,value,error);
			};
			auto exchange=[&](const double temperature,double& value){
				GasExchangeEvaluation evaluation;
				if(!EvaluateGasExchange(preRadiation,temperature,ambientTemperatureK,
					thermochemistry,opacity,evaluation,error))return false;
				value=evaluation.exchangeWPerM3;return true;
			};
			auto derivative=[&](const double lo,const double hi,double& value){
				return CertifiedGasExchangeDerivativeLower(preRadiation,lo,hi,
					ambientTemperatureK,thermochemistry,opacity,value,error);
			};
			MethaneCellState candidate = preRadiation;
			if(!CertifiedScalarRadiationBackwardEuler(radiationTemperatureK,
				ambientTemperatureK,preRadiation.sensibleEnergyJPerM3,cpLower,deltaTimeS,
				escapeFactor,knots,energy,exchange,derivative,candidate.temperatureK,
				acceptedCoolingWPerM3,error))return false;
			if(!energy(candidate.temperatureK,candidate.sensibleEnergyJPerM3))return false;
			result = candidate;
			return true;
		}

	inline bool ValidateFrozenMethaneSourcePacketLedger(
			const MethaneCellState& beginning,
			const MethaneReactionStep& reactionStep,
			const FireSimulationMethaneRecord& fuel,
			const MethaneSourcePacket& packet,
			std::string* error=0
			)
		{
			if(!ValidateCellState(beginning,error))return false;
			double massResidual=0.0,massScale=0.0,elementRatio=0.0;
			for(std::size_t species=0;species<MethaneSpeciesCount;++species){
				const double delta=packet.constituentDelta[species];massResidual+=delta;
				massScale+=std::fabs(beginning.constituent[species])+std::fabs(
					beginning.constituent[species]+delta);}
			const std::vector<double>& element=fuel.ElementMassFractionMatrix();
			for(std::size_t row=0;row<fuel.ElementOrder().size();++row){double residual=0.0,
				scale=0.0;for(std::size_t species=0;species<MethaneSpeciesCount;++species){
					const double coefficient=element[row*MethaneSpeciesCount+species];
					const double term=coefficient*packet.constituentDelta[species];residual+=term;
					scale+=std::fabs(coefficient*beginning.constituent[species])+std::fabs(
						coefficient*(beginning.constituent[species]+packet.constituentDelta[species]));}
				elementRatio=std::max(elementRatio,std::fabs(residual)/std::max(1.0,scale));}
		const FireAcceptedStateFeasibilityEnvelope& envelope=
			fuel.AcceptedStateFeasibilityEnvelope();
		const double factor=beginning.producerPrecision==FireStateProducerPrecision::Binary32?
			envelope.sourcePacketFactorEpsilon32*std::numeric_limits<float>::epsilon():
			envelope.sourcePacketFactorEpsilon64*std::numeric_limits<double>::epsilon();
			const double massRatio=std::fabs(massResidual)/std::max(1.0,massScale);
			MethanePilotProjectionMap expectedPilotMap;
			if(!ComputeMethanePilotProjectionMap(beginning,fuel,
				reactionStep.pilotSetpointTemperatureK,
				reactionStep.pilotExpansionVolumeRatioCap,expectedPilotMap,error))return false;
			const double expectedPilotEnergyJPerM3=
				expectedPilotMap.sensibleEnergyDeltaJPerM3;
			const double expectedEnergy=packet.reactedFuelKGPerM3*
				fuel.LowerHeatingValueJPerKG()+packet.oxidizedCarbonKGPerM3*
				fuel.SootHeatReleaseJPerKGCarbon()+expectedPilotEnergyJPerM3-
				reactionStep.deltaTimeS*
				packet.radiativeCoolingWPerM3;
			const double oxygenExpected=fuel.StoichiometricOxygenKGPerKGFuel()*
				packet.reactedFuelKGPerM3+fuel.SootOxygenKGPerKGCarbon()*
				packet.oxidizedCarbonKGPerM3;
			const double energyScale=std::max(1.0,
				std::fabs(beginning.sensibleEnergyJPerM3)+
				std::fabs(beginning.sensibleEnergyJPerM3+packet.sensibleEnergyDeltaJPerM3)+
				std::fabs(packet.reactedFuelKGPerM3*fuel.LowerHeatingValueJPerKG())+
				std::fabs(packet.oxidizedCarbonKGPerM3*fuel.SootHeatReleaseJPerKGCarbon())+
				std::fabs(expectedPilotEnergyJPerM3)+
				std::fabs(reactionStep.deltaTimeS*packet.radiativeCoolingWPerM3));
			const double energyRatio=std::fabs(packet.sensibleEnergyDeltaJPerM3-
				expectedEnergy)/energyScale;
			const double oxygenScale=std::max(1.0,
				std::fabs(beginning.constituent[MethaneO2])+std::fabs(
					beginning.constituent[MethaneO2]+packet.constituentDelta[MethaneO2])+
				std::fabs(oxygenExpected));
			const double oxygenRatio=std::fabs(-packet.constituentDelta[MethaneO2]-
				oxygenExpected)/oxygenScale;
			const double expectedGasRate=packet.reactedFuelKGPerM3*
				fuel.LowerHeatingValueJPerKG()/reactionStep.deltaTimeS;
			const double expectedSootRate=packet.oxidizedCarbonKGPerM3*
				fuel.SootHeatReleaseJPerKGCarbon()/reactionStep.deltaTimeS;
			const double gasRateRatio=std::fabs(packet.gasHeatReleaseWPerM3-expectedGasRate)/
				std::max(1.0,std::fabs(packet.gasHeatReleaseWPerM3)+std::fabs(expectedGasRate));
			const double sootRateRatio=std::fabs(packet.sootHeatReleaseWPerM3-expectedSootRate)/
				std::max(1.0,std::fabs(packet.sootHeatReleaseWPerM3)+std::fabs(expectedSootRate));
			const bool pilotEnergyExact=packet.pilotEnergyDeltaJPerM3==
				expectedPilotEnergyJPerM3;
			const bool pilotExpansionExact=packet.pilotExpansionIntegral==
				expectedPilotMap.expansionIntegral;
			const bool closes=massRatio<=factor&&elementRatio<=factor&&energyRatio<=factor&&
				oxygenRatio<=factor&&
				std::isfinite(packet.gasHeatReleaseWPerM3)&&
				std::isfinite(packet.sootHeatReleaseWPerM3)&&
				std::isfinite(packet.pilotEnergyDeltaJPerM3)&&
				gasRateRatio<=factor&&sootRateRatio<=factor&&pilotEnergyExact&&
				pilotExpansionExact;
			if(closes)return true;
			std::ostringstream message;message<<"fire solver frozen source packet failed its ledger: mass="
				<<massResidual<<", element_ratio="<<elementRatio<<", oxygen="
				<<(-packet.constituentDelta[MethaneO2]-oxygenExpected)<<", energy="
				<<(packet.sensibleEnergyDeltaJPerM3-expectedEnergy)<<", gas-rate="
				<<(packet.gasHeatReleaseWPerM3-expectedGasRate)<<", soot-rate="
				<<(packet.sootHeatReleaseWPerM3-expectedSootRate)<<", pilot-energy="
				<<(packet.pilotEnergyDeltaJPerM3-expectedPilotEnergyJPerM3)<<", pilot-expansion="
				<<(packet.pilotExpansionIntegral-expectedPilotMap.expansionIntegral);
			return Fail(error,message.str());
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
			if(fuel.RecordId()!=thermochemistry.RecordId())return Fail(error,
				"fire solver source packet fuel and thermochemistry records differ");
			MethaneSourcePacket reaction;
			MethaneCellState postReaction;
			if( !BuildMethaneReactionPacket(beginning,fuel,reactionStep,reaction,error,
				&postReaction) ) return false;
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
		if(beginning.producerPrecision==FireStateProducerPrecision::Binary32){
			RepresentMethaneSourcePacketBinary32(result);
		}
			return ValidateFrozenMethaneSourcePacketLedger(beginning,reactionStep,fuel,result,error)&&
				FrozenSourcePacketExpansionAdmissible(ToConservativeVector(beginning),
					beginning.temperatureK,result,reactionStep.deltaTimeS,thermochemistry,
					beginning.producerPrecision,0,error);
		}

		inline bool BuildFrozenMethaneSourcePackets(
			const std::vector<MethaneCellState>& beginning,
			const std::vector<MethaneReactionStep>& reactionStep,
			const std::vector<double>& cellVolumeM3,
			const double ambientTemperatureK,
			const double nominalPeakHeatReleaseW,
			const double radiativeFraction,
			const bool predictive,
			const FireSimulationMethaneRecord& fuel,
			const FireSimulationMethaneRecord& thermochemistry,
			const FireSimulationGasOpacityRecord& opacity,
			std::vector<MethaneSourcePacket>& result,
			RadiationEscapeFactor& factor,
			std::string* error = 0,
			const unsigned int workerCount = 1u
			)
		{
			const std::size_t count = beginning.size();
			if( count == 0 || reactionStep.size() != count || cellVolumeM3.size() != count ) {
				return Fail(error,"fire solver grid source-packet arrays are malformed");
			}
			if(fuel.RecordId()!=thermochemistry.RecordId())return Fail(error,
				"fire solver grid source packet fuel and thermochemistry records differ");
			std::vector<double> unscaledExchange(count,0.0);
			std::vector<MethaneSourcePacket> reaction(count);
			std::vector<MethaneCellState> postReaction(count);
			const unsigned int workers=std::max(1u,std::min(workerCount,
				std::min(static_cast<unsigned int>(count),FireWorkerCapacity())));
			const std::size_t noFailure=std::numeric_limits<std::size_t>::max();
			std::vector<std::size_t> failureCell(workers,noFailure);
			std::vector<std::string> failureMessage(workers);
			ParallelFireSlices(workers,workers,[&](const std::size_t worker) {
				const std::size_t first=count*worker/workers,last=count*(worker+1u)/workers;
				for(std::size_t cell=first;cell<last;++cell) {
				std::string cellError;
				if( !BuildMethaneReactionPacket(beginning[cell],fuel,reactionStep[cell],
					reaction[cell],&cellError,&postReaction[cell]) ) {
					failureCell[worker]=cell;failureMessage[worker]=cellError;break;
				}
				GasExchangeEvaluation exchange;
				if( !EvaluateGasExchange(postReaction[cell],postReaction[cell].temperatureK,
					ambientTemperatureK,thermochemistry,opacity,exchange,&cellError) ) {
					failureCell[worker]=cell;failureMessage[worker]=cellError;break;
				}
				unscaledExchange[cell] = exchange.exchangeWPerM3;
				}
			});
			std::size_t firstFailure=noFailure;unsigned int failedWorker=0u;
			for(unsigned int worker=0;worker<workers;++worker)if(failureCell[worker]<firstFailure){
				firstFailure=failureCell[worker];failedWorker=worker;
			}
			if(firstFailure!=noFailure)return Fail(error,failureMessage[failedWorker]);
			double totalHeatReleaseW = 0.0;
			for( std::size_t cell=0; cell<count; ++cell ) {
				const double heatRelease = (reaction[cell].gasHeatReleaseWPerM3+
					reaction[cell].sootHeatReleaseWPerM3)*cellVolumeM3[cell];
				if( !std::isfinite(heatRelease) || !std::isfinite(totalHeatReleaseW+heatRelease) ) {
					return Fail(error,"fire solver grid heat-release accumulation overflowed");
				}
				totalHeatReleaseW += heatRelease;
			}
			RadiationEscapeFactor candidateFactor;
			if( !ComputeRadiationEscapeFactor(totalHeatReleaseW,nominalPeakHeatReleaseW,
				radiativeFraction,unscaledExchange,cellVolumeM3,predictive,
				candidateFactor,error) ) return false;
			std::vector<MethaneSourcePacket> candidateResult(count);
			std::fill(failureCell.begin(),failureCell.end(),noFailure);
			std::fill(failureMessage.begin(),failureMessage.end(),std::string());
			ParallelFireSlices(workers,workers,[&](const std::size_t worker) {
				const std::size_t first=count*worker/workers,last=count*(worker+1u)/workers;
				for(std::size_t cell=first;cell<last;++cell) {
				MethaneCellState finalScratch;
				double signedCoolingWPerM3 = 0.0;
				std::string cellError;
				if(unscaledExchange[cell]==0.0) {
					finalScratch=postReaction[cell];signedCoolingWPerM3=0.0;
				} else if( !ApplyGasRadiationBackwardEuler(postReaction[cell],ambientTemperatureK,
					reactionStep[cell].deltaTimeS,candidateFactor.accepted,thermochemistry,opacity,
					finalScratch,signedCoolingWPerM3,&cellError) ) {
					failureCell[worker]=cell;failureMessage[worker]=cellError;break;
				}
				candidateResult[cell] = reaction[cell];
				for( std::size_t species=0; species<MethaneSpeciesCount; ++species ) {
					candidateResult[cell].constituentDelta[species] = finalScratch.constituent[species]-
						beginning[cell].constituent[species];
				}
				candidateResult[cell].sensibleEnergyDeltaJPerM3 = finalScratch.sensibleEnergyJPerM3-
					beginning[cell].sensibleEnergyJPerM3;
				candidateResult[cell].radiativeCoolingWPerM3 = signedCoolingWPerM3;
				if(beginning[cell].producerPrecision==FireStateProducerPrecision::Binary32)
					RepresentMethaneSourcePacketBinary32(candidateResult[cell]);
				if(!ValidateFrozenMethaneSourcePacketLedger(beginning[cell],reactionStep[cell],fuel,
					candidateResult[cell],&cellError)) {
					failureCell[worker]=cell;failureMessage[worker]=cellError;break;
				}
				if(!FrozenSourcePacketExpansionAdmissible(ToConservativeVector(beginning[cell]),
					beginning[cell].temperatureK,candidateResult[cell],reactionStep[cell].deltaTimeS,
					thermochemistry,beginning[cell].producerPrecision,0,&cellError)) {
					failureCell[worker]=cell;failureMessage[worker]=cellError;break;
				}
				}
			});
			firstFailure=noFailure;failedWorker=0u;
			for(unsigned int worker=0;worker<workers;++worker)if(failureCell[worker]<firstFailure){
				firstFailure=failureCell[worker];failedWorker=worker;
			}
			if(firstFailure!=noFailure)return Fail(error,failureMessage[failedWorker]);
			result.swap(candidateResult);
			factor = candidateFactor;
			return true;
		}
	}
}

#endif
