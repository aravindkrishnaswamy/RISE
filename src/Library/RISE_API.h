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
#include "Interfaces/IFilm.h"
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
#include "Interfaces/IScalarPainter.h"
#include "Interfaces/IScalarPainterManager.h"
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
#include "Utilities/RasterizerDefaults.h"     // AutoIntegratorChoice (auto_rasterizer dispatcher)
#include "Utilities/ProgressiveConfig.h"      // ProgressiveConfig (auto_rasterizer factory takes it directly)
#include "Interfaces/ProceduralDescriptors.h"
#include "Painters/ExpressionEval.h"	// Implementation::ExpressionProgram (expression_function2d factory)

namespace RISE
{
	// L6a-2 — forward decl for the optional `FrameStore*` param
	// appended to every `RISE_API_Create*Rasterizer` factory.
	// Default `nullptr` keeps existing call sites compiling
	// unchanged; callers that want a non-null FrameStore (Job in
	// L6b, anyone wanting a pre-allocated canonical buffer) pass
	// it positionally.  Defined in
	// `src/Library/Rendering/FrameStore.h`.
	namespace Implementation { class FrameStore; }

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
		const Vector2& target_orientation,						///< [in] Orientation relative to a target
		const Scalar iso = Scalar( 0 ),							///< [in] Landing 5: ISO sensitivity.  Default 0 = physical exposure disabled.
		const Scalar fstop = Scalar( 0 )						///< [in] Landing 5: f-number for EV computation.  Required when iso > 0.
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
		const Scalar pixelRate,									///< [in] Rate at which each pixel is recorded
		const Scalar iso = Scalar( 0 ),							///< [in] Landing 5: ISO sensitivity.  Default 0 = physical exposure disabled.
		const Scalar fstop = Scalar( 0 )						///< [in] Landing 5: f-number for EV computation.  Required when iso > 0.
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
		const Scalar shiftY,									///< [in] Lens shift along y (mm); 0 = centered
		const Scalar iso = Scalar( 0 )							///< [in] Landing 5: ISO sensitivity.  Default 0 = physical exposure disabled.  When > 0, fstop and exposure must also be > 0.
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
						const bool          face_normals,		///< [in] Use face normals rather than topologically re-averaged vertex normals
						const bool          seam_fold = true	///< [in] Tent-fold UV before displacement (closed wrap-seam surfaces); FALSE for open Cartesian fields
						);

	//! Creates a signed-distance-field (implicit) geometry: transformed
	//! primitives (sphere / box / roundbox / cylinder / torus / capsule /
	//! roundcone) composed with smooth-min / boolean ops, ray-traced by sphere
	//! tracing.  This is the C-API construction boundary for SDF geometry;
	//! IJob::AddSDFGeometry and the scene parser both route through here.
	//! The part list comes from exactly ONE of `szParts` (inline, newline-
	//! separated part lines -- the normal authoring path) or `szFileName`
	//! (external parts file, same one-part-per-line grammar; for very large
	//! SDFs).  szFileName must already be a resolvable path (the scene layer
	//! applies the media-path search before calling).  Both sources share
	//! SDFGeometry::ParsePartLines, so the grammar cannot drift.  Fails
	//! (returns false, logs source / line / token context) on a missing file,
	//! both-or-neither sources, a malformed part line, or an unknown
	//! primitive / op token.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSDFGeometry(
						IGeometry**          ppi,				///< [out] Pointer to receive the geometry
						const char*          szFileName,		///< [in] Parts file path (already media-path resolved); NULL / "" / "none" when szParts is given
						const char*          szParts,			///< [in] Inline newline-separated part lines; NULL / "" when szFileName is given
						const unsigned int   maxSteps,			///< [in] Sphere-trace step cap (0 = default 256)
						const double         surfaceEpsilonFraction,	///< [in] Surface epsilon as a fraction of the bbox diagonal (0 = auto)
						const unsigned int   samplingDetail		///< [in] Tessellation cells (longest axis) for area-light / SSS surface sampling (clamped 8..256)
						);


	//! Creates a FLAT Cartesian-grid circular disk (linear Cartesian UV,
	//! +Z normals) -- the general flat base for displacing an arbitrary 2D
	//! field onto a disk via displaced_geometry (uv_seam_fold FALSE).
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCartesianDiskGeometry(
						ITriangleMeshGeometryIndexed** ppi,	///< [out] Pointer to receive the geometry
						const double         radius,		///< [in] Disk radius (world units)
						const int            meshN			///< [in] Grid samples across the diameter
						);

	//! Creates a general PROFILE SWEEP: an arbitrary CLOSED 2D profile
	//! polygon swept along an arbitrary 3D Catmull-Rom path with
	//! rotation-minimizing frames, optional per-axis linear taper, and
	//! ear-clipped end caps.  Tubes, rails, mouldings, bands, cables.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSweepGeometry(
						ITriangleMeshGeometryIndexed** ppi,	///< [out] Pointer to receive the geometry
						const SweepDescriptor&         desc	///< [in] Profile + path + taper + cap parameters
						);

	//! Creates ALONG-PATH INSTANCES: a template geometry (tessellated once
	//! through the universal TessellateToMesh contract) stamped along a 3D
	//! Catmull-Rom path at arc-length pitch with optional slant and scale.
	//! Fence posts, rivets, beads, stitching, chain links.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePathInstancesGeometry(
						ITriangleMeshGeometryIndexed** ppi,	///< [out] Pointer to receive the geometry
						const PathInstancesDescriptor& desc	///< [in] Template + path + pitch parameters
						);

	//! Function2DScalarPainter with the affine output out = bias + scale * f(u,v)
	//! (mirrors RISE_API_CreateTextureScalarPainterAffine for procedural sources).
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateFunction2DScalarPainterAffine(
						IScalarPainter**     ppi,			///< [out] Pointer to receive the painter
						IFunction2D*         pFunc,			///< [in] Source function (addref'd)
						const double         scale,			///< [in] Output scale
						const double         bias			///< [in] Output bias
						);

	//! Wraps a named IFunction2D as a greyscale COLOUR painter (IPainter)
	//! -- out = bias + scale * f(u,v) on all three channels.  The colour
	//! analogue of RISE_API_CreateFunction2DScalarPainterAffine; lets any
	//! procedural 2D field feed a colour slot or a blend_painter mask.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateFunction2DColorPainter(
						IPainter**           ppi,			///< [out] Pointer to receive the painter
						IFunction2D*         pFunc,			///< [in] Source function (addref'd)
						const double         scale,			///< [in] Output scale
						const double         bias			///< [in] Output bias
						);

	//! Wraps a compiled in-scene math expression as a procedural 2D field
	//! painter (colour / displacement / scalar).  The program is built by
	//! the expression_function2d chunk parser (params + defs + final expr).
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateExpressionFunction2D(
						IPainter**           ppi,			///< [out] Pointer to receive the painter
						const Implementation::ExpressionProgram& prog	///< [in] Compiled expression program
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

	//! Creates a Polished material.  Scalar params (tau, IOR, scattering)
	//! are physical scalars carried by `IScalarPainter` — see
	//! docs/ISCALARPAINTER_REFACTOR.md.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePolishedMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& ref,			///< [in] Reflectance of diffuse substrate
								const IScalarPainter& tau,		///< [in] Transmittance of the dielectric top
								const IScalarPainter& Nt,		///< [in] Index of refraction of dielectric coating
								const IScalarPainter& scat,		///< [in] Scattering function (Phong cone or HG asymmetry)
								const bool hg					///< [in] Use Henyey-Greenstein phase function scattering
								);

	//! Creates a Dielectric material.  Scalar params (tau, IOR, scattering)
	//! are physical scalars carried by `IScalarPainter` — see
	//! docs/ISCALARPAINTER_REFACTOR.md.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDielectricMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IScalarPainter& tau,		///< [in] Transmittance (per-channel + spectral)
								const IScalarPainter& rIndex,	///< [in] Index of refraction
								const IScalarPainter& scat,		///< [in] Scattering function (Phong cone or HG)
								const bool hg,					///< [in] Use Henyey-Greenstein phase function scattering
								const Scalar arN = 0,			///< [in] AR coating film real index (0 = no coating)
								const Scalar arK = 0,			///< [in] AR coating film extinction (~0)
								const Scalar arThickness = 0	///< [in] AR coating thickness, nm (0 = no coating)
								);

	//! Creates a SubSurface Scattering material.  ior / absorption /
	//! scattering are physical scalars carried by `IScalarPainter`
	//! — see docs/ISCALARPAINTER_REFACTOR.md.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSubSurfaceScatteringMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IScalarPainter& ior,		///< [in] Index of refraction
								const IScalarPainter& absorption,///< [in] Absorption coefficient
								const IScalarPainter& scattering,///< [in] Scattering coefficient
								const Scalar g,					///< [in] HG asymmetry parameter
								const Scalar roughness			///< [in] Surface roughness [0,1]
								);

	//! Creates a Random Walk SSS material.  ior / absorption /
	//! scattering are physical scalars carried by `IScalarPainter`.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateRandomWalkSSSMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IScalarPainter& ior,		///< [in] Index of refraction
								const IScalarPainter& absorption,///< [in] Absorption coefficient
								const IScalarPainter& scattering,///< [in] Scattering coefficient
								const Scalar g,					///< [in] HG asymmetry parameter
								const Scalar roughness,			///< [in] Surface roughness [0,1]
								const unsigned int maxBounces	///< [in] Maximum walk steps
								);

	//! Creates an isotropic phong material.
	//! `exponent` is a physical scalar carried by `IScalarPainter` — see
	//! docs/ISCALARPAINTER_REFACTOR.md.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateIsotropicPhongMaterial(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& rd,					///< [in] Diffuse reflectance
								const IPainter& rs,					///< [in] Specular reflectance
								const IScalarPainter& exponent		///< [in] Phong exponent (physical scalar)
								);

	//! Creates the anisotropic phong material of Ashikmin and Shirley.
	//! `Nu` / `Nv` are physical scalars carried by `IScalarPainter` — see
	//! docs/ISCALARPAINTER_REFACTOR.md.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAshikminShirleyAnisotropicPhongMaterial(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& rd,					///< [in] Diffuse reflectance
								const IPainter& rs,					///< [in] Specular reflectance
								const IScalarPainter& Nu,			///< [in] Phong exponent in U (physical scalar)
								const IScalarPainter& Nv			///< [in] Phong exponent in V (physical scalar)
								);

	//! Creates a perfect reflector
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePerfectReflectorMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& ref				///< [in] Reflectance
								);

	//! Creates a perfect refractor
	//! `ior` is a physical scalar carried by `IScalarPainter` — see
	//! docs/ISCALARPAINTER_REFACTOR.md.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePerfectRefractorMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& ref,			///< [in] Amount of refraction
								const IScalarPainter& ior		///< [in] Index of refraction
								);

	//! Creates a translucent material.  Scalar params (extinction,
	//! Phong exponent, multiple-scattering) are physical scalars
	//! carried by `IScalarPainter` — see docs/ISCALARPAINTER_REFACTOR.md.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTranslucentMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& rF,				///< [in] Reflectance (color)
								const IPainter& T,				///< [in] Transmittance (color)
								const IScalarPainter& ext,		///< [in] Extinction (scalar)
								const IScalarPainter& N,		///< [in] Phong exponent (scalar)
								const IScalarPainter& scat		///< [in] Multiple scattering component (scalar)
								);

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
								);

	//! Creates a Donner & Jensen 2008 spectral skin BSSRDF material (BDPT-compatible)
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateDonnerJensenSkinBSSRDFMaterial(
								IMaterial** ppi,											///< [out] Pointer to recieve the material
								const IScalarPainter& melanin_fraction_,							///< C_m: melanin volume fraction (0-0.5)
								const IScalarPainter& melanin_blend_,								///< beta_m: eumelanin vs pheomelanin blend (0-1)
								const IScalarPainter& hemoglobin_epidermis_,						///< C_he: hemoglobin fraction in epidermis (0-0.05)
								const IScalarPainter& carotene_fraction_,							///< C_bc: carotene fraction (0-0.05)
								const IScalarPainter& hemoglobin_dermis_,							///< C_hd: hemoglobin fraction in dermis (0-0.1)
								const IScalarPainter& epidermis_thickness_,						///< Epidermis thickness in cm (default 0.025)
								const IScalarPainter& ior_epidermis_,								///< Index of refraction of epidermis (default 1.4)
								const IScalarPainter& ior_dermis_,								///< Index of refraction of dermis (default 1.38)
								const IScalarPainter& blood_oxygenation_,							///< Blood oxygenation ratio (default 0.7)
								const Scalar roughness										///< Surface roughness for microfacet boundary [0, 1]
								);

	//! Creates a generic human tissue material.  `sca` and `g` are
	//! physical scalars carried by `IScalarPainter`.
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

	//! Creates Ward's isotropic gaussian material.
	//! `alpha` is a physical scalar carried by `IScalarPainter` — see
	//! docs/ISCALARPAINTER_REFACTOR.md.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateWardIsotropicGaussianMaterial(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& diffuse,			///< [in] Diffuse reflectance
								const IPainter& specular,			///< [in] Specular reflectance
								const IScalarPainter& alpha			///< [in] Surface slope RMS (physical scalar)
								);

	//! Creates Ward's anisotropic elliptical gaussian material.
	//! `alphax` / `alphay` are physical scalars carried by `IScalarPainter` —
	//! see docs/ISCALARPAINTER_REFACTOR.md.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateWardAnisotropicEllipticalGaussianMaterial(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& diffuse,			///< [in] Diffuse reflectance
								const IPainter& specular,			///< [in] Specular reflectance
								const IScalarPainter& alphax,		///< [in] Surface slope RMS in x (physical scalar)
								const IScalarPainter& alphay		///< [in] Surface slope RMS in y (physical scalar)
								);

	//! Creates a GGX anisotropic microfacet material.
	//! `alphaX` / `alphaY` (roughness), `ior`, and `ext` are physical scalars
	//! carried by `IScalarPainter` — see docs/ISCALARPAINTER_REFACTOR.md.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGGXMaterial(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& diffuse,			///< [in] Diffuse reflectance
								const IPainter& specular,			///< [in] Specular reflectance / F0
								const IScalarPainter& alphaX,		///< [in] Roughness in tangent u direction (physical scalar)
								const IScalarPainter& alphaY,		///< [in] Roughness in tangent v direction (physical scalar)
								const IScalarPainter& ior,			///< [in] Index of refraction (physical scalar; ignored in Schlick mode)
								const IScalarPainter& ext,			///< [in] Extinction coefficient (physical scalar; ignored in Schlick mode)
								const FresnelMode fresnel_mode = eFresnelConductor,	///< [in] Fresnel evaluation model
								const IPainter* tangent_rotation = nullptr			///< [in] Landing 8 / KHR_materials_anisotropy: optional painter giving tangent-frame rotation in radians.  NULL = no rotation (default; bit-identical to pre-L8).
								);

	//! Creates a GGX anisotropic microfacet material WITH the thin-film
	//! interference slots (eFresnelThinFilmConductor).  This is the
	//! ABI-preserving evolution of RISE_API_CreateGGXMaterial: that older
	//! exported symbol is kept byte-for-byte (it now delegates here with
	//! NULL film painters) so out-of-tree callers compiled against the old
	//! header keep linking.  New callers that want thin-film use THIS
	//! function.  The three film slots are IScalarPainter (physical scalar,
	//! NO JH spectral uplift — see docs/ISCALARPAINTER_REFACTOR.md) and carry
	//! the oxide FILM (the substrate metal stays on `ior`/`ext`).  They MUST
	//! be non-NULL when `fresnel_mode == eFresnelThinFilmConductor`; the
	//! parser/Job layer enforces presence before reaching here.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGGXMaterialThinFilm(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& diffuse,			///< [in] Diffuse reflectance
								const IPainter& specular,			///< [in] Specular reflectance / F0 / thin-film tint
								const IScalarPainter& alphaX,		///< [in] Roughness in tangent u direction (physical scalar)
								const IScalarPainter& alphaY,		///< [in] Roughness in tangent v direction (physical scalar)
								const IScalarPainter& ior,			///< [in] Substrate IOR (physical scalar)
								const IScalarPainter& ext,			///< [in] Substrate extinction (physical scalar)
								const FresnelMode fresnel_mode = eFresnelConductor,	///< [in] Fresnel evaluation model
								const IPainter* tangent_rotation = nullptr,			///< [in] Landing 8 / KHR_materials_anisotropy
								const IScalarPainter* film_ior = nullptr,			///< [in] Thin-film oxide n (physical scalar); NULL = no film
								const IScalarPainter* film_extinction = nullptr,	///< [in] Thin-film oxide k (physical scalar); NULL = transparent film
								const IScalarPainter* film_thickness = nullptr		///< [in] Thin-film oxide thickness in nm (physical scalar; may be spatially varying)
								);

	//! Creates a GGX material with an optional emissive painter.  Pass
	//! emissive=NULL to skip the emitter (equivalent to RISE_API_CreateGGXMaterial).
	//! Same scalar-slot conventions as RISE_API_CreateGGXMaterial.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGGXEmissiveMaterial(
								IMaterial** ppi,
								const IPainter& diffuse,
								const IPainter& specular,
								const IScalarPainter& alphaX,
								const IScalarPainter& alphaY,
								const IScalarPainter& ior,
								const IScalarPainter& ext,
								const IPainter* emissive,		///< [in] Optional; NULL = no emitter
								const Scalar    emissive_scale,
								const FresnelMode fresnel_mode = eFresnelConductor,	///< [in] Fresnel evaluation model
								const IPainter* tangent_rotation = nullptr			///< [in] Landing 8 / KHR_materials_anisotropy.  See RISE_API_CreateGGXMaterial.
								);

	//! Thin-film-aware sibling of RISE_API_CreateGGXEmissiveMaterial (the
	//! emissive + thin-film combination).  ABI-preserving: the old symbol is
	//! retained and delegates here with NULL film painters.  See
	//! RISE_API_CreateGGXMaterialThinFilm for the film-slot conventions.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateGGXEmissiveMaterialThinFilm(
								IMaterial** ppi,
								const IPainter& diffuse,
								const IPainter& specular,
								const IScalarPainter& alphaX,
								const IScalarPainter& alphaY,
								const IScalarPainter& ior,
								const IScalarPainter& ext,
								const IPainter* emissive,		///< [in] Optional; NULL = no emitter
								const Scalar    emissive_scale,
								const FresnelMode fresnel_mode = eFresnelConductor,	///< [in] Fresnel evaluation model
								const IPainter* tangent_rotation = nullptr,			///< [in] Landing 8 / KHR_materials_anisotropy
								const IScalarPainter* film_ior = nullptr,			///< [in] Thin-film oxide n; NULL = no film
								const IScalarPainter* film_extinction = nullptr,	///< [in] Thin-film oxide k; NULL = transparent film
								const IScalarPainter* film_thickness = nullptr		///< [in] Thin-film oxide thickness in nm
								);

	//! Creates a glTF-spec pbrMetallicRoughness material.  Composes the
	//! same painter graph as IJob::AddPBRMetallicRoughnessMaterial but
	//! WITHOUT the Job-level painter manager — all internal helper
	//! painters live as members of the returned material's BSDF/SPF
	//! refcount chain (BlendPainter / UniformColorPainter constructors
	//! addref their inputs; GGXMaterial addrefs the chain head).  The
	//! caller is therefore responsible only for the lifetime of THEIR
	//! input painters and the returned IMaterial — internal helpers
	//! are reclaimed automatically when the material is released.
	//!
	//! Defaults preserve every pre-L7 / pre-L8 PBR-MR semantic:
	//!   - specular_factor = NULL → 1.0 (standard 0.04 dielectric F0)
	//!   - specular_color  = NULL → white (untinted)
	//!   - anisotropy_factor = NULL → 0 (isotropic; αx = αy = roughness²)
	//!   - anisotropy_rotation = NULL → 0 (aligned with surface tangent)
	//!
	//! Embedders that want the new L7 / L8 controls supply non-NULL
	//! painters for the corresponding parameter; the resulting material
	//! exhibits KHR_materials_specular and / or KHR_materials_anisotropy
	//! behaviour.  See docs/PHYSICALLY_BASED_PIPELINE_PLAN.md Landings
	//! 7 + 8 for the formulas.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePBRMetallicRoughnessMaterial(
								IMaterial** ppi,					///< [out] Pointer to receive the material
								const IPainter& base_color,			///< [in] baseColor painter (sRGB-decoded RGB)
								const IPainter& metallic,			///< [in] Metallic painter (scalar in [0, 1])
								const IPainter& roughness,			///< [in] Roughness painter (scalar in [0, 1])
								const IPainter* emissive,			///< [in] Optional emissive painter; NULL = no emitter
								const Scalar    emissive_scale,		///< [in] Multiplier on emissive radiance
								const IPainter* specular_factor = nullptr,		///< [in] Landing 7 / KHR_materials_specular.  NULL = 1.0 (default 0.04 dielectric F0).
								const IPainter* specular_color = nullptr,		///< [in] Landing 7 / KHR_materials_specular.  NULL = white (untinted).  Final dielectric F0 = 0.04 × specular_color × specular_factor.
								const IPainter* anisotropy_factor = nullptr,	///< [in] Landing 8 / KHR_materials_anisotropy.  NULL = 0 (isotropic).
								const IPainter* anisotropy_rotation = nullptr	///< [in] Landing 8 / KHR_materials_anisotropy.  NULL = no rotation.
								);

	//! Creates a Charlie / Neubelt sheen material for fabric / cloth.
	//! Designed as the top layer in a CompositeMaterial(top=sheen,
	//! bottom=baseGGX) pairing for glTF KHR_materials_sheen assets.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSheenMaterial(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& sheenColor,			///< [in] Sheen tint colour (typical 0..1)
								const IScalarPainter& sheenRoughness	///< [in] Sheen roughness (physical scalar, typical 0..1; clamped to >= 1e-3 internally)
								);

	//! Creates a Cook Torrance material.
	//! `facet` (roughness), `ior`, and `ext` are physical scalars carried by
	//! `IScalarPainter` — see docs/ISCALARPAINTER_REFACTOR.md.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCookTorranceMaterial(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& diffuse,			///< [in] Diffuse reflectance
								const IPainter& specular,			///< [in] Specular reflectance
								const IScalarPainter& facet,		///< [in] Facet distribution (roughness — physical scalar)
								const IScalarPainter& ior,			///< [in] Index of refraction (physical scalar)
								const IScalarPainter& ext			///< [in] Extinction coefficient (physical scalar)
								);

	//! Creates a Oren-Nayar material.
	//! `roughness` is a physical scalar carried by `IScalarPainter` — see
	//! docs/ISCALARPAINTER_REFACTOR.md.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateOrenNayarMaterial(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& reflectance,		///< [in] Reflectance
								const IScalarPainter& roughness		///< [in] Roughness (physical scalar)
								);

	//! Creates a Schlick material.
	//! `roughness` and `isotropy` are physical scalars carried by
	//! `IScalarPainter` — see docs/ISCALARPAINTER_REFACTOR.md.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateSchlickMaterial(
								IMaterial** ppi,					///< [out] Pointer to recieve the material
								const IPainter& diffuse,			///< [in] Diffuse reflectance
								const IPainter& specular,			///< [in] Specular reflectance
								const IScalarPainter& roughness,	///< [in] Roughness (physical scalar)
								const IScalarPainter& isotropy		///< [in] Isotropy (physical scalar)
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

	//! Creates a phong luminaire material.  `N` is a physical scalar
	//! carried by `IScalarPainter` (Phong exponent).
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePhongLuminaireMaterial(
								IMaterial** ppi,				///< [out] Pointer to recieve the material
								const IPainter& radEx,			///< [in] Radiance exitance (color)
								const IMaterial& mat,			///< [in] Material to use for all non emmission properties
								const IScalarPainter& N,		///< [in] Phong exponent (scalar)
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

	//! Creates a controlled-smoothness radial-bump painter (test/diagnostic).
	//! Implements both IPainter and IFunction2D.  smoothnessMode: 0=Heaviside,
	//! 1=Tent, 2=Quadratic, 3=Cubic, 5=Quintic, 99=Gaussian.  See
	//! `Painters/ControlledSmoothness2DPainter.h` for details.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateControlledSmoothness2DPainter(
								IPainter** ppi,					///< [out] Pointer to receive the painter
								const IPainter& cA,				///< [in] Low/zero-end color painter
								const IPainter& cB,				///< [in] High/peak-end color painter
								const Scalar centerU,			///< [in] Bump center, U axis (UV space)
								const Scalar centerV,			///< [in] Bump center, V axis (UV space)
								const Scalar radius,			///< [in] Bump radius (UV space)
								const Scalar amplitude,			///< [in] Peak height
								const unsigned int smoothnessMode	///< [in] 0/1/2/3/5/99
								);

	//! Creates a polynomial-based Function2D painter.  Evaluates a
	//! polynomial in normalised coords (x,y) = ((u−center.u)/scale.u,
	//! (v−center.v)/scale.v).  `polynomialType`:
	//!   0 = radial_bump        (amplitude × max(0, 1 − r²)^degree)
	//!   1 = monomial           (amplitude × x^powerX × y^powerY)
	//!   2 = paraboloid         (amplitude × (x² + y²))
	//!   3 = hyperbolic_saddle  (amplitude × (x² − y²))
	//!   4 = monkey_saddle      (amplitude × (x³ − 3·x·y²))
	//!   5 = bivariate          (amplitude × Σ a_{ij} x^i y^j, i+j ≤ degree)
	//! For bivariate, pCoeffs supplies the (degree+1)(degree+2)/2
	//! coefficients in row-major triangular order — see
	//! `Painters/PolynomialFunction2DPainter.h` for the exact
	//! ordering.  Pass nullptr / 0 for non-bivariate types.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreatePolynomialFunction2DPainter(
								IPainter** ppi,					///< [out] Pointer to receive the painter
								const IPainter& cA,				///< [in] Zero/low-end color painter
								const IPainter& cB,				///< [in] Positive/peak-end color painter
								const unsigned int polynomialType,	///< [in] Type enum (0..5)
								const Scalar centerU,			///< [in] U origin for normalised x
								const Scalar centerV,			///< [in] V origin for normalised y
								const Scalar scaleU,			///< [in] U divisor (x = (u − cu)/sU)
								const Scalar scaleV,			///< [in] V divisor
								const Scalar amplitude,			///< [in] Global multiplier
								const unsigned int degree,		///< [in] Degree (radial_bump / bivariate)
								const unsigned int powerX,		///< [in] x exponent (monomial only)
								const unsigned int powerY,		///< [in] y exponent (monomial only)
								const Scalar* pCoeffs,			///< [in] Bivariate coefficients (or nullptr)
								const unsigned int nCoeffs		///< [in] Number of bivariate coefficients (or 0)
								);

	//! Creates a composable Function2D painter.  Combines two operand
	//! Function2Ds per a chosen binary operator (sum/product/lerp/max/min/
	//! difference), after applying per-operand weight and (u,v) affine
	//! transform, then a global output remap.  Implements both IPainter
	//! (color interp between cA/cB by composite value) and IFunction2D
	//! (the scalar combination — driveable by DisplacedGeometry).
	//! Operator integer mapping: 0=Sum, 1=Product, 2=Lerp, 3=Max, 4=Min,
	//! 5=Difference.  Unknown values fall back to Sum with a warning.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCompositeFunction2DPainter(
								IPainter** ppi,					///< [out] Pointer to receive the painter
								const IPainter& cA,				///< [in] Low-value color painter
								const IPainter& cB,				///< [in] High-value color painter
								const IFunction2D& childA,		///< [in] First operand Function2D
								const IFunction2D& childB,		///< [in] Second operand Function2D
								const unsigned int op,			///< [in] Operator (0/1/2/3/4/5)
								const Scalar weightA,			///< [in] Scalar multiplier on A
								const Scalar uvScaleAU,			///< [in] U scale applied to (u,v) before sampling A
								const Scalar uvScaleAV,			///< [in] V scale applied to (u,v) before sampling A
								const Scalar uvOffsetAU,		///< [in] U offset applied to (u,v) before sampling A
								const Scalar uvOffsetAV,		///< [in] V offset applied to (u,v) before sampling A
								const Scalar weightB,			///< [in] Scalar multiplier on B
								const Scalar uvScaleBU,			///< [in] U scale applied to (u,v) before sampling B
								const Scalar uvScaleBV,			///< [in] V scale applied to (u,v) before sampling B
								const Scalar uvOffsetBU,		///< [in] U offset applied to (u,v) before sampling B
								const Scalar uvOffsetBV,		///< [in] V offset applied to (u,v) before sampling B
								const Scalar lerpT,				///< [in] Lerp parameter (clamped to [0,1]; only used if op == Lerp)
								const Scalar outputScale,		///< [in] Final-stage scalar multiplier
								const Scalar outputOffset		///< [in] Final-stage scalar offset
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

	//! Creates a texture painter.  The optional `kind` selects the
	//! spectral-uplift role applied at sample time (see
	//! RISE_API_CreateUniformColorPainter for the role semantics).
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTexturePainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								IRasterImageAccessor* pSA,		///< [in] Raster Image accessor to the image containing the texture
								SpectrumKind kind = eSpectrumKind_Albedo	///< [in] Spectral-uplift role
								);

	//! Creates a painter that paints a uniform color.  The optional
	//! `kind` selects the spectral-uplift role: Albedo (default; rgb
	//! ∈ [0,1] reflectance) / Unbounded (rgb ≥ 0; for HDR / emissive)
	//! / Illuminant (reference-SPD-pre-multiplied; D65 post Stage B colour-space migration, for light SPDs).
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateUniformColorPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const RISEPel& c,				///< [in] Color to paint
								SpectrumKind kind = eSpectrumKind_Albedo	///< [in] Spectral-uplift role
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

	//! Creates a TEXCOORD_1 selector painter (glTF L12.D).  Wraps the
	//! source painter and routes its UV reads through ri.ptCoord1
	//! instead of ri.ptCoord.  When the geometry has no TEXCOORD_1,
	//! the intersection code mirrors ptCoord into ptCoord1, so the
	//! wrapper degrades to a passthrough.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTexCoord1Painter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const IPainter& source			///< [in] Source painter
								);

	//! Creates a UV-transform wrapper painter (glTF KHR_texture_transform).
	//! Applies T * R * S to the source's (u, v) before sampling, with the
	//! KHR sign convention (positive `rotation` rotates the IMAGE clockwise).
	//! Defaults (0/0/0/1/1) collapse to a passthrough.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateUVTransformPainter(
								IPainter** ppi,					///< [out] Pointer to recieve the painter
								const IPainter& source,			///< [in] Source painter
								const Scalar offset_u,			///< [in] U translation
								const Scalar offset_v,			///< [in] V translation
								const Scalar rotation,			///< [in] Rotation in radians (KHR sign)
								const Scalar scale_u,			///< [in] U scale
								const Scalar scale_v			///< [in] V scale
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

	//! Creates an analytic Hosek-Wilkie spectral sun-and-sky radiance
	//! map (Landing 3.D).  v1 internally uses Preetham 1999 Perez form
	//! (HW supplemental coefficients are a future landing); the public
	//! API stays stable for the v2 swap.
	//!
	//! Solar elevation is in degrees from the horizon (0 = horizon,
	//! 90 = zenith).  Azimuth in degrees, 0 = +Z, 90 = +X.  Turbidity
	//! ∈ [1, 10] (1 = arctic clear, ~3 = typical clear day, 10 =
	//! polluted).  Ground albedo per-channel ∈ [0, 1].  The
	//! `skyIntensityScale` is a scene-level multiplier on the sky
	//! radiance only — it does NOT affect the matched directional
	//! light's intensity (which is created separately).
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateHosekWilkieRadianceMap(
								IRadianceMap** ppi,				///< [out] Pointer to receive the radiance map
								const Scalar solarElevationDegrees,
								const Scalar solarAzimuthDegrees,
								const Scalar turbidity,
								const RISEPel& groundAlbedo,
								const Scalar skyIntensityScale
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

	//! Creates a scalar-painter manager (see IScalarPainter.h).
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateScalarPainterManager(
								IScalarPainterManager** ppi		///< [out] Pointer to recieve the manager
								);

	// =========================================================================
	// Scalar painters (physical-scalar values; no colorspace).  Each
	// painter implements `IScalarPainter` (see IScalarPainter.h).  Used
	// for material parameters like IOR, scattering, roughness, etc.
	// =========================================================================

	//! Constant scalar value at every position and wavelength.
	bool RISE_API_CreateUniformScalarPainter(
								IScalarPainter** ppi,			///< [out] Created painter
								Scalar value					///< [in]  Scalar value
								);

	//! Per-channel scalar triple (R, G, B authored independently).  In
	//! the spectral path interpolates piecewise-linearly across the
	//! channel nominal wavelengths (450/550/650 nm).
	bool RISE_API_CreateRGBScalarPainter(
								IScalarPainter** ppi,
								Scalar r,
								Scalar g,
								Scalar b
								);

	//! Piecewise-linear interpolation across (nm, value) samples.
	//! `samples` is a vector of (wavelength, scalar) pairs; the
	//! constructor sorts and clamps outside the sample range.
	bool RISE_API_CreatePiecewiseLinearScalarPainter(
								IScalarPainter** ppi,
								const std::vector<std::pair<Scalar, Scalar>>& samples
								);

	//! Analytic Sellmeier IOR formula:
	//!   n²(λ) = 1 + Σᵢ Bᵢ·λ² / (λ² - Cᵢ),  λ in micrometres.
	bool RISE_API_CreateSellmeierScalarPainter(
								IScalarPainter** ppi,
								Scalar B1, Scalar B2, Scalar B3,
								Scalar C1, Scalar C2, Scalar C3
								);

	//! Polynomial function of wavelength:
	//!   f(λ) = c₀ + c₁·λ + … + cₙ·λⁿ
	//! Horner-form evaluation.
	bool RISE_API_CreatePolynomialScalarPainter(
								IScalarPainter** ppi,
								const std::vector<Scalar>& coeffs
								);

	//! Wraps an existing IFunction1D as a wavelength-varying scalar
	//! painter.  Lets authors reuse RISE's existing function-1d
	//! infrastructure (piecewise-linear, polynomial, custom C++
	//! subclasses) as a scalar painter.
	bool RISE_API_CreateFunction1DScalarPainter(
								IScalarPainter** ppi,
								IFunction1D* pFunc				///< [in] Wavelength-driven function (addref'd)
								);

	//! Wraps an existing IFunction2D as a spatially-varying scalar
	//! painter.  Evaluates at `(ri.ptCoord.x, ri.ptCoord.y)`.
	bool RISE_API_CreateFunction2DScalarPainter(
								IScalarPainter** ppi,
								IFunction2D* pFunc				///< [in] Spatial function (addref'd)
								);

	//! Texture-driven scalar painter.  Samples the raster texture at
	//! `ri.ptCoord` and returns the chosen channel value as a pure
	//! scalar — no JH-uplift, no colorspace conversion.
	bool RISE_API_CreateTextureScalarPainter(
								IScalarPainter** ppi,
								IRasterImageAccessor* pRIA,		///< [in] Texture accessor (addref'd)
								unsigned int channel			///< [in] 0=R, 1=G, 2=B
								);

	//! Texture-driven scalar painter with an affine remap of the
	//! sampled channel: `out = bias + scale * rawTexel` (rawTexel in
	//! [0,1]).  Lets a [0,1] greyscale map drive a physical quantity
	//! in real units (e.g. an oxide-thickness map `scale 220 bias 30`
	//! -> 30..250 nm) without a separate scaling painter in the chain.
	//! Like the un-remapped variant: NO JH-uplift, NO colourspace
	//! conversion.  Distinct symbol from
	//! RISE_API_CreateTextureScalarPainter so the frozen 3-arg ABI is
	//! preserved for out-of-tree callers.
	bool RISE_API_CreateTextureScalarPainterAffine(
								IScalarPainter** ppi,
								IRasterImageAccessor* pRIA,		///< [in] Texture accessor (addref'd)
								unsigned int channel,			///< [in] 0=R, 1=G, 2=B
								Scalar scale,					///< [in] multiplier on the raw texel
								Scalar bias						///< [in] additive offset
								);

	//! Composition: child scalar painter × constant.
	bool RISE_API_CreateScaledScalarPainter(
								IScalarPainter** ppi,
								IScalarPainter* pChild,			///< [in] Base painter (addref'd)
								Scalar scale
								);

	//! Composition: element-wise product of two scalar painters
	//! (`ScalarTriple` and per-wavelength).  Used for spatial × spectral
	//! and similar combinations.
	bool RISE_API_CreateMultiplyScalarPainter(
								IScalarPainter** ppi,
								IScalarPainter* pA,				///< [in] Painter A (addref'd)
								IScalarPainter* pB				///< [in] Painter B (addref'd)
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
	// Film
	//////////////////////////////////////////////////////////

	//! Creates a Film — the pixel-grid descriptor that the
	//! rasterizer reads to know what resolution and pixel aspect
	//! ratio to produce.  A Scene has one active Film at a time;
	//! the default (qHD 960x540, square pixels) is installed by
	//! Job::InitializeContainers and can be overridden by a
	//! `film` chunk in the scene file or by a CLI flag.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateFilm(
								IFilm** ppi,					///< [out] Pointer to receive the film object
								const unsigned int width,		///< [in] Image width in pixels
								const unsigned int height,		///< [in] Image height in pixels
								const Scalar pixelAR			///< [in] Pixel aspect ratio (1.0 = square pixels)
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
								const char wrap_t = 0,				///< [in] Wrap mode for V axis
								const bool mipmap = true,			///< [in] Build a mip pyramid + LOD-aware sampling (Landing 2; default ON for new code, OFF for vector-quantity normal maps — set explicitly false in those cases)
								const bool supersample = false		///< [in] Footprint stochastic supersampling fallback for lowmem mode (no pyramid; jitter at base).  When mipmap is also true, mipmap wins.
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

	//! Creates a EXR writer, choosing the channel pixel type.
	//! Overload of the 5-arg form (which stays half/FP16 for ABI
	//! stability).  `write_float == true` emits 32-bit FLOAT channels so
	//! bright HDR pixels above the FP16 max (65504) survive instead of
	//! being clamped to +Inf on write.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateEXRWriter(
								IRasterImageWriter** ppi,			///< [out] Pointer to recieve the raster image writer
								IWriteBuffer& buffer,				///< [in] Buffer to write to
								const COLOR_SPACE color_space,		///< [in] Color space to apply
								const EXR_COMPRESSION compression,	///< [in] EXR compression mode (PIZ default)
								const bool with_alpha,				///< [in] Write alpha channel (true default)
								const bool write_float				///< [in] true => 32-bit FLOAT channels; false => half (FP16)
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
								const bool useZSobol,				///< [in] Use Morton-indexed Sobol (blue-noise error distribution)
								Implementation::FrameStore* frameStore = nullptr    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
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
								const bool useHWSS,				///< [in] Use Hero Wavelength Spectral Sampling
								Implementation::FrameStore* frameStore = nullptr    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
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
								const bool oidnDenoise,				///< [in] Enable OIDN denoising post-process
								const OidnQuality oidnQuality,		///< [in] OIDN quality preset (Auto = render-time heuristic)
							const OidnDevice oidnDevice,		///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
							const OidnPrefilter oidnPrefilter,	///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
								const PathGuidingConfig& guidingConfig,	///< [in] Path guiding configuration
								const AdaptiveSamplingConfig& adaptiveConfig,	///< [in] Adaptive sampling configuration
								const StabilityConfig& stabilityConfig,	///< [in] Production stability controls
								const bool useZSobol,				///< [in] Use Morton-indexed Sobol (blue-noise error distribution)
								Implementation::FrameStore* frameStore = nullptr    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
								);

	//! Creates a spectral BDPT rasterizer (legacy — no adaptive sampling).
	/// Kept for ABI compatibility.  New callers should prefer
	/// RISE_API_CreateBDPTSpectralRasterizerAdaptive.  This thin wrapper
	/// delegates with a default-constructed AdaptiveSamplingConfig
	/// (adaptive disabled).
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
								const bool oidnDenoise,				///< [in] Enable OIDN denoising post-process
								const OidnQuality oidnQuality,		///< [in] OIDN quality preset (Auto = render-time heuristic)
							const OidnDevice oidnDevice,		///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
							const OidnPrefilter oidnPrefilter,	///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
								const PathGuidingConfig& guidingConfig,	///< [in] Path guiding configuration
								const StabilityConfig& stabilityConfig,	///< [in] Production stability controls
								const bool useZSobol,				///< [in] Use Morton-indexed Sobol (blue-noise error distribution)
								const bool useHWSS,					///< [in] Use Hero Wavelength Spectral Sampling
								Implementation::FrameStore* frameStore = nullptr    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
								);

	//! Creates a spectral BDPT rasterizer with adaptive sampling support.
	/// Spectral counterpart of RISE_API_CreateBDPTPelRasterizer's
	/// `adaptiveConfig` parameter.  Convergence is driven by the
	/// Welford-tracked Y channel of the XYZ accumulator (CIE photometric
	/// luminance).  Pass AdaptiveSamplingConfig() (maxSamples == 0) to
	/// disable adaptive termination — that's exactly what the legacy
	/// overload above does internally.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBDPTSpectralRasterizerAdaptive(
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
								const bool oidnDenoise,				///< [in] Enable OIDN denoising post-process
								const OidnQuality oidnQuality,		///< [in] OIDN quality preset (Auto = render-time heuristic)
							const OidnDevice oidnDevice,		///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
							const OidnPrefilter oidnPrefilter,	///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
								const PathGuidingConfig& guidingConfig,	///< [in] Path guiding configuration
								const AdaptiveSamplingConfig& adaptiveConfig,	///< [in] Adaptive sampling configuration
								const StabilityConfig& stabilityConfig,	///< [in] Production stability controls
								const bool useZSobol,				///< [in] Use Morton-indexed Sobol (blue-noise error distribution)
								const bool useHWSS,					///< [in] Use Hero Wavelength Spectral Sampling
								Implementation::FrameStore* frameStore = nullptr    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
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
								const bool useZSobol,				///< [in] Use Morton-indexed Sobol (blue-noise error distribution)
								Implementation::FrameStore* frameStore = nullptr    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
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
								const bool useHWSS,					///< [in] Use Hero Wavelength Spectral Sampling
								Implementation::FrameStore* frameStore = nullptr    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
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
								const unsigned int smsTargetBounces,	///< [in] REQUIRED specular-vertex count per seed chain (Mitsuba `m_config.bounces` analogue).  0 = no target.  Set to natural caustic K (typically 2 for glass shells / interior lights).  Active in BOTH snell and uniform modes; recommended for uniform mode.
								const bool oidnDenoise,				///< [in] Enable OIDN denoising post-process
								const OidnQuality oidnQuality,		///< [in] OIDN quality preset (Auto = render-time heuristic)
							const OidnDevice oidnDevice,		///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
							const OidnPrefilter oidnPrefilter,	///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
								const PathGuidingConfig& guidingConfig,	///< [in] Path guiding configuration
								const AdaptiveSamplingConfig& adaptiveConfig,	///< [in] Adaptive sampling configuration
								const StabilityConfig& stabilityConfig,	///< [in] Production stability controls
								const bool useZSobol,				///< [in] Use Morton-indexed Sobol (blue-noise error distribution)
								Implementation::FrameStore* frameStore = nullptr    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
								);

	//! Creates an auto-routing integrator dispatcher (the `auto_rasterizer`
	//! shell — Phase 1 of docs/AUTO_RASTERIZER_DESIGN.md).  It owns the
	//! shared build inputs and, at the first render-time entry, selects one
	//! of PT / BDPT / VCM and delegates to a concrete rasterizer built with
	//! that integrator's canonical defaults.  No integrator algorithm of its
	//! own.  Phase-1 selection is Tier-0 only (author pin, else PT).
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAutoRasterizer(
								IRasterizer** ppi,					///< [out] Pointer to receive the rasterizer
								IRayCaster* caster,					///< [in] Ray caster to use for rays
								ISampling2D* pSamples,				///< [in] Sampler for subsamples
								IPixelFilter* pFilter,				///< [in] Pixel Filter for samples
								const AutoIntegratorChoice integrator,	///< [in] Author pin (Auto = dispatcher decides -> PT in Phase 1)
								const bool oidnDenoise,				///< [in] Enable OIDN denoising post-process (forwarded to the delegate)
								const OidnQuality oidnQuality,		///< [in] OIDN quality preset (Auto = render-time heuristic)
								const OidnDevice oidnDevice,		///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
								const OidnPrefilter oidnPrefilter,	///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
								const PathGuidingConfig& guidingConfig,	///< [in] Path guiding configuration
								const AdaptiveSamplingConfig& adaptiveConfig,	///< [in] Adaptive sampling configuration
								const StabilityConfig& stabilityConfig,	///< [in] Production stability controls
								const bool useZSobol,				///< [in] Use Morton-indexed Sobol (blue-noise error distribution)
								const ProgressiveConfig& progressiveConfig,	///< [in] Progressive multi-pass configuration (applied to the delegate)
								const bool probeEnabled = false,	///< [in] Enable the Tier-2 render-time probe (Phase 4; gated on activation-spp)
								Implementation::FrameStore* frameStore = nullptr    ///< [in] Canonical FrameStore (default null until Job pushes one)
								);
	//! Creates the SPECTRAL auto-routing integrator dispatcher
	//! (`auto_spectral_rasterizer` - Phase 1b of docs/AUTO_RASTERIZER_DESIGN.md).
	//! Identical dispatcher to RISE_API_CreateAutoRasterizer (same AutoRasterizer
	//! class, same Tier-0 pin / Tier-1 static / Tier-2 probe decision logic via
	//! its domain flag), but the resolved delegate is one of the `*_spectral_`
	//! rasterizers, built with the spectral-core params below.  No path-guiding
	//! / optimal-MIS on the spectral domain (matching the concrete `*_spectral_`
	//! chunks); the disabled PathGuidingConfig the BDPT/VCM spectral factories
	//! still take is supplied internally.
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAutoSpectralRasterizer(
								IRasterizer** ppi,					///< [out] Pointer to receive the rasterizer
								IRayCaster* caster,					///< [in] Ray caster to use for rays
								ISampling2D* pSamples,				///< [in] Sampler for subsamples
								IPixelFilter* pFilter,				///< [in] Pixel Filter for samples
								const AutoIntegratorChoice integrator,	///< [in] Author pin (Auto = dispatcher decides)
								const Scalar lambda_begin,			///< [in] Start wavelength (nm)
								const Scalar lambda_end,			///< [in] End wavelength (nm)
								const unsigned int num_wavelengths,	///< [in] Number of wavelength bins
								const unsigned int spectral_samples,///< [in] Spectral samples per pixel
								const bool useHWSS,					///< [in] Use Hero Wavelength Spectral Sampling
								const bool oidnDenoise,				///< [in] Enable OIDN denoising post-process (forwarded to the delegate)
								const OidnQuality oidnQuality,		///< [in] OIDN quality preset (Auto = render-time heuristic)
								const OidnDevice oidnDevice,		///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
								const OidnPrefilter oidnPrefilter,	///< [in] OIDN aux source mode
								const AdaptiveSamplingConfig& adaptiveConfig,	///< [in] Adaptive sampling configuration
								const StabilityConfig& stabilityConfig,	///< [in] Production stability controls
								const bool useZSobol,				///< [in] Use Morton-indexed Sobol (blue-noise error distribution)
								const ProgressiveConfig& progressiveConfig,	///< [in] Progressive multi-pass configuration (applied to the delegate)
								const bool probeEnabled = false,	///< [in] Enable the Tier-2 render-time probe (Phase 4; gated on activation-spp)
								Implementation::FrameStore* frameStore = nullptr    ///< [in] Canonical FrameStore (default null until Job pushes one)
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
								const unsigned int smsTargetBounces,	///< [in] REQUIRED specular-vertex count per seed chain (Mitsuba `m_config.bounces` analogue).  0 = no target.  Set to natural caustic K (typically 2 for glass shells / interior lights).  Active in BOTH snell and uniform modes; recommended for uniform mode.
								const bool oidnDenoise,				///< [in] Enable OIDN denoising post-process
								const OidnQuality oidnQuality,		///< [in] OIDN quality preset (Auto = render-time heuristic)
							const OidnDevice oidnDevice,		///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
							const OidnPrefilter oidnPrefilter,	///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
								const AdaptiveSamplingConfig& adaptiveConfig,	///< [in] Adaptive sampling configuration
								const StabilityConfig& stabilityConfig,	///< [in] Production stability controls
								const bool useZSobol,				///< [in] Use Morton-indexed Sobol (blue-noise error distribution)
								const bool useHWSS,					///< [in] Use Hero Wavelength Spectral Sampling
								Implementation::FrameStore* frameStore = nullptr    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
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
								const bool oidnDenoise,						///< [in] Enable OIDN denoising post-process
								Implementation::FrameStore* frameStore = nullptr    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
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
								IPixelFilter* pFilter,						///< [in] Reconstruction kernel; may be null for unfiltered point splats
								Implementation::FrameStore* frameStore = nullptr    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
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
								const bool oidnDenoise,				///< [in] Enable OIDN denoising post-process
								Implementation::FrameStore* frameStore = nullptr    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
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
								IPixelFilter* pFilter,				///< [in] Reconstruction kernel; may be null for unfiltered point splats
								Implementation::FrameStore* frameStore = nullptr    ///< [in] L6a-2 — canonical FrameStore (default null until L6b)
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
		SceneEditCategory_Light      = 4,
		SceneEditCategory_Film       = 5,  ///< Output Settings (single Film per scene)
		SceneEditCategory_Material   = 6,  ///< Materials (editable for non-composed types)
		SceneEditCategory_Medium     = 7   ///< Participating media (Homogeneous editable;
		                                   ///< Heterogeneous read-only)
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

	//! Read the active toolbar tool — the value most recently passed to
	//! SetTool, or the default `Select` on a fresh controller.  Returns
	//! `Select` (0) on null controller.  Numeric values match
	//! SceneEditTool_* constants.
	int  RISE_API_SceneEditController_CurrentTool( SceneEditController* p );

	//! Map a tool int to its category int (Photoshop-style toolbar
	//! slot membership).  0 = Select, 1 = Camera, 2 = ObjectTransform.
	//! Pure-function; takes no controller pointer.  Out-of-range
	//! `tool` returns 0 (Select).
	int  RISE_API_SceneEditController_CategoryForTool( int tool );

	//! Per-category default sub-tool (the one a category's slot shows
	//! before the user picks anything from the flyout).  Pure-function.
	//! Out-of-range `category` returns Select (0).
	int  RISE_API_SceneEditController_DefaultSubToolForCategory( int category );

	//! Photoshop "last-used" memory: returns the sub-tool the user
	//! most recently picked from this category's flyout, or the
	//! category default if nothing has been picked yet.  Returns
	//! Select (0) on null controller / out-of-range category.
	int  RISE_API_SceneEditController_GetLastSubToolForCategory(
		SceneEditController* p, int category );

	//! Gizmo handle math — recompute the screen-space handle array
	//! from the current Object selection + tool + camera projection.
	//! No-op when the active tool isn't an Object-transform tool,
	//! when no Object is selected, or when the camera projection is
	//! degenerate.  Returns true on success (including the "no-op,
	//! count is 0" case); false on null controller.
	//!
	//! Platform UIs call this once per preview frame before reading
	//! the handle array.  Handle positions are in the camera's
	//! CURRENT image-pixel space — the same space the bridge's
	//! pointer events live in.  Apply the same `fullW`/`fullH`
	//! normalisation pointer events use (`GetCameraDimensions`) to
	//! map to widget space.
	bool RISE_API_SceneEditController_RefreshGizmoHandles( SceneEditController* p );

	//! Number of gizmo handles in the current array (0 when the
	//! gizmo system has no active object-transform tool / no
	//! selection).  Read AFTER `RefreshGizmoHandles`.
	unsigned int RISE_API_SceneEditController_GizmoHandleCount(
		SceneEditController* p );

	//! Read one gizmo handle.  Out parameters are populated when the
	//! call succeeds; values stay valid until the next refresh.
	//!   outKind: `GizmoHandle::Kind` cast to int (0..6).
	//!   outAxis: 0=X, 1=Y, 2=Z; -1 for screen-aligned handles.
	//!   outScreenX / outScreenY: image-pixel-space position.
	//!   outScreenRadius: hit-test radius / icon size hint in pixels.
	//! Returns false on null controller, out-of-range idx, or any
	//! null out-parameter pointer (caller may pass null for fields
	//! they don't need; non-null pointers all share the same return
	//! semantics).
	bool RISE_API_SceneEditController_GizmoHandle(
		SceneEditController* p, unsigned int idx,
		int* outKind, int* outAxis,
		double* outScreenX, double* outScreenY, double* outScreenRadius );

	//! Hit-test the current gizmo handle array against an image-
	//! pixel-space pointer position.  Returns the index of the
	//! closest hit handle, or -1 on miss / null controller.  Pure
	//! read (doesn't mutate drag state) — the platform UI can use
	//! this to render hover feedback before a real pointer-down.
	int  RISE_API_SceneEditController_GizmoHandleAt(
		SceneEditController* p, Scalar x, Scalar y );

	//! True iff a gizmo handle was hit on the most recent
	//! `OnPointerDown` and the drag is still active (no
	//! `OnPointerUp` yet).  False on null controller.  Platform UIs
	//! use this to switch the cursor / draw the active-handle
	//! highlight between PointerDown and PointerUp.
	bool RISE_API_SceneEditController_IsGizmoDragActive( SceneEditController* p );

	//! Active drag handle kind / axis.  Both return -1 when no drag
	//! is in progress (or on null controller).  Numeric values match
	//! `GizmoHandle::Kind`.
	int  RISE_API_SceneEditController_ActiveGizmoKind( SceneEditController* p );
	int  RISE_API_SceneEditController_ActiveGizmoAxis( SceneEditController* p );

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

	// ---- Phase 6.5 scene-file save ----------------------------------
	// Round-trip-save bindings.  See docs/ROUND_TRIP_SAVE_PLAN.md §9.9
	// + SceneEditController.h "Phase 6.5".

	//! Drives the GUI's "Save Scene" button enable state.
	bool RISE_API_SceneEditController_HasUnsavedChanges( SceneEditController* p );

	//! Save the in-memory edits to `filePath`.  Returns the engine's
	//! SaveResult.status numerically (0=Saved, 1=NoOp, 2=Refused,
	//! 3=Failed).  On Refused / Failed, `errOut` is filled with the
	//! engine's diagnostic — caller passes a buffer of at least
	//! `errOutCap` bytes; the function null-terminates and truncates
	//! if needed.  On success, `errOut` is set to "".
	//! Returns -1 on null controller / null filePath.
	int RISE_API_SceneEditController_RequestSave(
		SceneEditController* p,
		const char* filePath,
		char* errOut,
		unsigned int errOutCap );

	//! Install a C-callback fired on each `HasUnsavedChanges()`
	//! TRANSITION (clean→dirty or dirty→clean).  `userData` is passed
	//! verbatim to every invocation.  Pass `cb = nullptr` to detach.
	//! Caller is responsible for `userData` lifetime — typical pattern
	//! is __bridge_retained / CFBridgingRelease around an Obj-C block
	//! or an objc_storeWeak slot.
	typedef void (*RISE_API_DirtyChangedFn)( void* userData, int hasUnsavedChanges );
	bool RISE_API_SceneEditController_SetDirtyChangedCallback(
		SceneEditController* p,
		RISE_API_DirtyChangedFn cb,
		void* userData );

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
	//!   5 = Film (Output Settings — single Film per scene)
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
	//! SetActiveRasterizer respectively); Object / Light / Film
	//! selections are UI state only.  Returns false on null controller
	//! or category-specific failures (e.g. unknown camera name).
	bool RISE_API_SceneEditController_SetSelection(
		SceneEditController* p, int category, const char* name );

	//! Scene-level active entity for a category, independent of the UI
	//! selection.  Camera → active camera name; Rasterizer → active
	//! rasterizer type name; Film → "default" (a scene has exactly one
	//! Film by construction); Object/Light/None → empty.  Used by the
	//! accordion to populate the dropdown with the scene's current
	//! active entity on first load.  Returns false on null controller.
	bool RISE_API_SceneEditController_CategoryActiveName(
		SceneEditController* p, int category, char* buf, unsigned int bufLen );

	//! Monotonic counter — bumped on any structural mutation that
	//! could change a category's entity list.  Platform UIs cache
	//! (epoch, category) → entity-name list and re-pull when this
	//! advances.  0 on null controller.
	unsigned int RISE_API_SceneEditController_SceneEpoch(
		SceneEditController* p );

	//! Phase 4b — per-category selection accessor.  Returns the
	//! entity name picked in `category`'s panel section, or empty
	//! when nothing is picked in that section.  Distinct from
	//! `_GetSelectionName` which returns only the primary
	//! (most-recently-set) selection: the multi-section panel uses
	//! this to query each section's expansion + content
	//! independently (an Object pick auto-fills the Material section
	//! with the bound material name, so both sections report
	//! non-empty selections after a single click).  Returns false on
	//! null controller or bufLen == 0.
	bool RISE_API_SceneEditController_GetSelectionForCategory(
		SceneEditController* p, int category, char* buf, unsigned int bufLen );

	//! Phase 4b — is `category`'s accordion section expanded?
	//! Tracked SEPARATELY from the per-category selection so a
	//! user can click a section header to expand it without yet
	//! picking an entity (the dropdown shows the active-fallback;
	//! Camera/Rasterizer/Film render their active entity's rows
	//! beneath).  Returns false on null controller.
	bool RISE_API_SceneEditController_IsSectionExpanded(
		SceneEditController* p, int category );

	//! Phase 4b — collapse `category`'s section: clears both the
	//! expanded flag AND the per-category selection.  Does NOT
	//! affect other sections (use `_SetSelection` with category
	//! None for the panel-wide collapse).
	bool RISE_API_SceneEditController_CollapseSection(
		SceneEditController* p, int category );

	//! Phase 4b — per-category property snapshot accessors.  Each
	//! mirrors the matching single-tuple PropertyXxx call but takes
	//! a `category` (mirroring `SceneEditCategory_*`) so multi-
	//! section panels can read each expanded section's rows.
	//! `_RefreshProperties` populates all per-category snapshots in
	//! one pass; callers don't need to refresh per-category.
	unsigned int RISE_API_SceneEditController_PropertyCountFor(
		SceneEditController* p, int category );
	bool RISE_API_SceneEditController_PropertyNameFor(
		SceneEditController* p, int category, unsigned int idx,
		char* buf, unsigned int bufLen );
	bool RISE_API_SceneEditController_PropertyValueFor(
		SceneEditController* p, int category, unsigned int idx,
		char* buf, unsigned int bufLen );
	bool RISE_API_SceneEditController_PropertyDescriptionFor(
		SceneEditController* p, int category, unsigned int idx,
		char* buf, unsigned int bufLen );
	int RISE_API_SceneEditController_PropertyKindFor(
		SceneEditController* p, int category, unsigned int idx );
	bool RISE_API_SceneEditController_PropertyEditableFor(
		SceneEditController* p, int category, unsigned int idx );
	unsigned int RISE_API_SceneEditController_PropertyPresetCountFor(
		SceneEditController* p, int category, unsigned int idx );
	bool RISE_API_SceneEditController_PropertyPresetLabelFor(
		SceneEditController* p, int category, unsigned int idx, unsigned int presetIdx,
		char* buf, unsigned int bufLen );
	bool RISE_API_SceneEditController_PropertyPresetValueFor(
		SceneEditController* p, int category, unsigned int idx, unsigned int presetIdx,
		char* buf, unsigned int bufLen );
	bool RISE_API_SceneEditController_PropertyUnitLabelFor(
		SceneEditController* p, int category, unsigned int idx,
		char* buf, unsigned int bufLen );

	//! Phase 4b — per-category SetProperty.  Routes the edit through
	//! `category`'s per-section selection (matches the panel's
	//! multi-section layout: editing a row in the Materials section
	//! while Object is the primary still targets the right material).
	bool RISE_API_SceneEditController_SetPropertyForCategory(
		SceneEditController* p, int category, const char* name, const char* valueStr );

	//! Clone the currently-active camera under a new name and switch
	//! the scene to the new camera.  `proposedName` is the user's
	//! choice; on duplicate the controller appends a numeric dedup
	//! suffix.  The actual chosen name is written to `outName`
	//! (NUL-terminated; caller-owned buffer of `outLen` bytes).
	//! Returns false on null controller, no-active-camera, an
	//! unclonable camera type, or `outLen == 0`.  Bumps the controller's
	//! SceneEpoch so the platform UI auto-rebuilds the camera list.
	//!
	//! Persistence: the clone lives only in memory.  Reloading the
	//! .RISEscene file drops it (scene-text round-trip is the
	//! pending Phase 6 work).
	bool RISE_API_SceneEditController_AddCameraFromActive(
		SceneEditController* p,
		const char* proposedName,
		char* outName, unsigned int outLen );
}

#endif
