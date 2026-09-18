//////////////////////////////////////////////////////////////////////
//
//  IRayCaster.h - Interface to a ray caster
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 28, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IRAYCASTER_
#define IRAYCASTER_

#include "IReference.h"
#include "ISampling2D.h"
#include "IRadianceMap.h"
#include "../Utilities/Ray.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/SampledWavelengths.h"

namespace RISE
{
	class ILuminaryManager;
	class IScene;
	class IORStack;
	struct RuntimeContext;

	namespace Implementation { class LightSampler; }

	//! A ray caster traces a ray generated on the virtual screen and traces it through the 
	//! scene
	/// \sa IPixelTracer
	class IRayCaster : public virtual IReference
	{
	protected:
		IRayCaster(){};
		virtual ~IRayCaster(){};

	public:

		// RAY_STATE describes the state of a particular ray we are casting
		struct RAY_STATE
		{
			enum RayType
			{
				eRayView			= 0,		///< Viewer ray
				eRayDiffuse			= 1,		///< Diffuse ray
				eRaySpecular		= 2,		///< Some kind of specular ray
				eRayFinalGather		= 3			///< A ray used for the final gather process
			};

			unsigned int depth;					///< Number of times the ray has bounced
			Scalar importance;					///< Importance of this ray
			bool considerEmission;				///< Should shader consider direct emission
			RayType type;						///< The type of ray
			//! The TRUE density the continuation direction was drawn from
			//! (0 = not set / delta).  This is the throughput denominator:
			//! under path guiding it is the guided mixture / RIS pdf, not
			//! the raw material pdf.  Pairs with `bsdfTimesCos` to form the
			//! optimal-MIS second-moment estimator `(f/p)^2`, so it MUST
			//! stay the sampling density -- see DL-72.
			Scalar bsdfPdf;
			//! The NOMINAL density used as the BSDF-sampling technique's
			//! MIS partner against light sampling.  It and `LightSampler`'s
			//! NEE arms evaluate ONE lobe-independent function of
			//! direction, so the two weights partition to 1.
			//!
			//! It is NOT in general equal to `bsdfPdf` (an earlier version
			//! of this comment said it was "everywhere but under path
			//! guiding", which DL-103 made false).  For a surface
			//! continuation the partner is the MATERIAL'S AGGREGATE
			//! `ISPF::Pdf()` for the traced direction -- guided or not --
			//! while `bsdfPdf` is the SELECTED lobe's own density; those
			//! differ at every multi-lobe SPF.  Under guiding the partner
			//! is additionally blended with the guide density.  The volume
			//! continuations are the other standing case: `bsdfPdf`
			//! carries the guided effective pdf while the partner is the
			//! raw `phasePdf` its NEE arm uses (DL-73).
			//! DL-74 / docs/DL74_ENV_NEE_GUIDING_PARTITION.md,
			//! DL-103 / docs/DL103_PT_ESCAPE_MIS_PARTNER.md.
			//!
			//! Three-valued, and read through `MisPartnerPdf()` rather
			//! than directly:
			//!   < 0  not set -- fall back to `bsdfPdf`.  This is the
			//!        DEFAULT, so a producer that predates this field (or
			//!        simply has no guiding to describe) keeps its exact
			//!        pre-DL-74 weight instead of silently losing it.
			//!   0    no MIS partner exists.  Since DL-103 this means a
			//!        DELTA lobe (or a producer that has no partner to
			//!        describe at all, e.g. a vertex where NEE never
			//!        fires).  A non-delta surface continuation whose
			//!        AGGREGATE pdf reads zero in the traced direction
			//!        does NOT arrive here as 0: giving both sides "no
			//!        partner" would put weight 1 on each and double
			//!        count, so the producer substitutes the selected
			//!        lobe's own density instead (DL-41's SPFs, whose
			//!        `Pdf()` does not cover their own lobes).  With
			//!        guiding ACTIVE the producer folds in
			//!        `alpha_nom * guide` for the same reason (review
			//!        round 4 of DL-74, rows (h)/(i)).  The weight is 1
			//!        and the sample is taken whole.
			//!   > 0  the nominal partner density.
			Scalar bsdfMisPdf;
			RISEPel bsdfTimesCos;				///< BSDF * cos at scatter point (RGB), for optimal MIS full-integrand training

			// Per-type bounce counters for StabilityConfig bounce limits
			unsigned int diffuseBounces;		///< Accumulated diffuse bounces
			unsigned int glossyBounces;			///< Accumulated glossy/reflection bounces
			unsigned int transmissionBounces;	///< Accumulated refraction/transmission bounces
			unsigned int translucentBounces;	///< Accumulated translucent bounces

