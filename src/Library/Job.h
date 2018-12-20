//////////////////////////////////////////////////////////////////////
//
//  Job.h - A job is a central place that describes everything.
//          Within one job object, one can perform a complete
//          rendering.  All parameters pertinent to rasterization are
//			stored in the job.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 6, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef JOB_
#define JOB_

#include "Interfaces/IJobPriv.h"
#include "Interfaces/IScenePriv.h"
#include "Interfaces/IMaterial.h"
#include "Interfaces/IRayIntersectionModifier.h"
#include "Interfaces/IRasterizer.h"
#include "Interfaces/IPainter.h"
#include "Interfaces/IFunction2D.h"
#include "Interfaces/IGeometryManager.h"
#include "Interfaces/IObjectManager.h"
#include "Interfaces/ILightManager.h"
#include "Interfaces/IPainterManager.h"
#include "Interfaces/IMaterialManager.h"
#include "Interfaces/IShaderManager.h"
#include "Interfaces/IShaderOpManager.h"
#include "Interfaces/IModifierManager.h"
#include "Interfaces/IFunction1DManager.h"
#include "Interfaces/IFunction2DManager.h"
#include "Utilities/Reference.h"

namespace RISE
{
	//! Job - This is used to simplify the creation of a job, all things can be
	//! easily accessed by name, no need to keep track of managers and such
	class Job : public virtual IJobPriv, public Implementation::Reference
	{
	protected:
		virtual ~Job( );

		IScenePriv*									pScene;				// A job can have at most one scene
		IGeometryManager*							pGeomManager;		// Set of all geometry in the job
		IPainterManager*							pPntManager;		// Set of all painters in the job
		IFunction1DManager*							pFunc1DManager;		// Set of all 1D functions in the job
		IFunction2DManager*							pFunc2DManager;		// Set of all 2D functions in the job
		IMaterialManager*							pMatManager;		// Set of all materials in the job
		IModifierManager*							pModManager;		// Set of all modifiers in the job
		IObjectManager*								pObjectManager;		// Set of all objects in the job
		ILightManager*								pLightManager;		// Set of all the hacky lights in the job
		IShaderManager*								pShaderManager;		// Set of all shaders in the job
		IShaderOpManager*							pShaderOpManager;	// Set of all shaders ops in the job
		IRasterizer*								pRasterizer;		// A job can have at most one rasterizer

		IProgressCallback*							pGlobalProgress;	// A global progress reporter

		//
		// Helper functions
		//
		void InitializeContainers();
		void DestroyContainers();

		struct ANIMATION_OPTIONS
		{
			double time_start;						///< Scene time to start rasterizing at
			double time_end;						///< Scene time to finish rasterizing
			unsigned int num_frames;				///< Number of frames to rasterize
			bool do_fields;							///< Should the rasterizer do fields?
			bool invert_fields;						///< Should the fields be temporally inverted?

			ANIMATION_OPTIONS() :
			time_start( 0 ),
			time_end( 1 ),
			num_frames( 30 ),
			do_fields( false ),
			invert_fields( false )
			{
			}
		};

		ANIMATION_OPTIONS animOptions;

	public:
		Job( );

		//
		// Getters from the IJobPriv interface
		//
		IScenePriv*					GetScene()			{ return pScene; };
		IGeometryManager*			GetGeometries()		{ return pGeomManager; };
		IPainterManager*			GetPainters()		{ return pPntManager; };
		IFunction1DManager*			GetFunction1Ds()	{ return pFunc1DManager; };
		IFunction2DManager*			GetFunction2Ds()	{ return pFunc2DManager; };
		IMaterialManager*			GetMaterials()		{ return pMatManager; };
		IShaderManager*				GetShaders()		{ return pShaderManager; };
		IShaderOpManager*			GetShaderOps()		{ return pShaderOpManager; };
		IModifierManager*			GetModifiers()		{ return pModManager; };
		IObjectManager*				GetObjects()		{ return pObjectManager; };
		ILightManager*				GetLights()			{ return pLightManager; };
		IRasterizer*				GetRasterizer()		{ return pRasterizer; };



		//
		// Core settings
		//

		//! Resets the acceleration structure
		//! WARNING!  Call this before adding objects, otherwise you will LOSE them!
		//! \return TRUE if successful, FALSE otherwise
		bool SetPrimaryAcceleration(
			const bool bUseBSPtree,									///< [in] Use BSP trees for spatial partitioning
			const bool bUseOctree,									///< [in] Use Octrees for spatial partitioning
			const unsigned int nMaxObjectsPerNode,					///< [in] Maximum number of elements / node
			const unsigned int nMaxTreeDepth						///< [in] Maximum tree depth
			);

		//
		// Sets the camera
		//

		//! Sets a pinhole camera
		/// \return TRUE if successful, FALSE otherwise
		bool SetPinholeCamera(
			const double ptLocation[3],								///< [in] Absolute location of where the camera is located
			const double ptLookAt[3], 								///< [in] Absolute point the camera is looking at
			const double vUp[3],									///< [in] Up vector of the camera
			const double fov,										///< [in] Field of view in radians
			const unsigned int xres,								///< [in] X resolution of virtual screen
			const unsigned int yres,								///< [in] Y resolution of virtual screen
			const double pixelAR,									///< [in] Pixel aspect ratio
			const double exposure,									///< [in] Exposure time of the camera
			const double scanningRate,								///< [in] Rate at which each scanline is recorded
			const double pixelRate,									///< [in] Rate at which each pixel is recorded
			const double orientation[3],							///< [in] Orientation (Pitch,Roll,Yaw)
			const double target_orientation[2]						///< [in] Orientation relative to a target
			);

		//! Sets a pinhole camera
		/// \return TRUE if successful, FALSE otherwise
		bool SetPinholeCameraONB(
			const double ONB_U[3],									///< [in] U vector of orthonormal basis
			const double ONB_V[3],									///< [in] V vector of orthonormal basis
			const double ONB_W[3],									///< [in] W vector of orthonormal basis
			const double ptLocation[3],								///< [in] Absolute location of where the camera is located
			const double fov,										///< [in] Field of view in radians
			const unsigned int xres,								///< [in] X resolution of virtual screen
			const unsigned int yres,								///< [in] Y resolution of virtual screen
			const double pixelAR,									///< [in] Pixel aspect ratio
			const double exposure,									///< [in] Exposure time of the camera
			const double scanningRate,								///< [in] Rate at which each scanline is recorded
			const double pixelRate									///< [in] Rate at which each pixel is recorded
			);

		//! Sets a camera based on thin lens model
		/// \return TRUE if successful, FALSE otherwise
		bool SetThinlensCamera(
			const double ptLocation[3],								///< [in] Absolute location of where the camera is located
			const double ptLookAt[3], 								///< [in] Absolute point the camera is looking at
			const double vUp[3],									///< [in] Up vector of the camera
			const double fov,										///< [in] Field of view in radians
			const unsigned int xres,								///< [in] X resolution of virtual screen
			const unsigned int yres,								///< [in] Y resolution of virtual screen
			const double pixelAR,									///< [in] Pixel aspect ratio
			const double exposure,									///< [in] Exposure time of the camera
			const double scanningRate,								///< [in] Rate at which each scanline is recorded
			const double pixelRate,									///< [in] Rate at which each pixel is recorded
			const double orientation[3],							///< [in] Orientation (Pitch,Roll,Yaw)
			const double target_orientation[2],						///< [in] Orientation relative to a target
			const double aperture,									///< [in] Size of the aperture
			const double focalLength,								///< [in] Focal length
			const double focusDistance								///< [in] Focus distance default is INFINITY
			);

		//! Sets a fisheye camera
		/// \return TRUE if successful, FALSE otherwise
		bool SetFisheyeCamera(
			const double ptLocation[3],								///< [in] Absolute location of where the camera is located
			const double ptLookAt[3], 								///< [in] Absolute point the camera is looking at
			const double vUp[3],									///< [in] Up vector of the camera
			const unsigned int xres,								///< [in] X resolution of virtual screen
			const unsigned int yres,								///< [in] Y resolution of virtual screen
			const double pixelAR,									///< [in] Pixel aspect ratio
			const double exposure,									///< [in] Exposure time of the camera
			const double scanningRate,								///< [in] Rate at which each scanline is recorded
			const double pixelRate,									///< [in] Rate at which each pixel is recorded
			const double orientation[3],							///< [in] Orientation (Pitch,Roll,Yaw)
			const double target_orientation[2],						///< [in] Orientation relative to a target
			const double scale										///< [in] Scale factor to exagerrate the effects
			);

		//! Sets an orthographic camera
		/// \return TRUE if successful, FALSE otherwise
		bool SetOrthographicCamera(
			const double ptLocation[3],								///< [in] Absolute location of where the camera is located
			const double ptLookAt[3], 								///< [in] Absolute point the camera is looking at
			const double vUp[3],									///< [in] Up vector of the camera
			const unsigned int xres,								///< [in] X resolution of virtual screen
			const unsigned int yres,								///< [in] Y resolution of virtual screen
			const double vpScale[2],								///< [in] Viewport scale factor
			const double pixelAR,									///< [in] Pixel aspect ratio
			const double exposure,									///< [in] Exposure time of the camera
			const double scanningRate,								///< [in] Rate at which each scanline is recorded
			const double pixelRate,									///< [in] Rate at which each pixel is recorded
			const double orientation[3],							///< [in] Orientation (Pitch,Roll,Yaw)
			const double target_orientation[2]						///< [in] Orientation relative to a target
			);

