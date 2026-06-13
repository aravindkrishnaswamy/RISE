//////////////////////////////////////////////////////////////////////
//
//  RISE_API.cpp - Implements the RISE API
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifdef WIN32
#pragma warning( disable : 4512 )		// disables warning about not being able to generate an assignment operator (.NET 2003)
#pragma warning( disable : 4250 )		// disables silly virtual inheritance warning
#pragma warning( disable : 4344 )		// disables warning about explicit template argument passed to template function
#pragma warning( disable : 4290 )		// disables warning about C++ exception definition being ignored
#endif

#define _USE_MATH_DEFINES
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "Utilities/stl_utils.h"
#include "Interfaces/ILog.h"

//////////////////////////////////////////////////////////
// Library versioning information
//////////////////////////////////////////////////////////

#include "Version.h"

namespace RISE
{
	//! Queries the version of the library
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_GetVersion(
		int* pMajorVersion,									///< [out] Pointer to recieve the major version
		int* pMinorVersion,									///< [out] Pointer to recieve the minor version
		int* pRevision,										///< [out] Pointer to recieve the revision number
		int* pBuildNumber,									///< [out] Pointer to recieve the build numbers
		bool* pDebug										///< [out] Pointer to bool to recieve whether this is a debug build
		)
	{
		if( pMajorVersion ) {
			(*pMajorVersion) = RISE_VER_MAJOR_VERSION;
		}

		if( pMinorVersion ) {
			(*pMinorVersion) = RISE_VER_MINOR_VERSION;
		}

		if( pRevision ) {
			(*pRevision) = RISE_VER_REVISION_VERSION;
		}

		if( pBuildNumber ) {
			(*pBuildNumber) = RISE_VER_BUILD_VERSION;
		}

		if( pDebug ) {
	#ifdef _DEBUG
			*pDebug = true;
	#else
			*pDebug = false;
	#endif
		}

		return true;
	}
}

#include <string.h>										// for strncpy
#include <cstdio>										// for the SDF parts-file read in RISE_API_CreateSDFGeometry
#include <string>										// parts-file buffer in RISE_API_CreateSDFGeometry
#include <vector>

namespace RISE
{
	//! Queries the date the library was built
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_GetBuildDate(
		char* szDate,										///< [out] Pointer to string to recieve date
		unsigned int dateStrMax								///< [in] Maximum characters to store in date string
		)
	{
		strncpy( szDate, __DATE__, dateStrMax );
		return true;
	}

	//! Queries the time the library was built
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_GetBuildTime(
		char* szTime,										///< [out] Pointer to string to recieve time
		unsigned int timeStrMax								///< [in] Maximum characters to store in time string
		)
	{
		strncpy( szTime, __TIME__, timeStrMax );
		return true;
	}

	//! Queries for any copyright information
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_GetCopyrightInformation(
		char* szCopyright,									///< [out] Pointer to string to recieve copyright info
		unsigned int copyrightStrMax						///< [in] Maximum characters to store in info string
		)
	{
		static const char* copyright = "Realistic Image Synthesis Engine (R.I.S.E.)\n (c) 2001-2006 Aravind Krishnaswamy. All Rights Reserved.\n  Please see attached LICENSE.TXT";
		strncpy( szCopyright, copyright, copyrightStrMax );
		return true;
	}

	//! Queries any special build information
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_GetBuildSpecialInfo(
		char* szInfo,										///< [out] Pointer to string to recieve special info
		unsigned int infoStrMax								///< [in] Maximum characters to store in info string
		)
	{
		strncpy( szInfo, "INTERNAL NON PUBLIC BUILD - DO NOT DISTRIBUTE", infoStrMax );
		return true;
	}
}

//////////////////////////////////////////////////////////
// Defines camera creation
//////////////////////////////////////////////////////////

#include "Cameras/PinholeCamera.h"
#include "Cameras/ThinLensCamera.h"
#include "Cameras/FisheyeCamera.h"
#include "Cameras/OrthographicCamera.h"

using namespace RISE::Implementation;

namespace RISE
{
	//! Creates a traditional pinhole camera
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePinholeCamera(
		ICamera** ppi,											///< [out] Pointer to recieve the camera
		const Point3& ptLocation,								///< [in] Absolute location of where the camera is located
		const Point3& ptLookAt, 								///< [in] Absolute point the camera is looking at
		const Vector3& vUp,										///< [in] Up vector of the camera
		const Scalar fov,										///< [in] Field of view in radians
		const unsigned int xres,								///< [in] X resolution of virtual screen
		const unsigned int yres,								///< [in] Y resolution of virtual screen
		const Scalar pixelAR,									///< [in] Pixel aspect ratio
		const Scalar exposure,									///< [in] Exposure time of the camera
		const Scalar scanningRate,								///< [in] Rate at which each scanline is recorded
		const Scalar pixelRate,									///< [in] Rate at which each pixel is recorded
		const Vector3& orientation,								///< [in] Orientation (Pitch,Roll,Yaw)
		const Vector2& target_orientation,						///< [in] Orientation relative to a target
		const Scalar iso,										///< [in] Landing 5: ISO (0 = disabled)
		const Scalar fstop										///< [in] Landing 5: f-number for EV
		)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new PinholeCamera( ptLocation, ptLookAt, vUp, fov, xres, yres, pixelAR, exposure, scanningRate, pixelRate, orientation, target_orientation, iso, fstop );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "pinhole camera" );
		return true;
	}

	//! Creates a traditional pinhole camera
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePinholeCameraONB(
		ICamera** ppi,											///< [out] Pointer to recieve the camera
		const OrthonormalBasis3D& onb,							///< [in] Orthonormal basis that describes the camera's orientation
		const Point3& ptLocation,								///< [in] Absolute location of where the camera is located
		const Scalar fov,										///< [in] Field of view in radians
		const unsigned int xres,								///< [in] X resolution of virtual screen
		const unsigned int yres,								///< [in] Y resolution of virtual screen
		const Scalar pixelAR,									///< [in] Pixel aspect ratio
		const Scalar exposure,									///< [in] Exposure time of the camera
		const Scalar scanningRate,								///< [in] Rate at which each scanline is recorded
		const Scalar pixelRate,									///< [in] Rate at which each pixel is recorded
		const Scalar iso,										///< [in] Landing 5: ISO (0 = disabled)
		const Scalar fstop										///< [in] Landing 5: f-number for EV
		)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new PinholeCamera( onb, ptLocation, fov, xres, yres, pixelAR, exposure, scanningRate, pixelRate, iso, fstop );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "pinhole camera" );
		return true;
	}

	//! Creates a camera based on thin lens model
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateThinlensCamera(
		ICamera** ppi,											///< [out] Pointer to recieve the camera
		const Point3& ptLocation,								///< [in] Absolute location of where the camera is located
		const Point3& ptLookAt, 								///< [in] Absolute point the camera is looking at
		const Vector3& vUp,										///< [in] Up vector of the camera
		const Scalar sensorSize,								///< [in] Sensor width (mm)
		const Scalar focalLength,								///< [in] Lens focal length (mm)
		const Scalar fstop,										///< [in] f-number (dimensionless; aperture diameter = focalLength/fstop)
		const Scalar focusDistance,								///< [in] Focus plane distance (scene units; must be > focal_in_scene_units)
		const Scalar sceneUnitMeters,							///< [in] Meters per scene unit
		const unsigned int xres,								///< [in] X resolution of virtual screen
		const unsigned int yres,								///< [in] Y resolution of virtual screen
		const Scalar pixelAR,									///< [in] Pixel aspect ratio
		const Scalar exposure,									///< [in] Exposure time of the camera
		const Scalar scanningRate,								///< [in] Rate at which each scanline is recorded
		const Scalar pixelRate,									///< [in] Rate at which each pixel is recorded
		const Vector3& orientation,								///< [in] Orientation (Pitch,Roll,Yaw)
		const Vector2& target_orientation,						///< [in] Orientation relative to a target
		const unsigned int apertureBlades,						///< [in] Polygonal aperture blades; 0 = perfect disk
		const Scalar apertureRotation,							///< [in] Polygon rotation (radians)
		const Scalar anamorphicSqueeze,							///< [in] Aperture x-axis scale (1.0 = circular)
		const Scalar tiltX,										///< [in] Focal-plane tilt around x-axis (radians)
		const Scalar tiltY,										///< [in] Focal-plane tilt around y-axis (radians)
		const Scalar shiftX,									///< [in] Lens shift along x (mm)
		const Scalar shiftY,									///< [in] Lens shift along y (mm)
		const Scalar iso										///< [in] Landing 5: ISO (0 = disabled)
		)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ThinLensCamera( ptLocation, ptLookAt, vUp, sensorSize, focalLength, fstop, focusDistance, sceneUnitMeters, xres, yres, pixelAR, exposure, scanningRate, pixelRate, orientation, target_orientation, apertureBlades, apertureRotation, anamorphicSqueeze, tiltX, tiltY, shiftX, shiftY, iso );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "thinlens camera" );
		return true;
	}

	//! Creates a fisheye camera
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateFisheyeCamera(
		ICamera** ppi,											///< [out] Pointer to recieve the camera
		const Point3& ptLocation,								///< [in] Absolute location of where the camera is located
		const Point3& ptLookAt, 								///< [in] Absolute point the camera is looking at
		const Vector3& vUp,										///< [in] Up vector of the camera
		const unsigned int xres,								///< [in] X resolution of virtual screen
		const unsigned int yres,								///< [in] Y resolution of virtual screen
		const Scalar pixelAR,									///< [in] Pixel aspect ratio
		const Scalar exposure,									///< [in] Exposure time of the camera
		const Scalar scanningRate,								///< [in] Rate at which each scanline is recorded
		const Scalar pixelRate,									///< [in] Rate at which each pixel is recorded
		const Vector3& orientation,								///< [in] Orientation (Pitch,Roll,Yaw)
		const Vector2& target_orientation,						///< [in] Orientation relative to a target
		const Scalar scale										///< [in] Scale factor to exagerrate the effects
		)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new FisheyeCamera( ptLocation, ptLookAt, vUp, xres, yres, pixelAR, exposure, scanningRate, pixelRate, orientation, target_orientation, scale );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "fisheye camera" );
		return true;
	}

	//! Creates an orthographic camera
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateOrthographicCamera(
		ICamera** ppi,											///< [out] Pointer to recieve the camera
		const Point3& ptLocation,								///< [in] Absolute location of where the camera is located
		const Point3& ptLookAt, 								///< [in] Absolute point the camera is looking at
		const Vector3& vUp,										///< [in] Up vector of the camera
		const unsigned int xres,								///< [in] X resolution of virtual screen
		const unsigned int yres,								///< [in] Y resolution of virtual screen
		const Vector2& vpScale,									///< [in] Viewport scale factor
		const Scalar pixelAR,									///< [in] Pixel aspect ratio
		const Scalar exposure,									///< [in] Exposure time of the camera
		const Scalar scanningRate,								///< [in] Rate at which each scanline is recorded
		const Scalar pixelRate,									///< [in] Rate at which each pixel is recorded
		const Vector3& orientation,								///< [in] Orientation (Pitch,Roll,Yaw)
		const Vector2& target_orientation						///< [in] Orientation relative to a target
		)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new OrthographicCamera( ptLocation, ptLookAt, vUp, xres, yres, vpScale, pixelAR, exposure, scanningRate, pixelRate, orientation, target_orientation );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "orthographic camera" );
		return true;
	}
}

///////////////////////////////////////////////////////////
// Defines Light creation
///////////////////////////////////////////////////////////

#include "Lights/PointLight.h"
#include "Lights/SpotLight.h"
#include "Lights/AmbientLight.h"
#include "Lights/DirectionalLight.h"

namespace RISE
{
	//! Creates a infinite point omni light, located at the origin
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePointOmniLight(
		ILightPriv** ppi,										///< [out] Pointer to recieve the light
		const Scalar power,										///< [in] Power of the light in watts
		const RISEPel color,									///< [in] Color of the light in the linear ProPhoto colorspace
		const bool shootPhotons									///< [in] Should this light shoot photons for photon mapping?
		)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new PointLight( power, color, shootPhotons );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "point light" );
		return true;
	}

	//! Creates a infinite point spot light
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePointSpotLight(
		ILightPriv** ppi,										///< [out] Pointer to recieve the light
		const Scalar power,										///< [in] Power of the light in watts
		const RISEPel color,									///< [in] Color of the light in the linear ProPhoto colorspace
		const Point3& foc,										///< [in] Point the center of the light is focussing on
		const Scalar inner,										///< [in] Angle of the inner cone in radians
		const Scalar outer,										///< [in] Angle of the outer cone in radians
		const bool shootPhotons									///< [in] Should this light shoot photons for photon mapping?
		)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new SpotLight( power, foc, inner, outer, color, shootPhotons );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "spot light" );
		return true;
	}

	//! Creates the ambient light
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAmbientLight(
		ILightPriv** ppi,										///< [out] Pointer to recieve the light
		const Scalar power,										///< [in] Power of the light in watts
		const RISEPel color										///< [in] Color of the light in the linear ProPhoto colorspace
		)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new AmbientLight( power, color );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "ambient light" );
		return true;
	}

	//! Creates a infinite directional light, coming from a particular direction
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDirectionalLight(
		ILightPriv** ppi,										///< [out] Pointer to recieve the light
		const Scalar power,										///< [in] Power of the light in watts
		const RISEPel color,									///< [in] Color of the light in the linear ProPhoto colorspace
		const Vector3 vDir										///< [in] Direction the light is shining
		)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new DirectionalLight( power, color, vDir );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "directional light" );
		return true;
	}
}

///////////////////////////////////////////////////////////
// Defines Geometry creation
///////////////////////////////////////////////////////////

#include "Geometry/ClippedPlaneGeometry.h"
#include "Geometry/BoxGeometry.h"
#include "Geometry/SphereGeometry.h"
#include "Geometry/EllipsoidGeometry.h"
#include "Geometry/CylinderGeometry.h"
#include "Geometry/TorusGeometry.h"
#include "Geometry/CircularDiskGeometry.h"
#include "Geometry/InfinitePlaneGeometry.h"
#include "Geometry/BezierPatchGeometry.h"
#include "Geometry/BilinearPatchGeometry.h"
#include "Geometry/DisplacedGeometry.h"
#include "Geometry/SDFGeometry.h"
#include "Interfaces/ProceduralDescriptors.h"	// SweepDescriptor / PathInstancesDescriptor for the procedural mesh factories
#include "Geometry/GeometryUtilities.h"		// MakeIndexedTriangleSameIdx for the procedural mesh factories
#include "Geometry/TriangleMeshGeometry.h"
#include "Geometry/TriangleMeshGeometryIndexed.h"
#include "Geometry/TriangleMeshLoader3DS.h"
#include "Geometry/TriangleMeshLoaderRAW.h"
#include "Geometry/TriangleMeshLoaderRAW2.h"
#include "Geometry/TriangleMeshLoaderPLY.h"

