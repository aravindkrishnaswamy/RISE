//////////////////////////////////////////////////////////////////////
//
//  BDPTIntegrator.cpp - Implementation of the BDPTIntegrator class.
//
//  ALGORITHM OVERVIEW:
//    For each pixel sample, the integrator:
//    1. Generates a light subpath by sampling a light source (via
//       LightSampler), emitting a ray, and tracing it through the
//       scene.  At each surface hit, the SPF is sampled to extend
//       the path.  Forward PDFs (area measure) and cumulative
//       throughput are stored at each vertex.
//
//    2. Generates an eye subpath from the camera ray.  Same logic
//       as the light subpath but starting from the camera vertex.
//
//    3. Evaluates all (s,t) connection strategies where s is the
//       number of light vertices and t the number of eye vertices.
//       Each strategy gets a MIS weight via the balance heuristic.
//
//  CONNECTION STRATEGIES:
//    - s=0: Eye path naturally hits an emitter (no light subpath
//      needed).  Contribution = eyeThroughput * Le.
//    - s=1, t>1: Next event estimation — connect the last eye
//      vertex to the sampled light vertex.  Classic direct lighting.
//    - t=1: Connect the last light vertex to the camera.  Because
//      the eye subpath already stores the camera as vertex 0, this
//      is the full light-tracing/splat strategy.  The result lands
//      at an arbitrary pixel position and is accumulated via the
//      SplatFilm.
//    - s>1, t>1: General case — connect the two subpath endpoints,
//      evaluate BSDFs at both, multiply with the geometric term.
//
//  PDF BOOKKEEPING:
//    Each vertex stores pdfFwd (area measure, from the direction
//    it was generated) and pdfRev (area measure, computed after the
//    NEXT vertex is generated, representing the probability of
//    sampling the reverse direction).  These are used by MISWeight
//    to incrementally compute PDF ratios for all strategies.
//
//    Delta interactions (perfect specular) set isDelta=true.  The
//    MIS weight computation skips delta vertices since only exactly
//    one strategy can generate them.
//
//  RUSSIAN ROULETTE:
//    Applied after depth > 2 to terminate low-throughput paths.
//    Survival probability is clamped to [0, 1] and the throughput
//    is divided by the survival probability to maintain unbiasedness.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BDPTIntegrator.h"
#include "../Utilities/FiniteMath.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/ISPF.h"
#include "../Interfaces/IEmitter.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IGeometry.h"		// CanBeAreaLight(): crash-fix-round-2, s=0 eyeEnd.pObject->GetArea() null-geometry guard
#include "../Interfaces/ISubSurfaceDiffusionProfile.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/ILightManager.h"
#include "../Interfaces/ILightPriv.h"
#include "../Utilities/RandomWalkSSS.h"
#include "../Utilities/BDPTUtilities.h"
#include "../Utilities/PathTransportUtilities.h"
#include "../Utilities/PathVertexEval.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Color/ColorMath.h"
#include "../Utilities/Math3D/Constants.h"
#include "../Cameras/CameraUtilities.h"
#include "../Intersection/RayIntersection.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Rendering/LuminaryManager.h"
#include "../Rendering/RayCaster.h"
#include "../Rendering/AOVBuffers.h"
#include "../Utilities/MediumTracking.h"
#include "../Utilities/IORStackSeeding.h"
#include "../Utilities/GradedIndexMedium.h"
#include "../Interfaces/IMedium.h"
#include "../Interfaces/IPhaseFunction.h"
#include "../Utilities/IndependentSampler.h"
#include "../Utilities/Color/SpectralValueTraits.h"
#include "../Utilities/PathValueOps.h"
#include "BSSRDFEntryAdapters.h"
#include "../Utilities/SobolSampler.h"

// DL-283: the medium-distance stream layout (BDPTUtilities.h) must hold a
// whole distance sample, stay clear of every fixed stream, and fit the
// 32-bit Sobol' dimension counter.
static_assert( RISE::BDPTUtilities::kMediumDistanceStreamsPerEvent *
		RISE::Implementation::SobolSampler::kStreamStride >= RISE::IMedium::kMaxSampleDistanceDraws,
	"a medium-distance stream block is narrower than one SampleDistance call's draw bound" );
static_assert( RISE::BDPTUtilities::kMediumDistanceStreamBase >
		RISE::BDPTCameraUtilities::kApertureSamplerStream &&
	RISE::BDPTUtilities::kMediumDistanceStreamBase >
		RISE::BDPTCameraUtilities::kPSSMLTFilmLensApertureStream &&
	// VCM's per-eye-vertex NEE stream, 48 + i, with at most three
	// vertices per walk iteration (SobolDimensionBudgetTest Test F).
	RISE::BDPTUtilities::kMediumDistanceStreamBase >
		48 + 3 * static_cast<int>( RISE::BDPTUtilities::kWalkIterationCap ) + 1,
	"medium-distance streams overlap a fixed BDPT/VCM stream" );
static_assert( static_cast<unsigned long long>( RISE::BDPTUtilities::kMediumDistanceStreamEnd ) *
		RISE::Implementation::SobolSampler::kStreamStride < 0xFFFFFFFFull,
	"medium-distance streams overflow SobolSampler's 32-bit dimension counter" );

using namespace RISE;
using namespace RISE::Implementation;
using RISE::SpectralDispatch::PelTag;
using RISE::SpectralDispatch::NMTag;
using RISE::SpectralDispatch::SpectralValueTraits;
// DL-207: the zero-exitance-light sweep's BSSRDF-entry-vertex branch
// prices Sw the same way PathTracingIntegrator's own BSSRDF-entry NEE
// does, via these stack-local IBSDF adapters (see BSSRDFEntryAdapters.h).
using RISE::BSSRDFAdapters::BSSRDFEntryBSDF;
using RISE::BSSRDFAdapters::EntryEvaluationRay;
using RISE::BSSRDFAdapters::RandomWalkEntryBSDF;

//
// Small epsilon for ray offsets to avoid self-intersection
//
static const Scalar BDPT_RAY_EPSILON = 1e-6;

namespace
{
	inline void CaptureBDPTPrimaryAOV(
		const RuntimeContext& rc,
		const RayIntersection& ri,
		PixelAOV* pAOV )
	{
		if( !pAOV || pAOV->primaryDepthCaptured ) return;
		pAOV->primaryDepthCaptured = true;
		pAOV->depth = ri.geometric.bHit ? ri.geometric.range : Scalar( 0 );
		if( !ri.geometric.bHit || rc.aovPrefilterMode != OidnPrefilter::Fast ) return;

		RayIntersectionGeometric aovGeom( ri.geometric );
		if( ri.pModifier ) ri.pModifier->Modify( aovGeom );
		pAOV->normal = aovGeom.vNormal;
		pAOV->albedo = ( rc.hasPathTracingVariantConfig && rc.pathTracingClayOverride )
			? RISEPel( 0.5, 0.5, 0.5 )
			: ( ( ri.pMaterial && ri.pMaterial->GetBSDF() )
				? ri.pMaterial->GetBSDF()->albedo( aovGeom )
				: RISEPel( 1, 1, 1 ) );
		pAOV->valid = true;
	}

	inline void CaptureBDPTAccurateAOV(
		const RuntimeContext& rc,
		const RayIntersection& ri,
		PixelAOV* pAOV )
	{
		if( !pAOV || pAOV->valid || !ri.geometric.bHit ||
		    rc.aovPrefilterMode != OidnPrefilter::Accurate ) return;

		// This runs while the real trace-time intersection and incoming ray
		// are still available. Reconstructing them later from camera->vertex
		// is wrong after a reflection, refraction, medium event, or BSSRDF.
		pAOV->normal = ri.geometric.vNormal;
		pAOV->albedo = ( rc.hasPathTracingVariantConfig && rc.pathTracingClayOverride )
			? RISEPel( 0.5, 0.5, 0.5 )
			: ( ( ri.pMaterial && ri.pMaterial->GetBSDF() )
				? ri.pMaterial->GetBSDF()->albedo( ri.geometric )
				: RISEPel( 1, 1, 1 ) );
		pAOV->valid = true;
	}

	inline Vector3 GuidingCosineNormal(
		const Vector3& normal,
		const Vector3& incomingDir
		)
	{
		Vector3 orientedNormal = normal;
		if( Vector3Ops::Dot( incomingDir, orientedNormal ) > NEARZERO ) {
			orientedNormal = -orientedNormal;
		}
		return orientedNormal;
	}

	// Delegate to shared PathVertexEval utility
	using PathVertexEval::BuildVertexIORStack;

	inline bool GuidingSupportsSurfaceSampling( const ScatteredRay& scat )
	{
		return !scat.isDelta && scat.type == ScatteredRay::eRayDiffuse;
	}

	inline bool GuidingWantsMultibounceTraining(
		const unsigned int s,
		const unsigned int t
		)
	{
		// Ignore single-bounce direct-light-like strategies and favor
		// connections that required at least two bounces overall.
		return t >= 3 || (s >= 2 && t >= 2);
	}

	struct StrategySelectionCandidate
	{
		unsigned int	s;
		unsigned int	t;
		Scalar		probability;

		StrategySelectionCandidate() :
			s( 0 ),
			t( 0 ),
			probability( 0 )
		{
		}

		StrategySelectionCandidate(
			const unsigned int s_,
			const unsigned int t_,
			const Scalar probability_
			) :
			s( s_ ),
			t( t_ ),
			probability( probability_ )
		{
		}
	};

	struct StrategySelectionScratch
	{
		std::vector<Scalar> learnedWeights;
		std::vector<StrategySelectionCandidate> candidates;
		std::vector<Scalar> cdf;
		size_t reservedCandidates;

		StrategySelectionScratch() :
			learnedWeights(),
			candidates(),
			cdf(),
			reservedCandidates( 0 )
		{
		}
	};

#ifdef RISE_ENABLE_OPENPGL
	struct GuidingTrainingPathScratch
	{
		std::vector<RISEPel> localContributions;
		std::vector<RISEPel> directContributions;
		std::vector<Scalar> directMiWeights;
		std::vector<bool> hasDirectContribution;
		PGLPathSegmentStorage pathSegments;
		size_t reservedSegments;

		GuidingTrainingPathScratch() :
			pathSegments( pglNewPathSegmentStorage() ),
			reservedSegments( 0 )
		{
		}

		~GuidingTrainingPathScratch()
		{
			if( pathSegments ) {
				pglReleasePathSegmentStorage( pathSegments );
			}
		}
	};

	inline void RecordGuidingTrainingSampleNM(
		PathGuidingField* pGuidingField,
		const RuntimeContext& rc,
		const IRayCaster& caster,
		const RayIntersection& ri,
		const Ray& sampledRay,
		const Scalar samplePdf,
		const unsigned int rayDepth,
		const Scalar nm,
		const IORStack& ior_stack
		)
	{
		if( !pGuidingField || !pGuidingField->IsCollectingTrainingSamples() ||
			samplePdf <= NEARZERO )
		{
			return;
		}

		RuntimeContext trainRc( rc.random, RuntimeContext::PASS_NORMAL, false );

		IRayCaster::RAY_STATE rs;
		rs.depth = rayDepth;
		rs.considerEmission = true;
		rs.bsdfPdf = samplePdf;
		// DL-74: this probe cast is not a guided continuation -- the
		// sampling density and the MIS-partner density are the same value.
		rs.bsdfMisPdf = samplePdf;

		Scalar Li = 0;
		Scalar hitDist = 0;
		Ray traceRay = sampledRay;
		traceRay.Advance( BDPT_RAY_EPSILON );

		caster.CastRayNM(
			trainRc,
			nullRasterizerState,
			traceRay,
			Li,
			rs,
			nm,
			&hitDist,
			ri.pRadianceMap,
			ior_stack );

		if( fabs( Li ) > 0 )
		{
			pGuidingField->AddSample(
				ri.geometric.ptIntersection,
				traceRay.Dir(),
				hitDist > 0 ? hitDist : 1.0,
				samplePdf,
				fabs( Li ),
				false );
		}
		else
		{
			pGuidingField->AddZeroValueSample(
				ri.geometric.ptIntersection,
				traceRay.Dir() );
		}
	}

	inline pgl_vec3f GuidingVec3f( const Vector3& v )
	{
		pgl_vec3f out;
		pglVec3f( out,
			static_cast<float>( v.x ),
			static_cast<float>( v.y ),
			static_cast<float>( v.z ) );
		return out;
	}

	inline pgl_point3f GuidingPoint3f( const Point3& p )
	{
		pgl_point3f out;
		pglPoint3f( out,
			static_cast<float>( p.x ),
			static_cast<float>( p.y ),
			static_cast<float>( p.z ) );
		return out;
	}

	inline pgl_vec3f GuidingColor3f( const RISEPel& c )
	{
		pgl_vec3f out;
		pglVec3f( out,
			static_cast<float>( c[0] ),
			static_cast<float>( c[1] ),
			static_cast<float>( c[2] ) );
		return out;
	}

	inline RISEPel GuidingClampContribution( const RISEPel& contribution )
	{
		RISEPel clamped = contribution;
		for( int i = 0; i < 3; i++ ) {
			if( !RISE::IsFiniteDouble( clamped[i] ) || clamped[i] < 0 ) {
				clamped[i] = 0;
			}
		}
		return clamped;
	}

	// Called from BDPTPelRasterizer::IntegratePixel on N worker
	// threads concurrently.  `pStats` and its `pStatsMutex` live on
	// the shared BDPTIntegrator instance, so every field of `*pStats`
	// is a potential race target.  We accumulate into a stack-local
	// snapshot during the per-result loop (zero contention) and
	// merge once at the end under `*pStatsMutex`.
	inline void RecordGuidingTrainingPath(
		PathGuidingField* pGuidingField,
		BDPTIntegrator::GuidingTrainingStats* pStats,
		std::mutex* pStatsMutex,
		const std::vector<BDPTVertex>& eyeVerts,
		const std::vector<BDPTIntegrator::ConnectionResult>& results
		)
	{
		if( !pGuidingField || !pGuidingField->IsCollectingTrainingSamples() ||
			eyeVerts.size() <= 1 )
		{
			return;
		}

		bool hasSegments = false;
		for( unsigned int i = 1; i < eyeVerts.size(); i++ ) {
			if( eyeVerts[i].type == BDPTVertex::SURFACE && eyeVerts[i].guidingHasSegment ) {
				hasSegments = true;
				break;
			}
		}

		if( !hasSegments ) {
			return;
		}

		// Per-call local accumulator — merged into *pStats under
		// *pStatsMutex at the end.  Keeps the hot per-result loop
		// lock-free.
		BDPTIntegrator::GuidingTrainingStats localStats;

		static thread_local GuidingTrainingPathScratch scratch;
		if( !scratch.pathSegments ) {
			return;
		}

		scratch.localContributions.assign(
			eyeVerts.size(),
			RISEPel( 0, 0, 0 ) );
		scratch.directContributions.assign(
			eyeVerts.size(),
			RISEPel( 0, 0, 0 ) );
		scratch.directMiWeights.assign(
			eyeVerts.size(),
			Scalar( 1.0 ) );
		scratch.hasDirectContribution.assign(
			eyeVerts.size(),
			false );

		for( unsigned int i = 0; i < results.size(); i++ )
		{
			const BDPTIntegrator::ConnectionResult& cr = results[i];
			if( !cr.valid || !cr.guidingValid || cr.needsSplat ||
				cr.guidingEyeVertexIndex >= eyeVerts.size() ||
				!GuidingWantsMultibounceTraining( cr.s, cr.t ) )
				{
					continue;
				}

			if( cr.guidingUseDirectContribution )
			{
				scratch.directContributions[cr.guidingEyeVertexIndex] =
					scratch.directContributions[cr.guidingEyeVertexIndex] +
					cr.guidingLocalContribution;
				scratch.directMiWeights[cr.guidingEyeVertexIndex] = cr.misWeight;
				scratch.hasDirectContribution[cr.guidingEyeVertexIndex] = true;
			}
			else
			{
				scratch.localContributions[cr.guidingEyeVertexIndex] =
					scratch.localContributions[cr.guidingEyeVertexIndex] +
					(cr.guidingLocalContribution * cr.misWeight);
			}

			if( pStats )
			{
				const RISEPel effectiveContribution =
					cr.guidingLocalContribution * cr.misWeight;
				const Scalar energy =
					effectiveContribution[0] * Scalar( 0.2126 ) +
					effectiveContribution[1] * Scalar( 0.7152 ) +
					effectiveContribution[2] * Scalar( 0.0722 );
				if( energy > 0 )
				{
					localStats.totalEnergy += energy;
					if( cr.t >= 3 )
					{
						localStats.deepEyeConnectionEnergy += energy;
						localStats.deepEyeConnectionCount++;
					}
					else if( cr.s >= 2 && cr.t >= 2 )
					{
						localStats.firstSurfaceConnectionEnergy += energy;
						localStats.firstSurfaceConnectionCount++;
					}
				}
			}
		}

		const size_t requiredSegments = eyeVerts.size() > 1 ? eyeVerts.size() - 1 : 0;
		if( scratch.reservedSegments < requiredSegments ) {
			pglPathSegmentStorageReserve( scratch.pathSegments, requiredSegments );
			scratch.reservedSegments = requiredSegments;
		}
		pglPathSegmentStorageClear( scratch.pathSegments );

		for( unsigned int i = 1; i < eyeVerts.size(); i++ )
		{
			const BDPTVertex& v = eyeVerts[i];
			if( v.type != BDPTVertex::SURFACE || !v.guidingHasSegment ) {
				continue;
			}

			const RISEPel scatteredContribution =
				GuidingClampContribution( scratch.localContributions[i] );
			const RISEPel directContribution =
				GuidingClampContribution( scratch.directContributions[i] );
			const bool hasTerminalContribution =
				ColorMath::MaxValue( scatteredContribution ) > 0 ||
				ColorMath::MaxValue( directContribution ) > 0;

			// Only keep terminal vertices when they actually terminate with
			// radiance. Otherwise they add a synthetic dangling segment.
			if( !v.guidingHasDirectionIn && !hasTerminalContribution ) {
				continue;
			}

			PGLPathSegmentData* segment =
				pglPathSegmentStorageNextSegment( scratch.pathSegments );
			if( !segment ) {
				continue;
			}

			segment->position = GuidingPoint3f( v.position );
			segment->directionOut = GuidingVec3f( v.guidingDirectionOut );
			segment->normal = GuidingVec3f( v.guidingNormal );
			segment->volumeScatter = false;
			segment->transmittanceWeight = GuidingColor3f( RISEPel( 1, 1, 1 ) );
			segment->directContribution = GuidingColor3f( directContribution );
			segment->miWeight = static_cast<float>(
				scratch.hasDirectContribution[i] ?
					scratch.directMiWeights[i] :
					Scalar( 1.0 ) );
			segment->scatteredContribution = GuidingColor3f( scatteredContribution );
			segment->eta = static_cast<float>(
				v.guidingEta > NEARZERO ? v.guidingEta : 1.0 );

			if( v.guidingHasDirectionIn )
			{
				segment->directionIn = GuidingVec3f( v.guidingDirectionIn );
				segment->pdfDirectionIn = static_cast<float>(
					v.guidingPdfDirectionIn > 0 ? v.guidingPdfDirectionIn : 0 );
				segment->isDelta = v.isDelta;
				segment->scatteringWeight =
					GuidingColor3f( GuidingClampContribution( v.guidingScatteringWeight ) );
				segment->russianRouletteSurvivalProbability = static_cast<float>(
					v.guidingRussianRouletteSurvivalProbability > 0 ?
						v.guidingRussianRouletteSurvivalProbability : 1.0 );
				segment->roughness = static_cast<float>(
					v.guidingRoughness >= 0 ? v.guidingRoughness : 1.0 );
			}
			else
			{
				segment->directionIn = GuidingVec3f( Vector3( 0, 0, 0 ) );
				segment->pdfDirectionIn = 0.0f;
				segment->isDelta = false;
				segment->scatteringWeight = GuidingColor3f( RISEPel( 0, 0, 0 ) );
				segment->russianRouletteSurvivalProbability = 1.0f;
				segment->roughness = 1.0f;
			}
		}

		pGuidingField->AddPathSegments(
			scratch.pathSegments,
			false,
			false,
			true );

		// Flush this path's stats into the shared accumulator.
		// Single lock acquisition per path; no contention during
		// the per-result accumulation loop above.
		if( pStats && pStatsMutex )
		{
			std::lock_guard<std::mutex> lock( *pStatsMutex );
			pStats->totalEnergy += localStats.totalEnergy;
			pStats->firstSurfaceConnectionEnergy += localStats.firstSurfaceConnectionEnergy;
			pStats->deepEyeConnectionEnergy += localStats.deepEyeConnectionEnergy;
			pStats->firstSurfaceConnectionCount += localStats.firstSurfaceConnectionCount;
			pStats->deepEyeConnectionCount += localStats.deepEyeConnectionCount;
		}
	}

	/// Record light subpath vertices as guiding training samples.
	///
	/// For the shared guiding field (Option A), light subpath segments
	/// are recorded in REVERSE order so that OpenPGL's incident-radiance
	/// semantics align with the light transport direction.  At each
	/// surface position the "incoming direction" becomes the direction
	/// from which light scatters toward that point — which is the
	/// outgoing direction in the light-forward walk (guidingDirectionOut).
	///
	/// Because the segment chain is reversed, pdfDirectionIn and
	/// scatteringWeight must describe the reversed directionIn (toward
	/// the light), not the forward scatter direction.  These are stored
	/// as guidingReversePdfDirectionIn and guidingReverseScatteringWeight
	/// on each vertex, computed during path construction using
	/// EvalPdfAtVertex with swapped arguments and the reciprocal BSDF.
	///
	/// The terminal segment (vertex 1, closest to the light, recorded
	/// last in the chain) carries Le (the emitted radiance) as
	/// directContribution so OpenPGL has a non-zero radiance source to
	/// backpropagate through the segment chain during training.  Le is
	/// recovered from vertex 0 as throughput[0] * pdfFwd[0], keeping
	/// it on the same scale as eye training contributions (~O(100)
	/// rather than ~O(millions) from the raw throughput).
	///
	/// Segments are only emitted for vertices within maxLightGuidingDepth
	/// of the light source and only when the vertex has valid guiding
	/// metadata.
	inline void RecordGuidingTrainingLightPath(
		PathGuidingField* pGuidingField,
		const std::vector<BDPTVertex>& lightVerts,
		unsigned int maxLightGuidingDepth
		)
	{
		if( !pGuidingField || !pGuidingField->IsCollectingTrainingSamples() ||
			maxLightGuidingDepth == 0 || lightVerts.size() <= 1 )
		{
			return;
		}

		// Defensive: detect a malformed light subpath with a second
		// LIGHT-type vertex mid-array.  Path-tree branching was excised
		// in 2026-05 so the only remaining producer of this shape would
		// be a future regression; guard kept as cheap insurance.
		for( size_t i = 1; i < lightVerts.size(); i++ ) {
			if( lightVerts[i].type == BDPTVertex::LIGHT ) {
				return;
			}
		}

		// Find usable surface vertices (skip vertex 0 which is the light itself)
		bool hasSegments = false;
		for( unsigned int i = 1; i < lightVerts.size(); i++ ) {
			if( lightVerts[i].type == BDPTVertex::SURFACE &&
				lightVerts[i].guidingHasSegment )
			{
				hasSegments = true;
				break;
			}
		}

		if( !hasSegments ) {
			return;
		}

		static thread_local GuidingTrainingPathScratch lightScratch;
		if( !lightScratch.pathSegments ) {
			return;
		}

		// Count eligible vertices within the depth limit
		const unsigned int maxVert =
			std::min( static_cast<unsigned int>( lightVerts.size() ),
					  maxLightGuidingDepth + 1u );  // +1 for the light vertex at index 0

		const size_t requiredSegments = maxVert > 1 ? maxVert - 1 : 0;
		if( lightScratch.reservedSegments < requiredSegments ) {
			pglPathSegmentStorageReserve( lightScratch.pathSegments, requiredSegments );
			lightScratch.reservedSegments = requiredSegments;
		}
		pglPathSegmentStorageClear( lightScratch.pathSegments );

		// Record segments in reverse order (from deepest vertex back
		// toward the light) so that OpenPGL interprets "directionIn"
		// as the direction from which radiance arrives — matching the
		// shared-field incident-radiance convention.
		//
		// For a light subpath [L, v1, v2, v3, ...], the reversed
		// segment chain is:
		//   segment at v3: directionOut = v3.guidingDirectionIn  (toward v4 or terminated)
		//                  directionIn  = v3.guidingDirectionOut  (toward v2, i.e. toward light)
		//   segment at v2: directionOut = v2.guidingDirectionIn  (toward v3)
		//                  directionIn  = v2.guidingDirectionOut  (toward v1)
		//   segment at v1: directionOut = v1.guidingDirectionIn  (toward v2)
		//                  directionIn  = v1.guidingDirectionOut  (toward light)
		//
		// This way OpenPGL sees each position with an "incoming"
		// direction pointing back toward the light source, which
		// is exactly "where does light come from at this point."
		//
		// The terminal segment (vertex 1, recorded last) carries the
		// light emission as directContribution so OpenPGL has a
		// non-zero radiance source to backpropagate through the chain.
		//
		// pdfDirectionIn and scatteringWeight use the REVERSE values
		// (guidingReversePdfDirectionIn / guidingReverseScatteringWeight)
		// computed during path construction, so they describe the
		// reversed directionIn (toward light), not the forward scatter.
		for( unsigned int i = maxVert - 1; i >= 1; i-- )
		{
			const BDPTVertex& v = lightVerts[i];
			if( v.type != BDPTVertex::SURFACE || !v.guidingHasSegment ) {
				continue;
			}

			// In the reversed chain, guidingHasDirectionIn becomes the
			// reversed directionOut (continuation toward the next vertex
			// in the reversed walk).  If the light path terminated before
			// scattering at this vertex (early-out, no material, RR kill),
			// guidingHasDirectionIn is false and the reversed directionOut
			// would be a zero vector.  Recording such a segment — even at
			// vertex 1 with its Le source — creates an inconsistent
			// OpenPGL segment (non-zero directContribution into a zero
			// directionOut).  Skip all dangling vertices uniformly.
			if( !v.guidingHasDirectionIn ) {
				continue;
			}

			PGLPathSegmentData* segment =
				pglPathSegmentStorageNextSegment( lightScratch.pathSegments );
			if( !segment ) {
				continue;
			}

			segment->position = GuidingPoint3f( v.position );
			segment->normal = GuidingVec3f( v.guidingNormal );
			segment->volumeScatter = false;
			segment->transmittanceWeight = GuidingColor3f( RISEPel( 1, 1, 1 ) );

			// Terminal segment (vertex 1, closest to light) carries the
			// emitted radiance Le as directContribution.  This is the
			// radiance arriving at vertex 1 from the light direction,
			// analogous to how eye paths set directContribution = Le
			// when a vertex directly hits a light surface (s=0 strategy).
			//
			// We recover Le from vertex 0: Le = throughput[0] * pdfFwd[0],
			// since throughput[0] = Le / (pdfSelect * pdfPosition) and
			// pdfFwd[0] = pdfSelect * pdfPosition.
			//
			// Using Le (~150) instead of throughput[1] (~6M for the
			// Cornell box) keeps the light training signal on the same
			// scale as eye training contributions, preventing the light
			// data from overwhelming the shared OpenPGL field.
			if( i == 1 )
			{
				const RISEPel Le = GuidingClampContribution(
					lightVerts[0].throughput * lightVerts[0].pdfFwd );
				segment->directContribution = GuidingColor3f( Le );
			}
			else
			{
				segment->directContribution = GuidingColor3f( RISEPel( 0, 0, 0 ) );
			}
			segment->miWeight = 1.0f;
			segment->scatteredContribution = GuidingColor3f( RISEPel( 0, 0, 0 ) );

			segment->eta = static_cast<float>(
				v.guidingEta > NEARZERO ? v.guidingEta : 1.0 );

			// Reversed directions: swap directionIn and directionOut
			// relative to the light-forward walk so OpenPGL sees the
			// incident radiance direction pointing toward the light.
			segment->directionOut = v.guidingHasDirectionIn ?
				GuidingVec3f( v.guidingDirectionIn ) :
				GuidingVec3f( Vector3( 0, 0, 0 ) );

			// "directionIn" for the reversed path is the direction
			// toward the previous vertex (toward the light source).
			// Use the REVERSE PDF and scatteringWeight that correspond
			// to this reversed directionIn, not the forward values.
			segment->directionIn = GuidingVec3f( v.guidingDirectionOut );
			segment->pdfDirectionIn = static_cast<float>(
				v.guidingReversePdfDirectionIn > 0 ?
					v.guidingReversePdfDirectionIn : 0 );
			segment->isDelta = v.isDelta;
			segment->scatteringWeight =
				GuidingColor3f( GuidingClampContribution(
					v.guidingReverseScatteringWeight ) );
			segment->russianRouletteSurvivalProbability = static_cast<float>(
				v.guidingRussianRouletteSurvivalProbability > 0 ?
					v.guidingRussianRouletteSurvivalProbability : 1.0 );
			segment->roughness = static_cast<float>(
				v.guidingRoughness >= 0 ? v.guidingRoughness : 1.0 );
		}

		// rrAffectsDirectContribution = false here, intentionally asymmetric
		// with the eye-path call above.  Eye paths build directContribution
		// inside an accumulating throughput that already carries (1/p_rr)
		// factors from earlier survivals, so OpenPGL must divide it back
		// out.  The reversed light-path's directContribution at vertex 1 is
		// Le itself (bare emission, recovered from lightVerts[0]); no RR has
		// been applied to it.  The russianRouletteSurvivalProbability stored
		// on segment v1 is the RR rolled AT v1 before continuing toward v2,
		// which is unrelated to Le's amplification history.  Setting this to
		// true would ask OpenPGL to divide Le by that unrelated p_rr.
		// Empirically (K=16 EXR, bdpt_jewel_vault, 16 SPP, RIS) the choice
		// is within trial-to-trial training-non-determinism noise, so this
		// stays false on theoretical grounds.
		pGuidingField->AddPathSegments(
			lightScratch.pathSegments,
			false,
			false,
			false );
	}

	inline void RecordCompletePathSamples(
		CompletePathGuide* pCompletePathGuide,
		const std::vector<BDPTVertex>& lightVerts,
		const std::vector<BDPTVertex>& eyeVerts,
		const std::vector<BDPTIntegrator::ConnectionResult>& results
		)
	{
		if( !pCompletePathGuide || !pCompletePathGuide->IsCollectingTrainingSamples() ) {
			return;
		}

		for( unsigned int i = 0; i < results.size(); i++ )
		{
			const BDPTIntegrator::ConnectionResult& cr = results[i];
			if( !cr.valid || cr.needsSplat || cr.t == 0 || cr.t > eyeVerts.size() ) {
				continue;
			}

			const Point3& eyePosition = eyeVerts[cr.t - 1].position;
			const Point3* pLightPosition = 0;
			if( cr.s > 0 && cr.s <= lightVerts.size() ) {
				pLightPosition = &lightVerts[cr.s - 1].position;
			}

			pCompletePathGuide->AddSample(
				cr.s,
				cr.t,
				eyePosition,
				pLightPosition,
				cr.contribution * cr.misWeight,
				cr.s == 0,
				cr.needsSplat );
		}
	}

	//////////////////////////////////////////////////////////////////
	// DL-67 (docs/DL67_GUIDED_GENERATING_DENSITY.md): the guided
	// continuation at one BDPT subpath vertex, shared by the eye and
	// light generators (and so by MLT, which drives this generator; VCM
	// shares it too but never installs a guiding field).
	//
	// TWO techniques estimate the continuation integral: the BSDF
	// technique (Scatter + RandomlySelect, lobe I with realized
	// probability q_I) and the guide technique (w ~ g).  At a guided
	// vertex the CHOICE between them is made with a probability that does
	// not depend on the Scatter realization -- alpha in one-sample mode,
	// "always both" in RIS mode -- and the two are combined through ONE
	// deterministic partition of directions,
	//
	//   W_g(w) = a g(w) / (a g(w) + (1-a) p_agg(w)),   W_b = 1 - W_g,
	//
	// `p_agg` the AGGREGATE `ISPF::Pdf()` and `a` the one-sample alpha
	// (1/2 in RIS).  Each technique prices its UNWEIGHTED estimator times
	// its share, over the probability it fired:
	//
	//   lobe I kept (non-delta)  kray_I / q_I * W_b(w_I) / (1 - a)
	//   lobe I kept (delta)      kray_I / q_I          / (1 - a)
	//   guide draw               f_agg cos / g * W_g / a  =  f_agg cos / p_mix
	//
	// with p_mix = a g + (1 - a) p_agg.  The realization-independent
	// choice is what makes this unbiased when the Scatter realization
	// can lack a guide-eligible lobe (a zero-weight or horizon-dropped
	// diffuse ray, an empty container): an earlier per-lobe rule let the
	// guide fire only when the SELECTED lobe was eligible, which made
	// "can the guide fire here" a random event and biased BDPT by
	// P(no eligible lobe) * integral f cos W_g (+4.7% on topology L with
	// a black-diffuse `schlick_material`).  `hasLobe == false` (empty
	// container) is a zero sample of the kept technique, never a reason
	// to skip the guide.  A guide draw that yields nothing is a zero
	// sample of the guide technique, never a fall-back to the lobe.
	//
	// PREMISES (doc §2).  (2) `IBSDF::value` and the SPF's `kray` describe
	// ONE function -- the guide prices `value`, the kept lobes price
	// `kray`; `polished_material` violated it until DL-285's PolishedBRDF
	// (2026-09-28).  (3) 0 < a < 1
	// where the kept technique owns mass the guide cannot reach (a delta
	// lobe) -- enforced below through `GuidingOneSampleProbability`.
	// (4) The continuation state must not depend on which technique chose
	// the direction; a substituted vertex continues as a non-delta
	// `eRayDiffuse` event, so under a per-type bounce cap the two
	// techniques integrate differently truncated paths (documented
	// limitation, not enforced).
	//////////////////////////////////////////////////////////////////
	template<class V>
	struct BDPTGuidedChoice
	{
		bool	substituted;	///< the guide's direction replaced the lobe's
		bool	terminate;		///< the chosen technique contributed zero
		Vector3	dir;			///< substituted direction
		V		f;				///< aggregate BSDF at `dir` (substituted only)
		Scalar	equivPdf;		///< substituted: weight == f cos / equivPdf
		Scalar	keptScale;		///< kept lobe: multiplies kray / selectProb (delta lobes too)
		Scalar	aggAtTrace;		///< aggregate pdf at the traced direction, -1 = not evaluated

		BDPTGuidedChoice() :
			substituted( false ), terminate( false ), dir( 0, 0, 0 ),
			f(), equivPdf( 0 ), keptScale( 1 ), aggAtTrace( -1 )
		{}
	};

	template<class Tag, class V, class EvalF, class EvalP>
	inline void BDPTGuidedContinuation(
		PathGuidingField& field,
		GuidingDistributionHandle& dist,
		const ScatteredRay& scat,
		const bool hasLobe,
		const Scalar selectProb,
		const Scalar lobeMagnitude,		///< max-channel kray / selectProb of the kept lobe
		const Scalar alpha,
		const GuidingSamplingType samplingType,
		const Vector3& normal,
		ISampler& sampler,
		const EvalF& evalF,
		const EvalP& evalPdf,
		BDPTGuidedChoice<V>& out )
	{
		typedef SpectralValueTraits<Tag> Traits;
		const Vector3 wSel = scat.ray.Dir();

		if( samplingType == eGuidingRIS )
		{
			// Both candidates at every guided vertex (eps = 1).  Candidate
			// 0 is the kept lobe (absent when the container was empty),
			// candidate 1 a guide draw; the resampled output
			// `c_y * sum(w) / w_y` has conditional mean `c_0 + c_1` for ANY
			// positive weights, so a weight only has to be positive where
			// its contribution is.
			const Scalar mixA = Scalar( 0.5 );
			Scalar w[2] = { 0, 0 };
			Scalar v0 = 0;
			Scalar agg0 = -1;
			if( hasLobe && lobeMagnitude > 0 )
			{
				if( scat.isDelta ) {
					// The guide cannot produce a delta direction: W_b = 1.
					v0 = 1;
					w[0] = lobeMagnitude;
				} else {
					const V f0 = evalF( wSel );
					agg0 = evalPdf( wSel );
					const Scalar g0 = field.Pdf( dist, wSel );
					const Scalar cos0 = fabs( Vector3Ops::Dot( wSel, normal ) );
					const Scalar target0 = PathTransportUtilities::GuidingRISTarget(
						Traits::max_value( f0 ), cos0,
						field.IncomingRadiancePdf( dist, wSel ), alpha );
					// DL-103's guard: a zero aggregate at a generated
					// direction falls back to the lobe's own
					// `selectProb * pdf` in the (free) weight.
					const Scalar risPdf0 = PathTransportUtilities::GuidingRISProposalPdf(
						agg0 > 0 ? agg0 : selectProb * scat.pdf, g0 );
					v0 = PathTransportUtilities::GuidingPartitionBsdfWeight( mixA, g0, agg0 );
					w[0] = ( risPdf0 > NEARZERO && target0 > 0 ) ? target0 / risPdf0 : lobeMagnitude;
					if( v0 <= 0 ) {
						w[0] = 0;
					}
				}
			}

			V f1 = Traits::zero();
			Vector3 dir1( 0, 0, 0 );
			Scalar agg1 = -1;
			Scalar sum1 = 0;
			{
				Scalar gPdf = 0;
				const Point2 xi2d( sampler.Get1D(), sampler.Get1D() );
				dir1 = field.Sample( dist, xi2d, gPdf );
				// `> 0`: `g` divides out of candidate 1's contribution
				// `f cos W_g / g = f cos / (g + p_agg)`.
				if( gPdf > 0 )
				{
					f1 = evalF( dir1 );
					agg1 = evalPdf( dir1 );
					const Scalar cos1 = fabs( Vector3Ops::Dot( dir1, normal ) );
					const Scalar target1 = PathTransportUtilities::GuidingRISTarget(
						Traits::max_value( f1 ), cos1,
						field.IncomingRadiancePdf( dist, dir1 ), alpha );
					const Scalar risPdf1 = PathTransportUtilities::GuidingRISProposalPdf(
						agg1 > 0 ? agg1 : Scalar( 0 ), gPdf );
					sum1 = gPdf + ( agg1 > 0 ? agg1 : Scalar( 0 ) );
					// The weight is positive wherever candidate 1's
					// contribution `f cos / (g + p_agg)` is (review P3).
					const Scalar c1 = Traits::max_value( f1 ) * cos1 / sum1;
					if( c1 > 0 ) {
						w[1] = ( risPdf1 > NEARZERO && target1 > 0 ) ? target1 / risPdf1 : c1;
					}
				}
			}

			const Scalar total = w[0] + w[1];
			if( !( total > 0 ) ) {
				out.terminate = true;
				return;
			}
			const Scalar xiRIS = sampler.Get1D();
			const unsigned int sel = ( xiRIS * total < w[0] ) ? 0u : 1u;
			if( w[sel] <= 0 ) {
				out.terminate = true;
				return;
			}
			const Scalar resample = total / w[sel];
			if( sel == 0 ) {
				out.keptScale = v0 * resample;
				out.aggAtTrace = agg0;
			} else {
				out.substituted = true;
				out.dir = dir1;
				out.f = f1;
				// c_1 * resample = f cos / (g + p_agg) * total / w_1
				out.equivPdf = sum1 * w[1] / total;
				out.aggAtTrace = agg1;
			}
			return;
		}

		// One-sample MIS with the realization-independent coin.  The
		// firing probability is clamped strictly below 1 (DL-67 round 3,
		// premise 3): at 1 the kept technique never fires and every delta
		// lobe's transport is lost.
		const Scalar a = PathTransportUtilities::GuidingOneSampleProbability( alpha );
		const Scalar xi = sampler.Get1D();
		if( PathTransportUtilities::ShouldUseGuidedSample( a, xi ) )
		{
			Scalar gPdf = 0;
			const Point2 xi2d( sampler.Get1D(), sampler.Get1D() );
			const Vector3 gDir = field.Sample( dist, xi2d, gPdf );
			// `> 0`: `g` divides out of `f cos W_g / (g a) = f cos / p_mix`.
			if( gPdf > 0 )
			{
				const Scalar agg = evalPdf( gDir );
				out.substituted = true;
				out.dir = gDir;
				out.f = evalF( gDir );
				out.equivPdf = PathTransportUtilities::GuidingCombinedPdf(
					a, gPdf, agg > 0 ? agg : Scalar( 0 ) );
				out.aggAtTrace = agg;
				return;
			}
			out.terminate = true;
			return;
		}

		// Kept, with probability `1 - a > 0`.
		if( !hasLobe || lobeMagnitude <= 0 ) {
			out.terminate = true;
			return;
		}
		if( scat.isDelta ) {
			out.keptScale = Scalar( 1 ) / ( Scalar( 1 ) - a );
			return;
		}
		const Scalar agg = evalPdf( wSel );
		out.aggAtTrace = agg;
		out.keptScale = PathTransportUtilities::GuidingPartitionBsdfWeight(
			a, field.Pdf( dist, wSel ), agg ) / ( Scalar( 1 ) - a );
		if( out.keptScale <= 0 ) {
			out.terminate = true;
		}
	}
#endif
}

//////////////////////////////////////////////////////////////////////
// Construction / Destruction
//////////////////////////////////////////////////////////////////////

BDPTIntegrator::BDPTIntegrator(
	unsigned int maxEye,
	unsigned int maxLight,
	const StabilityConfig& stabilityCfg
	) :
  maxEyeDepth( maxEye ),
  maxLightDepth( maxLight ),
  pLightSampler( 0 ),
  stabilityConfig( stabilityCfg )
#ifdef RISE_ENABLE_OPENPGL
  ,pGuidingField( 0 ),
  pLightGuidingField( 0 ),
  pCompletePathGuide( 0 ),
  guidingAlpha( 0 ),
  maxGuidingDepth( 3 ),
  maxLightGuidingDepth( 0 ),
  guidingSamplingType( eGuidingOneSampleMIS ),
  guidingRISCandidates( 2 ),
  completePathStrategySelectionEnabled( false ),
  completePathStrategySampleCount( 0 ),
  strategySelectionPathCount( 0 ),
  strategySelectionCandidateCount( 0 ),
  strategySelectionEvaluatedCount( 0 ),
  guidingTrainingStats()
#endif
{
}

BDPTIntegrator::~BDPTIntegrator()
{
	safe_release( pLightSampler );
}

void BDPTIntegrator::SetLightSampler( const LightSampler* pSampler )
{
	if( pSampler == pLightSampler ) {
		return;
	}

	safe_release( pLightSampler );
	pLightSampler = pSampler;

	if( pLightSampler ) {
		pLightSampler->addref();
	}
}

#ifdef RISE_ENABLE_OPENPGL
void BDPTIntegrator::SetGuidingField( PathGuidingField* pField, PathGuidingField* pLightField, Scalar alpha, unsigned int maxDepth, unsigned int maxLightDepth, GuidingSamplingType samplingType, unsigned int risCandidates )
{
	pGuidingField = pField;
	pLightGuidingField = pLightField;
	guidingAlpha = alpha;
	maxGuidingDepth = maxDepth;
	maxLightGuidingDepth = maxLightDepth;
	guidingSamplingType = samplingType;
	guidingRISCandidates = risCandidates;
}

void BDPTIntegrator::SetCompletePathGuide(
	CompletePathGuide* pGuide,
	bool enableStrategySelection,
	unsigned int strategySamples )
{
	pCompletePathGuide = pGuide;
	completePathStrategySelectionEnabled = enableStrategySelection;
	completePathStrategySampleCount = strategySamples;
}

void BDPTIntegrator::ResetGuidingTrainingStats() const
{
	// Typically called from the dispatcher between training
	// iterations (single-threaded), but take the lock anyway so
	// we're defensible against future callers that reset mid-pass.
	std::lock_guard<std::mutex> lock( guidingTrainingStatsMutex );
	guidingTrainingStats = GuidingTrainingStats();
}

BDPTIntegrator::GuidingTrainingStats
BDPTIntegrator::GetGuidingTrainingStats() const
{
	// Returns a snapshot copy under the lock so the caller keeps a
	// stable value even if a subsequent training iteration starts
	// accumulating into guidingTrainingStats.
	std::lock_guard<std::mutex> lock( guidingTrainingStatsMutex );
	return guidingTrainingStats;
}

void BDPTIntegrator::ResetStrategySelectionStats() const
{
	strategySelectionPathCount.store( 0 );
	strategySelectionCandidateCount.store( 0 );
	strategySelectionEvaluatedCount.store( 0 );
}

void BDPTIntegrator::GetStrategySelectionStats(
	unsigned long long& pathCount,
	unsigned long long& candidateCount,
	unsigned long long& evaluatedCount
	) const
{
	pathCount = strategySelectionPathCount.load();
	candidateCount = strategySelectionCandidateCount.load();
	evaluatedCount = strategySelectionEvaluatedCount.load();
}
#endif

//////////////////////////////////////////////////////////////////////
// Helper: evaluate transmittance along a connection edge.
//
// For now this only handles the global medium (scene-level fog).
// Step 5 upgrades this to full boundary-walk transmittance for
// per-object media.  Without any medium in the scene, this returns
// (1,1,1) — the identity — so non-media scenes are unaffected.
//
// Transmittance along shared edges cancels in the MIS ratio walk.
// Both forward and reverse sampling traverse the same geometric
// edge with identical transmittance, so Tr factors appear in both
// numerator and denominator of pdfRev/pdfFwd and cancel.
// Therefore pdfFwd and pdfRev do NOT include Tr — only the
// directional PDF and the area-measure conversion factor
// (|cos|/dist^2 for surfaces, sigma_t/dist^2 for media).
// Connection edge Tr is applied as a multiplicative factor on
// the contribution, not in the MIS weight.
//////////////////////////////////////////////////////////////////////

/// Connection edge transmittance with boundary walking.
///
/// Evaluates transmittance along the segment from p1 to p2, accounting
/// for nested, overlapping, and disjoint per-object media as well as
/// the global medium.  Uses the same boundary-walk algorithm as
/// LightSampler::EvalShadowTransmittance.
///
/// The walk starts at p1 and traces toward p2, finding medium
/// boundaries (object surfaces with interior media).  At each
/// boundary, the active medium's transmittance is accumulated for
/// the traversed segment, and the medium stack is updated.  The
/// global medium fills segments where no per-object medium is active.
///
/// This Tr multiplies the connection contribution but is NOT included
/// in MIS PDFs (transmittance cancels in the MIS ratio walk — see
/// the note in ConnectAndEvaluate).

/// Small fixed-capacity stack of active per-object media along a
/// connection segment.
namespace {
	struct ConnectionMediumStack
	{
		static const int MAX_DEPTH = 4;

		struct Entry
		{
			const IObject* pObj;
			const IMedium* pMedium;
		};

		Entry entries[MAX_DEPTH];
		int   count;

		ConnectionMediumStack() : count( 0 ) {}

		void push( const IObject* pObj, const IMedium* pMedium )
		{
			if( count < MAX_DEPTH ) {
				entries[count].pObj = pObj;
				entries[count].pMedium = pMedium;
				count++;
			}
		}

		void remove( const IObject* pObj )
		{
			for( int i = 0; i < count; i++ ) {
				if( entries[i].pObj == pObj ) {
					for( int j = i; j < count - 1; j++ ) {
						entries[j] = entries[j + 1];
					}
					count--;
					return;
				}
			}
		}

		const IMedium* top() const
		{
			return count > 0 ? entries[count - 1].pMedium : 0;
		}
	};

	//////////////////////////////////////////////////////////////////
	// Tag-dispatched helpers for the templated connection-transmittance
	// walk (EvalConnectionTransmittanceImpl below).  Each forwards at
	// compile time to the existing Pel or NM path, so the single
	// templated body expands to the same machine code the hand-written
	// EvalConnectionTransmittance{,NM} bodies produced.  Phase 2c part 1
	// (lowest-divergence BDPT family).
	//////////////////////////////////////////////////////////////////

	/// Multiplicative identity transmittance of the tag value type:
	/// RISEPel(1,1,1) for Pel, 1.0 for NM.  (SpectralValueTraits has
	/// zero() but no one(); the walk needs the multiplicative identity.)
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type TrOne();

	template<> inline RISEPel TrOne<PelTag>() { return RISEPel( 1, 1, 1 ); }
	template<> inline Scalar  TrOne<NMTag>()  { return Scalar( 1 ); }

	/// Per-segment medium transmittance dispatch.  IMedium exposes both
	/// EvalTransmittance (RISEPel) and EvalTransmittanceNM (Scalar, nm),
	/// so this is a direct compile-time pick.
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type
	EvalMediumTransmittance(
		const IMedium& medium, const Ray& ray, const Scalar dist, const Tag& tag );

	template<>
	inline RISEPel EvalMediumTransmittance<PelTag>(
		const IMedium& medium, const Ray& ray, const Scalar dist, const PelTag& )
	{
		return medium.EvalTransmittance( ray, dist );
	}

	template<>
	inline Scalar EvalMediumTransmittance<NMTag>(
		const IMedium& medium, const Ray& ray, const Scalar dist, const NMTag& tag )
	{
		return medium.EvalTransmittanceNM( ray, dist, tag.nm );
	}

	/// Analog no-scatter SURVIVAL weight for a subpath that reaches a
	/// surface (or escapes) WITHOUT a medium scatter event.  Mirrors
	/// PathTracingIntegrator's PTSurvivalWeight exactly.
	///
	/// SampleDistance is an ANALOG estimator: sampling a free-flight
	/// distance PAST the surface (i.e. "no scatter") is a stochastic
	/// SURVIVAL outcome whose probability already carries the Beer-Lambert
	/// factor.  Multiplying beta by the full transmittance Tr AGAIN
	/// double-counts attenuation (a pure absorber would render
	/// exp(-2*sigma_a*d), the slab ~2x too thick).  The correct weight is
	/// Tr / pSurvival, where pSurvival is the DETERMINISTIC no-scatter
	/// survival pdf from IMedium::EvalDistancePdf[NM]( ray, dist,
	/// /*scattered=*/false, dist ).
	///
	/// Why the deterministic pdf and NOT MinValue(EvalTransmittance): for a
	/// HomogeneousMedium the two are identical -- EvalDistancePdf(false)
	/// returns exp(-sigma_t_max*d) = MinValue(Tr) (Pel), and
	/// EvalDistancePdfNM(false) returns exp(-sigma_t(nm)*d) =
	/// EvalTransmittanceNM (so the NM weight is exactly 1, byte-identical to
	/// the previous unconditional identity).  For a HeterogeneousMedium,
	/// EvalTransmittance is a STOCHASTIC ratio-tracking estimate, so
	/// MinValue(EvalTransmittance) would be a random denominator (a biased
	/// ratio-of-random-estimates); HeterogeneousMedium overrides
	/// EvalDistancePdf[NM] with a deterministic Simpson optical depth, so
	/// Tr / pSurvival is the correct unbiased no-scatter weight there.
	///
	/// Guard: divide only when pSurvival > 0 (a non-positive survival pdf
	/// means "no attenuation to apply"); otherwise return the value-type
	/// multiplicative identity.
	inline RISEPel BDPTSurvivalWeight( const RISEPel& Tr, const Scalar pSurvival )
	{
		if( pSurvival > 0 ) {
			return Tr * ( Scalar( 1 ) / pSurvival );
		}
		return RISEPel( 1, 1, 1 );
	}
	inline Scalar BDPTSurvivalWeight( const Scalar Tr, const Scalar pSurvival )
	{
		if( pSurvival > 0 ) {
			return Tr / pSurvival;
		}
		return Scalar( 1 );
	}

	/// Deterministic no-scatter survival pdf: the denominator of
	/// BDPTSurvivalWeight.  Pel routes to EvalDistancePdf, NM to
	/// EvalDistancePdfNM.  For HomogeneousMedium this equals MinValue(Tr)
	/// (Pel) / EvalTransmittanceNM (NM); for HeterogeneousMedium it is the
	/// deterministic Simpson optical depth (NOT the stochastic
	/// EvalTransmittance).
	template<class Tag>
	inline Scalar EvalNoScatterSurvivalPdf(
		const IMedium& medium, const Ray& ray, const Scalar dist, const Tag& tag );

	template<>
	inline Scalar EvalNoScatterSurvivalPdf<PelTag>(
		const IMedium& medium, const Ray& ray, const Scalar dist, const PelTag& )
	{
		return medium.EvalDistancePdf( ray, dist, /*scattered=*/false, dist );
	}

	template<>
	inline Scalar EvalNoScatterSurvivalPdf<NMTag>(
		const IMedium& medium, const Ray& ray, const Scalar dist, const NMTag& tag )
	{
		return medium.EvalDistancePdfNM( ray, dist, /*scattered=*/false, dist, tag.nm );
	}

	/// Scalar magnitude for the "transmittance effectively zero" early
	/// out (< 1e-6).  Pel: max channel (ColorMath::MaxValue, the RGB
	/// original).  NM: the bare scalar (the NM original tested Tr < 1e-6
	/// directly).  Deliberately NOT SpectralValueTraits::max_value, which
	/// is fabs() for NM: Tr is a product of [0,1] transmittances so it is
	/// always >= 0 and fabs would be a no-op, but reproducing the bare
	/// scalar keeps this byte-identical to the pre-refactor NM body and
	/// avoids re-introducing a fabs-in-a-gate (the Phase 2a P2 #2 footgun).
	template<class Tag>
	inline Scalar TrEarlyOutMagnitude(
		const typename SpectralValueTraits<Tag>::value_type& Tr );

	template<> inline Scalar TrEarlyOutMagnitude<PelTag>( const RISEPel& Tr )
	{ return ColorMath::MaxValue( Tr ); }
	template<> inline Scalar TrEarlyOutMagnitude<NMTag>( const Scalar& Tr )
	{ return Tr; }

	/// Templated connection-edge transmittance walk shared by
	/// EvalConnectionTransmittance (Pel) and EvalConnectionTransmittanceNM.
	/// Boundary-walks [0, maxDist] along connectionRay, accumulating
	/// per-object + global medium transmittance.  Uses no BDPTIntegrator
	/// member state, so it lives as a free function here (matching
	/// VCMIntegrator's *Impl<Tag> house pattern); the public member
	/// overloads are one-line forwarders.  `caster` is unused (kept for
	/// signature symmetry with the member overloads, as in the originals).
	template<class Tag>
	typename SpectralValueTraits<Tag>::value_type
	EvalConnectionTransmittanceImpl(
		const Ray& connectionRay,
		const Scalar maxDist,
		const IScene& scene,
		const IRayCaster& caster,
		const Tag& tag,
		const IObject* pStartMediumObject,
		const IMedium* pStartMedium )
	{
		typedef SpectralValueTraits<Tag> Traits;
		typedef typename Traits::value_type V;

		const IMedium* pGlobalMedium = scene.GetGlobalMedium();

		if( maxDist < BDPT_RAY_EPSILON ) {
			return TrOne<Tag>();
		}

		const Vector3 d = connectionRay.Dir();

		const IObjectManager* pObjects = scene.GetObjects();
		if( !pGlobalMedium && !pObjects && !pStartMedium ) {
			return TrOne<Tag>();
		}

		// Fast path: no per-object media, just global medium
		if( !pObjects && !pStartMedium ) {
			if( pGlobalMedium ) {
				return EvalMediumTransmittance<Tag>( *pGlobalMedium, connectionRay, maxDist, tag );
			}
			return TrOne<Tag>();
		}

		static const Scalar WALK_EPSILON = 1e-5;
		static const int MAX_WALK_STEPS = 16;

		V Tr = TrOne<Tag>();
		ConnectionMediumStack stack;

		// Pre-seed the medium stack if p1 is inside a per-object medium.
		// Without this, the first segment [p1, first_boundary] would have
		// no active medium, causing per-object media to be invisible in
		// BDPT connections from/to interior medium vertices.
		if( pStartMediumObject && pStartMedium ) {
			stack.push( pStartMediumObject, pStartMedium );
		}

		Scalar segStart = 0;
		Scalar objectCoveredDist = 0;

		for( int step = 0; step < MAX_WALK_STEPS && segStart < maxDist; step++ )
		{
			const Scalar castStart = segStart + WALK_EPSILON;
			if( castStart >= maxDist ) {
				break;
			}

			const Point3 castOrigin = connectionRay.PointAtLength( castStart );
			const Ray castRay( castOrigin, d );
			const Scalar castMax = maxDist - castStart;

			RasterizerState nullRast = {0};
			RayIntersection ri( castRay, nullRast );
			pObjects->IntersectRay( ri, true, true, false );

			if( !ri.geometric.bHit || ri.geometric.range >= castMax ) {
				// No more boundaries before p2
				const Scalar remaining = maxDist - segStart;
				if( remaining > 0 ) {
					const IMedium* pActive = stack.top();
					if( pActive ) {
						const Ray segRay( connectionRay.PointAtLength( segStart ), d );
						Tr = Tr * EvalMediumTransmittance<Tag>( *pActive, segRay, remaining, tag );
						objectCoveredDist += remaining;
					}
				}
				segStart = maxDist;
				break;
			}

			const IObject* pHitObj = ri.pObject;
			if( !pHitObj ) {
				break;
			}

			const Scalar boundaryDist = castStart + ri.geometric.range;

			// Apply active medium for [segStart, boundaryDist]
			const Scalar segLen = boundaryDist - segStart;
			if( segLen > 0 ) {
				const IMedium* pActive = stack.top();
				if( pActive ) {
					const Ray segRay( connectionRay.PointAtLength( segStart ), d );
					Tr = Tr * EvalMediumTransmittance<Tag>( *pActive, segRay, segLen, tag );
					objectCoveredDist += segLen;
				}
			}

			// Update stack based on boundary crossing
			const IMedium* pObjMedium = pHitObj->GetInteriorMedium();
			if( pObjMedium ) {
				// Medium boundary push/pop: GEOMETRIC normal — entering vs
				// exiting a closed solid is a topology question (PBRT 4e
				// §11.3.4).  Using shading on bumpy dielectrics mis-orders
				// the medium stack on connection rays.
				//
				// DL-70 (the exact twin of LightSampler.cpp's NEE shadow
				// walk — see the long note there): it must be the TRUE,
				// ray-INDEPENDENT geometric normal.  On a double-sided
				// mesh the reported one always opposes the ray, so the
				// raw dot read "entering" at every crossing and the stack
				// was never unwound.  `TrueGeomFacing` undoes the flip and
				// is a no-op on every geometry that does not set it; a
				// RAY-DERIVED normal (HairGeometry) is skipped, since a
				// 1-D curve has no interior for a medium to occupy.
				//
				// DL-97 CONTRACT (pinned by
				// tests/HairInteriorMediumSkipTest.cpp, not a runtime
				// assert -- this connection walk is per-sample hot path,
				// the same "no asserts" convention this file's own
				// MISWeight documents): correct ONLY because every
				// geometry setting `bGeomNormalRayDerived` (HairGeometry,
				// today the only one) ALSO reports a zero-length chord
				// (`range2 == range`, HairGeometry.cpp's own "no volume:
				// exit == entry" contract) -- nothing for this walk to
				// integrate either way.  A future geometry setting the
				// flag with a genuine positive-length chord would need
				// real handling here, not a widened skip.
				if( !ri.geometric.HasTrueGeomSide() ) {
					segStart = boundaryDist;
					continue;
				}
				const Scalar ndotd = ri.geometric.TrueGeomFacing( d );
				if( ndotd < 0 ) {
					stack.push( pHitObj, pObjMedium );
				} else {
					stack.remove( pHitObj );
				}
			}

			segStart = boundaryDist;

			if( TrEarlyOutMagnitude<Tag>( Tr ) < 1e-6 ) {
				return Traits::zero();
			}
		}

		// Handle remaining distance
		if( segStart < maxDist ) {
			const Scalar remaining = maxDist - segStart;
			if( remaining > 0 ) {
				const IMedium* pActive = stack.top();
				if( pActive ) {
					const Ray segRay( connectionRay.PointAtLength( segStart ), d );
					Tr = Tr * EvalMediumTransmittance<Tag>( *pActive, segRay, remaining, tag );
					objectCoveredDist += remaining;
				}
			}
		}

		// Apply global medium for segments where no per-object medium was active
		if( pGlobalMedium ) {
			const Scalar globalDist = maxDist - objectCoveredDist;
			if( globalDist > WALK_EPSILON ) {
				Tr = Tr * EvalMediumTransmittance<Tag>( *pGlobalMedium, connectionRay, globalDist, tag );
			}
		}

		return Tr;
	}
}

RISEPel BDPTIntegrator::EvalConnectionTransmittance(
	const Point3& p1,
	const Point3& p2,
	const IScene& scene,
	const IRayCaster& caster,
	const IObject* pStartMediumObject,
	const IMedium* pStartMedium
	) const
{
	Vector3 d = Vector3Ops::mkVector3( p2, p1 );
	const Scalar maxDist = Vector3Ops::Magnitude( d );
	if( maxDist < BDPT_RAY_EPSILON ) {
		return RISEPel( 1, 1, 1 );
	}
	d = d * (1.0 / maxDist);
	const Ray connectionRay( p1, d );
	return EvalConnectionTransmittance(
		connectionRay, maxDist, scene, caster,
		pStartMediumObject, pStartMedium );
}

RISEPel BDPTIntegrator::EvalConnectionTransmittance(
	const Ray& connectionRay,
	const Scalar maxDist,
	const IScene& scene,
	const IRayCaster& caster,
	const IObject* pStartMediumObject,
	const IMedium* pStartMedium
	) const
{
	return EvalConnectionTransmittanceImpl<PelTag>(
		connectionRay, maxDist, scene, caster, PelTag{},
		pStartMediumObject, pStartMedium );
}

/// Spectral variant of EvalConnectionTransmittance.
/// Same boundary-walk algorithm operating on scalar transmittance
/// at a single wavelength.
Scalar BDPTIntegrator::EvalConnectionTransmittanceNM(
	const Point3& p1,
	const Point3& p2,
	const IScene& scene,
	const IRayCaster& caster,
	const Scalar nm,
	const IObject* pStartMediumObject,
	const IMedium* pStartMedium
	) const
{
	Vector3 d = Vector3Ops::mkVector3( p2, p1 );
	const Scalar maxDist = Vector3Ops::Magnitude( d );
	if( maxDist < BDPT_RAY_EPSILON ) {
		return 1.0;
	}
	d = d * (1.0 / maxDist);
	const Ray connectionRay( p1, d );
	return EvalConnectionTransmittanceNM(
		connectionRay, maxDist, scene, caster, nm,
		pStartMediumObject, pStartMedium );
}

Scalar BDPTIntegrator::EvalConnectionTransmittanceNM(
	const Ray& connectionRay,
	const Scalar maxDist,
	const IScene& scene,
	const IRayCaster& caster,
	const Scalar nm,
	const IObject* pStartMediumObject,
	const IMedium* pStartMedium
	) const
{
	return EvalConnectionTransmittanceImpl<NMTag>(
		connectionRay, maxDist, scene, caster, NMTag( nm ),
		pStartMediumObject, pStartMedium );
}

// SampleBSSRDFEntryPoint has been extracted to BSSRDFSampling.h.
// See src/Library/Utilities/BSSRDFSampling.h for the implementation.


namespace {
	// Forward declaration of the templated light-subpath generator (F2b).
	// Defined just before GenerateLightSubpathNM, where it can see the F2a
	// subpath-gen dispatch helpers (StoreThroughput / KrayValue / ... ) it
	// shares with GenerateEyeSubpathImpl.  The Pel forwarder below calls it;
	// implicit instantiation is deferred to end-of-TU so the later definition
	// is in scope.
	template<class Tag>
	unsigned int GenerateLightSubpathImpl(
		unsigned int maxLightDepth,
		const StabilityConfig& stabilityConfig,
		const LightSampler* pLightSampler,
	#ifdef RISE_ENABLE_OPENPGL
		PathGuidingField* pLightGuidingField,
		unsigned int maxLightGuidingDepth,
		Scalar guidingAlpha,
		GuidingSamplingType guidingSamplingType,
	#endif
		const IScene& scene,
		const IRayCaster& caster,
		ISampler& sampler,
		const RandomNumberGenerator& random,
		std::vector<BDPTVertex>& vertices,
		std::vector<uint32_t>& subpathStarts,
		const Tag& tag,
		const SampledWavelengths* pSwlHWSS );
}

//////////////////////////////////////////////////////////////////////
// GenerateLightSubpath
//
// Traces a path starting from a randomly sampled light source.
// Vertex 0 is the light itself (type LIGHT), subsequent vertices
// are surface intersections.
//
// Light selection and emission sampling are delegated to
// LightSampler, which selects lights proportional to their radiant
// exitance (same strategy used by PhotonTracer for consistency).
//
// Throughput (beta) tracks the path weight beyond vertex 0:
//   beta_0 = Le * |cos(theta_0)| / (pdfSelect * pdfPos * pdfDir)
// At each bounce:
//   delta:     beta *= kray  (kray = f*|cos|/pdf, precomputed by SPF)
//   non-delta: beta *= f * |cos| / pdf  (f from BSDF evaluation)
//////////////////////////////////////////////////////////////////////

unsigned int BDPTIntegrator::GenerateLightSubpath(
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	std::vector<BDPTVertex>& vertices,
	std::vector<uint32_t>& subpathStarts,
	const RandomNumberGenerator& random
	) const
{
	return GenerateLightSubpathImpl<PelTag>(
		maxLightDepth, stabilityConfig, pLightSampler,
#ifdef RISE_ENABLE_OPENPGL
		pLightGuidingField, maxLightGuidingDepth, guidingAlpha, guidingSamplingType,
#endif
		scene, caster, sampler, random,
		vertices, subpathStarts, PelTag{}, nullptr );
}


//////////////////////////////////////////////////////////////////////
// GenerateEyeSubpath
//
// Traces a path starting from the camera.  Vertex 0 is the camera
// itself (type CAMERA, pdfFwd=1, throughput=1), subsequent vertices
// are surface intersections.
//
// The camera's directional PDF (BDPTCameraUtilities::PdfDirection)
// is used as pdfFwdPrev for the first surface vertex.  For pinhole
// cameras this is 1/cos^3(theta) * focaldist^2 (Veach eq. 8.10).
//////////////////////////////////////////////////////////////////////

namespace {

	//////////////////////////////////////////////////////////////////
	// Tag-dispatched helpers for the templated eye-subpath generator
	// (GenerateEyeSubpathImpl below).  Phase 2c family F2a -- the
	// camera-side half of subpath generation.  Each forwards at compile
	// time to the existing Pel or NM path, so the single templated body
	// expands to the same machine code the hand-written
	// GenerateEyeSubpath{,NM} bodies produced.  Reuses the F1
	// TrOne / EvalMediumTransmittance helpers above (same anon ns).
	//////////////////////////////////////////////////////////////////

	/// Store path throughput into the tag's vertex field: RISEPel
	/// `throughput` for Pel, Scalar `throughputNM` for NM.
	template<class Tag>
	inline void StoreThroughput(
		BDPTVertex& v, const typename SpectralValueTraits<Tag>::value_type& beta );
	template<> inline void StoreThroughput<PelTag>( BDPTVertex& v, const RISEPel& beta ) { v.throughput = beta; }
	template<> inline void StoreThroughput<NMTag>( BDPTVertex& v, const Scalar& beta ) { v.throughputNM = beta; }

	/// Read the tag's throughput field off a vertex (RR previous-throughput
	/// magnitude).
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type VertexThroughput( const BDPTVertex& v );
	template<> inline RISEPel VertexThroughput<PelTag>( const BDPTVertex& v ) { return v.throughput; }
	template<> inline Scalar  VertexThroughput<NMTag>( const BDPTVertex& v ) { return v.throughputNM; }

	/// Read the tag's per-lobe scatter weight off a ScatteredRay: RISEPel
	/// `kray` for Pel, Scalar `krayNM` for NM.
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type KrayValue( const ScatteredRay& s );
	template<> inline RISEPel KrayValue<PelTag>( const ScatteredRay& s ) { return s.kray; }
	template<> inline Scalar  KrayValue<NMTag>( const ScatteredRay& s ) { return s.krayNM; }

	/// Contribution-gate magnitude (`<= 0` / `> NEARZERO` skips).  Pel: max
	/// channel.  NM: the BARE scalar -- deliberately NOT
	/// SpectralValueTraits::max_value (which is fabs for NM): the
	/// pre-refactor NM gates tested the signed value directly, and a fabs
	/// here would admit negative spectral values the originals dropped (the
	/// Phase 2a P2 #2 footgun).  Use SpectralValueTraits::max_value (fabs
	/// for NM) for the *absolute* magnitude needed by Russian roulette.
	template<class Tag>
	inline Scalar PositiveMagnitude( const typename SpectralValueTraits<Tag>::value_type& v );
	template<> inline Scalar PositiveMagnitude<PelTag>( const RISEPel& v ) { return ColorMath::MaxValue( v ); }
	template<> inline Scalar PositiveMagnitude<NMTag>( const Scalar& v ) { return v; }

	/// SPF scatter dispatch: Scatter (Pel) / ScatterNM(nm) (NM).
	template<class Tag>
	inline void ScatterSPF(
		const ISPF& spf, const RayIntersectionGeometric& rig, ISampler& sampler,
		ScatteredRayContainer& scattered, IORStack& iorStack, const Tag& tag );
	template<> inline void ScatterSPF<PelTag>(
		const ISPF& spf, const RayIntersectionGeometric& rig, ISampler& sampler,
		ScatteredRayContainer& scattered, IORStack& iorStack, const PelTag& )
	{ spf.Scatter( rig, sampler, scattered, iorStack ); }
	template<> inline void ScatterSPF<NMTag>(
		const ISPF& spf, const RayIntersectionGeometric& rig, ISampler& sampler,
		ScatteredRayContainer& scattered, IORStack& iorStack, const NMTag& tag )
	{ spf.ScatterNM( rig, sampler, tag.nm, scattered, iorStack ); }

	/// Wavelength argument for BSSRDFSampling::SampleEntryPoint /
	/// RandomWalkSSS::SampleExit: 0 for Pel, tag.nm for NM.
	template<class Tag> inline Scalar NmOrZero( const Tag& tag );
	template<> inline Scalar NmOrZero<PelTag>( const PelTag& ) { return Scalar( 0 ); }
	template<> inline Scalar NmOrZero<NMTag>( const NMTag& tag ) { return tag.nm; }

	/// DL-307: does this material carry a SUBSURFACE-ENTRY branch -- a
	/// diffusion profile, or random-walk parameters for this tag -- that the
	/// two subpath generators take AFTER scattering the SPF?  Mirrors exactly
	/// the material half of those branches' own entry conditions (the
	/// diffusion-profile block; the random-walk block's static params, plus
	/// the NM-only per-wavelength fallback).  The generators consult it only
	/// when no lobe was selected: the subsurface coin is independent of the
	/// SPF realization, so an empty (or unselectable) container is a ZERO
	/// sample of the reflection technique and no reason to skip the
	/// subsurface one -- exactly DL-67's ruling for the guide technique.
	/// The NM specialization's third query (`GetRandomWalkSSSParamsNM`) is
	/// dead today -- no material in src/ overrides it (2026-09-28) -- and is
	/// kept only so this predicate cannot drift from the random-walk
	/// block's own NM fallback, which still asks it.
	template<class Tag> inline bool HasSubsurfaceEntryBranch( const IMaterial& m, const Tag& tag );
	template<> inline bool HasSubsurfaceEntryBranch<PelTag>( const IMaterial& m, const PelTag& )
	{ return m.GetDiffusionProfile() != 0 || m.GetRandomWalkSSSParams() != 0; }
	template<> inline bool HasSubsurfaceEntryBranch<NMTag>( const IMaterial& m, const NMTag& tag )
	{
		if( m.GetDiffusionProfile() != 0 || m.GetRandomWalkSSSParams() != 0 ) {
			return true;
		}
		RandomWalkSSSParams rwParamsNM;
		return m.GetRandomWalkSSSParamsNM( tag.nm, rwParamsNM );
	}

	/// Medium free-flight distance sample dispatch: SampleDistance (Pel) /
	/// SampleDistanceNM(nm) (NM).
	template<class Tag>
	inline Scalar SampleMediumDistance(
		const IMedium& medium, const Ray& ray, const Scalar maxDist,
		ISampler& sampler, bool& scattered, const Tag& tag );
	template<> inline Scalar SampleMediumDistance<PelTag>(
		const IMedium& medium, const Ray& ray, const Scalar maxDist,
		ISampler& sampler, bool& scattered, const PelTag& )
	{ return medium.SampleDistance( ray, maxDist, sampler, scattered ); }
	template<> inline Scalar SampleMediumDistance<NMTag>(
		const IMedium& medium, const Ray& ray, const Scalar maxDist,
		ISampler& sampler, bool& scattered, const NMTag& tag )
	{ return medium.SampleDistanceNM( ray, maxDist, tag.nm, sampler, scattered ); }

	/// Medium free-flight scatter throughput weight + the scalar sigma_t
	/// stored on the medium vertex.  Pel uses the delta-tracking weight
	///   Tr * sigma_s / (max(sigma_t) * min(Tr))   (chromatic, RISEPel);
	/// NM the single-wavelength single-scattering albedo  sigma_s/sigma_t
	/// (equal for one wavelength, where Tr cancels).
	template<class Tag>
	inline typename SpectralValueTraits<Tag>::value_type
	ComputeMediumScatterWeight(
		const IMedium& medium, const Point3& scatterPt, const Ray& ray,
		const Scalar t_m, const Tag& tag, Scalar& outSigmaTScalar );
	template<>
	inline RISEPel ComputeMediumScatterWeight<PelTag>(
		const IMedium& medium, const Point3& scatterPt, const Ray& ray,
		const Scalar t_m, const PelTag&, Scalar& outSigmaTScalar )
	{
		const MediumCoefficients coeff = medium.GetCoefficients( scatterPt );
		const RISEPel Tr = medium.EvalTransmittance( ray, t_m );
		const Scalar sigma_t_max = ColorMath::MaxValue( coeff.sigma_t );
		outSigmaTScalar = sigma_t_max;
		RISEPel medWeight( 0, 0, 0 );
		if( sigma_t_max > 0 ) {
			const Scalar Tr_scalar = ColorMath::MinValue( Tr );
			if( Tr_scalar > 0 ) {
				medWeight = Tr * coeff.sigma_s * (1.0 / (sigma_t_max * Tr_scalar));
			}
		}
		return medWeight;
	}
	template<>
	inline Scalar ComputeMediumScatterWeight<NMTag>(
		const IMedium& medium, const Point3& scatterPt, const Ray& ray,
		const Scalar t_m, const NMTag& tag, Scalar& outSigmaTScalar )
	{
		const MediumCoefficientsNM coeff = medium.GetCoefficientsNM( scatterPt, tag.nm );
		const Scalar TrNM = medium.EvalTransmittanceNM( ray, t_m, tag.nm );
		const Scalar sigma_t_nm = coeff.sigma_t;
		outSigmaTScalar = sigma_t_nm;
		Scalar medWeightNM = 0;
		if( sigma_t_nm > 0 && TrNM > 0 ) {
			medWeightNM = coeff.sigma_s / sigma_t_nm;
		}
		return medWeightNM;
	}

	template<class Tag>
	unsigned int GenerateEyeSubpathImpl(
		unsigned int maxEyeDepth,
		const StabilityConfig& stabilityConfig,
		const LightSampler* pLightSampler,
	#ifdef RISE_ENABLE_OPENPGL
		PathGuidingField* pGuidingField,
		unsigned int maxGuidingDepth,
		Scalar guidingAlpha,
		GuidingSamplingType guidingSamplingType,
	#endif
		const RuntimeContext& rc,
		const Ray& cameraRay,
		const Point2& screenPos,
		const IScene& scene,
		const IRayCaster& caster,
		ISampler& sampler,
		std::vector<BDPTVertex>& vertices,
		std::vector<uint32_t>& subpathStarts,
		const Tag& tag,
		const SampledWavelengths* pSwlHWSS,
		PixelAOV* pPrimaryAOV )
	{
		typedef SpectralValueTraits<Tag> Traits;
		typedef typename Traits::value_type V;

		vertices.clear();
		vertices.reserve( maxEyeDepth + 1 );
		subpathStarts.clear();
		subpathStarts.push_back( 0 );

		// Snapshot once at entry so vertex 0 and the pdfCamDir below see
		// the same camera.  Structural changes serialize against
		// rendering per the IScenePriv.h contract.
		const ICamera* pCamera = scene.GetCamera();

		//
		// Vertex 0: the camera
		//
		{
			BDPTVertex v;
			v.type = BDPTVertex::CAMERA;

			// Debt 28: on a FINITE-APERTURE camera the eye ray does not
			// leave the lens centre -- `ThinLensCamera::GenerateRay`
			// already sampled a point on the aperture and put it in
			// `cameraRay.origin`.  The camera path vertex has to BE
			// that point, or the path whose MIS weight is computed is
			// not the path that was sampled (the first edge's direction
			// and dist^2 would be measured from a point the ray never
			// touched).  Identical to `GetLocation()` for pinhole and
			// fisheye; orthographic keeps `GetLocation()` because its
			// per-pixel ray origin offset is deliberately out of scope
			// here (it is a delta-DIRECTION camera and its t==1
			// strategy is skipped outright).
			if( pCamera ) {
				v.position = BDPTCameraUtilities::HasFiniteAperture( *pCamera ) ?
					cameraRay.origin : pCamera->GetLocation();
			}

			v.normal = cameraRay.Dir();
			v.onb.CreateFromW( cameraRay.Dir() );
			v.pMaterial = 0;
			v.pObject = 0;
			v.pLight = 0;
			v.pLuminary = 0;
			v.screenPos = screenPos;
			// A delta-DIRECTION camera (orthographic: all rays parallel)
			// is the importance-side analogue of a directional light.  The
			// t==1 light-tracing strategy cannot scatter a non-specular
			// light vertex into the camera's single delta direction, so it
			// has zero density and must be excluded from the MIS
			// denominator.  Marking the camera vertex delta makes
			// MISWeight's eye-side walk skip the t==1 term (the
			// `eyeVerts[j-1].isDelta` gate at j==1), giving the eye-path
			// strategies (NEE / interior) their full weight.  The matching
			// ConnectToCamera sites skip the (now phantom) splat.  Pinhole /
			// thin-lens / fisheye are finite-direction → isDelta stays
			// false → t==1 remains a valid strategy for them.
			v.isDelta = pCamera ?
				BDPTCameraUtilities::IsDeltaDirection( *pCamera ) : false;

			// pdfFwd = 1 for a specific pixel
			v.pdfFwd = 1.0;
			v.pdfRev = 0;
			StoreThroughput<Tag>( v, TrOne<Tag>() );

			vertices.push_back( v );
		}

		// Trace the camera ray
		Ray currentRay = cameraRay;
		V beta = TrOne<Tag>();

		// The camera pdf for generating this direction.  Reuses the
		// pCamera cached at the top of GenerateEyeSubpath.
		Scalar pdfCamDir = 1.0;
		if( pCamera ) {
			pdfCamDir = BDPTCameraUtilities::PdfDirection( *pCamera, cameraRay );
		}

		// VCM post-pass inputs on the camera endpoint: the
		// directional importance PDF in solid-angle measure (used by
		// InitCamera), plus a cosine sentinel of 1.0.  BDPT itself
		// does not read these (its own walk uses the local
		// `pdfCamDir` below).
		//
		// ZERO for a delta-DIRECTION camera, which is the VCM-side
		// mirror of the `isDelta` flag BDPT sets on this same vertex.
		// `InitCamera` maps a non-positive pdf to `dVCM = 0`, and
		// dVCM is precisely the MIS mass reserved for the
		// light-tracing (t==1) strategy -- which for an orthographic
		// camera does not exist, since every pixel has its own ray
		// origin and no light vertex can connect to a single camera
		// point.  SmallVCM does the same thing on the light side for
		// a delta light.  Leaving `PdfDirection`'s orthographic
		// return (1/A_image, an AREA density, not a solid-angle one)
		// in here made dVCM enormous and crushed every eye-side
		// strategy's weight: VCM rendered an orthographic scene at
		// 7.7% of PT (debt 28 review, A P2-6).
		const bool cameraIsDeltaDirection =
			pCamera && BDPTCameraUtilities::IsDeltaDirection( *pCamera );
		vertices[0].emissionPdfW = cameraIsDeltaDirection ? Scalar( 0 ) : pdfCamDir;
		vertices[0].cosAtGen = 1.0;

		Scalar pdfFwdPrev = pdfCamDir;
		IORStack iorStack( 1.0 );
		// Seed from the camera position: if the camera sits inside a
		// dielectric (submerged camera, camera inside a medium volume),
		// the first scatter needs the stack to reflect that containment.
		// For cameras in free space this is a no-op — the probe finds no
		// enclosing objects and leaves the stack as-is.
		if( pCamera ) {
			// Seed from the CAMERA VERTEX, which on a finite-aperture
			// camera is the sampled lens point rather than the lens
			// centre (debt 28) -- the walk physically starts there.
			IORStackSeeding::SeedFromPoint( iorStack, vertices[0].position, scene );
		}
		// DL-09: the camera endpoint's graded medium and index, for a t==1
		// connection's (n_camera/n_light)^2 factor.
		GradedIndexMedium::RecordVertex( iorStack, vertices[0].pGradedMedium, vertices[0].gradedIOR );

	#ifdef RISE_ENABLE_OPENPGL
		static thread_local GuidingDistributionHandle guideDist;
	#endif

		// Per-type bounce counters for StabilityConfig limits
		unsigned int eyeDiffuseBounces = 0;
		unsigned int eyeGlossyBounces = 0;
		unsigned int eyeTransmissionBounces = 0;
		unsigned int eyeTranslucentBounces = 0;
		unsigned int eyeVolumeBounces = 0;
		// Unified surface-bounce cap (DL-210): caps surface vertices to
		// maxEyeDepth on both Pel and NM walks, matching GenerateLightSubpathImpl.
		unsigned int eyeSurfaceBounces = 0;

		// HWSS per-wavelength throughput tracking (NM bundle only).  When
		// pSwlHWSS is non-null the RR site below uses max over active
		// wavelengths of the post-scatter throughput, preventing hero-driven
		// RR from amplifying companion wavelengths on rare survivors (see the
		// PathTracingIntegrator HWSS RR comment for full rationale).  Index 0
		// is hero; kept in sync with beta.  [[maybe_unused]] because the Pel
		// instantiation discards every use below.
		[[maybe_unused]] Scalar hwssBetaNM[SampledWavelengths::N] = {};
		if constexpr( Traits::is_nm ) {
			if( pSwlHWSS ) {
				for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
					hwssBetaNM[w] = Scalar( 1 );
				}
			}
		}

		// Loop limit accounts for both surface and volume bounces.
		// Saturating addition: if a scene sets max_volume_bounce to
		// UINT_MAX (documented as "unlimited" for other bounce types), the
		// raw sum wraps to a small number, silently aborting the walk.
		// Cap at 1024 which is well above any realistic depth.  Also guard
		// `maxEyeDepth >= 1024` directly so the subtraction in the first
		// half of the ternary doesn't underflow.
		// The cap is BDPTUtilities::kWalkIterationCap, which also bounds
		// the per-iteration medium-distance stream layout (DL-283).
		const unsigned int kCap = BDPTUtilities::kWalkIterationCap;
		const unsigned int maxEyeTotalDepth =
			( maxEyeDepth >= kCap ||
			  stabilityConfig.maxVolumeBounce > kCap - maxEyeDepth ) ?
				kCap :
				maxEyeDepth + stabilityConfig.maxVolumeBounce;

		for( unsigned int depth = 0; depth < maxEyeTotalDepth; depth++ )
		{
			// Per-bounce stream offset on the eye subpath.  Streams
			// [16, 16+maxEyeTotalDepth) are reserved for the eye walk;
			// light walk uses [1, ...).
			sampler.StartStream( 16u + depth );

			// Intersect the scene
			RayIntersection ri( currentRay, nullRasterizerState );
			scene.GetObjects()->IntersectRay( ri, true, true, false );
			if( depth == 0 ) CaptureBDPTPrimaryAOV( rc, ri, pPrimaryAOV );

			// ----------------------------------------------------------------
			// Participating media: free-flight distance sampling.
			//
			// Before creating a surface vertex, check if the current ray
			// travels through a participating medium.  If a scatter event
			// occurs before reaching the surface, create a MEDIUM vertex
			// and sample the phase function for the continuation direction.
			// If no scatter occurs, apply transmittance to the path
			// throughput and fall through to normal surface vertex creation.
			// ----------------------------------------------------------------
			// Declared outside the medium block so surface vertices can
			// inherit the enclosing medium info for connection transmittance.
			const IObject* pMedObj_eye = 0;
			const IMedium* pMed_eye = 0;
			{
				const IObject* pMedObj = 0;
				const IMedium* pMed = MediumTracking::GetCurrentMediumWithObject(
					iorStack, &scene, pMedObj );
				pMedObj_eye = pMedObj;
				pMed_eye = pMed;

				// G6: stamp the ambient (incident-medium) IOR so the conductor
				// Fresnel in ScatterSPF (trace-time sampling below) uses the
				// surrounding medium rather than hardcoded air.  IORStack::top()
				// here is the medium the ray was travelling through (read before
				// SetCurrentObject, which does not push).  Guard to air (1.0).
				{
					const Scalar ambIOR = iorStack.top();
					ri.geometric.ambientIOR = ( ambIOR > 0.0 ) ? ambIOR : 1.0;
				}

				if( pMed )
				{
					const Scalar maxDist = ri.geometric.bHit ? ri.geometric.range : RISE_INFINITY;
					// DL-247 RULING -- `max_volume_bounce` N truncates the
					// medium's Neumann series at N scatter vertices PER FULL
					// PATH for every integrator, every medium segment
					// carrying its true transmittance.  Past the cap this
					// subpath may not scatter, so the segment is not sampled
					// and carries its deterministic Beer-Lambert Tr (the
					// `bAtCap` branch below).  It used to be sampled anyway,
					// and a scattered-at-cap event fell into the no-scatter
					// survival branch -- weight Tr/pSurvival ~ 1, i.e. the
					// medium turned to VACUUM past the cap.  The per-path
					// half of the ruling is the `volBounces` gate in
					// EvaluateAllStrategiesImpl (and VCM's twins).
					const bool bAtCap = eyeVolumeBounces >= stabilityConfig.maxVolumeBounce;
					bool scattered = false;
					Scalar t_m = 0;
					if( !bAtCap ) {
						// DL-283: under a fixed-budget sampler (Sobol) the
						// distance sample's open-ended draw sequence gets a
						// stream block of its own, and the vertex stream is
						// re-opened at slot 0 afterwards -- see
						// BDPTUtilities::MediumDistanceStream.  PSSMLT (MLT)
						// lanes are unbounded, so nothing can spill there and
						// its chains keep their pre-DL-283 lane layout.
						const bool bOwnStream = sampler.HasFixedDimensionBudget();
						if( bOwnStream ) {
							sampler.StartStream( BDPTUtilities::MediumDistanceStream(
								BDPTUtilities::eEyeWalk, depth ) );
						}
						t_m = SampleMediumDistance<Tag>(
							*pMed, currentRay, maxDist, sampler, scattered, tag );
						if( bOwnStream ) {
							sampler.StartStream( 16u + depth );
						}
					}

					if( scattered )
					{
						// Medium scatter event before surface hit.
						//
						// Throughput weight for delta-tracking scatter event:
						//   Sampling PDF:   p(t) = sigma_t_maj * exp(-sigma_t_maj * t)
						//   Transmittance:  Tr(t) = exp(-sigma_t * t)  per channel
						//   Weight:         Tr * sigma_s / (sigma_t_maj * Tr_scalar)
						//
						// For homogeneous media this simplifies to sigma_s / sigma_t_maj
						// (single-scattering albedo times a correction for multi-channel
						// media).  Tr_scalar = MinValue(Tr) is the scalar tracking
						// transmittance from the delta-tracking majorant channel.
						const Point3 scatterPt = currentRay.PointAtLength( t_m );
						const Vector3 wo = currentRay.Dir();
						// Medium free-flight scatter throughput weight + the scalar
						// sigma_t stored on the vertex (delta-tracking RISEPel weight
						// for Pel; single-scattering albedo for NM -- equal for one
						// wavelength, where Tr cancels).
						Scalar sigmaTScalar = 0;
						const V medWeight = ComputeMediumScatterWeight<Tag>(
							*pMed, scatterPt, currentRay, t_m, tag, sigmaTScalar );

						if( PositiveMagnitude<Tag>( medWeight ) <= 0 ) {
							break;
						}

						beta = beta * medWeight;

						// Create MEDIUM vertex
						BDPTVertex mv;
						mv.type = BDPTVertex::MEDIUM;
						mv.position = scatterPt;
						mv.normal = -wo;	// incoming direction (for guiding orientation)
						mv.onb.CreateFromW( -wo );
						mv.pMaterial = 0;
						mv.pObject = 0;
						mv.pMediumVol = pMed;
						mv.pPhaseFunc = pMed->GetPhaseFunction();
						mv.pMediumObject = pMedObj;
						mv.sigma_t_scalar = sigmaTScalar;
						mv.volumeBounces = eyeVolumeBounces + 1;
						mv.isDelta = false;

						// HISTORY (DL-200 struck this rule out entirely, so the
						// paragraph that used to sit here is gone rather than
						// amended): medium vertices "enclosed by a specular
						// (delta) boundary" were marked NOT connectible, keyed
						// off the previous vertex's per-surface `isConnectible`
						// rather than its per-draw `isDelta` (the debt-23-family
						// refinement, docs/CLOTH_FABRIC_DESIGN.md §15).  That
						// refinement was right about `isDelta` vs `isConnectible`
						// and wrong about the whole rule -- see below.
							// DL-200.  `isConnectible` describes THIS VERTEX'S OWN
							// SCATTERING FUNCTION -- "can a connection through it
							// carry nonzero density" -- and a phase function is
							// never a delta, so a MEDIUM vertex is ALWAYS
							// connectible.  It used to be demoted to false when the
							// vertex sat inside an enclosure whose boundary vertex
							// was itself non-connectible ("connection rays are
							// blocked by the specular surface"), inheriting the flag
							// from `vertices.back()`.
							//
							// That was a VISIBILITY heuristic wearing the wrong hat,
							// and it was wrong in three separate ways.
							//
							// (1) IT IS NOT A PROPERTY OF THE VERTEX.  Visibility is
							// a property of the PAIR (this vertex, the other
							// endpoint): two points inside the same enclosure are
							// perfectly connectible, and only a connection LEAVING
							// the enclosure is blocked.  A light inside a glass shell
							// filled with fog is exactly that case, and NEE from an
							// interior medium vertex to it was being dropped.
							//
							// (2) IT WAS ASYMMETRIC BETWEEN THE TWO WALKS, because it
							// read `vertices.back()`: a LIGHT-rooted predecessor
							// (type LIGHT) never fails the SURFACE/MEDIUM type check,
							// so a light emitting straight into an enclosed medium
							// left that vertex connectible, while an EYE walk that
							// crossed the same boundary to a geometrically coincident
							// point read false.  Instrumented on a
							// dielectric-shell-plus-interior-medium scene: 6816304
							// light-rooted enclosed medium vertices, 4683400 of them
							// connectible, against 2197533 eye-rooted ones, 0
							// connectible.
							//
							// (3) SURFACE VERTICES WERE NEVER TREATED THIS WAY.  A
							// diffuse surface inside the same shell reads
							// `isConnectible = (GetBSDF() != 0)` = true regardless of
							// any enclosure, and PT's own volume NEE connects from
							// interior medium vertices through the boundary using the
							// same transparent-shadow transmittance BDPT would.  Only
							// MEDIUM vertices carried the extra demotion, so only they
							// disagreed with the PT reference.
							//
							// The demotion became actively harmful when DL-126 made
							// `MISWeight` SKIP a strategy whose endpoint is
							// `!isConnectible`: a flag that depends on which walk
							// created the vertex then feeds the MIS denominator.
							//
							// MEASURED on that scene (48x48, mean of 4 renders, PT as
							// the reference): BDPT/PT 0.90338 -> 1.00356 (RGB) and
							// 0.79556 -> 0.98217 (spectral), with BDPT's own
							// run-to-run sd falling 2.94% -> 0.37% -- the dropped
							// eye-side NEE was a variance cost as well as a bias.
							// The alternative rule of deriving the flag from the
							// ENCLOSURE BOUNDARY'S material symmetrically (false on
							// both sides for a delta boundary) was implemented and
							// measured too: it leaves the image BIT-IDENTICAL to the
							// pre-fix baseline (0.033181734 / 0.030185761 on the same
							// four seeds), i.e. the light-side connections it removes
							// contribute exactly nothing, so it only deletes wasted
							// shadow rays and closes none of the gap.
							//
							// Cost: connections and NEE are now ATTEMPTED from medium
							// vertices inside a delta enclosure.  Where the boundary
							// is opaque those shadow rays fail and contribute 0 (the
							// ordinary, correct BDPT outcome); where it is a
							// dielectric they pass with RISE's transparent-shadow
							// transmittance, which is the same approximation PT's own
							// volume NEE already makes -- which is why BDPT now lands
							// on PT rather than past it.
							mv.isConnectible = true;
						StoreThroughput<Tag>( mv, beta );

						// PDF in generalized area measure for a medium scatter vertex.
						// For surface vertices: pdfArea = pdfDir * |cos(theta)| / dist^2
						//   (Veach thesis eq. 8.8)
						// For medium vertices: pdfArea = pdfDir * sigma_t / dist^2
						//   (Veach thesis Chapter 11; PBRT v4 Section 16.3)
						// sigma_t at the scatter point replaces |cos| because the
						// free-flight sampling probability density is proportional
						// to sigma_t, while surface "acceptance" is proportional to
						// the projected area (|cos|/dist^2).
						const Scalar distSqMed = t_m * t_m;
						mv.pdfFwd = BDPTUtilities::SolidAngleToAreaMedium(
							pdfFwdPrev, mv.sigma_t_scalar, distSqMed );
						mv.pdfRev = 0;

						// VCM post-pass uses sigma_t_scalar for the
						// area-to-solid-angle inversion at medium vertices.
						mv.cosAtGen = 0;

	#ifdef RISE_ENABLE_OPENPGL
						if constexpr( Traits::is_pel ) {
							mv.guidingHasSegment = true;
							mv.guidingDirectionOut = -wo;
							mv.guidingNormal = -wo;
							mv.guidingEta = 1.0;
						}
	#endif

						// DL-09: a medium vertex does not Advance (the next
						// surface vertex's Advance telescopes over it); it
						// records the stack top its throughput was priced to.
						GradedIndexMedium::RecordVertex( iorStack, mv.pGradedMedium, mv.gradedIOR );
						vertices.push_back( mv );

						// Sample the phase function for continuation direction
						const IPhaseFunction* pPhase = pMed->GetPhaseFunction();
						if( !pPhase ) {
							break;
						}

						const Vector3 wi = pPhase->Sample( wo, sampler );
						const Scalar phasePdf = pPhase->Pdf( wo, wi );
						if( phasePdf <= NEARZERO ) {
							break;
						}

						// Phase function throughput: value / pdf.
						// For isotropic: p = 1/(4pi), pdf = 1/(4pi), so weight = 1.
						// For HG: value and pdf are identical (self-normalized), weight = 1.
						const Scalar phaseVal = pPhase->Evaluate( wo, wi );
						beta = beta * (phaseVal / phasePdf);

						// Russian roulette for volume scattering
						const PathTransportUtilities::RussianRouletteResult rr =
							PathTransportUtilities::EvaluateRussianRoulette(
								depth + eyeVolumeBounces,
								stabilityConfig.rrMinDepth,
								stabilityConfig.rrThreshold,
								Traits::max_value( beta ),
								Traits::max_value( VertexThroughput<Tag>( mv ) ),
								sampler.Get1D() );
						if( rr.terminate ) {
							break;
						}
						if( rr.survivalProb < 1.0 ) {
							beta = beta * (1.0 / rr.survivalProb);
						}

	#ifdef RISE_ENABLE_OPENPGL
						if constexpr( Traits::is_pel ) {
							vertices.back().guidingHasDirectionIn = true;
							vertices.back().guidingDirectionIn = wi;
							vertices.back().guidingPdfDirectionIn = phasePdf;
							vertices.back().guidingScatteringWeight =
								RISEPel( phaseVal / phasePdf, phaseVal / phasePdf,
									phaseVal / phasePdf );
							vertices.back().guidingRussianRouletteSurvivalProbability = rr.survivalProb;
							vertices.back().guidingRoughness = 1.0;
						}
	#endif

						// Update pdfRev on the previous vertex.
						// Phase functions are symmetric: Pdf(wo, wi) == Pdf(wi, wo),
						// so reverse pdf == forward pdf.
						if( vertices.size() >= 2 ) {
							BDPTVertex& prev = vertices[ vertices.size() - 2 ];
							const Scalar revPdfSA = phasePdf;

							if( prev.type == BDPTVertex::MEDIUM ) {
								prev.pdfRev = BDPTUtilities::SolidAngleToAreaMedium(
									revPdfSA, prev.sigma_t_scalar, distSqMed );
							} else if( prev.type == BDPTVertex::CAMERA ) {
								prev.pdfRev = BDPTUtilities::SolidAngleToArea(
									revPdfSA, Scalar(1.0), distSqMed );
							} else {
								const Scalar absCosAtPrev = fabs( Vector3Ops::Dot(
									prev.geomNormal, currentRay.Dir() ) );
								prev.pdfRev = BDPTUtilities::SolidAngleToArea(
									revPdfSA, absCosAtPrev, distSqMed );
							}
						}

						// Store forward pdf for the next vertex
						pdfFwdPrev = phasePdf;

						// Advance ray
						currentRay = Ray( scatterPt, wi );
						currentRay.Advance( BDPT_RAY_EPSILON );

						eyeVolumeBounces++;
						continue;
					}
					else if( bAtCap )
					{
						beta = beta * EvalMediumTransmittance<Tag>( *pMed, currentRay, maxDist, tag );
					}
					else if( ri.geometric.bHit )
					{
						// No-scatter SURVIVAL: the eye subpath reached the
						// surface WITHOUT a medium scatter event.  Under the
						// analog estimator that "no scatter" outcome is itself
						// a stochastic survival whose probability already
						// carries Beer-Lambert (via the analog survival pdf);
						// multiplying beta by the full Tr again would
						// double-count.  Apply the survival weight
						// Tr / pSurvival (deterministic no-scatter survival pdf;
						// = 1 for monochrome/NM homogeneous, correct for hetero).
						const V Tr = EvalMediumTransmittance<Tag>( *pMed, currentRay, ri.geometric.range, tag );
						const Scalar pSurvival = EvalNoScatterSurvivalPdf<Tag>( *pMed, currentRay, ri.geometric.range, tag );
						beta = beta * BDPTSurvivalWeight( Tr, pSurvival );
					}
				}
			}

			if( !ri.geometric.bHit ) {
				// Path B: eye-subpath escape to environment.  Without this
				// branch, BDPT misses the s=0 contribution of camera rays
				// that escape the scene through env-IBL — every env-only
				// scene renders with a ~95% deficit vs PT (regression test:
				// EnvLightBalanceTest).  Push a synthetic env-light vertex
				// at the escape point so the standard s=0 strategy in
				// ConnectBDPT (which detects pEnvLight != NULL and routes
				// to env-radiance lookup) can credit the env contribution.
				//
				// On the FIRST iteration (camera ray miss) we still honour
				// the rasterizer's `radiance_background` flag — when off,
				// the rasterizer's caller-side accumulator already discards
				// background pixels, but for the BDPT s=0 path we must
				// also gate here so the synthetic env vertex doesn't drive
				// indirect-MIS strategies on a backdrop the user wanted
				// suppressed.  Indirect bounces (depth > 0) always credit
				// env (matches PT IntegrateFromHit behaviour at line ~2641).
				const IRadianceMap* pEnvForEscape = scene.GetGlobalRadianceMap();
				const bool bIsFirstBounce = ( depth == 0 );
				// Place the synthetic env vertex at the ray-sphere exit
				// point along currentRay.  Previous code used
				//   position = sceneCentre + ray.Dir * sceneRadius
				// which is on the scene-center line in the ray.Dir
				// direction — only matches the actual ray when the ray
				// origin lies at the scene center.  Downstream s=0
				// dispatcher then computes
				//   wiSky = (eyeEnd.position - eyePred.position).norm
				// which derives the WRONG direction for off-center
				// predecessors (adversarial review P1b).  Fix: place the
				// vertex on the ray (`ray.origin + ray.Dir * tExit`) so
				// `(eyeEnd.position - eyePred.position).norm` recovers
				// the actual ray direction whenever the predecessor lies
				// on the same ray (always true here, since eyePred is the
				// vertex the ray was scattered from).  The s=0 dispatcher
				// additionally reads `-eyeEnd.geomNormal` as the canonical
				// stored ray direction.
				//
				// tExit solves |origin + tExit*d - sceneCentre|² = r²,
				// taking the far root:
				//   b = d · (origin - sceneCentre)
				//   c = |origin - sceneCentre|² - r²
				//   tExit = -b + sqrt(b² - c)
				// For a ray escaping the scene the discriminant is always
				// positive and tExit > 0.
				const Scalar sceneRadius = pLightSampler ?
					pLightSampler->GetCachedSceneRadius() : Scalar( 0 );
				if( pEnvForEscape && sceneRadius > 0 &&
					( !bIsFirstBounce ||
					  caster.IsRadianceMapVisibleAsBackground() ) ) {
					BDPTVertex vEnv;
					vEnv.type = BDPTVertex::LIGHT;
					// DL-247: the escape adds no scatter vertex, so the
					// path's count is the one carried to here.  An unset
					// field (0) would let the s == 0 strategy through the
					// per-path `volBounces` gate while the s == 1 env-NEE
					// strategy for the SAME path, ending at the previous
					// vertex, is excluded -- a partition break once the
					// cap binds.
					vEnv.volumeBounces = eyeVolumeBounces;
					const Point3 sceneCentre =
						pLightSampler->GetCachedSceneCenter();
					// Ray-sphere intersection.  Let e = origin - center;
					// solve |e + t*d|² = r² for the far root:
					//   t² + 2 t (d·e) + (|e|² - r²) = 0
					//   t = -(d·e) + sqrt((d·e)² - |e|² + r²)
					// Earlier code computed `mkVector3(sceneCentre, origin)`
					// which returns `center - origin = -e`, then used the
					// far-root formula with -b — yielding `+(d·e) + sqrt`,
					// which OVERSHOOTS the far exit on off-center origins
					// (the synthetic env vertex landed past the bounding
					// sphere, contaminating distSq and breaking the
					// MIS-bookkeeping numerics).  Adversarial review P2,
					// 2026-05-25.  Fix: build e = origin - center directly.
					const Vector3 e = Vector3Ops::mkVector3(
						currentRay.origin, sceneCentre );
					const Vector3 rayD = currentRay.Dir();
					const Scalar b = Vector3Ops::Dot( rayD, e );
					const Scalar cSq = Vector3Ops::SquaredModulus( e );
					const Scalar disc = b * b - ( cSq - sceneRadius * sceneRadius );
					// Clamp to non-negative — origin-inside-sphere guarantees
					// real root, but guard against FP noise on degenerate
					// scenes where the ray origin sits exactly on the sphere.
					const Scalar tExit = -b + std::sqrt( std::max( Scalar( 0 ), disc ) );
					vEnv.position = Point3(
						currentRay.origin.x + rayD.x * tExit,
						currentRay.origin.y + rayD.y * tExit,
						currentRay.origin.z + rayD.z * tExit );
					// Store the actual ray direction in geomNormal (negated
					// so geomNormal points back into the scene, matching
					// the disc convention SampleEnvLightEmission uses).
					// Downstream s=0 dispatcher reads `-eyeEnd.geomNormal`
					// as the canonical sky direction.
					vEnv.normal = Vector3( -rayD.x, -rayD.y, -rayD.z );
					vEnv.geomNormal = vEnv.normal;
					vEnv.onb.CreateFromW( vEnv.normal );
					vEnv.pMaterial = 0;
					vEnv.pObject = 0;
					vEnv.pLight = 0;
					vEnv.pLuminary = 0;
					vEnv.pEnvLight = pEnvForEscape;
					vEnv.isDelta = false;
					vEnv.isConnectible = true;
					// Eye-side pdfFwd at env vertex: pdfFwdPrev (SA on
					// previous vertex) converted to area at env.  cosAtEnv
					// = 1 by construction (geomNormal = -rayD, incoming =
					// rayD).  distSq uses the actual eye→exit distance.
					const Scalar distSqToExit = tExit * tExit;
					vEnv.pdfFwd = BDPTUtilities::SolidAngleToArea(
						pdfFwdPrev, Scalar( 1.0 ), distSqToExit );
					// Apply the residual medium attenuation along the escape
					// segment before the synthetic env vertex stores beta.
					// This is a no-scatter SURVIVAL escape: the eye ray left the
					// scene through the medium WITHOUT a scatter event, so under
					// the analog estimator its survival probability already
					// carries Beer-Lambert.  Apply the survival weight Tr /
					// pSurvival (deterministic no-scatter survival pdf; = 1 for
					// monochrome/NM homogeneous) rather than the full Tr, mirroring
					// above and the PT escape fix (PTSurvivalWeight).  maxDist =
					// RISE_INFINITY matches PT and the env-NEE convention (see
					// ConnectAndEvaluate s=1 comment ~line 3520) exactly so a global
					// medium evaporates identically across integrators: for an
					// UNBOUNDED medium with tiny sigma_t a finite cap like 1e10
					// leaves exp(-sigma_t*1e10) ~ 1 (env leaks through where it must
					// be fully attenuated), whereas exp(-sigma_t*inf) -> 0 for any
					// sigma_t > 0.  A bounded (AABB) medium clips internally to a
					// finite Tr regardless of the cap.  VCM's s=0 env-escape consumes
					// this same throughput via the shared generator, so this is the
					// single fix point for PT-reference / BDPT / VCM s=0 consistency.
					V betaEsc = beta;
					if( pMed_eye ) {
						const V Tr = EvalMediumTransmittance<Tag>( *pMed_eye, currentRay, RISE_INFINITY, tag );
						const Scalar pSurvival = EvalNoScatterSurvivalPdf<Tag>( *pMed_eye, currentRay, RISE_INFINITY, tag );
						betaEsc = betaEsc * BDPTSurvivalWeight( Tr, pSurvival );
					}
					StoreThroughput<Tag>( vEnv, betaEsc );
					if constexpr( Traits::is_nm ) {
						// The spectral env vertex ALSO broadcasts throughputNM into
						// the RGB throughput field (the NM original does this; the
						// Pel original does not).  Preserved Pel/NM asymmetry.
						vEnv.throughput = RISEPel( betaEsc, betaEsc, betaEsc );
					}
					vEnv.pdfRev = 0;
					vEnv.cosAtGen = 1.0;
					vertices.push_back( vEnv );
				}
				break;
			}

			// Check surface depth limit (medium scatters don't count).
			// Unified surface-bounce cap (DL-210): bounds SURFACE vertices to
			// maxEyeDepth on both Pel and NM walks, matching GenerateLightSubpathImpl.
			if( eyeSurfaceBounces >= maxEyeDepth ) {
				break;
			}
			eyeSurfaceBounces++;

			// Apply intersection modifier if present
			if( ri.pModifier ) {
				ri.pModifier->Modify( ri.geometric );
			}

			// DL-09: the interior-segment basic-radiance factor for the
			// segment that just ended here, RADIANCE walk: (n_start/n_here)^2,
			// top <- n_here.  The stack top then IS the index the exit
			// crossing's RadianceEtaScale reads and the index the vertex
			// records for connections.  No-op unless the innermost medium is
			// a graded field (docs/DL09_GRADED_INDEX_INTERIOR_FACTOR.md).
			{
				Scalar gradedScale;
				if( GradedIndexMedium::Advance( iorStack, ri.geometric.ptIntersection,
						GradedIndexMedium::eRadiance, gradedScale ) )
				{
					if( gradedScale != Scalar( 1 ) ) {
						beta = beta * gradedScale;
						if constexpr( Traits::is_nm ) {
							if( pSwlHWSS ) {
								for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
									hwssBetaNM[w] *= gradedScale;
								}
							}
						}
					}
					// The medium the ray was travelling through is now read
					// at the hit, not at the segment's start.
					ri.geometric.ambientIOR = iorStack.top();
				}
			}

			// Create a new surface vertex
			BDPTVertex v;
			v.type = BDPTVertex::SURFACE;
			v.position = ri.geometric.ptIntersection;
			v.scatterIncomingDistance = ri.geometric.range;
			v.normal = ri.geometric.vNormal;
			v.geomNormal = ri.geometric.vGeomNormal;
			v.onb = ri.geometric.onb;
			v.ptCoord = ri.geometric.ptCoord;
			v.ptCoord1 = ri.geometric.ptCoord1;
			v.bHasTexCoord1 = ri.geometric.bHasTexCoord1;
			v.ptObjIntersec = ri.geometric.ptObjIntersec;
			// Per-vertex colour is geometry-interpolated surface state — the
			// coloured-mesh intersection path sets ri.geometric.vColor on the
			// RGB AND the spectral render alike — and PopulateRIGFromVertex
			// replays it into the reconstructed RayIntersectionGeometric at
			// connection time so VertexColorPainter::GetColorNM sees the true
			// per-vertex colour on the NM path too, not the white fallback.
			// Unconditional for both Pel and NM, mirroring GenerateLightSubpath
			// {,NM}.  (Pel-only through Phase 2c F2a — that gate was a latent
			// spectral-BDPT / VCM-spectral vertex-colour bug; see
			// docs/PRE_PHASE1_STATUS.md "Phase 2c F2a outcome".)
			v.vColor = ri.geometric.vColor;
			v.bHasVertexColor = ri.geometric.bHasVertexColor;
			// SHADING-INPUT STATE — the three fields a PAINTER reads and
			// the BSDF-geometry block above does not carry: `derivatives`
			// (the expression VM's `curv` / `curvR`), `signals` (its
			// `occlusion` / `thickness` / `convexity` own-surface half AND
			// its `proximity` / `interior` cross-object half) and
			// `txFootprint` (its `fw` / `fwo`).  Copied HERE, after
			// `ri.pModifier->Modify` and beside `vColor`, because this
			// walk samples the continuation direction against this very
			// record and then re-prices that sample — plus every
			// connection, every MIS reverse pdf and every VCM merge at
			// this vertex — through the record
			// `PathVertexEval::PopulateRIGFromVertex` rebuilds from the
			// vertex.  Sampling with the live material and weighting with
			// the neutral one is a bias, not a flat mask
			// (docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §1).
			//
			// `signals` in particular carries the object manager's
			// `pScene` / `pSelf` / `ptWorld` stamp FORWARD, one hop: the
			// source is the record `ObjectManager::IntersectRay` stamped,
			// so nothing here can put a default-constructed channel over a
			// stamp.  That is the argument that keeps this file inside
			// SourceHygieneTest's closed writer set (design §3.1).
			v.derivatives = ri.geometric.derivatives;
			v.signals = ri.geometric.signals;
			v.txFootprint = ri.geometric.txFootprint;
			v.pMaterial = ri.pMaterial;
			v.pObject = ri.pObject;
			v.pLight = 0;
			v.pLuminary = 0;
			// Store enclosing medium so connection transmittance can
			// seed its boundary walk from the correct starting medium.
			v.pMediumObject = pMedObj_eye;
			v.pMediumVol = pMed_eye;
			v.volumeBounces = eyeVolumeBounces;
			if( ri.pObject ) {
				iorStack.SetCurrentObject( ri.pObject );
				v.mediumIOR = iorStack.top();
				v.insideObject = iorStack.containsCurrent();
			}
			GradedIndexMedium::RecordVertex( iorStack, v.pGradedMedium, v.gradedIOR );

			// Convert pdfFwdPrev from solid angle to area measure
			const Scalar distSq = ri.geometric.range * ri.geometric.range;
			// Solid-angle <-> area Jacobian uses the GEOMETRIC normal —
			// the area-element parameterisation depends on the actual face
			// orientation, not the Phong-perturbed shading normal
			// (Veach 1997 §8.2.2 / PBRT 4e §13.6.4).  Using shading here
			// biases every interior path-pdf factor and therefore the MIS
			// balance heuristic.
			const Scalar absCosIn = fabs( Vector3Ops::Dot(
				ri.geometric.vGeomNormal,
				-currentRay.Dir() ) );

			v.pdfFwd = BDPTUtilities::SolidAngleToArea( pdfFwdPrev, absCosIn, distSq );
			StoreThroughput<Tag>( v, beta );
			v.pdfRev = 0;
			v.isDelta = false;

			// VCM post-pass input: the receiving-side cosine used to
			// invert the area-measure pdfFwd back to solid angle at
			// merge/connection time.
			v.cosAtGen = absCosIn;
	#ifdef RISE_ENABLE_OPENPGL
			if constexpr( Traits::is_pel ) {
				v.guidingHasSegment = true;
				v.guidingDirectionOut = -currentRay.Dir();
				v.guidingNormal = GuidingCosineNormal( v.normal, currentRay.Dir() );
				v.guidingEta = v.mediumIOR > NEARZERO ? v.mediumIOR : 1.0;
			}
	#endif

			if( !ri.pMaterial ) {
				vertices.push_back( v );
				break;
			}

			const ISPF* pSPF = ri.pMaterial->GetSPF();
			if( !pSPF ) {
				CaptureBDPTAccurateAOV( rc, ri, pPrimaryAOV );
				vertices.push_back( v );
				break;
			}

			vertices.push_back( v );

			//
			// Sample the SPF for the next direction
			//
			ScatteredRayContainer scattered;
			ScatterSPF<Tag>( *pSPF, ri.geometric, sampler, scattered, iorStack, tag );

			// DL-67: whether this vertex is guided is decided from the
			// VERTEX (field, depth, alpha, the material having a BSDF),
			// never from the Scatter realization -- see
			// `BDPTGuidedContinuation`.  At a guided vertex an empty or
			// unselectable container is a zero sample of the BSDF
			// technique, not a reason to skip the guide technique.
	#ifdef RISE_ENABLE_OPENPGL
			const bool guidedVertex = pGuidingField && pGuidingField->IsTrained() &&
				depth < maxGuidingDepth && guidingAlpha > NEARZERO &&
				ri.pMaterial->GetBSDF() != 0;
	#else
			const bool guidedVertex = false;
	#endif
			// DL-307: an empty container is a zero sample of the SPF's
			// reflection technique, not the end of the walk at a vertex
			// whose material also has a subsurface-entry branch below --
			// that branch's Fresnel coin is independent of the Scatter
			// realization (PT takes it BEFORE scattering at all).  Breaking
			// here dropped the whole subsurface contribution of every rough
			// SSS hit whose reflection draw fell below the horizon.
			// Queried lazily: only a vertex with no lobe pays the two (NM:
			// three) virtual material calls.
			bool subsurfaceCarrier = false;
			if( scattered.Count() == 0 && !guidedVertex ) {
				subsurfaceCarrier = HasSubsurfaceEntryBranch<Tag>( *ri.pMaterial, tag );
				if( !subsurfaceCarrier ) {
					CaptureBDPTAccurateAOV( rc, ri, pPrimaryAOV );
					break;
				}
			}

			// Stochastic single-lobe selection (no path-tree branching).
			// Consume one sampler dimension for Sobol alignment.
			const Scalar lobeSelectXi = sampler.Get1D();
			const ScatteredRay* pScat = 0;
			Scalar selectProb = 1.0;

			if( scattered.Count() > 0 ) {
				pScat = scattered.RandomlySelect( lobeSelectXi, Traits::is_nm, &selectProb );
			}
			// A guided vertex with no selectable lobe continues on a
			// placeholder (non-delta, zero weight): only a guide draw can
			// carry it on (`BDPTGuidedContinuation` terminates the kept
			// technique for it).  `guideTemplateRay` is the placeholder a
			// guide draw continues on either way.  DL-307: an un-guided
			// subsurface vertex with no selectable lobe rides the same
			// placeholder into the subsurface branch, and terminates right
			// after it if that branch does not continue the walk.
			ScatteredRay guideTemplateRay;
			guideTemplateRay.type = ScatteredRay::eRayDiffuse;
			guideTemplateRay.isDelta = false;
			guideTemplateRay.ray = Ray( ri.geometric.ptIntersection, ri.geometric.vNormal );
			const bool hasLobe = ( pScat != 0 );
			if( !pScat ) {
				if( !guidedVertex && !subsurfaceCarrier ) {
					// A non-empty container none of whose lobes is
					// selectable (every weight zero) is the same zero
					// sample of the reflection technique.
					subsurfaceCarrier = HasSubsurfaceEntryBranch<Tag>( *ri.pMaterial, tag );
					if( !subsurfaceCarrier ) {
						break;
					}
				}
				pScat = &guideTemplateRay;
				selectProb = 1.0;
			}

			// Connectibility is a property of the SURFACE, not of the one
			// continuation drawn: connectible iff the material has a BSDF.
			// RISE's pure-delta materials (PerfectReflector, PerfectRefractor,
			// Dielectric) return NULL here and mark every lobe delta; a mixed
			// material (weave_material `transmission thin` with gap > 0, a
			// coated/polished/composite over a continuum base) has a BSDF even
			// on the draws that picked its delta lobe.  Deriving this from
			// `scattered[i].isDelta` dropped NEE on a gap fraction of hits --
			// (1-gap)^2 vs PT's (1-gap) -- docs/CLOTH_FABRIC_DESIGN.md 15 debt
			// 23.  Deliberately NOT `|| any non-delta draw`: a null-BSDF vertex
			// evaluates every connection to 0 (PathVertexEval.h), so marking it
			// connectible would reserve MIS mass for zero-yield strategies.
			vertices.back().isConnectible = ( ri.pMaterial->GetBSDF() != 0 );

			// Mark the current vertex as delta
			vertices.back().isDelta = pScat->isDelta;
			if( !pScat->isDelta ) {
				CaptureBDPTAccurateAOV( rc, ri, pPrimaryAOV );
			}

			// --- BSSRDF sampling for materials with diffusion profiles ---
			Scalar bssrdfReflectCompensation = 1.0;
			if( ri.pMaterial && ri.pMaterial->GetDiffusionProfile() )
			{
				// Front-face gate uses GEOMETRIC; Fresnel cosine uses SHADING.
				// PBRT 4e §10.1.1 (front/back is geometric); §11.4.2 (BSSRDF
				// Fresnel angular dependence is shading-frame).
				const Vector3 wo_bss = -currentRay.Dir();
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
				const Scalar cosInGeom = ri.geometric.BSSRDFEntryFacing( wo_bss );
				// Fresnel cosine clamped via fabs+NEARZERO — see PT site for
				// rationale.  Replaces fallback-to-cosInGeom (discontinuous Ft).
				const Scalar cosInShade = Vector3Ops::Dot( ri.geometric.vNormal, wo_bss );
				const Scalar cosIn = r_max( fabs( cosInShade ), Scalar( NEARZERO ) );
				if( cosInGeom > NEARZERO )
				{
					ISubSurfaceDiffusionProfile* pProfile = ri.pMaterial->GetDiffusionProfile();
					const Scalar Ft = pProfile->FresnelTransmission( cosIn, ri.geometric );
					const Scalar R = 1.0 - Ft;

					if( Ft > NEARZERO && sampler.Get1D() < Ft )
					{
						BSSRDFSampling::SampleResult bssrdf = BSSRDFSampling::SampleEntryPoint(
							ri.geometric, ri.pObject, ri.pMaterial, sampler, NmOrZero<Tag>( tag ) );

							if( bssrdf.valid )
							{
								CaptureBDPTAccurateAOV( rc, ri, pPrimaryAOV );
								vertices.back().isDelta = true;
							V betaSpatial;
							if constexpr( Traits::is_pel ) {
								betaSpatial = beta * bssrdf.weightSpatial * (1.0 / Ft);
								beta = beta * bssrdf.weight * (1.0 / Ft);
							} else {
								betaSpatial = beta * bssrdf.weightSpatialNM / Ft;
								beta = beta * bssrdf.weightNM / Ft;
							}

							BDPTVertex entryV;
							entryV.type = BDPTVertex::SURFACE;
							// DL-247: the SSS exit adds no medium-scatter vertex; carry the
							// subpath's count, or the per-path `volBounces` gate under-counts
							// every strategy ending here.
							entryV.volumeBounces = eyeVolumeBounces;
							entryV.position = bssrdf.entryPoint;
							entryV.normal = bssrdf.entryNormal;
							entryV.geomNormal = bssrdf.entryGeomNormal;
							entryV.onb = bssrdf.entryONB;
							// DL-22: forward live derivatives, signals, texture coordinates,
							// and vertex color from BSSRDFSampling::SampleResult.
							entryV.derivatives = bssrdf.derivatives;
							entryV.signals = bssrdf.signals;
							entryV.txFootprint = bssrdf.txFootprint;
							entryV.ptCoord = bssrdf.ptCoord;
							entryV.ptCoord1 = bssrdf.ptCoord1;
							entryV.bHasTexCoord1 = bssrdf.bHasTexCoord1;
							entryV.ptObjIntersec = bssrdf.ptObjIntersec;
							entryV.vColor = bssrdf.vColor;
							entryV.bHasVertexColor = bssrdf.bHasVertexColor;
							entryV.pMaterial = ri.pMaterial;
							entryV.pObject = ri.pObject;
							entryV.pMediumObject = pMedObj_eye;
							entryV.pMediumVol = pMed_eye;
							// DL-49: the entry point shares the exit hit's exterior medium
							// (the continuation carries the same IOR stack).  Without this
							// the vertex kept the 1.0 default and every re-evaluation of its
							// Sw (PathVertexEval, the zero-exitance sweep) priced an air
							// interface.
							entryV.mediumIOR = vertices.back().mediumIOR;
							entryV.isDelta = false;
							entryV.isConnectible = true;
							entryV.isBSSRDFEntry = true;
							StoreThroughput<Tag>( entryV, betaSpatial );
							entryV.pdfFwd = bssrdf.pdfSurface;
							entryV.pdfRev = 0;
	#ifdef RISE_ENABLE_OPENPGL
							if constexpr( Traits::is_pel ) {
								entryV.guidingHasSegment = true;
								entryV.guidingDirectionOut = -bssrdf.scatteredRay.Dir();
								entryV.guidingNormal = entryV.normal;
								entryV.guidingEta = 1.0;
							}
	#endif
							// DL-09: the BSSRDF entry vertex records the tracked index its
							// throughput was priced to (the subsurface event is not a
							// straight segment, so no factor is paid across it).
							GradedIndexMedium::RecordVertex( iorStack, entryV.pGradedMedium, entryV.gradedIOR );
							vertices.push_back( entryV );

							pdfFwdPrev = bssrdf.cosinePdf;
							currentRay = bssrdf.scatteredRay;
							continue;
						}
						break;
					}
					if( R > NEARZERO ) {
						bssrdfReflectCompensation = 1.0 / R;
					}
				}
			}
			// --- Random-walk SSS (eye subpath) ---
			else if( ri.pMaterial )
			{
				// Param resolution + front-face cosine differ Pel/NM and are
				// preserved exactly: Pel gates on the static params with a
				// geometric front-face check + clamped shading Fresnel cosine;
				// NM resolves static-or-spectral params and uses the raw
				// shading cosine.
				const RandomWalkSSSParams* pRW = nullptr;
				[[maybe_unused]] RandomWalkSSSParams rwParamsNM;
				Scalar cosIn = 0;
				bool rwGate = false;
				if constexpr( Traits::is_pel ) {
					if( ri.pMaterial->GetRandomWalkSSSParams() ) {
						const Vector3 wo_bss = -currentRay.Dir();
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
						const Scalar cosInGeom = ri.geometric.BSSRDFEntryFacing( wo_bss );
						// Fresnel cosine clamped via fabs+NEARZERO -- see PT site.
						const Scalar cosInShade = Vector3Ops::Dot( ri.geometric.vNormal, wo_bss );
						cosIn = r_max( fabs( cosInShade ), Scalar( NEARZERO ) );
						if( cosInGeom > NEARZERO ) {
							pRW = ri.pMaterial->GetRandomWalkSSSParams();
							rwGate = true;
						}
					}
				} else {
					pRW = ri.pMaterial->GetRandomWalkSSSParams();
					if( !pRW && ri.pMaterial->GetRandomWalkSSSParamsNM( tag.nm, rwParamsNM ) ) {
						pRW = &rwParamsNM;
					}
					cosIn = pRW ? Vector3Ops::Dot(
						ri.geometric.vNormal, -currentRay.Dir() ) : 0;
					if( pRW && cosIn > NEARZERO ) {
						rwGate = true;
					}
				}
				if( rwGate )
				{
					// DL-49: the RELATIVE index -- the material over the
					// exterior the ray arrived through (`ambientIOR`, the
					// IOR-stack top), the same interface the SPF reflection this
					// coin competes with, and RandomWalkSSS's refraction, use.  A
					// denser exterior past its critical angle is totally reflected.
					// DL-306: the exact dielectric law, the SPF reflection's own,
					// so the coin's reflect branch (weight R_spf / R) carries
					// exactly 1 and the two branches partition the interface.
					const Scalar etaRW = BSSRDFSampling::RelativeBoundaryIOR(
						pRW->ior, BSSRDFSampling::ExteriorIOR( ri.geometric ) );
					const Scalar Ft = BSSRDFSampling::BoundaryTransmission( cosIn, etaRW );
					const Scalar R = 1.0 - Ft;

					if( Ft > NEARZERO && sampler.Get1D() < Ft )
					{
						// See RGB light subpath comment for rationale.
						IndependentSampler walkSampler( rc.random );
						ISampler& rwSampler = sampler.HasFixedDimensionBudget()
							? static_cast<ISampler&>(walkSampler) : sampler;

						BSSRDFSampling::SampleResult bssrdf = RandomWalkSSS::SampleExit(
							ri.geometric, ri.pObject,
							pRW->sigma_a, pRW->sigma_s, pRW->sigma_t,
							pRW->g, pRW->ior, pRW->maxBounces, rwSampler, NmOrZero<Tag>( tag ), pRW->maxDepth );

							if( bssrdf.valid )
							{
								CaptureBDPTAccurateAOV( rc, ri, pPrimaryAOV );
								vertices.back().isDelta = true;

							// SampleExit does NOT include Ft(entry).
							// Coin flip: weight * Ft / Ft = weight.
							// Apply boundary filter (e.g. melanin double-pass).
							const Scalar bf = pRW->boundaryFilter;
							V betaSpatial;
							if constexpr( Traits::is_pel ) {
								betaSpatial = beta * bssrdf.weightSpatial * bf;
								beta = beta * bssrdf.weight * bf;
							} else {
								betaSpatial = beta * bssrdf.weightSpatialNM * bf;
								beta = beta * bssrdf.weightNM * bf;
							}

							BDPTVertex entryV;
							entryV.type = BDPTVertex::SURFACE;
							// DL-247: the SSS exit adds no medium-scatter vertex; carry the
							// subpath's count, or the per-path `volBounces` gate under-counts
							// every strategy ending here.
							entryV.volumeBounces = eyeVolumeBounces;
							entryV.position = bssrdf.entryPoint;
							entryV.normal = bssrdf.entryNormal;
							entryV.geomNormal = bssrdf.entryGeomNormal;
							entryV.onb = bssrdf.entryONB;
							// DL-22: forward live derivatives, signals, texture coordinates,
							// and vertex color from RandomWalkSSS::SampleExit.
							entryV.derivatives = bssrdf.derivatives;
							entryV.signals = bssrdf.signals;
							entryV.txFootprint = bssrdf.txFootprint;
							entryV.ptCoord = bssrdf.ptCoord;
							entryV.ptCoord1 = bssrdf.ptCoord1;
							entryV.bHasTexCoord1 = bssrdf.bHasTexCoord1;
							entryV.ptObjIntersec = bssrdf.ptObjIntersec;
							entryV.vColor = bssrdf.vColor;
							entryV.bHasVertexColor = bssrdf.bHasVertexColor;
							entryV.pMaterial = ri.pMaterial;
							entryV.pObject = ri.pObject;
							entryV.pMediumObject = pMedObj_eye;
							entryV.pMediumVol = pMed_eye;
							// DL-49: the entry point shares the exit hit's exterior medium
							// (the continuation carries the same IOR stack).  Without this
							// the vertex kept the 1.0 default and every re-evaluation of its
							// Sw (PathVertexEval, the zero-exitance sweep) priced an air
							// interface.
							entryV.mediumIOR = vertices.back().mediumIOR;

							// See RGB light subpath block for rationale.
							entryV.isDelta = true;
							entryV.isConnectible = false;
							entryV.isBSSRDFEntry = true;
							StoreThroughput<Tag>( entryV, betaSpatial );
							entryV.pdfFwd = 0;
							entryV.pdfRev = 0;
	#ifdef RISE_ENABLE_OPENPGL
							if constexpr( Traits::is_pel ) {
								entryV.guidingHasSegment = true;
								entryV.guidingDirectionOut = -bssrdf.scatteredRay.Dir();
								entryV.guidingNormal = entryV.normal;
								entryV.guidingEta = 1.0;
							}
	#endif
							// DL-09: the BSSRDF entry vertex records the tracked index its
							// throughput was priced to (the subsurface event is not a
							// straight segment, so no factor is paid across it).
							GradedIndexMedium::RecordVertex( iorStack, entryV.pGradedMedium, entryV.gradedIOR );
							vertices.push_back( entryV );

							pdfFwdPrev = bssrdf.cosinePdf;
							currentRay = bssrdf.scatteredRay;
							continue;
						}
						break;
					}
					if( R > NEARZERO ) {
						bssrdfReflectCompensation = 1.0 / R;
					}
				}
			}
			// --- End BSSRDF sampling ---

			// DL-307: an un-guided vertex that reached here without a lobe
			// (only to offer the subsurface branch its coin) has nothing
			// left to continue on.  A guided one still has the guide draw.
			if( !hasLobe && !guidedVertex ) {
				break;
			}

			const IORStack* traceIorStack = pScat->ior_stack ? pScat->ior_stack : &iorStack;
			IORStack guidedIorStack( iorStack );
	#ifdef RISE_ENABLE_OPENPGL
			// --- Path guiding (eye subpath) ---
			// DL-67: see `BDPTGuidedContinuation` for the ONE partition
			// every branch below prices on.  The gate is `guidedVertex`
			// (decided above from the vertex, never from which lobe the
			// Scatter realization happened to select), so every lobe --
			// delta, glossy, transmission, or none at all -- is priced
			// against the same technique-firing probability.
			bool usedGuidedDirection = false;
			V guidedF = Traits::zero();
			Vector3 guidedDir;
			Scalar guidedEffectivePdf = 0;	// substituted: weight == f cos / this
			Scalar keptPartitionScale = 1;	// kept lobe: multiplies kray / selectProb
			Scalar aggregateAtScatDir = -1;	// aggregate pdf at the traced direction, -1 = not evaluated
			bool guidedTerminate = false;

			if( guidedVertex )
			{
				if( pGuidingField->InitDistribution( guideDist, v.position, sampler.Get1D() ) )
				{
					pGuidingField->ApplyCosineProduct(
						guideDist,
						GuidingCosineNormal( v.normal, currentRay.Dir() ) );

					const BDPTVertex& gv = vertices.back();
					const Vector3 woIn = -currentRay.Dir();
					BDPTGuidedChoice<V> choice;
					BDPTGuidedContinuation<Tag, V>(
						*pGuidingField, guideDist, *pScat, hasLobe, selectProb,
						Traits::max_value( KrayValue<Tag>( *pScat ) ) / selectProb, guidingAlpha,
						guidingSamplingType, v.normal, sampler,
						[&]( const Vector3& w ) -> V {
							return PathValueOps::EvalBSDFAtVertex<Tag>( gv, w, woIn, tag );
						},
						// DL-43: EvalPdfAtVertex(vertex, wi, wo) is the
						// density of scattering INTO `wo` given incoming
						// `wi` -- wi must be the incoming direction.
						[&]( const Vector3& w ) -> Scalar {
							return PathValueOps::EvalPdfAtVertex<Tag>( gv, woIn, w, tag );
						},
						choice );
					guidedTerminate = choice.terminate;
					usedGuidedDirection = choice.substituted;
					keptPartitionScale = choice.keptScale;
					aggregateAtScatDir = choice.aggAtTrace;
					if( usedGuidedDirection ) {
						guidedDir = choice.dir;
						guidedF = choice.f;
						guidedEffectivePdf = choice.equivPdf;
						// The guide draw is a non-delta event of its own,
						// not the selected lobe's: continue on the
						// placeholder (diffuse, no IOR stack of its own --
						// `GuidedContinuationIORStack` then resolves the
						// medium from the material) and mark the vertex
						// non-delta for the MIS walk.
						guideTemplateRay.ray = Ray( ri.geometric.ptIntersection, guidedDir );
						pScat = &guideTemplateRay;
						vertices.back().isDelta = false;
					}
				}
			}
			// No selectable lobe and no guide draw (the distribution did
			// not initialise here): nothing carries the walk on.
			if( guidedTerminate || !( hasLobe || usedGuidedDirection ) ) {
				break;
			}
			if( usedGuidedDirection ) {
				traceIorStack = PathTransportUtilities::GuidedContinuationIORStack(
					*pScat, iorStack, ri, guidedDir, guidedIorStack );
			}
			// The density the continuation's weight corresponds to,
			// INCLUDING lobe selection: `f cos / equivScatterPdf` for a
			// substituted direction (whose estimator is not conditioned on
			// a lobe, so no `selectProb`), `selectProb * pdf / W-scale`
			// for a kept lobe.  It feeds OpenPGL's `pdfDirectionIn`
			// training input and `pdfFwd`'s zero-aggregate fallback -- the
			// two consumers that want "the density this direction was
			// drawn with" -- and nothing that weights a strategy.
			const Scalar equivScatterPdf = usedGuidedDirection ? guidedEffectivePdf :
				( keptPartitionScale > 0 ? selectProb * pScat->pdf / keptPartitionScale : Scalar( 0 ) );
			if constexpr( Traits::is_nm ) {
				// NM-only inline guiding-training sample.  The Pel path trains
				// from connection results in a post-pass instead -- preserved
				// Pel/NM asymmetry.
				const Scalar trainingPdf = equivScatterPdf;
				if( pGuidingField && pGuidingField->IsCollectingTrainingSamples() &&
					GuidingSupportsSurfaceSampling( *pScat ) && trainingPdf > NEARZERO )
				{
					const Ray trainingRay = usedGuidedDirection ?
						Ray( pScat->ray.origin, guidedDir ) : pScat->ray;
					const IORStack& trainingIorStack = *traceIorStack;
					RecordGuidingTrainingSampleNM(
						pGuidingField,
						rc,
						caster,
						ri,
						trainingRay,
						trainingPdf,
						depth + 2,
						tag.nm,
						trainingIorStack );
				}
			}
	#endif
			// --- End path guiding ---

			// Compute effective scatter direction and PDF
			Vector3 scatDir = pScat->ray.Dir();
			Scalar effectivePdf = pScat->pdf;

	#ifdef RISE_ENABLE_OPENPGL
			if( usedGuidedDirection ) {
				scatDir = guidedDir;
				effectivePdf = guidedEffectivePdf;
			}
	#endif

			// DL-126.  `BioSpecSkinMaterial` / `GenericHumanTissueMaterial`
			// have `GetBSDF() == 0` (so `vertices.back().isConnectible`,
			// set above, is already false) and their SPFs'
			// `Scatter`/`ScatterNM` never populate `ScatteredRay::pdf`
			// (default 0) -- their entire physically simulated transport
			// weight is reported directly as `kray`, an already-integrated,
			// unbiased Monte Carlo weight, not `f*cos/pdf`.  Both
			// `effectivePdf <= 0` here (from `pScat->pdf == 0`) and,
			// further below, `PositiveMagnitude(f) <= 0` (from the null
			// aggregate BSDF) used to `break` the walk on any non-delta
			// scatter off such a material -- in every tag, and in VCM/MLT,
			// which share this generator.  PT is unaffected: it prices
			// continuations from `pS->kray` alone
			// (`PathTracingIntegrator.cpp`) and gates on neither quantity.
			// Neither gate is meaningful for a vertex like this: no rival
			// strategy can ever reconstruct it for a connection
			// (isConnectible is false, so `PathVertexEval::EvalBSDFAtVertex`
			// always returns 0 there), so it is a black box exactly like a
			// delta lobe to every OTHER technique, even though its own
			// sample is not itself drawn from a Dirac direction.  The
			// guiding block above is already gated on
			// `vertices.back().isConnectible`, so `usedGuidedDirection` is
			// false and `keptPartitionScale` 1 here.
			const bool nullBSDFContinuation =
				!pScat->isDelta && !vertices.back().isConnectible;

			if( effectivePdf <= 0 && !nullBSDFContinuation ) {
				break;
			}

			// Per-type bounce limits
			if( PathTransportUtilities::ExceedsBounceLimitForType(
					pScat->type, eyeDiffuseBounces, eyeGlossyBounces,
					eyeTransmissionBounces, eyeTranslucentBounces, stabilityConfig ) ) {
				break;
			}

	#ifdef RISE_ENABLE_OPENPGL
			const Scalar scatterPdf = equivScatterPdf;
	#else
			const Scalar scatterPdf = selectProb * effectivePdf;
	#endif

			// HWSS per-wavelength pre-scatter throughput snapshot (NM bundle
			// only) so the RR below can take max(pre)/max(post) over active
			// wavelengths.
			[[maybe_unused]] Scalar hwssBetaNMPre[SampledWavelengths::N] = {};
			if constexpr( Traits::is_nm ) {
				if( pSwlHWSS ) {
					for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
						hwssBetaNMPre[w] = hwssBetaNM[w];
					}
				}
			}

			// Throughput update.  localScatteringWeight is retained for the
			// Pel path-guiding storage below (NM trains via samples instead).
			V localScatteringWeight = Traits::zero();
			// DL-67: at a guided vertex a KEPT delta lobe was kept with
			// probability `1 - alpha` (one-sample) or resampled (RIS), and
			// carries that factor too; 1 elsewhere.
	#ifdef RISE_ENABLE_OPENPGL
			const Scalar deltaGuideScale = keptPartitionScale;
	#else
			const Scalar deltaGuideScale = 1;
	#endif
			if( pScat->isDelta ) {
				localScatteringWeight =
					KrayValue<Tag>( *pScat ) * (bssrdfReflectCompensation * deltaGuideScale / selectProb);
				beta = beta * localScatteringWeight;
				if constexpr( Traits::is_nm ) {
					if( pSwlHWSS ) {
						// HERO-ONLY delta convention, predating the debt-30
						// eta^2 factor below: at a delta lobe every
						// companion wavelength is scaled by the HERO's
						// krayNM (`deltaScale`), not by its own per-
						// wavelength krayNM the way the non-delta branch
						// below evaluates a per-wavelength `fw`.  Unlike
						// PathTracingIntegrator.cpp's PT site, there is no
						// swl.TerminateSecondary() gate here -- this
						// hero-only pricing is applied unconditionally at
						// every delta eye-subpath vertex, dispersive or
						// not.  The eta^2 scale computed just below this
						// block is a SEPARATE per-vertex medium-change
						// factor and is likewise applied as one scalar to
						// hero and every live companion (see its own
						// comment): the two hero-only choices are
						// independent conventions that happen to compose
						// the same way.
						const Scalar deltaScale = pScat->krayNM * bssrdfReflectCompensation * deltaGuideScale / selectProb;
						hwssBetaNM[0] = beta;
						for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
							if( pSwlHWSS->terminated[w] ) continue;
							hwssBetaNM[w] = hwssBetaNM[w] * deltaScale;
						}
					}
				}
			} else if( nullBSDFContinuation ) {
				// DL-126.  Mirror the DELTA branch's own formula exactly:
				// no aggregate BSDF, no pdf division (this SPF doesn't
				// track one -- see the `nullBSDFContinuation` derivation
				// above), no guiding substitution (guiding never runs at a
				// non-connectible vertex).  `KrayValue<Tag>(*pScat)` is the
				// only quantity this continuation can legally be priced
				// from.
				if( PositiveMagnitude<Tag>( KrayValue<Tag>( *pScat ) ) <= 0 ) {
					break;
				}
				localScatteringWeight =
					KrayValue<Tag>( *pScat ) * (bssrdfReflectCompensation / selectProb);
				beta = beta * localScatteringWeight;
				if constexpr( Traits::is_nm ) {
					if( pSwlHWSS ) {
						// Same hero-only convention as the delta branch
						// immediately above: neither SPF overrides
						// `EvaluateKrayNM` (DL-125's fallback pattern), so
						// a per-wavelength `kray` is unavailable regardless.
						const Scalar deltaScale = pScat->krayNM * bssrdfReflectCompensation / selectProb;
						hwssBetaNM[0] = beta;
						for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
							if( pSwlHWSS->terminated[w] ) continue;
							hwssBetaNM[w] = hwssBetaNM[w] * deltaScale;
						}
					}
				}
			} else {
				V f;
	#ifdef RISE_ENABLE_OPENPGL
				f = usedGuidedDirection ? guidedF :
					PathValueOps::EvalBSDFAtVertex<Tag>( vertices.back(), scatDir, -currentRay.Dir(), tag );
	#else
				f = PathValueOps::EvalBSDFAtVertex<Tag>( vertices.back(), scatDir, -currentRay.Dir(), tag );
	#endif
				const Scalar cosTheta = fabs( Vector3Ops::Dot(
					scatDir, ri.geometric.vNormal ) );

				// Retained as a PATH-TERMINATION gate only, for a
				// CONNECTIBLE material (the null-BSDF, non-connectible case
				// is handled in the branch above instead -- DL-126) whose
				// SELECTED lobe's aggregate BSDF value happens to be zero
				// at the sampled direction: every connection strategy
				// through it also yields zero, and killing the walk there
				// is pre-existing behaviour this row deliberately does not
				// change.
				if( PositiveMagnitude<Tag>( f ) <= 0 ) {
					break;
				}
				const Scalar invScale = bssrdfReflectCompensation * cosTheta / scatterPdf;

				// DL-69.  This branch used to compute `f * invScale` --
				// the material's AGGREGATE `IBSDF::value()` (every lobe
				// summed) over `scatterPdf = selectProb * pScat->pdf`
				// (the ONE stochastically-selected lobe's own
				// conditional density).  Those are two different
				// measures.  For N accepted non-delta lobes with
				// overlapping support the expectation is N times the
				// true integral: conditioned on one Scatter() draw, the
				// expectation over the lobe choice of
				// `f_agg(w_I) cos / (q_I p_I(w_I))` is
				// `sum_I f_agg(w_I) cos / p_I(w_I)`, whose expectation
				// over the draw is `N * integral f_agg cos dw`.
				// tests/SchlickLobePairingTest.cpp measures exactly
				// 2.00x on `schlick_material`'s diffuse+specular pair
				// (up to 4x on its per-channel specular branch, which
				// pushes three specular rays plus the diffuse one).
				//
				// The fix is the DELTA branch's own formula, two dozen
				// lines above, and PT's ordinary initialization
				// (`PathTracingIntegrator.cpp`,
				// `PTScatterKray<Tag>(*pS) * (1/selectProb)`): pair the
				// SELECTED lobe's own `kray_I` -- which the SPF defines
				// as that lobe's `f_I cos / p_I` -- with the same lobe's
				// selection probability.  `E[kray_I / q_I] =
				// sum_I integral p_I kray_I dw`, the aggregate integral,
				// and unlike the mixture pairing it needs no closed-form
				// mixture density (RISE's `q_I` is realization-dependent,
				// a function of the already-drawn `kray`s, so no fixed
				// direction-only `q_I(w)` exists for it).
				//
				// GUIDING (DL-67, `BDPTGuidedContinuation`): when OpenPGL
				// SUBSTITUTED a direction, `scatDir` is not the lobe's
				// direction at all and `kray_I` does not price it -- the
				// guide technique's own estimator `f_agg cos / equivPdf`
				// does, with no `selectProb` (the guide draw is not
				// conditioned on a lobe; `equivPdf` already carries the
				// realized probability the guide fired).  When the lobe's
				// OWN direction was kept, its `kray_I / selectProb` takes
				// the BSDF technique's share of the aggregate partition
				// (`keptPartitionScale`, which is 1 wherever the guide
				// cannot fire).  The HWSS aggregate fallback's `invScale`
				// carries the same factor through `scatterPdf`.
				Scalar krayScale = bssrdfReflectCompensation / selectProb;
				bool useKray = true;
	#ifdef RISE_ENABLE_OPENPGL
				if( usedGuidedDirection ) {
					useKray = false;
				} else {
					krayScale = krayScale * keptPartitionScale;
				}
	#endif
				if( useKray ) {
					localScatteringWeight = KrayValue<Tag>( *pScat ) * krayScale;
				} else {
					localScatteringWeight = f * invScale;
				}
				// DL-125.  Record WHICH lobe priced this vertex, so
				// `RecomputeSubpathThroughputNM` can ask the SPF for
				// that lobe's own companion-wavelength kray instead of
				// forming a ratio of the material's AGGREGATE BSDF.
				// `eRayUnknown` when guiding SUBSTITUTED the direction
				// (`!useKray`): the hero was then priced from the
				// aggregate `f`, so its companion must be too, or the
				// two sides of the ratio would describe different
				// estimators.
				vertices.back().scatterType =
					useKray ? pScat->type : ScatteredRay::eRayUnknown;
				beta = beta * localScatteringWeight;
				if constexpr( Traits::is_nm ) {
					if( pSwlHWSS ) {
						hwssBetaNM[0] = beta;
						for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
							if( pSwlHWSS->terminated[w] ) continue;
							// DL-125 closed: like PT, ask EvaluateKrayNM
							// for the selected lobe at this companion
							// wavelength. Schlick and the other repaired
							// per-lobe SPFs now supply that weight.
							// Aggregate evaluation remains appropriate for
							// aggregate-density rays with matching BSDF
							// response, or a guiding-substituted direction.
							// CompositeSPF declines for its walker-emitted
							// rays and for per-branch composites (DL-221,
							// narrowed by DL-24).
							// TranslucentSPF supports its entry/exit lobes;
							// unsupported types can still decline and warn.
							Scalar compScale = -1;
							if( useKray && pSPF ) {
								const Scalar krayW = pSPF->EvaluateKrayNM(
									ri.geometric, pScat->ray.Dir(), pScat->type,
									pSwlHWSS->lambda[w], iorStack,
									pScat->isDelta ? -1.0 : pScat->pdf );
								if( krayW >= 0 ) {
									compScale = krayW * krayScale;
								}
							}
							if( compScale < 0 ) {
								// Match aggregate response with aggregate
								// density; a per-lobe density can instead
								// produce DL-69's summed-response mismatch.
								// CompositeSPF declines for its walker-emitted
								// rays and per-branch composites (DL-221) and names itself. TranslucentSPF
								// now evaluates its normal entry/exit lobes.
								if( useKray ) {
									NotePerLobeDensityCompanionFallback( pSPF );
								}
								const Scalar fw = PathVertexEval::EvalBSDFAtVertexNM(
									vertices.back(), scatDir, -currentRay.Dir(), pSwlHWSS->lambda[w] );
								compScale = fw * invScale;
							}
							hwssBetaNM[w] = hwssBetaNM[w] * compScale;
						}
					}
				}
			}

			// eta^2 basic-radiance factor (debt 30) -- EYE SUBPATH ONLY.
			// This generator is the eye side of BDPT, VCM and MLT alike, and
			// all three gather RADIANCE here, so a lobe that moves the ray
			// into another medium scales beta by (eta_before/eta_after)^2.
			// Its light-side twin `GenerateLightSubpathImpl` carries
			// IMPORTANCE and deliberately gets NO factor -- that asymmetry
			// is what makes a VCM merge (flux-side photon x radiance-side
			// eye vertex) come out right. The resolved continuation stack also
			// supplies training and the next vertex: guiding changes the
			// direction, not the medium associated with that geometric side.
			{
				const Scalar etaScale = RadianceEtaScale( iorStack, traceIorStack );
				if( etaScale != Scalar( 1 ) ) {
					localScatteringWeight = localScatteringWeight * etaScale;
					beta = beta * etaScale;
					if constexpr( Traits::is_nm ) {
						if( pSwlHWSS ) {
							// hwssBetaNM[0] mirrors beta by construction at
							// both throughput-update branches above; re-mirror
							// it rather than scaling it a second time.
							hwssBetaNM[0] = beta;
							for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
								if( pSwlHWSS->terminated[w] ) continue;
								hwssBetaNM[w] = hwssBetaNM[w] * etaScale;
							}
						}
					}
				}
			}

			// Russian Roulette after a few bounces -- depth threshold and
			// throughput floor are configurable.  HWSS uses MAX throughput
			// over active wavelengths (prevents hero-driven RR from amplifying
			// companions on rare survival).
			Scalar rrCurrMax = Traits::max_value( beta );
			Scalar rrPrevMax = Traits::max_value( VertexThroughput<Tag>( vertices.back() ) );
			if constexpr( Traits::is_nm ) {
				if( pSwlHWSS ) {
					for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
						if( pSwlHWSS->terminated[w] ) continue;
						const Scalar p = fabs( hwssBetaNMPre[w] );
						if( p > rrPrevMax ) rrPrevMax = p;
						const Scalar c = fabs( hwssBetaNM[w] );
						if( c > rrCurrMax ) rrCurrMax = c;
					}
				}
			}
			const PathTransportUtilities::RussianRouletteResult rr =
				PathTransportUtilities::EvaluateRussianRoulette(
					depth, stabilityConfig.rrMinDepth, stabilityConfig.rrThreshold,
					rrCurrMax, rrPrevMax,
					sampler.Get1D() );
			if( rr.terminate ) {
				break;
			}
			if( rr.survivalProb < 1.0 ) {
				const Scalar rrScale = Scalar( 1 ) / rr.survivalProb;
				beta = beta * rrScale;
				if constexpr( Traits::is_nm ) {
					if( pSwlHWSS ) {
						for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
							hwssBetaNM[w] = hwssBetaNM[w] * rrScale;
						}
					}
				}
			}

	#ifdef RISE_ENABLE_OPENPGL
			if constexpr( Traits::is_pel ) {
				vertices.back().guidingHasDirectionIn = true;
				vertices.back().guidingDirectionIn = scatDir;
				vertices.back().guidingPdfDirectionIn = scatterPdf;
				vertices.back().guidingScatteringWeight = localScatteringWeight;
				vertices.back().guidingRussianRouletteSurvivalProbability = rr.survivalProb;
				vertices.back().guidingEta =
					traceIorStack->top() > NEARZERO ? traceIorStack->top() : Scalar( 1 );
				vertices.back().guidingRoughness = pScat->isDelta ?
					Scalar( 0.0 ) :
					(pScat->type == ScatteredRay::eRayDiffuse ? Scalar( 1.0 ) : Scalar( 0.5 ));
			}
	#endif

			// Store the forward pdf for the next vertex.
			//
			// DL-69, second half.  This used to store `scatterPdf =
			// selectProb * pScat->pdf` -- the realization-dependent
			// SAMPLING density of the one lobe that was drawn.  But
			// `pdfFwd` is only ever read by the MIS ratio chain
			// (`MISWeight`'s `ri *= pdfRev/pdfFwd`, and VCM's
			// `bsdfDirPdfW = next.pdfFwd * distSq / cosAtGen`
			// recurrence -- verified: no contribution-side consumer
			// reads `pdfFwd` on a SURFACE vertex; the `pdfFwd` divides
			// at `BDPTIntegrator.cpp` s=1 splat / s=1 connection and
			// `VCMIntegrator.cpp`'s light-tracing branch are all on
			// LIGHT-type vertices), and the reverse side of that same
			// chain -- `pdfRev` here, and every connection strategy's
			// `pdfRev` override -- evaluates the material's AGGREGATE,
			// direction-only `ISPF::Pdf()` through
			// `PathValueOps::EvalPdfAtVertex`.  Two different formulas
			// for what Veach eq. 10.9 treats as ONE density.
			//
			// At a multi-lobe vertex with overlapping support,
			// `selectProb * p_I(w)` is generically SMALLER than the
			// aggregate density at the same `w` (the other lobes' mass
			// there is missing), which inflates every `pdfRev/pdfFwd`
			// ratio and so DEFLATES the balance/power weight of the
			// strategy that generated this vertex.
			// tests/SchlickLobePairingTest.cpp measures the two
			// densities disagreeing across two orders of magnitude on
			// real `schlick_material` draws.
			//
			// So `pdfFwd` now uses the SAME function the reverse walk
			// does, evaluated at the forward direction.  When that
			// function reports nothing (`misFwdPdf <= NEARZERO`) the
			// per-lobe value is kept, which is what this site had
			// before this row: `pdfRev` is zero at the same vertex and
			// `MISWeight`'s remap0 already governs that case, so
			// substituting a zero would be a behaviour change for no
			// consistency gain.
			//
			// WHICH MATERIALS REACH THAT FALLBACK (review, 2026-09-14;
			// corrected again 2026-09-18, debt-dl126 round 2 -- BOTH
			// earlier claims in this paragraph were wrong).  It is NOT
			// `BioSpecSkinSPF` / `GenericHumanTissueSPF`: this comment
			// used to say the walk "never gets here for them" because
			// `GetBSDF()` is null and the `PositiveMagnitude(f) <= 0`
			// gate breaks first -- that was already wrong before DL-126
			// closed (the ACTUAL prior gate was `effectivePdf <= 0`,
			// since neither SPF ever sets `ScatteredRay::pdf` either;
			// see docs/DL126_BDPT_NULL_BSDF_CONTINUATION.md), and is
			// simply false now that DL-126's `nullBSDFContinuation`
			// branch (a few lines below) routes those materials around
			// this whole block instead of reaching it.  The reachable
			// case is a material whose `Pdf` is real but does not cover
			// the lobe that was drawn.  `TranslucentSPF` WAS that case --
			// its `Pdf`/`PdfNM` covered neither Phong `cos^N` lobe (the
			// entering transmission and the interior backscatter), which
			// is what DL-41 named -- but DL-41 closed 2026-09-18 and its
			// aggregate now covers every lobe the side can emit, so the
			// fallback has no known production inhabitant today.  It is
			// kept because it is still the right answer for a genuinely
			// zero aggregate, and because being silently wrong for the
			// next SPF that acquires the property is the worse failure.
			//
			// `guidingPdfDirectionIn`, set a few lines above, keeps
			// `scatterPdf` deliberately: it is OpenPGL's
			// `pdfDirectionIn` training input (see this file's
			// `segment->pdfDirectionIn` store), which wants the true
			// sampling density -- role 1, not role 2.
			// Both of this block's density queries -- this vertex's own
			// `pdfFwd` and the predecessor's `pdfRev` -- are evaluated at
			// THIS vertex, differing only in which of the two directions
			// plays `wi`.  One context reconstructs the record and IOR
			// stack once and both share it (review P2-5; the duplicated
			// rebuild was measurable on an all-multi-lobe BDPT render).
			// It holds a reference to `vertices.back()`, and nothing
			// between here and its last use pushes to `vertices`.
			PathVertexEval::VertexPdfContext pdfCtx( vertices.back() );

			// DL-67: ONE rule, guided or not -- `pdfFwd` is the MIS
			// partner density, the aggregate `ISPF::Pdf()` at the
			// direction actually traced (the function `pdfRev` and every
			// connection strategy evaluate), never the density the guided
			// continuation was drawn with.  MIS is unbiased for any
			// weights that partition to one; what it needs is the SAME
			// function on every strategy, and no other strategy can
			// evaluate this vertex's trained guide.  The zero-aggregate
			// fallback is the density the continuation's own weight
			// corresponds to (`scatterPdf`).  A guided branch that already
			// evaluated the aggregate at `scatDir` hands it over rather
			// than paying for a second identical `Pdf()`.
			pdfFwdPrev = scatterPdf;
			if( !pScat->isDelta ) {
	#ifdef RISE_ENABLE_OPENPGL
				const Scalar misFwdPdf = aggregateAtScatDir >= 0 ? aggregateAtScatDir :
					PathValueOps::EvalPdfAtVertex<Tag>( pdfCtx, -currentRay.Dir(), scatDir, tag );
	#else
				const Scalar misFwdPdf = PathValueOps::EvalPdfAtVertex<Tag>(
					pdfCtx, -currentRay.Dir(), scatDir, tag );
	#endif
				if( misFwdPdf > NEARZERO ) {
					pdfFwdPrev = misFwdPdf;
				}
			}

			// In Veach's formulation, delta vertices should be "transparent" in the MIS walk
			if( pScat->isDelta ) {
				pdfFwdPrev = 0;
			}

			// Update previous vertex's pdfRev
			if( vertices.size() >= 2 ) {
				BDPTVertex& prev = vertices[ vertices.size() - 2 ];

				// Reverse PDF: returns 0 for delta interactions, handled by remap0 in MISWeight.
				const Scalar revPdfSA = PathValueOps::EvalPdfAtVertex<Tag>(
					pdfCtx,
					scatDir,
					-currentRay.Dir(),
					tag );

				// Convert to area measure at prev.  The geometric cosine is
				// only meaningful for SURFACE/LIGHT predecessors -- CAMERA
				// gets the sentinel 1.0 and MEDIUM bypasses cos entirely
				// (Veach SS11 medium area-pdf uses sigma_t).  Reading
				// prev.geomNormal on a medium vertex would consume zero-
				// init data; gate the dot product behind the type check.
				if( prev.type == BDPTVertex::MEDIUM ) {
					prev.pdfRev = BDPTUtilities::SolidAngleToAreaMedium( revPdfSA, prev.sigma_t_scalar, distSq );
				} else {
					const Scalar absCosAtPrev = (prev.type == BDPTVertex::CAMERA)
						? Scalar(1.0)
						: fabs( Vector3Ops::Dot( prev.geomNormal, currentRay.Dir() ) );
					prev.pdfRev = BDPTUtilities::SolidAngleToArea( revPdfSA, absCosAtPrev, distSq );
				}
				if( pScat->isDelta ) { prev.pdfRev = 0; }
			}

			// Advance to next ray
	#ifdef RISE_ENABLE_OPENPGL
			if( usedGuidedDirection ) {
				currentRay = Ray( pScat->ray.origin, guidedDir );
			} else {
				currentRay = pScat->ray;
			}
	#else
			currentRay = pScat->ray;
	#endif
			currentRay.Advance( BDPT_RAY_EPSILON );
			if( traceIorStack != &iorStack ) {
				iorStack = *traceIorStack;
			}
		}

		// Record subpath boundary (single contiguous range now that
		// path-tree branching has been excised).
		subpathStarts.push_back( static_cast<uint32_t>( vertices.size() ) );

		return static_cast<unsigned int>( vertices.size() );
	}
} // anonymous namespace (GenerateEyeSubpath F2a)

unsigned int BDPTIntegrator::GenerateEyeSubpath(
	const RuntimeContext& rc,
	const Ray& cameraRay,
	const Point2& screenPos,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	std::vector<BDPTVertex>& vertices,
	std::vector<uint32_t>& subpathStarts,
	PixelAOV* pPrimaryAOV
	) const
{
	return GenerateEyeSubpathImpl<PelTag>(
		maxEyeDepth, stabilityConfig, pLightSampler,
#ifdef RISE_ENABLE_OPENPGL
		pGuidingField, maxGuidingDepth, guidingAlpha, guidingSamplingType,
#endif
		rc, cameraRay, screenPos, scene, caster, sampler,
		vertices, subpathStarts, PelTag{}, 0, pPrimaryAOV );
}

//////////////////////////////////////////////////////////////////////
// ConnectAndEvaluate - evaluate a single (s,t) strategy.
//
// Each case handles a different connection topology:
//   s=0:       Eye path hits emitter.  No connection needed.
//   s=1, t>1:  Next event estimation (direct lighting).
//   t=1:       Light endpoint connects to camera.  Needs splatting.
//   s>1, t>1:  General connection between two interior vertices.
//
// For strategies that reach the camera from the light side (t==1),
// the contribution lands at an arbitrary pixel, so needsSplat=true
// and rasterPos is computed via
// BDPTCameraUtilities::RasterizeThrough() -- through the SAMPLED
// aperture point, which is `Rasterize()` exactly when the aperture is
// a point (debt 28).
//
// Delta vertices cannot participate in explicit connections since
// there is zero probability of the connection direction matching
// the specular direction.
//////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////
// ConnectAndEvaluateImpl<Tag> -- Phase 2c family F3a.
//
// The per-(s,t)-strategy connection evaluator, templatized over
// PelTag/NMTag.  Free function (not a member template) taking the
// BDPTIntegrator (for the public MISWeight / EvalConnectionTransmittance
// members) + pLightSampler as parameters, so BDPTIntegrator.h is
// untouched and the public ConnectAndEvaluate{,NM} members stay
// byte-identical one-line forwarders.  Reuses the F1/F2a anon-namespace
// helpers (VertexThroughput / PositiveMagnitude / TrOne) and
// PathValueOps::Eval{BSDF,Pdf}AtVertex<Tag>.
//////////////////////////////////////////////////////////////////////

namespace {

// Return-type mapping: ConnectionResult (Pel) / ConnectionResultNM (NM).
template<class Tag> struct ConnectionResultFor;
template<> struct ConnectionResultFor<PelTag> { typedef BDPTIntegrator::ConnectionResult   type; };
template<> struct ConnectionResultFor<NMTag>  { typedef BDPTIntegrator::ConnectionResultNM type; };

// Visibility test for connection edges -- F3a routes all four
// connection-site visibility queries through this free function.
inline bool ConnectionIsVisible( const IRayCaster& caster, const Point3& p1, const Point3& p2 )
{
	Vector3 d = Vector3Ops::mkVector3( p2, p1 );
	const Scalar dist = Vector3Ops::Magnitude( d );
	if( dist < BDPT_RAY_EPSILON ) {
		return true;
	}
	d = d * (1.0 / dist);
	Ray shadowRay( p1, d );
	shadowRay.Advance( BDPT_RAY_EPSILON );
	return !caster.CastShadowRay( shadowRay, dist - 2.0 * BDPT_RAY_EPSILON );
}

// Connection-edge transmittance dispatch -> the public (F1-templatized)
// member overloads.  pt/pt and ray/maxDist forms.
template<class Tag>
typename SpectralValueTraits<Tag>::value_type
EvalConnTr( const BDPTIntegrator& self, const Point3& p1, const Point3& p2,
	const IScene& scene, const IRayCaster& caster,
	const IObject* pStartMediumObject, const IMedium* pStartMedium, Tag tag )
{
	if constexpr( SpectralValueTraits<Tag>::is_pel ) {
		(void)tag;
		return self.EvalConnectionTransmittance( p1, p2, scene, caster, pStartMediumObject, pStartMedium );
	} else {
		return self.EvalConnectionTransmittanceNM( p1, p2, scene, caster, tag.nm, pStartMediumObject, pStartMedium );
	}
}
template<class Tag>
typename SpectralValueTraits<Tag>::value_type
EvalConnTr( const BDPTIntegrator& self, const Ray& connectionRay, const Scalar maxDist,
	const IScene& scene, const IRayCaster& caster,
	const IObject* pStartMediumObject, const IMedium* pStartMedium, Tag tag )
{
	if constexpr( SpectralValueTraits<Tag>::is_pel ) {
		(void)tag;
		return self.EvalConnectionTransmittance( connectionRay, maxDist, scene, caster, pStartMediumObject, pStartMedium );
	} else {
		return self.EvalConnectionTransmittanceNM( connectionRay, maxDist, scene, caster, tag.nm, pStartMediumObject, pStartMedium );
	}
}

// Env-map radiance lookup: GetRadiance (Pel) / GetRadianceNM(nm) (NM).
// nullRast is constructed internally (matches the originals' local {0}).
template<class Tag>
typename SpectralValueTraits<Tag>::value_type
EnvRadiance( const IRadianceMap* pEnvLight, const Ray& skyProbe, Tag tag )
{
	RasterizerState nullRast = {0};
	if constexpr( SpectralValueTraits<Tag>::is_pel ) {
		(void)tag;
		return pEnvLight->GetRadiance( skyProbe, nullRast );
	} else {
		return pEnvLight->GetRadianceNM( skyProbe, nullRast, tag.nm );
	}
}

// Mesh-luminary emitted radiance toward `dir`: rebuilds the RIG then
// dispatches emittedRadiance (Pel) / emittedRadianceNM(nm) (NM).  Returns
// zero when the luminary carries no emitter (matches the originals' guard).
// Caller has already checked vertex.pLuminary && pLuminary->GetMaterial().
template<class Tag>
typename SpectralValueTraits<Tag>::value_type
LuminaryRadiance( const BDPTVertex& vertex, const Vector3& dir, Tag tag )
{
	const IEmitter* pEmitter = vertex.pLuminary->GetMaterial()->GetEmitter();
	if( !pEmitter ) {
		return SpectralValueTraits<Tag>::zero();
	}
	RayIntersectionGeometric rig( Ray( vertex.position, dir ), nullRasterizerState );
	PathVertexEval::PopulateRIGFromVertex( vertex, rig );
	// DL-320: this is the LIGHT root evaluated toward an ARBITRARY
	// direction (the s = 1 connection to an eye vertex, the t = 1 splat to
	// the camera), not along the direction its subpath was sampled in.  A
	// double-sided emitter radiates from the face toward `dir`, whichever
	// face `SampleLight` happened to pick for the continuation; hand the
	// emitter that face (and make the record agree with it).  A one-sided
	// emitter keeps its winding normal and stays dark behind.
	const Vector3 face = EmitterSides::FaceToward(
		LightSampler::LuminaryIsTwoSided( vertex.pLuminary ), vertex.geomNormal, dir );
	if( Vector3Ops::Dot( face, vertex.geomNormal ) < 0 ) {
		rig.vNormal = -rig.vNormal;
		rig.vGeomNormal = -rig.vGeomNormal;
		rig.onb.FlipW();
	}
	if constexpr( SpectralValueTraits<Tag>::is_pel ) {
		(void)tag;
		return pEmitter->emittedRadiance( rig, dir, face );
	} else {
		return pEmitter->emittedRadianceNM( rig, dir, face, tag.nm );
	}
}

// ILight (point/spot/...) radiance toward `dir`.  The NM path calls
// `ILight::emittedRadianceNM`, which evaluates the light's own illuminant
// spectrum at the wavelength (Stage C slice 2) -- the same pattern as
// VCM's EvalLightRadiance<Tag>.  It used to project RGB->scalar via
// Rec.709 luminance and reuse that one number at every wavelength, which
// made coloured point / spot / directional lights spectrally grey.
template<class Tag>
typename SpectralValueTraits<Tag>::value_type
LightRadiance( const ILight* pLight, const Vector3& dir, Tag tag )
{
	if constexpr( SpectralValueTraits<Tag>::is_pel ) {
		(void)tag;
		return pLight->emittedRadiance( dir );
	} else {
		return pLight->emittedRadianceNM( dir, tag.nm );
	}
}

// Emitter radiance toward a direction (the single EvalEmitterRadiance<Tag>
// dispatch -- F3b folded the former protected EvalEmitterRadianceNM member
// into this).  PER-TAG INPUT CONTRACT differs (intentional, pre-existing):
//   Pel: `pEmitter` is the caller's already-resolved SURFACE emitter; the
//        helper calls it directly (env / pLight are resolved by the s=0
//        caller before reaching here).
//   NM : `pEmitter` is ignored; the helper re-resolves env / pLight / surface
//        from the vertex (used by the s=0 connection site AND the HWSS
//        RecomputeSubpathThroughputNM light-vertex emission ratio).
template<class Tag>
typename SpectralValueTraits<Tag>::value_type
EvalEmitterRadiance( const BDPTVertex& eyeEnd, const Vector3& woFromEmitter,
	const IEmitter* pEmitter, Tag tag )
{
	if constexpr( SpectralValueTraits<Tag>::is_pel ) {
		(void)tag;
		RayIntersectionGeometric rig( Ray( eyeEnd.position, woFromEmitter ), nullRasterizerState );
		PathVertexEval::PopulateRIGFromVertex( eyeEnd, rig );
		return pEmitter->emittedRadiance( rig, woFromEmitter, eyeEnd.geomNormal );
	} else {
		(void)pEmitter;
		if( eyeEnd.pEnvLight ) {
			RasterizerState nullRast = {0};
			Ray skyProbe( eyeEnd.position, -woFromEmitter );
			return eyeEnd.pEnvLight->GetRadianceNM( skyProbe, nullRast, tag.nm );
		}
		if( eyeEnd.pLight ) {
			// Stage C slice 2: the light's own spectrum at tag.nm, not a
			// flat Rec.709 luma projection.
			return eyeEnd.pLight->emittedRadianceNM( woFromEmitter, tag.nm );
		}
		// Surface / mesh emitter.  Prefer pMaterial (set on an EYE vertex
		// that hit the emitter -- the s=0 strategy); fall back to pLuminary
		// (a light-SUBPATH endpoint sampled by the light sampler stores the
		// emissive material on pLuminary with pMaterial == 0 -- see
		// GenerateLightSubpath vertex 0).  Without the fallback the
		// RecomputeSubpathThroughputNM Phase-1 emission ratio re-evaluates
		// Le == 0 on every mesh-emitter light subpath, zeroing that
		// subpath's companion-wavelength throughput.
		const IMaterial* pEmMat = eyeEnd.pMaterial
			? eyeEnd.pMaterial
			: ( eyeEnd.pLuminary ? eyeEnd.pLuminary->GetMaterial() : 0 );
		if( !pEmMat ) {
			return 0;
		}
		const IEmitter* pEm = pEmMat->GetEmitter();
		if( !pEm ) {
			return 0;
		}
		RayIntersectionGeometric rig( Ray( eyeEnd.position, woFromEmitter ), nullRasterizerState );
		PathVertexEval::PopulateRIGFromVertex( eyeEnd, rig );
		return pEm->emittedRadianceNM( rig, woFromEmitter, eyeEnd.geomNormal, tag.nm );
	}
}

// Broadcast a scalar geometric term to the value type (interior contribution).
template<class Tag>
typename SpectralValueTraits<Tag>::value_type
BroadcastScalar( const Scalar g )
{
	if constexpr( SpectralValueTraits<Tag>::is_pel ) {
		return RISEPel( g, g, g );
	} else {
		return g;
	}
}

template<class Tag>
typename ConnectionResultFor<Tag>::type
ConnectAndEvaluateImplCore(
	const BDPTIntegrator& self,
	const LightSampler* pLightSampler,
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<BDPTVertex>& eyeVerts,
	unsigned int s,
	unsigned int t,
	const IScene& scene,
	const IRayCaster& caster,
	const ICamera& camera,
	// Debt 28: two canonical randoms picking the point on the camera's
	// entrance APERTURE that a t==1 connection lands on.  Ignored by
	// every camera whose aperture is a point.
	const Point2& cameraLensSample,
	Tag tag )
{
	typedef SpectralValueTraits<Tag> Traits;
	typedef typename Traits::value_type V;
	typename ConnectionResultFor<Tag>::type result;
	if constexpr( Traits::is_nm ) {
		result.s = s;
	}

	// Validate: s <= lightVerts.size(), t <= eyeVerts.size(), s+t >= 2
	if( s > lightVerts.size() || t > eyeVerts.size() ) {
		return result;
	}

	if( s + t < 2 ) {
		return result;
	}

	//
	// Case: s == 0, t > 0
	// Pure eye path -- last eye vertex hits an emitter directly
	//
	if( s == 0 )
	{
		const BDPTVertex& eyeEnd = eyeVerts[t - 1];

		// Env-light escape vertex (Path B).  Pushed by
		// GenerateEyeSubpath at the !ri.bHit termination so the s=0
		// strategy can credit the env contribution.  Bypass the
		// SURFACE / pMaterial / GetEmitter chain — env vertices have
		// no material and live "at infinity".
		if( eyeEnd.type == BDPTVertex::LIGHT && eyeEnd.pEnvLight ) {
			if( t < 2 ) {
				// t == 1 would mean the camera itself is the env,
				// which doesn't make sense.  Guard against pathological
				// camera-only subpaths that managed to push an env
				// vertex.
				return result;
			}
			const BDPTVertex& eyePred = eyeVerts[t - 2];
			// Recover the ORIGINAL escape ray direction from the
			// stored geomNormal (Path B sets geomNormal = -ray.Dir).
			// Computing wiSky from (eyeEnd.position - eyePred.position)
			// would give the wrong direction whenever eyePred is not
			// the immediate scatter origin of the escape ray (e.g.
			// after a refraction → eyePred is on the far side of a
			// glass surface but the escape ray came from a different
			// in-medium offset) — adversarial review P1b.  The disc
			// position is bookkeeping for MIS dist²; the direction
			// for env lookup must be the original ray direction.
			const Vector3 wiSky(
				-eyeEnd.geomNormal.x,
				-eyeEnd.geomNormal.y,
				-eyeEnd.geomNormal.z );

			Ray skyProbe( eyePred.position, wiSky );
			const V Le = EnvRadiance<Tag>( eyeEnd.pEnvLight, skyProbe, tag );
			if( PositiveMagnitude<Tag>( Le ) <= 0 ) {
				return result;
			}

			// s=0 contribution: throughput accumulated to the escape
			// point times the env radiance in the escape direction.
			// (Throughput at the env vertex already reflects all
			//  eye-subpath BSDF factors and cosines up to the miss.)
			result.contribution = VertexThroughput<Tag>( eyeEnd ) * Le;
			result.needsSplat = false;
			result.valid = true;
			if constexpr( Traits::is_pel ) {
			result.guidingLocalContribution = Le;
			result.guidingEyeVertexIndex = t - 1;
			result.guidingUseDirectContribution = true;
			result.guidingValid = true;
			}

			// MIS weight: install pdfRev on eyeEnd as "the probability
			// the s=1 NEE alternative would have sampled this env
			// vertex" — in area-measure on the disc:
			//   pdfRev_area = envSelectProb * pdfPosition_disc
			//               = envSelectProb / (π · r_scene²)
			// Post the 2026-05-29 continuous-PMF fix
			// (IMPROVEMENTS.md §12, PRE_PHASE1_STATUS.md Session 9),
			// `EnvSelectProbability()` returns a continuous positive
			// value whenever env exists — env is now part of the
			// alias-table selection space via the env-vs-alias roll
			// in `LightSampler::SampleLight()`.  So `pdfRevReal` is
			// strictly positive whenever this code is reached (the
			// reach gate is `eyeEnd.pEnvLight != 0`, which requires
			// env existed at sample time, which means
			// `cachedEnvSelectProb > 0` per
			// `RecomputeEnvSelectProbability`).  The prior
			// `kEnvZeroSentinel = 1e-30` workaround that paired with
			// MISWeight's `remap0` line for the binary-PMF mixed-
			// scene case is therefore dead code — removed in the
			// follow-up cleanup.  Restored after MIS call to preserve
			// const-correctness for other (s,t) evaluations.
			const Scalar savedEyeEndPdfRev = eyeEnd.pdfRev;
			const Scalar savedEyePredPdfRev = eyePred.pdfRev;
			if( pLightSampler ) {
				const Scalar envSelectProb =
					pLightSampler->EnvSelectProbability();
				const Scalar sceneRadius =
					pLightSampler->GetCachedSceneRadius();
				const Scalar discArea =
					( sceneRadius > 0 ) ?
					( PI * sceneRadius * sceneRadius ) : Scalar( 0 );
				const Scalar pdfPositionDisc =
					( discArea > 0 ) ? ( Scalar( 1 ) / discArea ) : Scalar( 0 );
				const_cast<BDPTVertex&>( eyeEnd ).pdfRev =
					envSelectProb * pdfPositionDisc;
			}
			if( pLightSampler && pLightSampler->GetEnvironmentSampler() ) {
				// Same continuous-PMF cleanup as the eyeEnd block
				// above — sentinel removed.  envSelectProb is now
				// continuous positive whenever env exists.
				const Scalar envSelectProb =
					pLightSampler->EnvSelectProbability();
				const Scalar pdfSA = envSelectProb *
					pLightSampler->GetEnvironmentSampler()->Pdf( wiSky );
				const Vector3 dToPred = Vector3Ops::mkVector3(
					eyePred.position, eyeEnd.position );
				const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
				Scalar predPdfRev = 0;
				if( eyePred.type == BDPTVertex::CAMERA ) {
					predPdfRev = BDPTUtilities::SolidAngleToArea(
						pdfSA, Scalar( 1.0 ), distPredSq );
				} else if( eyePred.type == BDPTVertex::MEDIUM ) {
					// Volume-scatter vertex: use the medium area-
					// Jacobian with sigma_t (matches s=1 NEE branch
					// and the eye-subpath gen for symmetry).
					predPdfRev = BDPTUtilities::SolidAngleToAreaMedium(
						pdfSA, eyePred.sigma_t_scalar, distPredSq );
				} else {
					const Scalar absCosAtPred = fabs( Vector3Ops::Dot(
						eyePred.geomNormal, Vector3Ops::Normalize( dToPred ) ) );
					predPdfRev = BDPTUtilities::SolidAngleToArea(
						pdfSA, absCosAtPred, distPredSq );
				}
				const_cast<BDPTVertex&>( eyePred ).pdfRev = predPdfRev;
			}
			result.misWeight = self.MISWeight( lightVerts, eyeVerts, s, t );
			const_cast<BDPTVertex&>( eyeEnd ).pdfRev = savedEyeEndPdfRev;
			const_cast<BDPTVertex&>( eyePred ).pdfRev = savedEyePredPdfRev;
			return result;
		}

		if( eyeEnd.type != BDPTVertex::SURFACE ) {
			return result;
		}

		if( !eyeEnd.pMaterial ) {
			return result;
		}

		const IEmitter* pEmitter = eyeEnd.pMaterial->GetEmitter();
		if( !pEmitter ) {
			return result;
		}

		// The eye path naturally arrived at an emitter
		// The outgoing emission direction is from the emitter toward the
		// predecessor eye vertex (the reverse of the eye ray's travel).
		Vector3 woFromEmitter;
		if( t >= 2 ) {
			woFromEmitter = Vector3Ops::mkVector3( eyeVerts[t - 2].position, eyeEnd.position );
			woFromEmitter = Vector3Ops::Normalize( woFromEmitter );
		} else {
			// t == 1 means the camera vertex is the emitter, which doesn't make sense
			return result;
		}

		// Evaluate emitted radiance at this point.  PopulateRIGFromVertex
		// is the canonical RIG-rebuild — same contract as the helper used
		// by the connection sites at lines 4341 / 7265.  Without it,
		// emissive painters bound to TEXCOORD_1 (or any non-default UV)
		// silently sample at (0,0) on the (s=0) emitter strategy because
		// the manual rebuild left ptCoord / ptCoord1 / bHasTexCoord1
		// default-constructed.
		const V Le = EvalEmitterRadiance<Tag>( eyeEnd, woFromEmitter, pEmitter, tag );

		if( PositiveMagnitude<Tag>( Le ) <= 0 ) {
			return result;
		}

		result.contribution = VertexThroughput<Tag>( eyeEnd ) * Le;
		result.needsSplat = false;
		result.valid = true;
		if constexpr( Traits::is_pel ) {
		result.guidingLocalContribution = Le;
		result.guidingEyeVertexIndex = t - 1;
		result.guidingUseDirectContribution = true;
		result.guidingValid = true;
		}

		// --- Update pdfRev at emitter vertex for correct MIS ---
		// eyeEnd.pdfRev should be the PDF that the light sampling process
		// would have generated this point: pdfSelect * pdfPosition.
		const Scalar savedEyeEndPdfRev = eyeEnd.pdfRev;
		const bool savedEyeEndLightSamplingAbsent = eyeEnd.lightSamplingStrategyAbsent;

		if( pLightSampler && eyeEnd.pObject )
		{
			const ILuminaryManager* pLumMgr = caster.GetLuminaries();
			const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
			LuminaryManager::LuminariesList emptyList;
			const LuminaryManager::LuminariesList& luminaries = pLumManager ?
				const_cast<LuminaryManager*>( pLumManager )->getLuminaries() : emptyList;

			// For BVH PDF, the shading point is the predecessor vertex
			// (where NEE would have selected this emitter from).
			const BDPTVertex& predVert_s0 = eyeVerts[t - 2];
			const Scalar pdfSelect = pLightSampler->PdfSelectLuminary(
				scene, luminaries, *eyeEnd.pObject,
				predVert_s0.position, predVert_s0.normal );
			// CRASH FIX (2026-07-31 fix round 2): eyeEnd.pObject->GetArea()
			// used to be called UNCONDITIONALLY here -- reached on the
			// primary camera ray directly viewing an emitter (the eye path's
			// s=0 strategy), so a csg_object luminary null-derefed inside
			// Object::GetArea() (pre this fix round).  PROVEN SAFE by TWO
			// independent layers even without this restructure: (1)
			// Object::GetArea() now has a base-layer null guard (Object.cpp)
			// and returns 0 for a null-geometry object, so `area > 0` below
			// was already false and pdfPosition was already 0 -- no
			// div-by-zero; (2) eyeEnd.pObject is never a REGISTERED NEE
			// luminary when its geometry is null (LuminaryManager refuses
			// it), so PdfSelectLuminary() -- which only matches against
			// LuminaryManager's already-filtered light table -- already
			// returns pdfSelect == 0 for it, making `pdfSelect * pdfPosition`
			// == 0 regardless of pdfPosition's value.  This restructure adds
			// a THIRD layer (skip GetArea() entirely when there is no
			// geometry to sample) purely for defense-in-depth /
			// consistency with the null-safety convention now used at every
			// other luminary-area call site -- it does not change the
			// computed pdfRev value in any case (still 0 whenever geometry
			// is null or CanBeAreaLight() is false, matching the existing
			// area<=0 semantics; no new MIS weighting introduced).
			//
			// P3 (fix round 3): this value-identity argument (pdfSelect==0
			// for a null-geometry/non-CanBeAreaLight object) is NOT purely
			// local -- it depends on two invariants enforced elsewhere:
			//   (a) LuminaryManager::AddToLuminaryList (LuminaryManager.cpp)
			//       is the SOLE admission gate for the luminaries list
			//       `pLightSampler`/`luminaries` are built from; no other
			//       code path adds an object to it, so "not admitted" here
			//       and "refused by LuminaryManager" are the same fact.
			//   (b) RayCaster::AttachScene's realize pass (RayCaster.cpp
			//       ~225-231, `obj.Realize()` over every world-visible
			//       object) runs BEFORE RebuildLightSamplers (~246/259),
			//       which runs before any pixel is shaded -- so by the time
			//       LuminaryManager decided admission AND by the time this
			//       code reads eyeEnd.pObject->GetGeometry() /
			//       CanBeAreaLight() at shading time, the object's geometry
			//       is in the SAME final realized state both times.  If
			//       realize ran AFTER RebuildLightSamplers, LuminaryManager
			//       could admit/refuse based on a not-yet-realized geometry
			//       that later resolves differently, breaking the identity
			//       this comment relies on.
			//
			// MIS CAVEAT -- RESOLVED 2026-08-01 (was: "known defect,
			// deferred", fix round 3 / Opus review).  pdfRev == 0 for a
			// non-NEE-sampleable emitter is the correct INPUT, but
			// MISWeight's remap0 step used to promote that zero to 1,
			// manufacturing a phantom NEE strategy in the denominator.  The
			// zero is now TAGGED (`lightSamplingStrategyAbsent`) so the
			// eye-side walk can tell it apart from the delta-vertex zero
			// remap0 exists for; see that field's contract in BDPTVertex.h
			// and the walk's own comment.
			const IGeometry* pEyeEndGeom = eyeEnd.pObject->GetGeometry();
			const bool eyeEndAreaSampleable = pEyeEndGeom && pEyeEndGeom->CanBeAreaLight();
			const Scalar area = eyeEndAreaSampleable ? eyeEnd.pObject->GetArea() : Scalar( 0 );
			const Scalar pdfPosition = (area > 0) ? (Scalar(1.0) / area) : 0;
			const Scalar eyeEndPdfRev = pdfSelect * pdfPosition;
			const_cast<BDPTVertex&>( eyeEnd ).pdfRev = eyeEndPdfRev;

			// The whole s >= 1 family -- NEE at s=1, and every light-tracing
			// strategy behind it -- requires the light-sampling process to
			// be able to root a subpath on THIS emitter.  When the emitter
			// is not in the NEE light set at all, none of those strategies
			// exists, so none of them may contribute to this path's MIS
			// denominator.  Tag that here, where the reason is known, rather
			// than trying to re-derive it inside the ratio walk.
			//
			// The predicate is SET MEMBERSHIP -- null geometry
			// (LuminaryManager refuses it), CanBeAreaLight() == false (an
			// SDF whose sampling mesh proved it misses renderable surface;
			// see SDFGeometry.h), or a degenerate zero-area emitter -- and
			// deliberately NOT `eyeEndPdfRev <= 0`.  The two differ on one
			// real case: with `light_bvh` enabled (opt-in),
			// `PdfSelectLuminary` can return 0 for an emitter that IS in the
			// set (an orientation-zeroed cluster -- NodeImportance's
			// max(0, cos thetaPrime) drives probL to 0 from THIS shading
			// point), while the s >= 1 strategies actually root their light
			// subpaths with the shading-point-independent alias draw, which
			// is strictly positive for any in-set light.  Keying off the
			// pdf would then delete EXISTING strategies from the
			// denominator, pushing the weights' sum above 1 -- an energy
			// EXCESS, the opposite error to the one being fixed.  Set
			// membership is the property the s >= 1 family actually depends
			// on, and it is what PT gates on too
			// (PathTracingIntegrator.cpp:2173, `pEmitGeom &&
			// pEmitGeom->CanBeAreaLight()` plus its own `area > 0`).
			//
			// For an out-of-set emitter this restores agreement with PT
			// (which skips its emission-MIS block entirely, leaving
			// emissionMiWeight = 1) and with VCM (`pdfSelect > 0 ?
			// wCameraJoint / pdfSelect : 0`).  It says nothing about the
			// zero-pdf-but-in-set case, which the three integrators handle
			// differently by design and which this flag no longer touches --
			// PT there falls back to pdfSelect = 1.0 and down-weights
			// (PathTracingIntegrator.cpp:2206-2213), and BDPT keeps the
			// treatment it has always had.
			const_cast<BDPTVertex&>( eyeEnd ).lightSamplingStrategyAbsent =
				( !eyeEndAreaSampleable || area <= 0 );
		}

		// --- Update predecessor pdfRev (eyeVerts[t-2]) ---
		// The emission directional PDF from the emitter toward the predecessor
		// vertex determines pdfRev at the predecessor.
		Scalar savedEyePredPdfRev = 0;
		const bool hasEyePred = (t >= 2);
		if( hasEyePred )
		{
			const BDPTVertex& eyePred = eyeVerts[t - 2];
			savedEyePredPdfRev = eyePred.pdfRev;

			// Emission directional PDF at eyeEnd toward eyePred
			Scalar emPdfDir = 0;
			if( eyeEnd.pMaterial ) {
				const IEmitter* pEm = eyeEnd.pMaterial->GetEmitter();
				if( pEm ) {
					// Cosine-weighted emission: the density of
					// `LightSampler::SampleLight`'s emission sampler, which
					// for a double-sided emitter picks a face with
					// probability 1/2 (DL-320).  `eyeEnd.geomNormal` is the
					// ray-facing normal there, so the one-sided branch would
					// double this density for the back face.
					emPdfDir = EmitterSides::CosineEmissionPdf(
						LightSampler::LuminaryIsTwoSided( eyeEnd.pObject ),
						eyeEnd.geomNormal, woFromEmitter );
				}
			}

			const Vector3 dToPred = Vector3Ops::mkVector3( eyePred.position, eyeEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			// Camera vertex: use 1.0; Medium vertex: sigma_t/dist^2
			if( eyePred.type == BDPTVertex::CAMERA ) {
				const_cast<BDPTVertex&>( eyePred ).pdfRev =
					BDPTUtilities::SolidAngleToArea( emPdfDir, Scalar(1.0), distPredSq );
			} else if( eyePred.type == BDPTVertex::MEDIUM ) {
				const_cast<BDPTVertex&>( eyePred ).pdfRev =
					BDPTUtilities::SolidAngleToAreaMedium( emPdfDir, eyePred.sigma_t_scalar, distPredSq );
			} else {
				const Scalar absCosAtPred =
					fabs( Vector3Ops::Dot( eyePred.geomNormal,
						Vector3Ops::Normalize( dToPred ) ) );
				const_cast<BDPTVertex&>( eyePred ).pdfRev =
					BDPTUtilities::SolidAngleToArea( emPdfDir, absCosAtPred, distPredSq );
			}
		}

		result.misWeight = self.MISWeight( lightVerts, eyeVerts, s, t );

		const_cast<BDPTVertex&>( eyeEnd ).pdfRev = savedEyeEndPdfRev;
		// Restored unconditionally alongside pdfRev (the set above is inside
		// the `pLightSampler && pObject` guard, so an unconditional restore
		// is what keeps the vertex array clean for the OTHER (s,t)
		// evaluations that share it).
		const_cast<BDPTVertex&>( eyeEnd ).lightSamplingStrategyAbsent =
			savedEyeEndLightSamplingAbsent;
		if( hasEyePred ) {
			const_cast<BDPTVertex&>( eyeVerts[t - 2] ).pdfRev = savedEyePredPdfRev;
		}

		return result;
	}

	//
	// Legacy t == 0 path-to-camera case.
	// The active path enumeration in this file includes the camera as
	// eye vertex 0, so the camera-connection strategy is t == 1.
	// This branch is kept only for compatibility with any future caller
	// that enumerates paths without an explicit camera vertex.
	//
	if( t == 0 )
	{
		// Legacy path-to-camera case -- DEAD CODE: EvaluateAllStrategies{,NM}
		// both enumerate t starting at 1, so t==0 is never reached.  The Pel
		// and NM originals carry minor (unreachable) divergences -- Pel is
		// medium-aware (lightIsMedium fLight + medium pdfRev sub-cases) while
		// NM is surface-only and gates the predecessor on `&& pMaterial`.
		// Preserved verbatim per tag; behaviourally inert.
		if constexpr( Traits::is_pel ) {
		// Light path endpoint connects directly to camera sensor
		const BDPTVertex& lightEnd = lightVerts[s - 1];

		if( !lightEnd.isConnectible ) {
			return result;
		}

		// Project the light vertex position onto the camera.  Aperture-
		// aware for the same reason the live t==1 site is (debt 28),
		// even though this branch is unreachable -- leaving one of the
		// two spellings of the same connection behind would be a trap
		// for whoever revives it.
		const BDPTCameraUtilities::ApertureSample apertureSample_t0 =
			BDPTCameraUtilities::SampleAperture( camera, cameraLensSample );
		Point2 rasterPos;
		if( !BDPTCameraUtilities::RasterizeThrough(
				camera, lightEnd.position, apertureSample_t0, rasterPos ) ) {
			return result;
		}

		// Check visibility from camera to light vertex using standard shadow ray.
		const Point3 camPos = apertureSample_t0.point;
		if( !ConnectionIsVisible( caster, camPos, lightEnd.position ) ) {
			return result;
		}

		// Compute the camera importance
		Vector3 dirToCam = Vector3Ops::mkVector3( camPos, lightEnd.position );
		const Scalar dist = Vector3Ops::Magnitude( dirToCam );
		if( dist < BDPT_RAY_EPSILON ) {
			return result;
		}
		dirToCam = dirToCam * (1.0 / dist);

		Ray camRay( camPos, -dirToCam );
		const Scalar We = BDPTCameraUtilities::Importance( camera, camRay );

		if( We <= 0 ) {
			return result;
		}

		// Evaluate BSDF (or phase function) at the light endpoint for the direction toward camera
		const bool lightIsMedium_t0 = (lightEnd.type == BDPTVertex::MEDIUM);
		V fLight = TrOne<Tag>();
		if( (lightEnd.type == BDPTVertex::SURFACE && lightEnd.pMaterial) || lightIsMedium_t0 ) {
			Vector3 wiAtLight;
			if( s >= 2 ) {
				wiAtLight = Vector3Ops::mkVector3( lightVerts[s - 2].position, lightEnd.position );
				wiAtLight = Vector3Ops::Normalize( wiAtLight );
			} else {
				// s == 1: the light source vertex itself; fLight from emitter Le is in throughput
				fLight = RISEPel( 1, 1, 1 );
			}

			if( s >= 2 ) {
				fLight = PathValueOps::EvalAreaBSDFAtVertex<Tag>( lightEnd, wiAtLight, dirToCam, tag );
			}
		}

		// Geometric term between light endpoint and camera
		// Medium vertices have no surface cosine
		const Scalar distSq = dist * dist;
		const Scalar absCosLight = lightEnd.isDelta ?
			Scalar(1.0) :
			(lightIsMedium_t0 ? Scalar(1.0) :
				fabs( Vector3Ops::Dot( lightEnd.geomNormal, dirToCam ) ));
		const Scalar G = absCosLight / distSq;

		result.contribution = VertexThroughput<Tag>( lightEnd ) * fLight * (G * We);
		result.rasterPos = rasterPos;
		result.needsSplat = true;
		result.valid = true;

		// --- Update pdfRev at light endpoint for correct MIS ---
		// lightEnd.pdfRev: PDF that camera would generate lightEnd
		// = camera's directional PDF converted to area at lightEnd
		const Scalar savedLightPdfRev = lightEnd.pdfRev;
		{
			Ray camRayToLight( camPos, -dirToCam );
			const Scalar camPdfDir = BDPTCameraUtilities::PdfDirection( camera, camRayToLight );
			if( lightIsMedium_t0 ) {
				const_cast<BDPTVertex&>( lightEnd ).pdfRev =
					BDPTUtilities::SolidAngleToAreaMedium( camPdfDir, lightEnd.sigma_t_scalar, distSq );
			} else {
				const_cast<BDPTVertex&>( lightEnd ).pdfRev =
					BDPTUtilities::SolidAngleToArea( camPdfDir, absCosLight, distSq );
			}
		}

		// --- Update predecessor pdfRev (lightVerts[s-2]) ---
		// PDF at lightEnd of scattering toward lightVerts[s-2] given incoming = dirToCam
		Scalar savedLightPredPdfRev_t0 = 0;
		const bool hasLightPred_t0 = (s >= 2);
		if( hasLightPred_t0 )
		{
			const BDPTVertex& lightPred = lightVerts[s - 2];
			savedLightPredPdfRev_t0 = lightPred.pdfRev;

			Vector3 wiAtLightEnd;
			if( s >= 2 ) {
				wiAtLightEnd = Vector3Ops::mkVector3( lightVerts[s - 2].position, lightEnd.position );
				wiAtLightEnd = Vector3Ops::Normalize( wiAtLightEnd );
			}

			const Scalar pdfPredSA = PathValueOps::EvalPdfAtVertex<Tag>( lightEnd, dirToCam, wiAtLightEnd, tag );
			const Vector3 dToPred = Vector3Ops::mkVector3( lightPred.position, lightEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			if( lightPred.type == BDPTVertex::MEDIUM ) {
				const_cast<BDPTVertex&>( lightPred ).pdfRev =
					BDPTUtilities::SolidAngleToAreaMedium( pdfPredSA, lightPred.sigma_t_scalar, distPredSq );
			} else {
				const Scalar absCosAtPred = fabs( Vector3Ops::Dot( lightPred.geomNormal,
					Vector3Ops::Normalize( dToPred ) ) );
				const_cast<BDPTVertex&>( lightPred ).pdfRev =
					BDPTUtilities::SolidAngleToArea( pdfPredSA, absCosAtPred, distPredSq );
			}
		}

		result.misWeight = self.MISWeight( lightVerts, eyeVerts, s, t );

		const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRev;
		if( hasLightPred_t0 ) {
			const_cast<BDPTVertex&>( lightVerts[s - 2] ).pdfRev = savedLightPredPdfRev_t0;
		}

		return result;
		} else {
		const BDPTVertex& lightEnd = lightVerts[s - 1];
		if( !lightEnd.isConnectible ) {
			return result;
		}

		const BDPTCameraUtilities::ApertureSample apertureSample_t0 =
			BDPTCameraUtilities::SampleAperture( camera, cameraLensSample );
		Point2 rasterPos;
		if( !BDPTCameraUtilities::RasterizeThrough(
				camera, lightEnd.position, apertureSample_t0, rasterPos ) ) {
			return result;
		}

		const Point3 camPos = apertureSample_t0.point;
		if( !ConnectionIsVisible( caster, camPos, lightEnd.position ) ) {
			return result;
		}

		Vector3 dirToCam = Vector3Ops::mkVector3( camPos, lightEnd.position );
		const Scalar dist = Vector3Ops::Magnitude( dirToCam );
		if( dist < BDPT_RAY_EPSILON ) {
			return result;
		}
		dirToCam = dirToCam * (1.0 / dist);

		Ray camRay( camPos, -dirToCam );
		const Scalar We = BDPTCameraUtilities::Importance( camera, camRay );
		if( We <= 0 ) {
			return result;
		}

		Scalar fLightNM = 1.0;
		if( lightEnd.type == BDPTVertex::SURFACE && lightEnd.pMaterial && s >= 2 ) {
			Vector3 wiAtLight = Vector3Ops::mkVector3( lightVerts[s - 2].position, lightEnd.position );
			wiAtLight = Vector3Ops::Normalize( wiAtLight );
			fLightNM = PathValueOps::EvalAreaBSDFAtVertex<Tag>( lightEnd, wiAtLight, dirToCam, tag );
		}

		const Scalar distSq = dist * dist;
		const Scalar absCosLight = lightEnd.isDelta ?
			Scalar(1.0) : fabs( Vector3Ops::Dot( lightEnd.geomNormal, dirToCam ) );
		const Scalar G = absCosLight / distSq;

		result.contribution = VertexThroughput<Tag>( lightEnd ) * fLightNM * G * We;
		result.rasterPos = rasterPos;
		result.needsSplat = true;
		result.valid = true;

		// --- Update pdfRev at light endpoint for correct MIS ---
		const Scalar savedLightPdfRev = lightEnd.pdfRev;
		{
			Ray camRayToLight( camPos, -dirToCam );
			const Scalar camPdfDir = BDPTCameraUtilities::PdfDirection( camera, camRayToLight );
			const_cast<BDPTVertex&>( lightEnd ).pdfRev =
				BDPTUtilities::SolidAngleToArea( camPdfDir, absCosLight, distSq );
		}

		// --- Update predecessor pdfRev (lightVerts[s-2]) ---
		Scalar savedLightPredPdfRevNM_t0 = 0;
		const bool hasLightPredNM_t0 = (s >= 2 && lightEnd.pMaterial);
		if( hasLightPredNM_t0 )
		{
			const BDPTVertex& lightPred = lightVerts[s - 2];
			savedLightPredPdfRevNM_t0 = lightPred.pdfRev;

			Vector3 wiAtLightEnd = Vector3Ops::mkVector3(
				lightVerts[s - 2].position, lightEnd.position );
			wiAtLightEnd = Vector3Ops::Normalize( wiAtLightEnd );

			const Scalar pdfPredSA = PathValueOps::EvalPdfAtVertex<Tag>( lightEnd, dirToCam, wiAtLightEnd, tag );
			const Vector3 dToPred = Vector3Ops::mkVector3( lightPred.position, lightEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			const Scalar absCosAtPred = fabs( Vector3Ops::Dot( lightPred.geomNormal,
				Vector3Ops::Normalize( dToPred ) ) );
			const_cast<BDPTVertex&>( lightPred ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfPredSA, absCosAtPred, distPredSq );
		}

		result.misWeight = self.MISWeight( lightVerts, eyeVerts, s, t );
		const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRev;
		if( hasLightPredNM_t0 ) {
			const_cast<BDPTVertex&>( lightVerts[s - 2] ).pdfRev = savedLightPredPdfRevNM_t0;
		}
		return result;
		}
	}

	//
	// Case: s == 1, t > 0
	// Connect the last eye vertex to a new light sample (next event estimation)
	//
	if( s == 1 )
	{
		const BDPTVertex& eyeEnd = eyeVerts[t - 1];
		const BDPTVertex& lightStart = lightVerts[0];

		// Eye endpoint must be a connectable surface or medium vertex
		if( eyeEnd.type != BDPTVertex::SURFACE && eyeEnd.type != BDPTVertex::MEDIUM ) {
			return result;
		}
		if( eyeEnd.type == BDPTVertex::SURFACE && !eyeEnd.pMaterial ) {
			return result;
		}

		if( !eyeEnd.isConnectible ) {
			return result;
		}

		const bool eyeIsMedium_s1 = (eyeEnd.type == BDPTVertex::MEDIUM);

		// Direction from eye vertex to light vertex (disc point for env).
		// Used for visibility on explicit lights and for the
		// MIS-bookkeeping geometric term throughout.
		Vector3 dirToLight = Vector3Ops::mkVector3( lightStart.position, eyeEnd.position );
		const Scalar dist = Vector3Ops::Magnitude( dirToLight );
		if( dist < BDPT_RAY_EPSILON ) {
			return result;
		}
		dirToLight = dirToLight * (1.0 / dist);

		// For env-light, the SAMPLED sky direction is wi, stored as
		// `-lightStart.geomNormal` (LightSampler::SampleEnvLightEmission
		// sets `sample.normal = -wi`).  The disc-position-derived
		// `dirToLight` only approximates wi (off by the disc-offset
		// geometry) — using dirToLight for env lookup / BSDF eval /
		// PDF / visibility produces systematic wrong-color and
		// wrong-energy on non-uniform HDRIs (adversarial review P1a,
		// 2026-05-25).  Switch every env-evaluation to wi.
		Vector3 wiForLight = dirToLight;
		const bool envCase_s1 = ( lightStart.pEnvLight != 0 );
		if( envCase_s1 ) {
			wiForLight = Vector3(
				-lightStart.geomNormal.x,
				-lightStart.geomNormal.y,
				-lightStart.geomNormal.z );
		}

		// Visibility: for explicit lights, segment to the light surface.
		// For env, infinite ray in wi from eye — synthesised as a far-
		// distance point to reuse the ConnectionIsVisible(p,q) helper.
		{
			Point3 visTarget = lightStart.position;
			if( envCase_s1 ) {
				const Scalar kVisFar = Scalar( 1.0e6 );
				visTarget = Point3(
					eyeEnd.position.x + wiForLight.x * kVisFar,
					eyeEnd.position.y + wiForLight.y * kVisFar,
					eyeEnd.position.z + wiForLight.z * kVisFar );
			}
			if( !ConnectionIsVisible( caster, eyeEnd.position, visTarget ) ) {
				return result;
			}
		}

		// Evaluate emitted radiance from the light toward the eye vertex
		V Le = Traits::zero();
		if constexpr( Traits::is_pel ) {
			// PEL emitter-resolution order: pLight -> pLuminary -> env.
			if( lightStart.pLight ) {
				Le = LightRadiance<Tag>( lightStart.pLight, -dirToLight, tag );
			} else if( lightStart.pLuminary && lightStart.pLuminary->GetMaterial() ) {
				Le = LuminaryRadiance<Tag>( lightStart, -dirToLight, tag );
			} else if( envCase_s1 ) {
				Ray skyProbe( eyeEnd.position, wiForLight );
				Le = EnvRadiance<Tag>( lightStart.pEnvLight, skyProbe, tag );
			}
		} else {
			// NM emitter-resolution order: pLuminary -> pLight(luminance) -> env
			// (preserved DIVERGENCE vs Pel branch order; ILight has no NM virtual,
			// so pLight projects via Rec.709 luminance inside LightRadiance<NMTag>).
			if( lightStart.pLuminary && lightStart.pLuminary->GetMaterial() ) {
				Le = LuminaryRadiance<Tag>( lightStart, -dirToLight, tag );
			} else if( lightStart.pLight ) {
				Le = LightRadiance<Tag>( lightStart.pLight, -dirToLight, tag );
			} else if( envCase_s1 ) {
				Ray skyProbe( eyeEnd.position, wiForLight );
				Le = EnvRadiance<Tag>( lightStart.pEnvLight, skyProbe, tag );
			}
		}

		if( PositiveMagnitude<Tag>( Le ) <= 0 ) {
			return result;
		}

		// Evaluate BSDF (or phase function for medium vertices) at the eye endpoint
		Vector3 woAtEye;
		if( t >= 2 ) {
			woAtEye = Vector3Ops::mkVector3( eyeVerts[t - 2].position, eyeEnd.position );
			woAtEye = Vector3Ops::Normalize( woAtEye );
		} else {
			// t == 1 means connecting camera directly to light, handled by t==0 case above
			return result;
		}

		// BSDF eval in the actually-sampled direction (wi for env,
		// dirToLight for explicit lights).
		const V fEye = PathValueOps::EvalAreaBSDFAtVertex<Tag>( eyeEnd, wiForLight, woAtEye, tag );

		if( PositiveMagnitude<Tag>( fEye ) <= 0 ) {
			return result;
		}

		// Geometric term
		// For delta-position lights (point/spot), the light has no surface
		// so the geometric coupling excludes the cosine at the light vertex.
		// For medium eye vertices, the eye-side cosine is also excluded.
		Scalar G;
		if( lightStart.isDelta ) {
			const Scalar dist2 = dist * dist;
			if( eyeIsMedium_s1 ) {
				// delta light <-> medium: 1/dist^2 (no cosine on either side)
				G = 1.0 / dist2;
			} else {
				const Scalar absCosEye = fabs( Vector3Ops::Dot( eyeEnd.geomNormal, dirToLight ) );
				G = absCosEye / dist2;
			}
		} else {
			if( eyeIsMedium_s1 ) {
				// area light <-> medium: |cos_light| / dist^2
				G = BDPTUtilities::GeometricTermSurfaceMedium( lightStart.position, lightStart.geomNormal, eyeEnd.position );
			} else {
				G = BDPTUtilities::GeometricTerm( lightStart.position, lightStart.geomNormal, eyeEnd.position, eyeEnd.geomNormal );
			}
		}

		// Connection transmittance through participating media.
		// For env-light: evaluate along the SAMPLED wi from eye to
		// RISE_INFINITY via the Ray+maxDist overload, matching PT's
		// env-NEE convention (LightSampler::EvaluateDirectLighting
		// at LightSampler.cpp ~line 1662).  Using the Point3+Point3
		// API with a constructed "far" endpoint would either (a)
		// overflow Magnitude() if the endpoint is at RISE_INFINITY,
		// or (b) under-attenuate ultra-thin media if a finite distance
		// like 1e10 is used (a medium with σ_t = 1e-12 still gives
		// non-trivial transmittance change at infinity that 1e10 caps
		// off).  The Ray+maxDist overload accepts RISE_INFINITY
		// directly and the medium walk handles it correctly
		// (Beer-Lambert exp(-σ·∞) → 0 for any σ > 0; vacuum → 1).
		// Adversarial review round 5, P2.
		V Tr_conn_s1;
		if( envCase_s1 ) {
			Ray envRay( eyeEnd.position, wiForLight );
			Tr_conn_s1 = EvalConnTr<Tag>( self, envRay, RISE_INFINITY, scene, caster,
				eyeEnd.pMediumObject, eyeEnd.pMediumVol, tag );
		} else {
			Tr_conn_s1 = EvalConnTr<Tag>( self, eyeEnd.position, lightStart.position, scene, caster,
				eyeEnd.pMediumObject, eyeEnd.pMediumVol, tag );
		}

		// Contribution: eyeThroughput * fEye * G * Le / pdfLight
		// VertexThroughput<Tag>( lightStart ) already has Le / (pdfSelect * pdfPosition)
		// but we need to re-evaluate since the direction changed
		// For s=1, we use: lightVerts[0].throughput * fEye * G
		// where lightVerts[0].throughput = Le / (pdfSelect * pdfPosition)

		// Actually for s=1, the light vertex stores throughput = Le / pdf_pos_select
		// We just need fEye * G * lightThroughput * eyeThroughput
		// But Le from light depends on the connection direction, which differs
		// from the sampled direction.  Re-evaluate:
		Scalar pdfLight = lightStart.pdfFwd;
		if( pdfLight <= 0 ) {
			return result;
		}

		// Env-light: bypass the disc-based G + pdfLight formula
		// entirely and use the standard PT env-NEE formula directly
		// with the SAMPLED wi.  Algebraic equivalent of the old
		// `pdfLight = pdfSA / dist²` override under cos_light ≈ 1,
		// but with three improvements over the previous code:
		//   1. Le, fEye, cos_eye, and pdfSA all evaluated at wi
		//      (not the approximate dirToLight), so HDRIs sample
		//      the correct env region — fixes the off-center /
		//      non-uniform-env wrong-color bug (adversarial P1a).
		//   2. cos_eye uses |dot(geomNormal, wi)| — same direction
		//      as the BSDF eval, so the cosine factor is consistent
		//      with the radiance estimate.
		//   3. Skips the G computation, dist²-cancel dance, and
		//      avoids the unnecessary dist-based numerical wobble
		//      on disc placements far from the eye.
		// MIS weight bookkeeping still uses the disc-area pdfs
		// (slightly suboptimal variance, documented in
		// docs/IMPROVEMENTS.md #12 as the PBRT-v4 SA-MIS follow-up).
		if( envCase_s1 ) {
			// Env-light path: bypass disc-based G/pdfLight, use the
			// standard PT env-NEE formula with the SAMPLED wi.  See
			// the BSDF-eval site above for the P1a rationale.
			// MIS weight: still computed via MISWeight() below using
			// the disc-area pdfRev/pdfFwd bookkeeping — a PT-style
			// power-2 override here was tested broken on spectral
			// BDPT (the override doesn't compose cleanly with the
			// remaining s>=2 strategies that still use disc-area
			// weights; RGB ended at 85% but NM collapsed to 31% of
			// PT).  The disc-area MIS leaves a documented ~15-22%
			// bias (docs/IMPROVEMENTS.md #12) but stays internally
			// consistent across all (s, t) strategies and both
			// integrators.
			const EnvironmentSampler* pEnvSamp =
				pLightSampler ? pLightSampler->GetEnvironmentSampler() : 0;
			if( !pEnvSamp ) {
				return result;
			}
			const Scalar pdfSA = pEnvSamp->Pdf( wiForLight );
			if( pdfSA <= 0 ) {
				return result;
			}
			const Scalar cosEyeWi = eyeIsMedium_s1 ? Scalar( 1.0 )
				: fabs( Vector3Ops::Dot( eyeEnd.geomNormal, wiForLight ) );
			// Continuous-PMF env-NEE rescale (2026-05-29 follow-up).
			// Env-NEE is invoked at rate `EnvSelectProbability()` per
			// SampleLight call (Session 9 wrapper).  Each successful
			// env-NEE sample contributes `Le · bsdf · cos /
			// (pdf_env_sa · pdfSelect)` for the importance-sampled
			// estimator to be unbiased — the prior `/ pdfSA` formula
			// assumed pdfSelect = 1 (env-only scenes); in mixed env +
			// other-light scenes pdfSelect = cachedEnvSelectProb < 1,
			// and the missing 1/pdfSelect factor caused env-NEE to
			// under-contribute.  Equivalent to dividing by
			// `lightStart.pdfFwd × πr²` since
			// `lightStart.pdfFwd = pdfSelect × pdfPos_disc =
			// pdfSelect / (πr²)`.  Using EnvSelectProbability()
			// directly keeps the dependency clear.  Mirrored at the
			// VCM twin (VCMIntegrator.cpp env-NEE branch).
			const Scalar envSelP_s1 =
				pLightSampler->EnvSelectProbability();
			const Scalar invEnvSel = ( envSelP_s1 > 0 ) ?
				( Scalar( 1 ) / envSelP_s1 ) : Scalar( 0 );
			const V contribEnv =
				VertexThroughput<Tag>( eyeEnd ) * fEye * Le * Tr_conn_s1 *
				(cosEyeWi / pdfSA) * invEnvSel;
			result.contribution = contribEnv;
			result.needsSplat = false;
			result.valid = true;
			if constexpr( Traits::is_pel ) {
			result.guidingLocalContribution =
				fEye * Le * Tr_conn_s1 * (cosEyeWi / pdfSA) * invEnvSel;
			result.guidingEyeVertexIndex = t - 1;
			result.guidingValid = true;
			}
		} else {
			result.contribution = VertexThroughput<Tag>( eyeEnd ) * fEye * Le * Tr_conn_s1 * (G / pdfLight);
			result.needsSplat = false;
			result.valid = true;
			if constexpr( Traits::is_pel ) {
			result.guidingLocalContribution = fEye * Le * Tr_conn_s1 * (G / pdfLight);
			result.guidingEyeVertexIndex = t - 1;
			result.guidingValid = true;
			}
		}

		// --- Update pdfRev at connection vertices for correct MIS ---
		const Scalar distSq_conn = dist * dist;
		const Scalar savedLightPdfRev = lightStart.pdfRev;
		const Scalar savedEyePdfRev = eyeEnd.pdfRev;

		// MIS bookkeeping direction: for env-light, use the SAMPLED
		// wi for pdfRev computations too — the contribution was
		// already redirected to wi (P1a fix).  Mixing dirToLight
		// for MIS denominators with wi for the numerator means the
		// MIS weight is computed for a slightly different path than
		// the contribution, biasing the result on non-uniform HDRIs
		// (adversarial review round 2, P1).
		const Vector3 dirForMIS_s1 = envCase_s1 ? wiForLight : dirToLight;

		// lightStart.pdfRev: PDF that eye-side process would "find" the light
		// For delta-position lights (point/spot), leave at 0 (eye can't hit a point)
		if( !lightStart.isDelta ) {
			const Scalar pdfRevSA = PathValueOps::EvalPdfAtVertex<Tag>( eyeEnd, woAtEye, dirForMIS_s1, tag );
			const Scalar absCosAtLight = fabs( Vector3Ops::Dot( lightStart.geomNormal, dirForMIS_s1 ) );
			const_cast<BDPTVertex&>( lightStart ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfRevSA, absCosAtLight, distSq_conn );
		}

		// eyeEnd.pdfRev: PDF that light-side would generate eyeEnd
		// = emission directional PDF at light toward eyeEnd, converted to area at eyeEnd
		{
			Scalar emissionPdfDir = 0;
			if( lightStart.pLuminary ) {
				// Mesh luminary: cosine-weighted emission, halved per face
				// for a double-sided emitter (DL-320) -- the same function
				// `LightSampler::SampleLight` draws from.
				emissionPdfDir = EmitterSides::CosineEmissionPdf(
					LightSampler::LuminaryIsTwoSided( lightStart.pLuminary ),
					lightStart.geomNormal, -dirToLight );
			} else if( lightStart.pLight ) {
				emissionPdfDir = lightStart.pLight->pdfDirection( -dirToLight );
			} else if( envCase_s1 ) {
				// Env-light: emission direction from disc = -wi.
				// Query env sampler at wiForLight (= -geomNormal) —
				// matches the wi used everywhere else for env.
				const EnvironmentSampler* pEnvSamp =
					pLightSampler ? pLightSampler->GetEnvironmentSampler() : 0;
				if( pEnvSamp ) {
					emissionPdfDir = pEnvSamp->Pdf( wiForLight );
				}
			}
			// Medium vertices: sigma_t/dist^2 replaces |cos|/dist^2
			if( eyeIsMedium_s1 ) {
				const_cast<BDPTVertex&>( eyeEnd ).pdfRev =
					BDPTUtilities::SolidAngleToAreaMedium( emissionPdfDir, eyeEnd.sigma_t_scalar, distSq_conn );
			} else {
				const Scalar absCosAtEye = fabs( Vector3Ops::Dot( eyeEnd.geomNormal, dirForMIS_s1 ) );
				const_cast<BDPTVertex&>( eyeEnd ).pdfRev =
					BDPTUtilities::SolidAngleToArea( emissionPdfDir, absCosAtEye, distSq_conn );
			}
		}

		// --- Update predecessor pdfRev (eyeVerts[t-2]) ---
		// PDF at eyeEnd of scattering toward eyeVerts[t-2] given incoming = dirToLight
		Scalar savedEyePredPdfRev = 0;
		const bool hasEyePred_s1 = (t >= 2);
		if( hasEyePred_s1 )
		{
			const BDPTVertex& eyePred = eyeVerts[t - 2];
			savedEyePredPdfRev = eyePred.pdfRev;

			const Vector3 dToPred = Vector3Ops::mkVector3( eyePred.position, eyeEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			const Vector3 dirToPred = Vector3Ops::Normalize( dToPred );
			// For env-light, the incoming direction at eyeEnd that the
			// light strategy would scatter back from was sampled wi
			// (not the disc-derived dirToLight) — use dirForMIS_s1
			// to keep the predecessor pdf consistent with the
			// contribution direction.
			const Scalar pdfPredSA = PathValueOps::EvalPdfAtVertex<Tag>( eyeEnd, dirForMIS_s1, dirToPred, tag );
			// Camera vertex: use 1.0; Medium vertex: sigma_t/dist^2
			if( eyePred.type == BDPTVertex::CAMERA ) {
				const_cast<BDPTVertex&>( eyePred ).pdfRev =
					BDPTUtilities::SolidAngleToArea( pdfPredSA, Scalar(1.0), distPredSq );
			} else if( eyePred.type == BDPTVertex::MEDIUM ) {
				const_cast<BDPTVertex&>( eyePred ).pdfRev =
					BDPTUtilities::SolidAngleToAreaMedium( pdfPredSA, eyePred.sigma_t_scalar, distPredSq );
			} else {
				const Scalar absCosAtPred =
					fabs( Vector3Ops::Dot( eyePred.geomNormal, dirToPred ) );
				const_cast<BDPTVertex&>( eyePred ).pdfRev =
					BDPTUtilities::SolidAngleToArea( pdfPredSA, absCosAtPred, distPredSq );
			}
		}

		result.misWeight = self.MISWeight( lightVerts, eyeVerts, s, t );

		const_cast<BDPTVertex&>( lightStart ).pdfRev = savedLightPdfRev;
		const_cast<BDPTVertex&>( eyeEnd ).pdfRev = savedEyePdfRev;
		if( hasEyePred_s1 ) {
			const_cast<BDPTVertex&>( eyeVerts[t - 2] ).pdfRev = savedEyePredPdfRev;
		}

		return result;
	}

	//
	// Case: s > 0, t == 1
	// Connect last light vertex to the camera
	//
	if( t == 1 )
	{
		// Delta-DIRECTION camera (orthographic): the light-tracing
		// strategy cannot scatter a non-specular light vertex into the
		// camera's single parallel direction (zero density), so skip it.
		// The camera vertex is marked isDelta in GenerateEyeSubpath so
		// MISWeight's eye-side walk excludes this (phantom) strategy from
		// every other strategy's denominator — keeping the partition of
		// unity consistent.  Without this skip the orthographic t==1
		// splat both misdirects energy and steals ~all the MIS weight from
		// the eye-path NEE / interior strategies (BDPT renders near-black).
		if( BDPTCameraUtilities::IsDeltaDirection( camera ) ) {
			return result;
		}

		const BDPTVertex& lightEnd = lightVerts[s - 1];

		if( !lightEnd.isConnectible ) {
			return result;
		}

		// Only connect surface or medium vertices to camera
		if( s >= 2 && lightEnd.type != BDPTVertex::SURFACE && lightEnd.type != BDPTVertex::MEDIUM ) {
			return result;
		}

		// Debt 28 -- FINITE-APERTURE cameras.  The camera path vertex
		// for this strategy is a point on the entrance APERTURE, drawn
		// with the same shape and density the primary rays use.  For a
		// pinhole / fisheye that point IS `camera.GetLocation()` and
		// everything below is unchanged; for a thin lens it is a
		// sampled lens point, and:
		//   - the light vertex is rasterized THROUGH it (which is what
		//     gives the splat layer the eye layer's depth of field),
		//   - the shadow ray, `dirToCam` and `dist` all use it,
		//   - `Importance` below already folds in 1 / (its area
		//     density) and the aperture cosine, so the contribution
		//     formula needs no extra factor (see CameraUtilities.h).
		// MIS is untouched: the aperture-positional density is the same
		// under every strategy, so it cancels out of every pdf ratio --
		// PBRT-v4's MISWeight likewise never reads the camera vertex's
		// own pdfFwd.
		const BDPTCameraUtilities::ApertureSample apertureSample =
			BDPTCameraUtilities::SampleAperture( camera, cameraLensSample );

		// Project light vertex onto camera
		Point2 rasterPos;
		if( !BDPTCameraUtilities::RasterizeThrough(
				camera, lightEnd.position, apertureSample, rasterPos ) ) {
			return result;
		}

		// This strategy is only valid for a direct camera connection.
		// Refractive blockers must be sampled as explicit specular eye
		// vertices; treating them as transparent here produces invalid
		// splats and severe caustic fireflies.
		const Point3 camPos = apertureSample.point;
		if( !ConnectionIsVisible( caster, lightEnd.position, camPos ) ) {
			return result;
		}

		// Direction from light vertex to camera
		Vector3 dirToCam = Vector3Ops::mkVector3( camPos, lightEnd.position );
		const Scalar dist = Vector3Ops::Magnitude( dirToCam );
		if( dist < BDPT_RAY_EPSILON ) {
			return result;
		}
		dirToCam = dirToCam * (1.0 / dist);

		// Camera importance
		Ray camRay( camPos, -dirToCam );
		const Scalar We = BDPTCameraUtilities::Importance( camera, camRay );
		if( We <= 0 ) {
			return result;
		}

		// Evaluate BSDF (or phase function) at the light endpoint for connection to camera
		const bool lightIsMedium_t1 = (lightEnd.type == BDPTVertex::MEDIUM);
		V fLight = TrOne<Tag>();
		if( (lightEnd.type == BDPTVertex::SURFACE && lightEnd.pMaterial) || lightIsMedium_t1 ) {
			Vector3 wiAtLight;
			if( s >= 2 ) {
				wiAtLight = Vector3Ops::mkVector3( lightVerts[s - 2].position, lightEnd.position );
				wiAtLight = Vector3Ops::Normalize( wiAtLight );
			}

			if( s >= 2 ) {
				fLight = PathValueOps::EvalAreaBSDFAtVertex<Tag>( lightEnd, wiAtLight, dirToCam, tag );
			}
		} else if( lightEnd.type == BDPTVertex::LIGHT ) {
			// s == 1: the light source directly connects to the camera.
			// Radiance toward camera.  DIVERGENCE (preserved): Pel resolves
			// pLight -> pLuminary and treats the degenerate env disc via the
			// trailing else-return; NM checks pEnvLight first, then
			// pLuminary -> pLight(luminance).  Each light kind is mutually
			// exclusive so the order is inert; reproduced per tag.  Env-disc
			// is degenerate (parallel-ray emitter authorised only in -wi);
			// matches PBRT-v3/v4 dropping this BDPT strategy for infinite lights.
			V LeToCam = Traits::zero();
			if constexpr( Traits::is_pel ) {
				if( lightEnd.pLight ) {
					LeToCam = LightRadiance<Tag>( lightEnd.pLight, dirToCam, tag );
				} else if( lightEnd.pLuminary && lightEnd.pLuminary->GetMaterial() ) {
					// An emitterless luminary contributes no radiance, so LuminaryRadiance
					// returns zero (black) here -- matching the NM side below.  This was a
					// latent white-firefly: the Pel original left fLight at its (1,1,1) init
					// when a LIGHT-vertex luminary carried no emitter, splatting white where
					// zero is correct.  Unreachable today (a sampled mesh-luminary vertex
					// always carries an emitter) -- defensive zero for any future
					// emitterless luminary type.
					LeToCam = LuminaryRadiance<Tag>( lightEnd, dirToCam, tag );
				} else {
					return result;
				}
			} else {
				if( lightEnd.pEnvLight ) {
					return result;
				}
				if( lightEnd.pLuminary && lightEnd.pLuminary->GetMaterial() ) {
					LeToCam = LuminaryRadiance<Tag>( lightEnd, dirToCam, tag );
				} else if( lightEnd.pLight ) {
					LeToCam = LightRadiance<Tag>( lightEnd.pLight, dirToCam, tag );
				}
			}

			const Scalar pdfLight = lightEnd.pdfFwd;
			if( pdfLight <= 0 ) {
				return result;
			}

			const Scalar distSq = dist * dist;
			const Scalar absCosLight = lightEnd.isDelta ?
				Scalar(1.0) : fabs( Vector3Ops::Dot( lightEnd.geomNormal, dirToCam ) );
			const Scalar G = absCosLight / distSq;

			// Contribution association preserved per tag (Pel parenthesises the
			// G*We/pdf factor; NM chains it left-to-right) -- value-identical.
			if constexpr( Traits::is_pel ) {
				result.contribution = LeToCam * (G * We / pdfLight);
			} else {
				result.contribution = LeToCam * G * We / pdfLight;
			}
			result.rasterPos = rasterPos;
			result.needsSplat = true;
			result.valid = true;

			// --- Update pdfRev at connection vertices for correct MIS ---
			const Scalar savedLightPdfRev = lightEnd.pdfRev;
			const Scalar savedEyePdfRev = eyeVerts[0].pdfRev;

			{
				Ray camRayToLight( camPos, -dirToCam );
				const Scalar camPdfDir = BDPTCameraUtilities::PdfDirection( camera, camRayToLight );
				const_cast<BDPTVertex&>( lightEnd ).pdfRev =
					BDPTUtilities::SolidAngleToArea( camPdfDir, absCosLight, distSq );
			}
			{
				Scalar emPdfDir = 0;
				if( lightEnd.pLuminary ) {
					// DL-320: two-faced for a double-sided emitter, the same
					// density `LightSampler::SampleLight` draws from.
					emPdfDir = EmitterSides::CosineEmissionPdf(
						LightSampler::LuminaryIsTwoSided( lightEnd.pLuminary ),
						lightEnd.geomNormal, dirToCam );
				} else if( lightEnd.pLight ) {
					emPdfDir = lightEnd.pLight->pdfDirection( dirToCam );
				}
				const_cast<BDPTVertex&>( eyeVerts[0] ).pdfRev =
					BDPTUtilities::SolidAngleToArea( emPdfDir, Scalar(1.0), distSq );
			}

			result.misWeight = self.MISWeight( lightVerts, eyeVerts, s, t );
			const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRev;
			const_cast<BDPTVertex&>( eyeVerts[0] ).pdfRev = savedEyePdfRev;
			return result;
		}

		if( PositiveMagnitude<Tag>( fLight ) <= 0 ) {
			return result;
		}

		// Geometric term (camera has no surface normal, use 1/dist^2)
		// For medium light endpoints, there's also no surface cosine:
		// medium <-> camera: 1/dist^2
		const Scalar distSq = dist * dist;
		const Scalar absCosLight = lightIsMedium_t1 ?
			Scalar(1.0) : fabs( Vector3Ops::Dot( lightEnd.geomNormal, dirToCam ) );
		const Scalar G = absCosLight / distSq;

		// Connection transmittance through participating media
		const V Tr_conn_t1 = EvalConnTr<Tag>( self, lightEnd.position, camPos, scene, caster,
			lightEnd.pMediumObject, lightEnd.pMediumVol, tag );

		result.contribution = VertexThroughput<Tag>( lightEnd ) * fLight * Tr_conn_t1 * (G * We);
		result.rasterPos = rasterPos;
		result.needsSplat = true;
		result.valid = true;

		// --- Update pdfRev at connection vertices for correct MIS ---
		const Scalar savedLightPdfRev = lightEnd.pdfRev;
		const Scalar savedEyePdfRev = eyeVerts[0].pdfRev;

		// lightEnd.pdfRev: camera's directional PDF at lightEnd
		// Medium vertices: sigma_t/dist^2 replaces |cos|/dist^2
		{
			Ray camRayToLight( camPos, -dirToCam );
			const Scalar camPdfDir = BDPTCameraUtilities::PdfDirection( camera, camRayToLight );
			if( lightIsMedium_t1 ) {
				const_cast<BDPTVertex&>( lightEnd ).pdfRev =
					BDPTUtilities::SolidAngleToAreaMedium( camPdfDir, lightEnd.sigma_t_scalar, distSq );
			} else {
				const_cast<BDPTVertex&>( lightEnd ).pdfRev =
					BDPTUtilities::SolidAngleToArea( camPdfDir, absCosLight, distSq );
			}
		}

		// eyeVerts[0].pdfRev: PDF at lightEnd of scattering toward camera
		if( s >= 2 && (lightEnd.pMaterial || lightIsMedium_t1) ) {
			Vector3 wiAtLightMIS = Vector3Ops::mkVector3(
				lightVerts[s - 2].position, lightEnd.position );
			wiAtLightMIS = Vector3Ops::Normalize( wiAtLightMIS );
			const Scalar pdfRevSA = PathValueOps::EvalPdfAtVertex<Tag>( lightEnd, wiAtLightMIS, dirToCam, tag );
			const_cast<BDPTVertex&>( eyeVerts[0] ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfRevSA, Scalar(1.0), distSq );
		}

		// --- Update predecessor pdfRev (lightVerts[s-2]) ---
		// PDF at lightEnd of scattering toward lightVerts[s-2] given incoming = dirToCam
		Scalar savedLightPredPdfRev_t1 = 0;
		const bool hasLightPred_t1 = (s >= 2 && (lightEnd.pMaterial || lightIsMedium_t1));
		if( hasLightPred_t1 )
		{
			const BDPTVertex& lightPred = lightVerts[s - 2];
			savedLightPredPdfRev_t1 = lightPred.pdfRev;

			Vector3 wiAtLightEnd = Vector3Ops::mkVector3(
				lightVerts[s - 2].position, lightEnd.position );
			wiAtLightEnd = Vector3Ops::Normalize( wiAtLightEnd );

			const Scalar pdfPredSA = PathValueOps::EvalPdfAtVertex<Tag>( lightEnd, dirToCam, wiAtLightEnd, tag );
			const Vector3 dToPred = Vector3Ops::mkVector3( lightPred.position, lightEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			// Medium predecessor: sigma_t/dist^2
			if( lightPred.type == BDPTVertex::MEDIUM ) {
				const_cast<BDPTVertex&>( lightPred ).pdfRev =
					BDPTUtilities::SolidAngleToAreaMedium( pdfPredSA, lightPred.sigma_t_scalar, distPredSq );
			} else {
				const Scalar absCosAtPred = fabs( Vector3Ops::Dot( lightPred.geomNormal,
					Vector3Ops::Normalize( dToPred ) ) );
				const_cast<BDPTVertex&>( lightPred ).pdfRev =
					BDPTUtilities::SolidAngleToArea( pdfPredSA, absCosAtPred, distPredSq );
			}
		}

		result.misWeight = self.MISWeight( lightVerts, eyeVerts, s, t );

		const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRev;
		const_cast<BDPTVertex&>( eyeVerts[0] ).pdfRev = savedEyePdfRev;
		if( hasLightPred_t1 ) {
			const_cast<BDPTVertex&>( lightVerts[s - 2] ).pdfRev = savedLightPredPdfRev_t1;
		}

		return result;
	}

	//
	// General case: s > 1, t > 1
	// Connect lightVerts[s-1] to eyeVerts[t-1]
	//
	{
		const BDPTVertex& lightEnd = lightVerts[s - 1];
		const BDPTVertex& eyeEnd = eyeVerts[t - 1];

		// Cannot connect at vertices with only delta lobes
		if( !lightEnd.isConnectible || !eyeEnd.isConnectible ) {
			return result;
		}

		// Both endpoints must be connectable surface or medium vertices.
		// Medium vertices have no material (phase function used instead).
		if( lightEnd.type != BDPTVertex::SURFACE && lightEnd.type != BDPTVertex::MEDIUM ) {
			return result;
		}
		if( eyeEnd.type != BDPTVertex::SURFACE && eyeEnd.type != BDPTVertex::MEDIUM ) {
			return result;
		}
		if( lightEnd.type == BDPTVertex::SURFACE && !lightEnd.pMaterial ) {
			return result;
		}
		if( eyeEnd.type == BDPTVertex::SURFACE && !eyeEnd.pMaterial ) {
			return result;
		}

		// Connection direction: from eye vertex to light vertex
		Vector3 dConnect = Vector3Ops::mkVector3( lightEnd.position, eyeEnd.position );
		const Scalar dist = Vector3Ops::Magnitude( dConnect );
		if( dist < BDPT_RAY_EPSILON ) {
			return result;
		}
		dConnect = dConnect * (1.0 / dist);

		// Check visibility
		if( !ConnectionIsVisible( caster, eyeEnd.position, lightEnd.position ) ) {
			return result;
		}

		// Evaluate BSDF at the light endpoint
		// wi at lightEnd = direction from previous light vertex
		Vector3 wiAtLight = Vector3Ops::mkVector3(
			lightVerts[s - 2].position, lightEnd.position );
		wiAtLight = Vector3Ops::Normalize( wiAtLight );

		// wo at lightEnd = direction toward eye vertex (connection)
		const Vector3 woAtLight = -dConnect;

		const V fLight = PathValueOps::EvalAreaBSDFAtVertex<Tag>( lightEnd, wiAtLight, woAtLight, tag );

		if( PositiveMagnitude<Tag>( fLight ) <= 0 ) {
			return result;
		}

		// Evaluate BSDF at the eye endpoint
		// wo at eyeEnd = direction toward previous eye vertex
		Vector3 woAtEye = Vector3Ops::mkVector3(
			eyeVerts[t - 2].position, eyeEnd.position );
		woAtEye = Vector3Ops::Normalize( woAtEye );

		// wi at eyeEnd = connection direction (from light side)
		const Vector3 wiAtEye = dConnect;

		const V fEye = PathValueOps::EvalAreaBSDFAtVertex<Tag>( eyeEnd, wiAtEye, woAtEye, tag );

		if( PositiveMagnitude<Tag>( fEye ) <= 0 ) {
			return result;
		}

		// Geometric coupling term G(x <-> y):
		//   surface <-> surface:  |cos_x| * |cos_y| / dist^2
		//   surface <-> medium:   |cos_surface| / dist^2
		//   medium  <-> medium:   1 / dist^2
		// Medium vertices have no surface orientation, so no cosine
		// factor appears.  The 1/dist^2 term is the inverse-square law
		// for point-to-point radiance transport in free space.
		const bool lightIsMedium = (lightEnd.type == BDPTVertex::MEDIUM);
		const bool eyeIsMedium = (eyeEnd.type == BDPTVertex::MEDIUM);
		Scalar G;
		if( lightIsMedium && eyeIsMedium ) {
			G = BDPTUtilities::GeometricTermMediumMedium(
				lightEnd.position, eyeEnd.position );
		} else if( lightIsMedium ) {
			G = BDPTUtilities::GeometricTermSurfaceMedium( eyeEnd.position, eyeEnd.geomNormal, lightEnd.position );
		} else if( eyeIsMedium ) {
			G = BDPTUtilities::GeometricTermSurfaceMedium( lightEnd.position, lightEnd.geomNormal, eyeEnd.position );
		} else {
			G = BDPTUtilities::GeometricTerm( lightEnd.position, lightEnd.geomNormal, eyeEnd.position, eyeEnd.geomNormal );
		}

		if( G <= 0 ) {
			return result;
		}

		// Connection edge transmittance: the connection between light
		// and eye subpath endpoints passes through potentially multiple
		// media.  We evaluate Tr by walking the connection segment and
		// accumulating per-segment Beer-Lambert transmittance.
		// This Tr multiplies the connection contribution but is NOT
		// included in MIS PDFs (see note on transmittance cancellation
		// in the MISWeight documentation).
		const V Tr_conn = EvalConnTr<Tag>( self, eyeEnd.position, lightEnd.position, scene, caster,
			eyeEnd.pMediumObject, eyeEnd.pMediumVol, tag );

		// Full path contribution
			result.contribution = VertexThroughput<Tag>( lightEnd ) * fLight *
				BroadcastScalar<Tag>( G ) * Tr_conn * fEye * VertexThroughput<Tag>( eyeEnd );
			result.needsSplat = false;
			result.valid = true;
			if constexpr( Traits::is_pel ) {
			result.guidingLocalContribution =
				VertexThroughput<Tag>( lightEnd ) * fLight * BroadcastScalar<Tag>( G ) * Tr_conn * fEye;
			result.guidingEyeVertexIndex = t - 1;
			result.guidingValid = true;
			}

			// --- Update pdfRev at connection vertices for correct MIS ---
		// The connection introduces a new edge between lightEnd and eyeEnd.
		// pdfRev at each endpoint must reflect the probability of generating
		// the reverse direction through this connection edge, not the
		// direction from subpath generation.
		//
		// Transmittance along shared edges cancels in the MIS ratio walk.
		// Both forward and reverse sampling traverse the same geometric
		// edge with identical transmittance, so Tr factors appear in both
		// numerator and denominator of pdfRev/pdfFwd and cancel.
		// Therefore pdfFwd and pdfRev do NOT include Tr — only the
		// directional PDF and the area-measure conversion factor
		// (|cos|/dist^2 for surfaces, sigma_t/dist^2 for media).
		// Connection edge Tr is applied as a multiplicative factor on
		// the contribution, not in the MIS weight.
		const Scalar distSq_conn = dist * dist;

		const Scalar savedLightPdfRev = lightEnd.pdfRev;
		const Scalar savedEyePdfRev = eyeEnd.pdfRev;

		// lightEnd.pdfRev: PDF that eye-side process would generate lightEnd
		// = PDF at eyeEnd of scattering toward lightEnd, converted to area at lightEnd
		// For medium vertices: sigma_t/dist^2 replaces |cos|/dist^2
		{
			const Scalar pdfRevSA = PathValueOps::EvalPdfAtVertex<Tag>( eyeEnd, woAtEye, dConnect, tag );
			if( lightIsMedium ) {
				const_cast<BDPTVertex&>( lightEnd ).pdfRev =
					BDPTUtilities::SolidAngleToAreaMedium( pdfRevSA, lightEnd.sigma_t_scalar, distSq_conn );
			} else {
				const Scalar absCosAtLight = fabs( Vector3Ops::Dot( lightEnd.geomNormal, dConnect ) );
				const_cast<BDPTVertex&>( lightEnd ).pdfRev =
					BDPTUtilities::SolidAngleToArea( pdfRevSA, absCosAtLight, distSq_conn );
			}
		}

		// eyeEnd.pdfRev: PDF that light-side process would generate eyeEnd
		// = PDF at lightEnd of scattering toward eyeEnd, converted to area at eyeEnd
		{
			const Scalar pdfRevSA = PathValueOps::EvalPdfAtVertex<Tag>( lightEnd, wiAtLight, -dConnect, tag );
			if( eyeIsMedium ) {
				const_cast<BDPTVertex&>( eyeEnd ).pdfRev =
					BDPTUtilities::SolidAngleToAreaMedium( pdfRevSA, eyeEnd.sigma_t_scalar, distSq_conn );
			} else {
				const Scalar absCosAtEye = fabs( Vector3Ops::Dot( eyeEnd.geomNormal, dConnect ) );
				const_cast<BDPTVertex&>( eyeEnd ).pdfRev =
					BDPTUtilities::SolidAngleToArea( pdfRevSA, absCosAtEye, distSq_conn );
			}
		}

		// --- Update predecessor pdfRev at lightVerts[s-2] ---
		// The connection changed the outgoing direction at lightEnd, so the
		// reverse PDF at the predecessor must reflect scattering at lightEnd
		// from the connection direction (-dConnect) back toward the predecessor.
		Scalar savedLightPredPdfRev = 0;
		const bool hasLightPred = (s >= 2);
		if( hasLightPred )
		{
			const BDPTVertex& lightPred = lightVerts[s - 2];
			savedLightPredPdfRev = lightPred.pdfRev;

			// woAtLight already points toward the eye side; wiAtLight points toward the predecessor.
			const Scalar pdfPredSA = PathValueOps::EvalPdfAtVertex<Tag>( lightEnd, woAtLight, wiAtLight, tag );
			const Vector3 dToPred = Vector3Ops::mkVector3( lightPred.position, lightEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			if( lightPred.type == BDPTVertex::MEDIUM ) {
				const_cast<BDPTVertex&>( lightPred ).pdfRev =
					BDPTUtilities::SolidAngleToAreaMedium( pdfPredSA, lightPred.sigma_t_scalar, distPredSq );
			} else {
				const Scalar absCosAtPred = fabs( Vector3Ops::Dot( lightPred.geomNormal,
					Vector3Ops::Normalize( dToPred ) ) );
				const_cast<BDPTVertex&>( lightPred ).pdfRev =
					BDPTUtilities::SolidAngleToArea( pdfPredSA, absCosAtPred, distPredSq );
			}
		}

		// --- Update predecessor pdfRev at eyeVerts[t-2] ---
		// The connection changed the outgoing direction at eyeEnd, so the
		// reverse PDF at the predecessor must reflect scattering at eyeEnd
		// from the connection direction (dConnect) back toward the predecessor.
		Scalar savedEyePredPdfRev = 0;
		const bool hasEyePred = (t >= 2);
		if( hasEyePred )
		{
			const BDPTVertex& eyePred = eyeVerts[t - 2];
			savedEyePredPdfRev = eyePred.pdfRev;

			// wiAtEye = dConnect (from light side), woAtEye = toward pred (away from vertex)
			const Scalar pdfPredSA = PathValueOps::EvalPdfAtVertex<Tag>( eyeEnd, wiAtEye, woAtEye, tag );
			const Vector3 dToPred = Vector3Ops::mkVector3( eyePred.position, eyeEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			// Camera vertex has no meaningful surface normal; use 1.0
			// Medium vertices use sigma_t/dist^2 instead of |cos|/dist^2
			if( eyePred.type == BDPTVertex::CAMERA ) {
				const_cast<BDPTVertex&>( eyePred ).pdfRev =
					BDPTUtilities::SolidAngleToArea( pdfPredSA, Scalar(1.0), distPredSq );
			} else if( eyePred.type == BDPTVertex::MEDIUM ) {
				const_cast<BDPTVertex&>( eyePred ).pdfRev =
					BDPTUtilities::SolidAngleToAreaMedium( pdfPredSA, eyePred.sigma_t_scalar, distPredSq );
			} else {
				const Scalar absCosAtPred =
					fabs( Vector3Ops::Dot( eyePred.geomNormal, Vector3Ops::Normalize( dToPred ) ) );
				const_cast<BDPTVertex&>( eyePred ).pdfRev =
					BDPTUtilities::SolidAngleToArea( pdfPredSA, absCosAtPred, distPredSq );
			}
		}

		result.misWeight = self.MISWeight( lightVerts, eyeVerts, s, t );

		// Restore original values
		const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRev;
		const_cast<BDPTVertex&>( eyeEnd ).pdfRev = savedEyePdfRev;
		if( hasLightPred ) {
			const_cast<BDPTVertex&>( lightVerts[s - 2] ).pdfRev = savedLightPredPdfRev;
		}
		if( hasEyePred ) {
			const_cast<BDPTVertex&>( eyeVerts[t - 2] ).pdfRev = savedEyePredPdfRev;
		}

		return result;
	}
}

//! DL-09 (docs/DL09_GRADED_INDEX_INTERIOR_FACTOR.md §3(iv)): every
//! CONNECTION strategy (s >= 1 and t >= 1 -- s==1 light-endpoint NEE, t==1
//! camera splat, the general case) builds one straight segment between the
//! eye endpoint and the light endpoint.  When both endpoints recorded the
//! SAME graded-index medium, that segment carries (n_eye/n_light)^2 --
//! the factor the eye walk would have paid had it traced that segment
//! itself, and the one the light walk pays in its own order.  Applied here,
//! once, around the one function every caller (BDPT, MLT, the complete-path
//! strategy selector, both tags) reaches, so no strategy branch can miss it.
//! It is a THROUGHPUT factor and never enters a pdf, so the MIS weight
//! computed inside is untouched.  s == 0 (the eye walk hitting an emitter)
//! builds no connection: the walk's own Advance already priced it.
template<class Tag>
typename ConnectionResultFor<Tag>::type
ConnectAndEvaluateImpl(
	const BDPTIntegrator& self,
	const LightSampler* pLightSampler,
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<BDPTVertex>& eyeVerts,
	unsigned int s,
	unsigned int t,
	const IScene& scene,
	const IRayCaster& caster,
	const ICamera& camera,
	const Point2& cameraLensSample,
	Tag tag )
{
	typename ConnectionResultFor<Tag>::type result = ConnectAndEvaluateImplCore<Tag>(
		self, pLightSampler, lightVerts, eyeVerts, s, t, scene, caster, camera,
		cameraLensSample, tag );
	if( result.valid && s >= 1 && t >= 1 &&
		s <= lightVerts.size() && t <= eyeVerts.size() )
	{
		const BDPTVertex& lightEnd = lightVerts[s - 1];
		const BDPTVertex& eyeEnd = eyeVerts[t - 1];
		const Scalar g = GradedIndexMedium::ConnectionScale(
			eyeEnd.pGradedMedium, eyeEnd.gradedIOR,
			lightEnd.pGradedMedium, lightEnd.gradedIOR );
		if( g != Scalar( 1 ) ) {
			result.contribution = result.contribution * g;
		}
	}
	return result;
}

}  // anonymous namespace (ConnectAndEvaluate F3a)

BDPTIntegrator::ConnectionResult BDPTIntegrator::ConnectAndEvaluate(
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<BDPTVertex>& eyeVerts,
	unsigned int s,
	unsigned int t,
	const IScene& scene,
	const IRayCaster& caster,
	const ICamera& camera,
	const Point2& cameraLensSample
	) const
{
	return ConnectAndEvaluateImpl<PelTag>(
		*this, pLightSampler, lightVerts, eyeVerts, s, t, scene, caster, camera,
		cameraLensSample, PelTag{} );
}

//////////////////////////////////////////////////////////////////////
// EvaluateAllStrategiesImpl<Tag> -- Phase 2c family F3b.
//
// The (s,t) strategy-enumeration driver, templatized over PelTag/NMTag.
// Free function (not a member template) taking the BDPTIntegrator guiding
// state as parameters so BDPTIntegrator.h is untouched and the public
// EvaluateAllStrategies{,NM} members (consumed by BDPT/MLT rasterizers)
// stay byte-identical.  Per-(s,t) work dispatches to the public
// ConnectAndEvaluate{,NM} members via `self` (they resolve pLightSampler);
// the Pel-only OpenPGL complete-path strategy selection + the two Pel-only
// training records sit behind `if constexpr( Traits::is_pel )`.
//////////////////////////////////////////////////////////////////////

namespace {

// Per-(s,t) connection dispatch to the public member forwarders (which own
// pLightSampler).  Pel -> ConnectAndEvaluate; NM -> ConnectAndEvaluateNM.
template<class Tag>
typename ConnectionResultFor<Tag>::type
DispatchConnectAndEvaluate(
	const BDPTIntegrator& self,
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<BDPTVertex>& eyeVerts,
	unsigned int s,
	unsigned int t,
	const IScene& scene,
	const IRayCaster& caster,
	const ICamera& camera,
	const Point2& cameraLensSample,
	Tag tag )
{
	if constexpr( SpectralValueTraits<Tag>::is_pel ) {
		(void)tag;
		return self.ConnectAndEvaluate( lightVerts, eyeVerts, s, t, scene, caster, camera, cameraLensSample );
	} else {
		return self.ConnectAndEvaluateNM( lightVerts, eyeVerts, s, t, scene, caster, camera, cameraLensSample, tag.nm );
	}
}

template<class Tag>
std::vector<typename ConnectionResultFor<Tag>::type>
EvaluateAllStrategiesImpl(
	const BDPTIntegrator& self,
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<BDPTVertex>& eyeVerts,
	const IScene& scene,
	const IRayCaster& caster,
	const ICamera& camera,
	const Point2& cameraLensSample,
	ISampler* pSampler,
#ifdef RISE_ENABLE_OPENPGL
	CompletePathGuide* pCompletePathGuide,
	bool completePathStrategySelectionEnabled,
	unsigned int completePathStrategySampleCount,
	std::atomic<unsigned long long>* pStrategySelectionPathCount,
	std::atomic<unsigned long long>* pStrategySelectionCandidateCount,
	std::atomic<unsigned long long>* pStrategySelectionEvaluatedCount,
	PathGuidingField* pGuidingField,
	BDPTIntegrator::GuidingTrainingStats* pGuidingTrainingStats,
	std::mutex* pGuidingTrainingStatsMutex,
	PathGuidingField* pLightGuidingField,
	unsigned int maxLightGuidingDepth,
#endif
	Tag tag )
{
	typedef SpectralValueTraits<Tag> Traits;
	typedef typename ConnectionResultFor<Tag>::type CR;
	const unsigned int nLight = static_cast<unsigned int>( lightVerts.size() );
	const unsigned int nEye = static_cast<unsigned int>( eyeVerts.size() );

	std::vector<CR> results;
	results.reserve( (nLight + 1) * (nEye + 1) );

	bool useCompletePathStrategySelection = false;
#ifdef RISE_ENABLE_OPENPGL
	if constexpr( Traits::is_pel ) {
		useCompletePathStrategySelection =
			pCompletePathGuide &&
			completePathStrategySelectionEnabled &&
			!pCompletePathGuide->IsCollectingTrainingSamples() &&
			pSampler &&
			completePathStrategySampleCount > 0;
	}
#endif

#ifdef RISE_ENABLE_OPENPGL
	if constexpr( Traits::is_pel )
	{
	if( useCompletePathStrategySelection )
	{
		static thread_local StrategySelectionScratch scratch;
		const size_t maxCandidates = static_cast<size_t>( (nLight + 1) * (nEye + 1) );
		if( scratch.reservedCandidates < maxCandidates ) {
			scratch.candidates.reserve( maxCandidates );
			scratch.learnedWeights.reserve( maxCandidates );
			scratch.cdf.reserve( maxCandidates );
			scratch.reservedCandidates = maxCandidates;
		}

		scratch.candidates.clear();
		scratch.learnedWeights.clear();
		scratch.cdf.clear();

		Scalar learnedWeightSum = 0;
		for( unsigned int t = 1; t <= nEye; t++ )
		{
			for( unsigned int s = 0; s <= nLight; s++ )
			{
				if( s + t < 2 ) {
					continue;
				}

				const unsigned int volBounces =
					(s > 0 ? lightVerts[s-1].volumeBounces : 0) +
					(t > 0 ? eyeVerts[t-1].volumeBounces : 0);
				if( volBounces > self.GetStabilityConfig().maxVolumeBounce ) {
					continue;
				}

				const Point3& eyePosition = eyeVerts[t - 1].position;
				const Point3* pLightPosition =
					(s > 0 && s <= nLight) ? &lightVerts[s - 1].position : 0;
				const Scalar learnedWeight =
					pCompletePathGuide->QueryStrategyWeight(
						s,
						t,
						eyePosition,
						pLightPosition,
						s == 0 ) *
					(GuidingWantsMultibounceTraining( s, t ) ? Scalar( 1.0 ) : Scalar( 0.25 ));

				scratch.candidates.push_back( StrategySelectionCandidate( s, t, 0 ) );
				scratch.learnedWeights.push_back( learnedWeight );
				learnedWeightSum += learnedWeight;
			}
		}

		const size_t candidateCount = scratch.candidates.size();
		if( candidateCount > 0 )
		{
			const Scalar uniformProbability = Scalar( 1.0 ) / static_cast<Scalar>( candidateCount );
			const Scalar guidedMix = learnedWeightSum > NEARZERO ? Scalar( 0.5 ) : Scalar( 0.0 );
			const Scalar uniformMix = Scalar( 1.0 ) - guidedMix;

			Scalar running = 0;
			for( size_t i = 0; i < candidateCount; i++ )
			{
				Scalar probability = uniformMix * uniformProbability;
				if( guidedMix > 0 ) {
					probability += guidedMix * (scratch.learnedWeights[i] / learnedWeightSum);
				}

				scratch.candidates[i].probability = probability;
				running += probability;
				scratch.cdf.push_back( running );
			}

			if( running > NEARZERO ) {
				scratch.cdf.back() = 1.0;
			}

			const unsigned int techniqueSamples =
				r_max( static_cast<unsigned int>( 1 ), completePathStrategySampleCount );

			pStrategySelectionPathCount->fetch_add( 1 );
			pStrategySelectionCandidateCount->fetch_add(
				static_cast<unsigned long long>( candidateCount ) );
			pStrategySelectionEvaluatedCount->fetch_add(
				static_cast<unsigned long long>( techniqueSamples ) );

			// Dedicated stream for (s,t) strategy choices so Sobol dimensions
			// for subpath construction stay stable.
			pSampler->StartStream( 47 );

			results.reserve( techniqueSamples );
			for( unsigned int i = 0; i < techniqueSamples; i++ )
			{
				const Scalar u = pSampler->Get1D();
				std::vector<Scalar>::const_iterator it =
					std::lower_bound( scratch.cdf.begin(), scratch.cdf.end(), u );
				const size_t candidateIndex =
					it != scratch.cdf.end() ?
						static_cast<size_t>( it - scratch.cdf.begin() ) :
						candidateCount - 1;
				const StrategySelectionCandidate& candidate =
					scratch.candidates[candidateIndex];

				CR cr = self.ConnectAndEvaluate(
					lightVerts,
					eyeVerts,
					candidate.s,
					candidate.t,
					scene,
					caster,
					camera,
					cameraLensSample );

				cr.s = candidate.s;
				cr.t = candidate.t;

				if( cr.valid && candidate.probability > NEARZERO )
				{
					cr.misWeight /=
						static_cast<Scalar>( techniqueSamples ) * candidate.probability;
					results.push_back( cr );
				}
			}
		}
	}
	}
#endif
	if( !useCompletePathStrategySelection )
	{
		// Iterate over all valid (s,t) combinations where s + t >= 2.
		for( unsigned int t = 1; t <= nEye; t++ )
		{
			for( unsigned int s = 0; s <= nLight; s++ )
			{
				if( s + t < 2 ) {
					continue;
				}

				const unsigned int volBounces =
					(s > 0 ? lightVerts[s-1].volumeBounces : 0) +
					(t > 0 ? eyeVerts[t-1].volumeBounces : 0);
				if( volBounces > self.GetStabilityConfig().maxVolumeBounce ) {
					continue;
				}

				CR cr = DispatchConnectAndEvaluate<Tag>(
					self, lightVerts, eyeVerts, s, t, scene, caster, camera, cameraLensSample, tag );
				if constexpr( Traits::is_pel ) {
					cr.s = s;
					cr.t = t;
				}

				if( cr.valid ) {
					results.push_back( cr );
				}
			}
		}
	}

	// ----------------------------------------------------------------
	// Deterministic evaluation of zero-exitance lights (directional,
	// ambient).  These lights have radiantExitance() == 0, so they are
	// excluded from the alias table and invisible to every (s,t)
	// strategy.  Since no strategy can produce their contribution, the
	// MIS weight is trivially 1.0 — this is the unique unbiased
	// estimator.  Mirrors Step 1 of LightSampler::EvaluateDirectLighting.
	// ----------------------------------------------------------------
	{
		const ILightManager* pLightMgr = scene.GetLights();
		if( pLightMgr )
		{
			const ILightManager::LightsList& lights = pLightMgr->getLights();
			for( ILightManager::LightsList::const_iterator m = lights.begin(),
				n = lights.end(); m != n; m++ )
			{
				const ILightPriv* l = *m;
				if( ColorMath::MaxValue( l->radiantExitance() ) > 0 ) {
					continue;
				}

				for( unsigned int t = 2; t <= nEye; t++ )
				{
					const BDPTVertex& eyeEnd = eyeVerts[t - 1];

					if( eyeEnd.volumeBounces > self.GetStabilityConfig().maxVolumeBounce ) continue;
					if( eyeEnd.type != BDPTVertex::SURFACE ) continue;
					if( !eyeEnd.pMaterial ) continue;

					// DL-207: a BSSRDF entry vertex (`isBSSRDFEntry`, spawned
					// by the eye subpath's own BSSRDF-sampling block above
					// when a `subsurfacescattering_material` /
					// `randomwalk_sss_material` transmits) must be priced
					// through its Sw(direction) diffusion term -- the SAME
					// adapter `PathVertexEval::EvalBSDFAtVertex` and PT's own
					// BSSRDF-entry NEE use -- never through the material's
					// raw aggregate `IBSDF`.  `SubSurfaceScatteringBSDF`
					// deliberately returns 0 off its own narrow front-
					// reflection lobe (see its own header comment), and at
					// this vertex `eyeEnd.position`/`eyeEnd.normal` are the
					// diffusion-profile ENTRY point, not the camera-visible
					// exit point that lobe is defined at -- so the raw
					// aggregate call was pricing the wrong physical
					// quantity at the wrong location, not merely evaluating
					// a legitimate lobe at zero.  Fed a directional/ambient
					// light delivered EXACTLY ZERO direct light to every
					// BSSRDF material under BDPT/MLT (docs/DL207_BDPT_ZERO_EXITANCE_BSSRDF.md).
					//
					// This branch deliberately BYPASSES the general
					// `isConnectible` gate below: a random-walk SSS entry
					// vertex is marked `isConnectible = false` so that the
					// GENERAL (s>=1) connection strategies -- which divide
					// by a real area-measure `pdfFwd`/`pdfRev` for MIS --
					// never target a vertex whose `pdfSurface` is only a
					// placeholder (see that vertex's own construction
					// comment, "Mark the vertex as delta + non-connectible").
					// THIS sweep's MIS weight is unconditionally 1.0 for
					// every zero-exitance light (the comment above this
					// block), so no pdf consistency is needed and the
					// exemption is safe -- it is the same reasoning that
					// makes a delta light's contribution here immune to
					// MIS in the first place.
					const IBSDF* pBSDF = nullptr;
					BSSRDFEntryBSDF diffusionEntryBSDF( nullptr, 0.0 );
					RandomWalkEntryBSDF randomWalkEntryBSDF( 1.0 );

					if( eyeEnd.isBSSRDFEntry )
					{
						if( ISubSurfaceDiffusionProfile* pProfile =
							eyeEnd.pMaterial->GetDiffusionProfile() )
						{
							diffusionEntryBSDF = BSSRDFEntryBSDF( pProfile, 0.0 );
							pBSDF = &diffusionEntryBSDF;
						}
						else
						{
							const RandomWalkSSSParams* pRW =
								eyeEnd.pMaterial->GetRandomWalkSSSParams();
							[[maybe_unused]] RandomWalkSSSParams rwParamsNM;
							if constexpr( !Traits::is_pel ) {
								if( !pRW && eyeEnd.pMaterial->GetRandomWalkSSSParamsNM(
									tag.nm, rwParamsNM ) ) {
									pRW = &rwParamsNM;
								}
							}
							if( pRW ) {
								randomWalkEntryBSDF = RandomWalkEntryBSDF( pRW->ior );
								pBSDF = &randomWalkEntryBSDF;
							}
						}
						if( !pBSDF ) continue;
					}
					else
					{
						if( !eyeEnd.isConnectible ) continue;
						pBSDF = eyeEnd.pMaterial->GetBSDF();
						if( !pBSDF ) continue;
					}

					// Incoming viewer direction (from previous eye vertex)
					Vector3 wo = Vector3Ops::mkVector3(
						eyeVerts[t - 2].position, eyeEnd.position );
					wo = Vector3Ops::Normalize( wo );

					// A BSSRDF's previous vertex is a nonlocal exit, not a local
					// viewer. Its entry adapter always supports the outward side.
					Ray evalRay = eyeEnd.isBSSRDFEntry
						? EntryEvaluationRay( eyeEnd.position, eyeEnd.normal )
						: Ray( eyeEnd.position, -wo );
					RayIntersectionGeometric ri( evalRay, nullRasterizerState );
					PathVertexEval::PopulateRIGFromVertex( eyeEnd, ri );

					const bool bReceivesShadows = eyeEnd.pObject
						? eyeEnd.pObject->DoesReceiveShadows() : true;

					// FULL-SPHERE NEE, DirectionalLight sibling (residual
					// wave 2 item D, 2026-08-27; docs/HAIR_FUR_DESIGN.md
					// section 4.1 / HairBSDF.h section 5's "KNOWN
					// REMAINING SIBLING" entry).  This s==1 row calls the
					// SAME ILight::ComputeDirectLighting virtual
					// LightSampler's Step 1 does, and inherits whatever
					// gate the concrete light applies -- 1472ae57's "BDPT
					// is already unconditionally full-sphere" audit was
					// about the s>=2 connection strategies' `fabs` in
					// `GeometricTerm`, not this zero-exitance sweep, so a
					// backlit hair groom under a `directional_light`
					// still lost this row's contribution before this
					// fix.  `eyeEnd.pMaterial` is guaranteed non-null by
					// the `continue` above.
					const bool bFullSphere = eyeEnd.pMaterial->ScattersFullSphere();

					// VOLUME RECEIVER (residual-ledger item 10 of
					// docs/PT_ENV_MIS_DOUBLECOUNT.md, 2026-08-27):
					// `bVolumeReceiver` is deliberately LEFT ON ITS
					// DEFAULT `false` at this site, and the corresponding
					// Step-1 medium-transmittance post-multiply is
					// deliberately NOT mirrored here.  Two reasons, both
					// checked rather than assumed: (1) this row can only
					// ever see a SURFACE vertex -- the `eyeEnd.type !=
					// BDPTVertex::SURFACE` continue above excludes MEDIUM
					// vertices outright -- so the flag would be false at
					// every reachable call anyway; (2) BDPT owns a
					// separate volume-NEE path, and reshaping its
					// zero-exitance sweep is out of scope for that item.
					// If this row is ever extended to MEDIUM vertices,
					// BOTH halves of the LightSampler Step-1 fix have to
					// come with it.  KNOWN DIVERGENCE (ledgered in
					// docs/PT_ENV_MIS_DOUBLECOUNT.md): PT's Step 1 now
					// post-multiplies directional-light medium attenuation
					// for a SURFACE in fog; this row does not, because
					// EvalShadowTransmittance is file-static in
					// LightSampler.cpp and lifting it trips the
					// five-build-project rule.  A directional light seen
					// through a bounded medium therefore reads brighter in
					// BDPT's s==1 row than in PT until that helper is
					// shared.  No in-tree BDPT scene pairs a directional
					// light with media today.
					// DL-157 P1: the vertex's own stack, rebuilt exactly as
					// `PathValueOps::EvalBSDFAtVertex` does -- this sweep
					// and the sampled-light arms must price a stateful BSDF
					// on the same SIDE.
					IORStack zeroExitStack( 1.0 );
					PathVertexEval::BuildVertexIORStack( eyeEnd, zeroExitStack );
					if constexpr( Traits::is_pel ) {
					RISEPel amount( 0, 0, 0 );
					l->ComputeDirectLighting( ri, caster, *pBSDF,
						bReceivesShadows, amount, bFullSphere, false, &zeroExitStack );

					if( ColorMath::MaxValue( amount ) > 0 )
					{
						CR cr;
						cr.contribution = eyeEnd.throughput * amount;
						cr.misWeight = 1.0;
						cr.needsSplat = false;
						cr.valid = true;
						cr.s = 1;
						cr.t = t;
						results.push_back( cr );
					}
					} else {
						// Per-NM direct lighting evaluation -- see
						// LightSampler::EvaluateDirectLightingNM site 1
						// for the rationale.  The previous flat-luminance
						// projection collapsed the surface's spectral
						// character; the per-NM virtual queries brdf.valueNM
						// at the connecting wavelength.
						const Scalar leNM = l->ComputeDirectLightingNM(
							ri, caster, *pBSDF, bReceivesShadows, tag.nm, bFullSphere,
							false, &zeroExitStack );
						if( leNM > 0 )
						{
							CR cr;
							cr.contribution = eyeEnd.throughputNM * leNM;
							cr.misWeight = 1.0;
							cr.needsSplat = false;
							cr.valid = true;
							cr.s = 1;
							results.push_back( cr );
						}
					}
				}
			}
		}
	}

#ifdef RISE_ENABLE_OPENPGL
	if constexpr( Traits::is_pel ) {
		if( pCompletePathGuide && pCompletePathGuide->IsCollectingTrainingSamples() ) {
			RecordCompletePathSamples( pCompletePathGuide, lightVerts, eyeVerts, results );
		}

		if( pGuidingField && pGuidingField->IsCollectingTrainingSamples() ) {
			RecordGuidingTrainingPath( pGuidingField, pGuidingTrainingStats, pGuidingTrainingStatsMutex, eyeVerts, results );
		}
	}
	if( pLightGuidingField && pLightGuidingField->IsCollectingTrainingSamples() ) {
		RecordGuidingTrainingLightPath( pLightGuidingField, lightVerts, maxLightGuidingDepth );
	}
#endif

	return results;
}

}  // anonymous namespace (EvaluateAllStrategies F3b)

//////////////////////////////////////////////////////////////////////
// EvaluateAllStrategies (Pel) -- thin forwarder to EvaluateAllStrategiesImpl<PelTag>.
//////////////////////////////////////////////////////////////////////

std::vector<BDPTIntegrator::ConnectionResult> BDPTIntegrator::EvaluateAllStrategies(
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<BDPTVertex>& eyeVerts,
	const IScene& scene,
	const IRayCaster& caster,
	const ICamera& camera,
	const Point2& cameraLensSample,
	ISampler* pSampler
	) const
{
	return EvaluateAllStrategiesImpl<PelTag>(
		*this, lightVerts, eyeVerts, scene, caster, camera, cameraLensSample, pSampler,
#ifdef RISE_ENABLE_OPENPGL
		pCompletePathGuide, completePathStrategySelectionEnabled, completePathStrategySampleCount,
		&strategySelectionPathCount, &strategySelectionCandidateCount, &strategySelectionEvaluatedCount,
		pGuidingField, &guidingTrainingStats, &guidingTrainingStatsMutex,
		pLightGuidingField, maxLightGuidingDepth,
#endif
		PelTag{} );
}


//////////////////////////////////////////////////////////////////////
// MISWeight - power heuristic (exponent = 2)
//
// Uses the technique from Veach's thesis: compute the weight by
// walking along the path and computing ratios of PDFs for adjacent
// strategies.
//
// For a path of length k = s + t - 1, the power heuristic weight
// for strategy (s,t) is:
//
//   w(s,t) = p_s^2 / sum_{i} p_i^2 = 1 / sum_{i} (p_i / p_s)^2
//
// where p_i is the probability of generating this path using
// strategy i.  The ratios p_i/p_{s} can be computed incrementally
// using the stored forward and reverse PDFs at each vertex.
//
// EXTENSIONS:
//
// Correlation-aware MIS (Grittmann et al. 2021):
// Strategies sharing more subpath vertices with the reference
// strategy (s,t) are correlated — their contributions tend to
// co-vary.  The discount factor reduces their effective weight
// in the denominator, redistributing MIS weight toward less
// correlated strategies.  Overlap is computed as the fraction of
// shared vertices between the alternative and reference strategy.
//
// Efficiency-aware MIS:
// Strategies with higher evaluation cost should receive lower
// MIS weight.  The cost for strategy (s',t') is proportional to
// the number of BSDF evaluations and visibility queries required.
//////////////////////////////////////////////////////////////////////
//#define MISWEIGHT_BALANCE_HEURISTIC 1
Scalar BDPTIntegrator::MISWeight(
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<BDPTVertex>& eyeVerts,
	unsigned int s,
	unsigned int t
	) const
{
	if( s + t < 2 ) {
		return 0;
	}

	// If only one strategy is possible, weight = 1
	if( s + t == 2 ) {
		// For a path of length 1 (2 vertices), there might still be
		// multiple strategies, but handle the simple case first
	}

	// Build the full path from light vertices [0..s-1] and eye vertices [t-1..0]
	// The path vertices in order are:
	//   lightVerts[0], ..., lightVerts[s-1], eyeVerts[t-1], ..., eyeVerts[0]
	//
	// Strategy (s,t) splits the path such that the light subpath has s vertices
	// and the eye subpath has t vertices.  The connection is between
	// lightVerts[s-1] and eyeVerts[t-1].

	// We compute the MIS weight using the power heuristic with exponent 2.
	// The power heuristic concentrates weight on the strategy with the
	// highest sampling probability, which provides provably lower variance
	// than the balance heuristic (exponent 1) for scenes with caustics and
	// other difficult light transport paths (Veach thesis, Section 9.2.4).
	//
	// w(s,t) = p_s^2 / sum_i p_i^2 = 1 / sum_i (p_i / p_s)^2
	//
	// The ratios p_i/p_s are computed incrementally by walking along the
	// path and accumulating forward/reverse PDF ratios at each vertex.

	Scalar sumWeights = 1.0;	// The weight for strategy (s,t) itself

	// Temporarily clear isDelta on the two connection vertices (PBRT convention).
	// The connection always evaluates the full BSDF (non-delta), so these
	// vertices should be treated as non-delta for MIS weight computation
	// regardless of which lobe was sampled during subpath generation.
	bool savedLightEndDelta = false, savedEyeEndDelta = false;
	if( s > 0 ) {
		savedLightEndDelta = lightVerts[s-1].isDelta;
		if( lightVerts[s-1].isConnectible ) {
			const_cast<BDPTVertex&>( lightVerts[s-1] ).isDelta = false;
		}
	}
	if( t > 0 ) {
		savedEyeEndDelta = eyeVerts[t-1].isDelta;
		if( eyeVerts[t-1].isConnectible ) {
			const_cast<BDPTVertex&>( eyeVerts[t-1] ).isDelta = false;
		}
	}

	//
	// Walk along the light subpath (decreasing s, increasing t)
	// This computes ratios for strategies (s-1, t+1), (s-2, t+2), etc.
	//
	{
		Scalar ri = 1.0;

		for( int i = static_cast<int>(s) - 1; i >= 0; i-- )
		{
			// Vertex at position i in the light subpath.
			//
			// The `eyeVerts[0]` arm is DEAD and is kept only as a
			// bounds belt: `i` runs from s-1 down to 0, and every
			// caller reaches this walk with s <= lightVerts.size()
			// (EvaluateAllStrategies enumerates s over
			// [0, lightVerts.size()]), so `i < lightVerts.size()`
			// always holds.  It matters that it is dead: the eye-side
			// walk below never reads eyeVerts[0] either, which is what
			// makes a finite aperture a non-event for MIS -- the camera
			// vertex's positional density is common to every strategy
			// and cancels (docs/RENDERING_INTEGRATORS.md §6.1).  If this
			// arm ever fired it would silently mix an EYE vertex's pdfs
			// into the light-side ratio chain.
			const BDPTVertex& vi = (static_cast<unsigned int>(i) < lightVerts.size()) ?
				lightVerts[i] : eyeVerts[0];

			// Compute the ratio: pdfRev / pdfFwd at this vertex.
			// Use remap0 (Veach/PBRT convention): map zero PDFs to 1
			// so that the ratio chain propagates through delta vertices.
			// Delta vertices have pdfRev=0 (since SPF::Pdf returns 0
			// for Dirac distributions), but they are always skipped in
			// the sum below.  Without remap0, the zero kills the chain
			// and prevents subsequent non-delta strategies from being
			// properly weighted, causing fireflies on caustic paths.
			const Scalar pdfR = (vi.pdfRev != 0) ? vi.pdfRev : Scalar(1);
			const Scalar pdfF = (vi.pdfFwd != 0) ? vi.pdfFwd : Scalar(1);
			ri *= pdfR / pdfF;

			// Skip non-connectible vertices — they can only be generated by
			// exactly one strategy, so their contribution to the sum is 0.
			// For non-connection vertices isDelta reflects the sampled lobe;
			// for connection vertices isDelta was cleared above if connectible.
			// Both vertices at the proposed connection must be non-delta
			// (PBRT convention: !v[i].delta && !v[i-1].delta).
			//
			// EXCEPTION (delta light sources at i=1): the strategy (1, t+1)
			// is NEE, which explicitly samples the delta-position light by
			// direct sampling — its pdf in area measure is well-defined
			// despite the delta vertex.  Without including it in the MIS
			// denominator, s>=2 light-tracing strategies for paths through
			// delta lights get misWeight=1 instead of being downweighted to
			// ~0, producing per-pixel bias every time a light-tracing splat
			// lands on a pixel.  The cure for that mode is to count NEE as
			// a competing strategy here.
			// DL-126.  A vertex whose material has no BSDF
			// (`!isConnectible`, e.g. `biospec_skin_material` /
			// `generic_human_tissue_material`) can never be a connection
			// endpoint -- `PathVertexEval::EvalBSDFAtVertex` returns 0
			// there in every direction, so every connection strategy
			// through it evaluates to 0 (the same "reserve no MIS mass for
			// a zero-yield strategy" rule `isConnectible`'s own contract
			// comment states above).  Such a vertex is a black box to
			// every OTHER technique exactly like a delta lobe, even though
			// its own sample was not drawn from one (`isDelta` is false).
			// CORRECTION (review round 2, 2026-09-18): this used to argue
			// the phantom ratio is "often exactly 1 once both `pdfFwd` and
			// `pdfRev` remap0 to 1" -- that overstates it.  Veach's
			// delta-transparency convention (`pdfFwdPrev = 0`, applied a
			// few lines above at THIS vertex's own scatter) zeroes the
			// FORWARD density of the vertex AFTER this one, not this
			// vertex's own `pdfFwd`/`pdfRev`, which come from the
			// PRECEDING and FOLLOWING vertices' own (generally ordinary,
			// non-null) materials and so are generically NONZERO, not
			// remapped at all.  The skip is still required regardless of
			// what those two numbers evaluate to: the strategy this ratio
			// would credit is zero-yield BY THE MATERIAL, not by an
			// arithmetic coincidence in the pdf ratio, so any finite `ri`
			// contributed here -- 1, or anything else -- wrongly reserves
			// denominator mass for a strategy that always evaluates its
			// own contribution to 0, deflating every strategy that
			// legitimately passes through it.
			if( vi.isDelta || !vi.isConnectible ) {
				continue;
			}
			if( i > 0 && ( lightVerts[i-1].isDelta || !lightVerts[i-1].isConnectible ) &&
				!( i == 1 && lightVerts[0].type == BDPTVertex::LIGHT ) )
			{
				continue;
			}

			// Strategy (i, s+t-i): compute contribution to denominator
			#if MISWEIGHT_BALANCE_HEURISTIC
			sumWeights += ri;
			#else
			sumWeights += ri * ri;
			#endif

		}
	}

	//
	// Walk along the eye subpath (increasing s, decreasing t)
	// This computes ratios for strategies (s+1, t-1), (s+2, t-2), etc.
	//
	{
		Scalar ri = 1.0;

		for( int j = static_cast<int>(t) - 1; j > 0; j-- )
		{
			// Vertex at position j in the eye subpath
			const BDPTVertex& vj = (static_cast<unsigned int>(j) < eyeVerts.size()) ?
				eyeVerts[j] : lightVerts[0];

			// STRATEGY-DOES-NOT-EXIST ZERO (fixed 2026-08-01; this was the
			// "known defect, deferred" note from fix round 3's Opus MIS
			// review).  remap0, immediately below, maps a zero pdf to 1 so
			// the ratio chain survives a DELTA vertex -- whose pdf is zero
			// only because a Dirac has no density, while the strategy
			// through it still exists.  It cannot tell that zero apart from
			// a zero that means "this strategy has no density at all".
			//
			// The (s=0) emitter-hit strategy produces the second kind at
			// j == t-1 (vj IS eyeEnd) whenever the emitter is outside the
			// NEE light set, and tags it via `lightSamplingStrategyAbsent`
			// (set where the reason is known -- see the s=0 block's
			// comment and BDPTVertex.h's contract for the field).
			//
			// In Veach's sum w(s,t) = p_s^2 / sum_i p_i^2, the term this
			// iteration would add is p_{s+t-j} for the strategy that
			// light-samples vj; its density is p_s * ri with
			// ri = pdfRev(vj) / pdfFwd(vj).  A genuinely zero pdfRev makes
			// that density exactly ZERO -- and, because the walk carries ri
			// forward multiplicatively, so is every later term (each of
			// those strategies also needs a light subpath rooted at or
			// through vj).  Leaving the loop therefore adds exactly the
			// terms Veach's sum contains, no more: the phantom NEE term and
			// the phantom s>=2 light-tracing family behind it are removed,
			// and nothing else changes.  `break` is precisely equivalent to
			// `ri = 0` plus running the remaining iterations -- the body has
			// no other effect -- and says the reason out loud.
			//
			// Result: sumWeights collapses to 1 for such a path, i.e.
			// weight 1, matching PT (PathTracingIntegrator.cpp skips its
			// emission-MIS block for a non-NEE-sampleable emitter) and VCM
			// (VCMIntegrator.cpp's `wCamera = pdfSelect > 0 ? ... : 0`).
			// Nothing here touches remap0's delta semantics, the power-2
			// heuristic, or any weight on a path whose emitter IS in the
			// light set -- for those, pdfRev > 0 and the flag is never set.
			// Pinned by tests/BDPTPhantomStrategyWeightTest.cpp; MLT
			// inherits the correction (MLTRasterizer builds a
			// BDPTIntegrator).
			//
			// SCOPE CONTRACT -- the only vertex that may carry this flag is
			// the (s=0) eye END, i.e. j == t-1, the FIRST iteration of this
			// loop.  That is what makes `break` sound: the flagged vertex is
			// the ROOT the whole s >= 1 family would have to be
			// light-sampled from, so zeroing the chain there zeroes exactly
			// the strategies that do not exist.  If some future code ever
			// tags an INTERIOR eye vertex, this `break` would silently
			// truncate the walk and drop strategies that DO exist (an energy
			// excess) -- such a change must revisit this gate, not just set
			// the flag.  Stated as a comment rather than an assert because
			// this file carries no asserts (checked: zero `assert(` in
			// BDPTIntegrator.cpp) and MISWeight is on the per-sample hot
			// path.
			if( vj.lightSamplingStrategyAbsent ) {
				break;
			}

			// Compute the ratio with remap0 (see light-side walk above)
			const Scalar pdfR = (vj.pdfRev != 0) ? vj.pdfRev : Scalar(1);
			const Scalar pdfF = (vj.pdfFwd != 0) ? vj.pdfFwd : Scalar(1);
			ri *= pdfR / pdfF;

			// Skip non-connectible vertices.
			// Both vertices at the proposed connection must be non-delta.
			// DL-126: also skip a vertex whose material has no BSDF at all
			// (`!isConnectible`) -- see the light-side walk's twin comment
			// above for the derivation.
			if( vj.isDelta || !vj.isConnectible ) {
				continue;
			}
			if( j > 0 && ( eyeVerts[j-1].isDelta || !eyeVerts[j-1].isConnectible ) ) {
				continue;
			}

			// Strategy (s+t-j, j): compute contribution to denominator
			#if MISWEIGHT_BALANCE_HEURISTIC
			sumWeights += ri;
			#else
			sumWeights += ri * ri;
			#endif
		}
	}

	// Restore isDelta on connection vertices
	if( s > 0 ) {
		const_cast<BDPTVertex&>( lightVerts[s-1] ).isDelta = savedLightEndDelta;
	}
	if( t > 0 ) {
		const_cast<BDPTVertex&>( eyeVerts[t-1] ).isDelta = savedEyeEndDelta;
	}

	if( sumWeights <= 0 ) {
		return 0;
	}

	return 1.0 / sumWeights;
}

//////////////////////////////////////////////////////////////////////
// GenerateLightSubpathImpl<Tag> -- Phase 2c family F2b.
//
// The light-side half of subpath generation, templatized over PelTag/NMTag.
// Free function (not a member template) taking the needed BDPTIntegrator
// state as parameters, so BDPTIntegrator.h is untouched and the public
// GenerateLightSubpath{,NM} members (consumed by VCM / MLT / BDPT-spectral)
// stay byte-identical.  Reuses the F2a anon-namespace dispatch helpers.
//////////////////////////////////////////////////////////////////////

namespace {

template<class Tag>
unsigned int GenerateLightSubpathImpl(
	unsigned int maxLightDepth,
	const StabilityConfig& stabilityConfig,
	const LightSampler* pLightSampler,
#ifdef RISE_ENABLE_OPENPGL
	PathGuidingField* pLightGuidingField,
	unsigned int maxLightGuidingDepth,
	Scalar guidingAlpha,
	GuidingSamplingType guidingSamplingType,
#endif
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const RandomNumberGenerator& random,
	std::vector<BDPTVertex>& vertices,
	std::vector<uint32_t>& subpathStarts,
	const Tag& tag,
	const SampledWavelengths* pSwlHWSS )
{
	typedef SpectralValueTraits<Tag> Traits;
	typedef typename Traits::value_type V;

	vertices.clear();
	subpathStarts.clear();
	subpathStarts.push_back( 0 );

	if( !pLightSampler ) {
		return 0;
	}

	// We need a LuminaryManager to get the luminaries list.
	// Get it from the caster's luminary manager.
	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	LuminaryManager::LuminariesList emptyList;

	// Use dynamic_cast since LuminaryManager inherits ILuminaryManager via virtual base
	const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
	const LuminaryManager::LuminariesList& luminaries = pLumManager ?
		const_cast<LuminaryManager*>(pLumManager)->getLuminaries() : emptyList;

	IORStack iorStack( 1.0 );

	// Phase 0: light source sampling (position + direction)
	sampler.StartStream( 0 );

	LightSample ls;
	if( !pLightSampler->SampleLight( scene, luminaries, sampler, ls ) ) {
		return 0;
	}

	// Seed the IOR stack with the chain of dielectric objects that
	// physically contain the light-emission point.  Without this, a
	// luminaire sealed inside nested refractors (e.g. a lambertian
	// sphere inside an `air_cavity` inside a glass egg) scatters its
	// first bounce as if entering the inner boundary from outside —
	// which for an IOR-matched inner boundary turns into a noise-
	// Fresnel reflection that destroys throughput by ~32 orders of
	// magnitude and leaves the walls unlit.
	IORStackSeeding::SeedFromPoint( iorStack, ls.position, scene );

	vertices.reserve( maxLightDepth + 1 );

	// Emission radiance in the tag's value type.  Pel takes ls.Le (RISEPel)
	// directly; NM converts to a scalar at wavelength tag.nm -- mesh
	// luminaires re-evaluate via emittedRadianceNM, non-mesh lights use a
	// luminance projection, and env-IBL samples query GetRadianceNM in the
	// sampled-sky direction.  Genuine Pel/NM divergence (NM has no RISEPel Le).
	V Le = Traits::zero();
	if constexpr( Traits::is_pel ) {
		Le = ls.Le;
	} else {
		Scalar LeNM = 0;
		if( ls.pLuminary && ls.pLuminary->GetMaterial() ) {
			const IEmitter* pEmitter = ls.pLuminary->GetMaterial()->GetEmitter();
			if( pEmitter ) {
				RayIntersectionGeometric rig( Ray( ls.position, ls.direction ), nullRasterizerState );
				rig.bHit = true;
				rig.ptIntersection = ls.position;
				rig.vNormal = ls.normal;
				rig.vGeomNormal = ls.normal;
				// DL-44: the sampled emitter UV, so a UV-keyed emission
				// painter (checker_painter, an image exitance map) reads the
				// same texel the RGB hero (`ls.Le`, evaluated inside
				// `SampleLight`) did, not the default-constructed (0,0).
				rig.ptCoord = ls.ptCoord;
				OrthonormalBasis3D onb;
				onb.CreateFromW( ls.normal );
				rig.onb = onb;
				// THE NM HERO twin of LightSampler's own emission record
				// (slice S3, docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md
				// §5).  `ls.surface` was probed ONCE inside `SampleLight`
				// for exactly this point, so the hero, the HWSS companions
				// below and the RGB record upstream all price the emitter
				// against the same live channel.  A no-op when the probe
				// was gated off or refused.
				LightSampler::ApplyEmitterSurface( rig, ls.surface );
				// `Po`, separately and UNCONDITIONALLY: `ls.ptObjIntersec`
				// is filled without a ray and without the signal gate (see
				// `LightSampler::EmitterObjectPoint`), because painters
				// that read `Po` register no signal demand.  `(0,0,0)` --
				// this record's previous value -- for a delta light, an
				// env sample, and a CSG-composite luminary.
				rig.ptObjIntersec = ls.ptObjIntersec;
				LeNM = pEmitter->emittedRadianceNM( rig, ls.direction, ls.normal, tag.nm );
			}
		} else if( ls.pLight ) {
			// Stage C slice 2: the light's own spectrum at tag.nm, not a
			// flat Rec.709 luma projection of ls.Le.
			LeNM = ls.pLight->emittedRadianceNM( ls.direction, tag.nm );
		} else if( ls.pEnvLight ) {
			RasterizerState nullRast = {0};
			const Vector3 toSky( -ls.direction.x, -ls.direction.y, -ls.direction.z );
			Ray skyProbe( ls.position, toSky );
			LeNM = ls.pEnvLight->GetRadianceNM( skyProbe, nullRast, tag.nm );
		}
		Le = LeNM;
	}

	//
	// Vertex 0: the light source itself
	//
	{
		BDPTVertex v;
		v.type = BDPTVertex::LIGHT;
		v.position = ls.position;
		v.normal = ls.normal;
		// Light samples come from `IObject::UniformRandomPoint` on the
		// luminary mesh.  That normal is the INTERPOLATED VERTEX normal
		// where the mesh carries per-vertex normals and the face normal
		// where it does not -- `GeometricUtilities::PointOnTriangle`
		// barycentrically averages `t.normals[]` and only falls back to the
		// cross product for a triangle with none.  (An earlier comment here
		// called it the geometric face normal flatly; corrected in the S3
		// review.)  No Phong / bump MODIFIER perturbs a luminaire record,
		// so shading == geometric on light vertex 0 in the sense that
		// matters: both fields hold the one normal the sampler produced and
		// the pdf was expressed against.
		v.geomNormal = ls.normal;
		v.onb.CreateFromW( ls.normal );
		v.pMaterial = 0;
		v.pObject = 0;
		v.pLight = ls.pLight;
		v.pLuminary = ls.pLuminary;
		// Env-light vertex 0 has pLight == pLuminary == NULL; the
		// downstream MIS dispatch sites recognise the env-vertex via
		// pEnvLight != NULL and recover Le via GetRadiance(skyProbe).
		// Without this, env-IBL scenes lose every BDPT connection
		// strategy that touches a light vertex.
		v.pEnvLight = ls.pEnvLight;
		v.isDelta = ls.isDelta;
		v.isConnectible = !ls.isDelta;

		// THE LIGHT-TYPE ROOT VERTEX carries the probed payload too (slice
		// S3, docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §5's last
		// bullet).  Two emitter evaluations price this vertex THROUGH
		// `PathVertexEval::PopulateRIGFromVertex` rather than through the
		// records above -- `LuminaryRadiance` (the t=1 light-to-camera
		// splat and the s=1 connections) here.  (`VCMIntegrator`'s splat
		// skips the LIGHT root and its s=0 strategy prices a real ray hit,
		// so VCM is NOT a consumer of this vertex -- DL-44 review
		// correction, 2026-09-14.)  A better record inside `LightSampler`
		// alone would leave `LuminaryRadiance` reading the neutral channel
		// while everything else on the same subpath read the live one.
		//
		// The three field groups are exactly the ones S1 put on the vertex
		// for surface hits (§3); `ptObjIntersec` rides along because the
		// rebuild copies it and the expression VM exposes it as `Po`.
		// Everything the SAMPLE determines -- position, both normals, the
		// ONB -- is left as set above, so no pdf or cosine moves.
		if( ls.surface.valid ) {
			v.derivatives   = ls.surface.derivatives;
			v.signals       = ls.surface.channel;
			v.txFootprint   = ls.surface.txFootprint;
		}
		// `ptObjIntersec` is NOT under that gate: `Po` is read by painters
		// that register no signal demand, so it must not depend on the
		// process-wide probe gate.  `ls.ptObjIntersec` is `(0,0,0)` --
		// exactly what this vertex carried before -- whenever there is no
		// single object frame to map into.
		v.ptObjIntersec = ls.ptObjIntersec;
		// DL-44: same ungated treatment for the sampled UV.  Read by
		// `PathVertexEval::PopulateRIGFromVertex`'s consumers of this
		// vertex -- `LuminaryRadiance` (BDPT's own s=0/t=1 splat and s=1
		// connections; VCM does not rebuild from this vertex) -- so a
		// UV-keyed emission painter
		// sees the same texel there that `SampleLight`'s own RGB
		// evaluation and the NM hero/HWSS rebuilds above see.  No second
		// UV channel: `IObject::UniformRandomPoint` returns one Point2, so
		// `v.ptCoord1` stays default (0,0), matching every real surface
		// vertex on an object with no TEXCOORD_1.
		v.ptCoord = ls.ptCoord;

		// pdfFwd is the probability of generating this light vertex
		// = pdfSelect * pdfPosition
		v.pdfFwd = ls.pdfSelect * ls.pdfPosition;

		// Store pdfSelect separately so VCM's `ConvertLightSubpath`
		// can extract the geometric `emissionPdfW = pdfPos × pdfDir`
		// from the joint `v.emissionPdfW = pdfSelect × pdfPos × pdfDir`
		// when computing SmallVCM's `dVC = cosLight / emissionPdfW_geom`
		// — see BDPTVertex.h's `pdfSelect` doc comment for the full
		// continuous-PMF rationale.
		v.pdfSelect = ls.pdfSelect;

		// Throughput: Le / (pdfSelect * pdfPosition); pdfDirection folds in at
		// trace time.  NM also broadcasts the scalar into the RISEPel throughput
		// field for guiding-training Le recovery (the Pel path sets only
		// throughput) -- preserved Pel/NM divergence.
		if( v.pdfFwd > 0 ) {
			StoreThroughput<Tag>( v, Le * (Scalar( 1 ) / v.pdfFwd) );
			if constexpr( Traits::is_nm ) {
				v.throughput = RISEPel( v.throughputNM, v.throughputNM, v.throughputNM );
			}
		} else {
			StoreThroughput<Tag>( v, Traits::zero() );
		}

		v.pdfRev = 0;
		// DL-09: the light endpoint's graded medium and index (SeedFromPoint
		// recorded n at the light point), for s==1 connections.
		GradedIndexMedium::RecordVertex( iorStack, v.pGradedMedium, v.gradedIOR );
		vertices.push_back( v );
	}

	// Check if Le is zero -- no point tracing further
	if( PositiveMagnitude<Tag>( Le ) <= 0 || ls.pdfDirection <= 0 ) {
		subpathStarts.push_back( static_cast<uint32_t>( vertices.size() ) );
		return static_cast<unsigned int>( vertices.size() );
	}

	//
	// Trace the emission ray into the scene
	//
	Ray currentRay( ls.position, ls.direction );
	currentRay.Advance( BDPT_RAY_EPSILON );

	// Beta tracks the path throughput beyond vertex 0.
	// After emission:  beta = Le * |cos(theta_0)| / (pdfSelect * pdfPosition * pdfDirection)
	const Scalar cosAtLight = fabs( Vector3Ops::Dot( ls.direction, ls.normal ) );
	V beta = Le * cosAtLight;
	const Scalar pdfDirArea = ls.pdfDirection;   // already in solid angle for now
	const Scalar pdfEmit = ls.pdfSelect * ls.pdfPosition * pdfDirArea;

	// VCM post-pass inputs on the light endpoint: combined
	// emission pdf (= directPdfA * solidAnglePdfW) and the
	// generator-side cosine at the light surface.  See
	// BDPTVertex.h for the semantics expected by VCMIntegrator.
	vertices[0].emissionPdfW = pdfEmit;
	vertices[0].cosAtGen = cosAtLight;

	if( pdfEmit > 0 ) {
		beta = beta * (Scalar( 1 ) / pdfEmit);
	} else {
		subpathStarts.push_back( static_cast<uint32_t>( vertices.size() ) );
		return static_cast<unsigned int>( vertices.size() );
	}

	Scalar pdfFwdPrev = pdfDirArea;	// solid angle PDF of emission direction

	// HWSS per-wavelength throughput tracking (NM bundle only).  Each active
	// wavelength's initial value is (Le_w * cosAtLight / pdfEmit) -- the hero
	// formula with Le re-evaluated at the companion lambda.  No Pel analog.
	[[maybe_unused]] Scalar hwssBetaNM[SampledWavelengths::N] = {};
	if constexpr( Traits::is_nm ) {
		if( pSwlHWSS ) {
			for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
				hwssBetaNM[w] = 0;
				if( pSwlHWSS->terminated[w] ) continue;
				if( w == 0 ) {
					hwssBetaNM[w] = beta;	// hero
					continue;
				}
				Scalar LeW = 0;
				if( ls.pLuminary && ls.pLuminary->GetMaterial() ) {
					const IEmitter* pEm = ls.pLuminary->GetMaterial()->GetEmitter();
					if( pEm ) {
						RayIntersectionGeometric rigW(
							Ray( ls.position, ls.direction ), nullRasterizerState );
						rigW.bHit = true;
						rigW.ptIntersection = ls.position;
						rigW.vNormal = ls.normal;
						rigW.vGeomNormal = ls.normal;
						// DL-44: same ungated UV as the hero `rig` above --
						// see that site's comment.
						rigW.ptCoord = ls.ptCoord;
						OrthonormalBasis3D onbW;
						onbW.CreateFromW( ls.normal );
						rigW.onb = onbW;
						// THE HWSS COMPANION twin.  Fixing the hero alone
						// would leave a hero-live / companion-neutral split
						// -- the spectral form of the defect slice S3
						// closes -- so this record gets the SAME probed
						// payload, and the same ungated `Po`, from the
						// SAME `ls`.  `vGeomNormal` is set here too (an
						// earlier draft left it default, unlike the hero
						// `rig` above) -- `rig` and `rigW` are now built
						// identically field-for-field.
						LightSampler::ApplyEmitterSurface( rigW, ls.surface );
						rigW.ptObjIntersec = ls.ptObjIntersec;
						LeW = pEm->emittedRadianceNM(
							rigW, ls.direction, ls.normal, pSwlHWSS->lambda[w] );
					}
				} else if( ls.pLight ) {
					// Stage C slice 2: per-wavelength, so the HWSS
					// companion wavelengths actually differ (they were all
					// the same luma scalar before).
					LeW = ls.pLight->emittedRadianceNM( ls.direction, pSwlHWSS->lambda[w] );
				} else if( ls.pEnvLight ) {
					RasterizerState nullRastW = {0};
					const Vector3 toSkyW( -ls.direction.x, -ls.direction.y, -ls.direction.z );
					Ray skyProbeW( ls.position, toSkyW );
					LeW = ls.pEnvLight->GetRadianceNM( skyProbeW, nullRastW, pSwlHWSS->lambda[w] );
				}
				if( pdfEmit > 0 ) {
					hwssBetaNM[w] = LeW * cosAtLight / pdfEmit;
				}
			}
		}
	}

	// Per-type bounce counters for StabilityConfig limits
	unsigned int diffuseBounces = 0;
	unsigned int glossyBounces = 0;
	unsigned int transmissionBounces = 0;
	unsigned int translucentBounces = 0;
	unsigned int volumeBounces = 0;
	unsigned int surfaceBounces = 0;

	// Loop limit accounts for both surface and volume bounces.
	// Surface bounces are capped by maxLightDepth, volume bounces by maxVolumeBounce.
	// Saturating add to avoid underflow when a scene sets maxLightDepth
	// pathologically high (≥1024).
	// Cap: BDPTUtilities::kWalkIterationCap (DL-283 stream layout).
	const unsigned int kCap = BDPTUtilities::kWalkIterationCap;
	const unsigned int maxLightTotalDepth =
		( maxLightDepth >= kCap ||
		  stabilityConfig.maxVolumeBounce > kCap - maxLightDepth ) ?
			kCap :
			maxLightDepth + stabilityConfig.maxVolumeBounce;

	for( unsigned int depth = 0; depth < maxLightTotalDepth; depth++ )
	{
		// Per-bounce stream offset on the light subpath.  Streams
		// [1, 1+maxLightTotalDepth) are reserved for the light walk;
		// eye walk uses [16, ...).
		sampler.StartStream( 1u + depth );

		// Intersect the scene
		RayIntersection ri( currentRay, nullRasterizerState );
		scene.GetObjects()->IntersectRay( ri, true, true, false );

		// ----------------------------------------------------------------
		// Participating media: free-flight distance sampling (light subpath).
		// Same logic as the eye subpath — see GenerateEyeSubpath for the
		// full derivation of throughput weights and PDF measure.
		// ----------------------------------------------------------------
		// Declared outside the medium block so surface vertices can
		// inherit the enclosing medium info for connection transmittance.
		const IObject* pMedObj_light = 0;
		const IMedium* pMed_light = 0;
		{
			const IObject* pMedObj = 0;
			const IMedium* pMed = MediumTracking::GetCurrentMediumWithObject(
				iorStack, &scene, pMedObj );
			pMedObj_light = pMedObj;
			pMed_light = pMed;

			// G6: stamp the ambient (incident-medium) IOR so the conductor
			// Fresnel in ScatterSPF (light-subpath trace-time sampling) uses the
			// surrounding medium rather than hardcoded air.  IORStack::top() here
			// is the medium the ray was travelling through (read before
			// SetCurrentObject, which does not push).  Guard to air (1.0).
			{
				const Scalar ambIOR = iorStack.top();
				ri.geometric.ambientIOR = ( ambIOR > 0.0 ) ? ambIOR : 1.0;
			}

			if( pMed )
			{
				const Scalar maxDist = ri.geometric.bHit ? ri.geometric.range : RISE_INFINITY;
				// DL-247 ruling -- see the eye subpath's twin: past the cap
				// the segment is not sampled and carries deterministic Tr.
				const bool bAtCap = volumeBounces >= stabilityConfig.maxVolumeBounce;
				bool scattered = false;
				Scalar t_m = 0;
				if( !bAtCap ) {
					// DL-283: own stream block under a fixed-budget sampler
					// -- see the eye subpath's twin.
					const bool bOwnStream = sampler.HasFixedDimensionBudget();
					if( bOwnStream ) {
						sampler.StartStream( BDPTUtilities::MediumDistanceStream(
							BDPTUtilities::eLightWalk, depth ) );
					}
					t_m = SampleMediumDistance<Tag>(
						*pMed, currentRay, maxDist, sampler, scattered, tag );
					if( bOwnStream ) {
						sampler.StartStream( 1u + depth );
					}
				}

				if( scattered )
				{
					const Point3 scatterPt = currentRay.PointAtLength( t_m );
					const Vector3 wo = currentRay.Dir();
					Scalar sigma_t_max = 0;
					const V medWeight = ComputeMediumScatterWeight<Tag>(
						*pMed, scatterPt, currentRay, t_m, tag, sigma_t_max );

					if( PositiveMagnitude<Tag>( medWeight ) <= 0 ) {
						break;
					}

					beta = beta * medWeight;

					BDPTVertex mv;
					mv.type = BDPTVertex::MEDIUM;
					mv.position = scatterPt;
					mv.normal = -wo;
					mv.onb.CreateFromW( -wo );
					mv.pMaterial = 0;
					mv.pObject = 0;
					mv.pMediumVol = pMed;
					mv.pPhaseFunc = pMed->GetPhaseFunction();
					mv.pMediumObject = pMedObj;
					mv.sigma_t_scalar = sigma_t_max;
					mv.volumeBounces = volumeBounces + 1;
					mv.isDelta = false;

					// DL-200: a MEDIUM vertex is always connectible.  See the
					// long derivation at the twin site in
					// GenerateEyeSubpathImpl -- this is the LIGHT-rooted half of
					// the asymmetry that row is about, and it is closed by both
					// halves now answering the same, walk-independent question.
					mv.isConnectible = true;
					StoreThroughput<Tag>( mv, beta );

					const Scalar distSqMed = t_m * t_m;
					mv.pdfFwd = BDPTUtilities::SolidAngleToAreaMedium(
						pdfFwdPrev, mv.sigma_t_scalar, distSqMed );
					mv.pdfRev = 0;

					// VCM post-pass uses sigma_t_scalar (not cosAtGen) for
					// the area-to-solid-angle inversion at medium vertices.
					// cosAtGen is left at zero as it is unused.
					mv.cosAtGen = 0;

					// DL-09: see the eye walk's medium vertex.
					GradedIndexMedium::RecordVertex( iorStack, mv.pGradedMedium, mv.gradedIOR );
					vertices.push_back( mv );

					// Sample phase function continuation
					const IPhaseFunction* pPhase = pMed->GetPhaseFunction();
					if( !pPhase ) {
						break;
					}

					const Vector3 wi = pPhase->Sample( wo, sampler );
					const Scalar phasePdf = pPhase->Pdf( wo, wi );
					if( phasePdf <= NEARZERO ) {
						break;
					}

					const Scalar phaseVal = pPhase->Evaluate( wo, wi );
					beta = beta * (phaseVal / phasePdf);

					// Russian roulette
					{
						const PathTransportUtilities::RussianRouletteResult rr =
							PathTransportUtilities::EvaluateRussianRoulette(
								depth + volumeBounces, stabilityConfig.rrMinDepth,
								stabilityConfig.rrThreshold,
								Traits::max_value( beta ),
								Traits::max_value( VertexThroughput<Tag>( mv ) ),
								sampler.Get1D() );
						if( rr.terminate ) {
							break;
						}
						if( rr.survivalProb < 1.0 ) {
							beta = beta * (Scalar( 1 ) / rr.survivalProb);
						}
					}

					// Update pdfRev on previous vertex
					if( vertices.size() >= 2 ) {
						BDPTVertex& prev = vertices[ vertices.size() - 2 ];
						const Scalar revPdfSA = phasePdf;

						if( prev.type == BDPTVertex::MEDIUM ) {
							prev.pdfRev = BDPTUtilities::SolidAngleToAreaMedium(
								revPdfSA, prev.sigma_t_scalar, distSqMed );
						} else if( prev.type == BDPTVertex::LIGHT ) {
							const Scalar absCosAtPrev = fabs( Vector3Ops::Dot(
								prev.geomNormal, currentRay.Dir() ) );
							prev.pdfRev = BDPTUtilities::SolidAngleToArea(
								revPdfSA, absCosAtPrev, distSqMed );
						} else {
							const Scalar absCosAtPrev = fabs( Vector3Ops::Dot(
								prev.geomNormal, currentRay.Dir() ) );
							prev.pdfRev = BDPTUtilities::SolidAngleToArea(
								revPdfSA, absCosAtPrev, distSqMed );
						}
					}

					pdfFwdPrev = phasePdf;

					currentRay = Ray( scatterPt, wi );
					currentRay.Advance( BDPT_RAY_EPSILON );

					volumeBounces++;
					continue;
				}
				else if( bAtCap )
				{
					beta = beta * EvalMediumTransmittance<Tag>( *pMed, currentRay, maxDist, tag );
				}
				else if( ri.geometric.bHit )
				{
					// No-scatter SURVIVAL: the light subpath reached the surface
					// WITHOUT a medium scatter event.  Same analog convention as
					// the eye-subpath surface-hit branch -- the survival
					// probability already carries the analog
					// Beer-Lambert, so apply the weight Tr / pSurvival (the
					// deterministic no-scatter survival pdf), NOT the full Tr,
					// or the medium reads ~2x too thick.  Must match the eye
					// side so BDPT connections stay unbiased.
					const V Tr = EvalMediumTransmittance<Tag>(
						*pMed, currentRay, ri.geometric.range, tag );
					const Scalar pSurvival = EvalNoScatterSurvivalPdf<Tag>( *pMed, currentRay, ri.geometric.range, tag );
					beta = beta * BDPTSurvivalWeight( Tr, pSurvival );
				}
			}
		}

		if( !ri.geometric.bHit ) {
			break;
		}

		// Check surface depth limit (medium scatters don't count)
		if( surfaceBounces >= maxLightDepth ) {
			break;
		}
		surfaceBounces++;

		// Apply intersection modifier if present
		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		// DL-09: the interior-segment factor, IMPORTANCE walk:
		// (n_here/n_start)^2.  An importance walk needs it EXPLICITLY on a
		// straight graded segment -- at an interface the refraction map's
		// Jacobian supplies the n^2 through this walk's sample density, and
		// a straight segment has no map to supply it.  See
		// GradedIndexMedium.h and the derivation doc §3(iv).
		{
			Scalar gradedScale;
			if( GradedIndexMedium::Advance( iorStack, ri.geometric.ptIntersection,
					GradedIndexMedium::eImportance, gradedScale ) )
			{
				if( gradedScale != Scalar( 1 ) ) {
					beta = beta * gradedScale;
					if constexpr( Traits::is_nm ) {
						if( pSwlHWSS ) {
							for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
								hwssBetaNM[w] *= gradedScale;
							}
						}
					}
				}
				ri.geometric.ambientIOR = iorStack.top();
			}
		}

		// Create a new surface vertex
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = ri.geometric.ptIntersection;
		v.scatterIncomingDistance = ri.geometric.range;
		v.isLightSubpathVertex = true;
		v.normal = ri.geometric.vNormal;
		v.geomNormal = ri.geometric.vGeomNormal;
		v.onb = ri.geometric.onb;
		v.ptCoord = ri.geometric.ptCoord;
		v.ptCoord1 = ri.geometric.ptCoord1;
		v.bHasTexCoord1 = ri.geometric.bHasTexCoord1;
		v.ptObjIntersec = ri.geometric.ptObjIntersec;
		v.vColor = ri.geometric.vColor;
		v.bHasVertexColor = ri.geometric.bHasVertexColor;
		// SHADING-INPUT STATE — the light-subpath twin of the eye-subpath
		// copy in GenerateEyeSubpathImpl; see the long comment there for
		// why a painter-read field has to travel on the vertex and why
		// this file is a sanctioned `signals` writer (the value copied is
		// the object manager's own stamp, forwarded one hop).  Both sides
		// must carry them or a connection strategy would price its two
		// ends against different material states.
		v.derivatives = ri.geometric.derivatives;
		v.signals = ri.geometric.signals;
		v.txFootprint = ri.geometric.txFootprint;
		v.pMaterial = ri.pMaterial;
		v.pObject = ri.pObject;
		v.pLight = 0;
		v.pLuminary = 0;
		// Store enclosing medium so connection transmittance can
		// seed its boundary walk from the correct starting medium.
		v.pMediumObject = pMedObj_light;
		v.pMediumVol = pMed_light;
		v.volumeBounces = volumeBounces;
		if( ri.pObject ) {
			iorStack.SetCurrentObject( ri.pObject );
			v.mediumIOR = iorStack.top();
			v.insideObject = iorStack.containsCurrent();
		}
		GradedIndexMedium::RecordVertex( iorStack, v.pGradedMedium, v.gradedIOR );

		// Convert pdfFwdPrev from solid angle to area measure
		const Scalar distSq = ri.geometric.range * ri.geometric.range;
		// Solid-angle <-> area Jacobian uses the GEOMETRIC normal —
		// the area-element parameterisation depends on the actual face
		// orientation, not the Phong-perturbed shading normal
		// (Veach 1997 §8.2.2 / PBRT 4e §13.6.4).  Using shading here
		// biases every interior path-pdf factor and therefore the MIS
		// balance heuristic.
		const Scalar absCosIn = fabs( Vector3Ops::Dot(
			ri.geometric.vGeomNormal,
			-currentRay.Dir() ) );

		v.pdfFwd = BDPTUtilities::SolidAngleToArea( pdfFwdPrev, absCosIn, distSq );
		StoreThroughput<Tag>( v, beta );
		if constexpr( Traits::is_nm ) {
			// NM broadcasts the scalar throughput into the RISEPel field so
			// light-subpath guiding training (which stores RISEPel weights on
			// both tags) recovers Le.  Pel sets only throughput.
			v.throughput = RISEPel( beta, beta, beta );
		}
		v.pdfRev = 0;

		// VCM post-pass input: the receiving-side cosine used to
		// invert the area-measure pdfFwd back to solid angle at
		// merge/connection time.
		v.cosAtGen = absCosIn;

		// Check if the material has a delta BSDF (perfect specular)
		v.isDelta = false;

#ifdef RISE_ENABLE_OPENPGL
		// Light subpath guiding: record vertex metadata for training.
		// guidingDirectionOut is the direction toward the previous vertex
		// (the direction light arrived from), matching the eye subpath
		// convention where directionOut points toward the camera.
		if( maxLightGuidingDepth > 0 )
		{
			v.guidingHasSegment = true;
			v.guidingDirectionOut = -currentRay.Dir();
			v.guidingNormal = GuidingCosineNormal( v.normal, currentRay.Dir() );
			v.guidingEta = v.mediumIOR > NEARZERO ? v.mediumIOR : 1.0;
		}
#endif

		if( !ri.pMaterial ) {
			vertices.push_back( v );
			break;
		}

		const ISPF* pSPF = ri.pMaterial->GetSPF();
		if( !pSPF ) {
			vertices.push_back( v );
			break;
		}

		vertices.push_back( v );

		//
		// Sample the SPF for the next direction
		//
		ScatteredRayContainer scattered;
		// IMPORTANCE mode: no eta^2 factor anywhere in this generator
		// (debt 30).  A light subpath transports importance / flux, and
		// applying the basic-radiance factor on both subpath sides would
		// cancel the very non-symmetry that makes refraction non-symmetric.
		// Its eye-side twin GenerateEyeSubpathImpl applies it; see the
		// block above that function's Russian roulette and
		// docs/REFRACTIVE_RADIANCE_SCALING.md.
		ScatterSPF<Tag>( *pSPF, ri.geometric, sampler, scattered, iorStack, tag );

		// DL-67: see the eye twin -- the guided-vertex decision is the
		// vertex's, never the Scatter realization's.
#ifdef RISE_ENABLE_OPENPGL
		const bool guidedVertex = pLightGuidingField && pLightGuidingField->IsTrained() &&
			depth < maxLightGuidingDepth && guidingAlpha > NEARZERO &&
			ri.pMaterial->GetBSDF() != 0;
#else
		const bool guidedVertex = false;
#endif
		// DL-307: see the eye twin -- an empty container at a vertex with a
		// subsurface-entry branch still offers that branch its coin.
		bool subsurfaceCarrier = false;
		if( scattered.Count() == 0 && !guidedVertex ) {
			subsurfaceCarrier = HasSubsurfaceEntryBranch<Tag>( *ri.pMaterial, tag );
			if( !subsurfaceCarrier ) {
				break;
			}
		}

		// Stochastic single-lobe selection (no path-tree branching).
		// Consume one sampler dimension for Sobol alignment.
		const Scalar lobeSelectXi = sampler.Get1D();
		const ScatteredRay* pScat = 0;
		Scalar selectProb = 1.0;

		if( scattered.Count() > 0 ) {
			pScat = scattered.RandomlySelect( lobeSelectXi, Traits::is_nm, &selectProb );
		}
		// See the eye twin: a placeholder carries a guided vertex (or, DL-307,
		// an un-guided subsurface vertex) with no selectable lobe, and is what
		// a guide draw continues on.
		ScatteredRay guideTemplateRay;
		guideTemplateRay.type = ScatteredRay::eRayDiffuse;
		guideTemplateRay.isDelta = false;
		guideTemplateRay.ray = Ray( ri.geometric.ptIntersection, ri.geometric.vNormal );
		const bool hasLobe = ( pScat != 0 );
		if( !pScat ) {
			if( !guidedVertex && !subsurfaceCarrier ) {
				subsurfaceCarrier = HasSubsurfaceEntryBranch<Tag>( *ri.pMaterial, tag );
				if( !subsurfaceCarrier ) {
					break;
				}
			}
			pScat = &guideTemplateRay;
			selectProb = 1.0;
		}

		// Connectibility is a property of the SURFACE, not of the one
		// continuation drawn: connectible iff the material has a BSDF.
		// RISE's pure-delta materials (PerfectReflector, PerfectRefractor,
		// Dielectric) return NULL here and mark every lobe delta; a mixed
		// material (weave_material `transmission thin` with gap > 0, a
		// coated/polished/composite over a continuum base) has a BSDF even
		// on the draws that picked its delta lobe.  Deriving this from
		// `scattered[i].isDelta` dropped NEE on a gap fraction of hits --
		// (1-gap)^2 vs PT's (1-gap) -- docs/CLOTH_FABRIC_DESIGN.md 15 debt
		// 23.  Deliberately NOT `|| any non-delta draw`: a null-BSDF vertex
		// evaluates every connection to 0 (PathVertexEval.h), so marking it
		// connectible would reserve MIS mass for zero-yield strategies.
		vertices.back().isConnectible = ( ri.pMaterial->GetBSDF() != 0 );

		// Mark the current vertex as delta if the scattered ray is delta
		vertices.back().isDelta = pScat->isDelta;



		// --- BSSRDF sampling for materials with diffusion profiles ---
		// At front-face hits on SSS surfaces, use a Fresnel coin flip to
		// choose between surface reflection and subsurface transmission.
		// This ensures reflection and subsurface compete with the correct
		// Fresnel-weighted probabilities (Veach/PBRT convention).
		Scalar bssrdfReflectCompensation = 1.0;
		if( ri.pMaterial && ri.pMaterial->GetDiffusionProfile() )
		{
			// No fabs: back-face hits (cosIn < 0) skip BSSRDF,
			// preventing artifacts on thin geometry (lips, eyelids).
			// Front-face gate uses GEOMETRIC; Fresnel cosine uses SHADING.
			// PBRT 4e §10.1.1 (front/back is geometric); §11.4.2 (BSSRDF
			// Fresnel angular dependence is shading-frame).
			const Vector3 wo_bss = -currentRay.Dir();
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
			const Scalar cosInGeom = ri.geometric.BSSRDFEntryFacing( wo_bss );
			// Fresnel cosine clamped via fabs+NEARZERO — see PT site for
			// rationale.  Replaces fallback-to-cosInGeom (discontinuous Ft).
			const Scalar cosInShade = Vector3Ops::Dot( ri.geometric.vNormal, wo_bss );
			const Scalar cosIn = r_max( fabs( cosInShade ), Scalar( NEARZERO ) );
			if( cosInGeom > NEARZERO )
			{
				ISubSurfaceDiffusionProfile* pProfile = ri.pMaterial->GetDiffusionProfile();
				const Scalar Ft = pProfile->FresnelTransmission( cosIn, ri.geometric );
				const Scalar R = 1.0 - Ft;

				if( Ft > NEARZERO && sampler.Get1D() < Ft )
				{
					// Chose subsurface transmission (probability Ft)
					BSSRDFSampling::SampleResult bssrdf = BSSRDFSampling::SampleEntryPoint(
						ri.geometric, ri.pObject, ri.pMaterial, sampler, NmOrZero<Tag>( tag ) );

					if( bssrdf.valid )
					{
						vertices.back().isDelta = true;

						// Spatial-only weight for connections (Sw not baked into
						// throughput).  Pel: * (1.0/Ft); NM: / Ft (ULP-level
						// difference, preserved exactly via if constexpr).
						V betaSpatial;
						if constexpr( Traits::is_pel ) {
							betaSpatial = beta * bssrdf.weightSpatial * (1.0 / Ft);
							beta = beta * bssrdf.weight * (1.0 / Ft);
						} else {
							betaSpatial = beta * bssrdf.weightSpatialNM / Ft;
							beta = beta * bssrdf.weightNM / Ft;
						}

						BDPTVertex entryV;
						entryV.type = BDPTVertex::SURFACE;
						// DL-247: the SSS exit adds no medium-scatter vertex; carry the
						// subpath's count, or the per-path `volBounces` gate under-counts
						// every strategy ending here.
						entryV.volumeBounces = volumeBounces;
						entryV.position = bssrdf.entryPoint;
						entryV.normal = bssrdf.entryNormal;
						entryV.geomNormal = bssrdf.entryGeomNormal;
						entryV.onb = bssrdf.entryONB;
						// DL-22: forward live derivatives, signals, texture coordinates,
						// and vertex color from BSSRDFSampling::SampleResult.
						entryV.derivatives = bssrdf.derivatives;
						entryV.signals = bssrdf.signals;
						entryV.txFootprint = bssrdf.txFootprint;
						entryV.ptCoord = bssrdf.ptCoord;
						entryV.ptCoord1 = bssrdf.ptCoord1;
						entryV.bHasTexCoord1 = bssrdf.bHasTexCoord1;
						entryV.ptObjIntersec = bssrdf.ptObjIntersec;
						entryV.vColor = bssrdf.vColor;
						entryV.bHasVertexColor = bssrdf.bHasVertexColor;
						entryV.pMaterial = ri.pMaterial;
						entryV.pObject = ri.pObject;
						entryV.pMediumObject = pMedObj_light;
						entryV.pMediumVol = pMed_light;
						// DL-49: the entry point shares the exit hit's exterior medium
						// (the continuation carries the same IOR stack).  Without this
						// the vertex kept the 1.0 default and every re-evaluation of its
						// Sw (PathVertexEval, the zero-exitance sweep) priced an air
						// interface.
						entryV.mediumIOR = vertices.back().mediumIOR;
						entryV.isDelta = false;
						entryV.isConnectible = true;
						entryV.isBSSRDFEntry = true;
						StoreThroughput<Tag>( entryV, betaSpatial );
						entryV.pdfFwd = bssrdf.pdfSurface;
						entryV.pdfRev = 0;
						// DL-09: the BSSRDF entry vertex records the tracked index its
						// throughput was priced to (the subsurface event is not a
						// straight segment, so no factor is paid across it).
						GradedIndexMedium::RecordVertex( iorStack, entryV.pGradedMedium, entryV.gradedIOR );
						vertices.push_back( entryV );

						pdfFwdPrev = bssrdf.cosinePdf;
						currentRay = bssrdf.scatteredRay;
						continue;
					}
					// Probe failed → no valid subsurface sample, terminate
					break;
				}
				// Chose reflection (probability R):
				// The SPF's kray already contains R, so throughput update
				// below gives beta *= R.  We need beta *= R/R = 1, so
				// compensate by dividing by R.
				if( R > NEARZERO ) {
					bssrdfReflectCompensation = 1.0 / R;
				}
			}
		}
		// --- Random-walk SSS (light subpath) ---
		else if( ri.pMaterial )
		{
			// Param resolution + front-face cosine differ Pel/NM and are
			// preserved exactly: Pel gates on static params with a geometric
			// front-face check + clamped shading Fresnel cosine; NM resolves
			// static-or-spectral params and uses the raw shading cosine.
			const RandomWalkSSSParams* pRW = nullptr;
			[[maybe_unused]] RandomWalkSSSParams rwParamsNM;
			Scalar cosIn = 0;
			bool rwGate = false;
			if constexpr( Traits::is_pel ) {
				if( ri.pMaterial->GetRandomWalkSSSParams() ) {
					const Vector3 wo_bss = -currentRay.Dir();
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
					const Scalar cosInGeom = ri.geometric.BSSRDFEntryFacing( wo_bss );
					const Scalar cosInShade = Vector3Ops::Dot( ri.geometric.vNormal, wo_bss );
					cosIn = r_max( fabs( cosInShade ), Scalar( NEARZERO ) );
					if( cosInGeom > NEARZERO ) {
						pRW = ri.pMaterial->GetRandomWalkSSSParams();
						rwGate = true;
					}
				}
			} else {
				pRW = ri.pMaterial->GetRandomWalkSSSParams();
				if( !pRW && ri.pMaterial->GetRandomWalkSSSParamsNM( tag.nm, rwParamsNM ) ) {
					pRW = &rwParamsNM;
				}
				cosIn = pRW ? Vector3Ops::Dot(
					ri.geometric.vNormal, -currentRay.Dir() ) : 0;
				if( pRW && cosIn > NEARZERO ) {
					rwGate = true;
				}
			}
			if( rwGate )
			{
				// DL-49: the RELATIVE index -- the material over the
				// exterior the ray arrived through (`ambientIOR`, the
				// IOR-stack top), the same interface the SPF reflection this
				// coin competes with, and RandomWalkSSS's refraction, use.  A
				// denser exterior past its critical angle is totally reflected.
				// DL-306: the exact dielectric law, the SPF reflection's own,
				// so the coin's reflect branch (weight R_spf / R) carries
				// exactly 1 and the two branches partition the interface.
				const Scalar etaRW = BSSRDFSampling::RelativeBoundaryIOR(
					pRW->ior, BSSRDFSampling::ExteriorIOR( ri.geometric ) );
				const Scalar Ft = BSSRDFSampling::BoundaryTransmission( cosIn, etaRW );
				const Scalar R = 1.0 - Ft;

				if( Ft > NEARZERO && sampler.Get1D() < Ft )
				{
					// The random walk consumes a variable number of
					// dimensions (up to ~7 per scatter × maxBounces).
					// Samplers with a fixed dimension budget (Sobol)
					// cannot tolerate this — use IndependentSampler.
					// Samplers without a budget (PSSMLT, independent)
					// are used directly so the walk stays in the
					// primary sample vector.
					IndependentSampler walkSampler( random );
					ISampler& rwSampler = sampler.HasFixedDimensionBudget()
						? static_cast<ISampler&>(walkSampler) : sampler;

					BSSRDFSampling::SampleResult bssrdf = RandomWalkSSS::SampleExit(
						ri.geometric, ri.pObject,
						pRW->sigma_a, pRW->sigma_s, pRW->sigma_t,
						pRW->g, pRW->ior, pRW->maxBounces, rwSampler, NmOrZero<Tag>( tag ), pRW->maxDepth );

					if( bssrdf.valid )
					{
						vertices.back().isDelta = true;

						// SampleExit does NOT include Ft(entry).
						// Coin flip selects with probability Ft, so the
						// physical Ft and the 1/Ft selection compensation
						// cancel: weight * Ft / Ft = weight.
						// Apply boundary filter (e.g. melanin double-pass).
						const Scalar bf = pRW->boundaryFilter;
						V betaSpatial;
						if constexpr( Traits::is_pel ) {
							betaSpatial = beta * bssrdf.weightSpatial * bf;
							beta = beta * bssrdf.weight * bf;
						} else {
							betaSpatial = beta * bssrdf.weightSpatialNM * bf;
							beta = beta * bssrdf.weightNM * bf;
						}

						BDPTVertex entryV;
						entryV.type = BDPTVertex::SURFACE;
						// DL-247: the SSS exit adds no medium-scatter vertex; carry the
						// subpath's count, or the per-path `volBounces` gate under-counts
						// every strategy ending here.
						entryV.volumeBounces = volumeBounces;
						entryV.position = bssrdf.entryPoint;
						entryV.normal = bssrdf.entryNormal;
						entryV.geomNormal = bssrdf.entryGeomNormal;
						entryV.onb = bssrdf.entryONB;
						// DL-22: forward live derivatives, signals, texture coordinates,
						// and vertex color from RandomWalkSSS::SampleExit.
						entryV.derivatives = bssrdf.derivatives;
						entryV.signals = bssrdf.signals;
						entryV.txFootprint = bssrdf.txFootprint;
						entryV.ptCoord = bssrdf.ptCoord;
						entryV.ptCoord1 = bssrdf.ptCoord1;
						entryV.bHasTexCoord1 = bssrdf.bHasTexCoord1;
						entryV.ptObjIntersec = bssrdf.ptObjIntersec;
						entryV.vColor = bssrdf.vColor;
						entryV.bHasVertexColor = bssrdf.bHasVertexColor;
						entryV.pMaterial = ri.pMaterial;
						entryV.pObject = ri.pObject;
						entryV.pMediumObject = pMedObj_light;
						entryV.pMediumVol = pMed_light;
						// DL-49: the entry point shares the exit hit's exterior medium
						// (the continuation carries the same IOR stack).  Without this
						// the vertex kept the 1.0 default and every re-evaluation of its
						// Sw (PathVertexEval, the zero-exitance sweep) priced an air
						// interface.
						entryV.mediumIOR = vertices.back().mediumIOR;

						// The random walk has no analytic area PDF for
						// the exit point — pdfSurface is a placeholder.
						// Mark the vertex as delta + non-connectible so
						// that (a) no connection strategy targets it,
						// and (b) the MIS ratio chain passes through
						// cleanly (remap0(0)/remap0(0) = 1).  The PT
						// path still does NEE at entry points via
						// EvaluateDirectLighting.
						entryV.isDelta = true;
						entryV.isConnectible = false;
						entryV.isBSSRDFEntry = true;
						StoreThroughput<Tag>( entryV, betaSpatial );
						entryV.pdfFwd = 0;
						entryV.pdfRev = 0;
						// DL-09: the BSSRDF entry vertex records the tracked index its
						// throughput was priced to (the subsurface event is not a
						// straight segment, so no factor is paid across it).
						GradedIndexMedium::RecordVertex( iorStack, entryV.pGradedMedium, entryV.gradedIOR );
						vertices.push_back( entryV );

						pdfFwdPrev = bssrdf.cosinePdf;
						currentRay = bssrdf.scatteredRay;
						continue;
					}
					break;
				}
				if( R > NEARZERO ) {
					bssrdfReflectCompensation = 1.0 / R;
				}
			}
		}
		// --- End BSSRDF sampling ---

		// DL-307: see the eye twin -- no lobe and no guide draw to continue on.
		if( !hasLobe && !guidedVertex ) {
			break;
		}

		const IORStack* traceIorStack = pScat->ior_stack ? pScat->ior_stack : &iorStack;
		IORStack guidedIorStack( iorStack );
#ifdef RISE_ENABLE_OPENPGL
		// --- Path guiding (light subpath) ---
		// Query the shared guiding field at each light subpath surface vertex
		// and blend with BSDF sampling using RIS or one-sample MIS.  The
		// shared field's incident-radiance distribution approximates the
		// reciprocal scattering distribution for diffuse-dominated transport.
		// DL-67: the continuation is priced on the ONE partition
		// `BDPTGuidedContinuation` documents -- the eye generator's twin
		// block above has the vertex-gate rationale.  `pdfFwdPrev` is the
		// aggregate `ISPF::Pdf()` at `scatDir` (DL-69's one rule, now
		// stated for the guided branches too), and the density the
		// continuation's weight corresponds to reaches OpenPGL through
		// `guidingPdfDirectionIn` and serves as `pdfFwd`'s zero-aggregate
		// fallback.
		bool usedGuidedDirection = false;
		V guidedF = Traits::zero();
		Vector3 guidedDir;
		Scalar guidedEffectivePdf = 0;
		Scalar keptPartitionScale = 1;
		Scalar aggregateAtScatDir = -1;
		bool guidedTerminate = false;

		if( guidedVertex )
		{
			// Use a separate thread_local handle from the eye subpath's
			// guideDist to avoid cross-contamination.
			static thread_local GuidingDistributionHandle lightGuideDist;
			if( pLightGuidingField->InitDistribution( lightGuideDist, v.position, sampler.Get1D() ) )
			{
				pLightGuidingField->ApplyCosineProduct(
					lightGuideDist,
					GuidingCosineNormal( v.normal, currentRay.Dir() ) );

				const BDPTVertex& gv = vertices.back();
				const Vector3 wiIn = -currentRay.Dir();
				BDPTGuidedChoice<V> choice;
				BDPTGuidedContinuation<Tag, V>(
					*pLightGuidingField, lightGuideDist, *pScat, hasLobe, selectProb,
					Traits::max_value( KrayValue<Tag>( *pScat ) ) / selectProb, guidingAlpha,
					guidingSamplingType, v.normal, sampler,
					[&]( const Vector3& w ) -> V {
						return PathValueOps::EvalBSDFAtVertex<Tag>( gv, wiIn, w, tag );
					},
					[&]( const Vector3& w ) -> Scalar {
						return PathValueOps::EvalPdfAtVertex<Tag>( gv, wiIn, w, tag );
					},
					choice );
				guidedTerminate = choice.terminate;
				usedGuidedDirection = choice.substituted;
				keptPartitionScale = choice.keptScale;
				aggregateAtScatDir = choice.aggAtTrace;
				if( usedGuidedDirection ) {
					guidedDir = choice.dir;
					guidedF = choice.f;
					guidedEffectivePdf = choice.equivPdf;
					// See the eye twin.
					guideTemplateRay.ray = Ray( ri.geometric.ptIntersection, guidedDir );
					pScat = &guideTemplateRay;
					vertices.back().isDelta = false;
				}
			}
		}
		if( guidedTerminate || !( hasLobe || usedGuidedDirection ) ) {
			break;
		}
		if( usedGuidedDirection ) {
			traceIorStack = PathTransportUtilities::GuidedContinuationIORStack(
				*pScat, iorStack, ri, guidedDir, guidedIorStack );
		}
		// See the eye twin: the density the continuation's weight
		// corresponds to, including lobe selection.
		const Scalar equivScatterPdf = usedGuidedDirection ? guidedEffectivePdf :
			( keptPartitionScale > 0 ? selectProb * pScat->pdf / keptPartitionScale : Scalar( 0 ) );
#endif
		// --- End light subpath path guiding ---

		// Compute effective scatter direction and PDF.
		Vector3 scatDir = pScat->ray.Dir();
		Scalar effectivePdf = pScat->pdf;

#ifdef RISE_ENABLE_OPENPGL
		if( usedGuidedDirection ) {
			scatDir = guidedDir;
			effectivePdf = guidedEffectivePdf;
		}
#endif

		// DL-126, light-subpath twin of the eye generator's block -- read
		// its comment for the derivation.  `BioSpecSkinMaterial` /
		// `GenericHumanTissueMaterial` have `GetBSDF() == 0`
		// (`vertices.back().isConnectible` is already false) and their
		// SPFs never populate `ScatteredRay::pdf` (default 0), so
		// `effectivePdf <= 0` here and, further below,
		// `PositiveMagnitude(f) <= 0` used to `break` the light subpath at
		// any such material too.  Neither gate is meaningful for a vertex
		// no rival strategy can ever reconstruct (isConnectible false);
		// the guiding block above is already gated on
		// `vertices.back().isConnectible`, so `usedGuidedDirection` is
		// false and `keptPartitionScale` 1 here.
		const bool nullBSDFContinuation =
			!pScat->isDelta && !vertices.back().isConnectible;

		if( effectivePdf <= 0 && !nullBSDFContinuation ) {
			break;
		}

		// Per-type bounce limits
		if( PathTransportUtilities::ExceedsBounceLimitForType(
				pScat->type, diffuseBounces, glossyBounces,
				transmissionBounces, translucentBounces, stabilityConfig ) ) {
			break;
		}

		// Throughput update: beta *= f * |cos| / pdf.  localScatteringWeight
		// (RISEPel both tags) captures f * |cos| / pdf for the guiding store
		// which both Pel and NM write on the light subpath; NM broadcasts the
		// scalar weight.  HWSS maintains per-wavelength companions.
		//
		// Snapshot pre-scatter HWSS throughput so the RR below can compare
		// max(pre) vs max(post) over active wavelengths.
		[[maybe_unused]] Scalar hwssBetaNMPre[SampledWavelengths::N] = {};
		if constexpr( Traits::is_nm ) {
			if( pSwlHWSS ) {
				for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
					hwssBetaNMPre[w] = hwssBetaNM[w];
				}
			}
		}

		RISEPel localScatteringWeight( 0, 0, 0 );
		// DL-67: see the eye twin -- a kept delta lobe at a guided vertex
		// carries its technique's firing probability too.
#ifdef RISE_ENABLE_OPENPGL
		const Scalar deltaGuideScale = keptPartitionScale;
#else
		const Scalar deltaGuideScale = 1;
#endif
		if( pScat->isDelta ) {
			// For delta scattering, kray already incorporates the right factor
			// but must be divided by the lobe selection probability.
			beta = beta * KrayValue<Tag>( *pScat ) * (bssrdfReflectCompensation * deltaGuideScale / selectProb);
			if constexpr( Traits::is_nm ) {
				if( pSwlHWSS ) {
					const Scalar deltaScale = pScat->krayNM * bssrdfReflectCompensation * deltaGuideScale / selectProb;
					hwssBetaNM[0] = beta;
					for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
						if( pSwlHWSS->terminated[w] ) continue;
						hwssBetaNM[w] = hwssBetaNM[w] * deltaScale;
					}
				}
			}
		} else if( nullBSDFContinuation ) {
			// DL-126, light-subpath twin of the eye branch above.  Mirror
			// the DELTA branch's own formula exactly: no aggregate BSDF,
			// no pdf division, no guiding substitution.
			if( PositiveMagnitude<Tag>( KrayValue<Tag>( *pScat ) ) <= 0 ) {
				break;
			}
			beta = beta * KrayValue<Tag>( *pScat ) * (bssrdfReflectCompensation / selectProb);
			if constexpr( Traits::is_nm ) {
				if( pSwlHWSS ) {
					const Scalar deltaScale = pScat->krayNM * bssrdfReflectCompensation / selectProb;
					hwssBetaNM[0] = beta;
					for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
						if( pSwlHWSS->terminated[w] ) continue;
						hwssBetaNM[w] = hwssBetaNM[w] * deltaScale;
					}
				}
			}
		} else {
			V f;
#ifdef RISE_ENABLE_OPENPGL
			// Reuse the BSDF evaluation from guiding RIS candidate selection
			// when a guided direction was chosen (avoids redundant eval).
			f = usedGuidedDirection ? guidedF :
				PathValueOps::EvalBSDFAtVertex<Tag>( vertices.back(), -currentRay.Dir(), scatDir, tag );
#else
			f = PathValueOps::EvalBSDFAtVertex<Tag>( vertices.back(), -currentRay.Dir(), scatDir, tag );
#endif
			const Scalar cosTheta = fabs( Vector3Ops::Dot(
				scatDir, ri.geometric.vNormal ) );

			// Retained as a PATH-TERMINATION gate only, for a CONNECTIBLE
			// material (the null-BSDF, non-connectible case is handled in
			// the branch above instead -- DL-126) whose SELECTED lobe's
			// aggregate BSDF value happens to be zero at the sampled
			// direction.
			if( PositiveMagnitude<Tag>( f ) <= 0 ) {
				break;
			}
#ifdef RISE_ENABLE_OPENPGL
			const Scalar scatterPdf = equivScatterPdf;
#else
			const Scalar scatterPdf = selectProb * effectivePdf;
#endif

			// DL-69, light-subpath twin of the eye generator's block --
			// read its comment for the derivation.  Same defect
			// (aggregate `EvalBSDFAtVertex` over a per-lobe
			// `scatterPdf`), same fix (the selected lobe's own `kray_I`
			// over the same lobe's selection probability, matching this
			// file's delta branch immediately above and PT's ordinary
			// initialization), same DL-67 guided pricing as the eye twin.
			Scalar krayScale = bssrdfReflectCompensation / selectProb;
			bool useKray = true;
#ifdef RISE_ENABLE_OPENPGL
			if( usedGuidedDirection ) {
				useKray = false;
			} else {
				krayScale = krayScale * keptPartitionScale;
			}
#endif
			if constexpr( Traits::is_pel ) {
				localScatteringWeight = useKray ?
					( KrayValue<Tag>( *pScat ) * krayScale ) :
					( f * (bssrdfReflectCompensation * cosTheta / scatterPdf) );
				beta = beta * localScatteringWeight;
			} else {
				// The guided fallback keeps the original left-to-right
				// multiplication order.  (This comment used to claim the
				// NM branch's "association" was thereby PRESERVED; that
				// was overstated -- the pre-DL-69 NM branch multiplied
				// `beta * f * comp * cos / scatterPdf` left to right,
				// while this one forms `wHero` first and then
				// `beta * wHero`, so the two differ at the ulp.  The
				// order is kept because there is no reason to perturb
				// it, not because it is bit-identical.)
				const Scalar wHero = useKray ?
					( KrayValue<Tag>( *pScat ) * krayScale ) :
					( f * bssrdfReflectCompensation * cosTheta / scatterPdf );
				// DL-125 -- see the eye twin.
				vertices.back().scatterType =
					useKray ? pScat->type : ScatteredRay::eRayUnknown;
				localScatteringWeight = RISEPel( wHero, wHero, wHero );
				beta = beta * wHero;
				if( pSwlHWSS ) {
					const Scalar invScale = bssrdfReflectCompensation * cosTheta / scatterPdf;
					hwssBetaNM[0] = beta;
					for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
						if( pSwlHWSS->terminated[w] ) continue;
						// Same selected-lobe contract as PT and the eye twin:
						// EvaluateKrayNM first (DL-125 closed), aggregate
						// fallback for matching aggregate-density response
						// or guiding substitution. CompositeSPF's walker-emitted
						// rays and per-branch composites remain DL-221; Translucent's entry/exit lobes are supported,
						// while unsupported types may decline and warn.
						Scalar compScale = -1;
						if( useKray && pSPF ) {
							const Scalar krayW = pSPF->EvaluateKrayNM(
								ri.geometric, pScat->ray.Dir(), pScat->type,
								pSwlHWSS->lambda[w], iorStack,
								pScat->isDelta ? -1.0 : pScat->pdf );
							if( krayW >= 0 ) {
								compScale = krayW * krayScale;
							}
						}
						if( compScale < 0 ) {
							// DL-125 -- see the eye twin's comment.
							if( useKray ) {
								NotePerLobeDensityCompanionFallback( pSPF );
							}
							const Scalar fw = PathVertexEval::EvalBSDFAtVertexNM(
								vertices.back(), -currentRay.Dir(), scatDir, pSwlHWSS->lambda[w] );
							compScale = fw * invScale;
						}
						hwssBetaNM[w] = hwssBetaNM[w] * compScale;
					}
				}
			}
		}

		// DL-224: light throughput transports importance. SPF kray is a
		// radiance-mode f_s*cos(Ns,out)/pdf weight, so changing to the
		// adjoint geometric-area kernel needs this projected-area ratio.
		// It changes contributions only: the sampler and every MIS density
		// retain their geometric-area Jacobians. Apply before roulette so
		// its survival probability observes the actual transported weight.
		const Scalar shadingAdjoint = PathVertexEval::ImportanceShadingNormalFactor(
			vertices.back(), -currentRay.Dir(), scatDir );
		beta = beta * shadingAdjoint;
		localScatteringWeight = localScatteringWeight * shadingAdjoint;
		if constexpr( Traits::is_nm ) {
			if( pSwlHWSS ) {
				for( unsigned int w = 0; w < SampledWavelengths::N; ++w ) {
					hwssBetaNM[w] *= shadingAdjoint;
				}
			}
		}

		// Russian Roulette — configurable depth threshold and floor.  HWSS
		// uses MAX throughput over active wavelengths (prevents hero-driven RR
		// from amplifying companions on rare survival).
		Scalar rrCurrMax = Traits::max_value( beta );
		Scalar rrPrevMax = Traits::max_value( VertexThroughput<Tag>( vertices.back() ) );
		if constexpr( Traits::is_nm ) {
			if( pSwlHWSS ) {
				for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
					if( pSwlHWSS->terminated[w] ) continue;
					const Scalar p = fabs( hwssBetaNMPre[w] );
					if( p > rrPrevMax ) rrPrevMax = p;
					const Scalar c = fabs( hwssBetaNM[w] );
					if( c > rrCurrMax ) rrCurrMax = c;
				}
			}
		}
		const PathTransportUtilities::RussianRouletteResult rr =
			PathTransportUtilities::EvaluateRussianRoulette(
				depth, stabilityConfig.rrMinDepth, stabilityConfig.rrThreshold,
				rrCurrMax, rrPrevMax,
				sampler.Get1D() );
		if( rr.terminate ) {
			break;
		}
		if( rr.survivalProb < 1.0 ) {
			const Scalar rrScale = Scalar( 1 ) / rr.survivalProb;
			beta = beta * rrScale;
			if constexpr( Traits::is_nm ) {
				if( pSwlHWSS ) {
					for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
						hwssBetaNM[w] = hwssBetaNM[w] * rrScale;
					}
				}
			}
		}

#ifdef RISE_ENABLE_OPENPGL
		// Light subpath guiding: record scatter direction and scattering
		// weight for training.  Mirrors eye subpath metadata at ~line 2293.
		if( maxLightGuidingDepth > 0 )
		{
			vertices.back().guidingHasDirectionIn = true;
			vertices.back().guidingDirectionIn = scatDir;
			vertices.back().guidingPdfDirectionIn = equivScatterPdf;
			vertices.back().guidingScatteringWeight = localScatteringWeight;
			vertices.back().guidingRussianRouletteSurvivalProbability = rr.survivalProb;
			vertices.back().guidingEta =
				traceIorStack->top() > NEARZERO ? traceIorStack->top() : Scalar( 1 );
			vertices.back().guidingRoughness = pScat->isDelta ?
				Scalar( 0.0 ) :
				(pScat->type == ScatteredRay::eRayDiffuse ? Scalar( 1.0 ) : Scalar( 0.5 ));

			// Reverse PDF and scattering weight for training.  When light
			// subpath segments are recorded in reverse order, the segment's
			// directionIn becomes guidingDirectionOut (toward the light).
			// pdfDirectionIn and scatteringWeight must correspond to that
			// reversed directionIn, not the forward scatter direction.
			if( !pScat->isDelta )
			{
				const Scalar revPdf = PathValueOps::EvalPdfAtVertex<Tag>(
					vertices.back(), scatDir, -currentRay.Dir(), tag );
				vertices.back().guidingReversePdfDirectionIn = revPdf;

				if( revPdf > NEARZERO )
				{
					// Reciprocal BSDF: f(wi->wo) = f(wo->wi).  Reuse the forward
					// BSDF eval; only cos and PDF change.  NM broadcasts the scalar
					// weight into the RISEPel guiding field.
					const V f = usedGuidedDirection ? guidedF :
						PathValueOps::EvalBSDFAtVertex<Tag>( vertices.back(), -currentRay.Dir(), scatDir, tag );
					const Scalar cosIncoming = fabs( Vector3Ops::Dot(
						-currentRay.Dir(), ri.geometric.vNormal ) );
					if constexpr( Traits::is_pel ) {
						vertices.back().guidingReverseScatteringWeight =
							f * (bssrdfReflectCompensation * cosIncoming / revPdf);
					} else {
						const Scalar revWeight = f * bssrdfReflectCompensation * cosIncoming / revPdf;
						vertices.back().guidingReverseScatteringWeight =
							RISEPel( revWeight, revWeight, revWeight );
					}
				}
			}
		}
#endif

		// Store the forward pdf for the next vertex (solid angle measure).
		//
		// DL-69, second half -- light-subpath twin of the eye
		// generator's `pdfFwdPrev` block; read its comment for the
		// derivation.  `pdfFwd` is the MIS ratio chain's density, and
		// the reverse side of that chain evaluates the aggregate,
		// direction-only `ISPF::Pdf()`; `selectProb * pScat->pdf` is
		// the realization-dependent sampling density and belongs only
		// to `guidingPdfDirectionIn` (set above, unchanged -- OpenPGL
		// training input).
		// One shared reconstruction for both density queries at this
		// vertex -- see the eye generator's twin block for the rationale
		// and the lifetime contract (review P2-5).
		PathVertexEval::VertexPdfContext pdfCtx( vertices.back() );

		// DL-67: the eye twin's one rule -- aggregate `ISPF::Pdf()` at
		// the traced direction, guided or not; zero-aggregate fallback =
		// the density the continuation's weight corresponds to.
#ifdef RISE_ENABLE_OPENPGL
		pdfFwdPrev = equivScatterPdf;
#else
		pdfFwdPrev = selectProb * effectivePdf;
#endif
		if( !pScat->isDelta ) {
#ifdef RISE_ENABLE_OPENPGL
			const Scalar misFwdPdf = aggregateAtScatDir >= 0 ? aggregateAtScatDir :
				PathValueOps::EvalPdfAtVertex<Tag>( pdfCtx, -currentRay.Dir(), scatDir, tag );
#else
			const Scalar misFwdPdf = PathValueOps::EvalPdfAtVertex<Tag>(
				pdfCtx, -currentRay.Dir(), scatDir, tag );
#endif
			if( misFwdPdf > NEARZERO ) {
				pdfFwdPrev = misFwdPdf;
			}
		}

		// In Veach's formulation, delta vertices should be "transparent" in the MIS walk
		if( pScat->isDelta ) {
			pdfFwdPrev = 0;
		}

		// Update the previous vertex's pdfRev
		// pdfRev of vertex[n-1] = pdf of sampling the reverse direction at vertex[n]
		if( vertices.size() >= 2 ) {
			BDPTVertex& prev = vertices[ vertices.size() - 2 ];

			// Reverse PDF: EvalPdfAtVertex returns 0 for delta interactions
			// (SPF::Pdf() returns 0 for Dirac distributions).  This is correct;
			// remap0 in MISWeight maps the zero to 1 so the ratio chain
			// propagates through delta vertices without dying.
			const Scalar revPdfSA = PathValueOps::EvalPdfAtVertex<Tag>(
				pdfCtx,
				scatDir,
				-currentRay.Dir(),
				tag
				);

			// Convert to area measure at prev.  Compute the geometric
			// cosine ONLY in the surface/light branch — medium vertices
			// don't populate geomNormal (default Vector3 is (0,0,0)) so
			// reading it before the type guard would consume meaningless
			// data even though the result is later ignored.
			const Scalar d2 = distSq;
			if( prev.type == BDPTVertex::MEDIUM ) {
				prev.pdfRev = BDPTUtilities::SolidAngleToAreaMedium( revPdfSA, prev.sigma_t_scalar, d2 );
			} else {
				const Scalar absCosAtPrev = fabs(
					Vector3Ops::Dot( prev.geomNormal, currentRay.Dir() ) );
				prev.pdfRev = BDPTUtilities::SolidAngleToArea( revPdfSA, absCosAtPrev, d2 );
			}
			if( pScat->isDelta ) { prev.pdfRev = 0; }
		}

		// Advance to next ray
#ifdef RISE_ENABLE_OPENPGL
		if( usedGuidedDirection ) {
			currentRay = Ray( pScat->ray.origin, guidedDir );
		} else {
			currentRay = pScat->ray;
		}
#else
		currentRay = pScat->ray;
#endif
		currentRay.Advance( BDPT_RAY_EPSILON );
		if( traceIorStack != &iorStack ) {
			iorStack = *traceIorStack;
		}
	}

	// Record subpath boundary (single contiguous range now that
	// path-tree branching has been excised).
	subpathStarts.push_back( static_cast<uint32_t>( vertices.size() ) );

	return static_cast<unsigned int>( vertices.size() );
}

} // anonymous namespace (GenerateLightSubpath F2b)

//////////////////////////////////////////////////////////////////////
// GenerateLightSubpathNM
//////////////////////////////////////////////////////////////////////

unsigned int BDPTIntegrator::GenerateLightSubpathNM(
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	std::vector<BDPTVertex>& vertices,
	std::vector<uint32_t>& subpathStarts,
	const Scalar nm,
	const RandomNumberGenerator& random,
	const SampledWavelengths* pSwlHWSS
	) const
{
	return GenerateLightSubpathImpl<NMTag>(
		maxLightDepth, stabilityConfig, pLightSampler,
#ifdef RISE_ENABLE_OPENPGL
		pLightGuidingField, maxLightGuidingDepth, guidingAlpha, guidingSamplingType,
#endif
		scene, caster, sampler, random,
		vertices, subpathStarts, NMTag( nm ), pSwlHWSS );
}

//////////////////////////////////////////////////////////////////////
// GenerateEyeSubpathNM
//////////////////////////////////////////////////////////////////////

unsigned int BDPTIntegrator::GenerateEyeSubpathNM(
	const RuntimeContext& rc,
	const Ray& cameraRay,
	const Point2& screenPos,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	std::vector<BDPTVertex>& vertices,
	std::vector<uint32_t>& subpathStarts,
	const Scalar nm,
	const SampledWavelengths* pSwlHWSS,
	PixelAOV* pPrimaryAOV
	) const
{
	return GenerateEyeSubpathImpl<NMTag>(
		maxEyeDepth, stabilityConfig, pLightSampler,
#ifdef RISE_ENABLE_OPENPGL
		pGuidingField, maxGuidingDepth, guidingAlpha, guidingSamplingType,
#endif
		rc, cameraRay, screenPos, scene, caster, sampler,
		vertices, subpathStarts, NMTag( nm ), pSwlHWSS, pPrimaryAOV );
}

//////////////////////////////////////////////////////////////////////
// ConnectAndEvaluateNM
//////////////////////////////////////////////////////////////////////

BDPTIntegrator::ConnectionResultNM BDPTIntegrator::ConnectAndEvaluateNM(
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<BDPTVertex>& eyeVerts,
	unsigned int s,
	unsigned int t,
	const IScene& scene,
	const IRayCaster& caster,
	const ICamera& camera,
	const Point2& cameraLensSample,
	const Scalar nm
	) const
{
	return ConnectAndEvaluateImpl<NMTag>(
		*this, pLightSampler, lightVerts, eyeVerts, s, t, scene, caster, camera,
		cameraLensSample, NMTag( nm ) );
}

//////////////////////////////////////////////////////////////////////
// EvaluateAllStrategiesNM
//////////////////////////////////////////////////////////////////////

std::vector<BDPTIntegrator::ConnectionResultNM> BDPTIntegrator::EvaluateAllStrategiesNM(
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<BDPTVertex>& eyeVerts,
	const IScene& scene,
	const IRayCaster& caster,
	const ICamera& camera,
	const Point2& cameraLensSample,
	const Scalar nm
	) const
{
	return EvaluateAllStrategiesImpl<NMTag>(
		*this, lightVerts, eyeVerts, scene, caster, camera, cameraLensSample, nullptr,
#ifdef RISE_ENABLE_OPENPGL
		pCompletePathGuide, completePathStrategySelectionEnabled, completePathStrategySampleCount,
		&strategySelectionPathCount, &strategySelectionCandidateCount, &strategySelectionEvaluatedCount,
		pGuidingField, &guidingTrainingStats, &guidingTrainingStatsMutex,
		pLightGuidingField, maxLightGuidingDepth,
#endif
		NMTag( nm ) );
}

//////////////////////////////////////////////////////////////////////
// RecomputeSubpathThroughputNM — HWSS companion wavelength
// re-evaluation.
//
// Adjusts every vertex's throughputNM from heroNM to companionNM.
//
// Vertex storage convention (GenerateLight/EyeSubpathNM):
//   vertex[i].throughputNM includes scatters at vertices 0..(i-1)
//   but NOT the scatter at vertex i.  The scatter at vertex i
//   determines the direction toward vertex i+1.
//
// Therefore the BSDF ratio at vertex i must be folded into the
// cumulativeRatio AFTER applying the ratio to vertex i (so that
// it affects vertex i+1 onwards).
//
// Light endpoint (v[0]): throughputNM = Le / pdfPos.
//   The emission Le is wavelength-dependent.  The emission ratio
//   is applied to v[0] and propagates forward.
//
// Camera endpoint (v[0]): throughputNM = 1.  No spectral
//   dependence; cumulativeRatio stays 1.0.
//
// Delta vertices: ratio = 1.0.  This is exact for non-dispersive
//   specular (same IOR at all wavelengths).  Dispersive delta
//   vertices should have companions terminated upstream — see
//   CheckDispersiveTermination().
//
// Medium vertices: phase function is wavelength-independent
//   (ratio = 1.0 for scattering).  Segment transmittance ratio
//   is a documented approximation.
//////////////////////////////////////////////////////////////////////
void BDPTIntegrator::RecomputeSubpathThroughputNM(
	std::vector<BDPTVertex>& verts,
	bool isLightPath,
	Scalar heroNM,
	Scalar companionNM,
	const IScene& scene,
	const IRayCaster& caster
	) const
{
	if( verts.empty() ) {
		return;
	}

	// Accumulated correction ratio.  Each vertex's scatter
	// contributes to the ratio for SUBSEQUENT vertices.
	Scalar cumulativeRatio = 1.0;

	for( unsigned int i = 0; i < verts.size(); i++ )
	{
		BDPTVertex& v = verts[i];

		// ---- Phase 1: endpoint emission ratio (applies to v[0] itself) ----
		if( i == 0 && isLightPath && v.type == BDPTVertex::LIGHT )
		{
			if( verts.size() >= 2 )
			{
				const Vector3 outDir = Vector3Ops::Normalize(
					Vector3Ops::mkVector3( verts[1].position, v.position ) );
				const Scalar heroLe = EvalEmitterRadiance<NMTag>( v, outDir, nullptr, NMTag( heroNM ) );
				const Scalar compLe = EvalEmitterRadiance<NMTag>( v, outDir, nullptr, NMTag( companionNM ) );
				if( heroLe > NEARZERO ) {
					cumulativeRatio *= compLe / heroLe;
				} else {
					cumulativeRatio = 0;
				}
			}
		}

		// ---- Phase 2: apply accumulated ratio to this vertex ----
		v.throughputNM *= cumulativeRatio;

		// ---- Phase 3: compute this vertex's scatter ratio ----
		// This ratio represents the scatter AT vertex i that creates
		// the direction toward vertex i+1.  It applies to v[i+1]
		// onwards, so we fold it into cumulativeRatio AFTER updating
		// v[i] above.
		if( i + 1 < verts.size() && i > 0 &&
			v.type == BDPTVertex::SURFACE && !v.isDelta )
		{
			const Vector3 dirIn = Vector3Ops::Normalize(
				Vector3Ops::mkVector3( v.position, verts[i-1].position ) );
			const Vector3 dirOut = Vector3Ops::Normalize(
				Vector3Ops::mkVector3( verts[i+1].position, v.position ) );

			// DL-219: Re-evaluate forward and reverse sampling PDFs at companionNM.
			// Subpath vertices were generated at heroNM, so verts[i+1].pdfFwd and
			// verts[i-1].pdfRev reflect hero proposal densities. For companion MIS
			// weights (e.g. s=0 vs s=1 or general connections) to form a consistent
			// partition of unity at companionNM, these densities must be scaled by
			// p(companionNM) / p(heroNM). Geometric Jacobians (|cos|/dist^2) cancel
			// identically between hero and companion.
			{
				PathVertexEval::VertexPdfContext pdfCtx( v );

				const Vector3 dirToPrev = -dirIn;
				const Vector3 dirToNext = dirOut;

				const Scalar fwdPdfHero = PathVertexEval::EvalPdfAtVertexNM( pdfCtx, dirToPrev, dirToNext, heroNM );
				const Scalar fwdPdfComp = PathVertexEval::EvalPdfAtVertexNM( pdfCtx, dirToPrev, dirToNext, companionNM );
				if( fwdPdfHero > NEARZERO ) {
					verts[i+1].pdfFwd *= ( fwdPdfComp / fwdPdfHero );
				} else if( fwdPdfComp <= NEARZERO ) {
					verts[i+1].pdfFwd = 0;
				}

				const Scalar revPdfHero = PathVertexEval::EvalPdfAtVertexNM( pdfCtx, dirToNext, dirToPrev, heroNM );
				const Scalar revPdfComp = PathVertexEval::EvalPdfAtVertexNM( pdfCtx, dirToNext, dirToPrev, companionNM );
				if( revPdfHero > NEARZERO ) {
					verts[i-1].pdfRev *= ( revPdfComp / revPdfHero );
				} else if( revPdfComp <= NEARZERO ) {
					verts[i-1].pdfRev = 0;
				}
			}

			// DL-125.  THE SELECTED LOBE'S OWN companion/hero kray ratio,
			// when the SPF can supply it.
			//
			// The aggregate-BSDF ratio below reproduces the selected-lobe
			// kray ratio only when `kray_I(lambda_c)/kray_I(lambda_h)`
			// equals `f_agg(lambda_c)/f_agg(lambda_h)`. These CAN differ
			// at a multi-lobe SPF, but common spectral dependence can
			// make them coincide:
			// `kray_I = f_I cos / p_I` is the SELECTED lobe's own
			// spectrum over the SELECTED lobe's own density, while
			// `f_agg` blends every lobe's spectrum -- so a material
			// whose diffuse and specular reflectances have different
			// spectra, or whose lobe density itself varies with
			// wavelength (a spectral roughness / isotropy / alpha /
			// exponent painter), can be priced with the wrong companion
			// weight. A selected-lobe kray ratio still uses each queried
			// wavelength's own proposal density; DL-216 separately tracks
			// that shape-dependent bias. The aggregate/lobe pairing is
			// the one DL-69 removed from the hero path and
			// DL-125 removed from PT's own HWSS body; THIS function is
			// the render-visible companion pricing for BDPT, VCM and MLT
			// alike (all three spectral rasterizers call it), unlike the
			// two `hwssBetaNM` ladders in the subpath generators, which
			// DL-126's review round 2 established are read only for
			// Russian roulette and guiding training.
			//
			// The aggregate fallback below is KEPT for every SPF that
			// declines (`EvaluateKrayNM` < 0). It matches aggregate-density
			// sampling whose response is represented by the aggregate BSDF:
			// CoatedSPF / FabricSPF / WeaveSPF / GGXSPF / CookTorranceSPF
			// deliberately use this path rather than an override --
			// and for a guiding-SUBSTITUTED direction, where
			// `scatterType` is deliberately left `eRayUnknown`.
			//
			// `isBSSRDFEntry` is excluded explicitly (review round 1,
			// P3).  It is a belt-and-braces guard, not a live fix: a
			// BSSRDF entry vertex is pushed by the eye walk's own
			// diffusion block and never passes through the scatter
			// branch that stamps `scatterType`, so it still carries the
			// default `eRayUnknown` and would take the aggregate branch
			// anyway.  Naming it here means a future stamp at that site
			// cannot silently start pricing a diffusion-profile ENTRY
			// vertex through the surface SPF's lobes (DL-207's own
			// lesson: the entry vertex's position/normal are the entry
			// point, not the camera-visible exit hit).
			Scalar lobeRatio = -1;
			if( v.pMaterial && !v.isBSSRDFEntry &&
			    v.scatterType != ScatteredRay::eRayUnknown )
			{
				const ISPF* pVertSPF = v.pMaterial->GetSPF();
				if( pVertSPF )
				{
					// The walk travelled prev -> v -> next on BOTH
					// subpaths, so the sampler's own incoming direction
					// at `v` is `v - prev` and its outgoing is
					// `next - v`, whichever side generated the subpath.
					// (`EvalBSDFAtVertex`'s wi/wo swap below is about the
					// BSDF's radiance-vs-importance argument convention;
					// `kray` is defined by the SAMPLER, which always
					// measured against `ri.ray.Dir()`.)

					// Preserve the sampler's live Beer distance, including ray
					// advances. Predecessor position is not the live origin.
					// This does not change connection reconstruction (DL-223).
					Ray inRay( Point3Ops::mkPoint3( v.position,
						-dirIn * v.scatterIncomingDistance ), dirIn );
					RayIntersectionGeometric rig( inRay, nullRasterizerState );
					PathVertexEval::PopulateRIGFromVertex( v, rig );

					IORStack vertexIor( 1.0 );
					BuildVertexIORStack( v, vertexIor );

					// DL-216: form pure BSDF ratio f_comp / f_hero through EvaluateLobeFNM
					// where hero density cancels out completely, avoiding redundant
					// density evaluations. Fall back to EvaluateKrayNM ratio if unimplemented.
					const Scalar fHero = pVertSPF->EvaluateLobeFNM(
						rig, dirOut, v.scatterType, heroNM, vertexIor );
					const Scalar fComp = pVertSPF->EvaluateLobeFNM(
						rig, dirOut, v.scatterType, companionNM, vertexIor );

					if( fHero >= 0 && fComp >= 0 ) {
						lobeRatio = ( fHero > NEARZERO ) ? ( fComp / fHero ) : 0;
					} else {
						const Scalar krayHero = pVertSPF->EvaluateKrayNM(
							rig, dirOut, v.scatterType, heroNM, vertexIor );
						const Scalar krayComp = pVertSPF->EvaluateKrayNM(
							rig, dirOut, v.scatterType, companionNM, vertexIor );

						if( krayHero >= 0 && krayComp >= 0 ) {
							lobeRatio = ( krayHero > NEARZERO ) ? ( krayComp / krayHero ) : 0;
						}
					}
				}
			}

			if( lobeRatio >= 0 )
			{
				cumulativeRatio *= lobeRatio;
			}
			else if( v.pMaterial && v.pMaterial->GetBSDF() )
			{
				// EvalBSDFAtVertex expects wi and wo BOTH pointing AWAY from
				// the surface (wi toward light, wo toward viewer); it
				// negates wo internally to build the incoming ray (see
				// BDPTIntegrator.h DIRECTION CONVENTIONS).  Both directions
				// must therefore point FROM v TOWARD the neighbour, matching
				// how GenerateEye/LightSubpath pass (scatDir,
				// -currentRay.Dir()).  dirToPrev = prev - v.
				const Vector3 dirToPrev = Vector3Ops::Normalize(
					Vector3Ops::mkVector3( verts[i-1].position, v.position ) );
				const Vector3 dirToNext = Vector3Ops::Normalize(
					Vector3Ops::mkVector3( verts[i+1].position, v.position ) );

				// Light subpath: wi = toward light (prev), wo = toward eye (next)
				// Eye subpath:   wi = toward light (next), wo = toward eye (prev)
				Vector3 wi, wo;
				if( isLightPath ) {
					wi = dirToPrev;
					wo = dirToNext;
				} else {
					wi = dirToNext;
					wo = dirToPrev;
				}

				const Scalar heroF = PathValueOps::EvalBSDFAtVertex<NMTag>( v, wi, wo, NMTag( heroNM ) );
				const Scalar compF = PathValueOps::EvalBSDFAtVertex<NMTag>( v, wi, wo, NMTag( companionNM ) );

				if( heroF > NEARZERO ) {
					cumulativeRatio *= compF / heroF;
				} else {
					cumulativeRatio = 0;
				}
			}
			else
			{
				// DL-126 P1 (review round 2).  A null-BSDF, non-delta
				// vertex (`biospec_skin_material` / `generic_human_tissue_
				// material`, whose `GetBSDF()` is null) has no aggregate
				// BSDF to form a companion/hero RATIO from, and neither
				// SPF overrides `EvaluateKrayNM` to hand one over directly
				// -- their whole response is a layered Monte Carlo
				// simulation with no analytic per-wavelength closed form
				// available after the fact.  Falling through to the
				// "Delta, BSSRDF, medium, endpoints: scatter ratio = 1.0"
				// convention below (the ORIGINAL code did exactly that, by
				// simply not entering this `if` at all) is wrong here for
				// the same reason it is right for an actual delta lobe: a
				// mirror reflects every wavelength identically, so ratio=1
				// is exact; this material's colour is PRECISELY its
				// wavelength-dependent absorption/re-emission response, so
				// ratio=1 broadcasts the HERO's realized outcome onto
				// every companion and reads grey (measured: BDPT hwss=TRUE
				// achromatic mean +34% over hwss=FALSE on
				// `tests/BDPTStrategyBalanceTest.cpp` topology N, B/R
				// channel ratio 1.09 against a true ~0.24).  Zero the
				// companion's remaining contribution instead.
				//
				// DL-126/DL-200 review round 4 (P1-2) correction: an
				// earlier revision of this comment called this "mirroring
				// PT's own HWSS companion fallback ... unbiased" as though
				// BDPT/VCM/MLT had simply adopted PT's convention wholesale.
				// That overstates the resemblance.  PT's HWSS bundle
				// accumulates each wavelength into its OWN independent XYZ
				// total (`PathTracingIntegrator.cpp`'s `compScatterNM[w] =
				// 0` just zeroes that wavelength's throughput multiplier for
				// the rest of ITS OWN walk -- there is no shared "bundle
				// mean" divisor for a zeroed companion to dilute, because
				// there is no bundle mean at all, only N independent
				// per-wavelength accumulators).  BDPT/VCM/MLT's spectral
				// rasterizers instead accumulate ONE shared bundle mean
				// across hero + companions and divide by an active-
				// wavelength count (`totalActive`/`activeWavelengthCount`,
				// P1-1) -- so zeroing `cumulativeRatio` here is only
				// unbiased when PAIRED with excluding this companion from
				// that shared count, which is what
				// `HasNullBSDFContinuationVertex` + `swl.TerminateSecondary()`
				// (called by all three spectral rasterizers before this
				// function ever runs) actually do.  The value convention
				// (zero when unpriceable) is the same in spirit as PT's;
				// the ACCOUNTING around it is not, and could not be made
				// identical without giving each companion wavelength its
				// own independent subpath (PT's real architecture) instead
				// of one shared geometry bundle -- a materially larger
				// change than this row's scope.  See
				// docs/DL126_BDPT_NULL_BSDF_CONTINUATION.md SS7.1 (P1-2)
				// for the measured variance cost of keeping
				// terminate-and-renormalize instead.
				cumulativeRatio = 0;
			}
		}
		// Delta, BSSRDF, medium, endpoints: scatter ratio = 1.0
	}
}

//////////////////////////////////////////////////////////////////////
// HasDispersiveDeltaVertex — checks stored subpath for dispersive
// delta interactions.
//
// At a delta (specular) vertex, the scattering direction is determined
// by Snell's law / Fresnel reflection.  If the IOR varies between
// heroNM and companionNM, the companion cannot share the hero's
// geometric path — it should be terminated.
//
// We reconstruct a minimal RayIntersectionGeometric from the stored
// vertex geometry and call IMaterial::GetSpecularInfoNM at both
// wavelengths.  If the IORs differ beyond a small tolerance, the
// vertex is dispersive.
//////////////////////////////////////////////////////////////////////
bool BDPTIntegrator::HasDispersiveDeltaVertex(
	const std::vector<BDPTVertex>& verts,
	Scalar heroNM,
	Scalar companionNM
	)
{
	for( unsigned int i = 0; i < verts.size(); i++ )
	{
		const BDPTVertex& v = verts[i];
		if( !v.isDelta || v.type != BDPTVertex::SURFACE || !v.pMaterial ) {
			continue;
		}

		// Reconstruct a minimal RayIntersectionGeometric for
		// GetSpecularInfoNM.  The function needs:
		//   - ptIntersection (for texture / painter lookup)
		//   - vNormal, onb (surface frame)
		//   - ray direction (incoming ray — used for IOR stack side determination)
		// We use the normal as a stand-in incoming direction; the IOR
		// painter typically only needs the intersection point and UV,
		// not the actual ray direction, for its wavelength-dependent
		// lookup.
		Ray dummyRay( Point3Ops::mkPoint3( v.position, v.normal ), -v.normal );
		RayIntersectionGeometric rig( dummyRay, nullRasterizerState );
		PathVertexEval::PopulateRIGFromVertex( v, rig );

		IORStack vertexIor( 1.0 );
		BuildVertexIORStack( v, vertexIor );
		const SpecularInfo heroSI = v.pMaterial->GetSpecularInfoNM( rig, vertexIor, heroNM );
		const SpecularInfo compSI = v.pMaterial->GetSpecularInfoNM( rig, vertexIor, companionNM );

		if( heroSI.valid && compSI.valid &&
			heroSI.isSpecular && compSI.isSpecular &&
			heroSI.canRefract && compSI.canRefract )
		{
			// Compare IOR at the two wavelengths
			const Scalar iorDiff = fabs( heroSI.ior - compSI.ior );
			if( iorDiff > 1e-6 ) {
				return true;
			}
		}
	}

	return false;
}

//////////////////////////////////////////////////////////////////////
// HasNullBSDFContinuationVertex — DL-126 P1.  See the header comment.
//////////////////////////////////////////////////////////////////////
bool BDPTIntegrator::HasNullBSDFContinuationVertex(
	const std::vector<BDPTVertex>& verts
	)
{
	// Mirrors RecomputeSubpathThroughputNM's own Phase-3 gate exactly:
	// `i + 1 < verts.size() && i > 0 && v.type == SURFACE && !v.isDelta`,
	// and -- review round 4, P3-c -- its INNER gate exactly too.  A hit
	// at i == 0 (the camera/light root) or i == verts.size()-1 (the
	// path's tail, which has no "onward scatter" to price) never reaches
	// that function's ratio computation, so it must not trigger this
	// check either.
	//
	// The inner gate is `if( v.pMaterial && v.pMaterial->GetBSDF() )`
	// ... `else { cumulativeRatio = 0; }` -- the else fires, and zeroes
	// every companion, whenever `!(v.pMaterial && v.pMaterial->GetBSDF())`,
	// i.e. `!v.pMaterial || !v.pMaterial->GetBSDF()`.  An earlier revision
	// of this function required `v.pMaterial &&` before checking
	// `GetBSDF()` (`v.pMaterial && !v.pMaterial->GetBSDF()`), which is a
	// STRICT SUBSET of that condition: a `v.pMaterial == 0` SURFACE,
	// non-delta vertex (not known to occur in production today, but not
	// excluded by anything this function can see) would take
	// `RecomputeSubpathThroughputNM`'s zeroing else-branch while this
	// function reported no termination needed -- reintroducing the exact
	// darkening-without-exclusion bug this row's P1-1 fix exists to
	// prevent, just gated on a different vertex condition.  Matching the
	// two predicates exactly removes that gap rather than relying on an
	// unproven "pMaterial is never null here" argument.
	if( verts.size() < 3 ) {
		return false;
	}
	for( unsigned int i = 1; i + 1 < verts.size(); i++ )
	{
		const BDPTVertex& v = verts[i];
		if( v.type == BDPTVertex::SURFACE && !v.isDelta &&
			!( v.pMaterial && v.pMaterial->GetBSDF() ) )
		{
			return true;
		}
	}
	return false;
}
