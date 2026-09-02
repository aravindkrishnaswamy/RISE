//////////////////////////////////////////////////////////////////////
//
//  PainterRadianceForwardingTest.cpp - Stage C slice 2 regression:
//    SINGLE-SOURCE forwarding painters must forward `GetRadianceNM`
//    to the same source they forward `GetColorNM` to, at the same
//    transformed `ri`.
//
//  WHY THIS EXISTS.  `IPainter::GetRadianceNM`'s default uplifts the
//  painter's COMPOSED `GetColor` through the Jakob-Hanika LUT as an
//  illuminant.  That is the correct semantics for a painter that
//  genuinely BLENDS several sources (blend / ramp / noise-interpolated
//  a-vs-b): what such a painter emits IS its composed colour.  It is
//  the WRONG semantics for a painter that composes nothing and merely
//  re-parameterises or selects — a UV transform, a mapping projection,
//  a checker/lines/voronoi selector, a scatter stamp, a hex tiling.
//  For those, the default discards the chosen source's own spectrum:
//
//    * a `spectral_painter` behind a `uv_transform` on a luminaire's
//      exitance gets re-uplifted from its RGB projection (wrong
//      spectrum), and
//    * a `piecewise_linear_function`-backed painter
//      (Function1DSpectralPainter), whose `GetColor` returns BLACK,
//      emits exactly ZERO.
//
//  The second case is what this test pins: every wrapper below is
//  built over a Function1DSpectralPainter with a strongly non-flat
//  SPD.  With the `GetRadianceNM` override removed from any wrapper,
//  that wrapper's GetRadianceNM collapses to 0 and the corresponding
//  checks fail — the mutation is unambiguous.
//
//  A second painter (UVSpectralEchoPainter) has a UV-DEPENDENT
//  radiance, which pins the OTHER half of the contract: the wrapper
//  must forward the TRANSFORMED `ri`, not the original one.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Painters/Painter.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IFunction1D.h"
#include "../src/Library/Interfaces/IPiecewiseFunction.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Color/RGBSpectra.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int s_pass = 0;
	int s_fail = 0;

	void Check( bool ok, const char* what )
	{
		if( ok ) {
			++s_pass;
		} else {
			++s_fail;
			std::cout << "  FAIL: " << what << "\n";
		}
	}

	bool Close( double a, double b, double eps = 1e-9 )
	{
		return std::fabs( a - b ) <= eps;
	}

	RayIntersectionGeometric MakeRiAtUV( const Scalar u, const Scalar v )
	{
		const Ray r( Point3( 0, 0, 1 ), Vector3( 0, 0, -1 ) );
		RayIntersectionGeometric ri( r, nullRasterizerState );
		ri.bHit = true;
		ri.ptIntersection = Point3( u, v, 0 );
		ri.ptObjIntersec = Point3( u, v, 0 );
		ri.ptCoord = Point2( u, v );
		ri.ptCoord1 = Point2( u, v );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.vGeomNormal = Vector3( 0, 0, 1 );
		return ri;
	}

	// A physically-authored SPD bound through `piecewise_linear_function`
	// -> `Function1DSpectralPainter`.  Deliberately steep so a wrapper
	// that re-uplifts an RGB projection cannot accidentally reproduce it,
	// and non-zero everywhere in the sampled range so "> 0" is a real
	// assertion.  Function1DSpectralPainter::GetColor is BLACK, which is
	// what makes the missing-override mutation collapse to exactly 0.
	IPainter* MakeSpectralSource()
	{
		IPiecewiseFunction1D* f = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &f );
		const Scalar x[] = { 380, 450, 550, 650, 780 };
		const Scalar y[] = { 0.10, 0.90, 0.30, 2.50, 0.40 };
		f->addControlPoints( 5, x, y );

		IPainter* p = 0;
		RISE_API_CreateFunction1DSpectralPainter( &p, *f );
		f->release();
		return p;
	}

	// UV-dependent spectral source: radiance = u * 1000 + v * 10 + nm.
	// Reading a wrapper's GetRadianceNM therefore reveals EXACTLY which
	// (u, v) the wrapper sampled at — the same trick UVTransformPainterTest
	// uses on the RGB path with UVEchoPainter.  GetColor is black here too,
	// mirroring Function1DSpectralPainter, so a wrapper that fell back to
	// the composed-GetColor default would read 0 rather than a plausible
	// wrong number.
	class UVSpectralEchoPainter : public Painter
	{
	public:
		UVSpectralEchoPainter() {}
	protected:
		virtual ~UVSpectralEchoPainter() {}
	public:
		RISEPel GetColor( const RayIntersectionGeometric& ) const
		{
			return RISEPel( 0, 0, 0 );
		}
		Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
		{
			return ri.ptCoord.x * Scalar( 1000 ) + ri.ptCoord.y * Scalar( 10 ) + nm;
		}
		Scalar GetRadianceNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
		{
			return GetColorNM( ri, nm );
		}
		Scalar GetAlpha( const RayIntersectionGeometric& ) const { return Scalar( 1 ); }
		IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
		void SetIntermediateValue( const IKeyframeParameter& ) {}
		void RegenerateData() {}
	};

	// UV-INDEPENDENT source with GetColorNM and GetRadianceNM carrying two
	// deliberately unrelated, strongly-differing formulas of `nm` (used
	// to independently reconstruct StochasticTilePainter's output below
	// without duplicating its internal hash-tiling; see TestScatterAndTiling).
	class SplitColorRadiancePainter : public Painter
	{
	public:
		SplitColorRadiancePainter() {}
	protected:
		virtual ~SplitColorRadiancePainter() {}
	public:
		RISEPel GetColor( const RayIntersectionGeometric& ) const { return RISEPel( 0, 0, 0 ); }
		Scalar GetColorNM( const RayIntersectionGeometric&, const Scalar nm ) const
		{
			return Scalar( 100 ) + nm;
		}
		Scalar GetRadianceNM( const RayIntersectionGeometric&, const Scalar nm ) const
		{
			return Scalar( 5000 ) - Scalar( 3 ) * nm;
		}
		Scalar GetAlpha( const RayIntersectionGeometric& ) const { return Scalar( 1 ); }
		IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
		void SetIntermediateValue( const IKeyframeParameter& ) {}
		void RegenerateData() {}
	};

	const Scalar kNMs[] = { 420, 500, 560, 640, 700 };
	const int kNumNM = 5;

	// Core contract for a single-source forwarder: at every wavelength the
	// wrapper's radiance equals the source's radiance at the ri the wrapper
	// chose, and it is non-zero (the missing-override mutation gives 0).
	void CheckForwardsSpectrum(
		const char* label,
		const IPainter& wrapper,
		const IPainter& source,
		const RayIntersectionGeometric& riOuter,
		const RayIntersectionGeometric& riInner )
	{
		bool allMatch = true;
		bool allPositive = true;
		for( int i = 0; i < kNumNM; ++i ) {
			const Scalar got = wrapper.GetRadianceNM( riOuter, kNMs[i] );
			const Scalar want = source.GetRadianceNM( riInner, kNMs[i] );
			if( !Close( double( got ), double( want ), 1e-12 ) ) {
				allMatch = false;
				std::printf( "    %s: nm=%g got=%.12g want=%.12g\n",
					label, double( kNMs[i] ), double( got ), double( want ) );
			}
			if( !( double( got ) > 0.0 ) ) allPositive = false;
		}
		std::string m1 = std::string( label ) + ": GetRadianceNM == wrapped source's GetRadianceNM";
		std::string m2 = std::string( label ) + ": GetRadianceNM > 0 (would be 0 without the override)";
		Check( allMatch, m1.c_str() );
		Check( allPositive, m2.c_str() );
	}
}

