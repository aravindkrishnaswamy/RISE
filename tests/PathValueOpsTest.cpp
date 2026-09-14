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
//    G. DL-43 red-proof: EvalPdfAtVertex(vertex, wi, wo) computes
//       Pdf(outgoing=wo | incoming=wi) -- PathVertexEval.h's own
//       documented contract, and Test E already pins the closed form
//       (fabs(Dot(wo,normal))*INV_PI for a Lambertian SPF).  BDPT's
//       light-subpath guiding candidates call this with
//       (wi=-currentRay.Dir(), wo=candidateDirection) -- correct.  Its
//       eye-subpath twin (BDPTIntegrator.cpp GenerateEyeSubpathImpl,
//       both the RIS candidate-1 and one-sample branches) instead
//       called it as (wi=candidateDirection, wo=-currentRay.Dir()) --
//       arguments swapped, so it silently evaluated the density of
//       scattering BACK toward the previous vertex instead of the
//       density of the guided candidate direction actually being
//       proposed.  This test reproduces both call shapes verbatim at a
//       Lambertian vertex with a non-normal candidate direction (60
//       degrees off the shading normal, distinctly different cosine
//       from the incoming direction) and asserts: the light-subpath
//       shape matches the OUTGOING candidate's cosine/pi (physically
//       correct); the eye-subpath (pre-fix) shape instead matches the
//       INCOMING direction's cosine/pi, independent of what candidate
//       was actually proposed -- exactly the bug BDPTIntegrator.cpp's
//       DL-43 fix (swapping the two arguments at the four eye-subpath
//       call sites) corrects.  See docs/DEBT_LEDGER.md DL-43 and
//       docs/DL03_GUIDED_IOR_CONTINUATION.md's "eye RIS actual-guide
//       limitation" note.
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

// DL-43 red-proof.  See the file-header comment for the full derivation.
// `incomingAway` mirrors what every call site builds as -currentRay.Dir()
// (the physical incoming ray, expressed away-from-surface); `candidate`
// mirrors a guided/RIS candidate direction (c.direction / gDir) chosen at
// a distinctly different angle off the normal so the two orderings cannot
// coincidentally agree.
static void TestDL43EyeVsLightGuidedPdfArgumentOrder()
{
	std::cout << "Test G: DL-43 eye-subpath vs light-subpath guided-candidate PDF argument order" << std::endl;

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

	// The physical ray arrives travelling in -Z (into the surface from
	// above); -currentRay.Dir() (the wrapper's "wi") is therefore +Z --
	// squarely along the normal, cosine 1.
	const Vector3 incomingAway = Vector3Ops::Normalize( Vector3( 0, 0, 1 ) );
	// A candidate continuation direction 60 degrees off the normal --
	// cosine 0.5, distinctly different from the incoming direction's
	// cosine of 1 so the two orderings cannot agree by coincidence.
	const Vector3 candidate = Vector3Ops::Normalize(
		Vector3( std::sqrt( 3.0 ) / 2.0, 0, 0.5 ) );

	PelTag pt;
	NMTag nt( 550.0 );

	const Scalar expectedOutgoingCosinePdf =
		std::fabs( Vector3Ops::Dot( candidate, v.normal ) ) * INV_PI;			// cos(60deg)/pi
	const Scalar expectedIncomingCosinePdf =
		std::fabs( Vector3Ops::Dot( incomingAway, v.normal ) ) * INV_PI;		// cos(0deg)/pi == 1/pi

	// Sanity: the two closed forms must actually differ, or this test
	// would not be able to distinguish the orderings.
	EXPECT_NEAR( expectedOutgoingCosinePdf, 0.5 * INV_PI, 1e-12 );
	EXPECT_NEAR( expectedIncomingCosinePdf, INV_PI, 1e-12 );

	// Light-subpath order (BDPTIntegrator.cpp GenerateLightSubpathImpl,
	// e.g. lines ~6291-6294/6346-6349): EvalPdfAtVertex(v, -currentRay.Dir(),
	// candidateDirection) -- wi=incoming, wo=candidate.  Already correct.
	const Scalar lightSubpathOrder_pel =
		PathValueOps::EvalPdfAtVertex<PelTag>( v, incomingAway, candidate, pt );
	const Scalar lightSubpathOrder_nm =
		PathValueOps::EvalPdfAtVertex<NMTag>( v, incomingAway, candidate, nt );

	// Eye-subpath order as it stood pre-DL-43-fix (BDPTIntegrator.cpp
	// GenerateEyeSubpathImpl lines ~2565-2566/2620-2621):
	// EvalPdfAtVertex(v, candidateDirection, -currentRay.Dir()) -- wi and
	// wo swapped relative to the light-subpath twin above.
	const Scalar eyeSubpathOrderPreFix_pel =
		PathValueOps::EvalPdfAtVertex<PelTag>( v, candidate, incomingAway, pt );
	const Scalar eyeSubpathOrderPreFix_nm =
		PathValueOps::EvalPdfAtVertex<NMTag>( v, candidate, incomingAway, nt );

	// The correct (light-subpath) order matches the OUTGOING candidate's
	// cosine/pi -- the physically meaningful density of the direction
	// actually being proposed.
	EXPECT_NEAR( lightSubpathOrder_pel, expectedOutgoingCosinePdf, 1e-14 );
	EXPECT_NEAR( lightSubpathOrder_nm, expectedOutgoingCosinePdf, 1e-14 );

	// The pre-fix eye-subpath order instead reproduces the INCOMING
	// direction's cosine/pi -- a value that does not depend on the
	// candidate at all.  This is the DL-43 bug's exact numerical shape:
	// every guided candidate at this vertex would evaluate to the same
	// wrong density regardless of which direction the guide proposed.
	EXPECT_NEAR( eyeSubpathOrderPreFix_pel, expectedIncomingCosinePdf, 1e-14 );
	EXPECT_NEAR( eyeSubpathOrderPreFix_nm, expectedIncomingCosinePdf, 1e-14 );

	// And the two orderings must disagree at this vertex -- if they ever
	// agreed, the eye-subpath call site could not be distinguished from
	// the (correct) light-subpath one by this fixture.
	if( std::fabs( eyeSubpathOrderPreFix_pel - lightSubpathOrder_pel ) < 1e-9 ) {
		std::cout << "FAIL: " << __FILE__ << ":" << __LINE__
			<< " eye/light orderings unexpectedly agree -- fixture cannot discriminate DL-43" << std::endl;
		failed++;
	}

	v.pMaterial = 0;
	material->release();
	painter->release();

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
	TestDL43EyeVsLightGuidedPdfArgumentOrder();

	std::cout << std::endl;
	if( failed == 0 ) {
		std::cout << "All tests passed." << std::endl;
		return 0;
	} else {
		std::cout << failed << " test(s) failed." << std::endl;
		return 1;
	}
}
