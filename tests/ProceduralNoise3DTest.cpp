//////////////////////////////////////////////////////////////////////
//
//  ProceduralNoise3DTest.cpp - Consolidated unit test for all 10
//    active 3D procedural noise/field generators and their painters,
//    plus signed-coordinate pattern painter parity:
//
//    1. CurlNoise3D            + CurlNoise3DPainter
//    2. DomainWarpNoise3D      + DomainWarp3DPainter
//    3. GaborNoise3D           + Gabor3DPainter
//    4. PerlinWorleyNoise3D    + PerlinWorley3DPainter
//    5. ReactionDiffusion3D    + ReactionDiffusion3DPainter
//    6. SDFPrimitive3D         + SDF3DPainter
//    7. SimplexNoise3D         + Simplex3DPainter
//    8. TurbulenceNoise3D      + Turbulence3DPainter
//    9. WaveletNoise3D         + Wavelet3DPainter
//   10. WorleyNoise3D          + Worley3DPainter
//   11. CheckerPainter / LinesPainter negative-coordinate parity
//
//  For every 3D generator: finiteness, [0,1] bounds, determinism,
//  and spatial non-constancy across positive and negative 3D points.
//  For every 3D painter: RGB/NM field parity, finiteness, bounds,
//  determinism, and spatial non-constancy.
//  Analytic contracts: DomainWarp warpAmplitude/levels=0 -> Perlin,
//  PerlinWorley blend=0/1 endpoints, Worley jitter=0 exact F1/F2-F1,
//  SDF sphere/box sign symmetry and shell mode, ReactionDiffusion
//  unit periodicity, Wavelet tile periodicity.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>

#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/SimpleInterpolators.h"

#include "../src/Library/Noise/CurlNoise.h"
#include "../src/Library/Noise/DomainWarpNoise.h"
#include "../src/Library/Noise/GaborNoise.h"
#include "../src/Library/Noise/PerlinNoise.h"
#include "../src/Library/Noise/PerlinWorleyNoise.h"
#include "../src/Library/Noise/ReactionDiffusion.h"
#include "../src/Library/Noise/SDFPrimitives.h"
#include "../src/Library/Noise/SimplexNoise.h"
#include "../src/Library/Noise/TurbulenceNoise.h"
#include "../src/Library/Noise/WaveletNoise.h"
#include "../src/Library/Noise/WorleyNoise.h"

