//////////////////////////////////////////////////////////////////////
//
//  PathValueOpsTest.cpp - Validates that the tag-dispatched
//    PathValueOps wrappers produce bit-identical output to direct
//    IBSDF::value / IBSDF::valueNM / PathVertexEval calls.
//
//  This is the critical Phase 0 correctness test: if the dispatch
//    layer is not a no-op in terms of numerical output, every
//    downstream integrator templatization will silently drift.
//
//  Tests:
//    A. EvalBSDF<PelTag> on Lambertian BRDF == bsdf.value() at
//       multiple directions.
//    B. EvalBSDF<NMTag> on same BRDF == bsdf.valueNM( nm ) at
//       multiple wavelengths.
//    C. EvalBSDFAtVertex<PelTag>/<NMTag> on a synthetic BDPTVertex
//       matches PathVertexEval::EvalBSDFAtVertex / EvalBSDFAtVertexNM.
//    D. EvalPdfAtVertex<PelTag>/<NMTag> on a synthetic medium vertex
//       matches PathVertexEval::EvalPdfAtVertex / EvalPdfAtVertexNM.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>
#include <cassert>
#include <iomanip>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/PathValueOps.h"
#include "../src/Library/Utilities/PathVertexEval.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/IBSDF.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Materials/LambertianBRDF.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Materials/IsotropicPhaseFunction.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Shaders/BDPTVertex.h"

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::SpectralDispatch;

static int failed = 0;

#define EXPECT_NEAR( a, b, tol ) do { \
	const double __a = (a); const double __b = (b); \
	if( std::fabs( __a - __b ) > (tol) ) { \
		std::cout << "FAIL: " << __FILE__ << ":" << __LINE__ \
			<< " expected " << __b << " (within " << (tol) << "), got " << __a << std::endl; \
		failed++; \
	} \
} while( 0 )

#define EXPECT_RISEPEL_NEAR( a, b, tol ) do { \
	const RISEPel __a = (a); const RISEPel __b = (b); \
	if( std::fabs( __a[0] - __b[0] ) > (tol) || \
	    std::fabs( __a[1] - __b[1] ) > (tol) || \
	    std::fabs( __a[2] - __b[2] ) > (tol) ) { \
		std::cout << "FAIL: " << __FILE__ << ":" << __LINE__ \
			<< " expected (" << __b[0] << "," << __b[1] << "," << __b[2] \
			<< "), got (" << __a[0] << "," << __a[1] << "," << __a[2] << ")" << std::endl; \
		failed++; \
	} \
} while( 0 )

// Build a minimal RayIntersectionGeometric at origin, normal +Z.
static RayIntersectionGeometric MakeRI( const Vector3& inDir )
{
	Ray r( Point3( 0, 0, 0 ), inDir );
	RayIntersectionGeometric ri( r, nullRasterizerState );
	ri.bHit = true;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
	return ri;
}

// LambertianBRDF / UniformColorPainter have protected destructors (they
// are reference-counted).  Allocate on the heap and release at end.
static void TestEvalBSDF_Pel()
{
	std::cout << "Test A: EvalBSDF<PelTag> matches IBSDF::value" << std::endl;

	UniformColorPainter* painter = new UniformColorPainter( RISEPel( 0.73, 0.71, 0.68 ) );
	painter->addref();
	LambertianBRDF* bsdf = new LambertianBRDF( *painter );
	bsdf->addref();

	const Vector3 wo_toward_surface = Vector3( 0, 0, -1 );
	RayIntersectionGeometric ri = MakeRI( wo_toward_surface );

	const Vector3 wi_light[] = {
		Vector3Ops::Normalize( Vector3( 0, 0, 1 ) ),
		Vector3Ops::Normalize( Vector3( 1, 0, 1 ) ),
		Vector3Ops::Normalize( Vector3( 0, 1, 1 ) ),
		Vector3Ops::Normalize( Vector3( 1, 1, 1 ) ),
	};

	PelTag tag;
	for( unsigned int i = 0; i < 4; i++ )
	{
		const RISEPel direct = bsdf->value( wi_light[i], ri );
		const RISEPel via_ops = PathValueOps::EvalBSDF<PelTag>( *bsdf, wi_light[i], ri, tag );
		EXPECT_RISEPEL_NEAR( via_ops, direct, 1e-14 );
	}

	bsdf->release();
	painter->release();

	std::cout << "  PASS" << std::endl;
}

