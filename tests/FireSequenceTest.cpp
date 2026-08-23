#include "../src/Library/Utilities/FireSequence.h"
#include "../src/Library/Utilities/FireCase.h"
#include "../src/Library/Rendering/FrameStore.h"
#include "../src/Library/Rendering/PixelBasedRasterizerHelper.h"
#include "../src/Library/Materials/HeterogeneousMedium.h"
#include "../src/Library/Materials/HenyeyGreensteinPhaseFunction.h"
#include "../src/Library/Lights/PointLight.h"
#include "../src/Library/Painters/Perlin3DPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Utilities/FireSimulationRecords.h"
#include "../src/Library/Utilities/FireProductionProjection.h"
#include "../src/Library/Utilities/FireProductionForce.h"
#include "../src/Library/Utilities/Reference.h"
#include "../tools/fire_simulator_core.h"
#include "FireOutputMetadataTestFixture.h"
#include "FireProductionCalibrationMirror.h"
#include "FireProductionRoundoffTraceAdapter.h"
#include "FireProductionRoundoffWalker.h"
#include "fire_production_trace/SourceManifest.h"

#include <cmath>
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(RISE_ENABLE_OPENVDB)
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#endif

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	constexpr double CapstonePoolDiameterM=0.30;
	constexpr double CapstoneHeatReleaseRateKW=33.0;
	// McCaffrey, NBSIR 79-1910, Table 1, adopted 33 kW plume row.
	constexpr double McCaffrey33KWVelocityCoefficient=1.13;
	constexpr double McCaffrey33KWTemperatureCoefficientK=23.3;

	using namespace RISE::FireSim;
	int failures = 0;
	bool forcePostRenameDirectorySyncFailureForTest=false;

	class FrozenPainterProbe final : public Perlin3DPainter
	{
	public:
		FrozenPainterProbe(const IPainter& a,const IPainter& b) :
			Perlin3DPainter(0.5,3,a,b,Vector3(1,1,1),Vector3(0,0,0)) {}
		Vector3 Scale() const { return vScale; }
		const void* Function() const { return pFunc; }
		~FrozenPainterProbe() override=default;
	};
	class FrozenUniformProbe final : public UniformColorPainter
	{
	public:
		FrozenUniformProbe() : UniformColorPainter(RISEPel(0.25,0.5,0.75)) {}
		RISEPel Value() const { return C; }
		~FrozenUniformProbe() override=default;
	};

	class FrozenMutationOutput final :
		public virtual IRasterizerOutput,
		public virtual IFireRasterizerOutputRoute,
		public virtual Reference
	{
	public:
		explicit FrozenMutationOutput(IJob& job) : job_(job) {}
		void OutputIntermediateImage(const IRasterImage&,const Rect*) override {}
		void OutputImage(const IRasterImage&,const Rect*,unsigned int) override
		{
			attempted=true;
			rejected=!job_.ClearAll() && !job_.SetFilm(2,2,1.0) &&
				!job_.SetGlobalMedium("sequence_fire") &&
				!job_.SetPrimaryAcceleration(true,false,4,32) &&
				!job_.SetFireFidelityMode("preview") &&
				!job_.SetLightSampleRRThreshold(0.25) &&
				!job_.ClearGlobalRadianceMap();
		}
		FireArtifactRouteKind FireArtifactRoute() const override
			{ return FireArtifactRouteKind::DisplayOnly; }
		bool attempted=false;
		bool rejected=false;
	protected:
		~FrozenMutationOutput() override=default;
	private:
		IJob& job_;
	};

	void Check( const bool condition, const char* message )
	{
		if( !condition ) {
			std::fprintf(stderr,"FAIL: %s\n",message);
			++failures;
		}
	}

	float FloatFromBits( const std::uint32_t bits )
	{
		float value = 0.0f;
		std::memcpy(&value,&bits,sizeof(value));
		return value;
	}
	std::uint64_t DoubleBits(const double value)
	{
		std::uint64_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));return bits;
	}
	RISECBOR64::Bytes AerosolRecord();
	RISECBOR64::Bytes SyntheticChemRecord();
	std::string DigestFile(const std::filesystem::path& path);
	RISECBOR64::Bytes ReadFileBytes(const std::filesystem::path& path);

	struct ResumeEquivalenceCertificate
	{
		std::string certificateId;
		std::string checkpointDigest;
		std::string oldBuildId,newBuildId;
		std::string oldExecutableDigest,newExecutableDigest;
		std::uint64_t resumedFromStep=0u,acceptedStepCount=0u;
		std::vector<std::uint64_t> timeStepBits,maximumTemperatureBits,
			maximumEOSResidualBits;
		std::vector<std::string> frameDigests;
	};
	struct ResumeEquivalenceTrace
	{
		std::string checkpointDigest,checkpointProducerBuildId;
		std::string buildId,executableDigest;
		std::uint64_t resumedFromStep=0u,acceptedStepCount=0u;
		std::vector<std::uint64_t> timeStepBits,maximumTemperatureBits,
			maximumEOSResidualBits;
		std::vector<std::string> frameDigests;
	};

	bool LoadResumeEquivalenceCertificate(const std::filesystem::path& path,
		ResumeEquivalenceCertificate& certificate,std::string& error);
	bool CurrentExecutableDigest(const RISECBOR64::Bytes& buildRecord,
		std::string& digest,std::string& error);
	bool SaveResumeEquivalenceTrace(const std::filesystem::path& path,
		const ResumeEquivalenceTrace& trace,std::string& error);
	bool LoadResumeEquivalenceTrace(const std::filesystem::path& path,
		ResumeEquivalenceTrace& trace,std::string& error);
	bool BuildResumeEquivalenceCertificate(const ResumeEquivalenceTrace& oldTrace,
		const ResumeEquivalenceTrace& newTrace,const std::filesystem::path& path,
		ResumeEquivalenceCertificate& certificate,std::string& error);
	const char* CurrentActiveSetAlgorithmVersion()
	{
		return "open_active_set_two_class_r81_v2";
	}
	const char* LegacyActiveSetAlgorithmVersion()
	{
		return "legacy_pre_r80_active_set";
	}
	bool DiscontinuousThreadIdentityAccepted(const bool identical,
		const bool activeSetChecked,std::string& error)
	{
		if(identical)return true;
		error=activeSetChecked?"active_set_thread_identity_mismatch":
			"limiter_thread_identity_mismatch";
		return false;
	}

	struct SolverFrameValues
	{
		bool succeeded=false;
		std::string structuredError;
		std::array<std::size_t,3> dimensions={{0,0,0}};
		double cellWidthM=0.5;
		std::string caseRecordId;
		float temperatureK=900.0f,reactionWPerM3=0.0f;
		std::vector<float> temperature, reaction, carbon;
		std::vector<std::array<float,3> > velocity;
		double realizedHeatReleaseW=0.0, fuelConsumptionKGPerS=0.0;
		double realizedRadiativeFraction=0.0, acceptedEscapeFactor=0.0;
		double selectedTimeStepS=0.0,acceptedTimeStepS=0.0;
		double simulatedTimeS=0.0,flowThroughTimeS=0.0,pilotEnergyJ=0.0;
		double expectedPilotEnergyJ=0.0;
		double maximumTemperatureK=0.0;
		double maximumPilotApproachEOSResidual=0.0;
		double maximumAcceptedEOSResidual=0.0;
		double minimumActiveHoldTemperatureK=0.0;
		double maximumActiveHoldTemperatureK=0.0;
		double maximumLimiterClassDiscrepancy=0.0;
		unsigned int discontinuousLimiterClassSteps=0u;
		bool discontinuousClassThreadIdentity=true;
		bool discontinuousClassThreadIdentityChecked=false;
		std::string activeSetAlgorithmVersion=CurrentActiveSetAlgorithmVersion();
		std::string priorActiveSetAlgorithmVersion;
		double maximumActiveSetComplementarityDiscrepancyMPerS=0.0;
		unsigned int discontinuousActiveSetEvents=0u;
		std::size_t maximumActiveSetCycleLength=0u;
		std::size_t maximumActiveSetDifferingFaceCount=0u;
		bool activeSetThreadIdentity=true;
		bool activeSetThreadIdentityChecked=false;
		double statisticsStartS=0.0,puffingFrequencyHz=0.0,puffingRelativeError=0.0;
		double firstStatisticsStepStartS=0.0;
		double integratedHeatReleaseJ=0.0,integratedRadiativeLossJ=0.0;
		double integratedFuelConsumptionKG=0.0,integratedRadiativeFraction=0.0;
		double effectiveRadiativeFraction=0.0;
		double centerlineTemperatureExponent=0.0,centerlineFitRMSE=0.0;
		double characteristicDiameterM=0.0,mccaffreyFlameTipHeightM=0.0;
		double mccaffreyMaximumTemperatureRelativeError=0.0;
		double mccaffreyMaximumVelocityRelativeError=0.0;
		std::size_t mccaffreyPlumeStationCount=0u;
		std::vector<double> probeTimeS,probeCenterlineHeatReleaseW;
		std::vector<double> acceptedMaximumEOSResidualHistory;
		std::vector<double> acceptedMaximumTemperatureHistoryK;
		std::vector<double> acceptedTimeStepHistoryS;
		std::vector<double> stationProbeTimeS,stationProbeHeightM;
		std::vector<double> stationProbeTemperatureK,stationProbeReactionWPerM3;
		std::vector<double> stationProbeVerticalVelocityMPerS;
		std::vector<double> centerlineHeightM,centerlineTemperatureK,centerlineVelocityMPerS;
		bool ignitedDuringPilot=false,sustainedAfterPilot=false;
		bool pilotHoldBandObserved=false,pilotHoldBandSatisfied=true;
		bool pilotApproachComplete=false;
		bool statisticsBoundaryObserved=false;
		double checkpointCadenceWallS=0.0;
		std::vector<std::uint64_t> checkpointStepIndices;
		std::vector<std::uint64_t> workerCountHistory;
		std::string reductionMode="fixed_order_tree_v1";
		bool resumedFromCheckpoint=false;
		std::uint64_t resumedFromStep=0u;
		std::uint64_t streamedFrameCount=0u;
		std::string migrationCertificateId;
		std::string migrationOldBuildId;
		std::string migrationNewBuildId;
		std::uint64_t migrationAcceptedStepCount=0u;
		std::uint64_t migrationResumedFromStep=0u;
	};

	struct RunPersistenceOptions
	{
		std::filesystem::path checkpointPath;
		std::filesystem::path finalCheckpointPath;
		double checkpointCadenceWallS=0.0;
		std::uint64_t streamedFrameCountAtStart=0u;
		bool resume=false;
		bool killAfterFirstCheckpoint=false;
		std::filesystem::path resumeEquivalenceCertificatePath;
		bool isolatedEquivalenceProbe=false;
		std::string isolatedExpectedCheckpointBuildId;
		std::uint64_t stopAfterAdditionalAcceptedSteps=0u;
		std::filesystem::path equivalenceSnapshotDirectory;
		bool forceActiveSetIdentityCheckForTest=false;
		bool injectActiveSetIdentityMismatchForTest=false;
		bool forceZeroSourceForTest=false;
	};

	class CheckpointWriter
	{
	public:
		explicit CheckpointWriter(const std::filesystem::path& path) :
			output_(path,std::ios::binary|std::ios::trunc) {}
		bool Good() const { return static_cast<bool>(output_); }
		bool HeaderBytes(const void* data,const std::size_t size)
		{
			output_.write(static_cast<const char*>(data),static_cast<std::streamsize>(size));
			return static_cast<bool>(output_);
		}
		bool SeekHeader(const std::streamoff offset)
		{
			output_.seekp(offset);return static_cast<bool>(output_);
		}
		template<typename T> bool Pod(const T& value)
		{
			static_assert(std::is_arithmetic<T>::value,"checkpoint POD must be arithmetic");
			return Bytes(&value,sizeof(value));
		}
		bool Bytes(const void* data,const std::size_t size)
		{
			if(!output_)return false;
			output_.write(static_cast<const char*>(data),static_cast<std::streamsize>(size));
			const unsigned char* byte=static_cast<const unsigned char*>(data);
			for(std::size_t i=0;i<size;++i){checksum_^=byte[i];checksum_*=1099511628211ull;}
			payloadBytes_+=static_cast<std::uint64_t>(size);return static_cast<bool>(output_);
		}
		bool String(const std::string& value)
		{
			const std::uint64_t size=static_cast<std::uint64_t>(value.size());
			return Pod(size)&&(size==0u||Bytes(value.data(),value.size()));
		}
		bool Finish()
		{
			output_.flush();output_.close();return !output_.fail();
		}
		std::uint64_t Checksum() const { return checksum_; }
		std::uint64_t PayloadBytes() const { return payloadBytes_; }
	private:
		std::ofstream output_;
		std::uint64_t checksum_=1469598103934665603ull,payloadBytes_=0u;
	};

	class CheckpointReader
	{
	public:
		explicit CheckpointReader(const std::filesystem::path& path) :
			input_(path,std::ios::binary) {}
		bool Good() const { return static_cast<bool>(input_); }
		bool HeaderBytes(void* data,const std::size_t size)
		{
			input_.read(static_cast<char*>(data),static_cast<std::streamsize>(size));
			return static_cast<bool>(input_);
		}
		template<typename T> bool Pod(T& value)
		{
			static_assert(std::is_arithmetic<T>::value,"checkpoint POD must be arithmetic");
			return Bytes(&value,sizeof(value));
		}
		bool Bytes(void* data,const std::size_t size)
		{
			if(!input_||consumed_+size>limit_)return false;
			input_.read(static_cast<char*>(data),static_cast<std::streamsize>(size));
			if(!input_)return false;
			const unsigned char* byte=static_cast<const unsigned char*>(data);
			for(std::size_t i=0;i<size;++i){checksum_^=byte[i];checksum_*=1099511628211ull;}
			consumed_+=static_cast<std::uint64_t>(size);return true;
		}
		bool String(std::string& value)
		{
			std::uint64_t size=0u;if(!Pod(size)||size>16u*1024u*1024u)return false;
			value.assign(static_cast<std::size_t>(size),'\0');
			return size==0u||Bytes(&value[0],static_cast<std::size_t>(size));
		}
		void SetLimit(const std::uint64_t limit){limit_=limit;}
		bool Finished(const std::uint64_t checksum) const
			{return consumed_==limit_&&checksum_==checksum;}
	private:
		std::ifstream input_;
		std::uint64_t checksum_=1469598103934665603ull,consumed_=0u;
		std::uint64_t limit_=std::numeric_limits<std::uint64_t>::max();
	};

	template<typename T> bool WriteArithmeticVector(CheckpointWriter& writer,
		const std::vector<T>& value)
	{
		const std::uint64_t size=static_cast<std::uint64_t>(value.size());
		if(!writer.Pod(size))return false;
		for(const T& item:value)if(!writer.Pod(item))return false;
		return true;
	}
	template<typename T> bool ReadArithmeticVector(CheckpointReader& reader,
		std::vector<T>& value,const std::uint64_t maximum=200000000u)
	{
		std::uint64_t size=0u;if(!reader.Pod(size)||size>maximum)return false;
		value.resize(static_cast<std::size_t>(size));
		for(T& item:value)if(!reader.Pod(item))return false;
		return true;
	}

	bool WriteSolverFrameValues(CheckpointWriter& w,const SolverFrameValues& v)
	{
		if(!w.Pod(v.succeeded)||!w.String(v.structuredError)||!w.String(v.reductionMode))return false;
		for(const std::size_t dimension:v.dimensions){const std::uint64_t encoded=dimension;
			if(!w.Pod(encoded))return false;}
		if(!w.Pod(v.cellWidthM)||!w.String(v.caseRecordId)||!w.Pod(v.temperatureK)||
			!w.Pod(v.reactionWPerM3)||!WriteArithmeticVector(w,v.temperature)||
			!WriteArithmeticVector(w,v.reaction)||!WriteArithmeticVector(w,v.carbon))return false;
		const std::uint64_t velocityCount=v.velocity.size();if(!w.Pod(velocityCount))return false;
		for(const auto& velocity:v.velocity)for(const float component:velocity)
			if(!w.Pod(component))return false;
#define WRITE_CHECKPOINT_FIELD(field) if(!w.Pod(v.field))return false
		WRITE_CHECKPOINT_FIELD(realizedHeatReleaseW);WRITE_CHECKPOINT_FIELD(fuelConsumptionKGPerS);
		WRITE_CHECKPOINT_FIELD(realizedRadiativeFraction);WRITE_CHECKPOINT_FIELD(acceptedEscapeFactor);
		WRITE_CHECKPOINT_FIELD(selectedTimeStepS);WRITE_CHECKPOINT_FIELD(acceptedTimeStepS);
		WRITE_CHECKPOINT_FIELD(simulatedTimeS);WRITE_CHECKPOINT_FIELD(flowThroughTimeS);
		WRITE_CHECKPOINT_FIELD(pilotEnergyJ);WRITE_CHECKPOINT_FIELD(expectedPilotEnergyJ);
		WRITE_CHECKPOINT_FIELD(maximumTemperatureK);
		WRITE_CHECKPOINT_FIELD(maximumPilotApproachEOSResidual);
		WRITE_CHECKPOINT_FIELD(maximumAcceptedEOSResidual);
		WRITE_CHECKPOINT_FIELD(minimumActiveHoldTemperatureK);
		WRITE_CHECKPOINT_FIELD(maximumActiveHoldTemperatureK);
		WRITE_CHECKPOINT_FIELD(maximumLimiterClassDiscrepancy);
		WRITE_CHECKPOINT_FIELD(discontinuousLimiterClassSteps);
		WRITE_CHECKPOINT_FIELD(discontinuousClassThreadIdentity);
		WRITE_CHECKPOINT_FIELD(discontinuousClassThreadIdentityChecked);
		WRITE_CHECKPOINT_FIELD(statisticsStartS);WRITE_CHECKPOINT_FIELD(puffingFrequencyHz);
		WRITE_CHECKPOINT_FIELD(puffingRelativeError);WRITE_CHECKPOINT_FIELD(firstStatisticsStepStartS);
		WRITE_CHECKPOINT_FIELD(integratedHeatReleaseJ);WRITE_CHECKPOINT_FIELD(integratedRadiativeLossJ);
		WRITE_CHECKPOINT_FIELD(integratedFuelConsumptionKG);WRITE_CHECKPOINT_FIELD(integratedRadiativeFraction);
		WRITE_CHECKPOINT_FIELD(effectiveRadiativeFraction);WRITE_CHECKPOINT_FIELD(centerlineTemperatureExponent);
		WRITE_CHECKPOINT_FIELD(centerlineFitRMSE);WRITE_CHECKPOINT_FIELD(characteristicDiameterM);
		WRITE_CHECKPOINT_FIELD(mccaffreyFlameTipHeightM);
		WRITE_CHECKPOINT_FIELD(mccaffreyMaximumTemperatureRelativeError);
		WRITE_CHECKPOINT_FIELD(mccaffreyMaximumVelocityRelativeError);
		{const std::uint64_t count=v.mccaffreyPlumeStationCount;if(!w.Pod(count))return false;}
		WRITE_CHECKPOINT_FIELD(ignitedDuringPilot);WRITE_CHECKPOINT_FIELD(sustainedAfterPilot);
		WRITE_CHECKPOINT_FIELD(pilotHoldBandObserved);WRITE_CHECKPOINT_FIELD(pilotHoldBandSatisfied);
		WRITE_CHECKPOINT_FIELD(pilotApproachComplete);
		WRITE_CHECKPOINT_FIELD(statisticsBoundaryObserved);WRITE_CHECKPOINT_FIELD(checkpointCadenceWallS);
		WRITE_CHECKPOINT_FIELD(resumedFromCheckpoint);WRITE_CHECKPOINT_FIELD(resumedFromStep);
		WRITE_CHECKPOINT_FIELD(streamedFrameCount);
#undef WRITE_CHECKPOINT_FIELD
		return WriteArithmeticVector(w,v.probeTimeS)&&WriteArithmeticVector(w,v.probeCenterlineHeatReleaseW)&&
			WriteArithmeticVector(w,v.acceptedMaximumEOSResidualHistory)&&
			WriteArithmeticVector(w,v.acceptedTimeStepHistoryS)&&
			WriteArithmeticVector(w,v.stationProbeTimeS)&&WriteArithmeticVector(w,v.stationProbeHeightM)&&
			WriteArithmeticVector(w,v.stationProbeTemperatureK)&&WriteArithmeticVector(w,v.stationProbeReactionWPerM3)&&
			WriteArithmeticVector(w,v.stationProbeVerticalVelocityMPerS)&&
			WriteArithmeticVector(w,v.centerlineHeightM)&&WriteArithmeticVector(w,v.centerlineTemperatureK)&&
			WriteArithmeticVector(w,v.centerlineVelocityMPerS)&&
			WriteArithmeticVector(w,v.checkpointStepIndices)&&
			WriteArithmeticVector(w,v.workerCountHistory);
	}

	bool ReadSolverFrameValues(CheckpointReader& r,SolverFrameValues& v)
	{
		if(!r.Pod(v.succeeded)||!r.String(v.structuredError)||!r.String(v.reductionMode))return false;
		for(std::size_t& dimension:v.dimensions){std::uint64_t encoded=0u;
			if(!r.Pod(encoded)||encoded>std::numeric_limits<std::size_t>::max())return false;
			dimension=static_cast<std::size_t>(encoded);}
		if(!r.Pod(v.cellWidthM)||!r.String(v.caseRecordId)||!r.Pod(v.temperatureK)||
			!r.Pod(v.reactionWPerM3)||!ReadArithmeticVector(r,v.temperature)||
			!ReadArithmeticVector(r,v.reaction)||!ReadArithmeticVector(r,v.carbon))return false;
		std::uint64_t velocityCount=0u;if(!r.Pod(velocityCount)||velocityCount>200000000u)return false;
		v.velocity.resize(static_cast<std::size_t>(velocityCount));
		for(auto& velocity:v.velocity)for(float& component:velocity)if(!r.Pod(component))return false;
#define READ_CHECKPOINT_FIELD(field) if(!r.Pod(v.field))return false
		READ_CHECKPOINT_FIELD(realizedHeatReleaseW);READ_CHECKPOINT_FIELD(fuelConsumptionKGPerS);
		READ_CHECKPOINT_FIELD(realizedRadiativeFraction);READ_CHECKPOINT_FIELD(acceptedEscapeFactor);
		READ_CHECKPOINT_FIELD(selectedTimeStepS);READ_CHECKPOINT_FIELD(acceptedTimeStepS);
		READ_CHECKPOINT_FIELD(simulatedTimeS);READ_CHECKPOINT_FIELD(flowThroughTimeS);
		READ_CHECKPOINT_FIELD(pilotEnergyJ);READ_CHECKPOINT_FIELD(expectedPilotEnergyJ);
		READ_CHECKPOINT_FIELD(maximumTemperatureK);
		READ_CHECKPOINT_FIELD(maximumPilotApproachEOSResidual);
		READ_CHECKPOINT_FIELD(maximumAcceptedEOSResidual);
		READ_CHECKPOINT_FIELD(minimumActiveHoldTemperatureK);
		READ_CHECKPOINT_FIELD(maximumActiveHoldTemperatureK);
		READ_CHECKPOINT_FIELD(maximumLimiterClassDiscrepancy);
		READ_CHECKPOINT_FIELD(discontinuousLimiterClassSteps);
		READ_CHECKPOINT_FIELD(discontinuousClassThreadIdentity);
		READ_CHECKPOINT_FIELD(discontinuousClassThreadIdentityChecked);
		READ_CHECKPOINT_FIELD(statisticsStartS);READ_CHECKPOINT_FIELD(puffingFrequencyHz);
		READ_CHECKPOINT_FIELD(puffingRelativeError);READ_CHECKPOINT_FIELD(firstStatisticsStepStartS);
		READ_CHECKPOINT_FIELD(integratedHeatReleaseJ);READ_CHECKPOINT_FIELD(integratedRadiativeLossJ);
		READ_CHECKPOINT_FIELD(integratedFuelConsumptionKG);READ_CHECKPOINT_FIELD(integratedRadiativeFraction);
		READ_CHECKPOINT_FIELD(effectiveRadiativeFraction);READ_CHECKPOINT_FIELD(centerlineTemperatureExponent);
		READ_CHECKPOINT_FIELD(centerlineFitRMSE);READ_CHECKPOINT_FIELD(characteristicDiameterM);
		READ_CHECKPOINT_FIELD(mccaffreyFlameTipHeightM);
		READ_CHECKPOINT_FIELD(mccaffreyMaximumTemperatureRelativeError);
		READ_CHECKPOINT_FIELD(mccaffreyMaximumVelocityRelativeError);
		{std::uint64_t count=0u;if(!r.Pod(count)||count>std::numeric_limits<std::size_t>::max())return false;
			v.mccaffreyPlumeStationCount=static_cast<std::size_t>(count);}
		READ_CHECKPOINT_FIELD(ignitedDuringPilot);READ_CHECKPOINT_FIELD(sustainedAfterPilot);
		READ_CHECKPOINT_FIELD(pilotHoldBandObserved);READ_CHECKPOINT_FIELD(pilotHoldBandSatisfied);
		READ_CHECKPOINT_FIELD(pilotApproachComplete);
		READ_CHECKPOINT_FIELD(statisticsBoundaryObserved);READ_CHECKPOINT_FIELD(checkpointCadenceWallS);
		READ_CHECKPOINT_FIELD(resumedFromCheckpoint);READ_CHECKPOINT_FIELD(resumedFromStep);
		READ_CHECKPOINT_FIELD(streamedFrameCount);
#undef READ_CHECKPOINT_FIELD
		return ReadArithmeticVector(r,v.probeTimeS)&&ReadArithmeticVector(r,v.probeCenterlineHeatReleaseW)&&
			ReadArithmeticVector(r,v.acceptedMaximumEOSResidualHistory,1000000u)&&
			ReadArithmeticVector(r,v.acceptedTimeStepHistoryS,1000000u)&&
			ReadArithmeticVector(r,v.stationProbeTimeS)&&ReadArithmeticVector(r,v.stationProbeHeightM)&&
			ReadArithmeticVector(r,v.stationProbeTemperatureK)&&ReadArithmeticVector(r,v.stationProbeReactionWPerM3)&&
			ReadArithmeticVector(r,v.stationProbeVerticalVelocityMPerS)&&
			ReadArithmeticVector(r,v.centerlineHeightM)&&ReadArithmeticVector(r,v.centerlineTemperatureK)&&
			ReadArithmeticVector(r,v.centerlineVelocityMPerS)&&
			ReadArithmeticVector(r,v.checkpointStepIndices,1000000u)&&
			ReadArithmeticVector(r,v.workerCountHistory,1000000u);
	}

	struct MethaneRunCheckpoint
	{
		std::uint64_t checkpointFormatVersion=0u;
		std::string caseRecordId;
		std::string producerBuildId;
		std::array<std::size_t,3> dimensions={{0u,0u,0u}};
		double cellWidthM=0.0;
		std::vector<MethaneCellState> states;
		PeriodicMACField momentum;
		PeriodicMACField velocity;
		SolverFrameValues values;
		std::vector<double> centerlineTemperatureIntegral;
		std::vector<double> centerlineVelocityIntegral;
		std::vector<double> planeHeatReleaseIntegral;
		double centerlineStatisticsDurationS=0.0;
		double simulationTimeS=0.0,previousStepS=0.0,lastAcceptedStepS=0.0;
		std::uint64_t acceptedSteps=0u;
		FireProductionAcceptedManifoldObservation productionManifoldObservation;
	};

	bool suppressExpectedCheckpointStateDiagnostic=false;
	bool BuildCheckpointAcceptedStatePayload(const MethaneRunCheckpoint& checkpoint,
		FireProductionProjectionShape& shape,std::vector<float>& conservative,
		std::array<std::vector<float>,3>& momentum,
		std::array<std::vector<float>,3>& velocity,std::uint64_t& digest)
	{
		digest=0u;shape=FireProductionProjectionShape();conservative.clear();
		for(unsigned int axis=0u;axis<3u;++axis){momentum[axis].clear();velocity[axis].clear();}
		if(checkpoint.dimensions[0u]==0u||checkpoint.dimensions[1u]==0u||
			checkpoint.dimensions[2u]==0u||!std::isfinite(checkpoint.cellWidthM)||
			checkpoint.cellWidthM<=0.0)return false;
		shape.nx=checkpoint.dimensions[0u];shape.ny=checkpoint.dimensions[1u];
		shape.nz=checkpoint.dimensions[2u];shape.cellWidthM=static_cast<float>(checkpoint.cellWidthM);
		if(shape.CellCount()!=checkpoint.states.size())return false;
		for(unsigned int axis=0u;axis<3u;++axis){
			const std::size_t expectedFaces=FireProductionProjectionFaceCount(shape,axis);
			if(expectedFaces==0u||checkpoint.momentum.component[axis].size()!=expectedFaces||
				checkpoint.velocity.component[axis].size()!=expectedFaces){
				if(!suppressExpectedCheckpointStateDiagnostic){
					std::fprintf(stderr,
					"checkpoint accepted face shape mismatch axis=%u expected=%zu momentum=%zu velocity=%zu\n",
					axis,expectedFaces,checkpoint.momentum.component[axis].size(),
					checkpoint.velocity.component[axis].size());}
				return false;
			}
		}
		conservative.assign(checkpoint.states.size()*9u,0.0f);
		std::vector<ConservativeVector> conservativeByCell(checkpoint.states.size());
		for(std::size_t cell=0u;cell<checkpoint.states.size();++cell){
			const MethaneCellState& state=checkpoint.states[cell];
			const ConservativeVector values=ToConservativeVector(state);
			conservativeByCell[cell]=values;
			for(std::size_t component=0u;component<9u;++component){
				const double value=values[component];const float represented=static_cast<float>(value);
				if(!std::isfinite(represented)||static_cast<double>(represented)!=value){
					std::fprintf(stderr,"checkpoint state is not exact binary32 cell=%zu component=%zu value=%.17g represented=%.17g\n",
						cell,component,value,static_cast<double>(represented));return false;}
				conservative[component*checkpoint.states.size()+cell]=represented;}
		}
		std::vector<double> reconstructedTemperature;
		std::string inversionError;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		if(!InvertPeriodicTemperaturesWithinBounds(conservativeByCell,fuel,
			fuel.TemperatureMinK(),fuel.TemperatureMaxK(),
			FireStateProducerPrecision::Binary32,reconstructedTemperature,&inversionError,1u)||
			reconstructedTemperature.size()!=checkpoint.states.size()){
			if(!suppressExpectedCheckpointStateDiagnostic){
				std::fprintf(stderr,
				"checkpoint accepted temperature reconstruction failed: %s\n",inversionError.c_str());}
			return false;
		}
		for(std::size_t cell=0u;cell<checkpoint.states.size();++cell){
			if(reconstructedTemperature[cell]!=checkpoint.states[cell].temperatureK){
				if(!suppressExpectedCheckpointStateDiagnostic){
				std::fprintf(stderr,
				"checkpoint accepted temperature mismatch cell=%zu stored=%.17g reconstructed=%.17g\n",
				cell,checkpoint.states[cell].temperatureK,reconstructedTemperature[cell]);}
				return false;
			}
		}
		for(unsigned int axis=0u;axis<3u;++axis){
			momentum[axis].reserve(checkpoint.momentum.component[axis].size());
			velocity[axis].reserve(checkpoint.velocity.component[axis].size());
			for(const double value:checkpoint.momentum.component[axis]){
				const float represented=static_cast<float>(value);
				if(!std::isfinite(represented)||static_cast<double>(represented)!=value){
					std::fprintf(stderr,"checkpoint momentum is not exact binary32 axis=%u value=%.17g represented=%.17g\n",
						axis,value,static_cast<double>(represented));return false;}
				momentum[axis].push_back(represented);}
			for(const double value:checkpoint.velocity.component[axis]){
				const float represented=static_cast<float>(value);
				if(!std::isfinite(represented)||static_cast<double>(represented)!=value){
					std::fprintf(stderr,"checkpoint velocity is not exact binary32 axis=%u value=%.17g represented=%.17g\n",
						axis,value,static_cast<double>(represented));return false;}
				velocity[axis].push_back(represented);}
		}
		digest=FireProductionAcceptedStatePayloadDigest(shape,conservative,momentum,velocity);
		return digest!=0u;
	}

	bool CheckpointAcceptedStateDigest(const MethaneRunCheckpoint& checkpoint,
		std::uint64_t& digest)
	{
		FireProductionProjectionShape shape;std::vector<float> conservative;
		std::array<std::vector<float>,3> momentum,velocity;
		return BuildCheckpointAcceptedStatePayload(checkpoint,shape,conservative,
			momentum,velocity,digest);
	}

	bool HomogeneousStateProducerPrecision(const std::vector<MethaneCellState>& states,
		FireStateProducerPrecision& precision)
	{
		if(states.empty())return false;
		precision=states.front().producerPrecision;
		if(precision!=FireStateProducerPrecision::Binary64&&
			precision!=FireStateProducerPrecision::Binary32)return false;
		return std::all_of(states.begin(),states.end(),[precision](const MethaneCellState& state){
			return state.producerPrecision==precision;
		});
	}

	bool WriteCellStates(CheckpointWriter& writer,const std::vector<MethaneCellState>& states,
		const std::uint64_t version)
	{
		FireStateProducerPrecision precision=FireStateProducerPrecision::Unknown;
		if(!HomogeneousStateProducerPrecision(states,precision)||
			(version<9u&&precision!=FireStateProducerPrecision::Binary64))return false;
		const std::uint64_t count=states.size();if(!writer.Pod(count))return false;
		for(const MethaneCellState& state:states){
			if(version>=9u){const unsigned char producerPrecision=
				static_cast<unsigned char>(state.producerPrecision);
				if(!writer.Pod(producerPrecision))return false;}
			if(!writer.Pod(state.rhoTotalZ))return false;
			for(const double value:state.constituent)if(!writer.Pod(value))return false;
			if(!writer.Pod(state.sensibleEnergyJPerM3)||!writer.Pod(state.temperatureK))return false;
		}
		return true;
	}

	bool ReadCellStates(CheckpointReader& reader,std::vector<MethaneCellState>& states,
		const std::uint64_t version)
	{
		std::uint64_t count=0u;if(!reader.Pod(count)||count>100000000u)return false;
		states.resize(static_cast<std::size_t>(count));
		for(MethaneCellState& state:states){
			if(version>=9u){unsigned char producerPrecision=0u;
				if(!reader.Pod(producerPrecision)||(producerPrecision!=
					static_cast<unsigned char>(FireStateProducerPrecision::Binary64)&&
					producerPrecision!=static_cast<unsigned char>(
						FireStateProducerPrecision::Binary32)))return false;
				state.producerPrecision=static_cast<FireStateProducerPrecision>(producerPrecision);
			}else state.producerPrecision=FireStateProducerPrecision::Binary64;
			if(!reader.Pod(state.rhoTotalZ))return false;
			for(double& value:state.constituent)if(!reader.Pod(value))return false;
			if(!reader.Pod(state.sensibleEnergyJPerM3)||!reader.Pod(state.temperatureK))return false;
		}
		FireStateProducerPrecision precision=FireStateProducerPrecision::Unknown;
		return HomogeneousStateProducerPrecision(states,precision);
	}

	template<typename Field> bool WriteMACField(CheckpointWriter& writer,const Field& field)
	{
		for(unsigned int axis=0;axis<3;++axis)
			if(!WriteArithmeticVector(writer,field.component[axis]))return false;
		return true;
	}
	template<typename Field> bool ReadMACField(CheckpointReader& reader,Field& field)
	{
		for(unsigned int axis=0;axis<3;++axis)
			if(!ReadArithmeticVector(reader,field.component[axis]))return false;
		return true;
	}
	bool forceMalformedManifoldLifecycleWriteForTest=false;

	bool WriteCheckpointPayload(CheckpointWriter& writer,const MethaneRunCheckpoint& checkpoint,
		const std::uint64_t version)
	{
		if(!writer.String(checkpoint.caseRecordId)||!writer.String(checkpoint.producerBuildId))return false;
		for(const std::size_t dimension:checkpoint.dimensions){const std::uint64_t encoded=dimension;
			if(!writer.Pod(encoded))return false;}
		const bool baseWritten=writer.Pod(checkpoint.cellWidthM)&&
			WriteCellStates(writer,checkpoint.states,version)&&
			WriteMACField(writer,checkpoint.momentum)&&WriteMACField(writer,checkpoint.velocity)&&
			WriteSolverFrameValues(writer,checkpoint.values)&&
			WriteArithmeticVector(writer,checkpoint.centerlineTemperatureIntegral)&&
			WriteArithmeticVector(writer,checkpoint.centerlineVelocityIntegral)&&
			WriteArithmeticVector(writer,checkpoint.planeHeatReleaseIntegral)&&
			writer.Pod(checkpoint.centerlineStatisticsDurationS)&&
			writer.Pod(checkpoint.simulationTimeS)&&writer.Pod(checkpoint.previousStepS)&&
			writer.Pod(checkpoint.lastAcceptedStepS)&&writer.Pod(checkpoint.acceptedSteps);
		if(!baseWritten)return false;
		if(version<6u)return true;
		const bool migrationWritten=
			WriteArithmeticVector(writer,checkpoint.values.acceptedMaximumTemperatureHistoryK)&&
			writer.String(checkpoint.values.migrationCertificateId)&&
			writer.String(checkpoint.values.migrationOldBuildId)&&
			writer.String(checkpoint.values.migrationNewBuildId)&&
			writer.Pod(checkpoint.values.migrationAcceptedStepCount)&&
			writer.Pod(checkpoint.values.migrationResumedFromStep);
		if(!migrationWritten)return false;
		if(version<7u)return true;
		const bool activeSetWritten=writer.String(checkpoint.values.activeSetAlgorithmVersion)&&
			writer.Pod(checkpoint.values.maximumActiveSetComplementarityDiscrepancyMPerS)&&
			writer.Pod(checkpoint.values.discontinuousActiveSetEvents)&&
			writer.Pod(checkpoint.values.maximumActiveSetCycleLength)&&
			writer.Pod(checkpoint.values.maximumActiveSetDifferingFaceCount)&&
			writer.Pod(checkpoint.values.activeSetThreadIdentity)&&
			writer.Pod(checkpoint.values.activeSetThreadIdentityChecked);
		if(!activeSetWritten)return false;
		if(version>=8u&&!writer.String(checkpoint.values.priorActiveSetAlgorithmVersion))return false;
		FireStateProducerPrecision precision=FireStateProducerPrecision::Unknown;
		if(version>=9u&&!HomogeneousStateProducerPrecision(checkpoint.states,precision))return false;
		if(version<10u){
			if(version==9u&&precision==FireStateProducerPrecision::Binary32&&
				(checkpoint.acceptedSteps!=0u||checkpoint.simulationTimeS!=0.0||
				checkpoint.previousStepS!=0.0||checkpoint.lastAcceptedStepS!=0.0||
				!checkpoint.values.acceptedTimeStepHistoryS.empty())&&
				!forceMalformedManifoldLifecycleWriteForTest)return false;
			return true;
		}
		const FireProductionAcceptedManifoldObservation& observation=
			checkpoint.productionManifoldObservation;
		const bool productionState=precision==FireStateProducerPrecision::Binary32;
		const bool acceptedProductionState=productionState&&checkpoint.acceptedSteps>0u;
		double acceptedDurationS=0.0;
		bool validAcceptedHistory=true;
		for(const double timeStepS:checkpoint.values.acceptedTimeStepHistoryS){
			if(!std::isfinite(timeStepS)||timeStepS<=0.0){validAcceptedHistory=false;break;}
			acceptedDurationS+=timeStepS;
			if(!std::isfinite(acceptedDurationS)){validAcceptedHistory=false;break;}
		}
		const bool invalidManifoldLifecycle=(observation.Available()&&(!std::isfinite(observation.TimeStepS())||
			observation.TimeStepS()<=0.0||!std::isfinite(observation.MaximumGeneration())||
			observation.MaximumGeneration()<0.0||
			!std::isfinite(observation.RestorationDrainFraction())||
			observation.RestorationDrainFraction()<0.0||
				observation.RestorationDrainFraction()>1.0||!acceptedProductionState||
				observation.TimeStepS()!=checkpoint.previousStepS||
				observation.TimeStepS()!=checkpoint.lastAcceptedStepS||
				checkpoint.values.acceptedTimeStepHistoryS.empty()||
				checkpoint.values.acceptedTimeStepHistoryS.back()!=checkpoint.previousStepS))||
			(!observation.Available()&&(observation.TimeStepS()!=0.0||
				observation.MaximumGeneration()!=0.0||
				observation.RestorationDrainFraction()!=0.0||
				(version>=12u&&acceptedProductionState)))||
			(productionState&&checkpoint.acceptedSteps==0u&&
				(checkpoint.previousStepS!=0.0||checkpoint.lastAcceptedStepS!=0.0||
				!checkpoint.values.acceptedTimeStepHistoryS.empty()))||
			(productionState&&(!validAcceptedHistory||
				checkpoint.values.acceptedTimeStepHistoryS.size()!=checkpoint.acceptedSteps||
				acceptedDurationS!=checkpoint.simulationTimeS));
		if(invalidManifoldLifecycle&&!forceMalformedManifoldLifecycleWriteForTest)return false;
		const unsigned char manifoldAvailable=
			observation.Available()?1u:0u;
		const bool observationWritten=writer.Pod(manifoldAvailable)&&
			writer.Pod(observation.TimeStepS())&&
			writer.Pod(observation.MaximumGeneration())&&
			writer.Pod(observation.RestorationDrainFraction());
		if(!observationWritten)return false;
		if(version==11u){const std::uint64_t retiredTupleSeal=0u;
			return writer.Pod(retiredTupleSeal);}
		if(version<12u)return true;
		std::uint64_t stateDigest=0u;
		if(observation.Available()&&(!CheckpointAcceptedStateDigest(checkpoint,stateDigest)||
			stateDigest!=observation.SerializedAcceptedStateDigest()))return false;
		if(!writer.Pod(stateDigest))return false;
		const std::uint64_t payloadBinding=writer.Checksum()^
			UINT64_C(0x63b96d44f1a72ec8);
		return writer.Pod(payloadBinding);
	}

	bool ReadCheckpointPayload(CheckpointReader& reader,MethaneRunCheckpoint& checkpoint,
		const std::uint64_t version)
	{
		checkpoint.productionManifoldObservation=
			FireProductionAcceptedManifoldObservation();
		if(!reader.String(checkpoint.caseRecordId)||!reader.String(checkpoint.producerBuildId))return false;
		for(std::size_t& dimension:checkpoint.dimensions){std::uint64_t encoded=0u;
			if(!reader.Pod(encoded)||encoded>std::numeric_limits<std::size_t>::max())return false;
			dimension=static_cast<std::size_t>(encoded);}
		const bool decoded=reader.Pod(checkpoint.cellWidthM)&&
			ReadCellStates(reader,checkpoint.states,version)&&
			ReadMACField(reader,checkpoint.momentum)&&ReadMACField(reader,checkpoint.velocity)&&
			ReadSolverFrameValues(reader,checkpoint.values)&&
			ReadArithmeticVector(reader,checkpoint.centerlineTemperatureIntegral)&&
			ReadArithmeticVector(reader,checkpoint.centerlineVelocityIntegral)&&
			ReadArithmeticVector(reader,checkpoint.planeHeatReleaseIntegral)&&
			reader.Pod(checkpoint.centerlineStatisticsDurationS)&&
			reader.Pod(checkpoint.simulationTimeS)&&reader.Pod(checkpoint.previousStepS)&&
			reader.Pod(checkpoint.lastAcceptedStepS)&&reader.Pod(checkpoint.acceptedSteps);
		if(!decoded)return false;
		if(version>=6u&&(!ReadArithmeticVector(reader,
			checkpoint.values.acceptedMaximumTemperatureHistoryK,1000000u)||
			!reader.String(checkpoint.values.migrationCertificateId)||
			!reader.String(checkpoint.values.migrationOldBuildId)||
			!reader.String(checkpoint.values.migrationNewBuildId)||
			!reader.Pod(checkpoint.values.migrationAcceptedStepCount)||
			!reader.Pod(checkpoint.values.migrationResumedFromStep)))return false;
		if(version>=7u&&(!reader.String(checkpoint.values.activeSetAlgorithmVersion)||
			!reader.Pod(checkpoint.values.maximumActiveSetComplementarityDiscrepancyMPerS)||
			!reader.Pod(checkpoint.values.discontinuousActiveSetEvents)||
			!reader.Pod(checkpoint.values.maximumActiveSetCycleLength)||
			!reader.Pod(checkpoint.values.maximumActiveSetDifferingFaceCount)||
			!reader.Pod(checkpoint.values.activeSetThreadIdentity)||
			!reader.Pod(checkpoint.values.activeSetThreadIdentityChecked)))return false;
		if(version>=8u&&!reader.String(checkpoint.values.priorActiveSetAlgorithmVersion))return false;
		if(version>=10u){unsigned char manifoldAvailable=0u;double timeStepS=0.0;
			double maximumGeneration=0.0,restorationDrainFraction=0.0;
			if(!reader.Pod(manifoldAvailable)||manifoldAvailable>1u||!reader.Pod(timeStepS)||
				!reader.Pod(maximumGeneration)||!reader.Pod(restorationDrainFraction))return false;
			if(version>=11u){std::uint64_t retiredOrStateDigest=0u;
				if(!reader.Pod(retiredOrStateDigest))return false;}
			if(version>=12u){std::uint64_t payloadBinding=0u;
				if(!reader.Pod(payloadBinding))return false;}
		}
		checkpoint.checkpointFormatVersion=version;
		if(version<7u)checkpoint.values.activeSetAlgorithmVersion=
			LegacyActiveSetAlgorithmVersion();
		return true;
	}

	bool DurableSyncFileAndDirectory(const std::filesystem::path& path,std::string& error)
	{
#if defined(_WIN32)
		HANDLE file=CreateFileW(path.wstring().c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,
			OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
		if(file==INVALID_HANDLE_VALUE){error="cannot open durable run file";return false;}
		const bool flushed=FlushFileBuffers(file)!=0;
		const bool closed=CloseHandle(file)!=0;
		if(!flushed||!closed)error="cannot flush durable run file";
		return flushed&&closed;
#else
		const int fd=::open(path.c_str(),O_RDONLY);
		if(fd<0){error="cannot open durable run file";return false;}
		const bool fileSynced=::fsync(fd)==0;
		const bool fileClosed=::close(fd)==0;
		if(!fileSynced||!fileClosed){error="cannot fsync durable run file";return false;}
		const std::filesystem::path directory=path.has_parent_path()?path.parent_path():".";
		const int directoryFd=::open(directory.c_str(),O_RDONLY);
		if(directoryFd<0){error="cannot open durable run directory";return false;}
		const bool directorySynced=::fsync(directoryFd)==0;
		const bool directoryClosed=::close(directoryFd)==0;
		if(!directorySynced||!directoryClosed){error="cannot fsync durable run directory";return false;}
		return true;
#endif
	}

	bool AtomicReplaceCheckpoint(const std::filesystem::path& temporary,
		const std::filesystem::path& target,std::string& error)
	{
#if defined(_WIN32)
		if(!MoveFileExW(temporary.wstring().c_str(),target.wstring().c_str(),
			MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
			error="cannot atomically replace run checkpoint";return false;}
		if(forcePostRenameDirectorySyncFailureForTest){
			error="cannot fsync replaced-run directory";return false;}
		return true;
#else
		if(::rename(temporary.c_str(),target.c_str())!=0){error="cannot atomically replace run checkpoint";return false;}
		const std::filesystem::path directory=target.has_parent_path()?target.parent_path():".";
		const int directoryFd=::open(directory.c_str(),O_RDONLY);
		if(directoryFd<0){error="cannot open replaced-run directory";return false;}
		const bool directorySynced=!forcePostRenameDirectorySyncFailureForTest&&
			::fsync(directoryFd)==0;
		const bool directoryClosed=::close(directoryFd)==0;
		if(!directorySynced||!directoryClosed){error="cannot fsync replaced-run directory";return false;}
		return true;
#endif
	}

	bool SaveMethaneRunCheckpoint(const std::filesystem::path& path,
		const MethaneRunCheckpoint& checkpoint,std::string& error,
		const std::uint64_t version=12u)
	{
		if(version<5u||version>12u){error="run checkpoint output version is invalid";return false;}
		if(path.has_parent_path())std::filesystem::create_directories(path.parent_path());
#if defined(_WIN32)
		const long long processId=static_cast<long long>(::_getpid());
#else
		const long long processId=static_cast<long long>(::getpid());
#endif
		const std::filesystem::path temporary=path.string()+".tmp."+std::to_string(processId);
		CheckpointWriter writer(temporary);if(!writer.Good()){error="cannot open run checkpoint";return false;}
		const char magic[16]={'R','I','S','E','F','I','R','E','C','H','K','P','T','1',0,0};
		const std::uint64_t endian=0x0102030405060708ull,zero=0u;
		auto rejectTemporary=[&temporary](){std::error_code ignored;
			std::filesystem::remove(temporary,ignored);};
		if(!writer.HeaderBytes(magic,sizeof(magic))||!writer.HeaderBytes(&version,sizeof(version))||
			!writer.HeaderBytes(&endian,sizeof(endian))||!writer.HeaderBytes(&zero,sizeof(zero))||
			!writer.HeaderBytes(&zero,sizeof(zero))||!WriteCheckpointPayload(writer,checkpoint,version)){
			error="cannot serialize complete run checkpoint";rejectTemporary();return false;}
		const std::uint64_t payloadBytes=writer.PayloadBytes(),checksum=writer.Checksum();
		if(!writer.SeekHeader(32)||!writer.HeaderBytes(&payloadBytes,sizeof(payloadBytes))||
			!writer.HeaderBytes(&checksum,sizeof(checksum))||!writer.Finish()){
			error="cannot finalize run checkpoint";rejectTemporary();return false;}
		if(!DurableSyncFileAndDirectory(temporary,error)||
			!AtomicReplaceCheckpoint(temporary,path,error)){rejectTemporary();return false;}
		return true;
	}

	bool VerifyCheckpointPayloadChecksum(const std::filesystem::path& path,
		const std::uint64_t payloadBytes,const std::uint64_t expected,std::string& error)
	{
		std::ifstream input(path,std::ios::binary);
		if(!input){error="cannot open run checkpoint for checksum";return false;}
		input.seekg(48,std::ios::beg);
		if(!input){error="cannot seek run checkpoint payload";return false;}
		std::array<unsigned char,65536u> buffer={{0u}};
		std::uint64_t remaining=payloadBytes;
		std::uint64_t checksum=1469598103934665603ull;
		while(remaining>0u){
			const std::size_t chunk=static_cast<std::size_t>(std::min<std::uint64_t>(
				remaining,buffer.size()));
			input.read(reinterpret_cast<char*>(buffer.data()),static_cast<std::streamsize>(chunk));
			if(!input){error="run checkpoint payload is incomplete";return false;}
			for(std::size_t i=0;i<chunk;++i){checksum^=buffer[i];checksum*=1099511628211ull;}
			remaining-=chunk;
		}
		if(checksum!=expected){error="run checkpoint checksum mismatch";return false;}
		return true;
	}

	bool LoadMethaneRunCheckpoint(const std::filesystem::path& path,
		MethaneRunCheckpoint& checkpoint,std::string& error)
	{
		std::error_code sizeError;
		const std::uintmax_t exactFileBytes=std::filesystem::file_size(path,sizeError);
		CheckpointReader reader(path);if(!reader.Good()){error="cannot open run checkpoint";return false;}
		char magic[16]={};std::uint64_t version=0u,endian=0u,payloadBytes=0u,checksum=0u;
		const char expected[16]={'R','I','S','E','F','I','R','E','C','H','K','P','T','1',0,0};
		if(!reader.HeaderBytes(magic,sizeof(magic))||std::memcmp(magic,expected,sizeof(magic))!=0||
			!reader.HeaderBytes(&version,sizeof(version))||
				(version!=5u&&version!=6u&&version!=7u&&version!=8u&&version!=9u&&
					version!=10u&&version!=11u&&version!=12u)||
			!reader.HeaderBytes(&endian,sizeof(endian))||endian!=0x0102030405060708ull||
			!reader.HeaderBytes(&payloadBytes,sizeof(payloadBytes))||
			!reader.HeaderBytes(&checksum,sizeof(checksum))||payloadBytes>64ull*1024ull*1024ull*1024ull||
			sizeError||exactFileBytes!=48ull+payloadBytes){
			error="run checkpoint header is invalid";return false;}
		if(!VerifyCheckpointPayloadChecksum(path,payloadBytes,checksum,error))return false;
		reader.SetLimit(payloadBytes);MethaneRunCheckpoint decoded;
		if(!ReadCheckpointPayload(reader,decoded,version)||!reader.Finished(checksum)){
			error="run checkpoint payload is incomplete or corrupt";return false;}
		FireStateProducerPrecision precision=FireStateProducerPrecision::Unknown;
		if(!HomogeneousStateProducerPrecision(decoded.states,precision)){
			error="run checkpoint producer precision is invalid";return false;}
		const bool productionState=precision==FireStateProducerPrecision::Binary32;
		if(version<12u&&productionState&&(decoded.acceptedSteps>0u||
			decoded.simulationTimeS!=0.0||decoded.previousStepS!=0.0||
			decoded.lastAcceptedStepS!=0.0||
			!decoded.values.acceptedTimeStepHistoryS.empty())){
			error="legacy production checkpoint lacks accepted manifold authority";return false;}
		if(version>=12u){std::uint64_t acceptedStateDigest=0u;
			FireProductionProjectionShape acceptedShape;std::vector<float> acceptedConservative;
			std::array<std::vector<float>,3> acceptedMomentum,acceptedVelocity;
			if(productionState&&decoded.acceptedSteps>0u&&
				!BuildCheckpointAcceptedStatePayload(decoded,acceptedShape,
				acceptedConservative,acceptedMomentum,acceptedVelocity,acceptedStateDigest)){
				error="production checkpoint accepted state is not canonical binary32";return false;}
			FireProductionAcceptedCheckpointStateView stateView;stateView.shape=acceptedShape;
			stateView.conservativeValues=&acceptedConservative;stateView.momentum=&acceptedMomentum;
			stateView.velocity=&acceptedVelocity;
			FireProductionAcceptedCheckpointLifecycleView lifecycle;
			lifecycle.simulationTimeS=decoded.simulationTimeS;
			lifecycle.previousStepS=decoded.previousStepS;
			lifecycle.lastAcceptedStepS=decoded.lastAcceptedStepS;
			lifecycle.acceptedSteps=decoded.acceptedSteps;
			lifecycle.productionState=productionState;
			lifecycle.acceptedTimeStepHistoryS=&decoded.values.acceptedTimeStepHistoryS;
			if(!FireProductionCheckpointManifoldAccess::RestoreValidatedCheckpointFile(
				path.string(),payloadBytes,checksum,version,stateView,lifecycle,
				decoded.productionManifoldObservation,&error))return false;
		}
		checkpoint=std::move(decoded);
		return true;
	}

	[[noreturn]] void HardKillCurrentProcess()
	{
#if defined(_WIN32)
		TerminateProcess(GetCurrentProcess(),91u);
#else
		::raise(SIGKILL);
#endif
		std::_Exit(91);
	}

	double DominantUniformResampledFrequency( const std::vector<double>& time,
		const std::vector<double>& signal )
	{
		if(time.size()<8u||time.size()!=signal.size()||!(time.back()>time.front())) return 0.0;
		constexpr std::size_t sampleCount=512u;
		std::array<double,sampleCount> uniform={{0.0}};
		const double duration=time.back()-time.front();
		std::size_t right=1u;
		for(std::size_t sample=0;sample<sampleCount;++sample) {
			const double target=time.front()+duration*static_cast<double>(sample)/
				static_cast<double>(sampleCount-1u);
			while(right<time.size()&&time[right]<target) ++right;
			if(right>=time.size()) uniform[sample]=signal.back();
			else {
				const double span=time[right]-time[right-1u];
				const double fraction=span>0.0?(target-time[right-1u])/span:0.0;
				uniform[sample]=signal[right-1u]+fraction*(signal[right]-signal[right-1u]);
			}
		}
		// Remove the least-squares affine trend before applying a Hann window.  The
		// direct DFT below is an independent harness calculation, not a solver path.
		double sumX=0.0,sumY=0.0,sumXX=0.0,sumXY=0.0;
		for(std::size_t sample=0;sample<sampleCount;++sample) {
			const double x=static_cast<double>(sample);
			sumX+=x;sumY+=uniform[sample];sumXX+=x*x;sumXY+=x*uniform[sample];
		}
		const double denominator=static_cast<double>(sampleCount)*sumXX-sumX*sumX;
		const double slope=denominator!=0.0?
			(static_cast<double>(sampleCount)*sumXY-sumX*sumY)/denominator:0.0;
		const double intercept=(sumY-slope*sumX)/static_cast<double>(sampleCount);
		double detrendedEnergy=0.0,signalScale=0.0;
		for(std::size_t sample=0;sample<sampleCount;++sample) {
			const double residual=uniform[sample]-intercept-slope*static_cast<double>(sample);
			detrendedEnergy+=residual*residual;
			signalScale=std::max(signalScale,std::fabs(uniform[sample]));
		}
		if(!(detrendedEnergy>64.0*std::numeric_limits<double>::epsilon()*
			std::max(1.0,signalScale*signalScale))) return 0.0;
		double bestPower=-1.0;std::size_t bestBin=0u;
		for(std::size_t bin=1u;bin<sampleCount/2u;++bin) {
			double real=0.0,imaginary=0.0;
			for(std::size_t sample=0;sample<sampleCount;++sample) {
				const double window=0.5-0.5*std::cos(2.0*3.14159265358979323846*
					static_cast<double>(sample)/static_cast<double>(sampleCount-1u));
				const double value=(uniform[sample]-intercept-slope*static_cast<double>(sample))*window;
				const double angle=2.0*3.14159265358979323846*static_cast<double>(bin*sample)/
					static_cast<double>(sampleCount);
				real+=value*std::cos(angle);imaginary-=value*std::sin(angle);
			}
			const double power=real*real+imaginary*imaginary;
			if(power>bestPower) {bestPower=power;bestBin=bin;}
		}
		return static_cast<double>(bestBin)*static_cast<double>(sampleCount-1u)/
			(static_cast<double>(sampleCount)*duration);
	}

	void FitCenterlineTemperaturePowerLaw( SolverFrameValues& values )
	{
		if(values.centerlineHeightM.size()!=values.centerlineTemperatureK.size()) return;
		double bestError=std::numeric_limits<double>::infinity(),bestSlope=0.0;
		const double spacing=values.cellWidthM;
		for(unsigned int originStep=0;originStep<32u;++originStep) {
			const double origin=-2.0*spacing+4.0*spacing*static_cast<double>(originStep)/31.0;
			double sx=0.0,sy=0.0,sxx=0.0,sxy=0.0,syy=0.0;std::size_t count=0u;
			for(std::size_t i=0;i<values.centerlineHeightM.size();++i) {
				const double distance=values.centerlineHeightM[i]-origin;
				const double excess=values.centerlineTemperatureK[i]-300.0;
				if(values.centerlineHeightM[i]<=values.mccaffreyFlameTipHeightM||
					distance<=2.0*spacing||excess<=1.0) continue;
				const double x=std::log(distance),y=std::log(excess);
				sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;syy+=y*y;++count;
			}
			if(count<4u) continue;
			const double divisor=static_cast<double>(count)*sxx-sx*sx;
			if(divisor==0.0) continue;
			const double fittedSlope=(static_cast<double>(count)*sxy-sx*sy)/divisor;
			const double fittedIntercept=(sy-fittedSlope*sx)/static_cast<double>(count);
			const double squared=syy+static_cast<double>(count)*fittedIntercept*fittedIntercept+
				fittedSlope*fittedSlope*sxx+2.0*fittedIntercept*fittedSlope*sx-
				2.0*fittedIntercept*sy-2.0*fittedSlope*sxy;
			const double error=std::sqrt(std::max(0.0,squared/static_cast<double>(count)));
			if(error<bestError) {bestError=error;bestSlope=fittedSlope;}
		}
		if(std::isfinite(bestError)) {
			values.centerlineTemperatureExponent=bestSlope;
			values.centerlineFitRMSE=bestError;
		}
	}

	void EvaluateMcCaffreyPlumeStations( SolverFrameValues& values,
		const double heatReleaseRateKW )
	{
		if(values.centerlineHeightM.size()!=values.centerlineTemperatureK.size()||
			values.centerlineHeightM.size()!=values.centerlineVelocityMPerS.size()||
			!(heatReleaseRateKW>0.0)) return;
		// McCaffrey, NBSIR 79-1910: compare to the adopted absolute 33 kW
		// plume row, rather than re-deriving rounded constants from one another.
		const double qTwoFifths=std::pow(heatReleaseRateKW,0.4);
		const double qOneFifth=std::pow(heatReleaseRateKW,0.2);
		const double top=values.centerlineHeightM.empty()?0.0:
			values.centerlineHeightM.back()+0.5*values.cellWidthM;
		const double maximumUncontaminatedHeight=top-0.5*values.characteristicDiameterM;
		const double minimumHeight=std::max(values.mccaffreyFlameTipHeightM,
			0.2*qTwoFifths);
		for(std::size_t station=0;station<values.centerlineHeightM.size();++station){
			const double height=values.centerlineHeightM[station];
			if(!(height>minimumHeight&&height<=maximumUncontaminatedHeight)) continue;
			const double normalizedHeight=height/qTwoFifths;
			const double expectedVelocity=McCaffrey33KWVelocityCoefficient*qOneFifth*
				std::pow(normalizedHeight,-1.0/3.0);
			const double expectedTemperatureRise=McCaffrey33KWTemperatureCoefficientK*
				std::pow(normalizedHeight,-5.0/3.0);
			const double observedTemperatureRise=values.centerlineTemperatureK[station]-300.0;
			values.mccaffreyMaximumTemperatureRelativeError=std::max(
				values.mccaffreyMaximumTemperatureRelativeError,
				std::fabs(observedTemperatureRise-expectedTemperatureRise)/
					expectedTemperatureRise);
			values.mccaffreyMaximumVelocityRelativeError=std::max(
				values.mccaffreyMaximumVelocityRelativeError,
				std::fabs(values.centerlineVelocityMPerS[station]-expectedVelocity)/
					expectedVelocity);
			++values.mccaffreyPlumeStationCount;
		}
	}
	SolverFrameValues RunMethaneFrameProbe( const unsigned int workerCount=1u,
		const unsigned int minimumStepCount=1u,const double targetTimeS=0.0,
		const double caseDurationS=1.0,const double caseFramesPerS=4.0,
		const double resolutionTier=6.0,const double poolDiameterM=CapstonePoolDiameterM,
		const double heatReleaseRateKW=CapstoneHeatReleaseRateKW,
		const bool injectSolverFailure=false,
		const RunPersistenceOptions& persistence=RunPersistenceOptions() )
	{
		SolverFrameValues values;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		MethaneCellState state; state.temperatureK=300.0;
		for(std::size_t i=0;i<MethaneSpeciesCount;++i)
			state.constituent[i]=fuel.AmbientMassFractions()[i];
		double invW=0.0;
		for(std::size_t i=0;i<MethaneCarbon;++i) {
			const FireThermochemistrySpecies* s=fuel.FindSpecies(fuel.SpeciesOrder()[i].c_str());
			if(s) invW+=state.constituent[i]/s->molecularWeightKGPerKMol;
		}
		const double rho=fuel.ThermodynamicPressurePa()/(8314.46261815324*state.temperatureK*invW);
		for(double& x:state.constituent) x*=rho;
		state.rhoTotalZ=0.0;
		std::string error;
		Check(fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(state),state.temperatureK,
			state.sensibleEnergyJPerM3,&error),"capstone initial methane state closes thermochemistry");
		FireCase::AuthoredV1 authored;
		authored.fuelRecordId=fuel.RecordId(); authored.poolDiameterM=poolDiameterM;
		authored.heatReleaseRateKW=heatReleaseRateKW; authored.envelope={{0.0,1.0}};
		authored.durationS=caseDurationS; authored.quality="dstar";
		authored.numericDStarTier=resolutionTier; authored.seed=1234;
		authored.outputFramesPerS=caseFramesPerS;
		authored.plumeLaw=true;
		FireCase::RecordV1 caseRecord;
		const RISECBOR64::Bytes aerosol=AerosolRecord();
		const RISECBOR64::Bytes chem=SyntheticChemRecord();
		const bool caseBuilt=FireCase::BuildMethaneV1(authored,fuel,{RISECBOR64::SHA256Hex(fuel.RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationThermochemistryRecord::OpenSubsetV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationTransportRecord::OpenV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1().RecordBytes()),
			RISECBOR64::SHA256Hex(FireOpticsPreset::PredictiveV1().RecordBytes()),RISECBOR64::SHA256Hex(aerosol),
			RISECBOR64::SHA256Hex(chem)},caseRecord,error);
		Check(caseBuilt,
			"capstone solver consumes the canonical r57 methane case");
		if(!caseBuilt) {
			std::fprintf(stderr,"capstone case derivation rejected: %s\n",error.c_str());
			values.structuredError="case_derivation_failure:"+error;
			return values;
		}
		RISECBOR64::Bytes currentBuildBytes;
		std::string currentBuildId,currentExecutableDigest;
		if((!persistence.checkpointPath.empty()||!persistence.finalCheckpointPath.empty())&&
			(!CurrentRendererBuildIdentity(currentBuildBytes,currentBuildId)||
			!CurrentExecutableDigest(currentBuildBytes,currentExecutableDigest,error))){
			values.structuredError="checkpoint_build_identity_failure";return values;
		}
		PeriodicMACShape shape; shape.nx=caseRecord.derived.nx;shape.ny=caseRecord.derived.ny;
		shape.nz=caseRecord.derived.nz;shape.cellWidthM=caseRecord.derived.cellWidthM;
		const std::array<std::size_t,2> centerXIndex={{(shape.nx-1u)/2u,shape.nx/2u}},
			centerYIndex={{(shape.ny-1u)/2u,shape.ny/2u}};
		const std::size_t centerXCount=centerXIndex[0]==centerXIndex[1]?1u:2u,
			centerYCount=centerYIndex[0]==centerYIndex[1]?1u:2u;
		const double centerSampleCount=static_cast<double>(centerXCount*centerYCount);
		values.effectiveRadiativeFraction=caseRecord.derived.effectiveRadiativeFraction;
		const bool reportCapstoneProgress=std::getenv("RISE_FIRE_CAPSTONE_OUTPUT")!=nullptr;
		if(reportCapstoneProgress) std::fprintf(stderr,
			"capstone probe workers=%u target=%.9g grid=%zux%zux%zu dx=%.9g t_ft=%.9g\n",
			workerCount,targetTimeS,shape.nx,shape.ny,shape.nz,shape.cellWidthM,
			caseRecord.derived.flowThroughTimeS);
		const double zeroGradient[3][3]={{0.0,0.0,0.0},{0.0,0.0,0.0},{0.0,0.0,0.0}};
		const double widths[3]={shape.cellWidthM,shape.cellWidthM,shape.cellWidthM};
		CellTransportEvaluation transportEvaluation;
		Check(EvaluateCellTransport(state,zeroGradient,widths,false,fuel,
			FireSimulationTransportRecord::OpenV1(),transportEvaluation,&error),
			"capstone evaluates physical methane transport from the adopted record");
		StableTimeStep selectedStep;
		const double maximumTransport=std::max(transportEvaluation.totalDiffusivityM2PerS,
			transportEvaluation.effectiveViscosityPaS/state.GasDensity());
		const double positiveReducedGravity=std::max(0.0,9.80665*(rho-state.GasDensity())/
			state.GasDensity());
		Check(ComputeStableTimeStep(shape.cellWidthM,0.0,positiveReducedGravity,
			maximumTransport,3,0.0,selectedStep,&error),
			"capstone consumes the production r54 timestep selector");
		MethaneReactionStep reaction; reaction.deltaTimeS=selectedStep.seconds;
		reaction.maximumAcceptedTemperatureK=caseRecord.derived.maximumAcceptedTemperatureK;
		Check(ComputeMixingTimeS(state,transportEvaluation,FireSimulationTransportRecord::OpenV1(),
			shape.cellWidthM,rho,9.80665,false,reaction.mixingTimeS,&error),
			"capstone derives mixing time instead of authoring a closure constant");
		std::vector<MethaneCellState> states(shape.CellCount(),state);
		std::vector<MethaneReactionStep> reactions(shape.CellCount(),reaction);
		std::vector<MethaneSourcePacket> packets;
		RadiationEscapeFactor escape;
		const double cellVolume=shape.cellWidthM*shape.cellWidthM*shape.cellWidthM;
		PeriodicMACField momentum; for(unsigned int axis=0;axis<3;++axis)
			momentum.component[axis].assign(OpenMACFaceCount3D(shape,axis),0.0);
		ConservativeAdvance3DConfig config; config.transport.cellWidthM=shape.cellWidthM;
		config.transport.deltaTimeS=reaction.deltaTimeS; config.transport.ambientTemperatureK=300.0;
		config.transport.adiabaticTemperatureK=caseRecord.derived.maximumAcceptedTemperatureK;
		config.transport.ambientGasDensityKGPerM3=rho;
		config.transport.producerPrecision=FireStateProducerPrecision::Binary64;
		Check(config.transport.adiabaticTemperatureK==2300.0&&
			config.transport.adiabaticTemperatureK<
				FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1().TemperatureMaxK(),
			"r74 capstone owner separates the case physical ceiling from the opacity domain");
		config.gravityMPerS2={{0.0,0.0,-9.80665}};
		Check(config.gravityMPerS2[0]==0.0&&config.gravityMPerS2[1]==0.0&&
			config.gravityMPerS2[2]==-9.80665,
			"capstone owning advance receives the pinned vertical gravitational acceleration");
		const double projectionReferenceVelocityMPerS=std::sqrt(9.80665*
			caseRecord.derived.characteristicDiameterM);
		const double projectionReferenceLengthM=std::max({caseRecord.derived.extentXM,
			caseRecord.derived.extentYM,caseRecord.derived.extentZM});
		config.projectionTolerancePerS=1.0e-3*projectionReferenceVelocityMPerS/
			projectionReferenceLengthM;
		config.dns=false; config.workerCount=workerCount;
		config.periodicBoundaries=false;
		config.injectedTemperatureK=300.0;
		MethaneCellState ambient; ambient.temperatureK=300.0;
		for(std::size_t i=0;i<MethaneSpeciesCount;++i)
			ambient.constituent[i]=fuel.AmbientMassFractions()[i];
		double ambientInvW=0.0;
		for(std::size_t i=0;i<MethaneCarbon;++i) {
			const FireThermochemistrySpecies* species=fuel.FindSpecies(fuel.SpeciesOrder()[i].c_str());
			if(species) ambientInvW+=ambient.constituent[i]/species->molecularWeightKGPerKMol;
		}
		const double ambientRho=fuel.ThermodynamicPressurePa()/(8314.46261815324*
			ambient.temperatureK*ambientInvW);
		for(double& x:ambient.constituent) x*=ambientRho;
		ambient.rhoTotalZ=0.0;
		Check(fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(ambient),
			ambient.temperatureK,ambient.sensibleEnergyJPerM3,&error),
			"capstone ambient state closes thermochemistry");
		MethaneCellState injected; injected.temperatureK=300.0;
		for(std::size_t i=0;i<MethaneSpeciesCount;++i)
			injected.constituent[i]=fuel.InjectedMassFractions()[i];
		double injectedInvW=0.0;
		for(std::size_t i=0;i<MethaneCarbon;++i) {
			const FireThermochemistrySpecies* species=fuel.FindSpecies(fuel.SpeciesOrder()[i].c_str());
			if(species) injectedInvW+=injected.constituent[i]/species->molecularWeightKGPerKMol;
		}
		const double injectedRho=fuel.ThermodynamicPressurePa()/(8314.46261815324*
			injected.temperatureK*injectedInvW);
		for(double& x:injected.constituent) x*=injectedRho;
		injected.rhoTotalZ=injected.TotalDensity();
		Check(fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(injected),
			injected.temperatureK,injected.sensibleEnergyJPerM3,&error),
			"capstone injected methane state closes thermochemistry");
		config.openBoundary.ambientDensityKGPerM3=ambient.GasDensity();
		config.openBoundary.injectedGasDensityKGPerM3=injected.GasDensity();
		config.openBoundary.ambientState=ToConservativeVector(ambient);
		config.openBoundary.injectedState=ToConservativeVector(injected);
		config.openBoundary.fuelMassFluxKGPerM2S=0.0;
		std::vector<double> sourcePattern;
		Check(FireCase::BuildSourcePattern(authored,caseRecord.derived,sourcePattern,error),
			"capstone solver installs the case's exact SplitMix64 source pattern");
		config.openBoundary.bottomFuelMask.resize(shape.nx*shape.ny,false);
		config.openBoundary.bottomFuelMassFluxKGPerM2S.resize(shape.nx*shape.ny,0.0);
		for(std::size_t face=0;face<sourcePattern.size();++face) if(sourcePattern[face]!=0.0) {
			config.openBoundary.bottomFuelMask[face]=true;
			config.openBoundary.bottomFuelMassFluxKGPerM2S[face]=
				caseRecord.derived.nominalFuelFluxKGPerM2S*sourcePattern[face];
		}
		std::vector<std::uint8_t> canonicalPilotMask;
		Check(FireCase::BuildPilotMask(authored,caseRecord.derived,canonicalPilotMask,error),
			"capstone installs the canonical r57 intensive pilot annulus");
		const double pilotEndS=caseRecord.derived.pilotDurationMultiplier*
			caseRecord.derived.flowThroughTimeS;
		const double pilotRampEndS=caseRecord.derived.flowThroughTimeS/
			caseRecord.derived.pilotRampExponentPerFlowThrough;
		const double pilotCommandMaximumStepS=FireCase::PilotCommandMaximumStepS(
			caseRecord.derived);
		Check(pilotCommandMaximumStepS>0.0,
			"capstone derives the r70 command-ramp step ceiling from the canonical cap");
		values.maximumTemperatureK=state.temperatureK;
		values.statisticsStartS=5.0*caseRecord.derived.flowThroughTimeS;
		values.characteristicDiameterM=caseRecord.derived.characteristicDiameterM;
		std::vector<double> centerlineTemperatureIntegral(shape.nz,0.0),
			centerlineVelocityIntegral(shape.nz,0.0),
			planeHeatReleaseIntegral(shape.nz,0.0);
		double centerlineStatisticsDurationS=0.0;
		config.openBoundary.velocityToleranceMPerS=1.0e-3*projectionReferenceVelocityMPerS;
		config.openBoundary.pressureTolerancePa=ambient.GasDensity()*
			projectionReferenceVelocityMPerS*config.openBoundary.velocityToleranceMPerS;
		Check(config.openBoundary.velocityToleranceMPerS==
			config.projectionTolerancePerS*projectionReferenceLengthM&&
			config.openBoundary.pressureTolerancePa==ambient.GasDensity()*
				projectionReferenceVelocityMPerS*config.openBoundary.velocityToleranceMPerS,
			"capstone pressure-open deadband derives from the pinned epsilon_abs scene scale");
		ConservativeAdvance3DResult advanced;
		bool advancedOK=minimumStepCount>0u || targetTimeS==0.0;
		double simulationTimeS=0.0,previousStepS=0.0;
		unsigned int acceptedSteps=0u;
		unsigned int effectiveMinimumStepCount=minimumStepCount;
		values.checkpointCadenceWallS=persistence.checkpointCadenceWallS;
		values.streamedFrameCount=persistence.streamedFrameCountAtStart;
		values.workerCountHistory.push_back(workerCount);
		if(persistence.resume&&!persistence.checkpointPath.empty()&&
			std::filesystem::exists(persistence.checkpointPath)){
			MethaneRunCheckpoint checkpoint;
			if(!LoadMethaneRunCheckpoint(persistence.checkpointPath,checkpoint,error)){
				values.structuredError="checkpoint_resume_failure:"+error;return values;
			}
			const bool sameBuild=checkpoint.producerBuildId==currentBuildId;
			const bool isolatedProbe=!sameBuild&&persistence.isolatedEquivalenceProbe&&
				persistence.isolatedExpectedCheckpointBuildId==checkpoint.producerBuildId&&
				persistence.stopAfterAdditionalAcceptedSteps>=8u;
			ResumeEquivalenceCertificate migration;
			bool certifiedMigration=false;
			if(!sameBuild&&!persistence.resumeEquivalenceCertificatePath.empty()&&
				LoadResumeEquivalenceCertificate(persistence.resumeEquivalenceCertificatePath,
					migration,error)){
				const std::string checkpointDigest=DigestFile(persistence.checkpointPath);
				certifiedMigration=!checkpointDigest.empty()&&
					migration.checkpointDigest==checkpointDigest&&
					migration.oldBuildId==checkpoint.producerBuildId&&
					migration.newBuildId==currentBuildId&&
					migration.newExecutableDigest==currentExecutableDigest&&
					migration.resumedFromStep==checkpoint.acceptedSteps;
			}
			const bool currentActiveSetCheckpoint=checkpoint.checkpointFormatVersion>=7u&&
				checkpoint.values.activeSetAlgorithmVersion==CurrentActiveSetAlgorithmVersion();
			const bool legacyActiveSetCheckpoint=checkpoint.checkpointFormatVersion<7u&&
				checkpoint.values.activeSetAlgorithmVersion==LegacyActiveSetAlgorithmVersion();
			const bool priorActiveSetHistoryValid=checkpoint.checkpointFormatVersion<8u?
				checkpoint.values.priorActiveSetAlgorithmVersion.empty():
				(checkpoint.values.priorActiveSetAlgorithmVersion.empty()||
					checkpoint.values.priorActiveSetAlgorithmVersion==
						LegacyActiveSetAlgorithmVersion());
			FireStateProducerPrecision checkpointPrecision=FireStateProducerPrecision::Unknown;
			if(checkpoint.caseRecordId!=caseRecord.caseRecordId||
				(!sameBuild&&!isolatedProbe&&!certifiedMigration)||
				checkpoint.values.reductionMode!="fixed_order_tree_v1"||
				(!currentActiveSetCheckpoint&&!legacyActiveSetCheckpoint)||
				!priorActiveSetHistoryValid||
				checkpoint.dimensions!=std::array<std::size_t,3>{{shape.nx,shape.ny,shape.nz}}||
				checkpoint.cellWidthM!=shape.cellWidthM||checkpoint.states.size()!=shape.CellCount()||
				checkpoint.acceptedSteps>std::numeric_limits<unsigned int>::max()||
				checkpoint.centerlineTemperatureIntegral.size()!=shape.nz||
				checkpoint.centerlineVelocityIntegral.size()!=shape.nz||
				checkpoint.planeHeatReleaseIntegral.size()!=shape.nz||
				!HomogeneousStateProducerPrecision(checkpoint.states,checkpointPrecision)){
				if(error.empty())error="checkpoint build migration is not certified";
				values.structuredError="checkpoint_resume_failure:"+error;return values;
			}
			for(unsigned int axis=0;axis<3;++axis)if(
				checkpoint.momentum.component[axis].size()!=OpenMACFaceCount3D(shape,axis)||
				checkpoint.velocity.component[axis].size()!=OpenMACFaceCount3D(shape,axis)){
				values.structuredError="checkpoint_resume_failure:checkpoint MAC shape mismatch";
				return values;
			}
			states=std::move(checkpoint.states);momentum=std::move(checkpoint.momentum);
			config.transport.producerPrecision=checkpointPrecision;
			advanced.velocityMPerS=std::move(checkpoint.velocity);
			values=std::move(checkpoint.values);
			if(legacyActiveSetCheckpoint)values.priorActiveSetAlgorithmVersion=
				LegacyActiveSetAlgorithmVersion();
			values.activeSetAlgorithmVersion=CurrentActiveSetAlgorithmVersion();
			values.workerCountHistory.push_back(workerCount);
			centerlineTemperatureIntegral=std::move(checkpoint.centerlineTemperatureIntegral);
			centerlineVelocityIntegral=std::move(checkpoint.centerlineVelocityIntegral);
			planeHeatReleaseIntegral=std::move(checkpoint.planeHeatReleaseIntegral);
			centerlineStatisticsDurationS=checkpoint.centerlineStatisticsDurationS;
			simulationTimeS=checkpoint.simulationTimeS;previousStepS=checkpoint.previousStepS;
			reaction.deltaTimeS=checkpoint.lastAcceptedStepS;
			acceptedSteps=static_cast<unsigned int>(checkpoint.acceptedSteps);
			if(persistence.stopAfterAdditionalAcceptedSteps>0u){
				const std::uint64_t requested=checkpoint.acceptedSteps+
					persistence.stopAfterAdditionalAcceptedSteps;
				if(requested>std::numeric_limits<unsigned int>::max()){
					values.structuredError="checkpoint_resume_failure:step target overflow";return values;
				}
				effectiveMinimumStepCount=static_cast<unsigned int>(requested);
			}
			values.resumedFromCheckpoint=true;values.resumedFromStep=acceptedSteps;
			if(certifiedMigration){
				values.migrationCertificateId=migration.certificateId;
				values.migrationOldBuildId=migration.oldBuildId;
				values.migrationNewBuildId=migration.newBuildId;
				values.migrationAcceptedStepCount=migration.acceptedStepCount;
				values.migrationResumedFromStep=migration.resumedFromStep;
			}
			values.checkpointCadenceWallS=persistence.checkpointCadenceWallS;
			if(reportCapstoneProgress)std::fprintf(stderr,
				"capstone resumed checkpoint step=%u time=%.17g path=%s\n",acceptedSteps,
				simulationTimeS,persistence.checkpointPath.string().c_str());
		}
		auto lastCheckpointWall=std::chrono::steady_clock::now();
		if(FireProfileEnabled())FireProfileReportAndReset("preloop");
		while(advancedOK&&(acceptedSteps<effectiveMinimumStepCount||simulationTimeS<targetTimeS)) {
			const auto profileStepStart=std::chrono::steady_clock::now();
			if(acceptedSteps>=65536u) { advancedOK=false;error="capstone exceeded its deterministic step cap";break; }
			IgnitionGrid eligibilityGrid;
			eligibilityGrid.nx=shape.nx;eligibilityGrid.ny=shape.ny;eligibilityGrid.nz=shape.nz;
			eligibilityGrid.cells=states;eligibilityGrid.pilotMask.resize(shape.CellCount(),false);
			for(std::size_t cell=0;cell<shape.CellCount();++cell)
				eligibilityGrid.pilotMask[cell]=!persistence.forceZeroSourceForTest&&
					canonicalPilotMask[cell]!=0u&&
					simulationTimeS<pilotEndS;
			std::vector<bool> eligibility;
			advancedOK=BuildIgnitionEligibility(eligibilityGrid,fuel,fuel,
				FireSimulationTransportRecord::OpenV1(),eligibility,&error);
			if(!advancedOK) break;
			double maximumSpeed=0.0;
			for(unsigned int axis=0;axis<3;++axis) for(const double velocity:
				(advanced.velocityMPerS.component[axis].empty()?momentum.component[axis]:
					advanced.velocityMPerS.component[axis])) maximumSpeed=std::max(maximumSpeed,
					std::fabs(velocity));
			std::vector<ConservativeVector> currentConservative;
			std::vector<double> currentTemperature;
			currentConservative.reserve(shape.CellCount());
			currentTemperature.reserve(shape.CellCount());
			for(const MethaneCellState& cell:states) {
				currentConservative.push_back(ToConservativeVector(cell));
				currentTemperature.push_back(cell.temperatureK);
			}
			OpenMACField3D currentVelocity;
			for(unsigned int axis=0;axis<3;++axis) currentVelocity.component[axis]=
				advanced.velocityMPerS.component[axis].empty()?momentum.component[axis]:
				advanced.velocityMPerS.component[axis];
			std::vector<CellTransportEvaluation> cellTransportEvaluations;
			advancedOK=BuildOpenStageTransportEvaluations3D(shape,currentConservative,
				currentTemperature,currentVelocity,config.openBoundary,config.dns,fuel,
				FireSimulationTransportRecord::OpenV1(),config.transport.producerPrecision,
				cellTransportEvaluations,&error,workerCount);
			if(!advancedOK) break;
			double maximumReducedGravity=0.0,maximumActiveDiffusivity=0.0;
			for(std::size_t cell=0;cell<shape.CellCount();++cell) {
				const CellTransportEvaluation& cellTransport=cellTransportEvaluations[cell];
				if(!ComputeMixingTimeS(states[cell],cellTransport,
					FireSimulationTransportRecord::OpenV1(),shape.cellWidthM,
					ambient.GasDensity(),9.80665,false,reactions[cell].mixingTimeS,&error)) {
					advancedOK=false;break;
				}
				maximumActiveDiffusivity=std::max({maximumActiveDiffusivity,
					cellTransport.totalDiffusivityM2PerS,
					cellTransport.effectiveViscosityPaS/states[cell].GasDensity(),
					cellTransport.effectiveConductivityWPerMK/
						(states[cell].GasDensity()*cellTransport.gasCpJPerKGK)});
				maximumReducedGravity=std::max(maximumReducedGravity,
					std::max(0.0,9.80665*(ambient.GasDensity()-states[cell].GasDensity())/
						states[cell].GasDensity()));
			}
			if(!advancedOK) break;
			// This owner is the binary64 oracle trajectory. Resident production owns
			// and persists its separate (dt,G,r) selector in the r118/r143 path.
			const double chosenStep=FireCase::SelectTimeStepS(shape.cellWidthM,maximumSpeed,
				maximumReducedGravity,maximumActiveDiffusivity,previousStepS);
			if(reportCapstoneProgress && acceptedSteps<4u) std::fprintf(stderr,
				"capstone step-start=%u speed=%.9g gprime=%.9g nu=%.9g selected=%.9g previous=%.9g\n",
				acceptedSteps,maximumSpeed,maximumReducedGravity,maximumActiveDiffusivity,
				chosenStep,previousStepS);
			if(!(chosenStep>0.0)) {advancedOK=false;error="capstone pinned timestep is unbounded";break;}
			double eventStep=chosenStep;
			if(simulationTimeS<pilotRampEndS)eventStep=std::min({eventStep,
				pilotCommandMaximumStepS,pilotRampEndS-simulationTimeS});
			if(simulationTimeS<pilotEndS) eventStep=std::min(eventStep,pilotEndS-simulationTimeS);
			if(simulationTimeS<values.statisticsStartS)
				eventStep=std::min(eventStep,values.statisticsStartS-simulationTimeS);
			if(targetTimeS>simulationTimeS) eventStep=std::min(eventStep,targetTimeS-simulationTimeS);
			if(reportCapstoneProgress&&acceptedSteps<4u)std::fprintf(stderr,
				"capstone event-step=%0.9g pilot_ceiling=%0.9g ramp_end=%0.9g\n",
				eventStep,pilotCommandMaximumStepS,pilotRampEndS);
			for(std::size_t cell=0;cell<shape.CellCount();++cell) {
				reactions[cell].primaryEligible=eligibility[cell];
				reactions[cell].sootOxidationEnabled=true;
			}
			std::vector<ConservativeVector> beginning;
			for(const MethaneCellState& cell:states) beginning.push_back(ToConservativeVector(cell));
			advancedOK=false;
			std::string lastAdvanceError;
			double trialStep=eventStep;
			const FireStateProducerPrecision advanceOutputPrecision=
				FireStateProducerPrecision::Binary64;
			std::vector<double> pilotSetpointTemperatureK(shape.CellCount(),0.0);
			for(unsigned int reduction=0;reduction<20u&&!advancedOK;++reduction) {
				if(injectSolverFailure){lastAdvanceError="injected_solver_failure";break;}
				bool commandOK=true;
				for(std::size_t cell=0;cell<shape.CellCount();++cell) {
					commandOK=commandOK&&(persistence.forceZeroSourceForTest||
						FireCase::EvaluatePilotSetpointTemperatureK(caseRecord.derived,
							canonicalPilotMask[cell]!=0u,simulationTimeS,
							simulationTimeS+trialStep,pilotSetpointTemperatureK[cell],error));
					reactions[cell].deltaTimeS=trialStep;
					reactions[cell].pilotSetpointTemperatureK=pilotSetpointTemperatureK[cell];
					reactions[cell].pilotExpansionVolumeRatioCap=
						pilotSetpointTemperatureK[cell]>0.0?
						caseRecord.derived.pilotExpansionVolumeRatioCap:0.0;
				}
				if(!commandOK){lastAdvanceError=error;break;}
				config.transport.deltaTimeS=trialStep;
				bool packetOK=true;
				if(persistence.forceZeroSourceForTest){
					packets.assign(shape.CellCount(),MethaneSourcePacket());escape=RadiationEscapeFactor();
				}else packetOK=BuildFrozenMethaneSourcePackets(states,reactions,
					std::vector<double>(shape.CellCount(),cellVolume),300.0,
					caseRecord.derived.referenceHeatReleaseRateW,
					caseRecord.derived.effectiveRadiativeFraction,false,fuel,fuel,
					FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1(),packets,escape,&error,
					workerCount);
				advancedOK=packetOK&&AdvanceConservative3D(shape,beginning,momentum,packets,
					config,fuel,fuel,FireSimulationTransportRecord::OpenV1(),advanced,&error);
				if(advancedOK) {
					std::vector<double> trialTemperature;
					advancedOK=InvertPeriodicTemperaturesWithinBounds(advanced.conservative,fuel,
						config.transport.ambientTemperatureK,
						caseRecord.derived.maximumAcceptedTemperatureK,
						advanceOutputPrecision,trialTemperature,&error,
						workerCount);
					if(advancedOK&&std::any_of(trialTemperature.begin(),trialTemperature.end(),
						[&caseRecord](const double temperatureK){return !std::isfinite(temperatureK)||
							temperatureK>=caseRecord.derived.maximumAcceptedTemperatureK;})) {
						advancedOK=false;
						error="accepted_physical_temperature_ceiling_violation";
					}
				}
				if(!advancedOK) {
					lastAdvanceError=error;
					if(reportCapstoneProgress) std::fprintf(stderr,
						"capstone retry reduction=%u dt=%.9g reason=%s\n",reduction,
						trialStep,error.c_str());
					trialStep*=0.5; error.clear();
				}
			}
			if(!advancedOK) error=lastAdvanceError;
			reaction.deltaTimeS=config.transport.deltaTimeS;
			if(advancedOK) {
				const bool checkLimiterIdentity=workerCount>1u&&
					advanced.discontinuousLimiterClassCount>0u&&
					!values.discontinuousClassThreadIdentityChecked;
				const bool checkActiveSetIdentity=workerCount>1u&&
					(advanced.discontinuousActiveSetClassCount>0u||
						persistence.forceActiveSetIdentityCheckForTest)&&
					!values.activeSetThreadIdentityChecked;
				if(checkLimiterIdentity||checkActiveSetIdentity){
					ConservativeAdvance3DConfig serialConfig=config;
					serialConfig.workerCount=1u;
					ConservativeAdvance3DResult serialAdvanced;
					std::string serialError;
					const bool serialOK=AdvanceConservative3D(shape,beginning,momentum,packets,
						serialConfig,fuel,fuel,FireSimulationTransportRecord::OpenV1(),
						serialAdvanced,&serialError);
					bool identical=serialOK&&serialAdvanced.conservative.size()==
						advanced.conservative.size()&&serialAdvanced.faceAlpha==advanced.faceAlpha&&
						serialAdvanced.divergenceHeunPerS==advanced.divergenceHeunPerS&&
						serialAdvanced.maximumLimiterClassDiscrepancy==
							advanced.maximumLimiterClassDiscrepancy&&
						serialAdvanced.discontinuousLimiterClassCount==
							advanced.discontinuousLimiterClassCount&&
						serialAdvanced.maximumActiveSetComplementarityDiscrepancyMPerS==
							advanced.maximumActiveSetComplementarityDiscrepancyMPerS&&
						serialAdvanced.discontinuousActiveSetClassCount==
							advanced.discontinuousActiveSetClassCount&&
						serialAdvanced.maximumActiveSetCycleLength==
							advanced.maximumActiveSetCycleLength&&
						serialAdvanced.maximumActiveSetDifferingFaceCount==
							advanced.maximumActiveSetDifferingFaceCount;
					for(std::size_t cell=0;identical&&cell<advanced.conservative.size();++cell)
						for(std::size_t component=0;component<MethaneConservativeDimension;++component)
							identical=identical&&serialAdvanced.conservative[cell][component]==
								advanced.conservative[cell][component];
					for(unsigned int axis=0;axis<3;++axis)identical=identical&&
						serialAdvanced.momentumKGPerM2S.component[axis]==
							advanced.momentumKGPerM2S.component[axis]&&
						serialAdvanced.velocityMPerS.component[axis]==
							advanced.velocityMPerS.component[axis];
					if(checkActiveSetIdentity&&
						persistence.injectActiveSetIdentityMismatchForTest)identical=false;
					if(checkLimiterIdentity){
						values.discontinuousClassThreadIdentity=identical;
						values.discontinuousClassThreadIdentityChecked=true;
					}
					if(checkActiveSetIdentity){
						values.activeSetThreadIdentity=identical;
						values.activeSetThreadIdentityChecked=true;
					}
					if(!identical&&reportCapstoneProgress)std::fprintf(stderr,
						"capstone discontinuous-class 1-vs-N mismatch: %s\n",
						serialError.c_str());
					advancedOK=DiscontinuousThreadIdentityAccepted(identical,
						checkActiveSetIdentity,error);
				}
				if(!advancedOK)break;
				values.maximumLimiterClassDiscrepancy=std::max(
					values.maximumLimiterClassDiscrepancy,
					advanced.maximumLimiterClassDiscrepancy);
				values.discontinuousLimiterClassSteps+=
					advanced.discontinuousLimiterClassCount;
				values.maximumActiveSetComplementarityDiscrepancyMPerS=std::max(
					values.maximumActiveSetComplementarityDiscrepancyMPerS,
					advanced.maximumActiveSetComplementarityDiscrepancyMPerS);
				values.discontinuousActiveSetEvents+=
					advanced.discontinuousActiveSetClassCount;
				values.maximumActiveSetCycleLength=std::max(
					values.maximumActiveSetCycleLength,advanced.maximumActiveSetCycleLength);
				values.maximumActiveSetDifferingFaceCount=std::max(
					values.maximumActiveSetDifferingFaceCount,
					advanced.maximumActiveSetDifferingFaceCount);
				double expectedStepPilotEnergyJ=0.0;
				for(std::size_t cell=0;cell<shape.CellCount();++cell){
					double pilotEnergyJPerM3=0.0;
					if(!ComputeMethanePilotEnergyDeltaJPerM3(states[cell],fuel,
						pilotSetpointTemperatureK[cell],
						reactions[cell].pilotExpansionVolumeRatioCap,
						pilotEnergyJPerM3,&error)){
						advancedOK=false;break;
					}
					expectedStepPilotEnergyJ+=pilotEnergyJPerM3*cellVolume;
				}
				if(!advancedOK)break;
				const double priorMaximumTemperatureK=values.maximumTemperatureK;
				std::vector<double> acceptedTemperature;
				advancedOK=InvertPeriodicTemperaturesWithinBounds(advanced.conservative,fuel,
					config.transport.ambientTemperatureK,
					caseRecord.derived.maximumAcceptedTemperatureK,
					advanceOutputPrecision,acceptedTemperature,&error,
					workerCount);
				const bool measurePilotApproach=!values.pilotApproachComplete&&
					simulationTimeS<pilotEndS;
				const bool holdPhase=simulationTimeS>=pilotRampEndS&&simulationTimeS<pilotEndS;
				bool allActiveHoldCellsQualified=true,activeHoldCellObserved=false;
				bool allPilotLedgerCeilingsQualified=true;
				double stepAcceptedEOSMaximum=0.0;
				double stepAcceptedMaximumTemperatureK=0.0;
				double heldPilotMinimumTemperatureK=std::numeric_limits<double>::infinity();
				double heldPilotMaximumTemperatureK=0.0;
				std::size_t heldPilotMinimumCell=0u,heldPilotMaximumCell=0u;
				std::vector<MethaneCellState> acceptedStates;
				acceptedStates.reserve(shape.CellCount());
				for(std::size_t acceptedCell=0;advancedOK&&acceptedCell<advanced.conservative.size();
					++acceptedCell) {
					const ConservativeVector& conservative=advanced.conservative[acceptedCell];
					MethaneCellState accepted=FromConservativeVector(conservative,
						advanceOutputPrecision);
					accepted.temperatureK=acceptedTemperature[acceptedCell];
					double acceptedEOSResidual=0.0;
					if(!EquationOfStateResidual(accepted,fuel,acceptedEOSResidual,&error)){
						advancedOK=false;break;
					}
					stepAcceptedEOSMaximum=std::max(stepAcceptedEOSMaximum,acceptedEOSResidual);
					stepAcceptedMaximumTemperatureK=std::max(
						stepAcceptedMaximumTemperatureK,accepted.temperatureK);
					if(measurePilotApproach)values.maximumPilotApproachEOSResidual=std::max(
						values.maximumPilotApproachEOSResidual,acceptedEOSResidual);
					values.maximumTemperatureK=std::max(values.maximumTemperatureK,
						accepted.temperatureK);
					if(pilotSetpointTemperatureK[acceptedCell]>0.0){
						MethanePilotProjectionMap pilotMap;
						if(!ComputeMethanePilotProjectionMap(states[acceptedCell],fuel,
							pilotSetpointTemperatureK[acceptedCell],
							reactions[acceptedCell].pilotExpansionVolumeRatioCap,pilotMap,&error)){
							advancedOK=false;break;
						}
						const bool abovePilotCeiling=
							states[acceptedCell].temperatureK>=caseRecord.derived.pilotSetpointTemperatureK;
						allPilotLedgerCeilingsQualified=allPilotLedgerCeilingsQualified&&
							packets[acceptedCell].pilotEnergyDeltaJPerM3==
								pilotMap.sensibleEnergyDeltaJPerM3&&
							packets[acceptedCell].pilotExpansionIntegral==pilotMap.expansionIntegral&&
							(abovePilotCeiling?
								packets[acceptedCell].pilotEnergyDeltaJPerM3==0.0:
								pilotMap.targetTemperatureK<=caseRecord.derived.pilotSetpointTemperatureK);
						if(holdPhase&&pilotMap.targetTemperatureK==
							caseRecord.derived.pilotSetpointTemperatureK){
							activeHoldCellObserved=true;
							if(accepted.temperatureK<heldPilotMinimumTemperatureK){
								heldPilotMinimumTemperatureK=accepted.temperatureK;
								heldPilotMinimumCell=acceptedCell;
							}
							if(accepted.temperatureK>heldPilotMaximumTemperatureK){
								heldPilotMaximumTemperatureK=accepted.temperatureK;
								heldPilotMaximumCell=acceptedCell;
							}
							allActiveHoldCellsQualified=allActiveHoldCellsQualified&&
								accepted.temperatureK>fuel.PilotTemperatureK();
						}
					}
					acceptedStates.push_back(accepted);
				}
				if(advancedOK){
					states=std::move(acceptedStates);
					config.transport.producerPrecision=advanceOutputPrecision;
					values.maximumAcceptedEOSResidual=std::max(
						values.maximumAcceptedEOSResidual,stepAcceptedEOSMaximum);
					values.acceptedMaximumEOSResidualHistory.push_back(stepAcceptedEOSMaximum);
					values.acceptedMaximumTemperatureHistoryK.push_back(
						stepAcceptedMaximumTemperatureK);
					if(activeHoldCellObserved){
						values.pilotApproachComplete=true;
						values.pilotHoldBandObserved=true;
						values.pilotHoldBandSatisfied=values.pilotHoldBandSatisfied&&
							allActiveHoldCellsQualified;
						if(values.minimumActiveHoldTemperatureK==0.0)
							values.minimumActiveHoldTemperatureK=heldPilotMinimumTemperatureK;
						else values.minimumActiveHoldTemperatureK=std::min(
							values.minimumActiveHoldTemperatureK,heldPilotMinimumTemperatureK);
						values.maximumActiveHoldTemperatureK=std::max(
							values.maximumActiveHoldTemperatureK,heldPilotMaximumTemperatureK);
					}
				}
				if(advancedOK&&!allPilotLedgerCeilingsQualified){
					advancedOK=false;error="pilot_ledger_ceiling_violation";break;
				}
				if(advancedOK&&!values.pilotHoldBandSatisfied){
					std::ostringstream message;message.precision(17);
					message<<"pilot_hold_gate_violation:min="<<heldPilotMinimumTemperatureK<<
						" min_cell="<<heldPilotMinimumCell<<" min_xyz="<<heldPilotMinimumCell%shape.nx<<
						','<<(heldPilotMinimumCell/shape.nx)%shape.ny<<','<<
						heldPilotMinimumCell/(shape.nx*shape.ny)<<
						" max="<<heldPilotMaximumTemperatureK<<" max_cell="<<heldPilotMaximumCell<<
						" max_xyz="<<heldPilotMaximumCell%shape.nx<<','<<
						(heldPilotMaximumCell/shape.nx)%shape.ny<<','<<
						heldPilotMaximumCell/(shape.nx*shape.ny)<<
						" max_T0="<<currentTemperature[heldPilotMaximumCell]<<
						" max_command="<<pilotSetpointTemperatureK[heldPilotMaximumCell]<<
						" max_reacted="<<packets[heldPilotMaximumCell].reactedFuelKGPerM3<<
						" max_qgas="<<packets[heldPilotMaximumCell].gasHeatReleaseWPerM3<<
						" max_pilot_dH="<<packets[heldPilotMaximumCell].pilotEnergyDeltaJPerM3;
					advancedOK=false;error=message.str();break;
				}
				values.expectedPilotEnergyJ+=expectedStepPilotEnergyJ;
				if(reportCapstoneProgress&&values.maximumTemperatureK>
					std::max(2250.0,priorMaximumTemperatureK)) {
					const std::size_t hottest=static_cast<std::size_t>(std::max_element(
						acceptedTemperature.begin(),acceptedTemperature.end())-
						acceptedTemperature.begin());
					std::fprintf(stderr,"capstone new physical peak cell=%zu xyz=%zu,%zu,%zu "
						"T0=%.9g T1=%.9g pilot=%.9g reacted=%.9g qgas=%.9g dt=%.9g\n",
						hottest,hottest%shape.nx,(hottest/shape.nx)%shape.ny,
						hottest/(shape.nx*shape.ny),currentTemperature[hottest],
						acceptedTemperature[hottest],pilotSetpointTemperatureK[hottest],
						packets[hottest].reactedFuelKGPerM3,
						packets[hottest].gasHeatReleaseWPerM3,reaction.deltaTimeS);
				}
				momentum=advanced.momentumKGPerM2S;
				double stepHeatReleaseW=0.0,stepRadiativeLossW=0.0;
				double stepFuelConsumptionKGPerS=0.0,centerlineHeatReleaseW=0.0;
				for(const MethaneSourcePacket& acceptedPacket:packets) {
					values.pilotEnergyJ+=acceptedPacket.pilotEnergyDeltaJPerM3*cellVolume;
					stepHeatReleaseW+=acceptedPacket.gasHeatReleaseWPerM3*cellVolume;
					stepRadiativeLossW+=acceptedPacket.radiativeCoolingWPerM3*cellVolume;
					stepFuelConsumptionKGPerS+=-acceptedPacket.constituentDelta[MethaneCH4]*
						cellVolume/reaction.deltaTimeS;
					if(acceptedPacket.reactedFuelKGPerM3>0.0) {
						if(simulationTimeS<pilotEndS) values.ignitedDuringPilot=true;
						else values.sustainedAfterPilot=true;
					}
				}
				for(std::size_t z=0;z<shape.nz;++z)for(std::size_t cy=0;cy<centerYCount;++cy)
					for(std::size_t cx=0;cx<centerXCount;++cx){const std::size_t center=
						centerXIndex[cx]+shape.nx*(centerYIndex[cy]+shape.ny*z);
						centerlineHeatReleaseW+=packets[center].gasHeatReleaseWPerM3*cellVolume/
							centerSampleCount;}
				const double stepEndS=simulationTimeS+reaction.deltaTimeS;
				if(stepEndS>=pilotEndS&&!values.ignitedDuringPilot) {
					advancedOK=false;
					error="pilot_window_expired_without_ignition";
					break;
				}
				const double statisticsDuration=std::max(0.0,stepEndS-
					std::max(simulationTimeS,values.statisticsStartS));
				if(statisticsDuration>0.0) {
					if(!values.statisticsBoundaryObserved) {
						values.statisticsBoundaryObserved=true;
						values.firstStatisticsStepStartS=simulationTimeS;
					}
					values.integratedHeatReleaseJ+=stepHeatReleaseW*statisticsDuration;
					values.integratedRadiativeLossJ+=stepRadiativeLossW*statisticsDuration;
					values.integratedFuelConsumptionKG+=stepFuelConsumptionKGPerS*statisticsDuration;
					values.probeTimeS.push_back(stepEndS);
					values.probeCenterlineHeatReleaseW.push_back(centerlineHeatReleaseW);
					centerlineStatisticsDurationS+=statisticsDuration;
					for(std::size_t z=0;z<shape.nz;++z){
						double stationTemperatureK=0.0,stationVelocityMPerS=0.0;
						double stationReactionWPerM3=0.0;
						for(std::size_t cy=0;cy<centerYCount;++cy)for(std::size_t cx=0;
							cx<centerXCount;++cx){const std::size_t center=centerXIndex[cx]+
								shape.nx*(centerYIndex[cy]+shape.ny*z);
							stationTemperatureK+=states[center].temperatureK/centerSampleCount;
							stationReactionWPerM3+=packets[center].gasHeatReleaseWPerM3/
								centerSampleCount;
							const std::size_t stationLower=OpenLowerFaceForCell3D(shape,center,2),
								stationUpper=OpenUpperFaceForCell3D(shape,center,2);
							stationVelocityMPerS+=0.5*(advanced.velocityMPerS.component[2][stationLower]+
								advanced.velocityMPerS.component[2][stationUpper])/centerSampleCount;
							centerlineTemperatureIntegral[z]+=states[center].temperatureK*
								statisticsDuration/centerSampleCount;
							const std::size_t lower=OpenLowerFaceForCell3D(shape,center,2),
								upper=OpenUpperFaceForCell3D(shape,center,2);
							centerlineVelocityIntegral[z]+=0.5*(advanced.velocityMPerS.component[2][lower]+
								advanced.velocityMPerS.component[2][upper])*statisticsDuration/
								centerSampleCount;
						}
						values.stationProbeTimeS.push_back(stepEndS);
						values.stationProbeHeightM.push_back((static_cast<double>(z)+0.5)*
							shape.cellWidthM);
						values.stationProbeTemperatureK.push_back(stationTemperatureK);
						values.stationProbeReactionWPerM3.push_back(stationReactionWPerM3);
						values.stationProbeVerticalVelocityMPerS.push_back(stationVelocityMPerS);
						for(std::size_t y=0;y<shape.ny;++y)for(std::size_t x=0;x<shape.nx;++x){
							const std::size_t planeCell=x+shape.nx*(y+shape.ny*z);
							planeHeatReleaseIntegral[z]+=packets[planeCell].gasHeatReleaseWPerM3*
								cellVolume*statisticsDuration;
						}
					}
				}
				values.acceptedTimeStepHistoryS.push_back(reaction.deltaTimeS);
				if(FireProfileEnabled()){
					std::fprintf(stderr,"FIREPROFSTEP step=%u dt=%.17g wall_ms=%.3f\n",
						acceptedSteps+1u,reaction.deltaTimeS,std::chrono::duration<double,std::milli>(
							std::chrono::steady_clock::now()-profileStepStart).count());
					FireProfileReportAndReset("step");
				}
				simulationTimeS+=reaction.deltaTimeS;previousStepS=reaction.deltaTimeS;++acceptedSteps;
				values.acceptedTimeStepS=reaction.deltaTimeS;
				values.simulatedTimeS=simulationTimeS;
				const bool moreWork=acceptedSteps<effectiveMinimumStepCount||simulationTimeS<targetTimeS;
				const double checkpointElapsedS=std::chrono::duration<double>(
					std::chrono::steady_clock::now()-lastCheckpointWall).count();
				const bool equivalenceSnapshotDue=
					!persistence.equivalenceSnapshotDirectory.empty();
				const bool finalCheckpointDue=!moreWork&&!persistence.finalCheckpointPath.empty();
				const bool checkpointDue=equivalenceSnapshotDue||finalCheckpointDue||
					(moreWork&&!persistence.checkpointPath.empty()&&
						(persistence.checkpointCadenceWallS<=0.0||
							checkpointElapsedS>=persistence.checkpointCadenceWallS));
				if(checkpointDue){
					values.checkpointStepIndices.push_back(acceptedSteps);
					MethaneRunCheckpoint checkpoint;
					checkpoint.caseRecordId=caseRecord.caseRecordId;
					checkpoint.producerBuildId=currentBuildId;
					checkpoint.dimensions={{shape.nx,shape.ny,shape.nz}};
					checkpoint.cellWidthM=shape.cellWidthM;checkpoint.states=std::move(states);
					checkpoint.momentum=std::move(momentum);
					checkpoint.velocity=std::move(advanced.velocityMPerS);
					checkpoint.values=std::move(values);
					checkpoint.centerlineTemperatureIntegral=std::move(centerlineTemperatureIntegral);
					checkpoint.centerlineVelocityIntegral=std::move(centerlineVelocityIntegral);
					checkpoint.planeHeatReleaseIntegral=std::move(planeHeatReleaseIntegral);
					checkpoint.centerlineStatisticsDurationS=centerlineStatisticsDurationS;
					checkpoint.simulationTimeS=simulationTimeS;
					checkpoint.previousStepS=previousStepS;
					checkpoint.lastAcceptedStepS=reaction.deltaTimeS;
					checkpoint.acceptedSteps=acceptedSteps;
					std::filesystem::path checkpointOutput=finalCheckpointDue?
						persistence.finalCheckpointPath:persistence.checkpointPath;
					if(equivalenceSnapshotDue){
						std::ostringstream snapshotName;snapshotName<<"step_"<<std::setw(2)<<
							std::setfill('0')<<(acceptedSteps-values.resumedFromStep)<<".checkpoint";
						checkpointOutput=persistence.equivalenceSnapshotDirectory/snapshotName.str();
					}
					const bool checkpointSaved=SaveMethaneRunCheckpoint(
						checkpointOutput,checkpoint,error);
					states=std::move(checkpoint.states);momentum=std::move(checkpoint.momentum);
					advanced.velocityMPerS=std::move(checkpoint.velocity);
					values=std::move(checkpoint.values);
					centerlineTemperatureIntegral=std::move(checkpoint.centerlineTemperatureIntegral);
					centerlineVelocityIntegral=std::move(checkpoint.centerlineVelocityIntegral);
					planeHeatReleaseIntegral=std::move(checkpoint.planeHeatReleaseIntegral);
					if(!checkpointSaved){
						advancedOK=false;break;
					}
					lastCheckpointWall=std::chrono::steady_clock::now();
					if(reportCapstoneProgress)std::fprintf(stderr,
						"capstone durable checkpoint count=%zu step=%u time=%.17g path=%s\n",
						values.checkpointStepIndices.size(),acceptedSteps,simulationTimeS,
						checkpointOutput.string().c_str());
					if(persistence.killAfterFirstCheckpoint)HardKillCurrentProcess();
				}
				if(reportCapstoneProgress && (acceptedSteps<=4u || acceptedSteps%10u==0u ||
					simulationTimeS>=targetTimeS)) {
					double maximumTemperatureK=0.0,maximumReactionWPerM3=0.0;
					for(std::size_t diagnosticCell=0;diagnosticCell<states.size();++diagnosticCell) {
						maximumTemperatureK=std::max(maximumTemperatureK,
							states[diagnosticCell].temperatureK);
						maximumReactionWPerM3=std::max(maximumReactionWPerM3,
							packets[diagnosticCell].gasHeatReleaseWPerM3);
					}
					std::fprintf(stderr,"capstone accepted step=%u time=%.9g dt=%.9g Tmax=%.9g "
						"qmax=%.9g eos_max=%.9g approach_eos_max=%.9g limiter_class=%s "
						"limiter_discrepancy=%.9g active_set_class=%s active_set_discrepancy=%.9g "
						"active_set_cycle=%zu active_set_faces=%zu\n",acceptedSteps,
						simulationTimeS,reaction.deltaTimeS,maximumTemperatureK,
						maximumReactionWPerM3,values.maximumAcceptedEOSResidual,
						values.maximumPilotApproachEOSResidual,
						advanced.discontinuousLimiterClassCount?
						"discontinuous":"continuous",advanced.maximumLimiterClassDiscrepancy,
						advanced.discontinuousActiveSetClassCount?
						"discontinuous":"continuous",
						advanced.maximumActiveSetComplementarityDiscrepancyMPerS,
						advanced.maximumActiveSetCycleLength,
						advanced.maximumActiveSetDifferingFaceCount);
				}
			}
		}
		if(!advancedOK) {
			values.structuredError="solver_failure:"+error;
			std::fprintf(stderr,"capstone solver diagnostic steps=%u time=%.17g: %s\n",
				acceptedSteps,simulationTimeS,values.structuredError.c_str());
			if(!values.acceptedMaximumEOSResidualHistory.empty()){
				std::fprintf(stderr,"capstone accepted EOS drift history tail=");
				const std::size_t first=values.acceptedMaximumEOSResidualHistory.size()>8u?
					values.acceptedMaximumEOSResidualHistory.size()-8u:0u;
				for(std::size_t sample=first;sample<values.acceptedMaximumEOSResidualHistory.size();
					++sample)std::fprintf(stderr,"%s%.9g",sample==first?"":",",
						values.acceptedMaximumEOSResidualHistory[sample]);
				std::fprintf(stderr,"\n");
			}
			return values;
		}
		if(reportCapstoneProgress&&advancedOK) {
			double pilotMaximumTemperatureK=0.0,pilotMaximumMethaneKGPerM3=0.0,
				pilotMaximumOxygenKGPerM3=0.0;
			for(std::size_t cell=0;cell<states.size();++cell) if(canonicalPilotMask[cell]) {
				pilotMaximumTemperatureK=std::max(pilotMaximumTemperatureK,
					states[cell].temperatureK);
				pilotMaximumMethaneKGPerM3=std::max(pilotMaximumMethaneKGPerM3,
					states[cell].constituent[MethaneCH4]);
				pilotMaximumOxygenKGPerM3=std::max(pilotMaximumOxygenKGPerM3,
					states[cell].constituent[MethaneO2]);
			}
			std::fprintf(stderr,"capstone pilot diagnostic Tmax=%.9g CH4max=%.9g O2max=%.9g "
				"eos_max=%.9g approach_eos_max=%.9g\n",
				pilotMaximumTemperatureK,pilotMaximumMethaneKGPerM3,
				pilotMaximumOxygenKGPerM3,values.maximumAcceptedEOSResidual,
				values.maximumPilotApproachEOSResidual);
		}
		values.succeeded=true;
		Check(advanced.conservative.empty() ||
			advanced.effectiveWorkerCount==std::max(1u,std::min(workerCount,
			static_cast<unsigned int>(shape.CellCount()))),
			"capstone one-vs-N fixture selects the owning solver worker path");
		if(advancedOK) {
			values.dimensions={{shape.nx,shape.ny,shape.nz}}; values.cellWidthM=shape.cellWidthM;
			values.caseRecordId=caseRecord.caseRecordId;
			values.temperature.resize(shape.CellCount());
			values.reaction.resize(shape.CellCount());
			values.carbon.resize(shape.CellCount());
			values.velocity.resize(shape.CellCount());
			for(std::size_t cell=0;cell<shape.CellCount();++cell) {
				const MethaneCellState& accepted=states[cell];
				values.temperature[cell]=static_cast<float>(accepted.temperatureK);
				values.reaction[cell]=static_cast<float>(packets.empty()?0.0:
					packets[cell].gasHeatReleaseWPerM3);
				values.carbon[cell]=static_cast<float>(std::max(0.0,
					accepted.constituent[MethaneCarbon]));
				std::array<float,3> velocity={{0.0f,0.0f,0.0f}};
				for(unsigned int axis=0;axis<3&&!advanced.velocityMPerS.component[axis].empty();++axis) {
					const std::size_t lower=OpenLowerFaceForCell3D(shape,cell,axis);
					const std::size_t upper=OpenUpperFaceForCell3D(shape,cell,axis);
					velocity[axis]=static_cast<float>(0.5*(advanced.velocityMPerS.component[axis][lower]+
						advanced.velocityMPerS.component[axis][upper]));
				}
				values.velocity[cell]=velocity;
				if(!packets.empty()) {
					values.realizedHeatReleaseW+=packets[cell].gasHeatReleaseWPerM3*cellVolume;
					values.fuelConsumptionKGPerS+=-packets[cell].constituentDelta[MethaneCH4]*
						cellVolume/reaction.deltaTimeS;
				}
			}
			values.temperatureK=*std::max_element(values.temperature.begin(),values.temperature.end());
			values.reactionWPerM3=*std::max_element(values.reaction.begin(),values.reaction.end());
			values.acceptedEscapeFactor=escape.accepted;
			values.selectedTimeStepS=selectedStep.seconds;
			values.acceptedTimeStepS=reaction.deltaTimeS;
			values.simulatedTimeS=simulationTimeS;
			values.flowThroughTimeS=caseRecord.derived.flowThroughTimeS;
			double coolingW=0.0;
			for(const MethaneSourcePacket& packet:packets)
				coolingW+=packet.radiativeCoolingWPerM3*cellVolume;
			values.realizedRadiativeFraction=values.realizedHeatReleaseW>0.0?
				coolingW/values.realizedHeatReleaseW:0.0;
			values.integratedRadiativeFraction=values.integratedHeatReleaseJ>0.0?
				values.integratedRadiativeLossJ/values.integratedHeatReleaseJ:0.0;
			values.puffingFrequencyHz=DominantUniformResampledFrequency(values.probeTimeS,
				values.probeCenterlineHeatReleaseW);
			const double expectedPuffing=1.5/std::sqrt(CapstonePoolDiameterM);
			values.puffingRelativeError=expectedPuffing>0.0?
				std::fabs(values.puffingFrequencyHz-expectedPuffing)/expectedPuffing:0.0;
			double peakMeanPlaneHeatReleaseW=0.0;
			for(const double integral:planeHeatReleaseIntegral)peakMeanPlaneHeatReleaseW=
				std::max(peakMeanPlaneHeatReleaseW,centerlineStatisticsDurationS>0.0?
					integral/centerlineStatisticsDurationS:0.0);
			if(peakMeanPlaneHeatReleaseW>0.0)for(std::size_t z=0;z<shape.nz;++z)if(
				planeHeatReleaseIntegral[z]/centerlineStatisticsDurationS>=
					0.01*peakMeanPlaneHeatReleaseW)values.mccaffreyFlameTipHeightM=
					(static_cast<double>(z)+0.5)*shape.cellWidthM;
			for(std::size_t z=0;z<shape.nz;++z) {
				double fallbackTemperature=0.0,fallbackVelocity=0.0;
				for(std::size_t cy=0;cy<centerYCount;++cy)for(std::size_t cx=0;
					cx<centerXCount;++cx){const std::size_t center=centerXIndex[cx]+
						shape.nx*(centerYIndex[cy]+shape.ny*z);
					fallbackTemperature+=states[center].temperatureK/centerSampleCount;
					fallbackVelocity+=values.velocity[center][2]/centerSampleCount;}
				values.centerlineHeightM.push_back((static_cast<double>(z)+0.5)*shape.cellWidthM);
				values.centerlineTemperatureK.push_back(centerlineStatisticsDurationS>0.0?
					centerlineTemperatureIntegral[z]/centerlineStatisticsDurationS:
					fallbackTemperature);
				values.centerlineVelocityMPerS.push_back(centerlineStatisticsDurationS>0.0?
					centerlineVelocityIntegral[z]/centerlineStatisticsDurationS:
					fallbackVelocity);
			}
			FitCenterlineTemperaturePowerLaw(values);
			if(heatReleaseRateKW==CapstoneHeatReleaseRateKW)
				EvaluateMcCaffreyPlumeStations(values,heatReleaseRateKW);
		}
		return values;
	}

	RISECBOR64::Bytes CanonicalRecord( const char* kind )
	{
		RISECBOR64::Bytes bytes;
		std::string error;
		Check(RISECBOR64::Encode(RISECBOR64::Value::MapValue({
			{"record_kind",RISECBOR64::Value::String(kind)},
			{"schema_version",RISECBOR64::Value::Unsigned(1)}
		}),bytes,&error),"synthetic embedded record encodes canonically");
		return bytes;
	}

	RISECBOR64::Bytes AerosolRecord()
	{
		RISECBOR64::Bytes bytes; std::string error;
		const FireSimulationMethaneRecord& fuel=FireSimulationMethaneRecord::PhysicalV1();
		Check(RISECBOR64::Encode(RISECBOR64::Value::MapValue({
			{"carbon_phase",RISECBOR64::Value::MapValue({
				{"common_T_ref_K",RISECBOR64::Value::Float(fuel.ReferenceTemperatureK())},
				{"interpolation",RISECBOR64::Value::String("nasa9_piecewise_cp_hs")},
				{"phase",RISECBOR64::Value::String("solid")},
				{"source_fuel_record_id",RISECBOR64::Value::String(fuel.RecordId())},
				{"species_id",RISECBOR64::Value::String("C(gr)")}
			})},
			{"condensable_stream",RISECBOR64::Value::MapValue({
				{"kind",RISECBOR64::Value::String("none")},
				{"reason",RISECBOR64::Value::String("methane_has_no_condensable_organic_stream")}
			})},
			{"record_kind",RISECBOR64::Value::String("fire-aerosol-thermochemistry-v1")},
			{"schema_version",RISECBOR64::Value::Unsigned(1)},
			{"status",RISECBOR64::Value::String("preview_methane_zero_yield")},
			{"temperature_domain_K",RISECBOR64::Value::ArrayValue({
				RISECBOR64::Value::Float(fuel.TemperatureMinK()),
				RISECBOR64::Value::Float(fuel.TemperatureMaxK())})}
		}),bytes,&error),"aerosol record encodes");
		return bytes;
	}

	RISECBOR64::Bytes ChemNoneRecord()
	{
		RISECBOR64::Bytes bytes; std::string error;
		Check(RISECBOR64::Encode(RISECBOR64::Value::MapValue({
			{"chem_model",RISECBOR64::Value::String("none")},
			{"provenance",RISECBOR64::Value::String("methane r52 no adopted chem record")},
			{"record_kind",RISECBOR64::Value::String("fire-chem-none-v1")},
			{"schema_version",RISECBOR64::Value::Unsigned(1)}
		}),bytes,&error),"chem-none record encodes");
		return bytes;
	}

	RISECBOR64::Bytes SyntheticChemRecord()
	{
		using RISECBOR64::Value;
		RISECBOR64::Bytes bytes; std::string error;
		Value::Values bands;
		const char* names[3]={"CH","C2","CO2"};
		const double limits[3][2]={{390.0,440.0},{450.0,570.0},{380.0,780.0}};
		for( unsigned int band=0; band<3u; ++band ) bands.push_back(Value::MapValue({
			{"band",Value::String(names[band])},
			{"normalization_interval_nm",Value::ArrayValue({
				Value::Float(limits[band][0]),Value::Float(limits[band][1])})},
			{"normalization_rule",Value::String("trapezoid_1nm_then_divide_once")},
			{"spd_shape",Value::String("uniform_unit_shape")}
		}));
		Check(RISECBOR64::Encode(Value::MapValue({
			{"absolute_calibration",Value::String("input_absolute_band_power_W_per_m3")},
			{"bands",Value::ArrayValue(bands)},
			{"provenance",Value::String("test-only analytic uniform-SPD estimator fixture")},
			{"record_class",Value::String("SYNTHETIC_NON_PREDICTIVE")},
			{"record_kind",Value::String("fire-chem-synthetic-fixture-v1")},
			{"schema_version",Value::Unsigned(1)},
			{"state_domain",Value::String("finite_nonnegative_absolute_channel_values")},
			{"wavelength_unit",Value::String("nm")}
		}),bytes,&error),"synthetic chem fixture record encodes");
		return bytes;
	}

	RISECBOR64::Value ReplaceMember( const RISECBOR64::Value& map,
		const char* key, const RISECBOR64::Value& replacement )
	{
		RISECBOR64::Value::Members members = map.GetMap();
		for( auto& member : members ) if( member.first == key ) member.second = replacement;
		return RISECBOR64::Value::MapValue(members);
	}

	RISECBOR64::Bytes EnvelopeForPayload( const RISECBOR64::Value& payload )
	{
		RISECBOR64::Bytes payloadBytes, envelope;
		std::string error;
		Check(RISECBOR64::Encode(payload,payloadBytes,&error),"mutated payload encodes");
		Check(RISECBOR64::Encode(RISECBOR64::Value::MapValue({
			{"payload",payload},{"sequence_id",RISECBOR64::Value::String(
				RISECBOR64::SHA256Hex(payloadBytes))}}),envelope,&error),"mutated envelope encodes");
		return envelope;
	}

	RISECBOR64::Value Channel( const char* name, const char* type,
		const char* units, const char* semantics, const std::vector<double>& background,
		const std::array<std::uint64_t,3> scalarDimensions={{2u,2u,2u}},
		const double voxelSize=0.5 )
	{
		using RISECBOR64::Value;
		Value::Values bg;
		for( const double value : background ) bg.push_back(Value::Float(value));
		const bool velocity = std::strcmp(name,"velocity") == 0;
		const double origin=velocity?-voxelSize:0.0;
		const double lower=-0.5*voxelSize;
		return Value::MapValue({
			{"background_value",Value::ArrayValue(bg)},
			{"core_face_bounds_m",Value::ArrayValue({Value::Float(lower),Value::Float(lower),
				Value::Float(lower),Value::Float((scalarDimensions[0]-0.5)*voxelSize),
				Value::Float((scalarDimensions[1]-0.5)*voxelSize),
				Value::Float((scalarDimensions[2]-0.5)*voxelSize)})},
			{"dimensions",Value::ArrayValue({Value::Unsigned(scalarDimensions[0]+(velocity?2u:0u)),
				Value::Unsigned(scalarDimensions[1]+(velocity?2u:0u)),
				Value::Unsigned(scalarDimensions[2]+(velocity?2u:0u))})},
			{"name",Value::String(name)},
			{"origin_m",Value::ArrayValue({Value::Float(origin),Value::Float(origin),Value::Float(origin)})},
			{"temporal_semantics",Value::String(semantics)},
			{"units",Value::String(units)},
			{"value_type",Value::String(type)},
			{"voxel_size_m",Value::ArrayValue({Value::Float(voxelSize),Value::Float(voxelSize),
				Value::Float(voxelSize)})}
		});
	}

	RISECBOR64::Bytes ManifestBytes( const std::string& firstDigest,
		const std::string& secondDigest, const char* endPolicy="hold",
		const bool useProductionOptics=false, const bool syntheticChem=false,
		const std::array<std::uint64_t,3> scalarDimensions={{2u,2u,2u}},
		const double voxelSize=0.5,
		const double simulationTimeOrigin=1.0, const double frameStepSeconds=0.25,
		const double sceneToSimulationScale=2.0, const double sceneTimeOrigin=10.0,
		const bool caseGridBound=false, const double casePoolDiameterM=0.02,
		const double caseHeatReleaseRateKW=0.10,
		const bool caseHasRadiativeFractionOverride=false,
		const double caseRadiativeFractionOverride=0.0,
		const double caseResolutionTier=6.0,const bool casePlumeLaw=false )
	{
		using RISECBOR64::Value;
		std::string error;
		RISECBOR64::Bytes build; std::string buildId;
		Check(CurrentRendererBuildIdentity(build,buildId),
			"sequence producer embeds the current executable build identity");
		const RISECBOR64::Bytes optics = FireOpticsPreset::PredictiveV1().RecordBytes();
		const RISECBOR64::Bytes thermo =
			FireSimulationThermochemistryRecord::OpenSubsetV1().RecordBytes();
		const RISECBOR64::Bytes transport =
			FireSimulationTransportRecord::OpenV1().RecordBytes();
		const RISECBOR64::Bytes aerosol = AerosolRecord();
		const RISECBOR64::Bytes opacity =
			FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1().RecordBytes();
		const RISECBOR64::Bytes chem = syntheticChem ? SyntheticChemRecord() : ChemNoneRecord();
		const RISECBOR64::Bytes fuel = FireSimulationMethaneRecord::PhysicalV1().RecordBytes();
		const RISECBOR64::Bytes gateEvidenceBytes={'p','h','a','s','e','_','c','_','r','5','3'};
		const std::string gateEvidenceId=RISECBOR64::SHA256Hex(gateEvidenceBytes);
		FireCase::AuthoredV1 caseAuthored;
		caseAuthored.fuelRecordId=FireSimulationMethaneRecord::PhysicalV1().RecordId();
		caseAuthored.poolDiameterM=casePoolDiameterM;
		caseAuthored.heatReleaseRateKW=caseHeatReleaseRateKW;
		caseAuthored.envelope={{0.0,1.0}}; caseAuthored.durationS=std::max(1.0,frameStepSeconds);
		caseAuthored.quality=caseGridBound?"dstar":"standard";
		caseAuthored.numericDStarTier=caseGridBound?caseResolutionTier:0.0; caseAuthored.seed=1234;
		caseAuthored.outputFramesPerS=1.0/frameStepSeconds;
		caseAuthored.plumeLaw=casePlumeLaw;
		caseAuthored.hasRadiativeFractionOverride=caseHasRadiativeFractionOverride;
		caseAuthored.radiativeFractionOverride=caseRadiativeFractionOverride;
		FireCase::RecordV1 caseRecord;
		const bool caseBuilt=FireCase::BuildMethaneV1(caseAuthored,
			FireSimulationMethaneRecord::PhysicalV1(),
			{RISECBOR64::SHA256Hex(thermo),RISECBOR64::SHA256Hex(transport),
			 RISECBOR64::SHA256Hex(opacity),RISECBOR64::SHA256Hex(optics),
			 RISECBOR64::SHA256Hex(aerosol),RISECBOR64::SHA256Hex(chem),
			 RISECBOR64::SHA256Hex(fuel)},caseRecord,error);
		Check(caseBuilt,"test sequence case record derives canonically");
		Value::Values channels={
			Channel("carbon","float32","g/m3","frozen_material_advection",{0.0},scalarDimensions,voxelSize),
			Channel("temperature","float32","K","frozen_material_advection",{300.0},scalarDimensions,voxelSize),
			Channel("reaction","float32","W/m3","derived_eulerian_source",{0.0},scalarDimensions,voxelSize),
			Channel("velocity","vec3_float32","m/s","frozen_material_advection",{0.0,0.0,0.0},scalarDimensions,voxelSize)
		};
		if( syntheticChem ) {
			channels.push_back(Channel("chem_CH","float32","W/m3","derived_eulerian_source",{0.0},scalarDimensions,voxelSize));
			channels.push_back(Channel("chem_C2","float32","W/m3","derived_eulerian_source",{0.0},scalarDimensions,voxelSize));
			channels.push_back(Channel("chem_CO2","float32","W/m3","derived_eulerian_source",{0.0},scalarDimensions,voxelSize));
		}
		const bool preview=useProductionOptics || syntheticChem;
		const Value payload = Value::MapValue({
			{"aerosol_thermochemistry_record",Value::BytesValue(aerosol)},
			{"aerosol_thermochemistry_record_id",Value::String(RISECBOR64::SHA256Hex(aerosol))},
			{"case_record",Value::BytesValue(caseRecord.envelopeBytes)},
			{"case_record_id",Value::String(caseRecord.caseRecordId)},
			{"channels",Value::ArrayValue(channels)},
			{"chem_record",Value::BytesValue(chem)},
			{"chem_record_id",Value::String(RISECBOR64::SHA256Hex(chem))},
			{"end_policy",Value::String(endPolicy)},
			{"first_frame_index",Value::Unsigned(4)},
			{"frame_count",Value::Unsigned(2)},
			{"frame_encoding",Value::String("openvdb-v1")},
			{"frames",Value::ArrayValue({
				Value::MapValue({{"index",Value::Unsigned(4)},
					{"path",Value::String("frame4.vdb")},{"sha256",Value::String(firstDigest)}}),
				Value::MapValue({{"index",Value::Unsigned(5)},
					{"path",Value::String("frame5.vdb")},{"sha256",Value::String(secondDigest)}})
			})},
			{"fuel_record",Value::BytesValue(fuel)},
			{"fuel_record_id",Value::String(RISECBOR64::SHA256Hex(fuel))},
			{"gas_opacity_record",Value::BytesValue(opacity)},
			{"gas_opacity_record_id",Value::String(RISECBOR64::SHA256Hex(opacity))},
			{"gas_thermochemistry_record",Value::BytesValue(thermo)},
			{"gas_thermochemistry_record_id",Value::String(RISECBOR64::SHA256Hex(thermo))},
			{"gate_evidence_ids",Value::ArrayValue({Value::String(gateEvidenceId)})},
			{"last_frame_index",Value::Unsigned(5)},
			{"optical_record",Value::BytesValue(optics)},
			{"optical_record_id",Value::String(RISECBOR64::SHA256Hex(optics))},
			{"outside_halo_policy",Value::String("reject_outside_declared_halo")},
			{"physical_mapping",Value::String("absolute_si")},
			{"producer_build_id",Value::String(buildId)},
			{"producer_build_v1",Value::BytesValue(build)},
			{"producer_reason_codes",Value::ArrayValue(preview ? (caseGridBound ? Value::Values{
				Value::String("case_grid_bound"),Value::String(syntheticChem ? "synthetic_chem_fixture" :
					"open_subset_records_preview")} : Value::Values{Value::String(syntheticChem ?
					"synthetic_chem_fixture":"open_subset_records_preview")}) : Value::Values{})},
			{"scene_translation_m",Value::ArrayValue({Value::Float(0),Value::Float(0),Value::Float(0)})},
			{"qdot_ref_W",Value::Float(caseRecord.derived.referenceHeatReleaseRateW)},
			{"scene_unit_meters",Value::Float(1.0)},
			{"schema_version",Value::Unsigned(1)},
			{"source_kind",Value::String("rise_simulation")},
			{"source_qualification",Value::String(preview ? "preview_only" : "predictive_qualified")},
			{"temperature_domain_K",Value::ArrayValue({Value::Float(300),Value::Float(2500)})},
			{"time_map",Value::MapValue({
				{"alpha",Value::Float(sceneToSimulationScale)},
				{"delta_t_frame",Value::Float(frameStepSeconds)},
				{"i0",Value::Unsigned(4)},
				{"t0",Value::Float(simulationTimeOrigin)},
				{"t_scene_0",Value::Float(sceneTimeOrigin)}
			})},
			{"transport_closure_record",Value::BytesValue(transport)},
			{"transport_closure_record_id",Value::String(RISECBOR64::SHA256Hex(transport))},
			{"velocity_halo_width_m",Value::Float(voxelSize)}
		});
		RISECBOR64::Bytes payloadBytes, envelope;
		Check(RISECBOR64::Encode(payload,payloadBytes,&error),"sequence payload encodes");
		Check(RISECBOR64::Encode(Value::MapValue({
			{"payload",payload},
			{"sequence_id",Value::String(RISECBOR64::SHA256Hex(payloadBytes))}
		}),envelope,&error),"sequence envelope encodes");
		return envelope;
	}

	std::string DigestFile( const std::filesystem::path& path )
	{
		std::ifstream input(path,std::ios::binary);
		if(!input)return std::string();
		input.seekg(0,std::ios::end);
		const std::streampos end = input.tellg();
		if(end<0)return std::string();
		input.seekg(0,std::ios::beg);
		RISECBOR64::Bytes bytes(static_cast<std::size_t>(end));
		if( end > 0 ) input.read(reinterpret_cast<char*>(bytes.data()),end);
		return RISECBOR64::SHA256Hex(bytes);
	}

	bool CurrentExecutableDigest(const RISECBOR64::Bytes& buildRecord,
		std::string& digest,std::string& error)
	{
		RISECBOR64::Value decoded;
		if(!RISECBOR64::DecodeCanonical(buildRecord,decoded,&error)||
			decoded.GetType()!=RISECBOR64::Value::Map)return false;
		const RISECBOR64::Value* renderer=decoded.Find("renderer_binary");
		const RISECBOR64::Value* sha=renderer&&renderer->GetType()==RISECBOR64::Value::Map?
			renderer->Find("sha256"):nullptr;
		if(!sha||sha->GetType()!=RISECBOR64::Value::Text||sha->GetText().size()!=64u){
			error="renderer build record has no executable SHA-256";return false;
		}
		digest=sha->GetText();return true;
	}

	RISECBOR64::Value UnsignedArrayValue(const std::vector<std::uint64_t>& values)
	{
		RISECBOR64::Value::Values encoded;encoded.reserve(values.size());
		for(const std::uint64_t value:values)encoded.push_back(RISECBOR64::Value::Unsigned(value));
		return RISECBOR64::Value::ArrayValue(encoded);
	}
	RISECBOR64::Value TextArrayValue(const std::vector<std::string>& values)
	{
		RISECBOR64::Value::Values encoded;encoded.reserve(values.size());
		for(const std::string& value:values)encoded.push_back(RISECBOR64::Value::String(value));
		return RISECBOR64::Value::ArrayValue(encoded);
	}
	bool ReadTextMember(const RISECBOR64::Value& map,const char* key,std::string& value)
	{
		const RISECBOR64::Value* found=map.Find(key);
		if(!found||found->GetType()!=RISECBOR64::Value::Text)return false;
		value=found->GetText();return true;
	}
	bool ReadUnsignedMember(const RISECBOR64::Value& map,const char* key,std::uint64_t& value)
	{
		const RISECBOR64::Value* found=map.Find(key);
		if(!found||found->GetType()!=RISECBOR64::Value::UnsignedInteger)return false;
		value=found->GetIntegerArgument();return true;
	}
	bool ReadUnsignedArray(const RISECBOR64::Value& map,const char* key,
		std::vector<std::uint64_t>& values)
	{
		const RISECBOR64::Value* found=map.Find(key);
		if(!found||found->GetType()!=RISECBOR64::Value::Array)return false;
		values.clear();values.reserve(found->GetArray().size());
		for(const RISECBOR64::Value& item:found->GetArray()){
			if(item.GetType()!=RISECBOR64::Value::UnsignedInteger)return false;
			values.push_back(item.GetIntegerArgument());
		}
		return true;
	}
	bool ReadTextArray(const RISECBOR64::Value& map,const char* key,
		std::vector<std::string>& values)
	{
		const RISECBOR64::Value* found=map.Find(key);
		if(!found||found->GetType()!=RISECBOR64::Value::Array)return false;
		values.clear();values.reserve(found->GetArray().size());
		for(const RISECBOR64::Value& item:found->GetArray()){
			if(item.GetType()!=RISECBOR64::Value::Text)return false;
			values.push_back(item.GetText());
		}
		return true;
	}

	bool DurableWriteCanonical(const std::filesystem::path& path,
		const RISECBOR64::Bytes& bytes,std::string& error)
	{
		if(path.has_parent_path())std::filesystem::create_directories(path.parent_path());
#if defined(_WIN32)
		const long long processId=static_cast<long long>(::_getpid());
#else
		const long long processId=static_cast<long long>(::getpid());
#endif
		const std::filesystem::path temporary=path.string()+".tmp."+std::to_string(processId);
		{
			std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
			if(!output){error="cannot open canonical run record";return false;}
			if(!bytes.empty())output.write(reinterpret_cast<const char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
			output.close();if(!output){error="cannot write canonical run record";return false;}
		}
		if(!DurableSyncFileAndDirectory(temporary,error)||
			!AtomicReplaceCheckpoint(temporary,path,error)){
			std::error_code ignored;std::filesystem::remove(temporary,ignored);return false;
		}
		return true;
	}

	RISECBOR64::Value ResumeTracePayload(const ResumeEquivalenceTrace& trace)
	{
		using RISECBOR64::Value;
		return Value::MapValue({
			{"accepted_step_count",Value::Unsigned(trace.acceptedStepCount)},
			{"build_id",Value::String(trace.buildId)},
			{"checkpoint_producer_build_id",Value::String(trace.checkpointProducerBuildId)},
			{"checkpoint_sha256",Value::String(trace.checkpointDigest)},
			{"executable_sha256",Value::String(trace.executableDigest)},
			{"frame_sha256",TextArrayValue(trace.frameDigests)},
			{"maximum_eos_residual_bits",UnsignedArrayValue(trace.maximumEOSResidualBits)},
			{"maximum_temperature_K_bits",UnsignedArrayValue(trace.maximumTemperatureBits)},
			{"record_kind",Value::String("fire-resume-equivalence-trace-v1")},
			{"resumed_from_step",Value::Unsigned(trace.resumedFromStep)},
			{"schema_version",Value::Unsigned(1)},
			{"time_step_s_bits",UnsignedArrayValue(trace.timeStepBits)}
		});
	}
	bool ParseResumeTracePayload(const RISECBOR64::Value& payload,
		ResumeEquivalenceTrace& trace)
	{
		std::string kind;std::uint64_t schema=0u;
		return payload.GetType()==RISECBOR64::Value::Map&&payload.GetMap().size()==12u&&
			ReadUnsignedMember(payload,"accepted_step_count",trace.acceptedStepCount)&&
			ReadTextMember(payload,"build_id",trace.buildId)&&
			ReadTextMember(payload,"checkpoint_producer_build_id",trace.checkpointProducerBuildId)&&
			ReadTextMember(payload,"checkpoint_sha256",trace.checkpointDigest)&&
			ReadTextMember(payload,"executable_sha256",trace.executableDigest)&&
			ReadTextArray(payload,"frame_sha256",trace.frameDigests)&&
			ReadUnsignedArray(payload,"maximum_eos_residual_bits",trace.maximumEOSResidualBits)&&
			ReadUnsignedArray(payload,"maximum_temperature_K_bits",trace.maximumTemperatureBits)&&
			ReadTextMember(payload,"record_kind",kind)&&kind=="fire-resume-equivalence-trace-v1"&&
			ReadUnsignedMember(payload,"resumed_from_step",trace.resumedFromStep)&&
			ReadUnsignedMember(payload,"schema_version",schema)&&schema==1u&&
			ReadUnsignedArray(payload,"time_step_s_bits",trace.timeStepBits);
	}
	bool SaveResumeEquivalenceTrace(const std::filesystem::path& path,
		const ResumeEquivalenceTrace& trace,std::string& error)
	{
		const RISECBOR64::Value payload=ResumeTracePayload(trace);
		RISECBOR64::Bytes payloadBytes,envelope;
		if(!RISECBOR64::Encode(payload,payloadBytes,&error)||
			!RISECBOR64::Encode(RISECBOR64::Value::MapValue({{"payload",payload},
				{"trace_id",RISECBOR64::Value::String(RISECBOR64::SHA256Hex(payloadBytes))}}),
				envelope,&error))return false;
		return DurableWriteCanonical(path,envelope,error);
	}
	bool LoadResumeEquivalenceTrace(const std::filesystem::path& path,
		ResumeEquivalenceTrace& trace,std::string& error)
	{
		const RISECBOR64::Bytes bytes=ReadFileBytes(path);RISECBOR64::Value envelope;
		if(bytes.empty()||!RISECBOR64::DecodeCanonical(bytes,envelope,&error)||
			envelope.GetType()!=RISECBOR64::Value::Map||envelope.GetMap().size()!=2u){
			error="resume-equivalence trace envelope is invalid";return false;
		}
		const RISECBOR64::Value* payload=envelope.Find("payload");
		const RISECBOR64::Value* id=envelope.Find("trace_id");RISECBOR64::Bytes payloadBytes;
		if(!payload||!id||id->GetType()!=RISECBOR64::Value::Text||
			!RISECBOR64::Encode(*payload,payloadBytes,&error)||
			id->GetText()!=RISECBOR64::SHA256Hex(payloadBytes)||
			!ParseResumeTracePayload(*payload,trace)){
			error="resume-equivalence trace is not canonical or self-consistent";return false;
		}
		return true;
	}

	RISECBOR64::Value ResumeCertificatePayload(const ResumeEquivalenceCertificate& certificate)
	{
		using RISECBOR64::Value;
		return Value::MapValue({
			{"accepted_step_count",Value::Unsigned(certificate.acceptedStepCount)},
			{"checkpoint_sha256",Value::String(certificate.checkpointDigest)},
			{"frame_sha256",TextArrayValue(certificate.frameDigests)},
			{"maximum_eos_residual_bits",UnsignedArrayValue(certificate.maximumEOSResidualBits)},
			{"maximum_temperature_K_bits",UnsignedArrayValue(certificate.maximumTemperatureBits)},
			{"new_build_id",Value::String(certificate.newBuildId)},
			{"new_executable_sha256",Value::String(certificate.newExecutableDigest)},
			{"old_build_id",Value::String(certificate.oldBuildId)},
			{"old_executable_sha256",Value::String(certificate.oldExecutableDigest)},
			{"record_kind",Value::String("fire-resume-equivalence-certificate-v1")},
			{"resumed_from_step",Value::Unsigned(certificate.resumedFromStep)},
			{"schema_version",Value::Unsigned(1)},
			{"time_step_s_bits",UnsignedArrayValue(certificate.timeStepBits)}
		});
	}
	bool ParseResumeCertificatePayload(const RISECBOR64::Value& payload,
		ResumeEquivalenceCertificate& certificate)
	{
		std::string kind;std::uint64_t schema=0u;
		return payload.GetType()==RISECBOR64::Value::Map&&payload.GetMap().size()==13u&&
			ReadUnsignedMember(payload,"accepted_step_count",certificate.acceptedStepCount)&&
			ReadTextMember(payload,"checkpoint_sha256",certificate.checkpointDigest)&&
			ReadTextArray(payload,"frame_sha256",certificate.frameDigests)&&
			ReadUnsignedArray(payload,"maximum_eos_residual_bits",certificate.maximumEOSResidualBits)&&
			ReadUnsignedArray(payload,"maximum_temperature_K_bits",certificate.maximumTemperatureBits)&&
			ReadTextMember(payload,"new_build_id",certificate.newBuildId)&&
			ReadTextMember(payload,"new_executable_sha256",certificate.newExecutableDigest)&&
			ReadTextMember(payload,"old_build_id",certificate.oldBuildId)&&
			ReadTextMember(payload,"old_executable_sha256",certificate.oldExecutableDigest)&&
			ReadTextMember(payload,"record_kind",kind)&&kind=="fire-resume-equivalence-certificate-v1"&&
			ReadUnsignedMember(payload,"resumed_from_step",certificate.resumedFromStep)&&
			ReadUnsignedMember(payload,"schema_version",schema)&&schema==1u&&
			ReadUnsignedArray(payload,"time_step_s_bits",certificate.timeStepBits);
	}
	bool LoadResumeEquivalenceCertificate(const std::filesystem::path& path,
		ResumeEquivalenceCertificate& certificate,std::string& error)
	{
		const RISECBOR64::Bytes bytes=ReadFileBytes(path);RISECBOR64::Value envelope;
		if(bytes.empty()||!RISECBOR64::DecodeCanonical(bytes,envelope,&error)||
			envelope.GetType()!=RISECBOR64::Value::Map||envelope.GetMap().size()!=2u){
			error="resume-equivalence certificate envelope is invalid";return false;
		}
		const RISECBOR64::Value* payload=envelope.Find("payload");
		const RISECBOR64::Value* id=envelope.Find("certificate_id");RISECBOR64::Bytes payloadBytes;
		if(!payload||!id||id->GetType()!=RISECBOR64::Value::Text||
			!RISECBOR64::Encode(*payload,payloadBytes,&error)||
			id->GetText()!=RISECBOR64::SHA256Hex(payloadBytes)||
			!ParseResumeCertificatePayload(*payload,certificate)){
			error="resume-equivalence certificate is not canonical or self-consistent";return false;
		}
		certificate.certificateId=id->GetText();
		const bool valid=certificate.acceptedStepCount>=8u&&
			certificate.timeStepBits.size()==certificate.acceptedStepCount&&
			certificate.maximumTemperatureBits.size()==certificate.acceptedStepCount&&
			certificate.maximumEOSResidualBits.size()==certificate.acceptedStepCount&&
			certificate.frameDigests.size()==certificate.acceptedStepCount;
		if(!valid)error="resume-equivalence certificate evidence is incomplete";
		return valid;
	}
	bool ResumeTraceEvidenceComplete(const ResumeEquivalenceTrace& trace)
	{
		return trace.acceptedStepCount>=8u&&trace.checkpointDigest.size()==64u&&
			trace.checkpointProducerBuildId.size()==64u&&trace.buildId.size()==64u&&
			trace.executableDigest.size()==64u&&
			trace.timeStepBits.size()==trace.acceptedStepCount&&
			trace.maximumTemperatureBits.size()==trace.acceptedStepCount&&
			trace.maximumEOSResidualBits.size()==trace.acceptedStepCount&&
			trace.frameDigests.size()==trace.acceptedStepCount&&std::all_of(trace.frameDigests.begin(),
				trace.frameDigests.end(),[](const std::string& digest){return digest.size()==64u;});
	}
	bool SaveResumeEquivalenceCertificate(const std::filesystem::path& path,
		ResumeEquivalenceCertificate& certificate,std::string& error)
	{
		const RISECBOR64::Value payload=ResumeCertificatePayload(certificate);
		RISECBOR64::Bytes payloadBytes,envelope;
		if(!RISECBOR64::Encode(payload,payloadBytes,&error))return false;
		certificate.certificateId=RISECBOR64::SHA256Hex(payloadBytes);
		if(!RISECBOR64::Encode(RISECBOR64::Value::MapValue({{"payload",payload},
			{"certificate_id",RISECBOR64::Value::String(certificate.certificateId)}}),
			envelope,&error))return false;
		return DurableWriteCanonical(path,envelope,error);
	}
	bool BuildResumeEquivalenceCertificate(const ResumeEquivalenceTrace& oldTrace,
		const ResumeEquivalenceTrace& newTrace,const std::filesystem::path& path,
		ResumeEquivalenceCertificate& certificate,std::string& error)
	{
		if(!ResumeTraceEvidenceComplete(oldTrace)||!ResumeTraceEvidenceComplete(newTrace)||
			oldTrace.acceptedStepCount!=newTrace.acceptedStepCount||
			oldTrace.checkpointDigest!=newTrace.checkpointDigest||
			oldTrace.checkpointProducerBuildId!=newTrace.checkpointProducerBuildId||
			oldTrace.buildId!=oldTrace.checkpointProducerBuildId||
			oldTrace.buildId==newTrace.buildId||
			oldTrace.executableDigest==newTrace.executableDigest||
			oldTrace.resumedFromStep!=newTrace.resumedFromStep||
			oldTrace.timeStepBits!=newTrace.timeStepBits||
			oldTrace.maximumTemperatureBits!=newTrace.maximumTemperatureBits||
			oldTrace.maximumEOSResidualBits!=newTrace.maximumEOSResidualBits||
			oldTrace.frameDigests!=newTrace.frameDigests){
			error="resume-equivalence traces differ";return false;
		}
		certificate=ResumeEquivalenceCertificate();
		certificate.checkpointDigest=oldTrace.checkpointDigest;
		certificate.oldBuildId=oldTrace.buildId;certificate.newBuildId=newTrace.buildId;
		certificate.oldExecutableDigest=oldTrace.executableDigest;
		certificate.newExecutableDigest=newTrace.executableDigest;
		certificate.resumedFromStep=oldTrace.resumedFromStep;
		certificate.acceptedStepCount=oldTrace.acceptedStepCount;
		certificate.timeStepBits=oldTrace.timeStepBits;
		certificate.maximumTemperatureBits=oldTrace.maximumTemperatureBits;
		certificate.maximumEOSResidualBits=oldTrace.maximumEOSResidualBits;
		certificate.frameDigests=oldTrace.frameDigests;
		return SaveResumeEquivalenceCertificate(path,certificate,error);
	}

	template<typename T> std::vector<T> FinalEvidenceValues(const std::vector<T>& values,
		const std::size_t count)
	{
		if(values.size()<count)return std::vector<T>();
		return std::vector<T>(values.end()-static_cast<std::ptrdiff_t>(count),values.end());
	}

	bool DurableCopyPublishedFile(const std::filesystem::path& source,
		const std::filesystem::path& target,std::string& error)
	{
		if(target.has_parent_path())std::filesystem::create_directories(target.parent_path());
#if defined(_WIN32)
		const long long processId=static_cast<long long>(::_getpid());
#else
		const long long processId=static_cast<long long>(::getpid());
#endif
		const std::filesystem::path temporary=target.string()+".tmp."+std::to_string(processId);
		std::error_code copyError;
		std::filesystem::copy_file(source,temporary,
			std::filesystem::copy_options::overwrite_existing,copyError);
		if(copyError){error="cannot copy durable run artifact";return false;}
		if(!DurableSyncFileAndDirectory(temporary,error)||
			!AtomicReplaceCheckpoint(temporary,target,error)){
			std::error_code ignored;std::filesystem::remove(temporary,ignored);return false;
		}
		return true;
	}

	bool DurableWritePublishedBytes(const RISECBOR64::Bytes& bytes,
		const std::filesystem::path& target,std::string& error)
	{
		if(target.has_parent_path())std::filesystem::create_directories(target.parent_path());
#if defined(_WIN32)
		const long long processId=static_cast<long long>(::_getpid());
#else
		const long long processId=static_cast<long long>(::getpid());
#endif
		const std::filesystem::path temporary=target.string()+".tmp."+std::to_string(processId);
		{
			std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
			if(!output){error="cannot open durable run metadata";return false;}
			output.write(reinterpret_cast<const char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
			output.flush();
			if(!output){error="cannot write durable run metadata";return false;}
		}
		if(!DurableSyncFileAndDirectory(temporary,error)||
			!AtomicReplaceCheckpoint(temporary,target,error)){
			std::error_code ignored;std::filesystem::remove(temporary,ignored);return false;
		}
		return true;
	}

	RISECBOR64::Bytes ReadFileBytes( const std::filesystem::path& path )
	{
		std::ifstream input(path,std::ios::binary);
		if(!input) return RISECBOR64::Bytes();
		input.seekg(0,std::ios::end);
		const std::streampos end=input.tellg();
		if(end<0) return RISECBOR64::Bytes();
		input.seekg(0,std::ios::beg);
		RISECBOR64::Bytes bytes(static_cast<std::size_t>(end));
		if(end>0) input.read(reinterpret_cast<char*>(bytes.data()),end);
		if(!input&&end>0) return RISECBOR64::Bytes();
		return bytes;
	}

	RISECBOR64::Bytes RunMetadataEnvelope(const SolverFrameValues& values,
		const unsigned int workerCount,const std::string& frame4Digest,
		const std::string& frame5Digest,const std::string& sequenceId,
		std::string& metadataId)
	{
		using RISECBOR64::Value;
		Value::Values steps;
		for(const std::uint64_t step:values.checkpointStepIndices)
			steps.push_back(Value::Unsigned(step));
		Value::Values workerHistory;
		for(const std::uint64_t workers:values.workerCountHistory)
			workerHistory.push_back(Value::Unsigned(workers));
		const Value buildMigration=values.migrationCertificateId.empty()?
			Value::MapValue({{"kind",Value::String("none")}}):
			Value::MapValue({
				{"accepted_step_count",Value::Unsigned(values.migrationAcceptedStepCount)},
				{"certificate_id",Value::String(values.migrationCertificateId)},
				{"kind",Value::String("resume_equivalence")},
				{"new_build_id",Value::String(values.migrationNewBuildId)},
				{"old_build_id",Value::String(values.migrationOldBuildId)},
				{"resumed_from_step",Value::Unsigned(values.migrationResumedFromStep)}
			});
		const Value payload=Value::MapValue({
			{"active_set_algorithm_version",Value::String(values.activeSetAlgorithmVersion)},
			{"active_set_prior_algorithm_version",Value::String(
				values.priorActiveSetAlgorithmVersion)},
			{"active_set_discontinuous_event_count",Value::Unsigned(
				values.discontinuousActiveSetEvents)},
			{"active_set_maximum_complementarity_discrepancy_m_per_s",Value::Float(
				values.maximumActiveSetComplementarityDiscrepancyMPerS)},
			{"active_set_maximum_cycle_length",Value::Unsigned(
				values.maximumActiveSetCycleLength)},
			{"active_set_maximum_differing_face_count",Value::Unsigned(
				values.maximumActiveSetDifferingFaceCount)},
			{"active_set_thread_identity_checked",Value::Bool(
				values.activeSetThreadIdentityChecked)},
			{"active_set_thread_identity",Value::Bool(values.activeSetThreadIdentity)},
			{"build_migration",buildMigration},
			{"checkpoint_cadence_wall_s",Value::Float(values.checkpointCadenceWallS)},
			{"checkpoint_count",Value::Unsigned(values.checkpointStepIndices.size())},
			{"checkpoint_step_indices",Value::ArrayValue(steps)},
			{"effective_worker_count",Value::Unsigned(workerCount)},
			{"frame4_sha256",Value::String(frame4Digest)},
			{"frame5_sha256",Value::String(frame5Digest)},
			{"record_kind",Value::String("fire-simulation-run-metadata-v3")},
			{"reduction_mode",Value::String(values.reductionMode)},
			{"resumed_from_checkpoint",Value::Bool(values.resumedFromCheckpoint)},
			{"resumed_from_step",Value::Unsigned(values.resumedFromStep)},
			{"schema_version",Value::Unsigned(3)},
			{"sequence_id",Value::String(sequenceId)},
			{"streamed_frame_count",Value::Unsigned(values.streamedFrameCount)},
			{"worker_count_history",Value::ArrayValue(workerHistory)}
		});
		std::string error;RISECBOR64::Bytes payloadBytes,envelope;
		if(!RISECBOR64::Encode(payload,payloadBytes,&error))return envelope;
		metadataId=RISECBOR64::SHA256Hex(payloadBytes);
		RISECBOR64::Encode(Value::MapValue({{"payload",payload},
			{"run_metadata_id",Value::String(metadataId)}}),envelope,&error);
		return envelope;
	}

	std::string TextArrayCSV( const RISECBOR64::Value* value )
	{
		if(!value || value->GetType()!=RISECBOR64::Value::Array) return std::string();
		std::string result;
		for(const RISECBOR64::Value& item:value->GetArray()) {
			if(item.GetType()!=RISECBOR64::Value::Text) continue;
			if(!result.empty()) result.push_back(',');
			result+=item.GetText();
		}
		return result;
	}
	void WriteJSON( std::ostream& output, const RISECBOR64::Value& value )
	{
		using RISECBOR64::Value;
		switch(value.GetType()) {
		case Value::Null: output << "null"; break;
		case Value::Boolean: output << (value.GetBoolean()?"true":"false"); break;
		case Value::UnsignedInteger: output << value.GetIntegerArgument(); break;
		case Value::NegativeInteger: output << (-1-static_cast<std::int64_t>(
			value.GetIntegerArgument())); break;
		case Value::Float64: output << std::setprecision(17) << value.GetFloat(); break;
		case Value::Text: {
			output << '"';
			for(const unsigned char c:value.GetText()) {
				if(c=='"'||c=='\\') output << '\\' << static_cast<char>(c);
				else if(c=='\n') output << "\\n";
				else if(c=='\r') output << "\\r";
				else if(c=='\t') output << "\\t";
				else if(c<0x20u) output << "\\u" << std::hex << std::setw(4) <<
					std::setfill('0') << static_cast<unsigned int>(c) << std::dec;
				else output << static_cast<char>(c);
			}
			output << '"'; break;
		}
		case Value::ByteString:
			output << '"'; for(const unsigned char byte:value.GetBytes()) output << std::hex <<
				std::setw(2) << std::setfill('0') << static_cast<unsigned int>(byte);
			output << std::dec << '"'; break;
		case Value::Array:
			output << '['; for(std::size_t i=0;i<value.GetArray().size();++i) {
				if(i) output << ','; WriteJSON(output,value.GetArray()[i]);
			} output << ']'; break;
		case Value::Map:
			output << '{'; for(std::size_t i=0;i<value.GetMap().size();++i) {
				if(i) output << ','; WriteJSON(output,Value::String(value.GetMap()[i].first));
				output << ':'; WriteJSON(output,value.GetMap()[i].second);
			} output << '}'; break;
		}
	}

#if defined(RISE_ENABLE_OPENVDB)
	struct FrameMutation
	{
		enum Kind { Valid, NegativeActiveCarbon, HotInactiveTemperature,
			NegativeInactiveCarbon, NonfiniteInactiveCarbon, NonfiniteVelocity,
			PositiveInfinityReaction, NegativeInfinityReaction, ZeroTemperature,
			OutOfDomainTemperature, NegativeActiveTile, HotInactiveTile,
			NaNInactiveTile, NegativeActiveChem, NaNInactiveChem } kind = Valid;
	};

	bool WriteFrame( const std::filesystem::path& path, const FrameMutation mutation,
		const float carbonValue, const bool includeChem=false,
		const SolverFrameValues solver=SolverFrameValues(),const float chemScale=1.0f )
	{
		openvdb::initialize();
		const bool solverGrid=solver.dimensions[0]&&solver.dimensions[1]&&solver.dimensions[2];
		const double voxelSize=solverGrid?solver.cellWidthM:0.5;
		const openvdb::math::Transform::Ptr transform =
			openvdb::math::Transform::createLinearTransform(voxelSize);
		openvdb::FloatGrid::Ptr carbon = openvdb::FloatGrid::create(0.0f);
		openvdb::FloatGrid::Ptr temperature = openvdb::FloatGrid::create(300.0f);
		openvdb::FloatGrid::Ptr reaction = openvdb::FloatGrid::create(0.0f);
		openvdb::FloatGrid::Ptr chemCH = openvdb::FloatGrid::create(0.0f);
		openvdb::FloatGrid::Ptr chemC2 = openvdb::FloatGrid::create(0.0f);
		openvdb::FloatGrid::Ptr chemCO2 = openvdb::FloatGrid::create(0.0f);
		openvdb::Vec3fGrid::Ptr velocity = openvdb::Vec3fGrid::create(openvdb::Vec3f(0));
		for( const auto& grid : {openvdb::GridBase::Ptr(carbon),openvdb::GridBase::Ptr(temperature),
			openvdb::GridBase::Ptr(reaction),openvdb::GridBase::Ptr(chemCH),
			openvdb::GridBase::Ptr(chemC2),openvdb::GridBase::Ptr(chemCO2)} ) {
			grid->setTransform(transform->copy());
		}
		openvdb::math::Transform::Ptr velocityTransform = transform->copy();
		velocityTransform->postTranslate(openvdb::Vec3d(-voxelSize));
		velocity->setTransform(velocityTransform);
		carbon->setName("carbon"); temperature->setName("temperature");
		reaction->setName("reaction"); velocity->setName("velocity");
		chemCH->setName("chem_CH"); chemC2->setName("chem_C2"); chemCO2->setName("chem_CO2");
		if(solverGrid && solver.temperature.size()==solver.dimensions[0]*solver.dimensions[1]*
			solver.dimensions[2] && solver.reaction.size()==solver.temperature.size() &&
			solver.carbon.size()==solver.temperature.size() &&
			solver.velocity.size()==solver.temperature.size()) {
			for(std::size_t z=0;z<solver.dimensions[2];++z)
				for(std::size_t y=0;y<solver.dimensions[1];++y)
				for(std::size_t x=0;x<solver.dimensions[0];++x) {
					const std::size_t index=(z*solver.dimensions[1]+y)*solver.dimensions[0]+x;
					const openvdb::Coord scalar(static_cast<int>(x),static_cast<int>(y),
						static_cast<int>(z));
					carbon->tree().setValueOn(scalar,solver.carbon[index]);
					temperature->tree().setValueOn(scalar,solver.temperature[index]);
					reaction->tree().setValueOn(scalar,solver.reaction[index]);
					const std::array<float,3>& v=solver.velocity[index];
					velocity->tree().setValueOn(openvdb::Coord(static_cast<int>(x+1),
						static_cast<int>(y+1),static_cast<int>(z+1)),openvdb::Vec3f(v[0],v[1],v[2]));
				}
		} else {
			carbon->tree().setValueOn(openvdb::Coord(0,0,0),carbonValue);
			temperature->tree().setValueOn(openvdb::Coord(0,0,0),solver.temperatureK);
			reaction->tree().setValueOn(openvdb::Coord(0,0,0),solver.reactionWPerM3);
		}
		if( includeChem ) {
			if(solverGrid) for(std::size_t z=0;z<solver.dimensions[2];++z)
				for(std::size_t y=0;y<solver.dimensions[1];++y)
					for(std::size_t x=0;x<solver.dimensions[0];++x) {
						const openvdb::Coord fixture(static_cast<int>(x),static_cast<int>(y),
							static_cast<int>(z));
						chemCH->tree().setValueOn(fixture,chemScale*120.0f);
						chemC2->tree().setValueOn(fixture,chemScale*50.0f);
						chemCO2->tree().setValueOn(fixture,chemScale*8.0f);
					}
			else {
				chemCH->tree().setValueOn(openvdb::Coord(0,0,0),chemScale*120.0f);
				chemC2->tree().setValueOn(openvdb::Coord(0,0,0),chemScale*50.0f);
				chemCO2->tree().setValueOn(openvdb::Coord(0,0,0),chemScale*8.0f);
			}
		}
		if( mutation.kind == FrameMutation::NegativeActiveCarbon ) {
			carbon->tree().setValueOn(openvdb::Coord(1,0,0),-1.0f);
		} else if( mutation.kind == FrameMutation::HotInactiveTemperature ) {
			temperature->tree().setValueOff(openvdb::Coord(1,0,0),1200.0f);
		} else if( mutation.kind == FrameMutation::NegativeInactiveCarbon ) {
			carbon->tree().setValueOff(openvdb::Coord(1,0,0),-1.0f);
		} else if( mutation.kind == FrameMutation::NonfiniteInactiveCarbon ) {
			carbon->tree().setValueOff(openvdb::Coord(1,0,0),FloatFromBits(0x7fc00001u));
		} else if( mutation.kind == FrameMutation::NonfiniteVelocity ) {
			velocity->tree().setValueOn(openvdb::Coord(0,0,0),
				openvdb::Vec3f(FloatFromBits(0x7f800000u),0,0));
		} else if( mutation.kind == FrameMutation::PositiveInfinityReaction ) {
			reaction->tree().setValueOn(openvdb::Coord(1,0,0),FloatFromBits(0x7f800000u));
		} else if( mutation.kind == FrameMutation::NegativeInfinityReaction ) {
			reaction->tree().setValueOn(openvdb::Coord(1,0,0),FloatFromBits(0xff800000u));
		} else if( mutation.kind == FrameMutation::ZeroTemperature ) {
			temperature->tree().setValueOn(openvdb::Coord(1,0,0),0.0f);
		} else if( mutation.kind == FrameMutation::OutOfDomainTemperature ) {
			temperature->tree().setValueOn(openvdb::Coord(1,0,0),2501.0f);
		} else if( mutation.kind == FrameMutation::NegativeActiveTile ) {
			carbon->tree().addTile(1,openvdb::Coord(0,0,0),-1.0f,true);
		} else if( mutation.kind == FrameMutation::HotInactiveTile ) {
			temperature->tree().addTile(1,openvdb::Coord(0,0,0),1200.0f,false);
		} else if( mutation.kind == FrameMutation::NaNInactiveTile ) {
			carbon->tree().addTile(1,openvdb::Coord(0,0,0),FloatFromBits(0x7fc00001u),false);
		} else if( mutation.kind == FrameMutation::NegativeActiveChem ) {
			chemC2->tree().setValueOn(openvdb::Coord(1,0,0),-1.0f);
		} else if( mutation.kind == FrameMutation::NaNInactiveChem ) {
			chemCO2->tree().setValueOff(openvdb::Coord(1,0,0),FloatFromBits(0x7fc00001u));
		}
		openvdb::GridPtrVec grids{carbon,temperature,reaction,velocity};
		if( includeChem ) {
			grids.push_back(chemCH); grids.push_back(chemC2); grids.push_back(chemCO2);
		}
		openvdb::io::File file(path.string());
		file.write(grids);
		file.close();
		std::string canonicalError;
		const bool canonical=CanonicalizeOpenVDBFileIdentity(path.string(),canonicalError);
		Check(canonical,
			"producer replaces OpenVDB's random UUID with a content-derived identity");
		const bool durable=canonical&&DurableSyncFileAndDirectory(path,canonicalError);
		Check(durable,
			"produced sequence frame is durable before the next simulation step");
		return canonical&&durable;
	}

	int RunCheckpointChild(const std::string& mode,const std::filesystem::path& checkpointPath,
		const std::filesystem::path& framePath,const unsigned int workerCount)
	{
		if(mode!="baseline"&&mode!="kill"&&mode!="resume"&&mode!="resume-final"&&
			mode!="resume-one-fp64-reject"&&
			mode!="syncfail")return 96;
		forcePostRenameDirectorySyncFailureForTest=mode=="syncfail";
		RunPersistenceOptions persistence;
		if(mode!="baseline"){
			persistence.checkpointPath=checkpointPath;
			persistence.checkpointCadenceWallS=0.0;
			persistence.resume=mode=="resume"||mode=="resume-final"||
				mode=="resume-one-fp64-reject";
			if(mode=="resume-final"||mode=="resume-one-fp64-reject")
				persistence.finalCheckpointPath=checkpointPath;
			if(mode=="resume-one-fp64-reject")persistence.stopAfterAdditionalAcceptedSteps=1u;
			if(mode=="resume-one-fp64-reject")persistence.forceZeroSourceForTest=true;
			persistence.killAfterFirstCheckpoint=mode=="kill"||mode=="syncfail";
		}
		SolverFrameValues result=RunMethaneFrameProbe(workerCount,3u,0.0,1.0,4.0,6.0,
			CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,persistence);
		if(mode=="syncfail"){
			forcePostRenameDirectorySyncFailureForTest=false;
			if(result.succeeded||result.structuredError.find("cannot fsync replaced-run directory")==
				std::string::npos)return 97;
			std::ofstream marker(framePath,std::ios::binary|std::ios::trunc);
			marker << result.structuredError;marker.close();return marker?0:98;
		}
		if(mode=="resume-one-fp64-reject")return !result.succeeded&&
			result.structuredError.find("violates the certified affine rows")!=std::string::npos?
			0:97;
		if(!result.succeeded){std::fprintf(stderr,"checkpoint child failed: %s\n",
			result.structuredError.c_str());return 93;}
		if(!WriteFrame(framePath,FrameMutation{},0.0f,true,result))return 94;
		std::string error;
		if(!DurableSyncFileAndDirectory(framePath,error)){std::fprintf(stderr,
			"checkpoint child frame durability failed: %s\n",error.c_str());return 94;}
		return failures?95:0;
	}

	SolverFrameValues CheckpointDiagnosticFrame(const MethaneRunCheckpoint& checkpoint)
	{
		SolverFrameValues values;
		PeriodicMACShape shape;shape.nx=checkpoint.dimensions[0];shape.ny=checkpoint.dimensions[1];
		shape.nz=checkpoint.dimensions[2];shape.cellWidthM=checkpoint.cellWidthM;
		values.dimensions=checkpoint.dimensions;values.cellWidthM=checkpoint.cellWidthM;
		values.caseRecordId=checkpoint.caseRecordId;
		values.temperature.resize(shape.CellCount());values.reaction.assign(shape.CellCount(),0.0f);
		values.carbon.resize(shape.CellCount());values.velocity.resize(shape.CellCount());
		for(std::size_t cell=0;cell<shape.CellCount();++cell){
			values.temperature[cell]=static_cast<float>(checkpoint.states[cell].temperatureK);
			values.carbon[cell]=static_cast<float>(std::max(0.0,
				checkpoint.states[cell].constituent[MethaneCarbon]));
			for(unsigned int axis=0;axis<3;++axis){
				const std::size_t lower=OpenLowerFaceForCell3D(shape,cell,axis);
				const std::size_t upper=OpenUpperFaceForCell3D(shape,cell,axis);
				values.velocity[cell][axis]=static_cast<float>(0.5*(
					checkpoint.velocity.component[axis][lower]+
					checkpoint.velocity.component[axis][upper]));
			}
		}
		values.temperatureK=*std::max_element(values.temperature.begin(),values.temperature.end());
		values.reactionWPerM3=0.0f;values.succeeded=true;
		return values;
	}

	bool ParseUnsignedArgument(const char* text,const unsigned long maximum,
		unsigned long& value)
	{
		char* end=nullptr;errno=0;value=std::strtoul(text,&end,10);
		return !errno&&end!=text&&*end=='\0'&&value>0u&&value<=maximum;
	}
	bool ParsePositiveDoubleArgument(const char* text,double& value)
	{
		char* end=nullptr;errno=0;value=std::strtod(text,&end);
		return !errno&&end!=text&&*end=='\0'&&std::isfinite(value)&&value>0.0;
	}

	int RunResumeEquivalenceTraceChild(const std::filesystem::path& checkpointPath,
		const std::filesystem::path& tracePath,const std::filesystem::path& framePath,
		const unsigned int workerCount,const unsigned int acceptedStepCount,
		const double caseDurationS,const double caseFramesPerS,const double resolutionTier,
		const double poolDiameterM,const double heatReleaseRateKW,
		const std::string& expectedCheckpointBuildId)
	{
		MethaneRunCheckpoint checkpoint;std::string error;
		if(!LoadMethaneRunCheckpoint(checkpointPath,checkpoint,error)||
			checkpoint.producerBuildId!=expectedCheckpointBuildId){
			std::fprintf(stderr,"resume-equivalence checkpoint rejected: %s\n",error.c_str());
			return 91;
		}
		const std::string checkpointDigest=DigestFile(checkpointPath);
		if(checkpointDigest.empty())return 92;
		const std::filesystem::path snapshotDirectory=tracePath.string()+".snapshots";
		std::error_code directoryError;
		std::filesystem::create_directories(snapshotDirectory,directoryError);
		if(directoryError)return 92;
		RunPersistenceOptions persistence;
		persistence.checkpointPath=checkpointPath;
		persistence.checkpointCadenceWallS=std::numeric_limits<double>::max();
		persistence.resume=true;persistence.isolatedEquivalenceProbe=true;
		persistence.isolatedExpectedCheckpointBuildId=expectedCheckpointBuildId;
		persistence.stopAfterAdditionalAcceptedSteps=acceptedStepCount;
		persistence.equivalenceSnapshotDirectory=snapshotDirectory;
		const SolverFrameValues result=RunMethaneFrameProbe(workerCount,1u,0.0,
			caseDurationS,caseFramesPerS,resolutionTier,poolDiameterM,heatReleaseRateKW,
			false,persistence);
		if(!result.succeeded){std::fprintf(stderr,"resume-equivalence continuation failed: %s\n",
			result.structuredError.c_str());return 93;}
		RISECBOR64::Bytes buildRecord;std::string buildId,executableDigest;
		if(!CurrentRendererBuildIdentity(buildRecord,buildId)||
			!CurrentExecutableDigest(buildRecord,executableDigest,error))return 95;
		ResumeEquivalenceTrace trace;
		trace.checkpointDigest=checkpointDigest;
		trace.checkpointProducerBuildId=checkpoint.producerBuildId;
		trace.buildId=buildId;trace.executableDigest=executableDigest;
		trace.resumedFromStep=checkpoint.acceptedSteps;
		trace.acceptedStepCount=acceptedStepCount;
		trace.timeStepBits.reserve(acceptedStepCount);
		trace.maximumTemperatureBits.reserve(acceptedStepCount);
		trace.maximumEOSResidualBits.reserve(acceptedStepCount);
		const std::vector<double> timeSteps=FinalEvidenceValues(
			result.acceptedTimeStepHistoryS,acceptedStepCount);
		const std::vector<double> temperatures=FinalEvidenceValues(
			result.acceptedMaximumTemperatureHistoryK,acceptedStepCount);
		const std::vector<double> eosResiduals=FinalEvidenceValues(
			result.acceptedMaximumEOSResidualHistory,acceptedStepCount);
		if(timeSteps.size()!=acceptedStepCount||temperatures.size()!=acceptedStepCount||
			eosResiduals.size()!=acceptedStepCount)return 96;
		for(const double value:timeSteps)trace.timeStepBits.push_back(DoubleBits(value));
		for(const double value:temperatures)trace.maximumTemperatureBits.push_back(DoubleBits(value));
		for(const double value:eosResiduals)trace.maximumEOSResidualBits.push_back(DoubleBits(value));
		for(unsigned int evidenceStep=1u;evidenceStep<=acceptedStepCount;++evidenceStep){
			std::ostringstream name;name<<"step_"<<std::setw(2)<<std::setfill('0')<<
				evidenceStep<<".checkpoint";
			MethaneRunCheckpoint snapshot;
			if(!LoadMethaneRunCheckpoint(snapshotDirectory/name.str(),snapshot,error))return 94;
			const SolverFrameValues diagnostic=CheckpointDiagnosticFrame(snapshot);
			const std::filesystem::path evidenceFrame=evidenceStep==acceptedStepCount?framePath:
				std::filesystem::path(framePath.string()+".step_"+std::to_string(evidenceStep)+".vdb");
			if(!WriteFrame(evidenceFrame,FrameMutation{},0.0f,true,diagnostic))return 94;
			trace.frameDigests.push_back(DigestFile(evidenceFrame));
		}
		if(trace.frameDigests.size()!=acceptedStepCount||
			std::any_of(trace.frameDigests.begin(),trace.frameDigests.end(),
				[](const std::string& digest){return digest.empty();})||
			!SaveResumeEquivalenceTrace(tracePath,trace,error)){
			std::fprintf(stderr,"resume-equivalence trace write failed: %s\n",error.c_str());
			return 97;
		}
		return 0;
	}

	int RunLegacyCheckpointTraceChild(const std::filesystem::path& checkpointPath,
		const std::filesystem::path& snapshotDirectory,const std::filesystem::path& tracePath,
		const std::filesystem::path& framePath,const std::string& executableDigest)
	{
		MethaneRunCheckpoint beginning;std::string error;
		if(executableDigest.size()!=64u||
			!LoadMethaneRunCheckpoint(checkpointPath,beginning,error))return 91;
		ResumeEquivalenceTrace trace;
		trace.checkpointDigest=DigestFile(checkpointPath);
		trace.checkpointProducerBuildId=beginning.producerBuildId;
		trace.buildId=beginning.producerBuildId;trace.executableDigest=executableDigest;
		trace.resumedFromStep=beginning.acceptedSteps;trace.acceptedStepCount=8u;
		MethaneRunCheckpoint finalCheckpoint;
		for(std::uint64_t evidenceStep=1u;evidenceStep<=8u;++evidenceStep){
			std::ostringstream name;name<<"step_"<<std::setw(2)<<std::setfill('0')<<
				evidenceStep<<".checkpoint";
			MethaneRunCheckpoint snapshot;
			if(!LoadMethaneRunCheckpoint(snapshotDirectory/name.str(),snapshot,error)||
				snapshot.caseRecordId!=beginning.caseRecordId||
				snapshot.producerBuildId!=beginning.producerBuildId||
				snapshot.acceptedSteps!=beginning.acceptedSteps+evidenceStep||
				snapshot.values.acceptedTimeStepHistoryS.empty()||
				snapshot.values.acceptedMaximumEOSResidualHistory.empty())return 92;
			trace.timeStepBits.push_back(DoubleBits(
				snapshot.values.acceptedTimeStepHistoryS.back()));
			double stepMaximumTemperatureK=0.0;
			for(const MethaneCellState& state:snapshot.states)
				stepMaximumTemperatureK=std::max(stepMaximumTemperatureK,state.temperatureK);
			trace.maximumTemperatureBits.push_back(DoubleBits(stepMaximumTemperatureK));
			trace.maximumEOSResidualBits.push_back(DoubleBits(
				snapshot.values.acceptedMaximumEOSResidualHistory.back()));
			const SolverFrameValues diagnostic=CheckpointDiagnosticFrame(snapshot);
			const std::filesystem::path evidenceFrame=evidenceStep==8u?framePath:
				std::filesystem::path(framePath.string()+".step_"+
					std::to_string(evidenceStep)+".vdb");
			if(!WriteFrame(evidenceFrame,FrameMutation{},0.0f,true,diagnostic))return 93;
			trace.frameDigests.push_back(DigestFile(evidenceFrame));
			finalCheckpoint=std::move(snapshot);
		}
		if(finalCheckpoint.acceptedSteps!=beginning.acceptedSteps+8u||
			trace.frameDigests.size()!=8u||
			std::any_of(trace.frameDigests.begin(),trace.frameDigests.end(),
				[](const std::string& digest){return digest.empty();})||
			!SaveResumeEquivalenceTrace(tracePath,trace,error))
			return 94;
		return 0;
	}

	int RunResumeEquivalenceCertificateChild(const std::filesystem::path& oldTracePath,
		const std::filesystem::path& newTracePath,const std::filesystem::path& checkpointPath,
		const std::filesystem::path& certificatePath)
	{
		ResumeEquivalenceTrace oldTrace,newTrace;ResumeEquivalenceCertificate certificate;
		std::string error;
		if(!LoadResumeEquivalenceTrace(oldTracePath,oldTrace,error)||
			!LoadResumeEquivalenceTrace(newTracePath,newTrace,error)||
			oldTrace.checkpointDigest!=DigestFile(checkpointPath)||
			!BuildResumeEquivalenceCertificate(oldTrace,newTrace,certificatePath,certificate,error)){
			std::fprintf(stderr,"resume-equivalence certificate failed: %s\n",error.c_str());
			return 91;
		}
		std::fprintf(stderr,"resume-equivalence certificate_id=%s steps=%llu\n",
			certificate.certificateId.c_str(),
			static_cast<unsigned long long>(certificate.acceptedStepCount));
		return 0;
	}

// Kept as a compact test-only include because the checkpoint schema and
// certified periodic oracle are private to this translation unit.
#include "FireProductionCalibrationFixture.h"
#include "FireProductionDyadicCalibrationFixture.h"
#include "FireProductionSubdominanceFixture.h"
#include "FireProductionGoldenProjectionFixture.h"
#include "FireProductionGoldenCompositionFixture.h"

	int RunR80GoldenContinuationFixture(const std::filesystem::path& checkpointPath,
		const std::filesystem::path& tracePath,const std::filesystem::path& framePath)
	{
		static const char* checkpointDigest=
			"1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947";
		static const char* checkpointBuild=
			"8fb5e3eb14566f29266013487be77bbbb64fa745383d7ec2af62c55f4f37cd56";
		if(DigestFile(checkpointPath)!=checkpointDigest){std::fprintf(stderr,
			"r80 golden continuation checkpoint digest mismatch\n");return 90;}
		const double targetS=25.032480502915522;
		const int status=RunResumeEquivalenceTraceChild(checkpointPath,tracePath,framePath,
			16u,8u,targetS,1.0/targetS,10.0,0.30,33.0,checkpointBuild);
		if(status!=0||DigestFile(checkpointPath)!=checkpointDigest)return status?status:91;
		ResumeEquivalenceTrace trace;std::string error;
		const std::vector<std::uint64_t> expectedTimeStepBits={
			4543432537948766955ull,4544197666642132584ull,4544654590867399846ull,
			4545157207515193834ull,4545710085827767221ull,4546318251971597947ull,
			4542483635102441249ull,4543219516136476427ull};
		const std::vector<std::uint64_t> expectedMaximumTemperatureBits={
			4655159579052727434ull,4655159374940711548ull,4655159154984970440ull,
			4655158912993804872ull,4655158636009097964ull,4655158346958421514ull,
			4655158196749501340ull,4655158022160599380ull};
		const std::vector<std::uint64_t> expectedMaximumEOSResidualBits={
			4485619931794112512ull,4489437736410808320ull,4488601145836568576ull,
			4486917660591783936ull,4488934740778287104ull,4487986528701644800ull,
			4484167744161316864ull,4484941030474383360ull};
		const std::vector<std::string> expectedFrameDigests={
			"4dd7381f54b3931f3e108f805f63d8d2ec456bd955ca7717059c182886092344",
			"77988ea6692f20e10bdf58035748bbbe33025dd33a4b969e82f37d7c3d10e4d7",
			"5d261f869ef75a40867ca0a80dfef4b21bd6e7d8016ac6123dd10541c64cedc2",
			"66ff620f3212e00f93d9f519813e6740201292ddf1b8ecf76ac3e2debee6b5b3",
			"4a36583eaf66cafd797077609248a3bc1cc44e5cc6611695a4363b55474d052e",
			"154b501c1b66437003557db728ba233aac04d89ba423bd678518d6014a2abce5",
			"5618775a8311ecb401277833ca5c38b9f5a6c0e6c4f4e8ab4fb820e20497b95c",
			"ccd9d2902468a6b2341307a495a9c567e89b4a68f1efffe176d780dd73a999fe"};
		if(!LoadResumeEquivalenceTrace(tracePath,trace,error)||
			trace.timeStepBits!=expectedTimeStepBits||
			trace.maximumTemperatureBits!=expectedMaximumTemperatureBits||
			trace.maximumEOSResidualBits!=expectedMaximumEOSResidualBits||
			trace.frameDigests!=expectedFrameDigests){
			std::fprintf(stderr,"r80 golden continuation trace mismatch: %s\n",error.c_str());
			return 92;
		}
		const std::filesystem::path finalSnapshot=tracePath.string()+
			".snapshots/step_08.checkpoint";
		MethaneRunCheckpoint final;
		if(!LoadMethaneRunCheckpoint(finalSnapshot,final,error)||final.acceptedSteps!=3487u||
			final.values.acceptedTimeStepHistoryS.empty()||
			DoubleBits(final.values.acceptedTimeStepHistoryS.back())!=
				expectedTimeStepBits.back()||
			final.values.discontinuousActiveSetEvents!=10u||
			final.values.maximumActiveSetCycleLength!=2u||
			final.values.maximumActiveSetDifferingFaceCount!=4u||
			DoubleBits(final.values.maximumActiveSetComplementarityDiscrepancyMPerS)!=
				DoubleBits(0.0011766913664658803)||
			final.values.activeSetAlgorithmVersion!=CurrentActiveSetAlgorithmVersion()||
			final.values.priorActiveSetAlgorithmVersion!=LegacyActiveSetAlgorithmVersion()||
			!final.values.activeSetThreadIdentityChecked||
			!final.values.activeSetThreadIdentity){
			std::fprintf(stderr,"r80 golden continuation failed: %s steps=%llu dt=%.17g "
				"events=%u thread_checked=%d thread_identical=%d\n",error.c_str(),
				static_cast<unsigned long long>(final.acceptedSteps),
				final.values.acceptedTimeStepHistoryS.empty()?0.0:
					final.values.acceptedTimeStepHistoryS.back(),
				final.values.discontinuousActiveSetEvents,
				final.values.activeSetThreadIdentityChecked?1:0,
				final.values.activeSetThreadIdentity?1:0);
			return 92;
		}
		std::fprintf(stderr,"r80 golden continuation passed step=%llu dt=%.17g events=%u "
			"cycle=%zu faces=%zu discrepancy=%.17g\n",
			static_cast<unsigned long long>(final.acceptedSteps),
			final.values.acceptedTimeStepHistoryS.back(),
			final.values.discontinuousActiveSetEvents,
			final.values.maximumActiveSetCycleLength,
			final.values.maximumActiveSetDifferingFaceCount,
			final.values.maximumActiveSetComplementarityDiscrepancyMPerS);
		return 0;
	}

#if defined(_WIN32)
	std::string QuoteSubprocessArgument(const std::string& value)
	{
		std::string result="\"";
		for(const char c:value){if(c=='\"')result+='\\';result+=c;}
		return result+'\"';
	}
#endif

	int RunCheckpointSubprocess(const std::filesystem::path& executable,const char* mode,
		const std::filesystem::path& checkpoint,const std::filesystem::path& frame,
		const unsigned int workers)
	{
#if defined(_WIN32)
		const std::string command=QuoteSubprocessArgument(executable.string())+
			" --fire-checkpoint-child "+mode+" "+QuoteSubprocessArgument(checkpoint.string())+
			" "+QuoteSubprocessArgument(frame.string())+" "+std::to_string(workers);
		return std::system(command.c_str());
#else
		const pid_t child=::fork();
		if(child<0)return -1;
		if(child==0){
			const std::string workerText=std::to_string(workers);
			::execl(executable.c_str(),executable.c_str(),"--fire-checkpoint-child",mode,
				checkpoint.c_str(),frame.c_str(),workerText.c_str(),static_cast<char*>(nullptr));
			::_exit(127);
		}
		int status=0;
		while(::waitpid(child,&status,0)<0){if(errno!=EINTR)return -1;}
		return status;
#endif
	}

	bool CheckpointSubprocessWasHardKilled(const int status)
	{
#if defined(_WIN32)
		return status==91;
#else
		return WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL;
#endif
	}
#endif
}

int main(int argc,char** argv)
{
#if defined(RISE_ENABLE_OPENVDB)
	if(const char* profileEnvironment=std::getenv("RISE_FIRE_PROFILE")){
		if(std::strcmp(profileEnvironment,"1")!=0){
			std::fprintf(stderr,"RISE_FIRE_PROFILE must be exactly 1\n");return 96;
		}
	}
	if(argc==7&&std::strcmp(argv[1],"--fire-case-id")==0){
		double duration=0.0,framesPerS=0.0,tier=0.0,diameter=0.0,heatRelease=0.0;
		if(!ParsePositiveDoubleArgument(argv[2],duration)||
			!ParsePositiveDoubleArgument(argv[3],framesPerS)||
			!ParsePositiveDoubleArgument(argv[4],tier)||
			!ParsePositiveDoubleArgument(argv[5],diameter)||
			!ParsePositiveDoubleArgument(argv[6],heatRelease))return 90;
		const SolverFrameValues identity=RunMethaneFrameProbe(1u,0u,0.0,duration,
			framesPerS,tier,diameter,heatRelease);
		if(!identity.succeeded)return 91;
		const double fullTarget=5.0*identity.flowThroughTimeS+
			40.0/(1.5/std::sqrt(CapstonePoolDiameterM));
		std::fprintf(stdout,"case_record_id=%s flow_through_time_s=%.17g "
			"full_target_s=%.17g full_frame_rate_per_s=%.17g\n",
			identity.caseRecordId.c_str(),identity.flowThroughTimeS,fullTarget,1.0/fullTarget);
		return 0;
	}
	if(argc==7&&std::strcmp(argv[1],"--fire-resume-equivalence-legacy-trace")==0)
		return RunLegacyCheckpointTraceChild(argv[2],argv[3],argv[4],argv[5],argv[6]);
	if(argc==13&&std::strcmp(argv[1],"--fire-resume-equivalence-trace")==0){
		unsigned long workers=0u,steps=0u;double duration=0.0,framesPerS=0.0,tier=0.0,
			diameter=0.0,heatRelease=0.0;
		if(!ParseUnsignedArgument(argv[5],64u,workers)||
			!ParseUnsignedArgument(argv[6],65536u,steps)||steps<8u||
			!ParsePositiveDoubleArgument(argv[7],duration)||
			!ParsePositiveDoubleArgument(argv[8],framesPerS)||
			!ParsePositiveDoubleArgument(argv[9],tier)||
			!ParsePositiveDoubleArgument(argv[10],diameter)||
			!ParsePositiveDoubleArgument(argv[11],heatRelease)||std::strlen(argv[12])!=64u)return 90;
		return RunResumeEquivalenceTraceChild(argv[2],argv[3],argv[4],
			static_cast<unsigned int>(workers),static_cast<unsigned int>(steps),duration,
			framesPerS,tier,diameter,heatRelease,argv[12]);
	}
	if(argc==6&&std::strcmp(argv[1],"--fire-resume-equivalence-certify")==0)
		return RunResumeEquivalenceCertificateChild(argv[2],argv[3],argv[4],argv[5]);
	if(argc==5&&std::strcmp(argv[1],"--fire-r80-golden-continuation")==0)
		return RunR80GoldenContinuationFixture(argv[2],argv[3],argv[4]);
	if(argc==3&&std::strcmp(argv[1],"--fire-production-golden-projection")==0)
		return RunProductionGoldenProjectionFixture(argv[2]);
	if(argc==4&&std::strcmp(argv[1],"--fire-production-golden-composition")==0)
		return RunProductionGoldenCompositionFixture(argv[2],argv[3]);
	if(argc==3&&std::strcmp(argv[1],"--fire-production-calibration-generate-inputs")==0)
		return RunProductionCalibrationStateGeneration(argv[2]);
	if(argc==3&&std::strcmp(argv[1],"--fire-production-calibration-seal-inputs")==0)
		return SealExistingProductionCalibrationInputs(argv[2]);
	if(argc==3&&std::strcmp(argv[1],"--fire-production-calibration-diagnose-input-family")==0)
		return DiagnoseProductionCalibrationInputFamily(argv[2]);
	if(argc==4&&std::strcmp(argv[1],"--fire-production-calibration-diagnose-input-family")==0)
		return DiagnoseProductionCalibrationInputFamily(argv[2],argv[3]);
	if(argc==3&&std::strcmp(argv[1],"--fire-production-calibration-seal-oracle-spatial")==0)
		return SealOracleSpatialCalibrationInputs(argv[2]);
	if(argc==4&&std::strcmp(argv[1],"--fire-production-calibration-check-oracle-spatial")==0)
		return CheckOracleSpatialCalibrationOutput(argv[2],argv[3]);
	if(argc==3&&std::strcmp(argv[1],"--fire-production-calibration-seal-dyadic-protocol")==0)
		return FireProductionDyadicCalibration::SealProtocol(argv[2]);
	if(argc==4&&std::strcmp(argv[1],"--fire-production-calibration-seal-dyadic-targets")==0)
		return FireProductionDyadicCalibration::SealTargets(argv[2],argv[3]);
	if(argc==5&&std::strcmp(argv[1],"--fire-production-calibration-check-dyadic-oracle")==0)
		return FireProductionDyadicCalibration::CheckOracle(argv[2],argv[3],argv[4]);
	if(argc==5&&std::strcmp(argv[1],"--fire-production-calibration-seal-dyadic-metrics")==0)
		return FireProductionDyadicCalibration::SealSupplementalMetrics(argv[2],argv[3],argv[4]);
	if(argc==6&&std::strcmp(argv[1],"--fire-production-calibration-check-dyadic-complete")==0)
		return FireProductionDyadicCalibration::CheckSupplementalOracleMetrics(
			argv[2],argv[3],argv[4],argv[5]);
	if(argc==5&&std::strcmp(argv[1],"--fire-production-calibration-check-dyadic-production")==0)
		return FireProductionDyadicCalibration::CheckProduction(argv[2],argv[3],argv[4]);
	if(argc==5&&std::strcmp(argv[1],"--fire-production-calibration-diagnose-roundoff")==0)
		return FireProductionDyadicCalibration::DiagnoseRoundoff(argv[2],argv[3],argv[4]);
	if(argc==7&&std::strcmp(argv[1],"--fire-production-calibration-measure-subdominance")==0)
		return FireProductionDyadicCalibration::MeasureProductionSubdominance(
			argv[2],argv[3],argv[4],argv[5],argv[6]);
	if(argc==6&&std::strcmp(argv[1],"--fire-checkpoint-child")==0){
		const unsigned long parsed=std::strtoul(argv[5],nullptr,10);
		if(parsed==0u||parsed>64u)return 92;
		return RunCheckpointChild(argv[2],argv[3],argv[4],static_cast<unsigned int>(parsed));
	}
	if(const char* probeEnvironment=std::getenv("RISE_FIRE_BUDGET_PROBE")){
		if(std::strcmp(probeEnvironment,"1")!=0){std::fprintf(stderr,
			"RISE_FIRE_BUDGET_PROBE must be exactly 1\n");return 96;}
		double targetS=1.2;
		unsigned int probeWorkers=1u;
		if(const char* targetEnvironment=std::getenv("RISE_FIRE_BUDGET_TARGET_S")){
			char* end=0;errno=0;targetS=std::strtod(targetEnvironment,&end);
			if(errno||end==targetEnvironment||*end!='\0'||!std::isfinite(targetS)||targetS<=0.0){
				std::fprintf(stderr,"RISE_FIRE_BUDGET_TARGET_S is invalid\n");return 96;
			}
		}
		if(const char* workerEnvironment=std::getenv("RISE_FIRE_BUDGET_WORKERS")){
			char* end=0;errno=0;const unsigned long parsed=std::strtoul(workerEnvironment,&end,10);
			if(errno||end==workerEnvironment||*end!='\0'||parsed==0u||parsed>64u){
				std::fprintf(stderr,"RISE_FIRE_BUDGET_WORKERS is invalid\n");return 96;
			}
			probeWorkers=static_cast<unsigned int>(parsed);
		}
		RunPersistenceOptions probePersistence;
		if(const char* checkpointEnvironment=std::getenv("RISE_FIRE_BUDGET_CHECKPOINT")){
			if(!*checkpointEnvironment){std::fprintf(stderr,
				"RISE_FIRE_BUDGET_CHECKPOINT is empty\n");return 96;}
			probePersistence.checkpointPath=checkpointEnvironment;
			probePersistence.checkpointCadenceWallS=900.0;
			probePersistence.resume=std::filesystem::exists(probePersistence.checkpointPath);
		}
		const SolverFrameValues probe=RunMethaneFrameProbe(probeWorkers,1u,targetS,
			std::max(1.0,targetS),1.0/std::max(1.0,targetS),6.0,
			CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,probePersistence);
		const double minimumAcceptedStep=probe.acceptedTimeStepHistoryS.empty()?0.0:
			*std::min_element(probe.acceptedTimeStepHistoryS.begin(),
				probe.acceptedTimeStepHistoryS.end());
		const double maximumAcceptedStep=probe.acceptedTimeStepHistoryS.empty()?0.0:
			*std::max_element(probe.acceptedTimeStepHistoryS.begin(),
				probe.acceptedTimeStepHistoryS.end());
		std::fprintf(stderr,"budget probe target=%.17g workers=%u succeeded=%d steps=%zu time=%.17g "
			"dt=%.17g dt_min=%.17g dt_max=%.17g eos_max=%.17g "
			"hold_min=%.17g hold_max=%.17g approach=%d hold_seen=%d hold_ok=%d ignition=%d "
			"sustained=%d error=%s\n",targetS,probeWorkers,probe.succeeded?1:0,
			probe.acceptedMaximumEOSResidualHistory.size(),probe.simulatedTimeS,
			probe.acceptedTimeStepS,minimumAcceptedStep,maximumAcceptedStep,
			probe.maximumAcceptedEOSResidual,probe.minimumActiveHoldTemperatureK,
			probe.maximumActiveHoldTemperatureK,
			probe.pilotApproachComplete?1:0,probe.pilotHoldBandObserved?1:0,
			probe.pilotHoldBandSatisfied?1:0,probe.ignitedDuringPilot?1:0,
			probe.sustainedAfterPilot?1:0,
			probe.structuredError.c_str());
		const std::size_t first=probe.acceptedMaximumEOSResidualHistory.size()>64u?
			probe.acceptedMaximumEOSResidualHistory.size()-64u:0u;
		for(std::size_t step=first;step<probe.acceptedMaximumEOSResidualHistory.size();++step)
			std::fprintf(stderr,"budget history step=%zu eos_max=%.17g\n",step+1u,
				probe.acceptedMaximumEOSResidualHistory[step]);
		return probe.succeeded?0:97;
	}
	if(argc==1)Check(FireProductionDyadicCalibration::DiagnoseRoundoff(
		"rendered/fire_production_calibration/r112_dyadic_smooth_open",
		"42185c882c52e8c94db4b58f40674c53341eabe1b75b6922fdd1c7f56415a4ed",
		"d4947cb8eedbc57732190bf1833e68c3f83a356346c1662db321d7831bce958b")==237,
		"r136 full-step a-priori refusal remains an exact normal-suite gate");
#endif
#if !defined(RISE_ENABLE_OPENVDB)
	if(argc==5&&std::strcmp(argv[1],"--fire-r80-golden-continuation")==0)
		return RunR80GoldenContinuationFixture(argv[2],argv[3],argv[4]);
	if(argc==4&&std::strcmp(argv[1],"--fire-production-golden-composition")==0)
		return RunProductionGoldenCompositionFixture(argv[2],argv[3]);
	if(argc==7&&std::strcmp(argv[1],"--fire-production-calibration-measure-subdominance")==0)
		return FireProductionDyadicCalibration::MeasureProductionSubdominance(
			argv[2],argv[3],argv[4],argv[5],argv[6]);
#endif
	std::string identityFailure;
	Check(!DiscontinuousThreadIdentityAccepted(false,true,identityFailure)&&
		identityFailure=="active_set_thread_identity_mismatch"&&
		DiscontinuousThreadIdentityAccepted(true,true,identityFailure),
		"r81 active-set 1-vs-N mismatch is a structured fail-closed run error");
	PointLight* directLight=new PointLight(1.0,RISEPel(1,1,1),false);
	IKeyframeParameter* lightEnergy=directLight->KeyframeFromParameters("energy","7");
	UniformColorPainter* colorA=new UniformColorPainter(RISEPel(0,0,0));
	UniformColorPainter* colorB=new UniformColorPainter(RISEPel(1,1,1));
	FrozenPainterProbe* painter=new FrozenPainterProbe(*colorA,*colorB);
	FrozenUniformProbe* uniform=new FrozenUniformProbe();
	colorA->release(); colorB->release();
	IKeyframeParameter* painterScale=painter->KeyframeFromParameters("scale","3 4 5");
	IKeyframeParameter* uniformColor=uniform->KeyframeFromParameters("risepel","1 0 0");
	const void* originalFunction=painter->Function();
	Transformable::BeginPreparedMutationFreeze();
	directLight->SetIntermediateValue(*lightEnergy);
	directLight->SetCanGeneratePhotons(true);
	painter->SetIntermediateValue(*painterScale);
	painter->RegenerateData();
	uniform->SetIntermediateValue(*uniformColor);
	Check(directLight->emissionEnergy()==1.0 && !directLight->CanGeneratePhotons() &&
		painter->Scale().x==1.0 &&
		painter->Function()==originalFunction && uniform->Value().g==0.5,
		"direct light and painter keyframe/data mutations fail fast under prepared freeze");
	Transformable::EndPreparedMutationFreeze();
	lightEnergy->release(); painterScale->release(); uniformColor->release();
	directLight->release(); painter->release(); uniform->release();
#if !defined(RISE_ENABLE_OPENVDB)
	std::string error;
	FireSequenceManifest manifest;
	const std::string digest(64u,'a');
	const RISECBOR64::Bytes envelope=ManifestBytes(digest,digest);
	FireSequenceMappedTime mapped;
	FireSequencePreparedFrame frame;
	Check(manifest.LoadCanonicalEnvelope(envelope,".",error) &&
		manifest.MapSceneTime(10.0,mapped,error),
		"capability-independent manifest/time contract remains available");
	Check(!manifest.LoadFrame(4,frame,error) &&
		error=="fire_sequence_openvdb_capability_unavailable",
		"OpenVDB-disabled build fails with the explicit capability result");
	RISECBOR64::Value decodedEnvelope;
	Check(RISECBOR64::DecodeCanonical(envelope,decodedEnvelope,&error),
		"capability-disabled envelope still decodes canonically");
	const RISECBOR64::Value* decodedPayload=decodedEnvelope.Find("payload");
	Check(decodedPayload && !FireSequenceManifest().LoadCanonicalEnvelope(
		EnvelopeForPayload(ReplaceMember(*decodedPayload,"frame_encoding",
			RISECBOR64::Value::String("raw-dense-v1"))),".",error),
		"capability-disabled build still enforces the exact manifest schema");
	Check(decodedPayload && !FireSequenceManifest().LoadCanonicalEnvelope(
		EnvelopeForPayload(ReplaceMember(*decodedPayload,"physical_mapping",
			RISECBOR64::Value::String("heuristic:bogus"))),".",error),
		"capability-disabled build still rejects unratified physical mappings");
	Check(manifest.MapSceneTime(10.25,mapped,error) && mapped.baseFrameIndex==5,
		"capability-disabled build still applies the complete time map");
	FireSequencePreparationController disabledController(manifest);
	Check(disabledController.SetFrameInstaller([](const FireSequencePreparedFrame&) {
		return true;
	},std::string(64u,'b'),error) && disabledController.SetActiveBindingIdentity(
		std::string(64u,'c'),error),
		"capability-disabled controller accepts only its immutable identity inputs");
	Check(!disabledController.PrepareMediaForRender(
		FireSequenceRenderTimeSupport{10.0,10.0,10.0},false,error) &&
		error=="fire_sequence_openvdb_capability_unavailable" &&
		disabledController.Generation()==0u,
		"capability-disabled preparation fails transactionally before publication");
	std::printf("FireSequenceTest: OpenVDB capability-unavailable gates passed\n");
	return failures ? 1 : 0;
#else
	const std::filesystem::path root = std::filesystem::temp_directory_path()/
		("rise-fire-sequence-test-"+std::to_string(static_cast<unsigned long long>(
			std::chrono::high_resolution_clock::now().time_since_epoch().count())));
	std::filesystem::create_directories(root);
	const std::filesystem::path checkpointFixture=root/"checkpoint_fixture";
	std::filesystem::create_directories(checkpointFixture);
	RunPersistenceOptions identityMismatchPersistence;
	identityMismatchPersistence.checkpointPath=checkpointFixture/"identity_mismatch.checkpoint";
	identityMismatchPersistence.checkpointCadenceWallS=0.0;
	identityMismatchPersistence.forceActiveSetIdentityCheckForTest=true;
	identityMismatchPersistence.injectActiveSetIdentityMismatchForTest=true;
	const SolverFrameValues identityMismatch=RunMethaneFrameProbe(4u,2u,0.0,1.0,
		4.0,6.0,CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,
		identityMismatchPersistence);
	Check(!identityMismatch.succeeded&&identityMismatch.structuredError==
		"solver_failure:active_set_thread_identity_mismatch"&&
		identityMismatch.discontinuousActiveSetEvents==0u&&
		!std::filesystem::exists(identityMismatchPersistence.checkpointPath),
		"r81 injected production-path active-set mismatch fails before accumulation and checkpoint publication");
	const std::filesystem::path checkpointPath=checkpointFixture/"run.checkpoint";
	const std::filesystem::path baselineCheckpointFrame=checkpointFixture/"baseline.vdb";
	const std::filesystem::path resumedCheckpointFrame=checkpointFixture/"resumed.vdb";
	const std::filesystem::path self=std::filesystem::absolute(argv[0]);
	const int baselineCheckpointExit=RunCheckpointSubprocess(self,"baseline",checkpointPath,
		baselineCheckpointFrame,1u);
	std::string checkpointFixtureError;
	const std::filesystem::path streamedPrefixFrame=checkpointFixture/"streamed_prefix_frame4.vdb";
	const bool streamedPrefixPublished=baselineCheckpointExit==0&&
		DurableCopyPublishedFile(baselineCheckpointFrame,streamedPrefixFrame,
			checkpointFixtureError);
	const std::filesystem::path syncFailureCheckpoint=checkpointFixture/"sync_failure.checkpoint";
	const std::filesystem::path syncFailureReturnedMarker=checkpointFixture/"sync_failure.returned";
	const int syncFailureExit=RunCheckpointSubprocess(self,"syncfail",syncFailureCheckpoint,
		syncFailureReturnedMarker,2u);
	Check(syncFailureExit==0&&std::filesystem::exists(syncFailureReturnedMarker),
		"r61 post-rename directory-sync failure returns a structured error instead of reporting durability or killing");
	const int killedCheckpointExit=RunCheckpointSubprocess(self,"kill",checkpointPath,
		resumedCheckpointFrame,2u);
	Check(baselineCheckpointExit==0&&CheckpointSubprocessWasHardKilled(killedCheckpointExit)&&
		streamedPrefixPublished&&DigestFile(streamedPrefixFrame)==DigestFile(baselineCheckpointFrame)&&
		std::filesystem::exists(checkpointPath)&&!std::filesystem::exists(resumedCheckpointFrame),
		"r61 streamed prefix survives a hard kill before the final frame is published");
	MethaneRunCheckpoint oneStepCheckpointMetadata;
	Check(LoadMethaneRunCheckpoint(checkpointPath,oneStepCheckpointMetadata,
		checkpointFixtureError)&&oneStepCheckpointMetadata.acceptedSteps==1u,
		"r115 binary32 resume fixture captures the immutable one-step beginning");
	const int resumedCheckpointExit=RunCheckpointSubprocess(self,"resume",checkpointPath,
		resumedCheckpointFrame,4u);
	MethaneRunCheckpoint resumedCheckpointMetadata;
	const bool checkpointMetadataLoaded=LoadMethaneRunCheckpoint(checkpointPath,
		resumedCheckpointMetadata,checkpointFixtureError);
	if(!(resumedCheckpointExit==0&&checkpointMetadataLoaded&&
		resumedCheckpointMetadata.values.checkpointStepIndices==
			std::vector<std::uint64_t>({1u,2u})&&
		resumedCheckpointMetadata.values.workerCountHistory==
			std::vector<std::uint64_t>({2u,4u}))){
		std::fprintf(stderr,"checkpoint fixture diagnostic: resume_exit=%d loaded=%d error=%s "
			"resumed=%d resumed_step=%llu checkpoint_count=%zu worker_count=%zu\n",
			resumedCheckpointExit,checkpointMetadataLoaded?1:0,checkpointFixtureError.c_str(),
			resumedCheckpointMetadata.values.resumedFromCheckpoint?1:0,
			static_cast<unsigned long long>(resumedCheckpointMetadata.values.resumedFromStep),
			resumedCheckpointMetadata.values.checkpointStepIndices.size(),
			resumedCheckpointMetadata.values.workerCountHistory.size());
	}
	Check(resumedCheckpointExit==0&&DigestFile(baselineCheckpointFrame)==
		DigestFile(resumedCheckpointFrame)&&checkpointMetadataLoaded&&
		resumedCheckpointMetadata.values.resumedFromCheckpoint&&
		resumedCheckpointMetadata.values.resumedFromStep==1u&&
		!resumedCheckpointMetadata.producerBuildId.empty()&&
		resumedCheckpointMetadata.values.checkpointCadenceWallS==0.0&&
		resumedCheckpointMetadata.values.checkpointStepIndices==
			std::vector<std::uint64_t>({1u,2u})&&
		resumedCheckpointMetadata.values.workerCountHistory==
			std::vector<std::uint64_t>({2u,4u})&&
		resumedCheckpointMetadata.values.reductionMode=="fixed_order_tree_v1",
		"r61 checkpoint plus hard kill plus different-thread resume is frame-bit-transparent and records run events");
	FireStateProducerPrecision resumedPrecision=FireStateProducerPrecision::Unknown;
	Check(HomogeneousStateProducerPrecision(resumedCheckpointMetadata.states,resumedPrecision)&&
		resumedPrecision==FireStateProducerPrecision::Binary64,
		"r115 ordinary binary64 checkpoints retain one authoritative producer class");
	for(std::uint64_t legacyVersion=5u;legacyVersion<=8u;++legacyVersion){
		const std::filesystem::path legacyPrecisionCheckpoint=checkpointFixture/
			("precision_class_legacy_v"+std::to_string(legacyVersion)+".checkpoint");
		MethaneRunCheckpoint loadedLegacyPrecision;
		loadedLegacyPrecision.states.assign(resumedCheckpointMetadata.states.size(),MethaneCellState());
		for(MethaneCellState& state:loadedLegacyPrecision.states)
			state.producerPrecision=FireStateProducerPrecision::Unknown;
		Check(SaveMethaneRunCheckpoint(legacyPrecisionCheckpoint,resumedCheckpointMetadata,
			checkpointFixtureError,legacyVersion)&&
			LoadMethaneRunCheckpoint(legacyPrecisionCheckpoint,loadedLegacyPrecision,
				checkpointFixtureError)&&
			loadedLegacyPrecision.checkpointFormatVersion==legacyVersion&&
			HomogeneousStateProducerPrecision(loadedLegacyPrecision.states,resumedPrecision)&&
			resumedPrecision==FireStateProducerPrecision::Binary64,
			"r115 every legacy v5-v8 checkpoint decodes in the binary64 producer class");
	}
	MethaneRunCheckpoint mixedPrecisionCheckpoint=resumedCheckpointMetadata;
	if(!mixedPrecisionCheckpoint.states.empty())mixedPrecisionCheckpoint.states.front().producerPrecision=
		FireStateProducerPrecision::Binary32;
	const std::filesystem::path rejectedMixedPrecision=
		checkpointFixture/"mixed_precision.checkpoint";
	Check(mixedPrecisionCheckpoint.states.size()>1u&&
		!SaveMethaneRunCheckpoint(rejectedMixedPrecision,mixedPrecisionCheckpoint,
			checkpointFixtureError)&&!std::filesystem::exists(rejectedMixedPrecision),
		"r115 a composed checkpoint rejects mixed producer precision before publication");
	MethaneRunCheckpoint precisionRoundTrip=oneStepCheckpointMetadata;
	const FireSimulationMethaneRecord& checkpointFuel=FireSimulationMethaneRecord::PhysicalV1();
	MethaneCellState checkpointAmbient;checkpointAmbient.temperatureK=300.0;
	double checkpointInvW=0.0;
	for(std::size_t species=0;species<MethaneSpeciesCount;++species){
		checkpointAmbient.constituent[species]=checkpointFuel.AmbientMassFractions()[species];
		if(species<MethaneCarbon){const FireThermochemistrySpecies* property=
			checkpointFuel.FindSpecies(checkpointFuel.SpeciesOrder()[species].c_str());
			if(property)checkpointInvW+=checkpointAmbient.constituent[species]/
				property->molecularWeightKGPerKMol;}
	}
	const double checkpointRho=checkpointFuel.ThermodynamicPressurePa()/(8314.46261815324*
		checkpointAmbient.temperatureK*checkpointInvW);
	for(double& density:checkpointAmbient.constituent)density*=checkpointRho;
	checkpointAmbient.rhoTotalZ=0.0;
	Check(checkpointFuel.MixtureSensibleEnergyJPerM3(
		ThermochemicalDensities(checkpointAmbient),checkpointAmbient.temperatureK,
		checkpointAmbient.sensibleEnergyJPerM3,&checkpointFixtureError),
		"r115 binary32 resume fixture reconstructs the canonical ambient state");
	checkpointAmbient.producerPrecision=FireStateProducerPrecision::Binary32;
	precisionRoundTrip.states.assign(precisionRoundTrip.states.size(),checkpointAmbient);
	for(unsigned int axis=0;axis<3u;++axis){
		std::fill(precisionRoundTrip.momentum.component[axis].begin(),
			precisionRoundTrip.momentum.component[axis].end(),0.0);
		std::fill(precisionRoundTrip.velocity.component[axis].begin(),
			precisionRoundTrip.velocity.component[axis].end(),0.0);
	}
	precisionRoundTrip.centerlineTemperatureIntegral.assign(
		precisionRoundTrip.centerlineTemperatureIntegral.size(),0.0);
	precisionRoundTrip.centerlineVelocityIntegral.assign(
		precisionRoundTrip.centerlineVelocityIntegral.size(),0.0);
	precisionRoundTrip.planeHeatReleaseIntegral.assign(
		precisionRoundTrip.planeHeatReleaseIntegral.size(),0.0);
	precisionRoundTrip.centerlineStatisticsDurationS=0.0;
	precisionRoundTrip.acceptedSteps=0u;
	precisionRoundTrip.simulationTimeS=0.0;
	precisionRoundTrip.previousStepS=0.0;
	precisionRoundTrip.lastAcceptedStepS=0.0;
	precisionRoundTrip.values.acceptedTimeStepHistoryS.clear();
	if(!precisionRoundTrip.states.empty()){
		const double excursion=0.5*AcceptedStateRoundoffFactor(
			checkpointFuel.AcceptedStateFeasibilityEnvelope(),FireStateProducerPrecision::Binary32)*
			AcceptedStateMassScale(ToConservativeVector(precisionRoundTrip.states.front()));
		precisionRoundTrip.states.front().rhoTotalZ+=excursion;
	}
	MethaneCellState binary64View=precisionRoundTrip.states.front();
	binary64View.producerPrecision=FireStateProducerPrecision::Binary64;
	for(std::uint64_t legacyVersion=5u;legacyVersion<=8u;++legacyVersion){
		const std::filesystem::path rejectedBinary32Legacy=checkpointFixture/
			("binary32_legacy_v"+std::to_string(legacyVersion)+".checkpoint");
		const std::filesystem::path rejectedMixedLegacy=checkpointFixture/
			("mixed_legacy_v"+std::to_string(legacyVersion)+".checkpoint");
		Check(!SaveMethaneRunCheckpoint(rejectedBinary32Legacy,precisionRoundTrip,
			checkpointFixtureError,legacyVersion)&&
			!std::filesystem::exists(rejectedBinary32Legacy)&&
			!SaveMethaneRunCheckpoint(rejectedMixedLegacy,mixedPrecisionCheckpoint,
				checkpointFixtureError,legacyVersion)&&
			!std::filesystem::exists(rejectedMixedLegacy),
			"r115 legacy v5-v8 publication rejects binary32 and mixed producer classes");
	}
	const std::filesystem::path precisionCheckpoint=checkpointFixture/"precision_class.checkpoint";
	const std::filesystem::path precisionFrame=checkpointFixture/"precision_class.vdb";
	MethaneRunCheckpoint loadedPrecisionRoundTrip;
	const bool precisionSave=!precisionRoundTrip.states.empty()&&
		AcceptedMethaneCellStateAdmissible(precisionRoundTrip.states.front(),checkpointFuel,
			&checkpointFixtureError)&&
		!AcceptedMethaneCellStateAdmissible(binary64View,checkpointFuel,&checkpointFixtureError)&&
		SaveMethaneRunCheckpoint(precisionCheckpoint,
		precisionRoundTrip,checkpointFixtureError)&&LoadMethaneRunCheckpoint(precisionCheckpoint,
		loadedPrecisionRoundTrip,checkpointFixtureError);
	FireStateProducerPrecision loadedPrecision=FireStateProducerPrecision::Unknown;
	const int precisionResumeExit=precisionSave?RunCheckpointSubprocess(self,
		"resume-one-fp64-reject",
		precisionCheckpoint,precisionFrame,4u):-1;
	MethaneRunCheckpoint resumedPrecisionRoundTrip;
	if(!(precisionSave&&precisionResumeExit==0&&
		LoadMethaneRunCheckpoint(precisionCheckpoint,resumedPrecisionRoundTrip,
			checkpointFixtureError))){
		std::fprintf(stderr,"precision checkpoint diagnostic: save=%d resume=%d error=%s\n",
			precisionSave?1:0,precisionResumeExit,checkpointFixtureError.c_str());
	}
	Check(precisionSave&&
		loadedPrecisionRoundTrip.checkpointFormatVersion==12u&&
		HomogeneousStateProducerPrecision(loadedPrecisionRoundTrip.states,loadedPrecision)&&
		loadedPrecision==FireStateProducerPrecision::Binary32&&
		!loadedPrecisionRoundTrip.productionManifoldObservation.Available()&&
		precisionResumeExit==0&&
		LoadMethaneRunCheckpoint(precisionCheckpoint,resumedPrecisionRoundTrip,
			checkpointFixtureError)&&
		HomogeneousStateProducerPrecision(resumedPrecisionRoundTrip.states,loadedPrecision)&&
		loadedPrecision==FireStateProducerPrecision::Binary32&&
		resumedPrecisionRoundTrip.acceptedSteps==precisionRoundTrip.acceptedSteps,
		"r115 binary32 checkpoint metadata survives serialization and a binary64 CPU resume cannot relabel the inherited excursion");
	const std::filesystem::path version9Checkpoint=checkpointFixture/"precision_class_v9.checkpoint";
	const std::filesystem::path version9ZeroCountCheckpoint=
		checkpointFixture/"precision_class_v9_zero_count.checkpoint";
	const std::filesystem::path version10Checkpoint=checkpointFixture/"precision_class_v10.checkpoint";
	const std::filesystem::path version11Checkpoint=checkpointFixture/"precision_class_v11.checkpoint";
	MethaneRunCheckpoint legacyAccepted=precisionRoundTrip;
	legacyAccepted.acceptedSteps=1u;legacyAccepted.simulationTimeS=0.001;
	legacyAccepted.previousStepS=0.0;legacyAccepted.lastAcceptedStepS=0.0;
	legacyAccepted.values.acceptedTimeStepHistoryS.push_back(0.001);
	MethaneRunCheckpoint rejectedLegacy;
	const std::filesystem::path unavailableV12Checkpoint=
		checkpointFixture/"precision_class_unavailable_v12.checkpoint";
	const bool unavailableAcceptedV12Rejected=
		!SaveMethaneRunCheckpoint(unavailableV12Checkpoint,legacyAccepted,
			checkpointFixtureError,12u)&&!std::filesystem::exists(unavailableV12Checkpoint);
	forceMalformedManifoldLifecycleWriteForTest=true;
	const bool malformedVersion9Written=SaveMethaneRunCheckpoint(version9Checkpoint,
		legacyAccepted,checkpointFixtureError,9u);
	MethaneRunCheckpoint legacyZeroCount=precisionRoundTrip;
	legacyZeroCount.simulationTimeS=0.001;
	legacyZeroCount.values.acceptedTimeStepHistoryS.push_back(0.001);
	const bool malformedVersion9ZeroCountWritten=SaveMethaneRunCheckpoint(
		version9ZeroCountCheckpoint,legacyZeroCount,checkpointFixtureError,9u);
	forceMalformedManifoldLifecycleWriteForTest=false;
	Check(malformedVersion9Written&&!LoadMethaneRunCheckpoint(version9Checkpoint,rejectedLegacy,
		checkpointFixtureError)&&malformedVersion9ZeroCountWritten&&
		!LoadMethaneRunCheckpoint(version9ZeroCountCheckpoint,rejectedLegacy,
		checkpointFixtureError)&&SaveMethaneRunCheckpoint(version10Checkpoint,legacyAccepted,
		checkpointFixtureError,10u)&&!LoadMethaneRunCheckpoint(version10Checkpoint,rejectedLegacy,
		checkpointFixtureError)&&SaveMethaneRunCheckpoint(version11Checkpoint,legacyAccepted,
		checkpointFixtureError,11u)&&!LoadMethaneRunCheckpoint(version11Checkpoint,rejectedLegacy,
		checkpointFixtureError)&&unavailableAcceptedV12Rejected,
		"r148 checksum-valid v9-v11 production resumes cannot alias accepted history to a first step");
	RISECBOR64::Bytes corruptedCheckpoint=ReadFileBytes(checkpointPath);
	if(!corruptedCheckpoint.empty())corruptedCheckpoint.back()^=0x01u;
	const std::filesystem::path corruptedCheckpointPath=checkpointFixture/"corrupt.checkpoint";
	{std::ofstream output(corruptedCheckpointPath,std::ios::binary|std::ios::trunc);
		output.write(reinterpret_cast<const char*>(corruptedCheckpoint.data()),
			static_cast<std::streamsize>(corruptedCheckpoint.size()));}
	MethaneRunCheckpoint rejectedCheckpoint;
	Check(!LoadMethaneRunCheckpoint(corruptedCheckpointPath,rejectedCheckpoint,
		checkpointFixtureError),"r61 corrupt checkpoint fails closed before resume");
	RISECBOR64::Bytes corruptEarlySize=ReadFileBytes(checkpointPath);
	if(corruptEarlySize.size()>48u)corruptEarlySize[48]^=0xffu;
	const std::filesystem::path corruptEarlySizePath=checkpointFixture/"corrupt_early_size.checkpoint";
	{std::ofstream output(corruptEarlySizePath,std::ios::binary|std::ios::trunc);
		output.write(reinterpret_cast<const char*>(corruptEarlySize.data()),
			static_cast<std::streamsize>(corruptEarlySize.size()));}
	Check(!LoadMethaneRunCheckpoint(corruptEarlySizePath,rejectedCheckpoint,
		checkpointFixtureError)&&checkpointFixtureError=="run checkpoint checksum mismatch",
		"r61 validates the payload checksum before a corrupted size can allocate memory");
	const std::filesystem::path trailingCheckpointPath=checkpointFixture/"trailing.checkpoint";
	corruptedCheckpoint=ReadFileBytes(checkpointPath);corruptedCheckpoint.push_back(0u);
	{std::ofstream output(trailingCheckpointPath,std::ios::binary|std::ios::trunc);
		output.write(reinterpret_cast<const char*>(corruptedCheckpoint.data()),
			static_cast<std::streamsize>(corruptedCheckpoint.size()));}
	Check(!LoadMethaneRunCheckpoint(trailingCheckpointPath,rejectedCheckpoint,
		checkpointFixtureError),"r61 checkpoint rejects a valid payload with trailing bytes");
	auto ValidMutatedCheckpointRejects=[&](const char* name,
		const MethaneRunCheckpoint& mutated)->bool{
		const std::filesystem::path path=checkpointFixture/(std::string(name)+".checkpoint");
		if(!SaveMethaneRunCheckpoint(path,mutated,checkpointFixtureError))return false;
		RunPersistenceOptions persistence;persistence.checkpointPath=path;persistence.resume=true;
		const SolverFrameValues attempt=RunMethaneFrameProbe(3u,3u,0.0,1.0,4.0,6.0,
			CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,persistence);
		return !attempt.succeeded&&attempt.structuredError.find("checkpoint_resume_failure:")==0;
	};
	MethaneRunCheckpoint bindingMutation=resumedCheckpointMetadata;
	bindingMutation.producerBuildId=std::string(64u,'0');
	Check(ValidMutatedCheckpointRejects("wrong_build",bindingMutation),
		"r61 resume rejects a checksummed checkpoint from a different executable build");
	bindingMutation=resumedCheckpointMetadata;
	bindingMutation.caseRecordId=std::string(64u,'0');
	Check(ValidMutatedCheckpointRejects("wrong_case",bindingMutation),
		"r61 resume rejects a checksummed checkpoint for a different case identity");
	bindingMutation=resumedCheckpointMetadata;
	bindingMutation.values.reductionMode="unordered_reduction";
	Check(ValidMutatedCheckpointRejects("wrong_reduction",bindingMutation),
		"r61 resume rejects a checksummed checkpoint with a different reduction mode");
	bindingMutation=resumedCheckpointMetadata;
	bindingMutation.values.activeSetAlgorithmVersion="mutated_active_set_algorithm";
	Check(ValidMutatedCheckpointRejects("wrong_active_set_algorithm",bindingMutation),
		"r81 resume rejects a checksummed checkpoint with different active-set semantics");
	bindingMutation=resumedCheckpointMetadata;
	bindingMutation.values.priorActiveSetAlgorithmVersion="fabricated_prior_algorithm";
	Check(ValidMutatedCheckpointRejects("wrong_prior_active_set_algorithm",bindingMutation),
		"r81 resume rejects a checksummed checkpoint with fabricated prior active-set history");
	RISECBOR64::Bytes migrationBuildRecord;std::string migrationNewBuildId,
		migrationNewExecutableDigest;
	Check(CurrentRendererBuildIdentity(migrationBuildRecord,migrationNewBuildId)&&
		CurrentExecutableDigest(migrationBuildRecord,migrationNewExecutableDigest,
			checkpointFixtureError),
		"r78 migration fixture resolves the exact current build and executable identities");
	MethaneRunCheckpoint migrationSource=resumedCheckpointMetadata;
	migrationSource.producerBuildId=std::string(64u,'a');
	const std::filesystem::path migrationCheckpoint=checkpointFixture/"migration_source.checkpoint";
	Check(SaveMethaneRunCheckpoint(migrationCheckpoint,migrationSource,checkpointFixtureError),
		"r78 migration fixture authors a valid foreign-build checkpoint");
	ResumeEquivalenceTrace oldMigrationTrace,newMigrationTrace;
	oldMigrationTrace.checkpointDigest=DigestFile(migrationCheckpoint);
	oldMigrationTrace.checkpointProducerBuildId=migrationSource.producerBuildId;
	oldMigrationTrace.buildId=migrationSource.producerBuildId;
	oldMigrationTrace.executableDigest=std::string(64u,'b');
	oldMigrationTrace.resumedFromStep=migrationSource.acceptedSteps;
	oldMigrationTrace.acceptedStepCount=8u;
	for(std::uint64_t step=0u;step<8u;++step){
		oldMigrationTrace.timeStepBits.push_back(DoubleBits(0.001+1.0e-6*step));
		oldMigrationTrace.maximumTemperatureBits.push_back(DoubleBits(900.0+step));
		oldMigrationTrace.maximumEOSResidualBits.push_back(DoubleBits(1.0e-7*(step+1u)));
	}
	oldMigrationTrace.frameDigests.assign(8u,DigestFile(resumedCheckpointFrame));
	newMigrationTrace=oldMigrationTrace;newMigrationTrace.buildId=migrationNewBuildId;
	newMigrationTrace.executableDigest=migrationNewExecutableDigest;
	const std::filesystem::path migrationCertificatePath=
		checkpointFixture/"resume_equivalence.cbor";
	ResumeEquivalenceCertificate migrationCertificate;
	Check(BuildResumeEquivalenceCertificate(oldMigrationTrace,newMigrationTrace,
		migrationCertificatePath,migrationCertificate,checkpointFixtureError)&&
		migrationCertificate.acceptedStepCount==8u,
		"r78 certificate requires eight bit-identical continuation steps and a frame digest");
	RunPersistenceOptions certifiedPersistence;
	certifiedPersistence.checkpointPath=migrationCheckpoint;
	certifiedPersistence.resume=true;
	certifiedPersistence.resumeEquivalenceCertificatePath=migrationCertificatePath;
	const SolverFrameValues certifiedMigration=RunMethaneFrameProbe(3u,3u,0.0,1.0,4.0,6.0,
		CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,certifiedPersistence);
	Check(certifiedMigration.succeeded&&certifiedMigration.migrationCertificateId==
		migrationCertificate.certificateId&&certifiedMigration.migrationOldBuildId==
		migrationSource.producerBuildId&&certifiedMigration.migrationNewBuildId==
		migrationNewBuildId&&certifiedMigration.migrationAcceptedStepCount==8u,
		"r78 a foreign checkpoint resumes only through its exact current-build certificate");
	ResumeEquivalenceCertificate rejectedMigrationCertificate;
	auto TraceMutationRejects=[&](const char* name,const ResumeEquivalenceTrace& trace)->bool{
		return !BuildResumeEquivalenceCertificate(oldMigrationTrace,trace,
			checkpointFixture/(std::string(name)+".cbor"),rejectedMigrationCertificate,
			checkpointFixtureError);
	};
	ResumeEquivalenceTrace mismatchedTrace=newMigrationTrace;
	mismatchedTrace.timeStepBits[3]^=1u;
	Check(TraceMutationRejects("mismatched_dt",mismatchedTrace),
		"r78 a one-bit timestep difference rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.maximumTemperatureBits[3]^=1u;
	Check(TraceMutationRejects("mismatched_temperature",mismatchedTrace),
		"r78 a one-bit per-step T_max difference rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.maximumEOSResidualBits[3]^=1u;
	Check(TraceMutationRejects("mismatched_eos",mismatchedTrace),
		"r78 a one-bit EOS-maximum difference rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.frameDigests[0]=std::string(64u,'c');
	Check(TraceMutationRejects("mismatched_frame",mismatchedTrace),
		"r78 a diagnostic-frame digest difference rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.timeStepBits.pop_back();
	Check(TraceMutationRejects("missing_step",mismatchedTrace),
		"r78 missing per-step evidence rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.timeStepBits.push_back(0u);
	Check(TraceMutationRejects("extra_step",mismatchedTrace),
		"r78 extra timestep evidence rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.maximumTemperatureBits.pop_back();
	Check(TraceMutationRejects("missing_temperature",mismatchedTrace),
		"r78 missing per-step T_max evidence rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.maximumTemperatureBits.push_back(0u);
	Check(TraceMutationRejects("extra_temperature",mismatchedTrace),
		"r78 extra per-step evidence rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.maximumEOSResidualBits.pop_back();
	Check(TraceMutationRejects("missing_eos",mismatchedTrace),
		"r78 missing per-step EOS evidence rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.maximumEOSResidualBits.push_back(0u);
	Check(TraceMutationRejects("extra_eos",mismatchedTrace),
		"r78 extra per-step EOS evidence rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.frameDigests.pop_back();
	Check(TraceMutationRejects("missing_frame",mismatchedTrace),
		"r78 missing per-step frame evidence rejects build migration");
	mismatchedTrace=newMigrationTrace;mismatchedTrace.frameDigests.push_back(std::string(64u,'c'));
	Check(TraceMutationRejects("extra_frame",mismatchedTrace),
		"r78 extra per-step frame evidence rejects build migration");
	auto CertificateMutationRejects=[&](const char* name,
		ResumeEquivalenceCertificate certificate)->bool{
		const std::filesystem::path path=checkpointFixture/(std::string(name)+".cbor");
		if(!SaveResumeEquivalenceCertificate(path,certificate,checkpointFixtureError))return false;
		RunPersistenceOptions persistence;
		persistence.checkpointPath=migrationCheckpoint;persistence.resume=true;
		persistence.resumeEquivalenceCertificatePath=path;
		const SolverFrameValues attempt=RunMethaneFrameProbe(3u,3u,0.0,1.0,4.0,6.0,
			CapstonePoolDiameterM,CapstoneHeatReleaseRateKW,false,persistence);
		return !attempt.succeeded&&
			attempt.structuredError.find("checkpoint_resume_failure:")==0;
	};
	ResumeEquivalenceCertificate mutatedCertificate=migrationCertificate;
	mutatedCertificate.checkpointDigest=std::string(64u,'c');
	Check(CertificateMutationRejects("wrong_migration_checkpoint",mutatedCertificate),
		"r78 production resume binds the certificate to the exact checkpoint digest");
	mutatedCertificate=migrationCertificate;mutatedCertificate.oldBuildId=std::string(64u,'c');
	Check(CertificateMutationRejects("wrong_migration_old_build",mutatedCertificate),
		"r78 production resume binds the certificate to the checkpoint producer build");
	mutatedCertificate=migrationCertificate;mutatedCertificate.newBuildId=std::string(64u,'c');
	Check(CertificateMutationRejects("wrong_migration_new_build",mutatedCertificate),
		"r78 production resume binds the certificate to the current build identity");
	mutatedCertificate=migrationCertificate;
	mutatedCertificate.newExecutableDigest=std::string(64u,'c');
	Check(CertificateMutationRejects("wrong_migration_executable",mutatedCertificate),
		"r78 production resume binds the certificate to the current executable digest");
	mutatedCertificate=migrationCertificate;++mutatedCertificate.resumedFromStep;
	Check(CertificateMutationRejects("wrong_migration_step",mutatedCertificate),
		"r78 production resume binds the certificate to the exact resumed step");
	mutatedCertificate=migrationCertificate;mutatedCertificate.acceptedStepCount=7u;
	mutatedCertificate.timeStepBits.resize(7u);
	mutatedCertificate.maximumTemperatureBits.resize(7u);
	mutatedCertificate.maximumEOSResidualBits.resize(7u);
	Check(CertificateMutationRejects("short_migration_evidence",mutatedCertificate),
		"r78 production resume rejects fewer than eight certified accepted steps");
	SolverFrameValues metadataFixture=resumedCheckpointMetadata.values;
	metadataFixture.streamedFrameCount=2u;
	metadataFixture.migrationCertificateId=certifiedMigration.migrationCertificateId;
	metadataFixture.migrationOldBuildId=certifiedMigration.migrationOldBuildId;
	metadataFixture.migrationNewBuildId=certifiedMigration.migrationNewBuildId;
	metadataFixture.migrationAcceptedStepCount=certifiedMigration.migrationAcceptedStepCount;
	metadataFixture.migrationResumedFromStep=certifiedMigration.migrationResumedFromStep;
	metadataFixture.maximumActiveSetComplementarityDiscrepancyMPerS=0.0025;
	metadataFixture.discontinuousActiveSetEvents=3u;
	metadataFixture.maximumActiveSetCycleLength=2u;
	metadataFixture.maximumActiveSetDifferingFaceCount=4u;
	metadataFixture.activeSetThreadIdentityChecked=true;
	metadataFixture.priorActiveSetAlgorithmVersion=LegacyActiveSetAlgorithmVersion();
	std::string fixtureRunMetadataId;
	const RISECBOR64::Bytes fixtureRunMetadata=RunMetadataEnvelope(metadataFixture,4u,
		DigestFile(streamedPrefixFrame),DigestFile(resumedCheckpointFrame),std::string(64u,'b'),
		fixtureRunMetadataId);
	RISECBOR64::Value decodedRunMetadata;
	std::string runMetadataError;
	const RISECBOR64::Value* runMetadataPayload=nullptr;
	RISECBOR64::Bytes runMetadataPayloadBytes;
	const bool runMetadataDecoded=RISECBOR64::DecodeCanonical(fixtureRunMetadata,
		decodedRunMetadata,&runMetadataError);
	if(runMetadataDecoded)runMetadataPayload=decodedRunMetadata.Find("payload");
	Check(runMetadataPayload&&RISECBOR64::Encode(*runMetadataPayload,
		runMetadataPayloadBytes,&runMetadataError)&&
		fixtureRunMetadataId==RISECBOR64::SHA256Hex(runMetadataPayloadBytes)&&
		!runMetadataPayload->Find("case_record_id")&&
		runMetadataPayload->Find("checkpoint_step_indices")&&
		runMetadataPayload->Find("reduction_mode")&&
		runMetadataPayload->Find("reduction_mode")->GetText()=="fixed_order_tree_v1"&&
		runMetadataPayload->Find("active_set_algorithm_version")&&
		runMetadataPayload->Find("active_set_algorithm_version")->GetText()==
			"open_active_set_two_class_r81_v2"&&
		runMetadataPayload->Find("active_set_prior_algorithm_version")&&
		runMetadataPayload->Find("active_set_prior_algorithm_version")->GetText()==
			"legacy_pre_r80_active_set"&&
		runMetadataPayload->Find("active_set_discontinuous_event_count")&&
		runMetadataPayload->Find("active_set_discontinuous_event_count")->
			GetIntegerArgument()==3u&&
		runMetadataPayload->Find("active_set_maximum_cycle_length")&&
		runMetadataPayload->Find("active_set_maximum_cycle_length")->
			GetIntegerArgument()==2u&&
		runMetadataPayload->Find("active_set_maximum_differing_face_count")&&
		runMetadataPayload->Find("active_set_maximum_differing_face_count")->
			GetIntegerArgument()==4u&&
		runMetadataPayload->Find("worker_count_history")&&
		runMetadataPayload->Find("worker_count_history")->GetArray().size()==2u&&
		runMetadataPayload->Find("build_migration")&&
		runMetadataPayload->Find("build_migration")->Find("certificate_id")&&
		runMetadataPayload->Find("build_migration")->Find("certificate_id")->GetText()==
			migrationCertificate.certificateId&&
		runMetadataPayload->Find("streamed_frame_count")&&
		runMetadataPayload->Find("streamed_frame_count")->GetIntegerArgument()==2u,
		"r61/r78 run and certified build-migration events are canonical companion metadata and do not enter case identity");
	const std::filesystem::path frame4 = root/"frame4.vdb";
	const std::filesystem::path frame5 = root/"frame5.vdb";
	WriteFrame(frame4,FrameMutation{},1.0f);
	WriteFrame(frame5,FrameMutation{},2.0f);

	std::string error;
	FireSequenceManifest manifest;
	RISECBOR64::Bytes envelope = ManifestBytes(DigestFile(frame4),DigestFile(frame5));
	Check(manifest.LoadCanonicalEnvelope(envelope,root.string(),error),
		"canonical sequence envelope loads");
	Check(manifest.SequenceId().size() == 64u && manifest.SourceKind() == "rise_simulation" &&
		manifest.PhysicalMapping() == "absolute_si" && manifest.Frames().size() == 2u,
		"sequence identity and producer qualification survive canonical decode");
	RISECBOR64::Value decodedEnvelope;
	Check(RISECBOR64::DecodeCanonical(envelope,decodedEnvelope,&error),"baseline envelope decodes");
	RISECBOR64::Bytes wrongIdentity;
	Check(RISECBOR64::Encode(ReplaceMember(decodedEnvelope,"sequence_id",
		RISECBOR64::Value::String(std::string(64u,'0'))),wrongIdentity,&error) &&
		!FireSequenceManifest().LoadCanonicalEnvelope(wrongIdentity,root.string(),error),
		"sequence_id must hash the exact canonical payload preimage");
	const RISECBOR64::Value* decodedPayload = decodedEnvelope.Find("payload");
	Check(decodedPayload && !FireSequenceManifest().LoadCanonicalEnvelope(
		EnvelopeForPayload(ReplaceMember(*decodedPayload,"frame_count",
			RISECBOR64::Value::Unsigned(3))),root.string(),error),
		"self-consistently rehashed inconsistent frame count rejects semantically");
	Check(decodedPayload && !FireSequenceManifest().LoadCanonicalEnvelope(
		EnvelopeForPayload(ReplaceMember(*decodedPayload,"producer_build_id",
			RISECBOR64::Value::String(std::string(64u,'0')))),root.string(),error),
		"self-consistently rehashed producer-build identity mutation rejects");
	Check(decodedPayload && !FireSequenceManifest().LoadCanonicalEnvelope(
		EnvelopeForPayload(ReplaceMember(*decodedPayload,"frame_encoding",
			RISECBOR64::Value::String("raw-dense-v1"))),root.string(),error),
		"frame encoding is exact OpenVDB v1");
	Check(decodedPayload && !FireSequenceManifest().LoadCanonicalEnvelope(
		EnvelopeForPayload(ReplaceMember(*decodedPayload,"physical_mapping",
			RISECBOR64::Value::String("heuristic:bogus"))),root.string(),error),
		"unknown normalized-to-physical mapping profiles reject");
	if( decodedPayload ) {
		const RISECBOR64::Bytes incompleteBuild=CanonicalRecord("producer_build_v1");
		RISECBOR64::Value badBuild=ReplaceMember(*decodedPayload,"producer_build_v1",
			RISECBOR64::Value::BytesValue(incompleteBuild));
		badBuild=ReplaceMember(badBuild,"producer_build_id",
			RISECBOR64::Value::String(RISECBOR64::SHA256Hex(incompleteBuild)));
		Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(badBuild),
			root.string(),error),"hash-valid incomplete producer build identity rejects");
		const RISECBOR64::Value* channelValue=decodedPayload->Find("channels");
		if( channelValue ) {
			auto channels=channelValue->GetArray();
			channels[0]=ReplaceMember(channels[0],"temporal_semantics",
				RISECBOR64::Value::String("derived_eulerian_source"));
			Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(
				ReplaceMember(*decodedPayload,"channels",RISECBOR64::Value::ArrayValue(channels))),
				root.string(),error),"material/source temporal roles cannot be exchanged");
		}
		Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(
			ReplaceMember(*decodedPayload,"velocity_halo_width_m",
				RISECBOR64::Value::Float(std::numeric_limits<double>::max()))),root.string(),error),
			"finite halo widths whose derived cell count overflows reject before conversion");
		const RISECBOR64::Value* timeMap=decodedPayload->Find("time_map");
		if( timeMap ) {
			const RISECBOR64::Value hugeTimeMap=ReplaceMember(*timeMap,"i0",
				RISECBOR64::Value::Signed(std::numeric_limits<std::int64_t>::max()));
			Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(
				ReplaceMember(*decodedPayload,"time_map",hugeTimeMap)),root.string(),error),
				"frame ranges that would overflow signed index arithmetic reject first");
		}
	}
	if( decodedPayload ) {
		const RISECBOR64::Bytes arbitrary=CanonicalRecord("arbitrary_record_v1");
		RISECBOR64::Value badAerosol=ReplaceMember(*decodedPayload,
			"aerosol_thermochemistry_record",RISECBOR64::Value::BytesValue(arbitrary));
		badAerosol=ReplaceMember(badAerosol,"aerosol_thermochemistry_record_id",
			RISECBOR64::Value::String(RISECBOR64::SHA256Hex(arbitrary)));
		Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(badAerosol),
			root.string(),error),"hash-valid arbitrary aerosol semantics reject");
		RISECBOR64::Value badChem=ReplaceMember(*decodedPayload,"chem_record",
			RISECBOR64::Value::BytesValue(arbitrary));
		badChem=ReplaceMember(badChem,"chem_record_id",
			RISECBOR64::Value::String(RISECBOR64::SHA256Hex(arbitrary)));
		Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(badChem),
			root.string(),error),"hash-valid arbitrary chemistry semantics reject");
		RISECBOR64::Value aerosolRecord;
		const RISECBOR64::Value* aerosolBytes=decodedPayload->Find(
			"aerosol_thermochemistry_record");
		if( aerosolBytes && RISECBOR64::DecodeCanonical(aerosolBytes->GetBytes(),
			aerosolRecord,&error) ) {
			RISECBOR64::Value carbon=*aerosolRecord.Find("carbon_phase");
			carbon=ReplaceMember(carbon,"source_fuel_record_id",
				RISECBOR64::Value::String(std::string(64u,'0')));
			aerosolRecord=ReplaceMember(aerosolRecord,"carbon_phase",carbon);
			RISECBOR64::Bytes badAerosolBytes;
			Check(RISECBOR64::Encode(aerosolRecord,badAerosolBytes,&error),
				"aerosol reference mutation encodes");
			RISECBOR64::Value badReference=ReplaceMember(*decodedPayload,
				"aerosol_thermochemistry_record",RISECBOR64::Value::BytesValue(badAerosolBytes));
			badReference=ReplaceMember(badReference,"aerosol_thermochemistry_record_id",
				RISECBOR64::Value::String(RISECBOR64::SHA256Hex(badAerosolBytes)));
			Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(badReference),
				root.string(),error),"aerosol carbon thermochemistry binds the methane fuel record");
		}
	}
	if( decodedPayload ) {
		const RISECBOR64::Value* channelValue=decodedPayload->Find("channels");
		for( unsigned int channelIndex=0; channelValue && channelIndex<4u; ++channelIndex ) {
			auto channels=channelValue->GetArray();
			auto background=channels[channelIndex].Find("background_value")->GetArray();
			background[0]=RISECBOR64::Value::Float(channelIndex==1u ? 301.0 : 1.0);
			channels[channelIndex]=ReplaceMember(channels[channelIndex],"background_value",
				RISECBOR64::Value::ArrayValue(background));
			Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(
				ReplaceMember(*decodedPayload,"channels",RISECBOR64::Value::ArrayValue(channels))),
				root.string(),error),"mandatory inactive-channel backgrounds reject mutation");
		}
	}

	FireSequenceMappedTime mapped;
	Check(manifest.MapSceneTime(10.0,mapped,error) && mapped.baseFrameIndex == 4 &&
		mapped.simulationTime == 1.0 && mapped.advectionOffsetSeconds == 0.0,
		"time map applies t0/alpha/t_scene_0 exactly");
	Check(manifest.MapSceneTime(10.25,mapped,error) && mapped.baseFrameIndex == 5 &&
		mapped.advectionOffsetSeconds == 0.25,
		"hold endpoint selects the last frame with one full-frame advection offset");
	Check(manifest.MapSceneTime(20.0,mapped,error) && mapped.baseFrameIndex == 5 && mapped.held,
		"hold policy clamps beyond authored support");
	FireSequenceManifest errorManifest;
	Check(errorManifest.LoadCanonicalEnvelope(
		ManifestBytes(DigestFile(frame4),DigestFile(frame5),"error"),root.string(),error) &&
		!errorManifest.MapSceneTime(10.25,mapped,error),
		"error policy rejects the half-open authored endpoint");

	FireSequencePreparedFrame loaded;
	Check(manifest.LoadFrame(4,loaded,error) && loaded.channels.size() == 4u &&
		loaded.channels.at("carbon").maximum == 1.0 &&
		loaded.channels.at("temperature").minimum == 300.0,
		"frame digest, topology, backgrounds, active values, and extrema preflight");

	const FrameMutation::Kind badKinds[] = {
		FrameMutation::NegativeActiveCarbon, FrameMutation::HotInactiveTemperature,
		FrameMutation::NegativeInactiveCarbon, FrameMutation::NonfiniteInactiveCarbon,
		FrameMutation::NonfiniteVelocity, FrameMutation::PositiveInfinityReaction,
		FrameMutation::NegativeInfinityReaction, FrameMutation::ZeroTemperature,
		FrameMutation::OutOfDomainTemperature, FrameMutation::NegativeActiveTile,
		FrameMutation::HotInactiveTile, FrameMutation::NaNInactiveTile };
	for( const FrameMutation::Kind kind : badKinds ) {
		WriteFrame(frame4,FrameMutation{kind},1.0f);
		FireSequenceManifest bad;
		Check(bad.LoadCanonicalEnvelope(ManifestBytes(DigestFile(frame4),DigestFile(frame5)),
			root.string(),error) && !bad.LoadFrame(4,loaded,error),
			"active and stored value-off defects reject before fidelity/derived structures");
	}
	WriteFrame(frame4,FrameMutation{},1.0f,true);
	WriteFrame(frame5,FrameMutation{},2.0f,true);
	FireSequenceManifest chemManifest;
	Check(chemManifest.LoadCanonicalEnvelope(ManifestBytes(DigestFile(frame4),DigestFile(frame5),
		"hold",false,true),root.string(),error) && chemManifest.HasChemChannels() &&
		chemManifest.PreflightAllFrames(error),
		"preview-only synthetic chem record enables and preflights the complete triplet");
	RISECBOR64::Value chemDecoded;
	const RISECBOR64::Bytes chemEnvelopeForLattice=ManifestBytes(DigestFile(frame4),
		DigestFile(frame5),"hold",false,true);
	Check(RISECBOR64::DecodeCanonical(chemEnvelopeForLattice,chemDecoded,&error),
		"chem envelope decodes for lattice RED");
	const RISECBOR64::Value* chemPayload=chemDecoded.Find("payload");
	const RISECBOR64::Value* chemChannels=chemPayload ? chemPayload->Find("channels") : nullptr;
	if( chemPayload && chemChannels ) {
		auto shifted=chemChannels->GetArray();
		for( auto& channel : shifted ) if( channel.Find("name") &&
			channel.Find("name")->GetText()=="chem_CH" ) {
			channel=ReplaceMember(channel,"origin_m",RISECBOR64::Value::ArrayValue({
				RISECBOR64::Value::Float(0.5),RISECBOR64::Value::Float(0.0),
				RISECBOR64::Value::Float(0.0)}));
			channel=ReplaceMember(channel,"core_face_bounds_m",RISECBOR64::Value::ArrayValue({
				RISECBOR64::Value::Float(0.25),RISECBOR64::Value::Float(-0.25),
				RISECBOR64::Value::Float(-0.25),RISECBOR64::Value::Float(1.25),
				RISECBOR64::Value::Float(0.75),RISECBOR64::Value::Float(0.75)}));
		}
		Check(!FireSequenceManifest().LoadCanonicalEnvelope(EnvelopeForPayload(
			ReplaceMember(*chemPayload,"channels",RISECBOR64::Value::ArrayValue(shifted))),
			root.string(),error),
			"chem lattices cannot be shifted while the renderer maps them onto carbon space");
	}
	FireSequencePreparedFrame chemFrame;
	Check(chemManifest.LoadFrame(4,chemFrame,error) && chemFrame.channels.size()==7u &&
		chemFrame.channels.at("chem_CH").maximum==120.0 &&
		chemFrame.channels.at("chem_C2").maximum==50.0 &&
		chemFrame.channels.at("chem_CO2").maximum==8.0,
		"chem triplet survives digest-bound OpenVDB decoding with absolute W/m3 values");
	HenyeyGreensteinPhaseFunction* chemPhase=new HenyeyGreensteinPhaseFunction(0.0);
	MultichannelHeterogeneousMedium* chemMedium=new MultichannelHeterogeneousMedium(
		chemFrame,chemManifest.ChemNormalizationIntervalsNM(),2,2,2,
		Point3(-0.25,-0.25,-0.25),Point3(0.75,0.75,0.75),1.0,
		FireOpticsPreset::PredictiveV1(),*chemPhase);
	chemPhase->release();
	Check(chemMedium->IsValid() && chemMedium->GetChemEmissionNM(Point3(0,0,0),420.0)>0.0,
		"sequence-backed medium installs the chem accessors and normalized fixture SPD");
	chemMedium->release();
	for( const FrameMutation::Kind kind : {FrameMutation::NegativeActiveChem,
		FrameMutation::NaNInactiveChem} ) {
		WriteFrame(frame4,FrameMutation{kind},1.0f,true);
		FireSequenceManifest badChemFrame;
		Check(badChemFrame.LoadCanonicalEnvelope(ManifestBytes(DigestFile(frame4),
			DigestFile(frame5),"hold",false,true),root.string(),error) &&
			!badChemFrame.LoadFrame(4,loaded,error),
			"negative and stored value-off nonfinite chem payloads reject before fidelity");
	}
	WriteFrame(frame4,FrameMutation{},1.0f,true);
	WriteFrame(frame5,FrameMutation{},2.0f,true);
	const std::filesystem::path chemManifestPath=root/"sequence_chem_fixture.rise-fire.cbor";
	const RISECBOR64::Bytes chemEnvelope=ManifestBytes(DigestFile(frame4),DigestFile(frame5),
		"hold",false,true);
	{
		std::ofstream output(chemManifestPath,std::ios::binary);
		output.write(reinterpret_cast<const char*>(chemEnvelope.data()),
			static_cast<std::streamsize>(chemEnvelope.size()));
	}
	IJob* chemJob=nullptr;
	Check(RISE_CreateJob(&chemJob) && chemJob && chemJob->AddFireMediumBound(
		"chem_sequence",chemManifestPath.string().c_str(),"carbon","temperature","",
		"reaction","chem_CH","chem_C2","chem_CO2","velocity",false),
		"Job consumes the preview-only non-none chem record through the complete binding path");
	const MultichannelHeterogeneousMedium* jobChemMedium=chemJob ?
		dynamic_cast<const MultichannelHeterogeneousMedium*>(chemJob->GetMedium("chem_sequence")) : nullptr;
	Check(jobChemMedium && jobChemMedium->IsValid() &&
		jobChemMedium->GetChemEmissionNM(Point3(0,0,0),420.0)>0.0,
		"Job-installed sequence medium retains nonzero absolute chemistry emission");
	if( chemJob ) chemJob->release();
	WriteFrame(frame4,FrameMutation{},1.0f);
	WriteFrame(frame5,FrameMutation{FrameMutation::NegativeActiveCarbon},2.0f);
	FireSequenceManifest laterMalformed;
	Check(laterMalformed.LoadCanonicalEnvelope(ManifestBytes(DigestFile(frame4),DigestFile(frame5)),
		root.string(),error) && !laterMalformed.PreflightAllFrames(error),
		"loadability preflight scans later frames before any frame is activated");
	WriteFrame(frame5,FrameMutation{},2.0f);
	manifest = FireSequenceManifest();
	Check(manifest.LoadCanonicalEnvelope(ManifestBytes(DigestFile(frame4),DigestFile(frame5)),
		root.string(),error),"restored valid manifest loads");
	std::fstream truncate(frame4,std::ios::binary|std::ios::in|std::ios::out);
	truncate.seekp(0,std::ios::end);
	const std::streampos originalSize = truncate.tellp();
	truncate.close();
	std::filesystem::resize_file(frame4,static_cast<std::uintmax_t>(originalSize)-1u);
	Check(!manifest.LoadFrame(4,loaded,error),"truncated frame is rejected by whole-file digest");
	WriteFrame(frame4,FrameMutation{},1.0f);
	manifest = FireSequenceManifest();
	Check(manifest.LoadCanonicalEnvelope(ManifestBytes(DigestFile(frame4),DigestFile(frame5)),
		root.string(),error),"controller fixture manifest loads");

	FireSequencePreparationController controller(manifest);
	unsigned int installedFrames = 0u;
	const std::string preparedComponentIdentity(64u,'b');
	Check(controller.SetFrameInstaller(
		[&installedFrames](const FireSequencePreparedFrame& candidate) {
			++installedFrames;
			return candidate.channels.at("temperature").minimum > 0.0;
		},preparedComponentIdentity,error) && controller.SetActiveBindingIdentity(
			std::string(64u,'c'),error),"prepared owner binds one transactional frame installer");
	FireSequenceRenderTimeSupport support{10.0,10.0,10.0};
	Check(controller.PrepareMediaForRender(support,false,error) &&
		controller.Generation() == 1u && controller.MajorantGeneration() == 1u &&
		controller.EmissionCDFGeneration() == 1u,
		"frame advance transaction rebuilds majorant and emission CDF once");
	Check(installedFrames == 1u,"first prepared generation performs one real install callback");
	const std::string firstPreparedId = controller.PreparedInputId();
	Check(controller.PrepareMediaForRender(support,false,error) &&
		controller.Generation() == 1u && controller.PreparedInputId() == firstPreparedId,
		"identical complete prepared-input identity reuses immutable state");
	Check(installedFrames == 1u,"identical prepared input does not rebuild derived structures");
	FireSequencePreparationController::RenderLease lease = controller.AcquireRenderLease(error);
	Check(lease.IsValid() && lease.StateStayedFrozen(),"render lease captures immutable prepared state");
	FireSequenceRenderTimeSupport next{10.125,10.125,10.125};
	Check(!controller.PrepareMediaForRender(next,false,error) && error == "mutation_frozen" &&
		lease.StateStayedFrozen(),"mid-render frame/majorant/CDF swap fails immediately");
	lease = FireSequencePreparationController::RenderLease();
	Check(controller.PrepareMediaForRender(next,false,error) && controller.Generation() == 2u &&
		controller.PreparedInputId() != firstPreparedId,
		"between-render frame advance atomically publishes a new prepared generation");
	Check(installedFrames == 2u,"frame advance performs exactly one new install callback");
	const std::string secondPreparedId=controller.PreparedInputId();
	Check(controller.SetActiveBindingIdentity(std::string(64u,'e'),error) &&
		controller.PrepareMediaForRender(next,false,error) && controller.Generation()==3u &&
		controller.PreparedInputId()!=secondPreparedId && installedFrames==3u,
		"identical-time binding mutation changes the complete prepared identity and rebuilds");

	FireSequencePreparedFrame physicalInitial;
	Check(manifest.LoadFrame(4,physicalInitial,error),"physical medium fixture reloads frame 4");
	HenyeyGreensteinPhaseFunction* phase = new HenyeyGreensteinPhaseFunction(0.0);
	MultichannelHeterogeneousMedium* medium = new MultichannelHeterogeneousMedium(
		physicalInitial,manifest.ChemNormalizationIntervalsNM(),2,2,2,
		Point3(-0.25,-0.25,-0.25),Point3(0.75,0.75,0.75),
		1.0,FireOpticsPreset::PredictiveV1(),*phase);
	phase->release();
	Check(medium->IsValid(),"manifest frame constructs the production fire medium");
	FireSequencePreparationController physicalController(manifest);
	Check(medium->BindSequencePreparationController(physicalController,
		preparedComponentIdentity,error) && physicalController.SetActiveBindingIdentity(
			std::string(64u,'d'),error),
		"physical medium is the prepared owner's sole frame installer");
	const unsigned long long initialMajorant = medium->ForTest_FireMajorantGeneration();
	const unsigned long long initialEmission = medium->ForTest_FireEmissionGeneration();
	Check(physicalController.PrepareMediaForRender(support,false,error) &&
		medium->FireDerivedStructuresCurrent() &&
		medium->ForTest_FireMajorantGeneration() == initialMajorant+1u &&
		medium->ForTest_FireEmissionGeneration() == initialEmission+1u,
		"prepared frame install performs the medium's real majorant and emission-CDF rebuild");
	const unsigned long long firstPhysicalMajorant = medium->ForTest_FireMajorantGeneration();
	const unsigned long long firstPhysicalEmission = medium->ForTest_FireEmissionGeneration();
	FireSequencePreparationController::RenderLease physicalLease =
		physicalController.AcquireRenderLease(error);
	Check(!physicalController.PrepareMediaForRender(next,false,error) &&
		medium->ForTest_FireMajorantGeneration() == firstPhysicalMajorant &&
		medium->ForTest_FireEmissionGeneration() == firstPhysicalEmission,
		"mid-render mutation cannot reach grid, majorant, or emission CDF");
	physicalLease = FireSequencePreparationController::RenderLease();
	Check(physicalController.PrepareMediaForRender(next,false,error) &&
		medium->ForTest_FireMajorantGeneration() == firstPhysicalMajorant+1u &&
		medium->ForTest_FireEmissionGeneration() == firstPhysicalEmission+1u,
		"between-render frame advance rebuilds both real derived structures once");
	medium->release();

	const bool capstoneArtifactRun=std::getenv("RISE_FIRE_CAPSTONE_OUTPUT")!=nullptr;
	const std::filesystem::path capstoneOutputDirectory=capstoneArtifactRun?
		std::filesystem::path(std::getenv("RISE_FIRE_CAPSTONE_OUTPUT")):std::filesystem::path();
	const bool capstoneValidationOnly=capstoneArtifactRun&&
		std::getenv("RISE_FIRE_CAPSTONE_VALIDATE_ONLY")!=nullptr;
	const unsigned int capstoneWorkerCount=std::max(4u,std::thread::hardware_concurrency());
	const double runDiameterM=capstoneArtifactRun?CapstonePoolDiameterM:0.03;
	const double runHeatReleaseRateKW=capstoneArtifactRun?CapstoneHeatReleaseRateKW:0.40;
	const SolverFrameValues injectedFailure=RunMethaneFrameProbe(1u,1u,0.0,1.0,4.0,
		6.0,0.03,0.40,true);
	Check(!injectedFailure.succeeded&&injectedFailure.temperature.empty()&&
		injectedFailure.structuredError.find("solver_failure:injected_solver_failure")==0,
		"solver failure aborts the run pipeline with a structured error before any frame state exists");
	const SolverFrameValues tier6PipelinePreview=RunMethaneFrameProbe(1u,0u,0.0,1.0,4.0,6.0,
		runDiameterM,runHeatReleaseRateKW);
	if(!tier6PipelinePreview.succeeded){std::fprintf(stderr,"capstone fail-fast: %s\n",
		tier6PipelinePreview.structuredError.c_str());return 1;}
	double reportedResolutionTier=capstoneArtifactRun?10.0:6.0;
	if(const char* tier=std::getenv("RISE_FIRE_CAPSTONE_TIER"))
		reportedResolutionTier=std::strtod(tier,nullptr);
	const SolverFrameValues capstoneCasePreview=capstoneArtifactRun?
		RunMethaneFrameProbe(1u,0u,0.0,1.0,4.0,reportedResolutionTier,
			runDiameterM,runHeatReleaseRateKW):tier6PipelinePreview;
	if(!capstoneCasePreview.succeeded){std::fprintf(stderr,"capstone fail-fast: %s\n",
		capstoneCasePreview.structuredError.c_str());return 1;}
	const double expectedPuffingHz=1.5/std::sqrt(CapstonePoolDiameterM);
	// Forty reference periods leave at least thirty observed periods even at
	// the allowed -20% frequency edge and after the first post-window step.
	double capstoneTargetS=5.0*capstoneCasePreview.flowThroughTimeS+40.0/expectedPuffingHz;
	if(capstoneValidationOnly)
		capstoneTargetS=1.1*capstoneCasePreview.flowThroughTimeS;
	if(const char* target=std::getenv("RISE_FIRE_CAPSTONE_TARGET_S"))
		capstoneTargetS=std::strtod(target,nullptr);
	const double caseDurationS=capstoneArtifactRun?std::max(1.0,capstoneTargetS):1.0;
	const double caseFramesPerS=capstoneArtifactRun?1.0/capstoneTargetS:4.0;
	const SolverFrameValues methaneFrame=RunMethaneFrameProbe(1u,0u,0.0,
		caseDurationS,caseFramesPerS,reportedResolutionTier,runDiameterM,runHeatReleaseRateKW);
	if(!methaneFrame.succeeded){std::fprintf(stderr,"capstone fail-fast: %s\n",
		methaneFrame.structuredError.c_str());return 1;}
	if(!WriteFrame(frame4,FrameMutation{},0.0f,true,methaneFrame)){
		std::fprintf(stderr,"capstone fail-fast: initial frame serialization failed\n");return 1;
	}
	std::string durableArtifactError;
	if(capstoneArtifactRun&&!DurableCopyPublishedFile(frame4,
		capstoneOutputDirectory/"frame4.vdb",durableArtifactError)){
		std::fprintf(stderr,"capstone frame streaming failed: %s\n",durableArtifactError.c_str());
		return 1;
	}
	const unsigned int determinismStepCount=capstoneArtifactRun&&!capstoneValidationOnly?1u:0u;
	const SolverFrameValues deterministicOne=RunMethaneFrameProbe(1u,determinismStepCount,0.0,
		caseDurationS,caseFramesPerS,6.0,runDiameterM,runHeatReleaseRateKW);
	if(!deterministicOne.succeeded){std::fprintf(stderr,"capstone fail-fast: %s\n",
		deterministicOne.structuredError.c_str());return 1;}
	const SolverFrameValues methaneFrameParallel=RunMethaneFrameProbe(capstoneWorkerCount,
		determinismStepCount,0.0,
		caseDurationS,caseFramesPerS,6.0,runDiameterM,runHeatReleaseRateKW);
	if(!methaneFrameParallel.succeeded){std::fprintf(stderr,"capstone fail-fast: %s\n",
		methaneFrameParallel.structuredError.c_str());return 1;}
	const std::filesystem::path deterministicOneFrame=root/"frame_deterministic_one.vdb";
	const std::filesystem::path deterministicParallelFrame=root/"frame_deterministic_parallel.vdb";
	if(!WriteFrame(deterministicOneFrame,FrameMutation{},0.0f,true,deterministicOne)||
		!WriteFrame(deterministicParallelFrame,FrameMutation{},0.0f,true,methaneFrameParallel)){
		std::fprintf(stderr,"capstone fail-fast: deterministic frame serialization failed\n");return 1;
	}
	const std::string singleWorkerDigest=DigestFile(deterministicOneFrame);
	const std::string parallelWorkerDigest=DigestFile(deterministicParallelFrame);
	Check(!singleWorkerDigest.empty()&&singleWorkerDigest==parallelWorkerDigest,
		"r57 same methane case at one and N workers produces identical frame bytes");
	if(capstoneArtifactRun&&!DurableCopyPublishedFile(deterministicOneFrame,
		capstoneOutputDirectory/"frame_single_worker.vdb",durableArtifactError)){
		std::fprintf(stderr,"capstone frame streaming failed: %s\n",durableArtifactError.c_str());
		return 1;
	}
	RunPersistenceOptions capstonePersistence;
	if(capstoneArtifactRun){
		capstonePersistence.checkpointPath=capstoneOutputDirectory/"tier10.run.checkpoint";
		capstonePersistence.checkpointCadenceWallS=900.0;
		capstonePersistence.streamedFrameCountAtStart=1u;
		capstonePersistence.resume=true;
		if(const char* migrationCertificate=
			std::getenv("RISE_FIRE_CAPSTONE_MIGRATION_CERTIFICATE")){
			if(!*migrationCertificate){std::fprintf(stderr,
				"RISE_FIRE_CAPSTONE_MIGRATION_CERTIFICATE is empty\n");return 1;}
			capstonePersistence.resumeEquivalenceCertificatePath=migrationCertificate;
		}
	}
	SolverFrameValues methaneFrameNext=capstoneArtifactRun?
		RunMethaneFrameProbe(capstoneWorkerCount,1u,capstoneTargetS,caseDurationS,caseFramesPerS,
			reportedResolutionTier,runDiameterM,runHeatReleaseRateKW,false,capstonePersistence):
		methaneFrameParallel;
	if(!methaneFrameNext.succeeded){std::fprintf(stderr,"capstone fail-fast: %s\n",
		methaneFrameNext.structuredError.c_str());return 1;}
	if(!WriteFrame(frame5,FrameMutation{},0.0f,true,methaneFrameNext,0.8f)){
		std::fprintf(stderr,"capstone fail-fast: final frame serialization failed\n");return 1;
	}
	methaneFrameNext.streamedFrameCount=2u;
	if(capstoneArtifactRun&&!DurableCopyPublishedFile(frame5,
		capstoneOutputDirectory/"frame5.vdb",durableArtifactError)){
		std::fprintf(stderr,"capstone frame streaming failed: %s\n",durableArtifactError.c_str());
		return 1;
	}
	const bool capstoneIgnition=!capstoneArtifactRun||(methaneFrameNext.temperatureK>800.0f&&
		methaneFrameNext.reactionWPerM3>0.0f&&
		methaneFrameNext.ignitedDuringPilot&&methaneFrameNext.sustainedAfterPilot&&
		methaneFrameNext.pilotHoldBandObserved&&methaneFrameNext.pilotHoldBandSatisfied&&
		std::fabs(methaneFrameNext.pilotEnergyJ-methaneFrameNext.expectedPilotEnergyJ)<=
			2.0e-12*std::max(1.0,methaneFrameNext.expectedPilotEnergyJ));
	if(!capstoneIgnition) std::printf("capstone ignition diagnostic T=%.9g Tmax=%.9g reaction=%.9g inside=%d sustained=%d hold_seen=%d hold_ok=%d eos_max=%.9g approach_eos_max=%.9g pilot=%.17g expected=%.17g time=%.17g tft=%.17g\n",
		methaneFrameNext.temperatureK,methaneFrameNext.maximumTemperatureK,
		methaneFrameNext.reactionWPerM3,
		methaneFrameNext.ignitedDuringPilot?1:0,methaneFrameNext.sustainedAfterPilot?1:0,
		methaneFrameNext.pilotHoldBandObserved?1:0,
		methaneFrameNext.pilotHoldBandSatisfied?1:0,
		methaneFrameNext.maximumAcceptedEOSResidual,
		methaneFrameNext.maximumPilotApproachEOSResidual,
		methaneFrameNext.pilotEnergyJ,methaneFrameNext.expectedPilotEnergyJ,
		methaneFrameNext.simulatedTimeS,methaneFrameNext.flowThroughTimeS);
	Check(capstoneIgnition,
		"capstone cold domain ignites with the r58 thermostat, sustains after shutoff, and ledgers exact pilot energy");
	Check(!capstoneArtifactRun||methaneFrameNext.maximumTemperatureK<
		FireSimulationGasOpacityRecord::HITEMPPlanckMeanV1().TemperatureMaxK(),
		"capstone thermostat keeps every tier inside the unchanged certified opacity domain");
	Check(!capstoneArtifactRun||
		methaneFrameNext.maximumTemperatureK<2300.0,
		"every capstone tier stays below the r74 case-derived methane physicality bound");
	Check(!capstoneArtifactRun||capstoneValidationOnly||reportedResolutionTier<10.0||(
		methaneFrameNext.discontinuousLimiterClassSteps>0u&&
		methaneFrameNext.discontinuousClassThreadIdentityChecked&&
		methaneFrameNext.discontinuousClassThreadIdentity),
		"r59 tier-10 owner enters the discontinuous class and produces bit-identical 1-vs-N accepted bytes at that step");
	Check(!capstoneArtifactRun||capstoneValidationOnly||reportedResolutionTier<10.0||
		methaneFrameNext.discontinuousActiveSetEvents==0u||(
			methaneFrameNext.activeSetThreadIdentityChecked&&
			methaneFrameNext.activeSetThreadIdentity),
		"r81 every discontinuous active-set event is fail-closed on a 1-vs-N byte mismatch");
	Check(!capstoneArtifactRun||capstoneValidationOnly||(
		methaneFrameNext.statisticsBoundaryObserved&&
		methaneFrameNext.firstStatisticsStepStartS==methaneFrameNext.statisticsStartS),
		"capstone timestep event-splits exactly at 5*t_ft before accumulating empirical rows");
	const double integratedFuelEnergy=methaneFrameNext.integratedFuelConsumptionKG*
		FireSimulationMethaneRecord::PhysicalV1().LowerHeatingValueJPerKG();
	Check(!capstoneArtifactRun||capstoneValidationOnly||(methaneFrameNext.probeTimeS.size()>=64u&&
		methaneFrameNext.simulatedTimeS>=capstoneTargetS&&
		std::isfinite(methaneFrameNext.puffingFrequencyHz)&&
		methaneFrameNext.puffingFrequencyHz>0.0&&
		methaneFrameNext.puffingRelativeError<=0.20&&
		(methaneFrameNext.probeTimeS.back()-methaneFrameNext.probeTimeS.front())*
			methaneFrameNext.puffingFrequencyHz>=30.0),
		"capstone exports thirty measured puffing periods and matches 1.5/sqrt(D) within 20 percent");
	Check(!capstoneArtifactRun||capstoneValidationOnly||(methaneFrameNext.integratedHeatReleaseJ>0.0&&
		std::fabs(methaneFrameNext.integratedHeatReleaseJ-integratedFuelEnergy)<=
			2.0e-10*methaneFrameNext.integratedHeatReleaseJ),
		"capstone statistics-window HRR equals fuel consumption times record LHV");
	Check(!capstoneArtifactRun||capstoneValidationOnly||(
		std::fabs(methaneFrameNext.integratedRadiativeFraction-0.20)<=0.02&&
		methaneFrameNext.integratedRadiativeFraction>=0.07&&
		methaneFrameNext.integratedRadiativeFraction<=0.28),
		"capstone statistics-window radiative fraction matches the methane default within its recorded spread");
	Check(!capstoneArtifactRun||capstoneValidationOnly||(methaneFrameNext.centerlineHeightM.size()==
		methaneFrameNext.dimensions[2]&&std::isfinite(methaneFrameNext.centerlineTemperatureExponent)&&
		methaneFrameNext.mccaffreyPlumeStationCount>=4u&&
		methaneFrameNext.mccaffreyMaximumTemperatureRelativeError<=0.10&&
		methaneFrameNext.mccaffreyMaximumVelocityRelativeError<=0.10),
		"capstone time-averaged above-tip centerline T and velocity match McCaffrey NBSIR 79-1910 within 10 percent");
	bool stationArchiveComplete=true;
	const std::size_t expectedStationRows=methaneFrameNext.probeTimeS.size()*
		methaneFrameNext.dimensions[2];
	stationArchiveComplete=expectedStationRows==methaneFrameNext.stationProbeTimeS.size()&&
		methaneFrameNext.stationProbeHeightM.size()==expectedStationRows&&
		methaneFrameNext.stationProbeTemperatureK.size()==expectedStationRows&&
		methaneFrameNext.stationProbeReactionWPerM3.size()==expectedStationRows&&
		methaneFrameNext.stationProbeVerticalVelocityMPerS.size()==expectedStationRows;
	for(std::size_t sample=0;stationArchiveComplete&&sample<methaneFrameNext.probeTimeS.size();
		++sample) for(std::size_t station=0;station<methaneFrameNext.dimensions[2];++station) {
		const std::size_t row=sample*methaneFrameNext.dimensions[2]+station;
		stationArchiveComplete=stationArchiveComplete&&
			methaneFrameNext.stationProbeTimeS[row]==methaneFrameNext.probeTimeS[sample]&&
			methaneFrameNext.stationProbeHeightM[row]==
				(static_cast<double>(station)+0.5)*methaneFrameNext.cellWidthM;
	}
	Check(!capstoneArtifactRun||capstoneValidationOnly||stationArchiveComplete,
		"capstone archives timestamped T, reaction-rate, and velocity at every fixed centerline station");
	if(capstoneArtifactRun&&!capstoneValidationOnly) {
		const double puffingSpanS=methaneFrameNext.probeTimeS.size()>1u?
			methaneFrameNext.probeTimeS.back()-methaneFrameNext.probeTimeS.front():0.0;
		const bool puffingQualified=methaneFrameNext.probeTimeS.size()>=64u&&
			std::isfinite(methaneFrameNext.puffingFrequencyHz)&&
			methaneFrameNext.puffingFrequencyHz>0.0&&methaneFrameNext.puffingRelativeError<=0.20&&
			puffingSpanS*methaneFrameNext.puffingFrequencyHz>=30.0;
		const bool mccaffreyQualified=methaneFrameNext.mccaffreyPlumeStationCount>=4u&&
			methaneFrameNext.mccaffreyMaximumTemperatureRelativeError<=0.10&&
			methaneFrameNext.mccaffreyMaximumVelocityRelativeError<=0.10;
		if(!puffingQualified||!mccaffreyQualified) {
			std::fprintf(stderr,"capstone empirical qualification failed: puffing_Hz=%.17g "
				"puffing_error=%.17g observed_cycles=%.17g McCaffrey_T_error=%.17g "
				"McCaffrey_u_error=%.17g\n",methaneFrameNext.puffingFrequencyHz,
				methaneFrameNext.puffingRelativeError,
				puffingSpanS*methaneFrameNext.puffingFrequencyHz,
				methaneFrameNext.mccaffreyMaximumTemperatureRelativeError,
				methaneFrameNext.mccaffreyMaximumVelocityRelativeError);
			return 1;
		}
	}
	const std::filesystem::path manifestPath = root/"sequence.rise-fire.cbor";
	const RISECBOR64::Bytes productionEnvelope = ManifestBytes(
		DigestFile(frame4),DigestFile(frame5),"hold",true,true,
		{{static_cast<std::uint64_t>(methaneFrame.dimensions[0]),
		  static_cast<std::uint64_t>(methaneFrame.dimensions[1]),
		  static_cast<std::uint64_t>(methaneFrame.dimensions[2])}},methaneFrame.cellWidthM,
		0.0,capstoneArtifactRun?capstoneTargetS:0.25,1.0,10.0,true,
		runDiameterM,runHeatReleaseRateKW,false,0.0,
		reportedResolutionTier,true);
	FireSequenceManifest productionManifest;
	Check(productionManifest.LoadCanonicalEnvelope(productionEnvelope,root.string(),error),
		"production-shaped Job manifest validates independently");
	Check(productionManifest.CaseRecordId()==methaneFrame.caseRecordId,
		"solver lattice and sequence manifest bind the identical case_record_id");
	{
		std::ofstream output(manifestPath,std::ios::binary);
		output.write(reinterpret_cast<const char*>(productionEnvelope.data()),
			static_cast<std::streamsize>(productionEnvelope.size()));
	}
	{
		RISECBOR64::Value productionDecoded;
		Check(RISECBOR64::DecodeCanonical(productionEnvelope,productionDecoded,&error),
			"production envelope decodes for cross-record RED");
		const RISECBOR64::Value* payload=productionDecoded.Find("payload");
		const RISECBOR64::Bytes syntheticOptics=FireOpticsPreset::SyntheticRegressionV1().RecordBytes();
		if( payload ) {
			RISECBOR64::Value mismatch=ReplaceMember(*payload,"optical_record",
				RISECBOR64::Value::BytesValue(syntheticOptics));
			mismatch=ReplaceMember(mismatch,"optical_record_id",
				RISECBOR64::Value::String(RISECBOR64::SHA256Hex(syntheticOptics)));
			const std::filesystem::path mismatchPath=root/"sequence_mismatched_optics.rise-fire.cbor";
			const RISECBOR64::Bytes mismatchEnvelope=EnvelopeForPayload(mismatch);
			std::ofstream output(mismatchPath,std::ios::binary);
			output.write(reinterpret_cast<const char*>(mismatchEnvelope.data()),
				static_cast<std::streamsize>(mismatchEnvelope.size()));
			output.close();
			IJob* mismatchJob=nullptr;
			Check(RISE_CreateJob(&mismatchJob) && mismatchJob &&
				!mismatchJob->AddFireMedium("mismatch",mismatchPath.string().c_str()),
				"fuel soot-density reference rejects a different valid optics identity");
			if( mismatchJob ) mismatchJob->release();
		}
	}
	IJob* job = nullptr;
	Check(RISE_CreateJob(&job) && job,"sequence binding test creates a Job");
	Check(job && job->AddFireMediumBound("sequence_fire",manifestPath.string().c_str(),
		"carbon","temperature","","reaction","chem_CH","chem_C2","chem_CO2","velocity",false) &&
		job->SetGlobalMedium("sequence_fire"),
		"fire_medium creates a named manager entry and binds through global_medium");
	if( job ) job->release();
	const std::filesystem::path scenePath = root/"sequence_scene.RISEscene";
	const std::filesystem::path renderBase = root/"sequence_render";
