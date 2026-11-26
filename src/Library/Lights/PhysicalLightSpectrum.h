// Checked immutable spectra for physical delta-light authoring.
// Samples are spectral radiant intensity W/(sr nm) for point/spot sources,
// spectral irradiance W/(m^2 nm) for directional sources. Scalar light energy
// is a dimensionless multiplier. Zero outside the authored interval is an
// explicit model policy, not evidence of measured coverage.
#ifndef RISE_PHYSICAL_LIGHT_SPECTRUM_H
#define RISE_PHYSICAL_LIGHT_SPECTRUM_H

#include "../Utilities/Color/Color.h"
#include <memory>
#include <vector>

namespace RISE { class ILight; namespace Implementation {
class PhysicalLightSpectrum;
std::shared_ptr<const PhysicalLightSpectrum> GetPhysicalLightSpectrum(const ILight& light);
class PhysicalLightSpectrum final
{
    std::vector<double> wavelengths;
    std::vector<double> values;
    RISEPel visibleRGB;
    RISEPel visibleFraction;
    int visibleExponent[3] = {0,0,0};
    void EvalParts(Scalar nm, double& fraction, int& exponent) const;
    PhysicalLightSpectrum(const double* nm, const double* samples, unsigned int count);
public:
    static std::shared_ptr<const PhysicalLightSpectrum> Create(
        const double* nm, const double* samples, unsigned int count, const char* outsidePolicy);
    Scalar Eval(Scalar nm) const;
    // Apply the physical intensity scale before rounding an interpolated value.
    Scalar EvalScaled(Scalar nm, Scalar scale) const;
    bool CanScale(Scalar scale, Scalar angularBound = 1) const;
    // Exponent-factored products used by actual RGB power consumers too.
    RISEPel ScaledVisibleRGB(Scalar scale, Scalar angularFactor = 1) const;
    const std::vector<double>& Wavelengths() const { return wavelengths; }
    const std::vector<double>& Values() const { return values; }
    const RISEPel& VisibleRGB() const { return visibleRGB; }
};
} }
#endif
