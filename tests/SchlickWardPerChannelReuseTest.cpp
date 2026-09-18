//////////////////////////////////////////////////////////////////////
//
//  SchlickWardPerChannelReuseTest.cpp - DL-101 red-proof.
//
//  CLAIM: `SchlickSPF`'s and both Ward SPFs' per-channel specular loop
//  ("HasPerChannelVariation" branch of `Scatter`) declares a fresh
//  `ScatteredRay` on EVERY iteration, so a lane whose sampler declines to
//  write a direction (`GenerateSpecularRay`'s `hdotk <= 0` guard) is
//  simply not pushed -- never mistaken for a DIFFERENT lane's leftover
//  ray.
//
//  Why this matters.  Before the fix, all three files declared ONE
//  `ScatteredRay s` OUTSIDE the per-channel loop and called
//  `GenerateSpecularRay(s, ...)` three times against it.
//  `GenerateSpecularRay` writes `s.ray` ONLY when its `hdotk > 0` accept
//  condition holds; otherwise it leaves `s` untouched.  On lane 0 an
//  untouched `s.ray` defaults to direction (0,0,0), which the caller's
//  own accept-check then rejects harmlessly -- but on lanes 1 and 2 the
//  untouched `s.ray` still holds the PREVIOUS lane's direction, which
//  generally DOES pass the accept-check, and the branch then assigns
//  THIS lane's `kray`/`pdf` to it before pushing. The container receives
//  a ray at lane j's direction, priced as lane i.
//
//  METHOD.  A direct, model-free signature: for each real `Scatter()`
//  call at a fixture where the bug is likely to fire (grazing incidence,
//  THREE MUTUALLY DISTINCT per-channel roughness/alpha values, so each
//  lane's `hdotk<=0` rejection rate differs from its neighbours' and a
//  rejected lane's fallback direction is a DIFFERENT lane's real,
//  accepted draw -- with three distinct roughness values, two lanes that
//  are BOTH accepted generically compute different half-vectors and so
//  different directions; two independently-computed continuous
//  directions coinciding to full float precision by chance has
//  probability approximately zero), collect every specular
//  (`eRayReflection`) ray the container holds and count how many CALLS
//  produce two or more of them with BIT-IDENTICAL directions.  Pre-fix
//  (the doc's own measurement, a different low-then-high-roughness
//  parameterisation than this file's): 111613/200000 calls corrupted for
//  SchlickSPF; this file's own red-proof (below) reproduces the same
//  mechanism, freshly measured, on all three affected classes.
//
//  A companion sanity control uses an `RGBScalarPainter` with EQUAL
//  r/g/b: `HasPerChannelVariation()` (RGBScalarPainter.h) then reports
//  `false`, so `Scatter()` takes the single-lobe branch, not the
//  per-channel loop, and the counter reads 0 regardless of the fix --
//  demonstrating the counter itself does not fire on an ordinary
//  single-lobe draw.
//
//  Build:
//    make -C build/make/rise build-test/SchlickWardPerChannelReuseTest
//
//  Author: Aravind Krishnaswamy (worker session, DL-101)
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <cmath>
#include <vector>
#include <string>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Materials/SchlickSPF.h"
#include "../src/Library/Materials/WardIsotropicGaussianSPF.h"
#include "../src/Library/Materials/WardAnisotropicEllipticalGaussianSPF.h"
#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

static StubObject* g_stubObject = 0;
static int g_failures = 0;
static int g_checks = 0;

#define CHECK( cond, msg ) \
    do { \
        g_checks++; \
        if( !(cond) ) { \
            std::cout << "  FAIL: " << msg << std::endl; \
            g_failures++; \
        } \
    } while( 0 )

