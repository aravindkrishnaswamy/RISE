//////////////////////////////////////////////////////////////////////
//
//  HairBSDFTest.cpp - Validation of the Chiang et al. 2016 near-field
//    hair BCSDF (src/Library/Materials/HairBSDF.{h,cpp}).
//
//  Six groups of tests:
//
//    1. WHITE FURNACE (the energy-conservation regression guard).
//       With sigma_a == 0 the bare Chiang lobe sum must integrate to
//       exactly 1 over the sphere in solid-angle measure.  Checked two
//       independent ways at every (beta_m, beta_n) x h cell:
//         (a) through the SPF -- mean of `kray` / `krayNM` over many
//             Scatter calls;
//         (b) through the BRDF -- deterministic tensor quadrature of
//             `value(wi) * |cos(wi, N)|` over the sphere, which is the
//             ONLY one of the two that actually tests the M_p / N_p
//             normalisation (see the note above RunFurnace).
//
//    2. SAMPLE <-> PDF CONSISTENCY.  Every sampled direction must
//       report a strictly positive `Pdf`, and that `Pdf` must equal the
//       pdf the sample carried.  Plus the estimator cross-check
//       E[value * cos / pdf] (over SPF samples) == the quadrature of
//       INTEGRAL value * cos, at a NON-zero sigma_a so the ratio is not
//       trivially 1.
//
//    3. PdfNM integrates to 1 over the sphere.
//
//    4. EvaluateKrayNM CONSISTENCY.  At the hero wavelength it must
//       reproduce the krayNM that ScatterNM produced; at a companion
//       wavelength it must reproduce the integrator's own fallback
//       route, valueNM * |cos| / pdf_hero.
//
//    5. MELANIN LADDER.  Increasing eumelanin lowers throughput
//       monotonically; pheomelanin reddens (throughput at 650 nm above
//       throughput at 450 nm).
//
//    6. TIER-3 INVERSION ROUND TRIP.  colour C -> sigma_a -> C.
//
//  DELIBERATELY NOT TESTED: reciprocity.  The Chiang model is knowingly
//  non-reciprocal -- the near-field h-conditioning and the cuticle-tilt
//  sign convention break f(wo->wi) == f(wi->wo), and PBRT documents and
//  accepts the same.  Asserting it would be asserting a property the
//  model does not have.  The consequence (a small model-level bias in
//  BDPT / VCM hair renders) is recorded in HairBSDF.h section 5 and in
//  docs/HAIR_FUR_DESIGN.md section 6.2.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/FiniteMath.h"
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
#include "../src/Library/Materials/HairBSDF.h"
#include "../src/Library/Materials/HairMaterial.h"

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

// ============================================================
//  Configuration
// ============================================================

//! Samples for the SPF-side furnace / estimator means.
static const int    kSPFSamples      = 200000;
//! Tensor-quadrature resolution for the sphere integrals.  The
//! longitudinal lobe at beta_m = 0.1 has an angular width of ~0.08 rad,
//! so 900 theta cells put ~23 samples across it -- comfortably inside
//! the midpoint rule's asymptotic regime.
static const int    kQuadTheta       = 900;
static const int    kQuadPhi         = 900;
//! Tolerance on the energy integrals.  The SPF-side furnace is exact by
//! construction (kray == 1 identically when sigma_a == 0), so it gets a
//! tight bound; the quadrature carries real discretisation error.
static const double kFurnaceSPFTol   = 1e-9;
static const double kFurnaceQuadTol  = 0.02;
static const double kPdfExactTol     = 1e-9;
static const double kEstimatorTol    = 0.02;
static const double kKrayNMTol       = 1e-9;
static const double kInversionTol    = 0.15;

static int g_failures = 0;
static int g_checks   = 0;

static StubObject* g_stubObject = 0;

// ============================================================
//  Reference-counted painters have PROTECTED destructors (they are
//  heap-only, released rather than deleted), so they cannot be stack
//  locals.  This is the local RAII holder the tests use instead.
// ============================================================

template<class T>
class Ref
{
public:
    explicit Ref( T* q ) : p( q ) { p->addref(); }
    ~Ref() { p->release(); }
    T& operator*()  const { return *p; }
    T* operator->() const { return p; }
    T* get()        const { return p; }
private:
    T* p;
    Ref( const Ref& );
    Ref& operator=( const Ref& );
};

typedef Ref<UniformScalarPainter> ScalarRef;
typedef Ref<UniformColorPainter>  ColorRef;

// ============================================================
//  Reporting
// ============================================================

static void Check( const bool ok, const std::string& what, const double got, const double expected )
{
    g_checks++;
    if( !ok ) {
        g_failures++;
        std::cout << "  FAIL  " << what
                  << "  got " << std::setprecision(10) << got
                  << ", expected " << expected << std::endl;
    }
}

static void CheckTrue( const bool ok, const std::string& what )
{
    g_checks++;
    if( !ok ) {
        g_failures++;
        std::cout << "  FAIL  " << what << std::endl;
    }
}

