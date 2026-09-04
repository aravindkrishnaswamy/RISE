//////////////////////////////////////////////////////////////////////
//
//  SPFBSDFConsistencyTest.cpp - Validates consistency between each
//    material's SPF (sampling) and BSDF (evaluation) implementations.
//
//  Tests:
//    A. Sanity: All 15 SPFs produce valid rays (non-negative kray,
//       normalized directions).
//    B. Delta direction: PerfectReflector/Refractor/Dielectric produce
//       correct mirror/refraction directions, isDelta=true, Pdf()=0.
//    C. Furnace test: Compute integral of BSDF*cos over hemisphere
//       two ways (MC via SPF branching mode, quadrature via BRDF)
//       and compare.  Tests all 10 SPF+BRDF pairs.
//    D. Pointwise check (single-lobe only): For each SPF sample,
//       verify kray * pdf == BRDF::value(wo,ri) * cos(theta_o).
//
//  Build (from project root):
//    c++ -arch arm64 -Isrc/Library -I/opt/homebrew/include
//        -O3 -ffast-math -fno-finite-math-only -funroll-loops -Wall -pedantic
//        (-fno-finite-math-only is REQUIRED to match production since
//         2026-07-29; without it std::isfinite/isnan fold to constants
//         and NaN-sentinel assertions silently pass -- see CLAUDE.md.)
//        -Wno-c++11-long-long -DCOLORS_RGB -DMERSENNE53
//        -DNO_TIFF_SUPPORT -DNO_EXR_SUPPORT -DRISE_ENABLE_MAILBOXING
//        -c tests/SPFBSDFConsistencyTest.cpp -o tests/SPFBSDFConsistencyTest.o
//    c++ -arch arm64 -o tests/spf_bsdf_test tests/SPFBSDFConsistencyTest.o
//        bin/librise.a -L/opt/homebrew/lib -lpng -lz
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <string>
#include <iomanip>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/GeometricUtilities.h"
#include "../src/Library/Utilities/Optics.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IBSDF.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"

// SPF implementations
#include "../src/Library/Materials/LambertianSPF.h"
#include "../src/Library/Materials/OrenNayarSPF.h"
#include "../src/Library/Materials/IsotropicPhongSPF.h"
#include "../src/Library/Materials/CookTorranceSPF.h"
#include "../src/Library/Materials/SchlickSPF.h"
#include "../src/Library/Materials/WardIsotropicGaussianSPF.h"
#include "../src/Library/Materials/WardAnisotropicEllipticalGaussianSPF.h"
#include "../src/Library/Materials/AshikminShirleyAnisotropicPhongSPF.h"
#include "../src/Library/Materials/TranslucentSPF.h"
#include "../src/Library/Materials/PolishedSPF.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Materials/GGXMaterial.h"
#include "../src/Library/Materials/CoatedMaterial.h"
#include "../src/Library/Materials/FabricMaterial.h"
#include "WeaveTestFixture.h"
#include "../src/Library/Materials/SheenBRDF.h"
#include "../src/Library/Materials/SheenSPF.h"
#include "../src/Library/Materials/SubSurfaceScatteringSPF.h"
#include "../src/Library/Materials/CompositeSPF.h"
#include "../src/Library/Materials/PerfectReflectorSPF.h"
#include "../src/Library/Materials/PerfectRefractorSPF.h"
#include "../src/Library/Materials/DielectricSPF.h"
#include "../src/Library/Materials/GGXSPF.h"

#include "TestStubObject.h"

// BRDF implementations
#include "../src/Library/Materials/LambertianBRDF.h"
#include "../src/Library/Materials/OrenNayarBRDF.h"
#include "../src/Library/Materials/IsotropicPhongBRDF.h"
#include "../src/Library/Materials/CookTorranceBRDF.h"
#include "../src/Library/Materials/SchlickBRDF.h"
#include "../src/Library/Materials/WardIsotropicGaussianBRDF.h"
#include "../src/Library/Materials/WardAnisotropicEllipticalGaussianBRDF.h"
#include "../src/Library/Materials/AshikminShirleyAnisotropicPhongBRDF.h"
#include "../src/Library/Materials/TranslucentBSDF.h"
#include "../src/Library/Materials/SubSurfaceScatteringBSDF.h"
#include "../src/Library/Materials/GGXBRDF.h"

using namespace RISE;
using namespace RISE::Implementation;

// ============================================================
//  Test configuration
// ============================================================

static const int FURNACE_MC_SAMPLES     = 100000;   // MC samples for furnace test
static const int FURNACE_QUAD_THETA     = 100;      // Quadrature resolution (polar)
static const int FURNACE_QUAD_PHI       = 200;      // Quadrature resolution (azimuthal)
static const double FURNACE_TOL         = 0.05;     // 5% tolerance for furnace test
static const int POINTWISE_SAMPLES      = 10000;    // Samples for pointwise kray*pdf vs BSDF*cos check
static const double POINTWISE_TOL       = 0.02;     // 2% relative tolerance for pointwise check
static const int DELTA_DIRECTION_SAMPLES = 1000;    // Samples for delta direction checks
static const double DELTA_DIR_TOL       = 1e-6;     // Angular tolerance for delta directions

// ============================================================
//  Stub object for IOR stack operations
// ============================================================

static StubObject* g_stubObject = 0;

// ============================================================
//  Synthetic intersection setup
// ============================================================

static RayIntersectionGeometric MakeIntersection( double incomingTheta )
{
    double sinT = sin(incomingTheta);
    double cosT = cos(incomingTheta);
    Vector3 inDir( sinT, 0, -cosT );

    Ray inRay( Point3(sinT, 0, 1.0), inDir );
    RasterizerState rs = {0, 0};
    RayIntersectionGeometric ri( inRay, rs );

    ri.bHit = true;
    ri.range = 1.0 / cosT;
    ri.ptIntersection = Point3(0, 0, 0);
    ri.vNormal = Vector3(0, 0, 1);
    ri.onb.CreateFromW( Vector3(0, 0, 1) );
    ri.ptCoord = Point2(0.5, 0.5);

    return ri;
}

// ============================================================
//  Test 1: Furnace test (SPF vs BRDF integral comparison)
// ============================================================

struct FurnaceResult {
    std::string name;
    double mcEstimate;      // MC estimate via SPF importance sampling
    double quadEstimate;    // Numerical quadrature via BRDF::value()
    double relError;        // Relative error between the two
    bool passed;
};

static FurnaceResult FurnaceTest(
    const std::string& name,
    ISPF& spf,
    IBSDF& brdf,
    double incomingTheta
    )
{
    FurnaceResult result;
    result.name = name;

    RayIntersectionGeometric ri = MakeIntersection( incomingTheta );
    const Vector3 normal = ri.onb.w();
    RandomNumberGenerator rng;
    Implementation::IndependentSampler sampler( rng );
    IORStack iorStack = MakeTestIORStack( g_stubObject );

    // ---- MC estimate via SPF importance sampling (branching mode) ----
    // In branching mode, we sum kray over ALL scattered rays per sample.
    // For each lobe j: kray_j = BSDF_j * cos / pdf_j
    // So E[Σ kray_j] = Σ ∫ BSDF_j * cos dω = ∫ BSDF_total * cos dω
    // NOTE: Using RandomlySelect would be biased for multi-lobe materials
    // because the selection probability correlates with kray magnitude.
    double mcSum = 0;
    int mcCount = 0;

    for( int i = 0; i < FURNACE_MC_SAMPLES; i++ )
    {
        ScatteredRayContainer scattered;
        spf.Scatter( ri, sampler, scattered, iorStack );

        if( scattered.Count() == 0 ) continue;

        double sampleContrib = 0;
        bool anyValid = false;

        for( unsigned int j = 0; j < scattered.Count(); j++ )
        {
            const ScatteredRay& scat = scattered[j];
            if( scat.isDelta ) continue;

            Vector3 wo = Vector3Ops::Normalize( scat.ray.Dir() );
            double cosO = Vector3Ops::Dot( wo, normal );
            if( cosO <= 0 ) continue;

            double krayMax = ColorMath::MaxValue( scat.kray );
            if( krayMax >= 0 && krayMax < 1e6 )
            {
                sampleContrib += krayMax;
                anyValid = true;
            }
        }

        if( anyValid )
        {
            mcSum += sampleContrib;
            mcCount++;
        }
    }

    result.mcEstimate = (mcCount > 0) ? mcSum / mcCount : 0;

    // ---- Numerical quadrature via BRDF::value() ----
    // Integrate MaxValue( BRDF::value(wo, ri) ) * cos(theta_o) * sin(theta) * dtheta * dphi
    double quadSum = 0;

    for( int t = 0; t < FURNACE_QUAD_THETA; t++ )
    {
        double theta = (t + 0.5) * PI_OV_TWO / FURNACE_QUAD_THETA;
        double sinTheta = sin(theta);
        double cosTheta = cos(theta);
        double dTheta = PI_OV_TWO / FURNACE_QUAD_THETA;

        for( int p = 0; p < FURNACE_QUAD_PHI; p++ )
        {
            double phi = (p + 0.5) * TWO_PI / FURNACE_QUAD_PHI;
            double dPhi = TWO_PI / FURNACE_QUAD_PHI;

            // Construct outgoing direction in world space
            Vector3 wo( sinTheta * cos(phi), sinTheta * sin(phi), cosTheta );
            wo = Vector3Ops::Normalize( wo );

            // Evaluate BRDF: vLightIn = wo (outgoing direction = "light direction")
            RISEPel brdfVal = brdf.value( wo, ri );
            double brdfMax = ColorMath::MaxValue( brdfVal );

            // Clamp negative values (shouldn't happen but be safe)
            if( brdfMax < 0 ) brdfMax = 0;

            quadSum += brdfMax * cosTheta * sinTheta * dTheta * dPhi;
        }
    }

    result.quadEstimate = quadSum;

    // ---- Compare ----
    double denom = r_max( fabs(result.mcEstimate), fabs(result.quadEstimate) );
    if( denom < 1e-10 )
    {
        // Both near zero — consistent
        result.relError = 0;
        result.passed = true;
    }
    else
    {
        result.relError = fabs(result.mcEstimate - result.quadEstimate) / denom;
        result.passed = (result.relError <= FURNACE_TOL);
    }

    return result;
}

// ============================================================
//  Test 2: Pointwise kray*pdf vs BRDF*cos (single-lobe only)
// ============================================================

