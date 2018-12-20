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
		const Vector2& target_orientation						///< [in] Orientation relative to a target
		)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new PinholeCamera( ptLocation, ptLookAt, vUp, fov, xres, yres, pixelAR, exposure, scanningRate, pixelRate, orientation, target_orientation );
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
		const Scalar pixelRate									///< [in] Rate at which each pixel is recorded
		)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new PinholeCamera( onb, ptLocation, fov, xres, yres, pixelAR, exposure, scanningRate, pixelRate );
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
		const Scalar fov,										///< [in] Field of view in radians
		const unsigned int xres,								///< [in] X resolution of virtual screen
		const unsigned int yres,								///< [in] Y resolution of virtual screen
		const Scalar pixelAR,									///< [in] Pixel aspect ratio
		const Scalar exposure,									///< [in] Exposure time of the camera
		const Scalar scanningRate,								///< [in] Rate at which each scanline is recorded
		const Scalar pixelRate,									///< [in] Rate at which each pixel is recorded
		const Vector3& orientation,								///< [in] Orientation (Pitch,Roll,Yaw)
		const Vector2& target_orientation,						///< [in] Orientation relative to a target
		const Scalar aperture,									///< [in] Size of the aperture
		const Scalar focalLength,								///< [in] Focal length
		const Scalar focusDistance								///< [in] Focus distance
		)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new ThinLensCamera( ptLocation, ptLookAt, vUp, fov, xres, yres, pixelAR, exposure, scanningRate, pixelRate, orientation, target_orientation, aperture, focalLength, focusDistance );
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
		const Scalar linearAttenuation,							///< [in] Amount of linear attenuation
		const Scalar quadraticAttenuation						///< [in] Amount of quadratic attenuation
		)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new PointLight( power, color, linearAttenuation, quadraticAttenuation );
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
		const Scalar linearAttenuation,							///< [in] Amount of linear attenuation
		const Scalar quadraticAttenuation						///< [in] Amount of quadratic attenuation
		)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new SpotLight( power, foc, inner, outer, color, linearAttenuation, quadraticAttenuation );
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
#include "Geometry/TriangleMeshGeometry.h"
#include "Geometry/TriangleMeshGeometryIndexed.h"
#include "Geometry/TriangleMeshLoader3DS.h"
#include "Geometry/TriangleMeshLoaderRAW.h"
#include "Geometry/TriangleMeshLoaderRAW2.h"
#include "Geometry/TriangleMeshLoaderBezier.h"
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

	//! Creates a triangle mesh geometry object
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTriangleMeshGeometry(
						ITriangleMeshGeometry** ppi,			///< [out] Pointer to recieve the geometry
						const unsigned int max_polys,			///< [in] Maximum number of polygons / octant node
						const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
						const bool double_sided,				///< [in] Are the triangles double sided ?
						const bool use_bsp						///< [in] Use a BSP tree rather than an Octree
						)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TriangleMeshGeometry( max_polys, max_recur, double_sided, use_bsp );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "triangle mesh" );
		return true;
	}

	//! Creates an indexed triangle mesh geometry object
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTriangleMeshGeometryIndexed(
						ITriangleMeshGeometryIndexed** ppi,	///< [out] Pointer to recieve the geometry
						const unsigned int max_polys,			///< [in] Maximum number of polygons / octant node
						const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
						const bool double_sided,				///< [in] Are the triangles double sided ?
						const bool use_bsp,					///< [in] Use a BSP tree rather than an Octree
						const bool face_normals				///< [in] Use face normals rather than vertex normals
						)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TriangleMeshGeometryIndexed( max_polys, max_recur, double_sided, use_bsp, face_normals );
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

	//! Creates a mesh loader capable of loading from a series of bezier patches
	/// \return TRUE if successful, FALSE otherwise
	/// \todo this is deprecated and should be removed
	bool RISE_API_CreateBezierTriangleMeshLoader(
						ITriangleMeshLoaderIndexed** ppi,		///< [out] Pointer to recieve the mesh loader
						const char* szFileName,					///< [in] Name of the file to load from
						const unsigned int detail,				///< [in] Level of tesselation
						const bool bCombineSharedVertices,		///< [in] Should we try to combine shared vertices?
						const bool bCenterObject,				///< [in] Should the object be re-centered around the origin
						IFunction2D* displacement,				///< [in] Displacement function for static displacement mapping
						const Scalar disp_scale					///< [in] Displacement scale factor
						)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TriangleMeshLoaderBezier( szFileName, detail, bCombineSharedVertices, bCenterObject, displacement, disp_scale );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "mesh loader bezier" );
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

	//! Creates a geometry object made up of a series of bezier patches
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBezierPatchGeometry(
						IBezierPatchGeometry** ppi,				///< [out] Pointer to recieve the geometry
						const unsigned int max_patches,			///< [in] Maximum number of patches / octant node
						const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
						const bool use_bsp,						///< [in] Use a BSP tree rather than an Octree
						const bool bAnalytic,					///< [in] Should the patches be analytically rendered?
						const unsigned int cache_size,			///< [in] Size of the geometry cache for non-analytic rendering
						const unsigned int max_polys,			///< [in] Maximum polygons / triangle octant node
						const unsigned char max_poly_recursion, ///< [in] Maximum polygon recursion
						const bool bDoubleSided,				///< [in] Are generated polygons double sided ?
						const bool bPolyUseBSP,					///< [in] Should the polygon list use a BSP tree?
						const bool bUseFaceNormals,				///< [in] Should we use face normals rather than vertex normals?
						const unsigned int detail,				///< [in] Level of tesselation for polygons
						IFunction2D* displacement,				///< [in] Displacement function for static displacement mapping
						const Scalar disp_scale					///< [in] Displacement scale factor
						)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new BezierPatchGeometry( max_patches, max_recur, use_bsp, bAnalytic, cache_size, max_polys, max_poly_recursion, bDoubleSided, bPolyUseBSP, bUseFaceNormals, detail, displacement, disp_scale );
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
#include "Materials/PerfectReflectorMaterial.h"
#include "Materials/PerfectRefractorMaterial.h"
#include "Materials/AshikminShirleyAnisotropicPhongMaterial.h"
#include "Materials/IsotropicPhongMaterial.h"
#include "Materials/TranslucentMaterial.h"
#include "Materials/BioSpecSkinMaterial.h"
#include "Materials/GenericHumanTissueMaterial.h"
#include "Materials/CompositeMaterial.h"
#include "Materials/WardIsotropicGaussianMaterial.h"
#include "Materials/WardAnisotropicEllipticalGaussianMaterial.h"
#include "Materials/CookTorranceMaterial.h"
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
								const IPainter& tau,			///< [in] Transmittance of the dielectric top
								const IPainter& Nt,				///< [in] Index of refraction of dielectric coating
								const IPainter& scat,			///< [in] Scattering function (either Phong or HG)
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
								const IPainter& tau,			///< [in] Transmittance
								const IPainter& rIndex,			///< [in] Index of refraction
								const IPainter& scat,			///< [in] Scattering function (either Phong or HG)
								const bool hg					///< [in] Use Henyey-Greenstein phase function scattering
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new DielectricMaterial( tau, rIndex, scat, hg );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "dielectric material" );
		return true;
	}

	//! Creates an isotropic phong material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateIsotropicPhongMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& rd,				///< [in] Diffuse reflectance
								const IPainter& rs,				///< [in] Specular reflectance
								const IPainter& exponent		///< [in] Phong exponent
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
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& rd,				///< [in] Diffuse reflectance
								const IPainter& rs,				///< [in] Specular reflectance
								const IPainter& Nu,				///< [in] Phong exponent in U
								const IPainter& Nv				///< [in] Phong exponent in V
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
								const IPainter& ior				///< [in] Index of refraction
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
								const IPainter& rF,				///< [in] Reflectance
								const IPainter& T,				///< [in] Transmittance
								const IPainter& ext,			///< [in] Extinction
								const IPainter& N,				///< [in] Phong exponent
								const IPainter& scat			///< [in] Multiple scattering component
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
								const IPainter& thickness_SC_,								///< Thickness of the stratum corneum (in cm)
								const IPainter& thickness_epidermis_,						///< Thickness of the epidermis (in cm)
								const IPainter& thickness_papillary_dermis_,				///< Thickness of the papillary dermis (in cm)
								const IPainter& thickness_reticular_dermis_,				///< Thickness of the reticular dermis (in cm)
								const IPainter& ior_SC_,									///< Index of refraction of the stratum corneum
								const IPainter& ior_epidermis_,								///< Index of refraction of the epidermis
								const IPainter& ior_papillary_dermis_,						///< Index of refraction of the papillary dermis
								const IPainter& ior_reticular_dermis_,						///< Index of refraction of the reticular dermis
								const IPainter& concentration_eumelanin_,					///< Average Concentration of eumelanin in the melanosomes
								const IPainter& concentration_pheomelanin_,					///< Average Concentration of pheomelanin in the melanosomes
								const IPainter& melanosomes_in_epidermis_,					///< Percentage of the epidermis made up of melanosomes
								const IPainter& hb_ratio_,									///< Ratio of oxyhemoglobin to deoxyhemoglobin in blood
								const IPainter& whole_blood_in_papillary_dermis_,			///< Percentage of the papillary dermis made up of whole blood
								const IPainter& whole_blood_in_reticular_dermis_,			///< Percentage of the reticular dermis made up of whole blood
								const IPainter& bilirubin_concentration_,					///< Concentration of Bilirubin in whole blood
								const IPainter& betacarotene_concentration_SC_,				///< Concentration of Beta-Carotene in the stratum corneum
								const IPainter& betacarotene_concentration_epidermis_,		///< Concentration of Beta-Carotene in the epidermis
								const IPainter& betacarotene_concentration_dermis_,			///< Concentration of Beta-Carotene in the dermis
								const IPainter& folds_aspect_ratio_,						///< Aspect ratio of the little folds and wrinkles on the skin surface
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

	//! Creates a generic human tissue material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGenericHumanTissueMaterial(
								IMaterial** ppi,											///< [out] Pointer to recieve the material
								const IPainter& sca,										///< [in] Scattering co-efficient
								const IPainter& g,											///< [in] The g factor in the HG phase function
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
								const Scalar thickness										///< [in] Thickness between the materials
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new CompositeMaterial( top, bottom, max_recur, max_reflection_recursion, max_refraction_recursion, max_diffuse_recursion, max_translucent_recursion, thickness );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "composite material" );
		return true;
	}

	//! Creates Ward's isotropic gaussian material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateWardIsotropicGaussianMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& diffuse,		///< [in] Diffuse reflectance
								const IPainter& specular,		///< [in] Specular reflectance
								const IPainter& alpha			///< [in] Standard deviation (RMS) of surface slope
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
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& diffuse,		///< [in] Diffuse reflectance
								const IPainter& specular,		///< [in] Specular reflectance
								const IPainter& alphax,			///< [in] Standard deviation (RMS) of surface slope in x
								const IPainter& alphay			///< [in] Standard deviation (RMS) of surface slope in y
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new WardAnisotropicEllipticalGaussianMaterial( diffuse, specular, alphax, alphay );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "ward anisotropic elliptical gaussian material" );
		return true;
	}

	//! Creates a Cook Torrance material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCookTorranceMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& diffuse,		///< [in] Diffuse reflectance
								const IPainter& specular,		///< [in] Specular reflectance
								const IPainter& facet,			///< [in] Facet distribution
								const IPainter& ior,			///< [in] IOR delta
								const IPainter& ext				///< [in] Extinction factor
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
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& reflectance,	///< [in] Reflectance
								const IPainter& roughness		///< [in] Roughness
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
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& diffuse,		///< [in] Diffuse reflectance
								const IPainter& specular,		///< [in] Specular reflectance
								const IPainter& roughness,		///< [in] Roughness
								const IPainter& isotropy		///< [in] Isotropy
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
								const IPainter& radEx,			///< [in] Radiance exitance
								const IMaterial& mat,			///< [in] Material to use for all non emmission properties
								const IPainter& N,				///< [in] Phong exponent
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
#include "Painters/SpectralColorPainter.h"
#include "Painters/TexturePainter.h"
#include "Painters/CheckerPainter.h"
#include "Painters/LinesPainter.h"
#include "Painters/MandelbrotPainter.h"
#include "Painters/Perlin2DPainter.h"
#include "Painters/Perlin3DPainter.h"
#include "Painters/Voronoi2DPainter.h"
#include "Painters/Voronoi3DPainter.h"
#include "Painters/IridescentPainter.h"
#include "Painters/Function1DSpectralPainter.h"
#include "Painters/BlackBodyPainter.h"
#include "Painters/BlendPainter.h"

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
								IRasterImageAccessor* pSA		///< [in] Raster Image accessor to the image containing the texture
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TexturePainter( pSA );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "texture painter" );
		return true;
	}

	//! Creates a painter that paints a uniform color
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateUniformColorPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const RISEPel& c					///< [in] Color to paint
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new UniformColorPainter( c );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "uniform color painter" );
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
}