static bool Near( const double a, const double b, const double relTol )
{
    const double scale = ( fabs(b) > 1e-12 ) ? fabs(b) : 1.0;
    return fabs( a - b ) <= relTol * scale;
}

// ============================================================
//  Synthetic fibre intersection
//
//  The fibre frame is built explicitly:  tangent = +X (onb.u()),
//  normal = +Z (onb.w()), bitangent = +Y (onb.v()).  CreateFromWU
//  fixes W and re-orthonormalises U against it, giving exactly
//  (u, v, w) = (X, Y, Z) -- the same right-handed frame the model
//  assumes (HairBSDF.h section 1).
//
//  `thetaO` is measured FROM the normal plane, `phiO` around the
//  fibre axis, matching the model's own parameterisation, and
//  ri.ptCoord.y encodes the near-field offset h = 2v - 1.
// ============================================================

static RayIntersectionGeometric MakeFibreHit(
    const double thetaO, const double phiO, const double h )
{
    const Vector3 wo( sin(thetaO),
                      cos(thetaO) * cos(phiO),
                      cos(thetaO) * sin(phiO) );

    // The incoming ray travels TOWARD the intersection, so its
    // direction is -wo and its origin sits one unit back along wo.
    const Ray inRay( Point3( wo.x, wo.y, wo.z ), -wo );
    RasterizerState rs = {0, 0};
    RayIntersectionGeometric ri( inRay, rs );

    ri.bHit = true;
    ri.range = 1.0;
    ri.ptIntersection = Point3( 0, 0, 0 );
    ri.vNormal     = Vector3( 0, 0, 1 );
    ri.vGeomNormal = Vector3( 0, 0, 1 );
    ri.onb.CreateFromWU( Vector3( 0, 0, 1 ), Vector3( 1, 0, 0 ) );
    ri.ptCoord  = Point2( 0.5, 0.5 * ( h + 1.0 ) );
    ri.ptCoord1 = ri.ptCoord;

    return ri;
}

//! World direction from fibre-frame spherical angles.
static Vector3 FibreDir( const RayIntersectionGeometric& ri,
                         const double theta, const double phi )
{
    const double ct = cos(theta), st = sin(theta);
    return Vector3Ops::Normalize(
          ri.onb.u() * st
        + ri.onb.v() * ( ct * cos(phi) )
        + ri.onb.w() * ( ct * sin(phi) ) );
}

// ============================================================
//  Painter helpers
// ============================================================

//! Colour tier 2 (direct sigma_a) plus the four mandatory appearance
//! slots -- the configuration most of the tests below use.
static HairPainters MakeSigmaAPainters(
    const IScalarPainter& sigmaAP, const IScalarPainter& betaMP,
    const IScalarPainter& betaNP,  const IScalarPainter& alphaP,
    const IScalarPainter& iorP )
{
    HairPainters p;
    p.sigma_a = &sigmaAP;
    p.beta_m  = &betaMP;
    p.beta_n  = &betaNP;
    p.alpha   = &alphaP;
    p.ior     = &iorP;
    return p;
}

// ============================================================
//  Sphere integrals through the PUBLIC BRDF / SPF API
//
//  In the fibre parameterisation w = (sin t, cos t cos p, cos t sin p),
//  the solid-angle element is  dw = cos(t) dt dp  with t in
//  [-pi/2, pi/2].  Midpoint rule, half-cell offset in BOTH axes so the
//  quadrature never lands exactly on phi = 0 or pi (where the shading
//  cosine vanishes and the 1/|cos| factor inside `value` is skipped).
//
//  This is the test that actually exercises energy conservation: it
//  integrates the BRDF the integrator itself would evaluate, with no
//  knowledge of the model's internal factorisation.  The SPF-side
//  furnace cannot do that -- with sigma_a == 0 the attenuation vector
//  equals the sampling PMF, so `kray` is identically 1 and the mean is
//  1 no matter what M_p and N_p are.  Both are reported; only this one
//  can fail on a normalisation bug.
// ============================================================

static double QuadratureBSDFEnergy(
    const HairBRDF& brdf, const RayIntersectionGeometric& ri, const int channel )
{
    const double dTheta = PI / kQuadTheta;
    const double dPhi   = TWO_PI / kQuadPhi;
    double sum = 0;

    for( int i = 0; i < kQuadTheta; i++ )
    {
        const double theta = -PI_OV_TWO + ( i + 0.5 ) * dTheta;
        const double ct = cos(theta);
        for( int j = 0; j < kQuadPhi; j++ )
        {
            const double phi = ( j + 0.5 ) * dPhi;
            const Vector3 wi = FibreDir( ri, theta, phi );
            const RISEPel f = brdf.value( wi, ri );
            const double cosN = fabs( Vector3Ops::Dot( wi, ri.onb.w() ) );
            sum += f[(unsigned int)channel] * cosN * ct;
        }
    }
    return sum * dTheta * dPhi;
}

