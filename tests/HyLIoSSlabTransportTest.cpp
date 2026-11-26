#include "../src/Library/Materials/HyLIoS/HyLIoSSlabTransport.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace RISE::HyLIoS;
namespace {
int checks=0;
void Check(bool b,const char* text) { ++checks; if (!b) throw std::runtime_error(text); }
template<class F> void Reject(F f,const char* message) { bool rejected=false; try { f(); } catch(const std::invalid_argument&) { rejected=true; } Check(rejected,message); }
struct Random : RISE::ISampler {
    std::uint64_t bits,draws=0;
    explicit Random(std::uint64_t seed): bits(seed) {}
    double Get1D() override { ++draws; bits+=0x9e3779b97f4a7c15ULL; auto z=bits; z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL; z=(z^(z>>27))*0x94d049bb133111ebULL; z^=z>>31; return static_cast<double>(z>>11)*0x1p-53; }
    RISE::Point2 Get2D() override { return {Get1D(),Get1D()}; }
};
struct Sequence : RISE::ISampler {
    std::vector<double> values; std::size_t index=0;
    explicit Sequence(std::vector<double> v):values(std::move(v)) {}
    double Get1D() override { if (index==values.size()) throw std::runtime_error("script sampler exhausted"); return values[index++]; }
    RISE::Point2 Get2D() override { return {Get1D(),Get1D()}; }
};
struct Fixed : Random { using Random::Random; bool HasFixedDimensionBudget() const override { return true; } };
std::array<SlabLayer,6> Layers(double mu=0, double n=1) {
    std::array<SlabLayer,6> l{}; for(auto& v:l) v={0.001,mu,n}; return l;
}
SlabState Run(const HyLIoSSlabTransport& k, RISE::ISampler& rng, std::size_t quantum=10000, SlabVector dir={0,0,1}) {
    auto s=k.Begin({0,0,0},dir);
    for(std::size_t calls=0;calls<100000;++calls) {
        const auto result=k.Advance(s,rng,quantum);
        if(result.kind==SlabAdvanceKind::Terminal) return s;
        if(result.kind==SlabAdvanceKind::OuterBoundaryEncounter) {
            const auto e=s.PendingBoundary();
            k.ResumeBoundary(s,e.token,SampleSlabSmoothDielectric(e,rng.Get1D()));
        }
    }
    throw std::runtime_error("test unresolved history");
}
double Absorbed(const SlabState& s) { double a=0;for(double v:s.AbsorbedByLayer()) a+=v;return a; }
struct Moments { double sum=0,sum2=0;std::size_t n=0;void Add(double x){sum+=x;sum2+=x*x;++n;} double Mean()const{return sum/static_cast<double>(n);} double SE()const {return std::sqrt(std::max(0.,(sum2-sum*sum/static_cast<double>(n))/static_cast<double>(n-1)/static_cast<double>(n)));} };
double ReflectanceOracle(double tau) {
    // Independent composite midpoint quadrature of Lambertian bottom integral.
    constexpr int n=200000; long double sum=0;
    for(int i=0;i<n;++i) { const long double c=(static_cast<long double>(i)+0.5L)/n;sum+=2*c*std::exp(-static_cast<long double>(tau)/c); }
    return std::exp(-tau)*static_cast<double>(sum/n);
}
void ExactControls() {
    Check(std::strcmp(SlabControlVersion,"slab-control-v1")==0,"explicit slab-only API version");
    HyLIoSSlabTransport k(Layers(),550,1,AbsorptionEstimator::Analog,0);
    Random random(90);
    for(int i=0;i<100;++i) {
        auto s=Run(k,random);
        Check(s.Termination()==SlabTermination::SubsurfaceReturn && s.Weight()==1 && Absorbed(s)==0,"matched-index zeroabs control returns unit weight");
        Check(s.Position().z==0 && s.Direction().z<0 && s.Counters().bottomReflections==1,"unit diffuse bottom and exact exit ownership");
        Check(s.Position().x!=0 || s.Position().y!=0,"retain actual lateral displacement");
    }
    // Independent per-path geometric identity: path = down thickness + thickness/c.
    Random rng(702); const auto s=Run(k,rng);
    const double expected=0.006*(1+1/std::abs(s.Direction().z));
    Check(std::abs(s.Counters().pathLengthMetres-expected)<2e-16,"path length uses six actual segments");
    for(double v:s.Counters().layerPathLengthMetres) Check(std::abs(v-expected/6)<2e-16,"per-layer path length identity");
    auto initial=k.Begin({4,-3,0},{0,0,1}); Fixed fixed(1);
    HyLIoSSlabTransport other(Layers(),550,1,AbsorptionEstimator::Analog,0);
    Reject([&]{other.Advance(initial,random,1);},"cross-kernel state rejected before use");
    Reject([&]{k.Advance(initial,fixed,1);},"fixed dimension samplers rejected before consuming");
    Check(fixed.draws==0,"fixed budget rejection consumes no random samples");
    Random noDraw(3); k.Advance(initial,noDraw,0);k.Advance(initial,noDraw,100);
    Check(noDraw.draws==0 && initial.PendingBoundary().token==1,"pending boundary re-Advance never draws");
    const auto first=initial.PendingBoundary();
    auto decision=SampleSlabSmoothDielectric(first,0);
    Reject([&]{k.ResumeBoundary(initial,first.token+1,decision);},"wrong token rejected");
    decision.targetSide=SlabSide::Exterior;
    Reject([&]{k.ResumeBoundary(initial,first.token,decision);},"wrong side rejected");
    decision=SampleSlabSmoothDielectric(first,0);decision.direction={0,0,-1};
    Reject([&]{k.ResumeBoundary(initial,first.token,decision);},"wrong hemisphere rejected");
    decision=SampleSlabSmoothDielectric(first,0);decision.weightMultiplier=std::numeric_limits<double>::infinity();
    Reject([&]{k.ResumeBoundary(initial,first.token,decision);},"nonfinite proposal weight rejected");
    Check(initial.HasPendingBoundary() && initial.PendingBoundary().token==first.token,"rejected proposals preserve pending token");
    decision=SampleSlabSmoothDielectric(first,0);k.ResumeBoundary(initial,first.token,decision);
    Reject([&]{k.ResumeBoundary(initial,first.token,decision);},"reused token rejected");
    Check(initial.Layer()==0 && initial.Position().z==0,"entry has exact layer ownership, no epsilon");
}
void BoundaryControls() {
    SlabBoundaryEncounter e{1,SlabSide::Exterior,{0,0,0},{0,0,1},550,1,1,1.5};
    auto a=EvaluateSlabSmoothDielectric(e);
    Check(std::abs(a.reflectance-0.04)<1e-16,"normal-incidence analytic Fresnel");
    Check(SampleSlabSmoothDielectric(e,0).action==BoundaryAction::Reflect && SampleSlabSmoothDielectric(e,0.5).action==BoundaryAction::Transmit,"physical analog branch probability");
    e.incidentSide=SlabSide::Interior;e.incidentDirection={0.8,0,-0.6};e.incidentIOR=1.5;e.targetIOR=1;
    a=EvaluateSlabSmoothDielectric(e);
    Check(a.totalInternalReflection && a.reflectance==1,"TIR exact certificate");
    e.incidentIOR=1.25;e.targetIOR=1;e.incidentDirection={0.8,0,-0.6};
    a=EvaluateSlabSmoothDielectric(e);
    Check(a.reflectance==1 && !a.totalInternalReflection,"exact critical-angle limiting reflection");
    const double below=std::nextafter(0.8,0.0),above=std::nextafter(0.8,1.0);
    e.incidentDirection={below,0,-std::sqrt((1-below)*(1+below))};
    Check(EvaluateSlabSmoothDielectric(e).reflectance<1,"one-ULP subcritical transmission preserved");
    e.incidentDirection={above,0,-std::sqrt((1-above)*(1+above))};
    Check(EvaluateSlabSmoothDielectric(e).totalInternalReflection,"one-ULP supercritical TIR preserved");
    e.incidentIOR=1.5;
    e.incidentDirection={0.4,0,-std::sqrt(0.84)};
    a=EvaluateSlabSmoothDielectric(e);
    Check(!a.totalInternalReflection && std::abs(a.transmittedDirection.x-0.6)<2e-16,"Snell tangential momentum identity");
    e.incidentIOR=e.targetIOR;e.incidentDirection={1,0,0};
    a=EvaluateSlabSmoothDielectric(e);
    Check(a.reflectance==0 && a.transmittedDirection.x==1 && a.transmittedDirection.z==0,"matched index exact tangent");
    e.incidentIOR=1.5;e.targetIOR=1;
    Check(EvaluateSlabSmoothDielectric(e).reflectance==1,"unmatched grazing reflection");
    e.incidentDirection={0,0,1};
    Reject([&]{EvaluateSlabSmoothDielectric(e);},"incident side mismatch rejected");
    e.incidentDirection={0,0,0};
    Reject([&]{EvaluateSlabSmoothDielectric(e);},"zero helper direction rejected");
    e.incidentDirection={0,0,-1};e.incidentIOR=0;
    Reject([&]{EvaluateSlabSmoothDielectric(e);},"nonpositive helper IOR rejected");
    Reject([&]{SampleSlabSmoothDielectric(e,1);},"uniform upper endpoint rejected");
    e.incidentSide=SlabSide::Exterior;e.incidentDirection={0,0,1};
    e.incidentIOR=std::numeric_limits<double>::max();e.targetIOR=e.incidentIOR/2;
    a=EvaluateSlabSmoothDielectric(e);
    Check(std::abs(a.reflectance-1./9)<1e-16,"Fresnel avoids overflow at finite large IOR");
    e.incidentIOR=1e300;e.targetIOR=1e-300;
    a=EvaluateSlabSmoothDielectric(e);
    Check(a.reflectance==1 && a.transmittedDirection.z==1,"extreme normal eta avoids infinity times zero");
    e.incidentDirection={1e-300,0,1};e.incidentIOR=1e300;e.targetIOR=1;
    Check(EvaluateSlabSmoothDielectric(e).reflectance==1,"critical tangent momentum avoids eta overflow");
    // Repeated internal outer visits: force R,R,T; each bottom remains one diffuse event.
    HyLIoSSlabTransport k(Layers(),550,1,AbsorptionEstimator::Analog,0);Random rng(998);
    auto s=k.Begin({0,0,0},{0,0,1});auto start=s.PendingBoundary();k.ResumeBoundary(s,start.token,SampleSlabSmoothDielectric(start,0));
    std::uint64_t prev=start.token;
    for(int visit=0;visit<3;++visit) {
        while(k.Advance(s,rng,1).kind!=SlabAdvanceKind::OuterBoundaryEncounter) {}
        auto event=s.PendingBoundary();Check(event.token>prev && event.incidentSide==SlabSide::Interior,"monotonic token per repeated visit");prev=event.token;
        if(visit<2) k.ResumeBoundary(s,event.token,{BoundaryAction::Reflect,SlabSide::Interior,{s.Direction().x,s.Direction().y,-s.Direction().z},1});
        else k.ResumeBoundary(s,event.token,SampleSlabSmoothDielectric(event,0));
    }
    Check(s.Termination()==SlabTermination::SubsurfaceReturn && s.Counters().bottomReflections==3,"R,R,T history preserves internal state");
    // Pure exterior Fresnel reflection is separately classified.
    HyLIoSSlabTransport coat(Layers(0,1.5),550,1,AbsorptionEstimator::Analog,0);
    auto external=coat.Begin({0,0,0},{0,0,1});auto ev=external.PendingBoundary();coat.ResumeBoundary(external,ev.token,SampleSlabSmoothDielectric(ev,0));
    Check(external.Termination()==SlabTermination::ExternalReflection && external.Counters().flights==0,"initial reflection is not subsurface return");
}
void Statistics() {
    constexpr std::size_t n=50000;
    for(double tau:{0.12,0.6,1.8}) {
        const double mu=tau/0.006, expected=ReflectanceOracle(tau);
        HyLIoSSlabTransport analog(Layers(mu),550,1,AbsorptionEstimator::Analog,0);
        HyLIoSSlabTransport weighted(Layers(mu),550,1,AbsorptionEstimator::Weighted,0);
        Random ra(7021),rw(8032);Moments ma,mw;
        for(std::size_t i=0;i<n;++i) {
            auto a=Run(analog,ra),w=Run(weighted,rw);
            Check(Absorbed(a)+a.Weight()==1,"analog path exact absorption/escape conservation");
            Check(std::abs(Absorbed(w)+w.Weight()-1)<2e-15,"weighted path physical absorption/escape conservation");
            ma.Add(a.Weight());mw.Add(w.Weight());
        }
        Check(std::abs(ma.Mean()-expected)<=5*ma.SE()+1e-4,"analog reflectance agrees with independent attenuation integral");
        Check(std::abs(mw.Mean()-expected)<=5*mw.SE()+1e-4,"weighted reflectance agrees with independent attenuation integral");
        Check(std::abs(ma.Mean()-mw.Mean())<=5*std::hypot(ma.SE(),mw.SE())+1e-4,"analog/weighted mean agreement 5SE+1e-4");
        std::cout<<"tau="<<tau<<" analytic="<<expected<<" analog="<<ma.Mean()<<" weighted="<<mw.Mean()<<'\n';
    }
    // Independent seeds are replicate means, including both physical channels.
    HyLIoSSlabTransport rr(Layers(100),550,1,AbsorptionEstimator::Weighted,0.8);
    Moments replicateR,replicateA;std::uint64_t killed=0;
    for(std::uint64_t replicate=0;replicate<24;++replicate) {
        Random rng(9261+replicate*70001);Moments r,a;
        for(int i=0;i<4000;++i) {auto s=Run(rr,rng);r.Add(s.Weight());a.Add(Absorbed(s));killed+=s.Counters().rouletteKills;}
        replicateR.Add(r.Mean());replicateA.Add(a.Mean());
    }
    const double expected=ReflectanceOracle(0.6);
    Check(killed>0,"roulette exercised");
    Check(std::abs(replicateR.Mean()-expected)<=5*replicateR.SE()+1e-4,"compensated roulette replicate reflectance mean");
    Check(std::abs(replicateA.Mean()-(1-expected))<=5*replicateA.SE()+1e-4,"roulette kills excluded from physical absorption estimator");
    // Force a roulette kill after three finite flights: tally must remain only Beer loss.
    auto s=rr.Begin({0,0,0},{0,0,1});auto ev=s.PendingBoundary();rr.ResumeBoundary(s,ev.token,SampleSlabSmoothDielectric(ev,0));
    Sequence script({0,0,0,0.999});
    rr.Advance(s,script,4);
    Check(s.Termination()==SlabTermination::RouletteKilled,"script reaches roulette kill");
    Check(std::abs(Absorbed(s)-(1-std::exp(-0.3)))<1e-15,"roulette kill does not become physical absorption");
}
void PauseAndInvalid() {
    auto layers=Layers(120,1.4);layers[2].realIOR=1.1;layers[3].realIOR=1.6;
    HyLIoSSlabTransport k(layers,550,1,AbsorptionEstimator::Weighted,0.6);
    for(std::uint64_t seed=0;seed<200;++seed) {
        Random a(seed),b(seed);auto x=Run(k,a,1),y=Run(k,b,100000);
        Check(a.bits==b.bits && a.draws==b.draws,"quantum yields preserve exact sample stream");
        Check(x.Termination()==y.Termination() && x.Weight()==y.Weight() && x.Layer()==y.Layer(),"quantum yields preserve terminal outcome");
        Check(x.Position().x==y.Position().x && x.Position().y==y.Position().y && x.Position().z==y.Position().z && x.Direction().x==y.Direction().x && x.Direction().y==y.Direction().y && x.Direction().z==y.Direction().z,"quantum yields preserve complete exit geometry");
        Check(x.AbsorbedByLayer()==y.AbsorbedByLayer() && x.Counters().pathLengthMetres==y.Counters().pathLengthMetres,"quantum yields preserve tallies and length");
    }
    auto bad=Layers();bad[2].thicknessMetres=0;
    Reject([&]{HyLIoSSlabTransport z(bad,550,1,AbsorptionEstimator::Analog,0);},"zero thickness rejected");
    bad=Layers();bad[1].absorptionPerMetre=-1;
    Reject([&]{HyLIoSSlabTransport z(bad,550,1,AbsorptionEstimator::Analog,0);},"negative absorption rejected");
    bad=Layers();bad[1].realIOR=std::numeric_limits<double>::quiet_NaN();
    Reject([&]{HyLIoSSlabTransport z(bad,550,1,AbsorptionEstimator::Analog,0);},"nonfinite index rejected");
    Reject([&]{HyLIoSSlabTransport z(Layers(),0,1,AbsorptionEstimator::Analog,0);},"invalid wavelength rejected");
    Reject([&]{HyLIoSSlabTransport z(Layers(),550,1,AbsorptionEstimator::Analog,1);},"analog roulette configuration rejected");
    HyLIoSSlabTransport clear(Layers(),550,1,AbsorptionEstimator::Analog,0);
    Random grazingRng(718);
    auto grazing=Run(clear,grazingRng,1,{1,0,1e-12});
    Check(grazing.Termination()==SlabTermination::SubsurfaceReturn && grazing.Position().x>1e9,"finite grazing flight retains large lateral displacement");
    auto tangent=clear.Begin({0,0,0},{1,0,0});auto e=tangent.PendingBoundary();clear.ResumeBoundary(tangent,e.token,SampleSlabSmoothDielectric(e,0));Random r(8);
    Check(clear.Advance(tangent,r,100).kind==SlabAdvanceKind::YieldBudget && r.draws==0 && Absorbed(tangent)==0,"transparent exact tangent yields without absorption or division");
    HyLIoSSlabTransport absorbing(Layers(100),550,1,AbsorptionEstimator::Analog,0);
    auto at=absorbing.Begin({0,0,0},{1,0,0});e=at.PendingBoundary();absorbing.ResumeBoundary(at,e.token,SampleSlabSmoothDielectric(e,0));
    Sequence us({0.5});absorbing.Advance(at,us,1);
    Check(at.Termination()==SlabTermination::Absorbed && at.Position().z==0 && std::abs(at.Position().x-std::log(2.)/100)<1e-17,"absorbing tangent samples finite physical absorption");
    // Every variate used by a candidate must satisfy [0,1), including NaN/Inf.
    for(double u:{-0.1,1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        auto s=absorbing.Begin({0,0,0},{0,0,1});e=s.PendingBoundary();absorbing.ResumeBoundary(s,e.token,SampleSlabSmoothDielectric(e,0));Sequence invalid({u});
        absorbing.Advance(s,invalid,100);
        Check(s.Termination()==SlabTermination::InvalidSample && Absorbed(s)==0,"invalid absorption sample is failure, not physical kill");
    }
    HyLIoSSlabTransport extreme(Layers(std::numeric_limits<double>::max()),550,1,AbsorptionEstimator::Analog,0);
    auto underflow=extreme.Begin({0,0,0},{0,0,1});e=underflow.PendingBoundary();extreme.ResumeBoundary(underflow,e.token,SampleSlabSmoothDielectric(e,0));
    Sequence tiny({std::numeric_limits<double>::denorm_min()});extreme.Advance(underflow,tiny,1);
    Check(underflow.Termination()==SlabTermination::NumericalFailure && Absorbed(underflow)==0,"unrepresentable positive free flight is a numerical failure");
    HyLIoSSlabTransport zero(Layers(),550,1,AbsorptionEstimator::Weighted,0);
    auto z=zero.Begin({0,0,0},{0,0,1});e=z.PendingBoundary();auto proposal=SampleSlabSmoothDielectric(e,0);proposal.weightMultiplier=0;zero.ResumeBoundary(z,e.token,proposal);
    Check(z.Termination()==SlabTermination::ZeroWeight && Absorbed(z)==0,"zero proposal contribution is not absorption");
    auto small=zero.Begin({0,0,0},{0,0,1});e=small.PendingBoundary();proposal=SampleSlabSmoothDielectric(e,0);proposal.weightMultiplier=1e-300;zero.ResumeBoundary(small,e.token,proposal);
    while(zero.Advance(small,r,100).kind!=SlabAdvanceKind::OuterBoundaryEncounter) {}
    e=small.PendingBoundary();proposal=SampleSlabSmoothDielectric(e,0);proposal.weightMultiplier=1e-300;zero.ResumeBoundary(small,e.token,proposal);
    Check(small.Termination()==SlabTermination::NumericalFailure && Absorbed(small)==0,"positive boundary weight underflow is explicit failure");
    // Zero rate does not draw an absorption variate; first draw belongs to interface.
    auto s=clear.Begin({0,0,0},{0,0,1});e=s.PendingBoundary();clear.ResumeBoundary(s,e.token,SampleSlabSmoothDielectric(e,0));Sequence invalid({1});clear.Advance(s,invalid,1);
    Check(s.Termination()==SlabTermination::InvalidSample,"internal dielectric samples validated even for matched indices");
    Reject([&]{clear.Begin({0,0,0.1},{0,0,1});},"off-plane launch rejected");
    Check(clear.Begin({0,0,0},{0,0,2}).Direction().z==1,"raw launch direction is normalized scale-safely");
    const double subnormal=std::numeric_limits<double>::denorm_min();
    Check(clear.Begin({0,0,0},{0,0,subnormal}).Direction().z==1,"subnormal direction normalization remains finite under fast-math");
    for (double scale : {subnormal, std::numeric_limits<double>::max()}) {
        const auto diagonal=clear.Begin({0,0,0},{scale,0,scale}).Direction();
        Check(std::isfinite(diagonal.x) && std::isfinite(diagonal.z) &&
              std::abs(diagonal.x-std::sqrt(0.5))<1e-15 && diagonal.x==diagonal.z,
              "extreme direction scale preserves diagonal orientation");
    }
    SlabBoundaryEncounter smallIndex{1,SlabSide::Exterior,{0,0,0},{0,0,1},550,1,subnormal,2*subnormal};
    const auto smallFresnel=EvaluateSlabSmoothDielectric(smallIndex);
    Check(std::isfinite(smallFresnel.reflectance) && std::abs(smallFresnel.reflectance-1.0/9.0)<1e-15,
          "subnormal IOR common scale preserves Fresnel ratio under fast-math");
}
}
int main() {
    try { ExactControls();BoundaryControls();Statistics();PauseAndInvalid();std::cout<<"PASS "<<checks<<" slab-control-v1 checks\n";return 0; }
    catch(const std::exception& e) {std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}
}
