//////////////////////////////////////////////////////////////////////
//
//  MAXBSDF.h - 3D Studio MAX shaderop, that calls gets MAX
//    to do the computations for us
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 5, 2005
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MAX_BSDF_
#define MAX_BSDF_

#include "maxincl.h"
#include "RISEIncludes.h"
#include "MAX2RISE_Helpers.h"
#include "scontext.h"

class RiseRenderer;
class RiseRendererParams;

class MAXBSDF : 
	public virtual RISE::IBSDF, 
	public virtual RISE::Implementation::Reference,
	public virtual LightDesc
{
protected:
	RiseRenderer* renderer;
	RiseRendererParams* rparams;
	mutable BGContext	bc;
	mutable SContext	sc;

	mutable Point3 lightDir;

public:
	MAXBSDF(
		RiseRenderer* renderer_,
		RiseRendererParams* rparams_
		);

	~MAXBSDF();

	RISE::RISEPel value( const RISE::Vector3& vLightIn, const RISE::RayIntersectionGeometric& ri ) const;
	RISE::Scalar valueNM( const RISE::Vector3& vLightIn, const RISE::RayIntersectionGeometric& ri, const RISE::Scalar nm ) const{ return 0; } // unsupported

	// determine color and direction of illumination: return FALSE if light behind surface.
	// also computes dot product with normal dot_nl, and diffCoef which is to be used in
	// place of dot_n for diffuse light computations.
	BOOL Illuminate(ShadeContext& sc, Point3& normal, Color& color, Point3 &dir, float &dot_nl, float &diffuseCoef)
	{
//		diffuseCoef = 1.0;			// We return 1.0 because we know that R.I.S.E. will take this into account
		color = Color( 1, 1, 1 );
		dot_nl = DotProd(normal, lightDir);
		
		if( dot_nl > 0.0 ) {
			dir = lightDir;
		} else {
			dir = -lightDir;
			dot_nl = -dot_nl;
		}

		diffuseCoef = dot_nl;

		return TRUE;
	}

//	Point3 LightPosition() { return Point3(lightDir.x*100.0f,lightDir.y*100.0f,lightDir.z*100.0f); } 
};

#include "rise3dsmax.h"

#endif

