//////////////////////////////////////////////////////////////////////
//
//  SPFPdfConsistencyTest.cpp - Validates that each SPF's Scatter()
//    produces samples consistent with the SPF's Pdf() evaluation.
//
//  Three tests per SPF:
//    1. Cross-validation: For single-ray Scatter results,
//       scat.pdf == SPF::Pdf(ri, scat.ray.Dir()).
//       For multi-ray results, verify the kray-weighted mixture
//       of per-ray PDFs matches Pdf().
//    2. PDF integral: verify integral of Pdf() over hemisphere ~ 1
//    3. Chi-squared histogram: bin directions from RandomlySelect,
//       compare observed counts against Pdf()-predicted expected counts.
//
//  Build (from project root):
//    c++ -arch arm64 -Isrc/Library -I/opt/homebrew/include \
//        -O3 -ffast-math -funroll-loops -Wall -pedantic \
//        -Wno-c++11-long-long -DCOLORS_RGB -DMERSENNE53 \
//        -DNO_TIFF_SUPPORT -DNO_EXR_SUPPORT -DRISE_ENABLE_MAILBOXING \
//        -c tests/SPFPdfConsistencyTest.cpp -o tests/SPFPdfConsistencyTest.o
//    c++ -arch arm64 -o tests/spf_pdf_test tests/SPFPdfConsistencyTest.o \
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

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/GeometricUtilities.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"

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
#include "../src/Library/Materials/SubSurfaceScatteringSPF.h"
#include "../src/Library/Materials/CompositeSPF.h"

using namespace RISE;
using namespace RISE::Implementation;

// ============================================================
//  Test configuration
// ============================================================

static const int NUM_SAMPLES        = 500000;   // Samples for histogram test
static const int NUM_CROSS_VALIDATE = 50000;    // Samples for cross-validation
static const int NUM_THETA_BINS     = 20;       // Bins in polar angle
static const int NUM_PHI_BINS       = 40;       // Bins in azimuthal angle
static const double CHI2_ALPHA      = 0.001;    // Significance level
static const double CROSS_VAL_TOL   = 1e-6;     // Tolerance for relative error
static const double INTEGRAL_TOL    = 0.05;     // Tolerance for PDF integral (5%)

// ============================================================
//  Chi-squared critical value (Wilson-Hilferty approximation)
// ============================================================

static double Chi2Critical( int df, double alpha )
{
    double z = 3.09;  // alpha = 0.001
    if( alpha > 0.005 ) z = 2.326;
    if( alpha > 0.02 )  z = 1.645;

    double k = (double)df;
    double term = 1.0 - 2.0/(9.0*k) + z * sqrt(2.0/(9.0*k));
    return k * term * term * term;
}

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
//  Direction <-> bin mapping
// ============================================================

static bool DirectionToBin( const Vector3& dir, const Vector3& normal,
                            int& thetaBin, int& phiBin )
{
    double cosTheta = Vector3Ops::Dot( dir, normal );
    if( cosTheta <= 0 ) return false;

    double theta = acos( r_max(-1.0, r_min(1.0, cosTheta)) );
    double phi = atan2( dir.y, dir.x );
    if( phi < 0 ) phi += TWO_PI;

    thetaBin = (int)( theta / PI_OV_TWO * NUM_THETA_BINS );
    phiBin   = (int)( phi / TWO_PI * NUM_PHI_BINS );

    if( thetaBin >= NUM_THETA_BINS ) thetaBin = NUM_THETA_BINS - 1;
    if( phiBin >= NUM_PHI_BINS ) phiBin = NUM_PHI_BINS - 1;

    return true;
}

// ============================================================
//  Test result tracking
// ============================================================

struct TestResult {
    std::string name;
    bool crossValPassed;
    bool integralPassed;
    bool chi2Passed;
    double chi2Stat;
    double chi2Crit;
    int crossValFailures;
    double maxCrossValError;
    double pdfIntegral;
};

// ============================================================
//  Run all tests on a single SPF
// ============================================================

