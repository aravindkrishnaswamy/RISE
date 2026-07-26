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

	static inline void AddPTIGuidingBackgroundSegment(
		PTIGuidingPathRecorder& recorder,
		const Ray& ray,
		const RISEPel& radiance
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
		segment->miWeight = 1.0f;
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
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type PTEvaluateDirectLighting(
		const Implementation::LightSampler* pLS, const RayIntersectionGeometric& ri,
		const IBSDF& brdf, const IMaterial* pMaterial, const IRayCaster& caster,
		ISampler& sampler, const IObject* pShadingObject, const IMedium* pMedium,
		bool isVolumeScatter, const IObject* pMediumObject, const Tag& tag );
	template<> inline RISEPel PTEvaluateDirectLighting<PelTag>(
		const Implementation::LightSampler* pLS, const RayIntersectionGeometric& ri,
		const IBSDF& brdf, const IMaterial* pMaterial, const IRayCaster& caster,
		ISampler& sampler, const IObject* pShadingObject, const IMedium* pMedium,
		bool isVolumeScatter, const IObject* pMediumObject, const PelTag& )
	{ return pLS->EvaluateDirectLighting( ri, brdf, pMaterial, caster, sampler, pShadingObject, pMedium, isVolumeScatter, pMediumObject ); }
	template<> inline Scalar PTEvaluateDirectLighting<NMTag>(
		const Implementation::LightSampler* pLS, const RayIntersectionGeometric& ri,
		const IBSDF& brdf, const IMaterial* pMaterial, const IRayCaster& caster,
		ISampler& sampler, const IObject* pShadingObject, const IMedium* pMedium,
		bool isVolumeScatter, const IObject* pMediumObject, const NMTag& tag )
	{ return pLS->EvaluateDirectLightingNM( ri, brdf, pMaterial, tag.nm, caster, sampler, pShadingObject, pMedium, isVolumeScatter, pMediumObject ); }

	// BSDF value at a surface (guiding RIS / one-sample MIS).
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type PTEvalBSDFAtSurface(
		const IBSDF* pBRDF, const Vector3& wi, const RayIntersectionGeometric& ri, const Tag& tag );
	template<> inline RISEPel PTEvalBSDFAtSurface<PelTag>(
		const IBSDF* pBRDF, const Vector3& wi, const RayIntersectionGeometric& ri, const PelTag& )
	{ return PathVertexEval::EvalBSDFAtSurface( pBRDF, wi, ri ); }
	template<> inline Scalar PTEvalBSDFAtSurface<NMTag>(
		const IBSDF* pBRDF, const Vector3& wi, const RayIntersectionGeometric& ri, const NMTag& tag )
	{ return PathVertexEval::EvalBSDFAtSurfaceNM( pBRDF, wi, ri, tag.nm ); }

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
	const Tag& tag
	) const
{
	using Traits = SpectralValueTraits<Tag>;
	using Value = typename Traits::value_type;

	Value result = Traits::zero();
	Value throughput = PTValueOne<Tag>();
	Value bsdfTimesCos = bsdfTimesCos_;

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
					Scalar volRrSurvivalProb = 1.0;
#endif
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
#ifdef RISE_ENABLE_OPENPGL
							volRrSurvivalProb = rr.survivalProb;
#endif
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
					bsdfPdf = effectivePdf;
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
					throughput = throughput * PTSurvivalWeight<Tag>( Tr, pSurvival );
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
					throughput = throughput * PTSurvivalWeight<Tag>( Tr, pSurvival );
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
				// Per-object radiance map (via material)
				if( pRadianceMap )
				{
					Value envRadiance = PTEvalRadianceMap<Tag>( pRadianceMap, currentRay, rast, tag );
					if( !suppressIndirectEnv && !soloSuppressEnv ) {
						result = result + throughput * envRadiance;
					}
				}
				else if( scene.GetGlobalRadianceMap() )
				{
					Value envRadiance = PTEvalRadianceMap<Tag>(
						scene.GetGlobalRadianceMap(), currentRay, rast, tag );

					// MIS weight for BSDF-sampled environment hit
					if( pLS && bsdfPdf > 0 )
					{
						const EnvironmentSampler* pES = pLS->GetEnvironmentSampler();
						if( pES )
						{
							const Scalar envPdf = pES->Pdf( currentRay.Dir() );
							if( envPdf > 0 )
							{
								// Optimal MIS training
								if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() )
								{
									const Scalar fLum = PTPositiveMagnitude( envRadiance * bsdfTimesCos );
									const Scalar f2 = fLum * fLum;
									if( f2 > 0 && bsdfPdf > 0 )
									{
										const_cast<OptimalMISAccumulator*>(rc.pOptimalMIS)->Accumulate(
											rast.x, rast.y,
											f2, bsdfPdf, kTechniqueBSDF );
									}
								}

								Scalar w_bsdf;
								if( rc.pOptimalMIS && rc.pOptimalMIS->IsReady() )
								{
									const Scalar alpha = rc.pOptimalMIS->GetAlpha( rast.x, rast.y );
									w_bsdf = MISWeights::OptimalMIS2Weight( bsdfPdf, envPdf, alpha );
								}
								else
								{
									w_bsdf = PowerHeuristic( bsdfPdf, envPdf );
								}
								envRadiance = envRadiance * w_bsdf;
							}
						}
					}

					// Continuation-ray env hit -- see suppressIndirectEnv's
					// doc above for the exact depth==1 MIS-partner rule.
					if( !suppressIndirectEnv && !soloSuppressEnv ) {
						result = result + throughput * envRadiance;
					}

#ifdef RISE_ENABLE_OPENPGL
					if( guidingRecorder && guidingRecorder->active &&
						PTGuidingLuminance( envRadiance ) > 0 )
					{
						AddPTIGuidingBackgroundSegment( *guidingRecorder, currentRay, PTGuidingPel( envRadiance ) );
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
				// false) is NOT in the NEE light set (LuminaryManager skips it), so the
				// light-sampling strategy's pdf for this BSDF hit is ZERO -> the emission must
				// take FULL weight.  Skipping the block leaves it unweighted (and un-zeroed under
				// RIS, so it is not lost).
				const IGeometry* pEmitGeom = ri.pObject ? ri.pObject->GetGeometry() : 0;
				const bool emitterNeeSampleable = ( !pEmitGeom || pEmitGeom->CanBeAreaLight() );

				if( bsdfPdf > 0 && ri.pObject && emitterNeeSampleable )
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

								if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() )
								{
									const Scalar fLum = PTPositiveMagnitude( rawEmission * bsdfTimesCos );
									const Scalar f2 = fLum * fLum;
									if( f2 > 0 && bsdfPdf > 0 )
									{
										const_cast<OptimalMISAccumulator*>(rc.pOptimalMIS)->Accumulate(
											rast.x, rast.y,
											f2, bsdfPdf, kTechniqueBSDF );
									}
								}

								Scalar w_bsdf;
								if( rc.pOptimalMIS && rc.pOptimalMIS->IsReady() )
								{
									const Scalar alpha = rc.pOptimalMIS->GetAlpha(
										rast.x, rast.y );
									w_bsdf = MISWeights::OptimalMIS2Weight( bsdfPdf, p_nee, alpha );
								}
								else
								{
									w_bsdf = PowerHeuristic( bsdfPdf, p_nee );
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
				if( !suppressIndirectEmission ) {
					result = result + throughput * emission;
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
				const Scalar cosInGeom = Vector3Ops::Dot( ri.geometric.vGeomNormal, wo );
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
									Value directSSS = PTEvaluateDirectLighting<Tag>(
										pLS, entryRI, entryBSDF, &entryMaterial, caster,
										bssrdfSampler, ri.pObject, 0, false, 0, tag );
									Value sssDirectContrib = throughput * bssrdfWeightSpatial * directSSS;
									sssDirectContrib = ClampContribution( sssDirectContrib,
										stabilityConfig.directClamp );
									if( !( EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) && depth == 0 ) ) {
										result = result + sssDirectContrib;
									}
								}

								// BSSRDF continuation via CastRay sub-path
								{
									Value sssThroughput = bssrdfWeight;

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
										if constexpr ( Traits::is_nm ) {
											// Preserved Pel/NM asymmetry: the NM original ALSO recorded the
											// BSSRDF cosine-sampled bsdfTimesCos for the continuation's
											// optimal-MIS and counted a BSDF sample.  Optimal-MIS is Pel-only
											// at runtime (rc.pOptimalMIS is null in spectral renders), so this
											// is a structural no-op kept for parity.  The SMS suppression flags
											// above are now set for BOTH tags (Codex review Finding 2).
											rs2.bsdfTimesCos = RISEPel( std::fabs( sssThroughput ) * bssrdf.cosinePdf );
											if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() && bssrdf.cosinePdf > 0 ) {
												const_cast<OptimalMISAccumulator*>( rc.pOptimalMIS )->AccumulateCount(
													rast.x, rast.y, kTechniqueBSDF );
											}
										}

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
				const Scalar cosInGeom = Vector3Ops::Dot( ri.geometric.vGeomNormal, wo );
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
									Value directSSS = PTEvaluateDirectLighting<Tag>(
										pLS, entryRI, entryBSDF, &entryMaterial, caster,
										bssrdfSampler, ri.pObject, 0, false, 0, tag );
									Value sssDirectContrib = throughput * bssrdfWeightSpatial * directSSS;
									sssDirectContrib = ClampContribution( sssDirectContrib,
										stabilityConfig.directClamp );
									if( !( EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) && depth == 0 ) ) {
										result = result + sssDirectContrib;
									}
								}

								// BSSRDF continuation via CastRay sub-path
								{
									Value sssThroughput = bssrdfWeight;

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
										if constexpr ( Traits::is_nm ) {
											// Preserved Pel/NM asymmetry: the NM original ALSO recorded the
											// BSSRDF cosine-sampled bsdfTimesCos for the continuation's
											// optimal-MIS and counted a BSDF sample.  Optimal-MIS is Pel-only
											// at runtime (rc.pOptimalMIS is null in spectral renders), so this
											// is a structural no-op kept for parity.  The SMS suppression flags
											// above are now set for BOTH tags (Codex review Finding 2).
											rs2.bsdfTimesCos = RISEPel( std::fabs( sssThroughput ) * bssrdf.cosinePdf );
											if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() && bssrdf.cosinePdf > 0 ) {
												const_cast<OptimalMISAccumulator*>( rc.pOptimalMIS )->AccumulateCount(
													rast.x, rast.y, kTechniqueBSDF );
											}
										}

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
				rs2.importance = importance * PTSurvivalMagnitude( PTScatterKray<Tag>( *pS ) ) / selectProb;
				rs2.bsdfPdf = pS->isDelta ? 0 : pS->pdf;
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

				// Pel multiplies (throughput * kray) * (1/selectProb); NM
				// multiplies throughput * (krayNM * (1/selectProb)).  The two
				// associativities differ at the ULP level, so preserve each.
				if constexpr ( Traits::is_pel ) {
					throughput = throughput * PTScatterKray<Tag>( *pS ) * ( Scalar( 1 ) / selectProb );
				} else {
					throughput = throughput * ( PTScatterKray<Tag>( *pS ) * ( Scalar( 1 ) / selectProb ) );
				}
				importance = rs2.importance;
				bsdfPdf = rs2.bsdfPdf;
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
			Value directAll = PTEvaluateDirectLighting<Tag>(
				pLS, ri.geometric, *pBRDF,
				EffectivePathTracingClayOverride( rc, mClayOverride ) ? pClayMaterial : ri.pMaterial, caster, neeSampler,
				ri.pObject, pCurrentMedium, false, pMediumObject, tag );
			directAll = ClampContribution( directAll, stabilityConfig.directClamp );
			// GUI render modes P2b `indirect`: suppress NEE's direct-
			// lighting contribution at the camera-visible vertex only --
			// see SetIndirectOnly's doc.  Still EVALUATED (same RNG
			// consumption / shadow-ray cost as every other mode) so the
			// sampler stream stays in lockstep; only the contribution is
			// zeroed.
			if( !( EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) && depth == 0 ) ) {
				result = result + throughput * directAll;
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
			static thread_local GuidingDistributionHandle guideDist;

			if( rc.pGuidingField && rc.pGuidingField->IsTrained() &&
				depth <= rc.maxGuidingDepth && GuidingSupportsSurfaceSampling( *pS ) )
			{
				const Scalar alpha = GuidingEffectiveAlpha(
					rc.guidingAlpha, *pS, rs );

				if( alpha > NEARZERO && rc.pGuidingField->InitDistribution( guideDist,
					ri.geometric.ptIntersection,
					sampler.Get1D() ) )
				{
					if( pS->type == ScatteredRay::eRayDiffuse ) {
						rc.pGuidingField->ApplyCosineProduct( guideDist, GuidingCosineNormal( ri.geometric ) );
					}

					if( rc.guidingSamplingType == eGuidingRIS )
					{
						PathTransportUtilities::GuidingRISCandidate<Value> candidates[2];

						// Candidate 0: BSDF sample (already drawn)
						{
							PathTransportUtilities::GuidingRISCandidate<Value>& c = candidates[0];
							c.direction = pS->ray.Dir();
							c.bsdfEval = PTEvalBSDFAtSurface<Tag>(
								pBRDF, c.direction, ri.geometric, tag );
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
									pBRDF, c.direction, ri.geometric, tag );
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
							traceIorStack = &iorStack;
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
									pBRDF, guidedDir, ri.geometric, tag );
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
									traceIorStack = &iorStack;
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
								scatterThroughput = PTScatterKray<Tag>( *pS ) * (pS->pdf / combinedPdf);
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

			// Optimal MIS training
			if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() &&
				!pS->isDelta && !skipContinuation )
			{
				const_cast<OptimalMISAccumulator*>(rc.pOptimalMIS)->AccumulateCount(
					rast.x, rast.y, kTechniqueBSDF );
			}

#ifdef RISE_ENABLE_OPENPGL
			// Capture pre-RR throughput so the guiding segment records
			// scatteringWeight = bsdf*cos/pdf (without RR amplification).
			// OpenPGL applies RR separately via russianRouletteSurvivalProbability.
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

			const Value bsdfTimesCosVal = pS->isDelta ? Traits::zero() :
				PTBsdfTimesCos( scatterThroughput, effectiveBsdfPdf );

			// Per-type bounce limits
			IRayCaster::RAY_STATE rs2 = rs;
			rs2.depth = depth + 2;
			rs2.importance = importance * PTSurvivalMagnitude( scatterThroughput );
			rs2.bsdfPdf = effectiveBsdfPdf;
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
					( pS->ior_stack && pS->ior_stack->top() > NEARZERO ) ?
						pS->ior_stack->top() :
						( iorStack.top() > NEARZERO ? iorStack.top() : 1.0 );
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
	PixelAOV* pAOV
	) const
{
	return IntegrateFromHitTemplated<PelTag>(
		rc, rast, firstHit, scene, caster, sampler, pRadianceMap,
		startDepth, initialIorStack, bsdfPdf_, bsdfTimesCos_,
		considerEmission_, importance_, rayType_, diffuseBounces_,
		glossyBounces_, transmissionBounces_, translucentBounces_,
		volumeBounces_, glossyFilterWidth_, smsPassedThroughSpecular_,
		smsHadNonSpecularShading_, pAOV, PelTag{} );
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
	const Tag& tag
	) const
{
	if constexpr ( SpectralValueTraits<Tag>::is_pel )
	{
		return IntegrateFromHit( rc, rast, firstHit, scene, caster, sampler,
			pRadianceMap, startDepth, initialIorStack, bsdfPdf, bsdfTimesCos,
			considerEmission, importance, rayType, diffuseBounces, glossyBounces,
			transmissionBounces, translucentBounces, volumeBounces, glossyFilterWidth,
			false, false, pAOV );
	}
	else
	{
		return IntegrateFromHitNM( rc, rast, firstHit, tag.nm, scene, caster, sampler,
			pRadianceMap, startDepth, initialIorStack, bsdfPdf, bsdfTimesCos,
			considerEmission, importance, rayType, diffuseBounces, glossyBounces,
			transmissionBounces, translucentBounces, volumeBounces, glossyFilterWidth,
			false, false, pAOV );
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
	const Tag& tag
	) const
{
	using Traits = SpectralValueTraits<Tag>;
	using Value = typename Traits::value_type;

	IORStack iorStack( 1.0 );
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
#ifdef RISE_ENABLE_OIDN
		const bool aovUseFirstHit = ( rc.aovPrefilterMode == OidnPrefilter::Fast );
#else
		const bool aovUseFirstHit = true;
#endif
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
			// Volume scatter on camera ray — handle inline and then
			// delegate continuation to IntegrateFromHit if we get a hit.
			const Point3 scatterPt = cameraRay.PointAtLength( t_m );
			const Vector3 wo = cameraRay.Dir();
			const PTMediumScatter<Tag> coeff = PTGetMediumScatter<Tag>( pCurrentMedium, scatterPt, tag );
			const Value Tr = PTEvalTransmittance<Tag>( pCurrentMedium, cameraRay, t_m, tag );

			Value medWeight = Traits::zero();
			if( mso.useExplicitThroughput && mso.combinedPdf > 0 )
			{
				// Equiangular-MIS throughput: Tr * sigma_s / combinedPdf.
				medWeight = PTDivByScalar( Tr * coeff.sigma_s, mso.combinedPdf );
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
				return Traits::zero();
			}

			Value result = Traits::zero();

			// NEE at scatter point.  GUI render modes P2b `indirect` fix
			// (review-p2b P2-e): this in-scattering NEE is on the PRIMARY
			// camera-ray segment -- the medium/volumetric analog of the
			// depth==0 surface vertex the loop's own NEE gate (see
			// SetIndirectOnly's doc) suppresses.  There is no explicit
			// `depth` variable at this call site (it runs before the main
			// loop's first iteration -- IntegrateFromHitForTag below is
			// invoked with startDepth=1, i.e. the CONTINUATION ray is
			// already past the camera-visible vertex), so the depth==0
			// rule applies unconditionally here rather than via a depth
			// comparison: a camera ray that scatters in fog before ever
			// reaching a surface is exactly the "direct light at the
			// camera-visible point" case indirect-only must suppress --
			// otherwise a camera in fog with a lamp still shows the direct
			// single-scatter glow.  Still EVALUATED (same RNG consumption/
			// shadow-ray cost as every other mode, matching the surface
			// site's lockstep contract) so the sampler stream stays in
			// lockstep across modes; only the contribution is zeroed.
			if( pLS )
			{
				Value Ld = PTEvaluateInScattering<Tag>(
					scatterPt, wo, pCurrentMedium, caster, pLS,
					sampler, rast, pMediumObject, tag );
				if( PTPositiveMagnitude( Ld ) > 0 )
				{
					Value directContrib = medWeight * Ld;
					directContrib = ClampContribution( directContrib,
						stabilityConfig.directClamp );
					if( !EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) ) {
						result = result + directContrib;
					}
				}
			}

			// Sample phase function for continuation
			const IPhaseFunction* pPhase = pCurrentMedium->GetPhaseFunction();
			if( !pPhase ) {
				return result;
			}

			const Vector3 wi = pPhase->Sample( wo, sampler );
			const Scalar phasePdf = pPhase->Pdf( wo, wi );
			if( phasePdf <= NEARZERO ) {
				return result;
			}

			const Scalar phaseVal = pPhase->Evaluate( wo, wi );
			// Preserve the per-variant arithmetic exactly: the Pel path
			// builds RISEPel(s,s,s) and multiplies channel-wise; the NM
			// path evaluates `medWeight * phaseVal / phasePdf` left-to-right
			// (multiply-then-divide).  These differ at the ULP level, so the
			// two forms are kept distinct rather than unified.
			Value volThroughput;
			if constexpr ( Traits::is_pel ) {
				volThroughput = medWeight * RISEPel(
					phaseVal / phasePdf, phaseVal / phasePdf, phaseVal / phasePdf );
			} else {
				volThroughput = medWeight * phaseVal / phasePdf;
			}

			// Intersect the scattered direction
			const Ray scatteredRay( scatterPt, wi );
			RayIntersection ri2( scatteredRay, rast );
			scene.GetObjects()->IntersectRay( ri2, true, true, false );

			if( !ri2.geometric.bHit ) {
				// Environment for volume-scattered ray.  The scattered ray
				// continues through the same medium and escapes — attenuate
				// the env contribution by the transmittance along that
				// escape segment (PBRT-v4 beta *= T_maj convention).
				if( !EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) &&
					!PTSoloSuppressEnvironment( caster ) && scene.GetGlobalRadianceMap() ) {
					const Value TrEsc = PTEvalTransmittance<Tag>(
						pCurrentMedium, scatteredRay, RISE_INFINITY, tag );
					result = result + volThroughput * TrEsc *
						PTEvalRadianceMap<Tag>( scene.GetGlobalRadianceMap(), scatteredRay, rast, tag );
				}
				return result;
			}

			// Continue from the volume-scattered hit
			Value hitResult = IntegrateFromHitForTag<Tag>( rc, rast, ri2, scene, caster,
				sampler, pRadianceMap, 1, iorStack, phasePdf,
				Traits::zero(), true, 1.0,
				IRayCaster::RAY_STATE::eRayDiffuse,
				0, 0, 0, 0, 1, 0,
				pAOV, tag );

			return result + volThroughput * hitResult;
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

			Value hitResult = IntegrateFromHitForTag<Tag>( rc, rast, ri, scene, caster,
				sampler, pRadianceMap, 0, iorStack,
				0, Traits::zero(), true, 1.0,
				IRayCaster::RAY_STATE::eRayView,
				0, 0, 0, 0, 0, 0,
				pAOV, tag );

			return PTSurvivalWeight<Tag>( Tr, pSurvival ) * hitResult;
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
		if( EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) ) {
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
		return escapeTr * envResult;
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
		sampler, pRadianceMap, pAOV, PelTag{} );
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
	PixelAOV* pAOV
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
		smsHadNonSpecularShading_initial, pAOV, NMTag{ nm } );
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
	PixelAOV* pAOV
	) const
{
	// Initialize results
	for( unsigned int i = 0; i < SampledWavelengths::N; i++ ) {
		hwssResult[i] = 0;
	}

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
						false, false, pAOV );
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
						false, false, pAOV );
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
					// Volume scatter in HWSS: fall back to per-wavelength NM
					// from this point since medium coefficients are wavelength-dependent
					const Point3 scatterPt = currentRay.PointAtLength( t_m );

					for( unsigned int w = 0; w < SampledWavelengths::N; w++ )
					{
						if( swl.terminated[w] ) continue;

						// Create a continuation ray from scatter point and
						// trace per-wavelength from here
						const MediumCoefficientsNM coeff = pCurrentMedium->GetCoefficientsNM( scatterPt, swl.lambda[w] );
						const Scalar Tr = pCurrentMedium->EvalTransmittanceNM( currentRay, t_m, swl.lambda[w] );

						Scalar medWeight = 0;
						if( mso.useExplicitThroughput && mso.combinedPdf > 0 )
						{
							// MIS throughput in hero-driven HWSS: per-wavelength
							// Tr_w * sigma_s_w divided by the combined hero-driven PDF.
							medWeight = Tr * coeff.sigma_s / mso.combinedPdf;
						}
						else if( coeff.sigma_t > 0 && Tr > 0 )
						{
							medWeight = Tr * coeff.sigma_s / (coeff.sigma_t * Tr);
						}

						if( medWeight <= 0 ) continue;

						// NEE at scatter point
						if( pLS )
						{
							const Vector3 wo = currentRay.Dir();
							Scalar Ld = MediumTransport::EvaluateInScatteringNM(
								scatterPt, wo, pCurrentMedium, swl.lambda[w], caster, pLS,
								sampler, rast, pMediumObject );
							if( Ld > 0 )
							{
								Scalar directContrib = throughputComp[w] * medWeight * Ld;
								directContrib = ClampContribution( directContrib,
									stabilityConfig.directClamp );
								hwssResult[w] += directContrib;
							}
						}
					}
					break;  // Volume scatter terminates the shared HWSS loop
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

						if( pLS && bsdfPdf > 0 )
						{
							const EnvironmentSampler* pES = pLS->GetEnvironmentSampler();
							if( pES )
							{
								const Scalar envPdf = pES->Pdf( currentRay.Dir() );
								if( envPdf > 0 )
								{
									Scalar w_bsdf;
									if( rc.pOptimalMIS && rc.pOptimalMIS->IsReady() )
									{
										const Scalar alpha = rc.pOptimalMIS->GetAlpha( rast.x, rast.y );
										w_bsdf = MISWeights::OptimalMIS2Weight( bsdfPdf, envPdf, alpha );
									}
									else
									{
										w_bsdf = PowerHeuristic( bsdfPdf, envPdf );
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
				hwssResult[w] += throughputComp[w] * IntegrateFromHitNM(
					rc, rast, ri, swl.lambda[w], scene, caster, sampler,
					pRadianceMap, depth, iorStack, bsdfPdf, 0,
					considerEmission, importance, rayType,
					diffuseBounces, glossyBounces, transmissionBounces,
					translucentBounces, volumeBounces, glossyFilterWidth,
					false, true, pAOV );
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
						false, true, pAOV );
				}
				break;
			}
		}

		// Build RAY_STATE
		IRayCaster::RAY_STATE rs;
		rs.depth = depth + 1;
		rs.importance = importance;
		rs.bsdfPdf = bsdfPdf;
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
					// false) is NOT in the NEE light set (LuminaryManager skips it), so the
					// light-sampling strategy's pdf for this BSDF hit is ZERO -> the emission must
					// take FULL weight.  Skipping the block leaves it unweighted (and un-zeroed under
					// RIS, so it is not lost).
					const IGeometry* pEmitGeomHW = ri.pObject ? ri.pObject->GetGeometry() : 0;
					const bool emitterNeeSampleableHW = ( !pEmitGeomHW || pEmitGeomHW->CanBeAreaLight() );
					if( bsdfPdf > 0 && ri.pObject && emitterNeeSampleableHW )
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

									if( rc.pOptimalMIS && rc.pOptimalMIS->IsReady() )
									{
										const Scalar alpha = rc.pOptimalMIS->GetAlpha(
											rast.x, rast.y );
										emission *= MISWeights::OptimalMIS2Weight(
											bsdfPdf, p_nee, alpha );
									}
									else
									{
										emission *= PowerHeuristic( bsdfPdf, p_nee );
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
					caster, neeSampler, ri.pObject, pCurrentMedium, false, pMediumObject );
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
				compWeight = pBRDFCur->valueNM(
					pS->ray.Dir(), ri.geometric, swl.lambda[w] );
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
		rs2.bsdfPdf = effectiveBsdfPdf;
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
		sampler, pRadianceMap, pAOV, NMTag( nm ) );
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
			// Volume scatter: fall back to per-wavelength NM
			const Point3 scatterPt = cameraRay.PointAtLength( t_m );
			const Vector3 wo = cameraRay.Dir();

			for( unsigned int w = 0; w < SampledWavelengths::N; w++ )
			{
				if( swl.terminated[w] ) continue;

				const MediumCoefficientsNM coeff = pCurrentMedium->GetCoefficientsNM( scatterPt, swl.lambda[w] );
				const Scalar Tr = pCurrentMedium->EvalTransmittanceNM( cameraRay, t_m, swl.lambda[w] );

				Scalar medWeight = 0;
				if( mso.useExplicitThroughput && mso.combinedPdf > 0 )
				{
					medWeight = Tr * coeff.sigma_s / mso.combinedPdf;
				}
				else if( coeff.sigma_t > 0 && Tr > 0 )
				{
					medWeight = Tr * coeff.sigma_s / (coeff.sigma_t * Tr);
				}

				if( medWeight <= 0 ) continue;

				// NEE at scatter point.  GUI render modes P2b `indirect`
				// fix (review-p2b P2-e, HWSS twin of the RGB/NM
				// IntegrateRayTemplated fix): same primary-camera-segment
				// depth==0 reasoning -- see that fix's doc for the full
				// rationale.  Still evaluated for RNG lockstep; only the
				// contribution is zeroed.
				if( pLS )
				{
					Scalar Ld = MediumTransport::EvaluateInScatteringNM(
						scatterPt, wo, pCurrentMedium, swl.lambda[w], caster,
						pLS, sampler, rast, pMediumObject );
					if( Ld > 0 )
					{
						Scalar directContrib = medWeight * Ld;
						directContrib = ClampContribution( directContrib,
							stabilityConfig.directClamp );
						if( !EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) ) {
							result[w] += directContrib;
						}
					}
				}

				// Phase function continuation
				const IPhaseFunction* pPhase = pCurrentMedium->GetPhaseFunction();
				if( pPhase )
				{
					const Vector3 wi = pPhase->Sample( wo, sampler );
					const Scalar phasePdf = pPhase->Pdf( wo, wi );
					if( phasePdf > NEARZERO )
					{
						const Scalar phaseVal = pPhase->Evaluate( wo, wi );
						Scalar volThroughput = medWeight * phaseVal / phasePdf;

						const Ray scatteredRay( scatterPt, wi );
						RayIntersection ri2( scatteredRay, rast );
						scene.GetObjects()->IntersectRay( ri2, true, true, false );

						if( !ri2.geometric.bHit )
						{
						if( !EffectivePathTracingIndirectOnly( rc, mIndirectOnly ) &&
							!PTSoloSuppressEnvironment( caster ) && scene.GetGlobalRadianceMap() )
							{
								const Scalar TrEsc = pCurrentMedium->EvalTransmittanceNM(
									scatteredRay, RISE_INFINITY, swl.lambda[w] );
								result[w] += volThroughput * TrEsc *
									scene.GetGlobalRadianceMap()->GetRadianceNM(
										scatteredRay, rast, swl.lambda[w] );
							}
						}
						else
						{
							result[w] += volThroughput * IntegrateFromHitNM(
								rc, rast, ri2, swl.lambda[w], scene, caster,
								sampler, pRadianceMap, 1, iorStack, phasePdf, 0,
								true, 1.0, IRayCaster::RAY_STATE::eRayDiffuse,
								0, 0, 0, 0, 1, 0 );
						}
					}
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
