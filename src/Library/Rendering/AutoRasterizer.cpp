//////////////////////////////////////////////////////////////////////
//
//  AutoRasterizer.cpp - Implements the auto-routing integrator
//    dispatcher.  See AutoRasterizer.h for the architecture rationale.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 4, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "AutoRasterizer.h"
#include "FrameStore.h"
#include "VCMRasterizerBase.h"
#include "../RISE_API.h"
#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/IPixelFilter.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/IScenePriv.h"
#include "../Interfaces/IFilm.h"
#include "../Interfaces/IObjectManager.h"
#include "../Managers/ObjectManager.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IMedium.h"
#include "../Interfaces/ILightManager.h"
#include "../Interfaces/ILightPriv.h"
#include "../Interfaces/IEnumCallback.h"
#include "../Interfaces/ISampling2D.h"
#include "../Interfaces/IRasterizerOutput.h"
#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IOptions.h"
#include "../Utilities/SMSConfig.h"
#include "../Utilities/Reference.h"
#include "../Utilities/FiniteMath.h"
#include "../Utilities/Transformable.h"
#include "../Cameras/CameraCommon.h"
#include "../Utilities/Color/Color_Template.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/SobolSampler.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <vector>
#include <memory>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	thread_local std::vector<const AutoRasterizer*> gResolvingAutoRasterizers;
	std::mutex gAutoResolutionMutex;
	std::condition_variable gAutoResolutionChanged;
	std::map<const AutoRasterizer*,const AutoRasterizer*> gAutoResolutionWaits;
	std::set<const AutoRasterizer*> gAutoResolutionsActive;
	std::atomic<unsigned int> gProbeCapturesLive(0u);

	std::unique_lock<std::recursive_mutex> AcquireAutoMutationLock(
		std::recursive_mutex& mutex )
	{
		std::unique_lock<std::recursive_mutex> lock(mutex,std::try_to_lock);
		if( !lock.owns_lock() ) {
			throw std::runtime_error(
				"output_provenance_unavailable: Auto delegate mutation is reentrant or concurrent");
		}
		return lock;
	}

	//! Lowercase scene-language spelling of an integrator choice; used
	//! only for the diagnostic log line that surfaces the runtime pick.
	const char* IntegratorName( AutoIntegratorChoice c )
	{
		switch( c ) {
			case AutoIntegratorChoice::PT:   return "pt";
			case AutoIntegratorChoice::BDPT: return "bdpt";
			case AutoIntegratorChoice::VCM:  return "vcm";
			case AutoIntegratorChoice::Auto:
			default:                         return "auto";
		}
	}

	//! Tier-1 signal: does any visible object carry a transmissive /
	//! dielectric material (`IMaterial::CouldLightPassThrough()`)?  That
	//! is the surface kind — glass, water, gems — that bends light into
	//! refractive caustics. ObjectManager enumerates all visible objects;
	//! its current implementation ignores the callback return value.
	//!
	//! `CouldLightPassThrough()` ALONE.  A `weave_material` under
	//! `transmission thin` also reports `CouldLightPassThrough() == true`
	//! (P2-B) — a flat, zero-thickness `ScattersFullSphere()` continuum
	//! material — and this signal used to carry a
	//! `&& !mat->ScattersFullSphere()` exclusion (docs/CLOTH_FABRIC_DESIGN.md
	//! §15 debt 20, REVIEW_P2R7.md P2) after a `100-350x over PT` BDPT/VCM
	//! measurement on the shipped backlit-curtain scene
	//! (tests/FabricRenderTest.cpp).
	//!
	//! RESOLVED 2026-09-04 (chip 4 / task_93ff4a8a): that measurement did
	//! not reproduce a BDPT/VCM-side defect.  Re-run against the identical
	//! scene on top of commit `d01a320a` (the SAME-DAY, otherwise-unrelated
	//! debt-21 fix — a scale-relative self-hit floor in
	//! `RayBilinearPatchIntersection`, the shared ray-vs-quad routine used
	//! by every ray cast against this curtain: primary, continuation AND
	//! shadow) gives BDPT/PT = 0.899-0.900 and VCM/PT = 0.932-0.933,
	//! stable across five independent seed bases at 256 and 1024 spp, on
	//! BOTH the mean and the max-pixel luminance (no residual firefly
	//! signature); a harsher "touching-distance" mesh-area-light stress
	//! scene (light plane 0.01 units behind the curtain,
	//! `max_light_depth 12`, deliberately shaped to maximise same-object
	//! near-coincident vertex connections) gives BDPT/PT = VCM/PT = 1.01,
	//! 0 fireflies by a 4x-neighbourhood-median criterion.  The root
	//! cause was the debt-21 bug itself, from the OTHER side: PT's own
	//! NEE shadow ray toward a light behind the curtain had no epsilon
	//! bump of its own and relied entirely on the geometry routine's
	//! self-hit rejection, which pre-fix accepted ANY positive `dRange`
	//! (literally `dRange > 0`, not even an absolute `NEARZERO` compare)
	//! — spuriously self-occluding the overwhelming majority of those
	//! shadow rays and deflating PT's OWN reference value by ~370x on
	//! this exact scene.  BDPT's and VCM's connection-visibility shadow
	//! rays were never meaningfully exposed to that bug: both apply their
	//! own `BDPT_RAY_EPSILON` / `VCM_RAY_EPSILON` (`= 1e-6`, six orders of
	//! magnitude above the ~1e-12 FP-noise floor debt 21 measured) via
	//! `Ray::Advance()` before casting (BDPTIntegrator.cpp / VCMIntegrator.cpp),
	//! so their absolute output barely moved across the debt-21 fix
	//! (BDPT read ~0.0253 before AND after) — the "100-350x" figure was
	//! comparing a stable BDPT/VCM number against a PT reference that was
	//! itself broken, the "PT may be the broken one" trap
	//! docs/skills/bdpt-vcm-mis-balance.md's step 0 warns about.  Full
	//! writeup: CLOTH_FABRIC_DESIGN.md §15 debt 20.  The exclusion below
	//! is therefore LIFTED — this material class routes through the same
	//! `hasTransmissive && hasPositional` test as any other dielectric —
	//! see `tests/AutoRasterizerTest.cpp`'s "thin weave + point light" case
	//! for the (now VCM-routing) regression.
	bool SceneHasTransmissiveMaterial( const IScene& scene )
	{
		struct TransmissiveScan : public IEnumCallback<IObject>
		{
			bool found;
			TransmissiveScan() : found( false ) {}
			bool operator()( const IObject& obj )
			{
				const IMaterial* mat = obj.GetMaterial();
				if( mat && mat->CouldLightPassThrough() ) {
					found = true;
					return false;   // one is enough — stop enumeration
				}
				return true;        // keep scanning
			}
		};

		const IObjectManager* objs = scene.GetObjects();
		if( !objs ) {
			return false;
		}
		TransmissiveScan scan;
		objs->EnumerateObjects( scan );
		return scan.found;
	}

    // DL-475: a conservative census of the proven C-S-M-S-L support gap.
    // Do not infer topology or IOR from a dummy shading record. Mixed and
    // absorption-only boundaries may over-route; a missing bound medium
    // at a CSG root remains outside this census.
    bool SceneHasTransmissiveInteriorMedium(const IScene& scene)
    {
        struct Scan final : IEnumCallback<IObject> {
            bool found=false;
            bool operator()(const IObject& obj) override {
                const IMaterial* mat=obj.GetMaterial();
                if(obj.IsWorldVisible() && obj.GetInteriorMedium() && mat && mat->CouldLightPassThrough()) {
                    found=true; return false;
                }
                return true;
            }
        } scan;
        if(const IObjectManager* objects=scene.GetObjects()) objects->EnumerateObjects(scan);
        return scan.found;
    }

	//! Tier-1 signal: does the scene have at least one positional
	//! (point / spot / omni) delta light?  These are the small / point
	//! sources able to concentrate refracted energy into a sharp caustic;
	//! `IsPositionalLight()` returns false for directional and ambient
	//! lights (and there is no entry for area / mesh emitters or env-IBL
	//! in the hack-light list at all — those are objects-with-emitter and
	//! the global radiance map respectively, deliberately NOT counted as
	//! caustic-prone here, which is what keeps area-lit dielectric scenes
	//! on PT/BDPT instead of over-routing them to VCM).
	bool SceneHasPositionalLight( const IScene& scene )
	{
		const ILightManager* lights = scene.GetLights();
		if( !lights ) {
			return false;
		}
		const ILightManager::LightsList& list = lights->getLights();
		for( ILightManager::LightsList::const_iterator it = list.begin(); it != list.end(); ++it ) {
			const ILightPriv* l = *it;
			if( l && l->IsPositionalLight() ) {
				return true;
			}
		}
		return false;
	}

	//! G10 Tier-1 guard: PT is the only fire/smoke target, and the other
	//! candidate integrators do not share its heterogeneous/emissive-volume
	//! feature set.  A medium therefore requires PT when it is spatially
	//! heterogeneous, or when its homogeneous coefficients carry emission.
	//! Scan both object interiors and the global medium; stop at the first
	//! match.  Homogeneous media ignore the query point by contract.
	bool SceneHasPTOnlyMedium( const IScene& scene )
	{
		auto requiresPT = []( const IMedium* medium ) -> bool {
			if( !medium ) {
				return false;
			}
			if( medium->IsFireMedium() || !medium->IsHomogeneous() ) {
				return true;
			}
			const MediumCoefficients coeff = medium->GetCoefficients( Point3(0,0,0) );
			return coeff.emission[0] != 0.0 || coeff.emission[1] != 0.0 ||
				coeff.emission[2] != 0.0 || medium->GetThermalEmissionImportance() > 0.0;
		};

		if( requiresPT( scene.GetGlobalMedium() ) ) {
			return true;
		}

		struct MediumScan : public IEnumCallback<IObject>
		{
			const decltype(requiresPT)& predicate;
			bool found;
			explicit MediumScan( const decltype(requiresPT)& p ) : predicate(p), found(false) {}
			bool operator()( const IObject& obj )
			{
				if( predicate( obj.GetInteriorMedium() ) ) {
					found = true;
					return false;
				}
				return true;
			}
		};

		const IObjectManager* objects = scene.GetObjects();
		if( !objects ) {
			return false;
		}
		MediumScan scan( requiresPT );
		objects->EnumerateObjects( scan );
		return scan.found;
	}

	//! Capturing output for the render-time probe.  Stores the final
	//! image's per-pixel luminance (mean of the linear RGB channels —
	//! matching the Phase-3 experiment's EXR-RGB-mean signal) so the
	//! probe can read back median-luminance and per-pixel variance
	//! WITHOUT an EXR round-trip.  Same pattern as the Capturing
	//! RasterizerOutput in tests/AutoRasterizerTest.cpp; only the two
	//! pure-virtual IRasterizerOutput methods need overriding.
	class ProbeCaptureOutput
		: public virtual IRasterizerOutput
		, public virtual Reference
	{
	public:
		std::vector<double> lum;	///< per-pixel luminance, row-major
		unsigned int width;
		unsigned int height;

		ProbeCaptureOutput() : width( 0 ), height( 0 )
		{
			gProbeCapturesLive.fetch_add(1u,std::memory_order_relaxed);
		}

	protected:
		virtual ~ProbeCaptureOutput()
		{
			gProbeCapturesLive.fetch_sub(1u,std::memory_order_relaxed);
		}

	public:
		virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

		virtual void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
		{
			width  = img.GetWidth();
			height = img.GetHeight();
			lum.resize( static_cast<size_t>( width ) * height );
			for( unsigned int y = 0; y < height; ++y ) {
				for( unsigned int x = 0; x < width; ++x ) {
					const RISEColor c = img.GetPEL( x, y );
					lum[ static_cast<size_t>( y ) * width + x ] =
						( c.base.r + c.base.g + c.base.b ) / 3.0;
				}
			}
		}
	};

	//! Median of a luminance vector (firefly-robust caustic signal).
	//! Skips non-finite samples.  Sorts a local copy (the caller's
	//! vector is reused across reads).
	double MedianLuminance( const std::vector<double>& lum )
	{
		std::vector<double> v;
		v.reserve( lum.size() );
		for( double x : lum ) {
			if( RISE::IsFiniteDouble( x ) ) {
				v.push_back( x );
			}
		}
		if( v.empty() ) {
			return 0.0;
		}
		std::sort( v.begin(), v.end() );
		const size_t n = v.size();
		return ( n & 1 ) ? v[n / 2] : 0.5 * ( v[n / 2 - 1] + v[n / 2] );
	}

	//! Mean of the finite luminance samples — the total-energy proxy behind
	//! the caustic transport-reach gate (the mean-lum VCM/PT ratio).  A real
	//! refractive caustic is energy PT structurally cannot reach, so VCM's
	//! mean far exceeds PT's; a converging dielectric scene (jewel_vault) has
	//! VCM-mean ≈ PT-mean.  Skips non-finite samples (same convention as the
	//! median).
	double MeanLuminance( const std::vector<double>& lum )
	{
		double accum = 0.0;
		size_t counted = 0;
		for( double x : lum ) {
			if( RISE::IsFiniteDouble( x ) ) { accum += x; ++counted; }
		}
		return counted ? accum / double( counted ) : 0.0;
	}

	//! Upper-tail-winsorized mean luminance — the firefly-robust transport-reach
	//! statistic.  The raw mean (MeanLuminance) is spiked by VCM's sparse merge
	//! fireflies at cheap probe spp, which inflates the VCM/PT reach ratio and
	//! over-fires the caustic route on non-caustic dielectric scenes (jewel_vault:
	//! true reach ~0.94, but a few fireflies push the raw-mean ratio past tau_reach
	//! ~2.6% of the time).  A REAL caustic's energy is BROAD (glass_pavilion reach
	//! 20-32x), so clamping each pixel to the p-th percentile leaves it essentially
	//! unchanged while clipping the sparse-spike tail.  Winsorize (clamp to the cap)
	//! rather than trim (drop) so broad real-caustic energy in the top bin still
	//! counts at the cap.  Skips non-finite samples (same convention as the others).
	double WinsorizedMeanLuminance( const std::vector<double>& lum, double pct )
	{
		std::vector<double> v;
		v.reserve( lum.size() );
		for( double x : lum ) {
			if( RISE::IsFiniteDouble( x ) ) { v.push_back( x ); }
		}
		if( v.empty() ) {
			return 0.0;
		}
		std::sort( v.begin(), v.end() );
		const size_t n = v.size();
		double p = pct;
		if( p < 0.0 ) { p = 0.0; }
		if( p > 1.0 ) { p = 1.0; }
		size_t capIdx = static_cast<size_t>( p * double( n ) );
		if( capIdx >= n ) { capIdx = n - 1; }
		const double cap = v[capIdx];
		double accum = 0.0;
		for( double x : v ) {
			accum += ( x > cap ) ? cap : x;
		}
		return accum / double( n );
	}

	//! Mean per-pixel sample variance across K probe sub-renders
	//! (unbiased, ddof=1) — the σ² in σ²·T.  `frames` are the
	//! per-render luminance vectors (all the same length).  Non-finite
	//! pixels contribute 0.  Returns 0 when fewer than 2 frames.
	//! Replicates phase3_analyze.py's `stack.var(axis=0, ddof=1).mean()`
	//! on the luminance channel.
	double MeanPerPixelVariance( const std::vector<std::vector<double>>& frames )
	{
		const size_t K = frames.size();
		if( K < 2 || frames[0].empty() ) {
			return 0.0;
		}
		const size_t N = frames[0].size();
		double accum = 0.0;
		size_t counted = 0;
		for( size_t i = 0; i < N; ++i ) {
			double mean = 0.0;
			bool   ok = true;
			for( size_t k = 0; k < K; ++k ) {
				const double v = frames[k][i];
			if( !RISE::IsFiniteDouble( v ) ) { ok = false; break; }
				mean += v;
			}
			if( !ok ) {
				continue;
			}
			mean /= double( K );
			double ss = 0.0;
			for( size_t k = 0; k < K; ++k ) {
				const double d = frames[k][i] - mean;
				ss += d * d;
			}
			accum += ss / double( K - 1 );
			++counted;
		}
		return counted ? accum / double( counted ) : 0.0;
	}
}