//////////////////////////////////////////////////////////////////////
// 1. uv_transform_painter
//////////////////////////////////////////////////////////////////////
static void TestUVTransform()
{
	std::cout << "\n[1] uv_transform_painter\n";
	IPainter* src = MakeSpectralSource();
	IPainter* wrapper = 0;
	RISE_API_CreateUVTransformPainter( &wrapper, *src,
		/*offset_u*/ 0.1, /*offset_v*/ 0.2, /*rotation*/ 0.0,
		/*scale_u*/ 2.0, /*scale_v*/ 3.0 );

	const RayIntersectionGeometric ri = MakeRiAtUV( 0.25, 0.5 );
	// Function1DSpectralPainter ignores ri, so ri==ri here is fine for
	// the spectrum-identity half; the ri-plumbing half is the echo test.
	CheckForwardsSpectrum( "uv_transform", *wrapper, *src, ri, ri );
	wrapper->release();
	src->release();

	// ri plumbing: the wrapper must sample at the TRANSFORMED uv.
	UVSpectralEchoPainter* echo = new UVSpectralEchoPainter();
	echo->addref();
	IPainter* w2 = 0;
	RISE_API_CreateUVTransformPainter( &w2, *echo,
		0.1, 0.2, 0.0, 2.0, 3.0 );
	// (u,v) = (1,1) -> scale (2,3) -> translate -> (2.1, 3.2)
	const RayIntersectionGeometric ri2 = MakeRiAtUV( 1.0, 1.0 );
	const Scalar got = w2->GetRadianceNM( ri2, Scalar( 550 ) );
	const Scalar want = Scalar( 2.1 ) * Scalar( 1000 ) + Scalar( 3.2 ) * Scalar( 10 ) + Scalar( 550 );
	Check( Close( double( got ), double( want ), 1e-9 ),
	       "uv_transform: GetRadianceNM sampled at the TRANSFORMED uv" );
	w2->release();
	echo->release();
}

