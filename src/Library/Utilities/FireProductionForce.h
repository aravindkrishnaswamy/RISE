//////////////////////////////////////////////////////////////////////
//
//  FireProductionForce.h - strict-binary32 production force primitives
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FIRE_PRODUCTION_FORCE_H
#define FIRE_PRODUCTION_FORCE_H

#include "FireProductionProjection.h"
#include "FireProductionTransport.h"
#include "FireSimulationRecords.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace RISE
{
	//! Shared bound for fail-closed step reductions.  The binary64 trajectory
	//! owner and the production manifold owner use the same rejection budget.
	constexpr unsigned int FireStepRejectionRetryCap=20u;

	struct FireProductionVremanInput
	{
		std::array<float,9> velocityGradientPerS;
		std::array<float,3> directionalWidthsM;
		float coefficient;

		FireProductionVremanInput() : coefficient(0.07f)
		{
			velocityGradientPerS.fill(0.0f);
			directionalWidthsM.fill(1.0f);
		}
	};

	//! Strict-binary32 transcription of the record Vreman expression. The
	//! caller supplies the coefficient obtained from the certified record.
	bool EvaluateFireProductionVremanEddyViscosity(
		const FireProductionVremanInput& input,
		float& eddyViscosityM2PerS,
		std::string* error=0 );

	struct FireProductionViscousSchedule
	{
		std::uint32_t substepCount;
		float substepTimeS;
		double outwardWork;
		double representedProductUpper;

		FireProductionViscousSchedule() : substepCount(0u),substepTimeS(0.0f),
			outwardWork(0.0),representedProductUpper(0.0) {}
	};

	//! Host half of the r90/r91 outward stability certificate. Both inputs are
	//! stored fp32 bytes; selection certifies the represented fp32 dt/N and
	//! rejects a required ninth substep before any physical update.
	bool SelectFireProductionViscousSchedule(
		float timeStepS,
		float outwardLambdaPerS,
		FireProductionViscousSchedule& schedule,
		std::string* error=0 );

	struct FireProductionFrozenForceRequest
	{
		FireProductionProjectionShape shape;
		float timeStepS;
		float ambientDensityKGPerM3;
		float vremanCoefficient;
		std::array<float,3> gravityMPerS2;
		std::array<FireProductionProjectionBoundary,6> boundary;
		std::vector<float> cellGasDensityKGPerM3;
		std::vector<float> molecularKinematicViscosityM2PerS;
		std::array<std::vector<float>,3> faceDensityKGPerM3;
		std::array<std::vector<float>,3> beginningMomentumKGPerM2S;

		FireProductionFrozenForceRequest() : timeStepS(0.0f),ambientDensityKGPerM3(1.0f),
			vremanCoefficient(0.07f)
		{
			gravityMPerS2.fill(0.0f);
			boundary.fill(FireProductionProjectionPeriodic);
		}
	};

	struct FireProductionFrozenForceResult
	{
		std::vector<float> eddyKinematicViscosityM2PerS;
		std::vector<float> effectiveDynamicViscosityPaS;
		std::array<std::vector<float>,3> beginningViscousMomentumRateKGPerM2S2;
		std::array<std::vector<float>,3> gravityMomentumRateKGPerM2S2;
		std::array<std::vector<float>,3> gravityMomentumIncrementKGPerM2S;
	};

	//! Logical peak of the CPU frozen-force comparator, including caller input,
	//! local work, and the atomically published result payload.
	bool FireProductionFrozenForceWorkingSetBytes(
		const FireProductionProjectionShape& shape,
		std::uint64_t& bytes );

	//! Complete standalone Metal-wrapper peak: live caller/result arrays plus
	//! every MTLBuffer rounded outward to the M4 allocation quantum.
	bool FireProductionFrozenForceMetalWorkingSetBytes(
		const FireProductionProjectionShape& shape,
		std::uint64_t& bytes );

	bool FireProductionResidentForceMetalWorkingSetBytes(
		const FireProductionProjectionShape& shape,
		bool captureIntermediateStates,
		std::uint64_t& bytes );

	//! Conservative force-owned plus resident-P2 peak, including the two target
	//! buffers retained by the composed owner. Used before any Metal allocation.
	bool FireProductionResidentForceProjectionWorkingSetBytes(
		const FireProductionProjectionShape& shape,
		std::uint64_t& bytes );

	bool FireProductionFrozenForceAdvanceWorkingSetBytes(
		const FireProductionProjectionShape& shape,
		std::uint64_t& bytes );

	//! Complete full resident-step certificate. This query is shape/boundary
	//! only so the owner can reject the two-GiB limit before inspecting payloads.
	bool FireProductionResidentStepWorkingSetBytes(
		const FireProductionProjectionShape& shape,
		const std::array<FireProductionProjectionBoundary,6>& boundary,
		std::uint64_t& bytes );

	//! Allocation-free structural and payload preflight shared by every frozen-
	//! force owner. It performs no physical work and publishes no partial state.
	bool ValidateFireProductionFrozenForceRequest(
		const FireProductionFrozenForceRequest& request,
		std::string* error=0 );

	//! Beginning-state, strict-binary32 force field used by the production P3
	//! comparator. Vreman and mu are frozen; viscous rates update only interior
	//! normal faces, relative gravity also owns pressure-open endpoint faces,
	//! and wall-normal endpoints remain prescribed zero.
	bool BuildFireProductionFrozenForceFieldsCPU(
		const FireProductionFrozenForceRequest& request,
		FireProductionFrozenForceResult& result,
		std::string* error=0 );

	//! Instantaneous nonpressure momentum owner for one projected stage state.
	//! The gas-phase source is a cell rate, not a step increment.  Every
	//! published momentum quantity is consequently a rate and is independent of
	//! request.force.timeStepS; the retained time step only serves the legacy
	//! frozen-force increment returned by the shared field builder.
	struct FireProductionNonpressureMomentumRHSRequest
	{
		FireProductionFrozenForceRequest force;
		std::vector<float> cellGasPhaseSourceRateKGPerM3S;
	};

	struct FireProductionNonpressureMomentumRHSResult
	{
		std::vector<float> eddyKinematicViscosityM2PerS;
		std::vector<float> effectiveDynamicViscosityPaS;
		std::array<std::vector<float>,3> buoyancyMomentumRateKGPerM2S2;
		std::array<std::vector<float>,3> stressMomentumRateKGPerM2S2;
		std::array<std::vector<float>,3> phaseSourceMomentumRateKGPerM2S2;
		std::array<std::vector<float>,3> combinedMomentumRateKGPerM2S2;
	};

	//! Evaluate buoyancy, Vreman stress divergence, and compatible phase-source
	//! momentum at the supplied stage state without advancing momentum.
	bool EvaluateFireProductionNonpressureMomentumRHSCPU(
		const FireProductionNonpressureMomentumRHSRequest& request,
		FireProductionNonpressureMomentumRHSResult& result,
		std::string* error=0 );

	//! Ordered stages of the complete Section 3.7 projected-Heun owner.  These
	//! values are identity-bearing protocol ordinals, not interchangeable labels.
	enum class FireProductionProjectedHeunStage : std::uint8_t
	{
		R0=1u,
		R1=2u,
		R2=3u
	};

	struct FireProductionProjectedHeunTransportContext
	{
		FireProductionProjectedHeunStage stage;
		std::uint64_t attemptIdentity;
		std::uint64_t parentCandidateIdentity;
		std::uint64_t projectionIdentity;
		const std::vector<float>* conservativeValues;
		const std::vector<float>* temperatureK;
		const std::array<std::vector<float>,3>* projectedVelocityMPerS;

		FireProductionProjectedHeunTransportContext() :
			stage(static_cast<FireProductionProjectedHeunStage>(0u)),attemptIdentity(0u),
			parentCandidateIdentity(0u),projectionIdentity(0u),conservativeValues(0),temperatureK(0),
			projectedVelocityMPerS(0) {}
	};

	//! Stage transport publication.  The echoed protocol lineage is checked
	//! before any coefficient can enter f_N, A-hat, or the next Picard iterate.
	struct FireProductionProjectedHeunTransportCoefficients
	{
		FireProductionProjectedHeunStage stage;
		std::uint64_t attemptIdentity;
		std::uint64_t parentCandidateIdentity;
		std::uint64_t projectionIdentity;
		std::uint64_t publicationIdentity;
		std::vector<float> diffusivityM2PerS;
		std::vector<float> conductivityWPerMK;
		std::vector<float> molecularKinematicViscosityM2PerS;

		FireProductionProjectedHeunTransportCoefficients() :
			stage(static_cast<FireProductionProjectedHeunStage>(0u)),attemptIdentity(0u),
			parentCandidateIdentity(0u),projectionIdentity(0u),publicationIdentity(0u) {}
	};

	//! Identity of the exact projected state, coefficient payload, and protocol
	//! parents. A provider that replays stale bytes cannot retain a current seal.
	std::uint64_t FireProductionProjectedHeunTransportPublicationIdentity(
		const FireProductionProjectedHeunTransportContext& context,
		const FireProductionProjectedHeunTransportCoefficients& result );

	class FireProductionProjectedHeunTransportProvider
	{
	public:
		virtual ~FireProductionProjectedHeunTransportProvider() {}
		virtual bool Evaluate(
			const FireProductionProjectedHeunTransportContext& context,
			FireProductionProjectedHeunTransportCoefficients& result,
			std::string* error=0 ) const=0;
	};

	//! Raw step inputs plus invariant scalar/force contracts.  State-, velocity-,
	//! target-, classification-, coefficient-, and force-owned arrays in the
	//! template requests must be empty: the owner fills them from the immediately
	//! preceding authenticated publication.
	struct FireProductionProjectedHeunOwnerRequest
	{
		std::uint64_t attemptIdentity;
		FireProductionFrozenSourcePacketSeal source;
		RISE::RISECBOR64::Bytes caseRecordEnvelope;
		std::vector<float> beginningConservativeValues;
		std::array<std::vector<float>,3> beginningMomentumKGPerM2S;
		FireProductionScalarFCTRequest scalarContract;
		FireProductionScalarPhysicalFluxPrerequisiteRequest physicalContract;
		FireProductionFrozenForceRequest forceContract;
		float projectionTolerancePerS;
		float endpointVelocityToleranceMPerS;
		std::uint32_t maximumPicardIterations;

		FireProductionProjectedHeunOwnerRequest() : attemptIdentity(0u),
			projectionTolerancePerS(0.0f),endpointVelocityToleranceMPerS(0.0f),
			maximumPicardIterations(64u) {}
	};

	struct FireProductionProjectedHeunCoupledStageResult
	{
		FireProductionProjectedHeunStage stage;
		FireProductionProjectionResult projection;
		FireProductionScalarHeunFluxStage flux;
		FireProductionScalarPhysicalFluxPrerequisiteResult endpointPhysicalFlux;
		FireProductionScalarFCTResult scalarAcceptance;
		FireProductionNonpressureMomentumRHSResult nonpressure;
		FireProductionScalarProjectionTargetSeal target;
		std::vector<float> picardResidualPerS;
		std::uint64_t parentCandidateIdentity;
		std::uint64_t acceptedCandidateIdentity;
		std::uint32_t acceptedIterationCount;
		std::uint32_t activeSetCycleLength;
		std::uint32_t activeSetDifferingFaceCount;
		std::uint32_t activeSetCanonicalProjectionCount;
		float maximumActiveSetComplementarityDiscrepancyMPerS;
		float maximumLimiterClassDiscrepancy;
		bool activeSetDiscontinuousClass;
		bool limiterDiscontinuousClass;

		FireProductionProjectedHeunCoupledStageResult() :
			stage(static_cast<FireProductionProjectedHeunStage>(0u)),
			parentCandidateIdentity(0u),acceptedCandidateIdentity(0u),
			acceptedIterationCount(0u),activeSetCycleLength(0u),
			activeSetDifferingFaceCount(0u),activeSetCanonicalProjectionCount(0u),
			maximumActiveSetComplementarityDiscrepancyMPerS(0.0f),
			maximumLimiterClassDiscrepancy(0.0f),activeSetDiscontinuousClass(false),
			limiterDiscontinuousClass(false) {}
	};

	//! Complete simultaneous host live-set certificate for the staged owner.
	bool FireProductionProjectedHeunCPUOwnerWorkingSetBytes(
		const FireProductionProjectionShape& shape,std::uint64_t& bytes );

	//! r60 permits rounding-scale negative constituent rows.  Gas density is
	//! therefore the signed sum of the six represented gas rows; per-row clamps
	//! would silently change the momentum/projection operand.
	bool ComputeFireProductionGasDensityCPU(
		const FireProductionProjectionShape& shape,
		const std::vector<float>& conservativeValues,
		std::vector<float>& gasDensityKGPerM3,
		std::string* error=0 );

	struct FireProductionProjectedHeunOwnerResult
	{
		std::vector<float> conservativeValues;
		std::array<std::vector<float>,3> momentumKGPerM2S;
		std::array<std::vector<float>,3> velocityMPerS;
		std::vector<float> stepAveragePressurePa;
		FireProductionScalarEOSAcceptanceResult predictorEOS;
		FireProductionScalarEOSAcceptanceResult committedEOS;
		FireProductionScalarHeunFluxStage averagedFlux;
		FireProductionScalarHeunSolveResult heunSolve;
		FireProductionProjectedHeunCoupledStageResult r0,r1,r2;
		std::uint64_t sourcePacketIdentity;
		bool accepted;

		FireProductionProjectedHeunOwnerResult() : sourcePacketIdentity(0u),
			accepted(false),ownerIdentity_(0u) {}

		//! Opaque acceptance seal minted only by the complete owner.  Publication
		//! payloads remain inspectable, but a client cannot re-sign mutated bytes.
		std::uint64_t OwnerIdentity() const { return ownerIdentity_; }

	private:
		std::uint64_t ownerIdentity_;
		friend class FireProductionProjectedHeunCPUOwner;
		friend bool FireProductionProjectedHeunOwnerResultMatches(
			const FireProductionProjectedHeunOwnerResult&,
			const FireProductionFrozenSourcePacketSeal& );
	};

	//! Recomputes the complete publication identity against the exact source
	//! packet expected by the current attempt. The diagnostic struct alone
	//! carries no acceptance power, and a prior attempt cannot be replayed.
	bool FireProductionProjectedHeunOwnerResultMatches(
		const FireProductionProjectedHeunOwnerResult& result,
		const FireProductionFrozenSourcePacketSeal& expectedSource );

	bool FireProductionNonpressureMomentumRHSWorkingSetBytes(
		const FireProductionProjectionShape& shape,
		std::uint64_t& bytes );

	struct FireProductionNonpressureMomentumRHSMetalDiagnostics
	{
		std::uint32_t commandCommitCount;
		std::uint32_t interstageFullGridTransferCount;
		std::uint32_t terminalStagingCount;
		std::uint64_t certifiedWorkingSetBytes;
		std::uint64_t actualMetalAllocationBytes;
		double deviceElapsedMS;

		FireProductionNonpressureMomentumRHSMetalDiagnostics() : commandCommitCount(0u),
			interstageFullGridTransferCount(0u),terminalStagingCount(0u),
			certifiedWorkingSetBytes(0u),actualMetalAllocationBytes(0u),deviceElapsedMS(0.0) {}
	};

	bool FireProductionNonpressureMomentumRHSMetalWorkingSetBytes(
		const FireProductionProjectionShape& shape,
		std::uint64_t& bytes );

	//! One-command Private-buffer stage evaluator.  Inputs upload before the
	//! first kernel and the six published rate/diagnostic fields stage only after
	//! every dependent kernel, so no full-grid host transfer can occur between
	//! setup, Vreman stress, face force, and phase-source composition.
	bool EvaluateFireProductionNonpressureMomentumRHSMetal(
		const FireProductionNonpressureMomentumRHSRequest& request,
		FireProductionNonpressureMomentumRHSResult& result,
		FireProductionNonpressureMomentumRHSMetalDiagnostics& diagnostics,
		std::string* error=0 );

	//! Standalone Metal comparison wrapper for the same beginning-state frozen
	//! fields. Platforms without Metal fail honestly; no CPU fallback is hidden
	//! behind this entry point. The later resident P3 seam reuses these kernels.
	bool BuildFireProductionFrozenForceFieldsMetal(
		const FireProductionFrozenForceRequest& request,
		FireProductionFrozenForceResult& result,
		double& deviceElapsedMS,
		std::string* error=0 );

	struct FireProductionFrozenForceAdvanceResult
	{
		FireProductionFrozenForceResult frozenFields;
		FireProductionViscousSchedule schedule;
		std::array<std::vector<float>,3> momentumKGPerM2S;
		std::array<std::uint64_t,8> intermediateMomentumByteDigests;
		std::uint32_t intermediateMomentumDigestCount;
		std::uint32_t executedViscousSubstepCount;

		FireProductionFrozenForceAdvanceResult() :
			intermediateMomentumDigestCount(0u),executedViscousSubstepCount(0u)
		{
			intermediateMomentumByteDigests.fill(0u);
		}
	};

	//! Strict-binary32 force comparator after the resident preflight has supplied
	//! its outward Lambda scalar. mu_eff and face density stay frozen through all
	//! viscous substeps; relative gravity is added exactly once afterward.
	bool AdvanceFireProductionFrozenForceCPU(
		const FireProductionFrozenForceRequest& request,
		float outwardLambdaPerS,
		FireProductionFrozenForceAdvanceResult& result,
		std::string* error=0 );

	struct FireProductionResidentForceDiagnostics
	{
		float outwardLambdaPerS;
		std::uint32_t scalarDiagnosticTransferCount;
		std::uint32_t substepLoopDeviceToHostTransferCount;
		std::uint32_t terminalStagingCount;
		std::uint32_t commandCommitCount;
		std::uint64_t certifiedWorkingSetBytes;
		std::uint64_t actualMetalAllocationBytes;
		double preflightDeviceElapsedMS;
		double advanceDeviceElapsedMS;
		double deviceStartTimeS;
		double deviceEndTimeS;

		FireProductionResidentForceDiagnostics() : outwardLambdaPerS(0.0f),
			scalarDiagnosticTransferCount(0u),substepLoopDeviceToHostTransferCount(0u),
			terminalStagingCount(0u),commandCommitCount(0u),certifiedWorkingSetBytes(0u),
			actualMetalAllocationBytes(0u),preflightDeviceElapsedMS(0.0),
			advanceDeviceElapsedMS(0.0),deviceStartTimeS(0.0),deviceEndTimeS(0.0) {}
	};

	//! Standalone oracle wrapper around the resident force sequence. Full-grid
	//! resources remain Private through every viscous substep and the gravity
	//! update. Optional Private snapshots stage only after command completion.
	bool AdvanceFireProductionFrozenForceMetal(
		const FireProductionFrozenForceRequest& request,
		bool captureIntermediateStates,
		FireProductionFrozenForceAdvanceResult& result,
		FireProductionResidentForceDiagnostics& diagnostics,
		std::string* error=0 );

	//! Oracle wrapper for the no-staging resident-state branch. It performs one
	//! terminal momentum tap after the resident call and leaves the force
	//! diagnostics' terminal-staging count at zero for that measured interval.
	bool AdvanceFireProductionFrozenForceMetalResidentStateComparator(
		const FireProductionFrozenForceRequest& request,
		FireProductionFrozenForceAdvanceResult& result,
		FireProductionResidentForceDiagnostics& diagnostics,
		std::string* error=0 );

#if defined(__OBJC__) && defined(__APPLE__)
	struct FireProductionMetalNonpressureMomentumRHSResidentInput
	{
		FireProductionProjectionShape shape;
		float ambientDensityKGPerM3;
		float vremanCoefficient;
		std::array<float,3> gravityMPerS2;
		std::array<FireProductionProjectionBoundary,6> boundary;
		id<MTLCommandBuffer> commandBuffer;
		id<MTLBuffer> cellGasDensityKGPerM3;
		id<MTLBuffer> molecularKinematicViscosityM2PerS;
		id<MTLBuffer> cellGasPhaseSourceRateKGPerM3S;
		id<MTLBuffer> packedFaceDensityKGPerM3;
		id<MTLBuffer> packedMomentumKGPerM2S;

		FireProductionMetalNonpressureMomentumRHSResidentInput() :
			ambientDensityKGPerM3(1.0f),vremanCoefficient(0.07f),commandBuffer(nil),
			cellGasDensityKGPerM3(nil),molecularKinematicViscosityM2PerS(nil),
			cellGasPhaseSourceRateKGPerM3S(nil),packedFaceDensityKGPerM3(nil),
			packedMomentumKGPerM2S(nil)
		{
			gravityMPerS2.fill(0.0f);boundary.fill(FireProductionProjectionPeriodic);
		}
	};

	struct FireProductionMetalNonpressureMomentumRHSResidentResult
	{
		id<MTLBuffer> eddyKinematicViscosityM2PerS;
		id<MTLBuffer> effectiveDynamicViscosityPaS;
		id<MTLBuffer> buoyancyMomentumRateKGPerM2S2;
		id<MTLBuffer> stressMomentumRateKGPerM2S2;
		id<MTLBuffer> phaseSourceMomentumRateKGPerM2S2;
		id<MTLBuffer> combinedMomentumRateKGPerM2S2;
		// Command-lifetime owners for the intermediates consumed by the rates.
		id<MTLBuffer> faceVelocityMPerS;
		id<MTLBuffer> cellVelocityMPerS;
		id<MTLBuffer> stressTensorPa;
		id<MTLBuffer> parameterBuffer;
		std::uint64_t privateAllocationBytes;

		FireProductionMetalNonpressureMomentumRHSResidentResult() :
			eddyKinematicViscosityM2PerS(nil),effectiveDynamicViscosityPaS(nil),
			buoyancyMomentumRateKGPerM2S2(nil),stressMomentumRateKGPerM2S2(nil),
			phaseSourceMomentumRateKGPerM2S2(nil),combinedMomentumRateKGPerM2S2(nil),
			faceVelocityMPerS(nil),cellVelocityMPerS(nil),stressTensorPa(nil),
			parameterBuffer(nil),
			privateAllocationBytes(0u) {}
	};

	//! Encode the complete stage RHS into a caller-owned command buffer.  Every
	//! input and output is Private; this call neither commits nor stages.
	bool EvaluateFireProductionNonpressureMomentumRHSMetalResident(
		const FireProductionMetalNonpressureMomentumRHSResidentInput& input,
		FireProductionMetalNonpressureMomentumRHSResidentResult& result,
		FireProductionNonpressureMomentumRHSMetalDiagnostics& diagnostics,
		std::string* error=0 );

	struct FireProductionMetalFrozenForceResidentState
	{
		id<MTLBuffer> cellGasDensityKGPerM3;
		id<MTLBuffer> packedFaceDensityKGPerM3;
		id<MTLBuffer> packedMomentumKGPerM2S;
		std::array<std::size_t,3> faceByteOffset;
		FireProductionViscousSchedule schedule;
		FireProductionResidentForceDiagnostics diagnostics;

		FireProductionMetalFrozenForceResidentState() : cellGasDensityKGPerM3(nil),
			packedFaceDensityKGPerM3(nil),packedMomentumKGPerM2S(nil)
		{
			faceByteOffset.fill(0u);
		}
	};

	//! Internal full-grid Private state seam. It executes preflight, every
	//! selected viscous substep, and gravity, then returns the still-resident
	//! density/momentum owners without terminal staging or projection.
	bool AdvanceFireProductionFrozenForceMetalResidentState(
		const FireProductionFrozenForceRequest& request,
		FireProductionMetalFrozenForceResidentState& state,
		std::string* error=0 );
#endif

	struct FireProductionResidentForceProjectionResult
	{
		FireProductionViscousSchedule forceSchedule;
		FireProductionProjectionResult projection;
		FireProductionResidentForceDiagnostics forceDiagnostics;
		std::uint32_t forceToProjectionDeviceToHostTransferCount;
		std::uint32_t residentProjectionInvocationCount;
		std::uint64_t combinedCertifiedWorkingSetBytes;
		std::uint64_t combinedActualMetalAllocationBytes;

		FireProductionResidentForceProjectionResult() :
			forceToProjectionDeviceToHostTransferCount(0u),residentProjectionInvocationCount(0u),
			combinedCertifiedWorkingSetBytes(0u),combinedActualMetalAllocationBytes(0u) {}
	};

	//! Resident production seam: frozen force substeps, gravity, and exactly one
	//! P2 projection without a full-grid host transfer between the operators.
	bool AdvanceFireProductionForceProjectionMetal(
		const FireProductionFrozenForceRequest& forceRequest,
		const std::vector<float>& divergenceTargetPerS,
		FireProductionResidentForceProjectionResult& result,
		std::string* error=0 );

	struct FireProductionResidentStepRequest
	{
		FireProductionFrozenForceRequest force;
		FireProductionCellPalindromeRequest cellTransport;
		FireProductionDualMomentumRequest dualTransport;
		std::vector<float> cellSourceIncrement;
		std::array<std::vector<float>,3> momentumSourceIncrement;
		std::vector<float> divergenceTargetPerS;
		std::vector<float> restorationDivergenceTargetPerS;
		std::vector<double> beginningManifoldDeviationPerCell;
		std::uint32_t physicalOpenProjectionVCycleCount;
		bool monitorManifoldDiagnostics;
		bool enforceManifoldPlateau;
		bool restoreManifoldOutliers;

		FireProductionResidentStepRequest() : physicalOpenProjectionVCycleCount(17u),
			monitorManifoldDiagnostics(true),enforceManifoldPlateau(false),
			restoreManifoldOutliers(true) {}
	};

	//! Stability-class boundary for monitored production.  The ordinary policy
	//! leaves the bulk manifold untouched, drains only the tail beyond 2^-4,
	//! and atomically refuses a beginning state beyond the 2^-2 dynamics bound.
	struct FireProductionManifoldTailTarget
	{
		std::vector<float> divergenceTargetPerS;
		std::uint32_t outlierCellCount;
		double excessSum;
		double drainedVolumeM3;
		double maximumTargetMagnitudePerS;

		FireProductionManifoldTailTarget() : outlierCellCount(0u),excessSum(0.0),
			drainedVolumeM3(0.0),maximumTargetMagnitudePerS(0.0) {}
	};
	bool DeriveFireProductionManifoldTailTarget(
		const std::vector<double>& beginningDeviationPerCell,
		double representedTimeStepS,double cellWidthM,
		FireProductionManifoldTailTarget&,std::string* error=0,
		double engagementThreshold=0x1p-4 );

	class FireProductionAcceptedManifoldObservation;
	struct FireProductionAcceptedCheckpointStateView
	{
		FireProductionProjectionShape shape;
		const std::vector<float>* conservativeValues;
		const std::array<std::vector<float>,3>* momentum;
		const std::array<std::vector<float>,3>* velocity;

		FireProductionAcceptedCheckpointStateView() : conservativeValues(0),momentum(0),velocity(0) {}
	};
	struct FireProductionAcceptedCheckpointLifecycleView
	{
		double simulationTimeS,previousStepS,lastAcceptedStepS;
		std::uint64_t acceptedSteps;
		bool productionState;
		const std::vector<double>* acceptedTimeStepHistoryS;

		FireProductionAcceptedCheckpointLifecycleView() : simulationTimeS(0.0),previousStepS(0.0),
			lastAcceptedStepS(0.0),acceptedSteps(0u),productionState(false),
			acceptedTimeStepHistoryS(0) {}
	};
	class FireProductionCheckpointManifoldAccess final
	{
	public:
		//! Reopens and independently validates the complete v12 checkpoint
		//! payload before restoring its trailing manifold record.  There is no
		//! public raw-tuple restoration seam.
		static bool RestoreValidatedCheckpointFile(
			const std::string&,std::uint64_t,std::uint64_t,std::uint64_t,
			const FireProductionAcceptedCheckpointStateView&,
			const FireProductionAcceptedCheckpointLifecycleView&,
			FireProductionAcceptedManifoldObservation&,std::string* );
	private:
		FireProductionCheckpointManifoldAccess()=delete;
	};
	struct FireProductionStableTimeStep;
	struct FireProductionResidentStepResult;

	class FireProductionAcceptedManifoldObservation
	{
	public:
		FireProductionAcceptedManifoldObservation() : available_(false),timeStepS_(0.0),
			maximumGeneration_(0.0),restorationDrainFraction_(0.0),
			residentPayloadDigest_(0u),acceptedStateDigest_(0u),acceptedStateDigestVersion_(0u),
			bindsResidentPayload_(false) {}
		bool Available() const { return available_; }
		double TimeStepS() const { return timeStepS_; }
		double MaximumGeneration() const { return maximumGeneration_; }
		double RestorationDrainFraction() const { return restorationDrainFraction_; }
		std::uint64_t SerializedAcceptedStateDigest() const { return acceptedStateDigest_; }
		bool MatchesAcceptedResidentPayload(const FireProductionResidentStepResult&) const;
		bool MatchesAcceptedStatePayload(const FireProductionProjectionShape&,
			const std::vector<float>&,const std::array<std::vector<float>,3>&,
			const std::array<std::vector<float>,3>&) const;

	private:
		void Clear() { available_=false;timeStepS_=0.0;maximumGeneration_=0.0;
			restorationDrainFraction_=0.0;residentPayloadDigest_=0u;
			acceptedStateDigest_=0u;acceptedStateDigestVersion_=0u;bindsResidentPayload_=false; }
		bool available_;
		double timeStepS_;
		double maximumGeneration_;
		double restorationDrainFraction_;
		std::uint64_t residentPayloadDigest_;
		std::uint64_t acceptedStateDigest_;
		unsigned int acceptedStateDigestVersion_;
		bool bindsResidentPayload_;
		friend class FireProductionCheckpointManifoldAccess;
		friend bool SelectFireProductionStableTimeStep(
			double,double,double,double,double,
			const FireProductionAcceptedCheckpointStateView&,
			const FireProductionAcceptedManifoldObservation&,
			FireProductionStableTimeStep&,std::string* );
		friend bool PublishFireProductionAcceptedManifoldObservation(
			double,FireProductionResidentStepResult&,
			FireProductionAcceptedManifoldObservation&,std::string* );
	};

	struct FireProductionStableTimeStep
	{
		double seconds;
		const char* activeLimit;

		FireProductionStableTimeStep() : seconds(0.0),activeLimit(0) {}
	};
	//! Pure r143 predictor arithmetic for diagnostics and the authority-bearing
	//! selector.  It does not confer accepted-observation authority.
	bool DeriveFireProductionManifoldTimeStep(
		double previousStepS,double maximumGeneration,double restorationDrainFraction,
		double& timeStepS,std::string* error=0 );
	//! Predictive production-campaign handoff.  The first Binary32 step uses the
	//! sealed audit observation directly; later steps use the accepted-step
	//! drain-aware predictor above.
	bool DeriveFireProductionInitialManifoldTimeStep(
		double auditTimeStepS,double auditMaximumGeneration,
		double& timeStepS,std::string* error=0 );

	//! Exact scalar topology consumed by the resident predictor/fold kernels.
	//! It replaces the beginning-state target with the predictor-state target.
	bool DeriveFireProductionAdvectiveAnomalyTarget(
		double beginningDeviation,double predictedDeviation,
		double representedTimeStepS,double inheritedTargetPerS,
		double& correctedTargetPerS,std::string* error=0 );

	//! Opaque proof that the resident owner, rather than a caller rewriting the
	//! public diagnostics, accepted the policy-selected projection/admissibility
	//! gates and measured the terminal manifold distribution.
	class FireProductionAcceptedManifoldToken
	{
	public:
		FireProductionAcceptedManifoldToken() : available_(false),representedTimeStepS_(0.0),
			maximumGeneration_(0.0),maximumAcceptedDeviation_(0.0),requiredDrainFraction_(0.0),
			deliveredDrainFraction_(0.0),maximumPostResidualPerS_(0.0),
			acceptedDeviationP95_(0.0),acceptedDeviationP50_(0.0),
			physicalMaximumPreResidualPerS_(0.0f),physicalMaximumPostResidualPerS_(0.0f),
			tailRestorationApplied_(false),tailCellCount_(0u),tailExcessSum_(0.0),
			tailDrainedVolumeM3_(0.0),dynamicsBoundPassed_(false),
			payloadDigest_(0u),acceptedStateDigest_(0u),acceptedStateDigestVersion_(0u),
			generationAuthoritative_(false),plateauEnforced_(false) {}
		FireProductionAcceptedManifoldToken(const FireProductionAcceptedManifoldToken&) :
			FireProductionAcceptedManifoldToken() {}
		FireProductionAcceptedManifoldToken& operator=(
			const FireProductionAcceptedManifoldToken&) { Clear();return *this; }
		FireProductionAcceptedManifoldToken(FireProductionAcceptedManifoldToken&& other) noexcept :
			available_(other.available_),representedTimeStepS_(other.representedTimeStepS_),
			maximumGeneration_(other.maximumGeneration_),
			maximumAcceptedDeviation_(other.maximumAcceptedDeviation_),
			requiredDrainFraction_(other.requiredDrainFraction_),
			deliveredDrainFraction_(other.deliveredDrainFraction_),
			maximumPostResidualPerS_(other.maximumPostResidualPerS_),
			acceptedDeviationP95_(other.acceptedDeviationP95_),
			acceptedDeviationP50_(other.acceptedDeviationP50_),
			physicalMaximumPreResidualPerS_(other.physicalMaximumPreResidualPerS_),
			physicalMaximumPostResidualPerS_(other.physicalMaximumPostResidualPerS_),
			tailRestorationApplied_(other.tailRestorationApplied_),
			tailCellCount_(other.tailCellCount_),tailExcessSum_(other.tailExcessSum_),
			tailDrainedVolumeM3_(other.tailDrainedVolumeM3_),
			dynamicsBoundPassed_(other.dynamicsBoundPassed_),
			payloadDigest_(other.payloadDigest_),acceptedStateDigest_(other.acceptedStateDigest_),
			acceptedStateDigestVersion_(other.acceptedStateDigestVersion_),
			generationAuthoritative_(other.generationAuthoritative_),
			plateauEnforced_(other.plateauEnforced_) {
			other.Clear(); }
		FireProductionAcceptedManifoldToken& operator=(
			FireProductionAcceptedManifoldToken&& other) noexcept {
			if(this!=&other){available_=other.available_;
				representedTimeStepS_=other.representedTimeStepS_;
				maximumGeneration_=other.maximumGeneration_;
				maximumAcceptedDeviation_=other.maximumAcceptedDeviation_;
				requiredDrainFraction_=other.requiredDrainFraction_;
				deliveredDrainFraction_=other.deliveredDrainFraction_;
				maximumPostResidualPerS_=other.maximumPostResidualPerS_;
				acceptedDeviationP95_=other.acceptedDeviationP95_;
				acceptedDeviationP50_=other.acceptedDeviationP50_;
				physicalMaximumPreResidualPerS_=other.physicalMaximumPreResidualPerS_;
				physicalMaximumPostResidualPerS_=other.physicalMaximumPostResidualPerS_;
				tailRestorationApplied_=other.tailRestorationApplied_;
				tailCellCount_=other.tailCellCount_;tailExcessSum_=other.tailExcessSum_;
				tailDrainedVolumeM3_=other.tailDrainedVolumeM3_;
				dynamicsBoundPassed_=other.dynamicsBoundPassed_;
				payloadDigest_=other.payloadDigest_;
				acceptedStateDigest_=other.acceptedStateDigest_;
				acceptedStateDigestVersion_=other.acceptedStateDigestVersion_;
				generationAuthoritative_=other.generationAuthoritative_;
				plateauEnforced_=other.plateauEnforced_;other.Clear();}
			return *this;
		}
		bool Available() const { return available_; }

	private:
		void Clear() { available_=false;representedTimeStepS_=0.0;maximumGeneration_=0.0;
			maximumAcceptedDeviation_=0.0;requiredDrainFraction_=0.0;
			deliveredDrainFraction_=0.0;maximumPostResidualPerS_=0.0;
			acceptedDeviationP95_=0.0;acceptedDeviationP50_=0.0;
			physicalMaximumPreResidualPerS_=0.0f;physicalMaximumPostResidualPerS_=0.0f;
			tailRestorationApplied_=false;tailCellCount_=0u;tailExcessSum_=0.0;
			tailDrainedVolumeM3_=0.0;dynamicsBoundPassed_=false;
			payloadDigest_=0u;acceptedStateDigest_=0u;acceptedStateDigestVersion_=0u;
			generationAuthoritative_=false;plateauEnforced_=false; }
		bool available_;
		double representedTimeStepS_;
		double maximumGeneration_;
		double maximumAcceptedDeviation_;
		double requiredDrainFraction_;
		double deliveredDrainFraction_;
		double maximumPostResidualPerS_;
		double acceptedDeviationP95_;
		double acceptedDeviationP50_;
		float physicalMaximumPreResidualPerS_;
		float physicalMaximumPostResidualPerS_;
		bool tailRestorationApplied_;
		std::uint32_t tailCellCount_;
		double tailExcessSum_;
		double tailDrainedVolumeM3_;
		bool dynamicsBoundPassed_;
		std::uint64_t payloadDigest_;
		std::uint64_t acceptedStateDigest_;
		unsigned int acceptedStateDigestVersion_;
		bool generationAuthoritative_;
		bool plateauEnforced_;
		friend struct FireProductionResidentStepResult;
		friend bool AdvanceFireProductionResidentStepMetal(
			const FireProductionResidentStepRequest&,
			FireProductionResidentStepResult&,
			std::string* );
		friend bool AttemptFireProductionResidentStepMetal(
			const FireProductionResidentStepRequest&,
			FireProductionResidentStepResult&,
			std::string* );
		friend bool PublishFireProductionAcceptedManifoldObservation(
			double,
			FireProductionResidentStepResult&,
			FireProductionAcceptedManifoldObservation&,
			std::string* );
	};

	//! Selects the production step from the CFL family, the 1.1 growth cap, and
	//! the r143 accepted-step manifold observation.  An unavailable observation
	//! is valid only for the first step of a run.
	bool SelectFireProductionStableTimeStep(
		double cellWidthM,
		double maximumVelocityMPerS,
		double maximumPositiveReducedGravityMPerS2,
		double maximumKinematicTransportM2PerS,
		double previousStepS,
		const FireProductionAcceptedCheckpointStateView& currentAcceptedState,
		const FireProductionAcceptedManifoldObservation& previousManifold,
		FireProductionStableTimeStep& result,
		std::string* error=0 );

	struct FireProductionResidentStepResult
	{
		std::vector<float> conservativeValues;
		FireProductionDualMomentumResult transportedDual;
		FireProductionProjectionResult physicalProjection;
		FireProductionProjectionResult projection;
		FireProductionViscousSchedule forceSchedule;
		FireProductionResidentForceDiagnostics forceDiagnostics;
		std::uint32_t cellSubmapCount;
		std::uint32_t dualSubmapCount;
		std::uint32_t sourceCommandCommitCount;
		std::uint32_t residentProjectionInvocationCount;
		std::uint32_t interstageFullGridTransferCount;
		std::uint32_t terminalStagingCount;
		std::uint64_t combinedCertifiedWorkingSetBytes;
		std::uint64_t combinedActualMetalAllocationBytes;
		double deviceElapsedMS;
		double deviceMakespanMS;
		float representedTimeStepS;
		double maximumManifoldGeneration;
		double maximumAcceptedManifoldDeviation;
		double acceptedManifoldDeviationP95;
		double acceptedManifoldDeviationP50;
		bool manifoldGenerationAuthoritative;
		bool manifoldTailRestorationApplied;
		std::uint32_t manifoldTailCellCount;
		double manifoldTailExcessSum;
		double manifoldTailDrainedVolumeM3;
		bool manifoldDynamicsBoundPassed;
		double maximumPredictedAdvectiveManifoldAnomaly;
		std::uint32_t manifoldMapCellCount;
		std::uint32_t manifoldScalarDeviceToHostTransferCount;
		std::uint32_t manifoldFullGridDeviceToHostTransferCount;
		std::uint32_t advectiveAnomalyClosurePassCount;
		std::array<double,3> manifoldStageGeneration;
		double requiredRestorationDrainFraction;
		double deliveredRestorationDrainFraction;
		double restorationResidualBandPerS;
		double suggestedManifoldTimeStepS;
		bool manifoldNextTimeStepAvailable;
		bool manifoldDiagnosticsMonitored;
		bool manifoldPlateauEnforced;
		bool manifoldAllowanceExceeded;
		bool manifoldCeilingExceeded;
		bool manifoldPlateauPassed;
		FireStateProducerPrecision conservativeProducerPrecision;
		FireProductionProjectionShape acceptedShape;
		bool HasAcceptedManifoldToken() const { return acceptedManifoldToken_.Available(); }
		//! Revalidates the producer-owned token against every mutable diagnostic and
		//! payload byte before an owner may classify the attempt as accepted.
		bool AcceptedManifoldTokenMatchesCurrentPayload() const;

		FireProductionResidentStepResult() : cellSubmapCount(0u),dualSubmapCount(0u),
			sourceCommandCommitCount(0u),residentProjectionInvocationCount(0u),
			interstageFullGridTransferCount(0u),terminalStagingCount(0u),
			combinedCertifiedWorkingSetBytes(0u),combinedActualMetalAllocationBytes(0u),
			deviceElapsedMS(0.0),deviceMakespanMS(0.0),
			representedTimeStepS(0.0f),maximumManifoldGeneration(0.0),
			maximumAcceptedManifoldDeviation(0.0),acceptedManifoldDeviationP95(0.0),
			acceptedManifoldDeviationP50(0.0),manifoldGenerationAuthoritative(false),
			manifoldTailRestorationApplied(false),manifoldTailCellCount(0u),
			manifoldTailExcessSum(0.0),manifoldTailDrainedVolumeM3(0.0),
			manifoldDynamicsBoundPassed(false),
			maximumPredictedAdvectiveManifoldAnomaly(0.0),
			manifoldMapCellCount(0u),
			manifoldScalarDeviceToHostTransferCount(0u),manifoldFullGridDeviceToHostTransferCount(0u),
			advectiveAnomalyClosurePassCount(0u),
			manifoldStageGeneration{{0.0,0.0,0.0}},requiredRestorationDrainFraction(0.0),
			deliveredRestorationDrainFraction(0.0),restorationResidualBandPerS(0.0),
			suggestedManifoldTimeStepS(0.0),manifoldNextTimeStepAvailable(false),
			manifoldDiagnosticsMonitored(false),manifoldPlateauEnforced(false),
			manifoldAllowanceExceeded(false),manifoldCeilingExceeded(false),
			manifoldPlateauPassed(false),
			conservativeProducerPrecision(FireStateProducerPrecision::Unknown) {}

	private:
		FireProductionAcceptedManifoldToken acceptedManifoldToken_;
		friend bool AdvanceFireProductionResidentStepMetal(
			const FireProductionResidentStepRequest&,
			FireProductionResidentStepResult&,
			std::string* );
		friend bool AttemptFireProductionResidentStepMetal(
			const FireProductionResidentStepRequest&,
			FireProductionResidentStepResult&,
			std::string* );
		friend bool PublishFireProductionAcceptedManifoldObservation(
			double,
			FireProductionResidentStepResult&,
			FireProductionAcceptedManifoldObservation&,
			std::string* );
	};

	enum class FireProductionResidentStepAttemptDisposition : std::uint8_t
	{
		Accepted=0u,
		RetryAtSuggestedTimeStep=1u,
		Rejected=2u
	};

	//! Production-owned ordinary rejection policy. The step operator reports a
	//! tokenless plateau refusal; the trajectory owner uses this disposition to
	//! rebuild its independently sealed target at the suggested duration. The
	//! retry index and cap live here rather than in calibration fixtures.
	inline FireProductionResidentStepAttemptDisposition
		ClassifyFireProductionResidentStepAttempt(const unsigned int candidateIndex,
			const FireProductionResidentStepResult& attempted,
			unsigned int& nextCandidateIndex,double& nextTimeStepS)
	{
		nextCandidateIndex=0u;nextTimeStepS=0.0;
		if(candidateIndex>=FireStepRejectionRetryCap)
			return FireProductionResidentStepAttemptDisposition::Rejected;
		if(attempted.manifoldPlateauPassed&&
			attempted.AcceptedManifoldTokenMatchesCurrentPayload())
			return FireProductionResidentStepAttemptDisposition::Accepted;
		const double represented=static_cast<double>(attempted.representedTimeStepS);
		if(!attempted.manifoldPlateauPassed&&!attempted.HasAcceptedManifoldToken()&&
			attempted.manifoldNextTimeStepAvailable&&candidateIndex<
			FireStepRejectionRetryCap-1u&&candidateIndex+1u<
			FireStepRejectionRetryCap&&represented>0.0&&std::isfinite(represented)&&
			attempted.suggestedManifoldTimeStepS>0.0&&
			std::isfinite(attempted.suggestedManifoldTimeStepS)&&
			attempted.suggestedManifoldTimeStepS<represented){
			nextCandidateIndex=candidateIndex+1u;
			nextTimeStepS=attempted.suggestedManifoldTimeStepS;
			return FireProductionResidentStepAttemptDisposition::RetryAtSuggestedTimeStep;
		}
		return FireProductionResidentStepAttemptDisposition::Rejected;
	}
	//! Single owner predicate for accepted-token issuance.  Diagnostics alone do
	//! not mint authority; both projection validations are structural inputs.
	bool FireProductionResidentStepEligibleForAcceptedManifoldToken(
		const FireProductionResidentStepResult& );
	bool FireProductionEulerianGenerationHasMaterialAuthority(
		const std::vector<double>& beginningManifoldDeviationPerCell,
		const std::array<std::vector<float>,3>& frozenTransportVelocityMPerS );

	//! Canonical bytes that an accepted production step applies to the next
	//! checkpoint state.  Checkpoint persistence must reproduce this digest;
	//! it cannot transplant a valid observation onto different state bytes.
	inline std::uint64_t FireProductionAcceptedStatePayloadDigest(
		const FireProductionProjectionShape& shape,
		const std::vector<float>& conservativeValues,
		const std::array<std::vector<float>,3>& momentum,
		const std::array<std::vector<float>,3>& velocity )
	{
		std::uint64_t digest=UINT64_C(0x65f07b31c42a98de);
		auto appendWord=[&](const std::uint64_t word) {
			for(unsigned int byte=0u;byte<8u;++byte){
				digest^=static_cast<unsigned char>(word>>(8u*byte));
				digest*=UINT64_C(1099511628211);}
		};
		std::uint64_t fieldTag=0u;
		appendWord(UINT64_C(0x7265736964656e74));
		appendWord(static_cast<std::uint64_t>(shape.nx));
		appendWord(static_cast<std::uint64_t>(shape.ny));
		appendWord(static_cast<std::uint64_t>(shape.nz));
		std::uint32_t widthBits=0u;std::memcpy(&widthBits,&shape.cellWidthM,sizeof(widthBits));
		appendWord(widthBits);
		auto append=[&](const std::vector<float>& values) {
			appendWord(++fieldTag);appendWord(static_cast<std::uint64_t>(values.size()));
			for(const float scalar:values){std::uint32_t bits=0u;
				std::memcpy(&bits,&scalar,sizeof(bits));
				for(unsigned int byte=0u;byte<4u;++byte){
					digest^=static_cast<unsigned char>(bits>>(8u*byte));
					digest*=UINT64_C(1099511628211);}}
		};
		append(conservativeValues);
		for(unsigned int axis=0u;axis<3u;++axis){append(momentum[axis]);append(velocity[axis]);}
		return digest^UINT64_C(0xd64b291e3fa5708c);
	}
	inline std::uint64_t FireProductionAcceptedStatePayloadDigestFast(
		const FireProductionProjectionShape& shape,
		const std::vector<float>& conservativeValues,
		const std::array<std::vector<float>,3>& momentum,
		const std::array<std::vector<float>,3>& velocity )
	{
		std::uint64_t digest=UINT64_C(0x65f07b31c42a98de);
		auto avalanche=[](std::uint64_t word) {
			word^=word>>32u;word*=UINT64_C(0xd6e8feb86659fd93);
			word^=word>>32u;word*=UINT64_C(0xd6e8feb86659fd93);
			return word^(word>>32u);
		};
		auto rotate=[](const std::uint64_t word,const unsigned int bits) {
			return (word<<bits)|(word>>(64u-bits));
		};
		auto appendWord=[&](const std::uint64_t word) {
			digest^=word+UINT64_C(0x9e3779b97f4a7c15)+(digest<<6u)+(digest>>2u);
		};
		std::uint64_t fieldTag=0u;
		appendWord(UINT64_C(0x7265736964656e74));
		appendWord(static_cast<std::uint64_t>(shape.nx));
		appendWord(static_cast<std::uint64_t>(shape.ny));
		appendWord(static_cast<std::uint64_t>(shape.nz));
		std::uint32_t widthBits=0u;std::memcpy(&widthBits,&shape.cellWidthM,sizeof(widthBits));
		appendWord(widthBits);
		auto append=[&](const std::vector<float>& values) {
			appendWord(++fieldTag);appendWord(static_cast<std::uint64_t>(values.size()));
			std::uint64_t ordered=UINT64_C(0x243f6a8885a308d3)^fieldTag;
			for(std::size_t index=0u;index<values.size();++index){std::uint32_t bits=0u;
				std::memcpy(&bits,&values[index],sizeof(bits));
				ordered^=static_cast<std::uint64_t>(bits)+UINT64_C(0x9e3779b97f4a7c15)+
					(static_cast<std::uint64_t>(index)<<32u);
				ordered=rotate(ordered,27u)*UINT64_C(0x3c79ac492ba7b653)+
					UINT64_C(0x1c69b3f74ac4ae35);
			}
			appendWord(avalanche(ordered));
		};
		append(conservativeValues);
		for(unsigned int axis=0u;axis<3u;++axis){append(momentum[axis]);append(velocity[axis]);}
		return avalanche(digest^fieldTag^UINT64_C(0xd64b291e3fa5708c));
	}

	//! Publishes authenticated accepted-state metadata. Enforced instrumentation
	//! may also publish a limiter operand; monitored production always publishes
	//! zero manifold timestep authority.
	inline std::uint64_t FireProductionAcceptedManifoldPayloadDigest(
		const FireProductionResidentStepResult& value )
	{
		std::uint64_t digest=UINT64_C(0xd6e8feb86659fd93);
		auto avalanche=[](std::uint64_t word) {
			word^=word>>32u;word*=UINT64_C(0xd6e8feb86659fd93);
			word^=word>>32u;word*=UINT64_C(0xd6e8feb86659fd93);
			return word^(word>>32u);
		};
		auto rotate=[](const std::uint64_t word,const unsigned int bits) {
			return (word<<bits)|(word>>(64u-bits));
		};
		auto appendWord=[&](const std::uint64_t word) {
			digest^=word+UINT64_C(0x9e3779b97f4a7c15)+(digest<<6u)+(digest>>2u);
		};
		std::uint64_t fieldTag=0u;
		auto append=[&](const std::vector<float>& values) {
			appendWord(++fieldTag);appendWord(static_cast<std::uint64_t>(values.size()));
			std::uint64_t ordered=UINT64_C(0x243f6a8885a308d3)^fieldTag;
			for(std::size_t index=0u;index<values.size();++index){std::uint32_t bits=0u;
				std::memcpy(&bits,&values[index],sizeof(bits));
				ordered^=static_cast<std::uint64_t>(bits)+UINT64_C(0x9e3779b97f4a7c15)+
					(static_cast<std::uint64_t>(index)<<32u);
				ordered=rotate(ordered,27u)*UINT64_C(0x3c79ac492ba7b653)+
					UINT64_C(0x1c69b3f74ac4ae35);
			}
			appendWord(avalanche(ordered));
		};
		append(value.conservativeValues);
		for(unsigned int axis=0u;axis<3u;++axis){
			append(value.transportedDual.auxiliaryFaceDensity[axis]);
			append(value.transportedDual.momentum[axis]);
			append(value.physicalProjection.faceDensityKGPerM3[axis]);
			append(value.physicalProjection.velocityMPerS[axis]);
			append(value.physicalProjection.momentumKGPerM2S[axis]);
			append(value.projection.faceDensityKGPerM3[axis]);
			append(value.projection.velocityMPerS[axis]);
			append(value.projection.momentumKGPerM2S[axis]);
		}
		append(value.physicalProjection.pressurePa);
		append(value.projection.pressurePa);
		auto appendBytes=[&](const std::vector<unsigned char>& values) {
			appendWord(++fieldTag);appendWord(static_cast<std::uint64_t>(values.size()));
			std::uint64_t ordered=UINT64_C(0x243f6a8885a308d3)^fieldTag;
			for(std::size_t index=0u;index<values.size();++index){
				ordered^=static_cast<std::uint64_t>(values[index])+
					UINT64_C(0x9e3779b97f4a7c15)+(static_cast<std::uint64_t>(index)<<32u);
				ordered=rotate(ordered,27u)*UINT64_C(0x3c79ac492ba7b653)+
					UINT64_C(0x1c69b3f74ac4ae35);
			}
			appendWord(avalanche(ordered));
		};
		for(const auto& side:value.physicalProjection.pressureOpenInflow)appendBytes(side);
		for(const auto& side:value.projection.pressureOpenInflow)appendBytes(side);
		return avalanche(digest^fieldTag);
	}
	inline bool FireProductionAcceptedManifoldObservation::MatchesAcceptedResidentPayload(
		const FireProductionResidentStepResult& value ) const
	{
		return available_&&bindsResidentPayload_&&residentPayloadDigest_==
			FireProductionAcceptedManifoldPayloadDigest(value);
	}
	inline bool FireProductionAcceptedManifoldObservation::MatchesAcceptedStatePayload(
		const FireProductionProjectionShape& shape,const std::vector<float>& conservativeValues,
		const std::array<std::vector<float>,3>& momentum,
		const std::array<std::vector<float>,3>& velocity ) const
	{
		const std::uint64_t digest=acceptedStateDigestVersion_==1u?
			FireProductionAcceptedStatePayloadDigest(shape,conservativeValues,momentum,velocity):
			(acceptedStateDigestVersion_==2u?FireProductionAcceptedStatePayloadDigestFast(
				shape,conservativeValues,momentum,velocity):0u);
		return available_&&digest!=0u&&digest==acceptedStateDigest_;
	}
	bool PublishFireProductionAcceptedManifoldObservation(
		double acceptedStepS,
		FireProductionResidentStepResult& acceptedStep,
		FireProductionAcceptedManifoldObservation& result,
		std::string* error=0 );

	//! Full resident P3 step. Production defaults to one physical P2 projection
	//! plus terminal manifold diagnostics. Absolute-reference restoration and
	//! its timestep limiter remain opt-in instrumentation through
	//! enforceManifoldPlateau.
	bool AdvanceFireProductionResidentStepMetal(
		const FireProductionResidentStepRequest& request,
		FireProductionResidentStepResult& result,
		std::string* error=0 );

	//! Executes one candidate without weakening rejection semantics.  A
	//! plateau refusal returns false but preserves only the computed, tokenless
	//! result and its derived next-step suggestion so the owner may retry from
	//! the unchanged beginning.  Every other failure leaves a default result.
	bool AttemptFireProductionResidentStepMetal(
		const FireProductionResidentStepRequest& request,
		FireProductionResidentStepResult& rejectedOrAcceptedResult,
		std::string* error=0 );

	//! Isolated Section 3.7 candidate seam.  Ordinary production never selects
	//! this operator.  The calibration owner may call it only after independently
	//! validating the sealed checkpoint/case identity; the request copy retains
	//! the five accepted primal gas-mass doses needed by the candidate.
	bool AttemptFireProductionCompatibleMomentumDiagnosticMetal(
		const FireProductionResidentStepRequest& request,
		FireProductionResidentStepResult& rejectedOrAcceptedResult,
		std::string* error=0 );

	//! Identity-bearing status for the isolated single-stage production FCT
	//! candidate. This type deliberately has no accepted-manifold token: a
	//! diagnostic trajectory may not confer ordinary-production authority.
	enum class FireProductionSingleStageFCTDiagnosticPhase : std::uint8_t
	{
		Unavailable=0u,
		Preflight=1u,
		ForceAdvance=2u,
		FCTSolve=3u,
		PhysicalProjection=4u,
		TailProjection=5u,
		Accepted=6u
	};

	//! Prior accepted pressure-open classification consumed only by the isolated
	//! FCT diagnostic. All six side arrays are mandatory and digest-bound with the
	//! accepted predecessor-state payload through the caller-supplied identity,
	//! including on wall and periodic sides.
	struct FireProductionSingleStageFCTBoundaryState
	{
		std::array<std::vector<unsigned char>,6> pressureOpenInflow;
		std::uint64_t statePayloadIdentity;
		std::uint64_t identity;

		FireProductionSingleStageFCTBoundaryState() : statePayloadIdentity(0u),identity(0u) {}
	};

	struct FireProductionSingleStageFCTDiagnosticResult
	{
		std::vector<float> conservativeValues;
		FireProductionProjectionResult physicalProjection;
		FireProductionProjectionResult projection;
		FireProductionViscousSchedule forceSchedule;
		FireProductionResidentForceDiagnostics forceDiagnostics;
		std::uint32_t operatorVersion;
		std::uint64_t operatorIdentity;
		std::uint64_t zeroPhysicalGasFluxIdentity;
		std::uint64_t boundaryStateIdentity;
		std::uint64_t beginningStateIdentity;
		FireProductionSingleStageFCTDiagnosticPhase phase;
		std::uint32_t fluxPairBuildCount;
		std::uint32_t fctSolveCount;
		std::uint32_t compatibleRateApplicationCount;
		std::uint32_t sourceApplicationCount;
		std::uint32_t residentProjectionInvocationCount;
		std::uint32_t interstageFullGridTransferCount;
		std::uint32_t terminalStagingCount;
		std::uint32_t failureBitmap;
		std::uint32_t tailCellCount;
		std::uint64_t certifiedWorkingSetBytes;
		std::uint64_t actualMetalAllocationBytes;
		std::uint64_t alphaIdentity;
		std::uint64_t scalarStageIdentity;
		float maximumCommutingResidual;
		float scalarStageGamma128;
		float maximumScalarStageAbsoluteDifference;
		float maximumScalarStageNormalizedDifference;
		float maximumCompatibleRateAbsoluteDifference;
		float maximumCompatibleRateNormalizedDifference;
		double tailDrainedVolumeM3;
		bool pipelineIdentityComplete;
		bool scalarAdmissible;
		bool scalarStageIdentityPassed;
		bool scalarStageExactZeroPassed;
		bool scalarStageProducerEquivalencePassed;
		bool compatibleRateExactZeroPassed;
		bool compatibleRateProducerEquivalencePassed;
		bool affineIdentityPassed;
		bool commutingIdentityPassed;
		bool accepted;

		FireProductionSingleStageFCTDiagnosticResult() : operatorVersion(0u),
			operatorIdentity(0u),zeroPhysicalGasFluxIdentity(0u),boundaryStateIdentity(0u),
			beginningStateIdentity(0u),
			phase(FireProductionSingleStageFCTDiagnosticPhase::Unavailable),
			fluxPairBuildCount(0u),fctSolveCount(0u),compatibleRateApplicationCount(0u),
			sourceApplicationCount(0u),residentProjectionInvocationCount(0u),
			interstageFullGridTransferCount(0u),terminalStagingCount(0u),failureBitmap(0u),
			tailCellCount(0u),certifiedWorkingSetBytes(0u),actualMetalAllocationBytes(0u),
			alphaIdentity(0u),scalarStageIdentity(0u),maximumCommutingResidual(0.0f),
			scalarStageGamma128(0.0f),maximumScalarStageAbsoluteDifference(0.0f),
			maximumScalarStageNormalizedDifference(0.0f),
			maximumCompatibleRateAbsoluteDifference(0.0f),
			maximumCompatibleRateNormalizedDifference(0.0f),
			tailDrainedVolumeM3(0.0),
			pipelineIdentityComplete(false),scalarAdmissible(false),
			scalarStageIdentityPassed(false),scalarStageExactZeroPassed(false),
			scalarStageProducerEquivalencePassed(false),compatibleRateExactZeroPassed(false),
			compatibleRateProducerEquivalencePassed(false),affineIdentityPassed(false),
			commutingIdentityPassed(false),accepted(false) {}
	};

	//! Explicit tokenless diagnostic seam for production's single-stage schedule
	//! with a coefficient-free §3.7 scalar/momentum FCT operator. It is never
	//! selected by the ordinary resident-step owner. The physical gas diffusion
	//! flux J_g is identity-bearing exact +0 because production owns no such flux.
	bool SealFireProductionSingleStageFCTBoundaryState(
		const FireProductionProjectionShape& shape,
		const std::array<FireProductionProjectionBoundary,6>& boundary,
		FireProductionSingleStageFCTBoundaryState& state,
		std::string* error=0 );
	bool AttemptFireProductionSingleStageFCTDiagnosticMetal(
		const FireProductionResidentStepRequest& request,
		const FireProductionSingleStageFCTBoundaryState& boundaryState,
		FireProductionSingleStageFCTDiagnosticResult& result,
		std::string* error=0 );

	//! Thread-local observed Metal commits, exposed only to bind fail-before-work
	//! owner gates. Unsupported builds return zero.
	std::uint64_t FireProductionResidentStepMetalCommandCommitCount();
}

#endif
