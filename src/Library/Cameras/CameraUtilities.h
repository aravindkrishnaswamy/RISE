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

		/// True if the camera's importance is a Dirac delta in DIRECTION
		/// (all rays parallel — orthographic).  Such a camera is the
		/// importance-side analogue of a directional light: the t==1
		/// light-tracing strategy (a non-specular light vertex scattering
		/// into the camera) has zero density, so BDPT/VCM must SKIP that
		/// strategy and exclude it from the MIS denominator — exactly the
		/// delta-direction-light treatment, applied on the camera side.
		/// Pinhole / thin-lens / fisheye are delta-POSITION (or finite)
		/// but finite-direction, so the t==1 connection is valid for them
		/// and this returns false.
		/// \return TRUE for orthographic cameras, FALSE otherwise
		bool IsDeltaDirection(
			const ICamera& cam						///< [in] Camera
			);
	}
}

#endif
