//////////////////////////////////////////////////////////////////////
//
//  SchlickWardBackfaceEnergyTest.cpp - DL-100 red-proof.
//
//  CLAIM: SchlickSPF's (and WardIsotropicGaussianSPF's) specular
//  sampler builds its half-vector in the FLIPPED sampling frame
//  (Scatter's `myonb`, oriented to face the incoming ray), the SAME
//  frame its own accept-check tests -- not the raw, unflipped `ri.onb`.
//
//  Why a back-face hit is the discriminator.  `Scatter`/`ScatterNM`
//  build `myonb` by copying `ri.onb` and calling `FlipW()` whenever
//  `dot(ri.ray.Dir(), ri.onb.w()) > 0` (a single-sided surface hit from
//  behind).  Every accept-check in `Scatter` then tests the FLIPPED
//  `myonb.w()`.  Before the DL-100 fix, `GenerateSpecularRay` (both
//  files) ignored the frame it was handed (in Ward's case a literal
//  dead parameter; in Schlick's case there was no parameter to ignore
//  -- it read `ri.onb` directly) and always built its half-vector `h`
//  around the UNFLIPPED normal.  On a back-face hit the reflected
//  direction then lands on the wrong side of `myonb.w()`, and the
//  accept-check rejects EVERY specular draw -- the whole specular lobe
//  silently disappears, diffuse-only, full diffuse weight.
//
//  METHOD.  Two intersections that are mirror images of each other
//  under x -> -x: a FRONT-face hit at incidence theta (the existing
//  convention every other Schlick/Ward test uses: `ri.onb.w() =
//  (0,0,1)`, incoming ray tilted toward +X) and a BACK-face hit at the
//  SAME incidence theta (incoming ray tilted toward +X but travelling
//  in +Z, so it strikes the same unflipped normal from behind and
//  `myonb` flips to `(0,0,-1)`-ish).  For an ISOTROPIC material this
//  mirror symmetry makes the two configurations physically identical --
//  same incidence angle, same roughness, same reflectance -- so the
//  measured fraction of `Scatter()` calls that emit a specular
//  (`eRayReflection`) ray must agree between front and back within
//  Monte-Carlo noise.  Pre-fix, the back-face rate collapses to ~0;
//  post-fix it matches the front-face rate.
//
//  A second, independent check integrates `Pdf()`/`WardIsotropicPdf`
//  (via `SchlickSPF::Pdf` for Schlick; Ward's `Pdf()` already builds its
//  half-vector from `wi+wo` rather than an onb-relative warp, so it was
//  never wrong FOR THE DENSITY -- only the SAMPLER's frame was; this
//  gate would already have passed before this fix for Ward, and is
//  included for completeness / regression coverage) over the physically
//  correct (post-flip) hemisphere at the back-face configuration and
//  checks it against the measured total emission mass -- exercising the
//  SchlickSPF `Pdf()` frame fix (ComputeSchlickSpecularPdf /
//  SchlickInvertSpecular / SchlickDiffuseSelectCoefficient) directly.
//
//  Build:
//    make -C build/make/rise build-test/SchlickWardBackfaceEnergyTest
//
//  Author: Aravind Krishnaswamy (worker session, DL-100)
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <cmath>
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

// A hit whose UNFLIPPED shading/geometric normal is always (0,0,1).
// `backface==false` reproduces every other Schlick/Ward test's
// convention (incoming ray tilted toward +X, travelling in -Z: a
// FRONT-face hit).  `backface==true` mirrors it under z -> -z (incoming
// ray tilted toward +X, travelling in +Z): same incidence angle off the
// SAME unflipped normal, but struck from behind, so Scatter's
// `myonb.FlipW()` fires.
static RayIntersectionGeometric MakeHit( double incomingTheta, bool backface )
{
    const double sinT = sin(incomingTheta);
    const double cosT = cos(incomingTheta);
    const double zSign = backface ? 1.0 : -1.0;
    const Vector3 inDir( sinT, 0, zSign*cosT );

    Ray inRay( Point3( -sinT, 0, -zSign ), inDir );
    RasterizerState rs = {0, 0};
    RayIntersectionGeometric ri( inRay, rs );

    ri.bHit = true;
    ri.range = 1.0;
    ri.ptIntersection = Point3(0, 0, 0);
    ri.vNormal = Vector3(0, 0, 1);
    ri.onb.CreateFromW( Vector3(0, 0, 1) );
    ri.ptCoord = Point2(0.5, 0.5);
    // Degenerate vGeomNormal (SquaredModulus 0): Scatter's geometric-
    // horizon gate then falls back to the shading normal and is a no-op,
    // isolating this test to the frame bug alone.

    return ri;
}

