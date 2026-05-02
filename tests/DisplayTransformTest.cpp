// DisplayTransformTest.cpp - Unit tests for the per-channel display
// transform tone curves added in Landing 1 of the PB pipeline plan.
//
// Each curve is a pure function from linear scalar radiance to a
// post-tone-curve linear value.  Tests verify:
//   - Identity / boundary behaviour at 0 and 1.
//   - Sanitisation: negatives, NaN, Inf collapse to 0.
//   - Monotonicity over a representative range.
//   - Output bounded in [0, 1] for non-identity curves.
//   - Specific reference values for the published formula fits.
//
// All tests are pure-CPU; no rasterisation, no scene loading.

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

#include "../src/Library/Rendering/DisplayTransform.h"

using namespace RISE;
using namespace RISE::DisplayTransforms;

static bool IsClose( double a, double b, double eps = 1e-6 )
{
    return std::fabs( a - b ) < eps;
}

// ---- Sanitisation: every curve must reject negatives / NaN / Inf ----

static void TestSanitiseUniform()
{
    std::cout << "TestSanitiseUniform..." << std::endl;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double pinf = std::numeric_limits<double>::infinity();
    const double ninf = -std::numeric_limits<double>::infinity();

    for( int dt_int = 0; dt_int <= 4; ++dt_int ) {
        const DISPLAY_TRANSFORM dt = static_cast<DISPLAY_TRANSFORM>(dt_int);
        // Negative input.
        {
            RISEPel out = Apply( dt, RISEPel( -1.0, -2.0, -10.0 ) );
            assert( IsClose( out.r, 0.0 ) );
            assert( IsClose( out.g, 0.0 ) );
            assert( IsClose( out.b, 0.0 ) );
        }
        // NaN input.
        {
            RISEPel out = Apply( dt, RISEPel( nan, nan, nan ) );
            assert( IsClose( out.r, 0.0 ) );
            assert( IsClose( out.g, 0.0 ) );
            assert( IsClose( out.b, 0.0 ) );
        }
        // +Inf input.
        {
            RISEPel out = Apply( dt, RISEPel( pinf, pinf, pinf ) );
            // Identity curve still has to reject Inf to avoid
            // poisoning the integerisation step downstream.
            // Reinhard, ACES, AgX, Hable should produce finite output.
            assert( std::isfinite( out.r ) );
            assert( std::isfinite( out.g ) );
            assert( std::isfinite( out.b ) );
        }
        // -Inf input.
        {
            RISEPel out = Apply( dt, RISEPel( ninf, ninf, ninf ) );
            assert( IsClose( out.r, 0.0 ) );
            assert( IsClose( out.g, 0.0 ) );
            assert( IsClose( out.b, 0.0 ) );
        }
    }
    std::cout << "  Passed!" << std::endl;
}

// ---- None: identity for non-pathological input ----

static void TestNoneIdentity()
{
    std::cout << "TestNoneIdentity..." << std::endl;
    const double samples[] = { 0.0, 0.18, 0.5, 1.0, 5.0, 100.0 };
    for( double v : samples ) {
        assert( IsClose( None( v ), v ) );
    }
    std::cout << "  Passed!" << std::endl;
}

// ---- Reinhard: f(x) = x / (1 + x) ----

static void TestReinhardFormula()
{
    std::cout << "TestReinhardFormula..." << std::endl;
    assert( IsClose( Reinhard( 0.0 ), 0.0 ) );
    assert( IsClose( Reinhard( 1.0 ), 0.5 ) );
    assert( IsClose( Reinhard( 3.0 ), 0.75 ) );

    // Asymptotic to 1.
    assert( Reinhard( 1e6 ) > 0.999999 );
    assert( Reinhard( 1e6 ) < 1.0 );

    // Monotonic.
    double prev = 0.0;
    for( int i = 0; i <= 1000; ++i ) {
        double x = i * 0.01;
        double y = Reinhard( x );
        assert( y >= prev - 1e-12 );
        prev = y;
    }
    std::cout << "  Passed!" << std::endl;
}

// ---- ACES (Narkowicz fit): published reference values ----

