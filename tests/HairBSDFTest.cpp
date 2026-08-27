//////////////////////////////////////////////////////////////////////
//
//  HairBSDFTest.cpp - Validation of the Chiang et al. 2016 near-field
//    hair BCSDF (src/Library/Materials/HairBSDF.{h,cpp}).
//
//  Fourteen groups of tests:
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
//       1b repeats both at the PARAMETER CORNERS: the beta floor (0.05,
//       the DEEPEST cell in LogBesselI0's asymptotic branch -- a reaches
//       ~2.7e3; the interior grid above already reaches ~41-610, so the
//       branch itself is exercised well before this corner), the beta
//       ceiling (1.0), the post-clamp fibre edge (h = +/- 0.9995), and
//       near-grazing theta_o (88 degrees).
//
//    2. SAMPLE <-> PDF CONSISTENCY.  Every sampled direction must
//       report a strictly positive `Pdf`, and that `Pdf` must equal the
//       pdf the sample carried (a WIRING identity -- see the note in
//       RunSPFSamples).  Plus the estimator cross-check
//       E[value * cos / pdf] (over SPF samples) == the quadrature of
//       INTEGRAL value * cos, at a NON-zero sigma_a so the ratio is not
//       trivially 1; 2c repeats it at the same corners as 1b.
//
//    3. PdfNM integrates to 1 over the sphere.
//
//    4. EvaluateKrayNM CONSISTENCY (also wiring identities -- see the
//       note above RunEvaluateKrayNM).  At the hero wavelength it must
//       reproduce the krayNM that ScatterNM produced; at a companion
//       wavelength it must reproduce the integrator's own fallback
//       route, valueNM * |cos| / pdf_hero.
//
//    5. MELANIN LADDER.  Increasing eumelanin lowers throughput
//       monotonically; pheomelanin reddens (throughput at 650 nm above
//       throughput at 450 nm).
//
//    6. TIER-3 INVERSION ROUND TRIP.  colour C -> sigma_a -> C, plus
//       the achromatic R-lobe Fresnel term `albedo()` must carry.
//
//    7. ROUGHNESS FLOORS.  8. MATERIAL AGGREGATE.
//
//    9. COLOUR-TIER MISCONFIGURATION.  A material that binds two tiers
//       must resolve to the mid-brown fallback its constructor logs,
//       not to a silent priority-order pick.
//
//   10. beta_m / beta_n AXIS DISCRIMINATOR.  Groups 1-4 are all
//       invariant under transposing the two roughnesses; this one is
//       not.
//
//   11. CUTICLE-TILT DIRECTION.  A rotation is measure preserving, so
//       every energy check above survives a flipped tilt sign; this one
//       pins the sign and the 2k-alpha recurrence.
//
//   12. ABSORPTION PATH LENGTH.  The one CLOSED-FORM absorption check:
//       at h = 0, theta_o = 0 the internal path is exactly 2 fibre
//       diameters, so A_1 == (1-F)^2 exp(-2 sigma_a) with F written out
//       longhand.  Plus an oblique cell against
//       2 cos(gamma_t)/cos(theta_t).
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
#include "../src/Library/Materials/HairMedullaProfile.h"
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
//!
//! The beta = kMinBeta (0.05) corner cells (group 1b) are the tight
//! case: the TT lobe there is ~0.019 rad wide, i.e. only ~5.5 of these
//! quadrature cells land across it.  That is NOT independently justified
//! to be enough -- it is justified by the MEASURED group-1b results
//! themselves, which is what the corner cells exist to check: every
//! beta = 0.05 "INTEGRAL value*cos" quadrature comes in within ~0.2% of
//! 1 (worst observed 0.99824), an order of magnitude inside the 2%
//! (`kFurnaceQuadTol`) gate.  If a future change to the model or to
//! these corner cells' geometry ever pushes that discretisation error
//! close to 2%, the fix is to raise `kQuadTheta`/`kQuadPhi` for the
//! corner cells specifically and re-measure -- not to widen the
//! tolerance.
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

//! EXPLICIT RNG SEED.  `RandomNumberGenerator`'s default argument is
//! `rand()`, i.e. libc's global generator in whatever state the process
//! left it -- so an unseeded construction here is only accidentally
//! reproducible, and stops being so the moment anything above it draws a
//! number.  Every generator in this file is constructed with this
//! constant, which makes the Monte-Carlo means below deterministic
//! run-to-run and a failure reproducible from the binary alone.
//! (MERSENNE53 is on in Config.common, so the seeded overload exists.)
static const unsigned int kRNGSeed = 20260826u;

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

