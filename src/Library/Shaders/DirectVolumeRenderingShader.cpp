//////////////////////////////////////////////////////////////////////
//
//  DirectVolumeRenderingShader.cpp - Implementation of the DirectVolumeRenderingShader
//    class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 9, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "DirectVolumeRenderingShader.h"
#include "../Interfaces/ILog.h"
#include "../Volume/Volume.h"
#include "../Volume/VolumeAccessor_NNB.h"
#include "../Volume/VolumeAccessor_TRI.h"
#include "../Volume/VolumeAccessor_TriCubic.h"
#include "../Volume/VolumeOp_Average.h"
#include "../Volume/VolumeOp_Composite.h"
#include "../Volume/VolumeOp_AlphaScaledComposite.h"
#include "../Volume/VolumeOp_GradISO.h"
#include "../Volume/VolumeOp_ISO.h"
#include "../Volume/VolumeOp_LMIP.h"
#include "../Volume/VolumeOp_MIP.h"
#include "../Volume/CentralDifferenceOperator.h"
#include "../Volume/IntermediateDifferenceOperator.h"
#include "../Volume/ZuckerHummelOperator.h"
#include "../Volume/SobelOperator.h"
#include "../Utilities/CubicInterpolator.h"

using namespace RISE;
using namespace RISE::Implementation;

inline IVolumeAccessor* VolumeAccessorFromChar( const char accessor )
{
	switch( accessor ) {
		default:
			GlobalLog()->PrintEasyWarning( "DirectVolumeRenderingShader:: Invalid accessor specified, defaulting to nearest neighbour" );
		case 'n':
			return new VolumeAccessor_NNB();
		case 't':
			return new VolumeAccessor_TRI();
		case 'c':
			{
				ICubicInterpolator<Scalar>* interp = new CatmullRomCubicInterpolator<Scalar>();
				IVolumeAccessor* pRet = new VolumeAccessor_TriCubic( *interp );
				interp->release();
				return pRet;
			}
		case 'b':
			{
				ICubicInterpolator<Scalar>* interp = new UniformBSplineCubicInterpolator<Scalar>();
				IVolumeAccessor* pRet = new VolumeAccessor_TriCubic( *interp );
				interp->release();
				return pRet;
			}
	}
}

inline IGradientEstimator* GradientEstimatorFromChar( const char gradient )
{
	switch( gradient ) {
		case 'c':
			return new CentralDifferenceOperator();
		case 'i':
			return new IntermediateDifferenceOperator();
		case 'z':
			return new SobelOperator();
		case 's':
		default:
			return new ZuckerHummelOperator();
	}
}

inline IVolumeOperation* VolumeOperatorFromChar( 
	const char composite, 
	const Scalar dThresholdStart, 
	const Scalar dThresholdEnd,
	const IGradientEstimator* pGradientEstimator
	)
{
	switch( composite ) {
		case 'a':
			return new VolumeOp_Average();
		case 'c':
			return new VolumeOp_Composite();
		case 's':
		default:
			return new VolumeOp_AlphaScaledComposite();
		case 'i':
			return new VolumeOp_ISO(dThresholdStart, dThresholdEnd);
		case 'g':
			return new VolumeOp_GradISO(dThresholdStart, dThresholdEnd, pGradientEstimator);
		case 'm':
			return new VolumeOp_MIP();
	}
}

