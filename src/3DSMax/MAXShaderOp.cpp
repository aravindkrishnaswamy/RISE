//////////////////////////////////////////////////////////////////////
//
//  MAXShaderOp.cpp - Implementation of the MAXShaderOp
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 13, 2005
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MAXShaderOp.h"
#include "HitInfo.h"
#include "rendutil.h"

MAXShaderOp::MAXShaderOp(
	RiseRenderer* renderer_,
	RiseRendererParams* rparams_
	) : 
  renderer( renderer_ ),
  rparams( rparams_ ),
  bc(rparams),
  sc(renderer, &bc, 0)
{

}

MAXShaderOp::~MAXShaderOp()
{
}

//! Tells the shader to apply shade to the given intersection point
void MAXShaderOp::PerformOperation( 
	const RISE::RayIntersection& ri,					///< [in] Intersection information 
	const RISE::IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const RISE::IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	RISE::RISEPel& c,									///< [in/out] Resultant color from op
	const RISE::IORStack* const ior_stack,			///< [in/out] Index of refraction stack
	const RISE::ScatteredRayContainer* pScat,			///< [in] Scattering information
	const RISE::RandomNumberGenerator& random			///< [in] Random number generator
	) const
{
	HitInfo* hitInfo = dynamic_cast<HitInfo*>( ri.geometric.pCustom );
	if( hitInfo ) {
		// We have to first setup the shade context, then we just route this call to the MAX material
		// Shade point
		Face* f = &(hitInfo->instance->mesh->faces[hitInfo->faceNum]);
		MtlID mid = 0;
		int nNumSubs = hitInfo->instance->mtl->NumSubMtls();

		if (nNumSubs) {
			// Get sub material ID of the face.
			mid = f->getMatID();
			// the material ID of the face can be larger than the
			// total number of sub materials (because it is user
			// configurable).
			// Here I use a modulus function to bring the number
			// down to a legal value.
			mid = mid % nNumSubs;
		}

		const Point3 viewDir = VectorTransform(renderer->view.affineTM, RISE2MAXVector(ri.geometric.ray.dir));
		const Point3 viewCam = renderer->view.affineTM * RISE2MAXPoint(ri.geometric.ray.origin);

		// Shade the face that was closest to the camera
		sc.SetViewDir( viewDir );
		sc.bc->SetViewDir( viewDir );
		sc.SetScreenPos(IPoint2(ri.geometric.rast.x, ri.geometric.rast.y));
		sc.SetCamPos( viewCam );
		sc.bc->SetCamPos( viewCam );
		sc.bc->SetScreenPos(ri.geometric.rast.x, ri.geometric.rast.y, rparams->devWidth, rparams->devHeight);
		sc.SetInstance(hitInfo->instance);
		sc.SetMtlNum(mid);
		sc.SetFaceNum(hitInfo->faceNum);
		sc.SetHitPos(renderer->view.affineTM * RISE2MAXPoint(ri.geometric.ptIntersection));
		sc.SetBary(hitInfo->baryCoord);
		sc.CalcNormals();
		sc.pRayCaster = &caster;
		sc.affineTM = renderer->view.affineTM;
		sc.worldPtIntersection = RISE2MAXPoint(ri.geometric.ptIntersection);

		// Go off and do the actual shading.
		hitInfo->instance->mtl->Shade(sc);	

		Color result = sc.out.c;

		// Handle transparency
		if( sc.out.t.r > 0 || sc.out.t.g > 0 || sc.out.t.b > 0 ) {
			// There is transparency, shoot a continuing ray
			RISE::Ray ray = ri.geometric.ray;
			ray.Advance( ri.geometric.range+2e-8 );

			RISE::RISEPel cthis;
			if( caster.CastRay( ri.geometric.rast, ray, cthis, rs, 0, ri.pRadianceMap, ior_stack ) ) {
				// Blend
				result.r = static_cast<float>(cthis[0]*sc.out.t.r + result.r);
				result.g = static_cast<float>(cthis[1]*sc.out.t.g + result.g);
				result.b = static_cast<float>(cthis[2]*sc.out.t.b + result.b);
			}			
		}

		c = RISE::RISEPel( result.r, result.g, result.b );	

		sc.pRayCaster = 0;
	}
}

//! Tells the shader to apply shade to the given intersection point for the given wavelength
/// \return Amplitude of spectral function 
RISE::Scalar MAXShaderOp::PerformOperationNM( 
	const RISE::RayIntersection& ri,					///< [in] Intersection information 
	const RISE::IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
	const RISE::IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
	const RISE::Scalar caccum,						///< [in] Current value for wavelength
	const RISE::Scalar nm,							///< [in] Wavelength to shade
	const RISE::IORStack* const ior_stack,			///< [in/out] Index of refraction stack
	const RISE::ScatteredRayContainer* pScat,			///< [in] Scattering information
	const RISE::RandomNumberGenerator& random			///< [in] Random number generator
	) const
{
	// Do nothing, MAX can't do spectral processing
	return 0;
}