//////////////////////////////////////////////////////////
// Radiance maps
//////////////////////////////////////////////////////////

#include "Rendering/RadianceMap.h"

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
}


//////////////////////////////////////////////////////////
// Managers
//////////////////////////////////////////////////////////

#include "Managers/ObjectManager.h"
#include "Managers/GeometryManager.h"
#include "Managers/LightManager.h"
#include "Managers/PainterManager.h"
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
								const bool nonmeshlights,			///< [in] Should we shoot from non mesh based lights?
								const bool useiorstack,				///< [in] Should the ray caster use a index of refraction stack?
								const Scalar power_scale,			///< [in] How much to scale light power by
								const unsigned int temporal_samples,///< [in] Number of temporal samples to take for animation frames
								const bool regenerate				///< [in] Should the tracer regenerate a new photon each time the scene time changes?
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new CausticPelPhotonTracer( maxR, minImp, branch, reflect, refract, nonmeshlights, useiorstack, power_scale, temporal_samples, regenerate );
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
								const bool nonmeshlights,			///< [in] Should we shoot from non mesh based lights?
								const bool useiorstack,				///< [in] Should the ray caster use a index of refraction stack?
								const Scalar power_scale,			///< [in] How much to scale light power by
								const unsigned int temporal_samples,///< [in] Number of temporal samples to take for animation frames
								const bool regenerate				///< [in] Should the tracer regenerate a new photon each time the scene time changes?
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new GlobalPelPhotonTracer( maxR, minImp, branch, nonmeshlights, useiorstack, power_scale, temporal_samples, regenerate );
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
								const bool nonmeshlights,			///< [in] Should we shoot from non mesh based lights?
								const bool useiorstack,				///< [in] Should the ray caster use a index of refraction stack?
								const Scalar power_scale,			///< [in] How much to scale light power by
								const unsigned int temporal_samples,///< [in] Number of temporal samples to take for animation frames
								const bool regenerate				///< [in] Should the tracer regenerate a new photon each time the scene time changes?
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new TranslucentPelPhotonTracer( maxR, minImp, reflect, refract, direct_translucent, nonmeshlights, useiorstack, power_scale, temporal_samples, regenerate );
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
								const bool useiorstack,				///< [in] Should the ray caster use a index of refraction stack?
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

		(*ppi) = new CausticSpectralPhotonTracer( maxR, minImp, nm_begin, nm_end, num_wavelengths, useiorstack, branch, reflect, refract, power_scale, temporal_samples, regenerate );
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
								const bool useiorstack,				///< [in] Should the ray caster use a index of refraction stack?
								const bool branch,					///< [in] Should the tracer branch or follow a single path?
								const Scalar power_scale,			///< [in] How much to scale light power by
								const unsigned int temporal_samples,///< [in] Number of temporal samples to take for animation frames
								const bool regenerate				///< [in] Should the tracer regenerate a new photon each time the scene time changes?
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new GlobalSpectralPhotonTracer( maxR, minImp, nm_begin, nm_end, num_wavelengths, useiorstack, branch, power_scale, temporal_samples, regenerate );
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
								const double max_spacing			///< [in] Maximum seperation
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new IrradianceCache( size, tolerance, min_spacing, max_spacing );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "irradiance cache" );
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
								IRasterizerOutput** ppi,			///< [out] Pointer to recieve the rasterizer output
								const char* szPattern,				///< [in] File pattern
								const bool bMultiple,				///< [in] Output multiple files (for animations usually)
								const char type,					///< [in] Type of file
																	///		0 - TGA
																	///		1 - PPM
																	///		2 - PNG
																	///		3 - HDR
																	///		4 - TIFF
																	///		5 - RGBEA
								const unsigned char bpp,			///< [in] Bits / pixel for the file
								const COLOR_SPACE color_space		///< [in] Color space to apply
								)
	{
		if( !ppi ) {
			return false;
		}

		switch( type )
		{
		case 0:
			(*ppi) = new FileRasterizerOutput( szPattern, bMultiple, FileRasterizerOutput::TGA, bpp, color_space);
			break;
		case 1:
			(*ppi) = new FileRasterizerOutput( szPattern, bMultiple, FileRasterizerOutput::PPM, bpp, color_space );
			break;
		default:
		case 2:
			(*ppi) = new FileRasterizerOutput( szPattern, bMultiple, FileRasterizerOutput::PNG, bpp, color_space );
			break;
		case 3:
			(*ppi) = new FileRasterizerOutput( szPattern, bMultiple, FileRasterizerOutput::HDR, bpp, color_space );
			break;
		case 4:
			(*ppi) = new FileRasterizerOutput( szPattern, bMultiple, FileRasterizerOutput::TIFF, bpp, color_space );
			break;
		case 5:
			(*ppi) = new FileRasterizerOutput( szPattern, bMultiple, FileRasterizerOutput::RGBEA, bpp, color_space );
			break;
		case 6:
			(*ppi) = new FileRasterizerOutput( szPattern, bMultiple, FileRasterizerOutput::EXR, bpp, color_space );
			break;
		}

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

namespace RISE
{
	//! Creates a block rasterize sequence
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

	//! Creates a scanline rasterize sequence
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

	//! Creates a hilbert space filling curve rasterize sequence
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
								IRasterImage& image						///< [in] Raster Image to access
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new NNBRasterImageAccessor<RISEColor>( image );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "NNB RIA" );
		return true;
	}

	//! Creates a bilinear raster image accessor
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBiLinRasterImageAccessor(
								IRasterImageAccessor** ppi,				///< [out] Pointer to recieve the accessor
								IRasterImage& image						///< [in] Raster Image to access
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new BilinRasterImageAccessor<RISEColor>( image );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "Bilin RIA" );
		return true;
	}

	//! Creates a catmull rom bicubic raster image accessor
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCatmullRomBicubicRasterImageAccessor(
								IRasterImageAccessor** ppi,			///< [out] Pointer to recieve the accessor
								IRasterImage& image					///< [in] Raster image to access
								)
	{
		if( !ppi ) {
			return false;
		}

		ICubicInterpolator<RISEColor>* interp = new CatmullRomCubicInterpolator<RISEColor>();

		(*ppi) = new BicubicRasterImageAccessor<RISEColor>( image, *interp );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "catmull rom RIA" );

		interp->release();
		return true;
	}

	//! Creates a bspline bicubic raster image accessor
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateUniformBSplineBicubicRasterImageAccessor(
								IRasterImageAccessor** ppi,			///< [out] Pointer to recieve the accessor
								IRasterImage& image					///< [in] Raster image to access
								)
	{
		if( !ppi ) {
			return false;
		}

		ICubicInterpolator<RISEColor>* interp = new UniformBSplineCubicInterpolator<RISEColor>();

		(*ppi) = new BicubicRasterImageAccessor<RISEColor>( image, *interp );
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "uniform bspline RIA" );

		interp->release();
		return true;
	}

	//! Creates a bicubic raster image accessor that uses the given matrix as the weights
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBicubicRasterImageAccessor(
								IRasterImageAccessor** ppi,			///< [out] Pointer to recieve the accessor
								IRasterImage& image,				///< [in] Raster image to access
								const Matrix4& m
								)
	{
		if( !ppi ) {
			return false;
		}

		ICubicInterpolator<RISEColor>* interp = new CubicInterpolator<RISEColor>(m);

		(*ppi) = new BicubicRasterImageAccessor<RISEColor>( image, *interp );
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
								const COLOR_SPACE color_space		///< [in] Color space to apply
								)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new EXRWriter( buffer, color_space );
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
#include "Shaders/SSS/SubSurfaceScatteringShaderOp.h"
#include "Shaders/SSS/SimpleExtinction.h"
#include "Shaders/SSS/DiffusionApproximationExtinction.h"
#include "Shaders/AreaLightShaderOp.h"
#include "Shaders/TransparencyShaderOp.h"

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
								const IMaterial* bsdf,			///< [in] BSDF to use when computing radiance (overrides object BSDF)
								const bool nonmeshlights,		///< [in] Compute lighting from non mesh lights?
								const bool meshlights,			///< [in] Compute lighting from mesh lights (area light sources)?
								const bool cache					///< [in] Should the rasterizer state cache be used?
								)
	{
		if( !ppi ) {
			return false;
		}

		DirectLightingShaderOp* pShaderOp = new DirectLightingShaderOp( bsdf, nonmeshlights, meshlights, cache );

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
								const bool branch,				///< [in] Should we branch when doing scattering?
								const bool reflections,			///< [in] Should reflections be traced?
								const bool refractions,			///< [in] Should refractions be traced?
								const bool diffuse,				///< [in] Should diffuse rays be traced?
								const bool translucents			///< [in] Should translucent rays be traced?
								)
	{
		if( !ppi ) {
			return false;
		}

		DistributionTracingShaderOp* pShaderOp = new DistributionTracingShaderOp( samples, irradiancecaching, forcecheckemitters, branch, reflections, refractions, diffuse, translucents );

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
								const bool cache					///< [in] Should the rasterizer state cache be used?
								)
	{
		if( !ppi ) {
			return false;
		}

		FinalGatherShaderOp* pShaderOp = new FinalGatherShaderOp( numThetaSamples, numPhiSamples, cachegradients, cache );

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "final gather shaderop" );
		return true;
	}

	//! Creates a path tracing shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePathTracingShaderOp(
								IShaderOp** ppi,				///< [out] Pointer to recieve the shaderop
								const bool branch,				///< [in] Should we branch the rays ?
								const bool forcecheckemitters,	///< [in] Force rays allowing to hit emitters even though the material may have a BRDF
								const bool bFinalGather,		///< [in] Should the path tracer co-operate and act as final gather?
								const bool reflections,			///< [in] Should reflections be traced?
								const bool refractions,			///< [in] Should refractions be traced?
								const bool diffuse,				///< [in] Should diffuse rays be traced?
								const bool translucents			///< [in] Should translucent rays be traced?
								)
	{
		if( !ppi ) {
			return false;
		}

		PathTracingShaderOp* pShaderOp = new PathTracingShaderOp( branch, forcecheckemitters, bFinalGather, reflections, refractions, diffuse, translucents );

		(*ppi) = pShaderOp;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "path tracing shaderop" );
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
								ILuminaryManager** ppi,				///< [out] Pointer to recieve the luminary manager
								const bool bChooseOnlyOneLight		///< [in] Choose only one luminaire for each sample
								)
	{
		if( !ppi ) {
			return false;
		}

		LuminaryManager* pLum = new LuminaryManager( bChooseOnlyOneLight );

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
								const Scalar minI,					///< [in] Minimum path importance before giving up
								const IShader& pDefaultShader,		///< [in] The default global shader
								const bool showLuminaires,			///< [in] Should we be able to see luminaries?
								const bool useiorstack,				///< [in] Should the ray caster use a index of refraction stack?
								const bool chooseonlyonelight		///< [in] For luminaire sampling, a random light is chosen for each sample
								)
	{
		if( !ppi ) {
			return false;
		}

		RayCaster* pCaster = new RayCaster( seeRadianceMap, maxR, minI, pDefaultShader, showLuminaires, useiorstack, chooseonlyonelight );

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
#include "Rendering/PixelBasedPelRasterizerAdaptiveSampling.h"
#include "Rendering/PixelBasedPelRasterizerContrastAA.h"

namespace RISE
{
	//! Creates a pixel based rasterizer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePixelBasedPelRasterizer(
								IRasterizer** ppi,					///< [out] Pointer to recieve the rasterizer
								IRayCaster* caster,					///< [in] Ray caster to use for rays
								ISampling2D* pSamples,				///< [in] Sampler for subsamples
								IPixelFilter* pFilter				///< [in] Pixel Filter for samples
								)
	{
		if( !ppi ) {
			return false;
		}

		PixelBasedPelRasterizer* pRasterizer = new PixelBasedPelRasterizer( caster );

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
								const unsigned int num_wavelengths	///< [in] Number of wavelengths to sample
								)
	{
		if( !ppi ) {
			return false;
		}

		PixelBasedSpectralIntegratingRasterizer* pRasterizer = new PixelBasedSpectralIntegratingRasterizer( caster, lambda_begin, lambda_end, num_wavelengths, specSamples );

		if( pSamples && pFilter ) {
			pRasterizer->SubSampleRays( pSamples, pFilter );
		}

		(*ppi) = pRasterizer;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "spectral integrating rasterizer" );
		return true;
	}

	//! Creates an adaptive sampling pixel based rasterizer
	/// \return TRUE if successful, FALSE otherwise
	/// \todo Needs to be rewritten to stop based on variance differences rather than a threshold, since variance threshold is not *that* useful
	bool RISE_API_CreateAdaptiveSamplingPixelBasedPelRasterizer(
								IRasterizer** ppi,					///< [out] Pointer to recieve the rasterizer
								IRayCaster* caster,					///< [in] Ray caster to use for rays
								ISampling2D* pSamples,				///< [in] Sampler for subsamples
								IPixelFilter* pFilter,				///< [in] Pixel Filter for samples
								unsigned int maxS,					///< [in] Maximum number of samples to take
								Scalar var,							///< [in] Variance threshold
								unsigned int numsteps,				///< [in] Number of steps to take from base sampling to max samples
								bool bOutputSamples					///< [in] Should the renderer show how many samples rather than an image
								)
	{
		if( !ppi ) {
			return false;
		}

		PixelBasedPelRasterizerAdaptiveSampling* pRasterizer = new PixelBasedPelRasterizerAdaptiveSampling( maxS, var, numsteps, caster, bOutputSamples );

		if( pSamples && pFilter ) {
			pRasterizer->SubSampleRays( pSamples, pFilter );
		}

		(*ppi) = pRasterizer;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "pixel rasterizer adaptive" );

		return true;
	}

	//! Creates a pixel based pel rasterizer that does adaptive sampling based on
	//! contrast differences in regions
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePixelBasedPelRasterizerContrastAA(
								IRasterizer** ppi,					///< [out] Pointer to recieve the rasterizer
								IRayCaster* caster,					///< [in] Ray caster to use for rays
								ISampling2D* pSamples,				///< [in] Sampler for subsamples
								IPixelFilter* pFilter,				///< [in] Pixel Filter for samples
								const RISEPel& contrast_threshold,	///< [in] Contrast threhold for each color component
								const bool bShowSamples				///< [in] Show the regions in which more samples were taken
								)
	{
		if( !ppi ) {
			return false;
		}

		PixelBasedPelRasterizerContrastAA* pRasterizer = new PixelBasedPelRasterizerContrastAA( caster, contrast_threshold, bShowSamples );

		if( pSamples && pFilter ) {
			pRasterizer->SubSampleRays( pSamples, pFilter );
		}

		(*ppi) = pRasterizer;
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "pixel rasterizer contrast AA" );

		return true;
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