static void TestACESReference()
{
    std::cout << "TestACESReference..." << std::endl;
    // Reference values computed analytically from the Narkowicz formula:
    //   f(x) = saturate( x * (2.51*x + 0.03) / (x * (2.43*x + 0.59) + 0.14) )
    auto ref = []( double x ) -> double {
        double n = x * (2.51 * x + 0.03);
        double d = x * (2.43 * x + 0.59) + 0.14;
        double y = n / d;
        if( y < 0 ) y = 0;
        if( y > 1 ) y = 1;
        return y;
    };

    const double samples[] = { 0.0, 0.18, 0.5, 1.0, 2.0, 5.0, 10.0, 100.0 };
    for( double v : samples ) {
        const double got = ACES( v );
        const double want = ref( v );
        assert( IsClose( got, want, 1e-6 ) );
    }

    // Bounded in [0, 1].
    for( int i = 0; i <= 200; ++i ) {
        double x = i * 0.1;  // 0..20
        double y = ACES( x );
        assert( y >= 0.0 && y <= 1.0 );
    }
    std::cout << "  Passed!" << std::endl;
}

// ---- AgX scalar: midpoint check, monotonicity, bounded ----

static void TestAgXScalar()
{
    std::cout << "TestAgXScalar..." << std::endl;
    // Sigmoid in log10 space: 0.5 + 0.5 * tanh( log10(x + eps) ).
    // At x = 1: log10 ~ 0, tanh(0) = 0, output = 0.5.
    assert( IsClose( AgX( 1.0 ), 0.5, 1e-3 ) );

    // Monotonic increasing in input.
    double prev = -1.0;
    for( int i = 1; i <= 1000; ++i ) {
        double x = i * 0.01;
        double y = AgX( x );
        assert( y >= prev - 1e-12 );
        prev = y;
    }

    // Bounded in [0, 1] including extreme inputs.
    for( double x : { 0.0, 1e-6, 0.5, 1.0, 100.0, 1e6 } ) {
        double y = AgX( x );
        assert( y >= 0.0 && y <= 1.0 );
    }
    std::cout << "  Passed!" << std::endl;
}

// ---- Hable (Uncharted 2): published reference values ----

static void TestHableReference()
{
    std::cout << "TestHableReference..." << std::endl;
    // Reference: pre-scale by 2, normalise by curve(11.2).
    auto curve = []( double v ) -> double {
        const double A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
        return ( ( v * (A * v + C * B) + D * E ) /
                 ( v * (A * v + B    ) + D * F ) ) - E / F;
    };
    auto ref = [&]( double x ) -> double {
        double y = curve( x * 2.0 ) / curve( 11.2 );
        if( y < 0 ) y = 0;
        if( y > 1 ) y = 1;
        return y;
    };

    const double samples[] = { 0.0, 0.18, 0.5, 1.0, 5.0 };
    for( double v : samples ) {
        const double got = Hable( v );
        const double want = ref( v );
        assert( IsClose( got, want, 1e-6 ) );
    }

    // Hable( 0 ) should be 0 (numerator collapses to 0 after E/F subtract).
    assert( IsClose( Hable( 0.0 ), 0.0, 1e-6 ) );

    std::cout << "  Passed!" << std::endl;
}

// ---- RISEPel-level: per-channel application is independent ----

static void TestPerChannelIndependence()
{
    std::cout << "TestPerChannelIndependence..." << std::endl;
    RISEPel input( 0.5, 1.0, 2.0 );
    RISEPel out = Apply( eDisplayTransform_Reinhard, input );
    assert( IsClose( out.r, 0.5 / 1.5 ) );
    assert( IsClose( out.g, 1.0 / 2.0 ) );
    assert( IsClose( out.b, 2.0 / 3.0 ) );
    std::cout << "  Passed!" << std::endl;
}

int main()
{
    TestSanitiseUniform();
    TestNoneIdentity();
    TestReinhardFormula();
    TestACESReference();
    TestAgXScalar();
    TestHableReference();
    TestPerChannelIndependence();
    std::cout << "All DisplayTransform tests passed!" << std::endl;
    return 0;
}
