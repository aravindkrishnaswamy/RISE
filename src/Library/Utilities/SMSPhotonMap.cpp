//////////////////////////////////////////////////////////////////////
//
//  SMSPhotonMap.cpp - Implementation of the SMS photon-aided seed
//    store.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-20
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SMSPhotonMap.h"

#include "../Interfaces/ILog.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ISPF.h"
#include "../Interfaces/ILightPriv.h"
#include "../Interfaces/ILightManager.h"
#include "../Interfaces/ILuminaryManager.h"
#include "../Interfaces/IScenePriv.h"
#include "../Intersection/RayIntersection.h"
#include "../Rendering/LuminaryManager.h"
#include "BoundingBox.h"
#include "IndependentSampler.h"
#include "RandomNumbers.h"
#include "IORStack.h"
#include "IORStackSeeding.h"
#include "ThreadPool.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>

using namespace RISE;
using namespace RISE::Implementation;

//////////////////////////////////////////////////////////////////////
// KD-tree implementation
//
// Lifted from VCMLightVertexStore's kd-tree clone (same layout as
// PhotonMap.h's PhotonMapCore) with the comparator and element type
// changed to SMSPhoton.  The partition-inclusive end-iterator fix
// and the tangent-case traversal fix are retained — see comments
// at those sites in VCMLightVertexStore.cpp.
//////////////////////////////////////////////////////////////////////
namespace
{
	inline bool LessThanX( const SMSPhoton& a, const SMSPhoton& b ) { return a.ptPosition.x < b.ptPosition.x; }
	inline bool LessThanY( const SMSPhoton& a, const SMSPhoton& b ) { return a.ptPosition.y < b.ptPosition.y; }
	inline bool LessThanZ( const SMSPhoton& a, const SMSPhoton& b ) { return a.ptPosition.z < b.ptPosition.z; }

	inline int ComputeMedian( const int from, const int to )
	{
		int median = 1;
		while( ( 4 * median ) <= ( to - from + 1 ) ) {
			median += median;
		}
		if( ( 3 * median ) <= ( to - from + 1 ) ) {
			median += median;
			median += from - 1;
		} else {
			median = to - median + 1;
		}
		return median;
	}

	void BalanceSegment(
		std::vector<SMSPhoton>& verts,
		BoundingBox& bbox,
		const int from,
		const int to
		)
	{
		if( to - from <= 0 ) {
			return;
		}

		unsigned char axis = 2;
		const Vector3& extents = bbox.GetExtents();
		if( extents.x > extents.y && extents.x > extents.z ) {
			axis = 0;
		} else if( extents.y > extents.z ) {
			axis = 1;
		}

		const int median = ComputeMedian( from, to );

		switch( axis )
		{
		case 0:
			std::nth_element( verts.begin() + from, verts.begin() + median, verts.begin() + to + 1, LessThanX );
			break;
		case 1:
			std::nth_element( verts.begin() + from, verts.begin() + median, verts.begin() + to + 1, LessThanY );
			break;
		case 2:
		default:
			std::nth_element( verts.begin() + from, verts.begin() + median, verts.begin() + to + 1, LessThanZ );
			break;
		}

		verts[median].plane = axis;

		{
			const Scalar tmp = bbox.ur[axis];
			bbox.ur[axis] = verts[median].ptPosition[axis];
			BalanceSegment( verts, bbox, from, median - 1 );
			bbox.ur[axis] = tmp;
		}

		{
			const Scalar tmp = bbox.ll[axis];
			bbox.ll[axis] = verts[median].ptPosition[axis];
			BalanceSegment( verts, bbox, median + 1, to );
			bbox.ll[axis] = tmp;
		}
	}