// Resolved concrete integrator name ("pt"/"bdpt"/"vcm", or "auto" pre-resolve)
// — the string form of ResolvedIntegrator() for the IRasterizer query surface.
const char* AutoRasterizer::ResolvedIntegratorName() const
{
	return IntegratorName( ResolvedIntegrator() );
}

AutoIntegratorChoice AutoRasterizer::ResolvedIntegrator() const
{
	std::lock_guard<std::mutex> lock(mResolutionStateMutex);
	return mResolved;
}

const char* AutoRasterizer::ResolveReason() const
{
	thread_local std::string snapshot;
	std::lock_guard<std::mutex> lock(mResolutionStateMutex);
	snapshot = mResolveReason;
	return snapshot.c_str();
}

double AutoRasterizer::LastProbeSeconds() const
{
	std::lock_guard<std::mutex> lock(mResolutionStateMutex);
	return mLastProbeSeconds;
}

unsigned int AutoRasterizer::LastProbeRenders() const
{
	std::lock_guard<std::mutex> lock(mResolutionStateMutex);
	return mLastProbeRenders;
}

void AutoRasterizer::SetResolveReason( const std::string& reason ) const
{
	std::lock_guard<std::mutex> lock(mResolutionStateMutex);
	mResolveReason = reason;
}

AutoRasterizer::AutoRasterizer(
	IRayCaster* caster,
	ISampling2D* samples,
	IPixelFilter* filter,
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
	const bool spectral,
	const SpectralConfig& spectralConfig,
	FrameStore* frameStore
	) :
	Rasterizer( frameStore )
	,mCaster( caster )
	,mSamples( samples )
	,mFilter( filter )
	,mPinned( integrator )
	,mOidnDenoise( oidnDenoise )
	,mOidnQuality( oidnQuality )
	,mOidnDevice( oidnDevice )
	,mOidnPrefilter( oidnPrefilter )
	,mGuiding( guidingConfig )
	,mAdaptive( adaptiveConfig )
	,mStability( stabilityConfig )
	,mUseZSobol( useZSobol )
	,mProgressive( progressiveConfig )
	,mProbeEnabled( probeEnabled )
	,mSpectral( spectral )
	,mSpectralConfig( spectralConfig )
	,mDelegate( 0 )
	,mReplayRevision( 0 )
	,mResolved( AutoIntegratorChoice::Auto )
	,mResolutionInProgress( false )
	,mResolutionComplete( false )
	,mLastProbeSeconds( 0.0 )
	,mLastProbeRenders( 0 )
{
	// Hold the shared build inputs alive for the wrapper's lifetime —
	// the delegate is built lazily and may not exist for many frames
	// after construction, so the caster / samples / filter must survive
	// past the Job's post-factory safe_release of its own local refs
	// (mirrors how the concrete rasterizers addref these via their
	// constructor + SubSampleRays).
	if( mCaster )  mCaster->addref();
	if( mSamples ) mSamples->addref();
	if( mFilter )  mFilter->addref();
	// Rasterizer owns the public prefilter introspection surface used by
	// AgentSession. Keep the wrapper's base state identical to the setting
	// forwarded to every lazily-created delegate.
	SetDenoisingPrefilter( mOidnPrefilter );
#ifdef RISE_ENABLE_OIDN
	SetDenoisingEnabled( mOidnDenoise );
	SetDenoisingQuality( mOidnQuality );
	SetDenoisingDevice( mOidnDevice );
#endif
}

AutoRasterizer::~AutoRasterizer()
{
	// The base ~Rasterizer releases mFrameStore + the buffered outs.
	IRasterizer* delegate = nullptr;
	{
		std::lock_guard<std::mutex> lock(outsMutex);
		delegate = mDelegate;
		mDelegate = nullptr;
	}
	safe_release( delegate );
	safe_release( mCaster );
	safe_release( mSamples );
	safe_release( mFilter );
}

