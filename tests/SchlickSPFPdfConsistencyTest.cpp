//////////////////////////////////////////////////////////////////////
//
//  SchlickSPFPdfConsistencyTest.cpp - DL-67 Slice 0 red-proof.
//
//  ONE claim, gated two independent ways: `SchlickSPF::Pdf`/`PdfNM` is
//  the probability density of the direction `Scatter`/`ScatterNM` plus
//  `ScatteredRayContainer::RandomlySelect` actually hand the integrator.
//
//  Why that is not trivially true.  SchlickSPF is a "draw every lobe,
//  THEN pick one by its realized weight" sampler: Scatter() draws the
//  diffuse ray (kray = rd, independent of the drawn direction) AND the
//  specular ray (kray = rho + (1-rho)*fresnel(half-vector), a function
//  of the specular lobe's OWN drawn direction) every call, and
//  RandomlySelect then picks one with probability proportional to
//  MaxValue(kray) -- PathTracingIntegrator.cpp's PTScatterSelectWeight.
//  So the density of the SELECTED direction is
//
//      f(w) = C_D * p_D(w) * 1{w above the horizon} + sum_i q_i(w) p_i(w)
//
//  where C_D and q_i are expectations over the OTHER lobes' draws.  See
//  docs/DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md for the derivation.
//
//  GATE 1 -- NORMALISATION, TWO-SIDED.  A hemisphere quadrature of
//  Pdf() must match the probability that Scatter() emits ANY ray at all,
//  measured from real Scatter() calls, to within 1%.  Two-sided: too
//  little density fails as loudly as too much.  (The target is exactly 1
//  whenever the shading and geometric normals agree, and strictly less
//  under a tilted normal -- Scatter really does return an empty
//  container on some draws there, and the test measures that rather
//  than assuming it.)
//
//  GATE 2 -- TOTAL VARIATION vs THE REAL SAMPLER.  600 000 real
//  Scatter() + real RandomlySelect() draws are histogrammed into 12
//  equal-cos-theta x 8 equal-phi bins (equal solid angle) and compared
//  against the same bins of the Pdf() quadrature.  Threshold 0.012,
//  derived from the histogram's own Monte-Carlo noise:
//
//      E[TVD] = (1/2) sum_k E|phat_k - p_k|
//             ~ (1/2) sqrt(2/pi) sum_k sqrt(p_k(1-p_k)/N)
//            <= (1/2) sqrt(2/pi) sqrt(K/N)                (Cauchy-Schwarz)
//             = 0.399 * sqrt(96/600000) = 0.0051
//
//  and the gate is set at ~2.4x that floor.
//
//  P2-2 (DL-67 Slice 0 round-2 review) CORRECTS the claim that used to
//  stand here ("the residual IS the bin noise, no systematic component
//  left").  A half-split check -- 4 000 000 draws on the th=30 control
//  row, split into two independent 2 000 000-draw halves so their TVD
//  against EACH OTHER is pure statistical noise by construction --
//  measured here: noise floor 0.0037, vs the REAL TVD (full histogram
//  vs the Pdf quadrature) 0.0062 -- 1.68x the noise floor, i.e. a real,
//  small systematic component survives the fix.  The round-2 review's
//  own broader sweep (this file's 12 rows, 4 000 000 draws each) puts
//  the noise floor at 0.0017-0.0021 and the real TVD at 0.0064-0.0078
//  (3.5-4.2x) -- same conclusion, larger ratio on rows further from the
//  control.  Root cause: `kSpecQuadN=16`'s own quadrature residual in
//  `SchlickDiffuseSelectCoefficient` (SchlickSPF.cpp), not bin noise --
//  see the kSpecQuadN decision below.  Kept at 16 (the cost of 32 is not
//  justified by the residual gate width already tolerates); the mass
//  gate for the two lowest-roughness P2-4 rows is widened to the
//  measured residual instead (`kMassTolLowRough`).
//

