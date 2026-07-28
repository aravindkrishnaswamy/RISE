//////////////////////////////////////////////////////////////////////
//
//  AOVBuffers.cpp - Implementation of AOV float buffers for denoiser
//  input.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 28, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "AOVBuffers.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/ICamera.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IBSDF.h"
#include "../Intersection/RayIntersection.h"
#include "../Utilities/RuntimeContext.h"
#include "../Utilities/ThreadPool.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/FiniteMath.h"
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

AOVBuffers::AOVBuffers( unsigned int w, unsigned int h, const Plan& requested ) :
  width( w ),
  height( h ),
  bHasAlbedoData( false ),
  bHasNormalData( false ),
  bHasDepthData( false ),
  plan( requested ),
  albedo( requested.albedo ? static_cast<size_t>( w ) * h * 3 : 0, 0.0f ),
  normals( requested.normal ? static_cast<size_t>( w ) * h * 3 : 0, 0.0f ),
  depths( requested.depth ? static_cast<size_t>( w ) * h : 0, 0.0f ),
  depthWeights( requested.depth ? static_cast<size_t>( w ) * h : 0, 0.0f )
{
}

void AOVBuffers::Reset( unsigned int w, unsigned int h, const Plan& requested )
{
	width = w;
	height = h;
	bHasAlbedoData.store( false, std::memory_order_relaxed );
	bHasNormalData.store( false, std::memory_order_relaxed );
	bHasDepthData.store( false, std::memory_order_relaxed );
	plan = requested;
	const size_t pixels = static_cast<size_t>( w ) * h;
	auto reset = []( std::vector<float>& v, size_t count ) {
		if( count == 0 ) {
			std::vector<float>().swap( v );
			return;
		}
		// A resolution change must also discard capacity from the previous
		// film. vector::assign() is permitted to retain that allocation, which
		// would let one historical high-resolution render pin guide memory
		// indefinitely after a downsize.
		if( v.size() != count ) std::vector<float>( count, 0.0f ).swap( v );
		else std::fill( v.begin(), v.end(), 0.0f );
	};
	reset( albedo, requested.albedo ? pixels * 3 : 0 );
	reset( normals, requested.normal ? pixels * 3 : 0 );
	reset( depths, requested.depth ? pixels : 0 );
	reset( depthWeights, requested.depth ? pixels : 0 );
}

void AOVBuffers::AccumulateAlbedo(
	unsigned int x,
	unsigned int y,
	const RISEPel& c,
	Scalar weight
	)
{
	if( albedo.empty() ) return;
	bHasAlbedoData.store( true, std::memory_order_relaxed );
	const size_t idx = ( static_cast<size_t>( y ) * width + x ) * 3;
	// Saturate each channel to [0, 1]: OIDN expects albedo in that
	// range.  IBSDF::albedo() should normally already be in range, but
	// pathological painters (HDR colors > 1) can push it over — keep
	// this as a safety net.
	const Scalar r = r_min( Scalar(1.0), r_max( Scalar(0.0), c.r ) );
	const Scalar g = r_min( Scalar(1.0), r_max( Scalar(0.0), c.g ) );
	const Scalar b = r_min( Scalar(1.0), r_max( Scalar(0.0), c.b ) );
	albedo[idx + 0] += static_cast<float>( r * weight );
	albedo[idx + 1] += static_cast<float>( g * weight );
	albedo[idx + 2] += static_cast<float>( b * weight );
}

void AOVBuffers::AccumulateNormal(
	unsigned int x,
	unsigned int y,
	const Vector3& n,
	Scalar weight
	)
{
	if( normals.empty() ) return;
	bHasNormalData.store( true, std::memory_order_relaxed );
	const size_t idx = ( static_cast<size_t>( y ) * width + x ) * 3;
	normals[idx + 0] += static_cast<float>( n.x * weight );
	normals[idx + 1] += static_cast<float>( n.y * weight );
	normals[idx + 2] += static_cast<float>( n.z * weight );
}

void AOVBuffers::AccumulateDepth(
	unsigned int x,
	unsigned int y,
	Scalar depth,
	Scalar weight
	)
{
	if( depths.empty() ) return;
	bHasDepthData.store( true, std::memory_order_relaxed );
	if( !RISE::IsFiniteDouble( static_cast<double>( depth ) ) || depth <= 0 ||
		!RISE::IsFiniteDouble( static_cast<double>( weight ) ) || weight <= 0 ) return;
	const size_t idx = static_cast<size_t>( y ) * width + x;
	depths[idx] += static_cast<float>( depth * weight );
	depthWeights[idx] += static_cast<float>( weight );
}

void AOVBuffers::MarkGuidesExamined()
{
	if( !albedo.empty() ) bHasAlbedoData.store( true, std::memory_order_relaxed );
	if( !normals.empty() ) bHasNormalData.store( true, std::memory_order_relaxed );
}

void AOVBuffers::Normalize(
	unsigned int x,
	unsigned int y,
	Scalar invWeight
	)
{
	NormalizeSelected( x, y, invWeight, plan );
}

void AOVBuffers::NormalizeSelected(
	unsigned int x,
	unsigned int y,
	Scalar invWeight,
	const Plan& selected
	)
{
	const float fw = static_cast<float>( invWeight );
	const size_t pixel = static_cast<size_t>( y ) * width + x;
	if( selected.albedo && !albedo.empty() ) {
		const size_t idx = pixel * 3;
		albedo[idx + 0] *= fw;
		albedo[idx + 1] *= fw;
		albedo[idx + 2] *= fw;
	}
	if( selected.normal && !normals.empty() ) {
		const size_t idx = pixel * 3;
		normals[idx + 0] *= fw;
		normals[idx + 1] *= fw;
		normals[idx + 2] *= fw;
	}
	if( selected.depth && !depths.empty() ) {
		const float hitWeight = depthWeights[pixel];
		depths[pixel] = hitWeight > 0.0f ? depths[pixel] / hitWeight : 0.0f;
	}
}

