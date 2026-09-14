//////////////////////////////////////////////////////////////////////
//
//  CookTorranceSchlickGlossyFilterConsistencyTest.cpp - DL-65 red proof.
//
//  DL-65 (sibling of DL-62, docs/skills/audit-by-bug-pattern.md): the
//  DL-62 pattern -- "an SPF widens roughness by ri.glossyFilterWidth for
//  sampling/density, but the paired BRDF's value/valueNM does not" -- is
//  structurally present, unfixed, in two more material families:
//
//    * CookTorranceSPF::Scatter/ScatterNM/Pdf widen `alpha` (read from
//      pMasking) by ri.glossyFilterWidth (CookTorranceSPF.cpp ~152-153,
//      365-366, 549-550: "if (ri.glossyFilterWidth > 0) alpha =
//      r_min(alpha + ri.glossyFilterWidth, 1.0);"), but
//      CookTorranceBRDF::value/valueNM read pMasking directly with no
//      widening at all.
//
//    * SchlickSPF::Scatter/ScatterNM/Pdf/PdfNM widen roughness (read
//      from pRoughness) the same way (SchlickSPF.cpp ~296-298, 368-369,
//      427-429, 483-484), but SchlickBRDF::value/valueNM read
//      pRoughness directly with no widening.
//
//  Same methodology as GGXSampleEvaluationConsistencyTest's DL-62
//  section: compare the production BRDF (raw/unwidened roughness
//  painter + nonzero ri.glossyFilterWidth) against an independently
//  constructed reference BRDF whose roughness painter ALREADY holds the
//  intended effective (pre-widened) value, with ri.glossyFilterWidth
//  left at 0.  The reference is right by construction, independent of
//  whether the production BRDF's own filter-width handling works.  A
//  zero-filter-width control is included for both families to confirm
//  the harness itself is not the source of any observed mismatch.
//
//  Build (from project root):
//    c++ -arch arm64 -Isrc/Library -I/opt/homebrew/include
//        -O3 -ffast-math -fno-finite-math-only -funroll-loops -Wall -pedantic
//        -Wno-c++11-long-long -DCOLORS_RGB -DMERSENNE53
//        -DNO_TIFF_SUPPORT -DNO_EXR_SUPPORT -DRISE_ENABLE_MAILBOXING
//        -c tests/CookTorranceSchlickGlossyFilterConsistencyTest.cpp
//        -o tests/CookTorranceSchlickGlossyFilterConsistencyTest.o
//    c++ -arch arm64 -o tests/cooktorrance_schlick_glossy_filter_test
//        tests/CookTorranceSchlickGlossyFilterConsistencyTest.o bin/librise.a
//        -L/opt/homebrew/lib -lpng -lz
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <cmath>
#include <string>
#include <sstream>
#include <algorithm>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/math_utils.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Materials/CookTorranceBRDF.h"
#include "../src/Library/Materials/SchlickBRDF.h"
#include "../src/Library/Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	static int checks = 0;
	static int failures = 0;

	static bool Report( const std::string& label, const bool passed )
	{
		std::cout << "  " << std::left << std::setw( 62 ) << label
			<< ( passed ? "PASS" : "FAIL" ) << "\n";
		++checks;
		if( !passed ) ++failures;
		return passed;
	}

	// Same fixed synthetic hit convention as GGXSampleEvaluationConsistencyTest::MakeRI.
	static RayIntersectionGeometric MakeRI( const double thetaDeg, const double azimuthDeg, const Scalar filterWidth )
	{
		const double theta = thetaDeg * PI / 180.0;
		const double phi = azimuthDeg * PI / 180.0;
		const Vector3 view( std::sin(theta)*std::cos(phi), std::sin(theta)*std::sin(phi), std::cos(theta) );
		const Ray ray( Point3( view.x, view.y, view.z ), -view );
		const RasterizerState rs = { 0, 0 };
		RayIntersectionGeometric ri( ray, rs );
		ri.bHit = true;
		ri.range = 1.0;
		ri.ptIntersection = Point3( 0, 0, 0 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.vGeomNormal = Vector3( 0, 0, 1 );
		ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
		ri.ptCoord = Point2( 0.5, 0.5 );
		ri.ambientIOR = 1.0;
		ri.glossyFilterWidth = filterWidth;
		return ri;
	}

	static Vector3 DirectionFromAngles( const double thetaDeg, const double azimuthDeg )
	{
		const double theta = thetaDeg * PI / 180.0;
		const double phi = azimuthDeg * PI / 180.0;
		return Vector3( std::sin(theta)*std::cos(phi), std::sin(theta)*std::sin(phi), std::cos(theta) );
	}

	// ================================================================
	//  CookTorrance fixture
	// ================================================================
	struct CookTorranceFixture
	{
		UniformColorPainter* diffuse;
		UniformColorPainter* specular;
		UniformScalarPainter* masking;
		UniformScalarPainter* ior;
		UniformScalarPainter* ext;
		CookTorranceBRDF* brdf;

		CookTorranceFixture( const Scalar d, const Scalar f0, const Scalar alpha,
			const Scalar iorVal = 2.74, const Scalar extVal = 3.79 )
			: diffuse( new UniformColorPainter( RISEPel(d,d,d) ) ),
			  specular( new UniformColorPainter( RISEPel(f0,f0,f0) ) ),
			  masking( new UniformScalarPainter( alpha ) ),
			  ior( new UniformScalarPainter( iorVal ) ),
			  ext( new UniformScalarPainter( extVal ) ),
			  brdf( 0 )
		{
			diffuse->addref(); specular->addref(); masking->addref(); ior->addref(); ext->addref();
			brdf = new CookTorranceBRDF( *diffuse, *specular, *masking, *ior, *ext );
			brdf->addref();
		}

		~CookTorranceFixture()
		{
			brdf->release();
			diffuse->release(); specular->release(); masking->release(); ior->release(); ext->release();
		}
	};

	// ================================================================
	//  Schlick fixture
	// ================================================================
	struct SchlickFixture
	{
		UniformColorPainter* diffuse;
		UniformColorPainter* specular;
		UniformScalarPainter* roughness;
		UniformScalarPainter* isotropy;
		SchlickBRDF* brdf;

		SchlickFixture( const Scalar d, const Scalar f0, const Scalar r, const Scalar p = 0.5 )
			: diffuse( new UniformColorPainter( RISEPel(d,d,d) ) ),
			  specular( new UniformColorPainter( RISEPel(f0,f0,f0) ) ),
			  roughness( new UniformScalarPainter( r ) ),
			  isotropy( new UniformScalarPainter( p ) ),
			  brdf( 0 )
		{
			diffuse->addref(); specular->addref(); roughness->addref(); isotropy->addref();
			brdf = new SchlickBRDF( *diffuse, *specular, *roughness, *isotropy );
			brdf->addref();
		}

		~SchlickFixture()
		{
			brdf->release();
			diffuse->release(); specular->release(); roughness->release(); isotropy->release();
		}
	};

	// ================================================================
	//  CookTorranceBRDF::value must match a pre-widened reference
	// ================================================================
	static bool TestCookTorranceValueMatchesPrewidened(
		const char* label,
		const Scalar diffuseVal, const Scalar f0, const Scalar alphaRaw,
		const Scalar filterWidth, const double thetaDeg )
	{
		const Scalar effAlpha = r_min( alphaRaw + filterWidth, Scalar(1.0) );

		CookTorranceFixture raw( diffuseVal, f0, alphaRaw );
		CookTorranceFixture reference( diffuseVal, f0, effAlpha );

		const RayIntersectionGeometric riRaw = MakeRI( thetaDeg, 0.0, filterWidth );
		const RayIntersectionGeometric riRef = MakeRI( thetaDeg, 0.0, Scalar(0.0) );

		const double probeThetas[]   = { thetaDeg, 20.0, 55.0, 75.0 };
		const double probeAzimuths[] = { 180.0,    0.0,  90.0, 45.0 };

		double maxRelErr = 0.0;
		for( int i = 0; i < 4; ++i )
		{
			const Vector3 wo = DirectionFromAngles( probeThetas[i], probeAzimuths[i] );
			const RISEPel vRaw = raw.brdf->value( wo, riRaw );
			const RISEPel vRef = reference.brdf->value( wo, riRef );
			for( int c = 0; c < 3; ++c )
			{
				const double denom = std::max( 1e-12, std::fabs( vRef[c] ) );
				const double relErr = std::fabs( vRaw[c] - vRef[c] ) / denom;
				maxRelErr = std::max( maxRelErr, relErr );
			}
		}
		const bool passed = maxRelErr <= 1e-6;

		std::ostringstream oss;
		oss << label << " maxRelErr=" << std::scientific << std::setprecision(3) << maxRelErr;
		return Report( oss.str(), passed );
	}

	static bool TestCookTorranceValueNMMatchesPrewidened(
		const char* label,
		const Scalar diffuseVal, const Scalar f0, const Scalar alphaRaw,
		const Scalar filterWidth, const double thetaDeg, const double nm )
	{
		const Scalar effAlpha = r_min( alphaRaw + filterWidth, Scalar(1.0) );

		CookTorranceFixture raw( diffuseVal, f0, alphaRaw );
		CookTorranceFixture reference( diffuseVal, f0, effAlpha );

		const RayIntersectionGeometric riRaw = MakeRI( thetaDeg, 0.0, filterWidth );
		const RayIntersectionGeometric riRef = MakeRI( thetaDeg, 0.0, Scalar(0.0) );

		const double probeThetas[]   = { thetaDeg, 20.0, 55.0, 75.0 };
		const double probeAzimuths[] = { 180.0,    0.0,  90.0, 45.0 };

		double maxRelErr = 0.0;
		for( int i = 0; i < 4; ++i )
		{
			const Vector3 wo = DirectionFromAngles( probeThetas[i], probeAzimuths[i] );
			const Scalar vRaw = raw.brdf->valueNM( wo, riRaw, nm );
			const Scalar vRef = reference.brdf->valueNM( wo, riRef, nm );
			const double denom = std::max( 1e-12, std::fabs( vRef ) );
			const double relErr = std::fabs( vRaw - vRef ) / denom;
			maxRelErr = std::max( maxRelErr, relErr );
		}
		const bool passed = maxRelErr <= 1e-6;

		std::ostringstream oss;
		oss << label << " nm=" << nm << " maxRelErr=" << std::scientific << std::setprecision(3) << maxRelErr;
		return Report( oss.str(), passed );
	}

	// ================================================================
	//  SchlickBRDF::value must match a pre-widened reference
	// ================================================================
	static bool TestSchlickValueMatchesPrewidened(
		const char* label,
		const Scalar diffuseVal, const Scalar f0, const Scalar roughRaw,
		const Scalar filterWidth, const double thetaDeg )
	{
		const Scalar effRough = r_min( roughRaw + filterWidth, Scalar(1.0) );

		SchlickFixture raw( diffuseVal, f0, roughRaw );
		SchlickFixture reference( diffuseVal, f0, effRough );

		const RayIntersectionGeometric riRaw = MakeRI( thetaDeg, 0.0, filterWidth );
		const RayIntersectionGeometric riRef = MakeRI( thetaDeg, 0.0, Scalar(0.0) );

		const double probeThetas[]   = { thetaDeg, 20.0, 55.0, 75.0 };
		const double probeAzimuths[] = { 180.0,    0.0,  90.0, 45.0 };

		double maxRelErr = 0.0;
		for( int i = 0; i < 4; ++i )
		{
			const Vector3 wo = DirectionFromAngles( probeThetas[i], probeAzimuths[i] );
			const RISEPel vRaw = raw.brdf->value( wo, riRaw );
			const RISEPel vRef = reference.brdf->value( wo, riRef );
			for( int c = 0; c < 3; ++c )
			{
				const double denom = std::max( 1e-12, std::fabs( vRef[c] ) );
				const double relErr = std::fabs( vRaw[c] - vRef[c] ) / denom;
				maxRelErr = std::max( maxRelErr, relErr );
			}
		}
		const bool passed = maxRelErr <= 1e-6;

		std::ostringstream oss;
		oss << label << " maxRelErr=" << std::scientific << std::setprecision(3) << maxRelErr;
		return Report( oss.str(), passed );
	}

	static bool TestSchlickValueNMMatchesPrewidened(
		const char* label,
		const Scalar diffuseVal, const Scalar f0, const Scalar roughRaw,
		const Scalar filterWidth, const double thetaDeg, const double nm )
	{
		const Scalar effRough = r_min( roughRaw + filterWidth, Scalar(1.0) );

		SchlickFixture raw( diffuseVal, f0, roughRaw );
		SchlickFixture reference( diffuseVal, f0, effRough );

		const RayIntersectionGeometric riRaw = MakeRI( thetaDeg, 0.0, filterWidth );
		const RayIntersectionGeometric riRef = MakeRI( thetaDeg, 0.0, Scalar(0.0) );

		const double probeThetas[]   = { thetaDeg, 20.0, 55.0, 75.0 };
		const double probeAzimuths[] = { 180.0,    0.0,  90.0, 45.0 };

		double maxRelErr = 0.0;
		for( int i = 0; i < 4; ++i )
		{
			const Vector3 wo = DirectionFromAngles( probeThetas[i], probeAzimuths[i] );
			const Scalar vRaw = raw.brdf->valueNM( wo, riRaw, nm );
			const Scalar vRef = reference.brdf->valueNM( wo, riRef, nm );
			const double denom = std::max( 1e-12, std::fabs( vRef ) );
			const double relErr = std::fabs( vRaw - vRef ) / denom;
			maxRelErr = std::max( maxRelErr, relErr );
		}
		const bool passed = maxRelErr <= 1e-6;

		std::ostringstream oss;
		oss << label << " nm=" << nm << " maxRelErr=" << std::scientific << std::setprecision(3) << maxRelErr;
		return Report( oss.str(), passed );
	}
}

