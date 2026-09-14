//////////////////////////////////////////////////////////////////////
//
//  GGXSampleEvaluationConsistencyTest.cpp - DL-62 / DL-64 red proof.
//
//  DL-62 (glossy-filter roughness mismatch): GGXSPF::Scatter, ScatterNM,
//    Pdf and PdfNM widen alphaX/alphaY by ri.glossyFilterWidth before
//    sampling/density evaluation ("min(alpha + width, 1)"), but
//    GGXBRDF::value/valueNM only floor the authored (unfiltered) alpha.
//    A texture-filtered / mip-blurred continuation therefore samples one
//    surface roughness and NEE evaluates a different, sharper one at the
//    SAME shading point.
//
//    An aggregate energy-conservation check cannot see this: Kulla-Conty
//    multiscatter compensation keeps total reflected energy close to 1
//    across nearly the whole alpha range, for both the narrow and the
//    widened lobe.  The test below instead compares GGXBRDF::value /
//    valueNM DETERMINISTICALLY (no RNG, no MC noise) against a second,
//    independently-constructed GGXBRDF whose alpha painters already hold
//    the intended effective (pre-widened) roughness and whose
//    ri.glossyFilterWidth is left at 0 -- i.e. a reference that is
//    right by construction, independent of whether GGXBRDF's own
//    filter-width handling works.  The production object and the
//    reference must agree at every sampled direction once GGXBRDF widens
//    its alpha the same way GGXSPF already does.
//
//  DL-64 (zero-F0 Schlick sampling collapse): GGXSPF::Scatter/ScatterNM
//    (and Pdf/PdfNM) derive the specular+multiscatter lobe-selection
//    weight from MaxValue/GuardedGetColorNM of the raw authored F0.  At
//    F0=0 that weight is EXACTLY zero, so the specular lobe is never
//    chosen and (since the multiscatter weight is `ws*(1-Ess)`) neither
//    is the multiscatter lobe -- even though GGXBRDF::value/valueNM's
//    Schlick term F0 + (1-F0)(1-cosTheta)^5 is clearly nonzero at
//    grazing incidence when F0=0.  This is a fully deterministic
//    collapse (not a rare-sample effect): pDiffuseSelect is either 1
//    (wd>0) or the total<1e-10 fallback of 1 (wd==0 too), so EVERY
//    Scatter()/ScatterNM() draw takes the diffuse branch, and the
//    specular ScatteredRay type (eRayReflection) is never emitted,
//    regardless of RNG seed or sample count.  A GGXSPF::Pdf/PdfNM query
//    at the exact specular peak direction is likewise pinned to the
//    pure cosine density, ignoring the local VNDF spike entirely.
//
//    The fix must give F0=0 a nonzero (but still small, hemispherical)
//    selection weight, e.g. via GGXInterfaceFresnel::Mean()/MeanNM(),
//    which is nonzero at F0=0 (SchlickFresnelAvg(0) = 1/21).  This test
//    asserts the SPF actually samples/weights the specular lobe when
//    F0=0, at both grazing and normal incidence, with both zero and
//    nonzero diffuse, in RGB and NM.
//
//  Build (from project root):
//    c++ -arch arm64 -Isrc/Library -I/opt/homebrew/include
//        -O3 -ffast-math -fno-finite-math-only -funroll-loops -Wall -pedantic
//        -Wno-c++11-long-long -DCOLORS_RGB -DMERSENNE53
//        -DNO_TIFF_SUPPORT -DNO_EXR_SUPPORT -DRISE_ENABLE_MAILBOXING
//        -c tests/GGXSampleEvaluationConsistencyTest.cpp
//        -o tests/GGXSampleEvaluationConsistencyTest.o
//    c++ -arch arm64 -o tests/ggx_sample_eval_test
//        tests/GGXSampleEvaluationConsistencyTest.o bin/librise.a
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

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/MicrofacetUtils.h"
#include "../src/Library/Utilities/MicrofacetEnergyLUT.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Materials/GGXBRDF.h"
#include "../src/Library/Materials/GGXSPF.h"
#include "../src/Library/Interfaces/ILog.h"

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	static int checks = 0;
	static int failures = 0;
	static StubObject* g_stubObject = 0;

	static bool Report( const std::string& label, const bool passed )
	{
		std::cout << "  " << std::left << std::setw( 62 ) << label
			<< ( passed ? "PASS" : "FAIL" ) << "\n";
		++checks;
		if( !passed ) ++failures;
		return passed;
	}

	// Fixed synthetic hit at the world origin with a +Z shading/geometric
	// normal, matching the convention already used by GGXWhiteFurnaceTest
	// and GGXDiffuseTransmissionTest.  `theta`/`azimuth` place the
	// incoming ray's ORIGIN so ri.ray.Dir() points toward the surface
	// from that direction (view = -ray.Dir()).
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

	// Extended for P2-2 (2026-09-13 review follow-up) to also build
	// eFresnelConductor / eFresnelThinFilmConductor fixtures: `mode`,
	// `iorVal`/`extVal` (real substrate n/k for conductor and thin-film;
	// irrelevant garbage 1.5/0.0 for Schlick, unchanged from before) and
	// the film n/k/thickness triple (allocated unconditionally but only
	// PASSED to GGXBRDF/GGXSPF -- and only dereferenced inside
	// GGXInterfaceFresnel -- when mode == eFresnelThinFilmConductor).
	// Every pre-existing call site omits the new trailing parameters, so
	// defaults reproduce the old Schlick-only fixture exactly.
	struct GGXFixture
	{
		UniformColorPainter* diffuse;
		UniformColorPainter* specular;
		UniformScalarPainter* alphaX;
		UniformScalarPainter* alphaY;
		UniformScalarPainter* ior;
		UniformScalarPainter* ext;
		UniformScalarPainter* filmIor;
		UniformScalarPainter* filmExt;
		UniformScalarPainter* filmThk;
		GGXBRDF* brdf;
		GGXSPF*  spf;
		const FresnelMode mode;

		GGXFixture( const Scalar d, const Scalar f0, const Scalar ax, const Scalar ay,
			const FresnelMode fresnelMode = eFresnelSchlickF0,
			const Scalar iorVal = 1.5, const Scalar extVal = 0.0,
			const Scalar filmN = 2.5, const Scalar filmK = 0.0, const Scalar filmThicknessNm = 180.0 )
			: diffuse( new UniformColorPainter( RISEPel(d,d,d) ) ),
			  specular( new UniformColorPainter( RISEPel(f0,f0,f0) ) ),
			  alphaX( new UniformScalarPainter( ax ) ),
			  alphaY( new UniformScalarPainter( ay ) ),
			  ior( new UniformScalarPainter( iorVal ) ),
			  ext( new UniformScalarPainter( extVal ) ),
			  filmIor( new UniformScalarPainter( filmN ) ),
			  filmExt( new UniformScalarPainter( filmK ) ),
			  filmThk( new UniformScalarPainter( filmThicknessNm ) ),
			  brdf( 0 ), spf( 0 ), mode( fresnelMode )
		{
			diffuse->addref(); specular->addref(); alphaX->addref(); alphaY->addref();
			ior->addref(); ext->addref();
			filmIor->addref(); filmExt->addref(); filmThk->addref();

			const bool isThinFilm = (fresnelMode == eFresnelThinFilmConductor);
			const IScalarPainter* pFilmIor = isThinFilm ? filmIor : nullptr;
			const IScalarPainter* pFilmExt = isThinFilm ? filmExt : nullptr;
			const IScalarPainter* pFilmThk = isThinFilm ? filmThk : nullptr;

			brdf = new GGXBRDF( *diffuse, *specular, *alphaX, *alphaY, *ior, *ext, fresnelMode, nullptr, pFilmIor, pFilmExt, pFilmThk );
			brdf->addref();
			spf = new GGXSPF( *diffuse, *specular, *alphaX, *alphaY, *ior, *ext, fresnelMode, nullptr, pFilmIor, pFilmExt, pFilmThk );
			spf->addref();
		}

		~GGXFixture()
		{
			brdf->release(); spf->release();
			diffuse->release(); specular->release();
			alphaX->release(); alphaY->release();
			ior->release(); ext->release();
			filmIor->release(); filmExt->release(); filmThk->release();
		}
	};

	static Vector3 DirectionFromAngles( const double thetaDeg, const double azimuthDeg )
	{
		const double theta = thetaDeg * PI / 180.0;
		const double phi = azimuthDeg * PI / 180.0;
		return Vector3( std::sin(theta)*std::cos(phi), std::sin(theta)*std::sin(phi), std::cos(theta) );
	}

	// P3-3 (2026-09-13 review follow-up): deterministic sampler replaying
	// a fixed list of 1D draws, so two GGXSPF::Scatter() calls land on
	// the SAME lobe selection / half-vector (mirrors
	// ThinFilmBRDFTest.cpp's ScriptedSampler).
	class ScriptedSampler : public ISampler
	{
		const Scalar*	seq;
		unsigned int	n;
		unsigned int	idx;
		Scalar			tail;
	public:
		ScriptedSampler( const Scalar* s, unsigned int count, Scalar tailVal )
			: seq( s ), n( count ), idx( 0 ), tail( tailVal ) {}
		Scalar Get1D() { return idx < n ? seq[idx++] : tail; }
		Point2 Get2D() { return Point2( Get1D(), Get1D() ); }
		void StartStream( int ) {}
	};

	// ================================================================
	//  DL-62: GGXBRDF::value/valueNM must widen alpha by
	//  ri.glossyFilterWidth exactly the way GGXSPF already does.
	//
	//  `raw` uses the AUTHORED (narrow) alpha painters with
	//  glossyFilterWidth = W (the production configuration under test).
	//  `reference` uses PRE-WIDENED alpha painters (min(alpha+W,1),
	//  computed here independently of GGXBRDF/GGXSPF) with
	//  glossyFilterWidth = 0, so it evaluates the intended effective
	//  roughness regardless of whether GGXBRDF's own filter-width
	//  handling is implemented.  Once GGXBRDF widens consistently with
	//  GGXSPF, `raw` and `reference` must agree at every direction.
	// ================================================================

	static bool TestGlossyFilterValueMatchesPrewidened(
		const char* label,
		const Scalar diffuseVal,
		const Scalar f0,
		const Scalar alphaXRaw,
		const Scalar alphaYRaw,
		const Scalar filterWidth,
		const double thetaDeg )
	{
		const Scalar effAlphaX = r_min( alphaXRaw + filterWidth, Scalar(1.0) );
		const Scalar effAlphaY = r_min( alphaYRaw + filterWidth, Scalar(1.0) );

		GGXFixture raw( diffuseVal, f0, alphaXRaw, alphaYRaw );
		GGXFixture reference( diffuseVal, f0, effAlphaX, effAlphaY );

		const RayIntersectionGeometric riRaw = MakeRI( thetaDeg, 0.0, filterWidth );
		const RayIntersectionGeometric riRef = MakeRI( thetaDeg, 0.0, Scalar(0.0) );

		// Probe a handful of outgoing directions: the mirror-reflection
		// peak (where D(h) is most sensitive to alpha) plus a few
		// off-peak directions spanning the upper hemisphere -- this also
		// exercises the Kulla-Conty multiscatter term, which depends on
		// alphaEff too.
		const double probeThetas[]   = { thetaDeg, 20.0, 55.0, 75.0 };
		const double probeAzimuths[] = { 180.0,    0.0,  90.0, 45.0 };

		bool passed = true;
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
		if( maxRelErr > 1e-6 ) passed = false;

		std::ostringstream oss;
		oss << label << " maxRelErr=" << std::scientific << std::setprecision(3) << maxRelErr;
		return Report( oss.str(), passed );
	}

	static bool TestGlossyFilterValueMatchesPrewidenedNM(
		const char* label,
		const Scalar diffuseVal,
		const Scalar f0,
		const Scalar alphaXRaw,
		const Scalar alphaYRaw,
		const Scalar filterWidth,
		const double thetaDeg,
		const double nm )
	{
		const Scalar effAlphaX = r_min( alphaXRaw + filterWidth, Scalar(1.0) );
		const Scalar effAlphaY = r_min( alphaYRaw + filterWidth, Scalar(1.0) );

		GGXFixture raw( diffuseVal, f0, alphaXRaw, alphaYRaw );
		GGXFixture reference( diffuseVal, f0, effAlphaX, effAlphaY );

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

	// Sanity control: GGXSPF::Pdf already widens by glossyFilterWidth, so
	// it should ALREADY agree with the pre-widened reference (both before
	// and after the GGXBRDF fix).  This isolates the divergence to
	// GGXBRDF::value/valueNM specifically.
	static bool TestGlossyFilterSPFAlreadyConsistent(
		const Scalar diffuseVal, const Scalar f0,
		const Scalar alphaXRaw, const Scalar alphaYRaw,
		const Scalar filterWidth, const double thetaDeg )
	{
		const Scalar effAlphaX = r_min( alphaXRaw + filterWidth, Scalar(1.0) );
		const Scalar effAlphaY = r_min( alphaYRaw + filterWidth, Scalar(1.0) );

		GGXFixture raw( diffuseVal, f0, alphaXRaw, alphaYRaw );
		GGXFixture reference( diffuseVal, f0, effAlphaX, effAlphaY );

		const RayIntersectionGeometric riRaw = MakeRI( thetaDeg, 0.0, filterWidth );
		const RayIntersectionGeometric riRef = MakeRI( thetaDeg, 0.0, Scalar(0.0) );

		IORStack iorStack = MakeTestIORStack( g_stubObject );
		const Vector3 wo = DirectionFromAngles( thetaDeg, 180.0 );	// near mirror peak

		const Scalar pdfRaw = raw.spf->Pdf( riRaw, wo, iorStack );
		const Scalar pdfRef = reference.spf->Pdf( riRef, wo, iorStack );
		const double denom = std::max( 1e-12, std::fabs( pdfRef ) );
		const double relErr = std::fabs( pdfRaw - pdfRef ) / denom;

		std::ostringstream oss;
		oss << "SPF Pdf already glossy-filter-consistent (control) relErr=" << std::scientific << std::setprecision(3) << relErr;
		return Report( oss.str(), relErr <= 1e-6 );
	}

	// P3-3 (2026-09-13 review follow-up): DL-62's checks so far only
	// probe GGXBRDF::value/valueNM and GGXSPF::Pdf under
	// glossyFilterWidth>0; add a GGXSPF::Scatter() case too.  diffuse=0
	// forces pDiffuseSelect=0 EXACTLY, so a scripted uLobe=0.0
	// deterministically selects the specular branch in both the raw
	// (authored alpha + real filter width) and reference (pre-widened
	// alpha, filterWidth=0) fixtures; identical subsequent u1/u2 draws
	// then sample the SAME half-vector in both (VNDF_Sample_Aniso is a
	// pure function of alphaX/alphaY/u1/u2, and both fixtures now carry
	// the same effective alpha) -- so the produced specular kray and
	// mixPdf must match EXACTLY, proving the widened roughness used for
	// SAMPLING and the widened roughness now used for value()
	// evaluation stay in lockstep end-to-end, not only at Pdf() queries.
	static bool TestGlossyFilterScatterMatchesPrewidened(
		const char* label,
		const Scalar f0,
		const Scalar alphaXRaw,
		const Scalar alphaYRaw,
		const Scalar filterWidth,
		const double thetaDeg )
	{
		const Scalar effAlphaX = r_min( alphaXRaw + filterWidth, Scalar(1.0) );
		const Scalar effAlphaY = r_min( alphaYRaw + filterWidth, Scalar(1.0) );

		GGXFixture raw( Scalar(0.0), f0, alphaXRaw, alphaYRaw );
		GGXFixture reference( Scalar(0.0), f0, effAlphaX, effAlphaY );

		const RayIntersectionGeometric riRaw = MakeRI( thetaDeg, 0.0, filterWidth );
		const RayIntersectionGeometric riRef = MakeRI( thetaDeg, 0.0, Scalar(0.0) );
		IORStack iorStack = MakeTestIORStack( g_stubObject );

		const Scalar draws[] = { 0.0, 0.37, 0.61 };	// uLobe=0 -> specular; u1,u2 -> VNDF sample
		ScriptedSampler sRaw( draws, 3, 0.5 );
		ScriptedSampler sRef( draws, 3, 0.5 );

		ScatteredRayContainer scRaw, scRef;
		raw.spf->Scatter( riRaw, sRaw, scRaw, iorStack );
		reference.spf->Scatter( riRef, sRef, scRef, iorStack );

		int iRaw = -1, iRef = -1;
		for( unsigned int k = 0; k < scRaw.Count(); ++k )
			if( scRaw[k].type == ScatteredRay::eRayReflection ) { iRaw = (int)k; break; }
		for( unsigned int k = 0; k < scRef.Count(); ++k )
			if( scRef[k].type == ScatteredRay::eRayReflection ) { iRef = (int)k; break; }

		if( iRaw < 0 || iRef < 0 )
			return Report( std::string(label) + " (no specular ray emitted -- invalid test config)", false );

		double maxRelErr = 0.0;
		for( int c = 0; c < 3; ++c )
		{
			const double denom = std::max( 1e-12, std::fabs( scRef[iRef].kray[c] ) );
			maxRelErr = std::max( maxRelErr, std::fabs( scRaw[iRaw].kray[c] - scRef[iRef].kray[c] ) / denom );
		}
		const double pdfDenom = std::max( 1e-12, std::fabs( scRef[iRef].pdf ) );
		const double pdfRelErr = std::fabs( scRaw[iRaw].pdf - scRef[iRef].pdf ) / pdfDenom;
		maxRelErr = std::max( maxRelErr, pdfRelErr );

		std::ostringstream oss;
		oss << label << " maxRelErr=" << std::scientific << std::setprecision(3) << maxRelErr;
		return Report( oss.str(), maxRelErr <= 1e-6 );
	}

	// P3-3 (2026-09-13 review follow-up): authored-alpha-below-floor +
	// glossyFilterWidth pins the FLOOR-THEN-WIDEN ordering both
	// GGXBRDF::value/valueNM and GGXSPF's four sample/density sites use:
	// `alpha = max(authored, 1e-4); if (filterWidth>0) alpha =
	// min(alpha + filterWidth, 1)`.  At an authored alpha below the
	// floor (0.0) and a filterWidth SMALLER than the floor itself
	// (5e-5), floor-then-widen and widen-then-floor diverge measurably:
	// floor-then-widen gives 1e-4+5e-5=1.05e-4; widen-then-floor gives
	// max(5e-5,1e-4)=1.0e-4 -- a ~5% difference in alphaEff, far above
	// the 1e-6 tolerance used throughout this file.  The reference below
	// computes floor-then-widen independently (not by calling
	// GGXBRDF/GGXSPF) and asserts the two orderings actually diverge for
	// this config first (so a coincidental numeric match can't hide a
	// broken assertion), then checks production `raw.brdf->value()`
	// (authored alpha 0.0 + real filterWidth, which must floor-then-
	// widen internally) against a fixture built directly from the
	// pre-computed floor-then-widen effective alpha (filterWidth=0).
	static bool TestGlossyFilterFloorThenWidenOrdering()
	{
		const Scalar authoredAlpha = Scalar(0.0);	// below the 1e-4 floor
		const Scalar filterWidth   = Scalar(0.00005);	// smaller than the floor itself
		const Scalar flooredAlpha  = r_max( authoredAlpha, Scalar(1e-4) );
		const Scalar effAlphaFloorThenWiden = r_min( flooredAlpha + filterWidth, Scalar(1.0) );
		const Scalar effAlphaWidenThenFloor = r_max( r_min( authoredAlpha + filterWidth, Scalar(1.0) ), Scalar(1e-4) );
		if( std::fabs( double(effAlphaFloorThenWiden) - double(effAlphaWidenThenFloor) ) < 1e-8 )
			return Report( "floor-then-widen ordering probe (orderings coincide -- invalid test config)", false );

		GGXFixture raw( Scalar(0.3), Scalar(0.5), authoredAlpha, authoredAlpha );
		GGXFixture reference( Scalar(0.3), Scalar(0.5), effAlphaFloorThenWiden, effAlphaFloorThenWiden );

		const RayIntersectionGeometric riRaw = MakeRI( 30.0, 0.0, filterWidth );
		const RayIntersectionGeometric riRef = MakeRI( 30.0, 0.0, Scalar(0.0) );
		const Vector3 wo = DirectionFromAngles( 30.0, 180.0 );	// mirror peak

		const RISEPel vRaw = raw.brdf->value( wo, riRaw );
		const RISEPel vRef = reference.brdf->value( wo, riRef );

		double maxRelErr = 0.0;
		for( int c = 0; c < 3; ++c )
		{
			const double denom = std::max( 1e-12, std::fabs( vRef[c] ) );
			maxRelErr = std::max( maxRelErr, std::fabs( vRaw[c] - vRef[c] ) / denom );
		}

		std::ostringstream oss;
		oss << "authored alpha below floor + W=" << double(filterWidth) << " (floor-then-widen ordering) maxRelErr="
			<< std::scientific << std::setprecision(3) << maxRelErr;
		return Report( oss.str(), maxRelErr <= 1e-6 );
	}

	// ================================================================
	//  DL-64: at F0=0, GGXSPF must still sample the specular/MS lobes
	//  some of the time (the actual Fresnel term is nonzero at grazing).
	// ================================================================

	// P3-2 companion: the SAME mode-gated `ws` formula, but returning the
	// LOBE-SELECTION PROBABILITY pSpecSelect = ws/total (independent of
	// wo) rather than the mixture Pdf at a specific outgoing direction --
	// this is the quantity GGXSPF::Scatter/ScatterNM actually compare
	// `uLobe` against, so it is what an empirical specular-draw fraction
	// should be checked against (not a Pdf value).
	static Scalar ExpectedGGXPSpecSelect(
		const RayIntersectionGeometric& ri,
		const IPainter& diffusePainter,
		const IPainter& specularPainter,
		const IScalarPainter& iorPainter,
		const IScalarPainter& extPainter,
		const Scalar alphaX,
		const Scalar alphaY,
		const FresnelMode mode = eFresnelSchlickF0 )
	{
		const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );
		const Vector3 n = ri.onb.w();
		const Scalar alphaEff = sqrt( alphaX * alphaY );
		const GGXInterfaceFresnel interfaceFresnel{ ri, mode, specularPainter, iorPainter, extPainter, 0, 0, 0 };

		const Scalar wd = ColorMath::MaxValue( diffusePainter.GetColor(ri) );
		const Scalar ws = (mode == eFresnelSchlickF0)
			? ColorMath::MaxValue( interfaceFresnel.Mean() )
			: ColorMath::MaxValue( specularPainter.GetColor(ri) );
		const Scalar cosWi = Vector3Ops::Dot( wi, n );
		const Scalar wms = ws * ( Scalar(1.0) - MicrofacetEnergyLUT::LookupEss( cosWi, alphaEff ) );
		const Scalar total = wd + ws + wms;
		return (total > Scalar(1e-10)) ? ws / total : Scalar(0);
	}

	static bool TestZeroF0SpecularIsSampled(
		const Scalar diffuseVal,
		const Scalar alphaX,
		const Scalar alphaY,
		const double thetaDeg,
		const unsigned int seed )
	{
		GGXFixture fixture( diffuseVal, Scalar(0.0), alphaX, alphaY );
		const RayIntersectionGeometric ri = MakeRI( thetaDeg, 0.0, Scalar(0.0) );

		RandomNumberGenerator rng( seed );
		IndependentSampler sampler( rng );
		IORStack iorStack = MakeTestIORStack( g_stubObject );

		const int kSamples = 20000;
		int specularCount = 0;
		double specularEnergy = 0.0;
		const Vector3 n = ri.onb.w();

		for( int i = 0; i < kSamples; ++i )
		{
			ScatteredRayContainer scattered;
			fixture.spf->Scatter( ri, sampler, scattered, iorStack );
			for( unsigned int r = 0; r < scattered.Count(); ++r )
			{
				const ScatteredRay& scat = scattered[r];
				if( scat.type == ScatteredRay::eRayReflection && ColorMath::MaxValue( scat.kray ) > 0 )
				{
					++specularCount;
					const Scalar cosWo = Vector3Ops::Dot( scat.ray.Dir(), n );
					specularEnergy += ColorMath::MaxValue( scat.kray ) * cosWo;
				}
			}
		}

		// P3-2 (2026-09-13 review follow-up): beyond "reachable at all",
		// pin the empirical specular-draw FRACTION against the expected
		// selection probability pSpecSelect = ws/total (the same
		// mode-gated formula GGXSPF::Scatter compares `uLobe` against).
		// Band derivation: under the null hypothesis phat==pSpecSelect,
		// the binomial standard error is SE=sqrt(p(1-p)/N); at the
		// largest pSpecSelect these callers probe (~0.4) with N=20000,
		// 6*SE ~= 0.0208.  A further +0.03 additive slack absorbs a real
		// (non-defect) bias: the specular branch can be SELECTED by
		// `uLobe` yet still emit zero rays when the sampled half-vector
		// reflects wi to a below-geometric-horizon wo (ordinary VNDF
		// rejection), which under-counts specularCount relative to
		// pSpecSelect.  Both together (<=0.05 at worst) remain two
		// orders of magnitude tighter than the gap a rejected epsilon-
		// hack weight would leave: the doc's "naive bound" section
		// measures the pre-DL-64 JH-black-uplift epsilon at ~2.5e-5,
		// versus a real hemispherical-average pSpecSelect of ~0.02-0.4
		// across these configs -- an epsilon-sized weight could not
		// land inside this band.
		const Scalar pSpecSelect = ExpectedGGXPSpecSelect(
			ri, *fixture.diffuse, *fixture.specular, *fixture.ior, *fixture.ext, alphaX, alphaY, eFresnelSchlickF0 );
		const double phat = double(specularCount) / double(kSamples);
		const double se = std::sqrt( std::max( 1e-12, double(pSpecSelect) * (1.0 - double(pSpecSelect)) ) / double(kSamples) );
		const double band = 6.0 * se + 0.03;
		const double fracErr = std::fabs( phat - double(pSpecSelect) );
		const bool fractionOk = fracErr <= band;

		std::ostringstream oss;
		oss << "F0=0 diffuse=" << diffuseVal << " theta=" << thetaDeg
			<< " specular draws=" << specularCount << "/" << kSamples
			<< " energy=" << std::fixed << std::setprecision(5) << (specularEnergy / kSamples)
			<< " phat=" << std::setprecision(4) << phat << " pSpecSelect=" << pSpecSelect
			<< " |err|=" << fracErr << " band=" << band;
		// The Schlick lobe at F0=0 has real grazing reflectance; the
		// specular ScatteredRay type must be reachable at all, AND the
		// draw rate must match the expected selection probability.
		return Report( oss.str(), specularCount > 0 && fractionOk );
	}

	// Independent reference: recomputes GGXSPF::Pdf/PdfNM's mixture
	// formula from the shared public MicrofacetUtils/MicrofacetEnergyLUT
	// primitives GGXSPF itself is built from (not by calling GGXSPF), but
	// derives the specular/MS selection weight from
	// GGXInterfaceFresnel::Mean()/MeanNM() -- the lobe's actual
	// hemispherical Fresnel-weighted albedo -- instead of raw F0.  This
	// pins the SPECIFIC weight the fix must adopt: an EXACT match here
	// (not just "elevated over cosine") is required because at F0=0 the
	// authored specular painter is not literally exact-zero after JH
	// spectral uplift (UniformColorPainter's black uplifts to ~2.5e-5 at
	// 450/550/650nm here, a "black guard" gap independent of DL-64/DL-62
	// and out of scope for this slice) -- so a loose ">cosine" bound is
	// already satisfied by that unrelated epsilon times a large VNDF
	// spike, and would pass BEFORE the fix too.  An exact match to the
	// Mean()-weighted formula is not satisfiable by the epsilon alone.
	// P2-1 (2026-09-13 review follow-up): `ws` uses the hemispherical
	// Fresnel-weighted average (GGXInterfaceFresnel::Mean()/MeanNM())
	// ONLY in eFresnelSchlickF0 mode; eFresnelConductor and
	// eFresnelThinFilmConductor keep the raw painter tint, mirroring
	// GGXSPF::Pdf/PdfNM's corrected formula exactly (see GGXSPF.cpp).
	// `filmIorPainter`/`filmExtPainter`/`filmThkPainter` are only
	// dereferenced (inside GGXInterfaceFresnel) when mode is thin-film;
	// pass nullptr for the other two modes.
	static Scalar ExpectedGGXPdfHemisphericalWeight(
		const RayIntersectionGeometric& ri,
		const Vector3& wo,
		const IPainter& diffusePainter,
		const IPainter& specularPainter,
		const IScalarPainter& iorPainter,
		const IScalarPainter& extPainter,
		const IScalarPainter* filmIorPainter,
		const IScalarPainter* filmExtPainter,
		const IScalarPainter* filmThkPainter,
		const FresnelMode mode,
		const Scalar alphaX,
		const Scalar alphaY,
		const bool spectral,
		const Scalar nm )
	{
		OrthonormalBasis3D myonb = ri.onb;
		if( Vector3Ops::Dot( ri.ray.Dir(), ri.onb.w() ) > NEARZERO ) myonb.FlipW();
		const Vector3 n = myonb.w();
		const Vector3 woNorm = Vector3Ops::Normalize( wo );
		const Scalar cosTheta = Vector3Ops::Dot( woNorm, n );
		if( cosTheta <= 0 ) return 0;
		const Vector3 wi = Vector3Ops::Normalize( -(ri.ray.Dir()) );
		const Scalar alphaEff = sqrt( alphaX * alphaY );

		const GGXInterfaceFresnel interfaceFresnel{ ri, mode, specularPainter, iorPainter, extPainter, filmIorPainter, filmExtPainter, filmThkPainter };

		const Scalar wd = spectral ? GuardedGetColorNM( diffusePainter, ri, nm ) : ColorMath::MaxValue( diffusePainter.GetColor(ri) );
		Scalar ws;
		if( mode == eFresnelSchlickF0 )
			ws = spectral ? interfaceFresnel.MeanNM( nm ) : ColorMath::MaxValue( interfaceFresnel.Mean() );
		else
			ws = spectral ? GuardedGetColorNM( specularPainter, ri, nm ) : ColorMath::MaxValue( specularPainter.GetColor(ri) );

		const Scalar cosWi = Vector3Ops::Dot( wi, n );
		const Scalar wms = ws * ( Scalar(1.0) - MicrofacetEnergyLUT::LookupEss( cosWi, alphaEff ) );
		const Scalar total = wd + ws + wms;
		if( total < Scalar(1e-10) ) return cosTheta * INV_PI;

		const Scalar diffPdf = cosTheta * INV_PI;
		const Scalar specPdf = (alphaEff >= Scalar(1e-6)) ?
			MicrofacetUtils::VNDF_Pdf_Aniso( wi, woNorm, myonb, alphaX, alphaY ) : Scalar(0);
		const Scalar msPdfHere = MicrofacetEnergyLUT::MSPdf( cosTheta, alphaEff, MicrofacetEnergyLUT::MSLobeZ( alphaEff ) );

		return (wd * diffPdf + wms * msPdfHere + ws * specPdf) / total;
	}


	// Deterministic (no RNG): GGXSPF::Pdf at the exact mirror-reflection
	// direction must match the hemispherical-weight reference above, not
	// the raw-F0-weighted formula GGXSPF currently uses.
	static bool TestZeroF0PdfMatchesHemisphericalWeight(
		const Scalar diffuseVal,
		const Scalar alphaX,
		const Scalar alphaY,
		const double thetaDeg )
	{
		GGXFixture fixture( diffuseVal, Scalar(0.0), alphaX, alphaY );
		const RayIntersectionGeometric ri = MakeRI( thetaDeg, 0.0, Scalar(0.0) );
		IORStack iorStack = MakeTestIORStack( g_stubObject );

		const Vector3 wo = DirectionFromAngles( thetaDeg, 180.0 );	// mirror-reflection peak
		const Scalar pdf = fixture.spf->Pdf( ri, wo, iorStack );
		const Scalar expected = ExpectedGGXPdfHemisphericalWeight(
			ri, wo, *fixture.diffuse, *fixture.specular, *fixture.ior, *fixture.ext,
			nullptr, nullptr, nullptr, eFresnelSchlickF0, alphaX, alphaY, false, 0.0 );

		const double denom = std::max( 1e-12, std::fabs( expected ) );
		const double relErr = std::fabs( pdf - expected ) / denom;

		std::ostringstream oss;
		oss << "F0=0 diffuse=" << diffuseVal << " Pdf-at-peak theta=" << thetaDeg
			<< " pdf=" << std::fixed << std::setprecision(6) << pdf << " expected=" << expected
			<< " relErr=" << std::scientific << std::setprecision(3) << relErr;
		return Report( oss.str(), relErr <= 1e-6 );
	}

	// NM companion, using PdfNM.  diffuseVal is kept clearly nonzero
	// (0.01 / 0.3, never literal 0.0): UniformColorPainter's spectral
	// path eagerly JH-uplifts even an authored (0,0,0), and that uplift
	// is not exactly zero (see the epsilon note above) -- keeping wd
	// unambiguously dominated by a real diffuse albedo, rather than by
	// that epsilon too, keeps this an F0-selection-weight test and not
	// an accidental probe of the separate black-uplift gap.
	static bool TestZeroF0PdfMatchesHemisphericalWeightNM(
		const Scalar diffuseVal,
		const Scalar alphaX,
		const Scalar alphaY,
		const double thetaDeg,
		const double nm )
	{
		GGXFixture fixture( diffuseVal, Scalar(0.0), alphaX, alphaY );
		const RayIntersectionGeometric ri = MakeRI( thetaDeg, 0.0, Scalar(0.0) );
		IORStack iorStack = MakeTestIORStack( g_stubObject );

		const Vector3 wo = DirectionFromAngles( thetaDeg, 180.0 );
		const Scalar pdf = fixture.spf->PdfNM( ri, wo, nm, iorStack );
		const Scalar expected = ExpectedGGXPdfHemisphericalWeight(
			ri, wo, *fixture.diffuse, *fixture.specular, *fixture.ior, *fixture.ext,
			nullptr, nullptr, nullptr, eFresnelSchlickF0, alphaX, alphaY, true, nm );

		const double denom = std::max( 1e-12, std::fabs( expected ) );
		const double relErr = std::fabs( pdf - expected ) / denom;

		std::ostringstream oss;
		oss << "NM F0=0 diffuse=" << diffuseVal << " Pdf-at-peak theta=" << thetaDeg << " nm=" << nm
			<< " pdf=" << std::fixed << std::setprecision(6) << pdf << " expected=" << expected
			<< " relErr=" << std::scientific << std::setprecision(3) << relErr;
		return Report( oss.str(), relErr <= 1e-6 );
	}

	// Sanity control (not itself a red proof): GGXBRDF::valueNM's Schlick
	// term already retains a nonzero grazing reflection at F0=0 -- HWSS
	// companion wavelengths route through valueNM (GGXSPF does not
	// override EvaluateKrayNM), so they were never affected by DL-64.
	static bool TestZeroF0ValueNMGrazingIsNonzero( const double nm )
	{
		GGXFixture fixture( Scalar(0.3), Scalar(0.0), Scalar(0.2), Scalar(0.2) );
		const RayIntersectionGeometric ri = MakeRI( 80.0, 0.0, Scalar(0.0) );
		const Vector3 wo = DirectionFromAngles( 80.0, 180.0 );	// near mirror peak
		const Scalar v = fixture.brdf->valueNM( wo, ri, nm );

		std::ostringstream oss;
		oss << "HWSS companion control: valueNM grazing F0=0 nm=" << nm
			<< " value=" << std::fixed << std::setprecision(6) << v;
		return Report( oss.str(), v > 0 );
	}

	// ================================================================
	//  P2-2 (2026-09-13 review follow-up): the DL62/DL64 doc's sibling-
	//  audit sentence claimed "verified by the conductor/thin-film RGB
	//  and NM probes in the red-proof test" -- no such probes existed
	//  (only eFresnelSchlickF0 was ever constructed above).  These pin
	//  GGXSPF::Pdf/PdfNM's mixture formula in BOTH non-Schlick modes
	//  against ExpectedGGXPdfHemisphericalWeight's independent
	//  reference (P2-1-corrected: `ws` is the RAW painter tint in these
	//  two modes, not GGXInterfaceFresnel::Mean()/MeanNM()) -- a
	//  regression guard against reintroducing the unconditional Mean()/
	//  MeanNM() call P2-1 removed for performance
	//  (docs/DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md "Cost"), and against
	//  Scatter()/Pdf() mixture-identity drift in these modes.
	// ================================================================

	static bool TestNonSchlickPdfMatchesExpectedWeight(
		const char* label,
		const FresnelMode mode,
		const Scalar diffuseVal,
		const Scalar tint,
		const Scalar alphaX,
		const Scalar alphaY,
		const double thetaDeg,
		const Scalar iorVal,
		const Scalar extVal,
		const Scalar filmN = 2.5,
		const Scalar filmK = 0.0,
		const Scalar filmThicknessNm = 180.0 )
	{
		GGXFixture fixture( diffuseVal, tint, alphaX, alphaY, mode, iorVal, extVal, filmN, filmK, filmThicknessNm );
		const RayIntersectionGeometric ri = MakeRI( thetaDeg, 0.0, Scalar(0.0) );
		IORStack iorStack = MakeTestIORStack( g_stubObject );

		const Vector3 wo = DirectionFromAngles( thetaDeg, 180.0 );	// mirror-reflection peak
		const Scalar pdf = fixture.spf->Pdf( ri, wo, iorStack );
		const Scalar expected = ExpectedGGXPdfHemisphericalWeight(
			ri, wo, *fixture.diffuse, *fixture.specular, *fixture.ior, *fixture.ext,
			fixture.filmIor, fixture.filmExt, fixture.filmThk, mode, alphaX, alphaY, false, 0.0 );

		const double denom = std::max( 1e-12, std::fabs( expected ) );
		const double relErr = std::fabs( pdf - expected ) / denom;

		std::ostringstream oss;
		oss << label << " theta=" << thetaDeg
			<< " pdf=" << std::fixed << std::setprecision(6) << pdf << " expected=" << expected
			<< " relErr=" << std::scientific << std::setprecision(3) << relErr;
		return Report( oss.str(), relErr <= 1e-6 );
	}

	static bool TestNonSchlickPdfMatchesExpectedWeightNM(
		const char* label,
		const FresnelMode mode,
		const Scalar diffuseVal,
		const Scalar tint,
		const Scalar alphaX,
		const Scalar alphaY,
		const double thetaDeg,
		const double nm,
		const Scalar iorVal,
		const Scalar extVal,
		const Scalar filmN = 2.5,
		const Scalar filmK = 0.0,
		const Scalar filmThicknessNm = 180.0 )
	{
		GGXFixture fixture( diffuseVal, tint, alphaX, alphaY, mode, iorVal, extVal, filmN, filmK, filmThicknessNm );
		const RayIntersectionGeometric ri = MakeRI( thetaDeg, 0.0, Scalar(0.0) );
		IORStack iorStack = MakeTestIORStack( g_stubObject );

		const Vector3 wo = DirectionFromAngles( thetaDeg, 180.0 );
		const Scalar pdf = fixture.spf->PdfNM( ri, wo, nm, iorStack );
		const Scalar expected = ExpectedGGXPdfHemisphericalWeight(
			ri, wo, *fixture.diffuse, *fixture.specular, *fixture.ior, *fixture.ext,
			fixture.filmIor, fixture.filmExt, fixture.filmThk, mode, alphaX, alphaY, true, nm );

		const double denom = std::max( 1e-12, std::fabs( expected ) );
		const double relErr = std::fabs( pdf - expected ) / denom;

		std::ostringstream oss;
		oss << label << " nm=" << nm << " theta=" << thetaDeg
			<< " pdf=" << std::fixed << std::setprecision(6) << pdf << " expected=" << expected
			<< " relErr=" << std::scientific << std::setprecision(3) << relErr;
		return Report( oss.str(), relErr <= 1e-6 );
	}
}