DirectVolumeRenderingShader::DirectVolumeRenderingShader(
	const char* szVolumeFilePattern,
	const unsigned int width,
	const unsigned int height, 
	const unsigned int startz,
	const unsigned int endz,
	const char accessor,
	const char gradient,
	const char composite,
	const Scalar dThresholdStart,
	const Scalar dThresholdEnd,
	ISampling1D& sampler,
	const IFunction1D& red,
	const IFunction1D& green,
	const IFunction1D& blue,
	const IFunction1D& alpha,
	const IShader* pISOShader_
	) : 
  pVolume( 0 ),
  pSampler( sampler ),
  pGradientEstimator( 0 ),
  pComposite( 0 ),
  pTransferFunctions( 0 ),
  pSpectralTransferFunctions( 0 ),
  pISOShader( pISOShader_ )
{
	// Create all the pertinent classes, start by loading the volume
	Volume<unsigned char>* v = new Volume<unsigned char>( szVolumeFilePattern, width, height, startz, endz );

	pSampler.addref();

	// Bind to the volume accessor
	pVolume = VolumeAccessorFromChar( accessor );
	pVolume->BindVolume( v );
	safe_release( v );

	// Create the gradient estimator
	pGradientEstimator = GradientEstimatorFromChar( gradient );

	// Create the volume operator
	pComposite = VolumeOperatorFromChar( composite, dThresholdStart, dThresholdEnd, pGradientEstimator );

	// Setup the transfer functions 
	pTransferFunctions = new TransferFunctions( red, green, blue, alpha );

	if( pISOShader ) {
		pISOShader->addref();
	}
}

DirectVolumeRenderingShader::DirectVolumeRenderingShader(
	const char* szVolumeFilePattern,
	const unsigned int width,
	const unsigned int height, 
	const unsigned int startz,
	const unsigned int endz,
	const char accessor,
	const char gradient,
	const char composite,
	const Scalar dThresholdStart,
	const Scalar dThresholdEnd,
	ISampling1D& sampler,
	const IFunction1D& alpha,
	const IFunction2D& spectral,
	const IShader* pISOShader_
	) : 
  pVolume( 0 ),
  pSampler( sampler ),
  pGradientEstimator( 0 ),
  pComposite( 0 ),
  pTransferFunctions( 0 ),
  pSpectralTransferFunctions( 0 ),
  pISOShader( pISOShader_ )
{
	// Create all the pertinent classes, start by loading the volume
	Volume<unsigned char>* v = new Volume<unsigned char>( szVolumeFilePattern, width, height, startz, endz );

	pSampler.addref();

	// Bind to the volume accessor
	pVolume = VolumeAccessorFromChar( accessor );
	pVolume->BindVolume( v );
	safe_release( v );

	// Create the gradient estimator
	pGradientEstimator = GradientEstimatorFromChar( gradient );

	// Create the volume operator
	pComposite = VolumeOperatorFromChar( composite, dThresholdStart, dThresholdEnd, pGradientEstimator );

	// Setup the transfer functions 
	pSpectralTransferFunctions = new SpectralTransferFunctions( alpha, spectral );

	if( pISOShader ) {
		pISOShader->addref();
	}
}

DirectVolumeRenderingShader::~DirectVolumeRenderingShader( )
{
	safe_release( pComposite );
	safe_release( pVolume );
	safe_release( pGradientEstimator );
	safe_delete( pTransferFunctions );
	safe_delete( pSpectralTransferFunctions );
	safe_release( pISOShader );

	pSampler.release();
}

