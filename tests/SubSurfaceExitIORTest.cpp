//////////////////////////////////////////////////////////////////////
//
//  SubSurfaceExitIORTest.cpp - Regression guard for DL-51's standalone
//  SubSurfaceScatteringSPF exit destination IOR.
//
//  The non-absorbing standalone SPF permits an inside hit.  At that
//  boundary, Snell and Fresnel must use the enclosing medium exposed by
//  popping the current object, while the transmitted ray publishes that
//  same popped stack.  This test keeps a nested 1.0 -> 1.33 -> 1.5 stack
//  and derives its directions and weights from independent scalar Snell
//  and Fresnel formulae (it deliberately does not call Optics for an
//  expectation).
//
//  The shipped diffusion and random-walk materials intentionally build
//  their SPFs with bAbsorbBackFace=true.  Their controls below exercise
//  GetSPF() at the same inside hit so DL-51 does not accidentally claim
//  to alter those supported material topologies.
//
//  Author: Aravind Krishnaswamy (RISE DL-51)
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Materials/RandomWalkSSSMaterial.h"
#include "../src/Library/Materials/SubSurfaceScatteringMaterial.h"
#include "../src/Library/Materials/SubSurfaceScatteringSPF.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/ISampler.h"
#include "../src/Library/Utilities/Ray.h"

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
    int gFailures = 0;
    const Scalar kAmbientIOR = 1.0;
    const Scalar kEnclosingIOR = 1.33;
    const Scalar kInteriorIOR = 1.50;
    const Scalar kTol = 2e-6;

    void Check( const bool condition, const char* const message )
    {
        if( !condition ) {
            ++gFailures;
            std::cerr << "FAIL: " << message << std::endl;
        }
    }

    void CheckNear( const Scalar actual, const Scalar expected,
        const char* const message, const Scalar tolerance = kTol )
    {
        if( std::fabs( actual - expected ) > tolerance ) {
            ++gFailures;
            std::cerr << "FAIL: " << message << " (got " << actual
                << ", expected " << expected << ")" << std::endl;
        }
    }

    void CheckVector( const Vector3& actual, const Vector3& expected,
        const char* const message )
    {
        const bool matches = std::fabs( actual.x - expected.x ) <= kTol &&
            std::fabs( actual.y - expected.y ) <= kTol &&
            std::fabs( actual.z - expected.z ) <= kTol;
        Check( matches, message );
    }

    // Smooth inside paths are delta paths and must not consume a draw.  The
    // storage is explicit so an unexpected draw fails deterministically.
    class VectorSampler : public ISampler
    {
    public:
        explicit VectorSampler( const std::vector<Scalar>& samples ) :
            m_samples( samples ), m_next( 0 ) {}

        Scalar Get1D() override
        {
            if( m_next >= m_samples.size() ) {
                ++m_overdraws;
                return 0.0;
            }
            return m_samples[m_next++];
        }

        Point2 Get2D() override { return Point2( Get1D(), Get1D() ); }
        void StartStream( int ) override {}

        size_t DrawCount() const { return m_next; }
        size_t OverdrawCount() const { return m_overdraws; }

    private:
        std::vector<Scalar> m_samples;
        size_t m_next;
        size_t m_overdraws = 0;
    };

    RayIntersectionGeometric MakeInsideIntersection( const Scalar sinI )
    {
        const Scalar cosI = std::sqrt( 1.0 - sinI * sinI );
        const Ray ray( Point3( 0, 0, -1 ), Vector3( sinI, 0, cosI ) );
        RayIntersectionGeometric ri( ray, nullRasterizerState );
        ri.bHit = true;
        ri.range = 1.0;
        ri.ptIntersection = Point3( 0, 0, 0 );
        ri.vNormal = Vector3( 0, 0, 1 );
        ri.vGeomNormal = Vector3( 0, 0, 1 );
        ri.onb.CreateFromW( ri.vNormal );
        ri.ptCoord = Point2( 0.5, 0.5 );
        return ri;
    }

    // The stack has actual distinct object identities: a nested outer medium
    // and the current SSS object.  Root is the ambient (1.0) stack entry.
    IORStack MakeNestedInsideStack( const IObject* const outer,
        const IObject* const current )
    {
        IORStack stack( kAmbientIOR );
        stack.SetCurrentObject( outer );
        stack.push( kEnclosingIOR );
        stack.SetCurrentObject( current );
        stack.push( kInteriorIOR );
        return stack;
    }

    IORStack MakeAirInsideStack( const IObject* const current )
    {
        IORStack stack( kAmbientIOR );
        stack.SetCurrentObject( current );
        stack.push( kInteriorIOR );
        return stack;
    }

    Scalar Fresnel( const Scalar nI, const Scalar nT, const Scalar sinI,
        const Scalar cosI )
    {
        const Scalar sinT = nI / nT * sinI;
        if( sinT >= 1.0 ) return 1.0;
        const Scalar cosT = std::sqrt( 1.0 - sinT * sinT );
        const Scalar rs = ( nI * cosI - nT * cosT ) / ( nI * cosI + nT * cosT );
        const Scalar rp = ( nT * cosI - nI * cosT ) / ( nT * cosI + nI * cosT );
        return ( rs * rs + rp * rp ) * 0.5;
    }

    const ScatteredRay* FindRay( const ScatteredRayContainer& rays,
        const ScatteredRay::ScatRayType type )
    {
        for( unsigned int i = 0; i < rays.Count(); ++i ) {
            if( rays[i].type == type ) return &rays[i];
        }
        return 0;
    }

    void CheckInputUnchanged( const IORStack& input, const IObject* const current,
        const char* const label )
    {
        Check( input.containsCurrent(), label );
        Check( input.topObject() == current, "input stack top object remains current SSS object" );
        CheckNear( input.top(), kInteriorIOR, "input stack top IOR remains interior" );
    }

    void CheckNestedStackOutputs( const ScatteredRay& reflection,
        const ScatteredRay& transmission, const IObject* const outer,
        const IObject* const current )
    {
        Check( reflection.ior_stack != 0, "reflection carries an IOR stack" );
        Check( transmission.ior_stack != 0, "transmission carries an IOR stack" );
        if( reflection.ior_stack ) {
            Check( reflection.delete_stack, "stored reflection owns its stack" );
            Check( reflection.ior_stack->containsCurrent(), "reflection remains inside current object" );
            Check( reflection.ior_stack->topObject() == current,
                "reflection stack top remains current SSS object" );
            CheckNear( reflection.ior_stack->top(), kInteriorIOR,
                "reflection stack top IOR remains interior" );
        }
        if( transmission.ior_stack ) {
            Check( transmission.delete_stack, "stored transmission owns its stack" );
            Check( !transmission.ior_stack->containsCurrent(),
                "transmission stack no longer contains current object" );
            Check( transmission.ior_stack->topObject() == outer,
                "transmission stack reveals enclosing object" );
            CheckNear( transmission.ior_stack->top(), kEnclosingIOR,
                "transmission stack reveals enclosing IOR" );
        }
    }

    void CheckDeltaMetadata( const ScatteredRay& ray, const char* const label )
    {
        Check( ray.isDelta, label );
        CheckNear( ray.pdf, 1.0, "inside delta ray has unit discrete PDF" );
    }

    void TestStandaloneRGB( const IObject* const outer, const IObject* const current )
    {
        UniformScalarPainter* const ior = new UniformScalarPainter( kInteriorIOR );
        SubSurfaceScatteringSPF* const spf = new SubSurfaceScatteringSPF( *ior, 0.0, 0.0, false );

        // Normal incidence exposes the Fresnel destination without a direction
        // difference.  The old pre-pop Nt=1.5 path emitted R=0 here.
        {
            const RayIntersectionGeometric ri = MakeInsideIntersection( 0.0 );
            IORStack input = MakeNestedInsideStack( outer, current );
            const std::vector<Scalar> noDraws;
            VectorSampler sampler( noDraws );
            ScatteredRayContainer rays;
            spf->Scatter( ri, sampler, rays, input );

            const ScatteredRay* const reflection = FindRay( rays, ScatteredRay::eRayReflection );
            const ScatteredRay* const transmission = FindRay( rays, ScatteredRay::eRayRefraction );
            Check( rays.Count() == 2, "RGB normal exit emits reflection and transmission" );
            Check( reflection != 0 && transmission != 0, "RGB normal exit has both active lobe types" );
            Check( sampler.DrawCount() == 0 && sampler.OverdrawCount() == 0,
                "RGB smooth inside delta path consumes no sampler draws" );
            CheckInputUnchanged( input, current, "RGB normal input still classifies as inside" );
            if( reflection && transmission ) {
                const Scalar r = Fresnel( kInteriorIOR, kEnclosingIOR, 0.0, 1.0 );
                CheckDeltaMetadata( *reflection, "RGB normal reflection is delta" );
                CheckDeltaMetadata( *transmission, "RGB normal transmission is delta" );
                CheckNear( reflection->kray[0], r, "RGB normal reflected R uses popped destination IOR" );
                CheckNear( reflection->kray[1], r, "RGB normal reflected G uses popped destination IOR" );
                CheckNear( reflection->kray[2], r, "RGB normal reflected B uses popped destination IOR" );
                CheckNear( transmission->kray[0], 1.0-r, "RGB normal transmitted R uses Fresnel complement" );
                CheckNear( transmission->kray[1], 1.0-r, "RGB normal transmitted G uses Fresnel complement" );
                CheckNear( transmission->kray[2], 1.0-r, "RGB normal transmitted B uses Fresnel complement" );
                CheckNear( reflection->kray[0] + transmission->kray[0], 1.0,
                    "RGB normal Fresnel lobes conserve surface weight" );
                CheckVector( reflection->ray.Dir(), Vector3( 0, 0, -1 ),
                    "RGB normal reflection points back into the interior" );
                CheckVector( transmission->ray.Dir(), Vector3( 0, 0, 1 ),
                    "RGB normal transmission points into the enclosing medium" );
                CheckNestedStackOutputs( *reflection, *transmission, outer, current );
            }
        }

        // Oblique incidence is the direct Snell guard: pre-pop Nt=n_s leaves
        // this ray unbent even though its published stack has n=1.33.
        {
            const Scalar sinI = 0.60;
            const Scalar cosI = std::sqrt( 1.0 - sinI*sinI );
            const RayIntersectionGeometric ri = MakeInsideIntersection( sinI );
            IORStack input = MakeNestedInsideStack( outer, current );
            const std::vector<Scalar> noDraws;
            VectorSampler sampler( noDraws );
            ScatteredRayContainer rays;
            spf->Scatter( ri, sampler, rays, input );
            const ScatteredRay* const reflection = FindRay( rays, ScatteredRay::eRayReflection );
            const ScatteredRay* const transmission = FindRay( rays, ScatteredRay::eRayRefraction );
            Check( reflection != 0 && transmission != 0, "RGB oblique exit has reflection and transmission" );
            Check( sampler.DrawCount() == 0 && sampler.OverdrawCount() == 0,
                "RGB oblique smooth delta path consumes no sampler draws" );
            CheckInputUnchanged( input, current, "RGB oblique input remains unchanged" );
            if( reflection && transmission ) {
                const Scalar sinT = kInteriorIOR / kEnclosingIOR * sinI;
                const Scalar r = Fresnel( kInteriorIOR, kEnclosingIOR, sinI, cosI );
                CheckDeltaMetadata( *reflection, "RGB oblique reflection is delta" );
                CheckDeltaMetadata( *transmission, "RGB oblique transmission is delta" );
                CheckVector( reflection->ray.Dir(), Vector3( sinI, 0, -cosI ),
                    "RGB oblique reflected direction is analytic mirror direction" );
                CheckVector( transmission->ray.Dir(), Vector3( sinT, 0, std::sqrt( 1.0-sinT*sinT ) ),
                    "RGB oblique transmitted direction obeys Snell to enclosing IOR" );
                CheckNear( reflection->kray[0], r, "RGB oblique Fresnel uses interior-to-enclosing indices" );
                CheckNear( transmission->kray[0], 1.0-r, "RGB oblique transmission is Fresnel complement" );
                CheckNear( reflection->kray[0] + transmission->kray[0], 1.0,
                    "RGB oblique Fresnel lobes conserve surface weight" );
                CheckNestedStackOutputs( *reflection, *transmission, outer, current );
            }
        }

        // Above asin(1.33/1.5), the enclosing-medium exit is total internal
        // reflection.  Nt=n_s would incorrectly emit an unbent transmission.
        {
            const Scalar sinI = std::sin( Scalar(70.0) * std::acos( Scalar(-1.0) ) / Scalar(180.0) );
            const Scalar cosI = std::sqrt( 1.0 - sinI*sinI );
            const RayIntersectionGeometric ri = MakeInsideIntersection( sinI );
            IORStack input = MakeNestedInsideStack( outer, current );
            const std::vector<Scalar> noDraws;
            VectorSampler sampler( noDraws );
            ScatteredRayContainer rays;
            spf->Scatter( ri, sampler, rays, input );
            const ScatteredRay* const reflection = FindRay( rays, ScatteredRay::eRayReflection );
            const ScatteredRay* const transmission = FindRay( rays, ScatteredRay::eRayRefraction );
            Check( kInteriorIOR / kEnclosingIOR * sinI > 1.0,
                "RGB TIR fixture is above the interior-to-enclosing critical angle" );
            Check( rays.Count() == 1 && reflection != 0 && transmission == 0,
                "RGB TIR emits only the reflected delta lobe" );
            Check( sampler.DrawCount() == 0 && sampler.OverdrawCount() == 0,
                "RGB TIR delta path consumes no sampler draws" );
            CheckInputUnchanged( input, current, "RGB TIR input remains unchanged" );
            if( reflection ) {
                CheckDeltaMetadata( *reflection, "RGB TIR reflection is delta" );
                CheckNear( reflection->kray[0], 1.0, "RGB TIR reflected R has unit Fresnel weight" );
                CheckNear( reflection->kray[1], 1.0, "RGB TIR reflected G has unit Fresnel weight" );
                CheckNear( reflection->kray[2], 1.0, "RGB TIR reflected B has unit Fresnel weight" );
                CheckVector( reflection->ray.Dir(), Vector3( sinI, 0, -cosI ),
                    "RGB TIR reflection points back into the interior" );
                Check( reflection->ior_stack != 0 && reflection->ior_stack->containsCurrent(),
                    "RGB TIR reflection retains the interior stack" );
            }
        }

        spf->release();
        ior->release();
    }

    void TestStandaloneNM( const IObject* const outer, const IObject* const current )
    {
        // RGBScalarPainter maps 650/550/450 nm to R/G/B respectively.  The
        // three calls therefore cover wavelength-dependent n_s without using
        // any production optics routine as an oracle.
        RGBScalarPainter* const ior = new RGBScalarPainter( 1.48, 1.50, 1.52 );
        SubSurfaceScatteringSPF* const spf = new SubSurfaceScatteringSPF( *ior, 0.0, 0.0, false );
        const Scalar wavelengths[] = { 450.0, 550.0, 650.0 };
        const Scalar interiorIndices[] = { 1.52, 1.50, 1.48 };
        const Scalar sinI = 0.60;
        const Scalar cosI = std::sqrt( 1.0 - sinI*sinI );

        for( unsigned int i = 0; i < 3; ++i ) {
            const RayIntersectionGeometric ri = MakeInsideIntersection( sinI );
            IORStack input = MakeNestedInsideStack( outer, current );
            const std::vector<Scalar> noDraws;
            VectorSampler sampler( noDraws );
            ScatteredRayContainer rays;
            spf->ScatterNM( ri, sampler, wavelengths[i], rays, input );

            const ScatteredRay* const reflection = FindRay( rays, ScatteredRay::eRayReflection );
            const ScatteredRay* const transmission = FindRay( rays, ScatteredRay::eRayRefraction );
            Check( reflection != 0 && transmission != 0, "NM oblique exit has reflection and transmission" );
            Check( sampler.DrawCount() == 0 && sampler.OverdrawCount() == 0,
                "NM smooth inside delta path consumes no sampler draws" );
            CheckInputUnchanged( input, current, "NM input remains unchanged" );
            if( reflection && transmission ) {
                const Scalar nI = interiorIndices[i];
                const Scalar sinT = nI / kEnclosingIOR * sinI;
                const Scalar r = Fresnel( nI, kEnclosingIOR, sinI, cosI );
                CheckDeltaMetadata( *reflection, "NM reflection is delta" );
                CheckDeltaMetadata( *transmission, "NM transmission is delta" );
                CheckNear( reflection->krayNM, r, "NM Fresnel uses wavelength interior-to-enclosing indices" );
                CheckNear( transmission->krayNM, 1.0-r, "NM transmission is wavelength Fresnel complement" );
                CheckNear( reflection->krayNM + transmission->krayNM, 1.0,
                    "NM Fresnel lobes conserve surface weight" );
                CheckVector( transmission->ray.Dir(),
                    Vector3( sinT, 0, std::sqrt( 1.0-sinT*sinT ) ),
                    "NM transmitted direction obeys wavelength Snell law" );
                CheckNestedStackOutputs( *reflection, *transmission, outer, current );
            }
        }

        spf->release();
        ior->release();
    }

    // The enclosing-medium fixture is the bug topology.  This compact air
    // control also pins the root-entry pop, whose topObject() is null.
    void TestStandaloneAirExitControl( const IObject* const current )
    {
        UniformScalarPainter* const ior = new UniformScalarPainter( kInteriorIOR );
        SubSurfaceScatteringSPF* const spf = new SubSurfaceScatteringSPF( *ior, 0.0, 0.0, false );
        const RayIntersectionGeometric ri = MakeInsideIntersection( 0.0 );
        IORStack input = MakeAirInsideStack( current );
        const std::vector<Scalar> noDraws;
        VectorSampler sampler( noDraws );
        ScatteredRayContainer rays;
        spf->Scatter( ri, sampler, rays, input );

        const ScatteredRay* const reflection = FindRay( rays, ScatteredRay::eRayReflection );
        const ScatteredRay* const transmission = FindRay( rays, ScatteredRay::eRayRefraction );
        Check( reflection != 0 && transmission != 0, "air exit control has reflection and transmission" );
        Check( sampler.DrawCount() == 0 && sampler.OverdrawCount() == 0,
            "air exit smooth delta path consumes no sampler draws" );
        CheckInputUnchanged( input, current, "air exit input remains inside current object" );
        if( reflection && transmission ) {
            const Scalar r = Fresnel( kInteriorIOR, kAmbientIOR, 0.0, 1.0 );
            CheckDeltaMetadata( *reflection, "air exit reflection is delta" );
            CheckDeltaMetadata( *transmission, "air exit transmission is delta" );
            CheckNear( reflection->kray[0], r, "air exit Fresnel uses ambient root IOR" );
            CheckNear( transmission->kray[0], 1.0-r, "air exit transmission is Fresnel complement" );
            CheckNear( reflection->kray[0] + transmission->kray[0], 1.0,
                "air exit Fresnel lobes conserve surface weight" );
            CheckVector( transmission->ray.Dir(), Vector3( 0, 0, 1 ),
                "air exit transmission points into the ambient" );
            Check( reflection->ior_stack != 0 && reflection->ior_stack->containsCurrent(),
                "air exit reflection retains the interior stack" );
            Check( transmission->ior_stack != 0 && !transmission->ior_stack->containsCurrent(),
                "air exit transmission removes the current object" );
            if( transmission->ior_stack ) {
                Check( transmission->ior_stack->topObject() == 0,
                    "air exit transmission exposes the root stack object" );
                CheckNear( transmission->ior_stack->top(), kAmbientIOR,
                    "air exit transmission exposes ambient IOR" );
            }
        }

        spf->release();
        ior->release();
    }

    void CheckAbsorbsInsideHit( ISPF* const spf, const IObject* const outer,
        const IObject* const current, const char* const label )
    {
        const RayIntersectionGeometric ri = MakeInsideIntersection( 0.60 );
        for( unsigned int spectral = 0; spectral < 2; ++spectral ) {
            IORStack input = MakeNestedInsideStack( outer, current );
            const std::vector<Scalar> noDraws;
            VectorSampler sampler( noDraws );
            ScatteredRayContainer rays;
            if( spectral ) spf->ScatterNM( ri, sampler, 550.0, rays, input );
            else spf->Scatter( ri, sampler, rays, input );
            Check( rays.Count() == 0, label );
            Check( sampler.DrawCount() == 0 && sampler.OverdrawCount() == 0,
                "absorbing back-face control consumes no sampler draws" );
            CheckInputUnchanged( input, current, "absorbing back-face control preserves caller stack" );
        }
    }

    void TestAbsorbingControls( const IObject* const outer, const IObject* const current )
    {
        UniformScalarPainter* const ior = new UniformScalarPainter( kInteriorIOR );
        UniformScalarPainter* const absorption = new UniformScalarPainter( 0.1 );
        UniformScalarPainter* const scattering = new UniformScalarPainter( 1.0 );

        SubSurfaceScatteringSPF* const standaloneAbsorbing =
            new SubSurfaceScatteringSPF( *ior, 0.0, 0.0, true );
        CheckAbsorbsInsideHit( standaloneAbsorbing, outer, current,
            "standalone bAbsorbBackFace=true absorbs RGB/NM inside hit" );
        standaloneAbsorbing->release();

        SubSurfaceScatteringMaterial* const diffusion = new SubSurfaceScatteringMaterial(
            *ior, *absorption, *scattering, 0.0, 0.0 );
        Check( diffusion->GetSPF() != 0, "diffusion material supplies a real SPF" );
        if( diffusion->GetSPF() ) CheckAbsorbsInsideHit( diffusion->GetSPF(), outer, current,
            "shipped diffusion material GetSPF absorbs RGB/NM inside hit" );
        diffusion->release();

        RandomWalkSSSMaterial* const randomWalk = new RandomWalkSSSMaterial(
            *ior, *absorption, *scattering, 0.0, 0.0, 4 );
        Check( randomWalk->GetSPF() != 0, "random-walk material supplies a real SPF" );
        if( randomWalk->GetSPF() ) CheckAbsorbsInsideHit( randomWalk->GetSPF(), outer, current,
            "shipped random-walk material GetSPF absorbs RGB/NM inside hit" );
        randomWalk->release();

        ior->release();
        absorption->release();
        scattering->release();
    }
}

int main()
{
    std::cout << "=== SubSurfaceExitIORTest (DL-51) ===" << std::endl;

    // IORStack stores these only as identity keys.  Each starts at refcount 1;
    // no addref is needed before the final owning release below.
    StubObject* const outer = new StubObject;
    StubObject* const current = new StubObject;

    TestStandaloneRGB( outer, current );
    TestStandaloneNM( outer, current );
    TestStandaloneAirExitControl( current );
    TestAbsorbingControls( outer, current );

    outer->release();
    current->release();

    if( gFailures ) {
        std::cerr << gFailures << " SubSurfaceExitIORTest check(s) failed" << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "All SubSurfaceExitIORTest checks passed" << std::endl;
    return EXIT_SUCCESS;
}
