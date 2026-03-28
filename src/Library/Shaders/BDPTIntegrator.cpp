//////////////////////////////////////////////////////////////////////
//
//  BDPTIntegrator.cpp - Implementation of the BDPTIntegrator class.
//
//  ALGORITHM OVERVIEW:
//    For each pixel sample, the integrator:
//    1. Generates a light subpath by sampling a light source (via
//       LightSampler), emitting a ray, and tracing it through the
//       scene.  At each surface hit, the SPF is sampled to extend
//       the path.  Forward PDFs (area measure) and cumulative
//       throughput are stored at each vertex.
//
//    2. Generates an eye subpath from the camera ray.  Same logic
//       as the light subpath but starting from the camera vertex.
//
//    3. Evaluates all (s,t) connection strategies where s is the
//       number of light vertices and t the number of eye vertices.
//       Each strategy gets a MIS weight via the balance heuristic.
//
//  CONNECTION STRATEGIES:
//    - s=0: Eye path naturally hits an emitter (no light subpath
//      needed).  Contribution = eyeThroughput * Le.
//    - s=1, t>1: Next event estimation — connect the last eye
//      vertex to the sampled light vertex.  Classic direct lighting.
//    - t=1: Connect the last light vertex to the camera.  Because
//      the eye subpath already stores the camera as vertex 0, this
//      is the full light-tracing/splat strategy.  The result lands
//      at an arbitrary pixel position and is accumulated via the
//      SplatFilm.
//    - s>1, t>1: General case — connect the two subpath endpoints,
//      evaluate BSDFs at both, multiply with the geometric term.
//
//  PDF BOOKKEEPING:
//    Each vertex stores pdfFwd (area measure, from the direction
//    it was generated) and pdfRev (area measure, computed after the
//    NEXT vertex is generated, representing the probability of
//    sampling the reverse direction).  These are used by MISWeight
//    to incrementally compute PDF ratios for all strategies.
//
//    Delta interactions (perfect specular) set isDelta=true.  The
//    MIS weight computation skips delta vertices since only exactly
//    one strategy can generate them.
//
//  RUSSIAN ROULETTE:
//    Applied after depth > 2 to terminate low-throughput paths.
//    Survival probability is clamped to [0, 1] and the throughput
//    is divided by the survival probability to maintain unbiasedness.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BDPTIntegrator.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/ISPF.h"
#include "../Interfaces/IEmitter.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/ISubSurfaceDiffusionProfile.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/ILightManager.h"
#include "../Interfaces/ILightPriv.h"
#include "../Utilities/BDPTUtilities.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Color/ColorMath.h"
#include "../Utilities/Math3D/Constants.h"
#include "../Cameras/CameraUtilities.h"
#include "../Intersection/RayIntersection.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Rendering/LuminaryManager.h"
#include "../Rendering/RayCaster.h"

using namespace RISE;
using namespace RISE::Implementation;

//
// Small epsilon for ray offsets to avoid self-intersection
//
static const Scalar BDPT_RAY_EPSILON = 1e-6;

//
// Minimum throughput before Russian Roulette kicks in
//
static const Scalar BDPT_RR_THRESHOLD = 0.05;

namespace
{
	inline const IORStack* BuildVertexIORStack(
		const BDPTVertex& vertex,
		IORStack& stack
		)
	{
		if( !vertex.pObject ) {
			return 0;
		}

		if( vertex.insideObject ) {
			stack = IORStack( 1.0 );
			stack.SetCurrentObject( vertex.pObject );
			stack.push( vertex.mediumIOR );
		} else {
			stack = IORStack( vertex.mediumIOR );
			stack.SetCurrentObject( vertex.pObject );
		}

		return &stack;
	}

	inline bool BDPTUsesIORStack( const IRayCaster& caster )
	{
		const RayCaster* pConcrete = dynamic_cast<const RayCaster*>( &caster );
		return pConcrete ? pConcrete->UsesIORStack() : false;
	}
}

//////////////////////////////////////////////////////////////////////
// Construction / Destruction
//////////////////////////////////////////////////////////////////////

BDPTIntegrator::BDPTIntegrator(
	unsigned int maxEye,
	unsigned int maxLight
	) :
  maxEyeDepth( maxEye ),
  maxLightDepth( maxLight ),
  pLightSampler( 0 ),
  pManifoldSolver( 0 )
{
}

BDPTIntegrator::~BDPTIntegrator()
{
	safe_release( pLightSampler );
	// pManifoldSolver is owned externally (by BDPTRasterizerBase), do not release here
}

void BDPTIntegrator::SetManifoldSolver( ManifoldSolver* pSolver )
{
	pManifoldSolver = pSolver;
}

void BDPTIntegrator::SetLightSampler( LightSampler* pSampler )
{
	if( pSampler == pLightSampler ) {
		return;
	}

	safe_release( pLightSampler );
	pLightSampler = pSampler;

	if( pLightSampler ) {
		pLightSampler->addref();
	}
}

//////////////////////////////////////////////////////////////////////
// Helper: evaluate BSDF at a vertex for given in/out directions.
//
// Adapts the RISE BSDF convention where value(vLightIn, ri) expects
// the incoming viewer ray stored in ri.ray.Dir() (toward-surface)
// and vLightIn pointing away from surface toward the light.
//
// Callers pass wi and wo both as "away from surface" directions,
// so we negate wo to construct the ri.ray toward-surface direction.
//////////////////////////////////////////////////////////////////////

RISEPel BDPTIntegrator::EvalBSDFAtVertex(
	const BDPTVertex& vertex,
	const Vector3& wi,
	const Vector3& wo
	) const
{
	if( !vertex.pMaterial ) {
		return RISEPel( 0, 0, 0 );
	}

	// BSSRDF entry vertex: evaluate Sw(direction) = Ft(cos) / (c * PI).
	// This is the directional component of the separable BSSRDF at the
	// re-emission point.  The cosine-hemisphere PDF cos/PI cancels PI in
	// the denominator during sampling, but for connections we need the
	// full Sw value.
	if( vertex.isBSSRDFEntry ) {
		ISubSurfaceDiffusionProfile* pProfile = vertex.pMaterial->GetDiffusionProfile();
		if( pProfile ) {
			// wi is the direction into the surface (from outside).
			// No fabs: back-face connections (cosTheta < 0) return zero.
			const Scalar cosTheta = Vector3Ops::Dot( wi, vertex.normal );
			if( cosTheta <= NEARZERO ) {
				return RISEPel( 0, 0, 0 );
			}

			RayIntersectionGeometric rig(
				Ray( vertex.position, -wi ), nullRasterizerState );
			rig.bHit = true;
			rig.ptIntersection = vertex.position;
			rig.vNormal = vertex.normal;
			rig.onb = vertex.onb;

			const Scalar FtEntry = pProfile->FresnelTransmission( cosTheta, rig );
			const Scalar eta = pProfile->GetIOR( rig );
			const Scalar F0 = ((eta - 1.0) / (eta + 1.0)) * ((eta - 1.0) / (eta + 1.0));
			const Scalar c = (41.0 - 20.0 * F0) / 42.0;
			const Scalar Sw = (c > 1e-20) ? FtEntry / (c * PI) : 0;
			return RISEPel( Sw, Sw, Sw );
		}
		return RISEPel( 0, 0, 0 );
	}

	const IBSDF* pBSDF = vertex.pMaterial->GetBSDF();
	if( !pBSDF ) {
		return RISEPel( 0, 0, 0 );
	}

	// Build a RayIntersectionGeometric for the BSDF evaluation.
	// In RISE's convention:
	//   vLightIn (wi) = direction away from surface toward the light source
	//   ri.ray.Dir()  = direction toward the surface (incoming viewing ray)
	// Callers pass wo as the outgoing direction (away from surface toward
	// the viewer), so we negate it to get the toward-surface convention.
	Ray evalRay( vertex.position, -wo );
	RayIntersectionGeometric ri( evalRay, nullRasterizerState );
	ri.bHit = true;
	ri.ptIntersection = vertex.position;
	ri.vNormal = vertex.normal;
	ri.onb = vertex.onb;

	return pBSDF->value( wi, ri );
}

//////////////////////////////////////////////////////////////////////
// Helper: evaluate SPF PDF at a vertex.
//
// SPF::Pdf(ri, wo) expects ri.ray.Dir() as the incoming direction
// (toward surface) and wo as the outgoing direction (away from
// surface).  Since callers pass wi as the "incoming" side and wo as
// the "outgoing" side (both away-from-surface), we negate wi to
// build ri.ray.Dir().
//////////////////////////////////////////////////////////////////////

Scalar BDPTIntegrator::EvalPdfAtVertex(
	const BDPTVertex& vertex,
	const Vector3& wi,
	const Vector3& wo
	) const
{
	if( !vertex.pMaterial ) {
		return 0;
	}

	// BSSRDF entry vertex: the scattered direction is cosine-weighted,
	// so the sampling PDF is cos(theta) / PI.
	if( vertex.isBSSRDFEntry ) {
		const Scalar cosTheta = fabs( Vector3Ops::Dot( wo, vertex.normal ) );
		return cosTheta * INV_PI;
	}

	const ISPF* pSPF = vertex.pMaterial->GetSPF();
	if( !pSPF ) {
		return 0;
	}

	// Build ri: ri.ray.Dir() must be toward-surface (incoming), and wo is the
	// outgoing direction (away from surface).  Callers pass wi as the outgoing
	// direction from the "incoming" side, so we negate to get toward-surface.
	Ray evalRay( vertex.position, -wi );
	RayIntersectionGeometric ri( evalRay, nullRasterizerState );
	ri.bHit = true;
	ri.ptIntersection = vertex.position;
	ri.vNormal = vertex.normal;
	ri.onb = vertex.onb;

	IORStack stack( 1.0 );
	return pSPF->Pdf( ri, wo, BuildVertexIORStack( vertex, stack ) );
}

//////////////////////////////////////////////////////////////////////
// Helper: visibility test between two points
//////////////////////////////////////////////////////////////////////

bool BDPTIntegrator::IsVisible(
	const IRayCaster& caster,
	const Point3& p1,
	const Point3& p2
	) const
{
	Vector3 d = Vector3Ops::mkVector3( p2, p1 );
	const Scalar dist = Vector3Ops::Magnitude( d );

	if( dist < BDPT_RAY_EPSILON ) {
		return true;
	}

	d = d * (1.0 / dist);

	Ray shadowRay( p1, d );
	shadowRay.Advance( BDPT_RAY_EPSILON );

	// CastShadowRay returns TRUE if something is hit (i.e. NOT visible)
	return !caster.CastShadowRay( shadowRay, dist - 2.0 * BDPT_RAY_EPSILON );
}

//////////////////////////////////////////////////////////////////////
// SampleBSSRDFEntryPoint
//
// Attempts to importance-sample an entry point on a translucent
// surface using the material's diffusion profile.  The disk
// projection method (Christensen & Burley 2015) works as follows:
//
//   1. Choose one of three color channels uniformly at random.
//   2. Choose a projection axis: the surface normal (50%),
//      tangent (25%), or bitangent (25%).  The normal is favored
//      because most nearby surface points are visible from it.
//   3. Sample a radius r from the profile's radial CDF for the
//      chosen channel.  Larger radii = more scattering.
//   4. Sample an angle phi uniformly on [0, 2pi).
//   5. In the plane perpendicular to the chosen axis, offset from
//      the exit point by (r*cos(phi), r*sin(phi)).
//   6. Cast a probe ray along the chosen axis through the object.
//   7. If the probe hits the same object: compute the actual 3D
//      distance r_actual, evaluate Rd(r_actual), and compute the
//      BSSRDF weight.
//   8. Generate a cosine-weighted direction from the entry normal,
//      pointing outward from the surface.
//
// The weight includes Fresnel transmission at the exit point and
// the full Rd(r) profile.  Fresnel transmission at the entry point
// is NOT included — it will be evaluated at the next vertex when
// the cosine-weighted ray interacts with the scene.
//
// Returns valid=false if the probe ray misses the object (caller
// should fall back to surface reflection).
//////////////////////////////////////////////////////////////////////

BDPTIntegrator::BSSRDFSampleResult BDPTIntegrator::SampleBSSRDFEntryPoint(
	const RayIntersectionGeometric& ri,
	const IObject* pObject,
	const IMaterial* pMaterial,
	ISampler& sampler,
	const Scalar nm
	) const
{
	BSSRDFSampleResult result;

	if( !pObject || !pMaterial ) {
		return result;
	}

	ISubSurfaceDiffusionProfile* pProfile = pMaterial->GetDiffusionProfile();
	if( !pProfile ) {
		return result;
	}

	// Exit point geometry
	const Point3& exitPoint = ri.ptIntersection;
	const Vector3& exitNormal = ri.onb.w();
	const Vector3& exitTangent = ri.onb.u();
	const Vector3& exitBitangent = ri.onb.v();

	// Fresnel transmission at exit point
	const Scalar cosExit = fabs( Vector3Ops::Dot( exitNormal,
		Vector3Ops::Normalize( -ri.ray.Dir() ) ) );
	const Scalar FtExit = pProfile->FresnelTransmission( cosExit, ri );

	if( FtExit < 1e-10 ) {
		return result;
	}

	//
	// Step 1: Choose a color channel uniformly
	//
	const int channel = static_cast<int>( sampler.Get1D() * 3.0 );
	const int ch = (channel >= 3) ? 2 : channel;  // clamp

	//
	// Step 2: Choose a projection axis
	//
	const Scalar axisSample = sampler.Get1D();
	Vector3 probeAxis;
	Vector3 perpU, perpV;

	if( axisSample < 0.5 )
	{
		probeAxis = exitNormal;
		perpU = exitTangent;
		perpV = exitBitangent;
	}
	else if( axisSample < 0.75 )
	{
		probeAxis = exitTangent;
		perpU = exitNormal;
		perpV = exitBitangent;
	}
	else
	{
		probeAxis = exitBitangent;
		perpU = exitNormal;
		perpV = exitTangent;
	}

	//
	// Step 3: Sample radius from profile CDF
	//
	const Scalar rSample = pProfile->SampleRadius( sampler.Get1D(), ch, ri );
	if( rSample <= 0 ) {
		return result;
	}

	//
	// Step 4: Sample angle uniformly
	//
	const Scalar phi = TWO_PI * sampler.Get1D();

	//
	// Step 5: Compute probe origin offset in the perpendicular plane
	//
	const Scalar offsetU = rSample * cos( phi );
	const Scalar offsetV = rSample * sin( phi );
	const Point3 probeCenter = Point3Ops::mkPoint3(
		exitPoint,
		perpU * offsetU + perpV * offsetV );

	//
	// Step 6: Cast probe rays in both +axis and -axis directions.
	// Trace the full intersection chain through the object and collect
	// all valid hits, then select one uniformly (PBRT convention).
	//
	struct ProbeHit {
		Point3 point;
		Vector3 normal;
		OrthonormalBasis3D onb;
	};
	std::vector<ProbeHit> hits;
	hits.reserve( 8 );
	// Limit probe distance to the profile's effective range — hits
	// beyond this contribute negligible energy and may cross voids.
	const Scalar probeMaxDist = pProfile->GetMaximumDistanceForError( 1e-4 );
	const int maxProbeHits = 64;  // safety cap

	// Trace all intersections along +axis and -axis
	for( int dir = 0; dir < 2; dir++ )
	{
		const Vector3 probeDir = (dir == 0) ? probeAxis : -probeAxis;
		Ray probeRay( probeCenter, probeDir );
		probeRay.Advance( BDPT_RAY_EPSILON );

		Scalar traveled = 0;
		for( int bounce = 0; bounce < maxProbeHits; bounce++ )
		{
			const Scalar remaining = probeMaxDist - traveled;
			if( remaining < BDPT_RAY_EPSILON ) break;

			RayIntersection probeRI( probeRay, nullRasterizerState );
			pObject->IntersectRay( probeRI, remaining, true, true, false );

			if( !probeRI.geometric.bHit ) break;

			if( probeRI.pModifier ) {
				probeRI.pModifier->Modify( probeRI.geometric );
			}

			ProbeHit h;
			h.point = probeRI.geometric.ptIntersection;
			h.normal = probeRI.geometric.vNormal;
			h.onb = probeRI.geometric.onb;
			hits.push_back( h );

			// Advance ray past this hit
			traveled += probeRI.geometric.range;
			probeRay = Ray( probeRI.geometric.ptIntersection, probeDir );
			probeRay.Advance( BDPT_RAY_EPSILON );
			traveled += BDPT_RAY_EPSILON;
		}
	}

	const int numHits = static_cast<int>( hits.size() );
	if( numHits == 0 ) {
		return result;
	}

	// Select uniformly among all hits
	const int selected = static_cast<int>(
		sampler.Get1D() * numHits );
	const int sel = (selected >= numHits) ? numHits - 1 : selected;

	Point3 entryPoint = hits[sel].point;
	Vector3 entryNormal = hits[sel].normal;
	OrthonormalBasis3D entryONB = hits[sel].onb;

	// Skip if entry point is too close to exit point (self-intersection)
	const Vector3 offset = Vector3Ops::mkVector3( exitPoint, entryPoint );
	const Scalar rActual = Vector3Ops::Magnitude( offset );
	if( rActual < BDPT_RAY_EPSILON ) {
		return result;
	}

	// Skip entry points beyond the profile's effective range.
	// This prevents probe rays from finding distant entry points
	// across voids (e.g., mouth cavity between lips).
	const Scalar maxDist = pProfile->GetMaximumDistanceForError( 1e-4 );
	if( rActual > maxDist ) {
		return result;
	}

	//
	// Step 7: Evaluate profile and compute multi-axis PDF
	//
	// Evaluate Rd(r) at the actual 3D distance between exit and entry.
	const RISEPel Rd = pProfile->EvaluateProfile( rActual, ri );

	// Compute offset in exit-point local frame for projected radii
	const Scalar dN = Vector3Ops::Dot( offset, exitNormal );
	const Scalar dT = Vector3Ops::Dot( offset, exitTangent );
	const Scalar dB = Vector3Ops::Dot( offset, exitBitangent );

	// Projected radii for each axis:
	//   Normal axis:    project onto tangent-bitangent plane
	//   Tangent axis:   project onto normal-bitangent plane
	//   Bitangent axis: project onto normal-tangent plane
	const Scalar rProjN = sqrt( dT*dT + dB*dB );
	const Scalar rProjT = sqrt( dN*dN + dB*dB );
	const Scalar rProjB = sqrt( dN*dN + dT*dT );

	// cosProjection for each axis: |dot(entryNormal, axisDir)|
	// This is the Jacobian converting disk area to surface area.
	const Scalar cosN = fabs( Vector3Ops::Dot( entryNormal, exitNormal ) );
	const Scalar cosT = fabs( Vector3Ops::Dot( entryNormal, exitTangent ) );
	const Scalar cosB = fabs( Vector3Ops::Dot( entryNormal, exitBitangent ) );

	// Sum PDF over all 3 axes × 3 channels (PBRT Pdf_Sp convention).
	// For each axis a with probability pdfAxis[a]:
	//   pdf_disk = PdfR(rProj[a], ch) / (2*pi*rProj[a])
	//   pdf_surface = pdf_disk * cosProj[a]
	// Average over channels and sum over axes.
	const Scalar axisProbs[3] = { 0.5, 0.25, 0.25 };
	const Scalar rProjs[3] = { rProjN, rProjT, rProjB };
	const Scalar cosProjs[3] = { cosN, cosT, cosB };

	Scalar pdfSurface = 0;
	for( int a = 0; a < 3; a++ )
	{
		if( rProjs[a] < 1e-10 || cosProjs[a] < 1e-6 ) {
			continue;
		}

		Scalar channelSum = 0;
		for( int c = 0; c < 3; c++ ) {
			channelSum += pProfile->PdfRadius( rProjs[a], c, ri );
		}
		channelSum /= 3.0;

		pdfSurface += axisProbs[a] * channelSum * cosProjs[a]
			/ (TWO_PI * rProjs[a]);
	}

	// Account for uniform selection among probe hits
	pdfSurface /= static_cast<Scalar>( numHits );

	if( pdfSurface < 1e-20 ) {
		return result;
	}

	//
	// Step 8: Generate cosine-weighted direction from entry point
	//
	OrthonormalBasis3D cosineONB;
	cosineONB.CreateFromW( entryNormal );

	const Scalar u1 = sampler.Get1D();
	const Scalar u2 = sampler.Get1D();
	const Scalar cosTheta = sqrt( u1 );
	const Scalar sinTheta = sqrt( 1.0 - u1 );
	const Scalar phiCosine = TWO_PI * u2;

	const Vector3 cosineDir = Vector3Ops::Normalize(
		cosineONB.u() * (sinTheta * cos(phiCosine)) +
		cosineONB.v() * (sinTheta * sin(phiCosine)) +
		cosineONB.w() * cosTheta );

	//
	// Step 9: Compute entry Fresnel and Sw normalization
	//
	const Scalar eta = pProfile->GetIOR( ri );
	const Scalar F0 = ((eta - 1.0) / (eta + 1.0)) * ((eta - 1.0) / (eta + 1.0));
	const Scalar SwNorm = (41.0 - 20.0 * F0) / 42.0;
	const Scalar FtEntry = pProfile->FresnelTransmission( cosTheta, ri );

	// Full BSSRDF weight:
	//   Rd(r) * Ft(exit) * Ft(entry) / (c * pdfSurface)
	const Scalar SwFactor = (SwNorm > 1e-20) ? FtEntry / SwNorm : FtEntry;
	result.weight = Rd * (FtExit * SwFactor / pdfSurface);

	// Scalar weight for NM path: use spectral profile evaluation when
	// a wavelength is provided, falling back to RGB luminance otherwise.
	if( nm > 0 ) {
		const Scalar RdNM = pProfile->EvaluateProfileNM( rActual, ri, nm );
		result.weightNM = RdNM * FtExit * SwFactor / pdfSurface;
	} else {
		const Scalar RdScalar = 0.2126 * Rd[0] + 0.7152 * Rd[1] + 0.0722 * Rd[2];
		result.weightNM = RdScalar * FtExit * SwFactor / pdfSurface;
	}

	result.entryPoint = entryPoint;
	result.entryNormal = entryNormal;
	result.entryONB = entryONB;
	result.scatteredRay = Ray( entryPoint, cosineDir );
	result.scatteredRay.Advance( BDPT_RAY_EPSILON );
	result.cosinePdf = cosTheta * INV_PI;
	result.pdfSurface = pdfSurface;
	result.valid = true;

	return result;
}