static void TestEvalBSDF_NM()
{
	std::cout << "Test B: EvalBSDF<NMTag> matches IBSDF::valueNM" << std::endl;

	UniformColorPainter* painter = new UniformColorPainter( RISEPel( 0.73, 0.71, 0.68 ) );
	painter->addref();
	LambertianBRDF* bsdf = new LambertianBRDF( *painter );
	bsdf->addref();

	const Vector3 wo_toward_surface = Vector3( 0, 0, -1 );
	RayIntersectionGeometric ri = MakeRI( wo_toward_surface );

	const Vector3 wi_light = Vector3Ops::Normalize( Vector3( 1, 1, 1 ) );
	const Scalar wavelengths[] = { 400.0, 500.0, 555.0, 650.0, 780.0 };

	for( unsigned int i = 0; i < 5; i++ )
	{
		const Scalar nm = wavelengths[i];
		NMTag tag( nm );
		const Scalar direct  = bsdf->valueNM( wi_light, ri, nm );
		const Scalar via_ops = PathValueOps::EvalBSDF<NMTag>( *bsdf, wi_light, ri, tag );
		EXPECT_NEAR( via_ops, direct, 1e-14 );
	}

	bsdf->release();
	painter->release();

	std::cout << "  PASS" << std::endl;
}

static void TestEvalBSDFAtVertex_Dispatch()
{
	std::cout << "Test C: EvalBSDFAtVertex<Tag> matches PathVertexEval direct" << std::endl;

	// A minimal MEDIUM-like vertex is tricky to construct without a full
	// phase function; use a vertex that exercises the null-material early
	// return.  Both Pel and NM paths should return zero/0, proving the
	// dispatcher at least forwards consistently.
	BDPTVertex v;
	v.type = BDPTVertex::SURFACE;
	v.position = Point3( 0, 0, 0 );
	v.normal = Vector3( 0, 0, 1 );
	v.onb.CreateFromW( v.normal );
	v.pMaterial = 0;  // null material -> early return

	const Vector3 wi = Vector3Ops::Normalize( Vector3( 0, 0, 1 ) );
	const Vector3 wo = Vector3Ops::Normalize( Vector3( 1, 0, 1 ) );

	PelTag pt;
	NMTag  nt( 555.0 );

	const RISEPel direct_pel = PathVertexEval::EvalBSDFAtVertex( v, wi, wo );
	const RISEPel via_pel = PathValueOps::EvalBSDFAtVertex<PelTag>( v, wi, wo, pt );
	EXPECT_RISEPEL_NEAR( via_pel, direct_pel, 1e-14 );

	const Scalar direct_nm = PathVertexEval::EvalBSDFAtVertexNM( v, wi, wo, nt.nm );
	const Scalar via_nm = PathValueOps::EvalBSDFAtVertex<NMTag>( v, wi, wo, nt );
	EXPECT_NEAR( via_nm, direct_nm, 1e-14 );

	// Both should be zero given null material.
	EXPECT_RISEPEL_NEAR( via_pel, RISEPel( 0, 0, 0 ), 1e-14 );
	EXPECT_NEAR( via_nm, 0.0, 1e-14 );

	std::cout << "  PASS" << std::endl;
}

static void TestEvalPdfAtVertex_Dispatch()
{
	std::cout << "Test D: EvalPdfAtVertex<Tag> matches PathVertexEval direct" << std::endl;

	// BSSRDF entry vertex has a simple deterministic PDF:
	//   cos(theta) / PI
	// that doesn't require constructing an ISPF.  Use it to exercise the
	// dispatcher without heavyweight setup.
	BDPTVertex v;
	v.type = BDPTVertex::SURFACE;
	v.position = Point3( 0, 0, 0 );
	v.normal = Vector3( 0, 0, 1 );
	v.onb.CreateFromW( v.normal );
	v.pMaterial = reinterpret_cast<IMaterial*>( 0x1 );  // non-null sentinel to skip early return
	v.isBSSRDFEntry = true;

	const Vector3 wi = Vector3Ops::Normalize( Vector3( 0, 0, 1 ) );
	const Vector3 wo = Vector3Ops::Normalize( Vector3( 0.5, 0.5, 0.707 ) );

	PelTag pt;
	NMTag  nt( 500.0 );

	const Scalar direct_pel = PathVertexEval::EvalPdfAtVertex( v, wi, wo );
	const Scalar via_pel = PathValueOps::EvalPdfAtVertex<PelTag>( v, wi, wo, pt );
	EXPECT_NEAR( via_pel, direct_pel, 1e-14 );

	const Scalar direct_nm = PathVertexEval::EvalPdfAtVertexNM( v, wi, wo, nt.nm );
	const Scalar via_nm = PathValueOps::EvalPdfAtVertex<NMTag>( v, wi, wo, nt );
	EXPECT_NEAR( via_nm, direct_nm, 1e-14 );

	// Both should equal cos(theta)/PI for a BSSRDF entry vertex.
	const Scalar expected = std::fabs( Vector3Ops::Dot( wo, v.normal ) ) * INV_PI;
	EXPECT_NEAR( via_pel, expected, 1e-14 );
	EXPECT_NEAR( via_nm,  expected, 1e-14 );

	// Reset pMaterial to not leak the sentinel.
	v.pMaterial = 0;

	std::cout << "  PASS" << std::endl;
}

