//////////////////////////////////////////////////////////////////////
//
//  VCMRasterizerBase.cpp - Implementation of the VCM rasterizer
//    base class.
//
//    Step 6 drops the VCM light pass into PreRenderSetup.  The
//    call order inside PixelBasedRasterizerHelper::RasterizeScene is:
//
//      1. Create image + scratch
//      2. pCaster->AttachScene( scene )
//      3. pScene.GetObjects()->PrepareForRendering()
//      4. PreRenderSetup( scene, rect )        <-- we hook here
//      5. Block-dispatch loop calling IntegratePixel per pixel
//         (this is the VCM eye pass; IntegratePixel queries the
//         LightVertexStore populated in step 4)
//      6. Resolve / flush outputs
//
//    Step 6 keeps the eye pass as the Step 0 solid-color placeholder
//    so we can validate:
//      - Light subpaths generate without crashing
//      - Post-walk produces LightVertex records
//      - Store accepts them and builds its KD-tree
//      - Memory stays within the plan's budget
//
//    Step 7 onward fills in IntegratePixel with the real VC/VM
//    evaluation that reads the store.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 14, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "VCMRasterizerBase.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/ICamera.h"
#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/IRasterImage.h"
#include "../Utilities/RuntimeContext.h"
#include "../Utilities/SobolSampler.h"
#include "../Utilities/Threads/Threads.h"
#include "../Utilities/ThreadPool.h"
#include "AdaptiveTileSizer.h"
#include "../Lights/LightSampler.h"
#include "../Shaders/BDPTIntegrator.h"
#include "../Shaders/BDPTVertex.h"
#include "../RasterImages/RasterImage.h"

#include <atomic>
#include <vector>

using namespace RISE;
using namespace RISE::Implementation;

VCMRasterizerBase::VCMRasterizerBase(
	IRayCaster* pCaster_,
	const unsigned int maxEyeDepth,
	const unsigned int maxLightDepth,
	const Scalar mergeRadius,
	const bool enableVC,
	const bool enableVM,
	const StabilityConfig& stabilityCfg
	) :
	PixelBasedRasterizerHelper( pCaster_ ),
	pIntegrator( 0 ),
	pSplatFilm( 0 ),
	pLightVertexStore( 0 ),
	pScratchImage( 0 ),
	mSplatTotalSamples( 1.0 ),
	mTotalAdaptiveSamples( 0 ),
	stabilityConfig( stabilityCfg )
{
	pIntegrator = new VCMIntegrator(
		maxEyeDepth,
		maxLightDepth,
		mergeRadius,
		enableVC,
		enableVM,
		stabilityCfg );
	pLightVertexStore = new LightVertexStore();
}

VCMRasterizerBase::~VCMRasterizerBase()
{
	safe_release( pIntegrator );
	safe_release( pSplatFilm );
	delete pLightVertexStore;
	pLightVertexStore = 0;
	safe_release( pScratchImage );
}

void VCMRasterizerBase::AddAdaptiveSamples( uint64_t count ) const
{
	mTotalAdaptiveSamples.fetch_add( count, std::memory_order_relaxed );
}

Scalar VCMRasterizerBase::GetEffectiveSplatSPP( unsigned int width, unsigned int height ) const
{
	const uint64_t totalSamples = mTotalAdaptiveSamples.load( std::memory_order_relaxed );
	if( totalSamples > 0 && width > 0 && height > 0 ) {
		const Scalar avgSPP = static_cast<Scalar>( totalSamples ) / static_cast<Scalar>( width * height );
		return avgSPP * GetSplatSampleScale();
	}
	return mSplatTotalSamples;
}

