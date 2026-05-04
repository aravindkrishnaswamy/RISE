//////////////////////////////////////////////////////////////////////
//
//  RISE_API.h - This is the API that all external programs that use
//    RISE library must use.  The set of API functions are
//    basically all C style functions in the RISE_API namespace
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 18, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_API_
#define RISE_API_

//////////////////////////////////////////////////////////
// Interface includes
//////////////////////////////////////////////////////////
#include "Interfaces/IBezierPatchGeometry.h"
#include "Interfaces/IBilinearPatchGeometry.h"
#include "Interfaces/ICamera.h"
#include "Interfaces/IDetectorSphere.h"
#include "Interfaces/IFunction1DManager.h"
#include "Interfaces/IFunction2DManager.h"
#include "Interfaces/IFunction3D.h"
#include "Interfaces/IGeometry.h"
#include "Interfaces/IGeometryManager.h"
#include "Interfaces/ILightPriv.h"
#include "Interfaces/IMaterial.h"
#include "Interfaces/IMaterialManager.h"
#include "Interfaces/IMemoryBuffer.h"
#include "Interfaces/IModifierManager.h"
#include "Interfaces/IObject.h"
#include "Interfaces/IOneColorOperator.h"
#include "Interfaces/IOptions.h"
#include "Interfaces/ICameraManager.h"
#include "Interfaces/IPainter.h"
#include "Interfaces/IPainterManager.h"
#include "Interfaces/IPiecewiseFunction.h"
#include "Interfaces/IPixelFilter.h"
#include "Interfaces/IPhotonTracer.h"
#include "Interfaces/IRasterImage.h"
#include "Interfaces/IRasterImageAccessor.h"
#include "Interfaces/IRasterizer.h"
#include "Interfaces/IRasterizerOutput.h"
#include "Interfaces/IRasterizeSequence.h"
#include "Interfaces/IRayIntersectionModifier.h"
#include "Interfaces/IReadBuffer.h"
#include "Interfaces/ISampling1D.h"
#include "Interfaces/ISampling2D.h"
#include "Interfaces/IScenePriv.h"
#include "Interfaces/IJobPriv.h"
#include "Interfaces/ILogPrinter.h"
#include "Interfaces/ISceneParser.h"
#include "Interfaces/IShader.h"
#include "Interfaces/IShaderManager.h"
#include "Interfaces/IShaderOp.h"
#include "Interfaces/IShaderOpManager.h"
#include "Interfaces/IPhaseFunction.h"
#include "Interfaces/IMedium.h"
#include "Interfaces/ITriangleMeshGeometry.h"
#include "Interfaces/ITriangleMeshLoader.h"
#include "Interfaces/ITwoColorOperator.h"
#include "Interfaces/IWriteBuffer.h"
#include "Rendering/DisplayTransform.h"
#include "RasterImages/EXRCompression.h"
#include "Utilities/SMSConfig.h"

namespace RISE
{

	//////////////////////////////////////////////////////////
	// Library versioning information
	//////////////////////////////////////////////////////////

	//! Queries the version of the library
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_GetVersion(
		int* pMajorVersion,									///< [out] Pointer to recieve the major version
		int* pMinorVersion,									///< [out] Pointer to recieve the minor version
		int* pRevision,										///< [out] Pointer to recieve the revision number
		int* pBuildNumber,									///< [out] Pointer to recieve the build numbers
		bool* pDebug										///< [out] Pointer to bool to recieve whether this is a debug build
		);

	//! Queries the date the library was built
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_GetBuildDate(
		char* szDate,										///< [out] Pointer to string to recieve date
		unsigned int dateStrMax								///< [in] Maximum characters to store in date string
		);

	//! Queries the time the library was built
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_GetBuildTime(
		char* szTime,										///< [out] Pointer to string to recieve time
		unsigned int timeStrMax								///< [in] Maximum characters to store in time string
		);

	//! Queries for any copyright information
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_GetCopyrightInformation(
		char* szCopyright,									///< [out] Pointer to string to recieve copyright info
		unsigned int copyrightStrMax						///< [in] Maximum characters to store in info string
		);

	//! Queries for any special build information
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_GetBuildSpecialInfo(
		char* szInfo,										///< [out] Pointer to string to recieve special info
		unsigned int infoStrMax								///< [in] Maximum characters to store in info string
		);