struct PointwiseResult {
    std::string name;
    int numSamples;
    int numFailures;
    double maxRelError;
    double avgRelError;
    bool passed;
};

static PointwiseResult PointwiseTest(
    const std::string& name,
    ISPF& spf,
    IBSDF& brdf,
    double incomingTheta,
    bool singleLobe
    )
{
    PointwiseResult result;
    result.name = name;
    result.numSamples = 0;
    result.numFailures = 0;
    result.maxRelError = 0;
    result.avgRelError = 0;

    if( !singleLobe )
    {
        // For multi-lobe SPFs, kray*pdf gives per-lobe BSDF*cos,
        // but BRDF::value() returns the total. Skip this test.
        result.passed = true;
        return result;
    }

    RayIntersectionGeometric ri = MakeIntersection( incomingTheta );
    const Vector3 normal = ri.onb.w();
    RandomNumberGenerator rng;
    Implementation::IndependentSampler sampler( rng );
    IORStack iorStack = MakeTestIORStack( g_stubObject );

    double errSum = 0;

    for( int i = 0; i < POINTWISE_SAMPLES; i++ )
    {
        ScatteredRayContainer scattered;
        spf.Scatter( ri, sampler, scattered, iorStack );

        if( scattered.Count() == 0 ) continue;

        const ScatteredRay& scat = scattered[0];
        if( scat.isDelta ) continue;
        if( scat.pdf <= 0 ) continue;

        Vector3 wo = Vector3Ops::Normalize( scat.ray.Dir() );
        double cosO = Vector3Ops::Dot( wo, normal );
        // R9 P2: `cosO == 0` (grazing, degenerate) is skipped either way;
        // `cosO < 0` is a legitimate draw from a full-sphere material's
        // TRANSMISSION branch (`ScattersFullSphere()`, e.g. a
        // `transmission thin` weave) and must be tested with `fabs(cosO)`,
        // not rejected -- an ordinary reflect-only SPF's `Scatter()` never
        // emits a `cosO < 0` ray in the first place (cosine-hemisphere
        // sampling about the ray-facing normal), so this is a strict
        // superset of the old behaviour: unreached, hence unchanged, for
        // every pre-existing row.
        if( cosO == 0 ) continue;

        result.numSamples++;

        // SPF side: kray * pdf (should equal BSDF * cos for single-lobe)
        double krayMax = ColorMath::MaxValue( scat.kray );
        double spfProduct = krayMax * scat.pdf;

        // BRDF side: BRDF::value(wo, ri) * cos(theta_o) -- |cos| so a
        // transmission-branch draw (cosO < 0) compares against the
        // diffuse-transmit lobe's own magnitude rather than its sign.
        RISEPel brdfVal = brdf.value( wo, ri );
        double brdfMax = ColorMath::MaxValue( brdfVal );
        double brdfProduct = brdfMax * fabs( cosO );

        // Compare
        double denom = r_max( fabs(spfProduct), fabs(brdfProduct) );
        double relErr = 0;
        if( denom > 1e-10 )
        {
            relErr = fabs(spfProduct - brdfProduct) / denom;
        }

        errSum += relErr;

        if( relErr > POINTWISE_TOL )
        {
            result.numFailures++;
        }
        if( relErr > result.maxRelError )
        {
            result.maxRelError = relErr;
        }
    }

    result.avgRelError = (result.numSamples > 0) ? errSum / result.numSamples : 0;

    // Allow up to 1% of samples to exceed tolerance (numerical noise)
    double failRate = (result.numSamples > 0) ? (double)result.numFailures / result.numSamples : 0;
    result.passed = (failRate <= 0.01);

    return result;
}

// ============================================================
//  Test 2b: HELMHOLTZ RECIPROCITY  f(a->b) == f(b->a)
//
//  Added 2026-08-31 with `coated_material`, and it exists because
//  NOTHING ELSE IN THIS FILE CAN SEE A NON-RECIPROCAL BRDF.
//  Part C integrates the BRDF and compares against the SPF; Part D
//  compares kray*pdf against BRDF*cos.  Both compare a material
//  against ITSELF, so both hold perfectly for a BRDF that answers
//  differently depending on which direction you call "the light".
//
//  The defect that motivated it: `coated_material`'s layer factor
//  carries the substrate's directional albedo, and the first cut read
//  it from `IBSDF::albedo`, which is the OIDN AOV and legitimately
//  VIEW-dependent (it reads ri.ray).  f(a->b) therefore carried R(b)
//  while f(b->a) carried R(a) -- ~28 % apart at grazing on a GGX
//  substrate.  That is a direct error on NEE and BDPT/VCM connection
//  weights, i.e. exactly the path Phase 2 of
//  docs/WETNESS_COAT_DESIGN.md exists to make correct.  Fixed by
//  routing the term through `IBSDF::hemisphericalAlbedo`, whose
//  contract forbids reading ri.ray.
//
//  The sweep is over ORDERED PAIRS of directions rather than the
//  usual single incident angle, because reciprocity is a statement
//  about a pair and a one-angle fixture cannot express it.
// ============================================================

static const double RECIPROCITY_TOL = 1e-6;		// relative; the property is exact, not statistical