		//
		// Adds painters
		//

		//! Adds a simple checker painter
		/// \return TRUE if successful, FALSE otherwise
		bool AddCheckerPainter(
									const char* name,				///< [in] Name of the painter
									const double size,				///< [in] Size of the checkers in texture mapping units
									const char* pa,					///< [in] First painter
									const char* pb					///< [in] Second painter
									);


		//! Adds a lines painter
		/// \return TRUE if successful, FALSE otherwise
		bool AddLinesPainter(
									const char* name,				///< [in] Name of the painter
									const double size,				///< [in] Size of the lines in texture mapping units
									const char* pa,					///< [in] First painter
									const char* pb,					///< [in] Second painter
									const bool bvert				///< [in] Are the lines vertical?
									);

		//! Adds a mandelbrot fractal painter
		/// \return TRUE if successful, FALSE otherwise
		bool AddMandelbrotFractalPainter(
									const char* name,				///< [in] Name of the painter
									const char* pa,					///< [in] First painter
									const char* pb,					///< [in] Second painter
									const double lower_x,
									const double upper_x,
									const double lower_y,
									const double upper_y,
									const double exp
									);

		//! Adds a 2D perlin noise painter
		/// \return TRUE if successful, FALSE otherwise
		bool AddPerlin2DPainter(
									const char* name,				///< [in] Name of the painter
									const double dPersistence,		///< [in] Persistence
									const unsigned int nOctaves,	///< [in] Number of octaves to use in noise generation
									const char* pa,					///< [in] First painter
									const char* pb,					///< [in] Second painter
									const double vScale[2],			///< [in] How much to scale the function by
									const double vShift[2]			///< [in] How much to shift the function by
									);

		//! Adds a 2D perlin noise painter
		/// \return TRUE if successful, FALSE otherwise
		bool AddPerlin3DPainter(
									const char* name,				///< [in] Name of the painter
									const double dPersistence,		///< [in] Persistence
									const unsigned int nOctaves,	///< [in] Number of octaves to use in noise generation
									const char* pa,					///< [in] First painter
									const char* pb,					///< [in] Second painter
									const double vScale[3],			///< [in] How much to scale the function by
									const double vShift[3]			///< [in] How much to shift the function by
									);

		//! Adds a spectral color painter
		/// \return TRUE if successful, FALSE otherwise
		bool AddSpectralColorPainter(
									const char* name,				///< [in] Name of the painter
									const double amplitudes[],		///< [in] Array that contains the amplitudes
									const double frequencies[],		///< [in] Array that contains the frequencies for the amplitudes
									const double lambda_begin,		///< [in] Begining of the spectral packet
									const double lambda_end,		///< [in] End of the spectral packet
									const unsigned int numfreq,		///< [in] Number of frequencies in the array
									const double scale				///< [in] How much to scale the amplitudes by
									);

		//! Adds a texture painter
		/// \return TRUE if successful, FALSE otherwise
		bool AddPNGTexturePainter(
									const char* name,				///< [in] Name of the painter
									const char* filename,			///< [in] Name of the file that contains the texture
									const char color_space,			///< [in] Color space in the file
																	///		0 - Rec709 RGB Linear
																	///		1 - sRGB profile
																	///		2 - ROMM RGB (ProPhotoRGB) linear
																	///		3 - ROMM RGB (ProPhotoRGB) non-linear
									const char filter_type,			///< [in] Type of texture filtering
																	///     0 - Nearest neighbour
																	///     1 - Bilinear
																	///     2 - Catmull Rom Bicubic
																	///     3 - Uniform BSpline Bicubic
									const bool lowmemory,			///< [in] low memory mode doesn't do an image convert
									const double scale[3],			///< [in] Scale factor for color values
									const double shift[3]			///< [in] Shift factor for color values
									);

		//! Adds a texture painter
		/// \return TRUE if successful, FALSE otherwise
		bool AddHDRTexturePainter(
									const char* name,				///< [in] Name of the painter
									const char* filename,			///< [in] Name of the file that contains the texture
									const char filter_type,			///< [in] Type of texture filtering
																	///     0 - Nearest neighbour
																	///     1 - Bilinear
																	///     2 - Catmull Rom Bicubic
																	///     3 - Uniform BSpline Bicubic
									const bool lowmemory,			///< [in] low memory mode doesn't do an image convert
									const double scale[3],			///< [in] Scale factor for color values
									const double shift[3]			///< [in] Shift factor for color values
									);

		//! Adds an EXR texture painter
		/// \return TRUE if successful, FALSE otherwise
		bool AddEXRTexturePainter(
									const char* name,				///< [in] Name of the painter
									const char* filename,			///< [in] Name of the file that contains the texture
									const char color_space,			///< [in] Color space in the file
																	///		0 - Rec709 RGB Linear
																	///		1 - sRGB profile
																	///		2 - ROMM RGB (ProPhotoRGB) linear
																	///		3 - ROMM RGB (ProPhotoRGB) non-linear
									const char filter_type,			///< [in] Type of texture filtering
																	///     0 - Nearest neighbour
																	///     1 - Bilinear
																	///     2 - Catmull Rom Bicubic
																	///     3 - Uniform BSpline Bicubic
									const bool lowmemory,			///< [in] low memory mode doesn't do an image convert
									const double scale[3],			///< [in] Scale factor for color values
									const double shift[3]			///< [in] Shift factor for color values
									);

		//! Adds a texture painter
		/// \return TRUE if successful, FALSE otherwise
		bool AddTIFFTexturePainter(
									const char* name,				///< [in] Name of the painter
									const char* filename,			///< [in] Name of the file that contains the texture
									const char color_space,			///< [in] Color space in the file
																	///		0 - Rec709 RGB Linear
																	///		1 - sRGB profile
																	///		2 - ROMM RGB (ProPhotoRGB) linear
																	///		3 - ROMM RGB (ProPhotoRGB) non-linear
									const char filter_type,			///< [in] Type of texture filtering
																	///     0 - Nearest neighbour
																	///     1 - Bilinear
																	///     2 - Catmull Rom Bicubic
																	///     3 - Uniform BSpline Bicubic
									const bool lowmemory,			///< [in] low memory mode doesn't do an image convert
									const double scale[3],			///< [in] Scale factor for color values
									const double shift[3]			///< [in] Shift factor for color values
									);

		//! Adds a painter that paints a uniform color
		/// \return TRUE if successful, FALSE otherwise
		bool AddUniformColorPainter(
									const char* name,				///< [in] Name of the painter
									const double pel[3],			///< [in] Color to paint
									const char* cspace				///< [in] Color space of the given color
									);

		//! Adds a painter that paints a voronoi diagram
		/// \return TRUE if successful, FALSE otherwise
		bool AddVoronoi2DPainter(
									const char* name,				///< [in] Name of the painter
									const double pt_x[],			///< [in] X co-ordinates of generators
									const double pt_y[],			///< [in] Y co-ordinates of generators
									const char** painters,			///< [in] The painters for each generator
									const unsigned int count,		///< [in] Number of the generators
									const char* border,				///< [in] Name of the painter for the border
									const double bsize				///< [in] Size of the border
									);

		//! Adds a painter that paints a voronoi diagram in 3D
		/// \return TRUE if successful, FALSE otherwise
		bool AddVoronoi3DPainter(
									const char* name,				///< [in] Name of the painter
									const double pt_x[],			///< [in] X co-ordinates of generators
									const double pt_y[],			///< [in] Y co-ordinates of generators
									const double pt_z[],			///< [in] Z co-ordinates of generators
									const char** painters,			///< [in] The painters for each generator
									const unsigned int count,		///< [in] Number of the generators
									const char* border,				///< [in] Name of the painter for the border
									const double bsize				///< [in] Size of the border
									);

		//! Adds a iridescent painter (a painter whose color changes as viewing angle changes)
		/// \return TRUE if successful, FALSE otherwise
		bool AddIridescentPainter(
									const char* name,				///< [in] Name of the painter
									const char* pa,					///< [in] First painter
									const char* pb,					///< [in] Second painter
									const double bias				///< [in] Biases the iridescence to one color or another
									);

		//! Creates a black body radiator painter
		/// \return TRUE if successful, FALSE otherwise
		bool AddBlackBodyPainter(
									const char* name,				///< [in] Name of the painter
									const double temperature,		///< [in] Temperature of the radiator in Kelvins
									const double lambda_begin,		///< [in] Where in the spectrum to start creating the spectral packet
									const double lambda_end,		///< [in] Where in the spectrum to end creating the spectral packet
									const unsigned int num_freq,	///< [in] Number of frequencies to use in the spectral packet
									const bool normalize,			///< [in] Should the values be normalized to peak intensity?
									const double scale				///< [in] Value to scale radiant exitance by
									);

		//! Adds a blend painter
		/// \return TRUE if successful, FALSE otherwise
		bool AddBlendPainter(
									const char* name,				///< [in] Name of the painter
									const char* pa,					///< [in] First painter
									const char* pb,					///< [in] Second painter
									const char* mask				///< [in] Mask painter
									);


		//
		// Adding materials
		//


		//! Creates Lambertian material
		/// \return TRUE if successful, FALSE otherwise
		bool AddLambertianMaterial(
									const char* name,				///< [in] Name of the material
									const char* ref					///< [in] Reflectance Painter
									);