static double QuadraturePdfNM(
    const HairSPF& spf, const RayIntersectionGeometric& ri,
    const IORStack& iorStack, const double nm )
{
    const double dTheta = PI / kQuadTheta;
    const double dPhi   = TWO_PI / kQuadPhi;
    double sum = 0;

    for( int i = 0; i < kQuadTheta; i++ )
    {
        const double theta = -PI_OV_TWO + ( i + 0.5 ) * dTheta;
        const double ct = cos(theta);
        for( int j = 0; j < kQuadPhi; j++ )
        {
            const double phi = ( j + 0.5 ) * dPhi;
            const Vector3 wi = FibreDir( ri, theta, phi );
            sum += spf.PdfNM( ri, wi, nm, iorStack ) * ct;
        }
    }
    return sum * dTheta * dPhi;
}

// ============================================================
//  SPF-side means
// ============================================================

struct SPFStats
{
    double meanKray;        //!< mean of kray[channel] (RGB) or krayNM (NM)
    int    nullScatters;    //!< Scatter calls that produced no ray
    int    pdfMismatches;   //!< samples whose Pdf() disagreed with the carried pdf
    double maxPdfRelErr;
};

static SPFStats RunSPFSamples(
    const HairSPF& spf, const RayIntersectionGeometric& ri,
    const IORStack& iorStack, const bool bNM, const double nm,
    const int channel, const int count )
{
    SPFStats st;
    st.meanKray = 0;
    st.nullScatters = 0;
    st.pdfMismatches = 0;
    st.maxPdfRelErr = 0;

    RandomNumberGenerator rng;
    IndependentSampler sampler( rng );

    double sum = 0;
    for( int i = 0; i < count; i++ )
    {
        ScatteredRayContainer scattered;
        if( bNM ) {
            spf.ScatterNM( ri, sampler, nm, scattered, iorStack );
        } else {
            spf.Scatter( ri, sampler, scattered, iorStack );
        }

        if( scattered.Count() == 0 ) {
            st.nullScatters++;
            continue;
        }

        const ScatteredRay& s = scattered[0];
        sum += bNM ? s.krayNM : s.kray[(unsigned int)channel];

        // Cross-validate the carried pdf against a fresh Pdf() query.
        const double pdfEval = bNM
            ? spf.PdfNM( ri, s.ray.Dir(), nm, iorStack )
            : spf.Pdf( ri, s.ray.Dir(), iorStack );
        if( !( pdfEval > 0 ) ) {
            st.pdfMismatches++;
        } else {
            const double rel = fabs( pdfEval - s.pdf ) / s.pdf;
            if( rel > st.maxPdfRelErr ) { st.maxPdfRelErr = rel; }
            if( rel > kPdfExactTol ) { st.pdfMismatches++; }
        }
    }

    st.meanKray = sum / count;
    return st;
}

// ============================================================
//  1 + 2.  White furnace and sample<->pdf consistency
// ============================================================

static void RunFurnace()
{
    std::cout << "=== 1. White furnace (sigma_a = 0) ===" << std::endl;

    const double betas[3] = { 0.1, 0.3, 0.8 };
    const double hs[4]    = { 0.0, 0.4, 0.95, -0.95 };

    ScalarRef sigmaZero( new UniformScalarPainter( 0.0 ) );
    ScalarRef alpha( new UniformScalarPainter( 2.0 ) );
    ScalarRef ior( new UniformScalarPainter( 1.55 ) );

    IORStack iorStack = MakeTestIORStack( g_stubObject );

    for( int bm = 0; bm < 3; bm++ )
    {
        for( int bn = 0; bn < 3; bn++ )
        {
            ScalarRef betaM( new UniformScalarPainter( betas[bm] ) );
            ScalarRef betaN( new UniformScalarPainter( betas[bn] ) );

            const HairPainters hp = MakeSigmaAPainters( *sigmaZero, *betaM, *betaN, *alpha, *ior );
            HairBRDF* brdf = new HairBRDF( hp );  brdf->addref();
            HairSPF*  spf  = new HairSPF( hp );   spf->addref();

            for( int hi = 0; hi < 4; hi++ )
            {
                const RayIntersectionGeometric ri = MakeFibreHit( 0.35, 0.9, hs[hi] );

                char label[192];
                snprintf( label, sizeof(label), "furnace bm=%.1f bn=%.1f h=%+.2f",
                          betas[bm], betas[bn], hs[hi] );

                // (a) SPF side, RGB and NM.
                const SPFStats rgb = RunSPFSamples( *spf, ri, iorStack, false, 0, 1, kSPFSamples );
                const SPFStats nm  = RunSPFSamples( *spf, ri, iorStack, true, 550.0, 0, kSPFSamples );

                Check( Near( rgb.meanKray, 1.0, kFurnaceSPFTol ),
                       std::string(label) + " SPF/RGB E[kray]", rgb.meanKray, 1.0 );
                Check( Near( nm.meanKray, 1.0, kFurnaceSPFTol ),
                       std::string(label) + " SPF/NM E[krayNM]", nm.meanKray, 1.0 );
                CheckTrue( rgb.nullScatters == 0 && nm.nullScatters == 0,
                           std::string(label) + " no null scatters" );

                // (2) Sample <-> pdf cross-validation rides along here.
                CheckTrue( rgb.pdfMismatches == 0,
                           std::string(label) + " RGB Pdf() == sample pdf" );
                CheckTrue( nm.pdfMismatches == 0,
                           std::string(label) + " NM PdfNM() == sample pdf" );

                // (b) BRDF side -- the real energy test.
                const double quad = QuadratureBSDFEnergy( *brdf, ri, 1 );
                Check( Near( quad, 1.0, kFurnaceQuadTol ),
                       std::string(label) + " INTEGRAL value*cos", quad, 1.0 );

                std::cout << "  " << label
                          << "  SPF=" << std::setprecision(8) << rgb.meanKray
                          << "  quad=" << quad
                          << "  maxPdfRelErr=" << rgb.maxPdfRelErr << std::endl;
            }

            spf->release();
            brdf->release();
        }
    }

}

