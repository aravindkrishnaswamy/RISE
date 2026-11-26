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
SlabVector Polar(SlabVector axis,double cosine,double azimuth) {
    const double radius=std::sqrt(std::max(0.,(1-cosine)*(1+cosine)));
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
HyLIoSForwardTransport::HyLIoSForwardTransport(std::array<ForwardLayer,6> layers,double wavelengthNm,
    double exteriorIOR,double fiberRate,AbsorptionEstimator estimator,double rouletteTarget,ForwardPlacementPolicy placementPolicy):
    layers_(layers),wavelengthNm_(wavelengthNm),exteriorIOR_(exteriorIOR),fiberRate_(fiberRate),
    rouletteTarget_(rouletteTarget),forwardAngle_(wavelengthNm<780 ? 5*Pi/180 : wavelengthNm<1400 ? (5*Pi/180)*(1400-wavelengthNm)/620 : 0),estimator_(estimator),placementPolicy_(placementPolicy) {
    Require(placementPolicy==ForwardPlacementPolicy::FixedEventDiagnosticV1||placementPolicy==ForwardPlacementPolicy::NullCollisionPlacementV1,"invalid placement policy");
    Require(Finite(wavelengthNm)&&wavelengthNm>0&&Finite(exteriorIOR)&&exteriorIOR>0,"invalid wavelength/exterior IOR");
    Require(Finite(fiberRate)&&fiberRate>=0,"invalid PD fiber rate");
    Require(estimator==AbsorptionEstimator::Analog||estimator==AbsorptionEstimator::Weighted,"invalid estimator");
    Require(Finite(rouletteTarget)&&rouletteTarget>=0&&(estimator==AbsorptionEstimator::Weighted||rouletteTarget==0),"invalid roulette target");
    for(std::size_t i=0;i<6;++i) {
        const auto& l=layers_[i];
        Require(Finite(l.thicknessMetres)&&l.thicknessMetres>0&&Finite(l.hostAbsorptionPerMetre)&&l.hostAbsorptionPerMetre>=0&&Finite(l.realIOR)&&l.realIOR>0,"invalid layer");
        planes_[i+1]=planes_[i]+l.thicknessMetres;
        Require(Finite(planes_[i+1])&&planes_[i+1]>planes_[i],"layer planes not representably separated");
        if(l.organelle) {
            const auto& o=*l.organelle;
            Require(o.organization==ForwardOrganization::IndividualSpheroids,"complex transport is unsupported");
            Require(Finite(o.volumeFraction)&&o.volumeFraction>=0&&o.volumeFraction<=1&&Finite(o.absorptionPerMetre)&&o.absorptionPerMetre>=0&&Finite(o.realIOR)&&o.realIOR>0,"invalid organelle coefficients");
            geometricRates_[i]=GeometricEncounterRate(ProlateSurfaceToVolume(o.semiMinorMetres,o.semiMajorMetres),o.volumeFraction);
            if(o.volumeFraction>0) Require(o.semiMinorMetres<l.thicknessMetres/2,"layer cannot contain positive-measure spheroid placements");
        }
    }
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
    s.position_=position;s.counters_.pathLengthMetres+=distance;s.counters_.layerPathLengthMetres[i]+=distance;path+=distance;++s.counters_.flights;
    if(s.hasPredecessor_) s.predecessorCentreFromPosition_=P(Add(V(s.predecessorCentreFromPosition_),Mul(step,-1)));
    auto& tally=inside ? s.particleAbsorbed_[i] : s.hostAbsorbed_[i];
    if(absorbed) {tally+=s.weight_;s.weight_=0;s.termination_=ForwardTermination::Absorbed;return false;}
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
            if(o.semiMinorMetres==o.semiMajorMetres) {
                const double az=std::abs(s.direction_.z);
                const double margin=o.semiMinorMetres*(az*az/(1+std::sqrt((1-az)*(1+az))));
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
        s.direction_=Polar({0,0,-1},std::sqrt(1-u),2*Pi*v);++s.counters_.bottomReflections;return;
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
    double u,v,r,a;if(!Draw(rng,u)||!Draw(rng,v)||!Draw(rng,r)||!Draw(rng,a)) {s.termination_=ForwardTermination::InvalidSample;return;}
    ++s.counters_.placementAttempts;
    const ProlateParticle particle{o.semiMinorMetres,o.semiMajorMetres,P(Polar(s.direction_,SampleOrientationCosine(u,o.semiMinorMetres,o.semiMajorMetres),2*Pi*v))};
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
    s.chordDistance_=hit.chord.chordLengthMetres;++s.counters_.acceptedParticles;s.phase_=ForwardPhase::ParticleChord;
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
    // Absorption precedes the sole Fresnel test. Entry has no extra Fresnel.
    const auto optics=ParticleDielectric(s.direction_,s.exitNormal_,o.realIOR,layers_[static_cast<std::size_t>(s.layer_)].realIOR);
    if(!Draw(rng,u)) {s.termination_=ForwardTermination::InvalidSample;return;}
    if(u<optics.reflectance) {
        s.direction_=optics.reflectedDirection;s.perturbationAxis_=s.direction_;
        s.phase_=ForwardPhase::ReflectedPerturbation;++s.counters_.particleReflections;
    } else {
        s.direction_=optics.transmittedDirection;s.perturbationAxis_=s.direction_;
        s.phase_=ForwardPhase::ForwardPerturbation;++s.counters_.particleExits;
    }
}
void HyLIoSForwardTransport::Perturb(ForwardState& s,ISampler& rng) const {
    double u,v,a;
    if(s.phase_==ForwardPhase::ForwardPerturbation&&forwardAngle_==0) {s.phase_=ForwardPhase::BulkFlight;return;}
    ++s.counters_.perturbationProposals;
    if(!Draw(rng,u)||!Draw(rng,v)) {s.termination_=ForwardTermination::InvalidSample;return;}
    if(s.phase_==ForwardPhase::ReflectedPerturbation) {
        const auto direction=Polar(s.perturbationAxis_,std::sqrt(1-u),2*Pi*v);
        if(Dot(direction,V(s.exitNormal_))>=0) return; // one conditional-inward proposal
        s.direction_=direction;
        SurfaceExit(s.particle_,s.particlePoint_,direction,s.chordDistance_,s.particleExit_,s.exitNormal_);
        s.phase_=ForwardPhase::ParticleChord;return;
    }
    if(s.phase_==ForwardPhase::FiberPerturbation) {
        const auto cosine=TrySampleRayleighCosine(u,v);if(!cosine) return;
        if(!Draw(rng,a)) {s.termination_=ForwardTermination::InvalidSample;return;}
        s.direction_=Polar(s.perturbationAxis_,*cosine,2*Pi*a);s.phase_=ForwardPhase::BulkFlight;return;
    }
    if(!Draw(rng,a)) {s.termination_=ForwardTermination::InvalidSample;return;}
    const auto angle=TrySampleExponentialForward(forwardAngle_,u,v,a);if(!angle) return;
    if(!Draw(rng,a)) {s.termination_=ForwardTermination::InvalidSample;return;}
    s.direction_=Polar(s.perturbationAxis_,std::cos(angle->thetaRadians),2*Pi*a);
    if(Dot(s.direction_,V(s.exitNormal_))<0) {
        // The publication does not define how post-exit angular perturbation
        // re-enters its own particle. Preserve drawn direction/weight and expose
        // unresolved work; neither truncate nor silently condition the law.
        s.termination_=ForwardTermination::ForwardPerturbationReentry;return;
    }
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
                case ForwardPhase::ParticleChord: ParticleFlight(s,rng);break;
                default: Perturb(s,rng);break;
            }
            if(s.pending_||s.termination_!=ForwardTermination::None) return Result(s);
        }
    } catch(const std::exception&) {s.termination_=ForwardTermination::NumericalFailure;s.failureDetail_=ForwardFailureDetail::ArithmeticOrGeometry;}
    return Result(s);
}
}