//////////////////////////////////////////////////////////////////////
// GenerateLightSubpath
//
// Traces a path starting from a randomly sampled light source.
// Vertex 0 is the light itself (type LIGHT), subsequent vertices
// are surface intersections.
//
// Light selection and emission sampling are delegated to
// LightSampler, which selects lights proportional to their radiant
// exitance (same strategy used by PhotonTracer for consistency).
//
// Throughput (beta) tracks the path weight beyond vertex 0:
//   beta_0 = Le * |cos(theta_0)| / (pdfSelect * pdfPos * pdfDir)
// At each bounce:
//   delta:     beta *= kray  (kray = f*|cos|/pdf, precomputed by SPF)
//   non-delta: beta *= f * |cos| / pdf  (f from BSDF evaluation)
//////////////////////////////////////////////////////////////////////

unsigned int BDPTIntegrator::GenerateLightSubpath(
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	std::vector<BDPTVertex>& vertices
	) const
{
	vertices.clear();

	if( !pLightSampler ) {
		return 0;
	}

	// We need a LuminaryManager to get the luminaries list.
	// Get it from the caster's luminary manager.
	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	LuminaryManager::LuminariesList emptyList;

	// Use dynamic_cast since LuminaryManager inherits ILuminaryManager via virtual base
	const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
	const LuminaryManager::LuminariesList& luminaries = pLumManager ?
		const_cast<LuminaryManager*>(pLumManager)->getLuminaries() : emptyList;

	const bool useIORStack = BDPTUsesIORStack( caster );
	IORStack iorStack( 1.0 );

	// Phase 0: light source sampling (position + direction)
	sampler.StartStream( 0 );

	LightSample ls;
	if( !pLightSampler->SampleLight( scene, luminaries, sampler, ls ) ) {
		return 0;
	}

	vertices.reserve( maxLightDepth + 1 );

	//
	// Vertex 0: the light source itself
	//
	{
		BDPTVertex v;
		v.type = BDPTVertex::LIGHT;
		v.position = ls.position;
		v.normal = ls.normal;
		v.onb.CreateFromW( ls.normal );
		v.pMaterial = 0;
		v.pObject = 0;
		v.pLight = ls.pLight;
		v.pLuminary = ls.pLuminary;
		v.isDelta = ls.isDelta;
		v.isConnectible = !ls.isDelta;

		// pdfFwd is the probability of generating this light vertex
		// = pdfSelect * pdfPosition
		v.pdfFwd = ls.pdfSelect * ls.pdfPosition;

		// Throughput: Le / (pdfSelect * pdfPosition)
		// but we also need to account for pdfDirection when we trace
		if( v.pdfFwd > 0 ) {
			v.throughput = ls.Le * (1.0 / v.pdfFwd);
		} else {
			v.throughput = RISEPel( 0, 0, 0 );
		}

		v.pdfRev = 0;
		vertices.push_back( v );
	}

	// Check if Le is zero -- no point tracing further
	if( ColorMath::MaxValue( ls.Le ) <= 0 || ls.pdfDirection <= 0 ) {
		return static_cast<unsigned int>( vertices.size() );
	}

	//
	// Trace the emission ray into the scene
	//
	Ray currentRay( ls.position, ls.direction );
	currentRay.Advance( BDPT_RAY_EPSILON );

	// Beta tracks the path throughput beyond vertex 0.
	// After emission:  beta = Le * |cos(theta_0)| / (pdfSelect * pdfPosition * pdfDirection)
	const Scalar cosAtLight = fabs( Vector3Ops::Dot( ls.direction, ls.normal ) );
	RISEPel beta = ls.Le * cosAtLight;
	const Scalar pdfDirArea = ls.pdfDirection;   // already in solid angle for now
	const Scalar pdfEmit = ls.pdfSelect * ls.pdfPosition * pdfDirArea;

	if( pdfEmit > 0 ) {
		beta = beta * (1.0 / pdfEmit);
	} else {
		return static_cast<unsigned int>( vertices.size() );
	}

	Scalar pdfFwdPrev = pdfDirArea;	// solid angle PDF of emission direction

	for( unsigned int depth = 0; depth < maxLightDepth; depth++ )
	{
		// Align to fixed dimension range for this bounce so that
		// cross-pixel Sobol stratification is preserved regardless
		// of how many dimensions previous bounces consumed.
		// Phases 1..15 = light bounces 0..14
		sampler.StartStream( 1 + depth );

		// Intersect the scene
		RayIntersection ri( currentRay, nullRasterizerState );
		scene.GetObjects()->IntersectRay( ri, true, true, false );

		if( !ri.geometric.bHit ) {
			break;
		}

		// Apply intersection modifier if present
		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		// Create a new surface vertex
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = ri.geometric.ptIntersection;
		v.normal = ri.geometric.vNormal;
		v.onb = ri.geometric.onb;
		v.pMaterial = ri.pMaterial;
		v.pObject = ri.pObject;
		v.pLight = 0;
		v.pLuminary = 0;
		if( useIORStack && ri.pObject ) {
			iorStack.SetCurrentObject( ri.pObject );
			v.mediumIOR = iorStack.top();
			v.insideObject = iorStack.containsCurrent();
		}

		// Convert pdfFwdPrev from solid angle to area measure
		const Scalar distSq = ri.geometric.range * ri.geometric.range;
		const Scalar absCosIn = fabs( Vector3Ops::Dot(
			ri.geometric.vNormal,
			-currentRay.Dir() ) );

		v.pdfFwd = BDPTUtilities::SolidAngleToArea( pdfFwdPrev, absCosIn, distSq );
		v.throughput = beta;
		v.pdfRev = 0;

		// Check if the material has a delta BSDF (perfect specular)
		v.isDelta = false;

		if( !ri.pMaterial ) {
			vertices.push_back( v );
			break;
		}

		const ISPF* pSPF = ri.pMaterial->GetSPF();
		if( !pSPF ) {
			vertices.push_back( v );
			break;
		}

		vertices.push_back( v );

		//
		// Sample the SPF for the next direction
		//
		ScatteredRayContainer scattered;
		pSPF->Scatter( ri.geometric, sampler, scattered, useIORStack ? &iorStack : 0 );

		if( scattered.Count() == 0 ) {
			break;
		}

		// Randomly select a scattered ray
		const ScatteredRay* pScat = scattered.RandomlySelect( sampler.Get1D(), false );
		if( !pScat ) {
			break;
		}

		// Compute the discrete lobe selection probability.
		// RandomlySelect picks ray i with probability proportional to MaxValue(kray_i).
		// For single-lobe materials this is 1.0; for multi-lobe (e.g., polished,
		// dielectric) we must account for this in both throughput and stored PDFs.
		Scalar selectProb = 1.0;
		if( scattered.Count() > 1 ) {
			Scalar totalKray = 0;
			for( unsigned int i = 0; i < scattered.Count(); i++ ) {
				totalKray += ColorMath::MaxValue( scattered[i].kray );
			}
			const Scalar selectedKray = ColorMath::MaxValue( pScat->kray );
			if( totalKray > NEARZERO && selectedKray > NEARZERO ) {
				selectProb = selectedKray / totalKray;
			}
		}

		// Determine connectibility: true if any scattered lobe is non-delta
		{
			bool hasNonDelta = false;
			for( unsigned int i = 0; i < scattered.Count(); i++ ) {
				if( !scattered[i].isDelta ) { hasNonDelta = true; break; }
			}
			if( !ri.pMaterial->GetBSDF() ) {
				hasNonDelta = false;
			}
			vertices.back().isConnectible = hasNonDelta;
		}

		// Mark the current vertex as delta if the scattered ray is delta
		vertices.back().isDelta = pScat->isDelta;

		// --- BSSRDF sampling for materials with diffusion profiles ---
		// At front-face hits on SSS surfaces, use a Fresnel coin flip to
		// choose between surface reflection and subsurface transmission.
		// This ensures reflection and subsurface compete with the correct
		// Fresnel-weighted probabilities (Veach/PBRT convention).
		Scalar bssrdfReflectCompensation = 1.0;
		if( ri.pMaterial && ri.pMaterial->GetDiffusionProfile() )
		{
			// No fabs: back-face hits (cosIn < 0) skip BSSRDF,
			// preventing artifacts on thin geometry (lips, eyelids).
			const Scalar cosIn = Vector3Ops::Dot(
				ri.geometric.vNormal, -currentRay.Dir() );
			if( cosIn > NEARZERO )
			{
				ISubSurfaceDiffusionProfile* pProfile = ri.pMaterial->GetDiffusionProfile();
				const Scalar Ft = pProfile->FresnelTransmission( cosIn, ri.geometric );
				const Scalar R = 1.0 - Ft;

				if( Ft > NEARZERO && sampler.Get1D() < Ft )
				{
					// Chose subsurface transmission (probability Ft)
					BSSRDFSampleResult bssrdf = SampleBSSRDFEntryPoint(
						ri.geometric, ri.pObject, ri.pMaterial, sampler );

					if( bssrdf.valid )
					{
						vertices.back().isDelta = true;

						// bssrdf.weight = Rd * Ft / pdfSurface.
						// Ft cancels with selection probability, so
						// effective weight = Rd / pdfSurface.
						beta = beta * bssrdf.weight * (1.0 / Ft);

						BDPTVertex entryV;
						entryV.type = BDPTVertex::SURFACE;
						entryV.position = bssrdf.entryPoint;
						entryV.normal = bssrdf.entryNormal;
						entryV.onb = bssrdf.entryONB;
						entryV.pMaterial = ri.pMaterial;
						entryV.pObject = ri.pObject;
						entryV.isDelta = false;
						entryV.isConnectible = true;
						entryV.isBSSRDFEntry = true;
						entryV.throughput = beta;
						entryV.pdfFwd = bssrdf.pdfSurface;
						entryV.pdfRev = 0;
						vertices.push_back( entryV );

						pdfFwdPrev = bssrdf.cosinePdf;
						currentRay = bssrdf.scatteredRay;
						continue;
					}
					// Probe failed → no valid subsurface sample, terminate
					break;
				}
				// Chose reflection (probability R):
				// The SPF's kray already contains R, so throughput update
				// below gives beta *= R.  We need beta *= R/R = 1, so
				// compensate by dividing by R.
				if( R > NEARZERO ) {
					bssrdfReflectCompensation = 1.0 / R;
				}
			}
		}
		// --- End BSSRDF sampling ---

		// Compute throughput update: beta *= f * |cos| / pdf
		const RISEPel f = EvalBSDFAtVertex(
			vertices.back(),
			-currentRay.Dir(),
			pScat->ray.Dir()
			);

		const Scalar cosTheta = fabs( Vector3Ops::Dot(
			pScat->ray.Dir(),
			ri.geometric.vNormal ) );

		Scalar pdf = pScat->pdf;
		if( pdf <= 0 ) {
			break;
		}

		// For delta scattering, kray already incorporates the right factor
		// but must be divided by the lobe selection probability.
		// For standard non-delta, use the BSDF * cos / pdf formula with
		// pdf scaled by the selection probability.
		if( pScat->isDelta ) {
			beta = beta * pScat->kray * (bssrdfReflectCompensation / selectProb);
		} else {
			const Scalar maxF = ColorMath::MaxValue( f );
			if( maxF <= 0 ) {
				break;
			}
			beta = beta * f * (bssrdfReflectCompensation * cosTheta / (selectProb * pdf));
		}

		// Russian Roulette
		const Scalar rrProb = r_min( Scalar(1.0), ColorMath::MaxValue( beta ) /
			r_max( ColorMath::MaxValue( vertices.back().throughput ), Scalar(BDPT_RR_THRESHOLD) ) );

		if( depth > 2 ) {
			if( sampler.Get1D() >= rrProb ) {
				break;
			}
			if( rrProb > 0 ) {
				beta = beta * (1.0 / rrProb);
			}
		}

		// Store the forward pdf for the next vertex (solid angle measure),
		// accounting for lobe selection probability.
		pdfFwdPrev = selectProb * pdf;

		// In Veach's formulation, delta vertices should be "transparent" in the MIS walk
		if( pScat->isDelta ) {
			pdfFwdPrev = 0;
		}

		// Update the previous vertex's pdfRev
		// pdfRev of vertex[n-1] = pdf of sampling the reverse direction at vertex[n]
		if( vertices.size() >= 2 ) {
			const BDPTVertex& curr = vertices.back();
			BDPTVertex& prev = vertices[ vertices.size() - 2 ];

			// Reverse PDF: EvalPdfAtVertex returns 0 for delta interactions
			// (SPF::Pdf() returns 0 for Dirac distributions).  This is correct;
			// remap0 in MISWeight maps the zero to 1 so the ratio chain
			// propagates through delta vertices without dying.
			const Scalar revPdfSA = EvalPdfAtVertex(
				curr,
				pScat->ray.Dir(),
				-currentRay.Dir()
				);

			// Convert to area measure at prev
			const Scalar d2 = distSq;
			const Scalar absCosAtPrev = (prev.type == BDPTVertex::LIGHT) ?
				fabs( Vector3Ops::Dot( prev.normal, currentRay.Dir() ) ) :
				fabs( Vector3Ops::Dot( prev.normal, currentRay.Dir() ) );

			prev.pdfRev = BDPTUtilities::SolidAngleToArea( revPdfSA, absCosAtPrev, d2 );
			if( pScat->isDelta ) { prev.pdfRev = 0; }
		}

		// Advance to next ray
		currentRay = pScat->ray;
		currentRay.Advance( BDPT_RAY_EPSILON );
		if( useIORStack && pScat->ior_stack ) {
			iorStack = *pScat->ior_stack;
		}
	}

	return static_cast<unsigned int>( vertices.size() );
}

//////////////////////////////////////////////////////////////////////
// GenerateEyeSubpath
//
// Traces a path starting from the camera.  Vertex 0 is the camera
// itself (type CAMERA, pdfFwd=1, throughput=1), subsequent vertices
// are surface intersections.
//
// The camera's directional PDF (BDPTCameraUtilities::PdfDirection)
// is used as pdfFwdPrev for the first surface vertex.  For pinhole
// cameras this is 1/cos^3(theta) * focaldist^2 (Veach eq. 8.10).
//////////////////////////////////////////////////////////////////////

