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
	namespace Implementation
	{
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
		};

		class FireSequenceManifest
		{
			bool valid_ = false;
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
			std::function<bool(const FireSequencePreparedFrame&)> frameInstaller_;
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
			bool SetActiveBindingIdentity(
				const std::string& bindingIdentity,
				std::string& error );
			bool PrepareMediaForRender(
				const FireSequenceRenderTimeSupport& support,
				bool effectiveVelocityBlur,
				std::string& error );
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
