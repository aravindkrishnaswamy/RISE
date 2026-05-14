//////////////////////////////////////////////////////////////////////
//
//  IScalarPainterTest.cpp - Direct unit tests for every
//    IScalarPainter implementation.
//
//  Tests that each painter:
//    - Implements GetValuesAt and GetValueAtNM consistently.
//    - Reports HasPerChannelVariation correctly.
//    - Round-trips physical scalars without JH-uplift artifacts
//      (the bug this whole refactor exists to fix).
//
//  Reference-counting convention: every `Reference` subclass starts
//  with refcount == 1 at construction.  When a test owns a painter
//  and is finished with it, a single `release()` deletes it.  When a
//  painter passes ownership to a parent painter (e.g. Scaled, Multiply),
//  the parent's constructor addref's the child so the test must
//  release ITS reference after construction.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Painters/PiecewiseLinearScalarPainter.h"
#include "../src/Library/Painters/SellmeierScalarPainter.h"
#include "../src/Library/Painters/PolynomialScalarPainter.h"
#include "../src/Library/Painters/ScaledScalarPainter.h"
#include "../src/Library/Painters/MultiplyScalarPainter.h"
#include "../src/Library/Painters/Function1DScalarPainter.h"
#include "../src/Library/Painters/Function2DScalarPainter.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/IFunction1D.h"
#include "../src/Library/Interfaces/IFunction2D.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

static bool ApproxEq( Scalar a, Scalar b, Scalar tol = Scalar( 1e-9 ) )
{
	return std::fabs( a - b ) <= tol;
}

static RayIntersectionGeometric MakeDummyRig()
{
	return RayIntersectionGeometric( Ray(), nullRasterizerState );
}

static void TestUniformScalarPainter()
{
	std::cout << "TestUniformScalarPainter" << std::endl;
	UniformScalarPainter* p = new UniformScalarPainter( Scalar( 1.5 ) );

	const RayIntersectionGeometric ri = MakeDummyRig();

	// GetValuesAt returns triple of v.
	const ScalarTriple t = p->GetValuesAt( ri );
	Check( ApproxEq( t.v[0], 1.5 ), "uniform: v[0]=1.5" );
	Check( ApproxEq( t.v[1], 1.5 ), "uniform: v[1]=1.5" );
	Check( ApproxEq( t.v[2], 1.5 ), "uniform: v[2]=1.5" );
	Check( t.IsUniform(),           "uniform: IsUniform" );

	// GetValueAtNM returns the same value at every wavelength.
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 405 ) ), 1.5 ), "uniform: nm=405" );
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 555 ) ), 1.5 ), "uniform: nm=555" );
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 705 ) ), 1.5 ), "uniform: nm=705" );

	// HasPerChannelVariation is false.
	Check( ! p->HasPerChannelVariation(), "uniform: !HasPerChannelVariation" );

	// CRITICAL REGRESSION: physical-scalar values much larger than 1.0
	// must NOT be clamped or JH-uplifted.  This is the spectral-rasterizer
	// bug this refactor exists to fix — `scattering 1000000` must
	// round-trip as 1000000, not as a JH-uplifted ≈ 1.0 / 0.0.
	UniformScalarPainter* huge = new UniformScalarPainter( Scalar( 1e6 ) );
	Check( ApproxEq( huge->GetValueAtNM( ri, Scalar( 555 ) ), 1e6 ),
	       "uniform: huge scalar round-trips (regression guard)" );
	huge->release();

	p->release();
}

