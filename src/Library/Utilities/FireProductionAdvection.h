//////////////////////////////////////////////////////////////////////
//
//  FireProductionAdvection.h - fp32 conservative production remap
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FIREPRODUCTIONADVECTION_
#define FIREPRODUCTIONADVECTION_

#include <cstddef>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace RISE
{
	//! r204 byte-payload digest namespace (distinct from historical manifold
	//! token digest variants). SHA-256 Merkle tree: 4096-byte leaves, fan-in 16.
	struct FireProductionPayloadDigestV2
	{
		std::uint32_t digestVersion=2u;
		std::uint32_t chunkBytes=4096u;
		std::uint32_t fanIn=16u;
		std::uint64_t payloadBytes=0u;
		std::string rootSHA256;
	};
	//! Ordered producer/FCT/force/projection/payload library source identities.
	std::string FireProductionOwnerKernelSetSHA256(const std::array<std::string,5>& sources);
	//! Parallelism is execution metadata, never part of the digest preimage.
	bool FireProductionPayloadDigestCPU(const unsigned char* bytes,std::size_t count,
		unsigned int parallelism,FireProductionPayloadDigestV2& result,std::string* error=0);
	//! Qualification entry; live producers use the same kernels on private
	//! resident buffers, without a CPU-produced digest authority.
	bool FireProductionPayloadDigestMetal(const std::vector<unsigned char>& bytes,
		unsigned int dispatchWidth,FireProductionPayloadDigestV2& result,
		double& deviceMS,std::string* error=0,unsigned int qualificationFailTreeAllocation=0u);

	enum FireProductionRemapBoundary
	{
		FireProductionRemapPeriodic,
		FireProductionRemapPressureOpen,
		FireProductionRemapWall
	};

	//! One batch of logically independent uniform-grid lines. Values are SoA:
	//! [component][line][cell], velocities are [line][face], ambient is one
	//! authored value per component.
	struct FireProductionRemapRequest
	{
		std::size_t lineLength;
		std::size_t lineCount;
		std::size_t componentCount;
		float cellWidthM;
		float timeStepS;
		FireProductionRemapBoundary boundary;
		bool asymmetricBoundaries;
		FireProductionRemapBoundary lowerBoundary;
		FireProductionRemapBoundary upperBoundary;
		std::vector<float> values;
		std::vector<float> faceVelocityMPerS;
		std::vector<float> ambientValues;
		bool lineSpecificAmbientValues;
		std::vector<float> lowerAmbientValues;
		std::vector<float> upperAmbientValues;

		FireProductionRemapRequest() : lineLength(0), lineCount(0),
			componentCount(0), cellWidthM(0.0f), timeStepS(0.0f),
			boundary(FireProductionRemapPeriodic), asymmetricBoundaries(false),
			lowerBoundary(FireProductionRemapPeriodic),
			upperBoundary(FireProductionRemapPeriodic),lineSpecificAmbientValues(false) {}
	};

	struct FireProductionRemapResult
	{
		std::vector<float> updatedValues;
		std::vector<float> faceFluxes;
		std::vector<float> sharedLimiterAlpha;
		double deviceElapsedMS;

		FireProductionRemapResult() : deviceElapsedMS(0.0) {}
	};

	//! Outward-rounded Metal allocation bytes for the request's buffer topology.
	bool FireProductionRemapWorkingSetBytes(
		const FireProductionRemapRequest& request,
		std::uint64_t& bytes );

	bool ValidateFireProductionRemapRequest(
		const FireProductionRemapRequest& request,
		std::string* error=0 );

	//! r124 continuous common-alpha cap. Exposed for independent contract gates.
	float FireProductionContinuousSharedLimiterAlpha(
		float alpha, float headroom, float signedConsumption,
		float center, float envelope ) noexcept;

	//! r127 continuous open-boundary donor transition. Exposed for contract gates.
	float FireProductionContinuousInflowValue(
		float nearest, float ambient, float velocity, bool positiveInflow,
		float cellWidthM, float timeStepS ) noexcept;

	//! Binary32 CPU oracle with the same stored intermediates as the Metal path.
	bool RemapFireProductionCPU(
		const FireProductionRemapRequest& request,
		FireProductionRemapResult& result,
		std::string* error=0 );

	//! Metal implementation. Non-Metal builds fail explicitly without CPU fallback.
	bool RemapFireProductionMetal(
		const FireProductionRemapRequest& request,
		FireProductionRemapResult& result,
		std::string* error=0 );
}

#endif
