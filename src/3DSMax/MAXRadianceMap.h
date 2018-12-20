//////////////////////////////////////////////////////////////////////
//
//  MAXRadianceMap.h - 3D Studio MAX radiance map, so we can get
//    MAX to do our background and radiance map shading for us
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 7, 2005
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef MAX_RADIANCE_MAP_H
#define MAX_RADIANCE_MAP_H

#include "maxincl.h"
#include "RISEIncludes.h"
#include "MAX2RISE_Helpers.h"
#include "scontext.h"

class RiseRenderer;
class RiseRendererParams;

class MAXRadianceMap : 
	public virtual RISE::IRadianceMap, 
	public virtual RISE::Implementation::Reference
{
protected:
	RiseRenderer* renderer;
	RiseRendererParams* rparams;
	mutable BGContext	bc;
	double	dScale;
	
public:
	MAXRadianceMap(
		RiseRenderer* renderer_,
		RiseRendererParams* rparams_,
		double scale
		) : 
	  renderer( renderer_ ),
	  rparams( rparams_ ),
	  bc(rparams),
	  dScale( scale )
	{
	}

	~MAXRadianceMap(){}

	//! Returns the radiance from that direction in the scene
	/// \return The radiance
	RISE::RISEPel GetRadiance( 
		const RISE::Ray& ray,
		const RISE::RASTERIZER_STATE& rast
		) const
	{
		Color bg;

		const Point3 viewDir = VectorTransform(renderer->view.affineTM, RISE2MAXVector(ray.dir));
		const Point3 viewCam = renderer->view.affineTM * RISE2MAXPoint(ray.origin);

		bc.SetViewDir( viewDir );
		bc.SetCamPos( viewCam );
		bc.SetScreenPos( rast.x, rast.y, rparams->devWidth, rparams->devHeight );

		if( rparams->envMap ) {
			AColor abg = rparams->envMap->EvalColor(bc);
			bg.r = abg.r;
			bg.g = abg.g;
			bg.b = abg.b;
		} else {
			if( rparams->pFrp ) {
				bg = rparams->pFrp->background;
			}
			else {
				bg = Color(0.0f, 0.0f, 0.0f);
			}
		}

		return RISE::RISEPel( bg.r, bg.g, bg.b ) * dScale;
	}

	//! Returns the radiance from that direction for the given wavelength
	RISE::Scalar GetRadianceNM(
		const RISE::Ray& ray,
		const RISE::RASTERIZER_STATE& rast,
		const RISE::Scalar nm
		) const
	{
		// Not Supported
		return 0;
	}


	///////////////////////////////////////////
	// Just Ignore this stuff
	///////////////////////////////////////////

	//! Sets the orientation of this map
	void SetOrientation( 
		const RISE::Vector3& orient			///< [in] Euler angles for the orientation
		)
	{}

	//! Sets the orientation of this map from the given matrix
	void SetTransformation( 
		const RISE::Matrix4& mx				///< [in] Transformation matrix for the map
		)
	{}

};

#endif