// ============================================================
//  2b.  Estimator cross-check at a NON-zero sigma_a
// ============================================================

static void RunEstimatorCrossCheck()
{
    std::cout << "=== 2. Estimator cross-check (sigma_a > 0) ===" << std::endl;

    ScalarRef sigmaA( new UniformScalarPainter( 0.6 ) );
    ScalarRef betaM( new UniformScalarPainter( 0.3 ) );
    ScalarRef betaN( new UniformScalarPainter( 0.3 ) );
    ScalarRef alpha( new UniformScalarPainter( 2.0 ) );
    ScalarRef ior( new UniformScalarPainter( 1.55 ) );

    const HairPainters hp = MakeSigmaAPainters( *sigmaA, *betaM, *betaN, *alpha, *ior );
    HairBRDF* brdf = new HairBRDF( hp );  brdf->addref();
    HairSPF*  spf  = new HairSPF( hp );   spf->addref();

    IORStack iorStack = MakeTestIORStack( g_stubObject );

    const double hs[3] = { 0.0, 0.6, -0.9 };
    for( int hi = 0; hi < 3; hi++ )
    {
        const RayIntersectionGeometric ri = MakeFibreHit( -0.2, 2.1, hs[hi] );

        const SPFStats st = RunSPFSamples( *spf, ri, iorStack, false, 0, 1, kSPFSamples );
        const double quad = QuadratureBSDFEnergy( *brdf, ri, 1 );

        char label[128];
        snprintf( label, sizeof(label), "estimator h=%+.2f", hs[hi] );

        Check( Near( st.meanKray, quad, kEstimatorTol ),
               std::string(label) + " E[value*cos/pdf] == INTEGRAL value*cos",
               st.meanKray, quad );
        CheckTrue( st.pdfMismatches == 0, std::string(label) + " Pdf() == sample pdf" );
        CheckTrue( st.nullScatters == 0,  std::string(label) + " no null scatters" );

        std::cout << "  " << label << "  E[kray]=" << std::setprecision(8) << st.meanKray
                  << "  quad=" << quad << std::endl;
    }

    spf->release(); brdf->release();
}

// ============================================================
//  3.  PdfNM integrates to 1
// ============================================================

static void RunPdfIntegral()
{
    std::cout << "=== 3. PdfNM integrates to 1 ===" << std::endl;

    const double betas[2] = { 0.15, 0.7 };
    ScalarRef sigmaA( new UniformScalarPainter( 0.8 ) );
    ScalarRef alpha( new UniformScalarPainter( 2.0 ) );
    ScalarRef ior( new UniformScalarPainter( 1.55 ) );

    IORStack iorStack = MakeTestIORStack( g_stubObject );

    for( int bm = 0; bm < 2; bm++ )
    {
        for( int bn = 0; bn < 2; bn++ )
        {
            ScalarRef betaM( new UniformScalarPainter( betas[bm] ) );
            ScalarRef betaN( new UniformScalarPainter( betas[bn] ) );

            const HairPainters hp = MakeSigmaAPainters( *sigmaA, *betaM, *betaN, *alpha, *ior );
            HairSPF* spf = new HairSPF( hp ); spf->addref();

            const RayIntersectionGeometric ri = MakeFibreHit( 0.25, 1.4, 0.3 );
            const double integral = QuadraturePdfNM( *spf, ri, iorStack, 550.0 );

            char label[128];
            snprintf( label, sizeof(label), "pdf integral bm=%.2f bn=%.2f", betas[bm], betas[bn] );
            Check( Near( integral, 1.0, kFurnaceQuadTol ), label, integral, 1.0 );
            std::cout << "  " << label << " = " << std::setprecision(8) << integral << std::endl;

            spf->release();
        }
    }

}

// ============================================================
//  4.  EvaluateKrayNM consistency
// ============================================================