namespace RISE
{
	//! Creates a box located at the origin
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBoxGeometry(
							IGeometry** ppi,					///< [out] Pointer to recieve the geometry
							const Scalar width,					///< [in] Width of the box
							const Scalar height,				///< [in] Height of the box
							const Scalar depth					///< [in] Depth of the box
							)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new BoxGeometry( width, height, depth );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "box" );
		return true;
	}

	//! Creates a circular disk at the origin
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCircularDiskGeometry(
									IGeometry** ppi,			///< [out] Pointer to recieve the geometry
									const Scalar radius,		///< [in] Radius of the disk
									const char axis				///< [in] (x|y|z) Which axis the disk sits on
									)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new CircularDiskGeometry( radius, axis );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "circular disk" );
		return true;
	}

	//! Creates a clipped plane, defined by four points
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateClippedPlaneGeometry(
									IGeometry** ppi,			///< [out] Pointer to recieve the geometry
									const Point3 (&points)[4], ///< [in] Points defining the clipped plane
									const bool doublesided		///< [in] Is it double sided?
									)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ClippedPlaneGeometry( points, doublesided );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "clipped plane" );
		return true;
	}

	//! Creates a Cylinder at the origin
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCylinderGeometry(
								IGeometry** ppi,				///< [out] Pointer to recieve the geometry
								const char axis,				///< [in] (x|y|z) Which axis the cylinder is sitting on
								const Scalar radius,			///< [in] Radius of the cylinder
								const Scalar height			///< [in] Height of the cylinder
								)
	{
		if( !ppi ) {
			return false;
		}


		(*ppi) = new CylinderGeometry( axis, radius, height );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "cylinder" );
		return true;
	}

	//! Creates an infinite plane that passes through the origin
	/// \return TRUE if successful, FALSE otherwise
	/// \todo This needs to be seriously re-evaluated
	bool RISE_API_CreateInfinitePlaneGeometry(
											IGeometry** ppi,	///< [out] Pointer to recieve the geometry
											const Scalar xt,	///< [in] How often to tile in X
											const Scalar yt	///< [in] How often to tile in Y
											)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new InfinitePlaneGeometry( xt, yt );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "infinite plane" );
		return true;
	}

	//! Creates a sphere at the origin
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSphereGeometry(
									IGeometry** ppi,			///< [out] Pointer to recieve the geometry
									const Scalar radius		///< [in] Radius of the sphere
									)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new SphereGeometry( radius );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "sphere" );
		return true;
	}

	//! Creates an ellipsoid at the origin
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateEllipsoidGeometry(
									IGeometry** ppi,			///< [out] Pointer to recieve the geometry
									const Vector3& radii		///< [in] Radii of the three axis
									)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new EllipsoidGeometry( radii );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "ellipsoid" );
		return true;
	}

	//! Creates a torus at the origin
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTorusGeometry(
									IGeometry** ppi,			///< [out] Pointer to recieve the geometry
									const Scalar majorRad,	///< [in] Major radius
									const Scalar minorRad		///< [in] Minor radius (as a percentage of the major radius)
									)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TorusGeometry( majorRad, minorRad );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "torus" );
		return true;
	}

	//! Creates a triangle mesh geometry object.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTriangleMeshGeometry(
						ITriangleMeshGeometry** ppi,			///< [out] Pointer to recieve the geometry
						const bool double_sided					///< [in] Are the triangles double sided ?
						)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TriangleMeshGeometry( double_sided );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "triangle mesh" );
		return true;
	}

	//! Creates an indexed triangle mesh geometry object.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTriangleMeshGeometryIndexed(
						ITriangleMeshGeometryIndexed** ppi,	///< [out] Pointer to recieve the geometry
						const bool double_sided,				///< [in] Are the triangles double sided ?
						const bool face_normals				///< [in] Use face normals rather than vertex normals
						)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TriangleMeshGeometryIndexed( double_sided, face_normals );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "triangle mesh indexed" );
		return true;
	}

	//! Creates a mesh loader capable of loading a 3DS mesh from read buffer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_Create3DSTriangleMeshLoader(
						ITriangleMeshLoaderIndexed** ppi,		///< [out] Pointer to recieve the mesh loader
						IReadBuffer* pBuffer					///< [in] The buffer to load the 3DS mesh from
						)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TriangleMeshLoader3DS( pBuffer );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "mesh loader 3DS" );
		return true;
	}

	//! Creates a mesh loader capable of loading from a raw file
	/// \return TRUE if successful, FALSE otherwise
	/// \todo this is deprecated and should be removed
	bool RISE_API_CreateRAWTriangleMeshLoader(
						ITriangleMeshLoader** ppi,				///< [out] Pointer to recieve the mesh loader
						const char* szFileName					///< [in] Name of the file to load from
						)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TriangleMeshLoaderRAW( szFileName );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "mesh loader RAW" );
		return true;
	}

	//! Creates a triangle mesh geometry from a file of version 2
	//! The format of the file for this version is different from the one
	//! above
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateRAW2TriangleMeshLoader(
						ITriangleMeshLoaderIndexed** ppi,		///< [out] Pointer to recieve the mesh loader
						const char* szFileName					///< [in] Name of the file to load from
						)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TriangleMeshLoaderRAW2( szFileName );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "mesh loader RAW2" );
		return true;
	}

	//! Creates a mesh loader capable of loading from a ply file
	/// \return TRUE if successful, FALSE otherwise
	/// \todo this is deprecated and should be removed
	bool RISE_API_CreatePLYTriangleMeshLoader(
						ITriangleMeshLoaderIndexed** ppi,		///< [out] Pointer to recieve the mesh loader
						const char* szFileName,					///< [in] Name of the file to load from
						const bool bInvertFaces					///< [in] Should the faces be inverted?
						)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TriangleMeshLoaderPLY( szFileName, bInvertFaces );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "mesh loader PLY" );
		return true;
	}

	//! RISE_API_CreateGLTFTriangleMeshLoader was retired 2026-05-01.  glTF
	//! single-primitive imports now flow through Job::AddGLTFTriangleMeshGeometry,
	//! which constructs a GLTFSceneImporter and calls ImportPrimitive.  The
	//! intermediate ITriangleMeshLoaderIndexed wrapper that used to live
	//! here re-parsed the .gltf for every primitive — pathological on
	//! NewSponza-class assets.  See Importers/GLTFSceneImporter.h
	//! "Lifecycle" for the new design.

	//! Creates a bezier-patch geometry (analytic intersection).
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBezierPatchGeometry(
						IBezierPatchGeometry** ppi,				///< [out] Pointer to recieve the geometry
						const unsigned int max_patches,			///< [in] Maximum number of patches per accelerator leaf
						const unsigned char max_recur,			///< [in] Maximum accelerator recursion depth
						const bool use_bsp						///< [in] Use BSP tree (true) or Octree (false)
						)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new BezierPatchGeometry( max_patches, max_recur, use_bsp );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "bezier patch geometry" );

		return true;
	}

	//! Creates a geometry object made up of a series of bilinear patches
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBilinearPatchGeometry(
						IBilinearPatchGeometry** ppi,			///< [out] Pointer to recieve the geometry
						const unsigned int max_patches,			///< [in] Maximum number of polygons / octant node
						const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
						const bool use_bsp						///< [in] Use a BSP tree rather than an Octree
						)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new BilinearPatchGeometry( max_patches, max_recur, use_bsp );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "bilinear patch geometry" );

		return true;
	}

	bool RISE_API_CreateDisplacedGeometry(
						IGeometry**         ppi,
						IGeometry*          pBase,
						const unsigned int  detail,
						IFunction2D*        displacement,
						const Scalar        disp_scale,
						const bool          double_sided,
						const bool          face_normals,
						const bool          seam_fold
						)
	{
		if( !ppi || !pBase ) {
			return false;
		}

		if( detail > 256 ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"RISE_API_CreateDisplacedGeometry: detail=%u is high; expect many triangles (grid is (detail+1)^2 per base axis or per face)",
				detail );
		}

		DisplacedGeometry* pGeom = new DisplacedGeometry(
			pBase, detail, displacement, disp_scale,
			double_sided, face_normals, seam_fold );
		GlobalLog()->PrintNew( pGeom, __FILE__, __LINE__, "displaced geometry" );

		if( !pGeom->IsValid() ) {
			// Base could not be tessellated (e.g. InfinitePlaneGeometry).  Refuse construction
			// loudly so the caller's scene-parse layer can surface the error.  The geometry
			// starts with refcount=1 from its Reference base, so one release destroys it.
			GlobalLog()->Print( eLog_Error, "RISE_API_CreateDisplacedGeometry: base geometry does not support TessellateToMesh \u2014 refusing." );
			pGeom->release();
			*ppi = 0;
			return false;
		}

		*ppi = pGeom;
		return true;
	}

	bool RISE_API_CreateSDFGeometry(
						IGeometry**          ppi,
						const char*          szFileName,
						const char*          szParts,
						const unsigned int   maxSteps,
						const double         surfaceEpsilonFraction,
						const unsigned int   samplingDetail
						)
	{
		if( !ppi ) {
			return false;
		}
		*ppi = 0;

		// Exactly one source.  "none" is the scene-language not-set
		// convention for filename parameters.
		const bool hasFile  = ( szFileName && szFileName[0] && strcmp( szFileName, "none" ) != 0 );
		const bool hasParts = ( szParts && szParts[0] );
		if( hasFile == hasParts ) {
			GlobalLog()->Print( eLog_Error, hasFile
				? "RISE_API_CreateSDFGeometry:: both an inline part list and a parts file were given -- provide exactly one"
				: "RISE_API_CreateSDFGeometry:: no SDF source -- provide inline part lines or a parts file" );
			return false;
		}

		// Both sources route through the ONE grammar --
		// SDFGeometry::ParsePartLines -- which logs source / line / token
		// context on failure.
		std::vector<SDFGeometry::Part> parts;
		if( hasParts ) {
			if( !SDFGeometry::ParsePartLines( szParts, "<inline part list>", parts ) ) {
				return false;
			}
		} else {
			FILE* inputFile = fopen( szFileName, "rb" );
			if( !inputFile ) {
				char msg[512];
				snprintf( msg, sizeof(msg), "RISE_API_CreateSDFGeometry:: cannot open `%s`", szFileName );
				GlobalLog()->Print( eLog_Error, msg );
				return false;
			}
			std::string source;
			char buf[4096];
			size_t got = 0;
			while( ( got = fread( buf, 1, sizeof(buf), inputFile ) ) > 0 ) {
				source.append( buf, got );
			}
			fclose( inputFile );
			if( !SDFGeometry::ParsePartLines( source.c_str(), szFileName, parts ) ) {
				return false;
			}
		}

		if( parts.empty() ) {
			GlobalLog()->Print( eLog_Error, hasParts
				? "RISE_API_CreateSDFGeometry:: inline part list contains no parts"
				: "RISE_API_CreateSDFGeometry:: parts file contains no parts" );
			return false;
		}

		(*ppi) = new SDFGeometry( parts, maxSteps, surfaceEpsilonFraction, samplingDetail );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "sdf geometry" );
		return true;
	}

	// A FLAT Cartesian-grid circular disk with LINEAR Cartesian UV
	// (u = (x+R)/2R, v = (y+R)/2R) and +Z normals -- the general flat base
	// for displacing an arbitrary 2D field (an expression_function2d, a
	// texture, noise) onto a disk via displaced_geometry (uv_seam_fold
	// FALSE).  Uniform world-space cell density everywhere (unlike a polar
	// fan), so a Cartesian pattern reads the same at the centre and the rim.
	bool RISE_API_CreateCartesianDiskGeometry(
						ITriangleMeshGeometryIndexed** ppi,
						const double         radius,
						const int            meshN
						)
	{
		if( !ppi ) {
			return false;
		}
		*ppi = 0;
		const int N = meshN < 2 ? 2 : ( meshN > 4096 ? 4096 : meshN );
		const Scalar R = radius;
		if( !( R > 0 ) ) {
			GlobalLog()->Print( eLog_Error, "RISE_API_CreateCartesianDiskGeometry: radius must be > 0" );
			return false;
		}
		std::vector<Scalar> xs( N );
		const Scalar step = Scalar(2) * R / Scalar( N - 1 );
		for( int i = 0; i < N; i++ ) xs[i] = -R + step * Scalar(i);
		xs[0] = -R; xs[ N - 1 ] = R;

		std::vector<int> idx( size_t(N) * N, -1 );
		int vc = 0;
		for( int j = 0; j < N; j++ )
			for( int i = 0; i < N; i++ )
				if( sqrt( xs[i]*xs[i] + xs[j]*xs[j] ) <= R )
					idx[ size_t(j)*N + i ] = vc++;
		if( vc < 3 ) {
			GlobalLog()->Print( eLog_Error, "RISE_API_CreateCartesianDiskGeometry: degenerate (fewer than 3 vertices inside the disk)" );
			return false;
		}

		TriangleMeshGeometryIndexed* pGeom = new TriangleMeshGeometryIndexed( false, false );
		GlobalLog()->PrintNew( pGeom, __FILE__, __LINE__, "cartesian disk geometry" );
		pGeom->BeginIndexedTriangles();
		const Scalar inv2R = Scalar(1) / ( Scalar(2) * R );
		for( int j = 0; j < N; j++ ) {
			const size_t row = size_t(j) * N;
			for( int i = 0; i < N; i++ ) {
				if( idx[ row + i ] < 0 ) continue;
				pGeom->AddVertex( Vertex( xs[i], xs[j], Scalar(0) ) );
				pGeom->AddNormal( Normal( 0, 0, 1 ) );
				pGeom->AddTexCoord( TexCoord( ( xs[i] + R ) * inv2R, ( xs[j] + R ) * inv2R ) );
			}
		}
		for( int j = 0; j < N - 1; j++ ) {
			const size_t row = size_t(j) * N;
			for( int i = 0; i < N - 1; i++ ) {
				const int a = idx[ row + i ], b = idx[ row + i + 1 ];
				const int c = idx[ row + N + i + 1 ], d = idx[ row + N + i ];
				if( a >= 0 && b >= 0 && c >= 0 && d >= 0 ) {
					pGeom->AddIndexedTriangle( MakeIndexedTriangleSameIdx( a, b, c ) );
					pGeom->AddIndexedTriangle( MakeIndexedTriangleSameIdx( a, c, d ) );
				}
			}
		}
		pGeom->DoneIndexedTriangles();
		*ppi = pGeom;
		return true;
	}

	//////////////////////////////////////////////////////////////////////
	// General procedural sweep machinery shared by RISE_API_CreateSweepGeometry
	// and RISE_API_CreatePathInstancesGeometry: a 3D Catmull-Rom path sampler
	// and rotation-minimizing frame transport (Wang et al. 2008 double
	// reflection -- no torsion-induced spin, stable through inflections).
	//////////////////////////////////////////////////////////////////////
	namespace
	{
		struct SweepV3 { Scalar x, y, z; };

		inline SweepV3 svSub( const SweepV3& a, const SweepV3& b ) { SweepV3 r = { a.x-b.x, a.y-b.y, a.z-b.z }; return r; }
		inline SweepV3 svAdd( const SweepV3& a, const SweepV3& b ) { SweepV3 r = { a.x+b.x, a.y+b.y, a.z+b.z }; return r; }
		inline SweepV3 svScale( const SweepV3& a, const Scalar s ) { SweepV3 r = { a.x*s, a.y*s, a.z*s }; return r; }
		inline Scalar  svDot( const SweepV3& a, const SweepV3& b ) { return a.x*b.x + a.y*b.y + a.z*b.z; }
		inline SweepV3 svCross( const SweepV3& a, const SweepV3& b )
		{
			SweepV3 r = { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
			return r;
		}
		inline Scalar svLen( const SweepV3& a ) { return sqrt( svDot( a, a ) ); }
		inline SweepV3 svNorm( const SweepV3& a )
		{
			const Scalar l = svLen( a );
			return ( l > 0 ) ? svScale( a, Scalar(1) / l ) : a;
		}

		// Catmull-Rom through 3D control points: reflective end padding,
		// per-segment t = k/per for k in [0, per), then the final point --
		// the same sampling scheme every RISE procedural path uses.
		void SampleCatmullRom3(
			const double* ctrl, const unsigned int nCtrl, const int nLen,
			std::vector<SweepV3>& out )
		{
			std::vector<SweepV3> pad( nCtrl + 2 );
			for( unsigned int i = 0; i < nCtrl; i++ ) {
				pad[ i + 1 ].x = ctrl[ 3*i ]; pad[ i + 1 ].y = ctrl[ 3*i + 1 ]; pad[ i + 1 ].z = ctrl[ 3*i + 2 ];
			}
			pad[0]         = svSub( svScale( pad[1], 2 ),       pad[2] );
			pad[ nCtrl+1 ] = svSub( svScale( pad[ nCtrl ], 2 ), pad[ nCtrl-1 ] );
			const int segs = int(nCtrl) - 1;
			const int per = ( nLen / segs ) < 2 ? 2 : ( nLen / segs );
			out.clear();
			out.reserve( size_t(segs) * per + 1 );
			for( int sgi = 0; sgi < segs; sgi++ ) {
				const SweepV3& p0 = pad[ sgi ];
				const SweepV3& p1 = pad[ sgi + 1 ];
				const SweepV3& p2 = pad[ sgi + 2 ];
				const SweepV3& p3 = pad[ sgi + 3 ];
				for( int k = 0; k < per; k++ ) {
					const Scalar t = Scalar(k) / Scalar(per);
					const Scalar t2 = t * t, t3 = t2 * t;
					SweepV3 q;
					q.x = Scalar(0.5) * ( 2*p1.x + (-p0.x+p2.x)*t + (2*p0.x-5*p1.x+4*p2.x-p3.x)*t2 + (-p0.x+3*p1.x-3*p2.x+p3.x)*t3 );
					q.y = Scalar(0.5) * ( 2*p1.y + (-p0.y+p2.y)*t + (2*p0.y-5*p1.y+4*p2.y-p3.y)*t2 + (-p0.y+3*p1.y-3*p2.y+p3.y)*t3 );
					q.z = Scalar(0.5) * ( 2*p1.z + (-p0.z+p2.z)*t + (2*p0.z-5*p1.z+4*p2.z-p3.z)*t2 + (-p0.z+3*p1.z-3*p2.z+p3.z)*t3 );
					out.push_back( q );
				}
			}
			out.push_back( pad[ nCtrl ] );
		}

		// Per-sample tangents (central differences, one-sided ends) and
		// rotation-minimizing frames.  B = binormal (profile x axis),
		// N = B x T (profile h axis).  The initial binormal comes from the
		// caller's hint, or auto-picks the world axis most perpendicular to
		// the start tangent (a planar path in YZ therefore gets the fixed
		// world-X width axis the retired band generator used).
		void BuildPathFrames(
			const std::vector<SweepV3>& path,
			const SweepV3& hint, const bool haveHint,
			std::vector<SweepV3>& T, std::vector<SweepV3>& B, std::vector<SweepV3>& N )
		{
			const size_t n = path.size();
			T.resize( n ); B.resize( n ); N.resize( n );
			for( size_t i = 0; i < n; i++ ) {
				const SweepV3& a = path[ i > 0 ? i - 1 : 0 ];
				const SweepV3& b = path[ i + 1 < n ? i + 1 : n - 1 ];
				SweepV3 t = svSub( b, a );
				const Scalar l = svLen( t );
				if( l > 0 ) {
					t = svScale( t, Scalar(1) / l );
				} else {
					t = ( i > 0 ) ? T[ i - 1 ] : t;
				}
				T[i] = t;
			}
			if( svLen( T[0] ) <= 0 ) {
				GlobalLog()->Print( eLog_Warning,
					"BuildPathFrames: degenerate start tangent (duplicated first path point?) -- frames may be unreliable" );
			}
			SweepV3 b0 = { 0, 0, 0 };
			const SweepV3 axisPick = [&]() {
				const Scalar ax = fabs( T[0].x ), ay = fabs( T[0].y ), az = fabs( T[0].z );
				SweepV3 a = { 0, 0, 0 };
				if( ax <= ay && ax <= az )      a.x = 1;
				else if( ay <= ax && ay <= az ) a.y = 1;
				else                            a.z = 1;
				return a;
			}();
			if( haveHint ) {
				b0 = svNorm( svSub( hint, svScale( T[0], svDot( hint, T[0] ) ) ) );
				if( svLen( b0 ) <= 0 ) {
					GlobalLog()->Print( eLog_Warning,
						"BuildPathFrames: frame_hint is parallel to the start tangent -- falling back to the auto axis" );
					b0 = svNorm( svSub( axisPick, svScale( T[0], svDot( axisPick, T[0] ) ) ) );
				}
			} else {
				b0 = svNorm( svSub( axisPick, svScale( T[0], svDot( axisPick, T[0] ) ) ) );
			}
			B[0] = b0;
			N[0] = svCross( B[0], T[0] );
			for( size_t i = 0; i + 1 < n; i++ ) {
				// double reflection step
				const SweepV3 v1 = svSub( path[i+1], path[i] );
				const Scalar c1 = svDot( v1, v1 );
				if( c1 <= 0 ) {
					// duplicated path sample: carry the frame, re-projected
					// off the (possibly different) next tangent
					B[i+1] = svNorm( svSub( B[i], svScale( T[i+1], svDot( B[i], T[i+1] ) ) ) );
					N[i+1] = svCross( B[i+1], T[i+1] );
					continue;
				}
				const SweepV3 bL = svSub( B[i], svScale( v1, ( Scalar(2)/c1 ) * svDot( v1, B[i] ) ) );
				const SweepV3 tL = svSub( T[i], svScale( v1, ( Scalar(2)/c1 ) * svDot( v1, T[i] ) ) );
				const SweepV3 v2 = svSub( T[i+1], tL );
				const Scalar c2 = svDot( v2, v2 );
				SweepV3 b = ( c2 > 0 ) ? svSub( bL, svScale( v2, ( Scalar(2)/c2 ) * svDot( v2, bL ) ) ) : bL;
				// re-orthonormalize against accumulated drift
				b = svNorm( svSub( b, svScale( T[i+1], svDot( b, T[i+1] ) ) ) );
				B[i+1] = b;
				N[i+1] = svCross( B[i+1], T[i+1] );
			}
		}

		// Ear-clipping triangulation of a simple 2D polygon (indices into the
		// profile).  Handles non-convex profiles (grooves, mouldings); the
		// polygon must be simple (non-self-intersecting).  O(n^2), fine for
		// authored profile sizes.
		bool EarClipProfile(
			const std::vector<Scalar>& px, const std::vector<Scalar>& ph,
			std::vector<unsigned int>& tris )
		{
			const size_t n = px.size();
			if( n < 3 ) {
				return false;
			}
			// signed area -> winding
			Scalar area2 = 0;
			for( size_t i = 0; i < n; i++ ) {
				const size_t j = ( i + 1 ) % n;
				area2 += px[i] * ph[j] - px[j] * ph[i];
			}
			const Scalar wind = ( area2 >= 0 ) ? Scalar(1) : Scalar(-1);
			// Drop consecutively-coincident points (the documented
			// duplicate-a-point hard-edge idiom for the SIDE normals) before
			// clipping: a zero-length edge makes its corner permanently
			// reflex (cross == 0) and its twin sits ON every candidate ear's
			// boundary, deadlocking the clip.  The dropped duplicates simply
			// go unreferenced by the cap triangles.
			std::vector<unsigned int> idx;
			idx.reserve( n );
			for( size_t i = 0; i < n; i++ ) {
				const size_t prev = ( i + n - 1 ) % n;
				if( px[i] == px[prev] && ph[i] == ph[prev] ) {
					continue;
				}
				idx.push_back( (unsigned int)i );
			}
			if( idx.size() < 3 ) {
				return false;
			}
			tris.clear();
			tris.reserve( ( n - 2 ) * 3 );
			size_t guard = n * n + 8;
			while( idx.size() > 3 && guard-- > 0 ) {
				bool clipped = false;
				for( size_t i = 0; i < idx.size(); i++ ) {
					const unsigned int ia = idx[ ( i + idx.size() - 1 ) % idx.size() ];
					const unsigned int ib = idx[ i ];
					const unsigned int ic = idx[ ( i + 1 ) % idx.size() ];
					const Scalar cross = ( px[ib]-px[ia] ) * ( ph[ic]-ph[ia] ) - ( ph[ib]-ph[ia] ) * ( px[ic]-px[ia] );
					if( cross * wind <= 0 ) {
						continue;	// reflex corner, not an ear
					}
					// no other active vertex inside the candidate ear
					bool inside = false;
					for( size_t k = 0; k < idx.size() && !inside; k++ ) {
						const unsigned int iq = idx[k];
						if( iq == ia || iq == ib || iq == ic ) continue;
						// a probe POSITIONALLY coincident with a corner is the
						// corner (authored duplicates) -- not an interior point
						if( ( px[iq] == px[ia] && ph[iq] == ph[ia] ) ||
							( px[iq] == px[ib] && ph[iq] == ph[ib] ) ||
							( px[iq] == px[ic] && ph[iq] == ph[ic] ) ) continue;
						const Scalar d1 = ( ( px[ib]-px[ia] )*( ph[iq]-ph[ia] ) - ( ph[ib]-ph[ia] )*( px[iq]-px[ia] ) ) * wind;
						const Scalar d2 = ( ( px[ic]-px[ib] )*( ph[iq]-ph[ib] ) - ( ph[ic]-ph[ib] )*( px[iq]-px[ib] ) ) * wind;
						const Scalar d3 = ( ( px[ia]-px[ic] )*( ph[iq]-ph[ic] ) - ( ph[ia]-ph[ic] )*( px[iq]-px[ic] ) ) * wind;
						inside = ( d1 >= 0 && d2 >= 0 && d3 >= 0 );
					}
					if( inside ) {
						continue;
					}
					tris.push_back( ia ); tris.push_back( ib ); tris.push_back( ic );
					idx.erase( idx.begin() + i );
					clipped = true;
					break;
				}
				if( !clipped ) {
					return false;	// degenerate / self-intersecting profile
				}
			}
			if( idx.size() == 3 ) {
				tris.push_back( idx[0] ); tris.push_back( idx[1] ); tris.push_back( idx[2] );
			}
			return true;
		}
	}

	// General profile sweep: an arbitrary CLOSED 2D profile polygon swept
	// along an arbitrary 3D Catmull-Rom path with rotation-minimizing
	// frames, optional linear per-axis taper, and ear-clipped end caps.
	// Profile-space (x, h) maps to  P(i) + x*scaleX(i)*B(i) + h*scaleY(i)*N(i).
	// Vertex normals come from closed-loop central differences over the
	// profile polyline (taper-corrected); longitudinal curvature lean is
	// deliberately ignored (matches mesh-sweep convention; per-vertex
	// normals carry the shading).
	bool RISE_API_CreateSweepGeometry(
						ITriangleMeshGeometryIndexed** ppi,
						const SweepDescriptor&         desc
						)
	{
		if( !ppi ) {
			return false;
		}
		*ppi = 0;

		if( !desc.profilePoints || desc.numProfilePoints < 3 ) {
			GlobalLog()->Print( eLog_Error, "RISE_API_CreateSweepGeometry: need at least 3 `profile_point` entries (a closed polygon)" );
			return false;
		}
		if( !desc.pathPoints || desc.numPathPoints < 2 ) {
			GlobalLog()->Print( eLog_Error, "RISE_API_CreateSweepGeometry: need at least 2 `point` path control points" );
			return false;
		}
		if( !( desc.endScaleX > 0 ) || !( desc.endScaleY > 0 ) ) {
			GlobalLog()->Print( eLog_Error, "RISE_API_CreateSweepGeometry: end_scale_x / end_scale_y must be > 0" );
			return false;
		}
		const int nLen = desc.nLen < 2 ? 2 : ( desc.nLen > 4096 ? 4096 : desc.nLen );
		if( nLen != desc.nLen ) {
			GlobalLog()->PrintEx( eLog_Warning, "RISE_API_CreateSweepGeometry: n_len %d clamped to %d", desc.nLen, nLen );
		}
		const unsigned int nProf = desc.numProfilePoints;
		if( nProf > 4096 ) {
			GlobalLog()->Print( eLog_Error, "RISE_API_CreateSweepGeometry: more than 4096 profile points" );
			return false;
		}

		std::vector<Scalar> px( nProf ), ph( nProf );
		for( unsigned int k = 0; k < nProf; k++ ) {
			px[k] = desc.profilePoints[ 2*k ];
			ph[k] = desc.profilePoints[ 2*k + 1 ];
		}

		// closed-loop central-difference profile normals, oriented OUTWARD
		// by the polygon winding (positive signed area = CCW -> outward is
		// the right-hand perpendicular of the running direction).
		Scalar area2 = 0;
		for( unsigned int k = 0; k < nProf; k++ ) {
			const unsigned int j = ( k + 1 ) % nProf;
			area2 += px[k] * ph[j] - px[j] * ph[k];
		}
		const Scalar outward = ( area2 >= 0 ) ? Scalar(1) : Scalar(-1);
		std::vector<Scalar> pnx( nProf ), pnh( nProf );
		for( unsigned int k = 0; k < nProf; k++ ) {
			const unsigned int kp = ( k + nProf - 1 ) % nProf;
			const unsigned int kn = ( k + 1 ) % nProf;
			const Scalar dx = px[kn] - px[kp];
			const Scalar dh = ph[kn] - ph[kp];
			const Scalar l = sqrt( dx*dx + dh*dh );
			if( l > 0 ) {
				pnx[k] =  outward * dh / l;
				pnh[k] = -outward * dx / l;
			} else {
				pnx[k] = 0; pnh[k] = 1;
			}
		}

		// profile arc-length fractions for U
		std::vector<Scalar> uFrac( nProf + 1 );
		uFrac[0] = 0;
		for( unsigned int k = 0; k < nProf; k++ ) {
			const unsigned int j = ( k + 1 ) % nProf;
			const Scalar dx = px[j] - px[k], dh = ph[j] - ph[k];
			uFrac[ k + 1 ] = uFrac[k] + sqrt( dx*dx + dh*dh );
		}
		const Scalar uTotal = uFrac[ nProf ] > 0 ? uFrac[ nProf ] : Scalar(1);
		for( unsigned int k = 0; k <= nProf; k++ ) {
			uFrac[k] /= uTotal;
		}

		std::vector<SweepV3> path;
		SampleCatmullRom3( desc.pathPoints, desc.numPathPoints, nLen, path );
		const size_t n = path.size();

		const SweepV3 hint = { desc.frameHintX, desc.frameHintY, desc.frameHintZ };
		const bool haveHint = svLen( hint ) > 0;
		std::vector<SweepV3> T, B, N;
		BuildPathFrames( path, hint, haveHint, T, B, N );

		TriangleMeshGeometryIndexed* pGeom = new TriangleMeshGeometryIndexed( true, false );
		GlobalLog()->PrintNew( pGeom, __FILE__, __LINE__, "sweep geometry" );
		pGeom->BeginIndexedTriangles();

		// rings
		int vc = 0;
		for( size_t i = 0; i < n; i++ ) {
			const Scalar frac = Scalar(i) / Scalar( n - 1 );
			const Scalar sx = Scalar(1) + ( desc.endScaleX - Scalar(1) ) * frac;
			const Scalar sy = Scalar(1) + ( desc.endScaleY - Scalar(1) ) * frac;
			for( unsigned int k = 0; k < nProf; k++ ) {
				const Scalar lx = px[k] * sx;
				const Scalar lh = ph[k] * sy;
				pGeom->AddVertex( Vertex(
					path[i].x + lx * B[i].x + lh * N[i].x,
					path[i].y + lx * B[i].y + lh * N[i].y,
					path[i].z + lx * B[i].z + lh * N[i].z ) );
				// taper-corrected profile normal mapped through the frame
				const Scalar nx = pnx[k] / sx;
				const Scalar nh = pnh[k] / sy;
				const Scalar nl = sqrt( nx*nx + nh*nh );
				const Scalar nxn = ( nl > 0 ) ? nx / nl : 0;
				const Scalar nhn = ( nl > 0 ) ? nh / nl : 1;
				pGeom->AddNormal( Normal(
					nxn * B[i].x + nhn * N[i].x,
					nxn * B[i].y + nhn * N[i].y,
					nxn * B[i].z + nhn * N[i].z ) );
				pGeom->AddTexCoord( TexCoord( uFrac[k], frac ) );
				vc++;
			}
		}

		// side quads between consecutive rings (cyclic over the profile);
		// winding gives outward-facing triangles for a CCW profile.
		for( size_t i = 0; i + 1 < n; i++ ) {
			const int r0 = int(i) * int(nProf);
			const int r1 = int(i+1) * int(nProf);
			for( unsigned int k = 0; k < nProf; k++ ) {
				const unsigned int kn = ( k + 1 ) % nProf;
				const int a = r0 + int(k);
				const int b = r1 + int(k);
				const int c = r1 + int(kn);
				const int d = r0 + int(kn);
				if( outward > 0 ) {
					pGeom->AddIndexedTriangle( MakeIndexedTriangleSameIdx( a, b, c ) );
					pGeom->AddIndexedTriangle( MakeIndexedTriangleSameIdx( a, c, d ) );
				} else {
					pGeom->AddIndexedTriangle( MakeIndexedTriangleSameIdx( a, c, b ) );
					pGeom->AddIndexedTriangle( MakeIndexedTriangleSameIdx( a, d, c ) );
				}
			}
		}

		// end caps: ear-clipped, duplicated vertices with the flat cap normal
		if( desc.capStart || desc.capEnd ) {
			std::vector<unsigned int> capTris;
			if( !EarClipProfile( px, ph, capTris ) ) {
				GlobalLog()->Print( eLog_Error, "RISE_API_CreateSweepGeometry: profile triangulation failed (self-intersecting or degenerate polygon)" );
				pGeom->release();
				return false;
			}
			// profile bbox for cap UV
			Scalar bx0 = px[0], bx1 = px[0], bh0 = ph[0], bh1 = ph[0];
			for( unsigned int k = 1; k < nProf; k++ ) {
				if( px[k] < bx0 ) bx0 = px[k];
				if( px[k] > bx1 ) bx1 = px[k];
				if( ph[k] < bh0 ) bh0 = ph[k];
				if( ph[k] > bh1 ) bh1 = ph[k];
			}
			const Scalar bxs = ( bx1 > bx0 ) ? Scalar(1)/( bx1-bx0 ) : Scalar(1);
			const Scalar bhs = ( bh1 > bh0 ) ? Scalar(1)/( bh1-bh0 ) : Scalar(1);
			for( int side = 0; side < 2; side++ ) {
				const bool isStart = ( side == 0 );
				if( isStart ? !desc.capStart : !desc.capEnd ) {
					continue;
				}
				const size_t i = isStart ? 0 : n - 1;
				const Scalar frac = Scalar(i) / Scalar( n - 1 );
				const Scalar sx = Scalar(1) + ( desc.endScaleX - Scalar(1) ) * frac;
				const Scalar sy = Scalar(1) + ( desc.endScaleY - Scalar(1) ) * frac;
				// cap faces along -T at the start, +T at the end
				const Scalar sgn = isStart ? Scalar(-1) : Scalar(1);
				const Normal capN( sgn * T[i].x, sgn * T[i].y, sgn * T[i].z );
				const int base = vc;
				for( unsigned int k = 0; k < nProf; k++ ) {
					const Scalar lx = px[k] * sx;
					const Scalar lh = ph[k] * sy;
					pGeom->AddVertex( Vertex(
						path[i].x + lx * B[i].x + lh * N[i].x,
						path[i].y + lx * B[i].y + lh * N[i].y,
						path[i].z + lx * B[i].z + lh * N[i].z ) );
					pGeom->AddNormal( capN );
					pGeom->AddTexCoord( TexCoord( ( px[k]-bx0 ) * bxs, ( ph[k]-bh0 ) * bhs ) );
					vc++;
				}
				for( size_t t = 0; t + 2 < capTris.size(); t += 3 ) {
					const int a = base + int(capTris[t]);
					const int b = base + int(capTris[t+1]);
					const int c = base + int(capTris[t+2]);
					// orient the cap triangles along the cap normal.  In the
					// (B, N) profile basis B x N = -T, so triangles emitted in
					// profile winding carry geometric normal -T for a CCW
					// (outward > 0) profile: the END cap (faces +T) must flip
					// them, the START cap (faces -T) keeps them.
					const bool flip = isStart ? ( outward < 0 ) : ( outward > 0 );
					if( flip ) {
						pGeom->AddIndexedTriangle( MakeIndexedTriangleSameIdx( a, c, b ) );
					} else {
						pGeom->AddIndexedTriangle( MakeIndexedTriangleSameIdx( a, b, c ) );
					}
				}
			}
		}

		pGeom->DoneIndexedTriangles();
		*ppi = pGeom;
		return true;
	}

	// General along-path instancing: tessellate the named template ONCE via
	// the universal TessellateToMesh contract, then stamp transformed copies
	// along the Catmull-Rom path at arc-length pitch.  Template local axes:
	// +X -> binormal, +Y -> tangent (rotated by slant about the normal),
	// +Z -> frame normal.  Instances interpolate position AND frame inside
	// the crossing segment (no sample-snapping).
	bool RISE_API_CreatePathInstancesGeometry(
						ITriangleMeshGeometryIndexed** ppi,
						const PathInstancesDescriptor& desc
						)
	{
		if( !ppi ) {
			return false;
		}
		*ppi = 0;

		if( !desc.pGeometry ) {
			GlobalLog()->Print( eLog_Error, "RISE_API_CreatePathInstancesGeometry: no template geometry" );
			return false;
		}
		if( !desc.pathPoints || desc.numPathPoints < 2 ) {
			GlobalLog()->Print( eLog_Error, "RISE_API_CreatePathInstancesGeometry: need at least 2 `point` path control points" );
			return false;
		}
		if( !( desc.pitch > 0 ) || !( desc.scale > 0 ) ) {
			GlobalLog()->Print( eLog_Error, "RISE_API_CreatePathInstancesGeometry: pitch and scale must be > 0" );
			return false;
		}
		const int nLen = desc.nLen < 2 ? 2 : ( desc.nLen > 8192 ? 8192 : desc.nLen );
		if( nLen != desc.nLen ) {
			GlobalLog()->PrintEx( eLog_Warning, "RISE_API_CreatePathInstancesGeometry: n_len %d clamped to %d", desc.nLen, nLen );
		}
		const unsigned int detail = desc.detail < 3 ? 3 : ( desc.detail > 256 ? 256 : desc.detail );
		if( detail != desc.detail ) {
			GlobalLog()->PrintEx( eLog_Warning, "RISE_API_CreatePathInstancesGeometry: detail %u clamped to %u (template tessellation is O(detail^2))", desc.detail, detail );
		}

		// tessellate the template once (the same contract area lights and
		// SSS sampling rely on -- every first-class geometry provides it)
		IndexTriangleListType tTris;
		VerticesListType      tVerts;
		NormalsListType       tNorms;
		TexCoordsListType     tCoords;
		if( !desc.pGeometry->TessellateToMesh( tTris, tVerts, tNorms, tCoords, detail ) ||
			tVerts.empty() || tTris.empty() ) {
			GlobalLog()->Print( eLog_Error, "RISE_API_CreatePathInstancesGeometry: template geometry does not support TessellateToMesh (or tessellated empty)" );
			return false;
		}

		std::vector<SweepV3> path;
		SampleCatmullRom3( desc.pathPoints, desc.numPathPoints, nLen, path );
		const size_t n = path.size();

		const SweepV3 hint = { desc.frameHintX, desc.frameHintY, desc.frameHintZ };
		const bool haveHint = svLen( hint ) > 0;
		std::vector<SweepV3> T, B, N;
		BuildPathFrames( path, hint, haveHint, T, B, N );

		const Scalar slant = desc.slantDeg * PI / Scalar(180);
		const Scalar cs = cos( slant ), sn = sin( slant );

		TriangleMeshGeometryIndexed* pGeom = new TriangleMeshGeometryIndexed( true, false );
		GlobalLog()->PrintNew( pGeom, __FILE__, __LINE__, "path instances geometry" );
		pGeom->BeginIndexedTriangles();

		int vc = 0;
		unsigned int placed = 0;
		Scalar arc = 0;
		Scalar nextAt = ( desc.phase >= 0 ) ? desc.phase : desc.pitch * Scalar(0.5);	// < 0 sentinel = centred pitch/2 default
		// total-output budget: instances x template verts capped well below
		// the signed-index ceiling, so a tiny pitch + heavy template degrades
		// to a warning instead of an OOM / index overflow
		const unsigned int kMaxTotalVerts = 20000000;
		const unsigned int kMaxByBudget = kMaxTotalVerts / (unsigned int)tVerts.size();
		const unsigned int kMaxInstances = ( 100000 < kMaxByBudget ) ? 100000 : kMaxByBudget;
		for( size_t i = 1; i < n && placed < kMaxInstances; i++ ) {
			const SweepV3 seg = svSub( path[i], path[i-1] );
			const Scalar segLen = svLen( seg );
			Scalar segStart = arc;
			arc += segLen;
			while( nextAt <= arc && segLen > 0 && placed < kMaxInstances ) {
				const Scalar f = ( nextAt - segStart ) / segLen;
				// interpolate position + frame inside the segment
				const SweepV3 P = svAdd( path[i-1], svScale( seg, f ) );
				const SweepV3 Ti = svNorm( svAdd( svScale( T[i-1], Scalar(1)-f ), svScale( T[i], f ) ) );
				SweepV3 Bi = svAdd( svScale( B[i-1], Scalar(1)-f ), svScale( B[i], f ) );
				Bi = svNorm( svSub( Bi, svScale( Ti, svDot( Bi, Ti ) ) ) );
				const SweepV3 Ni = svCross( Bi, Ti );
				// slant rotates the (B, T) pair about N
				SweepV3 Bs = svAdd( svScale( Bi, cs ),  svScale( Ti, -sn ) );
				SweepV3 Ts = svAdd( svScale( Bi, sn ),  svScale( Ti, cs ) );
				const int base = vc;
				for( size_t v = 0; v < tVerts.size(); v++ ) {
					const Scalar lx = tVerts[v].x * desc.scale;
					const Scalar ly = tVerts[v].y * desc.scale;
					const Scalar lz = tVerts[v].z * desc.scale;
					pGeom->AddVertex( Vertex(
						P.x + lx * Bs.x + ly * Ts.x + lz * Ni.x,
						P.y + lx * Bs.y + ly * Ts.y + lz * Ni.y,
						P.z + lx * Bs.z + ly * Ts.z + lz * Ni.z ) );
					const Vector3 tn = ( v < tNorms.size() ) ? tNorms[v] : Vector3( 0, 0, 1 );
					pGeom->AddNormal( Normal(
						tn.x * Bs.x + tn.y * Ts.x + tn.z * Ni.x,
						tn.x * Bs.y + tn.y * Ts.y + tn.z * Ni.y,
						tn.x * Bs.z + tn.y * Ts.z + tn.z * Ni.z ) );
					pGeom->AddTexCoord( v < tCoords.size() ? tCoords[v] : TexCoord( 0.5, 0.5 ) );
					vc++;
				}
				for( size_t t = 0; t < tTris.size(); t++ ) {
					pGeom->AddIndexedTriangle( MakeIndexedTriangleSameIdx(
						base + tTris[t].iVertices[0],
						base + tTris[t].iVertices[1],
						base + tTris[t].iVertices[2] ) );
				}
				placed++;
				nextAt += desc.pitch;
			}
		}

		if( placed == 0 ) {
			GlobalLog()->Print( eLog_Error, "RISE_API_CreatePathInstancesGeometry: path shorter than the first instance position (phase) -- refusing an empty mesh" );
			pGeom->release();
			return false;
		}
		if( placed >= kMaxInstances ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"RISE_API_CreatePathInstancesGeometry: instance cap (%u) reached (vertex budget %u / template %u verts) -- pitch too small for the path length?",
				kMaxInstances, kMaxTotalVerts, (unsigned int)tVerts.size() );
		}

		pGeom->DoneIndexedTriangles();
		*ppi = pGeom;
		return true;
	}


}


///////////////////////////////////////////////////////////
// UV generators
//////////////////////////////////////////////////////////

#include "Geometry/BoxUVGenerator.h"
#include "Geometry/CylindricalUVGenerator.h"
#include "Geometry/SphericalUVGenerator.h"

namespace RISE
{
	//! Creates a box UV generator
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBoxUVGenerator(
								IUVGenerator** ppi,				///< [out] Pointer to recieve the UV generator
								const Scalar width,				///< [in] Width
								const Scalar height,			///< [in] Height
								const Scalar depth				///< [in] Depth
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new BoxUVGenerator( width, height, depth );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "box UV" );
		return true;
	}

	//! Creates a cylindrical UV generator
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCylindricalUVGenerator(
								IUVGenerator** ppi,				///< [out] Pointer to recieve the UV generator
								const Scalar radius,			///< [in] Radius
								const char axis,				///< [in] (x|y|z) Primary axis
								const Scalar height				///< [in] Size
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new CylindricalUVGenerator( radius, axis, height );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "cylinder UV" );
		return true;
	}


	//! Creates a spherical UV generator
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSphericalUVGenerator(
								IUVGenerator** ppi,				///< [out] Pointer to recieve the UV generator
								const Scalar radius				///< [in] Radius
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new SphericalUVGenerator( radius );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "spherical UV" );
		return true;
	}
}


//////////////////////////////////////////////////////////
// Materials
//////////////////////////////////////////////////////////

#include "Materials/Material.h"
#include "Materials/LambertianMaterial.h"
#include "Materials/LambertianLuminaireMaterial.h"
#include "Materials/PhongLuminaireMaterial.h"
#include "Materials/PolishedMaterial.h"
#include "Materials/DielectricMaterial.h"
#include "Materials/SubSurfaceScatteringMaterial.h"
#include "Materials/RandomWalkSSSMaterial.h"
#include "Materials/PerfectReflectorMaterial.h"
#include "Materials/PerfectRefractorMaterial.h"
#include "Materials/AshikminShirleyAnisotropicPhongMaterial.h"
#include "Materials/IsotropicPhongMaterial.h"
#include "Materials/TranslucentMaterial.h"
#include "Materials/BioSpecSkinMaterial.h"
#include "Materials/DonnerJensenSkinBSSRDFMaterial.h"
#include "Materials/GenericHumanTissueMaterial.h"
#include "Materials/CompositeMaterial.h"
#include "Materials/WardIsotropicGaussianMaterial.h"
#include "Materials/WardAnisotropicEllipticalGaussianMaterial.h"
#include "Materials/CookTorranceMaterial.h"
#include "Materials/GGXMaterial.h"
#include "Painters/UniformColorPainter.h"		// Landing 7 / 8: PBR-MR painter graph helpers
#include "Painters/BlendPainter.h"				// Landing 7 / 8: PBR-MR painter graph helpers
#include "Painters/UniformScalarPainter.h"
#include "Painters/PainterToScalarAdapter.h"	// Bridge IPainter chains into IScalarPainter slots
#include "Materials/SheenMaterial.h"
#include "Materials/OrenNayarMaterial.h"
#include "Materials/SchlickMaterial.h"
#include "Materials/DataDrivenMaterial.h"

