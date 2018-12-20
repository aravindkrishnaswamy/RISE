//////////////////////////////////////////////////////////////////////
//
//  ICamera.h - Declaration of abstract camera class.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 31, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ICAMERA_
#define ICAMERA_

#include "IReference.h"
#include "IKeyframable.h"
#include "../Utilities/Ray.h"
#include "../Utilities/RuntimeContext.h"

namespace RISE
{
	//! A Camera is the viewer is located in the scene.  It also is what
	//! generates rays from a virtual screen
	class ICamera : 
		public virtual IReference,
		public virtual IKeyframable
	{
	protected:
		ICamera( ){};
		virtual ~ICamera( ){};

	public:
		//! Given two Scalars x and y in [0..1], generate a ray to fire
		/// \return TRUE if a ray exists, FALSE otherwise
		virtual bool GenerateRay( 
			const RuntimeContext& rc,					///< [in] Runtime context
			Ray& ray,									///< [in] The ray cast from point on screen
			const Point2& ptOnScreen					///< [in] Point on the virtual screen to generate for
			) const = 0;

		/// \return Point in space occupied by the camera
		virtual Point3 GetLocation( ) const = 0;

		/// \return Transformation matrix
		virtual Matrix4 GetMatrix( ) const = 0;

		/// \return The width of the frame of the camera
		virtual unsigned int GetWidth( ) const = 0;

		/// \return The height of the frame of the camera
		virtual unsigned int GetHeight( ) const = 0;

		/// \return The exposure time of the camera in some units (normalized unit time).  This is the same primary unit of animations
		virtual Scalar GetExposureTime( ) const = 0;

		/// \return The rate at which each scanline is 'recorded' by the camera in some units (normalized unit time/scanline)
		///         This is in the same primary units of animations
		///         If the scanning rate is infintely fast, returns 0
		virtual Scalar GetScanningRate( ) const = 0;

		/// \return The rate at which each pixel on a scanline is 'recorded' by the camera in some units (normalized unit time/pixel)
		///         This is in the same primary units of animations
		///         If the pixel rate is infintely fast, returns 0
		virtual Scalar GetPixelRate( ) const = 0;
	};
}

#endif
