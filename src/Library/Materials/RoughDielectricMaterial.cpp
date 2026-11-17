#include "pch.h"
#include "RoughDielectricMaterial.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/MicrofacetUtils.h"
#include "../Utilities/IORStackSeeding.h"

using namespace RISE;
using namespace RISE::Implementation;
namespace RD = MicrofacetUtils::RoughDielectric;

namespace {
Scalar Read(const IScalarPainter& p, const RayIntersectionGeometric& ri, Scalar nm)
{
    return nm > 0 ? p.GetValueAtNM(ri, nm) : p.GetValuesAt(ri).v[0];
}

// One crossing description is shared by value, density and sampling. In
// particular, an exit reads the enclosing index below the current object.
struct Crossing
{
    OrthonormalBasis3D frame;
    Vector3 wi, geom;
    Scalar index, eta, ax, ay;
    bool inside, valid;
    std::unique_ptr<IORStack> sheet;
    Scalar sheetFrom;

    Crossing(const RoughDielectricSurface& s, const RayIntersectionGeometric& ri,
        const IORStack* stack, Scalar nm) : frame(ri.onb), index(Read(s.GetIOR(), ri, nm)),
        eta(0), ax(Read(s.GetAlphaX(), ri, nm)), ay(Read(s.GetAlphaY(), ri, nm)),
        inside(false), valid(false), sheetFrom(1)
    {
        if(Vector3Ops::Dot(frame.w(), ri.ray.Dir()) > 0) frame.FlipW();
        const Vector3 incident = Vector3Ops::Normalize(-ri.ray.Dir());
        wi = Local(incident);
        geom = ri.HasTrueGeomSide() ? ri.UnflippedGeomNormal() : ri.vGeomNormal;
        if(Vector3Ops::SquaredModulus(geom) == 0) geom = frame.w();
        const bool back = Vector3Ops::Dot(geom, ri.ray.Dir()) > 0;
        if(Vector3Ops::Dot(geom, incident) < 0) geom = -geom;
        inside = ri.bProvablyNoInterior ? IORStackSeeding::OpenSheetBackFace(ri)
            : (stack ? stack->containsCurrent() : back);
        Scalar ni = inside ? index : (stack ? stack->top() : Scalar(1));
        Scalar nt = inside ? Scalar(1) : index;
        if(stack && ri.bProvablyNoInterior) {
            sheet.reset(new IORStack(*stack));
            const auto crossing = IORStackSeeding::ResolveOpenSheetCrossing(ri, index, *sheet, 0, nm);
            sheetFrom = crossing.etaFrom;
            ni = sheetFrom;
            nt = crossing.etaTo;
        } else if(inside && stack) {
            IORStack enclosing(*stack);
            enclosing.pop();
            nt = enclosing.top();
        }
        eta = nt / ni;
        valid = std::isfinite(index) && index > 0 && std::isfinite(ni) && ni > 0 &&
            std::isfinite(nt) && nt > 0 && std::isfinite(eta) && eta > 0 &&
            ax > 0 && ax <= 1 && ay > 0 && ay <= 1 && wi.z > 0;
    }
    Vector3 Local(const Vector3& v) const { return Vector3(Vector3Ops::Dot(v,frame.u()), Vector3Ops::Dot(v,frame.v()), Vector3Ops::Dot(v,frame.w())); }
    Vector3 World(const Vector3& v) const { return frame.u()*v.x + frame.v()*v.y + frame.w()*v.z; }
    bool Allowed(const Vector3& wo) const {
        // Clipped micronormal events remain null events; never renormalize the PDF.
        const Scalar z = Vector3Ops::Dot(wo,frame.w()), g = Vector3Ops::Dot(wo,geom);
        return valid && ((z > 0 && g > 0) || (z < 0 && g < 0));
    }
    ScatteredRay::ScatRayType Type(const Vector3& wo) const {
        return Vector3Ops::Dot(wo,frame.w()) > 0 ? ScatteredRay::eRayReflection : ScatteredRay::eRayRefraction;
    }
    RD::Evaluation Eval(const Vector3& wo, bool radiance) const {
        return Allowed(wo) ? RD::Evaluate(wi, Local(Vector3Ops::Normalize(wo)), ax, ay, eta, radiance) : RD::Evaluation();
    }
};

void DrawRay(const RoughDielectricSurface& surface, const RayIntersectionGeometric& ri,
    ISampler& sampler, Scalar nm, ScatteredRayContainer& rays, const IORStack& stack)
{
    const Crossing c(surface, ri, &stack, nm);
    if(!c.valid) return;
    const Scalar u = sampler.Get1D(), v = sampler.Get1D(), lobe = sampler.Get1D();
    // Integrators, including HWSS, apply their radiance eta scaling separately.
    const RD::Sample draw = RD::Draw(c.wi, c.ax, c.ay, c.eta, u, v, lobe, false);
    // The equal-index delta is exactly straight, even after a rotated frame.
    const Vector3 wo = draw.delta ? Vector3Ops::Normalize(ri.ray.Dir()) : c.World(draw.wo);
    if(!draw.valid || !c.Allowed(wo)) return;
    ScatteredRay ray;
    ray.ray = Ray(ri.ptIntersection, wo);
    ray.type = draw.transmission ? ScatteredRay::eRayRefraction : ScatteredRay::eRayReflection;
    ray.isDelta = draw.delta;
    ray.pdf = draw.delta ? 0 : draw.pdf;
    ray.kray = RISEPel(draw.weight, draw.weight, draw.weight);
    ray.krayNM = draw.weight;
    if(draw.transmission) {
        ray.ior_stack = IORStackSeeding::NewTransmittedStack(stack, c.sheet.get(), c.sheetFrom, !c.inside, c.index);
    }
    rays.AddScatteredRay(ray);
}
}

