#pragma once

#include "HyLIoSLocalResponse.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RISE::HyLIoS {
using LocalAssetRecord = std::vector<unsigned char>;

// CALLER PREPARATION OBLIGATION: construct these records from independently
// resolved actual model/data/specimen/exterior values, not asset producer hashes
// or preset names alone. Every record is a nonempty canonical RISE-CBOR64 map.
// Records must carry chosen versions, actual SI values, spectra/content hashes,
// organization/integerization, full source-input byte hashes, optical-data
// intersection/coverage, elastic/emission and scale conventions, and placement/
// discard laws where applicable. Data/exterior records must include the actual
// resolved spectral values/nodes, not only hashes or authored references.
// This codec checks canonical syntax and exact identity, NOT the scientific
// truth/completeness of these caller claims. The renderer/baker preparation
// layer must implement that obligation before this becomes a production asset.
struct LocalAssetSemanticIdentity {
    LocalAssetRecord model, placement, postscatter, boundary, lineage;
    LocalAssetRecord data, specimen, exterior, approximation;
    double coveredFirstNm = 0, coveredLastNm = 0;
};

// Supplying this declaration to Save also attests to a complete experiment:
// no unresolved/unclassified returned mass, all-history denominators, no escape
// conditioning, and statistics/evidence under the named estimator. These are
// producer obligations; file parsing cannot establish oracle history facts.
struct LocalAssetProductionIdentity {
    std::string oracleRevision, lineageRevision, estimatorRevision;
    std::string historyMappingRevision, statisticsRevision;
    std::uint64_t seed = 0, historiesPerCellPerReplicate = 0;
    std::uint64_t replicates = 0, normalGuideHistoriesPerReplicate = 0;
    std::uint64_t maximumWorkPerHistory = 0;
    // Declared producer records: actual executable/source/build identity,
    // and replicate/bin/atom/row uncertainty plus convergence evidence.
    // Canonical nonempty maps; the codec does not certify MC uncertainty.
    LocalAssetRecord build, statistics;
};

struct LocalAssetLimits {
    std::uint64_t maximumFileBytes = std::uint64_t{256} * 1024 * 1024;
    std::uint64_t maximumResidentBytes = std::uint64_t{1024} * 1024 * 1024;
    std::size_t maximumMetadataBytes = 64 * 1024;
    LocalResponseLimits response;
};

struct LocalAssetEncodingError {
    double maximumCoefficientAbsoluteError = 0;
    // Upper bound on nodal forward row loss from downward encoding only.
    // Distinct from MC/discretization error. Loader treats it as a producer
    // claim; originals are unavailable in the persisted representation.
    double maximumForwardRowAbsoluteError = 0;
};

struct LocalAssetDefinition {
    LocalResponseDefinition response;
    LocalAssetSemanticIdentity semantic;
    LocalAssetProductionIdentity production;
    // Separate normal-incidence experiment, one hemispherical reflectance
    // per EXACT response wavelength node; never the last incident-cell center.
    // Coefficients are estimates under the declared production statistics.
    std::vector<double> normalIncidenceGuide;
};

class HyLIoSLocalResponseAsset final {
public:
    const HyLIoSLocalResponse& Response() const noexcept { return *response_; }
    const LocalAssetSemanticIdentity& Semantic() const noexcept { return semantic_; }
    const LocalAssetProductionIdentity& Production() const noexcept { return production_; }
    const std::vector<double>& NormalIncidenceGuide() const noexcept { return guide_; }
    const std::vector<double>& WavelengthsNm() const noexcept { return wavelengths_; }
    const LocalAssetEncodingError& EncodingError() const noexcept { return encodingError_; }
    // Hash of exact framed content preceding the digest trailer, independent
    // of filename. Semantic hash covers its exact canonical fields. Production
    // hash covers both semantic and production fields, plus the actual
    // grids/nodes and encoding-error record: the complete declared experiment.
    const std::string& ContentSHA256() const noexcept { return contentHash_; }
    const std::string& SemanticSHA256() const noexcept { return semanticHash_; }
    const std::string& ProductionSHA256() const noexcept { return productionHash_; }
    std::uint64_t PeakResidentBytesBound() const noexcept { return residentBound_; }
    HyLIoSLocalResponseAsset(const HyLIoSLocalResponseAsset&) = delete;
    HyLIoSLocalResponseAsset& operator=(const HyLIoSLocalResponseAsset&) = delete;
private:
    HyLIoSLocalResponseAsset() = default;
    friend std::shared_ptr<const HyLIoSLocalResponseAsset> LoadLocalResponseAsset(
        const std::string&, const LocalAssetSemanticIdentity&, const LocalAssetLimits&);
    std::shared_ptr<const HyLIoSLocalResponse> response_;
    LocalAssetSemanticIdentity semantic_;
    LocalAssetProductionIdentity production_;
    LocalAssetEncodingError encodingError_;
    std::vector<double> guide_, wavelengths_;
    std::string contentHash_, semanticHash_, productionHash_;
    std::uint64_t residentBound_ = 0;
};

// Paths are UTF8 on every platform. The shared legacy native-string I/O APIs
// retain their existing pathname contract.
// Exceptions are errors. Return values and destination files change only after
// complete validation; loading ALWAYS requires an independently prepared expected
// semantic identity. This scope supplies no render/context aggregate cache.
std::shared_ptr<const HyLIoSLocalResponseAsset> LoadLocalResponseAsset(
    const std::string& path, const LocalAssetSemanticIdentity& expected,
    const LocalAssetLimits& limits = {});
void SaveLocalResponseAsset(const std::string& path, const LocalAssetDefinition&,
    const LocalAssetLimits& limits = {});

namespace LocalAssetDetail {
// Private per-call test seam at the real atomic helper's pre-publication
// boundary. Throwing refuses publication and preserves the prior destination.
void SaveObserved(const std::string&, const LocalAssetDefinition&, const LocalAssetLimits&,
    const std::function<void()>& beforePublication);
}
} // namespace RISE::HyLIoS