static void RunEvaluateKrayNM()
{
    std::cout << "=== 4. EvaluateKrayNM consistency ===" << std::endl;

    // A spectrally-varying tier-1 source, so the companion-wavelength
    // check is not trivially the same number as the hero's.
    ScalarRef eumel( new UniformScalarPainter( 1.3 ) );
    ScalarRef pheo( new UniformScalarPainter( 0.2 ) );
    ScalarRef betaM( new UniformScalarPainter( 0.3 ) );
    ScalarRef betaN( new UniformScalarPainter( 0.3 ) );
    ScalarRef alpha( new UniformScalarPainter( 2.0 ) );
    ScalarRef ior( new UniformScalarPainter( 1.55 ) );

    HairPainters hp;
    hp.eumelanin   = eumel.get();
    hp.pheomelanin = pheo.get();
    hp.beta_m = betaM.get();  hp.beta_n = betaN.get();
    hp.alpha  = alpha.get();  hp.ior    = ior.get();

    HairBRDF* brdf = new HairBRDF( hp ); brdf->addref();
    HairSPF*  spf  = new HairSPF( hp );  spf->addref();

    IORStack iorStack = MakeTestIORStack( g_stubObject );
    const RayIntersectionGeometric ri = MakeFibreHit( 0.4, 0.7, 0.25 );

    RandomNumberGenerator rng;
    IndependentSampler sampler( rng );

    const double heroNM = 550.0;
    const double compNM[3] = { 450.0, 600.0, 650.0 };

    int heroMismatch = 0, compMismatch = 0, evaluated = 0;
    double maxHeroErr = 0, maxCompErr = 0;

    for( int i = 0; i < 20000; i++ )
    {
        ScatteredRayContainer scattered;
        spf->ScatterNM( ri, sampler, heroNM, scattered, iorStack );
        if( scattered.Count() == 0 ) { continue; }
        const ScatteredRay& s = scattered[0];
        if( !( s.pdf > 0 ) || !( s.krayNM > 0 ) ) { continue; }
        evaluated++;

        // Hero: EvaluateKrayNM must reproduce the sample's own krayNM.
        const double heroEval = spf->EvaluateKrayNM(
            ri, s.ray.Dir(), s.type, heroNM, iorStack );
        const double heroErr = fabs( heroEval - s.krayNM ) / s.krayNM;
        if( heroErr > maxHeroErr ) { maxHeroErr = heroErr; }
        if( heroErr > kKrayNMTol ) { heroMismatch++; }

        // Companions: must reproduce the integrator's own fallback
        // route, PathTracingIntegrator.cpp:4712-4721.
        for( int c = 0; c < 3; c++ )
        {
            const double got = spf->EvaluateKrayNM(
                ri, s.ray.Dir(), s.type, compNM[c], iorStack );
            const double cosT = fabs( Vector3Ops::Dot( s.ray.Dir(), ri.vNormal ) );
            const double want = brdf->valueNM( s.ray.Dir(), ri, compNM[c] ) * cosT / s.pdf;
            if( !( want > 0 ) ) { continue; }
            const double err = fabs( got - want ) / want;
            if( err > maxCompErr ) { maxCompErr = err; }
            if( err > kKrayNMTol ) { compMismatch++; }
        }
    }

    CheckTrue( evaluated > 15000, "EvaluateKrayNM: enough usable samples" );
    CheckTrue( heroMismatch == 0, "EvaluateKrayNM hero == ScatterNM krayNM" );
    CheckTrue( compMismatch == 0, "EvaluateKrayNM companion == valueNM*cos/pdf" );

    // The tag guard: a lobe type we never emit must decline.
    CheckTrue( spf->EvaluateKrayNM( ri, Vector3(0,0,1), ScatteredRay::eRayRefraction,
                                    heroNM, iorStack ) < 0,
               "EvaluateKrayNM declines a foreign ray type" );

    std::cout << "  samples=" << evaluated
              << "  maxHeroRelErr=" << std::setprecision(4) << maxHeroErr
              << "  maxCompRelErr=" << maxCompErr << std::endl;

    spf->release(); brdf->release();
}

// ============================================================
//  5.  Melanin ladder
// ============================================================

//! Total single-scatter throughput = E[kray] over SPF samples, which
//! is the sphere integral of the bare lobe sum.  Falls monotonically
//! as absorption rises.
static double MelaninThroughput(
    const double ce, const double cp, const bool bNM, const double nm )
{
    ScalarRef eumel( new UniformScalarPainter( ce ) );
    ScalarRef pheo( new UniformScalarPainter( cp ) );
    ScalarRef betaM( new UniformScalarPainter( 0.3 ) );
    ScalarRef betaN( new UniformScalarPainter( 0.3 ) );
    ScalarRef alpha( new UniformScalarPainter( 2.0 ) );
    ScalarRef ior( new UniformScalarPainter( 1.55 ) );

    HairPainters hp;
    hp.eumelanin   = eumel.get();
    hp.pheomelanin = pheo.get();
    hp.beta_m = betaM.get();  hp.beta_n = betaN.get();
    hp.alpha  = alpha.get();  hp.ior    = ior.get();

    HairSPF* spf = new HairSPF( hp ); spf->addref();

    IORStack iorStack = MakeTestIORStack( g_stubObject );
    const RayIntersectionGeometric ri = MakeFibreHit( 0.3, 1.1, 0.2 );
    const SPFStats st = RunSPFSamples( *spf, ri, iorStack, bNM, nm, 1, kSPFSamples );

    spf->release();
    return st.meanKray;
}

