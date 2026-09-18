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
//  GATE 1 -- NORMALISATION, TWO-SIDED, 1%.  The quadrature runs over the
//  FULL SPHERE, matching tests/IsotropicPhongSPFPdfConsistencyTest.cpp.
//  Unlike Phong, this SPF's own sampler CANNOT emit below the shading
//  hemisphere (GenerateSpecularRay rejects `Dot(k2, onb.w()) < 0` and the
//  diffuse lobe is a cosine hemisphere about the same frame), and Pdf()
//  early-outs at `cosO <= 0` before any quadrature runs -- so the lower
//  half costs almost nothing and is carried as a guard that the two
//  supports agree.  The `belowZ` column below is the measured fraction of
//  emitted directions on the far side of the SAMPLING frame's normal; it
//  is expected to read 0 on every row here.
//
//  GATE 2 -- TOTAL VARIATION vs 600 000 real Scatter()+RandomlySelect()
//  draws, 12x8 equal-solid-angle bins, threshold 0.012 (same
//  Cauchy-Schwarz floor as the other two files, K=96 N=600000).
//
//  GATE 3 -- NO NEGATIVE SELECTION WEIGHT.  Every ray in the container is
//  inspected for the exact quantity `RandomlySelect` reads
//  (`MaxValue(kray)` / `krayNM`).  A negative value is unphysical: it
//  cannot be a probability mass, it reverses the CDF ordering inside
//  `RandomlySelect`, and `RandomlySelectNonDiffuse` (SMSPhotonMap.cpp,
//  CausticPelPhotonTracer.cpp, CausticSpectralPhotonTracer.cpp) returns
//  such a ray with no weight test at all.  The `backface` rows below are
//  this gate's red-proof.
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
//  `backFace` makes the ray arrive from BELOW the surface (`inDir.z` is
//  +cos instead of -cos) while `ri.onb`/`ri.vNormal` keep pointing at +Z,
//  which is exactly the condition `Scatter`/`ScatterNM`/`Pdf` test with
//  `Dot(ri.ray.Dir(), ri.onb.w()) > NEARZERO` before calling
//  `myonb.FlipW()`.  The SAMPLING frame's normal is then (0,0,-1), which
//  is why every frame-relative quantity below is expressed against `NZ`.
static RayIntersectionGeometric MakeIntersection( double incomingTheta, double azimuthDeg, double tiltDeg, bool backFace )
{
    const double nz = backFace ? -1.0 : 1.0;
    const double sinT = sin(incomingTheta);
    const double cosT = cos(incomingTheta);
    const double az = azimuthDeg * PI / 180.0;
    const Vector3 inDir( sinT*cos(az), sinT*sin(az), -cosT*nz );

    Ray inRay( Point3(-inDir.x, -inDir.y, nz), inDir );
    RasterizerState rs = {0, 0};
    RayIntersectionGeometric ri( inRay, rs );

    ri.bHit = true;
    ri.range = 1.0 / cosT;
    ri.ptIntersection = Point3(0, 0, 0);
    ri.vNormal = Vector3(0, 0, 1);
    ri.onb.CreateFromW( Vector3(0, 0, 1) );
    ri.ptCoord = Point2(0.5, 0.5);

    if( tiltDeg > 0 ) {
        // Tilt toward +X (arbitrary, fixed axis; azimuth varies the RAY
        // instead so the tilt axis and the ray azimuth are independent).
        // Reported in the SAMPLING frame's hemisphere so the intended tilt
        // angle is the same on a front- and a back-face hit; every
        // consumer re-anchors it to the ray direction anyway.
        const double t = tiltDeg * PI / 180.0;
        ri.vGeomNormal = Vector3( sin(t), 0, cos(t)*nz );
    }

    return ri;
}

static const int kNT = 12;
static const int kNP = 8;
static const int kNBins = kNT * kNP;

