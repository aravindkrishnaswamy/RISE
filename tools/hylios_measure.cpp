#include "Agent/Json.h"
#include "Materials/HyLIoS/HyLIoSLocalBake.h"
#include "Materials/HyLIoS/HyLIoSForwardTransport.h"
#include "Materials/HyLIoS/HyLIoSOptics.h"
#include "Materials/HyLIoS/HyLIoSSurfaceBoundary.h"
#include "Materials/HyLIoS/HyLIoSResolvedExperiment.h"
#include "Materials/HyLIoS/HyLIoSForwardExperiment.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace RISE::HyLIoS;
using J = RISE::Agent::JsonValue;
namespace {
constexpr const char *MeasurementVersion = "hylios-forward-measure-v6";
constexpr double MeasurementPi=3.141592653589793238462643383279502884;
volatile std::sig_atomic_t interrupted = 0;
void Signal(int) {
    interrupted = 1;
}
void Require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
J Num(double x) {
    Require(std::isfinite(x), "nonfinite result cannot be published");
    return J::MakeNumber(x);
}
J Str(const std::string &s) {
    return J::MakeString(s);
}
J Bool(bool b) {
    return J::MakeBool(b);
}
void Put(J &object, const std::string &key, double v) {
    object.set(key, Num(v));
}
std::uint64_t Integer(const std::string &s, const std::string &label, std::uint64_t maximum,
                      bool allowZero = false) {
    Require(!s.empty(), label + " is empty");
    std::uint64_t result = 0;
    for (char c : s) {
        Require(c >= '0' && c <= '9', label + " must be an unsigned decimal integer");
        const auto digit = static_cast<unsigned>(c - '0');
        Require(result <= (maximum - digit) / 10, label + " exceeds supported bound");
        result = result * 10 + digit;
    }
    Require(allowZero || result > 0, label + " must be positive");
    return result;
}
double Real(const std::string &s, const std::string &label, double minimum = 0,
            bool strict = false) {
    std::size_t end = 0;
    double v = 0;
    try {
        v = std::stod(s, &end);
    } catch (const std::exception &) {
        throw std::runtime_error(label + " invalid number");
    }
    Require(end == s.size() && std::isfinite(v) && (strict ? v > minimum : v >= minimum),
            label + " outside finite supported range");
    return v;
}
struct Options {
    std::string specimen, output, placement, postScatter, complexLayout, boundary, seedText, estimator = "analog";
    double incidentPolar=0,incidentAzimuth=0;
    double wavelength = 0, roulette = 0, fiberRadius = 1e-7, fiberFraction = .22,
           fiberEta = 1.5 / 1.33, exteriorIOR = 1;
    std::uint64_t histories = 0, maxTransitions = 0, seed = 0;
    J explicitOptions = J::MakeObject();
};
void Usage() {
    std::cout << "hylios_measure --specimen FILE --wavelength-nm N --placement fixed|null "
                 "--post-scatter diagnostic|discard --boundary smooth|hylios2015 --histories N --max-transitions N --seed U64 --output FILE\n"
                 "  [--incident-polar-deg N] [--incident-azimuth-deg N] [--complex-layout explicit-pack-v1] [--estimator analog|weighted] [--roulette-target N] [--exterior-ior N]\n"
                 "  [--fiber-radius-m N] [--fiber-volume-fraction N] [--fiber-relative-ior N]\n"
                 "Forward particle/complex transport supports explicit smooth or hylios2015 boundary selection; adjoint sampling is not "
                 "supported.\n";
}
Options Parse(int argc, char **argv) {
    Options o;
    std::map<std::string, std::string> flags;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        Require(i + 1 < argc, "missing value for " + key);
        Require(flags.emplace(key, argv[++i]).second, "duplicate CLI option: " + key);
    }
    const std::set<std::string> allowed = {"--specimen",
                                           "--wavelength-nm",
                                           "--incident-polar-deg",
                                           "--incident-azimuth-deg",
                                           "--placement",
                                           "--post-scatter",
                                           "--complex-layout",
                                           "--boundary",
                                           "--histories",
                                           "--max-transitions",
                                           "--seed",
                                           "--output",
                                           "--estimator",
                                           "--roulette-target",
                                           "--exterior-ior",
                                           "--fiber-radius-m",
                                           "--fiber-volume-fraction",
                                           "--fiber-relative-ior"};
    for (const auto &pair : flags) {
        Require(allowed.count(pair.first) > 0, "unknown option: " + pair.first);
        o.explicitOptions.set(pair.first, Str(pair.second));
    }
    for (const auto &key : {"--specimen", "--wavelength-nm", "--placement", "--post-scatter", "--boundary",
                            "--histories", "--max-transitions", "--seed", "--output"})
        Require(flags.count(key) > 0, std::string("required option: ") + key);
    o.specimen = flags.at("--specimen");
    o.output = flags.at("--output");
    Require(!o.specimen.empty() && !o.output.empty(), "empty input/output path");
    o.placement = flags.at("--placement");
    Require(o.placement == "fixed" || o.placement == "null", "placement must be fixed or null");
    o.postScatter = flags.at("--post-scatter");
    Require(o.postScatter == "diagnostic" || o.postScatter == "discard",
            "post-scatter must be diagnostic or discard");
    if (flags.count("--complex-layout")) {
        o.complexLayout = flags.at("--complex-layout");
        Require(o.complexLayout == "explicit-pack-v1", "complex-layout must be explicit-pack-v1");
    }
    o.boundary = flags.at("--boundary");
    Require(o.boundary == "smooth" || o.boundary == "hylios2015",
            "only explicit --boundary smooth or hylios2015 is implemented");
    o.wavelength = Real(flags.at("--wavelength-nm"), "wavelength", 0, true);
    if(flags.count("--incident-polar-deg")) o.incidentPolar=Real(flags.at("--incident-polar-deg"),"incident-polar-deg");
    Require(o.incidentPolar<90,"incident-polar-deg must be in [0,90)");
    if(flags.count("--incident-azimuth-deg")) {
        o.incidentAzimuth=std::fmod(Real(flags.at("--incident-azimuth-deg"),"incident-azimuth-deg",-std::numeric_limits<double>::max()),360.);
        if(o.incidentAzimuth<0) o.incidentAzimuth+=360;
        if(o.incidentAzimuth==360) o.incidentAzimuth=0;
    }
    if(o.incidentPolar==0) o.incidentAzimuth=0; // exact normal launch and canonical pole azimuth
    o.histories = Integer(flags.at("--histories"), "histories", 10000000);
    o.maxTransitions = Integer(flags.at("--max-transitions"), "max-transitions", 10000000);
    Require(o.histories <= 10000000000ULL / o.maxTransitions,
            "history*transition work budget exceeds 10^10");
    o.seedText = flags.at("--seed");
    o.seed = Integer(o.seedText, "seed", std::numeric_limits<std::uint64_t>::max(), true);
    if (flags.count("--estimator"))
        o.estimator = flags.at("--estimator");
    Require(o.estimator == "analog" || o.estimator == "weighted",
            "estimator must be analog or weighted");
    if (flags.count("--roulette-target"))
        o.roulette = Real(flags.at("--roulette-target"), "roulette-target");
    Require(o.estimator == "weighted" || o.roulette == 0, "roulette requires weighted estimator");
    if (flags.count("--exterior-ior"))
        o.exteriorIOR = Real(flags.at("--exterior-ior"), "exterior-ior", 0, true);
    if (flags.count("--fiber-radius-m"))
        o.fiberRadius = Real(flags.at("--fiber-radius-m"), "fiber-radius-m", 0, true);
    if (flags.count("--fiber-volume-fraction"))
        o.fiberFraction = Real(flags.at("--fiber-volume-fraction"), "fiber-volume-fraction");
    Require(o.fiberFraction <= 1, "fiber fraction must be <=1");
    if (flags.count("--fiber-relative-ior"))
        o.fiberEta = Real(flags.at("--fiber-relative-ior"), "fiber-relative-ior", 0, true);
    Require(std::filesystem::absolute(std::filesystem::u8path(o.specimen)).lexically_normal() !=
                std::filesystem::absolute(std::filesystem::u8path(o.output)).lexically_normal(),
            "input and output paths must differ");
    if (std::filesystem::exists(std::filesystem::u8path(o.output)))
        Require(!std::filesystem::equivalent(std::filesystem::u8path(o.specimen),
                                             std::filesystem::u8path(o.output)),
                "input and output name the same file");
    return o;
}
std::string Read(const std::string &path) {
    ValidateExperimentUnicode(path);
    const auto bytes=LocalAssetDetail::ReadRegularFile(path,64*1024*1024);
    return std::string(bytes.begin(),bytes.end());
}

