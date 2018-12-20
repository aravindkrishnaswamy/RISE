//////////////////////////////////////////////////////////////////////
//
//  AreaLightShaderOp.cpp - Implementation of the AreaLightShaderOp class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 28, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "../RISE_API.h"
#include "AreaLightShaderOp.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

AreaLightShaderOp::AreaLightShaderOp( 
	const Scalar width_,			///< [in] Width of the light source
	const Scalar height_,			///< [in] Height of the light source
	const Point3 location_,			///< [in] Where is the light source located
	const Vector3 dir_,				///< [in] What is the light source focussed on
	const unsigned int samples,		///< [in] Number of samples to take
	const IPainter& emm_,			///< [in] Emission of this light
	const Scalar power_,			///< [in] Power scale
	const IPainter& N_,				///< [in] Phong factor for focussing the light on something
	const Scalar hotSpot_,			///< [in] Angle in radians of the light's hot spot
	const bool cache_				///< [in] Should we use the rasterizer state cache?
	) : 
  width( width_ ),
  height( height_ ),
  location( location_ ),
  dir( dir_ ),
  emm( emm_ ),
  power( power_ ),
  N( N_ ),
  hotSpot( hotSpot_ ),
  cache( cache_ ),
  area( width*height )
{
	RISE_API_CreateMultiJitteredSampling2D( &pSampler, width, height );
	pSampler->SetNumSamples( samples );

	OrthonormalBasis3D onb;
	onb.CreateFromW( dir );
	mxtransform = onb.GetCanonicalToBasisMatrix();

	emm.addref();
	N.addref();
}
AreaLightShaderOp::~AreaLightShaderOp( )
{
	safe_release( pSampler );
	emm.release();
	N.release();
}

//! Tells the shader to apply shade to the given intersection point
void AreaLightShaderOp::PerformOperation(
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

	// Only do stuff on a normal pass or on final gather
	if( rc.pass != RuntimeContext::PASS_NORMAL && rs.type == rs.eRayView ) {
		return;
	}

	const IBSDF* pBRDF = ri.pMaterial ? ri.pMaterial->GetBSDF() : 0;

	// Lets check our rasterizer state to see if we even need to do work!
	if( cache ) {
		if( !rc.StateCache_HasStateChanged( this, c, ri.pObject, ri.geometric.rast ) ) {
			// State hasn't changed, use the value already there
			return;
		}
	}

	ISampling2D::SamplesList2D samples;
	pSampler->GenerateSamplePoints( rc.random, samples );

	{
		ISampling2D::SamplesList2D::iterator m, n;
		for( m=samples.begin(), n=samples.end(); m!=n; m++ ) {
			*m = Point2Ops::mkPoint2(*m, Vector2( -width/2, -height/2 ));
		}
	}

	const RISEPel pN = N.GetColor( ri.geometric );

	for( ISampling2D::SamplesList2D::const_iterator it = samples.begin(); it != samples.end(); it++ ) {
		const Point2& sample = *it;

		// Construct a random sample in R^3
		const Point3 ptOnLight = Point3Ops::mkPoint3( location, Vector3Ops::Transform( mxtransform, Vector3( sample.x, 0, sample.y ) ) );

		// Now then we do the usual lighting test for this sample point
		Vector3				vToLight = Vector3Ops::mkVector3( ptOnLight, ri.geometric.ptIntersection );
		const Scalar		fDistFromLight = Vector3Ops::NormalizeMag(vToLight);	
		const Scalar		fDot = Vector3Ops::Dot( vToLight, ri.geometric.vNormal );

		const Vector3		vFromLight = -vToLight;
		const Scalar		fDotLight = Vector3Ops::Dot( vFromLight, dir );

		if( fDotLight < 0 ) {
			continue;
		}

		if( fDot < 0 ) {
			continue;
		}

		const Scalar fAngleOfIncidence = acos(fDot);

		if( fAngleOfIncidence <= hotSpot/2.0 ) {
			// Check to see if there is a shadow
			if( ri.pObject->DoesReceiveShadows() ) {
				const Ray		rayToLight( ri.geometric.ptIntersection, vToLight );
				if( caster.CastShadowRay( rayToLight, fDistFromLight ) ) {
					continue;
				}
			}		

			const RISEPel	k = (pN + 1) * pow(fDot,pN) * (1.0 / TWO_PI);
			const Scalar	attenuation_size_factor = area / (fDistFromLight * fDistFromLight);
			c = c + (emm.GetColor(ri.geometric) * k * power * fDotLight * attenuation_size_factor * (pBRDF?pBRDF->value(vToLight,ri.geometric):RISEPel(1,1,1)));
		}
	}

	c = c * (1.0/samples.size());

	// Add the result to the rasterizer state cache
	if( cache ) {
		rc.StateCache_SetState( this, c, ri.pObject, ri.geometric.rast );
	}
}

//! Tells the shader to apply shade to the given intersection point for the given wavelength
/// \return Amplitude of spectral function 
Scalar AreaLightShaderOp::PerformOperationNM(
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

	// Only do stuff on a normal pass or on final gather
	if( rc.pass != RuntimeContext::PASS_NORMAL && rs.type == rs.eRayView ) {
		return 0;
	}

	const IBSDF* pBRDF = ri.pMaterial ? ri.pMaterial->GetBSDF() : 0;

	ISampling2D::SamplesList2D samples;
	pSampler->GenerateSamplePoints( rc.random, samples );

	{
		ISampling2D::SamplesList2D::iterator m, n;
		for( m=samples.begin(), n=samples.end(); m!=n; m++ ) {
			*m = Point2Ops::mkPoint2(*m, Vector2( -width/2, -height/2 ));
		}
	}

	const Scalar pN = N.GetColorNM( ri.geometric, nm );

	for( ISampling2D::SamplesList2D::const_iterator it = samples.begin(); it != samples.end(); it++ ) {
		const Point2& sample = *it;

		// Construct a random sample in R^3
		const Point3 ptOnLight = Point3Ops::mkPoint3( location, Vector3Ops::Transform( mxtransform, Vector3( sample.x, 0, sample.y ) ) );

		// Now then we do the usual lighting test for this sample point
		Vector3	vToLight = Vector3Ops::mkVector3( ptOnLight, ri.geometric.ptIntersection );
		const Scalar		fDistFromLight = Vector3Ops::NormalizeMag(vToLight);	
		const Scalar		fDot = Vector3Ops::Dot( vToLight, ri.geometric.vNormal );

		const Vector3		vFromLight = -vToLight;
		const Scalar		fDotLight = Vector3Ops::Dot( vFromLight, dir );

		if( fDotLight < 0 ) {
			continue;
		}

		if( fDot < 0 ) {
			continue;
		}

		const Scalar fAngleOfIncidence = acos(fDot);

		if( fAngleOfIncidence <= hotSpot/2.0 ) {
			// Check to see if there is a shadow
			if( ri.pObject->DoesReceiveShadows() ) {
				const Ray		rayToLight( ri.geometric.ptIntersection, vToLight );
				if( !caster.CastShadowRay( rayToLight, fDistFromLight ) ) {
					const Scalar	k = (pN + 1) * pow(fDot,pN) * (1.0 / TWO_PI);
					const Scalar	attenuation_size_factor = area / (fDistFromLight * fDistFromLight);
					c += (emm.GetColorNM(ri.geometric,nm) * k * power * fDotLight * attenuation_size_factor * (pBRDF?pBRDF->valueNM(vToLight,ri.geometric,nm):1));
				}
			}
		}
	}

	c /= Scalar(samples.size());

	return c;
}
