//////////////////////////////////////////////////////////////////////
//
//  SubSurfaceScatteringShaderOp.cpp - Implementation of the
//  point-sampled BSSRDF shader op.
//
//  See SubSurfaceScatteringShaderOp.h for algorithm overview.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 18, 2005
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SubSurfaceScatteringShaderOp.h"
#include "../../Utilities/GeometricUtilities.h"
#include "../../Interfaces/IGeometry.h"		// CanBeAreaLight(): SSS needs real surface sampling
#include "../../Utilities/stl_utils.h"
#include "../../Sampling/HaltonPoints.h"
#include "../../Utilities/Color/RGBSpectra.h"	// RGBUnboundedSpectrum (RGB->spectral uplift for PerformOperationNM)
#include <mutex>									// std::lock_guard (exception-safe create_mutex)

using namespace RISE;
using namespace RISE::Implementation;

SubSurfaceScatteringShaderOp::SubSurfaceScatteringShaderOp( 
	const unsigned int numPoints_,
	const Scalar error_,
	const unsigned int maxPointsPerNode_,
	const unsigned char maxDepth_,
	const Scalar irrad_scale_,
	const bool multiplyBSDF_,
	const bool regenerate_,
	const IShader& shader_,
	const ISubSurfaceExtinctionFunction& extinction_,
	const bool cache_,
	const bool low_discrepancy_
	) : 
  numPoints( numPoints_ ),
  error( error_ ),
  maxPointsPerNode( maxPointsPerNode_ ),
  maxDepth( maxDepth_ ),
  irrad_scale( irrad_scale_ ),
  multiplyBSDF( multiplyBSDF_ ),
  regenerate( regenerate_ ),
  shader( shader_ ),
  extinction( extinction_ ),
  cache( cache_ ),
  low_discrepancy( low_discrepancy_ )
{
	extinction.addref();
	shader.addref();
}

SubSurfaceScatteringShaderOp::~SubSurfaceScatteringShaderOp( )
{
	PointSetMap::iterator i, e;
	for( i=pointsets.begin(), e=pointsets.end(); i!=e; i++ ) {
		delete i->second;
	}

	pointsets.clear();

	extinction.release();
	shader.release();
}