static void RunMelaninLadder()
{
    std::cout << "=== 5. Melanin ladder ===" << std::endl;

    const double ce[5] = { 0.0, 0.3, 0.8, 1.3, 4.0 };
    double prev = 2.0;
    for( int i = 0; i < 5; i++ )
    {
        const double t = MelaninThroughput( ce[i], 0.0, false, 0 );
        char label[128];
        snprintf( label, sizeof(label), "eumelanin %.1f throughput below %.1f's", ce[i], (i > 0 ? ce[i-1] : 0.0) );
        CheckTrue( t < prev, label );
        std::cout << "  eumelanin=" << ce[i] << "  throughput=" << std::setprecision(6) << t << std::endl;
        prev = t;
    }

    // Pheomelanin reddens: its extinction falls steeply toward the red
    // end, so 650 nm must survive better than 450 nm.
    const double red  = MelaninThroughput( 0.0, 1.5, true, 650.0 );
    const double blue = MelaninThroughput( 0.0, 1.5, true, 450.0 );
    CheckTrue( red > blue, "pheomelanin reddens (650 nm > 450 nm)" );
    std::cout << "  pheomelanin 1.5: 650nm=" << std::setprecision(6) << red
              << "  450nm=" << blue << std::endl;
}

// ============================================================
//  6.  Tier-3 inversion round trip
//
//  NOTE ON WHAT IS ACTUALLY ASSERTED.  Chiang's D(beta_n) fit maps a
//  target MULTIPLE-scattering-averaged reflectance to sigma_a; it is
//  NOT a single-scatter quantity.  For C = 0.5 the inversion yields
//  sigma_a ~ 0.014, whose SINGLE-scatter furnace throughput is ~0.97,
//  not 0.5 -- the colour emerges over tens of forward bounces through
//  a hair assembly, which a unit test cannot reproduce without that
//  assembly.  So the round trip asserted here is the real one:
//  C -> sigma_a -> C through the model's own forward mapping, read
//  back via the closed-form `albedo()` the OIDN AOV consumes.  The
//  single-scatter behaviour is asserted only as MONOTONICITY, which is
//  the part that is meaningful at one bounce.
// ============================================================

static double AlbedoForColor( const double C, const double betaNVal )
{
    ColorRef color( new UniformColorPainter( RISEPel( C, C, C ), eSpectrumKind_Albedo ) );
    ScalarRef betaM( new UniformScalarPainter( 0.3 ) );
    ScalarRef betaN( new UniformScalarPainter( betaNVal ) );
    ScalarRef alpha( new UniformScalarPainter( 2.0 ) );
    ScalarRef ior( new UniformScalarPainter( 1.55 ) );

    HairPainters hp;
    hp.color  = color.get();
    hp.beta_m = betaM.get();  hp.beta_n = betaN.get();
    hp.alpha  = alpha.get();  hp.ior    = ior.get();

    HairBRDF* brdf = new HairBRDF( hp ); brdf->addref();
    const RayIntersectionGeometric ri = MakeFibreHit( 0.3, 1.1, 0.0 );
    const RISEPel a = brdf->albedo( ri );
    brdf->release();
    return a[1];
}

//! Bind the sigma_a the tier-3 inversion would produce, through tier 2,
//! and read the reflectance back out of `albedo()`.  That is the full
//! C -> sigma_a -> C round trip across both tiers.
static double RoundTripThroughSigmaA( const double C, const double betaNVal )
{
    const double b = betaNVal;
    const double D = 5.969 - 0.215*b + 2.532*b*b - 10.73*b*b*b
                   + 5.574*b*b*b*b + 0.245*b*b*b*b*b;
    const double sigma = ( log(C) / D ) * ( log(C) / D );

    ScalarRef sigmaA( new UniformScalarPainter( sigma ) );
    ScalarRef betaM( new UniformScalarPainter( 0.3 ) );
    ScalarRef betaN( new UniformScalarPainter( betaNVal ) );
    ScalarRef alpha( new UniformScalarPainter( 2.0 ) );
    ScalarRef ior( new UniformScalarPainter( 1.55 ) );

    const HairPainters hp = MakeSigmaAPainters( *sigmaA, *betaM, *betaN, *alpha, *ior );
    HairBRDF* brdf = new HairBRDF( hp ); brdf->addref();
    const RayIntersectionGeometric ri = MakeFibreHit( 0.3, 1.1, 0.0 );
    const RISEPel a = brdf->albedo( ri );
    brdf->release();
    return a[1];
}

