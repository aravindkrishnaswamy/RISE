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
#include "Interfaces/ISceneParser.h"
#include "Interfaces/IShader.h"
#include "Interfaces/IShaderManager.h"
#include "Interfaces/IShaderOp.h"
#include "Interfaces/IShaderOpManager.h"
#include "Interfaces/ITriangleMeshGeometry.h"
#include "Interfaces/ITriangleMeshLoader.h"
#include "Interfaces/ITwoColorOperator.h"
#include "Interfaces/IWriteBuffer.h"

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
		const Scalar linearAttenuation,							///< [in] Amount of linear attenuation
		const Scalar quadraticAttenuation						///< [in] Amount of quadratic attenuation
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
		const Scalar linearAttenuation,							///< [in] Amount of linear attenuation
		const Scalar quadraticAttenuation						///< [in] Amount of quadratic attenuation
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

	//! Creates a triangle mesh geometry object
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTriangleMeshGeometry(
						ITriangleMeshGeometry** ppi,			///< [out] Pointer to recieve the geometry
						const unsigned int max_polys,			///< [in] Maximum number of polygons / octant node
						const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
						const bool double_sided,				///< [in] Are the triangles double sided ?
						const bool use_bsp						///< [in] Use a BSP tree rather than an Octree
						);

	//! Creates an indexed triangle mesh geometry object
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateTriangleMeshGeometryIndexed(
						ITriangleMeshGeometryIndexed** ppi,	///< [out] Pointer to recieve the geometry
						const unsigned int max_polys,			///< [in] Maximum number of polygons / octant node
						const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
						const bool double_sided,				///< [in] Are the triangles double sided ?
						const bool use_bsp,					///< [in] Use a BSP tree rather than an Octree
						const bool face_normals				///< [in] Use face normals rather than vertex normals
						);

	//! Creates a mesh loader capable of loading a 3DS mesh from read buffer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_Create3DSTriangleMeshLoader(
						ITriangleMeshLoaderIndexed** ppi,		///< [out] Pointer to recieve the mesh loader
						IReadBuffer* pBuffer					///< [in] The buffer to load the 3DS mesh from
						);

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
						);

	//! Creates a geometry object made up of a series of bilinear patches
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBilinearPatchGeometry(
						IBilinearPatchGeometry** ppi,			///< [out] Pointer to recieve the geometry
						const unsigned int max_patches,			///< [in] Maximum number of patches / octant node
						const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
						const bool use_bsp						///< [in] Use a BSP tree rather than an Octree
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
								const Scalar thickness										///< [in] Thickness between the materials
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
								const bool nonmeshlights,			///< [in] Should we shoot from non mesh based lights?
								const bool useiorstack,				///< [in] Should the ray caster use a index of refraction stack?
								const Scalar power_scale,			///< [in] How much to scale light power by
								const unsigned int temporal_samples,///< [in] Number of temporal samples to take for animation frames
								const bool regenerate				///< [in] Should the tracer regenerate a new photon each time the scene time changes?
								);

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
								const bool nonmeshlights,			///< [in] Should we shoot from non mesh based lights?
								const bool useiorstack,				///< [in] Should the ray caster use a index of refraction stack?
								const Scalar power_scale,			///< [in] How much to scale light power by
								const unsigned int temporal_samples,///< [in] Number of temporal samples to take for animation frames
								const bool regenerate				///< [in] Should the tracer regenerate a new photon each time the scene time changes?
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
								const bool useiorstack,				///< [in] Should the ray caster use a index of refraction stack?
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
								const bool useiorstack,				///< [in] Should the ray caster use a index of refraction stack?
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
								const Scalar max_spacing 			///< [in] Maximum seperation
								);



	//////////////////////////////////////////////////////////
	// Rasterizer outputs
	//////////////////////////////////////////////////////////

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
								);

	//////////////////////////////////////////////////////////
	// Rasterize Sequeces
	//////////////////////////////////////////////////////////

	//! Creates a block rasterize sequence
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBlockRasterizeSequence(
								IRasterizeSequence** ppi,			///< [out] Pointer to recieve the rasterize sequence
								const unsigned int width,			///< [in] Width of the block
								const unsigned int height,			///< [in] Height of the block
								const char order					///< [in] Block ordering type
								);

	//! Creates a scanline rasterize sequence
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateScanlineRasterizeSequence(
								IRasterizeSequence** ppi			///< [out] Pointer to recieve the rasterize sequence
								);

	//! Creates a hilbert space filling curve rasterize sequence
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

	//! Creates a nearest neighbour raster image accessor
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateNNBRasterImageAccessor(
								IRasterImageAccessor** ppi,			///< [out] Pointer to recieve the accessor
								IRasterImage& image					///< [in] Raster image to access
								);

	//! Creates a bilinear raster image accessor
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBiLinRasterImageAccessor(
								IRasterImageAccessor** ppi,			///< [out] Pointer to recieve the accessor
								IRasterImage& image					///< [in] Raster image to access
								);

	//! Creates a catmull rom bicubic raster image accessor
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateCatmullRomBicubicRasterImageAccessor(
								IRasterImageAccessor** ppi,			///< [out] Pointer to recieve the accessor
								IRasterImage& image					///< [in] Raster image to access
								);


	//! Creates a bspline bicubic raster image accessor
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateUniformBSplineBicubicRasterImageAccessor(
								IRasterImageAccessor** ppi,			///< [out] Pointer to recieve the accessor
								IRasterImage& image					///< [in] Raster image to access
								);

	//! Creates a bicubic raster image accessor that uses the given matrix as the weights
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateBicubicRasterImageAccessor(
								IRasterImageAccessor** ppi,			///< [out] Pointer to recieve the accessor
								IRasterImage& image,				///< [in] Raster image to access
								const Matrix4& m
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
								const COLOR_SPACE color_space		///< [in] Color space to apply
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
								const IMaterial* bsdf,			///< [in] BSDF to use when computing radiance (overrides object BSDF)
								const bool nonmeshlights,		///< [in] Compute lighting from non mesh lights?
								const bool meshlights,			///< [in] Compute lighting from mesh lights (area light sources)?
								const bool cache					///< [in] Should the rasterizer state cache be used?
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
								const bool branch,				///< [in] Should we branch when doing scattering?
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
								const bool cache				///< [in] Should the rasterizer state cache be used?
								);

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
								ILuminaryManager** ppi,				///< [out] Pointer to recieve the luminary manager
								const bool bChooseOnlyOneLight		///< [in] Choose only one luminaire for each sample
								);

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
								IPixelFilter* pFilter				///< [in] Pixel Filter for samples
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
								const unsigned int num_wavelengths	///< [in] Number of wavelengths to sample
								);

	//! Creates an adaptive sampling pixel based rasterizer
	/// \return TRUE if successful, FALSE otherwise
	bool RISE_API_CreateAdaptiveSamplingPixelBasedPelRasterizer(
								IRasterizer** ppi,					///< [out] Pointer to recieve the rasterizer
								IRayCaster* caster,					///< [in] Ray caster to use for rays
								ISampling2D* pSamples,				///< [in] Sampler for subsamples
								IPixelFilter* pFilter,				///< [in] Pixel Filter for samples
								unsigned int maxS,					///< [in] Maximum number of samples to take
								Scalar var,							///< [in] Variance threshold
								unsigned int numsteps,				///< [in] Number of steps to take from base sampling to max samples
								bool bOutputSamples					///< [in] Should the renderer show how many samples rather than an image
								);

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
}

#endif
