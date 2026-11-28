//////////////////////////////////////////////////////////////////////
//
//  HyLIoSSlabTransport.cpp - 6-layer planar optical slab transport
//    (z = 0 top exterior boundary, +z inward across layers 0..5,
//    cosine-diffuse bottom reflector at layer 5, resumable outer
//    boundary tokens, and Analog or Weighted Beer-Lambert absorption).
//
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "HyLIoSSlabTransport.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace RISE::HyLIoS {
namespace {
constexpr double Pi = 3.14159265358979323846264338327950288;
bool Finite(double x) { return std::isfinite(x); }
bool ValidVector(SlabVector d) { return Finite(d.x) && Finite(d.y) && Finite(d.z); }
void Require(bool b, const char* message) { if (!b) throw std::invalid_argument(message); }
SlabVector Normalize(SlabVector d) {
    Require(ValidVector(d), "direction must be finite");
    const double scale=std::max({std::abs(d.x),std::abs(d.y),std::abs(d.z)});
    Require(scale>0, "direction must be nonzero");
    // Power-of-two scaling avoids a fast-math reciprocal of a subnormal
    // scale. Its reciprocal could overflow even though each quotient is <=1.
    int exponent=0;
    std::frexp(scale,&exponent);
    // Keep the largest component in [1,2), not [0.5,1): the latter can
    // discard a representable subnormal component before normalization.
    --exponent;
    const SlabVector scaled{std::ldexp(d.x,-exponent),std::ldexp(d.y,-exponent),std::ldexp(d.z,-exponent)};
    const double norm=std::hypot(scaled.x,scaled.y,scaled.z);
    return {scaled.x/norm,scaled.y/norm,scaled.z/norm};
}
bool Uniform(double u) { return Finite(u) && u >= 0 && u < 1; }
bool Draw(ISampler& sampler, double& u) {
    u = sampler.Get1D();
    return Uniform(u);
}
SlabAdvanceResult Result(const SlabState& s) {
    return {s.Termination()!=SlabTermination::None ? SlabAdvanceKind::Terminal :
        s.HasPendingBoundary() ? SlabAdvanceKind::OuterBoundaryEncounter : SlabAdvanceKind::YieldBudget, s.Termination()};
}
// Form (a*b-c*d)/(a*b+c*d) without materializing either product. Scaling
// indices alone loses a small index that can still compete with n*cos(theta)
// at grazing incidence. Scale each polarization's complete products instead.
long double PolarizationAmplitude(long double a, long double b, long double c, long double d) {
    if (a==0 || b==0) return -1;
    if (c==0 || d==0) return 1;
    int ea=0,eb=0,ec=0,ed=0;
    const long double lhs=std::frexp(a,&ea)*std::frexp(b,&eb);
    const long double rhs=std::frexp(c,&ec)*std::frexp(d,&ed);
    const int scale=std::max(ea+eb,ec+ed);
    const long double scaledLhs=std::scalbn(lhs,ea+eb-scale);
    const long double scaledRhs=std::scalbn(rhs,ec+ed-scale);
    return (scaledLhs-scaledRhs)/(scaledLhs+scaledRhs);
}
}
const SlabBoundaryEncounter& SlabState::PendingBoundary() const {
    if (!pending_) throw std::logic_error("no pending slab boundary");
    return encounter_;
}
// Evaluates unpolarized smooth dielectric Fresnel reflectance and Snell's law
// using exponent-scaled arithmetic to avoid overflow/subnormal artifacts.
SmoothDielectricEvaluation EvaluateSlabSmoothDielectric(const SlabBoundaryEncounter& e) {
    Require((e.incidentSide==SlabSide::Exterior && e.incidentDirection.z>=0) ||
        (e.incidentSide==SlabSide::Interior && e.incidentDirection.z<=0), "dielectric incident side mismatch");
    const auto d=Normalize(e.incidentDirection);
    Require(Finite(e.incidentIOR) && e.incidentIOR>0 && Finite(e.targetIOR) && e.targetIOR>0, "invalid dielectric IOR");
    Require((e.incidentSide==SlabSide::Exterior && d.z>=0) ||
        (e.incidentSide==SlabSide::Interior && d.z<=0), "dielectric incident side mismatch");
    SmoothDielectricEvaluation r{0,false,{d.x,d.y,-d.z},d};
    if (e.incidentIOR==e.targetIOR) return r; // exact matched-index identity, including tangent
    const long double si=std::hypot(static_cast<long double>(d.x),static_cast<long double>(d.y));
    const long double ci=std::abs(static_cast<long double>(d.z));
    // Keep eta*sin(theta) representable even when eta alone overflows; on
    // Apple Silicon long double has binary64 range. No critical-angle epsilon.
    int eni=0,ent=0,esi=0;
    const long double mni=std::frexp(e.incidentIOR,&eni), mnt=std::frexp(e.targetIOR,&ent);
    const long double msi=std::frexp(si,&esi);
    const long double st=si==0 ? 0 : std::scalbn((mni*msi)/mnt,eni+esi-ent);
    if (st>=1) { r.reflectance=1; r.totalInternalReflection=st>1; return r; }
    if (ci==0) { r.reflectance=1; return r; }
    const long double ct=std::sqrt((1-st)*(1+st));
    const long double rs=PolarizationAmplitude(e.incidentIOR,ci,e.targetIOR,ct);
    const long double rp=PolarizationAmplitude(e.targetIOR,ci,e.incidentIOR,ct);
    r.reflectance=static_cast<double>((rs*rs+rp*rp)/2);
    r.transmittedDirection={si==0 ? 0 : static_cast<double>(st*(d.x/si)),
        si==0 ? 0 : static_cast<double>(st*(d.y/si)),
        static_cast<double>(e.incidentSide==SlabSide::Exterior ? ct : -ct)};
    if (!Finite(r.reflectance) || !ValidVector(r.transmittedDirection))
        throw std::overflow_error("dielectric numerical range failure");
    r.transmittedDirection=Normalize(r.transmittedDirection);
    return r;
}
SlabBoundaryDecision SampleSlabSmoothDielectric(const SlabBoundaryEncounter& e, double u) {
    Require(Uniform(u), "dielectric variate must be finite in [0,1)");
    const auto optics=EvaluateSlabSmoothDielectric(e);
    if (u<optics.reflectance) return {BoundaryAction::Reflect,e.incidentSide,optics.reflectedDirection,1,SlabBoundaryAngularLaw::PlanarDielectric};
    return {BoundaryAction::Transmit,e.incidentSide==SlabSide::Exterior ? SlabSide::Interior : SlabSide::Exterior,
        optics.transmittedDirection,1,SlabBoundaryAngularLaw::PlanarDielectric};
}
HyLIoSSlabTransport::HyLIoSSlabTransport(std::array<SlabLayer,6> layers, double wavelengthNm,
    double exteriorIOR, AbsorptionEstimator estimator, double rouletteTargetWeight) :
    layers_(layers), wavelengthNm_(wavelengthNm), exteriorIOR_(exteriorIOR),
    rouletteTargetWeight_(rouletteTargetWeight), estimator_(estimator) {
    Require(Finite(wavelengthNm) && wavelengthNm>0 && Finite(exteriorIOR) && exteriorIOR>0, "invalid slab wavelength/exterior IOR");
    Require(estimator==AbsorptionEstimator::Analog || estimator==AbsorptionEstimator::Weighted, "invalid absorption estimator");
    Require(Finite(rouletteTargetWeight) && rouletteTargetWeight>=0 &&
        (estimator==AbsorptionEstimator::Weighted || rouletteTargetWeight==0), "invalid slab roulette configuration");
    for (std::size_t i=0;i<6;++i) {
        const auto l=layers[i];
        Require(Finite(l.thicknessMetres) && l.thicknessMetres>0 && Finite(l.absorptionPerMetre) && l.absorptionPerMetre>=0 && Finite(l.realIOR) && l.realIOR>0, "invalid slab layer");
        planes_[i+1]=planes_[i]+l.thicknessMetres;
        Require(Finite(planes_[i+1]) && planes_[i+1]>planes_[i], "slab planes not representably separated");
    }
}
SlabState HyLIoSSlabTransport::Begin(SlabVector p, SlabVector d) const {
    Require(d.z>=0, "slab launch direction must point inward");
    d=Normalize(d);
    Require(ValidVector(p) && p.z==0 && d.z>=0, "slab launch requires top plane and inward direction");
    SlabState s;
    s.owner_=this; s.position_=p; s.direction_=d; s.wavelengthNm_=wavelengthNm_;
    Encounter(s,SlabSide::Exterior);
    return s;
}
void HyLIoSSlabTransport::Encounter(SlabState& s, SlabSide side) const {
    if (s.nextToken_==std::numeric_limits<std::uint64_t>::max()) { s.termination_=SlabTermination::NumericalFailure; return; }
    s.encounter_={++s.nextToken_,side,s.position_,s.direction_,s.wavelengthNm_,s.weight_,
        side==SlabSide::Exterior ? exteriorIOR_ : layers_[0].realIOR,
        side==SlabSide::Exterior ? layers_[0].realIOR : exteriorIOR_};
    s.pending_=true; ++s.counters_.boundaryEncounters;
}
// Applies the caller's reflection/transmission decision for the matching outer
// boundary token, updating weight and terminating on exterior escape or zero weight.
void HyLIoSSlabTransport::ResumeBoundary(SlabState& s, std::uint64_t token, const SlabBoundaryDecision& d) const {
    Require(s.owner_==this, "slab state belongs to a different kernel");
    Require(s.pending_ && s.termination_==SlabTermination::None && token==s.encounter_.token, "stale or mismatched slab boundary token");
    const auto direction=Normalize(d.direction);
    Require(Finite(d.weightMultiplier) && d.weightMultiplier>=0, "invalid slab boundary proposal");
    Require(d.action==BoundaryAction::Reflect || d.action==BoundaryAction::Transmit, "invalid slab boundary action");
    const auto from=s.encounter_.incidentSide;
    const auto to=d.action==BoundaryAction::Reflect ? from : from==SlabSide::Exterior ? SlabSide::Interior : SlabSide::Exterior;
    Require(d.targetSide==to && (to==SlabSide::Interior ? d.direction.z>=0 : d.direction.z<=0), "slab boundary target side/direction mismatch");
    const double w=s.weight_*d.weightMultiplier;
    s.pending_=false; s.direction_=direction;
    if (!Finite(w) || (w==0 && s.weight_>0 && d.weightMultiplier>0)) {
        s.termination_=SlabTermination::NumericalFailure; return;
    }
    s.weight_=w;
    if (w==0) { s.termination_=SlabTermination::ZeroWeight; return; }
    if (to==SlabSide::Exterior) {
        s.layer_=-1;
        s.termination_=from==SlabSide::Exterior ? SlabTermination::ExternalReflection : SlabTermination::SubsurfaceReturn;
    } else s.layer_=0;
}
// Advances the ray by `distance` in the current layer, recording path lengths
// and depositing Analog or Weighted Beer-Lambert absorption.
bool HyLIoSSlabTransport::Flight(SlabState& s, double distance, bool absorptionEvent) const {
    const auto i=static_cast<std::size_t>(s.layer_);
    const SlabVector p={s.position_.x+distance*s.direction_.x,s.position_.y+distance*s.direction_.y,s.position_.z+distance*s.direction_.z};
    const double total=s.counters_.pathLengthMetres+distance;
    const double layerTotal=s.counters_.layerPathLengthMetres[i]+distance;
    if (!Finite(distance) || distance<0 || !ValidVector(p) || !Finite(total) || !Finite(layerTotal)) {
        s.termination_=SlabTermination::NumericalFailure; return false;
    }
    s.position_=p; s.counters_.pathLengthMetres=total; s.counters_.layerPathLengthMetres[i]=layerTotal; ++s.counters_.flights;
    if (absorptionEvent) {
        if (!Finite(s.absorbed_[i]+s.weight_)) { s.termination_=SlabTermination::NumericalFailure; return false; }
        s.absorbed_[i]+=s.weight_; s.weight_=0; s.termination_=SlabTermination::Absorbed; return false;
    }
    if (estimator_==AbsorptionEstimator::Weighted && layers_[i].absorptionPerMetre>0) {
        const double optical=layers_[i].absorptionPerMetre*distance;
        const double loss=s.weight_*(-std::expm1(-optical));
        const double remain=s.weight_*std::exp(-optical);
        if (!Finite(loss) || !Finite(remain) || !Finite(s.absorbed_[i]+loss)) { s.termination_=SlabTermination::NumericalFailure; return false; }
        s.absorbed_[i]+=loss; s.weight_=remain;
        if (remain==0) { s.termination_=SlabTermination::Absorbed; return false; }
    }
    return true;
}
bool HyLIoSSlabTransport::Roulette(SlabState& s, ISampler& sampler) const {
    if (rouletteTargetWeight_==0 || s.weight_>=rouletteTargetWeight_ || s.weight_==0) return true;
    const double probability=s.weight_/rouletteTargetWeight_;
    if (probability==0) { s.termination_=SlabTermination::NumericalFailure; return false; }
    double u;
    if (!Draw(sampler,u)) { s.termination_=SlabTermination::InvalidSample; return false; }
    ++s.counters_.rouletteTrials;
    if (u<probability) { s.weight_=rouletteTargetWeight_; return true; }
    ++s.counters_.rouletteKills; s.weight_=0; s.termination_=SlabTermination::RouletteKilled; return false;
}
// Steps slab transport up to `workQuantum` events: handles tangent rays, exact
// plane intersections (no epsilon offsets), outer-boundary yields at z=0,
// cosine-hemisphere bottom reflections at layer 5, and internal Fresnel interfaces.
SlabAdvanceResult HyLIoSSlabTransport::Advance(SlabState& s, ISampler& sampler, std::size_t workQuantum) const {
    Require(s.owner_==this, "slab state belongs to a different kernel");
    Require(!sampler.HasFixedDimensionBudget(), "slab transport requires a variable-dimension ISampler");
    if (s.pending_ || s.termination_!=SlabTermination::None) return Result(s);
    for (std::size_t work=0;work<workQuantum;++work) {
        if (!Roulette(s,sampler)) return Result(s);
        const auto i=static_cast<std::size_t>(s.layer_);
        const double dz=s.direction_.z, mu=layers_[i].absorptionPerMetre;
        if (dz==0) {
            if (mu==0) return Result(s); // transparent tangent has no finite next event
            double u;
            if (!Draw(sampler,u)) { s.termination_=SlabTermination::InvalidSample; return Result(s); }
            // Infinite absorbing tangent: sample its physical absorption location
            // in either estimator. There is no finite Beer-survival endpoint.
            const double sampled=-std::log1p(-u)/mu;
            if (sampled==0 && u>0) { s.termination_=SlabTermination::NumericalFailure; return Result(s); }
            Flight(s,sampled,true); return Result(s);
        }
        const bool down=dz>0;
        const double plane=planes_[down ? i+1 : i];
        const double distance=(plane-s.position_.z)/dz;
        if (!Finite(distance) || distance<0) { s.termination_=SlabTermination::NumericalFailure; return Result(s); }
        if (estimator_==AbsorptionEstimator::Analog && mu>0 && distance>0) {
            double u;
            if (!Draw(sampler,u)) { s.termination_=SlabTermination::InvalidSample; return Result(s); }
            const double sampled=-std::log1p(-u)/mu;
            if (sampled==0 && u>0) { s.termination_=SlabTermination::NumericalFailure; return Result(s); }
            // Boundary wins an exact tie (including zero-distance contacts).
            if (sampled<distance) { Flight(s,sampled,true); return Result(s); }
        }
        if (!Flight(s,distance,false)) return Result(s);
        s.position_.z=plane; // exact topological plane, no epsilon displacements
        if (!down && i==0) { Encounter(s,SlabSide::Interior); return Result(s); }
        if (down && i==5) {
            double u,v;
            if (!Draw(sampler,u) || !Draw(sampler,v)) { s.termination_=SlabTermination::InvalidSample; return Result(s); }
            // u=0 gives a valid normal direction, never a zero-measure tangent.
            const double radial=std::sqrt(u), phi=2*Pi*v;
            s.direction_={radial*std::cos(phi),radial*std::sin(phi),-std::sqrt(1-u)};
            ++s.counters_.bottomReflections;
        } else {
            const auto j=down ? i+1 : i-1;
            SlabBoundaryEncounter e{0,down ? SlabSide::Exterior : SlabSide::Interior,s.position_,s.direction_,s.wavelengthNm_,s.weight_,layers_[i].realIOR,layers_[j].realIOR};
            double u;
            if (!Draw(sampler,u)) { s.termination_=SlabTermination::InvalidSample; return Result(s); }
            SlabBoundaryDecision d;
            try { d=SampleSlabSmoothDielectric(e,u); }
            catch (const std::exception&) { s.termination_=SlabTermination::NumericalFailure; return Result(s); }
            s.direction_=d.direction;
            if (d.action==BoundaryAction::Transmit) s.layer_=static_cast<int>(j);
            ++s.counters_.internalInterfaces;
        }
    }
    return Result(s);
}
} // namespace RISE::HyLIoS