	void LocateAllInRadiusSq(
		const std::vector<SMSPhoton>& verts,
		const Point3& loc,
		const Scalar maxDistSq,
		std::vector<SMSPhoton>& out,
		const int from,
		const int to
		)
	{
		if( to - from < 0 ) {
			return;
		}

		const int median = ComputeMedian( from, to );

		const Vector3 v = Vector3Ops::mkVector3( loc, verts[median].ptPosition );
		const Scalar distanceToVertexSq = Vector3Ops::SquaredModulus( v );

		if( distanceToVertexSq < maxDistSq ) {
			out.push_back( verts[median] );
		}

		const int axis = verts[median].plane;
		const Scalar planeDelta = loc[axis] - verts[median].ptPosition[axis];
		const Scalar planeDeltaSq = planeDelta * planeDelta;

		if( planeDeltaSq > maxDistSq ) {
			if( planeDelta <= 0 ) {
				LocateAllInRadiusSq( verts, loc, maxDistSq, out, from, median - 1 );
			} else {
				LocateAllInRadiusSq( verts, loc, maxDistSq, out, median + 1, to );
			}
		} else {
			LocateAllInRadiusSq( verts, loc, maxDistSq, out, from, median - 1 );
			LocateAllInRadiusSq( verts, loc, maxDistSq, out, median + 1, to );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// SMSPhotonMap
//////////////////////////////////////////////////////////////////////

SMSPhotonMap::SMSPhotonMap() :
	mBuilt( false ),
	mCachedAutoRadius( -1 )
{
}

SMSPhotonMap::~SMSPhotonMap()
{
}

void SMSPhotonMap::Clear()
{
	mPhotons.clear();
	mBuilt = false;
	mCachedAutoRadius = -1;
}

//////////////////////////////////////////////////////////////////////
// Photon emission
//
// Mirrors PhotonTracer::TraceNPhotons (loop over luminaries + lights)
// + CausticPelPhotonTracer::TracePhoton (specular-chain walk, store
// on first diffuse after specular).  The difference from classical
// caustic PM: we also record the FIRST specular caster entry point
// so ManifoldSolver can use it as a Newton-iteration seed.
//
// The walk is fully inline (no recursion — deterministic bound of
// kMaxBounces) to avoid the static-variable shared-state in
// CausticPelPhotonTracer::TracePhoton's numRecursions counter.
//////////////////////////////////////////////////////////////////////
namespace
{
	// Walk one photon through the scene.  Returns true and fills 'out' if
	// the photon terminated on a diffuse surface AFTER at least one
	// specular hit (the caustic pattern).  Returns false otherwise.
	//
	// Records the FULL specular chain (positions, normals, objects,
	// materials, eta, and ray-direction-dependent exit flags) into
	// out.chain[0..chainLen-1] in photon-direction order.  Photons
	// exceeding kSMSMaxPhotonChain specular hits are dropped (return
	// false) rather than truncated — truncating would leave SMS with
	// an incomplete seed chain that can't be used directly.
	bool TraceSMSPhoton(
		const IScene& scene,
		Ray ray,
		RISEPel power,
		RandomNumberGenerator& rng,
		const IORStack& ior_stack_in,
		SMSPhoton& out
		)
	{
		const unsigned int kMaxBounces = 12;
		const Scalar kExtinction = 0.0001;
		const IObjectManager* pObjMgr = scene.GetObjects();
		if( !pObjMgr ) return false;

		unsigned char specularHits = 0;
		IORStack ior_stack( ior_stack_in );

		for( unsigned int bounce = 0; bounce < kMaxBounces; bounce++ )
		{
			if( ColorMath::MaxValue( power ) < kExtinction ) {
				return false;
			}

			ray.SetDir( Vector3Ops::Normalize( ray.Dir() ) );

			RayIntersection ri( ray, nullRasterizerState );
			pObjMgr->IntersectRay( ri, true, true, false );
			if( !ri.geometric.bHit ) {
				return false;
			}

			if( ri.pModifier ) {
				ri.pModifier->Modify( ri.geometric );
			}

			ior_stack.SetCurrentObject( ri.pObject );

			// CAPTURE the pre-scatter "is this object already in the stack"
			// state before the SPF mutates the stack.  The SPF's Scatter()
			// pushes the object's IOR on entry and pops on exit, so calling
			// containsCurrent() AFTER Scatter() gives the POST-transition
			// state — the exact opposite of what the chain-vertex flag
			// semantics want.  Earlier bug: reading it post-scatter flagged
			// every entering refraction as exiting, producing spurious
			// chainLen=1 "exit" vertices and massive fireflies on bezier-
			// patch teapot + displaced-shell geometries.
			const bool bSameObjectAlreadyPreScatter = ior_stack.containsCurrent();

			ISPF* pSPF = ri.pMaterial ? ri.pMaterial->GetSPF() : 0;
			IBSDF* pBRDF = ri.pMaterial ? ri.pMaterial->GetBSDF() : 0;

			if( !pSPF ) {
				return false;
			}

			ScatteredRayContainer scattered;
			IndependentSampler samplerWrapper( rng );
			pSPF->Scatter( ri.geometric, samplerWrapper, scattered, ior_stack );

			// Classical caustic pattern: a hit with a non-delta BSDF
			// means "this photon landed on a diffuse surface".  If we've
			// already traversed a specular chain, this is the deposit
			// point we wanted.
			if( pBRDF && specularHits > 0 ) {
				out.ptPosition       = ri.geometric.ptIntersection;
				out.plane            = 0;
				out.chainLen         = specularHits;
				out.entryPoint       = out.chain[0].position;
				out.entryObject      = out.chain[0].pObject;
				out.power            = power;
				return true;
			}

			// Pick a single specular scattered ray (no branching — branching
			// here would bias toward photons that land on reflect-at-both-
			// sides materials).  RandomlySelectNonDiffuse respects the
			// per-ray kray weights.
			ScatteredRay* pScat = scattered.RandomlySelectNonDiffuse( rng.CanonicalRandom(), false );
			if( !pScat ) {
				return false;
			}

			// Drop photons whose chain would exceed the storage budget.
			if( specularHits >= static_cast<unsigned char>( kSMSMaxPhotonChain ) ) {
				return false;
			}

			// Determine the entering/exiting flag for SMS chain reuse.
			//
			// Using the IOR stack rather than the raw cosI sign is
			// REQUIRED for double-sided / displaced meshes where the
			// canonical outward normal doesn't flip between the hits
			// that enter and exit the glass — the cosI test then reports
			// both hits with the same sign and we lose the entry/exit
			// alternation, producing a chain where every vertex looks
			// like an "exit refraction" and receiver-side SMS applies
			// the n²·T radiance-exit factor twice (→ ~17× over-bright
			// chainLen=2 fireflies in the firefly analysis).
			//
			// `containsCurrent()` returns true iff the current object's
			// IOR has already been pushed onto the stack — i.e. the
			// photon is CURRENTLY INSIDE that object, so this hit is an
			// exit.  Matches BuildSeedChain's receiver-side logic.
			//
			// IMPORTANT: the chosen scatter mode (refraction vs reflection)
			// matters for SMS chain semantics.  A refractor's Scatter()
			// emits BOTH a refraction and a Fresnel reflection ray;
			// picking the reflection branch means the photon bounced OFF
			// the surface without changing medium.  We record bit 1 of
			// flags for this so the SMS consumer can set mv.isReflection
			// correctly and avoid applying the n²·T radiance-exit factor
			// on a chain vertex the photon never actually refracted
			// through.  When bReflection=true, the medium stays the same
			// so we preserve the raw cosI-based side indicator (consumer
			// uses it only for Fresnel eta_i/eta_t bookkeeping on the
			// reflection, which is direction-dependent).
			// Side-of-surface decision: use the GEOMETRIC normal so the
			// `bEntering` flag stamped on the photon chain matches the
			// actual face orientation (not Phong-perturbed shading).
			// A bump-mapped reflector can otherwise mislabel entering vs
			// exiting on a reflection vertex, propagating a wrong
			// `isExiting` bit into the photon record and downstream
			// Fresnel etaI/etaT pair.  PBRT 4e §10.1.1.
			const Scalar cosI = Vector3Ops::Dot( ray.Dir(), ri.geometric.vGeomNormal );
			const bool bReflection = ( pScat->type == ScatteredRay::eRayReflection );
			const bool bEntering = bReflection
			    ? ( cosI < 0 )          // reflection: medium unchanged; keep
			                             // cosI-based side for Fresnel lookup
			    : ( !bSameObjectAlreadyPreScatter ); // refraction: pre-scatter stack state

			// Record vertex in photon-direction order.
			SMSPhotonChainVertex& v = out.chain[specularHits];
			v.position  = ri.geometric.ptIntersection;
			v.normal    = ri.geometric.vNormal;
			// Store geometric face normal alongside the shading normal so
			// the receiver-side ManifoldSolver reconstruction can populate
			// `mv.geomNormal` and the validator's geometric side-test fix
			// (ManifoldSolver::ValidateChainPhysics) actually fires on
			// photon-aided chains.  Without this slot, the validator would
			// silently fall back to shading via its NEARZERO check.
			v.geomNormal = ri.geometric.vGeomNormal;
			v.pObject   = ri.pObject;
			v.pMaterial = ri.pMaterial;
			// eta comes from the material's specular info at this vertex.
			// GetSpecularInfo returns (isSpecular, ior, canRefract, ...).
			// Use the IOR of the material directly.
			{
				SpecularInfo specInfo = ri.pMaterial->GetSpecularInfo( ri.geometric, ior_stack );
				v.eta = specInfo.ior;
			}
			v.flags = static_cast<unsigned char>(
				( bEntering   ? 0x0 : 0x1 ) |
				( bReflection ? 0x2 : 0x0 ) );
			specularHits++;

			ray = pScat->ray;
			ray.Advance( 1e-8 );
			power = power * pScat->kray;
			if( pScat->ior_stack ) {
				ior_stack = *pScat->ior_stack;
			}
		}
		// Ran out of bounce budget without landing on diffuse.
		return false;
	}
}

unsigned int SMSPhotonMap::Build(
	const IScene& scene,
	const unsigned int numPhotons
	)
{
	Clear();

	if( numPhotons == 0 ) {
		return 0;
	}

	// Build a LuminaryManager bound to this scene.  Mirrors what
	// PhotonTracer does in its constructor / AttachScene pair.
	// LuminaryManager's destructor is protected (reference-counted), so
	// heap-allocate and safe_release at the end.
	LuminaryManager* pLumManager = new LuminaryManager();
	// AttachScene wants IScenePriv* which derives from IScene; the only
	// in-tree implementation is Scene.  Cast is safe here because the
	// rasterizer always constructs through Scene.
	pLumManager->AttachScene( const_cast<IScenePriv*>( dynamic_cast<const IScenePriv*>( &scene ) ) );
	const LuminaryManager::LuminariesList& luminaries = pLumManager->getLuminaries();

	// Accumulate total exitance across all emitters so we can budget
	// photons proportional to each emitter's power.
	Scalar totalExitance = 0;
	for( LuminaryManager::LuminariesList::const_iterator i = luminaries.begin(); i != luminaries.end(); ++i ) {
		if( i->pLum && i->pLum->GetMaterial() && i->pLum->GetMaterial()->GetEmitter() ) {
			const Scalar area = i->pLum->GetArea();
			const RISEPel pw = i->pLum->GetMaterial()->GetEmitter()->averageRadiantExitance() * area;
			totalExitance += ColorMath::MaxValue( pw );
		}
	}
	const ILightManager* pLm = scene.GetLights();
	const ILightManager::LightsList& lightsList = pLm ? pLm->getLights() : ILightManager::LightsList();
	for( ILightManager::LightsList::const_iterator m = lightsList.begin(); m != lightsList.end(); ++m ) {
		const ILightPriv* l = *m;
		if( l && l->CanGeneratePhotons() ) {
			totalExitance += ColorMath::MaxValue( l->radiantExitance() );
		}
	}

	if( totalExitance <= 0 ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"SMSPhotonMap::Build:: no emitters with positive exitance — map empty" );
		return 0;
	}

	mPhotons.reserve( numPhotons );
	RandomNumberGenerator rng;
	RandomNumberGenerator geomRng;

	// Shoot photons proportional to each emitter's exitance.
	//
	// Known limitation: this is a single-threaded fixed-length loop.  For
	// typical SMS budgets (100k-1M) it's ~0.1-1 s so we don't bother
	// parallelizing in this initial version.  Future work: split the
	// budget across GlobalThreadPool workers and concat per-thread
	// buffers (VCM light-pass pattern).
	unsigned int numShot = 0;

	// ---- Mesh luminaries ----
	for( LuminaryManager::LuminariesList::const_iterator i = luminaries.begin(); i != luminaries.end(); ++i ) {
		if( !i->pLum || !i->pLum->GetMaterial() || !i->pLum->GetMaterial()->GetEmitter() ) {
			continue;
		}
		const IEmitter* pEmitter = i->pLum->GetMaterial()->GetEmitter();
		const Scalar area = i->pLum->GetArea();
		const RISEPel emitterTotal = pEmitter->averageRadiantExitance() * area;

		const unsigned int target = static_cast<unsigned int>(
			ColorMath::MaxValue( emitterTotal ) / totalExitance * numPhotons );
		if( target == 0 ) continue;

		unsigned int shotThisEmitter = 0;
		while( shotThisEmitter < target ) {
			Ray r;
			Vector3 normal;
			Point2 coord;
			i->pLum->UniformRandomPoint( &r.origin, &normal, &coord,
				Point3( geomRng.CanonicalRandom(), geomRng.CanonicalRandom(), geomRng.CanonicalRandom() ) );

			RayIntersectionGeometric rig( r, nullRasterizerState );
			rig.vNormal = normal;
			// Luminary normal from UniformRandomPoint is geometric; mirror.
			rig.vGeomNormal = normal;
			rig.ptCoord = coord;
			rig.onb.CreateFromW( rig.vNormal );

			r.SetDir( pEmitter->getEmmittedPhotonDir( rig,
				Point2( geomRng.CanonicalRandom(), geomRng.CanonicalRandom() ) ) );

			const RISEPel power = emitterTotal;

			// Seed the IOR stack with the chain of dielectric objects
			// that physically contain the emit point.  Without this, a
			// luminaire sealed inside nested refractors (the canonical
			// "lambertian sphere inside an air_cavity inside a glass
			// egg" Veach setup) treats the first inner-boundary hit as
			// "entering" — the IOR-matched cavity then fails the Fresnel
			// sign check and every photon dies on bounce 1.  Mirrors
			// BDPT's light-subpath seeding (BDPTIntegrator.cpp:1356).
			IORStack iorStack( 1.0 );
			IORStackSeeding::SeedFromPoint( iorStack, r.origin, scene );
			SMSPhoton out;
			if( TraceSMSPhoton( scene, r, power, rng, iorStack, out ) ) {
				mPhotons.push_back( out );
			}
			shotThisEmitter++;
			numShot++;

			// Safety: abort if storage is clearly saturated with failures.
			if( shotThisEmitter > target * 100 && mPhotons.empty() ) {
				break;
			}
		}
	}

	// ---- Non-mesh lights (point, spot, directional, ...) ----
	for( ILightManager::LightsList::const_iterator m = lightsList.begin(); m != lightsList.end(); ++m ) {
		const ILightPriv* l = *m;
		if( !l || !l->CanGeneratePhotons() ) continue;

		const Scalar lightPower = ColorMath::MaxValue( l->radiantExitance() );
		const unsigned int target = static_cast<unsigned int>(
			lightPower / totalExitance * numPhotons );
		if( target == 0 ) continue;

		unsigned int shotThisLight = 0;
		while( shotThisLight < target ) {
			Ray r = l->generateRandomPhoton(
				Point3( geomRng.CanonicalRandom(), geomRng.CanonicalRandom(), geomRng.CanonicalRandom() ) );

			const Scalar pdf = l->pdfDirection( r.Dir() );
			const RISEPel power = ( pdf > 0 ) ?
				l->emittedRadiance( r.Dir() ) * ( 1.0 / pdf ) :
				RISEPel( 0, 0, 0 );

			// See the mesh-luminaire branch above for the rationale on
			// pre-populating the IOR stack with the dielectric objects
			// that contain the emit point.
			IORStack iorStack( 1.0 );
			IORStackSeeding::SeedFromPoint( iorStack, r.origin, scene );
			SMSPhoton out;
			if( TraceSMSPhoton( scene, r, power, rng, iorStack, out ) ) {
				mPhotons.push_back( out );
			}
			shotThisLight++;
			numShot++;

			if( shotThisLight > target * 100 && mPhotons.empty() ) {
				break;
			}
		}
	}

	// ---- Build kd-tree ----
	if( !mPhotons.empty() ) {
		BoundingBox bbox(
			Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
			Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) );
		for( std::vector<SMSPhoton>::const_iterator p = mPhotons.begin(); p != mPhotons.end(); ++p ) {
			bbox.Include( p->ptPosition );
		}
		BalanceSegment( mPhotons, bbox, 0, static_cast<int>( mPhotons.size() ) - 1 );
	}
	mBuilt = true;

	GlobalLog()->PrintEx( eLog_Event,
		"SMSPhotonMap::Build:: shot=%u stored=%zu (caustic hits) ratio=%.2f%%",
		numShot, mPhotons.size(),
		numShot > 0 ? 100.0 * static_cast<double>( mPhotons.size() ) / static_cast<double>( numShot ) : 0.0 );

	safe_release( pLumManager );

	return static_cast<unsigned int>( mPhotons.size() );
}

void SMSPhotonMap::QuerySeeds(
	const Point3& center,
	const Scalar radiusSq,
	std::vector<SMSPhoton>& out
	) const
{
	if( !mBuilt || mPhotons.empty() || radiusSq <= 0 ) {
		return;
	}
	LocateAllInRadiusSq(
		mPhotons,
		center,
		radiusSq,
		out,
		0,
		static_cast<int>( mPhotons.size() ) - 1 );
}

Scalar SMSPhotonMap::GetAutoRadius() const
{
	if( mCachedAutoRadius >= 0 ) {
		return mCachedAutoRadius;
	}

	// Scene-auto radius: the bounding-box diagonal of the photon
	// landing positions times a small factor.  Mirrors VCM's
	// `0.01 * median_segment` heuristic but uses the landing-point
	// bounding box directly — which for SMS tracks the size of the
	// caustic-receiving region, not the entire scene.
	//
	// Factor 0.01 gives a radius of ~1% of the caustic-footprint
	// diameter; that's a few triangles on a well-tessellated receiver
	// and empirically captures enough photons for a reliable seed
	// density without over-blurring.
	if( mPhotons.empty() ) {
		mCachedAutoRadius = 0;
		return 0;
	}

	Point3 mn = mPhotons[0].ptPosition;
	Point3 mx = mn;
	for( std::size_t i = 1; i < mPhotons.size(); i++ ) {
		const Point3& p = mPhotons[i].ptPosition;
		if( p.x < mn.x ) mn.x = p.x;  if( p.x > mx.x ) mx.x = p.x;
		if( p.y < mn.y ) mn.y = p.y;  if( p.y > mx.y ) mx.y = p.y;
		if( p.z < mn.z ) mn.z = p.z;  if( p.z > mx.z ) mx.z = p.z;
	}
	const Vector3 d = Vector3Ops::mkVector3( mx, mn );
	const Scalar diag = Vector3Ops::Magnitude( d );
	mCachedAutoRadius = diag * 0.01;
	return mCachedAutoRadius;
}