int main()
{
	std::cout << "=== GGX Sample/Evaluation Consistency Test (DL-62, DL-64) ===\n";
	GlobalLog();

	g_stubObject = new StubObject();
	g_stubObject->addref();

	bool passed = true;

	std::cout << "\n--- DL-62: GGXBRDF::value matches pre-widened reference (RGB) ---\n";
	passed &= TestGlossyFilterValueMatchesPrewidened( "aniso mixed W=0 (zero-filter control)", 0.3, 0.5, 0.05, 0.30, 0.0, 30.0 );
	passed &= TestGlossyFilterValueMatchesPrewidened( "aniso mixed W=0.3",                       0.3, 0.5, 0.05, 0.30, 0.3, 30.0 );
	passed &= TestGlossyFilterValueMatchesPrewidened( "aniso spec-only W=0.25",                  0.0, 0.8, 0.10, 0.40, 0.25, 45.0 );
	passed &= TestGlossyFilterValueMatchesPrewidened( "iso mixed W=0.2 grazing",                 0.4, 0.3, 0.20, 0.20, 0.2, 75.0 );
	passed &= TestGlossyFilterValueMatchesPrewidened( "asymmetric saturation W=0.7 (Y saturates)",0.2, 0.6, 0.05, 0.40, 0.7, 20.0 );
	passed &= TestGlossyFilterValueMatchesPrewidened( "both axes saturate W=0.9",                0.2, 0.6, 0.60, 0.60, 0.9, 20.0 );

	std::cout << "\n--- DL-62: GGXBRDF::valueNM matches pre-widened reference (NM) ---\n";
	for( const double nm : { 450.0, 550.0, 650.0 } )
	{
		passed &= TestGlossyFilterValueMatchesPrewidenedNM( "NM aniso mixed W=0.3", 0.3, 0.5, 0.05, 0.30, 0.3, 30.0, nm );
		passed &= TestGlossyFilterValueMatchesPrewidenedNM( "NM spec-only W=0.4",  0.0, 0.8, 0.10, 0.40, 0.4, 60.0, nm );
	}

	std::cout << "\n--- DL-62: SPF Pdf already glossy-filter-consistent (control) ---\n";
	passed &= TestGlossyFilterSPFAlreadyConsistent( 0.3, 0.5, 0.05, 0.30, 0.3, 30.0 );
	passed &= TestGlossyFilterSPFAlreadyConsistent( 0.0, 0.8, 0.10, 0.40, 0.25, 45.0 );

	std::cout << "\n--- P3-3: GGXSPF::Scatter() under glossyFilterWidth>0 matches pre-widened reference ---\n";
	passed &= TestGlossyFilterScatterMatchesPrewidened( "Scatter spec-only aniso W=0.3", 0.7, 0.05, 0.30, 0.3, 40.0 );
	passed &= TestGlossyFilterScatterMatchesPrewidened( "Scatter spec-only W=0.6 grazing", 0.5, 0.15, 0.15, 0.6, 70.0 );

	std::cout << "\n--- P3-3: authored alpha below the 1e-4 floor + glossyFilterWidth (floor-then-widen ordering) ---\n";
	passed &= TestGlossyFilterFloorThenWidenOrdering();

	std::cout << "\n--- DL-64: specular lobe is reachable at F0=0 (RGB) ---\n";
	unsigned int seed = 7100;
	passed &= TestZeroF0SpecularIsSampled( 0.0, 0.2, 0.2, 80.0, seed++ );	// diffuse=0, grazing
	passed &= TestZeroF0SpecularIsSampled( 1.0, 0.2, 0.2, 80.0, seed++ );	// diffuse=1, grazing
	passed &= TestZeroF0SpecularIsSampled( 0.0, 0.2, 0.2, 0.0,  seed++ );	// diffuse=0, normal incidence
	passed &= TestZeroF0SpecularIsSampled( 1.0, 0.2, 0.2, 0.0,  seed++ );	// diffuse=1, normal incidence

	std::cout << "\n--- DL-64: Pdf at the specular peak matches the hemispherical-weight reference (RGB) ---\n";
	passed &= TestZeroF0PdfMatchesHemisphericalWeight( 0.0, 0.08, 0.08, 60.0 );
	passed &= TestZeroF0PdfMatchesHemisphericalWeight( 1.0, 0.08, 0.08, 60.0 );

	std::cout << "\n--- DL-64: PdfNM at the specular peak matches the hemispherical-weight reference (NM) ---\n";
	for( const double nm : { 450.0, 550.0, 650.0 } )
	{
		passed &= TestZeroF0PdfMatchesHemisphericalWeightNM( 0.01, 0.08, 0.08, 60.0, nm );
		passed &= TestZeroF0PdfMatchesHemisphericalWeightNM( 0.3,  0.08, 0.08, 60.0, nm );
	}

	std::cout << "\n--- DL-64: HWSS companion control -- valueNM already correct ---\n";
	for( const double nm : { 450.0, 550.0, 650.0 } )
		passed &= TestZeroF0ValueNMGrazingIsNonzero( nm );

	std::cout << "\n--- P2-2: Pdf matches the mode-gated reference in conductor mode (RGB) ---\n";
	passed &= TestNonSchlickPdfMatchesExpectedWeight( "conductor mixed", eFresnelConductor, 0.2, 0.7, 0.25, 0.25, 50.0, 2.74, 3.79 );
	passed &= TestNonSchlickPdfMatchesExpectedWeight( "conductor spec-only grazing", eFresnelConductor, 0.0, 0.9, 0.15, 0.15, 75.0, 2.74, 3.79 );

	std::cout << "\n--- P2-2: PdfNM matches the mode-gated reference in conductor mode (NM) ---\n";
	for( const double nm : { 450.0, 550.0, 650.0 } )
	{
		passed &= TestNonSchlickPdfMatchesExpectedWeightNM( "NM conductor mixed", eFresnelConductor, 0.2, 0.7, 0.25, 0.25, 50.0, nm, 2.74, 3.79 );
		passed &= TestNonSchlickPdfMatchesExpectedWeightNM( "NM conductor spec-only grazing", eFresnelConductor, 0.0, 0.9, 0.15, 0.15, 75.0, nm, 2.74, 3.79 );
	}

	std::cout << "\n--- P2-2: Pdf matches the mode-gated reference in thin-film mode (RGB) ---\n";
	passed &= TestNonSchlickPdfMatchesExpectedWeight( "thin-film mixed", eFresnelThinFilmConductor, 0.15, 0.6, 0.30, 0.30, 40.0, 2.74, 3.79, 2.5, 0.0, 180.0 );
	passed &= TestNonSchlickPdfMatchesExpectedWeight( "thin-film spec-only thick", eFresnelThinFilmConductor, 0.0, 0.8, 0.20, 0.20, 55.0, 2.74, 3.79, 2.5, 0.0, 420.0 );

	std::cout << "\n--- P2-2: PdfNM matches the mode-gated reference in thin-film mode (NM) ---\n";
	for( const double nm : { 450.0, 550.0, 650.0 } )
	{
		passed &= TestNonSchlickPdfMatchesExpectedWeightNM( "NM thin-film mixed", eFresnelThinFilmConductor, 0.15, 0.6, 0.30, 0.30, 40.0, nm, 2.74, 3.79, 2.5, 0.0, 180.0 );
		passed &= TestNonSchlickPdfMatchesExpectedWeightNM( "NM thin-film spec-only thick", eFresnelThinFilmConductor, 0.0, 0.8, 0.20, 0.20, 55.0, nm, 2.74, 3.79, 2.5, 0.0, 420.0 );
	}

	g_stubObject->release();

	std::cout << "\nGGXSampleEvaluationConsistencyTest: " << checks << " checks, " << failures << " failures\n";
	std::cout << "=== " << ( passed ? "ALL TESTS PASSED" : "TESTS FAILED" ) << " ===\n";
	return passed ? 0 : 1;
}