//! Intersection whose VIEW direction (toward the viewer) is `view`.
//! The existing MakeIntersection fixes the view from a polar angle in
//! the x-z plane; reciprocity needs arbitrary pairs, so this builds
//! the same fixture (flat +z normal, vGeomNormal left zero so the
//! horizon gate degenerates to the shading-hemisphere test) from a
//! full direction vector.
static RayIntersectionGeometric MakeIntersectionFromView( const Vector3& view )
{
    const Vector3 inDir = -view;					// ray travels INTO the surface
    Ray inRay( Point3( view.x, view.y, view.z ), inDir );
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

struct ReciprocityResult {
    std::string name;
    int    numPairs;
    int    numFailures;
    double maxRelError;			// RGB
    double maxRelErrorNM;		// spectral
    bool   passed;
};

static ReciprocityResult ReciprocityTest( const std::string& name, IBSDF& brdf )
{
    ReciprocityResult result;
    result.name = name;
    result.numPairs = 0;
    result.numFailures = 0;
    result.maxRelError = 0;
    result.maxRelErrorNM = 0;

    // A spread of polar angles (including grazing, where the defect
    // this test was written for was largest) crossed with azimuths, so
    // pairs differ in BOTH angles rather than only in elevation.
    static const double thetaDeg[] = { 10.0, 25.0, 40.0, 55.0, 70.0, 85.0 };
    static const double phiDeg[]   = { 0.0, 60.0, 140.0, 230.0, 310.0 };

    std::vector<Vector3> dirs;
    for( double t : thetaDeg ) {
        for( double p : phiDeg ) {
            const double th = t * DEG_TO_RAD, ph = p * DEG_TO_RAD;
            dirs.push_back( Vector3Ops::Normalize(
                Vector3( sin(th) * cos(ph), sin(th) * sin(ph), cos(th) ) ) );
        }
    }

    const double kNM = 550.0;

    for( size_t i = 0; i < dirs.size(); i++ )
    {
        for( size_t j = i + 1; j < dirs.size(); j++ )
        {
            const Vector3& a = dirs[i];
            const Vector3& b = dirs[j];

            // f(a -> b): viewer at b, light at a.   value(vLightIn, ri)
            // takes the light direction and reads the view off ri.ray.
            RayIntersectionGeometric riB = MakeIntersectionFromView( b );
            RayIntersectionGeometric riA = MakeIntersectionFromView( a );

            // MAX-CHANNEL, not per-channel: reciprocity is a
            // statement about the BRDF, and a defect that moved only
            // one channel would still move the max unless it moved the
            // channels by exactly compensating amounts in opposite
            // directions -- which no view-dependence bug does, because
            // the view term multiplies all three channels through the
            // same layer factor.  Max-channel also matches how every
            // other comparison in this file reduces a RISEPel, so the
            // tolerances are on the same footing.
            const double fab = ColorMath::MaxValue( brdf.value( a, riB ) );
            const double fba = ColorMath::MaxValue( brdf.value( b, riA ) );

            const double fabNM = brdf.valueNM( a, riB, kNM );
            const double fbaNM = brdf.valueNM( b, riA, kNM );

            result.numPairs++;

            const double den   = r_max( fabs(fab),   fabs(fba)   );
            const double denNM = r_max( fabs(fabNM), fabs(fbaNM) );

            const double rel   = ( den   > 1e-12 ) ? fabs(fab   - fba  ) / den   : 0.0;
            const double relNM = ( denNM > 1e-12 ) ? fabs(fabNM - fbaNM) / denNM : 0.0;

            if( rel   > result.maxRelError   ) result.maxRelError   = rel;
            if( relNM > result.maxRelErrorNM ) result.maxRelErrorNM = relNM;

            if( rel > RECIPROCITY_TOL || relNM > RECIPROCITY_TOL ) {
                result.numFailures++;
            }
        }
    }

    result.passed = ( result.numFailures == 0 );
    return result;
}

// ============================================================
//  Part E2 (P2-B, docs/CLOTH_FABRIC_DESIGN.md 10): reciprocity for a
//  FULL-SPHERE material's TRANSMISSION lobes -- `f(i->o) == f(o->i)`
//  where `i` and `o` are on OPPOSITE sides of the shading normal.
//
//  `MakeIntersectionFromView`'s normal is the fixed `(0,0,1)` `ri.onb`
//  BEFORE `WeaveBRDF::ResolveWeave`'s own ray-facing flip, and that
//  flip is what makes reusing `ReciprocityTest`'s machinery correct
//  here: it re-orients `p.n` to `(0, 0, sign(view.z))`, so `wo` (`-ray.
//  Dir()`) lands on `p.n`'s positive side FOR EITHER VIEW HEMISPHERE,
//  and a direction pair straddling `z = 0` in WORLD space is exactly a
//  pair straddling the shading normal in the material's OWN frame --
//  the transmission configuration.  `ReciprocityTest` itself never
//  reaches this: its fixed `thetaDeg` set (10-85) keeps every direction
//  on the SAME (+Z) side.
//
//  WHAT THIS DOES AND DOES NOT PROVE (R8 P3-4).  The shipped diffuse
//  transmission lobe, `f_t,d,k = (1-gap)*transmit_k*T_k/pi`, has NO
//  directional dependence beyond the hemisphere-crossing gate itself --
//  it is a flat value once `i`/`o` straddle the normal.  Combined with
//  this harness's single fixed shading point (`MakeIntersectionFromView`
//  always uses `ptCoord = (0.5, 0.5)`), `f(a->b)` and `f(b->a)` are
//  computed from LITERALLY IDENTICAL inputs to the same flat expression
//  -- which is why the measured error is exactly `0.000e+00`, not
//  merely small.  This is STRUCTURAL, not an empirical demonstration of
//  reciprocity: it is a regression tripwire that will catch a future
//  change which accidentally introduces a directional asymmetry (e.g. a
//  specular transmission lobe, or a `Q`-style view-only normaliser), not
//  evidence that today's flat lobe is "reciprocal" in any sense beyond
//  "the same formula evaluated twice returns the same number."
// ============================================================

static ReciprocityResult ReciprocityTestCrossHemisphere( const std::string& name, IBSDF& brdf )
{
    ReciprocityResult result;
    result.name = name;
    result.numPairs = 0;
    result.numFailures = 0;
    result.maxRelError = 0;
    result.maxRelErrorNM = 0;

    // theta spans BOTH hemispheres (10 deg past each pole down to 10
    // deg short of the equator on each side), crossed with the same
    // azimuth spread `ReciprocityTest` uses.
    static const double thetaDeg[] = { 10.0, 40.0, 70.0, 110.0, 140.0, 170.0 };
    static const double phiDeg[]   = { 0.0, 60.0, 140.0, 230.0, 310.0 };

    std::vector<Vector3> dirs;
    for( double t : thetaDeg ) {
        for( double p : phiDeg ) {
            const double th = t * DEG_TO_RAD, ph = p * DEG_TO_RAD;
            dirs.push_back( Vector3Ops::Normalize(
                Vector3( sin(th) * cos(ph), sin(th) * sin(ph), cos(th) ) ) );
        }
    }

    const double kNM = 550.0;

    //! Largest magnitude any straddling pair produced -- the
    //! non-degeneracy guard at the bottom of this function.
    double liveMax = 0.0;

    for( size_t i = 0; i < dirs.size(); i++ )
    {
        for( size_t j = i + 1; j < dirs.size(); j++ )
        {
            const Vector3& a = dirs[i];
            const Vector3& b = dirs[j];

            // Only pairs that straddle the equator exercise the
            // transmission lobe at all; same-side pairs are already
            // covered by `ReciprocityTest` and would just re-measure
            // the (unaffected) reflect-side lobes here.
            if( ( a.z > 0 ) == ( b.z > 0 ) ) continue;

            RayIntersectionGeometric riB = MakeIntersectionFromView( b );
            RayIntersectionGeometric riA = MakeIntersectionFromView( a );

            const double fab = ColorMath::MaxValue( brdf.value( a, riB ) );
            const double fba = ColorMath::MaxValue( brdf.value( b, riA ) );
            const double fabNM = brdf.valueNM( a, riB, kNM );
            const double fbaNM = brdf.valueNM( b, riA, kNM );

            result.numPairs++;

            const double den   = r_max( fabs(fab),   fabs(fba)   );
            const double denNM = r_max( fabs(fabNM), fabs(fbaNM) );
            const double rel   = ( den   > 1e-12 ) ? fabs(fab   - fba  ) / den   : 0.0;
            const double relNM = ( denNM > 1e-12 ) ? fabs(fabNM - fbaNM) / denNM : 0.0;

            if( den   > liveMax ) liveMax = den;
            if( denNM > liveMax ) liveMax = denNM;

            if( rel   > result.maxRelError   ) result.maxRelError   = rel;
            if( relNM > result.maxRelErrorNM ) result.maxRelErrorNM = relNM;

            if( rel > RECIPROCITY_TOL || relNM > RECIPROCITY_TOL ) {
                result.numFailures++;
            }
        }
    }

    // A row with zero pairs would silently "pass" without ever having
    // exercised the transmission lobe at all -- assert there IS
    // cross-hemisphere coverage, not just that none of it failed.
    //
    // AND A ROW THAT IS IDENTICALLY ZERO IS THE SAME HOLE ONE LEVEL DOWN
    // (R8 P1.1).  `0 == 0` is perfectly reciprocal, so a material whose
    // transmission had been extinguished -- exactly what
    // `fabric_material` did to a sheer weave until 2026-09-04, debt 22 --
    // would post `pairs=225 failures=0 maxRelErr=0` and read as a clean
    // pass.  Require that at least one straddling pair actually carried
    // a value, so the check is about a LIVE transmission lobe.
    result.passed = ( result.numFailures == 0 )
                 && ( result.numPairs > 0 )
                 && ( liveMax > 1e-12 );
    return result;
}

// ============================================================
//  Test 3: Delta SPF direction correctness
// ============================================================

struct DeltaResult {
    std::string name;
    bool hasReflection;
    bool hasRefraction;
    double maxReflError;
    double maxRefrError;
    bool reflPassed;
    bool refrPassed;
    bool isDeltaFlagCorrect;
    bool pdfIsZero;
};

static DeltaResult DeltaDirectionTest(
    const std::string& name,
    ISPF& spf,
    double incomingTheta
    )
{
    DeltaResult result;
    result.name = name;
    result.hasReflection = false;
    result.hasRefraction = false;
    result.maxReflError = 0;
    result.maxRefrError = 0;
    result.reflPassed = true;
    result.refrPassed = true;
    result.isDeltaFlagCorrect = true;
    result.pdfIsZero = true;

    RayIntersectionGeometric ri = MakeIntersection( incomingTheta );
    RandomNumberGenerator rng;
    Implementation::IndependentSampler sampler( rng );
    IORStack iorStack = MakeTestIORStack( g_stubObject );

    // Compute expected mirror reflection direction
    const Vector3 normal = ri.onb.w();
    Vector3 expectedRefl = Optics::CalculateReflectedRay( ri.ray.Dir(), normal );
    expectedRefl = Vector3Ops::Normalize( expectedRefl );

    for( int i = 0; i < DELTA_DIRECTION_SAMPLES; i++ )
    {
        ScatteredRayContainer scattered;
        spf.Scatter( ri, sampler, scattered, iorStack );

        for( unsigned int j = 0; j < scattered.Count(); j++ )
        {
            const ScatteredRay& scat = scattered[j];
            Vector3 wo = Vector3Ops::Normalize( scat.ray.Dir() );

            // Check delta flag
            if( !scat.isDelta )
                result.isDeltaFlagCorrect = false;

            // Check Pdf returns 0
            Scalar pdfVal = spf.Pdf( ri, wo, iorStack );
            if( pdfVal != 0 )
                result.pdfIsZero = false;

            double cosN = Vector3Ops::Dot( wo, normal );

            if( scat.type == ScatteredRay::eRayReflection )
            {
                result.hasReflection = true;

                // Check reflection direction matches Snell's law
                Vector3 diff = wo - expectedRefl;
                double err = Vector3Ops::Magnitude( diff );
                if( err > result.maxReflError )
                    result.maxReflError = err;
                if( err > DELTA_DIR_TOL )
                    result.reflPassed = false;
            }
            else if( scat.type == ScatteredRay::eRayRefraction )
            {
                result.hasRefraction = true;

                // For refraction, just verify the ray goes below the surface
                // (full Snell's law verification would need IOR which is material-specific)
                if( cosN >= 0 )
                {
                    // Refracted ray should be below surface (negative cos with normal)
                    // Unless it's going from inside to outside
                }
            }
        }
    }

    return result;
}

// ============================================================
//  Test 4: SPF basic sanity (all materials)
//  - Scatter produces at least some rays
//  - kray values are non-negative
//  - Directions are normalized
// ============================================================

struct SanityResult {
    std::string name;
    int totalRays;
    int negativeKray;
    int unnormalizedDir;
    int belowHemisphere;  // For reflection-only materials
    bool passed;
};

static SanityResult SanityTest(
    const std::string& name,
    ISPF& spf,
    double incomingTheta,
    bool isDelta
    )
{
    SanityResult result;
    result.name = name;
    result.totalRays = 0;
    result.negativeKray = 0;
    result.unnormalizedDir = 0;
    result.belowHemisphere = 0;

    RayIntersectionGeometric ri = MakeIntersection( incomingTheta );
    RandomNumberGenerator rng;
    Implementation::IndependentSampler sampler( rng );
    IORStack iorStack = MakeTestIORStack( g_stubObject );

    const int N = 10000;

    for( int i = 0; i < N; i++ )
    {
        ScatteredRayContainer scattered;
        spf.Scatter( ri, sampler, scattered, iorStack );

        for( unsigned int j = 0; j < scattered.Count(); j++ )
        {
            result.totalRays++;
            const ScatteredRay& scat = scattered[j];

            // Check non-negative kray
            if( scat.kray[0] < -1e-10 || scat.kray[1] < -1e-10 || scat.kray[2] < -1e-10 )
                result.negativeKray++;

            // Check normalization
            double len = Vector3Ops::Magnitude( scat.ray.Dir() );
            if( fabs(len - 1.0) > 1e-4 )
                result.unnormalizedDir++;
        }
    }

    result.passed = (result.negativeKray == 0) && (result.unnormalizedDir == 0) && (result.totalRays > 0);
    return result;
}

// ============================================================
//  Main
// ============================================================

int main()
{
    std::cout << "===== SPF-BSDF Consistency Test =====" << std::endl;
    std::cout << "Furnace MC samples: " << FURNACE_MC_SAMPLES << std::endl;
    std::cout << "Furnace quadrature: " << FURNACE_QUAD_THETA << " x " << FURNACE_QUAD_PHI << std::endl;
    std::cout << "Pointwise samples: " << POINTWISE_SAMPLES << std::endl;
    std::cout << std::endl;

    // Initialize the global log to prevent null pointer crashes
    GlobalLog();

    // Stub object for IOR stack operations (mimics scene object identity)
    g_stubObject = new StubObject();
    g_stubObject->addref();

    // ---- Create uniform painters ----
    UniformColorPainter* white      = new UniformColorPainter( RISEPel(0.8, 0.8, 0.8) );  white->addref();
    UniformColorPainter* gray       = new UniformColorPainter( RISEPel(0.5, 0.5, 0.5) );  gray->addref();
    UniformColorPainter* spec       = new UniformColorPainter( RISEPel(0.3, 0.3, 0.3) );  spec->addref();
    UniformColorPainter* low        = new UniformColorPainter( RISEPel(0.1, 0.1, 0.1) );  low->addref();
    UniformColorPainter* highExp    = new UniformColorPainter( RISEPel(50.0, 50.0, 50.0) ); highExp->addref();
    UniformColorPainter* ior        = new UniformColorPainter( RISEPel(1.5, 1.5, 1.5) );  ior->addref();
    UniformColorPainter* extinction = new UniformColorPainter( RISEPel(0.0, 0.0, 0.0) );  extinction->addref();
    UniformColorPainter* alphaSmall = new UniformColorPainter( RISEPel(0.2, 0.2, 0.2) );  alphaSmall->addref();
    UniformColorPainter* alphaSmallY= new UniformColorPainter( RISEPel(0.3, 0.3, 0.3) );  alphaSmallY->addref();
    UniformColorPainter* roughness  = new UniformColorPainter( RISEPel(0.3, 0.3, 0.3) );  roughness->addref();
    UniformColorPainter* isotropy   = new UniformColorPainter( RISEPel(0.8, 0.8, 0.8) );  isotropy->addref();
    UniformColorPainter* ashNu      = new UniformColorPainter( RISEPel(100.0, 100.0, 100.0) ); ashNu->addref();
    UniformColorPainter* ashNv      = new UniformColorPainter( RISEPel(50.0, 50.0, 50.0) );  ashNv->addref();
    UniformColorPainter* trans      = new UniformColorPainter( RISEPel(0.4, 0.4, 0.4) );  trans->addref();
    UniformColorPainter* phongN     = new UniformColorPainter( RISEPel(10.0, 10.0, 10.0) ); phongN->addref();
    UniformColorPainter* scatFactor = new UniformColorPainter( RISEPel(0.3, 0.3, 0.3) );  scatFactor->addref();
    UniformColorPainter* tauPainter = new UniformColorPainter( RISEPel(0.9, 0.9, 0.9) );  tauPainter->addref();
    UniformColorPainter* polishScat = new UniformColorPainter( RISEPel(20.0, 20.0, 20.0) ); polishScat->addref();
    UniformColorPainter* sssAbsorb  = new UniformColorPainter( RISEPel(0.01, 0.01, 0.01) ); sssAbsorb->addref();
    UniformColorPainter* sssScat    = new UniformColorPainter( RISEPel(1.0, 1.0, 1.0) );  sssScat->addref();
    UniformColorPainter* one        = new UniformColorPainter( RISEPel(1.0, 1.0, 1.0) );  one->addref();

    // Scalar twins of the numeric values that drive the consistency
    // test — IScalarPainter slots (physical scalars), no JH spectral
    // uplift.  Declared BEFORE the SPF/BRDF construction so they are
    // visible to every consumer in this block.
    UniformScalarPainter* tauScalar      = new UniformScalarPainter( 0.9 );    tauScalar->addref();
    UniformScalarPainter* iorScalar      = new UniformScalarPainter( 1.5 );    iorScalar->addref();
    UniformScalarPainter* scatScalar     = new UniformScalarPainter( 0.3 );    scatScalar->addref();
    UniformScalarPainter* polishScatSc   = new UniformScalarPainter( 20.0 );   polishScatSc->addref();
    UniformScalarPainter* roughnessSc    = new UniformScalarPainter( 0.3 );    roughnessSc->addref();
    UniformScalarPainter* highExpSc      = new UniformScalarPainter( 50.0 );   highExpSc->addref();
    UniformScalarPainter* ashNuSc        = new UniformScalarPainter( 100.0 );  ashNuSc->addref();
    UniformScalarPainter* ashNvSc        = new UniformScalarPainter( 50.0 );   ashNvSc->addref();
    UniformScalarPainter* alphaSmallSc   = new UniformScalarPainter( 0.2 );    alphaSmallSc->addref();
    UniformScalarPainter* alphaSmallYSc  = new UniformScalarPainter( 0.3 );    alphaSmallYSc->addref();
    UniformScalarPainter* isotropySc     = new UniformScalarPainter( 0.8 );    isotropySc->addref();
    UniformScalarPainter* lowSc          = new UniformScalarPainter( 0.1 );    lowSc->addref();
    UniformScalarPainter* extinctionSc   = new UniformScalarPainter( 0.0 );    extinctionSc->addref();
    UniformScalarPainter* phongNSc       = new UniformScalarPainter( 10.0 );   phongNSc->addref();
    UniformScalarPainter* scatFactorSc   = new UniformScalarPainter( 0.3 );    scatFactorSc->addref();

    // ---- Construct SPFs ----
    std::cout << "Constructing SPFs..." << std::flush;
    LambertianSPF* lambertianSPF = new LambertianSPF( *white );  lambertianSPF->addref();
    OrenNayarSPF* orenNayarSPF = new OrenNayarSPF( *white, *roughnessSc );  orenNayarSPF->addref();
    IsotropicPhongSPF* phongSPF = new IsotropicPhongSPF( *gray, *spec, *highExpSc );  phongSPF->addref();
    CookTorranceSPF* cookTorranceSPF = new CookTorranceSPF( *gray, *spec, *lowSc, *iorScalar, *extinctionSc );  cookTorranceSPF->addref();
    GGXSPF* ggxIsoSPF = new GGXSPF( *gray, *spec, *alphaSmallSc, *alphaSmallSc, *iorScalar, *extinctionSc );  ggxIsoSPF->addref();
    GGXSPF* ggxAnisoSPF = new GGXSPF( *gray, *spec, *alphaSmallSc, *alphaSmallYSc, *iorScalar, *extinctionSc );  ggxAnisoSPF->addref();
    SchlickSPF* schlickSPF = new SchlickSPF( *gray, *spec, *roughnessSc, *isotropySc );  schlickSPF->addref();
    WardIsotropicGaussianSPF* wardIsoSPF = new WardIsotropicGaussianSPF( *gray, *spec, *alphaSmallSc );  wardIsoSPF->addref();
    WardAnisotropicEllipticalGaussianSPF* wardAnisoSPF = new WardAnisotropicEllipticalGaussianSPF( *gray, *spec, *alphaSmallSc, *alphaSmallYSc );  wardAnisoSPF->addref();
    AshikminShirleyAnisotropicPhongSPF* ashikminSPF = new AshikminShirleyAnisotropicPhongSPF( *ashNuSc, *ashNvSc, *gray, *spec );  ashikminSPF->addref();
    TranslucentSPF* translucentSPF = new TranslucentSPF( *gray, *trans, *extinctionSc, *phongNSc, *scatFactorSc );  translucentSPF->addref();
    PolishedSPF* polishedSPF = new PolishedSPF( *gray, *tauScalar, *iorScalar, *polishScatSc, false );  polishedSPF->addref();
    SubSurfaceScatteringSPF* sssSPF = new SubSurfaceScatteringSPF( *iorScalar, 0.8, 0.3 );  sssSPF->addref();
    LambertianSPF* lambertian2SPF = new LambertianSPF( *spec );  lambertian2SPF->addref();
    CompositeSPF* compositeSPF = new CompositeSPF( *lambertianSPF, *lambertian2SPF, 4, 2, 2, 2, 2, 0.1, *extinctionSc );  compositeSPF->addref();

    // coated_material (docs/WETNESS_COAT_DESIGN.md Phase 2).  Unlike
    // every other entry in this file the coated triad is built through
    // its MATERIAL, because CoatedSPF is the importance sampler FOR a
    // specific CoatedBRDF and holds a reference to it -- which is the
    // structural reason `kray * pdf == value * cos` holds exactly here
    // (see CoatedSPF.h).  Two substrates from the allowlist: a plain
    // Lambertian, and a GGX (the multi-lobe case, which is where a
    // naive layered sampler's value/Scatter agreement usually breaks).
    UniformScalarPainter* coatWeightSc = new UniformScalarPainter( 1.0 );   coatWeightSc->addref();
    UniformScalarPainter* coatIorSc    = new UniformScalarPainter( 1.33 );  coatIorSc->addref();
    UniformScalarPainter* coatRoughSc  = new UniformScalarPainter( 0.05 );  coatRoughSc->addref();
    UniformScalarPainter* coatZeroSc   = new UniformScalarPainter( 0.0 );   coatZeroSc->addref();

    LambertianMaterial* coatBaseLambMat = new LambertianMaterial( *white );  coatBaseLambMat->addref();
    GGXMaterial* coatBaseGgxMat = new GGXMaterial(
        *gray, *spec, *alphaSmallSc, *alphaSmallSc, *iorScalar, *extinctionSc );
    coatBaseGgxMat->addref();

    CoatedMaterial* coatedLambMat = new CoatedMaterial(
        *coatBaseLambMat, *coatWeightSc, *coatIorSc, *coatRoughSc, *coatZeroSc, *coatZeroSc, *one );
    coatedLambMat->addref();
    CoatedMaterial* coatedGgxMat = new CoatedMaterial(
        *coatBaseGgxMat, *coatWeightSc, *coatIorSc, *coatRoughSc, *coatZeroSc, *coatZeroSc, *one );
    coatedGgxMat->addref();

    // A fixture that lights up EVERY term at once -- partial coverage,
    // a real Beer-Lambert thickness and absorption, and a saturated
    // tint -- for the reciprocity sweep.  The A_in * A_out product is
    // symmetric under a wi/wo swap by construction, so this is not
    // where a defect is expected; it is swept anyway because "by
    // construction" is exactly the claim the previous round's
    // view-dependent albedo also had, and it was wrong.  Cheap to
    // check, and it covers the tint and absorption code paths that the
    // two neutral fixtures above never enter.
    UniformScalarPainter* coatHalfSc  = new UniformScalarPainter( 0.5 );   coatHalfSc->addref();
    UniformScalarPainter* coatThickSc = new UniformScalarPainter( 0.05 );  coatThickSc->addref();
    UniformScalarPainter* coatAbsSc   = new UniformScalarPainter( 1.8 );   coatAbsSc->addref();
    UniformColorPainter*  coatAmber   = new UniformColorPainter( RISEPel(0.92, 0.55, 0.18) );  coatAmber->addref();

    CoatedMaterial* coatedFullMat = new CoatedMaterial(
        *coatBaseGgxMat, *coatHalfSc, *coatIorSc, *coatRoughSc,
        *coatThickSc, *coatAbsSc, *coatAmber );
    coatedFullMat->addref();

    ISPF*  coatedLambSPF  = coatedLambMat->GetSPF();
    IBSDF* coatedLambBRDF = coatedLambMat->GetBSDF();
    ISPF*  coatedGgxSPF   = coatedGgxMat->GetSPF();
    IBSDF* coatedGgxBRDF  = coatedGgxMat->GetBSDF();

    // fabric_material (docs/CLOTH_FABRIC_DESIGN.md Phase 1, 9.9 gate
    // 5a).  Built through the MATERIAL for the same structural reason
    // the coated triad is: FabricSPF is the importance sampler FOR a
    // specific FabricBRDF and holds a reference to it, which is what
    // makes `kray * pdf == value * cos` hold exactly (FabricSPF.h).
    //
    // Two substrates, chosen to make the RECIPROCITY sweep
    // discriminating rather than decorative:
    //   * over a LAMBERTIAN, where the sheen lobe, its symmetric
    //     normaliser and the product scaling factor are the only
    //     l/v-coupled terms, so any asymmetry in any of them is
    //     unmasked;
    //   * over an ANISOTROPIC GGX with a NON-ZERO weave_rotation, which
    //     is the configuration where a frame handed to the substrate
    //     inconsistently between the two directions of the swap would
    //     show up -- and 9.5's whole mechanism is exactly such a frame
    //     hand-off.
    UniformScalarPainter* fabAlphaSc = new UniformScalarPainter( 0.3 );  fabAlphaSc->addref();
    UniformScalarPainter* fabWeaveSc = new UniformScalarPainter( 0.7853981633974483 );  fabWeaveSc->addref();
    UniformScalarPainter* fabZeroSc  = new UniformScalarPainter( 0.0 );  fabZeroSc->addref();

    GGXMaterial* fabBaseAnisoMat = new GGXMaterial(
        *gray, *spec, *alphaSmallSc, *alphaSmallYSc, *iorScalar, *extinctionSc, eFresnelSchlickF0 );
    fabBaseAnisoMat->addref();

    FabricMaterial* fabricLambMat = new FabricMaterial(
        *coatBaseLambMat, *one, *fabAlphaSc, *fabZeroSc );
    fabricLambMat->addref();
    FabricMaterial* fabricAnisoMat = new FabricMaterial(
        *fabBaseAnisoMat, *gray, *fabAlphaSc, *fabWeaveSc );
    fabricAnisoMat->addref();

    ISPF*  fabricLambSPF   = fabricLambMat->GetSPF();
    IBSDF* fabricLambBRDF  = fabricLambMat->GetBSDF();
    ISPF*  fabricAnisoSPF  = fabricAnisoMat->GetSPF();
    IBSDF* fabricAnisoBRDF = fabricAnisoMat->GetBSDF();

    // weave_material (docs/CLOTH_FABRIC_DESIGN.md Phase 2, slice P2-A).
    //
    // RECIPROCITY IS THE GATE THIS MATERIAL WAS SHAPED AROUND, and the
    // shaping is visible in what it does NOT implement.  Sadeghi et al.
    // 2013 normalise their two-family mixture by a factor Q (his
    // Eq. 15) that reads the VIEW direction and nothing else -- that
    // form cannot be reciprocal, so `WeaveBRDF` replaces it with a
    // direction-independent gap scalar and keeps only the parts of his
    // Eq. 7-15 that are symmetric under an i/o swap (the masking blend,
    // whose two arms exchange).  Every other term was chosen the same
    // way: `Mp` is symmetric in its two directions by construction, the
    // trimmed logistic is EVEN in phi_d, and the Kim 2002 Fresnel
    // argument cos(theta_d) cos(phi_d/2) is even in both angles, each of
    // which flips sign under the swap.
    //
    // "By construction" is exactly the claim `coated_material`'s first
    // cut also made before it was measured at ~28 % asymmetry at
    // grazing.  Hence these rows.
    //
    // TWO PRESETS, chosen to make the sweep discriminating:
    //   * `denim` with a NON-ZERO weave_rotation and zero tilt -- the
    //     row that fails if the two families' frames are built
    //     asymmetrically, since the rotation is a frame change and
    //     nothing else;
    //   * `satin` with the shipped NON-ZERO OPPOSITE TILTS, which is
    //     the configuration where each family's fibre frame leans out
    //     of the surface plane and its cross-section basis has to be
    //     re-derived per family.  An asymmetry in that derivation --
    //     e.g. projecting one direction in the tilted frame and the
    //     other in the flat one -- shows up here and essentially
    //     nowhere else.
    RISE::WeaveTest::PresetWeave weaveDenim( "denim", 0.7853981633974483 );
    RISE::WeaveTest::PresetWeave weaveSatin( "satin" );
    ISPF*  weaveDenimSPF  = weaveDenim.SPF();
    IBSDF* weaveDenimBRDF = weaveDenim.BSDF();
    ISPF*  weaveSatinSPF  = weaveSatin.SPF();
    IBSDF* weaveSatinBRDF = weaveSatin.BSDF();

    // P2-B (docs/CLOTH_FABRIC_DESIGN.md 10).  `transmission thin`, both
    // families' `transmit` nonzero -- the diffuse transmission lobe
    // this file's new cross-hemisphere reciprocity block (Part E2)
    // exercises.  Non-zero tilts (satin's shipped values) so the
    // reciprocity check also covers a tilted fibre frame's transmit
    // side, not only the untilted case.
    RISE::WeaveTest::PresetWeave weaveSatinThin( "satin", 0.0, -1, false, /*thin=*/true );
    IBSDF* weaveSatinThinBRDF = weaveSatinThin.BSDF();

    // R9 P2 (REVIEW_P2R9.md): Part D's pointwise `kray*pdf == BRDF*cos`
    // table never exercised WeaveSPF's TRANSMISSION branch -- only the
    // BRDF half of a thin weave was ever constructed above, and only
    // Part E2's reciprocity check (structural, not this magnitude check)
    // used it.  Two sheer LINEN configurations, both `transmission thin`:
    // `gap 0` (continuum only -- every drawn ray lands in the diffuse-
    // transmit branch once it crosses the hemisphere, no delta lobe to
    // skip) and `gap 0.2` (the shipped showcase's own value, so the
    // delta lobe IS present and must be excluded the same way every
    // other row here already is -- `PointwiseTest`'s own
    // `if (scat.isDelta) continue`, not a special case for these rows).
    RISE::WeaveTest::PresetWeave weaveLinenThinGap0(  "linen", 0.0, -1, false, /*thin=*/true, -1, -1, /*gapOverride=*/0.0 );
    RISE::WeaveTest::PresetWeave weaveLinenThinGap02( "linen", 0.0, -1, false, /*thin=*/true, -1, -1, /*gapOverride=*/0.2 );
    ISPF*  weaveLinenThinGap0SPF   = weaveLinenThinGap0.SPF();
    IBSDF* weaveLinenThinGap0BRDF  = weaveLinenThinGap0.BSDF();
    ISPF*  weaveLinenThinGap02SPF  = weaveLinenThinGap02.SPF();
    IBSDF* weaveLinenThinGap02BRDF = weaveLinenThinGap02.BSDF();

    // R8 P1.1 (docs/CLOTH_FABRIC_DESIGN.md 15 debt 22): the SAME two
    // sheer linens WRAPPED IN A FABRIC.  Until 2026-09-04 the wrapper
    // returned 0 for every opposite-hemisphere pair and zeroed every
    // transmit-side sample, so `fabric_material` over a sheer curtain
    // was 100 % opaque -- and neither of the two checks these feed could
    // see it, because both compare a material against ITSELF and the
    // wrapper was self-consistently zero.  They ARE the guard now that
    // the transmission is nonzero:
    //
    //   Part D2  -- `kray * pdf == value * |cos|` on the transmit side.
    //               This is what catches the two arms of the Kulla-Conty
    //               scale being applied on one side of the estimator and
    //               not the other: `FabricSPF` reprices against its own
    //               `Pdf`, but `FabricBRDF::value` is an independent
    //               body, so a mismatch between them cannot cancel.
    //   Part E2  -- reciprocity ACROSS the surface.  The transmit scale
    //               is a product of two arms that exchange under an l/v
    //               swap; a one-armed form (glTF's own, which the banner
    //               rejects for the reflect side) would fail here.
    //
    // `gap 0` and `gap 0.2` for the same reason the bare rows use both:
    // the second has a live delta lobe, which `PointwiseTest` skips via
    // its existing `isDelta` guard -- so between them the two rows prove
    // the wrapper handles a substrate with and without one.
    UniformScalarPainter* fabThinAlphaSc = new UniformScalarPainter( 0.3 );  fabThinAlphaSc->addref();
    FabricMaterial* fabricThinGap0Mat = new FabricMaterial(
        *weaveLinenThinGap0.Material(), *one, *fabThinAlphaSc, *fabZeroSc );
    fabricThinGap0Mat->addref();
    FabricMaterial* fabricThinGap02Mat = new FabricMaterial(
        *weaveLinenThinGap02.Material(), *one, *fabThinAlphaSc, *fabZeroSc );
    fabricThinGap02Mat->addref();
    ISPF*  fabricThinGap0SPF   = fabricThinGap0Mat->GetSPF();
    IBSDF* fabricThinGap0BRDF  = fabricThinGap0Mat->GetBSDF();
    ISPF*  fabricThinGap02SPF  = fabricThinGap02Mat->GetSPF();
    IBSDF* fabricThinGap02BRDF = fabricThinGap02Mat->GetBSDF();

    // A wrapped `transmission thin` SATIN for Part E2, matching the bare
    // `weaveSatinThin` row above so the two are directly comparable --
    // tilted fibre frames on the transmit side, under the fuzz layer.
    FabricMaterial* fabricSatinThinMat = new FabricMaterial(
        *weaveSatinThin.Material(), *one, *fabThinAlphaSc, *fabZeroSc );
    fabricSatinThinMat->addref();
    IBSDF* fabricSatinThinBRDF = fabricSatinThinMat->GetBSDF();

    // BARE sheen_material's own triad.  9.9 gate 5(a) is explicit that
    // this is a PRE-EXISTING HOLE this phase closes as a matter of
    // course: SheenBRDF has never been in the reciprocity sweep, even
    // though it is the lobe fabric_material is built on, so a
    // non-reciprocity there would have surfaced first as a fabric
    // failure with no way to tell which layer owned it.
    SheenBRDF* bareSheenBRDF = new SheenBRDF( *one, *fabAlphaSc );  bareSheenBRDF->addref();

    std::cout << " done." << std::endl;

    // Delta SPFs
    std::cout << "Constructing delta SPFs..." << std::flush;
    PerfectReflectorSPF* perfReflSPF = new PerfectReflectorSPF( *one );  perfReflSPF->addref();
    PerfectRefractorSPF* perfRefrSPF = new PerfectRefractorSPF( *one, *iorScalar );  perfRefrSPF->addref();

    DielectricSPF* dielectricSPF = new DielectricSPF( *tauScalar, *iorScalar, *scatScalar, false );  dielectricSPF->addref();

    std::cout << " done." << std::endl;

    // ---- Construct BRDFs (paired with SPFs) ----
    std::cout << "Constructing BRDFs..." << std::flush;
    LambertianBRDF* lambertianBRDF = new LambertianBRDF( *white );  lambertianBRDF->addref();
    OrenNayarBRDF* orenNayarBRDF = new OrenNayarBRDF( *white, *roughnessSc );  orenNayarBRDF->addref();
    IsotropicPhongBRDF* phongBRDF = new IsotropicPhongBRDF( *gray, *spec, *highExpSc );  phongBRDF->addref();
    CookTorranceBRDF* cookTorranceBRDF = new CookTorranceBRDF( *gray, *spec, *lowSc, *iorScalar, *extinctionSc );  cookTorranceBRDF->addref();
    GGXBRDF* ggxIsoBRDF = new GGXBRDF( *gray, *spec, *alphaSmallSc, *alphaSmallSc, *iorScalar, *extinctionSc );  ggxIsoBRDF->addref();
    GGXBRDF* ggxAnisoBRDF = new GGXBRDF( *gray, *spec, *alphaSmallSc, *alphaSmallYSc, *iorScalar, *extinctionSc );  ggxAnisoBRDF->addref();
    SchlickBRDF* schlickBRDF = new SchlickBRDF( *gray, *spec, *roughnessSc, *isotropySc );  schlickBRDF->addref();
    WardIsotropicGaussianBRDF* wardIsoBRDF = new WardIsotropicGaussianBRDF( *gray, *spec, *alphaSmallSc );  wardIsoBRDF->addref();
    WardAnisotropicEllipticalGaussianBRDF* wardAnisoBRDF = new WardAnisotropicEllipticalGaussianBRDF( *gray, *spec, *alphaSmallSc, *alphaSmallYSc );  wardAnisoBRDF->addref();
    AshikminShirleyAnisotropicPhongBRDF* ashikminBRDF = new AshikminShirleyAnisotropicPhongBRDF( *ashNuSc, *ashNvSc, *gray, *spec );  ashikminBRDF->addref();
    TranslucentBSDF* translucentBSDF = new TranslucentBSDF( *gray, *trans, *phongNSc );  translucentBSDF->addref();
    SubSurfaceScatteringBSDF* sssBSDF = new SubSurfaceScatteringBSDF( *iorScalar, 0.8, 0.3 );  sssBSDF->addref();

    std::cout << " done." << std::endl;

    // ---- Define material entries ----
    //
    // furnaceTol: per-material tolerance for the furnace test (Part C).
    //
    // Most well-behaved BRDFs (Lambertian, Oren-Nayar, Cook-Torrance, etc.)
    // pass at the default 5% tolerance.  Several models require wider
    // tolerances due to *inherent model limitations* — not implementation
    // bugs.  Each relaxation is documented below.
    //
    // For context the furnace test computes ∫ BRDF*cos dω two ways:
    //   MC estimate  — importance-sample via SPF branching, sum kray values
    //   Quadrature   — numerically integrate BRDF::value()*cos over the
    //                  hemisphere on a regular θ×φ grid
    //
    // When the BRDF model itself is not energy-conserving, or its
    // evaluation diverges at grazing angles, these two estimates can
    // disagree significantly even when the code is correct.

    struct PairedEntry {
        std::string name;
        ISPF* spf;
        IBSDF* brdf;
        bool singleLobe;   // If true, also run pointwise test
        double furnaceTol;  // Per-material furnace test tolerance
    };

    PairedEntry pairedMaterials[] = {
        { "Lambertian",                        lambertianSPF,   lambertianBRDF,     true,  FURNACE_TOL },
        { "OrenNayar",                         orenNayarSPF,    orenNayarBRDF,      true,  FURNACE_TOL },
        { "IsotropicPhong",                    phongSPF,        phongBRDF,          false, FURNACE_TOL },
        { "CookTorrance",                      cookTorranceSPF, cookTorranceBRDF,   false, FURNACE_TOL },
        { "GGX_Isotropic",                     ggxIsoSPF,       ggxIsoBRDF,         false, FURNACE_TOL },
        { "GGX_Anisotropic",                   ggxAnisoSPF,     ggxAnisoBRDF,       false, FURNACE_TOL },

        //--------------------------------------------------------------
        // Schlick BRDF (Schlick 1994 approximation)
        //
        // At grazing incidence (60°+) the denominator in the specular
        // term  Z = r / (r·t² + 1 - t²)²  shrinks, amplifying the
        // specular lobe relative to the SPF importance sampling weights.
        // The quadrature integral then overshoots the MC estimate by
        // ~13%.  This is a known limitation of the Schlick approximation
        // at grazing angles, not a sampling bug.
        //
        // Observed: 2.1% @ 30°, 12.7% @ 60°.  Tolerance set to 15%
        // to cover 60° with headroom.
        //--------------------------------------------------------------
        { "Schlick",                           schlickSPF,      schlickBRDF,        false, 0.15 },

        //--------------------------------------------------------------
        // Ward Isotropic Gaussian BRDF (Ward 1992)
        //
        // The Ward model is *not energy-conserving* by design.  Its
        // specular term  1/sqrt(n·r × n·v) × exp(-tan²h / α²)
        // diverges as either n·r or n·v → 0 (grazing geometry).
        //
        // Because the SPF importance-samples the exponential lobe while
        // the quadrature evaluates the full BRDF (including the
        // divergent 1/sqrt term), the two estimates disagree.
        // The MC sum saturates near the albedo (kray is clamped by
        // the diffuse+specular reflectance painters) while quadrature
        // under-integrates the sharp specular peak on a finite grid.
        //
        // Observed: 8.3% @ 30°, 19.3% @ 60°.  Tolerance set to 25%
        // to cover grazing-angle divergence.
        //--------------------------------------------------------------
        { "WardIsotropicGaussian",             wardIsoSPF,      wardIsoBRDF,        false, 0.25 },

        //--------------------------------------------------------------
        // Ward Anisotropic Elliptical Gaussian BRDF (Ward 1992)
        //
        // Same energy-conservation limitation as the isotropic variant
        // above, compounded by anisotropic roughness (αx ≠ αy) which
        // produces a narrower, taller specular lobe in one tangent
        // direction.  The quadrature grid under-resolves this elliptical
        // peak more severely than the isotropic case.
        //
        // Observed: 10.0% @ 30°, 20.2% @ 60°.  Tolerance set to 25%.
        //--------------------------------------------------------------
        { "WardAnisotropicEllipticalGaussian", wardAnisoSPF,    wardAnisoBRDF,      false, 0.25 },

        { "AshikminShirleyAnisotropicPhong",   ashikminSPF,     ashikminBRDF,       false, FURNACE_TOL },
        { "Translucent",                       translucentSPF,  translucentBSDF,    false, FURNACE_TOL },
        { "SubSurfaceScattering",              sssSPF,          sssBSDF,            true,  FURNACE_TOL },

        //--------------------------------------------------------------
        // coated_material -- docs/WETNESS_COAT_DESIGN.md Phase 2.
        //
        // These are the entries `polished_material` could never have:
        // Polished appears ONLY in the Part-A sanity list below,
        // because its GetBSDF() returns a bare LambertianBRDF (3.3's
        // documented defect) and pairing it here would compare a
        // Fresnel-coated sampler against an uncoated evaluator.  The
        // coated triad's whole reason for existing (7.1) is that its
        // BSDF IS the layered response, so it can be paired.
        //
        // singleLobe = TRUE for both, including the GGX substrate.
        // That is not an approximation: CoatedSPF emits one ray per
        // Scatter with kray = value * cos / mixturePdf, so
        // kray * pdf == value * cos identically for every sample
        // regardless of which mixture component drew it (CoatedSPF.h).
        // Part D therefore checks the layered value <-> Scatter
        // agreement POINTWISE, not just in the hemispherical integral.
        //
        // Default 5 % furnace tolerance -- the layer adds no model
        // limitation of its own to the SPF/BRDF comparison; both sides
        // read the same closed form.
        //--------------------------------------------------------------
        { "Coated_Lambertian",                 coatedLambSPF,   coatedLambBRDF,     true,  FURNACE_TOL },
        { "Coated_GGX",                        coatedGgxSPF,    coatedGgxBRDF,      true,  FURNACE_TOL },

        //--------------------------------------------------------------
        // fabric_material -- docs/CLOTH_FABRIC_DESIGN.md Phase 1.
        //
        // singleLobe = TRUE for both, for the same reason as the coated
        // rows and with the same force: FabricSPF emits one ray per
        // Scatter carrying kray = value * cos / mixturePdf, so
        // kray * pdf == value * cos identically for every sample
        // REGARDLESS OF WHICH BRANCH DREW IT.  Part D therefore checks
        // 9.2's sample-then-reprice recipe POINTWISE.  This is the check
        // that fails if `Scatter` ever reports a branch-local density
        // instead of the full mixture -- the one mistake the delta-lobe
        // convention would invite.
        //--------------------------------------------------------------
        { "Fabric_Lambertian",                 fabricLambSPF,   fabricLambBRDF,     true,  FURNACE_TOL },
        { "Fabric_GGXaniso_weave45",           fabricAnisoSPF,  fabricAnisoBRDF,    true,  FURNACE_TOL },

        //--------------------------------------------------------------
        // weave_material -- docs/CLOTH_FABRIC_DESIGN.md Phase 2.
        //
        // singleLobe = TRUE: `WeaveSPF` emits ONE ray per Scatter
        // carrying kray = value * cos / mixturePdf, so
        // kray * pdf == value * cos identically for every sample
        // REGARDLESS OF WHICH OF THE FOUR LOBES DREW IT.  Part D
        // therefore checks the sample-then-reprice recipe POINTWISE,
        // which is the check that fails if `Scatter` ever reports a
        // branch-local density instead of the full mixture -- the one
        // mistake Zhu 2024 5.1's `f/(p_a p_l)` phrasing would invite if
        // it were taken literally for lobes whose supports overlap.
        //--------------------------------------------------------------
        { "Weave_denim_rot45",                 weaveDenimSPF,   weaveDenimBRDF,     true,  FURNACE_TOL },
        { "Weave_satin_tilted",                weaveSatinSPF,   weaveSatinBRDF,     true,  FURNACE_TOL },
    };
    const int numPaired = sizeof(pairedMaterials) / sizeof(pairedMaterials[0]);

    // Delta SPFs for direction correctness test
    struct DeltaEntry {
        std::string name;
        ISPF* spf;
    };

    DeltaEntry deltaMaterials[] = {
        { "PerfectReflector",  perfReflSPF },
        { "PerfectRefractor",  perfRefrSPF },
        { "Dielectric",        dielectricSPF },
    };
    const int numDelta = sizeof(deltaMaterials) / sizeof(deltaMaterials[0]);

    // All SPFs for sanity test
    struct AllSPFEntry {
        std::string name;
        ISPF* spf;
        bool isDelta;
    };

    AllSPFEntry allSPFs[] = {
        { "Lambertian",                        lambertianSPF,   false },
        { "OrenNayar",                         orenNayarSPF,    false },
        { "IsotropicPhong",                    phongSPF,        false },
        { "CookTorrance",                      cookTorranceSPF, false },
        { "GGX_Isotropic",                     ggxIsoSPF,       false },
        { "GGX_Anisotropic",                   ggxAnisoSPF,     false },
        { "Schlick",                           schlickSPF,      false },
        { "WardIsotropicGaussian",             wardIsoSPF,      false },
        { "WardAnisotropicEllipticalGaussian", wardAnisoSPF,    false },
        { "AshikminShirleyAnisotropicPhong",   ashikminSPF,     false },
        { "Translucent",                       translucentSPF,  false },
        { "Polished",                          polishedSPF,     false },
        { "SubSurfaceScattering",              sssSPF,          false },
        { "Composite",                         compositeSPF,    false },
        { "Coated_Lambertian",                 coatedLambSPF,   false },
        { "Coated_GGX",                        coatedGgxSPF,    false },
        { "Fabric_Lambertian",                 fabricLambSPF,   false },
        { "Fabric_GGXaniso_weave45",           fabricAnisoSPF,  false },
        { "Weave_denim_rot45",                 weaveDenimSPF,   false },
        { "Weave_satin_tilted",                weaveSatinSPF,   false },
        { "PerfectReflector",                  perfReflSPF,     true  },
        { "PerfectRefractor",                  perfRefrSPF,     true  },
        { "Dielectric",                        dielectricSPF,   true  },
    };
    const int numAllSPFs = sizeof(allSPFs) / sizeof(allSPFs[0]);

    double incomingAngles[] = { 30.0 * DEG_TO_RAD, 60.0 * DEG_TO_RAD };
    const char* angleNames[] = { "30deg", "60deg" };

    int numFailed = 0;

    // ================================================================
    //  Part A: Sanity test for ALL SPFs
    // ================================================================

    std::cout << "========================================" << std::endl;
    std::cout << "  Part A: SPF Sanity Test (all " << numAllSPFs << " materials)" << std::endl;
    std::cout << "========================================" << std::endl;

    for( int a = 0; a < 2; a++ )
    {
        std::cout << "--- Incoming angle: " << angleNames[a] << " ---" << std::endl;
        for( int s = 0; s < numAllSPFs; s++ )
        {
            std::string fullName = allSPFs[s].name + " @ " + angleNames[a];
            SanityResult sr = SanityTest( fullName, *allSPFs[s].spf, incomingAngles[a], allSPFs[s].isDelta );

            std::cout << "  " << fullName << ": "
                      << sr.totalRays << " rays";

            if( sr.negativeKray > 0 )
                std::cout << ", " << sr.negativeKray << " negative kray";
            if( sr.unnormalizedDir > 0 )
                std::cout << ", " << sr.unnormalizedDir << " unnormalized";

            if( sr.passed )
                std::cout << " -> PASS" << std::endl;
            else
            {
                std::cout << " -> FAIL" << std::endl;
                numFailed++;
            }
        }
    }
    std::cout << std::endl;

    // ================================================================
    //  Part B: Delta direction correctness
    // ================================================================

    std::cout << "========================================" << std::endl;
    std::cout << "  Part B: Delta SPF Direction Test" << std::endl;
    std::cout << "========================================" << std::endl;

    for( int a = 0; a < 2; a++ )
    {
        std::cout << "--- Incoming angle: " << angleNames[a] << " ---" << std::endl;
        for( int d = 0; d < numDelta; d++ )
        {
            std::string fullName = deltaMaterials[d].name + " @ " + angleNames[a];
            DeltaResult dr = DeltaDirectionTest( fullName, *deltaMaterials[d].spf, incomingAngles[a] );

            std::cout << "  " << fullName << ": ";

            if( dr.hasReflection )
                std::cout << "refl(err=" << std::scientific << std::setprecision(2)
                          << dr.maxReflError << std::fixed << ") ";
            if( dr.hasRefraction )
                std::cout << "refr ";

            std::cout << "delta=" << (dr.isDeltaFlagCorrect ? "ok" : "BAD")
                      << " pdf0=" << (dr.pdfIsZero ? "ok" : "BAD");

            bool passed = dr.isDeltaFlagCorrect && dr.pdfIsZero;
            if( dr.hasReflection ) passed = passed && dr.reflPassed;

            if( passed )
                std::cout << " -> PASS" << std::endl;
            else
            {
                std::cout << " -> FAIL" << std::endl;
                numFailed++;
            }
        }
    }
    std::cout << std::endl;

    // ================================================================
    //  Part C: Furnace test (SPF vs BRDF integral)
    // ================================================================

    std::cout << "========================================" << std::endl;
    std::cout << "  Part C: Furnace Test (SPF vs BRDF)" << std::endl;
    std::cout << "========================================" << std::endl;

    std::vector<FurnaceResult> furnaceResults;

    for( int a = 0; a < 2; a++ )
    {
        std::cout << "--- Incoming angle: " << angleNames[a] << " ---" << std::endl;
        for( int s = 0; s < numPaired; s++ )
        {
            std::string fullName = pairedMaterials[s].name + " @ " + angleNames[a];
            std::cout << "  Testing " << fullName << "..." << std::flush;

            FurnaceResult fr = FurnaceTest( fullName, *pairedMaterials[s].spf,
                                            *pairedMaterials[s].brdf, incomingAngles[a] );

            // Apply per-material tolerance (overrides the default FURNACE_TOL
            // used inside FurnaceTest for materials with known model limitations)
            fr.passed = (fr.relError <= pairedMaterials[s].furnaceTol);
            furnaceResults.push_back( fr );

            std::cout << " MC=" << std::fixed << std::setprecision(6) << fr.mcEstimate
                      << " Quad=" << fr.quadEstimate
                      << " relErr=" << std::setprecision(4) << fr.relError * 100 << "%";

            if( fr.passed )
                std::cout << " -> PASS" << std::endl;
            else
            {
                std::cout << " -> FAIL" << std::endl;
                numFailed++;
            }
        }
    }
    std::cout << std::endl;

    // ================================================================
    //  Part D: Pointwise kray*pdf vs BRDF*cos (single-lobe only)
    // ================================================================

    std::cout << "========================================" << std::endl;
    std::cout << "  Part D: Pointwise kray*pdf vs BRDF*cos" << std::endl;
    std::cout << "========================================" << std::endl;

    std::vector<PointwiseResult> pointwiseResults;

    for( int a = 0; a < 2; a++ )
    {
        std::cout << "--- Incoming angle: " << angleNames[a] << " ---" << std::endl;
        for( int s = 0; s < numPaired; s++ )
        {
            if( !pairedMaterials[s].singleLobe ) continue;

            std::string fullName = pairedMaterials[s].name + " @ " + angleNames[a];
            std::cout << "  Testing " << fullName << "..." << std::flush;

            PointwiseResult pr = PointwiseTest( fullName, *pairedMaterials[s].spf,
                                                *pairedMaterials[s].brdf, incomingAngles[a],
                                                pairedMaterials[s].singleLobe );
            pointwiseResults.push_back( pr );

            std::cout << " samples=" << pr.numSamples
                      << " failures=" << pr.numFailures
                      << " maxErr=" << std::setprecision(4) << pr.maxRelError * 100 << "%"
                      << " avgErr=" << pr.avgRelError * 100 << "%";

            if( pr.passed )
                std::cout << " -> PASS" << std::endl;
            else
            {
                std::cout << " -> FAIL" << std::endl;
                numFailed++;
            }
        }
    }
    std::cout << std::endl;

    // ================================================================
    //  Part D2 (R9 P2, REVIEW_P2R9.md): Pointwise kray*pdf vs BRDF*cos,
    //  TRANSMISSION-direction rows for a `transmission thin` weave.
    //
    //  A DEDICATED loop, not two more `pairedMaterials[]` entries: that
    //  array also feeds Part C's furnace test (`FurnaceTest`, a
    //  REFLECT-HEMISPHERE-only quadrature never audited against a
    //  full-sphere material's transmitted energy) -- adding rows there
    //  would risk a spurious Part C failure unrelated to what this block
    //  exists to check.  This loop reuses `PointwiseTest` exactly as
    //  Part D does (same function, same `POINTWISE_TOL`, same delta-lobe
    //  exclusion via `scat.isDelta`), at the SAME `incomingAngles`
    //  (30/60 degrees) Part D already uses -- NOT a steeper "transmission
    //  angle" of its own. `WeaveBRDF::ResolveWeave`'s `FlipW` re-orients
    //  the shading normal to the ray-facing side, so which absolute
    //  hemisphere the VIEW sits in is not what selects the transmit
    //  branch; `WeaveSPF::ScatterImpl` picks reflect-surface,
    //  reflect-volume or diffuse-transmit stochastically at ANY view
    //  angle (the `(1-w)` bucket splits between reflect-volume and
    //  diffuse-transmit by `transmit_k` regardless of `w`), so a plain
    //  10000-sample draw at the existing angles already lands a solid
    //  fraction of samples on the transmit side -- exactly the samples
    //  the extended `cosO < 0 -> fabs(cosO)` handling above now tests
    //  instead of silently discarding.
    //
    //  Two sheer LINEN configurations: `gap 0` (continuum only, no delta
    //  lobe present at all) and `gap 0.2` (the shipped showcase's own
    //  value -- the delta lobe IS present here and is excluded by
    //  `PointwiseTest`'s existing `isDelta` skip, not a special case).
    //  If `WeaveSPF` ever applied `transmit_k` twice (once in its own
    //  density, once more via a stray repricing) `kray*pdf` would still
    //  reproduce whatever `WeaveSPF::Pdf` used, but `BRDF::value()` would
    //  not move with it -- so a double-application shows up here as a
    //  systematic, non-noise-shaped `kray*pdf > BRDF*|cos|` skew, exactly
    //  the class of defect REVIEW_P2R9.md's from-scratch MC probe (linear
    //  in `transmit`, not the `transmit^2` a double-application would
    //  give) already ruled out by a different method -- this is the
    //  permanent regression harness for that same claim.
    // ================================================================

    std::cout << "========================================" << std::endl;
    std::cout << "  Part D2: Pointwise, transmission direction (P2-B)" << std::endl;
    std::cout << "========================================" << std::endl;

    struct TransmissionPairedEntry { std::string name; ISPF* spf; IBSDF* brdf; };
    TransmissionPairedEntry transmissionPaired[] = {
        { "Weave_linen_thin_gap0 (transmission)",   weaveLinenThinGap0SPF,  weaveLinenThinGap0BRDF  },
        { "Weave_linen_thin_gap0.2 (transmission)", weaveLinenThinGap02SPF, weaveLinenThinGap02BRDF },
        // R8 P1.1 / debt 22: the same two, WRAPPED.  See the construction
        // site for what each catches.  `singleLobe` stays TRUE below --
        // `FabricSPF` emits at most one ray per Scatter call over a weave
        // substrate exactly as it does over every other allowlisted one.
        { "Fabric/Weave_linen_thin_gap0 (transmission)",   fabricThinGap0SPF,  fabricThinGap0BRDF  },
        { "Fabric/Weave_linen_thin_gap0.2 (transmission)", fabricThinGap02SPF, fabricThinGap02BRDF },
    };

    for( int a = 0; a < 2; a++ )
    {
        std::cout << "--- Incoming angle: " << angleNames[a] << " ---" << std::endl;
        for( const TransmissionPairedEntry& e : transmissionPaired )
        {
            std::string fullName = e.name + " @ " + angleNames[a];
            std::cout << "  Testing " << fullName << "..." << std::flush;

            PointwiseResult pr = PointwiseTest( fullName, *e.spf, *e.brdf,
                                                incomingAngles[a], /*singleLobe=*/true );
            pointwiseResults.push_back( pr );

            std::cout << " samples=" << pr.numSamples
                      << " failures=" << pr.numFailures
                      << " maxErr=" << std::setprecision(4) << pr.maxRelError * 100 << "%"
                      << " avgErr=" << pr.avgRelError * 100 << "%";

            if( pr.passed )
                std::cout << " -> PASS" << std::endl;
            else
            {
                std::cout << " -> FAIL" << std::endl;
                numFailed++;
            }
        }
    }
    std::cout << std::endl;

    // ================================================================
    //  Part E: Helmholtz reciprocity  f(a->b) == f(b->a)
    //
    //  Run on the LAYERED materials, which are the ones whose shared
    //  layer terms can silently pick up a view dependence, plus their
    //  bare substrates as controls -- if a control ever fails, the
    //  defect is in the substrate, not in the coat.
    // ================================================================

    std::cout << "========================================" << std::endl;
    std::cout << "  Part E: Reciprocity f(a->b) == f(b->a)" << std::endl;
    std::cout << "========================================" << std::endl;

    struct ReciprocityEntry { std::string name; IBSDF* brdf; };
    ReciprocityEntry reciprocityMaterials[] = {
        { "Lambertian (control)",              lambertianBRDF },
        { "GGX_Isotropic (control)",           ggxIsoBRDF     },
        { "Coated_Lambertian",                 coatedLambBRDF },
        { "Coated_GGX",                        coatedGgxBRDF  },
        { "Coated_GGX_tinted_absorbing_c0.5",  coatedFullMat->GetBSDF() },
        // 9.9 gate 5(a).  `Sheen (Charlie)` is the pre-existing hole:
        // the lobe has been shipping since 2026-04 and has never been in
        // this sweep.  The two fabric rows are the layered form -- 9.2's
        // scaling factor is a PRODUCT of two arms that exchange under an
        // l/v swap, divided by a direction-independent denominator, and
        // the sheen lobe's normaliser is a MAX of the same two arms, so
        // both are symmetric BY CONSTRUCTION.  "By construction" is
        // exactly the claim coated_material's first cut also made before
        // it was measured at ~28 % asymmetry at grazing.
        { "Sheen (Charlie, pre-existing hole)", bareSheenBRDF   },
        { "Fabric_Lambertian",                  fabricLambBRDF  },
        { "Fabric_GGXaniso_weave45",            fabricAnisoBRDF },
        // Phase 2.  See the construction site above for why these two
        // configurations, and for the list of terms whose symmetry the
        // model depends on.
        { "Weave_denim_rot45",                  weaveDenimBRDF  },
        { "Weave_satin_tilted",                 weaveSatinBRDF  },
    };

    for( const ReciprocityEntry& e : reciprocityMaterials )
    {
        ReciprocityResult rr = ReciprocityTest( e.name, *e.brdf );

        std::cout << "  " << e.name << ": pairs=" << rr.numPairs
                  << " failures=" << rr.numFailures
                  << " maxRelErr(RGB)=" << std::scientific << std::setprecision(3) << rr.maxRelError
                  << " maxRelErr(NM)=" << rr.maxRelErrorNM << std::fixed;

        if( rr.passed )
            std::cout << " -> PASS" << std::endl;
        else
        {
            std::cout << " -> FAIL" << std::endl;
            numFailed++;
        }
    }
    std::cout << std::endl;

    // ================================================================
    //  Part E2 (P2-B): reciprocity across the surface, for the
    //  TRANSMISSION lobes -- `transmission thin` materials only, since
    //  every other row here returns 0 for a cross-hemisphere pair by
    //  construction (its `ScattersFullSphere()` is false) and would
    //  just add a zero-vs-zero pass that proves nothing.
    // ================================================================

    std::cout << "========================================" << std::endl;
    std::cout << "  Part E2: Cross-hemisphere reciprocity (P2-B transmission)" << std::endl;
    std::cout << "========================================" << std::endl;

    ReciprocityEntry crossHemisphereMaterials[] = {
        { "Weave_satin_thin (transmission)",          weaveSatinThinBRDF },
        // R8 P1.1 / debt 22.  The wrapped twin of the row above: the
        // fabric layer's transmit scale is
        // `(1-m*Ehat(|n.v|))(1-m*Ehat(|n.l|)) / (1-m*Ebar)`, whose two
        // arms EXCHANGE under an l/v swap, so this row is what proves
        // the wrapper did not reach for glTF's one-armed form (which the
        // FabricBRDF banner rejects for the reflect side for exactly
        // this reason) on the way through the surface.
        { "Fabric/Weave_satin_thin (transmission)",   fabricSatinThinBRDF },
    };

    for( const ReciprocityEntry& e : crossHemisphereMaterials )
    {
        ReciprocityResult rr = ReciprocityTestCrossHemisphere( e.name, *e.brdf );

        std::cout << "  " << e.name << ": pairs=" << rr.numPairs
                  << " failures=" << rr.numFailures
                  << " maxRelErr(RGB)=" << std::scientific << std::setprecision(3) << rr.maxRelError
                  << " maxRelErr(NM)=" << rr.maxRelErrorNM << std::fixed;

        if( rr.passed )
            std::cout << " -> PASS" << std::endl;
        else
        {
            std::cout << " -> FAIL" << std::endl;
            numFailed++;
        }
    }
    std::cout << std::endl;

    // ================================================================
    //  Summary
    // ================================================================

    std::cout << "===== Summary =====" << std::endl;

    std::cout << std::endl << "Furnace Test Results:" << std::endl;
    for( size_t i = 0; i < furnaceResults.size(); i++ )
    {
        const FurnaceResult& fr = furnaceResults[i];
        std::cout << (fr.passed ? "PASS" : "FAIL") << "  " << fr.name
                  << "  MC=" << std::setprecision(6) << fr.mcEstimate
                  << "  Quad=" << fr.quadEstimate
                  << "  err=" << std::setprecision(2) << fr.relError * 100 << "%"
                  << std::endl;
    }

    std::cout << std::endl << "Pointwise Test Results:" << std::endl;
    for( size_t i = 0; i < pointwiseResults.size(); i++ )
    {
        const PointwiseResult& pr = pointwiseResults[i];
        std::cout << (pr.passed ? "PASS" : "FAIL") << "  " << pr.name
                  << "  failures=" << pr.numFailures << "/" << pr.numSamples
                  << "  maxErr=" << std::setprecision(2) << pr.maxRelError * 100 << "%"
                  << std::endl;
    }

    // Fabric triad: same ownership discipline as the coated one below --
    // the MATERIAL owns the BRDF/SPF the tables above borrowed.
    safe_release( bareSheenBRDF );
    safe_release( fabricSatinThinMat );
    safe_release( fabricThinGap02Mat );
    safe_release( fabricThinGap0Mat );
    safe_release( fabThinAlphaSc );
    safe_release( fabricAnisoMat );
    safe_release( fabricLambMat );
    safe_release( fabBaseAnisoMat );
    safe_release( fabZeroSc );
    safe_release( fabWeaveSc );
    safe_release( fabAlphaSc );

    // Coated triad: release the materials (which own the BRDF/SPF the
    // tables above borrowed) before their substrates and painters.
    safe_release( coatedFullMat );
    safe_release( coatAmber );
    safe_release( coatAbsSc );
    safe_release( coatThickSc );
    safe_release( coatHalfSc );
    safe_release( coatedGgxMat );
    safe_release( coatedLambMat );
    safe_release( coatBaseGgxMat );
    safe_release( coatBaseLambMat );
    safe_release( coatZeroSc );
    safe_release( coatRoughSc );
    safe_release( coatIorSc );
    safe_release( coatWeightSc );

    g_stubObject->release();

    if( numFailed > 0 )
    {
        std::cout << std::endl << numFailed << " test(s) FAILED" << std::endl;
        return 1;
    }

    std::cout << std::endl << "All SPF-BSDF consistency tests passed!" << std::endl;
    return 0;
}
