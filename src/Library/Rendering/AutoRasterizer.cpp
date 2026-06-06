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
#include "../RISE_API.h"
#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/IPixelFilter.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/IScenePriv.h"
#include "../Interfaces/IFilm.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILightManager.h"
#include "../Interfaces/ILightPriv.h"
#include "../Interfaces/IEnumCallback.h"
#include "../Interfaces/ISampling2D.h"
#include "../Interfaces/IRasterizerOutput.h"
#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IOptions.h"
#include "../Utilities/SMSConfig.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Color/Color_Template.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
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
	//! refractive caustics.  One early-out object enumeration (stops at
	//! the first match).  Mirrors the `MediaScan` idiom in
	//! LightSampler::Prepare (return false = stop, true = continue).
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

		ProbeCaptureOutput() : width( 0 ), height( 0 ) {}

	protected:
		virtual ~ProbeCaptureOutput() {}

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
			if( std::isfinite( x ) ) {
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
			if( std::isfinite( x ) ) { accum += x; ++counted; }
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
			if( std::isfinite( x ) ) { v.push_back( x ); }
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
				if( !std::isfinite( v ) ) { ok = false; break; }
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
	return IntegratorName( mResolved );
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
	,mResolved( AutoIntegratorChoice::Auto )
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
}

AutoRasterizer::~AutoRasterizer()
{
	// The base ~Rasterizer releases mFrameStore + the buffered outs.
	safe_release( mDelegate );
	safe_release( mCaster );
	safe_release( mSamples );
	safe_release( mFilter );
}