namespace RISE
{
	//! Creates the NULL material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateNullMaterial(
								IMaterial** ppi				///< [out] Pointer to recieve the material
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new NullMaterial();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "null material" );
		return true;
	}

	//! Creates Lambertian material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateLambertianMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& ref				///< [in] Reflectance
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new LambertianMaterial( ref );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "lambertian material" );
		return true;
	}

	//! Creates a Polished material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePolishedMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& ref,			///< [in] Reflectance of diffuse substrate
								const IScalarPainter& tau,		///< [in] Transmittance of the dielectric top
								const IScalarPainter& Nt,		///< [in] Index of refraction of dielectric coating
								const IScalarPainter& scat,		///< [in] Scattering function (Phong cone or HG asymmetry)
								const bool hg					///< [in] Use Henyey-Greenstein phase function scattering
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new PolishedMaterial( ref, tau, Nt, scat, hg );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "polished material" );
		return true;
	}

	//! Creates a Dielectric material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDielectricMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IScalarPainter& tau,
								const IScalarPainter& rIndex,
								const IScalarPainter& scat,
								const bool hg,
								const Scalar arN,
								const Scalar arK,
								const Scalar arThickness
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new DielectricMaterial( tau, rIndex, scat, hg, arN, arK, arThickness );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "dielectric material" );
		return true;
	}

	//! Creates a SubSurface Scattering material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSubSurfaceScatteringMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IScalarPainter& ior,		///< [in] Index of refraction
								const IScalarPainter& absorption,///< [in] Absorption coefficient
								const IScalarPainter& scattering,///< [in] Scattering coefficient
								const Scalar g,					///< [in] HG asymmetry parameter
								const Scalar roughness			///< [in] Surface roughness [0,1]
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new SubSurfaceScatteringMaterial( ior, absorption, scattering, g, roughness );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "subsurface scattering material" );
		return true;
	}

	//! Creates a Random Walk SSS material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateRandomWalkSSSMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IScalarPainter& ior,		///< [in] Index of refraction
								const IScalarPainter& absorption,///< [in] Absorption coefficient
								const IScalarPainter& scattering,///< [in] Scattering coefficient
								const Scalar g,					///< [in] HG asymmetry parameter
								const Scalar roughness,			///< [in] Surface roughness [0,1]
								const unsigned int maxBounces	///< [in] Maximum walk steps
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new RandomWalkSSSMaterial( ior, absorption, scattering, g, roughness, maxBounces );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "random walk SSS material" );
		return true;
	}

	//! Creates an isotropic phong material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateIsotropicPhongMaterial(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& rd,					///< [in] Diffuse reflectance
								const IPainter& rs,					///< [in] Specular reflectance
								const IScalarPainter& exponent		///< [in] Phong exponent (physical scalar)
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new IsotropicPhongMaterial( rd, rs, exponent );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "isotropic phong material" );
		return true;
	}

	//! Creates the anisotropic phong material of Ashikmin and Shirley
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAshikminShirleyAnisotropicPhongMaterial(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& rd,					///< [in] Diffuse reflectance
								const IPainter& rs,					///< [in] Specular reflectance
								const IScalarPainter& Nu,			///< [in] Phong exponent in U (physical scalar)
								const IScalarPainter& Nv			///< [in] Phong exponent in V (physical scalar)
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new AshikminShirleyAnisotropicPhongMaterial( Nu, Nv, rd, rs );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "anisotropic phong material" );
		return true;
	}

	//! Creates a perfect reflector
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePerfectReflectorMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& ref				///< [in] Reflectance
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new PerfectReflectorMaterial( ref );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "perfect reflector material" );
		return true;
	}

	//! Creates a perfect refractor
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePerfectRefractorMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& ref,			///< [in] Amount of refraction
								const IScalarPainter& ior		///< [in] Index of refraction
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new PerfectRefractorMaterial( ref, ior );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "perfect refractor material" );
		return true;
	}

	//! Creates a translucent material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTranslucentMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& rF,				///< [in] Reflectance (color)
								const IPainter& T,				///< [in] Transmittance (color)
								const IScalarPainter& ext,		///< [in] Extinction (scalar)
								const IScalarPainter& N,		///< [in] Phong exponent (scalar)
								const IScalarPainter& scat		///< [in] Multiple scattering component (scalar)
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TranslucentMaterial( rF, T, ext, N, scat );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "translucent material" );
		return true;
	}

	//! Creates a BioSpec skin material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBioSpecSkinMaterial(
								IMaterial** ppi,											///< [out] Pointer to recieve the material
								const IScalarPainter& thickness_SC_,								///< Thickness of the stratum corneum (in cm)
								const IScalarPainter& thickness_epidermis_,						///< Thickness of the epidermis (in cm)
								const IScalarPainter& thickness_papillary_dermis_,				///< Thickness of the papillary dermis (in cm)
								const IScalarPainter& thickness_reticular_dermis_,				///< Thickness of the reticular dermis (in cm)
								const IScalarPainter& ior_SC_,									///< Index of refraction of the stratum corneum
								const IScalarPainter& ior_epidermis_,								///< Index of refraction of the epidermis
								const IScalarPainter& ior_papillary_dermis_,						///< Index of refraction of the papillary dermis
								const IScalarPainter& ior_reticular_dermis_,						///< Index of refraction of the reticular dermis
								const IScalarPainter& concentration_eumelanin_,					///< Average Concentration of eumelanin in the melanosomes
								const IScalarPainter& concentration_pheomelanin_,					///< Average Concentration of pheomelanin in the melanosomes
								const IScalarPainter& melanosomes_in_epidermis_,					///< Percentage of the epidermis made up of melanosomes
								const IScalarPainter& hb_ratio_,									///< Ratio of oxyhemoglobin to deoxyhemoglobin in blood
								const IScalarPainter& whole_blood_in_papillary_dermis_,			///< Percentage of the papillary dermis made up of whole blood
								const IScalarPainter& whole_blood_in_reticular_dermis_,			///< Percentage of the reticular dermis made up of whole blood
								const IScalarPainter& bilirubin_concentration_,					///< Concentration of Bilirubin in whole blood
								const IScalarPainter& betacarotene_concentration_SC_,				///< Concentration of Beta-Carotene in the stratum corneum
								const IScalarPainter& betacarotene_concentration_epidermis_,		///< Concentration of Beta-Carotene in the epidermis
								const IScalarPainter& betacarotene_concentration_dermis_,			///< Concentration of Beta-Carotene in the dermis
								const IScalarPainter& folds_aspect_ratio_,						///< Aspect ratio of the little folds and wrinkles on the skin surface
								const bool bSubdermalLayer									///< Should the model simulate a perfectly reflecting subdermal layer?
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new BioSpecSkinMaterial(
			thickness_SC_,
			thickness_epidermis_,
			thickness_papillary_dermis_,
			thickness_reticular_dermis_,
			ior_SC_,
			ior_epidermis_,
			ior_papillary_dermis_,
			ior_reticular_dermis_,
			concentration_eumelanin_,
			concentration_pheomelanin_,
			melanosomes_in_epidermis_,
			hb_ratio_,
			whole_blood_in_papillary_dermis_,
			whole_blood_in_reticular_dermis_,
			bilirubin_concentration_,
			betacarotene_concentration_SC_,
			betacarotene_concentration_epidermis_,
			betacarotene_concentration_dermis_,
			folds_aspect_ratio_,
			bSubdermalLayer
			);
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "biospec skin material" );
		return true;
	}

	bool RISE_API_CreateDonnerJensenSkinBSSRDFMaterial(
								IMaterial** ppi,
								const IScalarPainter& melanin_fraction_,
								const IScalarPainter& melanin_blend_,
								const IScalarPainter& hemoglobin_epidermis_,
								const IScalarPainter& carotene_fraction_,
								const IScalarPainter& hemoglobin_dermis_,
								const IScalarPainter& epidermis_thickness_,
								const IScalarPainter& ior_epidermis_,
								const IScalarPainter& ior_dermis_,
								const IScalarPainter& blood_oxygenation_,
								const Scalar roughness
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new DonnerJensenSkinBSSRDFMaterial(
			melanin_fraction_,
			melanin_blend_,
			hemoglobin_epidermis_,
			carotene_fraction_,
			hemoglobin_dermis_,
			epidermis_thickness_,
			ior_epidermis_,
			ior_dermis_,
			blood_oxygenation_,
			roughness
			);
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "donner jensen skin bssrdf material" );
		return true;
	}

	//! Creates a generic human tissue material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGenericHumanTissueMaterial(
								IMaterial** ppi,											///< [out] Pointer to recieve the material
								const IScalarPainter& sca,									///< [in] Scattering co-efficient (scalar)
								const IScalarPainter& g,									///< [in] HG phase function g (scalar)
								const Scalar whole_blood_,									///< Percentage of the tissue made up of whole blood
								const Scalar hb_ratio_,										///< Ratio of oxyhemoglobin to deoxyhemoglobin in blood
								const Scalar bilirubin_concentration_,						///< Concentration of Bilirubin in whole blood
								const Scalar betacarotene_concentration_,					///< Concentration of Beta-Carotene in whole blood
								const bool diffuse											///< Is the tissue just completely diffuse?
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new GenericHumanTissueMaterial(
			sca,
			g,
			whole_blood_,
			betacarotene_concentration_,
			bilirubin_concentration_,
			hb_ratio_,
			diffuse );

		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "generic human tissue material" );
		return true;
	}

	//! Creates Composite material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCompositeMaterial(
								IMaterial** ppi,											///< [out] Pointer to recieve the material
								const IMaterial& top,										///< [in] Material on top
								const IMaterial& bottom,									///< [in] Material on the bottom
								const unsigned int max_recur,								///< [in] Maximum recursion level in the random walk process
								const unsigned int max_reflection_recursion,				///< [in] Maximum level of reflection recursion
								const unsigned int max_refraction_recursion,				///< [in] Maximum level of refraction recursion
								const unsigned int max_diffuse_recursion,					///< [in] Maximum level of diffuse recursion
								const unsigned int max_translucent_recursion,				///< [in] Maximum level of translucent recursion
								const Scalar thickness,										///< [in] Thickness between the materials
								const IPainter& extinction									///< [in] Extinction coefficient for absorption between layers
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new CompositeMaterial( top, bottom, max_recur, max_reflection_recursion, max_refraction_recursion, max_diffuse_recursion, max_translucent_recursion, thickness, extinction );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "composite material" );
		return true;
	}

	//! Creates Ward's isotropic gaussian material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateWardIsotropicGaussianMaterial(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& diffuse,			///< [in] Diffuse reflectance
								const IPainter& specular,			///< [in] Specular reflectance
								const IScalarPainter& alpha			///< [in] Surface slope RMS (physical scalar)
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new WardIsotropicGaussianMaterial( diffuse, specular, alpha );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "ward isotropic gaussian material" );
		return true;
	}

	//! Creates Ward's anisotropic elliptical gaussian material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateWardAnisotropicEllipticalGaussianMaterial(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& diffuse,			///< [in] Diffuse reflectance
								const IPainter& specular,			///< [in] Specular reflectance
								const IScalarPainter& alphax,		///< [in] Surface slope RMS in x (physical scalar)
								const IScalarPainter& alphay		///< [in] Surface slope RMS in y (physical scalar)
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new WardAnisotropicEllipticalGaussianMaterial( diffuse, specular, alphax, alphay );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "ward anisotropic elliptical gaussian material" );
		return true;
	}

	// Forward declarations for the thin-film factories defined just below:
	// RISE_API.cpp includes its own header at the BOTTOM of the file, so the
	// ABI-preserving wrappers (which delegate to these) cannot see the header
	// declarations yet at this point.  Signatures must match RISE_API.h exactly.
	bool RISE_API_CreateGGXMaterialThinFilm(
								IMaterial** ppi, const IPainter& diffuse, const IPainter& specular,
								const IScalarPainter& alphaX, const IScalarPainter& alphaY,
								const IScalarPainter& ior, const IScalarPainter& ext,
								const FresnelMode fresnel_mode, const IPainter* tangent_rotation,
								const IScalarPainter* film_ior, const IScalarPainter* film_extinction,
								const IScalarPainter* film_thickness );
	bool RISE_API_CreateGGXEmissiveMaterialThinFilm(
								IMaterial** ppi, const IPainter& diffuse, const IPainter& specular,
								const IScalarPainter& alphaX, const IScalarPainter& alphaY,
								const IScalarPainter& ior, const IScalarPainter& ext,
								const IPainter* emissive, const Scalar emissive_scale,
								const FresnelMode fresnel_mode, const IPainter* tangent_rotation,
								const IScalarPainter* film_ior, const IScalarPainter* film_extinction,
								const IScalarPainter* film_thickness );

	//! Creates a GGX anisotropic microfacet material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGGXMaterial(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& diffuse,			///< [in] Diffuse reflectance
								const IPainter& specular,			///< [in] Specular reflectance
								const IScalarPainter& alphaX,		///< [in] Roughness in tangent u direction (physical scalar)
								const IScalarPainter& alphaY,		///< [in] Roughness in tangent v direction (physical scalar)
								const IScalarPainter& ior,			///< [in] Index of refraction (physical scalar)
								const IScalarPainter& ext,			///< [in] Extinction coefficient (physical scalar)
								const FresnelMode fresnel_mode,		///< [in] Fresnel evaluation model
								const IPainter* tangent_rotation	///< [in] Landing 8 / KHR_materials_anisotropy
								)
	{
		// ABI-preserving wrapper: this symbol's signature is frozen for
		// out-of-tree callers; the thin-film evolution lives in the new
		// *ThinFilm overload, which this delegates to with NULL film slots
		// (NULL film painters == the exact pre-thin-film behaviour).
		return RISE_API_CreateGGXMaterialThinFilm(
			ppi, diffuse, specular, alphaX, alphaY, ior, ext,
			fresnel_mode, tangent_rotation, nullptr, nullptr, nullptr );
	}

	//! Creates a GGX anisotropic microfacet material with thin-film slots.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGGXMaterialThinFilm(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& diffuse,			///< [in] Diffuse reflectance
								const IPainter& specular,			///< [in] Specular reflectance / F0 / thin-film tint
								const IScalarPainter& alphaX,		///< [in] Roughness in tangent u direction (physical scalar)
								const IScalarPainter& alphaY,		///< [in] Roughness in tangent v direction (physical scalar)
								const IScalarPainter& ior,			///< [in] Substrate IOR (physical scalar)
								const IScalarPainter& ext,			///< [in] Substrate extinction (physical scalar)
								const FresnelMode fresnel_mode,		///< [in] Fresnel evaluation model
								const IPainter* tangent_rotation,	///< [in] Landing 8 / KHR_materials_anisotropy
								const IScalarPainter* film_ior,		///< [in] Thin-film oxide n; NULL = no film
								const IScalarPainter* film_extinction,	///< [in] Thin-film oxide k; NULL = transparent film
								const IScalarPainter* film_thickness	///< [in] Thin-film oxide thickness in nm
								)
	{
		if( !ppi ) {
			return false;
		}

		// Thin-film mode REQUIRES the oxide n (film_ior) and thickness; both are
		// dereferenced unconditionally by the BSDF and have no sensible default
		// (unlike film_extinction, which defaults to k=0).  Reject the invalid
		// combination here rather than crash on the first shade -- the scene path
		// is guarded by Job's presence contract; this guards the direct-API path.
		if( fresnel_mode == eFresnelThinFilmConductor && ( film_ior == nullptr || film_thickness == nullptr ) ) {
			return false;
		}
		
		(*ppi) = new GGXMaterial( diffuse, specular, alphaX, alphaY, ior, ext, fresnel_mode, tangent_rotation, film_ior, film_extinction, film_thickness );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "ggx material" );
		return true;
	}

	//! Creates a GGX material with an optional LambertianEmitter folded in.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGGXEmissiveMaterial(
								IMaterial** ppi,
								const IPainter& diffuse,
								const IPainter& specular,
								const IScalarPainter& alphaX,
								const IScalarPainter& alphaY,
								const IScalarPainter& ior,
								const IScalarPainter& ext,
								const IPainter* emissive,
								const Scalar    emissive_scale,
								const FresnelMode fresnel_mode,
								const IPainter* tangent_rotation	///< [in] Landing 8 / KHR_materials_anisotropy
								)
	{
		// ABI-preserving wrapper — see RISE_API_CreateGGXMaterial above.
		return RISE_API_CreateGGXEmissiveMaterialThinFilm(
			ppi, diffuse, specular, alphaX, alphaY, ior, ext, emissive, emissive_scale,
			fresnel_mode, tangent_rotation, nullptr, nullptr, nullptr );
	}

	//! Thin-film-aware GGX-emissive factory.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGGXEmissiveMaterialThinFilm(
								IMaterial** ppi,
								const IPainter& diffuse,
								const IPainter& specular,
								const IScalarPainter& alphaX,
								const IScalarPainter& alphaY,
								const IScalarPainter& ior,
								const IScalarPainter& ext,
								const IPainter* emissive,
								const Scalar    emissive_scale,
								const FresnelMode fresnel_mode,
								const IPainter* tangent_rotation,	///< [in] Landing 8 / KHR_materials_anisotropy
								const IScalarPainter* film_ior,		///< [in] Thin-film oxide n; NULL = no film
								const IScalarPainter* film_extinction,	///< [in] Thin-film oxide k; NULL = transparent film
								const IScalarPainter* film_thickness	///< [in] Thin-film oxide thickness in nm
								)
	{
		if( !ppi ) {
			return false;
		}

		// Thin-film mode REQUIRES the oxide n (film_ior) and thickness; both are
		// dereferenced unconditionally by the BSDF and have no sensible default
		// (unlike film_extinction, which defaults to k=0).  Reject the invalid
		// combination here rather than crash on the first shade -- the scene path
		// is guarded by Job's presence contract; this guards the direct-API path.
		if( fresnel_mode == eFresnelThinFilmConductor && ( film_ior == nullptr || film_thickness == nullptr ) ) {
			return false;
		}
		
		(*ppi) = new GGXMaterial( diffuse, specular, alphaX, alphaY, ior, ext, emissive, emissive_scale, fresnel_mode, tangent_rotation, film_ior, film_extinction, film_thickness );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "ggx emissive material" );
		return true;
	}

	//! Creates a glTF-spec pbrMetallicRoughness material as a
	//! self-contained painter graph + final GGX.  Mirrors
	//! Job::AddPBRMetallicRoughnessMaterial's chain composition but
	//! without the Job-level painter manager — every internal helper
	//! is held alive via refcounts on the chain (each BlendPainter
	//! addrefs its inputs; GGXMaterial addrefs the chain head).  The
	//! caller's input painters keep their original refcounts; we
	//! release every locally-allocated helper before returning.
	bool RISE_API_CreatePBRMetallicRoughnessMaterial(
		IMaterial** ppi,
		const IPainter& base_color,
		const IPainter& metallic,
		const IPainter& roughness,
		const IPainter* emissive,
		const Scalar    emissive_scale,
		const IPainter* specular_factor,
		const IPainter* specular_color,
		const IPainter* anisotropy_factor,
		const IPainter* anisotropy_rotation
		)
	{
		if( !ppi ) {
			return false;
		}

		// Constants: zero, white, 0.04 (dielectric F0 baseline).
		IPainter* nZero    = new UniformColorPainter( RISEPel( 0, 0, 0 ) );  nZero->addref();
		IPainter* nWhite   = new UniformColorPainter( RISEPel( 1, 1, 1 ) );  nWhite->addref();
		IPainter* nPoint04 = new UniformColorPainter( RISEPel( 0.04, 0.04, 0.04 ) );  nPoint04->addref();

		// one_minus_met = blend(zero, white, metallic) = white * (1 - metallic).
		IPainter* nOneMinusMet = new BlendPainter( *nZero, *nWhite, metallic );  nOneMinusMet->addref();

		// rd = blend(base_color, zero, one_minus_met) = base_color * (1 - metallic).
		IPainter* nRd = new BlendPainter( base_color, *nZero, *nOneMinusMet );  nRd->addref();

		// Landing 7 — KHR_materials_specular F0 chain.  Defaults preserve
		// the standard 0.04 dielectric F0 bit-identically.
		IPainter* nF0Diel = 0;
		IPainter* nF0Aux  = 0;	// kept for refcount scope; released after consumption
		const bool hasSpec = ( specular_factor != nullptr || specular_color != nullptr );
		if( !hasSpec ) {
			nF0Diel = new UniformColorPainter( RISEPel( 0.04, 0.04, 0.04 ) );
			nF0Diel->addref();
		} else {
			const IPainter& specCol = specular_color ? *specular_color : *nWhite;
			const IPainter& specFct = specular_factor ? *specular_factor : *nWhite;
			// f0_aux = blend(specular_color, zero, point04) = specular_color * 0.04.
			nF0Aux = new BlendPainter( specCol, *nZero, *nPoint04 );  nF0Aux->addref();
			// f0_diel = blend(f0_aux, zero, specular_factor) = 0.04 * specCol * specFct.
			nF0Diel = new BlendPainter( *nF0Aux, *nZero, specFct );  nF0Diel->addref();
		}

		// rs = blend(base_color, f0_diel, metallic) = lerp(F0_dielectric, baseColor, metallic).
		IPainter* nRs = new BlendPainter( base_color, *nF0Diel, metallic );  nRs->addref();

		// rough_sq = roughness²
		IPainter* nRoughSq = new BlendPainter( roughness, *nZero, roughness );  nRoughSq->addref();

		// Landing 8 — KHR_materials_anisotropy α-split.  Defaults preserve
		// αx = αy = roughness² bit-identically.
		IPainter* nAlphaX_owned = 0;	// allocated only when anisotropy is set
		IPainter* nAnisoSq      = 0;
		const IPainter* alphaX = nRoughSq;
		const IPainter* alphaY = nRoughSq;
		if( anisotropy_factor ) {
			// aniso_sq = blend(aniso, zero, aniso) = aniso²
			nAnisoSq = new BlendPainter( *anisotropy_factor, *nZero, *anisotropy_factor );  nAnisoSq->addref();
			// alpha_t = blend(white, rough_sq, aniso_sq) = mix(roughness², 1, aniso²)
			nAlphaX_owned = new BlendPainter( *nWhite, *nRoughSq, *nAnisoSq );  nAlphaX_owned->addref();
			alphaX = nAlphaX_owned;
		}

		// IOR (scalar) — preserved for the GGX API; ignored under schlick_f0.
		UniformScalarPainter* nIORscalar = new UniformScalarPainter( Scalar( 1.5 ) );
		nIORscalar->addref();
		// Extinction (scalar) — zero under the schlick_f0 path.
		UniformScalarPainter* nExtScalar = new UniformScalarPainter( Scalar( 0.0 ) );
		nExtScalar->addref();

		// alphaX / alphaY come from a `BlendPainter` chain that varies
		// spatially via the roughness texture.  Adapt the chain to
		// IScalarPainter so it can plug into GGXMaterial's converted
		// alpha slots (channel 0 read; per-channel variation flagged).
		PainterToScalarAdapter* alphaXadapter = new PainterToScalarAdapter( *alphaX );
		alphaXadapter->addref();
		PainterToScalarAdapter* alphaYadapter = new PainterToScalarAdapter( *alphaY );
		alphaYadapter->addref();

		// Final GGX with the chain.  GGXMaterial addrefs each input; we
		// release locals after construction so the chain is self-supporting.
		(*ppi) = new GGXMaterial(
			*nRd, *nRs, *alphaXadapter, *alphaYadapter, *nIORscalar, *nExtScalar,
			emissive, emissive_scale, eFresnelSchlickF0, anisotropy_rotation );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "pbr-mr material" );

		// Release locals.  GGXMaterial's BSDF/SPF retain refs on the
		// adapted slots (which in turn keep the underlying IPainter
		// chains alive).
		safe_release( alphaXadapter );
		safe_release( alphaYadapter );
		safe_release( nIORscalar );
		safe_release( nExtScalar );
		if( nAlphaX_owned ) safe_release( nAlphaX_owned );
		if( nAnisoSq )      safe_release( nAnisoSq );
		safe_release( nRoughSq );
		safe_release( nRs );
		safe_release( nF0Diel );
		if( nF0Aux ) safe_release( nF0Aux );
		safe_release( nRd );
		safe_release( nOneMinusMet );
		safe_release( nPoint04 );
		safe_release( nWhite );
		safe_release( nZero );

		return true;
	}

	//! Creates a Charlie / Neubelt sheen material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSheenMaterial(
								IMaterial** ppi,
								const IPainter& sheenColor,
								const IScalarPainter& sheenRoughness
								)
	{
		if( !ppi ) {
			return false;
		}
		(*ppi) = new SheenMaterial( sheenColor, sheenRoughness );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "sheen material" );
		return true;
	}

	//! Creates a Cook Torrance material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCookTorranceMaterial(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& diffuse,			///< [in] Diffuse reflectance
								const IPainter& specular,			///< [in] Specular reflectance
								const IScalarPainter& facet,		///< [in] Facet distribution (roughness — physical scalar)
								const IScalarPainter& ior,			///< [in] Index of refraction (physical scalar)
								const IScalarPainter& ext			///< [in] Extinction coefficient (physical scalar)
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new CookTorranceMaterial( diffuse, specular, facet, ior, ext );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "cook torrance material" );
		return true;
	}

	//! Creates a Oren-Nayar material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateOrenNayarMaterial(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& reflectance,		///< [in] Reflectance
								const IScalarPainter& roughness		///< [in] Roughness (physical scalar)
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new OrenNayarMaterial( reflectance, roughness );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "oren nayar material" );
		return true;
	}

	//! Creates a Schlick material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSchlickMaterial(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& diffuse,			///< [in] Diffuse reflectance
								const IPainter& specular,			///< [in] Specular reflectance
								const IScalarPainter& roughness,	///< [in] Roughness (physical scalar)
								const IScalarPainter& isotropy		///< [in] Isotropy (physical scalar)
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new SchlickMaterial( diffuse, specular, roughness, isotropy );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "schlick material" );
		return true;
	}

	//! Creates a datadriven material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDataDrivenMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const char* filename			///< [in] Filename to load data from
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new DataDrivenMaterial( filename );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "data driven material" );
		return true;
	}

	//! Creates a lambertian luminaire material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateLambertianLuminaireMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& radEx,			///< [in] Radiant exitance
								const IMaterial& mat,			///< [in] Material to use for all non emmission properties
								const Scalar scale				///< [in] Value to scale radiant exitance by
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new LambertianLuminaireMaterial( radEx, scale, mat );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "lambertian luminaire material" );
		return true;
	}

	//! Creates a phong luminaire material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePhongLuminaireMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& radEx,			///< [in] Radiance exitance (color)
								const IMaterial& mat,			///< [in] Material to use for all non emmission properties
								const IScalarPainter& N,		///< [in] Phong exponent (scalar)
								const Scalar scale				///< [in] Value to scale radiant exitance by
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new PhongLuminaireMaterial( radEx, scale, N, mat );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "phong luminaire material" );
		return true;
	}

}

//////////////////////////////////////////////////////////
// Painters
//////////////////////////////////////////////////////////

#include "Painters/UniformColorPainter.h"
#include "Painters/VertexColorPainter.h"
#include "Painters/SpectralColorPainter.h"
#include "Painters/TexturePainter.h"
#include "Painters/CheckerPainter.h"
#include "Painters/LinesPainter.h"
#include "Painters/MandelbrotPainter.h"
#include "Painters/Perlin2DPainter.h"
#include "Painters/ControlledSmoothness2DPainter.h"
#include "Painters/CompositeFunction2DPainter.h"
#include "Painters/PolynomialFunction2DPainter.h"
#include "Painters/GerstnerWavePainter.h"
#include "Painters/Perlin3DPainter.h"
#include "Painters/Turbulence3DPainter.h"
#include "Painters/Worley3DPainter.h"
#include "Painters/PerlinWorley3DPainter.h"
#include "Painters/DomainWarp3DPainter.h"
#include "Painters/CurlNoise3DPainter.h"
#include "Painters/SDF3DPainter.h"
#include "Painters/Simplex3DPainter.h"
#include "Painters/Gabor3DPainter.h"
#include "Painters/Wavelet3DPainter.h"
#include "Painters/ReactionDiffusion3DPainter.h"
#include "Painters/Voronoi2DPainter.h"
#include "Painters/Voronoi3DPainter.h"
#include "Painters/IridescentPainter.h"
#include "Painters/Function1DSpectralPainter.h"
#include "Painters/BlackBodyPainter.h"
#include "Painters/BlendPainter.h"
#include "Painters/ChannelPainter.h"
#include "Painters/UVTransformPainter.h"
#include "Painters/TexCoord1Painter.h"