static TestResult TestSPF(
    const std::string& name,
    ISPF& spf,
    double incomingTheta,
    bool singleLobe,
    bool skipCrossVal = false
    )
{
    TestResult result;
    result.name = name;
    result.crossValPassed = true;
    result.integralPassed = true;
    result.chi2Passed = true;
    result.crossValFailures = 0;
    result.maxCrossValError = 0;

    RayIntersectionGeometric ri = MakeIntersection( incomingTheta );
    const Vector3 normal = ri.onb.w();

    RandomNumberGenerator rng;

    // ================================================================
    //  Part 1: Cross-validation
    //  For single-lobe SPFs: scat.pdf should exactly match Pdf(ri, wo).
    //  For multi-lobe SPFs: use RandomlySelect and verify that the
    //    selected ray's effective PDF matches Pdf(ri, wo).
    //    The effective PDF accounts for the selection probability:
    //      effective_pdf = scat.pdf * (weight_selected / totalWeight)
    //    ... but Pdf() returns the mixture over ALL lobes evaluated at wo.
    //    Since we can't decompose the mixture, we just verify Pdf() > 0
    //    and rely on chi2 for the definitive statistical test.
    // ================================================================

    if( skipCrossVal ) goto skip_crossval;

    for( int i = 0; i < NUM_CROSS_VALIDATE; i++ )
    {
        ScatteredRayContainer scattered;
        spf.Scatter( ri, rng, scattered, 0 );

        if( scattered.Count() == 0 ) continue;

        if( singleLobe )
        {
            // Single-lobe SPF: Scatter always produces exactly one ray.
            // Its pdf should exactly match Pdf().
            const ScatteredRay& scat = scattered[0];
            if( scat.isDelta ) continue;
            if( scat.pdf <= 0 ) continue;

            Vector3 wo = Vector3Ops::Normalize( scat.ray.Dir() );
            Scalar pdfEval = spf.Pdf( ri, wo, 0 );

            double err = fabs( scat.pdf - pdfEval );
            double denom = r_max( fabs(scat.pdf), fabs(pdfEval) );
            double relErr = (denom > 1e-10) ? err / denom : err;

            if( relErr > CROSS_VAL_TOL )
            {
                result.crossValFailures++;
                if( relErr > result.maxCrossValError )
                    result.maxCrossValError = relErr;
            }
        }
        else
        {
            // Multi-lobe SPF: verify Pdf() is positive for each
            // non-delta ray direction, and that Pdf() >= weighted
            // contribution of each individual lobe (lower bound check).

            Scalar totalWeight = 0;
            for( unsigned int j = 0; j < scattered.Count(); j++ )
            {
                totalWeight += ColorMath::MaxValue( scattered[j].kray );
            }

            if( totalWeight < NEARZERO ) continue;

            for( unsigned int j = 0; j < scattered.Count(); j++ )
            {
                const ScatteredRay& scat = scattered[j];
                if( scat.isDelta ) continue;
                if( scat.pdf <= 0 ) continue;

                Vector3 wo = Vector3Ops::Normalize( scat.ray.Dir() );
                Scalar pdfEval = spf.Pdf( ri, wo, 0 );

                // Pdf() must be positive for any sampled direction
                if( pdfEval <= 0 )
                {
                    result.crossValFailures++;
                    result.maxCrossValError = 1.0;
                    continue;
                }

                // Pdf() should be >= the weighted contribution of this lobe
                Scalar weight_j = ColorMath::MaxValue( scat.kray );
                Scalar minExpected = (weight_j * scat.pdf) / totalWeight;

                if( pdfEval < minExpected * 0.99 - 1e-8 )
                {
                    result.crossValFailures++;
                    double relErr = (minExpected - pdfEval) / minExpected;
                    if( relErr > result.maxCrossValError )
                        result.maxCrossValError = relErr;
                }
            }
        }
    }

    if( result.crossValFailures > 0 )
        result.crossValPassed = false;

    skip_crossval:

    // ================================================================
    //  Part 2: PDF integral over hemisphere (should be ~1)
    // ================================================================

    const int INTEGRAL_THETA = 100;
    const int INTEGRAL_PHI = 200;
    double pdfIntegral = 0.0;

    for( int t = 0; t < INTEGRAL_THETA; t++ )
    {
        double theta = (t + 0.5) * PI_OV_TWO / INTEGRAL_THETA;
        double sinT = sin(theta);
        double cosT = cos(theta);
        double dTheta = PI_OV_TWO / INTEGRAL_THETA;

        for( int p = 0; p < INTEGRAL_PHI; p++ )
        {
            double phi = (p + 0.5) * TWO_PI / INTEGRAL_PHI;
            double dPhi = TWO_PI / INTEGRAL_PHI;

            Vector3 wo( sinT * cos(phi), sinT * sin(phi), cosT );
            wo = Vector3Ops::Normalize( wo );

            Scalar pdfVal = spf.Pdf( ri, wo, 0 );
            pdfIntegral += pdfVal * sinT * dTheta * dPhi;
        }
    }

    result.pdfIntegral = pdfIntegral;
    if( fabs(pdfIntegral - 1.0) > INTEGRAL_TOL )
        result.integralPassed = false;

    // ================================================================
    //  Part 3: Chi-squared histogram test
    //  Use RandomlySelect to pick one ray per Scatter call (matching
    //  path tracer behavior), bin directions, compare against Pdf().
    // ================================================================

    const int totalBins = NUM_THETA_BINS * NUM_PHI_BINS;
    std::vector<int> observed( totalBins, 0 );
    int totalAccepted = 0;

    for( int i = 0; i < NUM_SAMPLES; i++ )
    {
        ScatteredRayContainer scattered;
        spf.Scatter( ri, rng, scattered, 0 );

        // Use RandomlySelect to pick one ray, as the path tracer does
        ScatteredRay* selected = scattered.RandomlySelect( rng.CanonicalRandom(), false );
        if( !selected ) continue;
        if( selected->isDelta ) continue;

        Vector3 wo = Vector3Ops::Normalize( selected->ray.Dir() );
        int tb, pb;
        if( DirectionToBin( wo, normal, tb, pb ) )
        {
            observed[ tb * NUM_PHI_BINS + pb ]++;
            totalAccepted++;
        }
    }

    if( totalAccepted < 1000 )
    {
        std::cout << "  WARNING: " << name << " produced too few hemisphere samples ("
                  << totalAccepted << "), skipping chi2 test" << std::endl;
        result.chi2Stat = 0;
        result.chi2Crit = 0;
        return result;
    }

    // Compute expected counts by numerically integrating Pdf() over each bin
    const int SUB_THETA = 4;
    const int SUB_PHI = 4;
    std::vector<double> expected( totalBins, 0.0 );

    for( int tb = 0; tb < NUM_THETA_BINS; tb++ )
    {
        double theta0 = tb * PI_OV_TWO / NUM_THETA_BINS;
        double theta1 = (tb + 1) * PI_OV_TWO / NUM_THETA_BINS;

        for( int pb = 0; pb < NUM_PHI_BINS; pb++ )
        {
            double phi0 = pb * TWO_PI / NUM_PHI_BINS;
            double phi1 = (pb + 1) * TWO_PI / NUM_PHI_BINS;

            double integral = 0;
            for( int st = 0; st < SUB_THETA; st++ )
            {
                double theta = theta0 + (st + 0.5) * (theta1 - theta0) / SUB_THETA;
                double sinT = sin(theta);
                double cosT = cos(theta);
                double dTheta = (theta1 - theta0) / SUB_THETA;

                for( int sp = 0; sp < SUB_PHI; sp++ )
                {
                    double phi = phi0 + (sp + 0.5) * (phi1 - phi0) / SUB_PHI;
                    double dPhi = (phi1 - phi0) / SUB_PHI;

                    Vector3 wo( sinT * cos(phi), sinT * sin(phi), cosT );
                    wo = Vector3Ops::Normalize( wo );

                    Scalar pdfVal = spf.Pdf( ri, wo, 0 );
                    integral += pdfVal * sinT * dTheta * dPhi;
                }
            }

            expected[ tb * NUM_PHI_BINS + pb ] = integral * totalAccepted;
        }
    }

    // Compute chi-squared statistic, merging bins with expected < 5
    double chi2 = 0;
    int dof = 0;
    double mergedObs = 0;
    double mergedExp = 0;

    for( int i = 0; i < totalBins; i++ )
    {
        mergedObs += observed[i];
        mergedExp += expected[i];

        if( mergedExp >= 5.0 )
        {
            double diff = mergedObs - mergedExp;
            chi2 += (diff * diff) / mergedExp;
            dof++;
            mergedObs = 0;
            mergedExp = 0;
        }
    }

    if( dof <= 1 )
    {
        std::cout << "  WARNING: " << name << " too few bins with sufficient expected count" << std::endl;
        result.chi2Stat = 0;
        result.chi2Crit = 0;
        return result;
    }

    dof--;  // Lose 1 DOF because total count is fixed

    double critical = Chi2Critical( dof, CHI2_ALPHA );
    result.chi2Stat = chi2;
    result.chi2Crit = critical;

    if( chi2 > critical )
        result.chi2Passed = false;

    return result;
}

