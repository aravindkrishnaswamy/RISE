#pragma once
#include "HyLIoSForwardExperiment.h"
#include "HyLIoSResolvedExperiment.h"
#include "HyLIoSLocalResponseAsset.h"
#include <stdexcept>
namespace RISE::HyLIoS {
inline constexpr const char* LocalBakeVersion="hylios_local_bake_analog_projected_v2";
struct LocalBakeConfig {
    LocalResponseGrid incident{{0,.5,1},{0,LocalResponseTwoPi/2,LocalResponseTwoPi}};
    LocalResponseGrid outgoing{{0,.5,1},{0,LocalResponseTwoPi/2,LocalResponseTwoPi}};
    std::vector<double> wavelengthsNm;
    std::uint64_t seed=0, historiesPerCellPerReplicate=64, replicates=2;
    std::uint64_t normalGuideHistoriesPerReplicate=64, maximumWorkPerHistory=10000;
    // All cardinalities and the total worst-case oracle work are checked before RNG.
    std::uint64_t maximumTotalHistories=10000000, maximumTotalWork=1000000000;
    std::uint64_t maximumReportBytes=64*1024*1024, maximumSourceArtifactBytes=64*1024*1024;
    ExperimentBoundary boundary=ExperimentBoundary::Smooth;
    ForwardPlacementPolicy placement=ForwardPlacementPolicy::NullCollisionPlacementV1;
    ForwardPostScatterPolicy postScatter=ForwardPostScatterPolicy::DiscardAfterScatterV1;
    bool explicitComplexPackV1=false;
    double exteriorIOR=1, fiberRadiusMetres=1e-7, fiberFraction=.22, fiberRelativeIOR=1.5/1.33;
    // Caller-selected artifact, independently read and hashed. The caller must
    // identify the relevant build. Its closed record requires bounded compiler
    // text, lowercase SHA256 syntax and a true strict-FP declaration; these
    // structural claims are not execution attestation. The CLI
    // resolves its own executable path.
    std::string buildArtifactPath;
    LocalAssetLimits limits;
};
struct LocalBakeExecution {
    ForwardExperimentHooks hooks;
    // Scheduling only: cannot change history IDs, estimator, or production key.
    std::uint64_t historyChunkSize=64;
    bool reverseTraversal=false;
};
struct LocalBakeResult {
    LocalAssetDefinition asset;
    // Canonical CBOR report: exact replicate/bin/atom/absorption/work counts,
    // binomial history SE and independent replicate SE, and estimator error bounds.
    LocalAssetRecord report;
    // Exact immutable source bytes, including full upstream provenance.
    LocalAssetRecord sourceArtifact;
    std::uint64_t completedHistories=0, bulkWork=0, surfaceWork=0;
};
struct LocalBakeFailure : std::runtime_error {
    LocalBakeFailure(const std::string&,std::uint64_t index,std::uint64_t completed,
        std::uint64_t bulk,std::uint64_t surface,ForwardTermination);
    std::uint64_t historyIndex,completedHistories,bulkWork,surfaceWork;
    ForwardTermination termination;
};
// Same independently resolved identity builder for requested runtime/bake config.
// Capacity follows exact compact canonical record sizes (<=4096 bytes each),
// the response limits, and the 64KiB total metadata profile. No fixed 8-node cap.
LocalAssetSemanticIdentity PrepareLocalBakeSemantic(const std::string& sourceBytes,const LocalBakeConfig&);
LocalBakeResult BakeLocalResponse(const std::string& sourceBytes,const LocalBakeConfig&,const LocalBakeExecution& = {});
// Immutable content-addressed exact source and report are verified/published
// before the asset. Full source provenance lives outside bounded metadata.
// Metadata syntax/size and aggregate decoder resident budgets are admitted
// before generic metadata allocation. Failed preparation/history never reaches
// this function. Existing assets retain
// their old source/report dependencies if asset publication fails.
void PublishLocalBake(const std::string& assetPath,const LocalBakeResult&,const LocalAssetLimits& = {},
    const std::function<void()>& beforeAssetPublication = {});
// Admits bounded metadata scans/aggregate decoder storage, then validates exact
// source/semantic rederivation, closed complete report rows,
// count conservation, derived statistics/work, all raw/quantized coefficient
// and guide values, and codec errors. This is internal consistency, not MC or
// selected-artifact execution attestation. Requires strict binary64 arithmetic,
// round-to-nearest and no contraction; cross-compiler last-bit portability of
// derived statistics is not certified. Use this for bake-produced bundles.
std::shared_ptr<const HyLIoSLocalResponseAsset> LoadLocalBake(const std::string& assetPath,
    const LocalAssetSemanticIdentity&,const LocalAssetLimits& = {});
namespace LocalBakeDetail {
SlabVector Launch(LocalResponsePoint);
LocalResponsePoint Escaped(SlabVector);
double DownwardCountRatio(std::uint64_t count,std::uint64_t denominator);
double DownwardCoefficient(std::uint64_t count,std::uint64_t denominator,double publicArea);
}
}
