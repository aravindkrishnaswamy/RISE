#include "pch.h"
#include "PhysicalLightSpectrum.h"
#include "PointLight.h"
#include "SpotLight.h"
#include "DirectionalLight.h"
#include "../Utilities/Color/ColorUtils.h"
#include <array>
#include "../Utilities/FiniteMath.h"
#include <algorithm>
#include <cstring>
#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;

namespace {
// Nonnegative products and interpolation terms retain their exponent until all
// source factors have been applied. No subnormal intermediate or overflow-prone
// large/small factor ordering is introduced by strict or full-LTO fast-math.
struct Parts { double fraction=0; int exponent=0; };
Parts Normalize(const double fraction, const int exponent)
{
    if (fraction==0) return {};
    int shift=0;
    const double normalized=std::frexp(fraction,&shift);
    return {normalized,exponent+shift};
}
Parts Decompose(const double value) { return Normalize(value,0); }
Parts Multiply(const Parts value, const double factor)
{
    if (value.fraction==0 || factor==0) return {};
    const Parts f=Decompose(factor);
    return Normalize(value.fraction*f.fraction,value.exponent+f.exponent);
}
Parts Weighted(const double value, const double numerator, const double denominator)
{
    if (value==0 || numerator==0) return {};
    const Parts v=Decompose(value),n=Decompose(numerator),d=Decompose(denominator);
    return Normalize((v.fraction*n.fraction)/d.fraction,v.exponent+n.exponent-d.exponent);
}
Parts Add(const Parts a, const Parts b)
{
    if (a.fraction==0) return b;
    if (b.fraction==0) return a;
    const int exponent=std::max(a.exponent,b.exponent);
    return Normalize(std::ldexp(a.fraction,a.exponent-exponent)+
        std::ldexp(b.fraction,b.exponent-exponent),exponent);
}
double Resolve(const Parts value) { return std::ldexp(value.fraction,value.exponent); }
}

PhysicalLightSpectrum::PhysicalLightSpectrum(
    const double* nm, const double* samples, const unsigned int count)
    : wavelengths(nm,nm+count),values(samples,samples+count),visibleRGB(0,0,0),visibleFraction(0,0,0)
{
    // Match the existing visible source projection's 1nm CIE grid and Y
    // normalization. Normalize the projected VISIBLE terms, not a global SPD
    // maximum: an unrelated huge SWIR value must not erase faint visible power.
    std::array<std::array<Parts,3>,401> terms{};
    double ySum=0;
    int commonExponent=0;
    bool any=false;
    for(int wavelength=380;wavelength<=780;++wavelength) {
        XYZPel cmf(0,0,0);
        if (!ColorUtils::XYZFromNM(cmf,Scalar(wavelength))) continue;
        ySum+=cmf.Y;
        Parts value;
        EvalParts(wavelength,value.fraction,value.exponent);
        auto& row=terms[static_cast<std::size_t>(wavelength-380)];
        row={Multiply(value,cmf.X),Multiply(value,cmf.Y),Multiply(value,cmf.Z)};
        for(const Parts term:row) if(term.fraction!=0) {
            commonExponent=any ? std::max(commonExponent,term.exponent):term.exponent;
            any=true;
        }
    }
    if(!any || !(ySum>0)) return;
    XYZPel sum(0,0,0);
    for(const auto& row:terms) {
        sum.X+=std::ldexp(row[0].fraction,row[0].exponent-commonExponent);
        sum.Y+=std::ldexp(row[1].fraction,row[1].exponent-commonExponent);
        sum.Z+=std::ldexp(row[2].fraction,row[2].exponent-commonExponent);
    }
    const RISEPel normalized=ColorUtils::XYZtoRec709RGB(sum*Scalar(1/ySum));
    for(unsigned int channel=0;channel<3;++channel) {
        const Parts projected=Normalize(normalized[channel],commonExponent);
        visibleFraction[channel]=projected.fraction;
        visibleExponent[channel]=projected.exponent;
    }
    visibleRGB=ScaledVisibleRGB(1);
}