namespace RISE
{
	//! Creates a checker painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCheckerPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const Scalar size,				///< [in] Size of the checkers in texture mapping units
								const IPainter& a,				///< [in] First color
								const IPainter& b				///< [in] Second color
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new CheckerPainter( size, a, b );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "checker painter" );
		return true;
	}


	//! Creates a lines painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateLinesPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const Scalar size,				///< [in] Size of the lines in texture mapping units
								const IPainter& a,				///< [in] First color
								const IPainter& b,				///< [in] Second color
								const bool bvert				///< [in] Are the lines vertical?
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new LinesPainter( size, a, b, bvert  );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "lines painter" );
		return true;
	}

	//! Creates a mandelbrot fractal painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMandelbrotFractalPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const IPainter& cA, 			///< [in] First color
								const IPainter& cB,				///< [in] Second color
								const Scalar lower_x,
								const Scalar upper_x,
								const Scalar lower_y,
								const Scalar upper_y,
								const Scalar exp
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new MandelbrotPainter( cA, cB, lower_x, upper_x, lower_y, upper_y, exp );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "mandelbrot painter" );
		return true;
	}

	//! Creates a 2D perlin noise painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePerlin2DPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const Scalar dPersistence,		///< [in] Persistence
								const unsigned int nOctaves,	///< [in] Number of octaves to use in noise generation
								const IPainter& cA, 			///< [in] First painter
								const IPainter& cB, 			///< [in] Second painter
								const Vector2& vScale,			///< [in] How much to scale the function by
								const Vector2& vShift			///< [in] How much to shift the function by
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new Perlin2DPainter( dPersistence, nOctaves, cA, cB, vScale, vShift  );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "perlin 2D painter" );
		return true;
	}

	//! Creates a controlled-smoothness radial-bump painter (test/diagnostic).
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateControlledSmoothness2DPainter(
								IPainter** ppi,
								const IPainter& cA,
								const IPainter& cB,
								const Scalar centerU,
								const Scalar centerV,
								const Scalar radius,
								const Scalar amplitude,
								const unsigned int smoothnessMode
								)
	{
		if( !ppi ) {
			return false;
		}

		// Map the integer to the painter's enum.  Unknown values fall
		// through to Gaussian as a safe smooth default — silently
		// truncating to the lowest-order mode would be more surprising.
		ControlledSmoothness2DPainter::SmoothnessMode mode;
		switch( smoothnessMode ) {
		case 0:  mode = ControlledSmoothness2DPainter::eHeaviside; break;
		case 1:  mode = ControlledSmoothness2DPainter::eTent;      break;
		case 2:  mode = ControlledSmoothness2DPainter::eQuadratic; break;
		case 3:  mode = ControlledSmoothness2DPainter::eCubic;     break;
		case 5:  mode = ControlledSmoothness2DPainter::eQuintic;   break;
		case 99: mode = ControlledSmoothness2DPainter::eGaussian;  break;
		default:
			GlobalLog()->PrintEx( eLog_Warning,
				"RISE_API_CreateControlledSmoothness2DPainter: unknown smoothness mode %u; using Gaussian",
				smoothnessMode );
			mode = ControlledSmoothness2DPainter::eGaussian;
			break;
		}

		(*ppi) = new ControlledSmoothness2DPainter( cA, cB, centerU, centerV, radius, amplitude, mode );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "controlled-smoothness 2D painter" );
		return true;
	}

	//! Creates a polynomial-based Function2D painter.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePolynomialFunction2DPainter(
								IPainter** ppi,
								const IPainter& cA,
								const IPainter& cB,
								const unsigned int polynomialType,
								const Scalar centerU,
								const Scalar centerV,
								const Scalar scaleU,
								const Scalar scaleV,
								const Scalar amplitude,
								const unsigned int degree,
								const unsigned int powerX,
								const unsigned int powerY,
								const Scalar* pCoeffs,
								const unsigned int nCoeffs
								)
	{
		if( !ppi ) {
			return false;
		}

		// Map the integer to the painter's enum.  Unknown values fall
		// through to RadialBump (the most innocuous default) with a
		// warning — silently dropping the chunk would be worse.
		PolynomialFunction2DPainter::PolynomialType resolvedType;
		switch( polynomialType ) {
		case 0: resolvedType = PolynomialFunction2DPainter::eRadialBump;       break;
		case 1: resolvedType = PolynomialFunction2DPainter::eMonomial;         break;
		case 2: resolvedType = PolynomialFunction2DPainter::eParaboloid;       break;
		case 3: resolvedType = PolynomialFunction2DPainter::eHyperbolicSaddle; break;
		case 4: resolvedType = PolynomialFunction2DPainter::eMonkeySaddle;     break;
		case 5: resolvedType = PolynomialFunction2DPainter::eBivariate;        break;
		default:
			GlobalLog()->PrintEx( eLog_Warning,
				"RISE_API_CreatePolynomialFunction2DPainter: unknown polynomial type %u; using radial_bump",
				polynomialType );
			resolvedType = PolynomialFunction2DPainter::eRadialBump;
			break;
		}

		(*ppi) = new PolynomialFunction2DPainter(
			cA, cB, resolvedType,
			centerU, centerV, scaleU, scaleV, amplitude,
			degree, powerX, powerY,
			pCoeffs, nCoeffs );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "polynomial function2d painter" );
		return true;
	}

	//! Creates a composable Function2D painter (binary operator on two operand Function2Ds).
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCompositeFunction2DPainter(
								IPainter** ppi,
								const IPainter& cA,
								const IPainter& cB,
								const IFunction2D& childA,
								const IFunction2D& childB,
								const unsigned int op,
								const Scalar weightA,
								const Scalar uvScaleAU,
								const Scalar uvScaleAV,
								const Scalar uvOffsetAU,
								const Scalar uvOffsetAV,
								const Scalar weightB,
								const Scalar uvScaleBU,
								const Scalar uvScaleBV,
								const Scalar uvOffsetBU,
								const Scalar uvOffsetBV,
								const Scalar lerpT,
								const Scalar outputScale,
								const Scalar outputOffset
								)
	{
		if( !ppi ) {
			return false;
		}

		// Map the integer to the painter's enum.  Unknown values fall
		// through to Sum (the most common case for layered displacement)
		// with a warning — silently dropping the chunk would be worse.
		CompositeFunction2DPainter::CompositeOp resolvedOp;
		switch( op ) {
		case 0: resolvedOp = CompositeFunction2DPainter::eSum;        break;
		case 1: resolvedOp = CompositeFunction2DPainter::eProduct;    break;
		case 2: resolvedOp = CompositeFunction2DPainter::eLerp;       break;
		case 3: resolvedOp = CompositeFunction2DPainter::eMax;        break;
		case 4: resolvedOp = CompositeFunction2DPainter::eMin;        break;
		case 5: resolvedOp = CompositeFunction2DPainter::eDifference; break;
		default:
			GlobalLog()->PrintEx( eLog_Warning,
				"RISE_API_CreateCompositeFunction2DPainter: unknown op %u; using Sum",
				op );
			resolvedOp = CompositeFunction2DPainter::eSum;
			break;
		}

		(*ppi) = new CompositeFunction2DPainter(
			cA, cB, childA, childB, resolvedOp,
			weightA, uvScaleAU, uvScaleAV, uvOffsetAU, uvOffsetAV,
			weightB, uvScaleBU, uvScaleBV, uvOffsetBU, uvOffsetBV,
			lerpT, outputScale, outputOffset );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "composite function2d painter" );
		return true;
	}

	//! Creates a sum-of-sines water-wave painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGerstnerWavePainter(
								IPainter** ppi,
								const IPainter& cA,
								const IPainter& cB,
								const unsigned int numWaves,
								const Scalar medianWavelength,
								const Scalar wavelengthRange,
								const Scalar medianAmplitude,
								const Scalar amplitudePower,
								const Scalar windDirX,
								const Scalar windDirY,
								const Scalar directionalSpread,
								const Scalar dispersionSpeed,
								const unsigned int seed,
								const Scalar time
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new GerstnerWavePainter(
			cA, cB,
			numWaves,
			medianWavelength, wavelengthRange,
			medianAmplitude, amplitudePower,
			windDirX, windDirY,
			directionalSpread,
			dispersionSpeed,
			seed,
			time );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "gerstner wave painter" );
		return true;
	}

	//! Creates a 2D perlin noise painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePerlin3DPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const Scalar dPersistence,		///< [in] Persistence
								const unsigned int nOctaves,	///< [in] Number of octaves to use in noise generation
								const IPainter& cA,				///< [in] First painter
								const IPainter& cB, 			///< [in] Second painter
								const Vector3& vScale,			///< [in] How much to scale the function by
								const Vector3& vShift			///< [in] How much to shift the function by
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new Perlin3DPainter( dPersistence, nOctaves, cA, cB, vScale, vShift  );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "perlin 3D painter" );
		return true;
	}

	//! Creates a 3D turbulence noise painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTurbulence3DPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const Scalar dPersistence,		///< [in] Persistence
								const unsigned int nOctaves,	///< [in] Number of octaves to use in noise generation
								const IPainter& cA,				///< [in] First painter
								const IPainter& cB, 			///< [in] Second painter
								const Vector3& vScale,			///< [in] How much to scale the function by
								const Vector3& vShift			///< [in] How much to shift the function by
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new Turbulence3DPainter( dPersistence, nOctaves, cA, cB, vScale, vShift  );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "turbulence 3D painter" );
		return true;
	}

	//! Creates a 3D Worley (cellular) noise painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateWorley3DPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const Scalar dJitter,			///< [in] Jitter amount [0,1]
								const unsigned int nMetric,		///< [in] Distance metric
								const unsigned int nOutput,		///< [in] Output mode
								const IPainter& cA,				///< [in] First painter
								const IPainter& cB, 			///< [in] Second painter
								const Vector3& vScale,			///< [in] How much to scale the function by
								const Vector3& vShift			///< [in] How much to shift the function by
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new Worley3DPainter( dJitter,
			(Implementation::WorleyDistanceMetric)nMetric,
			(Implementation::WorleyOutputMode)nOutput,
			cA, cB, vScale, vShift );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "worley 3D painter" );
		return true;
	}

	//! Creates a 3D Perlin-Worley hybrid (cloud noise) painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePerlinWorley3DPainter(
								IPainter** ppi,
								const Scalar dPersistence,
								const unsigned int nOctaves,
								const Scalar dWorleyJitter,
								const Scalar dBlend,
								const IPainter& cA,
								const IPainter& cB,
								const Vector3& vScale,
								const Vector3& vShift
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new PerlinWorley3DPainter( dPersistence, nOctaves, dWorleyJitter, dBlend, cA, cB, vScale, vShift );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "perlin-worley 3D painter" );
		return true;
	}

	bool RISE_API_CreateDomainWarp3DPainter(
								IPainter** ppi,
								const Scalar dPersistence,
								const unsigned int nOctaves,
								const Scalar dWarpAmplitude,
								const unsigned int nWarpLevels,
								const IPainter& cA,
								const IPainter& cB,
								const Vector3& vScale,
								const Vector3& vShift
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new DomainWarp3DPainter( dPersistence, nOctaves, dWarpAmplitude, nWarpLevels, cA, cB, vScale, vShift );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "domain warp 3D painter" );
		return true;
	}

	bool RISE_API_CreateCurlNoise3DPainter(
								IPainter** ppi,
								const Scalar dPersistence,
								const unsigned int nOctaves,
								const Scalar dEpsilon,
								const IPainter& cA,
								const IPainter& cB,
								const Vector3& vScale,
								const Vector3& vShift
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new CurlNoise3DPainter( dPersistence, nOctaves, dEpsilon, cA, cB, vScale, vShift );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "curl noise 3D painter" );
		return true;
	}

	bool RISE_API_CreateSDF3DPainter(
								IPainter** ppi,
								const unsigned int nType,
								const Scalar dParam1,
								const Scalar dParam2,
								const Scalar dParam3,
								const Scalar dShellThickness,
								const Scalar dNoiseAmplitude,
								const Scalar dNoiseFrequency,
								const IPainter& cA,
								const IPainter& cB,
								const Vector3& vScale,
								const Vector3& vShift
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new SDF3DPainter( (Implementation::SDFPrimitiveType)nType, dParam1, dParam2, dParam3, dShellThickness, dNoiseAmplitude, dNoiseFrequency, cA, cB, vScale, vShift );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "SDF 3D painter" );
		return true;
	}

	bool RISE_API_CreateSimplex3DPainter(
								IPainter** ppi,
								const Scalar dPersistence,
								const unsigned int nOctaves,
								const IPainter& cA,
								const IPainter& cB,
								const Vector3& vScale,
								const Vector3& vShift
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new Simplex3DPainter( dPersistence, nOctaves, cA, cB, vScale, vShift );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "simplex 3D painter" );
		return true;
	}

	bool RISE_API_CreateGabor3DPainter(
								IPainter** ppi,
								const Scalar dFrequency,
								const Scalar dBandwidth,
								const Vector3& vOrientation,
								const Scalar dImpulseDensity,
								const IPainter& cA,
								const IPainter& cB,
								const Vector3& vScale,
								const Vector3& vShift
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new Gabor3DPainter( dFrequency, dBandwidth, vOrientation, dImpulseDensity, cA, cB, vScale, vShift );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "gabor 3D painter" );
		return true;
	}

	bool RISE_API_CreateWavelet3DPainter(
								IPainter** ppi,
								const unsigned int nTileSize,
								const Scalar dPersistence,
								const unsigned int nOctaves,
								const IPainter& cA,
								const IPainter& cB,
								const Vector3& vScale,
								const Vector3& vShift
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new Wavelet3DPainter( nTileSize, dPersistence, nOctaves, cA, cB, vScale, vShift );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "wavelet 3D painter" );
		return true;
	}

	bool RISE_API_CreateReactionDiffusion3DPainter(
								IPainter** ppi,
								const unsigned int nGridSize,
								const Scalar dDa,
								const Scalar dDb,
								const Scalar dFeed,
								const Scalar dKill,
								const unsigned int nIterations,
								const IPainter& cA,
								const IPainter& cB,
								const Vector3& vScale,
								const Vector3& vShift
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ReactionDiffusion3DPainter( nGridSize, dDa, dDb, dFeed, dKill, nIterations, cA, cB, vScale, vShift );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "reaction-diffusion 3D painter" );
		return true;
	}


	//! Creates a spectral color painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSpectralColorPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const SpectralPacket& spectrum,	///< [in] Spectral packet
								const Scalar scale				///< [in] How much to scale the amplitudes by
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new SpectralColorPainter( spectrum, scale  );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "spectral color painter" );
		return true;
	}

	//! Creates a texture painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTexturePainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								IRasterImageAccessor* pSA,		///< [in] Raster Image accessor to the image containing the texture
								SpectrumKind kind				///< [in] Spectral-uplift role
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TexturePainter( pSA, kind );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "texture painter" );
		return true;
	}

	//! Creates a painter that paints a uniform color
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateUniformColorPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const RISEPel& c,				///< [in] Color to paint
								SpectrumKind kind				///< [in] Spectral-uplift role
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new UniformColorPainter( c, kind );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "uniform color painter" );
		return true;
	}

	bool RISE_API_CreateVertexColorPainter(
								IPainter** ppi,
								const RISEPel& fallback
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new VertexColorPainter( fallback );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "vertex color painter" );
		return true;
	}

	//! Creates a painter that paints a voronoi diagram
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateVoronoi2DPainter(
								IPainter**ppi,					///< [out] Pointer to recieve the painter
								const std::vector<Point2> pts,	///< [in] The locations of the generators
								const std::vector<IPainter*> p,	///< [in] The painters for the generators
								const IPainter& border,			///< [in] Painter for the border
								const Scalar bsize				///< [in] Size of the borders
								)
	{
		if( !ppi ) {
			return false;
		}

		if( pts.size() != p.size() ) {
			GlobalLog()->PrintEasyError( "RISE_API_CreateVoronoi2DPainter:: Size of points array and painters array does not match" );
			return false;
		}

		Voronoi2DPainter::GeneratorsList gl;

		stl_utils::shuffle( pts, p, gl );

		(*ppi) = new Voronoi2DPainter( gl, border, bsize );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "voronoi2D painter" );
		return true;
	}

	//! Creates a painter that paints a voronoi diagram in 3D
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateVoronoi3DPainter(
								IPainter**ppi,					///< [out] Pointer to recieve the painter
								const std::vector<Point3> pts,	///< [in] The locations of the generators
								const std::vector<IPainter*> p,	///< [in] The painters for the generators
								const IPainter& border,			///< [in] Painter for the border
								const Scalar bsize				///< [in] Size of the borders
								)
	{
		if( !ppi ) {
			return false;
		}

		if( pts.size() != p.size() ) {
			GlobalLog()->PrintEasyError( "RISE_API_CreateVoronoi3DPainter:: Size of points array and painters array does not match" );
			return false;
		}

		Voronoi3DPainter::GeneratorsList gl;

		stl_utils::shuffle( pts, p, gl );

		(*ppi) = new Voronoi3DPainter( gl, border, bsize );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "voronoi3D painter" );
		return true;
	}

	//! Creates a iridescent painter (a painter whose color changes as viewing angle changes)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateIridescentPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const IPainter& a,				///< [in] First color
								const IPainter& b,				///< [in] Second color
								const double bias				///< [in] Biases the iridescence to one color or another
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new IridescentPainter( a, b, bias );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "iridescent painter" );
		return true;
	}

	//! Creates a painter that paints a spectrum from a Function1D
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateFunction1DSpectralPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const IFunction1D& func			///< [in] Function to paint
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new Function1DSpectralPainter( func );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "function1d spectral painter" );
		return true;
	}

	bool RISE_API_CreateBlackBodyPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const Scalar temperature,		///< [in] Temperature of the radiator in Kelvins
								const Scalar lambda_begin,		///< [in] Where in the spectrum to start creating the spectral packet
								const Scalar lambda_end,		///< [in] Where in the spectrum to end creating the spectral packet
								const unsigned int num_freq,	///< [in] Number of frequencies to use in the spectral packet
								const bool normalize,			///< [in] Should the values be normalized to peak intensity?
								const Scalar scale				///< [in] Value to scale radiant exitance by
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new BlackBodyPainter( temperature, lambda_begin,lambda_end, num_freq, normalize, scale );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "blackbody painter" );
		return true;
	}

	//! Creates a channel-extraction painter.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateChannelPainter(
								IPainter** ppi,
								const IPainter& source,
								const char channel,
								const Scalar scale,
								const Scalar bias
								)
	{
		if( !ppi ) {
			return false;
		}
		(*ppi) = new ChannelPainter( source, (ChannelPainter::Channel)channel, scale, bias );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "channel painter" );
		return true;
	}

	//! Creates a blend painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBlendPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const IPainter& a,				///< [in] First color
								const IPainter& b,				///< [in] Second color
								const IPainter& mask			///< [in] Blend mask
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new BlendPainter( a, b, mask );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "blend painter" );
		return true;
	}

	//! Creates a TEXCOORD_1 selector painter (glTF L12.D)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTexCoord1Painter(
								IPainter** ppi,
								const IPainter& source
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TexCoord1Painter( source );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "texcoord1 painter" );
		return true;
	}

	//! Creates a UV-transform wrapper painter (glTF KHR_texture_transform)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateUVTransformPainter(
								IPainter** ppi,
								const IPainter& source,
								const Scalar offset_u,
								const Scalar offset_v,
								const Scalar rotation,
								const Scalar scale_u,
								const Scalar scale_v
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new UVTransformPainter( source, offset_u, offset_v, rotation, scale_u, scale_v );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "uv transform painter" );
		return true;
	}
}


//////////////////////////////////////////////////////////
// Radiance maps
//////////////////////////////////////////////////////////

#include "Rendering/RadianceMap.h"
#include "Rendering/HosekWilkieSpectralRadianceMap.h"

namespace RISE
{
	//! Creates a radiance map which is used for image based lighting
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateRadianceMap(
								IRadianceMap** ppi,				///< [out] Pointer to recieve the radiance map
								const IPainter& painter,		///< [in] Painter to use for the map
								const Scalar scale				///< [in] How much to scale the values in the map by
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new RadianceMap( painter, scale );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "radiance map" );
		return true;
	}

	//! Hosek-Wilkie analytic spectral sun-and-sky radiance map.
	//! See header for parameter semantics.
	bool RISE_API_CreateHosekWilkieRadianceMap(
								IRadianceMap** ppi,
								const Scalar solarElevationDegrees,
								const Scalar solarAzimuthDegrees,
								const Scalar turbidity,
								const RISEPel& groundAlbedo,
								const Scalar skyIntensityScale )
	{
		if( !ppi ) {
			return false;
		}

		const Scalar deg2rad = Scalar( M_PI / 180.0 );
		(*ppi) = new HosekWilkieSpectralRadianceMap(
			solarElevationDegrees * deg2rad,
			solarAzimuthDegrees   * deg2rad,
			turbidity,
			groundAlbedo,
			skyIntensityScale );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "hosek-wilkie radiance map" );
		return true;
	}
}

//////////////////////////////////////////////////////////
// Pixel filters
//////////////////////////////////////////////////////////

#include "Sampling/BoxPixelFilter.h"
#include "Sampling/TrianglePixelFilter.h"
#include "Sampling/GaussianPixelFilter.h"
#include "Sampling/SincPixelFilter.h"
#include "Sampling/WindowedSincPixelFilter.h"
#include "Sampling/CookPixelFilter.h"
#include "Sampling/MaxPixelFilter.h"
#include "Sampling/MitchellNetravaliPixelFilter.h"
#include "Sampling/LanczosPixelFilter.h"
#include "Sampling/CatmullRomPixelFilter.h"
#include "Sampling/CubicBSplinePixelFilter.h"
#include "Sampling/QuadraticBSplinePixelFilter.h"

namespace RISE
{
	//! Creates a box pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBoxPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new BoxPixelFilter( width, height );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "box filter" );
		return true;
	}

	//! Creates a triangle pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTrianglePixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TrianglePixelFilter( width, height );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "triangle filter" );
		return true;
	}

	//! Creates a gaussian pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGaussianPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar size,				///< [in] Size of the filter
								const Scalar sigma				///< [in] Distribution of the filter
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new GaussianPixelFilter( size, sigma );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "gaussian filter" );
		return true;
	}

	//! Creates a sinc pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSincPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar window,			///< [in] Window of the filter
								const Scalar scale				///< [in] Scale factor
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new SincPixelFilter( window, scale );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "sinc filter" );
		return true;
	}

	//! Creates a box windowed sinc pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBoxWindowedSincPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new BoxWindowSincPixelFilter( width, height );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "box windowed sinc filter" );
		return true;
	}

	//! Creates a bartlett windowed sinc pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBartlettWindowedSincPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new BarlettWindowSincPixelFilter( width, height );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "bartlett windowed sinc filter" );
		return true;
	}

	//! Creates a welch windowed sinc pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateWelchWindowedSincPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new WelchWindowSincPixelFilter( width, height );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "welch windowed sinc filter" );
		return true;
	}

	//! Creates a lanczos windowed sinc pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateLanczosWindowedSincPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new LanczosWindowSincPixelFilter( width, height );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "lanczos windowed sinc filter" );
		return true;
	}

	//! Creates a kaiser windowed sinc pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateKaiserWindowedSincPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height,			///< [in] Height of the filter
								const Scalar alpha
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new KaiserWindowSincPixelFilter( width, height, alpha );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "kaiser windowed sinc filter" );
		return true;
	}

	//! Creates a hanning windowed sinc pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateHanningWindowedSincPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new HanningWindowSincPixelFilter( width, height );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "hanning windowed sinc filter" );
		return true;
	}

	//! Creates a hamming windowed sinc pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateHammingWindowedSincPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new HammingWindowSincPixelFilter( width, height );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "hamming windowed sinc filter" );
		return true;
	}

	//! Creates a blackman windowed sinc pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBlackmanWindowedSincPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new BlackmanWindowSincPixelFilter( width, height );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "blackman windowed sinc filter" );
		return true;
	}

	//! Creates a Cook pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCookPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new CookPixelFilter( width, height );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "cook filter" );
		return true;
	}

	//! Creates a Max pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMaxPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height,			///< [in] Height of the filter
								const Scalar s_x,				///< [in] S paramter in x direction
								const Scalar s_y				///< [in] S paramter in y direction
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new MaxPixelFilter( width, height, s_x, s_y );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "max filter" );
		return true;
	}

	//! Creates a Mitchell-Netvravali pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMitchellNetravaliPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar b,					///< [in] b parameter
								const Scalar c					///< [in] c parameter
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new MitchellNetravaliPixelFilter( b, c );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "mitchell-netravali filter" );
		return true;
	}

	//! Creates a Lanczos pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateLanczosPixelFilter(
								IPixelFilter** ppi				///< [out] Pointer to recieve the pixel filter
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new LanczosPixelFilter();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "lanczos filter" );
		return true;
	}

	//! Creates a Catmull Rom pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCatmullRomPixelFilter(
								IPixelFilter** ppi				///< [out] Pointer to recieve the pixel filter
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new CatmullRomPixelFilter();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "catmull-rom filter" );
		return true;
	}

	//! Creates a Cubic B-Spline pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCubicBSplinePixelFilter(
								IPixelFilter** ppi				///< [out] Pointer to recieve the pixel filter
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new CubicBSplinePixelFilter();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "cubic bspline filter" );
		return true;
	}

	//! Creates a Quadratic B-Spline pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateQuadraticBSplinePixelFilter(
								IPixelFilter** ppi				///< [out] Pointer to recieve the pixel filter
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new QuadraticBSplinePixelFilter();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "quadratic bspline filter" );
		return true;
	}
}


//////////////////////////////////////////////////////////
// Sampling 1D
//////////////////////////////////////////////////////////

#include "Sampling/UniformSampling1D.h"
#include "Sampling/JitteredSampling1D.h"

namespace RISE
{
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateJitteredSampling1D(
								ISampling1D** ppi,				///< [out] Pointer to recieve the sampling1D object
								const Scalar size				///< [in] Size of the kernel
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new JitteredSampling1D( size );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "jittered sampling 1D" );
		return true;
	}

	//! Creates a 1D uniform sampler
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateUniformSampling1D(
								ISampling1D** ppi,				///< [out] Pointer to recieve the sampling1D object
								const Scalar size				///< [in] Size of the kernel
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new UniformSampling1D( size );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "uniform sampling 1D" );
		return true;
	}
}

//////////////////////////////////////////////////////////
// Sampling 2D
//////////////////////////////////////////////////////////

#include "Sampling/RandomSampling2D.h"
#include "Sampling/StratifiedSampling2D.h"
#include "Sampling/NRooksSampling2D.h"
#include "Sampling/PoissonDiskSampling2D.h"
#include "Sampling/UniformSampling2D.h"
#include "Sampling/MultiJitteredSampling2D.h"
#include "Sampling/HaltonPointsSampling2D.h"
#include "Sampling/SobolSampling2D.h"

namespace RISE
{
	//! Creates an NRooks sampling kernel
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateNRooksSampling2D(
								ISampling2D** ppi,				///< [out] Pointer to recieve the sampling2D object
								const Scalar width,				///< [in] Width of the kernel
								const Scalar height,			///< [in] Height of the kernel
								const Scalar howfar				///< [in] How far from the center should each sample be placed?
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new NRooksSampling2D( width, height, howfar );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "nrooks sampling" );
		return true;
	}

	//! Creates a poisson disk sampling kernel
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePoissonDiskSampling2D(
								ISampling2D** ppi,				///< [out] Pointer to recieve the sampling2D object
								const Scalar width,				///< [in] Width of the kernel
								const Scalar height,			///< [in] Height of the kernel
								const Scalar sep				///< [in] Minimum distance between any two samples
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new PoissonDiskSampling2D( width, height, sep );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "poisson sampling" );
		return true;
	}

	//! Creates a random sampling kernel
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateRandomSampling2D(
								ISampling2D** ppi,				///< [out] Pointer to recieve the sampling2D object
								const Scalar width,				///< [in] Width of the kernel
								const Scalar height				///< [in] Height of the kernel
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new RandomSampling2D( width, height );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "random sampling" );
		return true;
	}

	//! Creates a stratified sampling kernel
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateStratifiedSampling2D(
								ISampling2D** ppi,				///< [out] Pointer to recieve the sampling2D object
								const Scalar width,				///< [in] Width of the kernel
								const Scalar height,			///< [in] Height of the kernel
								const Scalar howfar				///< [in] How far from the center should each sample be placed?
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new StratifiedSampling2D( width, height, howfar );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "stratified sampling" );
		return true;
	}

	//! Creates a uniform sampling kernel
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateUniformSampling2D(
								ISampling2D** ppi,				///< [out] Pointer to recieve the sampling2D object
								const Scalar width,				///< [in] Width of the kernel
								const Scalar height				///< [in] Height of the kernel
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new UniformSampling2D( width, height );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "uniform sampling" );
		return true;
	}

	//! Creates a Multi Jittered sampling kernel
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMultiJitteredSampling2D(
								ISampling2D** ppi,				///< [out] Pointer to recieve the sampling2D object
								const Scalar width,				///< [in] Width of the kernel
								const Scalar height			///< [in] Height of the kernel
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new MultiJitteredSampling2D( width, height );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "multi-jittered sampling" );
		return true;
	}

	//! Creates an a Halton point set kernel
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateHaltonPointsSampling2D(
								ISampling2D** ppi,				///< [out] Pointer to recieve the sampling2D object
								const Scalar width,				///< [in] Width of the kernel
								const Scalar height				///< [in] Height of the kernel
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new HaltonPointsSampling2D( width, height );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "Halton points sampling" );
		return true;
	}

	//! Creates an Owen-scrambled Sobol (0,2)-net sampling kernel
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSobolSampling2D(
								ISampling2D** ppi,				///< [out] Pointer to recieve the sampling2D object
								const Scalar width,				///< [in] Width of the kernel
								const Scalar height				///< [in] Height of the kernel
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new SobolSampling2D( width, height );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "Owen-scrambled Sobol sampling" );
		return true;
	}
}


//////////////////////////////////////////////////////////
// Managers
//////////////////////////////////////////////////////////

#include "Managers/ObjectManager.h"
#include "Managers/GeometryManager.h"
#include "Managers/LightManager.h"
#include "Managers/CameraManager.h"
#include "Managers/PainterManager.h"
#include "Managers/ScalarPainterManager.h"
#include "Painters/UniformScalarPainter.h"
#include "Painters/RGBScalarPainter.h"
#include "Painters/PiecewiseLinearScalarPainter.h"
#include "Painters/SellmeierScalarPainter.h"
#include "Painters/PolynomialScalarPainter.h"
#include "Painters/Function1DScalarPainter.h"
#include "Painters/Function2DScalarPainter.h"
#include "Painters/Function2DColorPainter.h"
#include "Painters/ExpressionFunction2DPainter.h"
#include "Painters/TextureScalarPainter.h"
#include "Painters/ScaledScalarPainter.h"
#include "Painters/MultiplyScalarPainter.h"
#include "Managers/MaterialManager.h"
#include "Managers/ShaderManager.h"
#include "Managers/ShaderOpManager.h"
#include "Managers/ModifierManager.h"
#include "Managers/Function1DManager.h"
#include "Managers/Function2DManager.h"

