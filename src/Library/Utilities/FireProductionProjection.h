//////////////////////////////////////////////////////////////////////
//
//  FireProductionProjection.h - one-pass fp32 production MAC projection
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FIREPRODUCTIONPROJECTION_
#define FIREPRODUCTIONPROJECTION_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#if defined(__OBJC__) && defined(__APPLE__)
#import <Metal/Metal.h>
#endif

namespace RISE
{
#if defined(__APPLE__)
	//! Identity of the exact source compiled by the resident projection library.
	std::string FireProductionProjectionMetalKernelSourceSHA256();
#endif
	enum FireProductionProjectionBoundary
	{
		FireProductionProjectionPeriodic,
		FireProductionProjectionPressureOpen,
		FireProductionProjectionWall
	};

	//! Open-boundary classification ownership for one projection application.
	//! Derived mode is the ordinary one-pass production behavior.  Sealed mode
	//! consumes the caller's complete prior/Picard class without reclassifying it;
	//! this is the primitive required by the projected-Heun active-set owner.
	enum FireProductionProjectionOpenClassificationMode
	{
		FireProductionProjectionDeriveOpenClassification,
		FireProductionProjectionUseSealedOpenClassification
	};

	//! Pressure-open head ownership for one projection application.  Ordinary
	//! R0/R1 projections derive Bernoulli head from the projected stage's own
	//! provisional velocity.  The R2 endpoint projection instead consumes the
	//! time-integrated head assembled from the accepted R0/R1 stage records.
	enum FireProductionProjectionOpenHeadMode
	{
		FireProductionProjectionDeriveCurrentOpenHead,
		FireProductionProjectionUseSealedOpenHead
	};

	//! Publication policy for the pressure-open class after correction.  The
	//! ordinary frozen-active-set solve retains its input class.  R2 derives the
	//! endpoint class from the corrected velocity and uses the caller's sealed
	//! class only inside the velocity deadband.
	enum FireProductionProjectionOutputClassificationMode
	{
		FireProductionProjectionPreserveOpenClassification,
		FireProductionProjectionDeriveEndpointOpenClassification
	};

	struct FireProductionProjectionShape
	{
		std::size_t nx,ny,nz;
		float cellWidthM;

		FireProductionProjectionShape() : nx(0),ny(0),nz(0),cellWidthM(0.0f) {}
		std::size_t CellCount() const {return nx*ny*nz;}
	};

	struct FireProductionProjectionRequest
	{
		FireProductionProjectionShape shape;
		float timeStepS;
		float ambientDensityKGPerM3;
		std::array<FireProductionProjectionBoundary,6> boundary;
		std::vector<float> gasDensityKGPerM3;
		std::array<std::vector<float>,3> provisionalMomentumKGPerM2S;
		std::vector<float> divergenceTargetPerS;
		std::array<std::vector<unsigned char>,6> sealedPressureOpenInflow;
		std::array<std::vector<float>,6> sealedPressureOpenDynamicPressurePa;
		std::uint32_t residentPhysicalOpenVCycleCount;
		FireProductionProjectionOpenClassificationMode openClassificationMode;
		FireProductionProjectionOpenHeadMode openHeadMode;
		FireProductionProjectionOutputClassificationMode outputClassificationMode;
		float endpointVelocityToleranceMPerS;

		FireProductionProjectionRequest() : timeStepS(0.0f),ambientDensityKGPerM3(0.0f),
			residentPhysicalOpenVCycleCount(17u),
			openClassificationMode(FireProductionProjectionDeriveOpenClassification),
			openHeadMode(FireProductionProjectionDeriveCurrentOpenHead),
			outputClassificationMode(FireProductionProjectionPreserveOpenClassification),
			endpointVelocityToleranceMPerS(0.0f)
		{
			boundary.fill(FireProductionProjectionWall);
		}
	};

	struct FireProductionProjectionResult
	{
		std::array<std::vector<float>,3> faceDensityKGPerM3;
		std::array<std::vector<float>,3> velocityMPerS;
		std::array<std::vector<float>,3> momentumKGPerM2S;
		std::vector<float> pressurePa;
		std::array<std::vector<unsigned char>,6> pressureOpenInflow;
		float maximumPreProjectionResidualPerS;
		float maximumPostProjectionResidualPerS;
		float validationBandPerS;
		float maximumOpenComplementarityDiscrepancyMPerS;
		float removedFineRightHandSideMean;
		std::uint32_t executedVCycleCount;
		std::uint64_t executedJacobiSweepCount;
		std::uint32_t residentUploadStagingCount;
		std::uint32_t residentInterstageDeviceToHostTransferCount;
		std::uint32_t residentTerminalStagingCount;
		std::uint32_t residentCommandCommitCount;
		std::uint32_t residentProjectionInvocationCount;
		std::uint64_t residentCertifiedWorkingSetBytes;
		std::uint64_t residentActualMetalAllocationBytes;
		bool validationPassed;
		double deviceElapsedMS;
		double deviceStartTimeS;
		double deviceEndTimeS;