AutoIntegratorChoice AutoRasterizer::SelectIntegrator( const IScene* scene ) const
{
	// Tier 0 — an explicit author pin always wins and skips static
	// analysis entirely (and Phase 4's probe).
	switch( mPinned ) {
		case AutoIntegratorChoice::PT:   SetResolveReason("author pin"); return AutoIntegratorChoice::PT;
		case AutoIntegratorChoice::BDPT: SetResolveReason("author pin"); return AutoIntegratorChoice::BDPT;
		case AutoIntegratorChoice::VCM:  SetResolveReason("author pin"); return AutoIntegratorChoice::VCM;
		case AutoIntegratorChoice::Auto:
		default:                         break;   // -> Tier-1 static analysis
	}

	// Resolve author pins first, then the fire/smoke G10 PT-only medium
	// rule, then the DL-475 support guard, then probe/static selection
	// (docs/AUTO_RASTERIZER_DESIGN.md §§2,5).  Census the assembled scene
	// at the first render-time entry: the capability guards read bound
	// (interior / heterogeneous / emissive) media as well as transmissive
	// materials and positional lights; probes cannot repair missing support.
	if( !scene ) {
		// Defensive: the render path always passes a real scene, but a
		// null here must not crash the dispatcher — fall back to PT.
		SetResolveReason("no scene (defaulted)");
		return AutoIntegratorChoice::PT;
	}

	// G10 is a hard capability route for automatic selection, so it must
	// precede the optional Tier-2 probe: a probe result may refine supported
	// surface scenes, but may not select an integrator that lacks the PT-only
	// medium feature set.
	if( SceneHasPTOnlyMedium( *scene ) ) {
		SetResolveReason("fire, heterogeneous, or emissive medium");
		return AutoIntegratorChoice::PT;
	}

	// Part 0 containment (docs/MIX_MATERIAL_PATH_STATE_AO_DESIGN.md):
	// non-physical path/view-dependent inputs and spatially varying SSS mixes
	// are PT-only; route away from BDPT/VCM before any probe or static rule.
	if( const ObjectManager* pObjMgr = dynamic_cast<const ObjectManager*>( scene->GetObjects() ) ) {
		if( pObjMgr->ComputeScenePathDependence() != 0u ) {
			SetResolveReason("path-dependent or varying-SSS-mix material (PT-only)");
			return AutoIntegratorChoice::PT;
		}
		if( pObjMgr->SceneHasHyLIoSMaterial() ) {
			SetResolveReason("HyLIoS hyperspectral skin material (PT-only)");
			return AutoIntegratorChoice::PT;
		}
	}

	const bool hasTransmissive = SceneHasTransmissiveMaterial( *scene );
	const bool hasPositional   = SceneHasPositionalLight( *scene );
	const bool hasEnvIBL       = ( scene->GetGlobalRadianceMap() != 0 );

    // A variance probe cannot detect absent support: an all-black BDPT
    // candidate can even appear noise-free. Select a sampler that reaches
    // the single-medium-scatter class before measuring candidate costs.
    if(hasPositional && SceneHasTransmissiveInteriorMedium(*scene)) {
        SetResolveReason("DL-475: transmissive interior medium + positional light (support guard)");
        GlobalLog()->PrintEx(eLog_Warning,
            "AutoRasterizer:: DL-475: BDPT cannot sample a single medium scatter between delta walls under a positional light; conservatively selecting VCM before the probe. Explicit author pins remain honored.");
        return AutoIntegratorChoice::VCM;
    }


	// Tier 2 — the render-time probe (Phase 4).  Gated twice: the author
	// must opt in (`probe` on the chunk) AND the production sample count
	// must clear the activation-spp threshold — below it the probe's fixed
	// per-render tax dwarfs the budget (Phase-3 §6.1 COST: cheap only at
	// production spp >= ~256), so we stay on the Tier-1 static guess for
	// previews/interactive.  When it runs, the probe SUPERSEDES the static
	// guess: its gated/short-circuited decision tree (RunProbe) reads the
	// caustic + strong-indirect regimes the static tier provably can't
	// (the gi_spheres/ggx_showcase blind-spot split; the area-lit
	// spectral_caustic VCM case).
	if( mProbeEnabled && mSamples ) {
		const ProbeConfig cfg = ReadProbeConfig();
		const unsigned int productionSpp = mSamples->GetNumSamples();
		if( productionSpp >= cfg.activationSpp ) {
			return RunProbe( scene, cfg, hasTransmissive, hasEnvIBL );
		}
		GlobalLog()->PrintEx( eLog_Event,
			"AutoRasterizer:: probe enabled but production spp %u < activation %u -> Tier-1 static",
			productionSpp, cfg.activationSpp );
	}

	// Tier 1 — static best-guess (the fallback when the probe is inactive).
	// VCM — only where refractive caustics are plausible: a transmissive /
	// dielectric surface (glass / water / gem) AND a positional point/spot
	// source to concentrate the refracted energy.  This is deliberately
	// COARSE (a dielectric scene without strong caustics will still lean
	// VCM here; the Phase-4 probe is what refines it back off VCM).  The
	// positional-light requirement is what spares the dielectric-but-
	// area-lit diffuse/indirect scenes — jewel_vault, cloister, alchemists,
	// env_only, prism all carry glass yet have NO point light, so they
	// correctly stay off VCM.  Validated against the Phase-1 matrix: this
	// hits all four point/spot-lit dielectric-caustic scenes (pool_caustics,
	// glass_pavilion, diamond_teapot, torus_chain) with no false positives.
	if( hasTransmissive && hasPositional ) {
		SetResolveReason("dielectric + positional light");
		return AutoIntegratorChoice::VCM;
	}

	// Everything else -> PT.  Note in particular that the strong-indirect /
	// BDPT regime is NOT routed here.  It is not statically separable from
	// the PT-efficient glossy/diffuse bulk via cheap IScene signals: the
	// BDPT-winning gi_spheres is byte-identical in every reachable signal
	// to the PT-winning ggx_showcase (both enclosed, single area light, no
	// point/env/glass/fog — they differ only in diffuse-vs-glossy
	// reflectance, which IScene exposes no accessor for).  Routing on that
	// would mis-send the converged glossy bulk to BDPT, the expensive
	// mistake.  So BDPT detection is deferred to the Phase-4 σ²·T probe;
	// the static tier conservatively defaults to PT.  (Full analysis +
	// matrix hit/miss table in AUTO_RASTERIZER_DESIGN.md §5.)
	SetResolveReason("no caustic/strong-indirect signal");
	return AutoIntegratorChoice::PT;
}

AutoRasterizer::ProbeConfig AutoRasterizer::ReadProbeConfig() const
{
	// Defaults are the Phase-3 experiment's validated values; every knob is
	// overridable from GlobalOptions so the §6.2 resolution/cost sweep can
	// vary them without recompiling.  activationSpp default is the §6.2
	// measured single-digit-% crossover (NOT baked at 256 a priori — see
	// AUTO_RASTERIZER_DESIGN.md §6.2 "activation-spp").
	IOptions& opt = GlobalOptions();
	ProbeConfig cfg;
	cfg.spp             = static_cast<unsigned int>( std::max( 1, opt.ReadInt( "auto_probe_spp", 4 ) ) );
	// scale = 4 (quarter-res) is the §6.2-measured sweet spot: every probe
	// decision held at 1/2..1/8 with zero flips, while 1/4 keeps even the
	// worst case (a BDPT-on-a-volume probe ruling out homogeneous_fog) to a
	// single-digit % of the 256-spp budget — half-res leaves that case at
	// ~32%.  At production res (>= the activation-spp gate) 1/4 still leaves
	// ample pixels for a stable median / per-pixel σ².
	cfg.scale           = static_cast<unsigned int>( std::max( 1, opt.ReadInt( "auto_probe_scale", 4 ) ) );
	cfg.tauCaustic      = opt.ReadDouble( "auto_probe_tau_caustic", 1.30 );
	// tau_reach is the transport-reach (VCM merge flux share) gate that rejects the
	// jewel_vault / converging-dielectric over-fire (DL-167); default 0.10 sits in the
	// measured 0.045|0.225 gap between converging dielectric scenes and true caustics
	// (AUTO_RASTERIZER_DESIGN.md §6.2).
	cfg.tauReach        = opt.ReadDouble( "auto_probe_tau_reach", 0.10 );
	cfg.reachWinsorPct  = opt.ReadDouble( "auto_probe_reach_winsor_pct", 0.99 );
	cfg.tauBdpt         = opt.ReadDouble( "auto_probe_tau_bdpt", 1.35 );
	cfg.varianceRenders = static_cast<unsigned int>( std::max( 2, opt.ReadInt( "auto_probe_variance_renders", 2 ) ) );
	cfg.activationSpp   = static_cast<unsigned int>( std::max( 1, opt.ReadInt( "auto_probe_activation_spp", 256 ) ) );
	return cfg;
}