	//////////////////////////////////////////////////////////
	// Defines camera creation
	//////////////////////////////////////////////////////////

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
		);

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
		);

	//! Creates a camera based on thin lens model
	/// Photographic parameters: sensor size + focal length + f-stop +
	/// focus distance.  FOV is derived as 2*atan(sensor/(2*focal));
	/// aperture diameter is derived as focal/fstop.  All three lengths
	/// (sensor, focal, focus) must be in the same unit as scene
	/// geometry; the lens equation v = f*u/(u-f) requires it, even
	/// though the FOV formula's ratio is unit-free.
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
		const Scalar sceneUnitMeters,							///< [in] Meters per scene unit (1.0 = metres scene, 0.001 = mm scene, 0.0254 = inches)
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
		const Scalar tiltX,										///< [in] Focal-plane tilt around x-axis (radians); 0 = perpendicular focus
		const Scalar tiltY,										///< [in] Focal-plane tilt around y-axis (radians); 0 = perpendicular focus
		const Scalar shiftX,									///< [in] Lens shift along x (mm); 0 = centered
		const Scalar shiftY										///< [in] Lens shift along y (mm); 0 = centered
		);

	//! Creates a fisheye cemera
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
		);

	//! Creates an orthographic cemera
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
		);


	///////////////////////////////////////////////////////////
	// Defines Light creation
	///////////////////////////////////////////////////////////

	//! Creates a infinite point omni light, located at the origin
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePointOmniLight(
		ILightPriv** ppi,										///< [out] Pointer to recieve the light
		const Scalar power,										///< [in] Power of the light in watts
		const RISEPel color,									///< [in] Color of the light in the linear ProPhoto colorspace
		const bool shootPhotons									///< [in] Should this light shoot photons for photon mapping?
		);

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
		);

	//! Creates the ambient light
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAmbientLight(
		ILightPriv** ppi,										///< [out] Pointer to recieve the light
		const Scalar power,										///< [in] Power of the light in watts
		const RISEPel color										///< [in] Color of the light in the linear ProPhoto colorspace
		);

	//! Creates a infinite directional light, coming from a particular direction
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDirectionalLight(
		ILightPriv** ppi,										///< [out] Pointer to recieve the light
		const Scalar power,										///< [in] Power of the light in watts
		const RISEPel color,									///< [in] Color of the light in the linear ProPhoto colorspace
		const Vector3 vDir										///< [in] Direction the light is shining
		);

	///////////////////////////////////////////////////////////
	// Defines Geometry creation
	///////////////////////////////////////////////////////////

	//! Creates a box located at the origin
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBoxGeometry(
							IGeometry** ppi,					///< [out] Pointer to recieve the geometry
							const Scalar width,					///< [in] Width of the box
							const Scalar height,				///< [in] Height of the box
							const Scalar depth					///< [in] Depth of the box
							);

	//! Creates a circular disk at the origin
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCircularDiskGeometry(
									IGeometry** ppi,			///< [out] Pointer to recieve the geometry
									const Scalar radius,		///< [in] Radius of the disk
									const char axis				///< [in] (x|y|z) Which axis the disk sits on
									);

	//! Creates a clipped plane, defined by four points
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateClippedPlaneGeometry(
									IGeometry** ppi,			///< [out] Pointer to recieve the geometry
									const Point3 (&points)[4],	///< [in] Points defining the clipped plane
									const bool doublesided		///< [in] Is it double sided?
									);

	//! Creates a Cylinder at the origin
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCylinderGeometry(
								IGeometry** ppi,				///< [out] Pointer to recieve the geometry
								const char axis,				///< [in] (x|y|z) Which axis the cylinder is sitting on
								const Scalar radius,			///< [in] Radius of the cylinder
								const Scalar height			///< [in] Height of the cylinder
								);

	//! Creates an infinite plane that passes through the origin
	/// \return TRUE if successful, FALSE otherwise
	/// \todo This needs to be seriously re-evaluated
	bool RISE_API_CreateInfinitePlaneGeometry(
											IGeometry** ppi,	///< [out] Pointer to recieve the geometry
											const Scalar xt,	///< [in] How often to tile in X
											const Scalar yt	///< [in] How often to tile in Y
											);

	//! Creates a sphere at the origin
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSphereGeometry(
									IGeometry** ppi,			///< [out] Pointer to recieve the geometry
									const Scalar radius		///< [in] Radius of the sphere
									);

	//! Creates an ellipsoid at the origin
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateEllipsoidGeometry(
									IGeometry** ppi,			///< [out] Pointer to recieve the geometry
									const Vector3& radii		///< [in] Radii of the three axis
									);

	//! Creates a torus at the origin
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTorusGeometry(
									IGeometry** ppi,			///< [out] Pointer to recieve the geometry
									const Scalar majorRad,	///< [in] Major radius
									const Scalar minorRad		///< [in] Minor radius (as a percentage of the major radius)
									);

	//! Creates a triangle mesh geometry object.
	//! BVH is the only acceleration structure (Tier A2 cleanup, 2026-04-27);
	//! the legacy `max_polys`, `max_recur`, `use_bsp` parameters are gone.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTriangleMeshGeometry(
						ITriangleMeshGeometry** ppi,			///< [out] Pointer to recieve the geometry
						const bool double_sided					///< [in] Are the triangles double sided ?
						);

	//! Creates an indexed triangle mesh geometry object.
	//! BVH is the only acceleration structure (Tier A2 cleanup, 2026-04-27);
	//! the legacy `max_polys`, `max_recur`, `use_bsp` parameters are gone.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTriangleMeshGeometryIndexed(
						ITriangleMeshGeometryIndexed** ppi,	///< [out] Pointer to recieve the geometry
						const bool double_sided,				///< [in] Are the triangles double sided ?
						const bool face_normals				///< [in] Use face normals rather than vertex normals
						);

	//! Creates a mesh loader capable of loading a 3DS mesh from read buffer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_Create3DSTriangleMeshLoader(
						ITriangleMeshLoaderIndexed** ppi,		///< [out] Pointer to recieve the mesh loader
						IReadBuffer* pBuffer					///< [in] The buffer to load the 3DS mesh from
						);

	//! Creates a mesh loader capable of loading from a raw file
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateRAWTriangleMeshLoader(
						ITriangleMeshLoader** ppi,				///< [out] Pointer to recieve the mesh loader
						const char* szFileName					///< [in] Name of the file to load from
						);

	//! Creates a triangle mesh geometry from a file of version 2
	//! The format of the file for this version is different from the one
	//! above
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateRAW2TriangleMeshLoader(
						ITriangleMeshLoaderIndexed** ppi,		///< [out] Pointer to recieve the mesh loader
						const char* szFileName					///< [in] Name of the file to load from
						);

	//! Creates a mesh loader capable of loading from a ply file
	/// \return TRUE if successful, FALSE otherwise
	/// \todo this is deprecated and should be removed
	bool RISE_API_CreatePLYTriangleMeshLoader(
						ITriangleMeshLoaderIndexed** ppi,		///< [out] Pointer to recieve the mesh loader
						const char* szFileName,					///< [in] Name of the file to load from
						const bool bInvertFaces					///< [in] Should the faces be inverted?
						);

	// RISE_API_CreateGLTFTriangleMeshLoader was retired 2026-05-01.  glTF
	// single-primitive imports now flow through
	// Job::AddGLTFTriangleMeshGeometry, which constructs a
	// GLTFSceneImporter and calls ImportPrimitive.  The intermediate
	// ITriangleMeshLoaderIndexed wrapper re-parsed the .gltf per
	// primitive (pathological on NewSponza-class assets); the importer
	// now owns the parse for its lifetime.  See
	// src/Library/Importers/GLTFSceneImporter.h "Lifecycle" for the
	// design.

	//! Creates a bezier-patch geometry with analytic ray intersection.
	//! Displacement / bulk tessellation are handled by wrapping this in a
	//! DisplacedGeometry (or `displaced_geometry` in the scene file).
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBezierPatchGeometry(
						IBezierPatchGeometry** ppi,				///< [out] Pointer to recieve the geometry
						const unsigned int max_patches,			///< [in] Maximum number of patches per accelerator leaf
						const unsigned char max_recur,			///< [in] Maximum accelerator recursion depth
						const bool use_bsp						///< [in] Use BSP tree (true) or Octree (false)
						);

	//! Creates a geometry object made up of a series of bilinear patches
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBilinearPatchGeometry(
						IBilinearPatchGeometry** ppi,			///< [out] Pointer to recieve the geometry
						const unsigned int max_patches,			///< [in] Maximum number of patches / octant node
						const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
						const bool use_bsp						///< [in] Use a BSP tree rather than an Octree
						);

	//! Creates a displaced geometry that wraps any existing IGeometry, tessellates it, and
	//! applies a displacement map along the vertex normals.  The base geometry must support
	//! IGeometry::TessellateToMesh (InfinitePlaneGeometry does not — construction fails loudly).
	//! BVH is the only acceleration structure (Tier A2 cleanup, 2026-04-27);
	//! the legacy `max_polys`, `max_recur`, `use_bsp` parameters are gone.
	/// \return TRUE if successful, FALSE if the base cannot be tessellated or ppi is null
	bool RISE_API_CreateDisplacedGeometry(
						IGeometry**         ppi,				///< [out] Pointer to receive the geometry
						IGeometry*          pBase,				///< [in] Base geometry to wrap (AddRef'd internally)
						const unsigned int  detail,				///< [in] Tessellation detail; warning logged if > 256
						IFunction2D*        displacement,		///< [in] Displacement function (may be null for pure tessellation)
						const Scalar        disp_scale,			///< [in] Displacement scale factor
						const bool          double_sided,		///< [in] Are generated polygons double-sided?
						const bool          face_normals		///< [in] Use face normals rather than topologically re-averaged vertex normals
						);


	///////////////////////////////////////////////////////////
	// UV generators
	//////////////////////////////////////////////////////////

	//! Creates a box UV generator
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBoxUVGenerator(
								IUVGenerator** ppi,				///< [out] Pointer to recieve the UV generator
								const Scalar width,				///< [in] Width
								const Scalar height,			///< [in] Height
								const Scalar depth				///< [in] Depth
								);

	//! Creates a cylindrical UV generator
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCylindricalUVGenerator(
								IUVGenerator** ppi,				///< [out] Pointer to recieve the UV generator
								const Scalar radius,			///< [in] Radius
								const char axis,				///< [in] (x|y|z) Primary axis
								const Scalar height				///< [in] Size
								);

	//! Creates a spherical UV generator
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSphericalUVGenerator(
								IUVGenerator** ppi,				///< [out] Pointer to recieve the UV generator
								const Scalar radius				///< [in] Radius
								);


	//////////////////////////////////////////////////////////
	// Materials
	//////////////////////////////////////////////////////////

	//! Creates the NULL material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateNullMaterial(
								IMaterial** ppi				///< [out] Pointer to recieve the material
								);

	//! Creates Lambertian material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateLambertianMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& ref				///< [in] Reflectance
								);

	//! Creates a Polished material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePolishedMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& ref,			///< [in] Reflectance of diffuse substrate
								const IPainter& tau,			///< [in] Transmittance of the dielectric top
								const IPainter& Nt,				///< [in] Index of refraction of dielectric coating
								const IPainter& scat,			///< [in] Scattering function (either Phong or HG)
								const bool hg					///< [in] Use Henyey-Greenstein phase function scattering
								);

	//! Creates a Dielectric material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDielectricMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& tau,			///< [in] Transmittance
								const IPainter& rIndex,			///< [in] Index of refraction
								const IPainter& scat,			///< [in] Scattering function (either Phong or HG)
								const bool hg					///< [in] Use Henyey-Greenstein phase function scattering
								);

	//! Creates a SubSurface Scattering material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSubSurfaceScatteringMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& ior,			///< [in] Index of refraction
								const IPainter& absorption,		///< [in] Absorption coefficient
								const IPainter& scattering,		///< [in] Scattering coefficient
								const Scalar g,					///< [in] HG asymmetry parameter
								const Scalar roughness			///< [in] Surface roughness [0,1]
								);

	//! Creates a Random Walk SSS material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateRandomWalkSSSMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& ior,			///< [in] Index of refraction
								const IPainter& absorption,		///< [in] Absorption coefficient
								const IPainter& scattering,		///< [in] Scattering coefficient
								const Scalar g,					///< [in] HG asymmetry parameter
								const Scalar roughness,			///< [in] Surface roughness [0,1]
								const unsigned int maxBounces	///< [in] Maximum walk steps
								);

	//! Creates an isotropic phong material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateIsotropicPhongMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& rd,				///< [in] Diffuse reflectance
								const IPainter& rs,				///< [in] Specular reflectance
								const IPainter& exponent		///< [in] Phong exponent
								);

	//! Creates the anisotropic phong material of Ashikmin and Shirley
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAshikminShirleyAnisotropicPhongMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& rd,				///< [in] Diffuse reflectance
								const IPainter& rs,				///< [in] Specular reflectance
								const IPainter& Nu,				///< [in] Phong exponent in U
								const IPainter& Nv				///< [in] Phong exponent in V
								);

	//! Creates a perfect reflector
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePerfectReflectorMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& ref				///< [in] Reflectance
								);

	//! Creates a perfect refractor
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePerfectRefractorMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& ref,			///< [in] Amount of refraction
								const IPainter& ior				///< [in] Index of refraction
								);

	//! Creates a translucent material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTranslucentMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& rF,				///< [in] Reflectance
								const IPainter& T,				///< [in] Transmittance
								const IPainter& ext,			///< [in] Extinction
								const IPainter& N,				///< [in] Phong exponent
								const IPainter& scat			///< [in] Multiple scattering component
								);

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
								);

	//! Creates a Donner & Jensen 2008 spectral skin BSSRDF material (BDPT-compatible)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDonnerJensenSkinBSSRDFMaterial(
								IMaterial** ppi,											///< [out] Pointer to recieve the material
								const IPainter& melanin_fraction_,							///< C_m: melanin volume fraction (0-0.5)
								const IPainter& melanin_blend_,								///< beta_m: eumelanin vs pheomelanin blend (0-1)
								const IPainter& hemoglobin_epidermis_,						///< C_he: hemoglobin fraction in epidermis (0-0.05)
								const IPainter& carotene_fraction_,							///< C_bc: carotene fraction (0-0.05)
								const IPainter& hemoglobin_dermis_,							///< C_hd: hemoglobin fraction in dermis (0-0.1)
								const IPainter& epidermis_thickness_,						///< Epidermis thickness in cm (default 0.025)
								const IPainter& ior_epidermis_,								///< Index of refraction of epidermis (default 1.4)
								const IPainter& ior_dermis_,								///< Index of refraction of dermis (default 1.38)
								const IPainter& blood_oxygenation_,							///< Blood oxygenation ratio (default 0.7)
								const Scalar roughness										///< Surface roughness for microfacet boundary [0, 1]
								);

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
								);

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
								);

	//! Creates Ward's isotropic gaussian material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateWardIsotropicGaussianMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& diffuse,		///< [in] Diffuse reflectance
								const IPainter& specular,		///< [in] Specular reflectance
								const IPainter& alpha			///< [in] Standard deviation (RMS) of surface slope
								);

	//! Creates Ward's anisotropic elliptical gaussian material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateWardAnisotropicEllipticalGaussianMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& diffuse,		///< [in] Diffuse reflectance
								const IPainter& specular,		///< [in] Specular reflectance
								const IPainter& alphax,			///< [in] Standard deviation (RMS) of surface slope in x
								const IPainter& alphay			///< [in] Standard deviation (RMS) of surface slope in y
								);

	//! Creates a GGX anisotropic microfacet material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGGXMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& diffuse,		///< [in] Diffuse reflectance
								const IPainter& specular,		///< [in] Specular reflectance / F0
								const IPainter& alphaX,			///< [in] Roughness in tangent u direction
								const IPainter& alphaY,			///< [in] Roughness in tangent v direction
								const IPainter& ior,			///< [in] Index of refraction (ignored in Schlick mode)
								const IPainter& ext,			///< [in] Extinction coefficient (ignored in Schlick mode)
								const FresnelMode fresnel_mode = eFresnelConductor	///< [in] Fresnel evaluation model
								);

	//! Creates a GGX material with an optional emissive painter.  Pass
	//! emissive=NULL to skip the emitter (equivalent to RISE_API_CreateGGXMaterial).
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGGXEmissiveMaterial(
								IMaterial** ppi,
								const IPainter& diffuse,
								const IPainter& specular,
								const IPainter& alphaX,
								const IPainter& alphaY,
								const IPainter& ior,
								const IPainter& ext,
								const IPainter* emissive,		///< [in] Optional; NULL = no emitter
								const Scalar    emissive_scale,
								const FresnelMode fresnel_mode = eFresnelConductor	///< [in] Fresnel evaluation model
								);

	//! Creates a Charlie / Neubelt sheen material for fabric / cloth.
	//! Designed as the top layer in a CompositeMaterial(top=sheen,
	//! bottom=baseGGX) pairing for glTF KHR_materials_sheen assets.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSheenMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& sheenColor,		///< [in] Sheen tint colour (typical 0..1)
								const IPainter& sheenRoughness	///< [in] Sheen roughness (typical 0..1; clamped to >= 1e-3 internally)
								);

	//! Creates a Cook Torrance material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCookTorranceMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& diffuse,		///< [in] Diffuse reflectance
								const IPainter& specular,		///< [in] Specular reflectance
								const IPainter& facet,			///< [in] Facet distribution
								const IPainter& ior,			///< [in] IOR delta
								const IPainter& ext				///< [in] Extinction factor
								);

	//! Creates a Oren-Nayar material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateOrenNayarMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& reflectance,	///< [in] Reflectance
								const IPainter& roughness		///< [in] Roughness
								);

	//! Creates a Schlick material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSchlickMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& diffuse,		///< [in] Diffuse reflectance
								const IPainter& specular,		///< [in] Specular reflectance
								const IPainter& roughness,		///< [in] Roughness
								const IPainter& isotropy		///< [in] Isotropy
								);

	//! Creates a datadriven material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDataDrivenMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const char* filename			///< [in] Filename to load data from
								);


	//! Creates a lambertian luminaire material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateLambertianLuminaireMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& radEx,			///< [in] Radiant exitance
								const IMaterial& mat,			///< [in] Material to use for all non emmission properties
								const Scalar scale				///< [in] Value to scale radiant exitance by
								);

	//! Creates a phong luminaire material
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePhongLuminaireMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& radEx,			///< [in] Radiance exitance
								const IMaterial& mat,			///< [in] Material to use for all non emmission properties
								const IPainter& N,				///< [in] Phong exponent
								const Scalar scale				///< [in] Value to scale radiant exitance by
								);



	//////////////////////////////////////////////////////////
	// Painters
	//////////////////////////////////////////////////////////

	//! Creates a checker painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCheckerPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const Scalar size,				///< [in] Size of the checkers in texture mapping units
								const IPainter& a,				///< [in] First color
								const IPainter& b				///< [in] Second color
								);

	//! Creates a lines painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateLinesPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const Scalar size,				///< [in] Size of the lines in texture mapping units
								const IPainter& a,				///< [in] First color
								const IPainter& b,				///< [in] Second color
								const bool bvert				///< [in] Are the lines vertical?
								);

	//! Creates a mandelbrot fractal painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMandelbrotFractalPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const IPainter& cA,				///< [in] First color
								const IPainter& cB,				///< [in] Second color
								const Scalar lower_x,
								const Scalar upper_x,
								const Scalar lower_y,
								const Scalar upper_y,
								const Scalar exp
								);

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
								);

	//! Creates a sum-of-sines water-wave painter (Gerstner height variant).
	/// Evaluate(u,v) returns the summed height of `numWaves` sine waves derived
	/// from wind parameters + a deterministic seed.  Intended for
	/// DisplacedGeometry displacement; color is interpolated between cA and cB
	/// by the normalized height.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGerstnerWavePainter(
								IPainter** ppi,						///< [out] Pointer to receive the painter
								const IPainter& cA,					///< [in] Trough color painter
								const IPainter& cB,					///< [in] Crest color painter
								const unsigned int numWaves,		///< [in] Number of sine waves to sum
								const Scalar medianWavelength,		///< [in] Central wavelength (UV units)
								const Scalar wavelengthRange,		///< [in] Multiplicative range; span is [median/range, median*range]
								const Scalar medianAmplitude,		///< [in] Amplitude at median wavelength
								const Scalar amplitudePower,		///< [in] A_i = medianAmplitude * (lambda_i/medianWavelength)^power
								const Scalar windDirX,				///< [in] Wind direction X (normalized internally)
								const Scalar windDirY,				///< [in] Wind direction Y (normalized internally)
								const Scalar directionalSpread,		///< [in] Per-wave angle jitter around wind direction (radians)
								const Scalar dispersionSpeed,		///< [in] Multiplies sqrt(g*k) for the dispersion relation; tune motion speed
								const unsigned int seed,			///< [in] RNG seed; identical seeds produce identical spectra
								const Scalar time					///< [in] Simulation time (keyframeable at the scene level)
								);

	//! Creates a 2D perlin noise painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePerlin3DPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const Scalar dPersistence,		///< [in] Persistence
								const unsigned int nOctaves,	///< [in] Number of octaves to use in noise generation
								const IPainter& cA, 			///< [in] First painter
								const IPainter& cB, 			///< [in] Second painter
								const Vector3& vScale,			///< [in] How much to scale the function by
								const Vector3& vShift			///< [in] How much to shift the function by
								);


	bool RISE_API_CreateWavelet3DPainter(
								IPainter** ppi,
								const unsigned int nTileSize,
								const Scalar dPersistence,
								const unsigned int nOctaves,
								const IPainter& cA,
								const IPainter& cB,
								const Vector3& vScale,
								const Vector3& vShift
								);

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
								);

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
								);

	//! Creates a 3D simplex noise painter
	bool RISE_API_CreateSimplex3DPainter(
								IPainter** ppi,
								const Scalar dPersistence,
								const unsigned int nOctaves,
								const IPainter& cA,
								const IPainter& cB,
								const Vector3& vScale,
								const Vector3& vShift
								);

	//! Creates a 3D SDF primitives painter
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
								);

	//! Creates a 3D curl noise painter
	bool RISE_API_CreateCurlNoise3DPainter(
								IPainter** ppi,
								const Scalar dPersistence,
								const unsigned int nOctaves,
								const Scalar dEpsilon,
								const IPainter& cA,
								const IPainter& cB,
								const Vector3& vScale,
								const Vector3& vShift
								);

	//! Creates a 3D domain-warped noise painter
	/// \return TRUE if successful, FALSE otherwise
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
								);

	//! Creates a 3D Perlin-Worley hybrid (cloud noise) painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePerlinWorley3DPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const Scalar dPersistence,		///< [in] Persistence for Perlin FBM
								const unsigned int nOctaves,	///< [in] Number of octaves for Perlin
								const Scalar dWorleyJitter,		///< [in] Worley jitter [0,1]
								const Scalar dBlend,			///< [in] Blend factor [0,1]: 0=Perlin, 1=Worley
								const IPainter& cA,				///< [in] First painter
								const IPainter& cB,				///< [in] Second painter
								const Vector3& vScale,			///< [in] How much to scale the function by
								const Vector3& vShift			///< [in] How much to shift the function by
								);

	//! Creates a 3D Worley (cellular) noise painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateWorley3DPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const Scalar dJitter,			///< [in] Jitter amount [0,1]
								const unsigned int nMetric,		///< [in] Distance metric (0=Euclidean, 1=Manhattan, 2=Chebyshev)
								const unsigned int nOutput,		///< [in] Output mode (0=F1, 1=F2, 2=F2-F1)
								const IPainter& cA, 			///< [in] First painter
								const IPainter& cB, 			///< [in] Second painter
								const Vector3& vScale,			///< [in] How much to scale the function by
								const Vector3& vShift			///< [in] How much to shift the function by
								);

	//! Creates a 3D turbulence noise painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTurbulence3DPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const Scalar dPersistence,		///< [in] Persistence
								const unsigned int nOctaves,	///< [in] Number of octaves to use in noise generation
								const IPainter& cA, 			///< [in] First painter
								const IPainter& cB, 			///< [in] Second painter
								const Vector3& vScale,			///< [in] How much to scale the function by
								const Vector3& vShift			///< [in] How much to shift the function by
								);

	//! Creates a spectral color painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSpectralColorPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const SpectralPacket& spectrum,	///< [in] Spectral packet
								const Scalar scale				///< [in] How much to scale the amplitudes by
								);

	//! Creates a texture painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTexturePainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								IRasterImageAccessor* pSA		///< [in] Raster Image accessor to the image containing the texture
								);

	//! Creates a painter that paints a uniform color
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateUniformColorPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const RISEPel& c				///< [in] Color to paint
								);

	//! Creates a painter that returns the per-vertex color the geometry
	//! interpolates at the hit point (`ri.vColor`).  Falls back to the
	//! supplied default for hits on geometry without vertex colors.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateVertexColorPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const RISEPel& fallback			///< [in] Color to use when no per-vertex color is present
								);

	//! Creates a painter that paints a voronoi diagram
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateVoronoi2DPainter(
								IPainter**	ppi,				///< [out] Pointer to recieve the painter
								const std::vector<Point2> pts,	///< [in] The locations of the generators
								const std::vector<IPainter*> p,	///< [in] The painters for the generators
								const IPainter& border,			///< [in] Painter for the border
								const Scalar bsize				///< [in] Size of the borders
								);

	//! Creates a painter that paints a voronoi diagram in 3D
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateVoronoi3DPainter(
								IPainter**	ppi,				///< [out] Pointer to recieve the painter
								const std::vector<Point3> pts,	///< [in] The locations of the generators
								const std::vector<IPainter*> p,	///< [in] The painters for the generators
								const IPainter& border,			///< [in] Painter for the border
								const Scalar bsize				///< [in] Size of the borders
								);

	//! Creates a iridescent painter (a painter whose color changes as viewing angle changes)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateIridescentPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const IPainter& a,				///< [in] First color
								const IPainter& b,				///< [in] Second color
								const double bias				///< [in] Biases the iridescence to one color or another
								);

	//! Creates a painter that paints a spectrum from a Function1D
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateFunction1DSpectralPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const IFunction1D& func			///< [in] Function to paint
								);

	//! Creates a blackbody luminaire painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBlackBodyPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const Scalar temperature,		///< [in] Temperature of the radiator in Kelvins
								const Scalar lambda_begin,		///< [in] Where in the spectrum to start creating the spectral packet
								const Scalar lambda_end,		///< [in] Where in the spectrum to end creating the spectral packet
								const unsigned int num_freq,	///< [in] Number of frequencies to use in the spectral packet
								const bool normalize,			///< [in] Should the values be normalized to peak intensity?
								const Scalar scale				///< [in] Value to scale radiant exitance by
								);

	//! Creates a blend painter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBlendPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const IPainter& a,				///< [in] First color
								const IPainter& b,				///< [in] Second color
								const IPainter& mask			///< [in] Blend mask
								);

	//! Creates a channel-extraction painter (glTF MR-texture helper).
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateChannelPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const IPainter& source,			///< [in] Source painter
								const char channel,				///< [in] 0=R, 1=G, 2=B
								const Scalar scale,				///< [in] Scale on extracted channel
								const Scalar bias				///< [in] Bias added after scale
								);


	//////////////////////////////////////////////////////////
	// Radiance maps
	//////////////////////////////////////////////////////////

	//! Creates a radiance map which is used for image based lighting
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateRadianceMap(
								IRadianceMap** ppi,				///< [out] Pointer to recieve the radiance map
								const IPainter& painter,		///< [in] Painter to use for the map
								const Scalar scale				///< [in] How much to scale the values in the map by
								);

	//////////////////////////////////////////////////////////
	// Pixel filters
	//////////////////////////////////////////////////////////

	//! Creates a box pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBoxPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								);

	//! Creates a triangle pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTrianglePixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								);

	//! Creates a gaussian pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGaussianPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar size,				///< [in] Size of the filter
								const Scalar sigma				///< [in] Distribution of the filter
								);

	//! Creates a sinc pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSincPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar window,			///< [in] Window of the filter
								const Scalar scale				///< [in] Scale factor
								);

	//! Creates a box windowed sinc pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBoxWindowedSincPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								);

	//! Creates a bartlett windowed sinc pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBartlettWindowedSincPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								);

	//! Creates a welch windowed sinc pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateWelchWindowedSincPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								);

	//! Creates a lanczos windowed sinc pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateLanczosWindowedSincPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								);

	//! Creates a kaiser windowed sinc pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateKaiserWindowedSincPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height,			///< [in] Height of the filter
								const Scalar alpha
								);

	//! Creates a hanning windowed sinc pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateHanningWindowedSincPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								);

	//! Creates a hamming windowed sinc pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateHammingWindowedSincPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								);

	//! Creates a blackman windowed sinc pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBlackmanWindowedSincPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								);

	//! Creates a Cook pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCookPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height				///< [in] Height of the filter
								);

	//! Creates a Max pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMaxPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar width,				///< [in] Width of the filter
								const Scalar height,			///< [in] Height of the filter
								const Scalar s_x,				///< [in] S paramter in x direction
								const Scalar s_y				///< [in] S paramter in y direction
								);

	//! Creates a Mitchell-Netvravali pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMitchellNetravaliPixelFilter(
								IPixelFilter** ppi,				///< [out] Pointer to recieve the pixel filter
								const Scalar b,					///< [in] b parameter
								const Scalar c					///< [in] c parameter
								);

	//! Creates a Lanczos pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateLanczosPixelFilter(
								IPixelFilter** ppi				///< [out] Pointer to recieve the pixel filter
								);

	//! Creates a Catmull Rom pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCatmullRomPixelFilter(
								IPixelFilter** ppi				///< [out] Pointer to recieve the pixel filter
								);

	//! Creates a Cubic B-Spline pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCubicBSplinePixelFilter(
								IPixelFilter** ppi				///< [out] Pointer to recieve the pixel filter
								);

	//! Creates a Quadratic B-Spline pixel filter
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateQuadraticBSplinePixelFilter(
								IPixelFilter** ppi				///< [out] Pointer to recieve the pixel filter
								);

	//////////////////////////////////////////////////////////
	// Sampling 1D
	//////////////////////////////////////////////////////////

	//! Creates a jittered 1D sampler
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateJitteredSampling1D(
								ISampling1D** ppi,				///< [out] Pointer to recieve the sampling1D object
								const Scalar size				///< [in] Size of the kernel
								);

	//! Creates a 1D uniform sampler
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateUniformSampling1D(
								ISampling1D** ppi,				///< [out] Pointer to recieve the sampling1D object
								const Scalar size				///< [in] Size of the kernel
								);


	//////////////////////////////////////////////////////////
	// Sampling 2D
	//////////////////////////////////////////////////////////

	//! Creates an NRooks sampling kernel
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateNRooksSampling2D(
								ISampling2D** ppi,				///< [out] Pointer to recieve the sampling2D object
								const Scalar width,				///< [in] Width of the kernel
								const Scalar height,			///< [in] Height of the kernel
								const Scalar howfar				///< [in] How far from the center should each sample be placed?
								);

	//! Creates a poisson disk sampling kernel
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePoissonDiskSampling2D(
								ISampling2D** ppi,				///< [out] Pointer to recieve the sampling2D object
								const Scalar width,				///< [in] Width of the kernel
								const Scalar height,			///< [in] Height of the kernel
								const Scalar sep				///< [in] Minimum distance between any two samples
								);

	//! Creates a random sampling kernel
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateRandomSampling2D(
								ISampling2D** ppi,				///< [out] Pointer to recieve the sampling2D object
								const Scalar width,				///< [in] Width of the kernel
								const Scalar height				///< [in] Height of the kernel
								);

	//! Creates a stratified sampling kernel
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateStratifiedSampling2D(
								ISampling2D** ppi,				///< [out] Pointer to recieve the sampling2D object
								const Scalar width,				///< [in] Width of the kernel
								const Scalar height,			///< [in] Height of the kernel
								const Scalar howfar				///< [in] How far from the center should each sample be placed?
								);

	//! Creates a uniform sampling kernel
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateUniformSampling2D(
								ISampling2D** ppi,				///< [out] Pointer to recieve the sampling2D object
								const Scalar width,				///< [in] Width of the kernel
								const Scalar height				///< [in] Height of the kernel
								);

	//! Creates a Multi Jittered sampling kernel
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMultiJitteredSampling2D(
								ISampling2D** ppi,				///< [out] Pointer to recieve the sampling2D object
								const Scalar width,				///< [in] Width of the kernel
								const Scalar height			///< [in] Height of the kernel
								);

	//! Creates an a Halton point set kernel
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateHaltonPointsSampling2D(
								ISampling2D** ppi,				///< [out] Pointer to recieve the sampling2D object
								const Scalar width,				///< [in] Width of the kernel
								const Scalar height				///< [in] Height of the kernel
								);

	//! Creates an Owen-scrambled Sobol (0,2)-net sampling kernel
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSobolSampling2D(
								ISampling2D** ppi,				///< [out] Pointer to recieve the sampling2D object
								const Scalar width,				///< [in] Width of the kernel
								const Scalar height				///< [in] Height of the kernel
								);


	//////////////////////////////////////////////////////////
	// Managers
	//////////////////////////////////////////////////////////


	//! Creates a 1D function manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateFunction1DManager(
								IFunction1DManager** ppi		///< [out] Pointer to recieve the manager
								);

	//! Creates a 2D function manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateFunction2DManager(
								IFunction2DManager** ppi		///< [out] Pointer to recieve the manager
								);

	//! Creates a light manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateLightManager(
								ILightManager** ppi				///< [out] Pointer to recieve the manager
								);

	//! Creates a geometry manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGeometryManager(
								IGeometryManager** ppi				///< [out] Pointer to recieve the manager
								);

	//! Creates a painter manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePainterManager(
								IPainterManager** ppi			///< [out] Pointer to recieve the manager
								);

	//! Creates a camera manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCameraManager(
								ICameraManager** ppi			///< [out] Pointer to recieve the manager
								);

	//! Creates a material manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMaterialManager(
								IMaterialManager** ppi			///< [out] Pointer to recieve the manager
								);

	//! Creates a shader manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateShaderManager(
								IShaderManager** ppi			///< [out] Pointer to recieve the manager
								);

	//! Creates a shader op manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateShaderOpManager(
								IShaderOpManager** ppi			///< [out] Pointer to recieve the manager
								);

	//! Creates a object manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateObjectManager(
								IObjectManager** ppi,					///< [out] Pointer to recieve the manager
								const bool bUseBSPtree,					///< [in] Use BSP trees for spatial partitioning
								const bool bUseOctree,					///< [in] Use Octrees for spatial partitioning
								const unsigned int nMaxObjectsPerNode,	///< [in] Maximum number of elements / node
								const unsigned int nMaxTreeDepth		///< [in] Maximum tree depth
								);

	//! Creates a modifier manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateModifierManager(
								IModifierManager** ppi			///< [out] Pointer to recieve the manager
								);

	//////////////////////////////////////////////////////////
	// Modifiers
	//////////////////////////////////////////////////////////

	//! Creates a bump map
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBumpMapModifier(
								IRayIntersectionModifier** ppi,	///< [out] Pointer to recieve the modifier
								const IFunction2D& func,		///< [in] The function to use for the bumps
								const Scalar scale,				///< [in] Factor to scale values by
								const Scalar window				///< [in] Size of the window
								);

	//! Creates a tangent-space normal-map modifier.  Painter must be
	//! linear-RGB (no sRGB decode).  See Modifiers/NormalMap.h.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateNormalMapModifier(
								IRayIntersectionModifier** ppi,	///< [out] Pointer to recieve the modifier
								const IPainter& painter,		///< [in] Linear-RGB normal-map painter
								const Scalar scale				///< [in] glTF normalTexture.scale
								);


	//////////////////////////////////////////////////////////
	// Functions
	//////////////////////////////////////////////////////////

	//! Creates a constant 1D function
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateConstant1DFunction(
								IFunction1D** ppi,				///< [out] Pointer to recieve the function
								const Scalar value				///< [in] Value the function always returns
								);

	//! Creates a constant 2D function
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateConstant2DFunction(
								IFunction2D** ppi,				///< [out] Pointer to recieve the function
								const Scalar value				///< [in] Value the function always returns
								);

	//! Creates a constant 3D function
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateConstant3DFunction(
								IFunction3D** ppi,				///< [out] Pointer to recieve the function
								const Scalar value				///< [in] Value the function always returns
								);

	//! Creates a piecewise linear function 1D
	bool RISE_API_CreatePiecewiseLinearFunction1D(
								IPiecewiseFunction1D** ppi		///< [out] Pointer to recieve the function
								);

	//! Creates a piecewise linear function 2D
	bool RISE_API_CreatePiecewiseLinearFunction2D(
								IPiecewiseFunction2D** ppi		///< [out] Pointer to recieve the function
								);


	//////////////////////////////////////////////////////////
	// Utilities
	//////////////////////////////////////////////////////////

	//! Creates a memory buffer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMemoryBuffer(
								IMemoryBuffer** ppi				///< [out] Pointer to recieve the memory buffer
								);

	//! Creates a memory buffer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMemoryBufferSize(
								IMemoryBuffer** ppi,			///< [out] Pointer to recieve the memory buffer
								const unsigned int size			///< [in] Size of the memory buffer
								);

	//! Wraps a memory buffer around some given memory
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCompatibleMemoryBuffer(
								IMemoryBuffer** ppi,			///< [out] Pointer to recieve the memory buffer
								char* pMemory,					///< [in] Pointer to some memory
								const unsigned int size,		///< [in] Amount of memory
								bool bTakeOwnership				///< [in] Should the buffer now take ownership
								);

	//! Creates a memory buffer by loading a file
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMemoryBufferFromFile(
								IMemoryBuffer** ppi,			///< [out] Pointer to recieve the memory buffer
								const char* filename			///< [in] Name of the file to load
								);


	//! Creates a read buffer from a file directly
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDiskFileReadBuffer(
								IReadBuffer** ppi,				///< [out] Pointer to recieve the buffer
								const char* filename			///< [in] Name of the file to read
								);

	//! Creates a write buffer to a file directly
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDiskFileWriteBuffer(
								IWriteBuffer** ppi,				///< [out] Pointer to recieve the buffer
								const char* filename			///< [in] Name of the file to write
								);

	//! Creates a probability density function from a normal 1D function
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePDF_Function1D(
								IProbabilityDensityFunction** ppi,	///< [out] Pointer to recieve the PDF
								const IFunction1D* func,			///< [in] A 1-D function to build a PDF of
								const unsigned int numsteps			///< [in] Interval to sample the function
								);

	//! Creates a probability density function from a spectral packet
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePDF_SpectralPacket(
								IProbabilityDensityFunction** ppi,	///< [out] Pointer to recieve the PDF
								const SpectralPacket& p				///< [in] The spectral packet
								);

	//////////////////////////////////////////////////////////
	// Objects
	//////////////////////////////////////////////////////////

	//! Creates an object
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateObject(
							IObjectPriv** ppi,					///< [out] Pointer to recieve object
							const IGeometry* geom				///< [in] Geometry making up this objectzz
							);

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
								);

	//////////////////////////////////////////////////////////
	// Scene
	//////////////////////////////////////////////////////////

	//! Creates a scene
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateScene(
							IScenePriv** ppi						///< [out] Pointer to recieve the scene
							);


	//////////////////////////////////////////////////////////
	// Photon mapping
	//////////////////////////////////////////////////////////

	//! Creates a caustic pel photon map
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCausticPelPhotonMap(
								IPhotonMap** ppi,					///< [out] Pointer to recieve the photon map
								const unsigned int max				///< [in] Maximum number of photons to store
								);

	//! Creates a global pel photon map
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGlobalPelPhotonMap(
								IPhotonMap** ppi,					///< [out] Pointer to recieve the photon map
								const unsigned int max				///< [in] Maximum number of photons to store
								);

	//! Creates a translucent pel photon map
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTranslucentPelPhotonMap(
								IPhotonMap** ppi,					///< [out] Pointer to recieve the photon map
								const unsigned int max				///< [in] Maximum number of photons to store
								);

	//! Creates a caustic spectral photon map
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCausticSpectralPhotonMap(
								ISpectralPhotonMap** ppi,			///< [out] Pointer to recieve the photon map
								const unsigned int max				///< [in] Maximum number of photons to store
								);

	//! Creates a global spectral photon map
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGlobalSpectralPhotonMap(
								ISpectralPhotonMap** ppi,			///< [out] Pointer to recieve the photon map
								const unsigned int max				///< [in] Maximum number of photons to store
								);

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
								const bool shootFromMeshLights = true///< [in] Should we shoot from mesh based lights (luminaries)?
								);

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
								const bool shootFromMeshLights = true///< [in] Should we shoot from mesh based lights (luminaries)?
								);

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
								const bool shootFromMeshLights = true///< [in] Should we shoot from mesh based lights (luminaries)?
								);

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
								);

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
								);

	//! Creates a shadow photon map
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateShadowPhotonMap(
								IShadowPhotonMap** ppi,				///< [out] Pointer to recieve the photon map
								const unsigned int max				///< [in] Maximum number of photons to store
								);

	//! Creates a shadow photon tracer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateShadowPhotonTracer(
								IPhotonTracer** ppi,				///< [out] Pointer to recieve the translucent photon tracer
								const unsigned int temporal_samples,///< [in] Number of temporal samples to take for animation frames
								const bool regenerate				///< [in] Should the tracer regenerate a new photon each time the scene time changes?
								);

	//! Creates an irradiance cache
	/// \return TRUE if successful, FALSE otherwise