namespace
{
	//////////////////////////////////////////////////////////////////////
	// LightPassDispatcher — parallel dispatcher for VCM light-pass
	// subpath generation + conversion.
	//
	// Pixels are divided into blocks; each worker thread pulls blocks
	// from an atomic counter, generates subpaths with its own
	// RuntimeContext and SobolSampler, and accumulates converted
	// LightVertex records into a thread-local buffer.  After all
	// threads join, the main thread concatenates the per-thread
	// buffers into the shared store in deterministic index order.
	//////////////////////////////////////////////////////////////////////
	struct LightPassDispatcher
	{
		const IScene&			scene;
		const IRayCaster&		caster;
		BDPTIntegrator*			pGen;
		const VCMNormalization&	norm;
		const unsigned int		width;
		const unsigned int		height;
		const unsigned int		maxLightDepth;
		const uint32_t			baseSampleIndex;	// Sobol index base (= passIdx × samplesPerSuperIter)
		const unsigned int		samplesPerSuperIter;	// K — sub-samples within one super-iteration

		std::vector<Rect>		tiles;
		std::atomic<unsigned int>	nextTile;

		// Per-thread output buffers (one per worker).
		std::vector<std::vector<LightVertex>>	perThreadOutput;
		std::vector<unsigned long long>			perThreadPathsShot;

		struct ThreadLocal {
			std::vector<BDPTVertex>			tmpLightVerts;
			std::vector<LightVertex>		tmpConverted;
			std::vector<VCMMisQuantities>	tmpLightMis;
			unsigned long long				pathsShot = 0;
			unsigned long long				storedCount = 0;
		};

		LightPassDispatcher(
			const IScene& scene_,
			const IRayCaster& caster_,
			BDPTIntegrator* pGen_,
			const VCMNormalization& norm_,
			const unsigned int width_,
			const unsigned int height_,
			const unsigned int maxLightDepth_,
			const uint32_t baseSampleIndex_,
			const unsigned int samplesPerSuperIter_,
			const unsigned int numWorkers
			) :
			scene( scene_ ),
			caster( caster_ ),
			pGen( pGen_ ),
			norm( norm_ ),
			width( width_ ),
			height( height_ ),
			maxLightDepth( maxLightDepth_ ),
			baseSampleIndex( baseSampleIndex_ ),
			samplesPerSuperIter( std::max( 1u, samplesPerSuperIter_ ) ),
			nextTile( 0 ),
			perThreadOutput( numWorkers ),
			perThreadPathsShot( numWorkers, 0 )
		{
			// Block the pixel grid into tiles.  Tile size adapts to
			// image × thread count so small scenes / high core counts
			// still decompose into ≥8 tiles per thread.
			const unsigned int TILE = ComputeTileSize(
				width, height, numWorkers, 8, 8, 64 );
			for( unsigned int y = 0; y < height; y += TILE ) {
				for( unsigned int x = 0; x < width; x += TILE ) {
					const unsigned int right  = std::min( x + TILE - 1, width - 1 );
					const unsigned int bottom = std::min( y + TILE - 1, height - 1 );
					// Rect( top, left, bottom, right )
					tiles.push_back( Rect( y, x, bottom, right ) );
				}
			}
		}

		bool GetNextBlock( Rect& rc )
		{
			const unsigned int idx = nextTile.fetch_add( 1, std::memory_order_relaxed );
			if( idx >= tiles.size() ) {
				return false;
			}
			rc = tiles[idx];
			return true;
		}