std::string Outcome(ForwardTermination t) {
    switch (t) {
    case ForwardTermination::ExternalReflection:
        return "external_reflection";
    case ForwardTermination::SubsurfaceReturn:
        return "subsurface_return";
    case ForwardTermination::Absorbed:
        return "physical_absorption";
    case ForwardTermination::RouletteKilled:
        return "roulette_killed";
    case ForwardTermination::ZeroWeight:
        return "zero_weight";
    case ForwardTermination::Cancelled:
        return "cancelled";
    case ForwardTermination::InvalidSample:
        return "invalid_sample";
    case ForwardTermination::NumericalFailure:
        return "numerical_failure";
    case ForwardTermination::UnplaceableBoundaryCandidate:
        return "unplaceable_candidate";
    case ForwardTermination::ForwardPerturbationReentry:
        return "post_exit_reentry";
    case ForwardTermination::BudgetExceeded:
        return "budget_exceeded";
    default:
        throw std::runtime_error("unterminated history publication refused");
    }
}
bool Escaped(ForwardTermination t) {
    return t == ForwardTermination::ExternalReflection || t == ForwardTermination::SubsurfaceReturn;
}
bool Unresolved(ForwardTermination t) {
    return t == ForwardTermination::Cancelled || t == ForwardTermination::InvalidSample ||
           t == ForwardTermination::NumericalFailure ||
           t == ForwardTermination::UnplaceableBoundaryCandidate ||
           t == ForwardTermination::ForwardPerturbationReentry ||
           t == ForwardTermination::BudgetExceeded || t == ForwardTermination::ZeroWeight;
}
J Array(const std::array<double, 6> &a) {
    J j = J::MakeArray();
    for (double v : a)
        j.push_back(Num(v));
    return j;
}
// Nonzero observations are scaled by their running maximum. This is Welford,
// not a subtraction of squared sums: squared tiny/large physical weights never
// have to fit binary64. Zeros are merged analytically only at publication.
struct SparseSummary {
    double sumWeight=0,mean=0;
    std::optional<double> standardError;
    std::uint64_t nonzeroCount=0;
};
// Volatile materialization pins these binary64 operations even when the caller
// TU is built with reassociation. No rounded absolute mean feeds variance.
double RoundedMomentSum(double a,double b) {volatile double value=a+b;return value;}
double RoundedMomentDifference(double a,double b) {volatile double value=a-b;return value;}
double RoundedMomentQuotient(double a,double b) {volatile double value=a/b;return value;}
double RoundedMomentProduct(double a,double b) {volatile double value=a*b;return value;}
// All primitive rounding nodes are materialized so reassociation in the caller
// cannot erase a residual. The pair is a compensated value, not wider hardware FP.
struct MomentPair {double high=0,low=0;};
MomentPair MomentTwoSum(double a,double b) {
    const double sum=RoundedMomentSum(a,b),bv=RoundedMomentDifference(sum,a);
    const double error=RoundedMomentSum(RoundedMomentDifference(a,RoundedMomentDifference(sum,bv)),RoundedMomentDifference(b,bv));
    return {sum,error};
}
MomentPair MomentAdd(MomentPair a,MomentPair b) {
    const auto first=MomentTwoSum(a.high,b.high);
    return MomentTwoSum(first.high,RoundedMomentSum(first.low,RoundedMomentSum(a.low,b.low)));
}
MomentPair MomentSubtract(MomentPair a,MomentPair b) {return MomentAdd(a,{-b.high,-b.low});}
MomentPair MomentMultiply(MomentPair a,MomentPair b) {
    const double product=RoundedMomentProduct(a.high,b.high);
    double error=std::fma(a.high,b.high,-product);
    error=RoundedMomentSum(error,RoundedMomentProduct(a.high,b.low));
    error=RoundedMomentSum(error,RoundedMomentProduct(a.low,b.high));
    error=RoundedMomentSum(error,RoundedMomentProduct(a.low,b.low));
    return MomentTwoSum(product,error);
}
MomentPair MomentDivide(MomentPair a,MomentPair b) {
    const double first=RoundedMomentQuotient(a.high,b.high);
    const auto residual=MomentSubtract(a,MomentMultiply({first,0},b));
    const double second=RoundedMomentQuotient(RoundedMomentSum(residual.high,residual.low),b.high);
    return MomentTwoSum(first,second);
}
MomentPair MomentCount(std::uint64_t value) {
    return MomentTwoSum(static_cast<double>(value>>32)*0x1p32,static_cast<double>(value&0xffffffffULL));
}
double MomentValue(MomentPair value) {return RoundedMomentSum(value.high,value.low);}
bool FiniteMoment(MomentPair value) {return std::isfinite(value.high)&&std::isfinite(value.low);}
class SparseMoments {
public:
    std::uint64_t Count() const { return count_; }
    void Observe(double x) {
        Require(std::isfinite(x)&&x>=0,"measurement moment weight must be finite and nonnegative");
        if(x==0) return;
        Require(count_!=std::numeric_limits<std::uint64_t>::max(),"measurement moment count overflow");
        if(count_==0) {maximum_=minimum_=origin_=x;positiveMean_={1,0};count_=1;return;}
        if(x==maximum_&&x==minimum_) {++count_;return;}
        const auto next=MomentCount(count_+1);
        if(x>maximum_) {
            positiveMean_=MomentMultiply(positiveMean_,MomentDivide({maximum_,0},{x,0}));maximum_=x;
        }
        minimum_=std::min(minimum_,x);
        const auto positiveDelta=MomentSubtract(MomentDivide({x,0},{maximum_,0}),positiveMean_);
        positiveMean_=MomentAdd(positiveMean_,MomentDivide(positiveDelta,next));
        const double nextRange=RoundedMomentDifference(maximum_,minimum_);
        if(nextRange>range_) {
            const auto ratio=MomentDivide({range_,0},{nextRange,0});
            meanOffset_=MomentMultiply(meanOffset_,ratio);
            m2_=MomentMultiply(MomentMultiply(m2_,ratio),ratio);range_=nextRange;
        }
        if(range_>0) {
            const auto centered=MomentSubtract({x,0},{origin_,0});
            const auto value=MomentDivide(centered,{range_,0});
            const auto delta=MomentSubtract(value,meanOffset_);
            meanOffset_=MomentAdd(meanOffset_,MomentDivide(delta,next));
            const auto increment=MomentMultiply(MomentMultiply(delta,delta),MomentDivide(MomentCount(count_),next));
            m2_=MomentAdd(m2_,increment);
        }
        ++count_;
        Require(FiniteMoment(positiveMean_)&&MomentValue(positiveMean_)>0&&FiniteMoment(meanOffset_)&&
            FiniteMoment(m2_)&&MomentValue(m2_)>=0,"measurement centered moment range failure");
    }
    // Each group's own nonnegative fraction is used for its positive mean.
    // Subtraction-form blending loses a rare large contribution when a tiny
    // positive group dominates. Centered offsets and M2 retain their residuals.
    void MergeNonzero(const SparseMoments& other) {
        if(other.count_==0) return;
        if(count_==0) {*this=other;return;}
        Require(other.count_<=std::numeric_limits<std::uint64_t>::max()-count_,"measurement merged count overflow");
        const auto combined=count_+other.count_;
        const double maximum=std::max(maximum_,other.maximum_),minimum=std::min(minimum_,other.minimum_);
        const auto fractionA=MomentDivide(MomentCount(count_),MomentCount(combined));
        const auto fractionB=MomentDivide(MomentCount(other.count_),MomentCount(combined));
        const auto positiveA=MomentMultiply(positiveMean_,MomentDivide({maximum_,0},{maximum,0}));
        const auto positiveB=MomentMultiply(other.positiveMean_,MomentDivide({other.maximum_,0},{maximum,0}));
        const auto positiveMean=MomentAdd(MomentMultiply(positiveA,fractionA),MomentMultiply(positiveB,fractionB));
        const double range=RoundedMomentDifference(maximum,minimum);
        MomentPair meanOffset,m2;
        if(range>0) {
            const auto ratioA=MomentDivide({range_,0},{range,0}),ratioB=MomentDivide({other.range_,0},{range,0});
            const auto meanA=MomentMultiply(meanOffset_,ratioA);
            const auto meanB=MomentAdd(MomentDivide(MomentSubtract({other.origin_,0},{origin_,0}),{range,0}),
                MomentMultiply(other.meanOffset_,ratioB));
            const auto delta=MomentSubtract(meanB,meanA);
            meanOffset=MomentAdd(meanA,MomentMultiply(delta,fractionB));
            const auto cross=MomentMultiply(MomentMultiply(delta,delta),MomentMultiply(MomentCount(count_),fractionB));
            m2=MomentAdd(MomentAdd(MomentMultiply(MomentMultiply(m2_,ratioA),ratioA),
                MomentMultiply(MomentMultiply(other.m2_,ratioB),ratioB)),cross);
        }
        Require(FiniteMoment(positiveMean)&&MomentValue(positiveMean)>0&&FiniteMoment(meanOffset)&&
            FiniteMoment(m2)&&MomentValue(m2)>=0,"measurement merged centered moment range failure");
        maximum_=maximum;minimum_=minimum;positiveMean_=positiveMean;range_=range;meanOffset_=meanOffset;m2_=m2;count_=combined;
    }
    SparseSummary Summary(std::uint64_t total) const {
        Require(total>0&&total>=count_,"measurement denominator must cover all nonzero histories");
        SparseSummary result;result.nonzeroCount=count_;
        if(total>1) result.standardError=0;
        if(count_==0) return result;
        const auto n=MomentCount(total),nz=MomentCount(count_);
        result.sumWeight=ScaleProduct(MomentMultiply(positiveMean_,nz),"measurement sum weight outside binary64 range");
        result.mean=ScaleProduct(MomentMultiply(positiveMean_,MomentDivide(nz,n)),"measurement mean outside binary64 range");
        if(total>1) {
            const auto zeroFraction=MomentDivide(MomentCount(total-count_),n);
            const auto rangeRatio=MomentDivide({range_,0},{maximum_,0});
            const auto nonzeroM2=MomentMultiply(MomentMultiply(m2_,rangeRatio),rangeRatio);
            const auto zeroM2=MomentMultiply(MomentMultiply(positiveMean_,positiveMean_),MomentMultiply(nz,zeroFraction));
            const auto variance=MomentDivide(MomentDivide(MomentAdd(nonzeroM2,zeroM2),n),MomentCount(total-1));
            result.standardError=ScaleProduct({std::sqrt(MomentValue(variance)),0},"measurement standard error outside binary64 range");
        }
        return result;
    }
private:
    std::uint64_t count_=0;
    double maximum_=0,minimum_=0,origin_=0,range_=0;
    MomentPair positiveMean_,meanOffset_,m2_;
    double ScaleProduct(MomentPair value,const char* message) const {
        volatile double product=maximum_*value.high;
        Require(std::isfinite(product),message);
        const double error=RoundedMomentSum(std::fma(maximum_,value.high,-product),RoundedMomentProduct(maximum_,value.low));
        const double result=RoundedMomentSum(product,error);
        Require(std::isfinite(result)&&(MomentValue(value)==0||result>0),message);return result;
    }
};