AutoIntegratorChoice AutoRasterizer::SelectIntegrator( const IScene* scene ) const
{
	// Tier 0 — an explicit author pin always wins and skips static
	// analysis entirely (and Phase 4's probe).
	switch( mPinned ) {
		case AutoIntegratorChoice::PT:   mResolveReason = "author pin"; return AutoIntegratorChoice::PT;
		case AutoIntegratorChoice::BDPT: mResolveReason = "author pin"; return AutoIntegratorChoice::BDPT;
		case AutoIntegratorChoice::VCM:  mResolveReason = "author pin"; return AutoIntegratorChoice::VCM;
		case AutoIntegratorChoice::Auto:
		default:                         break;   // -> Tier-1 static analysis
	}

	// Tier 1 — cheap, conservative static best-guess over the assembled
	// scene (docs/AUTO_RASTERIZER_DESIGN.md §5).  Two early-out scans give
	// the only two signals the routing keys on; everything else defaults
	// to PT, the matrix's converged-bulk winner.  `scene` is the assembled
	// scene because resolution runs at the first render-time entry (not at
	// construction); Phase 4 replaces this body with the probe.
	if( !scene ) {
		// Defensive: the render path always passes a real scene, but a
		// null here must not crash the dispatcher — fall back to PT.
		mResolveReason = "no scene (defaulted)";
		return AutoIntegratorChoice::PT;
	}

	const bool hasTransmissive = SceneHasTransmissiveMaterial( *scene );
	const bool hasPositional   = SceneHasPositionalLight( *scene );
	const bool hasEnvIBL       = ( scene->GetGlobalRadianceMap() != 0 );

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
		mResolveReason = "dielectric + positional light";
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
	mResolveReason = "no caustic/strong-indirect signal";
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
	// tau_reach is the transport-reach (mean-lum VCM/PT) gate that rejects the
	// jewel_vault over-fire; default 1.50 sits in the measured 1.12|1.88 gap
	// between the converging-dielectric class and the real refractive caustics
	// (AUTO_RASTERIZER_DESIGN.md §6.2).
	cfg.tauReach        = opt.ReadDouble( "auto_probe_tau_reach", 1.50 );
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

	if( !scene || !mSamples ) {
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
	probeSampler->SetNumSamples( cfg.spp );

	// Shrink the film for the duration of the probe; restore on every exit
	// path below.  Safe: the probe runs single-threaded inside the
	// std::call_once selection, strictly BEFORE the real render's worker
	// threads spawn (ResizeFilm's concurrency contract).
	scenePriv->ResizeFilm( probeW, probeH, origAR );

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

	for( unsigned int r = 0; r < nRenders; ++r ) {
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
		ProbeCaptureOutput* cap = new ProbeCaptureOutput();
		GlobalLog()->PrintNew( cap, __FILE__, __LINE__, "probe capture output" );
		d->AddRasterizerOutput( cap );

		// MUST be multi-threaded: the QMC sample stream is deterministic
		// (HashCombine(x,y) seed), so the per-pixel variance signal comes
		// solely from multi-threaded run-to-run non-determinism — exactly the
		// Phase-3 inter-run proxy.  The default render path is multi-threaded.
		const auto t0 = std::chrono::steady_clock::now();
		d->RasterizeScene( *scene, /*pRect*/ 0, /*pRasterSequence*/ 0 );
		const auto t1 = std::chrono::steady_clock::now();
		totalSeconds += std::chrono::duration<double>( t1 - t0 ).count();

		if( !cap->lum.empty() ) {
			frames.push_back( cap->lum );
			anyValid = true;
		}

		safe_release( cap );
		safe_release( d );
	}

	// Restore the production film dims (+ re-sync cameras) before returning,
	// so the real render proceeds at full resolution.
	scenePriv->ResizeFilm( origW, origH, origAR );
	safe_release( probeSampler );

	if( anyValid ) {
		out.valid      = true;
		out.medianLum  = MedianLuminance( frames.front() );
		out.meanLum    = MeanLuminance( frames.front() );
		out.robustMeanLum = WinsorizedMeanLuminance( frames.front(), cfg.reachWinsorPct );
		out.meanVar    = MeanPerPixelVariance( frames );
		out.rasSeconds = totalSeconds;
	}
	return out;
}

AutoIntegratorChoice AutoRasterizer::RunProbe(
	const IScene* scene, const ProbeConfig& cfg,
	bool dielectric, bool envIbl ) const
{
	char reason[256];

	// Cost instrumentation (the §6.2 sweep reads this — directly via
	// LastProbeSeconds()/LastProbeRenders() and from the summary log line).
	mLastProbeSeconds = 0.0;
	mLastProbeRenders = 0;
	auto account = [this, &cfg]( const ProbeResult& r, bool variance ) {
		mLastProbeSeconds += r.rasSeconds;
		mLastProbeRenders += variance ? cfg.varianceRenders : 1u;
	};
	auto finish = [this, &cfg]( AutoIntegratorChoice pick ) -> AutoIntegratorChoice {
		GlobalLog()->PrintEx( eLog_Event,
			"AutoRasterizer:: probe cost %.3fs over %u candidate renders (scale 1/%u, spp %u)",
			mLastProbeSeconds, mLastProbeRenders, cfg.scale, cfg.spp );
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
			// jewel_vault over-fire fix).  BOTH must hold to route VCM:
			//  (1) MEDIAN-ratio gate (firefly-robust) — VCM's TYPICAL pixel is
			//      brighter than PT's.  This is the original trigger, but at probe
			//      spp it ALSO fires on a dielectric-but-CONVERGING scene
			//      (jewel_vault): PT's hard 3+-bounce indirect is transiently
			//      under-converged so its MEDIAN pixel reads dark — even though PT
			//      reaches the SAME total energy VCM does.  Median alone cannot
			//      tell that apart from a real caustic (jewel_vault 2.5-3.1x vs
			//      glass_pavilion 2.0-2.3x — no tau separates them).
			//  (2) MEAN-ratio (transport-reach) gate — the discriminator.  A REAL
			//      refractive caustic is energy PT structurally CANNOT reach, so
			//      VCM's MEAN luminance (= total energy) far exceeds PT's; a
			//      converging scene already has PT-mean ~= VCM-mean (ratio ~1).
			//      Measured at the real probe config (scale 4, spp 4, 3 trials):
			//      over-fire {jewel_vault 0.96-1.12, crystal_garden 0.67-0.69,
			//      cloister 0.88-0.99} vs real-caustic {diamond_teapot 1.88-1.95,
			//      glass_pavilion 20-32} — a clean 1.12|1.88 gap, tau_reach=1.50.
			//      meanLum is read from the SAME single render the median gate
			//      already uses, so this gate adds NO probe render (cost
			//      unchanged).  (NB the Phase-3-guessed "PT dark-and-flailing ->
			//      high PT variance/sigma2T" signal is INVERTED at probe spp:
			//      jewel_vault is the NOISIEST PT there, not the quietest, so PT
			//      sigma2T does NOT separate — the energy-reach mean ratio does.)
			// Reach numerator is VCM's WINSORIZED mean (vcm.robustMeanLum): VCM merging
			// produces sparse fireflies that inflate its raw mean and over-fire the route
			// on non-caustic dielectric scenes (jewel_vault).  The denominator is PT's RAW
			// mean (pt.meanLum) — PT has no analogous mean-inflating pathology, and capping
			// PT's tail would WRONGLY inflate the ratio on scenes where PT legitimately
			// reaches noisy-but-real bright energy the (energy-deficient) VCM merge cannot
			// (spectral_caustic: PT's dispersive caustic is noisy yet real -> keep it whole).
			const double medRatio  = vcm.medianLum / pt.medianLum;
			const double meanRatio = ( pt.meanLum > 0.0 ) ? ( vcm.robustMeanLum / pt.meanLum ) : 0.0;
			const double rawReachR = ( pt.meanLum > 0.0 ) ? ( vcm.meanLum / pt.meanLum ) : 0.0;
			if( medRatio > cfg.tauCaustic && meanRatio > cfg.tauReach ) {
				std::snprintf( reason, sizeof(reason),
					"probe -> vcm: median %.2fx > %.2f and reach %.2fx > %.2f (raw %.2fx)",
					medRatio, cfg.tauCaustic, meanRatio, cfg.tauReach, rawReachR );
				mResolveReason = reason;
				return finish( AutoIntegratorChoice::VCM );
			}
			if( medRatio > cfg.tauCaustic ) {
				// Median fired but PT reaches the same total energy as VCM -> this
				// is NOT a real caustic (the jewel_vault class), just a slow-to-
				// converge dielectric scene.  Don't over-route VCM; fall through
				// to the general BDPT-vs-PT check below (which correctly keeps
				// jewel_vault on PT via its sigma2T win).
				GlobalLog()->PrintEx( eLog_Event,
					"AutoRasterizer:: caustic median %.2fx fired but reach %.2fx <= %.2f (raw %.2fx)"
					" (PT reaches the energy) -> not a caustic, fall through to BDPT check",
					medRatio, meanRatio, cfg.tauReach, rawReachR );
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
		const double pe = pt.meanVar   * pt.rasSeconds;
		const double be = bdpt.meanVar * bdpt.rasSeconds;
		// σ²·T(PT) / σ²·T(BDPT) > τ_bdpt -> BDPT.  When BDPT reads
		// noise-free at probe spp (be <= 0) it wins iff PT still carries
		// noise — handled without forming an (under -ffast-math, UB)
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
					"probe -> bdpt: sigma2T %.1fx > %.2f", ratio, cfg.tauBdpt );
			}
			mResolveReason = reason;
			return finish( AutoIntegratorChoice::BDPT );
		}
		std::snprintf( reason, sizeof(reason),
			"probe -> pt: sigma2T %.2fx <= %.2f", ratio, cfg.tauBdpt );
		mResolveReason = reason;
		return finish( AutoIntegratorChoice::PT );
	}

	mResolveReason = "probe -> pt (insufficient probe signal)";
	return finish( AutoIntegratorChoice::PT );
}