		FireProductionProjectionResult() : maximumPreProjectionResidualPerS(0.0f),
			maximumPostProjectionResidualPerS(0.0f),validationBandPerS(0.0f),
			maximumOpenComplementarityDiscrepancyMPerS(0.0f),
			removedFineRightHandSideMean(0.0f),executedVCycleCount(0u),
			executedJacobiSweepCount(0u),residentUploadStagingCount(0u),
			residentInterstageDeviceToHostTransferCount(0u),residentTerminalStagingCount(0u),
			residentCommandCommitCount(0u),residentProjectionInvocationCount(0u),
			residentCertifiedWorkingSetBytes(0u),residentActualMetalAllocationBytes(0u),validationPassed(false),
			deviceElapsedMS(0.0),deviceStartTimeS(0.0),deviceEndTimeS(0.0) {}
	};

	std::size_t FireProductionProjectionFaceCount(
		const FireProductionProjectionShape& shape, unsigned int axis );

	bool FireProductionProjectionWorkingSetBytes(
		const FireProductionProjectionShape& shape,
		std::uint64_t& bytes );

	bool FireProductionProjectionResidualWithinBand(
		float maximumResidualPerS,
		float maximumVelocityMPerS,
		float domainLengthM,
		bool& withinBand );

	//! Restoration is independently validated against its own characteristic
	//! divergence scale. It deliberately does not borrow the physical
	//! projection's validation result or U/L scale.
	bool FireProductionRestorationProjectionResidualWithinBand(
		float maximumResidualPerS,
		float maximumRestorationTargetPerS,
		bool& withinBand );

	struct FireProductionRestorationPlateauValidation
	{
		double requiredDrainFraction;
		double deliveredDrainFraction;
		double maximumPostResidualPerS;
		bool mechanismPassed;

		FireProductionRestorationPlateauValidation() : requiredDrainFraction(0.0),
			deliveredDrainFraction(0.0),maximumPostResidualPerS(0.0),
			mechanismPassed(false) {}
	};

	//! Certifies that the restoration mechanism does not amplify its residual.
	//! The delivered drain remains a timestep-predictor diagnostic; a regime
	//! campaign separately proves non-secular long-shadow boundedness.
	bool FireProductionRestorationPlateauWithinBand(
		double maximumManifoldGeneration,
		double maximumPreProjectionResidualPerS,
		double maximumPostProjectionResidualPerS,
		FireProductionRestorationPlateauValidation& result );

	bool ValidateFireProductionProjectionRequest(
		const FireProductionProjectionRequest& request,
		std::string* error=0 );

	//! Strict-binary32 comparator implementing the stored P2 schedule.
	bool ProjectFireProductionCPU(
		const FireProductionProjectionRequest& request,
		FireProductionProjectionResult& result,
		std::string* error=0 );

	//! CPU calibration owner for the 17-cycle pressure-open physical pass used
	//! only inside the two-projection resident composition.
	bool ProjectFireProductionResidentPhysicalCPU(
		const FireProductionProjectionRequest& request,
		FireProductionProjectionResult& result,
		std::string* error=0 );

	//! CPU calibration mirror of the correction-only restoration invocation.
	//! The request target is an authored divergence increment, not an absolute
	//! final divergence target.
	bool ProjectFireProductionRestorationCPU(
		const FireProductionProjectionRequest& request,
		FireProductionProjectionResult& result,
		std::string* error=0 );

#if defined(__OBJC__) && defined(__APPLE__)
	struct FireProductionMetalProjectionResidentInput
	{
		id<MTLBuffer> gasDensityKGPerM3;
		std::array<id<MTLBuffer>,3> provisionalMomentumKGPerM2S;
		std::array<std::size_t,3> provisionalMomentumByteOffset;
		id<MTLBuffer> divergenceTargetPerS;
		//! Optional device-issued identity of the exact target surface.  Live
		//! projected-Heun owners must provide it; legacy resident callers leave it
		//! nil and consequently receive no projection publication identity.
		id<MTLBuffer> targetPublicationIdentity;
		//! Device-issued consumer capability for the exact target publication.  An
		//! identity alone is not authority: live owners provide both, and the
		//! projection publication hashes the conjunction.
		id<MTLBuffer> targetConsumerIdentity;
		//! r204 qualified owner placement. This selects lineage-token hashing,
		//! not projection arithmetic; standalone/legacy callers retain v1.
		bool qualifiedOwnerStageTokens=false;
		//! Optional private, device-produced pressure-open authorities. When
		//! present they replace the request's structural placeholder seals.
		id<MTLBuffer> sealedPressureOpenInflow;
		id<MTLBuffer> sealedPressureOpenDynamicPressurePa;