// All scalar channels and histogram cells use the same nonnegative moments;
// otherwise tiny physical weights could have a representable SE but an
// unrepresentable squared-weight variance in a second implementation.
struct Moments {
    std::uint64_t n=0;
    SparseMoments values;
    void Add(double x) {
        Require(n!=std::numeric_limits<std::uint64_t>::max(),"measurement history count overflow");
        values.Observe(x);++n;
    }
    SparseSummary Summary() const {return values.Summary(n);}
    J Json() const {
        const auto summary=Summary();J result=J::MakeObject();
        Put(result,"mean",summary.mean);
        result.set("standard_error",summary.standardError ? Num(*summary.standardError):J::MakeNull());
        Put(result,"history_count",static_cast<double>(n));return result;
    }
};
constexpr std::size_t ExitRadialBins=8,ExitAzimuthBins=8,ExitCosineBins=8;
constexpr std::size_t ExitCellCount=ExitRadialBins*ExitAzimuthBins*ExitCosineBins;
constexpr std::array<double,ExitRadialBins> ExitRadialEdges{{0,.0001,.0005,.001,.002,.005,.01,.02}};
std::array<double,ExitAzimuthBins+1> ExitPhiEdges() {
    std::array<double,ExitAzimuthBins+1> edges{};
    for(std::size_t i=0;i<edges.size();++i) edges[i]=2*MeasurementPi*static_cast<double>(i)/ExitAzimuthBins;
    return edges;
}
std::array<double,ExitCosineBins+1> ExitMuEdges() {
    std::array<double,ExitCosineBins+1> edges{};
    for(std::size_t i=0;i<edges.size();++i) edges[i]=static_cast<double>(i)/ExitCosineBins;
    return edges;
}
template<std::size_t N> J NumberArray(const std::array<double,N>& values) {
    J result=J::MakeArray();for(double x:values) result.push_back(Num(x));return result;
}
template<std::size_t N> std::size_t FiniteCell(double value,const std::array<double,N>& edges,bool includeUpper=false) {
    Require(std::isfinite(value)&&value>=edges.front()&&value<=edges.back(),"exit histogram angular coordinate outside domain");
    if(value==edges.back()) {
        Require(includeUpper,"exit histogram upper angular endpoint is excluded");return N-2;
    }
    return static_cast<std::size_t>(std::upper_bound(edges.begin(),edges.end(),value)-edges.begin()-1);
}
struct ExitCell {std::size_t radial,azimuth,cosine;bool radiusUnrepresentable;};
ExitCell LocateExit(SlabVector position,SlabVector direction) {
    Require(std::isfinite(position.x)&&std::isfinite(position.y)&&std::isfinite(position.z),"exit histogram position is nonfinite");
    Require(std::isfinite(direction.x)&&std::isfinite(direction.y)&&std::isfinite(direction.z)&&direction.z<=0,"exit histogram direction is not exterior");
    const double length=std::hypot(direction.x,direction.y,direction.z);
    Require(std::isfinite(length)&&length>0,"exit histogram direction has unsupported norm");
    const double mu=-direction.z/length;
    const auto muEdges=ExitMuEdges();const auto phiEdges=ExitPhiEdges();
    double angle=direction.x==0&&direction.y==0 ? 0:std::atan2(direction.y,direction.x);
    const bool negativeAngle=angle<0;
    if(negativeAngle) angle+=2*MeasurementPi;
    // An original negative angle may round to 2pi after wrapping. It still
    // approaches the periodic seam from below, so belongs in the final bin.
    const auto azimuth=negativeAngle&&angle==2*MeasurementPi ? ExitAzimuthBins-1:FiniteCell(angle,phiEdges);
    const double radius=std::hypot(position.x,position.y);
    const bool unrepresentable=!std::isfinite(radius);
    std::size_t radial=ExitRadialBins-1;
    if(!unrepresentable&&radius<ExitRadialEdges.back())
        radial=static_cast<std::size_t>(std::upper_bound(ExitRadialEdges.begin(),ExitRadialEdges.end(),radius)-ExitRadialEdges.begin()-1);
    return {radial,azimuth,FiniteCell(mu,muEdges,true),unrepresentable};
}
std::size_t ExitIndex(std::size_t radius,std::size_t phi,std::size_t mu) {
    Require(radius<ExitRadialBins&&phi<ExitAzimuthBins&&mu<ExitCosineBins,"exit histogram cell index outside grid");
    return (radius*ExitAzimuthBins+phi)*ExitCosineBins+mu;
}
J SparseJson(const SparseMoments& moments,std::uint64_t total) {
    const auto summary=moments.Summary(total);J result=J::MakeObject();
    Put(result,"sum_weight",summary.sumWeight);Put(result,"mean",summary.mean);
    result.set("standard_error",summary.standardError ? Num(*summary.standardError):J::MakeNull());
    Put(result,"history_count",static_cast<double>(total));
    Put(result,"nonzero_history_count",static_cast<double>(summary.nonzeroCount));return result;
}
class ExitHistogram {
public:
    void Observe(SlabVector position,SlabVector direction,double weight,bool external) {
        Require(std::isfinite(weight)&&weight>=0,"exit histogram weight must be finite and nonnegative");
        if(weight==0) return;
        const auto cell=LocateExit(position,direction);const auto index=ExitIndex(cell.radial,cell.azimuth,cell.cosine);
        cells_[0][index].Observe(weight);cells_[external ? 1:2][index].Observe(weight);nonzeroCellUpdates_+=2;
        if(cell.radiusUnrepresentable) ++unrepresentableRadiusCount_;
    }
    J Json(std::uint64_t total) const {
        J result=J::MakeObject();
        result.set("version",Str("hylios_exit_radius_azimuth_cosine_cells_v1"));
        result.set("normalization",Str("all incident histories, including absorbed, unresolved, roulette-killed and unstarted-cancelled zeros"));
        Put(result,"history_count",static_cast<double>(total));
        Put(result,"nonzero_cell_updates",static_cast<double>(nonzeroCellUpdates_));
        result.set("statistics_accumulation",Str("scaled nonzero Welford; two joint-cell updates per positive terminal escape, missing zero histories merged at publication; marginals merge disjoint joint observations"));
        result.set("conditional_on_escape",Bool(false));result.set("is_density",Bool(false));
        result.set("angular_measure_classification",Str("unclassified mixture; angular atoms may be present; no lineage inferred from terminal name or rounded direction"));
        result.set("continuous_pdf_available",Bool(false));result.set("production_bake_available",Bool(false));
        result.set("axis_order",Str("radius, outgoing azimuth, outgoing cosine; cosine index varies fastest"));
        result.set("cell_mass_measure",Str("returned physical energy per incident history in a radial annulus and outgoing angular cell; exit-position azimuth integrated out"));
        result.set("interval_convention",Str("lower inclusive, upper exclusive; cosine 1 included in last cell; radial last bin is unbounded overflow"));
        result.set("azimuth_convention",Str("atan2(outgoing.y,outgoing.x) from global +x toward +y, wrapped [0,2pi); exact pole x=y=0 uses phi=0"));
        result.set("cosine_convention",Str("outward macro normal (0,0,-1); mu=-direction.z/hypot(direction), removing only stored unit-length rounding"));
        result.set("radial_edges_m",NumberArray(ExitRadialEdges));
        const auto phiEdges=ExitPhiEdges();const auto muEdges=ExitMuEdges();
        result.set("outgoing_azimuth_edges_rad",NumberArray(phiEdges));result.set("outgoing_cosine_edges",NumberArray(muEdges));
        Put(result,"radial_bin_count",ExitRadialBins);Put(result,"azimuth_bin_count",ExitAzimuthBins);Put(result,"cosine_bin_count",ExitCosineBins);
        J overflow=J::MakeObject();Put(overflow,"index",ExitRadialBins-1);Put(overflow,"lower_edge_m",ExitRadialEdges.back());
        overflow.set("upper_edge_m",J::MakeNull());overflow.set("includes_unrepresentable_finite_position_radius",Bool(true));
        result.set("radial_overflow",overflow);Put(result,"unrepresentable_radius_history_count",static_cast<double>(unrepresentableRadiusCount_));
        result.set("density_caution",Str("masses are not finite PDFs or BSDF values; finite cell measures do not classify atomic or singular components; overflow has no finite radial measure"));
        J joint=J::MakeArray();
        std::array<std::array<SparseMoments,ExitAzimuthBins*ExitCosineBins>,3> angular{};
        std::array<std::array<SparseMoments,ExitRadialBins>,3> radial{};
        for(std::size_t r=0;r<ExitRadialBins;++r) for(std::size_t p=0;p<ExitAzimuthBins;++p) for(std::size_t m=0;m<ExitCosineBins;++m) {
            const auto index=ExitIndex(r,p,m);J cell=J::MakeObject();
            Put(cell,"index",index);Put(cell,"radial_bin",r);Put(cell,"azimuth_bin",p);Put(cell,"cosine_bin",m);
            const double dphi=phiEdges[p+1]-phiEdges[p],dmu=muEdges[m+1]-muEdges[m];
            Put(cell,"solid_angle_sr",dmu*dphi);
            Put(cell,"projected_angle_measure_sr",.5*dmu*(muEdges[m+1]+muEdges[m])*dphi);
            if(r+1<ExitRadialEdges.size()) {
                const double width=ExitRadialEdges[r+1]-ExitRadialEdges[r];
                const double area=MeasurementPi*width*(ExitRadialEdges[r+1]+ExitRadialEdges[r]);
                Put(cell,"radial_width_m",width);Put(cell,"annular_area_m2",area);Put(cell,"area_solid_angle_m2_sr",area*dmu*dphi);
            } else {
                cell.set("radial_width_m",J::MakeNull());cell.set("annular_area_m2",J::MakeNull());cell.set("area_solid_angle_m2_sr",J::MakeNull());
            }
            AddChannels(cell,cells_,index,total);
            for(std::size_t channel=0;channel<3;++channel) {
                angular[channel][p*ExitCosineBins+m].MergeNonzero(cells_[channel][index]);
                radial[channel][r].MergeNonzero(cells_[channel][index]);
            }
            joint.push_back(cell);
        }
        result.set("joint_cells",joint);J angularJson=J::MakeArray(),radialJson=J::MakeArray();
        for(std::size_t p=0;p<ExitAzimuthBins;++p) for(std::size_t m=0;m<ExitCosineBins;++m) {
            J cell=J::MakeObject();Put(cell,"azimuth_bin",p);Put(cell,"cosine_bin",m);
            AddChannels(cell,angular,p*ExitCosineBins+m,total);angularJson.push_back(cell);
        }
        for(std::size_t r=0;r<ExitRadialBins;++r) {
            J cell=J::MakeObject();Put(cell,"radial_bin",r);AddChannels(cell,radial,r,total);radialJson.push_back(cell);
        }
        result.set("angular_marginal",angularJson);result.set("radial_marginal",radialJson);
        result.set("uncertainty_interpretation",Str("per-bin sample standard error of realised terminal escaped contribution over all incident histories; unresolved future response is not resolved by this SE"));
        return result;
    }
private:
    std::array<std::array<SparseMoments,ExitCellCount>,3> cells_{};
    std::uint64_t unrepresentableRadiusCount_=0,nonzeroCellUpdates_=0;
    template<std::size_t N> static void AddChannels(J& cell,const std::array<std::array<SparseMoments,N>,3>& moments,std::size_t index,std::uint64_t total) {
        cell.set("all_returns",SparseJson(moments[0][index],total));
        cell.set("external_reflection",SparseJson(moments[1][index],total));
        cell.set("subsurface_return",SparseJson(moments[2][index],total));
    }
};