namespace RISE
{
	//! Creates a 1D function manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateFunction1DManager(
								IFunction1DManager** ppi		///< [out] Pointer to recieve the manager
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new Function1DManager();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "function 1d manager" );
		return true;
	}

	//! Creates a 2D function manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateFunction2DManager(
								IFunction2DManager** ppi		///< [out] Pointer to recieve the manager
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new Function2DManager();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "function2d manager" );
		return true;
	}

	//! Creates a light manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateLightManager(
								ILightManager** ppi				///< [out] Pointer to recieve the manager
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new LightManager();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "light manager" );
		return true;
	}

	//! Creates a geometry manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGeometryManager(
								IGeometryManager** ppi			///< [out] Pointer to recieve the manager
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new GeometryManager();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "geometry manager" );
		return true;
	}

	//! Creates a painter manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePainterManager(
								IPainterManager** ppi			///< [out] Pointer to recieve the manager
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new PainterManager();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "painter manager" );
		return true;
	}

	//! Creates a scalar painter manager
	bool RISE_API_CreateScalarPainterManager(
								IScalarPainterManager** ppi
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ScalarPainterManager();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "scalar painter manager" );
		return true;
	}

	// =========================================================================
	// IScalarPainter constructors.  Each one takes outparam, returns
	// TRUE on success.  Every painter is a small allocation tracked
	// via GlobalLog()->PrintNew so the leak-tracking infrastructure
	// catches mis-released constructions.
	// =========================================================================

	bool RISE_API_CreateUniformScalarPainter( IScalarPainter** ppi, Scalar value )
	{
		if( !ppi ) return false;
		*ppi = new UniformScalarPainter( value );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "uniform scalar painter" );
		return true;
	}

	bool RISE_API_CreateRGBScalarPainter( IScalarPainter** ppi, Scalar r, Scalar g, Scalar b )
	{
		if( !ppi ) return false;
		*ppi = new RGBScalarPainter( r, g, b );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "rgb scalar painter" );
		return true;
	}

	bool RISE_API_CreatePiecewiseLinearScalarPainter(
		IScalarPainter** ppi,
		const std::vector<std::pair<Scalar, Scalar>>& samples
		)
	{
		if( !ppi ) return false;
		std::vector<PiecewiseLinearScalarPainter::Sample> converted;
		converted.reserve( samples.size() );
		for( const auto& s : samples ) {
			converted.push_back( { s.first, s.second } );
		}
		*ppi = new PiecewiseLinearScalarPainter( std::move( converted ) );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "piecewise-linear scalar painter" );
		return true;
	}

	bool RISE_API_CreateSellmeierScalarPainter(
		IScalarPainter** ppi,
		Scalar B1, Scalar B2, Scalar B3,
		Scalar C1, Scalar C2, Scalar C3
		)
	{
		if( !ppi ) return false;
		*ppi = new SellmeierScalarPainter( B1, B2, B3, C1, C2, C3 );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "sellmeier scalar painter" );
		return true;
	}

	bool RISE_API_CreatePolynomialScalarPainter(
		IScalarPainter** ppi,
		const std::vector<Scalar>& coeffs
		)
	{
		if( !ppi ) return false;
		*ppi = new PolynomialScalarPainter( coeffs );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "polynomial scalar painter" );
		return true;
	}

	bool RISE_API_CreateFunction1DScalarPainter(
		IScalarPainter** ppi,
		IFunction1D* pFunc
		)
	{
		if( !ppi ) return false;
		*ppi = new Function1DScalarPainter( pFunc );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "function1d scalar painter" );
		return true;
	}

	bool RISE_API_CreateFunction2DScalarPainter(
		IScalarPainter** ppi,
		IFunction2D* pFunc
		)
	{
		if( !ppi ) return false;
		*ppi = new Function2DScalarPainter( pFunc );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "function2d scalar painter" );
		return true;
	}

	bool RISE_API_CreateFunction2DScalarPainterAffine(
		IScalarPainter** ppi,
		IFunction2D* pFunc,
		const double scale,
		const double bias
		)
	{
		if( !ppi ) return false;
		*ppi = new Function2DScalarPainter( pFunc, scale, bias );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "function2d scalar painter (affine)" );
		return true;
	}

	bool RISE_API_CreateFunction2DColorPainter(
		IPainter** ppi,
		IFunction2D* pFunc,
		const double scale,
		const double bias
		)
	{
		if( !ppi ) return false;
		*ppi = new Function2DColorPainter( pFunc, scale, bias );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "function2d color painter" );
		return true;
	}

	bool RISE_API_CreateExpressionFunction2D(
		IPainter** ppi,
		const Implementation::ExpressionProgram& prog
		)
	{
		if( !ppi ) return false;
		if( !prog.IsValid() ) {
			GlobalLog()->PrintEx( eLog_Error, "RISE_API_CreateExpressionFunction2D: invalid program (%s)", prog.Error().c_str() );
			return false;
		}
		*ppi = new Implementation::ExpressionFunction2DPainter( prog );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "expression function2d painter" );
		return true;
	}

	bool RISE_API_CreateTextureScalarPainter(
		IScalarPainter** ppi,
		IRasterImageAccessor* pRIA,
		unsigned int channel
		)
	{
		if( !ppi ) return false;
		const TextureScalarPainter::Channel ch =
			channel == 1 ? TextureScalarPainter::Channel_G :
			channel == 2 ? TextureScalarPainter::Channel_B :
			               TextureScalarPainter::Channel_R;
		*ppi = new TextureScalarPainter( pRIA, ch );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "texture scalar painter" );
		return true;
	}

	bool RISE_API_CreateTextureScalarPainterAffine(
		IScalarPainter** ppi,
		IRasterImageAccessor* pRIA,
		unsigned int channel,
		Scalar scale,
		Scalar bias
		)
	{
		if( !ppi ) return false;
		const TextureScalarPainter::Channel ch =
			channel == 1 ? TextureScalarPainter::Channel_G :
			channel == 2 ? TextureScalarPainter::Channel_B :
			               TextureScalarPainter::Channel_R;
		*ppi = new TextureScalarPainter( pRIA, ch, scale, bias );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "affine texture scalar painter" );
		return true;
	}

	bool RISE_API_CreateScaledScalarPainter(
		IScalarPainter** ppi,
		IScalarPainter* pChild,
		Scalar scale
		)
	{
		if( !ppi ) return false;
		*ppi = new ScaledScalarPainter( pChild, scale );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "scaled scalar painter" );
		return true;
	}

	bool RISE_API_CreateMultiplyScalarPainter(
		IScalarPainter** ppi,
		IScalarPainter* pA,
		IScalarPainter* pB
		)
	{
		if( !ppi ) return false;
		*ppi = new MultiplyScalarPainter( pA, pB );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "multiply scalar painter" );
		return true;
	}

	//! Creates a camera manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCameraManager(
								ICameraManager** ppi			///< [out] Pointer to recieve the manager
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new CameraManager();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "camera manager" );
		return true;
	}

	//! Creates a material manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMaterialManager(
								IMaterialManager** ppi			///< [out] Pointer to recieve the manager
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new MaterialManager();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "material manager" );
		return true;
	}

	//! Creates a shader manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateShaderManager(
								IShaderManager** ppi			///< [out] Pointer to recieve the shader
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ShaderManager();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "shader manager" );
		return true;
	}

	//! Creates a shader op manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateShaderOpManager(
								IShaderOpManager** ppi			///< [out] Pointer to recieve the shader
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ShaderOpManager();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "shader op manager" );
		return true;
	}

	//! Creates a object manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateObjectManager(
								IObjectManager** ppi,					///< [out] Pointer to recieve the manager
								const bool bUseBSPtree,					///< [in] Use BSP trees for spatial partitioning
								const bool bUseOctree,					///< [in] Use Octrees for spatial partitioning
								const unsigned int nMaxObjectsPerNode,	///< [in] Maximum number of elements / node
								const unsigned int nMaxTreeDepth		///< [in] Maximum tree depth
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ObjectManager( bUseBSPtree, bUseOctree, nMaxObjectsPerNode, nMaxTreeDepth );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "manager" );
		return true;
	}

	//! Creates a modifier manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateModifierManager(
								IModifierManager** ppi			///< [out] Pointer to recieve the manager
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ModifierManager();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "manager" );
		return true;
	}
}

//////////////////////////////////////////////////////////
// Modifiers
//////////////////////////////////////////////////////////

#include "Modifiers/BumpMap.h"
#include "Modifiers/NormalMap.h"

namespace RISE
{
	//! Creates a bump map
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBumpMapModifier(
								IRayIntersectionModifier** ppi,	///< [out] Pointer to recieve the modifier
								const IFunction2D& func,		///< [in] The function to use for the bumps
								const Scalar scale,				///< [in] Factor to scale values by
								const Scalar window				///< [in] Size of the window
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new BumpMap( func, scale, window );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "bumpmap" );
		return true;
	}

	//! Creates a tangent-space normal-map modifier.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateNormalMapModifier(
								IRayIntersectionModifier** ppi,	///< [out] Pointer to recieve the modifier
								const IPainter& painter,		///< [in] Linear-RGB normal-map painter
								const Scalar scale				///< [in] glTF normalTexture.scale
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new NormalMap( painter, scale );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "normalmap" );
		return true;
	}

}

//////////////////////////////////////////////////////////
// Functions
//////////////////////////////////////////////////////////

#include "Functions/ConstantFunctions.h"
#include "Utilities/PiecewiseLinearFunction.h"

namespace RISE
{
	//! Creates a constant 1D function
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateConstant1DFunction(
								IFunction1D** ppi,				///< [out] Pointer to recieve the function
								const Scalar value				///< [in] Value the function always returns
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ConstantFunction1D( value );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "constant function 1D" );
		return true;
	}

	//! Creates a constant 2D function
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateConstant2DFunction(
								IFunction2D** ppi,				///< [out] Pointer to recieve the function
								const Scalar value				///< [in] Value the function always returns
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ConstantFunction2D( value );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "constant function 2D" );
		return true;
	}

	//! Creates a constant 3D function
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateConstant3DFunction(
								IFunction3D** ppi,				///< [out] Pointer to recieve the function
								const Scalar value				///< [in] Value the function always returns
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ConstantFunction3D( value );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "constant function 3D" );
		return true;
	}

	//! Creates a piecewise linear function 1D
	bool RISE_API_CreatePiecewiseLinearFunction1D(
								IPiecewiseFunction1D** ppi		///< [out] Pointer to recieve the function
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new PiecewiseLinearFunction1D( );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "piecewise linear function 1D" );
		return true;
	}

	//! Creates a piecewise linear function 2D
	bool RISE_API_CreatePiecewiseLinearFunction2D(
								IPiecewiseFunction2D** ppi		///< [out] Pointer to recieve the function
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new PiecewiseLinearFunction2D( );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "piecewise linear function 2D" );
		return true;
	}
}

//////////////////////////////////////////////////////////
// Utilities
//////////////////////////////////////////////////////////

#include "Utilities/MemoryBuffer.h"
#include "Utilities/DiskFileReadBuffer.h"
#include "Utilities/DiskFileWriteBuffer.h"
#include "Utilities/ProbabilityDensityFunction.h"

namespace RISE
{
	//! Creates a memory buffer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMemoryBuffer(
								IMemoryBuffer** ppi				///< [out] Pointer to recieve the memory buffer
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new MemoryBuffer();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "memory buffer" );
		return true;
	}

	//! Creates a memory buffer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMemoryBufferSize(
								IMemoryBuffer** ppi,			///< [out] Pointer to recieve the memory buffer
								const unsigned int size			///< [in] Size of the memory buffer
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new MemoryBuffer( size );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "memory buffer" );
		return true;
	}

	//! Wraps a memory buffer around some given memory
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCompatibleMemoryBuffer(
								IMemoryBuffer** ppi,			///< [out] Pointer to recieve the memory buffer
								char* pMemory,					///< [in] Pointer to some memory
								const unsigned int size,		///< [in] Amount of memory
								bool bTakeOwnership				///< [in] Should the buffer now take ownership
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new MemoryBuffer( pMemory, size, bTakeOwnership );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "memory buffer" );
		return true;
	}

	//! Creates a memory buffer by loading a file
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMemoryBufferFromFile(
								IMemoryBuffer** ppi,			///< [out] Pointer to recieve the memory buffer
								const char* filename			///< [in] Name of the file to load
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new MemoryBuffer( filename );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "memory buffer" );
		return true;
	}


	//! Creates a read buffer from a file directly
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDiskFileReadBuffer(
								IReadBuffer** ppi,				///< [out] Pointer to recieve the buffer
								const char* filename			///< [in] Name of the file to read
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new DiskFileReadBuffer( filename );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "disk file read buffer" );
		return true;
	}

	//! Creates a write buffer to a file directly
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDiskFileWriteBuffer(
								IWriteBuffer** ppi,				///< [out] Pointer to recieve the buffer
								const char* filename			///< [in] Name of the file to write
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new DiskFileWriteBuffer( filename );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "disk file write buffer" );
		return true;
	}

	//! Creates a probability density function from a normal 1D function
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePDF_Function1D(
								IProbabilityDensityFunction** ppi,	///< [out] Pointer to recieve the PDF
								const IFunction1D* func,			///< [in] A 1-D function to build a PDF of
								const unsigned int numsteps			///< [in] Interval to sample the function
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ProbabilityDensityFunction( func, numsteps );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "PDF" );
		return true;
	}

	//! Creates a probability density function from a spectral packet
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePDF_SpectralPacket(
								IProbabilityDensityFunction** ppi,	///< [out] Pointer to recieve the PDF
								const SpectralPacket& p				///< [in] The spectral packet
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ProbabilityDensityFunction( p );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "PDF" );
		return true;
	}
}

//////////////////////////////////////////////////////////
// Objects
//////////////////////////////////////////////////////////

#include "Objects/Object.h"
#include "Objects/CSGObject.h"

namespace RISE
{
	//! Creates an object
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateObject(
							IObjectPriv** ppi,					///< [out] Pointer to recieve object
							const IGeometry* geom				///< [in] Geometry making up this object
							)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new Object( geom );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "object" );
		return true;
	}

	//! Creates a CSG object
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCSGObject(
								IObjectPriv** ppi,					///< [out] Pointer to recieve object
								IObjectPriv* objA,					///< [in] The first object
								IObjectPriv* objB,					///< [in] The second object
								const char op						///< [in] CSG operation
																	///< 0 -> Union
																	///< 1 -> Intersection
																	///< 2 -> A-B
																	///< 3 -> B-A
								)
	{
		if( !ppi ) {
			return false;
		}

		CSGObject* pObj = 0;

		switch( op ) {
		default:
		case 0:
			pObj = new CSGObject( CSG_UNION );
			break;
		case 1:
			pObj = new CSGObject( CSG_INTERSECTION );
			break;
		case 2:
			pObj = new CSGObject( CSG_SUBTRACTION );
			break;
		}

		pObj->AssignObjects( objA, objB );
		(*ppi) = pObj;

		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "CSG object" );

		return true;
	}
}

//////////////////////////////////////////////////////////
// Scene
//////////////////////////////////////////////////////////

#include "Scene.h"

namespace RISE
{
	//! Creates a scene
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateScene(
							IScenePriv** ppi						///< [out] Pointer to recieve the scene
							)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new Scene();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "scene" );
		return true;
	}
}


//////////////////////////////////////////////////////////
// Photon mapping
//////////////////////////////////////////////////////////

#include "PhotonMapping/CausticPelPhotonMap.h"
#include "PhotonMapping/GlobalPelPhotonMap.h"
#include "PhotonMapping/TranslucentPelPhotonMap.h"
#include "PhotonMapping/CausticSpectralPhotonMap.h"
#include "PhotonMapping/GlobalSpectralPhotonMap.h"
#include "PhotonMapping/CausticPelPhotonTracer.h"
#include "PhotonMapping/TranslucentPelPhotonTracer.h"
#include "PhotonMapping/GlobalPelPhotonTracer.h"
#include "PhotonMapping/CausticSpectralPhotonTracer.h"
#include "PhotonMapping/GlobalSpectralPhotonTracer.h"
#include "PhotonMapping/ShadowPhotonMap.h"
#include "PhotonMapping/ShadowPhotonTracer.h"
#include "PhotonMapping/IrradianceCache.h"

namespace RISE
{
	//! Creates a caustic pel photon map
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCausticPelPhotonMap(
								IPhotonMap** ppi,					///< [out] Pointer to recieve the photon map
								const unsigned int max				///< [in] Maximum number of photons to store
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new CausticPelPhotonMap( max, 0 );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "caustic pel photon map" );
		return true;
	}

	//! Creates a global pel photon map
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGlobalPelPhotonMap(
								IPhotonMap** ppi,					///< [out] Pointer to recieve the photon map
								const unsigned int max				///< [in] Maximum number of photons to store
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new GlobalPelPhotonMap( max, 0 );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "global pel photon map" );
		return true;
	}

	//! Creates a translucent pel photon map
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTranslucentPelPhotonMap(
								IPhotonMap** ppi,					///< [out] Pointer to recieve the photon map
								const unsigned int max				///< [in] Maximum number of photons to store
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TranslucentPelPhotonMap( max, 0 );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "translucent pel photon map" );
		return true;
	}

	//! Creates a caustic spectral photon map
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCausticSpectralPhotonMap(
								ISpectralPhotonMap** ppi,			///< [out] Pointer to recieve the photon map
								const unsigned int max				///< [in] Maximum number of photons to store
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new CausticSpectralPhotonMap( max, 0 );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "spectral photon map" );
		return true;
	}

	//! Creates a global spectral photon map
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGlobalSpectralPhotonMap(
								ISpectralPhotonMap** ppi,			///< [out] Pointer to recieve the photon map
								const unsigned int max				///< [in] Maximum number of photons to store
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new GlobalSpectralPhotonMap( max, 0 );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "global spectral photon map" );
		return true;
	}


	//! Creates a caustic pel photon tracer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCausticPelPhotonTracer(
								IPhotonTracer** ppi,				///< [out] Pointer to recieve the caustic photon tracer
								const unsigned int maxR,			///< [in] Maximum recursion level when tracing
								const Scalar minImp,				///< [in] Minimum photon importance before giving up
								const bool branch,					///< [in] Should the tracer branch or follow a single path?
								const bool reflect,					///< [in] Should we trace reflected rays?
								const bool refract,					///< [in] Should we trace refracted rays?
								const bool shootFromNonMeshLights,	///< [in] Should we shoot from non mesh based lights?
								const Scalar power_scale,			///< [in] How much to scale light power by
								const unsigned int temporal_samples,///< [in] Number of temporal samples to take for animation frames
								const bool regenerate,				///< [in] Should the tracer regenerate a new photon each time the scene time changes?
								const bool shootFromMeshLights		///< [in] Should we shoot from mesh based lights (luminaries)?
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new CausticPelPhotonTracer( maxR, minImp, branch, reflect, refract, shootFromNonMeshLights, power_scale, temporal_samples, regenerate, shootFromMeshLights );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, " caustic pel photon tracer" );
		return true;
	}

	//! Creates a global pel photon tracer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGlobalPelPhotonTracer(
								IPhotonTracer** ppi,				///< [out] Pointer to recieve the global photon tracer
								const unsigned int maxR,			///< [in] Maximum recursion level when tracing
								const Scalar minImp,				///< [in] Minimum photon importance before giving up
								const bool branch,					///< [in] Should the tracer branch or follow a single path?
								const bool shootFromNonMeshLights,	///< [in] Should we shoot from non mesh based lights?
								const Scalar power_scale,			///< [in] How much to scale light power by
								const unsigned int temporal_samples,///< [in] Number of temporal samples to take for animation frames
								const bool regenerate,				///< [in] Should the tracer regenerate a new photon each time the scene time changes?
								const bool shootFromMeshLights		///< [in] Should we shoot from mesh based lights (luminaries)?
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new GlobalPelPhotonTracer( maxR, minImp, branch, shootFromNonMeshLights, power_scale, temporal_samples, regenerate, shootFromMeshLights );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "global pel photon tracer" );
		return true;
	}


	//! Creates a translucent pel photon tracer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTranslucentPelPhotonTracer(
								IPhotonTracer** ppi,				///< [out] Pointer to recieve the translucent photon tracer
								const unsigned int maxR,			///< [in] Maximum recursion level when tracing
								const Scalar minImp,				///< [in] Minimum photon importance before giving up
								const bool reflect,					///< [in] Should we trace reflected rays?
								const bool refract,					///< [in] Should we trace refracted rays?
								const bool direct_translucent,		///< [in] Should we trace translucent primary interaction rays?
								const bool shootFromNonMeshLights,	///< [in] Should we shoot from non mesh based lights?
								const Scalar power_scale,			///< [in] How much to scale light power by
								const unsigned int temporal_samples,///< [in] Number of temporal samples to take for animation frames
								const bool regenerate,				///< [in] Should the tracer regenerate a new photon each time the scene time changes?
								const bool shootFromMeshLights		///< [in] Should we shoot from mesh based lights (luminaries)?
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TranslucentPelPhotonTracer( maxR, minImp, reflect, refract, direct_translucent, shootFromNonMeshLights, power_scale, temporal_samples, regenerate, shootFromMeshLights );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "translucent pel photon tracer" );
		return true;
	}

	//! Creates a caustic spectral photon tracer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCausticSpectralPhotonTracer(
								IPhotonTracer** ppi,				///< [out] Pointer to recieve the caustic photon tracer
								const unsigned int maxR,			///< [in] Maximum recursion level when tracing
								const Scalar minImp,				///< [in] Minimum photon importance before giving up
								const Scalar nm_begin,				///< [in] Wavelength to start shooting photons at
								const Scalar nm_end,				///< [in] Wavelength to end shooting photons at
								const unsigned int num_wavelengths,	///< [in] Number of wavelengths to shoot photons at
								const bool branch,					///< [in] Should the tracer branch or follow a single path?
								const bool reflect,					///< [in] Should we trace reflected rays?
								const bool refract,					///< [in] Should we trace refracted rays?
								const Scalar power_scale,			///< [in] How much to scale light power by
								const unsigned int temporal_samples,///< [in] Number of temporal samples to take for animation frames
								const bool regenerate				///< [in] Should the tracer regenerate a new photon each time the scene time changes?
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new CausticSpectralPhotonTracer( maxR, minImp, nm_begin, nm_end, num_wavelengths, branch, reflect, refract, power_scale, temporal_samples, regenerate );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "caustic spectral photon tracer" );
		return true;
	}

	//! Creates a global spectral photon tracer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGlobalSpectralPhotonTracer(
								IPhotonTracer** ppi,				///< [out] Pointer to recieve the caustic photon tracer
								const unsigned int maxR,			///< [in] Maximum recursion level when tracing
								const Scalar minImp,				///< [in] Minimum photon importance before giving up
								const Scalar nm_begin,				///< [in] Wavelength to start shooting photons at
								const Scalar nm_end,				///< [in] Wavelength to end shooting photons at
								const unsigned int num_wavelengths,	///< [in] Number of wavelengths to shoot photons at
								const bool branch,					///< [in] Should the tracer branch or follow a single path?
								const Scalar power_scale,			///< [in] How much to scale light power by
								const unsigned int temporal_samples,///< [in] Number of temporal samples to take for animation frames
								const bool regenerate				///< [in] Should the tracer regenerate a new photon each time the scene time changes?
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new GlobalSpectralPhotonTracer( maxR, minImp, nm_begin, nm_end, num_wavelengths, branch, power_scale, temporal_samples, regenerate );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "global spectral photon tracer" );
		return true;
	}

	//! Creates a shadow photon map
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateShadowPhotonMap(
								IShadowPhotonMap** ppi,				///< [out] Pointer to recieve the photon map
								const unsigned int max				///< [in] Maximum number of photons to store
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ShadowPhotonMap( max, 0 );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "shadow pel photon map" );
		return true;
	}

	//! Creates a shadow photon tracer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateShadowPhotonTracer(
								IPhotonTracer** ppi,				///< [out] Pointer to recieve the translucent photon tracer
								const unsigned int temporal_samples,///< [in] Number of temporal samples to take for animation frames
								const bool regenerate				///< [in] Should the tracer regenerate a new photon each time the scene time changes?
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ShadowPhotonTracer( temporal_samples, regenerate );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "shadow photon tracer" );
		return true;
	}

	//! Creates an irradiance cache
	/// \return TRUE if successful, FALSE otherwise
bool RISE_API_CreateIrradianceCache(
								IIrradianceCache** ppi,				///< [out] Pointer to recieve the caustic photon tracer
								const unsigned int size,			///< [in] Size of the cache
								const Scalar tolerance,				///< [in] Tolerance of the cache
								const Scalar min_spacing,			///< [in] Minimum seperation
								const double max_spacing,			///< [in] Maximum seperation
								const Scalar query_threshold_scale,	///< [in] Scale for the query acceptance threshold
								const Scalar neighbor_spacing_scale	///< [in] Scale for capping reuse radius by local neighbor spacing
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new IrradianceCache( size, tolerance, min_spacing, max_spacing, query_threshold_scale, neighbor_spacing_scale );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "irradiance cache" );
		return true;
	}
}


//////////////////////////////////////////////////////////
// Film
//////////////////////////////////////////////////////////

#include "Rendering/Film.h"

namespace RISE
{
	bool RISE_API_CreateFilm(
								IFilm** ppi,
								const unsigned int width,
								const unsigned int height,
								const Scalar pixelAR
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new Implementation::Film( width, height, pixelAR );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "film" );
		return true;
	}
}


//////////////////////////////////////////////////////////
// Rasterizer outputs
//////////////////////////////////////////////////////////

#include "Rendering/FileRasterizerOutput.h"

namespace RISE
{
	//! Creates a file rasterizer output
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateFileRasterizerOutput(
								IRasterizerOutput** ppi,				///< [out] Pointer to recieve the rasterizer output
								const char* szPattern,					///< [in] File pattern
								const bool bMultiple,					///< [in] Output multiple files (for animations usually)
								const char type,						///< [in] Type of file
																		///		0 - TGA
																		///		1 - PPM
																		///		2 - PNG
																		///		3 - HDR
																		///		4 - TIFF
																		///		5 - RGBEA
																		///		6 - EXR
								const unsigned char bpp,				///< [in] Bits / pixel for the file
								const COLOR_SPACE color_space,			///< [in] Color space to apply
								const Scalar exposureEV,				///< [in] Exposure offset in EV stops (LDR formats only)
								const DISPLAY_TRANSFORM display_transform,	///< [in] Tone curve (LDR formats only)
								const EXR_COMPRESSION exr_compression,	///< [in] EXR compression mode (EXR only)
								const bool exr_with_alpha				///< [in] Write alpha channel (EXR only)
								)
	{
		if( !ppi ) {
			return false;
		}

		// Map char type code to FRO_TYPE enum once; the per-case body
		// is otherwise identical across all formats.
		FileRasterizerOutput::FRO_TYPE fro_type;
		switch( type )
		{
		case 0:  fro_type = FileRasterizerOutput::TGA;   break;
		case 1:  fro_type = FileRasterizerOutput::PPM;   break;
		default:
		case 2:  fro_type = FileRasterizerOutput::PNG;   break;
		case 3:  fro_type = FileRasterizerOutput::HDR;   break;
		case 4:  fro_type = FileRasterizerOutput::TIFF;  break;
		case 5:  fro_type = FileRasterizerOutput::RGBEA; break;
		case 6:  fro_type = FileRasterizerOutput::EXR;   break;
		}

		(*ppi) = new FileRasterizerOutput(
			szPattern, bMultiple, fro_type, bpp, color_space,
			exposureEV, display_transform, exr_compression, exr_with_alpha );

		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "file rasterizer output" );

		return true;
	}
}

//////////////////////////////////////////////////////////
// Rasterize Sequeces
//////////////////////////////////////////////////////////

#include "Rendering/BlockRasterizeSequence.h"
#include "Rendering/ScanlineRasterizeSequence.h"
#include "Rendering/HilbertRasterizeSequence.h"
#include "Rendering/MortonRasterizeSequence.h"

namespace RISE
{
	//! Creates a Morton (Z-order) curve rasterize sequence for optimal cache locality
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMortonRasterizeSequence(
								IRasterizeSequence** ppi,			///< [out] Pointer to recieve the rasterize sequence
								const unsigned int tileSize			///< [in] Width and height of each square tile
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new MortonRasterizeSequence( tileSize );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "morton raster sequence" );
		return true;
	}

	//! Creates a block rasterize sequence (deprecated: prefer Morton sequence)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBlockRasterizeSequence(
								IRasterizeSequence** ppi,			///< [out] Pointer to recieve the rasterize sequence
								const unsigned int width,			///< [in] Width of the block
								const unsigned int height,			///< [in] Height of the block
								const char order					///< [in] Block ordering type
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new BlockRasterizeSequence( width, height, order );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "block raster sequence" );
		return true;
	}

	//! Creates a scanline rasterize sequence (deprecated: prefer Morton sequence)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateScanlineRasterizeSequence(
								IRasterizeSequence** ppi			///< [out] Pointer to recieve the rasterize sequence
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ScanlineRasterizeSequence( );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "scanline raster sequence" );
		return true;
	}

	//! Creates a hilbert space filling curve rasterize sequence (deprecated: prefer Morton sequence)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateHilbertRasterizeSequence(
								IRasterizeSequence** ppi,			///< [out] Pointer to recieve the rasterize sequence
								const unsigned int depth			///< [in] Depth of curve recursion
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new HilbertRasterizeSequence( depth );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "hilbert sequence" );
		return true;
	}
}

//////////////////////////////////////////////////////////
// Raster Images
//////////////////////////////////////////////////////////

#include "RasterImages/RasterImage.h"
#include "RasterImages/ReadOnlyRasterImage.h"
#include "RasterImages/WriteOnlyRasterImage.h"
#include "RasterImages/NNBRasterImageAccessor.h"
#include "RasterImages/BilinRasterImageAccessor.h"
#include "RasterImages/BicubicRasterImageAccessor.h"
#include "RasterImages/PNGReader.h"
#include "RasterImages/PNGWriter.h"
#include "RasterImages/JPEGReader.h"
#include "RasterImages/TGAReader.h"
#include "RasterImages/HDRReader.h"
#include "RasterImages/TGAWriter.h"
#include "RasterImages/PPMWriter.h"
#include "RasterImages/HDRWriter.h"
#include "RasterImages/RGBEAWriter.h"
#include "RasterImages/TIFFReader.h"
#include "RasterImages/TIFFWriter.h"
#include "RasterImages/EXRReader.h"
#include "RasterImages/EXRWriter.h"
#include "Utilities/CubicInterpolator.h"
#include "Utilities/Color/ScaleColorOperator.h"
#include "Utilities/Color/ShiftColorOperator.h"

