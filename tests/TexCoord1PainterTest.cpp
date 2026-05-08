//////////////////////////////////////////////////////////////////////
//
//  TexCoord1PainterTest.cpp - Focused wrapper test for the glTF
//  TEXCOORD_1 selector painter (Landing 12.D).
//
//  Verifies all four IPainter accessors forward correctly through a
//  UV1 swap:
//    GetColor      — RGB path (PT)
//    GetColorNM    — single-wavelength path (spectral integrators)
//    GetSpectrum   — full-spectrum path (spectral integrators / emitters)
//    GetAlpha      — alphaMode = MASK / BLEND
//
//  Without this test, a future copy-paste bug or a missed override
//  (the GetSpectrum case happened in round 4 of adversarial review)
//  silently passes through the base Painter dummy implementation.
//
//  Approach: an echo painter records which accessor was called and
//  with what ri.ptCoord (and ri.txFootprint.valid).  Wrapping with
//  TexCoord1Painter and asserting that the swapped UV / invalidated
//  footprint reach the source covers:
//    - bHasTexCoord1 == true : all four accessors swap ptCoord ←
//      ptCoord1 AND invalidate txFootprint.valid
//    - bHasTexCoord1 == false: all four accessors pass ri through
//      unchanged (no swap, no footprint touch — for graceful
//      degradation on analytic primitives or non-UV1 meshes)
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cmath>
#include <iostream>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Painters/Painter.h"	// Painter base for the EchoPainter helper
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/IPainter.h"

using namespace RISE;
using namespace RISE::Implementation;

static int s_pass = 0;
static int s_fail = 0;

static bool Close( double a, double b, double eps = 1e-9 )
{
	return std::fabs( a - b ) <= eps;
}

static void Check( bool ok, const char* what )
{
	if( ok ) {
		++s_pass;
		std::cout << "  PASS  " << what << "\n";
	} else {
		++s_fail;
		std::cout << "  FAIL  " << what << "\n";
	}
}

//
// EchoPainter records the ri seen at each accessor call so the test can
// assert "the wrapper forwarded this exact UV / footprint state".  We
// keep the recording mutable through `mutable` because the IPainter
// accessors are const — same idiom the codebase uses for stat counters.
//
class EchoPainter : public Painter
{
public:
	mutable bool   sawGetColor;
	mutable bool   sawGetColorNM;
	mutable bool   sawGetSpectrum;
	mutable bool   sawGetAlpha;
	mutable Point2 lastPtCoord;
	mutable bool   lastFootprintValid;

	EchoPainter() :
		sawGetColor( false ),
		sawGetColorNM( false ),
		sawGetSpectrum( false ),
		sawGetAlpha( false ),
		lastPtCoord( 0, 0 ),
		lastFootprintValid( false )
	{}
	~EchoPainter() {}

	void Reset() const {
		sawGetColor = sawGetColorNM = sawGetSpectrum = sawGetAlpha = false;
		lastPtCoord = Point2( 0, 0 );
		lastFootprintValid = false;
	}

	RISEPel GetColor( const RayIntersectionGeometric& ri ) const {
		sawGetColor = true;
		lastPtCoord = ri.ptCoord;
		lastFootprintValid = ri.txFootprint.valid;
		return RISEPel( ri.ptCoord.x, ri.ptCoord.y, 0.0 );
	}

	Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar /*nm*/ ) const {
		sawGetColorNM = true;
		lastPtCoord = ri.ptCoord;
		lastFootprintValid = ri.txFootprint.valid;
		return ri.ptCoord.x;
	}

	SpectralPacket GetSpectrum( const RayIntersectionGeometric& ri ) const {
		sawGetSpectrum = true;
		lastPtCoord = ri.ptCoord;
		lastFootprintValid = ri.txFootprint.valid;
		// Non-default packet so the wrapper's forwarding-vs-dummy
		// behaviour is observable.  Width = 1 per the codebase's
		// SpectralPacket sampling convention; range covers visible.
		SpectralPacket sp( 400, 700, 1 );
		return sp;
	}

	Scalar GetAlpha( const RayIntersectionGeometric& ri ) const {
		sawGetAlpha = true;
		lastPtCoord = ri.ptCoord;
		lastFootprintValid = ri.txFootprint.valid;
		return ri.ptCoord.x;
	}

	IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
	void SetIntermediateValue( const IKeyframeParameter& ) {}
	void RegenerateData() {}
};