//! Tells the shader to apply shade to the given intersection point
void SubSurfaceScatteringShaderOp::PerformOperation(
	const RuntimeContext& rc,					///< [in] Runtime context
	const RayIntersection& ri,					///< [in] Intersection information 
	const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	RISEPel& c,									///< [in/out] Resultant color from op
	const IORStack& ior_stack,			///< [in/out] Index of refraction stack
	const ScatteredRayContainer* pScat			///< [in] Scattering information
	) const
{
	c = RISEPel(0.0);

	const IScene* pScene = caster.GetAttachedScene();

	// If these three things don't exist, then we can't do anything for this object
	if( !pScene ) {
		return;
	}

	// Only do stuff on a normal pass or on final gather
	if( !rc.IsNormalShadingPass() && rs.type == rs.eRayView ) {
		return;
	}

	// Fast-preview fallback for the interactive viewport.
	//
	// The full SSS path below builds a per-object irradiance point
	// set on first hit (numPoints surface samples × full Shade per
	// sample, each shade casts shadow rays to all lights) and then
	// evaluates a hierarchical octree per pixel.  At numPoints =
	// 200K (typical for production-quality SSS) the build alone is
	// many seconds while holding `create_mutex`, blocking the
	// rasterizer's cancel-restart loop entirely — the user sees a
	// frozen viewport with no preview rendering.
	//
	// In interactive preview, we delegate to the embedded
	// irradiance-capture shader instead.  That shader is the one
	// configured for capturing irradiance at point samples
	// (typically a directlighting_shaderop on a Lambertian BSDF);
	// running it on the camera-ray hit gives a fast direct-lit
	// fallback — visible objects, correct positions, useful for
	// scene navigation.  The production rasterizer leaves
	// bFastPreview false and gets the full SSS contribution.
	//
	// Bypassing the StateCache deliberately: a fast-preview value
	// is NOT a valid cache entry for the production render, and a
	// cached production value isn't valid for fast preview either.
	// Both paths re-compute on demand; the production path is
	// already heavy enough that the cache hit pays off.
	if( rc.bFastPreview ) {
		shader.Shade( rc, ri, caster, rs, c, ior_stack );
		return;
	}

	// Lets check our rasterizer state to see if we even need to do work!
	if( cache ) {
		if( !rc.StateCache_HasStateChanged( this, c, ri.pObject, ri.geometric.rast ) ) {
			// State hasn't changed, use the value already there
			return;
		}
	}

	// Find-or-build this object's irradiance point set.  ALL access to
	// `pointsets` (a std::map, NOT thread-safe) is serialized by a std::lock_guard
	// on create_mutex spanning the whole find-or-build; the guard unlocks on EVERY
	// exit -- normal return, the CanBeAreaLight sentinel, AND a bad_alloc thrown by
	// the large build -- so a build OOM can never leave the mutex held and deadlock
	// the render.
	// A prior double-checked-locking variant did an UNLOCKED find() that raced
	// with the locked insert below: the unlocked read could observe a torn /
	// half-inserted node -- a data race on std::map, i.e. undefined behavior.
	// This is a latent-UB fix ONLY; it does NOT cure the separate, pre-existing
	// build non-determinism (the lazy build below runs once on whichever thread
	// wins the race, consuming that thread's scheduling-dependent rc.random, so
	// the whole SSS image lands on one of a few discrete brightness levels
	// run-to-run).  That residual survives this fix and hits the RGB path
	// identically -- see the SubsurfaceScatteringSpectralTest header.  The
	// one-time build runs inside the lock; the expensive octree Evaluate (Pass
	// 2) runs OUTSIDE the lock on the now-immutable octree, so render threads
	// still evaluate in parallel.
	PointSetOctree* ps = 0;
	{
		std::lock_guard<RMutex> guard( create_mutex );
		PointSetMap::iterator it = pointsets.find( ri.pObject );
		if( it == pointsets.end() ) {
			// SSS point-set generation uniformly samples the object's SURFACE via
			// UniformRandomPoint/GetArea.  A geometry that cannot honour that contract
			// (CanBeAreaLight() false -- e.g. a degenerate zero-area field) would
			// collapse the samples -> a bogus irradiance cache.  Refuse SSS on such
			// geometry, with a diagnostic, rather than build a garbage sample set.
			const IGeometry* pSSSGeom = ri.pObject ? ri.pObject->GetGeometry() : 0;
			if( pSSSGeom && !pSSSGeom->CanBeAreaLight() ) {
				GlobalLog()->PrintEasyWarning( "SubSurfaceScatteringShaderOp:: object geometry cannot be uniformly surface-sampled (CanBeAreaLight() == false); subsurface scattering is unsupported on it -- skipping (no SSS contribution)." );
				pointsets[ri.pObject] = 0;	// cache null sentinel: warn once per object, skip the bogus build on every later hit
				c = RISEPel( 0.0 );
				return;
			}
			// Pass 1: Generate the irradiance point set for this object.
			// This happens once per object, lazily on first hit.
			GlobalLog()->PrintEasyInfo( "SubSurfaceScatteringShaderOp:: Generating point sample set for object" );
		
			PointSetOctree::PointSet points;
			BoundingBox bbox( Point3(RISE_INFINITY,RISE_INFINITY,RISE_INFINITY), Point3(-RISE_INFINITY,-RISE_INFINITY,-RISE_INFINITY) );

			// Since we are uniformly sampling, we just divide the overall surface area by the number of sample points
	//		const Scalar sample_area = ri.pObject->GetArea() / Scalar(numPoints);

			// Use a halton point sequence to make sure the sampling points are distributed in a good way
			MultiHalton mh;
		
			for( unsigned int i=0; i<numPoints; i++ ) {
				// Ask the object for a uniform random point
				PointSetOctree::SamplePoint sp;
				Vector3 normal;

				Point3 random_variables;
				if( low_discrepancy ) {
					random_variables = Point3( mh.mod1(mh.halton(0,i)), mh.mod1(mh.halton(1,i)), mh.mod1(mh.halton(2,i)) );
				} else {
					random_variables = Point3( rc.random.CanonicalRandom(), rc.random.CanonicalRandom(), rc.random.CanonicalRandom() );
				}

				ri.pObject->UniformRandomPoint( &sp.ptPosition, &normal, 0, random_variables );
				// We may want in the future to move the point in (away from the surface) if we decide to add the option of occluders
	//				sp.ptPosition = Point3Ops::mkPoint3( sp.ptPosition, normal*NEARZERO );		// move the sample points slightly away from the surface

				// Now compute the irradiance for this point using the BDF
				RayIntersection newri( ri );
				newri.geometric.ray = Ray( sp.ptPosition, -normal );
				newri.geometric.bHit = true;
				newri.geometric.ptIntersection = sp.ptPosition;
				newri.geometric.vNormal = normal;

				// Advance the ray for the purpose of shading, this should help reduce errors
				newri.geometric.ray.Advance( 1e-8 );

				shader.Shade( rc, newri, caster, rs, sp.irrad, ior_stack );

				// Discard points that have no illumination
				if( ColorMath::MaxValue(sp.irrad) > 0 ) {
					sp.irrad = sp.irrad * irrad_scale;
					points.push_back( sp );
					bbox.Include( sp.ptPosition );
				}
			}

	//		bbox.Grow( NEARZERO );		// Grow for error tolerance
			bbox.EnsureBoxHasVolume();
			ps = new PointSetOctree( bbox, maxPointsPerNode );

			if( points.size() < 1 ) {
				GlobalLog()->PrintEasyError( "SubSurfaceScatteringShaderOp:: Not a single sample point could be generated" );
			}

			if( !ps->AddElements( points, maxDepth ) ) {
				GlobalLog()->PrintEasyError( "SubSurfaceScatteringShaderOp:: Fatal error while creating irradiance sample set" );
			}
			pointsets[ri.pObject] = ps;
		} else {
			// There is already a point set, so we can just do our approximation now
			ps = it->second;
		}
	}

	// Unsupported geometry was cached as a null sentinel above -> no SSS contribution
	// (c is already 0 from the top of the function).
	if( !ps ) return;

	// Pass 2: Evaluate the BSSRDF integral at the shading point.
	// The octree sums Rd(|xi - xo|) * E(xi) over all sample points,
	// using hierarchical approximation for distant clusters (Jensen 2002).
	ps->Evaluate( c, ri.geometric.ptIntersection, extinction, error, multiplyBSDF?ri.pMaterial->GetBSDF():0, ri.geometric );

	// Monte Carlo normalization: divide by N (the number of sample points).
	// Each sample's irradiance was pre-multiplied by irrad_scale, which
	// absorbs the area weight (dA = total_area / N) and the unit conversion
	// between the extinction function's internal units and scene-space.
	c = c * (1.0/Scalar(numPoints));

	// When multiplyBSDF is enabled, the octree evaluation already divided
	// by pi (via the Lambertian BSDF).  Multiply by pi to cancel that and
	// recover the correct BSSRDF integral.
	if( ri.pMaterial->GetBSDF() && multiplyBSDF ) {
		c = c*PI;
	}

	if( cache ) {
		// Add the result to the rasterizer state cache
		rc.StateCache_SetState( this, c, ri.pObject, ri.geometric.rast );
	}
}

