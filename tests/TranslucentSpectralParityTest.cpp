// DL-01: tau is paid at entry; each interior segment pays Beer extinction
// once, then splits between exit and backscatter. These are lobe-weight
// checks, not directional/Pdf consistency checks (DL-02 is separate).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include "../src/Library/Materials/TranslucentSPF.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

static unsigned checks = 0, failures = 0;
static const Scalar kWeightTolerance = 1e-3; // DL-01 recipe's absolute band

static void Check(bool condition, const char* label)
{
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", label); }
}

static void Near(Scalar actual, Scalar expected, const char* label)
{
    ++checks;
    if (!(std::isfinite(actual) && std::fabs(actual - expected) <= kWeightTolerance)) {
        ++failures;
        std::printf("FAIL: %s got %.9f expected %.9f\n", label, actual, expected);
    }
}

static RayIntersectionGeometric Hit(Scalar distance, bool exiting)
{
    RasterizerState rs = {0, 0};
    RayIntersectionGeometric ri(Ray(Point3(0, 0, distance), Vector3(0, 0, -1)), rs);
    ri.bHit = true;
    ri.range = distance;
    ri.ptIntersection = Point3(0, 0, 0);
    ri.vNormal = ri.vGeomNormal = Vector3(0, 0, exiting ? -1 : 1);
    ri.onb.CreateFromW(ri.vNormal);
    return ri;
}

static void RunCase(Scalar tau, Scalar extinction, Scalar distance, Scalar scatter, bool splitN)
{
    std::printf("case tau=%.1f ext=%.1f distance=%.1f scatter=%.1f N=%s\n",
                tau, extinction, distance, scatter, splitN ? "5/10/15" : "1");
    auto* ref = new UniformColorPainter(RISEPel(0.0));
    auto* trans = new UniformColorPainter(RISEPel(tau));
    auto* ext = new UniformScalarPainter(extinction);
    IScalarPainter* n = splitN ? static_cast<IScalarPainter*>(new RGBScalarPainter(5, 10, 15))
                              : static_cast<IScalarPainter*>(new UniformScalarPainter(1));
    auto* scat = new UniformScalarPainter(scatter);
    auto* spf = new TranslucentSPF(*ref, *trans, *ext, *n, *scat);
    auto* object = new StubObject();
    RandomNumberGenerator rng;
    IndependentSampler sampler(rng);
    const auto ri = Hit(distance, true);
    IORStack inside = MakeTestIORStack(object, 1.33);
    inside.push(inside.top());
    Check(inside.containsCurrent(), "exit fixture is inside translucent object");
    const Scalar beer = std::exp(-extinction * distance);
    Scalar rgbExit = 0;
    {
        ScatteredRayContainer rgb;
        spf->Scatter(ri, sampler, rgb, inside);
        RISEPel exit(0.0), back(0.0);
        unsigned exits = 0;
        for (unsigned i = 0; i < rgb.Count(); ++i) {
            const auto& ray = rgb[i];
            if (ray.type == ScatteredRay::eRayDiffuse) {
                ++exits;
                exit = exit + ray.kray;
                Check(ray.ior_stack && !ray.ior_stack->containsCurrent(), "RGB exit pops object");
            } else {
                back = back + ray.kray;
                Check(!ray.ior_stack, "RGB backscatter keeps input stack");
            }
        }
        Check(exits == 1, "exactly one RGB exit");
        for (unsigned c = 0; c < 3; ++c) {
            Near(exit[c], beer * (1 - scatter), "RGB exit Beer split");
            Near(back[c], beer * scatter, "RGB backscatter Beer split");
            Near(exit[c] + back[c], beer, "RGB segment total is Beer, independent of tau");
        }
        rgbExit = exit[0];
    }
    for (Scalar nm : {450.0, 550.0, 650.0}) {
        ScatteredRayContainer spectral;
        spf->ScatterNM(ri, sampler, nm, spectral, inside);
        Scalar exit = 0, back = 0;
        unsigned exits = 0;
        for (unsigned i = 0; i < spectral.Count(); ++i) {
            const auto& ray = spectral[i];
            if (ray.type == ScatteredRay::eRayDiffuse) {
                ++exits;
                exit += ray.krayNM;
                Check(ray.ior_stack && !ray.ior_stack->containsCurrent(), "NM exit pops object");
            } else {
                back += ray.krayNM;
                Check(!ray.ior_stack, "NM backscatter keeps input stack");
            }
        }
        Check(exits == 1, "exactly one NM exit");
        Near(exit, beer * (1 - scatter), "NM exit Beer split");
        Near(back, beer * scatter, "NM backscatter Beer split");
        Near(exit + back, beer, "NM segment total is Beer, independent of tau");
        Near(exit, rgbExit, "RGB/NM exit parity");

        // A real entry-produced stack chains into a second scatter. The
        // expected tau is the painter's spectral value, avoiding a claim
        // that arbitrary RGB spectra equal any one RGB channel.
        if (tau > 0) {
            const auto entryHit = Hit(1, false);
            IORStack outside = MakeTestIORStack(object, 1.33);
            ScatteredRayContainer entry;
            spf->ScatterNM(entryHit, sampler, nm, entry, outside);
            const ScatteredRay* transmitted = nullptr;
            unsigned transmissions = 0;
            for (unsigned i = 0; i < entry.Count(); ++i) {
                if (entry[i].type == ScatteredRay::eRayTranslucent) {
                    transmitted = &entry[i];
                    ++transmissions;
                }
            }
            Check(transmissions == 1, "one spectral entry transmission");
            if (transmissions == 1 && transmitted->ior_stack) {
                Near(transmitted->krayNM, GuardedGetColorNM(*trans, entryHit, nm), "NM entry pays tau");
                ScatteredRayContainer chained;
                spf->ScatterNM(ri, sampler, nm, chained, *transmitted->ior_stack);
                Scalar sum = 0;
                for (unsigned i = 0; i < chained.Count(); ++i) sum += chained[i].krayNM;
                Near(transmitted->krayNM * sum, GuardedGetColorNM(*trans, entryHit, nm) * beer,
                     "NM entry/segment throughput pays tau once");
            } else {
                Check(false, "spectral entry must supply a stack for chained exit");
            }
        }
    }
    spf->release();
    scat->release(); n->release(); ext->release(); trans->release(); ref->release();
    object->release();
}

int main()
{
    GlobalLog();
    std::srand(1729);
    for (bool splitN : {false, true}) {
        RunCase(0.4, 0.1, 1, 0, splitN);
        RunCase(0.4, 0.1, 1, 0.3, splitN);
        RunCase(0.4, 0.1, 1, 1, splitN);
        RunCase(0, 0.1, 1, 0.3, splitN); // seeded interior; no entry needed
        RunCase(1, 0.1, 1, 0.3, splitN);
        RunCase(0.7, 0, 2, 0.3, splitN);
        RunCase(0.4, 3, 2, 0.3, splitN);
        RunCase(0.4, 0.1, 0, 0.3, splitN);
    }
    std::printf("TranslucentSpectralParityTest: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
