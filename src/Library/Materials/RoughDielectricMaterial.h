// Anisotropic GGX dielectric with reciprocal multiscatter compensation. Bulk absorption belongs
// to a separately assigned medium. Positive widths only; use dielectric_material
// for a smooth interface. RGB uses a single physical IOR; NM/HWSS retain dispersion.
#ifndef RISE_ROUGH_DIELECTRIC_MATERIAL_H
#define RISE_ROUGH_DIELECTRIC_MATERIAL_H

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/ISPF.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE { namespace Implementation {

class RoughDielectricSurface : public virtual IBSDF, public virtual ISPF, public virtual Reference
{
    const IScalarPainter* pIOR;
    const IScalarPainter* pAlphaX;
    const IScalarPainter* pAlphaY;
protected:
    ~RoughDielectricSurface() override;
public:
    RoughDielectricSurface(const IScalarPainter& ior, const IScalarPainter& ax, const IScalarPainter& ay);
    const IScalarPainter& GetIOR() const { return *pIOR; }
    const IScalarPainter& GetAlphaX() const { return *pAlphaX; }
    const IScalarPainter& GetAlphaY() const { return *pAlphaY; }
    void SetIOR(const IScalarPainter& v);
    void SetAlphaX(const IScalarPainter& v);
    void SetAlphaY(const IScalarPainter& v);

    RISEPel value(const Vector3& wo, const RayIntersectionGeometric& ri) const override;
    Scalar valueNM(const Vector3& wo, const RayIntersectionGeometric& ri, Scalar nm) const override;
    RISEPel valueStateful(const Vector3& wo, const RayIntersectionGeometric& ri, const IORStack* stack) const override;
    Scalar valueStatefulNM(const Vector3& wo, const RayIntersectionGeometric& ri, Scalar nm, const IORStack* stack) const override;
    bool valueByScatterType(const Vector3& wo, const RayIntersectionGeometric& ri, const IORStack* stack, RISEPel out[5]) const override;
    bool valueByScatterTypeNM(const Vector3& wo, const RayIntersectionGeometric& ri, Scalar nm, const IORStack* stack, Scalar out[5]) const override;
    void Scatter(const RayIntersectionGeometric& ri, ISampler& sampler, ScatteredRayContainer& rays, const IORStack& stack) const override;
    void ScatterNM(const RayIntersectionGeometric& ri, ISampler& sampler, Scalar nm, ScatteredRayContainer& rays, const IORStack& stack) const override;
    Scalar Pdf(const RayIntersectionGeometric& ri, const Vector3& wo, const IORStack& stack) const override;
    Scalar PdfNM(const RayIntersectionGeometric& ri, const Vector3& wo, Scalar nm, const IORStack& stack) const override;
    SpecularInfo GetSpecularInfo(const RayIntersectionGeometric& ri, const IORStack& stack) const override;
    SpecularInfo GetSpecularInfoNM(const RayIntersectionGeometric& ri, const IORStack& stack, Scalar nm) const override;
    Scalar EvaluateLobeFNM(const RayIntersectionGeometric& ri, const Vector3& wo, ScatteredRay::ScatRayType type, Scalar nm, const IORStack& stack) const override;
    Scalar EvaluateKrayNM(const RayIntersectionGeometric& ri, const Vector3& wo, ScatteredRay::ScatRayType type, Scalar nm, const IORStack& stack) const override;
    Scalar EvaluateKrayNM(const RayIntersectionGeometric& ri, const Vector3& wo, ScatteredRay::ScatRayType type, Scalar nm, const IORStack& stack, Scalar pdfHero) const override;
};

class RoughDielectricMaterial : public virtual IMaterial, public virtual Reference
{
    RoughDielectricSurface* surface;
protected:
    ~RoughDielectricMaterial() override { safe_release(surface); }
public:
    RoughDielectricMaterial(const IScalarPainter& ior, const IScalarPainter& ax, const IScalarPainter& ay);
    IBSDF* GetBSDF() const override { return surface; }
    ISPF* GetSPF() const override { return surface; }
    IEmitter* GetEmitter() const override { return nullptr; }
    bool CouldLightPassThrough() const override { return true; }
    bool ScattersFullSphere() const override { return true; }
    unsigned int ConnectionScatterTypes() const override { return (1u << ScatteredRay::eRayReflection) | (1u << ScatteredRay::eRayRefraction); }
    bool HasConnectionTypeSplit() const override { return true; }
    SpecularInfo GetSpecularInfo(const RayIntersectionGeometric& ri, const IORStack& stack) const override { return surface->GetSpecularInfo(ri, stack); }
    SpecularInfo GetSpecularInfoNM(const RayIntersectionGeometric& ri, const IORStack& stack, Scalar nm) const override { return surface->GetSpecularInfoNM(ri, stack, nm); }
    const IScalarPainter& GetIOR() const { return surface->GetIOR(); }
    const IScalarPainter& GetAlphaX() const { return surface->GetAlphaX(); }
    const IScalarPainter& GetAlphaY() const { return surface->GetAlphaY(); }
    void SetIOR(const IScalarPainter& v) { surface->SetIOR(v); }
    void SetAlphaX(const IScalarPainter& v) { surface->SetAlphaX(v); }
    void SetAlphaY(const IScalarPainter& v) { surface->SetAlphaY(v); }
};

// Current HWSS walkers carry one IOR stack and one radiance eta factor.
// Until they carry wavelength-specific media, a rough interface conservatively
// ends companion transport, including when its surrounding medium disperses.
bool RoughDielectricRequiresHeroOnlySPF(const ISPF* spf);
inline bool RoughDielectricRequiresHeroOnlyHWSS(const IMaterial* material)
{ return material && RoughDielectricRequiresHeroOnlySPF(material->GetSPF()); }
} }
#endif
