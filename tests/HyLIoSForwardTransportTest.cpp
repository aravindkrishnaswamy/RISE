#include "../src/Library/Materials/HyLIoS/HyLIoSForwardTransport.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <stdexcept>
#include <vector>
using namespace RISE::HyLIoS;
namespace {
int checks=0;
void Check(bool value,const char* message) {++checks;if(!value) throw std::runtime_error(message);}
void Near(double actual,double expected,double tolerance,const char* message) {Check(std::abs(actual-expected)<=tolerance,message);}
template<class F> void Reject(F f,const char* message) {bool rejected=false;try {f();}catch(const std::invalid_argument&){rejected=true;}Check(rejected,message);}
struct Random: RISE::ISampler {
    std::uint64_t bits,draws=0;explicit Random(std::uint64_t seed):bits(seed){}
    double Get1D() override {++draws;bits+=0x9e3779b97f4a7c15ULL;auto z=bits;z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL;z=(z^(z>>27))*0x94d049bb133111ebULL;z^=z>>31;return static_cast<double>(z>>11)*0x1p-53;}
    RISE::Point2 Get2D() override {return {Get1D(),Get1D()};}
};
struct Sequence: RISE::ISampler {
    std::vector<double> values;std::size_t index=0;explicit Sequence(std::initializer_list<double> v):values(v){}
    double Get1D() override {if(index==values.size()) throw std::runtime_error("script exhausted");return values[index++];}
    RISE::Point2 Get2D() override {return {Get1D(),Get1D()};}
};
struct Fixed: Random {using Random::Random;bool HasFixedDimensionBudget()const override{return true;}};
std::array<ForwardLayer,6> Layers(double absorption=0,double index=1) {std::array<ForwardLayer,6> l{};for(auto& v:l) v={.001,absorption,index,std::nullopt};return l;}
std::array<SlabLayer,6> Slabs(double absorption=0,double index=1) {std::array<SlabLayer,6> l{};for(auto& v:l) v={.001,absorption,index};return l;}
std::array<ForwardLayer,6> Particles(double host=0,double particle=0,double radius=.0001) {
    auto l=Layers(host);l[0].organelle=ForwardOrganelle{ForwardOrganization::IndividualSpheroids,.2,radius,radius,particle,1};return l;
}
ForwardState Enter(const HyLIoSForwardTransport& k,SlabVector p={0,0,0},SlabVector d={0,0,1}) {
    auto s=k.Begin(p,d);auto e=s.PendingBoundary();k.ResumeBoundary(s,e.token,SampleSlabSmoothDielectric(e,.9));return s;
}
ForwardState Run(const HyLIoSForwardTransport& k,RISE::ISampler& rng,std::size_t quantum=1000) {
    auto s=k.Begin({0,0,0},{0,0,1});for(int calls=0;calls<100000;++calls) {
        const auto r=k.Advance(s,rng,quantum);if(r.kind==ForwardAdvanceKind::Terminal)return s;
        if(r.kind==ForwardAdvanceKind::OuterBoundaryEncounter){auto e=s.PendingBoundary();k.ResumeBoundary(s,e.token,SampleSlabSmoothDielectric(e,rng.Get1D()));}
    }std::cerr<<"phase="<<static_cast<int>(s.Phase())<<" layer="<<s.Layer()<<" accepted="<<s.Counters().acceptedParticles<<" reject="<<s.Counters().overlapRejections<<" containment="<<s.Counters().containmentRejections<<" z="<<s.Position().z<<" dz="<<s.Direction().z<<"\n";throw std::runtime_error("unfinished history");
}
SlabState RunSlab(const HyLIoSSlabTransport& k,RISE::ISampler& rng) {
    auto s=k.Begin({0,0,0},{0,0,1});for(int calls=0;calls<10000;++calls){auto r=k.Advance(s,rng,1000);if(r.kind==SlabAdvanceKind::Terminal)return s;if(r.kind==SlabAdvanceKind::OuterBoundaryEncounter){auto e=s.PendingBoundary();k.ResumeBoundary(s,e.token,SampleSlabSmoothDielectric(e,rng.Get1D()));}}throw std::runtime_error("unfinished slab");
}
double Sum(const std::array<double,6>& a){double s=0;for(double v:a)s+=v;return s;}
double Absorbed(const ForwardState& s){return Sum(s.HostAbsorbedByLayer())+Sum(s.ParticleAbsorbedByLayer());}
bool Returned(const ForwardState& s){return s.Termination()==ForwardTermination::SubsurfaceReturn||s.Termination()==ForwardTermination::ExternalReflection;}
struct Moments {double sum=0,sum2=0;std::size_t n=0;void Add(double x){sum+=x;sum2+=x*x;++n;}double Mean()const{return sum/static_cast<double>(n);}double SE()const{return std::sqrt(std::max(0.,(sum2-sum*sum/static_cast<double>(n))/static_cast<double>(n-1)/static_cast<double>(n)));}};
void ExactControls() {
    HyLIoSForwardTransport k(Layers(),1400,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::FixedEventDiagnosticV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);Random rng(4);
    for(int i=0;i<100;++i){auto s=Run(k,rng);Check(Returned(s)&&s.Weight()==1&&Absorbed(s)==0,"matched vacuum conserves power");Near(s.Counters().pathLengthMetres,.006*(1+1/std::abs(s.Direction().z)),1e-16,"six layer geometric path oracle");Check(s.Counters().acceptedParticles==0&&s.Counters().fiberEvents==0,"zero rates remove both processes");}
    auto s=k.Begin({0,0,0},{0,0,1});Fixed fixed(3);Reject([&]{k.Advance(s,fixed,1);},"fixed budget refused");Check(fixed.draws==0,"fixed budget refusal consumes no RNG");
    const auto e=s.PendingBoundary();Random noDraw(7);k.Advance(s,noDraw,100);Check(noDraw.draws==0,"pending boundary does not draw");
    Reject([&]{k.ResumeBoundary(s,e.token+1,SampleSlabSmoothDielectric(e,.5));},"token mismatch rejected");
    k.Cancel(s);Check(s.Termination()==ForwardTermination::Cancelled&&s.Weight()==1&&Absorbed(s)==0,"cancelled work is unresolved live power");
    Reject([&]{k.Begin({1e10,0,0},{0,0,1});},"scene translation must remain outside microscopic kernel");
    auto invalid=Layers();invalid[0].organelle=ForwardOrganelle{ForwardOrganization::UnsupportedComplexes,0,.0001,.0001,0,1};
    Reject([&]{HyLIoSForwardTransport bad(invalid,550,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::FixedEventDiagnosticV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);},"complexes explicitly unsupported even at zero authored fraction");
    invalid=Particles(0,0,.0005);Reject([&]{HyLIoSForwardTransport bad(invalid,550,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::FixedEventDiagnosticV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);},"minimum diameter cannot fit layer");
    auto a=Enter(k);Sequence badSample{1};k.Advance(a,badSample,1);Check(a.Termination()==ForwardTermination::InvalidSample&&a.Weight()==1&&Absorbed(a)==0,"invalid RNG is not absorption");
}
void AbsorbedTallyOverflow() {
    // A weighted round trip leaves a finite host absorption tally. A caller's
    // valid boundary proposal then promotes live power close to DBL_MAX and
    // sends it into an absorbing tangent, which samples complete absorption.
    // The final addition must fail before changing tally or live power.
    HyLIoSForwardTransport k(Layers(1),1400,1,0,AbsorptionEstimator::Weighted,0,ForwardPlacementPolicy::FixedEventDiagnosticV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);
    auto s=k.Begin({0,0,0},{0,0,1});auto e=s.PendingBoundary();
    k.ResumeBoundary(s,e.token,{BoundaryAction::Transmit,SlabSide::Interior,{0,0,1},1e308});
    Sequence roundTrip{.5,.5,.5,.5,.5,0,0,.5,.5,.5,.5,.5};k.Advance(s,roundTrip,12);
    Check(s.HasPendingBoundary(),"hylios_absorbed_tally_overflow reaches return boundary");
    const double before=s.HostAbsorbedByLayer()[0];
    Check(std::isfinite(before)&&before>0,"hylios_absorbed_tally_overflow starts with finite nonzero tally");
    e=s.PendingBoundary();k.ResumeBoundary(s,e.token,{BoundaryAction::Reflect,SlabSide::Interior,{1,0,0},std::numeric_limits<double>::max()/s.Weight()});
    const double live=s.Weight();Check(std::isfinite(live)&&live>1e308,"hylios_absorbed_tally_overflow starts with finite live weight");
    Sequence absorb{.5};k.Advance(s,absorb,1);
    Check(s.Termination()==ForwardTermination::NumericalFailure,"hylios_absorbed_tally_overflow is numerical failure, not physical absorption");
    Check(s.HostAbsorbedByLayer()[0]==before&&std::isfinite(s.HostAbsorbedByLayer()[0]),"hylios_absorbed_tally_overflow preserves finite prior tally");
    Check(s.Weight()==live,"hylios_absorbed_tally_overflow preserves unresolved live weight");
    Check(s.FailureDetail()==ForwardFailureDetail::ArithmeticOrGeometry,"hylios_absorbed_tally_overflow retains arithmetic diagnostic");
    std::cout<<"PASS hylios_absorbed_tally_overflow\n";
}
void ParticleOracles() {
    constexpr double radius=.0001,rate=3*.2/(4*radius),distance=.0003;
    HyLIoSForwardTransport k(Particles(200,1000),1400,1,0,AbsorptionEstimator::Weighted,0,ForwardPlacementPolicy::FixedEventDiagnosticV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);
    auto s=Enter(k);Sequence travel{-std::expm1(-rate*distance)};k.Advance(s,travel,1);
    Check(s.Phase()==ForwardPhase::CandidatePlacement&&s.Counters().acceptedParticles==0,"candidate placement phase freezes event");
    Sequence place{.5,0,0,0};k.Advance(s,place,1);Check(s.Phase()==ForwardPhase::ParticleChord&&s.Counters().acceptedParticles==1,"area uniform centre ray accepted");
    Sequence exit{.5};k.Advance(s,exit,1);
    Near(s.Counters().particlePathLengthMetres[0],2*radius,2e-19,"sphere central real chord");
    Near(s.Counters().hostPathLengthMetres[0],distance,2e-19,"distance is particle entry not centre");
    Near(s.Weight(),std::exp(-.06-.2),2e-15,"host and particle Beer each exactly once");
    Near(s.HostAbsorbedByLayer()[0],-std::expm1(-.06),2e-15,"host absorption excludes organelle chord");
    Near(s.ParticleAbsorbedByLayer()[0],std::exp(-.06)*(-std::expm1(-.2)),2e-15,"separate particle absorption tally");
    Near(Absorbed(s)+s.Weight(),1,2e-15,"weighted partial history conservation");
    Sequence none{};k.Advance(s,none,1);Check(s.Phase()==ForwardPhase::BulkFlight&&none.index==0,"1400nm forward delta consumes no angular RNG");
    // True spheroid exclusion uses the accepted predecessor, never a rejected
    // proposal. A second off-centre sphere at the previous exit overlaps it.
    HyLIoSForwardTransport overlapKernel(Particles(),1400,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::FixedEventDiagnosticV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);
    auto overlapState=Enter(overlapKernel);Sequence ot{.3};overlapKernel.Advance(overlapState,ot,1);
    Sequence op{.5,0,0,0};overlapKernel.Advance(overlapState,op,1);Sequence oe{.5};overlapKernel.Advance(overlapState,oe,1);
    Sequence od{};overlapKernel.Advance(overlapState,od,1);Sequence oz{-std::expm1(-rate*1e-6)};overlapKernel.Advance(overlapState,oz,1);
    Sequence ob{.5,0,.9,0};overlapKernel.Advance(overlapState,ob,1);
    Check(overlapState.Counters().overlapRejections==1&&overlapState.Counters().acceptedParticles==1&&overlapState.Phase()==ForwardPhase::CandidatePlacement,"true predecessor overlap rejects without replacing predecessor");
    Sequence og{.5,0,0,0};overlapKernel.Advance(overlapState,og,1);
    Check(overlapState.Counters().acceptedParticles==2,"nonoverlapping retry tested against original accepted predecessor");
    HyLIoSForwardTransport nullKernel(Particles(),1400,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);
    auto nullState=Enter(nullKernel);Sequence nt{-std::expm1(-rate*.00001)};nullKernel.Advance(nullState,nt,1);
    Sequence np{.5,0,.99,0};nullKernel.Advance(nullState,np,1);
    Check(nullState.Phase()==ForwardPhase::BulkFlight&&nullState.Counters().nullPlacements==1&&nullState.Counters().acceptedParticles==0&&nullState.Weight()==1,"explicit null collision consumes rejected event without inventing absorption");
    auto zeroNull=Enter(nullKernel);Sequence zn{0};nullKernel.Advance(zeroNull,zn,1);Sequence zp{.5,0,.99,0};nullKernel.Advance(zeroNull,zp,1);
    Check(zeroNull.Phase()==ForwardPhase::BulkFlight&&zeroNull.Counters().workUnits==2,"zero uniform null placement uses bounded two phase work");
    // Fixed candidate near a boundary: first projected point cannot fit; retry
    // keeps the physical event, weight and sampled free-flight unchanged.
    HyLIoSForwardTransport c(Particles(),1400,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::FixedEventDiagnosticV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);auto p=Enter(c);
    Sequence early{-std::expm1(-rate*.00001)};c.Advance(p,early,1);const auto before=p.Position();
    Sequence reject{.5,0,.99,0};c.Advance(p,reject,1);Check(p.Counters().containmentRejections==1&&p.Phase()==ForwardPhase::CandidatePlacement,"whole sphere containment rejection");
    Check(p.Position().z==before.z&&p.Counters().hostPathLengthMetres[0]==before.z,"placement rejection freezes distance");
    Sequence accept{.5,0,0,0};c.Advance(p,accept,1);Check(p.Counters().acceptedParticles==1&&p.Counters().placementAttempts==2,"bounded retry resumes same candidate");
    auto boundary=Enter(c);Sequence zero{0};c.Advance(boundary,zero,1);Check(boundary.Termination()==ForwardTermination::UnplaceableBoundaryCandidate&&boundary.Weight()==1&&Absorbed(boundary)==0,"U0 impossible boundary placement retains unresolved weight");
    // Force grazing exit then an exponential forward direction pointing inward.
    HyLIoSForwardTransport reentry(Particles(),550,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::FixedEventDiagnosticV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);auto base=Enter(reentry);
    Sequence tr{-std::expm1(-rate*distance)};reentry.Advance(base,tr,1);Sequence pl{.5,0,.9999,0};reentry.Advance(base,pl,1);Sequence ex{.5};reentry.Advance(base,ex,1);
    bool found=false;for(int az=0;az<64;++az){auto trial=base;Sequence angle{.5,.5,0,static_cast<double>(az)/64};reentry.Advance(trial,angle,1);if(trial.Termination()==ForwardTermination::ForwardPerturbationReentry){found=true;Check(trial.Weight()==1&&Absorbed(trial)==0,"forced postexit reentry retains power without absorption");Check(trial.Direction().x!=base.Direction().x||trial.Direction().y!=base.Direction().y||trial.Direction().z!=base.Direction().z,"unresolved reentry retains sampled direction");break;}}
    Check(found,"synthetic grazing reentry topology exercised");
    // Physical axes + layer lengths scale together; optical rates scale inversely.
    auto scaled=Particles(100,500,.0002);for(auto& l:scaled)l.thicknessMetres*=2;
    HyLIoSForwardTransport scale(scaled,1400,1,0,AbsorptionEstimator::Weighted,0,ForwardPlacementPolicy::FixedEventDiagnosticV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);auto q=Enter(scale);Sequence tq{-std::expm1(-rate*distance)};scale.Advance(q,tq,1);Sequence pq{.5,0,0,0};scale.Advance(q,pq,1);Sequence eq{.5};scale.Advance(q,eq,1);
    Near(q.Weight(),s.Weight(),3e-15,"physical length coefficient scaling identity");
}
void DiscardAfterScatterControls() {
    constexpr double radius=.0001,rate=1500,entry=.0003,gap=1e-6,host=200,particle=1000;
    const auto layers=Particles(host,particle);
    for(auto policy:{ForwardPostScatterPolicy::DiagnosticRetainParticleV1,ForwardPostScatterPolicy::DiscardAfterScatterV1}) {
        HyLIoSForwardTransport k(layers,550,1,0,AbsorptionEstimator::Weighted,0,ForwardPlacementPolicy::NullCollisionPlacementV1,policy);
        auto s=Enter(k);Sequence travel{-std::expm1(-rate*entry)};k.Advance(s,travel,1);
        Sequence place{.5,0,0,0};k.Advance(s,place,1);Sequence exit{.5};k.Advance(s,exit,1);
        const double before=s.Weight(),particleAbsorbed=s.ParticleAbsorbedByLayer()[0];
        // Central sphere's exit normal is +z. This valid unconditioned polar
        // proposal has theta > pi/2, so its inward classification is independent
        // of the transport's geometry and classification implementation.
        Sequence angle{.99995,.99995,0,0};k.Advance(s,angle,1);
        constexpr double pi=3.141592653589793238462643383279502884;
        const double theta=-(5*pi/180)*2*std::log1p(-.99995);
        Check(theta>pi/2&&theta<pi,"hylios_discard_after_scatter forced proposal has independent inward geometry");
        Near(s.Direction().z,std::cos(theta),2e-15,"hylios_discard_after_scatter retains full polar draw");
        Check(angle.index==4&&s.Counters().inwardPostExitEvents==1,"hylios_discard_after_scatter counts one inward event without resampling");
        Check(s.Weight()==before&&s.ParticleAbsorbedByLayer()[0]==particleAbsorbed,"hylios_discard_after_scatter angular event adds no absorption");
        if(policy==ForwardPostScatterPolicy::DiagnosticRetainParticleV1) {
            Check(s.Termination()==ForwardTermination::ForwardPerturbationReentry,"diagnostic post-scatter retains unresolved inward event");
            continue;
        }
        Check(s.Termination()==ForwardTermination::None&&s.Phase()==ForwardPhase::BulkFlight,"hylios_discard_after_scatter resumes host even inward");
        // For the central sphere, the discarded interior ends at distance
        // -2*r*dz. Both host steps below are strictly inside its former volume.
        Check(2*gap < -2*radius*s.Direction().z,"hylios_discard_after_scatter host probes lie in former particle interior");
        for(int event=1;event<=2;++event) {
            Sequence next{-std::expm1(-rate*gap)};k.Advance(s,next,1);
            Near(s.Counters().hostPathLengthMetres[0],entry+event*gap,2e-18,"hylios_discard_after_scatter former interior is exact host distance");
            Near(s.Counters().particlePathLengthMetres[0],2*radius,2e-19,"hylios_discard_after_scatter never adds another particle chord");
            Near(s.Weight(),before*std::exp(-host*event*gap),3e-15,"hylios_discard_after_scatter former interior uses host Beer coefficient");
            Near(s.HostAbsorbedByLayer()[0],-std::expm1(-host*entry)+before*(-std::expm1(-host*event*gap)),3e-15,"hylios_discard_after_scatter host owns former interior absorption");
            Check(s.ParticleAbsorbedByLayer()[0]==particleAbsorbed,"hylios_discard_after_scatter former interior has no extra particle absorption");
            // A central new sphere entered inside the old sphere overlaps the
            // retained predecessor. Each rejected proposal is a null placement.
            Sequence overlap{.5,0,0,0};k.Advance(s,overlap,1);
            Check(s.Counters().overlapRejections==static_cast<std::uint64_t>(event)&&s.Counters().acceptedParticles==1&&s.Counters().nullPlacements==static_cast<std::uint64_t>(event),"hylios_discard_after_scatter retains accepted predecessor through rejected candidates");
        }
    }
    HyLIoSForwardTransport delta(layers,1400,1,0,AbsorptionEstimator::Weighted,0,ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiscardAfterScatterV1);
    auto d=Enter(delta);Sequence tr{.3};delta.Advance(d,tr,1);Sequence pl{.5,0,0,0};delta.Advance(d,pl,1);Sequence ex{.5};delta.Advance(d,ex,1);Sequence noAngular{};delta.Advance(d,noAngular,1);
    Check(noAngular.index==0&&d.Phase()==ForwardPhase::BulkFlight&&d.Counters().inwardPostExitEvents==0,"discard 1400nm delta consumes no angular draws");
    Reject([&]{HyLIoSForwardTransport bad(layers,550,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::NullCollisionPlacementV1,static_cast<ForwardPostScatterPolicy>(-1));},"invalid explicit post-scatter policy refused");
    std::cout<<"PASS hylios_discard_after_scatter exact controls\n";
}
void AnalogSphereSurvival() {
    auto layers=Particles(0,2500);HyLIoSForwardTransport k(layers,1400,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);
    Random rng(87878);int survivors=0;constexpr int count=20000;
    for(int i=0;i<count;++i) {
        auto s=Enter(k);Sequence travel{.3};k.Advance(s,travel,1);Sequence place{.5,0,0,0};k.Advance(s,place,1);k.Advance(s,rng,1);
        if(s.Termination()==ForwardTermination::None)++survivors;
        else Check(s.Termination()==ForwardTermination::Absorbed&&s.ParticleAbsorbedByLayer()[0]==1&&s.HostAbsorbedByLayer()[0]==0,"analog sphere absorption is counted exactly once");
    }
    const double expected=std::exp(-.5),mean=static_cast<double>(survivors)/count;
    Check(std::abs(mean-expected)<=5*std::sqrt(expected*(1-expected)/count),"analog central sphere survival matches independent Beer oracle");
    // Rotating launch azimuth about the layer normal leaves a central sphere
    // chord and separate absorption unchanged.
    HyLIoSForwardTransport weighted(Particles(20,2500),1400,1,0,AbsorptionEstimator::Weighted,0,ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);
    auto a=Enter(weighted,{0,0,0},{.2,.3,std::sqrt(.87)}),b=Enter(weighted,{0,0,0},{-.3,.2,std::sqrt(.87)});
    Sequence ta{.3},tb{.3};weighted.Advance(a,ta,1);weighted.Advance(b,tb,1);Sequence pa{.5,0,0,0},pb{.5,0,0,0};weighted.Advance(a,pa,1);weighted.Advance(b,pb,1);Sequence ea{.5},eb{.5};weighted.Advance(a,ea,1);weighted.Advance(b,eb,1);
    Near(a.Weight(),b.Weight(),1e-15,"sphere transport rotates about planar normal");Near(a.Counters().particlePathLengthMetres[0],.0002,1e-18,"oblique sphere chord uses real path not layer depth");
}
void SingleSphereReentryProbability() {
    for(auto policy:{ForwardPostScatterPolicy::DiagnosticRetainParticleV1,ForwardPostScatterPolicy::DiscardAfterScatterV1}) {
        HyLIoSForwardTransport k(Particles(),550,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::FixedEventDiagnosticV1,policy);
        Random rng(987123);constexpr int count=100000;int reentries=0,failures=0;
        constexpr double rate=1500,distance=.0003;
        for(int trial=0;trial<count;++trial) {
            auto s=Enter(k);Sequence travel{-std::expm1(-rate*distance)};k.Advance(s,travel,1);
            k.Advance(s,rng,1);k.Advance(s,rng,1);
            while(s.Termination()==ForwardTermination::None&&s.Phase()==ForwardPhase::ForwardPerturbation)k.Advance(s,rng,1);
            reentries+=static_cast<int>(s.Counters().inwardPostExitEvents);
            const auto expectedTermination=policy==ForwardPostScatterPolicy::DiagnosticRetainParticleV1&&s.Counters().inwardPostExitEvents>0 ? ForwardTermination::ForwardPerturbationReentry : ForwardTermination::None;
            failures+=s.Termination()!=expectedTermination;
            Check(s.Weight()==1&&Absorbed(s)==0,"matched first-sphere inward event keeps unit live power under both policies");
        }
        // Independent geometric integral: uniform projected sphere hits give
        // P(inward|theta)=(1-cos(theta))/2. Integrating against the normalized
        // exp(-theta/t)*sin(theta) polar law yields this exact probability.
        constexpr double pi=3.141592653589793238462643383279502884;
        const double t=5*pi/180;
        const double expected=(1-(1+t*t)/(1+4*t*t)*std::tanh(pi/(2*t)))/2;
        const double observed=static_cast<double>(reentries)/count;
        const double standardError=std::sqrt(expected*(1-expected)/count);
        Check(failures==0,"single matched sphere obeys explicit post-scatter policy without numerical/placement failures");
        Check(std::abs(observed-expected)<=5*standardError,"single sphere inward count matches independent projected-area angular integral under both policies");
        std::cout<<"sphere inward policy="<<static_cast<int>(policy)<<" n="<<count<<" count="<<reentries<<" observed="<<observed<<" exact="<<expected<<" SE="<<standardError<<" failures="<<failures<<'\n';
    }
}
void AbsorbingMismatchedSphereEstimators(double wavelength,ForwardPostScatterPolicy policy) {
    auto layers=Particles(40,2000);layers[0].organelle->realIOR=1.7;
    HyLIoSForwardTransport analog(layers,wavelength,1,500,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::NullCollisionPlacementV1,policy);
    HyLIoSForwardTransport weighted(layers,wavelength,1,500,AbsorptionEstimator::Weighted,0,ForwardPlacementPolicy::NullCollisionPlacementV1,policy);
    Moments analogReturn,weightedReturn,analogHost,weightedHost,analogParticle,weightedParticle;
    constexpr int count=12000;std::uint64_t analogReflections=0,weightedReflections=0;
    int analogRepeated=0,weightedRepeated=0,failures=0;std::uint64_t inwardEvents=0;
    for(int seed=1;seed<=count;++seed) {
        Random ra(static_cast<std::uint64_t>(seed)*23+9087),rw(static_cast<std::uint64_t>(seed)*31+2345);
        const auto a=Run(analog,ra),w=Run(weighted,rw);
        failures+=!(Returned(a)||a.Termination()==ForwardTermination::Absorbed);
        failures+=!(Returned(w)||w.Termination()==ForwardTermination::Absorbed);
        analogReturn.Add(Returned(a)?a.Weight():0);weightedReturn.Add(Returned(w)?w.Weight():0);
        analogHost.Add(Sum(a.HostAbsorbedByLayer()));weightedHost.Add(Sum(w.HostAbsorbedByLayer()));
        analogParticle.Add(Sum(a.ParticleAbsorbedByLayer()));weightedParticle.Add(Sum(w.ParticleAbsorbedByLayer()));
        inwardEvents+=a.Counters().inwardPostExitEvents+w.Counters().inwardPostExitEvents;
        analogReflections+=a.Counters().particleReflections;weightedReflections+=w.Counters().particleReflections;
        analogRepeated+=a.Counters().particleReflections>=2;weightedRepeated+=w.Counters().particleReflections>=2;
        Near(Absorbed(w)+w.Weight(),1,256*std::numeric_limits<double>::epsilon()*static_cast<double>(w.Counters().flights+1),"absorbing mismatched weighted host/particle accounting");
    }
    const auto Agreement=[](const Moments& a,const Moments& b){return std::abs(a.Mean()-b.Mean())<=5*std::hypot(a.SE(),b.SE());};
    Check(failures==0,"absorbing mismatched sphere estimators finish every history");
    Check(analogReflections>0&&weightedReflections>0&&analogRepeated>0&&weightedRepeated>0,"absorbing mismatched sphere exercises repeated internal reflections and chords");
    Check(Agreement(analogReturn,weightedReturn),"absorbing mismatched sphere escaped power agrees within combined sampling SE");
    Check(Agreement(analogHost,weightedHost),"absorbing mismatched sphere host absorption agrees within combined sampling SE");
    Check(Agreement(analogParticle,weightedParticle),"absorbing mismatched sphere particle absorption agrees within combined sampling SE");
    if(wavelength==550) Check(inwardEvents>0,"visible discard estimator agreement includes completed inward post-exit events");
    std::cout<<"absorbing mismatched sphere wavelength="<<wavelength<<" policy="<<static_cast<int>(policy)<<" inward="<<inwardEvents<<" n="<<count
        <<" R_analog="<<analogReturn.Mean()<<" SE="<<analogReturn.SE()<<" R_weighted="<<weightedReturn.Mean()<<" SE="<<weightedReturn.SE()
        <<" hostA_analog="<<analogHost.Mean()<<" SE="<<analogHost.SE()<<" hostA_weighted="<<weightedHost.Mean()<<" SE="<<weightedHost.SE()
        <<" particleA_analog="<<analogParticle.Mean()<<" SE="<<analogParticle.SE()<<" particleA_weighted="<<weightedParticle.Mean()<<" SE="<<weightedParticle.SE()
        <<" reflections_analog="<<analogReflections<<" reflections_weighted="<<weightedReflections
        <<" repeated_histories_analog="<<analogRepeated<<" repeated_histories_weighted="<<weightedRepeated<<" failures="<<failures<<'\n';
}
void FiberAndReflection() {
    HyLIoSForwardTransport k(Layers(),1400,1,1e8,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::FixedEventDiagnosticV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);auto s=Enter(k);
    Sequence interfaces{.5,.5,.5,.5};k.Advance(s,interfaces,4);Check(s.Layer()==4&&s.Counters().fiberPasses==1,"PD actual entry starts one pass");
    Sequence fiber{.5};k.Advance(s,fiber,1);Check(s.FiberUsedThisPass()&&s.Counters().fiberEvents==1,"fiber event marks pass before perturbation");
    Sequence normal{.999999,0,0};k.Advance(s,normal,1);Check(s.Phase()==ForwardPhase::BulkFlight,"Rayleigh one proposal resumes bulk");
    Sequence outgoing{.5};k.Advance(s,outgoing,1);Check(s.Layer()==5&&!s.FiberUsedThisPass(),"actual PD exit clears eligibility state");
    Sequence bottom{0,0};k.Advance(s,bottom,1);Sequence returnInterface{.5};k.Advance(s,returnInterface,1);Check(s.Layer()==4&&s.Counters().fiberPasses==2,"actual PD reentry starts second pass");
    Sequence second{.5};k.Advance(s,second,1);Check(s.Counters().fiberEvents==2,"one fiber event eligible on second pass");
    auto layers=Layers();layers[4].realIOR=1.5;HyLIoSForwardTransport refl(layers,1400,1,1e8,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::FixedEventDiagnosticV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);auto r=Enter(refl);
    Sequence entry{.5,.5,.5,.5};refl.Advance(r,entry,4);Sequence f{.5};refl.Advance(r,f,1);Sequence ray{.999999,0,0};refl.Advance(r,ray,1);
    Sequence reflect{0};refl.Advance(r,reflect,1);Check(r.Layer()==4&&r.FiberUsedThisPass()&&r.Counters().fiberEvents==1&&r.Counters().fiberPasses==1,"internal Fresnel reflection never resets fiber pass");
    // A high-index particle must reflect once at exit and perturb around the
    // specular reflected direction. No second Fresnel occurs in that proposal.
    auto parts=Particles();parts[0].organelle->realIOR=2;HyLIoSForwardTransport pk(parts,1400,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::FixedEventDiagnosticV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);auto p=Enter(pk);
    Sequence d{.3};pk.Advance(p,d,1);Sequence place{.5,0,0,0};pk.Advance(p,place,1);Sequence reflection{0};pk.Advance(p,reflection,1);
    Check(p.Counters().particleReflections==1&&p.Phase()==ForwardPhase::ReflectedPerturbation,"particle exit Fresnel reflects once");
    Sequence inward{0,0};pk.Advance(p,inward,1);Check(p.Phase()==ForwardPhase::ParticleChord,"conditional inward proposal obtains new real chord");Sequence transmission{.9};pk.Advance(p,transmission,1);
    Near(p.Counters().particlePathLengthMetres[0],.0004,1e-18,"reflected sphere travels two complete diameters");Check(p.Counters().particleExits==1,"survivor exit Fresnel counted once");
}
ForwardState Bounded(const HyLIoSForwardTransport& k,RISE::ISampler& rng,std::size_t quantum) {
    auto s=k.Begin({0,0,0},{0,0,1});constexpr std::uint64_t limit=2000;
    while(s.Termination()==ForwardTermination::None&&s.Counters().workUnits<limit) {
        auto r=k.Advance(s,rng,std::min<std::size_t>(quantum,static_cast<std::size_t>(limit-s.Counters().workUnits)));
        if(r.kind==ForwardAdvanceKind::OuterBoundaryEncounter) {auto e=s.PendingBoundary();k.ResumeBoundary(s,e.token,SampleSlabSmoothDielectric(e,rng.Get1D()));}
    }
    if(s.Termination()==ForwardTermination::None) k.StopForBudget(s);
    return s;
}
void BothPolicyQuantum() {
    for(auto policy:{ForwardPlacementPolicy::FixedEventDiagnosticV1,ForwardPlacementPolicy::NullCollisionPlacementV1})
    for(auto postScatter:{ForwardPostScatterPolicy::DiagnosticRetainParticleV1,ForwardPostScatterPolicy::DiscardAfterScatterV1}) {
        HyLIoSForwardTransport k(Particles(15,150),550,1,1000,AbsorptionEstimator::Weighted,.1,policy,postScatter);
        for(int seed=1;seed<=50;++seed) {
            Random one(static_cast<std::uint64_t>(seed)),many(static_cast<std::uint64_t>(seed));auto a=Bounded(k,one,1),b=Bounded(k,many,64);
            Check(a.Termination()==b.Termination()&&a.Weight()==b.Weight()&&a.Position().x==b.Position().x&&a.Position().z==b.Position().z&&one.draws==many.draws&&a.Counters().workUnits==b.Counters().workUnits&&a.Direction().x==b.Direction().x&&a.Direction().y==b.Direction().y&&a.Direction().z==b.Direction().z&&a.Counters().inwardPostExitEvents==b.Counters().inwardPostExitEvents&&a.HostAbsorbedByLayer()==b.HostAbsorbedByLayer()&&a.ParticleAbsorbedByLayer()==b.ParticleAbsorbedByLayer(),"all placement/post-scatter policies retain RNG and live power across proposal quantum yields and hard budgets");
        }
    }
}
void QuantumAndStatistics() {
    auto layers=Particles(40,150);
    HyLIoSForwardTransport kernel(layers,1400,1,2000,AbsorptionEstimator::Weighted,.1,ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);
    for(int seed=1;seed<=200;++seed) {Random one(static_cast<std::uint64_t>(seed)),many(static_cast<std::uint64_t>(seed));auto a=Run(kernel,one,1),b=Run(kernel,many,1000);Check(a.Termination()==b.Termination()&&a.Weight()==b.Weight()&&a.Position().x==b.Position().x&&a.Position().z==b.Position().z&&one.draws==many.draws,"quantum partition preserves entire RNG history");}
    auto dilute=Particles(40,150,1e-7);dilute[0].organelle->volumeFraction=1e-6;
    HyLIoSForwardTransport analog(dilute,1400,1,500,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1),weighted(dilute,1400,1,500,AbsorptionEstimator::Weighted,0,ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);
    HyLIoSForwardTransport control(Layers(80,1.3),1400,1,0,AbsorptionEstimator::Weighted,0,ForwardPlacementPolicy::FixedEventDiagnosticV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);HyLIoSSlabTransport slab(Slabs(80,1.3),1400,1,AbsorptionEstimator::Weighted,0);
    HyLIoSForwardTransport rr(dilute,1400,1,500,AbsorptionEstimator::Weighted,.5,ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);
    Moments ma,mw,mrr,mc,ms;int unresolved=0;
    constexpr int count=12000;
    for(int seed=1;seed<=count;++seed) {
        Random ra(static_cast<std::uint64_t>(seed)*3),rw(static_cast<std::uint64_t>(seed)*7+999),rc(static_cast<std::uint64_t>(seed)*11),rs(static_cast<std::uint64_t>(seed)*13);
        Random rrr(static_cast<std::uint64_t>(seed)*17+987);const auto wr=Run(rr,rrr);mrr.Add(Returned(wr)?wr.Weight():0);
        const auto a=Run(analog,ra),w=Run(weighted,rw),c=Run(control,rc);const auto z=RunSlab(slab,rs);
        if(!(Returned(a)||a.Termination()==ForwardTermination::Absorbed)||!(Returned(w)||w.Termination()==ForwardTermination::Absorbed))++unresolved;
        ma.Add(Returned(a)?a.Weight():0);mw.Add(Returned(w)?w.Weight():0);mc.Add(Returned(c)?c.Weight():0);ms.Add(z.Termination()==SlabTermination::SubsurfaceReturn||z.Termination()==SlabTermination::ExternalReflection?z.Weight():0);
        Near(Absorbed(w)+w.Weight(),1,2e-13,"matched weighted complete history R+A conservation");
    }
    Check(unresolved==0,"1400nm matched particles complete without unreported failures");
    Check(std::abs(ma.Mean()-mw.Mean())<=5*std::hypot(ma.SE(),mw.SE())+1e-4,"independent analog weighted estimator mean agreement");
    Check(std::abs(mc.Mean()-ms.Mean())<=5*std::hypot(mc.SE(),ms.SE())+1e-4,"zero particle fiber rates reduce independent slab statistics");
    Check(std::abs(ma.Mean()-mrr.Mean())<=5*std::hypot(ma.SE(),mrr.SE())+1e-4,"compensated weighted roulette mean agrees with analog");
    std::cout<<"statistics n="<<count<<" analog="<<ma.Mean()<<" weighted="<<mw.Mean()<<" roulette="<<mrr.Mean()<<" slab="<<ms.Mean()<<" control="<<mc.Mean()<<" unresolved="<<unresolved<<'\n';
}
}
int main(int argc,char** argv){try {if(argc==2&&std::string(argv[1])=="--discard-only"){DiscardAfterScatterControls();return 0;}if(argc==2&&std::string(argv[1])=="--absorption-overflow-only"){AbsorbedTallyOverflow();return 0;}AbsorbedTallyOverflow();std::cout<<"exact\n";ExactControls();std::cout<<"particle\n";ParticleOracles();DiscardAfterScatterControls();std::cout<<"sphere\n";AnalogSphereSurvival();SingleSphereReentryProbability();AbsorbingMismatchedSphereEstimators(1400,ForwardPostScatterPolicy::DiagnosticRetainParticleV1);AbsorbingMismatchedSphereEstimators(550,ForwardPostScatterPolicy::DiscardAfterScatterV1);std::cout<<"fiber\n";FiberAndReflection();std::cout<<"quanta\n";BothPolicyQuantum();std::cout<<"stats\n";QuantumAndStatistics();std::cout<<"PASS "<<checks<<" forward transport checks\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