AutoRasterizer::ProbeResult AutoRasterizer::ProbeCandidate(
	const IScene* scene, AutoIntegratorChoice choice,
	const ProbeConfig& cfg, bool needVariance ) const
{
	ProbeResult out;
	out.valid = false;
	out.medianLum = 0.0;
	out.meanLum = 0.0;
	out.robustMeanLum = 0.0;
	out.meanVar = 0.0;
	out.rasSeconds = 0.0;
	out.vcmMergeShare = 0.0;

#if defined DRAND48 || (!defined MERSENNE && !defined MERSENNE53)
    // The process-global C RNG backends cannot be restored by copying a generator.
    // This test hook requires the owned Mersenne backend; ordinary probes still run.
    if (mForTestFixedProbeWork) return out;
#endif

	if( !scene || !mSamples || (mForTestFixedProbeWork &&
		GlobalOptions().ReadInt("force_number_of_threads", 0) != 1) ) {
		return out;
	}

	// Resolve the assembled film + the privileged handle that lets us
	// shrink it to probe resolution.  Without IScenePriv (or a film) we
	// cannot scale the render, so the probe cannot run for this scene.
	IScenePriv* scenePriv = dynamic_cast<IScenePriv*>( const_cast<IScene*>( scene ) );
	const IFilm* film = scene->GetFilm();
	if( !scenePriv || !film ) {
		GlobalLog()->PrintEasyWarning(
			"AutoRasterizer:: probe cannot resolve IScenePriv/film; skipping candidate" );
		return out;
	}

	const unsigned int origW  = film->GetWidth();
	const unsigned int origH  = film->GetHeight();
	const Scalar       origAR = film->GetPixelAR();
	const unsigned int probeW = std::max( 1u, origW / cfg.scale );
	const unsigned int probeH = std::max( 1u, origH / cfg.scale );

	// A low-spp probe sampler cloned off the canonical one — never mutate
	// the shared sampler the real delegate will use.
	ISampling2D* probeSampler = mSamples->Clone();
	if( !probeSampler ) {
		return out;
	}
	struct ProbeSamplerGuard
	{
		ISampling2D*& sampler;
		~ProbeSamplerGuard() { safe_release(sampler); }
	} probeSamplerGuard{ probeSampler };
	probeSampler->SetNumSamples( cfg.spp );

	// Shrink the film for the duration of the probe. The resolution owner is
	// exclusive, and each candidate render joins its workers before the next
	// resize or restoration (ResizeFilm's concurrency contract).
	//
	// The probe runs INSIDE a render request: Job::Rasterize holds the prepared
	// mutation freeze (Transformable::BeginPreparedMutationFreeze) for the whole
	// call, and under that freeze CameraCommon::SetDimensionsAndPixelAR refuses
	// an external mutation and returns silently.  Without the internal-mutation
	// capability below, ResizeFilm shrinks the Film but leaves every camera at
	// the full dimensions, so each candidate renders the probeW x probeH CORNER
	// of the full-resolution view (re-framed, not re-resolved) and the routing
	// statistics describe 1/scale^2 of the frame.  The resize is the render's
	// own derived, self-restoring update -- the same narrowly scoped capability
	// the prepared animator evaluation takes -- so it is taken here for exactly
	// the two ResizeFilm calls and on this thread only.
	struct ProbeFilmResize
	{
		static void Apply( IScenePriv& scene, unsigned int w, unsigned int h, Scalar pixelAR )
		{
			struct Capability
			{
				Capability() { Transformable::BeginPreparedInternalMutation(); }
				~Capability() { Transformable::EndPreparedInternalMutation(); }
			} capability;
			scene.ResizeFilm( w, h, pixelAR );
		}
	};
	struct FilmRestoreGuard
	{
		IScenePriv& scene;
		unsigned int width;
		unsigned int height;
		Scalar pixelAR;
		~FilmRestoreGuard() { ProbeFilmResize::Apply(scene,width,height,pixelAR); }
	} filmRestore{ *scenePriv, origW, origH, origAR };
	ProbeFilmResize::Apply( *scenePriv, probeW, probeH, origAR );

	// Fail closed if the active camera did not follow the film: a probe of a
	// re-framed corner is worse than no probe.  An invalid result sends RunProbe
	// to its "insufficient probe signal" PT fallback without rendering anything.
	if( const Implementation::CameraCommon* cam =
		dynamic_cast<const Implementation::CameraCommon*>( scene->GetCamera() ) )
	{
		if( cam->GetWidth() != probeW || cam->GetHeight() != probeH ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"AutoRasterizer:: probe camera stayed at %ux%u after film resize to %ux%u; skipping candidate",
				cam->GetWidth(), cam->GetHeight(), probeW, probeH );
			return out;
		}
	}

	const unsigned int nRenders = needVariance ? cfg.varianceRenders : 1u;
	std::vector<std::vector<double>> frames;
	frames.reserve( nRenders );
	double totalSeconds = 0.0;
	bool   anyValid = false;

	// Probe always renders RAW + UNIFORM: denoise OFF (the σ² signal is the
	// raw per-pixel noise; a denoiser would erase it), guiding/adaptive OFF
	// (no expensive training pass; uniform spp so cost + variance are
	// well-defined and the per-render time is the σ²·T `T`).
	const PathGuidingConfig    probeGuiding;	// default = disabled
	const AdaptiveSamplingConfig probeAdaptive;	// default maxSamples=0 = off

	// The opt-in test mode isolates routing policy from scheduler/clock noise.
	// Restore the Sobol salt and owned GlobalRNG after the candidate.
	// This hook also seeds C rand() for constructors; that process-global
	// stream is not restored. Production never enables the hook.
	const uint32_t savedSalt = mForTestFixedProbeWork ? SobolSamplerTestHooks::ValueSalt().load() : 0;
	std::unique_ptr<RandomNumberGenerator> savedRng;
	if( mForTestFixedProbeWork ) savedRng.reset( new RandomNumberGenerator(GlobalRNG()) );
	for( unsigned int r = 0; r < nRenders; ++r ) {
		if( mForTestFixedProbeWork ) {
			SobolSamplerTestHooks::ValueSalt().store( savedSalt ^ (0x9e3779b9u * (r + 1)) );
			GlobalRNG().Reseed( 0x546000u + r );
			std::srand( 0x546000u + r );
		}
		// Build a concrete probe delegate with the low-spp sampler and a
		// NULL FrameStore — the candidate render lands in the delegate's own
		// internal image (flushed to our capturing output) and never touches
		// the real canonical store.  Reuses the same factories as the real
		// delegate, so the probe image IS the candidate integrator's image.
		IRasterizer* d = BuildDelegate( choice, probeSampler, /*fs*/ 0,
			/*oidnDenoise*/ false, probeGuiding, probeAdaptive );
		if( !d ) {
			break;
		}
		struct ProbeDelegateGuard
		{
			IRasterizer*& delegate;
			~ProbeDelegateGuard() { safe_release(delegate); }
		} probeDelegateGuard{ d };
		ProbeCaptureOutput* cap = new ProbeCaptureOutput();
		GlobalLog()->PrintNew( cap, __FILE__, __LINE__, "probe capture output" );
		struct ProbeOutputGuard
		{
			ProbeCaptureOutput*& output;
			~ProbeOutputGuard() { safe_release(output); }
		} probeOutputGuard{ cap };
		d->AddRasterizerOutput( cap );
		if( mFailProbeForTest.exchange(false,std::memory_order_acq_rel) ) {
			throw std::runtime_error("injected Auto probe failure after output registration");
		}

		// Production uses multi-threaded frames: the QMC sample stream is deterministic
		// (HashCombine(x,y) seed), so the per-pixel variance signal comes
		// from multi-threaded run-to-run non-determinism — exactly the
		// Phase-3 inter-run proxy. The opt-in test mode instead supplies
		// distinct salts and forces one thread.
		const auto t0 = std::chrono::steady_clock::now();
		d->RasterizeScene( *scene, /*pRect*/ 0, /*pRasterSequence*/ 0 );
		const auto t1 = std::chrono::steady_clock::now();
		totalSeconds += std::chrono::duration<double>( t1 - t0 ).count();

		if( !cap->lum.empty() ) {
			frames.push_back( cap->lum );
			anyValid = true;
		}

		if( choice == AutoIntegratorChoice::VCM ) {
			VCMRasterizerBase* vcmBase = dynamic_cast<VCMRasterizerBase*>( d );
			if( vcmBase ) {
				out.vcmMergeShare = vcmBase->GetMergeFluxShare();
			}
		}
	}

	if( mForTestFixedProbeWork ) {
		SobolSamplerTestHooks::ValueSalt().store( savedSalt );
		GlobalRNG() = *savedRng;
	}

	// The production film dims (+ camera re-sync) are restored by
	// `filmRestore` on every exit path, after the statistics below.
	if( anyValid ) {
		out.valid      = true;
		out.medianLum  = MedianLuminance( frames.front() );
		out.meanLum    = MeanLuminance( frames.front() );
		out.robustMeanLum = WinsorizedMeanLuminance( frames.front(), cfg.reachWinsorPct );
		out.meanVar    = MeanPerPixelVariance( frames );
		out.rasSeconds = totalSeconds;
		out.lum        = frames.front();
	}
	return out;
}

AutoRasterizer::CausticReachSignals AutoRasterizer::ComputeCausticReachSignals(
	const ProbeResult& pt, const ProbeResult& vcm )
{
	CausticReachSignals s{};
	if( !pt.valid || !vcm.valid || pt.lum.empty() || vcm.lum.empty() ) {
		return s;
	}
	s.valid = true;
	s.medRatio = ( pt.medianLum > 0.0 ) ? ( vcm.medianLum / pt.medianLum ) : 0.0;
	s.meanRatio = ( pt.meanLum > 0.0 ) ? ( vcm.meanLum / pt.meanLum ) : 0.0;
	s.robustMeanRatio = ( pt.meanLum > 0.0 ) ? ( vcm.robustMeanLum / pt.meanLum ) : 0.0;
	s.vcmMergeShare = vcm.vcmMergeShare;

	const size_t N = std::min( pt.lum.size(), vcm.lum.size() );
	if( N == 0 ) {
		return s;
	}

	// Regularization floor to avoid 0/0 and spurious noise explosion on dark pixels.
	const double ptMean = pt.meanLum;
	const double eps = std::max( 1e-5, 0.001 * ptMean );
	const double sigFloor = std::max( 1e-5, 0.01 * vcm.meanLum );

	std::vector<double> ratios;
	ratios.reserve( N );

	size_t countGt2 = 0;
	size_t countGt1_5 = 0;
	size_t countGt3 = 0;
	double vcmGt2Flux = 0.0;
	double totalVcmFlux = 0.0;

	for( size_t i = 0; i < N; ++i ) {
		const double p = pt.lum[i];
		const double v = vcm.lum[i];
		if( !RISE::IsFiniteDouble( p ) || !RISE::IsFiniteDouble( v ) ) {
			continue;
		}
		if( v > 0.0 ) {
			totalVcmFlux += v;
		}

		// Regularized per-pixel ratio: (v + eps) / (p + eps)
		const double r = ( v + eps ) / ( p + eps );
		ratios.push_back( r );

		// Pixel counts where VCM is significantly brighter than PT
		if( v > sigFloor ) {
			if( v > 1.5 * p ) {
				++countGt1_5;
			}
			if( v > 2.0 * p ) {
				++countGt2;
				vcmGt2Flux += v;
			}
			if( v > 3.0 * p ) {
				++countGt3;
			}
		}
	}

	s.fracGt2 = double( countGt2 ) / double( N );
	s.fracGt1_5 = double( countGt1_5 ) / double( N );
	s.fracGt3 = double( countGt3 ) / double( N );
	s.energyFracGt2 = ( totalVcmFlux > 0.0 ) ? ( vcmGt2Flux / totalVcmFlux ) : 0.0;

	if( !ratios.empty() ) {
		std::sort( ratios.begin(), ratios.end() );
		const size_t nR = ratios.size();
		size_t idx95 = static_cast<size_t>( 0.95 * double( nR ) );
		if( idx95 >= nR ) idx95 = nR - 1;
		size_t idx90 = static_cast<size_t>( 0.90 * double( nR ) );
		if( idx90 >= nR ) idx90 = nR - 1;
		s.p95Ratio = ratios[idx95];
		s.p90Ratio = ratios[idx90];
	}

	return s;
}

