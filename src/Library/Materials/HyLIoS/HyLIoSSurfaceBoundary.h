#pragma once
#include "HyLIoSSlabTransport.h"

namespace RISE::HyLIoS {
// Clean-room, forward physical-power interpretation of paper (2015) Eq. (8).
// Macro Fresnel is sampled FIRST; only the reflected normal is perturbed.
// No reciprocal BSDF / adjoint response or radiance eta factor is asserted.
inline constexpr const char* SurfaceBoundaryVersion = "hylios2015_macro_fresnel_normal_perturb_v1";
enum class SurfaceBoundaryPhase { Branch, NormalProposal, Ready, Cancelled, BudgetExceeded };
struct SurfaceNormalProposal { double cosine, sine, acceptanceProbability; };
class HyLIoSSurfaceBoundary;
class SurfaceBoundaryState {
public:
    SurfaceBoundaryPhase Phase() const { return phase_; }
    const SlabBoundaryEncounter& Encounter() const { return encounter_; }
    double MacroReflectance() const { return optics_.reflectance; }
    double LiveWeight() const { return encounter_.weight; }
    std::uint64_t WorkUnits() const { return workUnits_; }
    std::uint64_t NormalProposals() const { return normalProposals_; }
    // Valid only in Ready; otherwise throws logic_error.
    const SlabBoundaryDecision& Decision() const;
private:
    friend class HyLIoSSurfaceBoundary;
    SurfaceBoundaryState() = default;
    const HyLIoSSurfaceBoundary* owner_ = nullptr;
    SlabBoundaryEncounter encounter_{};
    SmoothDielectricEvaluation optics_{};
    SlabBoundaryDecision decision_{};
    SurfaceBoundaryPhase phase_ = SurfaceBoundaryPhase::Branch;
    std::uint64_t workUnits_ = 0, normalProposals_ = 0;
};
class HyLIoSSurfaceBoundary {
public:
    // Zero is the exact smooth delta. Positive folds are supported in the
    // binary64 working interval [sqrt(epsilon),1/sqrt(epsilon)]. Outside it,
    // invalid_argument is raised before any branch work (see derivation).
    explicit HyLIoSSurfaceBoundary(double foldAspect);
    HyLIoSSurfaceBoundary(const HyLIoSSurfaceBoundary&) = delete;
    HyLIoSSurfaceBoundary& operator=(const HyLIoSSurfaceBoundary&) = delete;
    HyLIoSSurfaceBoundary(HyLIoSSurfaceBoundary&&) = delete;
    HyLIoSSurfaceBoundary& operator=(HyLIoSSurfaceBoundary&&) = delete;
    double FoldAspect() const { return aspect_; }
    double Normalization() const { return normalization_; } // Z_s; zero for delta
    // Normalized per-radian polar density, not a density per solid angle.
    // Delta mode has no finite PDF and throws domain_error.
    double PolarAnglePDF(double theta) const;
    // One proposal from q(mu), plus its rejection acceptance probability.
    // The target normalized Eq8 law is obtained by independent uniform < pAccept.
    SurfaceNormalProposal ProposeNormal(double uniform) const;
    // Pins the complete encounter, normalized direction and macro optics.
    // The immutable law must outlive every state; Begin consumes no work/RNG.
    SurfaceBoundaryState Begin(const SlabBoundaryEncounter&) const;
    // Exactly ONE transition, never a rejection loop. Branch consumes u only;
    // NormalProposal consumes all three variates in [0,1): polar u, azimuth v,
    // acceptance. Invalid used variates throw without changing the state.
    // Ready/stop states reject further sampling. Each valid call adds one work
    // unit, including rejected proposals. Rejection preserves the chosen R branch.
    SurfaceBoundaryPhase TrySample(SurfaceBoundaryState&, double u, double v=0, double acceptance=0) const;
    // Caller cancellation/caps are unresolved live power, never absorption.
    void Cancel(SurfaceBoundaryState&) const;
    void StopForBudget(SurfaceBoundaryState&) const;
private:
    double aspect_, parameter_=0, delta_=0, angle_=0, integral_=0, normalization_=0;
    void ValidateState(const SurfaceBoundaryState&) const;
};
}