//////////////////////////////////////////////////////////////////////
// 2. mapping_painter (uv projection + triplanar partition)
//////////////////////////////////////////////////////////////////////
static void TestMapping()
{
	std::cout << "\n[2] mapping_painter\n";
	IPainter* src = MakeSpectralSource();

	// Proj_UV
	IPainter* wUV = 0;
	RISE_API_CreateMappingPainter( &wUV, *src, /*projection*/ 0,
		Vector3( 1, 1, 1 ), Vector3( 0, 0, 0 ), Vector3( 0, 0, 0 ), 4.0 );
	const RayIntersectionGeometric ri = MakeRiAtUV( 0.3, 0.7 );
	CheckForwardsSpectrum( "mapping(uv)", *wUV, *src, ri, ri );
	wUV->release();

	// Proj_Triplanar: the three projections' weights sum to 1, so a
	// source with a ri-independent spectrum must come back unchanged.
	IPainter* wTri = 0;
	RISE_API_CreateMappingPainter( &wTri, *src, /*projection*/ 3,
		Vector3( 1, 1, 1 ), Vector3( 0, 0, 0 ), Vector3( 0, 0, 0 ), 4.0 );
	CheckForwardsSpectrum( "mapping(triplanar)", *wTri, *src, ri, ri );
	wTri->release();

	src->release();

	// ri plumbing on the uv projection: scale 2/3, no rotation, translate
	// 0.1/0.2.  Independently derived expected value (same style as
	// TestUVTransform): MappingPainter::ApplyUV computes
	// (cosRz*scaleU*u - sinRz*scaleV*v + translateU,
	//  sinRz*scaleU*u + cosRz*scaleV*v + translateV); with rotateDeg=0
	// (cosRz=1, sinRz=0) that is just (scaleU*u + translateU,
	// scaleV*v + translateV) = (2*1.0+0.1, 3*1.0+0.2) = (2.1, 3.2).
	UVSpectralEchoPainter* echo = new UVSpectralEchoPainter();
	echo->addref();
	IPainter* w2 = 0;
	RISE_API_CreateMappingPainter( &w2, *echo, /*projection*/ 0,
		Vector3( 2, 3, 1 ), Vector3( 0, 0, 0 ), Vector3( 0.1, 0.2, 0 ), 4.0 );
	const RayIntersectionGeometric ri2 = MakeRiAtUV( 1.0, 1.0 );
	const Scalar gotRad = w2->GetRadianceNM( ri2, Scalar( 550 ) );
	const Scalar want = Scalar( 2.1 ) * Scalar( 1000 ) + Scalar( 3.2 ) * Scalar( 10 ) + Scalar( 550 );
	Check( Close( double( gotRad ), double( want ), 1e-9 ),
	       "mapping(uv): GetRadianceNM sampled at the TRANSFORMED uv" );
	// GetColorNM must agree with GetRadianceNM here too (echo painter has
	// no colour-vs-radiance distinction) -- keeps the original same-uv
	// cross-check as a second, independent assertion rather than the sole one.
	const Scalar gotCol = w2->GetColorNM( ri2, Scalar( 550 ) );
	Check( Close( double( gotCol ), double( want ), 1e-9 ),
	       "mapping(uv): GetColorNM sampled at the TRANSFORMED uv" );
	w2->release();
	echo->release();
}