static void TestRGBScalarPainter()
{
	std::cout << "TestRGBScalarPainter" << std::endl;
	RGBScalarPainter* p = new RGBScalarPainter(
		Scalar( 1.3 ), Scalar( 1.5 ), Scalar( 2.0 ) );

	const RayIntersectionGeometric ri = MakeDummyRig();

	// GetValuesAt returns the authored triple.
	const ScalarTriple t = p->GetValuesAt( ri );
	Check( ApproxEq( t.v[0], 1.3 ), "rgb: v[0]=R=1.3" );
	Check( ApproxEq( t.v[1], 1.5 ), "rgb: v[1]=G=1.5" );
	Check( ApproxEq( t.v[2], 2.0 ), "rgb: v[2]=B=2.0" );
	Check( ! t.IsUniform(),         "rgb: !IsUniform" );

	// Wavelength endpoints clamp to channel endpoints.
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 380 ) ), 2.0 ), "rgb: nm=380 → B=2.0" );
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 720 ) ), 1.3 ), "rgb: nm=720 → R=1.3" );
	// Anchors at 450, 550, 650.
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 450 ) ), 2.0 ), "rgb: nm=450 → B=2.0" );
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 550 ) ), 1.5 ), "rgb: nm=550 → G=1.5" );
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 650 ) ), 1.3 ), "rgb: nm=650 → R=1.3" );
	// Midpoints interpolate.
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 500 ) ), Scalar( 0.5 ) * ( 2.0 + 1.5 ) ),
	       "rgb: nm=500 → midpoint B-G" );
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 600 ) ), Scalar( 0.5 ) * ( 1.5 + 1.3 ) ),
	       "rgb: nm=600 → midpoint G-R" );

	Check( p->HasPerChannelVariation(), "rgb: HasPerChannelVariation" );

	// A "uniform RGB" painter should report no channel variation.
	RGBScalarPainter* uni = new RGBScalarPainter( Scalar( 1.5 ), Scalar( 1.5 ), Scalar( 1.5 ) );
	Check( ! uni->HasPerChannelVariation(),
	       "rgb-uniform-disguise: !HasPerChannelVariation" );
	uni->release();

	p->release();
}

static void TestPiecewiseLinearScalarPainter()
{
	std::cout << "TestPiecewiseLinearScalarPainter" << std::endl;

	// colors/linear.ior shape: 380 → 1.10, 720 → 1.45.
	std::vector<PiecewiseLinearScalarPainter::Sample> samples = {
		{ Scalar( 380 ), Scalar( 1.10 ) },
		{ Scalar( 720 ), Scalar( 1.45 ) }
	};
	PiecewiseLinearScalarPainter* p = new PiecewiseLinearScalarPainter( samples );

	const RayIntersectionGeometric ri = MakeDummyRig();

	// Endpoints clamp.
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 300 ) ), 1.10 ),
	       "piecewise: clamp low → 1.10" );
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 800 ) ), 1.45 ),
	       "piecewise: clamp high → 1.45" );

	// Interior interpolation: 550 nm is (550-380)/(720-380) = 0.5 along
	// the segment → 1.10 + 0.5·(1.45-1.10) = 1.275.
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 550 ) ), 1.275, Scalar( 1e-6 ) ),
	       "piecewise: midpoint interpolation" );

	// Wavelength endpoints exact.
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 380 ) ), 1.10 ),
	       "piecewise: exact endpoint low" );
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 720 ) ), 1.45 ),
	       "piecewise: exact endpoint high" );

	Check( ! p->HasPerChannelVariation(), "piecewise: !HasPerChannelVariation" );

	p->release();

	// Empty-samples edge case: defensive fallback to 0 (consumers will
	// see "no contribution" rather than NaN / Inf).
	std::vector<PiecewiseLinearScalarPainter::Sample> empty;
	PiecewiseLinearScalarPainter* pe = new PiecewiseLinearScalarPainter( empty );
	Check( ApproxEq( pe->GetValueAtNM( ri, Scalar( 555 ) ), 0.0 ),
	       "piecewise: empty samples → 0" );
	pe->release();

	// Single-sample edge case: constant value regardless of nm.
	std::vector<PiecewiseLinearScalarPainter::Sample> single = {
		{ Scalar( 500 ), Scalar( 1.7 ) }
	};
	PiecewiseLinearScalarPainter* ps = new PiecewiseLinearScalarPainter( single );
	Check( ApproxEq( ps->GetValueAtNM( ri, Scalar( 400 ) ), 1.7 ),
	       "piecewise: single sample → 1.7 below" );
	Check( ApproxEq( ps->GetValueAtNM( ri, Scalar( 600 ) ), 1.7 ),
	       "piecewise: single sample → 1.7 above" );
	ps->release();

	// Duplicate-nm samples should not produce NaN.
	std::vector<PiecewiseLinearScalarPainter::Sample> dup = {
		{ Scalar( 500 ), Scalar( 1.5 ) },
		{ Scalar( 500 ), Scalar( 1.7 ) },	// duplicate nm
		{ Scalar( 600 ), Scalar( 2.0 ) }
	};
	PiecewiseLinearScalarPainter* pd = new PiecewiseLinearScalarPainter( dup );
	const Scalar dv = pd->GetValueAtNM( ri, Scalar( 500 ) );
	Check( std::isfinite( dv ),
	       "piecewise: duplicate-nm samples produce finite output" );
	pd->release();
}

