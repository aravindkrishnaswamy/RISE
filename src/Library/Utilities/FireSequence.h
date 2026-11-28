//////////////////////////////////////////////////////////////////////
//
//  FireSequence.h - Canonical Phase-C fire sequence contract
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FIRE_SEQUENCE_
#define FIRE_SEQUENCE_

#include "RISECBOR64.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace RISE
{
	namespace Simulation { class CacheSnapshot; struct GridFrame; }
	namespace Implementation
	{
		// The fire solver's native cache (simulation M2 track C): producer and
		// version of a cache written by the registered fire solver, and the
		// format of its per-frame receipt.  Its physical records are a sequence
		// envelope without a frame list (LoadFrameTemplate); fire_medium reads
		// such a cache directly (AttachCache).
		constexpr const char* FireMethaneSolverId = "fire.methane";
		// Version 3 (docs/SIMULATION_SOLVER_CONTRACT.md section 13): the pre-pilot rim-ignition
		// witness is a product fraction of 1e-6 (FireCase::RimIgnitionProductWitness), so
		// every fire key changed.  Caches of versions 1 and 2 use the same grid and receipt
		// formats: stale for baking (the version is in the key), still playable.
		// Version 4 (docs/FIRE_SMOKE_STATUS.md, "Fire solver version 4"): the fire tables, records,
		// optics, sequence and schema sources compile with strict IEEE arithmetic instead
		// of fast-math (docs/BUILD_FLAGS.md), so their bytes no longer depend on the LTO
		// mode; the solver's results move by rounding.  Version 3 caches stay playable.
		constexpr const char* FireMethaneSolverVersion = "4";
		inline bool FireMethaneSolverCacheVersionPlayable(const std::string& version)
			{ return version=="1" || version=="2" || version=="3" || version==FireMethaneSolverVersion; }
		constexpr const char* FireMethaneFrameReceiptFormat = "rise.fire.methane.frame.v1";

		bool CurrentRendererBuildIdentity(
			RISECBOR64::Bytes& bytes,std::string& identity );
		// OpenVDB writes a random archive UUID even when every grid byte is
		// identical.  Sequence frames use a content-derived UUID so r54's
		// cross-thread whole-file digest requirement is meaningful.
		bool CanonicalizeOpenVDBFileIdentity(
			const std::string& path,
			std::string& error );

		struct FireSequenceTimeMap
		{
			double simulationTimeOrigin = 0.0;
			double sceneToSimulationScale = 0.0;
			double sceneTimeOrigin = 0.0;
			double frameStepSeconds = 0.0;
			std::int64_t firstFrameIndex = 0;
		};

		struct FireSequenceChannelDescriptor
		{
			std::string name;
			std::string valueType;
			std::string units;
			std::string temporalSemantics;
			std::array<std::uint64_t,3> dimensions{{0,0,0}};
			std::array<double,3> originMeters{{0,0,0}};
			std::array<double,3> voxelSizeMeters{{0,0,0}};
			std::array<double,6> coreFaceBoundsMeters{{0,0,0,0,0,0}};
			std::vector<double> background;
		};

		struct FireSequenceFrameDescriptor
		{
			std::int64_t index = 0;
			std::string relativePath;
			std::string sha256;
		};

		struct FireSequenceMappedTime
		{
			std::int64_t baseFrameIndex = 0;
			double simulationTime = 0.0;
			double baseFrameTime = 0.0;
			double advectionOffsetSeconds = 0.0;
			bool held = false;
		};

		struct FireSequenceDenseChannel
		{
			std::string name;
			bool vectorValues = false;
			std::array<std::uint64_t,3> dimensions{{0,0,0}};
			std::vector<double> values;
			double minimum = 0.0;
			double maximum = 0.0;
		};

		struct FireSequencePreparedFrame
		{
			std::int64_t frameIndex = 0;
			std::string wholeFileSha256;
			std::map<std::string,FireSequenceDenseChannel> channels;
			bool available = true;
			std::string coverageDigest;
		};

		class FireSequenceManifest
		{
			bool valid_ = false;
			std::shared_ptr<const Simulation::CacheSnapshot> native_;
			std::string nativeDirectory_, originalManifestPath_, envelopeDigest_, qualifiedGeneration_;
			bool needsImport_ = false;

			bool ResolveImportedFrame(std::int64_t index,std::string& error);
			bool AttachCache(const std::string& directory, std::string& error);
			bool LoadNativeFrame(std::int64_t frameIndex, FireSequencePreparedFrame& frame,
				std::string& error) const;
			bool ReadVDBFrame(std::int64_t frameIndex, FireSequencePreparedFrame* dense,
				Simulation::GridFrame* grid, std::string& error) const;
			bool ReadVDBPath(const std::string& path, const std::string& sha256, std::int64_t frameIndex,
				FireSequencePreparedFrame* dense, Simulation::GridFrame* grid, std::string& error) const;
			bool LoadEnvelope(const RISECBOR64::Bytes& envelope, const std::string& manifestDirectory,
				bool frameTemplate, std::string& error);
			bool AttachSolverCache(const std::shared_ptr<Simulation::CacheSnapshot>& snapshot,
				const std::string& directory, std::string& error);
			bool frameTemplate_ = false;
			std::string sequenceId_;
			std::string manifestDirectory_;
			std::string sourceKind_;
			std::string physicalMapping_;
			std::string sourceQualification_;
			std::string caseRecordId_;
			double referenceHeatReleaseRateW_ = 0.0;
			std::string producerBuildId_;
			std::vector<std::string> gateEvidenceIds_;
			FireSequenceTimeMap timeMap_;
			std::string endPolicy_;
			double velocityHaloWidthMeters_ = 0.0;
			std::string outsideHaloPolicy_;
			double temperatureDomainMinimumK_ = 0.0;
			double temperatureDomainMaximumK_ = 0.0;
			std::vector<FireSequenceChannelDescriptor> channels_;
			std::vector<FireSequenceFrameDescriptor> frames_;
			double sceneUnitMeters_ = 0.0;
			std::array<double,3> sceneTranslation_{{0,0,0}};
			RISECBOR64::Bytes opticalRecord_;
			std::map<std::string,RISECBOR64::Bytes> embeddedRecords_;
			RISECBOR64::Bytes canonicalPayload_;
			bool hasChemChannels_ = false;
			std::array<std::array<double,2>,3> chemNormalizationIntervalsNM_{{
				{{0.0,0.0}},{{0.0,0.0}},{{0.0,0.0}} }};

		public:
			bool LoadCanonicalEnvelope(
				const RISECBOR64::Bytes& envelope,
				const std::string& manifestDirectory,
				std::string& error );
			bool LoadPlaybackFile(const std::string& path, std::string& error);
			//! A sequence envelope without frames ("frames", "frame_count",
			//! "first_frame_index", "last_frame_index" absent), every other field
			//! as LoadCanonicalEnvelope checks it.  The result is not playable
			//! (IsValid() stays false) but exposes the records, channels and time
			//! map, and converts VDB files with LoadVDBFileGrid.
			bool LoadFrameTemplate(const RISECBOR64::Bytes& envelope, std::string& error);
			bool IsFrameTemplate() const { return frameTemplate_; }
			//! One OpenVDB frame file with this sequence's channels, converted
			//! exactly as the legacy import converts a manifest frame.
			bool LoadVDBFileGrid(const std::string& path, const std::string& sha256,
				Simulation::GridFrame& grid, std::string& error) const;
			bool IsNativeCache() const { return static_cast<bool>(native_); }
			bool IsComplete() const;
			bool PreflightPredictive(std::string& error);
			void ReleasePredictiveGeneration() { qualifiedGeneration_.clear(); }
			bool AcquirePlaybackSnapshot(std::int64_t index, FireSequenceManifest& snapshot,
				std::string& error) const;
			const std::string& CoverageDigest() const;
			bool LoadSourceGrid(std::int64_t index, Simulation::GridFrame& grid, std::string& error) const;
			bool MapSceneTime(
				double sceneTime,
				FireSequenceMappedTime& mapped,
				std::string& error ) const;
			bool LoadFrame(
				std::int64_t frameIndex,
				FireSequencePreparedFrame& frame,
				std::string& error ) const;
			bool PreflightAllFrames( std::string& error ) const;

			bool IsValid() const { return valid_; }
			const std::string& SequenceId() const { return sequenceId_; }
			const std::string& CaseRecordId() const { return caseRecordId_; }
			double ReferenceHeatReleaseRateW() const { return referenceHeatReleaseRateW_; }
			const std::string& SourceKind() const { return sourceKind_; }
			const std::string& PhysicalMapping() const { return physicalMapping_; }
			const std::string& SourceQualification() const { return sourceQualification_; }
			const std::string& ProducerBuildId() const { return producerBuildId_; }
			const std::vector<std::string>& GateEvidenceIds() const { return gateEvidenceIds_; }
			const std::vector<FireSequenceFrameDescriptor>& Frames() const { return frames_; }
			const std::vector<FireSequenceChannelDescriptor>& Channels() const { return channels_; }
			double SceneUnitMeters() const { return sceneUnitMeters_; }
			const std::array<double,3>& SceneTranslation() const { return sceneTranslation_; }
			const RISECBOR64::Bytes& OpticalRecord() const { return opticalRecord_; }
			const RISECBOR64::Bytes* EmbeddedRecord( const std::string& name ) const
			{
				const auto found = embeddedRecords_.find(name);
				return found == embeddedRecords_.end() ? nullptr : &found->second;
			}
			const std::string& EndPolicy() const { return endPolicy_; }
			double VelocityHaloWidthMeters() const { return velocityHaloWidthMeters_; }
			double TemperatureDomainMinimumK() const { return temperatureDomainMinimumK_; }
			double TemperatureDomainMaximumK() const { return temperatureDomainMaximumK_; }
			const std::string& OutsideHaloPolicy() const { return outsideHaloPolicy_; }
			const FireSequenceTimeMap& TimeMap() const { return timeMap_; }
			bool HasChemChannels() const { return hasChemChannels_; }
			const std::array<std::array<double,2>,3>& ChemNormalizationIntervalsNM() const
			{
				return chemNormalizationIntervalsNM_;
			}
		};

		struct FireSequenceRenderTimeSupport
		{
			double nominalSceneTime = 0.0;
			double shutterOpenSceneTime = 0.0;
			double shutterCloseSceneTime = 0.0;
		};

		//! One transactional owner for a sequence's immutable frame state.  Prepare
		//! is the only swap path; a render lease prevents swaps and verifies that
		//! the generation and prepared-input identity remain stable until release.
		class FireSequencePreparationController
		{
		public:
			class RenderLease
			{
				friend class FireSequencePreparationController;
				FireSequencePreparationController* owner_ = nullptr;
				std::uint64_t generation_ = 0;
				std::string preparedInputId_;
				explicit RenderLease( FireSequencePreparationController& owner );
			public:
				RenderLease() = default;
				RenderLease( RenderLease&& other ) noexcept;
				RenderLease& operator=( RenderLease&& other ) noexcept;
				~RenderLease();
				RenderLease( const RenderLease& ) = delete;
				RenderLease& operator=( const RenderLease& ) = delete;
				bool IsValid() const { return owner_ != nullptr; }
				bool StateStayedFrozen() const;
			};

		private:
			const FireSequenceManifest* manifest_ = nullptr;
			mutable std::mutex mutex_;
			std::shared_ptr<const FireSequencePreparedFrame> frame_;
			std::string preparedInputId_;
			std::uint64_t generation_ = 0;
			std::uint64_t activeRenderLeases_ = 0;
			std::uint64_t majorantGeneration_ = 0;
			std::uint64_t emissionCDFGeneration_ = 0;
			using ReversibleInstaller = std::function<bool(const FireSequencePreparedFrame&,std::function<void()>*)>;
			ReversibleInstaller frameInstaller_;
			std::string frameInstallerIdentity_;
			std::string activeBindingIdentity_;

			void ReleaseLease();
		public:
			explicit FireSequencePreparationController(
				const FireSequenceManifest& manifest ) : manifest_(&manifest) {}
			bool SetFrameInstaller(
				const std::function<bool(const FireSequencePreparedFrame&)>& installer,
				const std::string& installerIdentity,
				std::string& error );
			bool SetReversibleFrameInstaller(const ReversibleInstaller& installer,
				const std::string& installerIdentity, std::string& error);
			bool SetActiveBindingIdentity(
				const std::string& bindingIdentity,
				std::string& error );
			bool PrepareMediaForRender(
				const FireSequenceRenderTimeSupport& support,
				bool effectiveVelocityBlur,
				std::string& error, std::function<void()>* rollback = nullptr );
			RenderLease AcquireRenderLease( std::string& error );
			std::shared_ptr<const FireSequencePreparedFrame> PreparedFrame() const;
			std::string PreparedInputId() const;
			std::uint64_t Generation() const;
			std::uint64_t MajorantGeneration() const;
			std::uint64_t EmissionCDFGeneration() const;
			bool HasActiveRenderLease() const;
		};
	}
}

#endif
