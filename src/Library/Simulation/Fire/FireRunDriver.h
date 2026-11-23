//////////////////////////////////////////////////////////////////////
//
//  FireRunDriver.h - Production fire run driver (fire-private)
//
//  The accepted-step driver of the methane pool fire: case derivation,
//  initial state or checkpoint restore, the resident projected-Heun owner
//  loop with its retries and exact event selection, temporal frame
//  publication, wall-time checkpoints and retained snapshots.  Moved
//  verbatim from tests/FireSequenceTest.cpp (simulation M2, track B); the
//  historical diagnostic modes keep their scope checks.  Fault injection
//  and fixture-only paths are reachable only through FireRunTestHooks,
//  which production never installs.
//
//  The generic simulation contract (track C) wraps RunFireMethaneDriver;
//  see FireRun.h for the options and observers it needs.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_FIRE_RUN_DRIVER_H
#define RISE_SIMULATION_FIRE_RUN_DRIVER_H

#include "FireRunFrames.h"
#include "FireRunHostMemory.h"
#include "FireProductionAcceptedStageBudget.h"
#include <algorithm>
#include <atomic>
#include <istream>
#include <sstream>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace RISE
{
	namespace FireRun
	{
	//! Test-only fault injection and fixture recording.  Production passes
	//! none (FireRunPersistenceOptions::testHooks is null) and every flag
	//! reads false.
	struct FireRunTestHooks
	{
		bool killAfterFirstCheckpoint=false;
		//! Called after the first published checkpoint when
		//! killAfterFirstCheckpoint is set (the test kills its own process).
		void (*killProcess)()=nullptr;
		bool injectSolverFailure=false;
		bool forceActiveSetIdentityCheckForTest=false;
		bool injectActiveSetIdentityMismatchForTest=false;
		bool forceZeroSourceForTest=false;
		bool burnerLauncherFixture=false,burnerLauncherRejectFirstCandidate=false;
		bool r246MemoryAdmissionRED=false,r246DeviceAdmissionRED=false;
		bool r245ObservationFixture=false;
		bool hostMemoryResumeStopRED=false;
		// Synthetic retention is admitted only by the isolated 24-step RSS RED.
		bool hostMemoryRetentionRED=false;
	};

	//! Integration policy, persistence and observers of one driver run.
	//! Field meanings and defaults are those of the historical test-file
	//! RunPersistenceOptions; the fields after "M2 additions" are new and
	//! neutral when left at their defaults.
	struct FireRunPersistenceOptions
	{
		std::filesystem::path checkpointPath;
		std::filesystem::path finalCheckpointPath;
		std::filesystem::path retainedCheckpointDirectory;
		double checkpointCadenceWallS=0.0;
		std::uint64_t streamedFrameCountAtStart=0u;
		bool resume=false;
		std::filesystem::path resumeEquivalenceCertificatePath;
		bool isolatedEquivalenceProbe=false;
		std::string isolatedExpectedCheckpointBuildId;
		std::string isolatedExpectedCheckpointDigest;
		std::uint64_t stopAfterAdditionalAcceptedSteps=0u;
		std::filesystem::path equivalenceSnapshotDirectory;
		bool productionMetal=false;
		// r212 engineering adoption. Historical baseline/replay flags remain
		// explicit; the temporal production entry selects this full owner.
		bool portedProductionTransport=false;
		bool r244LateralDouble=false;
		// Explicit taller commands retain full-window scope; campaign launch requires final review.
		bool r246TopDouble=false,r246FullWindow=false;
		bool r245AcceptedBudget=false;
		bool compatibleMomentumDiagnostic=false;
		bool singleStageFCTDiagnostic=false;
		bool sealedLegacyMomentumReplay=false;
		bool sealedProjectedHeunReplay=false;
		bool sealedProjectedHeunContinuation=false;
		bool projectedHeunCostDiagnostic=false;
		bool projectedHeunEOSDiagnostic=false;
		// Isolated checkpoint reporting probe, never a migration/publication authority.
		bool projectedHeunCertificateDiagnostic=false;
		// Isolated retained-state fuel observation; never empirical continuation.
		bool acceptedFuelBudgetDiagnostic=false;
		// Versioned r216 scope: original tier-ten kernel and retained step 792.
		bool tier10RetainedContinuation=false;
		bool r217RefusalDiagnostic=false;
		bool r222RefusalDiagnostic=false;
		bool r225RefusalDiagnostic=false;
		bool r241RefusalDiagnostic=false;
		bool r220ResumeProbe=false;
		bool r221Retained=false;
		bool UsesProjectedHeunOwner() const
		{
			return portedProductionTransport||sealedProjectedHeunReplay||sealedProjectedHeunContinuation||
				projectedHeunCostDiagnostic||projectedHeunEOSDiagnostic||acceptedFuelBudgetDiagnostic;
		}
		std::filesystem::path replayProtocolPath;
		std::string replayProtocolDigest;
		double productionMomentumObservationTimeS=0.0;
		std::size_t productionMomentumObservationColumnX=0u;
		std::size_t productionMomentumObservationColumnY=0u;
		double productionMomentumObservationReferenceTier=0.0;
		std::filesystem::path temporalSnapshotDirectory;
		double temporalSnapshotCadenceS=0.0;
		// r275 follow mode: called once the frame VDB, its sidecar and its .time record are all
		// published (frame index = number of the frame just written). Observation only: it must not
		// touch the solver state. A false return is logged and ignored; the run continues.
		std::function<bool(const SolverFrameValues&,std::uint64_t)> temporalFrameObserver;
		double maximumProductionSourceStepS=0.0;
		// r213 diagnostic composition only; never a production timestep policy
		// or checkpoint migration authorization.
		std::filesystem::path oracleCompositionDirectory;
		double maximumOracleCompositionStepS=0.0;
		std::filesystem::path productionOnsetDiagnosticDirectory;
		double productionOnsetStopVelocityMPerS=0.0;

		// M2 additions (track B).  All neutral at their defaults.
		//! Bounded window: the loop also stops once the accepted time reaches
		//! the start time plus windowS (0 = no window).  Exactly the r271 A/B
		//! harness bound (RISE_R271_WINDOW_S), now an option.
		double windowS=0.0;
		//! Temporal frame clock origin instead of the statistics start (the
		//! r271/r273 A/B harness's RISE_R273_FRAME_ORIGIN_S).
		bool hasTemporalFrameOrigin=false;
		double temporalFrameOriginS=0.0;
		//! Immutable step_<n>.checkpoint every n accepted steps after the start
		//! (the r273 harness's RISE_R273_SNAPSHOT_EVERY/_DIR); 0 = off.
		std::uint64_t retainedSnapshotEvery=0u;
		std::filesystem::path retainedSnapshotDirectory;
		//! Largest stopAfterAdditionalAcceptedSteps admitted for a retained
		//! production probe (the A/B harness lifts 64 to 8192).
		std::uint64_t maximumRetainedProbeSteps=64u;
		//! Called after every accepted step with the run's diagnostics and the
		//! accepted step count.  Observation only.
		std::function<void(const SolverFrameValues&,std::uint64_t)> acceptedStepObserver;
		//! Cooperative stop, polled once per accepted step: the step finishes,
		//! the final checkpoint (when finalCheckpointPath is set) is written and
		//! the run returns normally.
		const std::atomic<bool>* stopRequested=nullptr;
		//! Checkpoint request, polled (and cleared) once per accepted step: the
		//! current checkpoint (checkpointPath) is written at that step, as if
		//! the wall-time cadence had elapsed.
		std::atomic<bool>* checkpointRequested=nullptr;
		//! Test-only hooks; null in production.
		const FireRunTestHooks* testHooks=nullptr;
		const FireRunTestHooks& Test() const
		{
			static const FireRunTestHooks none;
			return testHooks?*testHooks:none;
		}
	};

	struct ResumeEquivalenceCertificate
	{
		std::uint64_t schemaVersion=1u;
		std::uint64_t beginningTimeBits=0u;
		std::string certificateId;
		std::string checkpointDigest;
		std::string oldBuildId,newBuildId;
		std::string oldExecutableDigest,newExecutableDigest;
		std::uint64_t resumedFromStep=0u,acceptedStepCount=0u;
		std::vector<std::uint64_t> timeStepBits,maximumTemperatureBits,
			maximumEOSResidualBits;
		std::vector<std::string> frameDigests;
		// v2 historical row expectations. Parsing alone is not native execution.
		std::vector<std::uint64_t> residentAcceptedTimeBits;
		std::vector<std::string> residentInputRoots,residentPublicationRoots,residentKernels;
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

	// Actual request-builder qualification seam: capture/refuse before any owner
	// reset or field copy. This partial family inventory never admits execution.
	struct ProductionCallerIngressProbe
	{
		FireProductionPreconstructionResourceSample source;
		std::array<std::uint64_t,8> ordinarySize{},ordinaryCapacity{};
		std::uint64_t ordinaryElementBytes=0u,ordinaryCapacityBytes=0u;
		std::uint64_t requestCopyElementLowerBoundBytes=0u;
		std::uint64_t copyElementLimitBytes=0u;
		FireRunHostMemory::Policy hostPolicy;
		bool ordinaryLayoutKnown=false,hostPolicyStop=false;
		unsigned int refusalSite=0u;
	};

	struct UniformHannSpectrum
	{
		std::vector<double> uniformTimeS,uniformSignal,detrendedSignal;
		std::vector<double> frequencyHz,amplitude,power,powerFraction;
		double affineIntercept=0.0,affineSlopePerSample=0.0;
		std::size_t dominantBin=0u;
	};

	constexpr const char* R216CheckpointSHA="65bb74aa8186a95acde57de593d54de664303c4d92ebaea374eea75e1f138c19";
	constexpr const char* R216ProtocolSHA="30196fdcfa1a17911e1200cf9a3eff1b805dff236c6529301054479f2ce2a28e";
	constexpr const char* R214ProducerBuild="0d7537194b877cdffd248de25eccf52262f824b4f1d4cde5fe035ad1eee52175";
	constexpr const char* R217CheckpointSHA="7eefb57bfd01f27817d0e245da8293c5ec85c770f0bf157556f20f906865e966";
	constexpr const char* R216ProducerBuild="81cf8340374423e2c5ae209062c35ed2e7d8a915535051324060c48a1d7cb5a9";
	constexpr const char* R222CheckpointSHA="e43cd54d2641190d7075738b2b74171da1ed718a1a892dd8c23530d1f702854c";
	constexpr const char* R225CheckpointSHA="98a8c138cdc644bfcbab12c223b2dcf9780b30c9c0218d7c53ff57ee1bb314de";
	constexpr const char* R225ProducerBuild="05bc1ed1635e977d3202e01b9ae81595ccf4cd8942065ce282ad758b94e1345d";
	constexpr const char* R226CheckpointSHA="d7c0632c7fccf71012f1bc618e9cc352d068068c7979cc3622eb17c47c5a9e9f";
	constexpr const char* R226ProducerBuild="461cd6c90adab0ded52c70175ff148ef31bc7ff55ac03762c875b8282cee76a9";
	constexpr double R226RefusalBeginningS=10.653518881154014;
	constexpr double R226RefusalStepS=2.1246003598207608e-5;

	constexpr double R225CaseDurationS=25.091040127156969;
	constexpr const char* R221ProducerBuild="bcd03e089a49d1117b693fcc165869e7cfbac32baf66d80a461a5b491028d4c6";
	constexpr const char* R220CheckpointSHA="4b5bfcbd02ec7c5f472352f739b1b39ee7157b871896efb8b2dc35e708bd8d64";
	constexpr const char* R220OldBuild="f2ee8ec4cdf43ed7c527dce95718439888561cb70861f99c22f3f5fd4a311609";
	constexpr const char* R220KernelSHA="e938373ec6cd9e0217c04242ad96cd4d46055236e422bd7ad0ea34e164d0a8b2";
	constexpr const char* R221CheckpointSHA="f18cac44b71b1fdac106d30d86bcc1efb955ed39ed4edc0a66be912a4130c337";
	constexpr const char* R221ProtocolSHA="983cfb4975a614a7bee2593c6d0f4536679925911afd0b22a5b2e897d6b04dc2";

	struct R245PendingBudget {
		FireProductionAcceptedStageBudget::Step step;
		std::vector<unsigned char> flux,native,beginning;
		std::string columns;
		std::uint64_t frame=0;
		bool ready=false,frameDue=false;
	};

	bool DiscontinuousThreadIdentityAccepted(const bool identical,
		const bool activeSetChecked,std::string& error);
	bool AppendOracleMomentumBudget(const std::filesystem::path& path,
		const PeriodicMACShape& shape,const PeriodicMACField& beginningMomentum,
		const ConservativeAdvance3DConfig& config,const ConservativeAdvance3DResult& advanced,
		const double beginningTimeS,std::string& error,
		const std::size_t columnX=38u,const std::size_t columnY=42u);
	bool RetainOracleMomentumOperands(const bool productionMetal,
		const bool composition,const bool legacyMomentumAudit);
	bool CarryCanonicalSourceForPersistence(
		const RISE::FireProductionFrozenSourcePacketSeal& source,
		std::vector<MethaneSourcePacket>& packets,RadiationEscapeFactor& escape,
		std::string& error);
	bool BindProductionBurnerBoundary(const OpenBoundaryConfig3D& boundary,
		RISE::FireProductionProjectedHeunMetalOwnerRequest& owner,std::string& error);
	double ProductionBurnerFuelDoseKG(const RISE::FireProductionProjectedHeunMetalOwnerRequest& owner);
	bool ProductionCandidateTemperatureAccepted(const std::vector<MethaneCellState>& candidate,
		const double ceilingK,std::string& error);
	bool RefuseProductionCallerIngressBeforeCopy(
		const FireProductionResidentStepRequest& ordinary,
		const FireProductionFrozenSourcePacketSeal& source,
		ProductionCallerIngressProbe& observed,std::string& error);
	bool BuildProductionProjectedHeunOwnerRequest(
		const RISE::FireProductionResidentStepRequest& ordinary,
		const RISE::FireProductionFrozenSourcePacketSeal& source,
		const RISE::RISECBOR64::Bytes& caseRecordEnvelope,
		const float projectionTolerancePerS,const float endpointVelocityToleranceMPerS,
		RISE::FireProductionProjectedHeunMetalOwnerRequest& owner,std::string& error,
		const OpenBoundaryConfig3D* burnerBoundary=nullptr,
		ProductionCallerIngressProbe* ingressProbe=nullptr);
	bool WriteProductionCrossingConvergence(const std::filesystem::path& auditPath,
		const RISE::FireProductionProjectedHeunMetalOwnerRequest& liveRequest,
		const RISE::FireProductionProjectedHeunMetalOwnerResult& accepted,
		const double beginningTimeS,const double representedStepS,const std::size_t acceptedStep,
		const std::size_t columnX,const std::size_t columnY,std::string& error,
		const bool syntheticFixture=false,const std::vector<double>& bottomSourcePattern={});
	void AppendReplayUInt64(RISECBOR64::Bytes& bytes,const std::uint64_t value);
	void AppendReplayDouble(RISECBOR64::Bytes& bytes,const double value);
	void AppendReplayFloat(RISECBOR64::Bytes& bytes,const float value);
	RISECBOR64::Bytes ProductionSourcePacketFieldBytes(
		const std::vector<MethaneSourcePacket>& packets);
	std::string ProductionSourcePacketFieldSHA256(
		const std::vector<MethaneSourcePacket>& packets);
	bool WriteProductionSourcePacketFieldEvidence(const std::filesystem::path& base,
		const std::vector<MethaneSourcePacket>& packets,
		const std::vector<MethaneCellState>& unstagedBeginning,
		const std::vector<MethaneCellState>& stagedBeginning,
		const PeriodicMACField& unstagedMomentum,
		const FireProductionResidentStepRequest& request,std::string& error);
	bool WriteProductionSourceAndAdjacentStateContext(const std::filesystem::path& path,
		const FireProductionProjectionShape& shape,const std::size_t centerX,
		const std::size_t centerY,const std::size_t centerZ,
		const std::vector<MethaneCellState>& beginning,
		const std::vector<MethaneSourcePacket>& packets,
		const std::vector<float>& acceptedConservative,
		const FireSimulationMethaneRecord& fuel,std::string& error);
	std::string ProductionBeginningFieldSHA256(const std::string& caseRecordId,
		const PeriodicMACShape& shape,const std::vector<MethaneCellState>& states,
		const PeriodicMACField& momentum,const PeriodicMACField& velocity);
	bool LimitBinary32ProductionStepToEvent(const double simulationTimeS,
		const double eventTimeS,double& timeStepS);
	bool ProductionSourceBoundaryContactCell(const std::size_t cell,const std::size_t nx,
		const std::size_t ny,const std::vector<std::uint8_t>& canonicalPilotMask,
		const std::vector<double>& sourcePattern);
	bool ProductionPilotCommandCell(const std::size_t cell,const std::size_t nx,const std::size_t ny,
		const std::vector<std::uint8_t>& canonicalPilotMask,
		const std::vector<double>& sourcePattern);
	bool ProductionEstablishedFlameHolderEligible(const std::size_t cell,
		const std::size_t nx,const std::size_t ny,
		const std::vector<std::uint8_t>& canonicalPilotMask,
		const std::vector<double>& sourcePattern,const bool pilotEstablished,
		const MethaneCellState& candidate,
		const FireSimulationMethaneRecord& fuel,
		const FireSimulationTransportRecord& transport,bool& eligible,std::string* error);
	bool ComputeUniformHannSpectrum( const std::vector<double>& time,
		const std::vector<double>& signal,const std::size_t sampleCount,
		UniformHannSpectrum& spectrum );
	double DominantUniformResampledFrequency( const std::vector<double>& time,
		const std::vector<double>& signal );
	void FitCenterlineTemperaturePowerLaw( SolverFrameValues& values );
	void EvaluateMcCaffreyPlumeStations( SolverFrameValues& values,
		const double heatReleaseRateKW );
	bool OwnerEOSDiagnosticSnapshotDirectoryAccepted(const FireRunPersistenceOptions& persistence);
	bool R226RecoveryEndpointAccepted(const std::size_t steps,const double dt,const double end);
	bool WindowRefusalDiagnosticInput(const std::string& digest,const std::string& build);
	bool R216KernelAccepted(const std::string& kernel);
	bool R216RetainedScopeAccepted(const double tier,const bool resume,
		const std::string& checkpointSHA,const bool continuation,const bool probe);
	bool R216AttemptPublicationAccepted(const bool scoped,const bool computed,const std::string& kernel);
	unsigned int FastModeConservationEvery();
	std::uint64_t& FastModeConservationCheckCount();
	bool FastModeConservationDue(const std::uint64_t acceptedSteps);
	bool FastModeConservationCheck(const FireProductionProjectedHeunMetalOwnerRequest& request,
		FireProductionProjectedHeunMetalOwnerResult& owner,const std::uint64_t stepIndex,
		const std::vector<double>& sourcePattern,std::string& error);
	bool PrepareR245Budget(const FireProductionProjectedHeunMetalOwnerRequest& request,
		FireProductionProjectedHeunMetalOwnerResult& owner,std::uint64_t stepIndex,
		const std::vector<double>& sourcePattern,bool frameDue,std::uint64_t frameIndex,
		R245PendingBudget& pending,std::string& error);
	void WriteR245Rows(std::ostream& stream,const char* scope,const FireProductionAcceptedStageBudget::Rows& rows);
	bool PublishR245Budget(const std::filesystem::path& directory,const std::string& caseId,
		const std::string& producer,const std::string& executable,double statisticsStart,
		const std::array<std::size_t,3>& dimensions,double solverDx,R245PendingBudget& pending,
		FireProductionAcceptedStageBudget::Ledger& ledger,std::string& error);
	bool ProductionOwnerObservationReserveBytes(const PeriodicMACShape& shape,
		const bool observe,std::uint64_t& bytes);
	bool R246PreallocationPolicy(const PeriodicMACShape& shape,FireRunHostMemory::Policy& policy,
		std::uint64_t& initialFields);
	unsigned int FireRunWorkerCount(const unsigned int defaultWorkers);
	double FireProductionSourceStepCapS(const double certifiedCapS);
	bool ReadTextMember(const RISECBOR64::Value& map,const char* key,std::string& value);
	bool ReadUnsignedMember(const RISECBOR64::Value& map,const char* key,std::uint64_t& value);
	bool ReadUnsignedArray(const RISECBOR64::Value& map,const char* key,
		std::vector<std::uint64_t>& values);
	bool ReadTextArray(const RISECBOR64::Value& map,const char* key,
		std::vector<std::string>& values);
	bool ParseResumeCertificatePayload(const RISECBOR64::Value& payload,
		ResumeEquivalenceCertificate& certificate);
	bool LoadResumeEquivalenceCertificate(const std::filesystem::path& path,
		ResumeEquivalenceCertificate& certificate,std::string& error);

	namespace ProductionStep
	{
		bool ResidentStepRequestsExactlyEqual(
			const RISE::FireProductionResidentStepRequest& a,
			const RISE::FireProductionResidentStepRequest& b);
		bool AmbientAndInjected(MethaneCellState& ambient,MethaneCellState& injected,
			std::string& error);
		bool BuildProductionRequest(const MethaneRunCheckpoint& state,
			const std::vector<double>& divergenceTarget,double timeStepS,
			RISE::FireProductionResidentStepRequest& request,std::string& error,
			unsigned int workerCount=1u,
			const std::vector<CellTransportEvaluation>* reusableTransport=nullptr,
			const std::vector<unsigned char>* invalidReusableTransport=nullptr);
		bool ApplyProductionResultUnchecked(const RISE::FireProductionResidentStepResult& production,
			MethaneRunCheckpoint& state,std::string& error,
			const bool enforceOracleEOSValidityDetector=true,
			const unsigned int temperatureWorkerCount=1u);
		bool ApplyAcceptedProductionResult(const RISE::FireProductionResidentStepResult& production,
			const RISE::FireProductionAcceptedManifoldObservation& acceptedObservation,
			MethaneRunCheckpoint& state,std::string& error,
			const unsigned int temperatureWorkerCount=1u);
	}

	//! The production driver (historically RunMethaneFrameProbe).  A zero
	//! minimumStepCount and targetTimeS derives the case and geometry only.
	SolverFrameValues RunFireMethaneDriver( const unsigned int workerCount,
		const unsigned int minimumStepCount,const double targetTimeS,
		const double caseDurationS,const double caseFramesPerS,
		const double resolutionTier,const double poolDiameterM,
		const double heatReleaseRateKW,
		const FireRunPersistenceOptions& persistence );

	// r211 resident resume adapter (formerly tests/fire_resident_resume_v2.h).
#include "fire_resident_resume_v2.h"
	}
}

#endif