static void TestSellmeierScalarPainter()
{
	std::cout << "TestSellmeierScalarPainter" << std::endl;

	// BK7 glass — published coefficients.
	SellmeierScalarPainter* p = new SellmeierScalarPainter(
		Scalar( 1.03961212 ), Scalar( 0.231792344 ), Scalar( 1.01046945 ),
		Scalar( 0.00600069867 ), Scalar( 0.0200179144 ), Scalar( 103.560653 )
		);

	const RayIntersectionGeometric ri = MakeDummyRig();

	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 587.6 ) ), Scalar( 1.5168 ), Scalar( 1e-3 ) ),
	       "sellmeier: BK7 d-line ≈ 1.5168" );
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 486.1 ) ), Scalar( 1.5224 ), Scalar( 1e-3 ) ),
	       "sellmeier: BK7 F-line ≈ 1.5224" );
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 656.3 ) ), Scalar( 1.5143 ), Scalar( 1e-3 ) ),
	       "sellmeier: BK7 C-line ≈ 1.5143" );

	Check( ! p->HasPerChannelVariation(), "sellmeier: !HasPerChannelVariation" );

	p->release();

	// Singularity-guard sanity check: place a coefficient C exactly at
	// the wavelength under test to provoke the (lam² - C) == 0 path.
	// Author's intent for the safe handling is "skip the term"; verify
	// the painter doesn't produce Inf/NaN.
	SellmeierScalarPainter* sing = new SellmeierScalarPainter(
		Scalar( 1.0 ), Scalar( 0.0 ), Scalar( 0.0 ),
		Scalar( 0.25 ),	// C1 = 0.25 µm² → singular at λ = 500 nm (0.5 µm)
		Scalar( 1.0 ), Scalar( 1.0 )
		);
	const Scalar ns = sing->GetValueAtNM( ri, Scalar( 500.0 ) );
	Check( std::isfinite( ns ),
	       "sellmeier: at-singularity λ produces finite output" );
	sing->release();
}

static void TestPolynomialScalarPainter()
{
	std::cout << "TestPolynomialScalarPainter" << std::endl;

	// f(λ) = 1.0 + 0.001·λ → linear.
	std::vector<Scalar> coeffs = { Scalar( 1.0 ), Scalar( 0.001 ) };
	PolynomialScalarPainter* p = new PolynomialScalarPainter( coeffs );

	const RayIntersectionGeometric ri = MakeDummyRig();
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 500 ) ), Scalar( 1.5 ) ),
	       "polynomial linear: f(500) = 1.5" );
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 0 ) ), Scalar( 1.0 ) ),
	       "polynomial linear: f(0) = 1.0" );

	p->release();

	// f(λ) = 2 + 3·λ + 4·λ² → quadratic.
	std::vector<Scalar> q = { Scalar( 2.0 ), Scalar( 3.0 ), Scalar( 4.0 ) };
	PolynomialScalarPainter* pq = new PolynomialScalarPainter( q );
	Check( ApproxEq( pq->GetValueAtNM( ri, Scalar( 2 ) ), Scalar( 2.0 + 6.0 + 16.0 ) ),
	       "polynomial quadratic: f(2) = 24" );

	pq->release();

	// Empty coeffs falls back to 1.0 (neutral, see header comment).
	std::vector<Scalar> empty;
	PolynomialScalarPainter* pe = new PolynomialScalarPainter( empty );
	Check( ApproxEq( pe->GetValueAtNM( ri, Scalar( 555 ) ), 1.0 ),
	       "polynomial: empty coeffs → 1.0 (air-IOR default)" );
	pe->release();
}

