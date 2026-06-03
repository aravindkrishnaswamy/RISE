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
#include "../Utilities/Color/SpectralValueTraits.h"
#include "../Utilities/PathValueOps.h"

using namespace RISE;
using namespace RISE::Implementation;
using RISE::SpectralDispatch::PelTag;
using RISE::SpectralDispatch::NMTag;
using RISE::SpectralDispatch::SpectralValueTraits;

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
				const Scalar ndotd = Vector3Ops::Dot( ri.geometric.vGeomNormal, d );
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
		const SampledWavelengths* pSwlHWSS )
	{
		typedef SpectralValueTraits<Tag> Traits;
		typedef typename Traits::value_type V;
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
		// NM-only: the spectral eye subpath caps SURFACE vertices at
		// maxEyeDepth explicitly (the Pel path relies on the loop bound
		// only) -- a preserved Pel/NM asymmetry, NOT fixed here.
		[[maybe_unused]] unsigned int eyeSurfaceBounces = 0;

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
					const Scalar maxDist = ri.geometric.bHit ? ri.geometric.range : RISE_INFINITY;
					bool scattered = false;
					const Scalar t_m = SampleMediumDistance<Tag>(
						*pMed, currentRay, maxDist, sampler, scattered, tag );

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
					else if( ri.geometric.bHit )
					{
						// No scatter event: apply transmittance to throughput.
						// The ray passes through the medium to the surface.
						const V Tr = EvalMediumTransmittance<Tag>( *pMed, currentRay, ri.geometric.range, tag );
						beta = beta * Tr;
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
					// Apply the residual medium transmittance along the escape
					// segment before the synthetic env vertex stores beta —
					// mirrors the surface-hit branch above (line ~2655) and the
					// PT escape fix (PBRT-v4 beta *= T_maj before the
					// infinite-light contribution).  maxDist = RISE_INFINITY
					// matches PT and the env-NEE convention (see ConnectAndEvaluate
					// s=1 comment ~line 3520) exactly so a global medium evaporates
					// identically across integrators: for an UNBOUNDED medium with
					// tiny σ_t a finite cap like 1e10 leaves exp(-σ_t·1e10) ≈ 1
					// (env leaks through where it must be fully attenuated), whereas
					// exp(-σ_t·∞) → 0 for any σ_t > 0.  A bounded (AABB) medium
					// clips internally to a finite Tr regardless of the cap.  VCM's
					// s=0 env-escape consumes this same throughput via the shared
					// generator, so this is the single fix point for PT-reference /
					// BDPT / VCM s=0 consistency.
					V betaEsc = beta;
					if( pMed_eye ) {
						betaEsc = betaEsc * EvalMediumTransmittance<Tag>( *pMed_eye, currentRay, RISE_INFINITY, tag );
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

			// NM-only surface-bounce cap: the spectral eye subpath bounds SURFACE
			// vertices to maxEyeDepth (Pel relies on the loop bound only).
			// Preserved Pel/NM asymmetry, NOT fixed here.
			if constexpr( Traits::is_nm ) {
				if( eyeSurfaceBounces >= maxEyeDepth ) {
					break;
				}
				eyeSurfaceBounces++;
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
				vertices.push_back( v );
				break;
			}

			vertices.push_back( v );

			//
			// Sample the SPF for the next direction
			//
			ScatteredRayContainer scattered;
			ScatterSPF<Tag>( *pSPF, ri.geometric, sampler, scattered, iorStack, tag );

			if( scattered.Count() == 0 ) {
				break;
			}

			// Stochastic single-lobe selection (no path-tree branching).
			// Consume one sampler dimension for Sobol alignment.
			const Scalar lobeSelectXi = sampler.Get1D();
			const ScatteredRay* pScat;
			Scalar selectProb = 1.0;

			{
				pScat = scattered.RandomlySelect( lobeSelectXi, Traits::is_nm );
				if( !pScat ) {
					break;
				}
				if( scattered.Count() > 1 ) {
					Scalar totalKray = 0;
					for( unsigned int i = 0; i < scattered.Count(); i++ ) {
						totalKray += Traits::max_value( KrayValue<Tag>( scattered[i] ) );
					}
					const Scalar selectedKray = Traits::max_value( KrayValue<Tag>( *pScat ) );
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
							ri.geometric, ri.pObject, ri.pMaterial, sampler, NmOrZero<Tag>( tag ) );

						if( bssrdf.valid )
						{
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
						const Scalar cosInGeom = Vector3Ops::Dot( ri.geometric.vGeomNormal, wo_bss );
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
							pRW->g, pRW->ior, pRW->maxBounces, rwSampler, NmOrZero<Tag>( tag ), pRW->maxDepth );

						if( bssrdf.valid )
						{
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
			V guidedF = Traits::zero();
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
						PathTransportUtilities::GuidingRISCandidate<V> candidates[2];

						// Candidate 0: BSDF sample
						{
							PathTransportUtilities::GuidingRISCandidate<V>& c = candidates[0];
							c.direction = pScat->ray.Dir();
							c.bsdfEval = PathValueOps::EvalBSDFAtVertex<Tag>(
								vertices.back(), c.direction, -currentRay.Dir(), tag );
							c.bsdfPdf = pScat->pdf;
							c.guidePdf = pGuidingField->Pdf( guideDist, c.direction );
							c.incomingRadPdf = pGuidingField->IncomingRadiancePdf( guideDist, c.direction );
							c.cosTheta = fabs( Vector3Ops::Dot( c.direction, v.normal ) );
							const Scalar avgBsdf = Traits::max_value( c.bsdfEval );
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
							PathTransportUtilities::GuidingRISCandidate<V>& c = candidates[1];
							Scalar gPdf = 0;
							const Point2 xi2d( sampler.Get1D(), sampler.Get1D() );
							c.direction = pGuidingField->Sample( guideDist, xi2d, gPdf );
							c.guidePdf = gPdf;

							if( gPdf > NEARZERO )
							{
								c.bsdfEval = PathValueOps::EvalBSDFAtVertex<Tag>(
									vertices.back(), c.direction, -currentRay.Dir(), tag );
								c.bsdfPdf = PathValueOps::EvalPdfAtVertex<Tag>(
									vertices.back(), c.direction, -currentRay.Dir(), tag );
								c.incomingRadPdf = pGuidingField->IncomingRadiancePdf( guideDist, c.direction );
								c.cosTheta = fabs( Vector3Ops::Dot( c.direction, v.normal ) );
								const Scalar avgBsdf = Traits::max_value( c.bsdfEval );
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
								guidedF = PathValueOps::EvalBSDFAtVertex<Tag>(
									vertices.back(), gDir, -currentRay.Dir(), tag );
								const Scalar bsdfPdf = PathValueOps::EvalPdfAtVertex<Tag>(
									vertices.back(), gDir, -currentRay.Dir(), tag );

								const Scalar combinedPdf =
									PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdf, bsdfPdf );

								if( combinedPdf > NEARZERO &&
									PositiveMagnitude<Tag>( guidedF ) > NEARZERO )
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
			if constexpr( Traits::is_nm ) {
				// NM-only inline guiding-training sample.  The Pel path trains
				// from connection results in a post-pass instead -- preserved
				// Pel/NM asymmetry.
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
			if( pScat->isDelta ) {
				localScatteringWeight =
					KrayValue<Tag>( *pScat ) * (bssrdfReflectCompensation / selectProb);
				beta = beta * localScatteringWeight;
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
				f = usedGuidedDirection ? guidedF :
					PathValueOps::EvalBSDFAtVertex<Tag>( vertices.back(), scatDir, -currentRay.Dir(), tag );
	#else
				f = PathValueOps::EvalBSDFAtVertex<Tag>( vertices.back(), scatDir, -currentRay.Dir(), tag );
	#endif
				const Scalar cosTheta = fabs( Vector3Ops::Dot(
					scatDir, ri.geometric.vNormal ) );

				if( PositiveMagnitude<Tag>( f ) <= 0 ) {
					break;
				}
				const Scalar invScale = bssrdfReflectCompensation * cosTheta / scatterPdf;
				localScatteringWeight = f * invScale;
				beta = beta * localScatteringWeight;
				if constexpr( Traits::is_nm ) {
					if( pSwlHWSS ) {
						hwssBetaNM[0] = beta;
						for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
							if( pSwlHWSS->terminated[w] ) continue;
							const Scalar fw = PathVertexEval::EvalBSDFAtVertexNM(
								vertices.back(), scatDir, -currentRay.Dir(), pSwlHWSS->lambda[w] );
							hwssBetaNM[w] = hwssBetaNM[w] * fw * invScale;
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
					(pScat->ior_stack && pScat->ior_stack->top() > NEARZERO) ?
						pScat->ior_stack->top() :
						(vertices.back().mediumIOR > NEARZERO ? vertices.back().mediumIOR : 1.0);
				vertices.back().guidingRoughness = pScat->isDelta ?
					Scalar( 0.0 ) :
					(pScat->type == ScatteredRay::eRayDiffuse ? Scalar( 1.0 ) : Scalar( 0.5 ));
			}
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
				const Scalar revPdfSA = PathValueOps::EvalPdfAtVertex<Tag>(
					curr,
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
} // anonymous namespace (GenerateEyeSubpath F2a)

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
	return GenerateEyeSubpathImpl<PelTag>(
		maxEyeDepth, stabilityConfig, pLightSampler,
#ifdef RISE_ENABLE_OPENPGL
		pGuidingField, maxGuidingDepth, guidingAlpha, guidingSamplingType,
#endif
		rc, cameraRay, screenPos, scene, caster, sampler,
		vertices, subpathStarts, PelTag{}, 0 );
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
	if constexpr( SpectralValueTraits<Tag>::is_pel ) {
		(void)tag;
		return pEmitter->emittedRadiance( rig, dir, vertex.geomNormal );
	} else {
		return pEmitter->emittedRadianceNM( rig, dir, vertex.geomNormal, tag.nm );
	}
}

// ILight (point/spot/...) radiance toward `dir`.  ILight has no NM virtual,
// so the NM path projects RGB->scalar via Rec.709 luminance -- the same
// pattern as VCM's EvalLightRadiance<Tag> and the original
// ConnectAndEvaluateNM s=1 / t=1 branches.
template<class Tag>
typename SpectralValueTraits<Tag>::value_type
LightRadiance( const ILight* pLight, const Vector3& dir, Tag tag )
{
	(void)tag;
	if constexpr( SpectralValueTraits<Tag>::is_pel ) {
		return pLight->emittedRadiance( dir );
	} else {
		const RISEPel Le = pLight->emittedRadiance( dir );
		return 0.2126 * Le[0] + 0.7152 * Le[1] + 0.0722 * Le[2];
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
			const RISEPel Le = eyeEnd.pLight->emittedRadiance( woFromEmitter );
			return 0.2126 * Le[0] + 0.7152 * Le[1] + 0.0722 * Le[2];
		}
		if( !eyeEnd.pMaterial ) {
			return 0;
		}
		const IEmitter* pEm = eyeEnd.pMaterial->GetEmitter();
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

		result.misWeight = self.MISWeight( lightVerts, eyeVerts, s, t );

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

		// Project the light vertex position onto the camera
		Point2 rasterPos;
		if( !BDPTCameraUtilities::Rasterize( camera, lightEnd.position, rasterPos ) ) {
			return result;
		}

		// Check visibility from camera to light vertex using standard shadow ray.
		const Point3 camPos = camera.GetLocation();
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
				fLight = PathValueOps::EvalBSDFAtVertex<Tag>( lightEnd, wiAtLight, dirToCam, tag );
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

		Point2 rasterPos;
		if( !BDPTCameraUtilities::Rasterize( camera, lightEnd.position, rasterPos ) ) {
			return result;
		}

		const Point3 camPos = camera.GetLocation();
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
			fLightNM = PathValueOps::EvalBSDFAtVertex<Tag>( lightEnd, wiAtLight, dirToCam, tag );
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
		const V fEye = PathValueOps::EvalBSDFAtVertex<Tag>( eyeEnd, wiForLight, woAtEye, tag );

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
				// Mesh luminary: cosine-weighted hemisphere emission (one-sided)
				const Scalar cosAtLight = Vector3Ops::Dot( lightStart.geomNormal, -dirToLight );
				emissionPdfDir = (cosAtLight > 0) ? (cosAtLight * INV_PI) : 0;
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
				fLight = PathValueOps::EvalBSDFAtVertex<Tag>( lightEnd, wiAtLight, dirToCam, tag );
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
					const Scalar cosEmit = Vector3Ops::Dot( lightEnd.geomNormal, dirToCam );
					emPdfDir = (cosEmit > 0) ? (cosEmit * INV_PI) : 0;
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

		const V fLight = PathValueOps::EvalBSDFAtVertex<Tag>( lightEnd, wiAtLight, woAtLight, tag );

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

		const V fEye = PathValueOps::EvalBSDFAtVertex<Tag>( eyeEnd, wiAtEye, woAtEye, tag );

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

}  // anonymous namespace (ConnectAndEvaluate F3a)

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
	return ConnectAndEvaluateImpl<PelTag>(
		*this, pLightSampler, lightVerts, eyeVerts, s, t, scene, caster, camera, PelTag{} );
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
	Tag tag )
{
	if constexpr( SpectralValueTraits<Tag>::is_pel ) {
		(void)tag;
		return self.ConnectAndEvaluate( lightVerts, eyeVerts, s, t, scene, caster, camera );
	} else {
		return self.ConnectAndEvaluateNM( lightVerts, eyeVerts, s, t, scene, caster, camera, tag.nm );
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

				CR cr = DispatchConnectAndEvaluate<Tag>(
					self, lightVerts, eyeVerts, s, t, scene, caster, camera, tag );
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

					if constexpr( Traits::is_pel ) {
					RISEPel amount( 0, 0, 0 );
					l->ComputeDirectLighting( ri, caster, *pBSDF,
						bReceivesShadows, amount );

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
							ri, caster, *pBSDF, bReceivesShadows, tag.nm );
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
	ISampler* pSampler
	) const
{
	return EvaluateAllStrategiesImpl<PelTag>(
		*this, lightVerts, eyeVerts, scene, caster, camera, pSampler,
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
				OrthonormalBasis3D onb;
				onb.CreateFromW( ls.normal );
				rig.onb = onb;
				LeNM = pEmitter->emittedRadianceNM( rig, ls.direction, ls.normal, tag.nm );
			}
		} else if( ls.pLight ) {
			LeNM = 0.2126 * ls.Le[0] + 0.7152 * ls.Le[1] + 0.0722 * ls.Le[2];
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
		// Env-light vertex 0 has pLight == pLuminary == NULL; the
		// downstream MIS dispatch sites recognise the env-vertex via
		// pEnvLight != NULL and recover Le via GetRadiance(skyProbe).
		// Without this, env-IBL scenes lose every BDPT connection
		// strategy that touches a light vertex.
		v.pEnvLight = ls.pEnvLight;
		v.isDelta = ls.isDelta;
		v.isConnectible = !ls.isDelta;

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
						OrthonormalBasis3D onbW;
						onbW.CreateFromW( ls.normal );
						rigW.onb = onbW;
						LeW = pEm->emittedRadianceNM(
							rigW, ls.direction, ls.normal, pSwlHWSS->lambda[w] );
					}
				} else if( ls.pLight ) {
					LeW = 0.2126 * ls.Le[0] + 0.7152 * ls.Le[1] + 0.0722 * ls.Le[2];
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
				const Scalar maxDist = ri.geometric.bHit ? ri.geometric.range : RISE_INFINITY;
				bool scattered = false;
				const Scalar t_m = SampleMediumDistance<Tag>(
					*pMed, currentRay, maxDist, sampler, scattered, tag );

				if( scattered && volumeBounces < stabilityConfig.maxVolumeBounce )
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
					StoreThroughput<Tag>( mv, beta );

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
				else if( ri.geometric.bHit )
				{
					const V Tr = EvalMediumTransmittance<Tag>(
						*pMed, currentRay, ri.geometric.range, tag );
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
		v.ptCoord1 = ri.geometric.ptCoord1;
		v.bHasTexCoord1 = ri.geometric.bHasTexCoord1;
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
		ScatterSPF<Tag>( *pSPF, ri.geometric, sampler, scattered, iorStack, tag );

		if( scattered.Count() == 0 ) {
			break;
		}

		// Stochastic single-lobe selection (no path-tree branching).
		// Consume one sampler dimension for Sobol alignment.
		const Scalar lobeSelectXi = sampler.Get1D();
		const ScatteredRay* pScat;
		Scalar selectProb = 1.0;

		{
			pScat = scattered.RandomlySelect( lobeSelectXi, Traits::is_nm );
			if( !pScat ) {
				break;
			}
			if( scattered.Count() > 1 ) {
				Scalar totalKray = 0;
				for( unsigned int i = 0; i < scattered.Count(); i++ ) {
					totalKray += Traits::max_value( KrayValue<Tag>( scattered[i] ) );
				}
				const Scalar selectedKray = Traits::max_value( KrayValue<Tag>( *pScat ) );
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
						StoreThroughput<Tag>( entryV, betaSpatial );
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
					const Scalar cosInGeom = Vector3Ops::Dot( ri.geometric.vGeomNormal, wo_bss );
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
						StoreThroughput<Tag>( entryV, betaSpatial );
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
		V guidedF = Traits::zero();
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
					PathTransportUtilities::GuidingRISCandidate<V> candidates[2];

					// Candidate 0: BSDF sample
					{
						PathTransportUtilities::GuidingRISCandidate<V>& c = candidates[0];
						c.direction = pScat->ray.Dir();
						c.bsdfEval = PathValueOps::EvalBSDFAtVertex<Tag>(
							vertices.back(), -currentRay.Dir(), c.direction, tag );
						c.bsdfPdf = pScat->pdf;
						c.guidePdf = pLightGuidingField->Pdf( lightGuideDist, c.direction );
						c.incomingRadPdf = pLightGuidingField->IncomingRadiancePdf( lightGuideDist, c.direction );
						c.cosTheta = fabs( Vector3Ops::Dot( c.direction, v.normal ) );
						const Scalar avgBsdf = Traits::max_value( c.bsdfEval );
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
						PathTransportUtilities::GuidingRISCandidate<V>& c = candidates[1];
						Scalar gPdf = 0;
						const Point2 xi2d( sampler.Get1D(), sampler.Get1D() );
						c.direction = pLightGuidingField->Sample( lightGuideDist, xi2d, gPdf );
						c.guidePdf = gPdf;

						if( gPdf > NEARZERO )
						{
							c.bsdfEval = PathValueOps::EvalBSDFAtVertex<Tag>(
								vertices.back(), -currentRay.Dir(), c.direction, tag );
							c.bsdfPdf = PathValueOps::EvalPdfAtVertex<Tag>(
								vertices.back(), -currentRay.Dir(), c.direction, tag );
							c.incomingRadPdf = pLightGuidingField->IncomingRadiancePdf( lightGuideDist, c.direction );
							c.cosTheta = fabs( Vector3Ops::Dot( c.direction, v.normal ) );
							const Scalar avgBsdf = Traits::max_value( c.bsdfEval );
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
							guidedF = PathValueOps::EvalBSDFAtVertex<Tag>(
								vertices.back(), -currentRay.Dir(), gDir, tag );
							const Scalar bsdfPdf = PathValueOps::EvalPdfAtVertex<Tag>(
								vertices.back(), -currentRay.Dir(), gDir, tag );

							const Scalar combinedPdf =
								PathTransportUtilities::GuidingCombinedPdf( alpha, guidePdf, bsdfPdf );

							if( combinedPdf > NEARZERO &&
								Traits::max_value( guidedF ) > NEARZERO )	// magnitude (light-NM orig used fabs); NOT PositiveMagnitude -- cf. the eye subpath's NM gate which is bare
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
		if( pScat->isDelta ) {
			// For delta scattering, kray already incorporates the right factor
			// but must be divided by the lobe selection probability.
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

			if( PositiveMagnitude<Tag>( f ) <= 0 ) {
				break;
			}
			const Scalar scatterPdf = selectProb * effectivePdf;
			if constexpr( Traits::is_pel ) {
				localScatteringWeight =
					f * (bssrdfReflectCompensation * cosTheta / scatterPdf);
				beta = beta * localScatteringWeight;
			} else {
				localScatteringWeight = RISEPel(
					f * bssrdfReflectCompensation * cosTheta / scatterPdf,
					f * bssrdfReflectCompensation * cosTheta / scatterPdf,
					f * bssrdfReflectCompensation * cosTheta / scatterPdf );
				beta = beta * f * bssrdfReflectCompensation * cosTheta / scatterPdf;
				if( pSwlHWSS ) {
					const Scalar invScale = bssrdfReflectCompensation * cosTheta / scatterPdf;
					hwssBetaNM[0] = beta;
					for( unsigned int w = 1; w < SampledWavelengths::N; w++ ) {
						if( pSwlHWSS->terminated[w] ) continue;
						const Scalar fw = PathVertexEval::EvalBSDFAtVertexNM(
							vertices.back(), -currentRay.Dir(), scatDir, pSwlHWSS->lambda[w] );
						hwssBetaNM[w] = hwssBetaNM[w] * fw * invScale;
					}
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
			const Scalar revPdfSA = PathValueOps::EvalPdfAtVertex<Tag>(
				curr,
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
	const SampledWavelengths* pSwlHWSS
	) const
{
	return GenerateEyeSubpathImpl<NMTag>(
		maxEyeDepth, stabilityConfig, pLightSampler,
#ifdef RISE_ENABLE_OPENPGL
		pGuidingField, maxGuidingDepth, guidingAlpha, guidingSamplingType,
#endif
		rc, cameraRay, screenPos, scene, caster, sampler,
		vertices, subpathStarts, NMTag( nm ), pSwlHWSS );
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
	return ConnectAndEvaluateImpl<NMTag>(
		*this, pLightSampler, lightVerts, eyeVerts, s, t, scene, caster, camera, NMTag( nm ) );
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
	return EvaluateAllStrategiesImpl<NMTag>(
		*this, lightVerts, eyeVerts, scene, caster, camera, nullptr,
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