AutoIntegratorChoice AutoRasterizer::RunProbe(
	const IScene* scene, const ProbeConfig& cfg,
	bool dielectric, bool envIbl ) const
{
	char reason[256];

	// Cost instrumentation (the §6.2 sweep reads this — directly via
	// LastProbeSeconds()/LastProbeRenders() and from the summary log line).
	double probeSeconds = 0.0;
	unsigned int probeRenders = 0u;
	auto account = [&cfg, &probeSeconds, &probeRenders](
		const ProbeResult& r, bool variance ) {
		probeSeconds += r.rasSeconds;
		probeRenders += variance ? cfg.varianceRenders : 1u;
	};
	auto finish = [this, &cfg, &probeSeconds, &probeRenders](
		AutoIntegratorChoice pick ) -> AutoIntegratorChoice {
		{
			std::lock_guard<std::mutex> lock(mResolutionStateMutex);
			mLastProbeSeconds = probeSeconds;
			mLastProbeRenders = probeRenders;
		}
		GlobalLog()->PrintEx( eLog_Event,
			"AutoRasterizer:: probe cost %.3fs over %u candidate renders (scale 1/%u, spp %u)",
			probeSeconds, probeRenders, cfg.scale, cfg.spp );
		return pick;
	};

	// --- Caustic check FIRST, short-circuited (AUTO_RASTERIZER_DESIGN.md
	// §6.1).  GATED on (dielectric ∧ ¬env-IBL): env-IBL elevates VCM
	// luminance via the documented +63..76% env-bias, indistinguishable from
	// caustic gain to a luminance probe, so the env gate is the only clean
	// discriminator (a τ can't separate env_only 1.67× from
	// spectral_caustic 1.81×).  Primary signal is MEDIAN luminance (PT/BDPT
	// fireflies wreck the mean — median is firefly-robust); a second MEAN-
	// luminance transport-reach gate then rejects the jewel_vault over-fire
	// (§6.2 — see the two-gate comment below). ---
	if( dielectric && !envIbl ) {
		const ProbeResult pt  = ProbeCandidate( scene, AutoIntegratorChoice::PT,  cfg, /*needVariance*/ false );
		const ProbeResult vcm = ProbeCandidate( scene, AutoIntegratorChoice::VCM, cfg, /*needVariance*/ false );
		account( pt, false );
		account( vcm, false );
		if( pt.valid && vcm.valid && pt.medianLum > 0.0 ) {
			// Two-gate caustic test (AUTO_RASTERIZER_DESIGN.md §6.2 — the
			// jewel_vault over-fire fix; updated DL-167).  BOTH must hold to route VCM:
			//  (1) MEDIAN-ratio gate (firefly-robust) — VCM's TYPICAL pixel is
			//      brighter than PT's.
			//  (2) VCM MERGE-SHARE (transport-reach) gate (DL-167) — the discriminator.
			//      In true caustic scenes requiring VCM (diamond_teapot_pour, pool_caustics),
			//      a substantial fraction of the flux comes from photon vertex merges
			//      (diamond_teapot_pour ~22.5%, pool_caustics ~56.0%). In converging dielectric
			//      scenes where PT/BDPT reaches the energy via ordinary transport (glass_pavilion,
			//      crystal_garden, jewel_vault), vertex merge flux share is <= 4.5% (jewel_vault 0.0%).
			//      Threshold tau_reach = 0.10 (10%) provides a 9.6-sigma separation between true
			//      VCM caustics and non-caustic dielectric scenes, cleanly rejecting over-fires
			//      while reliably selecting VCM where needed.
			const double medRatio   = vcm.medianLum / pt.medianLum;
			const double mergeShare = vcm.vcmMergeShare;
			if( medRatio > cfg.tauCaustic && mergeShare > cfg.tauReach ) {
				std::snprintf( reason, sizeof(reason),
					"probe -> vcm: median %.2fx > %.2f and reach merge %.2f%% > %.2f%%",
					medRatio, cfg.tauCaustic, mergeShare * 100.0, cfg.tauReach * 100.0 );
				SetResolveReason(reason);
				return finish( AutoIntegratorChoice::VCM );
			}
			if( medRatio > cfg.tauCaustic ) {
				// Median fired but VCM merge share is low -> this is NOT a real caustic
				// (the jewel_vault / glass_pavilion class), just a slow-to-converge
				// dielectric scene. Don't over-route VCM; fall through to the general
				// BDPT-vs-PT check below.
				GlobalLog()->PrintEx( eLog_Event,
					"AutoRasterizer:: caustic median %.2fx fired but reach merge %.2f%% <= %.2f%%"
					" (PT reaches the energy) -> not a caustic, fall through to BDPT check",
					medRatio, mergeShare * 100.0, cfg.tauReach * 100.0 );
			}
		}
	}

	// --- BDPT check (σ²·T).  Big-margin only: τ_bdpt is set between the
	// near-tie PT band (~1.1×) and the smallest cheaply-detectable BDPT win
	// (gi_spheres 90×+).  Marginal ~1.5× BDPT wins are NOT cheaply separable
	// (they collapse to ~1.1× at probe spp) and correctly stay PT — a ~1.5×
	// mis-route is low-stakes vs the 90× caught here.  σ² is the mean
	// per-pixel cross-sub-render variance from `cfg.varianceRenders`
	// multi-threaded renders; T is the summed probe wall time. ---
	const ProbeResult pt   = ProbeCandidate( scene, AutoIntegratorChoice::PT,   cfg, /*needVariance*/ true );
	const ProbeResult bdpt = ProbeCandidate( scene, AutoIntegratorChoice::BDPT, cfg, /*needVariance*/ true );
	account( pt, true );
	account( bdpt, true );
	if( pt.valid && bdpt.valid ) {
		// Equal fixed pixel-sample budgets cancel in the test metric. Keep
        // the real measured seconds for cost instrumentation.
        const double pe = pt.meanVar * (mForTestFixedProbeWork ? 1.0 : pt.rasSeconds);
        const double be = bdpt.meanVar * (mForTestFixedProbeWork ? 1.0 : bdpt.rasSeconds);
		// σ²·T(PT) / σ²·T(BDPT) > τ_bdpt -> BDPT.  When BDPT reads
		// noise-free at probe spp (be <= 0) it wins iff PT still carries
		// noise — handled without forming an (UB under bare -ffast-math; fixed 2026-07-29, convention retained)
		// infinity by deciding the be<=0 branch directly.
		bool   chooseBdpt;
		double ratio = 0.0;
		if( be <= 0.0 ) {
			chooseBdpt = ( pe > 0.0 );
		} else {
			ratio = pe / be;
			chooseBdpt = ( ratio > cfg.tauBdpt );
		}
		if( chooseBdpt ) {
			if( be <= 0.0 ) {
				std::snprintf( reason, sizeof(reason),
					"probe -> bdpt: BDPT noise-free, PT sigma2T %.3g > 0", pe );
			} else {
				std::snprintf( reason, sizeof(reason),
					mForTestFixedProbeWork ? "test probe -> bdpt: variance ratio %.3fx > %.2f" :
					"probe -> bdpt: sigma2T %.1fx > %.2f", ratio, cfg.tauBdpt );
			}
			SetResolveReason(reason);
			return finish( AutoIntegratorChoice::BDPT );
		}
		std::snprintf( reason, sizeof(reason),
			mForTestFixedProbeWork ? "test probe -> pt: variance ratio %.3fx <= %.2f" :
			"probe -> pt: sigma2T %.2fx <= %.2f", ratio, cfg.tauBdpt );
		SetResolveReason(reason);
		return finish( AutoIntegratorChoice::PT );
	}

	SetResolveReason("probe -> pt (insufficient probe signal)");
	return finish( AutoIntegratorChoice::PT );
}

IRasterizer* AutoRasterizer::BuildDelegate( AutoIntegratorChoice choice ) const
{
	if( mReturnNullDelegateForTest.exchange(false,std::memory_order_acq_rel) ) {
		return nullptr;
	}
	// The real delegate: canonical sampler + canonical FrameStore + the
	// wrapper's own denoise / guiding / adaptive configs.  This is the
	// construction the concrete chunk parsers perform, so pinning `auto`
	// to X yields the same image as a bare X_pel_rasterizer.
	FrameStore* frameStore = nullptr;
	{
		std::lock_guard<std::mutex> lock(outsMutex);
		frameStore = mFrameStore;
		if( frameStore ) frameStore->addref();
	}
	IRasterizer* delegate = nullptr;
	try {
		delegate = BuildDelegate( choice, mSamples, frameStore,
			mOidnDenoise, mGuiding, mAdaptive );
		if( delegate ) {
			delegate->SetTransparentBackground( GetTransparentBackground() );
		}
	}
	catch( ... ) {
		safe_release(frameStore);
		throw;
	}
	safe_release(frameStore);
	return delegate;
}

IRasterizer* AutoRasterizer::BuildDelegate(
	AutoIntegratorChoice choice,
	ISampling2D* samples, FrameStore* fs,
	bool oidnDenoise,
	const PathGuidingConfig& guiding,
	const AdaptiveSamplingConfig& adaptive ) const
{
	IRasterizer* d = 0;

	// SPECTRAL DOMAIN — delegate to the *_spectral_ factories.  This `if`
	// is the ONLY domain-specific code in the wrapper; the integrator choice,
	// the canonical per-integrator depth/merge defaults, and the
	// caster/sampler/filter/stability/FrameStore plumbing are identical to the
	// Pel branch.  The decision logic that PICKED `choice` (SelectIntegrator /
	// RunProbe / ProbeCandidate) is shared verbatim across both domains.
	if( mSpectral )
	{
		const SpectralConfig& sc = mSpectralConfig;
		switch( choice )
		{
		case AutoIntegratorChoice::BDPT:
			{
				// BDPTSpectralDefaults == BDPTPelDefaults depths; mirror
				// Job::SetBDPTSpectralRasterizer, which builds the *Adaptive
				// factory (the legacy non-adaptive overload is ABI-only).
				BDPTSpectralDefaults bd;
				RISE_API_CreateBDPTSpectralRasterizerAdaptive( &d, mCaster, samples, mFilter,
					bd.maxEyeDepth, bd.maxLightDepth,
					sc.nmBegin, sc.nmEnd, sc.numWavelengths, sc.spectralSamples,
					oidnDenoise, mOidnQuality, mOidnDevice, mOidnPrefilter,
					guiding, adaptive, mStability, mUseZSobol, sc.useHWSS, fs );
			}
			break;

		case AutoIntegratorChoice::VCM:
			{
				VCMSpectralDefaults vd;
				RISE_API_CreateVCMSpectralRasterizer( &d, mCaster, samples, mFilter,
					vd.maxEyeDepth, vd.maxLightDepth,
					sc.nmBegin, sc.nmEnd, sc.numWavelengths, sc.spectralSamples,
					vd.mergeRadius, vd.enableVC, vd.enableVM,
					oidnDenoise, mOidnQuality, mOidnDevice, mOidnPrefilter,
					guiding, adaptive, mStability, mUseZSobol, sc.useHWSS, fs );
			}
			break;

		case AutoIntegratorChoice::PT:
		case AutoIntegratorChoice::Auto:
		default:
			{
				// Spectral PT takes NO path-guiding arg (the spectral PT
				// factory has none — see SPECTRAL_PARITY_AUDIT §1) so `guiding`
				// is intentionally not forwarded here; it still rides the
				// BDPT/VCM spectral cases above.  Matches
				// Job::SetPathTracingSpectralRasterizer's argument set.
				SMSConfig sms;
				RISE_API_CreatePathTracingSpectralRasterizer( &d, mCaster, samples, mFilter,
					sc.nmBegin, sc.nmEnd, sc.numWavelengths, sc.spectralSamples,
					sms.enabled, sms.maxIterations, sms.threshold, sms.maxChainDepth, sms.biased,
					sms.bernoulliTrials, sms.multiTrials, sms.photonCount, sms.twoStage,
					sms.useLevenbergMarquardt, sms.seedingMode, sms.targetBounces,
					oidnDenoise, mOidnQuality, mOidnDevice, mOidnPrefilter,
					adaptive, mStability, mUseZSobol, sc.useHWSS, fs );
			}
			break;
		}
		return d;
	}

	// --- Pel (RGB) domain (the original Phase-1 path) ---
	switch( choice )
	{
	case AutoIntegratorChoice::BDPT:
		{
			// Per-integrator specifics use the canonical defaults — the
			// single source of truth shared with the bdpt_pel_rasterizer
			// parser + Job lazy-build (option (i) param population).
			BDPTPelDefaults bd;
			RISE_API_CreateBDPTPelRasterizer( &d, mCaster, samples, mFilter,
				bd.maxEyeDepth, bd.maxLightDepth,
				oidnDenoise, mOidnQuality, mOidnDevice, mOidnPrefilter,
				guiding, adaptive, mStability, mUseZSobol, fs );
		}
		break;

	case AutoIntegratorChoice::VCM:
		{
			VCMPelDefaults vd;
			RISE_API_CreateVCMPelRasterizer( &d, mCaster, samples, mFilter,
				vd.maxEyeDepth, vd.maxLightDepth, vd.mergeRadius, vd.enableVC, vd.enableVM,
				oidnDenoise, mOidnQuality, mOidnDevice, mOidnPrefilter,
				guiding, adaptive, mStability, mUseZSobol, fs );
		}
		break;

	case AutoIntegratorChoice::PT:
	case AutoIntegratorChoice::Auto:
	default:
		{
			// Default SMSConfig == SMS disabled, which is exactly what a
			// bare pathtracing_pel_rasterizer builds.  The factory ignores
			// the remaining SMS fields when enabled == false.
			SMSConfig sms;
			RISE_API_CreatePathTracingPelRasterizer( &d, mCaster, samples, mFilter,
				sms.enabled, sms.maxIterations, sms.threshold, sms.maxChainDepth, sms.biased,
				sms.bernoulliTrials, sms.multiTrials, sms.photonCount, sms.twoStage,
				sms.useLevenbergMarquardt, sms.seedingMode, sms.targetBounces,
				oidnDenoise, mOidnQuality, mOidnDevice, mOidnPrefilter,
				guiding, adaptive, mStability, mUseZSobol, fs );
		}
		break;
	}

	return d;
}