std::shared_ptr<const PhysicalLightSpectrum> PhysicalLightSpectrum::Create(
    const double* nm, const double* samples, const unsigned int count, const char* outsidePolicy)
{
    if (!nm || !samples || count < 2 || !outsidePolicy || std::strcmp(outsidePolicy, "zero") != 0) return {};
    for (unsigned int i = 0; i < count; ++i) {
        if (!RISE::IsFiniteDouble(nm[i]) || !RISE::IsFiniteDouble(samples[i]) ||
            nm[i] <= 0 || samples[i] < 0 || (i && nm[i] <= nm[i-1])) return {};
    }
    auto spectrum = std::shared_ptr<const PhysicalLightSpectrum>(new PhysicalLightSpectrum(nm, samples, count));
    const RISEPel& rgb = spectrum->VisibleRGB();
    if (!RISE::IsFiniteDouble(rgb.r) || !RISE::IsFiniteDouble(rgb.g) || !RISE::IsFiniteDouble(rgb.b)) return {};
    return spectrum;
}

RISEPel PhysicalLightSpectrum::ScaledVisibleRGB(const Scalar scale, const Scalar angularFactor) const
{
    RISEPel result(0,0,0);
    for(unsigned int channel=0;channel<3;++channel) {
        const Parts projected{visibleFraction[channel],visibleExponent[channel]};
        result[channel]=Resolve(Multiply(Multiply(projected,scale),angularFactor));
    }
    return result;
}

bool PhysicalLightSpectrum::CanScale(const Scalar scale, const Scalar angularBound) const
{
    if (!RISE::IsFiniteDouble(scale) || scale<0 || !RISE::IsFiniteDouble(angularBound) || angularBound<=0) return false;
    for(const double value:values) if(!RISE::IsFiniteDouble(Resolve(Multiply(Decompose(value),scale)))) return false;
    const RISEPel scaled=ScaledVisibleRGB(scale,angularBound);
    return RISE::IsFiniteDouble(scaled.r) && RISE::IsFiniteDouble(scaled.g) && RISE::IsFiniteDouble(scaled.b);
}

void PhysicalLightSpectrum::EvalParts(const Scalar nm, double& fraction, int& exponent) const
{
    Parts result;
    if(RISE::IsFiniteDouble(nm) && nm>=wavelengths.front() && nm<=wavelengths.back()) {
        const auto upper=std::lower_bound(wavelengths.begin(),wavelengths.end(),double(nm));
        const std::size_t i=static_cast<std::size_t>(upper-wavelengths.begin());
        if(*upper==nm || i==0) result=Decompose(values[i]);
        else {
            const double span=wavelengths[i]-wavelengths[i-1];
            // Add nonnegative weighted terms in a common exponent frame before
            // final rounding. A subnormal ramp can be recovered by light scale.
            result=Add(Weighted(values[i-1],wavelengths[i]-nm,span),
                Weighted(values[i],nm-wavelengths[i-1],span));
        }
    }
    fraction=result.fraction;exponent=result.exponent;
}

Scalar PhysicalLightSpectrum::EvalScaled(const Scalar nm, const Scalar scale) const
{
    Parts value;
    EvalParts(nm,value.fraction,value.exponent);
    return Resolve(Multiply(value,scale));
}

Scalar PhysicalLightSpectrum::Eval(const Scalar nm) const { return EvalScaled(nm,1); }

std::shared_ptr<const PhysicalLightSpectrum> RISE::Implementation::GetPhysicalLightSpectrum(const ILight& light)
{
    if (const auto* point = dynamic_cast<const PointLight*>(&light)) return point->GetPhysicalSpectrum();
    if (const auto* spot = dynamic_cast<const SpotLight*>(&light)) return spot->GetPhysicalSpectrum();
    if (const auto* directional = dynamic_cast<const DirectionalLight*>(&light)) return directional->GetPhysicalSpectrum();
    return {};
}
