//////////////////////////////////////////////////////////////////////
//
//  AshikminShirleySPFPdfConsistencyTest.cpp - DL-99 red-proof.
//
//  Same claim and two gates as tests/SchlickSPFPdfConsistencyTest.cpp
//  (DL-67 Slice 0) and tests/IsotropicPhongSPFPdfConsistencyTest.cpp
//  (DL-98): `AshikminShirleyAnisotropicPhongSPF::Pdf`/`PdfNM` must be the
//  actual probability density of the direction `Scatter`/`ScatterNM` plus
//  `ScatteredRayContainer::RandomlySelect` hand the integrator.
//
//  AshikminShirleyAnisotropicPhongSPF is the same "draw every lobe, then
//  pick one by its REALIZED weight" sampler shape, with two differences
//  from Schlick/Phong that make it its own case: (a) the specular lobe's
//  realized kray, `fresnel(Rs,hdotk) * max(cos_o,0) / max(cos_i,cos_o)`,
//  is a closed form independent of the sampled half-vector's azimuth or
//  polar angle beyond hdotk and cos_o (the Nu/Nv terms in the BRDF and
//  the inverse-pdf cancel exactly -- see
//  docs/DL98_DL99_PHONG_PDF_WEIGHTS.md section 5); (b) the DIFFUSE lobe's
//  realized kray is ALSO direction-dependent (Rd*(1-Rs)*(28/23)*
//  fromK1(cos_o)*fromK2(cos_i)), unlike Schlick/Phong's constant diffuse
//  kray, so the diffuse selection weight wD is evaluated exactly at the
//  query wo rather than held constant.
//
//  Pre-fix, `Pdf`/`PdfNM` used weights computed only at the MIRROR
//  direction (`AshikminShirleyAnisotropicPhongSPF.cpp`'s old
//  `AshikminShirleySpecularPdf`) with three independent factor errors on
//  top: a spurious `Rs` multiply and a missing `cos_o` on the specular
//  weight, and a missing `(1-Rs)` plus a spurious `1/pi` (and the wrong
//  `fromK1*fromK2` vs `fromK^2`) on the diffuse weight.
//
//  GATE 1 -- NORMALISATION, TWO-SIDED, 1%.
//  GATE 2 -- TOTAL VARIATION vs 600 000 real Scatter()+RandomlySelect()
//  draws, 12x8 equal-solid-angle bins, threshold 0.012 (same
//  Cauchy-Schwarz floor as the other two files, K=96 N=600000).
//
//  Build (from project root):
//    make -C build/make/rise build-test/AshikminShirleySPFPdfConsistencyTest
//
//  Author: Aravind Krishnaswamy (worker session, DL-99)
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
#include "../src/Library/Materials/AshikminShirleyAnisotropicPhongSPF.h"
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
//  Synthetic intersection.  `azimuthDeg` rotates the incoming ray's
//  transverse (X,Y) component so a genuinely anisotropic (Nu != Nv) lobe
//  is exercised at more than one angle relative to the object's (u,v)
//  tangent frame -- an isotropic-incidence fixture (azimuth 0, ray in the
//  X-Z plane) cannot distinguish "the anisotropy is handled" from
//  "the anisotropy is silently averaged away".
// ============================================================
static RayIntersectionGeometric MakeIntersection( double incomingTheta, double azimuthDeg, double tiltDeg )
{
    const double sinT = sin(incomingTheta);
    const double cosT = cos(incomingTheta);
    const double az = azimuthDeg * PI / 180.0;
    const Vector3 inDir( sinT*cos(az), sinT*sin(az), -cosT );

    Ray inRay( Point3(-inDir.x, -inDir.y, 1.0), inDir );
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
        // Tilt toward +X (arbitrary, fixed axis; azimuth varies the RAY
        // instead so the tilt axis and the ray azimuth are independent).
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
    double thetaDeg, azimuthDeg;
    double rd, rs;
    double Nu, Nv;                  // used when perChannel == false
    bool   perChannel;
    double Nur, Nug, Nub, Nvr, Nvg, Nvb;
    double tiltDeg;
    bool   runNM;
};

static const int kQT = 400;
static const int kQP = 800;

static const long kDraws = 600000;

static const double kMassTol = 0.01;
static const double kTvdTol  = 0.012;

