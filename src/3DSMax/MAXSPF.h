//////////////////////////////////////////////////////////////////////
//
//  MAXSPF.h - 3D Studio MAX SPF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 5, 2005
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MAX_SPF_
#define MAX_SPF_

#include "maxincl.h"
#include "RISEIncludes.h"
#include "MAX2RISE_Helpers.h"
#include "scontext.h"
#include "MAXBSDF.h"

class MAXSPF : 
	public virtual RISE::ISPF, 
	public virtual RISE::Implementation::Reference
{
protected:
	MAXBSDF& pBSDF;

public:
	MAXSPF(
		MAXBSDF& pBSDF_
		) : 
	  pBSDF( pBSDF_ )
	{
		pBSDF.addref();
	}

    ~MAXSPF()
	{
		pBSDF.release();
	}

	void Scatter(
		const RISE::RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
		const RISE::DimensionalRandomNumberSequence& random,				///< [in] Random number generator
		RISE::ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
		const RISE::IORStack* const ior_stack								///< [in/out] Index of refraction stack
		) const
	{
		const RISE::Point2 ptrand( random.CanonicalRandom(0), random.CanonicalRandom(1) );

		RISE::ScatteredRay	diffuse;
		diffuse.type = RISE::ScatteredRay::eRayDiffuse;
	
		RISE::OrthonormalBasis3D	myonb = ri.onb;

		// Generate a reflected ray randomly with a cosine distribution
		if( RISE::Vector3Ops::Dot(ri.ray.dir, ri.onb.w()) > RISE::NEARZERO ) {
			myonb.FlipW();				
		}

		diffuse.ray.Set( ri.ptIntersection, RISE::GeometricUtilities::CreateDiffuseVector( myonb, ptrand ) );
		diffuse.kray = pBSDF.value( diffuse.ray.dir,  ri ) * PI;
//		diffuse.kray = RISE::RISEPel(1,1,1);
		scattered.AddScatteredRay( diffuse );
	}

	void ScatterNM( 
		const RISE::RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
		const RISE::DimensionalRandomNumberSequence& random,				///< [in] Random number generator
		const RISE::Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
		RISE::ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
		const RISE::IORStack* const ior_stack								///< [in/out] Index of refraction stack
		) const
	{
		// Unsupported
	}

};


#endif