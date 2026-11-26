#pragma once

#include "Utilities/ISampler.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace RISE::HyLIoS {

// slab-control-v1: an infinite six-layer absorption/dielectric control only.
// This is NOT the HYLIOS skin model: no particles, fibers, folds, or geometry.
inline constexpr const char* SlabControlVersion = "slab-control-v1";
struct SlabVector { double x, y, z; };
struct SlabLayer { double thicknessMetres, absorptionPerMetre, realIOR; };
enum class AbsorptionEstimator { Analog, Weighted };
enum class SlabSide { Exterior, Interior };
enum class BoundaryAction { Reflect, Transmit };
enum class SlabTermination {
    None, ExternalReflection, SubsurfaceReturn, Absorbed,
    RouletteKilled, ZeroWeight, InvalidSample, NumericalFailure
};
enum class SlabAdvanceKind { OuterBoundaryEncounter, YieldBudget, Terminal };
struct SlabBoundaryEncounter {
    std::uint64_t token;
    SlabSide incidentSide;
    SlabVector position, incidentDirection;
    double wavelengthNm, weight, incidentIOR, targetIOR;
    // Frame: outward normal (0,0,-1), tangents (1,0,0),(0,1,0).
    // Physical power convention: analog Fresnel R/T, no radiance eta factor.
};
struct SlabBoundaryDecision {
    BoundaryAction action;
    SlabSide targetSide;
    SlabVector direction;
    double weightMultiplier;
};
struct SmoothDielectricEvaluation {
    double reflectance;
    bool totalInternalReflection;
    SlabVector reflectedDirection, transmittedDirection;
};
// Finite nonzero travel directions are normalized scale-safely; exterior incident z >= 0,
// interior incident z <= 0. Invalid input throws invalid_argument.
SmoothDielectricEvaluation EvaluateSlabSmoothDielectric(const SlabBoundaryEncounter&);
SlabBoundaryDecision SampleSlabSmoothDielectric(const SlabBoundaryEncounter&, double uniform);

struct SlabCounters {
    std::uint64_t flights = 0, internalInterfaces = 0, bottomReflections = 0;
    std::uint64_t boundaryEncounters = 0, rouletteTrials = 0, rouletteKills = 0;
    double pathLengthMetres = 0;
    std::array<double, 6> layerPathLengthMetres{};
};
class HyLIoSSlabTransport;
class SlabState {
public:
    const SlabVector& Position() const { return position_; }
    const SlabVector& Direction() const { return direction_; }
    double WavelengthNm() const { return wavelengthNm_; }
    double Weight() const { return weight_; }
    int Layer() const { return layer_; } // -1 exterior, 0..5 owned interior layer
    const std::array<double, 6>& AbsorbedByLayer() const { return absorbed_; }
    const SlabCounters& Counters() const { return counters_; }
    SlabTermination Termination() const { return termination_; }
    bool HasPendingBoundary() const { return pending_; }
    const SlabBoundaryEncounter& PendingBoundary() const;
private:
    friend class HyLIoSSlabTransport;
    SlabState() = default;
    const HyLIoSSlabTransport* owner_ = nullptr;
    SlabVector position_{}, direction_{};
    double wavelengthNm_ = 0, weight_ = 1;
    int layer_ = -1;
    std::array<double, 6> absorbed_{};
    SlabCounters counters_{};
    SlabTermination termination_ = SlabTermination::None;
    SlabBoundaryEncounter encounter_{};
    bool pending_ = false;
    std::uint64_t nextToken_ = 0;
};
struct SlabAdvanceResult { SlabAdvanceKind kind; SlabTermination termination; };

class HyLIoSSlabTransport {
public:
    // All coefficients are already resolved at wavelengthNm. No spectral or
    // biological defaults. rouletteTargetWeight==0 disables roulette; otherwise
    // weighted survival below the target is promoted with p=weight/target.
    HyLIoSSlabTransport(std::array<SlabLayer, 6> layers, double wavelengthNm,
        double exteriorIOR, AbsorptionEstimator, double rouletteTargetWeight);
    HyLIoSSlabTransport(const HyLIoSSlabTransport&) = delete;
    HyLIoSSlabTransport& operator=(const HyLIoSSlabTransport&) = delete;
    HyLIoSSlabTransport(HyLIoSSlabTransport&&) = delete;
    HyLIoSSlabTransport& operator=(HyLIoSSlabTransport&&) = delete;
    // A state belongs to this immutable kernel instance, which must outlive it.
    SlabState Begin(SlabVector topPosition, SlabVector incomingDirection) const;
    // workQuantum counts complete segment/interface transitions. It is a latency
    // yield, never a physical cap. A pending event is returned even at quantum=0.
    SlabAdvanceResult Advance(SlabState&, ISampler&, std::size_t workQuantum) const;
    // Tokens are monotonic per history. Reuse/wrong tokens or inconsistent side,
    // hemisphere, zero/nonfinite direction, invalid multiplier throw invalid_argument
    // without consuming the pending event. Arbitrary proposals are the caller's
    // responsibility; SampleSlabSmoothDielectric is the physical analog helper.
    void ResumeBoundary(SlabState&, std::uint64_t token, const SlabBoundaryDecision&) const;
private:
    std::array<SlabLayer, 6> layers_;
    std::array<double, 7> planes_{};
    double wavelengthNm_, exteriorIOR_, rouletteTargetWeight_;
    AbsorptionEstimator estimator_;
    void Encounter(SlabState&, SlabSide) const;
    bool Flight(SlabState&, double distance, bool absorptionEvent) const;
    bool Roulette(SlabState&, ISampler&) const;
};
} // namespace RISE::HyLIoS