//////////////////////////////////////////////////////////////////////
// 3. texcoord1_painter
//////////////////////////////////////////////////////////////////////
static void TestTexCoord1()
{
	std::cout << "\n[3] texcoord1_painter\n";
	IPainter* src = MakeSpectralSource();
	IPainter* wrapper = 0;
	RISE_API_CreateTexCoord1Painter( &wrapper, *src );
	const RayIntersectionGeometric ri = MakeRiAtUV( 0.4, 0.6 );
	CheckForwardsSpectrum( "texcoord1", *wrapper, *src, ri, ri );
	wrapper->release();
	src->release();

	// ri plumbing: with bHasTexCoord1 the wrapper must sample ptCoord1.
	UVSpectralEchoPainter* echo = new UVSpectralEchoPainter();
	echo->addref();
	IPainter* w2 = 0;
	RISE_API_CreateTexCoord1Painter( &w2, *echo );
	RayIntersectionGeometric ri2 = MakeRiAtUV( 0.1, 0.2 );
	ri2.bHasTexCoord1 = true;
	ri2.ptCoord1 = Point2( 0.7, 0.9 );
	const Scalar got = w2->GetRadianceNM( ri2, Scalar( 500 ) );
	const Scalar want = Scalar( 0.7 ) * Scalar( 1000 ) + Scalar( 0.9 ) * Scalar( 10 ) + Scalar( 500 );
	Check( Close( double( got ), double( want ), 1e-9 ),
	       "texcoord1: GetRadianceNM sampled at ptCoord1, not ptCoord" );
	w2->release();
	echo->release();
}

//////////////////////////////////////////////////////////////////////
// 4. Selector painters: checker / lines / voronoi2d / voronoi3d.
//    Each picks ONE child; both children are spectral here, with
//    different SPDs, so the result must equal one of them exactly.
//////////////////////////////////////////////////////////////////////
static void TestSelectors()
{
	std::cout << "\n[4] selector painters (checker / lines / voronoi)\n";

	IPainter* a = MakeSpectralSource();
	IPainter* bUniform = 0;
	// A second, DIFFERENT spectral child so "matches one of them" is a
	// real constraint: a mid-grey uniform colour painter's GetRadianceNM
	// is the illuminant uplift of (0.5, 0.5, 0.5).
	RISE_API_CreateUniformColorPainter( &bUniform, RISEPel( 0.5, 0.5, 0.5 ) );

	struct Case { const char* name; IPainter* w; };
	std::vector<Case> cases;

	IPainter* checker = 0;
	RISE_API_CreateCheckerPainter( &checker, 0.25, *a, *bUniform );
	cases.push_back( Case{ "checker", checker } );

	IPainter* lines = 0;
	RISE_API_CreateLinesPainter( &lines, 0.25, *a, *bUniform, true );
	cases.push_back( Case{ "lines", lines } );

	// voronoi2d: two generators, one spectral child and one uniform,
	// with a zero-width border so the border painter is never selected.
	IPainter* voronoi = 0;
	{
		std::vector<Point2> pts;
		pts.push_back( Point2( 0.25, 0.25 ) );
		pts.push_back( Point2( 0.75, 0.75 ) );
		std::vector<IPainter*> childs;
		childs.push_back( a );
		childs.push_back( bUniform );
		RISE_API_CreateVoronoi2DPainter( &voronoi, pts, childs, *bUniform, 0.0 );
	}
	cases.push_back( Case{ "voronoi2d", voronoi } );

	// voronoi3d: same generator-picks-one-child contract, but the
	// generators and the query point are Point3 (z=0 plane here, since
	// MakeRiAtUV puts (u,v,0) in both ptIntersection and ptObjIntersec).
	// RISE_API_CreateVoronoi3DPainter (the non-WithSpace overload) samples
	// ptObjIntersec, which MakeRiAtUV also sets, so this exercises the
	// same code path CheckerPainter/LinesPainter/Voronoi2DPainter do.
	IPainter* voronoi3d = 0;
	{
		std::vector<Point3> pts;
		pts.push_back( Point3( 0.25, 0.25, 0 ) );
		pts.push_back( Point3( 0.75, 0.75, 0 ) );
		std::vector<IPainter*> childs;
		childs.push_back( a );
		childs.push_back( bUniform );
		RISE_API_CreateVoronoi3DPainter( &voronoi3d, pts, childs, *bUniform, 0.0 );
	}
	cases.push_back( Case{ "voronoi3d", voronoi3d } );

	for( std::size_t c = 0; c < cases.size(); ++c ) {
		bool ok = true;
		bool sawSpectralChild = false;
		// Sweep UV so both branches of the selector are hit.
		for( int iu = 0; iu < 8; ++iu ) {
			for( int iv = 0; iv < 8; ++iv ) {
				const RayIntersectionGeometric ri =
					MakeRiAtUV( Scalar( iu ) * Scalar( 0.13 ), Scalar( iv ) * Scalar( 0.17 ) );
				for( int i = 0; i < kNumNM; ++i ) {
					const Scalar got = cases[c].w->GetRadianceNM( ri, kNMs[i] );
					const Scalar wa = a->GetRadianceNM( ri, kNMs[i] );
					const Scalar wb = bUniform->GetRadianceNM( ri, kNMs[i] );
					const bool matchA = Close( double( got ), double( wa ), 1e-12 );
					const bool matchB = Close( double( got ), double( wb ), 1e-12 );
					if( !matchA && !matchB ) ok = false;
					if( matchA && !matchB ) sawSpectralChild = true;
				}
			}
		}
		std::string m1 = std::string( cases[c].name ) + ": GetRadianceNM equals one CHILD's GetRadianceNM";
		std::string m2 = std::string( cases[c].name ) + ": the spectral child's SPD reaches the output";
		Check( ok, m1.c_str() );
		Check( sawSpectralChild, m2.c_str() );
		cases[c].w->release();
	}

	bUniform->release();
	a->release();
}