		FireProductionMetalProjectionResidentInput() : gasDensityKGPerM3(nil),
			divergenceTargetPerS(nil),targetPublicationIdentity(nil),targetConsumerIdentity(nil),
			sealedPressureOpenInflow(nil),sealedPressureOpenDynamicPressurePa(nil)
		{
			provisionalMomentumKGPerM2S.fill(nil);
			provisionalMomentumByteOffset.fill(0u);
		}
	};

	struct FireProductionMetalProjectionResidentState
	{
		id<MTLBuffer> pressurePa;
		id<MTLBuffer> pressureOpenInflow;
		//! Private device publication.  It hashes the consumed target identity and
		//! every projected full-grid result; it is never synthesized on the host.
		id<MTLBuffer> publicationIdentity;
		std::array<id<MTLBuffer>,3> faceDensityKGPerM3;
		std::array<id<MTLBuffer>,3> momentumKGPerM2S;
		std::array<id<MTLBuffer>,3> velocityMPerS;
		std::array<id<MTLBuffer>,3> provisionalMomentumKGPerM2S;
		std::array<std::size_t,3> provisionalMomentumByteOffset;
		std::array<std::size_t,3> momentumByteOffset;
		bool restoration;

		FireProductionMetalProjectionResidentState() : pressurePa(nil),
			pressureOpenInflow(nil),publicationIdentity(nil),restoration(false)
		{
			faceDensityKGPerM3.fill(nil);momentumKGPerM2S.fill(nil);
			velocityMPerS.fill(nil);provisionalMomentumKGPerM2S.fill(nil);
			provisionalMomentumByteOffset.fill(0u);momentumByteOffset.fill(0u);
		}
	};

	//! Internal full-grid Private-buffer seam for the composed resident P3 step.
	bool ProjectFireProductionMetalResident(
		const FireProductionProjectionRequest& request,
		const FireProductionMetalProjectionResidentInput& input,
		FireProductionProjectionResult& result,
		std::string* error=0 );

	//! Main physical projection without full-grid terminal staging. The returned
	//! momentum remains Private and is the sole admissible input to the resident
	//! restoration projection.
	bool ProjectFireProductionMetalResidentState(
		const FireProductionProjectionRequest& request,
		const FireProductionMetalProjectionResidentInput& input,
		FireProductionMetalProjectionResidentState& state,
		FireProductionProjectionResult& result,
		std::string* error=0 );

	//! Terminal resident restoration pass. Its target and validation result are
	//! owned by this invocation and are never shared with the physical pass.
	bool ProjectFireProductionMetalRestorationResident(
		const FireProductionProjectionRequest& request,
		const FireProductionMetalProjectionResidentInput& input,
		id<MTLBuffer> expectedRestorationTargetPerS,
		FireProductionProjectionResult& result,
		std::string* error=0 );

	//! Correction-only restoration solve that retains its complete Private
	//! result for an in-step scalar corrector. No full grid is staged here.
	bool ProjectFireProductionMetalRestorationResidentState(
		const FireProductionProjectionRequest& request,
		const FireProductionMetalProjectionResidentInput& input,
		id<MTLBuffer> expectedRestorationTargetPerS,
		FireProductionMetalProjectionResidentState& state,
		FireProductionProjectionResult& result,
		std::string* error=0 );

	//! Performs the sole terminal staging of an immutable retained restoration
	//! state. Publication and an in-step corrector may consume that state
	//! concurrently; both consumers complete before the owner mutates or
	//! releases any retained buffer.
	bool PublishFireProductionMetalRestorationResidentState(
		const FireProductionProjectionRequest& request,
		const FireProductionMetalProjectionResidentState& state,
		FireProductionProjectionResult& result,
		std::string* error=0 );
#endif

	//! Metal-resident implementation of the same fixed P2 schedule.  Platforms
	//! without Metal provide an honest fail-closed definition; there is no CPU
	//! fallback behind this entry point.
	bool ProjectFireProductionMetal(
		const FireProductionProjectionRequest& request,
		FireProductionProjectionResult& result,
		std::string* error=0 );

}

#endif