void AutoRasterizer::EnsureResolved( const IScene* scene ) const
{
	if( std::find(gResolvingAutoRasterizers.begin(),gResolvingAutoRasterizers.end(),this) !=
		gResolvingAutoRasterizers.end() ) {
		throw std::runtime_error(
			"output_provenance_unavailable: Auto delegate resolution is reentrant");
	}
	const AutoRasterizer* const waitingFrom = gResolvingAutoRasterizers.empty() ?
		nullptr : gResolvingAutoRasterizers.back();
	{
		std::unique_lock<std::mutex> lock(gAutoResolutionMutex);
		for( ;; ) {
			if( mResolutionComplete ) return;
			if( !mResolutionInProgress ) {
				if( !waitingFrom && !gAutoResolutionsActive.empty() ) {
					throw std::runtime_error(
						"output_provenance_unavailable: Auto delegate resolution is concurrent");
				}
				mResolutionInProgress = true;
				gAutoResolutionsActive.insert(this);
				break;
			}
			if( waitingFrom ) {
				gAutoResolutionWaits[waitingFrom] = this;
				const AutoRasterizer* cursor = this;
				bool cycle = false;
				for( std::size_t traversed=0u;
					cursor && traversed<=gAutoResolutionWaits.size(); ++traversed ) {
					if( cursor == waitingFrom ) {
						cycle = true;
						break;
					}
					const auto edge = gAutoResolutionWaits.find(cursor);
					cursor = edge == gAutoResolutionWaits.end() ? nullptr : edge->second;
				}
				if( cycle ) {
					gAutoResolutionWaits.erase(waitingFrom);
					throw std::runtime_error(
						"output_provenance_unavailable: Auto delegate resolution has a cross-thread cycle");
				}
				gAutoResolutionChanged.wait(lock,[this]() {
					return !mResolutionInProgress || mResolutionComplete;
				});
				gAutoResolutionWaits.erase(waitingFrom);
			} else {
				throw std::runtime_error(
					"output_provenance_unavailable: Auto delegate resolution is concurrent");
			}
		}
	}
	try {
		[&]() {
		struct ResolutionActivity
		{
			std::vector<const AutoRasterizer*>& active;
			ResolutionActivity(
				std::vector<const AutoRasterizer*>& stack,
				const AutoRasterizer* current ) : active(stack) { active.push_back(current); }
			~ResolutionActivity() { active.pop_back(); }
		} resolutionActivity(gResolvingAutoRasterizers,this);
		const AutoIntegratorChoice choice = SelectIntegrator( scene );
		IRasterizer* candidate = BuildDelegate( choice );

		if( !candidate ) {
			throw std::runtime_error(
				"output_provenance_unavailable: Auto failed to build the chosen delegate rasterizer" );
		}

		// The resolution coordinator retries the body after an exception. Keep the candidate
		// private and owned until every replay/configuration step succeeds so a
		// throwing output registration cannot publish or leak a half-resolved
		// delegate before that retry.
		struct CandidateUnwindGuard {
			IRasterizer*& p;
			~CandidateUnwindGuard() { safe_release( p ); }
		} candidateGuard{ candidate };

		// Progressive config can't ride RISE_API_SetRasterizerProgressiveRendering
		// on the wrapper (that down-casts to PixelBasedRasterizerHelper, which the
		// wrapper is not) — apply it to the delegate, which IS one.
		RISE_API_SetRasterizerProgressiveRendering(
			candidate, mProgressive.enabled, mProgressive.samplesPerPass );
		const std::string resolveReason = ResolveReason();
		GlobalLog()->PrintEx( eLog_Event,
			"AutoRasterizer:: [%s] integrator '%s' -> delegating to '%s' (%s)",
			mSpectral ? "spectral" : "pel", IntegratorName( mPinned ), IntegratorName( choice ),
			resolveReason.empty() ? "default" : resolveReason.c_str() );

		// Replay outside the wrapper lock because AddRasterizerOutput notifies
		// arbitrary sinks. Every wrapper mutation advances mReplayRevision. If a
		// callback changes progress or output state, discard the candidate's staged
		// state and replay a fresh authoritative snapshot before publication.
		bool replayedAfterMutation = false;
		for( ;; ) {
			std::vector<IRasterizerOutput*> snapshot;
			IProgressCallback* progress = nullptr;
			unsigned long long revision = 0;
			{
				std::lock_guard<std::mutex> lock( outsMutex );
				revision = mReplayRevision;
				progress = pProgressFunc;
				for( IRasterizerOutput* output : outs ) {
					output->addref();
					try {
						snapshot.push_back(output);
					}
					catch( ... ) {
						output->release();
						for( IRasterizerOutput* retained : snapshot ) retained->release();
						throw;
					}
				}
			}
			candidate->FreeRasterizerOutputs();
			candidate->SetProgressCallback( progress );
			try {
				for( IRasterizerOutput* output : snapshot ) {
					candidate->AddRasterizerOutput(output);
				}
			}
			catch( ... ) {
				for( IRasterizerOutput* output : snapshot ) output->release();
				throw;
			}
			for( IRasterizerOutput* output : snapshot ) output->release();
			{
				std::lock_guard<std::mutex> lock( outsMutex );
				if( revision != mReplayRevision ) {
					if( replayedAfterMutation ) {
						throw std::runtime_error(
							"output_provenance_unavailable: Auto delegate replay did not converge");
					}
					replayedAfterMutation = true;
					continue;
				}
				mDelegate = candidate;
				candidate = nullptr;
				{
					std::lock_guard<std::mutex> stateLock(mResolutionStateMutex);
					mResolved = choice;
				}
				break;
			}
		}

		}();
	}
	catch( ... ) {
		{
			std::lock_guard<std::mutex> lock(gAutoResolutionMutex);
			mResolutionInProgress = false;
			gAutoResolutionsActive.erase(this);
		}
		gAutoResolutionChanged.notify_all();
		throw;
	}
	{
		std::lock_guard<std::mutex> lock(gAutoResolutionMutex);
		mResolutionComplete = true;
		mResolutionInProgress = false;
		gAutoResolutionsActive.erase(this);
	}
	gAutoResolutionChanged.notify_all();
}

void AutoRasterizer::ForTest_ThrowInsideProbe( const IScene& scene ) const
{
	mFailProbeForTest.store(true,std::memory_order_release);
	ProbeCandidate(&scene,AutoIntegratorChoice::PT,ReadProbeConfig(),false);
}

unsigned int AutoRasterizer::ForTest_LiveProbeCaptureCount() const
{
	return gProbeCapturesLive.load(std::memory_order_acquire);
}

void AutoRasterizer::ForTest_ReturnNullDelegateOnce() const
{
	mReturnNullDelegateForTest.store(true,std::memory_order_release);
}

IRasterizer* AutoRasterizer::RetainDelegate() const
{
	std::lock_guard<std::mutex> lock(outsMutex);
	IRasterizer* delegate = mDelegate;
	if( delegate ) delegate->addref();
	return delegate;
}

bool AutoRasterizer::HonorsRegion() const
{
	IRasterizer* delegate = RetainDelegate();
	const bool honors = delegate ? delegate->HonorsRegion() : true;
	safe_release(delegate);
	return honors;
}

bool AutoRasterizer::LastRenderCompleted() const
{
	IRasterizer* delegate = RetainDelegate();
	const IFireRasterizerState* state =
		dynamic_cast<const IFireRasterizerState*>(delegate);
	const bool completed = !state || state->LastRenderCompleted();
	safe_release(delegate);
	return completed;
}

bool AutoRasterizer::ResolveForFirePreflight( const IScene& scene ) const
{
	EnsureResolved(&scene);
	SyncDelegateFrameStore();
	IRasterizer* delegate = RetainDelegate();
	const bool resolved = delegate != nullptr;
	safe_release(delegate);
	return resolved;
}

//! G1 fix-round (2026-08-10).  The public, narrowly-named trigger for the
//! one-time resolution above; see AutoRasterizer.h for the full rationale.
//! It is a verbatim forward on purpose -- the agent's `render{isolate:}`
//! path needs to control only WHEN resolution runs (before it hides every
//! non-isolated object, so the visibility-filtered Tier-1 scans see the
//! FULL scene), never what it decides.  Deliberately does NOT touch
//! SyncDelegateFrameStore(): the caller is pre-resolving, not rendering,
//! and every render-time entry point syncs the delegate's FrameStore on
//! its own -- as does AutoRasterizer::SetFrameStore, which is what the
//! agent's FrameStore isolation guard drives afterwards.
void AutoRasterizer::PreResolveIntegrator( const IScene* scene ) const
{
	EnsureResolved( scene );
}

void AutoRasterizer::SyncDelegateFrameStore() const
{
	auto transaction = AcquireAutoMutationLock(mDelegateMutationMutex);
	IRasterizer* delegate = nullptr;
	FrameStore* mine = nullptr;
	{
		std::lock_guard<std::mutex> lock(outsMutex);
		delegate = mDelegate;
		if( delegate ) delegate->addref();
		mine = mFrameStore;
		if( mine ) mine->addref();
	}
	if( !delegate ) {
		safe_release(delegate);
		safe_release(mine);
		return;
	}
	if( !ReplayWrapperOutputTopologyToDelegate(delegate) ) {
		safe_release(delegate);
		safe_release(mine);
		throw std::runtime_error(
			"output_provenance_unavailable: Auto delegate output topology could not be replayed");
	}
	if( delegate->GetFrameStore() == mine ) {
		safe_release(delegate);
		safe_release(mine);
		return;
	}
	// The delegate is always an in-tree Rasterizer; push our current
	// FrameStore so a late SetFrameStore (deferred Job push when the
	// camera dims weren't known at construction, or a viewport resize)
	// reaches the object that writes pixels.  SetFrameStore balances the
	// refcount internally (addref new, release old).
	Rasterizer* r = dynamic_cast<Rasterizer*>( delegate );
	try {
		if( r ) r->SetFrameStore(mine);
	}
	catch( ... ) {
		const std::exception_ptr failure = std::current_exception();
		const bool reconciled = ReplayWrapperOutputTopologyToDelegate(delegate);
		safe_release(delegate);
		safe_release(mine);
		if( !reconciled ) {
			throw std::runtime_error(
				"output_provenance_unavailable: Auto delegate output topology could not be replayed");
		}
		std::rethrow_exception(failure);
	}
	safe_release(delegate);
	safe_release(mine);
}

