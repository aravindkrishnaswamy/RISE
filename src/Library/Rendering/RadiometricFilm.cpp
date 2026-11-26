#include "pch.h"
#include "RadiometricFilm.h"
#include "RadiometricExactResolve.h"
#include "../Utilities/CheckedSize.h"
#include "../Utilities/FiniteMath.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace RISE { namespace Implementation {
namespace {
using RISECBOR64::Value;
constexpr std::uint64_t maxExactCount = std::uint64_t(1) << 53;
bool Error(std::string& error, const char* message) { error = message; return false; }
// Every error-free transform node is materialized as binary64. This is
// compensated arithmetic, independent of long-double width or FP reassociation.
double RoundedSum(double a,double b) { volatile double result=a+b; return result; }
double RoundedDifference(double a,double b) { volatile double result=a-b; return result; }
double RoundedProduct(double a,double b) { volatile double result=a*b; return result; }
double RoundedQuotient(double a,double b) { volatile double result=a/b; return result; }
struct MomentPair { double high=0,low=0; };
bool Finite(MomentPair value) { return IsFiniteDouble(value.high) && IsFiniteDouble(value.low); }
bool Zero(MomentPair value) { return IsZeroDouble(value.high) && IsZeroDouble(value.low); }
MomentPair TwoSum(double a,double b) {
    const double sum=RoundedSum(a,b);
    if(!IsFiniteDouble(sum)) return {sum,0};
    const double bv=RoundedDifference(sum,a);
    const double av=RoundedDifference(sum,bv);
    const double error=RoundedSum(RoundedDifference(a,av),RoundedDifference(b,bv));
    return {sum,error};
}
MomentPair Add(MomentPair a,MomentPair b) {
    auto high=TwoSum(a.high,b.high);
    if(!Finite(high)) return high;
    const auto low=TwoSum(a.low,b.low);
    high=TwoSum(high.high,RoundedSum(high.low,low.high));
    return TwoSum(high.high,RoundedSum(high.low,low.low));
}
MomentPair Subtract(MomentPair a,MomentPair b) { return Add(a,{-b.high,-b.low}); }
MomentPair MultiplyBounded(MomentPair a,MomentPair b) {
    const double product=RoundedProduct(a.high,b.high);
    double error=std::fma(a.high,b.high,-product);
    error=RoundedSum(error,RoundedProduct(a.high,b.low));
    error=RoundedSum(error,RoundedProduct(a.low,b.high));
    error=RoundedSum(error,RoundedProduct(a.low,b.low));
    return TwoSum(product,error);
}
MomentPair Divide(MomentPair a,double divisor) {
    const double first=RoundedQuotient(a.high,divisor);
    if(!IsFiniteDouble(first)) return {first,0};
    const double residual=RoundedSum(std::fma(-first,divisor,a.high),a.low);
    return TwoSum(first,RoundedQuotient(residual,divisor));
}
MomentPair Scale(MomentPair value,int exponent) {
    return TwoSum(std::scalbn(value.high,exponent),std::scalbn(value.low,exponent));
}
bool CanonicalNonnegative(MomentPair value) {
    if(!Finite(value) || value.high<0 || (IsZeroDouble(value.high) && !IsZeroDouble(value.low))) return false;
    if(value.high==std::numeric_limits<double>::max() && value.low>0) return false;
    const auto normalized=TwoSum(value.high,value.low);
    return Finite(normalized) && normalized.high==value.high && normalized.low==value.low;
}
bool AddSquaredDelta(MomentPair prior,MomentPair delta,std::uint64_t oldCount,std::uint64_t newCount,MomentPair& output) {
    if(!Finite(delta)) return false;
    if(!oldCount || Zero(delta)) { output=prior; return true; }
    // Recentered hi/lo delta preserves adjacent-value mean residuals. Bound
    // mantissas before squaring; apply the compensated count ratio before
    // restoring the physical exponent. No early square overflow/underflow.
    int exponent=0; std::frexp(std::fabs(delta.high),&exponent);
    const auto normalized=Scale(delta,-exponent);
    const auto square=MultiplyBounded(normalized,normalized);
    const auto ratio=Divide({static_cast<double>(oldCount),0},static_cast<double>(newCount));
    const auto coefficient=MultiplyBounded(square,ratio);
    if(!CanonicalNonnegative(coefficient) || Zero(coefficient)) return false;
    const int incrementExponent=2*exponent;
    int coefficientExponent=0; std::frexp(coefficient.high,&coefficientExponent);
    int commonExponent=incrementExponent+coefficientExponent;
    if(!Zero(prior)) {
        int priorExponent=0; std::frexp(prior.high,&priorExponent);
        commonExponent=std::max(commonExponent,priorExponent);
    }
    // Add BEFORE restoring the physical exponent. An increment can be below
    // binary64 range while the required total M2 remains normal. Its loss as
    // an auxiliary term at the common scale is finite-precision rounding,
    // not grounds for rejecting a valid trial. A positive total that becomes
    // zero (including zero-prior underflow) is still a measurement failure.
    const auto combined=Add(Scale(prior,-commonExponent),
        Scale(coefficient,incrementExponent-commonExponent));
    MomentPair sum;
    int combinedExponent=0; std::frexp(combined.high,&combinedExponent);
    if(combinedExponent+commonExponent<=std::numeric_limits<double>::min_exponent) {
        // At subnormal spacing, scaling the two components separately can
        // erase the residual that decides a halfway case. Resolve once from
        // their exact dyadic sum. The remaining residual is below TRUE_MIN.
        double rounded=0;
        if(!RadiometricExact::Round(combined.high,combined.low,commonExponent,1,1,rounded)) return false;
        sum={rounded,0};
    } else sum=Scale(combined,commonExponent);
    if(!CanonicalNonnegative(sum) || Zero(sum)) return false;
    output=sum;
    return true;
}
bool ValidatePixel(const RadiometricPixel& px,std::string& error) {
    const MomentPair mean{px.mean,px.meanCorrection},m2{px.m2,px.m2Correction};
    if(px.count!=px.nextSampleIndex || px.count>maxExactCount ||
       !CanonicalNonnegative(mean) || !CanonicalNonnegative(m2) ||
       (!px.count && (!Zero(mean) || !Zero(m2))) ||
       (px.count==1 && (!Zero(m2) || !IsZeroDouble(px.meanCorrection))) || (Zero(mean) && !Zero(m2)))
        return Error(error,"invalid radiometric compensated pixel state");
    if(px.count>=2 && !RadiometricExact::PositiveVarianceRepresentable(px.m2,px.m2Correction,px.count))
        return Error(error,"radiometric variance is not representable in float64");
    return true;
}
bool IdentityValid(const RadiometricIdentity& id, std::string& error, const RadiometricLimits& limits) {
    if(!id.width || !id.height || !IsFiniteDouble(id.wavelengthNM) || id.wavelengthNM <= 0 ||
       !IsFiniteDouble(id.pixelAspectRatio) || id.pixelAspectRatio <= 0 ||
       !id.targetSamples || id.targetSamples > maxExactCount || id.seedAlgorithm.empty() ||
       id.seedAlgorithm.size() > 256u || !RISECBOR64::IsUTF8NFC(id.seedAlgorithm))
        return Error(error,"invalid fixed-wavelength radiometric identity");
    if(id.provenance.empty() || id.provenance.size() > limits.maxProvenanceBytes)
        return Error(error,"radiometric provenance exceeds byte budget or is absent");
    Value provenance;
    if(!RISECBOR64::DecodeCanonical(id.provenance,provenance,&error) || provenance.GetType() != Value::Map)
        return Error(error,"radiometric provenance must be a canonical CBOR map");
    try {
        const auto count = CheckedPixelCount<RadiometricPixel>(id.width,id.height);
        if(count > limits.maxPixelBytes / sizeof(RadiometricPixel))
            return Error(error,"radiometric film exceeds pixel byte budget");
    } catch(const std::exception& ex) { error = ex.what(); return false; }
    return true;
}
}
bool EncodeRadiometricIdentity(const RadiometricIdentity& id, RISECBOR64::Bytes& bytes,
    std::string& error, const RadiometricLimits& limits) {
    error.clear();
    try {
    if(!IdentityValid(id,error,limits)) return false;
    RISECBOR64::Bytes encoded;
    if(!RISECBOR64::Encode(Value::MapValue({
        {"height",Value::Unsigned(id.height)}, {"width",Value::Unsigned(id.width)},
        {"wavelength_nm",Value::Float(id.wavelengthNM)},
        {"pixel_aspect_ratio",Value::Float(id.pixelAspectRatio)},
        {"target_samples",Value::Unsigned(id.targetSamples)}, {"seed",Value::Unsigned(id.seed)},
        {"seed_algorithm",Value::String(id.seedAlgorithm)},
        {"provenance",Value::BytesValue(id.provenance)},
        {"record_kind",Value::String("radiometric_fixed_wavelength_identity_v3")},
        {"schema_version",Value::Unsigned(3)},
        {"accumulator",Value::String("compensated_welford_scaled_delta_square_v3")},
        {"observable",Value::String("spectral_radiance_density")},
        {"units",Value::String("W m^-2 sr^-1 nm^-1")},
        {"wavelength_measure",Value::String("fixed_ordinate_no_bin_factor")}
    }),encoded,&error)) return false;
    bytes.swap(encoded);
    return true;
    } catch(const std::exception& ex) { error=ex.what(); return false; }
}
bool RadiometricPixelStatistics(const RadiometricPixel& px, RadiometricStatistics& output, std::string& error) {
    error.clear();
    if(!ValidatePixel(px,error)) return false;
    RadiometricStatistics result;
    result.mean=px.mean;
    if(px.count>=2) {
        if(!IsZeroDouble(px.m2) || !IsZeroDouble(px.m2Correction)) {
            // Resolve independently from the exact represented M2. Feeding a
            // rounded sample variance into the next division double-rounds
            // at subnormal ties (e.g. 9*TRUE_MIN / 2 / 3).
            if(!RadiometricExact::Round(px.m2,px.m2Correction,0,px.count-1,1,result.sampleVariance) ||
               !RadiometricExact::Round(px.m2,px.m2Correction,0,px.count-1,px.count,result.meanVariance))
                return Error(error,"radiometric variance is not representable in float64");
        }
        result.varianceAvailable=true;
    }
    output = result;
    return true;
}
bool ValidateRadiometricSnapshot(const RadiometricSnapshot& snapshot, std::string& error,
    const RadiometricLimits& limits) {
    RISECBOR64::Bytes identity;
    if(!EncodeRadiometricIdentity(snapshot.identity,identity,error,limits)) return false;
    if(snapshot.status == RadiometricStatus::Invalid || !snapshot.failure.empty())
        return Error(error,"invalid measurement cannot be serialized as a valid result/checkpoint");
    if(snapshot.status != RadiometricStatus::Running && snapshot.status != RadiometricStatus::Cancelled &&
       snapshot.status != RadiometricStatus::Complete)
        return Error(error,"unknown radiometric status");
    const auto count = static_cast<std::size_t>(snapshot.identity.width)*snapshot.identity.height;
    if(snapshot.pixels.size() != count) return Error(error,"radiometric pixel count mismatch");
    bool complete = true;
    for(const auto& px : snapshot.pixels) {
        if(px.count > snapshot.identity.targetSamples || !ValidatePixel(px,error))
            return Error(error,"invalid or unrepresentable radiometric pixel statistics");
        complete = complete && px.count == snapshot.identity.targetSamples;
    }
    if((snapshot.status == RadiometricStatus::Complete) != complete)
        return Error(error,"radiometric completion status disagrees with pixel counts");
    return true;
}
RadiometricFilm::RadiometricFilm(RadiometricSnapshot snapshot, RadiometricLimits limits_) : state(std::move(snapshot)), limits(limits_) {
    for(const auto& px:state.pixels) if(px.count==state.identity.targetSamples) ++completedPixels;
}
std::unique_ptr<RadiometricFilm> RadiometricFilm::Create(const RadiometricIdentity& id,
    std::string& error, const RadiometricLimits& limits) {
    RISECBOR64::Bytes identity;
    if(!EncodeRadiometricIdentity(id,identity,error,limits)) return nullptr;
    try {
        RadiometricSnapshot snapshot;
        snapshot.identity = id;
        snapshot.pixels.resize(CheckedPixelCount<RadiometricPixel>(id.width,id.height));
        return std::unique_ptr<RadiometricFilm>(new RadiometricFilm(std::move(snapshot),limits));
    } catch(const std::exception& ex) { error=ex.what(); return nullptr; }
}
std::unique_ptr<RadiometricFilm> RadiometricFilm::Restore(const RadiometricSnapshot& snapshot,
    std::string& error, const RadiometricLimits& limits) {
    if(!ValidateRadiometricSnapshot(snapshot,error,limits)) return nullptr;
    try { return std::unique_ptr<RadiometricFilm>(new RadiometricFilm(snapshot,limits)); }
    catch(const std::exception& ex) { error=ex.what(); return nullptr; }
}
bool RadiometricFilm::Fail(const std::string& reason, std::string& error) {
    state.status = RadiometricStatus::Invalid;
    if(state.failure.empty()) state.failure = reason;
    error = state.failure;
    return false;
}
bool RadiometricFilm::Commit(std::uint32_t x, std::uint32_t y, std::uint64_t sampleIndex,
    double value, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex);
    error.clear();
    // A lifecycle refusal does not destroy a previously complete/cancelled result.
    if(state.status != RadiometricStatus::Running) return Error(error,"radiometric film is not running");
    if(x >= state.identity.width || y >= state.identity.height) return Fail("radiometric pixel out of bounds",error);
    auto& px = state.pixels[static_cast<std::size_t>(y)*state.identity.width+x];
    if(sampleIndex != px.nextSampleIndex || px.count >= state.identity.targetSamples)
        return Fail("radiometric sample index mismatch or sample budget exhausted",error);
    if(!IsFiniteDouble(value) || value < 0) return Fail("nonfinite or negative radiometric trial",error);
    RadiometricPixel next=px;
    ++next.count;
    ++next.nextSampleIndex;
    if(!px.count) {
        next.mean=value;
        next.meanCorrection=0;
    } else {
        const MomentPair oldMean{px.mean,px.meanCorrection};
        const auto delta=Subtract({value,0},oldMean);
        const auto mean=Add(oldMean,Divide(delta,static_cast<double>(next.count)));
        MomentPair m2;
        if(!CanonicalNonnegative(mean) || ((!Zero(oldMean) || !IsZeroDouble(value)) && Zero(mean)) ||
           !AddSquaredDelta({px.m2,px.m2Correction},delta,px.count,next.count,m2))
            return Fail("radiometric compensated Welford state is not representable in float64",error);
        next.mean=mean.high; next.meanCorrection=mean.low;
        next.m2=m2.high; next.m2Correction=m2.low;
    }
    if(!ValidatePixel(next,error)) return Fail(error,error);
    px=next;
    if(next.count==state.identity.targetSamples) ++completedPixels;
    if(completedPixels==state.pixels.size()) state.status=RadiometricStatus::Complete;
    return true;
}
void RadiometricFilm::Cancel() {
    std::lock_guard<std::mutex> lock(mutex);
    if(state.status == RadiometricStatus::Running) state.status=RadiometricStatus::Cancelled;
}
bool RadiometricFilm::Resume(const RadiometricIdentity& expected, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex);
    RISECBOR64::Bytes a,b;
    if(!EncodeRadiometricIdentity(expected,a,error,limits) || !EncodeRadiometricIdentity(state.identity,b,error,limits)) return false;
    if(a != b) return Error(error,"radiometric resume identity mismatch");
    if(state.status != RadiometricStatus::Running && state.status != RadiometricStatus::Cancelled)
        return Error(error,"radiometric film cannot resume from this status");
    state.status=RadiometricStatus::Running;
    return true;
}
void RadiometricFilm::Invalidate(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex);
    std::string ignored;
    Fail(reason.empty() ? "unspecified radiometric measurement failure" : reason,ignored);
}
RadiometricSnapshot RadiometricFilm::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    return state;
}
} }