//! Single-scatter throughput for a tier-3 colour.
static double ColorThroughput( const double C )
{
    ColorRef color( new UniformColorPainter( RISEPel( C, C, C ), eSpectrumKind_Albedo ) );
    ScalarRef betaM( new UniformScalarPainter( 0.3 ) );
    ScalarRef betaN( new UniformScalarPainter( 0.3 ) );
    ScalarRef alpha( new UniformScalarPainter( 2.0 ) );
    ScalarRef ior( new UniformScalarPainter( 1.55 ) );

    HairPainters hp;
    hp.color  = color.get();
    hp.beta_m = betaM.get();  hp.beta_n = betaN.get();
    hp.alpha  = alpha.get();  hp.ior    = ior.get();

    HairSPF* spf = new HairSPF( hp ); spf->addref();
    IORStack iorStack = MakeTestIORStack( g_stubObject );
    const RayIntersectionGeometric ri = MakeFibreHit( 0.3, 1.1, 0.0 );
    const SPFStats st = RunSPFSamples( *spf, ri, iorStack, false, 0, 1, 40000 );
    spf->release();
    return st.meanKray;
}

static void RunInversionRoundTrip()
{
    std::cout << "=== 6. Tier-3 inversion round trip ===" << std::endl;

    const double Cs[4]    = { 0.9, 0.5, 0.2, 0.05 };
    const double betaNs[2] = { 0.15, 0.6 };

    for( int bi = 0; bi < 2; bi++ )
    {
        for( int i = 0; i < 4; i++ )
        {
            // Tier 3 reports the authored colour verbatim.
            const double direct = AlbedoForColor( Cs[i], betaNs[bi] );
            char l1[160];
            snprintf( l1, sizeof(l1), "tier-3 albedo C=%.2f bn=%.2f", Cs[i], betaNs[bi] );
            Check( Near( direct, Cs[i], 0.02 ), l1, direct, Cs[i] );

            // Full C -> sigma_a -> C round trip across the two tiers.
            const double rt = RoundTripThroughSigmaA( Cs[i], betaNs[bi] );
            char l2[160];
            snprintf( l2, sizeof(l2), "C->sigma_a->C round trip C=%.2f bn=%.2f", Cs[i], betaNs[bi] );
            Check( Near( rt, Cs[i], kInversionTol ), l2, rt, Cs[i] );

            std::cout << "  C=" << Cs[i] << " bn=" << betaNs[bi]
                      << "  albedo=" << std::setprecision(6) << direct
                      << "  roundtrip=" << rt << std::endl;
        }
    }

    // Single-scatter monotonicity: darker authored colour must not
    // scatter MORE energy at one bounce.
    double prev = 2.0;
    for( int i = 0; i < 4; i++ )
    {
        const double t = ColorThroughput( Cs[i] );
        char label[128];
        snprintf( label, sizeof(label), "single-scatter throughput monotone at C=%.2f", Cs[i] );
        CheckTrue( t < prev, label );
        std::cout << "  C=" << Cs[i] << " single-scatter throughput=" << std::setprecision(6) << t << std::endl;
        prev = t;
    }
}

// ============================================================
//  Roughness floor
// ============================================================

static void RunRoughnessFloor()
{
    std::cout << "=== 7. Roughness floors ===" << std::endl;

    // beta authored below the floor must behave EXACTLY as the floor:
    // the clamp lives at evaluation, so a painter cannot sneak under it.
    ScalarRef sigmaA( new UniformScalarPainter( 0.4 ) );
    ScalarRef alpha( new UniformScalarPainter( 2.0 ) );
    ScalarRef ior( new UniformScalarPainter( 1.55 ) );
    ScalarRef tiny( new UniformScalarPainter( 1e-6 ) );
    ScalarRef floorB( new UniformScalarPainter( 0.05 ) );

    IORStack iorStack = MakeTestIORStack( g_stubObject );
    const RayIntersectionGeometric ri = MakeFibreHit( 0.3, 1.1, 0.1 );
    const Vector3 wi = FibreDir( ri, -0.2, 2.4 );

    const HairPainters hpTiny  = MakeSigmaAPainters( *sigmaA, *tiny, *tiny, *alpha, *ior );
    const HairPainters hpFloor = MakeSigmaAPainters( *sigmaA, *floorB, *floorB, *alpha, *ior );

    HairBRDF* bTiny  = new HairBRDF( hpTiny );  bTiny->addref();
    HairBRDF* bFloor = new HairBRDF( hpFloor ); bFloor->addref();
    HairSPF*  sTiny  = new HairSPF( hpTiny );   sTiny->addref();
    HairSPF*  sFloor = new HairSPF( hpFloor );  sFloor->addref();

    const RISEPel vT = bTiny->value( wi, ri );
    const RISEPel vF = bFloor->value( wi, ri );
    Check( Near( vT[1], vF[1], 1e-12 ), "beta below floor == beta at floor (value)", vT[1], vF[1] );

    const double pT = sTiny->Pdf( ri, wi, iorStack );
    const double pF = sFloor->Pdf( ri, wi, iorStack );
    Check( Near( pT, pF, 1e-12 ), "beta below floor == beta at floor (Pdf)", pT, pF );

    // And the floored model must still be finite and non-delta.
    CheckTrue( RISE::IsFiniteDouble( vT[1] ) && vT[1] >= 0, "floored value finite" );
    CheckTrue( pT > 0, "floored Pdf strictly positive" );

    std::cout << "  value=" << std::setprecision(8) << vT[1] << "  pdf=" << pT << std::endl;

    sFloor->release(); sTiny->release(); bFloor->release(); bTiny->release();
}