unsigned int BDPTIntegrator::GenerateEyeSubpath(
	const RuntimeContext& rc,
	const Ray& cameraRay,
	const Point2& screenPos,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	std::vector<BDPTVertex>& vertices
	) const
{
	vertices.clear();
	vertices.reserve( maxEyeDepth + 1 );

	//
	// Vertex 0: the camera
	//
	{
		const ICamera* pCamera = scene.GetCamera();

		BDPTVertex v;
		v.type = BDPTVertex::CAMERA;

		if( pCamera ) {
			v.position = pCamera->GetLocation();
		}

		v.normal = cameraRay.Dir();
		v.onb.CreateFromW( cameraRay.Dir() );
		v.pMaterial = 0;
		v.pObject = 0;
		v.pLight = 0;
		v.pLuminary = 0;
		v.screenPos = screenPos;
		v.isDelta = false;

		// pdfFwd = 1 for a specific pixel
		v.pdfFwd = 1.0;
		v.pdfRev = 0;
		v.throughput = RISEPel( 1, 1, 1 );

		vertices.push_back( v );
	}

	// Trace the camera ray
	Ray currentRay = cameraRay;
	RISEPel beta( 1, 1, 1 );

	// The camera pdf for generating this direction
	const ICamera* pCamera = scene.GetCamera();
	Scalar pdfCamDir = 1.0;
	if( pCamera ) {
		pdfCamDir = BDPTCameraUtilities::PdfDirection( *pCamera, cameraRay );
	}

	Scalar pdfFwdPrev = pdfCamDir;
	const bool useIORStack = BDPTUsesIORStack( caster );
	IORStack iorStack( 1.0 );

	for( unsigned int depth = 0; depth < maxEyeDepth; depth++ )
	{
		// Align to fixed dimension range for this bounce.
		// Phases 16..30 = eye bounces 0..14
		sampler.StartStream( 16 + depth );

		// Intersect the scene
		RayIntersection ri( currentRay, nullRasterizerState );
		scene.GetObjects()->IntersectRay( ri, true, true, false );

		if( !ri.geometric.bHit ) {
			break;
		}

		// Apply intersection modifier if present
		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		// Create a new surface vertex
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = ri.geometric.ptIntersection;
		v.normal = ri.geometric.vNormal;
		v.onb = ri.geometric.onb;
		v.pMaterial = ri.pMaterial;
		v.pObject = ri.pObject;
		v.pLight = 0;
		v.pLuminary = 0;
		if( useIORStack && ri.pObject ) {
			iorStack.SetCurrentObject( ri.pObject );
			v.mediumIOR = iorStack.top();
			v.insideObject = iorStack.containsCurrent();
		}

		// Convert pdfFwdPrev from solid angle to area measure
		const Scalar distSq = ri.geometric.range * ri.geometric.range;
		const Scalar absCosIn = fabs( Vector3Ops::Dot(
			ri.geometric.vNormal,
			-currentRay.Dir() ) );

		v.pdfFwd = BDPTUtilities::SolidAngleToArea( pdfFwdPrev, absCosIn, distSq );
		v.throughput = beta;
		v.pdfRev = 0;
		v.isDelta = false;

		if( !ri.pMaterial ) {
			vertices.push_back( v );
			break;
		}

		const ISPF* pSPF = ri.pMaterial->GetSPF();
		if( !pSPF ) {
			vertices.push_back( v );
			break;
		}

		vertices.push_back( v );

		//
		// Sample the SPF for the next direction
		//
		ScatteredRayContainer scattered;
		pSPF->Scatter( ri.geometric, sampler, scattered, useIORStack ? &iorStack : 0 );

		if( scattered.Count() == 0 ) {
			break;
		}

		// Randomly select a scattered ray
		const ScatteredRay* pScat = scattered.RandomlySelect( sampler.Get1D(), false );
		if( !pScat ) {
			break;
		}

		// Compute the discrete lobe selection probability
		Scalar selectProb = 1.0;
		if( scattered.Count() > 1 ) {
			Scalar totalKray = 0;
			for( unsigned int i = 0; i < scattered.Count(); i++ ) {
				totalKray += ColorMath::MaxValue( scattered[i].kray );
			}
			const Scalar selectedKray = ColorMath::MaxValue( pScat->kray );
			if( totalKray > NEARZERO && selectedKray > NEARZERO ) {
				selectProb = selectedKray / totalKray;
			}
		}

		// Determine connectibility: true if any scattered lobe is non-delta
		{
			bool hasNonDelta = false;
			for( unsigned int i = 0; i < scattered.Count(); i++ ) {
				if( !scattered[i].isDelta ) { hasNonDelta = true; break; }
			}
			if( !ri.pMaterial->GetBSDF() ) {
				hasNonDelta = false;
			}
			vertices.back().isConnectible = hasNonDelta;
		}

		// Mark the current vertex as delta
		vertices.back().isDelta = pScat->isDelta;

		// --- BSSRDF sampling for materials with diffusion profiles ---
		Scalar bssrdfReflectCompensation = 1.0;
		if( ri.pMaterial && ri.pMaterial->GetDiffusionProfile() )
		{
			const Scalar cosIn = Vector3Ops::Dot(
				ri.geometric.vNormal, -currentRay.Dir() );
			if( cosIn > NEARZERO )
			{
				ISubSurfaceDiffusionProfile* pProfile = ri.pMaterial->GetDiffusionProfile();
				const Scalar Ft = pProfile->FresnelTransmission( cosIn, ri.geometric );
				const Scalar R = 1.0 - Ft;

				if( Ft > NEARZERO && sampler.Get1D() < Ft )
				{
					BSSRDFSampleResult bssrdf = SampleBSSRDFEntryPoint(
						ri.geometric, ri.pObject, ri.pMaterial, sampler );

					if( bssrdf.valid )
					{
						vertices.back().isDelta = true;
						beta = beta * bssrdf.weight * (1.0 / Ft);

						BDPTVertex entryV;
						entryV.type = BDPTVertex::SURFACE;
						entryV.position = bssrdf.entryPoint;
						entryV.normal = bssrdf.entryNormal;
						entryV.onb = bssrdf.entryONB;
						entryV.pMaterial = ri.pMaterial;
						entryV.pObject = ri.pObject;
						entryV.isDelta = false;
						entryV.isConnectible = true;
						entryV.isBSSRDFEntry = true;
						entryV.throughput = beta;
						entryV.pdfFwd = bssrdf.pdfSurface;
						entryV.pdfRev = 0;
						vertices.push_back( entryV );

						pdfFwdPrev = bssrdf.cosinePdf;
						currentRay = bssrdf.scatteredRay;
						continue;
					}
					break;
				}
				if( R > NEARZERO ) {
					bssrdfReflectCompensation = 1.0 / R;
				}
			}
		}
		// --- End BSSRDF sampling ---

		// Compute throughput update
		const RISEPel f = EvalBSDFAtVertex(
			vertices.back(),
			pScat->ray.Dir(),
			-currentRay.Dir()
			);

		const Scalar cosTheta = fabs( Vector3Ops::Dot(
			pScat->ray.Dir(),
			ri.geometric.vNormal ) );

		Scalar pdf = pScat->pdf;
		if( pdf <= 0 ) {
			break;
		}

		if( pScat->isDelta ) {
			beta = beta * pScat->kray * (bssrdfReflectCompensation / selectProb);
		} else {
			const Scalar maxF = ColorMath::MaxValue( f );
			if( maxF <= 0 ) {
				break;
			}
			beta = beta * f * (bssrdfReflectCompensation * cosTheta / (selectProb * pdf));
		}

		// Russian Roulette after a few bounces
		if( depth > 2 ) {
			const Scalar maxBeta = ColorMath::MaxValue( beta );
			const Scalar rrProb = r_min( Scalar(1.0), maxBeta );
			if( sampler.Get1D() >= rrProb ) {
				break;
			}
			if( rrProb > 0 ) {
				beta = beta * (1.0 / rrProb);
			}
		}

		// Store the forward pdf for the next vertex,
		// accounting for lobe selection probability.
		pdfFwdPrev = selectProb * pdf;

		// In Veach's formulation, delta vertices should be "transparent" in the MIS walk
		if( pScat->isDelta ) {
			pdfFwdPrev = 0;
		}

		// Update previous vertex's pdfRev
		if( vertices.size() >= 2 ) {
			const BDPTVertex& curr = vertices.back();
			BDPTVertex& prev = vertices[ vertices.size() - 2 ];

			// Reverse PDF: returns 0 for delta interactions, handled by remap0 in MISWeight.
			const Scalar revPdfSA = EvalPdfAtVertex(
				curr,
				pScat->ray.Dir(),
				-currentRay.Dir()
				);

			// Convert to area measure at prev
			const Scalar absCosAtPrev = (prev.type == BDPTVertex::CAMERA) ?
				Scalar(1.0) :
				fabs( Vector3Ops::Dot( prev.normal, currentRay.Dir() ) );

			prev.pdfRev = BDPTUtilities::SolidAngleToArea( revPdfSA, absCosAtPrev, distSq );
			if( pScat->isDelta ) { prev.pdfRev = 0; }
		}

		// Advance to next ray
		currentRay = pScat->ray;
		currentRay.Advance( BDPT_RAY_EPSILON );
		if( useIORStack && pScat->ior_stack ) {
			iorStack = *pScat->ior_stack;
		}
	}

	return static_cast<unsigned int>( vertices.size() );
}

//////////////////////////////////////////////////////////////////////
// ConnectAndEvaluate - evaluate a single (s,t) strategy.
//
// Each case handles a different connection topology:
//   s=0:       Eye path hits emitter.  No connection needed.
//   s=1, t>1:  Next event estimation (direct lighting).
//   t=1:       Light endpoint connects to camera.  Needs splatting.
//   s>1, t>1:  General connection between two interior vertices.
//
// For strategies that reach the camera from the light side (t==1),
// the contribution lands at an arbitrary pixel, so needsSplat=true
// and rasterPos is computed via BDPTCameraUtilities::Rasterize().
//
// Delta vertices cannot participate in explicit connections since
// there is zero probability of the connection direction matching
// the specular direction.
//////////////////////////////////////////////////////////////////////

