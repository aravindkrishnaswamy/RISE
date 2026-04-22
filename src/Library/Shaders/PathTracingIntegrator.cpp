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
#include "../Interfaces/ISubSurfaceDiffusionProfile.h"
#include "../Utilities/MediumTransport.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Intersection/RayIntersection.h"
#include "../Rendering/AOVBuffers.h"
#ifdef RISE_ENABLE_OPENPGL
#include "../Utilities/PathGuidingField.h"
#endif

using namespace RISE;
using namespace RISE::Implementation;

// Shared transport utilities
using PathTransportUtilities::PowerHeuristic;
// ClampContribution is a single function template that deduces the
// value type (RISEPel for RGB, Scalar for NM/HWSS).  Callers drop the
// historical `NM` suffix and let argument deduction pick the type.
using PathTransportUtilities::ClampContribution;
using PathTransportUtilities::PropagateBounceLimits;

// RR defaults are now in StabilityConfig.  These are kept as
// documentation of the defaults but not used directly.
// static const unsigned int PT_RR_MIN_DEPTH = 3;
// static const Scalar PT_RR_THRESHOLD = 0.05;

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
			// If not scattered: transmission has no equiangular counterpart; weight = 1.
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

	static inline bool GuidingSupportsSurfaceSampling( const ScatteredRay& scat )
	{
		return !scat.isDelta && scat.type == ScatteredRay::eRayDiffuse;
	}

	static inline Vector3 GuidingCosineNormal( const RayIntersectionGeometric& rig )
	{
		Vector3 normal = rig.vNormal;
		if( Vector3Ops::Dot( rig.ray.Dir(), normal ) > NEARZERO ) {
			normal = -normal;
		}
		return normal;
	}

	static inline Scalar GuidingEffectiveAlpha(
		const Scalar baseAlpha,
		const ScatteredRayContainer& scattered,
		const IMaterial* const pMaterial,
		const IRayCaster::RAY_STATE& rs,
		const bool bNM
		)
	{
		if( baseAlpha <= NEARZERO ) {
			return 0;
		}

		Scalar totalWeight = 0;
		Scalar diffuseWeight = 0;
		bool hasNonDiffuse = false;
		bool hasTransmissive = pMaterial && pMaterial->CouldLightPassThrough();

		for( unsigned int i=0; i<scattered.Count(); i++ )
		{
			const ScatteredRay& scat = scattered[i];
			const Scalar w = bNM ? fabs( scat.krayNM ) : ColorMath::MaxValue( scat.kray );

			if( w <= NEARZERO ) {
				continue;
			}

			totalWeight += w;

			if( GuidingSupportsSurfaceSampling( scat ) ) {
				diffuseWeight += w;
			} else {
				hasNonDiffuse = true;
			}

			if( scat.isDelta ||
				scat.type == ScatteredRay::eRayRefraction ||
				scat.type == ScatteredRay::eRayTranslucent )
			{
				hasTransmissive = true;
			}
		}

		if( totalWeight <= NEARZERO || diffuseWeight <= NEARZERO ) {
			return 0;
		}

		const Scalar diffuseFraction = diffuseWeight / totalWeight;
		if( rs.type == IRayCaster::RAY_STATE::eRaySpecular || hasTransmissive ) {
			return 0;
		}

		if( hasNonDiffuse && diffuseFraction < 0.8 ) {
			return 0;
		}

		Scalar alpha = baseAlpha;
		if( hasNonDiffuse ) {
			alpha *= diffuseFraction * diffuseFraction;
			if( alpha > 0.2 ) {
				alpha = 0.2;
			}
		}

		return alpha >= 0.05 ? alpha : 0;
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

	struct PTIGuidingPathRecorder
	{
		PGLPathSegmentStorage storage;
		bool active;

		PTIGuidingPathRecorder() :
			storage( 0 ),
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
			if( !storage ) {
				storage = pglNewPathSegmentStorage();
				if( storage ) {
					pglPathSegmentStorageReserve( storage, 64 );
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
				field->AddPathSegments( storage, false, false, false );
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

	static inline PGLPathSegmentData* BeginPTIGuidingSegment(
		PTIGuidingPathRecorder& recorder,
		const RayIntersectionGeometric& rig
		)
	{
		if( !recorder.active || !recorder.storage ) {
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
		const bool isDelta
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
		segment->roughness = isDelta ? 0.0f : 1.0f;
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
// Construction / destruction
//////////////////////////////////////////////////////////////////////

PathTracingIntegrator::PathTracingIntegrator(
	const ManifoldSolverConfig& smsConfig,
	const StabilityConfig& stabilityCfg
	) :
  pSolver( 0 ),
  bSMSEnabled( smsConfig.enabled ),
  stabilityConfig( stabilityCfg )
{
	if( smsConfig.enabled )
	{
		pSolver = new ManifoldSolver( smsConfig );
	}
}

PathTracingIntegrator::~PathTracingIntegrator()
{
	safe_release( pSolver );
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
	Scalar bsdfPdf,
	const RISEPel& bsdfTimesCos_,
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
	bool splitFired_initial
	) const
{
	RISEPel result( 0, 0, 0 );
	RISEPel throughput( 1, 1, 1 );
	RISEPel bsdfTimesCos = bsdfTimesCos_;

	RayIntersection ri( firstHit );
	Ray currentRay = ri.geometric.ray;
	IORStack iorStack = initialIorStack;
	bool needsIntersection = false;

	// Firefly tracing: assigns a monotonically increasing per-pixel sample
	// ID so the output log can be grouped by sample.  Only enabled when
	// env RISE_FFTRACE_X/Y match rast.x/y AND we're at startDepth==0
	// (top-level camera path).
	const bool ff = FF_TRACE_ACTIVE( rast.x, rast.y ) && startDepth == 0;
	static thread_local unsigned long ffSampleId = 0;
	const unsigned long ffSample = ff ? (++ffSampleId) : 0;
	::RISE::FireflyTrace::PathScope ffPathScope( ff );
	if( ff ) {
		FF_TRACE( "=== SAMPLE %lu px(%u,%u) startDepth=%u firstHit.bHit=%d ===",
			ffSample, rast.x, rast.y, startDepth, (int)ri.geometric.bHit );
	}

	const unsigned int rrMinDepth = stabilityConfig.rrMinDepth;
	const Scalar rrThreshold = stabilityConfig.rrThreshold;

	// Branching threshold: split at most once per subpath, at the first
	// multi-lobe delta vertex whose normalized surviving throughput
	// exceeds stabilityConfig.branchingThreshold.  See StabilityConfig.h.
	// beta_initial = MaxValue(throughput) at entry = 1.0 for top-level
	// camera paths; for recursive CastRay entries we treat the re-entered
	// subpath as its own origin (so the threshold is always anchored to
	// "fraction of this subpath's starting energy surviving").
	const Scalar betaInitial = ColorMath::MaxValue( throughput );
	bool splitFired = splitFired_initial;

	// When SMS is active, track whether the BSDF-sampled path went
	// through a specular surface.  If it did AND there was a prior
	// non-specular shading point where SMS was evaluated, the emission
	// contribution from hitting a light is suppressed because SMS
	// already accounts for those paths.  Without the non-specular
	// check, paths like camera->glass->light would be incorrectly
	// suppressed even though no SMS evaluation covered them.
	// Initialize from caller so recursive CastRay calls (from the
	// branching code path when dielectric SPFs produce multiple scattered
	// rays) carry the suppression state from the parent call.
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

	const unsigned int maxDepth = 128;

	for( unsigned int depth = startDepth; depth < maxDepth; depth++ )
	{
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
				const Scalar maxDist = bHit ? ri.geometric.range : Scalar(1e10);
				IndependentSampler mediumSampler( rc.random );
				const MediumSampleOutcome mso = SampleDistanceWithEquiangularMIS(
					pCurrentMedium, currentRay, maxDist, pLS, mediumSampler );
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
					const MediumCoefficients coeff = pCurrentMedium->GetCoefficients( scatterPt );
					const RISEPel Tr = pCurrentMedium->EvalTransmittance( currentRay, t_m );
					const Scalar sigma_t_max = ColorMath::MaxValue( coeff.sigma_t );

					RISEPel medWeight( 0, 0, 0 );
					if( mso.useExplicitThroughput && mso.combinedPdf > 0 )
					{
						// Equiangular-MIS throughput: Tr * sigma_s / combinedPdf.
						medWeight = Tr * coeff.sigma_s * (1.0 / mso.combinedPdf);
					}
					else if( sigma_t_max > 0 )
					{
						// Legacy max-channel throughput (no positional lights /
						// outside medium bounds).  Per-channel equivalent:
						//   sigma_s[c] / sigma_t_max * exp((sigma_t_max - sigma_t[c]) * t)
						const Scalar Tr_scalar = ColorMath::MinValue( Tr );
						if( Tr_scalar > 0 ) {
							medWeight = Tr * coeff.sigma_s *
								(1.0 / (sigma_t_max * Tr_scalar));
						}
					}

					if( ColorMath::MaxValue( medWeight ) <= 0 ) {
						break;
					}

					throughput = throughput * medWeight;

					// NEE at scatter point
					if( pLS )
					{
						RISEPel Ld = MediumTransport::EvaluateInScattering(
							scatterPt, wo, pCurrentMedium, caster, pLS,
							sampler, rast, pMediumObject );
						if( ColorMath::MaxValue( Ld ) > 0 )
						{
							RISEPel directContrib = throughput * Ld;
							directContrib = ClampContribution( directContrib,
								stabilityConfig.directClamp );
							result = result + directContrib;
						}
					}

					// Sample phase function for continuation
					const IPhaseFunction* pPhase = pCurrentMedium->GetPhaseFunction();
					if( !pPhase ) {
						break;
					}

					const Vector3 wi = pPhase->Sample( wo, sampler );
					const Scalar phasePdf = pPhase->Pdf( wo, wi );
					if( phasePdf <= NEARZERO ) {
						break;
					}

					const Scalar phaseVal = pPhase->Evaluate( wo, wi );
					throughput = throughput * RISEPel(
						phaseVal / phasePdf, phaseVal / phasePdf, phaseVal / phasePdf );

					// Russian roulette on volume scatter
					{
						const PathTransportUtilities::RussianRouletteResult rr =
							PathTransportUtilities::EvaluateRussianRoulette(
								depth + volumeBounces,
								rrMinDepth, rrThreshold,
								ColorMath::MaxValue( throughput ),
								importance,
								sampler.Get1D() );
						if( rr.terminate ) {
							break;
						}
						if( rr.survivalProb < 1.0 ) {
							throughput = throughput * (1.0 / rr.survivalProb);
						}
					}

					currentRay = Ray( scatterPt, wi );
					bsdfPdf = phasePdf;
					considerEmission = true;
					volumeBounces++;
					continue;  // Re-enter loop: needsIntersection is still true
				}
				else if( !scattered && bHit )
				{
					// Surface hit through medium: apply transmittance
					const RISEPel Tr = pCurrentMedium->EvalTransmittance(
						currentRay, ri.geometric.range );
					throughput = throughput * Tr;
				}
			}

			// Miss — environment / radiance map
			if( !bHit )
			{
				// Per-object radiance map (via material)
				if( pRadianceMap )
				{
					RISEPel envRadiance = pRadianceMap->GetRadiance( currentRay, rast );
					result = result + throughput * envRadiance;
				}
				else if( scene.GetGlobalRadianceMap() )
				{
					RISEPel envRadiance = scene.GetGlobalRadianceMap()->GetRadiance(
						currentRay, rast );

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
									const Scalar fLum = ColorMath::MaxValue( envRadiance * bsdfTimesCos );
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

					result = result + throughput * envRadiance;

#ifdef RISE_ENABLE_OPENPGL
					if( guidingRecorder && guidingRecorder->active &&
						GuidingTrainingLuminance( envRadiance ) > 0 )
					{
						AddPTIGuidingBackgroundSegment( *guidingRecorder, currentRay, envRadiance );
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

		// Apply intersection modifier (bump maps etc.)
		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		// Update IOR stack
		iorStack.SetCurrentObject( ri.pObject );

		const IBSDF* pBRDF = ri.pMaterial ? ri.pMaterial->GetBSDF() : 0;

		// Build a RAY_STATE for utility functions that need it
		IRayCaster::RAY_STATE rs;
		rs.depth = depth + 1;
		rs.importance = importance;
		rs.bsdfPdf = bsdfPdf;
		rs.bsdfTimesCos = bsdfTimesCos;
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

		if( ff ) {
			FF_TRACE( "  depth=%u HIT obj=%p mat=%p pos=(%.4f,%.4f,%.4f) n=(%.4f,%.4f,%.4f) thr=(%.4f,%.4f,%.4f) psS=%d nsS=%d",
				depth, (const void*)ri.pObject, (const void*)ri.pMaterial,
				ri.geometric.ptIntersection.x, ri.geometric.ptIntersection.y, ri.geometric.ptIntersection.z,
				ri.geometric.vNormal.x, ri.geometric.vNormal.y, ri.geometric.vNormal.z,
				throughput[0], throughput[1], throughput[2],
				(int)bPassedThroughSpecular, (int)bHadNonSpecularShading );
		}

		// ============================================================
		// PART 1: Emission
		// ============================================================
		{
			IEmitter* pEmitter = ri.pMaterial ? ri.pMaterial->GetEmitter() : 0;
			if( pEmitter && considerEmission )
			{
				// When SMS is active, suppress emission from BSDF paths
				// that passed through specular surfaces, but ONLY if
				// there was a prior non-specular shading point where SMS
				// was evaluated.  Without that check, camera->glass->light
				// paths (with no diffuse receiver) would be killed.
				if( pSolver && bPassedThroughSpecular && bHadNonSpecularShading )
				{
					if( ff ) {
						RISEPel rawE = pEmitter->emittedRadiance(
							ri.geometric, -ri.geometric.ray.Dir(), ri.geometric.vNormal );
						FF_TRACE( "  depth=%u EMISSION-SUPPRESSED-BY-SMS rawE=(%.3e,%.3e,%.3e) thr=(%.3e,%.3e,%.3e)",
							depth, rawE[0], rawE[1], rawE[2],
							throughput[0], throughput[1], throughput[2] );
					}
					// Skip emission entirely; SMS handles this contribution.
				}
				else
				{
				RISEPel emission = pEmitter->emittedRadiance(
					ri.geometric, -ri.geometric.ray.Dir(), ri.geometric.vNormal );
				const RISEPel rawEmission = emission;
				Scalar emissionMiWeight = 1.0;

				if( bsdfPdf > 0 && ri.pObject )
				{
					const Scalar area = ri.pObject->GetArea();
					if( area > 0 )
					{
						const Scalar cosLight = fabs( Vector3Ops::Dot(
							ri.geometric.ray.Dir(), ri.geometric.vNormal ) );
						if( cosLight > 0 )
						{
							const Scalar dist = Vector3Ops::Magnitude(
								Vector3Ops::mkVector3(
									ri.geometric.ptIntersection,
									ri.geometric.ray.origin ) );

							if( pLS && pLS->IsRISActive() )
							{
								emissionMiWeight = 0.0;
								emission = emission * 0.0;
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
									const Scalar fLum = ColorMath::MaxValue( rawEmission * bsdfTimesCos );
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

				result = result + throughput * emission;

				if( ff ) {
					const RISEPel contrib = throughput * emission;
					FF_TRACE( "  depth=%u EMISSION rawE=(%.3e,%.3e,%.3e) mis=%.4f thr=(%.3e,%.3e,%.3e) contrib=(%.3e,%.3e,%.3e) result=(%.3e,%.3e,%.3e)",
						depth, rawEmission[0], rawEmission[1], rawEmission[2],
						emissionMiWeight,
						throughput[0], throughput[1], throughput[2],
						contrib[0], contrib[1], contrib[2],
						result[0], result[1], result[2] );
				}

#ifdef RISE_ENABLE_OPENPGL
				if( guidingSegment && depth >= 2 &&
					ColorMath::MaxValue( rawEmission ) > 0 ) {
					SetPTIGuidingDirectContribution( guidingSegment, rawEmission, emissionMiWeight );
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

			if( pProfile && pBRDF )
			{
				const Scalar cosIn = Vector3Ops::Dot( ri.geometric.vNormal,
					Vector3Ops::Normalize( -ri.geometric.ray.Dir() ) );

				if( cosIn > NEARZERO )
				{
					const Scalar Ft = pProfile->FresnelTransmission( cosIn, ri.geometric );

					if( Ft > NEARZERO )
					{
						IndependentSampler fallbackSampler( rc.random );
						ISampler& bssrdfSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;

						BSSRDFSampling::SampleResult bssrdf = BSSRDFSampling::SampleEntryPoint(
							ri.geometric, ri.pObject, ri.pMaterial, bssrdfSampler, 0 );

						if( bssrdf.valid )
						{
							const RISEPel bssrdfWeight = bssrdf.weight;
							const RISEPel bssrdfWeightSpatial = bssrdf.weightSpatial;

							RayIntersectionGeometric entryRI(
								Ray( bssrdf.entryPoint, bssrdf.scatteredRay.Dir() ),
								rast );
							entryRI.bHit = true;
							entryRI.ptIntersection = bssrdf.entryPoint;
							entryRI.vNormal = bssrdf.entryNormal;
							entryRI.onb = bssrdf.entryONB;

							const Scalar eta = pProfile->GetIOR( ri.geometric );
							BSSRDFEntryBSDF entryBSDF( pProfile, eta );
							BSSRDFEntryMaterial entryMaterial;

							const unsigned int nextTranslucentBounces = translucentBounces + 1;
							const bool skipSSS =
								nextTranslucentBounces > stabilityConfig.maxTranslucentBounce;

							if( !skipSSS )
							{
								// NEE at BSSRDF entry point
								if( pLS )
								{
									RISEPel directSSS = pLS->EvaluateDirectLighting(
										entryRI, entryBSDF, &entryMaterial, caster,
										bssrdfSampler, ri.pObject, 0, false, 0 );
									RISEPel sssDirectContrib = throughput * bssrdfWeightSpatial * directSSS;
									sssDirectContrib = ClampContribution( sssDirectContrib,
										stabilityConfig.directClamp );
									result = result + sssDirectContrib;
								}

								// BSSRDF continuation via CastRay sub-path
								{
									RISEPel sssThroughput = bssrdfWeight;

									const PathTransportUtilities::RussianRouletteResult rr =
										PathTransportUtilities::EvaluateRussianRoulette(
											depth, rrMinDepth, rrThreshold,
											importance * ColorMath::MaxValue( sssThroughput ),
											importance, bssrdfSampler.Get1D() );
									if( !rr.terminate )
									{
										if( rr.survivalProb < 1.0 ) {
											sssThroughput = sssThroughput * (1.0 / rr.survivalProb);
										}

										RISEPel cthis( 0, 0, 0 );
										Ray continuationRay = bssrdf.scatteredRay;
										continuationRay.Advance( 1e-8 );

										IRayCaster::RAY_STATE rs2;
										rs2.depth = depth + 2;
										rs2.considerEmission = true;
										rs2.importance = importance * ColorMath::MaxValue( sssThroughput );
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

										caster.CastRay( rc, rast, continuationRay,
											cthis, rs2, 0, pRadianceMap, iorStack );

										RISEPel indirect = sssThroughput * cthis;
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

			if( pRWParams && pBRDF )
			{
				const Scalar cosIn = Vector3Ops::Dot( ri.geometric.vNormal,
					Vector3Ops::Normalize( -ri.geometric.ray.Dir() ) );

				if( cosIn > NEARZERO )
				{
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
							rwSampler, 0, pRWParams->maxDepth );

						if( bssrdf.valid )
						{
							const Scalar bf = pRWParams->boundaryFilter;
							const RISEPel bssrdfWeight = bssrdf.weight * Ft * bf;
							const RISEPel bssrdfWeightSpatial = bssrdf.weightSpatial * Ft * bf;

							RayIntersectionGeometric entryRI(
								Ray( bssrdf.entryPoint, bssrdf.scatteredRay.Dir() ),
								rast );
							entryRI.bHit = true;
							entryRI.ptIntersection = bssrdf.entryPoint;
							entryRI.vNormal = bssrdf.entryNormal;
							entryRI.onb = bssrdf.entryONB;

							RandomWalkEntryBSDF entryBSDF( pRWParams->ior );
							BSSRDFEntryMaterial entryMaterial;

							const unsigned int nextTranslucentBounces = translucentBounces + 1;
							const bool skipSSS =
								nextTranslucentBounces > stabilityConfig.maxTranslucentBounce;

							if( !skipSSS )
							{
								if( pLS )
								{
									RISEPel directSSS = pLS->EvaluateDirectLighting(
										entryRI, entryBSDF, &entryMaterial, caster,
										bssrdfSampler, ri.pObject, 0, false, 0 );
									RISEPel sssDirectContrib = throughput * bssrdfWeightSpatial * directSSS;
									sssDirectContrib = ClampContribution( sssDirectContrib,
										stabilityConfig.directClamp );
									result = result + sssDirectContrib;
								}

								// BSSRDF continuation via CastRay sub-path
								{
									RISEPel sssThroughput = bssrdfWeight;

									const PathTransportUtilities::RussianRouletteResult rr =
										PathTransportUtilities::EvaluateRussianRoulette(
											depth, rrMinDepth, rrThreshold,
											importance * ColorMath::MaxValue( sssThroughput ),
											importance, bssrdfSampler.Get1D() );
									if( !rr.terminate )
									{
										if( rr.survivalProb < 1.0 ) {
											sssThroughput = sssThroughput * (1.0 / rr.survivalProb);
										}

										RISEPel cthis( 0, 0, 0 );
										Ray continuationRay = bssrdf.scatteredRay;
										continuationRay.Advance( 1e-8 );

										IRayCaster::RAY_STATE rs2;
										rs2.depth = depth + 2;
										rs2.considerEmission = true;
										rs2.importance = importance * ColorMath::MaxValue( sssThroughput );
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

										caster.CastRay( rc, rast, continuationRay,
											cthis, rs2, 0, pRadianceMap, iorStack );

										RISEPel indirect = sssThroughput * cthis;
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
			const ISPF* pSPF = ri.pMaterial ? ri.pMaterial->GetSPF() : 0;
			if( !pSPF ) {
				break;
			}

			ScatteredRayContainer scattered;
			pSPF->Scatter( ri.geometric, sampler, scattered, iorStack );

			if( scattered.Count() == 0 ) {
				break;
			}

			if( scattered.Count() > 1 )
			{
				// Consume one sampler dimension unconditionally for
				// Sobol alignment — whether we split or RandomlySelect.
				const Scalar xi = sampler.Get1D();

				// Threshold-gated split: at the first multi-lobe delta
				// vertex whose normalized throughput still exceeds the
				// threshold, spawn one continuation per scattered ray
				// (weighted by kray — deterministic, not /selectProb).
				// SMS-safe: skipped when a prior non-specular shading
				// point has been recorded, since recursive CastRay
				// cannot propagate the suppression state out-of-band.
				const Scalar normalizedThroughput =
					( betaInitial > 0 ) ?
					ColorMath::MaxValue( throughput ) / betaInitial :
					Scalar( 0 );

				// All scattered lobes must be delta.  Mixed (e.g. polished
				// with mirror coat + diffuse underlayer) would feed a
				// non-delta ray into a CastRay expecting delta-transparency
				// semantics (smsPassedThroughSpecular gets a stale value
				// from the parent scope rather than being cleared to false).
				bool allLobesDelta = true;
				for( unsigned int li = 0; li < scattered.Count(); li++ ) {
					if( !scattered[li].isDelta ) { allLobesDelta = false; break; }
				}
				// Split at the first multi-lobe delta vertex whenever the
				// normalized throughput survives the threshold.  When SMS
				// is live on THIS integrator (pSolver != 0) the split
				// block recurses via this->IntegrateFromHit with
				// splitFired_=true, which keeps pSolver accessible for
				// every downstream diffuse vertex and caps the recursion
				// fan-out at N (each recursive frame can't split again).
				// When SMS is off, the split block still uses
				// caster.CastRay (legacy path) since the dispatch-to-
				// shader-op behavior is otherwise identical.
				const bool shouldSplit =
					!splitFired &&
					allLobesDelta &&
					scattered.Count() <= 4 &&
					!bHadNonSpecularShading &&
					normalizedThroughput > stabilityConfig.branchingThreshold;

				if( shouldSplit )
				{
					splitFired = true;
					for( unsigned int i = 0; i < scattered.Count(); i++ )
					{
						ScatteredRay& scat = scattered[i];
						const Scalar scatmaxv = ColorMath::MaxValue( scat.kray );
						if( scatmaxv <= 0 ) {
							continue;
						}

						IRayCaster::RAY_STATE rs2;
						rs2.depth = depth + 1;
						rs2.importance = importance * scatmaxv;
						rs2.bsdfPdf = scat.isDelta ? 0 : scat.pdf;
						rs2.type = PathTracingRayType( scat );
						rs2.considerEmission = true;
						rs2.diffuseBounces = diffuseBounces;
						rs2.glossyBounces = glossyBounces;
						rs2.transmissionBounces = transmissionBounces;
						rs2.translucentBounces = translucentBounces;
						rs2.glossyFilterWidth = glossyFilterWidth;
						rs2.smsPassedThroughSpecular = scat.isDelta ? true : bPassedThroughSpecular;
						rs2.smsHadNonSpecularShading = bHadNonSpecularShading;
						if( PropagateBounceLimits( rs, rs2, scat, &stabilityConfig ) ) {
							continue;
						}

						Ray branchRay = scat.ray;
						branchRay.Advance( 1e-8 );
						const IORStack& branchStack =
							scat.ior_stack ? *scat.ior_stack : iorStack;

						RISEPel cthis( 0, 0, 0 );

						if( pSolver )
						{
							// SMS-preserving branch: stay inside this
							// integrator so downstream diffuse vertices
							// still see pSolver.  splitFired_=true caps
							// fan-out at N per depth.
							RayIntersection branchRI( branchRay, rast );
							branchRI.geometric.glossyFilterWidth = glossyFilterWidth;
							scene.GetObjects()->IntersectRay(
								branchRI, true, true, false );

							if( !branchRI.geometric.bHit )
							{
								if( pRadianceMap ) {
									cthis = pRadianceMap->GetRadiance( branchRay, rast );
								} else if( scene.GetGlobalRadianceMap() ) {
									cthis = scene.GetGlobalRadianceMap()->GetRadiance(
										branchRay, rast );
								}
							}
							else
							{
								cthis = IntegrateFromHit(
									rc, rast, branchRI, scene, caster,
									sampler, pRadianceMap,
									depth + 1, branchStack,
									rs2.bsdfPdf, RISEPel( 0, 0, 0 ), true,
									rs2.importance, rs2.type,
									rs2.diffuseBounces, rs2.glossyBounces,
									rs2.transmissionBounces, rs2.translucentBounces,
									volumeBounces, rs2.glossyFilterWidth,
									rs2.smsPassedThroughSpecular,
									rs2.smsHadNonSpecularShading,
									true );
							}
						}
						else
						{
							// Legacy path: no SMS active, so dispatcher
							// indirection through caster.CastRay is fine.
							caster.CastRay( rc, rast, branchRay, cthis, rs2, 0,
								pRadianceMap, branchStack );
						}

						RISEPel indirect = scat.kray * cthis;
						if( depth > 0 ) {
							indirect = ClampContribution( indirect, stabilityConfig.indirectClamp );
						}
						result = result + throughput * indirect;
					}
					break;
				}

				// Not splitting: randomly select one scattered ray and
				// continue iteratively.  Staying inside IntegrateFromHit
				// preserves SMS emission-suppression flags.
				const ScatteredRay* pS = scattered.RandomlySelect( xi, false );
				if( !pS ) {
					break;
				}

				// RandomlySelect picks lobe i with prob
				// max(kray_i)/sum_j max(kray_j).  The unbiased throughput
				// update is kray_I / selectProb — without the division, PT
				// systematically dims at every multi-lobe delta vertex and
				// feeds a shrunken throughput into downstream RR, inflating
				// the 1/survivalProb amplification factor on rare survivals.
				// BDPT does this (see BDPTIntegrator.cpp:1782-1794, 2199).
				Scalar selectProb = 1.0;
				{
					Scalar totalKrayMax = 0;
					for( unsigned int li = 0; li < scattered.Count(); li++ ) {
						totalKrayMax += ColorMath::MaxValue( scattered[li].kray );
					}
					const Scalar pSMax = ColorMath::MaxValue( pS->kray );
					if( totalKrayMax > NEARZERO && pSMax > NEARZERO ) {
						selectProb = pSMax / totalKrayMax;
					}
				}
				if( selectProb < NEARZERO ) {
					break;
				}

				IRayCaster::RAY_STATE rs2 = rs;
				rs2.depth = depth + 1;
				rs2.importance = importance *
					ColorMath::MaxValue( pS->kray ) / selectProb;
				rs2.bsdfPdf = pS->isDelta ? 0 : pS->pdf;
				rs2.type = PathTracingRayType( *pS );
				rs2.considerEmission = true;
				if( PropagateBounceLimits( rs, rs2, *pS, &stabilityConfig ) ) {
					break;
				}

				throughput = throughput * pS->kray * (1.0 / selectProb);
				importance = rs2.importance;
				bsdfPdf = rs2.bsdfPdf;
				bsdfTimesCos = RISEPel( 0, 0, 0 );
				considerEmission = true;
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

				if( ff ) {
					FF_TRACE( "  depth=%u SCAT-MULTI-SEL isDelta=%d kray=(%.3e,%.3e,%.3e) pdf=%.3e selProb=%.3e dir=(%.4f,%.4f,%.4f) thr->(%.3e,%.3e,%.3e) psS=%d nsS=%d",
						depth, (int)pS->isDelta,
						pS->kray[0], pS->kray[1], pS->kray[2],
						pS->pdf, selectProb,
						pS->ray.Dir().x, pS->ray.Dir().y, pS->ray.Dir().z,
						throughput[0], throughput[1], throughput[2],
						(int)bPassedThroughSpecular, (int)bHadNonSpecularShading );
				}

				currentRay = pS->ray;
				currentRay.Advance( 1e-8 );

				if( pS->ior_stack ) {
					iorStack = *pS->ior_stack;
				}

				continue;
			}
			else
			{
				// Single scattered ray: continue iteratively
				const Scalar xi = sampler.Get1D();
				const ScatteredRay* pS = scattered.RandomlySelect( xi, false );
				if( !pS ) {
					break;
				}

				IRayCaster::RAY_STATE rs2 = rs;
				rs2.depth = depth + 1;
				rs2.importance = importance * ColorMath::MaxValue( pS->kray );
				rs2.bsdfPdf = pS->isDelta ? 0 : pS->pdf;
				rs2.type = PathTracingRayType( *pS );
				rs2.considerEmission = true;
				if( PropagateBounceLimits( rs, rs2, *pS, &stabilityConfig ) ) {
					break;
				}

				throughput = throughput * pS->kray;
				importance = rs2.importance;
				bsdfPdf = rs2.bsdfPdf;
				bsdfTimesCos = RISEPel( 0, 0, 0 );
				considerEmission = true;
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

				if( ff ) {
					FF_TRACE( "  depth=%u SCAT-SINGLE isDelta=%d kray=(%.3e,%.3e,%.3e) pdf=%.3e dir=(%.4f,%.4f,%.4f) thr->(%.3e,%.3e,%.3e) psS=%d nsS=%d",
						depth, (int)pS->isDelta,
						pS->kray[0], pS->kray[1], pS->kray[2],
						pS->pdf,
						pS->ray.Dir().x, pS->ray.Dir().y, pS->ray.Dir().z,
						throughput[0], throughput[1], throughput[2],
						(int)bPassedThroughSpecular, (int)bHadNonSpecularShading );
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

			RISEPel directAll = pLS->EvaluateDirectLighting(
				ri.geometric, *pBRDF, ri.pMaterial, caster, neeSampler,
				ri.pObject, pCurrentMedium, false, pMediumObject );
			directAll = ClampContribution( directAll, stabilityConfig.directClamp );
			result = result + throughput * directAll;
			if( ff ) {
				const RISEPel ct = throughput * directAll;
				FF_TRACE( "  depth=%u NEE direct=(%.3e,%.3e,%.3e) thr=(%.3e,%.3e,%.3e) contrib=(%.3e,%.3e,%.3e) result=(%.3e,%.3e,%.3e)",
					depth, directAll[0], directAll[1], directAll[2],
					throughput[0], throughput[1], throughput[2],
					ct[0], ct[1], ct[2],
					result[0], result[1], result[2] );
			}

#ifdef RISE_ENABLE_OPENPGL
			if( depth >= 2 ) {
				AddPTIGuidingScatteredContribution( guidingSegment, directAll );
			}
#endif
		}

		// SMS for caustics through specular surfaces
		if( pSolver )
		{
			const Vector3 woOutgoing = Vector3(
				-ri.geometric.ray.Dir().x,
				-ri.geometric.ray.Dir().y,
				-ri.geometric.ray.Dir().z );

			IndependentSampler fallbackSampler( rc.random );
			ISampler& smsSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;

			ManifoldSolver::SMSContribution sms = pSolver->EvaluateAtShadingPoint(
				ri.geometric.ptIntersection,
				ri.geometric.vNormal,
				ri.geometric.onb,
				ri.pMaterial,
				woOutgoing,
				scene,
				caster,
				smsSampler );

			if( sms.valid )
			{
				RISEPel smsContrib = sms.contribution * sms.misWeight;
				const RISEPel smsRaw = sms.contribution;
				const RISEPel smsClampedIn = smsContrib;
				smsContrib = ClampContribution( smsContrib, stabilityConfig.directClamp );
				result = result + throughput * smsContrib;
				if( ff ) {
					const RISEPel ct = throughput * smsContrib;
					FF_TRACE( "  depth=%u SMS raw=(%.3e,%.3e,%.3e) mis=%.4f preClamp=(%.3e,%.3e,%.3e) postClamp=(%.3e,%.3e,%.3e) thr=(%.3e,%.3e,%.3e) contrib=(%.3e,%.3e,%.3e) result=(%.3e,%.3e,%.3e)",
						depth, smsRaw[0], smsRaw[1], smsRaw[2], sms.misWeight,
						smsClampedIn[0], smsClampedIn[1], smsClampedIn[2],
						smsContrib[0], smsContrib[1], smsContrib[2],
						throughput[0], throughput[1], throughput[2],
						ct[0], ct[1], ct[2],
						result[0], result[1], result[2] );
				}

#ifdef RISE_ENABLE_OPENPGL
				if( depth >= 2 ) {
					AddPTIGuidingScatteredContribution( guidingSegment, sms.contribution * sms.misWeight );
				}
#endif
			} else if( ff ) {
				FF_TRACE( "  depth=%u SMS invalid (no path)", depth );
			}

		}

		// ============================================================
		// PART 3: BSDF sampling (continue path — iterative)
		// ============================================================
		const ISPF* pSPF = ri.pMaterial ? ri.pMaterial->GetSPF() : 0;
		if( !pSPF ) {
			break;
		}

		ScatteredRayContainer scattered;
		pSPF->Scatter( ri.geometric, sampler, scattered, iorStack );

		if( scattered.Count() == 0 ) {
			break;
		}

		if( scattered.Count() > 1 )
		{
			for( unsigned int i = 0; i < scattered.Count(); i++ )
			{
				ScatteredRay& scat = scattered[i];
				const Scalar scatmaxv = ColorMath::MaxValue( scat.kray );
				if( scatmaxv > 0 )
				{
					IRayCaster::RAY_STATE rs2;
					rs2.depth = depth + 2;
					rs2.importance = importance * scatmaxv;
					rs2.bsdfPdf = scat.isDelta ? 0 : scat.pdf;
					rs2.bsdfTimesCos = scat.isDelta ? RISEPel( 0, 0, 0 ) :
						scat.kray * scat.pdf;
					rs2.type = PathTracingRayType( scat );
					rs2.diffuseBounces = diffuseBounces;
					rs2.glossyBounces = glossyBounces;
					rs2.transmissionBounces = transmissionBounces;
					rs2.translucentBounces = translucentBounces;
					rs2.glossyFilterWidth = glossyFilterWidth;
					// Propagate SMS emission-suppression state across the
					// recursive CastRay.  Without this the child path starts
					// with default false/false, so a child that goes
					// non-specular shading → glass → light would re-enable
					// emission at the light and double-count what SMS at
					// the parent shading point already sampled.  Mirrors
					// the update logic used by the iterative single-scatter
					// path below.
					rs2.smsPassedThroughSpecular = scat.isDelta;
					rs2.smsHadNonSpecularShading = bHadNonSpecularShading || !scat.isDelta;

					if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() && !scat.isDelta )
					{
						const_cast<OptimalMISAccumulator*>(rc.pOptimalMIS)->AccumulateCount(
							rast.x, rast.y, kTechniqueBSDF );
					}

					if( PropagateBounceLimits( rs, rs2, scat, &stabilityConfig ) ) {
						continue;
					}

					if( scat.isDelta && bSMSEnabled ) {
						rs2.considerEmission = false;
					} else {
						rs2.considerEmission = true;
					}

					RISEPel cthis( 0, 0, 0 );
					Ray ray = scat.ray;
					ray.Advance( 1e-8 );
					caster.CastRay( rc, rast, ray, cthis, rs2, 0,
						pRadianceMap,
						scat.ior_stack ? *scat.ior_stack : iorStack );

					RISEPel indirect = scat.kray * cthis;
					if( depth > 0 ) {
						indirect = ClampContribution( indirect, stabilityConfig.indirectClamp );
					}
					result = result + throughput * indirect;
				}
			}
			break;
		}
		else
		{
			// Single scattered ray: continue iteratively
			const Scalar xi = sampler.Get1D();
			const ScatteredRay* pS = scattered.RandomlySelect( xi, false );
			if( !pS ) {
				break;
			}

			Ray traceRay = pS->ray;
			RISEPel scatterThroughput = pS->kray;
			Scalar effectiveBsdfPdf = pS->isDelta ? 0 : pS->pdf;
			const IORStack* traceIorStack = pS->ior_stack ? pS->ior_stack : &iorStack;

#ifdef RISE_ENABLE_OPENPGL
			static thread_local GuidingDistributionHandle guideDist;

			if( rc.pGuidingField && rc.pGuidingField->IsTrained() &&
				depth <= rc.maxGuidingDepth && GuidingSupportsSurfaceSampling( *pS ) )
			{
				const Scalar alpha = GuidingEffectiveAlpha(
					rc.guidingAlpha, scattered, ri.pMaterial, rs, false );

				if( alpha > NEARZERO && rc.pGuidingField->InitDistribution( guideDist,
					ri.geometric.ptIntersection,
					sampler.Get1D() ) )
				{
					if( pS->type == ScatteredRay::eRayDiffuse ) {
						rc.pGuidingField->ApplyCosineProduct( guideDist, GuidingCosineNormal( ri.geometric ) );
					}

					if( rc.guidingSamplingType == eGuidingRIS )
					{
						PathTransportUtilities::GuidingRISCandidate<RISEPel> candidates[2];

						// Candidate 0: BSDF sample (already drawn)
						{
							PathTransportUtilities::GuidingRISCandidate<RISEPel>& c = candidates[0];
							c.direction = pS->ray.Dir();
							c.bsdfEval = PathVertexEval::EvalBSDFAtSurface(
								pBRDF, c.direction, ri.geometric );
							c.bsdfPdf = pS->pdf;
							c.guidePdf = rc.pGuidingField->Pdf( guideDist, c.direction );
							c.incomingRadPdf = rc.pGuidingField->IncomingRadiancePdf( guideDist, c.direction );
							c.cosTheta = fabs(
								Vector3Ops::Dot( c.direction, ri.geometric.vNormal ) );
							const Scalar avgBsdf = ColorMath::MaxValue( c.bsdfEval );
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
							PathTransportUtilities::GuidingRISCandidate<RISEPel>& c = candidates[1];
							Scalar guidePdf = 0;
							const Point2 xi2d( sampler.Get1D(), sampler.Get1D() );
							c.direction = rc.pGuidingField->Sample( guideDist, xi2d, guidePdf );
							c.guidePdf = guidePdf;

							if( guidePdf > NEARZERO )
							{
								c.bsdfEval = PathVertexEval::EvalBSDFAtSurface(
									pBRDF, c.direction, ri.geometric );
								c.bsdfPdf = PathVertexEval::EvalPdfAtSurface(
									pSPF, ri.geometric, c.direction, iorStack );
								c.incomingRadPdf = rc.pGuidingField->IncomingRadiancePdf( guideDist, c.direction );
								c.cosTheta = fabs(
									Vector3Ops::Dot( c.direction, ri.geometric.vNormal ) );
								const Scalar avgBsdf = ColorMath::MaxValue( c.bsdfEval );
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
								c.bsdfEval = RISEPel( 0, 0, 0 );
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
							scatterThroughput = candidates[sel].bsdfEval *
								(candidates[sel].cosTheta / risEffectivePdf);
							traceRay = Ray( pS->ray.origin, candidates[sel].direction );
							effectiveBsdfPdf = risEffectivePdf;
							traceIorStack = &iorStack;
						}
						else
						{
							scatterThroughput = RISEPel( 0, 0, 0 );
						}
					}
					else
					{
						// One-sample MIS
						const Scalar xiG = sampler.Get1D();

						if( PathTransportUtilities::ShouldUseGuidedSample( alpha, xiG ) )
						{
							Scalar guidePdf = 0;
							const Point2 xi2d( sampler.Get1D(), sampler.Get1D() );
							const Vector3 guidedDir = rc.pGuidingField->Sample( guideDist, xi2d, guidePdf );

							if( guidePdf > NEARZERO )
							{
								const RISEPel fGuided = PathVertexEval::EvalBSDFAtSurface(
									pBRDF, guidedDir, ri.geometric );
								const Scalar bsdfPdfGuided = PathVertexEval::EvalPdfAtSurface(
									pSPF, ri.geometric, guidedDir, iorStack );
								const Scalar combinedPdf =
									PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdf, bsdfPdfGuided );

								if( combinedPdf > NEARZERO )
								{
									const Scalar cosTheta = fabs(
										Vector3Ops::Dot( guidedDir, ri.geometric.vNormal ) );
									scatterThroughput = fGuided * (cosTheta / combinedPdf);
									traceRay = Ray( pS->ray.origin, guidedDir );
									effectiveBsdfPdf = combinedPdf;
									traceIorStack = &iorStack;
								}
								else
								{
									scatterThroughput = RISEPel( 0, 0, 0 );
								}
							}
							else
							{
								scatterThroughput = RISEPel( 0, 0, 0 );
							}
						}
						else
						{
							const Scalar guidePdfForBsdf = rc.pGuidingField->Pdf( guideDist, pS->ray.Dir() );
							const Scalar combinedPdf =
								PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdfForBsdf, pS->pdf );

							if( combinedPdf > NEARZERO )
							{
								scatterThroughput = pS->kray * (pS->pdf / combinedPdf);
								effectiveBsdfPdf = combinedPdf;
							}
						}
					}
				}
			}

			(void)useGuidingPathSegments;  // Used in full guiding implementation
#endif // RISE_ENABLE_OPENPGL

			bool skipContinuation = ColorMath::MaxValue( scatterThroughput ) <= NEARZERO;

			// Optimal MIS training
			if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() &&
				!pS->isDelta && !skipContinuation )
			{
				const_cast<OptimalMISAccumulator*>(rc.pOptimalMIS)->AccumulateCount(
					rast.x, rast.y, kTechniqueBSDF );
			}

			// Russian roulette
			if( !skipContinuation )
			{
				const PathTransportUtilities::RussianRouletteResult rr =
					PathTransportUtilities::EvaluateRussianRoulette(
						depth, rrMinDepth, rrThreshold,
						importance * ColorMath::MaxValue( scatterThroughput ),
						importance,
						sampler.Get1D() );
				if( rr.terminate ) {
					skipContinuation = true;
				} else if( rr.survivalProb < 1.0 ) {
					scatterThroughput = scatterThroughput * (1.0 / rr.survivalProb);
				}
			}

			const RISEPel bsdfTimesCosVal = pS->isDelta ? RISEPel( 0, 0, 0 ) :
				scatterThroughput * effectiveBsdfPdf;

			// Per-type bounce limits
			IRayCaster::RAY_STATE rs2 = rs;
			rs2.depth = depth + 2;
			rs2.importance = importance * ColorMath::MaxValue( scatterThroughput );
			rs2.bsdfPdf = effectiveBsdfPdf;
			rs2.bsdfTimesCos = bsdfTimesCosVal;
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
				SetPTIGuidingContinuation(
					guidingSegment,
					traceRay.Dir(),
					effectiveBsdfPdf,
					scatterThroughput,
					pS->isDelta );
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
			// The branching (multi-scatter) path does this at line 1612; the
			// single-scatter iterative path previously did NOT, which caused
			// diffuse-floor → BSDF-sample → glass-chain → light paths to slip
			// through the emission suppression at the light: the suppression
			// check requires `bPassedThroughSpecular && bHadNonSpecularShading`
			// and the latter was never set, so `considerEmission=true` at the
			// light + `bsdfPdf=0` from the last delta gave MIS weight 1.0 and
			// full emission was accumulated — a deterministic firefly
			// contribution of hundreds of luminance units per sample at any
			// pixel whose random BSDF sequence found this path.
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

	if( ff ) {
		const Scalar lum = ColorMath::MaxValue( result );
		FF_TRACE( "=== END SAMPLE %lu result=(%.3e,%.3e,%.3e) maxLum=%.3e ===",
			ffSample, result[0], result[1], result[2], lum );
	}

	return result;
}


//////////////////////////////////////////////////////////////////////
// IntegrateRay — RGB path tracer entry point
//
// Intersects the camera ray, handles first-bounce medium transport,
// then delegates to IntegrateFromHit for the iterative path loop.
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
	IORStack iorStack( 1.0 );
	sampler.StartStream( 16 );

	// Intersect camera ray
	RayIntersection ri( cameraRay, rast );
	scene.GetObjects()->IntersectRay( ri, true, true, false );

	// Extract first-hit AOV data for the denoiser.
	// For delta/transparent surfaces (GetBSDF()==NULL), use white
	// albedo per OIDN documentation: transparent surfaces should
	// report albedo 1 since the beauty signal is pure illumination.
	if( pAOV && ri.geometric.bHit )
	{
		pAOV->normal = ri.geometric.vNormal;

		if( ri.pMaterial && ri.pMaterial->GetBSDF() )
		{
			Ray aovRay( Point3Ops::mkPoint3(
				ri.geometric.ptIntersection, ri.geometric.vNormal ),
				-ri.geometric.vNormal );
			RayIntersectionGeometric rig( aovRay, nullRasterizerState );
			rig.ptIntersection = ri.geometric.ptIntersection;
			rig.vNormal = ri.geometric.vNormal;
			rig.onb = ri.geometric.onb;
			pAOV->albedo = ri.pMaterial->GetBSDF()->value(
				ri.geometric.vNormal, rig ) * PI;
		} else {
			// Delta/transparent surface: white albedo per OIDN spec
			pAOV->albedo = RISEPel( 1, 1, 1 );
		}
		pAOV->valid = true;
	}

	// Medium transport for first bounce
	const IObject* pMediumObject = 0;
	const IMedium* pCurrentMedium = MediumTracking::GetCurrentMediumWithObject(
		iorStack, &scene, pMediumObject );

	if( pCurrentMedium )
	{
		const Scalar maxDist = ri.geometric.bHit ? ri.geometric.range : Scalar(1e10);
		const LightSampler* pLS = caster.GetLightSampler();
		IndependentSampler mediumSampler( rc.random );
		const MediumSampleOutcome mso = SampleDistanceWithEquiangularMIS(
			pCurrentMedium, cameraRay, maxDist, pLS, mediumSampler );
		const Scalar t_m = mso.t;
		const bool scattered = mso.scattered;

		if( mso.zeroContrib )
		{
			// Equiangular strategy sampled a zero-density / out-of-bounds
			// point.  Scatter-measure sample with zero weight — do not
			// fall through to surface shading.
			return RISEPel( 0, 0, 0 );
		}

		if( scattered )
		{
			// Volume scatter on camera ray — handle inline and then
			// delegate continuation to IntegrateFromHit if we get a hit.
			const Point3 scatterPt = cameraRay.PointAtLength( t_m );
			const Vector3 wo = cameraRay.Dir();
			const MediumCoefficients coeff = pCurrentMedium->GetCoefficients( scatterPt );
			const RISEPel Tr = pCurrentMedium->EvalTransmittance( cameraRay, t_m );
			const Scalar sigma_t_max = ColorMath::MaxValue( coeff.sigma_t );

			RISEPel medWeight( 0, 0, 0 );
			if( mso.useExplicitThroughput && mso.combinedPdf > 0 )
			{
				// Equiangular-MIS throughput: Tr * sigma_s / combinedPdf.
				medWeight = Tr * coeff.sigma_s * (1.0 / mso.combinedPdf);
			}
			else if( sigma_t_max > 0 )
			{
				const Scalar Tr_scalar = ColorMath::MinValue( Tr );
				if( Tr_scalar > 0 ) {
					medWeight = Tr * coeff.sigma_s *
						(1.0 / (sigma_t_max * Tr_scalar));
				}
			}

			if( ColorMath::MaxValue( medWeight ) <= 0 ) {
				return RISEPel( 0, 0, 0 );
			}

			RISEPel result( 0, 0, 0 );

			// NEE at scatter point
			if( pLS )
			{
				RISEPel Ld = MediumTransport::EvaluateInScattering(
					scatterPt, wo, pCurrentMedium, caster, pLS,
					sampler, rast, pMediumObject );
				if( ColorMath::MaxValue( Ld ) > 0 )
				{
					RISEPel directContrib = medWeight * Ld;
					directContrib = ClampContribution( directContrib,
						stabilityConfig.directClamp );
					result = result + directContrib;
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
			RISEPel volThroughput = medWeight * RISEPel(
				phaseVal / phasePdf, phaseVal / phasePdf, phaseVal / phasePdf );

			// Intersect the scattered direction
			const Ray scatteredRay( scatterPt, wi );
			RayIntersection ri2( scatteredRay, rast );
			scene.GetObjects()->IntersectRay( ri2, true, true, false );

			if( !ri2.geometric.bHit ) {
				// Environment for volume-scattered ray
				if( scene.GetGlobalRadianceMap() ) {
					result = result + volThroughput *
						scene.GetGlobalRadianceMap()->GetRadiance( scatteredRay, rast );
				}
				return result;
			}

			// Continue from the volume-scattered hit
			RISEPel hitResult = IntegrateFromHit( rc, rast, ri2, scene, caster,
				sampler, pRadianceMap, 1, iorStack, phasePdf,
				RISEPel( 0, 0, 0 ), true, 1.0,
				IRayCaster::RAY_STATE::eRayDiffuse,
				0, 0, 0, 0, 1, 0,
				false, false, false );

			return result + volThroughput * hitResult;
		}
		else if( ri.geometric.bHit )
		{
			// Surface hit through medium: transmittance is applied
			// inside IntegrateFromHit on subsequent bounces.  For the
			// first bounce we need to note it here — but IntegrateFromHit
			// starts with throughput=1.  We scale the result instead.
			const RISEPel Tr = pCurrentMedium->EvalTransmittance(
				cameraRay, ri.geometric.range );

			if( !ri.geometric.bHit ) {
				return RISEPel( 0, 0, 0 );
			}

			RISEPel hitResult = IntegrateFromHit( rc, rast, ri, scene, caster,
				sampler, pRadianceMap, 0, iorStack,
				0, RISEPel( 0, 0, 0 ), true, 1.0,
				IRayCaster::RAY_STATE::eRayView,
				0, 0, 0, 0, 0, 0,
				false, false, false );

			return Tr * hitResult;
		}
	}

	// No medium, or medium with no scatter and no surface hit
	if( !ri.geometric.bHit )
	{
		// Environment map
		RISEPel envResult( 0, 0, 0 );
		if( pRadianceMap )
		{
			envResult = pRadianceMap->GetRadiance( cameraRay, rast );
		}
		else if( scene.GetGlobalRadianceMap() )
		{
			envResult = scene.GetGlobalRadianceMap()->GetRadiance( cameraRay, rast );
		}
		return envResult;
	}

	return IntegrateFromHit( rc, rast, ri, scene, caster,
		sampler, pRadianceMap, 0, iorStack,
		0, RISEPel( 0, 0, 0 ), true, 1.0,
		IRayCaster::RAY_STATE::eRayView,
		0, 0, 0, 0, 0, 0,
		false, false, false );
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
	bool smsHadNonSpecularShading_initial
	) const
{
	Scalar result = 0;
	Scalar throughput = 1.0;

	RayIntersection ri( firstHit );
	Ray currentRay = ri.geometric.ray;
	IORStack iorStack = initialIorStack;
	bool needsIntersection = false;

	const unsigned int rrMinDepth = stabilityConfig.rrMinDepth;
	const Scalar rrThreshold = stabilityConfig.rrThreshold;

	const LightSampler* pLS = caster.GetLightSampler();

	// SMS emission-suppression bookkeeping — matches the RGB path.  Without
	// these flags, spectral (NM / HWSS) rendering would re-enable emission
	// on BSDF-sampled camera-through-glass-to-light paths that SMS already
	// handled at the diffuse shading point, producing per-wavelength
	// fireflies in the emitter hit.  Conversely, with them we must NOT
	// broadly kill camera-glass-light paths that have no diffuse vertex at
	// all — the AND check below uses both the "passed specular" and "had
	// non-specular shading" flags exactly as the RGB path does.
	const bool bSMSEnabled = ( pSolver != 0 );
	bool bPassedThroughSpecular = smsPassedThroughSpecular_initial;
	bool bHadNonSpecularShading = smsHadNonSpecularShading_initial;

#ifdef RISE_ENABLE_OPENPGL
	// Path guiding uses RGB internally; for NM we still collect
	// training samples via the scalar→luminance conversion.
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

	const unsigned int maxDepth = 128;

	for( unsigned int depth = startDepth; depth < maxDepth; depth++ )
	{
		sampler.StartStream( 16 + depth );

		// ============================================================
		// Intersection + medium transport (skipped for first iteration)
		// ============================================================
		if( needsIntersection )
		{
			ri = RayIntersection( currentRay, rast );
			ri.geometric.glossyFilterWidth = glossyFilterWidth;
			scene.GetObjects()->IntersectRay( ri, true, true, false );

			bool bHit = ri.geometric.bHit;

			// Medium transport (spectral)
			const IObject* pMediumObject = 0;
			const IMedium* pCurrentMedium = MediumTracking::GetCurrentMediumWithObject(
				iorStack, &scene, pMediumObject );

			if( pCurrentMedium )
			{
				const Scalar maxDist = bHit ? ri.geometric.range : Scalar(1e10);
				IndependentSampler mediumSampler( rc.random );
				const MediumSampleOutcome mso = SampleDistanceWithEquiangularMIS_NM(
					pCurrentMedium, currentRay, maxDist, nm, pLS, mediumSampler );
				const Scalar t_m = mso.t;
				const bool scattered = mso.scattered;

				if( mso.zeroContrib )
				{
					break;
				}

				if( scattered && volumeBounces < stabilityConfig.maxVolumeBounce )
				{
					const Point3 scatterPt = currentRay.PointAtLength( t_m );
					const Vector3 wo = currentRay.Dir();
					const MediumCoefficientsNM coeff = pCurrentMedium->GetCoefficientsNM( scatterPt, nm );
					const Scalar Tr = pCurrentMedium->EvalTransmittanceNM( currentRay, t_m, nm );

					Scalar medWeight = 0;
					if( mso.useExplicitThroughput && mso.combinedPdf > 0 )
					{
						medWeight = Tr * coeff.sigma_s / mso.combinedPdf;
					}
					else if( coeff.sigma_t > 0 && Tr > 0 )
					{
						// Legacy single-channel albedo (sigma_s / sigma_t).
						medWeight = Tr * coeff.sigma_s / (coeff.sigma_t * Tr);
					}

					if( medWeight <= 0 ) {
						break;
					}

					throughput *= medWeight;

					// NEE at scatter point (spectral)
					if( pLS )
					{
						Scalar Ld = MediumTransport::EvaluateInScatteringNM(
							scatterPt, wo, pCurrentMedium, nm, caster, pLS,
							sampler, rast, pMediumObject );
						if( Ld > 0 )
						{
							Scalar directContrib = throughput * Ld;
							directContrib = ClampContribution( directContrib,
								stabilityConfig.directClamp );
							result += directContrib;
						}
					}

					// Sample phase function for continuation
					const IPhaseFunction* pPhase = pCurrentMedium->GetPhaseFunction();
					if( !pPhase ) {
						break;
					}

					const Vector3 wi = pPhase->Sample( wo, sampler );
					const Scalar phasePdf = pPhase->Pdf( wo, wi );
					if( phasePdf <= NEARZERO ) {
						break;
					}

					const Scalar phaseVal = pPhase->Evaluate( wo, wi );
					throughput *= phaseVal / phasePdf;

					// Russian roulette on volume scatter
					{
						const PathTransportUtilities::RussianRouletteResult rr =
							PathTransportUtilities::EvaluateRussianRoulette(
								depth + volumeBounces,
								rrMinDepth, rrThreshold,
								fabs( throughput ),
								importance,
								sampler.Get1D() );
						if( rr.terminate ) {
							break;
						}
						if( rr.survivalProb < 1.0 ) {
							throughput /= rr.survivalProb;
						}
					}

					currentRay = Ray( scatterPt, wi );
					bsdfPdf = phasePdf;
					considerEmission = true;
					volumeBounces++;
					continue;
				}
				else if( !scattered && bHit )
				{
					const Scalar Tr = pCurrentMedium->EvalTransmittanceNM(
						currentRay, ri.geometric.range, nm );
					throughput *= Tr;
				}
			}

			// Miss — environment / radiance map (spectral)
			if( !bHit )
			{
				if( pRadianceMap )
				{
					Scalar envRadiance = pRadianceMap->GetRadianceNM( currentRay, rast, nm );
					result += throughput * envRadiance;
				}
				else if( scene.GetGlobalRadianceMap() )
				{
					Scalar envRadiance = scene.GetGlobalRadianceMap()->GetRadianceNM(
						currentRay, rast, nm );

					// MIS weight for BSDF-sampled environment hit
					if( pLS && bsdfPdf > 0 )
					{
						const EnvironmentSampler* pES = pLS->GetEnvironmentSampler();
						if( pES )
						{
							const Scalar envPdf = pES->Pdf( currentRay.Dir() );
							if( envPdf > 0 )
							{
								if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() )
								{
									const Scalar fVal = envRadiance * bsdfTimesCosNM;
									const Scalar f2 = fVal * fVal;
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
								envRadiance *= w_bsdf;
							}
						}
					}

					result += throughput * envRadiance;
				}
				break;
			}
		}
		needsIntersection = true;

		// ============================================================
		// Surface hit processing (spectral)
		// ============================================================
		const IObject* pMediumObject = 0;
		const IMedium* pCurrentMedium = MediumTracking::GetCurrentMediumWithObject(
			iorStack, &scene, pMediumObject );

		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		iorStack.SetCurrentObject( ri.pObject );

		const IBSDF* pBRDF = ri.pMaterial ? ri.pMaterial->GetBSDF() : 0;

		// Build a RAY_STATE for utility functions
		IRayCaster::RAY_STATE rs;
		rs.depth = depth + 1;
		rs.importance = importance;
		rs.bsdfPdf = bsdfPdf;
		rs.bsdfTimesCos = RISEPel( bsdfTimesCosNM );
		rs.considerEmission = considerEmission;
		rs.type = rayType;
		rs.diffuseBounces = diffuseBounces;
		rs.glossyBounces = glossyBounces;
		rs.transmissionBounces = transmissionBounces;
		rs.translucentBounces = translucentBounces;
		rs.glossyFilterWidth = glossyFilterWidth;

		// ============================================================
		// PART 1: Emission (spectral)
		// ============================================================
		{
			IEmitter* pEmitter = ri.pMaterial ? ri.pMaterial->GetEmitter() : 0;
			// When SMS is active, suppress emission from BSDF paths that
			// passed through specular surfaces IF there was a prior
			// non-specular shading point where SMS was evaluated.
			// Without the bHadNonSpecularShading clause the camera looking
			// directly through glass at the light would be killed too (no
			// diffuse vertex exists for SMS to cover that contribution).
			// Mirrors the RGB path at the same position.
			const bool smsSuppressEmission = bSMSEnabled
				&& bPassedThroughSpecular && bHadNonSpecularShading;
			if( pEmitter && considerEmission && !smsSuppressEmission )
			{
				Scalar emission = pEmitter->emittedRadianceNM(
					ri.geometric, -ri.geometric.ray.Dir(), ri.geometric.vNormal, nm );

				if( bsdfPdf > 0 && ri.pObject )
				{
					const Scalar area = ri.pObject->GetArea();
					if( area > 0 )
					{
						const Scalar cosLight = fabs( Vector3Ops::Dot(
							ri.geometric.ray.Dir(), ri.geometric.vNormal ) );
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
									if( pdfSelect <= 0 ) {
										pdfSelect = 1.0;
									}
								}

								const Scalar p_nee = pdfSelect * (dist * dist) / (area * cosLight);

								if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() )
								{
									const Scalar fVal = emission * bsdfTimesCosNM;
									const Scalar f2 = fVal * fVal;
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
								emission *= w_bsdf;
							}
						}
					}
				}

				if( depth > 0 ) {
					emission = ClampContribution( emission, stabilityConfig.directClamp );
				}

				result += throughput * emission;
			}
		}

		// ============================================================
		// BSSRDF: Subsurface scattering via diffusion profile (spectral)
		// ============================================================
		{
			ISubSurfaceDiffusionProfile* pProfile =
				ri.pMaterial ? ri.pMaterial->GetDiffusionProfile() : 0;

			if( pProfile && pBRDF )
			{
				const Scalar cosIn = Vector3Ops::Dot( ri.geometric.vNormal,
					Vector3Ops::Normalize( -ri.geometric.ray.Dir() ) );

				if( cosIn > NEARZERO )
				{
					const Scalar Ft = pProfile->FresnelTransmission( cosIn, ri.geometric );

					if( Ft > NEARZERO )
					{
						IndependentSampler fallbackSampler( rc.random );
						ISampler& bssrdfSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;

						BSSRDFSampling::SampleResult bssrdf = BSSRDFSampling::SampleEntryPoint(
							ri.geometric, ri.pObject, ri.pMaterial, bssrdfSampler, nm );

						if( bssrdf.valid )
						{
							const Scalar bssrdfWeightNM = bssrdf.weightNM;
							const Scalar bssrdfWeightSpatialNM = bssrdf.weightSpatialNM;

							RayIntersectionGeometric entryRI(
								Ray( bssrdf.entryPoint, bssrdf.scatteredRay.Dir() ),
								rast );
							entryRI.bHit = true;
							entryRI.ptIntersection = bssrdf.entryPoint;
							entryRI.vNormal = bssrdf.entryNormal;
							entryRI.onb = bssrdf.entryONB;

							const Scalar eta = pProfile->GetIOR( ri.geometric );
							BSSRDFEntryBSDF entryBSDF( pProfile, eta );
							BSSRDFEntryMaterial entryMaterial;

							const unsigned int nextTranslucentBounces = translucentBounces + 1;
							const bool skipSSS =
								nextTranslucentBounces > stabilityConfig.maxTranslucentBounce;

							if( !skipSSS )
							{
								if( pLS )
								{
									Scalar directSSSNM = pLS->EvaluateDirectLightingNM(
										entryRI, entryBSDF, &entryMaterial, nm, caster,
										bssrdfSampler, ri.pObject, 0, false, 0 );
									Scalar sssDirectContribNM = throughput * bssrdfWeightSpatialNM * directSSSNM;
									sssDirectContribNM = ClampContribution( sssDirectContribNM,
										stabilityConfig.directClamp );
									result += sssDirectContribNM;
								}

								// BSSRDF continuation via CastRayNM sub-path
								{
									Scalar sssThroughputNM = bssrdfWeightNM;

									const PathTransportUtilities::RussianRouletteResult rr =
										PathTransportUtilities::EvaluateRussianRoulette(
											depth, rrMinDepth, rrThreshold,
											importance * fabs( sssThroughputNM ),
											importance, bssrdfSampler.Get1D() );
									if( !rr.terminate )
									{
										if( rr.survivalProb < 1.0 ) {
											sssThroughputNM /= rr.survivalProb;
										}

										Scalar cthis = 0;
										Ray continuationRay = bssrdf.scatteredRay;
										continuationRay.Advance( 1e-8 );

										IRayCaster::RAY_STATE rs2;
										rs2.depth = depth + 2;
										rs2.considerEmission = true;
										rs2.importance = importance * fabs( sssThroughputNM );
										rs2.bsdfPdf = bssrdf.cosinePdf;
										rs2.bsdfTimesCos = RISEPel( fabs( sssThroughputNM ) * bssrdf.cosinePdf );
										rs2.type = IRayCaster::RAY_STATE::eRayDiffuse;
										rs2.diffuseBounces = diffuseBounces;
										rs2.glossyBounces = glossyBounces;
										rs2.transmissionBounces = transmissionBounces;
										rs2.translucentBounces = nextTranslucentBounces;
										rs2.glossyFilterWidth = glossyFilterWidth;

										if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() && bssrdf.cosinePdf > 0 )
										{
											const_cast<OptimalMISAccumulator*>(rc.pOptimalMIS)->AccumulateCount(
												rast.x, rast.y, kTechniqueBSDF );
										}

										caster.CastRayNM( rc, rast, continuationRay, cthis,
											rs2, nm, 0, pRadianceMap, iorStack );

										Scalar indirectNM = sssThroughputNM * cthis;
										if( depth > 0 ) {
											indirectNM = ClampContribution( indirectNM,
												stabilityConfig.indirectClamp );
										}
										result += throughput * indirectNM;
									}
								}
							}
						}
					}
				}
			}
		}

		// ============================================================
		// Random-walk subsurface scattering (spectral)
		// ============================================================
		{
			const RandomWalkSSSParams* pRWParams =
				ri.pMaterial ? ri.pMaterial->GetRandomWalkSSSParams() : 0;

			RandomWalkSSSParams rwParamsNM;
			if( !pRWParams && ri.pMaterial &&
				ri.pMaterial->GetRandomWalkSSSParamsNM( nm, rwParamsNM ) )
			{
				pRWParams = &rwParamsNM;
			}

			if( pRWParams && pBRDF )
			{
				const Scalar cosIn = Vector3Ops::Dot( ri.geometric.vNormal,
					Vector3Ops::Normalize( -ri.geometric.ray.Dir() ) );

				if( cosIn > NEARZERO )
				{
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
							rwSampler, nm, pRWParams->maxDepth );

						if( bssrdf.valid )
						{
							const Scalar bf = pRWParams->boundaryFilter;
							const Scalar bssrdfWeightNM = bssrdf.weightNM * Ft * bf;
							const Scalar bssrdfWeightSpatialNM = bssrdf.weightSpatialNM * Ft * bf;

							RayIntersectionGeometric entryRI(
								Ray( bssrdf.entryPoint, bssrdf.scatteredRay.Dir() ),
								rast );
							entryRI.bHit = true;
							entryRI.ptIntersection = bssrdf.entryPoint;
							entryRI.vNormal = bssrdf.entryNormal;
							entryRI.onb = bssrdf.entryONB;

							RandomWalkEntryBSDF entryBSDF( pRWParams->ior );
							BSSRDFEntryMaterial entryMaterial;

							const unsigned int nextTranslucentBounces = translucentBounces + 1;
							const bool skipSSS =
								nextTranslucentBounces > stabilityConfig.maxTranslucentBounce;

							if( !skipSSS )
							{
								if( pLS )
								{
									Scalar directSSSNM = pLS->EvaluateDirectLightingNM(
										entryRI, entryBSDF, &entryMaterial, nm, caster,
										bssrdfSampler, ri.pObject, 0, false, 0 );
									Scalar sssDirectContribNM = throughput * bssrdfWeightSpatialNM * directSSSNM;
									sssDirectContribNM = ClampContribution( sssDirectContribNM,
										stabilityConfig.directClamp );
									result += sssDirectContribNM;
								}

								// BSSRDF continuation via CastRayNM sub-path
								{
									Scalar sssThroughputNM = bssrdfWeightNM;

									const PathTransportUtilities::RussianRouletteResult rr =
										PathTransportUtilities::EvaluateRussianRoulette(
											depth, rrMinDepth, rrThreshold,
											importance * fabs( sssThroughputNM ),
											importance, bssrdfSampler.Get1D() );
									if( !rr.terminate )
									{
										if( rr.survivalProb < 1.0 ) {
											sssThroughputNM /= rr.survivalProb;
										}

										Scalar cthis = 0;
										Ray continuationRay = bssrdf.scatteredRay;
										continuationRay.Advance( 1e-8 );

										IRayCaster::RAY_STATE rs2;
										rs2.depth = depth + 2;
										rs2.considerEmission = true;
										rs2.importance = importance * fabs( sssThroughputNM );
										rs2.bsdfPdf = bssrdf.cosinePdf;
										rs2.bsdfTimesCos = RISEPel( fabs( sssThroughputNM ) * bssrdf.cosinePdf );
										rs2.type = IRayCaster::RAY_STATE::eRayDiffuse;
										rs2.diffuseBounces = diffuseBounces;
										rs2.glossyBounces = glossyBounces;
										rs2.transmissionBounces = transmissionBounces;
										rs2.translucentBounces = nextTranslucentBounces;
										rs2.glossyFilterWidth = glossyFilterWidth;

										if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() && bssrdf.cosinePdf > 0 )
										{
											const_cast<OptimalMISAccumulator*>(rc.pOptimalMIS)->AccumulateCount(
												rast.x, rast.y, kTechniqueBSDF );
										}

										caster.CastRayNM( rc, rast, continuationRay, cthis,
											rs2, nm, 0, pRadianceMap, iorStack );

										Scalar indirectNM = sssThroughputNM * cthis;
										if( depth > 0 ) {
											indirectNM = ClampContribution( indirectNM,
												stabilityConfig.indirectClamp );
										}
										result += throughput * indirectNM;
									}
								}
							}
						}
					}
				}
			}
		}

		// ============================================================
		// Specular surfaces (no BSDF — use SPF) (spectral)
		// ============================================================
		if( !pBRDF )
		{
			const ISPF* pSPF = ri.pMaterial ? ri.pMaterial->GetSPF() : 0;
			if( !pSPF ) {
				break;
			}

			ScatteredRayContainer scattered;
			pSPF->ScatterNM( ri.geometric, sampler, nm, scattered, iorStack );

			if( scattered.Count() == 0 ) {
				break;
			}

			if( scattered.Count() > 1 )
			{
				for( unsigned int i = 0; i < scattered.Count(); i++ )
				{
					ScatteredRay& scat = scattered[i];
					if( scat.krayNM > 0 )
					{
						IRayCaster::RAY_STATE rs2;
						rs2.depth = depth + 1;
						rs2.importance = importance * scat.krayNM;
						rs2.bsdfPdf = scat.isDelta ? 0 : scat.pdf;
						rs2.type = PathTracingRayType( scat );
						// SMS emission suppression: delta scatters beyond a
						// non-specular shading point must not re-enable
						// emission at the light.  Matches the RGB branching
						// path.  Pass SMS flags to the recursive child too.
						rs2.considerEmission = ( scat.isDelta && bSMSEnabled ) ? false : true;
						rs2.smsPassedThroughSpecular = scat.isDelta;
						rs2.smsHadNonSpecularShading = bHadNonSpecularShading || !scat.isDelta;
						rs2.diffuseBounces = diffuseBounces;
						rs2.glossyBounces = glossyBounces;
						rs2.transmissionBounces = transmissionBounces;
						rs2.translucentBounces = translucentBounces;
						rs2.glossyFilterWidth = glossyFilterWidth;
						if( PropagateBounceLimits( rs, rs2, scat, &stabilityConfig ) ) {
							continue;
						}

						Scalar cthis = 0;
						Ray ray = scat.ray;
						ray.Advance( 1e-8 );
						caster.CastRayNM( rc, rast, ray, cthis,
							rs2, nm, 0, pRadianceMap,
							scat.ior_stack ? *scat.ior_stack : iorStack );

						Scalar indirectNM = cthis * scat.krayNM;
						if( depth > 0 ) {
							indirectNM = ClampContribution( indirectNM, stabilityConfig.indirectClamp );
						}
						result += throughput * indirectNM;
					}
				}
				break;
			}
			else
			{
				// Single scattered ray: continue iteratively
				const Scalar xi = sampler.Get1D();
				const ScatteredRay* pS = scattered.RandomlySelect( xi, false );
				if( !pS ) {
					break;
				}

				IRayCaster::RAY_STATE rs2 = rs;
				rs2.depth = depth + 1;
				rs2.importance = importance * pS->krayNM;
				rs2.bsdfPdf = pS->isDelta ? 0 : pS->pdf;
				rs2.type = PathTracingRayType( *pS );
				// SMS emission-suppression: a delta continuation beyond a
				// non-specular shading point must not re-enable emission
				// at the next surface (SMS already covered that path).
				// Matches the RGB iterative continuation logic.
				const bool nextConsiderEmissionNM = ( pS->isDelta && bSMSEnabled )
					? false : true;
				rs2.considerEmission = nextConsiderEmissionNM;
				if( PropagateBounceLimits( rs, rs2, *pS, &stabilityConfig ) ) {
					break;
				}

				throughput *= pS->krayNM;
				importance = rs2.importance;
				bsdfPdf = rs2.bsdfPdf;
				bsdfTimesCosNM = 0;
				considerEmission = nextConsiderEmissionNM;
				rayType = rs2.type;
				diffuseBounces = rs2.diffuseBounces;
				glossyBounces = rs2.glossyBounces;
				transmissionBounces = rs2.transmissionBounces;
				translucentBounces = rs2.translucentBounces;
				glossyFilterWidth = rs2.glossyFilterWidth;

				// Track specular transitions for SMS double-counting
				// prevention.  Same semantic as the RGB path: a delta
				// scatter sets bPassedThroughSpecular=true; a non-delta
				// scatter resets that flag and raises
				// bHadNonSpecularShading so any subsequent specular chain
				// to the light gets suppressed at the light hit.
				if( pS->isDelta ) {
					bPassedThroughSpecular = true;
				} else {
					bPassedThroughSpecular = false;
					bHadNonSpecularShading = true;
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
		// PART 2: NEE + SMS at diffuse/glossy surfaces (spectral)
		// ============================================================
		if( pLS )
		{
			IndependentSampler fallbackSampler( rc.random );
			ISampler& neeSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;

			Scalar directAllNM = pLS->EvaluateDirectLightingNM(
				ri.geometric, *pBRDF, ri.pMaterial, nm, caster, neeSampler,
				ri.pObject, pCurrentMedium, false, pMediumObject );
			directAllNM = ClampContribution( directAllNM, stabilityConfig.directClamp );
			result += throughput * directAllNM;
		}

		// SMS for caustics (spectral — per-wavelength IOR for dispersion)
		if( pSolver )
		{
			const Vector3 woOutgoing = Vector3(
				-ri.geometric.ray.Dir().x,
				-ri.geometric.ray.Dir().y,
				-ri.geometric.ray.Dir().z );

			IndependentSampler fallbackSampler( rc.random );
			ISampler& smsSampler = rc.pSampler ? *rc.pSampler : fallbackSampler;

			ManifoldSolver::SMSContributionNM sms = pSolver->EvaluateAtShadingPointNM(
				ri.geometric.ptIntersection,
				ri.geometric.vNormal,
				ri.geometric.onb,
				ri.pMaterial,
				woOutgoing,
				scene,
				caster,
				smsSampler,
				nm );

			if( sms.valid )
			{
				Scalar smsContribNM = sms.contribution * sms.misWeight;
				smsContribNM = ClampContribution( smsContribNM, stabilityConfig.directClamp );
				result += throughput * smsContribNM;
			}
		}

		// ============================================================
		// PART 3: BSDF sampling (spectral — iterative continuation)
		// ============================================================
		const ISPF* pSPF = ri.pMaterial ? ri.pMaterial->GetSPF() : 0;
		if( !pSPF ) {
			break;
		}

		ScatteredRayContainer scattered;
		pSPF->ScatterNM( ri.geometric, sampler, nm, scattered, iorStack );

		if( scattered.Count() == 0 ) {
			break;
		}

		if( scattered.Count() > 1 )
		{
			for( unsigned int i = 0; i < scattered.Count(); i++ )
			{
				ScatteredRay& scat = scattered[i];
				if( scat.krayNM > 0 )
				{
					IRayCaster::RAY_STATE rs2;
					rs2.depth = depth + 2;
					rs2.importance = importance * scat.krayNM;
					rs2.bsdfPdf = scat.isDelta ? 0 : scat.pdf;
					rs2.bsdfTimesCos = scat.isDelta ? RISEPel( 0, 0, 0 ) :
						RISEPel( fabs( scat.krayNM ) * scat.pdf );
					rs2.type = PathTracingRayType( scat );
					rs2.diffuseBounces = diffuseBounces;
					rs2.glossyBounces = glossyBounces;
					rs2.transmissionBounces = transmissionBounces;
					rs2.translucentBounces = translucentBounces;
					rs2.glossyFilterWidth = glossyFilterWidth;

					if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() && !scat.isDelta )
					{
						const_cast<OptimalMISAccumulator*>(rc.pOptimalMIS)->AccumulateCount(
							rast.x, rast.y, kTechniqueBSDF );
					}

					if( PropagateBounceLimits( rs, rs2, scat, &stabilityConfig ) ) {
						continue;
					}

					if( scat.isDelta && bSMSEnabled ) {
						rs2.considerEmission = false;
					} else {
						rs2.considerEmission = true;
					}

					Scalar cthis = 0;
					Ray ray = scat.ray;
					ray.Advance( 1e-8 );
					caster.CastRayNM( rc, rast, ray, cthis,
						rs2, nm, 0, pRadianceMap,
						scat.ior_stack ? *scat.ior_stack : iorStack );

					Scalar indirectNM = cthis * scat.krayNM;
					if( depth > 0 ) {
						indirectNM = ClampContribution( indirectNM, stabilityConfig.indirectClamp );
					}
					result += throughput * indirectNM;
				}
			}
			break;
		}
		else
		{
			// Single scattered ray: continue iteratively
			const Scalar xi = sampler.Get1D();
			const ScatteredRay* pS = scattered.RandomlySelect( xi, false );
			if( !pS ) {
				break;
			}

			Ray traceRay = pS->ray;
			Scalar scatterThroughputNM = pS->krayNM;
			Scalar effectiveBsdfPdf = pS->isDelta ? 0 : pS->pdf;
			const IORStack* traceIorStack = pS->ior_stack ? pS->ior_stack : &iorStack;

#ifdef RISE_ENABLE_OPENPGL
			static thread_local GuidingDistributionHandle guideDistNM;

			if( rc.pGuidingField && rc.pGuidingField->IsTrained() &&
				depth <= rc.maxGuidingDepth && GuidingSupportsSurfaceSampling( *pS ) )
			{
				const Scalar alpha = GuidingEffectiveAlpha(
					rc.guidingAlpha, scattered, ri.pMaterial, rs, true );

				if( alpha > NEARZERO && rc.pGuidingField->InitDistribution( guideDistNM,
					ri.geometric.ptIntersection,
					sampler.Get1D() ) )
				{
					if( pS->type == ScatteredRay::eRayDiffuse ) {
						rc.pGuidingField->ApplyCosineProduct( guideDistNM, GuidingCosineNormal( ri.geometric ) );
					}

					if( rc.guidingSamplingType == eGuidingRIS )
					{
						// RIS-based guiding (spectral)
						PathTransportUtilities::GuidingRISCandidate<Scalar> candidates[2];

						// Candidate 0: BSDF sample
						{
							PathTransportUtilities::GuidingRISCandidate<Scalar>& c = candidates[0];
							c.direction = pS->ray.Dir();
							c.bsdfEval = PathVertexEval::EvalBSDFAtSurfaceNM(
								pBRDF, c.direction, ri.geometric, nm );
							c.bsdfPdf = pS->pdf;
							c.guidePdf = rc.pGuidingField->Pdf( guideDistNM, c.direction );
							c.incomingRadPdf = rc.pGuidingField->IncomingRadiancePdf( guideDistNM, c.direction );
							c.cosTheta = fabs(
								Vector3Ops::Dot( c.direction, ri.geometric.vNormal ) );
							const Scalar avgBsdf = fabs( c.bsdfEval );
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
							PathTransportUtilities::GuidingRISCandidate<Scalar>& c = candidates[1];
							Scalar guidePdf = 0;
							const Point2 xi2d( sampler.Get1D(), sampler.Get1D() );
							c.direction = rc.pGuidingField->Sample( guideDistNM, xi2d, guidePdf );
							c.guidePdf = guidePdf;

							if( guidePdf > NEARZERO )
							{
								c.bsdfEval = PathVertexEval::EvalBSDFAtSurfaceNM(
									pBRDF, c.direction, ri.geometric, nm );
								c.bsdfPdf = PathVertexEval::EvalPdfAtSurfaceNM(
									pSPF, ri.geometric, c.direction, nm, iorStack );
								c.incomingRadPdf = rc.pGuidingField->IncomingRadiancePdf( guideDistNM, c.direction );
								c.cosTheta = fabs(
									Vector3Ops::Dot( c.direction, ri.geometric.vNormal ) );
								const Scalar avgBsdf = fabs( c.bsdfEval );
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
								c.bsdfEval = 0;
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
							scatterThroughputNM = candidates[sel].bsdfEval *
								candidates[sel].cosTheta / risEffectivePdf;
							traceRay = Ray( pS->ray.origin, candidates[sel].direction );
							effectiveBsdfPdf = risEffectivePdf;
							traceIorStack = &iorStack;
						}
						else
						{
							scatterThroughputNM = 0;
						}
					}
					else
					{
						// One-sample MIS (spectral)
						const Scalar xiG = sampler.Get1D();

						if( PathTransportUtilities::ShouldUseGuidedSample( alpha, xiG ) )
						{
							Scalar guidePdf = 0;
							const Point2 xi2d( sampler.Get1D(), sampler.Get1D() );
							const Vector3 guidedDir = rc.pGuidingField->Sample( guideDistNM, xi2d, guidePdf );

							if( guidePdf > NEARZERO )
							{
								const Scalar fGuided = PathVertexEval::EvalBSDFAtSurfaceNM(
									pBRDF, guidedDir, ri.geometric, nm );
								const Scalar bsdfPdfGuided = PathVertexEval::EvalPdfAtSurfaceNM(
									pSPF, ri.geometric, guidedDir, nm, iorStack );
								const Scalar combinedPdf =
									PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdf, bsdfPdfGuided );

								if( combinedPdf > NEARZERO )
								{
									const Scalar cosTheta = fabs(
										Vector3Ops::Dot( guidedDir, ri.geometric.vNormal ) );
									scatterThroughputNM = fGuided * cosTheta / combinedPdf;
									traceRay = Ray( pS->ray.origin, guidedDir );
									effectiveBsdfPdf = combinedPdf;
									traceIorStack = &iorStack;
								}
								else
								{
									scatterThroughputNM = 0;
								}
							}
							else
							{
								scatterThroughputNM = 0;
							}
						}
						else
						{
							const Scalar guidePdfForBsdf = rc.pGuidingField->Pdf( guideDistNM, pS->ray.Dir() );
							const Scalar combinedPdf =
								PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdfForBsdf, pS->pdf );

							if( combinedPdf > NEARZERO )
							{
								scatterThroughputNM = pS->krayNM * (pS->pdf / combinedPdf);
								effectiveBsdfPdf = combinedPdf;
							}
						}
					}
				}
			}

			const bool collectTrainingSampleNM =
				rc.pGuidingField && rc.pGuidingField->IsCollectingTrainingSamples() &&
				GuidingSupportsSurfaceSampling( *pS ) && effectiveBsdfPdf > NEARZERO;
#endif // RISE_ENABLE_OPENPGL

			bool skipContinuation = fabs( scatterThroughputNM ) <= NEARZERO;

			// Optimal MIS training
			if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() &&
				!pS->isDelta && !skipContinuation )
			{
				const_cast<OptimalMISAccumulator*>(rc.pOptimalMIS)->AccumulateCount(
					rast.x, rast.y, kTechniqueBSDF );
			}

			// Russian roulette
			if( !skipContinuation )
			{
				const PathTransportUtilities::RussianRouletteResult rr =
					PathTransportUtilities::EvaluateRussianRoulette(
						depth, rrMinDepth, rrThreshold,
						importance * fabs( scatterThroughputNM ),
						importance,
						sampler.Get1D() );
				if( rr.terminate ) {
					skipContinuation = true;
				} else if( rr.survivalProb < 1.0 ) {
					scatterThroughputNM /= rr.survivalProb;
				}
			}

			const Scalar bsdfTimesCosValNM = pS->isDelta ? 0 :
				fabs( scatterThroughputNM ) * effectiveBsdfPdf;

			// Per-type bounce limits
			IRayCaster::RAY_STATE rs2 = rs;
			rs2.depth = depth + 2;
			rs2.importance = importance * fabs( scatterThroughputNM );
			rs2.bsdfPdf = effectiveBsdfPdf;
			rs2.bsdfTimesCos = RISEPel( bsdfTimesCosValNM );
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
			throughput *= scatterThroughputNM;
			importance = rs2.importance;
			bsdfPdf = effectiveBsdfPdf;
			bsdfTimesCosNM = bsdfTimesCosValNM;
			considerEmission = nextConsiderEmission;
			rayType = rs2.type;
			diffuseBounces = rs2.diffuseBounces;
			glossyBounces = rs2.glossyBounces;
			transmissionBounces = rs2.transmissionBounces;
			translucentBounces = rs2.translucentBounces;
			glossyFilterWidth = rs2.glossyFilterWidth;

			currentRay = traceRay;
			currentRay.Advance( 1e-8 );

			if( traceIorStack != &iorStack ) {
				iorStack = *traceIorStack;
			}

#ifdef RISE_ENABLE_OPENPGL
			// Training sample: collect after next hit/miss
			// (hitDist is computed when we intersect the next bounce)
			(void)collectTrainingSampleNM;
#endif
		}
	}

	return result;
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
	Scalar hwssResult[SampledWavelengths::N]
	) const
{
	// Initialize results
	for( unsigned int i = 0; i < SampledWavelengths::N; i++ ) {
		hwssResult[i] = 0;
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
					translucentBounces, volumeBounces, glossyFilterWidth );
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

		if( pProfile || pRWParams || hasRWNM )
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
						translucentBounces, volumeBounces, glossyFilterWidth );
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

	Scalar throughputHero = 1.0;
	Scalar throughputComp[SampledWavelengths::N];
	for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
		throughputComp[w] = 1.0;
	}

	RayIntersection ri( firstHit );
	Ray currentRay = ri.geometric.ray;
	IORStack iorStack = initialIorStack;
	bool needsIntersection = false;

	const unsigned int rrMinDepth = stabilityConfig.rrMinDepth;
	const Scalar rrThreshold = stabilityConfig.rrThreshold;

	const LightSampler* pLS = caster.GetLightSampler();
	const unsigned int maxDepth = 128;

	for( unsigned int depth = startDepth; depth < maxDepth; depth++ )
	{
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
				const Scalar maxDist = bHit ? ri.geometric.range : Scalar(1e10);
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
					// Apply per-wavelength transmittance
					for( unsigned int w = 0; w < SampledWavelengths::N; w++ )
					{
						if( swl.terminated[w] ) {
							continue;
						}
						const Scalar Tr = pCurrentMedium->EvalTransmittanceNM(
							currentRay, ri.geometric.range, swl.lambda[w] );
						throughputComp[w] *= Tr;
					}
					throughputHero = throughputComp[0];
				}
			}

			if( !bHit )
			{
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

						hwssResult[w] += throughputComp[w] * envRadiance;
					}
				}
				else if( pRadianceMap )
				{
					for( unsigned int w = 0; w < SampledWavelengths::N; w++ )
					{
						if( swl.terminated[w] ) continue;
						hwssResult[w] += throughputComp[w] *
							pRadianceMap->GetRadianceNM( currentRay, rast, swl.lambda[w] );
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

		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		iorStack.SetCurrentObject( ri.pObject );

		const IBSDF* pBRDFCur = ri.pMaterial ? ri.pMaterial->GetBSDF() : 0;

		// If we hit a material without BSDF mid-path (e.g. entered a
		// dielectric), fall back to per-wavelength NM for remaining path
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
					translucentBounces, volumeBounces, glossyFilterWidth );
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

			if( pProfile || pRWParams || hasRWNM )
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
						translucentBounces, volumeBounces, glossyFilterWidth );
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
			if( pEmitter && considerEmission )
			{
				for( unsigned int w = 0; w < SampledWavelengths::N; w++ )
				{
					if( swl.terminated[w] ) continue;

					Scalar emission = pEmitter->emittedRadianceNM(
						ri.geometric, -ri.geometric.ray.Dir(), ri.geometric.vNormal,
						swl.lambda[w] );

					if( bsdfPdf > 0 && ri.pObject )
					{
						const Scalar area = ri.pObject->GetArea();
						if( area > 0 )
						{
							const Scalar cosLight = fabs( Vector3Ops::Dot(
								ri.geometric.ray.Dir(), ri.geometric.vNormal ) );
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
					hwssResult[w] += throughputComp[w] * emission;
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

				Scalar directNM = pLS->EvaluateDirectLightingNM(
					ri.geometric, *pBRDFCur, ri.pMaterial, swl.lambda[w],
					caster, neeSampler, ri.pObject, pCurrentMedium, false, pMediumObject );
				directNM = ClampContribution( directNM, stabilityConfig.directClamp );
				hwssResult[w] += throughputComp[w] * directNM;
			}
		}

		// SMS (HWSS — per wavelength, since IOR varies)
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

				ManifoldSolver::SMSContributionNM sms = pSolver->EvaluateAtShadingPointNM(
					ri.geometric.ptIntersection,
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
		const ISPF* pSPF = ri.pMaterial ? ri.pMaterial->GetSPF() : 0;
		if( !pSPF ) {
			break;
		}

		ScatteredRayContainer scattered;
		pSPF->ScatterNM( ri.geometric, sampler, heroNM, scattered, iorStack );

		if( scattered.Count() == 0 ) {
			break;
		}

		// For HWSS we always use single-sample continuation (no branching)
		const Scalar xi = sampler.Get1D();
		const ScatteredRay* pS = scattered.RandomlySelect( xi, false );
		if( !pS ) {
			break;
		}

		// RandomlySelect uses hero-wavelength krayNM for selection: picks
		// lobe i with prob krayNM_i / sum_j krayNM_j.  The unbiased throughput
		// update divides by that selectProb — same pattern as RGB PT at
		// line 1543 and BDPT at BDPTIntegrator.cpp:1782-1794.  Companion
		// wavelengths inherit the hero's selection, so they divide by the
		// same hero-based selectProb.
		Scalar selectProb = 1.0;
		if( scattered.Count() > 1 )
		{
			Scalar totalKrayNM = 0;
			for( unsigned int li = 0; li < scattered.Count(); li++ ) {
				totalKrayNM += fabs( scattered[li].krayNM );
			}
			const Scalar pSKrayNM = fabs( pS->krayNM );
			if( totalKrayNM > NEARZERO && pSKrayNM > NEARZERO ) {
				selectProb = pSKrayNM / totalKrayNM;
			}
		}
		if( selectProb < NEARZERO ) {
			break;
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
		throughputHero = throughputComp[0];
		importance = rs2.importance;
		bsdfPdf = effectiveBsdfPdf;
		considerEmission = nextConsiderEmission;
		rayType = rs2.type;
		diffuseBounces = rs2.diffuseBounces;
		glossyBounces = rs2.glossyBounces;
		transmissionBounces = rs2.transmissionBounces;
		translucentBounces = rs2.translucentBounces;
		glossyFilterWidth = rs2.glossyFilterWidth;

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
	const IRadianceMap* pRadianceMap
	) const
{
	IORStack iorStack( 1.0 );
	sampler.StartStream( 16 );

	// Intersect camera ray
	RayIntersection ri( cameraRay, rast );
	scene.GetObjects()->IntersectRay( ri, true, true, false );

	// Medium transport for first bounce (spectral)
	const IObject* pMediumObject = 0;
	const IMedium* pCurrentMedium = MediumTracking::GetCurrentMediumWithObject(
		iorStack, &scene, pMediumObject );

	if( pCurrentMedium )
	{
		const Scalar maxDist = ri.geometric.bHit ? ri.geometric.range : Scalar(1e10);
		const LightSampler* pLS = caster.GetLightSampler();
		IndependentSampler mediumSampler( rc.random );
		const MediumSampleOutcome mso = SampleDistanceWithEquiangularMIS_NM(
			pCurrentMedium, cameraRay, maxDist, nm, pLS, mediumSampler );
		const Scalar t_m = mso.t;
		const bool scattered = mso.scattered;

		if( mso.zeroContrib )
		{
			return 0;
		}

		if( scattered )
		{
			const Point3 scatterPt = cameraRay.PointAtLength( t_m );
			const Vector3 wo = cameraRay.Dir();
			const MediumCoefficientsNM coeff = pCurrentMedium->GetCoefficientsNM( scatterPt, nm );
			const Scalar Tr = pCurrentMedium->EvalTransmittanceNM( cameraRay, t_m, nm );

			Scalar medWeight = 0;
			if( mso.useExplicitThroughput && mso.combinedPdf > 0 )
			{
				medWeight = Tr * coeff.sigma_s / mso.combinedPdf;
			}
			else if( coeff.sigma_t > 0 && Tr > 0 )
			{
				medWeight = Tr * coeff.sigma_s / (coeff.sigma_t * Tr);
			}

			if( medWeight <= 0 ) {
				return 0;
			}

			Scalar resultNM = 0;

			// NEE at scatter point
			if( pLS )
			{
				Scalar Ld = MediumTransport::EvaluateInScatteringNM(
					scatterPt, wo, pCurrentMedium, nm, caster,
					pLS, sampler, rast, pMediumObject );
				if( Ld > 0 )
				{
					Scalar directContrib = medWeight * Ld;
					directContrib = ClampContribution( directContrib,
						stabilityConfig.directClamp );
					resultNM += directContrib;
				}
			}

			// Sample phase function for continuation
			const IPhaseFunction* pPhase = pCurrentMedium->GetPhaseFunction();
			if( !pPhase ) {
				return resultNM;
			}

			const Vector3 wi = pPhase->Sample( wo, sampler );
			const Scalar phasePdf = pPhase->Pdf( wo, wi );
			if( phasePdf <= NEARZERO ) {
				return resultNM;
			}

			const Scalar phaseVal = pPhase->Evaluate( wo, wi );
			Scalar volThroughput = medWeight * phaseVal / phasePdf;

			// Intersect the scattered direction
			const Ray scatteredRay( scatterPt, wi );
			RayIntersection ri2( scatteredRay, rast );
			scene.GetObjects()->IntersectRay( ri2, true, true, false );

			if( !ri2.geometric.bHit ) {
				if( scene.GetGlobalRadianceMap() ) {
					resultNM += volThroughput *
						scene.GetGlobalRadianceMap()->GetRadianceNM( scatteredRay, rast, nm );
				}
				return resultNM;
			}

			Scalar hitResult = IntegrateFromHitNM( rc, rast, ri2, nm, scene, caster,
				sampler, pRadianceMap, 1, iorStack, phasePdf, 0,
				true, 1.0, IRayCaster::RAY_STATE::eRayDiffuse,
				0, 0, 0, 0, 1, 0 );

			return resultNM + volThroughput * hitResult;
		}
		else if( ri.geometric.bHit )
		{
			const Scalar Tr = pCurrentMedium->EvalTransmittanceNM(
				cameraRay, ri.geometric.range, nm );

			Scalar hitResult = IntegrateFromHitNM( rc, rast, ri, nm, scene, caster,
				sampler, pRadianceMap, 0, iorStack,
				0, 0, true, 1.0, IRayCaster::RAY_STATE::eRayView,
				0, 0, 0, 0, 0, 0 );

			return Tr * hitResult;
		}
	}

	// No medium, or medium with no scatter and no surface hit
	if( !ri.geometric.bHit )
	{
		Scalar envResult = 0;
		if( pRadianceMap )
		{
			envResult = pRadianceMap->GetRadianceNM( cameraRay, rast, nm );
		}
		else if( scene.GetGlobalRadianceMap() )
		{
			envResult = scene.GetGlobalRadianceMap()->GetRadianceNM( cameraRay, rast, nm );
		}
		return envResult;
	}

	return IntegrateFromHitNM( rc, rast, ri, nm, scene, caster,
		sampler, pRadianceMap, 0, iorStack,
		0, 0, true, 1.0, IRayCaster::RAY_STATE::eRayView,
		0, 0, 0, 0, 0, 0 );
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
	Scalar result[SampledWavelengths::N]
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

	// Medium transport for first bounce — use hero wavelength for
	// distance sampling; per-wavelength transmittance applied inside
	// IntegrateFromHitHWSS.
	const Scalar heroNM = swl.HeroLambda();
	const IObject* pMediumObject = 0;
	const IMedium* pCurrentMedium = MediumTracking::GetCurrentMediumWithObject(
		iorStack, &scene, pMediumObject );

	if( pCurrentMedium )
	{
		const Scalar maxDist = ri.geometric.bHit ? ri.geometric.range : Scalar(1e10);
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

				// NEE at scatter point
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
						result[w] += directContrib;
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
							if( scene.GetGlobalRadianceMap() )
							{
								result[w] += volThroughput *
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
			// Surface hit through medium: IntegrateFromHitHWSS handles
			// per-wavelength transmittance internally.  For the first
			// bounce, apply transmittance here and scale results.
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
				0, 0, 0, 0, 0, 0, result );

			for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
				result[w] *= Tr[w];
			}
			return;
		}
	}

	// No medium, or medium with no scatter and no surface hit
	if( !ri.geometric.bHit )
	{
		if( pRadianceMap )
		{
			for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
				if( !swl.terminated[w] ) {
					result[w] = pRadianceMap->GetRadianceNM( cameraRay, rast, swl.lambda[w] );
				}
			}
		}
		else if( scene.GetGlobalRadianceMap() )
		{
			for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
				if( !swl.terminated[w] ) {
					result[w] = scene.GetGlobalRadianceMap()->GetRadianceNM(
						cameraRay, rast, swl.lambda[w] );
				}
			}
		}
		return;
	}

	IntegrateFromHitHWSS( rc, rast, ri, swl, scene, caster,
		sampler, pRadianceMap, 0, iorStack,
		0, true, 1.0, IRayCaster::RAY_STATE::eRayView,
		0, 0, 0, 0, 0, 0, result );
}