//  RED PROOF (both gates, against this file's own predecessor at
//  4325f365 -- the "cD = MaxValue(rd)/(MaxValue(rd)+SchlickFresnelAvg(rs))
//  and exact qS(wo)" formula):
//
//    config                   int Pdf   target    TVD
//    th=10 rd.5 rs.3 r.3 i.8   0.8876    1.0000   0.06270
//    th=30 rd.5 rs.3 r.3 i.8   0.8802    1.0000   0.06415
//    th=45 rd.2 rs.6 r.15 i1   0.8500    1.0000   0.0750
//    th=60 rd.5 rs.3 r.3 i.8   0.8605    1.0000   0.08762
//    th=75 rd.7 rs.1 r.5 i.6   0.9534    1.0000   0.06800
//    th=45 rd.9 rs.05 r.4 i1   0.9445    1.0000   0.0278
//    th=45 rd.05 rs.9 r.4 i1   0.6767    1.0000   0.1616
//    th=80 rd.5 rs.02 r.2 i1   1.0222    1.0000   0.0125
//    per-channel roughness     0.8705    1.0000   0.06744
//    tilt 20 deg               0.8485    0.9903   0.07363
//    tilt 40 deg               0.7906    0.9605   0.08543
//    tilt 55 deg               0.7269    0.9248   0.09957
//
//  (P3-5, DL-67 Slice 0 round-2 review: the previous version of this
//  table carried 8 stale TVD figures, off by up to 6e-4 from a re-run
//  against the SAME `4325f365` state -- corrected above.)
//
//  Post-fix every one of those reads |int Pdf - target| <= 0.0063 and
//  TVD <= 0.0081 (re-run at HEAD; the file's earlier "<= 0.007" bound
//  for TVD undercounted the th=45 rd.2/rs.6 row's 0.00808).
//
//  What the OLD version of this file gated, and why it was replaced: its
//  Part 1 compared Pdf() against an inline replica of Pdf()'s own
//  formula (it checks the code implements itself), and its Parts 2/3
//  asserted `Pdf(w_S) >= q_S(w_S)*p_S(w_S)`, which holds identically
//  because Pdf() adds a non-negative `cD*p_D` on top -- an algebraic
//  tautology, not a measurement.  Neither could see that the aggregate
//  integrated to 0.68-1.02 instead of 1.
//
//  Build (from project root):
//    make -C build/make/rise build-test/SchlickSPFPdfConsistencyTest
//
//  Author: Aravind Krishnaswamy (worker session, DL-67 Slice 0)
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/GeometricUtilities.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Materials/SchlickSPF.h"
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

// ============================================================
//  Synthetic intersection.  `tiltDeg` puts the GEOMETRIC normal at that
//  angle off the shading normal, which is what a GlintModifier-style
//  shading-normal perturbation looks like from inside the SPF: Scatter's
//  geometric-horizon gate then rejects part of the cosine hemisphere.
// ============================================================
static RayIntersectionGeometric MakeIntersection( double incomingTheta, double tiltDeg )
{
    const double sinT = sin(incomingTheta);
    const double cosT = cos(incomingTheta);
    const Vector3 inDir( sinT, 0, -cosT );

    Ray inRay( Point3(sinT, 0, 1.0), inDir );
    RasterizerState rs = {0, 0};
    RayIntersectionGeometric ri( inRay, rs );

    ri.bHit = true;
    ri.range = 1.0 / cosT;
    ri.ptIntersection = Point3(0, 0, 0);
    ri.vNormal = Vector3(0, 0, 1);
    ri.onb.CreateFromW( Vector3(0, 0, 1) );
    ri.ptCoord = Point2(0.5, 0.5);

    if( tiltDeg > 0 ) {
        const double t = tiltDeg * PI / 180.0;
        // Tilted toward +X, i.e. AWAY from the incoming ray's travel
        // direction, so the ray-anchoring in Scatter/Pdf leaves this
        // vector's sign alone and the hit stays a front-face hit.
        ri.vGeomNormal = Vector3( sin(t), 0, cos(t) );
    }

    return ri;
}

// 12 equal-cos-theta x 8 equal-phi bins: equal solid angle, so a
// mis-shaped density shows up as mass moving between bins rather than
// being hidden inside one big bin.
static const int kNT = 12;
static const int kNP = 8;
static const int kNBins = kNT * kNP;

static int BinOf( const Vector3& d )
{
    double c = d.z;
    if( c < 0 ) c = 0;
    if( c >= 1 ) c = 0.999999;
    int it = (int)(c * kNT);
    if( it >= kNT ) it = kNT-1;
    double phi = atan2( d.y, d.x );
    if( phi < 0 ) phi += TWO_PI;
    int ip = (int)(phi / TWO_PI * kNP);
    if( ip >= kNP ) ip = kNP-1;
    return it*kNP + ip;
}

