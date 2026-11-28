#include "../src/Library/Materials/HyLIoS/HyLIoSSurfaceBoundary.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace RISE::HyLIoS;
namespace {
constexpr double Pi=3.14159265358979323846264338327950288;
int checks=0;
void Check(bool b,const char* label) { ++checks; if(!b) { std::cerr<<"FAIL: "<<label<<'\n';throw std::runtime_error(label); } }
template<class F> void Reject(F f,const char* label) { bool rejected=false; try {f();}catch(const std::invalid_argument&){rejected=true;} Check(rejected,label); }
struct RNG {
    std::uint64_t state,draws=0;
    explicit RNG(std::uint64_t seed):state(seed){}
    double Draw(){++draws;state+=0x9e3779b97f4a7c15ULL;auto z=state;z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL;z=(z^(z>>27))*0x94d049bb133111ebULL;z^=z>>31;return static_cast<double>(z>>11)*0x1p-53;}
};
SlabBoundaryEncounter Encounter(SlabSide side=SlabSide::Exterior,double cosine=1) {
    return {7,side,{0,0,0},{std::sqrt((1-cosine)*(1+cosine)),0,side==SlabSide::Exterior?cosine:-cosine},550,.375,
        side==SlabSide::Exterior?1.:1.5,side==SlabSide::Exterior?1.5:1.};
}
void Transition(const HyLIoSSurfaceBoundary& law,SurfaceBoundaryState& state,RNG& rng) {
    const double u=rng.Draw();
    if(state.Phase()==SurfaceBoundaryPhase::Branch) law.TrySample(state,u);
    else {const double v=rng.Draw(),a=rng.Draw();law.TrySample(state,u,v,a);}
}
SlabBoundaryDecision Finish(const HyLIoSSurfaceBoundary& law,SurfaceBoundaryState& state,RNG& rng) {
    for(std::size_t i=0;i<100000;++i) {
        if(state.Phase()==SurfaceBoundaryPhase::Ready) return state.Decision();
        Transition(law,state,rng);
    }
    throw std::runtime_error("test boundary unresolved");
}
// Independent composite Simpson quadrature of the printed Eq8 in theta.
long double PrintedIntegral(double s,double upper=Pi/2) {
    constexpr int n=40000;long double sum=0;
    for(int i=0;i<=n;++i) {
        const long double t=static_cast<long double>(upper)*i/n,si=std::sin(t),c=std::cos(t),ss=static_cast<long double>(s)*s;
        const long double den=ss*c*c+si*si,f=ss*ss*si/(den*den);
        sum+=(i==0||i==n?1:i%2?4:2)*f;
    }
    return sum*upper/(3*n);
}
void ExactContracts() {
    Check(std::strcmp(SurfaceBoundaryVersion,"hylios2015_macro_fresnel_normal_perturb_v1")==0,"versioned forward boundary rule");
    const double edge=std::sqrt(std::numeric_limits<double>::epsilon());
    for(double bad:{-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN(),std::nextafter(edge,0.),std::nextafter(1/edge,std::numeric_limits<double>::infinity())})
        Reject([&]{HyLIoSSurfaceBoundary law(bad);},"unsupported fold rejected before encounter work");
    HyLIoSSurfaceBoundary minimum(edge),maximum(1/edge);
    for(const auto* law:{&minimum,&maximum}) for(double u:{0.,.1,.5,.9,std::nextafter(1.,0.)}) {
        const auto p=law->ProposeNormal(u);
        Check(p.cosine>=0 && p.cosine<=1 && p.sine>=0 && std::abs(p.cosine*p.cosine+p.sine*p.sine-1)<1e-14,"supported range endpoint proposal stays a unit normal");
    }
    HyLIoSSurfaceBoundary smooth(0);
    for(SlabSide side:{SlabSide::Exterior,SlabSide::Interior}) for(double u:{0.,.5}) {
        const auto e=Encounter(side);auto state=smooth.Begin(e);const auto reference=SampleSlabSmoothDielectric(e,u);
        Check(state.WorkUnits()==0 && state.Phase()==SurfaceBoundaryPhase::Branch,"Begin performs zero branch work");
        smooth.TrySample(state,u);const auto actual=state.Decision();
        Check(actual.action==reference.action && actual.targetSide==reference.targetSide && actual.direction.x==reference.direction.x && actual.direction.y==reference.direction.y && actual.direction.z==reference.direction.z && actual.weightMultiplier==1,"zero fold exact smooth delta and four branch controls");
        Check(state.WorkUnits()==1 && state.NormalProposals()==0,"smooth branch has exactly one transition");
        Reject([&]{smooth.TrySample(state,.5);},"completed state cannot be sampled twice");
    }
    auto e=Encounter();e.incidentIOR=e.targetIOR;auto matched=smooth.Begin(e);smooth.TrySample(matched,0);
    Check(matched.Decision().action==BoundaryAction::Transmit && matched.MacroReflectance()==0,"matched indices transmit exactly");
    e=Encounter(SlabSide::Interior,.6);auto tir=smooth.Begin(e);smooth.TrySample(tir,std::nextafter(1.,0.));
    Check(tir.MacroReflectance()==1 && tir.Decision().action==BoundaryAction::Reflect,"interior TIR never transmits");
    e.incidentIOR=1.25;auto critical=smooth.Begin(e);smooth.TrySample(critical,.99);
    Check(critical.MacroReflectance()==1 && critical.Decision().action==BoundaryAction::Reflect,"exact critical angle limiting reflection");
    HyLIoSSurfaceBoundary law(1),other(1);auto state=law.Begin(Encounter());
    for(double bad:{-0.1,1.,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
        Reject([&]{law.TrySample(state,bad);},"invalid branch variate rejected");
        Check(state.WorkUnits()==0 && state.Phase()==SurfaceBoundaryPhase::Branch,"invalid branch leaves state unchanged");
    }
    Reject([&]{other.TrySample(state,0);},"cross-law event ownership rejected");
    law.TrySample(state,0);Check(state.Phase()==SurfaceBoundaryPhase::NormalProposal,"reflection branch pins before direction proposal");
    for(int index=0;index<3;++index) {
        double vars[3]={.9,.3,0};vars[index]=1;
        Reject([&]{law.TrySample(state,vars[0],vars[1],vars[2]);},"invalid proposal variate rejected");
        Check(state.WorkUnits()==1 && state.NormalProposals()==0,"invalid proposal consumes no work");
    }
    law.TrySample(state,.1,.3,0);
    Check(state.Phase()==SurfaceBoundaryPhase::NormalProposal && state.WorkUnits()==2 && state.NormalProposals()==1,"wrong hemisphere rejection yields after exactly one proposal");
    law.TrySample(state,.9,.3,0);
    Check(state.Decision().action==BoundaryAction::Reflect && state.Decision().direction.z<0,"rejected normal never rerolls macro Fresnel branch");
    auto cancelled=law.Begin(Encounter());law.TrySample(cancelled,0);law.TrySample(cancelled,.1,0,0);law.Cancel(cancelled);
    Check(cancelled.Phase()==SurfaceBoundaryPhase::Cancelled && cancelled.LiveWeight()==.375 && cancelled.NormalProposals()==1,"cancelled rejected reflection retains all unresolved power");
    auto capped=law.Begin(Encounter());law.StopForBudget(capped);
    Check(capped.Phase()==SurfaceBoundaryPhase::BudgetExceeded && capped.LiveWeight()==.375 && capped.WorkUnits()==0,"budget before branch retains live power with no sample");
    Reject([&]{law.TrySample(capped,0);},"stopped event cannot consume RNG work");
    e=Encounter();e.weight=std::numeric_limits<double>::quiet_NaN();Reject([&]{law.Begin(e);},"invalid encounter weight fails before branch");
}
void ProposalQuadrature() {
    for(double s:{.1,.25,.3,.45,1.,2.}) {
        HyLIoSSurfaceBoundary law(s);const double z=static_cast<double>(PrintedIntegral(s));
        Check(std::abs(law.Normalization()/z-1)<2e-10,"Eq8 normalization matches independent printed-form theta quadrature at source folds");
        long double integral=0;constexpr int n=40000;
        for(int i=0;i<=n;++i) integral+=(i==0||i==n?1:i%2?4:2)*law.PolarAnglePDF(Pi/2*i/n);
        Check(std::abs(static_cast<double>(integral)*(Pi/2)/(3*n)-1)<2e-10,"public polar density integrates to one per radian");
        for(double u:{.001,.1,.5,.9,.999}) {
            const auto p=law.ProposeNormal(u);long double proposalCDF=0;
            if(s<1) {
                const long double k=std::sqrt(1-static_cast<long double>(s)*s),delta=1-k;
                proposalCDF=delta*p.cosine/(1-k*p.cosine);
            } else if(s==1) proposalCDF=p.cosine;
            else {const long double a=std::sqrt(static_cast<long double>(s)*s-1);proposalCDF=std::atan(a*p.cosine)/std::atan(a);}
            Check(std::abs(static_cast<double>(proposalCDF)-u)<4e-13,"proposal inverse matches independent CDF");
            if(s<1) Check(p.acceptanceProbability>=.25,"small-fold pointwise proposal acceptance bounded by one quarter");
            if(s==1) Check(p.acceptanceProbability==1,"isotropic normal proposal accepted exactly");
        }
        RNG rng(113);constexpr int trials=180000;int accepted=0,lower=0;long double sum=0;
        for(int i=0;i<trials;++i) {const auto p=law.ProposeNormal(rng.Draw());if(rng.Draw()<p.acceptanceProbability){++accepted;sum+=p.cosine;if(p.cosine<std::cos(.3))++lower;}}
        // Independent integration of printed Eq8 over theta>.3 for cosine CDF.
        const double expected=1-static_cast<double>(PrintedIntegral(s,.3))/z;
        const double measured=static_cast<double>(lower)/accepted;
        const double se=std::sqrt(expected*(1-expected)/accepted);
        Check(std::abs(measured-expected)<6*se+2e-6,"normalized accepted normal CDF agrees with independent Eq8 quadrature");
        const double acceptance=static_cast<double>(accepted)/trials;
        Check(acceptance>(s<1?.24:s==1?.999:.49),"proposal acceptance empirical lower bound, independent of boundary hemisphere");
        Check(sum>0,"accepted normal moment populated");
    }
}
struct ConditionalReference { double mean,cdf; };
ConditionalReference ReflectedReference(double s,double ci,double threshold) {
    // Independent integration directly in theta,phi of the printed normal
    // density and explicit reflection geometry. Neither proposal inverse nor
    // production PDF/normalization is used in this oracle.
    constexpr int nt=1400,np=1600;long double mass=0,moment=0,lower=0;
    const long double si=std::sqrt((1-static_cast<long double>(ci))*(1+ci));
    for(int t=0;t<nt;++t) {
        const long double theta=(t+.5L)*(Pi/2)/nt,c=std::cos(theta),sine=std::sin(theta),ss=static_cast<long double>(s)*s;
        const long double den=ss*c*c+sine*sine,weight=ss*ss*sine/(den*den);
        for(int j=0;j<np;++j) {
            const long double phi=2*Pi*(j+.5L)/np;
            const long double facing=ci*c-si*sine*std::cos(phi),co=2*facing*c-ci;
            if(facing>0 && co>0) {mass+=weight;moment+=weight*co;if(co<threshold)lower+=weight;}
        }
    }
    return {static_cast<double>(moment/mass),static_cast<double>(lower/mass)};
}
void ReflectedConditionalAndBranchMass() {
    for(double s:{.1,.25,.45,1.,2.}) {
        HyLIoSSurfaceBoundary law(s);RNG rng(912);constexpr int n=50000;int reflected=0;
        for(int i=0;i<n;++i) {auto state=law.Begin(Encounter());const auto d=Finish(law,state,rng);if(d.action==BoundaryAction::Reflect)++reflected;Check(d.weightMultiplier==1,"analog reflected/transmitted physical power has no extra Fresnel or eta factor");}
        Check(std::abs(static_cast<double>(reflected)/n-.04)<6*std::sqrt(.04*.96/n),"macro Fresnel mass stays .04 across all surface folds");
    }
    for(double s:{.25,1.,2.}) for(double ci:{1.,.6,.001}) {
        HyLIoSSurfaceBoundary law(s);const auto reference=ReflectedReference(s,ci,.5);RNG rng(1519);constexpr int n=40000;double sum=0,sum2=0;int lower=0;
        for(int i=0;i<n;++i) {
            auto state=law.Begin(Encounter(SlabSide::Exterior,ci));law.TrySample(state,0);const auto d=Finish(law,state,rng);
            const double co=-d.direction.z;sum+=co;sum2+=co*co;if(co<.5)++lower;
            Check(d.action==BoundaryAction::Reflect && co>0,"conditioned reflection stays in selected incident hemisphere");
        }
        const double mean=sum/n,se=std::sqrt((sum2/n-mean*mean)/n),cdf=static_cast<double>(lower)/n;
        Check(std::abs(mean-reference.mean)<6*se+8e-4,"normal/oblique/grazing reflected mean matches independent two-dimensional quadrature");
        Check(std::abs(cdf-reference.cdf)<6*std::sqrt(reference.cdf*(1-reference.cdf)/n)+8e-4,"normal/oblique/grazing reflected CDF matches independent two-dimensional quadrature");
    }
    for(double s:{.1,.25,.45,1.,2.}) {
        HyLIoSSurfaceBoundary law(s);RNG extRng(331),intRng(331);
        for(int i=0;i<1000;++i) {
            auto ext=law.Begin(Encounter(SlabSide::Exterior,.6)),interior=law.Begin(Encounter(SlabSide::Interior,.6));
            law.TrySample(ext,0);law.TrySample(interior,0);
            const auto a=Finish(law,ext,extRng),b=Finish(law,interior,intRng);
            Check(a.action==BoundaryAction::Reflect && b.action==BoundaryAction::Reflect && a.direction.x==b.direction.x && a.direction.y==b.direction.y && a.direction.z==-b.direction.z && extRng.draws==intRng.draws,"entry and TIR exit reflected laws mirror exactly under identical proposals");
        }
    }
    // Exact s=1, normal-incidence oracle: accepted mu is uniform on
    // [sqrt(1/2),1], co=2mu^2-1, so P(co<.5) has this closed form.
    const auto ref=ReflectedReference(1,1,.5);
    const double exact=(std::sqrt(.75)-std::sqrt(.5))/(1-std::sqrt(.5));
    Check(std::abs(ref.cdf-exact)<8e-4,"independent angular quadrature agrees with s1 analytic reflected CDF");
}
void ResumeControls() {
    HyLIoSSurfaceBoundary law(.25);RNG one(991),split(991);
    for(int history=0;history<5000;++history) {
        auto a=law.Begin(Encounter(SlabSide::Interior,.1)),b=law.Begin(Encounter(SlabSide::Interior,.1));
        const auto expected=Finish(law,a,one);
        // Explicit caller quanta alternate zero work, one transition and three
        // transitions; copying unresolved event state exercises retained phase.
        for(int round=0;b.Phase()!=SurfaceBoundaryPhase::Ready;++round) {
            const int quantum=round%3==0?0:round%3==1?1:3;
            for(int i=0;i<quantum && b.Phase()!=SurfaceBoundaryPhase::Ready;++i) Transition(law,b,split);
            auto saved=b;b=saved;
            Check(round<100000,"resumed rejection event makes progress");
        }
        const auto actual=b.Decision();
        Check(actual.action==expected.action && actual.direction.x==expected.direction.x && actual.direction.y==expected.direction.y && actual.direction.z==expected.direction.z && one.draws==split.draws && a.WorkUnits()==b.WorkUnits() && a.NormalProposals()==b.NormalProposals(),"same-seed resume partitions preserve exact direction, phase work and RNG sequence");
    }
}
}
int main(){try{ExactContracts();ProposalQuadrature();ReflectedConditionalAndBranchMass();ResumeControls();std::cout<<"PASS: HyLIoSSurfaceBoundaryTest "<<checks<<" checks\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
