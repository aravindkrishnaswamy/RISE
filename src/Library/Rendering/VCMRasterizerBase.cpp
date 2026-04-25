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
#include "../Interfaces/IOptions.h"
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
	BidirectionalRasterizerBase( pCaster_, stabilityCfg ),
	pIntegrator( 0 ),
	pLightVertexStore( 0 ),
	mBaseMergeRadius( 0 ),
	mCurrentMergeRadius( 0 ),
	mMergeRadiusFloor( 0 ),
	mGeometricRadiusFloor( 0 ),
	mMergeRadiusPassCount( 0 ),
	mRadiusShrinkAlpha( Scalar( 2.0 ) / Scalar( 3.0 ) ),
	mTargetPhotonsPerQuery( Scalar( 20 ) ),
	// `vcm_disable_progressive_radius=true` in global.options (or via
	// RISE_OPTIONS_FILE) reverts to fixed-radius SmallVCM — used for
	// benchmarking + regression comparison.
	mProgressiveRadiusEnabled(
		!GlobalOptions().ReadBool( "vcm_disable_progressive_radius", false ) ),
	// Throughput clamp tuning.  Defaults: 99th-percentile threshold,
	// 20× multiplier — clamps only the brightest ~1% of photons and
	// only when they exceed 20× the bulk distribution's tail.  Tuned
	// to suppress SSS-induced fireflies without measurably biasing
	// well-behaved scenes.  Override via global options
	// `vcm_throughput_clamp_percentile` (0..1) and
	// `vcm_throughput_clamp_multiplier` (>0; 0 disables).
	mThroughputClampPercentile(
		GlobalOptions().ReadDouble( "vcm_throughput_clamp_percentile", 0.99 ) ),
	mThroughputClampMultiplier(
		GlobalOptions().ReadDouble( "vcm_throughput_clamp_multiplier", 20.0 ) )
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
	delete pLightVertexStore;
	pLightVertexStore = 0;
	// pSplatFilm and pScratchImage are released by the
	// BidirectionalRasterizerBase destructor.
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
							// array would corrupt seams).  See the
							// "NORMALIZATION INVARIANT" block below for
							// why pathsShot counts emissions, not
							// branches — the per-emission density
							// estimate is already correctly normalized
							// without treating each branch as an
							// independent subpath.
							pGen->GenerateLightSubpath( scene, caster, sampler, tl.tmpLightVerts, tmpLightSubpathStarts, rc.random, Scalar( -1 ) );
							if( tl.tmpLightVerts.empty() ) {
								continue;
							}

							// NORMALIZATION INVARIANT: pathsShot counts
							// INDEPENDENT EMISSIONS (one per
							// GenerateLightSubpath call), NOT branch
							// copies.  Splitting at a delta vertex is a
							// variance-reduction technique with zero
							// effect on expectation: every branch i
							// carries throughput beta * kray_i with
							// sum(kray_i) == original kray, so total
							// deposited energy across all branches
							// equals the single-branch total.
							// Counting each branch as its own subpath
							// would inflate mLightSubPathCount and
							// shrink the VM density by 1/N — producing
							// a 1/N-times-too-dim render (this was the
							// bug before the fix; set branching_
							// threshold=1.0 to force single-branch
							// behavior pre-fix).
							tl.pathsShot++;

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
			// Scene-geometric floor: 1/10th of the initial auto-radius.
			// Prevents progressive shrinkage collapsing to sub-numeric-
			// precision on pathological scenes.  Users can override via
			// a scene param in a later step.
			mGeometricRadiusFloor = Scalar( 0.001 ) * medianSeg;

			GlobalLog()->PrintEx( eLog_Event,
				"VCMRasterizerBase::PreRenderSetup:: auto-radius "
				"segments=%zu median_segment=%g effective_radius=%g geom_floor=%g",
				segLens.size(), (double)medianSeg,
				(double)effectiveMergeRadius, (double)mGeometricRadiusFloor );
		} else {
			GlobalLog()->PrintEx( eLog_Warning,
				"VCMRasterizerBase::PreRenderSetup:: auto-radius "
				"failed — disabling VM" );
			effectiveMergeRadius = 0;
		}
	}

	// Set up progressive-radius state.  With user-supplied radius, the
	// geometric floor defaults to 1/10th of it (mirrors the auto path).
	if( mGeometricRadiusFloor <= 0 && effectiveMergeRadius > 0 ) {
		mGeometricRadiusFloor = effectiveMergeRadius * Scalar( 0.1 );
	}
	mBaseMergeRadius = effectiveMergeRadius;
	mCurrentMergeRadius = effectiveMergeRadius;
	mMergeRadiusFloor = mGeometricRadiusFloor;
	mMergeRadiusPassCount = 0;


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
				GlobalLog()->PrintEx( eLog_Info,
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

	GlobalLog()->PrintEx( eLog_Info,
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
// Also shrinks the merge radius using the Hachisuka-Ogaki-Jensen
// (SPPM) formula r_{n+1} = r_n * sqrt((n+alpha)/(n+1)), clamped from
// below by mMergeRadiusFloor so shrinkage stops once Poisson noise
// on photon count per query would dominate bias reduction.
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

	// ---------------------------------------------------------------
	// Progressive radius reduction.
	//
	// Hachisuka-Ogaki-Jensen SPPM shrinkage:
	//     r_{n+1}^2 = r_n^2 * (n + alpha) / (n + 1)
	// with alpha in (0,1).  `n` is the 1-based completed-iteration
	// count: at the very first shrink call it equals 1 and the factor
	// is sqrt((1+alpha)/2) ~= 0.913 for alpha=2/3 — NOT sqrt(alpha),
	// which would be the formula one iteration later.
	//
	// We track mMergeRadiusPassCount as that 1-based counter.  Pre-
	// increment: value before ++ is the previous iteration's n,
	// post-increment it becomes current n.  Apply the factor
	// sqrt((n+alpha)/(n+1)) where n is the current value.
	//
	// The adaptive floor `mMergeRadiusFloor` caps further shrinkage.
	// When disabled (`mProgressiveRadiusEnabled=false`) or when the
	// floor == base, the radius stays at r_0 forever — matches the
	// prior fixed-radius behavior exactly.
	// ---------------------------------------------------------------
	if( mProgressiveRadiusEnabled && mBaseMergeRadius > 0 )
	{
		mMergeRadiusPassCount++;
		const Scalar n = static_cast<Scalar>( mMergeRadiusPassCount );
		const Scalar alpha = mRadiusShrinkAlpha;
		const Scalar shrinkFactor = std::sqrt( ( n + alpha ) / ( n + Scalar( 1 ) ) );
		const Scalar rShrunk = mCurrentMergeRadius * shrinkFactor;
		const Scalar rClamped = std::max( rShrunk, mMergeRadiusFloor );
		mCurrentMergeRadius = rClamped;
	}

	pLightVertexStore->Clear();

	// K = 1 matches the forced samplesPerPass = 1 in PreRenderSetup.
	const unsigned int samplesPerSuperIter = 1;
	const uint32_t baseSampleIndex = passIdx;

	// Recompute normalization against the current (possibly shrunken)
	// radius BEFORE generating the light pass; the dispatcher passes
	// mVCMNormalization into ConvertLightSubpath, which stores the MIS
	// quantities against this normalization on every LightVertex.
	mVCMNormalization = ComputeNormalization(
		width, height, mCurrentMergeRadius,
		pIntegrator->GetEnableVC(),
		pIntegrator->GetEnableVM() );

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
			width, height, mCurrentMergeRadius,
			pIntegrator->GetEnableVC(),
			pIntegrator->GetEnableVM(),
			static_cast<Scalar>( pathsShot ) );
	}

	// Cap outlier photon throughputs before tree balance.  The biased
	// rescale targets the firefly tail produced by rare bright photons
	// (notably BSSRDF emergences with small pdfSurface), which under
	// progressive radius shrinkage become more pronounced as 1/r²
	// amplifies their per-merge contribution.  Skipped when
	// mThroughputClampMultiplier == 0 (set via global option).
	if( pIntegrator->GetEnableVM() && mThroughputClampMultiplier > 0 ) {
		pLightVertexStore->ClampOutlierThroughputs(
			mThroughputClampPercentile,
			mThroughputClampMultiplier );
	}

	pLightVertexStore->BuildKDTreeParallel();

	// Update the adaptive radius floor from the just-built store's
	// photon density.  Target K photons per merge query:
	//   r_floor_density = sqrt(K / (pi * density))
	//   density         = storeSize / bboxSurfaceArea
	// Cap at 0.5 * r_0 so the adaptive floor can never prevent at
	// least a 2x shrinkage — otherwise pathologically sparse scenes
	// would freeze at the initial auto-radius.  The geometric floor
	// (0.001 * medianSegment, set in PreRenderSetup) is the hard
	// lower bound to avoid sub-numeric-precision collapse.
	if( mProgressiveRadiusEnabled && mBaseMergeRadius > 0 && totalStored > 0 ) {
		const Scalar surfaceArea = pLightVertexStore->ComputeBBoxSurfaceArea();
		if( surfaceArea > NEARZERO ) {
			const Scalar density = static_cast<Scalar>( totalStored ) / surfaceArea;
			if( density > NEARZERO ) {
				const Scalar rFloorRaw = std::sqrt( mTargetPhotonsPerQuery / ( PI * density ) );
				const Scalar rFloorCapped = std::min( rFloorRaw, mBaseMergeRadius * Scalar( 0.5 ) );
				mMergeRadiusFloor = std::max( mGeometricRadiusFloor, rFloorCapped );
			}
		}
	}

	// Periodic logging of shrinkage schedule.
	const bool verbose =
		( passIdx == 1 ) ||
		( passIdx <= 4 ) ||
		( ( passIdx % 16 ) == 0 );
	const LOG_ENUM logLevel = verbose ? eLog_Event : eLog_Info;
	GlobalLog()->PrintEx( logLevel,
		"VCMRasterizerBase::OnProgressivePassBegin:: iteration %u — "
		"rebuilt store with %llu light vertices (K=%u, r=%g, floor=%g, r/r_0=%.3f)",
		passIdx, totalStored, samplesPerSuperIter,
		(double)mCurrentMergeRadius, (double)mMergeRadiusFloor,
		(double)( mBaseMergeRadius > 0 ? mCurrentMergeRadius / mBaseMergeRadius : 1.0 ) );
}

// GetIntermediateOutputImage and ResolveSplatIntoScratch are inherited
// from BidirectionalRasterizerBase — both algorithms use the same
// lazy-scratch splat composition.

void VCMRasterizerBase::FlushToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	if( !pSplatFilm ) {
		PixelBasedRasterizerHelper::FlushToOutputs( img, rcRegion, frame );
		return;
	}
	IRasterImage& composited = ResolveSplatIntoScratch( img );
	PixelBasedRasterizerHelper::FlushToOutputs( composited, rcRegion, frame );
}

void VCMRasterizerBase::FlushPreDenoisedToOutputs( const IRasterImage& img, const Rect* rcRegion, const unsigned int frame ) const
{
	if( !pSplatFilm ) {
		PixelBasedRasterizerHelper::FlushPreDenoisedToOutputs( img, rcRegion, frame );
		return;
	}
	IRasterImage& composited = ResolveSplatIntoScratch( img );
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
	IRasterImage& composited = ResolveSplatIntoScratch( img );
	PixelBasedRasterizerHelper::FlushDenoisedToOutputs( composited, rcRegion, frame );
}