		//! Creates a Polished material
		/// \return TRUE if successful, FALSE otherwise
		bool AddPolishedMaterial(
									const char* name,				///< [in] Name of the material
									const char* ref,				///< [in] Reflectance of diffuse substrate
									const char* tau,				///< [in] Transmittance of dielectric top
									const char* Nt,					///< [in] Index of refraction of dielectric coating
									const char* scat,				///< [in] Scattering function for dielectric coating (either Phong or HG)
									const bool hg					///< [in] Use Henyey-Greenstein phase function scattering
									);

		//! Creates a Dielectric material
		/// \return TRUE if successful, FALSE otherwise
		bool AddDielectricMaterial(
									const char* name,				///< [in] Name of the material
									const char* tau,				///< [in] Transmittance painter
									const char* rIndex,				///< [in] Index of refraction
									const char* scat,				///< [in] Scattering function (either Phong or HG)
									const bool hg					///< [in] Use Henyey-Greenstein phase function scattering
									);

		//! Creates an isotropic phong material
		/// \return TRUE if successful, FALSE otherwise
		bool AddIsotropicPhongMaterial(
									const char* name,				///< [in] Name of the material
									const char* rd,					///< [in] Diffuse reflectance painter
									const char* rs,					///< [in] Specular reflectance painter
									const char* exponent			///< [in] Phong exponent
									);

		//! Creates the anisotropic phong material of Ashikmin and Shirley
		/// \return TRUE if successful, FALSE otherwise
		bool AddAshikminShirleyAnisotropicPhongMaterial(
									const char* name,				///< [in] Name of the material
									const char* rd,					///< [in] Diffuse reflectance painter
									const char* rs,					///< [in] Specular reflectance painter
									const char* Nu,					///< [in] Phong exponent in U
									const char* Nv					///< [in] Phong exponent in V
									);

		//! Creates a perfect reflector
		/// \return TRUE if successful, FALSE otherwise
		bool AddPerfectReflectorMaterial(
									const char* name,				///< [in] Name of the material
									const char* ref					///< [in] Reflectance painter
									);

		//! Creates a perfect refractor
		/// \return TRUE if successful, FALSE otherwise
		bool AddPerfectRefractorMaterial(
									const char* name,				///< [in] Name of the material
									const char* ref,				///< [in] Amount of refraction painter
									const char* ior					///< [in] Index of refraction
									);

		//! Creates a translucent material
		/// \return TRUE if successful, FALSE otherwise
		bool AddTranslucentMaterial(
									const char* name,				///< [in] Name of the material
									const char* rF,					///< [in] Reflectance painter
									const char* T,					///< [in] Transmittance painter
									const char* ext,				///< [in] Extinction painter
									const char* N,					///< [in] Index of refraction
									const char* scat				///< [in] Multiple scattering component
									);

		//! Adds a BioSpec skin material as described by Krishnaswamy and Baranoski
		/// \return TRUE if successful, FALSE otherwise
		bool AddBioSpecSkinMaterial(
			const char* name,
			const char* thickness_SC_,									///< Thickness of the stratum corneum (in cm)
			const char* thickness_epidermis_,							///< Thickness of the epidermis (in cm)
			const char* thickness_papillary_dermis_,					///< Thickness of the papillary dermis (in cm)
			const char* thickness_reticular_dermis_,					///< Thickness of the reticular dermis (in cm)
			const char* ior_SC_,										///< Index of refraction of the stratum corneum
			const char* ior_epidermis_,									///< Index of refraction of the epidermis
			const char* ior_papillary_dermis_,							///< Index of refraction of the papillary dermis
			const char* ior_reticular_dermis_,							///< Index of refraction of the reticular dermis
			const char* concentration_eumelanin_,						///< Average Concentration of eumelanin in the melanosomes
			const char* concentration_pheomelanin_,						///< Average Concentration of pheomelanin in the melanosomes
			const char* melanosomes_in_epidermis_,						///< Percentage of the epidermis made up of melanosomes
			const char* hb_ratio_,										///< Ratio of oxyhemoglobin to deoxyhemoglobin in blood
			const char* whole_blood_in_papillary_dermis_,				///< Percentage of the papillary dermis made up of whole blood
			const char* whole_blood_in_reticular_dermis_,				///< Percentage of the reticular dermis made up of whole blood
			const char* bilirubin_concentration_,						///< Concentration of Bilirubin in whole blood
			const char* betacarotene_concentration_SC_,					///< Concentration of Beta-Carotene in the stratum corneum
			const char* betacarotene_concentration_epidermis_,			///< Concentration of Beta-Carotene in the epidermis
			const char* betacarotene_concentration_dermis_,				///< Concentration of Beta-Carotene in the dermis
			const char* folds_aspect_ratio_,							///< Aspect ratio of the little folds and wrinkles on the skin surface
			const bool bSubdermalLayer									///< Should the model simulate a perfectly reflecting subdermal layer?
			);

		//! Adds a generic human tissue material based on BioSpec
		/// \return TRUE if successful, FALSE otherwise
		bool AddGenericHumanTissueMaterial(
			const char* name,
			const char* sca,											///< [in] Scattering co-efficient
			const char* g,												///< [in] The g factor in the HG phase function
			const double whole_blood_,									///< Percentage of the tissue made up of whole blood
			const double hb_ratio_,										///< Ratio of oxyhemoglobin to deoxyhemoglobin in blood
			const double bilirubin_concentration_,						///< Concentration of Bilirubin in whole blood
			const double betacarotene_concentration_,					///< Concentration of Beta-Carotene in whole blood
			const bool diffuse											///< Is the tissue just completely diffuse?
			);

		//! Adds Composite material
		/// \return TRUE if successful, FALSE otherwise
		bool AddCompositeMaterial(
			const char* name,											///< [in] Name of the material
			const char* top,											///< [in] Name of material on top
			const char* bottom,											///< [in] Name of material on bottom
			const unsigned int max_recur,								///< [in] Maximum recursion level in the random walk process
			const unsigned int max_reflection_recursion,				///< [in] Maximum level of reflection recursion
			const unsigned int max_refraction_recursion,				///< [in] Maximum level of refraction recursion
			const unsigned int max_diffuse_recursion,					///< [in] Maximum level of diffuse recursion
			const unsigned int max_translucent_recursion,				///< [in] Maximum level of translucent recursion
			const double thickness										///< [in] Thickness between the materials
			);

		//! Adds Ward's isotropic gaussian material
		/// \return TRUE if successful, FALSE otherwise
		bool AddWardIsotropicGaussianMaterial(
			const char* name,											///< [in] Name of the material
			const char* diffuse,										///< [in] Diffuse reflectance
			const char* specular,										///< [in] Specular reflectance
			const char* alpha											///< [in] Standard deviation (RMS) of surface slope
			);

		//! Adds Ward's anisotropic elliptical gaussian material
		/// \return TRUE if successful, FALSE otherwise
		bool AddWardAnisotropicEllipticalGaussianMaterial(
			const char* name,											///< [in] Name of the material
			const char* diffuse,										///< [in] Diffuse reflectance
			const char* specular,										///< [in] Specular reflectance
			const char* alphax,											///< [in] Standard deviation (RMS) of surface slope in x
			const char* alphay											///< [in] Standard deviation (RMS) of surface slope in y
			);

		//! Adds Cook Torrance material
		/// \return TRUE if successful, FALSE otherwise
		bool AddCookTorranceMaterial(
			const char* name,											///< [in] Name of the material
			const char* diffuse,										///< [in] Diffuse reflectance
			const char* specular,										///< [in] Specular reflectance
			const char* facet,											///< [in] Facet distribution
			const char* ior,											///< [in] IOR delta
			const char* ext												///< [in] Extinction factor
			);

		//! Adds Oren-Nayar material
		/// \return TRUE if successful, FALSE otherwise
		bool AddOrenNayarMaterial(
			const char* name,											///< [in] Name of the material
			const char* reflectance,									///< [in] Reflectance
			const char* roughness										///< [in] Roughness factor
			);

		//! Adds Schlick material
		/// \return TRUE if successful, FALSE otherwise
		bool AddSchlickMaterial(
			const char* name,											///< [in] Name of the material
			const char* diffuse,										///< [in] Diffuse reflectance
			const char* specular,										///< [in] Specular reflectance
			const char* roughness,										///< [in] Roughness factor
			const char* isotropy										///< [in] Isotropy factor
			);

		//! Adds a data driven material
		/// \return TRUE if successful, FALSE otherwise
		bool AddDataDrivenMaterial(
			const char* name,											///< [in] Name of the material
			const char* filename										///< [in] Filename to load data from
			);

		//! Creates a lambertian luminaire material
		/// \return TRUE if successful, FALSE otherwise
		bool AddLambertianLuminaireMaterial(
			const char* name,											///< [in] Name of the material
			const char* radEx,											///< [in] Radiant exitance painter
			const char* mat,											///< [in] Material to use for all non emmission properties
			const double scale											///< [in] Value to scale radiant exitance by
			);

		//! Creates a phong luminaire material
		/// \return TRUE if successful, FALSE otherwise
		bool AddPhongLuminaireMaterial(
			const char* name,											///< [in] Name of the material
			const char* radEx,											///< [in] Radiance exitance painter
			const char* mat,											///< [in] Material to use for all non emmission properties
			const char* N,												///< [in] Phong exponent function
			const double scale											///< [in] Value to scale radiant exitance by
			);


		//
		// Adds geometry
		//

