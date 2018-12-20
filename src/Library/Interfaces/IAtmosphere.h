//////////////////////////////////////////////////////////////////////
//
//  IAtmosphere.h - Interface to an atmosphere which applies 
//    atmospheric effects (such as fog)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 8, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IATMOSHERE_H
#define IATMOSHERE_H

#include "IReference.h"
#include "../Intersection/RayIntersectionGeometric.h"			// For RasterizerState
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"

namespace RISE
{
	class IAtmosphere : public virtual IReference
	{
	protected:
		IAtmosphere(){};
		virtual ~IAtmosphere(){};

	public:

		//! Returns the radiance from that direction in the scene
		/// \return The radiance
		virtual RISEPel ApplyAtmospherics( 
			const Ray& ray,										///< [in] Ray
			const Point3& ptIntersec,							///< [in] Point of intersection to apply atmospherics from
			const RasterizerState& rast,						///< [in] Rasterizer state
			const RISEPel& orig,								///< [in] Color before atmospherics are applied
			const bool bIsBackground							///< [in] Are we applying atmopsherics to the background?
			) const = 0;

		//! Returns the radiance from that direction for the given wavelength
		virtual Scalar ApplyAtmosphericsNM(
			const Ray& ray,										///< [in] Ray
			const Point3& ptIntersec,							///< [in] Point of intersection to apply atmospherics from
			const RasterizerState& rast,						///< [in] Rasterizer state
			const Scalar nm,									///< [in] Wavelength of light to process
			const Scalar orig,									///< [in] Value before atmospherics are applied
			const bool bIsBackground							///< [in] Are we applying atmopsherics to the background?
			) const = 0;
	};
}

#endif

