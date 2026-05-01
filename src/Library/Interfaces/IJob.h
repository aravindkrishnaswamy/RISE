//////////////////////////////////////////////////////////////////////
//
//  IJob.h - Interface to a job, which is one of the highest level
//    constructs.  A job is typically used by a parser to
//    load a scene and rasterizes it.  Internally it strictly uses
//    only the RISE API.
//
//  Looking at the Job implementation is a good way of learning
//  how to use the RISE API.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 21, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IJOB_
#define IJOB_

#include "IReference.h"
#include "IProgressCallback.h"
#include "IJobRasterizerOutput.h"
#include <string>
#include "../Utilities/PathGuidingField.h"
#include "../Utilities/AdaptiveSamplingConfig.h"
#include "../Utilities/StabilityConfig.h"
#include "../Utilities/ProgressiveConfig.h"
#include "../Utilities/PixelFilterConfig.h"
#include "../Utilities/SMSConfig.h"
#include "../Utilities/SpectralConfig.h"
#include "../Utilities/RadianceMapConfig.h"
#include "../Utilities/OidnConfig.h"

namespace RISE
{
	// Forward declaration to avoid pulling in the full
	// ITriangleMeshGeometry.h dependency chain.  Used by
	// AddPrebuiltTriangleMeshGeometry, declared near the bottom.
	class ITriangleMeshGeometryIndexed;

	//! One entry in a parallel-decode batch passed to
	//! IJob::AddTexturePaintersBatch.  Either filePath xor (bytes,numBytes)
	//! is set — never both, never neither.  See AddTexturePaintersBatch
	//! for full semantics.
	struct TexturePainterBatchRequest
	{
		const char*           name;          ///< Painter name to register under
		const char*           filePath;      ///< NULL if reading from in-memory bytes
		const unsigned char*  bytes;         ///< NULL if reading from filePath
		size_t                numBytes;      ///< 0 if reading from filePath
		char                  format;        ///< 0 = PNG, 1 = JPEG
		char                  colorSpace;    ///< 0=Rec709 linear, 1=sRGB, 2=ROMM linear, 3=ProPhoto
		char                  filterType;    ///< 0=NN, 1=Bilinear, 2=CRBicubic, 3=BSBicubic
		char                  wrap_s;        ///< U-axis wrap mode (0=clamp, 1=repeat, 2=mirrored repeat); see eRasterWrapMode
		char                  wrap_t;        ///< V-axis wrap mode (same encoding)
		bool                  lowmemory;     ///< Defer color-space convert to first sample (reduces RAM at the cost of slightly slower sampling)
		double                scale[3];      ///< Per-channel scale (multiply at decode time)
		double                shift[3];      ///< Per-channel shift (add at decode time)
	};

	//! Job - This is used to simplify the creation of a job, all things can be
	//! easily accessed by name, no need to keep track of managers and such
	class IJob : public virtual IReference
	{
	protected:
		IJob(){};
		virtual ~IJob(){};

	public:
		//
		// Core settings
		//

		//! Resets the acceleration structure
		//! WARNING!  Call this before adding objects, otherwise you will LOSE them!
		//! \return TRUE if successful, FALSE otherwise
		virtual bool SetPrimaryAcceleration(
			const bool bUseBSPtree,									///< [in] Use BSP trees for spatial partitioning
			const bool bUseOctree,									///< [in] Use Octrees for spatial partitioning
			const unsigned int nMaxObjectsPerNode,					///< [in] Maximum number of elements / node
			const unsigned int nMaxTreeDepth						///< [in] Maximum tree depth
			) = 0;

		//! Sets the light-sample Russian roulette threshold.
		//! When > 0, weak mesh-luminary shadow samples are
		//! probabilistically terminated before the shadow ray.
		//! \return TRUE if successful, FALSE otherwise
		virtual bool SetLightSampleRRThreshold(
			const double threshold									///< [in] RR threshold (0=disabled)
			) = 0;


		//
		// Cameras — multi-camera contract.
		//
		// A scene holds many cameras keyed by name in an
		// ICameraManager (Scene::GetCameras()), with one designated
		// active.  Each AddXxxCamera registers a new camera under
		// `name` AND makes it active by policy ("last added wins").
		// SetActiveCamera switches which camera the rasterizer draws
		// through; RemoveCamera unregisters one and auto-promotes
		// (lexicographic first-remaining) if the removed camera was
		// active.
		//
		// Names follow the same convention as painters / materials:
		// unique within the manager (duplicate-add returns false).
		// The parser auto-suffixes unnamed camera chunks
		// ("default", "default_1", ...) so existing single-camera
		// scenes parse unchanged.
		//

		//! Adds a pinhole camera
		/// \return TRUE if successful, FALSE on null/empty name or
		/// duplicate name.
		virtual bool AddPinholeCamera(
			const char* name,										///< [in] Name to register the camera under
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
			) = 0;

		//! Adds a pinhole camera oriented via an orthonormal basis
		virtual bool AddPinholeCameraONB(
			const char* name,										///< [in] Name to register the camera under
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
			) = 0;

		//! Adds a thin-lens camera.  Photographic parameters: sensor
		/// size + focal length + f-stop + focus distance.
		///
		/// Units (Phase 1.2):
		///   - sensor_size, focal_length, shift_x, shift_y are in MM.
		///   - focus_distance is in scene units (matches geometry coords).
		///   - sceneUnitMeters bridges the two: meters per scene unit
		///     (default 1.0 = metres scene; 0.001 = mm; 0.0254 = inches).
		///   - The camera converts mm → scene-units internally so the
		///     lens equation is unit-consistent with focus_distance.
		virtual bool AddThinlensCamera(
			const char* name,										///< [in] Name to register the camera under
			const double ptLocation[3],								///< [in] Absolute location of where the camera is located
			const double ptLookAt[3], 								///< [in] Absolute point the camera is looking at
			const double vUp[3],									///< [in] Up vector of the camera
			const double sensorSize,								///< [in] Sensor width (mm)
			const double focalLength,								///< [in] Lens focal length (mm)
			const double fstop,										///< [in] f-number (dimensionless; aperture diameter = focalLength/fstop)
			const double focusDistance,								///< [in] Focus plane distance (scene units; must be > focal_in_scene_units)
			const double sceneUnitMeters,							///< [in] Meters per scene unit (1.0 = metres scene; 0.001 = mm scene)
			const unsigned int xres,								///< [in] X resolution of virtual screen
			const unsigned int yres,								///< [in] Y resolution of virtual screen
			const double pixelAR,									///< [in] Pixel aspect ratio
			const double exposure,									///< [in] Exposure time of the camera
			const double scanningRate,								///< [in] Rate at which each scanline is recorded
			const double pixelRate,									///< [in] Rate at which each pixel is recorded
			const double orientation[3],							///< [in] Orientation (Pitch,Roll,Yaw)
			const double target_orientation[2],						///< [in] Orientation relative to a target
			const unsigned int apertureBlades,						///< [in] Polygonal aperture blades; 0 = perfect disk
			const double apertureRotation,							///< [in] Polygon rotation (radians)
			const double anamorphicSqueeze,							///< [in] Aperture x-axis scale (1.0 = circular)
			const double tiltX,										///< [in] Focal-plane tilt around x-axis (radians); 0 = perpendicular focus
			const double tiltY,										///< [in] Focal-plane tilt around y-axis (radians); 0 = perpendicular focus
			const double shiftX,									///< [in] Lens shift along x (mm); 0 = centered
			const double shiftY										///< [in] Lens shift along y (mm); 0 = centered
			) = 0;

		//! Adds a fisheye camera
		virtual bool AddFisheyeCamera(
			const char* name,										///< [in] Name to register the camera under
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
			) = 0;

		//! Adds an orthographic camera
		virtual bool AddOrthographicCamera(
			const char* name,										///< [in] Name to register the camera under
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
			) = 0;

		//! Designates the named camera as active.  Returns false if
		/// the name isn't registered with the scene's camera manager.
		virtual bool SetActiveCamera(
			const char* name										///< [in] Name of camera to make active
			) = 0;

		//! Reads back the currently-active camera name.  Returns an empty
		/// string when the scene has no cameras (and therefore no active
		/// camera).  Used by importers that need to snapshot+restore the
		/// active camera around a multi-camera registration step (see
		/// GLTFSceneImporter::ImportScene — a `gltf_import` chunk with
		/// `import_cameras TRUE` registers N cameras, each auto-promoting
		/// to active under the last-added-wins rule; the importer uses
		/// this query to detect a pre-existing user camera and preserve
		/// it across the walk instead of letting the asset's last DFS
		/// camera silently take over).
		virtual std::string GetActiveCameraName() const = 0;

		//! Removes the named camera.  If the removed camera was
		/// active, auto-promotes a remaining camera (or leaves the
		/// scene cameraless if none remain).
		virtual bool RemoveCamera(
			const char* name										///< [in] Name of camera to remove
			) = 0;


		//
		// Adding painters
		//

		//! Adds a simple checker painter
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddCheckerPainter(
									const char* name,				///< [in] Name of the painter
									const double size,				///< [in] Size of the checkers in texture mapping units
									const char* pa,					///< [in] First painter
									const char* pb					///< [in] Second painter
									) = 0;


		//! Adds a lines painter
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddLinesPainter(
									const char* name,				///< [in] Name of the painter
									const double size,				///< [in] Size of the lines in texture mapping units
									const char* pa,					///< [in] First painter
									const char* pb,					///< [in] Second painter
									const bool bvert				///< [in] Are the lines vertical?
									) = 0;

		//! Adds a mandelbrot fractal painter
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddMandelbrotFractalPainter(
									const char* name,				///< [in] Name of the painter
									const char* pa,					///< [in] First painter
									const char* pb,					///< [in] Second painter
									const double lower_x,
									const double upper_x,
									const double lower_y,
									const double upper_y,
									const double exp
									) = 0;

		//! Adds a 2D perlin noise painter
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddPerlin2DPainter(
									const char* name,				///< [in] Name of the painter
									const double dPersistence,		///< [in] Persistence
									const unsigned int nOctaves,	///< [in] Number of octaves to use in noise generation
									const char* pa,					///< [in] First painter
									const char* pb,					///< [in] Second painter
									const double vScale[2],			///< [in] How much to scale the function by
									const double vShift[2]			///< [in] How much to shift the function by
									) = 0;

		//! Adds a sum-of-sines water-wave painter (Gerstner height variant).
		//! Intended to drive a DisplacedGeometry via the IFunction2D hook.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddGerstnerWavePainter(
									const char* name,					///< [in] Name of the painter
									const char* pa,						///< [in] Trough color painter
									const char* pb,						///< [in] Crest color painter
									const unsigned int numWaves,		///< [in] Number of sine waves to sum
									const double medianWavelength,		///< [in] Central wavelength (UV units)
									const double wavelengthRange,		///< [in] Multiplicative range; span is [median/range, median*range]
									const double medianAmplitude,		///< [in] Amplitude at median wavelength
									const double amplitudePower,		///< [in] A_i = medianAmplitude * (lambda_i/medianWavelength)^power
									const double windDir[2],			///< [in] Wind direction (x, y); normalized internally
									const double directionalSpread,		///< [in] Per-wave angle jitter (radians)
									const double dispersionSpeed,		///< [in] Multiplies sqrt(g*k); tune motion speed
									const unsigned int seed,			///< [in] RNG seed
									const double time					///< [in] Simulation time
									) = 0;

		//! Adds a 2D perlin noise painter
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddPerlin3DPainter(
									const char* name,				///< [in] Name of the painter
									const double dPersistence,		///< [in] Persistence
									const unsigned int nOctaves,	///< [in] Number of octaves to use in noise generation
									const char* pa,					///< [in] First painter
									const char* pb,					///< [in] Second painter
									const double vScale[3],			///< [in] How much to scale the function by
									const double vShift[3]			///< [in] How much to shift the function by
									) = 0;

		virtual bool AddWavelet3DPainter(
									const char* name,
									const unsigned int nTileSize,
									const double dPersistence,
									const unsigned int nOctaves,
									const char* pa,
									const char* pb,
									const double vScale[3],
									const double vShift[3]
									) = 0;

		virtual bool AddReactionDiffusion3DPainter(
									const char* name,
									const unsigned int nGridSize,
									const double dDa,
									const double dDb,
									const double dFeed,
									const double dKill,
									const unsigned int nIterations,
									const char* pa,
									const char* pb,
									const double vScale[3],
									const double vShift[3]
									) = 0;

		virtual bool AddGabor3DPainter(
									const char* name,
									const double dFrequency,
									const double dBandwidth,
									const double vOrientation[3],
									const double dImpulseDensity,
									const char* pa,
									const char* pb,
									const double vScale[3],
									const double vShift[3]
									) = 0;