		void RunWorker( unsigned int workerIdx )
		{
			// Each worker has its own RNG (for RR) and scratch buffers.
			RandomNumberGenerator localRng;
			RuntimeContext rc( localRng, RuntimeContext::PASS_NORMAL, true );

			ThreadLocal tl;
			tl.tmpLightVerts.reserve( maxLightDepth + 1 );
			tl.tmpConverted.reserve( maxLightDepth + 1 );
			tl.tmpLightMis.reserve( maxLightDepth + 1 );

			std::vector<LightVertex>& out = perThreadOutput[workerIdx];
			// Estimate: only a fraction of subpaths deposit a vertex
			// (typically 1-2 non-delta surface hits per subpath), and
			// the store gets at most ~one vertex per subpath on
			// average.  Reserve that much per worker to amortize
			// std::vector growth without over-committing memory.
			const std::size_t pixelsPerWorker =
				( static_cast<std::size_t>( width ) * height +
				  std::max<std::size_t>( 1, perThreadOutput.size() ) - 1 ) /
				std::max<std::size_t>( 1, perThreadOutput.size() );
			const std::size_t estimatedPerWorker =
				pixelsPerWorker * samplesPerSuperIter * 2;	// ~2 verts/subpath avg
			out.reserve( estimatedPerWorker );

			Rect rect( 0, 0, 0, 0 );
			while( GetNextBlock( rect ) )
			{
				for( unsigned int y = rect.top; y <= rect.bottom; y++ )
				{
					for( unsigned int x = rect.left; x <= rect.right; x++ )
					{
						const uint32_t pixelSeed = y * width + x;

						// Generate K sub-samples per pixel for this
						// super-iteration.  Each sub-sample uses a
						// distinct Sobol sample index so photons land
						// at different positions across sub-samples.
						for( unsigned int k = 0; k < samplesPerSuperIter; k++ )
						{
							const uint32_t sampleIdx = baseSampleIndex + k;
							SobolSampler sampler( sampleIdx, pixelSeed );

							tl.tmpLightVerts.clear();
							static thread_local std::vector<uint32_t> tmpLightSubpathStarts;
							// Allow branching on the light store build
							// (pass -1 to use the configured threshold).
							// Per-branch Convert below walks each branch
							// slice independently so dVCM/dVC/dVM
							// recurrences are correct (a single-chain
							// Convert on the concatenated multi-branch
							// array would corrupt seams).  Each non-empty
							// branch counts as its own subpath in
							// pathsShot; the caller renormalizes
							// mLightSubPathCount after the light pass so
							// the VM density estimator matches the
							// actual stored photon / effective-subpath
							// ratio.
							pGen->GenerateLightSubpath( scene, caster, sampler, tl.tmpLightVerts, tmpLightSubpathStarts, rc.random, Scalar( -1 ) );
							if( tl.tmpLightVerts.empty() ) {
								continue;
							}

							const std::size_t numLbPhotonStore = tmpLightSubpathStarts.size() >= 2 ?
								( tmpLightSubpathStarts.size() - 1 ) : 0;
							static thread_local std::vector<BDPTVertex> branchVertsPhotonStore;
							for( std::size_t lb = 0; lb < numLbPhotonStore; lb++ ) {
								const uint32_t lbeg = tmpLightSubpathStarts[lb];
								const uint32_t lend = tmpLightSubpathStarts[lb + 1];
								if( lbeg >= lend ) continue;
								branchVertsPhotonStore.assign(
									tl.tmpLightVerts.begin() + lbeg,
									tl.tmpLightVerts.begin() + lend );
								tl.tmpConverted.clear();
								VCMIntegrator::ConvertLightSubpath(
									branchVertsPhotonStore, norm, tl.tmpConverted, &tl.tmpLightMis );
								for( std::size_t m = 0; m < tl.tmpConverted.size(); m++ ) {
									out.push_back( tl.tmpConverted[m] );
									tl.storedCount++;
								}
								tl.pathsShot++;
							}
						}
					}
				}
			}

			perThreadPathsShot[workerIdx] = tl.pathsShot;
		}
	};

	// Returns total pathsShot across all workers.  After this call,
	// dispatcher.perThreadOutput[i] holds that worker's LightVertex
	// buffer — the caller concatenates them into the shared store in
	// index order for determinism.
	unsigned long long RunLightPassParallel(
		LightPassDispatcher& dispatcher,
		unsigned int numWorkers
		)
	{
		if( numWorkers <= 1 ) {
			// Single-threaded fallback.
			dispatcher.RunWorker( 0 );
			return dispatcher.perThreadPathsShot[0];
		}

		ThreadPool& pool = GlobalThreadPool();
		pool.ParallelFor( numWorkers, [&dispatcher]( unsigned int workerIdx ) {
			dispatcher.RunWorker( workerIdx );
		} );

		unsigned long long totalPaths = 0;
		for( unsigned int i = 0; i < numWorkers; i++ ) {
			totalPaths += dispatcher.perThreadPathsShot[i];
		}
		return totalPaths;
	}
}