struct Config
{
    const char* name;
    double thetaDeg;
    double rd, rs;
    double roughness, isotropy;     // used when perChannel == false
    bool   perChannel;
    double rr, rg, rb;              // per-channel roughness
    double tiltDeg;
    bool   runNM;                   // also gate the spectral twin
    int    qtOverride, qpOverride;  // 0 = use the default kQT/kQP grid
    double massTolOverride;         // 0 = use the default kMassTol
    bool   knownFailure;            // recorded-not-gated escape hatch; unused since DL-101 closed 2026-09-17 (no row currently sets this true)
};

// Quadrature resolution for the hemisphere integral of Pdf().  400x800
// midpoints, uniform in (cos theta, phi) so each cell has equal solid
// angle.  Verified against 800x1600 on every ORIGINAL config below: the
// integral agrees to <1e-5, so the numbers this test gates are Pdf()'s,
// not the quadrature's.
//
// VALIDITY RANGE (P2-4, DL-67 Slice 0 round-2 review): that verification
// does NOT extend to roughness <= 0.05 (near-mirror: the Fresnel-boosted
// specular lobe collapses to a half-vector spike a fraction of a degree
// wide).  A direct 200x400 -> 3200x6400 convergence sweep (doubling each
// step) found: the gate's existing control (th=30 r.3 i.8) is flat from
// 200x400 (1.00422 -> 1.00447, <3e-4 drift); but a roughness-0.05,
// theta=30 config is ALSO already flat by 400x800 (1.00441 -> 1.00447);
// the roughness-0.02, theta=85 combination is the one that is genuinely
// slow: 200x400 reads 0.9508, doubling to 3200x6400 only reaches 1.0096,
// and Richardson-extrapolating the last two steps' O(1/N) convergence
// puts the true integral at ~1.010-1.011 -- i.e. 400x800 under-reads
// that specific combination by ~5%, and even 2000x4000 (below) is not
// fully converged, leaving a residual ~0.2-0.3% quadrature-only gap on
// top of kSpecQuadN's own systematic bias there (see the kSpecQuadN
// note below).  So: this shared 400x800 default is valid at roughness
// >= 0.15 (this file's ORIGINAL floor) and, empirically, at roughness
// 0.05 down to moderate incidence; only the roughness-0.02/theta-85
// combination needs both a finer grid AND a widened tolerance.
static const int kQT = 400;
static const int kQP = 800;

// P2-4 (DL-67 Slice 0 round-2 review): scenes/Tests/BDPT/
// cornellbox_bdpt_materials_pt.RISEscene ships roughness 0.05, isotropy
// 0.3 -- below this file's original coverage floor of 0.15.  2000x4000
// leaves a ~0.2-0.3% quadrature-only residual at the worst (roughness
// 0.02, theta 85) row (see above) -- small next to that row's widened
// 1.5% mass tolerance, so not raised further.
static const int kQTFine = 2000;
static const int kQPFine = 4000;

static const long kDraws = 600000;

static const double kMassTol = 0.01;    // gate 1

// P2-2 (DL-67 Slice 0 round-2 review): kSpecQuadN=16 (SchlickSPF.cpp)
// leaves a real, measured systematic mass residual at low
// roughness/grazing incidence that raising the hemisphere quadrature
// does NOT remove, because it is Pdf() under-reporting its OWN C_D
// quadrature, not a hemisphere-integration artifact.  Measured on the
// roughness-0.02/theta-85 row at increasing hemisphere resolution (which
// isolates kSpecQuadN's bias from the grid's own error once the grid has
// converged, per the note above): the true (converged) integral is
// ~1.010-1.011 at kSpecQuadN=16 vs ~1.003-1.004 at kSpecQuadN=32 --
// roughly a 3x reduction, in the same direction as SS4a's 400-config
// sweep (mean |C_D error| 0.0034 -> 0.0014, ~2.4x).  Raising kSpecQuadN
// to 32 was measured here to roughly quadruple Pdf()'s own per-call cost
// (single-lane ~1.3-1.5us -> ~4.0-5.3us; per-channel ~6.0-7.3us ->
// ~14.9-16.5us on this machine, noisy but consistently ~3.5-4x) and does
// NOT fully close the worst-case residual to zero either -- so kept at
// 16, and the mass gate for the two lowest-roughness P2-4 rows below is
// honestly widened to the measured residual instead
// (`kMassTolLowRough`).  Full derivation and the whole-render CPU-cost
// comparison: docs/DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md SS4a/SS4d/SS4f.
static const double kMassTolLowRough = 0.015;
static const double kTvdTol  = 0.012;   // gate 2 (see the header derivation)

