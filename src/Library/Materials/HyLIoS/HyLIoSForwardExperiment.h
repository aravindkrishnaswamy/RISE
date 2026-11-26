#pragma once
#include "HyLIoSForwardTransport.h"
#include "HyLIoSSurfaceBoundary.h"
namespace RISE::HyLIoS {
// SplitMix64, top 53 bits to [0,1). Seed + checked unique history index
// (modulo 2^64) defines the reproducible measurement history seed mapping.
class ForwardExperimentRandom final : public ISampler {
public:
    explicit ForwardExperimentRandom(std::uint64_t seed);
    double Get1D() override;
    Point2 Get2D() override;
    std::uint64_t state, draws = 0;
};
struct ForwardSurfaceWork {
    std::uint64_t workUnits=0, branchAttempts=0, normalProposalAttempts=0, normalRejections=0;
    std::uint64_t exteriorReflectionBranches=0, exteriorTransmissionBranches=0;
    std::uint64_t interiorReflectionBranches=0, interiorTransmissionBranches=0;
    std::uint64_t readyDecisions=0, budgetStops=0, cancellations=0;
    std::uint64_t invalidSamples=0, numericalFailures=0;
};
// Callbacks run synchronously outside any signal handler. A signal handler may
// set volatile sig_atomic_t; isCancelled can read that flag. Context lifetime
// must cover the call. A true cancellation result is latched for this call.
// Exceptions from callbacks propagate to the caller.
struct ForwardExperimentHooks {
    void* context = nullptr;
    bool (*isCancelled)(void*) = nullptr;
    void (*onFoldReflection)(void*) = nullptr;
};
// On normal return, drives the state to a terminal outcome while preserving
// measurement-v6 draw/dispatch order. Each fold branch/proposal attempt counts
// one surface work unit; smooth retains v6's uncharged dispatch. Bulk yields
// are 64 quanta. No shared mutable state. Fold boundary sampling/application
// errors cancel the transport and return an explicit diagnostic. Smooth boundary
// sampling errors propagate as exceptions. Entry validation and hook exceptions
// also propagate; an exception can leave a pending, nonterminal state. The caller
// must abort/reject the entire experiment on any propagated exception rather
// than treating it as a completed history or a returned terminal diagnostic.
// Supply fresh zero counters for each history; law==nullptr selects smooth.
ForwardTermination DriveForwardExperimentHistory(const HyLIoSForwardTransport&,
    const HyLIoSSurfaceBoundary* law, ForwardState&, ISampler&, std::uint64_t workLimit,
    ForwardSurfaceWork&, const ForwardExperimentHooks& = {});
}