		virtual bool AddSimplex3DPainter(
									const char* name,
									const double dPersistence,
									const unsigned int nOctaves,
									const char* pa,
									const char* pb,
									const double vScale[3],
									const double vShift[3]
									) = 0;

		virtual bool AddSDF3DPainter(
									const char* name,
									const unsigned int nType,
									const double dParam1,
									const double dParam2,
									const double dParam3,
									const double dShellThickness,
									const double dNoiseAmplitude,
									const double dNoiseFrequency,
									const char* pa,
									const char* pb,
									const double vScale[3],
									const double vShift[3]
									) = 0;

		virtual bool AddCurlNoise3DPainter(
									const char* name,
									const double dPersistence,
									const unsigned int nOctaves,
									const double dEpsilon,
									const char* pa,
									const char* pb,
									const double vScale[3],
									const double vShift[3]
									) = 0;

		virtual bool AddDomainWarp3DPainter(
									const char* name,
									const double dPersistence,
									const unsigned int nOctaves,
									const double dWarpAmplitude,
									const unsigned int nWarpLevels,
									const char* pa,
									const char* pb,
									const double vScale[3],
									const double vShift[3]
									) = 0;

		//! Adds a 3D Perlin-Worley hybrid (cloud noise) painter
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddPerlinWorley3DPainter(
									const char* name,				///< [in] Name of the painter
									const double dPersistence,		///< [in] Persistence for Perlin FBM
									const unsigned int nOctaves,	///< [in] Number of octaves for Perlin
									const double dWorleyJitter,		///< [in] Worley jitter [0,1]
									const double dBlend,			///< [in] Blend: 0=Perlin, 1=Worley
									const char* pa,					///< [in] First painter
									const char* pb,					///< [in] Second painter
									const double vScale[3],			///< [in] Scale
									const double vShift[3]			///< [in] Shift
									) = 0;

		//! Adds a 3D Worley (cellular) noise painter
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddWorley3DPainter(
									const char* name,				///< [in] Name of the painter
									const double dJitter,			///< [in] Jitter amount [0,1]
									const unsigned int nMetric,		///< [in] Distance metric (0=Euclidean, 1=Manhattan, 2=Chebyshev)
									const unsigned int nOutput,		///< [in] Output mode (0=F1, 1=F2, 2=F2-F1)
									const char* pa,					///< [in] First painter
									const char* pb,					///< [in] Second painter
									const double vScale[3],			///< [in] How much to scale the function by
									const double vShift[3]			///< [in] How much to shift the function by
									) = 0;

		//! Adds a 3D turbulence noise painter
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddTurbulence3DPainter(
									const char* name,				///< [in] Name of the painter
									const double dPersistence,		///< [in] Persistence
									const unsigned int nOctaves,	///< [in] Number of octaves to use in noise generation
									const char* pa,					///< [in] First painter
									const char* pb,					///< [in] Second painter
									const double vScale[3],			///< [in] How much to scale the function by
									const double vShift[3]			///< [in] How much to shift the function by
									) = 0;

		//! Adds a spectral color painter
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddSpectralColorPainter(
									const char* name,				///< [in] Name of the painter
									const double amplitudes[],		///< [in] Array that contains the amplitudes
									const double frequencies[],		///< [in] Array that contains the frequencies for the amplitudes
									const double lambda_begin,		///< [in] Begining of the spectral packet
									const double lambda_end,		///< [in] End of the spectral packet
									const unsigned int numfreq,		///< [in] Number of frequencies in the array
									const double scale				///< [in] How much to scale the amplitudes by
									) = 0;

		//! Adds a texture painter
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddPNGTexturePainter(
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
									const double shift[3],			///< [in] Shift factor for color values
									const char wrap_s = 0,			///< [in] U-axis wrap mode (0 = clamp [legacy default], 1 = repeat, 2 = mirrored repeat).  See eRasterWrapMode.
									const char wrap_t = 0			///< [in] V-axis wrap mode (same encoding as wrap_s)
									) = 0;

		//! Adds a painter that paints a uniform color
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddUniformColorPainter(
									const char* name,				///< [in] Name of the painter
									const double pel[3],			///< [in] Color to paint
									const char* cspace				///< [in] Color space of the given color
									) = 0;