BDPTIntegrator::ConnectionResult BDPTIntegrator::ConnectAndEvaluate(
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<BDPTVertex>& eyeVerts,
	unsigned int s,
	unsigned int t,
	const IScene& scene,
	const IRayCaster& caster,
	const ICamera& camera
	) const
{
	ConnectionResult result;

	// Validate: s <= lightVerts.size(), t <= eyeVerts.size(), s+t >= 2
	if( s > lightVerts.size() || t > eyeVerts.size() ) {
		return result;
	}

	if( s + t < 2 ) {
		return result;
	}

	//
	// Case: s == 0, t > 0
	// Pure eye path -- last eye vertex hits an emitter directly
	//
	if( s == 0 )
	{
		const BDPTVertex& eyeEnd = eyeVerts[t - 1];

		if( eyeEnd.type != BDPTVertex::SURFACE ) {
			return result;
		}

		if( !eyeEnd.pMaterial ) {
			return result;
		}

		const IEmitter* pEmitter = eyeEnd.pMaterial->GetEmitter();
		if( !pEmitter ) {
			return result;
		}

		// The eye path naturally arrived at an emitter
		// Direction from the surface is the reverse of how we arrived
		// We need the direction *from* the emitter surface (outgoing toward camera)
		Vector3 woFromEmitter;
		if( t >= 2 ) {
			woFromEmitter = Vector3Ops::mkVector3( eyeVerts[t - 2].position, eyeEnd.position );
			woFromEmitter = Vector3Ops::Normalize( woFromEmitter );
		} else {
			// t == 1 means the camera vertex is the emitter, which doesn't make sense
			return result;
		}

		// Evaluate emitted radiance at this point
		RayIntersectionGeometric rig( Ray( eyeEnd.position, woFromEmitter ), nullRasterizerState );
		rig.bHit = true;
		rig.ptIntersection = eyeEnd.position;
		rig.vNormal = eyeEnd.normal;
		rig.onb = eyeEnd.onb;

		const RISEPel Le = pEmitter->emittedRadiance( rig, woFromEmitter, eyeEnd.normal );

		if( ColorMath::MaxValue( Le ) <= 0 ) {
			return result;
		}

		result.contribution = eyeEnd.throughput * Le;
		result.needsSplat = false;
		result.valid = true;

		// --- Update pdfRev at emitter vertex for correct MIS ---
		// eyeEnd.pdfRev should be the PDF that the light sampling process
		// would have generated this point: pdfSelect * pdfPosition.
		const Scalar savedEyeEndPdfRev = eyeEnd.pdfRev;

		if( pLightSampler && eyeEnd.pObject )
		{
			const ILuminaryManager* pLumMgr = caster.GetLuminaries();
			const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
			LuminaryManager::LuminariesList emptyList;
			const LuminaryManager::LuminariesList& luminaries = pLumManager ?
				const_cast<LuminaryManager*>( pLumManager )->getLuminaries() : emptyList;

			const Scalar pdfSelect = pLightSampler->PdfSelectLuminary(
				scene, luminaries, *eyeEnd.pObject );
			const Scalar area = eyeEnd.pObject->GetArea();
			const Scalar pdfPosition = (area > 0) ? (Scalar(1.0) / area) : 0;
			const_cast<BDPTVertex&>( eyeEnd ).pdfRev = pdfSelect * pdfPosition;
		}

		// --- Update predecessor pdfRev (eyeVerts[t-2]) ---
		// The emission directional PDF from the emitter toward the predecessor
		// vertex determines pdfRev at the predecessor.
		Scalar savedEyePredPdfRev = 0;
		const bool hasEyePred = (t >= 2);
		if( hasEyePred )
		{
			const BDPTVertex& eyePred = eyeVerts[t - 2];
			savedEyePredPdfRev = eyePred.pdfRev;

			// Emission directional PDF at eyeEnd toward eyePred
			Scalar emPdfDir = 0;
			if( eyeEnd.pMaterial ) {
				const IEmitter* pEm = eyeEnd.pMaterial->GetEmitter();
				if( pEm ) {
					// Cosine-weighted hemisphere emission
					const Scalar cosAtEmitter = fabs( Vector3Ops::Dot( eyeEnd.normal, woFromEmitter ) );
					emPdfDir = (cosAtEmitter > 0) ? (cosAtEmitter * INV_PI) : 0;
				}
			}

			const Vector3 dToPred = Vector3Ops::mkVector3( eyePred.position, eyeEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			// Camera vertex has no meaningful surface normal; use 1.0
			const Scalar absCosAtPred = (eyePred.type == BDPTVertex::CAMERA) ?
				Scalar(1.0) :
				fabs( Vector3Ops::Dot( eyePred.normal,
					Vector3Ops::Normalize( dToPred ) ) );
			const_cast<BDPTVertex&>( eyePred ).pdfRev =
				BDPTUtilities::SolidAngleToArea( emPdfDir, absCosAtPred, distPredSq );
		}

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );

		const_cast<BDPTVertex&>( eyeEnd ).pdfRev = savedEyeEndPdfRev;
		if( hasEyePred ) {
			const_cast<BDPTVertex&>( eyeVerts[t - 2] ).pdfRev = savedEyePredPdfRev;
		}

		return result;
	}

	//
	// Legacy t == 0 path-to-camera case.
	// The active path enumeration in this file includes the camera as
	// eye vertex 0, so the camera-connection strategy is t == 1.
	// This branch is kept only for compatibility with any future caller
	// that enumerates paths without an explicit camera vertex.
	//
	if( t == 0 )
	{
		// Light path endpoint connects directly to camera sensor
		const BDPTVertex& lightEnd = lightVerts[s - 1];

		if( !lightEnd.isConnectible ) {
			return result;
		}

		// Project the light vertex position onto the camera
		Point2 rasterPos;
		if( !BDPTCameraUtilities::Rasterize( camera, lightEnd.position, rasterPos ) ) {
			return result;
		}

		// Check visibility from camera to light vertex using standard shadow ray.
		const Point3 camPos = camera.GetLocation();
		if( !IsVisible( caster, camPos, lightEnd.position ) ) {
			return result;
		}

		// Compute the camera importance
		Vector3 dirToCam = Vector3Ops::mkVector3( camPos, lightEnd.position );
		const Scalar dist = Vector3Ops::Magnitude( dirToCam );
		if( dist < BDPT_RAY_EPSILON ) {
			return result;
		}
		dirToCam = dirToCam * (1.0 / dist);

		Ray camRay( camPos, -dirToCam );
		const Scalar We = BDPTCameraUtilities::Importance( camera, camRay );

		if( We <= 0 ) {
			return result;
		}

		// Evaluate BSDF at the light endpoint for the direction toward camera
		RISEPel fLight( 1, 1, 1 );
		if( lightEnd.type == BDPTVertex::SURFACE && lightEnd.pMaterial ) {
			Vector3 wiAtLight;
			if( s >= 2 ) {
				wiAtLight = Vector3Ops::mkVector3( lightVerts[s - 2].position, lightEnd.position );
				wiAtLight = Vector3Ops::Normalize( wiAtLight );
			} else {
				// s == 1: the light source vertex itself; fLight from emitter Le is in throughput
				fLight = RISEPel( 1, 1, 1 );
			}

			if( s >= 2 ) {
				fLight = EvalBSDFAtVertex( lightEnd, wiAtLight, dirToCam );
			}
		}

		// Geometric term between light endpoint and camera
		const Scalar distSq = dist * dist;
		const Scalar absCosLight = lightEnd.isDelta ?
			Scalar(1.0) : fabs( Vector3Ops::Dot( lightEnd.normal, dirToCam ) );
		const Scalar G = absCosLight / distSq;

		result.contribution = lightEnd.throughput * fLight * (G * We);
		result.rasterPos = rasterPos;
		result.needsSplat = true;
		result.valid = true;

		// --- Update pdfRev at light endpoint for correct MIS ---
		// lightEnd.pdfRev: PDF that camera would generate lightEnd
		// = camera's directional PDF converted to area at lightEnd
		const Scalar savedLightPdfRev = lightEnd.pdfRev;
		{
			Ray camRayToLight( camPos, -dirToCam );
			const Scalar camPdfDir = BDPTCameraUtilities::PdfDirection( camera, camRayToLight );
			const_cast<BDPTVertex&>( lightEnd ).pdfRev =
				BDPTUtilities::SolidAngleToArea( camPdfDir, absCosLight, distSq );
		}

		// --- Update predecessor pdfRev (lightVerts[s-2]) ---
		// PDF at lightEnd of scattering toward lightVerts[s-2] given incoming = dirToCam
		Scalar savedLightPredPdfRev_t0 = 0;
		const bool hasLightPred_t0 = (s >= 2);
		if( hasLightPred_t0 )
		{
			const BDPTVertex& lightPred = lightVerts[s - 2];
			savedLightPredPdfRev_t0 = lightPred.pdfRev;

			Vector3 wiAtLightEnd;
			if( s >= 2 ) {
				wiAtLightEnd = Vector3Ops::mkVector3( lightVerts[s - 2].position, lightEnd.position );
				wiAtLightEnd = Vector3Ops::Normalize( wiAtLightEnd );
			}

			const Scalar pdfPredSA = EvalPdfAtVertex( lightEnd, dirToCam, -wiAtLightEnd );
			const Vector3 dToPred = Vector3Ops::mkVector3( lightPred.position, lightEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			const Scalar absCosAtPred = fabs( Vector3Ops::Dot( lightPred.normal,
				Vector3Ops::Normalize( dToPred ) ) );
			const_cast<BDPTVertex&>( lightPred ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfPredSA, absCosAtPred, distPredSq );
		}

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );

		const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRev;
		if( hasLightPred_t0 ) {
			const_cast<BDPTVertex&>( lightVerts[s - 2] ).pdfRev = savedLightPredPdfRev_t0;
		}

		return result;
	}

	//
	// Case: s == 1, t > 0
	// Connect the last eye vertex to a new light sample (next event estimation)
	//
	if( s == 1 )
	{
		const BDPTVertex& eyeEnd = eyeVerts[t - 1];
		const BDPTVertex& lightStart = lightVerts[0];

		if( eyeEnd.type != BDPTVertex::SURFACE || !eyeEnd.pMaterial ) {
			return result;
		}

		if( !eyeEnd.isConnectible ) {
			return result;
		}

		// Direction from eye vertex to light
		Vector3 dirToLight = Vector3Ops::mkVector3( lightStart.position, eyeEnd.position );
		const Scalar dist = Vector3Ops::Magnitude( dirToLight );
		if( dist < BDPT_RAY_EPSILON ) {
			return result;
		}
		dirToLight = dirToLight * (1.0 / dist);

		// Check visibility
		if( !IsVisible( caster, eyeEnd.position, lightStart.position ) ) {
			return result;
		}

		// Evaluate emitted radiance from the light toward the eye vertex
		RISEPel Le( 0, 0, 0 );
		if( lightStart.pLight ) {
			Le = lightStart.pLight->emittedRadiance( -dirToLight );
		} else if( lightStart.pLuminary && lightStart.pLuminary->GetMaterial() ) {
			const IEmitter* pEmitter = lightStart.pLuminary->GetMaterial()->GetEmitter();
			if( pEmitter ) {
				RayIntersectionGeometric rig(
					Ray( lightStart.position, -dirToLight ),
					nullRasterizerState );
				rig.bHit = true;
				rig.ptIntersection = lightStart.position;
				rig.vNormal = lightStart.normal;
				rig.onb = lightStart.onb;

				Le = pEmitter->emittedRadiance(
					rig,
					-dirToLight,
					lightStart.normal );
			}
		}

		if( ColorMath::MaxValue( Le ) <= 0 ) {
			return result;
		}

		// Evaluate BSDF at the eye endpoint
		Vector3 woAtEye;
		if( t >= 2 ) {
			woAtEye = Vector3Ops::mkVector3( eyeVerts[t - 2].position, eyeEnd.position );
			woAtEye = Vector3Ops::Normalize( woAtEye );
		} else {
			// t == 1 means connecting camera directly to light, handled by t==0 case above
			return result;
		}

		const RISEPel fEye = EvalBSDFAtVertex( eyeEnd, dirToLight, woAtEye );

		if( ColorMath::MaxValue( fEye ) <= 0 ) {
			return result;
		}

		// Geometric term
		// For delta-position lights (point/spot), the light has no surface
		// so the geometric coupling excludes the cosine at the light vertex.
		Scalar G;
		if( lightStart.isDelta ) {
			const Scalar dist2 = dist * dist;
			const Scalar absCosEye = fabs( Vector3Ops::Dot( eyeEnd.normal, dirToLight ) );
			G = absCosEye / dist2;
		} else {
			G = BDPTUtilities::GeometricTerm(
				lightStart.position, lightStart.normal,
				eyeEnd.position, eyeEnd.normal );
		}

		// Contribution: eyeThroughput * fEye * G * Le / pdfLight
		// lightStart.throughput already has Le / (pdfSelect * pdfPosition)
		// but we need to re-evaluate since the direction changed
		// For s=1, we use: lightVerts[0].throughput * fEye * G
		// where lightVerts[0].throughput = Le / (pdfSelect * pdfPosition)

		// Actually for s=1, the light vertex stores throughput = Le / pdf_pos_select
		// We just need fEye * G * lightThroughput * eyeThroughput
		// But Le from light depends on the connection direction, which differs
		// from the sampled direction.  Re-evaluate:
		const Scalar pdfLight = lightStart.pdfFwd;
		if( pdfLight <= 0 ) {
			return result;
		}

		result.contribution = eyeEnd.throughput * fEye * Le * (G / pdfLight);
		result.needsSplat = false;
		result.valid = true;

		// --- Update pdfRev at connection vertices for correct MIS ---
		const Scalar distSq_conn = dist * dist;
		const Scalar savedLightPdfRev = lightStart.pdfRev;
		const Scalar savedEyePdfRev = eyeEnd.pdfRev;

		// lightStart.pdfRev: PDF that eye-side process would "find" the light
		// For delta-position lights (point/spot), leave at 0 (eye can't hit a point)
		if( !lightStart.isDelta ) {
			const Scalar pdfRevSA = EvalPdfAtVertex( eyeEnd, woAtEye, dirToLight );
			const Scalar absCosAtLight = fabs( Vector3Ops::Dot( lightStart.normal, dirToLight ) );
			const_cast<BDPTVertex&>( lightStart ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfRevSA, absCosAtLight, distSq_conn );
		}

		// eyeEnd.pdfRev: PDF that light-side would generate eyeEnd
		// = emission directional PDF at light toward eyeEnd, converted to area at eyeEnd
		{
			Scalar emissionPdfDir = 0;
			if( lightStart.pLuminary ) {
				// Mesh luminary: cosine-weighted hemisphere emission
				const Scalar cosAtLight = fabs( Vector3Ops::Dot( lightStart.normal, -dirToLight ) );
				emissionPdfDir = (cosAtLight > 0) ? (cosAtLight * INV_PI) : 0;
			} else if( lightStart.pLight ) {
				emissionPdfDir = lightStart.pLight->pdfDirection( -dirToLight );
			}
			const Scalar absCosAtEye = fabs( Vector3Ops::Dot( eyeEnd.normal, dirToLight ) );
			const_cast<BDPTVertex&>( eyeEnd ).pdfRev =
				BDPTUtilities::SolidAngleToArea( emissionPdfDir, absCosAtEye, distSq_conn );
		}

		// --- Update predecessor pdfRev (eyeVerts[t-2]) ---
		// PDF at eyeEnd of scattering toward eyeVerts[t-2] given incoming = dirToLight
		Scalar savedEyePredPdfRev = 0;
		const bool hasEyePred_s1 = (t >= 2);
		if( hasEyePred_s1 )
		{
			const BDPTVertex& eyePred = eyeVerts[t - 2];
			savedEyePredPdfRev = eyePred.pdfRev;

			const Vector3 dToPred = Vector3Ops::mkVector3( eyePred.position, eyeEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			const Vector3 dirToPred = Vector3Ops::Normalize( dToPred );
			const Scalar pdfPredSA = EvalPdfAtVertex( eyeEnd, dirToLight, dirToPred );
			// Camera vertex has no meaningful surface normal; use 1.0
			const Scalar absCosAtPred = (eyePred.type == BDPTVertex::CAMERA) ?
				Scalar(1.0) :
				fabs( Vector3Ops::Dot( eyePred.normal, dirToPred ) );
			const_cast<BDPTVertex&>( eyePred ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfPredSA, absCosAtPred, distPredSq );
		}

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );

		const_cast<BDPTVertex&>( lightStart ).pdfRev = savedLightPdfRev;
		const_cast<BDPTVertex&>( eyeEnd ).pdfRev = savedEyePdfRev;
		if( hasEyePred_s1 ) {
			const_cast<BDPTVertex&>( eyeVerts[t - 2] ).pdfRev = savedEyePredPdfRev;
		}

		return result;
	}

	//
	// Case: s > 0, t == 1
	// Connect last light vertex to the camera
	//
	if( t == 1 )
	{
		const BDPTVertex& lightEnd = lightVerts[s - 1];

		if( !lightEnd.isConnectible ) {
			return result;
		}

		// Only connect surface vertices to camera
		if( s >= 2 && lightEnd.type != BDPTVertex::SURFACE ) {
			return result;
		}

		// Project light vertex onto camera
		Point2 rasterPos;
		if( !BDPTCameraUtilities::Rasterize( camera, lightEnd.position, rasterPos ) ) {
			return result;
		}

		// This strategy is only valid for a direct camera connection.
		// Refractive blockers must be sampled as explicit specular eye
		// vertices; treating them as transparent here produces invalid
		// splats and severe caustic fireflies.
		const Point3 camPos = camera.GetLocation();
		if( !IsVisible( caster, lightEnd.position, camPos ) ) {
			return result;
		}

		// Direction from light vertex to camera
		Vector3 dirToCam = Vector3Ops::mkVector3( camPos, lightEnd.position );
		const Scalar dist = Vector3Ops::Magnitude( dirToCam );
		if( dist < BDPT_RAY_EPSILON ) {
			return result;
		}
		dirToCam = dirToCam * (1.0 / dist);

		// Camera importance
		Ray camRay( camPos, -dirToCam );
		const Scalar We = BDPTCameraUtilities::Importance( camera, camRay );
		if( We <= 0 ) {
			return result;
		}

		// Evaluate BSDF at the light endpoint for connection to camera
		RISEPel fLight( 1, 1, 1 );
		if( lightEnd.type == BDPTVertex::SURFACE && lightEnd.pMaterial ) {
			Vector3 wiAtLight;
			if( s >= 2 ) {
				wiAtLight = Vector3Ops::mkVector3( lightVerts[s - 2].position, lightEnd.position );
				wiAtLight = Vector3Ops::Normalize( wiAtLight );
			}

			if( s >= 2 ) {
				fLight = EvalBSDFAtVertex( lightEnd, wiAtLight, dirToCam );
			}
		} else if( lightEnd.type == BDPTVertex::LIGHT ) {
			// s == 1: the light source directly connects to camera
			// Contribution is Le in the direction toward camera
			if( lightEnd.pLight ) {
				fLight = lightEnd.pLight->emittedRadiance( dirToCam );
				// Re-scale: throughput already has Le/pdf, but Le was for the sampled direction
				// So we want Le(dirToCam) / Le(sampled) * throughput contribution
				// Simplify: just use throughput which is Le/pdf, direction-dependent Le re-evaluated
			} else if( lightEnd.pLuminary && lightEnd.pLuminary->GetMaterial() ) {
				const IEmitter* pEmitter = lightEnd.pLuminary->GetMaterial()->GetEmitter();
				if( pEmitter ) {
					RayIntersectionGeometric rig(
						Ray( lightEnd.position, dirToCam ), nullRasterizerState );
					rig.bHit = true;
					rig.ptIntersection = lightEnd.position;
					rig.vNormal = lightEnd.normal;
					rig.onb = lightEnd.onb;
					fLight = pEmitter->emittedRadiance( rig, dirToCam, lightEnd.normal );
				}
			}

			// For s=1, contribution = Le(dirToCam) * We * G / pdfLight
			const Scalar pdfLight = lightEnd.pdfFwd;
			if( pdfLight <= 0 ) {
				return result;
			}

			const Scalar distSq = dist * dist;
			const Scalar absCosLight = lightEnd.isDelta ?
				Scalar(1.0) : fabs( Vector3Ops::Dot( lightEnd.normal, dirToCam ) );
			const Scalar G = absCosLight / distSq;

			result.contribution = fLight * (G * We / pdfLight);
			result.rasterPos = rasterPos;
			result.needsSplat = true;
			result.valid = true;

			// --- Update pdfRev at connection vertices for correct MIS ---
			const Scalar savedLightPdfRev = lightEnd.pdfRev;
			const Scalar savedEyePdfRev = eyeVerts[0].pdfRev;

			// lightEnd.pdfRev: camera's directional PDF at lightEnd
			{
				Ray camRayToLight( camPos, -dirToCam );
				const Scalar camPdfDir = BDPTCameraUtilities::PdfDirection( camera, camRayToLight );
				const_cast<BDPTVertex&>( lightEnd ).pdfRev =
					BDPTUtilities::SolidAngleToArea( camPdfDir, absCosLight, distSq );
			}

			// eyeVerts[0].pdfRev: emission directional PDF toward camera
			{
				Scalar emPdfDir = 0;
				if( lightEnd.pLuminary ) {
					const Scalar cosEmit = fabs( Vector3Ops::Dot( lightEnd.normal, dirToCam ) );
					emPdfDir = (cosEmit > 0) ? (cosEmit * INV_PI) : 0;
				} else if( lightEnd.pLight ) {
					emPdfDir = lightEnd.pLight->pdfDirection( dirToCam );
				}
				const_cast<BDPTVertex&>( eyeVerts[0] ).pdfRev =
					BDPTUtilities::SolidAngleToArea( emPdfDir, Scalar(1.0), distSq );
			}

			result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );
			const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRev;
			const_cast<BDPTVertex&>( eyeVerts[0] ).pdfRev = savedEyePdfRev;
			return result;
		}

		if( ColorMath::MaxValue( fLight ) <= 0 ) {
			return result;
		}

		// Geometric term (camera has no surface normal, use 1/dist^2)
		const Scalar distSq = dist * dist;
		const Scalar absCosLight = fabs( Vector3Ops::Dot( lightEnd.normal, dirToCam ) );
		const Scalar G = absCosLight / distSq;

		result.contribution = lightEnd.throughput * fLight * (G * We);
		result.rasterPos = rasterPos;
		result.needsSplat = true;
		result.valid = true;

		// --- Update pdfRev at connection vertices for correct MIS ---
		const Scalar savedLightPdfRev = lightEnd.pdfRev;
		const Scalar savedEyePdfRev = eyeVerts[0].pdfRev;

		// lightEnd.pdfRev: camera's directional PDF at lightEnd
		{
			Ray camRayToLight( camPos, -dirToCam );
			const Scalar camPdfDir = BDPTCameraUtilities::PdfDirection( camera, camRayToLight );
			const_cast<BDPTVertex&>( lightEnd ).pdfRev =
				BDPTUtilities::SolidAngleToArea( camPdfDir, absCosLight, distSq );
		}

		// eyeVerts[0].pdfRev: PDF at lightEnd of scattering toward camera
		if( s >= 2 && lightEnd.pMaterial ) {
			Vector3 wiAtLightMIS = Vector3Ops::mkVector3(
				lightVerts[s - 2].position, lightEnd.position );
			wiAtLightMIS = Vector3Ops::Normalize( wiAtLightMIS );
			const Scalar pdfRevSA = EvalPdfAtVertex( lightEnd, wiAtLightMIS, dirToCam );
			const_cast<BDPTVertex&>( eyeVerts[0] ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfRevSA, Scalar(1.0), distSq );
		}

		// --- Update predecessor pdfRev (lightVerts[s-2]) ---
		// PDF at lightEnd of scattering toward lightVerts[s-2] given incoming = dirToCam
		Scalar savedLightPredPdfRev_t1 = 0;
		const bool hasLightPred_t1 = (s >= 2 && lightEnd.pMaterial);
		if( hasLightPred_t1 )
		{
			const BDPTVertex& lightPred = lightVerts[s - 2];
			savedLightPredPdfRev_t1 = lightPred.pdfRev;

			Vector3 wiAtLightEnd = Vector3Ops::mkVector3(
				lightVerts[s - 2].position, lightEnd.position );
			wiAtLightEnd = Vector3Ops::Normalize( wiAtLightEnd );

			const Scalar pdfPredSA = EvalPdfAtVertex( lightEnd, dirToCam, -wiAtLightEnd );
			const Vector3 dToPred = Vector3Ops::mkVector3( lightPred.position, lightEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			const Scalar absCosAtPred = fabs( Vector3Ops::Dot( lightPred.normal,
				Vector3Ops::Normalize( dToPred ) ) );
			const_cast<BDPTVertex&>( lightPred ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfPredSA, absCosAtPred, distPredSq );
		}

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );

		const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRev;
		const_cast<BDPTVertex&>( eyeVerts[0] ).pdfRev = savedEyePdfRev;
		if( hasLightPred_t1 ) {
			const_cast<BDPTVertex&>( lightVerts[s - 2] ).pdfRev = savedLightPredPdfRev_t1;
		}

		return result;
	}

	//
	// General case: s > 1, t > 1
	// Connect lightVerts[s-1] to eyeVerts[t-1]
	//
	{
		const BDPTVertex& lightEnd = lightVerts[s - 1];
		const BDPTVertex& eyeEnd = eyeVerts[t - 1];

		// Cannot connect at vertices with only delta lobes
		if( !lightEnd.isConnectible || !eyeEnd.isConnectible ) {
			return result;
		}

		// Both must be surface vertices with materials
		if( lightEnd.type != BDPTVertex::SURFACE || !lightEnd.pMaterial ) {
			return result;
		}
		if( eyeEnd.type != BDPTVertex::SURFACE || !eyeEnd.pMaterial ) {
			return result;
		}

		// Connection direction: from eye vertex to light vertex
		Vector3 dConnect = Vector3Ops::mkVector3( lightEnd.position, eyeEnd.position );
		const Scalar dist = Vector3Ops::Magnitude( dConnect );
		if( dist < BDPT_RAY_EPSILON ) {
			return result;
		}
		dConnect = dConnect * (1.0 / dist);

		// Check visibility
		if( !IsVisible( caster, eyeEnd.position, lightEnd.position ) ) {
			return result;
		}

		// Evaluate BSDF at the light endpoint
		// wi at lightEnd = direction from previous light vertex
		Vector3 wiAtLight = Vector3Ops::mkVector3(
			lightVerts[s - 2].position, lightEnd.position );
		wiAtLight = Vector3Ops::Normalize( wiAtLight );

		// wo at lightEnd = direction toward eye vertex (connection)
		const Vector3 woAtLight = -dConnect;

		const RISEPel fLight = EvalBSDFAtVertex( lightEnd, wiAtLight, woAtLight );

		if( ColorMath::MaxValue( fLight ) <= 0 ) {
			return result;
		}

		// Evaluate BSDF at the eye endpoint
		// wo at eyeEnd = direction toward previous eye vertex
		Vector3 woAtEye = Vector3Ops::mkVector3(
			eyeVerts[t - 2].position, eyeEnd.position );
		woAtEye = Vector3Ops::Normalize( woAtEye );

		// wi at eyeEnd = connection direction (from light side)
		const Vector3 wiAtEye = dConnect;

		const RISEPel fEye = EvalBSDFAtVertex( eyeEnd, wiAtEye, woAtEye );

		if( ColorMath::MaxValue( fEye ) <= 0 ) {
			return result;
		}

		// Geometric term
		const Scalar G = BDPTUtilities::GeometricTerm(
			lightEnd.position, lightEnd.normal,
			eyeEnd.position, eyeEnd.normal );

		if( G <= 0 ) {
			return result;
		}

		// Full path contribution
		result.contribution = lightEnd.throughput * fLight *
			RISEPel( G, G, G ) * fEye * eyeEnd.throughput;
		result.needsSplat = false;
		result.valid = true;

		// --- Update pdfRev at connection vertices for correct MIS ---
		// The connection introduces a new edge between lightEnd and eyeEnd.
		// pdfRev at each endpoint must reflect the probability of generating
		// the reverse direction through this connection edge, not the
		// direction from subpath generation.
		const Scalar distSq_conn = dist * dist;

		const Scalar savedLightPdfRev = lightEnd.pdfRev;
		const Scalar savedEyePdfRev = eyeEnd.pdfRev;

		// lightEnd.pdfRev: PDF that eye-side process would generate lightEnd
		// = PDF at eyeEnd of scattering toward lightEnd, converted to area at lightEnd
		{
			const Scalar pdfRevSA = EvalPdfAtVertex( eyeEnd, woAtEye, dConnect );
			const Scalar absCosAtLight = fabs( Vector3Ops::Dot( lightEnd.normal, dConnect ) );
			const_cast<BDPTVertex&>( lightEnd ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfRevSA, absCosAtLight, distSq_conn );
		}

		// eyeEnd.pdfRev: PDF that light-side process would generate eyeEnd
		// = PDF at lightEnd of scattering toward eyeEnd, converted to area at eyeEnd
		{
			const Scalar pdfRevSA = EvalPdfAtVertex( lightEnd, wiAtLight, -dConnect );
			const Scalar absCosAtEye = fabs( Vector3Ops::Dot( eyeEnd.normal, dConnect ) );
			const_cast<BDPTVertex&>( eyeEnd ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfRevSA, absCosAtEye, distSq_conn );
		}

		// --- Update predecessor pdfRev at lightVerts[s-2] ---
		// The connection changed the outgoing direction at lightEnd, so the
		// reverse PDF at the predecessor must reflect scattering at lightEnd
		// from the connection direction (-dConnect) back toward the predecessor.
		Scalar savedLightPredPdfRev = 0;
		const bool hasLightPred = (s >= 2);
		if( hasLightPred )
		{
			const BDPTVertex& lightPred = lightVerts[s - 2];
			savedLightPredPdfRev = lightPred.pdfRev;

			// woAtLight = -dConnect (toward eye side), scatter toward pred = -wiAtLight
			const Scalar pdfPredSA = EvalPdfAtVertex( lightEnd, woAtLight, -wiAtLight );
			const Vector3 dToPred = Vector3Ops::mkVector3( lightPred.position, lightEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			const Scalar absCosAtPred = fabs( Vector3Ops::Dot( lightPred.normal,
				Vector3Ops::Normalize( dToPred ) ) );
			const_cast<BDPTVertex&>( lightPred ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfPredSA, absCosAtPred, distPredSq );
		}

		// --- Update predecessor pdfRev at eyeVerts[t-2] ---
		// The connection changed the outgoing direction at eyeEnd, so the
		// reverse PDF at the predecessor must reflect scattering at eyeEnd
		// from the connection direction (dConnect) back toward the predecessor.
		Scalar savedEyePredPdfRev = 0;
		const bool hasEyePred = (t >= 2);
		if( hasEyePred )
		{
			const BDPTVertex& eyePred = eyeVerts[t - 2];
			savedEyePredPdfRev = eyePred.pdfRev;

			// wiAtEye = dConnect (from light side), scatter toward pred = -woAtEye
			const Scalar pdfPredSA = EvalPdfAtVertex( eyeEnd, wiAtEye, -woAtEye );
			const Vector3 dToPred = Vector3Ops::mkVector3( eyePred.position, eyeEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			// Camera vertex has no meaningful surface normal; use 1.0
			const Scalar absCosAtPred = (eyePred.type == BDPTVertex::CAMERA) ?
				Scalar(1.0) :
				fabs( Vector3Ops::Dot( eyePred.normal, Vector3Ops::Normalize( dToPred ) ) );
			const_cast<BDPTVertex&>( eyePred ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfPredSA, absCosAtPred, distPredSq );
		}

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );

		// Restore original values
		const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRev;
		const_cast<BDPTVertex&>( eyeEnd ).pdfRev = savedEyePdfRev;
		if( hasLightPred ) {
			const_cast<BDPTVertex&>( lightVerts[s - 2] ).pdfRev = savedLightPredPdfRev;
		}
		if( hasEyePred ) {
			const_cast<BDPTVertex&>( eyeVerts[t - 2] ).pdfRev = savedEyePredPdfRev;
		}

		return result;
	}
}