// Same front-face fixture convention every other Schlick/Ward test uses:
// ri.onb.w() = (0,0,1), incoming ray at incidence `incomingTheta` tilted
// toward +X.
static RayIntersectionGeometric MakeFrontHit( double incomingTheta )
{
    const double sinT = sin(incomingTheta);
    const double cosT = cos(incomingTheta);
    const Vector3 inDir( sinT, 0, -cosT );

    Ray inRay( Point3(-sinT, 0, 1.0), inDir );
    RasterizerState rs = {0, 0};
    RayIntersectionGeometric ri( inRay, rs );

    ri.bHit = true;
    ri.range = 1.0;
    ri.ptIntersection = Point3(0, 0, 0);
    ri.vNormal = Vector3(0, 0, 1);
    ri.onb.CreateFromW( Vector3(0, 0, 1) );
    ri.ptCoord = Point2(0.5, 0.5);

    return ri;
}

// Count, over `nDraws` real Scatter() calls, how many calls produce TWO
// (or more) specular (eRayReflection) rays whose directions are
// bit-identical -- the direct DL-101 signature.  Returns the count of
// AFFECTED CALLS (not pairs), matching the doc's own "111613/200000"
// framing.
template <class SPF>
static long CountDuplicateDirectionCalls( const SPF* spf, const RayIntersectionGeometric& ri, const IORStack& iorStack, long nDraws, unsigned int seed )
{
    RandomNumberGenerator rng( seed );
    IndependentSampler sampler( rng );
    long affected = 0;

    for( long i = 0; i < nDraws; i++ ) {
        ScatteredRayContainer scattered;
        spf->Scatter( ri, sampler, scattered, iorStack );

        std::vector<Vector3> specDirs;
        for( unsigned int k = 0; k < scattered.Count(); k++ ) {
            if( scattered[k].type == ScatteredRay::eRayReflection ) {
                specDirs.push_back( Vector3Ops::Normalize( scattered[k].ray.Dir() ) );
            }
        }

        bool dup = false;
        for( size_t a = 0; a < specDirs.size() && !dup; a++ ) {
            for( size_t b = a+1; b < specDirs.size() && !dup; b++ ) {
                // Bit-identical (to tight float tolerance): two
                // INDEPENDENTLY drawn continuous directions coinciding
                // is a probability-zero event, so any near-exact match
                // is the reuse bug, not chance.
                if( Vector3Ops::SquaredModulus( specDirs[a] - specDirs[b] ) < 1e-24 ) {
                    dup = true;
                }
            }
        }
        if( dup ) {
            affected++;
        }
    }
    return affected;
}

static const long kDraws = 200000;

