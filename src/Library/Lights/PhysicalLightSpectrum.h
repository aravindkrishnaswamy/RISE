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
#include <initializer_list>

namespace RISE { class ILight; namespace Implementation {
class PhysicalLightSpectrum;
std::shared_ptr<const PhysicalLightSpectrum> GetPhysicalLightSpectrum(const ILight& light);
class PhysicalLightSpectrum final
{
    std::vector<double> wavelengths;
    std::vector<double> values;
    RISEPel visibleFraction;
    int visibleExponent[3] = {0,0,0};
    void EvalParts(Scalar nm, double& fraction, int& exponent) const;
    PhysicalLightSpectrum(const double* nm, const double* samples, unsigned int count);
public:
    struct ProductTerm {
        Scalar value;
        int exponent;
        ProductTerm(Scalar v=1,int e=0) : value(v),exponent(e) {}
    };
    static bool NormalizeDirection(const Vector3& authored,Vector3& unit);
    // Rejects zero, nonfinite or out-of-range finite-point distances. The
    // retained term keeps inverse-square arithmetic independent of d*d range.
    static bool DirectionBetween(const Point3& destination,const Point3& origin,
        Vector3& unit,ProductTerm& distance,Scalar& queryDistance);
    static std::shared_ptr<const PhysicalLightSpectrum> Create(
        const double* nm, const double* samples, unsigned int count, const char* outsidePolicy);
    Scalar Eval(Scalar nm) const;
    // Apply the physical intensity scale before rounding an interpolated value.
    Scalar EvalScaled(Scalar nm, Scalar scale) const;
    Scalar EvalProduct(Scalar nm,std::initializer_list<ProductTerm> numerators,
        std::initializer_list<ProductTerm> denominators={},
        std::initializer_list<ProductTerm> sourceFactors={}) const;
    RISEPel VisibleProduct(const RISEPel& response,std::initializer_list<ProductTerm> numerators,
        std::initializer_list<ProductTerm> denominators={},const RISEPel& channelFactors=RISEPel(1,1,1)) const;
    bool CanScale(Scalar scale, Scalar angularBound = 1) const;
    // Exponent-factored products used by actual RGB power consumers too.
    RISEPel ScaledVisibleRGB(Scalar scale, Scalar angularFactor = 1) const;
    const std::vector<double>& Wavelengths() const { return wavelengths; }
    const std::vector<double>& Values() const { return values; }
    // Unit-scale query may be outside double range; physical light
    // bookkeeping uses the admitted scale-applied ScaledVisibleRGB instead.
    RISEPel VisibleRGB() const { return ScaledVisibleRGB(1); }
};
} }
#endif