// ============================================================
//  Material aggregate
// ============================================================

static void RunMaterial()
{
    std::cout << "=== 8. HairMaterial aggregate ===" << std::endl;

    ScalarRef sigmaA( new UniformScalarPainter( 0.4 ) );
    ScalarRef betaM( new UniformScalarPainter( 0.3 ) );
    ScalarRef betaN( new UniformScalarPainter( 0.3 ) );
    ScalarRef alpha( new UniformScalarPainter( 2.0 ) );
    ScalarRef ior( new UniformScalarPainter( 1.55 ) );

    const HairPainters hp = MakeSigmaAPainters( *sigmaA, *betaM, *betaN, *alpha, *ior );
    HairMaterial* mat = new HairMaterial( hp ); mat->addref();

    CheckTrue( mat->GetBSDF() != 0, "HairMaterial exposes a BSDF (BDPT/VCM connectable)" );
    CheckTrue( mat->GetSPF()  != 0, "HairMaterial exposes an SPF" );
    CheckTrue( mat->GetEmitter() == 0, "HairMaterial does not emit" );
    CheckTrue( !mat->IsVolumetric(), "HairMaterial is not volumetric" );

    IORStack iorStack = MakeTestIORStack( g_stubObject );
    const RayIntersectionGeometric ri = MakeFibreHit( 0.3, 1.1, 0.1 );
    const SpecularInfo si = mat->GetSpecularInfo( ri, iorStack );
    CheckTrue( !si.isSpecular, "HairMaterial reports NON-specular (SMS must ignore hair)" );

    // The material's Pdf hook must route through the SPF, or MIS
    // silently degrades to unweighted NEE.
    const Vector3 wi = FibreDir( ri, -0.2, 2.4 );
    const double matPdf = mat->Pdf( wi, ri, iorStack );
    const double spfPdf = mat->GetSPF()->Pdf( ri, wi, iorStack );
    Check( Near( matPdf, spfPdf, 1e-12 ), "IMaterial::Pdf delegates to the SPF", matPdf, spfPdf );
    CheckTrue( matPdf > 0, "IMaterial::Pdf is non-zero (MIS enabled)" );

    // Every sampled lobe must carry the glossy tag and be non-delta.
    RandomNumberGenerator rng;
    IndependentSampler sampler( rng );
    int badType = 0, badDelta = 0, badCount = 0;
    for( int i = 0; i < 5000; i++ )
    {
        ScatteredRayContainer scattered;
        mat->GetSPF()->Scatter( ri, sampler, scattered, iorStack );
        if( scattered.Count() != 1 ) { badCount++; continue; }
        if( scattered[0].type != ScatteredRay::eRayReflection ) { badType++; }
        if( scattered[0].isDelta ) { badDelta++; }
    }
    CheckTrue( badCount == 0,  "Scatter populates exactly one ScatteredRay" );
    CheckTrue( badType == 0,   "every hair lobe is tagged eRayReflection" );
    CheckTrue( badDelta == 0,  "no hair lobe is delta" );

    mat->release();
}

// ============================================================
//  main
// ============================================================

int main()
{
    std::cout << "===== HairBSDFTest (Chiang et al. 2016 near-field hair BCSDF) =====" << std::endl;
    std::cout << std::endl;

    g_stubObject = new StubObject();
    g_stubObject->addref();

    RunFurnace();
    std::cout << std::endl;
    RunEstimatorCrossCheck();
    std::cout << std::endl;
    RunPdfIntegral();
    std::cout << std::endl;
    RunEvaluateKrayNM();
    std::cout << std::endl;
    RunMelaninLadder();
    std::cout << std::endl;
    RunInversionRoundTrip();
    std::cout << std::endl;
    RunRoughnessFloor();
    std::cout << std::endl;
    RunMaterial();
    std::cout << std::endl;

    g_stubObject->release();

    std::cout << "===== Summary =====" << std::endl;
    std::cout << "checks run:    " << g_checks << std::endl;
    std::cout << "checks failed: " << g_failures << std::endl;

    if( g_failures > 0 ) {
        std::cout << std::endl << "HairBSDFTest FAILED" << std::endl;
        return 1;
    }
    std::cout << std::endl << "All hair BSDF tests passed!" << std::endl;
    return 0;
}