//! Tells the shader to apply shade to the given intersection point
void DirectVolumeRenderingShader::Shade(
	const RuntimeContext& rc,					///< [in] The runtime context
	const RayIntersection& ri,					///< [in] Intersection information 
	const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	RISEPel& c,									///< [out] RISEPel value at the point
	const IORStack* const ior_stack				///< [in/out] Index of refraction stack
	) const
{
	const IScene* pScene = caster.GetAttachedScene();
	if( !pScene ) {
		return;
	}

	// Only do stuff on a normal pass or on final gather
	if( rc.pass != RuntimeContext::PASS_NORMAL && rs.type == rs.eRayView ) {
		return;
	}

	// First we see if are entering or leaving the volume of interest
	// If we are entering the volume, defer the shader to the leaving portion
	// If we are leaving, then actually do the shading

	// We tell if we are entering or leaving by the ray's orientation to the surface normal
	const Scalar cosine = -Vector3Ops::Dot(ri.geometric.vNormal, ri.geometric.ray.dir);
	if( cosine < NEARZERO ) {
		// We are coming from the inside of the object, hence we are leaving
		// We need to do the volume shading now

		// The start point is where the ray started, the end point is this intersection
		// If the user was smart they used a box as the geometry cuz thats basically what we need

		// Always regenerate sample points
		ISampling1D::SamplesList1D samples;
		pSampler.GenerateSamplePoints(rc.random, samples);

		const Point3 ptStart = Point3Ops::Transform( ri.pObject->GetFinalInverseTransformMatrix(), ri.geometric.ray.origin );
		const Vector3 vDir = Vector3Ops::mkVector3( ri.geometric.ptObjIntersec, ptStart );

		pComposite->BeginOperation( samples.size(), pVolume );

		RISEColor	cPel;

		ISampling1D::SamplesList1D::const_iterator i, e;
		for( i=samples.begin(), e=samples.end(); i!=e; i++ ) {
			const Point3 voxel = Point3Ops::mkPoint3( ptStart, vDir * (*i) );
			const Scalar voxel_value = pVolume->GetValue( voxel.x, voxel.y, voxel.z );

			// Apply intensity / compositing
			IVolumeOperation::CompOpRet compret = pComposite->ApplyComposite( 
				cPel, 
				voxel_value,
				pTransferFunctions,
				voxel
				);

			if( compret == IVolumeOperation::StopApplyLighting )
			{
				// Stop and apply lighting means that the compositor is finished 
				// and that the value it should use is in cPel and we should apply lighting
				// as if this were the exact target voxel
				if( pISOShader ) {
					// If there an ISO shader assigned, defer shading to it in the case of
					// lighting
					
					// We can use the normal of the ISO surface, however the intersection point must
					// be outside the box so that we don't hit this object again.
					// The way to do that is to trace a ray from the surface along the normal until
					// we exit this object
					
					const Vector3 iso_normal = -pGradientEstimator->ComputeGradient( pVolume, voxel ).vNormal;
					RayIntersection myri = ri;

					myri.geometric.ptIntersection = Point3Ops::Transform(ri.pObject->GetFinalTransformMatrix(),Point3Ops::mkPoint3( ptStart, vDir * ((*i)*1e6) ));
					myri.geometric.ptObjIntersec = voxel;
					myri.geometric.vNormal = Vector3Ops::Normalize(Vector3Ops::Transform(ri.pObject->GetFinalTransformMatrix(),iso_normal));
					myri.geometric.onb.CreateFromW( myri.geometric.vNormal );

					pISOShader->Shade( rc, myri, caster, rs, c, ior_stack );
					return;
				} else if( ri.pMaterial ) {
					const IBSDF* pBRDF = ri.pMaterial->GetBSDF();

					if( pBRDF ) {
						RayIntersection myri = ri;
						myri.geometric.ptIntersection = Point3Ops::Transform(ri.pObject->GetFinalTransformMatrix(),Point3Ops::mkPoint3( ptStart, vDir * ((*i)*1e6) ));
						myri.geometric.vNormal = Vector3Ops::Normalize(Vector3Ops::Transform(ri.pObject->GetFinalTransformMatrix(),-pGradientEstimator->ComputeGradient( pVolume, voxel ).vNormal));
						myri.geometric.onb.CreateFromW( myri.geometric.vNormal );

						RISEPel diffuse;

						// Account for lighting from non mesh based lights
						const ILightManager* pLM = pScene->GetLights();
						if( pLM ) {
							pLM->ComputeDirectLighting( myri.geometric, caster, *pBRDF, ri.pObject->DoesReceiveShadows(), diffuse );
						}

						const ILuminaryManager* pLumManager = caster.GetLuminaries();
						// Account for lighting from luminaries
						if( pLumManager ) {
							diffuse = diffuse + pLumManager->ComputeDirectLighting( myri, *pBRDF, rc.random, caster, pScene->GetShadowMap() );			
						}

						cPel.base = cPel.base * diffuse;
					}
				}

				break;
			}
		}

		if( pComposite->doTransferFunctionAtEnd() ){
			cPel = pTransferFunctions->ComputeColorFromIntensity( pComposite->getSelectedVoxelValue() );
		}

		pComposite->EndOperation( );

		if( cPel.a == 0 ) {
			Ray r( ri.geometric.ptIntersection,ri.geometric.ray.dir );
			r.Advance( 1e-6 );
			caster.CastRay( rc, ri.geometric.rast, r, c, rs, 0, ri.pRadianceMap, ior_stack );
		} else if( cPel.a == 1.0 ) {
			c = cPel.base;
		} else {
			// Still some transparancy, continue the ray
			Ray r( ri.geometric.ptIntersection,ri.geometric.ray.dir );
			r.Advance( 1e-6 );
			caster.CastRay( rc, ri.geometric.rast, r, c, rs, 0, ri.pRadianceMap, ior_stack );
            c = (c*(1.0-cPel.a)) + (cPel.base*cPel.a);
		}
	} else {
		// Defer the shading to the other side
		Ray r( ri.geometric.ptIntersection,ri.geometric.ray.dir );
		r.Advance( 1e-6 );
		caster.CastRay( rc, ri.geometric.rast, r, c, rs, 0, ri.pRadianceMap, ior_stack );
	}
}