//////////////////////////////////////////////////////////////////////
// PreRenderSetup — VCM light pass
//
// The initial light pass is parallelized via LightPassDispatcher.
// The auto-radius pre-pass remains single-threaded (it runs once
// before the store-build and collects segment-length statistics
// into a shared vector; keeping it single-threaded is simpler than
// merging per-thread segLens arrays and this pre-pass is only ~1%
// of total time).
//////////////////////////////////////////////////////////////////////
void VCMRasterizerBase::PreRenderSetup( const IScene& pScene, const Rect* /*pRect*/ ) const
{
	if( !pIntegrator || !pLightVertexStore ) {
		return;
	}

	// Plumb the ray caster's light sampler into the BDPT
	// generator that VCM wraps.  Mirrors what BDPTRasterizerBase
	// does at the top of its RasterizeScene, which never runs for
	// VCM because we inherit PixelBasedRasterizerHelper directly.
	const LightSampler* pLS = pCaster ? pCaster->GetLightSampler() : 0;
	pIntegrator->SetLightSampler( pLS );

	// Force 1 SPP per progressive pass.  Super-iteration batching
	// (K > 1) was tested and found to scale merge evaluation cost
	// by K× because photon density grows K× but merge radius stays
	// fixed.  SPPM-style radius-reduction-per-K would counter this
	// (√K radius shrink keeps candidate count constant) but is a
	// v3 feature.  For v2, keep K=1 matching the original paper.
	if( pIntegrator->GetEnableVM() ) {
		const_cast<VCMRasterizerBase*>(this)->progressiveConfig.samplesPerPass = 1;
		const_cast<VCMRasterizerBase*>(this)->progressiveConfig.enabled = true;
	}

	// Reset the adaptive sample counter for this render.
	mTotalAdaptiveSamples.store( 0, std::memory_order_relaxed );

	const ICamera* pCamera = pScene.GetCamera();
	if( !pCamera ) {
		return;
	}
	const unsigned int width = pCamera->GetWidth();
	const unsigned int height = pCamera->GetHeight();

	// PreRenderSetup runs ONE light pass to populate the photon
	// store used by VM merging.  The t=1 splat strategy is handled
	// per eye sample inside IntegratePixel (mirroring BDPT), so it
	// does NOT use the subpaths generated here — only the merge
	// store does.
	const unsigned int lightSubpathsPerPixel = 1;

	BDPTIntegrator* pGen = pIntegrator->GetGenerator();
	if( !pGen ) {
		return;
	}

	// Splat film for t=1 light-to-camera contributions.  Needed
	// regardless of VM state (VC's t=1 splat goes through here).
	safe_release( pSplatFilm );
	pSplatFilm = new SplatFilm( width, height );

	// VM-disabled: skip the entire light vertex store build.
	// VC's t=1 splats are generated per-sample in IntegratePixel,
	// not from this prepass, so VC-only mode needs no store.
	if( !pIntegrator->GetEnableVM() ) {
		mVCMNormalization = ComputeNormalization(
			width, height,
			Scalar( 0 ),
			pIntegrator->GetEnableVC(),
			false );

		mSplatTotalSamples = 1.0;
		if( pSampling ) {
			mSplatTotalSamples = static_cast<Scalar>( pSampling->GetNumSamples() );
		}
		mSplatTotalSamples *= GetSplatSampleScale();

		GlobalLog()->PrintEx( eLog_Event,
			"VCMRasterizerBase::PreRenderSetup:: VM disabled — "
			"skipping light vertex store build" );
		return;
	}

	pLightVertexStore->Clear();
	pLightVertexStore->Reserve(
		static_cast<std::size_t>( width ) *
		static_cast<std::size_t>( height ) *
		static_cast<std::size_t>( pIntegrator->GetMaxLightDepth() + 1 ) );

	std::vector<BDPTVertex> tmpLightVerts;
	std::vector<LightVertex> tmpConverted;
	std::vector<VCMMisQuantities> tmpLightMis;
	tmpLightVerts.reserve( pIntegrator->GetMaxLightDepth() + 1 );
	tmpConverted.reserve( pIntegrator->GetMaxLightDepth() + 1 );
	tmpLightMis.reserve( pIntegrator->GetMaxLightDepth() + 1 );

	// Single-threaded: one RuntimeContext, reused across all pixels.
	RuntimeContext rc( GlobalRNG(), RuntimeContext::PASS_NORMAL, false );
	PrepareRuntimeContext( rc );

	// Determine the merge radius.  If the user supplied a positive
	// radius, use it directly.  Otherwise run a pre-pass over the
	// light subpaths to derive an auto-radius.
	Scalar effectiveMergeRadius = pIntegrator->GetRequestedMergeRadius();
	if( pIntegrator->GetEnableVM() && effectiveMergeRadius <= 0 )
	{
		std::vector<Scalar> segLens;
		segLens.reserve( static_cast<std::size_t>( width ) *
		                 static_cast<std::size_t>( height ) * 4 );

		bool foundSpecular = false;

		for( unsigned int s = 0; s < lightSubpathsPerPixel; s++ )
		{
			for( unsigned int y = 0; y < height; y++ )
			{
				for( unsigned int x = 0; x < width; x++ )
				{
					const uint32_t pixelSeed = y * width + x;
					const uint32_t sampleIndex = s;
					SobolSampler sampler( sampleIndex, pixelSeed );

					tmpLightVerts.clear();
					static thread_local std::vector<uint32_t> tmpLightSubpathStarts2;
					// Auto-radius pre-pass: use single-branch subpaths so
					// segment-length median isn't skewed by branch-crossing
					// prefix-copy boundaries (which are not real segments).
					pGen->GenerateLightSubpath( pScene, *pCaster, sampler, tmpLightVerts, tmpLightSubpathStarts2, rc.random, Scalar( 1.0 ) );
					for( std::size_t k = 1; k < tmpLightVerts.size(); k++ ) {
						const BDPTVertex& curr = tmpLightVerts[k];
						const BDPTVertex& prevV = tmpLightVerts[k - 1];

						if( curr.isDelta || prevV.isDelta ) {
							foundSpecular = true;
						}

						const bool currStoreable = ( curr.isConnectible && curr.type == BDPTVertex::SURFACE );
						const bool prevStoreable = ( prevV.isConnectible && prevV.type == BDPTVertex::SURFACE );
						if( !currStoreable && !prevStoreable ) {
							continue;
						}
						const Vector3 d = Vector3Ops::mkVector3( curr.position, prevV.position );
						const Scalar len = Vector3Ops::Magnitude( d );
						if( len > 0 ) {
							segLens.push_back( len );
						}
					}
				}
			}
		}

		if( !foundSpecular ) {
			GlobalLog()->PrintEx( eLog_Event,
				"VCMRasterizerBase::PreRenderSetup:: no specular "
				"vertices — disabling VM" );
			effectiveMergeRadius = 0;

			mVCMNormalization = ComputeNormalization(
				width, height, Scalar( 0 ),
				pIntegrator->GetEnableVC(), false );

			mSplatTotalSamples = 1.0;
			if( pSampling ) {
				mSplatTotalSamples = static_cast<Scalar>( pSampling->GetNumSamples() );
			}
			mSplatTotalSamples *= GetSplatSampleScale();

			GlobalLog()->PrintEx( eLog_Event,
				"VCMRasterizerBase::PreRenderSetup:: light pass done — "
				"paths shot=0, light vertices stored=0, kd-tree built=0" );
			return;
		}
		else if( segLens.size() >= 8 ) {
			std::sort( segLens.begin(), segLens.end() );
			const Scalar medianSeg = segLens[segLens.size() / 2];
			effectiveMergeRadius = Scalar( 0.01 ) * medianSeg;

			GlobalLog()->PrintEx( eLog_Event,
				"VCMRasterizerBase::PreRenderSetup:: auto-radius "
				"segments=%zu median_segment=%g effective_radius=%g",
				segLens.size(), (double)medianSeg, (double)effectiveMergeRadius );
		} else {
			GlobalLog()->PrintEx( eLog_Warning,
				"VCMRasterizerBase::PreRenderSetup:: auto-radius "
				"failed — disabling VM" );
			effectiveMergeRadius = 0;
		}
	}

	// K = 1 (samplesPerPass forced to 1 above).  Normalization uses
	// W × H (one light subpath per pixel per iteration).
	const unsigned int samplesPerSuperIter = 1;

	mVCMNormalization = ComputeNormalization(
		width, height,
		effectiveMergeRadius,
		pIntegrator->GetEnableVC(),
		pIntegrator->GetEnableVM() );

	unsigned long long totalStored = 0;
	unsigned long long pathsShot = 0;

	// Parallel light pass — generates K × W × H light subpaths using
	// distinct Sobol sample indices [0 .. K-1] for super-iteration 0.
	// Per-thread local buffers are concat'd into the shared store in
	// deterministic worker-index order.
	{
		const unsigned int numWorkers = std::max( 1, HowManyThreadsToSpawn() );
		LightPassDispatcher dispatcher(
			pScene, *pCaster, pGen, mVCMNormalization,
			width, height, pIntegrator->GetMaxLightDepth(),
			/*baseSampleIndex=*/ 0,
			samplesPerSuperIter,
			numWorkers );

		pathsShot = RunLightPassParallel( dispatcher, numWorkers );

		for( unsigned int i = 0; i < numWorkers; i++ ) {
			const std::vector<LightVertex>& localBuf = dispatcher.perThreadOutput[i];
			totalStored += localBuf.size();
			pLightVertexStore->Concat( std::move( dispatcher.perThreadOutput[i] ) );
		}
	}

	// Renormalize mVCMNormalization with the actual count of subpaths
	// deposited.  Light-subpath branching can produce more than W×H
	// subpaths (each non-empty branch counts once); using the real
	// count keeps the VM density estimator and per-pixel VC MIS
	// weights consistent with the store.  When no branching fires,
	// pathsShot equals the pre-branching W×H total and this re-
	// normalization is a no-op.  The photons' own dVCM/dVC values
	// were Convert'd with the pre-pass normalization; the stored
	// values retain that calibration — same approximation used by
	// the specular-only store filter path.
	if( pathsShot > 0 ) {
		mVCMNormalization = ComputeNormalization(
			width, height, effectiveMergeRadius,
			pIntegrator->GetEnableVC(),
			pIntegrator->GetEnableVM(),
			static_cast<Scalar>( pathsShot ) );
	}

	// Throughput clamping (kept as secondary safeguard).
	// With per-iteration store rebuild the density noise averages
	// out, but extreme throughput outliers can still cause fireflies.
	// to median × 20 to control the worst variance while preserving
	// caustic structure.
	{
		const std::size_t storeSize = pLightVertexStore->Size();
		if( storeSize > 16 ) {
			std::vector<Scalar> throughputLums;
			throughputLums.reserve( storeSize );
			for( std::size_t k = 0; k < storeSize; k++ ) {
				const LightVertex& lv = pLightVertexStore->Get( k );
				throughputLums.push_back( ColorMath::MaxValue( lv.throughput ) );
			}
			std::sort( throughputLums.begin(), throughputLums.end() );
			const Scalar medianThroughput = throughputLums[storeSize / 2];
			const Scalar clampThreshold = medianThroughput * Scalar( 20 );

			unsigned long long clamped = 0;
			if( clampThreshold > 0 ) {
				for( std::size_t k = 0; k < storeSize; k++ ) {
					LightVertex& lv = pLightVertexStore->GetMutable( k );
					const Scalar maxC = ColorMath::MaxValue( lv.throughput );
					if( maxC > clampThreshold ) {
						lv.throughput = lv.throughput * ( clampThreshold / maxC );
						clamped++;
					}
				}
			}

			if( clamped > 0 ) {
				GlobalLog()->PrintEx( eLog_Event,
					"VCMRasterizerBase::PreRenderSetup:: throughput clamp "
					"median=%g threshold=%g clamped=%llu of %zu vertices",
					(double)medianThroughput, (double)clampThreshold,
					clamped, storeSize );
			}
		}
	}

	pLightVertexStore->BuildKDTreeParallel();

	mSplatTotalSamples = 1.0;
	if( pSampling ) {
		mSplatTotalSamples = static_cast<Scalar>( pSampling->GetNumSamples() );
	}
	mSplatTotalSamples *= GetSplatSampleScale();

	GlobalLog()->PrintEx( eLog_Event,
		"VCMRasterizerBase::PreRenderSetup:: light pass done — "
		"paths shot=%llu, light vertices stored=%llu, kd-tree built=%d",
		pathsShot,
		totalStored,
		(int)pLightVertexStore->IsBuilt() );
}

