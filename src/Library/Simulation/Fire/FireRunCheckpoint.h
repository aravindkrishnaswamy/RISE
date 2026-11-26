//////////////////////////////////////////////////////////////////////
//
//  FireRunCheckpoint.h - Fire run state and checkpoint codec (fire-private)
//
//  The production fire driver's accepted-run state (SolverFrameValues,
//  MethaneRunCheckpoint), the version 5..13 checkpoint codec with its
//  checksum, payload binding and legacy migration, and the durable
//  publication helpers (payload-v2 Merkle sidecars, atomic replacement,
//  run-directory sealing).  Moved verbatim from tests/FireSequenceTest.cpp
//  (simulation M2, track B); the file format is unchanged.
//
//  Not a public RISE API: the generic simulation contract wraps this in
//  track C.  Compiled without fast-math or FP contraction, like the test
//  translation unit it came from.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_FIRE_RUN_CHECKPOINT_H
#define RISE_SIMULATION_FIRE_RUN_CHECKPOINT_H

#include "../../Utilities/RISECBOR64.h"
#include "../../Utilities/FireProductionForce.h"
#include "../../Utilities/FireProductionProjection.h"
#include "../../../../tools/fire_simulator_core.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace RISE
{
	namespace FireRun
	{
	using namespace RISE::FireSim;

	//! Test-only checkpoint mutations.  Production never installs hooks; with
	//! none installed every flag reads false.  The pointer is not owned.
	struct FireRunCheckpointTestHooks
	{
		bool suppressExpectedCheckpointStateDiagnostic=false;
		bool forceMalformedManifoldLifecycleWrite=false;
		bool forcePostRenameDirectorySyncFailure=false;
	};
	void SetFireRunCheckpointTestHooks(const FireRunCheckpointTestHooks* hooks);

	//! Invariant checks carried over from the test translation unit report
	//! here.  Without a handler a failed check prints "FAIL: <message>" to
	//! stderr and the run continues, as before; the fire test installs its
	//! failure counter.
	typedef void (*FireRunCheckHandler)(bool condition,const char* message);
	void SetFireRunCheckHandler(FireRunCheckHandler handler);
	void ReportFireRunCheck(bool condition,const char* message);

	const char* CurrentActiveSetAlgorithmVersion();
	const char* LegacyActiveSetAlgorithmVersion();

	inline std::uint64_t DoubleBits(const double value)
	{
		std::uint64_t bits=0u;std::memcpy(&bits,&value,sizeof(bits));return bits;
	}

	struct SolverFrameValues
	{
		bool succeeded=false;
		std::string structuredError;
		std::array<std::size_t,3> dimensions={{0,0,0}};
		double cellWidthM=0.5;
		std::string caseRecordId;
		float temperatureK=900.0f,reactionWPerM3=0.0f;
		std::vector<float> temperature, reaction, carbon; // K, W/m3, kg/m3 (carbon converts at VDB export)
		std::vector<std::array<float,3> > velocity;
		double realizedHeatReleaseW=0.0, fuelConsumptionKGPerS=0.0;
		double realizedRadiativeFraction=0.0, acceptedEscapeFactor=0.0;
		double selectedTimeStepS=0.0,acceptedTimeStepS=0.0;
		double simulatedTimeS=0.0,flowThroughTimeS=0.0,pilotEnergyJ=0.0;
		// Diagnostic fields are not part of the legacy checkpoint wire layout.
		double matchedScheduleFlowThroughTimeS=0.0;
		double projectionReferenceLengthM=0.0,projectionTolerancePerS=0.0;
		double velocityToleranceMPerS=0.0,pressureTolerancePa=0.0;
		std::uint64_t r246PreallocationReserveBytes=0u;
		bool r246InitialFieldsAllocated=false;
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
		std::vector<double> monitoredManifoldMaximumHistory;
		std::vector<double> monitoredManifoldP95History;
		std::vector<double> monitoredManifoldP50History;
		std::vector<std::uint32_t> monitoredTailCellHistory;
		std::vector<double> monitoredTailDrainedVolumeHistoryM3;
		std::vector<double> productionDeviceHistoryMS;
		std::vector<double> productionWallHistoryMS;
		std::uint64_t productionHardBoundRetryCount=0u;
		std::uint64_t productionPhysicalProjectionRetryCount=0u;
		double externalFuelMassKG=0.0;
		std::string burnerFixtureQ0SHA,burnerFixtureAcceptedSHA;
		std::vector<std::string> burnerFixtureInputSHA;
		bool burnerFixtureVelocityMutantDetected=false;
	};

	//! Selects CheckpointWriter's in-memory file: the bytes are exactly the file
	//! the path writer would produce (header, payload, header rewrite).
	struct InMemoryCheckpointFile {};

	// M8 portability: checkpoint values are written in host byte order behind
	// the 0x0102030405060708 endian tag, which readers check (a foreign-order
	// file is refused, never misread). Every supported target (macOS arm64,
	// Windows x64/ARM64, Android arm64) is little-endian and 64-bit, so the
	// same bytes read everywhere; std::size_t and bool fields rely on these.
	static_assert(sizeof(std::size_t)==8u&&sizeof(bool)==1u&&sizeof(float)==4u&&sizeof(double)==8u,
		"fire checkpoint layout assumes a 64-bit target with 1-byte bool");
	class CheckpointWriter
	{
	public:
		explicit CheckpointWriter(const std::filesystem::path& path) :
			output_(path,std::ios::binary|std::ios::trunc),digestOnly_(false) {}
		CheckpointWriter() : digestOnly_(true) {}
		explicit CheckpointWriter(RISECBOR64::Bytes& bytes) : digestOnly_(true),memory_(&bytes)
			{bytes.clear();}
		CheckpointWriter(InMemoryCheckpointFile,RISECBOR64::Bytes& file) : digestOnly_(false),file_(&file)
			{file.clear();}
		bool Good() const { return digestOnly_||file_||static_cast<bool>(output_); }
		bool HeaderBytes(const void* data,const std::size_t size)
		{
			if(digestOnly_)return false;
			if(file_)return Put(data,size);
			output_.write(static_cast<const char*>(data),static_cast<std::streamsize>(size));
			return static_cast<bool>(output_);
		}
		bool SeekHeader(const std::streamoff offset)
		{
			if(digestOnly_)return false;
			if(file_){
				if(offset<0||static_cast<std::uint64_t>(offset)>file_->size())return false;
				position_=static_cast<std::size_t>(offset);return true;
			}
			output_.seekp(offset);return static_cast<bool>(output_);
		}
		template<typename T> bool Pod(const T& value)
		{
			static_assert(std::is_arithmetic<T>::value,"checkpoint POD must be arithmetic");
			static_assert(sizeof(T)==1u||sizeof(T)==4u||sizeof(T)==8u,"checkpoint POD width must match on every target");
			return Bytes(&value,sizeof(value));
		}
		bool Bytes(const void* data,const std::size_t size)
		{
			if(!digestOnly_&&!file_&&!output_)return false;
			if(file_){if(!Put(data,size))return false;}
			else if(!digestOnly_)output_.write(static_cast<const char*>(data),static_cast<std::streamsize>(size));
			const unsigned char* byte=static_cast<const unsigned char*>(data);
			if(memory_)memory_->insert(memory_->end(),byte,byte+size);
			for(std::size_t i=0;i<size;++i){checksum_^=byte[i];checksum_*=1099511628211ull;}
			payloadBytes_+=static_cast<std::uint64_t>(size);
			return digestOnly_||file_||static_cast<bool>(output_);
		}
		bool String(const std::string& value)
		{
			const std::uint64_t size=static_cast<std::uint64_t>(value.size());
			return Pod(size)&&(size==0u||Bytes(value.data(),value.size()));
		}
		bool Finish()
		{
			if(digestOnly_||file_)return true;
			output_.flush();output_.close();return !output_.fail();
		}
		std::uint64_t Checksum() const { return checksum_; }
		std::uint64_t PayloadBytes() const { return payloadBytes_; }
	private:
		// In-memory file: write at the cursor, growing the file like a stream.
		bool Put(const void* data,const std::size_t size)
		{
			if(size>file_->max_size()-position_)return false;
			const std::size_t end=position_+size;
			if(end>file_->size())file_->resize(end);
			if(size)std::memcpy(file_->data()+position_,data,size);
			position_=end;return true;
		}
		std::ofstream output_;
		bool digestOnly_;
		RISECBOR64::Bytes* memory_=nullptr;
		RISECBOR64::Bytes* file_=nullptr;
		std::size_t position_=0u;
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
			static_assert(sizeof(T)==1u||sizeof(T)==4u||sizeof(T)==8u,"checkpoint POD width must match on every target");
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

	class Binary64CheckpointAuthority final
	{
	public:
		Binary64CheckpointAuthority() : digest_(0u) {}
		bool Available() const { return digest_!=0u; }
		std::uint64_t Digest() const { return digest_; }
	private:
		std::uint64_t digest_;
		friend bool IssueBinary64CheckpointAuthority(struct MethaneRunCheckpoint&);
		friend bool RestoreBinary64CheckpointAuthority(struct MethaneRunCheckpoint&,
			std::uint64_t);
	};

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
		Binary64CheckpointAuthority binary64CheckpointAuthority;
	};

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

	enum class PayloadPublicationPolicy { Immutable,MutableCurrentCheckpoint };

	template<class Stream> bool ClosePublishedStream(Stream& stream)
	{
		stream.close();return static_cast<bool>(stream);
	}

	bool WriteSolverFrameValues(CheckpointWriter& w,const SolverFrameValues& v);
	bool ReadSolverFrameValues(CheckpointReader& r,SolverFrameValues& v);
	bool AcceptedCheckpointTimelineValid(const MethaneRunCheckpoint& checkpoint);
	bool Binary64CheckpointStateAdmissible(const MethaneRunCheckpoint& checkpoint);
	bool CanonicalizeLegacyBinary64CheckpointTemperature(MethaneRunCheckpoint& checkpoint);
	bool IssueBinary64CheckpointAuthority(MethaneRunCheckpoint& checkpoint);
	bool RestoreBinary64CheckpointAuthority(MethaneRunCheckpoint& checkpoint,
		const std::uint64_t storedDigest);
	bool BuildCheckpointAcceptedStatePayload(const MethaneRunCheckpoint& checkpoint,
		FireProductionProjectionShape& shape,std::vector<float>& conservative,
		std::array<std::vector<float>,3>& momentum,
		std::array<std::vector<float>,3>& velocity,std::uint64_t& digest);
	bool CheckpointAcceptedStateDigest(const MethaneRunCheckpoint& checkpoint,
		std::uint64_t& digest);
	bool CheckpointProductionBeginningSHA256(const MethaneRunCheckpoint& checkpoint,
		std::string& digest);
	bool CheckpointAcceptedStateMatchesObservation(const MethaneRunCheckpoint& checkpoint,
		const FireProductionAcceptedManifoldObservation& observation,
		std::uint64_t& legacyDigest);
	bool HomogeneousStateProducerPrecision(const std::vector<MethaneCellState>& states,
		FireStateProducerPrecision& precision);
	bool WriteCellStates(CheckpointWriter& writer,const std::vector<MethaneCellState>& states,
		const std::uint64_t version);
	bool ReadCellStates(CheckpointReader& reader,std::vector<MethaneCellState>& states,
		const std::uint64_t version);
	bool WriteCheckpointSerializedPrefix(CheckpointWriter& writer,
		const MethaneRunCheckpoint& checkpoint,const std::uint64_t version);
	std::uint64_t Binary64CheckpointPayloadDigest(const MethaneRunCheckpoint& checkpoint);
	bool WriteCheckpointPayload(CheckpointWriter& writer,const MethaneRunCheckpoint& checkpoint,
		const std::uint64_t version);
	bool ReadCheckpointPayload(CheckpointReader& reader,MethaneRunCheckpoint& checkpoint,
		const std::uint64_t version);
	bool DurableSyncFileAndDirectory(const std::filesystem::path& path,std::string& error);
	bool AtomicReplaceCheckpoint(const std::filesystem::path& temporary,
		const std::filesystem::path& target,std::string& error);
	bool BuildPayloadMerkleCertificateBytes(const RISECBOR64::Bytes& bytes,
		const std::string& caseRecordId,std::string& certificateText,std::string& error);
	bool BuildPayloadMerkleCertificate(const std::filesystem::path& path,
		const std::string& caseRecordId,std::string& certificateText,std::string& error);
	bool PublishPayloadMerkleSidecar(const std::filesystem::path& path,
		const std::string& caseRecordId,std::string& error);
	bool ExistingPayloadCertificateValid(const std::filesystem::path& path,
		const std::string& caseRecordId,std::string& error);
	void CleanupPreparedPayload(const std::filesystem::path& prepared,const bool published);
	bool PublishPreparedPayload(const std::filesystem::path& prepared,
		const std::filesystem::path& target,const std::string& caseRecordId,
		const PayloadPublicationPolicy policy,std::string& error);
	bool InitializePublishedCheckpointCopy(const std::filesystem::path& source,
		const std::filesystem::path& target,const std::string& caseRecordId,std::string& error);
	bool SealPublishedRunDirectory(const std::filesystem::path& directory,
		const std::string& caseRecordId,std::string& error);
	bool SaveMethaneRunCheckpoint(const std::filesystem::path& path,
		const MethaneRunCheckpoint& checkpoint,std::string& error,
		const std::uint64_t version=13u);
	//! The bytes SaveMethaneRunCheckpoint would write, in memory (M2 track C).
	bool SerializeMethaneRunCheckpoint(const MethaneRunCheckpoint& checkpoint,
		RISECBOR64::Bytes& file,std::string& error,const std::uint64_t version=13u);
	PayloadPublicationPolicy CheckpointPublicationPolicy(const std::filesystem::path& output,
		const std::filesystem::path& current,const bool finalDue,const bool equivalenceDue);
	bool SavePublishedCheckpoint(const std::filesystem::path& path,
		const MethaneRunCheckpoint& checkpoint,const PayloadPublicationPolicy policy,std::string& error);
	bool VerifyCheckpointPayloadChecksum(const std::filesystem::path& path,
		const std::uint64_t payloadBytes,const std::uint64_t expected,std::string& error);
	bool LoadMethaneRunCheckpoint(const std::filesystem::path& path,
		MethaneRunCheckpoint& checkpoint,std::string& error);
	std::string DigestFile( const std::filesystem::path& path );
	RISECBOR64::Bytes ReadFileBytes( const std::filesystem::path& path );
	}
}

#endif