IRasterizer* AutoRasterizer::BuildDelegate( AutoIntegratorChoice choice ) const
{
	// The real delegate: canonical sampler + canonical FrameStore + the
	// wrapper's own denoise / guiding / adaptive configs.  This is the
	// construction the concrete chunk parsers perform, so pinning `auto`
	// to X yields the same image as a bare X_pel_rasterizer.
	return BuildDelegate( choice, mSamples, GetFrameStore(),
		mOidnDenoise, mGuiding, mAdaptive );
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
	std::call_once( mResolveOnce, [this, scene]() {
		const AutoIntegratorChoice choice = SelectIntegrator( scene );
		mResolved = choice;
		mDelegate = BuildDelegate( choice );

		if( !mDelegate ) {
			GlobalLog()->PrintEasyError(
				"AutoRasterizer:: failed to build the chosen delegate rasterizer" );
			return;
		}

		// Replay the buffered render state onto the freshly-built
		// delegate.  The base Rasterizer stored these (the file output /
		// viewport sink via AddRasterizerOutput, the progress callback)
		// BEFORE the delegate existed; the delegate is the object that
		// actually renders into them.
		{
			std::lock_guard<std::mutex> lock( outsMutex );
			for( IRasterizerOutput* ro : outs ) {
				mDelegate->AddRasterizerOutput( ro );
			}
		}
		mDelegate->SetProgressCallback( pProgressFunc );

		// Progressive config can't ride RISE_API_SetRasterizerProgressiveRendering
		// on the wrapper (that down-casts to PixelBasedRasterizerHelper, which the
		// wrapper is not) — apply it to the delegate, which IS one.
		RISE_API_SetRasterizerProgressiveRendering(
			mDelegate, mProgressive.enabled, mProgressive.samplesPerPass );

		GlobalLog()->PrintEx( eLog_Event,
			"AutoRasterizer:: [%s] integrator '%s' -> delegating to '%s' (%s)",
			mSpectral ? "spectral" : "pel", IntegratorName( mPinned ), IntegratorName( choice ),
			mResolveReason.empty() ? "default" : mResolveReason.c_str() );
	} );
}

