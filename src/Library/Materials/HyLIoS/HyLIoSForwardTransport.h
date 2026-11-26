#pragma once
#include "HyLIoSSlabTransport.h"
#include "HyLIoSParticleGeometry.h"
#include "HyLIoSComplexLayout.h"
#include <optional>

namespace RISE::HyLIoS {
// Forward-only clean-room interpretation: individual spheroids or explicit
// finite spatial complexes, resolved monochromatic coefficients; no adjoint PDF.
inline constexpr const char* ForwardTransportVersion = "hylios-forward-explicit-complex-draft-v2";
// Both policies sample the same nominal geometric encounter rate. Fixed-event
// resamples geometry at one frozen event and can remain unplaceable indefinitely.
// Null-collision consumes one rejected candidate as a host-only flight event;
// its accepted volume fraction is NOT asserted equal to the authored fraction.
enum class ForwardPlacementPolicy { FixedEventDiagnosticV1, NullCollisionPlacementV1 };
// Diagnostic keeps an inward post-exit direction unresolved. Discard is the
// explicitly versioned virtual-particle model: the full angular law is kept,
// and only predecessor exclusion retains the accepted particle's geometry.
enum class ForwardPostScatterPolicy { DiagnosticRetainParticleV1, DiscardAfterScatterV1 };
inline constexpr const char* ForwardDiagnosticPostScatterVersion = "hylios_diagnostic_retain_particle_v1";
inline constexpr const char* ForwardDiscardPostScatterVersion = "hylios_discard_after_scatter_v1";
enum class ForwardOrganization { IndividualSpheroids, UnsupportedComplexes, ExplicitComplexPackV1 };
struct ForwardOrganelle {
    ForwardOrganization organization;
    double volumeFraction, semiMinorMetres, semiMajorMetres;
    double absorptionPerMetre, realIOR;
    // Required for explicit complexes; zero defaults preserve individual inputs.
    double envelopeRadiusMetres=0;
    std::size_t childCount=0;
};
struct ForwardLayer {
    double thicknessMetres, hostAbsorptionPerMetre, realIOR;
    std::optional<ForwardOrganelle> organelle;
};
enum class ForwardTermination {
    None, ExternalReflection, SubsurfaceReturn, Absorbed, RouletteKilled,
    ZeroWeight, Cancelled, InvalidSample, NumericalFailure,
    UnplaceableBoundaryCandidate, ForwardPerturbationReentry, BudgetExceeded
};
enum class ForwardFailureDetail { None, OverlapIndeterminate, OverlapInvalidInput, OverlapUnsupportedRange, ArithmeticOrGeometry };
enum class ForwardPhase { BulkFlight, CandidatePlacement, ParticleChord,
    ReflectedPerturbation, ForwardPerturbation, FiberPerturbation, ComplexLayout, ComplexHostFlight };
enum class ForwardAdvanceKind { OuterBoundaryEncounter, YieldBudget, Terminal };
struct ForwardAdvanceResult { ForwardAdvanceKind kind; ForwardTermination termination; };
struct ForwardCounters {
    std::uint64_t workUnits=0, nullPlacements=0, flights=0, internalInterfaces=0, bottomReflections=0;
    std::uint64_t boundaryEncounters=0, rouletteTrials=0, rouletteKills=0;
    std::uint64_t particleCandidates=0, placementAttempts=0, containmentRejections=0, overlapRejections=0;
    std::uint64_t acceptedParticles=0, particleReflections=0, particleExits=0, inwardPostExitEvents=0;
    std::uint64_t fiberEvents=0, fiberPasses=0, perturbationProposals=0;
    // acceptedParticles counts outer objects; child events are not outer accepts.
    std::uint64_t acceptedComplexes=0, complexExits=0, complexChildEntries=0;
    std::uint64_t complexChildExits=0, complexChildReflections=0, complexHostSegments=0;
    std::uint64_t complexLayoutProposals=0, complexLayoutContainmentRejections=0;
    std::uint64_t complexLayoutOverlapRejections=0, complexLayoutRestarts=0, complexLayoutPairChecks=0;
    double pathLengthMetres=0;
    std::array<double,6> layerPathLengthMetres{}, hostPathLengthMetres{}, particlePathLengthMetres{};
};
class HyLIoSForwardTransport;
class ForwardState {
public:
    const SlabVector& Position() const { return position_; }
    const SlabVector& Direction() const { return direction_; }
    double Weight() const { return weight_; }
    double WavelengthNm() const { return wavelengthNm_; }
    int Layer() const { return layer_; }
    ForwardPhase Phase() const { return phase_; }
    ForwardTermination Termination() const { return termination_; }
    ForwardFailureDetail FailureDetail() const { return failureDetail_; }
    const ForwardCounters& Counters() const { return counters_; }
    const std::array<double,6>& HostAbsorbedByLayer() const { return hostAbsorbed_; }
    const std::array<double,6>& ParticleAbsorbedByLayer() const { return particleAbsorbed_; }
    bool FiberUsedThisPass() const { return fiberUsed_; }
    bool HasPendingBoundary() const { return pending_; }
    const SlabBoundaryEncounter& PendingBoundary() const;
private:
    friend class HyLIoSForwardTransport;
    ForwardState()=default;
    const HyLIoSForwardTransport* owner_=nullptr;
    SlabVector position_{},direction_{};
    double wavelengthNm_=0,weight_=1;
    int layer_=-1;
    ForwardPhase phase_=ForwardPhase::BulkFlight;
    ForwardTermination termination_=ForwardTermination::None;
    ForwardFailureDetail failureDetail_=ForwardFailureDetail::None;
    ForwardCounters counters_{};
    std::array<double,6> hostAbsorbed_{},particleAbsorbed_{};
    bool pending_=false,fiberUsed_=false,hasPredecessor_=false;
    std::uint64_t nextToken_=0;
    SlabBoundaryEncounter encounter_{};
    ProlateParticle particle_{},predecessor_{};
    ParticleVector particlePoint_{},particleExit_{},exitNormal_{},predecessorCentreFromPosition_{};
    double chordDistance_=0;
    SlabVector perturbationAxis_{};
    std::optional<ComplexLayout> complexLayout_;
    ProlateParticle envelope_{};
    ParticleVector complexPoint_{};
    bool complexActive_=false,complexEntryChord_=false;
    std::size_t activeChild_=ComplexLayout::MaximumChildren, lastExitedChild_=ComplexLayout::MaximumChildren;
};
class HyLIoSForwardTransport {
public:
    // Layer order SC,SG,SS,SB,PD,RD; fiber rate applies exclusively to PD (4).
    // Every value is supplied; no biological coefficient defaults. Angular taper
    // is the named 1400-nm endpoint interpretation in HYLIOS_EQUATIONS.md §4.
    HyLIoSForwardTransport(std::array<ForwardLayer,6>, double wavelengthNm,
        double exteriorIOR, double papillaryFiberRatePerMetre,
        AbsorptionEstimator, double rouletteTargetWeight, ForwardPlacementPolicy, ForwardPostScatterPolicy);
    HyLIoSForwardTransport(const HyLIoSForwardTransport&)=delete;
    HyLIoSForwardTransport& operator=(const HyLIoSForwardTransport&)=delete;
    HyLIoSForwardTransport(HyLIoSForwardTransport&&)=delete;
    HyLIoSForwardTransport& operator=(HyLIoSForwardTransport&&)=delete;
    // Local launch origin must be (0,0,0); adapters retain scene translation.
    ForwardState Begin(SlabVector topPosition, SlabVector incomingDirection) const;
    // Each proposal/segment consumes one quantum. Pending boundary is returned at
    // quantum=0. Yields preserve all candidate/internal/rejection state and RNG.
    ForwardAdvanceResult Advance(ForwardState&, ISampler&, std::size_t quantum) const;
    void ResumeBoundary(ForwardState&,std::uint64_t token,const SlabBoundaryDecision&) const;
    void Cancel(ForwardState&) const;
    // Explicit caller hard cap: unresolved power, distinct from cancellation.
    void StopForBudget(ForwardState&) const;
private:
    std::array<ForwardLayer,6> layers_;
    std::array<double,7> planes_{};
    std::array<double,6> geometricRates_{};
    double wavelengthNm_,exteriorIOR_,fiberRate_,rouletteTarget_,forwardAngle_;
    AbsorptionEstimator estimator_;
    ForwardPlacementPolicy placementPolicy_;
    ForwardPostScatterPolicy postScatterPolicy_;
    void Encounter(ForwardState&,SlabSide) const;
    bool Flight(ForwardState&,double,double,bool,bool) const;
    bool Roulette(ForwardState&,ISampler&) const;
    void Bulk(ForwardState&,ISampler&) const;
    void Placement(ForwardState&,ISampler&) const;
    void ParticleFlight(ForwardState&,ISampler&) const;
    void Perturb(ForwardState&,ISampler&) const;
    void BuildComplex(ForwardState&,ISampler&) const;
    void ComplexHost(ForwardState&,ISampler&) const;
    void FinishForwardScatter(ForwardState&) const;
};
}