//////////////////////////////////////////////////////////////////////
// 5. scatter_painter (coverage compose) and stochastic_tile_painter
//    (single-source hex reconstruction).
//////////////////////////////////////////////////////////////////////
static void TestScatterAndTiling()
{
	std::cout << "\n[5] scatter_painter / stochastic_tile_painter\n";

	// Scatter with probability 0: no stamp is ever placed, so every
	// query is pure BACKGROUND -- a clean forward through the wrapper.
	{
		IPainter* stamp = 0;
		RISE_API_CreateUniformColorPainter( &stamp, RISEPel( 1, 1, 1 ) );
		IPainter* bg = MakeSpectralSource();
		IPainter* w = 0;
		RISE_API_CreateScatterPainter( &w, *stamp, *bg,
			/*cellScale*/ 4.0, /*stampScale*/ 0.5, /*jitterPosition*/ 0.0,
			/*jitterRotationDeg*/ 0.0, /*jitterScale*/ 0.0,
			/*probability*/ 0.0, /*seed*/ 7 );
		const RayIntersectionGeometric ri = MakeRiAtUV( 0.33, 0.61 );
		CheckForwardsSpectrum( "scatter(background)", *w, *bg, ri, ri );
		w->release();
		bg->release();
		stamp->release();
	}

	// Stochastic tiling reads ONE source at three hash-offset UVs and
	// recombines them about the authored mean:
	//   result = mu + sum_k( (s_k - mu) * w_k ) / sqrt(sum_k w_k^2)
	// where (w_k, uv_k) come from the SAME ComputeHexTiling(ri.ptCoord)
	// call in GetColorNM and GetRadianceNM (StochasticTilePainter.cpp
	// calls it identically in both -- "structural twins"), and `mu` is
	// RGBAlbedoSpectrum::FromRGB(mean).Eval(nm) for GetColorNM vs
	// RGBIlluminantSpectrum::FromRGB(mean).Eval(nm) for GetRadianceNM.
	//
	// ComputeHexTiling is `protected` (not reachable from this free
	// function without duplicating the Worley-hash tiling), so reproduce
	// the hashing is impractical here.  Instead: drive the source with
	// SplitColorRadiancePainter, whose GetColorNM and GetRadianceNM are
	// UV-INDEPENDENT and carry two strongly different formulas of `nm`.
	// Because the source is UV-independent, all three hash-offset samples
	// read the identical value in each call, collapsing the sum to a
	// single unknown `S = sum_k w_k / sqrt(sum_k w_k^2)` -- shared between
	// the two calls because they tile the SAME ri.ptCoord. Solve for S
	// from the (independently-checkable) GetColorNM equation, then
	// predict GetRadianceNM from it using the illuminant-space `mu` and
	// the source's own (very different) GetRadianceNM value, and assert
	// the wrapper's actual GetRadianceNM matches that prediction to 1e-9.
	// This is sensitive to exactly the mutations the override exists to
	// prevent: reverting to `meanSpec` (wrong mu) or to
	// `source.GetColorNM` (wrong s) inside GetRadianceNM both break the
	// prediction, since here GetColorNM and GetRadianceNM genuinely
	// differ.
	{
		SplitColorRadiancePainter* src = new SplitColorRadiancePainter();
		src->addref();
		IPainter* w = 0;
		const RISEPel mean( 0.5, 0.2, 0.8 );
		RISE_API_CreateStochasticTilePainter( &w, *src,
			/*tileScale*/ 4.0, /*seed*/ 11,
			/*mean*/ mean, /*blendGamma*/ 1.0 );

		const RayIntersectionGeometric ri = MakeRiAtUV( 0.21, 0.44 );
		bool matches = true;
		bool positive = true;
		for( int i = 0; i < kNumNM; ++i ) {
			const Scalar nm = kNMs[i];
			const double muC = double( RGBAlbedoSpectrum::FromRGB( mean ).Eval( nm ) );
			const double muR = double( RGBIlluminantSpectrum::FromRGB( mean ).Eval( nm ) );
			const double sC = double( src->GetColorNM( ri, nm ) );
			const double sR = double( src->GetRadianceNM( ri, nm ) );
			const double Rc = double( w->GetColorNM( ri, nm ) );
			const double Rr = double( w->GetRadianceNM( ri, nm ) );

			// S is shared between the two reconstructions (same ri.ptCoord
			// -> same ComputeHexTiling weights); sC != muC by construction
			// (100+nm is far from a [0,1]-clamped albedo mean) so this
			// division is safe.
			const double S = ( Rc - muC ) / ( sC - muC );
			const double RrPredicted = muR + ( sR - muR ) * S;

			if( !Close( Rr, RrPredicted, 1e-9 ) ) {
				matches = false;
				std::printf( "    stochastic_tile: nm=%g Rr=%.12g predicted=%.12g\n",
					double( nm ), Rr, RrPredicted );
			}
			if( !( Rr > 0.0 ) ) positive = false;
		}
		Check( matches,
		       "stochastic_tile: GetRadianceNM matches the independently-derived "
		       "illuminant-space reconstruction (RGBIlluminantSpectrum::FromRGB(mean).Eval(nm))" );
		Check( positive,
		       "stochastic_tile: GetRadianceNM > 0 (would be 0 without the override)" );
		w->release();
		src->release();
	}
}