static void TestScaledScalarPainter()
{
	std::cout << "TestScaledScalarPainter" << std::endl;

	UniformScalarPainter* base = new UniformScalarPainter( Scalar( 2.0 ) );
	ScaledScalarPainter* scaled = new ScaledScalarPainter( base, Scalar( 0.5 ) );
	base->release();	// scaled holds its own reference now

	const RayIntersectionGeometric ri = MakeDummyRig();
	Check( ApproxEq( scaled->GetValueAtNM( ri, Scalar( 555 ) ), Scalar( 1.0 ) ),
	       "scaled: 2.0 * 0.5 = 1.0" );
	const ScalarTriple ts = scaled->GetValuesAt( ri );
	Check( ApproxEq( ts.v[0], 1.0 ) && ApproxEq( ts.v[1], 1.0 ) && ApproxEq( ts.v[2], 1.0 ),
	       "scaled: triple = (1.0, 1.0, 1.0)" );

	// Null-child defensive behavior.
	ScaledScalarPainter* nullChild = new ScaledScalarPainter( nullptr, Scalar( 2.0 ) );
	Check( ApproxEq( nullChild->GetValueAtNM( ri, 555 ), 0.0 ),
	       "scaled-null-child: GetValueAtNM = 0 (defensive)" );
	nullChild->release();

	scaled->release();
}

static void TestMultiplyScalarPainter()
{
	std::cout << "TestMultiplyScalarPainter" << std::endl;

	UniformScalarPainter* a = new UniformScalarPainter( Scalar( 3.0 ) );
	UniformScalarPainter* b = new UniformScalarPainter( Scalar( 4.0 ) );
	MultiplyScalarPainter* m = new MultiplyScalarPainter( a, b );
	a->release();
	b->release();

	const RayIntersectionGeometric ri = MakeDummyRig();
	const ScalarTriple t = m->GetValuesAt( ri );
	Check( ApproxEq( t.v[0], Scalar( 12.0 ) ), "multiply: 3 * 4 = 12 (v[0])" );
	Check( ApproxEq( m->GetValueAtNM( ri, Scalar( 555 ) ), Scalar( 12.0 ) ),
	       "multiply: 3 * 4 = 12 (nm)" );

	// Channel-variation propagation: multiply uniform × RGB → variation.
	UniformScalarPainter* u = new UniformScalarPainter( Scalar( 2.0 ) );
	RGBScalarPainter*     r = new RGBScalarPainter( Scalar( 1.3 ), Scalar( 1.5 ), Scalar( 2.0 ) );
	MultiplyScalarPainter* m2 = new MultiplyScalarPainter( u, r );
	u->release();
	r->release();
	Check( m2->HasPerChannelVariation(),
	       "multiply: variation propagates from child" );
	const ScalarTriple t2 = m2->GetValuesAt( ri );
	Check( ApproxEq( t2.v[0], 2.6 ) && ApproxEq( t2.v[1], 3.0 ) && ApproxEq( t2.v[2], 4.0 ),
	       "multiply: per-channel product (2.6, 3.0, 4.0)" );
	m2->release();

	// Wavelength composition: Sellmeier × Uniform → varies with λ.
	SellmeierScalarPainter* bk7 = new SellmeierScalarPainter(
		Scalar( 1.03961212 ), Scalar( 0.231792344 ), Scalar( 1.01046945 ),
		Scalar( 0.00600069867 ), Scalar( 0.0200179144 ), Scalar( 103.560653 )
		);
	UniformScalarPainter* half = new UniformScalarPainter( Scalar( 0.5 ) );
	MultiplyScalarPainter* m3 = new MultiplyScalarPainter( bk7, half );
	bk7->release();
	half->release();
	// At λ = 587.6: 1.5168 * 0.5 = 0.7584.
	Check( ApproxEq( m3->GetValueAtNM( ri, Scalar( 587.6 ) ),
	                  Scalar( 0.5 ) * Scalar( 1.5168 ), Scalar( 1e-3 ) ),
	       "multiply (sellmeier × uniform): per-wavelength product" );
	m3->release();

	m->release();
}

