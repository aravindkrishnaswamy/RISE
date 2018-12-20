//////////////////////////////////////////////////////////////////////
//
//  MAXBSDF.cpp - Implementation of the MAXBSDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 5, 2005
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MAXBSDF.h"
#include "HitInfo.h"
#include "rendutil.h"

#pragma warning( disable : 4355	 )			// this used in initializer list

MAXBSDF::MAXBSDF(
	RiseRenderer* renderer_,
	RiseRendererParams* rparams_
	) : 
  renderer( renderer_ ),
  rparams( rparams_ ),
  bc(rparams),
  sc(renderer, &bc, this)
{

}

#pragma warning( default : 4355	 )

MAXBSDF::~MAXBSDF()
{
}

RISE::RISEPel MAXBSDF::value( const RISE::Vector3& vLightIn, const RISE::RayIntersectionGeometric& ri ) const
{
	HitInfo* hitInfo = dynamic_cast<HitInfo*>( ri.pCustom );
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

		const Point3 viewDir = VectorTransform(renderer->view.affineTM, RISE2MAXVector(ri.ray.dir));
		const Point3 viewCam = renderer->view.affineTM * RISE2MAXPoint(ri.ray.origin);

		lightDir = VectorTransform(renderer->view.affineTM, RISE2MAXVector(vLightIn));

		// Shade the face that was closest to the camera
		sc.SetViewDir( viewDir );
		sc.bc->SetViewDir( viewDir );
		sc.SetScreenPos(IPoint2(ri.rast.x, ri.rast.y));
		sc.SetCamPos( viewCam );
		sc.bc->SetCamPos( viewCam );
		sc.bc->SetScreenPos(ri.rast.x, ri.rast.y, rparams->devWidth, rparams->devHeight);
		sc.SetInstance(hitInfo->instance);
		sc.SetMtlNum(mid);
		sc.SetFaceNum(hitInfo->faceNum);
		sc.SetHitPos(renderer->view.affineTM * RISE2MAXPoint(ri.ptIntersection));
		sc.SetBary(hitInfo->baryCoord);
		sc.CalcNormals();

		// Go off and do the actual shading.
		hitInfo->instance->mtl->Shade(sc);	

		const Color result = sc.out.c;

		// We cannot handle transparency here unfortunately, Need to come up with a way to do this

		// The compensation factor is account for the diffuseCoef we told MAX and for the PI factor 
		const RISE::Scalar compensation_factor = 1.0 / (PI * RISE::Vector3Ops::Dot( vLightIn, ri.vNormal ));
		return RISE::RISEPel( result.r*compensation_factor, result.g*compensation_factor, result.b*compensation_factor );	
	}

	return RISE::RISEPel(0,0,0);
}