//////////////////////////////////////////////////////////////////////
// 6. NEGATIVE CONTROL: a genuine BLEND keeps the composed default.
//    blend_painter mixes two sources, so what it EMITS is its composed
//    colour -- and this test must not silently start asserting the
//    forwarding contract on it.
//////////////////////////////////////////////////////////////////////
static void TestBlendKeepsComposedDefault()
{
	std::cout << "\n[6] blend_painter keeps the composed default (negative control)\n";
	IPainter* a = 0;
	RISE_API_CreateUniformColorPainter( &a, RISEPel( 0.8, 0.2, 0.2 ) );
	IPainter* b = 0;
	RISE_API_CreateUniformColorPainter( &b, RISEPel( 0.2, 0.2, 0.8 ) );
	IPainter* mask = 0;
	RISE_API_CreateUniformColorPainter( &mask, RISEPel( 0.5, 0.5, 0.5 ) );
	IPainter* blend = 0;
	RISE_API_CreateBlendPainter( &blend, *a, *b, *mask );

	const RayIntersectionGeometric ri = MakeRiAtUV( 0.5, 0.5 );
	// The default uplifts GetColor(ri) as an illuminant.  Reproduce that
	// here from the public surface: a uniform painter of the blend's
	// composed colour has exactly that GetRadianceNM.
	IPainter* composed = 0;
	RISE_API_CreateUniformColorPainter( &composed, blend->GetColor( ri ) );
	bool ok = true;
	for( int i = 0; i < kNumNM; ++i ) {
		if( !Close( double( blend->GetRadianceNM( ri, kNMs[i] ) ),
		            double( composed->GetRadianceNM( ri, kNMs[i] ) ), 1e-12 ) ) ok = false;
	}
	Check( ok, "blend: GetRadianceNM == illuminant uplift of the COMPOSED colour" );

	composed->release();
	blend->release();
	mask->release();
	b->release();
	a->release();
}

int main()
{
	std::cout << "PainterRadianceForwardingTest -- Stage C slice 2 single-source forwarders\n";

	TestUVTransform();
	TestMapping();
	TestTexCoord1();
	TestSelectors();
	TestScatterAndTiling();
	TestBlendKeepsComposedDefault();

	std::cout << "\nResults: " << s_pass << " passed, " << s_fail << " failed.\n";
	return ( s_fail == 0 ) ? 0 : 1;
}
