//////////////////////////////////////////////////////////////////////
//
//  SubSurfaceScatteringShaderOp.cpp - Implementation of the SubSurfaceScatteringShaderOp class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 18, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SubSurfaceScatteringShaderOp.h"
#include "../../Utilities/GeometricUtilities.h"
#include "../../Utilities/stl_utils.h"
#include "../../Sampling/HaltonPoints.h"

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
	const IORStack* const ior_stack,			///< [in/out] Index of refraction stack
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
	if( rc.pass != RuntimeContext::PASS_NORMAL && rs.type == rs.eRayView ) {
		return;
	}

	// Lets check our rasterizer state to see if we even need to do work!
	if( cache ) {
		if( !rc.StateCache_HasStateChanged( this, c, ri.pObject, ri.geometric.rast ) ) {
			// State hasn't changed, use the value already there
			return;
		}
	}

	// Lets check our internal map and see if we have already setup a point set for this object
	PointSetMap::iterator it = pointsets.find( ri.pObject );

	PointSetOctree* ps = 0;

	if( it == pointsets.end() ) {

		// Grab the mutex, we should only create this once per object
		create_mutex.lock();

		// Once grabbed, check again, just in case someone else made one
		PointSetMap::iterator again = pointsets.find( ri.pObject );
		if( again == pointsets.end() ) {
			// There is no point set ready, so we need to create one
			GlobalLog()->PrintEasyInfo( "SubSurfaceScatteringShaderOp:: Generating point sample set for object" );
			
			PointSetOctree::PointSet points;
			BoundingBox bbox( Point3(INFINITY,INFINITY,INFINITY), Point3(-INFINITY,-INFINITY,-INFINITY) );

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
	//				sp.irrad = sp.irrad * sample_area;			// remember we are integrating over surface area
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
			// Some other thread just set it
			ps = again->second;
		}

		create_mutex.unlock();

	} else {
		// There is already a point set, so we can just do our approximation now
		ps = it->second;
	}

    // We have a list of points that are part of our sample, 
	// we must query all the points that are within one path length of us and evaluate how much light
	// gets here from there
	ps->Evaluate( c, ri.geometric.ptIntersection, extinction, error, multiplyBSDF?ri.pMaterial->GetBSDF():0, ri.geometric );

	// Account for the integration over area
//	c = c * (1.0/Scalar(ri.pObject->GetArea()));
	// Since we know each point samples the area of the surface equally, we can just do this to save time
	c = c * (1.0/Scalar(numPoints));

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
	const IORStack* const ior_stack,			///< [in/out] Index of refraction stack
	const ScatteredRayContainer* pScat			///< [in] Scattering information
	) const
{
	Scalar c=0;

	//! \TODO To be implemented

	return c;
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
