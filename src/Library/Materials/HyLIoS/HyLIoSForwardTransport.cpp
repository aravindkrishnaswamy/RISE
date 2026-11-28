#include "pch.h"
#include "HyLIoSForwardTransport.h"
#include "HyLIoSOptics.h"
#include "HyLIoSParticleOverlap.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace RISE::HyLIoS {
namespace {
constexpr double Pi=3.141592653589793238462643383279502884;
// Sterbenz subtraction is exact for nm in [780,1400). Materialize that
// binary64 evaluation before the angular scale multiplication so fast-math
// cannot distribute the scale across the nearly cancelling subtraction.
double ForwardEndpointGap(double nm) {
    volatile double gap=1400-nm;
    return gap;
}
void Require(bool v,const char* m) { if(!v) throw std::invalid_argument(m); }
bool Finite(double v) { return std::isfinite(v); }
bool Finite(SlabVector v) { return Finite(v.x)&&Finite(v.y)&&Finite(v.z); }
SlabVector Add(SlabVector a,SlabVector b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
SlabVector Mul(SlabVector a,double v) { return {a.x*v,a.y*v,a.z*v}; }
double Dot(SlabVector a,SlabVector b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
SlabVector Cross(SlabVector a,SlabVector b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
SlabVector Unit(SlabVector a) {
    Require(Finite(a),"nonfinite direction");
    const double scale=std::max({std::abs(a.x),std::abs(a.y),std::abs(a.z)});
    Require(scale>0,"zero direction"); int exponent=0;std::frexp(scale,&exponent);--exponent;
    a={std::ldexp(a.x,-exponent),std::ldexp(a.y,-exponent),std::ldexp(a.z,-exponent)};
    return Mul(a,1/std::hypot(a.x,a.y,a.z));
}
SlabVector V(ParticleVector a) { return {a.x,a.y,a.z}; }
ParticleVector P(SlabVector a) { return {a.x,a.y,a.z}; }
struct Frame {
    SlabVector x,y,z;
    explicit Frame(SlabVector axis):z(Unit(axis)) {
        x=Unit(std::abs(z.z)<0.75 ? Cross({0,0,1},z) : Cross({0,1,0},z));y=Cross(z,x);
    }
    SlabVector ToWorld(SlabVector d) const { return Unit(Add(Add(Mul(x,d.x),Mul(y,d.y)),Mul(z,d.z))); }
    SlabVector ToLocal(SlabVector d) const { return {Dot(d,x),Dot(d,y),Dot(d,z)}; }
};
SlabVector PolarComponents(SlabVector axis,double cosine,double radius,double azimuth) {
    return Frame(axis).ToWorld({radius*std::cos(azimuth),radius*std::sin(azimuth),cosine});
}
bool Draw(ISampler& rng,double& u) { u=rng.Get1D();return Finite(u)&&u>=0&&u<1; }
ForwardAdvanceResult Result(const ForwardState& s) {
    return {s.Termination()!=ForwardTermination::None ? ForwardAdvanceKind::Terminal :
        s.HasPendingBoundary() ? ForwardAdvanceKind::OuterBoundaryEncounter : ForwardAdvanceKind::YieldBudget,s.Termination()};
}
// A known surface root is t=0. Deflate it rather than rediscovering a tiny
// self root from a general quadratic. Dimensionless Q uses the minor axis as
// length scale; all geometry remains particle-relative, independent of scene x/y.
void SurfaceExit(ProlateParticle particle,ParticleVector point,SlabVector direction,
    double& distance,ParticleVector& end,ParticleVector& normal) {
    const SlabVector axis=Unit(V(particle.axis));
    const SlabVector p=Mul(V(point),1/particle.semiMinorMetres);
    const double pa=Dot(p,axis),da=Dot(direction,axis),ratio=particle.semiMinorMetres/particle.semiMajorMetres;
    const SlabVector pp=Add(p,Mul(axis,-pa)),dp=Add(direction,Mul(axis,-da));
    const long double numerator=static_cast<long double>(Dot(pp,dp))+static_cast<long double>(ratio)*ratio*pa*da;
    const long double denominator=static_cast<long double>(Dot(dp,dp))+static_cast<long double>(ratio)*ratio*da*da;
    distance=static_cast<double>((-2*numerator/denominator)*particle.semiMinorMetres);
    if(!Finite(distance)||distance<=0) throw std::runtime_error("nonpositive internal chord");
    const SlabVector q=Add(V(point),Mul(direction,distance));end=P(q);
    const SlabVector qs=Mul(q,1/particle.semiMinorMetres);const double qa=Dot(qs,axis);
    normal=P(Unit(Add(Add(qs,Mul(axis,-qa)),Mul(axis,ratio*ratio*qa))));
}
bool IsEnvelopeSphere(const ComplexChild& child,const ProlateParticle& envelope) {
    return envelope.semiMinorMetres==envelope.semiMajorMetres&&
        child.particle.semiMinorMetres==envelope.semiMinorMetres&&child.particle.semiMajorMetres==envelope.semiMajorMetres&&
        child.centreFromEnvelopeMetres.x==0&&child.centreFromEnvelopeMetres.y==0&&child.centreFromEnvelopeMetres.z==0;
}
SmoothDielectricEvaluation ParticleDielectric(SlabVector direction,ParticleVector normal,double from,double to) {
    const Frame f(V(normal));
    // Positive local z travels from the particle toward its host. The slab's
    // "Exterior" label here is only its positive-z incidence convention.
    const auto r=EvaluateSlabSmoothDielectric({0,SlabSide::Exterior,{},f.ToLocal(direction),0,1,from,to});
    return {r.reflectance,r.totalInternalReflection,f.ToWorld(r.reflectedDirection),f.ToWorld(r.transmittedDirection)};
}
}
const SlabBoundaryEncounter& ForwardState::PendingBoundary() const {
    if(!pending_) throw std::logic_error("no pending forward boundary");return encounter_;
}
ForwardLayerValidation ValidateForwardLayers(const std::array<ForwardLayer,6>& layers) {
    ForwardLayerValidation result;
    for(std::size_t i=0;i<6;++i) {
        const auto& l=layers[i];
        Require(Finite(l.thicknessMetres)&&l.thicknessMetres>0&&Finite(l.hostAbsorptionPerMetre)&&l.hostAbsorptionPerMetre>=0&&Finite(l.realIOR)&&l.realIOR>0,"invalid layer");
        result.planes[i+1]=result.planes[i]+l.thicknessMetres;
        Require(Finite(result.planes[i+1])&&result.planes[i+1]>result.planes[i],"layer planes not representably separated");
        if(l.organelle) {
            const auto& o=*l.organelle;
            Require(o.organization==ForwardOrganization::IndividualSpheroids||o.organization==ForwardOrganization::ExplicitComplexPackV1,"unversioned complex transport is unsupported");
            Require(Finite(o.volumeFraction)&&o.volumeFraction>=0&&o.volumeFraction<=1&&Finite(o.absorptionPerMetre)&&o.absorptionPerMetre>=0&&Finite(o.realIOR)&&o.realIOR>0,"invalid organelle coefficients");
            if(o.organization==ForwardOrganization::ExplicitComplexPackV1) {
                // Constructor validates count, conservative support and range even
                // at zero outer fraction; it does not generate a layout.
                ComplexLayout check(o.envelopeRadiusMetres,o.semiMinorMetres,o.semiMajorMetres,o.childCount);
                result.geometricRates[i]=GeometricEncounterRate(ProlateSurfaceToVolume(o.envelopeRadiusMetres,o.envelopeRadiusMetres),o.volumeFraction);
                if(o.volumeFraction>0) Require(o.envelopeRadiusMetres<l.thicknessMetres/2,"layer cannot contain positive-measure envelopes");
            } else {
                result.geometricRates[i]=GeometricEncounterRate(ProlateSurfaceToVolume(o.semiMinorMetres,o.semiMajorMetres),o.volumeFraction);
                if(o.volumeFraction>0) Require(o.semiMinorMetres<l.thicknessMetres/2,"layer cannot contain positive-measure spheroid placements");
            }
        }
    }
    return result;
}
HyLIoSForwardTransport::HyLIoSForwardTransport(std::array<ForwardLayer,6> layers,double wavelengthNm,
    double exteriorIOR,double fiberRate,AbsorptionEstimator estimator,double rouletteTarget,ForwardPlacementPolicy placementPolicy,ForwardPostScatterPolicy postScatterPolicy):
    layers_(layers),wavelengthNm_(wavelengthNm),exteriorIOR_(exteriorIOR),fiberRate_(fiberRate),
    rouletteTarget_(rouletteTarget),forwardAngle_(wavelengthNm<780 ? 5*Pi/180 : wavelengthNm<1400 ? (5*Pi/180)*ForwardEndpointGap(wavelengthNm)/620 : 0),estimator_(estimator),placementPolicy_(placementPolicy),postScatterPolicy_(postScatterPolicy) {
    Require(postScatterPolicy==ForwardPostScatterPolicy::DiagnosticRetainParticleV1||postScatterPolicy==ForwardPostScatterPolicy::DiscardAfterScatterV1,"invalid post-scatter policy");
    Require(placementPolicy==ForwardPlacementPolicy::FixedEventDiagnosticV1||placementPolicy==ForwardPlacementPolicy::NullCollisionPlacementV1,"invalid placement policy");
    Require(Finite(wavelengthNm)&&wavelengthNm>0&&Finite(exteriorIOR)&&exteriorIOR>0,"invalid wavelength/exterior IOR");
    Require(Finite(fiberRate)&&fiberRate>=0,"invalid PD fiber rate");
    Require(estimator==AbsorptionEstimator::Analog||estimator==AbsorptionEstimator::Weighted,"invalid estimator");
    Require(Finite(rouletteTarget)&&rouletteTarget>=0&&(estimator==AbsorptionEstimator::Weighted||rouletteTarget==0),"invalid roulette target");
    const auto validated=ValidateForwardLayers(layers_);
    planes_=validated.planes;
    geometricRates_=validated.geometricRates;
    if(layers_[4].organelle&&layers_[4].organelle->organization==ForwardOrganization::ExplicitComplexPackV1)
        Require(fiberRate_==0,"PD complex host gaps with fiber scattering are unsupported");
}
ForwardState HyLIoSForwardTransport::Begin(SlabVector position,SlabVector direction) const {
    Require(direction.z>=0,"forward launch points outward");direction=Unit(direction);
    Require(Finite(position)&&position.x==0&&position.y==0&&position.z==0&&direction.z>=0,"forward launch requires local top origin; keep scene translation outside kernel");
    ForwardState s;s.owner_=this;s.position_=position;s.direction_=direction;s.wavelengthNm_=wavelengthNm_;
    Encounter(s,SlabSide::Exterior);return s;
}
void HyLIoSForwardTransport::Encounter(ForwardState& s,SlabSide side) const {
    if(s.nextToken_==std::numeric_limits<std::uint64_t>::max()) {s.termination_=ForwardTermination::NumericalFailure;return;}
    s.encounter_={++s.nextToken_,side,s.position_,s.direction_,wavelengthNm_,s.weight_,
        side==SlabSide::Exterior ? exteriorIOR_ : layers_[0].realIOR,
        side==SlabSide::Exterior ? layers_[0].realIOR : exteriorIOR_};
    s.pending_=true;++s.counters_.boundaryEncounters;
}
void HyLIoSForwardTransport::ResumeBoundary(ForwardState& s,std::uint64_t token,const SlabBoundaryDecision& d) const {
    Require(s.owner_==this,"state belongs to another kernel");
    Require(s.pending_&&s.termination_==ForwardTermination::None&&token==s.encounter_.token,"stale boundary token");
    const auto direction=Unit(d.direction);Require(Finite(d.weightMultiplier)&&d.weightMultiplier>=0,"invalid boundary weight");
    Require(d.action==BoundaryAction::Reflect||d.action==BoundaryAction::Transmit,"invalid boundary action");
    const auto from=s.encounter_.incidentSide;
    const auto to=d.action==BoundaryAction::Reflect ? from : from==SlabSide::Exterior ? SlabSide::Interior : SlabSide::Exterior;
    Require(d.targetSide==to&&(to==SlabSide::Interior ? d.direction.z>=0 : d.direction.z<=0),"boundary side/hemisphere mismatch");
    const double weight=s.weight_*d.weightMultiplier;
    s.pending_=false;s.direction_=direction;
    if(!Finite(weight)||(weight==0&&s.weight_>0&&d.weightMultiplier>0)) {s.termination_=ForwardTermination::NumericalFailure;return;}
    s.weight_=weight;
    if(d.angularLaw==SlabBoundaryAngularLaw::IndependentFoldReflection&&d.action==BoundaryAction::Reflect)
        s.angularLineage_={ForwardAngularClass::AbsolutelyContinuous,ForwardAngularReason::FoldReflection};
    else if(d.angularLaw!=SlabBoundaryAngularLaw::PlanarDielectric)
        s.angularLineage_={ForwardAngularClass::UnclassifiedSingular,ForwardAngularReason::UntypedBoundary};
    if(weight==0) {s.termination_=ForwardTermination::ZeroWeight;return;}
    if(to==SlabSide::Exterior) {s.layer_=-1;s.termination_=from==SlabSide::Exterior ? ForwardTermination::ExternalReflection : ForwardTermination::SubsurfaceReturn;}
    else s.layer_=0;
}
void HyLIoSForwardTransport::Cancel(ForwardState& s) const {
    Require(s.owner_==this,"state belongs to another kernel");
    if(s.termination_==ForwardTermination::None) {s.pending_=false;s.termination_=ForwardTermination::Cancelled;}
}
void HyLIoSForwardTransport::StopForBudget(ForwardState& s) const {
    Require(s.owner_==this,"state belongs to another kernel");
    if(s.termination_==ForwardTermination::None) {s.pending_=false;s.termination_=ForwardTermination::BudgetExceeded;}
}
bool HyLIoSForwardTransport::Flight(ForwardState& s,double distance,double absorption,bool inside,bool absorbed) const {
    const auto i=static_cast<std::size_t>(s.layer_);const SlabVector step=Mul(s.direction_,distance),position=Add(s.position_,step);
    auto& path=inside ? s.counters_.particlePathLengthMetres[i] : s.counters_.hostPathLengthMetres[i];
    if(!Finite(distance)||distance<0||!Finite(position)||!Finite(path+distance)||
        !Finite(s.counters_.layerPathLengthMetres[i]+distance)||!Finite(s.counters_.pathLengthMetres+distance)) {
        s.termination_=ForwardTermination::NumericalFailure;return false;
    }
    s.position_=position;
    if(s.complexActive_) s.complexPoint_=P(Add(V(s.complexPoint_),step));
    s.counters_.pathLengthMetres+=distance;s.counters_.layerPathLengthMetres[i]+=distance;path+=distance;++s.counters_.flights;
    if(s.hasPredecessor_) s.predecessorCentreFromPosition_=P(Add(V(s.predecessorCentreFromPosition_),Mul(step,-1)));
    auto& tally=inside ? s.particleAbsorbed_[i] : s.hostAbsorbed_[i];
    if(absorbed) {
        // Shared by host and organelle analog absorption, including the
        // weighted estimator's infinite absorbing tangent. Preserve both the
        // accumulated physical absorption and unresolved live power on failure.
        if(!Finite(tally+s.weight_)) {
            s.termination_=ForwardTermination::NumericalFailure;
            s.failureDetail_=ForwardFailureDetail::ArithmeticOrGeometry;
            return false;
        }
        tally+=s.weight_;s.weight_=0;s.termination_=ForwardTermination::Absorbed;return false;
    }
    if(estimator_==AbsorptionEstimator::Weighted&&absorption>0) {
        const double optical=absorption*distance,loss=s.weight_*(-std::expm1(-optical)),remaining=s.weight_*std::exp(-optical);
        if(!Finite(loss)||!Finite(remaining)||!Finite(tally+loss)) {s.termination_=ForwardTermination::NumericalFailure;return false;}
        tally+=loss;s.weight_=remaining;
        if(remaining==0) {s.termination_=ForwardTermination::Absorbed;return false;}
    }
    return true;
}
bool HyLIoSForwardTransport::Roulette(ForwardState& s,ISampler& rng) const {
    if(rouletteTarget_==0||s.weight_>=rouletteTarget_||s.weight_==0) return true;
    const double p=s.weight_/rouletteTarget_;if(p==0) {s.termination_=ForwardTermination::NumericalFailure;return false;}
    double u;if(!Draw(rng,u)) {s.termination_=ForwardTermination::InvalidSample;return false;}
    ++s.counters_.rouletteTrials;
    if(u<p) {s.weight_=rouletteTarget_;return true;}
    ++s.counters_.rouletteKills;s.weight_=0;s.termination_=ForwardTermination::RouletteKilled;return false;
}
void HyLIoSForwardTransport::Bulk(ForwardState& s,ISampler& rng) const {
    const auto i=static_cast<std::size_t>(s.layer_);const auto& l=layers_[i];
    const bool down=s.direction_.z>0;double distance=0;int event=0;
    bool finiteEvent=s.direction_.z!=0;
    const double plane=planes_[down ? i+1 : i];
    if(finiteEvent) {distance=(plane-s.position_.z)/s.direction_.z;if(!Finite(distance)||distance<0) {s.termination_=ForwardTermination::NumericalFailure;return;}}
    const bool infiniteAbsorbingTangent=!finiteEvent&&geometricRates_[i]==0&&(i!=4||s.fiberUsed_||fiberRate_==0)&&l.hostAbsorptionPerMetre>0;
    const double rates[3]={estimator_==AbsorptionEstimator::Analog||infiniteAbsorbingTangent ? l.hostAbsorptionPerMetre : 0,geometricRates_[i],i==4&&!s.fiberUsed_ ? fiberRate_ : 0};
    for(int channel=0;channel<3;++channel) if(rates[channel]>0) {
        double u;if(!Draw(rng,u)) {s.termination_=ForwardTermination::InvalidSample;return;}
        const double proposal=*SampleExponentialFreePath(rates[channel],u);
        if(!finiteEvent||proposal<distance) {distance=proposal;event=channel+1;finiteEvent=true;}
    }
    if(!finiteEvent) return; // transparent tangent: unresolved resumable yield
    if(!Flight(s,distance,l.hostAbsorptionPerMetre,false,event==1)) return;
    if(event==2) {
        ++s.counters_.particleCandidates;
        s.phase_=ForwardPhase::CandidatePlacement;
        if(placementPolicy_==ForwardPlacementPolicy::NullCollisionPlacementV1) return;
        if(s.position_.z<=planes_[i]||s.position_.z>=planes_[i+1]) s.termination_=ForwardTermination::UnplaceableBoundaryCandidate;
        else {
            const auto& o=*l.organelle;
            // For a sphere, centre offsets at an entering ray's silhouette
            // attain +/-r*sqrt(1-dz^2). Therefore its minimum downstream
            // clearance is r*(1-sqrt(1-dz^2)); use the cancellation-free form.
            // Equality has zero projected-area support, hence is unresolved.
            if(o.organization==ForwardOrganization::ExplicitComplexPackV1||o.semiMinorMetres==o.semiMajorMetres) {
                const double az=std::abs(s.direction_.z);
                const double radius=o.organization==ForwardOrganization::ExplicitComplexPackV1 ? o.envelopeRadiusMetres : o.semiMinorMetres;
                const double margin=radius*(az*az/(1+std::sqrt((1-az)*(1+az))));
                const double clearance=down ? planes_[i+1]-s.position_.z : s.position_.z-planes_[i];
                if(clearance<=margin) s.termination_=ForwardTermination::UnplaceableBoundaryCandidate;
            }
        }
        return;
    }
    if(event==3) {s.fiberUsed_=true;++s.counters_.fiberEvents;s.phase_=ForwardPhase::FiberPerturbation;s.perturbationAxis_=s.direction_;return;}
    s.position_.z=plane;
    if(!down&&i==0) {Encounter(s,SlabSide::Interior);return;}
    double u,v;
    if(down&&i==5) {
        if(!Draw(rng,u)||!Draw(rng,v)) {s.termination_=ForwardTermination::InvalidSample;return;}
        s.direction_=PolarComponents({0,0,-1},std::sqrt(1-u),std::sqrt(u),2*Pi*v);
        s.angularLineage_={ForwardAngularClass::AbsolutelyContinuous,ForwardAngularReason::DiffuseBottom};
        ++s.counters_.bottomReflections;return;
    }
    const auto j=down ? i+1 : i-1;
    if(!Draw(rng,u)) {s.termination_=ForwardTermination::InvalidSample;return;}
    const auto d=SampleSlabSmoothDielectric({0,down ? SlabSide::Exterior : SlabSide::Interior,s.position_,s.direction_,wavelengthNm_,s.weight_,l.realIOR,layers_[j].realIOR},u);
    s.direction_=d.direction;++s.counters_.internalInterfaces;
    if(d.action==BoundaryAction::Transmit) {
        s.layer_=static_cast<int>(j);
        if(i==4) s.fiberUsed_=false;
        if(j==4) {s.fiberUsed_=false;++s.counters_.fiberPasses;}
    }
}
void HyLIoSForwardTransport::Placement(ForwardState& s,ISampler& rng) const {
    const auto i=static_cast<std::size_t>(s.layer_);const auto& o=*layers_[i].organelle;
    double r,a;
    ProlateParticle particle{o.envelopeRadiusMetres,o.envelopeRadiusMetres,{0,0,1}};
    if(o.organization==ForwardOrganization::IndividualSpheroids) {
        double u,v;if(!Draw(rng,u)||!Draw(rng,v)) {s.termination_=ForwardTermination::InvalidSample;return;}
        const auto orientation=SampleOrientation(u,o.semiMinorMetres,o.semiMajorMetres);
        particle={o.semiMinorMetres,o.semiMajorMetres,P(PolarComponents(s.direction_,orientation.cosine,orientation.transverseRadius,2*Pi*v))};
    }
    if(!Draw(rng,r)||!Draw(rng,a)) {s.termination_=ForwardTermination::InvalidSample;return;}
    ++s.counters_.placementAttempts;
    const auto hit=SampleProlateProjectedHit(particle,P(s.direction_),r,a);
    // Distance convention: geometric free-flight ends at the ENTRY point, so
    // centre = event position - sampled entryPoint. Never add half a chord.
    const SlabVector centre=Mul(V(hit.chord.entryPointMetres),-1);
    const double az=particle.axis.z;
    const double extent=std::hypot(particle.semiMinorMetres*std::sqrt(std::max(0.,(1-az)*(1+az))),particle.semiMajorMetres*az);
    const double depth=s.position_.z+centre.z;
    if(depth-extent<planes_[i]||depth+extent>planes_[i+1]) {++s.counters_.containmentRejections;if(placementPolicy_==ForwardPlacementPolicy::NullCollisionPlacementV1) {s.phase_=ForwardPhase::BulkFlight;++s.counters_.nullPlacements;}return;}
    if(s.hasPredecessor_) {
        const auto overlap=ClassifyProlateOverlap(s.predecessor_,particle,P(Add(centre,Mul(V(s.predecessorCentreFromPosition_),-1))));
        if(overlap.status==ParticleOverlapStatus::Overlap) {++s.counters_.overlapRejections;if(placementPolicy_==ForwardPlacementPolicy::NullCollisionPlacementV1) {s.phase_=ForwardPhase::BulkFlight;++s.counters_.nullPlacements;}return;}
        if(overlap.status!=ParticleOverlapStatus::Disjoint&&overlap.status!=ParticleOverlapStatus::Touching) {
            s.termination_=ForwardTermination::NumericalFailure;
            s.failureDetail_=overlap.status==ParticleOverlapStatus::Indeterminate ? ForwardFailureDetail::OverlapIndeterminate :
                overlap.status==ParticleOverlapStatus::InvalidInput ? ForwardFailureDetail::OverlapInvalidInput : ForwardFailureDetail::OverlapUnsupportedRange;
            return;
        }
    }
    s.particle_=particle;s.predecessor_=particle;s.hasPredecessor_=true;s.predecessorCentreFromPosition_=P(centre);
    s.particlePoint_=hit.chord.entryPointMetres;s.particleExit_=hit.chord.exitPointMetres;s.exitNormal_=hit.chord.exitOutwardNormal;
    s.chordDistance_=hit.chord.chordLengthMetres;++s.counters_.acceptedParticles;
    s.complexLayout_.reset();s.complexActive_=false;s.complexEntryChord_=false;
    if(o.organization==ForwardOrganization::ExplicitComplexPackV1) {
        s.envelope_=particle;s.complexPoint_=hit.chord.entryPointMetres;s.complexActive_=true;s.complexEntryChord_=true;
        s.activeChild_=ComplexLayout::MaximumChildren;s.lastExitedChild_=ComplexLayout::MaximumChildren;
        s.complexLayout_.emplace(o.envelopeRadiusMetres,o.semiMinorMetres,o.semiMajorMetres,o.childCount);
        ++s.counters_.acceptedComplexes;
        s.phase_=s.complexLayout_->Complete() ? ForwardPhase::ComplexHostFlight : ForwardPhase::ComplexLayout;
    } else s.phase_=ForwardPhase::ParticleChord;
    // Fresh projected impact adds two dimensions. Child layout is generated
    // independently in global coordinates; its fixed-layout disintegration
    // does not condition on the impact-dependent world envelope centre.
    const bool complex=o.organization==ForwardOrganization::ExplicitComplexPackV1;
    const bool phaseAC=s.angularLineage_.classification==ForwardAngularClass::AbsolutelyContinuous;
    s.angularSupport_={complex ? ForwardAngularFrame::ComplexLayout : ForwardAngularFrame::IndividualBody,
        phaseAC,!complex||phaseAC,false,complex};
    s.independentProjectedChord_=!complex;
}
void HyLIoSForwardTransport::BuildComplex(ForwardState& s,ISampler& rng) const {
    std::array<double,5> u{};
    for(double& value:u) if(!Draw(rng,value)) {s.termination_=ForwardTermination::InvalidSample;return;}
    const auto previous=s.complexLayout_->Counters();
    const auto result=s.complexLayout_->Propose(u);
    const auto& now=s.complexLayout_->Counters();
    s.counters_.complexLayoutProposals+=now.proposals-previous.proposals;
    s.counters_.complexLayoutContainmentRejections+=now.containmentRejections-previous.containmentRejections;
    s.counters_.complexLayoutOverlapRejections+=now.overlapRejections-previous.overlapRejections;
    s.counters_.complexLayoutRestarts+=now.restarts-previous.restarts;
    s.counters_.complexLayoutPairChecks+=now.overlapChecks-previous.overlapChecks;
    if(result.kind==ComplexProposalKind::InvalidSample) s.termination_=ForwardTermination::InvalidSample;
    else if(result.kind==ComplexProposalKind::NumericalFailure) {
        s.termination_=ForwardTermination::NumericalFailure;
        s.failureDetail_=result.geometryStatus==ParticleOverlapStatus::Indeterminate ? ForwardFailureDetail::OverlapIndeterminate :
            result.geometryStatus==ParticleOverlapStatus::InvalidInput ? ForwardFailureDetail::OverlapInvalidInput : ForwardFailureDetail::OverlapUnsupportedRange;
    } else if(s.complexLayout_->Complete()) s.phase_=ForwardPhase::ComplexHostFlight;
}
void HyLIoSForwardTransport::ComplexHost(ForwardState& s,ISampler& rng) const {
    const auto& children=s.complexLayout_->Children();
    double distance;
    ParticleVector envelopeExit,envelopeNormal;
    if(s.complexEntryChord_) {
        // Placement already owns a projected-area entry and its complete chord.
        // Layout yields do not change that ray. Do not rediscover a rounded
        // surface root, including for an empty envelope.
        distance=s.chordDistance_;envelopeExit=s.particleExit_;envelopeNormal=s.exitNormal_;
    } else if(s.lastExitedChild_<children.size()&&IsEnvelopeSphere(children[s.lastExitedChild_],s.envelope_)) {
        // Equal centred spheres have the same boundary. Child transmission
        // has already reached the envelope exit; there is exactly zero host
        // distance even when refraction changed the transmitted direction.
        distance=0;envelopeExit=s.complexPoint_;envelopeNormal=s.exitNormal_;
    } else {
        const auto hit=IntersectProlateRay(s.envelope_,s.complexPoint_,P(s.direction_));
        if(!hit||hit->exitDistanceMetres<0) throw std::runtime_error("complex lost its envelope");
        distance=hit->exitDistanceMetres;envelopeExit=hit->exitPointMetres;envelopeNormal=hit->exitOutwardNormal;
    }
    std::size_t selected=ComplexLayout::MaximumChildren;
    std::optional<ParticleChord> selectedHit;
    for(std::size_t index=0;index<children.size();++index) {
        // A convex child just transmitted outward cannot be re-entered by the
        // unchanged host direction. This topological exclusion removes its
        // known zero surface root, without shifting the ray or hiding another
        // child's touching entry. Any reflection clears this exclusion.
        if(index==s.lastExitedChild_) continue;
        const auto& child=children[index];
        const bool ownedEntry=s.complexEntryChord_&&IsEnvelopeSphere(child,s.envelope_);
        const auto hit=ownedEntry ? std::optional<ParticleChord>{ParticleChord{0,distance,distance,s.complexPoint_,envelopeExit,
            P(Unit(V(s.complexPoint_))),envelopeNormal}} :
            IntersectProlateRay(child.particle,P(Add(V(s.complexPoint_),Mul(V(child.centreFromEnvelopeMetres),-1))),P(s.direction_));
        if(!hit||hit->chordLengthMetres==0) continue;
        if(hit->entryDistanceMetres<0) throw std::runtime_error("complex host lies inside unowned child");
        if(!ownedEntry&&Dot(s.direction_,V(hit->entryOutwardNormal))>=0) continue;
        // Stable array order resolves equal child entries. Strictly entering
        // child boundaries own ties with the envelope's outward boundary.
        if(hit->entryDistanceMetres<distance||(hit->entryDistanceMetres==distance&&selected==ComplexLayout::MaximumChildren)) {
            distance=hit->entryDistanceMetres;selected=index;selectedHit=hit;
        }
    }
    const double absorption=layers_[static_cast<std::size_t>(s.layer_)].hostAbsorptionPerMetre;
    bool absorbed=false;
    if(estimator_==AbsorptionEstimator::Analog&&absorption>0) {
        double u;if(!Draw(rng,u)) {s.termination_=ForwardTermination::InvalidSample;return;}
        const double proposal=*SampleExponentialFreePath(absorption,u);
        if(proposal<distance) {distance=proposal;absorbed=true;}
    }
    ++s.counters_.complexHostSegments;
    if(!Flight(s,distance,absorption,false,absorbed)) return;
    s.complexEntryChord_=false;
    if(selected!=ComplexLayout::MaximumChildren) {
        const auto& child=children[selected];
        s.activeChild_=selected;s.particle_=child.particle;s.independentProjectedChord_=false;
        s.particlePoint_=selectedHit->entryPointMetres;s.particleExit_=selectedHit->exitPointMetres;
        s.exitNormal_=selectedHit->exitOutwardNormal;s.chordDistance_=selectedHit->chordLengthMetres;
        s.complexPoint_=P(Add(V(child.centreFromEnvelopeMetres),V(s.particlePoint_)));
        ++s.counters_.complexChildEntries;s.phase_=ForwardPhase::ParticleChord;
        // Bare position/Fan proofs do not cross an unproved host/child chain.
        s.angularSupport_.boundaryPositionAreaAC=s.angularSupport_.boundaryPhaseAC;
        s.angularSupport_.cosinePointSourceChord=false;
    } else {
        s.complexPoint_=envelopeExit;s.particle_=s.envelope_;
        s.particlePoint_=s.complexPoint_;s.exitNormal_=envelopeNormal;
        s.perturbationAxis_=s.direction_;s.complexActive_=false;
        ++s.counters_.complexExits;
        s.angularSupport_.boundaryPositionAreaAC=s.angularSupport_.boundaryPhaseAC||s.angularSupport_.untouchedComplexProjectedBeam;
        s.angularSupport_.cosinePointSourceChord=false;
        if(children.empty()) FinishForwardScatter(s);
        else s.phase_=ForwardPhase::ForwardPerturbation;
    }
}
void HyLIoSForwardTransport::ParticleFlight(ForwardState& s,ISampler& rng) const {
    const auto& o=*layers_[static_cast<std::size_t>(s.layer_)].organelle;
    double u;
    if(estimator_==AbsorptionEstimator::Analog&&o.absorptionPerMetre>0) {
        if(!Draw(rng,u)) {s.termination_=ForwardTermination::InvalidSample;return;}
        const double distance=*SampleExponentialFreePath(o.absorptionPerMetre,u);
        if(distance<s.chordDistance_) {s.particlePoint_=P(Add(V(s.particlePoint_),Mul(s.direction_,distance)));Flight(s,distance,o.absorptionPerMetre,true,true);return;}
    }
    if(!Flight(s,s.chordDistance_,o.absorptionPerMetre,true,false)) return;
    s.particlePoint_=s.particleExit_;
    if(s.complexActive_) s.complexPoint_=P(Add(V(s.complexLayout_->Children()[s.activeChild_].centreFromEnvelopeMetres),V(s.particlePoint_)));
    // Absorption precedes the sole Fresnel test. Entry has no extra Fresnel.
    const auto optics=ParticleDielectric(s.direction_,s.exitNormal_,o.realIOR,layers_[static_cast<std::size_t>(s.layer_)].realIOR);
    if(!Draw(rng,u)) {s.termination_=ForwardTermination::InvalidSample;return;}
    // Commit certificates only after a successful chord and valid Fresnel draw.
    // A cosine fan supplies area density at its far point by radial projection;
    // it need not have supplied full phase density at its starting point.
    const bool phaseAC=s.angularSupport_.boundaryPhaseAC;
    const bool beam=s.complexActive_&&s.angularSupport_.untouchedComplexProjectedBeam;
    const bool projected=s.independentProjectedChord_||beam;
    const bool fan=s.angularSupport_.cosinePointSourceChord;
    const bool pointAC=phaseAC||projected||fan;
    if(u<optics.reflectance) {
        s.direction_=optics.reflectedDirection;s.perturbationAxis_=s.direction_;
        if(phaseAC)
            s.angularLineage_={ForwardAngularClass::AbsolutelyContinuous,ForwardAngularReason::BoundaryPhaseReflection};
        else if(projected)
            s.angularLineage_={ForwardAngularClass::AbsolutelyContinuous,beam ?
                ForwardAngularReason::ProjectedComplexReflection : ForwardAngularReason::ProjectedParticleReflection};
        else
            s.angularLineage_={ForwardAngularClass::UnclassifiedSingular,ForwardAngularReason::RetainedCurvedReflection};
        s.angularSupport_.boundaryPositionAreaAC=pointAC;
        s.angularSupport_.cosinePointSourceChord=false;
        s.angularSupport_.untouchedComplexProjectedBeam=false;
        s.independentProjectedChord_=false;
        s.phase_=ForwardPhase::ReflectedPerturbation;++s.counters_.particleReflections;
        if(s.complexActive_) {++s.counters_.complexChildReflections;s.lastExitedChild_=ComplexLayout::MaximumChildren;}
    } else {
        s.direction_=optics.transmittedDirection;s.perturbationAxis_=s.direction_;
        // Exact index equality is an identity law, never a vector comparison.
        if(o.realIOR!=layers_[static_cast<std::size_t>(s.layer_)].realIOR) {
            if(phaseAC)
                s.angularLineage_={ForwardAngularClass::AbsolutelyContinuous,ForwardAngularReason::BoundaryPhaseTransmission};
            else if(projected)
                s.angularLineage_={ForwardAngularClass::AbsolutelyContinuous,beam ?
                    ForwardAngularReason::ProjectedComplexTransmission : ForwardAngularReason::ProjectedParticleTransmission};
            else if(fan&&s.particle_.semiMinorMetres==s.particle_.semiMajorMetres)
                s.angularLineage_={ForwardAngularClass::AbsolutelyContinuous,ForwardAngularReason::PointSourceSphereTransmission};
            else if(fan&&o.realIOR<layers_[static_cast<std::size_t>(s.layer_)].realIOR)
                s.angularLineage_={ForwardAngularClass::AbsolutelyContinuous,ForwardAngularReason::PointSourceLowIndexTransmission};
            else
                s.angularLineage_={ForwardAngularClass::UnclassifiedSingular,ForwardAngularReason::RetainedCurvedTransmission};
            s.angularSupport_.untouchedComplexProjectedBeam=false;
        }
        s.angularSupport_.boundaryPositionAreaAC=s.complexActive_ ? phaseAC : pointAC;
        s.angularSupport_.cosinePointSourceChord=false;
        s.independentProjectedChord_=false;
        ++s.counters_.particleExits;
        if(s.complexActive_) {
            ++s.counters_.complexChildExits;s.lastExitedChild_=s.activeChild_;
            s.activeChild_=ComplexLayout::MaximumChildren;s.phase_=ForwardPhase::ComplexHostFlight;
        } else s.phase_=ForwardPhase::ForwardPerturbation;
    }
}
void HyLIoSForwardTransport::Perturb(ForwardState& s,ISampler& rng) const {
    double u,v,a;
    if(s.phase_==ForwardPhase::ForwardPerturbation&&forwardAngle_==0) {FinishForwardScatter(s);return;}
    ++s.counters_.perturbationProposals;
    if(!Draw(rng,u)||!Draw(rng,v)) {s.termination_=ForwardTermination::InvalidSample;return;}
    if(s.phase_==ForwardPhase::ReflectedPerturbation) {
        const auto direction=PolarComponents(s.perturbationAxis_,std::sqrt(1-u),std::sqrt(u),2*Pi*v);
        if(Dot(direction,V(s.exitNormal_))>=0) return; // one conditional-inward proposal
        s.direction_=direction;
        SurfaceExit(s.particle_,s.particlePoint_,direction,s.chordDistance_,s.particleExit_,s.exitNormal_);
        // Fresh conditional angle density times a proved area-density point
        // gives P4. Otherwise retain only the point-source Fan certificate.
        const bool phaseAC=s.angularSupport_.boundaryPhaseAC||s.angularSupport_.boundaryPositionAreaAC;
        s.angularSupport_.boundaryPhaseAC=phaseAC;
        s.angularSupport_.boundaryPositionAreaAC=phaseAC;
        s.angularSupport_.cosinePointSourceChord=true;
        s.angularSupport_.untouchedComplexProjectedBeam=false;
        s.angularLineage_={ForwardAngularClass::AbsolutelyContinuous,phaseAC ?
            ForwardAngularReason::ReflectedCosinePhaseAcquired : ForwardAngularReason::ReflectedCosine};
        s.independentProjectedChord_=false;s.phase_=ForwardPhase::ParticleChord;return;
    }
    if(s.phase_==ForwardPhase::FiberPerturbation) {
        const auto cosine=TrySampleRayleighCosine(u,v);if(!cosine) return;
        if(!Draw(rng,a)) {s.termination_=ForwardTermination::InvalidSample;return;}
        s.direction_=PolarComponents(s.perturbationAxis_,*cosine,2*std::sqrt(u)*std::sqrt(1-u),2*Pi*a);
        s.angularLineage_={ForwardAngularClass::AbsolutelyContinuous,ForwardAngularReason::FiberRayleigh};
        s.phase_=ForwardPhase::BulkFlight;return;
    }
    if(!Draw(rng,a)) {s.termination_=ForwardTermination::InvalidSample;return;}
    const auto angle=TrySampleExponentialForward(forwardAngle_,u,v,a);if(!angle) return;
    if(!Draw(rng,a)) {s.termination_=ForwardTermination::InvalidSample;return;}
    // Preserve the sampled transverse component when cos(theta) rounds to one.
    // This applies the same ideal angular law; generic sub-ULP rotations can
    // still round at the final Cartesian output, as with any binary64 vector.
    s.direction_=PolarComponents(s.perturbationAxis_,std::cos(angle->thetaRadians),std::sin(angle->thetaRadians),2*Pi*a);
    s.angularLineage_={ForwardAngularClass::AbsolutelyContinuous,ForwardAngularReason::ExponentialForward};
    FinishForwardScatter(s);
}
void HyLIoSForwardTransport::FinishForwardScatter(ForwardState& s) const {
    if(Dot(s.direction_,V(s.exitNormal_))<0) {
        ++s.counters_.inwardPostExitEvents;
        if(postScatterPolicy_==ForwardPostScatterPolicy::DiagnosticRetainParticleV1) {
            // Preserve the full draw, active particle and live power for diagnosis.
            s.termination_=ForwardTermination::ForwardPerturbationReentry;return;
        }
    }
    if(postScatterPolicy_==ForwardPostScatterPolicy::DiscardAfterScatterV1) {
        // The virtual particle no longer owns any future travel or absorption.
        // Its independent predecessor copy remains solely for placement exclusion.
        s.particle_={};s.particlePoint_={};s.particleExit_={};s.exitNormal_={};
        s.chordDistance_=0;s.perturbationAxis_={};s.complexLayout_.reset();
    }
    // The autonomous body no longer owns the ray. Its frame-specific support
    // cannot authorize future host/candidate geometry, including diagnostic
    // outward completion. An unresolved diagnostic reentry returned above.
    s.angularSupport_={};s.independentProjectedChord_=false;
    s.phase_=ForwardPhase::BulkFlight;
}
ForwardAdvanceResult HyLIoSForwardTransport::Advance(ForwardState& s,ISampler& rng,std::size_t quantum) const {
    Require(s.owner_==this,"state belongs to another kernel");
    Require(!rng.HasFixedDimensionBudget(),"forward transport requires variable sampler budget");
    if(s.pending_||s.termination_!=ForwardTermination::None) return Result(s);
    try {
        for(std::size_t work=0;work<quantum;++work) {
            ++s.counters_.workUnits;
            // Roulette at segment/proposal boundaries is explicit and compensated.
            if(!Roulette(s,rng)) return Result(s);
            switch(s.phase_) {
                case ForwardPhase::BulkFlight: Bulk(s,rng);break;
                case ForwardPhase::CandidatePlacement: Placement(s,rng);break;
                case ForwardPhase::ComplexLayout: BuildComplex(s,rng);break;
                case ForwardPhase::ComplexHostFlight: ComplexHost(s,rng);break;
                case ForwardPhase::ParticleChord: ParticleFlight(s,rng);break;
                default: Perturb(s,rng);break;
            }
            if(s.pending_||s.termination_!=ForwardTermination::None) return Result(s);
        }
    } catch(const std::exception&) {s.termination_=ForwardTermination::NumericalFailure;s.failureDetail_=ForwardFailureDetail::ArithmeticOrGeometry;}
    return Result(s);
}
}
