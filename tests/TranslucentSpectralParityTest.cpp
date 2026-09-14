// DL-01: tau is paid at entry; each interior segment pays Beer extinction
// once, then splits between exit and backscatter. These are lobe-weight
// checks. DL-02 below independently pins exit-direction/Pdf consistency.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
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

// Fixed variates make the exit inverse CDF test independent of rand() and
// argument evaluation order: every Get1D in one scatter returns the same u.
class FixedSampler : public ISampler
{
    Scalar u;
public:
    explicit FixedSampler(Scalar value) : u(value) {}
    Scalar Get1D() override { return u; }
    Point2 Get2D() override { return Point2(u, u); }
};

static bool DensityNear(Scalar actual, Scalar expected)
{
    return std::isfinite(actual) && std::fabs(actual - expected) <= 1e-10;
}

static void RunExitDensity(Scalar exponent, Scalar scatter, bool tilted)
{
    std::printf("density N=%g scatter=%g tilted=%d\n", exponent, scatter, int(tilted));
    auto* ref = new UniformColorPainter(RISEPel(0.5));
    auto* trans = new UniformColorPainter(RISEPel(0.4));
    auto* ext = new UniformScalarPainter(0.1);
    IScalarPainter* n = exponent < 0
        ? static_cast<IScalarPainter*>(new RGBScalarPainter(5, 10, 15))
        : static_cast<IScalarPainter*>(new UniformScalarPainter(exponent));
    auto* scat = new UniformScalarPainter(scatter);
    auto* spf = new TranslucentSPF(*ref, *trans, *ext, *n, *scat);
    auto* object = new StubObject();
    auto ri = Hit(1, true);
    if (tilted) {
        ri.vNormal = Vector3Ops::Normalize(Vector3(1, 2, -3));
        ri.onb.CreateFromW(ri.vNormal);
        // Retain the true geometric normal (ri.vGeomNormal stays (0,0,-1)
        // from Hit()): DL-45 -- the exit re-emission's geometric-horizon
        // gate uses vGeomNormal directly (NOT the entering reflection
        // lobe's ray-anchored `geomN`), so tilting only the SHADING
        // normal here deliberately exercises that gate/renormalization,
        // not the entry lobe's separate, unrenormalized one.
    }
    IORStack inside = MakeTestIORStack(object, 1.33);
    inside.push(inside.top());
    Check(inside.containsCurrent(), "density fixture starts inside");
    // DL-45: the exit density is normalized to the geometrically-valid
    // sub-hemisphere (dot(wo,vGeomNormal)>0), not the full shading
    // hemisphere -- see TranslucentSPF.cpp's ExitValidFraction.  At
    // tilt=0, onb.w() == vGeomNormal exactly (both (0,0,-1) from Hit()),
    // so cosPhi=1 and validFraction=1: the untitled sub-test's original
    // expectations are unaffected by this factor.
    const Scalar cosPhi = Vector3Ops::Dot(ri.onb.w(), ri.vGeomNormal);
    const Scalar validFraction = std::max(Scalar(1e-4), (Scalar(1) + cosPhi) * Scalar(0.5));
    for (int pipe = 0; pipe < 4; ++pipe) {
        const Scalar nm = 450 + 100 * (pipe - 1);
        bool support = true, stored = true, evaluated = true, cdf = true, popped = true;
        Scalar secondMoment = 0;
        for (int i = 0; i < 8; ++i) {
            const Scalar u = (i + 0.5) / 8;
            FixedSampler sampler(u);
            ScatteredRayContainer rays;
            if (pipe == 0) spf->Scatter(ri, sampler, rays, inside);
            else spf->ScatterNM(ri, sampler, nm, rays, inside);
            unsigned exits = 0;
            for (unsigned j = 0; j < rays.Count(); ++j) {
                const auto& ray = rays[j];
                if (ray.type != ScatteredRay::eRayDiffuse) continue;
                ++exits;
                const Scalar mu = Vector3Ops::Dot(ray.ray.Dir(), ri.onb.w());
                const Scalar expected = mu * INV_PI / validFraction;
                const Scalar pdf = pipe == 0 ? spf->Pdf(ri, ray.ray.Dir(), inside)
                    : spf->PdfNM(ri, ray.ray.Dir(), nm, inside);
                support &= mu > 0 && ray.pdf > 0 && !ray.isDelta;
                stored &= DensityNear(ray.pdf, expected);
                evaluated &= DensityNear(pdf, ray.pdf);
                // P1 (same debt-cleanup slice, 2026-09-13): `cdf` and `secondMoment` pin the UNCLIPPED
                // plain-cosine sampler's inverse-CDF identity
                // (mu^2 == u, hence E[mu^2] == 1/2) under a FixedSampler
                // that returns the SAME canonical (u1,u2) on every draw.
                // DL-45's original rejection-loop implementation either
                // reproduced that exact unclipped sample (first attempt
                // already valid) or, since a FixedSampler makes every
                // retry identical, failed all 32 identical attempts and
                // emitted NOTHING -- so whenever `RunExitDensity` DID see
                // an accepted sample under tilt, it was, by construction,
                // an unmodified unclipped one, and this identity held
                // coincidentally.  P1's exact two-draw remap instead
                // deterministically TRANSFORMS (u1,u2) into a genuinely
                // different, geometrically-valid direction whenever
                // clipping is active (cosPhi<1) -- an accepted tilted
                // sample's mu is no longer mu^2==u by construction, even
                // though the resulting DISTRIBUTION remains exactly
                // normalized (proven by `stored`/`evaluated` above and by
                // TranslucentTiltedExitTest's independent fine-quadrature
                // integral-to-1 checks).  So these two identities are
                // correctly restricted to the untilted (cosPhi==1,
                // identity remap) case, where the new remap is a no-op
                // and the old identity still holds exactly -- verified:
                // 0 failures at tilted=0 pre- and post-P1.
                if (!tilted) {
                    cdf &= DensityNear(mu * mu, u);
                    secondMoment += mu * mu / 8;
                }
                // Under tilt neither identity applies; both checks are
                // SKIPPED below rather than satisfied by assigning the
                // expected value to the observable (a round-2 revision
                // set `secondMoment = 0.5` here, which made
                // `Check(DensityNear(secondMoment, 0.5))` a tautology
                // that would pass against any sampler at all).
                popped &= ray.ior_stack && !ray.ior_stack->containsCurrent();
            }
            Check(exits == 1, "density sample has exactly one exit");
        }
        std::printf("  %s nm=%g\n", pipe == 0 ? "RGB" : "NM", pipe == 0 ? 0.0 : nm);
        Check(support, "exit support is positive shading hemisphere");
        Check(stored, "stored exit density is cosine, independent of N");
        Check(evaluated, "exit evaluated density equals stored density");
        // Both identities are specific to the UNCLIPPED plain-cosine
        // draw the untilted case reduces to; the tilted case's own
        // distribution is checked by TranslucentTiltedExitTest's
        // chi-squared goodness-of-fit (sub-test 4), not here.
        if (!tilted) {
            Check(cdf, "exit sample inverse CDF is cosine, independent of N");
            Check(DensityNear(secondMoment, 0.5), "exit sampled cosine second moment is 1/2");
        } else {
            std::printf("  (tilted: inverse-CDF and second-moment identities skipped -- "
                "see TranslucentTiltedExitTest sub-test 4 for the tilted sampler's own test)\n");
        }
        Check(popped, "density samples carry popped exit stack");

        // Integrate in mu/phi, where dOmega = dmu dphi. A midpoint rule
        // integrates the cosine density exactly; sample both hemispheres
        // to distinguish normalization from support (a flipped PDF also
        // integrates to one over the full sphere).
        Scalar positive = 0, negative = 0;
        for (int m = 0; m < 32; ++m) {
            const Scalar mu = (m + 0.5) / 32;
            for (int a = 0; a < 8; ++a) {
                const Scalar phi = TWO_PI * (a + 0.5) / 8;
                const Scalar r = std::sqrt(1 - mu * mu);
                const Vector3 tangent = ri.onb.u() * (r * std::cos(phi))
                    + ri.onb.v() * (r * std::sin(phi));
                const Vector3 wo = tangent + ri.onb.w() * mu;
                const Vector3 back = tangent - ri.onb.w() * mu;
                positive += (pipe == 0 ? spf->Pdf(ri, wo, inside)
                    : spf->PdfNM(ri, wo, nm, inside)) * TWO_PI / (32 * 8);
                negative += (pipe == 0 ? spf->Pdf(ri, back, inside)
                    : spf->PdfNM(ri, back, nm, inside)) * TWO_PI / (32 * 8);
            }
        }
        // A tilted shading normal makes the geometric-horizon cutoff fall
        // inside a mu-cell rather than exactly at a grid line, so this
        // coarse (32x8) midpoint-rule quadrature no longer integrates the
        // (now genuinely discontinuous, DL-45) density EXACTLY the way it
        // does the untilted plain-cosine case -- loosen the tolerance by
        // the quadrature's own O(1/gridsize) discretization error instead
        // of the exact float-noise band DensityNear uses.  Still tight
        // enough to separate a normalized density (~1.0) from the old
        // unrenormalized-reject policy's value (validFraction ==
        // (1+cosPhi)/2, e.g. ~0.90 for this fixture's tilt -- NOT cosPhi
        // itself, ~0.80, which this comment previously and incorrectly
        // cited), which is what a regression of DL-45 would produce here.
        const Scalar positiveTol = tilted ? 0.05 : 1e-10;
        ++checks;
        if (!(std::isfinite(positive) && std::fabs(positive - 1) <= positiveTol)) {
            ++failures;
            std::printf("FAIL: exit PDF integrates to one on exit hemisphere got %.9f\n", positive);
        }
        // This API describes the diffuse lobe only; backscatter has a
        // separate stored Phong density, not a complete Pdf/PdfNM mixture.
        Check(DensityNear(negative, 0), "diffuse PDF excludes backscatter hemisphere");
    }
    auto entry = Hit(1, false);
    IORStack outside = MakeTestIORStack(object, 1.33);
    Check(DensityNear(spf->Pdf(entry, entry.onb.w(), outside), INV_PI),
          "entry reflection PDF retains front hemisphere");
    Check(DensityNear(spf->Pdf(entry, -entry.onb.w(), outside), 0),
          "entry reflection PDF excludes back hemisphere");
    entry.vGeomNormal = Vector3Ops::Normalize(Vector3(1, 0, 1));
    const Vector3 belowGeometry = Vector3Ops::Normalize(Vector3(-1, 0, 0.1));
    Check(DensityNear(spf->Pdf(entry, belowGeometry, outside), 0),
          "entry geometric-horizon rejection remains active");
    Check(DensityNear(spf->PdfNM(entry, belowGeometry, 550, outside), 0),
          "spectral entry geometric-horizon rejection remains active");
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
    for (Scalar exponent : {1.0, 8.0, -1.0})
        for (Scalar scatter : {0.0, 0.3, 1.0})
            for (bool tilted : {false, true}) RunExitDensity(exponent, scatter, tilted);
    std::printf("TranslucentSpectralParityTest: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
