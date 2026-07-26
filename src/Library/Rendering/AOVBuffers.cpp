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
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

AOVBuffers::AOVBuffers( unsigned int w, unsigned int h, const Plan& requested ) :
  width( w ),
  height( h ),
  bHasData( false ),
  plan( requested ),
  albedo( requested.albedo ? static_cast<size_t>( w ) * h * 3 : 0, 0.0f ),
  normals( requested.normal ? static_cast<size_t>( w ) * h * 3 : 0, 0.0f ),
  depths( requested.depth ? static_cast<size_t>( w ) * h : 0, 0.0f )
{
}

void AOVBuffers::Reset( unsigned int w, unsigned int h, const Plan& requested )
{
	width = w;
	height = h;
	bHasData.store( false, std::memory_order_relaxed );
	plan = requested;
	const size_t pixels = static_cast<size_t>( w ) * h;
	auto reset = []( std::vector<float>& v, size_t count ) {
		if( count == 0 ) {
			std::vector<float>().swap( v );
			return;
		}
		if( v.size() != count ) v.assign( count, 0.0f );
		else std::fill( v.begin(), v.end(), 0.0f );
	};
	reset( albedo, requested.albedo ? pixels * 3 : 0 );
	reset( normals, requested.normal ? pixels * 3 : 0 );
	reset( depths, requested.depth ? pixels : 0 );
}

void AOVBuffers::AccumulateAlbedo(
	unsigned int x,
	unsigned int y,
	const RISEPel& c,
	Scalar weight
	)
{
	if( albedo.empty() ) return;
	bHasData.store( true, std::memory_order_relaxed );
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
	bHasData.store( true, std::memory_order_relaxed );
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
	bHasData.store( true, std::memory_order_relaxed );
	const size_t idx = static_cast<size_t>( y ) * width + x;
	depths[idx] += static_cast<float>( depth * weight );
}

void AOVBuffers::Normalize(
	unsigned int x,
	unsigned int y,
	Scalar invWeight
	)
{
	const float fw = static_cast<float>( invWeight );
	const size_t pixel = static_cast<size_t>( y ) * width + x;
	if( !albedo.empty() ) {
		const size_t idx = pixel * 3;
		albedo[idx + 0] *= fw;
		albedo[idx + 1] *= fw;
		albedo[idx + 2] *= fw;
	}
	if( !normals.empty() ) {
		const size_t idx = pixel * 3;
		normals[idx + 0] *= fw;
		normals[idx + 1] *= fw;
		normals[idx + 2] *= fw;
	}
	if( !depths.empty() ) depths[pixel] *= fw;
}

void AOVBuffers::ReleaseDepthStorage()
{
	std::vector<float>().swap( depths );
	plan.depth = false;
}

void RISE::Implementation::CollectFirstHitAOVs(
	const IScene& scene,
	IRayCaster& caster,
	AOVBuffers& aovBuffers,
	unsigned int samplesPerPixel
	)
{
	const ICamera* pCamera = scene.GetCamera();
	const IObjectManager* pObjects = scene.GetObjects();
	const IFilm* pFilm = scene.GetFilm();
	if( !pCamera || !pObjects || !pFilm ) return;

	const unsigned int width = pFilm->GetWidth();
	const unsigned int height = pFilm->GetHeight();
	if( width != aovBuffers.GetWidth() || height != aovBuffers.GetHeight() ) return;
	if( samplesPerPixel == 0 ) samplesPerPixel = 1;
	const Scalar invSamples = Scalar( 1.0 ) / Scalar( samplesPerPixel );

	GlobalThreadPool().ParallelFor( height, [&]( unsigned int y ) {
		static thread_local RandomNumberGenerator tl_rng;
		RuntimeContext rc( tl_rng, RuntimeContext::PASS_NORMAL, false );

		for( unsigned int x = 0; x < width; ++x ) {
			for( unsigned int s = 0; s < samplesPerPixel; ++s ) {
				const Point2 ptOnScreen(
					static_cast<Scalar>( x ) + tl_rng.CanonicalRandom(),
					static_cast<Scalar>( height - y ) - tl_rng.CanonicalRandom() );
				Vector3 sampleNormal( 0, 0, 0 );
				RISEPel sampleAlbedo( 1, 1, 1 );
				Scalar sampleDepth = 0;
				Ray ray;
				if( pCamera->GenerateRay( rc, ray, ptOnScreen ) ) {
					RasterizerState rast = { x, y };
					RayIntersection ri( ray, rast );
					pObjects->IntersectRay( ri, true, true, false );
					if( ri.geometric.bHit ) {
						sampleNormal = ri.geometric.vNormal;
						sampleDepth = ri.geometric.range;
						sampleAlbedo = ( ri.pMaterial && ri.pMaterial->GetBSDF() )
							? ri.pMaterial->GetBSDF()->albedo( ri.geometric )
							: RISEPel( 1, 1, 1 );
					}
				}
				aovBuffers.AccumulateAlbedo( x, y, sampleAlbedo, 1.0 );
				aovBuffers.AccumulateNormal( x, y, sampleNormal, 1.0 );
				aovBuffers.AccumulateDepth( x, y, sampleDepth, 1.0 );
			}
			aovBuffers.Normalize( x, y, invSamples );
		}
	} );
	(void)caster; // retained in the API for parity with the historical collector
}