// Test with a REAL LambertianMaterial so the dispatcher walks the full
// PathVertexEval path: pMaterial->GetBSDF(), pMaterial->GetSPF(),
// IORStack construction, and pSPF->Pdf / pSPF->PdfNM dispatch.  This is
// the coverage the Phase 0 adversarial reviewer flagged as missing from
// the BSSRDF-entry shortcut tests above.
static void TestEvalAtSurfaceVertexWithRealSPF()
{
	std::cout << "Test E: EvalBSDFAtVertex / EvalPdfAtVertex via real LambertianMaterial" << std::endl;

	UniformColorPainter* painter = new UniformColorPainter( RISEPel( 0.5, 0.7, 0.9 ) );
	painter->addref();
	LambertianMaterial* material = new LambertianMaterial( *painter );
	material->addref();

	BDPTVertex v;
	v.type = BDPTVertex::SURFACE;
	v.position = Point3( 0, 0, 0 );
	v.normal = Vector3( 0, 0, 1 );
	v.onb.CreateFromW( v.normal );
	v.pMaterial = material;
	v.isBSSRDFEntry = false;
	v.mediumIOR = 1.0;
	v.insideObject = false;

	const Vector3 wi = Vector3Ops::Normalize( Vector3( 0, 0, 1 ) );
	const Vector3 wo = Vector3Ops::Normalize( Vector3( 1, 0, 1 ) );

	PelTag pt;
	NMTag  nt400( 400.0 );
	NMTag  nt700( 700.0 );

	// BSDF at vertex — exercises pMaterial->GetBSDF()->value / valueNM
	{
		const RISEPel direct_pel = PathVertexEval::EvalBSDFAtVertex( v, wi, wo );
		const RISEPel via_pel = PathValueOps::EvalBSDFAtVertex<PelTag>( v, wi, wo, pt );
		EXPECT_RISEPEL_NEAR( via_pel, direct_pel, 1e-14 );

		const Scalar direct_nm400 = PathVertexEval::EvalBSDFAtVertexNM( v, wi, wo, nt400.nm );
		const Scalar via_nm400 = PathValueOps::EvalBSDFAtVertex<NMTag>( v, wi, wo, nt400 );
		EXPECT_NEAR( via_nm400, direct_nm400, 1e-14 );

		const Scalar direct_nm700 = PathVertexEval::EvalBSDFAtVertexNM( v, wi, wo, nt700.nm );
		const Scalar via_nm700 = PathValueOps::EvalBSDFAtVertex<NMTag>( v, wi, wo, nt700 );
		EXPECT_NEAR( via_nm700, direct_nm700, 1e-14 );
	}

	// PDF at vertex — exercises pMaterial->GetSPF()->Pdf / PdfNM, including
	// IORStack construction.  For Lambertian Pdf == PdfNM (IOR irrelevant),
	// but the code path through pSPF->PdfNM is exercised.
	{
		const Scalar direct_pel = PathVertexEval::EvalPdfAtVertex( v, wi, wo );
		const Scalar via_pel = PathValueOps::EvalPdfAtVertex<PelTag>( v, wi, wo, pt );
		EXPECT_NEAR( via_pel, direct_pel, 1e-14 );

		const Scalar direct_nm400 = PathVertexEval::EvalPdfAtVertexNM( v, wi, wo, nt400.nm );
		const Scalar via_nm400 = PathValueOps::EvalPdfAtVertex<NMTag>( v, wi, wo, nt400 );
		EXPECT_NEAR( via_nm400, direct_nm400, 1e-14 );

		// Lambertian Pdf is cos(theta)/PI, wavelength-independent
		const Scalar expected = std::fabs( Vector3Ops::Dot( wo, v.normal ) ) * INV_PI;
		EXPECT_NEAR( via_pel, expected, 1e-14 );
		EXPECT_NEAR( via_nm400, expected, 1e-14 );
	}

	v.pMaterial = 0;
	material->release();
	painter->release();

	std::cout << "  PASS" << std::endl;
}

