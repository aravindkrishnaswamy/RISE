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
#include "../Lights/LightSampler.h"
#include "../Shaders/BDPTIntegrator.h"
#include "../Shaders/BDPTVertex.h"
#include "../RasterImages/RasterImage.h"

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

//////////////////////////////////////////////////////////////////////
// PreRenderSetup — VCM light pass
//
// Runs SINGLE-THREADED for Step 6.  Parallel dispatch is a follow-up
// (see Step 6 notes in docs/humming-snuggling-cascade.md).  The
// single-threaded loop validates memory + state-machine correctness
// without racing the Sobol sampler against thread-local BDPT guiding
// state.
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

	// Force 1 SPP per progressive pass.  The original VCM paper
	// (Georgiev 2012, Section 2.3) defines one iteration as:
	// generate W×H light subpaths → build store → 1 eye sample
	// per pixel.  Each progressive pass IS one VCM iteration.
	// The store is rebuilt at the start of each pass via
	// OnProgressivePassBegin.  This decouples store rebuilds from
	// the UI progressive-preview concept.
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
					pGen->GenerateLightSubpath( pScene, *pCaster, sampler, tmpLightVerts, rc.random );
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

	mVCMNormalization = ComputeNormalization(
		width, height,
		effectiveMergeRadius,
		pIntegrator->GetEnableVC(),
		pIntegrator->GetEnableVM() );

	unsigned long long totalStored = 0;
	unsigned long long pathsShot = 0;

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
			pGen->GenerateLightSubpath( pScene, *pCaster, sampler, tmpLightVerts, rc.random );
			if( tmpLightVerts.empty() ) {
				continue;
			}
			pathsShot++;

			tmpConverted.clear();
			VCMIntegrator::ConvertLightSubpath(
				tmpLightVerts, mVCMNormalization, tmpConverted, &tmpLightMis );

			for( std::size_t k = 0; k < tmpConverted.size(); k++ ) {
				pLightVertexStore->Append( tmpConverted[k] );
				totalStored++;
			}
		}
	}
	}  // for s

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

	pLightVertexStore->BuildKDTree();

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

	std::vector<BDPTVertex> tmpLightVerts;
	std::vector<LightVertex> tmpConverted;
	std::vector<VCMMisQuantities> tmpLightMis;
	tmpLightVerts.reserve( pIntegrator->GetMaxLightDepth() + 1 );
	tmpConverted.reserve( pIntegrator->GetMaxLightDepth() + 1 );
	tmpLightMis.reserve( pIntegrator->GetMaxLightDepth() + 1 );

	// Each iteration uses passIdx as the Sobol sample index so light
	// subpaths get different positions per iteration.  The eye pass
	// in IntegratePixel uses the same passIdx via globalSampleIndex,
	// but draws from non-overlapping Sobol dimension streams
	// (StartStream partitioning).
	RuntimeContext rc( GlobalRNG(), RuntimeContext::PASS_NORMAL, false );
	PrepareRuntimeContext( rc );

	unsigned long long totalStored = 0;

	for( unsigned int y = 0; y < height; y++ )
	{
		for( unsigned int x = 0; x < width; x++ )
		{
			const uint32_t pixelSeed = y * width + x;
			SobolSampler sampler( passIdx, pixelSeed );

			tmpLightVerts.clear();
			pGen->GenerateLightSubpath( pScene, *pCaster, sampler, tmpLightVerts, rc.random );
			if( tmpLightVerts.empty() ) {
				continue;
			}

			tmpConverted.clear();
			VCMIntegrator::ConvertLightSubpath(
				tmpLightVerts, mVCMNormalization, tmpConverted, &tmpLightMis );

			for( std::size_t k = 0; k < tmpConverted.size(); k++ ) {
				pLightVertexStore->Append( tmpConverted[k] );
				totalStored++;
			}
		}
	}

	pLightVertexStore->BuildKDTree();

	GlobalLog()->PrintEx( eLog_Event,
		"VCMRasterizerBase::OnProgressivePassBegin:: iteration %u — "
		"rebuilt store with %llu light vertices",
		passIdx, totalStored );
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