		//! Creates a box located at the origin
		/// \return TRUE if successful, FALSE otherwise
		bool AddBoxGeometry(
								const char* name,					///< [in] Name of the geometry
								const double width,					///< [in] Width of the box
								const double height,				///< [in] Height of the box
								const double depth					///< [in] Depth of the box
								);

		//! Creates a circular disk at the origin
		/// \return TRUE if successful, FALSE otherwise
		bool AddCircularDiskGeometry(
										const char* name,			///< [in] Name of the geometry
										const double radius,		///< [in] Radius of the disk
										const char axis				///< [in] (x|y|z) Which axis the disk sits on
										);

		//! Creates a clipped plane, defined by four points
		/// \return TRUE if successful, FALSE otherwise
		bool AddClippedPlaneGeometry(
										const char* name,			///< [in] Name of the geometry
										const double ptA[4],		///< [in] Point A of the clipped plane
										const double ptB[4],		///< [in] Point B of the clipped plane
										const double ptC[4],		///< [in] Point C of the clipped plane
										const double ptD[4],		///< [in] Point D of the clipped plane
										const bool doublesided		///< [in] Is it doublesided?
										);

		//! Creates a Cylinder at the origin
		/// \return TRUE if successful, FALSE otherwise
		bool AddCylinderGeometry(
									const char* name,				///< [in] Name of the geometry
									const char axis,				///< [in] (x|y|z) Which axis the cylinder is sitting on
									const double radius,			///< [in] Radius of the cylinder
									const double height			///< [in] Height of the cylinder
									);

		//! Creates an infinite plane that passes through the origin
		/// \return TRUE if successful, FALSE otherwise
		/// \todo This needs to be seriously re-evaluated
		bool AddInfinitePlaneGeometry(
												const char* name,	///< [in] Name of the geometry
												const double xt,	///< [in] How often to tile in X
												const double yt	///< [in] How often to tile in Y
												);

		//! Creates a sphere at the origin
		/// \return TRUE if successful, FALSE otherwise
		bool AddSphereGeometry(
										const char* name,		///< [in] Name of the geometry
										const double radius		///< [in] Radius of the sphere
										);

		//! Creates an ellipsoid at the origin
		/// \return TRUE if successful, FALSE otherwise
		bool AddEllipsoidGeometry(
										const char* name,		///< [in] Name of the geometry
										const double radii[3]	///< [in] Radii of the ellipse
										);

		//! Creates a torus at the origin
		/// \return TRUE if successful, FALSE otherwise
		bool AddTorusGeometry(
										const char* name,			///< [in] Name of the geometry
										const double majorRad,	///< [in] Major radius
										const double minorRad		///< [in] Minor radius (as a percentage of the major radius)
										);

		//! Adds a triangle mesh geometry from the pointers passed it
		/// \return TRUE if successful, FALSE otherwise
		bool AddIndexedTriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const float* vertices,					///< [in] List of vertices
							const float* normals,					///< [in] List of normals
							const float* coords,					///< [in] Texture co-ordinates
							const unsigned int* vertexface,			///< [in] List of the vertex faces
							const unsigned int* uvwface,			///< [in] List of the texture coord faces
							const unsigned int* normalface,			///< [in] List of normal faces
							const unsigned int numpts,				///< [in] Number of points, normals and texture coords
							const unsigned int numnormals,			///< [in] Number of normals
							const unsigned int numcoords,			///< [in] Number of texture co-ordinate points
							const unsigned int numfaces,			///< [in] Number of faces
							const unsigned int max_polys,			///< [in] Maximum number of polygons in one octant node
							const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
							const bool double_sided,				///< [in] Are the triangles double sided ?
							const bool use_bsp,						///< [in] Use a BSP tree rather than an Octree
							const bool face_normals					///< [in] Use face normals rather than vertex normals
							);