struct Aggregate {
    std::map<std::string, std::uint64_t> counts, failureDetails;
    std::map<std::string, double> outcomeWeights, counters, surfaceCounters;
    std::array<double, 6> hostAbs{}, particleAbs{}, layerPath{}, hostPath{}, particlePath{};
    Moments reflectance, external, subsurface, absorption, unresolved, accounted;
    std::array<Moments, 6> hostLayerAbsorption, particleLayerAbsorption;
    ExitHistogram exits;
    std::array<Moments,3> returnedAngularMass;
    std::array<std::uint64_t,3> returnedAngularCounts{};
    std::uint64_t draws = 0,callerWork=0;
    void Add(const ForwardState &s, std::uint64_t randomDraws,const ForwardSurfaceWork& surface,
             ForwardTermination boundaryDiagnostic) {
        const auto term = boundaryDiagnostic==ForwardTermination::None ? s.Termination():boundaryDiagnostic;
        callerWork+=s.Counters().workUnits+surface.workUnits;
#define ADD_SURFACE(name) surfaceCounters[#name]+=static_cast<double>(surface.name)
        ADD_SURFACE(workUnits);ADD_SURFACE(branchAttempts);ADD_SURFACE(normalProposalAttempts);
        ADD_SURFACE(normalRejections);ADD_SURFACE(exteriorReflectionBranches);ADD_SURFACE(exteriorTransmissionBranches);
        ADD_SURFACE(interiorReflectionBranches);ADD_SURFACE(interiorTransmissionBranches);ADD_SURFACE(readyDecisions);
        ADD_SURFACE(budgetStops);ADD_SURFACE(cancellations);ADD_SURFACE(invalidSamples);ADD_SURFACE(numericalFailures);
#undef ADD_SURFACE
        const auto outcome = Outcome(term);
        if(Escaped(term)) exits.Observe(s.Position(),s.Direction(),s.Weight(),term==ForwardTermination::ExternalReflection);
        ++counts[outcome];
        if (term == ForwardTermination::NumericalFailure) {
            const auto detail = s.FailureDetail();
            const auto name =
                detail == ForwardFailureDetail::OverlapIndeterminate  ? "overlap_indeterminate"
                : detail == ForwardFailureDetail::OverlapInvalidInput ? "overlap_invalid_input"
                : detail == ForwardFailureDetail::OverlapUnsupportedRange
                    ? "overlap_unsupported_range"
                : detail == ForwardFailureDetail::ArithmeticOrGeometry ? "arithmetic_or_geometry"
                                                                       : "unspecified_arithmetic";
            ++failureDetails[boundaryDiagnostic==ForwardTermination::NumericalFailure ? "surface_boundary":name];
        }
        outcomeWeights[outcome] += s.Weight();
        draws += randomDraws;
        const auto &c = s.Counters();
#define ADD_COUNTER(name) counters[#name] += static_cast<double>(c.name)
        ADD_COUNTER(workUnits);
        ADD_COUNTER(particleCandidates);
        ADD_COUNTER(placementAttempts);
        ADD_COUNTER(nullPlacements);
        ADD_COUNTER(flights);
        ADD_COUNTER(internalInterfaces);
        ADD_COUNTER(bottomReflections);
        ADD_COUNTER(boundaryEncounters);
        ADD_COUNTER(rouletteTrials);
        ADD_COUNTER(rouletteKills);
        ADD_COUNTER(containmentRejections);
        ADD_COUNTER(overlapRejections);
        ADD_COUNTER(acceptedParticles);
        ADD_COUNTER(particleReflections);
        ADD_COUNTER(particleExits);
        ADD_COUNTER(inwardPostExitEvents);
        ADD_COUNTER(acceptedComplexes);
        ADD_COUNTER(complexExits);
        ADD_COUNTER(complexChildEntries);
        ADD_COUNTER(complexChildExits);
        ADD_COUNTER(complexChildReflections);
        ADD_COUNTER(complexHostSegments);
        ADD_COUNTER(complexLayoutProposals);
        ADD_COUNTER(complexLayoutContainmentRejections);
        ADD_COUNTER(complexLayoutOverlapRejections);
        ADD_COUNTER(complexLayoutRestarts);
        ADD_COUNTER(complexLayoutPairChecks);
        ADD_COUNTER(fiberEvents);
        ADD_COUNTER(fiberPasses);
        ADD_COUNTER(perturbationProposals);
        ADD_COUNTER(pathLengthMetres);
#undef ADD_COUNTER
        double absorbed = 0;
        for (std::size_t i = 0; i < 6; ++i) {
            hostLayerAbsorption[i].Add(s.HostAbsorbedByLayer()[i]);
            particleLayerAbsorption[i].Add(s.ParticleAbsorbedByLayer()[i]);
            hostAbs[i] += s.HostAbsorbedByLayer()[i];
            particleAbs[i] += s.ParticleAbsorbedByLayer()[i];
            absorbed += s.HostAbsorbedByLayer()[i] + s.ParticleAbsorbedByLayer()[i];
            layerPath[i] += c.layerPathLengthMetres[i];
            hostPath[i] += c.hostPathLengthMetres[i];
            particlePath[i] += c.particlePathLengthMetres[i];
        }
        const double escaped = Escaped(term) ? s.Weight() : 0,
                     live = Unresolved(term) ? s.Weight() : 0;
        // Disjoint RETURNED law classes; unfinished and nonreturn trials are
        // zeros in the same all-history denominator. No direction comparison.
        std::size_t law=2;
        if(Escaped(term)) {
            if(s.ReturnedAtomMap()==ForwardReturnedAtomMap::PlanarSameExteriorMirror) law=0;
            else if(s.AngularLineage().classification==ForwardAngularClass::AbsolutelyContinuous) law=1;
            ++returnedAngularCounts[law];
        }
        for(std::size_t i=0;i<returnedAngularMass.size();++i)
            returnedAngularMass[i].Add(Escaped(term)&&i==law ? s.Weight():0);
        reflectance.Add(escaped);
        external.Add(term == ForwardTermination::ExternalReflection ? s.Weight() : 0);
        subsurface.Add(term == ForwardTermination::SubsurfaceReturn ? s.Weight() : 0);
        absorption.Add(absorbed);
        unresolved.Add(live);
        accounted.Add(absorbed + escaped + live);
    }
};
J Measure(const Options &o, const ResolvedExperiment &r) {
    const double fiberRate =
        RayleighRate(o.fiberRadius, o.wavelength * 1e-9, o.fiberFraction, o.fiberEta);
    HyLIoSForwardTransport kernel(
        r.Layers(), o.wavelength, o.exteriorIOR, fiberRate,
        o.estimator == "analog" ? AbsorptionEstimator::Analog : AbsorptionEstimator::Weighted,
        o.roulette,
        o.placement == "fixed" ? ForwardPlacementPolicy::FixedEventDiagnosticV1
                               : ForwardPlacementPolicy::NullCollisionPlacementV1,
        o.postScatter == "diagnostic" ? ForwardPostScatterPolicy::DiagnosticRetainParticleV1
                                      : ForwardPostScatterPolicy::DiscardAfterScatterV1);
    std::optional<HyLIoSSurfaceBoundary> surfaceLaw;
    if(o.boundary=="hylios2015") surfaceLaw.emplace(r.SurfaceFoldAspect());
    bool boundaryProgressAnnounced=false;
    Aggregate aggregate;
    const double theta=o.incidentPolar*MeasurementPi/180,phi=o.incidentAzimuth*MeasurementPi/180;
    const SlabVector incoming=o.incidentPolar==0 ? SlabVector{0,0,1} :
        SlabVector{-std::sin(theta)*std::cos(phi),-std::sin(theta)*std::sin(phi),std::cos(theta)};
    SlabVector actualLaunch=incoming;
    for (std::uint64_t history = 0; history < o.histories; ++history) {
        ForwardExperimentRandom rng(o.seed + history);
        auto s = kernel.Begin({0, 0, 0}, incoming);
        if(history==0) actualLaunch=s.Direction();
        ForwardSurfaceWork surface;
        const ForwardExperimentHooks hooks{&boundaryProgressAnnounced,
            [](void*) {return interrupted != 0;},
            [](void* context) {
                auto& announced=*static_cast<bool*>(context);
                if(!announced) {
                    std::cerr<<"hylios_measure: reflected surface branch pending\n"<<std::flush;
                    announced=true;
                }
            }};
        const auto boundaryDiagnostic=DriveForwardExperimentHistory(kernel,
            surfaceLaw ? &*surfaceLaw : nullptr,s,rng,o.maxTransitions,surface,hooks);
        aggregate.Add(s, rng.draws,surface,boundaryDiagnostic);
    }
    J output = J::MakeObject();
    output.set("measurement_version", Str(MeasurementVersion));
    output.set("kernel_version", Str(ForwardTransportVersion));
    output.set("model_scope", Str("six-layer monochromatic individual/explicit-complex forward transport; "
                                  "renderer adjoint unsupported"));
    output.set("placement_policy",
               Str(o.placement == "fixed" ? "FixedEventDiagnosticV1" : "NullCollisionPlacementV1"));
    output.set("complex_layout_policy", o.complexLayout.empty() ? J::MakeNull() : Str(ComplexLayoutVersion));
    J construction = J::MakeObject();
    construction.set("centre_support", Str("uniform ball of largest binary64 radius <= R-b, via cube rejection"));
    construction.set("child_axis_prior", Str("isotropic, independent of incoming ray"));
    Put(construction, "maximum_children", ComplexLayout::MaximumChildren);
    Put(construction, "proposals_per_child_before_restart", ComplexLayout::ProposalsPerChildBeforeRestart);
    construction.set("interaction_count_cap", J::MakeNull());
    output.set("complex_layout_convention", o.complexLayout.empty() ? J::MakeNull() : construction);
    output.set("boundary_law", Str(surfaceLaw ? SurfaceBoundaryVersion:"smooth_planar_dielectric_override_v1"));
    output.set("authored_surface_folds_simulated", Bool(surfaceLaw.has_value()));
    output.set("boundary_work_convention",Str(surfaceLaw ?
        "transport transitions plus every surface branch/proposal attempt; pending branch retained":
        "legacy smooth diagnostic: transport transitions only; smooth boundary draws excluded"));
    output.set("boundary_failure_convention",Str(surfaceLaw ?
        "pending transport cancelled to preserve live power; adapter classifies boundary invalid/numerical failure separately":
        "legacy smooth diagnostic: helper errors abort before output replacement"));
    if(surfaceLaw) Put(output,"surface_polar_normalization",surfaceLaw->Normalization());
    else output.set("surface_polar_normalization",J::MakeNull());
    output.set("authored_surface_fold_aspect_ratio", r.Original().get("surface_fold_aspect_ratio"));
    output.set("post_exit_reentry_policy",
               Str(o.postScatter == "diagnostic" ? "hylios_diagnostic_retain_particle_v1"
                                                  : "hylios_discard_after_scatter_v1"));
    output.set("transport_interpretation", Str(o.postScatter == "discard"
        ? "versioned clean-room virtual particles; discarded after scattering"
        : "diagnostic; inward post-exit directions retain unresolved live weight"));
    output.set("approximate", r.Original().get("approximate"));
    for (const auto &key :
         {"composition_revision", "coverage_profile_id", "spectral_bundle_sha256",
          "resolved_specimen_sha256", "preset_id", "interpretation", "provenance"})
        output.set(key, r.Original().get(key));
    output.set("claimed_resolved_specimen_sha256_verified", Bool(false));
    output.set("input_identity_basis", Str("sha256 of exact opened input bytes; source canonical "
                                           "JSON hash retained as unverified claim"));
    output.set("input_bytes_sha256", Str(r.BytesHash()));
    output.set("specimen_path", Str(o.specimen));
    output.set("normalization", Str("all incident histories, including physical absorption, "
                                    "roulette and unresolved outcomes"));
    std::uint64_t unresolvedCount = 0;
    for (auto term :
         {ForwardTermination::Cancelled, ForwardTermination::InvalidSample,
          ForwardTermination::NumericalFailure, ForwardTermination::UnplaceableBoundaryCandidate,
          ForwardTermination::ForwardPerturbationReentry, ForwardTermination::BudgetExceeded,
          ForwardTermination::ZeroWeight})
        unresolvedCount += aggregate.counts[Outcome(term)];
    output.set("complete_physical_estimate", Bool(unresolvedCount == 0));
    Put(output, "unresolved_history_count", static_cast<double>(unresolvedCount));
    Put(output, "wavelength_nm", o.wavelength);
    Put(output, "initial_history_count", static_cast<double>(o.histories));
    Put(output, "max_transitions_per_history", static_cast<double>(o.maxTransitions));
    output.set("estimator", Str(o.estimator));
    Put(output, "roulette_target", o.roulette);
    Put(output, "exterior_ior", o.exteriorIOR);
    output.set("incident_measurement_rule",Str("one fixed outward-source direction per measurement; no incident-cell jitter or averaging"));
    output.set("incident_direction",Str("incoming travel is opposite outward source wi; launch at local origin; source polar from outward (0,0,-1), azimuth +x toward +y"));
    Put(output,"incident_polar_degrees",o.incidentPolar);Put(output,"incident_source_azimuth_degrees",o.incidentAzimuth);
    output.set("incident_azimuth_defined",Bool(o.incidentPolar!=0));
    output.set("incident_source_direction",NumberArray(std::array<double,3>{{-actualLaunch.x,-actualLaunch.y,-actualLaunch.z}}));
    output.set("incoming_travel_direction",NumberArray(std::array<double,3>{{actualLaunch.x,actualLaunch.y,actualLaunch.z}}));
    output.set("direction_frame",Str("global +x/+y azimuth with +x cross +y=+z inward; outward macro normal is -z; do not treat incoming travel as outward source wi"));
    output.set("exit_distribution",aggregate.exits.Json(o.histories));
    J angular=J::MakeObject();
    angular.set("version",Str(ForwardAngularLineageVersion));
    angular.set("interpretation",Str("ensemble laws conditional on incident direction; returned mass only; not an angular PDF or production bake certificate"));
    angular.set("unknown_policy",Str("positive unclassified returned mass must not be histogrammed as finite f or counted as absorption in a production bake"));
    const std::array<const char*,3> lawNames{{"planar_same_exterior_mirror","absolutely_continuous","unclassified_singular"}};
    for(std::size_t i=0;i<lawNames.size();++i) {
        J channel=aggregate.returnedAngularMass[i].Json();
        Put(channel,"returned_history_count",static_cast<double>(aggregate.returnedAngularCounts[i]));
        angular.set(lawNames[i],channel);
    }
    output.set("returned_angular_law",angular);
    output.set("explicit_cli_options", o.explicitOptions);
    output.set("seed_u64", Str(std::to_string(o.seed)));
    output.set(
        "rng_algorithm",
        Str("splitmix64 top53; state=(seed+zero_based_history_id) mod 2^64; no shared stream"));
    Put(output, "random_draw_count", static_cast<double>(aggregate.draws));
    J fiber = J::MakeObject();
    fiber.set("parameter_convention", Str("paper Eq7 r100nm v.22 eta1.5/1.33 named defaults; each "
                                          "explicit override recorded below"));
    Put(fiber, "radius_m", o.fiberRadius);
    Put(fiber, "volume_fraction", o.fiberFraction);
    Put(fiber, "relative_refractive_index", o.fiberEta);
    Put(fiber, "rate_m_inv", fiberRate);
    output.set("papillary_fiber", fiber);
    output.set("selected_resolved_layers", r.Selected());
    J outcomes = J::MakeObject();
    for (auto term :
         {ForwardTermination::ExternalReflection, ForwardTermination::SubsurfaceReturn,
          ForwardTermination::Absorbed, ForwardTermination::RouletteKilled,
          ForwardTermination::ZeroWeight, ForwardTermination::Cancelled,
          ForwardTermination::InvalidSample, ForwardTermination::NumericalFailure,
          ForwardTermination::UnplaceableBoundaryCandidate,
          ForwardTermination::ForwardPerturbationReentry, ForwardTermination::BudgetExceeded}) {
        const auto name = Outcome(term);
        J value = J::MakeObject();
        Put(value, "count", static_cast<double>(aggregate.counts[name]));
        Put(value, "terminal_live_weight", aggregate.outcomeWeights[name]);
        value.set("unresolved", Bool(Unresolved(term)));
        outcomes.set(name, value);
    }
    output.set("outcomes", outcomes);
    J failures = J::MakeObject();
    for (const auto &pair : aggregate.failureDetails)
        Put(failures, pair.first, static_cast<double>(pair.second));
    output.set("numerical_failure_details", failures);
    J counters = J::MakeObject();
    for (const auto &pair : aggregate.counters)
        Put(counters, pair.first, pair.second);
    output.set("aggregate_counters", counters);
    J surfaceCounters=J::MakeObject();
    for(const auto& pair:aggregate.surfaceCounters) Put(surfaceCounters,pair.first,pair.second);
    output.set("surface_boundary_counters",surfaceCounters);
    Put(output,"caller_work_units",static_cast<double>(aggregate.callerWork));
    output.set("host_absorbed_weight_by_layer", Array(aggregate.hostAbs));
    output.set("particle_absorbed_weight_by_layer", Array(aggregate.particleAbs));
    output.set("path_length_m_by_layer", Array(aggregate.layerPath));
    output.set("host_path_length_m_by_layer", Array(aggregate.hostPath));
    output.set("particle_path_length_m_by_layer", Array(aggregate.particlePath));
    J estimates = J::MakeObject();
    estimates.set("escaped_reflectance", aggregate.reflectance.Json());
    estimates.set("external_reflection", aggregate.external.Json());
    estimates.set("subsurface_return", aggregate.subsurface.Json());
    estimates.set("physical_absorption", aggregate.absorption.Json());
    estimates.set("unresolved_live_weight", aggregate.unresolved.Json());
    estimates.set("accounted_weight", aggregate.accounted.Json());
    Put(estimates, "escaped_plus_unresolved_mean",
        aggregate.reflectance.Summary().mean + aggregate.unresolved.Summary().mean);
    estimates.set(
        "bound_interpretation",
        Str("remaining expected physical escaped power conditional on history lies in [0,live "
            "weight]; future roulette realizations and MC uncertainty remain statistical"));
    J hostEst = J::MakeArray(), particleEst = J::MakeArray();
    for (std::size_t i = 0; i < 6; ++i) {
        hostEst.push_back(aggregate.hostLayerAbsorption[i].Json());
        particleEst.push_back(aggregate.particleLayerAbsorption[i].Json());
    }
    estimates.set("host_absorption_by_layer", hostEst);
    estimates.set("particle_absorption_by_layer", particleEst);
    output.set("estimates", estimates);
    return output;
}
void AtomicWrite(const std::string &path, const std::string &bytes) {
#ifdef _WIN32
    const auto destination = std::filesystem::u8path(path).wstring();
    std::wstring temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 1024; ++attempt) {
        temporary = destination + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
                    std::to_wstring(attempt);
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
            break;
        Require(GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS,
                "unique Windows output creation failed");
    }
    Require(file != INVALID_HANDLE_VALUE, "unique Windows output sibling budget exhausted");
    bool installed = false;
    try {
        std::size_t position = 0;
        while (position < bytes.size()) {
            const DWORD requested =
                static_cast<DWORD>(std::min<std::size_t>(bytes.size() - position, 1024 * 1024));
            DWORD written = 0;
            Require(WriteFile(file, bytes.data() + position, requested, &written, nullptr) &&
                        written > 0,
                    "Windows output write failed");
            position += written;
        }
        Require(FlushFileBuffers(file) != 0, "Windows output sync failed");
        const BOOL closed = CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        Require(closed != 0, "Windows output close failed");
        Require(MoveFileExW(temporary.c_str(), destination.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
                "atomic Windows output replacement failed");
        installed = true;
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        if (!installed)
            DeleteFileW(temporary.c_str());
        throw;
    }
#else
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    int fd = mkstemp(name.data());
    Require(fd >= 0, "cannot create unique output sibling: " + std::string(std::strerror(errno)));
    bool installed = false;
    try {
        std::size_t position = 0;
        while (position < bytes.size()) {
            const auto count = write(fd, bytes.data() + position, bytes.size() - position);
            if (count < 0 && errno == EINTR)
                continue;
            Require(count > 0, "output write failed");
            position += static_cast<std::size_t>(count);
        }
        Require(fsync(fd) == 0, "output sync failed");
        const int closed = close(fd);
        fd = -1;
        Require(closed == 0, "output close failed");
        Require(rename(name.data(), path.c_str()) == 0, "atomic output replacement failed");
        installed = true;
    } catch (...) {
        if (fd >= 0)
            close(fd);
        if (!installed)
            unlink(name.data());
        throw;
    }
#endif
}

