//////////////////////////////////////////////////////////////////////
//
//  OIDNDenoiser.cpp - Intel OIDN denoiser wrapper implementation.
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
#include "OIDNDenoiser.h"
#include "AOVBuffers.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/Color_Template.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/ICamera.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IBSDF.h"
#include "../Intersection/RayIntersection.h"
#include "../Utilities/RuntimeContext.h"

#ifdef RISE_ENABLE_OIDN
#include <OpenImageDenoise/oidn.hpp>
#endif

using namespace RISE;
using namespace RISE::Implementation;

void OIDNDenoiser::ImageToFloatBuffer(
	const IRasterImage& img,
	float* buf,
	unsigned int w,
	unsigned int h
	)
{
	for( unsigned int y = 0; y < h; y++ ) {
		for( unsigned int x = 0; x < w; x++ ) {
			const RISEColor c = img.GetPEL( x, y );
			const unsigned int idx = (y * w + x) * 3;
			buf[idx + 0] = static_cast<float>( c.base.r );
			buf[idx + 1] = static_cast<float>( c.base.g );
			buf[idx + 2] = static_cast<float>( c.base.b );
		}
	}
}

void OIDNDenoiser::FloatBufferToImage(
	const float* buf,
	IRasterImage& img,
	unsigned int w,
	unsigned int h
	)
{
	for( unsigned int y = 0; y < h; y++ ) {
		for( unsigned int x = 0; x < w; x++ ) {
			const unsigned int idx = (y * w + x) * 3;
			img.SetPEL( x, y, RISEColor(
				RISEPel(
					static_cast<Chel>( buf[idx + 0] ),
					static_cast<Chel>( buf[idx + 1] ),
					static_cast<Chel>( buf[idx + 2] ) ),
				1.0 ) );
		}
	}
}

#ifdef RISE_ENABLE_OIDN

void OIDNDenoiser::Denoise(
	float* beautyBuffer,
	const float* albedoBuffer,
	const float* normalBuffer,
	unsigned int w,
	unsigned int h,
	float* outputBuffer
	)
{
	oidn::DeviceRef device = oidn::newDevice( oidn::DeviceType::Default );
	device.commit();

	oidn::FilterRef filter = device.newFilter( "RT" );
	filter.setImage( "color", beautyBuffer, oidn::Format::Float3, w, h );
	filter.setImage( "output", outputBuffer, oidn::Format::Float3, w, h );

	if( albedoBuffer ) {
		filter.setImage( "albedo", const_cast<float*>( albedoBuffer ),
			oidn::Format::Float3, w, h );
	}
	if( normalBuffer ) {
		filter.setImage( "normal", const_cast<float*>( normalBuffer ),
			oidn::Format::Float3, w, h );
	}

	filter.set( "hdr", true );
	filter.commit();
	filter.execute();

	const char* errorMessage;
	if( device.getError( errorMessage ) != oidn::Error::None ) {
		GlobalLog()->PrintEx( eLog_Error, "OIDN denoiser error: %s", errorMessage );
	}
}

void OIDNDenoiser::CollectFirstHitAOVs(
	const IScene& scene,
	IRayCaster& caster,
	AOVBuffers& aovBuffers
	)
{
	const ICamera* pCamera = scene.GetCamera();
	if( !pCamera ) {
		return;
	}

	const IObjectManager* pObjects = scene.GetObjects();
	if( !pObjects ) {
		return;
	}

	const unsigned int width = pCamera->GetWidth();
	const unsigned int height = pCamera->GetHeight();

	RuntimeContext rc( GlobalRNG(), RuntimeContext::PASS_NORMAL, false );

	for( unsigned int y = 0; y < height; y++ )
	{
		for( unsigned int x = 0; x < width; x++ )
		{
			Point2 ptOnScreen( x, height - y );

			Ray ray;
			if( !pCamera->GenerateRay( rc, ray, ptOnScreen ) ) {
				continue;
			}

			RasterizerState rast;
			rast.x = x;
			rast.y = y;

			RayIntersection ri( ray, rast );
			pObjects->IntersectRay( ri, true, true, false );

			if( !ri.geometric.bHit ) {
				continue;
			}

			// Normal AOV
			aovBuffers.AccumulateNormal( x, y, ri.geometric.vNormal, 1.0 );

			// Albedo AOV: evaluate BSDF at normal incidence × PI
			if( ri.pMaterial && ri.pMaterial->GetBSDF() )
			{
				Ray aovRay( Point3Ops::mkPoint3( ri.geometric.ptIntersection, ri.geometric.vNormal ),
					-ri.geometric.vNormal );
				RayIntersectionGeometric rig( aovRay, nullRasterizerState );
				rig.ptIntersection = ri.geometric.ptIntersection;
				rig.vNormal = ri.geometric.vNormal;
				rig.onb = ri.geometric.onb;

				RISEPel albedo = ri.pMaterial->GetBSDF()->value( ri.geometric.vNormal, rig ) * PI;
				aovBuffers.AccumulateAlbedo( x, y, albedo, 1.0 );
			}

			aovBuffers.Normalize( x, y, 1.0 );
		}
	}
}

void OIDNDenoiser::ApplyDenoise(
	IRasterImage& image,
	const AOVBuffers& aovBuffers,
	unsigned int w,
	unsigned int h
	)
{
	GlobalLog()->PrintEx( eLog_Event, "Running OIDN denoiser (%ux%u)...", w, h );

	std::vector<float> beautyBuf( w * h * 3 );
	std::vector<float> denoisedBuf( w * h * 3 );
	ImageToFloatBuffer( image, beautyBuf.data(), w, h );
	Denoise(
		beautyBuf.data(),
		aovBuffers.GetAlbedoPtr(),
		aovBuffers.GetNormalPtr(),
		w, h,
		denoisedBuf.data() );
	FloatBufferToImage( denoisedBuf.data(), image, w, h );

	GlobalLog()->PrintEx( eLog_Event, "OIDN denoising complete." );
}

#endif // RISE_ENABLE_OIDN