void AutoRasterizer::SetFrameStore( FrameStore* frameStore )
{
	if( mFireOutputBindingInProgress.load(std::memory_order_acquire) != 0u ) {
		throw std::runtime_error(
			"output_provenance_unavailable: Auto FrameStore replay is reentrant");
	}
	auto transaction = AcquireAutoMutationLock(mDelegateMutationMutex);
	bool expected = false;
	if( !mFrameStoreReplayInProgress.compare_exchange_strong(
		expected,true,std::memory_order_acq_rel) ) {
		throw std::runtime_error(
			"output_provenance_unavailable: Auto FrameStore replay is reentrant");
	}
	struct ReplayActivity
	{
		std::atomic<bool>& active;
		~ReplayActivity() { active.store(false,std::memory_order_release); }
	} replayActivity { mFrameStoreReplayInProgress };
	FrameStore* previous = nullptr;
	{
		std::lock_guard<std::mutex> lock(outsMutex);
		previous = mFrameStore;
		if( previous ) previous->addref();
	}
	IRasterizer* retainedDelegate = RetainDelegate();
	try {
		Rasterizer::SetFrameStore( frameStore );
		{
			std::lock_guard<std::mutex> lock(outsMutex);
			++mReplayRevision;
		}
	}
	catch( ... ) {
		{
			std::lock_guard<std::mutex> lock(outsMutex);
			++mReplayRevision;
		}
		if( retainedDelegate ) ReconcileDelegateOutputTopology(retainedDelegate);
		safe_release(retainedDelegate);
		safe_release(previous);
		throw;
	}
	Rasterizer* delegate = dynamic_cast<Rasterizer*>(retainedDelegate);
	if( !delegate ) {
		safe_release(retainedDelegate);
		safe_release(previous);
		return;
	}
	try {
		delegate->SetFrameStore( frameStore );
	}
	catch( ... ) {
		const std::exception_ptr failure = std::current_exception();
		bool restored = true;
		try { RestoreFrameStoreAfterFailedTransaction(previous); }
		catch( ... ) { restored = false; }
		const bool reconciled = ReconcileDelegateOutputTopology(retainedDelegate);
		safe_release(retainedDelegate);
		safe_release(previous);
		if( !restored || !reconciled ) {
			throw std::runtime_error(
				"output_provenance_unavailable: Auto FrameStore rollback could not "
				"reconcile output topology");
		}
		std::rethrow_exception(failure);
	}
	safe_release(retainedDelegate);
	safe_release(previous);
}

FrameStore* AutoRasterizer::ForTest_GetDelegateFrameStore() const
{
	auto transaction = AcquireAutoMutationLock(mDelegateMutationMutex);
	IRasterizer* retained = RetainDelegate();
	Rasterizer* delegate = dynamic_cast<Rasterizer*>(retained);
	FrameStore* frameStore = delegate ? delegate->GetFrameStore() : nullptr;
	safe_release(retained);
	return frameStore;
}

void AutoRasterizer::ForTest_SetDelegateFrameStore( FrameStore* frameStore )
{
	auto transaction = AcquireAutoMutationLock(mDelegateMutationMutex);
	IRasterizer* retained = RetainDelegate();
	Rasterizer* delegate = dynamic_cast<Rasterizer*>(retained);
	if( delegate ) delegate->SetFrameStore(frameStore);
	safe_release(retained);
}

void AutoRasterizer::ForTest_FreeDelegateRasterizerOutputs()
{
	auto transaction = AcquireAutoMutationLock(mDelegateMutationMutex);
	IRasterizer* retained = RetainDelegate();
	if( retained ) retained->FreeRasterizerOutputs();
	safe_release(retained);
}

bool AutoRasterizer::ForTest_WrapperContainsOutput( IRasterizerOutput* output ) const
{
	auto transaction = AcquireAutoMutationLock(mDelegateMutationMutex);
	std::lock_guard<std::mutex> lock(outsMutex);
	return std::find(outs.begin(),outs.end(),output) != outs.end();
}

bool AutoRasterizer::ForTest_DelegateContainsOutput( IRasterizerOutput* output ) const
{
	auto transaction = AcquireAutoMutationLock(mDelegateMutationMutex);
	IRasterizer* retained = RetainDelegate();
	Rasterizer* concrete = dynamic_cast<Rasterizer*>(retained);
	std::vector<IRasterizerOutput*> outputs = concrete ?
		concrete->RetainRasterizerOutputs() : std::vector<IRasterizerOutput*>();
	const bool found = std::find(outputs.begin(),outputs.end(),output) != outputs.end();
	for( IRasterizerOutput* retainedOutput : outputs ) retainedOutput->release();
	safe_release(retained);
	return found;
}

bool AutoRasterizer::ReconcileDelegateOutputTopology( IRasterizer* delegate )
{
	Rasterizer* concrete = dynamic_cast<Rasterizer*>(delegate);
	if( !concrete ) return false;
	std::vector<IRasterizerOutput*> wrapperOutputs;
	std::vector<IRasterizerOutput*> delegateOutputs;
	try {
		wrapperOutputs = RetainRasterizerOutputs();
		delegateOutputs = concrete->RetainRasterizerOutputs();
		for( IRasterizerOutput* output : wrapperOutputs ) {
			if( std::find(delegateOutputs.begin(),delegateOutputs.end(),output) ==
				delegateOutputs.end() ) {
				Rasterizer::RemoveRasterizerOutput(output);
				std::lock_guard<std::mutex> lock(outsMutex);
				++mReplayRevision;
			}
		}
		for( IRasterizerOutput* output : delegateOutputs ) {
			if( std::find(wrapperOutputs.begin(),wrapperOutputs.end(),output) ==
				wrapperOutputs.end() ) concrete->RemoveRasterizerOutput(output);
		}
	}
	catch( ... ) {
		for( IRasterizerOutput* output : wrapperOutputs ) output->release();
		for( IRasterizerOutput* output : delegateOutputs ) output->release();
		return false;
	}
	for( IRasterizerOutput* output : wrapperOutputs ) output->release();
	for( IRasterizerOutput* output : delegateOutputs ) output->release();
	return true;
}

bool AutoRasterizer::ReplayWrapperOutputTopologyToDelegate( IRasterizer* delegate ) const
{
	Rasterizer* concrete = dynamic_cast<Rasterizer*>(delegate);
	if( !concrete ) return false;
	bool replayedAfterMutation = false;
	for( ;; ) {
		std::vector<IRasterizerOutput*> wrapperOutputs;
		std::vector<IRasterizerOutput*> delegateOutputs;
		unsigned long long revision = 0u;
		try {
			{
				std::lock_guard<std::mutex> lock(outsMutex);
				revision = mReplayRevision;
				for( IRasterizerOutput* output : outs ) {
					output->addref();
					try { wrapperOutputs.push_back(output); }
					catch( ... ) { output->release(); throw; }
				}
			}
			delegateOutputs = concrete->RetainRasterizerOutputs();
			for( IRasterizerOutput* output : delegateOutputs ) {
				if( std::find(wrapperOutputs.begin(),wrapperOutputs.end(),output) ==
					wrapperOutputs.end() ) concrete->RemoveRasterizerOutput(output);
			}
			for( IRasterizerOutput* output : wrapperOutputs ) {
				if( std::find(delegateOutputs.begin(),delegateOutputs.end(),output) ==
					delegateOutputs.end() ) concrete->AddRasterizerOutput(output);
			}
		}
		catch( ... ) {
			for( IRasterizerOutput* output : wrapperOutputs ) output->release();
			for( IRasterizerOutput* output : delegateOutputs ) output->release();
			return false;
		}
		for( IRasterizerOutput* output : wrapperOutputs ) output->release();
		for( IRasterizerOutput* output : delegateOutputs ) output->release();
		wrapperOutputs.clear();
		delegateOutputs.clear();
		{
			std::lock_guard<std::mutex> lock(outsMutex);
			if( revision != mReplayRevision ) {
				if( replayedAfterMutation ) return false;
				replayedAfterMutation = true;
				continue;
			}
		}

		try {
			wrapperOutputs = RetainRasterizerOutputs();
			delegateOutputs = concrete->RetainRasterizerOutputs();
		}
		catch( ... ) {
			for( IRasterizerOutput* output : wrapperOutputs ) output->release();
			for( IRasterizerOutput* output : delegateOutputs ) output->release();
			return false;
		}
		const bool equal = wrapperOutputs.size() == delegateOutputs.size() &&
			std::all_of(wrapperOutputs.begin(),wrapperOutputs.end(),
				[&]( IRasterizerOutput* output ) {
					return std::find(delegateOutputs.begin(),delegateOutputs.end(),output) !=
						delegateOutputs.end();
				});
		for( IRasterizerOutput* output : wrapperOutputs ) output->release();
		for( IRasterizerOutput* output : delegateOutputs ) output->release();
		if( equal ) return true;
		if( replayedAfterMutation ) return false;
		replayedAfterMutation = true;
	}
}

void AutoRasterizer::AttachToScene( const IScene* pScene )
{
	EnsureResolved( pScene );
	SyncDelegateFrameStore();
	IRasterizer* delegate = RetainDelegate();
	try {
		if( delegate ) delegate->AttachToScene(pScene);
	}
	catch( ... ) {
		safe_release(delegate);
		throw;
	}
	safe_release(delegate);
}

void AutoRasterizer::DetachFromScene( const IScene* pScene )
{
	IRasterizer* delegate = RetainDelegate();
	try {
		if( delegate ) delegate->DetachFromScene(pScene);
	}
	catch( ... ) {
		safe_release(delegate);
		throw;
	}
	safe_release(delegate);
}

