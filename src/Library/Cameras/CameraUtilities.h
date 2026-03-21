//////////////////////////////////////////////////////////////////////
//
//  CameraUtilities.h - BDPT camera utility functions providing
//  inverse projection, importance, and PDF for all camera types.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CAMERA_UTILITIES_
#define CAMERA_UTILITIES_

#include "../Interfaces/ICamera.h"
#include "../Utilities/Ray.h"

namespace RISE
{
	namespace BDPTCameraUtilities
	{
		/// Maps a 3D world point to raster coordinates [0,width) x [0,height)
		/// \return FALSE if the point is behind the camera or outside the image
		bool Rasterize(
			const ICamera& cam,					///< [in] Camera to project through
			const Point3& worldPoint,				///< [in] 3D world point to project
			Point2& rasterPoint						///< [out] Resulting raster coordinates
			);

		/// Camera importance function We for a ray
		/// \return The importance value for this ray direction
		Scalar Importance(
			const ICamera& cam,					///< [in] Camera
			const Ray& ray							///< [in] Ray from the camera
			);

		/// PDF of generating this ray direction from the camera
		/// \return The probability density
		Scalar PdfDirection(
			const ICamera& cam,					///< [in] Camera
			const Ray& ray							///< [in] Ray from the camera
			);
	}
}

#endif