//! Tells the shader to apply shade to the given intersection point for the given wavelength
/// \return Amplitude of spectral function 
Scalar SubSurfaceScatteringShaderOp::PerformOperationNM(
	const RuntimeContext& rc,					///< [in] Runtime context
	const RayIntersection& ri,					///< [in] Intersection information 
	const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	const Scalar caccum,						///< [in] Current value for wavelength
	const Scalar nm,							///< [in] Wavelength to shade
	const IORStack& ior_stack,			///< [in/out] Index of refraction stack
	const ScatteredRayContainer* pScat			///< [in] Scattering information
	) const
{
	// Spectral path: evaluate the full RGB BSSRDF (this reuses the
	// per-object irradiance octree -- built once and shared across all
	// wavelengths) and uplift the resulting RGB exitant radiance to
	// wavelength `nm` with the same chroma-preserving JH uplift the RGB
	// painters use for GetColorNM.  The prior `return 0` stub rendered
	// every SSS object BLACK under the *_spectral_* rasterizers
	// (pixelintegratingspectral / pathtracing_spectral / bdpt_spectral /
	// vcm_spectral / mlt_spectral).
	//
	// Result-uplift rather than a true per-wavelength transport because
	// both the diffusion profile (ISubSurfaceExtinctionFunction, RGB
	// ComputeTotalExtinction) and the cached irradiance are RGB-valued;
	// a per-lambda port would need a spectral extinction interface plus a
	// per-wavelength irradiance cache.  Uplifting the result makes the
	// spectral render reconstruct the RGB SSS appearance (the uplift
	// round-trips through the CMFs).  The diffusion *radius* is therefore
	// at RGB resolution, not per-lambda -- documented approximation; a
	// true spectral BSSRDF is future work.  RGBUnboundedSpectrum (not
	// Albedo): SSS exitant radiance is >= 0 and may exceed 1.
	RISEPel c;
	PerformOperation( rc, ri, caster, rs, c, ior_stack, pScat );
	return RGBUnboundedSpectrum::FromRGB( c ).Eval( nm );
}

void SubSurfaceScatteringShaderOp::ResetRuntimeData() const
{
	if( regenerate ) {
		PointSetMap::iterator i, e;
		for( i=pointsets.begin(), e=pointsets.end(); i!=e; i++ ) {
			delete i->second;
		}

		pointsets.clear();
	}
}