		//! Adds a painter that returns the per-vertex color interpolated by the
		//! geometry at the hit point (`ri.vColor`).  Falls back to the supplied
		//! default for hits on geometry without vertex colors.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddVertexColorPainter(
									const char* name,				///< [in] Name of the painter
									const double fallback[3],		///< [in] Default color when no vertex color is present
									const char* cspace				///< [in] Color space of the fallback color
									) = 0;

		//! Adds a JPEG texture painter
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddJPEGTexturePainter(
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
									const double shift[3],			///< [in] Shift factor for color values
									const char wrap_s = 0,			///< [in] U-axis wrap mode (see AddPNGTexturePainter)
									const char wrap_t = 0			///< [in] V-axis wrap mode
									) = 0;

		//! Adds a PNG texture painter from an in-memory byte buffer.  Used
		//! by the glTF importer to register painters for embedded `.glb`
		//! images without writing them to disk first.  The byte buffer is
		//! consumed during decode (pixels are copied into an internal
		//! IRasterImage), so the caller may free `bytes` immediately
		//! after this call returns.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddInMemoryPNGTexturePainter(
									const char* name,				///< [in] Name of the painter
									const unsigned char* bytes,		///< [in] Pointer to PNG-encoded byte buffer
									const size_t numBytes,			///< [in] Length of byte buffer
									const char color_space,			///< [in] Color space in the file (same encoding as AddPNGTexturePainter)
									const char filter_type,			///< [in] Texture filtering (same encoding as AddPNGTexturePainter)
									const bool lowmemory,			///< [in] Low-memory mode skips image convert
									const double scale[3],			///< [in] Scale factor for color values
									const double shift[3],			///< [in] Shift factor for color values
									const char wrap_s = 0,			///< [in] U-axis wrap mode (see AddPNGTexturePainter)
									const char wrap_t = 0			///< [in] V-axis wrap mode
									) = 0;

		//! Adds a JPEG texture painter from an in-memory byte buffer.
		//! See AddInMemoryPNGTexturePainter for rationale + lifetime contract.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddInMemoryJPEGTexturePainter(
									const char* name,
									const unsigned char* bytes,
									const size_t numBytes,
									const char color_space,
									const char filter_type,
									const bool lowmemory,
									const double scale[3],
									const double shift[3],
									const char wrap_s = 0,			///< [in] U-axis wrap mode (see AddPNGTexturePainter)
									const char wrap_t = 0			///< [in] V-axis wrap mode
									) = 0;

		//! Adds a HDR texture painter
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddHDRTexturePainter(
									const char* name,				///< [in] Name of the painter
									const char* filename,			///< [in] Name of the file that contains the texture
									const char filter_type,			///< [in] Type of texture filtering
																	///     0 - Nearest neighbour
																	///     1 - Bilinear
																	///     2 - Catmull Rom Bicubic
																	///     3 - Uniform BSpline Bicubic
									const bool lowmemory,			///< [in] low memory mode doesn't do an image convert
									const double scale[3],			///< [in] Scale factor for color values
									const double shift[3],			///< [in] Shift factor for color values
									const char wrap_s = 0,			///< [in] U-axis wrap mode (see AddPNGTexturePainter)
									const char wrap_t = 0			///< [in] V-axis wrap mode
									) = 0;

		//! Adds an EXR texture painter
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddEXRTexturePainter(
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
									const double shift[3],			///< [in] Shift factor for color values
									const char wrap_s = 0,			///< [in] U-axis wrap mode (see AddPNGTexturePainter)
									const char wrap_t = 0			///< [in] V-axis wrap mode
									) = 0;

		//! Adds a texture painter
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddTIFFTexturePainter(
									const char* name,				///< [in] Name of the painter
									const char* filename,			///< [in] Name of the file that contains the texture
									const char color_space,			///< [in] Color space in the file
																	///		0 - Rec709 RGB linear
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
									const double shift[3],			///< [in] Shift factor for color values
									const char wrap_s = 0,			///< [in] U-axis wrap mode (see AddPNGTexturePainter)
									const char wrap_t = 0			///< [in] V-axis wrap mode
									) = 0;

		//! Adds a painter that paints a voronoi diagram
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddVoronoi2DPainter(
									const char* name,				///< [in] Name of the painter
									const double pt_x[],			///< [in] X co-ordinates of generators
									const double pt_y[],			///< [in] Y co-ordinates of generators
									const char** painters,			///< [in] The painters for each generator
									const unsigned int count,		///< [in] Number of the generators
									const char* border,				///< [in] Name of the painter for the border
									const double bsize				///< [in] Size of the border
									) = 0;

		//! Adds a painter that paints a voronoi diagram in 3D
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddVoronoi3DPainter(
									const char* name,				///< [in] Name of the painter
									const double pt_x[],			///< [in] X co-ordinates of generators
									const double pt_y[],			///< [in] Y co-ordinates of generators
									const double pt_z[],			///< [in] Z co-ordinates of generators
									const char** painters,			///< [in] The painters for each generator
									const unsigned int count,		///< [in] Number of the generators
									const char* border,				///< [in] Name of the painter for the border
									const double bsize				///< [in] Size of the border
									) = 0;

		//! Adds a iridescent painter (a painter whose color changes as viewing angle changes)
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddIridescentPainter(
									const char* name,				///< [in] Name of the painter
									const char* pa,					///< [in] First painter
									const char* pb,					///< [in] Second painter
									const double bias				///< [in] Biases the iridescence to one color or another
									) = 0;

		//! Creates a black body radiator painter
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddBlackBodyPainter(
									const char* name,				///< [in] Name of the painter
									const double temperature,		///< [in] Temperature of the radiator in Kelvins
									const double lambda_begin,		///< [in] Where in the spectrum to start creating the spectral packet
									const double lambda_end,		///< [in] Where in the spectrum to end creating the spectral packet
									const unsigned int num_freq,	///< [in] Number of frequencies to use in the spectral packet
									const bool normalize,			///< [in] Should the values be normalized to peak intensity?
									const double scale				///< [in] Value to scale radiant exitance by
									) = 0;

		//! Adds a blend painter
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddBlendPainter(
									const char* name,				///< [in] Name of the painter
									const char* pa,					///< [in] First painter
									const char* pb,					///< [in] Second painter
									const char* mask				///< [in] Mask painter
									) = 0;

		//! Adds a channel-extraction painter.  Returns scale * source[channel] + bias
		//! broadcast as (v, v, v).  Designed for glTF metallic-roughness texture
		//! decomposition (B = metallic, G = roughness).
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddChannelPainter(
									const char* name,				///< [in] Name of the painter
									const char* source,				///< [in] Source painter
									const char  channel,			///< [in] 0=R, 1=G, 2=B
									const double scale,				///< [in] Multiplier on extracted channel
									const double bias				///< [in] Additive offset after scale
									) = 0;


		//
		// Adding materials
		//


		//! Adds Lambertian material
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddLambertianMaterial(
									const char* name,				///< [in] Name of the material
									const char* ref					///< [in] Reflectance Painter
									) = 0;

		//! Adds a Polished material
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddPolishedMaterial(
									const char* name,				///< [in] Name of the material
									const char* ref,				///< [in] Reflectance of diffuse substrate
									const char* tau,				///< [in] Transmittance of dielectric top
									const char* Nt,					///< [in] Index of refraction of dielectric coating
									const char* scat,				///< [in] Scattering function for dielectric coating (either Phong or HG)
									const bool hg					///< [in] Use Henyey-Greenstein phase function scattering
									) = 0;

		//! Adds a Dielectric material
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddDielectricMaterial(
									const char* name,				///< [in] Name of the material
									const char* tau,				///< [in] Transmittance painter
									const char* rIndex,				///< [in] Index of refraction
									const char* scat,				///< [in] Scattering function (either Phong or HG)
									const bool hg					///< [in] Use Henyey-Greenstein phase function scattering
									) = 0;

		//! Adds a SubSurface Scattering material
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddSubSurfaceScatteringMaterial(
									const char* name,				///< [in] Name of the material
									const char* ior,				///< [in] Index of refraction
									const char* absorption,			///< [in] Absorption coefficient
									const char* scattering,			///< [in] Scattering coefficient
									const char* g,					///< [in] HG asymmetry parameter
									const char* roughness			///< [in] Surface roughness [0,1]
									) = 0;

		//! Adds a Random Walk SSS material
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddRandomWalkSSSMaterial(
									const char* name,				///< [in] Name of the material
									const char* ior,				///< [in] Index of refraction
									const char* absorption,			///< [in] Absorption coefficient
									const char* scattering,			///< [in] Scattering coefficient
									const char* g,					///< [in] HG asymmetry parameter
									const char* roughness,			///< [in] Surface roughness [0,1]
									const char* maxBounces			///< [in] Maximum walk steps
									) = 0;

		//! Adds an isotropic phong material
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddIsotropicPhongMaterial(
									const char* name,				///< [in] Name of the material
									const char* rd,					///< [in] Diffuse reflectance painter
									const char* rs,					///< [in] Specular reflectance painter
									const char* exponent			///< [in] Phong exponent
									) = 0;

		//! Adds the anisotropic phong material of Ashikmin and Shirley
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddAshikminShirleyAnisotropicPhongMaterial(
									const char* name,				///< [in] Name of the material
									const char* rd,					///< [in] Diffuse reflectance painter
									const char* rs,					///< [in] Specular reflectance painter
									const char* Nu,					///< [in] Phong exponent in U
									const char* Nv					///< [in] Phong exponent in V
									) = 0;

		//! Adds a perfect reflector
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddPerfectReflectorMaterial(
									const char* name,				///< [in] Name of the material
									const char* ref					///< [in] Reflectance painter
									) = 0;

		//! Adds a perfect refractor
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddPerfectRefractorMaterial(
									const char* name,				///< [in] Name of the material
									const char* ref,				///< [in] Amount of refraction painter
									const char* ior					///< [in] Index of refraction
									) = 0;

		//! Adds a translucent material
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddTranslucentMaterial(
									const char* name,				///< [in] Name of the material
									const char* rF,					///< [in] Reflectance painter
									const char* T,					///< [in] Transmittance painter
									const char* ext,				///< [in] Extinction painter
									const char* N,					///< [in] Index of refraction
									const char* scat				///< [in] Multiple scattering component
									) = 0;

		//! Adds a BioSpec skin material as described by Krishnaswamy and Baranoski
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddBioSpecSkinMaterial(
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
			) = 0;

		//! Adds a Donner & Jensen 2008 spectral skin BSSRDF material
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddDonnerJensenSkinBSSRDFMaterial(
			const char* name,
			const char* melanin_fraction_,
			const char* melanin_blend_,
			const char* hemoglobin_epidermis_,
			const char* carotene_fraction_,
			const char* hemoglobin_dermis_,
			const char* epidermis_thickness_,
			const char* ior_epidermis_,
			const char* ior_dermis_,
			const char* blood_oxygenation_,
			const char* roughness
			) = 0;

		//! Adds a generic human tissue material based on BioSpec
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddGenericHumanTissueMaterial(
			const char* name,
			const char* sca,											///< [in] Scattering co-efficient
			const char* g,												///< [in] The g factor in the HG phase function
			const double whole_blood_,									///< Percentage of the tissue made up of whole blood
			const double hb_ratio_,										///< Ratio of oxyhemoglobin to deoxyhemoglobin in blood
			const double bilirubin_concentration_,						///< Concentration of Bilirubin in whole blood
			const double betacarotene_concentration_,					///< Concentration of Beta-Carotene in whole blood
			const bool diffuse											///< Is the tissue just completely diffuse?
			) = 0;

		//! Adds Composite material
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddCompositeMaterial(
			const char* name,											///< [in] Name of the material
			const char* top,											///< [in] Name of material on top
			const char* bottom,											///< [in] Name of material on bottom
			const unsigned int max_recur,								///< [in] Maximum recursion level in the random walk process
			const unsigned int max_reflection_recursion,				///< [in] Maximum level of reflection recursion
			const unsigned int max_refraction_recursion,				///< [in] Maximum level of refraction recursion
			const unsigned int max_diffuse_recursion,					///< [in] Maximum level of diffuse recursion
			const unsigned int max_translucent_recursion,				///< [in] Maximum level of translucent recursion
			const double thickness,										///< [in] Thickness between the materials
			const char* extinction										///< [in] Extinction painter name
			) = 0;

		//! Adds Composite material
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddWardIsotropicGaussianMaterial(
			const char* name,											///< [in] Name of the material
			const char* diffuse,										///< [in] Diffuse reflectance
			const char* specular,										///< [in] Specular reflectance
			const char* alpha											///< [in] Standard deviation (RMS) of surface slope
			) = 0;

		//! Adds Ward's anisotropic elliptical gaussian material
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddWardAnisotropicEllipticalGaussianMaterial(
			const char* name,											///< [in] Name of the material
			const char* diffuse,										///< [in] Diffuse reflectance
			const char* specular,										///< [in] Specular reflectance
			const char* alphax,											///< [in] Standard deviation (RMS) of surface slope in x
			const char* alphay											///< [in] Standard deviation (RMS) of surface slope in y
			) = 0;

		//! Adds GGX anisotropic microfacet material.
		//!
		//! `fresnel_mode` selects the Fresnel evaluation model:
		//!   "conductor"  (default) — Optics::CalculateConductorReflectance using
		//!                            ior + ext painters; specular acts as a tint
		//!                            on top of conductor Fresnel.  Correct for
		//!                            hand-authored materials with real ior /
		//!                            extinction values.
		//!   "schlick_f0" — Schlick approximation `F = F0 + (1-F0)(1-cosθ_h)^5`
		//!                  treating the `specular` painter as F0 directly; ior
		//!                  and ext painters are unused.  Required by glTF
		//!                  metallicRoughness PBR mapping.  Diffuse is modulated
		//!                  by (1 - max(F0)) per the glTF spec, multiscatter
		//!                  uses the closed-form Schlick hemispherical average.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddGGXMaterial(
			const char* name,											///< [in] Name of the material
			const char* diffuse,										///< [in] Diffuse reflectance
			const char* specular,										///< [in] Specular reflectance / F0
			const char* alphaX,											///< [in] Roughness in tangent u direction
			const char* alphaY,											///< [in] Roughness in tangent v direction
			const char* ior,											///< [in] Index of refraction (ignored in schlick_f0 mode)
			const char* ext,											///< [in] Extinction coefficient (ignored in schlick_f0 mode)
			const char* fresnel_mode = "conductor"						///< [in] "conductor" or "schlick_f0"
			) = 0;

		//! Adds GGX material with optional emissive (LambertianEmitter)
		//! folded in.  glTF pbrMetallicRoughness materials with non-zero
		//! emissiveFactor / emissiveTexture map onto this overload.  Pass
		//! `emissive` = NULL or "none" to fall through to the no-emitter
		//! AddGGXMaterial behaviour.  See AddGGXMaterial for `fresnel_mode`.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddGGXEmissiveMaterial(
			const char* name,											///< [in] Name of the material
			const char* diffuse,										///< [in] Diffuse reflectance
			const char* specular,										///< [in] Specular reflectance / F0
			const char* alphaX,											///< [in] Roughness in tangent u direction
			const char* alphaY,											///< [in] Roughness in tangent v direction
			const char* ior,											///< [in] Index of refraction (ignored in schlick_f0 mode)
			const char* ext,											///< [in] Extinction coefficient (ignored in schlick_f0 mode)
			const char* emissive,										///< [in] Optional emissive painter; "none" / NULL = no emitter
			const double emissive_scale,								///< [in] Multiplier on emissive radiance
			const char* fresnel_mode = "conductor"						///< [in] "conductor" or "schlick_f0"
			) = 0;

		//! Adds a glTF-spec pbrMetallicRoughness material.  Composes a
		//! GGX BRDF from PBR-shaped inputs by auto-creating internal
		//! painters (uniformcolor for constants, blend_painter for the
		//! base_color / metallic / roughness combinators) and a final
		//! ggx_material that consumes them.  The internal painters are
		//! registered in the painter manager with prefix `__pbrmr_<name>__`
		//! so they don't clash with user-authored painters; users
		//! shouldn't reference them by name.
		//!
		//! Mapping (per glTF 2.0 §3.9.2):
		//!   diffuse  = baseColor * (1 - metallic)        // diffuse color
		//!   F0       = lerp(0.04, baseColor, metallic)   // F0 painter
		//!   alpha    = roughness * roughness
		//!   ior      = ignored — Fresnel mode is forced to schlick_f0,
		//!              which uses F0 directly via Schlick's approximation.
		//!              The `ior` argument is preserved for API stability;
		//!              it has no effect on rendering.
		//!
		//! The (1 - 0.04) "diffuse retention factor" used in earlier
		//! revisions is now gone — the GGX BRDF's schlick_f0 mode applies
		//! the (1 - max(F0)) factor at evaluation time per the glTF spec,
		//! so pre-multiplying it into the diffuse painter would
		//! double-apply.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddPBRMetallicRoughnessMaterial(
			const char* name,											///< [in] Name of the material
			const char* base_color,										///< [in] baseColor painter (sRGB-decoded RGB)
			const char* metallic,										///< [in] Metallic painter or scalar string
			const char* roughness,										///< [in] Roughness painter or scalar string
			const double ior,											///< [in] Preserved for API stability; ignored
			const char* emissive,										///< [in] Optional emissive painter; "none" = no emitter
			const double emissive_scale									///< [in] Multiplier on emissive radiance
			) = 0;

		//! Adds a Charlie / Neubelt sheen material for fabric / cloth.
		//! Designed to compose as the top layer in a CompositeMaterial
		//! pairing for glTF KHR_materials_sheen, but usable standalone
		//! for hand-authored fabric scenes.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddSheenMaterial(
			const char* name,											///< [in] Name of the material
			const char* sheen_color,									///< [in] Sheen colour painter
			const char* sheen_roughness									///< [in] Sheen roughness painter or scalar
			) = 0;

		//! Adds Cook Torrance material
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddCookTorranceMaterial(
			const char* name,											///< [in] Name of the material
			const char* diffuse,										///< [in] Diffuse reflectance
			const char* specular,										///< [in] Specular reflectance
			const char* facet,											///< [in] Facet distribution
			const char* ior,											///< [in] IOR delta
			const char* ext												///< [in] Extinction factor
			) = 0;

		//! Adds Oren-Nayar material
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddOrenNayarMaterial(
			const char* name,											///< [in] Name of the material
			const char* reflectance,									///< [in] Reflectance
			const char* roughness										///< [in] Roughness factor
			) = 0;

		//! Adds Schlick material
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddSchlickMaterial(
			const char* name,											///< [in] Name of the material
			const char* diffuse,										///< [in] Diffuse reflectance
			const char* specular,										///< [in] Specular reflectance
			const char* roughness,										///< [in] Roughness factor
			const char* isotropy										///< [in] Isotropy factor
			) = 0;

		//! Adds a data driven material
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddDataDrivenMaterial(
			const char* name,											///< [in] Name of the material
			const char* filename										///< [in] Filename to load data from
			) = 0;

		//! Adds a lambertian luminaire material
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddLambertianLuminaireMaterial(
			const char* name,											///< [in] Name of the material
			const char* radEx,											///< [in] Radiant exitance painter
			const char* mat,											///< [in] Material to use for all non emmission properties
			const double scale											///< [in] Value to scale radiant exitance by
			) = 0;


		//! Adds a phong luminaire material
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddPhongLuminaireMaterial(
			const char* name,											///< [in] Name of the material
			const char* radEx,											///< [in] Radiance exitance painter
			const char* mat,											///< [in] Material to use for all non emmission properties
			const char* N,												///< [in] Phong exponent function
			const double scale											///< [in] Value to scale radiant exitance by
			) = 0;


		//
		// Adds geometry
		//

		//! Adds a box located at the origin
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddBoxGeometry(
								const char* name,					///< [in] Name of the geometry
								const double width,					///< [in] Width of the box
								const double height,				///< [in] Height of the box
								const double depth					///< [in] Depth of the box
								) = 0;

		//! Adds a circular disk at the origin
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddCircularDiskGeometry(
										const char* name,			///< [in] Name of the geometry
										const double radius,		///< [in] Radius of the disk
										const char axis				///< [in] (x|y|z) Which axis the disk sits on
										) = 0;

		//! Adds a clipped plane, defined by four points
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddClippedPlaneGeometry(
										const char* name,			///< [in] Name of the geometry
										const double ptA[4],		///< [in] Point A of the clipped plane
										const double ptB[4],		///< [in] Point B of the clipped plane
										const double ptC[4],		///< [in] Point C of the clipped plane
										const double ptD[4],		///< [in] Point D of the clipped plane
										const bool doublesided		///< [in] Is it doublesided?
										) = 0;

		//! Adds a Cylinder at the origin
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddCylinderGeometry(
									const char* name,				///< [in] Name of the geometry
									const char axis,				///< [in] (x|y|z) Which axis the cylinder is sitting on
									const double radius,			///< [in] Radius of the cylinder
									const double height			///< [in] Height of the cylinder
									) = 0;

		//! Adds an infinite plane that passes through the origin
		/// \return TRUE if successful, FALSE otherwise
		/// \todo This needs to be seriously re-evaluated
		virtual bool AddInfinitePlaneGeometry(
												const char* name,	///< [in] Name of the geometry
												const double xt,	///< [in] How often to tile in X
												const double yt	///< [in] How often to tile in Y
												) = 0;

		//! Adds a sphere at the origin
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddSphereGeometry(
										const char* name,		///< [in] Name of the geometry
										const double radius		///< [in] Radius of the sphere
										) = 0;

		//! Creates an ellipsoid at the origin
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddEllipsoidGeometry(
										const char* name,		///< [in] Name of the geometry
										const double radii[3]	///< [in] Radii of the ellipse
										) = 0;


		//! Adds a torus at the origin
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddTorusGeometry(
										const char* name,			///< [in] Name of the geometry
										const double majorRad,	///< [in] Major radius
										const double minorRad		///< [in] Minor radius (as a percentage of the major radius)
										) = 0;

		//! Adds a triangle mesh geometry from the pointers passed it.
		//! BVH is the only acceleration structure (Tier A2 cleanup, 2026-04-27);
		//! the legacy `max_polys`, `max_recur`, `use_bsp` parameters are gone.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddIndexedTriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const float* vertices,					///< [in] List of vertices
							const float* normals,					///< [in] List of normals
							const float* coords,					///< [in] Texture co-ordinates
							const unsigned int* vertexface,			///< [in] List of the vertex faces
							const unsigned int* uvwface,			///< [in] List of the texture coord faces
							const unsigned int* normalface,			///< [in] List of normal faces
							const unsigned int numpts,				///< [in] Number of points
							const unsigned int numnormals,			///< [in] Number of normals
							const unsigned int numcoords,			///< [in] Number of texture co-ordinate points
							const unsigned int numfaces,			///< [in] Number of faces
							const bool double_sided,				///< [in] Are the triangles double sided ?
							const bool face_normals					///< [in] Use face normals rather than vertex normals
							) = 0;

		//! Adds a triangle mesh geometry from a 3DS file.
		//! BVH is the only acceleration structure (Tier A2 cleanup, 2026-04-27).
		/// \return TRUE if successful, FALSE otherwise
		virtual bool Add3DSTriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* filename,					///< [in] The 3DS file to load
							const bool double_sided,				///< [in] Are the triangles double sided ?
							const bool face_normals					///< [in] Use face normals rather than vertex normals
							) = 0;

		//! Adds a triangle mesh geometry from a raw file.
		//! BVH is the only acceleration structure (Tier A2 cleanup, 2026-04-27).
		/// \return TRUE if successful, FALSE otherwise
		/// \todo this is deprecated and should be removed
		virtual bool AddRAWTriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] Name of the file to load from
							const bool double_sided					///< [in] Are the triangles double sided ?
							) = 0;

		//! Creates a triangle mesh geometry from a file of version 2.
		//! BVH is the only acceleration structure (Tier A2 cleanup, 2026-04-27).
		//! The format of the file for this version is different from the one above.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddRAW2TriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] Name of the file to load from
							const bool double_sided,				///< [in] Are the triangles double sided ?
							const bool face_normals					///< [in] Use face normals rather than vertex normals
							) = 0;

		//! Creates a triangle mesh geometry from a ply file.
		//! BVH is the only acceleration structure (Tier A2 cleanup, 2026-04-27).
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddPLYTriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] Name of the file to load from
							const bool double_sided,				///< [in] Are the triangles double sided ?
							const bool bInvertFaces,				///< [in] Should the faces be inverted?
							const bool face_normals					///< [in] Use face normals rather than vertex normals
							) = 0;

		//! Imports a complete glTF 2.0 scene into the Job.  Walks the
		//! file's scene tree and creates per-primitive geometries +
		//! standard_objects (transforms flow through Job::AddObjectMatrix
		//! verbatim — no Euler decomposition), per-material PBR materials
		//! (Schlick-from-F0 mode), per-texture painters (embedded `.glb`
		//! images decode in-memory; external URIs read from disk — no
		//! sidecar cache), per-light KHR_lights_punctual entries, and
		//! the first camera (subsequent ones warn).  alphaMode = MASK /
		//! BLEND are both honoured via auto-wired alpha_test_shaderop /
		//! transparency_shaderop with per-pixel alpha read straight
		//! from the baseColor texture (PT-only; BDPT/VCM/MLT/photon
		//! tracers bypass shader ops and treat both as opaque).  Phase
		//! 4 also wires KHR_materials_emissive_strength, KHR_materials_
		//! unlit (LambertianLuminaireMaterial), and the scalar subset
		//! of KHR_materials_transmission + volume + ior (Beer-Lambert
		//! glass; transmission_texture is ignored with a warning).
		//! Object names are prefixed with `name_prefix` to avoid
		//! manager-level collisions.  See docs/GLTF_IMPORT.md §7
		//! (phased plan), §13 (Phase 2/3 review), and §15 (Phase 4
		//! status) for the full design and the out-of-scope features
		//! that warn-and-skip (animation / skinning / morph targets /
		//! KHR_materials_clearcoat / KHR_materials_sheen as a layer
		//! over PBR / transmission_texture / other KHR_materials_*
		//! beyond core PBR / Draco / meshopt).
		//!
		//! `scene_index = UINT_MAX` (the recommended default; this is
		//! `GLTFImportOptions::kSceneIndexDefault`) imports the file's
		//! default scene as set in the glTF JSON's top-level `"scene"`
		//! field, falling back to scenes[0] when no default is declared.
		//! Any other value is treated as an explicit zero-based index
		//! into the file's `scenes[]` array; out-of-range values warn
		//! and fall back to the file default.  Note: passing 0 will now
		//! force selection of scenes[0], which is NOT the same as
		//! "default scene" for multi-scene assets whose default is at
		//! a higher index.
		/// \return TRUE if the file parsed successfully, FALSE otherwise.
		virtual bool ImportGLTFScene(
							const char* filename,					///< [in] .gltf or .glb file
							const char* name_prefix,				///< [in] Prefix for created object names
							const unsigned int scene_index,			///< [in] Index into the file's scenes[] array, or UINT_MAX (= GLTFImportOptions::kSceneIndexDefault) for the file's default scene
							const bool import_meshes,				///< [in] Create per-primitive standard_objects
							const bool import_materials,			///< [in] Create one PBR material per glTF material
							const bool import_lights,				///< [in] Create lights from KHR_lights_punctual
							const bool import_cameras,				///< [in] Create the first camera (subsequent ones warn)
							const bool import_normal_maps,			///< [in] Attach normal_map_modifier when material has normalTexture
							const bool lowmem_textures,				///< [in] Defer texture color-space conversion to per-sample (saves ~4x texture RAM + 5-10x faster load on heavy-PBR scenes; pays ~25% per-sample render cost).  Default FALSE for final-render workflow; flip to TRUE for iteration on NewSponza-class scenes.
							const double lights_intensity_override	///< [in] When > 0, replaces zero authored intensities on imported KHR_lights_punctual entries.  Default 0 (no override).  Many assets carry light fixtures as positional metadata with intensity=0 by convention; a positive override wakes them up uniformly without touching lights the author did set non-zero.
							) = 0;

		//! Creates a triangle mesh geometry from a glTF 2.0 file (.gltf or .glb).
		//! Phase 1 of the glTF import work: a single primitive of a single mesh
		//! becomes one named geometry.  Materials, scene structure, lights,
		//! cameras, and animations are NOT imported by this call -- the user
		//! assembles the rest of the scene with existing chunks.  See
		//! docs/GLTF_IMPORT.md §7 for the phased plan.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddGLTFTriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] .gltf or .glb file to load
							const unsigned int mesh_index,			///< [in] Which mesh in the file (0-based)
							const unsigned int primitive_index,		///< [in] Which primitive within the mesh (0-based)
							const bool double_sided,				///< [in] Are the triangles double sided?
							const bool face_normals,				///< [in] Use face normals rather than vertex normals
							const bool flip_v						///< [in] Flip TEXCOORD V at load.  Default TRUE on the chunk parser because glTF stores V increasing upward (V=0 at bottom of texture, OpenGL convention) while RISE's TexturePainter samples V increasing downward (row 0 = top of stored image, DirectX convention).  Override to FALSE only for atypical glTF exports with DirectX V already baked in.
							) = 0;

		//! Adds a mesh from a .risemesh file
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddRISEMeshTriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] Name of the file to load from
							const bool load_into_memory,			///< [in] Do we load the entire file into memory before reading?
							const bool face_normals					///< [in] Use face normals rather than vertex normals
							) = 0;

		//! Creates a bezier patch geometry.
		//! Rendering is always analytic (Kajiya resultant + 2D Newton polish).
		//! Displacement is NOT a parameter either — wrap this geometry in a
		//! `displaced_geometry` chunk for displacement, and for bulk-tessellated
		//! rendering use `displaced_geometry { displacement none disp_scale 0 }`.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddBezierPatchGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] Name of the file to load from
							const unsigned int max_patches,			///< [in] Maximum number of patches per accelerator leaf
							const unsigned char max_recur,			///< [in] Maximum accelerator recursion depth
							const bool use_bsp,						///< [in] Use BSP tree (true) or Octree (false) for the patch accelerator
							const bool bCenterObject				///< [in] Recenter all patch control points around the object-space origin
							) = 0;

		//! Creates a bilinear patch geometry
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddBilinearPatchGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] Name of the file to load from
							const unsigned int max_polys,			///< [in] Maximum number of polygons in one octant node
							const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
							const bool use_bsp						///< [in] Use a BSP tree rather than an Octree
							) = 0;

		//! Creates a displaced geometry wrapping a previously-registered base geometry,
		//! tessellating it via IGeometry::TessellateToMesh and displacing vertices along the
		//! surface normal by displacement(u,v)*disp_scale.  Returns false if the base cannot be
		//! tessellated (e.g. InfinitePlaneGeometry) — the whole scene should then fail to parse.
		//! BVH is the only acceleration structure (Tier A2 cleanup, 2026-04-27);
		//! the legacy `max_polys`, `max_recur`, `use_bsp` parameters are gone.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddDisplacedGeometry(
							const char*         name,				///< [in] Name of the geometry to register
							const char*         base_geometry_name,	///< [in] Name of a previously-registered IGeometry to wrap
							const unsigned int  detail,				///< [in] Tessellation detail; warning logged if > 256
							const char*         displacement,		///< [in] Name of registered IFunction2D, or NULL for pure tessellation
							const Scalar        disp_scale,			///< [in] Displacement scale factor
							const bool          double_sided,		///< [in] Are the displaced triangles double sided?
							const bool          face_normals		///< [in] Use face normals instead of topologically re-averaged vertex normals
							) = 0;


		//
		// Adds lights
		//

		//! Adds a infinite point omni light, located at the origin
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddPointOmniLight(
			const char* name,										///< [in] Name of the light
			const double power,										///< [in] Power of the light in watts
			const double srgb[3],									///< [in] Color of the light in a non-linear colorspace
			const double pos[3],									///< [in] Position of the light
			const bool shootPhotons									///< [in] Should this light shoot photons for photon mapping?
			) = 0;

		//! Adds a infinite point spot light
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddPointSpotLight(
			const char* name,										///< [in] Name of the light
			const double power,										///< [in] Power of the light in watts
			const double srgb[3],									///< [in] Color of the light in a non-linear colorspace
			const double foc[3],									///< [in] Point the center of the light is focussing on
			const double inner,										///< [in] Angle of the inner cone in radians
			const double outer,										///< [in] Angle of the outer cone in radians
			const double pos[3],									///< [in] Position of the light
			const bool shootPhotons									///< [in] Should this light shoot photons for photon mapping?
			) = 0;

		//! Adds the ambient light
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddAmbientLight(
			const char* name,										///< [in] Name of the light
			const double power,										///< [in] Power of the light in watts
			const double srgb[3]									///< [in] Color of the light in a non-linear colorspace
			) = 0;

		//! Adds an infinite directional light, shining in a particular direction
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddDirectionalLight(
			const char* name,										///< [in] Name of the light
			const double power,										///< [in] Power of the light in watts
			const double srgb[3],									///< [in] Color of the light in a non-linear colorspace
			const double dir[3]										///< [in] Direction of the light
			) = 0;


		//
		// Participating media
		//

		//! Adds a homogeneous participating medium
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddHomogeneousMedium(
			const char* name,										///< [in] Name of the medium
			const double sigma_a[3],								///< [in] Absorption coefficient (linear RGB)
			const double sigma_s[3],								///< [in] Scattering coefficient (linear RGB)
			const char* phase_type,									///< [in] Phase function type ("isotropic" or "hg")
			const double phase_g									///< [in] Asymmetry factor for HG (ignored for isotropic)
			) = 0;

		//! Adds a heterogeneous participating medium driven by volume data
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddHeterogeneousMedium(
			const char* name,										///< [in] Name of the medium
			const double max_sigma_a[3],							///< [in] Max absorption coefficient (linear RGB)
			const double max_sigma_s[3],							///< [in] Max scattering coefficient (linear RGB)
			const double emission[3],								///< [in] Volumetric emission (linear RGB)
			const char* phase_type,									///< [in] Phase function type ("isotropic" or "hg")
			const double phase_g,									///< [in] Asymmetry factor for HG
			const char* szVolumeFilePattern,						///< [in] File pattern for volume slices
			const unsigned int volWidth,							///< [in] Volume width in voxels
			const unsigned int volHeight,							///< [in] Volume height in voxels
			const unsigned int volStartZ,							///< [in] Starting z slice index
			const unsigned int volEndZ,								///< [in] Ending z slice index
			const char accessor,									///< [in] Volume accessor type: 'n', 't', or 'c'
			const double bboxMin[3],								///< [in] World-space AABB minimum corner
			const double bboxMax[3]									///< [in] World-space AABB maximum corner
			) = 0;

		//! Adds a heterogeneous participating medium driven by a painter
		/// The painter is evaluated at world-space points to produce density.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddPainterHeterogeneousMedium(
			const char* name,										///< [in] Name of the medium
			const double max_sigma_a[3],							///< [in] Max absorption coefficient (linear RGB)
			const double max_sigma_s[3],							///< [in] Max scattering coefficient (linear RGB)
			const double emission[3],								///< [in] Volumetric emission (linear RGB)
			const char* phase_type,									///< [in] Phase function type ("isotropic" or "hg")
			const double phase_g,									///< [in] Asymmetry factor for HG
			const char* density_painter,							///< [in] Name of the painter to use for density
			const unsigned int virtualResolution,					///< [in] Virtual grid resolution per axis
			const char colorToScalar,								///< [in] Color-to-scalar mode: 'l', 'm', or 'r'
			const double bboxMin[3],								///< [in] World-space AABB minimum corner
			const double bboxMax[3]									///< [in] World-space AABB maximum corner
			) = 0;

		//! Sets the scene's global participating medium
		/// \return TRUE if successful, FALSE otherwise
		virtual bool SetGlobalMedium(
			const char* name										///< [in] Name of a previously added medium
			) = 0;

		//! Assigns an interior participating medium to an object
		/// \return TRUE if successful, FALSE otherwise
		virtual bool SetObjectInteriorMedium(
			const char* object_name,								///< [in] Name of the object
			const char* medium_name									///< [in] Name of the medium
			) = 0;


		//
		// Adds modifiers
		//

		//! Adds a bump map
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddBumpMapModifier(
			const char* name,										///< [in] Name of the modifiers
			const char* func,										///< [in] The function to use as the bump generator
			const double scale,										///< [in] Factor to scale values by
			const double window										///< [in] Size of the window
			) = 0;

		//! Adds a tangent-space normal-map modifier.  The painter must
		//! be loaded with NO colour-matrix conversion: in today's RISE
		//! (RISEPel == ROMMRGBPel) that means `color_space
		//! ROMMRGB_Linear` on the png_painter / jpg_painter / etc.,
		//! which stores PNG bytes verbatim into the engine working
		//! space.  sRGB would gamma-decode and break the [0,1] vector
		//! domain; Rec709RGB_Linear skips gamma but still applies a
		//! Rec709 -> ROMM colour matrix that warps the encoded normal.
		//! Designed for glTF 2.0 normalTexture but works with any
		//! linear-RGB normal map.  See Modifiers/NormalMap.h for the
		//! full rationale.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddNormalMapModifier(
			const char* name,										///< [in] Name of the modifier
			const char* painter,									///< [in] Normal-map painter, loaded with no colour-matrix conversion (ROMMRGB_Linear in today's build)
			const double scale										///< [in] glTF normalTexture.scale (xy multiplier; default 1.0)
			) = 0;

		//
		// Adds functions
		//

		//! Adds a piecewise linear function
		virtual bool AddPiecewiseLinearFunction(
			const char* name,										///< [in] Name of the function
			const double x[],										///< [in] X values of the function
			const double y[],										///< [in] Y values of the function
			const unsigned int num,									///< [in] Number of control points in the x and y arrays
			const bool bUseLUTs,									///< [in] Should the function use lookup tables
			const unsigned int lutsize								///< [in] Size of the lookup table
			) = 0;

		//! Adds a 2D piecewise linear function built up of other functions
		virtual bool AddPiecewiseLinearFunction2D(
			const char* name,										///< [in] Name of the function
			const double x[],										///< [in] X values of the function
			char** y,												///< [in] Y values which is the name of other function1Ds
			const unsigned int num									///< [in] Number of control points in the x and y arrays
			) = 0;


		//
		// Adds Objects
		//

		//! Adds an object
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddObject(
			const char* name,										///< [in] Name of the object
			const char* geom,										///< [in] Name of the geometry for the object
			const char* material,									///< [in] Name of the material
			const char* modifier,									///< [in] Name of the modifier
			const char* shader,										///< [in] Name of the shader
			const RadianceMapConfig& radianceMapConfig,				///< [in] Per-object radiance map (IBL); `isBackground` is ignored here
			const double pos[3],									///< [in] Position of the object
			const double orient[3],									///< [in] Orientation of the object
			const double scale[3],									///< [in] Object scaling
			const bool bCastsShadows,								///< [in] Does the object cast shadows?
			const bool bReceivesShadows								///< [in] Does the object receive shadows?
		) = 0;

		//! Adds an object whose world transform is supplied as a pre-composed
		//! 4x4 matrix.  Used by the glTF importer to pass node transforms
		//! through losslessly instead of round-tripping through Euler XYZ
		//! decomposition (the latter is gimbal-lock prone for arbitrary
		//! quaternion rotations).  Matrix layout is column-major to match
		//! both glTF's `nodes[i].matrix` field and RISE's internal Matrix4.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddObjectMatrix(
			const char* name,										///< [in] Name of the object
			const char* geom,										///< [in] Name of the geometry
			const char* material,									///< [in] Name of the material
			const char* modifier,									///< [in] Name of the modifier
			const char* shader,										///< [in] Name of the shader
			const RadianceMapConfig& radianceMapConfig,				///< [in] Per-object radiance map (IBL)
			const double matrix[16],								///< [in] World transform, 4x4 column-major
			const bool bCastsShadows,
			const bool bReceivesShadows
		) = 0;

		//! Creates a CSG object
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddCSGObject(
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
			const RadianceMapConfig& radianceMapConfig,				///< [in] Per-object radiance map (IBL); `isBackground` is ignored here
			const double pos[3],									///< [in] Position of the object
			const double orient[3],									///< [in] Orientation of the object
			const bool bCastsShadows,								///< [in] Does the object cast shadows?
			const bool bReceivesShadows								///< [in] Does the object receive shadows?
			) = 0;


		//
		// Adds ShaderOps
		//
		virtual bool AddReflectionShaderOp(
			const char* name										///< [in] Name of the shaderop
			) = 0;

		virtual bool AddRefractionShaderOp(
			const char* name										///< [in] Name of the shaderop
			) = 0;

		virtual bool AddEmissionShaderOp(
			const char* name										///< [in] Name of the shaderop
			) = 0;

		virtual bool AddDirectLightingShaderOp(
			const char* name,										///< [in] Name of the shaderop
			const char* bsdf										///< [in] BSDF to use when computing radiance (overrides object BSDF)
			) = 0;

		virtual bool AddCausticPelPhotonMapShaderOp(
			const char* name										///< [in] Name of the shaderop
			) = 0;

		virtual bool AddCausticSpectralPhotonMapShaderOp(
			const char* name										///< [in] Name of the shaderop
			) = 0;

		virtual bool AddGlobalPelPhotonMapShaderOp(
			const char* name										///< [in] Name of the shaderop
			) = 0;

		virtual bool AddGlobalSpectralPhotonMapShaderOp(
			const char* name										///< [in] Name of the shaderop
			) = 0;

		virtual bool AddTranslucentPelPhotonMapShaderOp(
			const char* name										///< [in] Name of the shaderop
			) = 0;

		virtual bool AddShadowPhotonMapShaderOp(
			const char* name										///< [in] Name of the shaderop
			) = 0;

		virtual bool AddDistributionTracingShaderOp(
			const char* name,										///< [in] Name of the shaderop
			const unsigned int samples,								///< [in] Number of sample to use in distribution
			const bool irradiancecaching,							///< [in] Should irradiance caching be used if available?
			const bool forcecheckemitters,							///< [in] Force rays allowing to hit emitters even though the material may have a BRDF
			const bool reflections,									///< [in] Should reflections be traced?
			const bool refractions,									///< [in] Should refractions be traced?
			const bool diffuse,										///< [in] Should diffuse rays be traced?
			const bool translucents									///< [in] Should translucent rays be traced?
			) = 0;

		virtual bool AddFinalGatherShaderOp(
			const char* name,										///< [in] Name of the shaderop
			const unsigned int numtheta,							///< [in] Number of samples in the theta direction
			const unsigned int numphi,								///< [in] Number of samples in the phi direction
			const bool cachegradients,								///< [in] Should cache gradients be used in the irradiance cache?
			const unsigned int min_effective_contributors,			///< [in] Minimum effective contributors required for interpolation
			const double high_variation_reuse_scale,				///< [in] Minimum reuse scale for bright high-variation cache records
			const bool cache										///< [in] Should the rasterizer state cache be used?
			) = 0;

		virtual bool AddPathTracingShaderOp(
			const char* name,
			const bool smsEnabled,
			const unsigned int smsMaxIterations,
			const double smsThreshold,
			const unsigned int smsMaxChainDepth,
			const bool smsBiased
			) = 0;

		virtual bool AddSMSShaderOp(
			const char* name,
			const unsigned int maxIterations,
			const double threshold,
			const unsigned int maxChainDepth,
			const bool biased
			) = 0;

		virtual bool AddAmbientOcclusionShaderOp(
			const char* name,										///< [in] Name of the shaderop
			const unsigned int numtheta,							///< [in] Number of samples in the theta direction
			const unsigned int numphi,								///< [in] Number of samples in the phi direction
			const bool multiplybrdf,								///< [in] Should individual samples be multiplied by the BRDF ?
			const bool irradiance_cache								///< [in] Should the irradiance state cache be used?
			) = 0;

		virtual bool AddSimpleSubSurfaceScatteringShaderOp(
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
			) = 0;

		virtual bool AddDiffusionApproximationSubSurfaceScatteringShaderOp(
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
			) = 0;

		//! Adds a Donner & Jensen 2008 skin SSS shader op (octree-based)
		virtual bool AddDonnerJensenSkinSSSShaderOp(
			const char* name,
			const unsigned int numPoints,
			const double error,
			const unsigned int maxPointsPerNode,
			const unsigned char maxDepth,
			const double irrad_scale,
			const char* shader,
			const bool cache,
			const double melanin_fraction,
			const double melanin_blend,
			const double hemoglobin_epidermis,
			const double carotene_fraction,
			const double hemoglobin_dermis,
			const double epidermis_thickness,
			const double ior_epidermis,
			const double ior_dermis,
			const double blood_oxygenation,
			const char* melanin_fraction_offset,
			const char* hemoglobin_epidermis_offset,
			const char* hemoglobin_dermis_offset
			) = 0;

		virtual bool AddAreaLightShaderOp(
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
			) = 0;

		virtual bool AddTransparencyShaderOp(
			const char* name,										///< [in] Name of the shaderop
			const char* transparency,								///< [in] Transparency painter
			const bool one_sided									///< [in] One sided transparency only (ignore backfaces)
			) = 0;

		//! Adds a stochastic alpha-test shader op.  Implements glTF
		//! alphaMode = MASK: at hit time, samples alpha from the
		//! painter; if alpha < cutoff, the ray is forwarded past the
		//! surface and shading from behind is composited into the
		//! current pixel.  Otherwise this op is a no-op and downstream
		//! shader ops handle the surface normally.
		//!
		//! Caveat: this op runs only on integrators that go through
		//! IShader::Shade() (the path tracer and legacy direct shaders).
		//! BDPT, VCM, MLT, and photon tracers bypass the shader-op
		//! pipeline and treat the surface as fully opaque.  No runtime
		//! warning is emitted when an alpha-masked material renders
		//! under one of those integrators -- users must choose PT for
		//! glTF MASK assets, or accept the cutout being silently
		//! ignored.  See AlphaTestShaderOp.h for the full architectural
		//! rationale.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddAlphaTestShaderOp(
			const char* name,										///< [in] Name of the shaderop
			const char* alpha_painter,								///< [in] Painter sampling alpha at the hit
			const double cutoff										///< [in] Threshold; alpha < cutoff continues the ray past the surface
			) = 0;

		//
		// Adds Shaders
		//
		virtual bool AddStandardShader(
			const char* name,										///< [in] Name of the shader
			const unsigned int count,								///< [in] Number of shaderops
			const char** shaderops									///< [in] All of the shaderops
			) = 0;

		virtual bool AddAdvancedShader(
			const char* name,										///< [in] Name of the shader
			const unsigned int count,								///< [in] Number of shaderops
			const char** shaderops,									///< [in] All of the shaderops
			const unsigned int* mindepths,							///< [in] All of the minimum depths for the shaderops
			const unsigned int* maxdepths,							///< [in] All of the maximum depths for the shaderops
			const char* operations									///< [in] All the operations for the shaderops
			) = 0;

		virtual bool AddDirectVolumeRenderingShader(
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
			) = 0;

		virtual bool AddSpectralDirectVolumeRenderingShader(
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
			) = 0;

		//
		// Sets Rasterization parameters
		//

		//! Sets the rasterizer type to be pixel based PEL
		virtual bool SetPixelBasedPelRasterizer(
			const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
			const unsigned int numLumSamples,						///< [in] Number of samples / luminaire
			const unsigned int maxRecur,							///< [in] Maximum recursion level
			const char* shader,										///< [in] The default shader
			const RadianceMapConfig& radianceMapConfig,				///< [in] Global radiance map (IBL) configuration
			const char* luminarySampler,							///< [in] Type of sampling to use for luminaries
			const double luminarySamplerParam,						///< [in] Parameter for the luminary sampler
			const PixelFilterConfig& pixelFilterConfig,				///< [in] Pixel reconstruction filter
			const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
			const bool oidnDenoise,									///< [in] Enable OIDN denoising post-process
			const OidnQuality oidnQuality,							///< [in] OIDN quality preset (Auto = render-time heuristic)
			const OidnDevice oidnDevice,							///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
			const OidnPrefilter oidnPrefilter,						///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
			const PathGuidingConfig& guidingConfig,					///< [in] Path guiding configuration
			const AdaptiveSamplingConfig& adaptiveConfig,			///< [in] Adaptive sampling configuration
			const StabilityConfig& stabilityConfig,					///< [in] Production stability controls
			const ProgressiveConfig& progressiveConfig				///< [in] Progressive multi-pass rendering configuration
			) = 0;

		//! Sets the rasterizer type to be pixel based spectral integrating
		virtual bool SetPixelBasedSpectralIntegratingRasterizer(
			const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
			const unsigned int numLumSamples,						///< [in] Number of samples / luminaire
			const SpectralConfig& spectralConfig,					///< [in] Spectral wavelength range, bins, and sampling strategy
			const unsigned int maxRecur,							///< [in] Maximum recursion level
			const char* shader,										///< [in] The default shader
			const RadianceMapConfig& radianceMapConfig,				///< [in] Global radiance map (IBL) configuration
			const char* luminarySampler,							///< [in] Type of sampling to use for luminaries
			const double luminarySamplerParam,						///< [in] Parameter for the luminary sampler
			const PixelFilterConfig& pixelFilterConfig,				///< [in] Pixel reconstruction filter
			const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
			const bool bIntegrateRGB,								///< [in] Should we use the CIE XYZ spd functions or will they be specified now?
			const unsigned int numSPDvalues,						///< [in] Number of values in the RGB SPD arrays
			const double rgb_spd_frequencies[],						///< [in] Array that contains the RGB SPD frequencies
			const double rgb_spd_r[],								///< [in] Array that contains the RGB SPD amplitudes for red
			const double rgb_spd_g[],								///< [in] Array that contains the RGB SPD amplitudes for green
			const double rgb_spd_b[],								///< [in] Array that contains the RGB SPD amplitudes for blue
			const bool oidnDenoise,									///< [in] Enable OIDN denoising post-process
			const OidnQuality oidnQuality,							///< [in] OIDN quality preset (Auto = render-time heuristic)
			const OidnDevice oidnDevice,							///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
			const OidnPrefilter oidnPrefilter,						///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
			const StabilityConfig& stabilityConfig					///< [in] Production stability controls
			) = 0;

		//! Sets the rasterizer type to be Pel (RGB) BDPT
		virtual bool SetBDPTPelRasterizer(
			const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
			const unsigned int maxEyeDepth,							///< [in] Maximum eye subpath depth
			const unsigned int maxLightDepth,						///< [in] Maximum light subpath depth
			const char* shader,										///< [in] The default shader
			const RadianceMapConfig& radianceMapConfig,				///< [in] Global radiance map (IBL) configuration
			const PixelFilterConfig& pixelFilterConfig,				///< [in] Pixel reconstruction filter
			const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
			const SMSConfig& smsConfig,								///< [in] Specular Manifold Sampling configuration
			const bool oidnDenoise,									///< [in] Enable OIDN denoising post-process
			const OidnQuality oidnQuality,							///< [in] OIDN quality preset (Auto = render-time heuristic)
			const OidnDevice oidnDevice,							///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
			const OidnPrefilter oidnPrefilter,						///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
			const PathGuidingConfig& guidingConfig,					///< [in] Path guiding configuration
			const AdaptiveSamplingConfig& adaptiveConfig,			///< [in] Adaptive sampling configuration
			const StabilityConfig& stabilityConfig,					///< [in] Production stability controls
			const ProgressiveConfig& progressiveConfig				///< [in] Progressive multi-pass rendering configuration
			) = 0;

		//! Sets the rasterizer type to be spectral BDPT
		virtual bool SetBDPTSpectralRasterizer(
			const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
			const unsigned int maxEyeDepth,							///< [in] Maximum eye subpath depth
			const unsigned int maxLightDepth,						///< [in] Maximum light subpath depth
			const char* shader,										///< [in] The default shader
			const RadianceMapConfig& radianceMapConfig,				///< [in] Global radiance map (IBL) configuration
			const PixelFilterConfig& pixelFilterConfig,				///< [in] Pixel reconstruction filter
			const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
			const SpectralConfig& spectralConfig,					///< [in] Spectral wavelength range, bins, and sampling strategy
			const SMSConfig& smsConfig,								///< [in] Specular Manifold Sampling configuration
			const bool oidnDenoise,									///< [in] Enable OIDN denoising post-process
			const OidnQuality oidnQuality,							///< [in] OIDN quality preset (Auto = render-time heuristic)
			const OidnDevice oidnDevice,							///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
			const OidnPrefilter oidnPrefilter,						///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
			const PathGuidingConfig& guidingConfig,					///< [in] Path guiding configuration
			const StabilityConfig& stabilityConfig,					///< [in] Production stability controls
			const ProgressiveConfig& progressiveConfig				///< [in] Progressive multi-pass rendering configuration
			) = 0;

		//! Sets the rasterizer type to be Pel (RGB) Vertex Connection and Merging
		virtual bool SetVCMPelRasterizer(
			const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
			const unsigned int maxEyeDepth,							///< [in] Maximum eye subpath depth
			const unsigned int maxLightDepth,						///< [in] Maximum light subpath depth
			const char* shader,										///< [in] The default shader
			const RadianceMapConfig& radianceMapConfig,				///< [in] Global radiance map (IBL) configuration
			const PixelFilterConfig& pixelFilterConfig,				///< [in] Pixel reconstruction filter
			const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
			const double mergeRadius,								///< [in] Photon merge radius (0 => scene-auto fallback)
			const bool enableVC,									///< [in] Enable vertex connection strategies
			const bool enableVM,									///< [in] Enable vertex merging strategy
			const bool oidnDenoise,									///< [in] Enable OIDN denoising
			const OidnQuality oidnQuality,							///< [in] OIDN quality preset (Auto = render-time heuristic)
			const OidnDevice oidnDevice,							///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
			const OidnPrefilter oidnPrefilter,						///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
			const PathGuidingConfig& guidingConfig,					///< [in] Path guiding configuration
			const AdaptiveSamplingConfig& adaptiveConfig,			///< [in] Adaptive sampling configuration
			const StabilityConfig& stabilityConfig,					///< [in] Production stability controls
			const ProgressiveConfig& progressiveConfig				///< [in] Progressive multi-pass rendering configuration
			) = 0;

		//! Sets the rasterizer type to be spectral Vertex Connection and Merging
		virtual bool SetVCMSpectralRasterizer(
			const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
			const unsigned int maxEyeDepth,							///< [in] Maximum eye subpath depth
			const unsigned int maxLightDepth,						///< [in] Maximum light subpath depth
			const char* shader,										///< [in] The default shader
			const RadianceMapConfig& radianceMapConfig,				///< [in] Global radiance map (IBL) configuration
			const PixelFilterConfig& pixelFilterConfig,				///< [in] Pixel reconstruction filter
			const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
			const SpectralConfig& spectralConfig,					///< [in] Spectral wavelength range, bins, and sampling strategy
			const double mergeRadius,								///< [in] Photon merge radius (0 => scene-auto fallback)
			const bool enableVC,									///< [in] Enable vertex connection strategies
			const bool enableVM,									///< [in] Enable vertex merging strategy
			const bool oidnDenoise,									///< [in] Enable OIDN denoising
			const OidnQuality oidnQuality,							///< [in] OIDN quality preset (Auto = render-time heuristic)
			const OidnDevice oidnDevice,							///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
			const OidnPrefilter oidnPrefilter,						///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
			const PathGuidingConfig& guidingConfig,					///< [in] Path guiding configuration
			const AdaptiveSamplingConfig& adaptiveConfig,			///< [in] Adaptive sampling configuration
			const StabilityConfig& stabilityConfig,					///< [in] Production stability controls
			const ProgressiveConfig& progressiveConfig				///< [in] Progressive multi-pass rendering configuration
			) = 0;

		//! Sets the rasterizer to pure path tracing (Pel, bypasses shader ops)
		virtual bool SetPathTracingPelRasterizer(
			const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
			const char* shader,										///< [in] The default shader
			const RadianceMapConfig& radianceMapConfig,				///< [in] Global radiance map (IBL) configuration
			const PixelFilterConfig& pixelFilterConfig,				///< [in] Pixel reconstruction filter
			const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
			const SMSConfig& smsConfig,								///< [in] Specular Manifold Sampling configuration
			const bool oidnDenoise,									///< [in] Enable OIDN denoising post-process
			const OidnQuality oidnQuality,							///< [in] OIDN quality preset (Auto = render-time heuristic)
			const OidnDevice oidnDevice,							///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
			const OidnPrefilter oidnPrefilter,						///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
			const PathGuidingConfig& guidingConfig,					///< [in] Path guiding configuration
			const AdaptiveSamplingConfig& adaptiveConfig,			///< [in] Adaptive sampling configuration
			const StabilityConfig& stabilityConfig,					///< [in] Production stability controls
			const ProgressiveConfig& progressiveConfig				///< [in] Progressive multi-pass rendering configuration
			) = 0;

		//! Sets the rasterizer to pure path tracing (spectral, bypasses shader ops)
		virtual bool SetPathTracingSpectralRasterizer(
			const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
			const char* shader,										///< [in] The default shader
			const RadianceMapConfig& radianceMapConfig,				///< [in] Global radiance map (IBL) configuration
			const PixelFilterConfig& pixelFilterConfig,				///< [in] Pixel reconstruction filter
			const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
			const SpectralConfig& spectralConfig,					///< [in] Spectral wavelength range, bins, and sampling strategy
			const SMSConfig& smsConfig,								///< [in] Specular Manifold Sampling configuration
			const bool oidnDenoise,									///< [in] Enable OIDN denoising post-process
			const OidnQuality oidnQuality,							///< [in] OIDN quality preset (Auto = render-time heuristic)
			const OidnDevice oidnDevice,							///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
			const OidnPrefilter oidnPrefilter,						///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
			const AdaptiveSamplingConfig& adaptiveConfig,			///< [in] Adaptive sampling configuration
			const StabilityConfig& stabilityConfig,					///< [in] Production stability controls
			const ProgressiveConfig& progressiveConfig				///< [in] Progressive multi-pass rendering configuration
			) = 0;

		//! Sets up an MLT (Metropolis Light Transport / PSSMLT) rasterizer.
		//! Filter-aware signature — this is what the parser and new
		//! code calls.  Subclasses must implement this overload.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool SetMLTRasterizer(
			const unsigned int maxEyeDepth,							///< [in] Maximum eye subpath depth
			const unsigned int maxLightDepth,						///< [in] Maximum light subpath depth
			const unsigned int nBootstrap,							///< [in] Number of bootstrap samples
			const unsigned int nChains,								///< [in] Number of Markov chains
			const unsigned int nMutationsPerPixel,					///< [in] Mutations per pixel budget
			const double largeStepProb,								///< [in] Large step probability
			const char* shader,										///< [in] The default shader
			const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
			const bool oidnDenoise,									///< [in] Enable OIDN denoising post-process
			const OidnQuality oidnQuality,							///< [in] OIDN quality preset (Auto = render-time heuristic)
			const OidnDevice oidnDevice,							///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
			const OidnPrefilter oidnPrefilter,						///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
			const PixelFilterConfig& pixelFilterConfig,				///< [in] Pixel reconstruction filter
			const StabilityConfig& stabilityConfig					///< [in] Production stability controls
			) = 0;

		//! Legacy pre-filter overload of SetMLTRasterizer.  Preserves
		//! source compatibility for external code written against the
		//! pre-filter IJob API — it forwards to the filter-aware
		//! overload asking for `pixel_filter none`, which reproduces
		//! the old unfiltered (round-to-nearest point splat) behaviour.
		//! New code should call the full overload with an explicit
		//! filter choice.  Non-virtual so it dispatches through the
		//! derived class's pure-virtual implementation.
		/// \return TRUE if successful, FALSE otherwise
		bool SetMLTRasterizer(
			const unsigned int maxEyeDepth,
			const unsigned int maxLightDepth,
			const unsigned int nBootstrap,
			const unsigned int nChains,
			const unsigned int nMutationsPerPixel,
			const double largeStepProb,
			const char* shader,
			const bool bShowLuminaires,
			const bool oidnDenoise,
			const StabilityConfig& stabilityConfig
			)
		{
			PixelFilterConfig legacyNone;
			legacyNone.filter = "none";
			return SetMLTRasterizer(
				maxEyeDepth, maxLightDepth, nBootstrap, nChains,
				nMutationsPerPixel, largeStepProb, shader,
				bShowLuminaires, oidnDenoise, OidnQuality::Auto, OidnDevice::Auto, OidnPrefilter::Fast,
				legacyNone, stabilityConfig );
		}

		//! Sets up a spectral MLT (Metropolis Light Transport / PSSMLT) rasterizer.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool SetMLTSpectralRasterizer(
			const unsigned int maxEyeDepth,							///< [in] Maximum eye subpath depth
			const unsigned int maxLightDepth,						///< [in] Maximum light subpath depth
			const unsigned int nBootstrap,							///< [in] Number of bootstrap samples
			const unsigned int nChains,								///< [in] Number of Markov chains
			const unsigned int nMutationsPerPixel,					///< [in] Mutations per pixel budget
			const double largeStepProb,								///< [in] Large step probability
			const char* shader,										///< [in] The default shader
			const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
			const SpectralConfig& spectralConfig,					///< [in] Spectral wavelength range, bins, and sampling strategy
			const bool oidnDenoise,									///< [in] Enable OIDN denoising post-process
			const OidnQuality oidnQuality,							///< [in] OIDN quality preset (Auto = render-time heuristic)
			const OidnDevice oidnDevice,							///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
			const OidnPrefilter oidnPrefilter,						///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
			const PixelFilterConfig& pixelFilterConfig,				///< [in] Pixel reconstruction filter
			const StabilityConfig& stabilityConfig					///< [in] Production stability controls
			) = 0;

		//! Legacy pre-filter overload.  See SetMLTRasterizer above for
		//! the rationale.
		/// \return TRUE if successful, FALSE otherwise
		bool SetMLTSpectralRasterizer(
			const unsigned int maxEyeDepth,
			const unsigned int maxLightDepth,
			const unsigned int nBootstrap,
			const unsigned int nChains,
			const unsigned int nMutationsPerPixel,
			const double largeStepProb,
			const char* shader,
			const bool bShowLuminaires,
			const double nmbegin,
			const double nmend,
			const unsigned int nSpectralSamples,
			const bool useHWSS,
			const bool oidnDenoise,
			const StabilityConfig& stabilityConfig
			)
		{
			PixelFilterConfig legacyNone;
			legacyNone.filter = "none";
			SpectralConfig legacySpectral;
			legacySpectral.nmBegin = nmbegin;
			legacySpectral.nmEnd = nmend;
			legacySpectral.spectralSamples = nSpectralSamples;
			legacySpectral.useHWSS = useHWSS;
			return SetMLTSpectralRasterizer(
				maxEyeDepth, maxLightDepth, nBootstrap, nChains,
				nMutationsPerPixel, largeStepProb, shader,
				bShowLuminaires,
				legacySpectral, oidnDenoise, OidnQuality::Auto, OidnDevice::Auto, OidnPrefilter::Fast,
				legacyNone, stabilityConfig );
		}

		//
		// Adds rasterizer outputs
		//

		//! Creates a file rasterizer output
		//! This should be called after a rasterizer has been set
		//! Note that setting a new rasterizer after adding file rasterizer outputs will
		//! delete existing outputs
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddFileRasterizerOutput(
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
			) = 0;

		//! Creates a user callback rasterizer output
		//! This should be called after a rasterizer has been set
		//! Note that no attemps at reference counting are made, the user
		//! better not go delete the object
		virtual bool AddCallbackRasterizerOutput(
			IJobRasterizerOutput* pObj
			) = 0;


		//
		// Photon mapping
		//

		//! Sets the gather parameters for the caustic pel photon map
		/// \return TRUE if successful, FALSE otherwise
		virtual bool SetCausticPelGatherParameters(
			const double radius,							///< [in] Search radius
			const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
			const unsigned int min,							///< [in] Maximum number of photons to store
			const unsigned int max							///< [in] Total number of photons to shoot
			) = 0;

		//! Sets the gather parameters for the global pel photon map
		/// \return TRUE if successful, FALSE otherwise
		virtual bool SetGlobalPelGatherParameters(
			const double radius,							///< [in] Search radius
			const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
			const unsigned int min,							///< [in] Maximum number of photons to store
			const unsigned int max							///< [in] Total number of photons to shoot
			) = 0;

		//! Sets the gather parameters for the translucent pel photon map
		/// \return TRUE if successful, FALSE otherwise
		virtual bool SetTranslucentPelGatherParameters(
			const double radius,							///< [in] Search radius
			const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
			const unsigned int min,							///< [in] Maximum number of photons to store
			const unsigned int max							///< [in] Total number of photons to shoot
			) = 0;

		//! Sets the gather parameters for the caustic spectral photon map
		/// \return TRUE if successful, FALSE otherwise
		virtual bool SetCausticSpectralGatherParameters(
			const double radius,							///< [in] Search radius
			const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
			const unsigned int min,							///< [in] Maximum number of photons to store
			const unsigned int max,							///< [in] Total number of photons to shoot
			const double nm_range							///< [in] Range of wavelengths to search for a NM irradiance estimate
			) = 0;

		//! Sets the gather parameters for the global spectral photon map
		/// \return TRUE if successful, FALSE otherwise
		virtual bool SetGlobalSpectralGatherParameters(
			const double radius,							///< [in] Search radius
			const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
			const unsigned int min,							///< [in] Maximum number of photons to store
			const unsigned int max,							///< [in] Total number of photons to shoot
			const double nm_range							///< [in] Range of wavelengths to search for a NM irradiance estimate
			) = 0;

		//! Sets the irradiance cache parameters
		/// \return TRUE if successful, FALSE otherwise
		virtual bool SetIrradianceCacheParameters(
			const unsigned int size,						///< [in] Size of the cache
			const double tolerance,							///< [in] Tolerance of the cache
			const double min_spacing,						///< [in] Minimum seperation
			const double max_spacing,						///< [in] Maximum seperation
			const double query_threshold_scale,				///< [in] Scale for the query acceptance threshold
			const double neighbor_spacing_scale				///< [in] Scale for capping reuse radius by local neighbor spacing
			) = 0;

		//! Sets the gather parameters for the shadow photon map
		/// \return TRUE if successful, FALSE otherwise
		virtual bool SetShadowGatherParameters(
			const double radius,							///< [in] Search radius
			const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
			const unsigned int min,							///< [in] Maximum number of photons to store
			const unsigned int max							///< [in] Total number of photons to shoot
			) = 0;

		//! Saves the caustic pel photon map to disk
		virtual bool SaveCausticPelPhotonmap(
			const char* file_name							///< [in] Name of the file to save it to
			) = 0;

		//! Saves the global pel photon map to disk
		virtual bool SaveGlobalPelPhotonmap(
			const char* file_name							///< [in] Name of the file to save it to
			) = 0;

		//! Saves the translucent pel photon map to disk
		virtual bool SaveTranslucentPelPhotonmap(
			const char* file_name							///< [in] Name of the file to save it to
			) = 0;

		//! Saves the caustic spectral photon map to disk
		virtual bool SaveCausticSpectralPhotonmap(
			const char* file_name							///< [in] Name of the file to save it to
			) = 0;

		//! Saves the global spectral photon map to disk
		virtual bool SaveGlobalSpectralPhotonmap(
			const char* file_name							///< [in] Name of the file to save it to
			) = 0;

		//! Saves the shadow photon map to disk
		virtual bool SaveShadowPhotonmap(
			const char* file_name							///< [in] Name of the file to save it to
			) = 0;


		//! Loads the caustic pel photon map from disk
		virtual bool LoadCausticPelPhotonmap(
			const char* file_name							///< [in] Name of the file to load from
			) = 0;

		//! Loads the global pel photon map from disk
		virtual bool LoadGlobalPelPhotonmap(
			const char* file_name							///< [in] Name of the file to load from
			) = 0;

		//! Loads the translucent pel photon map from disk
		virtual bool LoadTranslucentPelPhotonmap(
			const char* file_name							///< [in] Name of the file to load from
			) = 0;

		//! Loads the caustic spectral photon map from disk
		virtual bool LoadCausticSpectralPhotonmap(
			const char* file_name							///< [in] Name of the file to load from
			) = 0;

		//! Loads the global spectral photon map from disk
		virtual bool LoadGlobalSpectralPhotonmap(
			const char* file_name							///< [in] Name of the file to load from
			) = 0;

		//! Loads the shadow photon map from disk
		virtual bool LoadShadowPhotonmap(
			const char* file_name							///< [in] Name of the file to load from
			) = 0;


		//
		// Commands
		//

		//! Shoots caustic photons and populates the caustic pel photon map
		/// \return TRUE if successful, FALSE otherwise
		virtual bool ShootCausticPelPhotons(
			const unsigned int num,							///< [in] Number of photons to acquire
			const double power_scale,						///< [in] How much to scale light power by
			const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
			const double minImportance,						///< [in] Minimum importance when a photon is discarded
			const bool branch,								///< [in] Should the tracer branch or follow a single path?
			const bool reflect,								///< [in] Should we trace reflected rays?
			const bool refract,								///< [in] Should we trace refracted rays?
			const bool shootFromNonMeshLights,				///< [in] Should we shoot from non mesh based lights?
			const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
			const bool regenerate,							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
			const bool shootFromMeshLights = true			///< [in] Should we shoot from mesh based lights (luminaries)?
			) = 0;

		//! Shoots global photons and populates the global pel photon map
		/// \return TRUE if successful, FALSE otherwise
		virtual bool ShootGlobalPelPhotons(
			const unsigned int num,							///< [in] Number of photons to acquire
			const double power_scale,						///< [in] How much to scale light power by
			const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
			const double minImportance,						///< [in] Minimum importance when a photon is discarded
			const bool branch,								///< [in] Should the tracer branch or follow a single path?
			const bool shootFromNonMeshLights,				///< [in] Should we shoot from non mesh based lights?
			const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
			const bool regenerate,							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
			const bool shootFromMeshLights = true			///< [in] Should we shoot from mesh based lights (luminaries)?
			) = 0;

		//! Shoots translucent photons and populates the translucent pel photon map
		/// \return TRUE if successful, FALSE otherwise
		virtual bool ShootTranslucentPelPhotons(
			const unsigned int num,							///< [in] Number of photons to acquire
			const double power_scale,						///< [in] How much to scale light power by
			const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
			const double minImportance,						///< [in] Minimum importance when a photon is discarded
			const bool reflect,								///< [in] Should we trace reflected rays?
			const bool refract,								///< [in] Should we trace refracted rays?
			const bool direct_translucent,					///< [in] Should we trace translucent primary interaction rays?
			const bool shootFromNonMeshLights,				///< [in] Should we shoot from non mesh based lights?
			const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
			const bool regenerate,							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
			const bool shootFromMeshLights = true			///< [in] Should we shoot from mesh based lights (luminaries)?
			) = 0;

		//! Shoots caustic photons and populates the caustic spectral photon map
		/// \return TRUE if successful, FALSE otherwise
		virtual bool ShootCausticSpectralPhotons(
			const unsigned int num,							///< [in] Number of photons to acquire
			const double power_scale,						///< [in] How much to scale light power by
			const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
			const double minImportance,						///< [in] Minimum importance when a photon is discarded
			const double nm_begin,							///< [in] Wavelength to start shooting photons at
			const double nm_end,							///< [in] Wavelength to end shooting photons at
			const unsigned int num_wavelengths,				///< [in] Number of wavelengths to shoot photons at
			const bool branch,								///< [in] Should the tracer branch or follow a single path?
			const bool reflect,								///< [in] Should we trace reflected rays?
			const bool refract,								///< [in] Should we trace refracted rays?
			const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
			const bool regenerate							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
			) = 0;

		//! Shoots global photons and populates the global spectral photon map
		/// \return TRUE if successful, FALSE otherwise
		virtual bool ShootGlobalSpectralPhotons(
			const unsigned int num,							///< [in] Number of photons to acquire
			const double power_scale,						///< [in] How much to scale light power by
			const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
			const double minImportance,						///< [in] Minimum importance when a photon is discarded
			const double nm_begin,							///< [in] Wavelength to start shooting photons at
			const double nm_end,							///< [in] Wavelength to end shooting photons at
			const unsigned int num_wavelengths,				///< [in] Number of wavelengths to shoot photons at
			const bool branch,								///< [in] Should the tracer branch or follow a single path?
			const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
			const bool regenerate							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
			) = 0;

		//! Shoots shadow photons and populates the shadow photon map
		/// \return TRUE if successful, FALSE otherwise
		virtual bool ShootShadowPhotons(
			const unsigned int num,							///< [in] Number of photons to acquire
			const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
			const bool regenerate							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
			) = 0;

		//! Predicts the amount of time in ms it will take to rasterize the current scene
		/// \return TRUE if successful, FALSE otherwise
		virtual bool PredictRasterizationTime(
			unsigned int num,								///< [in] Number of samples to take when determining how long it will take (higher is more accurate)
			unsigned int* ms,								///< [out] Amount of in ms it would take to rasterize
			unsigned int* actual							///< [out] Actual time it took to do the predicted kernel
			) = 0;

		//! Rasterizes the entire scene
		/// \return TRUE if successful, FALSE otherwise
		virtual bool Rasterize(
			) = 0;

		//! Rasterizes an animation
		/// \return TRUE if successful, FALSE otherwise
		virtual bool RasterizeAnimation(
			const double time_start,						///< [in] Scene time to start rasterizing at
			const double time_end,							///< [in] Scene time to finish rasterizing
			const unsigned int num_frames,					///< [in] Number of frames to rasterize
			const bool do_fields,							///< [in] Should the rasterizer do fields?
			const bool invert_fields						///< [in] Should the fields be temporally inverted?
			) = 0;

		//! Rasterizes an animation using the global preset options
		/// \return TRUE if successful, FALSE otherwise
		virtual bool RasterizeAnimationUsingOptions(
			) = 0;

		//! Rasterizes a frame of an animation using the global preset options
		/// \return TRUE if successful, FALSE otherwise
		virtual bool RasterizeAnimationUsingOptions(
			const unsigned int frame						///< [in] The frame to rasterize
			) = 0;

		//! Rasterizes the scene in this region.  The region values are inclusive!
		/// \return TRUE if successful, FALSE otherwise
		virtual bool RasterizeRegion(
			const unsigned int left,						///< [in] Left most pixel
			const unsigned int top,							///< [in] Top most scanline
			const unsigned int right,						///< [in] Right most pixel
			const unsigned int bottom						///< [in] Bottom most scanline
			) = 0;

		//
		// Transformation of elements
		//

		//! Sets the a given object's position
		/// \return TRUE if successful, FALSE otherwise
		virtual bool SetObjectPosition(
			const char* name,								///< [in] Name of the object
			const double pos[3]								///< [in] Position of the object
			) = 0;

		//! Sets a given object's orientation
		/// \return TRUE if successful, FALSE otherwise
		virtual bool SetObjectOrientation(
			const char* name,								///< [in] Name of the object
			const double orient[3]							///< [in] Orientation of the object
			) = 0;

		//! Sets a given object's scale
		/// \return TRUE if successful, FALSE otherwise
		virtual bool SetObjectScale(
			const char* name,								///< [in] Name of the object
			const double scale								///< [in] Scaling of the object
			) = 0;

		//
		// Object modification functions
		//


		//! Sets the UV generator for an object
		/// \return TRUE if successful, FALSE otherwise
		virtual bool SetObjectUVToSpherical(
			const char* name,								///< [in] Name of the object
			const double radius								///< [in] Radius of the sphere
			) = 0;

		//! Sets the UV generator for an object
		/// \return TRUE if successful, FALSE otherwise
		virtual bool SetObjectUVToBox(
			const char* name,								///< [in] Name of the object
			const double width,								///< [in] Width of the box
			const double height,							///< [in] Height of the box
			const double depth								///< [in] Depth of the box
			) = 0;

		//! Sets the UV generator for an object
		/// \return TRUE if successful, FALSE otherwise
		virtual bool SetObjectUVToCylindrical(
			const char* name,								///< [in] Name of the object
			const double radius,							///< [in] Radius of the cylinder
			const char axis,								///< [in] Axis the cylinder is sitting on
			const double size								///< [in] Size of the cylinder
			) = 0;

		//! Sets the object's surface intersection threshold
		/// \return TRUE if successful, FALSE otherwise
		virtual bool SetObjectIntersectionError(
			const char* name,								///< [in] Name of the object
			const double error								///< [in] Threshold of error
			) = 0;

		//
		// Removal of objects
		//

		//! Removes the given painter from the scene
		/// \return TRUE if successful, FALSE otherwise
		virtual bool RemovePainter(
			const char* name								///< [in] Name of the painter to remove
			) = 0;

		//! Removes the given material from the scene
		/// \return TRUE if successful, FALSE otherwise
		virtual bool RemoveMaterial(
			const char* name								///< [in] Name of the material to remove
			) = 0;

		//! Removes the given geometry from the scene
		/// \return TRUE if successful, FALSE otherwise
		virtual bool RemoveGeometry(
			const char* name								///< [in] Name of the geometry to remove
			) = 0;

		//! Removes the given object from the scene
		/// \return TRUE if successful, FALSE otherwise
		virtual bool RemoveObject(
			const char* name								///< [in] Name of the object to remove
			) = 0;

		//! Removes the given light from the scene
		/// \return TRUE if successful, FALSE otherwise
		virtual bool RemoveLight(
			const char* name								///< [in] Name of the light to remove
			) = 0;

		//! Removes the given modifier from the scene
		/// \return TRUE if successful, FALSE otherwise
		virtual bool RemoveModifier(
			const char* name								///< [in] Name of the modifer to remove
			) = 0;

		//! Clears the entire scene, resets everything back to defaults
		/// \return TRUE if successful, FALSE otherwise
		virtual bool ClearAll(
			) = 0;

		//! Removes all the rasterizer outputs
		/// \return TRUE if successful, FALSE otherwise
		virtual bool RemoveRasterizerOutputs(
			) = 0;

		//! Loading an ascii scene description
		/// \return TRUE if successful, FALSE otherwise
		virtual bool LoadAsciiScene(
			const char* filename							///< [in] Name of the file containing the scene
			) = 0;

		//! Runs an ascii script
		/// \return TRUE if successful, FALSE otherwise
		virtual bool RunAsciiScript(
			const char* filename							///< [in] Name of the file containing the script
			) = 0;

		//! Tells us whether anything is keyframed
		virtual bool AreThereAnyKeyframedObjects() = 0;

		//! Adds a keyframe for the specified element
		virtual bool AddKeyframe(
			const char* element_type,						///< [in] Type of element to keyframe (ie. camera, painter, geometry, object...)
			const char* element,							///< [in] Name of the element to keyframe
			const char* param,								///< [in] Name of the parameter to keyframe
			const char* value,								///< [in] Value at this keyframe
			const double time,								///< [in] Time of the keyframe
			const char* interp,								///< [in] Type of interpolation to use between this keyframe and the next
			const char* interp_params						///< [in] Parameters to pass to the interpolator (this can be NULL)
			) = 0;

		//! Sets animation rasterization options
		//! Basically everything that can be passed to RasterizeAnimation can be passed here
		//! Then you can just call RasterizeAnimationUsingOptions
		virtual bool SetAnimationOptions(
			const double time_start,						///< [in] Scene time to start rasterizing at
			const double time_end,							///< [in] Scene time to finish rasterizing
			const unsigned int num_frames,					///< [in] Number of frames to rasterize
			const bool do_fields,							///< [in] Should the rasterizer do fields?
			const bool invert_fields						///< [in] Should the fields be temporally inverted?
			) = 0;

		//! Reads back the animation options previously set via
		//! SetAnimationOptions, or the defaults (0, 1, 30) for jobs
		//! whose .RISEscene file declared no `animation_options`
		//! chunk.  The interactive editor's timeline scrubber reads
		//! these to size the slider's range and report the duration.
		virtual bool GetAnimationOptions(
			double& time_start,
			double& time_end,
			unsigned int& num_frames,
			bool& do_fields,
			bool& invert_fields
			) const = 0;

		//! Sets progress class to report progress for anything we do
		virtual void SetProgress(
			IProgressCallback* pProgress				///< [in] The progress function
			) = 0;

		//! Loads N PNG/JPEG texture painters in parallel and registers each
		//! one under its requested name.  The decode work (libpng /
		//! libjpeg + the color-space promotion to a working IRasterImage)
		//! runs across the global ThreadPool — a multi-core saturation
		//! pattern that scales with the number of textures the import
		//! needs.  The actual manager registrations happen serially in
		//! the calling thread (manager state is not thread-safe).
		//!
		//! Used by GLTFSceneImporter to decode every texture a scene
		//! references in one batch before the materials loop, instead of
		//! N sequential libpng decodes.  On NewSponza-class assets (137
		//! PNGs) this collapses tens of seconds of decode-thread idle to
		//! single-digit seconds across the worker pool.
		//!
		//! Each request has either filePath set (on-disk asset, used by
		//! .gltf JSON-form imports with external image files) OR bytes +
		//! numBytes set (in-memory, used by .glb embedded images).
		//! `format` selects PNG (0) or JPEG (1).
		//!
		//! Returns true iff every request decoded + registered
		//! successfully.  Individual failures are logged; the batch as a
		//! whole still attempts every request before returning, so a
		//! single malformed PNG doesn't abort the rest.
		//!
		//! `outRequestSuccess` is an OPTIONAL parallel array of length
		//! `numRequests`.  When non-null, each entry is filled with
		//! `true` iff that request both decoded AND registered with the
		//! manager (i.e. is reachable by name afterwards).  Callers that
		//! memoize "this name is now registered" MUST consult this array
		//! — relying on the aggregate bool alone misclassifies failed
		//! requests as registered, which makes downstream lookups fail
		//! silently.  When null the caller doesn't care and only the
		//! aggregate is reported.
		/// \return TRUE if every request succeeded; FALSE if any failed
		virtual bool AddTexturePaintersBatch(
			const TexturePainterBatchRequest* requests,
			size_t numRequests,
			bool* outRequestSuccess = nullptr
			) = 0;

		//! Registers a caller-owned, fully-built triangle mesh geometry under
		//! `name`.  Symmetric with the format-specific `AddXXXTriangleMeshGeometry`
		//! family, but bypasses the loader step — the caller has already filled
		//! the geometry (BeginIndexedTriangles → AddVertex/Normal/etc. →
		//! DoneIndexedTriangles, including BVH build).  Used by importers that
		//! parse a multi-asset container (e.g. glTF) once and emit many
		//! geometries from the cached parse — registering each one through
		//! this method instead of going back through a per-call file loader.
		//! Reference-counts `pGeom` like every other manager registration.
		/// \return TRUE if successful, FALSE otherwise
		virtual bool AddPrebuiltTriangleMeshGeometry(
			const char* name,									///< [in] Name to register the geometry under
			ITriangleMeshGeometryIndexed* pGeom					///< [in] Caller-owned, already-built geometry
			) = 0;
	};


	//! Creates a new empty job
	bool RISE_CreateJob(
			IJob** ppi										///< [out] Pointer to recieve the job
			);


	//////////////////////////////////////////////////////////
	// Library versioning information
	//  These are the same prototypes as those in RISE_API.h
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
}

#endif