bool RISE_API_CreateIrradianceCache(
								IIrradianceCache** ppi,				///< [out] Pointer to recieve the caustic photon tracer
								const unsigned int size,			///< [in] Size of the cache
								const Scalar tolerance,				///< [in] Tolerance of the cache
								const Scalar min_spacing,			///< [in] Minimum seperation
								const Scalar max_spacing, 			///< [in] Maximum seperation
								const Scalar query_threshold_scale,	///< [in] Scale for the query acceptance threshold
								const Scalar neighbor_spacing_scale	///< [in] Scale for capping reuse radius by local neighbor spacing
								);



	//////////////////////////////////////////////////////////
	// Rasterizer outputs
	//////////////////////////////////////////////////////////

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
								);

	//////////////////////////////////////////////////////////
	// Rasterize Sequeces
	//////////////////////////////////////////////////////////

	//! Creates a Morton (Z-order) curve rasterize sequence for optimal cache locality
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMortonRasterizeSequence(
								IRasterizeSequence** ppi,			///< [out] Pointer to recieve the rasterize sequence
								const unsigned int tileSize			///< [in] Width and height of each square tile
								);

	//! Creates a block rasterize sequence (deprecated: prefer Morton sequence)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBlockRasterizeSequence(
								IRasterizeSequence** ppi,			///< [out] Pointer to recieve the rasterize sequence
								const unsigned int width,			///< [in] Width of the block
								const unsigned int height,			///< [in] Height of the block
								const char order					///< [in] Block ordering type
								);

	//! Creates a scanline rasterize sequence (deprecated: prefer Morton sequence)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateScanlineRasterizeSequence(
								IRasterizeSequence** ppi			///< [out] Pointer to recieve the rasterize sequence
								);

	//! Creates a hilbert space filling curve rasterize sequence (deprecated: prefer Morton sequence)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateHilbertRasterizeSequence(
								IRasterizeSequence** ppi,			///< [out] Pointer to recieve the rasterize sequence
								const unsigned int depth			///< [in] Depth of curve recursion
								);


	//////////////////////////////////////////////////////////
	// Raster Images
	//////////////////////////////////////////////////////////

	//! Creates a raster image of RISEColor type
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateRISEColorRasterImage(
								IRasterImage** ppi,					///< [out] Pointer to recieve the raster image
								const unsigned int width,			///< [in] Width
								const unsigned int height,			///< [in] Height
								const RISEColor c					///< [in] Value to initialize the raster image to
								);

	//! Creates a read-only raster image of RISEColor type
	bool RISE_API_CreateReadOnlyRISEColorRasterImage(
								IRasterImage** ppi					///< [out] Pointer to recieve the raster image
								);

	//! Creates a write-only raster image of RISEColor type
	bool RISE_API_CreateWriteOnlyRISEColorRasterImage(
								IRasterImage** ppi,					///< [out] Pointer to recieve the raster image
								const unsigned int width,			///< [in] Width
								const unsigned int height			///< [in] Height
								);

	//! Creates a scale color operator
	bool RISE_API_CreateScaleColorOperatorRasterImage(
								IOneColorOperator** ppi,			///< [out] Pointer to recieve the operator
								const RISEColor& scale				///< [in] Scale factor
								);

	//! Creates a shift color operator
	bool RISE_API_CreateShiftColorOperatorRasterImage(
								IOneColorOperator** ppi,			///< [out] Pointer to recieve the operator
								const RISEColor& shift				///< [in] Shift factor
								);

	//! Creates a nearest neighbour raster image accessor.  `wrap_s` /
	//! `wrap_t` choose the per-axis address-wrap mode (0 = clamp, 1 =
	//! repeat, 2 = mirrored repeat — see eRasterWrapMode).  Default 0
	//! preserves the pre-2026-05-01 clamp-to-edge behaviour for any
	//! existing caller that doesn't pass wrap params (notably the
	//! `imageconverter` and `meshconverter` standalones built on
	//! Linux/macOS that don't go through Job's painter API).
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateNNBRasterImageAccessor(
								IRasterImageAccessor** ppi,			///< [out] Pointer to recieve the accessor
								IRasterImage& image,				///< [in] Raster image to access
								const char wrap_s = 0,				///< [in] Wrap mode for U axis
								const char wrap_t = 0				///< [in] Wrap mode for V axis
								);

	//! Creates a bilinear raster image accessor
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBiLinRasterImageAccessor(
								IRasterImageAccessor** ppi,			///< [out] Pointer to recieve the accessor
								IRasterImage& image,				///< [in] Raster image to access
								const char wrap_s = 0,				///< [in] Wrap mode for U axis
								const char wrap_t = 0				///< [in] Wrap mode for V axis
								);

	//! Creates a catmull rom bicubic raster image accessor
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCatmullRomBicubicRasterImageAccessor(
								IRasterImageAccessor** ppi,			///< [out] Pointer to recieve the accessor
								IRasterImage& image,				///< [in] Raster image to access
								const char wrap_s = 0,				///< [in] Wrap mode for U axis
								const char wrap_t = 0				///< [in] Wrap mode for V axis
								);


	//! Creates a bspline bicubic raster image accessor
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateUniformBSplineBicubicRasterImageAccessor(
								IRasterImageAccessor** ppi,			///< [out] Pointer to recieve the accessor
								IRasterImage& image,				///< [in] Raster image to access
								const char wrap_s = 0,				///< [in] Wrap mode for U axis
								const char wrap_t = 0				///< [in] Wrap mode for V axis
								);

	//! Creates a bicubic raster image accessor that uses the given matrix as the weights
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBicubicRasterImageAccessor(
								IRasterImageAccessor** ppi,			///< [out] Pointer to recieve the accessor
								IRasterImage& image,				///< [in] Raster image to access
								const Matrix4& m,
								const char wrap_s = 0,				///< [in] Wrap mode for U axis
								const char wrap_t = 0				///< [in] Wrap mode for V axis
								);

	//! Creates a PNG reader
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePNGReader(
								IRasterImageReader** ppi,			///< [out] Pointer to recieve the raster image reader
								IReadBuffer& buffer,				///< [in] Buffer containing PNG data
								const COLOR_SPACE color_space		///< [in] Color space in the file
								);

	//! Creates a PNG writer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePNGWriter(
								IRasterImageWriter** ppi,			///< [out] Pointer to recieve the raster image writer
								IWriteBuffer& buffer,				///< [in] Buffer to write to
								const unsigned char bpp,			///< [in] Bits / pixel for the file
								const COLOR_SPACE color_space		///< [in] Color space to apply
								);

	//! Creates a JPEG reader
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateJPEGReader(
								IRasterImageReader** ppi,			///< [out] Pointer to recieve the raster image reader
								IReadBuffer& buffer,				///< [in] Buffer containing JPEG data
								const COLOR_SPACE color_space		///< [in] Color space in the file
								);

	//! Creates a TGA reader
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTGAReader(
								IRasterImageReader** ppi,			///< [out] Pointer to recieve the raster image reader
								IReadBuffer& buffer,				///< [in] Buffer containing TGA data
								const COLOR_SPACE color_space		///< [in] Color space in the file
								);

	//! Creates a HDR reader (Radiance RGBE format)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateHDRReader(
								IRasterImageReader** ppi,			///< [out] Pointer to recieve the raster image reader
								IReadBuffer& buffer					///< [in] Buffer containing HDR data
								);

	//! Creates a TGA writer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTGAWriter(
								IRasterImageWriter** ppi,			///< [out] Pointer to recieve the raster image writer
								IWriteBuffer& buffer,				///< [in] Buffer to write to
								const COLOR_SPACE color_space		///< [in] Color space to apply
								);

	//! Creates a PPM writer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePPMWriter(
								IRasterImageWriter** ppi,			///< [out] Pointer to recieve the raster image writer
								IWriteBuffer& buffer,				///< [in] Buffer to write to
								const COLOR_SPACE color_space 		///< [in] Color space to apply
								);

	//! Creates a HDR writer (Radiance RGBE format)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateHDRWriter(
								IRasterImageWriter** ppi,			///< [out] Pointer to recieve the raster image writer
								IWriteBuffer& buffer,				///< [in] Buffer to write to
								const COLOR_SPACE color_space		///< [in] Color space to apply
								);

	//! Creates a RGBEA writer (variant on the Radiance RGBE format)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateRGBEAWriter(
								IRasterImageWriter** ppi,			///< [out] Pointer to recieve the raster image writer
								IWriteBuffer& buffer				///< [in] Buffer to write to
								);

	//! Creates a TIFF reader
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTIFFReader(
								IRasterImageReader** ppi,			///< [out] Pointer to recieve the raster image reader
								IReadBuffer& buffer,				///< [in] Buffer containing TIFF data
								const COLOR_SPACE color_space		///< [in] Color space in the file
								);

	//! Creates a TIFF writer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTIFFWriter(
								IRasterImageWriter** ppi,			///< [out] Pointer to recieve the raster image writer
								IWriteBuffer& buffer,				///< [in] Buffer to write to
								const COLOR_SPACE color_space		///< [in] Color space to apply
								);

	//! Creates an OpenEXR reader
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateEXRReader(
								IRasterImageReader** ppi,			///< [out] Pointer to recieve the raster image reader
								IReadBuffer& buffer,				///< [in] Buffer to read from
								const COLOR_SPACE color_space		///< [in] Color space to apply
								);

	//! Creates a EXR writer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateEXRWriter(
								IRasterImageWriter** ppi,			///< [out] Pointer to recieve the raster image writer
								IWriteBuffer& buffer,				///< [in] Buffer to write to
								const COLOR_SPACE color_space,		///< [in] Color space to apply
								const EXR_COMPRESSION compression,	///< [in] EXR compression mode (PIZ default)
								const bool with_alpha				///< [in] Write alpha channel (true default)
								);


	//////////////////////////////////////////////////////////
	// Shader Ops
	//////////////////////////////////////////////////////////

	//! Creates a reflection shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateReflectionShaderOp(
								IShaderOp** ppi					///< [out] Pointer to recieve the shaderop
								);

	//! Creates a refraction shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateRefractionShaderOp(
								IShaderOp** ppi					///< [out] Pointer to recieve the shaderop
								);

	//! Creates an emission shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateEmissionShaderOp(
								IShaderOp** ppi					///< [out] Pointer to recieve the shaderop
								);

	//! Creates a direct lighting shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDirectLightingShaderOp(
								IShaderOp** ppi,				///< [out] Pointer to recieve the shaderop
								const IMaterial* bsdf			///< [in] BSDF to use when computing radiance (overrides object BSDF)
								);

	//! Creates a caustic pel photonmap shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCausticPelPhotonMapShaderOp(
								IShaderOp** ppi					///< [out] Pointer to recieve the shaderop
								);

	//! Creates a caustic spectral photonmap shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCausticSpectralPhotonMapShaderOp(
								IShaderOp** ppi					///< [out] Pointer to recieve the shaderop
								);

	//! Creates a global pel photonmap shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGlobalPelPhotonMapShaderOp(
								IShaderOp** ppi					///< [out] Pointer to recieve the shaderop
								);

	//! Creates a global spectral photonmap shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGlobalSpectralPhotonMapShaderOp(
								IShaderOp** ppi					///< [out] Pointer to recieve the shaderop
								);

	//! Creates a translucent pel photonmap shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTranslucentPelPhotonMapShaderOp(
								IShaderOp** ppi					///< [out] Pointer to recieve the shaderop
								);

	//! Creates a shadow photonmap shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateShadowPhotonMapShaderOp(
								IShaderOp** ppi					///< [out] Pointer to recieve the shaderop
								);

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
								);

	//! Creates an ambient occlusion shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAmbientOcclusionShaderOp(
								IShaderOp** ppi,					///< [out] Pointer to recieve the shaderop
								const unsigned int numThetaSamples, ///< [in] Number of samples in the theta direction
								const unsigned int numPhiSamples,	///< [in] Number of samples in the phi direction
								const bool multiplyBRDF,			///< [in] Should individual samples by multiplied by the BRDF?
								const bool irradiance_cache			///< [in] Should the irradiance state cache be used?
								);

	//! Creates a final gather shaderop
	/// \return TRUE if successful, FALSE otherwise
