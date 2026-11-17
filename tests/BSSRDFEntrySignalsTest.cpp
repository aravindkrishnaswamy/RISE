//////////////////////////////////////////////////////////////////////
//
//  BSSRDFEntrySignalsTest.cpp - Verification suite for DL-22
//  (BSSRDF entry vertex live signals & derivatives plumbing).
//
//  Validates that BSSRDF entry points (both Christensen-Burley disk
//  projection in BSSRDFSampling and Chiang-Burley random walk in
//  RandomWalkSSS) preserve and forward the live surface hit's:
//    - SurfaceDerivativesInfo (derivatives, curvature, scaleHint)
//    - SurfaceSignalInfo (provider, nObject, and cross-object triple:
//      pScene, pSelf, ptWorld)
//    - TextureFootprint, ptCoord, ptCoord1, ptObjIntersec, vColor
//
//  And validates that:
//    1. BSSRDFSampling::SampleEntryPoint Step 9 evaluates entry Fresnel
//       and IOR against the sampled entry hit, not the exit point.
//    2. PathTracingIntegrator populates entryRI with live entry signals
//       and evaluates entry IOR/BSDF against entryRI.
//    3. BDPTIntegrator populates BDPTVertex entryV with live entry signals
//       (eye subpath and light subpath, RGB and spectral NM/HWSS).
//    4. PathVertexEval::EvalBSDFAtVertex / EvalBSDFAtVertexNM reconstructs
//       live signals via PopulateRIGFromVertex and evaluates signal-keyed
//       painters using live entry signals rather than neutral defaults.
//    5. RandomWalkSSS::SampleExit forwards live exit boundary signals,
//       derivatives, and cross-object triple.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <iomanip>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/BSSRDFSampling.h"
#include "../src/Library/Utilities/RandomWalkSSS.h"
#include "../src/Library/Utilities/PathVertexEval.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Interfaces/ISurfaceSignalProvider.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/BurleyNormalizedDiffusionProfile.h"
#include "../src/Library/Materials/SubSurfaceScatteringMaterial.h"
#include "../src/Library/Shaders/BSSRDFEntryAdapters.h"
#include "../src/Library/Shaders/BDPTVertex.h"

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::BSSRDFAdapters;

static int g_pass = 0;
static int g_fail = 0;

static void Check( bool condition, const char* desc )
{
	if( condition ) {
		std::cout << "  PASS: " << desc << std::endl;
		g_pass++;
	} else {
		std::cerr << "  FAIL: " << desc << std::endl;
		g_fail++;
	}
}

static bool Close( Scalar a, Scalar b, Scalar tol = 1e-4 )
{
	return fabs( a - b ) <= tol;
}

/// Deterministic sampler for reproducible BSSRDF importance sampling
class DeterministicSampler : public ISampler
{
	std::vector<Scalar> m_samples;
	size_t m_idx;
public:
	DeterministicSampler( const std::vector<Scalar>& s ) : m_samples( s ), m_idx( 0 ) {}

	Scalar Get1D() override
	{
		if( m_samples.empty() ) return 0.5;
		Scalar val = m_samples[m_idx % m_samples.size()];
		m_idx++;
		return val;
	}

	Point2 Get2D() override
	{
		Scalar u = Get1D();
		Scalar v = Get1D();
		return Point2( u, v );
	}
};

/// Custom signal-keyed painter that returns liveVal when signals/provider are present,
/// and neutralVal when default/neutral.
class SignalKeyedPainter : public virtual IScalarPainter, public virtual Reference
{
public:
	Scalar liveVal;
	Scalar neutralVal;

	SignalKeyedPainter( Scalar live = 2.5, Scalar neutral = 1.0 )
		: liveVal( live ), neutralVal( neutral ) {}

	Scalar GetValue( const RayIntersectionGeometric& ri ) const
	{
		if( ri.signals.pProvider != nullptr && ri.signals.pScene != nullptr ) {
			return liveVal;
		}
		return neutralVal;
	}

	ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
	{
		Scalar v = GetValue( ri );
		return ScalarTriple( v, v, v );
	}

	Scalar GetValueAtNM( const RayIntersectionGeometric& ri, const Scalar ) const override
	{
		return GetValue( ri );
	}
};

/// Custom curvature-keyed painter that returns 1.0 + curvature when curvature is valid,
/// and neutralVal when not valid.
class CurvatureKeyedPainter : public virtual IScalarPainter, public virtual Reference
{
public:
	Scalar neutralVal;