namespace RISE
{
	//! Creates a raster image of RISEColor type
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateRISEColorRasterImage(
								IRasterImage** ppi,					///< [out] Pointer to recieve the raster image
								const unsigned int width,			///< [in] Width
								const unsigned int height,			///< [in] Height
								const RISEColor c					///< [in] Value to initialize the raster image to
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new RISERasterImage( width, height, c );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "RISEColor raster image" );
		return true;
	}

	//! Creates a read-only raster image of RISEColor type
	bool RISE_API_CreateReadOnlyRISEColorRasterImage(
								IRasterImage** ppi					///< [out] Pointer to recieve the raster image
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new RISEReadOnlyRasterImage();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "RISEColor read-only raster image" );
		return true;
	}

	//! Creates a write-only raster image of RISEColor type
	bool RISE_API_CreateWriteOnlyRISEColorRasterImage(
								IRasterImage** ppi,					///< [out] Pointer to recieve the raster image
								const unsigned int width,			///< [in] Width
								const unsigned int height			///< [in] Height
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new RISEWriteOnlyRasterImage( width, height );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "RISEColor write-only raster image" );
		return true;
	}

	//! Creates a scale color operator
	bool RISE_API_CreateScaleColorOperatorRasterImage(
								IOneColorOperator** ppi,			///< [out] Pointer to recieve the operator
								const RISEColor& scale				///< [in] Scale factor
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ScaleColorOperator( scale );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "scale color operator" );
		return true;
	}

	//! Creates a shift color operator
	bool RISE_API_CreateShiftColorOperatorRasterImage(
								IOneColorOperator** ppi,			///< [out] Pointer to recieve the operator
								const RISEColor& shift				///< [in] Shift factor
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ShiftColorOperator( shift );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "shift color operator" );
		return true;
	}

	//! Creates a nearest neighbour raster image accessor
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateNNBRasterImageAccessor(
								IRasterImageAccessor** ppi,				///< [out] Pointer to recieve the accessor
								IRasterImage& image,					///< [in] Raster Image to access
								const char wrap_s,						///< [in] Wrap mode for U axis (0 = clamp, 1 = repeat, 2 = mirrored repeat)
								const char wrap_t						///< [in] Wrap mode for V axis (same encoding)
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new NNBRasterImageAccessor<RISEColor>( image, wrap_s, wrap_t );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "NNB RIA" );
		return true;
	}

	//! Creates a bilinear raster image accessor
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBiLinRasterImageAccessor(
								IRasterImageAccessor** ppi,				///< [out] Pointer to recieve the accessor
								IRasterImage& image,					///< [in] Raster Image to access
								const char wrap_s,						///< [in] Wrap mode for U axis (0 = clamp, 1 = repeat, 2 = mirrored repeat)
								const char wrap_t,						///< [in] Wrap mode for V axis (same encoding)
								const bool mipmap,						///< [in] Build a mip pyramid + use LOD-aware sampling (Landing 2)
								const bool supersample					///< [in] Footprint stochastic supersampling (no pyramid; lowmem mode)
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new BilinRasterImageAccessor<RISEColor>( image, wrap_s, wrap_t, mipmap, supersample );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "Bilin RIA" );
		return true;
	}

	//! Creates a catmull rom bicubic raster image accessor
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCatmullRomBicubicRasterImageAccessor(
								IRasterImageAccessor** ppi,			///< [out] Pointer to recieve the accessor
								IRasterImage& image,				///< [in] Raster image to access
								const char wrap_s,					///< [in] Wrap mode for U axis (0 = clamp, 1 = repeat, 2 = mirrored repeat)
								const char wrap_t					///< [in] Wrap mode for V axis (same encoding)
								)
	{
		if( !ppi ) {
			return false;
		}

		ICubicInterpolator<RISEColor>* interp = new CatmullRomCubicInterpolator<RISEColor>();

		(*ppi) = new BicubicRasterImageAccessor<RISEColor>( image, *interp, wrap_s, wrap_t );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "catmull rom RIA" );

		interp->release();
		return true;
	}

	//! Creates a bspline bicubic raster image accessor
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateUniformBSplineBicubicRasterImageAccessor(
								IRasterImageAccessor** ppi,			///< [out] Pointer to recieve the accessor
								IRasterImage& image,				///< [in] Raster image to access
								const char wrap_s,					///< [in] Wrap mode for U axis (0 = clamp, 1 = repeat, 2 = mirrored repeat)
								const char wrap_t					///< [in] Wrap mode for V axis (same encoding)
								)
	{
		if( !ppi ) {
			return false;
		}

		ICubicInterpolator<RISEColor>* interp = new UniformBSplineCubicInterpolator<RISEColor>();

		(*ppi) = new BicubicRasterImageAccessor<RISEColor>( image, *interp, wrap_s, wrap_t );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "uniform bspline RIA" );

		interp->release();
		return true;
	}

	//! Creates a bicubic raster image accessor that uses the given matrix as the weights
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBicubicRasterImageAccessor(
								IRasterImageAccessor** ppi,			///< [out] Pointer to recieve the accessor
								IRasterImage& image,				///< [in] Raster image to access
								const Matrix4& m,
								const char wrap_s,					///< [in] Wrap mode for U axis (0 = clamp, 1 = repeat, 2 = mirrored repeat)
								const char wrap_t					///< [in] Wrap mode for V axis (same encoding)
								)
	{
		if( !ppi ) {
			return false;
		}

		ICubicInterpolator<RISEColor>* interp = new CubicInterpolator<RISEColor>(m);

		(*ppi) = new BicubicRasterImageAccessor<RISEColor>( image, *interp, wrap_s, wrap_t );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "cubic RIA" );

		interp->release();
		return true;
	}

	//! Creates a PNG reader
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePNGReader(
								IRasterImageReader** ppi,			///< [out] Pointer to recieve the raster image reader
								IReadBuffer& buffer,				///< [in] Buffer containing PNG data
								const COLOR_SPACE color_space		///< [in] Color space in the file
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new PNGReader( buffer, color_space );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "PNG reader" );
		return true;
	}

	//! Creates a PNG writer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePNGWriter(
								IRasterImageWriter** ppi,			///< [out] Pointer to recieve the raster image writer
								IWriteBuffer& buffer,				///< [in] Buffer to write to
								const unsigned char bpp,			///< [in] Bits / pixel for the file
								const COLOR_SPACE color_space		///< [in] Color space tp apply
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new PNGWriter( buffer, bpp, color_space );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "PNG writer" );
		return true;
	}

	//! Creates a JPEG reader
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateJPEGReader(
								IRasterImageReader** ppi,			///< [out] Pointer to recieve the raster image reader
								IReadBuffer& buffer,				///< [in] Buffer containing JPEG data
								const COLOR_SPACE color_space		///< [in] Color space in the file
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new JPEGReader( buffer, color_space );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "JPEG reader" );
		return true;
	}

	//! Creates a TGA reader
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTGAReader(
								IRasterImageReader** ppi,			///< [out] Pointer to recieve the raster image reader
								IReadBuffer& buffer,				///< [in] Buffer containing TGA data
								const COLOR_SPACE color_space		///< [in] Color space in the file
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TGAReader( buffer, color_space );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "TGA reader" );
		return true;
	}

	//! Creates a HDR reader (Radiance RGBE format)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateHDRReader(
								IRasterImageReader** ppi,			///< [out] Pointer to recieve the raster image reader
								IReadBuffer& buffer					///< [in] Buffer containing HDR data
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new HDRReader( buffer );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "HDR reader" );
		return true;
	}

	//! Creates a TGA writer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTGAWriter(
								IRasterImageWriter** ppi,			///< [out] Pointer to recieve the raster image writer
								IWriteBuffer& buffer,				///< [in] Buffer to write to
								const COLOR_SPACE color_space		///< [in] Color space to apply
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TGAWriter( buffer, color_space );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "TGA writer" );
		return true;
	}

	//! Creates a PPM writer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePPMWriter(
								IRasterImageWriter** ppi,			///< [out] Pointer to recieve the raster image writer
								IWriteBuffer& buffer,				///< [in] Buffer to write to
								const COLOR_SPACE color_space		///< [in] Color space to apply
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new PPMWriter( buffer, color_space );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "PPM writer" );
		return true;
	}

	//! Creates a HDR writer (Radiance RGBE format)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateHDRWriter(
								IRasterImageWriter** ppi,			///< [out] Pointer to recieve the raster image writer
								IWriteBuffer& buffer,				///< [in] Buffer to write to
								const COLOR_SPACE color_space		///< [in] Color space to apply
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new HDRWriter( buffer, color_space );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "HDR writer" );
		return true;
	}

	//! Creates a RGBEA writer (variant on the Radiance RGBE format)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateRGBEAWriter(
								IRasterImageWriter** ppi,			///< [out] Pointer to recieve the raster image writer
								IWriteBuffer& buffer				///< [in] Buffer to write to
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new RGBEAWriter( buffer );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "RGBEA writer" );
		return true;
	}

	//! Creates a TIFF reader
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTIFFReader(
								IRasterImageReader** ppi,			///< [out] Pointer to recieve the raster image reader
								IReadBuffer& buffer,				///< [in] Buffer containing TIFF data
								const COLOR_SPACE color_space		///< [in] Color space in the file
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TIFFReader( buffer, color_space );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "TIFF reader" );
		return true;
	}

	//! Creates a TIFF writer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTIFFWriter(
								IRasterImageWriter** ppi,			///< [out] Pointer to recieve the raster image writer
								IWriteBuffer& buffer,				///< [in] Buffer to write to
								const COLOR_SPACE color_space		///< [in] Color space to apply
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TIFFWriter( buffer, color_space );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "TIFF writer" );
		return true;
	}

	//! Creates an OpenEXR reader
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateEXRReader(
								IRasterImageReader** ppi,			///< [out] Pointer to recieve the raster image reader
								IReadBuffer& buffer,				///< [in] Buffer to read from
								const COLOR_SPACE color_space		///< [in] Color space to apply
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new EXRReader( buffer, color_space );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "EXR reader" );
		return true;
	}

	//! Creates an OpenEXR writer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateEXRWriter(
								IRasterImageWriter** ppi,			///< [out] Pointer to recieve the raster image writer
								IWriteBuffer& buffer,				///< [in] Buffer to write to
								const COLOR_SPACE color_space,		///< [in] Color space to apply
								const EXR_COMPRESSION compression,	///< [in] EXR compression mode
								const bool with_alpha				///< [in] Write alpha channel
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new EXRWriter( buffer, color_space, compression, with_alpha );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "EXR writer" );
		return true;
	}

	bool RISE_API_CreateEXRWriter(
								IRasterImageWriter** ppi,
								IWriteBuffer& buffer,
								const COLOR_SPACE color_space,
								const EXR_COMPRESSION compression,
								const bool with_alpha,
								const bool write_float
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new EXRWriter( buffer, color_space, compression, with_alpha, write_float );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "EXR writer" );
		return true;
	}
}

//////////////////////////////////////////////////////////
// Shader Ops
//////////////////////////////////////////////////////////

#include "Shaders/ReflectionShaderOp.h"
#include "Shaders/RefractionShaderOp.h"
#include "Shaders/EmissionShaderOp.h"
#include "Shaders/DirectLightingShaderOp.h"
#include "Shaders/CausticPelPhotonMapShaderOp.h"
#include "Shaders/CausticSpectralPhotonMapShaderOp.h"
#include "Shaders/GlobalPelPhotonMapShaderOp.h"
#include "Shaders/GlobalSpectralPhotonMapShaderOp.h"
#include "Shaders/TranslucentPelPhotonMapShaderOp.h"
#include "Shaders/ShadowPhotonMapShaderOp.h"
#include "Shaders/DistributionTracingShaderOp.h"
#include "Shaders/AmbientOcclusionShaderOp.h"
#include "Shaders/FinalGatherShaderOp.h"
#include "Shaders/PathTracingShaderOp.h"
#include "Shaders/SMSShaderOp.h"
#include "Utilities/SMSConfig.h"
#include "Shaders/SSS/SubSurfaceScatteringShaderOp.h"
#include "Shaders/SSS/SimpleExtinction.h"
#include "Shaders/SSS/DiffusionApproximationExtinction.h"
#include "Shaders/AreaLightShaderOp.h"
#include "Shaders/TransparencyShaderOp.h"
#include "Shaders/AlphaTestShaderOp.h"

namespace RISE
{
	//! Creates a reflection shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateReflectionShaderOp(
								IShaderOp** ppi					///< [out] Pointer to recieve the shaderop
								)
	{
		if( !ppi ) {
			return false;
		}

		ReflectionShaderOp* pShaderOp = new ReflectionShaderOp();

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "reflection shaderop" );
		return true;
	}

	//! Creates a refraction shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateRefractionShaderOp(
								IShaderOp** ppi					///< [out] Pointer to recieve the shaderop
								)
	{
		if( !ppi ) {
			return false;
		}

		RefractionShaderOp* pShaderOp = new RefractionShaderOp();

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "refraction shaderop" );
		return true;
	}

	//! Creates an emission shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateEmissionShaderOp(
								IShaderOp** ppi					///< [out] Pointer to recieve the shaderop
								)
	{
		if( !ppi ) {
			return false;
		}

		EmissionShaderOp* pShaderOp = new EmissionShaderOp();

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "emission shaderop" );
		return true;
	}

	//! Creates a direct lighting shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDirectLightingShaderOp(
								IShaderOp** ppi,				///< [out] Pointer to recieve the shaderop
								const IMaterial* bsdf			///< [in] BSDF to use when computing radiance (overrides object BSDF)
								)
	{
		if( !ppi ) {
			return false;
		}

		DirectLightingShaderOp* pShaderOp = new DirectLightingShaderOp( bsdf );

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "direct lighting shaderop" );
		return true;
	}

	//! Creates a caustic pel photonmap shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCausticPelPhotonMapShaderOp(
								IShaderOp** ppi					///< [out] Pointer to recieve the shaderop
								)
	{
		if( !ppi ) {
			return false;
		}

		CausticPelPhotonMapShaderOp* pShaderOp = new CausticPelPhotonMapShaderOp();

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "caustic pel photonmap shaderop" );
		return true;
	}

	//! Creates a caustic spectral photonmap shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCausticSpectralPhotonMapShaderOp(
								IShaderOp** ppi					///< [out] Pointer to recieve the shaderop
								)
	{
		if( !ppi ) {
			return false;
		}

		CausticSpectralPhotonMapShaderOp* pShaderOp = new CausticSpectralPhotonMapShaderOp();

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "caustic spectral photonmap shaderop" );
		return true;
	}

	//! Creates a global pel photonmap shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGlobalPelPhotonMapShaderOp(
								IShaderOp** ppi					///< [out] Pointer to recieve the shaderop
								)
	{
		if( !ppi ) {
			return false;
		}

		GlobalPelPhotonMapShaderOp* pShaderOp = new GlobalPelPhotonMapShaderOp();

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "global pel photonmap shaderop" );
		return true;
	}

	//! Creates a global spectral photonmap shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGlobalSpectralPhotonMapShaderOp(
								IShaderOp** ppi					///< [out] Pointer to recieve the shaderop
								)
	{
		if( !ppi ) {
			return false;
		}

		GlobalSpectralPhotonMapShaderOp* pShaderOp = new GlobalSpectralPhotonMapShaderOp();

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "global spectral photonmap shaderop" );
		return true;
	}

	//! Creates a translucent pel photonmap shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTranslucentPelPhotonMapShaderOp(
								IShaderOp** ppi					///< [out] Pointer to recieve the shaderop
								)
	{
		if( !ppi ) {
			return false;
		}

		TranslucentPelPhotonMapShaderOp* pShaderOp = new TranslucentPelPhotonMapShaderOp();

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "translucent pel photonmap shaderop" );
		return true;
	}

	//! Creates a shadow photonmap shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateShadowPhotonMapShaderOp(
								IShaderOp** ppi					///< [out] Pointer to recieve the shaderop
								)
	{
		if( !ppi ) {
			return false;
		}

		ShadowPhotonMapShaderOp* pShaderOp = new ShadowPhotonMapShaderOp();

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "shadow photonmap shaderop" );
		return true;
	}

	//! Creates a distribution tracing shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDistributionTracingShaderOp(
								IShaderOp** ppi,				///< [out] Pointer to recieve the shaderop
								const unsigned int samples,		///< [in] Number of sample to use in distribution
								const bool irradiancecaching,	///< [in] Should irradiance caching be used if available?
								const bool forcecheckemitters,	///< [in] Force rays allowing to hit emitters even though the material may have a BRDF
								const bool reflections,			///< [in] Should reflections be traced?
								const bool refractions,			///< [in] Should refractions be traced?
								const bool diffuse,				///< [in] Should diffuse rays be traced?
								const bool translucents			///< [in] Should translucent rays be traced?
								)
	{
		if( !ppi ) {
			return false;
		}

		DistributionTracingShaderOp* pShaderOp = new DistributionTracingShaderOp( samples, irradiancecaching, forcecheckemitters, reflections, refractions, diffuse, translucents );

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "distribution tracing shaderop" );
		return true;
	}

	//! Creates an ambient occlusion shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAmbientOcclusionShaderOp(
								IShaderOp** ppi,					///< [out] Pointer to recieve the shaderop
								const unsigned int numThetaSamples, ///< [in] Number of samples in the theta direction
								const unsigned int numPhiSamples,	///< [in] Number of samples in the phi direction
								const bool multiplyBRDF,			///< [in] Should individual samples by multiplied by the BRDF?
								const bool irradiance_cache			///< [in] Should the irradiance state cache be used?
								)
	{
		if( !ppi ) {
			return false;
		}

		AmbientOcclusionShaderOp* pShaderOp = new AmbientOcclusionShaderOp( numThetaSamples, numPhiSamples, multiplyBRDF, irradiance_cache );

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "ambient occlusion shaderop" );
		return true;
	}

	//! Creates a final gather shaderop
	/// \return TRUE if successful, FALSE otherwise
bool RISE_API_CreateFinalGatherShaderOp(
								IShaderOp** ppi,					///< [out] Pointer to recieve the shaderop
								const unsigned int numThetaSamples, ///< [in] Number of samples in the theta direction
								const unsigned int numPhiSamples,	///< [in] Number of samples in the phi direction
								const bool cachegradients,			///< [in] Should cache gradients be used in the irradiance cache?
								const unsigned int min_effective_contributors,	///< [in] Minimum effective contributors required for interpolation
								const Scalar high_variation_reuse_scale,	///< [in] Minimum reuse scale for bright high-variation cache records
								const bool cache					///< [in] Should the rasterizer state cache be used?
								)
	{
		if( !ppi ) {
			return false;
		}

		FinalGatherShaderOp* pShaderOp = new FinalGatherShaderOp( numThetaSamples, numPhiSamples, cachegradients, min_effective_contributors, high_variation_reuse_scale, cache );

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "final gather shaderop" );
		return true;
	}

	//! Creates a path tracing shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePathTracingShaderOp(
								IShaderOp** pShaderOp,
								const bool smsEnabled,
								const unsigned int smsMaxIterations,
								const double smsThreshold,
								const unsigned int smsMaxChainDepth,
								const bool smsBiased
								)
	{
		if( !pShaderOp ) {
			return false;
		}

		ManifoldSolverConfig smsConfig;
		smsConfig.enabled = smsEnabled;
		if( smsEnabled ) {
			smsConfig.maxIterations = smsMaxIterations;
			smsConfig.solverThreshold = smsThreshold;
			smsConfig.maxChainDepth = smsMaxChainDepth;
			smsConfig.biased = smsBiased;
		}

		*pShaderOp = new PathTracingShaderOp( smsConfig, StabilityConfig() );
		GlobalLog()->PrintNew( *pShaderOp, __FILE__, __LINE__, "path tracing shaderop" );
		return true;
	}

	//! Creates an SMS shaderop (standalone, for use with older shader chains)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSMSShaderOp(
								IShaderOp** pShaderOp,
								const unsigned int maxIterations,
								const double threshold,
								const unsigned int maxChainDepth,
								const bool biased
								)
	{
		if( !pShaderOp ) {
			return false;
		}

		ManifoldSolverConfig config;
		config.enabled = true;
		config.maxIterations = maxIterations;
		config.solverThreshold = threshold;
		config.maxChainDepth = maxChainDepth;
		config.biased = biased;

		*pShaderOp = new SMSShaderOp( config );
		GlobalLog()->PrintNew( *pShaderOp, __FILE__, __LINE__, "SMS shaderop" );
		return true;
	}

	//! Creates a simple SSS shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSimpleSubSurfaceScatteringShaderOp(
								IShaderOp** ppi,						///< [out] Pointer to recieve the shaderop
								const unsigned int numPoints,			///< [in] Number of points to use in sampling
								const Scalar error,						///< [in] Error tolerance for bounding the number of point samples
								const unsigned int maxPointsPerNode,	///< [in] Maximum number of points / octree node
								const unsigned char maxDepth,			///< [in] Maximum depth of the octree
								const Scalar irrad_scale,				///< [in] Irradiance scale factor
								const Scalar geometric_scale,			///< [in] Geometric scale factor
								const bool multiplyBSDF,				///< [in] Should the BSDF be evaluated at the point of exitance?
								const bool regenerate,					///< [in] Regenerate the point set on reset calls?
								const IShader& shader,					///< [in] Shader to use for computing irradiance
								const bool cache,						///< [in] Should the rasterizer state cache be used?
								const bool low_discrepancy,				///< [in] Should use a low discrepancy sequence during sample point generation?
								const RISEPel& extinction				///< [in] Extinction in mm^-1
								)
	{
		if( !ppi ) {
			return false;
		}

		ISubSurfaceExtinctionFunction* pExt = new SimpleExtinction( extinction, geometric_scale );
		GlobalLog()->PrintNew( pExt, __FILE__, __LINE__, "simple extinction" );

		SubSurfaceScatteringShaderOp* pShaderOp = new SubSurfaceScatteringShaderOp( numPoints, error, maxPointsPerNode, maxDepth, irrad_scale, multiplyBSDF, regenerate, shader, *pExt, cache, low_discrepancy );

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "simple sss shaderop" );

		pExt->release();
		return true;
	}

	//! Creates a SSS shaderop based on diffusion approximation
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDiffusionApproximationSubSurfaceScatteringShaderOp(
								IShaderOp** ppi,						///< [out] Pointer to recieve the shaderop
								const unsigned int numPoints,			///< [in] Number of points to use in sampling
								const Scalar error,						///< [in] Error tolerance for bounding the number of point samples
								const unsigned int maxPointsPerNode,	///< [in] Maximum number of points / octree node
								const unsigned char maxDepth,			///< [in] Maximum depth of the octree
								const Scalar irrad_scale,				///< [in] Irradiance scale factor
								const Scalar geometric_scale,			///< [in] Geometric scale factor
								const bool multiplyBSDF,				///< [in] Should the BSDF be evaluated at the point of exitance?
								const bool regenerate,					///< [in] Regenerate the point set on reset calls?
								const IShader& shader,					///< [in] Shader to use for computing irradiance
								const bool cache,						///< [in] Should the rasterizer state cache be used?
								const bool low_discrepancy,				///< [in] Should use a low discrepancy sequence during sample point generation?
								const RISEPel& scattering,				///< [in] Scattering coefficient in mm^-1
								const RISEPel& absorption,				///< [in] Absorption coefficient in mm^-1
								const Scalar ior,						///< [in] Index of refraction ratio
								const Scalar g							///< [in] Scattering asymmetry
								)
	{
		if( !ppi ) {
			return false;
		}

		ISubSurfaceExtinctionFunction* pExt = new DiffusionApproximationExtinction( absorption, scattering, ior, g, geometric_scale );
		GlobalLog()->PrintNew( pExt, __FILE__, __LINE__, "simple extinction" );

		SubSurfaceScatteringShaderOp* pShaderOp = new SubSurfaceScatteringShaderOp( numPoints, error, maxPointsPerNode, maxDepth, irrad_scale, multiplyBSDF, regenerate, shader, *pExt, cache, low_discrepancy );

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "simple sss shaderop" );

		pExt->release();
		return true;
	}

	//! Creates an area light shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAreaLightShaderOp(
								IShaderOp** ppi,						///< [out] Pointer to recieve the shaderop
								const Scalar width,						///< [in] Width of the light source
								const Scalar height,					///< [in] Height of the light source
								const Point3 location,					///< [in] Where is the light source located
								const Vector3 dir,						///< [in] What is the light source focussed on
								const unsigned int samples,				///< [in] Number of samples to take
								const IPainter& emm,					///< [in] Emission of this light
								const Scalar power,						///< [in] Power scale
								const IPainter& N,						///< [in] Phong factor for focussing the light
								const Scalar hotSpot,					///< [in] Angle in radians of the light's hot spot
								const bool cache						///< [in] Should the rasterizer state cache be used?
								)
	{
		if( !ppi ) {
			return false;
		}

		AreaLightShaderOp* pShaderOp = new AreaLightShaderOp( width, height, location, dir, samples, emm, power, N, hotSpot, cache );

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "area light shaderop" );
		return true;
	}

	//! Creates a transparency shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTransparencyShaderOp(
								IShaderOp** ppi,						///< [out] Pointer to recieve the shaderop
								const IPainter& transparency,			///< [in] Transparency
								const bool one_sided					///< [in] One sided transparency only (ignore backfaces)
								)
	{
		if( !ppi ) {
			return false;
		}

		TransparencyShaderOp* pShaderOp = new TransparencyShaderOp( transparency, one_sided );

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "transparency shaderop" );
		return true;
	}

	//! Creates an alpha-test shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAlphaTestShaderOp(
								IShaderOp** ppi,
								const IPainter& alpha_painter,
								const Scalar    cutoff
								)
	{
		if( !ppi ) {
			return false;
		}

		AlphaTestShaderOp* pShaderOp = new AlphaTestShaderOp( alpha_painter, cutoff );

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "alpha-test shaderop" );
		return true;
	}
}

//////////////////////////////////////////////////////////
// Shaders
//////////////////////////////////////////////////////////

#include "Shaders/StandardShader.h"
#include "Shaders/AdvancedShader.h"
#include "Shaders/DirectVolumeRenderingShader.h"

namespace RISE
{
	//! Creates a standard shader
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateStandardShader(
								IShader** ppi,						///< [out] Pointer to recieve the shader
								const std::vector<IShaderOp*>& ops	///< [in] The shader ops
								)
	{
		if( !ppi ) {
			return false;
		}

		StandardShader* pShader = new StandardShader( ops );

		(*ppi) = pShader;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "standard shader" );

		return true;
	}

	//! Creates an advanced shader
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAdvancedShader(
								IShader** ppi,						///< [out] Pointer to recieve the shader
								const std::vector<IShaderOp*>& ops,	///< [in] The shader ops
								const std::vector<unsigned int>& m,	///< [in] Parallel array of minimum depths
								const std::vector<unsigned int>& x,	///< [in] Parallel array of maximum depths
								const char* operations				///< [in] Parallel array of operations
								)
	{
		if( !ppi ) {
			return false;
		}

		if( (ops.size() != m.size()) || (m.size() != x.size()) || (ops.size() != x.size()) ) {
			GlobalLog()->PrintEasyError( "RISE_API_CreateAdvancedShader: You parallel arrays don't match in size" );
			return false;
		}

		AdvancedShader::ShadeOpListType sops;

		for( unsigned int i=0; i<ops.size(); i++ ) {
			AdvancedShader::SHADE_OP op;
			op.pShaderOp = ops[i];
			op.nMinDepth = m[i];
			op.nMaxDepth = x[i];
			op.operation = operations[i];
			sops.push_back( op );
		}

		AdvancedShader* pShader = new AdvancedShader( sops );

		(*ppi) = pShader;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "standard shader" );

		return true;
	}


	//! Creates a direct volume rendering shader
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDirectVolumeRenderingShader(
								IShader** ppi,						///< [out] Pointer to recieve the shader
								const char* szVolumeFilePattern,	///< [in] File pattern for volume data
								const unsigned int width,			///< [in] Width of the volume
								const unsigned int height,			///< [in] Height of the volume
								const unsigned int startz,			///< [in] Starting z value for volume data
								const unsigned int endz,			///< [in] Ending z value for the volume data
								const char accessor,				///< [in] Type of volume accessor
								const char gradient,				///< [in] Type of gradient estimator to use
								const char composite,				///< [in] Type of composite operation to use
								const Scalar dThresholdStart,		///< [in] Start of ISO threshold value (for ISO renderings only)
								const Scalar dThresholdEnd,			///< [in] End of ISO threshold value (for ISO renderings only)
								ISampling1D& sampler,				///< [in] The sampler to use when sampling the ray
								const IFunction1D& red,				///< [in] Transfer function for the red channel
								const IFunction1D& green,			///< [in] Transfer function for the green channel
								const IFunction1D& blue,			///< [in] Transfer function for the blue channel
								const IFunction1D& alpha,			///< [in] Transfer function for the alpha channel
								const IShader* pISOShader			///< [in] Shader to use for ISO surfaces (only valid for ISO rendering)
								)
	{
		if( !ppi ) {
			return false;
		}

		DirectVolumeRenderingShader* pShader = new DirectVolumeRenderingShader(
			szVolumeFilePattern, width, height, startz, endz,
			accessor, gradient, composite, dThresholdStart, dThresholdEnd, sampler,
			red, green, blue, alpha, pISOShader );

		(*ppi) = pShader;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "direct volume rendering shader" );

		return true;
	}

	//! Creates a direct volume rendering shader with spectral transfer functions
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSpectralDirectVolumeRenderingShader(
								IShader** ppi,						///< [out] Pointer to recieve the shader
								const char* szVolumeFilePattern,	///< [in] File pattern for volume data
								const unsigned int width,			///< [in] Width of the volume
								const unsigned int height,			///< [in] Height of the volume
								const unsigned int startz,			///< [in] Starting z value for volume data
								const unsigned int endz,			///< [in] Ending z value for the volume data
								const char accessor,				///< [in] Type of volume accessor
								const char gradient,				///< [in] Type of gradient estimator to use
								const char composite,				///< [in] Type of composite operation to use
								const Scalar dThresholdStart,		///< [in] Start of ISO threshold value (for ISO renderings only)
								const Scalar dThresholdEnd,			///< [in] End of ISO threshold value (for ISO renderings only)
								ISampling1D& sampler,				///< [in] The sampler to use when sampling the ray
								const IFunction1D& alpha,			///< [in] Transfer function for the alpha channel
								const IFunction2D& spectral,		///< [in] Spectral transfer functions
								const IShader* pISOShader			///< [in] Shader to use for ISO surfaces (only valid for ISO rendering)
								)
	{
		if( !ppi ) {
			return false;
		}

		DirectVolumeRenderingShader* pShader = new DirectVolumeRenderingShader(
			szVolumeFilePattern, width, height, startz, endz,
			accessor, gradient, composite, dThresholdStart, dThresholdEnd, sampler,
			alpha, spectral, pISOShader );

		(*ppi) = pShader;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "spectral direct volume rendering shader" );

		return true;
	}
}

//////////////////////////////////////////////////////////
// Rendering Core
/////////////////////////////////////////////////////////

#include "Rendering/LuminaryManager.h"
#include "Rendering/RayCaster.h"

namespace RISE
{
	//! Creates a luminary manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateLuminaryManager(
								ILuminaryManager** ppi				///< [out] Pointer to recieve the luminary manager
								)
	{
		if( !ppi ) {
			return false;
		}

		LuminaryManager* pLum = new LuminaryManager();

		(*ppi) = pLum;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "luminary manager" );
		return true;
	}

	//! Creates a ray caster (something similar to a ray server)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateRayCaster(
								IRayCaster** ppi,					///< [out] Pointer to recieve the ray caster
								const bool seeRadianceMap,			///< [in] Is the radiance map (environment) visible to the view rays?
								const unsigned int maxR,			///< [in] Maximum recursion level
								const IShader& pDefaultShader,		///< [in] The default global shader
								const bool showLuminaires			///< [in] Should we be able to see luminaries?
								)
	{
		if( !ppi ) {
			return false;
		}

		RayCaster* pCaster = new RayCaster( seeRadianceMap, maxR, pDefaultShader, showLuminaires );

		(*ppi) = pCaster;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "ray caster" );
		return true;
	}
}

//////////////////////////////////////////////////////////
// Rasterizer
//////////////////////////////////////////////////////////

#include "Rendering/PixelBasedPelRasterizer.h"
#include "Rendering/PixelBasedSpectralIntegratingRasterizer.h"