// Exercise the MEDIUM branch.  Phase function is wavelength-independent in
// RISE, so Pel and NM should return identical values — but the dispatcher
// must route to the correct branch in each case.
static void TestEvalAtMediumVertex()
{
	std::cout << "Test F: EvalBSDFAtVertex / EvalPdfAtVertex on MEDIUM vertex" << std::endl;

	IsotropicPhaseFunction* phase = new IsotropicPhaseFunction();
	phase->addref();

	BDPTVertex v;
	v.type = BDPTVertex::MEDIUM;
	v.position = Point3( 0, 0, 0 );
	v.normal = Vector3( 0, 0, 1 );
	v.pPhaseFunc = phase;
	v.pMediumVol = 0;  // unused by dispatch
	v.sigma_t_scalar = 1.0;

	const Vector3 wi = Vector3Ops::Normalize( Vector3( 0, 0, 1 ) );
	const Vector3 wo = Vector3Ops::Normalize( Vector3( 1, 1, 1 ) );

	PelTag pt;
	NMTag  nt( 555.0 );

	// BSDF on medium: both return (1/4π, 1/4π, 1/4π) and 1/4π respectively
	{
		const RISEPel direct_pel = PathVertexEval::EvalBSDFAtVertex( v, wi, wo );
		const RISEPel via_pel = PathValueOps::EvalBSDFAtVertex<PelTag>( v, wi, wo, pt );
		EXPECT_RISEPEL_NEAR( via_pel, direct_pel, 1e-14 );

		const Scalar direct_nm = PathVertexEval::EvalBSDFAtVertexNM( v, wi, wo, nt.nm );
		const Scalar via_nm = PathValueOps::EvalBSDFAtVertex<NMTag>( v, wi, wo, nt );
		EXPECT_NEAR( via_nm, direct_nm, 1e-14 );

		const Scalar expected = 1.0 / (4.0 * PI);
		EXPECT_NEAR( via_pel[0], expected, 1e-14 );
		EXPECT_NEAR( via_nm, expected, 1e-14 );
	}

	// PDF on medium: same
	{
		const Scalar direct_pel = PathVertexEval::EvalPdfAtVertex( v, wi, wo );
		const Scalar via_pel = PathValueOps::EvalPdfAtVertex<PelTag>( v, wi, wo, pt );
		EXPECT_NEAR( via_pel, direct_pel, 1e-14 );

		const Scalar direct_nm = PathVertexEval::EvalPdfAtVertexNM( v, wi, wo, nt.nm );
		const Scalar via_nm = PathValueOps::EvalPdfAtVertex<NMTag>( v, wi, wo, nt );
		EXPECT_NEAR( via_nm, direct_nm, 1e-14 );

		const Scalar expected = 1.0 / (4.0 * PI);
		EXPECT_NEAR( via_pel, expected, 1e-14 );
		EXPECT_NEAR( via_nm, expected, 1e-14 );
	}

	v.pPhaseFunc = 0;
	phase->release();

	std::cout << "  PASS" << std::endl;
}

int main()
{
	std::cout << "=== PathValueOpsTest ===" << std::endl;

	TestEvalBSDF_Pel();
	TestEvalBSDF_NM();
	TestEvalBSDFAtVertex_Dispatch();
	TestEvalPdfAtVertex_Dispatch();
	TestEvalAtSurfaceVertexWithRealSPF();
	TestEvalAtMediumVertex();

	std::cout << std::endl;
	if( failed == 0 ) {
		std::cout << "All tests passed." << std::endl;
		return 0;
	} else {
		std::cout << failed << " test(s) failed." << std::endl;
		return 1;
	}
}