			Scalar glossyFilterWidth;			///< Accumulated glossy filter roughness increase (0 = no filtering)

			unsigned int volumeBounces;			///< Accumulated volume scattering bounces

			// SMS emission suppression state, propagated into recursive
			// CastRay calls (e.g. via SSS / BSSRDF entry, shader-op
			// chains) so emission through specular chains is correctly
			// suppressed across the recursion boundary.  See docs/SMS.md.
			bool smsPassedThroughSpecular;	///< True if path traversed a delta surface since last non-specular bounce
			bool smsHadNonSpecularShading;	///< True if path had at least one non-specular shading point (where SMS evaluated)

			//! DL-185: `RayCaster::CastRay{,NM,HWSS}` applies its own
			//! CAST-LEVEL importance Russian roulette (`RC_RR_THRESHOLD`)
			//! to the COMBINED radiance it returns for THIS call, at the
			//! very end of the call frame -- see `rrCompensation` in
			//! RayCaster.cpp.  That factor is known at the top of the call
			//! (before intersection), but the NEE done while shading this
			//! same hit (`LightSampler::EvaluateDirectLighting{,NM}`,
			//! reached through `SelectShader(ri).Shade{,NM,HWSS}`) lives in
			//! a different call frame and has no visibility into it, so its
			//! own trained optimal-MIS moment used to disagree with the
			//! BSDF-escape arm's (DL-148) by exactly this factor.  CastRay
			//! stamps the LOCAL `rrCompensation` it computed for THIS call
			//! onto a copy of `rs` handed to `Shade{,NM,HWSS}`, purely for
			//! that NEE call's `neeTrainingScale` argument -- it is NEVER
			//! read for anything that changes returned radiance.  Default 1
			//! (no compensation) for every producer that predates this
			//! field or is not itself wrapped by that roulette.
			Scalar castRRCompensation;

			//! DL-171/DL-209 (legacy shader-op chain, chain-aware MIS
			//! partner): does THIS SHADER's own op list contain a
			//! `DirectLightingShaderOp` (NEE)?  Set once per `Shade{,NM}`
			//! call by the owning `StandardShader`/`AdvancedShader`, from
			//! its own resolved op list (`AdvancedShader` restricts to the
			//! ops whose `[nMinDepth,nMaxDepth]` covers `depth`, since its
			//! op list is depth-gated; `StandardShader` has no such
			//! ranges, so it resolves once at construction).  Read by
			//! `DistributionTracingShaderOp`'s own continuation: TRUE
			//! stamps the real aggregate-density MIS partner (a
			//! `DirectLightingShaderOp` sibling genuinely competes for the
			//! same light); FALSE stamps 0 (no NEE sibling in this
			//! shader, so `EmissionShaderOp` must take the traced hit at
			//! FULL, unweighted credit -- DL-209's shape).  Default TRUE:
			//! every producer outside the legacy chain (PT, BDPT, the
			//! entry-point construction below) has always paired
			//! BSDF-sampling with a real NEE strategy, so this is a no-op
			//! everywhere else.
			bool chainHasNEEOp;
			//! The converse: does THIS shader's own op list contain a
			//! `DistributionTracingShaderOp` (a non-delta, emission-
			//! considering BSDF-sampled continuation)?  Read by
			//! `DirectLightingShaderOp`, forwarded to `LightSampler::
			//! EvaluateDirectLighting{,NM}`'s `bBsdfSamplingPartnerExists`
			//! parameter: TRUE weights NEE's own sample against the
			//! material's aggregate density as before (a competing
			//! BSDF-sampled strategy exists to partition against); FALSE
			//! forces weight 1 (no such strategy exists in this shader to
			//! discount against -- `DirectLightingShaderOp` alone, or with
			//! only delta `Reflection`/`Refraction` ops (already weight-1
			//! on their own side, so nothing to partition with either),
			//! or with `FinalGatherShaderOp` (never considers emission at
			//! all)).  Default TRUE for the same reason as `chainHasNEEOp`.
			bool chainHasBsdfContinuationOp;

			RAY_STATE() : depth( 1 ), importance( 1.0 ), considerEmission( true ), type( eRayView ), bsdfPdf( 0 ),
				bsdfMisPdf( -1 ),
				diffuseBounces( 0 ), glossyBounces( 0 ), transmissionBounces( 0 ), translucentBounces( 0 ),
				glossyFilterWidth( 0 ), volumeBounces( 0 ),
				smsPassedThroughSpecular( false ), smsHadNonSpecularShading( false ),
				castRRCompensation( 1.0 ),
				chainHasNEEOp( true ), chainHasBsdfContinuationOp( true ) {}

