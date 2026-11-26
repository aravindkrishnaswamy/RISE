#include "../src/Library/Materials/HyLIoS/HyLIoSComplexLayout.h"
#include <cmath>
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <stdexcept>
using namespace RISE::HyLIoS;
namespace {
unsigned checks=0;
void Check(bool condition,const char* message) {
    ++checks;if(!condition) {std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}
}
void Near(double actual,double expected,const char* message) {
    Check(std::abs(actual-expected)<=1e-13*std::max(1.,std::abs(expected)),message);
}
std::array<double,5> Draw(std::mt19937_64& rng) {
    std::array<double,5> u;for(double& x:u) x=static_cast<double>(rng()>>11)*0x1p-53;return u;
}
void ExactControls() {
    const int savedRounding=std::fegetround();
    if(std::fesetround(FE_DOWNWARD)==0) {
        bool rejected=false;
        try {ComplexLayout unsupported(1,.1,.2,1);}catch(const std::range_error&) {rejected=true;}
        const bool restored=std::fesetround(savedRounding)==0;
        Check(restored,"layout control restores caller rounding mode");
        Check(rejected,"layout rejects unsupported floating-point environment before construction");
    }
    bool tooMany=false;try {ComplexLayout invalid(100,.01,.01,ComplexLayout::MaximumChildren+1);}catch(const std::invalid_argument&) {tooMany=true;}
    Check(tooMany,"layout resource bound rejects count before allocation without physical truncation");
    ComplexLayout empty(1,.25,.25,0);
    Check(empty.Complete()&&empty.Children().empty(),"zero children is an empty envelope");
    Check(empty.Propose({-1,-1,-1,-1,-1}).kind==ComplexProposalKind::Complete,"complete layout consumes no sample");
    Check(empty.Counters().proposals==0,"empty envelope consumes no construction work");
    ComplexLayout one(1,1,1,1);
    Check(one.Propose({0,0,0,.5,.5}).kind==ComplexProposalKind::Complete,"one full sphere at zero centre");
    const auto& only=one.Children()[0];
    Check(only.centreFromEnvelopeMetres.x==0&&only.centreFromEnvelopeMetres.y==0&&only.centreFromEnvelopeMetres.z==0,"one centre degeneracy exact");
    const auto oneChord=IntersectProlateRay(only.particle,{-1,0,0},{1,0,0});
    Check(bool(oneChord),"one-child ray intersects");Near(oneChord->chordLengthMetres,2,"one child full diameter oracle");
    bool impossible=false;try {ComplexLayout invalid(2,1,1,9);}catch(const std::invalid_argument&) {impossible=true;}
    Check(impossible,"necessary volume bound rejects nine unit spheres in radius-two envelope");
    ComplexLayout exactVolume(2,1,1,8);Check(!exactVolume.Complete(),"exact equality in necessary volume bound is permitted");
    ComplexLayout layout(1.25,.25,.25,2);
    Check(layout.Propose({.25,.5,.5,.5,0}).kind==ComplexProposalKind::AcceptedChild,"first known sphere");
    Check(layout.Propose({.75,.5,.5,.5,0}).kind==ComplexProposalKind::Complete,"second known sphere");
    double childDistance=0;
    for(const auto& c:layout.Children()) {
        const auto hit=IntersectProlateRay(c.particle,{-1.25-c.centreFromEnvelopeMetres.x,0,0},{1,0,0});
        Check(bool(hit),"known two-child intersection");childDistance+=hit->chordLengthMetres;
    }
    Near(childDistance,1,"two children have exactly one metre total chord");
    const double hostDistance=2.5-childDistance;
    Near(std::exp(-.7*hostDistance-1.3*childDistance),std::exp(-2.35),"matched-index Beer oracle separates gap and child optical depths");
    ComplexLayout rotation(1.25,.25,.25,2);
    rotation.Propose({.5,.25,.5,.5,0});rotation.Propose({.5,.75,.5,.5,0});
    double rotatedDistance=0;
    for(const auto& c:rotation.Children()) {
        const auto hit=IntersectProlateRay(c.particle,{0,-1.25-c.centreFromEnvelopeMetres.y,0},{0,1,0});
        Check(bool(hit),"rotated known two-child intersection");rotatedDistance+=hit->chordLengthMetres;
    }
    Check(rotatedDistance==childDistance,"axis-swapped fixed-layout chord equivalence");
    ComplexLayout restart(1,.25,.25,2);
    restart.Propose({.5,.5,.5,.5,0});
    for(unsigned i=1;i<=ComplexLayout::ProposalsPerChildBeforeRestart;++i) {
        const auto r=restart.Propose({.5,.5,.5,.5,0});
        Check(r.kind==(i==256 ? ComplexProposalKind::RestartedLayout:ComplexProposalKind::RejectedOverlap),"bounded failed proposals restart once, never report completion");
    }
    Check(restart.Children().empty()&&restart.Counters().restarts==1&&restart.Counters().overlapRejections==256,"restart clears partial geometry and preserves accounting");
    ComplexLayout uncertain(1,.25,.25,1);
    const auto failure=uncertain.Propose({.5,.5,.5,.5,0x1p-200});
    Check(failure.kind==ComplexProposalKind::NumericalFailure&&failure.geometryStatus==ParticleOverlapStatus::UnsupportedRange,"unsupported nonzero axis component is failure, not rejection");
    Check(uncertain.Propose({.5,.5,.5,.5,0}).kind==ComplexProposalKind::NumericalFailure&&uncertain.Counters().proposals==1,"numerical failure sticky; no conditioning retry");
}
void IndependentBallMoments() {
    std::mt19937_64 rng(0x42616c6c5f7631ULL);
    constexpr unsigned trials=20000;
    double moment[3]={},axisSecond[3]={};
    for(unsigned trial=0;trial<trials;++trial) {
        ComplexLayout one(1.25,.25,.25,1);
        while(!one.Complete()) Check(one.Propose(Draw(rng)).kind!=ComplexProposalKind::NumericalFailure,"one-child ball moment supported");
        const auto c=one.Children()[0];
        const double p[3]={c.centreFromEnvelopeMetres.x,c.centreFromEnvelopeMetres.y,c.centreFromEnvelopeMetres.z};
        const double a[3]={c.particle.axis.x,c.particle.axis.y,c.particle.axis.z};
        for(unsigned k=0;k<3;++k) {moment[k]+=p[k]*p[k];axisSecond[k]+=a[k]*a[k];}
    }
    for(unsigned k=0;k<3;++k) {
        Check(std::abs(moment[k]/trials-.2)<.007,"uniform unit-ball coordinate second moment equals 1/5, rejects cube/shell priors");
        Check(std::abs(axisSecond[k]/trials-1./3)<.009,"one-child isotropic axis second moment equals 1/3");
    }
}
void SampledControlsAndConstructionCosts() {
    std::mt19937_64 rng(0x48794c496f53ULL);
    constexpr unsigned trials=3000;
    double sum[3]={},second[3]={},axes[3]={};
    std::uint64_t proposals=0,restarts=0,overlaps=0,maxProposals=0;
    // SG/SS/SB share the same dimensionless packing law. Test each actual size
    // with independent seeds rather than assuming that floating range is equal.
    for(double q:{.25,.5,1.}) {
        proposals=0;restarts=0;overlaps=0;maxProposals=0;
        for(unsigned trial=0;trial<trials;++trial) {
            const double a=q*.17e-6/2,b=q*.41e-6/2,r=2*b;
            ComplexLayout layout(r,a,b,5);
            for(unsigned work=0;!layout.Complete()&&work<100000;++work) {
                const auto result=layout.Propose(Draw(rng));
                Check(result.kind!=ComplexProposalKind::NumericalFailure,"preset random proposal geometry supported");
            }
            Check(layout.Complete(),"preset N5 construction completes within explicit test-only work budget");
            const auto& c=layout.Counters();proposals+=c.proposals;restarts+=c.restarts;overlaps+=c.overlapChecks;maxProposals=std::max(maxProposals,c.proposals);
            for(std::size_t i=0;i<layout.Children().size();++i) {
                const auto& child=layout.Children()[i];const auto v=child.centreFromEnvelopeMetres;
                Check(std::hypot(v.x,v.y,v.z)+b<=r*(1+1e-15),"independent bounding-ball containment control");
                for(std::size_t j=0;j<i;++j) {
                    const auto p=layout.Children()[j];
                    const auto verdict=ClassifyProlateOverlap(p.particle,child.particle,{v.x-p.centreFromEnvelopeMetres.x,v.y-p.centreFromEnvelopeMetres.y,v.z-p.centreFromEnvelopeMetres.z}).status;
                    Check(verdict==ParticleOverlapStatus::Disjoint||verdict==ParticleOverlapStatus::Touching,"all accepted children nonoverlapping");
                }
                // Finite RSA layout biases accepted spatial occupancy; only
                // rotational invariance, not independent accepted axes, is tested.
                const double components[3]={v.x/b,v.y/b,v.z/b};
                const auto axis=child.particle.axis;const double direction[3]={axis.x,axis.y,axis.z};
                for(unsigned k=0;k<3;++k) {sum[k]+=components[k];second[k]+=components[k]*components[k];axes[k]+=direction[k]*direction[k];}
            }
        }
        std::printf("preset S1/S2/S3 q=%.2f N=5 trials=%u mean_proposals=%.6f max_proposals=%llu restarts=%llu mean_pair_checks=%.6f\n",q,trials,double(proposals)/trials,static_cast<unsigned long long>(maxProposals),static_cast<unsigned long long>(restarts),double(overlaps)/trials);
    }
    const double n=trials*3.*5.;
    for(unsigned k=0;k<3;++k) {
        Check(std::abs(sum[k]/n)<.015,"sampled packing centre mean rotational control");
        Check(std::abs(axes[k]/n-1./3)<.015,"sampled packed-axis rotational control");
        Check(std::abs(second[k]/n-second[(k+1)%3]/n)<.015,"sampled packed-centre second-moment rotational control");
    }
}
}
int main() {ExactControls();IndependentBallMoments();SampledControlsAndConstructionCosts();std::printf("%u checks passed\n",checks);}