namespace RISE
{
	//! Creates a pixel based rasterizer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePixelBasedPelRasterizer(
								IRasterizer** ppi,					///< [out] Pointer to recieve the rasterizer
								IRayCaster* caster,					///< [in] Ray caster to use for rays
								ISampling2D* pSamples,				///< [in] Sampler for subsamples
								IPixelFilter* pFilter,				///< [in] Pixel Filter for samples
								const bool oidnDenoise,				///< [in] Enable OIDN denoising post-process
								const OidnQuality oidnQuality,		///< [in] OIDN quality preset (Auto = render-time heuristic)
							const OidnDevice oidnDevice,		///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
							const OidnPrefilter oidnPrefilter,	///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
								const PathGuidingConfig& guidingConfig,	///< [in] Path guiding configuration
								const AdaptiveSamplingConfig& adaptiveConfig,	///< [in] Adaptive sampling configuration
								const StabilityConfig& stabilityConfig,	///< [in] Production stability controls
								const bool useZSobol,				///< [in] Use Morton-indexed Sobol (blue-noise error distribution)
								Implementation::FrameStore* frameStore    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
								)
	{
		if( !ppi ) {
			return false;
		}

		PixelBasedPelRasterizer* pRasterizer = new PixelBasedPelRasterizer( caster, guidingConfig, adaptiveConfig, stabilityConfig, useZSobol, frameStore);

#ifdef RISE_ENABLE_OIDN
		pRasterizer->SetDenoisingEnabled( oidnDenoise );
		pRasterizer->SetDenoisingQuality( oidnQuality );
		pRasterizer->SetDenoisingDevice( oidnDevice );
		pRasterizer->SetDenoisingPrefilter( oidnPrefilter );
#else
		if( oidnDenoise ) {
			GlobalLog()->PrintEasyWarning( "OIDN denoising requested but RISE was compiled without RISE_ENABLE_OIDN support" );
		}
		(void)oidnQuality;
		(void)oidnDevice;
		(void)oidnPrefilter;
#endif

#ifndef RISE_ENABLE_OPENPGL
		if( guidingConfig.enabled ) {
			GlobalLog()->PrintEasyWarning( "Path guiding requested but RISE was compiled without RISE_ENABLE_OPENPGL support" );
		}
#endif

		if( pSamples && pFilter ) {
			pRasterizer->SubSampleRays( pSamples, pFilter );
		}

		(*ppi) = pRasterizer;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "pixel rasterizer" );
		return true;
	}

	//! Creates a pixel based spectral integrating rasterizer
	bool RISE_API_CreatePixelBasedSpectralIntegratingRasterizer(
								IRasterizer** ppi,					///< [out] Pointer to recieve the rasterizer
								IRayCaster* caster,					///< [in] Ray caster to use for rays
								ISampling2D* pSamples,				///< [in] Sampler for subsamples
								IPixelFilter* pFilter,				///< [in] Pixel Filter for samples
								const unsigned int specSamples,		///< [in] Number of spectral samples / pixel
								const Scalar lambda_begin,			///< [in] nm to begin sampling at
								const Scalar lambda_end,			///< [in] nm to end sampling at
								const unsigned int num_wavelengths,	///< [in] Number of wavelengths to sample
								const bool oidnDenoise,				///< [in] Enable OIDN denoising post-process
								const OidnQuality oidnQuality,		///< [in] OIDN quality preset (Auto = render-time heuristic)
							const OidnDevice oidnDevice,		///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
							const OidnPrefilter oidnPrefilter,	///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
								const StabilityConfig& stabilityConfig,	///< [in] Production stability controls
								const bool useZSobol,			///< [in] Use Morton-indexed Sobol (blue-noise error distribution)
								const bool useHWSS,				///< [in] Use Hero Wavelength Spectral Sampling
								Implementation::FrameStore* frameStore    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
								)
	{
		if( !ppi ) {
			return false;
		}

		PixelBasedSpectralIntegratingRasterizer* pRasterizer = new PixelBasedSpectralIntegratingRasterizer( caster, lambda_begin, lambda_end, num_wavelengths, specSamples, stabilityConfig, useZSobol, useHWSS, frameStore);

#ifdef RISE_ENABLE_OIDN
		pRasterizer->SetDenoisingEnabled( oidnDenoise );
		pRasterizer->SetDenoisingQuality( oidnQuality );
		pRasterizer->SetDenoisingDevice( oidnDevice );
		pRasterizer->SetDenoisingPrefilter( oidnPrefilter );
#else
		if( oidnDenoise ) {
			GlobalLog()->PrintEasyWarning( "OIDN denoising requested but RISE was compiled without RISE_ENABLE_OIDN support" );
		}
		(void)oidnQuality;
		(void)oidnDevice;
		(void)oidnPrefilter;
#endif

		if( pSamples && pFilter ) {
			pRasterizer->SubSampleRays( pSamples, pFilter );
		}

		(*ppi) = pRasterizer;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "spectral integrating rasterizer" );
		return true;
	}

}

#include "Rendering/BDPTPelRasterizer.h"
#include "Rendering/BDPTSpectralRasterizer.h"
#include "Rendering/VCMPelRasterizer.h"
#include "Rendering/VCMSpectralRasterizer.h"
#include "Rendering/PathTracingPelRasterizer.h"
#include "Rendering/PathTracingSpectralRasterizer.h"
#include "Rendering/MLTRasterizer.h"
#include "Rendering/MLTSpectralRasterizer.h"
#include "Rendering/AutoRasterizer.h"
#include "Utilities/ManifoldSolver.h"

namespace RISE
{
	//! Creates a Pel (RGB) BDPT rasterizer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBDPTPelRasterizer(
								IRasterizer** ppi,
								IRayCaster* caster,
								ISampling2D* pSamples,
								IPixelFilter* pFilter,
								const unsigned int maxEyeDepth,
								const unsigned int maxLightDepth,
								const bool oidnDenoise,
								const OidnQuality oidnQuality,
							const OidnDevice oidnDevice,
							const OidnPrefilter oidnPrefilter,
								const PathGuidingConfig& guidingConfig,
								const AdaptiveSamplingConfig& adaptiveConfig,
								const StabilityConfig& stabilityConfig,
								const bool useZSobol,
								Implementation::FrameStore* frameStore    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
								)
	{
		if( !ppi ) {
			return false;
		}

		BDPTPelRasterizer* pRasterizer = new BDPTPelRasterizer( caster, maxEyeDepth, maxLightDepth, guidingConfig, adaptiveConfig, stabilityConfig, useZSobol, frameStore);

		if( pSamples && pFilter ) {
			pRasterizer->SubSampleRays( pSamples, pFilter );
		}

#ifdef RISE_ENABLE_OIDN
		pRasterizer->SetDenoisingEnabled( oidnDenoise );
		pRasterizer->SetDenoisingQuality( oidnQuality );
		pRasterizer->SetDenoisingDevice( oidnDevice );
		pRasterizer->SetDenoisingPrefilter( oidnPrefilter );
#else
		if( oidnDenoise ) {
			GlobalLog()->PrintEasyWarning( "OIDN denoising requested but RISE was compiled without RISE_ENABLE_OIDN support" );
		}
		(void)oidnQuality;
		(void)oidnDevice;
		(void)oidnPrefilter;
#endif

		(*ppi) = pRasterizer;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "BDPT Pel rasterizer" );
		return true;
	}

	//! Creates a spectral BDPT rasterizer with adaptive sampling support.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBDPTSpectralRasterizerAdaptive(
								IRasterizer** ppi,
								IRayCaster* caster,
								ISampling2D* pSamples,
								IPixelFilter* pFilter,
								const unsigned int maxEyeDepth,
								const unsigned int maxLightDepth,
								const Scalar lambda_begin,
								const Scalar lambda_end,
								const unsigned int num_wavelengths,
								const unsigned int spectral_samples,
								const bool oidnDenoise,
								const OidnQuality oidnQuality,
							const OidnDevice oidnDevice,
							const OidnPrefilter oidnPrefilter,
								const PathGuidingConfig& guidingConfig,
								const AdaptiveSamplingConfig& adaptiveConfig,
								const StabilityConfig& stabilityConfig,
								const bool useZSobol,
								const bool useHWSS,
								Implementation::FrameStore* frameStore    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
								)
	{
		if( !ppi ) {
			return false;
		}

		BDPTSpectralRasterizer* pRasterizer = new BDPTSpectralRasterizer(
			caster, maxEyeDepth, maxLightDepth,
			lambda_begin, lambda_end, num_wavelengths, spectral_samples,
			guidingConfig, adaptiveConfig, stabilityConfig, useZSobol, useHWSS, frameStore);

		if( pSamples && pFilter ) {
			pRasterizer->SubSampleRays( pSamples, pFilter );
		}

#ifdef RISE_ENABLE_OIDN
		pRasterizer->SetDenoisingEnabled( oidnDenoise );
		pRasterizer->SetDenoisingQuality( oidnQuality );
		pRasterizer->SetDenoisingDevice( oidnDevice );
		pRasterizer->SetDenoisingPrefilter( oidnPrefilter );
#else
		if( oidnDenoise ) {
			GlobalLog()->PrintEasyWarning( "OIDN denoising requested but RISE was compiled without RISE_ENABLE_OIDN support" );
		}
		(void)oidnQuality;
		(void)oidnDevice;
		(void)oidnPrefilter;
#endif

		(*ppi) = pRasterizer;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "BDPT Spectral rasterizer" );
		return true;
	}

	//! Creates a spectral BDPT rasterizer (legacy — no adaptive sampling).
	/// Thin wrapper delegating to RISE_API_CreateBDPTSpectralRasterizerAdaptive
	/// with a default-constructed AdaptiveSamplingConfig.  Kept for ABI
	/// compatibility with callers that predate adaptive-sampling support.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBDPTSpectralRasterizer(
								IRasterizer** ppi,
								IRayCaster* caster,
								ISampling2D* pSamples,
								IPixelFilter* pFilter,
								const unsigned int maxEyeDepth,
								const unsigned int maxLightDepth,
								const Scalar lambda_begin,
								const Scalar lambda_end,
								const unsigned int num_wavelengths,
								const unsigned int spectral_samples,
								const bool oidnDenoise,
								const OidnQuality oidnQuality,
							const OidnDevice oidnDevice,
							const OidnPrefilter oidnPrefilter,
								const PathGuidingConfig& guidingConfig,
								const StabilityConfig& stabilityConfig,
								const bool useZSobol,
								const bool useHWSS,
								Implementation::FrameStore* frameStore
								)
	{
		return RISE_API_CreateBDPTSpectralRasterizerAdaptive(
			ppi, caster, pSamples, pFilter,
			maxEyeDepth, maxLightDepth,
			lambda_begin, lambda_end, num_wavelengths, spectral_samples,
			oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter,
			guidingConfig, AdaptiveSamplingConfig(),
			stabilityConfig, useZSobol, useHWSS, frameStore );
	}

	//! Creates a Pel (RGB) Vertex Connection and Merging rasterizer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateVCMPelRasterizer(
								IRasterizer** ppi,
								IRayCaster* caster,
								ISampling2D* pSamples,
								IPixelFilter* pFilter,
								const unsigned int maxEyeDepth,
								const unsigned int maxLightDepth,
								const Scalar mergeRadius,
								const bool enableVC,
								const bool enableVM,
								const bool oidnDenoise,
								const OidnQuality oidnQuality,
							const OidnDevice oidnDevice,
							const OidnPrefilter oidnPrefilter,
								const PathGuidingConfig& guidingConfig,
								const AdaptiveSamplingConfig& adaptiveConfig,
								const StabilityConfig& stabilityConfig,
								const bool useZSobol,
								Implementation::FrameStore* frameStore    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
								)
	{
		if( !ppi ) {
			return false;
		}

		VCMPelRasterizer* pRasterizer = new VCMPelRasterizer(
			caster,
			maxEyeDepth,
			maxLightDepth,
			mergeRadius,
			enableVC,
			enableVM,
			guidingConfig,
			adaptiveConfig,
			stabilityConfig,
			useZSobol, frameStore);

		if( pSamples && pFilter ) {
			pRasterizer->SubSampleRays( pSamples, pFilter );
		}

#ifdef RISE_ENABLE_OIDN
		pRasterizer->SetDenoisingEnabled( oidnDenoise );
		pRasterizer->SetDenoisingQuality( oidnQuality );
		pRasterizer->SetDenoisingDevice( oidnDevice );
		pRasterizer->SetDenoisingPrefilter( oidnPrefilter );
#else
		if( oidnDenoise ) {
			GlobalLog()->PrintEasyWarning( "OIDN denoising requested but RISE was compiled without RISE_ENABLE_OIDN support" );
		}
		(void)oidnQuality;
		(void)oidnDevice;
		(void)oidnPrefilter;
#endif

		(*ppi) = pRasterizer;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "VCM Pel rasterizer" );
		return true;
	}

	//! Creates a spectral Vertex Connection and Merging rasterizer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateVCMSpectralRasterizer(
								IRasterizer** ppi,
								IRayCaster* caster,
								ISampling2D* pSamples,
								IPixelFilter* pFilter,
								const unsigned int maxEyeDepth,
								const unsigned int maxLightDepth,
								const Scalar lambda_begin,
								const Scalar lambda_end,
								const unsigned int num_wavelengths,
								const unsigned int spectral_samples,
								const Scalar mergeRadius,
								const bool enableVC,
								const bool enableVM,
								const bool oidnDenoise,
								const OidnQuality oidnQuality,
							const OidnDevice oidnDevice,
							const OidnPrefilter oidnPrefilter,
								const PathGuidingConfig& guidingConfig,
								const AdaptiveSamplingConfig& adaptiveConfig,
								const StabilityConfig& stabilityConfig,
								const bool useZSobol,
								const bool useHWSS,
								Implementation::FrameStore* frameStore    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
								)
	{
		if( !ppi ) {
			return false;
		}

		VCMSpectralRasterizer* pRasterizer = new VCMSpectralRasterizer(
			caster,
			maxEyeDepth,
			maxLightDepth,
			lambda_begin,
			lambda_end,
			num_wavelengths,
			spectral_samples,
			mergeRadius,
			enableVC,
			enableVM,
			guidingConfig,
			adaptiveConfig,
			stabilityConfig,
			useZSobol,
			useHWSS, frameStore);

		if( pSamples && pFilter ) {
			pRasterizer->SubSampleRays( pSamples, pFilter );
		}

#ifdef RISE_ENABLE_OIDN
		pRasterizer->SetDenoisingEnabled( oidnDenoise );
		pRasterizer->SetDenoisingQuality( oidnQuality );
		pRasterizer->SetDenoisingDevice( oidnDevice );
		pRasterizer->SetDenoisingPrefilter( oidnPrefilter );
#else
		if( oidnDenoise ) {
			GlobalLog()->PrintEasyWarning( "OIDN denoising requested but RISE was compiled without RISE_ENABLE_OIDN support" );
		}
		(void)oidnQuality;
		(void)oidnDevice;
		(void)oidnPrefilter;
#endif

		(*ppi) = pRasterizer;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "VCM Spectral rasterizer" );
		return true;
	}

	//! Creates a pure path tracing Pel rasterizer
	bool RISE_API_CreatePathTracingPelRasterizer(
								IRasterizer** ppi,
								IRayCaster* caster,
								ISampling2D* pSamples,
								IPixelFilter* pFilter,
								const bool smsEnabled,
								const unsigned int smsMaxIterations,
								const double smsThreshold,
								const unsigned int smsMaxChainDepth,
								const bool smsBiased,
								const unsigned int smsBernoulliTrials,
								const unsigned int smsMultiTrials,
								const unsigned int smsPhotonCount,
								const bool smsTwoStage,
								const bool smsUseLevenbergMarquardt,
								const SMSSeedingMode smsSeedingMode,
							const unsigned int smsTargetBounces,
								const bool oidnDenoise,
								const OidnQuality oidnQuality,
							const OidnDevice oidnDevice,
							const OidnPrefilter oidnPrefilter,
								const PathGuidingConfig& guidingConfig,
								const AdaptiveSamplingConfig& adaptiveConfig,
								const StabilityConfig& stabilityConfig,
								const bool useZSobol,
								Implementation::FrameStore* frameStore    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
								)
	{
		if( !ppi ) {
			return false;
		}

		ManifoldSolverConfig smsConfig;
		smsConfig.enabled = smsEnabled;
		if( smsEnabled ) {
			smsConfig.maxIterations = smsMaxIterations;
			smsConfig.solverThreshold = smsThreshold;
			smsConfig.maxChainDepth = smsMaxChainDepth;
			smsConfig.biased = smsBiased;
			smsConfig.maxBernoulliTrials = smsBernoulliTrials;
			smsConfig.multiTrials = smsMultiTrials;
			smsConfig.photonCount = smsPhotonCount;
			smsConfig.twoStage = smsTwoStage;
			smsConfig.useLevenbergMarquardt = smsUseLevenbergMarquardt;
			smsConfig.seedingMode = ( smsSeedingMode == SMSSeedingMode::Uniform )
				? ManifoldSolverConfig::eSeedingUniform
				: ManifoldSolverConfig::eSeedingSnell;
			smsConfig.targetBounces = smsTargetBounces;
		}

		PathTracingPelRasterizer* pRasterizer = new PathTracingPelRasterizer(
			caster, smsConfig, guidingConfig, adaptiveConfig, stabilityConfig, useZSobol, frameStore);

		if( pSamples && pFilter ) {
			pRasterizer->SubSampleRays( pSamples, pFilter );
		}

#ifdef RISE_ENABLE_OIDN
		pRasterizer->SetDenoisingEnabled( oidnDenoise );
		pRasterizer->SetDenoisingQuality( oidnQuality );
		pRasterizer->SetDenoisingDevice( oidnDevice );
		pRasterizer->SetDenoisingPrefilter( oidnPrefilter );
#else
		if( oidnDenoise ) {
			GlobalLog()->PrintEasyWarning( "OIDN denoising requested but RISE was compiled without RISE_ENABLE_OIDN support" );
		}
		(void)oidnQuality;
		(void)oidnDevice;
		(void)oidnPrefilter;
#endif

		(*ppi) = pRasterizer;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "PathTracing Pel rasterizer" );
		return true;
	}

	//! Creates the auto-routing integrator dispatcher (auto_rasterizer)
	bool RISE_API_CreateAutoRasterizer(
								IRasterizer** ppi,
								IRayCaster* caster,
								ISampling2D* pSamples,
								IPixelFilter* pFilter,
								const AutoIntegratorChoice integrator,
								const bool oidnDenoise,
								const OidnQuality oidnQuality,
								const OidnDevice oidnDevice,
								const OidnPrefilter oidnPrefilter,
								const PathGuidingConfig& guidingConfig,
								const AdaptiveSamplingConfig& adaptiveConfig,
								const StabilityConfig& stabilityConfig,
								const bool useZSobol,
								const ProgressiveConfig& progressiveConfig,
								const bool probeEnabled,
								Implementation::FrameStore* frameStore
								)
	{
		if( !ppi ) {
			return false;
		}

		// The dispatcher stores these inputs and builds the chosen concrete
		// delegate lazily at the first render-time entry (so the probe can
		// see the assembled scene).  OIDN / progressive are NOT applied to
		// the wrapper here — they ride into the delegate's factory call
		// (BuildDelegate) and the post-build progressive wiring inside the
		// wrapper, because the wrapper itself renders nothing.
		AutoRasterizer* pRasterizer = new AutoRasterizer(
			caster, pSamples, pFilter, integrator,
			oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter,
			guidingConfig, adaptiveConfig, stabilityConfig,
			useZSobol, progressiveConfig, probeEnabled,
			/*spectral*/ false, SpectralConfig(), frameStore );

		(*ppi) = pRasterizer;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "Auto rasterizer" );
		return true;
	}

	//! Creates the SPECTRAL auto-routing integrator dispatcher (auto_spectral_rasterizer)
	bool RISE_API_CreateAutoSpectralRasterizer(
								IRasterizer** ppi,
								IRayCaster* caster,
								ISampling2D* pSamples,
								IPixelFilter* pFilter,
								const AutoIntegratorChoice integrator,
								const Scalar lambda_begin,
								const Scalar lambda_end,
								const unsigned int num_wavelengths,
								const unsigned int spectral_samples,
								const bool useHWSS,
								const bool oidnDenoise,
								const OidnQuality oidnQuality,
								const OidnDevice oidnDevice,
								const OidnPrefilter oidnPrefilter,
								const AdaptiveSamplingConfig& adaptiveConfig,
								const StabilityConfig& stabilityConfig,
								const bool useZSobol,
								const ProgressiveConfig& progressiveConfig,
								const bool probeEnabled,
								Implementation::FrameStore* frameStore
								)
	{
		if( !ppi ) {
			return false;
		}

		// Bundle the spectral-core params the wrapper carries for the
		// *_spectral_ delegate factories (BuildDelegate unpacks them).
		SpectralConfig spectralConfig;
		spectralConfig.nmBegin         = lambda_begin;
		spectralConfig.nmEnd           = lambda_end;
		spectralConfig.numWavelengths  = num_wavelengths;
		spectralConfig.spectralSamples = spectral_samples;
		spectralConfig.useHWSS         = useHWSS;

		// The spectral domain exposes NO path-guiding (the spectral PT factory
		// has no guiding arg; the BDPT/VCM spectral factories take a disabled
		// one).  Supply the disabled default so the shared AutoRasterizer ctor
		// signature is satisfied without a guiding knob on the spectral chunk.
		const PathGuidingConfig guidingConfig;   // default = disabled

		// Same dispatcher class as the Pel factory; the `true` domain flag is
		// the only difference, switching BuildDelegate to the *_spectral_
		// factories.  The Tier-0/1/2 decision logic is shared verbatim.
		AutoRasterizer* pRasterizer = new AutoRasterizer(
			caster, pSamples, pFilter, integrator,
			oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter,
			guidingConfig, adaptiveConfig, stabilityConfig,
			useZSobol, progressiveConfig, probeEnabled,
			/*spectral*/ true, spectralConfig, frameStore );

		(*ppi) = pRasterizer;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "Auto spectral rasterizer" );
		return true;
	}

	//! Creates a pure path tracing spectral rasterizer
	bool RISE_API_CreatePathTracingSpectralRasterizer(
								IRasterizer** ppi,
								IRayCaster* caster,
								ISampling2D* pSamples,
								IPixelFilter* pFilter,
								const Scalar lambda_begin,
								const Scalar lambda_end,
								const unsigned int num_wavelengths,
								const unsigned int spectral_samples,
								const bool smsEnabled,
								const unsigned int smsMaxIterations,
								const double smsThreshold,
								const unsigned int smsMaxChainDepth,
								const bool smsBiased,
								const unsigned int smsBernoulliTrials,
								const unsigned int smsMultiTrials,
								const unsigned int smsPhotonCount,
								const bool smsTwoStage,
								const bool smsUseLevenbergMarquardt,
								const SMSSeedingMode smsSeedingMode,
							const unsigned int smsTargetBounces,
								const bool oidnDenoise,
								const OidnQuality oidnQuality,
							const OidnDevice oidnDevice,
							const OidnPrefilter oidnPrefilter,
								const AdaptiveSamplingConfig& adaptiveConfig,
								const StabilityConfig& stabilityConfig,
								const bool useZSobol,
								const bool useHWSS,
								Implementation::FrameStore* frameStore    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
								)
	{
		if( !ppi ) {
			return false;
		}

		ManifoldSolverConfig smsConfig;
		smsConfig.enabled = smsEnabled;
		if( smsEnabled ) {
			smsConfig.maxIterations = smsMaxIterations;
			smsConfig.solverThreshold = smsThreshold;
			smsConfig.maxChainDepth = smsMaxChainDepth;
			smsConfig.biased = smsBiased;
			smsConfig.maxBernoulliTrials = smsBernoulliTrials;
			smsConfig.multiTrials = smsMultiTrials;
			smsConfig.photonCount = smsPhotonCount;
			smsConfig.twoStage = smsTwoStage;
			smsConfig.useLevenbergMarquardt = smsUseLevenbergMarquardt;
			smsConfig.seedingMode = ( smsSeedingMode == SMSSeedingMode::Uniform )
				? ManifoldSolverConfig::eSeedingUniform
				: ManifoldSolverConfig::eSeedingSnell;
			smsConfig.targetBounces = smsTargetBounces;
		}

		PathTracingSpectralRasterizer* pRasterizer = new PathTracingSpectralRasterizer(
			caster, lambda_begin, lambda_end, num_wavelengths, spectral_samples,
			smsConfig, adaptiveConfig, stabilityConfig, useZSobol, useHWSS, frameStore);

		if( pSamples && pFilter ) {
			pRasterizer->SubSampleRays( pSamples, pFilter );
		}

#ifdef RISE_ENABLE_OIDN
		pRasterizer->SetDenoisingEnabled( oidnDenoise );
		pRasterizer->SetDenoisingQuality( oidnQuality );
		pRasterizer->SetDenoisingDevice( oidnDevice );
		pRasterizer->SetDenoisingPrefilter( oidnPrefilter );
#else
		if( oidnDenoise ) {
			GlobalLog()->PrintEasyWarning( "OIDN denoising requested but RISE was compiled without RISE_ENABLE_OIDN support" );
		}
		(void)oidnQuality;
		(void)oidnDevice;
		(void)oidnPrefilter;
#endif

		(*ppi) = pRasterizer;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "PathTracing Spectral rasterizer" );
		return true;
	}

	bool RISE_API_SetRasterizerProgressiveRendering(
								IRasterizer* pRasterizer,
								const bool enabled,
								const unsigned int samplesPerPass
								)
	{
		PixelBasedRasterizerHelper* pHelper =
			dynamic_cast<PixelBasedRasterizerHelper*>( pRasterizer );
		if( !pHelper ) {
			return false;
		}

		ProgressiveConfig config;
		config.enabled = enabled;
		config.samplesPerPass = samplesPerPass > 0 ? samplesPerPass : 1;
		pHelper->SetProgressiveConfig( config );
		return true;
	}

	bool RISE_API_CreateMLTRasterizerWithFilter(
								IRasterizer** ppi,
								IRayCaster* caster,
								const unsigned int maxEyeDepth,
								const unsigned int maxLightDepth,
								const unsigned int nBootstrap,
								const unsigned int nChains,
								const unsigned int nMutationsPerPixel,
								const Scalar largeStepProb,
								const bool oidnDenoise,
								const OidnQuality oidnQuality,
							const OidnDevice oidnDevice,
							const OidnPrefilter oidnPrefilter,
								ISampling2D* pSampler,
								IPixelFilter* pFilter,
								Implementation::FrameStore* frameStore    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
								)
	{
		if( !ppi ) {
			return false;
		}

		MLTRasterizer* pRasterizer = new MLTRasterizer( caster, maxEyeDepth, maxLightDepth,
			nBootstrap, nChains, nMutationsPerPixel, largeStepProb, frameStore);

#ifdef RISE_ENABLE_OIDN
		pRasterizer->SetDenoisingEnabled( oidnDenoise );
		pRasterizer->SetDenoisingQuality( oidnQuality );
		pRasterizer->SetDenoisingDevice( oidnDevice );
		pRasterizer->SetDenoisingPrefilter( oidnPrefilter );
#else
		if( oidnDenoise ) {
			GlobalLog()->PrintEasyWarning( "OIDN denoising requested but RISE was compiled without RISE_ENABLE_OIDN support" );
		}
		(void)oidnQuality;
		(void)oidnDevice;
		(void)oidnPrefilter;
#endif

		// Install the pixel filter (and sampler, if any) on the base
		// class.  The MLT render loop doesn't consume the sampler, but
		// SubSampleRays() addref's both so the filter pointer stays
		// valid for the rasterizer's entire lifetime.  This wiring is
		// what lets MLT splats go through SplatFilm::SplatFiltered
		// instead of the old integer point-splat path.
		if( pSampler || pFilter ) {
			pRasterizer->SubSampleRays( pSampler, pFilter );
		}

		(*ppi) = pRasterizer;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "MLT rasterizer" );
		return true;
	}

	// Legacy wrapper preserving the pre-filter ABI.  Forwards to the
	// *WithFilter variant with null sampler/filter, which reproduces
	// the old unfiltered (round-to-nearest point splat) behaviour.
	// External callers linked against the pre-filter RISE can keep
	// using this symbol.  New code should call the *WithFilter variant.
	bool RISE_API_CreateMLTRasterizer(
								IRasterizer** ppi,
								IRayCaster* caster,
								const unsigned int maxEyeDepth,
								const unsigned int maxLightDepth,
								const unsigned int nBootstrap,
								const unsigned int nChains,
								const unsigned int nMutationsPerPixel,
								const Scalar largeStepProb,
								const bool oidnDenoise,
								Implementation::FrameStore* frameStore    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
								)
	{
		return RISE_API_CreateMLTRasterizerWithFilter( ppi, caster,
			maxEyeDepth, maxLightDepth, nBootstrap, nChains,
			nMutationsPerPixel, largeStepProb, oidnDenoise, OidnQuality::Auto, OidnDevice::Auto, OidnPrefilter::Fast, 0, 0,
			frameStore );  // L6a-2: thread through
	}

	bool RISE_API_CreateMLTSpectralRasterizerWithFilter(
								IRasterizer** ppi,
								IRayCaster* caster,
								const unsigned int maxEyeDepth,
								const unsigned int maxLightDepth,
								const unsigned int nBootstrap,
								const unsigned int nChains,
								const unsigned int nMutationsPerPixel,
								const Scalar largeStepProb,
								const Scalar lambda_begin,
								const Scalar lambda_end,
								const unsigned int nSpectralSamples,
								const bool useHWSS,
								const bool oidnDenoise,
								const OidnQuality oidnQuality,
							const OidnDevice oidnDevice,
							const OidnPrefilter oidnPrefilter,
								ISampling2D* pSampler,
								IPixelFilter* pFilter,
								Implementation::FrameStore* frameStore    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
								)
	{
		if( !ppi ) {
			return false;
		}

		MLTSpectralRasterizer* pRasterizer = new MLTSpectralRasterizer( caster, maxEyeDepth, maxLightDepth,
			nBootstrap, nChains, nMutationsPerPixel, largeStepProb,
			lambda_begin, lambda_end, nSpectralSamples, useHWSS, frameStore);

#ifdef RISE_ENABLE_OIDN
		pRasterizer->SetDenoisingEnabled( oidnDenoise );
		pRasterizer->SetDenoisingQuality( oidnQuality );
		pRasterizer->SetDenoisingDevice( oidnDevice );
		pRasterizer->SetDenoisingPrefilter( oidnPrefilter );
#else
		if( oidnDenoise ) {
			GlobalLog()->PrintEasyWarning( "OIDN denoising requested but RISE was compiled without RISE_ENABLE_OIDN support" );
		}
		(void)oidnQuality;
		(void)oidnDevice;
		(void)oidnPrefilter;
#endif

		// See RISE_API_CreateMLTRasterizerWithFilter above — install the
		// filter on the base-class members so the spectral MLT splat
		// path can spread each contribution through its reconstruction
		// kernel instead of truncating to an integer pixel.
		if( pSampler || pFilter ) {
			pRasterizer->SubSampleRays( pSampler, pFilter );
		}

		(*ppi) = pRasterizer;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "MLT Spectral rasterizer" );
		return true;
	}

	// Legacy wrapper — see RISE_API_CreateMLTRasterizer above.
	bool RISE_API_CreateMLTSpectralRasterizer(
								IRasterizer** ppi,
								IRayCaster* caster,
								const unsigned int maxEyeDepth,
								const unsigned int maxLightDepth,
								const unsigned int nBootstrap,
								const unsigned int nChains,
								const unsigned int nMutationsPerPixel,
								const Scalar largeStepProb,
								const Scalar lambda_begin,
								const Scalar lambda_end,
								const unsigned int nSpectralSamples,
								const bool useHWSS,
								const bool oidnDenoise,
								Implementation::FrameStore* frameStore    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
								)
	{
		return RISE_API_CreateMLTSpectralRasterizerWithFilter( ppi, caster,
			maxEyeDepth, maxLightDepth, nBootstrap, nChains,
			nMutationsPerPixel, largeStepProb, lambda_begin, lambda_end,
			nSpectralSamples, useHWSS, oidnDenoise, OidnQuality::Auto, OidnDevice::Auto, OidnPrefilter::Fast, 0, 0,
			frameStore );  // L6a-2: thread through
	}
}

//////////////////////////////////////////////////////////
// Parsers
//////////////////////////////////////////////////////////