#if defined(_WIN32)
	_putenv_s("RISE_MEDIA_PATH",(root.string()+"/").c_str());
#else
	setenv("RISE_MEDIA_PATH",(root.string()+"/").c_str(),1);
#endif
	{
		std::ofstream scene(scenePath);
		const double cameraCenterX=0.5*static_cast<double>(methaneFrame.dimensions[0])*
			methaneFrame.cellWidthM;
		const double cameraCenterY=0.5*static_cast<double>(methaneFrame.dimensions[1])*
			methaneFrame.cellWidthM;
		const double cameraDepth=std::max({static_cast<double>(methaneFrame.dimensions[0]),
			static_cast<double>(methaneFrame.dimensions[1]),
			static_cast<double>(methaneFrame.dimensions[2])})*methaneFrame.cellWidthM;
		scene << "RISE ASCII SCENE 7\n\n"
			<< "scene_options\n{\nscene_unit 1\nfidelity_mode preview\n}\n\n"
			<< "standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
			<< "pathtracing_spectral_rasterizer\n{\nsamples 1\nnmbegin 380\n"
			<< "nmend 780\nnum_wavelengths 1\nspectral_samples 1\nhwss false\n"
			<< "pixel_filter box\noidn_denoise false\n}\n\n"
			<< "file_rasterizeroutput\n{\npattern sequence_render"
			<< "\ntype EXR\nbpp 32\ncolor_space Rec709RGB_Linear"
			<< "\nexposure 0\ndisplay_transform none\nexr_compression piz\n}\n\n"
			<< "file_rasterizeroutput\n{\npattern sequence_display\ntype PNG\nbpp 16\n"
			<< "color_space sRGB\nexposure 20\ndisplay_transform aces\n}\n\n"
			<< "film\n{\nwidth 3\nheight 3\n}\n\n"
			<< "pinhole_camera\n{\nname camera\nlocation " << cameraCenterX << ' ' <<
			cameraCenterY << ' ' << -cameraDepth << "\n"
			<< "lookat " << cameraCenterX << ' ' << cameraCenterY << ' ' << 0.5*cameraDepth << "\n"
			<< "up 0 1 0\nfov 45\nexposure 0.04\nscanning_rate -0.1\n"
			<< "pixel_rate 0.02\n}\n\nfire_medium\n{\n"
			<< "name sequence_fire\nfidelity_mode preview\nsequence_manifest "
			<< manifestPath.string() << "\nchannel_carbon carbon\n"
			<< "channel_temperature temperature\nchannel_reaction reaction\n"
			<< "channel_chem_ch chem_CH\nchannel_chem_c2 chem_C2\n"
			<< "channel_chem_co2 chem_CO2\nchannel_velocity velocity\n}\n\n"
			<< "global_medium\n{\nmedium sequence_fire\n}\n";
	}
	IJobPriv* parsedJob = nullptr;
	Check(RISE_CreateJobPriv(&parsedJob) && parsedJob &&
		parsedJob->LoadAsciiSceneViaCst(scenePath.string().c_str()),
		"descriptor-driven fire_medium chunk binds the sequence through the existing global-medium seam");
	const RenderTimeSupport fullSupport = parsedJob && parsedJob->GetScene() ?
		ComputePathTimeSupport(*parsedJob->GetScene(),10.0,nullptr,
			PixelBasedRasterizerHelper::FIELD_BOTH) :
		RenderTimeSupport();
	const RenderTimeSupport oddFieldSupport = parsedJob && parsedJob->GetScene() ?
		ComputePathTimeSupport(*parsedJob->GetScene(),10.0,nullptr,
			PixelBasedRasterizerHelper::FIELD_LOWER) :
		RenderTimeSupport();
	Check(std::fabs(fullSupport.open-9.88)<1e-12 &&
		std::fabs(fullSupport.close-10.16)<1e-12 &&
		std::fabs(oddFieldSupport.open-9.98)<1e-12 &&
		std::fabs(oddFieldSupport.close-10.06)<1e-12,
		"prepared time support is sign-aware over exposure, scan, pixels, and field parity");
	Check(parsedJob && parsedJob->SetFilm(32,32,1.0),
		"capstone renders a small inspectable spectral frame after qualifying time support");
	const MultichannelHeterogeneousMedium* parsedMedium = parsedJob ?
		dynamic_cast<const MultichannelHeterogeneousMedium*>(parsedJob->GetMedium("sequence_fire")) : nullptr;
	const unsigned long long beforeRenderMajorant = parsedMedium ?
		parsedMedium->ForTest_FireMajorantGeneration() : 0u;
	const double firstProductionRenderTime=capstoneArtifactRun?
		10.0+capstoneTargetS+0.2:10.0;
	Check(parsedJob && parsedJob->SetAnimationOptions(firstProductionRenderTime,
		firstProductionRenderTime,1,false,false) &&
		parsedJob->Rasterize(),
		"sequence-backed Job render enters the prepared rasterizer seam and completes");
	const IRasterizer* parsedRasterizer = parsedJob ? parsedJob->GetRasterizer() : nullptr;
	const FrameStore* parsedStore = parsedRasterizer ? parsedRasterizer->GetFrameStore() : nullptr;
	const FrameStore::Metadata parsedMetadata = parsedStore ? parsedStore->Meta() :
		FrameStore::Metadata();
	double maximumPreviewRadiance=0.0,maximumPreviewBlue=0.0,maximumPreviewRed=0.0;
	if(parsedStore) for(unsigned int y=0;y<parsedStore->AsBeautyRasterImage().GetHeight();++y)
		for(unsigned int x=0;x<parsedStore->AsBeautyRasterImage().GetWidth();++x) {
			const RISEColor pixel=parsedStore->AsBeautyRasterImage().GetPEL(x,y);
			Check(std::isfinite(pixel.base.r)&&std::isfinite(pixel.base.g)&&
				std::isfinite(pixel.base.b),"capstone spectral output pixels are finite");
			maximumPreviewRadiance=std::max(maximumPreviewRadiance,
				static_cast<double>(pixel.base.r+pixel.base.g+pixel.base.b));
			maximumPreviewRed=std::max(maximumPreviewRed,static_cast<double>(pixel.base.r));
			maximumPreviewBlue=std::max(maximumPreviewBlue,static_cast<double>(pixel.base.b));
		}
	Check(maximumPreviewRadiance>0.0 && maximumPreviewBlue>maximumPreviewRed,
		"capstone spectral path emits a nonzero bluish synthetic-chem preview");
	const bool publishedSequence = parsedStore && parsedMetadata.activeFireMedia.size()==1u &&
		parsedMetadata.activeFireMedia[0].mediaKind=="sequence_backed" &&
		parsedMetadata.activeFireMedia[0].sequenceId==productionManifest.SequenceId() &&
		parsedMetadata.activeFireMedia[0].selectedBaseFrameIndex==
			(capstoneArtifactRun?5u:4u) &&
		parsedMetadata.activeFireMedia[0].wholeFileDigest==DigestFile(
			capstoneArtifactRun?frame5:frame4) &&
		!parsedMetadata.activeFireMedia[0].preparedInputId.empty();
	Check(publishedSequence,
		"actual prepared render publishes the sequence-backed provenance variant");
	RISECBOR64::Bytes renderedSidecar;
	{
		const std::filesystem::path sidecar=renderBase.string()+".exr.provenance.cbor";
		std::ifstream input(sidecar,std::ios::binary);
		input.seekg(0,std::ios::end);
		const std::streampos size=input.tellg();
		input.seekg(0,std::ios::beg);
		if( size > 0 ) {
			renderedSidecar.resize(static_cast<std::size_t>(size));
			input.read(reinterpret_cast<char*>(renderedSidecar.data()),size);
		}
	}
	RISECBOR64::Value renderedEnvelope;
	const bool renderedEnvelopeValid=RISECBOR64::DecodeCanonical(
		renderedSidecar,renderedEnvelope,&error);
	const RISECBOR64::Value* renderedPayload=renderedEnvelopeValid ?
		renderedEnvelope.Find("payload") : nullptr;
	const RISECBOR64::Value* renderedMedia=renderedPayload ?
		renderedPayload->Find("active_fire_media") : nullptr;
	Check(renderedMedia && renderedMedia->GetType()==RISECBOR64::Value::Array &&
		renderedMedia->GetArray().size()==1u &&
		renderedMedia->GetArray()[0].Find("sequence_id") &&
		renderedMedia->GetArray()[0].Find("sequence_id")->GetText()==
			productionManifest.SequenceId() &&
		renderedMedia->GetArray()[0].Find("whole_file_digest") &&
		renderedMedia->GetArray()[0].Find("prepared_input_id"),
		"real sequence render writes the ratified sequence_backed provenance sidecar");
	const char* capstoneOutput=std::getenv("RISE_FIRE_CAPSTONE_OUTPUT");
	if(capstoneOutput && *capstoneOutput && renderedEnvelopeValid &&
		std::filesystem::exists(renderBase.string()+".exr") &&
		std::filesystem::exists(renderBase.string()+".exr.provenance.cbor")) {
		const std::filesystem::path destination(capstoneOutput);
		std::filesystem::create_directories(destination);
		std::filesystem::copy_file(renderBase.string()+".exr",destination/"methane_preview.exr",
			std::filesystem::copy_options::overwrite_existing);
		std::filesystem::copy_file(renderBase.string()+".exr.provenance.cbor",
			destination/"methane_preview.exr.provenance.cbor",
			std::filesystem::copy_options::overwrite_existing);
		if(std::filesystem::exists(root/"sequence_display.png"))
			std::filesystem::copy_file(root/"sequence_display.png",
				destination/"methane_preview_display.png",
				std::filesystem::copy_options::overwrite_existing);
		if(std::filesystem::exists(root/"sequence_display.png.provenance.cbor"))
			std::filesystem::copy_file(root/"sequence_display.png.provenance.cbor",
				destination/"methane_preview_display.png.provenance.cbor",
				std::filesystem::copy_options::overwrite_existing);
		Check(DigestFile(destination/"frame4.vdb")==DigestFile(frame4)&&
			DigestFile(destination/"frame5.vdb")==DigestFile(frame5),
			"streamed capstone frames remain identical to the completed sequence inputs");
		Check(DurableCopyPublishedFile(manifestPath,
			destination/"sequence_manifest.rise-fire.cbor",error),
			"published capstone manifest is durable");
		std::string runMetadataId;
		const RISECBOR64::Bytes runMetadata=RunMetadataEnvelope(methaneFrameNext,
			capstoneWorkerCount,DigestFile(frame4),DigestFile(frame5),
			productionManifest.SequenceId(),runMetadataId);
		Check(!runMetadata.empty()&&DurableWritePublishedBytes(runMetadata,
			destination/"run_metadata.rise-fire-run.cbor",error),
			"checkpoint cadence, step indices, resume event, and frame streaming enter durable run metadata only");
		FireSequenceManifest copiedManifest;
		FireSequencePreparedFrame copiedFrame;
		const RISECBOR64::Bytes copiedEnvelope=ReadFileBytes(
			destination/"sequence_manifest.rise-fire.cbor");
		Check(!copiedEnvelope.empty()&&
			copiedManifest.LoadCanonicalEnvelope(copiedEnvelope,destination.string(),error)&&
			copiedManifest.LoadFrame(4,copiedFrame,error)&&copiedManifest.LoadFrame(5,copiedFrame,error),
			"published capstone manifest resolves and verifies both copied frame paths");
		Check(std::filesystem::exists(destination/"methane_preview_display.png")&&
			std::filesystem::exists(destination/"methane_preview_display.png.provenance.cbor"),
			"published display derivative carries its own provenance sidecar");
		RISECBOR64::Value productionDecoded;
		if(RISECBOR64::DecodeCanonical(productionEnvelope,productionDecoded,&error)) {
			const RISECBOR64::Value* sequencePayload=productionDecoded.Find("payload");
			const RISECBOR64::Value* caseBytes=sequencePayload?sequencePayload->Find("case_record"):nullptr;
			if(caseBytes && caseBytes->GetType()==RISECBOR64::Value::ByteString) {
				std::ofstream caseOutput(destination/"case_record.rise-fire-case.cbor",std::ios::binary);
				caseOutput.write(reinterpret_cast<const char*>(caseBytes->GetBytes().data()),
					static_cast<std::streamsize>(caseBytes->GetBytes().size()));
			}
		}
		const double hrrFromFuelW=methaneFrameNext.fuelConsumptionKGPerS*
			FireSimulationMethaneRecord::PhysicalV1().LowerHeatingValueJPerKG();
		const RISECBOR64::Value* provenanceId=renderedEnvelope.Find("provenance_id");
		const RISECBOR64::Value* artifactFidelity=renderedPayload?
			renderedPayload->Find("artifact_fidelity"):nullptr;
		const RISECBOR64::Value* artifactDigest=renderedPayload?
			renderedPayload->Find("artifact_sha256"):nullptr;
		const RISECBOR64::Value* rendererBuild=renderedPayload?
			renderedPayload->Find("renderer_build_id"):nullptr;
		const RISECBOR64::Value* configurationId=renderedPayload?
			renderedPayload->Find("resolved_render_configuration_id"):nullptr;
		const RISECBOR64::Value* sidecarMedium=renderedMedia&&
			renderedMedia->GetType()==RISECBOR64::Value::Array&&!renderedMedia->GetArray().empty()?
			&renderedMedia->GetArray()[0]:nullptr;
		const bool empiricalRowsQualified=capstoneArtifactRun&&!capstoneValidationOnly&&
			reportedResolutionTier>=10.0;
		std::ofstream report(destination/"capstone_report.txt");
		std::ofstream json(destination/"methane_preview.exr.provenance.json");
		std::ofstream probes(destination/"centerline_probes.csv");
		std::ofstream timeSteps(destination/"accepted_timestep_history.csv");
		timeSteps << "accepted_step,accepted_time_s,delta_t_s\n";
		double acceptedTime=0.0;
		for(std::size_t step=0;step<methaneFrameNext.acceptedTimeStepHistoryS.size();++step){
			acceptedTime+=methaneFrameNext.acceptedTimeStepHistoryS[step];
			timeSteps << step+1u << ',' << std::setprecision(17) << acceptedTime << ',' <<
				methaneFrameNext.acceptedTimeStepHistoryS[step] << '\n';
		}
		probes << "time_s,height_m,temperature_K,reaction_W_per_m3,vertical_velocity_m_per_s,"
			"centerline_heat_release_W\n";
		for(std::size_t row=0;row<methaneFrameNext.stationProbeTimeS.size();++row) {
			const std::size_t sample=methaneFrameNext.dimensions[2]>0u?
				row/methaneFrameNext.dimensions[2]:0u;
			probes << std::setprecision(17) << methaneFrameNext.stationProbeTimeS[row] << ',' <<
				methaneFrameNext.stationProbeHeightM[row] << ',' <<
				methaneFrameNext.stationProbeTemperatureK[row] << ',' <<
				methaneFrameNext.stationProbeReactionWPerM3[row] << ',' <<
				methaneFrameNext.stationProbeVerticalVelocityMPerS[row] << ',' <<
				(sample<methaneFrameNext.probeCenterlineHeatReleaseW.size()?
					methaneFrameNext.probeCenterlineHeatReleaseW[sample]:0.0) << '\n';
		}
		std::ofstream profile(destination/"centerline_final_profile.csv");
		profile << "height_m,temperature_K,vertical_velocity_m_per_s\n";
		for(std::size_t sample=0;sample<methaneFrameNext.centerlineHeightM.size();++sample)
			profile << std::setprecision(17) << methaneFrameNext.centerlineHeightM[sample] << ',' <<
				methaneFrameNext.centerlineTemperatureK[sample] << ',' <<
				methaneFrameNext.centerlineVelocityMPerS[sample] << '\n';
		WriteJSON(json,renderedEnvelope); json << '\n';
		const double minimumAcceptedStep=methaneFrameNext.acceptedTimeStepHistoryS.empty()?0.0:
			*std::min_element(methaneFrameNext.acceptedTimeStepHistoryS.begin(),
				methaneFrameNext.acceptedTimeStepHistoryS.end());
		const double maximumAcceptedStep=methaneFrameNext.acceptedTimeStepHistoryS.empty()?0.0:
			*std::max_element(methaneFrameNext.acceptedTimeStepHistoryS.begin(),
				methaneFrameNext.acceptedTimeStepHistoryS.end());
		report.precision(17);
		report << "artifact_fidelity=" << (artifactFidelity?artifactFidelity->GetText():"") << "\n"
			<< "render_fidelity_status=" << (renderedPayload&&renderedPayload->Find(
				"render_fidelity_status")?renderedPayload->Find("render_fidelity_status")->GetText():"") << "\n"
			<< "render_reason_codes=" << TextArrayCSV(renderedPayload?
				renderedPayload->Find("render_reason_codes"):nullptr) << "\n"
			<< "artifact_reason_codes=" << TextArrayCSV(renderedPayload?
				renderedPayload->Find("artifact_reason_codes"):nullptr) << "\n"
			<< "capstone_reason_codes=preview_primary,synthetic_chem_fixture,"
				"methane_zero_soot_yield,cold_start_r70_continuous_pilot,"
				"r59_two_class_limiter,r70_manifold_exact_acceptance\n"
			<< "diagnostic_narrative=zero_gravity_harness_defect_was_rejected_by_empirical_gate;"
				"gravity_restored;invariant_limit_identity_closed_stationary_hot_cold_contrast\n"
			<< "empirical_rows_status=" << (empiricalRowsQualified?
				"capstone_tier10":"pipeline_validation_only") << "\n"
			<< "provenance_id=" << (provenanceId?provenanceId->GetText():"") << "\n"
			<< "sidecar_artifact_sha256=" << (artifactDigest?artifactDigest->GetText():"") << "\n"
			<< "renderer_build_id=" << (rendererBuild?rendererBuild->GetText():"") << "\n"
			<< "resolved_render_configuration_id=" <<
				(configurationId?configurationId->GetText():"") << "\n"
			<< "case_record_id=" << productionManifest.CaseRecordId() << "\n"
			<< "sequence_id=" << productionManifest.SequenceId() << "\n"
			<< "run_metadata_id=" << runMetadataId << "\n"
			<< "checkpoint_cadence_wall_s=" << methaneFrameNext.checkpointCadenceWallS << "\n"
			<< "checkpoint_count=" << methaneFrameNext.checkpointStepIndices.size() << "\n"
			<< "checkpoint_step_indices=";
		for(std::size_t checkpoint=0;checkpoint<methaneFrameNext.checkpointStepIndices.size();
			++checkpoint)report << (checkpoint?",":"") <<
				methaneFrameNext.checkpointStepIndices[checkpoint];
		report << "\n"
			<< "resumed_from_checkpoint=" <<
				(methaneFrameNext.resumedFromCheckpoint?"true":"false") << "\n"
			<< "resumed_from_step=" << methaneFrameNext.resumedFromStep << "\n"
			<< "streamed_frame_count=" << methaneFrameNext.streamedFrameCount << "\n"
			<< "prepared_input_id=" << (sidecarMedium&&sidecarMedium->Find("prepared_input_id")?
				sidecarMedium->Find("prepared_input_id")->GetText():"") << "\n"
			<< "selected_base_frame_index=" << (sidecarMedium&&
				sidecarMedium->Find("selected_base_frame_index")?
				sidecarMedium->Find("selected_base_frame_index")->GetIntegerArgument():0u) << "\n"
			<< "frame4_sha256=" << DigestFile(frame4) << "\n"
			<< "frame5_sha256=" << DigestFile(frame5) << "\n"
			<< "frame5_policy=honest_cold_start_sustained_solver_state_with_synthetic_chem_fixture\n"
			<< "thread_determinism_workers=1," << capstoneWorkerCount << "\n"
			<< "thread_determinism_digest=" << singleWorkerDigest << "\n"
			<< "run_reduction_mode=" << methaneFrameNext.reductionMode << "\n"
			<< "run_worker_count_history=";
		for(std::size_t segment=0;segment<methaneFrameNext.workerCountHistory.size();++segment)
			report << (segment?",":"") << methaneFrameNext.workerCountHistory[segment];
		report << "\n"
			<< "r59_discontinuous_limiter_steps=" <<
				methaneFrameNext.discontinuousLimiterClassSteps << "\n"
			<< "r59_maximum_limiter_face_discrepancy=" <<
				methaneFrameNext.maximumLimiterClassDiscrepancy << "\n"
			<< "r59_discontinuous_class_thread_identity=" <<
				(methaneFrameNext.discontinuousClassThreadIdentityChecked&&
				methaneFrameNext.discontinuousClassThreadIdentity?"true":"not_exercised") << "\n"
			<< "r81_active_set_algorithm_version=" <<
				methaneFrameNext.activeSetAlgorithmVersion << "\n"
			<< "r81_prior_active_set_algorithm_version=" <<
				(methaneFrameNext.priorActiveSetAlgorithmVersion.empty()?"none":
					methaneFrameNext.priorActiveSetAlgorithmVersion) << "\n"
			<< "r81_discontinuous_active_set_events=" <<
				methaneFrameNext.discontinuousActiveSetEvents << "\n"
			<< "r81_maximum_active_set_complementarity_discrepancy_m_per_s=" <<
				methaneFrameNext.maximumActiveSetComplementarityDiscrepancyMPerS << "\n"
			<< "r81_maximum_active_set_cycle_length=" <<
				methaneFrameNext.maximumActiveSetCycleLength << "\n"
			<< "r81_maximum_active_set_differing_face_count=" <<
				methaneFrameNext.maximumActiveSetDifferingFaceCount << "\n"
			<< "r81_active_set_thread_identity=" <<
				(methaneFrameNext.activeSetThreadIdentityChecked&&
				methaneFrameNext.activeSetThreadIdentity?"true":"not_exercised") << "\n"
			<< "r54_pinned_selected_timestep_s=" << methaneFrameNext.selectedTimeStepS << "\n"
			<< "r57_resolution_tier=" << reportedResolutionTier << "\n"
			<< "accepted_final_timestep_s=" << methaneFrameNext.acceptedTimeStepS << "\n"
			<< "accepted_timestep_min_s=" << minimumAcceptedStep << "\n"
			<< "accepted_timestep_max_s=" << maximumAcceptedStep << "\n"
			<< "simulated_time_s=" << methaneFrameNext.simulatedTimeS << "\n"
			<< "flow_through_time_s=" << methaneFrameNext.flowThroughTimeS << "\n"
			<< "r68_maximum_pilot_approach_eos_residual=" <<
				methaneFrameNext.maximumPilotApproachEOSResidual << "\n"
			<< "r69_maximum_accepted_eos_residual=" <<
				methaneFrameNext.maximumAcceptedEOSResidual << "\n"
			<< "r72_active_hold_temperature_min_K=" <<
				methaneFrameNext.minimumActiveHoldTemperatureK << "\n"
			<< "r72_active_hold_temperature_max_K=" <<
				methaneFrameNext.maximumActiveHoldTemperatureK << "\n"
			<< "pilot_energy_J=" << methaneFrameNext.pilotEnergyJ << "\n"
			<< "pilot_ignited_during_window=" << (methaneFrameNext.ignitedDuringPilot?"true":"false") << "\n"
			<< "combustion_sustained_after_pilot=" << (methaneFrameNext.sustainedAfterPilot?"true":"false") << "\n"
			<< "rendered_exr_sha256=" << DigestFile(renderBase.string()+".exr") << "\n"
			<< "sidecar_sha256=" << DigestFile(renderBase.string()+".exr.provenance.cbor") << "\n"
			<< "display_preview=PNG_ACES_exposure_plus20_non_primary_derivative\n";
		if(empiricalRowsQualified) {
			const double observationSpanS=methaneFrameNext.probeTimeS.empty()?0.0:
				methaneFrameNext.probeTimeS.back()-methaneFrameNext.probeTimeS.front();
			report << "puffing_status=capstone_tier10_evaluated\n"
				<< "puffing_reference_Hz=" << expectedPuffingHz << "\n"
				<< "statistics_start_s=" << methaneFrameNext.statisticsStartS << "\n"
				<< "statistics_duration_s=" << std::max(0.0,methaneFrameNext.simulatedTimeS-
					methaneFrameNext.statisticsStartS) << "\n"
				<< "puffing_observation_span_s=" << observationSpanS << "\n"
				<< "puffing_observed_Hz=" << methaneFrameNext.puffingFrequencyHz << "\n"
				<< "puffing_observed_cycles=" << observationSpanS*
					methaneFrameNext.puffingFrequencyHz << "\n"
				<< "puffing_relative_error=" << methaneFrameNext.puffingRelativeError << "\n"
				<< "puffing_observation=preview_single_diameter_at_least_30_observed_cycles_full_three_diameter_refinement_gate_remains_separate\n"
				<< "mccaffrey_status=capstone_tier10_evaluated\n"
				<< "mccaffrey_temperature_power_exponent=" << methaneFrameNext.centerlineTemperatureExponent << "\n"
				<< "mccaffrey_temperature_log_fit_RMSE=" << methaneFrameNext.centerlineFitRMSE << "\n"
				<< "mccaffrey_flame_tip_height_m=" << methaneFrameNext.mccaffreyFlameTipHeightM << "\n"
				<< "mccaffrey_plume_station_count=" << methaneFrameNext.mccaffreyPlumeStationCount << "\n"
				<< "mccaffrey_max_temperature_relative_error=" <<
					methaneFrameNext.mccaffreyMaximumTemperatureRelativeError << "\n"
				<< "mccaffrey_max_velocity_relative_error=" <<
					methaneFrameNext.mccaffreyMaximumVelocityRelativeError << "\n"
				<< "mccaffrey_observation=time_averaged_above_reaction_tip_NBSIR_79_1910_Table_1_33_kW_absolute_T_and_u\n"
				<< "hrr_status=capstone_tier10_evaluated\n"
				<< "realized_HRR_W=" << methaneFrameNext.realizedHeatReleaseW << "\n"
				<< "fuel_consumption_times_LHV_W=" << hrrFromFuelW << "\n"
				<< "HRR_relative_ledger_error=" << std::fabs(methaneFrameNext.realizedHeatReleaseW-
					hrrFromFuelW)/std::max(1.0,methaneFrameNext.realizedHeatReleaseW) << "\n"
				<< "radiative_fraction_status=capstone_tier10_evaluated\n"
				<< "declared_chi_r=" << methaneFrameNext.effectiveRadiativeFraction << "\n"
				<< "realized_step_radiative_fraction=" << methaneFrameNext.realizedRadiativeFraction << "\n"
				<< "statistics_integrated_heat_release_J=" << methaneFrameNext.integratedHeatReleaseJ << "\n"
				<< "statistics_integrated_radiative_loss_J=" << methaneFrameNext.integratedRadiativeLossJ << "\n"
				<< "statistics_integrated_fuel_consumption_kg=" << methaneFrameNext.integratedFuelConsumptionKG << "\n"
				<< "statistics_integrated_fuel_times_LHV_J=" << methaneFrameNext.integratedFuelConsumptionKG*
					FireSimulationMethaneRecord::PhysicalV1().LowerHeatingValueJPerKG() << "\n"
				<< "statistics_integrated_radiative_fraction=" << methaneFrameNext.integratedRadiativeFraction << "\n"
				<< "accepted_escape_factor=" << methaneFrameNext.acceptedEscapeFactor << "\n"
				<< "radiative_status=preview_final_step_combustion_only_chi_r_diagnostic\n";
		} else {
			report << "puffing_status=not_evaluated_pipeline_validation_only\n"
				<< "mccaffrey_status=not_evaluated_pipeline_validation_only\n"
				<< "hrr_status=not_evaluated_pipeline_validation_only\n"
				<< "radiative_fraction_status=not_evaluated_pipeline_validation_only\n";
		}
		report << "visible_expectation=faint_methane_y_s_zero_with_bluish_synthetic_chem_fixture\n";
	}
	Check(parsedMedium && parsedMedium->ForTest_FireMajorantGeneration()==beforeRenderMajorant+1u,
		"actual render schedules exactly one per-frame majorant/CDF rebuild");
	const std::string jobFirstPrepared = parsedStore &&
		!parsedMetadata.activeFireMedia.empty() ?
		parsedMetadata.activeFireMedia[0].preparedInputId : std::string();
	const double secondProductionRenderTime=capstoneArtifactRun?10.0:10.25;
	Check(parsedJob && parsedJob->SetAnimationOptions(secondProductionRenderTime,
		secondProductionRenderTime,1,false,false) &&
		parsedJob->Rasterize(),
		"between-render scene-time advance prepares the next immutable frame");
	const FrameStore::Metadata advancedMetadata = parsedStore ? parsedStore->Meta() :
		FrameStore::Metadata();
	Check(parsedStore && advancedMetadata.activeFireMedia.size()==1u &&
		advancedMetadata.activeFireMedia[0].selectedBaseFrameIndex==
			(capstoneArtifactRun?4u:5u) &&
		advancedMetadata.activeFireMedia[0].preparedInputId!=jobFirstPrepared &&
		parsedMedium && parsedMedium->ForTest_FireMajorantGeneration()==beforeRenderMajorant+2u,
		"frame advance atomically swaps grid, majorant, CDF, and provenance between renders");
	FrozenMutationOutput* mutationOutput=parsedJob ? new FrozenMutationOutput(*parsedJob) : nullptr;
	if( parsedRasterizer && mutationOutput )
		const_cast<IRasterizer*>(parsedRasterizer)->AddRasterizerOutput(mutationOutput);
	Check(parsedJob && parsedJob->Rasterize() && mutationOutput && mutationOutput->attempted &&
		mutationOutput->rejected && parsedMedium &&
		parsedMedium->ForTest_FireMajorantGeneration()==beforeRenderMajorant+2u,
		"mid-render Job mutation is detected while the prepared grid/majorant/CDF stay frozen");
	if( mutationOutput ) mutationOutput->release();
	// The adversarial interlace fixture has its own fixed 0.25 s cadence.  The
	// capstone manifest intentionally uses the much longer empirical-window
	// cadence and is not a substitute for that timing construction.
	if(!capstoneArtifactRun) {
		const double interlaceFixtureFrameStepS=0.25;
		Check(parsedJob && !parsedJob->RasterizeAnimation(10.0,
			10.0+2.0*interlaceFixtureFrameStepS,2,true,false) && parsedMedium &&
			parsedMedium->ForTest_FireMajorantGeneration()==beforeRenderMajorant+2u,
			"cadence-crossing interlaced fields reject before one artifact can misstate its base frame");
		Check(parsedJob && parsedJob->RasterizeAnimation(10.0+interlaceFixtureFrameStepS,
			10.0+interlaceFixtureFrameStepS,1,true,false) && parsedMedium &&
			parsedMedium->ForTest_FireMajorantGeneration()==beforeRenderMajorant+2u,
			"same-base-frame interlaced fields reuse one truthful prepared identity");
	}
	const IFireRasterizerState* preparedState=parsedRasterizer ?
		dynamic_cast<const IFireRasterizerState*>(parsedRasterizer) : nullptr;
	if( parsedRasterizer && parsedJob && parsedJob->GetScene() )
		parsedRasterizer->RasterizeScene(*parsedJob->GetScene(),nullptr,nullptr);
	Check(preparedState && !preparedState->LastRenderCompleted() && parsedMedium &&
		parsedMedium->ForTest_FireMajorantGeneration()==beforeRenderMajorant+2u,
		"legacy direct rasterizer entry cannot bypass sequence preparation or mutate state");
	if( parsedJob ) parsedJob->release();

	FrameStoreOutput::Metadata metadata;
	metadata.renderFidelityStatus = "preview";
	metadata.renderReasonCodes = {"requested_preview"};
	metadata.activeFireOpticsRecordIds = {std::string(64u,'b')};
	FrameStoreOutput::ActiveFireMedium sequenceMedium;
	sequenceMedium.mediaKind = "sequence_backed";
	sequenceMedium.managerName = "fire";
	sequenceMedium.bindingKind = "global_medium";
	sequenceMedium.bindingOwner = "scene";
	sequenceMedium.opticalRecordIds = metadata.activeFireOpticsRecordIds;
	sequenceMedium.sequenceId = manifest.SequenceId();
	sequenceMedium.selectedBaseFrameIndex = controller.PreparedFrame()->frameIndex;
	sequenceMedium.wholeFileDigest = controller.PreparedFrame()->wholeFileSha256;
	sequenceMedium.sourceKind = manifest.SourceKind();
	sequenceMedium.physicalMapping = manifest.PhysicalMapping();
	sequenceMedium.effectiveBlurState = "disabled";
	sequenceMedium.preparedInputId = controller.PreparedInputId();
	sequenceMedium.preparedStateGeneration = controller.Generation();
	metadata.activeFireMedia.push_back(sequenceMedium);
	metadata.resolvedRenderConfigCoreV1 = FireOutputMetadataTestFixture::ResolvedConfig(2,2);
	metadata.rendererBuildV1 = FireOutputMetadataTestFixture::RendererBuild();
	metadata.rendererBuildId = RISECBOR64::SHA256Hex(metadata.rendererBuildV1);
	Check(FrameStoreOutput::ValidateFireOutputMetadata(metadata,error),
		"ratified active_fire_media sequence_backed tagged variant validates");
	metadata.activeFireMedia[0].authoredConfigDigest = std::string(64u,'c');
	Check(!FrameStoreOutput::ValidateFireOutputMetadata(metadata,error),
		"sequence_backed provenance rejects a structurally present static-only field");

	std::filesystem::remove_all(root);
	std::printf("FireSequenceTest: canonical sequence/loadability/preparation gates passed\n");
	return failures ? 1 : 0;
#endif
}