RoughDielectricSurface::RoughDielectricSurface(const IScalarPainter& ior, const IScalarPainter& ax, const IScalarPainter& ay)
    : pIOR(&ior), pAlphaX(&ax), pAlphaY(&ay)
{ pIOR->addref(); pAlphaX->addref(); pAlphaY->addref(); }
RoughDielectricSurface::~RoughDielectricSurface()
{ safe_release(pIOR); safe_release(pAlphaX); safe_release(pAlphaY); }
void RoughDielectricSurface::SetIOR(const IScalarPainter& v) { v.addref(); safe_release(pIOR); pIOR=&v; }
void RoughDielectricSurface::SetAlphaX(const IScalarPainter& v) { v.addref(); safe_release(pAlphaX); pAlphaX=&v; }
void RoughDielectricSurface::SetAlphaY(const IScalarPainter& v) { v.addref(); safe_release(pAlphaY); pAlphaY=&v; }
RoughDielectricMaterial::RoughDielectricMaterial(const IScalarPainter& ior, const IScalarPainter& ax, const IScalarPainter& ay)
    : surface(new RoughDielectricSurface(ior,ax,ay))
{ GlobalLog()->PrintNew(surface,__FILE__,__LINE__,"rough dielectric surface"); }

RISEPel RoughDielectricSurface::value(const Vector3& wo, const RayIntersectionGeometric& ri) const
{ return valueStateful(wo,ri,nullptr); }
Scalar RoughDielectricSurface::valueNM(const Vector3& wo, const RayIntersectionGeometric& ri, Scalar nm) const
{ return valueStatefulNM(wo,ri,nm,nullptr); }
RISEPel RoughDielectricSurface::valueStateful(const Vector3& wo, const RayIntersectionGeometric& ri, const IORStack* stack) const
{ const Scalar f=Crossing(*this,ri,stack,0).Eval(wo,!BSDFImportanceModeFlag()).f; return RISEPel(f,f,f); }
Scalar RoughDielectricSurface::valueStatefulNM(const Vector3& wo, const RayIntersectionGeometric& ri, Scalar nm, const IORStack* stack) const
{ return Crossing(*this,ri,stack,nm).Eval(wo,!BSDFImportanceModeFlag()).f; }
bool RoughDielectricSurface::valueByScatterType(const Vector3& wo, const RayIntersectionGeometric& ri, const IORStack* stack, RISEPel out[5]) const
{ for(int i=0;i<5;++i) out[i]=RISEPel(0,0,0); out[Crossing(*this,ri,stack,0).Type(wo)]=valueStateful(wo,ri,stack); return true; }
bool RoughDielectricSurface::valueByScatterTypeNM(const Vector3& wo, const RayIntersectionGeometric& ri, Scalar nm, const IORStack* stack, Scalar out[5]) const
{ for(int i=0;i<5;++i) out[i]=0; out[Crossing(*this,ri,stack,nm).Type(wo)]=valueStatefulNM(wo,ri,nm,stack); return true; }
void RoughDielectricSurface::Scatter(const RayIntersectionGeometric& ri, ISampler& sampler, ScatteredRayContainer& rays, const IORStack& stack) const
{ DrawRay(*this,ri,sampler,0,rays,stack); }
void RoughDielectricSurface::ScatterNM(const RayIntersectionGeometric& ri, ISampler& sampler, Scalar nm, ScatteredRayContainer& rays, const IORStack& stack) const
{ DrawRay(*this,ri,sampler,nm,rays,stack); }
Scalar RoughDielectricSurface::Pdf(const RayIntersectionGeometric& ri, const Vector3& wo, const IORStack& stack) const
{ return Crossing(*this,ri,&stack,0).Eval(wo,false).pdf; }
Scalar RoughDielectricSurface::PdfNM(const RayIntersectionGeometric& ri, const Vector3& wo, Scalar nm, const IORStack& stack) const
{ return Crossing(*this,ri,&stack,nm).Eval(wo,false).pdf; }
SpecularInfo RoughDielectricSurface::GetSpecularInfo(const RayIntersectionGeometric& ri, const IORStack& stack) const
{ return GetSpecularInfoNM(ri,stack,0); }
SpecularInfo RoughDielectricSurface::GetSpecularInfoNM(const RayIntersectionGeometric& ri, const IORStack& stack, Scalar nm) const
{
    const Crossing c(*this,ri,&stack,nm);
    SpecularInfo info;
    info.valid = c.valid;
    info.canRefract = true;
    info.ior = c.index;
    // Equal indices remove the interface, regardless of its roughness.
    // Otherwise only advertise its interior index, without ballistic shadows.
    info.isSpecular = c.valid && c.eta == 1;
    info.clearTransmission = info.isSpecular;
    return info;
}
Scalar RoughDielectricSurface::EvaluateLobeFNM(const RayIntersectionGeometric& ri, const Vector3& wo, ScatteredRay::ScatRayType type, Scalar nm, const IORStack& stack) const
{ const Crossing c(*this,ri,&stack,nm); return type==c.Type(wo) ? c.Eval(wo,false).f : Scalar(0); }
Scalar RoughDielectricSurface::EvaluateKrayNM(const RayIntersectionGeometric& ri, const Vector3& wo, ScatteredRay::ScatRayType type, Scalar nm, const IORStack& stack) const
{
    const Crossing c(*this,ri,&stack,nm);
    if(type!=c.Type(wo) || !c.Allowed(wo)) return 0;
    if(c.eta==1) return type==ScatteredRay::eRayRefraction &&
        Vector3Ops::SquaredModulus(wo-Vector3Ops::Normalize(ri.ray.Dir()))==0 ? Scalar(1) : Scalar(0);
    return c.Eval(wo,false).weight;
}
Scalar RoughDielectricSurface::EvaluateKrayNM(const RayIntersectionGeometric& ri, const Vector3& wo, ScatteredRay::ScatRayType type, Scalar nm, const IORStack& stack, Scalar pdfHero) const
{
    if(pdfHero<=0) return EvaluateKrayNM(ri,wo,type,nm,stack);
    const Crossing c(*this,ri,&stack,nm);
    return type==c.Type(wo) ? c.Eval(wo,false).f * std::abs(Vector3Ops::Dot(Vector3Ops::Normalize(wo),c.frame.w())) / pdfHero : Scalar(0);
}
