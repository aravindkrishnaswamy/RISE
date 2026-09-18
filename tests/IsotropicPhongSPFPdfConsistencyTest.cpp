//////////////////////////////////////////////////////////////////////
//
//  IsotropicPhongSPFPdfConsistencyTest.cpp - DL-98 red-proof.
//
//  Same claim, same two gates as tests/SchlickSPFPdfConsistencyTest.cpp
//  (DL-67 Slice 0): `IsotropicPhongSPF::Pdf`/`PdfNM` must be the actual
//  probability density of the direction `Scatter`/`ScatterNM` plus
//  `ScatteredRayContainer::RandomlySelect` hand the integrator, not a
//  raw-albedo-weighted average of the two lobe pdfs.
//
//  IsotropicPhongSPF is the same "draw every lobe, then pick one by its
//  REALIZED weight" sampler shape as SchlickSPF: the diffuse lobe's kray
//  (pRd->GetColor(ri)) is direction-independent, but the specular lobe's
//  kray, `Rs*(N+2)/(N+1)*max(cos_o,0)`, is a function of the specular
//  lobe's OWN drawn direction (cos_o = dot(direction, n)) -- simpler than
//  Schlick's Fresnel-from-half-vector, since cos_o is a direct dot product
//  with no half-vector reconstruction needed at all.  See
//  docs/DL98_DL99_PHONG_PDF_WEIGHTS.md for the full derivation, including
//  the per-channel sibling-direction reconstruction (a closed-form power
//  law in cos(down), since Perturb()'s azimuth warp is exponent-
//  independent -- no inversion needed).
//
//  GATE 1 -- NORMALISATION, TWO-SIDED.  A FULL-SPHERE quadrature of Pdf()
//  must match the measured probability that Scatter() emits ANY ray at
//  all, to within 1%.
//
//  The domain is the full sphere, not the hemisphere about `n`, and that
//  is load-bearing rather than defensive: IsotropicPhongSPF::Scatter has
//  NO dot(dir,n) >= 0 accept-check at all (only the geomN gate), and
//  GeometricUtilities::Perturb around `reflected` reaches down = pi/2, so
//  at low exponent and grazing incidence a real fraction of emitted
//  specular rays have dot(dir,n) < 0.  Their kray is r_max(cos_o,0) = 0,
//  so they never WIN a two-ray RandomlySelect -- but when the diffuse ray
//  has been dropped by the geomN gate they are the container's only
//  occupant and RandomlySelect's freeidx==1 short-circuit returns them
//  regardless of weight.  Pdf() prices exactly that event (the (1-aD)
//  branch of PhongSpecularDensity's q), so a hemisphere-only quadrature
//  systematically under-reads its own integrand.  Measured on the
//  `N1 rd.05 rs.95 tilt30 th45` row: 0.870% of emitted directions are
//  below the shading horizon, the hemisphere integral read 0.99180
//  against a measured emission probability of 0.99884, and the full
//  sphere reads 1.00047.  The same domain error also inflated gate 2
//  there (TVD 0.01196 of a 0.012 threshold -- 99.7% of the band, on an
//  artifact; 0.00763 once the domain is right, and the worst row over
//  the whole file drops from 0.01196 to 0.00855, i.e. 71% of the band).
//  The `belowZ` column reports that fraction per row.
//
//  GATE 2 -- TOTAL VARIATION vs THE REAL SAMPLER.  600 000 real
//  Scatter()+RandomlySelect() draws, histogrammed into 12x8 equal-solid-
//  angle bins, compared against the Pdf() quadrature's own bins.
//  Threshold 0.012, same Cauchy-Schwarz-derived floor as SchlickSPF's own
//  gate (see that file's header comment for the derivation; K=96,
//  N=600000 are identical here).
//
//  RED PROOF (pre-fix code, i.e. DL-98's raw-albedo weighting
//  `(dWeight*diffusePdf + sWeight*specPdf)/(dWeight+sWeight)` with
//  dWeight=MaxValue(Rd), sWeight=MaxValue(Rs), no geomN-tilt handling in
//  the weighting and an averaged exponent for the per-channel row):
//  measured against THIS test's own harness before the fix landed --
//  see the fix commit message for the captured numbers.
//
//  Build (from project root):
//    make -C build/make/rise build-test/IsotropicPhongSPFPdfConsistencyTest
//
//  Author: Aravind Krishnaswamy (worker session, DL-98)
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
#include "../src/Library/Materials/IsotropicPhongSPF.h"
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
//  Synthetic intersection -- identical construction to
//  SchlickSPFPdfConsistencyTest.cpp's MakeIntersection.  `tiltDeg` puts
//  the GEOMETRIC normal at that angle off the shading normal.
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
        ri.vGeomNormal = Vector3( sin(t), 0, cos(t) );
    }

    return ri;
}

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
    double N;                       // used when perChannel == false
    bool   perChannel;
    double Nr, Ng, Nb;              // per-channel exponent
    double tiltDeg;
    bool   runNM;
    int    qtOverride, qpOverride;  // 0 = use the default kQT/kQP grid
};