		//! Creates a triangle mesh geometry from a 3DS file
		/// \return TRUE if successful, FALSE otherwise
		bool Add3DSTriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* filename,					///< [in] The 3DS file to load
							const unsigned int max_polys,			///< [in] Maximum number of polygons in one octant node
							const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
							const bool double_sided,				///< [in] Are the triangles double sided ?
							const bool use_bsp,						///< [in] Use a BSP tree rather than an Octree
							const bool face_normals					///< [in] Use face normals rather than vertex normals
							);

		//! Creates a triangle mesh geometry from a raw file
		/// \return TRUE if successful, FALSE otherwise
		bool AddRAWTriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] Name of the file to load from
							const unsigned int max_polys,			///< [in] Maximum number of polygons in one octant node
							const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
							const bool double_sided,				///< [in] Are the triangles double sided ?
							const bool use_bsp						///< [in] Use a BSP tree rather than an Octree
							);

		//! Creates a triangle mesh geometry from a file of version 2
		//! The format of the file for this version is different from the one
		//! above
		/// \return TRUE if successful, FALSE otherwise
		bool AddRAW2TriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] Name of the file to load from
							const unsigned int max_polys,			///< [in] Maximum number of polygons in one octant node
							const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
							const bool double_sided,				///< [in] Are the triangles double sided ?
							const bool use_bsp,						///< [in] Use a BSP tree rather than an Octree
							const bool face_normals					///< [in] Use face normals rather than vertex normals
							);

		//! Creates a triangle mesh geometry from a series of bezier patches
		/// \return TRUE if successful, FALSE otherwise
		/// \todo this is deprecated and should be removed
		bool AddBezierTriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] Name of the file to load from
							const unsigned int detail,				///< [in] Level of tesselation
							const bool bCombineSharedVertices,		///< [in] Should we try to combine shared vertices?
							const bool bCenterObject,				///< [in] Should the object be re-centered around the origin
							const unsigned int max_polys,			///< [in] Maximum number of polygons in one octant node
							const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
							const bool double_sided,				///< [in] Are the triangles double sided ?
							const bool use_bsp,						///< [in] Use a BSP tree rather than an Octree
							const bool face_normals,				///< [in] Use face normals rather than vertex normals
							const char* displacement,				///< [in] Displacement function for static displacement mapping
							const double disp_scale					///< [in] Displacement scale factor
							);

		//! Creates a triangle mesh geometry from a ply file
		/// \return TRUE if successful, FALSE otherwise
		bool AddPLYTriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] Name of the file to load from
							const unsigned int max_polys,			///< [in] Maximum number of polygons in one octant node
							const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
							const bool double_sided,				///< [in] Are the triangles double sided ?
							const bool use_bsp,						///< [in] Use a BSP tree rather than an Octree
							const bool bInvertFaces,				///< [in] Should the faces be inverted?
							const bool face_normals					///< [in] Use face normals rather than vertex normals
							);

		//! Creates a mesh from a .risemesh file
		/// \return TRUE if successful, FALSE otherwise
		bool AddRISEMeshTriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] Name of the file to load from
							const bool load_into_memory,			///< [in] Do we load the entire file into memory before reading?
							const bool face_normals					///< [in] Use face normals rather than vertex normals
							);

		//! Creates a bezier patch geometry
		/// \return TRUE if successful, FALSE otherwise
		bool AddBezierPatchGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] Name of the file to load from
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
							const char* displacement,				///< [in] Displacement function for static displacement mapping
							const double disp_scale					///< [in] Displacement scale factor
							);

		//! Creates a bilinear patch geometry
		/// \return TRUE if successful, FALSE otherwise
		bool AddBilinearPatchGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] Name of the file to load from
							const unsigned int max_polys,			///< [in] Maximum number of polygons in one octant node
							const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
							const bool use_bsp						///< [in] Use a BSP tree rather than an Octree
							);

		//
		// Adds lights
		//

		//! Creates a infinite point omni light, located at the origin
		/// \return TRUE if successful, FALSE otherwise
		bool AddPointOmniLight(
			const char* name,										///< [in] Name of the light
			const double power,										///< [in] Power of the light in watts
			const double srgb[3],									///< [in] Color of the light in a non-linear colorspace
			const double pos[3],									///< [in] Position of the light
			const double linearAttenuation,							///< [in] Amount of linear attenuation
			const double quadraticAttenuation						///< [in] Amount of quadratic attenuation
			);

		//! Creates a infinite point spot light
		/// \return TRUE if successful, FALSE otherwise
		bool AddPointSpotLight(
			const char* name,										///< [in] Name of the light
			const double power,										///< [in] Power of the light in watts
			const double srgb[3],									///< [in] Color of the light in a non-linear colorspace
			const double foc[3],									///< [in] Point the center of the light is focussing on
			const double inner,										///< [in] Angle of the inner cone in radians
			const double outer,										///< [in] Angle of the outer cone in radians
			const double pos[3],									///< [in] Position of the light
			const double linearAttenuation,							///< [in] Amount of linear attenuation
			const double quadraticAttenuation						///< [in] Amount of quadratic attenuation
			);

		//! Creates the ambient light
		/// \return TRUE if successful, FALSE otherwise
		bool AddAmbientLight(
			const char* name,										///< [in] Name of the light
			const double power,										///< [in] Power of the light in watts
			const double srgb[3]									///< [in] Color of the light in a non-linear colorspace
			);

		//! Adds an infinite directional light, shining in a particular direction
		/// \return TRUE if successful, FALSE otherwise
		bool AddDirectionalLight(
			const char* name,										///< [in] Name of the light
			const double power,										///< [in] Power of the light in watts
			const double srgb[3],									///< [in] Color of the light in a non-linear colorspace
			const double dir[3]										///< [in] Direction of the light
			);

		//
		// Adds modifiers
		//

		//! Creates a bump map
		/// \return TRUE if successful, FALSE otherwise
		bool AddBumpMapModifier(
			const char* name,										///< [in] Name of the modifiers
			const char* func,										///< [in] The function to use as the bump generator
			const double scale,										///< [in] Factor to scale values by
			const double window										///< [in] Size of the window
			);

		//
		// Adds functions
		//

		//! Adds a piecewise linear function
		bool AddPiecewiseLinearFunction(
			const char* name,										///< [in] Name of the function
			const double x[],										///< [in] X values of the function
			const double y[],										///< [in] Y values of the function
			const unsigned int num,									///< [in] Number of control points in the x and y arrays
			const bool bUseLUTs,									///< [in] Should the function use lookup tables
			const unsigned int lutsize								///< [in] Size of the lookup table
			);

		//! Adds a 2D piecewise linear function built up of other functions
		bool AddPiecewiseLinearFunction2D(
			const char* name,										///< [in] Name of the function
			const double x[],										///< [in] X values of the function
			char** y,												///< [in] Y values which is the name of other function1Ds
			const unsigned int num									///< [in] Number of control points in the x and y arrays
			);


		//
		// Adds Objects
		//

		//! Adds an object
		/// \return TRUE if successful, FALSE otherwise
		bool AddObject(
			const char* name,										///< [in] Name of the object
			const char* geom,										///< [in] Name of the geometry for the object
			const char* material,									///< [in] Name of the material
			const char* modifier,									///< [in] Name of the modifier
			const char* shader,										///< [in] Name of the shader
			const char* radiancemap,								///< [in] Name of the painter to use as a radiance map
			const double radiancemap_scale,							///< [in] How much to scale the radiance map values by
			const double radiance_orient[3],						///< [in] Orientation of the object
			const double pos[3],									///< [in] Position of the object
			const double orient[3],									///< [in] Orientation of the object
			const double scale[3],									///< [in] Object scaling
			const bool bCastsShadows,								///< [in] Does the object cast shadows?
			const bool bReceivesShadows								///< [in] Does the object receive shadows?
		);

		//! Creates a CSG object
		/// \return TRUE if successful, FALSE otherwise
		bool AddCSGObject(
			const char* name,										///< [in] Name of the object
			const char* objA,										///< [in] Name of the first object
			const char* objB,										///< [in] Name of the second object
			const char op,											///< [in] CSG operation
																	///< 0 -> Union
																	///< 1 -> Intersection
																	///< 2 -> A-B
																	///< 3 -> B-A
			const char* material,									///< [in] Name of the material
			const char* modifier,									///< [in] Name of the modifier
			const char* shader,										///< [in] Name of the shader
			const char* radiancemap,								///< [in] Name of the painter to use as a radiance map
			const double radiancemap_scale,							///< [in] How much to scale the radiance map values by
			const double radiance_orient[3],						///< [in] Orientation of the object
			const double pos[3],									///< [in] Position of the object
			const double orient[3],									///< [in] Orientation of the object
			const bool bCastsShadows,								///< [in] Does the object cast shadows?
			const bool bReceivesShadows								///< [in] Does the object receive shadows?
			);

		//
		// Adds ShaderOps
		//
		bool AddReflectionShaderOp(
			const char* name										///< [in] Name of the shaderop
			);

		bool AddRefractionShaderOp(
			const char* name										///< [in] Name of the shaderop
			);

		bool AddEmissionShaderOp(
			const char* name										///< [in] Name of the shaderop
			);

		bool AddDirectLightingShaderOp(
			const char* name,										///< [in] Name of the shaderop
			const char* bsdf,										///< [in] BSDF to use when computing radiance (overrides object BSDF)
			const bool nonmeshlights,								///< [in] Compute lighting from non mesh lights?
			const bool meshlights,									///< [in] Compute lighting from mesh lights (area light sources)?
			const bool cache										///< [in] Should the rasterizer state cache be used?
			);

		bool AddCausticPelPhotonMapShaderOp(
			const char* name										///< [in] Name of the shaderop
			);

		bool AddCausticSpectralPhotonMapShaderOp(
			const char* name										///< [in] Name of the shaderop
			);

		bool AddGlobalPelPhotonMapShaderOp(
			const char* name										///< [in] Name of the shaderop
			);

		bool AddGlobalSpectralPhotonMapShaderOp(
			const char* name										///< [in] Name of the shaderop
			);

		bool AddTranslucentPelPhotonMapShaderOp(
			const char* name										///< [in] Name of the shaderop
			);

		bool AddShadowPhotonMapShaderOp(
			const char* name										///< [in] Name of the shaderop
			);

		bool AddDistributionTracingShaderOp(
			const char* name,										///< [in] Name of the shaderop
			const unsigned int samples,								///< [in] Number of sample to use in distribution
			const bool irradiancecaching,							///< [in] Should irradiance caching be used if available?
			const bool forcecheckemitters,							///< [in] Force rays allowing to hit emitters even though the material may have a BRDF
			const bool branch,										///< [in] Should we branch when doing scattering?
			const bool reflections,									///< [in] Should reflections be traced?
			const bool refractions,									///< [in] Should refractions be traced?
			const bool diffuse,										///< [in] Should diffuse rays be traced?
			const bool translucents									///< [in] Should translucent rays be traced?
			);

		bool AddFinalGatherShaderOp(
			const char* name,										///< [in] Name of the shaderop
			const unsigned int numtheta,							///< [in] Number of samples in the theta direction
			const unsigned int numphi,								///< [in] Number of samples in the phi direction
			const bool cachegradients,								///< [in] Should cache gradients be used in the irradiance cache?
			const bool cache										///< [in] Should the rasterizer state cache be used?
			);

		bool AddPathTracingShaderOp(
			const char* name,										///< [in] Name of the shaderop
			const bool branch,										///< [in] Should we branch the rays ?
			const bool forcecheckemitters,							///< [in] Force rays allowing to hit emitters even though the material may have a BRDF
			const bool bFinalGather,								///< [in] Should the path tracer co-operate and act as final gather?
			const bool reflections,									///< [in] Should reflections be traced?
			const bool refractions,									///< [in] Should refractions be traced?
			const bool diffuse,										///< [in] Should diffuse rays be traced?
			const bool translucents									///< [in] Should translucent rays be traced?
			);

		bool AddAmbientOcclusionShaderOp(
			const char* name,										///< [in] Name of the shaderop
			const unsigned int numtheta,							///< [in] Number of samples in the theta direction
			const unsigned int numphi,								///< [in] Number of samples in the phi direction
			const bool multiplybrdf,								///< [in] Should individual samples be multiplied by the BRDF ?
			const bool irradiance_cache								///< [in] Should the irradiance state cache be used?
			);

		bool AddSimpleSubSurfaceScatteringShaderOp(
			const char* name,										///< [in] Name of the shaderop
			const unsigned int numPoints,							///< [in] Number of points to use in sampling
			const double error,										///< [in] Error tolerance for bounding the number of point samples
			const unsigned int maxPointsPerNode,					///< [in] Maximum number of points / octree node
			const unsigned char maxDepth,							///< [in] Maximum depth of the octree
			const double irrad_scale,								///< [in] Irradiance scale factor
			const double geometric_scale,							///< [in] Geometric scale factor
			const bool multiplyBSDF,								///< [in] Should the BSDF be evaluated at the point of exitance?
			const bool regenerate,									///< [in] Regenerate the point set on reset calls?
			const char* shader,										///< [in] Shader to use for irradiance calculations
			const bool cache,										///< [in] Should the rasterizer state cache be used?
			const bool low_discrepancy,								///< [in] Should use a low discrepancy sequence during sample point generation?
			const double extinction[3]								///< [in] Extinction in mm^-1
			);

		bool AddDiffusionApproximationSubSurfaceScatteringShaderOp(
			const char* name,										///< [in] Name of the shaderop
			const unsigned int numPoints,							///< [in] Number of points to use in sampling
			const double error,										///< [in] Error tolerance for bounding the number of point samples
			const unsigned int maxPointsPerNode,					///< [in] Maximum number of points / octree node
			const unsigned char maxDepth,							///< [in] Maximum depth of the octree
			const double irrad_scale,								///< [in] Irradiance scale factor
			const double geometric_scale,							///< [in] Geometric scale factor
			const bool multiplyBSDF,								///< [in] Should the BSDF be evaluated at the point of exitance?
			const bool regenerate,									///< [in] Regenerate the point set on reset calls?
			const char* shader,										///< [in] Shader to use for irradiance calculations
			const bool cache,										///< [in] Should the rasterizer state cache be used?
			const bool low_discrepancy,								///< [in] Should use a low discrepancy sequence during sample point generation?
			const double scattering[3],								///< [in] Scattering coefficient in mm^-1
			const double absorption[3],								///< [in] Absorption coefficient in mm^-1
			const double ior,										///< [in] Index of refraction ratio
			const double g											///< [in] Scattering asymmetry
			);

		bool AddAreaLightShaderOp(
			const char* name,										///< [in] Name of the shaderop
			const double width,										///< [in] Width of the light source
			const double height,									///< [in] Height of the light source
			const double location[3],								///< [in] Where is the light source located
			const double dir[3],									///< [in] What is the light source focussed on
			const unsigned int samples,								///< [in] Number of samples to take
			const char* emm,										///< [in] Emission painter of this light
			const double power,										///< [in] Power scale
			const char* N,											///< [in] Phong factor for focussing the light
			const double hotSpot,									///< [in] Angle in radians of the light's hot spot
			const bool cache										///< [in] Should the rasterizer state cache be used?
			);

		bool AddTransparencyShaderOp(
			const char* name,										///< [in] Name of the shaderop
			const char* transparency,								///< [in] Transparency painter
			const bool one_sided									///< [in] One sided transparency only (ignore backfaces)
			);


		//
		// Adds Shaders
		//
		bool AddStandardShader(
			const char* name,										///< [in] Name of the shader
			const unsigned int count,								///< [in] Number of shaderops
			const char** shaderops									///< [in] All of the shaderops
			);

		bool AddAdvancedShader(
			const char* name,										///< [in] Name of the shader
			const unsigned int count,								///< [in] Number of shaderops
			const char** shaderops,									///< [in] All of the shaderops
			const unsigned int* mindepths,							///< [in] All of the minimum depths for the shaderops
			const unsigned int* maxdepths,							///< [in] All of the maximum depths for the shaderops
			const char* operations									///< [in] All the operations for the shaderops
			);

		bool AddDirectVolumeRenderingShader(
			const char* name,										///< [in] Name of the shader
			const char* szVolumeFilePattern,						///< [in] File pattern for volume data
			const unsigned int width,								///< [in] Width of the volume
			const unsigned int height,								///< [in] Height of the volume
			const unsigned int startz,								///< [in] Starting z value for volume data
			const unsigned int endz,								///< [in] Ending z value for the volume data
			const char accessor,									///< [in] Type of volume accessor
			const char gradient,									///< [in] Type of gradient estimator to use
			const char composite,									///< [in] Type of composite operation to use
			const double dThresholdStart,							///< [in] Start of ISO threshold value (for ISO renderings only)
			const double dThresholdEnd,								///< [in] End of ISO threshold value (for ISO renderings only)
			const char sampler,										///< [in] Type of sampler to use
			const unsigned int samples,								///< [in] Number of samples along the ray to take
			const char* transfer_red,								///< [in] Name of the transfer function for the red channel
			const char* transfer_green,								///< [in] Name of the transfer function for the green channel
			const char* transfer_blue,								///< [in] Name of the transfer function for the blue channel
			const char* transfer_alpha,								///< [in] Name of the transfer function for the alpha channel
			const char* iso_shader									///< [in] Shader to use for ISO surface rendering (optional)
			);

		bool AddSpectralDirectVolumeRenderingShader(
			const char* name,										///< [in] Name of the shader
			const char* szVolumeFilePattern,						///< [in] File pattern for volume data
			const unsigned int width,								///< [in] Width of the volume
			const unsigned int height,								///< [in] Height of the volume
			const unsigned int startz,								///< [in] Starting z value for volume data
			const unsigned int endz,								///< [in] Ending z value for the volume data
			const char accessor,									///< [in] Type of volume accessor
			const char gradient,									///< [in] Type of gradient estimator to use
			const char composite,									///< [in] Type of composite operation to use
			const double dThresholdStart,							///< [in] Start of ISO threshold value (for ISO renderings only)
			const double dThresholdEnd,								///< [in] End of ISO threshold value (for ISO renderings only)
			const char sampler,										///< [in] Type of sampler to use
			const unsigned int samples,								///< [in] Number of samples along the ray to take
			const char* transfer_alpha,								///< [in] Name of the transfer function for the alpha channel
			const char* transfer_spectral,							///< [in] Name of the 2D spectral transfer function
			const char* iso_shader									///< [in] Shader to use for ISO surface rendering (optional)
			);

		//
		// Sets Rasterization parameters
		//

		//! Sets the rasterizer type to be pixel based PEL
		bool SetPixelBasedPelRasterizer(
			const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
			const unsigned int numLumSamples,						///< [in] Number of samples / luminaire
			const unsigned int maxRecur,							///< [in] Maximum recursion level
			const double minImportance,								///< [in] Minimum importance to stop at
			const char* shader,										///< [in] The default shader
			const char* globalRadianceMap,							///< [in] Name of the painter for global IBL
			const bool bBackground,									///< [in] Is the radiance map a background object
			const double scale,										///< [in] How much to scale the radiance values
			const double orient[3],									///< [in] Euler angles for orienting the radiance map
			const char* pixelSampler,								///< [in] Type of sampling to use for the pixel sampler
			const double pixelSamplerParam,							///< [in] Parameter for the pixel sampler
			const char* luminarySampler,							///< [in] Type of sampling to use for luminaries
			const double luminarySamplerParam,						///< [in] Parameter for the luminary sampler
			const char* pixelFilter,								///< [in] Type of filtering to use for the pixels
			const double pixelFilterWidth,							///< [in] How wide is the pixel filter?
			const double pixelFilterHeight,							///< [in] How high is the pixel filter?
			const double pixelFilterParamA,							///< [in] Pixel filter parameter A
			const double pixelFilterParamB,							///< [in] Pixel filter parameter B
			const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
			const bool bUseIORStack,								///< [in] Should we use an index of refraction stack?
			const bool bChooseOnlyOneLight							///< [in] For the luminaire sampler only one random light is chosen for each sample
			);

		//! Sets the rasterizer type to be pixel based spectral integrating
		bool SetPixelBasedSpectralIntegratingRasterizer(
			const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
			const unsigned int numLumSamples,						///< [in] Number of samples / luminaire
			const unsigned int specSamples,							///< [in] Number of spectral samples / pixel
			const double lambda_begin,								///< [in] Wavelength to start sampling at
			const double lambda_end,								///< [in] Wavelength to finish sampling at
			const unsigned int num_wavelengths,						///< [in] Number of wavelengths to sample
			const unsigned int maxRecur,							///< [in] Maximum recursion level
			const double minImportance,								///< [in] Minimum importance to stop at
			const char* shader,										///< [in] The default shader
			const char* globalRadianceMap,							///< [in] Name of the painter for global IBL
			const bool bBackground,									///< [in] Is the radiance map a background object
			const double scale,										///< [in] How much to scale the radiance values
			const double orient[3],									///< [in] Euler angles for orienting the radiance map
			const char* pixelSampler,								///< [in] Type of sampling to use for the pixel sampler
			const double pixelSamplerParam,							///< [in] Parameter for the pixel sampler
			const char* luminarySampler,							///< [in] Type of sampling to use for luminaries
			const double luminarySamplerParam,						///< [in] Parameter for the luminary sampler
			const char* pixelFilter,								///< [in] Type of filtering to use for the pixels
			const double pixelFilterWidth,							///< [in] How wide is the pixel filter?
			const double pixelFilterHeight,							///< [in] How high is the pixel filter?
			const double pixelFilterParamA,							///< [in] Pixel filter parameter A
			const double pixelFilterParamB,							///< [in] Pixel filter parameter B
			const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
			const bool bUseIORStack,								///< [in] Should we use an index of refraction stack?
			const bool bChooseOnlyOneLIght,							///< [in] For the luminaire sampler only one random light is chosen for each sample
			const bool bIntegrateRGB,								///< [in] Should we use the CIE XYZ spd functions or will they be specified now?
			const unsigned int numSPDvalues,						///< [in] Number of values in the RGB SPD arrays
			const double rgb_spd_frequencies[],						///< [in] Array that contains the RGB SPD frequencies
			const double rgb_spd_r[],								///< [in] Array that contains the RGB SPD amplitudes for red
			const double rgb_spd_g[],								///< [in] Array that contains the RGB SPD amplitudes for green
			const double rgb_spd_b[]								///< [in] Array that contains the RGB SPD amplitudes for blue
			);

		//! Sets the rasterizer type to be adaptive pixel based PEL
		bool SetAdaptivePixelBasedPelRasterizer(
			const unsigned int numMinPixelSamples,					///< [in] Minimum or base number of samples to start with
			const unsigned int numMaxPixelSamples,					///< [in] Maximum number of samples to go to
			const unsigned int numSteps,							///< [in] Number of steps to maximum sampling level
			const unsigned int numLumSamples,						///< [in] Number of samples / luminaire
			const double threshold,									///< [in] Threshold at which to stop sampling further
			const bool bOutputSamples,								///< [in] Should the renderer show how many samples rather than an image
			const unsigned int maxRecur,							///< [in] Maximum recursion level
			const double minImportance,								///< [in] Minimum importance to stop at
			const char* shader,										///< [in] The default shader
			const char* globalRadianceMap,							///< [in] Name of the painter for global IBL
			const bool bBackground,									///< [in] Is the radiance map a background object
			const double scale,										///< [in] How much to scale the radiance values
			const double orient[3],									///< [in] Euler angles for orienting the radiance map
			const char* pixelSampler,								///< [in] Type of sampling to use for the pixel sampler
			const double pixelSamplerParam,							///< [in] Parameter for the pixel sampler
			const char* luminarySampler,							///< [in] Type of sampling to use for luminaries
			const double luminarySamplerParam,						///< [in] Parameter for the luminary sampler
			const char* pixelFilter,								///< [in] Type of filtering to use for the pixels
			const double pixelFilterWidth,							///< [in] How wide is the pixel filter?
			const double pixelFilterHeight,							///< [in] How high is the pixel filter?
			const double pixelFilterParamA,							///< [in] Pixel filter parameter A
			const double pixelFilterParamB,							///< [in] Pixel filter parameter B
			const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
			const bool bUseIORStack,								///< [in] Should we use an index of refraction stack?
			const bool bChooseOnlyOneLight							///< [in] For the luminaire sampler only one random light is chosen for each sample
			);

		//! Sets the rasterizer type to be contrast AA pixel pel
		bool SetContrastAAPixelBasedPelRasterizer(
			const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
			const unsigned int numLumSamples,						///< [in] Number of samples / luminaire
			const unsigned int maxRecur,							///< [in] Maximum recursion level
			const double minImportance,								///< [in] Minimum importance to stop at
			const char* shader,										///< [in] The default shader
			const char* globalRadianceMap,							///< [in] Name of the painter for global IBL
			const bool bBackground,									///< [in] Is the radiance map a background object
			const double scale,										///< [in] How much to scale the radiance values
			const double orient[3],									///< [in] Euler angles for orienting the radiance map
			const char* pixelSampler,								///< [in] Type of sampling to use for the pixel sampler
			const double pixelSamplerParam,							///< [in] Parameter for the pixel sampler
			const char* luminarySampler,							///< [in] Type of sampling to use for luminaries
			const double luminarySamplerParam,						///< [in] Parameter for the luminary sampler
			const char* pixelFilter,								///< [in] Type of filtering to use for the pixels
			const double pixelFilterWidth,							///< [in] How wide is the pixel filter?
			const double pixelFilterHeight,							///< [in] How high is the pixel filter?
			const double pixelFilterParamA,							///< [in] Pixel filter parameter A
			const double pixelFilterParamB,							///< [in] Pixel filter parameter B
			const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
			const bool bUseIORStack,								///< [in] Should we use an index of refraction stack?
			const bool bChooseOnlyOneLight,							///< [in] For the luminaire sampler only one random light is chosen for each sample
			const double contrast_threshold[3],						///< [in] Contrast threshold for each color component
			const bool show_samples									///< [in] Should the number of samples be taken be shown?
			);


		//
		// Adds rasterizer outputs
		//

		//! Creates a file rasterizer output
		//! This should be called after a rasterizer has been set
		//! Note that setting a new rasterizer after adding file rasterizer outputs will
		//! delete existing outputs
		/// \return TRUE if successful, FALSE otherwise
		bool AddFileRasterizerOutput(
			const char* szPattern,									///< [in] File pattern
			const bool bMultiple,									///< [in] Output multiple files (for animations usually)
			const char type,										///< [in] Type of file
																	///		0 - TGA
																	///		1 - PPM
																	///		2 - PNG
																	///		3 - HDR
																	///     4 - TIFF
																	///		5 - RGBEA
			const unsigned char bpp,								///< [in] Bits / pixel for the file
			const char color_space									///< [in] Color space to apply
																	///		0 - Rec709 RGB linear
																	///		1 - sRGB profile
																	///		2 - ROMM RGB (ProPhotoRGB) linear
																	///		3 - ROMM RGB (ProPhotoRGB) non-linear
			);

		//! Creates a user callback rasterizer output
		//! This should be called after a rasterizer has been set
		//! Note that no attemps at reference counting are made, the user
		//! better not go delete the object
		bool AddCallbackRasterizerOutput(
			IJobRasterizerOutput* pObj
			);


		//
		// Photon mapping
		//

		//! Sets the gather parameters for the caustic pel photon map
		/// \return TRUE if successful, FALSE otherwise
		bool SetCausticPelGatherParameters(
			const double radius,							///< [in] Search radius
			const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
			const unsigned int min,							///< [in] Minimum number of photons in the gather
			const unsigned int max							///< [in] Maximum number of photons in the gather
			);

		//! Sets the gather parameters for the global pel photon map
		/// \return TRUE if successful, FALSE otherwise
		bool SetGlobalPelGatherParameters(
			const double radius,							///< [in] Search radius
			const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
			const unsigned int min,							///< [in] Minimum number of photons in the gather
			const unsigned int max							///< [in] Maximum number of photons in the gather
			);

		//! Sets the gather parameters for the translucent pel photon map
		/// \return TRUE if successful, FALSE otherwise
		bool SetTranslucentPelGatherParameters(
			const double radius,							///< [in] Search radius
			const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
			const unsigned int min,							///< [in] Minimum number of photons in the gather
			const unsigned int max							///< [in] Maximum number of photons in the gather
			);

		//! Sets the gather parameters for the caustic spectral photon map
		/// \return TRUE if successful, FALSE otherwise
		bool SetCausticSpectralGatherParameters(
			const double radius,							///< [in] Search radius
			const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
			const unsigned int min,							///< [in] Minimum number of photons in the gather
			const unsigned int max,							///< [in] Maximum number of photons in the gather
			const double nm_range							///< [in] Range of wavelengths to search for a NM irradiance estimate
			);

		//! Sets the gather parameters for the global spectral photon map
		/// \return TRUE if successful, FALSE otherwise
		bool SetGlobalSpectralGatherParameters(
			const double radius,							///< [in] Search radius
			const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
			const unsigned int min,							///< [in] Minimum number of photons in the gather
			const unsigned int max,							///< [in] Maximum number of photons in the gather
			const double nm_range							///< [in] Range of wavelengths to search for a NM irradiance estimate
			);

		//! Sets the gather parameters for the shadow photon map
		/// \return TRUE if successful, FALSE otherwise
		bool SetShadowGatherParameters(
			const double radius,							///< [in] Search radius
			const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
			const unsigned int min,							///< [in] Minimum number of photons in the gather
			const unsigned int max							///< [in] Maximum number of photons in the gather
			);

		//! Sets the irradiance cache parameters
		/// \return TRUE if successful, FALSE otherwise
		bool SetIrradianceCacheParameters(
			const unsigned int size,						///< [in] Size of the cache
			const double tolerance,							///< [in] Tolerance of the cache
			const double min_spacing,						///< [in] Minimum seperation
			const double max_spacing						///< [in] Maximum seperation
			);

		//! Saves the caustic pel photon map to disk
		bool SaveCausticPelPhotonmap(
			const char* file_name							///< [in] Name of the file to save it to
			);

		//! Saves the global pel photon map to disk
		bool SaveGlobalPelPhotonmap(
			const char* file_name							///< [in] Name of the file to save it to
			);

		//! Saves the translucent pel photon map to disk
		bool SaveTranslucentPelPhotonmap(
			const char* file_name							///< [in] Name of the file to save it to
			);

		//! Saves the caustic spectral photon map to disk
		bool SaveCausticSpectralPhotonmap(
			const char* file_name							///< [in] Name of the file to save it to
			);

		//! Saves the global spectral photon map to disk
		bool SaveGlobalSpectralPhotonmap(
			const char* file_name							///< [in] Name of the file to save it to
			);

		//! Saves the shadow photon map to disk
		bool SaveShadowPhotonmap(
			const char* file_name							///< [in] Name of the file to save it to
			);

		//! Loads the caustic pel photon map from disk
		bool LoadCausticPelPhotonmap(
			const char* file_name							///< [in] Name of the file to load from
			);

		//! Loads the global pel photon map from disk
		bool LoadGlobalPelPhotonmap(
			const char* file_name							///< [in] Name of the file to load from
			);

		//! Loads the translucent pel photon map from disk
		bool LoadTranslucentPelPhotonmap(
			const char* file_name							///< [in] Name of the file to load from
			);

		//! Loads the caustic spectral photon map from disk
		bool LoadCausticSpectralPhotonmap(
			const char* file_name							///< [in] Name of the file to load from
			);

		//! Loads the caustic spectral photon map from disk
		bool LoadGlobalSpectralPhotonmap(
			const char* file_name							///< [in] Name of the file to load from
			);

		//! Loads the shadow photon map from disk
		bool LoadShadowPhotonmap(
			const char* file_name							///< [in] Name of the file to load from
			);

		//
		// Commands
		//

		//! Shoots caustic photons and populates the caustic pel photon map
		/// \return TRUE if successful, FALSE otherwise
		bool ShootCausticPelPhotons(
			const unsigned int num,							///< [in] Number of photons to acquire
			const double power_scale,						///< [in] How much to scale light power by
			const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
			const double minImportance,						///< [in] Minimum importance when a photon is discarded
			const bool branch,								///< [in] Should the tracer branch or follow a single path?
			const bool reflect,								///< [in] Should we trace reflected rays?
			const bool refract,								///< [in] Should we trace refracted rays?
			const bool nonmeshlights,						///< [in] Should we shoot from non mesh based lights?
			const bool useiorstack,							///< [in] Should the ray caster use a index of refraction stack?
			const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
			const bool regenerate							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
			);

		//! Shoots global photons and populates the global pel photon map
		/// \return TRUE if successful, FALSE otherwise
		bool ShootGlobalPelPhotons(
			const unsigned int num,							///< [in] Number of photons to acquire
			const double power_scale,						///< [in] How much to scale light power by
			const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
			const double minImportance,						///< [in] Minimum importance when a photon is discarded
			const bool branch,								///< [in] Should the tracer branch or follow a single path?
			const bool nonmeshlights,						///< [in] Should we shoot from non mesh based lights?
			const bool useiorstack,							///< [in] Should the ray caster use a index of refraction stack?
			const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
			const bool regenerate							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
			);

		//! Shoots translucent photons and populates the translucent pel photon map
		/// \return TRUE if successful, FALSE otherwise
		bool ShootTranslucentPelPhotons(
			const unsigned int num,							///< [in] Number of photons to acquire
			const double power_scale,						///< [in] How much to scale light power by
			const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
			const double minImportance,						///< [in] Minimum importance when a photon is discarded
			const bool reflect,								///< [in] Should we trace reflected rays?
			const bool refract,								///< [in] Should we trace refracted rays?
			const bool direct_translucent,					///< [in] Should we trace translucent primary interaction rays?
			const bool nonmeshlights,						///< [in] Should we shoot from non mesh based lights?
			const bool useiorstack,							///< [in] Should the ray caster use a index of refraction stack?
			const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
			const bool regenerate							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
			);

		//! Shoots caustic photons and populates the caustic spectral photon map
		/// \return TRUE if successful, FALSE otherwise
		bool ShootCausticSpectralPhotons(
			const unsigned int num,							///< [in] Number of photons to acquire
			const double power_scale,						///< [in] How much to scale light power by
			const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
			const double minImportance,						///< [in] Minimum importance when a photon is discarded
			const double nm_begin,							///< [in] Wavelength to start shooting photons at
			const double nm_end,							///< [in] Wavelength to end shooting photons at
			const unsigned int num_wavelengths,				///< [in] Number of wavelengths to shoot photons at
			const bool useiorstack,							///< [in] Should the ray caster use a index of refraction stack?
			const bool branch,								///< [in] Should the tracer branch or follow a single path?
			const bool reflect,								///< [in] Should we trace reflected rays?
			const bool refract,								///< [in] Should we trace refracted rays?
			const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
			const bool regenerate							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
			);

		//! Shoots global photons and populates the global spectral photon map
		/// \return TRUE if successful, FALSE otherwise
		bool ShootGlobalSpectralPhotons(
			const unsigned int num,							///< [in] Number of photons to acquire
			const double power_scale,						///< [in] How much to scale light power by
			const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
			const double minImportance,						///< [in] Minimum importance when a photon is discarded
			const double nm_begin,							///< [in] Wavelength to start shooting photons at
			const double nm_end,							///< [in] Wavelength to end shooting photons at
			const unsigned int num_wavelengths,				///< [in] Number of wavelengths to shoot photons at
			const bool useiorstack,							///< [in] Should the ray caster use a index of refraction stack?
			const bool branch,								///< [in] Should the tracer branch or follow a single path?
			const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
			const bool regenerate							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
			);

		//! Shoots shadow photons and populates the shadow photon map
		/// \return TRUE if successful, FALSE otherwise
		bool ShootShadowPhotons(
			const unsigned int num,							///< [in] Number of photons to acquire
			const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
			const bool regenerate							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
			);

		//! Predicts the amount of time in ms it will take to rasterize the current scene
		/// \return TRUE if successful, FALSE otherwise
		bool PredictRasterizationTime(
			unsigned int num,								///< [in] Number of samples to take when determining how long it will take (higher is more accurate)
			unsigned int* ms,								///< [out] Amount of in ms it would take to rasterize
			unsigned int* actual							///< [out] Actual time it took to do the predicted kernel
			);

		//! Rasterizes the entire scene
		/// \return TRUE if successful, FALSE otherwise
		bool Rasterize(
			);

		//! Rasterizes the scene in this region.  The region values are inclusive!
		/// \return TRUE if successful, FALSE otherwise
		bool RasterizeRegion(
			const unsigned int left,						///< [in] Left most pixel
			const unsigned int top,							///< [in] Top most scanline
			const unsigned int right,						///< [in] Right most pixel
			const unsigned int bottom						///< [in] Bottom most scanline
			);

		//! Rasterizes an animation
		/// \return TRUE if successful, FALSE otherwise
		bool RasterizeAnimation(
			const double time_start,						///< [in] Scene time to start rasterizing at
			const double time_end,							///< [in] Scene time to finish rasterizing
			const unsigned int num_frames,					///< [in] Number of frames to rasterize
			const bool do_fields,							///< [in] Should the rasterizer do fields?
			const bool invert_fields						///< [in] Should the fields be temporally inverted?
			);

		//! Rasterizes an animation using the global preset options
		/// \return TRUE if successful, FALSE otherwise
		bool RasterizeAnimationUsingOptions(
			);

		//! Rasterizes a frame of an animation using the global preset options
		/// \return TRUE if successful, FALSE otherwise
		bool RasterizeAnimationUsingOptions(
			const unsigned int frame						///< [in] The frame to rasterize
			);

		//
		// Transformation of elements
		//

		//! Sets the a given object's position
		/// \return TRUE if successful, FALSE otherwise
		bool SetObjectPosition(
			const char* name,								///< [in] Name of the object
			const double pos[3]								///< [in] Position of the object
			);

		//! Sets a given object's orientation
		/// \return TRUE if successful, FALSE otherwise
		bool SetObjectOrientation(
			const char* name,								///< [in] Name of the object
			const double orient[3]							///< [in] Orientation of the object
			);

		//! Sets a given object's scale
		/// \return TRUE if successful, FALSE otherwise
		bool SetObjectScale(
			const char* name,								///< [in] Name of the object
			const double scale								///< [in] Scaling of the object
			);

		//
		// Object modification functions
		//

		//! Sets the UV generator for an object
		/// \return TRUE if successful, FALSE otherwise
		bool SetObjectUVToSpherical(
			const char* name,								///< [in] Name of the object
			const double radius								///< [in] Radius of the sphere
			);

		//! Sets the UV generator for an object
		/// \return TRUE if successful, FALSE otherwise
		bool SetObjectUVToBox(
			const char* name,								///< [in] Name of the object
			const double width,								///< [in] Width of the box
			const double height,							///< [in] Height of the box
			const double depth								///< [in] Depth of the box
			);

		//! Sets the UV generator for an object
		/// \return TRUE if successful, FALSE otherwise
		bool SetObjectUVToCylindrical(
			const char* name,								///< [in] Name of the object
			const double radius,							///< [in] Radius of the cylinder
			const char axis,								///< [in] Axis the cylinder is sitting on
			const double size								///< [in] Size of the cylinder
			);

		//! Sets the object's surface intersection threshold
		/// \return TRUE if successful, FALSE otherwise
		bool SetObjectIntersectionError(
			const char* name,								///< [in] Name of the object
			const double error								///< [in] Threshold of error
			);

		//
		// Removal of objects
		//

		//! Removes the given painter from the scene
		/// \return TRUE if successful, FALSE otherwise
		bool RemovePainter(
			const char* name								///< [in] Name of the painter to remove
			);

		//! Removes the given material from the scene
		/// \return TRUE if successful, FALSE otherwise
		bool RemoveMaterial(
			const char* name								///< [in] Name of the material to remove
			);

		//! Removes the given geometry from the scene
		/// \return TRUE if successful, FALSE otherwise
		bool RemoveGeometry(
			const char* name								///< [in] Name of the geometry to remove
			);

		//! Removes the given object from the scene
		/// \return TRUE if successful, FALSE otherwise
		bool RemoveObject(
			const char* name								///< [in] Name of the object to remove
			);

		//! Removes the given light from the scene
		/// \return TRUE if successful, FALSE otherwise
		bool RemoveLight(
			const char* name								///< [in] Name of the light to remove
			);

		//! Removes the given modifier from the scene
		/// \return TRUE if successful, FALSE otherwise
		bool RemoveModifier(
			const char* name								///< [in] Name of the modifer to remove
			);

		//! Clears the entire scene, resets everything back to defaults
		/// \return TRUE if successful, FALSE otherwise
		bool ClearAll(
			);

		//! Removes all the rasterizer outputs
		/// \return TRUE if successful, FALSE otherwise
		bool RemoveRasterizerOutputs(
			);


		//! Loading an ascii scene description
		/// \return TRUE if successful, FALSE otherwise
		bool LoadAsciiScene(
			const char* filename							///< [in] Name of the file containing the scene
			);

		//! Runs an ascii script
		/// \return TRUE if successful, FALSE otherwise
		bool RunAsciiScript(
			const char* filename							///< [in] Name of the file containing the script
			);

		//! Tells us whether anything is keyframed
		bool AreThereAnyKeyframedObjects();

		//! Adds a keyframe for the specified element
		bool AddKeyframe(
			const char* element_type,						///< [in] Type of element to keyframe (ie. camera, painter, geometry, object...)
			const char* element,							///< [in] Name of the element to keyframe
			const char* param,								///< [in] Name of the parameter to keyframe
			const char* value,								///< [in] Value at this keyframe
			const double time,								///< [in] Time of the keyframe
			const char* interp,								///< [in] Type of interpolation to use between this keyframe and the next
			const char* interp_params						///< [in] Parameters to pass to the interpolator (this can be NULL)
			);

		//! Sets animation rasterization options
		//! Basically everything that can be passed to RasterizeAnimation can be passed here
		//! Then you can just call RasterizeAnimationUsingOptions
		/// \return TRUE if successful, FALSE otherwise
		bool SetAnimationOptions(
			const double time_start,						///< [in] Scene time to start rasterizing at
			const double time_end,							///< [in] Scene time to finish rasterizing
			const unsigned int num_frames,					///< [in] Number of frames to rasterize
			const bool do_fields,							///< [in] Should the rasterizer do fields?
			const bool invert_fields						///< [in] Should the fields be temporally inverted?
			);

		//! Sets progress class to report progress for anything we do
		void SetProgress(
			IProgressCallback* pProgress				///< [in] The progress function
			);
	};
}

#endif