//! Tells the shader to apply shade to the given intersection point for the given wavelength
/// \return Amplitude of spectral function 
Scalar DirectVolumeRenderingShader::ShadeNM(
	const RuntimeContext& rc,					///< [in] The runtime context
	const RayIntersection& ri,					///< [in] Intersection information 
	const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	const Scalar nm,							///< [in] Wavelength to shade
	const IORStack* const ior_stack				///< [in/out] Index of refraction stack
	) const
{
	const IScene* pScene = caster.GetAttachedScene();
	if( !pScene ) {
		return 0;
	}

	// Only do stuff on a normal pass or on final gather
	if( rc.pass != RuntimeContext::PASS_NORMAL && rs.type == rs.eRayView ) {
		return 0;
	}

	Scalar c = 0;

	// First we see if are entering or leaving the volume of interest
	// If we are entering the volume, defer the shader to the leaving portion
	// If we are leaving, then actually do the shading

	// We tell if we are entering or leaving by the ray's orientation to the surface normal
	const Scalar cosine = -Vector3Ops::Dot(ri.geometric.vNormal, ri.geometric.ray.dir);
	if( cosine < NEARZERO ) {
		// We are coming from the inside of the object, hence we are leaving
		// We need to do the volume shading now

		// The start point is where the ray started, the end point is this intersection
		// If the user was smart they used a box as the geometry cuz thats basically what we need

		// Always regenerate sample points
		ISampling1D::SamplesList1D samples;
		pSampler.GenerateSamplePoints(rc.random, samples);

		const Point3 ptStart = Point3Ops::Transform( ri.pObject->GetFinalInverseTransformMatrix(), ri.geometric.ray.origin );
		const Vector3 vDir = Vector3Ops::mkVector3( ri.geometric.ptObjIntersec, ptStart );

		pComposite->BeginOperation( samples.size(), pVolume );

		IVolumeOperation::SpectralColor	cPel;

		ISampling1D::SamplesList1D::const_iterator i, e;
		for( i=samples.begin(), e=samples.end(); i!=e; i++ ) {
			const Point3 voxel = Point3Ops::mkPoint3( ptStart, vDir * (*i) );
			const Scalar voxel_value = pVolume->GetValue( voxel.x, voxel.y, voxel.z );

			// Apply intensity / compositing
			IVolumeOperation::CompOpRet compret = pComposite->ApplyCompositeNM( 
				cPel, 
				nm,
				voxel_value,
				pSpectralTransferFunctions,
				voxel
				);

			if( compret == IVolumeOperation::StopApplyLighting )
			{
				// Stop and apply lighting means that the compositor is finished 
				// and that the value it should use is in cPel and we should apply lighting
				// as if this were the exact target voxel
				if( pISOShader ) {
					// If there an ISO shader assigned, defer shading to it in the case of
					// lighting
					
					// We can use the normal of the ISO surface, however the intersection point must
					// be outside the box so that we don't hit this object again.
					// The way to do that is to trace a ray from the surface along the normal until
					// we exit this object
					
					const Vector3 iso_normal = -pGradientEstimator->ComputeGradient( pVolume, voxel ).vNormal;
					RayIntersection myri = ri;

					myri.geometric.ptIntersection = Point3Ops::Transform(ri.pObject->GetFinalTransformMatrix(),Point3Ops::mkPoint3( ptStart, vDir * ((*i)*1e6) ));
					myri.geometric.ptObjIntersec = voxel;
					myri.geometric.vNormal = Vector3Ops::Normalize(Vector3Ops::Transform(ri.pObject->GetFinalTransformMatrix(),iso_normal));

					return pISOShader->ShadeNM( rc, myri, caster, rs, nm, ior_stack );
				} else if( ri.pMaterial ) {
					const IBSDF* pBRDF = ri.pMaterial->GetBSDF();

					if( pBRDF ) {
						RayIntersection myri = ri;
						myri.geometric.ptIntersection = Point3Ops::Transform(ri.pObject->GetFinalTransformMatrix(),Point3Ops::mkPoint3( ptStart, vDir * ((*i)*1e6) ));
						myri.geometric.vNormal = Vector3Ops::Normalize(Vector3Ops::Transform(ri.pObject->GetFinalTransformMatrix(),-pGradientEstimator->ComputeGradient( pVolume, voxel ).vNormal));
						myri.geometric.onb.CreateFromW( myri.geometric.vNormal );

						const ILuminaryManager* pLumManager = caster.GetLuminaries();
						// Account for lighting from luminaries
						if( pLumManager ) {
							cPel.second *= pLumManager->ComputeDirectLightingNM( myri, *pBRDF, nm, rc.random, caster, pScene->GetShadowMap() );
						}						
					}
				}

				break;
			}
		}

		if( pComposite->doTransferFunctionAtEnd() ){
			const Scalar voxel_value = pComposite->getSelectedVoxelValue();
			cPel.first = pSpectralTransferFunctions->ComputeAlphaFromIntensity( voxel_value );
			cPel.second = pSpectralTransferFunctions->ComputeSpectralIntensityFromVolumeIntensity( nm, voxel_value );
		}

		pComposite->EndOperation( );

		if( cPel.first == 0 ) {
			Ray r( ri.geometric.ptIntersection,ri.geometric.ray.dir );
			r.Advance( 1e-6 );
			caster.CastRayNM( rc, ri.geometric.rast, r, c, rs, nm, 0, ri.pRadianceMap, ior_stack );
		} else if( cPel.first == 1.0 ) {
			c = cPel.second;
		} else {
			// Still some transparancy, continue the ray
			Ray r( ri.geometric.ptIntersection,ri.geometric.ray.dir );
			r.Advance( 1e-6 );
			caster.CastRayNM( rc, ri.geometric.rast, r, c, rs, nm, 0, ri.pRadianceMap, ior_stack );
            c = (c*(1.0-cPel.first)) + (cPel.second*cPel.first);
		}
	} else {
		// Defer the shading to the other side
		Ray r( ri.geometric.ptIntersection,ri.geometric.ray.dir );
		r.Advance( 1e-6 );
		caster.CastRayNM( rc, ri.geometric.rast, r, c, rs, nm, 0, ri.pRadianceMap, ior_stack );
	}

	return c;
}

//! Tells the shader to reset itself
void DirectVolumeRenderingShader::ResetRuntimeData() const
{
	if( pISOShader ) {
		pISOShader->ResetRuntimeData();
	}
}