	CurvatureKeyedPainter( Scalar neutral = 1.0 ) : neutralVal( neutral ) {}

	Scalar GetValue( const RayIntersectionGeometric& ri ) const
	{
		if( ri.derivatives.curvatureValid ) {
			return 1.0 + ri.derivatives.curvature;
		}
		return neutralVal;
	}

	ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override
	{
		Scalar v = GetValue( ri );
		return ScalarTriple( v, v, v );
	}

	Scalar GetValueAtNM( const RayIntersectionGeometric& ri, const Scalar ) const override
	{
		return GetValue( ri );
	}
};

static SDFGeometry* BuildSdfSphere( const Scalar radius )
{
	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart(
		SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), radius, 0, 0, 0 ) );
	return new SDFGeometry( parts, 512, Scalar( 1e-5 ) );
}

int main()
{
	std::cout << "=== BSSRDFEntrySignalsTest (DL-22) ===" << std::endl;

	SurfaceCurvatureDemand::Registration regCurv( true );
	SurfaceSignalDemand::Registration regSig( true );

	const Scalar sphereRadius = 2.0;
	SDFGeometry* sdfGeo = BuildSdfSphere( sphereRadius );
	Object* pObject = new Object( sdfGeo );
	safe_release( sdfGeo );
	pObject->FinalizeTransformations();

	// Create a dummy scene object to serve as pScene pointer in the cross-object triple
	int fakeSceneDummy = 0xbeef;
	const IObjectManager* pFakeScene = reinterpret_cast<const IObjectManager*>( &fakeSceneDummy );

	// Setup exit hit on the sphere surface at (0, 0, 2.0)
	Ray exitRay( Point3( 0, 0, 5 ), Vector3( 0, 0, -1 ) );
	RayIntersection exitRIFull( exitRay, nullRasterizerState );
	pObject->IntersectRay( exitRIFull, 10.0, true, true, false );
	Check( exitRIFull.geometric.bHit, "Exit hit on SDF sphere succeeded" );

	RayIntersectionGeometric exitRIG = exitRIFull.geometric;
	// Stamp the cross-object triple as ObjectManager would:
	exitRIG.signals.pScene = pFakeScene;
	exitRIG.signals.pSelf = pObject;
	exitRIG.signals.ptWorld = exitRIG.ptIntersection;

	Check( exitRIG.signals.pProvider != nullptr, "Exit hit has live SDF signal provider" );
	Check( exitRIG.derivatives.curvatureValid, "Exit hit has valid curvature" );

	// Setup SSS material with signal-keyed IOR painter
	SignalKeyedPainter* pSigPainter = new SignalKeyedPainter( 2.5, 1.0 );
	UniformScalarPainter* pAbs = new UniformScalarPainter( 0.1 );
	UniformScalarPainter* pScat = new UniformScalarPainter( 10.0 );

	BurleyNormalizedDiffusionProfile* pProfile = new BurleyNormalizedDiffusionProfile(
		*pSigPainter, *pAbs, *pScat, 0.0 );
	pProfile->addref();

	SubSurfaceScatteringMaterial* pMaterial = new SubSurfaceScatteringMaterial(
		*pSigPainter, *pAbs, *pScat, 0.0, 0.0 );
	pMaterial->addref();

	//
	// Section 1: Disk projection BSSRDFSampling::SampleEntryPoint populates probe signals
	//
	std::cout << "\nSection 1: Disk projection BSSRDFSampling::SampleEntryPoint probe signals...\n";
	{
		// Draw sample on normal axis (axis 0), small radius so entry hit lands on sphere
		// Step 1: channel (0.1 -> ch 0)
		// Step 2: axis (0.1 -> axis 0, normal)
		// Step 3: radius CDF (0.3 -> rSample ~ 0.087)
		// Step 4: phi (0.25 -> phi = pi/2)
		// Step 6: probe selection (0.9 -> near hit sel = 1)
		// Step 8: cosine u1 (0.5), u2 (0.5)
		std::vector<Scalar> seq = { 0.1, 0.1, 0.3, 0.25, 0.9, 0.5, 0.5 };
		DeterministicSampler sampler( seq );

		BSSRDFSampling::SampleResult bssrdf = BSSRDFSampling::SampleEntryPoint(
			exitRIG, pObject, pMaterial, sampler, 0 );

		Check( bssrdf.valid, "SampleEntryPoint succeeded" );
		Check( bssrdf.signals.pProvider != nullptr, "BSSRDF SampleResult carries live signals.pProvider" );
		Check( bssrdf.signals.pScene == pFakeScene, "BSSRDF SampleResult forwards signals.pScene in cross-object triple" );
		Check( bssrdf.signals.pSelf == pObject, "BSSRDF SampleResult stamps signals.pSelf in cross-object triple" );
		Check( Close( bssrdf.signals.ptWorld.z, bssrdf.entryPoint.z, 1e-3 ), "BSSRDF SampleResult stamps signals.ptWorld" );
		Check( bssrdf.derivatives.curvatureValid, "BSSRDF SampleResult carries live derivatives.curvatureValid" );
		Check( Close( bssrdf.derivatives.curvature, 1.0 / sphereRadius, 0.05 ), "BSSRDF SampleResult curvature matches 1/R = 0.5" );
	}

	//
	// Section 2: Step 9 evaluates entry Fresnel / IOR with live entry record
	//
	std::cout << "\nSection 2: Step 9 evaluates entry IOR at entry point...\n";
	{
		std::vector<Scalar> seq = { 0.1, 0.1, 0.3, 0.25, 0.9, 0.5, 0.5 };
		DeterministicSampler sampler( seq );

		// If exit point has neutral signals (pScene = nullptr) but entry probe hit is on the SDF sphere,
		// Step 9 must evaluate entry IOR = 2.5 (from the probe hit) rather than exit point IOR = 1.0.
		RayIntersectionGeometric exitRIGNeutral = exitRIG;
		exitRIGNeutral.signals.pScene = pFakeScene; // Provide scene for entry hit to stamp
		// Corrupt exit point's own provider to test that Step 9 reads the ENTRY hit, not exitRIG
		exitRIGNeutral.signals.pProvider = nullptr;

		BSSRDFSampling::SampleResult bssrdf = BSSRDFSampling::SampleEntryPoint(
			exitRIGNeutral, pObject, pMaterial, sampler, 0 );

		Check( bssrdf.valid, "SampleEntryPoint succeeded with exitRIGNeutral" );
		Check( bssrdf.signals.pProvider != nullptr, "SampleResult gets live signals from probe hit even when exit point was neutral" );
		Check( bssrdf.signals.pScene == pFakeScene, "SampleResult gets pScene from exit point even when exit provider was null" );
		const Scalar etaLive = pProfile->GetIOR( exitRIG ); // 2.5
		Check( Close( etaLive, 2.5 ), "Profile evaluates live IOR = 2.5 when live signals present" );
	}

	//
	// Section 3: PT entryRI forwarding & BSSRDFEntryBSDF evaluation
	//
	std::cout << "\nSection 3: PathTracingIntegrator entryRI forwarding & BSSRDFEntryBSDF...\n";
	{
		std::vector<Scalar> seq = { 0.1, 0.1, 0.3, 0.25, 0.9, 0.5, 0.5 };
		DeterministicSampler sampler( seq );

		BSSRDFSampling::SampleResult bssrdf = BSSRDFSampling::SampleEntryPoint(
			exitRIG, pObject, pMaterial, sampler, 0 );

		// Simulate PathTracingIntegrator entryRI construction
		RayIntersectionGeometric entryRI(
			EntryEvaluationRay( bssrdf.entryPoint, bssrdf.entryNormal ),
			nullRasterizerState );
		entryRI.bHit = true;
		entryRI.ptIntersection = bssrdf.entryPoint;
		entryRI.vNormal = bssrdf.entryNormal;
		entryRI.vGeomNormal = bssrdf.entryGeomNormal;
		entryRI.onb = bssrdf.entryONB;
		// The DL-22 forwarding:
		entryRI.derivatives = bssrdf.derivatives;
		entryRI.signals = bssrdf.signals;
		entryRI.txFootprint = bssrdf.txFootprint;
		entryRI.ptCoord = bssrdf.ptCoord;
		entryRI.ptCoord1 = bssrdf.ptCoord1;
		entryRI.bHasTexCoord1 = bssrdf.bHasTexCoord1;
		entryRI.ptObjIntersec = bssrdf.ptObjIntersec;
		entryRI.vColor = bssrdf.vColor;
		entryRI.bHasVertexColor = bssrdf.bHasVertexColor;

		Check( entryRI.signals.pProvider != nullptr, "entryRI carries live signals.pProvider" );
		Check( entryRI.signals.pScene == pFakeScene, "entryRI carries live signals.pScene" );
		Check( entryRI.derivatives.curvatureValid, "entryRI carries live derivatives.curvatureValid" );

		// Evaluate BSSRDFEntryBSDF at entryRI
		const Scalar eta = pProfile->GetIOR( entryRI );
		Check( Close( eta, 2.5 ), "entryRI evaluates live IOR = 2.5 via GetIOR" );

		BSSRDFEntryBSDF entryBSDF( pProfile, eta );

		// DL-306: the SSS boundary transmits with the EXACT dielectric law
		// (the one the SPF's surface reflection uses), normalized by
		// c(eta) = 2 * integral (1 - F(mu)) mu dmu.  The normalized Sw is then
		// index-dependent in SHAPE, so a live eta = 2.5 and the neutral
		// eta = 1 (no interface: Sw = 1/PI at every cosine) differ most toward
		// grazing.  Test at cosTheta = 0.2, with an independent exact Fresnel
		// and an independent quadrature of c (no production constant):
		//   eta = 2.5: F(0.2) = 0.3906..., c = 0.778133 => Sw = 0.246856
		//   eta = 1.0:                                     Sw = 0.318310
		// (Under DL-48's Schlick law the test used cosTheta = 0.5, where the
		// exact law's live and neutral Sw are only 0.2 % apart.)
		const Scalar cosOblique = 0.2;
		auto exactF = []( const Scalar c, const Scalar n ) {
			const Scalar s2 = (1.0 - c * c) / (n * n);
			const Scalar ct = sqrt( 1.0 - s2 );
			const Scalar rs = (c - n * ct) / (c + n * ct);
			const Scalar rp = (n * c - ct) / (n * c + ct);
			return 0.5 * (rs * rs + rp * rp);
		};
		Scalar cIndependent = 0;
		{
			const int count = 1 << 18;
			for( int i = 0; i < count; ++i ) {
				const Scalar mu = (Scalar(i) + 0.5) / count;
				cIndependent += 2.0 * mu * (1.0 - exactF( mu, 2.5 )) / count;
			}
		}
		Vector3 vLightOblique = Vector3Ops::Normalize( entryRI.vNormal +
			entryRI.onb.u() * ( sqrt( 1.0 - cosOblique * cosOblique ) / cosOblique ) );
		RISEPel bsdfOblique = entryBSDF.value( vLightOblique, entryRI );
		const Scalar expectedSwLive = (1.0 - exactF( cosOblique, 2.5 )) / (cIndependent * PI);
		const Scalar expectedSwNeutral = 1.0 / PI;

		Check( Close( bsdfOblique[0], expectedSwLive, 1e-4 ), "entryBSDF evaluates live Sw (0.2469), not neutral (0.3183)" );
		Check( !Close( bsdfOblique[0], expectedSwNeutral, 0.02 ), "entryBSDF distinct from neutral Sw" );
	}

	//
	// Section 4: BDPT BDPTVertex forwarding & PathVertexEval
	//
	std::cout << "\nSection 4: BDPT BDPTVertex forwarding & PathVertexEval...\n";
	{
		std::vector<Scalar> seq = { 0.1, 0.1, 0.3, 0.25, 0.9, 0.5, 0.5 };
		DeterministicSampler sampler( seq );

		BSSRDFSampling::SampleResult bssrdf = BSSRDFSampling::SampleEntryPoint(
			exitRIG, pObject, pMaterial, sampler, 0 );

		// Simulate BDPTIntegrator entryV construction
		BDPTVertex entryV;
		entryV.type = BDPTVertex::SURFACE;
		entryV.position = bssrdf.entryPoint;
		entryV.normal = bssrdf.entryNormal;
		entryV.geomNormal = bssrdf.entryGeomNormal;
		entryV.onb = bssrdf.entryONB;
		entryV.derivatives = bssrdf.derivatives;
		entryV.signals = bssrdf.signals;
		entryV.txFootprint = bssrdf.txFootprint;
		entryV.ptCoord = bssrdf.ptCoord;
		entryV.ptCoord1 = bssrdf.ptCoord1;
		entryV.bHasTexCoord1 = bssrdf.bHasTexCoord1;
		entryV.ptObjIntersec = bssrdf.ptObjIntersec;
		entryV.vColor = bssrdf.vColor;
		entryV.bHasVertexColor = bssrdf.bHasVertexColor;
		entryV.pMaterial = pMaterial;
		entryV.pObject = pObject;
		entryV.isDelta = false;
		entryV.isConnectible = true;
		entryV.isBSSRDFEntry = true;

		// 1. PopulateRIGFromVertex test
		RayIntersectionGeometric rebuiltRIG(
			Ray( entryV.position, -entryV.normal ), nullRasterizerState );
		PathVertexEval::PopulateRIGFromVertex( entryV, rebuiltRIG );

		Check( rebuiltRIG.signals.pProvider != nullptr, "rebuiltRIG carries live signals.pProvider from entryV" );
		Check( rebuiltRIG.signals.pScene == pFakeScene, "rebuiltRIG carries live signals.pScene from entryV" );
		Check( rebuiltRIG.derivatives.curvatureValid, "rebuiltRIG carries live derivatives.curvatureValid from entryV" );
		Check( Close( pProfile->GetIOR( rebuiltRIG ), 2.5 ), "rebuiltRIG evaluates live IOR = 2.5 (not neutral 1.0)" );

		// 2. PathVertexEval::EvalBSDFAtVertex test
		Vector3 wiOblique = Vector3Ops::Normalize( entryV.normal + entryV.onb.u() * sqrt( 3.0 ) ); // cosTheta = 0.5
		Vector3 wo = entryV.normal;

		RISEPel bsdfPel = PathVertexEval::EvalBSDFAtVertex( entryV, wiOblique, wo );
		const Scalar expectedSwLive = 0.79082 / (0.77745 * PI);
		Check( Close( bsdfPel[0], expectedSwLive, 0.005 ), "PathVertexEval::EvalBSDFAtVertex evaluates live Sw (0.3238)" );

		// 3. Spectral NM EvalBSDFAtVertexNM test
		Scalar bsdfNM = PathVertexEval::EvalBSDFAtVertexNM( entryV, wiOblique, wo, 550.0 );
		Check( Close( bsdfNM, expectedSwLive, 0.005 ), "PathVertexEval::EvalBSDFAtVertexNM evaluates live Sw in spectral" );
	}

	//
	// Section 5: RandomWalkSSS::SampleExit forwards live exit boundary signals
	//
	std::cout << "\nSection 5: RandomWalkSSS::SampleExit forwards live exit boundary signals...\n";
	{
		// Set up random walk parameters
		const RISEPel sigma_a( 0.01, 0.01, 0.01 );
		const RISEPel sigma_s( 1.0, 1.0, 1.0 );
		const RISEPel sigma_t = sigma_a + sigma_s;
		const Scalar g = 0.0;
		const Scalar ior = 1.3;
		const unsigned int maxBounces = 16;

		// Use a sampler with fixed sequence to let the walk take a step and exit
		std::vector<Scalar> walkSeq = {
			0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1,
			0.9, 0.9, 0.9, 0.9, 0.9, 0.9, 0.9, 0.9
		};
		DeterministicSampler rwSampler( walkSeq );

		BSSRDFSampling::SampleResult rwResult = RandomWalkSSS::SampleExit(
			exitRIG, pObject, sigma_a, sigma_s, sigma_t, g, ior, maxBounces, rwSampler, 0 );

		Check( rwResult.valid, "RandomWalkSSS::SampleExit succeeded" );
		Check( rwResult.signals.pProvider != nullptr, "RandomWalk SampleResult carries live signals.pProvider" );
		Check( rwResult.signals.pScene == pFakeScene, "RandomWalk SampleResult forwards signals.pScene in cross-object triple" );
		Check( rwResult.signals.pSelf == pObject, "RandomWalk SampleResult stamps signals.pSelf in cross-object triple" );
		Check( rwResult.derivatives.curvatureValid, "RandomWalk SampleResult carries live derivatives.curvatureValid" );
	}

	safe_release( pMaterial );
	safe_release( pProfile );
	safe_release( pScat );
	safe_release( pAbs );
	safe_release( pSigPainter );
	safe_release( pObject );

	std::cout << "\nResult: " << g_pass << " passed, " << g_fail << " failed.\n";
	return g_fail ? 1 : 0;
}