int main()
{
	std::cout << "=== CookTorrance/Schlick Glossy-Filter Consistency Test (DL-65) ===\n";
	GlobalLog();

	bool passed = true;

	std::cout << "\n--- DL-65: CookTorranceBRDF::value matches pre-widened reference (RGB) ---\n";
	passed &= TestCookTorranceValueMatchesPrewidened( "mixed W=0 (zero-filter control)", 0.3, 0.5, 0.2, 0.0, 30.0 );
	passed &= TestCookTorranceValueMatchesPrewidened( "mixed W=0.3",                     0.3, 0.5, 0.2, 0.3, 30.0 );
	passed &= TestCookTorranceValueMatchesPrewidened( "spec-only W=0.25",                0.0, 0.8, 0.1, 0.25, 45.0 );
	passed &= TestCookTorranceValueMatchesPrewidened( "grazing W=0.2",                   0.4, 0.3, 0.2, 0.2, 75.0 );
	passed &= TestCookTorranceValueMatchesPrewidened( "saturation W=0.7",                0.2, 0.6, 0.4, 0.7, 20.0 );

	std::cout << "\n--- DL-65: CookTorranceBRDF::valueNM matches pre-widened reference (NM) ---\n";
	for( const double nm : { 450.0, 550.0, 650.0 } )
	{
		passed &= TestCookTorranceValueNMMatchesPrewidened( "mixed W=0.3", 0.3, 0.5, 0.2, 0.3, 30.0, nm );
		passed &= TestCookTorranceValueNMMatchesPrewidened( "spec-only W=0.25", 0.0, 0.8, 0.1, 0.25, 45.0, nm );
	}

	std::cout << "\n--- DL-65: SchlickBRDF::value matches pre-widened reference (RGB) ---\n";
	passed &= TestSchlickValueMatchesPrewidened( "mixed W=0 (zero-filter control)", 0.3, 0.5, 0.2, 0.0, 30.0 );
	passed &= TestSchlickValueMatchesPrewidened( "mixed W=0.3",                     0.3, 0.5, 0.2, 0.3, 30.0 );
	passed &= TestSchlickValueMatchesPrewidened( "spec-only W=0.25",                0.0, 0.8, 0.1, 0.25, 45.0 );
	passed &= TestSchlickValueMatchesPrewidened( "grazing W=0.2",                   0.4, 0.3, 0.2, 0.2, 75.0 );
	passed &= TestSchlickValueMatchesPrewidened( "saturation W=0.7",                0.2, 0.6, 0.4, 0.7, 20.0 );

	std::cout << "\n--- DL-65: SchlickBRDF::valueNM matches pre-widened reference (NM) ---\n";
	for( const double nm : { 450.0, 550.0, 650.0 } )
	{
		passed &= TestSchlickValueNMMatchesPrewidened( "mixed W=0.3", 0.3, 0.5, 0.2, 0.3, 30.0, nm );
		passed &= TestSchlickValueNMMatchesPrewidened( "spec-only W=0.25", 0.0, 0.8, 0.1, 0.25, 45.0, nm );
	}

	std::cout << "\nCookTorranceSchlickGlossyFilterConsistencyTest: " << checks << " checks, " << failures << " failures\n";
	std::cout << "=== " << ( passed ? "ALL TESTS PASSED" : "TESTS FAILED" ) << " ===\n";
	return passed ? 0 : 1;
}