// Fraction of `nDraws` Scatter()/ScatterNM() calls whose container holds
// at least one eRayReflection (specular) entry.
template <class SPF>
static double MeasureSpecularEmissionRate( const SPF* spf, const RayIntersectionGeometric& ri, const IORStack& iorStack, long nDraws, unsigned int seed, bool bNM )
{
    RandomNumberGenerator rng( seed );
    IndependentSampler sampler( rng );
    long nSpecular = 0;
    for( long i = 0; i < nDraws; i++ ) {
        ScatteredRayContainer scattered;
        if( bNM ) {
            spf->ScatterNM( ri, sampler, 550.0, scattered, iorStack );
        } else {
            spf->Scatter( ri, sampler, scattered, iorStack );
        }
        for( unsigned int k = 0; k < scattered.Count(); k++ ) {
            if( scattered[k].type == ScatteredRay::eRayReflection ) {
                nSpecular++;
                break;
            }
        }
    }
    return double(nSpecular) / double(nDraws);
}

static const long kDraws = 200000;
static const double kTol = 0.02; // Monte-Carlo tolerance on a 200k-draw rate

static void TestSchlick()
{
    std::cout << "\n-- SchlickSPF: front vs back-face specular emission rate --" << std::endl;

    UniformColorPainter* diff = new UniformColorPainter( RISEPel(0.5,0.5,0.5) ); diff->addref();
    UniformColorPainter* spec = new UniformColorPainter( RISEPel(0.6,0.6,0.6) ); spec->addref();
    UniformScalarPainter* rough = new UniformScalarPainter( 0.3 ); rough->addref();
    UniformScalarPainter* iso = new UniformScalarPainter( 1.0 ); iso->addref();
    SchlickSPF* spf = new SchlickSPF( *diff, *spec, *rough, *iso ); spf->addref();

    IORStack iorStack = MakeTestIORStack( g_stubObject );

    const double thetas[] = { 10.0, 30.0, 45.0, 60.0 };
    for( double thetaDeg : thetas ) {
        const double theta = thetaDeg * PI / 180.0;
        RayIntersectionGeometric front = MakeHit( theta, false );
        RayIntersectionGeometric back  = MakeHit( theta, true );

        const double rateFront = MeasureSpecularEmissionRate( spf, front, iorStack, kDraws, 111000+(long)thetaDeg, false );
        const double rateBack  = MeasureSpecularEmissionRate( spf, back,  iorStack, kDraws, 222000+(long)thetaDeg, false );

        std::cout << "  theta=" << thetaDeg << "  front=" << rateFront
                  << "  back=" << rateBack << "  |diff|=" << fabs(rateFront-rateBack) << std::endl;

        CHECK( rateFront > 0.3, "SchlickSPF front-face specular emission rate must be substantial (sanity: the fixture itself is not degenerate)" );
        CHECK( fabs(rateFront - rateBack) <= kTol,
            "SchlickSPF theta=" + std::to_string(thetaDeg) + ": back-face specular emission rate must match the mirror-image front-face rate (DL-100)" );
    }

    // Spectral twin, one config.
    {
        const double theta = 45.0 * PI / 180.0;
        RayIntersectionGeometric front = MakeHit( theta, false );
        RayIntersectionGeometric back  = MakeHit( theta, true );
        const double rateFront = MeasureSpecularEmissionRate( spf, front, iorStack, kDraws, 333045, true );
        const double rateBack  = MeasureSpecularEmissionRate( spf, back,  iorStack, kDraws, 444045, true );
        std::cout << "  NM theta=45  front=" << rateFront << "  back=" << rateBack
                  << "  |diff|=" << fabs(rateFront-rateBack) << std::endl;
        CHECK( fabs(rateFront - rateBack) <= kTol, "SchlickSPF ScatterNM theta=45: back-face specular emission rate must match front-face (DL-100)" );
    }

    // Per-channel branch (RGBScalarPainter roughness): same claim, since
    // GenerateSpecularRay is called identically per lane.
    {
        RGBScalarPainter* roughPC = new RGBScalarPainter( 0.2, 0.3, 0.4 ); roughPC->addref();
        SchlickSPF* spfPC = new SchlickSPF( *diff, *spec, *roughPC, *iso ); spfPC->addref();
        const double theta = 45.0 * PI / 180.0;
        RayIntersectionGeometric front = MakeHit( theta, false );
        RayIntersectionGeometric back  = MakeHit( theta, true );
        const double rateFront = MeasureSpecularEmissionRate( spfPC, front, iorStack, kDraws, 555045, false );
        const double rateBack  = MeasureSpecularEmissionRate( spfPC, back,  iorStack, kDraws, 666045, false );
        std::cout << "  per-channel theta=45  front=" << rateFront << "  back=" << rateBack
                  << "  |diff|=" << fabs(rateFront-rateBack) << std::endl;
        CHECK( fabs(rateFront - rateBack) <= kTol, "SchlickSPF per-channel branch theta=45: back-face specular emission rate must match front-face (DL-100)" );
        spfPC->release(); roughPC->release();
    }

    // Pdf() two-sided consistency at a back-face hit: integrate Pdf over
    // the PHYSICALLY CORRECT (post-flip) hemisphere and compare against
    // the measured total emission mass (any ray at all).  This directly
    // exercises the Pdf-side frame fix (ComputeSchlickSpecularPdf /
    // SchlickInvertSpecular / SchlickDiffuseSelectCoefficient all now
    // taking `myonb`), independent of the emission-rate check above.
    {
        const double theta = 30.0 * PI / 180.0;
        RayIntersectionGeometric back = MakeHit( theta, true );

        RandomNumberGenerator rng( 777030 );
        IndependentSampler sampler( rng );
        long emitted = 0;
        for( long i = 0; i < kDraws; i++ ) {
            ScatteredRayContainer scattered;
            spf->Scatter( back, sampler, scattered, iorStack );
            if( scattered.RandomlySelect( rng.CanonicalRandom(), false ) ) {
                emitted++;
            }
        }
        const double massEmp = double(emitted) / double(kDraws);

        // Hemisphere quadrature around myonb.w() == (0,0,-1) (the
        // post-flip frame): sample directions with negative z.
        const int qt = 400, qp = 800;
        double intPdf = 0;
        const double dw = (1.0/qt) * (TWO_PI/qp);
        for( int a = 0; a < qt; a++ ) {
            const double ct = (a + 0.5)/qt;      // cos(theta) measured from -Z
            const double st = sqrt( 1.0 - ct*ct );
            for( int b = 0; b < qp; b++ ) {
                const double ph = (b + 0.5)/qp * TWO_PI;
                const Vector3 wo( st*cos(ph), st*sin(ph), -ct );  // hemisphere around -Z
                const double pdf = spf->Pdf( back, wo, iorStack );
                intPdf += pdf * dw;
            }
        }

        std::cout << "  Pdf mass check (back-face, theta=30):  intPdf=" << intPdf
                  << "  emitted=" << massEmp << "  |diff|=" << fabs(intPdf-massEmp) << std::endl;
        CHECK( fabs(intPdf - massEmp) <= 0.02, "SchlickSPF::Pdf integral over the post-flip hemisphere must match measured emission mass at a back-face hit (DL-100)" );
    }

    spf->release(); diff->release(); spec->release(); rough->release(); iso->release();
}