// Minimal IFunction1D for the Function1D wrapper test.  Returns
// `lambda * 2`, lets the test verify (a) the wrapper queries the
// function at the right argument and (b) the function's value
// propagates correctly.
class TestFunction1D : public virtual IFunction1D, public virtual Reference
{
public:
	Scalar Evaluate( const Scalar v ) const override { return v * Scalar( 2 ); }
protected:
	virtual ~TestFunction1D() {}
};

class TestFunction2D : public virtual IFunction2D, public virtual Reference
{
public:
	Scalar Evaluate( const Scalar x, const Scalar y ) const override
	{
		return x + y * Scalar( 10 );
	}
protected:
	virtual ~TestFunction2D() {}
};

static void TestFunction1DScalarPainter()
{
	std::cout << "TestFunction1DScalarPainter" << std::endl;
	TestFunction1D* f = new TestFunction1D();
	Function1DScalarPainter* p = new Function1DScalarPainter( f );
	f->release();

	const RayIntersectionGeometric ri = MakeDummyRig();
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 100 ) ), Scalar( 200 ) ),
	       "function1d-wrap: f(100) = 200" );
	Check( ApproxEq( p->GetValueAtNM( ri, Scalar( 555 ) ), Scalar( 1110 ) ),
	       "function1d-wrap: f(555) = 1110" );

	// Null-function defensive.
	Function1DScalarPainter* pn = new Function1DScalarPainter( nullptr );
	Check( ApproxEq( pn->GetValueAtNM( ri, Scalar( 555 ) ), 0.0 ),
	       "function1d-null-func: GetValueAtNM = 0 (defensive)" );
	pn->release();

	p->release();
}

static void TestFunction2DScalarPainter()
{
	std::cout << "TestFunction2DScalarPainter" << std::endl;
	TestFunction2D* f = new TestFunction2D();
	Function2DScalarPainter* p = new Function2DScalarPainter( f );
	f->release();

	RayIntersectionGeometric ri = MakeDummyRig();
	ri.ptCoord = Point2( Scalar( 3 ), Scalar( 7 ) );
	const ScalarTriple t = p->GetValuesAt( ri );
	// 3 + 7*10 = 73.
	Check( ApproxEq( t.v[0], Scalar( 73 ) ),
	       "function2d-wrap: f(3, 7) = 73" );

	p->release();
}

static void TestScalarTripleUniformity()
{
	std::cout << "TestScalarTripleUniformity" << std::endl;
	Check(   ScalarTriple( Scalar( 1.5 ) ).IsUniform(),
	         "triple: explicit-uniform IsUniform" );
	Check(   ScalarTriple( Scalar( 1.5 ), Scalar( 1.5 ), Scalar( 1.5 ) ).IsUniform(),
	         "triple: per-channel-equal IsUniform" );
	Check( ! ScalarTriple( Scalar( 1.3 ), Scalar( 1.5 ), Scalar( 2.0 ) ).IsUniform(),
	         "triple: per-channel-differ !IsUniform" );
}

int main()
{
	std::cout << "IScalarPainter unit tests" << std::endl;
	TestScalarTripleUniformity();
	TestUniformScalarPainter();
	TestRGBScalarPainter();
	TestPiecewiseLinearScalarPainter();
	TestSellmeierScalarPainter();
	TestPolynomialScalarPainter();
	TestScaledScalarPainter();
	TestMultiplyScalarPainter();
	TestFunction1DScalarPainter();
	TestFunction2DScalarPainter();

	std::cout << "\nResults: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