void AutoRasterizer::SyncDelegateFrameStore() const
{
	if( !mDelegate ) {
		return;
	}
	FrameStore* mine = GetFrameStore();
	if( mDelegate->GetFrameStore() == mine ) {
		return;   // already in sync (the common case)
	}
	// The delegate is always an in-tree Rasterizer; push our current
	// FrameStore so a late SetFrameStore (deferred Job push when the
	// camera dims weren't known at construction, or a viewport resize)
	// reaches the object that writes pixels.  SetFrameStore balances the
	// refcount internally (addref new, release old).
	Rasterizer* r = dynamic_cast<Rasterizer*>( mDelegate );
	if( r ) {
		r->SetFrameStore( mine );
	}
}

void AutoRasterizer::AttachToScene( const IScene* pScene )
{
	EnsureResolved( pScene );
	SyncDelegateFrameStore();
	if( mDelegate ) {
		mDelegate->AttachToScene( pScene );
	}
}

void AutoRasterizer::DetachFromScene( const IScene* pScene )
{
	if( mDelegate ) {
		mDelegate->DetachFromScene( pScene );
	}
}

unsigned int AutoRasterizer::PredictTimeToRasterizeScene(
	const IScene& pScene,
	const ISampling2D& pSampling,
	unsigned int* pActualTime
	) const
{
	EnsureResolved( &pScene );
	SyncDelegateFrameStore();
	if( mDelegate ) {
		return mDelegate->PredictTimeToRasterizeScene( pScene, pSampling, pActualTime );
	}
	if( pActualTime ) {
		*pActualTime = 0;
	}
	return 0;
}

void AutoRasterizer::RasterizeScene(
	const IScene& pScene,
	const Rect* pRect,
	IRasterizeSequence* pRasterSequence
	) const
{
	EnsureResolved( &pScene );
	SyncDelegateFrameStore();
	if( mDelegate ) {
		mDelegate->RasterizeScene( pScene, pRect, pRasterSequence );
	}
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
	if( mDelegate ) {
		mDelegate->RasterizeSceneAnimation( pScene, time_start, time_end, num_frames,
			do_fields, invert_fields, pRect, specificFrame, pRasterSequence );
	}
}