static void RunConfig( const Config& c, double nm )
{
    UniformColorPainter* diff = new UniformColorPainter( RISEPel(c.rd,c.rd,c.rd) ); diff->addref();
    UniformColorPainter* spec = new UniformColorPainter( RISEPel(c.rs,c.rs,c.rs) ); spec->addref();

    IScalarPainter* nuP = 0;
    IScalarPainter* nvP = 0;
    if( c.perChannel ) {
        RGBScalarPainter* rpu = new RGBScalarPainter( c.Nur, c.Nug, c.Nub ); rpu->addref(); nuP = rpu;
        RGBScalarPainter* rpv = new RGBScalarPainter( c.Nvr, c.Nvg, c.Nvb ); rpv->addref(); nvP = rpv;
    } else {
        UniformScalarPainter* rpu = new UniformScalarPainter( c.Nu ); rpu->addref(); nuP = rpu;
        UniformScalarPainter* rpv = new UniformScalarPainter( c.Nv ); rpv->addref(); nvP = rpv;
    }

    AshikminShirleyAnisotropicPhongSPF* spf = new AshikminShirleyAnisotropicPhongSPF( *nuP, *nvP, *diff, *spec ); spf->addref();

    RayIntersectionGeometric ri = MakeIntersection( c.thetaDeg * PI / 180.0, c.azimuthDeg, c.tiltDeg );
    IORStack iorStack = MakeTestIORStack( g_stubObject );

    const bool bNM = (nm > 0);

    RandomNumberGenerator rng( bNM ? 707070 : 515151 );
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

    std::cout << "  " << std::left << std::setw(30) << c.name << std::right
              << ( bNM ? "  NM " : "  RGB" )
              << "  intPdf=" << std::fixed << std::setprecision(5) << intPdf
              << "  emitted=" << massEmp
              << "  |diff|=" << fabs(intPdf - massEmp)
              << "  TVD=" << tvd
              << std::endl;

    CHECK( fabs(intPdf - massEmp) <= kMassTol,
        std::string(c.name) + (bNM?" (NM)":" (RGB)") + ": integral of Pdf must match the measured probability that Scatter emits a ray, within tolerance" );
    CHECK( tvd <= kTvdTol,
        std::string(c.name) + (bNM?" (NM)":" (RGB)") + ": total variation between Pdf and the real Scatter+RandomlySelect histogram must sit at the MC noise floor" );

    spf->release(); nuP->release(); nvP->release(); diff->release(); spec->release();
}

int main()
{
    std::cout << "===== AshikminShirleyAnisotropicPhongSPF Pdf Consistency Test (DL-99) =====" << std::endl;

    g_stubObject = new StubObject();
    g_stubObject->addref();
    GlobalLog();

    const Config cfgs[] = {
        // name                          th    az    rd    rs    Nu    Nv    perCh  Nur  Nug  Nub  Nvr  Nvg  Nvb   tilt  NM
        { "th=10 az0 rd.5 rs.3 Nu20 Nv80",  10.0, 0.0,  0.5,  0.3,  20.0, 80.0, false, 0,0,0, 0,0,0,  0.0,  true  },
        { "th=30 az0 rd.5 rs.3 Nu20 Nv80",  30.0, 0.0,  0.5,  0.3,  20.0, 80.0, false, 0,0,0, 0,0,0,  0.0,  true  },
        { "th=45 az45 rd.2 rs.6 Nu5 Nv60",  45.0, 45.0, 0.2,  0.6,  5.0,  60.0, false, 0,0,0, 0,0,0,  0.0,  false },
        { "th=60 az90 rd.5 rs.3 Nu20 Nv80", 60.0, 90.0, 0.5,  0.3,  20.0, 80.0, false, 0,0,0, 0,0,0,  0.0,  false },
        { "th=45 az0 rd.05 rs.9 Nu2 Nv2",   45.0, 0.0,  0.05, 0.9,  2.0,  2.0,  false, 0,0,0, 0,0,0,  0.0,  true  },
        { "th=70 az30 rd.5 rs.02 Nu5 Nv5",  70.0, 30.0, 0.5,  0.02, 5.0,  5.0,  false, 0,0,0, 0,0,0,  0.0,  false },
        { "per-channel Nu/Nv",              45.0, 20.0, 0.5,  0.3,  0.0,  0.0,  true,  5,20,80, 80,20,5, 0.0, false },
        { "tilt 20 deg th=45 az0",          45.0, 0.0,  0.5,  0.3,  20.0, 80.0, false, 0,0,0, 0,0,0, 20.0, true  },
        { "tilt 30 deg th=45 az60",         45.0, 60.0, 0.5,  0.3,  20.0, 80.0, false, 0,0,0, 0,0,0, 30.0, false },
        // Adversarial: low, EQUAL Nu=Nv (broadest isotropic-in-shape lobe,
        // maximises the below-horizon spill under tilt -- same logic as
        // DL-98's N=1 rows) with Rs dominant, at a tilted normal.
        { "Nu2Nv2 rd.05 rs.95 tilt20 th45", 45.0, 0.0,  0.05, 0.95, 2.0,  2.0,  false, 0,0,0, 0,0,0, 20.0, true  },
        { "Nu2Nv2 rd.05 rs.95 tilt30 th45", 45.0, 90.0, 0.05, 0.95, 2.0,  2.0,  false, 0,0,0, 0,0,0, 30.0, false },
        { "Nu2Nv2 rd.05 rs.95 tilt20 th60", 60.0, 0.0,  0.05, 0.95, 2.0,  2.0,  false, 0,0,0, 0,0,0, 20.0, false },
        { "Nu2Nv2 rd.05 rs.95 tilt25 th50", 50.0, 45.0, 0.05, 0.95, 2.0,  2.0,  false, 0,0,0, 0,0,0, 25.0, true  },
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
        std::cout << "All AshikminShirleyAnisotropicPhongSPF Pdf consistency checks passed!" << std::endl;
        return 0;
    }
    return 1;
}
