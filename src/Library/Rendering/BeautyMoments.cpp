#include "pch.h"
#include "BeautyMoments.h"
#include "../Utilities/FiniteMath.h"
#include <algorithm>
#include <cmath>
#include <limits>

#ifdef __FAST_MATH__
#error BeautyMoments.cpp requires strict floating-point arithmetic
#endif

namespace RISE { namespace Implementation {
namespace {
struct Pair { double high, low; };
bool Finite(Pair x) { return IsFiniteDouble(x.high) && IsFiniteDouble(x.low); }
bool Zero(Pair x) { return IsZeroDouble(x.high) && IsZeroDouble(x.low); }
// Materialize error-free-transform nodes as binary64 even under full LTO.
double Sum(double a, double b) { volatile double r = a+b; return r; }
double Difference(double a, double b) { volatile double r = a-b; return r; }
double Product(double a, double b) { volatile double r = a*b; return r; }
double Quotient(double a, double b) { volatile double r = a/b; return r; }
Pair TwoSum(double a, double b)
{
    const double s = Sum(a,b);
    if(!IsFiniteDouble(s)) return {s,0};
    const double bv = Difference(s,a);
    return {s, Sum(Difference(a,Difference(s,bv)), Difference(b,bv))};
}
Pair AddPair(Pair a, Pair b)
{
    auto hi = TwoSum(a.high,b.high);
    if(!Finite(hi)) return hi;
    const auto lo = TwoSum(a.low,b.low);
    hi = TwoSum(hi.high,Sum(hi.low,lo.high));
    return TwoSum(hi.high,Sum(hi.low,lo.low));
}
Pair Divide(Pair a, double n)
{
    const double q = Quotient(a.high,n);
    if(!IsFiniteDouble(q)) return {q,0};
    return TwoSum(q,Quotient(Sum(std::fma(-q,n,a.high),a.low),n));
}
Pair Multiply(Pair a, Pair b)
{
    const double p = Product(a.high,b.high);
    double e = std::fma(a.high,b.high,-p);
    e = Sum(e,Product(a.high,b.low));
    e = Sum(e,Product(a.low,b.high));
    e = Sum(e,Product(a.low,b.low));
    return TwoSum(p,e);
}
Pair Scale(Pair a, int exponent)
{
    return TwoSum(std::scalbn(a.high,exponent),std::scalbn(a.low,exponent));
}
bool Nonnegative(Pair x)
{
    return Finite(x) && x.high >= 0 && !(IsZeroDouble(x.high) && x.low < 0) &&
        !(x.high == std::numeric_limits<double>::max() && x.low > 0);
}
bool SquaredDelta(Pair prior, Pair delta, std::uint32_t n, Pair& result)
{
    if(!Finite(delta)) return false;
    if(Zero(delta)) { result = prior; return true; }
    int exponent = 0;
    std::frexp(std::fabs(delta.high),&exponent);
    const auto scaled = Scale(delta,-exponent);
    const auto square = Multiply(scaled,scaled);
    const auto ratio = Divide({double(n),0},double(n)+1.0);
    const auto coefficient = Multiply(square,ratio);
    if(!Nonnegative(coefficient) || Zero(coefficient)) return false;
    int coefficientExponent = 0, priorExponent = 0;
    std::frexp(coefficient.high,&coefficientExponent);
    if(!Zero(prior)) std::frexp(prior.high,&priorExponent);
    const int commonExponent = Zero(prior) ? 2*exponent+coefficientExponent :
        std::max(2*exponent+coefficientExponent,priorExponent);
    // Add at a common scale first: an individually tiny increment can still
    // change a representable total. Restore physical units only afterward.
    const auto combined = AddPair(Scale(prior,-commonExponent),
        Scale(coefficient,2*exponent-commonExponent));
    result = Scale(combined,commonExponent);
    return Nonnegative(result) && !Zero(result);
}
}

void BeautyMoments::Add(double luminance)
{
    if(!valid) return;
    if(!IsFiniteDouble(luminance) || count == std::numeric_limits<std::uint32_t>::max()) {
        valid = false;
        return;
    }
    const std::uint32_t nextCount = count+1;
    if(!count) {
        mean = luminance;
        count = nextCount;
        return;
    }
    const auto delta = AddPair({luminance,0},{-mean,-meanCorrection});
    const auto nextMean = AddPair({mean,meanCorrection},Divide(delta,double(nextCount)));
    Pair nextM2;
    if(!Finite(nextMean) || !SquaredDelta({m2,m2Correction},delta,count,nextM2)) {
        valid = false;
        return;
    }
    mean = nextMean.high; meanCorrection = nextMean.low;
    m2 = nextM2.high; m2Correction = nextM2.low;
    count = nextCount;
}

bool BeautyMoments::StandardError(double& result) const
{
    const Pair moment{m2,m2Correction};
    if(!valid || count < 2 || !Finite({mean,meanCorrection}) || !Nonnegative(moment)) return false;
    if(Zero(moment)) { result = 0; return true; }
    // Taking the square root before dividing avoids underflow of variance/n.
    // Use the compensated M2 in a bounded relative correction.
    const double root = std::sqrt(m2);
    const double correction = 1.0 + 0.5*Quotient(m2Correction,m2);
    result = Quotient(Product(root,correction),Product(std::sqrt(double(count)),std::sqrt(double(count-1))));
    return IsFiniteDouble(result) && result > 0;
}

bool BeautyMoments::IsConverged(double threshold, double nearZero) const
{
    double error = 0;
    // Validate before the historical near-black shortcut as well.
    if(count < 32 || !StandardError(error)) return false;
    const double magnitude = std::fabs(Sum(mean,meanCorrection));
    if(magnitude > nearZero) {
        const double confidence = 1.0 - 4.0/double(count);
        return error/magnitude < threshold*confidence;
    }
    return count >= 64 && Sum(m2,m2Correction) < nearZero;
}
} }
