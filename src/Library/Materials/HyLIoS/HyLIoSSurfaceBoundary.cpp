#include "pch.h"
#include "HyLIoSSurfaceBoundary.h"
#include <cmath>
#include <limits>
#include <stdexcept>
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#error HyLIoSSurfaceBoundary requires strict floating-point evaluation
#endif

namespace RISE::HyLIoS {
namespace {
constexpr double Pi = 3.14159265358979323846264338327950288;
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits==53,
    "surface-fold working interval requires IEEE binary64");
void Require(bool b, const char* message) { if (!b) throw std::invalid_argument(message); }
bool Uniform(double u) { return std::isfinite(u) && u>=0 && u<1; }
bool FiniteVector(SlabVector v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
SlabVector Unit(SlabVector v) {
    const double length=std::hypot(v.x,v.y,v.z);
    if (!std::isfinite(length) || length==0) throw std::overflow_error("surface reflection numerical range failure");
    return {v.x/length,v.y/length,v.z/length};
}
}
const SlabBoundaryDecision& SurfaceBoundaryState::Decision() const {
    if (phase_!=SurfaceBoundaryPhase::Ready) throw std::logic_error("surface boundary decision is not ready");
    return decision_;
}
HyLIoSSurfaceBoundary::HyLIoSSurfaceBoundary(double s) : aspect_(s) {
    Require(std::isfinite(s) && s>=0, "surface fold aspect must be finite and nonnegative");
    if (s==0) return;
    // Conservative binary64 working domain: s^2 remains in [eps,1/eps].
    // Below it 1-s^2/2 cannot resolve the polar cosine width; above it the
    // arctangent endpoint approaches pi/2 closer than sqrt(eps) radians.
    const double endpoint=std::sqrt(std::numeric_limits<double>::epsilon());
    Require(s>=endpoint && s<=1/endpoint, "surface fold aspect outside supported binary64 working interval");
    if (s<1) {
        parameter_=std::sqrt((1-s)*(1+s)); // k
        delta_=(s*s)/(1+parameter_);      // 1-k, without cancellation
        const double atanhK=std::log1p(parameter_)-std::log(s);
        integral_=0.5/(s*s)+0.5*atanhK/parameter_;
    } else if (s>1) {
        parameter_=std::sqrt((s-1)*(s+1)); // a
        angle_=std::atan(parameter_);
        integral_=0.5/(s*s)+0.5*angle_/parameter_;
    } else integral_=1;
    normalization_=(s*s)*(s*s)*integral_;
}
double HyLIoSSurfaceBoundary::PolarAnglePDF(double theta) const {
    Require(std::isfinite(theta) && theta>=0 && theta<=Pi/2, "surface polar angle outside [0,pi/2]");
    if (aspect_==0) throw std::domain_error("smooth surface is a delta, without finite polar PDF");
    const double c=std::cos(theta), t=std::sin(theta);
    const double denominator=(aspect_*c)*(aspect_*c)+t*t;
    return t/(integral_*denominator*denominator);
}
SurfaceNormalProposal HyLIoSSurfaceBoundary::ProposeNormal(double u) const {
    Require(Uniform(u), "surface polar variate must be finite in [0,1)");
    if (aspect_==0) return {1,0,1};
    if (aspect_<1) {
        const double denominator=u+delta_*(1-u);
        const double mu=u/denominator;
        // Avoid subtracting mu from one: the small polar sine carries the
        // perturbation even when its cosine rounds to one.
        const double oneMinusMu=delta_*(1-u)/denominator;
        const double factor=1+parameter_*mu;
        return {mu,std::sqrt(oneMinusMu*(1+mu)),1/(factor*factor)};
    }
    if (aspect_==1) return {u,std::sqrt((1-u)*(1+u)),1};
    const double tangent=std::tan(u*angle_);
    const double mu=tangent/parameter_;
    if (!(mu>=0 && mu<=1)) throw std::overflow_error("surface polar inverse outside representable domain");
    return {mu,std::sqrt((1-mu)*(1+mu)),1/(1+tangent*tangent)};
}
SurfaceBoundaryState HyLIoSSurfaceBoundary::Begin(const SlabBoundaryEncounter& e) const {
    Require(e.token!=0 && FiniteVector(e.position) && e.position.z==0 &&
        std::isfinite(e.wavelengthNm) && e.wavelengthNm>0 && std::isfinite(e.weight) && e.weight>=0,
        "invalid surface boundary encounter");
    SurfaceBoundaryState state;
    state.optics_=EvaluateSlabSmoothDielectric(e);
    state.encounter_=e;
    // Smooth evaluator already normalized scale-safely. Recover the normalized
    // incident direction from its reflected direction, without another division.
    state.encounter_.incidentDirection={state.optics_.reflectedDirection.x,state.optics_.reflectedDirection.y,-state.optics_.reflectedDirection.z};
    state.owner_=this;
    return state;
}
void HyLIoSSurfaceBoundary::ValidateState(const SurfaceBoundaryState& state) const {
    Require(state.owner_==this, "surface boundary state belongs to another law");
    Require(state.phase_==SurfaceBoundaryPhase::Branch || state.phase_==SurfaceBoundaryPhase::NormalProposal,
        "surface boundary is already ready or stopped");
}
SurfaceBoundaryPhase HyLIoSSurfaceBoundary::TrySample(SurfaceBoundaryState& state, double u, double v, double acceptance) const {
    ValidateState(state);
    Require(Uniform(u), "surface boundary variate must be finite in [0,1)");
    if (state.phase_==SurfaceBoundaryPhase::NormalProposal)
        Require(Uniform(v) && Uniform(acceptance), "surface proposal variates must be finite in [0,1)");
    if (state.workUnits_==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("surface boundary work counter exhausted");
    if (state.phase_==SurfaceBoundaryPhase::Branch) {
        const auto from=state.encounter_.incidentSide;
        ++state.workUnits_;
        if (u<state.optics_.reflectance) {
            state.decision_={BoundaryAction::Reflect,from,state.optics_.reflectedDirection,1};
            state.phase_=aspect_==0 ? SurfaceBoundaryPhase::Ready : SurfaceBoundaryPhase::NormalProposal;
        } else {
            state.decision_={BoundaryAction::Transmit,from==SlabSide::Exterior ? SlabSide::Interior : SlabSide::Exterior,
                state.optics_.transmittedDirection,1};
            state.phase_=SurfaceBoundaryPhase::Ready;
        }
        return state.phase_;
    }
    const auto p=ProposeNormal(u); // validates arithmetic before mutating state
    ++state.workUnits_; ++state.normalProposals_;
    if (acceptance>=p.acceptanceProbability) return state.phase_;
    const double phi=2*Pi*v;
    const double side=state.encounter_.incidentSide==SlabSide::Exterior ? -1 : 1;
    const SlabVector normal{p.sine*std::cos(phi),p.sine*std::sin(phi),side*p.cosine};
    const auto d=state.encounter_.incidentDirection;
    const double facing=-std::fma(d.x,normal.x,std::fma(d.y,normal.y,d.z*normal.z));
    if (!(facing>0)) return state.phase_;
    const SlabVector reflected{std::fma(2*facing,normal.x,d.x),std::fma(2*facing,normal.y,d.y),std::fma(2*facing,normal.z,d.z)};
    // Reject horizon endpoints as well as the wrong hemisphere. No epsilon:
    // each retained reflected direction must point into its incident side.
    if (!(side*reflected.z>0)) return state.phase_;
    state.decision_.direction=Unit(reflected);
    state.phase_=SurfaceBoundaryPhase::Ready;
    return state.phase_;
}
void HyLIoSSurfaceBoundary::Cancel(SurfaceBoundaryState& state) const {
    ValidateState(state); state.phase_=SurfaceBoundaryPhase::Cancelled;
}
void HyLIoSSurfaceBoundary::StopForBudget(SurfaceBoundaryState& state) const {
    ValidateState(state); state.phase_=SurfaceBoundaryPhase::BudgetExceeded;
}
}