//////////////////////////////////////////////////////////////////////
// OnProgressivePassBegin — rebuild the light vertex store
//
// Each progressive pass gets a fresh set of light subpaths so the
// photon density noise averages across passes instead of persisting
// from a single fixed store.  Uses passIdx as a seed offset so each
// pass generates different photon positions.
//
// Pass 0 is a no-op because PreRenderSetup already built the store.
//////////////////////////////////////////////////////////////////////
void VCMRasterizerBase::OnProgressivePassBegin(
	const IScene& pScene,
	const unsigned int passIdx
	) const
{
	// Pass 0: PreRenderSetup already built the store.
	if( passIdx == 0 ) {
		return;
	}

	// Nothing to rebuild if VM is disabled or the integrator is absent.
	if( !pIntegrator || !pLightVertexStore || !pIntegrator->GetEnableVM() ) {
		return;
	}
	if( mVCMNormalization.mMergeRadiusSq <= 0 ) {
		return;
	}

	BDPTIntegrator* pGen = pIntegrator->GetGenerator();
	if( !pGen ) {
		return;
	}

	const ICamera* pCamera = pScene.GetCamera();
	if( !pCamera ) {
		return;
	}
	const unsigned int width = pCamera->GetWidth();
	const unsigned int height = pCamera->GetHeight();

	pLightVertexStore->Clear();

	// K = 1 matches the forced samplesPerPass = 1 in PreRenderSetup.
	const unsigned int samplesPerSuperIter = 1;
	const uint32_t baseSampleIndex = passIdx;

	unsigned long long totalStored = 0;
	unsigned long long pathsShot = 0;
	{
		const unsigned int numWorkers = std::max( 1, HowManyThreadsToSpawn() );
		LightPassDispatcher dispatcher(
			pScene, *pCaster, pGen, mVCMNormalization,
			width, height, pIntegrator->GetMaxLightDepth(),
			baseSampleIndex,
			samplesPerSuperIter,
			numWorkers );

		pathsShot = RunLightPassParallel( dispatcher, numWorkers );

		for( unsigned int i = 0; i < numWorkers; i++ ) {
			totalStored += dispatcher.perThreadOutput[i].size();
			pLightVertexStore->Concat( std::move( dispatcher.perThreadOutput[i] ) );
		}
	}

	// Renormalize with actual pathsShot — see comment at the matching
	// PreRenderSetup site for rationale.  Branching can produce more
	// than W×H subpaths; the per-pixel VC/VM weights must match.
	if( pathsShot > 0 ) {
		mVCMNormalization = ComputeNormalization(
			width, height, mVCMNormalization.mMergeRadius,
			pIntegrator->GetEnableVC(),
			pIntegrator->GetEnableVM(),
			static_cast<Scalar>( pathsShot ) );
	}

	pLightVertexStore->BuildKDTreeParallel();

	GlobalLog()->PrintEx( eLog_Info,
		"VCMRasterizerBase::OnProgressivePassBegin:: iteration %u — "
		"rebuilt store with %llu light vertices (K=%u)",
		passIdx, totalStored, samplesPerSuperIter );
}

