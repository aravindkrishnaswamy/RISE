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
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/ISPF.h"
#include "../Interfaces/IEmitter.h"
#include "../Interfaces/IObject.h"
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
#include "../Utilities/MediumTracking.h"
#include "../Utilities/IORStackSeeding.h"
#include "../Interfaces/IMedium.h"
#include "../Interfaces/IPhaseFunction.h"
#include "../Utilities/IndependentSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

//
// Small epsilon for ray offsets to avoid self-intersection
//
static const Scalar BDPT_RAY_EPSILON = 1e-6;

namespace
{
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
			if( !std::isfinite( clamped[i] ) || clamped[i] < 0 ) {
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
// Helper: evaluate BSDF at a vertex for given in/out directions.
//
// Adapts the RISE BSDF convention where value(vLightIn, ri) expects
// the incoming viewer ray stored in ri.ray.Dir() (toward-surface)
// and vLightIn pointing away from surface toward the light.
//
// Callers pass wi and wo both as "away from surface" directions,
// so we negate wo to construct the ri.ray toward-surface direction.
//////////////////////////////////////////////////////////////////////

RISEPel BDPTIntegrator::EvalBSDFAtVertex(
	const BDPTVertex& vertex,
	const Vector3& wi,
	const Vector3& wo
	) const
{
	return PathVertexEval::EvalBSDFAtVertex( vertex, wi, wo );
}

//////////////////////////////////////////////////////////////////////
// Helper: evaluate SPF PDF at a vertex.
//
// SPF::Pdf(ri, wo) expects ri.ray.Dir() as the incoming direction
// (toward surface) and wo as the outgoing direction (away from
// surface).  Since callers pass wi as the "incoming" side and wo as
// the "outgoing" side (both away-from-surface), we negate wi to
// build ri.ray.Dir().
//////////////////////////////////////////////////////////////////////

Scalar BDPTIntegrator::EvalPdfAtVertex(
	const BDPTVertex& vertex,
	const Vector3& wi,
	const Vector3& wo
	) const
{
	return PathVertexEval::EvalPdfAtVertex( vertex, wi, wo );
}

//////////////////////////////////////////////////////////////////////
// Helper: visibility test between two points
//////////////////////////////////////////////////////////////////////

bool BDPTIntegrator::IsVisible(
	const IRayCaster& caster,
	const Point3& p1,
	const Point3& p2
	) const
{
	Vector3 d = Vector3Ops::mkVector3( p2, p1 );
	const Scalar dist = Vector3Ops::Magnitude( d );

	if( dist < BDPT_RAY_EPSILON ) {
		return true;
	}

	d = d * (1.0 / dist);

	Ray shadowRay( p1, d );
	shadowRay.Advance( BDPT_RAY_EPSILON );

	// CastShadowRay returns TRUE if something is hit (i.e. NOT visible)
	return !caster.CastShadowRay( shadowRay, dist - 2.0 * BDPT_RAY_EPSILON );
}

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
	const IMedium* pGlobalMedium = scene.GetGlobalMedium();

	Vector3 d = Vector3Ops::mkVector3( p2, p1 );
	const Scalar maxDist = Vector3Ops::Magnitude( d );
	if( maxDist < BDPT_RAY_EPSILON ) {
		return RISEPel( 1, 1, 1 );
	}

	d = d * (1.0 / maxDist);
	const Ray connectionRay( p1, d );

	const IObjectManager* pObjects = scene.GetObjects();
	if( !pGlobalMedium && !pObjects && !pStartMedium ) {
		return RISEPel( 1, 1, 1 );
	}

	// Fast path: no per-object media, just global medium
	if( !pObjects && !pStartMedium ) {
		if( pGlobalMedium ) {
			return pGlobalMedium->EvalTransmittance( connectionRay, maxDist );
		}
		return RISEPel( 1, 1, 1 );
	}

	static const Scalar WALK_EPSILON = 1e-5;
	static const int MAX_WALK_STEPS = 16;

	RISEPel Tr( 1, 1, 1 );
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
					Tr = Tr * pActive->EvalTransmittance( segRay, remaining );
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
				Tr = Tr * pActive->EvalTransmittance( segRay, segLen );
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
			const Scalar ndotd = Vector3Ops::Dot( ri.geometric.vGeomNormal, d );
			if( ndotd < 0 ) {
				stack.push( pHitObj, pObjMedium );
			} else {
				stack.remove( pHitObj );
			}
		}

		segStart = boundaryDist;

		if( ColorMath::MaxValue( Tr ) < 1e-6 ) {
			return RISEPel( 0, 0, 0 );
		}
	}

	// Handle remaining distance
	if( segStart < maxDist ) {
		const Scalar remaining = maxDist - segStart;
		if( remaining > 0 ) {
			const IMedium* pActive = stack.top();
			if( pActive ) {
				const Ray segRay( connectionRay.PointAtLength( segStart ), d );
				Tr = Tr * pActive->EvalTransmittance( segRay, remaining );
				objectCoveredDist += remaining;
			}
		}
	}

	// Apply global medium for segments where no per-object medium was active
	if( pGlobalMedium ) {
		const Scalar globalDist = maxDist - objectCoveredDist;
		if( globalDist > WALK_EPSILON ) {
			Tr = Tr * pGlobalMedium->EvalTransmittance( connectionRay, globalDist );
		}
	}

	return Tr;
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
	const IMedium* pGlobalMedium = scene.GetGlobalMedium();

	Vector3 d = Vector3Ops::mkVector3( p2, p1 );
	const Scalar maxDist = Vector3Ops::Magnitude( d );
	if( maxDist < BDPT_RAY_EPSILON ) {
		return 1.0;
	}

	d = d * (1.0 / maxDist);
	const Ray connectionRay( p1, d );

	const IObjectManager* pObjects = scene.GetObjects();
	if( !pGlobalMedium && !pObjects && !pStartMedium ) {
		return 1.0;
	}

	if( !pObjects && !pStartMedium ) {
		if( pGlobalMedium ) {
			return pGlobalMedium->EvalTransmittanceNM( connectionRay, maxDist, nm );
		}
		return 1.0;
	}

	static const Scalar WALK_EPSILON = 1e-5;
	static const int MAX_WALK_STEPS = 16;

	Scalar Tr = 1.0;
	ConnectionMediumStack stack;

	// Pre-seed the medium stack if p1 is inside a per-object medium
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
			const Scalar remaining = maxDist - segStart;
			if( remaining > 0 ) {
				const IMedium* pActive = stack.top();
				if( pActive ) {
					const Ray segRay( connectionRay.PointAtLength( segStart ), d );
					Tr *= pActive->EvalTransmittanceNM( segRay, remaining, nm );
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

		const Scalar segLen = boundaryDist - segStart;
		if( segLen > 0 ) {
			const IMedium* pActive = stack.top();
			if( pActive ) {
				const Ray segRay( connectionRay.PointAtLength( segStart ), d );
				Tr *= pActive->EvalTransmittanceNM( segRay, segLen, nm );
				objectCoveredDist += segLen;
			}
		}

		const IMedium* pObjMedium = pHitObj->GetInteriorMedium();
		if( pObjMedium ) {
			// Medium boundary push/pop: GEOMETRIC normal — entering vs
			// exiting a closed solid is a topology question (PBRT 4e
			// §11.3.4).  Using shading on bumpy dielectrics mis-orders
			// the medium stack on connection rays.
			const Scalar ndotd = Vector3Ops::Dot( ri.geometric.vGeomNormal, d );
			if( ndotd < 0 ) {
				stack.push( pHitObj, pObjMedium );
			} else {
				stack.remove( pHitObj );
			}
		}

		segStart = boundaryDist;

		if( Tr < 1e-6 ) {
			return 0;
		}
	}

	if( segStart < maxDist ) {
		const Scalar remaining = maxDist - segStart;
		if( remaining > 0 ) {
			const IMedium* pActive = stack.top();
			if( pActive ) {
				const Ray segRay( connectionRay.PointAtLength( segStart ), d );
				Tr *= pActive->EvalTransmittanceNM( segRay, remaining, nm );
				objectCoveredDist += remaining;
			}
		}
	}

	if( pGlobalMedium ) {
		const Scalar globalDist = maxDist - objectCoveredDist;
		if( globalDist > WALK_EPSILON ) {
			Tr *= pGlobalMedium->EvalTransmittanceNM( connectionRay, globalDist, nm );
		}
	}

	return Tr;
}