std::string BakeExecutablePath(const char*
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__linux__)
    argv0
#endif
) {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);const DWORD n=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    Require(n>0&&n<buffer.size(),"cannot resolve executable path");return std::filesystem::path(std::wstring(buffer.data(),n)).u8string();
#elif defined(__APPLE__)
    std::uint32_t size=0;_NSGetExecutablePath(nullptr,&size);std::vector<char> buffer(size);
    Require(_NSGetExecutablePath(buffer.data(),&size)==0,"cannot resolve executable path");return std::filesystem::absolute(std::filesystem::u8path(buffer.data())).u8string();
#elif defined(__linux__)
    return std::filesystem::read_symlink("/proc/self/exe").u8string();
#else
    return std::filesystem::absolute(std::filesystem::u8path(argv0)).u8string();
#endif
}
LocalResponseGrid UniformBakeGrid(std::uint64_t nt,std::uint64_t np) {
    Require(nt>0&&nt<=64&&np>0&&np<=64,"bake grid axes require 1..64 cells");
    LocalResponseGrid grid;
    for(std::uint64_t i=0;i<=nt;++i)grid.tEdges.push_back(static_cast<double>(i)/static_cast<double>(nt));
    for(std::uint64_t i=0;i<=np;++i)grid.phiEdges.push_back(static_cast<double>(i)/static_cast<double>(np)*LocalResponseTwoPi);
    return grid;
}
int BakeLocalMain(int argc,char** argv,const std::string& executable) {
    if(argc==2&&std::string(argv[1])=="--help") {
        std::cout<<"hylios_measure bake-local [measurement input/model options] --wavelengths-nm N[,N...]\n"
            "  --histories N --max-transitions N --seed U64 --output FILE\n"
            "  [--replicates N] [--guide-histories N] [--incident-t-cells N] [--incident-phi-cells N]\n"
            "  [--outgoing-t-cells N] [--outgoing-phi-cells N] [--max-total-histories N] [--max-total-work N]\n"
            "Bounded analog/no-RR local approximation, N>1 complex refusal at >=1400nm; full-model default measure remains available.\n";
        return 0;
    }
    const std::set<std::string> extras{"--wavelengths-nm","--replicates","--guide-histories","--incident-t-cells","--incident-phi-cells","--outgoing-t-cells","--outgoing-phi-cells","--max-total-histories","--max-total-work","--max-source-bytes","--max-report-bytes"};
    std::map<std::string,std::string> bake;
    std::vector<std::string> base{"hylios_measure"};bool singleWavelength=false;
    for(int i=1;i<argc;++i){const std::string key=argv[i];Require(i+1<argc,"missing value for "+key);const std::string value=argv[++i];
        if(extras.count(key))Require(bake.emplace(key,value).second,"duplicate CLI option: "+key);
        else {base.push_back(key);base.push_back(value);if(key=="--wavelength-nm")singleWavelength=true;}
    }
    LocalBakeConfig config;
    if(bake.count("--wavelengths-nm")) {
        Require(!singleWavelength,"use either --wavelength-nm or --wavelengths-nm");
        const std::string text=bake.at("--wavelengths-nm");std::size_t start=0;
        while(true){const auto end=text.find(',',start);const auto token=text.substr(start,end==std::string::npos?end:end-start);
            config.wavelengthsNm.push_back(Real(token,"wavelength node",0,true));if(end==std::string::npos)break;start=end+1;}
        base.push_back("--wavelength-nm");base.push_back(text.substr(0,text.find(',')));
    }
    std::vector<char*> args;for(auto& value:base)args.push_back(value.data());
    const auto options=Parse(static_cast<int>(args.size()),args.data());
    Require(options.estimator=="analog"&&options.roulette==0,"bake-local requires analog absorption and no roulette");
    Require(options.incidentPolar==0&&options.incidentAzimuth==0,"bake-local uses the complete independent incident grid; fixed launch flags unsupported");
    if(config.wavelengthsNm.empty())config.wavelengthsNm={options.wavelength};
    config.seed=options.seed;config.historiesPerCellPerReplicate=options.histories;config.maximumWorkPerHistory=options.maxTransitions;
    config.boundary=options.boundary=="smooth"?ExperimentBoundary::Smooth:ExperimentBoundary::HyLIoS2015;
    config.placement=options.placement=="fixed"?ForwardPlacementPolicy::FixedEventDiagnosticV1:ForwardPlacementPolicy::NullCollisionPlacementV1;
    config.postScatter=options.postScatter=="discard"?ForwardPostScatterPolicy::DiscardAfterScatterV1:ForwardPostScatterPolicy::DiagnosticRetainParticleV1;
    config.explicitComplexPackV1=options.complexLayout=="explicit-pack-v1";config.exteriorIOR=options.exteriorIOR;
    config.fiberRadiusMetres=options.fiberRadius;config.fiberFraction=options.fiberFraction;config.fiberRelativeIOR=options.fiberEta;
    auto count=[&](const char* key,std::uint64_t fallback){return bake.count(key)?Integer(bake.at(key),key,std::numeric_limits<std::uint64_t>::max()):fallback;};
    config.replicates=count("--replicates",2);config.normalGuideHistoriesPerReplicate=count("--guide-histories",options.histories);
    config.maximumSourceArtifactBytes=count("--max-source-bytes",config.maximumSourceArtifactBytes);config.maximumReportBytes=count("--max-report-bytes",config.maximumReportBytes);
    config.maximumTotalHistories=count("--max-total-histories",config.maximumTotalHistories);config.maximumTotalWork=count("--max-total-work",config.maximumTotalWork);
    config.incident=UniformBakeGrid(count("--incident-t-cells",2),count("--incident-phi-cells",4));
    config.outgoing=UniformBakeGrid(count("--outgoing-t-cells",2),count("--outgoing-phi-cells",4));
    config.buildArtifactPath=executable;
    std::signal(SIGINT,Signal);std::signal(SIGTERM,Signal);
    LocalBakeExecution execution;execution.hooks.isCancelled=[](void*){return interrupted!=0;};
    std::cerr<<"bake-local: signal handlers ready\n"<<std::flush;
    try {
        const auto result=BakeLocalResponse(Read(options.specimen),config,execution);
        Require(!interrupted,"bake cancelled before publication");
        PublishLocalBake(options.output,result,config.limits,[]{Require(!interrupted,"bake cancelled at asset publication");});
        std::cout<<"Wrote bounded local approximation for "<<result.completedHistories<<" histories, bulk work "<<result.bulkWork<<", surface work "<<result.surfaceWork<<" to "<<options.output<<'\n';
    } catch(const LocalBakeFailure& failure) {
        std::cerr<<failure.what()<<"; history_index="<<failure.historyIndex<<" completed="<<failure.completedHistories
            <<" bulk_work="<<failure.bulkWork<<" surface_work="<<failure.surfaceWork<<" outcome="<<Outcome(failure.termination)<<'\n';return interrupted?130:1;
    } catch(const std::exception& failure) {
        if(interrupted){std::cerr<<"bake-local cancelled: "<<failure.what()<<'\n';return 130;}throw;
    }
    return 0;
}

} // namespace
int main(int argc, char **argv) {
    try {
        if(argc>=2&&std::string(argv[1])=="bake-local")return BakeLocalMain(argc-1,argv+1,BakeExecutablePath(argv[0]));
        if (argc == 2 && std::string(argv[1]) == "--help") {
            Usage();
            return 0;
        }
        const auto options = Parse(argc, argv);
        const auto resolved = ResolveExperiment(Read(options.specimen),
            {options.wavelength, options.boundary == "hylios2015" ? ExperimentBoundary::HyLIoS2015 : ExperimentBoundary::Smooth,
             options.complexLayout == "explicit-pack-v1"});
        std::signal(SIGINT, Signal);
        std::signal(SIGTERM, Signal);
        std::cerr << "hylios_measure: signal handlers ready\n" << std::flush;
        const J output = Measure(options, resolved);
        // Complete serialization precedes any output mutation; temporary is unique.
        const auto bytes = RISE::Agent::JsonSerialize(output) + "\n";
        ValidateExperimentUnicode(bytes);
        AtomicWrite(options.output, bytes);
        std::cout << "Wrote diagnostic measurement for " << options.histories
                  << " incident histories to " << options.output << '\n';
        return interrupted ? 130 : 0;
    } catch (const std::exception &e) {
        std::cerr << "hylios_measure: " << e.what() << '\n';
        return 1;
    }
}