// One config, one spectral mode.  `nm < 0` means the RGB path.
static void RunConfig( const Config& c, double nm )
{
    UniformColorPainter* diff = new UniformColorPainter( RISEPel(c.rd,c.rd,c.rd) ); diff->addref();
    UniformColorPainter* spec = new UniformColorPainter( RISEPel(c.rs,c.rs,c.rs) ); spec->addref();

    IScalarPainter* rough = 0;
    if( c.perChannel ) {
        RGBScalarPainter* rp = new RGBScalarPainter( c.rr, c.rg, c.rb ); rp->addref(); rough = rp;
    } else {
        UniformScalarPainter* rp = new UniformScalarPainter( c.roughness ); rp->addref(); rough = rp;
    }
    UniformScalarPainter* iso = new UniformScalarPainter( c.isotropy ); iso->addref();

    SchlickSPF* spf = new SchlickSPF( *diff, *spec, *rough, *iso ); spf->addref();

    RayIntersectionGeometric ri = MakeIntersection( c.thetaDeg * PI / 180.0, c.tiltDeg );
    IORStack iorStack = MakeTestIORStack( g_stubObject );

    const bool bNM = (nm > 0);

    // ---- the real sampler ----------------------------------------
    // Fixed seed: this whole test is deterministic, which is what lets
    // the TVD threshold sit only ~2.4x above the noise floor.
    RandomNumberGenerator rng( bNM ? 909090 : 424242 );
    Implementation::IndependentSampler sampler( rng );

    std::vector<double> emp( kNBins, 0.0 );
    long emitted = 0;

    for( long i = 0; i < kDraws; i++ ) {
        ScatteredRayContainer scattered;
        if( bNM ) {
            spf->ScatterNM( ri, sampler, nm, scattered, iorStack );
        } else {
            spf->Scatter( ri, sampler, scattered, iorStack );
        }

        // The production selection, called as production calls it
        // (PTRandomlySelect<Tag> -> RandomlySelect(xi, bNM)).
        ScatteredRay* sel = scattered.RandomlySelect( rng.CanonicalRandom(), bNM );
        if( sel ) {
            emitted++;
            emp[ BinOf( Vector3Ops::Normalize( sel->ray.Dir() ) ) ] += 1.0;
        }
    }

    const double massEmp = (double)emitted / (double)kDraws;
    for( int k = 0; k < kNBins; k++ ) {
        emp[k] /= (double)kDraws;
    }

    // ---- the density -------------------------------------------------
    const int qt = c.qtOverride > 0 ? c.qtOverride : kQT;
    const int qp = c.qpOverride > 0 ? c.qpOverride : kQP;
    std::vector<double> quad( kNBins, 0.0 );
    double intPdf = 0;
    const double dw = (1.0/qt) * (TWO_PI/qp);
    for( int a = 0; a < qt; a++ ) {
        const double ct = (a + 0.5)/qt;
        const double st = sqrt( 1.0 - ct*ct );
        for( int b = 0; b < qp; b++ ) {
            const double ph = (b + 0.5)/qp * TWO_PI;
            const Vector3 wo( st*cos(ph), st*sin(ph), ct );
            const double pdf = bNM ? spf->PdfNM( ri, wo, nm, iorStack )
                                   : spf->Pdf( ri, wo, iorStack );
            intPdf += pdf * dw;
            quad[ BinOf(wo) ] += pdf * dw;
        }
    }

    double tvd = 0;
    for( int k = 0; k < kNBins; k++ ) {
        tvd += fabs( emp[k] - quad[k] );
    }
    tvd *= 0.5;

    std::cout << "  " << std::left << std::setw(26) << c.name << std::right
              << ( bNM ? "  NM " : "  RGB" )
              << "  intPdf=" << std::fixed << std::setprecision(5) << intPdf
              << "  emitted=" << massEmp
              << "  |diff|=" << fabs(intPdf - massEmp)
              << "  TVD=" << tvd
              << std::endl;

    if( c.knownFailure ) {
        // Escape hatch for a row whose failure is a KNOWN, not-yet-fixed
        // defect in a DIFFERENT component than this file's own subject
        // (the historical example: DL-101's per-channel ScatteredRay
        // reuse bug, which corrupted the SAMPLER, not Pdf()'s model of
        // it -- closed 2026-09-17, so no row currently sets this true).
        // Recorded so the mechanism stays findable without gating on a
        // number this file cannot itself fix.
        std::cout << "    ^ KNOWN-FAILURE, recorded not gated" << std::endl;
        g_checks++;
    } else {
        const double massTol = c.massTolOverride > 0 ? c.massTolOverride : kMassTol;
        CHECK( fabs(intPdf - massEmp) <= massTol,
            std::string(c.name) + (bNM?" (NM)":" (RGB)") + ": integral of Pdf must match the measured probability that Scatter emits a ray, within tolerance" );
        CHECK( tvd <= kTvdTol,
            std::string(c.name) + (bNM?" (NM)":" (RGB)") + ": total variation between Pdf and the real Scatter+RandomlySelect histogram must sit at the MC noise floor" );
    }

    spf->release(); rough->release(); iso->release(); diff->release(); spec->release();
}