// SampleBSSRDFEntryPoint has been extracted to BSSRDFSampling.h.
// See src/Library/Utilities/BSSRDFSampling.h for the implementation.


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

	//
	// Vertex 0: the light source itself
	//
	{
		BDPTVertex v;
		v.type = BDPTVertex::LIGHT;
		v.position = ls.position;
		v.normal = ls.normal;
		// Light samples come from `LightSampler::UniformRandomPoint` on the
		// luminary mesh — that normal is the geometric face normal (no
		// Phong / bump perturbation applies to luminaire surfaces here),
		// so shading == geometric on light vertex 0.
		v.geomNormal = ls.normal;
		v.onb.CreateFromW( ls.normal );
		v.pMaterial = 0;
		v.pObject = 0;
		v.pLight = ls.pLight;
		v.pLuminary = ls.pLuminary;
		v.isDelta = ls.isDelta;
		v.isConnectible = !ls.isDelta;

		// pdfFwd is the probability of generating this light vertex
		// = pdfSelect * pdfPosition
		v.pdfFwd = ls.pdfSelect * ls.pdfPosition;

		// Throughput: Le / (pdfSelect * pdfPosition)
		// but we also need to account for pdfDirection when we trace
		if( v.pdfFwd > 0 ) {
			v.throughput = ls.Le * (1.0 / v.pdfFwd);
		} else {
			v.throughput = RISEPel( 0, 0, 0 );
		}

		v.pdfRev = 0;
		vertices.push_back( v );
	}

	// Check if Le is zero -- no point tracing further
	if( ColorMath::MaxValue( ls.Le ) <= 0 || ls.pdfDirection <= 0 ) {
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
	RISEPel beta = ls.Le * cosAtLight;
	const Scalar pdfDirArea = ls.pdfDirection;   // already in solid angle for now
	const Scalar pdfEmit = ls.pdfSelect * ls.pdfPosition * pdfDirArea;

	// VCM post-pass inputs on the light endpoint: combined
	// emission pdf (= directPdfA * solidAnglePdfW) and the
	// generator-side cosine at the light surface.  See
	// BDPTVertex.h for the semantics expected by VCMIntegrator.
	vertices[0].emissionPdfW = pdfEmit;
	vertices[0].cosAtGen = cosAtLight;

	if( pdfEmit > 0 ) {
		beta = beta * (1.0 / pdfEmit);
	} else {
		subpathStarts.push_back( static_cast<uint32_t>( vertices.size() ) );
		return static_cast<unsigned int>( vertices.size() );
	}

	Scalar pdfFwdPrev = pdfDirArea;	// solid angle PDF of emission direction

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
	const unsigned int maxLightTotalDepth =
		( maxLightDepth >= 1024u ||
		  stabilityConfig.maxVolumeBounce > 1024u - maxLightDepth ) ?
			1024u :
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

			if( pMed )
			{
				const Scalar maxDist = ri.geometric.bHit ? ri.geometric.range : Scalar(1e10);
				bool scattered = false;
				const Scalar t_m = pMed->SampleDistance(
					currentRay, maxDist, sampler, scattered );

				if( scattered && volumeBounces < stabilityConfig.maxVolumeBounce )
				{
					const Point3 scatterPt = currentRay.PointAtLength( t_m );
					const Vector3 wo = currentRay.Dir();
					const MediumCoefficients coeff = pMed->GetCoefficients( scatterPt );
					const RISEPel Tr = pMed->EvalTransmittance( currentRay, t_m );
					const Scalar sigma_t_max = ColorMath::MaxValue( coeff.sigma_t );

					RISEPel medWeight( 0, 0, 0 );
					if( sigma_t_max > 0 ) {
						const Scalar Tr_scalar = ColorMath::MinValue( Tr );
						if( Tr_scalar > 0 ) {
							medWeight = Tr * coeff.sigma_s *
								(1.0 / (sigma_t_max * Tr_scalar));
						}
					}

					if( ColorMath::MaxValue( medWeight ) <= 0 ) {
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
					mv.isDelta = false;

					// Medium vertices enclosed by a specular (delta) boundary
					// are not connectible: connection rays are blocked by the
					// specular surface.  Propagate from previous vertex.
					// Exception: vertices in the GLOBAL medium (pMedObj == NULL)
					// are always connectable — not enclosed by any specular
					// boundary.
					{
						bool connectible = true;
						if( pMedObj == 0 ) {
							connectible = true;
						} else if( !vertices.empty() ) {
							const BDPTVertex& prev = vertices.back();
							if( prev.type == BDPTVertex::SURFACE && prev.isDelta ) {
								connectible = false;
							} else if( prev.type == BDPTVertex::MEDIUM && !prev.isConnectible ) {
								connectible = false;
							}
						}
						mv.isConnectible = connectible;
					}
					mv.throughput = beta;

					const Scalar distSqMed = t_m * t_m;
					mv.pdfFwd = BDPTUtilities::SolidAngleToAreaMedium(
						pdfFwdPrev, mv.sigma_t_scalar, distSqMed );
					mv.pdfRev = 0;

					// VCM post-pass uses sigma_t_scalar (not cosAtGen) for
					// the area-to-solid-angle inversion at medium vertices.
					// cosAtGen is left at zero as it is unused.
					mv.cosAtGen = 0;

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
					beta = beta * RISEPel( phaseVal / phasePdf, phaseVal / phasePdf,
						phaseVal / phasePdf );

					// Russian roulette
					{
						const PathTransportUtilities::RussianRouletteResult rr =
							PathTransportUtilities::EvaluateRussianRoulette(
								depth + volumeBounces, stabilityConfig.rrMinDepth,
								stabilityConfig.rrThreshold,
								ColorMath::MaxValue( beta ),
								ColorMath::MaxValue( mv.throughput ),
								sampler.Get1D() );
						if( rr.terminate ) {
							break;
						}
						if( rr.survivalProb < 1.0 ) {
							beta = beta * (1.0 / rr.survivalProb);
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
				else if( ri.geometric.bHit )
				{
					const RISEPel Tr = pMed->EvalTransmittance(
						currentRay, ri.geometric.range );
					beta = beta * Tr;
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

		// Create a new surface vertex
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = ri.geometric.ptIntersection;
		v.normal = ri.geometric.vNormal;
		v.geomNormal = ri.geometric.vGeomNormal;
		v.onb = ri.geometric.onb;
		v.ptCoord = ri.geometric.ptCoord;
		v.ptObjIntersec = ri.geometric.ptObjIntersec;
		v.vColor = ri.geometric.vColor;
		v.bHasVertexColor = ri.geometric.bHasVertexColor;
		v.pMaterial = ri.pMaterial;
		v.pObject = ri.pObject;
		v.pLight = 0;
		v.pLuminary = 0;
		// Store enclosing medium so connection transmittance can
		// seed its boundary walk from the correct starting medium.
		v.pMediumObject = pMedObj_light;
		v.pMediumVol = pMed_light;
		if( ri.pObject ) {
			iorStack.SetCurrentObject( ri.pObject );
			v.mediumIOR = iorStack.top();
			v.insideObject = iorStack.containsCurrent();
		}

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
		v.throughput = beta;
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
		pSPF->Scatter( ri.geometric, sampler, scattered, iorStack );

		if( scattered.Count() == 0 ) {
			break;
		}

		// Stochastic single-lobe selection (no path-tree branching).
		// Consume one sampler dimension for Sobol alignment.
		const Scalar lobeSelectXi = sampler.Get1D();
		const ScatteredRay* pScat;
		Scalar selectProb = 1.0;

		{
			pScat = scattered.RandomlySelect( lobeSelectXi, false );
			if( !pScat ) {
				break;
			}
			if( scattered.Count() > 1 ) {
				Scalar totalKray = 0;
				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					totalKray += ColorMath::MaxValue( scattered[i].kray );
				}
				const Scalar selectedKray = ColorMath::MaxValue( pScat->kray );
				if( totalKray > NEARZERO && selectedKray > NEARZERO ) {
					selectProb = selectedKray / totalKray;
				}
			}
		}

		// Determine connectibility: true if any scattered lobe is non-delta
		{
			bool hasNonDelta = false;
			for( unsigned int i = 0; i < scattered.Count(); i++ ) {
				if( !scattered[i].isDelta ) { hasNonDelta = true; break; }
			}
			if( !ri.pMaterial->GetBSDF() ) {
				hasNonDelta = false;
			}
			vertices.back().isConnectible = hasNonDelta;
		}

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
			const Scalar cosInGeom = Vector3Ops::Dot( ri.geometric.vGeomNormal, wo_bss );
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
						ri.geometric, ri.pObject, ri.pMaterial, sampler, 0 );

					if( bssrdf.valid )
					{
						vertices.back().isDelta = true;

						// Spatial-only weight for connections: EvalBSDFAtVertex
						// evaluates Sw independently for each connection direction,
						// so the continuation Sw must not be baked into throughput.
						const RISEPel betaSpatial = beta * bssrdf.weightSpatial * (1.0 / Ft);

						// Full weight for continuation (includes Sw for cosine dir).
						// Ft cancels with selection probability.
						beta = beta * bssrdf.weight * (1.0 / Ft);

						BDPTVertex entryV;
						entryV.type = BDPTVertex::SURFACE;
						entryV.position = bssrdf.entryPoint;
						entryV.normal = bssrdf.entryNormal;
						entryV.geomNormal = bssrdf.entryGeomNormal;
						entryV.onb = bssrdf.entryONB;
						entryV.pMaterial = ri.pMaterial;
						entryV.pObject = ri.pObject;
						entryV.pMediumObject = pMedObj_light;
						entryV.pMediumVol = pMed_light;
						entryV.isDelta = false;
						entryV.isConnectible = true;
						entryV.isBSSRDFEntry = true;
						entryV.throughput = betaSpatial;
						entryV.pdfFwd = bssrdf.pdfSurface;
						entryV.pdfRev = 0;
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
		// --- Random-walk SSS (RGB light subpath) ---
		else if( ri.pMaterial && ri.pMaterial->GetRandomWalkSSSParams() )
		{
			// Front-face gate uses GEOMETRIC; Fresnel cosine uses SHADING.
			// PBRT 4e §10.1.1 (front/back is geometric); §11.4.2 (BSSRDF
			// Fresnel angular dependence is shading-frame).
			const Vector3 wo_bss = -currentRay.Dir();
			const Scalar cosInGeom = Vector3Ops::Dot( ri.geometric.vGeomNormal, wo_bss );
			// Fresnel cosine clamped via fabs+NEARZERO — see PT site for
			// rationale.  Replaces fallback-to-cosInGeom (discontinuous Ft).
			const Scalar cosInShade = Vector3Ops::Dot( ri.geometric.vNormal, wo_bss );
			const Scalar cosIn = r_max( fabs( cosInShade ), Scalar( NEARZERO ) );
			if( cosInGeom > NEARZERO )
			{
				const RandomWalkSSSParams* pRW = ri.pMaterial->GetRandomWalkSSSParams();
				const Scalar F0 = ((pRW->ior - 1.0) / (pRW->ior + 1.0)) *
					((pRW->ior - 1.0) / (pRW->ior + 1.0));
				const Scalar F = F0 + (1.0 - F0) * pow( 1.0 - cosIn, 5.0 );
				const Scalar Ft = 1.0 - F;
				const Scalar R = F;

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
						pRW->g, pRW->ior, pRW->maxBounces, rwSampler, 0, pRW->maxDepth );

					if( bssrdf.valid )
					{
						vertices.back().isDelta = true;

						// SampleExit does NOT include Ft(entry).
						// Coin flip selects with probability Ft, so the
						// physical Ft and the 1/Ft selection compensation
						// cancel: weight * Ft / Ft = weight.
						// Apply boundary filter (e.g. melanin double-pass).
						const Scalar bf = pRW->boundaryFilter;
						const RISEPel betaSpatial = beta * bssrdf.weightSpatial * bf;
						beta = beta * bssrdf.weight * bf;

						BDPTVertex entryV;
						entryV.type = BDPTVertex::SURFACE;
						entryV.position = bssrdf.entryPoint;
						entryV.normal = bssrdf.entryNormal;
						entryV.geomNormal = bssrdf.entryGeomNormal;
						entryV.onb = bssrdf.entryONB;
						entryV.pMaterial = ri.pMaterial;
						entryV.pObject = ri.pObject;
						entryV.pMediumObject = pMedObj_light;
						entryV.pMediumVol = pMed_light;

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
						entryV.throughput = betaSpatial;
						entryV.pdfFwd = 0;
						entryV.pdfRev = 0;
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

#ifdef RISE_ENABLE_OPENPGL
		// --- Path guiding (light subpath) ---
		// Query the shared guiding field at each light subpath surface vertex
		// and blend with BSDF sampling using RIS or one-sample MIS.  The
		// shared field's incident-radiance distribution approximates the
		// reciprocal scattering distribution for diffuse-dominated transport.
		// The guided PDF flows into pdfFwdPrev, which becomes pdfFwd of the
		// next vertex, so MISWeight() auto-corrects via the ratio chain.
		bool usedGuidedDirection = false;
		RISEPel guidedF( 0, 0, 0 );
		Vector3 guidedDir;
		Scalar guidedEffectivePdf = 0;
		Scalar bsdfCombinedPdf = 0;

		if( pLightGuidingField && pLightGuidingField->IsTrained() &&
			depth < maxLightGuidingDepth &&
			GuidingSupportsSurfaceSampling( *pScat ) &&
			vertices.back().isConnectible )
		{
			// Use a separate thread_local handle from the eye subpath's
			// guideDist to avoid cross-contamination.
			static thread_local GuidingDistributionHandle lightGuideDist;
			if( pLightGuidingField->InitDistribution( lightGuideDist, v.position, sampler.Get1D() ) )
			{
				if( pScat->type == ScatteredRay::eRayDiffuse ) {
					pLightGuidingField->ApplyCosineProduct(
						lightGuideDist,
						GuidingCosineNormal( v.normal, currentRay.Dir() ) );
				}

				const Scalar alpha = guidingAlpha;

				if( guidingSamplingType == eGuidingRIS )
				{
					// RIS-based guiding (BDPT light subpath)
					PathTransportUtilities::GuidingRISCandidate<RISEPel> candidates[2];

					// Candidate 0: BSDF sample
					{
						PathTransportUtilities::GuidingRISCandidate<RISEPel>& c = candidates[0];
						c.direction = pScat->ray.Dir();
						c.bsdfEval = EvalBSDFAtVertex(
							vertices.back(), -currentRay.Dir(), c.direction );
						c.bsdfPdf = pScat->pdf;
						c.guidePdf = pLightGuidingField->Pdf( lightGuideDist, c.direction );
						c.incomingRadPdf = pLightGuidingField->IncomingRadiancePdf( lightGuideDist, c.direction );
						c.cosTheta = fabs( Vector3Ops::Dot( c.direction, v.normal ) );
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
						Scalar gPdf = 0;
						const Point2 xi2d( sampler.Get1D(), sampler.Get1D() );
						c.direction = pLightGuidingField->Sample( lightGuideDist, xi2d, gPdf );
						c.guidePdf = gPdf;

						if( gPdf > NEARZERO )
						{
							c.bsdfEval = EvalBSDFAtVertex(
								vertices.back(), -currentRay.Dir(), c.direction );
							c.bsdfPdf = EvalPdfAtVertex(
								vertices.back(), -currentRay.Dir(), c.direction );
							c.incomingRadPdf = pLightGuidingField->IncomingRadiancePdf( lightGuideDist, c.direction );
							c.cosTheta = fabs( Vector3Ops::Dot( c.direction, v.normal ) );
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
					const unsigned int sel = PathTransportUtilities::GuidingRISSelectCandidate(
						candidates, 2, sampler.Get1D(), risEffectivePdf );

					if( risEffectivePdf > NEARZERO && candidates[sel].valid )
					{
						usedGuidedDirection = true;
						guidedDir = candidates[sel].direction;
						guidedF = candidates[sel].bsdfEval;
						guidedEffectivePdf = risEffectivePdf;
					}
				}
				else
				{
					// One-sample MIS (BDPT light subpath)
					const Scalar xi = sampler.Get1D();

					if( PathTransportUtilities::ShouldUseGuidedSample( alpha, xi ) )
					{
						Scalar guidePdf = 0;
						const Point2 xi2d( sampler.Get1D(), sampler.Get1D() );
						const Vector3 gDir = pLightGuidingField->Sample( lightGuideDist, xi2d, guidePdf );

						if( guidePdf > NEARZERO )
						{
							guidedF = EvalBSDFAtVertex(
								vertices.back(), -currentRay.Dir(), gDir );
							const Scalar bsdfPdf = EvalPdfAtVertex(
								vertices.back(), -currentRay.Dir(), gDir );

							const Scalar combinedPdf =
								PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdf, bsdfPdf );

							if( combinedPdf > NEARZERO &&
								ColorMath::MaxValue( guidedF ) > NEARZERO )
							{
								usedGuidedDirection = true;
								guidedDir = gDir;
								guidedEffectivePdf = combinedPdf;
							}
						}
					}

					if( !usedGuidedDirection )
					{
						const Scalar guidePdfForBsdfDir =
							pLightGuidingField->Pdf( lightGuideDist, pScat->ray.Dir() );
						bsdfCombinedPdf =
							PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdfForBsdfDir, pScat->pdf );
					}
				}
			}
		}
#endif
		// --- End light subpath path guiding ---

		// Compute effective scatter direction and PDF.
		Vector3 scatDir = pScat->ray.Dir();
		Scalar effectivePdf = pScat->pdf;

#ifdef RISE_ENABLE_OPENPGL
		if( usedGuidedDirection ) {
			scatDir = guidedDir;
			effectivePdf = guidedEffectivePdf;
		} else if( bsdfCombinedPdf > NEARZERO ) {
			effectivePdf = bsdfCombinedPdf;
		}
#endif

		if( effectivePdf <= 0 ) {
			break;
		}

		// Per-type bounce limits
		if( PathTransportUtilities::ExceedsBounceLimitForType(
				pScat->type, diffuseBounces, glossyBounces,
				transmissionBounces, translucentBounces, stabilityConfig ) ) {
			break;
		}

		// Compute throughput update: beta *= f * |cos| / pdf
		// localScatteringWeight captures f * |cos| / pdf for guiding training.
		RISEPel localScatteringWeight( 0, 0, 0 );
		if( pScat->isDelta ) {
			// For delta scattering, kray already incorporates the right factor
			// but must be divided by the lobe selection probability.
			beta = beta * pScat->kray * (bssrdfReflectCompensation / selectProb);
		} else {
			RISEPel f;
#ifdef RISE_ENABLE_OPENPGL
			// Reuse the BSDF evaluation from guiding RIS candidate selection
			// when a guided direction was chosen (avoids redundant eval).
			f = usedGuidedDirection ? guidedF :
				EvalBSDFAtVertex( vertices.back(), -currentRay.Dir(), scatDir );
#else
			f = EvalBSDFAtVertex( vertices.back(), -currentRay.Dir(), scatDir );
#endif
			const Scalar cosTheta = fabs( Vector3Ops::Dot(
				scatDir, ri.geometric.vNormal ) );

			if( ColorMath::MaxValue( f ) <= 0 ) {
				break;
			}
			const Scalar scatterPdf = selectProb * effectivePdf;
			localScatteringWeight =
				f * (bssrdfReflectCompensation * cosTheta / scatterPdf);
			beta = beta * localScatteringWeight;
		}

		// Russian Roulette — depth threshold and throughput floor
		// are configurable via StabilityConfig.
		const PathTransportUtilities::RussianRouletteResult rr =
			PathTransportUtilities::EvaluateRussianRoulette(
				depth, stabilityConfig.rrMinDepth, stabilityConfig.rrThreshold,
				ColorMath::MaxValue( beta ),
				ColorMath::MaxValue( vertices.back().throughput ),
				sampler.Get1D() );
		if( rr.terminate ) {
			break;
		}
		if( rr.survivalProb < 1.0 ) {
			beta = beta * (1.0 / rr.survivalProb);
		}

#ifdef RISE_ENABLE_OPENPGL
		// Light subpath guiding: record scatter direction and scattering
		// weight for training.  Mirrors eye subpath metadata at ~line 2293.
		if( maxLightGuidingDepth > 0 )
		{
			vertices.back().guidingHasDirectionIn = true;
			vertices.back().guidingDirectionIn = scatDir;
			vertices.back().guidingPdfDirectionIn = selectProb * effectivePdf;
			vertices.back().guidingScatteringWeight = localScatteringWeight;
			vertices.back().guidingRussianRouletteSurvivalProbability = rr.survivalProb;
			vertices.back().guidingEta =
				(pScat->ior_stack && pScat->ior_stack->top() > NEARZERO) ?
					pScat->ior_stack->top() :
					(vertices.back().mediumIOR > NEARZERO ? vertices.back().mediumIOR : 1.0);
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
				const Scalar revPdf = EvalPdfAtVertex(
					vertices.back(), scatDir, -currentRay.Dir() );
				vertices.back().guidingReversePdfDirectionIn = revPdf;

				if( revPdf > NEARZERO )
				{
					// Reciprocal BSDF: f(wi→wo) = f(wo→wi).  Reuse the
					// forward BSDF eval; only cos and PDF change.
					const RISEPel f = usedGuidedDirection ? guidedF :
						EvalBSDFAtVertex( vertices.back(), -currentRay.Dir(), scatDir );
					const Scalar cosIncoming = fabs( Vector3Ops::Dot(
						-currentRay.Dir(), ri.geometric.vNormal ) );
					vertices.back().guidingReverseScatteringWeight =
						f * (bssrdfReflectCompensation * cosIncoming / revPdf);
				}
			}
		}
#endif

		// Store the forward pdf for the next vertex (solid angle measure),
		// accounting for lobe selection probability.
		pdfFwdPrev = selectProb * effectivePdf;

		// In Veach's formulation, delta vertices should be "transparent" in the MIS walk
		if( pScat->isDelta ) {
			pdfFwdPrev = 0;
		}

		// Update the previous vertex's pdfRev
		// pdfRev of vertex[n-1] = pdf of sampling the reverse direction at vertex[n]
		if( vertices.size() >= 2 ) {
			const BDPTVertex& curr = vertices.back();
			BDPTVertex& prev = vertices[ vertices.size() - 2 ];

			// Reverse PDF: EvalPdfAtVertex returns 0 for delta interactions
			// (SPF::Pdf() returns 0 for Dirac distributions).  This is correct;
			// remap0 in MISWeight maps the zero to 1 so the ratio chain
			// propagates through delta vertices without dying.
			const Scalar revPdfSA = EvalPdfAtVertex(
				curr,
				scatDir,
				-currentRay.Dir()
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
	#ifdef RISE_ENABLE_OPENPGL
		if( !usedGuidedDirection && pScat->ior_stack ) {
	#else
		if( pScat->ior_stack ) {
	#endif
			iorStack = *pScat->ior_stack;
		}
	}

	// Record subpath boundary (single contiguous range now that
	// path-tree branching has been excised).
	subpathStarts.push_back( static_cast<uint32_t>( vertices.size() ) );

	return static_cast<unsigned int>( vertices.size() );
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

unsigned int BDPTIntegrator::GenerateEyeSubpath(
	const RuntimeContext& rc,
	const Ray& cameraRay,
	const Point2& screenPos,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	std::vector<BDPTVertex>& vertices,
	std::vector<uint32_t>& subpathStarts
	) const
{
	(void)rc;

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

		if( pCamera ) {
			v.position = pCamera->GetLocation();
		}

		v.normal = cameraRay.Dir();
		v.onb.CreateFromW( cameraRay.Dir() );
		v.pMaterial = 0;
		v.pObject = 0;
		v.pLight = 0;
		v.pLuminary = 0;
		v.screenPos = screenPos;
		v.isDelta = false;

		// pdfFwd = 1 for a specific pixel
		v.pdfFwd = 1.0;
		v.pdfRev = 0;
		v.throughput = RISEPel( 1, 1, 1 );

		vertices.push_back( v );
	}

	// Trace the camera ray
	Ray currentRay = cameraRay;
	RISEPel beta( 1, 1, 1 );

	// The camera pdf for generating this direction.  Reuses the
	// pCamera cached at the top of GenerateEyeSubpath.
	Scalar pdfCamDir = 1.0;
	if( pCamera ) {
		pdfCamDir = BDPTCameraUtilities::PdfDirection( *pCamera, cameraRay );
	}

	// VCM post-pass inputs on the camera endpoint: the
	// directional importance PDF in solid-angle measure (used by
	// InitCamera), plus a cosine sentinel of 1.0.  BDPT itself
	// does not read these.
	vertices[0].emissionPdfW = pdfCamDir;
	vertices[0].cosAtGen = 1.0;

	Scalar pdfFwdPrev = pdfCamDir;
	IORStack iorStack( 1.0 );
	// Seed from the camera position: if the camera sits inside a
	// dielectric (submerged camera, camera inside a medium volume),
	// the first scatter needs the stack to reflect that containment.
	// For cameras in free space this is a no-op — the probe finds no
	// enclosing objects and leaves the stack as-is.
	if( pCamera ) {
		IORStackSeeding::SeedFromPoint( iorStack, pCamera->GetLocation(), scene );
	}

#ifdef RISE_ENABLE_OPENPGL
	static thread_local GuidingDistributionHandle guideDist;
#endif

	// Per-type bounce counters for StabilityConfig limits
	unsigned int eyeDiffuseBounces = 0;
	unsigned int eyeGlossyBounces = 0;
	unsigned int eyeTransmissionBounces = 0;
	unsigned int eyeTranslucentBounces = 0;
	unsigned int eyeVolumeBounces = 0;

	// Loop limit accounts for both surface and volume bounces.
	// Saturating addition: if a scene sets max_volume_bounce to
	// UINT_MAX (documented as "unlimited" for other bounce types), the
	// raw sum wraps to a small number, silently aborting the walk.
	// Cap at 1024 which is well above any realistic depth.  Also guard
	// `maxEyeDepth >= 1024` directly so the subtraction in the first
	// half of the ternary doesn't underflow.
	const unsigned int maxEyeTotalDepth =
		( maxEyeDepth >= 1024u ||
		  stabilityConfig.maxVolumeBounce > 1024u - maxEyeDepth ) ?
			1024u :
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

			if( pMed )
			{
				const Scalar maxDist = ri.geometric.bHit ? ri.geometric.range : Scalar(1e10);
				bool scattered = false;
				const Scalar t_m = pMed->SampleDistance(
					currentRay, maxDist, sampler, scattered );

				if( scattered && eyeVolumeBounces < stabilityConfig.maxVolumeBounce )
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
					const MediumCoefficients coeff = pMed->GetCoefficients( scatterPt );
					const RISEPel Tr = pMed->EvalTransmittance( currentRay, t_m );
					const Scalar sigma_t_max = ColorMath::MaxValue( coeff.sigma_t );

					RISEPel medWeight( 0, 0, 0 );
					if( sigma_t_max > 0 ) {
						const Scalar Tr_scalar = ColorMath::MinValue( Tr );
						if( Tr_scalar > 0 ) {
							medWeight = Tr * coeff.sigma_s *
								(1.0 / (sigma_t_max * Tr_scalar));
						}
					}

					if( ColorMath::MaxValue( medWeight ) <= 0 ) {
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
					mv.sigma_t_scalar = sigma_t_max;
					mv.isDelta = false;

					// Medium vertices enclosed by a specular (delta) boundary
					// are not connectible: connection rays are blocked by the
					// specular surface.  Propagate from previous vertex.
					// Exception: vertices in the GLOBAL medium (pMedObj == NULL)
					// are always connectable — they are not enclosed by any
					// specular boundary, even if the previous vertex was a
					// specular surface (e.g., reflected off glass into open fog).
					{
						bool connectible = true;
						if( pMedObj == 0 ) {
							// Global medium: always connectable
							connectible = true;
						} else if( !vertices.empty() ) {
							const BDPTVertex& prev = vertices.back();
							if( prev.type == BDPTVertex::SURFACE && prev.isDelta ) {
								connectible = false;
							} else if( prev.type == BDPTVertex::MEDIUM && !prev.isConnectible ) {
								connectible = false;
							}
						}
						mv.isConnectible = connectible;
					}
					mv.throughput = beta;

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
					mv.guidingHasSegment = true;
					mv.guidingDirectionOut = -wo;
					mv.guidingNormal = -wo;
					mv.guidingEta = 1.0;
#endif

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
					beta = beta * RISEPel( phaseVal / phasePdf, phaseVal / phasePdf,
						phaseVal / phasePdf );

					// Russian roulette for volume scattering
					const PathTransportUtilities::RussianRouletteResult rr =
						PathTransportUtilities::EvaluateRussianRoulette(
							depth + eyeVolumeBounces,
							stabilityConfig.rrMinDepth,
							stabilityConfig.rrThreshold,
							ColorMath::MaxValue( beta ),
							ColorMath::MaxValue( mv.throughput ),
							sampler.Get1D() );
					if( rr.terminate ) {
						break;
					}
					if( rr.survivalProb < 1.0 ) {
						beta = beta * (1.0 / rr.survivalProb);
					}

#ifdef RISE_ENABLE_OPENPGL
					vertices.back().guidingHasDirectionIn = true;
					vertices.back().guidingDirectionIn = wi;
					vertices.back().guidingPdfDirectionIn = phasePdf;
					vertices.back().guidingScatteringWeight =
						RISEPel( phaseVal / phasePdf, phaseVal / phasePdf,
							phaseVal / phasePdf );
					vertices.back().guidingRussianRouletteSurvivalProbability = rr.survivalProb;
					vertices.back().guidingRoughness = 1.0;
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
				else if( ri.geometric.bHit )
				{
					// No scatter event: apply transmittance to throughput.
					// The ray passes through the medium to the surface.
					const RISEPel Tr = pMed->EvalTransmittance(
						currentRay, ri.geometric.range );
					beta = beta * Tr;
				}
			}
		}

		if( !ri.geometric.bHit ) {
			break;
		}

		// Apply intersection modifier if present
		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		// Create a new surface vertex
		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = ri.geometric.ptIntersection;
		v.normal = ri.geometric.vNormal;
		v.geomNormal = ri.geometric.vGeomNormal;
		v.onb = ri.geometric.onb;
		v.ptCoord = ri.geometric.ptCoord;
		v.ptObjIntersec = ri.geometric.ptObjIntersec;
		v.vColor = ri.geometric.vColor;
		v.bHasVertexColor = ri.geometric.bHasVertexColor;
		v.pMaterial = ri.pMaterial;
		v.pObject = ri.pObject;
		v.pLight = 0;
		v.pLuminary = 0;
		// Store enclosing medium so connection transmittance can
		// seed its boundary walk from the correct starting medium.
		v.pMediumObject = pMedObj_eye;
		v.pMediumVol = pMed_eye;
		if( ri.pObject ) {
			iorStack.SetCurrentObject( ri.pObject );
			v.mediumIOR = iorStack.top();
			v.insideObject = iorStack.containsCurrent();
		}

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
		v.throughput = beta;
		v.pdfRev = 0;
		v.isDelta = false;

		// VCM post-pass input: the receiving-side cosine used to
		// invert the area-measure pdfFwd back to solid angle at
		// merge/connection time.
		v.cosAtGen = absCosIn;
#ifdef RISE_ENABLE_OPENPGL
		v.guidingHasSegment = true;
		v.guidingDirectionOut = -currentRay.Dir();
		v.guidingNormal = GuidingCosineNormal( v.normal, currentRay.Dir() );
		v.guidingEta = v.mediumIOR > NEARZERO ? v.mediumIOR : 1.0;
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
		pSPF->Scatter( ri.geometric, sampler, scattered, iorStack );

		if( scattered.Count() == 0 ) {
			break;
		}

		// Stochastic single-lobe selection (no path-tree branching).
		// Consume one sampler dimension for Sobol alignment.
		const Scalar lobeSelectXi = sampler.Get1D();
		const ScatteredRay* pScat;
		Scalar selectProb = 1.0;

		{
			pScat = scattered.RandomlySelect( lobeSelectXi, false );
			if( !pScat ) {
				break;
			}
			if( scattered.Count() > 1 ) {
				Scalar totalKray = 0;
				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					totalKray += ColorMath::MaxValue( scattered[i].kray );
				}
				const Scalar selectedKray = ColorMath::MaxValue( pScat->kray );
				if( totalKray > NEARZERO && selectedKray > NEARZERO ) {
					selectProb = selectedKray / totalKray;
				}
			}
		}

		// Determine connectibility: true if any scattered lobe is non-delta
		{
			bool hasNonDelta = false;
			for( unsigned int i = 0; i < scattered.Count(); i++ ) {
				if( !scattered[i].isDelta ) { hasNonDelta = true; break; }
			}
			if( !ri.pMaterial->GetBSDF() ) {
				hasNonDelta = false;
			}
			vertices.back().isConnectible = hasNonDelta;
		}

		// Mark the current vertex as delta
		vertices.back().isDelta = pScat->isDelta;



		// --- BSSRDF sampling for materials with diffusion profiles ---
		Scalar bssrdfReflectCompensation = 1.0;
		if( ri.pMaterial && ri.pMaterial->GetDiffusionProfile() )
		{
			// Front-face gate uses GEOMETRIC; Fresnel cosine uses SHADING.
			// PBRT 4e §10.1.1 (front/back is geometric); §11.4.2 (BSSRDF
			// Fresnel angular dependence is shading-frame).
			const Vector3 wo_bss = -currentRay.Dir();
			const Scalar cosInGeom = Vector3Ops::Dot( ri.geometric.vGeomNormal, wo_bss );
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
						ri.geometric, ri.pObject, ri.pMaterial, sampler, 0 );

					if( bssrdf.valid )
					{
						vertices.back().isDelta = true;
						const RISEPel betaSpatial = beta * bssrdf.weightSpatial * (1.0 / Ft);
						beta = beta * bssrdf.weight * (1.0 / Ft);

						BDPTVertex entryV;
						entryV.type = BDPTVertex::SURFACE;
						entryV.position = bssrdf.entryPoint;
						entryV.normal = bssrdf.entryNormal;
						entryV.geomNormal = bssrdf.entryGeomNormal;
						entryV.onb = bssrdf.entryONB;
						entryV.pMaterial = ri.pMaterial;
						entryV.pObject = ri.pObject;
						entryV.pMediumObject = pMedObj_eye;
						entryV.pMediumVol = pMed_eye;
						entryV.isDelta = false;
						entryV.isConnectible = true;
						entryV.isBSSRDFEntry = true;
						entryV.throughput = betaSpatial;
						entryV.pdfFwd = bssrdf.pdfSurface;
						entryV.pdfRev = 0;
#ifdef RISE_ENABLE_OPENPGL
						entryV.guidingHasSegment = true;
						entryV.guidingDirectionOut = -bssrdf.scatteredRay.Dir();
						entryV.guidingNormal = entryV.normal;
						entryV.guidingEta = 1.0;
#endif
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
		// --- Random-walk SSS (RGB eye subpath) ---
		else if( ri.pMaterial && ri.pMaterial->GetRandomWalkSSSParams() )
		{
			// Front-face gate uses GEOMETRIC; Fresnel cosine uses SHADING.
			// PBRT 4e §10.1.1 (front/back is geometric); §11.4.2 (BSSRDF
			// Fresnel angular dependence is shading-frame).
			const Vector3 wo_bss = -currentRay.Dir();
			const Scalar cosInGeom = Vector3Ops::Dot( ri.geometric.vGeomNormal, wo_bss );
			// Fresnel cosine clamped via fabs+NEARZERO — see PT site for
			// rationale.  Replaces fallback-to-cosInGeom (discontinuous Ft).
			const Scalar cosInShade = Vector3Ops::Dot( ri.geometric.vNormal, wo_bss );
			const Scalar cosIn = r_max( fabs( cosInShade ), Scalar( NEARZERO ) );
			if( cosInGeom > NEARZERO )
			{
				const RandomWalkSSSParams* pRW = ri.pMaterial->GetRandomWalkSSSParams();
				const Scalar F0 = ((pRW->ior - 1.0) / (pRW->ior + 1.0)) *
					((pRW->ior - 1.0) / (pRW->ior + 1.0));
				const Scalar F = F0 + (1.0 - F0) * pow( 1.0 - cosIn, 5.0 );
				const Scalar Ft = 1.0 - F;
				const Scalar R = F;

				if( Ft > NEARZERO && sampler.Get1D() < Ft )
				{
					// See RGB light subpath comment for rationale.
					IndependentSampler walkSampler( rc.random );
					ISampler& rwSampler = sampler.HasFixedDimensionBudget()
						? static_cast<ISampler&>(walkSampler) : sampler;

					BSSRDFSampling::SampleResult bssrdf = RandomWalkSSS::SampleExit(
						ri.geometric, ri.pObject,
						pRW->sigma_a, pRW->sigma_s, pRW->sigma_t,
						pRW->g, pRW->ior, pRW->maxBounces, rwSampler, 0, pRW->maxDepth );

					if( bssrdf.valid )
					{
						vertices.back().isDelta = true;

						// SampleExit does NOT include Ft(entry).
						// Coin flip: weight * Ft / Ft = weight.
						// Apply boundary filter (e.g. melanin double-pass).
						const Scalar bf = pRW->boundaryFilter;
						const RISEPel betaSpatial = beta * bssrdf.weightSpatial * bf;
						beta = beta * bssrdf.weight * bf;

						BDPTVertex entryV;
						entryV.type = BDPTVertex::SURFACE;
						entryV.position = bssrdf.entryPoint;
						entryV.normal = bssrdf.entryNormal;
						entryV.geomNormal = bssrdf.entryGeomNormal;
						entryV.onb = bssrdf.entryONB;
						entryV.pMaterial = ri.pMaterial;
						entryV.pObject = ri.pObject;
						entryV.pMediumObject = pMedObj_eye;
						entryV.pMediumVol = pMed_eye;

						// See RGB light subpath block for rationale.
						entryV.isDelta = true;
						entryV.isConnectible = false;
						entryV.isBSSRDFEntry = true;
						entryV.throughput = betaSpatial;
						entryV.pdfFwd = 0;
						entryV.pdfRev = 0;
#ifdef RISE_ENABLE_OPENPGL
						entryV.guidingHasSegment = true;
						entryV.guidingDirectionOut = -bssrdf.scatteredRay.Dir();
						entryV.guidingNormal = entryV.normal;
						entryV.guidingEta = 1.0;
#endif
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

#ifdef RISE_ENABLE_OPENPGL
		// --- Path guiding (eye subpath) ---
		bool usedGuidedDirection = false;
		RISEPel guidedF( 0, 0, 0 );
		Vector3 guidedDir;
		Scalar guidedEffectivePdf = 0;
		Scalar bsdfCombinedPdf = 0;

		if( pGuidingField && pGuidingField->IsTrained() &&
			depth < maxGuidingDepth && GuidingSupportsSurfaceSampling( *pScat ) &&
			vertices.back().isConnectible )
		{
			if( pGuidingField->InitDistribution( guideDist, v.position, sampler.Get1D() ) )
			{
				if( pScat->type == ScatteredRay::eRayDiffuse ) {
					pGuidingField->ApplyCosineProduct(
						guideDist,
						GuidingCosineNormal( v.normal, currentRay.Dir() ) );
				}

				const Scalar alpha = guidingAlpha;

				if( guidingSamplingType == eGuidingRIS )
				{
					// RIS-based guiding (BDPT eye subpath)
					PathTransportUtilities::GuidingRISCandidate<RISEPel> candidates[2];

					// Candidate 0: BSDF sample
					{
						PathTransportUtilities::GuidingRISCandidate<RISEPel>& c = candidates[0];
						c.direction = pScat->ray.Dir();
						c.bsdfEval = EvalBSDFAtVertex(
							vertices.back(), c.direction, -currentRay.Dir() );
						c.bsdfPdf = pScat->pdf;
						c.guidePdf = pGuidingField->Pdf( guideDist, c.direction );
						c.incomingRadPdf = pGuidingField->IncomingRadiancePdf( guideDist, c.direction );
						c.cosTheta = fabs( Vector3Ops::Dot( c.direction, v.normal ) );
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
						Scalar gPdf = 0;
						const Point2 xi2d( sampler.Get1D(), sampler.Get1D() );
						c.direction = pGuidingField->Sample( guideDist, xi2d, gPdf );
						c.guidePdf = gPdf;

						if( gPdf > NEARZERO )
						{
							c.bsdfEval = EvalBSDFAtVertex(
								vertices.back(), c.direction, -currentRay.Dir() );
							c.bsdfPdf = EvalPdfAtVertex(
								vertices.back(), c.direction, -currentRay.Dir() );
							c.incomingRadPdf = pGuidingField->IncomingRadiancePdf( guideDist, c.direction );
							c.cosTheta = fabs( Vector3Ops::Dot( c.direction, v.normal ) );
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
					const unsigned int sel = PathTransportUtilities::GuidingRISSelectCandidate(
						candidates, 2, sampler.Get1D(), risEffectivePdf );

					if( risEffectivePdf > NEARZERO && candidates[sel].valid )
					{
						usedGuidedDirection = true;
						guidedDir = candidates[sel].direction;
						guidedF = candidates[sel].bsdfEval;
						guidedEffectivePdf = risEffectivePdf;
					}
				}
				else
				{
					// One-sample MIS (BDPT eye subpath)
					const Scalar xi = sampler.Get1D();

					if( PathTransportUtilities::ShouldUseGuidedSample( alpha, xi ) )
					{
						Scalar guidePdf = 0;
						const Point2 xi2d( sampler.Get1D(), sampler.Get1D() );
						const Vector3 gDir = pGuidingField->Sample( guideDist, xi2d, guidePdf );

						if( guidePdf > NEARZERO )
						{
							guidedF = EvalBSDFAtVertex(
								vertices.back(), gDir, -currentRay.Dir() );
							const Scalar bsdfPdf = EvalPdfAtVertex(
								vertices.back(), gDir, -currentRay.Dir() );

							const Scalar combinedPdf =
								PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdf, bsdfPdf );

							if( combinedPdf > NEARZERO &&
								ColorMath::MaxValue( guidedF ) > NEARZERO )
							{
								usedGuidedDirection = true;
								guidedDir = gDir;
								guidedEffectivePdf = combinedPdf;
							}
						}
					}

					if( !usedGuidedDirection )
					{
						const Scalar guidePdfForBsdfDir =
							pGuidingField->Pdf( guideDist, pScat->ray.Dir() );
						bsdfCombinedPdf =
							PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdfForBsdfDir, pScat->pdf );
					}
				}
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
		} else if( bsdfCombinedPdf > NEARZERO ) {
			effectivePdf = bsdfCombinedPdf;
		}
#endif

		if( effectivePdf <= 0 ) {
			break;
		}

		// Per-type bounce limits
		if( PathTransportUtilities::ExceedsBounceLimitForType(
				pScat->type, eyeDiffuseBounces, eyeGlossyBounces,
				eyeTransmissionBounces, eyeTranslucentBounces, stabilityConfig ) ) {
			break;
		}

		const Scalar scatterPdf = selectProb * effectivePdf;
		RISEPel localScatteringWeight( 0, 0, 0 );

		// Compute throughput update
		if( pScat->isDelta ) {
			localScatteringWeight =
				pScat->kray * (bssrdfReflectCompensation / selectProb);
			beta = beta * localScatteringWeight;
		} else {
			RISEPel f;
#ifdef RISE_ENABLE_OPENPGL
			f = usedGuidedDirection ? guidedF :
				EvalBSDFAtVertex( vertices.back(), scatDir, -currentRay.Dir() );
#else
			f = EvalBSDFAtVertex( vertices.back(), scatDir, -currentRay.Dir() );
#endif
			const Scalar cosTheta = fabs( Vector3Ops::Dot(
				scatDir, ri.geometric.vNormal ) );

			if( ColorMath::MaxValue( f ) <= 0 ) {
				break;
			}
			localScatteringWeight =
				f * (bssrdfReflectCompensation * cosTheta / scatterPdf);
			beta = beta * localScatteringWeight;
		}

		// Russian Roulette after a few bounces — depth threshold
		// and throughput floor are configurable via StabilityConfig.
		const PathTransportUtilities::RussianRouletteResult rr =
			PathTransportUtilities::EvaluateRussianRoulette(
				depth, stabilityConfig.rrMinDepth, stabilityConfig.rrThreshold,
				ColorMath::MaxValue( beta ),
				ColorMath::MaxValue( vertices.back().throughput ),
				sampler.Get1D() );
		if( rr.terminate ) {
			break;
		}
		if( rr.survivalProb < 1.0 ) {
			beta = beta * (1.0 / rr.survivalProb);
		}

#ifdef RISE_ENABLE_OPENPGL
		vertices.back().guidingHasDirectionIn = true;
		vertices.back().guidingDirectionIn = scatDir;
		vertices.back().guidingPdfDirectionIn = scatterPdf;
		vertices.back().guidingScatteringWeight = localScatteringWeight;
		vertices.back().guidingRussianRouletteSurvivalProbability = rr.survivalProb;
		vertices.back().guidingEta =
			(pScat->ior_stack && pScat->ior_stack->top() > NEARZERO) ?
				pScat->ior_stack->top() :
				(vertices.back().mediumIOR > NEARZERO ? vertices.back().mediumIOR : 1.0);
		vertices.back().guidingRoughness = pScat->isDelta ?
			Scalar( 0.0 ) :
			(pScat->type == ScatteredRay::eRayDiffuse ? Scalar( 1.0 ) : Scalar( 0.5 ));
#endif

		// Store the forward pdf for the next vertex,
		// accounting for lobe selection probability.
		pdfFwdPrev = scatterPdf;

		// In Veach's formulation, delta vertices should be "transparent" in the MIS walk
		if( pScat->isDelta ) {
			pdfFwdPrev = 0;
		}

		// Update previous vertex's pdfRev
		if( vertices.size() >= 2 ) {
			const BDPTVertex& curr = vertices.back();
			BDPTVertex& prev = vertices[ vertices.size() - 2 ];

			// Reverse PDF: returns 0 for delta interactions, handled by remap0 in MISWeight.
			const Scalar revPdfSA = EvalPdfAtVertex(
				curr,
				scatDir,
				-currentRay.Dir()
				);

			// Convert to area measure at prev.  The geometric cosine is
			// only meaningful for SURFACE/LIGHT predecessors — CAMERA
			// gets the sentinel 1.0 and MEDIUM bypasses cos entirely
			// (Veach §11 medium area-pdf uses sigma_t).  Reading
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
	#ifdef RISE_ENABLE_OPENPGL
		if( !usedGuidedDirection && pScat->ior_stack ) {
	#else
		if( pScat->ior_stack ) {
	#endif
			iorStack = *pScat->ior_stack;
		}
	}

	// Record subpath boundary (single contiguous range now that
	// path-tree branching has been excised).
	subpathStarts.push_back( static_cast<uint32_t>( vertices.size() ) );

	return static_cast<unsigned int>( vertices.size() );
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
// and rasterPos is computed via BDPTCameraUtilities::Rasterize().
//
// Delta vertices cannot participate in explicit connections since
// there is zero probability of the connection direction matching
// the specular direction.
//////////////////////////////////////////////////////////////////////

BDPTIntegrator::ConnectionResult BDPTIntegrator::ConnectAndEvaluate(
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<BDPTVertex>& eyeVerts,
	unsigned int s,
	unsigned int t,
	const IScene& scene,
	const IRayCaster& caster,
	const ICamera& camera
	) const
{
	ConnectionResult result;

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

		// Evaluate emitted radiance at this point
		RayIntersectionGeometric rig( Ray( eyeEnd.position, woFromEmitter ), nullRasterizerState );
		rig.bHit = true;
		rig.ptIntersection = eyeEnd.position;
		rig.vNormal = eyeEnd.normal;
		rig.vGeomNormal = eyeEnd.geomNormal;
		rig.onb = eyeEnd.onb;

		const RISEPel Le = pEmitter->emittedRadiance( rig, woFromEmitter, eyeEnd.geomNormal );

		if( ColorMath::MaxValue( Le ) <= 0 ) {
			return result;
		}

		result.contribution = eyeEnd.throughput * Le;
		result.needsSplat = false;
		result.valid = true;
		result.guidingLocalContribution = Le;
		result.guidingEyeVertexIndex = t - 1;
		result.guidingUseDirectContribution = true;
		result.guidingValid = true;

		// --- Update pdfRev at emitter vertex for correct MIS ---
		// eyeEnd.pdfRev should be the PDF that the light sampling process
		// would have generated this point: pdfSelect * pdfPosition.
		const Scalar savedEyeEndPdfRev = eyeEnd.pdfRev;

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
			const Scalar area = eyeEnd.pObject->GetArea();
			const Scalar pdfPosition = (area > 0) ? (Scalar(1.0) / area) : 0;
			const_cast<BDPTVertex&>( eyeEnd ).pdfRev = pdfSelect * pdfPosition;
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
					// Cosine-weighted hemisphere emission (one-sided)
					const Scalar cosAtEmitter = Vector3Ops::Dot( eyeEnd.geomNormal, woFromEmitter );
					emPdfDir = (cosAtEmitter > 0) ? (cosAtEmitter * INV_PI) : 0;
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

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );

		const_cast<BDPTVertex&>( eyeEnd ).pdfRev = savedEyeEndPdfRev;
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
		// Light path endpoint connects directly to camera sensor
		const BDPTVertex& lightEnd = lightVerts[s - 1];

		if( !lightEnd.isConnectible ) {
			return result;
		}

		// Project the light vertex position onto the camera
		Point2 rasterPos;
		if( !BDPTCameraUtilities::Rasterize( camera, lightEnd.position, rasterPos ) ) {
			return result;
		}

		// Check visibility from camera to light vertex using standard shadow ray.
		const Point3 camPos = camera.GetLocation();
		if( !IsVisible( caster, camPos, lightEnd.position ) ) {
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
		RISEPel fLight( 1, 1, 1 );
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
				fLight = EvalBSDFAtVertex( lightEnd, wiAtLight, dirToCam );
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

		result.contribution = lightEnd.throughput * fLight * (G * We);
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

			const Scalar pdfPredSA = EvalPdfAtVertex( lightEnd, dirToCam, wiAtLightEnd );
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

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );

		const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRev;
		if( hasLightPred_t0 ) {
			const_cast<BDPTVertex&>( lightVerts[s - 2] ).pdfRev = savedLightPredPdfRev_t0;
		}

		return result;
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

		// Direction from eye vertex to light
		Vector3 dirToLight = Vector3Ops::mkVector3( lightStart.position, eyeEnd.position );
		const Scalar dist = Vector3Ops::Magnitude( dirToLight );
		if( dist < BDPT_RAY_EPSILON ) {
			return result;
		}
		dirToLight = dirToLight * (1.0 / dist);

		// Check visibility
		if( !IsVisible( caster, eyeEnd.position, lightStart.position ) ) {
			return result;
		}

		// Evaluate emitted radiance from the light toward the eye vertex
		RISEPel Le( 0, 0, 0 );
		if( lightStart.pLight ) {
			Le = lightStart.pLight->emittedRadiance( -dirToLight );
		} else if( lightStart.pLuminary && lightStart.pLuminary->GetMaterial() ) {
			const IEmitter* pEmitter = lightStart.pLuminary->GetMaterial()->GetEmitter();
			if( pEmitter ) {
				RayIntersectionGeometric rig(
					Ray( lightStart.position, -dirToLight ),
					nullRasterizerState );
				rig.bHit = true;
				rig.ptIntersection = lightStart.position;
				rig.vNormal = lightStart.normal;
				rig.vGeomNormal = lightStart.geomNormal;
				rig.onb = lightStart.onb;

				Le = pEmitter->emittedRadiance(
					rig,
					-dirToLight,
					lightStart.geomNormal );
			}
		}

		if( ColorMath::MaxValue( Le ) <= 0 ) {
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

		const RISEPel fEye = EvalBSDFAtVertex( eyeEnd, dirToLight, woAtEye );

		if( ColorMath::MaxValue( fEye ) <= 0 ) {
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

		// Connection transmittance through participating media
		const RISEPel Tr_conn_s1 = EvalConnectionTransmittance(
			eyeEnd.position, lightStart.position, scene, caster,
			eyeEnd.pMediumObject, eyeEnd.pMediumVol );

		// Contribution: eyeThroughput * fEye * G * Le / pdfLight
		// lightStart.throughput already has Le / (pdfSelect * pdfPosition)
		// but we need to re-evaluate since the direction changed
		// For s=1, we use: lightVerts[0].throughput * fEye * G
		// where lightVerts[0].throughput = Le / (pdfSelect * pdfPosition)

		// Actually for s=1, the light vertex stores throughput = Le / pdf_pos_select
		// We just need fEye * G * lightThroughput * eyeThroughput
		// But Le from light depends on the connection direction, which differs
		// from the sampled direction.  Re-evaluate:
		const Scalar pdfLight = lightStart.pdfFwd;
		if( pdfLight <= 0 ) {
			return result;
		}

		result.contribution = eyeEnd.throughput * fEye * Le * Tr_conn_s1 * (G / pdfLight);
		result.needsSplat = false;
		result.valid = true;
		result.guidingLocalContribution = fEye * Le * Tr_conn_s1 * (G / pdfLight);
		result.guidingEyeVertexIndex = t - 1;
		result.guidingValid = true;

		// --- Update pdfRev at connection vertices for correct MIS ---
		const Scalar distSq_conn = dist * dist;
		const Scalar savedLightPdfRev = lightStart.pdfRev;
		const Scalar savedEyePdfRev = eyeEnd.pdfRev;

		// lightStart.pdfRev: PDF that eye-side process would "find" the light
		// For delta-position lights (point/spot), leave at 0 (eye can't hit a point)
		if( !lightStart.isDelta ) {
			const Scalar pdfRevSA = EvalPdfAtVertex( eyeEnd, woAtEye, dirToLight );
			const Scalar absCosAtLight = fabs( Vector3Ops::Dot( lightStart.geomNormal, dirToLight ) );
			const_cast<BDPTVertex&>( lightStart ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfRevSA, absCosAtLight, distSq_conn );
		}

		// eyeEnd.pdfRev: PDF that light-side would generate eyeEnd
		// = emission directional PDF at light toward eyeEnd, converted to area at eyeEnd
		{
			Scalar emissionPdfDir = 0;
			if( lightStart.pLuminary ) {
				// Mesh luminary: cosine-weighted hemisphere emission (one-sided)
				const Scalar cosAtLight = Vector3Ops::Dot( lightStart.geomNormal, -dirToLight );
				emissionPdfDir = (cosAtLight > 0) ? (cosAtLight * INV_PI) : 0;
			} else if( lightStart.pLight ) {
				emissionPdfDir = lightStart.pLight->pdfDirection( -dirToLight );
			}
			// Medium vertices: sigma_t/dist^2 replaces |cos|/dist^2
			if( eyeIsMedium_s1 ) {
				const_cast<BDPTVertex&>( eyeEnd ).pdfRev =
					BDPTUtilities::SolidAngleToAreaMedium( emissionPdfDir, eyeEnd.sigma_t_scalar, distSq_conn );
			} else {
				const Scalar absCosAtEye = fabs( Vector3Ops::Dot( eyeEnd.geomNormal, dirToLight ) );
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
			const Scalar pdfPredSA = EvalPdfAtVertex( eyeEnd, dirToLight, dirToPred );
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

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );

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
		const BDPTVertex& lightEnd = lightVerts[s - 1];

		if( !lightEnd.isConnectible ) {
			return result;
		}

		// Only connect surface or medium vertices to camera
		if( s >= 2 && lightEnd.type != BDPTVertex::SURFACE && lightEnd.type != BDPTVertex::MEDIUM ) {
			return result;
		}

		// Project light vertex onto camera
		Point2 rasterPos;
		if( !BDPTCameraUtilities::Rasterize( camera, lightEnd.position, rasterPos ) ) {
			return result;
		}

		// This strategy is only valid for a direct camera connection.
		// Refractive blockers must be sampled as explicit specular eye
		// vertices; treating them as transparent here produces invalid
		// splats and severe caustic fireflies.
		const Point3 camPos = camera.GetLocation();
		if( !IsVisible( caster, lightEnd.position, camPos ) ) {
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
		RISEPel fLight( 1, 1, 1 );
		if( (lightEnd.type == BDPTVertex::SURFACE && lightEnd.pMaterial) || lightIsMedium_t1 ) {
			Vector3 wiAtLight;
			if( s >= 2 ) {
				wiAtLight = Vector3Ops::mkVector3( lightVerts[s - 2].position, lightEnd.position );
				wiAtLight = Vector3Ops::Normalize( wiAtLight );
			}

			if( s >= 2 ) {
				fLight = EvalBSDFAtVertex( lightEnd, wiAtLight, dirToCam );
			}
		} else if( lightEnd.type == BDPTVertex::LIGHT ) {
			// s == 1: the light source directly connects to camera
			// Contribution is Le in the direction toward camera
			if( lightEnd.pLight ) {
				fLight = lightEnd.pLight->emittedRadiance( dirToCam );
				// Re-scale: throughput already has Le/pdf, but Le was for the sampled direction
				// So we want Le(dirToCam) / Le(sampled) * throughput contribution
				// Simplify: just use throughput which is Le/pdf, direction-dependent Le re-evaluated
			} else if( lightEnd.pLuminary && lightEnd.pLuminary->GetMaterial() ) {
				const IEmitter* pEmitter = lightEnd.pLuminary->GetMaterial()->GetEmitter();
				if( pEmitter ) {
					RayIntersectionGeometric rig(
						Ray( lightEnd.position, dirToCam ), nullRasterizerState );
					rig.bHit = true;
					rig.ptIntersection = lightEnd.position;
					rig.vNormal = lightEnd.normal;
					rig.vGeomNormal = lightEnd.geomNormal;
					rig.onb = lightEnd.onb;
					fLight = pEmitter->emittedRadiance( rig, dirToCam, lightEnd.geomNormal );
				}
			}

			// For s=1, contribution = Le(dirToCam) * We * G / pdfLight
			const Scalar pdfLight = lightEnd.pdfFwd;
			if( pdfLight <= 0 ) {
				return result;
			}

			const Scalar distSq = dist * dist;
			const Scalar absCosLight = lightEnd.isDelta ?
				Scalar(1.0) : fabs( Vector3Ops::Dot( lightEnd.geomNormal, dirToCam ) );
			const Scalar G = absCosLight / distSq;

			result.contribution = fLight * (G * We / pdfLight);
			result.rasterPos = rasterPos;
			result.needsSplat = true;
			result.valid = true;

			// --- Update pdfRev at connection vertices for correct MIS ---
			const Scalar savedLightPdfRev = lightEnd.pdfRev;
			const Scalar savedEyePdfRev = eyeVerts[0].pdfRev;

			// lightEnd.pdfRev: camera's directional PDF at lightEnd
			{
				Ray camRayToLight( camPos, -dirToCam );
				const Scalar camPdfDir = BDPTCameraUtilities::PdfDirection( camera, camRayToLight );
				const_cast<BDPTVertex&>( lightEnd ).pdfRev =
					BDPTUtilities::SolidAngleToArea( camPdfDir, absCosLight, distSq );
			}

			// eyeVerts[0].pdfRev: emission directional PDF toward camera
			{
				Scalar emPdfDir = 0;
				if( lightEnd.pLuminary ) {
					// One-sided emission PDF
					const Scalar cosEmit = Vector3Ops::Dot( lightEnd.geomNormal, dirToCam );
					emPdfDir = (cosEmit > 0) ? (cosEmit * INV_PI) : 0;
				} else if( lightEnd.pLight ) {
					emPdfDir = lightEnd.pLight->pdfDirection( dirToCam );
				}
				const_cast<BDPTVertex&>( eyeVerts[0] ).pdfRev =
					BDPTUtilities::SolidAngleToArea( emPdfDir, Scalar(1.0), distSq );
			}

			result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );
			const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRev;
			const_cast<BDPTVertex&>( eyeVerts[0] ).pdfRev = savedEyePdfRev;
			return result;
		}

		if( ColorMath::MaxValue( fLight ) <= 0 ) {
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
		const RISEPel Tr_conn_t1 = EvalConnectionTransmittance(
			lightEnd.position, camPos, scene, caster,
			lightEnd.pMediumObject, lightEnd.pMediumVol );

		result.contribution = lightEnd.throughput * fLight * Tr_conn_t1 * (G * We);
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
			const Scalar pdfRevSA = EvalPdfAtVertex( lightEnd, wiAtLightMIS, dirToCam );
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

			const Scalar pdfPredSA = EvalPdfAtVertex( lightEnd, dirToCam, wiAtLightEnd );
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

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );

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
		if( !IsVisible( caster, eyeEnd.position, lightEnd.position ) ) {
			return result;
		}

		// Evaluate BSDF at the light endpoint
		// wi at lightEnd = direction from previous light vertex
		Vector3 wiAtLight = Vector3Ops::mkVector3(
			lightVerts[s - 2].position, lightEnd.position );
		wiAtLight = Vector3Ops::Normalize( wiAtLight );

		// wo at lightEnd = direction toward eye vertex (connection)
		const Vector3 woAtLight = -dConnect;

		const RISEPel fLight = EvalBSDFAtVertex( lightEnd, wiAtLight, woAtLight );

		if( ColorMath::MaxValue( fLight ) <= 0 ) {
			return result;
		}

		// Evaluate BSDF at the eye endpoint
		// wo at eyeEnd = direction toward previous eye vertex
		Vector3 woAtEye = Vector3Ops::mkVector3(
			eyeVerts[t - 2].position, eyeEnd.position );
		woAtEye = Vector3Ops::Normalize( woAtEye );

		// wi at eyeEnd = connection direction (from light side)
		const Vector3 wiAtEye = dConnect;

		const RISEPel fEye = EvalBSDFAtVertex( eyeEnd, wiAtEye, woAtEye );

		if( ColorMath::MaxValue( fEye ) <= 0 ) {
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
		const RISEPel Tr_conn = EvalConnectionTransmittance(
			eyeEnd.position, lightEnd.position, scene, caster,
			eyeEnd.pMediumObject, eyeEnd.pMediumVol );

		// Full path contribution
			result.contribution = lightEnd.throughput * fLight *
				RISEPel( G, G, G ) * Tr_conn * fEye * eyeEnd.throughput;
			result.needsSplat = false;
			result.valid = true;
			result.guidingLocalContribution =
				lightEnd.throughput * fLight * RISEPel( G, G, G ) * Tr_conn * fEye;
			result.guidingEyeVertexIndex = t - 1;
			result.guidingValid = true;

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
			const Scalar pdfRevSA = EvalPdfAtVertex( eyeEnd, woAtEye, dConnect );
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
			const Scalar pdfRevSA = EvalPdfAtVertex( lightEnd, wiAtLight, -dConnect );
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
			const Scalar pdfPredSA = EvalPdfAtVertex( lightEnd, woAtLight, wiAtLight );
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
			const Scalar pdfPredSA = EvalPdfAtVertex( eyeEnd, wiAtEye, woAtEye );
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

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );

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

//////////////////////////////////////////////////////////////////////
// EvaluateAllStrategies
//////////////////////////////////////////////////////////////////////

std::vector<BDPTIntegrator::ConnectionResult> BDPTIntegrator::EvaluateAllStrategies(
	const std::vector<BDPTVertex>& lightVerts,
	const std::vector<BDPTVertex>& eyeVerts,
	const IScene& scene,
	const IRayCaster& caster,
	const ICamera& camera,
	ISampler* pSampler
	) const
{
	const unsigned int nLight = static_cast<unsigned int>( lightVerts.size() );
	const unsigned int nEye = static_cast<unsigned int>( eyeVerts.size() );

	std::vector<ConnectionResult> results;
	results.reserve( (nLight + 1) * (nEye + 1) );

	const bool useCompletePathStrategySelection =
#ifdef RISE_ENABLE_OPENPGL
		pCompletePathGuide &&
		completePathStrategySelectionEnabled &&
		!pCompletePathGuide->IsCollectingTrainingSamples() &&
		pSampler &&
		completePathStrategySampleCount > 0;
#else
		false;
#endif

#ifdef RISE_ENABLE_OPENPGL
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

			strategySelectionPathCount.fetch_add( 1 );
			strategySelectionCandidateCount.fetch_add(
				static_cast<unsigned long long>( candidateCount ) );
			strategySelectionEvaluatedCount.fetch_add(
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

				ConnectionResult cr = ConnectAndEvaluate(
					lightVerts,
					eyeVerts,
					candidate.s,
					candidate.t,
					scene,
					caster,
					camera );

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
	else
#endif
	{
		// Iterate over all valid (s,t) combinations where s + t >= 2.
		for( unsigned int t = 1; t <= nEye; t++ )
		{
			for( unsigned int s = 0; s <= nLight; s++ )
			{
				if( s + t < 2 ) {
					continue;
				}

				ConnectionResult cr = ConnectAndEvaluate(
					lightVerts, eyeVerts, s, t, scene, caster, camera );
				cr.s = s;
				cr.t = t;

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

					if( eyeEnd.type != BDPTVertex::SURFACE ) continue;
					if( !eyeEnd.isConnectible ) continue;
					if( !eyeEnd.pMaterial ) continue;

					const IBSDF* pBSDF = eyeEnd.pMaterial->GetBSDF();
					if( !pBSDF ) continue;

					// Incoming viewer direction (from previous eye vertex)
					Vector3 wo = Vector3Ops::mkVector3(
						eyeVerts[t - 2].position, eyeEnd.position );
					wo = Vector3Ops::Normalize( wo );

					Ray evalRay( eyeEnd.position, -wo );
					RayIntersectionGeometric ri( evalRay, nullRasterizerState );
					PathVertexEval::PopulateRIGFromVertex( eyeEnd, ri );

					const bool bReceivesShadows = eyeEnd.pObject
						? eyeEnd.pObject->DoesReceiveShadows() : true;

					RISEPel amount( 0, 0, 0 );
					l->ComputeDirectLighting( ri, caster, *pBSDF,
						bReceivesShadows, amount );

					if( ColorMath::MaxValue( amount ) > 0 )
					{
						ConnectionResult cr;
						cr.contribution = eyeEnd.throughput * amount;
						cr.misWeight = 1.0;
						cr.needsSplat = false;
						cr.valid = true;
						cr.s = 1;
						cr.t = t;
						results.push_back( cr );
					}
				}
			}
		}
	}

#ifdef RISE_ENABLE_OPENPGL
	if( pCompletePathGuide && pCompletePathGuide->IsCollectingTrainingSamples() ) {
		RecordCompletePathSamples( pCompletePathGuide, lightVerts, eyeVerts, results );
	}

	if( pGuidingField && pGuidingField->IsCollectingTrainingSamples() ) {
		RecordGuidingTrainingPath( pGuidingField, &guidingTrainingStats, &guidingTrainingStatsMutex, eyeVerts, results );
	}
	if( pLightGuidingField && pLightGuidingField->IsCollectingTrainingSamples() ) {
		RecordGuidingTrainingLightPath( pLightGuidingField, lightVerts, maxLightGuidingDepth );
	}
#endif

	return results;
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
			// Vertex at position i in the light subpath
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
			if( vi.isDelta ) {
				continue;
			}
			if( i > 0 && lightVerts[i-1].isDelta &&
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

			// Compute the ratio with remap0 (see light-side walk above)
			const Scalar pdfR = (vj.pdfRev != 0) ? vj.pdfRev : Scalar(1);
			const Scalar pdfF = (vj.pdfFwd != 0) ? vj.pdfFwd : Scalar(1);
			ri *= pdfR / pdfF;

			// Skip non-connectible vertices.
			// Both vertices at the proposed connection must be non-delta.
			if( vj.isDelta ) {
				continue;
			}
			if( j > 0 && eyeVerts[j-1].isDelta ) {
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
//
// SPECTRAL (NM) VARIANTS
//
// These methods mirror the RGB versions above but work with scalar
// throughput at a single wavelength.  They use IBSDF::valueNM(),
// ISPF::ScatterNM(), ISPF::PdfNM(), and IEmitter::emittedRadianceNM()
// to evaluate the path contribution for a specific wavelength.
//
//////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////
// NM Helper: evaluate BSDF at a vertex for a single wavelength
//////////////////////////////////////////////////////////////////////

Scalar BDPTIntegrator::EvalBSDFAtVertexNM(
	const BDPTVertex& vertex,
	const Vector3& wi,
	const Vector3& wo,
	const Scalar nm
	) const
{
	return PathVertexEval::EvalBSDFAtVertexNM( vertex, wi, wo, nm );
}

//////////////////////////////////////////////////////////////////////
// NM Helper: evaluate SPF PDF at a vertex for a single wavelength
//////////////////////////////////////////////////////////////////////

Scalar BDPTIntegrator::EvalPdfAtVertexNM(
	const BDPTVertex& vertex,
	const Vector3& wi,
	const Vector3& wo,
	const Scalar nm
	) const
{
	return PathVertexEval::EvalPdfAtVertexNM( vertex, wi, wo, nm );
}

//////////////////////////////////////////////////////////////////////
// NM Helper: evaluate emitter radiance at a vertex
//////////////////////////////////////////////////////////////////////

Scalar BDPTIntegrator::EvalEmitterRadianceNM(
	const BDPTVertex& vertex,
	const Vector3& outDir,
	const Scalar nm
	) const
{
	if( !vertex.pMaterial ) {
		return 0;
	}

	const IEmitter* pEmitter = vertex.pMaterial->GetEmitter();
	if( !pEmitter ) {
		return 0;
	}

	RayIntersectionGeometric rig( Ray( vertex.position, outDir ), nullRasterizerState );
	rig.bHit = true;
	rig.ptIntersection = vertex.position;
	rig.vNormal = vertex.normal;
	rig.vGeomNormal = vertex.geomNormal;
	rig.onb = vertex.onb;

	return pEmitter->emittedRadianceNM( rig, outDir, vertex.geomNormal, nm );
}

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
	vertices.clear();
	subpathStarts.clear();
	subpathStarts.push_back( 0 );

	if( !pLightSampler ) {
		return 0;
	}

	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
	const LuminaryManager::LuminariesList& luminaries = pLumManager ?
		const_cast<LuminaryManager*>(pLumManager)->getLuminaries() : emptyList;

	IORStack iorStack( 1.0 );

	// Phase 0: light source sampling
	sampler.StartStream( 0 );

	LightSample ls;
	if( !pLightSampler->SampleLight( scene, luminaries, sampler, ls ) ) {
		return 0;
	}

	// Seed the IOR stack with the nested dielectrics containing the
	// emission point — see the RGB light subpath for the rationale.
	IORStackSeeding::SeedFromPoint( iorStack, ls.position, scene );

	vertices.reserve( maxLightDepth + 1 );

	// Convert Le from RISEPel to scalar at wavelength nm
	// For mesh luminaries, re-evaluate using emittedRadianceNM
	// For non-mesh lights, approximate from the RGB value
	Scalar LeNM = 0;
	if( ls.pLuminary && ls.pLuminary->GetMaterial() ) {
		const IEmitter* pEmitter = ls.pLuminary->GetMaterial()->GetEmitter();
		if( pEmitter ) {
			RayIntersectionGeometric rig( Ray( ls.position, ls.direction ), nullRasterizerState );
			rig.bHit = true;
			rig.ptIntersection = ls.position;
			rig.vNormal = ls.normal;
			// LightSampler returns the geometric face normal in ls.normal
			// (UniformRandomPoint on the luminary mesh; no Phong/bump on
			// emitter surfaces in RISE), so vGeomNormal mirrors it.
			rig.vGeomNormal = ls.normal;
			OrthonormalBasis3D onb;
			onb.CreateFromW( ls.normal );
			rig.onb = onb;
			LeNM = pEmitter->emittedRadianceNM( rig, ls.direction, ls.normal, nm );
		}
	} else if( ls.pLight ) {
		// Non-mesh light: approximate spectral radiance from RGB
		// Use luminance-like weighting: 0.2126*R + 0.7152*G + 0.0722*B
		LeNM = 0.2126 * ls.Le[0] + 0.7152 * ls.Le[1] + 0.0722 * ls.Le[2];
	}

	//
	// Vertex 0: the light source itself
	//
	{
		BDPTVertex v;
		v.type = BDPTVertex::LIGHT;
		v.position = ls.position;
		v.normal = ls.normal;
		// Light samples come from `LightSampler::UniformRandomPoint` on the
		// luminary mesh — that normal is the geometric face normal (no
		// Phong / bump perturbation applies to luminaire surfaces here),
		// so shading == geometric on light vertex 0.
		v.geomNormal = ls.normal;
		v.onb.CreateFromW( ls.normal );
		v.pMaterial = 0;
		v.pObject = 0;
		v.pLight = ls.pLight;
		v.pLuminary = ls.pLuminary;
		v.isDelta = ls.isDelta;
		v.isConnectible = !ls.isDelta;

		v.pdfFwd = ls.pdfSelect * ls.pdfPosition;

		if( v.pdfFwd > 0 ) {
			v.throughputNM = LeNM / v.pdfFwd;
			v.throughput = RISEPel( v.throughputNM, v.throughputNM, v.throughputNM );	// for guiding training (Le recovery)
		} else {
			v.throughputNM = 0;
		}

		v.pdfRev = 0;
		vertices.push_back( v );
	}

	if( LeNM <= 0 || ls.pdfDirection <= 0 ) {
		subpathStarts.push_back( static_cast<uint32_t>( vertices.size() ) );
		return static_cast<unsigned int>( vertices.size() );
	}

	// Trace the emission ray
	Ray currentRay( ls.position, ls.direction );
	currentRay.Advance( BDPT_RAY_EPSILON );

	const Scalar cosAtLight = fabs( Vector3Ops::Dot( ls.direction, ls.normal ) );
	Scalar betaNM = LeNM * cosAtLight;
	const Scalar pdfEmit = ls.pdfSelect * ls.pdfPosition * ls.pdfDirection;

	// VCM post-pass inputs on the light endpoint (spectral variant).
	// See BDPTVertex.h for the semantics.
	vertices[0].emissionPdfW = pdfEmit;
	vertices[0].cosAtGen = cosAtLight;

	if( pdfEmit > 0 ) {
		betaNM = betaNM / pdfEmit;
	} else {
		subpathStarts.push_back( static_cast<uint32_t>( vertices.size() ) );
		return static_cast<unsigned int>( vertices.size() );
	}

	Scalar pdfFwdPrev = ls.pdfDirection;

	// HWSS per-wavelength throughput tracking.  Initial value for each
	// active wavelength is (Le_w * cosAtLight / pdfEmit) — the same
	// formula used for hero but with Le evaluated at companion nm.
	// See PathTracingIntegrator HWSS RR comment for full rationale.
	Scalar hwssBetaNM[SampledWavelengths::N];
	if( pSwlHWSS ) {
		for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
			hwssBetaNM[w] = 0;
			if( pSwlHWSS->terminated[w] ) continue;
			if( w == 0 ) {
				hwssBetaNM[w] = betaNM;	// hero
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
					OrthonormalBasis3D onbW;
					onbW.CreateFromW( ls.normal );
					rigW.onb = onbW;
					LeW = pEm->emittedRadianceNM(
						rigW, ls.direction, ls.normal, pSwlHWSS->lambda[w] );
				}
			} else if( ls.pLight ) {
				LeW = 0.2126 * ls.Le[0] + 0.7152 * ls.Le[1] + 0.0722 * ls.Le[2];
			}
			if( pdfEmit > 0 ) {
				hwssBetaNM[w] = LeW * cosAtLight / pdfEmit;
			}
		}
	}

	// Per-type bounce counters for StabilityConfig limits
	unsigned int nmLightDiffuseBounces = 0;
	unsigned int nmLightGlossyBounces = 0;
	unsigned int nmLightTransmissionBounces = 0;
	unsigned int nmLightTranslucentBounces = 0;
	unsigned int nmLightVolumeBounces = 0;
	unsigned int nmLightSurfaceBounces = 0;

	// Saturating add; see matching comment in GenerateEyeSubpath.
	// Guard maxLightDepth >= 1024 so the subtraction doesn't underflow.
	const unsigned int nmMaxLightTotalDepth =
		( maxLightDepth >= 1024u ||
		  stabilityConfig.maxVolumeBounce > 1024u - maxLightDepth ) ?
			1024u :
			maxLightDepth + stabilityConfig.maxVolumeBounce;

	for( unsigned int depth = 0; depth < nmMaxLightTotalDepth; depth++ )
	{
		// Per-bounce stream offset on the NM light subpath.  Streams
		// [1, ...) are reserved for the light walk; eye walk uses
		// [16, ...).
		sampler.StartStream( 1u + depth );

		RayIntersection ri( currentRay, nullRasterizerState );
		scene.GetObjects()->IntersectRay( ri, true, true, false );

		// ----------------------------------------------------------------
		// Participating media: free-flight distance sampling (NM light subpath).
		// Same algorithm as the RGB variant — see GenerateLightSubpath.
		// Uses spectral SampleDistanceNM / GetCoefficientsNM so that
		// chromatic media are sampled at the correct wavelength.
		// ----------------------------------------------------------------
		const IObject* pMedObj_nmLight = 0;
		const IMedium* pMed_nmLight = 0;
		{
			const IObject* pMedObj = 0;
			const IMedium* pMed = MediumTracking::GetCurrentMediumWithObject(
				iorStack, &scene, pMedObj );
			pMedObj_nmLight = pMedObj;
			pMed_nmLight = pMed;

			if( pMed )
			{
				const Scalar maxDist = ri.geometric.bHit ? ri.geometric.range : Scalar(1e10);
				bool scattered = false;
				const Scalar t_m = pMed->SampleDistanceNM(
					currentRay, maxDist, nm, sampler, scattered );

				if( scattered && nmLightVolumeBounces < stabilityConfig.maxVolumeBounce )
				{
					const Point3 scatterPt = currentRay.PointAtLength( t_m );
					const Vector3 wo = currentRay.Dir();
					const MediumCoefficientsNM coeff = pMed->GetCoefficientsNM( scatterPt, nm );
					const Scalar TrNM = pMed->EvalTransmittanceNM( currentRay, t_m, nm );
					const Scalar sigma_t_nm = coeff.sigma_t;

					Scalar medWeightNM = 0;
					if( sigma_t_nm > 0 && TrNM > 0 ) {
						medWeightNM = coeff.sigma_s / sigma_t_nm;
					}

					if( medWeightNM <= 0 ) {
						break;
					}

					betaNM *= medWeightNM;

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
					mv.sigma_t_scalar = sigma_t_nm;
					mv.isDelta = false;

					// Medium vertices enclosed by a specular (delta) boundary
					// are not connectible: connection rays are blocked by the
					// specular surface.  Propagate from previous vertex.
					// Exception: vertices in the GLOBAL medium (pMedObj == NULL)
					// are always connectable.
					{
						bool connectible = true;
						if( pMedObj == 0 ) {
							connectible = true;
						} else if( !vertices.empty() ) {
							const BDPTVertex& prev = vertices.back();
							if( prev.type == BDPTVertex::SURFACE && prev.isDelta ) {
								connectible = false;
							} else if( prev.type == BDPTVertex::MEDIUM && !prev.isConnectible ) {
								connectible = false;
							}
						}
						mv.isConnectible = connectible;
					}
					mv.throughputNM = betaNM;

					const Scalar distSqMed = t_m * t_m;
					mv.pdfFwd = BDPTUtilities::SolidAngleToAreaMedium(
						pdfFwdPrev, mv.sigma_t_scalar, distSqMed );
					mv.pdfRev = 0;

					// VCM uses sigma_t_scalar for medium vertices.
					mv.cosAtGen = 0;

					vertices.push_back( mv );

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
					betaNM *= phaseVal / phasePdf;

					// Russian roulette
					{
						const PathTransportUtilities::RussianRouletteResult rr =
							PathTransportUtilities::EvaluateRussianRoulette(
								depth + nmLightVolumeBounces, stabilityConfig.rrMinDepth,
								stabilityConfig.rrThreshold,
								fabs( betaNM ), fabs( mv.throughputNM ),
								sampler.Get1D() );
						if( rr.terminate ) {
							break;
						}
						if( rr.survivalProb < 1.0 ) {
							betaNM /= rr.survivalProb;
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

					nmLightVolumeBounces++;
					continue;
				}
				else if( ri.geometric.bHit )
				{
					const Scalar TrNM = pMed->EvalTransmittanceNM(
						currentRay, ri.geometric.range, nm );
					betaNM *= TrNM;
				}
			}
		}

		if( !ri.geometric.bHit ) {
			break;
		}

		// Check surface depth limit (medium scatters don't count)
		if( nmLightSurfaceBounces >= maxLightDepth ) {
			break;
		}
		nmLightSurfaceBounces++;

		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = ri.geometric.ptIntersection;
		v.normal = ri.geometric.vNormal;
		v.geomNormal = ri.geometric.vGeomNormal;
		v.onb = ri.geometric.onb;
		v.ptCoord = ri.geometric.ptCoord;
		v.ptObjIntersec = ri.geometric.ptObjIntersec;
		v.pMaterial = ri.pMaterial;
		v.pObject = ri.pObject;
		v.pLight = 0;
		v.pLuminary = 0;
		v.pMediumObject = pMedObj_nmLight;
		v.pMediumVol = pMed_nmLight;
		if( ri.pObject ) {
			iorStack.SetCurrentObject( ri.pObject );
			v.mediumIOR = iorStack.top();
			v.insideObject = iorStack.containsCurrent();
		}

		const Scalar distSq = ri.geometric.range * ri.geometric.range;
		// Solid-angle <-> area Jacobian uses the GEOMETRIC normal —
		// area-element parameterisation depends on the actual face
		// orientation (Veach 1997 §8.2.2 / PBRT 4e §13.6.4).
		const Scalar absCosIn = fabs( Vector3Ops::Dot(
			ri.geometric.vGeomNormal, -currentRay.Dir() ) );

		v.pdfFwd = BDPTUtilities::SolidAngleToArea( pdfFwdPrev, absCosIn, distSq );
		v.throughputNM = betaNM;
		v.throughput = RISEPel( betaNM, betaNM, betaNM );	// for guiding training
		v.pdfRev = 0;
		v.isDelta = false;

		// VCM post-pass input: the receiving-side cosine at this
		// vertex (spectral variant).
		v.cosAtGen = absCosIn;

#ifdef RISE_ENABLE_OPENPGL
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

		// Sample the SPF at this wavelength
		ScatteredRayContainer scattered;
		pSPF->ScatterNM( ri.geometric, sampler, nm, scattered, iorStack );

		if( scattered.Count() == 0 ) {
			break;
		}

		// Stochastic single-lobe selection (no path-tree branching).
		// Consume one sampler dimension for Sobol alignment.
		const Scalar lobeSelectXi = sampler.Get1D();
		const ScatteredRay* pScat;
		Scalar selectProb = 1.0;

		{
			pScat = scattered.RandomlySelect( lobeSelectXi, true );
			if( !pScat ) {
				break;
			}
			// Compute lobe selection probability using spectral krayNM weights
			if( scattered.Count() > 1 ) {
				Scalar totalKray = 0;
				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					totalKray += fabs( scattered[i].krayNM );
				}
				const Scalar selectedKray = fabs( pScat->krayNM );
				if( totalKray > NEARZERO && selectedKray > NEARZERO ) {
					selectProb = selectedKray / totalKray;
				}
			}
		}

		// Determine connectibility: true if any scattered lobe is non-delta
		{
			bool hasNonDelta = false;
			for( unsigned int i = 0; i < scattered.Count(); i++ ) {
				if( !scattered[i].isDelta ) { hasNonDelta = true; break; }
			}
			if( !ri.pMaterial->GetBSDF() ) {
				hasNonDelta = false;
			}
			vertices.back().isConnectible = hasNonDelta;
		}

		vertices.back().isDelta = pScat->isDelta;



		// --- BSSRDF sampling (NM light subpath) ---
		Scalar bssrdfReflectCompensation = 1.0;
		if( ri.pMaterial && ri.pMaterial->GetDiffusionProfile() )
		{
			// Front-face gate uses GEOMETRIC; Fresnel cosine uses SHADING.
			// PBRT 4e §10.1.1 (front/back is geometric); §11.4.2 (BSSRDF
			// Fresnel angular dependence is shading-frame).
			const Vector3 wo_bss = -currentRay.Dir();
			const Scalar cosInGeom = Vector3Ops::Dot( ri.geometric.vGeomNormal, wo_bss );
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
					ri.geometric, ri.pObject, ri.pMaterial, sampler, nm );

				if( bssrdf.valid )
				{
					vertices.back().isDelta = true;
					const Scalar betaSpatialNM = betaNM * bssrdf.weightSpatialNM / Ft;
					betaNM = betaNM * bssrdf.weightNM / Ft;

					BDPTVertex entryV;
					entryV.type = BDPTVertex::SURFACE;
					entryV.position = bssrdf.entryPoint;
					entryV.normal = bssrdf.entryNormal;
					entryV.geomNormal = bssrdf.entryGeomNormal;
					entryV.onb = bssrdf.entryONB;
					entryV.pMaterial = ri.pMaterial;
					entryV.pObject = ri.pObject;
					entryV.pMediumObject = pMedObj_nmLight;
					entryV.pMediumVol = pMed_nmLight;
					entryV.isDelta = false;
					entryV.isConnectible = true;
					entryV.isBSSRDFEntry = true;
					entryV.throughputNM = betaSpatialNM;
					entryV.pdfFwd = bssrdf.pdfSurface;
					entryV.pdfRev = 0;
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
		// --- Random-walk SSS (NM light subpath) ---
		else if( ri.pMaterial )
		{
			const RandomWalkSSSParams* pRW = ri.pMaterial->GetRandomWalkSSSParams();
			RandomWalkSSSParams rwParamsNM;
			if( !pRW && ri.pMaterial->GetRandomWalkSSSParamsNM( nm, rwParamsNM ) ) {
				pRW = &rwParamsNM;
			}
			const Scalar cosIn = pRW ? Vector3Ops::Dot(
				ri.geometric.vNormal, -currentRay.Dir() ) : 0;
			if( pRW && cosIn > NEARZERO )
			{
				const Scalar F0 = ((pRW->ior - 1.0) / (pRW->ior + 1.0)) *
					((pRW->ior - 1.0) / (pRW->ior + 1.0));
				const Scalar F = F0 + (1.0 - F0) * pow( 1.0 - cosIn, 5.0 );
				const Scalar Ft = 1.0 - F;
				const Scalar R = F;

				if( Ft > NEARZERO && sampler.Get1D() < Ft )
				{
					// See RGB light subpath comment for rationale.
					IndependentSampler walkSampler( random );
					ISampler& rwSampler = sampler.HasFixedDimensionBudget()
						? static_cast<ISampler&>(walkSampler) : sampler;

					BSSRDFSampling::SampleResult bssrdf = RandomWalkSSS::SampleExit(
						ri.geometric, ri.pObject,
						pRW->sigma_a, pRW->sigma_s, pRW->sigma_t,
						pRW->g, pRW->ior, pRW->maxBounces, rwSampler, nm, pRW->maxDepth );

					if( bssrdf.valid )
					{
						vertices.back().isDelta = true;

						// SampleExit does NOT include Ft(entry).
						// Coin flip: weight * Ft / Ft = weight.
						// Apply boundary filter (e.g. melanin double-pass).
						const Scalar bf = pRW->boundaryFilter;
						const Scalar betaSpatialNM = betaNM * bssrdf.weightSpatialNM * bf;
						betaNM = betaNM * bssrdf.weightNM * bf;

						BDPTVertex entryV;
						entryV.type = BDPTVertex::SURFACE;
						entryV.position = bssrdf.entryPoint;
						entryV.normal = bssrdf.entryNormal;
						entryV.geomNormal = bssrdf.entryGeomNormal;
						entryV.onb = bssrdf.entryONB;
						entryV.pMaterial = ri.pMaterial;
						entryV.pObject = ri.pObject;
						entryV.pMediumObject = pMedObj_nmLight;
						entryV.pMediumVol = pMed_nmLight;

						// See RGB light subpath block for rationale.
						entryV.isDelta = true;
						entryV.isConnectible = false;
						entryV.isBSSRDFEntry = true;
						entryV.throughputNM = betaSpatialNM;
						entryV.pdfFwd = 0;
						entryV.pdfRev = 0;
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

#ifdef RISE_ENABLE_OPENPGL
		// --- Path guiding (NM light subpath) ---
		bool usedGuidedDirection = false;
		Vector3 guidedDir;
		Scalar guidedFNM = 0;
		Scalar guidedEffectivePdf = 0;
		Scalar bsdfCombinedPdf = 0;

		if( pLightGuidingField && pLightGuidingField->IsTrained() &&
			depth < maxLightGuidingDepth &&
			GuidingSupportsSurfaceSampling( *pScat ) &&
			vertices.back().isConnectible )
		{
			static thread_local GuidingDistributionHandle lightGuideDistNM;
			if( pLightGuidingField->InitDistribution( lightGuideDistNM, v.position, sampler.Get1D() ) )
			{
				if( pScat->type == ScatteredRay::eRayDiffuse ) {
					pLightGuidingField->ApplyCosineProduct(
						lightGuideDistNM,
						GuidingCosineNormal( v.normal, currentRay.Dir() ) );
				}

				const Scalar alpha = guidingAlpha;

				if( guidingSamplingType == eGuidingRIS )
				{
					PathTransportUtilities::GuidingRISCandidate<Scalar> candidates[2];

					// Candidate 0: BSDF sample
					{
						PathTransportUtilities::GuidingRISCandidate<Scalar>& c = candidates[0];
						c.direction = pScat->ray.Dir();
						c.bsdfEval = EvalBSDFAtVertexNM(
							vertices.back(), -currentRay.Dir(), c.direction, nm );
						c.bsdfPdf = pScat->pdf;
						c.guidePdf = pLightGuidingField->Pdf( lightGuideDistNM, c.direction );
						c.incomingRadPdf = pLightGuidingField->IncomingRadiancePdf( lightGuideDistNM, c.direction );
						c.cosTheta = fabs( Vector3Ops::Dot( c.direction, v.normal ) );
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
						Scalar gPdf = 0;
						const Point2 xi2d( sampler.Get1D(), sampler.Get1D() );
						c.direction = pLightGuidingField->Sample( lightGuideDistNM, xi2d, gPdf );
						c.guidePdf = gPdf;

						if( gPdf > NEARZERO )
						{
							c.bsdfEval = EvalBSDFAtVertexNM(
								vertices.back(), -currentRay.Dir(), c.direction, nm );
							c.bsdfPdf = EvalPdfAtVertexNM(
								vertices.back(), -currentRay.Dir(), c.direction, nm );
							c.incomingRadPdf = pLightGuidingField->IncomingRadiancePdf( lightGuideDistNM, c.direction );
							c.cosTheta = fabs( Vector3Ops::Dot( c.direction, v.normal ) );
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
					const unsigned int sel = PathTransportUtilities::GuidingRISSelectCandidate(
						candidates, 2, sampler.Get1D(), risEffectivePdf );

					if( risEffectivePdf > NEARZERO && candidates[sel].valid )
					{
						usedGuidedDirection = true;
						guidedDir = candidates[sel].direction;
						guidedFNM = candidates[sel].bsdfEval;
						guidedEffectivePdf = risEffectivePdf;
					}
				}
				else
				{
					// One-sample MIS (NM light subpath)
					const Scalar xi = sampler.Get1D();

					if( PathTransportUtilities::ShouldUseGuidedSample( alpha, xi ) )
					{
						Scalar guidePdf = 0;
						const Point2 xi2d( sampler.Get1D(), sampler.Get1D() );
						const Vector3 gDir = pLightGuidingField->Sample( lightGuideDistNM, xi2d, guidePdf );

						if( guidePdf > NEARZERO )
						{
							guidedFNM = EvalBSDFAtVertexNM(
								vertices.back(), -currentRay.Dir(), gDir, nm );
							const Scalar bsdfPdf = EvalPdfAtVertexNM(
								vertices.back(), -currentRay.Dir(), gDir, nm );

							const Scalar combinedPdf =
								PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdf, bsdfPdf );

							if( combinedPdf > NEARZERO && fabs( guidedFNM ) > NEARZERO )
							{
								usedGuidedDirection = true;
								guidedDir = gDir;
								guidedEffectivePdf = combinedPdf;
							}
						}
					}

					if( !usedGuidedDirection )
					{
						const Scalar guidePdfForBsdfDir =
							pLightGuidingField->Pdf( lightGuideDistNM, pScat->ray.Dir() );
						bsdfCombinedPdf =
							PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdfForBsdfDir, pScat->pdf );
					}
				}
			}
		}
#endif
		// --- End NM light subpath path guiding ---

		// Compute effective scatter direction and PDF
		Vector3 scatDir = pScat->ray.Dir();
		Scalar effectivePdf = pScat->pdf;

#ifdef RISE_ENABLE_OPENPGL
		if( usedGuidedDirection ) {
			scatDir = guidedDir;
			effectivePdf = guidedEffectivePdf;
		} else if( bsdfCombinedPdf > NEARZERO ) {
			effectivePdf = bsdfCombinedPdf;
		}
#endif

		if( effectivePdf <= 0 ) {
			break;
		}

		// Per-type bounce limits
		if( PathTransportUtilities::ExceedsBounceLimitForType(
				pScat->type, nmLightDiffuseBounces, nmLightGlossyBounces,
				nmLightTransmissionBounces, nmLightTranslucentBounces, stabilityConfig ) ) {
			break;
		}

		// Snapshot pre-scatter HWSS throughput so the RR below can
		// compare max(pre) vs max(post) over active wavelengths.
		Scalar hwssBetaNMPre[SampledWavelengths::N];
		if( pSwlHWSS ) {
			for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
				hwssBetaNMPre[w] = hwssBetaNM[w];
			}
		}

		// Throughput update using valueNM
		// localScatteringWeight captures f * |cos| / pdf for guiding training.
		RISEPel localScatteringWeight( 0, 0, 0 );
		if( pScat->isDelta ) {
			betaNM = betaNM * pScat->krayNM * bssrdfReflectCompensation / selectProb;
			if( pSwlHWSS ) {
				const Scalar deltaScale = pScat->krayNM * bssrdfReflectCompensation / selectProb;
				hwssBetaNM[0] = betaNM;
				for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
					if( pSwlHWSS->terminated[w] ) continue;
					hwssBetaNM[w] = hwssBetaNM[w] * deltaScale;
				}
			}
		} else {
			Scalar fNM;
#ifdef RISE_ENABLE_OPENPGL
			fNM = usedGuidedDirection ? guidedFNM :
				EvalBSDFAtVertexNM( vertices.back(), -currentRay.Dir(), scatDir, nm );
#else
			fNM = EvalBSDFAtVertexNM( vertices.back(), -currentRay.Dir(), scatDir, nm );
#endif
			const Scalar cosTheta = fabs( Vector3Ops::Dot(
				scatDir, ri.geometric.vNormal ) );

			if( fNM <= 0 ) {
				break;
			}
			const Scalar scatterPdf = selectProb * effectivePdf;
			localScatteringWeight = RISEPel(
				fNM * bssrdfReflectCompensation * cosTheta / scatterPdf,
				fNM * bssrdfReflectCompensation * cosTheta / scatterPdf,
				fNM * bssrdfReflectCompensation * cosTheta / scatterPdf );
			betaNM = betaNM * fNM * bssrdfReflectCompensation * cosTheta / scatterPdf;
			if( pSwlHWSS ) {
				const Scalar invScale = bssrdfReflectCompensation * cosTheta / scatterPdf;
				hwssBetaNM[0] = betaNM;
				for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
					if( pSwlHWSS->terminated[w] ) continue;
					const Scalar fw = EvalBSDFAtVertexNM(
						vertices.back(), -currentRay.Dir(), scatDir, pSwlHWSS->lambda[w] );
					hwssBetaNM[w] = hwssBetaNM[w] * fw * invScale;
				}
			}
		}

		// Russian Roulette — configurable depth and threshold.  HWSS
		// uses max over active wavelengths to avoid hero-driven firefly
		// amplification; see PathTracingIntegrator HWSS RR comment.
		Scalar rrPrevMax = fabs( vertices.back().throughputNM );
		Scalar rrCurrMax = fabs( betaNM );
		if( pSwlHWSS ) {
			for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
				if( pSwlHWSS->terminated[w] ) continue;
				const Scalar p = fabs( hwssBetaNMPre[w] );
				if( p > rrPrevMax ) rrPrevMax = p;
				const Scalar c = fabs( hwssBetaNM[w] );
				if( c > rrCurrMax ) rrCurrMax = c;
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
			betaNM *= rrScale;
			if( pSwlHWSS ) {
				for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
					hwssBetaNM[w] *= rrScale;
				}
			}
		}

#ifdef RISE_ENABLE_OPENPGL
		if( maxLightGuidingDepth > 0 )
		{
			vertices.back().guidingHasDirectionIn = true;
			vertices.back().guidingDirectionIn = scatDir;
			vertices.back().guidingPdfDirectionIn = selectProb * effectivePdf;
			vertices.back().guidingScatteringWeight = localScatteringWeight;
			vertices.back().guidingRussianRouletteSurvivalProbability = rr.survivalProb;
			vertices.back().guidingEta =
				(pScat->ior_stack && pScat->ior_stack->top() > NEARZERO) ?
					pScat->ior_stack->top() :
					(vertices.back().mediumIOR > NEARZERO ? vertices.back().mediumIOR : 1.0);
			vertices.back().guidingRoughness = pScat->isDelta ?
				Scalar( 0.0 ) :
				(pScat->type == ScatteredRay::eRayDiffuse ? Scalar( 1.0 ) : Scalar( 0.5 ));

			// Reverse PDF and scattering weight for reversed training segments.
			if( !pScat->isDelta )
			{
				const Scalar revPdfNM = EvalPdfAtVertexNM(
					vertices.back(), scatDir, -currentRay.Dir(), nm );
				vertices.back().guidingReversePdfDirectionIn = revPdfNM;

				if( revPdfNM > NEARZERO )
				{
					const Scalar fRevNM = usedGuidedDirection ? guidedFNM :
						EvalBSDFAtVertexNM( vertices.back(), -currentRay.Dir(), scatDir, nm );
					const Scalar cosIncoming = fabs( Vector3Ops::Dot(
						-currentRay.Dir(), ri.geometric.vNormal ) );
					const Scalar revWeight = fRevNM * bssrdfReflectCompensation * cosIncoming / revPdfNM;
					vertices.back().guidingReverseScatteringWeight =
						RISEPel( revWeight, revWeight, revWeight );
				}
			}
		}
#endif

		pdfFwdPrev = selectProb * effectivePdf;

		// In Veach's formulation, delta vertices should be "transparent" in the MIS walk
		if( pScat->isDelta ) {
			pdfFwdPrev = 0;
		}

		// Update previous vertex's pdfRev
		if( vertices.size() >= 2 ) {
			const BDPTVertex& curr = vertices.back();
			BDPTVertex& prev = vertices[ vertices.size() - 2 ];

			// Reverse PDF: returns 0 for delta, handled by remap0 in MISWeight.
			const Scalar revPdfSA = EvalPdfAtVertexNM(
				curr, scatDir, -currentRay.Dir(), nm );

			// Geometric cosine read only inside the surface/light branch
			// — medium vertices don't populate geomNormal.  See RGB
			// twin around line 2394 for the same pattern + rationale.
			if( prev.type == BDPTVertex::MEDIUM ) {
				prev.pdfRev = BDPTUtilities::SolidAngleToAreaMedium( revPdfSA, prev.sigma_t_scalar, distSq );
			} else {
				const Scalar absCosAtPrev = fabs(
					Vector3Ops::Dot( prev.geomNormal, currentRay.Dir() ) );
				prev.pdfRev = BDPTUtilities::SolidAngleToArea( revPdfSA, absCosAtPrev, distSq );
			}
			if( pScat->isDelta ) { prev.pdfRev = 0; }
		}

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
	#ifdef RISE_ENABLE_OPENPGL
		if( !usedGuidedDirection && pScat->ior_stack ) {
	#else
		if( pScat->ior_stack ) {
	#endif
			iorStack = *pScat->ior_stack;
		}
	}

	// Record subpath boundary (single contiguous range now that
	// path-tree branching has been excised).
	subpathStarts.push_back( static_cast<uint32_t>( vertices.size() ) );

	return static_cast<unsigned int>( vertices.size() );
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
	const SampledWavelengths* pSwlHWSS
	) const
{
	vertices.clear();
	vertices.reserve( maxEyeDepth + 1 );
	subpathStarts.clear();
	subpathStarts.push_back( 0 );

	// Snapshot once — see parallel comment in GenerateEyeSubpath.
	const ICamera* pCamera = scene.GetCamera();

	// Vertex 0: the camera
	{
		BDPTVertex v;
		v.type = BDPTVertex::CAMERA;

		if( pCamera ) {
			v.position = pCamera->GetLocation();
		}

		v.normal = cameraRay.Dir();
		v.onb.CreateFromW( cameraRay.Dir() );
		v.pMaterial = 0;
		v.pObject = 0;
		v.pLight = 0;
		v.pLuminary = 0;
		v.screenPos = screenPos;
		v.isDelta = false;
		v.pdfFwd = 1.0;
		v.pdfRev = 0;
		v.throughputNM = 1.0;

		vertices.push_back( v );
	}

	Ray currentRay = cameraRay;
	Scalar betaNM = 1.0;

	Scalar pdfCamDir = 1.0;
	if( pCamera ) {
		pdfCamDir = BDPTCameraUtilities::PdfDirection( *pCamera, cameraRay );
	}

	// VCM post-pass inputs on the camera endpoint (spectral).
	vertices[0].emissionPdfW = pdfCamDir;
	vertices[0].cosAtGen = 1.0;

	Scalar pdfFwdPrev = pdfCamDir;
	IORStack iorStack( 1.0 );
	// Seed from the camera position — see the RGB eye subpath for
	// why this matters and why it's a no-op for cameras in free space.
	if( pCamera ) {
		IORStackSeeding::SeedFromPoint( iorStack, pCamera->GetLocation(), scene );
	}

#ifdef RISE_ENABLE_OPENPGL
	static thread_local GuidingDistributionHandle guideDist;
#endif

	// Per-type bounce counters for StabilityConfig limits
	unsigned int nmEyeDiffuseBounces = 0;
	unsigned int nmEyeGlossyBounces = 0;
	unsigned int nmEyeTransmissionBounces = 0;
	unsigned int nmEyeTranslucentBounces = 0;
	unsigned int nmEyeVolumeBounces = 0;
	unsigned int nmEyeSurfaceBounces = 0;

	// HWSS per-wavelength throughput tracking.  When pSwlHWSS is non-null
	// the RR site below uses max over active wavelengths of the post-
	// scatter throughput, preventing hero-driven RR from amplifying
	// companion wavelengths on rare survivors (see PathTracingIntegrator
	// HWSS RR comment for full rationale).  Kept in sync with betaNM
	// (index 0 is hero; initialized 1.0 for camera start).
	Scalar hwssBetaNM[SampledWavelengths::N];
	if( pSwlHWSS ) {
		for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
			hwssBetaNM[w] = Scalar( 1 );
		}
	}


	// Saturating add; see matching comment in GenerateEyeSubpath.
	// Guard maxEyeDepth >= 1024 so the subtraction doesn't underflow.
	const unsigned int nmMaxEyeTotalDepth =
		( maxEyeDepth >= 1024u ||
		  stabilityConfig.maxVolumeBounce > 1024u - maxEyeDepth ) ?
			1024u :
			maxEyeDepth + stabilityConfig.maxVolumeBounce;

	for( unsigned int depth = 0; depth < nmMaxEyeTotalDepth; depth++ )
	{
		// Per-bounce stream offset on the NM eye subpath.  Streams
		// [16, ...) are reserved for the eye walk; light walk uses
		// [1, ...).
		sampler.StartStream( 16u + depth );

		RayIntersection ri( currentRay, nullRasterizerState );
		scene.GetObjects()->IntersectRay( ri, true, true, false );

		// ----------------------------------------------------------------
		// Participating media: free-flight distance sampling (NM eye subpath).
		// Same algorithm as the RGB variant — see GenerateEyeSubpath.
		// Uses spectral SampleDistanceNM / GetCoefficientsNM so that
		// chromatic media are sampled at the correct wavelength.
		// ----------------------------------------------------------------
		const IObject* pMedObj_nmEye = 0;
		const IMedium* pMed_nmEye = 0;
		{
			const IObject* pMedObj = 0;
			const IMedium* pMed = MediumTracking::GetCurrentMediumWithObject(
				iorStack, &scene, pMedObj );
			pMedObj_nmEye = pMedObj;
			pMed_nmEye = pMed;

			if( pMed )
			{
				const Scalar maxDist = ri.geometric.bHit ? ri.geometric.range : Scalar(1e10);
				bool scattered = false;
				const Scalar t_m = pMed->SampleDistanceNM(
					currentRay, maxDist, nm, sampler, scattered );

				if( scattered && nmEyeVolumeBounces < stabilityConfig.maxVolumeBounce )
				{
					const Point3 scatterPt = currentRay.PointAtLength( t_m );
					const Vector3 wo = currentRay.Dir();
					const MediumCoefficientsNM coeff = pMed->GetCoefficientsNM( scatterPt, nm );
					const Scalar TrNM = pMed->EvalTransmittanceNM( currentRay, t_m, nm );
					const Scalar sigma_t_nm = coeff.sigma_t;

					// NM throughput weight: sigma_s / sigma_t (single-scattering albedo)
					Scalar medWeightNM = 0;
					if( sigma_t_nm > 0 && TrNM > 0 ) {
						medWeightNM = coeff.sigma_s / sigma_t_nm;
					}

					if( medWeightNM <= 0 ) {
						break;
					}

					betaNM *= medWeightNM;

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
					mv.sigma_t_scalar = sigma_t_nm;
					mv.isDelta = false;

					// Medium vertices enclosed by a specular (delta) boundary
					// are not connectible: connection rays are blocked by the
					// specular surface.  Propagate from previous vertex.
					// Exception: vertices in the GLOBAL medium (pMedObj == NULL)
					// are always connectable.
					{
						bool connectible = true;
						if( pMedObj == 0 ) {
							connectible = true;
						} else if( !vertices.empty() ) {
							const BDPTVertex& prev = vertices.back();
							if( prev.type == BDPTVertex::SURFACE && prev.isDelta ) {
								connectible = false;
							} else if( prev.type == BDPTVertex::MEDIUM && !prev.isConnectible ) {
								connectible = false;
							}
						}
						mv.isConnectible = connectible;
					}
					mv.throughputNM = betaNM;

					const Scalar distSqMed = t_m * t_m;
					mv.pdfFwd = BDPTUtilities::SolidAngleToAreaMedium(
						pdfFwdPrev, mv.sigma_t_scalar, distSqMed );
					mv.pdfRev = 0;

					// VCM uses sigma_t_scalar for medium vertices.
					mv.cosAtGen = 0;

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
					betaNM *= phaseVal / phasePdf;

					// Russian roulette
					{
						const PathTransportUtilities::RussianRouletteResult rr =
							PathTransportUtilities::EvaluateRussianRoulette(
								depth + nmEyeVolumeBounces,
								stabilityConfig.rrMinDepth,
								stabilityConfig.rrThreshold,
								fabs( betaNM ),
								fabs( mv.throughputNM ),
								sampler.Get1D() );
						if( rr.terminate ) {
							break;
						}
						if( rr.survivalProb < 1.0 ) {
							betaNM /= rr.survivalProb;
						}
					}

					// Update pdfRev on previous vertex
					if( vertices.size() >= 2 ) {
						BDPTVertex& prev = vertices[ vertices.size() - 2 ];
						const Scalar revPdfSA = phasePdf;

						if( prev.type == BDPTVertex::MEDIUM ) {
							prev.pdfRev = BDPTUtilities::SolidAngleToAreaMedium(
								revPdfSA, prev.sigma_t_scalar, distSqMed );
						} else {
							const Scalar absCosAtPrev = (prev.type == BDPTVertex::CAMERA) ?
								Scalar(1.0) :
								fabs( Vector3Ops::Dot( prev.geomNormal, currentRay.Dir() ) );
							prev.pdfRev = BDPTUtilities::SolidAngleToArea(
								revPdfSA, absCosAtPrev, distSqMed );
						}
					}

					pdfFwdPrev = phasePdf;

					currentRay = Ray( scatterPt, wi );
					currentRay.Advance( BDPT_RAY_EPSILON );

					nmEyeVolumeBounces++;
					continue;
				}
				else if( ri.geometric.bHit )
				{
					const Scalar TrNM = pMed->EvalTransmittanceNM(
						currentRay, ri.geometric.range, nm );
					betaNM *= TrNM;
				}
			}
		}

		if( !ri.geometric.bHit ) {
			break;
		}

		if( nmEyeSurfaceBounces >= maxEyeDepth ) {
			break;
		}
		nmEyeSurfaceBounces++;

		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		BDPTVertex v;
		v.type = BDPTVertex::SURFACE;
		v.position = ri.geometric.ptIntersection;
		v.normal = ri.geometric.vNormal;
		v.geomNormal = ri.geometric.vGeomNormal;
		v.onb = ri.geometric.onb;
		v.ptCoord = ri.geometric.ptCoord;
		v.ptObjIntersec = ri.geometric.ptObjIntersec;
		v.pMaterial = ri.pMaterial;
		v.pObject = ri.pObject;
		v.pLight = 0;
		v.pLuminary = 0;
		v.pMediumObject = pMedObj_nmEye;
		v.pMediumVol = pMed_nmEye;
		if( ri.pObject ) {
			iorStack.SetCurrentObject( ri.pObject );
			v.mediumIOR = iorStack.top();
			v.insideObject = iorStack.containsCurrent();
		}

		const Scalar distSq = ri.geometric.range * ri.geometric.range;
		// Solid-angle <-> area Jacobian uses the GEOMETRIC normal —
		// area-element parameterisation depends on the actual face
		// orientation (Veach 1997 §8.2.2 / PBRT 4e §13.6.4).
		const Scalar absCosIn = fabs( Vector3Ops::Dot(
			ri.geometric.vGeomNormal, -currentRay.Dir() ) );

		v.pdfFwd = BDPTUtilities::SolidAngleToArea( pdfFwdPrev, absCosIn, distSq );
		v.throughputNM = betaNM;
		v.pdfRev = 0;
		v.isDelta = false;

		// VCM post-pass input (spectral).
		v.cosAtGen = absCosIn;

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

		// Sample the SPF at this wavelength
		ScatteredRayContainer scattered;
		pSPF->ScatterNM( ri.geometric, sampler, nm, scattered, iorStack );

		if( scattered.Count() == 0 ) {
			break;
		}

		// Stochastic single-lobe selection (no path-tree branching).
		// Consume one sampler dimension for Sobol alignment.
		const Scalar lobeSelectXi = sampler.Get1D();
		const ScatteredRay* pScat;
		Scalar selectProb = 1.0;

		{
			pScat = scattered.RandomlySelect( lobeSelectXi, true );
			if( !pScat ) {
				break;
			}
			// Compute lobe selection probability using spectral krayNM weights
			if( scattered.Count() > 1 ) {
				Scalar totalKray = 0;
				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					totalKray += fabs( scattered[i].krayNM );
				}
				const Scalar selectedKray = fabs( pScat->krayNM );
				if( totalKray > NEARZERO && selectedKray > NEARZERO ) {
					selectProb = selectedKray / totalKray;
				}
			}
		}

		// Determine connectibility: true if any scattered lobe is non-delta
		{
			bool hasNonDelta = false;
			for( unsigned int i = 0; i < scattered.Count(); i++ ) {
				if( !scattered[i].isDelta ) { hasNonDelta = true; break; }
			}
			if( !ri.pMaterial->GetBSDF() ) {
				hasNonDelta = false;
			}
			vertices.back().isConnectible = hasNonDelta;
		}

		vertices.back().isDelta = pScat->isDelta;



		// --- BSSRDF sampling (NM eye subpath) ---
		Scalar bssrdfReflectCompensation = 1.0;
		if( ri.pMaterial && ri.pMaterial->GetDiffusionProfile() )
		{
			// Front-face gate uses GEOMETRIC; Fresnel cosine uses SHADING.
			// PBRT 4e §10.1.1 (front/back is geometric); §11.4.2 (BSSRDF
			// Fresnel angular dependence is shading-frame).
			const Vector3 wo_bss = -currentRay.Dir();
			const Scalar cosInGeom = Vector3Ops::Dot( ri.geometric.vGeomNormal, wo_bss );
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
					ri.geometric, ri.pObject, ri.pMaterial, sampler, nm );

				if( bssrdf.valid )
				{
					vertices.back().isDelta = true;
					const Scalar betaSpatialNM = betaNM * bssrdf.weightSpatialNM / Ft;
					betaNM = betaNM * bssrdf.weightNM / Ft;

					BDPTVertex entryV;
					entryV.type = BDPTVertex::SURFACE;
					entryV.position = bssrdf.entryPoint;
					entryV.normal = bssrdf.entryNormal;
					entryV.geomNormal = bssrdf.entryGeomNormal;
					entryV.onb = bssrdf.entryONB;
					entryV.pMaterial = ri.pMaterial;
					entryV.pObject = ri.pObject;
					entryV.pMediumObject = pMedObj_nmEye;
					entryV.pMediumVol = pMed_nmEye;
					entryV.isDelta = false;
					entryV.isConnectible = true;
					entryV.isBSSRDFEntry = true;
					entryV.throughputNM = betaSpatialNM;
					entryV.pdfFwd = bssrdf.pdfSurface;
					entryV.pdfRev = 0;
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
		// --- Random-walk SSS (NM eye subpath) ---
		else if( ri.pMaterial )
		{
			const RandomWalkSSSParams* pRW = ri.pMaterial->GetRandomWalkSSSParams();
			RandomWalkSSSParams rwParamsNM;
			if( !pRW && ri.pMaterial->GetRandomWalkSSSParamsNM( nm, rwParamsNM ) ) {
				pRW = &rwParamsNM;
			}
			const Scalar cosIn = pRW ? Vector3Ops::Dot(
				ri.geometric.vNormal, -currentRay.Dir() ) : 0;
			if( pRW && cosIn > NEARZERO )
			{
				const Scalar F0 = ((pRW->ior - 1.0) / (pRW->ior + 1.0)) *
					((pRW->ior - 1.0) / (pRW->ior + 1.0));
				const Scalar F = F0 + (1.0 - F0) * pow( 1.0 - cosIn, 5.0 );
				const Scalar Ft = 1.0 - F;
				const Scalar R = F;

				if( Ft > NEARZERO && sampler.Get1D() < Ft )
				{
					// See RGB light subpath comment for rationale.
					IndependentSampler walkSampler( rc.random );
					ISampler& rwSampler = sampler.HasFixedDimensionBudget()
						? static_cast<ISampler&>(walkSampler) : sampler;

					BSSRDFSampling::SampleResult bssrdf = RandomWalkSSS::SampleExit(
						ri.geometric, ri.pObject,
						pRW->sigma_a, pRW->sigma_s, pRW->sigma_t,
						pRW->g, pRW->ior, pRW->maxBounces, rwSampler, nm, pRW->maxDepth );

					if( bssrdf.valid )
					{
						vertices.back().isDelta = true;

						// SampleExit does NOT include Ft(entry).
						// Coin flip: weight * Ft / Ft = weight.
						// Apply boundary filter (e.g. melanin double-pass).
						const Scalar bf = pRW->boundaryFilter;
						const Scalar betaSpatialNM = betaNM * bssrdf.weightSpatialNM * bf;
						betaNM = betaNM * bssrdf.weightNM * bf;

						BDPTVertex entryV;
						entryV.type = BDPTVertex::SURFACE;
						entryV.position = bssrdf.entryPoint;
						entryV.normal = bssrdf.entryNormal;
						entryV.geomNormal = bssrdf.entryGeomNormal;
						entryV.onb = bssrdf.entryONB;
						entryV.pMaterial = ri.pMaterial;
						entryV.pObject = ri.pObject;
						entryV.pMediumObject = pMedObj_nmEye;
						entryV.pMediumVol = pMed_nmEye;

						// See RGB light subpath block for rationale.
						entryV.isDelta = true;
						entryV.isConnectible = false;
						entryV.isBSSRDFEntry = true;
						entryV.throughputNM = betaSpatialNM;
						entryV.pdfFwd = 0;
						entryV.pdfRev = 0;
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

#ifdef RISE_ENABLE_OPENPGL
		// --- Path guiding (NM eye subpath) ---
		bool usedGuidedDirection = false;
		Vector3 guidedDir;
		Scalar guidedFNM = 0;
		Scalar guidedEffectivePdf = 0;
		Scalar bsdfCombinedPdf = 0;

		if( pGuidingField && pGuidingField->IsTrained() &&
			depth < maxGuidingDepth && GuidingSupportsSurfaceSampling( *pScat ) &&
			vertices.back().isConnectible )
		{
			if( pGuidingField->InitDistribution( guideDist, v.position, sampler.Get1D() ) )
			{
				if( pScat->type == ScatteredRay::eRayDiffuse ) {
					pGuidingField->ApplyCosineProduct(
						guideDist,
						GuidingCosineNormal( v.normal, currentRay.Dir() ) );
				}

				const Scalar alpha = guidingAlpha;

				if( guidingSamplingType == eGuidingRIS )
				{
					// RIS-based guiding (BDPT NM eye subpath)
					PathTransportUtilities::GuidingRISCandidate<Scalar> candidates[2];

					// Candidate 0: BSDF sample
					{
						PathTransportUtilities::GuidingRISCandidate<Scalar>& c = candidates[0];
						c.direction = pScat->ray.Dir();
						c.bsdfEval = EvalBSDFAtVertexNM(
							vertices.back(), c.direction, -currentRay.Dir(), nm );
						c.bsdfPdf = pScat->pdf;
						c.guidePdf = pGuidingField->Pdf( guideDist, c.direction );
						c.incomingRadPdf = pGuidingField->IncomingRadiancePdf( guideDist, c.direction );
						c.cosTheta = fabs( Vector3Ops::Dot( c.direction, v.normal ) );
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
						Scalar gPdf = 0;
						const Point2 xi2d( sampler.Get1D(), sampler.Get1D() );
						c.direction = pGuidingField->Sample( guideDist, xi2d, gPdf );
						c.guidePdf = gPdf;

						if( gPdf > NEARZERO )
						{
							c.bsdfEval = EvalBSDFAtVertexNM(
								vertices.back(), c.direction, -currentRay.Dir(), nm );
							c.bsdfPdf = EvalPdfAtVertexNM(
								vertices.back(), c.direction, -currentRay.Dir(), nm );
							c.incomingRadPdf = pGuidingField->IncomingRadiancePdf( guideDist, c.direction );
							c.cosTheta = fabs( Vector3Ops::Dot( c.direction, v.normal ) );
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
					const unsigned int sel = PathTransportUtilities::GuidingRISSelectCandidate(
						candidates, 2, sampler.Get1D(), risEffectivePdf );

					if( risEffectivePdf > NEARZERO && candidates[sel].valid )
					{
						usedGuidedDirection = true;
						guidedDir = candidates[sel].direction;
						guidedFNM = candidates[sel].bsdfEval;
						guidedEffectivePdf = risEffectivePdf;
					}
				}
				else
				{
					// One-sample MIS (BDPT NM eye subpath)
					const Scalar xi = sampler.Get1D();

					if( PathTransportUtilities::ShouldUseGuidedSample( alpha, xi ) )
					{
						Scalar guidePdf = 0;
						const Point2 xi2d( sampler.Get1D(), sampler.Get1D() );
						const Vector3 gDir = pGuidingField->Sample( guideDist, xi2d, guidePdf );

						if( guidePdf > NEARZERO )
						{
							guidedFNM = EvalBSDFAtVertexNM(
								vertices.back(), gDir, -currentRay.Dir(), nm );
							const Scalar bsdfPdf = EvalPdfAtVertexNM(
								vertices.back(), gDir, -currentRay.Dir(), nm );

							const Scalar combinedPdf =
								PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdf, bsdfPdf );

							if( combinedPdf > NEARZERO && guidedFNM > NEARZERO )
							{
								usedGuidedDirection = true;
								guidedDir = gDir;
								guidedEffectivePdf = combinedPdf;
							}
						}
					}

					if( !usedGuidedDirection )
					{
						const Scalar guidePdfForBsdfDir =
							pGuidingField->Pdf( guideDist, pScat->ray.Dir() );
						bsdfCombinedPdf =
							PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdfForBsdfDir, pScat->pdf );
					}
				}
			}
		}

		const Scalar trainingEffectivePdf =
			usedGuidedDirection ? guidedEffectivePdf :
			(bsdfCombinedPdf > NEARZERO ? bsdfCombinedPdf : pScat->pdf);
		const Scalar trainingPdf = selectProb * trainingEffectivePdf;
		if( pGuidingField && pGuidingField->IsCollectingTrainingSamples() &&
			GuidingSupportsSurfaceSampling( *pScat ) && trainingPdf > NEARZERO )
		{
			const Ray trainingRay = usedGuidedDirection ?
				Ray( pScat->ray.origin, guidedDir ) : pScat->ray;
			const IORStack& trainingIorStack = usedGuidedDirection ?
				iorStack :
				(pScat->ior_stack ? *pScat->ior_stack : iorStack);
			RecordGuidingTrainingSampleNM(
				pGuidingField,
				rc,
				caster,
				ri,
				trainingRay,
				trainingPdf,
				depth + 2,
				nm,
				trainingIorStack );
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
		} else if( bsdfCombinedPdf > NEARZERO ) {
			effectivePdf = bsdfCombinedPdf;
		}
#endif

		if( effectivePdf <= 0 ) {
			break;
		}

		// Per-type bounce limits
		if( PathTransportUtilities::ExceedsBounceLimitForType(
				pScat->type, nmEyeDiffuseBounces, nmEyeGlossyBounces,
				nmEyeTransmissionBounces, nmEyeTranslucentBounces, stabilityConfig ) ) {
			break;
		}

		// Snapshot pre-scatter HWSS throughput so the RR below can
		// compare max(pre) vs max(post) over active wavelengths without
		// floating-point gymnastics.
		Scalar hwssBetaNMPre[SampledWavelengths::N];
		if( pSwlHWSS ) {
			for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
				hwssBetaNMPre[w] = hwssBetaNM[w];
			}
		}

		// Throughput update using valueNM.  When HWSS is active, update
		// hwssBetaNM[w] in parallel — delta scatter scales all
		// wavelengths by the same krayNM (non-dispersive; dispersive
		// already terminated); non-delta scatter evaluates BSDF at
		// each companion wavelength along the hero-sampled direction.
		if( pScat->isDelta ) {
			betaNM = betaNM * pScat->krayNM * bssrdfReflectCompensation / selectProb;
			if( pSwlHWSS ) {
				const Scalar deltaScale = pScat->krayNM * bssrdfReflectCompensation / selectProb;
				hwssBetaNM[0] = betaNM;
				for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
					if( pSwlHWSS->terminated[w] ) continue;
					hwssBetaNM[w] = hwssBetaNM[w] * deltaScale;
				}
			}
		} else {
			Scalar fNM;
#ifdef RISE_ENABLE_OPENPGL
			fNM = usedGuidedDirection ? guidedFNM :
				EvalBSDFAtVertexNM( vertices.back(), scatDir, -currentRay.Dir(), nm );
#else
			fNM = EvalBSDFAtVertexNM( vertices.back(), scatDir, -currentRay.Dir(), nm );
#endif
			const Scalar cosTheta = fabs( Vector3Ops::Dot(
				scatDir, ri.geometric.vNormal ) );

			if( fNM <= 0 ) {
				break;
			}
			betaNM = betaNM * fNM * bssrdfReflectCompensation * cosTheta / (selectProb * effectivePdf);
			if( pSwlHWSS ) {
				const Scalar invScale = bssrdfReflectCompensation * cosTheta / ( selectProb * effectivePdf );
				hwssBetaNM[0] = betaNM;
				for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
					if( pSwlHWSS->terminated[w] ) continue;
					const Scalar fw = EvalBSDFAtVertexNM(
						vertices.back(), scatDir, -currentRay.Dir(), pSwlHWSS->lambda[w] );
					hwssBetaNM[w] = hwssBetaNM[w] * fw * invScale;
				}
			}
		}

		// Russian Roulette — configurable depth and throughput floor.
		// In HWSS mode the survival probability uses the MAX throughput
		// across all active wavelengths so hero-low / companion-high
		// configurations don't get nearly terminated + amplified on
		// rare survival.  Mirrors the PT HWSS RR fix.
		{
			Scalar rrPrevMax = fabs( vertices.back().throughputNM );
			Scalar rrCurrMax = fabs( betaNM );
			if( pSwlHWSS ) {
				for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
					if( pSwlHWSS->terminated[w] ) continue;
					const Scalar p = fabs( hwssBetaNMPre[w] );
					if( p > rrPrevMax ) rrPrevMax = p;
					const Scalar c = fabs( hwssBetaNM[w] );
					if( c > rrCurrMax ) rrCurrMax = c;
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
				betaNM *= rrScale;
				if( pSwlHWSS ) {
					for( unsigned int w = 0; w < SampledWavelengths::N; w++ ) {
						hwssBetaNM[w] *= rrScale;
					}
				}
			}
		}

		pdfFwdPrev = selectProb * effectivePdf;

		// In Veach's formulation, delta vertices should be "transparent" in the MIS walk
		if( pScat->isDelta ) {
			pdfFwdPrev = 0;
		}

		if( vertices.size() >= 2 ) {
			const BDPTVertex& curr = vertices.back();
			BDPTVertex& prev = vertices[ vertices.size() - 2 ];

			// Reverse PDF: returns 0 for delta, handled by remap0 in MISWeight.
			const Scalar revPdfSA = EvalPdfAtVertexNM(
				curr, scatDir, -currentRay.Dir(), nm );

			// Gate the geometric-cosine read behind the type checks —
			// CAMERA gets the sentinel 1.0, MEDIUM bypasses cos via
			// sigma_t.  Reading prev.geomNormal on a medium vertex
			// would consume zero-init data.  Mirrors the RGB fix at
			// line ~3486.
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
	#ifdef RISE_ENABLE_OPENPGL
		if( !usedGuidedDirection && pScat->ior_stack ) {
	#else
		if( pScat->ior_stack ) {
	#endif
			iorStack = *pScat->ior_stack;
		}
	}

	// Record subpath boundary (single contiguous range now that
	// path-tree branching has been excised).
	subpathStarts.push_back( static_cast<uint32_t>( vertices.size() ) );

	return static_cast<unsigned int>( vertices.size() );
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
	const Scalar nm
	) const
{
	ConnectionResultNM result;
	result.s = s;

	if( s > lightVerts.size() || t > eyeVerts.size() ) {
		return result;
	}
	if( s + t < 2 ) {
		return result;
	}

	//
	// Case: s == 0 -- eye path hits emitter directly
	//
	if( s == 0 )
	{
		const BDPTVertex& eyeEnd = eyeVerts[t - 1];

		if( eyeEnd.type != BDPTVertex::SURFACE || !eyeEnd.pMaterial ) {
			return result;
		}

		if( !eyeEnd.pMaterial->GetEmitter() ) {
			return result;
		}

		Vector3 woFromEmitter;
		if( t >= 2 ) {
			woFromEmitter = Vector3Ops::mkVector3( eyeVerts[t - 2].position, eyeEnd.position );
			woFromEmitter = Vector3Ops::Normalize( woFromEmitter );
		} else {
			return result;
		}

		const Scalar LeNM = EvalEmitterRadianceNM( eyeEnd, woFromEmitter, nm );
		if( LeNM <= 0 ) {
			return result;
		}

		result.contribution = eyeEnd.throughputNM * LeNM;
		result.needsSplat = false;
		result.valid = true;

		// --- Update pdfRev at emitter vertex for correct MIS ---
		const Scalar savedEyeEndPdfRev = eyeEnd.pdfRev;

		if( pLightSampler && eyeEnd.pObject )
		{
			const ILuminaryManager* pLumMgr = caster.GetLuminaries();
			const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
			LuminaryManager::LuminariesList emptyList;
			const LuminaryManager::LuminariesList& luminaries = pLumManager ?
				const_cast<LuminaryManager*>( pLumManager )->getLuminaries() : emptyList;

			// For BVH PDF, the shading point is the predecessor vertex
			const BDPTVertex& predVert_s0_NM = eyeVerts[t - 2];
			const Scalar pdfSelect = pLightSampler->PdfSelectLuminary(
				scene, luminaries, *eyeEnd.pObject,
				predVert_s0_NM.position, predVert_s0_NM.normal );
			const Scalar area = eyeEnd.pObject->GetArea();
			const Scalar pdfPosition = (area > 0) ? (Scalar(1.0) / area) : 0;
			const_cast<BDPTVertex&>( eyeEnd ).pdfRev = pdfSelect * pdfPosition;
		}

		// --- Update predecessor pdfRev (eyeVerts[t-2]) ---
		Scalar savedEyePredPdfRevNM_s0 = 0;
		const bool hasEyePredNM_s0 = (t >= 2);
		if( hasEyePredNM_s0 )
		{
			const BDPTVertex& eyePred = eyeVerts[t - 2];
			savedEyePredPdfRevNM_s0 = eyePred.pdfRev;

			Scalar emPdfDir = 0;
			if( eyeEnd.pMaterial ) {
				const IEmitter* pEm = eyeEnd.pMaterial->GetEmitter();
				if( pEm ) {
					// One-sided emission PDF
					const Scalar cosAtEmitter = Vector3Ops::Dot( eyeEnd.geomNormal, woFromEmitter );
					emPdfDir = (cosAtEmitter > 0) ? (cosAtEmitter * INV_PI) : 0;
				}
			}

			const Vector3 dToPred = Vector3Ops::mkVector3( eyePred.position, eyeEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			// Camera: 1.0; Medium: sigma_t/dist^2
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

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );
		const_cast<BDPTVertex&>( eyeEnd ).pdfRev = savedEyeEndPdfRev;
		if( hasEyePredNM_s0 ) {
			const_cast<BDPTVertex&>( eyeVerts[t - 2] ).pdfRev = savedEyePredPdfRevNM_s0;
		}
		return result;
	}

	//
	// Legacy t == 0 path-to-camera case (NM).
	// Same as RGB: legacy path kept for compatibility.
	//
	if( t == 0 )
	{
		const BDPTVertex& lightEnd = lightVerts[s - 1];
		if( !lightEnd.isConnectible ) {
			return result;
		}

		Point2 rasterPos;
		if( !BDPTCameraUtilities::Rasterize( camera, lightEnd.position, rasterPos ) ) {
			return result;
		}

		const Point3 camPos = camera.GetLocation();
		if( !IsVisible( caster, camPos, lightEnd.position ) ) {
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
			fLightNM = EvalBSDFAtVertexNM( lightEnd, wiAtLight, dirToCam, nm );
		}

		const Scalar distSq = dist * dist;
		const Scalar absCosLight = lightEnd.isDelta ?
			Scalar(1.0) : fabs( Vector3Ops::Dot( lightEnd.geomNormal, dirToCam ) );
		const Scalar G = absCosLight / distSq;

		result.contribution = lightEnd.throughputNM * fLightNM * G * We;
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

			const Scalar pdfPredSA = EvalPdfAtVertexNM( lightEnd, dirToCam, wiAtLightEnd, nm );
			const Vector3 dToPred = Vector3Ops::mkVector3( lightPred.position, lightEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			const Scalar absCosAtPred = fabs( Vector3Ops::Dot( lightPred.geomNormal,
				Vector3Ops::Normalize( dToPred ) ) );
			const_cast<BDPTVertex&>( lightPred ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfPredSA, absCosAtPred, distPredSq );
		}

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );
		const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRev;
		if( hasLightPredNM_t0 ) {
			const_cast<BDPTVertex&>( lightVerts[s - 2] ).pdfRev = savedLightPredPdfRevNM_t0;
		}
		return result;
	}

	//
	// Case: s == 1 -- connect eye endpoint to light vertex
	//
	if( s == 1 )
	{
		const BDPTVertex& eyeEnd = eyeVerts[t - 1];
		const BDPTVertex& lightStart = lightVerts[0];

		// Eye endpoint: surface or medium
		if( eyeEnd.type != BDPTVertex::SURFACE && eyeEnd.type != BDPTVertex::MEDIUM ) {
			return result;
		}
		if( eyeEnd.type == BDPTVertex::SURFACE && !eyeEnd.pMaterial ) {
			return result;
		}
		if( !eyeEnd.isConnectible ) {
			return result;
		}

		const bool eyeIsMediumNM_s1 = (eyeEnd.type == BDPTVertex::MEDIUM);

		Vector3 dirToLight = Vector3Ops::mkVector3( lightStart.position, eyeEnd.position );
		const Scalar dist = Vector3Ops::Magnitude( dirToLight );
		if( dist < BDPT_RAY_EPSILON ) {
			return result;
		}
		dirToLight = dirToLight * (1.0 / dist);

		if( !IsVisible( caster, eyeEnd.position, lightStart.position ) ) {
			return result;
		}

		// Evaluate Le at this wavelength
		Scalar LeNM = 0;
		if( lightStart.pLuminary && lightStart.pLuminary->GetMaterial() ) {
			const IEmitter* pEmitter = lightStart.pLuminary->GetMaterial()->GetEmitter();
			if( pEmitter ) {
				RayIntersectionGeometric rig(
					Ray( lightStart.position, -dirToLight ), nullRasterizerState );
				rig.bHit = true;
				rig.ptIntersection = lightStart.position;
				rig.vNormal = lightStart.normal;
				rig.vGeomNormal = lightStart.geomNormal;
				rig.onb = lightStart.onb;
				LeNM = pEmitter->emittedRadianceNM( rig, -dirToLight, lightStart.geomNormal, nm );
			}
		} else if( lightStart.pLight ) {
			const RISEPel Le = lightStart.pLight->emittedRadiance( -dirToLight );
			LeNM = 0.2126 * Le[0] + 0.7152 * Le[1] + 0.0722 * Le[2];
		}

		if( LeNM <= 0 ) {
			return result;
		}

		Vector3 woAtEye;
		if( t >= 2 ) {
			woAtEye = Vector3Ops::mkVector3( eyeVerts[t - 2].position, eyeEnd.position );
			woAtEye = Vector3Ops::Normalize( woAtEye );
		} else {
			return result;
		}

		const Scalar fEyeNM = EvalBSDFAtVertexNM( eyeEnd, dirToLight, woAtEye, nm );
		if( fEyeNM <= 0 ) {
			return result;
		}

		// Medium-aware geometric term
		Scalar G;
		if( lightStart.isDelta ) {
			const Scalar dist2 = dist * dist;
			if( eyeIsMediumNM_s1 ) {
				G = 1.0 / dist2;
			} else {
				const Scalar absCosEye = fabs( Vector3Ops::Dot( eyeEnd.geomNormal, dirToLight ) );
				G = absCosEye / dist2;
			}
		} else {
			if( eyeIsMediumNM_s1 ) {
				G = BDPTUtilities::GeometricTermSurfaceMedium( lightStart.position, lightStart.geomNormal, eyeEnd.position );
			} else {
				G = BDPTUtilities::GeometricTerm( lightStart.position, lightStart.geomNormal, eyeEnd.position, eyeEnd.geomNormal );
			}
		}

		// Connection transmittance (NM)
		const Scalar Tr_connNM_s1 = EvalConnectionTransmittanceNM(
			eyeEnd.position, lightStart.position, scene, caster, nm,
			eyeEnd.pMediumObject, eyeEnd.pMediumVol );

		const Scalar pdfLight = lightStart.pdfFwd;
		if( pdfLight <= 0 ) {
			return result;
		}

		result.contribution = eyeEnd.throughputNM * fEyeNM * LeNM * Tr_connNM_s1 * G / pdfLight;
		result.needsSplat = false;
		result.valid = true;

		// --- Update pdfRev at connection vertices for correct MIS ---
		const Scalar distSq_conn = dist * dist;
		const Scalar savedLightPdfRev = lightStart.pdfRev;
		const Scalar savedEyePdfRev = eyeEnd.pdfRev;

		if( !lightStart.isDelta ) {
			const Scalar pdfRevSA = EvalPdfAtVertexNM( eyeEnd, woAtEye, dirToLight, nm );
			const Scalar absCosAtLight = fabs( Vector3Ops::Dot( lightStart.geomNormal, dirToLight ) );
			const_cast<BDPTVertex&>( lightStart ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfRevSA, absCosAtLight, distSq_conn );
		}

		{
			Scalar emissionPdfDir = 0;
			if( lightStart.pLuminary ) {
				const Scalar cosAtLight = Vector3Ops::Dot( lightStart.geomNormal, -dirToLight );
				emissionPdfDir = (cosAtLight > 0) ? (cosAtLight * INV_PI) : 0;
			} else if( lightStart.pLight ) {
				emissionPdfDir = lightStart.pLight->pdfDirection( -dirToLight );
			}
			if( eyeIsMediumNM_s1 ) {
				const_cast<BDPTVertex&>( eyeEnd ).pdfRev =
					BDPTUtilities::SolidAngleToAreaMedium( emissionPdfDir, eyeEnd.sigma_t_scalar, distSq_conn );
			} else {
				const Scalar absCosAtEye = fabs( Vector3Ops::Dot( eyeEnd.geomNormal, dirToLight ) );
				const_cast<BDPTVertex&>( eyeEnd ).pdfRev =
					BDPTUtilities::SolidAngleToArea( emissionPdfDir, absCosAtEye, distSq_conn );
			}
		}

		// --- Update predecessor pdfRev (eyeVerts[t-2]) ---
		Scalar savedEyePredPdfRevNM_s1 = 0;
		const bool hasEyePredNM_s1 = (t >= 2);
		if( hasEyePredNM_s1 )
		{
			const BDPTVertex& eyePred = eyeVerts[t - 2];
			savedEyePredPdfRevNM_s1 = eyePred.pdfRev;

			const Vector3 dToPred = Vector3Ops::mkVector3( eyePred.position, eyeEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
			const Vector3 dirToPred = Vector3Ops::Normalize( dToPred );
			const Scalar pdfPredSA = EvalPdfAtVertexNM( eyeEnd, dirToLight, dirToPred, nm );
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

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );
		const_cast<BDPTVertex&>( lightStart ).pdfRev = savedLightPdfRev;
		const_cast<BDPTVertex&>( eyeEnd ).pdfRev = savedEyePdfRev;
		if( hasEyePredNM_s1 ) {
			const_cast<BDPTVertex&>( eyeVerts[t - 2] ).pdfRev = savedEyePredPdfRevNM_s1;
		}
		return result;
	}

	//
	// Case: t == 1 -- connect light endpoint to camera
	//
	if( t == 1 )
	{
		const BDPTVertex& lightEnd = lightVerts[s - 1];
		if( !lightEnd.isConnectible ) {
			return result;
		}

		// Surface or medium vertices can connect to camera
		if( s >= 2 && lightEnd.type != BDPTVertex::SURFACE && lightEnd.type != BDPTVertex::MEDIUM ) {
			return result;
		}

		const bool lightIsMediumNM_t1 = (lightEnd.type == BDPTVertex::MEDIUM);

		Point2 rasterPos;
		if( !BDPTCameraUtilities::Rasterize( camera, lightEnd.position, rasterPos ) ) {
			return result;
		}

		// Same validity rule as the RGB path above: t == 1 only handles a
		// direct camera connection, not a refracted one through glass.
		const Point3 camPos = camera.GetLocation();
		if( !IsVisible( caster, lightEnd.position, camPos ) ) {
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
		if( (lightEnd.type == BDPTVertex::SURFACE && lightEnd.pMaterial) || lightIsMediumNM_t1 ) {
			if( s >= 2 ) {
				Vector3 wiAtLight = Vector3Ops::mkVector3( lightVerts[s - 2].position, lightEnd.position );
				wiAtLight = Vector3Ops::Normalize( wiAtLight );
				fLightNM = EvalBSDFAtVertexNM( lightEnd, wiAtLight, dirToCam, nm );
			}
		} else if( lightEnd.type == BDPTVertex::LIGHT ) {
			// s == 1: light directly connects to camera
			Scalar LeNM = 0;
			if( lightEnd.pLuminary && lightEnd.pLuminary->GetMaterial() ) {
				const IEmitter* pEmitter = lightEnd.pLuminary->GetMaterial()->GetEmitter();
				if( pEmitter ) {
					RayIntersectionGeometric rig(
						Ray( lightEnd.position, dirToCam ), nullRasterizerState );
					rig.bHit = true;
					rig.ptIntersection = lightEnd.position;
					rig.vNormal = lightEnd.normal;
					rig.vGeomNormal = lightEnd.geomNormal;
					rig.onb = lightEnd.onb;
					LeNM = pEmitter->emittedRadianceNM( rig, dirToCam, lightEnd.geomNormal, nm );
				}
			} else if( lightEnd.pLight ) {
				const RISEPel Le = lightEnd.pLight->emittedRadiance( dirToCam );
				LeNM = 0.2126 * Le[0] + 0.7152 * Le[1] + 0.0722 * Le[2];
			}

			const Scalar pdfLight = lightEnd.pdfFwd;
			if( pdfLight <= 0 ) {
				return result;
			}

			const Scalar distSq = dist * dist;
			const Scalar absCosLight = lightEnd.isDelta ?
				Scalar(1.0) : fabs( Vector3Ops::Dot( lightEnd.geomNormal, dirToCam ) );
			const Scalar G = absCosLight / distSq;

			result.contribution = LeNM * G * We / pdfLight;
			result.rasterPos = rasterPos;
			result.needsSplat = true;
			result.valid = true;

			// --- Update pdfRev at connection vertices for correct MIS ---
			const Scalar savedLightPdfRevNM = lightEnd.pdfRev;
			const Scalar savedEyePdfRevNM = eyeVerts[0].pdfRev;

			{
				Ray camRayToLight( camPos, -dirToCam );
				const Scalar camPdfDir = BDPTCameraUtilities::PdfDirection( camera, camRayToLight );
				const_cast<BDPTVertex&>( lightEnd ).pdfRev =
					BDPTUtilities::SolidAngleToArea( camPdfDir, absCosLight, distSq );
			}
			{
				Scalar emPdfDir = 0;
				if( lightEnd.pLuminary ) {
					// One-sided emission PDF
					const Scalar cosEmit = Vector3Ops::Dot( lightEnd.geomNormal, dirToCam );
					emPdfDir = (cosEmit > 0) ? (cosEmit * INV_PI) : 0;
				} else if( lightEnd.pLight ) {
					emPdfDir = lightEnd.pLight->pdfDirection( dirToCam );
				}
				const_cast<BDPTVertex&>( eyeVerts[0] ).pdfRev =
					BDPTUtilities::SolidAngleToArea( emPdfDir, Scalar(1.0), distSq );
			}

			result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );
			const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRevNM;
			const_cast<BDPTVertex&>( eyeVerts[0] ).pdfRev = savedEyePdfRevNM;
			return result;
		}

		if( fLightNM <= 0 ) {
			return result;
		}

		// Medium-aware geometric term (camera has no surface normal)
		const Scalar distSq = dist * dist;
		const Scalar absCosLight = lightIsMediumNM_t1 ?
			Scalar(1.0) : fabs( Vector3Ops::Dot( lightEnd.geomNormal, dirToCam ) );
		const Scalar G = absCosLight / distSq;

		// Connection transmittance (NM)
		const Scalar Tr_connNM_t1 = EvalConnectionTransmittanceNM(
			lightEnd.position, camPos, scene, caster, nm,
			lightEnd.pMediumObject, lightEnd.pMediumVol );

		result.contribution = lightEnd.throughputNM * fLightNM * Tr_connNM_t1 * G * We;
		result.rasterPos = rasterPos;
		result.needsSplat = true;
		result.valid = true;

		// --- Update pdfRev at connection vertices for correct MIS ---
		const Scalar savedLightPdfRevNM2 = lightEnd.pdfRev;
		const Scalar savedEyePdfRevNM2 = eyeVerts[0].pdfRev;

		{
			Ray camRayToLight( camPos, -dirToCam );
			const Scalar camPdfDir = BDPTCameraUtilities::PdfDirection( camera, camRayToLight );
			if( lightIsMediumNM_t1 ) {
				const_cast<BDPTVertex&>( lightEnd ).pdfRev =
					BDPTUtilities::SolidAngleToAreaMedium( camPdfDir, lightEnd.sigma_t_scalar, distSq );
			} else {
				const_cast<BDPTVertex&>( lightEnd ).pdfRev =
					BDPTUtilities::SolidAngleToArea( camPdfDir, absCosLight, distSq );
			}
		}

		if( s >= 2 && (lightEnd.pMaterial || lightIsMediumNM_t1) ) {
			Vector3 wiAtLightMIS = Vector3Ops::mkVector3(
				lightVerts[s - 2].position, lightEnd.position );
			wiAtLightMIS = Vector3Ops::Normalize( wiAtLightMIS );
			const Scalar pdfRevSA = EvalPdfAtVertexNM( lightEnd, wiAtLightMIS, dirToCam, nm );
			const_cast<BDPTVertex&>( eyeVerts[0] ).pdfRev =
				BDPTUtilities::SolidAngleToArea( pdfRevSA, Scalar(1.0), distSq );
		}

		// --- Update predecessor pdfRev (lightVerts[s-2]) ---
		Scalar savedLightPredPdfRevNM_t1 = 0;
		const bool hasLightPredNM_t1 = (s >= 2 && (lightEnd.pMaterial || lightIsMediumNM_t1));
		if( hasLightPredNM_t1 )
		{
			const BDPTVertex& lightPred = lightVerts[s - 2];
			savedLightPredPdfRevNM_t1 = lightPred.pdfRev;

			Vector3 wiAtLightEnd = Vector3Ops::mkVector3(
				lightVerts[s - 2].position, lightEnd.position );
			wiAtLightEnd = Vector3Ops::Normalize( wiAtLightEnd );

			const Scalar pdfPredSA = EvalPdfAtVertexNM( lightEnd, dirToCam, wiAtLightEnd, nm );
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

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );
		const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRevNM2;
		const_cast<BDPTVertex&>( eyeVerts[0] ).pdfRev = savedEyePdfRevNM2;
		if( hasLightPredNM_t1 ) {
			const_cast<BDPTVertex&>( lightVerts[s - 2] ).pdfRev = savedLightPredPdfRevNM_t1;
		}
		return result;
	}

	//
	// General case: s > 1, t > 1
	//
	{
		const BDPTVertex& lightEnd = lightVerts[s - 1];
		const BDPTVertex& eyeEnd = eyeVerts[t - 1];

		if( !lightEnd.isConnectible || !eyeEnd.isConnectible ) {
			return result;
		}

		// Both endpoints must be connectable surface or medium vertices
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

		const bool lightIsMediumNM = (lightEnd.type == BDPTVertex::MEDIUM);
		const bool eyeIsMediumNM = (eyeEnd.type == BDPTVertex::MEDIUM);

		Vector3 dConnect = Vector3Ops::mkVector3( lightEnd.position, eyeEnd.position );
		const Scalar dist = Vector3Ops::Magnitude( dConnect );
		if( dist < BDPT_RAY_EPSILON ) {
			return result;
		}
		dConnect = dConnect * (1.0 / dist);

		if( !IsVisible( caster, eyeEnd.position, lightEnd.position ) ) {
			return result;
		}

		Vector3 wiAtLight = Vector3Ops::mkVector3(
			lightVerts[s - 2].position, lightEnd.position );
		wiAtLight = Vector3Ops::Normalize( wiAtLight );
		const Vector3 woAtLight = -dConnect;

		const Scalar fLightNM = EvalBSDFAtVertexNM( lightEnd, wiAtLight, woAtLight, nm );
		if( fLightNM <= 0 ) {
			return result;
		}

		Vector3 woAtEye = Vector3Ops::mkVector3(
			eyeVerts[t - 2].position, eyeEnd.position );
		woAtEye = Vector3Ops::Normalize( woAtEye );
		const Vector3 wiAtEye = dConnect;

		const Scalar fEyeNM = EvalBSDFAtVertexNM( eyeEnd, wiAtEye, woAtEye, nm );
		if( fEyeNM <= 0 ) {
			return result;
		}

		// Medium-aware geometric term
		Scalar G;
		if( lightIsMediumNM && eyeIsMediumNM ) {
			G = BDPTUtilities::GeometricTermMediumMedium(
				lightEnd.position, eyeEnd.position );
		} else if( lightIsMediumNM ) {
			G = BDPTUtilities::GeometricTermSurfaceMedium( eyeEnd.position, eyeEnd.geomNormal, lightEnd.position );
		} else if( eyeIsMediumNM ) {
			G = BDPTUtilities::GeometricTermSurfaceMedium( lightEnd.position, lightEnd.geomNormal, eyeEnd.position );
		} else {
			G = BDPTUtilities::GeometricTerm( lightEnd.position, lightEnd.geomNormal, eyeEnd.position, eyeEnd.geomNormal );
		}

		if( G <= 0 ) {
			return result;
		}

		// Connection transmittance (NM)
		const Scalar Tr_connNM = EvalConnectionTransmittanceNM(
			eyeEnd.position, lightEnd.position, scene, caster, nm,
			eyeEnd.pMediumObject, eyeEnd.pMediumVol );

		result.contribution = lightEnd.throughputNM * fLightNM * G * Tr_connNM * fEyeNM * eyeEnd.throughputNM;
		result.needsSplat = false;
		result.valid = true;

		// --- Update pdfRev at connection vertices for correct MIS ---
		const Scalar distSq_conn = dist * dist;

		const Scalar savedLightPdfRev = lightEnd.pdfRev;
		const Scalar savedEyePdfRev = eyeEnd.pdfRev;

		{
			const Scalar pdfRevSA = EvalPdfAtVertexNM( eyeEnd, woAtEye, dConnect, nm );
			if( lightIsMediumNM ) {
				const_cast<BDPTVertex&>( lightEnd ).pdfRev =
					BDPTUtilities::SolidAngleToAreaMedium( pdfRevSA, lightEnd.sigma_t_scalar, distSq_conn );
			} else {
				const Scalar absCosAtLight = fabs( Vector3Ops::Dot( lightEnd.geomNormal, dConnect ) );
				const_cast<BDPTVertex&>( lightEnd ).pdfRev =
					BDPTUtilities::SolidAngleToArea( pdfRevSA, absCosAtLight, distSq_conn );
			}
		}

		{
			const Scalar pdfRevSA = EvalPdfAtVertexNM( lightEnd, wiAtLight, -dConnect, nm );
			if( eyeIsMediumNM ) {
				const_cast<BDPTVertex&>( eyeEnd ).pdfRev =
					BDPTUtilities::SolidAngleToAreaMedium( pdfRevSA, eyeEnd.sigma_t_scalar, distSq_conn );
			} else {
				const Scalar absCosAtEye = fabs( Vector3Ops::Dot( eyeEnd.geomNormal, dConnect ) );
				const_cast<BDPTVertex&>( eyeEnd ).pdfRev =
					BDPTUtilities::SolidAngleToArea( pdfRevSA, absCosAtEye, distSq_conn );
			}
		}

		// --- Update predecessor pdfRev at lightVerts[s-2] ---
		Scalar savedLightPredPdfRevNM = 0;
		const bool hasLightPredNM = (s >= 2);
		if( hasLightPredNM )
		{
			const BDPTVertex& lightPred = lightVerts[s - 2];
			savedLightPredPdfRevNM = lightPred.pdfRev;

			const Scalar pdfPredSA = EvalPdfAtVertexNM( lightEnd, woAtLight, wiAtLight, nm );
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
		Scalar savedEyePredPdfRevNM = 0;
		const bool hasEyePredNM = (t >= 2);
		if( hasEyePredNM )
		{
			const BDPTVertex& eyePred = eyeVerts[t - 2];
			savedEyePredPdfRevNM = eyePred.pdfRev;

			const Scalar pdfPredSA = EvalPdfAtVertexNM( eyeEnd, wiAtEye, woAtEye, nm );
			const Vector3 dToPred = Vector3Ops::mkVector3( eyePred.position, eyeEnd.position );
			const Scalar distPredSq = Vector3Ops::SquaredModulus( dToPred );
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

		result.misWeight = MISWeight( lightVerts, eyeVerts, s, t );

		const_cast<BDPTVertex&>( lightEnd ).pdfRev = savedLightPdfRev;
		const_cast<BDPTVertex&>( eyeEnd ).pdfRev = savedEyePdfRev;
		if( hasLightPredNM ) {
			const_cast<BDPTVertex&>( lightVerts[s - 2] ).pdfRev = savedLightPredPdfRevNM;
		}
		if( hasEyePredNM ) {
			const_cast<BDPTVertex&>( eyeVerts[t - 2] ).pdfRev = savedEyePredPdfRevNM;
		}

		return result;
	}
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
	const Scalar nm
	) const
{
	const unsigned int nLight = static_cast<unsigned int>( lightVerts.size() );
	const unsigned int nEye = static_cast<unsigned int>( eyeVerts.size() );

	std::vector<ConnectionResultNM> results;
	results.reserve( (nLight + 1) * (nEye + 1) );

	for( unsigned int t = 1; t <= nEye; t++ )
	{
		for( unsigned int s = 0; s <= nLight; s++ )
		{
			if( s + t < 2 ) {
				continue;
			}

			ConnectionResultNM cr = ConnectAndEvaluateNM(
				lightVerts, eyeVerts, s, t, scene, caster, camera, nm );

			if( cr.valid ) {
				results.push_back( cr );
			}
		}
	}

	// ----------------------------------------------------------------
	// Deterministic evaluation of zero-exitance lights (spectral).
	// Same logic as the RGB path; converts RISEPel to scalar via
	// luminance, matching LightSampler::EvaluateDirectLightingNM.
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

					if( eyeEnd.type != BDPTVertex::SURFACE ) continue;
					if( !eyeEnd.isConnectible ) continue;
					if( !eyeEnd.pMaterial ) continue;

					const IBSDF* pBSDF = eyeEnd.pMaterial->GetBSDF();
					if( !pBSDF ) continue;

					Vector3 wo = Vector3Ops::mkVector3(
						eyeVerts[t - 2].position, eyeEnd.position );
					wo = Vector3Ops::Normalize( wo );

					Ray evalRay( eyeEnd.position, -wo );
					RayIntersectionGeometric ri( evalRay, nullRasterizerState );
					PathVertexEval::PopulateRIGFromVertex( eyeEnd, ri );

					const bool bReceivesShadows = eyeEnd.pObject
						? eyeEnd.pObject->DoesReceiveShadows() : true;

					RISEPel amount( 0, 0, 0 );
					l->ComputeDirectLighting( ri, caster, *pBSDF,
						bReceivesShadows, amount );

					const Scalar luminance = ColorMath::Luminance( amount );
					if( luminance > 0 )
					{
						ConnectionResultNM cr;
						cr.contribution = eyeEnd.throughputNM * luminance;
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

#ifdef RISE_ENABLE_OPENPGL
	if( pLightGuidingField && pLightGuidingField->IsCollectingTrainingSamples() ) {
		RecordGuidingTrainingLightPath( pLightGuidingField, lightVerts, maxLightGuidingDepth );
	}
#endif

	return results;
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
				const Scalar heroLe = EvalEmitterRadianceNM( v, outDir, heroNM );
				const Scalar compLe = EvalEmitterRadianceNM( v, outDir, companionNM );
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
			v.type == BDPTVertex::SURFACE && !v.isDelta &&
			v.pMaterial && v.pMaterial->GetBSDF() )
		{
			const Vector3 dirFromPrev = Vector3Ops::Normalize(
				Vector3Ops::mkVector3( v.position, verts[i-1].position ) );
			const Vector3 dirToNext = Vector3Ops::Normalize(
				Vector3Ops::mkVector3( verts[i+1].position, v.position ) );

			// Light subpath: wi = toward light (prev), wo = toward eye (next)
			// Eye subpath:   wi = toward light (next), wo = toward eye (prev)
			Vector3 wi, wo;
			if( isLightPath ) {
				wi = dirFromPrev;
				wo = dirToNext;
			} else {
				wi = dirToNext;
				wo = dirFromPrev;
			}

			const Scalar heroF = EvalBSDFAtVertexNM( v, wi, wo, heroNM );
			const Scalar compF = EvalBSDFAtVertexNM( v, wi, wo, companionNM );

			if( heroF > NEARZERO ) {
				cumulativeRatio *= compF / heroF;
			} else {
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
		rig.ptIntersection = v.position;
		rig.vNormal = v.normal;
		rig.vGeomNormal = v.geomNormal;
		rig.onb = v.onb;

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
