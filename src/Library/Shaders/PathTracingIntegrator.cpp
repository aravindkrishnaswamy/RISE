//////////////////////////////////////////////////////////////////////
//
//  PathTracingIntegrator.cpp - Iterative unidirectional path tracer
//
//  Ports the recursive PathTracingShaderOp logic to an iterative
//  main loop with direct intersection (no shader dispatch).
//  Shares utilities with BDPTIntegrator: LightSampler, MediumTracking,
//  PathTransportUtilities, BSSRDFSampling, RandomWalkSSS, ManifoldSolver.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 10, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PathTracingIntegrator.h"
#include "../Rendering/LuminaryManager.h"
#include "../Lights/LightSampler.h"
#include "../Utilities/IndependentSampler.h"
#include "../Utilities/BSSRDFSampling.h"
#include "../Utilities/RandomWalkSSS.h"
#include "../Utilities/MediumTracking.h"
#include "../Utilities/PathTransportUtilities.h"
#include "../Utilities/EquiangularSampler.h"
#include "../Utilities/PathVertexEval.h"
#include "../Utilities/OptimalMISAccumulator.h"
#include "../Utilities/IORStackSeeding.h"
#include "../Utilities/MISWeights.h"
#include "../Utilities/Profiling.h"
#include "../Utilities/FiniteMath.h"
#include "../Interfaces/ISubSurfaceDiffusionProfile.h"
#include "../Interfaces/IGeometry.h"		// CanBeAreaLight(): emissive non-area-light geometries (SDF) get full BSDF weight
#include "../Utilities/MediumTransport.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Intersection/RayIntersection.h"
#include "../Rendering/AOVBuffers.h"
#ifdef RISE_ENABLE_OPENPGL
#include "../Utilities/PathGuidingField.h"
#endif
// GUI render modes P2b (docs/gui/RENDER_MODES.md §3 Lighting): the shared
// clay-reflectance state SetClayOverride substitutes in -- a mid-grey
// UniformColorPainter wrapped by a LambertianBRDF/LambertianSPF pair.
#include "../Materials/LambertianBRDF.h"
#include "../Materials/LambertianSPF.h"
#include "../Painters/UniformColorPainter.h"

using namespace RISE;
using namespace RISE::Implementation;

// Shared transport utilities
using PathTransportUtilities::PowerHeuristic;
// ClampContribution is a single function template that deduces the
// value type (RISEPel for RGB, Scalar for NM/HWSS).  Callers drop the
// historical `NM` suffix and let argument deduction pick the type.
using PathTransportUtilities::ClampContribution;
using PathTransportUtilities::PropagateBounceLimits;

// Phase 2b templatization: compile-time RGB-vs-spectral dispatch tags.
using RISE::SpectralDispatch::PelTag;
using RISE::SpectralDispatch::NMTag;
using RISE::SpectralDispatch::SpectralValueTraits;

namespace
{
	inline unsigned int EffectivePathTracingMaxDepth( const RuntimeContext& rc,
	                                                  unsigned int configured )
	{
		return rc.hasPathTracingVariantConfig ? rc.pathTracingMaxDepth : configured;
	}

	inline bool EffectivePathTracingIndirectOnly( const RuntimeContext& rc,
	                                              bool configured )
	{
		return rc.hasPathTracingVariantConfig ? rc.pathTracingIndirectOnly : configured;
	}

	inline bool EffectivePathTracingClayOverride( const RuntimeContext& rc,
	                                             bool configured )
	{
		return rc.hasPathTracingVariantConfig ? rc.pathTracingClayOverride : configured;
	}
}

// RR defaults are now in StabilityConfig.  These are kept as
// documentation of the defaults but not used directly.
// static const unsigned int PT_RR_MIN_DEPTH = 3;
// static const Scalar PT_RR_THRESHOLD = 0.05;

// SMS-DIAG (temporary): per-process counters used during the two-stage
// SMS investigation (see docs/SMS_TWO_STAGE_SOLVER.md).  Three thread-
// safe counters plus two summed-luminance accumulators.  Dumped to
// stderr at process exit.  Disabled-by-default by guarding both the
// declarations and the increment sites under SMS_DIAG_ENABLED — flip
// to 1 to re-activate when re-investigating SMS energy ratios.
#define SMS_DIAG_ENABLED 0
#if SMS_DIAG_ENABLED
namespace {
	std::atomic<uint64_t> g_smsDiag_evals{0};
	std::atomic<uint64_t> g_smsDiag_valid{0};
	std::atomic<uint64_t> g_smsDiag_emissionSuppressed{0};
	constexpr double kSMSDiag_LumScale = 1.0e6;
	std::atomic<uint64_t> g_smsDiag_sumSmsLumX{0};
	std::atomic<uint64_t> g_smsDiag_sumSuppLumX{0};

	inline void SMSDiag_AddLum( std::atomic<uint64_t>& acc, double lum ) {
		if( lum <= 0 || !RISE::IsFiniteDouble( lum ) ) return;
		const uint64_t fixed = static_cast<uint64_t>( lum * kSMSDiag_LumScale );
		acc.fetch_add( fixed, std::memory_order_relaxed );
	}

	struct SMSDiagAtExitInstaller {
		SMSDiagAtExitInstaller() {
			std::atexit([](){
				const uint64_t e = g_smsDiag_evals.load();
				const uint64_t v = g_smsDiag_valid.load();
				const uint64_t s = g_smsDiag_emissionSuppressed.load();
				const double lumSms  = double( g_smsDiag_sumSmsLumX.load() ) / kSMSDiag_LumScale;
				const double lumSupp = double( g_smsDiag_sumSuppLumX.load() ) / kSMSDiag_LumScale;
				std::fprintf( stderr,
					"[SMS-DIAG] sms_evals=%llu sms_valid=%llu emission_suppressed=%llu",
					(unsigned long long)e, (unsigned long long)v, (unsigned long long)s );
				if( e > 0 ) std::fprintf( stderr, "  valid/evals=%.4f", double(v)/double(e) );
				std::fprintf( stderr,
					"  ΣL_sms=%.4e  ΣL_supp=%.4e",
					lumSms, lumSupp );
				if( lumSupp > 0 ) std::fprintf( stderr, "  ΣL_sms/ΣL_supp=%.4f", lumSms / lumSupp );
				std::fprintf( stderr, "\n" );
			});
		}
	};
	SMSDiagAtExitInstaller g_smsDiag_installer;
}
#endif

//////////////////////////////////////////////////////////////////////
// BSSRDF entry point adapters — shared via BSSRDFEntryAdapters.h
//////////////////////////////////////////////////////////////////////

#include "BSSRDFEntryAdapters.h"
#include "../Utilities/FireflyTrace.h"
using RISE::BSSRDFAdapters::BSSRDFEntryBSDF;
using RISE::BSSRDFAdapters::RandomWalkEntryBSDF;
using RISE::BSSRDFAdapters::BSSRDFEntryMaterial;

namespace
{
	//
	// Volume distance sampling with optional equiangular MIS for
	// positional lights.  When one or more omni/spot lights are present,
	// one-sample MIS between delta tracking and equiangular sampling
	// (Kulla & Fajardo, EGSR 2012) is used to tame the 1/r^2 singularity
	// that makes plain NEE have unbounded variance near point lights.
	// When no positional lights are present, falls back to plain delta
	// tracking (identical to the previous behavior).
	//
	// Ports the RayCaster.cpp RGB-path logic (lines 269–440) into the
	// pel path tracer so the pel rasterizer gets the same variance
	// reduction that BDPT/VCM already enjoy.
	//
	struct MediumSampleOutcome
	{
		Scalar	t;
		bool	scattered;
		Scalar	combinedPdf;			///< MIS-combined PDF (0 unless useExplicitThroughput)
		bool	useExplicitThroughput;	///< true => medWeight = Tr * sigma_s / combinedPdf
		bool	zeroContrib;			///< true => equiangular landed at zero-density; no surface fallthrough
		Scalar	noScatterPdfScale;		///< strategy-selection factor for the no-scatter outcome: 0.5 in the equiangular-MIS regime (a no-scatter outcome can only arise from the DT strategy, chosen with prob 0.5, so its true mixture probability is 0.5*pSurvival), 1.0 in the pure-DT / analog / no-positional-light regime. Consumed at the no-scatter survival sites as Tr / (noScatterPdfScale * pSurvival).
	};

	//
	// Medium distance sampling runs on an IndependentSampler (pure i.i.d.)
	// rather than the main QMC path sampler.  Rationale: the equiangular
	// branch consumes 3 random dimensions while the fallback consumes 1,
	// so threading medium decisions through the QMC sequence would shift
	// downstream stream positions per-bounce and leak structured correlation
	// into pixel estimates.  RayCaster.cpp follows the same pattern
	// (IndependentSampler mediumSampler( rc.random )).
	//
	static MediumSampleOutcome SampleDistanceWithEquiangularMIS(
		const IMedium* pMedium,
		const Ray& ray,
		const Scalar maxDist,
		const Implementation::LightSampler* pLS,
		ISampler& sampler					///< Independent medium sampler (not the path QMC sampler)
		)
	{
		MediumSampleOutcome out;
		out.t = 0;
		out.scattered = false;
		out.combinedPdf = 0;
		out.useExplicitThroughput = false;
		out.zeroContrib = false;
		out.noScatterPdfScale = 1.0;

		const bool useEquiangularMIS = (pLS && pLS->GetPositionalLightCount() > 0);
		if( !useEquiangularMIS )
		{
			out.t = pMedium->SampleDistance( ray, maxDist, sampler, out.scattered );
			return out;
		}

		// Clip equiangular range to medium AABB (unbounded global media
		// return false from GetBoundingBox and are integrated over
		// [0, maxDist]).
		Scalar eqTNear = 0;
		Scalar eqTFar = maxDist;
		{
			Point3 bbMin, bbMax;
			if( pMedium->GetBoundingBox( bbMin, bbMax ) )
			{
				Scalar tEntry = 0, tExit = maxDist;
				const Scalar invX = (fabs(ray.Dir().x) > 1e-20) ? 1.0 / ray.Dir().x : 0;
				const Scalar invY = (fabs(ray.Dir().y) > 1e-20) ? 1.0 / ray.Dir().y : 0;
				const Scalar invZ = (fabs(ray.Dir().z) > 1e-20) ? 1.0 / ray.Dir().z : 0;

				bool aabbHit = true;
				if( invX != 0 ) {
					Scalar t0 = (bbMin.x - ray.origin.x) * invX;
					Scalar t1 = (bbMax.x - ray.origin.x) * invX;
					if( t0 > t1 ) { const Scalar tmp = t0; t0 = t1; t1 = tmp; }
					tEntry = fmax( tEntry, t0 );
					tExit = fmin( tExit, t1 );
				} else if( ray.origin.x < bbMin.x || ray.origin.x > bbMax.x ) {
					aabbHit = false;
				}
				if( aabbHit && invY != 0 ) {
					Scalar t0 = (bbMin.y - ray.origin.y) * invY;
					Scalar t1 = (bbMax.y - ray.origin.y) * invY;
					if( t0 > t1 ) { const Scalar tmp = t0; t0 = t1; t1 = tmp; }
					tEntry = fmax( tEntry, t0 );
					tExit = fmin( tExit, t1 );
				} else if( ray.origin.y < bbMin.y || ray.origin.y > bbMax.y ) {
					aabbHit = false;
				}
				if( aabbHit && invZ != 0 ) {
					Scalar t0 = (bbMin.z - ray.origin.z) * invZ;
					Scalar t1 = (bbMax.z - ray.origin.z) * invZ;
					if( t0 > t1 ) { const Scalar tmp = t0; t0 = t1; t1 = tmp; }
					tEntry = fmax( tEntry, t0 );
					tExit = fmin( tExit, t1 );
				} else if( ray.origin.z < bbMin.z || ray.origin.z > bbMax.z ) {
					aabbHit = false;
				}

				if( aabbHit && tEntry < tExit )
				{
					eqTNear = fmax( 0.0, tEntry );
					eqTFar = fmin( maxDist, tExit );
				}
			}
		}

		if( eqTFar <= eqTNear )
		{
			// Medium AABB does not intersect the ray segment — plain delta tracking.
			out.t = pMedium->SampleDistance( ray, maxDist, sampler, out.scattered );
			return out;
		}

		// Select one positional light proportional to exitance.
		const unsigned int nPosLights = pLS->GetPositionalLightCount();
		const Scalar totalPosExitance = pLS->GetPositionalLightTotalExitance();
		unsigned int selectedLight = 0;
		{
			const Scalar xiLight = sampler.Get1D();
			Scalar cumulative = 0;
			for( unsigned int i = 0; i < nPosLights; i++ )
			{
				cumulative += pLS->GetPositionalLightExitance( i ) / totalPosExitance;
				if( xiLight <= cumulative ) { selectedLight = i; break; }
			}
		}
		const Point3& lightPos = pLS->GetPositionalLightPosition( selectedLight );

		const Scalar xiStrategy = sampler.Get1D();

		if( xiStrategy < 0.5 )
		{
			// Delta tracking strategy with PDF.
			IMedium::DistanceSample ds = pMedium->SampleDistanceWithPdf(
				ray, maxDist, sampler );
			out.t = ds.t;
			out.scattered = ds.scattered;

			if( out.scattered )
			{
				const Scalar pdf_dt = pMedium->EvalDistancePdf( ray, out.t, true, maxDist );
				Scalar pdf_eq = 0;
				for( unsigned int i = 0; i < nPosLights; i++ )
				{
					const Scalar pSel = pLS->GetPositionalLightExitance( i ) / totalPosExitance;
					pdf_eq += pSel * EquiangularSampling::Pdf(
						ray, pLS->GetPositionalLightPosition( i ),
						eqTNear, eqTFar, out.t );
				}
				out.combinedPdf = 0.5 * pdf_dt + 0.5 * pdf_eq;
				out.useExplicitThroughput = true;
			}
			else
			{
				// No-scatter outcome under equiangular MIS.  Equiangular ONLY
				// proposes scatter events, so a no-scatter outcome can arise
				// ONLY from this delta-tracking strategy, which is chosen with
				// prob 0.5.  Its true mixture probability is 0.5*pSurvival, so
				// the no-scatter survival sites must divide by that extra 0.5.
				out.noScatterPdfScale = 0.5;
			}
		}
		else
		{
			// Equiangular strategy: sample distance toward the selected light.
			// Unlike delta tracking, equiangular ONLY proposes scatter events.
			EquiangularSampling::Sample eqSample =
				EquiangularSampling::SampleDistance(
					ray, lightPos, eqTNear, eqTFar, sampler.Get1D() );
			out.t = eqSample.t;

			if( out.t > eqTNear && out.t < maxDist )
			{
				const Point3 eqPt = ray.PointAtLength( out.t );
				const MediumCoefficients eqCoeff = pMedium->GetCoefficients( eqPt );

				if( ColorMath::MaxValue( eqCoeff.sigma_t ) > 0 )
				{
					out.scattered = true;
					const Scalar pdf_dt = pMedium->EvalDistancePdf( ray, out.t, true, maxDist );
					Scalar pdf_eq = 0;
					for( unsigned int i = 0; i < nPosLights; i++ )
					{
						const Scalar pSel = pLS->GetPositionalLightExitance( i ) / totalPosExitance;
						pdf_eq += pSel * EquiangularSampling::Pdf(
							ray, pLS->GetPositionalLightPosition( i ),
							eqTNear, eqTFar, out.t );
					}
					out.combinedPdf = 0.5 * pdf_dt + 0.5 * pdf_eq;
					out.useExplicitThroughput = true;
				}
				else
				{
					// Zero-density scatter proposal — zero weight, not a transmission.
					out.zeroContrib = true;
				}
			}
			else
			{
				// Sample outside medium range — zero contribution.
				out.zeroContrib = true;
			}
		}

		return out;
	}

	//
	// Spectral (single-wavelength) variant of SampleDistanceWithEquiangularMIS.
	// Structure identical to the RGB version; uses NM medium calls.
	// The combinedPdf is in distance measure — the caller multiplies
	// Tr_w * sigma_s_w (per-wavelength) by 1/combinedPdf to form the
	// scatter-event throughput.  HWSS callers reuse the same hero-driven
	// combinedPdf across all tracked wavelengths (one-sample MIS).
	//
	static MediumSampleOutcome SampleDistanceWithEquiangularMIS_NM(
		const IMedium* pMedium,
		const Ray& ray,
		const Scalar maxDist,
		const Scalar nm,
		const Implementation::LightSampler* pLS,
		ISampler& sampler
		)
	{
		MediumSampleOutcome out;
		out.t = 0;
		out.scattered = false;
		out.combinedPdf = 0;
		out.useExplicitThroughput = false;
		out.zeroContrib = false;
		out.noScatterPdfScale = 1.0;

		const bool useEquiangularMIS = (pLS && pLS->GetPositionalLightCount() > 0);
		if( !useEquiangularMIS )
		{
			out.t = pMedium->SampleDistanceNM( ray, maxDist, nm, sampler, out.scattered );
			return out;
		}

		Scalar eqTNear = 0;
		Scalar eqTFar = maxDist;
		{
			Point3 bbMin, bbMax;
			if( pMedium->GetBoundingBox( bbMin, bbMax ) )
			{
				Scalar tEntry = 0, tExit = maxDist;
				const Scalar invX = (fabs(ray.Dir().x) > 1e-20) ? 1.0 / ray.Dir().x : 0;
				const Scalar invY = (fabs(ray.Dir().y) > 1e-20) ? 1.0 / ray.Dir().y : 0;
				const Scalar invZ = (fabs(ray.Dir().z) > 1e-20) ? 1.0 / ray.Dir().z : 0;

				bool aabbHit = true;
				if( invX != 0 ) {
					Scalar t0 = (bbMin.x - ray.origin.x) * invX;
					Scalar t1 = (bbMax.x - ray.origin.x) * invX;
					if( t0 > t1 ) { const Scalar tmp = t0; t0 = t1; t1 = tmp; }
					tEntry = fmax( tEntry, t0 );
					tExit = fmin( tExit, t1 );
				} else if( ray.origin.x < bbMin.x || ray.origin.x > bbMax.x ) {
					aabbHit = false;
				}
				if( aabbHit && invY != 0 ) {
					Scalar t0 = (bbMin.y - ray.origin.y) * invY;
					Scalar t1 = (bbMax.y - ray.origin.y) * invY;
					if( t0 > t1 ) { const Scalar tmp = t0; t0 = t1; t1 = tmp; }
					tEntry = fmax( tEntry, t0 );
					tExit = fmin( tExit, t1 );
				} else if( ray.origin.y < bbMin.y || ray.origin.y > bbMax.y ) {
					aabbHit = false;
				}
				if( aabbHit && invZ != 0 ) {
					Scalar t0 = (bbMin.z - ray.origin.z) * invZ;
					Scalar t1 = (bbMax.z - ray.origin.z) * invZ;
					if( t0 > t1 ) { const Scalar tmp = t0; t0 = t1; t1 = tmp; }
					tEntry = fmax( tEntry, t0 );
					tExit = fmin( tExit, t1 );
				} else if( ray.origin.z < bbMin.z || ray.origin.z > bbMax.z ) {
					aabbHit = false;
				}

				if( aabbHit && tEntry < tExit )
				{
					eqTNear = fmax( 0.0, tEntry );
					eqTFar = fmin( maxDist, tExit );
				}
			}
		}

		if( eqTFar <= eqTNear )
		{
			out.t = pMedium->SampleDistanceNM( ray, maxDist, nm, sampler, out.scattered );
			return out;
		}

		const unsigned int nPosLights = pLS->GetPositionalLightCount();
		const Scalar totalPosExitance = pLS->GetPositionalLightTotalExitance();
		unsigned int selectedLight = 0;
		{
			const Scalar xiLight = sampler.Get1D();
			Scalar cumulative = 0;
			for( unsigned int i = 0; i < nPosLights; i++ )
			{
				cumulative += pLS->GetPositionalLightExitance( i ) / totalPosExitance;
				if( xiLight <= cumulative ) { selectedLight = i; break; }
			}
		}
		const Point3& lightPos = pLS->GetPositionalLightPosition( selectedLight );

		const Scalar xiStrategy = sampler.Get1D();

		if( xiStrategy < 0.5 )
		{
			IMedium::DistanceSample ds = pMedium->SampleDistanceWithPdfNM(
				ray, maxDist, nm, sampler );
			out.t = ds.t;
			out.scattered = ds.scattered;

			if( out.scattered )
			{
				const Scalar pdf_dt = pMedium->EvalDistancePdfNM(
					ray, out.t, true, maxDist, nm );
				Scalar pdf_eq = 0;
				for( unsigned int i = 0; i < nPosLights; i++ )
				{
					const Scalar pSel = pLS->GetPositionalLightExitance( i ) / totalPosExitance;
					pdf_eq += pSel * EquiangularSampling::Pdf(
						ray, pLS->GetPositionalLightPosition( i ),
						eqTNear, eqTFar, out.t );
				}
				out.combinedPdf = 0.5 * pdf_dt + 0.5 * pdf_eq;
				out.useExplicitThroughput = true;
			}
			else
			{
				// No-scatter outcome under equiangular MIS: reachable only via
				// this delta-tracking strategy (chosen with prob 0.5).  See the
				// RGB variant for the rationale — the survival sites divide by
				// the extra 0.5.
				out.noScatterPdfScale = 0.5;
			}
		}
		else
		{
			EquiangularSampling::Sample eqSample =
				EquiangularSampling::SampleDistance(
					ray, lightPos, eqTNear, eqTFar, sampler.Get1D() );
			out.t = eqSample.t;

			if( out.t > eqTNear && out.t < maxDist )
			{
				const Point3 eqPt = ray.PointAtLength( out.t );
				const MediumCoefficientsNM eqCoeff = pMedium->GetCoefficientsNM( eqPt, nm );

				if( eqCoeff.sigma_t > 0 )
				{
					out.scattered = true;
					const Scalar pdf_dt = pMedium->EvalDistancePdfNM(
						ray, out.t, true, maxDist, nm );
					Scalar pdf_eq = 0;
					for( unsigned int i = 0; i < nPosLights; i++ )
					{
						const Scalar pSel = pLS->GetPositionalLightExitance( i ) / totalPosExitance;
						pdf_eq += pSel * EquiangularSampling::Pdf(
							ray, pLS->GetPositionalLightPosition( i ),
							eqTNear, eqTFar, out.t );
					}
					out.combinedPdf = 0.5 * pdf_dt + 0.5 * pdf_eq;
					out.useExplicitThroughput = true;
				}
				else
				{
					out.zeroContrib = true;
				}
			}
			else
			{
				out.zeroContrib = true;
			}
		}

		return out;
	}

	static inline IRayCaster::RAY_STATE::RayType PathTracingRayType(
		const ScatteredRay& scat
		)
	{
		return (scat.type == ScatteredRay::eRayDiffuse && !scat.isDelta) ?
			IRayCaster::RAY_STATE::eRayDiffuse :
			IRayCaster::RAY_STATE::eRaySpecular;
	}

	static inline Scalar GuidingTrainingLuminance( const RISEPel& pel )
	{
		return 0.212671 * pel[0] + 0.715160 * pel[1] + 0.072169 * pel[2];
	}

	// Guiding-eligible scatter types — non-delta upper-hemisphere
	// reflection (diffuse or glossy).  Refraction and translucent are
	// excluded because their sampling space is the lower hemisphere
	// or a delta-transmission, neither of which the surface guiding
	// distribution covers.  Cycles enables guiding on glossy via the
	// roughness threshold; here we admit any non-delta reflection
	// lobe and let GuidingEffectiveAlpha damp glossy down by half.
	static inline bool GuidingSupportsSurfaceSampling( const ScatteredRay& scat )
	{
		if( scat.isDelta ) {
			return false;
		}
		return scat.type == ScatteredRay::eRayDiffuse ||
		       scat.type == ScatteredRay::eRayReflection;
	}

	static inline Vector3 GuidingCosineNormal( const RayIntersectionGeometric& rig )
	{
		Vector3 normal = rig.vNormal;
		if( Vector3Ops::Dot( rig.ray.Dir(), normal ) > NEARZERO ) {
			normal = -normal;
		}
		return normal;
	}

	// Per-vertex effective guiding alpha — Cycles-style: drop to zero
	// for delta or specular ray-state, half-trust glossy reflection,
	// full-trust diffuse.  No multi-lobe penalty: the caller has
	// already selected one scatter via RandomlySelect, so the chosen
	// lobe is what we sample for.  Real material roughness would be
	// an upgrade over this scatter-type proxy (would need a new
	// IMaterial::GetRoughness API).
	static inline Scalar GuidingEffectiveAlpha(
		const Scalar baseAlpha,
		const ScatteredRay& scat,
		const IRayCaster::RAY_STATE& rs
		)
	{
		if( baseAlpha <= NEARZERO ) {
			return 0;
		}
		if( scat.isDelta ) {
			return 0;
		}
		if( rs.type == IRayCaster::RAY_STATE::eRaySpecular ) {
			return 0;
		}
		switch( scat.type )
		{
			case ScatteredRay::eRayDiffuse:
				return baseAlpha;
			case ScatteredRay::eRayReflection:
				return baseAlpha * 0.5;
			default:
				return 0;
		}
	}
}

#ifdef RISE_ENABLE_OPENPGL
namespace
{
	static inline void SetPGLVec3FromRISEPel( pgl_vec3f& dst, const RISEPel& src )
	{
		dst.x = static_cast<float>( src[0] );
		dst.y = static_cast<float>( src[1] );
		dst.z = static_cast<float>( src[2] );
	}

	static inline void AddRISEPelToPGLVec3( pgl_vec3f& dst, const RISEPel& src )
	{
		dst.x += static_cast<float>( src[0] );
		dst.y += static_cast<float>( src[1] );
		dst.z += static_cast<float>( src[2] );
	}

	// One pending Adam update for the per-cell learned α.  Populated
	// at one-sample MIS guide-selection time and applied after the
	// path completes so the f estimate uses the actual radiance that
	// flowed through the chosen direction (deltaResult / throughputBefore
	// · combinedPdf), not a BSDF-only proxy.  See Müller 2017 v2 / Tom94.
	struct PTIPendingGuideUpdate
	{
		uint32_t	cellId;
		Scalar		bsdfPdf;
		Scalar		guidePdf;
		Scalar		combinedPdf;
		Scalar		resultBefore;		///< lum(result) at sample time
		Scalar		throughputBefore;	///< lum(throughput) at sample time
	};

	static inline std::vector<PTIPendingGuideUpdate>& GetPTIPendingGuideUpdates()
	{
		static thread_local std::vector<PTIPendingGuideUpdate> pending;
		return pending;
	}

	struct PTIGuidingPathRecorder
	{
		PGLPathSegmentStorage storage;
		// Capacity passed to pglPathSegmentStorageReserve.  Tracked
		// here because openpgl 0.7.1's pglPathSegmentStorageNextSegment
		// has an off-by-one (`m_seg_idx + 1 <= m_max_seg_size`) that
		// performs ONE OOB write on call N+1 when the buffer was
		// reserved for N elements, BEFORE returning nullptr on call
		// N+2.  By the time the nullptr signal arrives, the heap has
		// already been corrupted.  We early-out at `numSegments + 1
		// >= reservedCapacity` so openpgl's bug never triggers (caught
		// 2026-04-29 by Application Verifier full-page-heap on
		// pt_jewel_vault.RISEscene during HQ render with path guiding
		// + dielectric refraction).
		int reservedCapacity;
		bool active;

		PTIGuidingPathRecorder() :
			storage( 0 ),
			reservedCapacity( 0 ),
			active( false )
		{
		}

		~PTIGuidingPathRecorder()
		{
			if( storage ) {
				pglReleasePathSegmentStorage( storage );
				storage = 0;
			}
		}

		void Begin()
		{
			// Reserve well above any plausible path complexity so the
			// per-pixel hot path never has to grow.  The dominant
			// contributors are: per-hit segments up to the integrator's
			// recursion depth, plus volume-scatter segments along each
			// participating-medium leg, plus one optional background
			// segment.  256 covers max_depth values comfortably even
			// when SMS / dispersion / heavy refraction inflate the
			// effective vertex count.
			static const size_t kReservedSegments = 256;

			if( !storage ) {
				storage = pglNewPathSegmentStorage();
				if( storage ) {
					pglPathSegmentStorageReserve( storage, kReservedSegments );
					reservedCapacity = static_cast<int>( kReservedSegments );
				}
			}

			if( storage ) {
				pglPathSegmentStorageClear( storage );
				active = true;
			} else {
				active = false;
			}
		}

		void End( PathGuidingField* field )
		{
			if( active && field && storage && pglPathSegmentGetNumSegments( storage ) > 0 ) {
				field->AddPathSegments( storage, false, false, true );
			}
			active = false;
		}
	};

	struct PTIGuidingPathScope
	{
		PTIGuidingPathRecorder* recorder;
		PathGuidingField* field;
		bool isRoot;

		PTIGuidingPathScope(
			PTIGuidingPathRecorder* recorder_,
			PathGuidingField* field_,
			const bool isRoot_
			) :
			recorder( recorder_ ),
			field( field_ ),
			isRoot( isRoot_ )
		{
		}

		~PTIGuidingPathScope()
		{
			if( isRoot && recorder ) {
				recorder->End( field );
			}
		}
	};

	static inline PTIGuidingPathRecorder& GetPTIGuidingPathRecorder()
	{
		static thread_local PTIGuidingPathRecorder recorder;
		return recorder;
	}

	// True when one more pglPathSegmentStorageNextSegment() call would
	// trigger openpgl 0.7.1's off-by-one OOB write.  See the comment
	// on PTIGuidingPathRecorder::reservedCapacity.
	static inline bool PTIGuidingAtCapacity(
		const PTIGuidingPathRecorder& recorder
		)
	{
		if( recorder.reservedCapacity <= 0 ) {
			return false;
		}
		return pglPathSegmentGetNumSegments( recorder.storage ) + 1 >= recorder.reservedCapacity;
	}

	static inline PGLPathSegmentData* BeginPTIGuidingSegment(
		PTIGuidingPathRecorder& recorder,
		const RayIntersectionGeometric& rig
		)
	{
		if( !recorder.active || !recorder.storage ) {
			return 0;
		}
		if( PTIGuidingAtCapacity( recorder ) ) {
			return 0;
		}

		PGLPathSegmentData* segment = pglPathSegmentStorageNextSegment( recorder.storage );
		if( !segment ) {
			return 0;
		}

		pglPoint3f( segment->position,
			static_cast<float>( rig.ptIntersection.x ),
			static_cast<float>( rig.ptIntersection.y ),
			static_cast<float>( rig.ptIntersection.z ) );

		pglVec3f( segment->directionOut,
			static_cast<float>( -rig.ray.Dir().x ),
			static_cast<float>( -rig.ray.Dir().y ),
			static_cast<float>( -rig.ray.Dir().z ) );

		const Vector3 normal = GuidingCosineNormal( rig );
		pglVec3f( segment->normal,
			static_cast<float>( normal.x ),
			static_cast<float>( normal.y ),
			static_cast<float>( normal.z ) );

		pglVec3f( segment->directionIn, 0.0f, 0.0f, 0.0f );
		segment->volumeScatter = false;
		segment->pdfDirectionIn = 0.0f;
		segment->isDelta = false;
		pglVec3f( segment->scatteringWeight, 0.0f, 0.0f, 0.0f );
		pglVec3f( segment->transmittanceWeight, 1.0f, 1.0f, 1.0f );
		pglVec3f( segment->directContribution, 0.0f, 0.0f, 0.0f );
		segment->miWeight = 1.0f;
		pglVec3f( segment->scatteredContribution, 0.0f, 0.0f, 0.0f );
		segment->russianRouletteSurvivalProbability = 1.0f;
		segment->eta = 1.0f;
		segment->roughness = 1.0f;
		segment->regionPtr = 0;

		return segment;
	}

	static inline void SetPTIGuidingDirectContribution(
		PGLPathSegmentData* segment,
		const RISEPel& contribution,
		const Scalar miWeight
		)
	{
		if( !segment ) {
			return;
		}
		SetPGLVec3FromRISEPel( segment->directContribution, contribution );
		segment->miWeight = static_cast<float>( miWeight );
	}

	static inline void AddPTIGuidingScatteredContribution(
		PGLPathSegmentData* segment,
		const RISEPel& contribution
		)
	{
		if( !segment ) {
			return;
		}
		AddRISEPelToPGLVec3( segment->scatteredContribution, contribution );
	}

	static inline void SetPTIGuidingContinuation(
		PGLPathSegmentData* segment,
		const Vector3& direction,
		const Scalar pdf,
		const RISEPel& scatteringWeight,
		const bool isDelta,
		const Scalar rrSurvivalProb,
		const Scalar eta,
		const Scalar roughness
		)
	{
		if( !segment ) {
			return;
		}

		pglVec3f( segment->directionIn,
			static_cast<float>( direction.x ),
			static_cast<float>( direction.y ),
			static_cast<float>( direction.z ) );
		segment->pdfDirectionIn = static_cast<float>( pdf );
		SetPGLVec3FromRISEPel( segment->scatteringWeight, scatteringWeight );
		segment->isDelta = isDelta;
		segment->roughness = static_cast<float>( roughness );
		segment->eta = static_cast<float>( eta > NEARZERO ? eta : 1.0 );
		segment->russianRouletteSurvivalProbability =
			static_cast<float>( rrSurvivalProb > 0 ? rrSurvivalProb : 1.0 );
	}

	static inline PGLPathSegmentData* BeginPTIGuidingVolumeSegment(
		PTIGuidingPathRecorder& recorder,
		const Point3& scatterPt,
		const Vector3& wo
		)
	{
		if( !recorder.active || !recorder.storage ) {
			return 0;
		}
		if( PTIGuidingAtCapacity( recorder ) ) {
			return 0;
		}

		PGLPathSegmentData* segment = pglPathSegmentStorageNextSegment( recorder.storage );
		if( !segment ) {
			return 0;
		}

		pglPoint3f( segment->position,
			static_cast<float>( scatterPt.x ),
			static_cast<float>( scatterPt.y ),
			static_cast<float>( scatterPt.z ) );

		pglVec3f( segment->directionOut,
			static_cast<float>( -wo.x ),
			static_cast<float>( -wo.y ),
			static_cast<float>( -wo.z ) );

		// Volumes have no surface normal; OpenPGL needs SOME unit vector
		// here (it's used to orient the cosine product, which is gated by
		// volumeScatter=true and shouldn't matter for medium events).
		pglVec3f( segment->normal, 0.0f, 0.0f, 1.0f );

		pglVec3f( segment->directionIn, 0.0f, 0.0f, 0.0f );
		segment->volumeScatter = true;
		segment->pdfDirectionIn = 0.0f;
		segment->isDelta = false;
		pglVec3f( segment->scatteringWeight, 0.0f, 0.0f, 0.0f );
		pglVec3f( segment->transmittanceWeight, 1.0f, 1.0f, 1.0f );
		pglVec3f( segment->directContribution, 0.0f, 0.0f, 0.0f );
		segment->miWeight = 1.0f;
		pglVec3f( segment->scatteredContribution, 0.0f, 0.0f, 0.0f );
		segment->russianRouletteSurvivalProbability = 1.0f;
		segment->eta = 1.0f;
		segment->roughness = 1.0f;
		segment->regionPtr = 0;

		return segment;
	}

	// Record an escaping ray's environment radiance as a terminal
	// "background" segment for the guiding field.
	//
	// CONVENTION (must match SetPTIGuidingDirectContribution, the surface
	// emission recorder): `directContribution` carries the RAW, UNWEIGHTED
	// radiance and `miWeight` carries the MIS weight that would be applied
	// to it.  OpenPGL's own documentation for PGLPathSegmentData spells this
	// out -- "The MIS weight which would be applied to directContribution
	// (e.g., miWeight = bsdfPDF^2/(bsdfPDF^2+neePDF^2))" -- and the field
	// trains on L, using miWeight only to reconstruct the estimator.
	// Storing an already-weighted value here with miWeight = 1.0 (as this
	// helper did when its only caller was unreachable) makes the field learn
	// L * w instead of L, systematically under-training the bright env
	// directions that env-NEE also samples -- exactly the directions guiding
	// most needs to know about.
	//
	// NOTE, honestly: RISE has no guiding regression test that exercises an
	// environment-lit scene, so this correction is argued from the OpenPGL
	// contract and the sibling emission site rather than measured.  A
	// guiding-vs-env test is out of scope here.
	static inline void AddPTIGuidingBackgroundSegment(
		PTIGuidingPathRecorder& recorder,
		const Ray& ray,
		const RISEPel& radiance,
		const Scalar miWeight
		)
	{
		if( !recorder.active || !recorder.storage ) {
			return;
		}
		if( PTIGuidingAtCapacity( recorder ) ) {
			return;
		}

		PGLPathSegmentData* segment = pglPathSegmentStorageNextSegment( recorder.storage );
		if( !segment ) {
			return;
		}

		const Point3 farPoint(
			ray.origin.x + ray.Dir().x * 1.0e6,
			ray.origin.y + ray.Dir().y * 1.0e6,
			ray.origin.z + ray.Dir().z * 1.0e6 );

		pglPoint3f( segment->position,
			static_cast<float>( farPoint.x ),
			static_cast<float>( farPoint.y ),
			static_cast<float>( farPoint.z ) );
		pglVec3f( segment->directionOut,
			static_cast<float>( -ray.Dir().x ),
			static_cast<float>( -ray.Dir().y ),
			static_cast<float>( -ray.Dir().z ) );
		pglVec3f( segment->normal, 0.0f, 0.0f, 1.0f );
		pglVec3f( segment->directionIn, 0.0f, 0.0f, 0.0f );
		segment->volumeScatter = false;
		segment->pdfDirectionIn = 0.0f;
		segment->isDelta = false;
		pglVec3f( segment->scatteringWeight, 0.0f, 0.0f, 0.0f );
		pglVec3f( segment->transmittanceWeight, 1.0f, 1.0f, 1.0f );
		SetPGLVec3FromRISEPel( segment->directContribution, radiance );
		segment->miWeight = static_cast<float>( miWeight );
		pglVec3f( segment->scatteredContribution, 0.0f, 0.0f, 0.0f );
		segment->russianRouletteSurvivalProbability = 1.0f;
		segment->eta = 1.0f;
		segment->roughness = 1.0f;
		segment->regionPtr = 0;
	}
}
#endif // RISE_ENABLE_OPENPGL


//////////////////////////////////////////////////////////////////////
// Phase 2b tag-dispatch helpers (Pre-Phase-1 Piece 3)
//
// Collapse the Pel (RISEPel) and NM (Scalar + nm) variants of the
// path-tracing inner loop into templated bodies.  Each helper forwards
// at compile time to the matching member of the existing dual-signature
// API (GetCoefficients/GetCoefficientsNM, EvalTransmittance/
// EvalTransmittanceNM, EvaluateInScattering/EvaluateInScatteringNM,
// GetRadiance/GetRadianceNM, ...) selected on the tag.  No new logic;
// mirrors the VCMIntegrator Phase-2a pattern.  HWSS is NOT a tag — it is
// the hero-driven bundle handled by IntegrateRayHWSS / IntegrateFromHitHWSS
// (see SpectralValueTraits.h header comment).
//////////////////////////////////////////////////////////////////////
namespace
{
	// Contribution-gate magnitude.  Pel -> max channel; Scalar -> the
	// value itself (NO fabs, preserving the signed `<= 0` / `> 0` skip
	// semantics the NM path relies on).  Mirrors VCM's PositiveMagnitude.
	inline Scalar PTPositiveMagnitude( const RISEPel& v ) { return ColorMath::MaxValue( v ); }
	inline Scalar PTPositiveMagnitude( const Scalar  v ) { return v; }

	// Multiplicative identity in the value type (Pel -> (1,1,1); Scalar -> 1).
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type PTValueOne();
	template<> inline RISEPel PTValueOne<PelTag>() { return RISEPel( 1, 1, 1 ); }
	template<> inline Scalar  PTValueOne<NMTag>()  { return Scalar( 1 ); }

	// Reduce a transmittance value to the scalar used in the legacy
	// max-channel medium-throughput denominator.  Pel -> min channel
	// (matches the original ColorMath::MinValue(Tr)); Scalar -> itself.
	inline Scalar PTTrReduced( const RISEPel& Tr ) { return ColorMath::MinValue( Tr ); }
	inline Scalar PTTrReduced( const Scalar  Tr ) { return Tr; }

	// value_type divided by a scalar, preserving the per-variant
	// arithmetic exactly: Pel multiplies by the reciprocal (matching the
	// original `RISEPel * (1.0/d)`); Scalar divides (matching `Scalar/d`).
	inline RISEPel PTDivByScalar( const RISEPel& v, const Scalar d ) { return v * ( Scalar( 1 ) / d ); }
	inline Scalar  PTDivByScalar( const Scalar  v, const Scalar d ) { return v / d; }

	// Analog no-scatter survival weight.  SampleDistance is an ANALOG
	// estimator: reaching the surface (or escaping) WITHOUT scattering is a
	// stochastic survival event whose probability already carries the
	// Beer-Lambert factor.  Multiplying throughput by Tr again would
	// double-count attenuation (a pure absorber would render exp(-2*sigma_a*d)).
	// The correct weight is Tr / pSurvival, where pSurvival is the
	// DETERMINISTIC no-scatter survival pdf obtained from
	// IMedium::EvalDistancePdf[NM]( ray, dist, /*scattered=*/false, dist ).
	//
	// Why the deterministic pdf and NOT MinValue(EvalTransmittance): for a
	// HomogeneousMedium the two are identical -- EvalDistancePdf(false) returns
	// exp(-sigma_t_max*d) = MinValue(Tr) (Pel) and EvalDistancePdfNM(false)
	// returns exp(-sigma_t(lambda)*d) = EvalTransmittanceNM (so the NM weight is
	// exactly 1).  For a HeterogeneousMedium, however, EvalTransmittance is a
	// STOCHASTIC ratio-tracking estimate, so MinValue(EvalTransmittance) would
	// be a random denominator (a biased ratio-of-random-estimates).
	// HeterogeneousMedium overrides EvalDistancePdf[NM] with a deterministic
	// Simpson-quadrature optical depth, so pSurvival is deterministic there too
	// and the weight Tr / pSurvival is the correct unbiased no-scatter weight.
	// The numerator stays the per-channel unbiased transmittance estimate Tr.
	//
	// Guard: divide only when pSurvival > 0 (a non-positive survival pdf means
	// "no attenuation to apply"); otherwise return the value-type multiplicative
	// identity (1).
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type PTSurvivalWeight(
		const typename SpectralValueTraits<Tag>::value_type& Tr,
		const Scalar pSurvival )
	{
		if( pSurvival > 0 ) {
			return PTDivByScalar( Tr, pSurvival );
		}
		return PTValueOne<Tag>();
	}

	// Medium scattering coefficients reduced to what the throughput math
	// needs: the value_type scattering coefficient sigma_s, plus a scalar
	// extinction used for the gate + denominator (Pel -> max channel of
	// sigma_t, matching ColorMath::MaxValue; Scalar -> sigma_t itself).
	template<class Tag>
	struct PTMediumScatter
	{
		typename SpectralValueTraits<Tag>::value_type sigma_s;
		Scalar sigmaTReduced;
	};

	template<class Tag>
	inline PTMediumScatter<Tag> PTGetMediumScatter(
		const IMedium* pMedium, const Point3& pt, const Tag& tag );

	template<>
	inline PTMediumScatter<PelTag> PTGetMediumScatter<PelTag>(
		const IMedium* pMedium, const Point3& pt, const PelTag& )
	{
		const MediumCoefficients c = pMedium->GetCoefficients( pt );
		return PTMediumScatter<PelTag>{ c.sigma_s, ColorMath::MaxValue( c.sigma_t ) };
	}

	template<>
	inline PTMediumScatter<NMTag> PTGetMediumScatter<NMTag>(
		const IMedium* pMedium, const Point3& pt, const NMTag& tag )
	{
		const MediumCoefficientsNM c = pMedium->GetCoefficientsNM( pt, tag.nm );
		return PTMediumScatter<NMTag>{ c.sigma_s, c.sigma_t };
	}

	// Beer-Lambert transmittance along a ray segment.
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type PTEvalTransmittance(
		const IMedium* pMedium, const Ray& ray, const Scalar dist, const Tag& tag );

	template<>
	inline RISEPel PTEvalTransmittance<PelTag>(
		const IMedium* pMedium, const Ray& ray, const Scalar dist, const PelTag& )
	{ return pMedium->EvalTransmittance( ray, dist ); }

	template<>
	inline Scalar PTEvalTransmittance<NMTag>(
		const IMedium* pMedium, const Ray& ray, const Scalar dist, const NMTag& tag )
	{ return pMedium->EvalTransmittanceNM( ray, dist, tag.nm ); }

	// Deterministic no-scatter survival pdf along a ray segment: the
	// denominator of PTSurvivalWeight.  Pel routes to EvalDistancePdf, NM to
	// EvalDistancePdfNM.  For HomogeneousMedium this equals MinValue(Tr) (Pel)
	// / EvalTransmittanceNM (NM); for HeterogeneousMedium it is the deterministic
	// Simpson optical depth (NOT the stochastic EvalTransmittance).
	template<class Tag>
	inline Scalar PTEvalNoScatterSurvivalPdf(
		const IMedium* pMedium, const Ray& ray, const Scalar dist, const Tag& tag );

	template<>
	inline Scalar PTEvalNoScatterSurvivalPdf<PelTag>(
		const IMedium* pMedium, const Ray& ray, const Scalar dist, const PelTag& )
	{ return pMedium->EvalDistancePdf( ray, dist, /*scattered=*/false, dist ); }

	template<>
	inline Scalar PTEvalNoScatterSurvivalPdf<NMTag>(
		const IMedium* pMedium, const Ray& ray, const Scalar dist, const NMTag& tag )
	{ return pMedium->EvalDistancePdfNM( ray, dist, /*scattered=*/false, dist, tag.nm ); }

	// Volume distance sampling with optional equiangular MIS.
	template<class Tag>
	inline MediumSampleOutcome PTSampleMediumDistance(
		const IMedium* pMedium, const Ray& ray, const Scalar maxDist,
		const Implementation::LightSampler* pLS, ISampler& sampler, const Tag& tag );

	template<>
	inline MediumSampleOutcome PTSampleMediumDistance<PelTag>(
		const IMedium* pMedium, const Ray& ray, const Scalar maxDist,
		const Implementation::LightSampler* pLS, ISampler& sampler, const PelTag& )
	{ return SampleDistanceWithEquiangularMIS( pMedium, ray, maxDist, pLS, sampler ); }

	template<>
	inline MediumSampleOutcome PTSampleMediumDistance<NMTag>(
		const IMedium* pMedium, const Ray& ray, const Scalar maxDist,
		const Implementation::LightSampler* pLS, ISampler& sampler, const NMTag& tag )
	{ return SampleDistanceWithEquiangularMIS_NM( pMedium, ray, maxDist, tag.nm, pLS, sampler ); }

	// In-scattered radiance (NEE) at a medium scatter point.
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type PTEvaluateInScattering(
		const Point3& scatterPoint, const Vector3& wo, const IMedium* pMedium,
		const IRayCaster& caster, const Implementation::LightSampler* pLS,
		ISampler& sampler, const RasterizerState& rast, const IObject* pMediumObject,
		const Tag& tag );

	template<>
	inline RISEPel PTEvaluateInScattering<PelTag>(
		const Point3& scatterPoint, const Vector3& wo, const IMedium* pMedium,
		const IRayCaster& caster, const Implementation::LightSampler* pLS,
		ISampler& sampler, const RasterizerState& rast, const IObject* pMediumObject,
		const PelTag& )
	{ return MediumTransport::EvaluateInScattering( scatterPoint, wo, pMedium, caster, pLS, sampler, rast, pMediumObject ); }

	template<>
	inline Scalar PTEvaluateInScattering<NMTag>(
		const Point3& scatterPoint, const Vector3& wo, const IMedium* pMedium,
		const IRayCaster& caster, const Implementation::LightSampler* pLS,
		ISampler& sampler, const RasterizerState& rast, const IObject* pMediumObject,
		const NMTag& tag )
	{ return MediumTransport::EvaluateInScatteringNM( scatterPoint, wo, pMedium, tag.nm, caster, pLS, sampler, rast, pMediumObject ); }

	// Radiance-map lookup (per-object or global environment).
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type PTEvalRadianceMap(
		const IRadianceMap* pMap, const Ray& ray, const RasterizerState& rast, const Tag& tag );

	template<>
	inline RISEPel PTEvalRadianceMap<PelTag>(
		const IRadianceMap* pMap, const Ray& ray, const RasterizerState& rast, const PelTag& )
	{ return pMap->GetRadiance( ray, rast ); }

	template<>
	inline Scalar PTEvalRadianceMap<NMTag>(
		const IRadianceMap* pMap, const Ray& ray, const RasterizerState& rast, const NMTag& tag )
	{ return pMap->GetRadianceNM( ray, rast, tag.nm ); }

	//! GUI render modes P2b `light solo`: THE ENVIRONMENT IS A LIGHT.
	//!
	//! Under SoloKind::Light / SoloKind::Luminary the env-NEE strategy is
	//! switched off in LightSampler::EvaluateDirectLighting{,NM}.  Every
	//! BSDF-side env-radiance add must therefore be switched off too.  If
	//! it is not, the environment leaks into a light-solo render at a
	//! FRACTIONAL MIS weight -- roughly bsdfPdf^2/(bsdfPdf^2 + envPdf^2),
	//! power-heuristic-weighted against a partner strategy that no longer
	//! runs.  That is neither "one light" nor an honest render of two, and
	//! it breaks the partition identity solo(A) + solo(B) == all that the
	//! light-solo tests certify as the correctness property.
	//!
	//! Under SoloKind::Environment the env IS the target and stays; the
	//! mirror-image consistency holds there because env-NEE weights with
	//! pES->Pdf() alone (never cachedEnvSelectProb), which is exactly the
	//! density the BSDF-side partner below uses.
	inline bool PTSoloSuppressEnvironment( const IRayCaster& caster )
	{
		const Implementation::LightSampler* pLS = caster.GetLightSampler();
		return pLS && pLS->IsSoloActive() && !pLS->IsSoloTargetEnvironment();
	}

	// ================================================================
	// Phase 2b part 2: additional IntegrateFromHit dispatch helpers.
	// Each forwards at compile time to the existing dual-signature API,
	// reproducing the EXACT per-variant call the original Pel / NM
	// IntegrateFromHit bodies made.  See PRE_PHASE1_STATUS.md
	// "Pre-Phase-1 Piece 3 outcome (Phase 2b)".
	// ================================================================

	// Russian-roulette / importance survival magnitude.  Pel -> SIGNED
	// max channel (ColorMath::MaxValue, matching every original RR +
	// rs2.importance site); NM -> fabs.  DISTINCT from PTPositiveMagnitude
	// (NM raw, no fabs — for `<=0`/`>0` contribution gates) and from
	// PTAbsMaxMagnitude (Pel abs-max — runaway guard).  Do NOT conflate
	// the three: MaxValue is a SIGNED max, fabs is unsigned, and the
	// runaway guard takes the max of per-channel absolute values.
	inline Scalar PTSurvivalMagnitude( const RISEPel& v ) { return ColorMath::MaxValue( v ); }
	inline Scalar PTSurvivalMagnitude( const Scalar  v ) { return std::fabs( v ); }

	// Runaway-throughput guard magnitude.  Pel -> max of per-channel fabs
	// (matches the original r_max(fabs(t[0]),fabs(t[1]),fabs(t[2])), which
	// catches a path that has swung negative); NM -> fabs.
	inline Scalar PTAbsMaxMagnitude( const RISEPel& v ) {
		return r_max( r_max( std::fabs( v[0] ), std::fabs( v[1] ) ), std::fabs( v[2] ) );
	}
	inline Scalar PTAbsMaxMagnitude( const Scalar  v ) { return std::fabs( v ); }

	// Wavelength argument for the BSSRDF / random-walk-SSS samplers.
	// Pel passes 0 (the original RGB call's literal nm); NM passes tag.nm.
	inline Scalar PTTagNm( const PelTag& ) { return Scalar( 0 ); }
	inline Scalar PTTagNm( const NMTag& tag ) { return tag.nm; }

	// value_type -> RISEPel projection for guiding-contribution recording.
	// Pel -> identity; NM -> RISEPel(scalar) broadcast.  Matches the
	// AddPTIGuiding* / SetPTIGuiding* call sites (which take RISEPel).
	inline RISEPel PTGuidingPel( const RISEPel& v ) { return v; }
	inline RISEPel PTGuidingPel( const Scalar  v ) { return RISEPel( v ); }

	// Guiding luminance reduction for the env-background gate + Adam
	// pending/apply updates.  Pel -> GuidingTrainingLuminance (Rec.709
	// luma); NM -> fabs.  (The emission guiding gate instead uses
	// PTSurvivalMagnitude — Pel MaxValue — matching its original.)
	inline Scalar PTGuidingLuminance( const RISEPel& v ) { return GuidingTrainingLuminance( v ); }
	inline Scalar PTGuidingLuminance( const Scalar  v ) { return std::fabs( v ); }

	// Emitter radiance dispatch.
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type PTEvalEmittedRadiance(
		IEmitter* pEmitter, const RayIntersectionGeometric& ri,
		const Vector3& out, const Vector3& N, const Tag& tag );
	template<> inline RISEPel PTEvalEmittedRadiance<PelTag>(
		IEmitter* pEmitter, const RayIntersectionGeometric& ri,
		const Vector3& out, const Vector3& N, const PelTag& )
	{ return pEmitter->emittedRadiance( ri, out, N ); }
	template<> inline Scalar PTEvalEmittedRadiance<NMTag>(
		IEmitter* pEmitter, const RayIntersectionGeometric& ri,
		const Vector3& out, const Vector3& N, const NMTag& tag )
	{ return pEmitter->emittedRadianceNM( ri, out, N, tag.nm ); }

	// Direct-lighting (NEE) dispatch.  Covers PART2 surface NEE and the
	// BSSRDF / RW-SSS entry-point NEE (which pass an entry-BSDF + entry-
	// material).  NM inserts nm after pMaterial, matching the original.
	// DL-74: the trailing `pGuidedBlend` and `pMisIorStack` default to null
	// on the primary template's declaration only (explicit specializations
	// may not re-declare a default; callers that write
	// `PTEvaluateDirectLighting<Tag>(...)` resolve the defaults from this
	// declaration regardless of which specialization's body ends up
	// running).  `pMisIorStack` is the IOR stack the MIS-partner aggregate
	// pdf is evaluated under -- it must be the SAME stack the BSDF-sampling
	// side's `PTEvalPdfAtSurface` uses at that vertex (DL-74 P2).  The two
	// BSSRDF/RW-SSS entry NEE sites pass neither: their continuation's
	// density is the BSSRDF cosine pdf, which is neither guided nor
	// stack-dependent, so both sides already agree.
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type PTEvaluateDirectLighting(
		const Implementation::LightSampler* pLS, const RayIntersectionGeometric& ri,
		const IBSDF& brdf, const IMaterial* pMaterial, const IRayCaster& caster,
		ISampler& sampler, const IObject* pShadingObject, const IMedium* pMedium,
		bool isVolumeScatter, const IObject* pMediumObject, const Tag& tag,
		const IGuidedNEEPdfBlend* pGuidedBlend = 0,
		const IORStack* pMisIorStack = 0,
		Scalar neeTrainingScale = 1 );
	template<> inline RISEPel PTEvaluateDirectLighting<PelTag>(
		const Implementation::LightSampler* pLS, const RayIntersectionGeometric& ri,
		const IBSDF& brdf, const IMaterial* pMaterial, const IRayCaster& caster,
		ISampler& sampler, const IObject* pShadingObject, const IMedium* pMedium,
		bool isVolumeScatter, const IObject* pMediumObject, const PelTag&,
		const IGuidedNEEPdfBlend* pGuidedBlend, const IORStack* pMisIorStack,
		Scalar neeTrainingScale )
	{ return pLS->EvaluateDirectLighting( ri, brdf, pMaterial, caster, sampler, pShadingObject, pMedium, isVolumeScatter, pMediumObject, pGuidedBlend, pMisIorStack, neeTrainingScale ); }
	template<> inline Scalar PTEvaluateDirectLighting<NMTag>(
		const Implementation::LightSampler* pLS, const RayIntersectionGeometric& ri,
		const IBSDF& brdf, const IMaterial* pMaterial, const IRayCaster& caster,
		ISampler& sampler, const IObject* pShadingObject, const IMedium* pMedium,
		bool isVolumeScatter, const IObject* pMediumObject, const NMTag& tag,
		const IGuidedNEEPdfBlend* pGuidedBlend, const IORStack* pMisIorStack,
		Scalar neeTrainingScale )
	{ return pLS->EvaluateDirectLightingNM( ri, brdf, pMaterial, tag.nm, caster, sampler, pShadingObject, pMedium, isVolumeScatter, pMediumObject, pGuidedBlend, pMisIorStack, neeTrainingScale ); }

	// BSDF value at a surface (guiding RIS / one-sample MIS).
	template<class Tag>
	// DL-157 P1: `pIORStack` is the LIVE stack.  The guiding candidate
	// sites below already hand the SAME `iorStack` to `PTEvalPdfAtSurface`
	// one line away, so leaving the BSDF evaluation stackless was a drift
	// inside a single block; a stateful BSDF (`translucent_material`)
	// prices a hit by which side of the surface the walk is on.
	inline typename SpectralValueTraits<Tag>::value_type PTEvalBSDFAtSurface(
		const IBSDF* pBRDF, const Vector3& wi, const RayIntersectionGeometric& ri, const Tag& tag,
		const IORStack* pIORStack );
	template<> inline RISEPel PTEvalBSDFAtSurface<PelTag>(
		const IBSDF* pBRDF, const Vector3& wi, const RayIntersectionGeometric& ri, const PelTag&,
		const IORStack* pIORStack )
	{ return PathVertexEval::EvalBSDFAtSurface( pBRDF, wi, ri, pIORStack ); }
	template<> inline Scalar PTEvalBSDFAtSurface<NMTag>(
		const IBSDF* pBRDF, const Vector3& wi, const RayIntersectionGeometric& ri, const NMTag& tag,
		const IORStack* pIORStack )
	{ return PathVertexEval::EvalBSDFAtSurfaceNM( pBRDF, wi, ri, tag.nm, pIORStack ); }

	// Pdf at a surface (always Scalar).  Guiding RIS / one-sample MIS.
	template<class Tag>
	inline Scalar PTEvalPdfAtSurface(
		const ISPF* pSPF, const RayIntersectionGeometric& ri, const Vector3& wi,
		const IORStack& iorStack, const Tag& tag );
	template<> inline Scalar PTEvalPdfAtSurface<PelTag>(
		const ISPF* pSPF, const RayIntersectionGeometric& ri, const Vector3& wi,
		const IORStack& iorStack, const PelTag& )
	{ return PathVertexEval::EvalPdfAtSurface( pSPF, ri, wi, iorStack ); }
	template<> inline Scalar PTEvalPdfAtSurface<NMTag>(
		const ISPF* pSPF, const RayIntersectionGeometric& ri, const Vector3& wi,
		const IORStack& iorStack, const NMTag& tag )
	{ return PathVertexEval::EvalPdfAtSurfaceNM( pSPF, ri, wi, tag.nm, iorStack ); }

	// SPF scatter dispatch.
	template<class Tag>
	inline void PTScatter(
		const ISPF* pSPF, const RayIntersectionGeometric& ri, ISampler& sampler,
		ScatteredRayContainer& scattered, const IORStack& iorStack, const Tag& tag );
	template<> inline void PTScatter<PelTag>(
		const ISPF* pSPF, const RayIntersectionGeometric& ri, ISampler& sampler,
		ScatteredRayContainer& scattered, const IORStack& iorStack, const PelTag& )
	{ pSPF->Scatter( ri, sampler, scattered, iorStack ); }
	template<> inline void PTScatter<NMTag>(
		const ISPF* pSPF, const RayIntersectionGeometric& ri, ISampler& sampler,
		ScatteredRayContainer& scattered, const IORStack& iorStack, const NMTag& tag )
	{ pSPF->ScatterNM( ri, sampler, tag.nm, scattered, iorStack ); }

	// Lobe selection: Pel uses RGB-max weights (bNM=false), NM uses
	// spectral weights (bNM=true), matching the original RandomlySelect
	// so the selection distribution matches the selectProb compensation.
	template<class Tag>
	inline ScatteredRay* PTRandomlySelect( const ScatteredRayContainer& scattered, Scalar xi );
	template<> inline ScatteredRay* PTRandomlySelect<PelTag>( const ScatteredRayContainer& scattered, Scalar xi )
	{ return scattered.RandomlySelect( xi, false ); }
	template<> inline ScatteredRay* PTRandomlySelect<NMTag>( const ScatteredRayContainer& scattered, Scalar xi )
	{ return scattered.RandomlySelect( xi, true ); }

	// Scatter-ray kray in the value type (throughput multiply).
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type PTScatterKray( const ScatteredRay& pS );
	template<> inline RISEPel PTScatterKray<PelTag>( const ScatteredRay& pS ) { return pS.kray; }
	template<> inline Scalar  PTScatterKray<NMTag>( const ScatteredRay& pS ) { return pS.krayNM; }

	// Lobe selection weight (selectProb numerator/denominator terms).
	// Pel -> signed-max channel of kray (ColorMath::MaxValue); NM -> raw
	// krayNM (matches the CDF inside RandomlySelect with bNM=true).
	template<class Tag>
	inline Scalar PTScatterSelectWeight( const ScatteredRay& pS );
	template<> inline Scalar PTScatterSelectWeight<PelTag>( const ScatteredRay& pS ) { return ColorMath::MaxValue( pS.kray ); }
	template<> inline Scalar PTScatterSelectWeight<NMTag>( const ScatteredRay& pS ) { return pS.krayNM; }

	// BSSRDF entry weights (diffusion + random-walk).
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type PTBssrdfWeight( const BSSRDFSampling::SampleResult& b );
	template<> inline RISEPel PTBssrdfWeight<PelTag>( const BSSRDFSampling::SampleResult& b ) { return b.weight; }
	template<> inline Scalar  PTBssrdfWeight<NMTag>( const BSSRDFSampling::SampleResult& b ) { return b.weightNM; }

	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type PTBssrdfWeightSpatial( const BSSRDFSampling::SampleResult& b );
	template<> inline RISEPel PTBssrdfWeightSpatial<PelTag>( const BSSRDFSampling::SampleResult& b ) { return b.weightSpatial; }
	template<> inline Scalar  PTBssrdfWeightSpatial<NMTag>( const BSSRDFSampling::SampleResult& b ) { return b.weightSpatialNM; }

	// DL-72 / DL-84 (round 4): the numerator `OptimalMISAccumulator` has to
	// be trained with at a BSSRDF EXIT.
	//
	// THE ACCUMULATOR'S CONTRACT, from its own derivation (Kondapaneni 2019;
	// OptimalMISAccumulator.h "CORRECT MOMENT ESTIMATION"): technique i's
	// moment is `M_i = E_{x~p_i}[(f(x)/p_i(x))^2]`, where `f` is THE
	// INTEGRAND -- the whole per-sample contribution of the vertex-local
	// estimator, not some directional factor inside it -- and `p_i` is the
	// density that sample was drawn from.  Every other training site obeys
	// that: the surface continuation stores `scatterThroughput *
	// effectiveBsdfPdf` (the entire vertex-local `BSDF*cos`), and NEE
	// accumulates its `contrib` -- geometry factor included -- over its own
	// pdf.
	//
	// WHAT THE BSSRDF EXIT'S INTEGRAND IS.  BSSRDFSampling.h's header gives
	//   weight        = Rd * Ft(exit) * Ft(entry) / (c * pdfSurface)
	//   weightSpatial = Rd * Ft(exit) / pdfSurface                (no entry Sw)
	//   Sw            = Ft(entry) / (c * PI)      (EvaluateSwWithFresnel)
	// so `weight = weightSpatial * Sw * PI`.  The exit direction is
	// cosine-sampled, `cosinePdf = cos/PI`, and the contribution this
	// continuation adds is `weightSpatial * Sw(w) * cos(w) * L(w)` divided
	// by `cosinePdf` -- which is why the code multiplies the escaped
	// radiance by `weight`.  So
	//
	//     f(w) = weightSpatial * Sw(w) * cos(w) = weight * cosinePdf
	//
	// exactly, with `Rd`, `pdfSurface` and PI all surviving in it, and
	//     f / p = (weight * cosinePdf) / cosinePdf = weight
	// -- the throughput this sample really carries.  No division is needed
	// to form it, so unlike round 3's version there is no Rd==0 channel to
	// guard and the Pel form stays PER-CHANNEL rather than collapsing to
	// MaxValue().
	//
	// WHAT ROUND 3 TRAINED INSTEAD, and why it was wrong: `Sw * cos` alone,
	// i.e. `f` with `weightSpatial` DROPPED.  `weightSpatial` is an
	// area-measure quantity (`Rd / pdfSurface`), not an O(1) directional
	// factor, so that scaled this site's moments by an arbitrary amount and
	// skewed the alpha of every tile an SSS surface touches.  (Variance
	// only -- alpha never biases a partition-of-unity weight.)  Its NEE
	// partner at the same vertex had the identical defect, from the other
	// side: `LightSampler` accumulates the `contrib` it computes, and
	// `weightSpatial` is applied by the CALLER afterwards, so the NEE arm
	// trained `f / weightSpatial` too.  That is what
	// `EvaluateDirectLighting{,NM}`'s `neeTrainingScale` closes; the two
	// halves of this pair are only comparable to each other, and to every
	// other pair sharing the tile, when BOTH carry it.
	inline RISEPel PTBssrdfTrainedBsdfTimesCos( const RISEPel& weight, Scalar cosinePdf )
	{
		return weight * cosinePdf;
	}
	inline Scalar PTBssrdfTrainedBsdfTimesCos( Scalar weight, Scalar cosinePdf )
	{
		return weight * cosinePdf;
	}

	// CastRay continuation (BSSRDF / RW-SSS sub-path), 8-arg form with
	// IOR stack; distance is always passed null as in both originals.
	template<class Tag>
	inline void PTCastRay(
		const IRayCaster& caster, const RuntimeContext& rc, const RasterizerState& rast,
		const Ray& ray, typename SpectralValueTraits<Tag>::value_type& out,
		const IRayCaster::RAY_STATE& rs, const IRadianceMap* pRadianceMap,
		const IORStack& iorStack, const Tag& tag );
	template<> inline void PTCastRay<PelTag>(
		const IRayCaster& caster, const RuntimeContext& rc, const RasterizerState& rast,
		const Ray& ray, RISEPel& out, const IRayCaster::RAY_STATE& rs,
		const IRadianceMap* pRadianceMap, const IORStack& iorStack, const PelTag& )
	{ caster.CastRay( rc, rast, ray, out, rs, 0, pRadianceMap, iorStack ); }
	template<> inline void PTCastRay<NMTag>(
		const IRayCaster& caster, const RuntimeContext& rc, const RasterizerState& rast,
		const Ray& ray, Scalar& out, const IRayCaster::RAY_STATE& rs,
		const IRadianceMap* pRadianceMap, const IORStack& iorStack, const NMTag& tag )
	{ caster.CastRayNM( rc, rast, ray, out, rs, tag.nm, 0, pRadianceMap, iorStack ); }

	// SMS evaluation result + dispatch (NM uses per-wavelength IOR for
	// dispersion).  Unifies SMSContribution / SMSContributionNM.
	template<class Tag>
	struct PTSMSResult
	{
		typename SpectralValueTraits<Tag>::value_type contribution;
		Scalar misWeight;
		bool   valid;
	};
	template<class Tag>
	inline PTSMSResult<Tag> PTEvaluateSMS(
		ManifoldSolver* pSolver, const Point3& pos, const Vector3& geomNormal,
		const Vector3& shadingNormal, const OrthonormalBasis3D& onb,
		const IMaterial* pMaterial, const Vector3& woOutgoing, const IScene& scene,
		const IRayCaster& caster, ISampler& sampler, const Tag& tag );
	template<> inline PTSMSResult<PelTag> PTEvaluateSMS<PelTag>(
		ManifoldSolver* pSolver, const Point3& pos, const Vector3& geomNormal,
		const Vector3& shadingNormal, const OrthonormalBasis3D& onb,
		const IMaterial* pMaterial, const Vector3& woOutgoing, const IScene& scene,
		const IRayCaster& caster, ISampler& sampler, const PelTag& )
	{
		ManifoldSolver::SMSContribution sms = pSolver->EvaluateAtShadingPoint(
			pos, geomNormal, shadingNormal, onb, pMaterial, woOutgoing, scene, caster, sampler );
		return PTSMSResult<PelTag>{ sms.contribution, sms.misWeight, sms.valid };
	}
	template<> inline PTSMSResult<NMTag> PTEvaluateSMS<NMTag>(
		ManifoldSolver* pSolver, const Point3& pos, const Vector3& geomNormal,
		const Vector3& shadingNormal, const OrthonormalBasis3D& onb,
		const IMaterial* pMaterial, const Vector3& woOutgoing, const IScene& scene,
		const IRayCaster& caster, ISampler& sampler, const NMTag& tag )
	{
		ManifoldSolver::SMSContributionNM sms = pSolver->EvaluateAtShadingPointNM(
			pos, geomNormal, shadingNormal, onb, pMaterial, woOutgoing, scene, caster, sampler, tag.nm );
		return PTSMSResult<NMTag>{ sms.contribution, sms.misWeight, sms.valid };
	}

	// PART3 bsdfTimesCos VALUE (carried in the iterative state).  Pel:
	// scatterThroughput * pdf (RISEPel); NM: fabs(scatterThroughputNM) *
	// pdf (Scalar) — the NM path stores the unsigned magnitude × pdf, a
	// genuine pre-existing Pel/NM asymmetry preserved here verbatim.
	inline RISEPel PTBsdfTimesCos( const RISEPel& scatterThroughput, Scalar pdf ) { return scatterThroughput * pdf; }
	inline Scalar  PTBsdfTimesCos( const Scalar  scatterThroughput, Scalar pdf ) { return std::fabs( scatterThroughput ) * pdf; }

	// RAY_STATE.bsdfTimesCos field is always RISEPel.  Pel: identity;
	// NM: RISEPel(scalar) broadcast (matches rs.bsdfTimesCos = RISEPel(nm)).
	inline RISEPel PTRayStateBsdfTimesCos( const RISEPel& v ) { return v; }
	inline RISEPel PTRayStateBsdfTimesCos( const Scalar  v ) { return RISEPel( v ); }

	// Guiding scatter-throughput `value * cos / pdf`.  The Pel original
	// grouped it as `value * (cos/pdf)`; the NM original as `value*cos/pdf`
	// = `(value*cos)/pdf`.  Those associativities differ at the ULP level,
	// so each is reproduced exactly rather than unified.
	inline RISEPel PTMulDiv( const RISEPel& a, const Scalar b, const Scalar c ) { return a * ( b / c ); }
	inline Scalar  PTMulDiv( const Scalar  a, const Scalar b, const Scalar c ) { return a * b / c; }

	//! P1-c fix (review-p2b, `clay_lights` MIS inconsistency): a thin
	//! IMaterial adapter over the integrator's shared pClayBRDF/pClaySPF,
	//! passed to LightSampler::EvaluateDirectLighting{,NM} in place of
	//! ri.pMaterial wherever the NEE eval already substitutes the clay
	//! BRDF for the value/contribution term.  Without this, NEE evaluated
	//! `f = clayBRDF.value(...)` (numerator) while computing the MIS
	//! BSDF-sampling pdf from the AUTHORED material's Pdf() (denominator)
	//! -- a mismatched pair that makes clay_lights biased AND dependent on
	//! the hidden authored material (mirror/dielectric materials have a
	//! near-zero or delta Pdf(), which starves or floods the NEE weight
	//! for a surface that is visually identical clay).  GetBSDF()/GetSPF()
	//! return the SAME pClayBRDF/pClaySPF instances every other clay call
	//! site uses (no duplicate Lambertian pair -- see the ctor).
	//! GetEmitter() is always null: LightSampler::EvaluateDirectLighting
	//! only ever calls pMaterial->Pdf()/PdfNM() on this parameter (verified
	//! by reading every pMaterial use in both EvaluateDirectLighting and
	//! EvaluateDirectLightingNM -- LightSampler.cpp), never
	//! pMaterial->GetEmitter() -- the surface's own emission is evaluated
	//! separately (PART 1) against the REAL ri.pMaterial and is never
	//! substituted, matching SetClayOverride's documented contract.  Pdf/
	//! PdfNM are deliberately NOT overridden: IMaterial's base
	//! implementation (Materials/IMaterial.cpp) delegates to
	//! GetSPF()->Pdf(...)/PdfNM(...), i.e. pClaySPF's OWN pdf formula --
	//! the EXACT function the continuation ray is actually sampled from
	//! (pClaySPF::Scatter), so NEE's MIS weight and the BSDF-sampling
	//! strategy's density can never drift apart.
	class ClayNEEMaterial :
		public virtual IMaterial,
		public virtual Reference
	{
	public:
		ClayNEEMaterial( const IBSDF* brdf, const ISPF* spf ) :
		  pBRDF( const_cast<IBSDF*>( brdf ) ),
		  pSPF( const_cast<ISPF*>( spf ) )
		{}

		IBSDF* GetBSDF() const override { return pBRDF; }
		ISPF* GetSPF() const override { return pSPF; }
		IEmitter* GetEmitter() const override { return 0; }

	protected:
		~ClayNEEMaterial() override {}

	private:
		IBSDF* pBRDF;
		ISPF* pSPF;
	};

#ifdef RISE_ENABLE_OPENPGL
	//! DL-74 (docs/DL74_ENV_NEE_GUIDING_PARTITION.md): the ONE nominal
	//! MIS-partner density for a shading point under active path guiding.
	//!
	//! THE TWO ROLES OF A GUIDED PDF.  A guided continuation's throughput
	//! must be divided by the density the direction was ACTUALLY drawn
	//! from -- the per-lobe `GuidingEffectiveAlpha`, the per-lobe cosine
	//! convention, `combinedPdf` or `risEffectivePdf`.  Getting that wrong
	//! biases the estimator, so none of it is touched.  The MIS WEIGHT is a
	//! different job: `sum_s w_s(w) == 1` is the only property the estimator
	//! needs (see the unbiasedness note in
	//! docs/DL74_ENV_NEE_GUIDING_PARTITION.md), so the weights may be built
	//! from ANY common density, and the useful choice is one that both the
	//! BSDF-sampling side and every NEE arm can evaluate for the same
	//! direction without knowing which lobe was, or will be, selected:
	//!
	//!     p_mis(w) = alpha_nom * guide(w) + (1 - alpha_nom) * p_aggregate(w)
	//!
	//! Fixed conventions, and why each is the one that can be shared:
	//!  - `alpha_nom` is the base `rc.guidingAlpha`, scaled by the learned
	//!    per-cell sigmoid when `rc.guidingLearnedAlpha` is on (the
	//!    default) exactly as PART 3 scales it, and NOT halved for a glossy
	//!    lobe -- NEE runs before any lobe is chosen, so a per-lobe alpha is
	//!    unavailable to it by construction.
	//!  - the cosine product is applied UNCONDITIONALLY (PART 3 applied it
	//!    only for `eRayDiffuse`).  `guide(w)` has to be one function of
	//!    direction, and guide-times-cosine is the physically motivated
	//!    factorisation for surface reflection in both lobe regimes; the
	//!    guided estimator stays unbiased because its throughput divides by
	//!    the same post-product density it sampled from.
	//!  - `p_aggregate` is the material's all-lobes `Pdf()`, which is what
	//!    every NEE arm already computes and is independent of the
	//!    stochastic lobe choice.
	//!  - the mode (one-sample MIS vs RIS) does not enter at all, which is
	//!    why RIS's own residual (DL-83) closes with this row.
	//!
	//! Deliberately plain (no IReference/AddRef/Release): constructed fresh
	//! on the stack per shading point, never shared across threads or
	//! calls, unlike the heap-allocated, integrator-lifetime
	//! ClayNEEMaterial above.  Default-constructed (unconfigured) it is a
	//! pure pass-through, which is what every non-guided vertex uses.
	class PTGuidingMisPdf : public IGuidedNEEPdfBlend
	{
	public:
		PTGuidingMisPdf() : pField( 0 ), pDist( 0 ), alphaNominal( 0 ), bActive( false ) {}

		void Configure(
			Implementation::PathGuidingField* pF,
			Implementation::GuidingDistributionHandle* pD,
			Scalar aNominal )
		{
			pField = pF;
			pDist = pD;
			alphaNominal = aNominal;
			bActive = ( pF != 0 && pD != 0 );
		}

		bool IsActive() const { return bActive; }

		//! The nominal MIS-partner density at `wo`.
		//!
		//! `aggregatePdf <= 0` is NOT a special case while guiding is
		//! active (DL-74 P2-2, round-4 review).  The mixture the
		//! BSDF-sampling technique actually draws from is
		//! `alpha_nom*guide + (1-alpha_nom)*p_aggregate`, and its first
		//! term does not vanish just because the material's own pdf
		//! does: the guide can and does propose a direction outside the
		//! material's sampling support (the tilted-lobe wedge of
		//! `PTGuidingMISPartitionTest` row (h) is exactly that region).
		//! Returning 0 there told BOTH sides "the BSDF technique never
		//! generates this direction", so both took weight 1 and the
		//! wedge's energy was counted twice.  The mixture density is
		//! returned instead, which is one function of direction on both
		//! sides and partitions exactly.
		//!
		//! With guiding INACTIVE the aggregate pdf is passed straight
		//! through, so `aggregatePdf == 0` still means "no MIS partner
		//! exists" -- which is then the true statement, because the only
		//! sampler is the material itself.
		Scalar Eval( const Vector3& wo, Scalar aggregatePdf ) const
		{
			if( !bActive ) {
				return aggregatePdf;
			}
			const Scalar agg = aggregatePdf > 0 ? aggregatePdf : Scalar( 0 );
			const Scalar guidePdf = pField->Pdf( *pDist, wo );
			return PathTransportUtilities::GuidingCombinedPdf(
				alphaNominal, guidePdf, agg );
		}

		Scalar Blend( const Vector3& wo, Scalar rawPdf ) const override
		{
			return Eval( wo, rawPdf );
		}

	private:
		Implementation::PathGuidingField* pField;
		Implementation::GuidingDistributionHandle* pDist;
		Scalar alphaNominal;
		bool bActive;
	};
#endif
}

//////////////////////////////////////////////////////////////////////
// Construction / destruction
//////////////////////////////////////////////////////////////////////

// P1-a leak-fix test hook (review-p2c): see ConstructionCount()/
// DestructionCount()'s doc in the header.
std::atomic<long long> PathTracingIntegrator::sConstructionCount( 0 );
std::atomic<long long> PathTracingIntegrator::sDestructionCount( 0 );

PathTracingIntegrator::PathTracingIntegrator(
	const ManifoldSolverConfig& smsConfig,
	const StabilityConfig& stabilityCfg
	) :
  pSolver( 0 ),
  bSMSEnabled( smsConfig.enabled ),
  stabilityConfig( stabilityCfg ),
  mMaxPathDepth( 128 ),
  mIndirectOnly( false ),
  mClayOverride( false ),
  pClayPainter( 0 ),
  pClayBRDF( 0 ),
  pClaySPF( 0 ),
  pClayMaterial( 0 )
{
	if( smsConfig.enabled )
	{
		pSolver = new ManifoldSolver( smsConfig );
	}

	// GUI render modes P2b `clay_lights`: built unconditionally (cheap --
	// one painter + two thin wrapper objects) rather than lazily on first
	// SetClayOverride(true), so there is no first-use race to reason about.
	// A mid-grey (0.5,0.5,0.5) albedo reflectance -- neutral clay, not
	// pure white (would over-brighten bounce energy) or pure black (would
	// kill it).  Refcount discipline (verified against LambertianBRDF /
	// LambertianSPF's actual ctors, both of which addref their painter
	// argument): `new UniformColorPainter` starts refcount 1; the BRDF
	// wrapper's ctor addrefs it to 2; the SPF wrapper's ctor addrefs it to
	// 3.  Deliberately NOT releasing the local `pPainter` here: the third
	// reference IS `pClayPainter`'s own -- i.e. `new` is the acquisition
	// for the member, matching every other raw-pointer-member-holds-a-ref
	// idiom in this file (pSolver, etc).  The three-way symmetric release
	// in the dtor below (BRDF, then SPF, then pClayPainter) exactly
	// balances this ctor's three addrefs, so pClayPainter is never touched
	// after the object it points to is freed.
	{
		IPainter* pPainter = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );
		// review-p3 P3 fix: all three are Reference-counted and each gets
		// its own symmetric safe_release in the dtor below -- without a
		// matching PrintNew, LOG_TRACK_MEMORY prints a spurious "Specified
		// Allocation does not exist!" for each on teardown.
		GlobalLog()->PrintNew( pPainter, __FILE__, __LINE__, "clay_lights neutral painter" );
		pClayPainter = pPainter;
		pClayBRDF = new LambertianBRDF( *pPainter );
		GlobalLog()->PrintNew( pClayBRDF, __FILE__, __LINE__, "clay_lights BRDF" );
		pClaySPF  = new LambertianSPF( *pPainter );
		GlobalLog()->PrintNew( pClaySPF, __FILE__, __LINE__, "clay_lights SPF" );
	}

	// P1-c fix: the NEE-material adapter (see ClayNEEMaterial's doc above)
	// wraps pClayBRDF/pClaySPF by raw (non-owning) pointer -- it does not
	// addref them.  This is safe because all four clay members share one
	// build-once/tear-down-once lifetime scoped to this integrator: nothing
	// dereferences pClayMaterial's GetBSDF()/GetSPF() results outside of an
	// active render, and pClayBRDF/pClaySPF are never released before
	// pClayMaterial in the dtor below (in fact -- release order among the
	// four is inconsequential here specifically because none of their
	// destructors dereference each other; ClayNEEMaterial's dtor is a
	// trivial no-op).
	pClayMaterial = new ClayNEEMaterial( pClayBRDF, pClaySPF );
	// review-p3 P3 fix: same tracking-asymmetry fix as the trio above --
	// safe_release( pClayMaterial ) runs unconditionally in the dtor.
	GlobalLog()->PrintNew( pClayMaterial, __FILE__, __LINE__, "clay_lights NEE material" );

	sConstructionCount.fetch_add( 1, std::memory_order_relaxed );
}

PathTracingIntegrator::~PathTracingIntegrator()
{
	safe_release( pSolver );
	safe_release( pClayMaterial );
	safe_release( pClayBRDF );
	safe_release( pClaySPF );
	safe_release( pClayPainter );

	sDestructionCount.fetch_add( 1, std::memory_order_relaxed );
}

//////////////////////////////////////////////////////////////////////
// IntegrateFromHit — Core path tracing loop starting from a
// pre-computed surface hit.
//
// Both IntegrateRay (pure PT rasterizer) and the ShaderOp wrapper
// delegate here.  The caller provides the first intersection;
// subsequent bounces are handled iteratively within the loop.
//
// At each surface hit:
//   1. Emission (MIS weighted against NEE)
//   2. BSSRDF (disk-projection and random-walk)
//   3. NEE via LightSampler
//   4. SMS for caustics
//   5. BSDF continuation (iterative, not recursive)
//
// Medium transport and intersection are performed at the end of
// each iteration for the *next* bounce (the first hit is pre-computed).
//////////////////////////////////////////////////////////////////////

template<class Tag>
typename SpectralValueTraits<Tag>::value_type
PathTracingIntegrator::IntegrateFromHitTemplated(
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const RayIntersection& firstHit,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const IRadianceMap* pRadianceMap,
	unsigned int startDepth,
	const IORStack& initialIorStack,
	Scalar bsdfPdf,
	const typename SpectralValueTraits<Tag>::value_type& bsdfTimesCos_,
	bool considerEmission,
	Scalar importance,
	IRayCaster::RAY_STATE::RayType rayType,
	unsigned int diffuseBounces,
	unsigned int glossyBounces,
	unsigned int transmissionBounces,
	unsigned int translucentBounces,
	unsigned int volumeBounces,
	Scalar glossyFilterWidth,
	bool smsPassedThroughSpecular_initial,
	bool smsHadNonSpecularShading_initial,
	PixelAOV* pAOV,
	typename SpectralValueTraits<Tag>::value_type* pDirectResult,
	const Tag& tag,
	Scalar bsdfMisPdf_,
	Scalar castRRCompensation_
	) const
{
	using Traits = SpectralValueTraits<Tag>;
	using Value = typename Traits::value_type;

	Value result = Traits::zero();
	Value throughput = PTValueOne<Tag>();
	Value bsdfTimesCos = bsdfTimesCos_;
	// DL-74: the incoming vertex's MIS-partner density, supplied by the
	// caller (`RAY_STATE::MisPartnerPdf()` at the shader-op boundary) and
	// defaulting to `bsdfPdf` when the caller has nothing else to say.
	//
	// It is NOT safe to assume every caller enters from a non-guided
	// context -- an earlier round of this row did, and that assumption was
	// false for exactly one producer: `RayCaster`'s volume phase-scatter
	// continuation sets `bsdfPdf = effectivePdf` (the guided mixture) and
	// `bsdfMisPdf = phasePdf` (the raw density volume NEE weights against).
	// When that continuation hits an emissive surface it re-enters here
	// through `PathTracingShaderOp`, and collapsing the two back together
	// made the emitter-hit weight use the guided pdf against an NEE arm
	// that had used the raw one -- measured +44 % on the row-(g) fixture.
	Scalar bsdfMisPdf = bsdfMisPdf_ < 0 ? bsdfPdf : bsdfMisPdf_;

	RayIntersection ri( firstHit );
	Ray currentRay = ri.geometric.ray;
	IORStack iorStack = initialIorStack;
	bool needsIntersection = false;

	// Firefly tracing: assigns a monotonically increasing per-pixel sample
	// ID so the output log can be grouped by sample.  Only enabled when
	// env RISE_FFTRACE_X/Y match rast.x/y AND we're at startDepth==0
	// (top-level camera path).  FF_TRACE_ACTIVE is tag-neutral and only
	// true under the RISE_FFTRACE_* debug env (never during tests /
	// production), so the machinery is render-neutral for both tags; the
	// FF_TRACE *bodies* (which index throughput[0..2]) compile only for
	// PelTag, preserving the original NM path's complete absence of FF.
	const bool ff = FF_TRACE_ACTIVE( rast.x, rast.y ) && startDepth == 0;
	static thread_local unsigned long ffSampleId = 0;
	const unsigned long ffSample = ff ? (++ffSampleId) : 0;
	::RISE::FireflyTrace::PathScope ffPathScope( ff );
	if constexpr ( Traits::is_pel ) {
		if( ff ) {
			FF_TRACE( "=== SAMPLE %lu px(%u,%u) startDepth=%u firstHit.bHit=%d ===",
				ffSample, rast.x, rast.y, startDepth, (int)ri.geometric.bHit );
		}
	}

	const unsigned int rrMinDepth = stabilityConfig.rrMinDepth;
	const Scalar rrThreshold = stabilityConfig.rrThreshold;
	// SMS enablement — declared for both tags so the PART1 emission-
	// suppression test reads identically to the NM original (the Pel
	// original spelled the same predicate as `pSolver != 0` inline).
	const bool bSMSEnabled = ( pSolver != 0 );

	// When SMS is active, track whether the BSDF-sampled path went
	// through a specular surface.  If it did AND there was a prior
	// non-specular shading point where SMS was evaluated, the emission
	// contribution from hitting a light is suppressed because SMS
	// already accounts for those paths.  Without the non-specular
	// check, paths like camera->glass->light would be incorrectly
	// suppressed even though no SMS evaluation covered them.
	// Initialize from caller so recursive CastRay calls (e.g. via
	// SSS / BSSRDF entry, branching shader-op chains) carry the
	// suppression state from the parent call.
	bool bPassedThroughSpecular = smsPassedThroughSpecular_initial;
	bool bHadNonSpecularShading = smsHadNonSpecularShading_initial;

	const LightSampler* pLS = caster.GetLightSampler();

#ifdef RISE_ENABLE_OPENPGL
	const bool useGuidingPathSegments = rc.pGuidingField &&
		rc.pGuidingField->IsCollectingTrainingSamples();
	PTIGuidingPathRecorder* guidingRecorder = useGuidingPathSegments ?
		&GetPTIGuidingPathRecorder() : 0;
	const bool guidingRootRay = guidingRecorder != 0 && startDepth == 0;
	if( guidingRootRay ) {
		guidingRecorder->Begin();
	}
	PTIGuidingPathScope guidingPathScope( guidingRecorder, rc.pGuidingField, guidingRootRay );
#endif

	// GUI render modes P2a fix: was a hardcoded literal 128; now the
	// configurable cap (see SetMaxPathDepth's doc for the exact depth
	// accounting).  Default 128 preserves byte-identical behavior for every
	// caller that never calls the setter.
	const unsigned int maxDepth = EffectivePathTracingMaxDepth( rc, mMaxPathDepth );

	for( unsigned int depth = startDepth; depth < maxDepth; depth++ )
	{
		// Runaway-throughput guard.  PT can compound per-bounce BSDF
		// kray amplification (Ward / multi-lobe-select divides by
		// selection probability < 1) into exponential throughput
		// growth in scenes with deep-bounce recursion through glossy
		// metallic chains.  Without a cap, ~70 such bounces overflow
		// the float32 EXR archival format to +/-inf, producing pixels
		// that "never converge" no matter how many samples you throw
		// at them.  Use the absolute per-channel magnitude so a path
		// that has swung negative (rare but possible if a BSDF returns
		// a negative kray due to e.g. Kulla-Conty `1 - Eavg` precision
		// at high alpha) still terminates.  RR cannot reach in here --
		// its survival prob is gated on signed MaxValue(throughput) >=
		// rrThreshold (default 0.05), so paths that diverge negative
		// have rrProb collapse and never terminate.  Cap at 1e6: well
		// above any physically plausible throughput on a non-pathological
		// path (typical caustics peak around 1e1-1e3) but well below
		// the float32 EXR overflow ceiling (~3.4e38).
		{
			const Scalar absMax = PTAbsMaxMagnitude( throughput );
			if( !RISE::IsFiniteDouble( absMax ) || absMax > Scalar(1e6) ) {
				break;
			}
		}

		sampler.StartStream( 16 + depth );

		// DL-124: the escape/hit-through-medium survival weight crossed
		// on THIS iteration's segment (Tr / pSurvival; set below when
		// either the `!scattered && bHit` or `!scattered && !bHit`
		// medium branch fires; stays 1 -- a no-op -- when this segment
		// crosses no medium, or `needsIntersection` is false, i.e. the
		// caller-provided initial hit).  Declared here, OUTSIDE
		// `if( needsIntersection )`, so it both resets every iteration
		// AND stays in scope for PART 1's emission-hit MIS training
		// below, which runs unconditionally every iteration regardless
		// of whether this one re-intersected.  `throughput` already
		// carries the SAME factor via the ordinary multiply in those
		// branches, so `result`'s actual value is unaffected; this copy
		// exists ONLY so the optimal-MIS training (which reads
		// `bsdfTimesCos`, a value separate from `throughput`) can fold
		// in the SAME medium attenuation its NEE partner already
		// carries via `EvalShadowTransmittance` -- see
		// docs/DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md's DL-124 entry.
		Value escapeTr = PTValueOne<Tag>();

		// ============================================================
		// Intersection + medium transport (skipped for first iteration
		// — the caller provides the pre-computed hit)
		// ============================================================
		if( needsIntersection )
		{
			ri = RayIntersection( currentRay, rast );
			ri.geometric.glossyFilterWidth = glossyFilterWidth;
			scene.GetObjects()->IntersectRay( ri, true, true, false );

			bool bHit = ri.geometric.bHit;

			// Medium transport
			const IObject* pMediumObject = 0;
			const IMedium* pCurrentMedium = MediumTracking::GetCurrentMediumWithObject(
				iorStack, &scene, pMediumObject );

			if( pCurrentMedium )
			{
				const Scalar maxDist = bHit ? ri.geometric.range : RISE_INFINITY;
				IndependentSampler mediumSampler( rc.random );
				const MediumSampleOutcome mso = PTSampleMediumDistance<Tag>(
					pCurrentMedium, currentRay, maxDist, pLS, mediumSampler, tag );
				const Scalar t_m = mso.t;
				const bool scattered = mso.scattered;

				if( mso.zeroContrib )
				{
					// Equiangular strategy landed at a zero-density point or
					// outside the medium.  This is a scatter-measure sample
					// with zero weight — do not fall through to surface
					// shading.
					break;
				}

				if( scattered && volumeBounces < stabilityConfig.maxVolumeBounce )
				{
					// Volume scatter event
					const Point3 scatterPt = currentRay.PointAtLength( t_m );
					const Vector3 wo = currentRay.Dir();
					const PTMediumScatter<Tag> coeff = PTGetMediumScatter<Tag>( pCurrentMedium, scatterPt, tag );
					const Value Tr = PTEvalTransmittance<Tag>( pCurrentMedium, currentRay, t_m, tag );

					Value medWeight = Traits::zero();
					if( mso.useExplicitThroughput && mso.combinedPdf > 0 )
					{
						// Equiangular-MIS throughput: Tr * sigma_s / combinedPdf.
						medWeight = PTDivByScalar( Tr * coeff.sigma_s, mso.combinedPdf );
					}
					else if( coeff.sigmaTReduced > 0 )
					{
						// Legacy max-channel throughput (no positional lights /
						// outside medium bounds).  Per-channel equivalent:
						//   sigma_s[c] / sigma_t_max * exp((sigma_t_max - sigma_t[c]) * t)
						const Scalar Tr_scalar = PTTrReduced( Tr );
						if( Tr_scalar > 0 ) {
							medWeight = PTDivByScalar( Tr * coeff.sigma_s,
								coeff.sigmaTReduced * Tr_scalar );
						}
					}

					if( PTPositiveMagnitude( medWeight ) <= 0 ) {
						break;
					}

					throughput = throughput * medWeight;

#ifdef RISE_ENABLE_OPENPGL
					PGLPathSegmentData* volSegment =
						(guidingRecorder && guidingRecorder->active) ?
							BeginPTIGuidingVolumeSegment( *guidingRecorder, scatterPt, wo ) : 0;
#endif

					// NEE at scatter point
					if( pLS )
					{
						Value Ld = PTEvaluateInScattering<Tag>(
							scatterPt, wo, pCurrentMedium, caster, pLS,
							sampler, rast, pMediumObject, tag );
						if( PTPositiveMagnitude( Ld ) > 0 )
						{
							Value directContrib = throughput * Ld;
							directContrib = ClampContribution( directContrib,
								stabilityConfig.directClamp );
							result = result + directContrib;
#ifdef RISE_ENABLE_OPENPGL
							AddPTIGuidingScatteredContribution( volSegment, PTGuidingPel( Ld ) );
#endif
						}
					}

					// Sample phase function for continuation
					const IPhaseFunction* pPhase = pCurrentMedium->GetPhaseFunction();
					if( !pPhase ) {
						break;
					}

					Vector3 wi = pPhase->Sample( wo, sampler );
					Scalar phasePdf = pPhase->Pdf( wo, wi );
					if( phasePdf <= NEARZERO ) {
						break;
					}
					Scalar effectivePdf = phasePdf;

#ifdef RISE_ENABLE_OPENPGL
					// Volume guiding: one-sample MIS between phase function
					// and learned volume distribution.  Mirrors the surface
					// guiding path.  Falls through to pure phase sampling
					// when the field has no volume data at this position.
					if( rc.pGuidingField && rc.pGuidingField->IsTrained() &&
						rc.guidingAlpha > 0 && depth <= rc.maxGuidingDepth )
					{
						static thread_local Implementation::GuidingVolumeDistributionHandle volGuideHandle;
						if( rc.pGuidingField->InitVolumeDistribution(
								volGuideHandle, scatterPt, sampler.Get1D() ) )
						{
							const Scalar meanCosine = pPhase->GetMeanCosine();
							if( fabs( meanCosine ) > 1e-6 ) {
								rc.pGuidingField->ApplyHGProduct(
									volGuideHandle, wo, meanCosine );
							}

							const Scalar alpha = rc.guidingAlpha;
							const Scalar xiG = sampler.Get1D();
							if( PathTransportUtilities::ShouldUseGuidedSample( alpha, xiG ) )
							{
								Scalar guidePdf = 0;
								const Point2 xi2D( sampler.Get1D(), sampler.Get1D() );
								const Vector3 guidedDir =
									rc.pGuidingField->SampleVolume( volGuideHandle, xi2D, guidePdf );
								if( guidePdf > 0 )
								{
									wi = guidedDir;
									phasePdf = pPhase->Pdf( wo, wi );
									effectivePdf = PathTransportUtilities::GuidingCombinedPdf(
										alpha, guidePdf, phasePdf );
								}
							}
							else
							{
								const Scalar guidePdf =
									rc.pGuidingField->PdfVolume( volGuideHandle, wi );
								if( guidePdf > 0 ) {
									effectivePdf = PathTransportUtilities::GuidingCombinedPdf(
										alpha, guidePdf, phasePdf );
								}
							}
						}
					}
#endif

					if( effectivePdf <= NEARZERO ) {
						break;
					}

					// DL-84: pair the training moment below (recorded once this
					// vertex's continuation either re-scatters again or escapes
					// to the environment, whichever happens first) with one
					// `AccumulateCount` HERE -- at the same "one call per sample
					// attempt" granularity the main surface continuation
					// (`AccumulateCount` a few hundred lines below, gated on
					// `!skipContinuation`) and the BSSRDF exit continuation both
					// use.  Placed BEFORE Russian roulette, so an RR-terminated
					// attempt (the `break` a few lines down) is still counted --
					// `OptimalMISAccumulator.h`'s `AccumulateCount` doc: "regardless
					// of whether the sample contributed non-zero radiance".  This
					// mirrors `RayCaster.cpp`'s own two volume sites, which gate
					// the identical call on `effectivePdf > 0` (true here by the
					// check just above; kept explicit for the same reason those
					// sites keep it explicit -- symmetry under future edits).
					if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() && effectivePdf > 0 )
					{
						const_cast<OptimalMISAccumulator*>( rc.pOptimalMIS )->AccumulateCount(
							rast.x, rast.y, kTechniqueBSDF );
					}

					const Scalar phaseVal = pPhase->Evaluate( wo, wi );
					const Scalar volScatterScalar = phaseVal / effectivePdf;
					// Pel multiplies channel-wise by RISEPel(s,s,s); NM
					// multiplies by the scalar.  Both reduce throughput by
					// phaseVal/effectivePdf — kept distinct so each matches
					// its original arithmetic exactly.
					Value volScatterThroughput;
					if constexpr ( Traits::is_pel ) {
						volScatterThroughput = RISEPel(
							volScatterScalar, volScatterScalar, volScatterScalar );
					} else {
						volScatterThroughput = volScatterScalar;
					}
#ifdef RISE_ENABLE_OPENPGL
					const Value preRRVolScatterThroughput = volScatterThroughput;
#endif
					// DL-84 round 7: needed OUTSIDE the OpenPGL guard now,
					// because the optimal-MIS moment this vertex trains is
					// the REALIZED one and therefore carries `1/q`.
					Scalar volRrSurvivalProb = 1.0;
					throughput = throughput * volScatterThroughput;

					// Russian roulette on volume scatter
					{
						const PathTransportUtilities::RussianRouletteResult rr =
							PathTransportUtilities::EvaluateRussianRoulette(
								depth + volumeBounces,
								rrMinDepth, rrThreshold,
								PTSurvivalMagnitude( throughput ),
								importance,
								sampler.Get1D() );
						if( rr.terminate ) {
							break;
						}
						if( rr.survivalProb < 1.0 ) {
							// PTDivByScalar preserves the per-variant arithmetic:
							// Pel `throughput * (1/p)` (orig RGB), NM `throughput / p`
							// (orig spectral used `/=`).  Inlining `*(1/p)` for both
							// would change the NM path at the ULP level.
							throughput = PTDivByScalar( throughput, rr.survivalProb );
							volRrSurvivalProb = rr.survivalProb;
						}
					}

#ifdef RISE_ENABLE_OPENPGL
					if( volSegment ) {
						SetPTIGuidingContinuation(
							volSegment,
							wi,
							effectivePdf,
							PTGuidingPel( preRRVolScatterThroughput ),
							false,
							volRrSurvivalProb,
							1.0,
							1.0 );
					}
#endif

					currentRay = Ray( scatterPt, wi );
					// DL-74 (round-3 review): this loop's OWN volume vertex is
					// a THIRD producer of the two densities, and it was the one
					// the round-2 split missed.  It is reached only after a
					// SURFACE bounce (a camera ray's first medium interaction
					// is handled by `IntegrateRayTemplated`'s separate walk),
					// which is why "fog box + white floor" caught it and "fog
					// box" alone did not.
					//
					//  * `bsdfPdf` is the TRUE sampling density -- `effectivePdf`,
					//    the guided mixture when volume guiding fires, since the
					//    throughput above divided by exactly that.
					//  * `bsdfMisPdf` is the MIS partner, and at a volume vertex
					//    that is the RAW `phasePdf` (DL-73): the NEE call a few
					//    lines above weights through `MediumScatterMaterial::Pdf`,
					//    which forwards straight to `IPhaseFunction::Pdf`.
					//
					// Leaving `bsdfMisPdf` at the PREVIOUS vertex's value (what
					// round 2 did) made the env escape after this scatter weight
					// against the floor's BSDF pdf -- and after a camera ray,
					// against 0, i.e. FULL weight on top of an already-weighted
					// volume NEE sample.  Measured +9.6 % on
					// VolumeEnvFurnaceTest's floor-in-fog furnace.
					bsdfPdf = effectivePdf;
					bsdfMisPdf = phasePdf;
					// DL-84 (fixed): the optimal-MIS moment's numerator must be
					// the FULL vertex-local integrand at THIS vertex, matching
					// its own denominator (`bsdfPdf` above).  A phase function
					// has no separate cosine term (there is no surface normal in
					// free space -- DL-72 round 3's derivation for RayCaster.cpp's
					// sibling sites), so the volume analogue of "BSDF*cos at the
					// scatter point" is just the phase VALUE at the sampled
					// direction.  This loop already computes that value
					// explicitly a few lines above (`phaseVal = pPhase->Evaluate(
					// wo, wi)`, used for `volScatterScalar`), so it is used
					// directly here rather than leaning on "for a normalized
					// phase function this equals Pdf()" the way RayCaster.cpp's
					// sites do (they have no separate Evaluate() call at hand).
					// The surface `bsdfTimesCos` left standing here belonged to
					// the PREVIOUS vertex -- a numerator from one vertex over a
					// denominator from another -- which is why round 2
					// conservatively cleared it to zero (the `f2 > 0` gate then
					// simply skips training) rather than reuse it.  Paired with
					// the `AccumulateCount` added above.
					//
					// ROUND 7 (the realized-moment convention -- see the
					// ordinary surface continuation's `bsdfTimesCosVal` for the
					// derivation): this site applies Russian roulette to
					// `throughput` BETWEEN its `AccumulateCount` and the moment
					// the escape arm later accumulates, so the AS-CARRIED
					// numerator is `phaseVal / q`.  Dividing here is what makes
					// the trained quantity `E_pre/q` -- the same convention the
					// surface and BSSRDF continuations use -- rather than
					// `q*E_pre`.  `volRrSurvivalProb` is 1 whenever the roulette
					// did not fire, so this is a no-op on shallow paths.
					const Scalar trainedPhaseVal = phaseVal / volRrSurvivalProb;
					if constexpr ( Traits::is_pel ) {
						bsdfTimesCos = RISEPel( trainedPhaseVal, trainedPhaseVal, trainedPhaseVal );
					} else {
						bsdfTimesCos = trainedPhaseVal;
					}
					considerEmission = true;
					volumeBounces++;
					continue;  // Re-enter loop: needsIntersection is still true
				}
				else if( !scattered && bHit )
				{
					// Surface hit through medium (analog no-scatter survival).
					// SampleDistance already drew "reach the surface"; that
					// survival event carries Beer-Lambert.  Apply only the
					// per-channel weight Tr / pSurvival (deterministic no-scatter
					// survival pdf; = 1 for monochrome/NM homogeneous) so we don't
					// double-count attenuation.
					const Value Tr = PTEvalTransmittance<Tag>(
						pCurrentMedium, currentRay, ri.geometric.range, tag );
					const Scalar pSurvival = mso.noScatterPdfScale * PTEvalNoScatterSurvivalPdf<Tag>(
						pCurrentMedium, currentRay, ri.geometric.range, tag );
					const Value survivalWeight = PTSurvivalWeight<Tag>( Tr, pSurvival );
					throughput = throughput * survivalWeight;
					// DL-124: same factor, kept aside for the training fold
					// at PART 1's emission-hit MIS training below -- see
					// `escapeTr`'s declaration.
					escapeTr = survivalWeight;
				}
				else if( !scattered && !bHit )
				{
					// Ray escapes the scene through the medium (analog
					// no-scatter survival).  The escape survival event already
					// carries the Beer-Lambert factor via its probability, so
					// apply only the per-channel weight Tr / pSurvival
					// (deterministic no-scatter survival pdf; = 1 for
					// monochrome/NM homogeneous) before the env radiance below
					// multiplies into throughput — not Tr again.
					const Value Tr = PTEvalTransmittance<Tag>(
						pCurrentMedium, currentRay, maxDist, tag );
					const Scalar pSurvival = mso.noScatterPdfScale * PTEvalNoScatterSurvivalPdf<Tag>(
						pCurrentMedium, currentRay, maxDist, tag );
					const Value survivalWeight = PTSurvivalWeight<Tag>( Tr, pSurvival );
					throughput = throughput * survivalWeight;
					// DL-124: same factor, kept aside for the training fold
					// below -- see `escapeTr`'s declaration.
					escapeTr = survivalWeight;
				}
			}

			// Miss — environment / radiance map
			if( !bHit )
			{
				// GUI render modes P2b `indirect` (review-p2c P2-b fix): a
				// continuation ray landing on the env is reachable only at
				// depth>=1 (this block is inside `needsIntersection`) -- the
				// primary camera-ray miss is a separate, top-level path
				// (IntegrateRayTemplated) where the direct-background
				// suppression actually lives (see SetIndirectOnly's doc).
				// depth>=2 is always genuinely indirect.  depth==1 is the
				// same MIS-partner question as the emission gate above: this
				// BSDF-sampled env hit is env-NEE's suppressed MIS partner
				// ONLY when depth==0's scatter was non-delta (env-NEE was
				// actually performed and suppressed there); when depth==0
				// was delta (mirror/glass showing the env), there is no
				// suppressed partner and this is the sole estimator of that
				// specular-transport path, so it must survive.
				const bool suppressIndirectEnv = EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) &&
					depth == 1 && !bPassedThroughSpecular;
				// review-p2d P1-1: light solo switches env-NEE off; the
				// BSDF-side partner must go with it (see
				// PTSoloSuppressEnvironment for why fractional leakage
				// is worse than either extreme).
				const bool soloSuppressEnv = PTSoloSuppressEnvironment( caster );
				// Which radiance map does this escaping ray see?  A
				// per-object map (bound via the material) overrides the
				// global one; otherwise the global map supplies the
				// background radiance.
				//
				// MIS PARTNER RULE.  LightSampler's env-NEE block samples
				// the GLOBAL radiance map through the EnvironmentSampler
				// and MIS-weights its contribution against the BSDF pdf.
				// This BSDF-sampled env hit is that strategy's partner --
				// and must carry the complementary weight -- ONLY when the
				// map read here IS the global map.  A genuinely per-object
				// map has no NEE partner (nothing importance-samples it),
				// so it is the sole estimator of that transport and must be
				// added at full weight.
				//
				// CAVEAT on that per-object arm (slice-F2 review): LightSampler
				// does not know about per-object maps.  At a vertex whose
				// material carries its own radiance map, env-NEE still samples
				// the GLOBAL map (when one exists) and still MIS-weights that
				// sample against the BSDF pdf -- so the global-env NEE strategy
				// sits there MIS-weighted with no full-weight partner, while
				// this full-weight per-object arm has no partner of its own.
				// Both halves of that mismatch are UNREACHABLE from the
				// pathtracing_* rasterizers today (they pass the global map in
				// as pRadianceMap, so pEnvForEscape is always the global map and
				// the MIS arm always wins).  It goes live only if some caller
				// ever passes a genuinely per-object map here, and fixing it
				// properly means teaching LightSampler which map a shading point
				// actually sees -- out of scope for this arc, recorded so the
				// next reader does not mistake the arm for fully worked out.
				//
				// This used to be written `if( pRadianceMap ) { no MIS }
				// else if( global ) { MIS }`.  Every production rasterizer
				// passes the GLOBAL map in as `pRadianceMap`
				// (PathTracing{Pel,Spectral}Rasterizer::IntegratePixel both
				// do `pRadianceMap = pScene.GetGlobalRadianceMap()`), so the
				// first arm always won and the MIS arm was unreachable:
				// every BSDF-sampled env hit was added at weight 1 on top of
				// a correctly MIS-weighted env-NEE, i.e. the two strategies
				// summed to 1 + w_nee instead of 1.  Measured on a white
				// furnace (Lambertian albedo 1, uniform L = 1 env, no other
				// lights): +17.72 % over unity, matching the closed form
				// INTEGRAL_H (cos/pi) w_nee dw = ln(17)/16 = 0.17708 for
				// envPdf = 1/(4 pi) under the power-2 heuristic.  The HWSS
				// loop never had this bug (it tests the global map first),
				// which is exactly why hwss=true and hwss=false disagreed on
				// the same furnace scene.  That ordering is right for MIS but
				// wrong for map SELECTION, and its error is the mirror image of
				// the one fixed here: when a global map exists, HWSS reads it
				// and silently ignores a genuinely per-object map (over there,
				// the `else if( pRadianceMap )` arm is the unreachable one).
				// Also unreachable from pathtracing_* today, for the same
				// reason -- noted so the HWSS block is not read as the finished
				// reference for per-object maps.
				//
				// Independently corroborated on a second scene with its own
				// closed form: EnvLightBalanceTest's "env-only Lambertian"
				// topology (albedo 0.5 quad, uniform L = 1 env) has expected
				// mean radiance exactly 0.5.  PT read 0.58848 / 0.58948 /
				// 0.58857 before this fix (+17.70 %, the same ln(17)/16) and
				// 0.49994 / 0.50076 / 0.49997 after.  NOTE for whoever reads
				// that suite next: it asserts BDPT and VCM agree with PT, so
				// its tolerances were calibrated against the inflated PT.
				// With PT correct, BDPT (0.6422, +28.5 %) and VCM (0.6220,
				// +24.4 %) -- both untouched by this fix, both genuinely over
				// the closed form -- now fall outside those bands where all 101
				// passed before.  The failing COUNT IS NOT FIXED: measured over
				// nine consecutive runs of the suite on one machine it is 6, 7
				// or 8 (i.e. 93-95 of 101 pass), modally 7.  Six failures are
				// stable -- BDPT p99 on env-only Lambertian (RGB, spectral
				// hwss=false, spectral hwss=true), BDPT p99 on env+omni, and
				// VCM mean+p99 on env+mesh -- and TWO more sit right on the
				// band edge and flip run to run: `BDPT mean within 30% of PT:
				// env-only Lambertian` in the spectral hwss=false and hwss=true
				// topologies, both landing just under/over that 30 % line.  (The
				// suite's renders are not bit-reproducible: repeated runs of the
				// SAME binary move PT means by ~1 %, so any exact count quoted
				// for this suite is a sample, not a constant.)  These failures
				// are pre-existing BDPT/VCM env bias surfacing, not a regression
				// from this change, and the suite's reference / tolerances need
				// re-deriving against the closed form rather than against PT.
				//
				// One further consequence of merging the two arms: the
				// OpenPGL background-segment recorder at the bottom of this
				// block used to live only in the (unreachable) global arm, so
				// PT never trained the guiding field on env background at
				// all.  It now runs for every escaping ray, which is the
				// intended behaviour -- guiding should learn where the
				// environment's energy is -- and is a no-op for
				// `pathguiding FALSE`.
				const IRadianceMap* pEnvForEscape =
					pRadianceMap ? pRadianceMap : scene.GetGlobalRadianceMap();
				if( pEnvForEscape )
				{
					// `envRadiance` stays RAW (unweighted) all the way down; the MIS
					// weight is kept beside it in `envMiWeight` and applied once at
					// each consumer.  The guiding recorder needs the raw radiance and
					// the weight as two SEPARATE fields (see
					// AddPTIGuidingBackgroundSegment's CONVENTION note), so the weight
					// is deliberately not folded into `envRadiance` the way it was
					// before -- the arithmetic reaching `result` is unchanged.
					const Value envRadiance = PTEvalRadianceMap<Tag>(
						pEnvForEscape, currentRay, rast, tag );
					Scalar envMiWeight = 1.0;

					// MIS weight for BSDF-sampled environment hit
					// DL-74 (round-3 review): the gate admits EITHER density
					// being positive, because the block's two arms use two
					// different ones -- the optimal-MIS training below reads
					// `bsdfPdf` (the true sampling density) and the weight
					// reads `bsdfMisPdf` (the nominal partner).  Gating on
					// `bsdfPdf` alone could skip a live partner.  Same rule
					// as `RayCasterEnvEscapeMISWeight`.
					if( pEnvForEscape == scene.GetGlobalRadianceMap() && pLS &&
						( bsdfPdf > 0 || bsdfMisPdf > 0 ) )
					{
						const EnvironmentSampler* pES = pLS->GetEnvironmentSampler();
						if( pES )
						{
							const Scalar envPdf = pES->Pdf( currentRay.Dir() );
							if( envPdf > 0 )
							{
								// Optimal MIS training.
								// DL-124: fold `escapeTr` (the medium
								// transmittance along THIS escape segment,
								// 1 when no medium was crossed) into the
								// trained numerator -- its NEE partner
								// (LightSampler's env arm) already carries
								// the equivalent shadow-ray transmittance
								// via EvalShadowTransmittance, and the
								// realized moment must match what actually
								// reaches the film (`bsdfTimesCos` alone
								// omitted it; `throughput`, which DOES
								// carry it, is not part of this per-vertex
								// moment).
								if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() )
								{
									const Scalar fLum = PTPositiveMagnitude( envRadiance * bsdfTimesCos * escapeTr );
									const Scalar f2 = fLum * fLum;
									if( f2 > 0 && bsdfPdf > 0 )
									{
										const_cast<OptimalMISAccumulator*>(rc.pOptimalMIS)->Accumulate(
											rast.x, rast.y,
											f2, bsdfPdf, kTechniqueBSDF );
									}
								}

								// DL-74: the WEIGHT uses the nominal
								// MIS-partner density (the same function
								// LightSampler's env-NEE arm evaluates for
								// this direction); the TRAINING above uses
								// the true sampling density.  A zero
								// nominal density means no BSDF-side
								// partner exists, so the escape keeps the
								// full sample -- matching NEE's own
								// `pBsdf > 0` fallback.
								Scalar w_bsdf = 1.0;
								if( bsdfMisPdf > 0 )
								{
									if( rc.pOptimalMIS && rc.pOptimalMIS->IsReady() )
									{
										const Scalar alpha = rc.pOptimalMIS->GetAlpha( rast.x, rast.y );
										w_bsdf = MISWeights::OptimalMIS2Weight( bsdfMisPdf, envPdf, alpha );
									}
									else
									{
										w_bsdf = PowerHeuristic( bsdfMisPdf, envPdf );
									}
								}
								envMiWeight = w_bsdf;
							}
						}
					}

					// Continuation-ray env hit -- see suppressIndirectEnv's
					// doc above for the exact depth==1 MIS-partner rule.
					if( !suppressIndirectEnv && !soloSuppressEnv ) {
						result = result + throughput * ( envRadiance * envMiWeight );
					}

#ifdef RISE_ENABLE_OPENPGL
					// Store the RAW radiance plus the MIS weight, matching the
					// surface-emission recorder (SetPTIGuidingDirectContribution(
					// .., rawEmission, emissionMiWeight )).  The gate tests the raw
					// luminance for the same reason that site tests raw emission: a
					// direction whose MIS weight happens to be near 0 still carries
					// real radiance the field should learn.
					if( guidingRecorder && guidingRecorder->active &&
						PTGuidingLuminance( envRadiance ) > 0 )
					{
						AddPTIGuidingBackgroundSegment( *guidingRecorder, currentRay,
							PTGuidingPel( envRadiance ), envMiWeight );
					}
#endif
				}
				break;
			}
		}
		needsIntersection = true;

		// ============================================================
		// Surface hit processing
		// ============================================================

		// Determine current medium BEFORE updating IOR stack, so NEE
		// shadow rays use the medium the ray was traveling through.
		const IObject* pMediumObject = 0;
		const IMedium* pCurrentMedium = MediumTracking::GetCurrentMediumWithObject(
			iorStack, &scene, pMediumObject );

		// G6: stamp the ambient (incident-medium) IOR from the stack so the GGX
		// conductor Fresnel evaluated in BOTH SPF::Scatter and BRDF::value sees
		// the surrounding medium (e.g. enamel glass) rather than hardcoded air.
		// Read BEFORE SetCurrentObject so top() is the medium the ray was
		// travelling through.  Guard a non-positive stack top to air (1.0).
		{
			const Scalar ambIOR = iorStack.top();
			ri.geometric.ambientIOR = ( ambIOR > 0.0 ) ? ambIOR : 1.0;
		}

		// Apply intersection modifier (bump maps etc.)
		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		// Update IOR stack
		iorStack.SetCurrentObject( ri.pObject );

		// GUI render modes P2b `clay_lights`: substitute the shared clay
		// BRDF for EVERY hit when the mode is active -- see mClayOverride's
		// doc.  This also makes pBRDF non-null for materials that
		// authored none (pure-specular SPF-only materials), so the
		// "no-BSDF" branch below is never taken under clay_lights either.
		const IBSDF* pBRDF = EffectivePathTracingClayOverride( rc, mClayOverride ) ? pClayBRDF :
			( ri.pMaterial ? ri.pMaterial->GetBSDF() : 0 );

		// Build a RAY_STATE for utility functions that need it
		IRayCaster::RAY_STATE rs;
		rs.depth = depth + 1;
		rs.importance = importance;
		rs.bsdfPdf = bsdfPdf;
		rs.bsdfMisPdf = bsdfMisPdf;
		rs.bsdfTimesCos = PTRayStateBsdfTimesCos( bsdfTimesCos );
		rs.considerEmission = considerEmission;
		rs.type = rayType;
		rs.diffuseBounces = diffuseBounces;
		rs.glossyBounces = glossyBounces;
		rs.transmissionBounces = transmissionBounces;
		rs.translucentBounces = translucentBounces;
		rs.glossyFilterWidth = glossyFilterWidth;

#ifdef RISE_ENABLE_OPENPGL
		PGLPathSegmentData* guidingSegment =
			(guidingRecorder && guidingRecorder->active) ?
				BeginPTIGuidingSegment( *guidingRecorder, ri.geometric ) : 0;
#endif

		if constexpr ( Traits::is_pel ) {
		if( ff ) {
			FF_TRACE( "  depth=%u HIT obj=%p mat=%p pos=(%.4f,%.4f,%.4f) n=(%.4f,%.4f,%.4f) thr=(%.4f,%.4f,%.4f) psS=%d nsS=%d",
				depth, (const void*)ri.pObject, (const void*)ri.pMaterial,
				ri.geometric.ptIntersection.x, ri.geometric.ptIntersection.y, ri.geometric.ptIntersection.z,
				ri.geometric.vNormal.x, ri.geometric.vNormal.y, ri.geometric.vNormal.z,
				throughput[0], throughput[1], throughput[2],
				(int)bPassedThroughSpecular, (int)bHadNonSpecularShading );
		}
		}

		// ============================================================
		// PART 1: Emission
		// ============================================================
		{
			IEmitter* pEmitter = ri.pMaterial ? ri.pMaterial->GetEmitter() : 0;
			// When SMS is active, suppress emission from BSDF paths that
			// passed through specular surfaces, but ONLY if there was a
			// prior non-specular shading point where SMS was evaluated.
			// Without that check, camera->glass->light paths (with no
			// diffuse receiver) would be killed.  bSMSEnabled == (pSolver
			// != 0); the Pel original spelled this `pSolver && ...` inline,
			// the NM original as the `smsSuppressEmission` flag used here.
			const bool smsSuppressEmission = bSMSEnabled
				&& bPassedThroughSpecular && bHadNonSpecularShading;
			// GUI render modes P2b `light solo` (docs/gui/RENDER_MODES.md
			// §3): under solo, a BSDF-sampled hit contributes emission
			// ONLY when the hit object IS the soloed mesh luminary --
			// every other emitter (including one that is itself NEE-
			// unreachable, e.g. CanBeAreaLight()==false, which normally
			// takes full unweighted emission) reads black, so "exactly
			// one light enabled" holds for the BSDF-sampling strategy too,
			// not just NEE.  Reads solo state directly off `pLS` -- the
			// LightSampler already owns it (SetSoloLight/SetSoloLuminary),
			// so there is no second, duplicated flag on the integrator to
			// drift out of sync.
			const bool soloSuppressEmission = pLS && pLS->IsSoloActive() &&
				!( ri.pObject && pLS->IsSoloTargetLuminary( ri.pObject ) );
			if( pEmitter && considerEmission && !soloSuppressEmission )
			{
				if( smsSuppressEmission )
				{
					// Skip emission entirely; SMS handles this contribution.
					// SMS_DIAG counters + the firefly trace are Pel-only
					// diagnostics (the NM original had neither), so they
					// compile out for NMTag.
					if constexpr ( Traits::is_pel )
					{
#if SMS_DIAG_ENABLED
						g_smsDiag_emissionSuppressed.fetch_add( 1, std::memory_order_relaxed );
						const RISEPel rawE_diag = pEmitter->emittedRadiance(
							ri.geometric, -ri.geometric.ray.Dir(), ri.geometric.vGeomNormal );
						SMSDiag_AddLum( g_smsDiag_sumSuppLumX,
							ColorMath::MaxValue( throughput * rawE_diag ) );
#endif
						if( ff ) {
							RISEPel rawE = pEmitter->emittedRadiance(
								ri.geometric, -ri.geometric.ray.Dir(), ri.geometric.vGeomNormal );
							FF_TRACE( "  depth=%u EMISSION-SUPPRESSED-BY-SMS rawE=(%.3e,%.3e,%.3e) thr=(%.3e,%.3e,%.3e)",
								depth, rawE[0], rawE[1], rawE[2],
								throughput[0], throughput[1], throughput[2] );
						}
					}
				}
				else
				{
				Value emission = PTEvalEmittedRadiance<Tag>(
					pEmitter, ri.geometric, -ri.geometric.ray.Dir(), ri.geometric.vGeomNormal, tag );
				const Value rawEmission = emission;
				Scalar emissionMiWeight = 1.0;

				// An emitter on geometry that cannot be uniformly area-sampled (CanBeAreaLight()
				// false, OR NO geometry at all -- e.g. a csg_object, see LuminaryManager::
				// AddToLuminaryList) is NOT in the NEE light set (LuminaryManager skips it), so
				// the light-sampling strategy's pdf for this BSDF hit is ZERO -> the emission
				// must take FULL weight.  Skipping the block leaves it unweighted (and un-zeroed
				// under RIS, so it is not lost).
				//
				// CRASH FIX (2026-07-31 fix round 2): this used to read
				// `( !pEmitGeom || pEmitGeom->CanBeAreaLight() )` -- INVERTED for the null case,
				// so a csg_object's null pEmitGeom evaluated emitterNeeSampleable = TRUE and
				// entered this block, reaching `ri.pObject->GetArea()` (null-deref pre this fix
				// round; Object::GetArea() now has its own base-layer null guard and returns 0,
				// which `area > 0` below would also have caught -- but null geometry must
				// independently be treated as "NOT NEE-sampleable", matching the
				// CanBeAreaLight()==false precedent, not the inverted default).
				const IGeometry* pEmitGeom = ri.pObject ? ri.pObject->GetGeometry() : 0;
				const bool emitterNeeSampleable = ( pEmitGeom && pEmitGeom->CanBeAreaLight() );

				// DL-74 (round-3 review): EITHER density -- see the env
				// escape block above for why the gate cannot name just one.
				if( ( bsdfPdf > 0 || bsdfMisPdf > 0 ) && ri.pObject && emitterNeeSampleable )
				{
					const Scalar area = ri.pObject->GetArea();
					if( area > 0 )
					{
						const Scalar cosLight = fabs( Vector3Ops::Dot(
							ri.geometric.ray.Dir(), ri.geometric.vGeomNormal ) );
						if( cosLight > 0 )
						{
							const Scalar dist = Vector3Ops::Magnitude(
								Vector3Ops::mkVector3(
									ri.geometric.ptIntersection,
									ri.geometric.ray.origin ) );

							if( pLS && pLS->IsRISActive() )
							{
								emissionMiWeight = 0.0;
								// Pel zeroed via `emission * 0.0`; NM via a
								// hard `0` (they differ only for a non-finite
								// emission — preserve each variant exactly).
								if constexpr ( Traits::is_pel ) {
									emission = emission * Scalar( 0 );
								} else {
									emission = Traits::zero();
								}
							}
							else
							{
								Scalar pdfSelect = 1.0;
								if( pLS )
								{
									pdfSelect = pLS->CachedPdfSelectLuminary(
										*ri.pObject,
										ri.geometric.ray.origin,
										ri.geometric.ray.Dir() );
									if( pdfSelect <= 0 ) {
										pdfSelect = 1.0;
									}
								}

								const Scalar p_nee = pdfSelect * (dist * dist) / (area * cosLight);

								// DL-124: fold `escapeTr` in here too -- when the
								// vertex that set `bsdfTimesCos` was a volume
								// scatter, its NEE partner (LightSampler's
								// mesh-luminary arm, called via
								// PTEvaluateInScattering) already multiplies by
								// EvalShadowTransmittance; this BSDF-sampled hit
								// on the SAME luminary must carry the matching
								// medium attenuation crossed en route.  A no-op
								// (escapeTr == 1) whenever no medium was crossed
								// since the training vertex, including the
								// ordinary all-surface case.
								if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() )
								{
									const Scalar fLum = PTPositiveMagnitude( rawEmission * bsdfTimesCos * escapeTr );
									const Scalar f2 = fLum * fLum;
									if( f2 > 0 && bsdfPdf > 0 )
									{
										const_cast<OptimalMISAccumulator*>(rc.pOptimalMIS)->Accumulate(
											rast.x, rast.y,
											f2, bsdfPdf, kTechniqueBSDF );
									}
								}

								// DL-74: weight from the nominal MIS-partner
								// density -- `LightSampler`'s area-light NEE
								// arm evaluates the same function for the
								// same direction, which is what makes this
								// pair sum to one under guiding.  Training
								// above keeps the true sampling density.
								Scalar w_bsdf = 1.0;
								if( bsdfMisPdf > 0 )
								{
									if( rc.pOptimalMIS && rc.pOptimalMIS->IsReady() )
									{
										const Scalar alpha = rc.pOptimalMIS->GetAlpha(
											rast.x, rast.y );
										w_bsdf = MISWeights::OptimalMIS2Weight( bsdfMisPdf, p_nee, alpha );
									}
									else
									{
										w_bsdf = PowerHeuristic( bsdfMisPdf, p_nee );
									}
								}
								emissionMiWeight = w_bsdf;
								emission = emission * w_bsdf;
							}
						}
					}
				}

				// Clamp at depth > 0 (not the first camera hit)
				if( depth > 0 ) {
					emission = ClampContribution( emission, stabilityConfig.directClamp );
				}

				// GUI render modes P2b `indirect` (review-p2c P2-b fix):
				// gate on whether an MIS PARTNER EXISTS, not on depth alone.
				// depth==0 is unconditionally suppressed (the directly-
				// visible emitter itself -- see SetIndirectOnly's doc for
				// why this uses a raw depth compare, not depth==startDepth).
				// depth==1 is suppressed ONLY when the depth==0 scatter was
				// NON-delta (`!bPassedThroughSpecular`) -- that is exactly
				// the case where NEE was actually performed (and suppressed)
				// at depth==0, so this BSDF-sampled emission hit is NEE's
				// MIS partner; suppressing it too keeps the "direct at the
				// camera vertex" pair (NEE + its BSDF-sampled partner) fully
				// zeroed instead of leaving the unweighted partner half in.
				// When depth==0's scatter WAS delta (mirror/glass), NEE
				// cannot sample through a delta BSDF, so
				// there is no suppressed partner at depth==0 to compensate
				// for -- this depth==1 emission is the ONLY estimator of
				// that specular-transport path and must survive (a mirror
				// reflecting an area light must still show it under
				// `indirect`).  depth>=2 emission is always the MIS partner
				// of NEE at depth>=1 (genuinely indirect) and stays
				// untouched regardless of specular history.
				const bool suppressIndirectEmission = EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) &&
					( depth == 0 || ( depth == 1 && !bPassedThroughSpecular ) );
				const Value emissionContrib = throughput * emission;
				if( !suppressIndirectEmission ) {
					result = result + emissionContrib;
				} else if( pDirectResult && depth == 0 ) {
					*pDirectResult = *pDirectResult + emissionContrib;
				}

				if constexpr ( Traits::is_pel ) {
					if( ff ) {
						const RISEPel contrib = throughput * emission;
						FF_TRACE( "  depth=%u EMISSION rawE=(%.3e,%.3e,%.3e) mis=%.4f thr=(%.3e,%.3e,%.3e) contrib=(%.3e,%.3e,%.3e) result=(%.3e,%.3e,%.3e)",
							depth, rawEmission[0], rawEmission[1], rawEmission[2],
							emissionMiWeight,
							throughput[0], throughput[1], throughput[2],
							contrib[0], contrib[1], contrib[2],
							result[0], result[1], result[2] );
					}
				}

#ifdef RISE_ENABLE_OPENPGL
				if( guidingSegment &&
					PTSurvivalMagnitude( rawEmission ) > 0 ) {
					SetPTIGuidingDirectContribution( guidingSegment, PTGuidingPel( rawEmission ), emissionMiWeight );
				}
#endif
			} // else (not suppressed by SMS)
			}
		}

		// ============================================================
		// BSSRDF: Subsurface scattering via diffusion profile
		// ============================================================
		{
			ISubSurfaceDiffusionProfile* pProfile =
				ri.pMaterial ? ri.pMaterial->GetDiffusionProfile() : 0;

			// P2-d fix (review-p2b): under clay_lights the surface is
			// purely the clay Lambertian -- the authored material's
			// diffusion-profile transport must not run (it would leak
			// scattering parameters through, defeating the mode's
			// material-independence contract).  See SetClayOverride's doc.
			if( pProfile && pBRDF && !EffectivePathTracingClayOverride( rc, mClayOverride ) )
			{
				// Front-face gate uses the GEOMETRIC normal — "is the ray
				// hitting the outside of this surface" is a side-of-surface
				// question that PBRT 4e §10.1.1 explicitly assigns to the
				// geometric normal.  The original comment ("back-face hits
				// skip BSSRDF") would otherwise leak/kill subsurface energy
				// through bumpy regions where the shading normal flips
				// independently of the actual face orientation.
				const Vector3 wo = Vector3Ops::Normalize( -ri.geometric.ray.Dir() );
				// DL-70: against the TRUE, ray-INDEPENDENT geometric
				// normal.  A double-sided mesh reports a `vGeomNormal`
				// that opposes the ray at every hit, so this gate was an
				// unconditional PASS and a BACK-face (interior) hit was
				// admitted into BSSRDF entry sampling -- feeding
				// `BSSRDFSampling::SampleEntryPoint`, whose own DL-71
				// correction already works in TRUE-normal space, a
				// shading point on the wrong side of the surface.
				// `TrueGeomFacing` restores the agreement; it is a no-op
				// on single-sided meshes and analytic primitives.
				// Deliberately NOT a `HasTrueGeomSide()` SKIP: a hair hit
				// has no true side (DL-75), but rejecting it here would
				// silently remove subsurface scattering from hair, a
				// combination DL-75 left undefined-but-permitted and
				// `HairSSSEntryNormalTest` characterises as producing
				// well-defined output.  `TrueGeomFacing` is the identity
				// on a ray-derived normal, so hair keeps exactly its
				// pre-DL-70 behaviour here.
				//
				// DL-96 (CLOSED): this gate used to assume CLOSED-SOLID
				// semantics unconditionally -- "outside" is the single,
				// fixed, TRUE outward normal, so exactly one face of a
				// double-sided mesh admitted BSSRDF entry, silently
				// dropping SSS entry from an OPEN double-sided sheet's
				// (a leaf, a cloth card) second face, where both faces
				// are legitimate entry points.  `BSSRDFEntryFacing()`
				// keeps that closed-solid gate for a genuinely closed
				// solid (`!bOpenSheet`), and for an open sheet
				// (`bOpenSheet`, set by the geometry -- see its doc
				// comment) admits entry from whichever RAY-FACING side
				// the ray actually struck instead, so both faces enter.
				const Scalar cosInGeom = ri.geometric.BSSRDFEntryFacing( wo );
				if( cosInGeom > NEARZERO )
				{
					// Fresnel cosine uses the SHADING normal — the
					// transmission is a BSDF-coupled angular dependence
					// (PBRT 4e §11.4.2 sampling the BSSRDF).  Clamp away
					// from zero in case shading and geometric disagree
					// near grazing.
					// Fresnel cosine clamped via fabs+NEARZERO to a safe
					// positive value: Sw is symmetric in cos sign and
					// parameterised against the shading frame.  This
					// replaces a fallback-to-cosInGeom branch that produced
					// a discontinuous Ft when shading swung past horizon.
					const Scalar cosInShade = Vector3Ops::Dot( ri.geometric.vNormal, wo );
					const Scalar cosIn = r_max( fabs( cosInShade ), Scalar( NEARZERO ) );
					const Scalar Ft = pProfile->FresnelTransmission( cosIn, ri.geometric );

					if( Ft > NEARZERO )
					{
						IndependentSampler fallbackSampler( rc.random );
						ISampler& bssrdfSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;

						BSSRDFSampling::SampleResult bssrdf = BSSRDFSampling::SampleEntryPoint(
							ri.geometric, ri.pObject, ri.pMaterial, bssrdfSampler, PTTagNm( tag ) );

						if( bssrdf.valid )
						{
							const Value bssrdfWeight = PTBssrdfWeight<Tag>( bssrdf );
							const Value bssrdfWeightSpatial = PTBssrdfWeightSpatial<Tag>( bssrdf );

							RayIntersectionGeometric entryRI(
								Ray( bssrdf.entryPoint, bssrdf.scatteredRay.Dir() ),
								rast );
							entryRI.bHit = true;
							entryRI.ptIntersection = bssrdf.entryPoint;
							entryRI.vNormal = bssrdf.entryNormal;
							entryRI.vGeomNormal = bssrdf.entryGeomNormal;
							entryRI.onb = bssrdf.entryONB;

							const Scalar eta = pProfile->GetIOR( ri.geometric );
							BSSRDFEntryBSDF entryBSDF( pProfile, eta );
							BSSRDFEntryMaterial entryMaterial;

							const unsigned int nextTranslucentBounces = translucentBounces + 1;
							const bool skipSSS =
								nextTranslucentBounces > stabilityConfig.maxTranslucentBounce;

							if( !skipSSS )
							{
								// NEE at BSSRDF entry point.  GUI render modes P2b
								// `indirect` (review-p2c P2-c fix): this is the
								// SSS analog of the surface NEE gate above --
								// mask it the same way (LOOP-LOCAL depth==0
								// only; still EVALUATED for RNG lockstep).
								// Previously ungated, so a directly-lit
								// translucent surface (skin, wax, marble) kept
								// showing its direct SSS glow under `indirect`.
								if( pLS )
								{
									// DL-72 P2-3: this arm's own result is scaled by
									// `bssrdfWeightSpatial` on the next line, so the
									// INTEGRAND it must train the optimal-MIS moment
									// with carries that factor -- exactly as its MIS
									// partner, the exit continuation below, now does
									// (PTBssrdfTrainedBsdfTimesCos).  Training-only;
									// `directSSS` itself is unchanged.
									//
									// DL-185: `castRRCompensation_` folds in only at
									// `depth == startDepth` -- the exact vertex CastRay
									// handed off to this call, if this call is itself a
									// re-entry through RayCaster::CastRay.  Deeper
									// iterations of THIS loop are the integrator's own
									// internal continuations, never re-wrapped by
									// RayCaster's cast-level RR, so they must not be
									// scaled by it.
									Value directSSS = PTEvaluateDirectLighting<Tag>(
										pLS, entryRI, entryBSDF, &entryMaterial, caster,
										bssrdfSampler, ri.pObject, 0, false, 0, tag,
										0, 0, PTSurvivalMagnitude( bssrdfWeightSpatial ) *
											( depth == startDepth ? castRRCompensation_ : Scalar( 1.0 ) ) );
									Value sssDirectContrib = throughput * bssrdfWeightSpatial * directSSS;
									sssDirectContrib = ClampContribution( sssDirectContrib,
										stabilityConfig.directClamp );
									if( !( EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) && depth == 0 ) ) {
										result = result + sssDirectContrib;
									} else if( pDirectResult ) {
										*pDirectResult = *pDirectResult + sssDirectContrib;
									}
								}

								// BSSRDF continuation via CastRay sub-path
								{
									Value sssThroughput = bssrdfWeight;

									// DL-72 (round 3, this pass): pair every Accumulate
									// (inside RayCasterEnvEscapeMISWeight, if this
									// continuation happens to escape to the global env
									// map) with one AccumulateCount here, at the SAME
									// "sample attempt" granularity the main surface
									// continuation uses -- BEFORE Russian roulette, so
									// a subsequently RR-killed attempt is still counted
									// (OptimalMISAccumulator.h's AccumulateCount doc:
									// "regardless of whether the sample contributed
									// non-zero radiance").  Round 2 removed this call
									// because the paired moment was wrong-shaped; round
									// 3 derives the correct one below and reinstates it.
									// Round-2 review (P2-2): dropped the
									// `PTSurvivalMagnitude(sssThroughput) > NEARZERO` gate --
									// it excluded a zero-throughput BSSRDF exit attempt from
									// the count, the same undercounting pattern fixed at the
									// main surface continuation's `!skipContinuation` gate.
									// A zero-throughput attempt gets no matching
									// `Accumulate()` either way, so counting it here is a
									// correctly COUNTED ZERO, not a phantom sample.
									if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() )
									{
										const_cast<OptimalMISAccumulator*>(rc.pOptimalMIS)->AccumulateCount(
											rast.x, rast.y, kTechniqueBSDF );
									}

									const PathTransportUtilities::RussianRouletteResult rr =
										PathTransportUtilities::EvaluateRussianRoulette(
											depth, rrMinDepth, rrThreshold,
											importance * PTSurvivalMagnitude( sssThroughput ),
											importance, bssrdfSampler.Get1D() );
									if( !rr.terminate )
									{
										if( rr.survivalProb < 1.0 ) {
											sssThroughput = PTDivByScalar( sssThroughput, rr.survivalProb );
										}

										Value cthis = Traits::zero();
										Ray continuationRay = bssrdf.scatteredRay;
										continuationRay.Advance( 1e-8 );

										IRayCaster::RAY_STATE rs2;
										rs2.depth = depth + 2;
										rs2.considerEmission = true;
										rs2.importance = importance * PTSurvivalMagnitude( sssThroughput );
										rs2.bsdfPdf = bssrdf.cosinePdf;
										// DL-74: the BSSRDF exit is sampled from its own
										// cosine density with no guiding anywhere in the
										// path, so the sampling density and the MIS-partner
										// density coincide.
										rs2.bsdfMisPdf = bssrdf.cosinePdf;
										rs2.type = IRayCaster::RAY_STATE::eRayDiffuse;
										rs2.diffuseBounces = diffuseBounces;
										rs2.glossyBounces = glossyBounces;
										rs2.transmissionBounces = transmissionBounces;
										rs2.translucentBounces = nextTranslucentBounces;
										rs2.glossyFilterWidth = glossyFilterWidth;
										// BSSRDF emerges as a diffuse scatter at a
										// non-specular shading point — propagate
										// SMS emission-suppression state so an
										// onwards child ray through glass to a
										// light doesn't re-enable emission.
										rs2.smsPassedThroughSpecular = false;
										rs2.smsHadNonSpecularShading = true;
										// DL-72 / DL-84 (round 4): the FULL vertex-local
										// integrand of this exit sample, `weight *
										// cosinePdf` -- see PTBssrdfTrainedBsdfTimesCos's
										// derivation above.  Round 3 trained `Sw*cos`,
										// which is the same quantity with the
										// area-measure `weightSpatial` dropped; the
										// accumulator's moment is of the INTEGRAND, so
										// that scaled this site's moments arbitrarily.
										// Paired with the AccumulateCount call above,
										// with rs2.bsdfPdf = bssrdf.cosinePdf (unchanged)
										// as the matching density, and with the same
										// `weightSpatial` scale now given to the NEE arm
										// at this vertex (its `neeTrainingScale`
										// argument, a few lines above).
										//
										// ROUND 7: `sssThroughput`, not the pre-roulette
										// `bssrdfWeight`.  The `AccumulateCount` above
										// sits BEFORE Russian roulette, so the moment
										// paired with it has to be the AS-CARRIED one --
										// the realized `E_pre/q` rather than `q*E_pre`.
										// `sssThroughput` IS `bssrdfWeight` until the
										// roulette divides it by `rr.survivalProb` a few
										// lines above, so this is a no-op whenever the
										// roulette did not fire.  Derivation at the
										// ordinary surface continuation's
										// `bsdfTimesCosVal`.
										rs2.bsdfTimesCos = PTRayStateBsdfTimesCos(
											PTBssrdfTrainedBsdfTimesCos( sssThroughput, bssrdf.cosinePdf ) );

										PTCastRay<Tag>( caster, rc, rast, continuationRay,
											cthis, rs2, pRadianceMap, iorStack, tag );

										Value indirect = sssThroughput * cthis;
										if( depth > 0 ) {
											indirect = ClampContribution( indirect,
												stabilityConfig.indirectClamp );
										}
										result = result + throughput * indirect;
									}
								}
							}
						}
					}
				}
			}
		}

		// ============================================================
		// Random-walk subsurface scattering
		// ============================================================
		{
			const RandomWalkSSSParams* pRWParams =
				ri.pMaterial ? ri.pMaterial->GetRandomWalkSSSParams() : 0;

			// NM-only fallback (preserved asymmetry): when the material
			// provides per-wavelength random-walk params but no RGB ones,
			// the NM original synthesised them via GetRandomWalkSSSParamsNM.
			[[maybe_unused]] RandomWalkSSSParams rwParamsNM;
			if constexpr ( Traits::is_nm ) {
				if( !pRWParams && ri.pMaterial &&
					ri.pMaterial->GetRandomWalkSSSParamsNM( tag.nm, rwParamsNM ) ) {
					pRWParams = &rwParamsNM;
				}
			}

			// P2-d fix (review-p2b): same clay-independence rule as the
			// diffusion-profile branch above -- random-walk SSS must not
			// run under clay_lights either.
			if( pRWParams && pBRDF && !EffectivePathTracingClayOverride( rc, mClayOverride ) )
			{
				// Front-face gate uses GEOMETRIC normal; Schlick Fresnel
				// cosine uses SHADING.  See the BSSRDF site above for
				// the rationale.
				const Vector3 wo = Vector3Ops::Normalize( -ri.geometric.ray.Dir() );
				// DL-70: against the TRUE, ray-INDEPENDENT geometric
				// normal.  A double-sided mesh reports a `vGeomNormal`
				// that opposes the ray at every hit, so this gate was an
				// unconditional PASS and a BACK-face (interior) hit was
				// admitted into BSSRDF entry sampling -- feeding
				// `BSSRDFSampling::SampleEntryPoint`, whose own DL-71
				// correction already works in TRUE-normal space, a
				// shading point on the wrong side of the surface.
				// `TrueGeomFacing` restores the agreement; it is a no-op
				// on single-sided meshes and analytic primitives.
				// Deliberately NOT a `HasTrueGeomSide()` SKIP: a hair hit
				// has no true side (DL-75), but rejecting it here would
				// silently remove subsurface scattering from hair, a
				// combination DL-75 left undefined-but-permitted and
				// `HairSSSEntryNormalTest` characterises as producing
				// well-defined output.  `TrueGeomFacing` is the identity
				// on a ray-derived normal, so hair keeps exactly its
				// pre-DL-70 behaviour here.
				//
				// DL-96 (CLOSED): this gate used to assume CLOSED-SOLID
				// semantics unconditionally -- "outside" is the single,
				// fixed, TRUE outward normal, so exactly one face of a
				// double-sided mesh admitted BSSRDF entry, silently
				// dropping SSS entry from an OPEN double-sided sheet's
				// (a leaf, a cloth card) second face, where both faces
				// are legitimate entry points.  `BSSRDFEntryFacing()`
				// keeps that closed-solid gate for a genuinely closed
				// solid (`!bOpenSheet`), and for an open sheet
				// (`bOpenSheet`, set by the geometry -- see its doc
				// comment) admits entry from whichever RAY-FACING side
				// the ray actually struck instead, so both faces enter.
				const Scalar cosInGeom = ri.geometric.BSSRDFEntryFacing( wo );
				if( cosInGeom > NEARZERO )
				{
					// Fresnel cosine clamped via fabs+NEARZERO to a safe
					// positive value: Sw is symmetric in cos sign and
					// parameterised against the shading frame.  This
					// replaces a fallback-to-cosInGeom branch that produced
					// a discontinuous Ft when shading swung past horizon.
					const Scalar cosInShade = Vector3Ops::Dot( ri.geometric.vNormal, wo );
					const Scalar cosIn = r_max( fabs( cosInShade ), Scalar( NEARZERO ) );
					const Scalar F0 = ((pRWParams->ior - 1.0) / (pRWParams->ior + 1.0)) *
						((pRWParams->ior - 1.0) / (pRWParams->ior + 1.0));
					const Scalar F = F0 + (1.0 - F0) * pow( 1.0 - cosIn, 5.0 );
					const Scalar Ft = 1.0 - F;

					if( Ft > NEARZERO )
					{
						IndependentSampler walkSampler( rc.random );
						IndependentSampler fallbackSampler( rc.random );
						ISampler& bssrdfSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;
						ISampler& rwSampler = bssrdfSampler.HasFixedDimensionBudget()
							? static_cast<ISampler&>(walkSampler) : bssrdfSampler;

						BSSRDFSampling::SampleResult bssrdf = RandomWalkSSS::SampleExit(
							ri.geometric, ri.pObject,
							pRWParams->sigma_a, pRWParams->sigma_s, pRWParams->sigma_t,
							pRWParams->g, pRWParams->ior, pRWParams->maxBounces,
							rwSampler, PTTagNm( tag ), pRWParams->maxDepth );

						if( bssrdf.valid )
						{
							const Scalar bf = pRWParams->boundaryFilter;
							const Value bssrdfWeight = PTBssrdfWeight<Tag>( bssrdf ) * Ft * bf;
							const Value bssrdfWeightSpatial = PTBssrdfWeightSpatial<Tag>( bssrdf ) * Ft * bf;

							RayIntersectionGeometric entryRI(
								Ray( bssrdf.entryPoint, bssrdf.scatteredRay.Dir() ),
								rast );
							entryRI.bHit = true;
							entryRI.ptIntersection = bssrdf.entryPoint;
							entryRI.vNormal = bssrdf.entryNormal;
							entryRI.vGeomNormal = bssrdf.entryGeomNormal;
							entryRI.onb = bssrdf.entryONB;

							RandomWalkEntryBSDF entryBSDF( pRWParams->ior );
							BSSRDFEntryMaterial entryMaterial;

							const unsigned int nextTranslucentBounces = translucentBounces + 1;
							const bool skipSSS =
								nextTranslucentBounces > stabilityConfig.maxTranslucentBounce;

							if( !skipSSS )
							{
								// GUI render modes P2b `indirect` (review-p2c
								// P2-c fix): same SSS-analog-of-surface-NEE
								// gate as the diffusion-profile site above.
								if( pLS )
								{
									// DL-72 P2-3: this arm's own result is scaled by
									// `bssrdfWeightSpatial` on the next line, so the
									// INTEGRAND it must train the optimal-MIS moment
									// with carries that factor -- exactly as its MIS
									// partner, the exit continuation below, now does
									// (PTBssrdfTrainedBsdfTimesCos).  Training-only;
									// `directSSS` itself is unchanged.
									//
									// DL-185: `castRRCompensation_` folds in only at
									// `depth == startDepth` -- the exact vertex CastRay
									// handed off to this call, if this call is itself a
									// re-entry through RayCaster::CastRay.  Deeper
									// iterations of THIS loop are the integrator's own
									// internal continuations, never re-wrapped by
									// RayCaster's cast-level RR, so they must not be
									// scaled by it.
									Value directSSS = PTEvaluateDirectLighting<Tag>(
										pLS, entryRI, entryBSDF, &entryMaterial, caster,
										bssrdfSampler, ri.pObject, 0, false, 0, tag,
										0, 0, PTSurvivalMagnitude( bssrdfWeightSpatial ) *
											( depth == startDepth ? castRRCompensation_ : Scalar( 1.0 ) ) );
									Value sssDirectContrib = throughput * bssrdfWeightSpatial * directSSS;
									sssDirectContrib = ClampContribution( sssDirectContrib,
										stabilityConfig.directClamp );
									if( !( EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) && depth == 0 ) ) {
										result = result + sssDirectContrib;
									} else if( pDirectResult ) {
										*pDirectResult = *pDirectResult + sssDirectContrib;
									}
								}

								// BSSRDF continuation via CastRay sub-path
								{
									Value sssThroughput = bssrdfWeight;

									// DL-72 (round 3, this pass): pair every Accumulate
									// (inside RayCasterEnvEscapeMISWeight, if this
									// continuation happens to escape to the global env
									// map) with one AccumulateCount here, at the SAME
									// "sample attempt" granularity the main surface
									// continuation uses -- BEFORE Russian roulette, so
									// a subsequently RR-killed attempt is still counted
									// (OptimalMISAccumulator.h's AccumulateCount doc:
									// "regardless of whether the sample contributed
									// non-zero radiance").  Round 2 removed this call
									// because the paired moment was wrong-shaped; round
									// 3 derives the correct one below and reinstates it.
									// Round-2 review (P2-2): dropped the
									// `PTSurvivalMagnitude(sssThroughput) > NEARZERO` gate --
									// it excluded a zero-throughput BSSRDF exit attempt from
									// the count, the same undercounting pattern fixed at the
									// main surface continuation's `!skipContinuation` gate.
									// A zero-throughput attempt gets no matching
									// `Accumulate()` either way, so counting it here is a
									// correctly COUNTED ZERO, not a phantom sample.
									if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() )
									{
										const_cast<OptimalMISAccumulator*>(rc.pOptimalMIS)->AccumulateCount(
											rast.x, rast.y, kTechniqueBSDF );
									}

									const PathTransportUtilities::RussianRouletteResult rr =
										PathTransportUtilities::EvaluateRussianRoulette(
											depth, rrMinDepth, rrThreshold,
											importance * PTSurvivalMagnitude( sssThroughput ),
											importance, bssrdfSampler.Get1D() );
									if( !rr.terminate )
									{
										if( rr.survivalProb < 1.0 ) {
											sssThroughput = PTDivByScalar( sssThroughput, rr.survivalProb );
										}

										Value cthis = Traits::zero();
										Ray continuationRay = bssrdf.scatteredRay;
										continuationRay.Advance( 1e-8 );

										IRayCaster::RAY_STATE rs2;
										rs2.depth = depth + 2;
										rs2.considerEmission = true;
										rs2.importance = importance * PTSurvivalMagnitude( sssThroughput );
										rs2.bsdfPdf = bssrdf.cosinePdf;
										// DL-74: the BSSRDF exit is sampled from its own
										// cosine density with no guiding anywhere in the
										// path, so the sampling density and the MIS-partner
										// density coincide.
										rs2.bsdfMisPdf = bssrdf.cosinePdf;
										rs2.type = IRayCaster::RAY_STATE::eRayDiffuse;
										rs2.diffuseBounces = diffuseBounces;
										rs2.glossyBounces = glossyBounces;
										rs2.transmissionBounces = transmissionBounces;
										rs2.translucentBounces = nextTranslucentBounces;
										rs2.glossyFilterWidth = glossyFilterWidth;
										// BSSRDF emerges as a diffuse scatter at a
										// non-specular shading point — propagate
										// SMS emission-suppression state so an
										// onwards child ray through glass to a
										// light doesn't re-enable emission.
										rs2.smsPassedThroughSpecular = false;
										rs2.smsHadNonSpecularShading = true;
										// DL-72 / DL-84 (round 4): the FULL vertex-local
										// integrand of this exit sample, `weight *
										// cosinePdf` -- see PTBssrdfTrainedBsdfTimesCos's
										// derivation above.  Round 3 trained `Sw*cos`,
										// which is the same quantity with the
										// area-measure `weightSpatial` dropped; the
										// accumulator's moment is of the INTEGRAND, so
										// that scaled this site's moments arbitrarily.
										// Paired with the AccumulateCount call above,
										// with rs2.bsdfPdf = bssrdf.cosinePdf (unchanged)
										// as the matching density, and with the same
										// `weightSpatial` scale now given to the NEE arm
										// at this vertex (its `neeTrainingScale`
										// argument, a few lines above).
										//
										// ROUND 7: `sssThroughput`, not the pre-roulette
										// `bssrdfWeight`.  The `AccumulateCount` above
										// sits BEFORE Russian roulette, so the moment
										// paired with it has to be the AS-CARRIED one --
										// the realized `E_pre/q` rather than `q*E_pre`.
										// `sssThroughput` IS `bssrdfWeight` until the
										// roulette divides it by `rr.survivalProb` a few
										// lines above, so this is a no-op whenever the
										// roulette did not fire.  Derivation at the
										// ordinary surface continuation's
										// `bsdfTimesCosVal`.
										rs2.bsdfTimesCos = PTRayStateBsdfTimesCos(
											PTBssrdfTrainedBsdfTimesCos( sssThroughput, bssrdf.cosinePdf ) );

										PTCastRay<Tag>( caster, rc, rast, continuationRay,
											cthis, rs2, pRadianceMap, iorStack, tag );

										Value indirect = sssThroughput * cthis;
										if( depth > 0 ) {
											indirect = ClampContribution( indirect,
												stabilityConfig.indirectClamp );
										}
										result = result + throughput * indirect;
									}
								}
							}
						}
					}
				}
			}
		}

		// ============================================================
		// Specular surfaces (no BSDF — use SPF)
		// ============================================================
		if( !pBRDF )
		{
			// GUI render modes P2b `clay_lights`: dead under clay_lights in
			// practice (pBRDF is never null there -- see the acquisition
			// above), kept substituted for consistency in case a future
			// caller reaches this branch some other way.
			const ISPF* pSPF = EffectivePathTracingClayOverride( rc, mClayOverride ) ? pClaySPF :
				( ri.pMaterial ? ri.pMaterial->GetSPF() : 0 );
			if( !pSPF ) {
				break;
			}

			ScatteredRayContainer scattered;
			{
				RISE_PROFILE_PHASE(BSDFScatter);
				RISE_PROFILE_INC(nBSDFScatterCalls);
				PTScatter<Tag>( pSPF, ri.geometric, sampler, scattered, iorStack, tag );
			}

			if( scattered.Count() == 0 ) {
				break;
			}

			// Stochastic single-lobe selection (no path-tree branching at
			// multi-lobe delta vertices).  Branching was excised in 2026-05;
			// matches PBRT/Mitsuba/Arnold/Cycles X.  Pel selects with RGB-max
			// weights (bNM=false), NM with spectral weights (bNM=true) via
			// PTRandomlySelect / PTScatterSelectWeight, so the selection and
			// the selectProb compensation stay in the same domain.  The Pel
			// multi-lobe and single-lobe branches are unified here: for a
			// single lobe selectProb stays 1.0 and the `* (1/selectProb)`
			// factor is an exact multiply by 1.0.
			{
				const Scalar xi = sampler.Get1D();
				const ScatteredRay* pS = PTRandomlySelect<Tag>( scattered, xi );
				if( !pS ) {
					break;
				}

				Scalar selectProb = 1.0;
				if( scattered.Count() > 1 ) {
					Scalar totalKray = 0;
					for( unsigned int li = 0; li < scattered.Count(); li++ ) {
						totalKray += PTScatterSelectWeight<Tag>( scattered[li] );
					}
					const Scalar pSWeight = PTScatterSelectWeight<Tag>( *pS );
					if( totalKray > NEARZERO && pSWeight > NEARZERO ) {
						selectProb = pSWeight / totalKray;
					}
				}
				if( selectProb < NEARZERO ) {
					break;
				}

				IRayCaster::RAY_STATE rs2 = rs;
				rs2.depth = depth + 1;
				// Russian roulette / importance must track the throughput it
				// is predicting, eta^2 included -- an eye ray entering water
				// really does carry 1/n^2 less, and a stale importance only
				// makes RR less efficient, never wrong.
				rs2.importance = importance
					* PTSurvivalMagnitude( PTScatterKray<Tag>( *pS ) )
					* RadianceEtaScale( iorStack, pS->ior_stack ) / selectProb;
				rs2.bsdfPdf = pS->isDelta ? 0 : pS->pdf;
				// DL-74 / DL-103: this is the no-BRDF (SPF-only)
				// continuation, and guiding never runs on it.
				//
				// It does NOT need DL-103's aggregate partner, and the
				// reason is checkable rather than a claim about NEE:
				// `rs2.bsdfPdf` is ALWAYS 0 here, so there is no per-lobe
				// density for an aggregate to disagree with.  Exactly two
				// families reach this branch (`IMaterial::GetBSDF()` null):
				//   * `DielectricMaterial` / `PerfectReflectorMaterial` /
				//     `PerfectRefractorMaterial` -- every lobe they emit
				//     sets `isDelta = true` (DielectricSPF's `scattering`
				//     /HG-warped transmission included), so the line above
				//     stores 0.
				//   * `BioSpecSkinMaterial` / `GenericHumanTissueMaterial`
				//     -- their lobes are non-delta but never assign `.pdf`
				//     at all, so it keeps `ScatteredRay()`'s 0; their
				//     `ISPF::Pdf` is likewise the base-class 0, so the
				//     aggregate would be 0 too.
				// Should a future SPF reach here with a real non-delta
				// density, this line needs PART 3's treatment.
				rs2.bsdfMisPdf = rs2.bsdfPdf;
				rs2.type = PathTracingRayType( *pS );
				// Accurate guides describe the first non-delta interaction the
				// sampled path reached, whether or not transport is allowed to
				// continue beyond it. Capture before bounce-limit termination so
				// maxDiffuseBounce=0 and similar direct-only configurations do not
				// erase a perfectly valid guide surface.
				if constexpr ( Traits::supports_aov ) {
					if( pAOV && !pAOV->valid && !pS->isDelta &&
					    rc.aovPrefilterMode == OidnPrefilter::Accurate )
					{
						pAOV->normal = ri.geometric.vNormal;
						pAOV->albedo = ( ri.pMaterial && ri.pMaterial->GetBSDF() )
							? ri.pMaterial->GetBSDF()->albedo( ri.geometric )
							: RISEPel( 1, 1, 1 );
						pAOV->valid = true;
					}
				}
				// SPF/no-BSDF specular continuation.  Keep emission enabled at the
				// next vertex for BOTH color modes and let the PART1
				// `smsSuppressEmission` predicate (gated by bHadNonSpecularShading)
				// do the suppression.  A camera->glass->light path has no diffuse
				// anchor for SMS to evaluate at, so its emission MUST survive; the
				// NM original instead forced considerEmission=false here, which
				// turned lights seen directly through glass/mirrors black under
				// spectral rendering with SMS on.  Fixed together with the PART3
				// flag tracking below -- see PT_PEL_NM_ASYMMETRY_AUDIT.md #1/#3.
				const bool nextConsiderEmissionSPF = true;
				rs2.considerEmission = nextConsiderEmissionSPF;
				if( PropagateBounceLimits( rs, rs2, *pS, &stabilityConfig ) ) {
					break;
				}

				// eta^2 basic-radiance factor (debt 30).  PT is a
				// RADIANCE-mode walk, so a lobe that moves the ray into a
				// different medium scales the throughput by
				// (eta_before/eta_after)^2; kray carries only Fresnel and
				// Beer's law (ISPF.h contract).  Applied AFTER lobe
				// selection on purpose: the selection CDF inside
				// PTRandomlySelect reads raw kray, so selectProb must stay
				// in that same domain, and E[kray_I * eta_I / p_I] =
				// sum_i kray_i * eta_i regardless.  Identically 1 for every
				// reflection and every non-transmissive lobe.
				const Scalar etaScale = RadianceEtaScale( iorStack, pS->ior_stack );

				// Pel multiplies (throughput * kray) * (1/selectProb); NM
				// multiplies throughput * (krayNM * (1/selectProb)).  The two
				// associativities differ at the ULP level, so preserve each.
				if constexpr ( Traits::is_pel ) {
					throughput = throughput * PTScatterKray<Tag>( *pS ) * ( etaScale / selectProb );
				} else {
					throughput = throughput * ( PTScatterKray<Tag>( *pS ) * ( etaScale / selectProb ) );
				}
				importance = rs2.importance;
				bsdfPdf = rs2.bsdfPdf;
				bsdfMisPdf = rs2.bsdfMisPdf;
				bsdfTimesCos = Traits::zero();
				considerEmission = nextConsiderEmissionSPF;
				rayType = rs2.type;
				diffuseBounces = rs2.diffuseBounces;
				glossyBounces = rs2.glossyBounces;
				transmissionBounces = rs2.transmissionBounces;
				translucentBounces = rs2.translucentBounces;
				glossyFilterWidth = rs2.glossyFilterWidth;

				// Track specular transitions for SMS double-counting prevention
				if( pS->isDelta ) {
					bPassedThroughSpecular = true;
					} else {
						bPassedThroughSpecular = false;
						bHadNonSpecularShading = true;
					}

				if constexpr ( Traits::is_pel ) {
					if( ff ) {
						FF_TRACE( "  depth=%u SCAT-SPF isDelta=%d kray=(%.3e,%.3e,%.3e) pdf=%.3e selProb=%.3e dir=(%.4f,%.4f,%.4f) thr->(%.3e,%.3e,%.3e) psS=%d nsS=%d",
							depth, (int)pS->isDelta,
							pS->kray[0], pS->kray[1], pS->kray[2],
							pS->pdf, selectProb,
							pS->ray.Dir().x, pS->ray.Dir().y, pS->ray.Dir().z,
							throughput[0], throughput[1], throughput[2],
							(int)bPassedThroughSpecular, (int)bHadNonSpecularShading );
					}
				}

				currentRay = pS->ray;
				currentRay.Advance( 1e-8 );

				if( pS->ior_stack ) {
					iorStack = *pS->ior_stack;
				}

				continue;
			}
		}

#ifdef RISE_ENABLE_OPENPGL
		// ============================================================
		// DL-74: ONE guiding distribution per shading point, shared by
		// PART 2's NEE and PART 3's continuation.
		// ============================================================
		// Hoisted here, above PART 2, for three reasons that were three
		// separate defects when this lived inside PART 3:
		//  1. PARTITION.  PART 2's NEE and PART 3's escape/emitter-hit
		//     weights are the two halves of the same MIS pair, so they
		//     must evaluate ONE nominal density (`PTGuidingMisPdf`).
		//  2. ONE STOCHASTIC LOOKUP.  `InitDistribution`'s 1D sample
		//     picks the spatial region stochastically; drawing it twice
		//     could resolve NEE and the continuation to DIFFERENT regions
		//     at the same point.  One draw, one handle, both users.
		//  3. ONE SAMPLER DIMENSION.  The NEE-side setup used to draw an
		//     extra `Get1D()` off the NEE sampler, shifting every
		//     downstream QMC dimension whenever it fired.  Nothing extra
		//     is drawn now, and when guiding is inactive (the gate below
		//     fails on its first clause) NOTHING is drawn at all, so a
		//     guiding-off render's dimension budget is untouched.
		// The gate carries only the VERTEX-level parts of
		// GuidingEffectiveAlpha's eligibility -- a trained field, the
		// configured guiding depth, a nonzero base alpha, and the same
		// `eRaySpecular` incoming-state rejection.  The per-LOBE parts
		// (`isDelta`, glossy half-damping, `GuidingSupportsSurfaceSampling`)
		// stay in PART 3 where the lobe is known; they steer SAMPLING and
		// must not steer the shared nominal density, which is
		// lobe-independent by design.
		// `static thread_local` (the same storage class PART 3 used before
		// this hoist, and BDPTIntegrator's own guiding block uses) so the
		// OpenPGL distribution object is allocated once per thread rather
		// than per shading point.  INVARIANT it depends on: nothing between
		// this initialisation and PART 3's use may re-enter
		// IntegrateFromHit{,NM} on the same thread, or the inner call's
		// distribution would overwrite the outer's.  Audited: PART 2's NEE
		// reaches LightSampler and its shadow/transmittance queries, which
		// dispatch no shader and start no new integrator walk, and the SMS
		// block between them casts visibility rays only.  Re-audit before
		// putting anything shader-dispatching in that window.
		static thread_local GuidingDistributionHandle guideDist;
		PTGuidingMisPdf guidingMis;
		if( rc.pGuidingField && rc.pGuidingField->IsTrained() &&
			depth <= rc.maxGuidingDepth &&
			rc.guidingAlpha > NEARZERO &&
			rs.type != IRayCaster::RAY_STATE::eRaySpecular &&
			rc.pGuidingField->InitDistribution( guideDist,
				ri.geometric.ptIntersection, sampler.Get1D() ) )
		{
			// Unconditional, unlike PART 3's old `eRayDiffuse`-only
			// application -- see PTGuidingMisPdf's doc.
			rc.pGuidingField->ApplyCosineProduct( guideDist,
				GuidingCosineNormal( ri.geometric ) );

			// Same learned-alpha scaling PART 3 applies (Mueller 2017 v2's
			// per-cell sigmoid, 2x so a neutral 0.5 reproduces the fixed-
			// alpha behaviour), clamped to [0,1] for the MIS probability
			// invariant.  No per-lobe damping: NEE cannot know the lobe.
			Scalar alphaNominal = rc.guidingAlpha;
			if( rc.guidingLearnedAlpha ) {
				alphaNominal = rc.guidingAlpha * 2.0 *
					rc.pGuidingField->GetCellAlpha( guideDist );
				if( alphaNominal > 1.0 ) alphaNominal = 1.0;
			}
			guidingMis.Configure( rc.pGuidingField, &guideDist, alphaNominal );
		}
#endif

		// ============================================================
		// PART 2: NEE + SMS at diffuse/glossy surfaces
		// ============================================================
		if( pLS )
		{
			IndependentSampler fallbackSampler( rc.random );
			ISampler& neeSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;

			// P1-c fix (review-p2b): under clay_lights, pBRDF is already
			// pClayBRDF (see the acquisition above) -- the MIS BSDF-
			// sampling pdf must come from the SAME clay lobe (pClayMaterial,
			// whose Pdf()/PdfNM() delegate to pClaySPF), not the authored
			// ri.pMaterial (whose Pdf() could be near-zero/delta for a
			// mirror or dielectric, breaking MIS and making clay output
			// depend on the hidden material).  See ClayNEEMaterial's doc.
			const IMaterial* pNEEMaterial =
				EffectivePathTracingClayOverride( rc, mClayOverride ) ? pClayMaterial : ri.pMaterial;

			Value directAll = PTEvaluateDirectLighting<Tag>(
				pLS, ri.geometric, *pBRDF,
				pNEEMaterial, caster, neeSampler,
				ri.pObject, pCurrentMedium, false, pMediumObject, tag,
#ifdef RISE_ENABLE_OPENPGL
				guidingMis.IsActive() ? &guidingMis : 0,
#else
				0,
#endif
				// DL-74 P2: the MIS-partner aggregate pdf must be evaluated
				// under the stack this vertex is actually standing in -- the
				// same `iorStack` PART 3's `PTEvalPdfAtSurface` uses a few
				// lines below.  `iorStack` is not reassigned between here and
				// that call (only at the very end of the iteration), so the
				// two sides see the identical stack.
				&iorStack,
				// DL-185: fold in the cast-level RR compensation ONLY at
				// the FIRST vertex of this call (see the BSSRDF entry
				// NEE's identical comment above) -- 1.0 (no-op) for every
				// deeper iteration of this loop.
				depth == startDepth ? castRRCompensation_ : Scalar( 1.0 ) );
			directAll = ClampContribution( directAll, stabilityConfig.directClamp );
			// GUI render modes P2b `indirect`: suppress NEE's direct-
			// lighting contribution at the camera-visible vertex only --
			// see SetIndirectOnly's doc.  Still EVALUATED (same RNG
			// consumption / shadow-ray cost as every other mode) so the
			// sampler stream stays in lockstep; only the contribution is
			// zeroed.
			const Value directContrib = throughput * directAll;
			if( !( EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) && depth == 0 ) ) {
				result = result + directContrib;
			} else if( pDirectResult ) {
				*pDirectResult = *pDirectResult + directContrib;
			}
			if constexpr ( Traits::is_pel ) {
				if( ff ) {
					const RISEPel ct = throughput * directAll;
					FF_TRACE( "  depth=%u NEE direct=(%.3e,%.3e,%.3e) thr=(%.3e,%.3e,%.3e) contrib=(%.3e,%.3e,%.3e) result=(%.3e,%.3e,%.3e)",
						depth, directAll[0], directAll[1], directAll[2],
						throughput[0], throughput[1], throughput[2],
						ct[0], ct[1], ct[2],
						result[0], result[1], result[2] );
				}
			}

#ifdef RISE_ENABLE_OPENPGL
			AddPTIGuidingScatteredContribution( guidingSegment, PTGuidingPel( directAll ) );
#endif
		}

		// SMS for caustics through specular surfaces.  GUI render modes P2b
		// `indirect`: deliberately NOT gated on depth==0 -- the
		// task scope for `indirect` is "beauty minus the emission/NEE direct
		// contribution", and a caustic reaching this vertex through a
		// specular chain is a genuinely multi-bounce transport (the light
		// energy already traveled through >=1 specular scatter to arrive
		// here), not the open-air direct connection NEE evaluates.
		if( pSolver )
		{
			const Vector3 woOutgoing = Vector3(
				-ri.geometric.ray.Dir().x,
				-ri.geometric.ray.Dir().y,
				-ri.geometric.ray.Dir().z );

			IndependentSampler fallbackSampler( rc.random );
			ISampler& smsSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;

#if SMS_DIAG_ENABLED
			if constexpr ( Traits::is_pel ) {
				g_smsDiag_evals.fetch_add( 1, std::memory_order_relaxed );
			}
#endif
			// SMS receiver: pass BOTH geometric and shading normals.
			// Shading drives BSDF eval and cosine factor (Veach §5.3.6),
			// geometric drives probe-direction fallback / chain topology.
			// NM uses per-wavelength IOR for dispersion (inside PTEvaluateSMS).
			const PTSMSResult<Tag> sms = PTEvaluateSMS<Tag>(
				pSolver,
				ri.geometric.ptIntersection,
				ri.geometric.vGeomNormal,
				ri.geometric.vNormal,
				ri.geometric.onb,
				ri.pMaterial,
				woOutgoing,
				scene,
				caster,
				smsSampler,
				tag );

			if( sms.valid )
			{
#if SMS_DIAG_ENABLED
				if constexpr ( Traits::is_pel ) {
					g_smsDiag_valid.fetch_add( 1, std::memory_order_relaxed );
				}
#endif
				Value smsContrib = sms.contribution * sms.misWeight;
#if SMS_DIAG_ENABLED
				if constexpr ( Traits::is_pel ) {
					SMSDiag_AddLum( g_smsDiag_sumSmsLumX,
						ColorMath::MaxValue( throughput * smsContrib ) );
				}
#endif
				// Pre-clamp value captured for the Pel firefly trace below
				// (compiled out for NM, which had no SMS trace).
				[[maybe_unused]] const Value smsContribPreClamp = smsContrib;
				smsContrib = ClampContribution( smsContrib, stabilityConfig.directClamp );
				result = result + throughput * smsContrib;
				if constexpr ( Traits::is_pel ) {
					if( ff ) {
						const RISEPel ct = throughput * smsContrib;
						FF_TRACE( "  depth=%u SMS raw=(%.3e,%.3e,%.3e) mis=%.4f preClamp=(%.3e,%.3e,%.3e) postClamp=(%.3e,%.3e,%.3e) thr=(%.3e,%.3e,%.3e) contrib=(%.3e,%.3e,%.3e) result=(%.3e,%.3e,%.3e)",
							depth, sms.contribution[0], sms.contribution[1], sms.contribution[2], sms.misWeight,
							smsContribPreClamp[0], smsContribPreClamp[1], smsContribPreClamp[2],
							smsContrib[0], smsContrib[1], smsContrib[2],
							throughput[0], throughput[1], throughput[2],
							ct[0], ct[1], ct[2],
							result[0], result[1], result[2] );
					}
				}

#ifdef RISE_ENABLE_OPENPGL
				AddPTIGuidingScatteredContribution( guidingSegment, PTGuidingPel( sms.contribution * sms.misWeight ) );
#endif
			}
			else
			{
				if constexpr ( Traits::is_pel ) {
					if( ff ) {
						FF_TRACE( "  depth=%u SMS invalid (no path)", depth );
					}
				}
			}
		}

		// ============================================================
		// PART 3: BSDF sampling (continue path — iterative)
		// ============================================================
		// GUI render modes P2b `clay_lights`: the continuation-ray SPF --
		// substituting clay here (rather than only at the acquisition
		// above) keeps NEE (which reads pBRDF) and the continuation
		// (which reads pSPF) consistent, so the estimator stays energy-
		// consistent under clay_lights the same way it does for any real
		// material's matched BRDF/SPF pair.
		const ISPF* pSPF = EffectivePathTracingClayOverride( rc, mClayOverride ) ? pClaySPF :
			( ri.pMaterial ? ri.pMaterial->GetSPF() : 0 );
		if( !pSPF ) {
			break;
		}

		ScatteredRayContainer scattered;
		{
			RISE_PROFILE_PHASE(BSDFScatter);
			RISE_PROFILE_INC(nBSDFScatterCalls);
			PTScatter<Tag>( pSPF, ri.geometric, sampler, scattered, iorStack, tag );
		}

		if( scattered.Count() == 0 ) {
			break;
		}

		// Stochastic single-lobe selection (no path-tree branching at
		// multi-lobe BSDF vertices).  Branching was excised in 2026-05;
		// matches PBRT/Mitsuba/Arnold/Cycles X conventions.
		//
		// Multi-lobe correction: RandomlySelect picks lobe i with prob
		// max(kray_i)/sum_j max(kray_j).  The unbiased throughput
		// update is `kray_I / selectProb` — without the division, the
		// estimator is biased low at every multi-lobe vertex.
		{
			const Scalar xi = sampler.Get1D();
			const ScatteredRay* pS = PTRandomlySelect<Tag>( scattered, xi );
			if( !pS ) {
				break;
			}

			Scalar selectProb = 1.0;
			if( scattered.Count() > 1 ) {
				Scalar totalKrayMax = 0;
				for( unsigned int li = 0; li < scattered.Count(); li++ ) {
					totalKrayMax += PTScatterSelectWeight<Tag>( scattered[li] );
				}
				const Scalar pSMax = PTScatterSelectWeight<Tag>( *pS );
				if( totalKrayMax > NEARZERO && pSMax > NEARZERO ) {
					selectProb = pSMax / totalKrayMax;
				}
			}
			if( selectProb < NEARZERO ) {
				break;
			}

			Ray traceRay = pS->ray;
			Value scatterThroughput = PTScatterKray<Tag>( *pS ) * ( Scalar( 1 ) / selectProb );
			Scalar effectiveBsdfPdf = pS->isDelta ? 0 : pS->pdf;
			const IORStack* traceIorStack = pS->ior_stack ? pS->ior_stack : &iorStack;

#ifdef RISE_ENABLE_OPENPGL
			// DL-74: `guideDist` was initialised (and cosine-multiplied)
			// ONCE above PART 2 and is shared with NEE -- no second
			// InitDistribution, no second `Get1D()`, no second
			// ApplyCosineProduct.  `guidingMis.IsActive()` already folds
			// in the vertex-level gates (trained field, guiding depth,
			// nonzero base alpha, non-specular arrival); what remains here
			// is only the per-LOBE eligibility, which governs SAMPLING and
			// deliberately does NOT govern the shared nominal MIS density.
			if( guidingMis.IsActive() && GuidingSupportsSurfaceSampling( *pS ) )
			{
				const Scalar alpha = GuidingEffectiveAlpha(
					rc.guidingAlpha, *pS, rs );

				if( alpha > NEARZERO )
				{
					if( rc.guidingSamplingType == eGuidingRIS )
					{
						PathTransportUtilities::GuidingRISCandidate<Value> candidates[2];

						// Candidate 0: BSDF sample (already drawn)
						{
							PathTransportUtilities::GuidingRISCandidate<Value>& c = candidates[0];
							c.direction = pS->ray.Dir();
							c.bsdfEval = PTEvalBSDFAtSurface<Tag>(
								pBRDF, c.direction, ri.geometric, tag, &iorStack );
							c.bsdfPdf = pS->pdf;
							c.guidePdf = rc.pGuidingField->Pdf( guideDist, c.direction );
							c.incomingRadPdf = rc.pGuidingField->IncomingRadiancePdf( guideDist, c.direction );
							c.cosTheta = fabs(
								Vector3Ops::Dot( c.direction, ri.geometric.vNormal ) );
							const Scalar avgBsdf = PTSurvivalMagnitude( c.bsdfEval );
							c.risTarget = PathTransportUtilities::GuidingRISTarget(
								avgBsdf, c.cosTheta, c.incomingRadPdf, alpha );
							c.risPdf = PathTransportUtilities::GuidingRISProposalPdf(
								c.bsdfPdf, c.guidePdf );
							c.risWeight = c.risPdf > NEARZERO ? c.risTarget / c.risPdf : 0;
							c.valid = c.bsdfPdf > NEARZERO && c.risPdf > NEARZERO && avgBsdf > 0;
							if( !c.valid ) {
								c.risWeight = 0;
							}
						}

						// Candidate 1: guide sample
						{
							PathTransportUtilities::GuidingRISCandidate<Value>& c = candidates[1];
							Scalar guidePdf = 0;
							const Point2 xi2d( sampler.Get1D(), sampler.Get1D() );
							c.direction = rc.pGuidingField->Sample( guideDist, xi2d, guidePdf );
							c.guidePdf = guidePdf;

							if( guidePdf > NEARZERO )
							{
								c.bsdfEval = PTEvalBSDFAtSurface<Tag>(
									pBRDF, c.direction, ri.geometric, tag, &iorStack );
								c.bsdfPdf = PTEvalPdfAtSurface<Tag>(
									pSPF, ri.geometric, c.direction, iorStack, tag );
								c.incomingRadPdf = rc.pGuidingField->IncomingRadiancePdf( guideDist, c.direction );
								c.cosTheta = fabs(
									Vector3Ops::Dot( c.direction, ri.geometric.vNormal ) );
								const Scalar avgBsdf = PTSurvivalMagnitude( c.bsdfEval );
								c.risTarget = PathTransportUtilities::GuidingRISTarget(
									avgBsdf, c.cosTheta, c.incomingRadPdf, alpha );
								c.risPdf = PathTransportUtilities::GuidingRISProposalPdf(
									c.bsdfPdf, c.guidePdf );
								c.risWeight = c.risPdf > NEARZERO ? c.risTarget / c.risPdf : 0;
								c.valid = c.bsdfPdf > NEARZERO && avgBsdf > 0;
								if( !c.valid ) {
									c.risWeight = 0;
								}
							}
							else
							{
								c.bsdfEval = Traits::zero();
								c.bsdfPdf = 0;
								c.incomingRadPdf = 0;
								c.cosTheta = 0;
								c.risTarget = 0;
								c.risPdf = 0;
								c.risWeight = 0;
								c.valid = false;
							}
						}

						Scalar risEffectivePdf = 0;
						const Scalar xiRIS = sampler.Get1D();
						const unsigned int sel = PathTransportUtilities::GuidingRISSelectCandidate(
							candidates, 2, xiRIS, risEffectivePdf );

						if( risEffectivePdf > NEARZERO && candidates[sel].valid )
						{
							scatterThroughput = PTMulDiv( candidates[sel].bsdfEval, candidates[sel].cosTheta, risEffectivePdf );
							traceRay = Ray( pS->ray.origin, candidates[sel].direction );
							effectiveBsdfPdf = risEffectivePdf;
							traceIorStack = PathTransportUtilities::GuidedContinuationIORStack(
								*pS, iorStack, ri.geometric, traceRay.Dir() );
						}
						else
						{
							scatterThroughput = Traits::zero();
						}
					}
					else
					{
						// One-sample MIS.  When rc.guidingLearnedAlpha
						// is true, use Müller 2017 v2's per-cell Adam-
						// learned mixing weight σ(θ_cell) — `alpha` is
						// the Cycles-style scatter-type damping factor
						// and learnedCellAlpha ∈ (0,1) (default 0.5)
						// scales it via 2× so initial learned=0.5
						// reproduces the fixed-α (2a) behaviour and
						// learning can push effective up or down.
						// Clamp to [0,1] for the MIS probability
						// invariant.  Adam step is deferred to path
						// completion so f = BSDF·cos·Li uses the
						// actual radiance flowing through the chosen
						// direction.  When false, falls back to the
						// fixed `alpha` from GuidingEffectiveAlpha —
						// reproducible, slightly higher mean σ² in
						// production (~2% measured at 256 SPP).
						Scalar effectiveAlpha = alpha;
						if( rc.guidingLearnedAlpha )
						{
							const Scalar learnedCellAlpha =
								rc.pGuidingField->GetCellAlpha( guideDist );
							effectiveAlpha = alpha * 2.0 * learnedCellAlpha;
							if( effectiveAlpha > 1.0 ) effectiveAlpha = 1.0;
						}
						const Scalar xiG = sampler.Get1D();

						Scalar smplBsdfPdf = 0;
						Scalar smplGuidePdf = 0;
						Scalar smplCombinedPdf = 0;

						if( PathTransportUtilities::ShouldUseGuidedSample( effectiveAlpha, xiG ) )
						{
							Scalar guidePdf = 0;
							const Point2 xi2d( sampler.Get1D(), sampler.Get1D() );
							const Vector3 guidedDir = rc.pGuidingField->Sample( guideDist, xi2d, guidePdf );

							if( guidePdf > NEARZERO )
							{
								const Value fGuided = PTEvalBSDFAtSurface<Tag>(
									pBRDF, guidedDir, ri.geometric, tag, &iorStack );
								const Scalar bsdfPdfGuided = PTEvalPdfAtSurface<Tag>(
									pSPF, ri.geometric, guidedDir, iorStack, tag );
								const Scalar combinedPdf =
									PathTransportUtilities::GuidingCombinedPdf( effectiveAlpha, guidePdf, bsdfPdfGuided );

								if( combinedPdf > NEARZERO )
								{
									const Scalar cosTheta = fabs(
										Vector3Ops::Dot( guidedDir, ri.geometric.vNormal ) );
									scatterThroughput = PTMulDiv( fGuided, cosTheta, combinedPdf );
									traceRay = Ray( pS->ray.origin, guidedDir );
									effectiveBsdfPdf = combinedPdf;
									traceIorStack = PathTransportUtilities::GuidedContinuationIORStack(
										*pS, iorStack, ri.geometric, traceRay.Dir() );
									smplBsdfPdf = bsdfPdfGuided;
									smplGuidePdf = guidePdf;
									smplCombinedPdf = combinedPdf;
								}
								else
								{
									scatterThroughput = Traits::zero();
								}
							}
							else
							{
								scatterThroughput = Traits::zero();
							}
						}
						else
						{
							const Scalar guidePdfForBsdf = rc.pGuidingField->Pdf( guideDist, pS->ray.Dir() );
							const Scalar combinedPdf =
								PathTransportUtilities::GuidingCombinedPdf( effectiveAlpha, guidePdfForBsdf, pS->pdf );

							if( combinedPdf > NEARZERO )
							{
								// DL-42: kray is already f_lobe*cos/pS->pdf for
								// the ONE lobe PTRandomlySelect stochastically
								// chose (ISPF.h's documented kray contract), so
								// reweighting its own pdf to combinedPdf must
								// still divide by selectProb -- the same
								// multi-lobe compensation applied at this
								// function's non-guided initialization above
								// (`scatterThroughput = kray * (1/selectProb)`).
								// DL-42 fixes THIS branch only. The
								// RIS-accepted and guided-direction-accepted
								// branches elsewhere in this block instead
								// re-evaluate the BSDF/PDF through the
								// material's AGGREGATE (all-lobes) interfaces
								// -- that does NOT make them complete,
								// self-contained estimators immune to
								// selectProb: at a multi-lobe surface where
								// GuidingSupportsSurfaceSampling's eligibility
								// gate admits only SOME lobes (non-delta
								// diffuse/reflection), the guided/RIS branches
								// price the material's FULL aggregate value()
								// while the guiding machinery only ever
								// proposed/weighted the eligible subset, and
								// ineligible lobes are additionally priced a
								// second time through the ordinary
								// kray/selectProb path below this block --
								// see DL-67 for the derivation and the
								// remaining inconsistency between all three
								// branches (this one, RIS-accepted, and
								// guided-direction-accepted).
								scatterThroughput = PTScatterKray<Tag>( *pS ) * (pS->pdf / (selectProb * combinedPdf));
								effectiveBsdfPdf = combinedPdf;
								smplBsdfPdf = pS->pdf;
								smplGuidePdf = guidePdfForBsdf;
								smplCombinedPdf = combinedPdf;
							}
						}

						// Defer Adam update until path completion (only
						// at root: recursive split branches use a
						// separate result frame and would attribute
						// radiance to the wrong vertex).  Skipped when
						// learning is disabled — keeps the queue
						// empty so the apply-pending block is a no-op.
						if( rc.guidingLearnedAlpha && guidingRootRay &&
							smplCombinedPdf > NEARZERO )
						{
							PTIPendingGuideUpdate u;
							u.cellId           = rc.pGuidingField->GetCellId( guideDist );
							u.bsdfPdf          = smplBsdfPdf;
							u.guidePdf         = smplGuidePdf;
							u.combinedPdf      = smplCombinedPdf;
							u.resultBefore     = PTGuidingLuminance( result );
							u.throughputBefore = PTGuidingLuminance( throughput );
							GetPTIPendingGuideUpdates().push_back( u );
						}
					}
				}
			}

			(void)useGuidingPathSegments;  // Used in full guiding implementation
#endif // RISE_ENABLE_OPENPGL

			// Radiance follows the resolved continuation medium, whether the
			// direction came from the SPF or guiding. A same-side exit retains
			// the SPF pop; an opposite-side guide sample keeps the input stack.
			{
				const Scalar etaScale = ( traceIorStack != &iorStack )
					? RadianceEtaScale( iorStack, traceIorStack )
					: Scalar( 1 );
				if( etaScale != Scalar( 1 ) ) {
					scatterThroughput = scatterThroughput * etaScale;
				}
			}

			// Guide capture is a property of the selected interaction, not of
			// Russian-roulette or bounce-limit survival. Keeping it before both
			// termination decisions prevents direct-only and RR-terminated paths
			// from producing transport-correlated holes in Accurate AOVs.
			if constexpr ( Traits::supports_aov ) {
				if( pAOV && !pAOV->valid && !pS->isDelta &&
				    rc.aovPrefilterMode == OidnPrefilter::Accurate )
				{
					pAOV->normal = ri.geometric.vNormal;
					pAOV->albedo = pBRDF ? pBRDF->albedo( ri.geometric )
					                     : RISEPel( 1, 1, 1 );
					pAOV->valid = true;
				}
			}

			bool skipContinuation = PTSurvivalMagnitude( scatterThroughput ) <= NEARZERO;

			// Optimal MIS training (round-2 review, P2-2): count every
			// NON-DELTA attempt, INCLUDING a zero-throughput draw (a
			// below-horizon or zero-value lobe sample) -- gating this on
			// `!skipContinuation` excluded exactly those, while
			// `LightSampler.cpp`'s NEE arm counts every attempt including
			// geometry-rejected zero draws, and
			// `OptimalMISAccumulator.h`'s own `AccumulateCount` doc says
			// "regardless of whether the sample contributed [non-zero
			// radiance]".  A zero-throughput attempt already carries no
			// matching `Accumulate()` call (the `if(skipContinuation)
			// break;` below stops before any escape/NEE can fire), so it
			// is correctly a COUNTED ZERO once counted here -- consistent
			// with the realized-moment convention (a killed/zero attempt
			// is a counted zero).  Leaving it uncounted instead estimates
			// `E[.]/P(throughput>0)`, inflating `M_bsdf` by `1/P`.
			if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() && !pS->isDelta )
			{
				const_cast<OptimalMISAccumulator*>(rc.pOptimalMIS)->AccumulateCount(
					rast.x, rast.y, kTechniqueBSDF );
			}

#ifdef RISE_ENABLE_OPENPGL
			// Capture pre-RR throughput so the guiding segment records
			// scatteringWeight = bsdf*cos/pdf (without RR amplification).
			// OpenPGL applies RR separately via russianRouletteSurvivalProbability.
			// NOTE (DL-84 round 7): this is OpenPGL's convention and OpenPGL's
			// alone.  The optimal-MIS moment below deliberately uses the
			// POST-RR `scatterThroughput` -- see its own comment.
			const Value preRRScatterThroughput = scatterThroughput;
			Scalar rrSurvivalProb = 1.0;
#endif

			// Russian roulette
			if( !skipContinuation )
			{
				const PathTransportUtilities::RussianRouletteResult rr =
					PathTransportUtilities::EvaluateRussianRoulette(
						depth, rrMinDepth, rrThreshold,
						importance * PTSurvivalMagnitude( scatterThroughput ),
						importance,
						sampler.Get1D() );
				if( rr.terminate ) {
					skipContinuation = true;
				} else if( rr.survivalProb < 1.0 ) {
					scatterThroughput = PTDivByScalar( scatterThroughput, rr.survivalProb );
#ifdef RISE_ENABLE_OPENPGL
					rrSurvivalProb = rr.survivalProb;
#endif
				}
			}

			// THE REALIZED-MOMENT CONVENTION (DL-84 round 7, docs/
			// DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md "Round 7" -- which
			// RETRACTS round 6's opposite ruling).
			//
			// `OptimalMISAccumulator::Solve()` weights each technique by
			// `1/M_i`, so `M_i` is the second moment of technique `i`'s
			// OWN single-sample, MIS-UNWEIGHTED estimator (the film's
			// per-technique MIS weight `w_i` is never part of `M_i` --
			// see `OptimalMISAccumulator.h`'s own header comment and
			// `LightSampler.cpp`, which trains `contrib` before
			// `contrib *= w`), evaluated under that technique's EFFECTIVE
			// (RR-defective) density `p~ = q * p`.  Russian roulette does
			// not sit outside that estimator: it makes the density
			// defective, and the surviving sample is compensated by
			// `1/q`.  Hence
			//
			//     M_bsdf = integral f^2 / (p q) = E_pre / q
			//
			// and the estimator of it -- accumulate the AS-CARRIED (post-RR)
			// numerator for survivors, count EVERY attempt including the
			// RR-killed ones (a counted zero) -- is exactly what this site
			// does by reading the post-RR `scatterThroughput` while its
			// `AccumulateCount` above sits BEFORE the roulette.
			//
			// Round 6 replaced `scatterThroughput` here with the PRE-RR
			// value on the argument that RR is "a separate later estimator";
			// that combination (pre-RR numerator over an all-attempts
			// denominator) estimates `q * E_pre`, i.e. the realized moment
			// scaled by `q^2` -- neither of the two defensible conventions.
			// Quadrature over two-technique toys
			// (`tests/OptimalMISTrainingSitesTest.cpp`,
			// `RunAlphaQualityCheck`) puts the realized moment at the lowest
			// combined variance in every configuration and round 6's form at
			// the highest (+9% to +984%).  Round 6's supporting claim that
			// "NEE undergoes no RR" is also false: `LightSampler`'s
			// mesh-luminary arm has its own `light_rr_threshold` roulette,
			// now trained under the SAME realized-moment convention.
			//
			// Variance-only either way -- `alpha` cannot bias the rendered
			// radiance, only how the budget is split between techniques.
			const Value bsdfTimesCosVal = pS->isDelta ? Traits::zero() :
				PTBsdfTimesCos( scatterThroughput, effectiveBsdfPdf );

			// DL-74 / DL-103: the nominal MIS-partner density for the
			// direction actually being traced.
			//
			// DL-103 (docs/DL103_PT_ESCAPE_MIS_PARTNER.md): the partner is
			// the material's AGGREGATE `ISPF::Pdf()` in BOTH branches, not
			// just the guided one.  `LightSampler`'s four NEE arms have
			// always weighted against that aggregate
			// (`pMaterial->Pdf(...)`, which forwards to this same
			// function); with guiding inactive this side used to store the
			// SELECTED lobe's own `pS->pdf` instead.  Those coincide at a
			// single-lobe SPF, and are different functions of direction at
			// a multi-lobe one (`SchlickSPF`, `IsotropicPhongSPF`,
			// `PolishedSPF`, the two Ward SPFs, `CompositeSPF`), so
			// `w_bsdf(w) + w_nee(w) != 1` in ordinary, un-guided,
			// default-configuration PT.  Same direction, same live
			// `iorStack` (DL-74 P2), same `ri.geometric` and therefore the
			// same `glossyFilterWidth` -- one function of omega on both
			// sides, which is the whole requirement.
			//
			// `bsdfPdf` is deliberately NOT changed: it stays the TRUE
			// density this direction was drawn from (the throughput
			// denominator and the optimal-MIS `f^2/pdf^2` divisor).  The
			// two roles are two fields -- that is DL-74's design.
			//
			// A delta lobe keeps 0 (no partner exists); see the field
			// comment below.
			Scalar misBsdfPdf = 0;
			if( !pS->isDelta )
			{
				const Scalar aggregatePdf = PTEvalPdfAtSurface<Tag>(
					pSPF, ri.geometric, traceRay.Dir(), iorStack, tag );
#ifdef RISE_ENABLE_OPENPGL
				if( guidingMis.IsActive() )
				{
					misBsdfPdf = guidingMis.Eval( traceRay.Dir(), aggregatePdf );
				}
				else
#endif
				// DL-41 guard, guiding-inactive branch only.  An SPF that
				// emits a non-delta lobe its own `Pdf()` does not cover
				// reads 0 at a direction the technique really did generate.
				// (`TranslucentSPF`'s two Phong `cos^N` lobes WERE the
				// documented case; DL-41 closed 2026-09-18 and its
				// aggregate now covers both, so the guard has NO KNOWN
				// production inhabitant today -- it is kept because the
				// alternative is silently wrong for the next SPF that
				// acquires the property, and because it costs one
				// comparison.)  Handing 0 to both
				// sides would mean "no BSDF-side partner exists" -- weight
				// 1 on BOTH, a full double count, strictly worse than the
				// pre-DL-103 asymmetry.  Fall back to the lobe's own
				// density there, which reproduces the pre-DL-103 behaviour
				// exactly at those SPFs and nowhere else.  (Under guiding
				// the blend is evaluated UNCONDITIONALLY instead -- DL-74
				// round 4: the mixture's guide term reaches those
				// directions even when the material's own pdf does not.)
				{
					misBsdfPdf = aggregatePdf > 0 ? aggregatePdf : effectiveBsdfPdf;
				}
			}

			// Per-type bounce limits
			IRayCaster::RAY_STATE rs2 = rs;
			rs2.depth = depth + 2;
			rs2.importance = importance * PTSurvivalMagnitude( scatterThroughput );
			// DL-74 (docs/DL74_ENV_NEE_GUIDING_PARTITION.md): the two roles
			// of the guided pdf, kept in two fields.
			//  * `bsdfPdf` stays `effectiveBsdfPdf` -- the density this
			//    direction was really drawn from and the one
			//    `scatterThroughput` (and therefore `bsdfTimesCosVal`
			//    above) was divided by, so the optimal-MIS second moment
			//    `(f/p)^2` is the true squared contribution.  DL-84
			//    round 7: `scatterThroughput` additionally carries the
			//    `1/q` Russian-roulette compensation, which is deliberate
			//    -- RR is part of the BSDF technique's EFFECTIVE density,
			//    see `bsdfTimesCosVal`'s own derivation above.
			//  * `bsdfMisPdf` is the lobe-independent nominal density
			//    LightSampler's NEE arms evaluate for the same direction.
			//    Under guiding that is `alpha_nom*guide + (1-alpha_nom)*
			//    p_aggregate`; with guiding inactive it is `p_aggregate`
			//    itself (DL-103 -- it used to be the selected lobe's own
			//    `effectiveBsdfPdf`, which is a DIFFERENT function of
			//    direction at a multi-lobe SPF).
			// A delta lobe keeps 0 in BOTH fields: it has no MIS partner
			// (NEE cannot sample through it) and no density to train from.
			// The aggregate pdf is evaluated at the FINAL `traceRay`
			// direction, which is the guided/RIS direction when one
			// replaced the lobe's own.
			rs2.bsdfPdf = effectiveBsdfPdf;
			rs2.bsdfMisPdf = misBsdfPdf;
			rs2.bsdfTimesCos = PTRayStateBsdfTimesCos( bsdfTimesCosVal );
			rs2.type = PathTracingRayType( *pS );

			if( PropagateBounceLimits( rs, rs2, *pS, &stabilityConfig ) ) {
				skipContinuation = true;
			}

			bool nextConsiderEmission = true;
			if( pS->isDelta && bSMSEnabled ) {
				nextConsiderEmission = false;
			}

#ifdef RISE_ENABLE_OPENPGL
			if( guidingSegment && !skipContinuation )
			{
				const Scalar segEta =
					traceIorStack->top() > NEARZERO ? traceIorStack->top() : Scalar( 1 );
				const Scalar segRoughness = pS->isDelta ?
					Scalar( 0.0 ) :
					( pS->type == ScatteredRay::eRayDiffuse ?
						Scalar( 1.0 ) :
						Scalar( 0.5 ) );
				SetPTIGuidingContinuation(
					guidingSegment,
					traceRay.Dir(),
					effectiveBsdfPdf,
					PTGuidingPel( preRRScatterThroughput ),
					pS->isDelta,
					rrSurvivalProb,
					segEta,
					segRoughness );
			}
#endif

			if( skipContinuation ) {
				break;
			}

			// Update iterative state
			throughput = throughput * scatterThroughput;
			importance = rs2.importance;
			bsdfPdf = effectiveBsdfPdf;
			bsdfMisPdf = misBsdfPdf;
			bsdfTimesCos = bsdfTimesCosVal;
			considerEmission = nextConsiderEmission;
			rayType = rs2.type;
			diffuseBounces = rs2.diffuseBounces;
			glossyBounces = rs2.glossyBounces;
			transmissionBounces = rs2.transmissionBounces;
			translucentBounces = rs2.translucentBounces;
			glossyFilterWidth = rs2.glossyFilterWidth;

			// Track specular transitions for SMS double-counting prevention.
			// Without this, diffuse-floor → BSDF-sample → glass-chain →
			// light paths slip through the emission suppression at the
			// light: the suppression check requires
			// `bPassedThroughSpecular && bHadNonSpecularShading` and the
			// latter was never set, so `considerEmission=true` at the
			// light + `bsdfPdf=0` from the last delta gave MIS weight
			// 1.0 and full emission was accumulated — a deterministic
			// firefly contribution of hundreds of luminance units per
			// sample at any pixel whose random BSDF sequence found this
			// path.
			// Tracked for BOTH color modes.  With the SPF section now keeping
			// emission enabled on camera->glass->light (asymmetry #1 fix), NM
			// relies on this same flag predicate to suppress the
			// diffuse->glass->light double-count exactly as Pel does.  Were #1
			// fixed while these flags stayed Pel-only, bHadNonSpecularShading
			// would never latch for NM, smsSuppressEmission would stay false,
			// and the double-count would return as fireflies.  See
			// PT_PEL_NM_ASYMMETRY_AUDIT.md #1/#3.
			if( pS->isDelta ) {
				bPassedThroughSpecular = true;
			} else {
				bPassedThroughSpecular = false;
				bHadNonSpecularShading = true;
			}

			currentRay = traceRay;
			currentRay.Advance( 1e-8 );

			if( traceIorStack != &iorStack ) {
				iorStack = *traceIorStack;
			}

#ifdef RISE_ENABLE_OPENPGL
			// Training sample collection would go here
			// (deferred: collect after next intersection hit/miss)
#endif
		}
	}

	if constexpr ( Traits::is_pel ) {
		if( ff ) {
			const Scalar lum = ColorMath::MaxValue( result );
			FF_TRACE( "=== END SAMPLE %lu result=(%.3e,%.3e,%.3e) maxLum=%.3e ===",
				ffSample, result[0], result[1], result[2], lum );
		}
	}

#ifdef RISE_ENABLE_OPENPGL
	// Apply pending Adam updates from this path's guide samples.
	// f at vertex i = lum(deltaResult_i) · combinedPdf_i / lum(throughputBefore_i)
	// where deltaResult_i = lum(result) - lum(resultBefore_i).
	if( guidingRootRay && rc.pGuidingField )
	{
		auto& pending = GetPTIPendingGuideUpdates();
		if( !pending.empty() )
		{
			const Scalar resultEndLum = PTGuidingLuminance( result );
			for( const PTIPendingGuideUpdate& u : pending )
			{
				const Scalar deltaResult = resultEndLum - u.resultBefore;
				if( u.throughputBefore > NEARZERO && deltaResult > 0 )
				{
					const Scalar f = deltaResult * u.combinedPdf / u.throughputBefore;
					rc.pGuidingField->UpdateCellAlpha(
						u.cellId, u.bsdfPdf, u.guidePdf, f, u.combinedPdf, 0.01 );
				}
			}
			pending.clear();
		}
	}
#endif

	return result;
}


//////////////////////////////////////////////////////////////////////
// IntegrateFromHit — RGB entry point (thin forwarder to the templated
// body).  Public; called by PathTracingShaderOp::PerformOperation and by
// IntegrateRay via IntegrateFromHitForTag<PelTag>.
//////////////////////////////////////////////////////////////////////
RISEPel PathTracingIntegrator::IntegrateFromHit(
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const RayIntersection& firstHit,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const IRadianceMap* pRadianceMap,
	unsigned int startDepth,
	const IORStack& initialIorStack,
	Scalar bsdfPdf_,
	const RISEPel& bsdfTimesCos_,
	bool considerEmission_,
	Scalar importance_,
	IRayCaster::RAY_STATE::RayType rayType_,
	unsigned int diffuseBounces_,
	unsigned int glossyBounces_,
	unsigned int transmissionBounces_,
	unsigned int translucentBounces_,
	unsigned int volumeBounces_,
	Scalar glossyFilterWidth_,
	bool smsPassedThroughSpecular_,
	bool smsHadNonSpecularShading_,
	PixelAOV* pAOV,
	Scalar bsdfMisPdf_,
	Scalar castRRCompensation_
	) const
{
	return IntegrateFromHitTemplated<PelTag>(
		rc, rast, firstHit, scene, caster, sampler, pRadianceMap,
		startDepth, initialIorStack, bsdfPdf_, bsdfTimesCos_,
		considerEmission_, importance_, rayType_, diffuseBounces_,
		glossyBounces_, transmissionBounces_, translucentBounces_,
		volumeBounces_, glossyFilterWidth_, smsPassedThroughSpecular_,
		smsHadNonSpecularShading_, pAOV, nullptr, PelTag{}, bsdfMisPdf_,
		castRRCompensation_ );
}


//////////////////////////////////////////////////////////////////////
// IntegrateFromHitForTag — tag-dispatched delegation to the (non-template)
// IntegrateFromHit / IntegrateFromHitNM, used by IntegrateRayTemplated for
// the medium-scatter continuation and the surface hand-off.  The SMS
// emission-suppression flags are always false (camera-ray entry), matching
// every original IntegrateRay* call site.
//////////////////////////////////////////////////////////////////////
template<class Tag>
typename SpectralValueTraits<Tag>::value_type
PathTracingIntegrator::IntegrateFromHitForTag(
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const RayIntersection& firstHit,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const IRadianceMap* pRadianceMap,
	unsigned int startDepth,
	const IORStack& initialIorStack,
	Scalar bsdfPdf,
	const typename SpectralValueTraits<Tag>::value_type& bsdfTimesCos,
	bool considerEmission,
	Scalar importance,
	IRayCaster::RAY_STATE::RayType rayType,
	unsigned int diffuseBounces,
	unsigned int glossyBounces,
	unsigned int transmissionBounces,
	unsigned int translucentBounces,
	unsigned int volumeBounces,
	Scalar glossyFilterWidth,
	PixelAOV* pAOV,
	const Tag& tag,
	Scalar bsdfMisPdf
	) const
{
	if constexpr ( SpectralValueTraits<Tag>::is_pel )
	{
		return IntegrateFromHit( rc, rast, firstHit, scene, caster, sampler,
			pRadianceMap, startDepth, initialIorStack, bsdfPdf, bsdfTimesCos,
			considerEmission, importance, rayType, diffuseBounces, glossyBounces,
			transmissionBounces, translucentBounces, volumeBounces, glossyFilterWidth,
			false, false, pAOV, bsdfMisPdf );
	}
	else
	{
		return IntegrateFromHitNM( rc, rast, firstHit, tag.nm, scene, caster, sampler,
			pRadianceMap, startDepth, initialIorStack, bsdfPdf, bsdfTimesCos,
			considerEmission, importance, rayType, diffuseBounces, glossyBounces,
			transmissionBounces, translucentBounces, volumeBounces, glossyFilterWidth,
			false, false, pAOV, bsdfMisPdf );
	}
}


//////////////////////////////////////////////////////////////////////
// IntegrateRayTemplated — shared body of IntegrateRay / IntegrateRayNM.
//
// Intersects the camera ray, handles first-bounce medium transport,
// then delegates to IntegrateFromHit(NM) for the iterative path loop.
// Tag = PelTag (RGB) or NMTag (single wavelength).  HWSS is the separate
// hero-bundle IntegrateRayHWSS.
//////////////////////////////////////////////////////////////////////
template<class Tag>
typename SpectralValueTraits<Tag>::value_type
PathTracingIntegrator::IntegrateRayTemplated(
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const Ray& cameraRay,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const IRadianceMap* pRadianceMap,
	PixelAOV* pAOV,
	typename SpectralValueTraits<Tag>::value_type* pDirectResult,
	const Tag& tag
	) const
{
	using Traits = SpectralValueTraits<Tag>;
	using Value = typename Traits::value_type;

	IORStack iorStack( 1.0 );
	// Seed from the camera-ray origin: if the camera sits inside a
	// dielectric (submerged camera, camera inside a medium volume), the
	// first boundary crossing must see bFromInside==true or the
	// DielectricSPF wrong-side test drops the transmission lobe entirely.
	// Free-space cameras: the probe finds no enclosing objects, no-op.
	// Mirrors the eye-subpath seeding in BDPTIntegrator (GenerateEyeSubpath).
	IORStackSeeding::SeedFromPoint( iorStack, cameraRay.origin, scene );
	sampler.StartStream( 16 );

	// Intersect camera ray
	RayIntersection ri( cameraRay, rast );
	scene.GetObjects()->IntersectRay( ri, true, true, false );
	if constexpr ( Traits::supports_aov ) {
		// Primary depth is independent of Accurate-mode albedo/normal
		// traversal.  Never replace this camera-ray range with a later
		// bounce segment.
		if( pAOV ) {
			pAOV->primaryDepthCaptured = true;
			pAOV->depth = ri.geometric.bHit ? ri.geometric.range : Scalar( 0 );
		}
	}

	// Extract first-hit AOV data for the denoiser (Fast prefilter mode).
	// For delta / transparent surfaces (GetBSDF()==NULL) use white albedo
	// per OIDN documentation: those surfaces have no diffuse signature
	// and the beauty pass is pure illumination.
	//
	// Accurate prefilter mode SKIPS this hook and instead records inside
	// IntegrateFromHit at the first vertex where the shader's scatter
	// was non-delta (per-sample via ScatteredRay::isDelta).  Glass /
	// mirror are walked through naturally; rough dielectrics record at
	// the rough surface or behind it depending on each sample's Fresnel
	// decision.  See docs/OIDN.md (OIDN-P1-1) for the design.
	// AOV recording is compiled in for both AOV-capable tags: Pel and NM.
	// Callers that did not request auxiliaries pass a null PixelAOV.
	if constexpr ( Traits::supports_aov )
	{
		const bool aovUseFirstHit = ( rc.aovPrefilterMode == OidnPrefilter::Fast );
		if( pAOV && ri.geometric.bHit && aovUseFirstHit )
		{
			RayIntersectionGeometric aovGeom( ri.geometric );
			if( ri.pModifier ) ri.pModifier->Modify( aovGeom );
			pAOV->normal = aovGeom.vNormal;
			// GUI render modes P2b `clay_lights` (review-p2c P2-d fix):
			// under mClayOverride every surface's REFLECTANCE is the
			// substituted clay BRDF (see SetClayOverride's doc) -- the
			// albedo AOV captured here for OIDN must match, or the
			// DENOISED clay output reacquires the authored material's
			// colouration via OIDN's albedo-guided filtering, defeating
			// the mode's material-independence contract exactly where
			// users look at it (the variant pipeline runs with OIDN on).
			// This is the FAST-prefilter hook (camera ray's first hit,
			// evaluated before IntegrateFromHit's loop ever substitutes
			// pBRDF) -- the ACCURATE-prefilter hook inside the loop
			// already reads the loop-local `pBRDF`, which IS already
			// clay-substituted there, so only this site needed the fix.
			pAOV->albedo = EffectivePathTracingClayOverride( rc, mClayOverride ) ? pClayBRDF->albedo( aovGeom ) :
				( ( ri.pMaterial && ri.pMaterial->GetBSDF() )
					? ri.pMaterial->GetBSDF()->albedo( aovGeom )
					: RISEPel( 1, 1, 1 ) );
			pAOV->valid = true;
		}
	}

	// Medium transport for first bounce
	const IObject* pMediumObject = 0;
	const IMedium* pCurrentMedium = MediumTracking::GetCurrentMediumWithObject(
		iorStack, &scene, pMediumObject );

	// Residual transmittance along an escape segment.  When the camera
	// ray escapes the scene through a medium (no scatter, no surface hit)
	// the env radiance below must be attenuated by the medium it crossed
	// (PBRT-v4 VolPathIntegrator: beta *= T_maj before the `if (!si)`
	// infinite-light branch).  Stays 1 (no-op) in vacuum.
	Value escapeTr = PTValueOne<Tag>();

	if( pCurrentMedium )
	{
		const Scalar maxDist = ri.geometric.bHit ? ri.geometric.range : RISE_INFINITY;
		const LightSampler* pLS = caster.GetLightSampler();
		IndependentSampler mediumSampler( rc.random );
		const MediumSampleOutcome mso = PTSampleMediumDistance<Tag>(
			pCurrentMedium, cameraRay, maxDist, pLS, mediumSampler, tag );
		const Scalar t_m = mso.t;
		const bool scattered = mso.scattered;

		if( mso.zeroContrib )
		{
			// Equiangular strategy sampled a zero-density / out-of-bounds
			// point.  Scatter-measure sample with zero weight — do not
			// fall through to surface shading.
			return Traits::zero();
		}

		if( scattered )
		{
			// ============================================================
			// CAMERA-RAY VOLUMETRIC WALK
			//
			// The camera ray scattered in a participating medium before
			// reaching any surface.  This is the medium analogue of the
			// `if( scattered && volumeBounces < maxVolumeBounce )` block in
			// IntegrateFromHitTemplated, and it exists as a separate site
			// only because that loop is entered FROM A HIT: there is no
			// RayIntersection to hand it until this walk finds a surface.
			//
			// It used to handle exactly ONE scatter -- NEE, sample the
			// phase function, then either delegate to
			// IntegrateFromHitForTag (the continuation hit a surface) or
			// TERMINATE with a deterministic Beer-Lambert escape factor
			// times the environment.  That terminal branch dropped the
			// "scatter AGAIN" event entirely, so the estimator lost the
			// whole multiple-scatter tail: order tau*tau' of the signal,
			// which is ~0.4 % on a thin column but 12 % on a tau = 0.4 fog
			// box with the camera inside it (VolumeEnvFurnaceTest cells
			// 1-3 vs 4-6).  The walk below continues instead.
			//
			// ESTIMATOR.  The ordinary volumetric random walk, over scatter
			// events k = 1, 2, ...:
			//
			//   T_1     = Tr(camera segment) * sigma_s / p_dist       (medWeight)
			//   L      += T_k * Ld_NEE(x_k)                                 (a)
			//   T_k    *= phase(wo,wi) / phasePdf   [then RR compensation]  (b)
			//   trace the phase-sampled ray from x_k:
			//     surface hit -> L += T_k * IntegrateFromHitForTag(...)     (d)
			//     miss        -> sample the medium along it once more:
			//        no scatter -> L += T_k * (Tr/pSurvival) * w_phase * L_env  (c)
			//        scatter    -> T_{k+1} = T_k * medWeight, loop with k+1
			//
			// (c) has the SAME EXPECTATION as the deterministic escape it
			// replaces -- for a bounded medium the no-scatter event has
			// probability Tr and weight Tr/pSurvival == 1, so E[(c)] is the
			// old `T_k * Tr * env` exactly.  The whole of the fix is that
			// the complementary event (probability 1 - Tr) now continues
			// the walk instead of being discarded.
			//
			// MIS INVARIANT -- holds at EVERY k, which is what makes this a
			// furnace rather than an approximation.  (a) is env-NEE,
			// MIS-weighted against the phase pdf inside
			// MediumTransport::EvaluateInScattering; (c) is that same
			// strategy's phase-sampled partner and carries the
			// complementary weight (full derivation at the escape site
			// below).  Extending the walk to N scatters therefore means
			// extending the PAIR to every scatter vertex, not just the
			// first -- a continuation that re-scattered and then escaped is
			// weighted against the pdf of the LAST phase sample (`walkPdf`),
			// which is the density the NEE at that same last vertex weighed
			// itself against.
			//
			// LOOP INVARIANTS:
			//   `result`      radiance estimated so far.
			//   `throughput`  path weight from the camera through the LAST
			//                 phase sample -- the factor multiplying
			//                 anything `walkRay` goes on to see.
			//   `walkRay`     the ray currently being followed; `walkMso` /
			//                 `walkT` are the medium-sampling outcome along
			//                 it, always with `scattered == true` at the top
			//                 of the loop body.
			//   `walkPdf`     solid-angle pdf of `walkRay`'s direction (0 on
			//                 the camera segment, which has no phase sample
			//                 behind it and never reaches the env branch).
			//   `volumeBounces` scatter events already COMPLETED.
			//   `pCurrentMedium` is LOOP-INVARIANT.  The walk only continues
			//                 while the ray misses ALL geometry, so no
			//                 boundary is ever crossed and the IOR stack --
			//                 hence MediumTracking's answer -- cannot change.
			//                 The moment a surface is hit the walk hands off
			//                 to IntegrateFromHitForTag, which re-derives the
			//                 medium for itself.
			//
			// TERMINATION.  `stabilityConfig.maxVolumeBounce` (64) and
			// Russian roulette, both driven exactly as the main loop drives
			// them: `EvaluateRussianRoulette( depth + volumeBounces, ... )`
			// with depth == 0 on the camera segment, and `importance` == 1
			// (the camera-entry value every IntegrateFromHit* call site
			// passes).  At the bounce cap the walk falls back to the old
			// deterministic Beer-Lambert escape rather than dropping the
			// tail, so the cap degrades to the PREVIOUS behaviour instead
			// of to black.
			//
			// INDIRECT-ONLY ROUTING.  Scatter 1 is the camera-visible
			// vertex: its NEE and its phase-sampled env partner are the
			// "direct light at the camera-visible point" that indirect-only
			// must suppress (see SetIndirectOnly's doc and the original
			// single-scatter comment this block replaces).  Scatters 2..N
			// are genuine MULTIPLE scattering -- indirect by construction --
			// and are kept, mirroring the main loop, whose volume NEE is
			// likewise ungated.  Both are still EVALUATED in every mode so
			// the sampler stream stays in lockstep; only the routing of the
			// contribution differs.
			// ============================================================
			const bool bIndirectOnly = EffectivePathTracingIndirectOnly( rc, mIndirectOnly );
			const unsigned int rrMinDepth = stabilityConfig.rrMinDepth;
			const Scalar rrThreshold = stabilityConfig.rrThreshold;

			Value result = Traits::zero();
			Value throughput = PTValueOne<Tag>();
			Ray walkRay = cameraRay;
			MediumSampleOutcome walkMso = mso;
			Scalar walkT = t_m;
			Scalar walkPdf = 0;
			unsigned int volumeBounces = 0;

			for( ;; )
			{
				//
				// --- scatter event k == volumeBounces + 1 ---------------
				//
				const Point3 scatterPt = walkRay.PointAtLength( walkT );
				const Vector3 wo = walkRay.Dir();
				const PTMediumScatter<Tag> coeff = PTGetMediumScatter<Tag>( pCurrentMedium, scatterPt, tag );
				const Value Tr = PTEvalTransmittance<Tag>( pCurrentMedium, walkRay, walkT, tag );

				Value medWeight = Traits::zero();
				if( walkMso.useExplicitThroughput && walkMso.combinedPdf > 0 )
				{
					// Equiangular-MIS throughput: Tr * sigma_s / combinedPdf.
					medWeight = PTDivByScalar( Tr * coeff.sigma_s, walkMso.combinedPdf );
				}
				else if( coeff.sigmaTReduced > 0 )
				{
					const Scalar Tr_scalar = PTTrReduced( Tr );
					if( Tr_scalar > 0 ) {
						medWeight = PTDivByScalar( Tr * coeff.sigma_s,
							coeff.sigmaTReduced * Tr_scalar );
					}
				}

				if( PTPositiveMagnitude( medWeight ) <= 0 ) {
					// Degenerate scatter vertex.  At k == 1 `result` is still
					// zero, so this is the original `return Traits::zero()`.
					return result;
				}

				throughput = throughput * medWeight;

				// NEE at the scatter point.
				if( pLS )
				{
					Value Ld = PTEvaluateInScattering<Tag>(
						scatterPt, wo, pCurrentMedium, caster, pLS,
						sampler, rast, pMediumObject, tag );
					if( PTPositiveMagnitude( Ld ) > 0 )
					{
						Value directContrib = throughput * Ld;
						directContrib = ClampContribution( directContrib,
							stabilityConfig.directClamp );
						if( !bIndirectOnly || volumeBounces > 0 ) {
							result = result + directContrib;
						} else if( pDirectResult ) {
							*pDirectResult = *pDirectResult + directContrib;
						}
					}
				}

				// Sample the phase function for the continuation.
				const IPhaseFunction* pPhase = pCurrentMedium->GetPhaseFunction();
				if( !pPhase ) {
					return result;
				}

				const Vector3 wi = pPhase->Sample( wo, sampler );
				const Scalar phasePdf = pPhase->Pdf( wo, wi );
				if( phasePdf <= NEARZERO ) {
					return result;
				}

				// DL-109: this walk trains optimal-MIS the same way
				// IntegrateFromHitTemplated's own in-loop volume vertex
				// does (DL-84) -- one AccumulateCount per phase-sampled
				// attempt, placed BEFORE Russian roulette so an
				// RR-terminated attempt is still counted.  The paired
				// Accumulate happens below, at whichever env escape or
				// bounce-cap closure this vertex's continuation reaches
				// (see `trainedPhaseVal`) -- an earlier vertex in a
				// multi-scatter chain is superseded by the next one's
				// `trainedPhaseVal` exactly as DL-84's sibling site
				// overwrites `bsdfTimesCos` each iteration.
				if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() )
				{
					const_cast<OptimalMISAccumulator*>( rc.pOptimalMIS )->AccumulateCount(
						rast.x, rast.y, kTechniqueBSDF );
				}

				const Scalar phaseVal = pPhase->Evaluate( wo, wi );
				// Preserve the per-variant arithmetic exactly: the Pel path
				// builds RISEPel(s,s,s) and multiplies channel-wise; the NM
				// path evaluates `throughput * phaseVal / phasePdf`
				// left-to-right (multiply-then-divide).  These differ at the
				// ULP level, so the two forms are kept distinct rather than
				// unified.
				if constexpr ( Traits::is_pel ) {
					throughput = throughput * RISEPel(
						phaseVal / phasePdf, phaseVal / phasePdf, phaseVal / phasePdf );
				} else {
					throughput = throughput * phaseVal / phasePdf;
				}

				// DL-109: track this vertex's own RR survival probability
				// so the trained moment (`trainedPhaseVal` below) is the
				// REALIZED, post-RR quantity `phaseVal / q` -- the DL-84
				// round-7 convention -- rather than the pre-RR value.
				Scalar phaseRrSurvivalProb = 1.0;

				// Russian roulette on the volume scatter -- same call, same
				// arguments, same ordering as the main loop's volume RR.
				{
					const PathTransportUtilities::RussianRouletteResult rr =
						PathTransportUtilities::EvaluateRussianRoulette(
							volumeBounces,
							rrMinDepth, rrThreshold,
							PTSurvivalMagnitude( throughput ),
							1.0,
							sampler.Get1D() );
					if( rr.terminate ) {
						return result;
					}
					if( rr.survivalProb < 1.0 ) {
						throughput = PTDivByScalar( throughput, rr.survivalProb );
						phaseRrSurvivalProb = rr.survivalProb;
					}
				}

				// DL-109: the realized (post-RR) phase value at THIS
				// vertex.  Consumed by the Accumulate call below only if
				// THIS vertex's continuation is the one that escapes to
				// the environment; otherwise it is superseded by the next
				// iteration's own value, or the walk ends without ever
				// consuming it (a counted zero for this technique).
				const Scalar trainedPhaseVal = phaseVal / phaseRrSurvivalProb;

				walkRay = Ray( scatterPt, wi );
				walkPdf = phasePdf;
				volumeBounces++;

				//
				// --- follow the continuation ---------------------------
				//
				RayIntersection ri2( walkRay, rast );
				scene.GetObjects()->IntersectRay( ri2, true, true, false );

				if( ri2.geometric.bHit )
				{
					// A surface ends the walk: the main loop takes over and
					// handles everything past this point, including any
					// further medium transport, with `volumeBounces` carried
					// across so the shared bounce cap keeps counting.
					const Value hitResult = IntegrateFromHitForTag<Tag>( rc, rast, ri2, scene, caster,
						sampler, pRadianceMap, 1, iorStack, walkPdf,
						Traits::zero(), true, 1.0,
						IRayCaster::RAY_STATE::eRayDiffuse,
						0, 0, 0, 0, volumeBounces, 0,
						pAOV, tag );

					return result + throughput * hitResult;
				}

				//
				// --- the continuation missed all geometry --------------
				//
				// Sample the medium along it once more.  A scatter continues
				// the walk; a no-scatter is the escape, and carries the
				// per-channel survival weight Tr / pSurvival (== 1 for a
				// bounded medium, where "no scatter" means "left the
				// medium's AABB") rather than Tr itself, which would
				// double-count the attenuation the survival probability
				// already encodes.  Identical bookkeeping to the main loop's
				// `!scattered && !bHit` branch.
				//
				Value escapeWeight;
				if( volumeBounces < stabilityConfig.maxVolumeBounce )
				{
					const MediumSampleOutcome mso2 = PTSampleMediumDistance<Tag>(
						pCurrentMedium, walkRay, RISE_INFINITY, pLS, mediumSampler, tag );

					if( mso2.zeroContrib ) {
						// Equiangular strategy sampled a zero-density point:
						// a scatter-measure sample with zero weight.  Same
						// disposition as the two sites above -- stop, keeping
						// what the walk has already estimated.
						return result;
					}

					if( mso2.scattered ) {
						walkMso = mso2;
						walkT = mso2.t;
						continue;			// next scatter event
					}

					const Value TrEsc = PTEvalTransmittance<Tag>(
						pCurrentMedium, walkRay, RISE_INFINITY, tag );
					const Scalar pSurvival = mso2.noScatterPdfScale * PTEvalNoScatterSurvivalPdf<Tag>(
						pCurrentMedium, walkRay, RISE_INFINITY, tag );
					escapeWeight = PTSurvivalWeight<Tag>( TrEsc, pSurvival );
				}
				else
				{
					// Bounce cap reached.  Close the path with the
					// deterministic Beer-Lambert escape -- the estimator
					// this whole block replaced -- so the cap loses only the
					// tail beyond it rather than the escape as well.
					escapeWeight = PTEvalTransmittance<Tag>(
						pCurrentMedium, walkRay, RISE_INFINITY, tag );
				}

				// Environment for the escaped volume-scattered ray.
				//
				// MIS PARTNER RULE -- volume twin of the surface-escape block
				// in IntegrateFromHitTemplated (read that block's doc first;
				// this is the same rule on the camera-ray medium-scatter
				// path).  The NEE call at the scatter vertex this ray left
				// (PTEvaluateInScattering -> MediumTransport::
				// EvaluateInScattering -> LightSampler::EvaluateDirectLighting
				// with isVolumeScatter=true) runs the env-NEE strategy and
				// MIS-weights it against the PHASE pdf: EvaluateDirectLighting's
				// env block calls pMaterial->Pdf(), and pMaterial there is
				// MediumTransport's MediumScatterMaterial, whose Pdf() forwards
				// straight to IPhaseFunction::Pdf.  This phase-sampled env hit
				// is that strategy's MIS partner and must carry the
				// complementary weight.  It used to be added at weight 1, so the
				// two env strategies summed to 1 + w_nee instead of 1.  For an
				// isotropic phase function in a uniform environment
				// envPdf == phasePdf == 1/(4 pi), so w_nee = 0.5 and the
				// single-scatter env term was over-counted by 50 %.
				//
				// `walkPdf` is the pdf of the LAST phase sample, i.e. of the
				// very vertex whose NEE this partners -- which is why it is
				// carried across iterations rather than recomputed.
				//
				// Argument-order note: the NEE side evaluates
				// pPhase->Pdf( envDir, wo ) while this side has
				// pPhase->Pdf( wo, wi ).  Both concrete phase functions
				// (IsotropicPhaseFunction, HenyeyGreensteinPhaseFunction) depend
				// on the two directions only through Dot(wi, wo), which is
				// symmetric, so the two densities agree exactly and the MIS
				// partition closes.
				//
				// Delta guard: RISE has no delta phase function -- both
				// implementations return a finite density -- and `phasePdf >
				// NEARZERO` was already required to reach this point, so the
				// `walkPdf > 0` test below is a formality kept for textual
				// parallelism with the surface site (where `bsdfPdf > 0` really
				// does select the "delta lobe keeps full weight" arm).
				//
				// DL-109 (2026-09-17): optimal-MIS TRAINING IS now
				// accumulated here (the block below, gated on `envPdf > 0`
				// alongside the weight) -- this comment used to say the
				// opposite and left this walk untrained, the same
				// training-input gap DL-84 closed at the OTHER volume
				// vertex (IntegrateFromHitTemplated's in-loop one).  See
				// `trainedPhaseVal`'s derivation a few dozen lines above
				// for the realized-moment numerator this site trains.
				//
				// The env escape after scatter 1 is the camera-visible
				// vertex's direct partner and follows the indirect-only
				// routing; after scatter 2 or later it is multiple
				// scattering and is kept in every mode.
				const bool bDirectPartner = ( volumeBounces == 1 );
				if( ( !bIndirectOnly || !bDirectPartner || pDirectResult )
				 && !PTSoloSuppressEnvironment( caster ) && scene.GetGlobalRadianceMap() ) {
					Value envRadiance = PTEvalRadianceMap<Tag>(
						scene.GetGlobalRadianceMap(), walkRay, rast, tag );

					// MIS weight for the phase-sampled environment hit.
					if( pLS && walkPdf > 0 )
					{
						const EnvironmentSampler* pES = pLS->GetEnvironmentSampler();
						if( pES )
						{
							const Scalar envPdf = pES->Pdf( walkRay.Dir() );
							if( envPdf > 0 )
							{
								// DL-109/DL-124: train the optimal-MIS moment
								// for THIS vertex's phase-sampled escape.
								// `trainedPhaseVal` is the realized (post-RR)
								// phase value at the vertex that produced
								// `walkRay` (DL-84's convention); `escapeWeight`
								// (Tr/pSurvival along the escape segment, or
								// the deterministic Beer-Lambert term at the
								// bounce cap) is folded in too, matching its
								// NEE partner's own EvalShadowTransmittance
								// (DL-124) -- both are already computed above
								// for the real image contribution, so this is
								// purely additive to the trained moment.
								if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() )
								{
									const Scalar fLum = PTPositiveMagnitude(
										envRadiance * trainedPhaseVal * escapeWeight );
									const Scalar f2 = fLum * fLum;
									if( f2 > 0 && walkPdf > 0 )
									{
										const_cast<OptimalMISAccumulator*>(rc.pOptimalMIS)->Accumulate(
											rast.x, rast.y,
											f2, walkPdf, kTechniqueBSDF );
									}
								}

								Scalar w_phase;
								if( rc.pOptimalMIS && rc.pOptimalMIS->IsReady() )
								{
									const Scalar alpha = rc.pOptimalMIS->GetAlpha( rast.x, rast.y );
									w_phase = MISWeights::OptimalMIS2Weight( walkPdf, envPdf, alpha );
								}
								else
								{
									w_phase = PowerHeuristic( walkPdf, envPdf );
								}
								envRadiance = envRadiance * w_phase;
							}
						}
					}

					const Value volumeEnv = throughput * escapeWeight * envRadiance;
					if( bIndirectOnly && bDirectPartner ) {
						if( pDirectResult ) *pDirectResult = *pDirectResult + volumeEnv;
					} else {
						result = result + volumeEnv;
					}
				}
				return result;
			}
		}
		else if( ri.geometric.bHit )
		{
			// Surface hit through medium (analog no-scatter survival).
			// SampleDistance already drew "reach the surface"; that survival
			// event carries Beer-Lambert.  IntegrateFromHit starts with
			// throughput=1, so we scale its result by only the per-channel weight
			// Tr / pSurvival (deterministic no-scatter survival pdf; = 1 for
			// monochrome/NM homogeneous) rather than Tr, which would double-count
			// attenuation.
			const Value Tr = PTEvalTransmittance<Tag>(
				pCurrentMedium, cameraRay, ri.geometric.range, tag );
			const Scalar pSurvival = mso.noScatterPdfScale * PTEvalNoScatterSurvivalPdf<Tag>(
				pCurrentMedium, cameraRay, ri.geometric.range, tag );

			if( !ri.geometric.bHit ) {
				return Traits::zero();
			}

			Value directAtHit = Traits::zero();
			Value hitResult;
			if constexpr ( Traits::is_pel ) {
				hitResult = IntegrateFromHitTemplated<Tag>( rc, rast, ri, scene, caster,
					sampler, pRadianceMap, 0, iorStack,
					0, Traits::zero(), true, 1.0,
					IRayCaster::RAY_STATE::eRayView,
					0, 0, 0, 0, 0, 0, false, false,
					pAOV, pDirectResult ? &directAtHit : nullptr, tag );
			} else {
				hitResult = IntegrateFromHitForTag<Tag>( rc, rast, ri, scene, caster,
					sampler, pRadianceMap, 0, iorStack,
					0, Traits::zero(), true, 1.0,
					IRayCaster::RAY_STATE::eRayView,
					0, 0, 0, 0, 0, 0,
					pAOV, tag );
			}
			const Value survivalWeight = PTSurvivalWeight<Tag>( Tr, pSurvival );
			if( pDirectResult ) {
				*pDirectResult = *pDirectResult + survivalWeight * directAtHit;
			}
			return survivalWeight * hitResult;
		}
		else
		{
			// !scattered && !ri.geometric.bHit: the camera ray escapes the
			// scene through the medium (analog no-scatter survival).  The
			// escape survival event already carries the Beer-Lambert factor via
			// its probability, so store only the per-channel weight Tr /
			// pSurvival (deterministic no-scatter survival pdf; = 1 for
			// monochrome/NM homogeneous) for the env radiance below —
			// multiplying the full Tr would double-count attenuation.
			const Value Tr = PTEvalTransmittance<Tag>( pCurrentMedium, cameraRay, maxDist, tag );
			const Scalar pSurvival = mso.noScatterPdfScale * PTEvalNoScatterSurvivalPdf<Tag>(
				pCurrentMedium, cameraRay, maxDist, tag );
			escapeTr = PTSurvivalWeight<Tag>( Tr, pSurvival );
		}
	}

	// No medium, or medium with no scatter and no surface hit
	if( !ri.geometric.bHit )
	{
		// GUI render modes P2b `indirect`: a camera ray that misses all
		// geometry sees the environment/background DIRECTLY -- a direct
		// contribution (same as a directly-visible emitter), so indirect-
		// only returns black here.  Continuation-ray env misses (depth>=1,
		// inside the IntegrateFromHit loop) are indirect environment
		// lighting and are kept.  This is the ONLY primary-miss env site;
		// the in-loop gates were dead (never reached at depth 0).
		if( EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) && !pDirectResult ) {
			return Traits::zero();
		}

		// Camera ray missed all geometry.  Honour the rasterizer's
		// `radiance_background` / RadianceMapConfig::isBackground
		// switch: when false (`Mix Shader gated by Light Path.Is
		// Camera Ray` pattern in Blender; same scene-language flag
		// for hand-authored scenes), the environment radiance still
		// drives indirect bounces but primary rays return black,
		// matching Cycles' default for that pattern.
		if( !caster.IsRadianceMapVisibleAsBackground() ) {
			return Traits::zero();
		}

		// review-p2d P1-1: a directly-visible environment IS env illumination
		// reaching the camera; under a light/luminary solo it must read black
		// so that solo(light) + solo(env) == all holds pixel-for-pixel.
		if( PTSoloSuppressEnvironment( caster ) ) {
			return Traits::zero();
		}

		// Environment map
		Value envResult = Traits::zero();
		if( pRadianceMap )
		{
			envResult = PTEvalRadianceMap<Tag>( pRadianceMap, cameraRay, rast, tag );
		}
		else if( scene.GetGlobalRadianceMap() )
		{
			envResult = PTEvalRadianceMap<Tag>( scene.GetGlobalRadianceMap(), cameraRay, rast, tag );
		}
		const Value primaryEnvironment = escapeTr * envResult;
		if( EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) ) {
			if( pDirectResult ) *pDirectResult = *pDirectResult + primaryEnvironment;
			return Traits::zero();
		}
		return primaryEnvironment;
	}

	if constexpr ( Traits::is_pel ) {
		return IntegrateFromHitTemplated<Tag>( rc, rast, ri, scene, caster,
			sampler, pRadianceMap, 0, iorStack,
			0, Traits::zero(), true, 1.0,
			IRayCaster::RAY_STATE::eRayView,
			0, 0, 0, 0, 0, 0, false, false,
			pAOV, pDirectResult, tag );
	}
	return IntegrateFromHitForTag<Tag>( rc, rast, ri, scene, caster,
		sampler, pRadianceMap, 0, iorStack,
		0, Traits::zero(), true, 1.0,
		IRayCaster::RAY_STATE::eRayView,
		0, 0, 0, 0, 0, 0,
		pAOV, tag );
}


//////////////////////////////////////////////////////////////////////
// IntegrateRay — RGB path tracer entry point (thin forwarder).
//////////////////////////////////////////////////////////////////////

RISEPel PathTracingIntegrator::IntegrateRay(
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const Ray& cameraRay,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const IRadianceMap* pRadianceMap,
	PixelAOV* pAOV
	) const
{
	return IntegrateRayTemplated<PelTag>( rc, rast, cameraRay, scene, caster,
		sampler, pRadianceMap, pAOV, nullptr, PelTag{} );
}

RISEPel PathTracingIntegrator::IntegrateRayDirectIndirect(
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const Ray& cameraRay,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const IRadianceMap* pRadianceMap,
	RISEPel& direct,
	PixelAOV* pAOV
	) const
{
	direct = RISEPel( 0, 0, 0 );
	return IntegrateRayTemplated<PelTag>( rc, rast, cameraRay, scene, caster,
		sampler, pRadianceMap, pAOV, &direct, PelTag{} );
}


//////////////////////////////////////////////////////////////////////
// IntegrateFromHitNM — Iterative NM path tracer starting from a
// pre-computed surface hit.
//
// Spectral single-wavelength variant of IntegrateFromHit.  Uses
// Scalar instead of RISEPel, ScatterNM instead of Scatter, and
// NM-specific material evaluation (emittedRadianceNM, valueNM,
// EvaluateDirectLightingNM, EvaluateAtShadingPointNM).
//
// Same iterative structure: first iteration processes the caller's
// pre-computed hit; subsequent iterations do intersection + medium
// transport + miss handling before processing the next hit.
//
// BSSRDF/SSS continuation sites stay recursive via CastRayNM
// (same pattern as RGB calling CastRay for BSSRDF).
//////////////////////////////////////////////////////////////////////

Scalar PathTracingIntegrator::IntegrateFromHitNM(
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const RayIntersection& firstHit,
	const Scalar nm,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const IRadianceMap* pRadianceMap,
	unsigned int startDepth,
	const IORStack& initialIorStack,
	Scalar bsdfPdf,
	Scalar bsdfTimesCosNM,
	bool considerEmission,
	Scalar importance,
	IRayCaster::RAY_STATE::RayType rayType,
	unsigned int diffuseBounces,
	unsigned int glossyBounces,
	unsigned int transmissionBounces,
	unsigned int translucentBounces,
	unsigned int volumeBounces,
	Scalar glossyFilterWidth,
	bool smsPassedThroughSpecular_initial,
	bool smsHadNonSpecularShading_initial,
	PixelAOV* pAOV,
	Scalar bsdfMisPdf_,
	Scalar castRRCompensation_
	) const
{
	// Thin forwarder to the shared templated body.  pAOV carries the
	// denoiser AOV for the spectral (NM) path: NMTag::supports_aov is
	// true, so IntegrateFromHitTemplated records normal/albedo at the
	// first non-delta vertex (Accurate mode) exactly as the RGB path.
	// The spectral rasterizer plumbs a PixelAOV through IntegrateRayNM;
	// callers that do not denoise (PathTracingShaderOp) pass null.
	return IntegrateFromHitTemplated<NMTag>(
		rc, rast, firstHit, scene, caster, sampler, pRadianceMap,
		startDepth, initialIorStack, bsdfPdf, bsdfTimesCosNM,
		considerEmission, importance, rayType, diffuseBounces,
		glossyBounces, transmissionBounces, translucentBounces,
		volumeBounces, glossyFilterWidth, smsPassedThroughSpecular_initial,
		smsHadNonSpecularShading_initial, pAOV, nullptr, NMTag{ nm }, bsdfMisPdf_,
		castRRCompensation_ );
}


//////////////////////////////////////////////////////////////////////
// IntegrateFromHitHWSS — Hero wavelength spectral sampling variant
// starting from a pre-computed surface hit.
//
// For SPF-only materials and SSS materials, falls back to
// per-wavelength IntegrateFromHitNM.  For BSDF materials, hero
// wavelength drives direction sampling and companions evaluate
// throughput at the hero's geometric direction.
//////////////////////////////////////////////////////////////////////

void PathTracingIntegrator::IntegrateFromHitHWSS(
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const RayIntersection& firstHit,
	SampledWavelengths& swl,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const IRadianceMap* pRadianceMap,
	unsigned int startDepth,
	const IORStack& initialIorStack,
	Scalar bsdfPdf,
	bool considerEmission,
	Scalar importance,
	IRayCaster::RAY_STATE::RayType rayType,
	unsigned int diffuseBounces,
	unsigned int glossyBounces,
	unsigned int transmissionBounces,
	unsigned int translucentBounces,
	unsigned int volumeBounces,
	Scalar glossyFilterWidth,
	Scalar hwssResult[SampledWavelengths::N],
	PixelAOV* pAOV,
	Scalar bsdfMisPdf_,
	Scalar castRRCompensation_
	) const
{
	// Initialize results
	for( unsigned int i = 0; i < SampledWavelengths::N; i++ ) {
		hwssResult[i] = 0;
	}

	// DL-74: this body has NO guiding block of its own, so every density it
	// PRODUCES is both the true sampling density and the MIS partner.  What
	// it CONSUMES is a different matter: the caller may have entered from
	// `RayCaster`'s volume phase-scatter continuation, whose two fields
	// differ under volume guiding, so the incoming partner is carried
	// separately and used for every weight below.  Negative = "same as
	// bsdfPdf" (every camera-ray and legacy-rasterizer entry).
	Scalar bsdfMisPdf = bsdfMisPdf_ < 0 ? bsdfPdf : bsdfMisPdf_;

	// Fast-mode albedo/normal fallback for callers that begin from a
	// pre-computed hit. Root camera-ray entry points normally record this
	// before medium sampling; they also own primary-depth capture. Accurate
	// mode skips this and records at the first non-delta scatter below.
	// Albedo/normal are wavelength-independent, so the hero bundle records once.
	if( pAOV && !pAOV->valid && firstHit.geometric.bHit &&
	    rc.aovPrefilterMode == OidnPrefilter::Fast )
	{
		pAOV->normal = firstHit.geometric.vNormal;
		// GUI render modes P2b `clay_lights` (review-p2c P2-d fix, HWSS
		// twin of the RGB/NM IntegrateRay fast-mode hook -- see that
		// site's fuller comment).
		pAOV->albedo = EffectivePathTracingClayOverride( rc, mClayOverride ) ? pClayBRDF->albedo( firstHit.geometric ) :
			( ( firstHit.pMaterial && firstHit.pMaterial->GetBSDF() )
				? firstHit.pMaterial->GetBSDF()->albedo( firstHit.geometric )
				: RISEPel( 1, 1, 1 ) );
		pAOV->valid = true;
	}

	// Check material at first hit to determine path strategy
	const IBSDF* pBRDF = firstHit.pMaterial ? firstHit.pMaterial->GetBSDF() : 0;

	// ================================================================
	// Fallback 1: SPF-only materials (no BSDF)
	// ================================================================
	if( !pBRDF )
	{
		for( unsigned int i = 0; i < SampledWavelengths::N; i++ )
		{
			if( !swl.terminated[i] )
			{
				hwssResult[i] = IntegrateFromHitNM( rc, rast, firstHit,
					swl.lambda[i], scene, caster, sampler, pRadianceMap,
					startDepth, initialIorStack, bsdfPdf, 0,
					considerEmission, importance, rayType,
					diffuseBounces, glossyBounces, transmissionBounces,
					translucentBounces, volumeBounces, glossyFilterWidth,
						false, false, pAOV, bsdfMisPdf,
						// DL-196: this delegation IS `firstHit` at
						// `startDepth` -- forward the cast-level RR
						// compensation (see this function's own trailing
						// parameter doc).
						castRRCompensation_ );
			}
		}
		return;
	}

	// ================================================================
	// Fallback 2: SSS materials
	// ================================================================
	{
		ISubSurfaceDiffusionProfile* pProfile =
			firstHit.pMaterial ? firstHit.pMaterial->GetDiffusionProfile() : 0;

		const RandomWalkSSSParams* pRWParams =
			firstHit.pMaterial ? firstHit.pMaterial->GetRandomWalkSSSParams() : 0;

		RandomWalkSSSParams rwParamsNM;
		bool hasRWNM = firstHit.pMaterial &&
			firstHit.pMaterial->GetRandomWalkSSSParamsNM( swl.HeroLambda(), rwParamsNM );

		// P2-d fix (review-p2b): under clay_lights the delegated per-
		// wavelength path (IntegrateFromHitNM) now bypasses SSS transport
		// itself (see the diffusion-profile/RW-SSS gates above), so this
		// routing decision would be numerically harmless either way -- but
		// gating it here too keeps clay_lights on the more efficient HWSS
		// hero-bundle path instead of unconditionally falling back to
		// per-wavelength for a surface that no longer has any SSS behavior
		// to represent.
		if( ( pProfile || pRWParams || hasRWNM ) && !EffectivePathTracingClayOverride( rc, mClayOverride ) )
		{
			for( unsigned int i = 0; i < SampledWavelengths::N; i++ )
			{
				if( !swl.terminated[i] )
				{
					hwssResult[i] = IntegrateFromHitNM( rc, rast, firstHit,
						swl.lambda[i], scene, caster, sampler, pRadianceMap,
						startDepth, initialIorStack, bsdfPdf, 0,
						considerEmission, importance, rayType,
						diffuseBounces, glossyBounces, transmissionBounces,
						translucentBounces, volumeBounces, glossyFilterWidth,
						false, false, pAOV, bsdfMisPdf,
						// DL-196: this delegation IS `firstHit` at
						// `startDepth` too (see the Fallback 1 site above).
						castRRCompensation_ );
				}
			}
			return;
		}
	}

	// ================================================================
	// HWSS path: materials with BSDF, no SSS
	// ================================================================
	// Hero wavelength drives all directional decisions.  Companions
	// evaluate throughput at the hero's geometric direction.

	const Scalar heroNM = swl.HeroLambda();

	// throughputComp[0] is the hero-wavelength throughput; throughputComp[1..N-1]
	// are companion wavelengths.  An earlier draft kept a separate
	// `throughputHero` mirror but every read site now uses throughputComp[0]
	// directly — the mirror was dropped to remove an always-equal bookkeeping
	// pair that the compiler (rightly) flagged as set-but-not-used.
	Scalar throughputComp[SampledWavelengths::N];
	for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
		throughputComp[w] = 1.0;
	}

	// DL-170: the per-lane MIS-PARTNER density, alongside the per-lane
	// throughput above.  `RAY_STATE::bsdfMisPdf` (the scalar `bsdfMisPdf`
	// local variable below) carries ONE partner across the function-call
	// boundary by design (DL-103's own cost analysis is why -- see that
	// row) -- but ONCE INSIDE this bundle's own loop, nothing forces the
	// hero's aggregate density onto every companion wavelength's own
	// emitter-hit / env-escape weight, and PART 2's NEE arms already
	// evaluate the aggregate `PdfNM` at EACH companion's OWN lambda.
	// `misBsdfPdfComp[0]` mirrors the hero scalar exactly (bit-identical
	// hero lane, see PART 3's update below); `misBsdfPdfComp[1..N-1]` are
	// recomputed once per bounce, at the vertex that produces the
	// continuation, from the SAME aggregate `PdfNM` PART 2 uses -- no
	// sampler draws, so this costs one extra `ISPF::PdfNM` per active
	// companion per non-delta bounce and touches no RNG stream.
	// Initialized to the caller's incoming scalar partner (the only
	// information available before this bundle's own first bounce, and
	// wavelength-independent by construction at that boundary).
	Scalar misBsdfPdfComp[SampledWavelengths::N];
	for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
		misBsdfPdfComp[w] = bsdfMisPdf;
	}

	RayIntersection ri( firstHit );
	Ray currentRay = ri.geometric.ray;
	IORStack iorStack = initialIorStack;
	bool needsIntersection = false;

	// GUI render modes P2b `indirect` (review-p2c P2-b fix): HWSS twin of
	// the Pel/NM loop's `bPassedThroughSpecular` -- tracks whether the
	// PREVIOUS iteration's BSDF sample (i.e. the depth==0 scatter, by the
	// time depth==1 is being processed) was a delta/specular event.  Used
	// only to gate the depth==1 MIS-partner suppression below (this loop is
	// always entered fresh -- the SPF-only and SSS fallbacks above delegate
	// to IntegrateFromHitNM, which tracks its own copy -- so a plain local
	// initialized false is correct with no carried-in initial value needed).
	bool bPassedThroughSpecular = false;

	const unsigned int rrMinDepth = stabilityConfig.rrMinDepth;
	const Scalar rrThreshold = stabilityConfig.rrThreshold;

	const LightSampler* pLS = caster.GetLightSampler();
	// GUI render modes P2a fix: HWSS's twin of the Pel/NM loop cap above --
	// was also a hardcoded literal 128; see SetMaxPathDepth's doc.
	const unsigned int maxDepth = EffectivePathTracingMaxDepth( rc, mMaxPathDepth );

	for( unsigned int depth = startDepth; depth < maxDepth; depth++ )
	{
		// Runaway-throughput guard -- see RGB IntegrateFromHit.  HWSS
		// carries per-wavelength throughput in throughputComp[]; take
		// the max across the bundle.
		{
			Scalar maxThr = 0;
			bool anyBad = false;
			for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
				const Scalar v = throughputComp[w];
				if( !RISE::IsFiniteDouble( v ) ) { anyBad = true; break; }
				if( fabs( v ) > maxThr ) maxThr = fabs( v );
			}
			if( anyBad || maxThr > Scalar(1e6) ) {
				break;
			}
		}

		sampler.StartStream( 16 + depth );

		// ============================================================
		// Intersection + medium transport (spectral, hero drives)
		// ============================================================
		if( needsIntersection )
		{
			ri = RayIntersection( currentRay, rast );
			ri.geometric.glossyFilterWidth = glossyFilterWidth;
			scene.GetObjects()->IntersectRay( ri, true, true, false );

			bool bHit = ri.geometric.bHit;

			const IObject* pMediumObject = 0;
			const IMedium* pCurrentMedium = MediumTracking::GetCurrentMediumWithObject(
				iorStack, &scene, pMediumObject );

			if( pCurrentMedium )
			{
				const Scalar maxDist = bHit ? ri.geometric.range : RISE_INFINITY;
				IndependentSampler mediumSampler( rc.random );
				// Hero wavelength drives free-flight sampling; the MIS
				// combinedPdf is in distance measure (wavelength-independent
				// for equiangular; hero-driven for delta tracking).
				const MediumSampleOutcome mso = SampleDistanceWithEquiangularMIS_NM(
					pCurrentMedium, currentRay, maxDist, heroNM, pLS, mediumSampler );
				const Scalar t_m = mso.t;
				const bool scattered = mso.scattered;

				if( mso.zeroContrib )
				{
					break;
				}

				if( scattered && volumeBounces < stabilityConfig.maxVolumeBounce )
				{
					// ====================================================
					// MAIN-LOOP VOLUMETRIC WALK -- HWSS twin.
					//
					// Read IntegrateRayTemplated's CAMERA-RAY VOLUMETRIC
					// WALK first: the estimator, the per-scatter MIS
					// invariant, the loop invariants and the termination
					// rule are all derived there and are NOT restated here.
					// IntegrateRayHWSS's walk is the camera-side HWSS twin
					// of that derivation; this block is the same walk on the
					// SURFACE-BOUNCE -> MEDIUM path, i.e. it is entered only
					// after >= 1 BSDF scatter in this loop (the medium block
					// is gated on `needsIntersection`, which is false on the
					// entry iteration and true from then on).
					//
					// It used to run NEE at the scatter vertex and then
					// `break` out of the shared loop -- the same dropped-
					// continuation loss class wave 4 fixed on camera rays,
					// but reached through a surface bounce instead of
					// directly.  Everything past the first in-medium vertex
					// (order tau*tau' of the signal on that path) was
					// discarded.
					//
					// STRUCTURE.  As on the camera side, the hero bundle
					// SPLITS at a volume scatter: medium coefficients are
					// wavelength-dependent, so each wavelength runs its own
					// walk with its own phase samples, its own directions
					// and its own escape, and the surface hand-off is
					// IntegrateFromHitNM rather than IntegrateFromHitHWSS.
					// The original single-scatter code already made that
					// choice ("fall back to per-wavelength NM"); continuing
					// the walk keeps it.  Consequently only the FIRST
					// segment -- the shared one, sampled before the split --
					// is hero-driven, which is why `walkMso` starts as the
					// hero `mso` and every continuation segment re-samples
					// at ITS OWN `lambda`.
					//
					// DIFFERENCES FROM THE CAMERA-SIDE WALK, all because
					// this one starts MID-PATH rather than at the camera:
					//   - `throughput` starts at `throughputComp[w]`, not 1.
					//   - `walkDepth` / `walkVolumeBounces` start at the
					//     carried `depth` / `volumeBounces` and BOTH advance
					//     per scatter, so Russian roulette sees exactly the
					//     `depth + volumeBounces` schedule the RGB/NM main
					//     loop produces by falling through its `continue`
					//     (that loop's `continue` bumps `depth` too).
					//   - `importance` is the carried path importance -- the
					//     RGB/NM main loop's volume RR reference.  The camera
					//     walks pass 1.0 only because that IS the value at
					//     camera entry.
					//   - the walk stops when `walkDepth` reaches `maxDepth`,
					//     dropping the remainder exactly as the enclosing
					//     `for( depth < maxDepth )` would have.
					//   - the surface hand-off carries this loop's live
					//     per-type bounce counters, `rayType` and
					//     `glossyFilterWidth`, and passes
					//     smsHadNonSpecularShading=true for the same reason
					//     the no-BSDF and SSS delegations below do (this
					//     site is likewise reachable only after >= 1
					//     non-specular SMS anchor vertex, whose SMS pass
					//     already counted the BSDF-sampled emission at the
					//     light).
					//   - the escape mirrors the enclosing loop's own `!bHit`
					//     env branch, INCLUDING its `pRadianceMap` fallback:
					//     an escaping continuation is precisely what that
					//     branch would have shaded had the walk been able to
					//     `continue` in-loop the way the RGB/NM twin does.
					//
					// INDIRECT-ONLY.  Nothing to suppress.  This loop's
					// indirect-only gates fire at `depth == 0` (NEE) and
					// `depth == 1` (the BSDF-sampled env partner of that
					// suppressed NEE).  Every vertex of this walk sits at
					// least one BSDF scatter past the camera-visible vertex,
					// so its NEE is genuine indirect illumination and its
					// phase-sampled env partner pairs with an NEE that WAS
					// evaluated.
					// ====================================================
					const bool bSoloSuppressEnv = PTSoloSuppressEnvironment( caster );
					const IPhaseFunction* pPhase = pCurrentMedium->GetPhaseFunction();

					for( unsigned int w = 0; w < SampledWavelengths::N; w++ )
					{
						if( swl.terminated[w] ) continue;

						const Scalar lambda = swl.lambda[w];

						Scalar throughput = throughputComp[w];
						Ray walkRay = currentRay;
						MediumSampleOutcome walkMso = mso;
						Scalar walkT = t_m;
						Scalar walkPdf = 0;
						unsigned int walkVolumeBounces = volumeBounces;
						unsigned int walkDepth = depth;

						for( ;; )
						{
							//
							// --- scatter event ------------------------
							//
							const Point3 scatterPt = walkRay.PointAtLength( walkT );
							const Vector3 wo = walkRay.Dir();

							const MediumCoefficientsNM coeff = pCurrentMedium->GetCoefficientsNM( scatterPt, lambda );
							const Scalar Tr = pCurrentMedium->EvalTransmittanceNM( walkRay, walkT, lambda );

							Scalar medWeight = 0;
							if( walkMso.useExplicitThroughput && walkMso.combinedPdf > 0 )
							{
								// MIS throughput in hero-driven HWSS: per-wavelength
								// Tr_w * sigma_s_w divided by the combined hero-driven PDF.
								medWeight = Tr * coeff.sigma_s / walkMso.combinedPdf;
							}
							else if( coeff.sigma_t > 0 && Tr > 0 )
							{
								medWeight = Tr * coeff.sigma_s / (coeff.sigma_t * Tr);
							}

							if( medWeight <= 0 ) break;

							throughput *= medWeight;

							// NEE at scatter point
							if( pLS )
							{
								Scalar Ld = MediumTransport::EvaluateInScatteringNM(
									scatterPt, wo, pCurrentMedium, lambda, caster, pLS,
									sampler, rast, pMediumObject );
								if( Ld > 0 )
								{
									Scalar directContrib = throughput * Ld;
									directContrib = ClampContribution( directContrib,
										stabilityConfig.directClamp );
									hwssResult[w] += directContrib;
								}
							}

							// Phase function continuation
							if( !pPhase ) break;

							const Vector3 wi = pPhase->Sample( wo, sampler );
							const Scalar phasePdf = pPhase->Pdf( wo, wi );
							if( phasePdf <= NEARZERO ) break;

							const Scalar phaseVal = pPhase->Evaluate( wo, wi );
							throughput = throughput * phaseVal / phasePdf;

							// Russian roulette on the volume scatter -- same
							// call and ordering as the RGB/NM main loop's
							// volume RR, with its `depth + volumeBounces`
							// schedule and its `importance` reference.
							{
								const PathTransportUtilities::RussianRouletteResult rr =
									PathTransportUtilities::EvaluateRussianRoulette(
										walkDepth + walkVolumeBounces,
										rrMinDepth, rrThreshold,
										PTSurvivalMagnitude( throughput ),
										importance,
										sampler.Get1D() );
								if( rr.terminate ) break;
								if( rr.survivalProb < 1.0 ) {
									throughput /= rr.survivalProb;
								}
							}

							walkRay = Ray( scatterPt, wi );
							walkPdf = phasePdf;
							walkVolumeBounces++;
							walkDepth++;

							// Path-depth cap: the enclosing loop would have
							// exited here, dropping the remainder.  Match it.
							if( walkDepth >= maxDepth ) break;

							//
							// --- follow the continuation ---------------
							//
							RayIntersection ri2( walkRay, rast );
							ri2.geometric.glossyFilterWidth = glossyFilterWidth;
							scene.GetObjects()->IntersectRay( ri2, true, true, false );

							if( ri2.geometric.bHit )
							{
								hwssResult[w] += throughput * IntegrateFromHitNM(
									rc, rast, ri2, lambda, scene, caster,
									sampler, pRadianceMap, walkDepth, iorStack,
									walkPdf, 0, true, importance, rayType,
									diffuseBounces, glossyBounces, transmissionBounces,
									translucentBounces, walkVolumeBounces, glossyFilterWidth,
									false, true,
									// HWSS geometry is hero-driven.  Let only the hero
									// continuation populate the shared, wavelength-independent
									// Accurate guide so companion paths cannot race to define it.
									w == 0 ? pAOV : 0 );
								break;
							}

							//
							// --- the continuation missed all geometry ---
							//
							// Sample this wavelength's medium along it once
							// more: a scatter continues the walk, a
							// no-scatter is the escape and carries the
							// survival weight Tr / pSurvival (== 1 for a
							// bounded medium) rather than Tr itself, which
							// would double-count the attenuation the
							// survival probability already encodes (G1-c).
							//
							Scalar escapeWeight;
							if( walkVolumeBounces < stabilityConfig.maxVolumeBounce )
							{
								const MediumSampleOutcome mso2 = SampleDistanceWithEquiangularMIS_NM(
									pCurrentMedium, walkRay, RISE_INFINITY, lambda, pLS, mediumSampler );

								if( mso2.zeroContrib ) break;

								if( mso2.scattered ) {
									walkMso = mso2;
									walkT = mso2.t;
									continue;			// next scatter event
								}

								const Scalar TrEsc = pCurrentMedium->EvalTransmittanceNM(
									walkRay, RISE_INFINITY, lambda );
								const Scalar pSurvival = mso2.noScatterPdfScale *
									pCurrentMedium->EvalDistancePdfNM(
										walkRay, RISE_INFINITY, /*scattered=*/false, RISE_INFINITY, lambda );
								escapeWeight = ( pSurvival > 0.0 ) ? ( TrEsc / pSurvival ) : TrEsc;
							}
							else
							{
								// Bounce cap: close the path with the
								// deterministic Beer-Lambert escape -- the
								// estimator this block replaced -- so the cap
								// loses only the tail beyond it rather than
								// the escape as well.
								escapeWeight = pCurrentMedium->EvalTransmittanceNM(
									walkRay, RISE_INFINITY, lambda );
							}

							// MIS PARTNER RULE -- see the RGB/NM camera walk's
							// escape site for the full derivation (phase-sampled
							// env hit is env-NEE's MIS partner;
							// MediumScatterMaterial::Pdf is the density the NEE
							// side weighs against; the phase pdf is symmetric in
							// its two arguments; no delta phase function exists;
							// optimal-MIS training deliberately not accumulated).
							// envPdf/walkPdf MUST be recomputed per-wavelength:
							// pPhase->Sample above runs inside the per-wavelength
							// loop, so each wavelength holds its OWN sampled
							// direction, and EnvironmentSampler::Pdf of that
							// direction generically differs across the bundle.
							// They coincide only in the isotropic-phase /
							// uniform-env special case -- do NOT hoist.
							// `walkPdf` is the pdf of the LAST phase sample, i.e.
							// of the very vertex whose NEE this partners.
							if( !bSoloSuppressEnv )
							{
								if( scene.GetGlobalRadianceMap() )
								{
									Scalar envRadiance =
										scene.GetGlobalRadianceMap()->GetRadianceNM(
											walkRay, rast, lambda );

									if( pLS && walkPdf > 0 )
									{
										const EnvironmentSampler* pES = pLS->GetEnvironmentSampler();
										if( pES )
										{
											const Scalar envPdf = pES->Pdf( walkRay.Dir() );
											if( envPdf > 0 )
											{
												Scalar w_phase;
												if( rc.pOptimalMIS && rc.pOptimalMIS->IsReady() )
												{
													const Scalar alpha = rc.pOptimalMIS->GetAlpha( rast.x, rast.y );
													w_phase = MISWeights::OptimalMIS2Weight( walkPdf, envPdf, alpha );
												}
												else
												{
													w_phase = PowerHeuristic( walkPdf, envPdf );
												}
												envRadiance *= w_phase;
											}
										}
									}

									hwssResult[w] += throughput * escapeWeight * envRadiance;
								}
								else if( pRadianceMap )
								{
									// Local (shader-op) radiance map: no
									// EnvironmentSampler exists for it, so there
									// is no env-NEE strategy to partner and the
									// hit carries full weight -- exactly what the
									// enclosing loop's `!bHit` branch does.
									hwssResult[w] += throughput * escapeWeight *
										pRadianceMap->GetRadianceNM( walkRay, rast, lambda );
								}
							}
							break;
						}
					}
					break;  // every wavelength's walk ran to completion
				}
				else if( !scattered && bHit )
				{
					// HWSS no-scatter survival reweight (G1-c): the free-flight distance
					// was sampled once at the HERO wavelength, so the no-scatter (survival)
					// event carries the hero survival pdf.  Each bundle wavelength's Tr_w
					// is reweighted by Tr_w / (noScatterPdfScale * pSurvivalHero) -- NOT
					// multiplied by Tr_w, which double-counts Beer-Lambert (the survival
					// probability already encodes the attenuation).  = 1 for a gray bundle;
					// mirrors the non-HWSS PTSurvivalWeight scalar sites (~3376).
					const Scalar pSurvivalHero = mso.noScatterPdfScale *
						pCurrentMedium->EvalDistancePdfNM(
							currentRay, ri.geometric.range, /*scattered=*/false, ri.geometric.range, heroNM );
					for( unsigned int w = 0; w < SampledWavelengths::N; w++ )
					{
						if( swl.terminated[w] ) {
							continue;
						}
						const Scalar Tr = pCurrentMedium->EvalTransmittanceNM(
							currentRay, ri.geometric.range, swl.lambda[w] );
						throughputComp[w] *= ( pSurvivalHero > 0.0 ) ? ( Tr / pSurvivalHero ) : Tr;
					}
				}
				else if( !scattered && !bHit )
				{
					// HWSS-2 bounce-loop escape: no-scatter survival reweight (G1-c),
					// same as HWSS-1 but the segment is the full escape distance maxDist
					// (PBRT-v4 beta *= T_maj, now survival-corrected per wavelength).
					const Scalar pSurvivalHero = mso.noScatterPdfScale *
						pCurrentMedium->EvalDistancePdfNM(
							currentRay, maxDist, /*scattered=*/false, maxDist, heroNM );
					for( unsigned int w = 0; w < SampledWavelengths::N; w++ )
					{
						if( swl.terminated[w] ) {
							continue;
						}
						const Scalar Tr = pCurrentMedium->EvalTransmittanceNM(
							currentRay, maxDist, swl.lambda[w] );
						throughputComp[w] *= ( pSurvivalHero > 0.0 ) ? ( Tr / pSurvivalHero ) : Tr;
					}
				}
			}

			if( !bHit )
			{
				// GUI render modes P2b `indirect` (review-p2c P2-b fix, HWSS
				// twin of the Pel/NM loop's `suppressIndirectEnv` -- see that
				// site's doc for the full MIS-partner rationale).  Was
				// previously UNGATED here (a distinct bug from the Pel/NM
				// loop's depth<=1 over-suppression): every HWSS BSDF-sampled
				// env hit at any depth survived under `indirect`, including
				// depth==1 hits whose env-NEE partner (at depth==0) was
				// suppressed just above -- a double count in the opposite
				// direction.
				const bool suppressIndirectEnv = EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) &&
					depth == 1 && !bPassedThroughSpecular;
				// review-p2d P1-1: light solo switches env-NEE off; the
				// BSDF-side partner must go with it (see
				// PTSoloSuppressEnvironment for why fractional leakage
				// is worse than either extreme).
				const bool soloSuppressEnv = PTSoloSuppressEnvironment( caster );
				// Environment contribution per wavelength
				if( scene.GetGlobalRadianceMap() )
				{
					for( unsigned int w = 0; w < SampledWavelengths::N; w++ )
					{
						if( swl.terminated[w] ) continue;
						Scalar envRadiance = scene.GetGlobalRadianceMap()->GetRadianceNM(
							currentRay, rast, swl.lambda[w] );

						// DL-74: gate on EITHER density being positive and
						// weight from the MIS PARTNER -- the RGB/NM twin's
						// rule, and `RayCasterEnvEscapeMISWeight`'s.
						// DL-170: the partner is THIS LANE's own aggregate
						// density (`misBsdfPdfComp[w]`), not the hero's --
						// see the array's declaration above.
						if( pLS && ( bsdfPdf > 0 || misBsdfPdfComp[w] > 0 ) )
						{
							const EnvironmentSampler* pES = pLS->GetEnvironmentSampler();
							if( pES )
							{
								const Scalar envPdf = pES->Pdf( currentRay.Dir() );
								if( envPdf > 0 )
								{
									Scalar w_bsdf = 1.0;
									if( misBsdfPdfComp[w] > 0 )
									{
										if( rc.pOptimalMIS && rc.pOptimalMIS->IsReady() )
										{
											const Scalar alpha = rc.pOptimalMIS->GetAlpha( rast.x, rast.y );
											w_bsdf = MISWeights::OptimalMIS2Weight( misBsdfPdfComp[w], envPdf, alpha );
										}
										else
										{
											w_bsdf = PowerHeuristic( misBsdfPdfComp[w], envPdf );
										}
									}
									envRadiance *= w_bsdf;
								}
							}
						}

						// Continuation-ray env hit -- see suppressIndirectEnv's
						// doc above for the exact depth==1 MIS-partner rule.
						if( !suppressIndirectEnv && !soloSuppressEnv ) {
							hwssResult[w] += throughputComp[w] * envRadiance;
						}
					}
				}
				else if( pRadianceMap )
				{
					for( unsigned int w = 0; w < SampledWavelengths::N; w++ )
					{
						if( swl.terminated[w] ) continue;
						if( !suppressIndirectEnv && !soloSuppressEnv ) {
							hwssResult[w] += throughputComp[w] *
								pRadianceMap->GetRadianceNM( currentRay, rast, swl.lambda[w] );
						}
					}
				}
				break;
			}
		}
		needsIntersection = true;

		// ============================================================
		// Surface hit processing (HWSS)
		// ============================================================
		const IObject* pMediumObject = 0;
		const IMedium* pCurrentMedium = MediumTracking::GetCurrentMediumWithObject(
			iorStack, &scene, pMediumObject );

		// G6 (HWSS): stamp the ambient (incident-medium) IOR from the stack
		// (per-wavelength n(λ) in the spectral path) so the GGX conductor Fresnel
		// in SPF::ScatterNM and BRDF::valueNM sees the surrounding medium rather
		// than hardcoded air.  Read BEFORE SetCurrentObject; guard to air (1.0).
		{
			const Scalar ambIOR = iorStack.top();
			ri.geometric.ambientIOR = ( ambIOR > 0.0 ) ? ambIOR : 1.0;
		}

		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		iorStack.SetCurrentObject( ri.pObject );

		// GUI render modes P2b `clay_lights` (HWSS twin of the Pel/NM
		// acquisition above): substituting clay here also makes the
		// no-BSDF NM-delegation branch immediately below unreachable
		// under clay_lights, matching the Pel/NM loop's behaviour.
		const IBSDF* pBRDFCur = EffectivePathTracingClayOverride( rc, mClayOverride ) ? pClayBRDF :
			( ri.pMaterial ? ri.pMaterial->GetBSDF() : 0 );

		// If we hit a material without BSDF mid-path (e.g. entered a
		// dielectric), fall back to per-wavelength NM for remaining path.
		// SMS double-count guard: this delegation is reached ONLY after the
		// HWSS loop has processed >= 1 BSDF (non-specular) vertex where SMS
		// was evaluated -- the first hit has a BSDF (else Fallback 1 returned)
		// and needsIntersection gates re-intersection, so any prior vertex was
		// a non-specular SMS anchor.  Pass smsHadNonSpecularShading=true so the
		// delegated NM body suppresses the BSDF-sampled emission at the light
		// that the HWSS-side SMS pass already counted.  Without it the SPF
		// emission fix (asymmetry #1: considerEmission stays true through
		// glass) would double-count diffuse->glass->light in HWSS mode.
		if( !pBRDFCur )
		{
			for( unsigned int w = 0; w < SampledWavelengths::N; w++ )
			{
				if( swl.terminated[w] )
				{
					continue;
				}
				// DL-170: forward THIS LANE's own partner
				// (`misBsdfPdfComp[w]`), not the hero's -- the delegated
				// `IntegrateFromHitNM` call is dedicated to wavelength
				// `swl.lambda[w]` and its own emitter-hit/env-escape sites
				// read this as their MIS partner at that same wavelength.
				hwssResult[w] += throughputComp[w] * IntegrateFromHitNM(
					rc, rast, ri, swl.lambda[w], scene, caster, sampler,
					pRadianceMap, depth, iorStack, bsdfPdf, 0,
					considerEmission, importance, rayType,
					diffuseBounces, glossyBounces, transmissionBounces,
					translucentBounces, volumeBounces, glossyFilterWidth,
					false, true, pAOV, misBsdfPdfComp[w] );
			}
			break;
		}

		// Check for SSS mid-path — fall back to per-wavelength
		{
			ISubSurfaceDiffusionProfile* pProfile =
				ri.pMaterial ? ri.pMaterial->GetDiffusionProfile() : 0;
			const RandomWalkSSSParams* pRWParams =
				ri.pMaterial ? ri.pMaterial->GetRandomWalkSSSParams() : 0;
			RandomWalkSSSParams rwParamsNM;
			bool hasRWNM = ri.pMaterial &&
				ri.pMaterial->GetRandomWalkSSSParamsNM( heroNM, rwParamsNM );

			// P2-d fix (review-p2b): same clay-independence reasoning as
			// the entry-point Fallback 2 routing above -- don't route away
			// from HWSS for a surface that no longer has SSS behavior
			// under clay_lights.
			if( ( pProfile || pRWParams || hasRWNM ) && !EffectivePathTracingClayOverride( rc, mClayOverride ) )
			{
				for( unsigned int w = 0; w < SampledWavelengths::N; w++ )
				{
					if( swl.terminated[w] )
					{
						continue;
					}
					hwssResult[w] += throughputComp[w] * IntegrateFromHitNM(
						rc, rast, ri, swl.lambda[w], scene, caster, sampler,
						pRadianceMap, depth, iorStack, bsdfPdf, 0,
						considerEmission, importance, rayType,
						diffuseBounces, glossyBounces, transmissionBounces,
						translucentBounces, volumeBounces, glossyFilterWidth,
						// SMS double-count guard — identical reasoning to the no-BSDF
						// (glass) delegation above: this SSS mid-path fallback is reached
						// only after >=1 non-specular SMS anchor, so pass
						// smsHadNonSpecularShading=true to suppress the BSDF-sampled
						// emission the HWSS-side SMS pass already counted.  Previously
						// dropped (defaulted false/false), double-counting HWSS
						// SMS+SSS+glass+emitter paths — the HWSS sibling of Codex Finding 2.
						//
						// DL-74 P2-1 (round-4 review): forward the incoming
						// MIS PARTNER too.  Its three siblings -- the two
						// HWSS-entry NM fallbacks and the no-BSDF (glass)
						// mid-path delegation above -- all pass a partner;
						// this one defaulted to -1 ("same as `bsdfPdf`").
						// DL-170: what they forward is now THIS LANE's own
						// `misBsdfPdfComp[w]`, not the hero's -- see the
						// no-BSDF delegation above.
						false, true, pAOV, misBsdfPdfComp[w] );
				}
				break;
			}
		}

		// Build RAY_STATE
		IRayCaster::RAY_STATE rs;
		rs.depth = depth + 1;
		rs.importance = importance;
		rs.bsdfPdf = bsdfPdf;
		// DL-74: no guiding in the HWSS body -- see the continuation
		// site below -- but an incoming partner that differs from
		// `bsdfPdf` (the guided volume continuation) must be carried
		// forward, not overwritten.
		rs.bsdfMisPdf = bsdfMisPdf;
		rs.considerEmission = considerEmission;
		rs.type = rayType;
		rs.diffuseBounces = diffuseBounces;
		rs.glossyBounces = glossyBounces;
		rs.transmissionBounces = transmissionBounces;
		rs.translucentBounces = translucentBounces;
		rs.glossyFilterWidth = glossyFilterWidth;

		// ============================================================
		// PART 1: Emission (HWSS — all wavelengths)
		// ============================================================
		{
			IEmitter* pEmitter = ri.pMaterial ? ri.pMaterial->GetEmitter() : 0;
			// GUI render modes P2b `light solo` (HWSS twin -- see the
			// Pel/NM loop's PART 1 for the full rationale): suppress a
			// BSDF-sampled hit's emission unless the hit object IS the
			// soloed mesh luminary.
			const bool soloSuppressEmissionHW = pLS && pLS->IsSoloActive() &&
				!( ri.pObject && pLS->IsSoloTargetLuminary( ri.pObject ) );
			if( pEmitter && considerEmission && !soloSuppressEmissionHW )
			{
				for( unsigned int w = 0; w < SampledWavelengths::N; w++ )
				{
					if( swl.terminated[w] ) continue;

					Scalar emission = pEmitter->emittedRadianceNM(
						ri.geometric, -ri.geometric.ray.Dir(), ri.geometric.vGeomNormal,
						swl.lambda[w] );

					// An emitter on geometry that cannot be uniformly area-sampled (CanBeAreaLight()
					// false, OR NO geometry at all -- e.g. a csg_object) is NOT in the NEE light
					// set (LuminaryManager skips it), so the light-sampling strategy's pdf for
					// this BSDF hit is ZERO -> the emission must take FULL weight.  Skipping the
					// block leaves it unweighted (and un-zeroed under RIS, so it is not lost).
					// See the RGB PART 1 block above for the crash-fix-round-2 rationale on why
					// the null case is `pEmitGeomHW && ...` (NOT NEE-sampleable) rather than the
					// previous inverted `!pEmitGeomHW || ...`.
					const IGeometry* pEmitGeomHW = ri.pObject ? ri.pObject->GetGeometry() : 0;
					const bool emitterNeeSampleableHW = ( pEmitGeomHW && pEmitGeomHW->CanBeAreaLight() );
					// DL-74: gate on EITHER density; the weight below uses
					// the MIS partner (RGB/NM twin's rule).
					// DL-170: the partner is THIS LANE's own aggregate
					// density (`misBsdfPdfComp[w]`), not the hero's -- see
					// the array's declaration above PART 1/2/3.
					if( ( bsdfPdf > 0 || misBsdfPdfComp[w] > 0 ) && ri.pObject && emitterNeeSampleableHW )
					{
						const Scalar area = ri.pObject->GetArea();
						if( area > 0 )
						{
							const Scalar cosLight = fabs( Vector3Ops::Dot(
								ri.geometric.ray.Dir(), ri.geometric.vGeomNormal ) );
							if( cosLight > 0 )
							{
								const Scalar dist = Vector3Ops::Magnitude(
									Vector3Ops::mkVector3(
										ri.geometric.ptIntersection,
										ri.geometric.ray.origin ) );

								if( pLS && pLS->IsRISActive() )
								{
									emission = 0;
								}
								else
								{
									Scalar pdfSelect = 1.0;
									if( pLS )
									{
										pdfSelect = pLS->CachedPdfSelectLuminary(
											*ri.pObject,
											ri.geometric.ray.origin,
											ri.geometric.ray.Dir() );
										if( pdfSelect <= 0 ) pdfSelect = 1.0;
									}
									const Scalar p_nee = pdfSelect * (dist * dist) / (area * cosLight);

									if( misBsdfPdfComp[w] > 0 )
									{
										if( rc.pOptimalMIS && rc.pOptimalMIS->IsReady() )
										{
											const Scalar alpha = rc.pOptimalMIS->GetAlpha(
												rast.x, rast.y );
											emission *= MISWeights::OptimalMIS2Weight(
												misBsdfPdfComp[w], p_nee, alpha );
										}
										else
										{
											emission *= PowerHeuristic( misBsdfPdfComp[w], p_nee );
										}
									}
								}
							}
						}
					}

					if( depth > 0 ) {
						emission = ClampContribution( emission, stabilityConfig.directClamp );
					}
					// GUI render modes P2b `indirect` (HWSS twin, review-p2c
					// P2-b fix): gate on MIS-partner existence, not depth
					// alone -- see the RGB/NM twin's fuller comment above.
					// depth==0 always suppressed; depth==1 suppressed only
					// when depth==0's scatter was non-delta (bPassedThrough-
					// Specular false), i.e. only when NEE actually ran (and
					// was suppressed) at depth==0.  A delta depth==0 scatter
					// (mirror/glass) has no suppressed NEE partner, so this
					// emission is the sole estimator and must survive.
					const bool suppressIndirectEmissionHW = EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) &&
						( depth == 0 || ( depth == 1 && !bPassedThroughSpecular ) );
					if( !suppressIndirectEmissionHW ) {
						hwssResult[w] += throughputComp[w] * emission;
					}
				}
			}
		}

		// ============================================================
		// PART 2: NEE (HWSS — per wavelength)
		// ============================================================
		if( pLS )
		{
			IndependentSampler fallbackSampler( rc.random );
			ISampler& neeSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;

			for( unsigned int w = 0; w < SampledWavelengths::N; w++ )
			{
				if( swl.terminated[w] ) continue;

				// P1-c fix (HWSS twin of the Pel/NM loop's fix above):
				// pBRDFCur is already pClayBRDF under clay_lights (see its
				// acquisition above) -- pass the matching clay material so
				// the MIS BSDF-sampling pdf agrees with it instead of the
				// authored ri.pMaterial.
				Scalar directNM = pLS->EvaluateDirectLightingNM(
					ri.geometric, *pBRDFCur,
					EffectivePathTracingClayOverride( rc, mClayOverride ) ? pClayMaterial : ri.pMaterial, swl.lambda[w],
					caster, neeSampler, ri.pObject, pCurrentMedium, false, pMediumObject,
					// DL-74 P2: no guiding hook in the HWSS body (it has no
					// guiding block at all), but the MIS-partner aggregate
					// pdf still has to be evaluated under the LIVE stack --
					// this loop's own escape/emitter weights partner against
					// a density the SPF produced under `iorStack`.
					/*pGuidedBlend*/ 0, &iorStack,
					// DL-196: fold in the cast-level RR compensation ONLY at
					// the FIRST vertex of this call -- the HWSS twin of the
					// RGB/NM main loop's identical PART-2 NEE site (DL-185).
					// 1.0 (no-op) for every deeper iteration of this loop.
					depth == startDepth ? castRRCompensation_ : Scalar( 1.0 ) );
				directNM = ClampContribution( directNM, stabilityConfig.directClamp );
				// GUI render modes P2b `indirect` (HWSS twin): suppress
				// NEE's direct-lighting contribution at the camera-visible
				// vertex only -- see SetIndirectOnly's doc.  Still
				// EVALUATED so the sampler stream stays in lockstep.
				if( !( EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) && depth == 0 ) ) {
					hwssResult[w] += throughputComp[w] * directNM;
				}
			}
		}

		// SMS (HWSS — per wavelength, since IOR varies).  GUI render modes
		// P2b `indirect`: deliberately NOT gated -- see the Pel/NM loop's
		// matching SMS comment above.
		if( pSolver )
		{
			const Vector3 woOutgoing = Vector3(
				-ri.geometric.ray.Dir().x,
				-ri.geometric.ray.Dir().y,
				-ri.geometric.ray.Dir().z );

			IndependentSampler fallbackSampler( rc.random );
			ISampler& smsSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;

			for( unsigned int w = 0; w < SampledWavelengths::N; w++ )
			{
				if( swl.terminated[w] ) continue;

				// Pass both geometric and shading — see other SMS sites.
				ManifoldSolver::SMSContributionNM sms = pSolver->EvaluateAtShadingPointNM(
					ri.geometric.ptIntersection,
					ri.geometric.vGeomNormal,
					ri.geometric.vNormal,
					ri.geometric.onb,
					ri.pMaterial,
					woOutgoing,
					scene,
					caster,
					smsSampler,
					swl.lambda[w] );

				if( sms.valid )
				{
					Scalar smsContribNM = sms.contribution * sms.misWeight;
					smsContribNM = ClampContribution( smsContribNM, stabilityConfig.directClamp );
					hwssResult[w] += throughputComp[w] * smsContribNM;
				}
			}
		}

		// ============================================================
		// PART 3: BSDF sampling (HWSS — hero drives, companions eval)
		// ============================================================
		// GUI render modes P2b `clay_lights` (HWSS twin of the Pel/NM PART 3
		// substitution): keeps NEE (pBRDFCur) and the continuation (pSPF)
		// consistent under clay_lights, same reasoning as the Pel/NM loop.
		const ISPF* pSPF = EffectivePathTracingClayOverride( rc, mClayOverride ) ? pClaySPF :
			( ri.pMaterial ? ri.pMaterial->GetSPF() : 0 );
		if( !pSPF ) {
			break;
		}

		ScatteredRayContainer scattered;
		{
			RISE_PROFILE_PHASE(BSDFScatter);
			RISE_PROFILE_INC(nBSDFScatterCalls);
			pSPF->ScatterNM( ri.geometric, sampler, heroNM, scattered, iorStack );
		}

		if( scattered.Count() == 0 ) {
			break;
		}

		// HWSS single-sample continuation (no branching).  Select with
		// bNM=true so selection uses hero-wavelength krayNM weights —
		// matches the selectProb computation below.  Companion
		// wavelengths inherit the hero's selection and divide by the
		// same hero-based selectProb.
		const Scalar xi = sampler.Get1D();
		const ScatteredRay* pS = scattered.RandomlySelect( xi, true );
		if( !pS ) {
			break;
		}

		// RandomlySelect with bNM=true picks lobe i with prob
		// krayNM_i / sum_j krayNM_j (raw, not fabs — matches the CDF
		// inside ScatteredRayContainer::RandomlySelect).
		Scalar selectProb = 1.0;
		if( scattered.Count() > 1 )
		{
			Scalar totalKrayNM = 0;
			for( unsigned int li = 0; li < scattered.Count(); li++ ) {
				totalKrayNM += scattered[li].krayNM;
			}
			if( totalKrayNM > NEARZERO && pS->krayNM > NEARZERO ) {
				selectProb = pS->krayNM / totalKrayNM;
			}
		}
		if( selectProb < NEARZERO ) {
			break;
		}

		// Accurate-mode inline AOV (OIDN aux): record at the first non-delta
		// scatter on the HWSS path — glass / mirror delta vertices are walked
		// through, so a camera looking through the glass records the surface
		// BEHIND it, not the glass front face.  Hero drives; albedo/normal are
		// wavelength-independent.  Mirrors the NM/Pel hook in
		// IntegrateFromHitTemplated.
		if( pAOV && !pAOV->valid && !pS->isDelta &&
		    rc.aovPrefilterMode == OidnPrefilter::Accurate )
		{
			pAOV->normal = ri.geometric.vNormal;
			pAOV->albedo = pBRDFCur ? pBRDFCur->albedo( ri.geometric )
			                        : RISEPel( 1, 1, 1 );
			pAOV->valid = true;
		}

		// Dispersive specular termination
		if( pS->isDelta && !swl.SecondaryTerminated() )
		{
			SpecularInfo heroInfo = pSPF->GetSpecularInfoNM(
				ri.geometric, iorStack, heroNM );
			if( heroInfo.valid && heroInfo.canRefract )
			{
				for( unsigned int w = 1; w < SampledWavelengths::N; w++ )
				{
					if( swl.terminated[w] ) continue;
					SpecularInfo compInfo = pSPF->GetSpecularInfoNM(
						ri.geometric, iorStack, swl.lambda[w] );
					if( compInfo.valid && fabs( compInfo.ior - heroInfo.ior ) > 1e-8 )
					{
						swl.TerminateSecondary();
						break;
					}
				}
			}
		}

		// Hero throughput (divided by selectProb for unbiased estimator)
		const Scalar invSelectProb = 1.0 / selectProb;
		Scalar heroScatterNM = pS->krayNM * invSelectProb;
		Scalar effectiveBsdfPdf = pS->isDelta ? 0 : pS->pdf;
		Ray traceRay = pS->ray;
		const IORStack* traceIorStack = pS->ior_stack ? pS->ior_stack : &iorStack;

		// Companion throughputs at hero's direction
		Scalar compScatterNM[SampledWavelengths::N];
		compScatterNM[0] = heroScatterNM;
		for( unsigned int w = 1; w < SampledWavelengths::N; w++ )
		{
			compScatterNM[w] = 0;
			if( swl.terminated[w] ) continue;

			Scalar compWeight = -1;

			// Try SPF-provided companion evaluation first
			if( pSPF )
			{
				compWeight = pSPF->EvaluateKrayNM(
					ri.geometric, pS->ray.Dir(), pS->type,
					swl.lambda[w], iorStack );
			}

			if( compWeight < 0 && pBRDFCur )
			{
				// DL-157: the SPF branch just above already passes the
				// live `iorStack`; this fallback must too, or a stateful
				// BSDF would price the companion through the other side's
				// lobes.  Every other BSDF ignores the argument.
				compWeight = pBRDFCur->valueStatefulNM(
					pS->ray.Dir(), ri.geometric, swl.lambda[w], &iorStack );
				Scalar cosTheta = fabs( Vector3Ops::Dot(
					pS->ray.Dir(), ri.geometric.vNormal ) );
				compWeight *= cosTheta;
				if( pS->pdf > 0 ) {
					compWeight /= pS->pdf;
				}
			}

			// Companions inherit hero's selection probability — divide by
			// the same selectProb for an unbiased per-wavelength estimator.
			compScatterNM[w] = compWeight > 0 ? compWeight * invSelectProb : 0;
		}

		// eta^2 basic-radiance factor (debt 30), hero and companions alike.
		// ONE scalar for the whole bundle is correct here ONLY WHEN the
		// block above actually terminated secondary wavelengths on a
		// dispersive delta refraction: that block calls
		// pSPF->GetSpecularInfoNM(), whose base-class default is
		// `SpecularInfo()` (non-specular/invalid) -- an SPF that doesn't
		// override it (e.g. CompositeSPF) never reports `canRefract`, so
		// swl.TerminateSecondary() is never reached through this path for
		// it, and any wavelength still active here need NOT share the
		// hero's index.  For an SPF that DOES implement GetSpecularInfoNM
		// (DielectricSPF, PerfectRefractorSPF, PolishedSPF), a dispersive
		// delta refraction has already terminated the rest of the bundle
		// by this point, so the per-vertex IOR on `pS->ior_stack` (the
		// HERO's) is the only one still live and one scalar is exact.
		// This is a property of THIS PT site's termination check, not a
		// general guarantee about GetSpecularInfoNM implementers -- see
		// the BDPT/VCM/MLT HWSS eye subpath in BDPTIntegrator.cpp, which
		// has no equivalent termination and instead applies the hero's
		// krayNM to every companion at delta lobes by convention (predating
		// this factor).  Applied before RR so the survival probability
		// sees the throughput the path actually carries.
		//
		// BOUNDED ERROR when this broadcast is wrong (review round 2,
		// 2026-09-12): the unterminated case above is not merely
		// theoretical -- when a coated_material or composite_material
		// wraps a DISPERSIVE dielectric, neither CoatedSPF nor
		// CompositeSPF overrides GetSpecularInfoNM, so this PT
		// termination block never fires for it (the base-class default
		// reports non-specular/invalid), and BDPT's
		// HasDispersiveDeltaVertex (BDPTIntegrator.cpp ~6866) likewise
		// never sees a dispersive delta vertex through that wrapper --
		// the hero's etaScale is broadcast to every companion
		// wavelength regardless of its own IOR. The resulting per-
		// crossing error is exactly (n_hero/n_companion)^2 - 1: for a
		// crown-glass-class dielectric (illustrative Delta-n ~ 0.02
		// across 400-700nm, e.g. n=1.50 vs 1.52) that's about 2.6-2.7%;
		// for a high-dispersion flint-class dielectric (illustrative
		// Delta-n ~ 0.07, e.g. n=1.78 vs 1.85) it climbs to about
		// 7.4-8.0%. These are illustrative index pairs, not a specific
		// glass catalog's measured curve -- the point is the error
		// SCALES with the wrapped dielectric's dispersion, is bounded by
		// ordinary optical Delta-n magnitudes (not unbounded), and
		// compounds once per crossing on a multi-bounce path through
		// such a wrapper.
		{
			const Scalar etaScale = RadianceEtaScale( iorStack, pS->ior_stack );
			if( etaScale != Scalar( 1 ) ) {
				heroScatterNM *= etaScale;
				compScatterNM[0] = heroScatterNM;
				for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
					if( swl.terminated[w] ) continue;
					compScatterNM[w] *= etaScale;
				}
			}
		}

		// Russian roulette — use MAX over wavelengths for the survival
		// probability.  Hero-driven RR creates wavelength-dependent
		// fireflies: when the hero wavelength's surface albedo is
		// small (e.g. green hero on a red wall at 0.05), RR
		// terminates ~95 % of paths, and the rare survivors scale
		// ALL wavelengths by 1/survivalProb.  Companion wavelengths
		// with legitimately high throughput (red at 0.9) get
		// amplified ~20× on those survivors, producing persistent
		// fireflies.  Taking the max over active wavelengths of the
		// post-scatter throughput (as the current-throughput metric)
		// and of the pre-scatter throughput (as the prev metric)
		// keeps the RR decision aligned with the PATH's total
		// remaining energy; unbiasedness is preserved because every
		// wavelength is scaled by the same 1/survivalProb and the
		// estimator identity E[survived×scale] = unscaled holds
		// regardless of how survivalProb is chosen.  Mirrors the
		// MaxValue(throughput) pattern used by RGB PT.
		bool skipContinuation = false;
		{
			Scalar maxPrevThroughput = fabs( throughputComp[0] );
			Scalar maxCurrThroughput = fabs( throughputComp[0] * heroScatterNM );
			for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
				if( swl.terminated[w] ) continue;
				const Scalar p = fabs( throughputComp[w] );
				if( p > maxPrevThroughput ) maxPrevThroughput = p;
				const Scalar c = fabs( throughputComp[w] * compScatterNM[w] );
				if( c > maxCurrThroughput ) maxCurrThroughput = c;
			}
			if( maxCurrThroughput <= NEARZERO ) {
				skipContinuation = true;
			} else {
				const PathTransportUtilities::RussianRouletteResult rr =
					PathTransportUtilities::EvaluateRussianRoulette(
						depth, rrMinDepth, rrThreshold,
						maxCurrThroughput,
						maxPrevThroughput,
						sampler.Get1D() );
				if( rr.terminate ) {
					skipContinuation = true;
				} else if( rr.survivalProb < 1.0 ) {
					const Scalar rrScale = 1.0 / rr.survivalProb;
					heroScatterNM *= rrScale;
					for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
						compScatterNM[w] *= rrScale;
					}
					compScatterNM[0] = heroScatterNM;
				}
			}
		}

		// Per-type bounce limits
		IRayCaster::RAY_STATE rs2 = rs;
		rs2.depth = depth + 2;
		rs2.importance = importance * fabs( heroScatterNM );
		// DL-74: NOT a sibling of the RGB/NM site for the GUIDING half --
		// this function has no guiding block at all, so `effectiveBsdfPdf`
		// is assigned once from `pS->isDelta ? 0 : pS->pdf` and never
		// reassigned by a guided/RIS replacement.
		//
		// DL-103: it IS a sibling for the multi-lobe half.  The claim this
		// comment used to make -- "LightSampler's NEE arms also use the raw
		// material pdf, so no mismatch exists here" -- was wrong in the
		// same way the RGB/NM site was: those arms evaluate the material's
		// AGGREGATE `PdfNM()`, while `pS->pdf` is the SELECTED lobe's own
		// density.  The partner is the aggregate at the HERO wavelength,
		// which is the wavelength this continuation was sampled at
		// (`pSPF->ScatterNM(..., heroNM, ...)` above) and the one the hero
		// NEE arm weights with.
		//
		// DL-170 (CLOSED): `RAY_STATE` still carries ONE scalar partner
		// across the function-call boundary -- that is unchanged, and is
		// what `rs2.bsdfMisPdf` below is for (it is what a mid-loop
		// delegation to `IntegrateFromHitNM`, or the next call into this
		// function, would receive as its OWN single incoming partner).
		// But INSIDE this bundle's own loop, HWSS's per-lane
		// `misBsdfPdfComp[]` array (declared above, alongside
		// `throughputComp[]`) is recomputed per lane right below and is
		// what PART 1's emitter-hit block and the env-escape block above
		// actually read -- so the emitter-hit / env-escape weight for
		// companion lane `w` uses THAT lane's own aggregate `PdfNM`, the
		// same function PART 2's NEE arm evaluates at `swl.lambda[w]`.
		const Scalar misBsdfPdfHW = pS->isDelta ? Scalar( 0 ) :
			[&]() -> Scalar {
				const Scalar aggregatePdf = pSPF->PdfNM(
					ri.geometric, traceRay.Dir(), heroNM, iorStack );
				// DL-41 guard -- see the RGB/NM twin's fuller derivation.
				return aggregatePdf > 0 ? aggregatePdf : effectiveBsdfPdf;
			}();
		// Companion lanes: the IDENTICAL construction, evaluated at each
		// active companion's OWN wavelength.  No sampler draws here --
		// `PdfNM` is a pure density evaluation of the ALREADY-SAMPLED
		// `traceRay` direction, so this touches no RNG stream and cannot
		// perturb any other lane's random sequence.  `effectiveBsdfPdf`
		// (the DL-41 fallback) is deliberately the HERO's own selected-
		// lobe density for every lane, not a per-companion one: `pS->pdf`
		// is sampled once, at `heroNM`, and has no per-wavelength analogue
		// (`ScatteredRay` carries a single `pdf` field) -- the same
		// hero-only fallback `bsdfPdf`/`effectiveBsdfPdf` already use
		// everywhere else in this function.
		misBsdfPdfComp[0] = misBsdfPdfHW;
		for( unsigned int w = 1; w < SampledWavelengths::N; w++ )
		{
			if( swl.terminated[w] ) continue;
			if( pS->isDelta ) {
				misBsdfPdfComp[w] = Scalar( 0 );
				continue;
			}
			const Scalar aggregatePdfComp = pSPF->PdfNM(
				ri.geometric, traceRay.Dir(), swl.lambda[w], iorStack );
			misBsdfPdfComp[w] = aggregatePdfComp > 0 ? aggregatePdfComp : effectiveBsdfPdf;
		}
		rs2.bsdfPdf = effectiveBsdfPdf;
		rs2.bsdfMisPdf = misBsdfPdfHW;
		rs2.type = PathTracingRayType( *pS );

		if( PropagateBounceLimits( rs, rs2, *pS, &stabilityConfig ) ) {
			skipContinuation = true;
		}

		bool nextConsiderEmission = true;
		if( pS->isDelta && bSMSEnabled ) {
			nextConsiderEmission = false;
		}

		if( skipContinuation ) {
			break;
		}

		// Update iterative state
		for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
			throughputComp[w] *= compScatterNM[w];
		}
		importance = rs2.importance;
		bsdfPdf = effectiveBsdfPdf;
		// DL-74: the incoming caller-supplied partner applies to the ENTRY
		// vertex only and must not survive into the next iteration.
		// DL-103: what replaces it is the aggregate partner computed above,
		// not the selected lobe's sampling density.
		bsdfMisPdf = misBsdfPdfHW;
		considerEmission = nextConsiderEmission;
		rayType = rs2.type;
		diffuseBounces = rs2.diffuseBounces;
		glossyBounces = rs2.glossyBounces;
		transmissionBounces = rs2.transmissionBounces;
		translucentBounces = rs2.translucentBounces;
		glossyFilterWidth = rs2.glossyFilterWidth;

		// GUI render modes P2b `indirect` (review-p2c P2-b fix): HWSS twin
		// of the Pel/NM loop's per-iteration bPassedThroughSpecular update
		// (see that site) -- records whether THIS depth's scatter was delta,
		// read at the top of the NEXT iteration's emission/env-miss gate.
		bPassedThroughSpecular = pS->isDelta;

		currentRay = traceRay;
		currentRay.Advance( 1e-8 );

		if( traceIorStack != &iorStack ) {
			iorStack = *traceIorStack;
		}
	}

	// Hero result was accumulated into hwssResult[0] during the loop
	// (emission, NEE, SMS were added directly per-wavelength)
}


//////////////////////////////////////////////////////////////////////
// IntegrateRayNM — Spectral single-wavelength variant
//
// Intersects the camera ray, handles first-bounce medium transport,
// then delegates to IntegrateFromHitNM for the iterative path loop.
//////////////////////////////////////////////////////////////////////

Scalar PathTracingIntegrator::IntegrateRayNM(
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const Ray& cameraRay,
	const Scalar nm,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const IRadianceMap* pRadianceMap,
	PixelAOV* pAOV
	) const
{
	return IntegrateRayTemplated<NMTag>( rc, rast, cameraRay, scene, caster,
		sampler, pRadianceMap, pAOV, nullptr, NMTag( nm ) );
}


//////////////////////////////////////////////////////////////////////
// IntegrateRayHWSS — Hero wavelength spectral sampling variant
//
// Intersects the camera ray, handles first-bounce medium transport,
// then delegates to IntegrateFromHitHWSS for the iterative path.
//////////////////////////////////////////////////////////////////////

void PathTracingIntegrator::IntegrateRayHWSS(
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const Ray& cameraRay,
	SampledWavelengths& swl,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const IRadianceMap* pRadianceMap,
	Scalar result[SampledWavelengths::N],
	PixelAOV* pAOV
	) const
{
	for( unsigned int i = 0; i < SampledWavelengths::N; i++ ) {
		result[i] = 0;
	}

	IORStack iorStack( 1.0 );
	// Seed from the camera-ray origin: if the camera sits inside a
	// dielectric (submerged camera, camera inside a medium volume), the
	// first boundary crossing must see bFromInside==true or the
	// DielectricSPF wrong-side test drops the transmission lobe entirely.
	// Free-space cameras: the probe finds no enclosing objects, no-op.
	// Mirrors the eye-subpath seeding in BDPTIntegrator (GenerateEyeSubpath).
	IORStackSeeding::SeedFromPoint( iorStack, cameraRay.origin, scene );
	sampler.StartStream( 16 );

	// Intersect camera ray
	RayIntersection ri( cameraRay, rast );
	scene.GetObjects()->IntersectRay( ri, true, true, false );
	// Capture before primary-medium sampling: HWSS can return from a volume
	// scatter without ever entering IntegrateFromHitHWSS.
	if( pAOV ) {
		pAOV->primaryDepthCaptured = true;
		pAOV->depth = ri.geometric.bHit ? ri.geometric.range : Scalar( 0 );
	}
	if( pAOV && ri.geometric.bHit ) {
		if( !pAOV->valid && rc.aovPrefilterMode == OidnPrefilter::Fast ) {
			RayIntersectionGeometric aovGeom( ri.geometric );
			if( ri.pModifier ) ri.pModifier->Modify( aovGeom );
			pAOV->normal = aovGeom.vNormal;
			pAOV->albedo = EffectivePathTracingClayOverride( rc, mClayOverride )
				? pClayBRDF->albedo( aovGeom )
				: ( ( ri.pMaterial && ri.pMaterial->GetBSDF() )
					? ri.pMaterial->GetBSDF()->albedo( aovGeom )
					: RISEPel( 1, 1, 1 ) );
			pAOV->valid = true;
		}
	}

	// Medium transport for first bounce — use hero wavelength for
	// distance sampling; per-wavelength transmittance applied inside
	// IntegrateFromHitHWSS.
	const Scalar heroNM = swl.HeroLambda();
	const IObject* pMediumObject = 0;
	const IMedium* pCurrentMedium = MediumTracking::GetCurrentMediumWithObject(
		iorStack, &scene, pMediumObject );

	// Per-wavelength residual transmittance along an escape segment
	// (see RGB IntegrateRay).  Stays 1 (no-op) in vacuum.
	Scalar escapeTr[SampledWavelengths::N];
	for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
		escapeTr[w] = 1;
	}

	if( pCurrentMedium )
	{
		const Scalar maxDist = ri.geometric.bHit ? ri.geometric.range : RISE_INFINITY;
		const LightSampler* pLS = caster.GetLightSampler();
		IndependentSampler mediumSampler( rc.random );
		// Hero wavelength drives free-flight sampling; MIS combinedPdf
		// in distance measure (hero-driven delta tracking + wavelength-
		// independent equiangular).
		const MediumSampleOutcome mso = SampleDistanceWithEquiangularMIS_NM(
			pCurrentMedium, cameraRay, maxDist, heroNM, pLS, mediumSampler );
		const Scalar t_m = mso.t;
		const bool scattered = mso.scattered;

		if( mso.zeroContrib )
		{
			return;
		}

		if( scattered )
		{
			// ============================================================
			// CAMERA-RAY VOLUMETRIC WALK -- HWSS twin.
			//
			// Read IntegrateRayTemplated's walk first: the estimator, the
			// per-scatter MIS invariant, the loop invariants, the
			// termination rule and the indirect-only routing are all
			// derived there and are NOT restated here.  This block differs
			// from that one in exactly one structural way, which is
			// pre-existing: at a volume scatter the hero bundle SPLITS, and
			// each wavelength runs its own independent walk (its own phase
			// samples, its own directions, its own escape).  That is why
			// the walk sits INSIDE the per-wavelength loop and why the
			// surface hand-off is IntegrateFromHitNM rather than
			// IntegrateFromHitHWSS -- the original single-scatter code
			// already made that choice ("fall back to per-wavelength NM"),
			// and continuing the walk keeps it.
			//
			// Consequence for the continuation's medium sampling: after the
			// bundle has split there is no hero to drive anything, so each
			// continuation segment samples its free flight at ITS OWN
			// `lambda` rather than at `heroNM`.  Only the FIRST segment --
			// the shared camera segment, sampled before the split -- is
			// hero-driven, which is why `walkMso` starts as the hero `mso`
			// and is replaced by a per-wavelength outcome from then on.
			// This matches what the surface hand-off has always done:
			// IntegrateFromHitNM samples its media at the wavelength it was
			// given.
			//
			// The truncation this replaces cost more here than on the
			// RGB/NM path (VolumeEnvFurnaceTest fog box: -15.5 % vs -12 %)
			// because it stacked on top of the pre-existing hero-bundle env
			// deficit that cell also measures.
			// ============================================================
			const bool bIndirectOnly = EffectivePathTracingIndirectOnly( rc, mIndirectOnly );
			const bool bSoloSuppressEnv = PTSoloSuppressEnvironment( caster );
			const unsigned int rrMinDepth = stabilityConfig.rrMinDepth;
			const Scalar rrThreshold = stabilityConfig.rrThreshold;
			const IPhaseFunction* pPhase = pCurrentMedium->GetPhaseFunction();

			for( unsigned int w = 0; w < SampledWavelengths::N; w++ )
			{
				if( swl.terminated[w] ) continue;

				const Scalar lambda = swl.lambda[w];

				Scalar throughput = 1.0;
				Ray walkRay = cameraRay;
				MediumSampleOutcome walkMso = mso;
				Scalar walkT = t_m;
				Scalar walkPdf = 0;
				unsigned int volumeBounces = 0;

				for( ;; )
				{
					//
					// --- scatter event k == volumeBounces + 1 -----------
					//
					const Point3 scatterPt = walkRay.PointAtLength( walkT );
					const Vector3 wo = walkRay.Dir();

					const MediumCoefficientsNM coeff = pCurrentMedium->GetCoefficientsNM( scatterPt, lambda );
					const Scalar Tr = pCurrentMedium->EvalTransmittanceNM( walkRay, walkT, lambda );

					Scalar medWeight = 0;
					if( walkMso.useExplicitThroughput && walkMso.combinedPdf > 0 )
					{
						// MIS throughput in hero-driven HWSS: per-wavelength
						// Tr_w * sigma_s_w divided by the combined hero-driven PDF.
						medWeight = Tr * coeff.sigma_s / walkMso.combinedPdf;
					}
					else if( coeff.sigma_t > 0 && Tr > 0 )
					{
						medWeight = Tr * coeff.sigma_s / (coeff.sigma_t * Tr);
					}

					if( medWeight <= 0 ) break;

					throughput *= medWeight;

					// NEE at the scatter point.  GUI render modes P2b
					// `indirect` fix (review-p2b P2-e, HWSS twin of the
					// RGB/NM IntegrateRayTemplated fix): same
					// primary-camera-segment depth==0 reasoning -- see that
					// fix's doc for the full rationale.  Still evaluated for
					// RNG lockstep; only the contribution is zeroed.  From
					// the SECOND scatter on the vertex is genuine multiple
					// scattering, i.e. indirect, and is kept in every mode.
					if( pLS )
					{
						Scalar Ld = MediumTransport::EvaluateInScatteringNM(
							scatterPt, wo, pCurrentMedium, lambda, caster,
							pLS, sampler, rast, pMediumObject );
						if( Ld > 0 )
						{
							Scalar directContrib = throughput * Ld;
							directContrib = ClampContribution( directContrib,
								stabilityConfig.directClamp );
							if( !bIndirectOnly || volumeBounces > 0 ) {
								result[w] += directContrib;
							}
						}
					}

					// Phase function continuation
					if( !pPhase ) break;

					const Vector3 wi = pPhase->Sample( wo, sampler );
					const Scalar phasePdf = pPhase->Pdf( wo, wi );
					if( phasePdf <= NEARZERO ) break;

					const Scalar phaseVal = pPhase->Evaluate( wo, wi );
					throughput = throughput * phaseVal / phasePdf;

					// Russian roulette on the volume scatter -- same call,
					// same arguments, same ordering as the RGB/NM walk and
					// the shared main loop.
					{
						const PathTransportUtilities::RussianRouletteResult rr =
							PathTransportUtilities::EvaluateRussianRoulette(
								volumeBounces,
								rrMinDepth, rrThreshold,
								PTSurvivalMagnitude( throughput ),
								1.0,
								sampler.Get1D() );
						if( rr.terminate ) break;
						if( rr.survivalProb < 1.0 ) {
							throughput /= rr.survivalProb;
						}
					}

					walkRay = Ray( scatterPt, wi );
					walkPdf = phasePdf;
					volumeBounces++;

					//
					// --- follow the continuation ------------------------
					//
					RayIntersection ri2( walkRay, rast );
					scene.GetObjects()->IntersectRay( ri2, true, true, false );

					if( ri2.geometric.bHit )
					{
						result[w] += throughput * IntegrateFromHitNM(
							rc, rast, ri2, lambda, scene, caster,
							sampler, pRadianceMap, 1, iorStack, walkPdf, 0,
							true, 1.0, IRayCaster::RAY_STATE::eRayDiffuse,
							0, 0, 0, 0, volumeBounces, 0, false, false,
							// HWSS geometry is hero-driven. Let only the hero
							// continuation populate the shared, wavelength-independent
							// Accurate guide so companion paths cannot race to define it.
							w == 0 ? pAOV : 0 );
						break;
					}

					//
					// --- the continuation missed all geometry -----------
					//
					// Sample this wavelength's medium along it once more: a
					// scatter continues the walk, a no-scatter is the escape
					// and carries the survival weight Tr / pSurvival (== 1
					// for a bounded medium, where "no scatter" means "left
					// the medium's AABB") rather than Tr itself, which would
					// double-count the attenuation the survival probability
					// already encodes (G1-c).
					//
					Scalar escapeWeight;
					if( volumeBounces < stabilityConfig.maxVolumeBounce )
					{
						const MediumSampleOutcome mso2 = SampleDistanceWithEquiangularMIS_NM(
							pCurrentMedium, walkRay, RISE_INFINITY, lambda, pLS, mediumSampler );

						if( mso2.zeroContrib ) break;

						if( mso2.scattered ) {
							walkMso = mso2;
							walkT = mso2.t;
							continue;			// next scatter event
						}

						const Scalar TrEsc = pCurrentMedium->EvalTransmittanceNM(
							walkRay, RISE_INFINITY, lambda );
						const Scalar pSurvival = mso2.noScatterPdfScale *
							pCurrentMedium->EvalDistancePdfNM(
								walkRay, RISE_INFINITY, /*scattered=*/false, RISE_INFINITY, lambda );
						escapeWeight = ( pSurvival > 0.0 ) ? ( TrEsc / pSurvival ) : TrEsc;
					}
					else
					{
						// Bounce cap: close the path with the deterministic
						// Beer-Lambert escape -- the estimator this block
						// replaced -- so the cap loses only the tail beyond
						// it rather than the escape as well.
						escapeWeight = pCurrentMedium->EvalTransmittanceNM(
							walkRay, RISE_INFINITY, lambda );
					}

					// MIS PARTNER RULE -- HWSS twin of the RGB/NM
					// IntegrateRayTemplated volume escape; see that site's
					// doc for the full derivation (phase-sampled env hit is
					// env-NEE's MIS partner; MediumScatterMaterial::Pdf is
					// the pdf the NEE side weighs against; the phase pdf is
					// symmetric in its two arguments; no delta phase
					// function exists; optimal-MIS training deliberately not
					// accumulated).  envPdf/walkPdf MUST be recomputed
					// per-wavelength: pPhase->Sample above runs inside the
					// per-wavelength loop, so each wavelength holds its OWN
					// sampled direction, and EnvironmentSampler::Pdf of that
					// direction generically differs across the bundle (any
					// HG medium or non-uniform env map).  They coincide only
					// in the isotropic-phase / uniform-env special case the
					// VolumeEnvFurnaceTest scene exercises -- do NOT hoist
					// this out of the loop as a "redundant" recomputation.
					// `walkPdf` is the pdf of the LAST phase sample, i.e. of
					// the very vertex whose NEE this partners.
					const bool bDirectPartner = ( volumeBounces == 1 );
					if( ( !bIndirectOnly || !bDirectPartner ) &&
						!bSoloSuppressEnv && scene.GetGlobalRadianceMap() )
					{
						Scalar envRadiance =
							scene.GetGlobalRadianceMap()->GetRadianceNM(
								walkRay, rast, lambda );

						if( pLS && walkPdf > 0 )
						{
							const EnvironmentSampler* pES = pLS->GetEnvironmentSampler();
							if( pES )
							{
								const Scalar envPdf = pES->Pdf( walkRay.Dir() );
								if( envPdf > 0 )
								{
									Scalar w_phase;
									if( rc.pOptimalMIS && rc.pOptimalMIS->IsReady() )
									{
										const Scalar alpha = rc.pOptimalMIS->GetAlpha( rast.x, rast.y );
										w_phase = MISWeights::OptimalMIS2Weight( walkPdf, envPdf, alpha );
									}
									else
									{
										w_phase = PowerHeuristic( walkPdf, envPdf );
									}
									envRadiance *= w_phase;
								}
							}
						}

						result[w] += throughput * escapeWeight * envRadiance;
					}
					break;
				}
			}
			return;
		}
		else if( ri.geometric.bHit )
		{
			// HWSS-3 camera-first-bounce surface-hit: no-scatter survival reweight
			// (G1-c).  IntegrateFromHitHWSS fills result[w] from throughput=1; we
			// then scale by Tr_w / (noScatterPdfScale * pSurvivalHero) -- the HERO
			// survival pdf, since the free-flight was sampled at heroNM -- NOT the
			// full Tr_w (which double-counts Beer-Lambert).  HWSS twin of the
			// non-HWSS camera surface site at ~3376.
			const Scalar pSurvivalHero = mso.noScatterPdfScale *
				pCurrentMedium->EvalDistancePdfNM(
					cameraRay, ri.geometric.range, /*scattered=*/false, ri.geometric.range, heroNM );
			Scalar Tr[SampledWavelengths::N];
			for( unsigned int w = 0; w < SampledWavelengths::N; w++ )
			{
				Tr[w] = swl.terminated[w] ? 0 :
					pCurrentMedium->EvalTransmittanceNM(
						cameraRay, ri.geometric.range, swl.lambda[w] );
			}

			IntegrateFromHitHWSS( rc, rast, ri, swl, scene, caster,
				sampler, pRadianceMap, 0, iorStack,
				0, true, 1.0, IRayCaster::RAY_STATE::eRayView,
				0, 0, 0, 0, 0, 0, result, pAOV );

			for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
				result[w] *= ( pSurvivalHero > 0.0 ) ? ( Tr[w] / pSurvivalHero ) : Tr[w];
			}
			return;
		}
		else
		{
			// HWSS-4 camera-first-bounce escape: no-scatter survival reweight
			// (G1-c).  The escape survival event carries the hero survival pdf, so
			// escapeTr holds Tr_w / (noScatterPdfScale * pSurvivalHero) for the env
			// contribution below -- not the full Tr_w (which double-counts
			// Beer-Lambert).  HWSS twin of the non-HWSS camera escape at ~3390.
			const Scalar pSurvivalHero = mso.noScatterPdfScale *
				pCurrentMedium->EvalDistancePdfNM(
					cameraRay, maxDist, /*scattered=*/false, maxDist, heroNM );
			for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
				const Scalar Tr = swl.terminated[w] ? Scalar(0) :
					pCurrentMedium->EvalTransmittanceNM(
						cameraRay, maxDist, swl.lambda[w] );
				escapeTr[w] = ( pSurvivalHero > 0.0 ) ? ( Tr / pSurvivalHero ) : Tr;
			}
		}
	}

	// No medium, or medium with no scatter and no surface hit
	if( !ri.geometric.bHit )
	{
		// GUI render modes P2b `indirect` (HWSS twin): directly-visible
		// background is a direct contribution -- return black under
		// indirect-only (see the RGB IntegrateRayTemplated twin).
		if( EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) ) {
			for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
				result[w] = 0;
			}
			return;
		}

		// See RGB IntegrateRay above — when isBackground=false the
		// camera-visible background stays black; indirect bounces
		// still pull from the global radiance map elsewhere.
		if( !caster.IsRadianceMapVisibleAsBackground() ) {
			for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
				result[w] = 0;
			}
			return;
		}

		// review-p2d P1-1 (HWSS twin of the Pel/NM camera-background gate).
		// Returning early is safe here: IntegrateRayHWSS zeroes result[] on
		// entry, so an unwritten result reads black rather than garbage.
		if( PTSoloSuppressEnvironment( caster ) ) {
			return;
		}

		if( pRadianceMap )
		{
			for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
				if( !swl.terminated[w] ) {
					result[w] = escapeTr[w] *
						pRadianceMap->GetRadianceNM( cameraRay, rast, swl.lambda[w] );
				}
			}
		}
		else if( scene.GetGlobalRadianceMap() )
		{
			for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
				if( !swl.terminated[w] ) {
					result[w] = escapeTr[w] *
						scene.GetGlobalRadianceMap()->GetRadianceNM(
							cameraRay, rast, swl.lambda[w] );
				}
			}
		}
		return;
	}

	IntegrateFromHitHWSS( rc, rast, ri, swl, scene, caster,
		sampler, pRadianceMap, 0, iorStack,
		0, true, 1.0, IRayCaster::RAY_STATE::eRayView,
		0, 0, 0, 0, 0, 0, result, pAOV );
}
