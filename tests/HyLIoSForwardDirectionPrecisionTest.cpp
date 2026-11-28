#include "../src/Library/Materials/HyLIoS/HyLIoSForwardExperiment.h"
#include "../src/Library/Materials/HyLIoS/HyLIoSOptics.h"
#include <cmath>
#include <iostream>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <vector>
#include <string>
using namespace RISE::HyLIoS;
namespace {
void Check(bool b,const char* marker){if(!b)throw std::runtime_error(marker);std::cout<<"ok: "<<marker<<'\n';}
class Constant final:public RISE::ISampler {public:explicit Constant(double v):value(v){}double Get1D()override{return value;}RISE::Point2 Get2D()override{return {value,value};}double value;};
std::array<ForwardLayer,6> Layers(bool particle=false){std::array<ForwardLayer,6> layers;for(auto& l:layers)l={.001,0,1,std::nullopt};if(particle)layers[0].organelle=ForwardOrganelle{ForwardOrganization::IndividualSpheroids,.001,1e-6,1e-6,0,1};return layers;}
void Step(const HyLIoSForwardTransport& kernel,ForwardState& s,RISE::ISampler& rng){const auto result=kernel.Advance(s,rng,1);if(result.kind==ForwardAdvanceKind::OuterBoundaryEncounter){const auto e=s.PendingBoundary();kernel.ResumeBoundary(s,e.token,SampleSlabSmoothDielectric(e,.5));}if(s.Termination()!=ForwardTermination::None)throw std::runtime_error("unexpected terminal before angular event");}
ForwardState BeforeKick(const HyLIoSForwardTransport& kernel){auto s=kernel.Begin({0,0,0},{0,0,1});Constant rng(.5);for(unsigned k=0;s.Phase()!=ForwardPhase::ForwardPerturbation&&k<100;++k){rng.value=s.Phase()==ForwardPhase::CandidatePlacement?0:.5;Step(kernel,s,rng);}Check(s.Phase()==ForwardPhase::ForwardPerturbation,"reached matched-index axial final kick");return s;}

class Sequence final:public RISE::ISampler {
public:
    explicit Sequence(std::initializer_list<double> values):values_(values){}
    double Get1D() override {if(index_==values_.size())throw std::runtime_error("unexpected extra sampler draw");return values_[index_++];}
    RISE::Point2 Get2D() override {return {Get1D(),Get1D()};}
    std::size_t Count()const{return index_;}
private:std::vector<double> values_;std::size_t index_=0;
};
void OrientationDirect(){
    // Rounded references from 1400-decimal evaluation of the independently
    // integrated density: u=(r*d+(1-r)*d*d/2)/(1+r), d=1-|cos|.
    struct Case{double u,r,radius;};
    const Case cases[]={{0x1.79ca10c924223p-67,0x1.0000000000000p-1,0x1.0d5301bfa13a1p-32},{0x0.0000000000001p-1022,0x1.3333333333333p-2,0x1.78d26149296afp-536},{0x0.0000000000001p-1022,0x1.0000000000000p+0,0x1.0000000000000p-536},{0x0.0000000000001p-1022,0x1.87e92154ef7acp-665,0x1.306fe0a31b715p-268},{0x1.87e92154ef7acp-665,0x1.bff2ee48e0530p-333,0x1.21bcd3391bb4dp-166},{0x1.79ca10c924223p-67,0x1.fffffffffffffp-1,0x1.b7cdfd9d7bdbbp-33},{0x1.0000000000000p-3,0x1.0000000000000p-1,0x1.78c3c57c17aabp-1},{0x1.c000000000000p-1,0x1.0000000000000p-1,0x1.78c3c57c17aabp-1},{0x1.fffffffffffffp-1,0x1.0000000000000p-1,0x1.bb67ae8584ca9p-26},{0x1.0000000000000p-1,0x1.0000000000000p-1,0x1.0000000000000p+0}};
    for(const auto& q:cases){
        const auto sample=SampleOrientation(q.u,q.r,1);
        Check(sample.transverseRadius>0&&std::abs(sample.transverseRadius/q.radius-1)<2e-14,"ORIENTATION_DIRECT independent tiny-tail/sphere/aspect/reference radius");
        Check(std::abs(std::hypot(sample.cosine,sample.transverseRadius)-1)<2e-15,"orientation coupled components remain unit length");
        Check(sample.cosine==SampleOrientationCosine(q.u,q.r,1),"orientation cosine API delegates compatibly");
    }
    const auto endpoint=SampleOrientation(0,.5,1);Check(endpoint.cosine==-1&&endpoint.transverseRadius==0,"orientation exact lower endpoint");
    const auto extreme=SampleOrientation(std::numeric_limits<double>::denorm_min(),std::numeric_limits<double>::denorm_min(),std::numeric_limits<double>::max());
    // The underflowed axis ratio is negligible compared with sqrt(2*u).
    Check(extreme.transverseRadius>0&&std::abs(extreme.transverseRadius/0x1.306fe0a31b715p-268-1)<2e-14,"ORIENTATION_DIRECT underflowed axis ratio retains its finite limiting radius");
    for(double r:{1.,.5,1e-100}){
        for(double u:{.125,.25,.375}){auto lo=SampleOrientation(u,r,1),hi=SampleOrientation(1-u,r,1);Check(lo.cosine==-hi.cosine&&lo.transverseRadius==hi.transverseRadius,"orientation exact paired-tail symmetry");}
        // Midpoint quadrature of the inverse CDF, compared with an analytic
        // second moment obtained by integrating x^2*(1-(1-r)*|x|)/(1+r).
        double sum=0;constexpr unsigned count=1000000;
        for(unsigned i=0;i<count;++i){auto q=SampleOrientation((i+.5)/count,r,1);sum+=q.cosine*q.cosine;}
        Check(std::abs(sum/count-(1+3*r)/(6*(1+r)))<2e-9,"orientation independent analytic second moment");
    }
}
void OrientationPlacement(){
    auto layers=Layers(true);layers[0].organelle->semiMajorMetres=2e-6;layers[0].organelle->volumeFraction=.01;layers[0].organelle->realIOR=1.5;
    HyLIoSForwardTransport kernel(layers,550,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiscardAfterScatterV1);
    auto s=kernel.Begin({0,0,0},{0,0,1});Constant fixed(.5);
    for(unsigned k=0;s.Phase()!=ForwardPhase::CandidatePlacement&&k<100;++k)Step(kernel,s,fixed);
    Check(s.Phase()==ForwardPhase::CandidatePlacement&&s.Direction().x==0&&s.Direction().y==0&&s.Direction().z==1,"exact axial orientation placement fixture");
    Sequence placement{1e-20,0,0,0};Step(kernel,s,placement);
    Check(placement.Count()==4&&s.Counters().acceptedParticles==1&&s.Phase()==ForwardPhase::ParticleChord,"orientation placement keeps exactly four draws and accepted central chord");
    Constant reflect(0);Step(kernel,s,reflect);
    Check(s.Phase()==ForwardPhase::ReflectedPerturbation,"orientation fixture reflects at sole exit Fresnel");
    // A central +Z chord has exit gradient Q*Z. For r=1/2 and
    // alpha near pi, its transverse reflected direction is 6*sin(alpha)
    // to relative O(u); the 1400-decimal radius reference is above.
    const double expected=6*0x1.0d5301bfa13a1p-32;
    Check(std::hypot(s.Direction().x,s.Direction().y)>0&&std::abs(std::hypot(s.Direction().x,s.Direction().y)/expected-1)<2e-14,"ORIENTATION_PLACEMENT actual axial tiny-u orientation reaches reflected direction");
}
void ReflectedCosine(){
    auto layers=Layers(true);layers[0].organelle->realIOR=1.5;
    HyLIoSForwardTransport kernel(layers,550,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiscardAfterScatterV1);
    auto s=kernel.Begin({0,0,0},{0,0,1});Constant fixed(.5);
    for(unsigned k=0;s.Phase()!=ForwardPhase::CandidatePlacement&&k<100;++k)Step(kernel,s,fixed);
    Sequence placement{0,0,0,0};Step(kernel,s,placement);Constant reflect(0);Step(kernel,s,reflect);
    Check(s.Phase()==ForwardPhase::ReflectedPerturbation&&s.Direction().x==0&&s.Direction().y==0&&s.Direction().z==-1,"exact axial conditional reflected cosine fixture");
    Sequence angular{1e-20,0};Step(kernel,s,angular);
    Check(angular.Count()==2&&std::abs(std::hypot(s.Direction().x,s.Direction().y)/1e-10-1)<2e-14,"REFLECTED_COSINE_RADIUS actual tiny-u conditional proposal retains sqrt(u)");
}

}
int main(int argc,char** argv){try{
    if(argc>1){const std::string which=argv[1];if(which=="--orientation-direct-only")OrientationDirect();else if(which=="--orientation-placement-only")OrientationPlacement();else if(which=="--reflected-only")ReflectedCosine();else throw std::runtime_error("unknown case");return 0;}
    OrientationDirect();OrientationPlacement();ReflectedCosine();
    const double near=std::nextafter(1400.0,0.0);HyLIoSForwardTransport kernel(Layers(true),near,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiscardAfterScatterV1);
    auto s=BeforeKick(kernel);Check(s.Direction().x==0&&s.Direction().y==0&&s.Direction().z==1,"exact axial fixture before final kick");Constant fixed(.5);Step(kernel,s,fixed);
    Check(std::hypot(s.Direction().x,s.Direction().y)>0,"FINAL_KICK_RADIUS near-1400 axial transverse component retained");
    Check(s.AngularLineage().classification==ForwardAngularClass::AbsolutelyContinuous&&s.Phase()==ForwardPhase::BulkFlight,"accepted positive final kick refreshes AC before bulk");
    const double scale=(5*3.141592653589793238462643383279502884/180)*std::ldexp(1.0,-42)/620,theta=scale*(-2*std::log(.5));
    std::cout<<std::setprecision(17)<<"near_actual="<<std::hypot(s.Direction().x,s.Direction().y)<<" expected="<<std::sin(theta)<<" scale="<<scale<<" ratio="<<std::hypot(s.Direction().x,s.Direction().y)/std::sin(theta)<<'\n';
    Check(std::abs(std::hypot(s.Direction().x,s.Direction().y)/std::sin(theta)-1)<1e-14,"near-1400 transverse value matches sampled law");
    HyLIoSForwardTransport endpoint(Layers(true),1400,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiscardAfterScatterV1);
    auto zero=BeforeKick(endpoint);Step(endpoint,zero,fixed);Check(zero.Direction().x==0&&zero.Direction().y==0&&zero.AngularLineage().classification==ForwardAngularClass::KnownPlanarMirrorEligible,"1400 exact zero-kick identity remains atomic");
    HyLIoSForwardTransport vacuum(Layers(),550,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiscardAfterScatterV1);
    auto bottom=vacuum.Begin({0,0,0},{0,0,1});Constant tiny(1e-20);for(unsigned k=0;bottom.Counters().bottomReflections==0&&k<100;++k)Step(vacuum,bottom,tiny);
    Check(bottom.Counters().bottomReflections==1&&std::hypot(bottom.Direction().x,bottom.Direction().y)>0,"COSINE_RADIUS tiny-u diffuse bottom transverse retained");
    Check(std::abs(std::hypot(bottom.Direction().x,bottom.Direction().y)/std::sqrt(tiny.value)-1)<1e-14,"tiny-u cosine radius equals sqrt(u)");
    HyLIoSForwardTransport fiber(Layers(),550,1,1e6,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiscardAfterScatterV1);
    struct RayleighCase {double u,radius;};
    const RayleighCase rayleighCases[]={{1e-20,2e-10},{std::numeric_limits<double>::denorm_min(),0x1p-536},{.25,0x1.bb67ae8584caap-1},{.5,1}};
    for(const auto& q:rayleighCases) {
        auto ray=fiber.Begin({0,0,0},{0,0,1});Constant flight(.5);
        for(unsigned k=0;ray.Phase()!=ForwardPhase::FiberPerturbation&&k<100;++k)Step(fiber,ray,flight);
        Check(ray.Phase()==ForwardPhase::FiberPerturbation,"reached fiber angular event");
        Check(ray.Direction().x==0&&ray.Direction().y==0&&ray.Direction().z==1,"Rayleigh quantitative fixture has axial incoming direction");
        Sequence angular{q.u,0,0};Step(fiber,ray,angular);
        const double radius=std::hypot(ray.Direction().x,ray.Direction().y);
        Check(angular.Count()==3&&radius>0&&std::abs(radius/q.radius-1)<2e-14,"RAYLEIGH_RADIUS quantitative tiny/minsub/ordinary transverse law");
    }
    HyLIoSForwardTransport common(Layers(true),550,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiscardAfterScatterV1);
    auto commonState=BeforeKick(common);Step(common,commonState,fixed);const double commonTheta=(5*3.141592653589793238462643383279502884/180)*(-2*std::log(.5));
    Check(std::abs(commonState.Direction().z-std::cos(commonTheta))<2e-15&&std::abs(std::hypot(commonState.Direction().x,commonState.Direction().y)-std::sin(commonTheta))<2e-15,"common-angle exact direction components preserve law");
    // Endpoint gaps below are independent exact dyadic references. They do
    // not repeat the optimized constructor's 1400-nm expression.
    struct Boundary {double nm,gap;};
    const Boundary boundaries[]={{std::nextafter(780.0,0.0),620},{780,620},{std::nextafter(780.0,1000.0),620-0x1p-43},{std::nextafter(1024.0,0.0),376+0x1p-43},{1024,376},{std::nextafter(1024.0,2000.0),376-0x1p-42},{near,0x1p-42}};
    for(const auto& b:boundaries){
        HyLIoSForwardTransport boundary(Layers(true),b.nm,1,0,AbsorptionEstimator::Analog,0,ForwardPlacementPolicy::NullCollisionPlacementV1,ForwardPostScatterPolicy::DiscardAfterScatterV1);
        auto state=BeforeKick(boundary);Step(boundary,state,fixed);const double angle=(5*3.141592653589793238462643383279502884/180)*(b.gap/620)*(-2*std::log(.5));
        Check(std::abs(std::hypot(state.Direction().x,state.Direction().y)/std::sin(angle)-1)<1e-14,"ENDPOINT_GAP 780/1024/1400 dyadic gap scale preserved under full LTO");
    }
    // Independent analytic first moment of sin(theta)*exp(-theta/a), [0,pi].
    const double a=5*3.141592653589793238462643383279502884/180;const double expected=(1+a*a)/(1+4*a*a)*std::tanh(3.141592653589793238462643383279502884/(2*a));double sum=0,sum2=0;ForwardExperimentRandom rng(135);
    for(unsigned n=0;n<20000;++n){auto sample=common.Begin({0,0,0},{0,0,1});Constant placement(.5);for(unsigned k=0;sample.Phase()!=ForwardPhase::ForwardPerturbation&&k<100;++k)Step(common,sample,placement);for(unsigned k=0;sample.Phase()!=ForwardPhase::BulkFlight&&k<100;++k)Step(common,sample,rng);if(sample.Phase()!=ForwardPhase::BulkFlight)throw std::runtime_error("statistical final kick did not complete");const double c=sample.Direction().z;sum+=c;sum2+=c*c;}
    const double mean=sum/20000,se=std::sqrt((sum2/20000-mean*mean)/20000);Check(std::abs(mean-expected)<6*se,"common-angle analytic cosine moment within six measured SE");
    std::cout<<"mean="<<mean<<" expected="<<expected<<" se="<<se<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
