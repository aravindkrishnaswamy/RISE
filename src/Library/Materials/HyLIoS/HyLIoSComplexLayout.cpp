#include "pch.h"
#include "HyLIoSComplexLayout.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#error HyLIoSComplexLayout requires strict floating-point evaluation
#endif
namespace RISE::HyLIoS {
namespace {
constexpr double Pi=3.141592653589793238462643383279502884;
bool FinitePositive(double x) { return std::isfinite(x)&&x>0; }
// TwoSum retains the exact difference residual without a tolerance. Select the
// greatest representable value <= R-b; a downward step is needed only if the
// rounded difference exceeds the exact difference. No geometric epsilon.
double LowerDifference(double a,double b) {
    const double sum=a-b;
    const double bv=sum-a;
    const double residual=(a-(sum-bv))+(-b-bv);
    return residual<0 ? std::nextafter(sum,0.) : sum;
}
// Error-free expansions test N*a*a*b <= R*R*R independently of rounded
// volume ratios. All dimensions are power-of-two-scaled; the exponent spread
// restriction keeps every degree-three product far above underflow.
using Expansion=std::vector<double>;
void AddTerm(Expansion& e,double value) {
    Expansion result;result.reserve(e.size()+1);
    for(double x:e) {
        const double sum=value+x,bv=sum-value;
        const double error=(value-(sum-bv))+(x-bv);
        if(error!=0) result.push_back(error);
        value=sum;
    }
    if(value!=0||result.empty()) result.push_back(value);
    e.swap(result);
}
Expansion Product(Expansion e,double value) {
    Expansion result;
    for(double x:e) {
        const double product=x*value;
        AddTerm(result,std::fma(x,value,-product));AddTerm(result,product);
    }
    return result;
}
bool ExceedsEnvelopeVolume(double radius,double minor,double major,std::size_t count) {
    static_assert(sizeof(std::size_t)<=sizeof(std::uint64_t),"unsupported size_t");
    int exponent=0;std::frexp(radius,&exponent);
    const double r=std::ldexp(radius,-exponent),a=std::ldexp(minor,-exponent),b=std::ldexp(major,-exponent);
    if(a<0x1p-100||b<0x1p-100) throw std::range_error("unsupported complex dimension spread");
    const auto n=static_cast<std::uint64_t>(count);
    Expansion difference;
    const double pieces[2]={static_cast<double>(n&0xffffffffU),static_cast<double>(n>>32)*0x1p32};
    for(double piece:pieces) {
        const auto term=Product(Product(Product(Expansion{piece},a),a),b);
        for(double x:term) AddTerm(difference,x);
    }
    const auto envelope=Product(Product(Expansion{-r},r),r);
    for(double x:envelope) AddTerm(difference,x);
    for(auto i=difference.rbegin();i!=difference.rend();++i) if(*i!=0) return *i>0;
    return false;
}
bool Verdict(ParticleOverlapStatus s) {
    return s==ParticleOverlapStatus::Overlap||s==ParticleOverlapStatus::Touching||s==ParticleOverlapStatus::Disjoint;
}
}
ComplexLayout::ComplexLayout(double radius,double minor,double major,std::size_t count)
    :minor_(minor),major_(major),centreRadius_(0),target_(count) {
    // Validate the FP environment before comparing dimensions: FTZ/DAZ can
    // otherwise misclassify valid subnormal inputs as malformed geometry.
    const ProlateParticle unitControl{1,1,{1,0,0}};
    if(ClassifyProlateOverlap(unitControl,unitControl,{0,0,0}).status!=ParticleOverlapStatus::Overlap)
        throw std::range_error("unsupported complex floating-point environment");
    if(!FinitePositive(radius)||!FinitePositive(minor)||!FinitePositive(major)||major<minor||radius<major||
        (radius==major&&count>1)||count>MaximumChildren) throw std::invalid_argument("unsupported complex dimensions/count");
    const ProlateParticle environmentControl{radius,radius,{1,0,0}};
    if(ClassifyProlateOverlap(environmentControl,environmentControl,{0,0,0}).status!=ParticleOverlapStatus::Overlap)
        throw std::range_error("unsupported complex floating-point environment/range");
    if(ExceedsEnvelopeVolume(radius,minor,major,count)) throw std::invalid_argument("child volume exceeds envelope volume");
    centreRadius_=LowerDifference(radius,major);
    if(radius>major&&!(centreRadius_>0)) throw std::range_error("complex centre support underflows");
}
ComplexProposalResult ComplexLayout::Fail(ParticleOverlapStatus status) {
    failed_=true;failure_=status;
    return {ComplexProposalKind::NumericalFailure,status};
}
ComplexProposalResult ComplexLayout::Reject(ComplexProposalKind kind) {
    if(childProposals_==ProposalsPerChildBeforeRestart) {
        children_.clear();childProposals_=0;++counters_.restarts;
        return {ComplexProposalKind::RestartedLayout};
    }
    return {kind};
}
ComplexProposalResult ComplexLayout::Propose(const std::array<double,5>& u) {
    if(failed_) return {ComplexProposalKind::NumericalFailure,failure_};
    if(Complete()) return {ComplexProposalKind::Complete};
    for(double x:u) if(!std::isfinite(x)||x<0||x>=1) return {ComplexProposalKind::InvalidSample};
    ++counters_.proposals;++childProposals_;
    const ParticleVector centre{(2*u[0]-1)*centreRadius_,(2*u[1]-1)*centreRadius_,(2*u[2]-1)*centreRadius_};
    const double components[3]={centre.x,centre.y,centre.z};
    for(std::size_t i=0;i<3;++i) if(centreRadius_>0&&(2*u[i]-1)!=0&&components[i]==0)
        return Fail(ParticleOverlapStatus::UnsupportedRange);
    // Exact sphere polynomial control tests |2*c| <= 2*d. This avoids square
    // under/overflow and a new approximate norm or containment predicate.
    if(centreRadius_>0) {
        const ProlateParticle control{centreRadius_,centreRadius_,{1,0,0}};
        const ParticleVector twiceCentre{2*centre.x,2*centre.y,2*centre.z};
        const auto containment=ClassifyProlateOverlap(control,control,twiceCentre);
        if(!Verdict(containment.status)) return Fail(containment.status);
        if(containment.status==ParticleOverlapStatus::Disjoint) {
            ++counters_.containmentRejections;return Reject(ComplexProposalKind::RejectedContainment);
        }
    }
    const double z=1-2*u[3],azimuth=2*Pi*u[4];
    const double radial=std::sqrt((1-z)*(1+z));
    const ProlateParticle child{minor_,major_,{radial*std::cos(azimuth),radial*std::sin(azimuth),z}};
    // Self-classification certifies the orientation/axis range and FP environment
    // even for the first child or the degenerate one-centre support.
    const auto self=ClassifyProlateOverlap(child,child,{0,0,0});
    if(self.status!=ParticleOverlapStatus::Overlap) return Fail(self.status);
    for(const auto& previous:children_) {
        const ParticleVector delta{centre.x-previous.centreFromEnvelopeMetres.x,
            centre.y-previous.centreFromEnvelopeMetres.y,centre.z-previous.centreFromEnvelopeMetres.z};
        const auto overlap=ClassifyProlateOverlap(previous.particle,child,delta);
        ++counters_.overlapChecks;
        if(!Verdict(overlap.status)) return Fail(overlap.status);
        if(overlap.status==ParticleOverlapStatus::Overlap) {
            ++counters_.overlapRejections;return Reject(ComplexProposalKind::RejectedOverlap);
        }
    }
    children_.push_back({child,centre});childProposals_=0;
    return {Complete() ? ComplexProposalKind::Complete:ComplexProposalKind::AcceptedChild};
}
}