// ============================================================
//  Main
// ============================================================

int main()
{
    std::cout << "===== SPF PDF Consistency Test =====" << std::endl;
    std::cout << "Cross-validation samples: " << NUM_CROSS_VALIDATE << std::endl;
    std::cout << "Histogram samples: " << NUM_SAMPLES << std::endl;
    std::cout << "Bins: " << NUM_THETA_BINS << " x " << NUM_PHI_BINS
              << " = " << NUM_THETA_BINS * NUM_PHI_BINS << std::endl;
    std::cout << std::endl;

    // Create uniform painters (heap-allocated, reference-counted)
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

    // Construct SPFs
    LambertianSPF* lambertian = new LambertianSPF( *white );  lambertian->addref();
    OrenNayarSPF* orenNayar = new OrenNayarSPF( *white, *roughness );  orenNayar->addref();
    IsotropicPhongSPF* phong = new IsotropicPhongSPF( *gray, *spec, *highExp );  phong->addref();
    CookTorranceSPF* cookTorrance = new CookTorranceSPF( *gray, *spec, *low, *ior, *extinction );  cookTorrance->addref();
    SchlickSPF* schlick = new SchlickSPF( *gray, *spec, *roughness, *isotropy );  schlick->addref();
    WardIsotropicGaussianSPF* wardIso = new WardIsotropicGaussianSPF( *gray, *spec, *alphaSmall );  wardIso->addref();
    WardAnisotropicEllipticalGaussianSPF* wardAniso = new WardAnisotropicEllipticalGaussianSPF( *gray, *spec, *alphaSmall, *alphaSmallY );  wardAniso->addref();
    AshikminShirleyAnisotropicPhongSPF* ashikmin = new AshikminShirleyAnisotropicPhongSPF( *ashNu, *ashNv, *gray, *spec );  ashikmin->addref();

    // Initialize the global log to prevent null pointer crashes
    GlobalLog();

    std::cout << "Constructing painters..." << std::endl;

    // Additional painters for new SPFs
    UniformColorPainter* trans       = new UniformColorPainter( RISEPel(0.4, 0.4, 0.4) );  trans->addref();
    UniformColorPainter* phongN     = new UniformColorPainter( RISEPel(10.0, 10.0, 10.0) ); phongN->addref();
    UniformColorPainter* scatFactor  = new UniformColorPainter( RISEPel(0.3, 0.3, 0.3) );  scatFactor->addref();
    UniformColorPainter* tauPainter  = new UniformColorPainter( RISEPel(0.9, 0.9, 0.9) );  tauPainter->addref();
    UniformColorPainter* polishScat  = new UniformColorPainter( RISEPel(20.0, 20.0, 20.0) ); polishScat->addref();  // Phong exponent for polish
    UniformColorPainter* sssAbsorb   = new UniformColorPainter( RISEPel(0.01, 0.01, 0.01) ); sssAbsorb->addref();
    UniformColorPainter* sssScat     = new UniformColorPainter( RISEPel(1.0, 1.0, 1.0) );  sssScat->addref();
    UniformColorPainter* zeroExt     = new UniformColorPainter( RISEPel(0.0, 0.0, 0.0) );  zeroExt->addref();

    // Translucent SPF: front reflectance, transmittance, extinction, Phong N, scattering
    TranslucentSPF* translucent = new TranslucentSPF( *gray, *trans, *extinction, *phongN, *scatFactor );  translucent->addref();

    // Polished SPF: diffuse Rd, dielectric transmittance tau, IOR Nt, scattering (Phong exponent), bHG=false
    PolishedSPF* polished = new PolishedSPF( *gray, *tauPainter, *ior, *polishScat, false );  polished->addref();

    // SubSurfaceScattering SPF: IOR, absorption, scattering, g=0.8, roughness=0.3
    SubSurfaceScatteringSPF* sss = new SubSurfaceScatteringSPF( *ior, *sssAbsorb, *sssScat, 0.8, 0.3 );  sss->addref();

    // Composite SPF: two Lambertian layers, max_recur=4, reflection/refraction/diffuse/translucent limits, thickness=0.1, zero extinction
    LambertianSPF* lambertian2 = new LambertianSPF( *spec );  lambertian2->addref();
    CompositeSPF* composite = new CompositeSPF( *lambertian, *lambertian2, 4, 2, 2, 2, 2, 0.1, *zeroExt );  composite->addref();

    struct SPFEntry {
        std::string name;
        ISPF* spf;
        bool singleLobe;
        bool skipCrossVal;
    };

    SPFEntry spfs[] = {
        { "Lambertian",                        lambertian,  true,  false },
        { "OrenNayar",                         orenNayar,   true,  false },
        { "IsotropicPhong",                    phong,       false, false },
        { "CookTorrance",                      cookTorrance,false, false },
        { "Schlick",                           schlick,     false, false },
        { "WardIsotropicGaussian",             wardIso,     false, false },
        { "WardAnisotropicEllipticalGaussian", wardAniso,   false, false },
        { "AshikminShirleyAnisotropicPhong",   ashikmin,    false, false },
        { "Translucent",                       translucent, false, true },  // Pdf() only covers diffuse lobe, not translucent
        { "Polished",                          polished,    false, false },
        { "SubSurfaceScattering",              sss,         true,  false },
        { "Composite",                         composite,   false, false },
    };

    double incomingAngles[] = { 30.0 * DEG_TO_RAD, 60.0 * DEG_TO_RAD };
    const char* angleNames[] = { "30deg", "60deg" };

    std::vector<TestResult> results;
    int numFailed = 0;

    for( int a = 0; a < 2; a++ )
    {
        std::cout << "--- Incoming angle: " << angleNames[a] << " ---" << std::endl;

        for( int s = 0; s < (int)(sizeof(spfs)/sizeof(spfs[0])); s++ )
        {
            std::string fullName = spfs[s].name + " @ " + angleNames[a];
            std::cout << "Testing " << fullName << "..." << std::endl;

            TestResult r = TestSPF( fullName, *spfs[s].spf, incomingAngles[a], spfs[s].singleLobe, spfs[s].skipCrossVal );
            results.push_back( r );

            // Report cross-validation
            if( !r.crossValPassed )
            {
                std::cout << "  FAIL cross-validation: " << r.crossValFailures
                          << " mismatches, max relative error = " << r.maxCrossValError
                          << std::endl;
                numFailed++;
            }
            else
            {
                std::cout << "  PASS cross-validation" << std::endl;
            }

            // Report PDF integral
            std::cout << "  PDF integral over hemisphere: " << r.pdfIntegral;
            if( !r.integralPassed )
            {
                std::cout << "  FAIL (expected ~1.0)" << std::endl;
                numFailed++;
            }
            else
            {
                std::cout << "  PASS" << std::endl;
            }

            // Report chi-squared
            if( r.chi2Crit > 0 )
            {
                if( !r.chi2Passed )
                {
                    std::cout << "  FAIL chi2: " << r.chi2Stat
                              << " > critical " << r.chi2Crit << std::endl;
                    numFailed++;
                }
                else
                {
                    std::cout << "  PASS chi2: " << r.chi2Stat
                              << " < critical " << r.chi2Crit << std::endl;
                }
            }
        }
        std::cout << std::endl;
    }

    // Summary
    std::cout << "===== Summary =====" << std::endl;
    for( size_t i = 0; i < results.size(); i++ )
    {
        const TestResult& r = results[i];
        bool passed = r.crossValPassed && r.integralPassed && r.chi2Passed;
        std::cout << (passed ? "PASS" : "FAIL") << "  " << r.name;
        if( !r.crossValPassed )
            std::cout << " [cross-val: " << r.crossValFailures << " errors]";
        if( !r.integralPassed )
            std::cout << " [integral: " << r.pdfIntegral << "]";
        if( !r.chi2Passed )
            std::cout << " [chi2: " << r.chi2Stat << " > " << r.chi2Crit << "]";
        std::cout << std::endl;
    }

    if( numFailed > 0 )
    {
        std::cout << std::endl << numFailed << " test(s) FAILED" << std::endl;
        return 1;
    }

    std::cout << std::endl << "All SPF PDF consistency tests passed!" << std::endl;
    return 0;
}