//////////////////////////////////////////////////////////////////////
// EvaluateAllStrategies
//////////////////////////////////////////////////////////////////////

std::vector<BDPTIntegrator::ConnectionResult> BDPTIntegrator::EvaluateAllStrategies(
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<BDPTVertex>& eyeVerts,
	const IScene& scene,
	const IRayCaster& caster,
	const ICamera& camera
	) const
{
	const unsigned int nLight = static_cast<unsigned int>( lightVerts.size() );
	const unsigned int nEye = static_cast<unsigned int>( eyeVerts.size() );

	std::vector<ConnectionResult> results;
	results.reserve( (nLight + 1) * (nEye + 1) );

	// Iterate over all valid (s,t) combinations where s + t >= 2
	for( unsigned int t = 1; t <= nEye; t++ )
	{
		for( unsigned int s = 0; s <= nLight; s++ )
		{
			if( s + t < 2 ) {
				continue;
			}

			ConnectionResult cr = ConnectAndEvaluate(
				lightVerts, eyeVerts, s, t, scene, caster, camera );

			if( cr.valid ) {
				results.push_back( cr );
			}
		}
	}

	return results;
}

//////////////////////////////////////////////////////////////////////
// MISWeight - power heuristic (exponent = 2)
//
// Uses the technique from Veach's thesis: compute the weight by
// walking along the path and computing ratios of PDFs for adjacent
// strategies.
//
// For a path of length k = s + t - 1, the power heuristic weight
// for strategy (s,t) is:
//
//   w(s,t) = p_s^2 / sum_{i} p_i^2 = 1 / sum_{i} (p_i / p_s)^2
//
// where p_i is the probability of generating this path using
// strategy i.  The ratios p_i/p_{s} can be computed incrementally
// using the stored forward and reverse PDFs at each vertex.
//////////////////////////////////////////////////////////////////////
//#define MISWEIGHT_BALANCE_HEURISTIC 1
Scalar BDPTIntegrator::MISWeight(
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<BDPTVertex>& eyeVerts,
	unsigned int s,
	unsigned int t
	) const
{
	if( s + t < 2 ) {
		return 0;
	}

	// If only one strategy is possible, weight = 1
	if( s + t == 2 ) {
		// For a path of length 1 (2 vertices), there might still be
		// multiple strategies, but handle the simple case first
	}

	// Build the full path from light vertices [0..s-1] and eye vertices [t-1..0]
	// The path vertices in order are:
	//   lightVerts[0], ..., lightVerts[s-1], eyeVerts[t-1], ..., eyeVerts[0]
	//
	// Strategy (s,t) splits the path such that the light subpath has s vertices
	// and the eye subpath has t vertices.  The connection is between
	// lightVerts[s-1] and eyeVerts[t-1].

	// We compute the MIS weight using the power heuristic with exponent 2.
	// The power heuristic concentrates weight on the strategy with the
	// highest sampling probability, which provides provably lower variance
	// than the balance heuristic (exponent 1) for scenes with caustics and
	// other difficult light transport paths (Veach thesis, Section 9.2.4).
	//
	// w(s,t) = p_s^2 / sum_i p_i^2 = 1 / sum_i (p_i / p_s)^2
	//
	// The ratios p_i/p_s are computed incrementally by walking along the
	// path and accumulating forward/reverse PDF ratios at each vertex.

	Scalar sumWeights = 1.0;	// The weight for strategy (s,t) itself contributes 1^2 = 1

	// Temporarily clear isDelta on the two connection vertices (PBRT convention).
	// The connection always evaluates the full BSDF (non-delta), so these
	// vertices should be treated as non-delta for MIS weight computation
	// regardless of which lobe was sampled during subpath generation.
	bool savedLightEndDelta = false, savedEyeEndDelta = false;
	if( s > 0 ) {
		savedLightEndDelta = lightVerts[s-1].isDelta;
		if( lightVerts[s-1].isConnectible ) {
			const_cast<BDPTVertex&>( lightVerts[s-1] ).isDelta = false;
		}
	}
	if( t > 0 ) {
		savedEyeEndDelta = eyeVerts[t-1].isDelta;
		if( eyeVerts[t-1].isConnectible ) {
			const_cast<BDPTVertex&>( eyeVerts[t-1] ).isDelta = false;
		}
	}

	//
	// Walk along the light subpath (decreasing s, increasing t)
	// This computes ratios for strategies (s-1, t+1), (s-2, t+2), etc.
	//
	{
		Scalar ri = 1.0;

		for( int i = static_cast<int>(s) - 1; i >= 0; i-- )
		{
			// Vertex at position i in the light subpath
			const BDPTVertex& vi = (static_cast<unsigned int>(i) < lightVerts.size()) ?
				lightVerts[i] : eyeVerts[0];

			// Compute the ratio: pdfRev / pdfFwd at this vertex.
			// Use remap0 (Veach/PBRT convention): map zero PDFs to 1
			// so that the ratio chain propagates through delta vertices.
			// Delta vertices have pdfRev=0 (since SPF::Pdf returns 0
			// for Dirac distributions), but they are always skipped in
			// the sum below.  Without remap0, the zero kills the chain
			// and prevents subsequent non-delta strategies from being
			// properly weighted, causing fireflies on caustic paths.
			const Scalar pdfR = (vi.pdfRev != 0) ? vi.pdfRev : Scalar(1);
			const Scalar pdfF = (vi.pdfFwd != 0) ? vi.pdfFwd : Scalar(1);
			ri *= pdfR / pdfF;

			// Skip non-connectible vertices — they can only be generated by
			// exactly one strategy, so their contribution to the sum is 0.
			// For non-connection vertices isDelta reflects the sampled lobe;
			// for connection vertices isDelta was cleared above if connectible.
			// Both vertices at the proposed connection must be non-delta
			// (PBRT convention: !v[i].delta && !v[i-1].delta).
			if( vi.isDelta ) {
				continue;
			}
			if( i > 0 && lightVerts[i-1].isDelta ) {
				continue;
			}

			#if MISWEIGHT_BALANCE_HEURISTIC
			// Strategy (i, s+t-i) contributes ri to the sum (balance heuristic)
			sumWeights += ri;
			#else
			// Strategy (i, s+t-i) contributes ri^2 to the sum (power heuristic)
			sumWeights += ri * ri;
			#endif

		}
	}

	//
	// Walk along the eye subpath (increasing s, decreasing t)
	// This computes ratios for strategies (s+1, t-1), (s+2, t-2), etc.
	//
	{
		Scalar ri = 1.0;

		for( int j = static_cast<int>(t) - 1; j > 0; j-- )
		{
			// Vertex at position j in the eye subpath
			const BDPTVertex& vj = (static_cast<unsigned int>(j) < eyeVerts.size()) ?
				eyeVerts[j] : lightVerts[0];

			// Compute the ratio with remap0 (see light-side walk above)
			const Scalar pdfR = (vj.pdfRev != 0) ? vj.pdfRev : Scalar(1);
			const Scalar pdfF = (vj.pdfFwd != 0) ? vj.pdfFwd : Scalar(1);
			ri *= pdfR / pdfF;

			// Skip non-connectible vertices.
			// Both vertices at the proposed connection must be non-delta.
			if( vj.isDelta ) {
				continue;
			}
			if( j > 0 && eyeVerts[j-1].isDelta ) {
				continue;
			}

			#if MISWEIGHT_BALANCE_HEURISTIC
			// Strategy (i, s+t-i) contributes ri to the sum (balance heuristic)
			sumWeights += ri;
			#else
			// Strategy (i, s+t-i) contributes ri^2 to the sum (power heuristic)
			sumWeights += ri * ri;
			#endif
		}
	}

	// Restore isDelta on connection vertices
	if( s > 0 ) {
		const_cast<BDPTVertex&>( lightVerts[s-1] ).isDelta = savedLightEndDelta;
	}
	if( t > 0 ) {
		const_cast<BDPTVertex&>( eyeVerts[t-1] ).isDelta = savedEyeEndDelta;
	}

	if( sumWeights <= 0 ) {
		return 0;
	}

	return 1.0 / sumWeights;
}

//////////////////////////////////////////////////////////////////////
//
// SPECTRAL (NM) VARIANTS
//
// These methods mirror the RGB versions above but work with scalar
// throughput at a single wavelength.  They use IBSDF::valueNM(),
// ISPF::ScatterNM(), ISPF::PdfNM(), and IEmitter::emittedRadianceNM()
// to evaluate the path contribution for a specific wavelength.
//
//////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////
// NM Helper: evaluate BSDF at a vertex for a single wavelength
//////////////////////////////////////////////////////////////////////

Scalar BDPTIntegrator::EvalBSDFAtVertexNM(
	const BDPTVertex& vertex,
	const Vector3& wi,
	const Vector3& wo,
	const Scalar nm
	) const
{
	if( !vertex.pMaterial ) {
		return 0;
	}

	// BSSRDF entry vertex: Sw(direction) = Ft(cos) / (c * PI)
	if( vertex.isBSSRDFEntry ) {
		ISubSurfaceDiffusionProfile* pProfile = vertex.pMaterial->GetDiffusionProfile();
		if( pProfile ) {
			// No fabs: back-face connections (cosTheta < 0) return zero.
			const Scalar cosTheta = Vector3Ops::Dot( wi, vertex.normal );
			if( cosTheta <= NEARZERO ) {
				return 0;
			}

			RayIntersectionGeometric rig(
				Ray( vertex.position, -wi ), nullRasterizerState );
			rig.bHit = true;
			rig.ptIntersection = vertex.position;
			rig.vNormal = vertex.normal;
			rig.onb = vertex.onb;

			const Scalar FtEntry = pProfile->FresnelTransmission( cosTheta, rig );
			const Scalar eta = pProfile->GetIOR( rig );
			const Scalar F0 = ((eta - 1.0) / (eta + 1.0)) * ((eta - 1.0) / (eta + 1.0));
			const Scalar c = (41.0 - 20.0 * F0) / 42.0;
			return (c > 1e-20) ? FtEntry / (c * PI) : 0;
		}
		return 0;
	}

	const IBSDF* pBSDF = vertex.pMaterial->GetBSDF();
	if( !pBSDF ) {
		return 0;
	}

	// Same convention as EvalBSDFAtVertex: wi and wo are both
	// outgoing (away from surface).  Negate wo to get ri.ray.Dir()
	// toward surface.
	Ray evalRay( vertex.position, -wo );
	RayIntersectionGeometric ri( evalRay, nullRasterizerState );
	ri.bHit = true;
	ri.ptIntersection = vertex.position;
	ri.vNormal = vertex.normal;
	ri.onb = vertex.onb;

	return pBSDF->valueNM( wi, ri, nm );
}

//////////////////////////////////////////////////////////////////////
// NM Helper: evaluate SPF PDF at a vertex for a single wavelength
//////////////////////////////////////////////////////////////////////

Scalar BDPTIntegrator::EvalPdfAtVertexNM(
	const BDPTVertex& vertex,
	const Vector3& wi,
	const Vector3& wo,
	const Scalar nm
	) const
{
	if( !vertex.pMaterial ) {
		return 0;
	}

	// BSSRDF entry vertex: cosine hemisphere PDF
	if( vertex.isBSSRDFEntry ) {
		const Scalar cosTheta = fabs( Vector3Ops::Dot( wo, vertex.normal ) );
		return cosTheta * INV_PI;
	}

	const ISPF* pSPF = vertex.pMaterial->GetSPF();
	if( !pSPF ) {
		return 0;
	}

	// Negate wi to get toward-surface direction for ri.ray.Dir()
	Ray evalRay( vertex.position, -wi );
	RayIntersectionGeometric ri( evalRay, nullRasterizerState );
	ri.bHit = true;
	ri.ptIntersection = vertex.position;
	ri.vNormal = vertex.normal;
	ri.onb = vertex.onb;

	IORStack stack( 1.0 );
	return pSPF->PdfNM( ri, wo, nm, BuildVertexIORStack( vertex, stack ) );
}

//////////////////////////////////////////////////////////////////////
// NM Helper: evaluate emitter radiance at a vertex
//////////////////////////////////////////////////////////////////////

Scalar BDPTIntegrator::EvalEmitterRadianceNM(
	const BDPTVertex& vertex,
	const Vector3& outDir,
	const Scalar nm
	) const
{
	if( !vertex.pMaterial ) {
		return 0;
	}

	const IEmitter* pEmitter = vertex.pMaterial->GetEmitter();
	if( !pEmitter ) {
		return 0;
	}

	RayIntersectionGeometric rig( Ray( vertex.position, outDir ), nullRasterizerState );
	rig.bHit = true;
	rig.ptIntersection = vertex.position;
	rig.vNormal = vertex.normal;
	rig.onb = vertex.onb;

	return pEmitter->emittedRadianceNM( rig, outDir, vertex.normal, nm );
}

//////////////////////////////////////////////////////////////////////
// GenerateLightSubpathNM
//////////////////////////////////////////////////////////////////////