// `nz` is the SAMPLING frame's normal z-component (+1 front face, -1 back
// face), so the polar row is always keyed on dot(d, n) and the empirical
// histogram and the Pdf quadrature resolve the same hemisphere.
static int BinOf( const Vector3& d, double nz )
{
    double c = d.z * nz;
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
    bool   backFace;
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

    RayIntersectionGeometric ri = MakeIntersection( c.thetaDeg * PI / 180.0, c.azimuthDeg, c.tiltDeg, c.backFace );
    const double NZ = c.backFace ? -1.0 : 1.0;
    IORStack iorStack = MakeTestIORStack( g_stubObject );

    const bool bNM = (nm > 0);

    RandomNumberGenerator rng( bNM ? 707070 : 515151 );
    Implementation::IndependentSampler sampler( rng );

    std::vector<double> emp( kNBins, 0.0 );
    long emitted = 0;
    long belowZ  = 0;
    long negKray = 0;
    double minKray = 0;

    for( long i = 0; i < kDraws; i++ ) {
        ScatteredRayContainer scattered;
        if( bNM ) {
            spf->ScatterNM( ri, sampler, nm, scattered, iorStack );
        } else {
            spf->Scatter( ri, sampler, scattered, iorStack );
        }

        // GATE 3: the selection weight RandomlySelect actually reads.  A
        // negative one is unphysical -- it cannot be a probability mass,
        // it silently reverses the CDF ordering inside RandomlySelect, and
        // RandomlySelectNonDiffuse (SMSPhotonMap.cpp, the two caustic
        // photon tracers) hands such a ray straight back with no weight
        // test at all.
        for( unsigned int k = 0; k < scattered.Count(); k++ ) {
            const Scalar w = bNM ? scattered[k].krayNM
                                 : ColorMath::MaxValue( scattered[k].kray );
            if( w < 0 ) {
                negKray++;
                if( w < minKray ) {
                    minKray = w;
                }
            }
        }

        ScatteredRay* sel = scattered.RandomlySelect( rng.CanonicalRandom(), bNM );
        if( sel ) {
            emitted++;
            const Vector3 d = Vector3Ops::Normalize( sel->ray.Dir() );
            if( d.z * NZ <= 0 ) {
                belowZ++;
            }
            emp[ BinOf( d, NZ ) ] += 1.0;
        }
    }

    const double massEmp = (double)emitted / (double)kDraws;
    for( int k = 0; k < kNBins; k++ ) {
        emp[k] /= (double)kDraws;
    }

    std::vector<double> quad( kNBins, 0.0 );
    double intPdf = 0;
    // FULL SPHERE (see the header comment): 2*kQT rows of cos(theta)
    // spanning [-1,1], per-row weight and upper-hemisphere resolution
    // unchanged from the old hemisphere-only grid.
    const double dw = (1.0/kQT) * (TWO_PI/kQP);
    for( int a = 0; a < 2*kQT; a++ ) {
        const double ct = -1.0 + (a + 0.5)/kQT;
        const double st = sqrt( r_max( 0.0, 1.0 - ct*ct ) );
        for( int b = 0; b < kQP; b++ ) {
            const double ph = (b + 0.5)/kQP * TWO_PI;
            const Vector3 wo( st*cos(ph), st*sin(ph), ct );
            const double pdf = bNM ? spf->PdfNM( ri, wo, nm, iorStack )
                                   : spf->Pdf( ri, wo, iorStack );
            intPdf += pdf * dw;
            quad[ BinOf(wo, NZ) ] += pdf * dw;
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
              << "  belowZ=" << (emitted ? (double)belowZ/(double)emitted : 0.0)
              << "  negKray=" << negKray << "(min " << std::setprecision(4) << minKray << std::setprecision(5) << ")"
              << std::endl;

    CHECK( fabs(intPdf - massEmp) <= kMassTol,
        std::string(c.name) + (bNM?" (NM)":" (RGB)") + ": integral of Pdf must match the measured probability that Scatter emits a ray, within tolerance" );
    CHECK( tvd <= kTvdTol,
        std::string(c.name) + (bNM?" (NM)":" (RGB)") + ": total variation between Pdf and the real Scatter+RandomlySelect histogram must sit at the MC noise floor" );
    CHECK( negKray == 0,
        std::string(c.name) + (bNM?" (NM)":" (RGB)") + ": no scattered ray may carry a NEGATIVE selection weight (MaxValue(kray)/krayNM)" );

    spf->release(); nuP->release(); nvP->release(); diff->release(); spec->release();
}

int main()
{
    std::cout << "===== AshikminShirleyAnisotropicPhongSPF Pdf Consistency Test (DL-99) =====" << std::endl;

    g_stubObject = new StubObject();
    g_stubObject->addref();
    GlobalLog();

    const Config cfgs[] = {
        // name                          th    az    rd    rs    Nu    Nv    perCh  Nur  Nug  Nub  Nvr  Nvg  Nvb   tilt  NM     backFace
        { "th=10 az0 rd.5 rs.3 Nu20 Nv80",  10.0, 0.0,  0.5,  0.3,  20.0, 80.0, false, 0,0,0, 0,0,0,  0.0,  true , false },
        { "th=30 az0 rd.5 rs.3 Nu20 Nv80",  30.0, 0.0,  0.5,  0.3,  20.0, 80.0, false, 0,0,0, 0,0,0,  0.0,  true , false },
        { "th=45 az45 rd.2 rs.6 Nu5 Nv60",  45.0, 45.0, 0.2,  0.6,  5.0,  60.0, false, 0,0,0, 0,0,0,  0.0,  false, false },
        { "th=60 az90 rd.5 rs.3 Nu20 Nv80", 60.0, 90.0, 0.5,  0.3,  20.0, 80.0, false, 0,0,0, 0,0,0,  0.0,  false, false },
        { "th=45 az0 rd.05 rs.9 Nu2 Nv2",   45.0, 0.0,  0.05, 0.9,  2.0,  2.0,  false, 0,0,0, 0,0,0,  0.0,  true , false },
        { "th=70 az30 rd.5 rs.02 Nu5 Nv5",  70.0, 30.0, 0.5,  0.02, 5.0,  5.0,  false, 0,0,0, 0,0,0,  0.0,  false, false },
        { "per-channel Nu/Nv",              45.0, 20.0, 0.5,  0.3,  0.0,  0.0,  true,  5,20,80, 80,20,5, 0.0, false, false },
        { "tilt 20 deg th=45 az0",          45.0, 0.0,  0.5,  0.3,  20.0, 80.0, false, 0,0,0, 0,0,0, 20.0, true , false },
        { "tilt 30 deg th=45 az60",         45.0, 60.0, 0.5,  0.3,  20.0, 80.0, false, 0,0,0, 0,0,0, 30.0, false, false },
        // Adversarial: low, EQUAL Nu=Nv (broadest isotropic-in-shape lobe,
        // maximises the below-horizon spill under tilt -- same logic as
        // DL-98's N=1 rows) with Rs dominant, at a tilted normal.
        { "Nu2Nv2 rd.05 rs.95 tilt20 th45", 45.0, 0.0,  0.05, 0.95, 2.0,  2.0,  false, 0,0,0, 0,0,0, 20.0, true , false },
        { "Nu2Nv2 rd.05 rs.95 tilt30 th45", 45.0, 90.0, 0.05, 0.95, 2.0,  2.0,  false, 0,0,0, 0,0,0, 30.0, false, false },
        { "Nu2Nv2 rd.05 rs.95 tilt20 th60", 60.0, 0.0,  0.05, 0.95, 2.0,  2.0,  false, 0,0,0, 0,0,0, 20.0, false, false },
        { "Nu2Nv2 rd.05 rs.95 tilt25 th50", 50.0, 45.0, 0.05, 0.95, 2.0,  2.0,  false, 0,0,0, 0,0,0, 25.0, true , false },
        // BACK-FACE hits (DL-100 sibling row).  The ray arrives from below
        // while ri.onb/ri.vNormal still point at +Z, so Scatter/ScatterNM
        // FlipW the sampling frame.  Pre-fix, Scatter's own cos_o /
        // cos_o_diff / cos_i were all taken against the UNFLIPPED
        // ri.onb.w(), so every one of them came out with the wrong sign:
        // the diffuse kray collapsed to 0 and the specular kray went
        // NEGATIVE.  Three incidence angles because the emitted mass that
        // survives is P(the diffuse ray is dropped by the geomN gate),
        // which is strongly angle-dependent.
        { "backface th=10 Nu20 Nv80",       10.0, 0.0,  0.5,  0.3,  20.0, 80.0, false, 0,0,0, 0,0,0,  0.0, true , true  },
        { "backface th=45 Nu20 Nv80",       45.0, 0.0,  0.5,  0.3,  20.0, 80.0, false, 0,0,0, 0,0,0,  0.0, true , true  },
        { "backface th=70 az30 Nu5 Nv5",    70.0, 30.0, 0.5,  0.02, 5.0,  5.0,  false, 0,0,0, 0,0,0,  0.0, false, true  },
        { "backface tilt20 th=45 az60",     45.0, 60.0, 0.5,  0.3,  20.0, 80.0, false, 0,0,0, 0,0,0, 20.0, false, true  },
        { "backface per-channel Nu/Nv",     45.0, 20.0, 0.5,  0.3,  0.0,  0.0,  true,  5,20,80, 80,20,5, 0.0, false, true  },
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