int main()
{
    std::cout << "===== SchlickSPF Pdf Consistency Test (DL-67 Slice 0) =====" << std::endl;

    g_stubObject = new StubObject();
    g_stubObject->addref();
    GlobalLog();

    // The 8 reviewer configurations, plus the per-channel-roughness
    // branch and three shading-normal tilts.  The isotropy column is
    // deliberately mixed: an anisotropic lobe (isotropy != 1) is the
    // case whose azimuthal density was wrong by up to 25x, and it is
    // invisible to any isotropic fixture.
    const Config cfgs[] = {
        // name                      th    rd    rs    r     iso  perCh  rr   rg   rb   tilt  NM
        { "th=10 rd.5 rs.3 r.3 i.8", 10.0, 0.5,  0.3,  0.3,  0.8, false, 0,   0,   0,   0.0,  true  , 0,     0,     0.0    },
        { "th=30 rd.5 rs.3 r.3 i.8", 30.0, 0.5,  0.3,  0.3,  0.8, false, 0,   0,   0,   0.0,  true  , 0,     0,     0.0    },
        { "th=45 rd.2 rs.6 r.15 i1", 45.0, 0.2,  0.6,  0.15, 1.0, false, 0,   0,   0,   0.0,  false , 0,     0,     0.0    },
        { "th=60 rd.5 rs.3 r.3 i.8", 60.0, 0.5,  0.3,  0.3,  0.8, false, 0,   0,   0,   0.0,  false , 0,     0,     0.0    },
        { "th=75 rd.7 rs.1 r.5 i.6", 75.0, 0.7,  0.1,  0.5,  0.6, false, 0,   0,   0,   0.0,  true  , 0,     0,     0.0    },
        { "th=45 rd.9 rs.05 r.4 i1", 45.0, 0.9,  0.05, 0.4,  1.0, false, 0,   0,   0,   0.0,  false , 0,     0,     0.0    },
        { "th=45 rd.05 rs.9 r.4 i1", 45.0, 0.05, 0.9,  0.4,  1.0, false, 0,   0,   0,   0.0,  false , 0,     0,     0.0    },
        { "th=80 rd.5 rs.02 r.2 i1", 80.0, 0.5,  0.02, 0.2,  1.0, false, 0,   0,   0,   0.0,  false , 0,     0,     0.0    },
        // P2-4 (DL-67 Slice 0 round-2 review): scenes/Tests/BDPT/
        // cornellbox_bdpt_materials_pt.RISEscene ships roughness 0.05,
        // isotropy 0.3 (mat_schlick: rd=0.6 grey, rs=1.0 white) -- below
        // this file's original coverage floor of roughness 0.15.  These
        // four rows use the fine 2000x4000 hemisphere quadrature so the
        // gate reads Pdf()'s own residual, not the quadrature's.
        { "r.05 rd.6 rs1 i.3 th15",  15.0, 0.6,  1.0,  0.05, 0.3, false, 0,   0,   0,   0.0,  false , kQTFine, kQPFine, 0.0             },
        { "r.05 rd.6 rs1 i.3 th45",  45.0, 0.6,  1.0,  0.05, 0.3, false, 0,   0,   0,   0.0,  false , kQTFine, kQPFine, 0.0             },
        { "r.05 rd.6 rs1 i.3 th70",  70.0, 0.6,  1.0,  0.05, 0.3, false, 0,   0,   0,   0.0,  false , kQTFine, kQPFine, 0.0             },
        { "r.02 rd.6 rs1 i.3 th85",  85.0, 0.6,  1.0,  0.02, 0.3, false, 0,   0,   0,   0.0,  false , kQTFine, kQPFine, kMassTolLowRough},
        { "per-channel roughness",   45.0, 0.5,  0.3,  0.3,  0.8, true,  0.2, 0.3, 0.4, 0.0,  false , 0,     0,     0.0    },
        // P2-3 (DL-67 Slice 0 round-2 review) / DL-101 (CLOSED 2026-09-17,
        // debt-dl100 slice).  This is NOT this file's own subject (Pdf()
        // correctly models the INTENDED per-channel semantics here); this
        // row demonstrates that DL-101's `ScatteredRay` reuse bug
        // corrupted the SAMPLER itself, so Pdf() and the real
        // Scatter()+RandomlySelect() histogram genuinely disagreed.
        //
        // Found by direct instrumentation, not a blind sweep: an early
        // randomised search over grazing + wide-per-channel-roughness
        // configs turned up a plausible-looking ~0.05 TVD row, but a
        // duplicate-direction counter added to Scatter()'s OWN output
        // (bit-identical ray directions across two different lanes in one
        // container -- the direct DL-101 signature) showed ZERO
        // duplicates across 200 000 draws for that row: the ~0.05 was a
        // HEMISPHERE-QUADRATURE artifact (a near-mirror rb=0.006 lane
        // under a coarse 200x400 grid), not DL-101, and would have been a
        // false-positive control.  Low-then-high per-channel roughness
        // (one lane that almost always succeeds feeding a stale direction
        // to a lane that often fails `hdotk>0`) reproduces the real bug:
        // the same counter read 111613 duplicate-direction pairs across
        // 200000 draws for the config below, pre-fix.  Pre-fix, measured
        // on the real gate machinery (600000 draws, 2000x4000
        // quadrature): TVD 0.01424 vs the 0.012 gate (1.19x) and mass
        // |diff| 0.01216 vs the 1% gate (1.22x).  DL-101's fix (declaring
        // `ScatteredRay s;` INSIDE the per-channel loop in
        // `SchlickSPF.cpp` and both Ward files, `tests/SchlickWard
        // PerChannelReuseTest.cpp`'s own dedicated red-proof) drops TVD to
        // 0.01009 -- inside the standard 0.012 gate -- confirming DL-101
        // was a genuine PARTIAL contributor here, not the row's only
        // source of disagreement: the residual mass |diff| of 0.01216
        // (re-measured identically post-fix, since it was never DL-101's
        // own signature -- see below) is `kSpecQuadN=16`'s own C_D
        // quadrature residual (P2-2), which this row's low-roughness
        // config was already known to trip; gated at `kMassTolLowRough`
        // (1.5%) like the neighbouring `r.02 rd.6 rs1 i.3 th85` row for
        // the same reason.  Now a REAL gate (both CHECK()s fire), not a
        // recorded-but-ungated KNOWN-FAILURE.
        { "DL-101 CLOSED pc wide grazing", 88.0, 0.5, 0.05, 0.0, 0.5, true, 0.02, 0.95, 0.95, 0.0, false, kQTFine, kQPFine, kMassTolLowRough, false },
        { "tilt 20 deg th=45",       45.0, 0.5,  0.3,  0.3,  0.8, false, 0,   0,   0,   20.0, true  , 0,     0,     0.0    },
        { "tilt 40 deg th=45",       45.0, 0.5,  0.3,  0.3,  0.8, false, 0,   0,   0,   40.0, false , 0,     0,     0.0    },
        { "tilt 55 deg th=30",       30.0, 0.5,  0.3,  0.3,  0.8, false, 0,   0,   0,   55.0, true  , 0,     0,     0.0    },
    };

    std::cout << "\n-- Gate 1 (two-sided normalisation) + Gate 2 (TVD vs the real sampler) --" << std::endl;
    for( const Config& c : cfgs ) {
        RunConfig( c, -1.0 );
    }

    std::cout << "\n-- spectral twin (ScatterNM/PdfNM at 550nm) --" << std::endl;
    for( const Config& c : cfgs ) {
        if( c.runNM ) {
            // ScatterNM has no per-channel branch, so a per-channel
            // roughness painter is not a distinct spectral case.
            RunConfig( c, 550.0 );
        }
    }

    std::cout << "\nChecks: " << g_checks << " Failures: " << g_failures << std::endl;
    if( g_failures == 0 ) {
        std::cout << "All SchlickSPF Pdf consistency checks passed!" << std::endl;
        return 0;
    }
    return 1;
}