unsigned int BDPTIntegrator::GenerateLightSubpathNM(
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	std::vector<BDPTVertex>& vertices,
	const Scalar nm
	) const
{
	vertices.clear();

	if( !pLightSampler ) {
		return 0;
	}

	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
	const LuminaryManager::LuminariesList& luminaries = pLumManager ?
		const_cast<LuminaryManager*>(pLumManager)->getLuminaries() : emptyList;

	const bool useIORStack = BDPTUsesIORStack( caster );
	IORStack iorStack( 1.0 );

	// Phase 0: light source sampling
	sampler.StartStream( 0 );

	LightSample ls;
	if( !pLightSampler->SampleLight( scene, luminaries, sampler, ls ) ) {
		return 0;
	}

	vertices.reserve( maxLightDepth + 1 );

	// Convert Le from RISEPel to scalar at wavelength nm
	// For mesh luminaries, re-evaluate using emittedRadianceNM
	// For non-mesh lights, approximate from the RGB value
	Scalar LeNM = 0;
	if( ls.pLuminary && ls.pLuminary->GetMaterial() ) {
		const IEmitter* pEmitter = ls.pLuminary->GetMaterial()->GetEmitter();
		if( pEmitter ) {
			RayIntersectionGeometric rig( Ray( ls.position, ls.direction ), nullRasterizerState );
			rig.bHit = true;
			rig.ptIntersection = ls.position;
			rig.vNormal = ls.normal;
			OrthonormalBasis3D onb;
			onb.CreateFromW( ls.normal );
			rig.onb = onb;
			LeNM = pEmitter->emittedRadianceNM( rig, ls.direction, ls.normal, nm );
		}
	} else if( ls.pLight ) {
		// Non-mesh light: approximate spectral radiance from RGB
		// Use luminance-like weighting: 0.2126*R + 0.7152*G + 0.0722*B
		LeNM = 0.2126 * ls.Le[0] + 0.7152 * ls.Le[1] + 0.0722 * ls.Le[2];
	}

	//
	// Vertex 0: the light source itself
	//
	{
		BDPTVertex v;
		v.type = BDPTVertex::LIGHT;
		v.position = ls.position;
		v.normal = ls.normal;
		v.onb.CreateFromW( ls.normal );
		v.pMaterial = 0;
		v.pObject = 0;
		v.pLight = ls.pLight;
		v.pLuminary = ls.pLuminary;
		v.isDelta = ls.isDelta;
		v.isConnectible = !ls.isDelta;

		v.pdfFwd = ls.pdfSelect * ls.pdfPosition;

		if( v.pdfFwd > 0 ) {
			v.throughputNM = LeNM / v.pdfFwd;
		} else {
			v.throughputNM = 0;
		}

		v.pdfRev = 0;
		vertices.push_back( v );
	}

	if( LeNM <= 0 || ls.pdfDirection <= 0 ) {
		return static_cast<unsigned int>( vertices.size() );
	}

	// Trace the emission ray
	Ray currentRay( ls.position, ls.direction );
	currentRay.Advance( BDPT_RAY_EPSILON );

	const Scalar cosAtLight = fabs( Vector3Ops::Dot( ls.direction, ls.normal ) );
	Scalar betaNM = LeNM * cosAtLight;
	const Scalar pdfEmit = ls.pdfSelect * ls.pdfPosition * ls.pdfDirection;

	if( pdfEmit > 0 ) {
		betaNM = betaNM / pdfEmit;
	} else {
		return static_cast<unsigned int>( vertices.size() );
	}

	Scalar pdfFwdPrev = ls.pdfDirection;

	for( unsigned int depth = 0; depth < maxLightDepth; depth++ )
	{
		// Phases 1..15 = light bounces 0..14
		sampler.StartStream( 1 + depth );

		RayIntersection ri( currentRay, nullRasterizerState );
		scene.GetObjects()->IntersectRay( ri, true, true, false );

		if( !ri.geometric.bHit ) {
			break;
		}

		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = ri.geometric.ptIntersection;
		v.normal = ri.geometric.vNormal;
		v.onb = ri.geometric.onb;
		v.pMaterial = ri.pMaterial;
		v.pObject = ri.pObject;
		v.pLight = 0;
		v.pLuminary = 0;
		if( useIORStack && ri.pObject ) {
			iorStack.SetCurrentObject( ri.pObject );
			v.mediumIOR = iorStack.top();
			v.insideObject = iorStack.containsCurrent();
		}

		const Scalar distSq = ri.geometric.range * ri.geometric.range;
		const Scalar absCosIn = fabs( Vector3Ops::Dot(
			ri.geometric.vNormal, -currentRay.Dir() ) );

		v.pdfFwd = BDPTUtilities::SolidAngleToArea( pdfFwdPrev, absCosIn, distSq );
		v.throughputNM = betaNM;
		v.pdfRev = 0;
		v.isDelta = false;

		if( !ri.pMaterial ) {
			vertices.push_back( v );
			break;
		}

		const ISPF* pSPF = ri.pMaterial->GetSPF();
		if( !pSPF ) {
			vertices.push_back( v );
			break;
		}

		vertices.push_back( v );

		// Sample the SPF at this wavelength
		ScatteredRayContainer scattered;
		pSPF->ScatterNM( ri.geometric, sampler, nm, scattered, useIORStack ? &iorStack : 0 );

		if( scattered.Count() == 0 ) {
			break;
		}

		const ScatteredRay* pScat = scattered.RandomlySelect( sampler.Get1D(), true );
		if( !pScat ) {
			break;
		}

		// Compute lobe selection probability using spectral krayNM weights
		Scalar selectProb = 1.0;
		if( scattered.Count() > 1 ) {
			Scalar totalKray = 0;
			for( unsigned int i = 0; i < scattered.Count(); i++ ) {
				totalKray += fabs( scattered[i].krayNM );
			}
			const Scalar selectedKray = fabs( pScat->krayNM );
			if( totalKray > NEARZERO && selectedKray > NEARZERO ) {
				selectProb = selectedKray / totalKray;
			}
		}

		// Determine connectibility: true if any scattered lobe is non-delta
		{
			bool hasNonDelta = false;
			for( unsigned int i = 0; i < scattered.Count(); i++ ) {
				if( !scattered[i].isDelta ) { hasNonDelta = true; break; }
			}
			if( !ri.pMaterial->GetBSDF() ) {
				hasNonDelta = false;
			}
			vertices.back().isConnectible = hasNonDelta;
		}

		vertices.back().isDelta = pScat->isDelta;

		// --- BSSRDF sampling (NM light subpath) ---
		Scalar bssrdfReflectCompensation = 1.0;
		if( ri.pMaterial && ri.pMaterial->GetDiffusionProfile() )
		{
			const Scalar cosIn = Vector3Ops::Dot(
				ri.geometric.vNormal, -currentRay.Dir() );
			if( cosIn > NEARZERO )
			{
				ISubSurfaceDiffusionProfile* pProfile = ri.pMaterial->GetDiffusionProfile();
			const Scalar Ft = pProfile->FresnelTransmission( cosIn, ri.geometric );
			const Scalar R = 1.0 - Ft;

			if( Ft > NEARZERO && sampler.Get1D() < Ft )
			{
				BSSRDFSampleResult bssrdf = SampleBSSRDFEntryPoint(
					ri.geometric, ri.pObject, ri.pMaterial, sampler, nm );

				if( bssrdf.valid )
				{
					vertices.back().isDelta = true;
					betaNM = betaNM * bssrdf.weightNM / Ft;

					BDPTVertex entryV;
					entryV.type = BDPTVertex::SURFACE;
					entryV.position = bssrdf.entryPoint;
					entryV.normal = bssrdf.entryNormal;
					entryV.onb = bssrdf.entryONB;
					entryV.pMaterial = ri.pMaterial;
					entryV.pObject = ri.pObject;
					entryV.isDelta = false;
					entryV.isConnectible = true;
					entryV.isBSSRDFEntry = true;
					entryV.throughputNM = betaNM;
					entryV.pdfFwd = bssrdf.pdfSurface;
					entryV.pdfRev = 0;
					vertices.push_back( entryV );

					pdfFwdPrev = bssrdf.cosinePdf;
					currentRay = bssrdf.scatteredRay;
					continue;
				}
				break;
			}
			if( R > NEARZERO ) {
				bssrdfReflectCompensation = 1.0 / R;
			}
			}
		}
		// --- End BSSRDF sampling ---

		// Throughput update using valueNM
		const Scalar fNM = EvalBSDFAtVertexNM(
			vertices.back(),
			-currentRay.Dir(),
			pScat->ray.Dir(),
			nm );

		const Scalar cosTheta = fabs( Vector3Ops::Dot(
			pScat->ray.Dir(), ri.geometric.vNormal ) );

		Scalar pdf = pScat->pdf;
		if( pdf <= 0 ) {
			break;
		}

		if( pScat->isDelta ) {
			betaNM = betaNM * pScat->krayNM * bssrdfReflectCompensation / selectProb;
		} else {
			if( fNM <= 0 ) {
				break;
			}
			betaNM = betaNM * fNM * bssrdfReflectCompensation * cosTheta / (selectProb * pdf);
		}

		// Russian Roulette
		if( depth > 2 ) {
			const Scalar rrProb = r_min( Scalar(1.0), fabs(betaNM) /
				r_max( fabs(vertices.back().throughputNM), Scalar(BDPT_RR_THRESHOLD) ) );
			if( sampler.Get1D() >= rrProb ) {
				break;
			}
			if( rrProb > 0 ) {
				betaNM = betaNM / rrProb;
			}
		}

		pdfFwdPrev = selectProb * pdf;

		// In Veach's formulation, delta vertices should be "transparent" in the MIS walk
		if( pScat->isDelta ) {
			pdfFwdPrev = 0;
		}

		// Update previous vertex's pdfRev
		if( vertices.size() >= 2 ) {
			const BDPTVertex& curr = vertices.back();
			BDPTVertex& prev = vertices[ vertices.size() - 2 ];

			// Reverse PDF: returns 0 for delta, handled by remap0 in MISWeight.
			const Scalar revPdfSA = EvalPdfAtVertexNM(
				curr, pScat->ray.Dir(), -currentRay.Dir(), nm );

			const Scalar absCosAtPrev = fabs( Vector3Ops::Dot( prev.normal, currentRay.Dir() ) );
			prev.pdfRev = BDPTUtilities::SolidAngleToArea( revPdfSA, absCosAtPrev, distSq );
			if( pScat->isDelta ) { prev.pdfRev = 0; }
		}

		currentRay = pScat->ray;
		currentRay.Advance( BDPT_RAY_EPSILON );
		if( useIORStack && pScat->ior_stack ) {
			iorStack = *pScat->ior_stack;
		}
	}

	return static_cast<unsigned int>( vertices.size() );
}

//////////////////////////////////////////////////////////////////////
// GenerateEyeSubpathNM
//////////////////////////////////////////////////////////////////////

unsigned int BDPTIntegrator::GenerateEyeSubpathNM(
	const RuntimeContext& rc,
	const Ray& cameraRay,
	const Point2& screenPos,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	std::vector<BDPTVertex>& vertices,
	const Scalar nm
	) const
{
	vertices.clear();
	vertices.reserve( maxEyeDepth + 1 );

	// Vertex 0: the camera
	{
		const ICamera* pCamera = scene.GetCamera();

		BDPTVertex v;
		v.type = BDPTVertex::CAMERA;

		if( pCamera ) {
			v.position = pCamera->GetLocation();
		}

		v.normal = cameraRay.Dir();
		v.onb.CreateFromW( cameraRay.Dir() );
		v.pMaterial = 0;
		v.pObject = 0;
		v.pLight = 0;
		v.pLuminary = 0;
		v.screenPos = screenPos;
		v.isDelta = false;
		v.pdfFwd = 1.0;
		v.pdfRev = 0;
		v.throughputNM = 1.0;

		vertices.push_back( v );
	}

	Ray currentRay = cameraRay;
	Scalar betaNM = 1.0;

	const ICamera* pCamera = scene.GetCamera();
	Scalar pdfCamDir = 1.0;
	if( pCamera ) {
		pdfCamDir = BDPTCameraUtilities::PdfDirection( *pCamera, cameraRay );
	}

	Scalar pdfFwdPrev = pdfCamDir;
	const bool useIORStack = BDPTUsesIORStack( caster );
	IORStack iorStack( 1.0 );

	for( unsigned int depth = 0; depth < maxEyeDepth; depth++ )
	{
		// Phases 16..30 = eye bounces 0..14
		sampler.StartStream( 16 + depth );

		RayIntersection ri( currentRay, nullRasterizerState );
		scene.GetObjects()->IntersectRay( ri, true, true, false );

		if( !ri.geometric.bHit ) {
			break;
		}

		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = ri.geometric.ptIntersection;
		v.normal = ri.geometric.vNormal;
		v.onb = ri.geometric.onb;
		v.pMaterial = ri.pMaterial;
		v.pObject = ri.pObject;
		v.pLight = 0;
		v.pLuminary = 0;
		if( useIORStack && ri.pObject ) {
			iorStack.SetCurrentObject( ri.pObject );
			v.mediumIOR = iorStack.top();
			v.insideObject = iorStack.containsCurrent();
		}

		const Scalar distSq = ri.geometric.range * ri.geometric.range;
		const Scalar absCosIn = fabs( Vector3Ops::Dot(
			ri.geometric.vNormal, -currentRay.Dir() ) );

		v.pdfFwd = BDPTUtilities::SolidAngleToArea( pdfFwdPrev, absCosIn, distSq );
		v.throughputNM = betaNM;
		v.pdfRev = 0;
		v.isDelta = false;

		if( !ri.pMaterial ) {
			vertices.push_back( v );
			break;
		}

		const ISPF* pSPF = ri.pMaterial->GetSPF();
		if( !pSPF ) {
			vertices.push_back( v );
			break;
		}

		vertices.push_back( v );

		// Sample the SPF at this wavelength
		ScatteredRayContainer scattered;
		pSPF->ScatterNM( ri.geometric, sampler, nm, scattered, useIORStack ? &iorStack : 0 );

		if( scattered.Count() == 0 ) {
			break;
		}

		const ScatteredRay* pScat = scattered.RandomlySelect( sampler.Get1D(), true );
		if( !pScat ) {
			break;
		}

		// Compute lobe selection probability using spectral krayNM weights
		Scalar selectProb = 1.0;
		if( scattered.Count() > 1 ) {
			Scalar totalKray = 0;
			for( unsigned int i = 0; i < scattered.Count(); i++ ) {
				totalKray += fabs( scattered[i].krayNM );
			}
			const Scalar selectedKray = fabs( pScat->krayNM );
			if( totalKray > NEARZERO && selectedKray > NEARZERO ) {
				selectProb = selectedKray / totalKray;
			}
		}

		// Determine connectibility: true if any scattered lobe is non-delta
		{
			bool hasNonDelta = false;
			for( unsigned int i = 0; i < scattered.Count(); i++ ) {
				if( !scattered[i].isDelta ) { hasNonDelta = true; break; }
			}
			if( !ri.pMaterial->GetBSDF() ) {
				hasNonDelta = false;
			}
			vertices.back().isConnectible = hasNonDelta;
		}

		vertices.back().isDelta = pScat->isDelta;

		// --- BSSRDF sampling (NM eye subpath) ---
		Scalar bssrdfReflectCompensation = 1.0;
		if( ri.pMaterial && ri.pMaterial->GetDiffusionProfile() )
		{
			const Scalar cosIn = Vector3Ops::Dot(
				ri.geometric.vNormal, -currentRay.Dir() );
			if( cosIn > NEARZERO )
			{
				ISubSurfaceDiffusionProfile* pProfile = ri.pMaterial->GetDiffusionProfile();
			const Scalar Ft = pProfile->FresnelTransmission( cosIn, ri.geometric );
			const Scalar R = 1.0 - Ft;

			if( Ft > NEARZERO && sampler.Get1D() < Ft )
			{
				BSSRDFSampleResult bssrdf = SampleBSSRDFEntryPoint(
					ri.geometric, ri.pObject, ri.pMaterial, sampler, nm );

				if( bssrdf.valid )
				{
					vertices.back().isDelta = true;
					betaNM = betaNM * bssrdf.weightNM / Ft;

					BDPTVertex entryV;
					entryV.type = BDPTVertex::SURFACE;
					entryV.position = bssrdf.entryPoint;
					entryV.normal = bssrdf.entryNormal;
					entryV.onb = bssrdf.entryONB;
					entryV.pMaterial = ri.pMaterial;
					entryV.pObject = ri.pObject;
					entryV.isDelta = false;
					entryV.isConnectible = true;
					entryV.isBSSRDFEntry = true;
					entryV.throughputNM = betaNM;
					entryV.pdfFwd = bssrdf.pdfSurface;
					entryV.pdfRev = 0;
					vertices.push_back( entryV );

					pdfFwdPrev = bssrdf.cosinePdf;
					currentRay = bssrdf.scatteredRay;
					continue;
				}
				break;
			}
			if( R > NEARZERO ) {
				bssrdfReflectCompensation = 1.0 / R;
			}
			}
		}
		// --- End BSSRDF sampling ---

		// Throughput update using valueNM
		const Scalar fNM = EvalBSDFAtVertexNM(
			vertices.back(),
			pScat->ray.Dir(),
			-currentRay.Dir(),
			nm );

		const Scalar cosTheta = fabs( Vector3Ops::Dot(
			pScat->ray.Dir(), ri.geometric.vNormal ) );

		Scalar pdf = pScat->pdf;
		if( pdf <= 0 ) {
			break;
		}

		if( pScat->isDelta ) {
			betaNM = betaNM * pScat->krayNM * bssrdfReflectCompensation / selectProb;
		} else {
			if( fNM <= 0 ) {
				break;
			}
			betaNM = betaNM * fNM * bssrdfReflectCompensation * cosTheta / (selectProb * pdf);
		}

		// Russian Roulette
		if( depth > 2 ) {
			const Scalar rrProb = r_min( Scalar(1.0), fabs(betaNM) );
			if( sampler.Get1D() >= rrProb ) {
				break;
			}
			if( rrProb > 0 ) {
				betaNM = betaNM / rrProb;
			}
		}

		pdfFwdPrev = selectProb * pdf;

		// In Veach's formulation, delta vertices should be "transparent" in the MIS walk
		if( pScat->isDelta ) {
			pdfFwdPrev = 0;
		}

		if( vertices.size() >= 2 ) {
			const BDPTVertex& curr = vertices.back();
			BDPTVertex& prev = vertices[ vertices.size() - 2 ];

			// Reverse PDF: returns 0 for delta, handled by remap0 in MISWeight.
			const Scalar revPdfSA = EvalPdfAtVertexNM(
				curr, pScat->ray.Dir(), -currentRay.Dir(), nm );

			const Scalar absCosAtPrev = (prev.type == BDPTVertex::CAMERA) ?
				Scalar(1.0) :
				fabs( Vector3Ops::Dot( prev.normal, currentRay.Dir() ) );

			prev.pdfRev = BDPTUtilities::SolidAngleToArea( revPdfSA, absCosAtPrev, distSq );
			if( pScat->isDelta ) { prev.pdfRev = 0; }
		}

		currentRay = pScat->ray;
		currentRay.Advance( BDPT_RAY_EPSILON );
		if( useIORStack && pScat->ior_stack ) {
			iorStack = *pScat->ior_stack;
		}
	}

	return static_cast<unsigned int>( vertices.size() );
}