// Hemisphere-quadrature resolution.  400x800 uniform in (cos theta, phi).
// Phong's lobe is smoother than Schlick's Fresnel-boosted one (no half-
// vector spike at grazing), so this default is not expected to need the
// SchlickSPF file's finer low-roughness override; verified per-config
// below by comparing 400x800 against 800x1600 in a one-off local run
// (not re-checked in every CI invocation) -- see the fix commit message.
static const int kQT = 400;
static const int kQP = 800;

static const long kDraws = 600000;

static const double kMassTol = 0.01;    // gate 1
static const double kTvdTol  = 0.012;   // gate 2 (Cauchy-Schwarz floor, K=96 N=600000)

// One config, one spectral mode.  `nm < 0` means the RGB path.
static void RunConfig( const Config& c, double nm )
{
    UniformColorPainter* diff = new UniformColorPainter( RISEPel(c.rd,c.rd,c.rd) ); diff->addref();
    UniformColorPainter* spec = new UniformColorPainter( RISEPel(c.rs,c.rs,c.rs) ); spec->addref();

    IScalarPainter* exp = 0;
    if( c.perChannel ) {
        RGBScalarPainter* rp = new RGBScalarPainter( c.Nr, c.Ng, c.Nb ); rp->addref(); exp = rp;
    } else {
        UniformScalarPainter* rp = new UniformScalarPainter( c.N ); rp->addref(); exp = rp;
    }

    IsotropicPhongSPF* spf = new IsotropicPhongSPF( *diff, *spec, *exp ); spf->addref();

    RayIntersectionGeometric ri = MakeIntersection( c.thetaDeg * PI / 180.0, c.tiltDeg );
    IORStack iorStack = MakeTestIORStack( g_stubObject );

    const bool bNM = (nm > 0);

    RandomNumberGenerator rng( bNM ? 808080 : 313131 );
    Implementation::IndependentSampler sampler( rng );

    std::vector<double> emp( kNBins, 0.0 );
    long emitted = 0;
    long belowZ  = 0;

    for( long i = 0; i < kDraws; i++ ) {
        ScatteredRayContainer scattered;
        if( bNM ) {
            spf->ScatterNM( ri, sampler, nm, scattered, iorStack );
        } else {
            spf->Scatter( ri, sampler, scattered, iorStack );
        }

        ScatteredRay* sel = scattered.RandomlySelect( rng.CanonicalRandom(), bNM );
        if( sel ) {
            emitted++;
            const Vector3 d = Vector3Ops::Normalize( sel->ray.Dir() );
            if( d.z <= 0 ) {
                belowZ++;
            }
            emp[ BinOf( d ) ] += 1.0;
        }
    }

    const double massEmp = (double)emitted / (double)kDraws;
    for( int k = 0; k < kNBins; k++ ) {
        emp[k] /= (double)kDraws;
    }

    const int qt = c.qtOverride > 0 ? c.qtOverride : kQT;
    const int qp = c.qpOverride > 0 ? c.qpOverride : kQP;
    std::vector<double> quad( kNBins, 0.0 );
    double intPdf = 0;
    // FULL SPHERE: `a` runs over 2*qt rows of cos(theta) spanning [-1,1],
    // so the per-row solid-angle weight (and the resolution within the
    // upper hemisphere) is unchanged from the old hemisphere-only grid.
    // See the header comment for why the lower half is not optional.
    const double dw = (1.0/qt) * (TWO_PI/qp);
    for( int a = 0; a < 2*qt; a++ ) {
        const double ct = -1.0 + (a + 0.5)/qt;
        const double st = sqrt( r_max( 0.0, 1.0 - ct*ct ) );
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
              << "  belowZ=" << (emitted ? (double)belowZ/(double)emitted : 0.0)
              << std::endl;

    CHECK( fabs(intPdf - massEmp) <= kMassTol,
        std::string(c.name) + (bNM?" (NM)":" (RGB)") + ": integral of Pdf must match the measured probability that Scatter emits a ray, within tolerance" );
    CHECK( tvd <= kTvdTol,
        std::string(c.name) + (bNM?" (NM)":" (RGB)") + ": total variation between Pdf and the real Scatter+RandomlySelect histogram must sit at the MC noise floor" );

    spf->release(); exp->release(); diff->release(); spec->release();
}

int main()
{
    std::cout << "===== IsotropicPhongSPF Pdf Consistency Test (DL-98) =====" << std::endl;

    g_stubObject = new StubObject();
    g_stubObject->addref();
    GlobalLog();

    const Config cfgs[] = {
        // name                     th    rd    rs    N     perCh  Nr   Ng   Nb    tilt  NM
        { "th=10 rd.5 rs.3 N5",    10.0, 0.5,  0.3,  5.0,  false, 0,   0,   0,    0.0,  true , 0, 0 },
        { "th=30 rd.5 rs.3 N5",    30.0, 0.5,  0.3,  5.0,  false, 0,   0,   0,    0.0,  true , 0, 0 },
        { "th=45 rd.2 rs.6 N20",   45.0, 0.2,  0.6,  20.0, false, 0,   0,   0,    0.0,  false, 0, 0 },
        { "th=60 rd.5 rs.3 N5",    60.0, 0.5,  0.3,  5.0,  false, 0,   0,   0,    0.0,  false, 0, 0 },
        { "th=75 rd.7 rs.1 N50",   75.0, 0.7,  0.1,  50.0, false, 0,   0,   0,    0.0,  true , 0, 0 },
        { "th=45 rd.9 rs.05 N100", 45.0, 0.9,  0.05, 100.0,false, 0,   0,   0,    0.0,  false, 0, 0 },
        { "th=45 rd.05 rs.9 N5",   45.0, 0.05, 0.9,  5.0,  false, 0,   0,   0,    0.0,  false, 0, 0 },
        { "th=80 rd.5 rs.02 N2",   80.0, 0.5,  0.02, 2.0,  false, 0,   0,   0,    0.0,  false, 0, 0 },
        { "per-channel exponent",  45.0, 0.5,  0.3,  0.0,  true,  5.0, 20.0,80.0, 0.0,  false, 0, 0 },
        { "tilt 20 deg th=45",     45.0, 0.5,  0.3,  5.0,  false, 0,   0,   0,   20.0,  true , 0, 0 },
        { "tilt 40 deg th=45",     45.0, 0.5,  0.3,  5.0,  false, 0,   0,   0,   40.0,  false, 0, 0 },
        { "tilt 55 deg th=30",     30.0, 0.5,  0.3,  5.0,  false, 0,   0,   0,   55.0,  true , 0, 0 },
        // Adversarial rows: N=1 (the broadest possible cosine-power lobe,
        // maximising how much of the specular cone spills below the
        // shading-normal horizon at grazing incidence) with rs dominant
        // over rd, so the missing (N+2)/(N+1)=1.5x boost and the missing
        // geomN-tilt accounting both bite hardest.  A standalone quadrature
        // of the UNFIXED formula (see the fix commit message) reads mass
        // 0.76-0.93 at these four configs -- well outside gate 1 -- which
        // is what makes them a real red-proof rather than a restatement of
        // the formula.
        { "N1 rd.05 rs.95 tilt20 th45", 45.0, 0.05, 0.95, 1.0, false, 0, 0, 0, 20.0, true , 0, 0 },
        { "N1 rd.05 rs.95 tilt30 th45", 45.0, 0.05, 0.95, 1.0, false, 0, 0, 0, 30.0, false, 0, 0 },
        { "N1 rd.05 rs.95 tilt20 th60", 60.0, 0.05, 0.95, 1.0, false, 0, 0, 0, 20.0, false, 0, 0 },
        { "N1 rd.05 rs.95 tilt25 th50", 50.0, 0.05, 0.95, 1.0, false, 0, 0, 0, 25.0, true , 0, 0 },
    };

    std::cout << "\n-- Gate 1 (two-sided normalisation) + Gate 2 (TVD vs the real sampler) --" << std::endl;
    for( const Config& c : cfgs ) {
        RunConfig( c, -1.0 );
    }

    std::cout << "\n-- spectral twin (ScatterNM/PdfNM at 550nm) --" << std::endl;
    for( const Config& c : cfgs ) {
        if( c.runNM ) {
            RunConfig( c, 550.0 );
        }
    }

    std::cout << "\nChecks: " << g_checks << " Failures: " << g_failures << std::endl;
    if( g_failures == 0 ) {
        std::cout << "All IsotropicPhongSPF Pdf consistency checks passed!" << std::endl;
        return 0;
    }
    return 1;
}