IRasterImage& VCMRasterizerBase::GetIntermediateOutputImage( IRasterImage& primary ) const
{
	if( !pSplatFilm ) {
		return primary;
	}

	const unsigned int w = primary.GetWidth();
	const unsigned int h = primary.GetHeight();

	// Lazy scratch allocation so the override is free for scenes
	// that have no splat film (e.g. VM-only configurations).
	if( !pScratchImage ) {
		pScratchImage = new RISERasterImage( w, h, RISEColor( 0, 0, 0, 0 ) );
	}

	// Copy the current primary image into the scratch buffer.
	for( unsigned int y = 0; y < h; y++ ) {
		for( unsigned int x = 0; x < w; x++ ) {
			pScratchImage->SetPEL( x, y, primary.GetPEL( x, y ) );
		}
	}

	// Resolve splats into the scratch copy (primary is untouched).
	pSplatFilm->Resolve( *pScratchImage,
		GetEffectiveSplatSPP( w, h ) );

	return *pScratchImage;
}

namespace
{
	// Shared splat-resolve body used by all three flush paths.
	// Copies 'src' into the instance's scratch buffer, resolves
	// the splat film on top, and returns a reference to the
	// scratch buffer for the caller to forward to the base class.
	IRasterImage& ResolveSplatIntoScratch(
		const IRasterImage& src,
		SplatFilm* pSplatFilm,
		IRasterImage*& pScratchImage,
		const Scalar splatTotalSamples
		)
	{
		const unsigned int w = src.GetWidth();
		const unsigned int h = src.GetHeight();
		if( !pScratchImage ) {
			pScratchImage = new RISERasterImage( w, h, RISEColor( 0, 0, 0, 0 ) );
		}
		for( unsigned int y = 0; y < h; y++ ) {
			for( unsigned int x = 0; x < w; x++ ) {
				pScratchImage->SetPEL( x, y, src.GetPEL( x, y ) );
			}
		}
		pSplatFilm->Resolve( *pScratchImage, splatTotalSamples );
		return *pScratchImage;
	}
}