#include "Parsers/AsciiSceneParser.h"
#include "Parsers/AsciiScriptParser.h"
#include "Parsers/AsciiCommandParser.h"
#include "Options.h"

namespace RISE
{
	bool RISE_API_CreateAsciiSceneParser(
								ISceneParser** ppi,					///< [out] Pointer to recieve the parser
								const char* name					///< [in] Name of the file to load
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new AsciiSceneParser( name );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "ascii scene parser" );
		return true;
	}

	//! Creates a parser capable of processing a script from a text file
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAsciiScriptParser(
								IScriptParser** ppi,				///< [out] Pointer to recieve the parser
								const char* name					///< [in] Name of the file to load
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new AsciiScriptParser( name );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "ascii script parser" );
		return true;
	}

	//! Creates a command parser capable of parsing string commands and putting them
	//! in a job
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAsciiCommandParser(
								ICommandParser** ppi				///< [out] Pointer to recieve the parser
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new AsciiCommandParser();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "ascii command parser" );
		return true;
	}

	//! Creates an options file parser
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateOptionsParser(
								IOptions** ppi,						///< [out] Pointer to recieve the parser
								const char* name					///< [in] Name of the file to load
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new Options( name );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "options parser" );
		return true;
	}
}

//////////////////////////////////////////////////////////
// Virtual measurement devices
//////////////////////////////////////////////////////////

#include "DetectorSpheres/DetectorSphere.h"

namespace RISE
{
	//! Creates a virtual detector sphere used in a virtual goniophotometer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDetectorSphere(
								IDetectorSphere** ppi,				///< [out] Pointer to recieve the detector sphere
								unsigned int numTheta,				///< [in] Number of patches in theta
								unsigned int numPhi,				///< [in] Number of patches in phi
								Scalar radius,						///< [in] Radius of the sphere
								int discretization					///< [in] How to discretize the patches
																	///<      0 - Equal Angles
																	///<      1 - Equal Areas (equal solid angle)
																	///<      2 - Equal Projected solid angles
								)
	{
		if( !ppi ) {
			return false;
		}

		DetectorSphere* pSphere = new DetectorSphere();
		pSphere->InitPatches( numTheta, numPhi, radius, (DetectorSphere::PatchDiscretization)discretization );

		(*ppi) = pSphere;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "detector sphere" );
		return true;
	}
}

///////////////////////////////////////////////////////////
// Phase functions and participating media
///////////////////////////////////////////////////////////

#include "Materials/IsotropicPhaseFunction.h"
#include "Materials/HenyeyGreensteinPhaseFunction.h"
#include "Materials/HomogeneousMedium.h"
#include "Materials/HeterogeneousMedium.h"
#include "Volume/Volume.h"
#include "Volume/VolumeAccessor_NNB.h"
#include "Volume/VolumeAccessor_TRI.h"
#include "Volume/VolumeAccessor_TriCubic.h"
#include "Volume/VolumeAccessor_Painter.h"
#include "Utilities/CubicInterpolator.h"

namespace RISE
{
	bool RISE_API_CreateIsotropicPhaseFunction(
								IPhaseFunction** ppi				///< [out] Pointer to recieve the phase function
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new IsotropicPhaseFunction();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "isotropic phase function" );
		return true;
	}

	bool RISE_API_CreateHenyeyGreensteinPhaseFunction(
								IPhaseFunction** ppi,				///< [out] Pointer to recieve the phase function
								const Scalar g						///< [in] Asymmetry factor [-1, 1]
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new HenyeyGreensteinPhaseFunction( g );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "henyey-greenstein phase function" );
		return true;
	}

	bool RISE_API_CreateHomogeneousMedium(
								IMedium** ppi,						///< [out] Pointer to recieve the medium
								const RISEPel& sigma_a,				///< [in] Absorption coefficient
								const RISEPel& sigma_s,				///< [in] Scattering coefficient
								const IPhaseFunction& phase			///< [in] Phase function for scattering
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new HomogeneousMedium( sigma_a, sigma_s, phase );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "homogeneous medium" );
		return true;
	}

	bool RISE_API_CreateHomogeneousMediumWithEmission(
								IMedium** ppi,						///< [out] Pointer to recieve the medium
								const RISEPel& sigma_a,				///< [in] Absorption coefficient
								const RISEPel& sigma_s,				///< [in] Scattering coefficient
								const RISEPel& emission,			///< [in] Volumetric emission
								const IPhaseFunction& phase			///< [in] Phase function for scattering
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new HomogeneousMedium( sigma_a, sigma_s, emission, phase );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "homogeneous medium with emission" );
		return true;
	}

	/// Helper to create a volume accessor from a type character
	static IVolumeAccessor* MediumVolumeAccessorFromChar( const char accessor )
	{
		switch( accessor ) {
			default:
				GlobalLog()->PrintEasyWarning( "HeterogeneousMedium:: Invalid accessor specified, defaulting to trilinear" );
			case 't':
				return new VolumeAccessor_TRI();
			case 'n':
				return new VolumeAccessor_NNB();
			case 'c':
				{
					ICubicInterpolator<Scalar>* interp = new CatmullRomCubicInterpolator<Scalar>();
					IVolumeAccessor* pRet = new VolumeAccessor_TriCubic( *interp );
					interp->release();
					return pRet;
				}
		}
	}

	bool RISE_API_CreateHeterogeneousMedium(
								IMedium** ppi,
								const RISEPel& max_sigma_a,
								const RISEPel& max_sigma_s,
								const IPhaseFunction& phase,
								const char* szVolumeFilePattern,
								const unsigned int volWidth,
								const unsigned int volHeight,
								const unsigned int volStartZ,
								const unsigned int volEndZ,
								const char accessor,
								const Point3& bboxMin,
								const Point3& bboxMax
								)
	{
		if( !ppi ) {
			return false;
		}

		// Load the volume data
		Volume<unsigned char>* pVol = new Volume<unsigned char>(
			szVolumeFilePattern, volWidth, volHeight, volStartZ, volEndZ );

		// Create the volume accessor
		IVolumeAccessor* pAccessor = MediumVolumeAccessorFromChar( accessor );
		pAccessor->BindVolume( pVol );
		safe_release( pVol );

		const unsigned int volDepth = volEndZ - volStartZ + 1;

		(*ppi) = new HeterogeneousMedium(
			max_sigma_a, max_sigma_s, phase, *pAccessor,
			volWidth, volHeight, volDepth, bboxMin, bboxMax );
		safe_release( pAccessor );

		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "heterogeneous medium" );
		return true;
	}

	bool RISE_API_CreateHeterogeneousMediumWithEmission(
								IMedium** ppi,
								const RISEPel& max_sigma_a,
								const RISEPel& max_sigma_s,
								const RISEPel& emission,
								const IPhaseFunction& phase,
								const char* szVolumeFilePattern,
								const unsigned int volWidth,
								const unsigned int volHeight,
								const unsigned int volStartZ,
								const unsigned int volEndZ,
								const char accessor,
								const Point3& bboxMin,
								const Point3& bboxMax
								)
	{
		if( !ppi ) {
			return false;
		}

		Volume<unsigned char>* pVol = new Volume<unsigned char>(
			szVolumeFilePattern, volWidth, volHeight, volStartZ, volEndZ );

		IVolumeAccessor* pAccessor = MediumVolumeAccessorFromChar( accessor );
		pAccessor->BindVolume( pVol );
		safe_release( pVol );

		const unsigned int volDepth = volEndZ - volStartZ + 1;

		(*ppi) = new HeterogeneousMedium(
			max_sigma_a, max_sigma_s, emission, phase, *pAccessor,
			volWidth, volHeight, volDepth, bboxMin, bboxMax );
		safe_release( pAccessor );

		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "heterogeneous medium with emission" );
		return true;
	}

	bool RISE_API_CreatePainterHeterogeneousMedium(
								IMedium** ppi,
								const RISEPel& max_sigma_a,
								const RISEPel& max_sigma_s,
								const IPhaseFunction& phase,
								const IPainter& densityPainter,
								const unsigned int virtualResolution,
								const char colorToScalar,
								const Point3& bboxMin,
								const Point3& bboxMax
								)
	{
		if( !ppi ) {
			return false;
		}

		IVolumeAccessor* pAccessor = new VolumeAccessor_Painter(
			densityPainter, virtualResolution, virtualResolution, virtualResolution,
			bboxMin, bboxMax, colorToScalar );

		(*ppi) = new HeterogeneousMedium(
			max_sigma_a, max_sigma_s, phase, *pAccessor,
			virtualResolution, virtualResolution, virtualResolution,
			bboxMin, bboxMax );
		safe_release( pAccessor );

		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "painter heterogeneous medium" );
		return true;
	}

	bool RISE_API_CreatePainterHeterogeneousMediumWithEmission(
								IMedium** ppi,
								const RISEPel& max_sigma_a,
								const RISEPel& max_sigma_s,
								const RISEPel& emission,
								const IPhaseFunction& phase,
								const IPainter& densityPainter,
								const unsigned int virtualResolution,
								const char colorToScalar,
								const Point3& bboxMin,
								const Point3& bboxMax
								)
	{
		if( !ppi ) {
			return false;
		}

		IVolumeAccessor* pAccessor = new VolumeAccessor_Painter(
			densityPainter, virtualResolution, virtualResolution, virtualResolution,
			bboxMin, bboxMax, colorToScalar );

		(*ppi) = new HeterogeneousMedium(
			max_sigma_a, max_sigma_s, emission, phase, *pAccessor,
			virtualResolution, virtualResolution, virtualResolution,
			bboxMin, bboxMax );
		safe_release( pAccessor );

		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "painter heterogeneous medium with emission" );
		return true;
	}

	//////////////////////////////////////////////////////////
	// Interactive scene editor — C-API entry points
	//////////////////////////////////////////////////////////
}

#include "RISE_API.h"
#include "SceneEditor/SceneEditController.h"

namespace RISE
{
	bool RISE_API_CreateSceneEditController(
		IJobPriv* pJob,
		IRasterizer* pInteractiveRasterizer,
		SceneEditController** ppOut )
	{
		if( !pJob || !ppOut ) return false;
		(*ppOut) = new SceneEditController( *pJob, pInteractiveRasterizer );
		return (*ppOut) != 0;
	}

	void RISE_API_DestroySceneEditController( SceneEditController* p )
	{
		delete p;
	}

	bool RISE_API_SceneEditController_Start( SceneEditController* p )
	{
		if( !p ) return false;
		p->Start();
		return true;
	}

	bool RISE_API_SceneEditController_Stop( SceneEditController* p )
	{
		if( !p ) return false;
		p->Stop();
		return true;
	}

	bool RISE_API_SceneEditController_SetPreviewSink(
		SceneEditController* p, IRasterizerOutput* sink )
	{
		if( !p ) return false;
		p->SetPreviewSink( sink );
		return true;
	}

	bool RISE_API_SceneEditController_SetProgressSink(
		SceneEditController* p, IProgressCallback* sink )
	{
		if( !p ) return false;
		p->SetProgressSink( sink );
		return true;
	}

	bool RISE_API_SceneEditController_SetLogSink(
		SceneEditController* p, ILogPrinter* sink )
	{
		if( !p ) return false;
		p->SetLogSink( sink );
		return true;
	}

	bool RISE_API_SceneEditController_SetTool( SceneEditController* p, int tool )
	{
		if( !p ) return false;
		// Upper bound is RollCamera (=8), the highest enum value.  An
		// earlier version used ScrubTimeline (=7) as the upper bound,
		// which silently rejected RollCamera selections at the C-API
		// boundary — the bridge would call SetTool(8), the call would
		// return false, and the controller's mTool stayed at whatever
		// the previous selection was.  Visually, clicking the Roll
		// button looked like the previous tool was still active —
		// e.g., dragging behaved like Zoom (move camera) instead of
		// Roll (rotate around camera→lookat axis).
		if( tool < SceneEditTool_Select || tool > SceneEditTool_RollCamera ) return false;
		p->SetTool( static_cast<SceneEditController::Tool>( tool ) );
		return true;
	}

	int RISE_API_SceneEditController_CurrentTool( SceneEditController* p )
	{
		if( !p ) return SceneEditTool_Select;
		return static_cast<int>( p->CurrentTool() );
	}

	int RISE_API_SceneEditController_CategoryForTool( int tool )
	{
		if( tool < SceneEditTool_Select || tool > SceneEditTool_RollCamera ) {
			return static_cast<int>( SceneEditController::ToolCategory::Select );
		}
		const auto t = static_cast<SceneEditController::Tool>( tool );
		return static_cast<int>( SceneEditController::CategoryForTool( t ) );
	}

	int RISE_API_SceneEditController_DefaultSubToolForCategory( int category )
	{
		if( category < 0 || category >= SceneEditController::kNumToolCategories ) {
			return SceneEditTool_Select;
		}
		const auto cat = static_cast<SceneEditController::ToolCategory>( category );
		return static_cast<int>( SceneEditController::DefaultSubToolForCategory( cat ) );
	}

	int RISE_API_SceneEditController_GetLastSubToolForCategory(
		SceneEditController* p, int category )
	{
		if( !p ) return SceneEditTool_Select;
		if( category < 0 || category >= SceneEditController::kNumToolCategories ) {
			return SceneEditTool_Select;
		}
		const auto cat = static_cast<SceneEditController::ToolCategory>( category );
		return static_cast<int>( p->GetLastSubToolForCategory( cat ) );
	}

	bool RISE_API_SceneEditController_RefreshGizmoHandles( SceneEditController* p )
	{
		if( !p ) return false;
		p->RefreshGizmoHandles();
		return true;
	}

	unsigned int RISE_API_SceneEditController_GizmoHandleCount(
		SceneEditController* p )
	{
		if( !p ) return 0;
		return p->GizmoHandleCount();
	}

	bool RISE_API_SceneEditController_GizmoHandle(
		SceneEditController* p, unsigned int idx,
		int* outKind, int* outAxis,
		double* outScreenX, double* outScreenY, double* outScreenRadius )
	{
		if( !p ) return false;
		if( idx >= p->GizmoHandleCount() ) return false;
		if( outKind         ) *outKind         = p->GizmoHandleKind( idx );
		if( outAxis         ) *outAxis         = p->GizmoHandleAxis( idx );
		if( outScreenX      ) *outScreenX      = p->GizmoHandleScreenX( idx );
		if( outScreenY      ) *outScreenY      = p->GizmoHandleScreenY( idx );
		if( outScreenRadius ) *outScreenRadius = p->GizmoHandleScreenRadius( idx );
		return true;
	}

	int RISE_API_SceneEditController_GizmoHandleAt(
		SceneEditController* p, Scalar x, Scalar y )
	{
		if( !p ) return -1;
		return p->GizmoHandleAt( Point2( x, y ) );
	}

	bool RISE_API_SceneEditController_IsGizmoDragActive( SceneEditController* p )
	{
		if( !p ) return false;
		return p->IsGizmoDragActive();
	}

	int RISE_API_SceneEditController_ActiveGizmoKind( SceneEditController* p )
	{
		if( !p ) return -1;
		return p->ActiveGizmoKind();
	}

	int RISE_API_SceneEditController_ActiveGizmoAxis( SceneEditController* p )
	{
		if( !p ) return -1;
		return p->ActiveGizmoAxis();
	}

	bool RISE_API_SceneEditController_OnPointerDown(
		SceneEditController* p, Scalar x, Scalar y )
	{
		if( !p ) return false;
		p->OnPointerDown( Point2( x, y ) );
		return true;
	}

	bool RISE_API_SceneEditController_OnPointerMove(
		SceneEditController* p, Scalar x, Scalar y )
	{
		if( !p ) return false;
		p->OnPointerMove( Point2( x, y ) );
		return true;
	}

	bool RISE_API_SceneEditController_OnPointerUp(
		SceneEditController* p, Scalar x, Scalar y )
	{
		if( !p ) return false;
		p->OnPointerUp( Point2( x, y ) );
		return true;
	}

	bool RISE_API_SceneEditController_OnTimeScrubBegin( SceneEditController* p )
	{
		if( !p ) return false;
		p->OnTimeScrubBegin();
		return true;
	}

	bool RISE_API_SceneEditController_OnTimeScrub( SceneEditController* p, Scalar t )
	{
		if( !p ) return false;
		p->OnTimeScrub( t );
		return true;
	}

	bool RISE_API_SceneEditController_OnTimeScrubEnd( SceneEditController* p )
	{
		if( !p ) return false;
		p->OnTimeScrubEnd();
		return true;
	}

	bool RISE_API_SceneEditController_BeginPropertyScrub( SceneEditController* p )
	{
		if( !p ) return false;
		p->BeginPropertyScrub();
		return true;
	}

	bool RISE_API_SceneEditController_EndPropertyScrub( SceneEditController* p )
	{
		if( !p ) return false;
		p->EndPropertyScrub();
		return true;
	}

	bool RISE_API_SceneEditController_Undo( SceneEditController* p )
	{
		if( !p ) return false;
		p->Undo();
		return true;
	}

	bool RISE_API_SceneEditController_Redo( SceneEditController* p )
	{
		if( !p ) return false;
		p->Redo();
		return true;
	}

	// ---- Phase 6.5 scene-file save ----------------------------------

	bool RISE_API_SceneEditController_HasUnsavedChanges( SceneEditController* p )
	{
		if( !p ) return false;
		return p->HasUnsavedChanges();
	}

	int RISE_API_SceneEditController_RequestSave(
		SceneEditController* p,
		const char* filePath,
		char* errOut,
		unsigned int errOutCap )
	{
		if( !p || !filePath ) return -1;
		const SaveResult r = p->RequestSave( std::string( filePath ) );
		if( errOut && errOutCap > 0 ) {
			errOut[0] = '\0';
			const std::string& msg = r.errorMessage;
			if( !msg.empty() ) {
				const unsigned int n =
					( msg.size() + 1 < errOutCap ) ? (unsigned int)msg.size()
					                               : ( errOutCap - 1 );
				std::memcpy( errOut, msg.data(), n );
				errOut[n] = '\0';
			}
		}
		return static_cast<int>( r.status );
	}

	bool RISE_API_SceneEditController_SetDirtyChangedCallback(
		SceneEditController* p,
		RISE_API_DirtyChangedFn cb,
		void* userData )
	{
		if( !p ) return false;
		if( cb ) {
			// Capture by value: cb + userData live as long as the
			// std::function; controller owns the function, so the
			// closure outlives the C shim's stack frame.
			p->SetDirtyChangedListener(
				[cb, userData]( bool hasUnsaved ) {
					cb( userData, hasUnsaved ? 1 : 0 );
				} );
		} else {
			p->SetDirtyChangedListener( SceneEditController::DirtyChangedFn() );
		}
		return true;
	}

	bool RISE_API_SceneEditController_LastSceneTime( SceneEditController* p, double* out )
	{
		if( !p || !out ) return false;
		*out = static_cast<double>( p->LastSceneTime() );
		return true;
	}

	bool RISE_API_SceneEditController_RequestProductionRender( SceneEditController* p )
	{
		if( !p ) return false;
		return p->RequestProductionRender();
	}

	// Properties panel ------------------------------------------------

	static void CopyToBuf( const String& s, char* buf, unsigned int bufLen )
	{
		if( !buf || bufLen == 0 ) return;
		const char* src = s.c_str();
		unsigned int i = 0;
		while( src && src[i] && i + 1 < bufLen ) { buf[i] = src[i]; ++i; }
		buf[i] = 0;
	}

	bool RISE_API_SceneEditController_RefreshProperties( SceneEditController* p )
	{
		if( !p ) return false;
		p->RefreshProperties();
		return true;
	}

	int RISE_API_SceneEditController_PanelMode( SceneEditController* p )
	{
		if( !p ) return -1;
		return static_cast<int>( p->CurrentPanelMode() );
	}

	bool RISE_API_SceneEditController_PanelHeader(
		SceneEditController* p, char* buf, unsigned int bufLen )
	{
		if( !p ) return false;
		CopyToBuf( p->CurrentPanelHeader(), buf, bufLen );
		return true;
	}

	unsigned int RISE_API_SceneEditController_PropertyCount( SceneEditController* p )
	{
		if( !p ) return 0;
		return p->PropertyCount();
	}

	bool RISE_API_SceneEditController_PropertyName(
		SceneEditController* p, unsigned int idx, char* buf, unsigned int bufLen )
	{
		if( !p || idx >= p->PropertyCount() ) return false;
		CopyToBuf( p->PropertyName( idx ), buf, bufLen );
		return true;
	}

	bool RISE_API_SceneEditController_PropertyValue(
		SceneEditController* p, unsigned int idx, char* buf, unsigned int bufLen )
	{
		if( !p || idx >= p->PropertyCount() ) return false;
		CopyToBuf( p->PropertyValue( idx ), buf, bufLen );
		return true;
	}

	bool RISE_API_SceneEditController_PropertyDescription(
		SceneEditController* p, unsigned int idx, char* buf, unsigned int bufLen )
	{
		if( !p || idx >= p->PropertyCount() ) return false;
		CopyToBuf( p->PropertyDescription( idx ), buf, bufLen );
		return true;
	}

	int RISE_API_SceneEditController_PropertyKind(
		SceneEditController* p, unsigned int idx )
	{
		if( !p ) return -1;
		return p->PropertyKind( idx );
	}

	bool RISE_API_SceneEditController_PropertyEditable(
		SceneEditController* p, unsigned int idx )
	{
		if( !p ) return false;
		return p->PropertyEditable( idx );
	}

	unsigned int RISE_API_SceneEditController_PropertyPresetCount(
		SceneEditController* p, unsigned int idx )
	{
		if( !p ) return 0;
		return p->PropertyPresetCount( idx );
	}

	bool RISE_API_SceneEditController_PropertyPresetLabel(
		SceneEditController* p, unsigned int idx, unsigned int presetIdx,
		char* buf, unsigned int bufLen )
	{
		if( !p || idx >= p->PropertyCount() || presetIdx >= p->PropertyPresetCount( idx ) ) return false;
		CopyToBuf( p->PropertyPresetLabel( idx, presetIdx ), buf, bufLen );
		return true;
	}

	bool RISE_API_SceneEditController_PropertyPresetValue(
		SceneEditController* p, unsigned int idx, unsigned int presetIdx,
		char* buf, unsigned int bufLen )
	{
		if( !p || idx >= p->PropertyCount() || presetIdx >= p->PropertyPresetCount( idx ) ) return false;
		CopyToBuf( p->PropertyPresetValue( idx, presetIdx ), buf, bufLen );
		return true;
	}

	bool RISE_API_SceneEditController_PropertyUnitLabel(
		SceneEditController* p, unsigned int idx,
		char* buf, unsigned int bufLen )
	{
		if( !p || idx >= p->PropertyCount() ) return false;
		CopyToBuf( p->PropertyUnitLabel( idx ), buf, bufLen );
		return true;
	}

	bool RISE_API_SceneEditController_SetProperty(
		SceneEditController* p, const char* name, const char* valueStr )
	{
		if( !p || !name || !valueStr ) return false;
		return p->SetProperty( String( name ), String( valueStr ) );
	}

	bool RISE_API_SceneEditController_GetAnimationOptions(
		SceneEditController* p,
		double* outTimeStart, double* outTimeEnd,
		unsigned int* outNumFrames )
	{
		if( !p || !outTimeStart || !outTimeEnd || !outNumFrames ) return false;
		double t0 = 0, t1 = 0;
		unsigned int nf = 0;
		if( !p->GetAnimationOptions( t0, t1, nf ) ) return false;
		*outTimeStart = t0;
		*outTimeEnd   = t1;
		*outNumFrames = nf;
		return true;
	}

	bool RISE_API_SceneEditController_GetCameraDimensions(
		SceneEditController* p, unsigned int* outW, unsigned int* outH )
	{
		if( !p || !outW || !outH ) return false;
		unsigned int w = 0, h = 0;
		if( !p->GetCameraDimensions( w, h ) ) return false;
		*outW = w;
		*outH = h;
		return true;
	}

	// Accordion list entries -----------------------------------------

	unsigned int RISE_API_SceneEditController_CategoryEntityCount(
		SceneEditController* p, int category )
	{
		if( !p ) return 0;
		return p->CategoryEntityCount(
			static_cast<SceneEditController::Category>( category ) );
	}

	bool RISE_API_SceneEditController_CategoryEntityName(
		SceneEditController* p, int category, unsigned int idx,
		char* buf, unsigned int bufLen )
	{
		if( !p ) return false;
		const SceneEditController::Category cat =
			static_cast<SceneEditController::Category>( category );
		if( idx >= p->CategoryEntityCount( cat ) ) return false;
		CopyToBuf( p->CategoryEntityName( cat, idx ), buf, bufLen );
		return true;
	}

	int RISE_API_SceneEditController_GetSelectionCategory( SceneEditController* p )
	{
		if( !p ) return -1;
		return static_cast<int>( p->GetSelectionCategory() );
	}

	bool RISE_API_SceneEditController_GetSelectionName(
		SceneEditController* p, char* buf, unsigned int bufLen )
	{
		if( !p ) return false;
		CopyToBuf( p->GetSelectionName(), buf, bufLen );
		return true;
	}

	bool RISE_API_SceneEditController_SetSelection(
		SceneEditController* p, int category, const char* name )
	{
		if( !p ) return false;
		const String entityName = name ? String( name ) : String();
		return p->SetSelection(
			static_cast<SceneEditController::Category>( category ),
			entityName );
	}

	bool RISE_API_SceneEditController_CategoryActiveName(
		SceneEditController* p, int category, char* buf, unsigned int bufLen )
	{
		if( !p ) return false;
		const SceneEditController::Category cat =
			static_cast<SceneEditController::Category>( category );
		CopyToBuf( p->CategoryActiveName( cat ), buf, bufLen );
		return true;
	}

	unsigned int RISE_API_SceneEditController_SceneEpoch( SceneEditController* p )
	{
		if( !p ) return 0;
		return p->SceneEpoch();
	}

	bool RISE_API_SceneEditController_AddCameraFromActive(
		SceneEditController* p,
		const char* proposedName,
		char* outName, unsigned int outLen )
	{
		if( !p || !outName || outLen == 0 ) return false;
		const String prop = String( proposedName ? proposedName : "" );
		return p->CloneActiveCamera( prop, outName, outLen );
	}

	// -------------------------------------------------------------------
	// Phase 4b — per-category selection + per-category property accessors.
	// Each is a thin wrapper around the matching SceneEditController
	// method.  The buffer-handling pattern follows the rest of the
	// SceneEditController C-API: CopyToBuf-style truncation with null-
	// terminator, returns false on null controller / null buf / bufLen 0.
	// -------------------------------------------------------------------

	bool RISE_API_SceneEditController_GetSelectionForCategory(
		SceneEditController* p, int category, char* buf, unsigned int bufLen )
	{
		if( !p || !buf || bufLen == 0 ) return false;
		const SceneEditController::Category cat =
			static_cast<SceneEditController::Category>( category );
		CopyToBuf( p->GetSelectionNameForCategory( cat ), buf, bufLen );
		return true;
	}

	bool RISE_API_SceneEditController_IsSectionExpanded(
		SceneEditController* p, int category )
	{
		if( !p ) return false;
		return p->IsSectionExpanded( static_cast<SceneEditController::Category>( category ) );
	}

	bool RISE_API_SceneEditController_CollapseSection(
		SceneEditController* p, int category )
	{
		if( !p ) return false;
		p->CollapseSection( static_cast<SceneEditController::Category>( category ) );
		return true;
	}

	unsigned int RISE_API_SceneEditController_PropertyCountFor(
		SceneEditController* p, int category )
	{
		if( !p ) return 0;
		return p->PropertyCountFor( static_cast<SceneEditController::Category>( category ) );
	}

	bool RISE_API_SceneEditController_PropertyNameFor(
		SceneEditController* p, int category, unsigned int idx,
		char* buf, unsigned int bufLen )
	{
		if( !p || !buf || bufLen == 0 ) return false;
		CopyToBuf( p->PropertyNameFor( static_cast<SceneEditController::Category>( category ), idx ), buf, bufLen );
		return true;
	}

	bool RISE_API_SceneEditController_PropertyValueFor(
		SceneEditController* p, int category, unsigned int idx,
		char* buf, unsigned int bufLen )
	{
		if( !p || !buf || bufLen == 0 ) return false;
		CopyToBuf( p->PropertyValueFor( static_cast<SceneEditController::Category>( category ), idx ), buf, bufLen );
		return true;
	}

	bool RISE_API_SceneEditController_PropertyDescriptionFor(
		SceneEditController* p, int category, unsigned int idx,
		char* buf, unsigned int bufLen )
	{
		if( !p || !buf || bufLen == 0 ) return false;
		CopyToBuf( p->PropertyDescriptionFor( static_cast<SceneEditController::Category>( category ), idx ), buf, bufLen );
		return true;
	}

	int RISE_API_SceneEditController_PropertyKindFor(
		SceneEditController* p, int category, unsigned int idx )
	{
		if( !p ) return -1;
		return p->PropertyKindFor( static_cast<SceneEditController::Category>( category ), idx );
	}

	bool RISE_API_SceneEditController_PropertyEditableFor(
		SceneEditController* p, int category, unsigned int idx )
	{
		if( !p ) return false;
		return p->PropertyEditableFor( static_cast<SceneEditController::Category>( category ), idx );
	}

	unsigned int RISE_API_SceneEditController_PropertyPresetCountFor(
		SceneEditController* p, int category, unsigned int idx )
	{
		if( !p ) return 0;
		return p->PropertyPresetCountFor( static_cast<SceneEditController::Category>( category ), idx );
	}

	bool RISE_API_SceneEditController_PropertyPresetLabelFor(
		SceneEditController* p, int category, unsigned int idx, unsigned int presetIdx,
		char* buf, unsigned int bufLen )
	{
		if( !p || !buf || bufLen == 0 ) return false;
		CopyToBuf( p->PropertyPresetLabelFor(
			static_cast<SceneEditController::Category>( category ), idx, presetIdx ), buf, bufLen );
		return true;
	}

	bool RISE_API_SceneEditController_PropertyPresetValueFor(
		SceneEditController* p, int category, unsigned int idx, unsigned int presetIdx,
		char* buf, unsigned int bufLen )
	{
		if( !p || !buf || bufLen == 0 ) return false;
		CopyToBuf( p->PropertyPresetValueFor(
			static_cast<SceneEditController::Category>( category ), idx, presetIdx ), buf, bufLen );
		return true;
	}

	bool RISE_API_SceneEditController_PropertyUnitLabelFor(
		SceneEditController* p, int category, unsigned int idx,
		char* buf, unsigned int bufLen )
	{
		if( !p || !buf || bufLen == 0 ) return false;
		CopyToBuf( p->PropertyUnitLabelFor(
			static_cast<SceneEditController::Category>( category ), idx ), buf, bufLen );
		return true;
	}

	bool RISE_API_SceneEditController_SetPropertyForCategory(
		SceneEditController* p, int category, const char* name, const char* valueStr )
	{
		if( !p || !name || !valueStr ) return false;
		return p->SetPropertyForCategory(
			static_cast<SceneEditController::Category>( category ),
			String( name ), String( valueStr ) );
	}
}