void AOVBuffers::ReleaseDepthStorage()
{
	std::vector<float>().swap( depths );
	std::vector<float>().swap( depthWeights );
	plan.depth = false;
	bHasDepthData.store( false, std::memory_order_relaxed );
}

void RISE::Implementation::CollectFirstHitAOVs(
	const IScene& scene,
	IRayCaster& caster,
	AOVBuffers& aovBuffers,
	unsigned int samplesPerPixel,
	OidnPrefilter prefilterMode,
	const Rect* region
	)
{
	CollectFirstHitAOVRows( scene, caster, aovBuffers, aovBuffers.MissingPlan(),
		0, 1, samplesPerPixel, prefilterMode, region );
}

void RISE::Implementation::CollectFirstHitAOVRows(
	const IScene& scene,
	IRayCaster& caster,
	AOVBuffers& aovBuffers,
	const AOVBuffers::Plan& selected,
	unsigned int firstRow,
	unsigned int rowStride,
	unsigned int samplesPerPixel,
	OidnPrefilter prefilterMode,
	const Rect* region
	)
{
	const ICamera* pCamera = scene.GetCamera();
	const IObjectManager* pObjects = scene.GetObjects();
	const IFilm* pFilm = scene.GetFilm();
	if( !pCamera || !pObjects || !pFilm ) return;

	const unsigned int width = pFilm->GetWidth();
	const unsigned int height = pFilm->GetHeight();
	if( width != aovBuffers.GetWidth() || height != aovBuffers.GetHeight() ) return;
	if( !selected.Any() || firstRow >= height || rowStride == 0 ) return;
	if( width == 0 || height == 0 ) return;
	unsigned int startX = 0, startY = 0, endX = width-1, endY = height-1;
	if( region ) {
		if( region->left > region->right || region->top > region->bottom
			|| region->left >= width || region->top >= height ) return;
		startX = region->left;
		startY = region->top;
		endX = r_min( region->right, width-1 );
		endY = r_min( region->bottom, height-1 );
	}
	if( samplesPerPixel == 0 ) samplesPerPixel = 1;
	const AOVBuffers::Plan missing = selected;
	const Scalar invSamples = Scalar( 1.0 ) / Scalar( samplesPerPixel );
	const unsigned int rowCount = 1u + ( height - 1u - firstRow ) / rowStride;

	GlobalThreadPool().ParallelFor( rowCount, [&]( unsigned int row ) {
		const unsigned int y = firstRow + row * rowStride;
		if( y < startY || y > endY ) return;
		static thread_local RandomNumberGenerator tl_rng;
		RuntimeContext rc( tl_rng, RuntimeContext::PASS_NORMAL, false );
		rc.aovPrefilterMode = prefilterMode;

		for( unsigned int x = startX; x <= endX; ++x ) {
			for( unsigned int s = 0; s < samplesPerPixel; ++s ) {
				const Point2 ptOnScreen(
					static_cast<Scalar>( x ) + tl_rng.CanonicalRandom(),
					static_cast<Scalar>( height - y ) - tl_rng.CanonicalRandom() );
				Vector3 sampleNormal( 0, 0, 0 );
				RISEPel sampleAlbedo( 0, 0, 0 );
				Scalar sampleDepth = 0;
				Ray ray;
				if( pCamera->GenerateRay( rc, ray, ptOnScreen ) ) {
					RasterizerState rast = { x, y };
					const bool traceAccurateGuide =
						prefilterMode == OidnPrefilter::Accurate &&
						( missing.albedo || missing.normal );
					if( traceAccurateGuide ) {
						// Estimators such as MLT cannot attach PixelAOV to their
						// splat-chain samples. Their bounded fallback retrace still
						// honors Accurate semantics by asking the prepared shader
						// caster to walk delta/medium continuations and populate one
						// coherent guide. Depth remains the raw camera intersection,
						// captured by RayCaster before any continuation.
						PixelAOV aov;
						rc.pAOV = &aov;
						RISEPel ignoredRadiance( 0, 0, 0 );
						caster.CastRay( rc, rast, ray, ignoredRadiance,
							IRayCaster::RAY_STATE(), 0, 0 );
						rc.pAOV = 0;
						if( aov.valid ) {
							sampleNormal = aov.normal;
							sampleAlbedo = aov.albedo;
						}
						if( aov.primaryDepthCaptured ) sampleDepth = aov.depth;
					} else {
						RayIntersection ri( ray, rast );
						pObjects->IntersectRay( ri, true, true, false );
						if( ri.geometric.bHit ) {
							if( ri.pModifier ) ri.pModifier->Modify( ri.geometric );
							sampleNormal = ri.geometric.vNormal;
							sampleDepth = ri.geometric.range;
							sampleAlbedo = ( ri.pMaterial && ri.pMaterial->GetBSDF() )
								? ri.pMaterial->GetBSDF()->albedo( ri.geometric )
								: RISEPel( 1, 1, 1 );
						}
					}
				}
				if( missing.albedo ) aovBuffers.AccumulateAlbedo( x, y, sampleAlbedo, 1.0 );
				if( missing.normal ) aovBuffers.AccumulateNormal( x, y, sampleNormal, 1.0 );
				if( missing.depth ) aovBuffers.AccumulateDepth( x, y, sampleDepth, 1.0 );
			}
			aovBuffers.NormalizeSelected( x, y, invSamples, missing );
		}
	} );
}