static void TestWardIsotropic()
{
    std::cout << "\n-- WardIsotropicGaussianSPF: front vs back-face specular emission rate --" << std::endl;

    UniformColorPainter* diff = new UniformColorPainter( RISEPel(0.5,0.5,0.5) ); diff->addref();
    UniformColorPainter* spec = new UniformColorPainter( RISEPel(0.6,0.6,0.6) ); spec->addref();
    UniformScalarPainter* alpha = new UniformScalarPainter( 0.2 ); alpha->addref();
    WardIsotropicGaussianSPF* spf = new WardIsotropicGaussianSPF( *diff, *spec, *alpha ); spf->addref();

    IORStack iorStack = MakeTestIORStack( g_stubObject );

    const double thetas[] = { 10.0, 30.0, 45.0, 60.0 };
    for( double thetaDeg : thetas ) {
        const double theta = thetaDeg * PI / 180.0;
        RayIntersectionGeometric front = MakeHit( theta, false );
        RayIntersectionGeometric back  = MakeHit( theta, true );

        const double rateFront = MeasureSpecularEmissionRate( spf, front, iorStack, kDraws, 121000+(long)thetaDeg, false );
        const double rateBack  = MeasureSpecularEmissionRate( spf, back,  iorStack, kDraws, 232000+(long)thetaDeg, false );

        std::cout << "  theta=" << thetaDeg << "  front=" << rateFront
                  << "  back=" << rateBack << "  |diff|=" << fabs(rateFront-rateBack) << std::endl;

        CHECK( rateFront > 0.3, "WardIsotropicGaussianSPF front-face specular emission rate must be substantial (sanity)" );
        CHECK( fabs(rateFront - rateBack) <= kTol,
            "WardIsotropicGaussianSPF theta=" + std::to_string(thetaDeg) + ": back-face specular emission rate must match the mirror-image front-face rate (DL-100)" );
    }

    spf->release(); diff->release(); spec->release(); alpha->release();
}

int main()
{
    std::cout << "===== SchlickSPF / WardIsotropicGaussianSPF Back-Face Specular Energy Test (DL-100) =====" << std::endl;

    g_stubObject = new StubObject();
    g_stubObject->addref();
    GlobalLog();

    TestSchlick();
    TestWardIsotropic();

    std::cout << "\nChecks: " << g_checks << " Failures: " << g_failures << std::endl;
    if( g_failures == 0 ) {
        std::cout << "All DL-100 back-face energy checks passed!" << std::endl;
        return 0;
    }
    return 1;
}