//////////////////////////////////////////////////////////////////////
// ConnectAndEvaluateNM
//////////////////////////////////////////////////////////////////////

BDPTIntegrator::ConnectionResultNM BDPTIntegrator::ConnectAndEvaluateNM(
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<BDPTVertex>& eyeVerts,
	unsigned int s,
	unsigned int t,
	const IScene& scene,
	const IRayCaster& caster,
	const ICamera& camera,
	const Scalar nm
	) const
{
	ConnectionResultNM result;

	if( s > lightVerts.size() || t > eyeVerts.size() ) {
		return result;
	}
	if( s + t < 2 ) {
		return result;
	}

	//
	// Case: s == 0 -- eye path hits emitter directly
	//
	if( s == 0 )
	{
		const BDPTVertex& eyeEnd = eyeVerts[t - 1];

		if( eyeEnd.type != BDPTVertex::SURFACE || !eyeEnd.pMaterial ) {
			return result;
		}

		Vector3 woFromEmitter;
		if( t >= 2 ) {
			woFromEmitter = Vector3Ops::mkVector3( eyeVerts[t - 2].position, eyeEnd.position );
			woFromEmitter = Vector3Ops::Normalize( woFromEmitter );
		} else {
			return result;
		}

		const Scalar LeNM = EvalEmitterRadianceNM( eyeEnd, woFromEmitter, nm );
		if( LeNM <= 0 ) {
			return result;
		}

		result.contribution = eyeEnd.throughputNM * LeNM;
		result.needsSplat = false;
		result.valid = true;

		// --- Update pdfRev at emitter vertex for correct MIS ---
		const Scalar savedEyeEndPdfRev = eyeEnd.pdfRev;

		if( pLightSampler && eyeEnd.pObject )
		{
			const ILuminaryManager* pLumMgr = caster.GetLuminaries();
			const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
			LuminaryManager::LuminariesList emptyList;
			const LuminaryManager::LuminariesList& luminaries = pLumManager ?
				const_cast<LuminaryManager*>( pLumManager )->getLuminaries() : emptyList;

			const Scalar pdfSelect = pLightSampler->PdfSelectLuminary(
				scene, luminaries, *eyeEnd.pObject );
			const Scalar area = eyeEnd.pObject->GetArea();
			const Scalar pdfPosition = (area > 0) ? (Scalar(1.0) / area) : 0;
			const_cast<BDPTVertex&>( eyeEnd ).pdfRev = pdfSelect * pdfPosition;
		}

		// --- Update predecessor pdfRev (eyeVerts[t-2]) ---
		Scalar savedEyePredPdfRevNM_s0 = 0;
		const bool hasEyePredNM_s0 = (t >= 2);
		if( hasEyePredNM_s0 )
		{
			const BDPTVertex& eyePred = eyeVerts[t - 2];
			savedEyePredPdfRevNM_s0 = eyePred.pdfRev;

			Scalar emPdfDir = 0;
			if( eyeEnd.pMaterial ) {
				const IEmitter* pEm = eyeEnd.pMaterial->GetEmitter();
				if( pEm ) {
					const Scalar cosAtEmitter = fabs( Vector3Ops::Dot( eyeEnd.normal, woFromEmitter ) );
					emPdfDir = (cosAtEmitter > 0) ? (cosAtEmitter * INV_PI) : 0;
				}
			}

			const Vector3 dToPred = Vector3Ops::mkVector3( eyePred.position, eyeEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			// Camera vertex has no meaningful surface normal; use 1.0
			const Scalar absCosAtPred = (eyePred.type == BDPTVertex::CAMERA) ?
				Scalar(1.0) :
				fabs( Vector3Ops::Dot( eyePred.normal,
					Vector3Ops::Normalize( dToPred ) ) );
			const_cast<BDPTVertex&>( eyePred ).pdfRev =
				BDPTUtilities::SolidAngleToArea( emPdfDir, absCosAtPred, distPredSq );
		}

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );
		const_cast<BDPTVertex&>( eyeEnd ).pdfRev = savedEyeEndPdfRev;
		if( hasEyePredNM_s0 ) {
			const_cast<BDPTVertex&>( eyeVerts[t - 2] ).pdfRev = savedEyePredPdfRevNM_s0;
		}
		return result;
	}

	//
	// Legacy t == 0 path-to-camera case.
	// The active path enumeration in this file includes the camera as
	// eye vertex 0, so the camera-connection strategy is t == 1.
	// This branch is kept only for compatibility with any future caller
	// that enumerates paths without an explicit camera vertex.
	//
	if( t == 0 )
	{
		const BDPTVertex& lightEnd = lightVerts[s - 1];
		if( !lightEnd.isConnectible ) {
			return result;
		}

		Point2 rasterPos;
		if( !BDPTCameraUtilities::Rasterize( camera, lightEnd.position, rasterPos ) ) {
			return result;
		}

		const Point3 camPos = camera.GetLocation();
		if( !IsVisible( caster, camPos, lightEnd.position ) ) {
			return result;
		}

		Vector3 dirToCam = Vector3Ops::mkVector3( camPos, lightEnd.position );
		const Scalar dist = Vector3Ops::Magnitude( dirToCam );
		if( dist < BDPT_RAY_EPSILON ) {
			return result;
		}
		dirToCam = dirToCam * (1.0 / dist);

		Ray camRay( camPos, -dirToCam );
		const Scalar We = BDPTCameraUtilities::Importance( camera, camRay );
		if( We <= 0 ) {
			return result;
		}

		Scalar fLightNM = 1.0;
		if( lightEnd.type == BDPTVertex::SURFACE && lightEnd.pMaterial && s >= 2 ) {
			Vector3 wiAtLight = Vector3Ops::mkVector3( lightVerts[s - 2].position, lightEnd.position );
			wiAtLight = Vector3Ops::Normalize( wiAtLight );
			fLightNM = EvalBSDFAtVertexNM( lightEnd, wiAtLight, dirToCam, nm );
		}

		const Scalar distSq = dist * dist;
		const Scalar absCosLight = lightEnd.isDelta ?
			Scalar(1.0) : fabs( Vector3Ops::Dot( lightEnd.normal, dirToCam ) );
		const Scalar G = absCosLight / distSq;

		result.contribution = lightEnd.throughputNM * fLightNM * G * We;
		result.rasterPos = rasterPos;
		result.needsSplat = true;
		result.valid = true;

		// --- Update pdfRev at light endpoint for correct MIS ---
		const Scalar savedLightPdfRev = lightEnd.pdfRev;
		{
			Ray camRayToLight( camPos, -dirToCam );
			const Scalar camPdfDir = BDPTCameraUtilities::PdfDirection( camera, camRayToLight );
			const_cast<BDPTVertex&>( lightEnd ).pdfRev =
				BDPTUtilities::SolidAngleToArea( camPdfDir, absCosLight, distSq );
		}

		// --- Update predecessor pdfRev (lightVerts[s-2]) ---
		Scalar savedLightPredPdfRevNM_t0 = 0;
		const bool hasLightPredNM_t0 = (s >= 2 && lightEnd.pMaterial);
		if( hasLightPredNM_t0 )
		{
			const BDPTVertex& lightPred = lightVerts[s - 2];
			savedLightPredPdfRevNM_t0 = lightPred.pdfRev;

			Vector3 wiAtLightEnd = Vector3Ops::mkVector3(
				lightVerts[s - 2].position, lightEnd.position );
			wiAtLightEnd = Vector3Ops::Normalize( wiAtLightEnd );

			const Scalar pdfPredSA = EvalPdfAtVertexNM( lightEnd, dirToCam, -wiAtLightEnd, nm );
			const Vector3 dToPred = Vector3Ops::mkVector3( lightPred.position, lightEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			const Scalar absCosAtPred = fabs( Vector3Ops::Dot( lightPred.normal,
				Vector3Ops::Normalize( dToPred ) ) );
			const_cast<BDPTVertex&>( lightPred ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfPredSA, absCosAtPred, distPredSq );
		}

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );
		const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRev;
		if( hasLightPredNM_t0 ) {
			const_cast<BDPTVertex&>( lightVerts[s - 2] ).pdfRev = savedLightPredPdfRevNM_t0;
		}
		return result;
	}

	//
	// Case: s == 1 -- connect eye endpoint to light vertex
	//
	if( s == 1 )
	{
		const BDPTVertex& eyeEnd = eyeVerts[t - 1];
		const BDPTVertex& lightStart = lightVerts[0];

		if( eyeEnd.type != BDPTVertex::SURFACE || !eyeEnd.pMaterial ) {
			return result;
		}
		if( !eyeEnd.isConnectible ) {
			return result;
		}

		Vector3 dirToLight = Vector3Ops::mkVector3( lightStart.position, eyeEnd.position );
		const Scalar dist = Vector3Ops::Magnitude( dirToLight );
		if( dist < BDPT_RAY_EPSILON ) {
			return result;
		}
		dirToLight = dirToLight * (1.0 / dist);

		if( !IsVisible( caster, eyeEnd.position, lightStart.position ) ) {
			return result;
		}

		// Evaluate Le at this wavelength
		Scalar LeNM = 0;
		if( lightStart.pLuminary && lightStart.pLuminary->GetMaterial() ) {
			const IEmitter* pEmitter = lightStart.pLuminary->GetMaterial()->GetEmitter();
			if( pEmitter ) {
				RayIntersectionGeometric rig(
					Ray( lightStart.position, -dirToLight ), nullRasterizerState );
				rig.bHit = true;
				rig.ptIntersection = lightStart.position;
				rig.vNormal = lightStart.normal;
				rig.onb = lightStart.onb;
				LeNM = pEmitter->emittedRadianceNM( rig, -dirToLight, lightStart.normal, nm );
			}
		} else if( lightStart.pLight ) {
			const RISEPel Le = lightStart.pLight->emittedRadiance( -dirToLight );
			LeNM = 0.2126 * Le[0] + 0.7152 * Le[1] + 0.0722 * Le[2];
		}

		if( LeNM <= 0 ) {
			return result;
		}

		Vector3 woAtEye;
		if( t >= 2 ) {
			woAtEye = Vector3Ops::mkVector3( eyeVerts[t - 2].position, eyeEnd.position );
			woAtEye = Vector3Ops::Normalize( woAtEye );
		} else {
			return result;
		}

		const Scalar fEyeNM = EvalBSDFAtVertexNM( eyeEnd, dirToLight, woAtEye, nm );
		if( fEyeNM <= 0 ) {
			return result;
		}

		Scalar G;
		if( lightStart.isDelta ) {
			const Scalar dist2 = dist * dist;
			const Scalar absCosEye = fabs( Vector3Ops::Dot( eyeEnd.normal, dirToLight ) );
			G = absCosEye / dist2;
		} else {
			G = BDPTUtilities::GeometricTerm(
				lightStart.position, lightStart.normal,
				eyeEnd.position, eyeEnd.normal );
		}

		const Scalar pdfLight = lightStart.pdfFwd;
		if( pdfLight <= 0 ) {
			return result;
		}

		result.contribution = eyeEnd.throughputNM * fEyeNM * LeNM * G / pdfLight;
		result.needsSplat = false;
		result.valid = true;

		// --- Update pdfRev at connection vertices for correct MIS ---
		const Scalar distSq_conn = dist * dist;
		const Scalar savedLightPdfRev = lightStart.pdfRev;
		const Scalar savedEyePdfRev = eyeEnd.pdfRev;

		if( !lightStart.isDelta ) {
			const Scalar pdfRevSA = EvalPdfAtVertexNM( eyeEnd, woAtEye, dirToLight, nm );
			const Scalar absCosAtLight = fabs( Vector3Ops::Dot( lightStart.normal, dirToLight ) );
			const_cast<BDPTVertex&>( lightStart ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfRevSA, absCosAtLight, distSq_conn );
		}

		{
			Scalar emissionPdfDir = 0;
			if( lightStart.pLuminary ) {
				const Scalar cosAtLight = fabs( Vector3Ops::Dot( lightStart.normal, -dirToLight ) );
				emissionPdfDir = (cosAtLight > 0) ? (cosAtLight * INV_PI) : 0;
			} else if( lightStart.pLight ) {
				emissionPdfDir = lightStart.pLight->pdfDirection( -dirToLight );
			}
			const Scalar absCosAtEye = fabs( Vector3Ops::Dot( eyeEnd.normal, dirToLight ) );
			const_cast<BDPTVertex&>( eyeEnd ).pdfRev =
				BDPTUtilities::SolidAngleToArea( emissionPdfDir, absCosAtEye, distSq_conn );
		}

		// --- Update predecessor pdfRev (eyeVerts[t-2]) ---
		Scalar savedEyePredPdfRevNM_s1 = 0;
		const bool hasEyePredNM_s1 = (t >= 2);
		if( hasEyePredNM_s1 )
		{
			const BDPTVertex& eyePred = eyeVerts[t - 2];
			savedEyePredPdfRevNM_s1 = eyePred.pdfRev;

			const Vector3 dToPred = Vector3Ops::mkVector3( eyePred.position, eyeEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			const Vector3 dirToPred = Vector3Ops::Normalize( dToPred );
			const Scalar pdfPredSA = EvalPdfAtVertexNM( eyeEnd, dirToLight, dirToPred, nm );
			// Camera vertex has no meaningful surface normal; use 1.0
			const Scalar absCosAtPred = (eyePred.type == BDPTVertex::CAMERA) ?
				Scalar(1.0) :
				fabs( Vector3Ops::Dot( eyePred.normal, dirToPred ) );
			const_cast<BDPTVertex&>( eyePred ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfPredSA, absCosAtPred, distPredSq );
		}

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );
		const_cast<BDPTVertex&>( lightStart ).pdfRev = savedLightPdfRev;
		const_cast<BDPTVertex&>( eyeEnd ).pdfRev = savedEyePdfRev;
		if( hasEyePredNM_s1 ) {
			const_cast<BDPTVertex&>( eyeVerts[t - 2] ).pdfRev = savedEyePredPdfRevNM_s1;
		}
		return result;
	}

	//
	// Case: t == 1 -- connect light endpoint to camera
	//
	if( t == 1 )
	{
		const BDPTVertex& lightEnd = lightVerts[s - 1];
		if( !lightEnd.isConnectible ) {
			return result;
		}

		if( s >= 2 && lightEnd.type != BDPTVertex::SURFACE ) {
			return result;
		}

		Point2 rasterPos;
		if( !BDPTCameraUtilities::Rasterize( camera, lightEnd.position, rasterPos ) ) {
			return result;
		}

		// Same validity rule as the RGB path above: t == 1 only handles a
		// direct camera connection, not a refracted one through glass.
		const Point3 camPos = camera.GetLocation();
		if( !IsVisible( caster, lightEnd.position, camPos ) ) {
			return result;
		}

		Vector3 dirToCam = Vector3Ops::mkVector3( camPos, lightEnd.position );
		const Scalar dist = Vector3Ops::Magnitude( dirToCam );
		if( dist < BDPT_RAY_EPSILON ) {
			return result;
		}
		dirToCam = dirToCam * (1.0 / dist);

		Ray camRay( camPos, -dirToCam );
		const Scalar We = BDPTCameraUtilities::Importance( camera, camRay );
		if( We <= 0 ) {
			return result;
		}

		Scalar fLightNM = 1.0;
		if( lightEnd.type == BDPTVertex::SURFACE && lightEnd.pMaterial ) {
			if( s >= 2 ) {
				Vector3 wiAtLight = Vector3Ops::mkVector3( lightVerts[s - 2].position, lightEnd.position );
				wiAtLight = Vector3Ops::Normalize( wiAtLight );
				fLightNM = EvalBSDFAtVertexNM( lightEnd, wiAtLight, dirToCam, nm );
			}
		} else if( lightEnd.type == BDPTVertex::LIGHT ) {
			// s == 1: light directly connects to camera
			Scalar LeNM = 0;
			if( lightEnd.pLuminary && lightEnd.pLuminary->GetMaterial() ) {
				const IEmitter* pEmitter = lightEnd.pLuminary->GetMaterial()->GetEmitter();
				if( pEmitter ) {
					RayIntersectionGeometric rig(
						Ray( lightEnd.position, dirToCam ), nullRasterizerState );
					rig.bHit = true;
					rig.ptIntersection = lightEnd.position;
					rig.vNormal = lightEnd.normal;
					rig.onb = lightEnd.onb;
					LeNM = pEmitter->emittedRadianceNM( rig, dirToCam, lightEnd.normal, nm );
				}
			} else if( lightEnd.pLight ) {
				const RISEPel Le = lightEnd.pLight->emittedRadiance( dirToCam );
				LeNM = 0.2126 * Le[0] + 0.7152 * Le[1] + 0.0722 * Le[2];
			}

			const Scalar pdfLight = lightEnd.pdfFwd;
			if( pdfLight <= 0 ) {
				return result;
			}

			const Scalar distSq = dist * dist;
			const Scalar absCosLight = lightEnd.isDelta ?
				Scalar(1.0) : fabs( Vector3Ops::Dot( lightEnd.normal, dirToCam ) );
			const Scalar G = absCosLight / distSq;

			result.contribution = LeNM * G * We / pdfLight;
			result.rasterPos = rasterPos;
			result.needsSplat = true;
			result.valid = true;

			// --- Update pdfRev at connection vertices for correct MIS ---
			const Scalar savedLightPdfRevNM = lightEnd.pdfRev;
			const Scalar savedEyePdfRevNM = eyeVerts[0].pdfRev;

			{
				Ray camRayToLight( camPos, -dirToCam );
				const Scalar camPdfDir = BDPTCameraUtilities::PdfDirection( camera, camRayToLight );
				const_cast<BDPTVertex&>( lightEnd ).pdfRev =
					BDPTUtilities::SolidAngleToArea( camPdfDir, absCosLight, distSq );
			}
			{
				Scalar emPdfDir = 0;
				if( lightEnd.pLuminary ) {
					const Scalar cosEmit = fabs( Vector3Ops::Dot( lightEnd.normal, dirToCam ) );
					emPdfDir = (cosEmit > 0) ? (cosEmit * INV_PI) : 0;
				} else if( lightEnd.pLight ) {
					emPdfDir = lightEnd.pLight->pdfDirection( dirToCam );
				}
				const_cast<BDPTVertex&>( eyeVerts[0] ).pdfRev =
					BDPTUtilities::SolidAngleToArea( emPdfDir, Scalar(1.0), distSq );
			}

			result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );
			const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRevNM;
			const_cast<BDPTVertex&>( eyeVerts[0] ).pdfRev = savedEyePdfRevNM;
			return result;
		}

		if( fLightNM <= 0 ) {
			return result;
		}

		const Scalar distSq = dist * dist;
		const Scalar absCosLight = fabs( Vector3Ops::Dot( lightEnd.normal, dirToCam ) );
		const Scalar G = absCosLight / distSq;

		result.contribution = lightEnd.throughputNM * fLightNM * G * We;
		result.rasterPos = rasterPos;
		result.needsSplat = true;
		result.valid = true;

		// --- Update pdfRev at connection vertices for correct MIS ---
		const Scalar savedLightPdfRevNM2 = lightEnd.pdfRev;
		const Scalar savedEyePdfRevNM2 = eyeVerts[0].pdfRev;

		{
			Ray camRayToLight( camPos, -dirToCam );
			const Scalar camPdfDir = BDPTCameraUtilities::PdfDirection( camera, camRayToLight );
			const_cast<BDPTVertex&>( lightEnd ).pdfRev =
				BDPTUtilities::SolidAngleToArea( camPdfDir, absCosLight, distSq );
		}

		if( s >= 2 && lightEnd.pMaterial ) {
			Vector3 wiAtLightMIS = Vector3Ops::mkVector3(
				lightVerts[s - 2].position, lightEnd.position );
			wiAtLightMIS = Vector3Ops::Normalize( wiAtLightMIS );
			const Scalar pdfRevSA = EvalPdfAtVertexNM( lightEnd, wiAtLightMIS, dirToCam, nm );
			const_cast<BDPTVertex&>( eyeVerts[0] ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfRevSA, Scalar(1.0), distSq );
		}

		// --- Update predecessor pdfRev (lightVerts[s-2]) ---
		Scalar savedLightPredPdfRevNM_t1 = 0;
		const bool hasLightPredNM_t1 = (s >= 2 && lightEnd.pMaterial);
		if( hasLightPredNM_t1 )
		{
			const BDPTVertex& lightPred = lightVerts[s - 2];
			savedLightPredPdfRevNM_t1 = lightPred.pdfRev;

			Vector3 wiAtLightEnd = Vector3Ops::mkVector3(
				lightVerts[s - 2].position, lightEnd.position );
			wiAtLightEnd = Vector3Ops::Normalize( wiAtLightEnd );

			const Scalar pdfPredSA = EvalPdfAtVertexNM( lightEnd, dirToCam, -wiAtLightEnd, nm );
			const Vector3 dToPred = Vector3Ops::mkVector3( lightPred.position, lightEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			const Scalar absCosAtPred = fabs( Vector3Ops::Dot( lightPred.normal,
				Vector3Ops::Normalize( dToPred ) ) );
			const_cast<BDPTVertex&>( lightPred ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfPredSA, absCosAtPred, distPredSq );
		}

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );
		const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRevNM2;
		const_cast<BDPTVertex&>( eyeVerts[0] ).pdfRev = savedEyePdfRevNM2;
		if( hasLightPredNM_t1 ) {
			const_cast<BDPTVertex&>( lightVerts[s - 2] ).pdfRev = savedLightPredPdfRevNM_t1;
		}
		return result;
	}

	//
	// General case: s > 1, t > 1
	//
	{
		const BDPTVertex& lightEnd = lightVerts[s - 1];
		const BDPTVertex& eyeEnd = eyeVerts[t - 1];

		if( !lightEnd.isConnectible || !eyeEnd.isConnectible ) {
			return result;
		}

		if( lightEnd.type != BDPTVertex::SURFACE || !lightEnd.pMaterial ) {
			return result;
		}
		if( eyeEnd.type != BDPTVertex::SURFACE || !eyeEnd.pMaterial ) {
			return result;
		}

		Vector3 dConnect = Vector3Ops::mkVector3( lightEnd.position, eyeEnd.position );
		const Scalar dist = Vector3Ops::Magnitude( dConnect );
		if( dist < BDPT_RAY_EPSILON ) {
			return result;
		}
		dConnect = dConnect * (1.0 / dist);

		if( !IsVisible( caster, eyeEnd.position, lightEnd.position ) ) {
			return result;
		}

		Vector3 wiAtLight = Vector3Ops::mkVector3(
			lightVerts[s - 2].position, lightEnd.position );
		wiAtLight = Vector3Ops::Normalize( wiAtLight );
		const Vector3 woAtLight = -dConnect;

		const Scalar fLightNM = EvalBSDFAtVertexNM( lightEnd, wiAtLight, woAtLight, nm );
		if( fLightNM <= 0 ) {
			return result;
		}

		Vector3 woAtEye = Vector3Ops::mkVector3(
			eyeVerts[t - 2].position, eyeEnd.position );
		woAtEye = Vector3Ops::Normalize( woAtEye );
		const Vector3 wiAtEye = dConnect;

		const Scalar fEyeNM = EvalBSDFAtVertexNM( eyeEnd, wiAtEye, woAtEye, nm );
		if( fEyeNM <= 0 ) {
			return result;
		}

		const Scalar G = BDPTUtilities::GeometricTerm(
			lightEnd.position, lightEnd.normal,
			eyeEnd.position, eyeEnd.normal );

		if( G <= 0 ) {
			return result;
		}

		result.contribution = lightEnd.throughputNM * fLightNM * G * fEyeNM * eyeEnd.throughputNM;
		result.needsSplat = false;
		result.valid = true;

		// --- Update pdfRev at connection vertices for correct MIS ---
		const Scalar distSq_conn = dist * dist;

		const Scalar savedLightPdfRev = lightEnd.pdfRev;
		const Scalar savedEyePdfRev = eyeEnd.pdfRev;

		{
			const Scalar pdfRevSA = EvalPdfAtVertexNM( eyeEnd, woAtEye, dConnect, nm );
			const Scalar absCosAtLight = fabs( Vector3Ops::Dot( lightEnd.normal, dConnect ) );
			const_cast<BDPTVertex&>( lightEnd ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfRevSA, absCosAtLight, distSq_conn );
		}

		{
			const Scalar pdfRevSA = EvalPdfAtVertexNM( lightEnd, wiAtLight, -dConnect, nm );
			const Scalar absCosAtEye = fabs( Vector3Ops::Dot( eyeEnd.normal, dConnect ) );
			const_cast<BDPTVertex&>( eyeEnd ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfRevSA, absCosAtEye, distSq_conn );
		}

		// --- Update predecessor pdfRev at lightVerts[s-2] ---
		Scalar savedLightPredPdfRevNM = 0;
		const bool hasLightPredNM = (s >= 2);
		if( hasLightPredNM )
		{
			const BDPTVertex& lightPred = lightVerts[s - 2];
			savedLightPredPdfRevNM = lightPred.pdfRev;

			const Scalar pdfPredSA = EvalPdfAtVertexNM( lightEnd, woAtLight, -wiAtLight, nm );
			const Vector3 dToPred = Vector3Ops::mkVector3( lightPred.position, lightEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			const Scalar absCosAtPred = fabs( Vector3Ops::Dot( lightPred.normal,
				Vector3Ops::Normalize( dToPred ) ) );
			const_cast<BDPTVertex&>( lightPred ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfPredSA, absCosAtPred, distPredSq );
		}

		// --- Update predecessor pdfRev at eyeVerts[t-2] ---
		Scalar savedEyePredPdfRevNM = 0;
		const bool hasEyePredNM = (t >= 2);
		if( hasEyePredNM )
		{
			const BDPTVertex& eyePred = eyeVerts[t - 2];
			savedEyePredPdfRevNM = eyePred.pdfRev;

			const Scalar pdfPredSA = EvalPdfAtVertexNM( eyeEnd, wiAtEye, -woAtEye, nm );
			const Vector3 dToPred = Vector3Ops::mkVector3( eyePred.position, eyeEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			// Camera vertex has no meaningful surface normal; use 1.0
			const Scalar absCosAtPred = (eyePred.type == BDPTVertex::CAMERA) ?
				Scalar(1.0) :
				fabs( Vector3Ops::Dot( eyePred.normal, Vector3Ops::Normalize( dToPred ) ) );
			const_cast<BDPTVertex&>( eyePred ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfPredSA, absCosAtPred, distPredSq );
		}

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );

		const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRev;
		const_cast<BDPTVertex&>( eyeEnd ).pdfRev = savedEyePdfRev;
		if( hasLightPredNM ) {
			const_cast<BDPTVertex&>( lightVerts[s - 2] ).pdfRev = savedLightPredPdfRevNM;
		}
		if( hasEyePredNM ) {
			const_cast<BDPTVertex&>( eyeVerts[t - 2] ).pdfRev = savedEyePredPdfRevNM;
		}

		return result;
	}
}