//! `nTheta` / `nPhi` default to the group-1 resolution.  The medulla
//! groups (16 and 18) pass a coarser one: they sweep a large PARAMETER
//! grid rather than the roughness corners, their beta_m never goes
//! below 0.05, and 450^2 still puts ~20 theta cells across the
//! narrowest lobe those groups reach -- measured worst case there is
//! 6.6e-4 off 1, two orders inside the 2 % gate.  Group 1 keeps 900^2.
static double QuadratureBSDFEnergy(
    const HairBRDF& brdf, const RayIntersectionGeometric& ri, const int channel,
    const int nTheta = kQuadTheta, const int nPhi = kQuadPhi )
{
    const int kQuadTheta = nTheta;
    const int kQuadPhi   = nPhi;
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

    RandomNumberGenerator rng( kRNGSeed );
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

        // WIRING check, not an accuracy check.  `DoScatter` computes the
        // pdf it stores by calling `EvalPdf`, and `Pdf`/`PdfNM` call the
        // same `EvalPdf` on the same (ri, wi) -- so this is an algebraic
        // IDENTITY between two evaluations of one pure function, and it
        // is 1e-9-tight for that reason and no other.  What it does
        // catch: a direction rebuilt inconsistently between sampling and
        // query, a stale `Resolved`, a PdfNM that forgets to delegate, or
        // a future lobe-index shortcut in `Pdf`.  What it does NOT
        // evidence: that either number is the CORRECT density -- that is
        // group 3 (PdfNM integrates to 1) and group 2b (the estimator
        // cross-check against an independent quadrature).
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
//  1b.  Furnace + pdf CORNERS
//
//  The main grid above samples the interior of the parameter space.
//  These are the edges, where the model is most likely to fall over:
//    * beta = kMinBeta (0.05), the FLOOR.  This is the DEEPEST cell in
//      `LogBesselI0`'s large-argument asymptotic branch, not the only one
//      that reaches it: the TT variance here is ~3.7e-4, so a = cos.cos/v
//      reaches ~2.7e3 (the direct sinh(1/v)/I0(a) form would overflow),
//      while the INTERIOR grid above (RunFurnace, beta down to 0.1)
//      already reaches a ~41-610 and is well inside the same branch.
//    * beta = kMaxBeta (1.0), the CEILING, where the pow(b, 20) / pow(b,
//      22) terms in the beta -> variance / beta -> s remaps are at full
//      strength.
//    * h = +/- 0.9995, the post-clamp fibre EDGE, where cos(gamma_o) is
//      ~0.032, the Fresnel term is ~0.85 and the transmissive orders are
//      nearly starved -- the regime the h clamp exists to keep finite.
//    * theta_o = 88 degrees, near-grazing along the fibre, where
//      eta' ~ 34 and the tilted TRT lobe rotates past the pole.
//  Sample counts are trimmed (the SPF furnace is an exact identity at
//  sigma_a == 0, so it needs no statistics) while the quadrature keeps
//  full resolution, since it is the only check that can actually fail.
// ============================================================

static void RunFurnaceCorners()
{
    std::cout << "=== 1b. Furnace corners (beta floor / ceiling, fibre edge, grazing) ===" << std::endl;

    ScalarRef sigmaZero( new UniformScalarPainter( 0.0 ) );
    ScalarRef alpha( new UniformScalarPainter( 2.0 ) );
    ScalarRef ior( new UniformScalarPainter( 1.55 ) );

    IORStack iorStack = MakeTestIORStack( g_stubObject );

    const int kCornerSPFSamples = 20000;

    // (beta_m, beta_n, h, theta_o)
    const double cells[16][4] = {
        { 0.05, 0.05,  0.0,     0.35 },
        { 0.05, 0.05,  0.9995,  0.35 },
        { 0.05, 0.05, -0.9995,  0.35 },
        { 0.05, 1.0,   0.0,     0.35 },
        { 0.05, 1.0,   0.9995,  0.35 },
        { 1.0,  0.05,  0.0,     0.35 },
        { 1.0,  0.05, -0.9995,  0.35 },
        { 1.0,  1.0,   0.0,     0.35 },
        { 1.0,  1.0,   0.9995,  0.35 },
        { 1.0,  1.0,  -0.9995,  0.35 },
        // Near-grazing along the fibre axis (88 degrees).
        { 0.05, 0.05,  0.0,     1.53589 },
        { 0.05, 0.05,  0.9995,  1.53589 },
        { 0.3,  0.3,   0.0,     1.53589 },
        { 0.3,  0.3,   0.9995,  1.53589 },
        { 1.0,  1.0,   0.0,     1.53589 },
        { 1.0,  1.0,  -0.9995,  1.53589 }
    };

    for( int i = 0; i < 16; i++ )
    {
        ScalarRef betaM( new UniformScalarPainter( cells[i][0] ) );
        ScalarRef betaN( new UniformScalarPainter( cells[i][1] ) );

        const HairPainters hp = MakeSigmaAPainters( *sigmaZero, *betaM, *betaN, *alpha, *ior );
        HairBRDF* brdf = new HairBRDF( hp );  brdf->addref();
        HairSPF*  spf  = new HairSPF( hp );   spf->addref();

        const RayIntersectionGeometric ri = MakeFibreHit( cells[i][3], 0.9, cells[i][2] );

        char label[192];
        snprintf( label, sizeof(label), "corner bm=%.2f bn=%.2f h=%+.4f thO=%.2f",
                  cells[i][0], cells[i][1], cells[i][2], cells[i][3] );

        const SPFStats rgb = RunSPFSamples( *spf, ri, iorStack, false, 0, 1, kCornerSPFSamples );
        const SPFStats nm  = RunSPFSamples( *spf, ri, iorStack, true, 550.0, 0, kCornerSPFSamples );

        Check( Near( rgb.meanKray, 1.0, kFurnaceSPFTol ),
               std::string(label) + " SPF/RGB E[kray]", rgb.meanKray, 1.0 );
        Check( Near( nm.meanKray, 1.0, kFurnaceSPFTol ),
               std::string(label) + " SPF/NM E[krayNM]", nm.meanKray, 1.0 );
        CheckTrue( rgb.nullScatters == 0 && nm.nullScatters == 0,
                   std::string(label) + " no null scatters" );
        CheckTrue( rgb.pdfMismatches == 0 && nm.pdfMismatches == 0,
                   std::string(label) + " Pdf() == sample pdf" );

        const double quad = QuadratureBSDFEnergy( *brdf, ri, 1 );
        Check( Near( quad, 1.0, kFurnaceQuadTol ),
               std::string(label) + " INTEGRAL value*cos", quad, 1.0 );

        const double pdfInt = QuadraturePdfNM( *spf, ri, iorStack, 550.0 );
        Check( Near( pdfInt, 1.0, kFurnaceQuadTol ),
               std::string(label) + " INTEGRAL PdfNM", pdfInt, 1.0 );

        std::cout << "  " << label
                  << "  SPF=" << std::setprecision(8) << rgb.meanKray
                  << "  quad=" << quad << "  pdfInt=" << pdfInt << std::endl;

        spf->release();
        brdf->release();
    }
}

// ============================================================
//  2c.  Estimator cross-check at the CORNERS
//
//  The interior estimator check below runs at beta = 0.3.  These repeat
//  it at the roughness floor and ceiling and at the post-clamp fibre
//  edge, with a NON-zero sigma_a so the f/pdf ratio is not the trivial
//  1 the sigma_a == 0 furnace produces.
// ============================================================

static void RunEstimatorCorners()
{
    std::cout << "=== 2c. Estimator cross-check at the corners ===" << std::endl;

    ScalarRef sigmaA( new UniformScalarPainter( 0.6 ) );
    ScalarRef alpha( new UniformScalarPainter( 2.0 ) );
    ScalarRef ior( new UniformScalarPainter( 1.55 ) );

    IORStack iorStack = MakeTestIORStack( g_stubObject );

    // (beta_m, beta_n, h, theta_o)
    const double cells[5][4] = {
        { 0.05, 0.05,  0.0,     -0.2 },
        { 1.0,  1.0,   0.0,     -0.2 },
        { 0.05, 1.0,   0.9995,  -0.2 },
        { 1.0,  0.05, -0.9995,  -0.2 },
        { 0.3,  0.3,   0.4,      1.53589 }
    };

    for( int i = 0; i < 5; i++ )
    {
        ScalarRef betaM( new UniformScalarPainter( cells[i][0] ) );
        ScalarRef betaN( new UniformScalarPainter( cells[i][1] ) );

        const HairPainters hp = MakeSigmaAPainters( *sigmaA, *betaM, *betaN, *alpha, *ior );
        HairBRDF* brdf = new HairBRDF( hp );  brdf->addref();
        HairSPF*  spf  = new HairSPF( hp );   spf->addref();

        const RayIntersectionGeometric ri = MakeFibreHit( cells[i][3], 2.1, cells[i][2] );

        const SPFStats st = RunSPFSamples( *spf, ri, iorStack, false, 0, 1, kSPFSamples );
        const double quad = QuadratureBSDFEnergy( *brdf, ri, 1 );

        char label[192];
        snprintf( label, sizeof(label), "estimator corner bm=%.2f bn=%.2f h=%+.4f thO=%.2f",
                  cells[i][0], cells[i][1], cells[i][2], cells[i][3] );

        Check( Near( st.meanKray, quad, kEstimatorTol ),
               std::string(label) + " E[value*cos/pdf] == INTEGRAL value*cos",
               st.meanKray, quad );
        CheckTrue( st.pdfMismatches == 0, std::string(label) + " Pdf() == sample pdf" );
        CheckTrue( st.nullScatters == 0,  std::string(label) + " no null scatters" );

        std::cout << "  " << label << "  E[kray]=" << std::setprecision(8) << st.meanKray
                  << "  quad=" << quad << std::endl;

        spf->release();
        brdf->release();
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

//! WHAT THIS GROUP IS AND IS NOT.  Both checks below are algebraic
//! IDENTITIES -- `EvaluateKrayNM`, `ScatterNM` and `valueNM` are three
//! entry points into the SAME `EvalFsum` / `EvalPdf` pair, so at
//! matching arguments they cannot disagree except through a wiring bug.
//! That is precisely what is being guarded, and it is worth guarding:
//! the contract `EvaluateKrayNM` must satisfy is "return exactly what
//! the integrator's own fallback would have computed", and the only way
//! to break it is to wire it wrong (wrong pdf source, a lobe-index
//! shortcut, a forgotten cosine, or -- the real hazard -- a sampling pdf
//! that is NOT wavelength independent after all, which would make the
//! hero identity fail).  It is NOT evidence that the underlying spectral
//! values are physically right; that comes from groups 1, 2b and 5.
static void RunEvaluateKrayNM()
{
    std::cout << "=== 4. EvaluateKrayNM consistency (wiring identities) ===" << std::endl;

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

    RandomNumberGenerator rng( kRNGSeed );
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

//! What `albedo()` must report for an authored (tier 3) or implied
//! (tiers 1-2) reflectance C: the absorption-driven part PLUS the
//! achromatic R-lobe surface reflection, composited C + (1 - C) * F_avg
//! with F_avg the normal-incidence dielectric Fresnel for `eta`.
//! Chiang's C <-> sigma_a fit describes only the light that goes through
//! the fibre, so without the surface term `albedo()` reports 0 for black
//! hair -- and OIDN divides by the albedo AOV, turning the specular
//! highlight into unguided noise.  See HairBSDF.h / ReflectanceRGB.
static double ExpectedAlbedo( const double C, const double eta )
{
    const double r = ( eta - 1 ) / ( eta + 1 );
    return C + ( 1 - C ) * r * r;
}

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
            // Tier 3 reports the authored colour, plus the achromatic
            // R-lobe Fresnel term the AOV must carry.
            const double want = ExpectedAlbedo( Cs[i], 1.55 );
            const double direct = AlbedoForColor( Cs[i], betaNs[bi] );
            char l1[160];
            snprintf( l1, sizeof(l1), "tier-3 albedo C=%.2f bn=%.2f", Cs[i], betaNs[bi] );
            Check( Near( direct, want, 0.02 ), l1, direct, want );

            // Full C -> sigma_a -> C round trip across the two tiers.
            const double rt = RoundTripThroughSigmaA( Cs[i], betaNs[bi] );
            char l2[160];
            snprintf( l2, sizeof(l2), "C->sigma_a->C round trip C=%.2f bn=%.2f", Cs[i], betaNs[bi] );
            Check( Near( rt, want, kInversionTol ), l2, rt, want );

            std::cout << "  C=" << Cs[i] << " bn=" << betaNs[bi]
                      << "  albedo=" << std::setprecision(6) << direct
                      << "  roundtrip=" << rt << std::endl;
        }
    }

    // The R-lobe Fresnel floor: pure black hair still reflects its
    // surface highlight, so the AOV must NOT be 0 (OIDN divides by it).
    const double blackAlbedo = AlbedoForColor( 0.0, 0.3 );
    Check( Near( blackAlbedo, ExpectedAlbedo( 0.0, 1.55 ), 1e-9 ),
           "albedo(C=0) == R-lobe Fresnel, not 0",
           blackAlbedo, ExpectedAlbedo( 0.0, 1.55 ) );
    CheckTrue( blackAlbedo > 0.04, "black hair albedo AOV is non-zero" );
    std::cout << "  C=0 albedo=" << std::setprecision(6) << blackAlbedo << std::endl;

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
    RandomNumberGenerator rng( kRNGSeed );
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
//  9.  Colour-tier misconfiguration really does fall back
//
//  `HairScatteringBase`'s constructor logs "exactly one colour tier ...
//  falling back to a uniform mid-brown sigma_a" whenever
//  ActiveColorTierCount() != 1.  This asserts the CODE keeps that
//  promise.  The failure mode being guarded is a resolution path that
//  quietly applies a PRIORITY ORDER among the bound tiers instead --
//  which renders a plausible-looking image while the log says something
//  else, the worst possible combination for an author debugging a scene.
// ============================================================

static void RunColorTierMisconfig()
{
    std::cout << "=== 9. Colour-tier misconfiguration ===" << std::endl;

    ScalarRef betaM( new UniformScalarPainter( 0.3 ) );
    ScalarRef betaN( new UniformScalarPainter( 0.3 ) );
    ScalarRef alpha( new UniformScalarPainter( 2.0 ) );
    ScalarRef ior( new UniformScalarPainter( 1.55 ) );

    // Two tiers at once, each deliberately FAR from the mid-brown
    // fallback (kFallbackSigmaA == 1.0) and far from each other, so any
    // priority-order resolution is unmistakable: sigma_a 0.05 is nearly
    // white hair, eumelanin 3.0 is sigma_a ~1.55 / 2.09 / 3.88 per RGB.
    ScalarRef sigmaLight( new UniformScalarPainter( 0.05 ) );
    ScalarRef eumelDark( new UniformScalarPainter( 3.0 ) );
    ScalarRef midBrown( new UniformScalarPainter( 1.0 ) );

    HairPainters bad;
    bad.sigma_a   = sigmaLight.get();
    bad.eumelanin = eumelDark.get();
    bad.beta_m = betaM.get();  bad.beta_n = betaN.get();
    bad.alpha  = alpha.get();  bad.ior    = ior.get();

    CheckTrue( bad.ActiveColorTierCount() == 2,
               "the misconfigured painter set really does bind 2 tiers" );

    const HairPainters ref = MakeSigmaAPainters( *midBrown, *betaM, *betaN, *alpha, *ior );

    HairBRDF* bBad = new HairBRDF( bad ); bBad->addref();
    HairBRDF* bRef = new HairBRDF( ref ); bRef->addref();

    // Also confirm the fallback is NOT what either bound tier would have
    // produced -- otherwise the assertions below would be vacuous.
    const HairPainters lightOnly = MakeSigmaAPainters( *sigmaLight, *betaM, *betaN, *alpha, *ior );
    HairBRDF* bLight = new HairBRDF( lightOnly ); bLight->addref();

    const RayIntersectionGeometric ri = MakeFibreHit( 0.3, 1.1, 0.2 );

    int rgbMismatch = 0, nmMismatch = 0, vacuous = 0;
    const double dirs[5][2] = { {-0.2, 2.4}, {0.3, 0.6}, {-0.9, 4.1}, {0.05, 1.1}, {1.1, 5.3} };
    for( int d = 0; d < 5; d++ )
    {
        const Vector3 wi = FibreDir( ri, dirs[d][0], dirs[d][1] );

        const RISEPel vBad = bBad->value( wi, ri );
        const RISEPel vRef = bRef->value( wi, ri );
        for( unsigned int c = 0; c < 3; c++ ) {
            if( !Near( vBad[c], vRef[c], 1e-12 ) ) { rgbMismatch++; }
        }

        const double nBad = bBad->valueNM( wi, ri, 610.0 );
        const double nRef = bRef->valueNM( wi, ri, 610.0 );
        if( !Near( nBad, nRef, 1e-12 ) ) { nmMismatch++; }

        // Vacuity guard: the light-hair tier must NOT already agree.
        if( Near( bLight->value( wi, ri )[1], vRef[1], 1e-6 ) ) { vacuous++; }
    }

    CheckTrue( rgbMismatch == 0,
               "2-tier misconfig resolves to the mid-brown fallback (value, RGB)" );
    CheckTrue( nmMismatch == 0,
               "2-tier misconfig resolves to the mid-brown fallback (valueNM)" );
    CheckTrue( vacuous == 0,
               "the fallback differs from the bound sigma_a tier (assertion is not vacuous)" );

    // albedo() reads the same tier resolution through ReflectanceRGB.
    const RISEPel aBad = bBad->albedo( ri );
    const RISEPel aRef = bRef->albedo( ri );
    Check( Near( aBad[1], aRef[1], 1e-12 ),
           "2-tier misconfig albedo == mid-brown albedo", aBad[1], aRef[1] );

    std::cout << "  fallback value=" << std::setprecision(8) << bRef->value( FibreDir( ri, -0.2, 2.4 ), ri )[1]
              << "  (sigma_a-only tier would be "
              << bLight->value( FibreDir( ri, -0.2, 2.4 ), ri )[1] << ")" << std::endl;

    bLight->release(); bRef->release(); bBad->release();
}

// ============================================================
//  10.  beta_m and beta_n control DIFFERENT axes
//
//  Every furnace / pdf / estimator check above is invariant under
//  swapping the two roughness parameters: both integrals stay 1 and the
//  estimator still matches the quadrature, because M_p and N_p are each
//  normalised on their own axis.  So the whole suite would pass with
//  beta_m and beta_n transposed in `Resolve`, or with the v[] ladder and
//  the logistic scale s reading each other's input.
//
//  This test breaks that symmetry.  Two configurations, transposed:
//      A = (beta_m 0.05, beta_n 0.8)  narrow longitudinal, broad azimuthal
//      B = (beta_m 0.8,  beta_n 0.05) broad  longitudinal, narrow azimuthal
//  are evaluated at two probe directions off the R-lobe specular peak:
//      P_phi   -- ON the longitudinal peak, 0.3 rad off in AZIMUTH
//      P_theta -- ON the azimuthal peak,    0.4 rad off LONGITUDINALLY
//  A must dominate at P_phi for TWO compounding reasons, not one: it sits
//  exactly ON its own (narrow, beta_m 0.05) M_p peak, which is TALLER
//  there than B's broad M_p is at ITS peak (a narrower lobe normalised to
//  the same area is taller at its centre); AND its N_p is broad
//  (beta_n 0.8), so it still carries real weight 0.3 rad off-centre where
//  B's near-delta N_p (beta_n 0.05) has already died.  B must dominate at
//  P_theta by the mirrored pair: taller N_p at its own (narrow,
//  beta_n 0.05) peak, AND a broad M_p (beta_m 0.8) that still survives
//  0.4 rad off-peak where A's near-delta M_p has died.  Transposing the
//  two parameters anywhere in the model flips both verdicts.
//
//  sigma_a is set very high so only the p = 0 lobe carries energy and
//  the geometry of the comparison is unambiguous; alpha is 0 so the
//  specular peak sits at the untilted theta_i = -theta_o, phi_i = phi_o
//  (h = 0 => gamma_o = 0 => the R lobe's ideal azimuthal exit is 0).
// ============================================================

//! The BARE Chiang lobe sum at `wi`: `value()` times the shading cosine
//! it divides out.  Comparing fsum rather than value keeps the
//! 1/|wi . N| reconciliation out of the ratio, so a ratio between two
//! directions reflects the MODEL and not the reconciliation.
static double Fsum( const HairBRDF& brdf, const RayIntersectionGeometric& ri, const Vector3& wi )
{
    return brdf.value( wi, ri )[1] * fabs( Vector3Ops::Dot( wi, ri.onb.w() ) );
}

static void RunRoughnessAxisDiscriminator()
{
    std::cout << "=== 10. beta_m / beta_n drive different axes ===" << std::endl;

    ScalarRef sigmaOpaque( new UniformScalarPainter( 20.0 ) );   // kills TT / TRT / residual
    ScalarRef alphaZero( new UniformScalarPainter( 0.0 ) );
    ScalarRef ior( new UniformScalarPainter( 1.55 ) );

    ScalarRef bLo( new UniformScalarPainter( 0.05 ) );           // the model's floor
    ScalarRef bHi( new UniformScalarPainter( 0.8 ) );

    const HairPainters hpA = MakeSigmaAPainters( *sigmaOpaque, *bLo, *bHi, *alphaZero, *ior );
    const HairPainters hpB = MakeSigmaAPainters( *sigmaOpaque, *bHi, *bLo, *alphaZero, *ior );

    HairBRDF* A = new HairBRDF( hpA ); A->addref();
    HairBRDF* B = new HairBRDF( hpB ); B->addref();

    const double thetaO = 0.4;
    const double phiO   = 1.0;
    const RayIntersectionGeometric ri = MakeFibreHit( thetaO, phiO, 0.0 );

    // R-lobe specular peak with alpha == 0 and h == 0.
    const double thetaPeak = -thetaO;

    const Vector3 wPhi   = FibreDir( ri, thetaPeak,       phiO + 0.3 );
    const Vector3 wTheta = FibreDir( ri, thetaPeak + 0.4, phiO       );

    const double aPhi = Fsum( *A, ri, wPhi ),   bPhi = Fsum( *B, ri, wPhi );
    const double aTh  = Fsum( *A, ri, wTheta ), bTh  = Fsum( *B, ri, wTheta );

    CheckTrue( aPhi > 0 && bTh > 0, "both discriminator winners are strictly positive" );
    CheckTrue( aPhi > 2.0 * bPhi,
               "broad beta_n survives a 0.3 rad AZIMUTHAL offset where narrow beta_n does not" );
    CheckTrue( bTh > 2.0 * aTh,
               "broad beta_m survives a 0.4 rad LONGITUDINAL offset where narrow beta_m does not" );

    std::cout << "  azimuthal probe:    A(bm.05,bn.8)=" << std::setprecision(6) << aPhi
              << "  B(bm.8,bn.05)=" << bPhi << std::endl;
    std::cout << "  longitudinal probe: A(bm.05,bn.8)=" << aTh
              << "  B(bm.8,bn.05)=" << bTh << std::endl;

    B->release(); A->release();
}

// ============================================================
//  11.  Cuticle-tilt DIRECTION
//
//  The 2k-alpha recurrence rotates the R lobe's outgoing longitudinal
//  angle by -2 alpha (ApplyLobeTilt, p == 0), which moves the R-lobe
//  specular peak from theta_i = -theta_o to theta_i = -theta_o + 2 alpha.
//  Flipping that sign is invisible to every energy / pdf / estimator
//  check in this file: a rotation is measure preserving, so all the
//  integrals stay exactly 1 either way.  It is, however, the single most
//  visible parameter in a hair render -- it is what separates the white
//  primary highlight from the coloured secondary one.
//
//  The probe pair straddles the UNTILTED peak symmetrically, at
//  theta_i = -theta_o +/- 4 alpha.  With the correct sign the peak sits
//  at -theta_o + 2 alpha, so the + probe is 2 alpha from the peak and
//  the - probe is 6 alpha away; the + probe must therefore win by a wide
//  margin.  A sign flip swaps the two.
//
//  THE alpha == 0 CONTROL IS NOT AN EQUALITY.  M_p is a von-Mises-like
//  lobe on the sphere, not a Gaussian in the flat angle theta, so the two
//  equidistant-from-peak probes are not equal even untilted.  For small v
//  the asymptotic form gives LogI0(a) ~= a - 0.5*log(2*pi*a), so
//      M_p  ~  exp( cos(theta_i + theta_o) / v ) / sqrt(cos_i * cos_o),
//  and with theta_o common to both probes the cos(theta_i+theta_o) factor
//  is IDENTICAL for the +/- pair here (it is an even function of the
//  probes' shared +/-4*alpha offset) -- the entire asymmetry comes from
//  the 1/sqrt(cos_i) prefactor, which is LARGER (i.e. STRONGER, not
//  weaker) for the SMALLER cos_i, i.e. for the probe CLOSER to the pole.
//  Here that is the - probe (theta_i = -theta_o - 4*alpha, further from
//  theta_i = 0 than the + probe).  Predicted ratio
//  sqrt(cos(0.91888) / cos(0.08112)) ~= 0.780, measured ~0.771 -- close
//  enough to confirm the mechanism.  That is the baseline the tilt has to
//  overturn, and it makes the
//  tilted ratio (~5.6) a strictly stronger statement, not a weaker one.
// ============================================================

static void RunCuticleTiltDirection()
{
    std::cout << "=== 11. Cuticle-tilt direction ===" << std::endl;

    ScalarRef sigmaOpaque( new UniformScalarPainter( 20.0 ) );   // isolate the R lobe
    ScalarRef betaM( new UniformScalarPainter( 0.3 ) );
    ScalarRef betaN( new UniformScalarPainter( 0.3 ) );
    ScalarRef ior( new UniformScalarPainter( 1.55 ) );
    ScalarRef alphaTilt( new UniformScalarPainter( 6.0 ) );      // degrees
    ScalarRef alphaZero( new UniformScalarPainter( 0.0 ) );

    const HairPainters hpTilt = MakeSigmaAPainters( *sigmaOpaque, *betaM, *betaN, *alphaTilt, *ior );
    const HairPainters hpFlat = MakeSigmaAPainters( *sigmaOpaque, *betaM, *betaN, *alphaZero, *ior );

    HairBRDF* tilt = new HairBRDF( hpTilt ); tilt->addref();
    HairBRDF* flat = new HairBRDF( hpFlat ); flat->addref();

    const double thetaO = 0.5;
    const double phiO   = 1.0;
    const double aRad   = 6.0 * PI / 180.0;
    const RayIntersectionGeometric ri = MakeFibreHit( thetaO, phiO, 0.0 );

    // Straddle the untilted peak by +/- 2 * (2 alpha).
    const Vector3 wPlus  = FibreDir( ri, -thetaO + 4.0 * aRad, phiO );
    const Vector3 wMinus = FibreDir( ri, -thetaO - 4.0 * aRad, phiO );

    const double tPlus  = Fsum( *tilt, ri, wPlus  );
    const double tMinus = Fsum( *tilt, ri, wMinus );
    const double fPlus  = Fsum( *flat, ri, wPlus  );
    const double fMinus = Fsum( *flat, ri, wMinus );

    // Baseline: with no tilt the + probe is the WEAKER of the pair.
    CheckTrue( fPlus < fMinus,
               "alpha = 0 control: the untilted lobe favours the - probe (ratio < 1)" );
    // ... and the tilt has to overturn that by a wide margin.
    CheckTrue( tPlus > 3.0 * tMinus,
               "R-lobe peak is rotated toward theta_i = -theta_o + 2*alpha (tilt SIGN)" );
    CheckTrue( tPlus > fPlus,
               "the tilt moves the peak TOWARD the + probe (it is not merely broadening)" );

    // The peak really is at -theta_o + 2 alpha, not at the mirror angle
    // and not on the -2 alpha side.
    const double atPeak   = Fsum( *tilt, ri, FibreDir( ri, -thetaO + 2.0 * aRad, phiO ) );
    const double atMirror = Fsum( *tilt, ri, FibreDir( ri, -thetaO, phiO ) );
    const double atAnti   = Fsum( *tilt, ri, FibreDir( ri, -thetaO - 2.0 * aRad, phiO ) );
    CheckTrue( atPeak > atMirror,
               "tilted peak beats the untilted mirror angle" );
    CheckTrue( atPeak > atAnti,
               "tilted peak beats the -2*alpha (sign-flipped) angle" );

    // REGRESSION PIN.  One fixed (theta_o, alpha, h, beta) tuple, tight
    // tolerance.  This guards the 2k-alpha RECURRENCE's FIRST step -- the
    // double-angle step that turns sin(alpha) into sin(2 alpha) -- which
    // is all the R lobe (p == 0, exercised here via `sigmaOpaque` killing
    // every other order) ever reads.  It does NOT reach the SECOND step
    // (sin(4 alpha), recurrence index 2, consumed only by the TRT lobe's
    // p == 2 branch); that index is what the `ApplyLobeTilt` white-box
    // group (13, RunApplyLobeTiltWhiteBox) pins directly, since nothing
    // in an energy/estimator/peak test built on the R lobe can reach it.
    // The ordering checks above only pin the sign; a recurrence that
    // produced sin(alpha) where sin(2 alpha) belongs would keep every
    // ordering here and still shift the highlight.
    const double kTiltPin = 0.1467911904;
    Check( Near( atPeak, kTiltPin, 1e-6 ),
           "tilt regression pin: fsum at (theta_o=0.5, alpha=6deg, h=0, beta=0.3)",
           atPeak, kTiltPin );

    std::cout << "  tilt:  +probe=" << std::setprecision(10) << tPlus
              << "  -probe=" << tMinus << "  ratio=" << tPlus / tMinus << std::endl;
    std::cout << "  flat:  +probe=" << fPlus << "  -probe=" << fMinus
              << "  ratio=" << fPlus / fMinus << std::endl;
    std::cout << "  atPeak=" << atPeak << "  atMirror=" << atMirror
              << "  atAnti=" << atAnti << std::endl;

    flat->release(); tilt->release();
}

// ============================================================
//  12.  Absorption path length -- one CLOSED-FORM cell, one CHANGE-DETECTOR
//
//  Every other absorption assertion in this file is a self-consistency
//  identity or a monotonicity trend.  The AXIAL cell here pins the actual
//  formula from first principles and is the genuinely closed-form check:
//
//  At h = 0 the ray crosses the fibre through its axis: gamma_o = 0,
//  sin(gamma_t) = h / eta' = 0, so cos(gamma_t) = 1.  At theta_o = 0 the
//  ray is perpendicular to the fibre axis: sin(theta_t) = 0, so
//  cos(theta_t) = 1.  The internal optical path is therefore
//      L = 2 cos(gamma_t) / cos(theta_t) = 2      (fibre diameters)
//  EXACTLY, and the TT order's single-pass transmittance is exactly
//  exp(-2 sigma_a), with
//      A_0 = F,   A_1 = (1 - F)^2 exp(-2 sigma_a)
//  and F the normal-incidence dielectric Fresnel ((eta-1)/(eta+1))^2.
//  Nothing here is read back out of the model: F, L and A_1 are all
//  written out longhand from the physics, independent of MakeGeom.
//
//  The OBLIQUE cell (theta_o = 0.6, h = 0.5) is NOT closed-form in the
//  same sense: `wantL` is `MakeGeom`'s own sinThetaT / etap / sinGammaT
//  expressions, retyped longhand in the test rather than derived from an
//  independent physical argument.  It is a CHANGE-DETECTOR against that
//  formula (real coverage -- it pins the gamma_t / eta' geometry the
//  axial cell can't reach, and would catch a sign or index slip in
//  MakeGeom), not a from-first-principles proof the axial cell is.
// ============================================================

static void RunAbsorptionPathLength()
{
    std::cout << "=== 12. Absorption path length (closed form) ===" << std::endl;

    const double eta = 1.55;

    ScalarRef sigmaP( new UniformScalarPainter( 0.4 ) );
    ScalarRef betaM( new UniformScalarPainter( 0.3 ) );
    ScalarRef betaN( new UniformScalarPainter( 0.3 ) );
    ScalarRef alpha( new UniformScalarPainter( 2.0 ) );
    ScalarRef ior( new UniformScalarPainter( eta ) );

    const HairPainters hp = MakeSigmaAPainters( *sigmaP, *betaM, *betaN, *alpha, *ior );
    HairBRDF* brdf = new HairBRDF( hp ); brdf->addref();

    // --- axial cell: h = 0, theta_o = 0 -> L == 2 exactly -------------
    {
        const RayIntersectionGeometric ri = MakeFibreHit( 0.0, 0.9, 0.0 );

        const double r = ( eta - 1 ) / ( eta + 1 );
        const double F = r * r;                     // normal-incidence Fresnel

        const double sigmas[3] = { 0.0, 0.3, 0.7 };
        double ap1[3];
        for( int i = 0; i < 3; i++ )
        {
            Scalar ap[4], L = 0;
            brdf->TestApAndPathLength( ri, sigmas[i], ap, L );
            ap1[i] = ap[1];

            char lab[128];
            snprintf( lab, sizeof(lab), "path length == 2 at h=0,theta_o=0 (sigma_a=%.1f)", sigmas[i] );
            Check( Near( L, 2.0, 1e-12 ), lab, L, 2.0 );

            snprintf( lab, sizeof(lab), "A_0 == normal-incidence Fresnel (sigma_a=%.1f)", sigmas[i] );
            Check( Near( ap[0], F, 1e-12 ), lab, ap[0], F );

            const double wantAp1 = ( 1 - F ) * ( 1 - F ) * exp( -2.0 * sigmas[i] );
            snprintf( lab, sizeof(lab), "A_1 == (1-F)^2 exp(-2 sigma_a) (sigma_a=%.1f)", sigmas[i] );
            Check( Near( ap[1], wantAp1, 1e-12 ), lab, ap[1], wantAp1 );

            std::cout << "  sigma_a=" << sigmas[i] << "  L=" << std::setprecision(12) << L
                      << "  A_0=" << ap[0] << "  A_1=" << ap[1] << std::endl;
        }

        // The exponent's COEFFICIENT (the 2, i.e. the path length) is
        // what this ratio isolates: it is independent of F.
        Check( Near( ap1[2] / ap1[1], exp( -2.0 * ( 0.7 - 0.3 ) ), 1e-12 ),
               "A_1 ratio across sigma_a == exp(-2 * delta sigma_a)",
               ap1[2] / ap1[1], exp( -2.0 * ( 0.7 - 0.3 ) ) );
    }

    // --- oblique cell: L == 2 cos(gamma_t) / cos(theta_t) -------------
    {
        const double thetaO = 0.6;
        const double h      = 0.5;
        const RayIntersectionGeometric ri = MakeFibreHit( thetaO, 0.9, h );

        // Longhand Snell, exactly as the model's MakeGeom derives it.
        const double sinThetaO = sin( thetaO );
        const double cosThetaO = cos( thetaO );
        const double sinThetaT = sinThetaO / eta;
        const double cosThetaT = sqrt( 1 - sinThetaT * sinThetaT );
        const double etap      = sqrt( eta * eta - sinThetaO * sinThetaO ) / cosThetaO;
        const double sinGammaT = h / etap;
        const double cosGammaT = sqrt( 1 - sinGammaT * sinGammaT );
        const double wantL     = 2 * cosGammaT / cosThetaT;

        Scalar ap[4], L = 0;
        brdf->TestApAndPathLength( ri, 0.5, ap, L );

        Check( Near( L, wantL, 1e-12 ),
               "oblique path length == 2 cos(gamma_t)/cos(theta_t)", L, wantL );
        CheckTrue( wantL > 2.0,
                   "the oblique path really is longer than the axial one (check is not vacuous)" );

        // And the transmittance follows THAT length, not the axial 2.
        const double wantRatio = exp( -( 0.9 - 0.5 ) * wantL );
        Scalar ap2[4], L2 = 0;
        brdf->TestApAndPathLength( ri, 0.9, ap2, L2 );
        Check( Near( ap2[1] / ap[1], wantRatio, 1e-12 ),
               "oblique A_1 ratio == exp(-delta sigma_a * L)", ap2[1] / ap[1], wantRatio );

        std::cout << "  oblique L=" << std::setprecision(12) << L << " (want " << wantL << ")"
                  << "  A_1 ratio=" << ap2[1] / ap[1] << std::endl;
    }

    brdf->release();
}

// ============================================================
//  13.  ApplyLobeTilt WHITE-BOX (test-only hook)
//
//  Groups 10 and 11 only ever exercise `ApplyLobeTilt` through the p == 0
//  (R) branch, and only through mixture-level energy / estimator / peak
//  checks that are measure preserving under a tilt-SIGN flip and cannot
//  see a corruption isolated to a single branch.  In particular: a p == 1
//  or p == 2 sign flip is invisible to every furnace / pdf / estimator
//  check in this file (rotation is measure preserving), and a corrupted
//  residual-lobe (p == kPMax) IDENTITY branch -- e.g. one that forgot the
//  `else` and fell through to a rotated angle -- is invisible to anything
//  that does not isolate that lobe.
//
//  This calls the PRODUCTION `ApplyLobeTilt` through the `TestApplyLobeTilt`
//  hook, which routes through the REAL `Resolve()` -- the material's bound
//  alpha painter feeds `Resolve()`'s actual 2k-alpha recurrence, exactly as
//  render time does -- so this guards the FULL recurrence assembly (every
//  index) plus ApplyLobeTilt's branches, not just ApplyLobeTilt in
//  isolation.  Each branch's output angle is checked against an
//  INDEPENDENTLY evaluated std::sin/std::cos of the composite angle --
//  NOT the code's own recurrence -- so a sign-flipped or mis-indexed
//  2k-alpha table cannot cancel out against the expectation.
//
//  VERIFIED CONVENTION (round-2 analysis of ApplyLobeTilt): the code
//  applies sin/cos of (theta_o - 2*alpha) at p == 0, (theta_o + alpha) at
//  p == 1, (theta_o + 4*alpha) at p == 2, and the untilted identity at
//  p == kPMax -- each followed by the unconditional pole-reflection
//  `cos = fabs(cos)`.  The expectations below are written from that
//  independently-verified convention, not read back out of the function
//  under test.
// ============================================================

static void RunApplyLobeTiltWhiteBox()
{
    std::cout << "=== 13. ApplyLobeTilt white-box (sign + recurrence index) ===" << std::endl;

    // Mirrors HairBSDF.cpp's file-local `kPMax` (R=0, TT=1, TRT=2,
    // residual=3).  Not includable from here (it is anonymous-namespace
    // scoped in the .cpp); kept in step by the same discipline as
    // `TestApAndPathLength`'s static_assert on its `ap[4]` extent.
    const int kPMaxTest = 3;

    // sigma_a / beta_m / beta_n / ior are irrelevant -- TestApplyLobeTilt's
    // only path through `Resolve()`-derived state is `sin2kAlpha` /
    // `cos2kAlpha`, which come solely from the alpha painter below.  Alpha
    // IS load-bearing now: it is bound to `alphaDeg` through a real
    // material, so `Resolve()` -- not this test -- produces the recurrence
    // under check.  `ri` is likewise a real hit; its h/theta/phi are
    // irrelevant to a uniform alpha painter, but Resolve() needs a valid
    // RayIntersectionGeometric to run.
    ScalarRef sigmaA( new UniformScalarPainter( 0.4 ) );
    ScalarRef betaM( new UniformScalarPainter( 0.3 ) );
    ScalarRef betaN( new UniformScalarPainter( 0.3 ) );
    const double alphaDeg = 6.0;
    ScalarRef alphaP( new UniformScalarPainter( alphaDeg ) );
    ScalarRef ior( new UniformScalarPainter( 1.55 ) );
    const HairPainters hp = MakeSigmaAPainters( *sigmaA, *betaM, *betaN, *alphaP, *ior );
    HairBRDF* brdf = new HairBRDF( hp ); brdf->addref();
    const RayIntersectionGeometric ri = MakeFibreHit( 0.3, 1.1, 0.0 );

    const double aRad     = alphaDeg * PI / 180.0;
    const double thetaOMags[3] = { 0.2, 0.5, 1.2 };

    for( int ti = 0; ti < 3; ti++ )
    {
        for( int sgn = 0; sgn < 2; sgn++ )
        {
            const double thetaO    = ( sgn == 0 ) ? thetaOMags[ti] : -thetaOMags[ti];
            const double sinThetaO = sin( thetaO );
            const double cosThetaO = cos( thetaO );

            // Composite angle per branch, from the VERIFIED convention
            // above -- p indexes { R, TT, TRT }; index 3 is the residual.
            const double composite[3] = {
                thetaO - 2.0 * aRad,
                thetaO + aRad,
                thetaO + 4.0 * aRad
            };

            for( int p = 0; p <= kPMaxTest; p++ )
            {
                Scalar sinOut = 0, cosOut = 0;
                brdf->TestApplyLobeTilt( ri, p, sinThetaO, cosThetaO, sinOut, cosOut );

                char lab[192];
                if( p == kPMaxTest ) {
                    // Residual-lobe identity branch: exact to machine
                    // precision (no trig re-derivation needed, and none
                    // of thetaOMags reach the pole so fabs() is a no-op
                    // here -- see the p == 2, thetaO == 1.2 case below for
                    // the one that actually exercises the reflection).
                    snprintf( lab, sizeof(lab),
                        "ApplyLobeTilt p=kPMax identity sin thetaO=%+.2f", thetaO );
                    Check( Near( sinOut, sinThetaO, 1e-15 ), lab, sinOut, sinThetaO );
                    snprintf( lab, sizeof(lab),
                        "ApplyLobeTilt p=kPMax identity cos thetaO=%+.2f", thetaO );
                    Check( Near( cosOut, cosThetaO, 1e-15 ), lab, cosOut, cosThetaO );
                    continue;
                }

                const double wantSin = sin( composite[p] );
                // The pole-reflection rule, applied to the INDEPENDENTLY
                // computed expectation -- exercised for real at
                // p == 2, thetaO == +/-1.2 (composite == 1.619, past
                // +/- pi/2).
                const double wantCos = fabs( cos( composite[p] ) );

                snprintf( lab, sizeof(lab),
                    "ApplyLobeTilt p=%d thetaO=%+.2f sin", p, thetaO );
                Check( Near( sinOut, wantSin, 1e-9 ), lab, sinOut, wantSin );
                snprintf( lab, sizeof(lab),
                    "ApplyLobeTilt p=%d thetaO=%+.2f cos", p, thetaO );
                Check( Near( cosOut, wantCos, 1e-9 ), lab, cosOut, wantCos );
            }
        }
    }

    // The pole-reflection case really is exercised: pin it explicitly so
    // a future change to thetaOMags cannot silently drop coverage of it.
    CheckTrue( 1.2 + 4.0 * aRad > PI_OV_TWO,
               "sanity: thetaO=1.2, p=2 composite angle exceeds pi/2 (reflection case is live)" );

    brdf->release();
}

// ============================================================
//  14.  kHEdge CLAMP -- the UV-less trap stays CHROMATIC
//
//  HairBSDF.h section 1 documents the trap: geometry with no UV channel
//  leaves `ptCoord == (0, 0)`, which resolves the near-field offset h to
//  the fibre EDGE (raw h == -1) at every hit.  At h == -1 EXACTLY,
//  cos(gamma_o) == 0, the Fresnel argument collapses to 0, F == 1, and
//  EVERY transmissive order (TT, TRT, residual) goes to exactly zero --
//  the model degenerates to a colourless white mirror, discarding the
//  melanin colour entirely.  `kHEdge` (0.9995) keeps `Resolve()` from
//  ever reaching that exact point, trading a physically irrelevant
//  sliver of h range for real, if heavily Fresnel-suppressed, colour.
//
//  This is checked, not merely asserted: a strongly-coloured melanin
//  material (eumelanin ~1.3, so R/G/B sigma_a differ substantially) is
//  evaluated at raw h == -1 (`ptCoord == (0.5, 0)`, exactly the documented
//  trap) over a coarse directional sweep, and the test requires that AT
//  LEAST ONE sampled direction stays clearly CHROMATIC (max/min channel
//  ratio > 1.2).  Channel ratio is the right invariant here, not absolute
//  magnitude: ap[p>=1] = (...) * T_channel with the achromatic Fresnel
//  factor common to every channel, so the RATIO between channels is
//  driven purely by sigma_a(lambda) and survives even when F is close to
//  (but, thanks to the clamp, provably not exactly) 1.
// ============================================================

static void RunKHEdgeClampChromatic()
{
    std::cout << "=== 14. kHEdge clamp keeps the UV-less trap chromatic ===" << std::endl;

    ScalarRef eumelanin( new UniformScalarPainter( 1.3 ) );
    ScalarRef betaM( new UniformScalarPainter( 0.3 ) );
    ScalarRef betaN( new UniformScalarPainter( 0.3 ) );
    ScalarRef alphaP( new UniformScalarPainter( 2.0 ) );
    ScalarRef ior( new UniformScalarPainter( 1.55 ) );

    HairPainters hp;
    hp.eumelanin = eumelanin.get();
    hp.beta_m = betaM.get(); hp.beta_n = betaN.get();
    hp.alpha  = alphaP.get(); hp.ior    = ior.get();

    HairBRDF* brdf = new HairBRDF( hp ); brdf->addref();

    // h = -1.0 raw -> MakeFibreHit's ptCoord = (0.5, 0.5*(h+1)) = (0.5, 0)
    // -- exactly the documented UV-less trap, with no need to poke
    // RayIntersectionGeometric fields directly.
    const RayIntersectionGeometric ri = MakeFibreHit( 0.3, 0.9, -1.0 );

    const int kThetaSteps = 48;
    const int kPhiSteps   = 48;
    double bestRatio = 0;
    double bestTheta = 0, bestPhi = 0;
    RISEPel bestC( 0, 0, 0 );

    for( int i = 0; i < kThetaSteps; i++ )
    {
        const double theta = -PI_OV_TWO + ( i + 0.5 ) * ( PI / kThetaSteps );
        for( int j = 0; j < kPhiSteps; j++ )
        {
            const double phi = ( j + 0.5 ) * ( TWO_PI / kPhiSteps );
            const Vector3 wi = FibreDir( ri, theta, phi );
            const RISEPel c = brdf->value( wi, ri );

            const double lo = r_min( r_min( c[0], c[1] ), c[2] );
            const double hi = r_max( r_max( c[0], c[1] ), c[2] );
            if( lo <= 1e-30 ) { continue; }        // avoid a divide near zero

            const double ratio = hi / lo;
            if( ratio > bestRatio ) {
                bestRatio = ratio;
                bestTheta = theta; bestPhi = phi;
                bestC = c;
            }
        }
    }

    CheckTrue( bestRatio > 1.2,
               "kHEdge clamp: UV-less trap (h raw = -1) still shows a chromatic direction (max/min > 1.2)" );

    std::cout << "  best ratio=" << std::setprecision(6) << bestRatio
              << "  at theta=" << bestTheta << " phi=" << bestPhi
              << "  value=(" << bestC[0] << ", " << bestC[1] << ", " << bestC[2] << ")"
              << std::endl;

    brdf->release();
}

// ============================================================
//  15.  MEDULLA-OFF BIT-IDENTITY  (Phase 3's non-negotiable gate)
//
//  Phase 3 (Yan 2017 medulla) added three parameters and two extra
//  lobes to this BCSDF.  The contract with every Phase-1 / Phase-2
//  scene is that `medulla_ratio == 0` -- the default -- takes the
//  EXACT pre-Phase-3 code path and reproduces the shipped numbers
//  BIT for BIT, not to a tolerance.
//
//  Two independent gates, because they fail on different bugs:
//
//   15a  GOLDEN.  A fixed grid of `value` / `valueNM` / `Pdf` /
//        `Scatter` / `ScatterNM` outputs compared against literals
//        CAPTURED FROM THE PHASE-2 BUILD AT COMMIT e819bbee (macOS /
//        AppleClang, optimised, `make -C build/make/rise` Deployment
//        flags).  This is the only gate that can see a change to the
//        SHARED (non-medulla) code -- a reordered accumulation, a
//        different `Resolve`, a lost sampler dimension.
//
//        ! THE COMPARISON IS TOOLCHAIN-CONDITIONAL, ON PURPOSE.  These
//        values flow through `exp` / `log` / `pow` / `atan2` / `sin` /
//        `cos`, whose results legitimately differ between Apple libm,
//        glibc and the MSVC CRT by more than one ULP -- before FMA
//        contraction is even considered -- and there is ONE golden
//        file, so a re-capture on any other platform would break this
//        one.  So:
//          * on the CAPTURE toolchain (Apple clang, optimised) the
//            comparison is `==`, exact, no tolerance.  That is where
//            the bit-identity promise HairBSDF.h section 3b makes was
//            established (0 differences over a 35,700-value grid) and
//            it is held to the letter;
//          * everywhere else it is a 1e-14 RELATIVE bound -- the
//            threshold this file's own investigation identified as
//            "floating-point reassociation, not a model change", and
//            the same order of magnitude tests/ThinFilmProductionTest
//            uses against its high-precision reference.  A real
//            regression in the shared code moves these numbers by
//            percent, not by 1e-14.
//        The exact-mismatch COUNT is printed either way, so a
//        tolerance-path run still reports how far the platform drifts.
//        Group 15b below is exact on EVERY platform and is what
//        actually pins "kappa == 0 short-circuits"; 15a's job is the
//        stronger, narrower claim that the shared code is untouched.
//
//        To RE-CAPTURE (only on the capture toolchain, and only for a
//        consciously-reviewed intentional change): set
//        HAIR_MEDULLA_GOLDEN_CAPTURE to 1, rebuild, run the binary,
//        paste the emitted block over tests/HairMedullaGolden.inl, set
//        the macro back to 0 -- and say so in the commit.
//
//   15b  IN-BINARY IDENTITY (portable -- no literals).  A material
//        with NO medulla painters bound at all (byte-for-byte the
//        Phase-2 painter set) must agree bit-for-bit with a material
//        whose `medulla_ratio` painter is bound to 0.0, and with one
//        whose `medulla_scatter` is bound to 0.0 at a LARGE
//        medulla_ratio.  This one cannot rot across compilers and is
//        the gate that actually pins "kappa == 0 short-circuits".
// ============================================================

//! Set to 1, rebuild, run, and paste the emitted block over
//! `tests/HairMedullaGolden.inl` to re-capture the group-15a literals.
//! Only meaningful on the capture toolchain -- see the group-15 header.
#define HAIR_MEDULLA_GOLDEN_CAPTURE 0

//! True on the toolchain the group-15a literals were captured with:
//! Apple clang, optimised.  A Debug (unoptimised) build changes
//! inlining and therefore FMA contraction, which is exactly the class
//! of difference the tolerance path exists to absorb, so it takes the
//! tolerance path too.
#if defined(__APPLE__) && defined(__clang__) && defined(__OPTIMIZE__)
static const bool kGoldenExactPlatform = true;
#else
static const bool kGoldenExactPlatform = false;
#endif

//! Relative bound used on every OTHER toolchain.  See the group-15
//! header for why this specific number.
static const double kGoldenRelTol = 1e-14;

namespace
{
    struct GoldenCase
    {
        double thetaO, phiO, h;
        double betaM, betaN, alphaDeg, ior;
        double sigmaA;      //!< tier 2 when >= 0
        double eumelanin;   //!< tier 1 when > 0 (and sigmaA < 0)
    };

    const GoldenCase kGoldenCases[6] = {
        {  0.00, 0.0,  0.0,     0.30, 0.30,  2.0, 1.55,  0.00, -1 },
        {  0.35, 1.1,  0.4,     0.30, 0.30,  2.0, 1.55,  0.40, -1 },
        { -1.20, 4.3, -0.4,     0.10, 0.70,  3.5, 1.60,  -1.0, 1.3 },
        {  1.50, 5.9,  0.95,    0.80, 0.15,  0.0, 1.45,  0.05, -1 },
        {  0.90, 2.7, -0.9995,  0.05, 1.00,  5.0, 1.70,  1.00, -1 },
        {  0.05, 0.0,  0.4,     1.00, 0.05, -3.0, 1.55,  0.20, -1 },
    };

    const double kGoldenWiTheta[3] = {  0.3, -0.5, 1.0 };
    const double kGoldenWiPhi[3]   = {  1.0,  3.0, 4.5 };

    //! 6 cases x ( 3 dirs x (3 value + valueNM + Pdf) + 2 Scatter x 7
    //! + 1 ScatterNM x 5 ) = 6 x 34.
    const int kGoldenPerCase = 34;
    const int kGoldenCount   = 6 * kGoldenPerCase;
}

//! Fills `out` (kGoldenPerCase entries) for one case.  Shared by the
//! capture and the compare paths so the two can never drift.
static void GoldenEvaluateCase( const GoldenCase& gc, double* out )
{
    ScalarRef sigmaP( new UniformScalarPainter( gc.sigmaA >= 0 ? gc.sigmaA : 0.0 ) );
    ScalarRef euP   ( new UniformScalarPainter( gc.eumelanin > 0 ? gc.eumelanin : 0.0 ) );
    ScalarRef betaMP( new UniformScalarPainter( gc.betaM ) );
    ScalarRef betaNP( new UniformScalarPainter( gc.betaN ) );
    ScalarRef alphaP( new UniformScalarPainter( gc.alphaDeg ) );
    ScalarRef iorP  ( new UniformScalarPainter( gc.ior ) );

    HairPainters hp;
    if( gc.sigmaA >= 0 ) { hp.sigma_a = sigmaP.get(); } else { hp.eumelanin = euP.get(); }
    hp.beta_m = betaMP.get(); hp.beta_n = betaNP.get();
    hp.alpha  = alphaP.get(); hp.ior    = iorP.get();

    HairBRDF* brdf = new HairBRDF( hp ); brdf->addref();
    HairSPF*  spf  = new HairSPF( hp );  spf->addref();

    const RayIntersectionGeometric ri = MakeFibreHit( gc.thetaO, gc.phiO, gc.h );
    const IORStack iorStack = MakeTestIORStack( g_stubObject );

    int n = 0;
    for( int d = 0; d < 3; d++ ) {
        const Vector3 wi = FibreDir( ri, kGoldenWiTheta[d], kGoldenWiPhi[d] );
        const RISEPel v = brdf->value( wi, ri );
        out[n++] = v[0]; out[n++] = v[1]; out[n++] = v[2];
        out[n++] = brdf->valueNM( wi, ri, 550.0 );
        out[n++] = spf->Pdf( ri, wi, iorStack );
    }

    {
        RandomNumberGenerator rng( kRNGSeed );
        IndependentSampler sampler( rng );
        for( int s = 0; s < 2; s++ ) {
            ScatteredRayContainer sc;
            spf->Scatter( ri, sampler, sc, iorStack );
            if( sc.Count() == 0 ) {
                for( int q = 0; q < 7; q++ ) { out[n++] = -1; }
            } else {
                const ScatteredRay& r = sc[0];
                out[n++] = r.ray.Dir().x; out[n++] = r.ray.Dir().y; out[n++] = r.ray.Dir().z;
                out[n++] = r.pdf;
                out[n++] = r.kray[0]; out[n++] = r.kray[1]; out[n++] = r.kray[2];
            }
        }
    }
    {
        RandomNumberGenerator rng( kRNGSeed + 1u );
        IndependentSampler sampler( rng );
        ScatteredRayContainer sc;
        spf->ScatterNM( ri, sampler, 550.0, sc, iorStack );
        if( sc.Count() == 0 ) {
            for( int q = 0; q < 5; q++ ) { out[n++] = -1; }
        } else {
            const ScatteredRay& r = sc[0];
            out[n++] = r.ray.Dir().x; out[n++] = r.ray.Dir().y; out[n++] = r.ray.Dir().z;
            out[n++] = r.pdf; out[n++] = r.krayNM;
        }
    }

    brdf->release();
    spf->release();
}

#if HAIR_MEDULLA_GOLDEN_CAPTURE
static const double kMedullaGolden[1] = { 0 };
#else
#include "HairMedullaGolden.inl"
#endif

static void RunMedullaOffBitIdentity()
{
    std::cout << "=== 15a. medulla-off GOLDEN bit-identity (vs commit e819bbee) ===" << std::endl;

    std::vector<double> got( (size_t)kGoldenCount, 0.0 );
    for( int c = 0; c < 6; c++ ) {
        GoldenEvaluateCase( kGoldenCases[c], &got[(size_t)( c * kGoldenPerCase )] );
    }

#if HAIR_MEDULLA_GOLDEN_CAPTURE
    printf( "static const double kMedullaGolden[kGoldenCount] = {\n" );
    for( int i = 0; i < kGoldenCount; i++ ) {
        printf( "    %.17g,\n", got[(size_t)i] );
    }
    printf( "};\n" );
    CheckTrue( false, "golden CAPTURE mode is on -- paste the block above and set "
                      "HAIR_MEDULLA_GOLDEN_CAPTURE back to 0" );
#else
    int mismatches = 0;
    int firstBad = -1;
    int overTol = 0;
    double worstRel = 0;
    for( int i = 0; i < kGoldenCount; i++ ) {
        if( got[(size_t)i] != kMedullaGolden[i] ) {
            mismatches++;
            if( firstBad < 0 ) { firstBad = i; }
            const double scale = fabs( kMedullaGolden[i] ) > 1e-300 ? fabs( kMedullaGolden[i] ) : 1.0;
            const double rel = fabs( got[(size_t)i] - kMedullaGolden[i] ) / scale;
            if( rel > worstRel ) { worstRel = rel; }
            if( rel > kGoldenRelTol ) { overTol++; }
        }
    }
    if( mismatches ) {
        std::cout << "  first mismatch at index " << firstBad
                  << " (case " << ( firstBad / kGoldenPerCase )
                  << ", slot " << ( firstBad % kGoldenPerCase ) << "): got "
                  << std::setprecision(17) << got[(size_t)firstBad]
                  << ", golden " << kMedullaGolden[firstBad]
                  << " ; worst relative difference over all "
                  << mismatches << " mismatches = " << std::setprecision(6) << worstRel << std::endl;
    }

    // The exact count is ALWAYS reported, so a tolerance-path platform
    // still tells you how far it drifts from the capture toolchain.
    std::cout << "  exact (bitwise) mismatches: " << mismatches << " of " << kGoldenCount
              << "; worst relative difference " << std::setprecision(4) << worstRel
              << ( kGoldenExactPlatform
                     ? "  [capture toolchain -- asserting EXACT equality]"
                     : "  [non-capture toolchain -- asserting a 1e-14 relative bound; "
                       "see the group-15 header]" )
              << std::endl;

    if( kGoldenExactPlatform ) {
        Check( mismatches == 0,
               "MONEY ASSERTION -- medulla-off output is BIT-IDENTICAL to the Phase-2 golden capture",
               mismatches, 0 );
    } else {
        Check( overTol == 0,
               "MONEY ASSERTION -- medulla-off output matches the Phase-2 golden capture to 1e-14 "
               "relative (this toolchain did not produce the literals; see the group-15 header)",
               overTol, 0 );
    }
#endif
}

//! 15b -- the portable half.  No literals: three painter sets that
//! must all resolve to the same (medulla-inactive) model.
static void RunMedullaOffInBinaryIdentity()
{
    std::cout << "=== 15b. medulla-off in-binary identity (no medulla painters == kappa 0 == sigma_m 0) ===" << std::endl;

    ScalarRef sigmaP( new UniformScalarPainter( 0.35 ) );
    ScalarRef betaMP( new UniformScalarPainter( 0.3 ) );
    ScalarRef betaNP( new UniformScalarPainter( 0.4 ) );
    ScalarRef alphaP( new UniformScalarPainter( 2.0 ) );
    ScalarRef iorP  ( new UniformScalarPainter( 1.55 ) );
    ScalarRef zeroP ( new UniformScalarPainter( 0.0 ) );
    ScalarRef bigKap( new UniformScalarPainter( 0.8 ) );
    ScalarRef gP    ( new UniformScalarPainter( 0.4 ) );
    ScalarRef scatP ( new UniformScalarPainter( 2.0 ) );

    HairPainters base;
    base.sigma_a = sigmaP.get();
    base.beta_m = betaMP.get(); base.beta_n = betaNP.get();
    base.alpha  = alphaP.get(); base.ior    = iorP.get();

    // (a) the Phase-2 painter set, verbatim -- no medulla slots bound.
    HairPainters unbound = base;

    // (b) medulla plumbed but kappa == 0.
    HairPainters kappaZero = base;
    kappaZero.medulla_ratio   = zeroP.get();
    kappaZero.medulla_scatter = scatP.get();
    kappaZero.medulla_g       = gP.get();

    // (c) a FAT medulla that scatters nothing.
    HairPainters scatterZero = base;
    scatterZero.medulla_ratio   = bigKap.get();
    scatterZero.medulla_scatter = zeroP.get();
    scatterZero.medulla_g       = gP.get();

    HairBRDF* brdfA = new HairBRDF( unbound );     brdfA->addref();
    HairBRDF* brdfB = new HairBRDF( kappaZero );   brdfB->addref();
    HairBRDF* brdfC = new HairBRDF( scatterZero ); brdfC->addref();
    HairSPF*  spfA  = new HairSPF( unbound );      spfA->addref();
    HairSPF*  spfB  = new HairSPF( kappaZero );    spfB->addref();
    HairSPF*  spfC  = new HairSPF( scatterZero );  spfC->addref();

    const IORStack iorStack = MakeTestIORStack( g_stubObject );

    const double thetas[5] = { 0.0, 0.6, -0.9, 1.4, -0.2 };
    const double hs[4]     = { 0.0, 0.55, -0.75, 0.99 };

    int diffB = 0, diffC = 0;
    for( int i = 0; i < 5; i++ )
    for( int k = 0; k < 4; k++ )
    {
        const RayIntersectionGeometric ri = MakeFibreHit( thetas[i], 1.3, hs[k] );

        for( int d = 0; d < 12; d++ )
        {
            const double t = -PI_OV_TWO + ( d + 0.5 ) * ( PI / 12 );
            const Vector3 wi = FibreDir( ri, t, 0.3 + d * 0.5 );

            const RISEPel a = brdfA->value( wi, ri );
            const RISEPel b = brdfB->value( wi, ri );
            const RISEPel c = brdfC->value( wi, ri );
            for( unsigned int ch = 0; ch < 3; ch++ ) {
                if( a[ch] != b[ch] ) { diffB++; }
                if( a[ch] != c[ch] ) { diffC++; }
            }
            if( brdfA->valueNM( wi, ri, 610.0 ) != brdfB->valueNM( wi, ri, 610.0 ) ) { diffB++; }
            if( brdfA->valueNM( wi, ri, 610.0 ) != brdfC->valueNM( wi, ri, 610.0 ) ) { diffC++; }
            if( spfA->Pdf( ri, wi, iorStack ) != spfB->Pdf( ri, wi, iorStack ) ) { diffB++; }
            if( spfA->Pdf( ri, wi, iorStack ) != spfC->Pdf( ri, wi, iorStack ) ) { diffC++; }
        }

        // The SAMPLER path too -- a medulla lobe that consumed an extra
        // sampler dimension at kappa == 0 would desynchronise the stream
        // and show up here and nowhere else.
        RandomNumberGenerator rngA( kRNGSeed ), rngB( kRNGSeed ), rngC( kRNGSeed );
        IndependentSampler sA( rngA ), sB( rngB ), sC( rngC );
        for( int s = 0; s < 64; s++ ) {
            ScatteredRayContainer ca, cb, cc;
            spfA->Scatter( ri, sA, ca, iorStack );
            spfB->Scatter( ri, sB, cb, iorStack );
            spfC->Scatter( ri, sC, cc, iorStack );
            if( ca.Count() != cb.Count() ) { diffB++; continue; }
            if( ca.Count() != cc.Count() ) { diffC++; continue; }
            if( ca.Count() == 0 ) { continue; }
            if( ca[0].ray.Dir().x != cb[0].ray.Dir().x ||
                ca[0].ray.Dir().y != cb[0].ray.Dir().y ||
                ca[0].ray.Dir().z != cb[0].ray.Dir().z ||
                ca[0].pdf != cb[0].pdf ||
                ca[0].kray[0] != cb[0].kray[0] ||
                ca[0].kray[1] != cb[0].kray[1] ||
                ca[0].kray[2] != cb[0].kray[2] ) { diffB++; }
            if( ca[0].ray.Dir().x != cc[0].ray.Dir().x ||
                ca[0].ray.Dir().y != cc[0].ray.Dir().y ||
                ca[0].ray.Dir().z != cc[0].ray.Dir().z ||
                ca[0].pdf != cc[0].pdf ||
                ca[0].kray[0] != cc[0].kray[0] ||
                ca[0].kray[1] != cc[0].kray[1] ||
                ca[0].kray[2] != cc[0].kray[2] ) { diffC++; }
        }
    }

    Check( diffB == 0, "MONEY ASSERTION -- medulla_ratio = 0 is bit-identical to no medulla painters at all",
           diffB, 0 );
    Check( diffC == 0, "MONEY ASSERTION -- medulla_scatter = 0 (at kappa = 0.8) is bit-identical to no medulla painters at all",
           diffC, 0 );

    brdfA->release(); brdfB->release(); brdfC->release();
    spfA->release();  spfB->release();  spfC->release();
}

// ============================================================
//  16-19.  THE YAN 2017 MEDULLA  (Phase 3, medulla ON)
//
//  Group 15 pinned that turning the medulla OFF changes nothing.
//  These four pin that turning it ON is physically defensible.
//
//   16  MEDULLA FURNACE.  The white-furnace gate of group 1, repeated
//       across a (kappa, sigma_m, g) grid including kappa = 0.9.  The
//       split is designed to be energy-preserving BY CONSTRUCTION
//       (HairBSDF.h section 3b), so this is a regression guard on that
//       design, not a hope: it fails the moment a scattered lobe's
//       M_p or N_p stops integrating to 1, which is exactly what a
//       mis-normalised profile table or a broken piecewise-linear
//       interpolation would do.  Energy must also never EXCEED 1.
//
//   17  SPLIT BOOKKEEPING (the white-box half).  Group 16 measures the
//       SUM, which the split leaves unchanged by construction -- it
//       would stay green with the two halves swapped, mis-weighted, or
//       attached to the wrong parent order.  This group reads the
//       per-lobe A_p vector through `TestMedullaAp` and asserts the
//       actual claims: A_TT + A_TTs reproduces the medulla-free A_TT
//       to the last bit, likewise TRT; A_TTs / A_TRTs rise
//       monotonically with sigma_m and vanish as sigma_m -> 0; the
//       unscattered TT correspondingly dims; and the residual lobe is
//       untouched.
//
//   18  SAMPLE <-> PDF AND THE ESTIMATOR, medulla on.  The scattered
//       lobes are sampled from a tabulated piecewise-linear density;
//       this checks the sampler and `Pdf` agree, that `PdfNM`
//       integrates to 1 over the sphere with the new lobes in the
//       mixture, and that E[value*cos/pdf] still matches an
//       independent quadrature at a NON-zero sigma_a.
//
//   19  THE TABLE ITSELF.  Normalisation at grid nodes and between
//       them, continuity across cell boundaries, and a direct
//       Sample-draws-from-Eval check.
// ============================================================

//! The medulla painter set: tier-2 colour plus the three medulla slots.
struct MedullaRig
{
    ScalarRef sigmaA, betaM, betaN, alpha, ior, kappa, scatter, g;
    HairPainters painters;

    MedullaRig( double sigmaAV, double kappaV, double scatterV, double gV,
                double betaMV = 0.3, double betaNV = 0.3 )
      : sigmaA ( new UniformScalarPainter( sigmaAV ) ),
        betaM  ( new UniformScalarPainter( betaMV ) ),
        betaN  ( new UniformScalarPainter( betaNV ) ),
        alpha  ( new UniformScalarPainter( 2.0 ) ),
        ior    ( new UniformScalarPainter( 1.55 ) ),
        kappa  ( new UniformScalarPainter( kappaV ) ),
        scatter( new UniformScalarPainter( scatterV ) ),
        g      ( new UniformScalarPainter( gV ) )
    {
        painters.sigma_a         = sigmaA.get();
        painters.beta_m          = betaM.get();
        painters.beta_n          = betaN.get();
        painters.alpha           = alpha.get();
        painters.ior             = ior.get();
        painters.medulla_ratio   = kappa.get();
        painters.medulla_scatter = scatter.get();
        painters.medulla_g       = g.get();
    }
};

static void RunMedullaFurnace()
{
    std::cout << "=== 16. Medulla white furnace (sigma_a = 0, kappa > 0) ===" << std::endl;

    const double kappas[3]  = { 0.3, 0.6, 0.9 };
    const double scatters[3]= { 0.2, 1.0, 5.0 };
    const double gs[3]      = { -0.6, 0.0, 0.6 };

    // Fewer geometry cells than group 1 -- the parameter grid is the
    // new axis here and the (theta_o, h) behaviour is already covered.
    const double thetas[2] = { 0.0, 0.7 };
    const double hs[2]     = { 0.0, 0.6 };

    double worstQuad = 0, worstSPF = 0;
    double maxEnergy = 0;

    for( int a = 0; a < 3; a++ )
    for( int b = 0; b < 3; b++ )
    for( int c = 0; c < 3; c++ )
    {
        MedullaRig rig( 0.0, kappas[a], scatters[b], gs[c] );

        HairBRDF* brdf = new HairBRDF( rig.painters ); brdf->addref();
        HairSPF*  spf  = new HairSPF( rig.painters );  spf->addref();
        const IORStack iorStack = MakeTestIORStack( g_stubObject );

        for( int i = 0; i < 2; i++ )
        for( int k = 0; k < 2; k++ )
        {
            const RayIntersectionGeometric ri = MakeFibreHit( thetas[i], 0.9, hs[k] );

            const double quad = QuadratureBSDFEnergy( *brdf, ri, 1, 450, 450 );
            if( fabs( quad - 1.0 ) > worstQuad ) { worstQuad = fabs( quad - 1.0 ); }
            if( quad > maxEnergy ) { maxEnergy = quad; }
            CheckTrue( Near( quad, 1.0, kFurnaceQuadTol ),
                       "medulla furnace: INTEGRAL value*cos over the sphere == 1" );
            CheckTrue( quad <= 1.0 + kFurnaceQuadTol,
                       "medulla furnace: energy never EXCEEDS 1" );

            const SPFStats st = RunSPFSamples( *spf, ri, iorStack, false, 0, 1, 20000 );
            if( fabs( st.meanKray - 1.0 ) > worstSPF ) { worstSPF = fabs( st.meanKray - 1.0 ); }
            CheckTrue( Near( st.meanKray, 1.0, 1e-9 ),
                       "medulla furnace: mean kray over SPF samples == 1" );
            CheckTrue( st.nullScatters == 0,
                       "medulla furnace: every Scatter call produced a ray" );
            CheckTrue( st.pdfMismatches == 0,
                       "medulla furnace: sampled pdf agrees with Pdf()" );
        }

        brdf->release();
        spf->release();
    }

    std::cout << "  worst |quadrature - 1| = " << std::setprecision(6) << worstQuad
              << " (tol " << kFurnaceQuadTol << "), worst |mean kray - 1| = " << worstSPF
              << ", max energy seen = " << maxEnergy << std::endl;
}

static void RunMedullaSplitBookkeeping()
{
    std::cout << "=== 17. Medulla split bookkeeping (per-lobe A_p) ===" << std::endl;

    const double kSigmaA = 0.35;
    const double kKappa  = 0.7;

    // The medulla-FREE reference: the same material with no medulla
    // painters bound at all.
    ScalarRef sigA ( new UniformScalarPainter( kSigmaA ) );
    ScalarRef bM   ( new UniformScalarPainter( 0.3 ) );
    ScalarRef bN   ( new UniformScalarPainter( 0.3 ) );
    ScalarRef al   ( new UniformScalarPainter( 2.0 ) );
    ScalarRef io   ( new UniformScalarPainter( 1.55 ) );
    HairPainters plain;
    plain.sigma_a = sigA.get(); plain.beta_m = bM.get(); plain.beta_n = bN.get();
    plain.alpha = al.get();     plain.ior = io.get();
    HairBRDF* plainBRDF = new HairBRDF( plain ); plainBRDF->addref();

    const double hs[3] = { 0.0, 0.35, 0.9 };
    const double sigmaMs[5] = { 0.0, 0.05, 0.3, 1.5, 8.0 };

    for( int k = 0; k < 3; k++ )
    {
        const RayIntersectionGeometric ri = MakeFibreHit( 0.25, 1.4, hs[k] );

        double refAp[4], refLen;
        plainBRDF->TestApAndPathLength( ri, kSigmaA, refAp, refLen );

        double prevTTs = -1, prevTRTs = -1, prevTT = 1e30;

        for( int s = 0; s < 5; s++ )
        {
            MedullaRig rig( kSigmaA, kKappa, sigmaMs[s], 0.4 );
            HairBRDF* brdf = new HairBRDF( rig.painters ); brdf->addref();

            double ap[6]; int nLobes = 0;
            double q1 = 0, q2 = 0, mb = 0, mtau = 0, mvar = 0;
            brdf->TestMedullaAp( ri, kSigmaA, ap, nLobes, q1, q2, mb, mtau, mvar );

            if( sigmaMs[s] <= 0 ) {
                // sigma_m == 0 must latch the medulla OFF entirely.
                Check( nLobes == 4, "split: sigma_m = 0 reports 4 lobes (medulla latched off)",
                       nLobes, 4 );
                for( int p = 0; p < 4; p++ ) {
                    CheckTrue( ap[p] == refAp[p],
                               "split: sigma_m = 0 reproduces the medulla-free A_p exactly" );
                }
                brdf->release();
                continue;
            }

            Check( nLobes == 6, "split: sigma_m > 0 reports 6 lobes", nLobes, 6 );

            // --- THE MONEY ASSERTION: energy bookkeeping ------------
            // Each parent order's two halves must reconstruct the
            // medulla-free attenuation.  Not a tolerance check on a
            // Monte-Carlo mean -- an algebraic identity, held to
            // rounding.
            CheckTrue( Near( ap[1] + ap[4], refAp[1], 1e-12 ),
                       "split: MONEY ASSERTION -- A_TT + A_TTs reproduces the medulla-free A_TT" );
            CheckTrue( Near( ap[2] + ap[5], refAp[2], 1e-12 ),
                       "split: MONEY ASSERTION -- A_TRT + A_TRTs reproduces the medulla-free A_TRT" );
            CheckTrue( ap[0] == refAp[0],
                       "split: the R lobe (no medulla crossing) is untouched" );
            CheckTrue( ap[3] == refAp[3],
                       "split: the residual lobe is deliberately NOT split" );

            double total = 0, refTotal = 0;
            for( int p = 0; p < 6; p++ ) { total += ap[p]; }
            for( int p = 0; p < 4; p++ ) { refTotal += refAp[p]; }
            CheckTrue( Near( total, refTotal, 1e-12 ),
                       "split: total apparent energy is unchanged by the medulla" );

            // --- direction of the redistribution --------------------
            CheckTrue( ap[4] > prevTTs,  "split: A_TTs grows with sigma_m" );
            CheckTrue( ap[5] > prevTRTs, "split: A_TRTs grows with sigma_m" );
            CheckTrue( ap[1] < prevTT,   "split: the unscattered A_TT dims as sigma_m grows" );
            prevTTs = ap[4]; prevTRTs = ap[5]; prevTT = ap[1];

            // --- THE SPLIT FACTOR ITSELF, against closed forms ------
            // The bookkeeping identity above holds for ANY x in
            // ap[1]*x + ap[1]*(1-x), so on its own it would stay green
            // with TRTs weighted 1-q instead of 1-q^2, with the
            // sqrt(1-b^2) chord factor dropped from q, with tau missing
            // its 1/cos(theta_t) inclination stretch, or with b missing
            // its /kappa.  Every one of those is a silent physical
            // error.  So the geometry is re-derived here from
            // (theta_o, h, eta, kappa, sigma_m) alone -- independently
            // of HairBSDF's own Geom/Medulla code -- and pinned.
            const double sinThetaO = sin( 0.25 );
            const double cosThetaO = cos( 0.25 );
            const double eta       = 1.55;
            const double sinThetaT = sinThetaO / eta;
            const double cosThetaT = sqrt( 1.0 - sinThetaT * sinThetaT );
            const double etap      = sqrt( eta * eta - sinThetaO * sinThetaO ) / cosThetaO;
            const double sinGammaT = hs[k] / etap;
            const double bExpect   = sinGammaT / kKappa;
            const double tauExpect = sigmaMs[s] * 2.0 * kKappa / cosThetaT;
            const double q1Expect  = exp( -tauExpect * sqrt( 1.0 - bExpect * bExpect ) );

            CheckTrue( Near( mb, bExpect, 1e-12 ),
                       "split: MONEY ASSERTION -- b == sin(gamma_t) / kappa (the /kappa is present)" );
            CheckTrue( Near( mtau, tauExpect, 1e-12 ),
                       "split: MONEY ASSERTION -- tau == sigma_m * 2 kappa / cos(theta_t) "
                       "(the inclination stretch is present)" );
            CheckTrue( Near( q1, q1Expect, 1e-12 ),
                       "split: MONEY ASSERTION -- q == exp(-tau * sqrt(1 - b^2)) (the chord factor "
                       "is present)" );
            CheckTrue( Near( q2, q1 * q1, 1e-15 ),
                       "split: MONEY ASSERTION -- TRT's survival is q SQUARED (two crossings), "
                       "not q" );
            CheckTrue( Near( ap[4], refAp[1] * ( 1 - q1 ), 1e-12 ),
                       "split: MONEY ASSERTION -- A_TTs == A_TT_free * (1 - q)" );
            CheckTrue( Near( ap[5], refAp[2] * ( 1 - q1 * q1 ), 1e-12 ),
                       "split: MONEY ASSERTION -- A_TRTs == A_TRT_free * (1 - q^2)" );
            CheckTrue( fabs( mb ) < 1.0,
                       "split: the chord's medulla impact parameter is inside the medulla" );

            // The variance the REAL BSDF path reports must be the one
            // the table holds for this hit's cell -- closing the loop
            // between group 19's table-level offset check and the
            // renderer's own read.
            {
                RISE::Implementation::HairMedullaProfile prof;
                RISE::Implementation::HairMedullaLookup( mb, mtau, 0.4, prof );
                CheckTrue( Near( mvar, prof.longVariance, 1e-12 ),
                           "split: the longitudinal variance the BSDF path reports IS the table's "
                           "value for this hit's (b, tau, g) cell" );
            }

            if( k == 0 && s == 3 ) {
                std::cout << "    h=0 sigma_m=" << sigmaMs[s]
                          << ": q1=" << std::setprecision(6) << q1 << " tau=" << mtau
                          << " b=" << mb << " longVar=" << mvar
                          << "  A=(" << ap[0] << "," << ap[1] << "," << ap[2] << ","
                          << ap[3] << " | " << ap[4] << "," << ap[5] << ")" << std::endl;
            }

            brdf->release();
        }

        // sigma_m -> 0 limit: the scattered lobes must vanish.
        MedullaRig tiny( kSigmaA, kKappa, 1e-7, 0.4 );
        HairBRDF* tinyBRDF = new HairBRDF( tiny.painters ); tinyBRDF->addref();
        double ap[6]; int n = 0; double q1, q2, mb, mtau, mvar;
        tinyBRDF->TestMedullaAp( ri, kSigmaA, ap, n, q1, q2, mb, mtau, mvar );
        CheckTrue( ap[4] < 1e-6 && ap[5] < 1e-6,
                   "split: the scattered lobes vanish as sigma_m -> 0" );
        CheckTrue( Near( ap[1], refAp[1], 1e-6 ),
                   "split: the unscattered TT converges to the medulla-free A_TT as sigma_m -> 0" );
        tinyBRDF->release();
    }

    plainBRDF->release();
}

static void RunMedullaEstimator()
{
    std::cout << "=== 18. Medulla sample<->pdf, pdf integral, estimator ===" << std::endl;

    struct Cell { double kappa, scatter, g, sigmaA, betaM, betaN, thetaO, h; };
    const Cell cells[5] = {
        { 0.70, 1.0, 0.40, 0.35, 0.30, 0.30,  0.20,  0.00 },
        { 0.90, 5.0, 0.00, 0.60, 0.30, 0.30, -0.60,  0.45 },
        { 0.30, 0.3,-0.60, 0.15, 0.10, 0.70,  1.10, -0.80 },
        { 0.85, 2.0, 0.80, 1.00, 0.80, 0.15,  0.00,  0.95 },
        { 0.50, 1.0, 0.40, 0.05, 0.05, 1.00,  0.90, -0.35 },
    };

    for( int c = 0; c < 5; c++ )
    {
        const Cell& C = cells[c];
        MedullaRig rig( C.sigmaA, C.kappa, C.scatter, C.g, C.betaM, C.betaN );

        HairBRDF* brdf = new HairBRDF( rig.painters ); brdf->addref();
        HairSPF*  spf  = new HairSPF( rig.painters );  spf->addref();
        const IORStack iorStack = MakeTestIORStack( g_stubObject );

        const RayIntersectionGeometric ri = MakeFibreHit( C.thetaO, 2.2, C.h );

        // (a) PdfNM integrates to 1 with the two extra lobes present.
        const double pdfInt = QuadraturePdfNM( *spf, ri, iorStack, 550.0 );
        CheckTrue( Near( pdfInt, 1.0, kFurnaceQuadTol ),
                   "medulla: PdfNM integrates to 1 over the sphere" );

        // (b) sampler <-> Pdf wiring.
        const SPFStats st = RunSPFSamples( *spf, ri, iorStack, false, 0, 1, 60000 );
        CheckTrue( st.pdfMismatches == 0, "medulla: sampled pdf agrees with Pdf()" );
        CheckTrue( st.nullScatters == 0,  "medulla: every Scatter call produced a ray" );

        // (c) estimator cross-check at NON-zero sigma_a: the SPF mean
        //     of kray must equal the independent quadrature of
        //     INTEGRAL value*cos.  This is the check that would catch
        //     a scattered lobe whose Sample and Eval disagree -- the
        //     furnace cannot, because at sigma_a = 0 kray is
        //     identically 1.
        const double quad = QuadratureBSDFEnergy( *brdf, ri, 1, 450, 450 );
        CheckTrue( Near( st.meanKray, quad, kEstimatorTol ),
                   "medulla: MONEY ASSERTION -- E[kray] over SPF samples == quadrature of "
                   "INTEGRAL value*cos" );
        CheckTrue( st.meanKray <= 1.0 + 1e-9,
                   "medulla: kray stays inside the section-3 [0,1] proxy bound" );

        std::cout << "    cell " << c << ": pdfInt=" << std::setprecision(6) << pdfInt
                  << "  E[kray]=" << st.meanKray << "  quad=" << quad
                  << "  (rel " << fabs( st.meanKray - quad ) / ( quad > 0 ? quad : 1 ) << ")"
                  << std::endl;

        brdf->release();
        spf->release();
    }
}

static void RunMedullaTable()
{
    std::cout << "=== 19. Medulla profile table (normalisation, continuity, sampling) ===" << std::endl;

    using RISE::Implementation::HairMedullaProfile;
    using RISE::Implementation::HairMedullaLookup;

    Check( (int)kHairMedullaNumFloats ==
               (int)( kHairMedullaNumB * kHairMedullaNumTau * kHairMedullaNumG *
                      ( kHairMedullaPhiBins + 1 ) ),
           "table: the baked float count matches the baked extents",
           (double)kHairMedullaNumFloats,
           (double)( kHairMedullaNumB * kHairMedullaNumTau * kHairMedullaNumG *
                     ( kHairMedullaPhiBins + 1 ) ) );
    CheckTrue( kHairMedullaPhiBins <= (unsigned int)HairMedullaProfile::kMaxBins,
               "table: the baked bin count fits the fixed-capacity runtime struct" );

    // --- (a) normalisation, at grid nodes AND between them -----------
    double worstNorm = 0;
    for( int ib = 0; ib < 9; ib++ )
    for( int it = 0; it < 9; it++ )
    for( int ig = 0; ig < 5; ig++ )
    {
        // Deliberately off-node fractions (x.5 steps) as well as nodes.
        const double b   = -1.0 + ib * 0.25;
        const double tau = kHairMedullaTauMin *
                           pow( kHairMedullaTauMax / kHairMedullaTauMin, it / 8.0 );
        const double g   = -0.9 + ig * 0.45;

        HairMedullaProfile prof;
        HairMedullaLookup( b, tau, g, prof );
        CheckTrue( prof.nBins == kHairMedullaPhiBins, "table: lookup returns a full profile" );

        // Fine trapezoid over the circle of the PUBLIC Eval.
        const int N = 2048;
        double sum = 0;
        double minV = 1e30;
        for( int i = 0; i < N; i++ ) {
            const double dphi = -PI + ( i + 0.5 ) * ( TWO_PI / N );
            const double v = prof.Eval( dphi );
            if( v < minV ) { minV = v; }
            sum += v;
        }
        sum *= TWO_PI / N;
        if( fabs( sum - 1.0 ) > worstNorm ) { worstNorm = fabs( sum - 1.0 ); }
        CheckTrue( Near( sum, 1.0, 1e-6 ), "table: the interpolated profile integrates to 1" );
        CheckTrue( minV > 0, "table: the profile is strictly positive everywhere "
                             "(a zero would be an unsampleable direction with nonzero energy)" );
    }
    std::cout << "    worst |INTEGRAL profile - 1| over the (b, tau, g) sweep = "
              << std::setprecision(4) << worstNorm << std::endl;

    // --- (b) continuity across a cell boundary -----------------------
    // Trilinear interpolation is C0, so stepping a hair across a node
    // must move the density by ~nothing.  A stencil that read the
    // wrong cell, or an off-by-one in the index clamp, shows up as a
    // jump here and essentially nowhere else.
    double worstJump = 0;
    for( int ib = 1; ib < (int)kHairMedullaNumB - 1; ib++ )
    {
        const double bNode = (double)ib / (double)( kHairMedullaNumB - 1 );
        const double eps = 1e-7;
        HairMedullaProfile lo, hi;
        HairMedullaLookup( bNode - eps, 1.0, 0.4, lo );
        HairMedullaLookup( bNode + eps, 1.0, 0.4, hi );
        for( int i = 0; i < 64; i++ ) {
            const double dphi = -PI + ( i + 0.5 ) * ( TWO_PI / 64 );
            const double d = fabs( lo.Eval( dphi ) - hi.Eval( dphi ) );
            if( d > worstJump ) { worstJump = d; }
        }
    }
    for( int it = 1; it < (int)kHairMedullaNumTau - 1; it++ )
    {
        const double tNode = kHairMedullaTauMin *
            pow( kHairMedullaTauMax / kHairMedullaTauMin,
                 (double)it / (double)( kHairMedullaNumTau - 1 ) );
        HairMedullaProfile lo, hi;
        HairMedullaLookup( 0.3, tNode * ( 1 - 1e-7 ), 0.4, lo );
        HairMedullaLookup( 0.3, tNode * ( 1 + 1e-7 ), 0.4, hi );
        for( int i = 0; i < 64; i++ ) {
            const double dphi = -PI + ( i + 0.5 ) * ( TWO_PI / 64 );
            const double d = fabs( lo.Eval( dphi ) - hi.Eval( dphi ) );
            if( d > worstJump ) { worstJump = d; }
        }
    }
    CheckTrue( worstJump < 1e-5,
               "table: MONEY ASSERTION -- the interpolation is continuous across cell boundaries" );
    std::cout << "    worst density jump across a (b or tau) cell boundary = " << worstJump << std::endl;

    // --- (c) mirror symmetry ----------------------------------------
    // ! WEAK BY CONSTRUCTION, and recorded as such.  `pos.Eval(dphi)`
    // vs `neg.Eval(-dphi)` is an algebraic identity of the single
    // `if( mirrored ) dphi = -dphi;` line, independent of what the
    // table holds -- it catches that line being deleted and little
    // else.  In particular it does NOT check that the runtime's sign
    // convention for b matches the BAKE's; that is pinned instead by
    // group 17's closed-form `b == sin(gamma_t) / kappa` assertion
    // plus the (d) draw test below, which is run in BOTH orientations.
    {
        HairMedullaProfile pos, neg;
        HairMedullaLookup(  0.55, 2.0, 0.4, pos );
        HairMedullaLookup( -0.55, 2.0, 0.4, neg );
        double worst = 0;
        for( int i = 0; i < 128; i++ ) {
            const double dphi = -PI + ( i + 0.5 ) * ( TWO_PI / 128 );
            worst = r_max( worst, fabs( pos.Eval( dphi ) - neg.Eval( -dphi ) ) );
        }
        CheckTrue( worst < 1e-12,
                   "table: a negative impact parameter gives the MIRRORED profile" );
    }

    // --- (c2) THE LONGITUDINAL VARIANCE ------------------------------
    // The 33rd float of every cell.  It is structurally invisible to
    // every other check in this file and to every render gate: `Mp` is
    // normalised at EVERY v, so reading the wrong float -- say
    // `cell[bins - 1]`, an azimuthal density, instead of `cell[bins]` --
    // leaves the furnace, the pdf integral and the estimator
    // cross-check all green and only changes how fur LOOKS.  So the
    // offset is pinned directly, against an index formula written out
    // here rather than borrowed from the runtime.
    {
        const unsigned int perCell = kHairMedullaPhiBins + 1u;

        // Land exactly on grid nodes so interpolation is the identity.
        const unsigned int ib = 3, it = 6, ig = 3;
        const double bNode   = (double)ib / (double)( kHairMedullaNumB - 1 );
        const double tauNode = kHairMedullaTauMin *
            pow( (double)kHairMedullaTauMax / (double)kHairMedullaTauMin,
                 (double)it / (double)( kHairMedullaNumTau - 1 ) );
        const double gNode   = -(double)kHairMedullaGMax +
            2.0 * (double)kHairMedullaGMax * (double)ig / (double)( kHairMedullaNumG - 1 );

        const size_t base = ( ( (size_t)ib * kHairMedullaNumTau + it ) * kHairMedullaNumG + ig ) * perCell;
        const double rawVariance = (double)kHairMedullaProfileData[base + kHairMedullaPhiBins];
        const double rawLastBin  = (double)kHairMedullaProfileData[base + kHairMedullaPhiBins - 1];

        HairMedullaProfile prof;
        HairMedullaLookup( bNode, tauNode, gNode, prof );

        CheckTrue( Near( prof.longVariance, rawVariance, 1e-6 ),
                   "table: MONEY ASSERTION -- longVariance reads cell[bins], the 33rd float, "
                   "not a neighbouring azimuthal density" );
        // The guard is only meaningful if the two floats actually
        // differ; assert that too, so a future table cannot make this
        // check vacuous without saying so.
        CheckTrue( fabs( rawVariance - rawLastBin ) > 0.01,
                   "table: the variance float and its neighbouring bin are far enough apart for "
                   "the offset check above to bite" );
        CheckTrue( prof.longVariance > 0, "table: longVariance is positive at an interior node" );

        // Physical bounds and direction.  The isotropic (thick-medulla)
        // limit of E[theta^2] for an exit direction uniform on the
        // sphere is INTEGRAL_0^1 asin(u)^2 du = 0.46740; the bake's
        // global maximum is 0.46977, so anything materially above that
        // is a unit or an indexing error, not physics.
        double prev = -1;
        bool risesWithTau = true;
        double maxVar = 0;
        for( int t = 0; t < 10; t++ ) {
            const double tau = kHairMedullaTauMin *
                pow( (double)kHairMedullaTauMax / (double)kHairMedullaTauMin, t / 9.0 );
            HairMedullaProfile pf;
            HairMedullaLookup( 0.0, tau, 0.0, pf );
            if( pf.longVariance > maxVar ) { maxVar = pf.longVariance; }
            // Isotropic medulla: one scatter already randomises theta,
            // so the variance starts near the isotropic value and drifts
            // only mildly.  The invariant worth asserting is the BOUND,
            // not monotonicity -- see the printout.
            if( t > 0 && pf.longVariance < prev - 0.2 ) { risesWithTau = false; }
            prev = pf.longVariance;
        }
        CheckTrue( maxVar < 0.55,
                   "table: longVariance never exceeds the isotropic-exit limit (~0.467) by more "
                   "than sampling noise" );
        CheckTrue( risesWithTau,
                   "table: longVariance does not collapse as tau grows" );
        std::cout << "    longVariance at node (b,tau,g)=(" << bNode << "," << tauNode << ","
                  << gNode << ") = " << std::setprecision(6) << prof.longVariance
                  << " (raw " << rawVariance << ", neighbouring bin " << rawLastBin
                  << "); max over the tau sweep = " << maxVar << std::endl;
    }

    // --- (d) Sample really does draw from Eval -----------------------
    // Histogram a large number of Sample() draws and compare each bin's
    // empirical density against Eval at the bin centre.  This is the
    // check that the piecewise-linear inversion (a quadratic solve per
    // segment, plus the wrap-around segment at +/- pi) is right; a
    // subtly wrong inverse leaves the pdf integrating to 1 and the
    // furnace green while quietly biasing every fur render.
    // Three cells: a thin forward-scattering one, its MIRRORED twin
    // (negative b -- the only direct exercise of the mirror path's
    // Sample half), and a thick diffusive one.
    {
        const double cellB[3]   = {  0.35, -0.35,  0.55 };
        const double cellTau[3] = {  1.70,  1.70, 12.00 };
        const double cellG[3]   = {  0.40,  0.40,  0.00 };
        const char*  cellTag[3] = { "thin", "thin MIRRORED", "thick" };

        for( int c = 0; c < 3; c++ )
        {
            HairMedullaProfile prof;
            HairMedullaLookup( cellB[c], cellTau[c], cellG[c], prof );

            const int kBins = 32;
            const int kDraws = 400000;
            std::vector<int> hist( (size_t)kBins, 0 );
            RandomNumberGenerator rng( kRNGSeed );
            for( int i = 0; i < kDraws; i++ ) {
                const double x = prof.Sample( rng.CanonicalRandom() );
                int bin = (int)floor( ( x + PI ) / TWO_PI * kBins );
                if( bin < 0 ) { bin = 0; }
                if( bin >= kBins ) { bin = kBins - 1; }
                hist[(size_t)bin]++;
            }

            // The gate is a BINOMIAL one, not a fixed percentage.  A
            // thick cell's forward bins carry ~0.4 % of the mass, so a
            // flat relative tolerance is simultaneously far too loose
            // for the fat bins and too tight for the thin ones -- the
            // first version of this test used 3 % and the thick cell
            // failed at 3.1 %, which was 1.3 sigma of pure sampling
            // noise.  Each bin's count is Binomial(N, mass), so the
            // right yardstick is sigma = sqrt(mass (1-mass) / N).  Five
            // sigma over 3 cells x 32 bins is a ~5e-5 false-alarm rate
            // (and the seed is fixed anyway), while a genuine inversion
            // bug displaces mass by tens of percent -- hundreds of
            // sigma.
            const double binW = TWO_PI / kBins;
            double worstSigma = 0;
            double worstRelAtWorst = 0;
            int worstBin = -1;
            for( int i = 0; i < kBins; i++ ) {
                // Reference: the analytic mass of the bin, from Eval,
                // integrated finely (the density is piecewise linear, so
                // this is exact up to the sub-sampling).
                double mass = 0;
                const int sub = 64;
                for( int j = 0; j < sub; j++ ) {
                    mass += prof.Eval( -PI + i * binW + ( j + 0.5 ) * ( binW / sub ) );
                }
                mass *= binW / sub;

                const double got = (double)hist[(size_t)i] / kDraws;
                const double sd = sqrt( r_max( mass * ( 1 - mass ), 1e-12 ) / (double)kDraws );
                const double dev = fabs( got - mass ) / sd;
                if( dev > worstSigma ) {
                    worstSigma = dev;
                    worstBin = i;
                    worstRelAtWorst = fabs( got - mass ) / ( mass > 1e-9 ? mass : 1.0 );
                }
            }
            CheckTrue( worstSigma < 5.0,
                       std::string( "table: MONEY ASSERTION -- Sample() draws from exactly the "
                                    "density Eval reports (" ) + cellTag[c] + " cell)" );
            std::cout << "    worst Sample-vs-Eval bin deviation, " << cellTag[c] << " cell = "
                      << std::setprecision(4) << worstSigma << " sigma (" << worstRelAtWorst
                      << " relative, bin " << worstBin << ", " << kDraws << " draws)" << std::endl;
        }
    }
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
    RunFurnaceCorners();
    std::cout << std::endl;
    RunEstimatorCrossCheck();
    std::cout << std::endl;
    RunEstimatorCorners();
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
    RunColorTierMisconfig();
    std::cout << std::endl;
    RunRoughnessAxisDiscriminator();
    std::cout << std::endl;
    RunCuticleTiltDirection();
    std::cout << std::endl;
    RunAbsorptionPathLength();
    std::cout << std::endl;
    RunApplyLobeTiltWhiteBox();
    std::cout << std::endl;
    RunKHEdgeClampChromatic();
    std::cout << std::endl;
    RunMedullaOffBitIdentity();
    std::cout << std::endl;
    RunMedullaOffInBinaryIdentity();
    std::cout << std::endl;
    RunMedullaFurnace();
    std::cout << std::endl;
    RunMedullaSplitBookkeeping();
    std::cout << std::endl;
    RunMedullaEstimator();
    std::cout << std::endl;
    RunMedullaTable();
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