//
// State-mutator forwarding (the use-after-free fix).  The base
// `Rasterizer` impls mutate the wrapper's OWN pProgressFunc / outs; we
// additionally push the change onto the resolved delegate so the object
// that actually renders stays in lock-step.  Before this, the delegate
// kept whatever it was seeded with at the one-time `EnsureResolved`
// replay -- a use-after-free once the host swapped that state.  The
// macOS GUI repro: render `auto` (a PT delegate freezes the then-current
// progress callback), Merge-load a new scene (the bridge deletes +
// recreates its progress callback), render `auto` again -- the delegate
// still pointed at the freed callback, so `pProgressFunc->SetTitle()` in
// PixelBasedRasterizerHelper::RasterizeScene jumped through a zeroed
// vtable slot (PC=0).  Forwarding makes every per-render
// SetProgressCallback / FreeRasterizerOutputs / AddRasterizerOutput from
// Job::Rasterize + the host bridge reach the delegate.
//
// The base call stays FIRST so the wrapper's own copy remains
// authoritative: a not-yet-resolved delegate is seeded from the
// wrapper's outs / pProgressFunc when `EnsureResolved` finally builds it.
//
void AutoRasterizer::SetProgressCallback( IProgressCallback* pFunc )
{
	auto transaction = AcquireAutoMutationLock(mDelegateMutationMutex);
	IRasterizer* delegate = nullptr;
	{
		std::lock_guard<std::mutex> lock( outsMutex );
		if( pProgressFunc != pFunc ) {
			pProgressFunc = pFunc;
			++mReplayRevision;
		}
		delegate = mDelegate;
		if( delegate ) delegate->addref();
	}
	if( delegate ) delegate->SetProgressCallback( pFunc );
	safe_release(delegate);
}

void AutoRasterizer::AddRasterizerOutput( IRasterizerOutput* ro )
{
	auto transaction = AcquireAutoMutationLock(mDelegateMutationMutex);
	const bool wrapperAdded = RegisterRasterizerOutput( ro );
	IRasterizer* delegate = nullptr;
	bool wrapperContains = false;
	{
		std::lock_guard<std::mutex> lock( outsMutex );
		if( wrapperAdded ) ++mReplayRevision;
		for( IRasterizerOutput* output : outs ) {
			if( output == ro ) {
				wrapperContains = true;
				break;
			}
		}
		delegate = mDelegate;
		if( delegate ) delegate->addref();
	}
	try {
		if( delegate && wrapperContains ) {
			delegate->AddRasterizerOutput( ro );
		}
	}
	catch( ... ) {
		safe_release(delegate);
		// Preserve the wrapper's pre-call state. The delegate's base
		// registration is itself strongly exception-safe; remove only an entry
		// inserted by this call, never a pre-existing deduplicated entry.
		if( wrapperAdded ) {
			Rasterizer::RemoveRasterizerOutput( ro );
		}
		throw;
	}
	safe_release(delegate);
}

void AutoRasterizer::RemoveRasterizerOutput( IRasterizerOutput* ro )
{
	auto transaction = AcquireAutoMutationLock(mDelegateMutationMutex);
	const bool wrapperRemoved = UnregisterRasterizerOutput( ro );
	IRasterizer* delegate = nullptr;
	{
		std::lock_guard<std::mutex> lock( outsMutex );
		if( wrapperRemoved ) ++mReplayRevision;
		delegate = mDelegate;
		if( delegate ) delegate->addref();
	}
	if( Rasterizer* concrete = dynamic_cast<Rasterizer*>(delegate) ) {
		concrete->RemoveRasterizerOutput( ro );
	}
	safe_release(delegate);
}

void AutoRasterizer::FreeRasterizerOutputs()
{
	auto transaction = AcquireAutoMutationLock(mDelegateMutationMutex);
	const bool wrapperReleased = ReleaseRasterizerOutputs();
	IRasterizer* delegate = nullptr;
	{
		std::lock_guard<std::mutex> lock( outsMutex );
		if( wrapperReleased ) ++mReplayRevision;
		delegate = mDelegate;
		if( delegate ) delegate->addref();
	}
	if( delegate ) delegate->FreeRasterizerOutputs();
	safe_release(delegate);
}

bool AutoRasterizer::SetDenoisingOverride( bool enabled )
{
	// Same delegate-mutation transaction as the other forwarded mutators:
	// the delegate is retained, never read through the raw pointer.
	auto transaction = AcquireAutoMutationLock(mDelegateMutationMutex);
	if( !Rasterizer::SetDenoisingOverride( enabled ) ) {
		return false;
	}
	mOidnDenoise = enabled;
	IRasterizer* delegate = RetainDelegate();
	bool applied = true;
	try {
		if( delegate ) applied = delegate->SetDenoisingOverride( enabled );
	}
	catch( ... ) {
		safe_release(delegate);
		throw;
	}
	safe_release(delegate);
	return applied;
}

int AutoRasterizer::GetDenoisingOverride() const
{
#ifdef RISE_ENABLE_OIDN
	IRasterizer* delegate = RetainDelegate();
	const int value = delegate ? delegate->GetDenoisingOverride() :
		( mOidnDenoise ? 1 : 0 );
	safe_release(delegate);
	return value;
#else
	return 0;
#endif
}

void AutoRasterizer::SetTransparentBackground( bool enabled )
{
	auto transaction = AcquireAutoMutationLock(mDelegateMutationMutex);
	Rasterizer::SetTransparentBackground( enabled );
	IRasterizer* delegate = RetainDelegate();
	try {
		if( delegate ) delegate->SetTransparentBackground( enabled );
	}
	catch( ... ) {
		safe_release(delegate);
		throw;
	}
	safe_release(delegate);
}

bool AutoRasterizer::GetTransparentBackground() const
{
	IRasterizer* delegate = RetainDelegate();
	const bool value = delegate ? delegate->GetTransparentBackground() :
		Rasterizer::GetTransparentBackground();
	safe_release(delegate);
	return value;
}

unsigned int AutoRasterizer::PredictTimeToRasterizeScene(
	const IScene& pScene,
	const ISampling2D& pSampling,
	unsigned int* pActualTime
	) const
{
	EnsureResolved( &pScene );
	SyncDelegateFrameStore();
	RequireFireRenderPreflight(
		pScene,FireRenderPreflightAuthorization::Prediction);
	IRasterizer* delegate = RetainDelegate();
	if( delegate ) {
		unsigned int predicted = 0u;
		try {
			predicted = delegate->PredictTimeToRasterizeScene(
				pScene,pSampling,pActualTime);
		}
		catch( ... ) {
			safe_release(delegate);
			throw;
		}
		safe_release(delegate);
		return predicted;
	}
	if( pActualTime ) {
		*pActualTime = 0;
	}
	return 0;
}

unsigned int AutoRasterizer::PredictTimeToRasterizeScenePrepared(
	const IScene& scene, const ISampling2D& sampling, unsigned int* actual,
	const Scalar nominal, IRenderPreparationController& controller ) const
{
	EnsureResolved(&scene);
	SyncDelegateFrameStore();
	IRasterizer* delegate = RetainDelegate();
	if( !delegate ) return 0xFFFFFFFFu;
	unsigned int result = 0xFFFFFFFFu;
	try { result = delegate->PredictTimeToRasterizeScenePrepared(
		scene,sampling,actual,nominal,controller); }
	catch( ... ) { safe_release(delegate); throw; }
	safe_release(delegate);
	return result;
}

void AutoRasterizer::RasterizeScene(
	const IScene& pScene,
	const Rect* pRect,
	IRasterizeSequence* pRasterSequence
	) const
{
	EnsureResolved( &pScene );
	SyncDelegateFrameStore();
	FireOutputTopologyLease fireOutputTopologyLease(
		*this,pScene,FireRenderPreflightAuthorization::Render);
	IRasterizer* delegate = RetainDelegate();
	try {
		if( delegate ) delegate->RasterizeScene(pScene,pRect,pRasterSequence);
	}
	catch( ... ) {
		safe_release(delegate);
		throw;
	}
	safe_release(delegate);
	ValidateFireOutputLeaseState();
}

void AutoRasterizer::RasterizeScenePrepared(
	const IScene& scene, const Scalar nominal,
	IRenderPreparationController& controller, const Rect* rect,
	IRasterizeSequence* sequence ) const
{
	EnsureResolved(&scene);
	SyncDelegateFrameStore();
	IRasterizer* delegate = RetainDelegate();
	try { if( delegate ) delegate->RasterizeScenePrepared(
		scene,nominal,controller,rect,sequence); }
	catch( ... ) { safe_release(delegate); throw; }
	safe_release(delegate);
}

void AutoRasterizer::RasterizeSceneAnimation(
	const IScene& pScene,
	const Scalar time_start,
	const Scalar time_end,
	const unsigned int num_frames,
	const bool do_fields,
	const bool invert_fields,
	const Rect* pRect,
	const unsigned int* specificFrame,
	IRasterizeSequence* pRasterSequence
	) const
{
	EnsureResolved( &pScene );
	SyncDelegateFrameStore();
	FireOutputTopologyLease fireOutputTopologyLease(
		*this,pScene,FireRenderPreflightAuthorization::Render);
	IRasterizer* delegate = RetainDelegate();
	try {
		if( delegate ) {
			delegate->RasterizeSceneAnimation( pScene, time_start, time_end, num_frames,
				do_fields, invert_fields, pRect, specificFrame, pRasterSequence );
		}
	}
	catch( ... ) {
		safe_release(delegate);
		throw;
	}
	safe_release(delegate);
	ValidateFireOutputLeaseState();
}

void AutoRasterizer::RasterizeSceneAnimationPrepared(
	const IScene& scene, const Scalar timeStart, const Scalar timeEnd,
	const unsigned int frameCount, const bool fields, const bool invertFields,
	IRenderPreparationController& controller, const Rect* rect,
	const unsigned int* frame, IRasterizeSequence* sequence ) const
{
	EnsureResolved(&scene);
	SyncDelegateFrameStore();
	IRasterizer* delegate = RetainDelegate();
	try { if( delegate ) delegate->RasterizeSceneAnimationPrepared(scene,timeStart,timeEnd,
		frameCount,fields,invertFields,controller,rect,frame,sequence); }
	catch( ... ) { safe_release(delegate); throw; }
	safe_release(delegate);
}

bool AutoRasterizer::AuthorizeFireDelegatePreflight(
	const IScene& scene,
	const FireRenderPreflightAuthorization authorization ) const
{
	auto transaction = AcquireAutoMutationLock(mDelegateMutationMutex);
	IRasterizer* delegate = nullptr;
	{
		std::lock_guard<std::mutex> lock(outsMutex);
		delegate = mDelegate;
		if( delegate ) delegate->addref();
	}
	if( !delegate ) return false;
	bool authorized = true;
	try {
		AuthorizeInternalFireDelegate(*delegate,scene,authorization);
	}
	catch( ... ) {
		authorized = false;
	}
	safe_release(delegate);
	return authorized;
}

void AutoRasterizer::ClearFireDelegatePreflight() const
{
	auto transaction = AcquireAutoMutationLock(mDelegateMutationMutex);
	IRasterizer* delegate = nullptr;
	{
		std::lock_guard<std::mutex> lock(outsMutex);
		delegate = mDelegate;
		if( delegate ) delegate->addref();
	}
	if( delegate ) ClearInternalFireDelegateAuthorization(*delegate);
	safe_release(delegate);
}
