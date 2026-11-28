#include "pch.h"
#include "HyLIoSForwardExperiment.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
namespace RISE::HyLIoS {
ForwardExperimentRandom::ForwardExperimentRandom(std::uint64_t seed):state(seed) {}
double ForwardExperimentRandom::Get1D() {
    ++draws;
    state += 0x9e3779b97f4a7c15ULL;
    auto z=state;
    z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL;
    z=(z^(z>>27))*0x94d049bb133111ebULL;
    z^=z>>31;
    return static_cast<double>(z>>11)*0x1p-53;
}
Point2 ForwardExperimentRandom::Get2D() {return {Get1D(),Get1D()};}
namespace {
class Cancellation {
public:
    explicit Cancellation(const ForwardExperimentHooks& hooks):hooks_(hooks) {}
    bool operator()() {
        if(!observed_&&hooks_.isCancelled) observed_=hooks_.isCancelled(hooks_.context);
        return observed_;
    }
private:
    const ForwardExperimentHooks& hooks_;
    bool observed_=false;
};
struct InvalidBoundarySample : std::runtime_error {
    InvalidBoundarySample():std::runtime_error("invalid used surface-boundary random variate") {}
};
double BoundaryDraw(RISE::ISampler& rng) {
    const double u=rng.Get1D();
    if(!std::isfinite(u)||u<0||u>=1) throw InvalidBoundarySample();
    return u;
}
// The adapter owns the outer law, while the transport owns all live power and
// absorption. Boundary diagnostics therefore cancel the pending transport and
// explicitly override only the report's terminal classification below.
ForwardTermination RunFoldBoundaryHistory(const HyLIoSForwardTransport& kernel,
    const HyLIoSSurfaceBoundary& law,ForwardState& state,RISE::ISampler& rng,
    std::uint64_t limit,ForwardSurfaceWork& counters,const ForwardExperimentHooks& hooks) {
    Cancellation cancelled(hooks);
    std::optional<SurfaceBoundaryState> pending;
    ForwardTermination diagnostic=ForwardTermination::None;
    const auto StopBoundary=[&](bool cancel) {
        if(!pending||(pending->Phase()!=SurfaceBoundaryPhase::Branch&&pending->Phase()!=SurfaceBoundaryPhase::NormalProposal)) return;
        if(cancel) {law.Cancel(*pending);++counters.cancellations;}
        else {law.StopForBudget(*pending);++counters.budgetStops;}
    };
    while(state.Termination()==ForwardTermination::None&&
        state.Counters().workUnits<limit&&
        counters.workUnits<limit-state.Counters().workUnits&&!cancelled()) {
        if(!state.HasPendingBoundary()) {
            kernel.Advance(state,rng,static_cast<std::size_t>(std::min<std::uint64_t>(64,
                limit-state.Counters().workUnits-counters.workUnits)));
            continue;
        }
        bool notifyFoldReflection=false;
        try {
            if(!pending) pending.emplace(law.Begin(state.PendingBoundary()));
            const bool branch=pending->Phase()==SurfaceBoundaryPhase::Branch;
            const auto side=pending->Encounter().incidentSide;
            // Caller work counts every attempted transition, including failures.
            ++counters.workUnits;
            if(branch) ++counters.branchAttempts;else ++counters.normalProposalAttempts;
            const double u=BoundaryDraw(rng);
            double v=0,acceptance=0;
            if(!branch) {v=BoundaryDraw(rng);acceptance=BoundaryDraw(rng);}
            const auto phase=law.TrySample(*pending,u,v,acceptance);
            if(branch) {
                const bool reflected=phase==SurfaceBoundaryPhase::NormalProposal||pending->Decision().action==BoundaryAction::Reflect;
                if(side==SlabSide::Exterior) {
                    if(reflected) ++counters.exteriorReflectionBranches;else ++counters.exteriorTransmissionBranches;
                } else {
                    if(reflected) ++counters.interiorReflectionBranches;else ++counters.interiorTransmissionBranches;
                }
                notifyFoldReflection=phase==SurfaceBoundaryPhase::NormalProposal;
            } else if(phase==SurfaceBoundaryPhase::NormalProposal) ++counters.normalRejections;
            if(phase==SurfaceBoundaryPhase::Ready) {
                ++counters.readyDecisions;
                kernel.ResumeBoundary(state,pending->Encounter().token,pending->Decision());
                pending.reset();
            }
        } catch(const InvalidBoundarySample&) {
            ++counters.invalidSamples;diagnostic=ForwardTermination::InvalidSample;
            StopBoundary(true);kernel.Cancel(state);
        } catch(const std::exception&) {
            ++counters.numericalFailures;diagnostic=ForwardTermination::NumericalFailure;
            StopBoundary(true);kernel.Cancel(state);
        }
        if(notifyFoldReflection&&hooks.onFoldReflection) hooks.onFoldReflection(hooks.context);
    }
    if(state.Termination()==ForwardTermination::None) {
        if(cancelled()) {StopBoundary(true);kernel.Cancel(state);}
        else {StopBoundary(false);kernel.StopForBudget(state);}
    }
    return diagnostic;
}
}
ForwardTermination DriveForwardExperimentHistory(const HyLIoSForwardTransport& kernel,
    const HyLIoSSurfaceBoundary* law, ForwardState& state, ISampler& rng,
    std::uint64_t limit, ForwardSurfaceWork& counters, const ForwardExperimentHooks& hooks) {
    // Zero quantum validates owner and variable-dimension sampler before any
    // pending boundary can bypass the bulk entry point. It draws no samples.
    kernel.Advance(state,rng,0);
    if(law) return RunFoldBoundaryHistory(kernel,*law,state,rng,limit,counters,hooks);
    Cancellation cancelled(hooks);
        // Keep smooth's legacy RNG, cap and boundary-dispatch ordering exact.
        while (state.Termination() == ForwardTermination::None &&
               state.Counters().workUnits < limit && !cancelled()) {
            const auto result = kernel.Advance(state, rng,
                                               static_cast<std::size_t>(std::min<std::uint64_t>(
                                                   64, limit - state.Counters().workUnits)));
            if (result.kind == ForwardAdvanceKind::OuterBoundaryEncounter) {
                const auto event = state.PendingBoundary();
                kernel.ResumeBoundary(state, event.token,
                                      SampleSlabSmoothDielectric(event, rng.Get1D()));
            }
        }
        if (state.Termination() == ForwardTermination::None) {
            if (cancelled())
                kernel.Cancel(state);
            else
                kernel.StopForBudget(state);
        }

    return ForwardTermination::None;
}
}