//////////////////////////////////////////////////////////////////////
// EvaluateAllStrategiesNM
//////////////////////////////////////////////////////////////////////

std::vector<BDPTIntegrator::ConnectionResultNM> BDPTIntegrator::EvaluateAllStrategiesNM(
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<BDPTVertex>& eyeVerts,
	const IScene& scene,
	const IRayCaster& caster,
	const ICamera& camera,
	const Scalar nm
	) const
{
	const unsigned int nLight = static_cast<unsigned int>( lightVerts.size() );
	const unsigned int nEye = static_cast<unsigned int>( eyeVerts.size() );

	std::vector<ConnectionResultNM> results;
	results.reserve( (nLight + 1) * (nEye + 1) );

	for( unsigned int t = 1; t <= nEye; t++ )
	{
		for( unsigned int s = 0; s <= nLight; s++ )
		{
			if( s + t < 2 ) {
				continue;
			}

			ConnectionResultNM cr = ConnectAndEvaluateNM(
				lightVerts, eyeVerts, s, t, scene, caster, camera, nm );

			if( cr.valid ) {
				results.push_back( cr );
			}
		}
	}

	return results;
}

//////////////////////////////////////////////////////////////////////
// EvaluateSMSStrategies - SMS (Specular Manifold Sampling) for RGB.
// For each non-delta eye vertex, samples a light, traces toward it
// to find specular object chains, then uses ManifoldSolver to find
// valid specular paths.
//////////////////////////////////////////////////////////////////////

std::vector<BDPTIntegrator::ConnectionResult> BDPTIntegrator::EvaluateSMSStrategies(
	const std::vector<BDPTVertex>& eyeVerts,
	const IScene& scene,
	const IRayCaster& caster,
	const ICamera& camera,
	ISampler& sampler
	) const
{
	std::vector<ConnectionResult> results;

	if( !pManifoldSolver ) return results;

	// Limit SMS to the first few diffuse eye vertices.
	const unsigned int maxSMSDepth = (eyeVerts.size() < 4) ?
		static_cast<unsigned int>(eyeVerts.size()) : 4;
	for( unsigned int t = 1; t < maxSMSDepth; t++ )
	{
		const BDPTVertex& eyeVertex = eyeVerts[t];

		// Skip delta vertices (they can't receive caustics on their surface)
		if( eyeVertex.isDelta || !eyeVertex.isConnectible ) continue;
		if( eyeVertex.type != BDPTVertex::SURFACE || !eyeVertex.pMaterial ) continue;

		// Outgoing direction at eye vertex (toward previous vertex)
		Vector3 woAtEye;
		if( t >= 2 ) {
			woAtEye = Vector3Ops::mkVector3( eyeVerts[t-2].position, eyeVertex.position );
			woAtEye = Vector3Ops::Normalize( woAtEye );
		} else {
			woAtEye = Vector3Ops::mkVector3( eyeVerts[0].position, eyeVertex.position );
			woAtEye = Vector3Ops::Normalize( woAtEye );
		}

		// Delegate to the standalone EvaluateAtShadingPoint
		ManifoldSolver::SMSContribution sms = pManifoldSolver->EvaluateAtShadingPoint(
			eyeVertex.position, eyeVertex.normal, eyeVertex.onb,
			eyeVertex.pMaterial, woAtEye,
			scene, caster, sampler );

		if( !sms.valid ) continue;

		ConnectionResult cr;
		cr.contribution = eyeVertex.throughput * sms.contribution;
		cr.misWeight = sms.misWeight;
		cr.needsSplat = false;
		cr.valid = true;

		results.push_back( cr );
	}

	return results;
}

//////////////////////////////////////////////////////////////////////
// EvaluateSMSStrategiesNM - SMS for spectral (single wavelength).
//////////////////////////////////////////////////////////////////////

std::vector<BDPTIntegrator::ConnectionResultNM> BDPTIntegrator::EvaluateSMSStrategiesNM(
	const std::vector<BDPTVertex>& eyeVerts,
	const IScene& scene,
	const IRayCaster& caster,
	const ICamera& camera,
	ISampler& sampler,
	const Scalar nm
	) const
{
	std::vector<ConnectionResultNM> results;

	if( !pManifoldSolver ) return results;
	if( !pLightSampler ) return results;

	// Get luminaries list from the caster
	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
	const LuminaryManager::LuminariesList& luminaries = pLumManager ?
		const_cast<LuminaryManager*>(pLumManager)->getLuminaries() : emptyList;

	// Limit SMS to the first few diffuse eye vertices.
	// Deeper vertices contribute diminishing caustic energy and the
	// cost of BuildSeedChain (one ray-trace per vertex) adds up.
	const unsigned int maxSMSDepth = (eyeVerts.size() < 4) ?
		static_cast<unsigned int>(eyeVerts.size()) : 4;
	for( unsigned int t = 1; t < maxSMSDepth; t++ )
	{
		const BDPTVertex& eyeVertex = eyeVerts[t];

		// Skip delta vertices (they can't receive caustics on their surface)
		if( eyeVertex.isDelta || !eyeVertex.isConnectible ) continue;
		if( eyeVertex.type != BDPTVertex::SURFACE || !eyeVertex.pMaterial ) continue;

		// Sample a light
		LightSample lightSample;
		if( !pLightSampler->SampleLight( scene, luminaries, sampler, lightSample ) ) continue;

		// Build seed chain using ManifoldSolver's BuildSeedChain
		std::vector<ManifoldVertex> seedChain;
		unsigned int chainLen = pManifoldSolver->BuildSeedChain(
			eyeVertex.position, lightSample.position,
			scene, caster, seedChain );

		if( chainLen == 0 || seedChain.empty() ) continue;

		// Run manifold solve
		ManifoldResult mResult = pManifoldSolver->Solve(
			eyeVertex.position, eyeVertex.normal,
			lightSample.position, lightSample.normal,
			seedChain, sampler );

		if( !mResult.valid ) continue;

		// Compute contribution
		// BSDF at eye vertex toward first specular vertex
		// mkVector3(b, a) = b - a, so mkVector3(spec, eye) points from eye to spec
		Vector3 dirToFirstSpec = Vector3Ops::mkVector3(
			mResult.specularChain[0].position, eyeVertex.position );
		Scalar distToSpec = Vector3Ops::Magnitude( dirToFirstSpec );
		if( distToSpec < 1e-8 ) continue;
		dirToFirstSpec = dirToFirstSpec * (1.0 / distToSpec);

		const Vector3 wiAtEye = dirToFirstSpec;

		Vector3 woAtEye;
		if( t >= 2 ) {
			woAtEye = Vector3Ops::mkVector3( eyeVerts[t-2].position, eyeVertex.position );
			woAtEye = Vector3Ops::Normalize( woAtEye );
		} else {
			woAtEye = Vector3Ops::mkVector3( eyeVerts[0].position, eyeVertex.position );
			woAtEye = Vector3Ops::Normalize( woAtEye );
		}

		Scalar fEye = EvalBSDFAtVertexNM( eyeVertex, wiAtEye, woAtEye, nm );
		if( fEye <= 0 ) continue;

		// Emitter radiance (use max component as scalar approximation for spectral)
		Scalar Le = ColorMath::MaxValue( lightSample.Le );

		// Geometric term: eye vertex to first specular vertex
		const ManifoldVertex& firstSpec = mResult.specularChain[0];
		Scalar G_eyeToSpec = BDPTUtilities::GeometricTerm(
			eyeVertex.position, eyeVertex.normal,
			firstSpec.position, firstSpec.normal );

		if( G_eyeToSpec <= 0 ) continue;

		// Geometric term: last specular vertex to emitter
		const ManifoldVertex& lastSpec = mResult.specularChain.back();
		Vector3 dirSpecToLight = Vector3Ops::mkVector3(
			lastSpec.position, lightSample.position );
		Scalar distSpecToLight = Vector3Ops::Magnitude( dirSpecToLight );
		if( distSpecToLight < 1e-8 ) continue;
		dirSpecToLight = dirSpecToLight * (1.0 / distSpecToLight);

		Scalar cosAtLastSpec = fabs( Vector3Ops::Dot( lastSpec.normal, dirSpecToLight ) );
		Scalar cosAtLight = fabs( Vector3Ops::Dot( lightSample.normal, dirSpecToLight ) );
		Scalar G_specToLight = cosAtLastSpec * cosAtLight / (distSpecToLight * distSpecToLight);

		if( G_specToLight <= 0 ) continue;

		// Visibility: skip for now (same rationale as RGB variant above)

		// Chain throughput (Fresnel factors)
		Scalar smsChainContrib = ColorMath::MaxValue( mResult.contribution );

		// Full contribution
		ConnectionResultNM cr;
		cr.contribution = eyeVertex.throughputNM * fEye
			* G_eyeToSpec * smsChainContrib * G_specToLight * Le
			/ (lightSample.pdfSelect * lightSample.pdfPosition);

		cr.misWeight = 1.0 / mResult.pdf;
		cr.needsSplat = false;
		cr.valid = true;

		results.push_back( cr );
	}

	return results;
}