			//! The MIS-partner density to weight with -- see `bsdfMisPdf`.
			Scalar MisPartnerPdf() const
			{
				return bsdfMisPdf < 0 ? bsdfPdf : bsdfMisPdf;
			}
		};

		//! Tells the ray caster to cast the specified ray into the scene
		/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
		virtual bool CastRay( 
			const RuntimeContext& rc,							///< [in] The runtime context
			const RasterizerState& rast,						///< [in] Current state of the rasterizer
			const Ray& ray,										///< [in] Ray to cast
			RISEPel& c,											///< [out] RISEColor for the ray
			const RAY_STATE& rs,								///< [in] The ray state
			Scalar* distance,									///< [in] If there was a hit, how far?
			const IRadianceMap* pRadianceMap					///< [in] Radiance map to use in case there is no hit
			) const = 0;

		//! Tells the ray caster to cast the specified ray into the scene for the specific wavelength
		/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
		virtual bool CastRayNM( 
			const RuntimeContext& rc,							///< [in] The runtime context
			const RasterizerState& rast,						///< [in] Current state of the rasterizer
			const Ray& ray,										///< [in] Ray to cast
			Scalar& c,											///< [out] Amplitude of spectral function for the given wavelength
			const RAY_STATE& rs,								///< [in] The ray state
			const Scalar nm,									///< [in] Wavelength to cast
			Scalar* distance,									///< [in] If there was a hit, how far?
			const IRadianceMap* pRadianceMap					///< [in] Radiance map to use in case there is no hit
			) const = 0;

		//! Tells the ray caster to cast the specified ray into the scene
		/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
		virtual bool CastRay(
			const RuntimeContext& rc,							///< [in] The runtime context
			const RasterizerState& rast,						///< [in] Current state of the rasterizer
			const Ray& ray,										///< [in] Ray to cast
			RISEPel& c,											///< [out] RISEColor for the ray
			const RAY_STATE& rs,								///< [in] The ray state
			Scalar* distance,									///< [in] If there was a hit, how far?
			const IRadianceMap* pRadianceMap,					///< [in] Radiance map to use in case there is no hit
			const IORStack& ior_stack							///< [in/out] Index of refraction stack
			) const = 0;

		//! Tells the ray caster to cast the specified ray into the scene for the specific wavelength
		/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
		virtual bool CastRayNM(
			const RuntimeContext& rc,							///< [in] The runtime context
			const RasterizerState& rast,						///< [in] Current state of the rasterizer
			const Ray& ray,										///< [in] Ray to cast
			Scalar& c,											///< [out] Amplitude of spectral function for the given wavelength
			const RAY_STATE& rs,								///< [in] The ray state
			const Scalar nm,									///< [in] Wavelength to cast
			Scalar* distance,									///< [in] If there was a hit, how far?
			const IRadianceMap* pRadianceMap,					///< [in] Radiance map to use in case there is no hit
			const IORStack& ior_stack							///< [in/out] Index of refraction stack
			) const = 0;

		//! Casts a ray for a bundle of HWSS wavelengths.
		//! Default implementation calls CastRayNM independently for each
		//! active wavelength.  RayCaster overrides for shared intersection.
		/// \return TRUE if any wavelength produced a hit
		virtual bool CastRayHWSS(
			const RuntimeContext& rc,							///< [in] The runtime context
			const RasterizerState& rast,						///< [in] Current state of the rasterizer
			const Ray& ray,										///< [in] Ray to cast
			Scalar c[SampledWavelengths::N],					///< [out] Per-wavelength amplitudes
			const RAY_STATE& rs,								///< [in] The ray state
			SampledWavelengths& swl,							///< [in/out] Wavelength bundle
			Scalar* distance,									///< [in] If there was a hit, how far?
			const IRadianceMap* pRadianceMap,					///< [in] Radiance map for misses
			const IORStack& ior_stack							///< [in/out] Index of refraction stack
			) const
		{
			bool anyHit = false;
			for( unsigned int i = 0; i < SampledWavelengths::N; i++ )
			{
				c[i] = 0;
				if( !swl.terminated[i] )
				{
					bool hit = CastRayNM( rc, rast, ray, c[i], rs,
						swl.lambda[i], distance, pRadianceMap, ior_stack );
					if( hit ) anyHit = true;
				}
			}
			return anyHit;
		}