static RayIntersectionGeometric MakeRi( double u0, double v0,
                                        double u1, double v1,
                                        bool hasUV1,
                                        bool footprintValid )
{
	const Ray r( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
	const RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( r, rs );
	ri.bHit                 = true;
	ri.ptCoord              = Point2( u0, v0 );
	ri.ptCoord1             = Point2( u1, v1 );
	ri.bHasTexCoord1        = hasUV1;
	ri.txFootprint.dudx     = 0.001;
	ri.txFootprint.dudy     = 0.001;
	ri.txFootprint.dvdx     = 0.001;
	ri.txFootprint.dvdy     = 0.001;
	ri.txFootprint.valid    = footprintValid;
	return ri;
}

//
// Test 1 — bHasTexCoord1 == true: all four accessors swap UV in.  The
// echoed `lastPtCoord` should be the UV1 (NOT UV0); the recorded
// `lastFootprintValid` should be FALSE (the wrapper invalidates).
//
static void TestSwapUV1AndInvalidatesFootprint()
{
	std::cout << "\n[1/3] bHasTexCoord1=true: all four accessors swap to UV1 + invalidate footprint\n";

	EchoPainter src; src.addref();
	IPainter* wrapper = 0;
	RISE_API_CreateTexCoord1Painter( &wrapper, src );
	assert( wrapper );

	const RayIntersectionGeometric ri = MakeRi(
		/*u0*/ 0.10, /*v0*/ 0.20,
		/*u1*/ 0.70, /*v1*/ 0.80,
		/*hasUV1*/ true,
		/*footprintValid*/ true );

	src.Reset();
	const RISEPel rgb = wrapper->GetColor( ri );
	Check( src.sawGetColor,
	       "GetColor reaches source painter" );
	Check( Close( src.lastPtCoord.x, 0.70 ) && Close( src.lastPtCoord.y, 0.80 ),
	       "GetColor: source sees UV1 (ri.ptCoord1), not UV0" );
	Check( src.lastFootprintValid == false,
	       "GetColor: source sees txFootprint.valid = false (wrapper invalidated)" );
	Check( Close( rgb[0], 0.70 ) && Close( rgb[1], 0.80 ),
	       "GetColor returns the source's UV1-sampled RGBPel" );

	src.Reset();
	const Scalar v = wrapper->GetColorNM( ri, 550.0 );
	Check( src.sawGetColorNM, "GetColorNM reaches source painter" );
	Check( Close( src.lastPtCoord.x, 0.70 ) && Close( src.lastPtCoord.y, 0.80 ),
	       "GetColorNM: source sees UV1" );
	Check( src.lastFootprintValid == false,
	       "GetColorNM: source sees footprint.valid = false" );
	Check( Close( v, 0.70 ),
	       "GetColorNM returns the source's UV1-sampled scalar" );

	// The user-found bug: GetSpectrum was missing the override.  Without
	// it, the base Painter::GetSpectrum returned a dummy packet without
	// even reaching the source.  This assertion is the canary.
	src.Reset();
	const SpectralPacket sp = wrapper->GetSpectrum( ri );
	(void)sp;
	Check( src.sawGetSpectrum,
	       "GetSpectrum reaches source painter (catches the round-4 bug)" );
	Check( Close( src.lastPtCoord.x, 0.70 ) && Close( src.lastPtCoord.y, 0.80 ),
	       "GetSpectrum: source sees UV1" );
	Check( src.lastFootprintValid == false,
	       "GetSpectrum: source sees footprint.valid = false" );

	src.Reset();
	const Scalar a = wrapper->GetAlpha( ri );
	Check( src.sawGetAlpha, "GetAlpha reaches source painter" );
	Check( Close( src.lastPtCoord.x, 0.70 ) && Close( src.lastPtCoord.y, 0.80 ),
	       "GetAlpha: source sees UV1" );
	Check( src.lastFootprintValid == false,
	       "GetAlpha: source sees footprint.valid = false" );
	Check( Close( a, 0.70 ),
	       "GetAlpha returns the source's UV1-sampled value" );

	wrapper->release();
	src.release();
}

//
// Test 2 — bHasTexCoord1 == false (analytic primitive / non-UV1 mesh):
// all four accessors pass ri through unchanged.  No UV swap.  No
// footprint touch (the source might still want valid footprints when
// sampling at UV0).
//
static void TestPassThroughWhenNoUV1()
{
	std::cout << "\n[2/3] bHasTexCoord1=false: all four accessors pass through unchanged\n";

	EchoPainter src; src.addref();
	IPainter* wrapper = 0;
	RISE_API_CreateTexCoord1Painter( &wrapper, src );
	assert( wrapper );

	const RayIntersectionGeometric ri = MakeRi(
		/*u0*/ 0.30, /*v0*/ 0.40,
		/*u1*/ 0.99, /*v1*/ 0.99,	// would-be UV1, but should not be used
		/*hasUV1*/ false,
		/*footprintValid*/ true );

	src.Reset();
	wrapper->GetColor( ri );
	Check( src.sawGetColor, "GetColor reaches source painter" );
	Check( Close( src.lastPtCoord.x, 0.30 ) && Close( src.lastPtCoord.y, 0.40 ),
	       "GetColor: source sees UV0 (no swap)" );
	Check( src.lastFootprintValid == true,
	       "GetColor: source sees footprint.valid PRESERVED (no UV swap)" );

	src.Reset();
	wrapper->GetColorNM( ri, 550.0 );
	Check( src.sawGetColorNM, "GetColorNM reaches source painter" );
	Check( Close( src.lastPtCoord.x, 0.30 ) && Close( src.lastPtCoord.y, 0.40 ),
	       "GetColorNM: source sees UV0" );
	Check( src.lastFootprintValid == true,
	       "GetColorNM: footprint.valid preserved" );

	src.Reset();
	wrapper->GetSpectrum( ri );
	Check( src.sawGetSpectrum, "GetSpectrum reaches source painter" );
	Check( Close( src.lastPtCoord.x, 0.30 ) && Close( src.lastPtCoord.y, 0.40 ),
	       "GetSpectrum: source sees UV0" );
	Check( src.lastFootprintValid == true,
	       "GetSpectrum: footprint.valid preserved" );

	src.Reset();
	wrapper->GetAlpha( ri );
	Check( src.sawGetAlpha, "GetAlpha reaches source painter" );
	Check( Close( src.lastPtCoord.x, 0.30 ) && Close( src.lastPtCoord.y, 0.40 ),
	       "GetAlpha: source sees UV0" );
	Check( src.lastFootprintValid == true,
	       "GetAlpha: footprint.valid preserved" );

	wrapper->release();
	src.release();
}

//
// Test 3 — invariant: the caller's ri is NEVER mutated by the wrapper.
// The wrapper builds a local ri2 copy; the input ri must be untouched
// after every accessor call (otherwise downstream code that re-reads ri
// would see the swap leak).
//
static void TestInputRiNotMutated()
{
	std::cout << "\n[3/3] caller's ri is not mutated by the wrapper\n";

	EchoPainter src; src.addref();
	IPainter* wrapper = 0;
	RISE_API_CreateTexCoord1Painter( &wrapper, src );
	assert( wrapper );

	RayIntersectionGeometric ri = MakeRi(
		/*u0*/ 0.10, /*v0*/ 0.20,
		/*u1*/ 0.70, /*v1*/ 0.80,
		/*hasUV1*/ true,
		/*footprintValid*/ true );

	wrapper->GetColor( ri );
	Check( Close( ri.ptCoord.x, 0.10 ) && Close( ri.ptCoord.y, 0.20 ),
	       "ri.ptCoord unchanged after GetColor" );
	Check( ri.txFootprint.valid == true,
	       "ri.txFootprint.valid unchanged after GetColor" );
	Check( ri.bHasTexCoord1 == true,
	       "ri.bHasTexCoord1 unchanged after GetColor" );

	wrapper->GetColorNM( ri, 550.0 );
	Check( ri.txFootprint.valid == true,
	       "ri.txFootprint.valid unchanged after GetColorNM" );

	wrapper->GetSpectrum( ri );
	Check( ri.txFootprint.valid == true,
	       "ri.txFootprint.valid unchanged after GetSpectrum" );

	wrapper->GetAlpha( ri );
	Check( ri.txFootprint.valid == true,
	       "ri.txFootprint.valid unchanged after GetAlpha" );

	wrapper->release();
	src.release();
}

int main()
{
	std::cout << "TexCoord1PainterTest -- glTF L12.D wrapper accessor forwarding\n";

	TestSwapUV1AndInvalidatesFootprint();
	TestPassThroughWhenNoUV1();
	TestInputRiNotMutated();

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
