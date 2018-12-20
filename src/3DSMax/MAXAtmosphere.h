//////////////////////////////////////////////////////////////////////
//
//  MAXAtmosphere.h - 3D Studio MAX atmosphere, so we can get
//    MAX to do our atmospheric effects for us
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 8, 2005
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef MAX_ATMOPSHERE_H
#define MAX_ATMOPSHERE_H

#include "maxincl.h"
#include "RISEIncludes.h"
#include "MAX2RISE_Helpers.h"
#include "scontext.h"

class RiseRenderer;
class RiseRendererParams;

class MAXAtmosphere : 
	public virtual RISE::IAtmosphere, 
	public virtual RISE::Implementation::Reference
{
protected:
	RiseRenderer* renderer;
	RiseRendererParams* rparams;
	mutable BGContext	bc;
	
public:
	MAXAtmosphere(
		RiseRenderer* renderer_,
		RiseRendererParams* rparams_
		) : 
	  renderer( renderer_ ),
	  rparams( rparams_ ),
	  bc(rparams)
	{
	}

	~MAXAtmosphere(){}

	//! Returns the radiance from that direction in the scene
	/// \return The radiance
	RISE::RISEPel ApplyAtmospherics( 
		const RISE::Ray& ray,									///< [in] Ray
		const RISE::Point3& ptIntersec,							///< [in] Point of intersection to apply atmospherics from
		const RISE::RASTERIZER_STATE& rast,						///< [in] Rasterizer state
		const RISE::RISEPel& orig,								///< [in] Color before atmospherics are applied
		const bool bIsBackground								///< [in] Are we applying atmopsherics to the background?
		) const
	{
		if( rparams->atmos ) {
			Color bg = Color(orig[0], orig[1], orig[2]);

			const Point3 viewDir = VectorTransform(renderer->view.affineTM, RISE2MAXVector(ray.dir));
			const Point3 viewCam = renderer->view.affineTM * RISE2MAXPoint(ray.origin);

			bc.SetViewDir( viewDir );
			bc.SetCamPos( viewCam );
			bc.SetScreenPos( rast.x, rast.y, rparams->devWidth, rparams->devHeight );

			Color xp;
			if( bIsBackground ) {
				rparams->atmos->Shade(bc, viewCam, viewCam * TransMatrix(-FARZ * viewDir), bg, xp, TRUE);
			} else {
				const Point3 hitPos = renderer->view.affineTM * RISE2MAXPoint(ptIntersec);
				rparams->atmos->Shade(bc, viewCam, hitPos, bg, xp, FALSE);
			}

			return RISE::RISEPel( bg.r, bg.g, bg.b );
		}

		return RISE::RISEPel(0,0,0);
	}

	//! Returns the radiance from that direction for the given wavelength
	RISE::Scalar ApplyAtmosphericsNM(
		const RISE::Ray& ray,									///< [in] Ray
		const RISE::Point3& ptIntersec,							///< [in] Point of intersection to apply atmospherics from
		const RISE::RASTERIZER_STATE& rast,						///< [in] Rasterizer state
		const RISE::Scalar nm,									///< [in] Wavelength of light to process
		const RISE::Scalar orig,								///< [in] Value before atmospherics are applied
		const bool bIsBackground								///< [in] Are we applying atmopsherics to the background?
		) const
	{
		// Not Supported
		return 0;
	}

};

#endif