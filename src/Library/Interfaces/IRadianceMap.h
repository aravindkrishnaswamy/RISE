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
	class IPainter;

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

		//! Returns the painter used by this radiance map
		virtual const IPainter& GetPainter() const = 0;

		//! Returns the radiance scale factor
		virtual Scalar GetScale() const = 0;

		//! Returns the world-to-map transformation matrix
		virtual const Matrix4& GetTransform() const = 0;

		//! Overrides the radiance scale factor at runtime.  Used by
		//! Job::SetActiveRasterizerRadianceScale to push a `> modify
		//! rasterizer radiance_scale` override into the background/miss
		//! radiance lookup so it stays consistent with the environment
		//! importance sampler.  Default is a no-op: a map type that
		//! treats scale as immutable (or has no meaningful runtime scale)
		//! simply ignores the override.  Appended at the END of the
		//! interface with a body (not pure) so existing implementers —
		//! including out-of-tree ones — are not forced to add it.
		virtual void SetScale( const Scalar scale ) {}
	};
}

#endif

