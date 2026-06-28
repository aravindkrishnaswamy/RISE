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
#include "Interfaces/IScalarPainterManager.h"
#include "Interfaces/IMaterialManager.h"
#include "Interfaces/IShaderManager.h"
#include "Interfaces/IShaderOpManager.h"
#include "Interfaces/IModifierManager.h"
#include "Interfaces/IFunction1DManager.h"
#include "Interfaces/IFunction2DManager.h"
#include "Interfaces/IMedium.h"
#include "SceneEditor/OverrideSpanIndex.h"
#include "SceneEditor/SourceSpanIndex.h"
#include "SceneEditor/TransformSnapshot.h"
#include "Utilities/Reference.h"
#include "Utilities/RString.h"
#include "Utilities/ProgressiveConfig.h"
#include "Utilities/RadianceMapConfig.h"
#include "Utilities/PixelFilterConfig.h"
#include "Utilities/SMSConfig.h"
#include "Utilities/SpectralConfig.h"
#include "Utilities/PathGuidingField.h"
#include "Utilities/AdaptiveSamplingConfig.h"
#include "Utilities/StabilityConfig.h"
#include "Utilities/OidnConfig.h"
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace RISE
{
	namespace Cst { struct Document; }   // P5 (save-as-CST): the retained canonical CST; fwd-decl keeps Cst.h out of Job.h

	//! Job - This is used to simplify the creation of a job, all things can be
	//! easily accessed by name, no need to keep track of managers and such
	class Job : public virtual IJobPriv, public Implementation::Reference
	{
	protected:
		virtual ~Job( );

		IScenePriv*									pScene;				// A job can have at most one scene
		IGeometryManager*							pGeomManager;		// Set of all geometry in the job
		ICameraManager*								pCameraManager;		// Set of all cameras in the job
		IPainterManager*							pPntManager;		// Set of all painters in the job
		IScalarPainterManager*						pScalarPntManager;	// Set of all scalar painters in the job
		IFunction1DManager*							pFunc1DManager;		// Set of all 1D functions in the job
		IFunction2DManager*							pFunc2DManager;		// Set of all 2D functions in the job
		IMaterialManager*							pMatManager;		// Set of all materials in the job
		IModifierManager*							pModManager;		// Set of all modifiers in the job
		IObjectManager*								pObjectManager;		// Set of all objects in the job
		ILightManager*								pLightManager;		// Set of all the hacky lights in the job
		IShaderManager*								pShaderManager;		// Set of all shaders in the job
		IShaderOpManager*							pShaderOpManager;	// Set of all shaders ops in the job
		// Currently-active rasterizer.  Borrowed pointer into the
		// registry below; the map owns the addref.
		//
		// CONCURRENCY CONTRACT (matches the rest of `Job`'s mutable
		// state, including `pScene`): mutations to `pRasterizer` MUST
		// NOT race with render-thread reads.  Existing callers honor
		// this through the cancel-and-park machinery in
		// `SceneEditController::SetSelection` (Camera/Rasterizer paths)
		// and `OnTimeScrub`, which trip the cancel flag, wait for
		// `mRendering=false`, and only then mutate.  Future direct
		// callers of `IJob::SetActiveRasterizer` bypassing the
		// controller MUST establish the same precondition before the
		// call.  Plain pointer (not atomic) for consistency with the
		// rest of `Job`'s mutable state — `pScene`, `pCameraManager`,
		// etc. follow the same contract.
		IRasterizer*								pRasterizer;

		// L5d — when true, `AddFileRasterizerOutput` becomes a no-op
		// (logs at info level + returns true so parser doesn't fail).
		// GUI hosts (mac SwiftUI / Windows Qt / Android Compose) flip
		// this on at construction so loading a `.RISEscene` with
		// `file_rasterizeroutput` chunks doesn't silently litter the
		// filesystem with auto-generated PNG/EXR/etc.  Each interactive
		// render in the GUI would otherwise overwrite a file the user
		// didn't ask to be created.  CLI keeps the default (false) so
		// command-line behaviour is byte-identical to legacy.
		bool										m_suppressFileRasterizerOutputs = false;

		// Phase 6.1 (docs/ROUND_TRIP_SAVE_PLAN.md §6.3 + §7.4).
		// Per-entity source-file metadata + two transform snapshots
		// captured at scene-load time, owned by Job for its lifetime.
		// Populated by AsciiSceneParser; consumed by the save engine
		// (Phase 6.4).  Held by unique_ptr so they're cheap to swap
		// out on rescene if/when we add that path; raw pointers
		// returned by the IJobPriv getters remain stable across the
		// scene's lifetime.
		std::unique_ptr<SourceSpanIndex>			pSourceSpanIndex;
		std::unique_ptr<TransformSnapshot>			pBaseTransforms;
		std::unique_ptr<TransformSnapshot>			pLoadedTransforms;
		// Phase 6.2 (§6.8): per-`override_object` chunk catalog.
		std::unique_ptr<OverrideSpanIndex>			pOverrideSpans;

		// P5 (save-as-CST, Slice 1): the retained canonical CST when the scene was loaded via
		// LoadAsciiSceneViaCst (Model-B: Scene = derive(CST)); null for a legacy LoadAsciiScene.  Slices 3-4
		// (edit/save) patch + serialize this.  unique_ptr<fwd-decl> is safe -- Job::~Job is out-of-line (Job.cpp).
		std::unique_ptr<RISE::Cst::Document>		pCstDocument;

		// L6b — canonical FrameStore allocated at the active camera's
		// dims and threaded into every rasterizer factory.  Per
		// docs/FRAMESTORE_DESIGN.md §7.5 the FrameStore SURVIVES
		// rasterizer swaps: when the user changes the active rasterizer,
		// we keep the same FrameStore so observers (file outputs,
		// platform UI viewports) don't have to re-attach.  Reallocated
		// only when the active camera's resolution changes (rare for
		// production; common for the interactive viewport which scrubs
		// preview-scale).  Held by counted reference; the rasterizer's
		// own ctor addrefs (see Rasterizer.cpp), so this Job-side
		// reference is independent — releasing the rasterizer doesn't
		// destroy the FrameStore until Job releases too.  Null until
		// the first `Set*Rasterizer` call resolves an active camera.
		Implementation::FrameStore*					m_jobFrameStore = nullptr;

		// Ensure m_jobFrameStore exists at the requested dims.  If
		// already allocated at matching dims, returns the existing
		// pointer (no reallocation).  If dims differ or the FrameStore
		// hasn't been allocated yet, releases any previous instance
		// and allocates fresh.  Returns nullptr if width or height
		// are zero (caller should pass nullptr to the factory).
		Implementation::FrameStore* EnsureJobFrameStore_locked(
			unsigned int width, unsigned int height );

		// L6b — convenience wrapper: looks up the active camera's dims
		// and calls EnsureJobFrameStore_locked.  Used by every
		// `Set*Rasterizer` impl to thread a non-null FrameStore into
		// the factory.  Returns nullptr if no scene / no active
		// camera (the rasterizer's Phase 1 internal-IRasterImage path
		// keeps working).
		Implementation::FrameStore* ResolveJobFrameStoreForActiveCamera();

		// L6b — Push the canonical FrameStore to every rasterizer in
		// the registry.  Called after scene load completes (and any
		// other moment the active camera's dims may have changed
		// after rasterizer construction), so the rasterizer receives
		// the FrameStore it could not get at construction time
		// (typical scene files declare rasterizer BEFORE camera).
		void PushJobFrameStoreToRasterizers();

	public:
		//! Snapshot of every parameter each `Set*Rasterizer` accepts.
		//! Recorded into `rasterizerRegistry` alongside the instance
		//! so editing a single param can re-instantiate with all the
		//! other params preserved (vs. re-running the parser on the
		//! full chunk text).
		//!
		//! In-class initializers below mirror the canonical universal
		//! defaults from `Utilities/RasterizerDefaults.h`.  Per-type
		//! `Set*Rasterizer` methods overwrite the relevant subset
		//! before the snapshot is stored, so these initializers only
		//! matter for fields the active rasterizer doesn't touch
		//! (e.g. MLT-specific fields on a snapshot captured for BDPT).
		//! The `*Defaults` structs in RasterizerDefaults.h are the
		//! single source of truth — keep these in sync.
		struct RasterizerParams
		{
			// Common subset (most types take these)
			unsigned int        numPixelSamples = 32;       // BaseRasterizerDefaults::numPixelSamples
			unsigned int        maxEyeDepth     = 8;        // BDPT/VCM canonical (MLT overrides to 10)
			unsigned int        maxLightDepth   = 8;        // BDPT/VCM canonical (MLT overrides to 10)
			std::string         shader;
			bool                showLuminaires  = true;     // BaseRasterizerDefaults::showLuminaires
			bool                oidnDenoise     = true;     // BaseRasterizerDefaults::oidnDenoise (MLT overrides to false)
			OidnQuality         oidnQuality     = OidnQuality::Auto;
			OidnDevice          oidnDevice      = OidnDevice::Auto;
			OidnPrefilter       oidnPrefilter   = OidnPrefilter::Fast;

			// PixelBased (legacy) extras
			unsigned int        maxRecursion    = 10;       // PixelPelDefaults::maxRecursion
			unsigned int        numLumSamples   = 1;        // PixelPelDefaults::numLumSamples
			std::string         luminarySampler = "none";   // PixelPelDefaults::luminarySampler
			double              luminarySamplerParam = 1.0; // PixelPelDefaults::luminarySamplerParam
			bool                integrateRGB    = false;    // PixelIntegratingSpectralDefaults::integrateRGB

			// VCM-specific
			double              mergeRadius     = 0.0;      // VCMPelDefaults::mergeRadius
			bool                enableVC        = true;     // VCMPelDefaults::enableVC
			bool                enableVM        = true;     // VCMPelDefaults::enableVM

			// MLT-specific (production values matching MLTDefaults)
			unsigned int        nBootstrap         = 100000;
			unsigned int        nChains            = 512;
			unsigned int        nMutationsPerPixel = 32;
			double              largeStepProb      = 0.3;

			// auto_rasterizer-specific: the author's integrator pin so the
			// snapshot round-trips the dispatcher faithfully (Auto = decide).
			AutoIntegratorChoice autoIntegrator    = AutoIntegratorChoice::Auto;
			// auto_rasterizer-specific: the Tier-2 probe master enable.
			bool                 autoProbeEnabled  = false;

			// Configs (default-constructed = struct in-class initializers)
			RadianceMapConfig      radianceMap;
			PixelFilterConfig      pixelFilter;
			SMSConfig              sms;
			SpectralConfig         spectral;
			PathGuidingConfig      pathGuiding;
			AdaptiveSamplingConfig adaptive;
			StabilityConfig        stability;
			ProgressiveConfig      progressive;
		};

		struct RasterizerEntry
		{
			IRasterizer*     instance;     // Owned by the registry (addref'd).
			RasterizerParams params;       // Snapshot for re-instantiation.
		};

		// Public so an anonymous-namespace helper in Job.cpp
		// (`ComputeRasterizerUnion`) can take the typedef as a
		// parameter without hoisting the helper into Job's class body.
		typedef std::map<std::string, RasterizerEntry> RasterizerRegistry;

	protected:
		// Rasterizer registry — each successful Set*Rasterizer
		// instantiates a new IRasterizer, captures all of its input
		// params into a `RasterizerParams` snapshot, and stores both
		// here keyed by chunk-name.  Replacing an entry under the same
		// key releases the prior instance.  pRasterizer borrows a
		// pointer from this map; the map owns the addref.  Edit-and-
		// rebuild reads the snapshot, modifies one param, and re-calls
		// the matching Set*Rasterizer — preserving all other params.
		RasterizerRegistry							rasterizerRegistry;
		std::string									activeRasterizerName;

		// Replace any existing entry under `name` in the rasterizer
		// registry with `pRaster` + `params`, then make it the active
		// rasterizer.  Takes ownership of an existing addref on
		// `pRaster`; caller should not release after handing the
		// pointer over.  All Set*Rasterizer methods funnel through
		// this helper so the registry + snapshot stay in lockstep
		// with the active pointer.
		void RegisterAndActivateRasterizer( const std::string& name, IRasterizer* pRaster,
			const RasterizerParams& params );

		IProgressCallback*							pGlobalProgress;	// A global progress reporter

		double										lightSampleRRThreshold;	// Light-sample RR threshold (0=disabled)

		typedef std::map<String, IMedium*>		MediumMap;
		MediumMap									mediaMap;				// Named participating media

		// Materials registered via a composing factory
		// (`AddPBRMetallicRoughnessMaterial`, `AddGGXEmissiveMaterial`)
		// rather than a direct `Add*Material` call.  The interactive
		// editor's Materials panel consults `IsMaterialComposed` to
		// surface these as read-only — rebinding a slot on a composed
		// material would break the painter-graph contract the
		// composition relies on (e.g. PBR-MR's metallic/roughness
		// blend chain).  Set is keyed by the material's manager-
		// registered name and grows-only (no removal path today).
		std::set<String>							composedMaterialNames;
		// Scene variants (doc 63): named selectable overlays.  P0 records name->active_camera + the active
		// selection (for GUI/save + the derive-end active-camera apply); the material override BAKE is
		// derive-local (Cst.cpp), so there is no override store here.
		std::map<String,String>						sceneVariantCameras;	// scene_variant name -> active_camera ("" = none)
		String										activeSceneVariant;	// "" / "none" => base default

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
		ICameraManager*				GetCameras()		{ return pCameraManager; };
		IPainterManager*			GetPainters()		{ return pPntManager; };
		IScalarPainterManager*		GetScalarPainters()	{ return pScalarPntManager; };
		IFunction1DManager*			GetFunction1Ds()	{ return pFunc1DManager; };
		IFunction2DManager*			GetFunction2Ds()	{ return pFunc2DManager; };
		IMaterialManager*			GetMaterials()		{ return pMatManager; };
		IShaderManager*				GetShaders()		{ return pShaderManager; };
		IShaderOpManager*			GetShaderOps()		{ return pShaderOpManager; };
		IModifierManager*			GetModifiers()		{ return pModManager; };
		IObjectManager*				GetObjects()		{ return pObjectManager; };
		ILightManager*				GetLights()			{ return pLightManager; };
		IRasterizer*				GetRasterizer()		{ return pRasterizer; };

		// Phase 6.1 getters.  Mutable versions return non-const pointers
		// for AsciiSceneParser's population pass; const versions are
		// what the save engine consumes.  Pointers stable across the
		// Job's lifetime (the unique_ptr's are populated in
		// InitializeContainers and never reseated).  Inline like the
		// other manager getters; matches Job's no-`override` convention
		// (see SetSuppressFileRasterizerOutputs comment).
		SourceSpanIndex*			GetSourceSpanIndexMutable()			{ return pSourceSpanIndex.get(); }
		const SourceSpanIndex*		GetSourceSpanIndex() const			{ return pSourceSpanIndex.get(); }
		TransformSnapshot*			GetBaseTransformSnapshotMutable()	{ return pBaseTransforms.get(); }
		const TransformSnapshot*	GetBaseTransformSnapshot() const	{ return pBaseTransforms.get(); }
		TransformSnapshot*			GetLoadedTransformSnapshotMutable()	{ return pLoadedTransforms.get(); }
		const TransformSnapshot*	GetLoadedTransformSnapshot() const	{ return pLoadedTransforms.get(); }
		OverrideSpanIndex*			GetOverrideSpanIndexMutable()		{ return pOverrideSpans.get(); }
		const OverrideSpanIndex*	GetOverrideSpanIndex() const		{ return pOverrideSpans.get(); }

		// L5d — suppress file_rasterizeroutput at parse time.
		// See member-variable comment for rationale.
		// `override` is intentionally OMITTED to match the rest of
		// Job's member-function style (no other override of an IJob
		// method in this class is marked `override`).  Adding it on
		// just these two would trip
		// `-Winconsistent-missing-override` against every other
		// method in the file.  Adding `override` everywhere is the
		// strictly-more-correct fix but is out of scope for this
		// landing.
		void SetSuppressFileRasterizerOutputs( bool suppress )
			{ m_suppressFileRasterizerOutputs = suppress; }
		bool GetSuppressFileRasterizerOutputs() const
			{ return m_suppressFileRasterizerOutputs; }


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

		bool SetLightSampleRRThreshold(
			const double threshold									///< [in] RR threshold (0=disabled)
			);

		//! Replaces the active Film.  See IJob.h for semantics.
		bool SetFilm(
			const unsigned int width,
			const unsigned int height,
			const double pixelAR
			);

		//! Shrink the active Film to fit a viewport surface + long-edge
		//! cap, preserving aspect.  See IJob.h for semantics.
		bool ScaleFilmToFit(
			const unsigned int maxSurfaceW,
			const unsigned int maxSurfaceH,
			const unsigned int maxLongEdge
			);

		//
		// Cameras — see IJob.h for the multi-camera contract.  Each
		// AddXxxCamera registers the camera under `name` in the
		// scene's camera manager AND makes it active; SetActiveCamera
		// switches which camera the rasterizer draws through;
		// RemoveCamera unregisters one (auto-promoting if it was
		// active).
		//

		//! Adds a pinhole camera (dims read from active Film)
		bool AddPinholeCamera(
			const char* name,										///< [in] Name to register the camera under
			const double ptLocation[3],
			const double ptLookAt[3],
			const double vUp[3],
			const double fov,
			const double exposure,
			const double scanningRate,
			const double pixelRate,
			const double orientation[3],
			const double target_orientation[2],
			const double iso = 0.0,
			const double fstop = 0.0
			);

		//! Adds an ONB pinhole camera (dims read from active Film)
		bool AddPinholeCameraONB(
			const char* name,										///< [in] Name to register the camera under
			const double ONB_U[3],
			const double ONB_V[3],
			const double ONB_W[3],
			const double ptLocation[3],
			const double fov,
			const double exposure,
			const double scanningRate,
			const double pixelRate,
			const double iso = 0.0,
			const double fstop = 0.0
			);

		//! Adds a thin-lens camera.  See IJob::AddThinlensCamera for
		//! the unit contract (sensor / focal / shift in mm; focus in
		//! scene units; sceneUnitMeters bridges them).
		bool AddThinlensCamera(
			const char* name,										///< [in] Name to register the camera under
			const double ptLocation[3],
			const double ptLookAt[3],
			const double vUp[3],
			const double sensorSize,
			const double focalLength,
			const double fstop,
			const double focusDistance,
			const double sceneUnitMeters,
			const double exposure,
			const double scanningRate,
			const double pixelRate,
			const double orientation[3],
			const double target_orientation[2],
			const unsigned int apertureBlades,
			const double apertureRotation,
			const double anamorphicSqueeze,
			const double tiltX,
			const double tiltY,
			const double shiftX,
			const double shiftY,
			const double iso = 0.0
			);

		//! Adds a fisheye camera (dims read from active Film)
		bool AddFisheyeCamera(
			const char* name,										///< [in] Name to register the camera under
			const double ptLocation[3],
			const double ptLookAt[3],
			const double vUp[3],
			const double exposure,
			const double scanningRate,
			const double pixelRate,
			const double orientation[3],
			const double target_orientation[2],
			const double scale
			);

		//! Adds an orthographic camera (dims read from active Film)
		bool AddOrthographicCamera(
			const char* name,										///< [in] Name to register the camera under
			const double ptLocation[3],
			const double ptLookAt[3],
			const double vUp[3],
			const double vpScale[2],
			const double exposure,
			const double scanningRate,
			const double pixelRate,
			const double orientation[3],
			const double target_orientation[2]
			);

		//! Designates the named camera as the scene's active one.
		bool SetActiveCamera( const char* name );

		//! See IJob.h for the contract.  Empty string when no active.
		std::string GetActiveCameraName() const;

		//! Removes the named camera; auto-promotes if it was active.
		bool RemoveCamera( const char* name );

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

		//! Adds a controlled-smoothness radial-bump painter (test/diagnostic).
		bool AddControlledSmoothness2DPainter(
									const char* name,
									const char* pa,
									const char* pb,
									const double centerU,
									const double centerV,
									const double radius,
									const double amplitude,
									const unsigned int smoothnessMode
									);

		//! Adds a polynomial-based Function2D painter.
		/// \return TRUE if successful, FALSE otherwise
		bool AddPolynomialFunction2DPainter(
									const char* name,
									const char* pa,
									const char* pb,
									const unsigned int polynomialType,
									const double center[2],
									const double scale[2],
									const double amplitude,
									const unsigned int degree,
									const unsigned int powerX,
									const unsigned int powerY,
									const double* pCoeffs,
									const unsigned int nCoeffs
									);

		//! Adds a composable Function2D painter.
		/// \return TRUE if successful, FALSE otherwise
		bool AddCompositeFunction2DPainter(
									const char* name,
									const char* pa,
									const char* pb,
									const char* childA,
									const char* childB,
									const unsigned int op,
									const double weightA,
									const double uvScaleA[2],
									const double uvOffsetA[2],
									const double weightB,
									const double uvScaleB[2],
									const double uvOffsetB[2],
									const double lerpT,
									const double outputScale,
									const double outputOffset
									);

		//! Adds a sum-of-sines water-wave painter (Gerstner height variant).
		/// \return TRUE if successful, FALSE otherwise
		bool AddGerstnerWavePainter(
									const char* name,
									const char* pa,
									const char* pb,
									const unsigned int numWaves,
									const double medianWavelength,
									const double wavelengthRange,
									const double medianAmplitude,
									const double amplitudePower,
									const double windDir[2],
									const double directionalSpread,
									const double dispersionSpeed,
									const unsigned int seed,
									const double time
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

		bool AddWavelet3DPainter(
									const char* name,
									const unsigned int nTileSize,
									const double dPersistence,
									const unsigned int nOctaves,
									const char* pa,
									const char* pb,
									const double vScale[3],
									const double vShift[3]
									);

		bool AddReactionDiffusion3DPainter(
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
									);

		bool AddGabor3DPainter(
									const char* name,
									const double dFrequency,
									const double dBandwidth,
									const double vOrientation[3],
									const double dImpulseDensity,
									const char* pa,
									const char* pb,
									const double vScale[3],
									const double vShift[3]
									);

		bool AddSimplex3DPainter(
									const char* name,
									const double dPersistence,
									const unsigned int nOctaves,
									const char* pa,
									const char* pb,
									const double vScale[3],
									const double vShift[3]
									);

		bool AddSDF3DPainter(
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
									);

		bool AddCurlNoise3DPainter(
									const char* name,
									const double dPersistence,
									const unsigned int nOctaves,
									const double dEpsilon,
									const char* pa,
									const char* pb,
									const double vScale[3],
									const double vShift[3]
									);

		bool AddDomainWarp3DPainter(
									const char* name,
									const double dPersistence,
									const unsigned int nOctaves,
									const double dWarpAmplitude,
									const unsigned int nWarpLevels,
									const char* pa,
									const char* pb,
									const double vScale[3],
									const double vShift[3]
									);

		bool AddPerlinWorley3DPainter(
									const char* name,
									const double dPersistence,
									const unsigned int nOctaves,
									const double dWorleyJitter,
									const double dBlend,
									const char* pa,
									const char* pb,
									const double vScale[3],
									const double vShift[3]
									);

		//! Adds a 3D Worley (cellular) noise painter
		/// \return TRUE if successful, FALSE otherwise
		bool AddWorley3DPainter(
									const char* name,				///< [in] Name of the painter
									const double dJitter,			///< [in] Jitter amount [0,1]
									const unsigned int nMetric,		///< [in] Distance metric
									const unsigned int nOutput,		///< [in] Output mode
									const char* pa,					///< [in] First painter
									const char* pb,					///< [in] Second painter
									const double vScale[3],			///< [in] How much to scale the function by
									const double vShift[3]			///< [in] How much to shift the function by
									);

		//! Adds a 3D turbulence noise painter
		/// \return TRUE if successful, FALSE otherwise
		bool AddTurbulence3DPainter(
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
									const double shift[3],			///< [in] Shift factor for color values
									const char wrap_s = 0,			///< [in] U-axis wrap mode (see IJob.h / eRasterWrapMode)
									const char wrap_t = 0,			///< [in] V-axis wrap mode
									const bool mipmap = true,		///< [in] Build mip pyramid + LOD-aware sampling (Landing 2)
									const SpectrumKind spectrumKind = eSpectrumKind_Albedo	///< [in] L3 spectral-uplift role
									);

		//! Adds a JPEG texture painter
		/// \return TRUE if successful, FALSE otherwise
		bool AddJPEGTexturePainter(
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
									const char wrap_s = 0,			///< [in] U-axis wrap mode
									const char wrap_t = 0,			///< [in] V-axis wrap mode
									const bool mipmap = true,		///< [in] Build mip pyramid + LOD-aware sampling (Landing 2)
									const SpectrumKind spectrumKind = eSpectrumKind_Albedo	///< [in] L3 spectral-uplift role
									);

		bool AddInMemoryPNGTexturePainter(
									const char* name,
									const unsigned char* bytes,
									const size_t numBytes,
									const char color_space,
									const char filter_type,
									const bool lowmemory,
									const double scale[3],
									const double shift[3],
									const char wrap_s = 0,
									const char wrap_t = 0,
									const bool mipmap = true,		///< [in] Build mip pyramid + LOD-aware sampling (Landing 2)
									const SpectrumKind spectrumKind = eSpectrumKind_Albedo	///< [in] L3 spectral-uplift role
									);

		bool AddInMemoryJPEGTexturePainter(
									const char* name,
									const unsigned char* bytes,
									const size_t numBytes,
									const char color_space,
									const char filter_type,
									const bool lowmemory,
									const double scale[3],
									const double shift[3],
									const char wrap_s = 0,
									const char wrap_t = 0,
									const bool mipmap = true,		///< [in] Build mip pyramid + LOD-aware sampling (Landing 2)
									const SpectrumKind spectrumKind = eSpectrumKind_Albedo	///< [in] L3 spectral-uplift role
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
									const double shift[3],			///< [in] Shift factor for color values
									const char wrap_s = 0,			///< [in] U-axis wrap mode
									const char wrap_t = 0,			///< [in] V-axis wrap mode
									const SpectrumKind spectrumKind = eSpectrumKind_Unbounded	///< [in] L3: spectral-uplift role
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
									const double shift[3],			///< [in] Shift factor for color values
									const char wrap_s = 0,			///< [in] U-axis wrap mode
									const char wrap_t = 0,			///< [in] V-axis wrap mode
									const SpectrumKind spectrumKind = eSpectrumKind_Unbounded	///< [in] L3: spectral-uplift role
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
									const double shift[3],			///< [in] Shift factor for color values
									const char wrap_s = 0,			///< [in] U-axis wrap mode
									const char wrap_t = 0			///< [in] V-axis wrap mode
									);

		//! Adds a painter that paints a uniform color
		/// \return TRUE if successful, FALSE otherwise
		bool AddUniformColorPainter(
									const char* name,				///< [in] Name of the painter
									const double pel[3],			///< [in] Color to paint
									const char* cspace				///< [in] Color space of the given color
									);

		//! Adds a painter that returns the per-vertex color interpolated by the
		//! geometry at the hit point.  Falls back to the supplied default for
		//! hits on geometry without vertex colors.
		/// \return TRUE if successful, FALSE otherwise
		bool AddVertexColorPainter(
									const char* name,				///< [in] Name of the painter
									const double fallback[3],		///< [in] Default color when no vertex color is present
									const char* cspace				///< [in] Color space of the fallback color
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

		//! Adds a channel-extraction painter.  See IJob.h for the doc.
		/// \return TRUE if successful, FALSE otherwise
		bool AddChannelPainter(
									const char* name,				///< [in] Name of the painter
									const char* source,				///< [in] Source painter
									const char  channel,			///< [in] 0=R, 1=G, 2=B
									const double scale,				///< [in] Multiplier on extracted channel
									const double bias				///< [in] Additive offset after scale
									);

		//! Adds a UV-transform wrapper painter.  See IJob.h for the doc.
		/// \return TRUE if successful, FALSE otherwise
		bool AddUVTransformPainter(
									const char* name,				///< [in] Name of the painter
									const char* source,				///< [in] Source painter
									const double offset_u,			///< [in] U translation
									const double offset_v,			///< [in] V translation
									const double rotation,			///< [in] Rotation in radians (KHR sign)
									const double scale_u,			///< [in] U scale
									const double scale_v			///< [in] V scale
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
									const bool hg,					///< [in] Use Henyey-Greenstein phase function scattering
									const Scalar arN,
									const Scalar arK,
									const Scalar arThickness
									);

		//! Creates a SubSurface Scattering material
		/// \return TRUE if successful, FALSE otherwise
		bool AddSubSurfaceScatteringMaterial(
									const char* name,				///< [in] Name of the material
									const char* ior,				///< [in] Index of refraction
									const char* absorption,			///< [in] Absorption coefficient
									const char* scattering,			///< [in] Scattering coefficient
									const char* g,					///< [in] HG asymmetry parameter
									const char* roughness			///< [in] Surface roughness [0,1]
									);

		//! Creates a Random Walk SSS material
		/// \return TRUE if successful, FALSE otherwise
		bool AddRandomWalkSSSMaterial(
									const char* name,				///< [in] Name of the material
									const char* ior,				///< [in] Index of refraction
									const char* absorption,			///< [in] Absorption coefficient
									const char* scattering,			///< [in] Scattering coefficient
									const char* g,					///< [in] HG asymmetry parameter
									const char* roughness,			///< [in] Surface roughness [0,1]
									const char* maxBounces			///< [in] Maximum walk steps
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

		//! Adds a Donner & Jensen 2008 spectral skin BSSRDF material
		bool AddDonnerJensenSkinBSSRDFMaterial(
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
			const double thickness,										///< [in] Thickness between the materials
			const char* extinction										///< [in] Extinction painter name
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

		//! Adds GGX anisotropic microfacet material
		/// \return TRUE if successful, FALSE otherwise
		bool AddGGXEmissiveMaterial(
			const char* name,
			const char* diffuse,
			const char* specular,
			const char* alphaX,
			const char* alphaY,
			const char* ior,
			const char* ext,
			const char* emissive,
			const double emissive_scale,
			const char* fresnel_mode = "conductor",
			const char* tangent_rotation = "none",			///< Landing 8 / KHR_materials_anisotropy
			const char* film_ior = "none",					///< Thin-film FILM (oxide) n; thinfilm mode only
			const char* film_extinction = "none",			///< Thin-film FILM (oxide) k; thinfilm mode only
			const char* film_thickness = "none"				///< Thin-film FILM (oxide) thickness nm; thinfilm mode only
			);

		bool AddPBRMetallicRoughnessMaterial(
			const char* name,
			const char* base_color,
			const char* metallic,
			const char* roughness,
			const double ior,
			const char* emissive,
			const double emissive_scale,
			const char* specular_factor = "1.0",			///< Landing 7 / KHR_materials_specular
			const char* specular_color = "none",			///< Landing 7 / KHR_materials_specular
			const char* anisotropy_factor = "0.0",			///< Landing 8 / KHR_materials_anisotropy
			const char* anisotropy_rotation = "0.0"			///< Landing 8 / KHR_materials_anisotropy (rotation now applied via GGX{BRDF,SPF})
			);

		bool AddGGXMaterial(
			const char* name,											///< [in] Name of the material
			const char* diffuse,										///< [in] Diffuse reflectance
			const char* specular,										///< [in] Specular reflectance
			const char* alphaX,											///< [in] Roughness in tangent u direction
			const char* alphaY,											///< [in] Roughness in tangent v direction
			const char* ior,											///< [in] Index of refraction
			const char* ext,											///< [in] Extinction coefficient
			const char* fresnel_mode = "conductor",
			const char* tangent_rotation = "none",						///< Landing 8 / KHR_materials_anisotropy
			const char* film_ior = "none",								///< Thin-film FILM (oxide) n; thinfilm mode only
			const char* film_extinction = "none",						///< Thin-film FILM (oxide) k; thinfilm mode only
			const char* film_thickness = "none"							///< Thin-film FILM (oxide) thickness nm; thinfilm mode only
			);

		bool AddSheenMaterial(
			const char* name,
			const char* sheen_color,
			const char* sheen_roughness
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
							const bool double_sided,				///< [in] Are the triangles double sided ?
							const bool face_normals					///< [in] Use face normals rather than vertex normals
							);

		//! Creates a triangle mesh geometry from a 3DS file
		/// \return TRUE if successful, FALSE otherwise
		bool Add3DSTriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* filename,					///< [in] The 3DS file to load
							const bool double_sided,				///< [in] Are the triangles double sided ?
							const bool face_normals					///< [in] Use face normals rather than vertex normals
							);

		//! Creates a triangle mesh geometry from a raw file
		/// \return TRUE if successful, FALSE otherwise
		bool AddRAWTriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] Name of the file to load from
							const bool double_sided					///< [in] Are the triangles double sided ?
							);

		//! Creates a triangle mesh geometry from a file of version 2
		//! The format of the file for this version is different from the one
		//! above
		/// \return TRUE if successful, FALSE otherwise
		bool AddRAW2TriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] Name of the file to load from
							const bool double_sided,				///< [in] Are the triangles double sided ?
							const bool face_normals					///< [in] Use face normals rather than vertex normals
							);

		//! Creates a triangle mesh geometry from a ply file
		/// \return TRUE if successful, FALSE otherwise
		bool AddPLYTriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] Name of the file to load from
							const bool double_sided,				///< [in] Are the triangles double sided ?
							const bool bInvertFaces,				///< [in] Should the faces be inverted?
							const bool face_normals					///< [in] Use face normals rather than vertex normals
							);

		//! Bulk-imports a glTF 2.0 scene.  See IJob.h for the full doc.
		/// \return TRUE if successful, FALSE otherwise
		bool ImportGLTFScene(
							const char* filename,
							const char* name_prefix,
							const unsigned int scene_index,
							const bool import_meshes,
							const bool import_materials,
							const bool import_lights,
							const bool import_cameras,
							const bool import_normal_maps,
							const bool lowmem_textures,
							const double lights_intensity_override,
							const double directional_intensity_override,
							const double point_intensity_override,
							const double spot_intensity_override,
							const bool   respect_baked_occlusion = true,
							const double emissive_intensity_scale = 1.0,
							const double emissive_tint_r = 1.0,
							const double emissive_tint_g = 1.0,
							const double emissive_tint_b = 1.0
							);

		//! Creates a triangle mesh geometry from a glTF 2.0 file.  See IJob.h
		//! for the full doc; Phase 1 of glTF import only loads geometry.
		/// \return TRUE if successful, FALSE otherwise
		bool AddGLTFTriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] .gltf or .glb file to load
							const unsigned int mesh_index,			///< [in] Which mesh in the file (0-based)
							const unsigned int primitive_index,		///< [in] Which primitive within the mesh (0-based)
							const bool double_sided,				///< [in] Are the triangles double sided?
							const bool face_normals,				///< [in] Use face normals rather than vertex normals
							const bool flip_v						///< [in] Flip TEXCOORD V at load
							);

		//! Creates a mesh from a .risemesh file
		/// \return TRUE if successful, FALSE otherwise
		bool AddRISEMeshTriangleMeshGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] Name of the file to load from
							const bool load_into_memory,			///< [in] Do we load the entire file into memory before reading?
							const bool face_normals					///< [in] Use face normals rather than vertex normals
							);

		//! Creates a bezier patch geometry (analytic).  See IJob.h for details.
		/// \return TRUE if successful, FALSE otherwise
		bool AddBezierPatchGeometry(
							const char* name,						///< [in] Name of the geometry
							const char* szFileName,					///< [in] Name of the file to load from
							const unsigned int max_patches,			///< [in] Maximum number of patches per accelerator leaf
							const unsigned char max_recur,			///< [in] Maximum accelerator recursion depth
							const bool use_bsp,						///< [in] Use BSP tree (true) or Octree (false) for the patch accelerator
							const bool bCenterObject				///< [in] Recenter all patch control points around the object-space origin
							);

		//! Creates a signed-distance-field (implicit) geometry from inline part
		//! lines (the normal path) or an external parts file (for very large SDFs).
		bool AddSDFGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* szFileName,					///< [in] SDF parts file to load ("" / "none" = use szParts)
					const char* szParts,					///< [in] Inline newline-separated part lines ("" = use szFileName)
					const unsigned int maxSteps,			///< [in] Sphere-trace step cap (0 = default 256)
					const double surfaceEpsilonFraction,	///< [in] Surface epsilon as a fraction of the bbox diagonal (0 = auto)
					const unsigned int samplingDetail		///< [in] Tessellation cells (longest axis) for surface sampling
					);

		//! Creates a heightfield SDF geometry from a named IFunction2D (see IJob)
		bool AddSDFHeightfieldGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* heightfieldFunction,		///< [in] Named IFunction2D giving f(u,v)
					const double radius,					///< [in] Half-extent of the square domain
					const double scale,						///< [in] World amplitude
					const unsigned int maxSteps,			///< [in] Sphere-trace step cap (0 = default 256)
					const double surfaceEpsilonFraction,	///< [in] Surface epsilon as a fraction of the bbox diagonal (0 = auto)
					const unsigned int samplingDetail		///< [in] Tessellation cells (longest axis) for surface sampling
					);


		//! Creates a general profile sweep (see IJob)
		bool AddSweepGeometry(
					const char* name,						///< [in] Name of the geometry
					const SweepDescriptor& desc				///< [in] Profile + path + taper + cap parameters
					);

		//! Creates along-path instances of a named template geometry (see IJob)
		bool AddPathInstancesGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* szTemplate,					///< [in] Name of the template geometry
					const PathInstancesDescriptor& desc		///< [in] Path + pitch parameters
					);

		//! Wraps a named IFunction2D as a greyscale colour painter (see IJob)
		bool AddFunction2DColorPainter(
					const char* name,						///< [in] Name of the painter
					const char* szFunction,					///< [in] Name of the source IFunction2D
					const double scale,						///< [in] Output scale
					const double bias						///< [in] Output bias
					);

		//! Creates a flat Cartesian-grid circular disk base (see IJob)
		bool AddCartesianDiskGeometry(
					const char* name,						///< [in] Name of the geometry
					const double radius,					///< [in] Disk radius (world units)
					const int meshN							///< [in] Grid samples across the diameter
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

		//! Creates a displaced geometry wrapping a previously-registered base geometry.
		bool AddDisplacedGeometry(
							const char*         name,
							const char*         base_geometry_name,
							const unsigned int  detail,
							const char*         displacement,
							const Scalar        disp_scale,
							const bool          double_sided,
							const bool          face_normals,
					const bool          seam_fold = true );

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
			const bool shootPhotons									///< [in] Should this light shoot photons for photon mapping?
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
			const bool shootPhotons									///< [in] Should this light shoot photons for photon mapping?
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
		// Participating media
		//

		//! Adds a homogeneous participating medium
		/// \return TRUE if successful, FALSE otherwise
		bool AddHomogeneousMedium(
			const char* name,										///< [in] Name of the medium
			const double sigma_a[3],								///< [in] Absorption coefficient (linear RGB)
			const double sigma_s[3],								///< [in] Scattering coefficient (linear RGB)
			const char* phase_type,									///< [in] Phase function type ("isotropic" or "hg")
			const double phase_g									///< [in] Asymmetry factor for HG (ignored for isotropic)
			);

		//! Adds a heterogeneous participating medium driven by volume data
		/// \return TRUE if successful, FALSE otherwise
		bool AddHeterogeneousMedium(
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
			);

		//! Adds a heterogeneous participating medium driven by a painter
		/// \return TRUE if successful, FALSE otherwise
		bool AddPainterHeterogeneousMedium(
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
			);

		//! Sets the scene's global participating medium
		/// \return TRUE if successful, FALSE otherwise
		bool SetGlobalMedium(
			const char* name										///< [in] Name of a previously added medium
			);

		//! Assigns an interior participating medium to an object
		/// \return TRUE if successful, FALSE otherwise
		bool SetObjectInteriorMedium(
			const char* object_name,								///< [in] Name of the object
			const char* medium_name									///< [in] Name of the medium
			);

		//! Look up a registered medium by name; see IJob.h.
		const IMedium* GetMedium(
			const char* name
			) const;

		//! Enumerate registered medium names; see IJob.h.
		void EnumerateMediumNames(
			IEnumCallback<const char*>& cb
			) const;

		//! Return a registered geometry by name; see IJob.h.
		const IGeometry* GetGeometry(
			const char* name
			) const;

		//! Enumerate registered geometry names; see IJob.h.
		void EnumerateGeometryNames(
			IEnumCallback<const char*>& cb
			) const;

		//! True if the named material was registered via a composing
		//! factory; see IJob.h.
		bool IsMaterialComposed(
			const char* name
			) const;


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

		//! Creates a tangent-space normal-map modifier.  See IJob.h for the doc.
		/// \return TRUE if successful, FALSE otherwise
		bool AddNormalMapModifier(
			const char* name,										///< [in] Name of the modifier
			const char* painter,									///< [in] Linear-RGB normal-map painter
			const double scale										///< [in] glTF normalTexture.scale
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
			const RadianceMapConfig& radianceMapConfig,				///< [in] Per-object radiance map (IBL); `isBackground` ignored here
			const double pos[3],									///< [in] Position of the object
			const double orient[3],									///< [in] Orientation of the object
			const double scale[3],									///< [in] Object scaling
			const bool bCastsShadows,								///< [in] Does the object cast shadows?
			const bool bReceivesShadows								///< [in] Does the object receive shadows?
		);

		bool AddObjectMatrix(
			const char* name,
			const char* geom,
			const char* material,
			const char* modifier,
			const char* shader,
			const RadianceMapConfig& radianceMapConfig,
			const double matrix[16],
			const bool bCastsShadows,
			const bool bReceivesShadows
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
			const RadianceMapConfig& radianceMapConfig,				///< [in] Per-object radiance map (IBL); `isBackground` ignored here
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
			const char* bsdf										///< [in] BSDF to use when computing radiance (overrides object BSDF)
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
			const unsigned int min_effective_contributors,			///< [in] Minimum effective contributors required for interpolation
			const double high_variation_reuse_scale,				///< [in] Minimum reuse scale for bright high-variation cache records
			const bool cache										///< [in] Should the rasterizer state cache be used?
			);

		bool AddPathTracingShaderOp(
			const char* name,
			const bool smsEnabled,
			const unsigned int smsMaxIterations,
			const double smsThreshold,
			const unsigned int smsMaxChainDepth,
			const bool smsBiased
			);

		bool AddSMSShaderOp(
			const char* name,
			const unsigned int maxIterations,
			const double threshold,
			const unsigned int maxChainDepth,
			const bool biased
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

		bool AddDonnerJensenSkinSSSShaderOp(
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

		bool AddAlphaTestShaderOp(
			const char* name,
			const char* alpha_painter,
			const double cutoff
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
			);

		//! Sets the rasterizer type to be pixel based spectral integrating
		bool SetPixelBasedSpectralIntegratingRasterizer(
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
			);

		//! Sets the rasterizer type to be Pel (RGB) BDPT
		bool SetBDPTPelRasterizer(
			const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
			const unsigned int maxEyeDepth,							///< [in] Maximum eye subpath depth
			const unsigned int maxLightDepth,						///< [in] Maximum light subpath depth
			const char* shader,										///< [in] The default shader
			const RadianceMapConfig& radianceMapConfig,				///< [in] Global radiance map (IBL) configuration
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
			);

		//! Sets the rasterizer type to be Pel (RGB) Vertex Connection and Merging
		bool SetVCMPelRasterizer(
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
			);

		//! Sets the rasterizer type to be spectral Vertex Connection and Merging
		bool SetVCMSpectralRasterizer(
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
			);

		//! Sets the rasterizer type to be spectral BDPT
		bool SetBDPTSpectralRasterizer(
			const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
			const unsigned int maxEyeDepth,							///< [in] Maximum eye subpath depth
			const unsigned int maxLightDepth,						///< [in] Maximum light subpath depth
			const char* shader,										///< [in] The default shader
			const RadianceMapConfig& radianceMapConfig,				///< [in] Global radiance map (IBL) configuration
			const PixelFilterConfig& pixelFilterConfig,				///< [in] Pixel reconstruction filter
			const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
			const SpectralConfig& spectralConfig,					///< [in] Spectral wavelength range, bins, and sampling strategy
			const bool oidnDenoise,									///< [in] Enable OIDN denoising post-process
			const OidnQuality oidnQuality,							///< [in] OIDN quality preset (Auto = render-time heuristic)
			const OidnDevice oidnDevice,							///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
			const OidnPrefilter oidnPrefilter,						///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
			const PathGuidingConfig& guidingConfig,					///< [in] Path guiding configuration
			const AdaptiveSamplingConfig& adaptiveConfig,			///< [in] Adaptive sampling configuration
			const StabilityConfig& stabilityConfig,					///< [in] Production stability controls
			const ProgressiveConfig& progressiveConfig				///< [in] Progressive multi-pass rendering configuration
			);

		// Auto-routing integrator dispatcher (auto_rasterizer).  No
		// `override` keyword: Job declares ~150 IJob overrides without it
		// and adding it here would wake -Winconsistent-missing-override
		// (see feedback_override_keyword_in_job memory).
		bool SetAutoRasterizer(
			const AutoIntegratorChoice integrator,
			const unsigned int numPixelSamples,
			const char* shader,
			const RadianceMapConfig& radianceMapConfig,
			const PixelFilterConfig& pixelFilterConfig,
			const bool bShowLuminaires,
			const bool oidnDenoise,
			const OidnQuality oidnQuality,
			const OidnDevice oidnDevice,
			const OidnPrefilter oidnPrefilter,
			const PathGuidingConfig& guidingConfig,
			const AdaptiveSamplingConfig& adaptiveConfig,
			const StabilityConfig& stabilityConfig,
			const ProgressiveConfig& progressiveConfig,
			const bool probeEnabled
			);

		// Spectral sibling of SetAutoRasterizer (auto_spectral_rasterizer).
		// No `override` keyword for the same reason (see above / the
		// feedback_override_keyword_in_job memory).
		bool SetAutoSpectralRasterizer(
			const AutoIntegratorChoice integrator,
			const unsigned int numPixelSamples,
			const char* shader,
			const RadianceMapConfig& radianceMapConfig,
			const PixelFilterConfig& pixelFilterConfig,
			const bool bShowLuminaires,
			const SpectralConfig& spectralConfig,
			const bool oidnDenoise,
			const OidnQuality oidnQuality,
			const OidnDevice oidnDevice,
			const OidnPrefilter oidnPrefilter,
			const AdaptiveSamplingConfig& adaptiveConfig,
			const StabilityConfig& stabilityConfig,
			const ProgressiveConfig& progressiveConfig,
			const bool probeEnabled
			);

		bool SetPathTracingPelRasterizer(
			const unsigned int numPixelSamples,
			const char* shader,
			const RadianceMapConfig& radianceMapConfig,
			const PixelFilterConfig& pixelFilterConfig,
			const bool bShowLuminaires,
			const SMSConfig& smsConfig,
			const bool oidnDenoise,
			const OidnQuality oidnQuality,
			const OidnDevice oidnDevice,
			const OidnPrefilter oidnPrefilter,
			const PathGuidingConfig& guidingConfig,
			const AdaptiveSamplingConfig& adaptiveConfig,
			const StabilityConfig& stabilityConfig,
			const ProgressiveConfig& progressiveConfig
			);

		bool SetPathTracingSpectralRasterizer(
			const unsigned int numPixelSamples,
			const char* shader,
			const RadianceMapConfig& radianceMapConfig,
			const PixelFilterConfig& pixelFilterConfig,
			const bool bShowLuminaires,
			const SpectralConfig& spectralConfig,
			const SMSConfig& smsConfig,
			const bool oidnDenoise,
			const OidnQuality oidnQuality,
			const OidnDevice oidnDevice,
			const OidnPrefilter oidnPrefilter,
			const AdaptiveSamplingConfig& adaptiveConfig,
			const StabilityConfig& stabilityConfig,
			const ProgressiveConfig& progressiveConfig
			);

		// Unhide the legacy (pre-filter) IJob overloads so external code
		// that holds a concrete Job* or Job& can still call them.
		// Without these using-declarations, declaring the filter-aware
		// overrides below would HIDE the legacy overloads inherited
		// from IJob — so `job->SetMLTRasterizer(legacy_args)` on a
		// Job* would fail to compile even though the inline wrapper
		// exists in IJob.  The `using` brings the base-class overloads
		// into Job's scope, restoring overload resolution.
		using IJob::SetMLTRasterizer;
		using IJob::SetMLTSpectralRasterizer;

		bool SetMLTRasterizer(
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
			);

		bool SetMLTSpectralRasterizer(
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
																	///		6 - EXR
			const unsigned char bpp,								///< [in] Bits / pixel for the file
			const char color_space,									///< [in] Color space to apply
																	///		0 - Rec709 RGB linear
																	///		1 - sRGB profile
																	///		2 - ROMM RGB (ProPhotoRGB) linear
																	///		3 - ROMM RGB (ProPhotoRGB) non-linear
			const double exposureEV,								///< [in] Exposure offset in EV stops, LDR formats only
			const char display_transform,							///< [in] Display tone curve, LDR formats only (0=none,1=reinhard,2=aces,3=agx,4=hable)
			const char exr_compression,								///< [in] EXR compression (0=none,1=zip,2=piz,3=dwaa); EXR only
			const bool exr_with_alpha								///< [in] EXR alpha channel; EXR only
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
			const double max_spacing,						///< [in] Maximum seperation
			const double query_threshold_scale,				///< [in] Scale for the query acceptance threshold
			const double neighbor_spacing_scale				///< [in] Scale for capping reuse radius by local neighbor spacing
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
			const bool shootFromNonMeshLights,				///< [in] Should we shoot from non mesh based lights?
			const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
			const bool regenerate,							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
			const bool shootFromMeshLights = true			///< [in] Should we shoot from mesh based lights (luminaries)?
			);

		//! Shoots global photons and populates the global pel photon map
		/// \return TRUE if successful, FALSE otherwise
		bool ShootGlobalPelPhotons(
			const unsigned int num,							///< [in] Number of photons to acquire
			const double power_scale,						///< [in] How much to scale light power by
			const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
			const double minImportance,						///< [in] Minimum importance when a photon is discarded
			const bool branch,								///< [in] Should the tracer branch or follow a single path?
			const bool shootFromNonMeshLights,				///< [in] Should we shoot from non mesh based lights?
			const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
			const bool regenerate,							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
			const bool shootFromMeshLights = true			///< [in] Should we shoot from mesh based lights (luminaries)?
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
			const bool shootFromNonMeshLights,				///< [in] Should we shoot from non mesh based lights?
			const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
			const bool regenerate,							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
			const bool shootFromMeshLights = true			///< [in] Should we shoot from mesh based lights (luminaries)?
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
		// `> modify` runtime-mutation surface (see IJob.h).  No `override`
		// keyword — Job matches the file's existing no-override style (see
		// CLAUDE.md's note on -Winconsistent-missing-override).
		//

		//! Reassigns the material bound to an existing scene object.
		/// \return TRUE if both names resolve, FALSE otherwise
		bool SetObjectMaterial(
			const char* objName,							///< [in] Name of the object to retarget
			const char* materialName						///< [in] Name of the material to bind
			);

		//! Reassigns the shader bound to an existing scene object.
		/// \return TRUE if both names resolve, FALSE otherwise
		bool SetObjectShader(
			const char* objName,							///< [in] Name of the object to retarget
			const char* shaderName							///< [in] Name of the shader to bind
			);

		//! Rescales the emission of a luminaire material.
		/// \return TRUE if the material exists and is a luminaire, FALSE otherwise
		bool SetMaterialEmissionScale(
			const char* materialName,						///< [in] Name of the luminaire material
			const double scale								///< [in] New emission scale
			);

		//! Overrides the active rasterizer's environment radiance scale.
		/// \return TRUE if an active rasterizer with a RayCaster exists, FALSE otherwise
		bool SetActiveRasterizerRadianceScale(
			const double scale								///< [in] New environment radiance scale
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

		//! Invalidates the top-level acceleration structure (see IJob).  Annotated
		//! without `override` to match this file's house style (Job omits it on all
		//! IJob virtuals; adding it here would wake -Winconsistent-missing-override).
		void InvalidateSpatialStructure();

		//! Bumps the scene's light-topology generation (see IJob).
		void BumpLightTopologyGeneration();

		//! Reads the scene's light-topology generation (see IJob).  No `override` to
		//! match this file's house style.
		unsigned int GetLightTopologyGeneration() const;

		//! Object-override accounting (see IJob).  No `override` (house style).
		void NoteObjectOverride();
		unsigned int GetObjectOverrideCount() const;

		//! Enables/disables incremental re-point mode (see IJob).  No `override` to
		//! match this file's house style.
		void SetIncrementalRepointMode( bool b );

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

		//! P5 Slice 1: load a scene by building the canonical CST (ParseToCst) and deriving the Scene from
		//! it (DeriveToJob), RETAINING the Document for edit/save (Model-B "Scene = derive(CST)").  Additive +
		//! flagged -- the legacy LoadAsciiScene above stays the default.  Input must be NATIVE v7-form
		//! (macro/expr/directive-free) -- ENFORCED via Cst::IsNativeV7Document: an UN-migrated top-level construct
		//! (a `FOR`/`ENDFOR` loop, a `> run`/`> load` include, or a render-AFFECTING `>` directive -- `> modify`,
		//! `> set <other>`) or a missing `RISE ASCII SCENE` header is REFUSED (returns false), NOT silently
		//! mis-derived.  (Only render-NEUTRAL `>` directives -- `> echo`, `> set accelerator` -- are accepted;
		//! DeriveToJob skips them render-neutrally.)  The v6 corpus is converted OFFLINE (migrator, plan Slice 2).
		//! Load-once: re-loading a live Job is refused.
		/// \return TRUE iff native v7-form AND derived with no error diagnostics
		bool LoadAsciiSceneViaCst(
			const char* filename
			);

		//! P5: the retained canonical CST (null unless the scene was loaded via LoadAsciiSceneViaCst).
		const RISE::Cst::Document*	GetCstDocument() const { return pCstDocument.get(); }

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

		//! Read back the animation options previously set via
		//! SetAnimationOptions or the defaults if no animation_options
		//! chunk was parsed.
		bool GetAnimationOptions(
			double& time_start,
			double& time_end,
			unsigned int& num_frames,
			bool& do_fields,
			bool& invert_fields
			) const;

		//
		// Named animation paths
		//
		bool DeclareAnimation(
			const char* name,
			const double time_start,
			const double time_end,
			const unsigned int num_frames,
			const bool do_fields,
			const bool invert_fields,
			const bool make_active
			);
		bool AddKeyframeToAnimation(
			const char* element_type,
			const char* element,
			const char* param,
			const char* value,
			const double time,
			const char* interp,
			const char* interp_params,
			const char* animation
			);
		bool SetActiveAnimation( const char* name );
		bool SetActiveAnimationByIndex( const unsigned int index );
		unsigned int GetAnimationCount() const;
		bool GetAnimationName( const unsigned int index, char* buf, const unsigned int bufLen ) const;
		unsigned int GetActiveAnimationIndex() const;
		bool GetActiveAnimationName( char* buf, const unsigned int bufLen ) const;
		bool DeclareSceneVariant( const char* name, const char* active_camera );
		bool SetActiveSceneVariant( const char* name );
		bool GetActiveSceneVariant( char* buf, const unsigned int bufLen ) const;
		const std::map<String,String>& GetSceneVariantCameras() const { return sceneVariantCameras; }
		const String& GetActiveSceneVariantName() const { return activeSceneVariant; }
		void ClearSceneVariants() { sceneVariantCameras.clear(); activeSceneVariant.clear(); }

		//! Sets progress class to report progress for anything we do
		void SetProgress(
			IProgressCallback* pProgress				///< [in] The progress function
			);

		//! Registers a caller-owned, fully-built triangle mesh geometry under
		//! `name`.  See IJob.h for the full contract.
		/// \return TRUE if successful, FALSE otherwise
		bool AddPrebuiltTriangleMeshGeometry(
			const char* name,
			ITriangleMeshGeometryIndexed* pGeom
			);

		//! Parallel batch of PNG/JPEG texture-painter loads.  See IJob.h for
		//! the full contract; uses ThreadPool::ParallelFor for decode and
		//! serial manager registration in the calling thread.  Optional
		//! `outRequestSuccess` parallel array reports per-request outcome.
		/// \return TRUE if every request succeeded; FALSE if any failed
		bool AddTexturePaintersBatch(
			const TexturePainterBatchRequest* requests,
			size_t numRequests,
			bool* outRequestSuccess = nullptr
			);

		// Rasterizer registry — see IJob.h for the contract.
		bool         SetActiveRasterizer( const char* name );
		std::string  GetActiveRasterizerName() const { return activeRasterizerName; }
		unsigned int GetRasterizerTypeCount() const;
		std::string  GetRasterizerTypeName( unsigned int idx ) const;
		bool         SetRasterizerParameter( const char* rasterizerName,
			const char* paramName, const char* valueStr );
		std::string  GetRasterizerParameter( const char* rasterizerName,
			const char* paramName ) const;

		//! Direct access to the registry snapshot (used by
		//! `RasterizerIntrospection` to enumerate parameters with
		//! their current values).  Returns nullptr if no snapshot
		//! exists for `name`.
		const RasterizerParams* GetRasterizerParams( const std::string& name ) const;

		//! The 8 standard rasterizer chunk-names that the GUI accordion
		//! always lists, even when the scene file declared none of
		//! them.  Selecting one that's not yet in the registry triggers
		//! a lazy `InstantiateRasterizerWithDefaults` build.  Order is
		//! display order: PT before BDPT before VCM before MLT, with
		//! Pel before Spectral within each family.
		static const std::vector<std::string>& StandardRasterizerTypes();

		//! Adds a TEXCOORD_1 selector painter.  See IJob.h for the doc.
		//! Appended at end of class to mirror the IJob vtable position.
		/// \return TRUE if successful, FALSE otherwise
		bool AddTexCoord1Painter(
									const char* name,				///< [in] Name of the painter
									const char* source				///< [in] Source painter
									);

		//! L3.D: install a non-painter-based IRadianceMap (e.g.
		//! HosekWilkieSpectralRadianceMap) on the scene.  See IJob.h.
		bool SetGlobalRadianceMap(
									IRadianceMap* pRm				///< [in] Radiance map; Job adds a ref
									);

		//! L3.D: atomic HW skylight construction.  See IJob.h.
		bool AddHosekWilkieSkylight(
									const double solarElevationDegrees,
									const double solarAzimuthDegrees,
									const double turbidity,
									const double groundAlbedo[3],
									const double skyIntensityScale,
									const double sunIntensityScale,
									const bool   createSun
									);

	private:
		//! Lazy-build a rasterizer of the given chunk-name with
		//! sensible defaults and a shader picked from the scene's
		//! shader manager (first registered name).  Returns false if
		//! the name isn't a recognised standard type, no shader is
		//! registered (rasterizers need one), or the underlying
		//! `Set*Rasterizer` fails.  On success the new instance is
		//! both registered AND activated (the `Set*Rasterizer` path
		//! does both) and `pRasterizer` / `activeRasterizerName`
		//! point at it.
		bool InstantiateRasterizerWithDefaults( const std::string& name );

		//! Read xres/yres/pixelAR from the Scene's active Film for
		//! the camera factories (Phase B2).  Single helper used by
		//! every Add*Camera impl so the camera-factory dim-arg
		//! plumbing has one source of truth — the Scene's Film.
		void ReadFilmDims( unsigned int& xres, unsigned int& yres, double& pixelAR ) const;

		//! Incremental re-point mode (slice 3 stable-object apply).  When true,
		//! AddObject/AddObjectMatrix re-point an existing same-named object in place
		//! rather than creating a new one (see SetIncrementalRepointMode / IJob).  Set
		//! only by the single-threaded CST incremental re-Finalize loop; false in every
		//! full derive, so the full-derive object-creation path is byte-unchanged.
		bool m_bIncrementalRepoint;

		//! Count of override_object chunks that modified an object in place this derive
		//! (see IJob::NoteObjectOverride).  The CST incremental apply refuses when > 0.
		unsigned int m_objectOverrideCount;
	};
}

#endif