bool RISE_API_CreateFinalGatherShaderOp(
								IShaderOp** ppi,				///< [out] Pointer to recieve the shaderop
								const unsigned int numThetaSamples, ///< [in] Number of samples in the theta direction
								const unsigned int numPhiSamples,	///< [in] Number of samples in the phi direction
								const bool cachegradients,		///< [in] Should cache gradients be used in the irradiance cache?
								const unsigned int min_effective_contributors,	///< [in] Minimum effective contributors required for interpolation
								const Scalar high_variation_reuse_scale,	///< [in] Minimum reuse scale for bright high-variation cache records
								const bool cache				///< [in] Should the rasterizer state cache be used?
								);

	//! Creates a path tracing shaderop with integrated MIS and optional SMS
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePathTracingShaderOp(
								IShaderOp** pShaderOp,
								const bool smsEnabled,
								const unsigned int smsMaxIterations,
								const double smsThreshold,
								const unsigned int smsMaxChainDepth,
								const bool smsBiased
								);

	//! Creates a standalone SMS shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSMSShaderOp(
								IShaderOp** pShaderOp,
								const unsigned int maxIterations,
								const double threshold,
								const unsigned int maxChainDepth,
								const bool biased
								);

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
								);

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
								);

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
								);

	//! Creates a transparency shaderop
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTransparencyShaderOp(
								IShaderOp** ppi,						///< [out] Pointer to recieve the shaderop
								const IPainter& transparency,			///< [in] Transparency
								const bool one_sided					///< [in] One sided transparency only (ignore backfaces)
								);

	//! Creates an alpha-test (cutout) shaderop.  See IJob::AddAlphaTestShaderOp
	//! for the integrator-compatibility caveat.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAlphaTestShaderOp(
								IShaderOp** ppi,						///< [out] Pointer to recieve the shaderop
								const IPainter& alpha_painter,			///< [in] Alpha painter
								const Scalar    cutoff					///< [in] alpha < cutoff -> continue ray past surface
								);


	//////////////////////////////////////////////////////////
	// Shaders
	//////////////////////////////////////////////////////////

	//! Creates a standard shader
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateStandardShader(
								IShader** ppi,						///< [out] Pointer to recieve the shader
								const std::vector<IShaderOp*>& ops	///< [in] The shader ops
								);

	//! Creates an advanced shader
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAdvancedShader(
								IShader** ppi,						///< [out] Pointer to recieve the shader
								const std::vector<IShaderOp*>& ops,	///< [in] The shader ops
								const std::vector<unsigned int>& m,	///< [in] Parallel array of minimum depths
								const std::vector<unsigned int>& x,	///< [in] Parallel array of maximum depths
								const char* operations				///< [in] Parallel array of operations
								);


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
								);

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
								);

	//////////////////////////////////////////////////////////
	// Rendering Core
	/////////////////////////////////////////////////////////

	//! Creates a luminary manager
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateLuminaryManager(
								ILuminaryManager** ppi				///< [out] Pointer to recieve the luminary manager
								);

	//! Creates a ray caster (something similar to a ray server)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateRayCaster(
								IRayCaster** ppi,					///< [out] Pointer to recieve the ray caster
								const bool seeRadianceMap,			///< [in] Is the radiance map (environment) visible to the view rays?
								const unsigned int maxR,			///< [in] Maximum recursion level
								const IShader& pDefaultShader,		///< [in] The default global shader
								const bool showLuminaires			///< [in] Should we be able to see luminaries?
								);

	//////////////////////////////////////////////////////////
	// Rasterizers
	//////////////////////////////////////////////////////////

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
								const bool useZSobol				///< [in] Use Morton-indexed Sobol (blue-noise error distribution)
								);

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
								const bool useHWSS				///< [in] Use Hero Wavelength Spectral Sampling
								);

	//! Creates a Pel (RGB) BDPT rasterizer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBDPTPelRasterizer(
								IRasterizer** ppi,					///< [out] Pointer to recieve the rasterizer
								IRayCaster* caster,					///< [in] Ray caster to use for rays
								ISampling2D* pSamples,				///< [in] Sampler for subsamples
								IPixelFilter* pFilter,				///< [in] Pixel Filter for samples
								const unsigned int maxEyeDepth,		///< [in] Maximum eye subpath depth
								const unsigned int maxLightDepth,	///< [in] Maximum light subpath depth
								const bool smsEnabled,				///< [in] Enable Specular Manifold Sampling
								const unsigned int smsMaxIterations,///< [in] SMS Newton iteration limit
								const double smsThreshold,			///< [in] SMS convergence threshold
								const unsigned int smsMaxChainDepth,///< [in] SMS maximum specular chain depth
								const bool smsBiased,				///< [in] SMS biased mode (skip Bernoulli PDF)
								const unsigned int smsBernoulliTrials,///< [in] SMS Bernoulli trials for unbiased PDF
								const unsigned int smsMultiTrials,	///< [in] SMS independent Newton solves per eval (Zeltner 2020); 1 = single-solve
								const unsigned int smsPhotonCount,	///< [in] SMS photon-aided seeding budget; 0 = disabled (single-seed fallback)
								const bool smsTwoStage,				///< [in] SMS two-stage solver (Zeltner 2020 §5); first pass on smoothed surface, second on actual.  Helps Newton through C1-discontinuity plateau on Phong-shaded triangle meshes.
								const bool smsUseLevenbergMarquardt,	///< [in] LM damping in NewtonSolve; default true.  Recovers ~5pp Newton-fail rate on heavy-displacement scenes at the cost of ~50-100% more solver work per shading point.
								const SMSSeedingMode smsSeedingMode,	///< [in] SMS seeding strategy: `SMSSeedingMode::Snell` (legacy Snell-trace, default) or `SMSSeedingMode::Uniform` (Mitsuba-faithful uniform-area on caustic-caster shapes; required for principled geometric Bernoulli).
								const bool oidnDenoise,				///< [in] Enable OIDN denoising post-process
								const OidnQuality oidnQuality,		///< [in] OIDN quality preset (Auto = render-time heuristic)
							const OidnDevice oidnDevice,		///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
							const OidnPrefilter oidnPrefilter,	///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
								const PathGuidingConfig& guidingConfig,	///< [in] Path guiding configuration
								const AdaptiveSamplingConfig& adaptiveConfig,	///< [in] Adaptive sampling configuration
								const StabilityConfig& stabilityConfig,	///< [in] Production stability controls
								const bool useZSobol				///< [in] Use Morton-indexed Sobol (blue-noise error distribution)
								);

	//! Creates a spectral BDPT rasterizer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBDPTSpectralRasterizer(
								IRasterizer** ppi,					///< [out] Pointer to recieve the rasterizer
								IRayCaster* caster,					///< [in] Ray caster to use for rays
								ISampling2D* pSamples,				///< [in] Sampler for subsamples
								IPixelFilter* pFilter,				///< [in] Pixel Filter for samples
								const unsigned int maxEyeDepth,		///< [in] Maximum eye subpath depth
								const unsigned int maxLightDepth,	///< [in] Maximum light subpath depth
								const Scalar lambda_begin,			///< [in] Start wavelength (nm)
								const Scalar lambda_end,			///< [in] End wavelength (nm)
								const unsigned int num_wavelengths,	///< [in] Number of wavelength bins
								const unsigned int spectral_samples,///< [in] Spectral samples per pixel
								const bool smsEnabled,				///< [in] Enable Specular Manifold Sampling
								const unsigned int smsMaxIterations,///< [in] SMS Newton iteration limit
								const double smsThreshold,			///< [in] SMS convergence threshold
								const unsigned int smsMaxChainDepth,///< [in] SMS maximum specular chain depth
								const bool smsBiased,				///< [in] SMS biased mode (skip Bernoulli PDF)
								const unsigned int smsBernoulliTrials,///< [in] SMS Bernoulli trials for unbiased PDF
								const unsigned int smsMultiTrials,	///< [in] SMS independent Newton solves per eval (Zeltner 2020); 1 = single-solve
								const unsigned int smsPhotonCount,	///< [in] SMS photon-aided seeding budget; 0 = disabled (single-seed fallback)
								const bool smsTwoStage,				///< [in] SMS two-stage solver (Zeltner 2020 §5); first pass on smoothed surface, second on actual.  Helps Newton through C1-discontinuity plateau on Phong-shaded triangle meshes.
								const bool smsUseLevenbergMarquardt,	///< [in] LM damping in NewtonSolve; default true.  Recovers ~5pp Newton-fail rate on heavy-displacement scenes at the cost of ~50-100% more solver work per shading point.
								const SMSSeedingMode smsSeedingMode,	///< [in] SMS seeding strategy: `SMSSeedingMode::Snell` (legacy Snell-trace, default) or `SMSSeedingMode::Uniform` (Mitsuba-faithful uniform-area on caustic-caster shapes; required for principled geometric Bernoulli).
								const bool oidnDenoise,				///< [in] Enable OIDN denoising post-process
								const OidnQuality oidnQuality,		///< [in] OIDN quality preset (Auto = render-time heuristic)
							const OidnDevice oidnDevice,		///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
							const OidnPrefilter oidnPrefilter,	///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
								const PathGuidingConfig& guidingConfig,	///< [in] Path guiding configuration
								const StabilityConfig& stabilityConfig,	///< [in] Production stability controls
								const bool useZSobol,				///< [in] Use Morton-indexed Sobol (blue-noise error distribution)
								const bool useHWSS					///< [in] Use Hero Wavelength Spectral Sampling
								);

	//! Creates a Pel (RGB) Vertex Connection and Merging rasterizer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateVCMPelRasterizer(
								IRasterizer** ppi,					///< [out] Pointer to receive the rasterizer
								IRayCaster* caster,					///< [in] Ray caster to use for rays
								ISampling2D* pSamples,				///< [in] Sampler for subsamples
								IPixelFilter* pFilter,				///< [in] Pixel Filter for samples
								const unsigned int maxEyeDepth,		///< [in] Maximum eye subpath depth
								const unsigned int maxLightDepth,	///< [in] Maximum light subpath depth
								const Scalar mergeRadius,			///< [in] Merge radius (0 => scene-auto fallback)
								const bool enableVC,				///< [in] Enable vertex connection strategies
								const bool enableVM,				///< [in] Enable vertex merging strategy
								const bool oidnDenoise,				///< [in] Enable OIDN denoising
								const OidnQuality oidnQuality,		///< [in] OIDN quality preset (Auto = render-time heuristic)
							const OidnDevice oidnDevice,		///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
							const OidnPrefilter oidnPrefilter,	///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
								const PathGuidingConfig& guidingConfig,	///< [in] Path guiding configuration
								const AdaptiveSamplingConfig& adaptiveConfig,	///< [in] Adaptive sampling configuration
								const StabilityConfig& stabilityConfig,	///< [in] Production stability controls
								const bool useZSobol				///< [in] Use Morton-indexed Sobol (blue-noise error distribution)
								);

	//! Creates a spectral Vertex Connection and Merging rasterizer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateVCMSpectralRasterizer(
								IRasterizer** ppi,					///< [out] Pointer to receive the rasterizer
								IRayCaster* caster,					///< [in] Ray caster to use for rays
								ISampling2D* pSamples,				///< [in] Sampler for subsamples
								IPixelFilter* pFilter,				///< [in] Pixel Filter for samples
								const unsigned int maxEyeDepth,		///< [in] Maximum eye subpath depth
								const unsigned int maxLightDepth,	///< [in] Maximum light subpath depth
								const Scalar lambda_begin,			///< [in] Start wavelength (nm)
								const Scalar lambda_end,			///< [in] End wavelength (nm)
								const unsigned int num_wavelengths,	///< [in] Number of wavelength bins
								const unsigned int spectral_samples,///< [in] Spectral samples per pixel
								const Scalar mergeRadius,			///< [in] Merge radius (0 => scene-auto fallback)
								const bool enableVC,				///< [in] Enable vertex connection strategies
								const bool enableVM,				///< [in] Enable vertex merging strategy
								const bool oidnDenoise,				///< [in] Enable OIDN denoising
								const OidnQuality oidnQuality,		///< [in] OIDN quality preset (Auto = render-time heuristic)
							const OidnDevice oidnDevice,		///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
							const OidnPrefilter oidnPrefilter,	///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
								const PathGuidingConfig& guidingConfig,	///< [in] Path guiding configuration
								const AdaptiveSamplingConfig& adaptiveConfig,	///< [in] Adaptive sampling configuration
								const StabilityConfig& stabilityConfig,	///< [in] Production stability controls
								const bool useZSobol,				///< [in] Use Morton-indexed Sobol (blue-noise error distribution)
								const bool useHWSS					///< [in] Use Hero Wavelength Spectral Sampling
								);

	//! Creates a pure path tracing Pel rasterizer (bypasses shader ops)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePathTracingPelRasterizer(
								IRasterizer** ppi,					///< [out] Pointer to recieve the rasterizer
								IRayCaster* caster,					///< [in] Ray caster to use for rays
								ISampling2D* pSamples,				///< [in] Sampler for subsamples
								IPixelFilter* pFilter,				///< [in] Pixel Filter for samples
								const bool smsEnabled,				///< [in] Enable Specular Manifold Sampling
								const unsigned int smsMaxIterations,///< [in] SMS Newton iteration limit
								const double smsThreshold,			///< [in] SMS convergence threshold
								const unsigned int smsMaxChainDepth,///< [in] SMS maximum specular chain depth
								const bool smsBiased,				///< [in] SMS biased mode (skip Bernoulli PDF)
								const unsigned int smsBernoulliTrials,///< [in] SMS Bernoulli trials for unbiased PDF
								const unsigned int smsMultiTrials,	///< [in] SMS independent Newton solves per eval (Zeltner 2020); 1 = single-solve
								const unsigned int smsPhotonCount,	///< [in] SMS photon-aided seeding budget; 0 = disabled (single-seed fallback)
								const bool smsTwoStage,				///< [in] SMS two-stage solver (Zeltner 2020 §5); first pass on smoothed surface, second on actual.  Helps Newton through C1-discontinuity plateau on Phong-shaded triangle meshes.
								const bool smsUseLevenbergMarquardt,	///< [in] LM damping in NewtonSolve; default true.  Recovers ~5pp Newton-fail rate on heavy-displacement scenes at the cost of ~50-100% more solver work per shading point.
								const SMSSeedingMode smsSeedingMode,	///< [in] SMS seeding strategy: `SMSSeedingMode::Snell` (legacy Snell-trace, default) or `SMSSeedingMode::Uniform` (Mitsuba-faithful uniform-area on caustic-caster shapes; required for principled geometric Bernoulli).
								const bool oidnDenoise,				///< [in] Enable OIDN denoising post-process
								const OidnQuality oidnQuality,		///< [in] OIDN quality preset (Auto = render-time heuristic)
							const OidnDevice oidnDevice,		///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
							const OidnPrefilter oidnPrefilter,	///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
								const PathGuidingConfig& guidingConfig,	///< [in] Path guiding configuration
								const AdaptiveSamplingConfig& adaptiveConfig,	///< [in] Adaptive sampling configuration
								const StabilityConfig& stabilityConfig,	///< [in] Production stability controls
								const bool useZSobol				///< [in] Use Morton-indexed Sobol (blue-noise error distribution)
								);

	//! Creates a pure path tracing spectral rasterizer (bypasses shader ops)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePathTracingSpectralRasterizer(
								IRasterizer** ppi,					///< [out] Pointer to recieve the rasterizer
								IRayCaster* caster,					///< [in] Ray caster to use for rays
								ISampling2D* pSamples,				///< [in] Sampler for subsamples
								IPixelFilter* pFilter,				///< [in] Pixel Filter for samples
								const Scalar lambda_begin,			///< [in] Start wavelength (nm)
								const Scalar lambda_end,			///< [in] End wavelength (nm)
								const unsigned int num_wavelengths,	///< [in] Number of wavelength bins
								const unsigned int spectral_samples,///< [in] Spectral samples per pixel
								const bool smsEnabled,				///< [in] Enable Specular Manifold Sampling
								const unsigned int smsMaxIterations,///< [in] SMS Newton iteration limit
								const double smsThreshold,			///< [in] SMS convergence threshold
								const unsigned int smsMaxChainDepth,///< [in] SMS maximum specular chain depth
								const bool smsBiased,				///< [in] SMS biased mode (skip Bernoulli PDF)
								const unsigned int smsBernoulliTrials,///< [in] SMS Bernoulli trials for unbiased PDF
								const unsigned int smsMultiTrials,	///< [in] SMS independent Newton solves per eval (Zeltner 2020); 1 = single-solve
								const unsigned int smsPhotonCount,	///< [in] SMS photon-aided seeding budget; 0 = disabled (single-seed fallback)
								const bool smsTwoStage,				///< [in] SMS two-stage solver (Zeltner 2020 §5); first pass on smoothed surface, second on actual.  Helps Newton through C1-discontinuity plateau on Phong-shaded triangle meshes.
								const bool smsUseLevenbergMarquardt,	///< [in] LM damping in NewtonSolve; default true.  Recovers ~5pp Newton-fail rate on heavy-displacement scenes at the cost of ~50-100% more solver work per shading point.
								const SMSSeedingMode smsSeedingMode,	///< [in] SMS seeding strategy: `SMSSeedingMode::Snell` (legacy Snell-trace, default) or `SMSSeedingMode::Uniform` (Mitsuba-faithful uniform-area on caustic-caster shapes; required for principled geometric Bernoulli).
								const bool oidnDenoise,				///< [in] Enable OIDN denoising post-process
								const OidnQuality oidnQuality,		///< [in] OIDN quality preset (Auto = render-time heuristic)
							const OidnDevice oidnDevice,		///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
							const OidnPrefilter oidnPrefilter,	///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
								const AdaptiveSamplingConfig& adaptiveConfig,	///< [in] Adaptive sampling configuration
								const StabilityConfig& stabilityConfig,	///< [in] Production stability controls
								const bool useZSobol,				///< [in] Use Morton-indexed Sobol (blue-noise error distribution)
								const bool useHWSS					///< [in] Use Hero Wavelength Spectral Sampling
								);

	//! Configures progressive multi-pass rendering on a pixel-based rasterizer.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_SetRasterizerProgressiveRendering(
								IRasterizer* pRasterizer,			///< [in] Rasterizer to configure
								const bool enabled,					///< [in] Enable progressive multi-pass rendering
								const unsigned int samplesPerPass	///< [in] SPP per progressive pass
								);

	//! Creates an MLT (Metropolis Light Transport / PSSMLT) rasterizer.
	//! LEGACY signature preserved for ABI/source compatibility with
	//! external code linked against pre-filter RISE.  Equivalent to
	//! RISE_API_CreateMLTRasterizerWithFilter with null sampler/filter,
	//! which makes the MLT render loop fall back to round-to-nearest
	//! point splats (the pre-change behaviour).  New code should call
	//! the *WithFilter variant directly.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMLTRasterizer(
								IRasterizer** ppi,					///< [out] Pointer to recieve the rasterizer
								IRayCaster* caster,					///< [in] Ray caster to use for rays
								const unsigned int maxEyeDepth,		///< [in] Maximum eye subpath depth
								const unsigned int maxLightDepth,	///< [in] Maximum light subpath depth
								const unsigned int nBootstrap,				///< [in] Number of bootstrap samples
								const unsigned int nChains,					///< [in] Number of Markov chains
								const unsigned int nMutationsPerPixel,		///< [in] Mutations per pixel budget
								const Scalar largeStepProb,					///< [in] Large step probability
								const bool oidnDenoise						///< [in] Enable OIDN denoising post-process
								);

	//! Extended MLT rasterizer factory with pixel sampler + filter.
	//! This is the entry point Job::SetMLTRasterizer uses — it wires
	//! up sub-pixel reconstruction via SplatFilm::SplatFiltered.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMLTRasterizerWithFilter(
								IRasterizer** ppi,					///< [out] Pointer to recieve the rasterizer
								IRayCaster* caster,					///< [in] Ray caster to use for rays
								const unsigned int maxEyeDepth,		///< [in] Maximum eye subpath depth
								const unsigned int maxLightDepth,	///< [in] Maximum light subpath depth
								const unsigned int nBootstrap,				///< [in] Number of bootstrap samples
								const unsigned int nChains,					///< [in] Number of Markov chains
								const unsigned int nMutationsPerPixel,		///< [in] Mutations per pixel budget
								const Scalar largeStepProb,					///< [in] Large step probability
								const bool oidnDenoise,						///< [in] Enable OIDN denoising post-process
								const OidnQuality oidnQuality,				///< [in] OIDN quality preset (Auto = render-time heuristic)
								const OidnDevice oidnDevice,				///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
								const OidnPrefilter oidnPrefilter,			///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
								ISampling2D* pSampler,						///< [in] Pixel sampler (stored but unused by the MLT loop); may be null
								IPixelFilter* pFilter						///< [in] Reconstruction kernel; may be null for unfiltered point splats
								);

	//! Creates a spectral MLT (Metropolis Light Transport / PSSMLT) rasterizer.
	//! LEGACY signature preserved for ABI compatibility — see the Pel
	//! variant above.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMLTSpectralRasterizer(
								IRasterizer** ppi,					///< [out] Pointer to recieve the rasterizer
								IRayCaster* caster,					///< [in] Ray caster to use for rays
								const unsigned int maxEyeDepth,		///< [in] Maximum eye subpath depth
								const unsigned int maxLightDepth,	///< [in] Maximum light subpath depth
								const unsigned int nBootstrap,		///< [in] Number of bootstrap samples
								const unsigned int nChains,			///< [in] Number of Markov chains
								const unsigned int nMutationsPerPixel,	///< [in] Mutations per pixel budget
								const Scalar largeStepProb,			///< [in] Large step probability
								const Scalar lambda_begin,			///< [in] Start of spectral range (nm)
								const Scalar lambda_end,			///< [in] End of spectral range (nm)
								const unsigned int nSpectralSamples,///< [in] Spectral samples per evaluation
								const bool useHWSS,					///< [in] Use Hero Wavelength Spectral Sampling
								const bool oidnDenoise				///< [in] Enable OIDN denoising post-process
								);

	//! Extended spectral MLT rasterizer factory with pixel sampler + filter.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateMLTSpectralRasterizerWithFilter(
								IRasterizer** ppi,					///< [out] Pointer to recieve the rasterizer
								IRayCaster* caster,					///< [in] Ray caster to use for rays
								const unsigned int maxEyeDepth,		///< [in] Maximum eye subpath depth
								const unsigned int maxLightDepth,	///< [in] Maximum light subpath depth
								const unsigned int nBootstrap,		///< [in] Number of bootstrap samples
								const unsigned int nChains,			///< [in] Number of Markov chains
								const unsigned int nMutationsPerPixel,	///< [in] Mutations per pixel budget
								const Scalar largeStepProb,			///< [in] Large step probability
								const Scalar lambda_begin,			///< [in] Start of spectral range (nm)
								const Scalar lambda_end,			///< [in] End of spectral range (nm)
								const unsigned int nSpectralSamples,///< [in] Spectral samples per evaluation
								const bool useHWSS,					///< [in] Use Hero Wavelength Spectral Sampling
								const bool oidnDenoise,				///< [in] Enable OIDN denoising post-process
								const OidnQuality oidnQuality,		///< [in] OIDN quality preset (Auto = render-time heuristic)
							const OidnDevice oidnDevice,		///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
							const OidnPrefilter oidnPrefilter,	///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
								ISampling2D* pSampler,				///< [in] Pixel sampler (stored but unused by the MLT loop); may be null
								IPixelFilter* pFilter				///< [in] Reconstruction kernel; may be null for unfiltered point splats
								);

	//////////////////////////////////////////////////////////
	// Parsers
	//////////////////////////////////////////////////////////

	//! Creates a parser capable of loading a job from a text file
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAsciiSceneParser(
								ISceneParser** ppi,					///< [out] Pointer to recieve the parser
								const char* name					///< [in] Name of the file to load
								);

	//! Creates a parser capable of processing a script from a text file
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAsciiScriptParser(
								IScriptParser** ppi,				///< [out] Pointer to recieve the parser
								const char* name					///< [in] Name of the file to load
								);

	//! Creates a command parser capable of parsing string commands and putting them
	//! in a job
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAsciiCommandParser(
								ICommandParser** ppi				///< [out] Pointer to recieve the parser
								);

	//! Creates an options file parser
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateOptionsParser(
								IOptions** ppi,						///< [out] Pointer to recieve the parser
								const char* name					///< [in] Name of the file to load
								);


	//////////////////////////////////////////////////////////
	// Phase functions and participating media
	//////////////////////////////////////////////////////////

	//! Creates an isotropic phase function (uniform scattering over the sphere)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateIsotropicPhaseFunction(
								IPhaseFunction** ppi				///< [out] Pointer to recieve the phase function
								);

	//! Creates a Henyey-Greenstein phase function with given asymmetry factor
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateHenyeyGreensteinPhaseFunction(
								IPhaseFunction** ppi,				///< [out] Pointer to recieve the phase function
								const Scalar g						///< [in] Asymmetry factor [-1, 1]
								);

	//! Creates a homogeneous participating medium with constant coefficients
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateHomogeneousMedium(
								IMedium** ppi,						///< [out] Pointer to recieve the medium
								const RISEPel& sigma_a,				///< [in] Absorption coefficient
								const RISEPel& sigma_s,				///< [in] Scattering coefficient
								const IPhaseFunction& phase			///< [in] Phase function for scattering
								);

	//! Creates a homogeneous participating medium with emission
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateHomogeneousMediumWithEmission(
								IMedium** ppi,						///< [out] Pointer to recieve the medium
								const RISEPel& sigma_a,				///< [in] Absorption coefficient
								const RISEPel& sigma_s,				///< [in] Scattering coefficient
								const RISEPel& emission,			///< [in] Volumetric emission
								const IPhaseFunction& phase			///< [in] Phase function for scattering
								);


	//! Creates a heterogeneous participating medium driven by a volume dataset
	/// Uses delta tracking for distance sampling and deterministic ray
	/// marching for transmittance.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateHeterogeneousMedium(
								IMedium** ppi,						///< [out] Pointer to recieve the medium
								const RISEPel& max_sigma_a,			///< [in] Max absorption coefficient
								const RISEPel& max_sigma_s,		///< [in] Max scattering coefficient
								const IPhaseFunction& phase,		///< [in] Phase function for scattering
								const char* szVolumeFilePattern,	///< [in] File pattern for volume slices
								const unsigned int volWidth,		///< [in] Volume width in voxels
								const unsigned int volHeight,		///< [in] Volume height in voxels
								const unsigned int volStartZ,		///< [in] Starting z slice index
								const unsigned int volEndZ,			///< [in] Ending z slice index
								const char accessor,				///< [in] Volume accessor type: 'n'=NNB, 't'=trilinear
								const Point3& bboxMin,				///< [in] World-space AABB minimum corner
								const Point3& bboxMax				///< [in] World-space AABB maximum corner
								);

	//! Creates a heterogeneous participating medium with emission
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateHeterogeneousMediumWithEmission(
								IMedium** ppi,						///< [out] Pointer to recieve the medium
								const RISEPel& max_sigma_a,			///< [in] Max absorption coefficient
								const RISEPel& max_sigma_s,		///< [in] Max scattering coefficient
								const RISEPel& emission,			///< [in] Volumetric emission
								const IPhaseFunction& phase,		///< [in] Phase function for scattering
								const char* szVolumeFilePattern,	///< [in] File pattern for volume slices
								const unsigned int volWidth,		///< [in] Volume width in voxels
								const unsigned int volHeight,		///< [in] Volume height in voxels
								const unsigned int volStartZ,		///< [in] Starting z slice index
								const unsigned int volEndZ,			///< [in] Ending z slice index
								const char accessor,				///< [in] Volume accessor type: 'n'=NNB, 't'=trilinear
								const Point3& bboxMin,				///< [in] World-space AABB minimum corner
								const Point3& bboxMax				///< [in] World-space AABB maximum corner
								);


	//! Creates a heterogeneous participating medium driven by a painter
	/// The painter is evaluated at world-space points to produce density
	/// values.  A virtual resolution controls the majorant grid granularity.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePainterHeterogeneousMedium(
								IMedium** ppi,						///< [out] Pointer to recieve the medium
								const RISEPel& max_sigma_a,			///< [in] Max absorption coefficient
								const RISEPel& max_sigma_s,		///< [in] Max scattering coefficient
								const IPhaseFunction& phase,		///< [in] Phase function for scattering
								const IPainter& densityPainter,		///< [in] Painter to evaluate for density
								const unsigned int virtualResolution,///< [in] Virtual grid resolution per axis
								const char colorToScalar,			///< [in] Color-to-scalar mode: 'l', 'm', or 'r'
								const Point3& bboxMin,				///< [in] World-space AABB minimum corner
								const Point3& bboxMax				///< [in] World-space AABB maximum corner
								);

	//! Creates a heterogeneous participating medium driven by a painter, with emission
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePainterHeterogeneousMediumWithEmission(
								IMedium** ppi,						///< [out] Pointer to recieve the medium
								const RISEPel& max_sigma_a,			///< [in] Max absorption coefficient
								const RISEPel& max_sigma_s,		///< [in] Max scattering coefficient
								const RISEPel& emission,			///< [in] Volumetric emission
								const IPhaseFunction& phase,		///< [in] Phase function for scattering
								const IPainter& densityPainter,		///< [in] Painter to evaluate for density
								const unsigned int virtualResolution,///< [in] Virtual grid resolution per axis
								const char colorToScalar,			///< [in] Color-to-scalar mode: 'l', 'm', or 'r'
								const Point3& bboxMin,				///< [in] World-space AABB minimum corner
								const Point3& bboxMax				///< [in] World-space AABB maximum corner
								);


	//////////////////////////////////////////////////////////
	// Virtual measurement devices
	//////////////////////////////////////////////////////////

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
								);

	//////////////////////////////////////////////////////////
	// Interactive scene editor (cross-platform 3D viewport)
	//
	// See docs/INTERACTIVE_EDITOR_PLAN.md.  These are the C-API
	// entry points that platform bridges (RISEViewportBridge.mm
	// on macOS, the Qt RenderEngine on Windows, the JNI bridge on
	// Android) consume.  The controller owns the render thread,
	// the cancel-restart loop, and the toolbar state machine; the
	// platform code just forwards events here.
	//////////////////////////////////////////////////////////

	class SceneEditController;

	//! Tool enum mirroring SceneEditController::Tool.  Kept as a
	//! plain int in the C-API surface for ABI stability across
	//! language bridges (Swift, Kotlin/JNI, Qt).
	enum SceneEditTool
	{
		SceneEditTool_Select           = 0,
		SceneEditTool_TranslateObject  = 1,
		SceneEditTool_RotateObject     = 2,
		SceneEditTool_ScaleObject      = 3,
		SceneEditTool_OrbitCamera      = 4,
		SceneEditTool_PanCamera        = 5,
		SceneEditTool_ZoomCamera       = 6,
		SceneEditTool_ScrubTimeline    = 7,
		SceneEditTool_RollCamera       = 8
	};

	//! Category enum for the right-side accordion.  Mirrors
	//! SceneEditController::Category.  Numeric values match
	//! SceneEditController::PanelMode so the C-API can return either
	//! as the same int.
	enum SceneEditCategory
	{
		SceneEditCategory_None       = 0,
		SceneEditCategory_Camera     = 1,
		SceneEditCategory_Rasterizer = 2,
		SceneEditCategory_Object     = 3,
		SceneEditCategory_Light      = 4
	};

	//! Construct a SceneEditController over an existing job.
	/// @param pJob                   Borrowed.  Must outlive the controller.
	///                               In practice this is the same object
	///                               returned by Job::GetScene's owner —
	///                               IJobPriv inherits IJob and Job
	///                               concretely implements IJobPriv.
	/// @param pInteractiveRasterizer Borrowed.  May be NULL for skeleton
	///                               mode.  Must outlive the controller.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSceneEditController(
		IJobPriv* pJob,
		IRasterizer* pInteractiveRasterizer,
		SceneEditController** ppOut
		);

	//! Destroy a controller.  Stops the render thread first.
	void RISE_API_DestroySceneEditController( SceneEditController* p );

	//! Start the controller's render thread.
	bool RISE_API_SceneEditController_Start( SceneEditController* p );

	//! Stop the controller's render thread.  Joins the thread.
	bool RISE_API_SceneEditController_Stop( SceneEditController* p );

	//! Install the preview image sink (typically a platform-specific
	//! IRasterizerOutput that marshals the framebuffer to the UI
	//! thread).  Set BEFORE Start().
	bool RISE_API_SceneEditController_SetPreviewSink(
		SceneEditController* p, IRasterizerOutput* sink );

	bool RISE_API_SceneEditController_SetProgressSink(
		SceneEditController* p, IProgressCallback* sink );

	bool RISE_API_SceneEditController_SetLogSink(
		SceneEditController* p, ILogPrinter* sink );

	//! Set the active toolbar mode.  Use SceneEditTool_* constants.
	bool RISE_API_SceneEditController_SetTool( SceneEditController* p, int tool );

	//! Pointer events from the platform UI.  Coordinates are in the
	//! preview surface's pixel space.
	bool RISE_API_SceneEditController_OnPointerDown(
		SceneEditController* p, Scalar x, Scalar y );
	bool RISE_API_SceneEditController_OnPointerMove(
		SceneEditController* p, Scalar x, Scalar y );
	bool RISE_API_SceneEditController_OnPointerUp(
		SceneEditController* p, Scalar x, Scalar y );

	//! Time scrubber.
	bool RISE_API_SceneEditController_OnTimeScrubBegin( SceneEditController* p );
	bool RISE_API_SceneEditController_OnTimeScrub( SceneEditController* p, Scalar t );
	bool RISE_API_SceneEditController_OnTimeScrubEnd( SceneEditController* p );

	//! Bracket a property-panel scrub gesture (chevron drag on a
	//! numeric row).  Same scale-bump machinery as the camera tools
	//! use during a drag, exposed so the panel can drive it.  Begin
	//! arms the adaptive preview-scale loop and bumps the divisor
	//! to kPreviewScaleMotionStart; End restores full resolution.
	//! Without these brackets the rapid-fire SetProperty stream
	//! cancels every in-flight render before the outer tiles get a
	//! chance, so only the center of the image updates.
	bool RISE_API_SceneEditController_BeginPropertyScrub( SceneEditController* p );
	bool RISE_API_SceneEditController_EndPropertyScrub( SceneEditController* p );

	bool RISE_API_SceneEditController_Undo( SceneEditController* p );
	bool RISE_API_SceneEditController_Redo( SceneEditController* p );

	//! Canonical scene time tracked by the controller's edit history.
	//! Updated by OnTimeScrub AND by Undo/Redo of a SetSceneTime edit
	//! — i.e. the scrubber widget's local copy goes stale after an
	//! undo/redo, but this getter returns the truth.  Platform UIs
	//! should query this just before the production-render handoff
	//! (instead of trusting their timeline-widget state) so a Render
	//! click after Undo doesn't roll the scene back to the pre-undo
	//! time.  Returns true on success; on null controller, returns
	//! false and leaves *out unchanged.
	bool RISE_API_SceneEditController_LastSceneTime(
		SceneEditController* p, double* out );

	//! Run the production rasterizer (whatever the scene declared)
	//! on the in-memory mutated scene.  Blocks until complete.
	bool RISE_API_SceneEditController_RequestProductionRender( SceneEditController* p );

	// Properties panel — descriptor-driven introspection of the
	// currently active editable entity.  The platform UI calls
	// RefreshProperties() before reading the rest, then iterates by
	// index.  Returns 0/empty on null controller.

	bool RISE_API_SceneEditController_RefreshProperties( SceneEditController* p );

	//! Returns the panel-mode discriminator the platform UI uses to
	//! decide whether the right-side panel renders a header / list.
	//! Values match SceneEditCategory_*:
	//!   0 = None
	//!   1 = Camera
	//!   2 = Rasterizer
	//!   3 = Object
	//!   4 = Light
	//! Maps onto SceneEditController::PanelMode.  -1 on null controller.
	int RISE_API_SceneEditController_PanelMode( SceneEditController* p );

	//! Copies up to bufLen-1 bytes of the panel header into buf and
	//! null-terminates.  "Camera", "Object: <name>", or empty.  Returns
	//! true on success.
	bool RISE_API_SceneEditController_PanelHeader(
		SceneEditController* p, char* buf, unsigned int bufLen );

	unsigned int RISE_API_SceneEditController_PropertyCount( SceneEditController* p );

	//! Copies up to bufLen-1 bytes of the parameter name into buf
	//! and null-terminates.  Returns true if idx is in range.
	bool RISE_API_SceneEditController_PropertyName(
		SceneEditController* p, unsigned int idx,
		char* buf, unsigned int bufLen );

	bool RISE_API_SceneEditController_PropertyValue(
		SceneEditController* p, unsigned int idx,
		char* buf, unsigned int bufLen );

	bool RISE_API_SceneEditController_PropertyDescription(
		SceneEditController* p, unsigned int idx,
		char* buf, unsigned int bufLen );

	//! Returns the parameter's ValueKind enum cast to int, or -1 on
	//! out-of-range idx.  Map at the call site to an enum the bridge
	//! can interpret.
	int RISE_API_SceneEditController_PropertyKind(
		SceneEditController* p, unsigned int idx );

	bool RISE_API_SceneEditController_PropertyEditable(
		SceneEditController* p, unsigned int idx );

	//! Quick-pick preset accessors.  PropertyPresetCount returns 0 for
	//! parameters whose descriptor declared no presets, in which case
	//! the GUI falls back to a plain line edit.  Otherwise the GUI
	//! shows a combo box with the labels and writes the corresponding
	//! parser-acceptable value through SetProperty when the user picks.
	unsigned int RISE_API_SceneEditController_PropertyPresetCount(
		SceneEditController* p, unsigned int idx );
	bool RISE_API_SceneEditController_PropertyPresetLabel(
		SceneEditController* p, unsigned int idx, unsigned int presetIdx,
		char* buf, unsigned int bufLen );
	bool RISE_API_SceneEditController_PropertyPresetValue(
		SceneEditController* p, unsigned int idx, unsigned int presetIdx,
		char* buf, unsigned int bufLen );

	//! Short unit suffix shown next to the editor field — e.g. "mm"
	//! for camera lens lengths, "°" for angles, "scene units" for
	//! focus_distance.  Empty when the descriptor declared no unit
	//! label.
	bool RISE_API_SceneEditController_PropertyUnitLabel(
		SceneEditController* p, unsigned int idx,
		char* buf, unsigned int bufLen );

	//! Apply an edited value.  Triggers a re-render via the
	//! cancel-restart loop.  Returns false on parse failure or
	//! read-only property.
	bool RISE_API_SceneEditController_SetProperty(
		SceneEditController* p, const char* name, const char* valueStr );

	//! Read the scene's animation options (start time, end time,
	//! number of frames) for sizing the interactive timeline
	//! scrubber.  Defaults are (0, 1, 30) when the .RISEscene file
	//! declared no `animation_options` chunk.  Returns false on null
	//! controller / no job attached.
	bool RISE_API_SceneEditController_GetAnimationOptions(
		SceneEditController* p,
		double* outTimeStart, double* outTimeEnd,
		unsigned int* outNumFrames );

	//! Read the scene camera's stable full-resolution dimensions.
	//! Bridges call this from their pointer-event handlers to convert
	//! window-space mouse coords to a coord space that doesn't drift
	//! with the preview-scale state machine.  See
	//! SceneEditController::GetCameraDimensions for the full
	//! rationale — short version: ICamera::GetWidth/Height flicker
	//! during subsampling, this getter doesn't.  Returns false on
	//! null controller / no camera attached / cache uninitialised.
	bool RISE_API_SceneEditController_GetCameraDimensions(
		SceneEditController* p, unsigned int* outW, unsigned int* outH );

	// Accordion list entries -----------------------------------------
	//
	// The right-side panel renders one collapsible section per
	// SceneEditCategory.  Each section's list of selectable rows is
	// pulled by polling these getters.  The platform UIs cache the
	// (epoch, category) → list mapping and refresh when SceneEpoch()
	// advances.

	//! Number of selectable entries in `category`.  Returns 0 for
	//! Category::None or on null controller.
	unsigned int RISE_API_SceneEditController_CategoryEntityCount(
		SceneEditController* p, int category );

	//! Display name for the `idx`-th entry of `category`.  Returns
	//! false on null controller, out-of-range idx, or unknown
	//! category.
	bool RISE_API_SceneEditController_CategoryEntityName(
		SceneEditController* p, int category, unsigned int idx,
		char* buf, unsigned int bufLen );

	// Selection (the accordion's "expanded section + picked row") ----

	//! Current selection's category, mapped to SceneEditCategory_*.
	//! -1 on null controller.
	int RISE_API_SceneEditController_GetSelectionCategory(
		SceneEditController* p );

	//! Display name of the currently-selected entity within the
	//! selected category.  Empty when the section is open with no row
	//! highlighted.  Returns false on null controller.
	bool RISE_API_SceneEditController_GetSelectionName(
		SceneEditController* p, char* buf, unsigned int bufLen );

	//! Apply a (category, name) selection.  Empty `name` selects the
	//! category with no row picked (the section opens, the property
	//! panel below shows nothing).  Camera / Rasterizer selections
	//! also activate the named entity (calls SetActiveCamera /
	//! SetActiveRasterizer respectively); Object / Light selections
	//! are UI state only.  Returns false on null controller or
	//! category-specific failures (e.g. unknown camera name).
	bool RISE_API_SceneEditController_SetSelection(
		SceneEditController* p, int category, const char* name );

	//! Monotonic counter — bumped on any structural mutation that
	//! could change a category's entity list.  Platform UIs cache
	//! (epoch, category) → entity-name list and re-pull when this
	//! advances.  0 on null controller.
	unsigned int RISE_API_SceneEditController_SceneEpoch(
		SceneEditController* p );
}

#endif
