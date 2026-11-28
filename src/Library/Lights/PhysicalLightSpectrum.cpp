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
Parts ApplyProduct(Parts value,std::initializer_list<PhysicalLightSpectrum::ProductTerm> numerators,
    std::initializer_list<PhysicalLightSpectrum::ProductTerm> denominators)
{
    for(const auto term:numerators) {
        value=Multiply(value,term.value);
        value.exponent+=term.exponent;
    }
    for(const auto term:denominators) {
        const Parts d=Decompose(term.value);
        value=Normalize(value.fraction/d.fraction,value.exponent-d.exponent-term.exponent);
    }
    return value;
}
bool UnitAndMagnitude(const Vector3& authored,Vector3& unit,Parts& magnitude)
{
    if(!RISE::IsFiniteDouble(authored.x)||!RISE::IsFiniteDouble(authored.y)||!RISE::IsFiniteDouble(authored.z)) return false;
    const double largest=std::max({std::fabs(authored.x),std::fabs(authored.y),std::fabs(authored.z)});
    if(!(largest>0)) return false;
    // Scaling by an exponent never forms 1/largest (which fast-math may
    // introduce for repeated division by a subnormal maximum).
    int exponent=0;
    std::frexp(largest,&exponent);
    const Vector3 scaled(std::ldexp(authored.x,-exponent),std::ldexp(authored.y,-exponent),std::ldexp(authored.z,-exponent));
    const double length=std::sqrt(Vector3Ops::Dot(scaled,scaled));
    unit=scaled*(1/length);
    magnitude=Normalize(length,exponent);
    return true;
}

double Resolve(const Parts value) { return std::ldexp(value.fraction,value.exponent); }
}

PhysicalLightSpectrum::PhysicalLightSpectrum(
    const double* nm, const double* samples, const unsigned int count)
    : wavelengths(nm,nm+count),values(samples,samples+count),visibleFraction(0,0,0)
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
    return spectrum;
}

bool PhysicalLightSpectrum::NormalizeDirection(const Vector3& authored,Vector3& unit)
{
    Parts magnitude;
    return UnitAndMagnitude(authored,unit,magnitude);
}

bool PhysicalLightSpectrum::DirectionBetween(const Point3& destination,const Point3& origin,
    Vector3& unit,ProductTerm& distance,Scalar& queryDistance)
{
    if(!RISE::IsFiniteDouble(destination.x)||!RISE::IsFiniteDouble(destination.y)||!RISE::IsFiniteDouble(destination.z)||
        !RISE::IsFiniteDouble(origin.x)||!RISE::IsFiniteDouble(origin.y)||!RISE::IsFiniteDouble(origin.z)) return false;
    Parts magnitude;
    if(!UnitAndMagnitude(Vector3Ops::mkVector3(destination,origin),unit,magnitude)) return false;
    queryDistance=Resolve(magnitude);
    if(!RISE::IsFiniteDouble(queryDistance)||!(queryDistance>0)) return false;
    distance=ProductTerm(magnitude.fraction,magnitude.exponent);
    return true;
}

RISEPel PhysicalLightSpectrum::VisibleProduct(const RISEPel& response,
    std::initializer_list<ProductTerm> numerators,std::initializer_list<ProductTerm> denominators,const RISEPel& channelFactors) const
{
    RISEPel result(0,0,0);
    for(unsigned int channel=0;channel<3;++channel) {
        const Parts projected{visibleFraction[channel],visibleExponent[channel]};
        result[channel]=Resolve(ApplyProduct(Multiply(Multiply(projected,response[channel]),channelFactors[channel]),numerators,denominators));
    }
    return result;
}

Scalar PhysicalLightSpectrum::EvalProduct(const Scalar nm,
    std::initializer_list<ProductTerm> numerators,std::initializer_list<ProductTerm> denominators,
    std::initializer_list<ProductTerm> sourceFactors) const
{
    Parts value;
    EvalParts(nm,value.fraction,value.exponent);
    return Resolve(ApplyProduct(ApplyProduct(value,sourceFactors,{}),numerators,denominators));
}

RISEPel PhysicalLightSpectrum::ScaledVisibleRGB(const Scalar scale,const Scalar angularFactor) const
{
    return VisibleProduct(RISEPel(1,1,1),{scale,angularFactor});
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

Scalar PhysicalLightSpectrum::EvalScaled(const Scalar nm,const Scalar scale) const
{
    return EvalProduct(nm,{scale});
}

Scalar PhysicalLightSpectrum::Eval(const Scalar nm) const { return EvalScaled(nm,1); }

std::shared_ptr<const PhysicalLightSpectrum> RISE::Implementation::GetPhysicalLightSpectrum(const ILight& light)
{
    if (const auto* point = dynamic_cast<const PointLight*>(&light)) return point->GetPhysicalSpectrum();
    if (const auto* spot = dynamic_cast<const SpotLight*>(&light)) return spot->GetPhysicalSpectrum();
    if (const auto* directional = dynamic_cast<const DirectionalLight*>(&light)) return directional->GetPhysicalSpectrum();
    return {};
}
