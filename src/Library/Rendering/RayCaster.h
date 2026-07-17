//////////////////////////////////////////////////////////////////////
//
//  RayCaster.h - Definition of a class which an attached scene
//    capable of tracing rays through that scene. 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 16, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RAYCASTER_
#define RAYCASTER_

#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/IRadianceMap.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	class Ray;
	class IScene;
	class RayIntersection;

	namespace Implementation { class LightSampler; }

	namespace Implementation
	{
		class RayCaster : 
			public virtual IRayCaster, 
			public virtual Reference
		{
		protected:
			const IScene*				pScene;
			const IShader&				pDefaultShader;
			ILuminaryManager*			pLuminaryManager;
			LightSampler*				pLightSampler;
			ISampling2D*				pLumSampling;

			bool						bConsiderRMapAsBackground;

			const unsigned int			nMaxRecursions;

			const bool					bShowLuminaires;

			Scalar						dPendingLightRRThreshold;
			bool						bPendingUseLightBVH;

			//! Last-set RIS candidate count, retained so a same-pointer
			//! sampler rebuild (#2b(a)) re-applies it — without this, a
			//! rebuild would silently reset RIS to the LightSampler default
			//! (0).  -1 = "never set; leave the fresh sampler at its
			//! default", matching the other pending settings' "untouched"
			//! sentinels.  (No in-tree rasterizer calls SetRISCandidates
			//! today, so this is forward-safety / parity, not a live fix.)
			int							iPendingRISCandidates;

			//! The Scene light/structure generation the cached samplers
			//! (LuminaryManager / LightSampler / EnvironmentSampler) were
			//! last built against (feature/gui-snapshot-prototype #2b(a)).
			//! AttachScene compares this against the live Scene's
			//! GetLightTopologyGeneration(): on a same-Scene-pointer
			//! re-attach whose generation has ADVANCED (a restore or an
			//! in-place light edit), it rebuilds the samplers and refreshes
			//! this value, instead of taking the same-pointer fast path.
			//! 0 is the "never built" sentinel only in concert with a null
			//! pScene; once a scene is attached this tracks its generation.
			unsigned int				builtLightGeneration;

			//! Rebuilds the cached LuminaryManager / LightSampler /
			//! EnvironmentSampler from the currently-attached `pScene`.
			//! Shared by the first-attach path and the same-pointer-
			//! generation-advanced path so the two cannot drift.  Assumes
			//! `pScene` is non-null and already set; increments the
			//! process-wide sampler-rebuild diagnostic counter.
			void RebuildLightSamplers();

			//! When true, the unidirectional path tracer's NEE shadow
			//! tests route through CastShadowRayTransmittance (Fresnel-
			//! attenuated transparent shadows) instead of the binary
			//! CastShadowRay.  Default false (binary occlusion).  Set by
			//! Job::SetPathTracing{Pel,Spectral}Rasterizer from the scene's
			//! `transparent_shadows` flag.
			bool						bTransparentShadows;

			//! Runtime override for the environment radiance scale,
			//! backing `> modify rasterizer radiance_scale`.  Negative
			//! (the construction default) means "no override — use the
			//! radiance map's own scale" so existing scenes are
			//! unaffected.  When >= 0, AttachScene builds the environment
			//! importance sampler at this scale.  Job::SetActiveRasterizer-
			//! RadianceScale also pushes the same value into the scene's
			//! radiance map (IRadianceMap::SetScale) so the direct-view /
			//! ray-miss background stays consistent with NEE.  Set before
			//! a render (single-threaded); the next AttachScene applies it.
			Scalar						dRadianceScaleOverride;

			//! When true, every intersection record this caster produces
			//! requests wireframe closest-edge info from triangle-mesh
			//! intersectors (RayIntersectionGeometric::bWantsWireEdgeInfo).
			//! Default false -- production renders never pay for the
			//! feature.  Set only by the interactive wireframe view-mode
			//! caster (InteractivePelRasterizer.cpp).
			bool						bWantsWireEdgeInfo;

			//! GUI render modes (docs/gui/RENDER_MODES.md "X-ray axis"):
			//! when true, CastRay/CastRayNM resolve the primary hit THROUGH
			//! transmissive (glass-like) surfaces to the first OPAQUE hit --
			//! a straight-line continuation of the ORIGINAL ray direction,
			//! deliberately with NO refraction bending (an x-ray, not an
			//! optics simulation).  Moved into the caster layer (was
			//! previously shader-side, InteractivePelRasterizer-only) so
			//! EVERY shader benefits -- including the studio material-
			//! preview shader -- with no per-shader plumbing.  Default
			//! false; production casters never set it (cost when off is
			//! one bool test).  Set via SetXrayViewResolve.
			bool						bXrayViewResolve;

			virtual ~RayCaster();

			//! Selects the shader used for a surface hit.  The default
			//! honours the per-object shader and falls back to the
			//! caster's default shader.  Interactive preview casters can
			//! override this policy without mutating scene objects.
			virtual const IShader& SelectShader( const RayIntersection& ri ) const;

			//! GUI render modes (docs/gui/RENDER_MODES.md "X-ray axis"):
			//! the caster-layer x-ray resolver.  Bounded to 16 skips so a
			//! stack of nested transmissive shells can't loop forever.  A
			//! miss partway through the chain KEEPS the last transmissive
			//! hit rather than reporting a miss -- shading something
			//! honest beats a black hole.  When at least one skip
			//! happened, restores the ORIGINAL primary ray onto
			//! `ri.geometric.ray` and recomputes `ri.geometric.range` as
			//! the TOTAL distance from the original ray's origin to the
			//! resolved hit point -- NOT the resolved hit's own (last-
			//! segment) range -- so depth and every other range-reading
			//! consumer is correct with zero shader-side knowledge of
			//! x-ray.  Deliberately does NOT apply `ri.pModifier` --
			//! CastRay/CastRayNM's existing modifier site runs
			//! immediately after this returns and covers whatever `ri`
			//! ends up being.
			void ResolveXrayView_( RayIntersection& ri ) const;

		public:
			RayCaster(
				const bool seeRadianceMap,
				const unsigned int maxR,
				const IShader& pDefaultShader_,
				const bool showLuminaires
				);

			void AttachScene( const IScene* pScene_ );

			//! Tells the ray caster to cast the specified ray into the scene
			/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
			bool CastRay( 
				const RuntimeContext& rc,							///< [in] The runtime context
				const RasterizerState& rast,						///< [in] Current state of the rasterizer
				const Ray& ray,										///< [in] Ray to cast
				RISEPel& c,											///< [out] RISEColor for the ray
				const RAY_STATE& rs,								///< [in] The ray state
				Scalar* distance,									///< [in] If there was a hit, how far?
				const IRadianceMap* pRadianceMap					///< [in] Radiance map to use in case there is no hit
				) const;

			//! Tells the ray caster to cast the specified ray into the scene for the specific wavelength
			/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
			bool CastRayNM( 
				const RuntimeContext& rc,							///< [in] The runtime context
				const RasterizerState& rast,						///< [in] Current state of the rasterizer
				const Ray& ray,										///< [in] Ray to cast
				Scalar& c,											///< [out] Amplitude of spectral function for the given wavelength
				const RAY_STATE& rs,								///< [in] The ray state
				const Scalar nm,									///< [in] Wavelength to cast
				Scalar* distance,									///< [in] If there was a hit, how far?
				const IRadianceMap* pRadianceMap					///< [in] Radiance map to use in case there is no hit
				) const;

			//! Tells the ray caster to cast the specified ray into the scene
			/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
			bool CastRay( 
				const RuntimeContext& rc,							///< [in] The runtime context
				const RasterizerState& rast,						///< [in] Current state of the rasterizer
				const Ray& ray,										///< [in] Ray to cast
				RISEPel& c,											///< [out] RISEColor for the ray
				const RAY_STATE& rs,								///< [in] The ray state
				Scalar* distance,									///< [in] If there was a hit, how far?
				const IRadianceMap* pRadianceMap,					///< [in] Radiance map to use in case there is no hit
				const IORStack& ior_stack							///< [in/out] Index of refraction stack
				) const;

			//! Tells the ray caster to cast the specified ray into the scene for the specific wavelength
			/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
			bool CastRayNM(
				const RuntimeContext& rc,							///< [in] The runtime context
				const RasterizerState& rast,						///< [in] Current state of the rasterizer
				const Ray& ray,										///< [in] Ray to cast
				Scalar& c,											///< [out] Amplitude of spectral function for the given wavelength
				const RAY_STATE& rs,								///< [in] The ray state
				const Scalar nm,									///< [in] Wavelength to cast
				Scalar* distance,									///< [in] If there was a hit, how far?
				const IRadianceMap* pRadianceMap,					///< [in] Radiance map to use in case there is no hit
				const IORStack& ior_stack							///< [in/out] Index of refraction stack
				) const;

			//! Casts a ray for a bundle of HWSS wavelengths with shared
			//! scene intersection.  The hero wavelength drives medium
			//! transport; the shader op evaluates all wavelengths at
			//! the shared geometric intersection.
			/// \return TRUE if any wavelength produced a hit
			bool CastRayHWSS(
				const RuntimeContext& rc,							///< [in] The runtime context
				const RasterizerState& rast,						///< [in] Current state of the rasterizer
				const Ray& ray,										///< [in] Ray to cast
				Scalar c[SampledWavelengths::N],					///< [out] Per-wavelength amplitudes
				const RAY_STATE& rs,								///< [in] The ray state
				SampledWavelengths& swl,							///< [in/out] Wavelength bundle
				Scalar* distance,									///< [in] If there was a hit, how far?
				const IRadianceMap* pRadianceMap,					///< [in] Radiance map for misses
				const IORStack& ior_stack							///< [in/out] Index of refraction stack
				) const;

			//! This function casts a ray into the scene and only checks to see if it intersects something.
			//! Very useful for shadow checks
			/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
			bool CastShadowRay(
				const Ray& ray,										///< [in] Ray to cast
				const Scalar dHowFar								///< [in] How far to follow the ray, optimization
				) const;

			//! TRANSPARENT (Fresnel-attenuated) shadow ray.  Walks the
			//! shadow segment hit-by-hit (closest-hit IntersectRay); at
			//! each interface that is a PERFECT-SPECULAR TRANSMISSIVE
			//! DIELECTRIC (queried via IMaterial::GetSpecularInfo{,NM}:
			//! valid && isSpecular && canRefract && clearTransmission) the ray
			//! passes STRAIGHT
			//! through (no refractive bend) and the running transmittance
			//! is multiplied by the per-interface Fresnel transmittance
			//! T = 1 - F(cosTheta, eta).  Any OTHER hit (opaque, diffuse,
			//! rough, pure mirror) fully blocks (returns true, transmittance
			//! left at 0).
			//!
			//! APPROXIMATION (documented at the implementation): straight-
			//! through propagation ignores refractive ray bending and
			//! internal multi-bounce; the per-interface eta is a single
			//! representative value (see the .cpp for the pel-vs-NM eta
			//! source).  Used ONLY by the unidirectional PT integrator when
			//! `transparent_shadows` is enabled; BDPT / VCM / MLT keep the
			//! binary CastShadowRay.
			//!
			//! Caps at 32 interface crossings, then conservatively returns
			//! blocked (true).
			//!
			//! \return TRUE if the light is FULLY occluded (an opaque hit
			//!         or the crossing cap was reached); FALSE if the ray
			//!         reached dHowFar, with @a transmittance carrying the
			//!         accumulated Fresnel transmittance (1.0 when the
			//!         segment was clear of any geometry).
			bool CastShadowRayTransmittance(
				const Ray& ray,										///< [in] Ray to cast (origin = shading point, dir = toward light, normalized)
				const Scalar dHowFar,								///< [in] How far to follow the ray (distance to the light minus epsilon)
				const bool bNM,										///< [in] True for the spectral (single-wavelength) path; false for the RGB path
				const Scalar nm,									///< [in] Wavelength (only used when bNM == true)
				RISEPel& transmittance								///< [out] Accumulated per-interface Fresnel transmittance product (RGB; all 3 channels equal on the NM path)
				) const;

			//! Flag-aware NEE shadow occlusion — the SINGLE entry point that
			//! routes to the Fresnel-transmittance walk when
			//! `transparent_shadows` is enabled, else the binary CastShadowRay.
			//! Every NEE shadow site uses this so the flag is honored
			//! UNIFORMLY.  Two families of caller:
			//!   * LightSampler's delta / mesh-luminary NEE branches (the PT
			//!     path for omni / spot / area lights), via ShadowOccluded.
			//!   * Every concrete light's ILight::ComputeDirectLighting
			//!     (Directional, Point, Spot) — the path the Step-1 zero-
			//!     exitance lights and any direct
			//!     ILightManager::ComputeDirectLighting caller take.  (Ambient
			//!     casts no shadow ray.)
			//! Geometry-agnostic — it forwards to the same CastShadowRay /
			//! CastShadowRayTransmittance the analytic primitives and SDFs
			//! both flow through.
			//!
			//! \return TRUE if the light is FULLY occluded; FALSE if it is
			//!         reachable, with @a transmittance carrying the
			//!         accumulated per-interface Fresnel transmittance
			//!         (1,1,1 when the segment was clear, or when the flag is
			//!         off / binary).
			bool CastShadowRayAuto(
				const Ray& ray,										///< [in] Ray to cast (origin = shading point, dir = toward light, normalized)
				const Scalar dHowFar,								///< [in] How far to follow the ray
				const bool bNM,										///< [in] True for the spectral (single-wavelength) path; false for the RGB path
				const Scalar nm,									///< [in] Wavelength (only used when bNM == true)
				RISEPel& transmittance								///< [out] Accumulated per-interface Fresnel transmittance (1,1,1 when clear or binary)
				) const;

			//! To retreive the current scene
			/// \return Pointer to currently attached scene, NULL if no scene is currently attached
			const IScene* GetAttachedScene() const { return pScene; };

			//! Sets the luminaire sampler
			void SetLuminaireSampling(
				ISampling2D* pLumSam								///< [in] Kernel to use for luminaire sampling
				);

			/// \return The luminary manager for the current scene
			const ILuminaryManager* GetLuminaries() const { return pLuminaryManager; };

			/// \return The unified light sampler for the current scene
			const LightSampler* GetLightSampler() const { return pLightSampler; };

			/// Diagnostic: process-wide count of LightSampler rebuilds
			/// performed inside AttachScene (the first build on a fresh
			/// scene-pointer attach PLUS any same-pointer rebuild driven by
			/// an advanced Scene light-generation).  Proves the #2b(a)
			/// generation gate: a no-change re-attach leaves it unchanged
			/// (fast path); a restore / in-place light edit followed by a
			/// re-attach bumps it by exactly one.  Same static-counter
			/// pattern as Scene::GetPhotonShootCount.  Single-threaded —
			/// AttachScene runs at the pre-parallel scene-setup seam.
			static unsigned int GetSamplerRebuildCount();
			static void         ResetSamplerRebuildCount();

			/// Sets the number of RIS candidates for spatially-aware
			/// light selection.  Must be called after AttachScene().
			void SetRISCandidates( const unsigned int M );

			/// Sets the threshold for light-sample Russian roulette.
			/// Must be called after AttachScene().
			void SetLightSampleRRThreshold( const Scalar threshold );

			/// Enables or disables the light BVH.
			void SetUseLightBVH( const bool enable );

			/// Enables or disables transparent (Fresnel-attenuated) shadow
			/// rays for NEE.  When enabled, the unidirectional path
			/// tracer's shadow tests route through
			/// CastShadowRayTransmittance.  Default disabled (binary).
			void SetTransparentShadows( const bool enable ) { bTransparentShadows = enable; }

			/// \return Whether transparent shadow rays are enabled.  Read
			/// by LightSampler's NEE evaluators (via a dynamic_cast to the
			/// concrete RayCaster) to decide between the binary and
			/// Fresnel-attenuated shadow test.
			bool GetTransparentShadows() const { return bTransparentShadows; }

			/// Overrides the environment radiance scale (backs `> modify
			/// rasterizer radiance_scale`).  A negative value clears the
			/// override (revert to the radiance map's own scale).  Takes
			/// effect on the NEXT AttachScene, which rebuilds the
			/// environment importance sampler.  Set before a render.
			void SetRadianceScale( const Scalar scale ) { dRadianceScaleOverride = scale; }

			/// \return The current radiance-scale override, or a negative
			/// value when no override is set.
			Scalar GetRadianceScale() const { return dRadianceScaleOverride; }

			//! GUI render modes (docs/gui/RENDER_MODES.md "X-ray axis"):
			//! enables/disables the caster-layer x-ray resolver (see
			//! `ResolveXrayView_`).  Off by default; production casters
			//! never call this.
			void SetXrayViewResolve( const bool enable ) { bXrayViewResolve = enable; }

			/// \return Whether the caster-layer x-ray resolver is enabled.
			bool GetXrayViewResolve() const { return bXrayViewResolve; }

			/// See IRayCaster::IsRadianceMapVisibleAsBackground.
			/// (No `override` — RayCaster matches the file's existing
			///  style; see CLAUDE.md's note on -Winconsistent-missing-override.)
			bool IsRadianceMapVisibleAsBackground() const { return bConsiderRMapAsBackground; }

		};
	}
}

#include "../Interfaces/IScene.h"

#endif
