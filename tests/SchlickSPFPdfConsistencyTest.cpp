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
//  and the gate is set at ~2.4x that floor.  Measured post-fix values
//  are 0.0051-0.0063 -- i.e. the residual IS the bin noise, with no
//  detectable systematic component left.
//
//  RED PROOF (both gates, against this file's own predecessor at
//  4325f365 -- the "cD = MaxValue(rd)/(MaxValue(rd)+SchlickFresnelAvg(rs))
//  and exact qS(wo)" formula):
//
//    config                   int Pdf   target    TVD
//    th=10 rd.5 rs.3 r.3 i.8   0.8876    1.0000   0.0621
//    th=30 rd.5 rs.3 r.3 i.8   0.8802    1.0000   0.0646
//    th=45 rd.2 rs.6 r.15 i1   0.8500    1.0000   0.0750
//    th=60 rd.5 rs.3 r.3 i.8   0.8605    1.0000   0.0875
//    th=75 rd.7 rs.1 r.5 i.6   0.9534    1.0000   0.0681
//    th=45 rd.9 rs.05 r.4 i1   0.9445    1.0000   0.0278
//    th=45 rd.05 rs.9 r.4 i1   0.6767    1.0000   0.1616
//    th=80 rd.5 rs.02 r.2 i1   1.0222    1.0000   0.0125
//    per-channel roughness     0.8705    1.0000   0.0670
//    tilt 20 deg               0.8485    0.9903   0.0738
//    tilt 40 deg               0.7906    0.9605   0.0852
//    tilt 55 deg               0.7269    0.9248   0.0994
//
//  Post-fix every one of those reads |int Pdf - target| <= 0.007 and
//  TVD <= 0.007.
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
};

// Quadrature resolution for the hemisphere integral of Pdf().  400x800
// midpoints, uniform in (cos theta, phi) so each cell has equal solid
// angle.  Verified against 800x1600 on every config below: the integral
// agrees to <1e-5, so the numbers this test gates are Pdf()'s, not the
// quadrature's.
static const int kQT = 400;
static const int kQP = 800;

static const long kDraws = 600000;

static const double kMassTol = 0.01;    // gate 1
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
    std::vector<double> quad( kNBins, 0.0 );
    double intPdf = 0;
    const double dw = (1.0/kQT) * (TWO_PI/kQP);
    for( int a = 0; a < kQT; a++ ) {
        const double ct = (a + 0.5)/kQT;
        const double st = sqrt( 1.0 - ct*ct );
        for( int b = 0; b < kQP; b++ ) {
            const double ph = (b + 0.5)/kQP * TWO_PI;
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

    CHECK( fabs(intPdf - massEmp) <= kMassTol,
        std::string(c.name) + (bNM?" (NM)":" (RGB)") + ": integral of Pdf must match the measured probability that Scatter emits a ray, within 1%" );
    CHECK( tvd <= kTvdTol,
        std::string(c.name) + (bNM?" (NM)":" (RGB)") + ": total variation between Pdf and the real Scatter+RandomlySelect histogram must sit at the MC noise floor" );

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
        { "th=10 rd.5 rs.3 r.3 i.8", 10.0, 0.5,  0.3,  0.3,  0.8, false, 0,   0,   0,   0.0,  true  },
        { "th=30 rd.5 rs.3 r.3 i.8", 30.0, 0.5,  0.3,  0.3,  0.8, false, 0,   0,   0,   0.0,  true  },
        { "th=45 rd.2 rs.6 r.15 i1", 45.0, 0.2,  0.6,  0.15, 1.0, false, 0,   0,   0,   0.0,  false },
        { "th=60 rd.5 rs.3 r.3 i.8", 60.0, 0.5,  0.3,  0.3,  0.8, false, 0,   0,   0,   0.0,  false },
        { "th=75 rd.7 rs.1 r.5 i.6", 75.0, 0.7,  0.1,  0.5,  0.6, false, 0,   0,   0,   0.0,  true  },
        { "th=45 rd.9 rs.05 r.4 i1", 45.0, 0.9,  0.05, 0.4,  1.0, false, 0,   0,   0,   0.0,  false },
        { "th=45 rd.05 rs.9 r.4 i1", 45.0, 0.05, 0.9,  0.4,  1.0, false, 0,   0,   0,   0.0,  false },
        { "th=80 rd.5 rs.02 r.2 i1", 80.0, 0.5,  0.02, 0.2,  1.0, false, 0,   0,   0,   0.0,  false },
        { "per-channel roughness",   45.0, 0.5,  0.3,  0.3,  0.8, true,  0.2, 0.3, 0.4, 0.0,  false },
        { "tilt 20 deg th=45",       45.0, 0.5,  0.3,  0.3,  0.8, false, 0,   0,   0,   20.0, true  },
        { "tilt 40 deg th=45",       45.0, 0.5,  0.3,  0.3,  0.8, false, 0,   0,   0,   40.0, false },
        { "tilt 55 deg th=30",       30.0, 0.5,  0.3,  0.3,  0.8, false, 0,   0,   0,   55.0, true  },
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