#include "../src/Library/Painters/CheckerPainter.h"
#include "../src/Library/Painters/CurlNoise3DPainter.h"
#include "../src/Library/Painters/DomainWarp3DPainter.h"
#include "../src/Library/Painters/Gabor3DPainter.h"
#include "../src/Library/Painters/LinesPainter.h"
#include "../src/Library/Painters/PerlinWorley3DPainter.h"
#include "../src/Library/Painters/ReactionDiffusion3DPainter.h"
#include "../src/Library/Painters/SDF3DPainter.h"
#include "../src/Library/Painters/Simplex3DPainter.h"
#include "../src/Library/Painters/Turbulence3DPainter.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/Wavelet3DPainter.h"
#include "../src/Library/Painters/Worley3DPainter.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int g_checks = 0;
	int g_failures = 0;

	void Check( const bool cond, const std::string& label )
	{
		++g_checks;
		if( !cond ) {
			++g_failures;
			std::cout << "  FAIL: " << label << std::endl;
		}
	}

	bool IsClose( const Scalar a, const Scalar b, const Scalar tol = 1e-6 )
	{
		return std::isfinite( a ) && std::isfinite( b ) && std::fabs( a - b ) < tol;
	}

	bool IsUnitInterval( const Scalar v, const Scalar eps = 1e-6 )
	{
		return std::isfinite( v ) && v >= -eps && v <= Scalar( 1.0 ) + eps;
	}

	Scalar NormalizedPerlin( const PerlinNoise3D& perlin, const Scalar x, const Scalar y, const Scalar z )
	{
		Scalar v = ( perlin.Evaluate( x, y, z ) + Scalar( 1.0 ) ) * Scalar( 0.5 );
		if( v < Scalar( 0.0 ) ) v = Scalar( 0.0 );
		if( v > Scalar( 1.0 ) ) v = Scalar( 1.0 );
		return v;
	}

	struct Sample3D { Scalar x, y, z; };

	const Sample3D kSamples[] = {
		{  0.0,    0.0,    0.0   },
		{  0.5,    0.5,    0.5   },
		{ -0.5,   -0.5,   -0.5   },
		{  0.125,  0.375,  0.625 },
		{ -0.125, -0.375, -0.625 },
		{  0.25,  -0.25,   0.75  },
		{ -0.75,   0.25,  -0.25  },
		{  0.875,  0.125,  0.125 },
		{  0.7,    0.0,    0.0   },
		{ -0.7,    0.0,    0.0   },
		{  0.0,    0.8,   -0.4   },
		{  1.2,   -0.9,    0.6   },
		{ -1.4,   -0.6,    1.1   },
		{  1.7,    2.3,   -0.9   },
		{ -2.1,    1.3,   -1.7   },
		{  3.4,   -2.7,    1.9   },
		{ -3.7,   -4.1,   -2.9   },
		{  5.3,    1.1,   -4.2   },
		{ -6.5,    3.2,   -1.8   },
		{ -10.5, -20.3,   -5.7   },
		{  0.31,   0.67,   0.19  },
		{ -0.43,   0.89,  -0.11  },
		{  0.61,  -0.17,   0.43  },
		{ -0.91,  -0.07,   0.53  },
	};
	constexpr size_t kNumSamples = sizeof( kSamples ) / sizeof( kSamples[0] );

	RayIntersectionGeometric MakeHitAt( const Scalar x, const Scalar y, const Scalar z )
	{
		Ray ray( Point3( x, y, z + 1.0 ), Vector3( 0, 0, -1 ) );
		RasterizerState rast = { 0, 0 };
		RayIntersectionGeometric ri( ray, rast );
		ri.bHit = true;
		ri.range = 1.0;
		ri.ptIntersection = Point3( x, y, z );
		ri.ptObjIntersec = Point3( x, y, z );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.vGeomNormal = Vector3( 0, 0, 1 );
		ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
		ri.ptCoord = Point2( x, y );
		return ri;
	}

	void VerifyGenerator3D( const char* name, const IFunction3D& fn )
	{
		bool inBounds = true;
		bool deterministic = true;
		Scalar minVal = 1e30;
		Scalar maxVal = -1e30;

		for( size_t i = 0; i < kNumSamples; ++i ) {
			const Sample3D& s = kSamples[i];
			const Scalar v1 = fn.Evaluate( s.x, s.y, s.z );
			const Scalar v2 = fn.Evaluate( s.x, s.y, s.z );
			if( !IsUnitInterval( v1 ) ) inBounds = false;
			if( !IsClose( v1, v2, 1e-12 ) ) deterministic = false;
			if( v1 < minVal ) minVal = v1;
			if( v1 > maxVal ) maxVal = v1;
		}

		Check( inBounds, std::string( name ) + ": all samples finite and in [0,1] (pos + neg coords)" );
		Check( deterministic, std::string( name ) + ": deterministic on repeated evaluation" );
		Check( ( maxVal - minVal ) > Scalar( 0.01 ),
			std::string( name ) + ": spatially non-constant (range=" + std::to_string( maxVal - minVal ) + ")" );
	}

	void VerifyPainter3DParity(
		const char* name,
		const IPainter& painter,
		const IPainter& black,
		const IPainter& white )
	{
		const RayIntersectionGeometric riRef = MakeHitAt( 0.0, 0.0, 0.0 );
		const Scalar nm = 550.0;
		const Scalar aNM = black.GetColorNM( riRef, nm );
		const Scalar bNM = white.GetColorNM( riRef, nm );
		const Scalar spanNM = bNM - aNM;
		Check( std::isfinite( spanNM ) && spanNM > Scalar( 0.5 ),
			std::string( name ) + ": reference black/white NM span valid" );

		bool rgbValid = true;
		bool nmValid = true;
		bool deterministic = true;
		bool parityOk = true;
		Scalar minRGB = 1e30, maxRGB = -1e30;
		Scalar minNM = 1e30, maxNM = -1e30;

		for( size_t i = 0; i < kNumSamples; ++i ) {
			const Sample3D& s = kSamples[i];
			const RayIntersectionGeometric ri = MakeHitAt( s.x, s.y, s.z );
			const RISEPel c1 = painter.GetColor( ri );
			const RISEPel c2 = painter.GetColor( ri );
			const Scalar n1 = painter.GetColorNM( ri, nm );
			const Scalar n2 = painter.GetColorNM( ri, nm );

			for( int ch = 0; ch < 3; ++ch ) {
				if( !IsUnitInterval( c1[ch] ) ) rgbValid = false;
				if( !IsClose( c1[ch], c2[ch], 1e-12 ) ) deterministic = false;
			}
			if( !std::isfinite( n1 ) || n1 < aNM - 1e-6 || n1 > bNM + 1e-6 ) nmValid = false;
			if( !IsClose( n1, n2, 1e-12 ) ) deterministic = false;

			// All 10 3D procedural painters evaluate the same field d in [0,1],
			// blending GetColor with CosineInterpolator and GetColorNM with LinearInterpolator.
			const Scalar dFromNM = ( n1 - aNM ) / spanNM;
			const Scalar expectedRGB = ( Scalar( 1.0 ) - std::cos( dFromNM * PI ) ) * Scalar( 0.5 );
			for( int ch = 0; ch < 3; ++ch ) {
				if( !IsClose( c1[ch], expectedRGB, 1e-5 ) ) parityOk = false;
			}

			if( c1[0] < minRGB ) minRGB = c1[0];
			if( c1[0] > maxRGB ) maxRGB = c1[0];
			if( n1 < minNM ) minNM = n1;
			if( n1 > maxNM ) maxNM = n1;
		}

		Check( rgbValid, std::string( name ) + ": GetColor finite and in [0,1]" );
		Check( nmValid, std::string( name ) + ": GetColorNM finite and within [aNM, bNM]" );
		Check( deterministic, std::string( name ) + ": GetColor and GetColorNM deterministic" );
		Check( parityOk, std::string( name ) + ": RGB/NM evaluate identical underlying 3D field" );
		Check( ( maxRGB - minRGB ) > Scalar( 0.01 ) && ( maxNM - minNM ) > Scalar( 0.01 ),
			std::string( name ) + ": spatially non-constant in both RGB and NM" );
		Check( !painter.IsSpectrallyDefined(),
			std::string( name ) + ": IsSpectrallyDefined false over UniformColorPainter operands" );
	}

	void TestAllGeneratorsAndPainters()
	{
		std::cout << "--- Section 1: All 10 3D Noise Generators + Painters ---" << std::endl;

		auto* interp = new RealLinearInterpolator();
		auto* black  = new UniformColorPainter( RISEPel( 0.0, 0.0, 0.0 ) );
		auto* white  = new UniformColorPainter( RISEPel( 1.0, 1.0, 1.0 ) );
		const Vector3 scale( 1.0, 1.0, 1.0 );
		const Vector3 shift( 0.0, 0.0, 0.0 );

		// 1. CurlNoise3D
		{
			auto* gen = new CurlNoise3D( *interp, 0.65, 3, 0.01 );
			auto* pnt = new CurlNoise3DPainter( 0.65, 3, 0.01, *black, *white, scale, shift );
			VerifyGenerator3D( "CurlNoise3D", *gen );
			VerifyPainter3DParity( "CurlNoise3DPainter", *pnt, *black, *white );
			pnt->release();
			gen->release();
		}

		// 2. DomainWarpNoise3D
		{
			auto* gen = new DomainWarpNoise3D( *interp, 0.65, 3, 2.0, 2 );
			auto* pnt = new DomainWarp3DPainter( 0.65, 3, 2.0, 2, *black, *white, scale, shift );
			VerifyGenerator3D( "DomainWarpNoise3D", *gen );
			VerifyPainter3DParity( "DomainWarp3DPainter", *pnt, *black, *white );
			pnt->release();
			gen->release();
		}

		// 3. GaborNoise3D
		{
			auto* gen = new GaborNoise3D( 2.0, 1.0, Vector3( 1.0, 0.0, 0.0 ), 4.0, 42u );
			auto* pnt = new Gabor3DPainter( 2.0, 1.0, Vector3( 1.0, 0.0, 0.0 ), 4.0, *black, *white, scale, shift );
			VerifyGenerator3D( "GaborNoise3D", *gen );
			VerifyPainter3DParity( "Gabor3DPainter", *pnt, *black, *white );
			pnt->release();
			gen->release();
		}

		// 4. PerlinWorleyNoise3D
		{
			auto* gen = new PerlinWorleyNoise3D( *interp, 0.5, 3, 1.0, 0.5 );
			auto* pnt = new PerlinWorley3DPainter( 0.5, 3, 1.0, 0.5, *black, *white, scale, shift );
			VerifyGenerator3D( "PerlinWorleyNoise3D", *gen );
			VerifyPainter3DParity( "PerlinWorley3DPainter", *pnt, *black, *white );
			pnt->release();
			gen->release();
		}

		// 5. ReactionDiffusion3D (small grid_size=8, iterations=5 -> milliseconds)
		{
			auto* gen = new ReactionDiffusion3D( 8, 0.2, 0.1, 0.037, 0.06, 5 );
			auto* pnt = new ReactionDiffusion3DPainter( 8, 0.2, 0.1, 0.037, 0.06, 5, *black, *white, scale, shift );
			VerifyGenerator3D( "ReactionDiffusion3D", *gen );
			VerifyPainter3DParity( "ReactionDiffusion3DPainter", *pnt, *black, *white );
			pnt->release();
			gen->release();
		}

		// 6. SDFPrimitive3D
		{
			auto* gen = new SDFPrimitive3D( eSDF_Sphere, 0.5, 0.0, 0.0, 0.0, 0.0, 0.0, *interp );
			auto* pnt = new SDF3DPainter( eSDF_Sphere, 0.5, 0.0, 0.0, 0.0, 0.0, 0.0, *black, *white, scale, shift );
			VerifyGenerator3D( "SDFPrimitive3D", *gen );
			VerifyPainter3DParity( "SDF3DPainter", *pnt, *black, *white );
			pnt->release();
			gen->release();
		}

		// 7. SimplexNoise3D
		{
			auto* gen = new SimplexNoise3D( 0.5, 3 );
			auto* pnt = new Simplex3DPainter( 0.5, 3, *black, *white, scale, shift );
			VerifyGenerator3D( "SimplexNoise3D", *gen );
			VerifyPainter3DParity( "Simplex3DPainter", *pnt, *black, *white );
			pnt->release();
			gen->release();
		}

		// 8. TurbulenceNoise3D (numOctaves=4 -> 3 active octaves)
		{
			auto* gen = new TurbulenceNoise3D( *interp, 0.5, 4 );
			auto* pnt = new Turbulence3DPainter( 0.5, 4, *black, *white, scale, shift );
			VerifyGenerator3D( "TurbulenceNoise3D", *gen );
			VerifyPainter3DParity( "Turbulence3DPainter", *pnt, *black, *white );
			pnt->release();
			gen->release();
		}

		// 9. WaveletNoise3D (smallest clamped tileSize=16)
		{
			auto* gen = new WaveletNoise3D( 16, 0.5, 3 );
			auto* pnt = new Wavelet3DPainter( 16, 0.5, 3, *black, *white, scale, shift );
			VerifyGenerator3D( "WaveletNoise3D", *gen );
			VerifyPainter3DParity( "Wavelet3DPainter", *pnt, *black, *white );
			pnt->release();
			gen->release();
		}

		// 10. WorleyNoise3D
		{
			auto* gen = new WorleyNoise3D( 1.0, eWorley_Euclidean, eWorley_F1 );
			auto* pnt = new Worley3DPainter( 1.0, eWorley_Euclidean, eWorley_F1, *black, *white, scale, shift );
			VerifyGenerator3D( "WorleyNoise3D", *gen );
			VerifyPainter3DParity( "Worley3DPainter", *pnt, *black, *white );
			pnt->release();
			gen->release();
		}

		white->release();
		black->release();
		interp->release();
	}

	void TestAnalyticContracts()
	{
		std::cout << "--- Section 2: Analytic Contracts ---" << std::endl;
		auto* interp = new RealLinearInterpolator();

		// DomainWarpNoise3D: warpAmplitude=0 and warpLevels=0 collapse to normalized PerlinNoise3D
		{
			auto* perlin    = new PerlinNoise3D( *interp, 0.65, 4 );
			auto* warpAmp0  = new DomainWarpNoise3D( *interp, 0.65, 4, 0.0, 2 );
			auto* warpLvl0  = new DomainWarpNoise3D( *interp, 0.65, 4, 4.0, 0 );
			bool amp0Match = true;
			bool lvl0Match = true;
			for( size_t i = 0; i < kNumSamples; ++i ) {
				const Sample3D& s = kSamples[i];
				const Scalar p = NormalizedPerlin( *perlin, s.x, s.y, s.z );
				if( !IsClose( warpAmp0->Evaluate( s.x, s.y, s.z ), p, 1e-5 ) ) amp0Match = false;
				if( !IsClose( warpLvl0->Evaluate( s.x, s.y, s.z ), p, 1e-5 ) ) lvl0Match = false;
			}
			Check( amp0Match, "DomainWarpNoise3D: warpAmplitude=0 collapses to PerlinNoise3D" );
			Check( lvl0Match, "DomainWarpNoise3D: warpLevels=0 collapses to PerlinNoise3D" );
			warpLvl0->release();
			warpAmp0->release();
			perlin->release();
		}

		// PerlinWorleyNoise3D: blend=0 -> Perlin, blend=1 -> (1 - Worley F1)
		{
			auto* perlin = new PerlinNoise3D( *interp, 0.5, 3 );
			auto* worley = new WorleyNoise3D( 1.0, eWorley_Euclidean, eWorley_F1 );
			auto* pw0    = new PerlinWorleyNoise3D( *interp, 0.5, 3, 1.0, 0.0 );
			auto* pw1    = new PerlinWorleyNoise3D( *interp, 0.5, 3, 1.0, 1.0 );
			bool match0 = true;
			bool match1 = true;
			for( size_t i = 0; i < kNumSamples; ++i ) {
				const Sample3D& s = kSamples[i];
				if( !IsClose( pw0->Evaluate( s.x, s.y, s.z ), NormalizedPerlin( *perlin, s.x, s.y, s.z ), 1e-5 ) ) match0 = false;
				if( !IsClose( pw1->Evaluate( s.x, s.y, s.z ), Scalar( 1.0 ) - worley->Evaluate( s.x, s.y, s.z ), 1e-5 ) ) match1 = false;
			}
			Check( match0, "PerlinWorleyNoise3D: blend=0 matches PerlinNoise3D" );
			Check( match1, "PerlinWorleyNoise3D: blend=1 matches (1 - WorleyNoise3D F1)" );
			pw1->release();
			pw0->release();
			worley->release();
			perlin->release();
		}

		// WorleyNoise3D: jitter=0 exact cell-center F1=0, corner F1=sqrt(3)/2, corner F2-F1=0, metrics valid
		{
			auto* wF1   = new WorleyNoise3D( 0.0, eWorley_Euclidean, eWorley_F1 );
			auto* wF2F1 = new WorleyNoise3D( 0.0, eWorley_Euclidean, eWorley_F2minusF1 );
			auto* wMht  = new WorleyNoise3D( 1.0, eWorley_Manhattan, eWorley_F1 );
			auto* wChb  = new WorleyNoise3D( 1.0, eWorley_Chebyshev, eWorley_F1 );

			Check( IsClose( wF1->Evaluate( 0.5, 0.5, 0.5 ), 0.0, 1e-8 ),
				"WorleyNoise3D: jitter=0 cell center (0.5,0.5,0.5) F1 == 0" );
			Check( IsClose( wF1->Evaluate( -1.5, -2.5, 0.5 ), 0.0, 1e-8 ),
				"WorleyNoise3D: jitter=0 negative cell center (-1.5,-2.5,0.5) F1 == 0" );
			Check( IsClose( wF1->Evaluate( 0.0, 0.0, 0.0 ), std::sqrt( 3.0 ) * 0.5, 1e-6 ),
				"WorleyNoise3D: jitter=0 cell corner (0,0,0) F1 == sqrt(3)/2" );
			Check( IsClose( wF2F1->Evaluate( 0.0, 0.0, 0.0 ), 0.0, 1e-8 ),
				"WorleyNoise3D: jitter=0 cell corner F2-F1 == 0" );
			Check( IsUnitInterval( wMht->Evaluate( -1.3, 2.7, -0.4 ) ) &&
				IsUnitInterval( wChb->Evaluate( -1.3, 2.7, -0.4 ) ),
				"WorleyNoise3D: Manhattan and Chebyshev metrics return valid [0,1] values" );

			wChb->release();
			wMht->release();
			wF2F1->release();
			wF1->release();
		}

		// SDFPrimitive3D: sphere/box sign symmetry, axis symmetry, shell hollow center, torus/cylinder bounds
		{
			auto* sphere = new SDFPrimitive3D( eSDF_Sphere, 0.5, 0.0, 0.0, 0.0, 0.0, 0.0, *interp );
			auto* box    = new SDFPrimitive3D( eSDF_Box, 0.5, 0.25, 0.75, 0.0, 0.0, 0.0, *interp );
			auto* shell  = new SDFPrimitive3D( eSDF_Sphere, 1.0, 0.0, 0.0, 0.1, 0.0, 0.0, *interp );
			auto* torus  = new SDFPrimitive3D( eSDF_Torus, 1.0, 0.3, 0.0, 0.0, 0.0, 0.0, *interp );
			auto* cyl    = new SDFPrimitive3D( eSDF_Cylinder, 0.5, 1.0, 0.0, 0.0, 0.0, 0.0, *interp );

			Check( IsClose( sphere->Evaluate( 0.0, 0.0, 0.0 ), 1.0, 1e-8 ),
				"SDFPrimitive3D sphere: center density == 1" );
			Check( sphere->Evaluate( 10.0, 10.0, 10.0 ) < 0.01,
				"SDFPrimitive3D sphere: far outside density < 0.01" );
			const Scalar sx = sphere->Evaluate( 0.7, 0.0, 0.0 );
			const Scalar sy = sphere->Evaluate( 0.0, 0.7, 0.0 );
			const Scalar sz = sphere->Evaluate( 0.0, 0.0, 0.7 );
			const Scalar sNeg = sphere->Evaluate( -0.4, -0.5, -0.3 );
			const Scalar sPos = sphere->Evaluate(  0.4,  0.5,  0.3 );
			Check( sx > 0.1 && sx < 0.99 && IsClose( sx, sy, 1e-6 ) && IsClose( sx, sz, 1e-6 ),
				"SDFPrimitive3D sphere: non-trivial exterior axis symmetry" );
			Check( IsClose( sPos, sNeg, 1e-6 ),
				"SDFPrimitive3D sphere: sign symmetry f(p) == f(-p)" );
			Check( IsClose( box->Evaluate( 0.7, -0.4, 0.9 ), box->Evaluate( -0.7, 0.4, -0.9 ), 1e-6 ),
				"SDFPrimitive3D box: sign symmetry f(p) == f(-p)" );
			Check( shell->Evaluate( 1.0, 0.0, 0.0 ) > shell->Evaluate( 0.0, 0.0, 0.0 ),
				"SDFPrimitive3D shell: surface denser than hollow center" );
			Check( torus->Evaluate( 1.0, 0.0, 0.0 ) > 0.5 && torus->Evaluate( 5.0, 5.0, 5.0 ) < 0.01,
				"SDFPrimitive3D torus: dense on ring, sparse outside" );
			Check( cyl->Evaluate( 0.0, 0.0, 0.0 ) > 0.9 && cyl->Evaluate( 2.0, 0.0, 0.0 ) < 0.05,
				"SDFPrimitive3D cylinder: dense at center, sparse outside" );

			cyl->release();
			torus->release();
			shell->release();
			box->release();
			sphere->release();
		}

		// ReactionDiffusion3D: unit periodicity across negative coordinates
		{
			auto* rd = new ReactionDiffusion3D( 8, 0.2, 0.1, 0.037, 0.06, 5 );
			Check( IsClose( rd->Evaluate( -0.3, -0.5, -0.1 ), rd->Evaluate( 0.7, 0.5, 0.9 ), 1e-6 ),
				"ReactionDiffusion3D: unit periodicity across negative coordinates" );
			rd->release();
		}

		// WaveletNoise3D: tile periodicity on single octave
		{
			auto* w1 = new WaveletNoise3D( 16, 0.5, 1 );
			Check( IsClose( w1->Evaluate( 1.25, -2.5, 3.75 ), w1->Evaluate( 17.25, 13.5, -12.25 ), 1e-6 ),
				"WaveletNoise3D: single-octave tile periodicity (period 16)" );
			w1->release();
		}

		interp->release();
	}

	void TestPatternPainterNegativeParity()
	{
		std::cout << "--- Section 3: CheckerPainter / LinesPainter Negative-Coordinate Parity ---" << std::endl;

		auto* black = new UniformColorPainter( RISEPel( 0.0, 0.0, 0.0 ) );
		auto* white = new UniformColorPainter( RISEPel( 1.0, 1.0, 1.0 ) );
		auto* checker = new CheckerPainter( 1.0, *black, *white );
		auto* linesU  = new LinesPainter( 1.0, *black, *white, true );
		auto* linesV  = new LinesPainter( 0.25, *black, *white, false );

		auto which = []( const IPainter& p, const Scalar u, const Scalar v ) -> int {
			RayIntersectionGeometric ri = MakeHitAt( 0.0, 0.0, 0.0 );
			ri.ptCoord = Point2( u, v );
			return p.GetColor( ri )[0] > Scalar( 0.5 ) ? 1 : 0;
		};

		// Checker: origin quadrant consistency and negative-quadrant alternation
		const int pp = which( *checker,  0.5,  0.5 );
		const int np = which( *checker, -0.5,  0.5 );
		const int pn = which( *checker,  0.5, -0.5 );
		const int nn = which( *checker, -0.5, -0.5 );
		Check( pp == nn && np == pn && pp != np,
			"CheckerPainter: origin quadrant parity consistent across (++, -+, +-, --)" );

		const int n1 = which( *checker, -5.5, -5.5 );
		const int n2 = which( *checker, -4.5, -5.5 );
		const int n3 = which( *checker, -4.5, -4.5 );
		Check( n1 != n2 && n1 == n3,
			"CheckerPainter: alternates properly in deep negative quadrant" );

		// Lines: uniform stripe width across origin (no doubled center stripe) + deep negative parity
		bool linesAlt = true;
		int prev = -1;
		for( int i = -8; i <= 7; ++i ) {
			const int cur = which( *linesU, i + 0.5, 0.0 );
			if( prev >= 0 && cur == prev ) linesAlt = false;
			prev = cur;
		}
		Check( linesAlt, "LinesPainter: alternates uniformly across origin [-8..7]" );
		Check( which( *linesV, 0.0, -100.0 + 0.125 ) == which( *linesV, 0.0, 100.0 + 0.125 ),
			"LinesPainter: deep negative/positive even stripes match" );

		linesV->release();
		linesU->release();
		checker->release();
		white->release();
		black->release();
	}
}

int main()
{
	GlobalLog();
	std::cout << "=== ProceduralNoise3DTest ===" << std::endl;

	TestAllGeneratorsAndPainters();
	TestAnalyticContracts();
	TestPatternPainterNegativeParity();

	std::cout << "----------------------------------------" << std::endl;
	std::cout << "checks: " << g_checks << "   failures: " << g_failures << std::endl;
	if( g_failures == 0 ) {
		std::cout << "ProceduralNoise3DTest: PASSED" << std::endl;
		return 0;
	}
	std::cout << "ProceduralNoise3DTest: FAILED" << std::endl;
	return 1;
}
