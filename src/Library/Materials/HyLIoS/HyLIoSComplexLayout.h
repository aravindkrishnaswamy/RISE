#pragma once
#include "HyLIoSParticleOverlap.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace RISE::HyLIoS {
// Versioned clean-room construction, not the authors' spatial model. The integer
// counts actual children, never interactions. No ray or transport state is used.
// Numerical revision v2 retains the isotropic ideal law and five-variates
// proposal contract, preserving transverse axes from the original polar u.
inline constexpr const char* ComplexLayoutVersion="hylios_complex_explicit_pack_v2";
struct ComplexChild {
    ProlateParticle particle;
    ParticleVector centreFromEnvelopeMetres;
};
enum class ComplexProposalKind {
    AcceptedChild, Complete, RejectedContainment, RejectedOverlap,
    RestartedLayout, InvalidSample, NumericalFailure
};
struct ComplexProposalResult {
    ComplexProposalKind kind;
    ParticleOverlapStatus geometryStatus=ParticleOverlapStatus::Disjoint;
};
struct ComplexLayoutCounters {
    std::uint64_t proposals=0, containmentRejections=0, overlapRejections=0;
    std::uint64_t restarts=0, overlapChecks=0;
};
class ComplexLayout {
public:
    // Resource limit: bounds per-proposal overlap work and live layout memory.
    // Larger requested layouts are rejected before allocation, never truncated.
    static constexpr std::size_t MaximumChildren=64;
    // All units metres. Supports zero children for the exact empty-envelope
    // control. Centre support is the largest binary64 d <= exact (R-b), giving
    // conservative bounding-sphere containment, not the full spheroid support.
    // Exact necessary N*a*a*b <= R*R*R volume check; no packing guarantee.
    // Dimension spread relative to R must be <=100 binary exponents.
    // At R==b only zero/one children are supported, at centre zero.
    // Each child has at most 256 proposals before a rejected proposal clears the
    // entire partial layout. Rejection and restart never absorb or consume power.
    ComplexLayout(double envelopeRadiusMetres,double childMinorMetres,
        double childMajorMetres,std::size_t childCount);
    // One call is ONE proposal and uses all five supplied independent U[0,1)
    // samples: centre x/y/z, axis cosine, axis azimuth. No hidden retries/RNG.
    // Completed layouts return Complete without inspecting samples/counters.
    // Numerical uncertainty is sticky NumericalFailure, never rejection/restart.
    ComplexProposalResult Propose(const std::array<double,5>& uniforms);
    bool Complete() const { return children_.size()==target_; }
    const std::vector<ComplexChild>& Children() const { return children_; }
    const ComplexLayoutCounters& Counters() const { return counters_; }
    double CentreSupportRadiusMetres() const { return centreRadius_; }
    static constexpr unsigned ProposalsPerChildBeforeRestart=256;
private:
    double minor_,major_,centreRadius_;
    std::size_t target_;
    unsigned childProposals_=0;
    bool failed_=false;
    ParticleOverlapStatus failure_=ParticleOverlapStatus::Disjoint;
    std::vector<ComplexChild> children_;
    ComplexLayoutCounters counters_;
    ComplexProposalResult Reject(ComplexProposalKind);
    ComplexProposalResult Fail(ParticleOverlapStatus);
};
}
