//////////////////////////////////////////////////////////////////////
//
//  MatchedIndexGrazingConsumerTest.cpp - DL-58 consumer regressions.
//
//  Matched refractive indices describe no optical boundary.  In
//  particular, a tiny but positive grazing cosine must not turn into TIR
//  merely because 1-cos^2 rounds to one.  The helper-level tests cover the
//  individual formulas; this file pins the real SSS consumers that convert
//  those formulas into stack changes and BSDF/SPF throughput.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Materials/SubSurfaceScatteringBSDF.h"
#include "../src/Library/Materials/SubSurfaceScatteringSPF.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/ISampler.h"
#include "../src/Library/Utilities/MicrofacetUtils.h"
#include "../src/Library/Utilities/Ray.h"

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
    int gChecks = 0;
    int gFailures = 0;

    const Scalar kGrazingCos = 1e-7;
    const Scalar kBRDFGrazingCos = 1e-9;
    // The SPF's outgoing-direction and PDF guards require positive values
    // above 1e-10.  This still-extreme incident cosine lets a false-TIR-band
    // microfacet cosine clear both production support checks.
    const Scalar kSPFIncidentCos = 2e-10;
    const Scalar kMatchedIOR = 1.5;
    const Scalar kTol = 1e-12;

    void Check( const bool condition, const char* const message )
    {
        ++gChecks;
        if( !condition ) {
            ++gFailures;
            std::cerr << "FAIL: " << message << std::endl;
        }
    }

    void CheckNear( const Scalar actual, const Scalar expected,
        const char* const message, const Scalar tolerance = kTol )
    {
        ++gChecks;
        const Scalar scaledTolerance = tolerance * std::fmax( Scalar(1), std::fabs(expected) );
        if( !std::isfinite(actual) || !std::isfinite(expected) ||
            std::fabs(actual-expected) > scaledTolerance ) {
            ++gFailures;
            std::cerr << "FAIL: " << message << " (got " << actual
                << ", expected " << expected << ")" << std::endl;
        }
    }

    bool IsFinitePel( const RISEPel& value )
    {
        return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
    }

    class OrderedSampler : public ISampler
    {
    public:
        explicit OrderedSampler( const std::vector<Scalar>& values ) :
            mValues( values ), mNext( 0 ), mOverdraws( 0 ) {}

        Scalar Get1D() override
        {
            if( mNext >= mValues.size() ) {
                ++mOverdraws;
                return 0;
            }
            return mValues[mNext++];
        }

        Point2 Get2D() override
        {
            // Do not hide sample ordering in a synthetic 2-D generator.
            const Scalar first = Get1D();
            const Scalar second = Get1D();
            return Point2( first, second );
        }

        void StartStream( int ) override {}
        size_t Draws() const { return mNext; }
        size_t Overdraws() const { return mOverdraws; }

    private:
        std::vector<Scalar> mValues;
        size_t mNext;
        size_t mOverdraws;
    };

    RayIntersectionGeometric MakeInsideHit( const Scalar cosine )
    {
        const Scalar sine = std::sqrt( Scalar(1) - cosine*cosine );
        const Ray ray( Point3( 0, 0, -1 ), Vector3( sine, 0, cosine ) );
        RayIntersectionGeometric ri( ray, nullRasterizerState );
        ri.bHit = true;
        ri.range = 1;
        ri.ptIntersection = Point3( 0, 0, 0 );
        ri.vNormal = ri.vGeomNormal = Vector3( 0, 0, 1 );
        ri.onb.CreateFromW( ri.vNormal );
        ri.ptCoord = Point2( 0.5, 0.5 );
        return ri;
    }

    // Outgoing and light directions are symmetric about +Z.  Their
    // half-vector is exactly +Z even when each direction is grazing.
    RayIntersectionGeometric MakeOutsideHit( const Scalar cosine )
    {
        const Scalar sine = std::sqrt( Scalar(1) - cosine*cosine );
        const Vector3 wo( sine, 0, cosine );
        const Ray ray( Point3( wo.x, wo.y, wo.z ), -wo );
        RayIntersectionGeometric ri( ray, nullRasterizerState );
        ri.bHit = true;
        ri.range = 1;
        ri.ptIntersection = Point3( 0, 0, 0 );
        ri.vNormal = ri.vGeomNormal = Vector3( 0, 0, 1 );
        ri.onb.CreateFromW( ri.vNormal );
        ri.ptCoord = Point2( 0.5, 0.5 );
        return ri;
    }

    Vector3 SymmetricLightDirection( const Scalar cosine )
    {
        return Vector3( -std::sqrt( Scalar(1) - cosine*cosine ), 0, cosine );
    }

    const ScatteredRay* FindRay( const ScatteredRayContainer& rays,
        const ScatteredRay::ScatRayType type )
    {
        for( unsigned int i = 0; i < rays.Count(); ++i ) {
            if( rays[i].type == type ) return &rays[i];
        }
        return 0;
    }

    IORStack MatchedInsideStack( const IObject* const object )
    {
        // The environment and current medium are intentionally both 1.5.
        // This makes the stack transition observable without inventing a
        // non-matched interface.  New Reference objects start at one.
        IORStack stack( kMatchedIOR );
        stack.SetCurrentObject( object );
        stack.push( kMatchedIOR );
        return stack;
    }

    void CheckPoppedExitStack( const IORStack& input,
        const ScatteredRay* const transmission, const IObject* const object,
        const char* const label )
    {
        Check( input.containsCurrent(), "input stack remains inside current SSS object" );
        CheckNear( input.top(), kMatchedIOR, "input stack retains matched current-medium IOR" );
        Check( transmission != 0, label );
        if( transmission ) {
            Check( transmission->ior_stack != 0, "transmission publishes a stack" );
            if( transmission->ior_stack ) {
                Check( transmission->delete_stack, "transmission owns its copied stack" );
                Check( !transmission->ior_stack->containsCurrent(),
                    "matched-index transmission pops the current object" );
                Check( transmission->ior_stack->topObject() != object,
                    "matched-index transmission no longer names the exited object" );
                CheckNear( transmission->ior_stack->top(), kMatchedIOR,
                    "matched-index transmission retains the popped destination IOR" );
            }
        }
    }

    void TestSmoothStandaloneExit( const IObject* const object )
    {
        std::cout << "Standalone smooth SSS matched-index grazing exit" << std::endl;
        UniformScalarPainter* const ior = new UniformScalarPainter( kMatchedIOR );
        SubSurfaceScatteringSPF* const spf =
            new SubSurfaceScatteringSPF( *ior, 0, 0, false );
        const RayIntersectionGeometric ri = MakeInsideHit( kGrazingCos );

        for( unsigned int nm = 0; nm < 2; ++nm ) {
            IORStack input = MatchedInsideStack( object );
            OrderedSampler sampler{ std::vector<Scalar>{} };
            ScatteredRayContainer rays;
            if( nm ) spf->ScatterNM( ri, sampler, 550, rays, input );
            else     spf->Scatter( ri, sampler, rays, input );

            const ScatteredRay* const reflection = FindRay( rays, ScatteredRay::eRayReflection );
            const ScatteredRay* const transmission = FindRay( rays, ScatteredRay::eRayRefraction );
            Check( rays.Count() == 2 && reflection && transmission,
                "matched-index standalone exit keeps reflection record and active transmission" );
            Check( sampler.Draws() == 0 && sampler.Overdraws() == 0,
                "smooth inside exit is delta and consumes no sampler values" );
            if( reflection && transmission ) {
                Check( reflection->isDelta && transmission->isDelta,
                    "smooth inside exit has delta lobes" );
                CheckNear( reflection->pdf, 1, "smooth reflection has unit discrete PDF" );
                CheckNear( transmission->pdf, 1, "smooth transmission has unit discrete PDF" );
                if( nm ) {
                    CheckNear( reflection->krayNM, 0,
                        "NM matched-index grazing exit has zero Fresnel reflection" );
                    CheckNear( transmission->krayNM, 1,
                        "NM matched-index grazing exit transmits all throughput" );
                } else {
                    for( unsigned int channel = 0; channel < 3; ++channel ) {
                        CheckNear( reflection->kray[channel], 0,
                            "RGB matched-index grazing exit has zero Fresnel reflection" );
                        CheckNear( transmission->kray[channel], 1,
                            "RGB matched-index grazing exit transmits all throughput" );
                    }
                }
            }
            CheckPoppedExitStack( input, transmission, object,
                "matched-index extreme-grazing exit must emit a transmission ray" );
        }

        spf->release();
        ior->release();
    }

    void TestRoughBSDFMatchedFresnel()
    {
        std::cout << "Rough SSS BSDF matched-index microfacet Fresnel" << std::endl;
        const RayIntersectionGeometric ri = MakeOutsideHit( kBRDFGrazingCos );
        const Vector3 wi = SymmetricLightDirection( kBRDFGrazingCos );
        const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );
        const Vector3 h = Vector3Ops::Normalize( wo + wi );
        CheckNear( Vector3Ops::Dot( h, ri.onb.w() ), 1,
            "symmetric grazing directions put the half-vector at the surface normal" );

        UniformScalarPainter* const matchedIOR = new UniformScalarPainter( 1 );
        UniformScalarPainter* const controlIOR = new UniformScalarPainter( kMatchedIOR );
        SubSurfaceScatteringBSDF* const matched =
            new SubSurfaceScatteringBSDF( *matchedIOR, 0, 0.5 );
        SubSurfaceScatteringBSDF* const control =
            new SubSurfaceScatteringBSDF( *controlIOR, 0, 0.5 );

        const RISEPel matchedRGB = matched->value( wi, ri );
        const Scalar matchedNM = matched->valueNM( wi, ri, 550 );
        const RISEPel controlRGB = control->value( wi, ri );
        const Scalar controlNM = control->valueNM( wi, ri, 550 );

        Check( IsFinitePel( matchedRGB ), "matched RGB rough SSS value is finite" );
        Check( std::isfinite( matchedNM ), "matched NM rough SSS value is finite" );
        CheckNear( matchedRGB[0], 0,
            "RGB rough SSS uses zero matched-index Fresnel at grazing cosine 1e-9" );
        CheckNear( matchedRGB[1], 0,
            "RGB rough SSS is achromatically zero at matched IOR" );
        CheckNear( matchedRGB[2], 0,
            "RGB rough SSS is zero in every channel at matched IOR" );
        CheckNear( matchedNM, 0,
            "NM rough SSS uses zero matched-index Fresnel at grazing cosine 1e-9" );

        Check( IsFinitePel( controlRGB ) && std::isfinite(controlNM),
            "unequal-IOR rough SSS control remains finite" );
        Check( controlRGB[0] > 0 && controlRGB[1] > 0 && controlRGB[2] > 0,
            "unequal-IOR RGB control has positive Fresnel BRDF" );
        Check( controlNM > 0,
            "unequal-IOR NM control has positive Fresnel BRDF" );

        matched->release();
        control->release();
        matchedIOR->release();
        controlIOR->release();
    }

    void TestRoughSPFReachability( const IObject* const object )
    {
        std::cout << "Rough SSS SPF VNDF reachability" << std::endl;
        // This is deliberately not a generic VNDF sample.  At alpha=.25,
        // u=(0, 0.99999996), and positive incident cosine 2e-10, the
        // production sampler makes wi.m about 5e-9: small enough that
        // `1-wi.m*wi.m` rounds to one in a Scalar calculation, but its
        // reflected wo still clears the production n.wo > 1e-10 gate.
        // Thus an unfixed local cosine Fresnel helper emits an erroneous
        // positive lobe; the corrected matched-IOR helper emits none.
        UniformScalarPainter* const matchedIOR = new UniformScalarPainter( 1 );
        UniformScalarPainter* const controlIOR = new UniformScalarPainter( kMatchedIOR );
        SubSurfaceScatteringSPF* const matched =
            new SubSurfaceScatteringSPF( *matchedIOR, 0, 0.5, false );
        SubSurfaceScatteringSPF* const control =
            new SubSurfaceScatteringSPF( *controlIOR, 0, 0.5, false );
        const RayIntersectionGeometric ri = MakeOutsideHit( kSPFIncidentCos );
        IORStack stack( 1 );
        stack.SetCurrentObject( object );

        const Scalar u1 = 0;
        const Scalar u2 = 0.99999996;
        const Scalar alpha = 0.25;
        const Vector3 wi = Vector3Ops::Normalize( -ri.ray.Dir() );
        const Vector3 microNormal = MicrofacetUtils::VNDF_Sample( wi, ri.onb, alpha, u1, u2 );
        const Scalar wiDotM = Vector3Ops::Dot( wi, microNormal );
        const Vector3 wo = Vector3Ops::Normalize( microNormal * (2*wiDotM) - wi );
        const Scalar nDotWo = Vector3Ops::Dot( ri.onb.w(), wo );
        Check( wiDotM > 0 && wiDotM < 1e-8,
            "actual VNDF micronormal reaches the positive false-TIR cosine band" );
        Check( Scalar(1) - wiDotM*wiDotM == Scalar(1),
            "actual VNDF micronormal squares away in the local Fresnel sine calculation" );
        Check( nDotWo > 1e-10,
            "same actual VNDF reflected direction clears the production geometric-horizon gate" );
        Check( MicrofacetUtils::VNDF_Pdf( wi, wo, ri.onb.w(), alpha ) > 0,
            "same actual VNDF direction has a positive production density" );

        for( unsigned int nm = 0; nm < 2; ++nm ) {
            OrderedSampler matchedSampler{ std::vector<Scalar>{ u1, u2 } };
            OrderedSampler controlSampler{ std::vector<Scalar>{ u1, u2 } };
            ScatteredRayContainer matchedRays;
            ScatteredRayContainer controlRays;
            if( nm ) {
                matched->ScatterNM( ri, matchedSampler, 550, matchedRays, stack );
                control->ScatterNM( ri, controlSampler, 550, controlRays, stack );
            } else {
                matched->Scatter( ri, matchedSampler, matchedRays, stack );
                control->Scatter( ri, controlSampler, controlRays, stack );
            }
            Check( matchedSampler.Draws() == 2 && matchedSampler.Overdraws() == 0,
                "rough matched SPF reads two ordered VNDF samples" );
            Check( controlSampler.Draws() == 2 && controlSampler.Overdraws() == 0,
                "rough control SPF reads two ordered VNDF samples" );
            const ScatteredRay* const mr = FindRay( matchedRays, ScatteredRay::eRayReflection );
            const ScatteredRay* const cr = FindRay( controlRays, ScatteredRay::eRayReflection );
            Check( !mr && matchedRays.Count() == 0,
                "matched-index rough SPF suppresses the zero-Fresnel lobe" );
            Check( cr && controlRays.Count() == 1,
                "unequal-IOR rough SPF control reaches the local Fresnel weight branch" );
            if( cr ) {
                Check( !cr->isDelta && cr->pdf > 0 && std::isfinite(cr->pdf),
                    "active rough control SPF sample is finite non-delta" );
                const Scalar controlWeight = nm ? cr->krayNM : cr->kray[0];
                Check( controlWeight > 0 && std::isfinite(controlWeight),
                    "active rough unequal-IOR SPF control has positive Fresnel weight" );
            }
        }

        matched->release();
        control->release();
        matchedIOR->release();
        controlIOR->release();
    }
}

int main()
{
    std::cout << "=== MatchedIndexGrazingConsumerTest (DL-58) ===" << std::endl;
    StubObject* const object = new StubObject;
    TestSmoothStandaloneExit( object );
    TestRoughBSDFMatchedFresnel();
    TestRoughSPFReachability( object );
    object->release();

    if( gFailures ) {
        std::cerr << "Checks: " << gChecks << "  Failures: " << gFailures << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "Checks: " << gChecks << "  Failures: 0" << std::endl;
    return EXIT_SUCCESS;
}