		//! LIGHT-VISIBILITY query: is this ray blocked from reaching a light?
		//! Delegates to IObjectManager::IntersectShadowRay, which honours
		//! `casts_shadows` -- an object authored `casts_shadows FALSE` is
		//! invisible to this query BY DESIGN.  Very useful for shadow /
		//! NEE checks; NOT for a geometry-presence occlusion estimator
		//! (ambient occlusion and similar) -- use CastOcclusionRay for
		//! that (added 2026-09-07 after AmbientOcclusionShaderOp was found
		//! reusing this one; see docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md
		//! section 8.1).
		/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
		virtual bool CastShadowRay(
			const Ray& ray,										///< [in] Ray to cast
			const Scalar dHowFar								///< [in] How far to follow the ray, optimization
			) const = 0;

		//! This function attaches this ray caster to the given scene
		virtual void AttachScene( 
			const IScene* pScene_								///< [in] Scene to attach
			) = 0;

		//! To retreive the current scene
		/// \return Pointer to currently attached scene, NULL if no scene is currently attached
		virtual const IScene* GetAttachedScene() const = 0;

		//! Sets the luminaire sampler
		virtual void SetLuminaireSampling(
			ISampling2D* pLumSam								///< [in] Kernel to use for luminaire sampling
			) = 0;

		/// \return The luminary manager for the current scene
		virtual const ILuminaryManager* GetLuminaries() const = 0;

		/// \return The unified light sampler for the current scene, or NULL if not available
		virtual const Implementation::LightSampler* GetLightSampler() const = 0;

		/// Sets the number of RIS candidates for spatially-aware light
		/// selection.  Must be called after AttachScene().
		virtual void SetRISCandidates(
			const unsigned int M								///< [in] Number of RIS candidates (0=disabled)
			) = 0;

		/// Sets the threshold for light-sample Russian roulette.
		/// Must be called after AttachScene().
		virtual void SetLightSampleRRThreshold(
			const Scalar threshold								///< [in] RR threshold (0=disabled)
			) = 0;

		/// Enables or disables the light BVH for importance-weighted
		/// many-light selection.  Must be called before AttachScene().
		virtual void SetUseLightBVH(
			const bool enable									///< [in] True to enable light BVH
			) = 0;

		/// True when the rasterizer was built with `radiance_background
		/// true` (the default) and missed primary rays should display
		/// the global radiance map.  Integrators that compute their own
		/// primary-ray miss radiance (PT, BDPT) consult this so an
		/// `isBackground = false` configuration leaves camera rays
		/// black while indirect bounces still pick up the IBL.
		virtual bool IsRadianceMapVisibleAsBackground() const = 0;

		//! GEOMETRY-PRESENCE query: is there ANY geometry in the way,
		//! independent of whether it casts shadows?  Delegates to
		//! IObjectManager::IntersectOcclusionRay, which does NOT honour
		//! `casts_shadows` -- an object authored `casts_shadows FALSE`
		//! still occludes here, because it has not stopped existing, only
		//! stopped blocking light for NEE purposes.
		//!
		//! Sibling of CastShadowRay above, which answers the OPPOSITE
		//! question (light visibility, `casts_shadows`-gated).  Use this
		//! one for ambient-occlusion-style estimators (AmbientOcclusion-
		//! ShaderOp, InteractivePelRasterizer's preview AO) and anything
		//! else asking "is this point cavity-like", never for NEE / light
		//! sampling.  Added 2026-09-07; see
		//! docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md section 8.1.
		//!
		//! Appended at the interface TAIL, matching CastShadowRay's own
		//! evolution on this interface (SetRISCandidates, SetLightSample-
		//! RRThreshold, SetUseLightBVH, IsRadianceMapVisibleAsBackground
		//! above were all added the same way) -- RayCaster is this
		//! interface's sole in-tree implementer, always obtained through
		//! RISE_API_CreateRayCaster, never constructed by an out-of-tree
		//! caller, so a new pure virtual here is source- but not silently
		//! binary-vtable-breaking (contrast IScene/IMaterial, which ARE
		//! reached through caller-supplied instances and therefore do NOT
		//! get new virtuals -- see Scene.h's GetLightTopologyGeneration
		//! comment).  A dynamic_cast-to-concrete-RayCaster alternative
		//! (the pattern used for e.g. CastShadowRayTransmittance) was
		//! rejected here because AmbientOcclusionShaderOp only ever holds
		//! an `const IRayCaster&` and calls this once per AO sample --
		//! adding a dynamic_cast to that hot loop is both slower and
		//! uglier than the one appended virtual CastShadowRay already
		//! established the precedent for.
		/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
		virtual bool CastOcclusionRay(
			const Ray& ray,										///< [in] Ray to cast
			const Scalar dHowFar								///< [in] How far to follow the ray, optimization
			) const = 0;
	};
}

#include "ILuminaryManager.h"
#include "IScene.h"
#include "../Utilities/IORStack.h"
#include "../Utilities/RuntimeContext.h"

#endif
