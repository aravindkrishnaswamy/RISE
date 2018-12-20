//////////////////////////////////////////////////////////////////////
//
//  IRadianceMap.h - Interface to a radiance map which basically 
//    light probe data for doing image based lighting
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 18, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IRADIANCE_MAP_
#define IRADIANCE_MAP_

#include "IReference.h"
#include "../Intersection/RayIntersectionGeometric.h"			// For RasterizerState
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"

namespace RISE
{
	class IRadianceMap : public virtual IReference
	{
	protected:
		IRadianceMap(){};
		virtual ~IRadianceMap(){};

	public:

		//! Returns the radiance from that direction in the scene
		/// \return The radiance
		virtual RISEPel GetRadiance( 
			const Ray& ray,
			const RasterizerState& rast
			) const = 0;

		//! Returns the radiance from that direction for the given wavelength
		virtual Scalar GetRadianceNM(
			const Ray& ray,
			const RasterizerState& rast,
			const Scalar nm
			) const = 0;

		//! Sets the orientation of this map
		virtual void SetOrientation( 
			const Vector3& orient			///< [in] Euler angles for the orientation
			) = 0;

		//! Sets the orientation of this map from the given matrix
		virtual void SetTransformation( 
			const Matrix4& mx				///< [in] Transformation matrix for the map
			) = 0;
	};
}

#endif