void VCMRasterizerBase::FlushToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	if( !pSplatFilm ) {
		PixelBasedRasterizerHelper::FlushToOutputs( img, rcRegion, frame );
		return;
	}
	IRasterImage& composited = ResolveSplatIntoScratch(
		img, pSplatFilm, pScratchImage,
		GetEffectiveSplatSPP( img.GetWidth(), img.GetHeight() ) );
	PixelBasedRasterizerHelper::FlushToOutputs( composited, rcRegion, frame );
}

void VCMRasterizerBase::FlushPreDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	if( !pSplatFilm ) {
		PixelBasedRasterizerHelper::FlushPreDenoisedToOutputs( img, rcRegion, frame );
		return;
	}
	IRasterImage& composited = ResolveSplatIntoScratch(
		img, pSplatFilm, pScratchImage,
		GetEffectiveSplatSPP( img.GetWidth(), img.GetHeight() ) );
	PixelBasedRasterizerHelper::FlushPreDenoisedToOutputs( composited, rcRegion, frame );
}

void VCMRasterizerBase::FlushDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	// BDPT flow (which we mirror): the incoming image holds only the
	// DENOISED non-splat contributions; we must add the splat film on
	// top before writing the final denoised output so it matches what
	// BDPT produces.
	if( !pSplatFilm ) {
		PixelBasedRasterizerHelper::FlushDenoisedToOutputs( img, rcRegion, frame );
		return;
	}
	IRasterImage& composited = ResolveSplatIntoScratch(
		img, pSplatFilm, pScratchImage,
		GetEffectiveSplatSPP( img.GetWidth(), img.GetHeight() ) );
	PixelBasedRasterizerHelper::FlushDenoisedToOutputs( composited, rcRegion, frame );
}
