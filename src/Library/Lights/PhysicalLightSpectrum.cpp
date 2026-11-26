#include "pch.h"
#include "PhysicalLightSpectrum.h"
#include "PointLight.h"
#include "SpotLight.h"
#include "DirectionalLight.h"
#include "../Painters/Painter.h"
#include "../Utilities/FiniteMath.h"
#include <algorithm>
#include <cstring>
#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;

namespace {
// Same exponent-factored formulation as HyLIoSOpticalData. A tiny wavelength
// ratio can underflow while its product with a spectral value is representable.
// Keep exponents separate until the complete product/quotient is evaluated.
double ScaledProductQuotient(const double value, const double numerator, const double denominator)
{
    if (value == 0 || numerator == 0) return 0;
    int ve = 0, ne = 0, de = 0;
    const double vm = std::frexp(value, &ve);
    const double nm = std::frexp(numerator, &ne);
    const double dm = std::frexp(denominator, &de);
    return std::ldexp((vm * nm) / dm, ve + ne - de);
}
// Keep each factor bounded until the final ldexp. This remains safe under
// full-LTO fast-math: no ordering of the mantissa products can overflow.
double ScaledProduct(const double a, const double b, const double c)
{
    if (a == 0 || b == 0 || c == 0) return 0;
    int ae = 0, be = 0, ce = 0;
    const double am = std::frexp(a,&ae);
    const double bm = std::frexp(b,&be);
    const double cm = std::frexp(c,&ce);
    return std::ldexp(am*bm*cm,ae+be+ce);
}

}

PhysicalLightSpectrum::PhysicalLightSpectrum(
    const double* nm, const double* samples, const unsigned int count)
    : wavelengths(nm, nm + count), values(samples, samples + count), visibleRGB(0,0,0)
{
    visibleRGB = ProjectPhysicalSpectrumToRGB(
        [](const void* ctx, const Scalar wavelength) {
            return static_cast<const PhysicalLightSpectrum*>(ctx)->Eval(wavelength);
        }, this, true);
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
    return RISEPel(ScaledProduct(visibleRGB.r,scale,angularFactor),
        ScaledProduct(visibleRGB.g,scale,angularFactor),
        ScaledProduct(visibleRGB.b,scale,angularFactor));
}

bool PhysicalLightSpectrum::CanScale(const Scalar scale, const Scalar angularBound) const
{
    if (!RISE::IsFiniteDouble(scale) || scale < 0 || !RISE::IsFiniteDouble(angularBound) || angularBound <= 0) return false;
    for (const double value : values) if (!RISE::IsFiniteDouble(ScaledProduct(value,scale,1))) return false;
    const RISEPel scaled = ScaledVisibleRGB(scale,angularBound);
    return RISE::IsFiniteDouble(scaled.r) && RISE::IsFiniteDouble(scaled.g) && RISE::IsFiniteDouble(scaled.b);
}

Scalar PhysicalLightSpectrum::Eval(const Scalar nm) const
{
    if (!RISE::IsFiniteDouble(nm) || nm < wavelengths.front() || nm > wavelengths.back()) return 0;
    const auto upper = std::lower_bound(wavelengths.begin(), wavelengths.end(), double(nm));
    const std::size_t i = static_cast<std::size_t>(upper - wavelengths.begin());
    if (*upper == nm || i == 0) return values[i];
    const double left = nm - wavelengths[i-1];
    const double right = wavelengths[i] - nm;
    const double span = wavelengths[i] - wavelengths[i-1];
    // The nearer endpoint avoids cancellation; a constant subnormal source
    // remains exact instead of underflowing two independently weighted terms.
    return left <= right
        ? values[i-1] + ScaledProductQuotient(values[i] - values[i-1], left, span)
        : values[i] + ScaledProductQuotient(values[i-1] - values[i], right, span);
}

std::shared_ptr<const PhysicalLightSpectrum> RISE::Implementation::GetPhysicalLightSpectrum(const ILight& light)
{
    if (const auto* point = dynamic_cast<const PointLight*>(&light)) return point->GetPhysicalSpectrum();
    if (const auto* spot = dynamic_cast<const SpotLight*>(&light)) return spot->GetPhysicalSpectrum();
    if (const auto* directional = dynamic_cast<const DirectionalLight*>(&light)) return directional->GetPhysicalSpectrum();
    return {};
}