int main()
{
    std::cout << "===== SchlickSPF / Ward Per-Channel ScatteredRay Reuse Test (DL-101) =====" << std::endl;

    g_stubObject = new StubObject();
    g_stubObject->addref();
    GlobalLog();

    IORStack iorStack = MakeTestIORStack( g_stubObject );

    // Grazing incidence, WIDE per-channel roughness spread (matching the
    // DL-101 ledger recipe / DL67 Slice-0 doc's own reproduction: a lane
    // with a very low roughness has a near-mirror lobe with a HIGH
    // hdotk<=0 rejection rate at grazing incidence, while its neighbours
    // at high roughness rarely reject -- exactly the asymmetric
    // rejection-rate mismatch that makes the leaked-direction bug common
    // rather than a one-in-a-million coincidence).
    const double theta = 88.0 * PI / 180.0;
    RayIntersectionGeometric ri = MakeFrontHit( theta );

    // ---- SchlickSPF ----
    {
        UniformColorPainter* diff = new UniformColorPainter( RISEPel(0.5,0.5,0.5) ); diff->addref();
        UniformColorPainter* spec = new UniformColorPainter( RISEPel(0.6,0.6,0.6) ); spec->addref();
        RGBScalarPainter* rough = new RGBScalarPainter( 0.02, 0.5, 0.95 ); rough->addref();
        UniformScalarPainter* iso = new UniformScalarPainter( 0.5 ); iso->addref();
        SchlickSPF* spf = new SchlickSPF( *diff, *spec, *rough, *iso ); spf->addref();

        const long affected = CountDuplicateDirectionCalls( spf, ri, iorStack, kDraws, 909001 );
        std::cout << "  SchlickSPF grazing r={0.02,0.5,0.95}: duplicate-direction calls = "
                  << affected << "/" << kDraws << std::endl;
        CHECK( affected == 0, "SchlickSPF per-channel loop must never push two specular rays with bit-identical directions (DL-101)" );

        // Sanity control: an RGBScalarPainter with EQUAL r/g/b reports
        // HasPerChannelVariation() == false (RGBScalarPainter.h), so
        // Scatter() takes the single-lobe branch, not the per-channel
        // loop, and this reads 0 regardless of the fix -- confirms the
        // counter itself does not fire on ordinary single-lobe draws.
        RGBScalarPainter* roughEq = new RGBScalarPainter( 0.3, 0.3, 0.3 ); roughEq->addref();
        SchlickSPF* spfEq = new SchlickSPF( *diff, *spec, *roughEq, *iso ); spfEq->addref();
        const long affectedEq = CountDuplicateDirectionCalls( spfEq, ri, iorStack, kDraws, 909002 );
        std::cout << "  SchlickSPF grazing r={0.3,0.3,0.3} (control): duplicate-direction calls = "
                  << affectedEq << "/" << kDraws << std::endl;
        CHECK( affectedEq == 0, "SchlickSPF per-channel loop control (equal roughness) must read zero duplicates" );

        spfEq->release(); roughEq->release();
        spf->release(); diff->release(); spec->release(); rough->release(); iso->release();
    }

    // ---- WardIsotropicGaussianSPF ----
    {
        UniformColorPainter* diff = new UniformColorPainter( RISEPel(0.5,0.5,0.5) ); diff->addref();
        UniformColorPainter* spec = new UniformColorPainter( RISEPel(0.6,0.6,0.6) ); spec->addref();
        RGBScalarPainter* alpha = new RGBScalarPainter( 0.02, 0.2, 0.4 ); alpha->addref();
        WardIsotropicGaussianSPF* spf = new WardIsotropicGaussianSPF( *diff, *spec, *alpha ); spf->addref();

        const long affected = CountDuplicateDirectionCalls( spf, ri, iorStack, kDraws, 909101 );
        std::cout << "  WardIsotropicGaussianSPF grazing alpha={0.02,0.2,0.4}: duplicate-direction calls = "
                  << affected << "/" << kDraws << std::endl;
        CHECK( affected == 0, "WardIsotropicGaussianSPF per-channel loop must never push two specular rays with bit-identical directions (DL-101)" );

        spf->release(); diff->release(); spec->release(); alpha->release();
    }

    // ---- WardAnisotropicEllipticalGaussianSPF ----
    {
        UniformColorPainter* diff = new UniformColorPainter( RISEPel(0.5,0.5,0.5) ); diff->addref();
        UniformColorPainter* spec = new UniformColorPainter( RISEPel(0.6,0.6,0.6) ); spec->addref();
        RGBScalarPainter* alphax = new RGBScalarPainter( 0.02, 0.25, 0.4 ); alphax->addref();
        UniformScalarPainter* alphay = new UniformScalarPainter( 0.2 ); alphay->addref();
        WardAnisotropicEllipticalGaussianSPF* spf = new WardAnisotropicEllipticalGaussianSPF( *diff, *spec, *alphax, *alphay ); spf->addref();

        const long affected = CountDuplicateDirectionCalls( spf, ri, iorStack, kDraws, 909201 );
        std::cout << "  WardAnisotropicEllipticalGaussianSPF grazing alphaX={0.02,0.25,0.4}: duplicate-direction calls = "
                  << affected << "/" << kDraws << std::endl;
        CHECK( affected == 0, "WardAnisotropicEllipticalGaussianSPF per-channel loop must never push two specular rays with bit-identical directions (DL-101)" );

        spf->release(); diff->release(); spec->release(); alphax->release(); alphay->release();
    }

    std::cout << "\nChecks: " << g_checks << " Failures: " << g_failures << std::endl;
    if( g_failures == 0 ) {
        std::cout << "All DL-101 per-channel ScatteredRay reuse checks passed!" << std::endl;
        return 0;
    }
    return 1;
}
