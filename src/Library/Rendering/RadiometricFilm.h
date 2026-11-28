// Raw fixed-wavelength radiometric accumulation primitives.
// License: see the attached LICENSE.TXT file.
#ifndef RISE_RADIOMETRIC_FILM_H
#define RISE_RADIOMETRIC_FILM_H
#include "../Utilities/RISECBOR64.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace RISE { namespace Implementation {
struct RadiometricLimits {
    std::size_t maxPixelBytes = 256u * 1024u * 1024u;
    std::size_t maxCheckpointBytes = 512u * 1024u * 1024u;
    std::size_t maxProvenanceBytes = 16384u;
};
struct RadiometricIdentity {
    std::uint32_t width = 0, height = 0;
    double wavelengthNM = 0;
    double pixelAspectRatio = 1;
    std::uint64_t targetSamples = 0, seed = 0;
    // Caller-defined, versioned indexed-trial RNG algorithm. Primitives generate no RNG.
    std::string seedAlgorithm;
    // Canonical CBOR map of resolved configuration/source/build identities, supplied
    // by the caller. Storing this record does not certify source calibration.
    RISECBOR64::Bytes provenance;
};
enum class RadiometricStatus { Running, Cancelled, Complete, Invalid };
struct RadiometricPixel {
    std::uint64_t count = 0, nextSampleIndex = 0;
    double mean = 0, m2 = 0;
    // Normalized hi/lo compensated mean and M2. Residuals are persisted so
    // recentering preserves adjacent fluctuations and repeated small trends.
    double meanCorrection = 0, m2Correction = 0;
};
struct RadiometricStatistics {
    double mean = 0, sampleVariance = 0, meanVariance = 0;
    bool varianceAvailable = false;
};
struct RadiometricSnapshot {
    RadiometricIdentity identity;
    RadiometricStatus status = RadiometricStatus::Running;
    std::vector<RadiometricPixel> pixels;
    std::string failure;
};
bool EncodeRadiometricIdentity(const RadiometricIdentity&, RISECBOR64::Bytes&,
    std::string& error, const RadiometricLimits& = {});
bool RadiometricPixelStatistics(const RadiometricPixel&, RadiometricStatistics&, std::string& error);
bool ValidateRadiometricSnapshot(const RadiometricSnapshot&, std::string& error,
    const RadiometricLimits& = {});

// Serialized operations deliberately favor correctness over throughput in this
// primitive slice. Snapshot/commit/cancel are mutually exclusive. Renderer adapters
// may batch commits later without changing the atomic per-trial contract.
class RadiometricFilm final {
    mutable std::mutex mutex;
    RadiometricSnapshot state;
    std::size_t completedPixels = 0;
    RadiometricLimits limits;
    explicit RadiometricFilm(RadiometricSnapshot snapshot, RadiometricLimits limits_);
    bool Fail(const std::string&, std::string& error);
public:
    static std::unique_ptr<RadiometricFilm> Create(const RadiometricIdentity&,
        std::string& error, const RadiometricLimits& = {});
    static std::unique_ptr<RadiometricFilm> Restore(const RadiometricSnapshot&,
        std::string& error, const RadiometricLimits& = {});
    // value is L_lambda in W m^-2 sr^-1 nm^-1, already fully compensated
    // for path sampling. At fixed wavelength there is NO wavelength/bin factor.
    // Zero trials count. Failed commits invalidate the whole measurement and
    // leave that pixel's count/index/statistics unchanged.
    bool Commit(std::uint32_t x, std::uint32_t y, std::uint64_t sampleIndex,
        double value, std::string& error);
    void Cancel();
    bool Resume(const RadiometricIdentity& expected, std::string& error);
    void Invalidate(const std::string& reason);
    RadiometricSnapshot Snapshot() const;
};
} }
#endif
