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
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

AOVBuffers::AOVBuffers( unsigned int w, unsigned int h ) :
  width( w ),
  height( h ),
  bHasData( false ),
  albedo( w * h * 3, 0.0f ),
  normals( w * h * 3, 0.0f )
{
}

void AOVBuffers::Reset( unsigned int w, unsigned int h )
{
	width = w;
	height = h;
	bHasData = false;
	const size_t count = static_cast<size_t>( w ) * h * 3;
	if( albedo.size() != count ) {
		albedo.assign( count, 0.0f );
		normals.assign( count, 0.0f );
	} else {
		std::fill( albedo.begin(), albedo.end(), 0.0f );
		std::fill( normals.begin(), normals.end(), 0.0f );
	}
}

void AOVBuffers::AccumulateAlbedo(
	unsigned int x,
	unsigned int y,
	const RISEPel& c,
	Scalar weight
	)
{
	bHasData = true;
	const unsigned int idx = (y * width + x) * 3;
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
	const unsigned int idx = (y * width + x) * 3;
	normals[idx + 0] += static_cast<float>( n.x * weight );
	normals[idx + 1] += static_cast<float>( n.y * weight );
	normals[idx + 2] += static_cast<float>( n.z * weight );
}

void AOVBuffers::Normalize(
	unsigned int x,
	unsigned int y,
	Scalar invWeight
	)
{
	const unsigned int idx = (y * width + x) * 3;
	const float fw = static_cast<float>( invWeight );
	albedo[idx + 0] *= fw;
	albedo[idx + 1] *= fw;
	albedo[idx + 2] *= fw;
	normals[idx + 0] *= fw;
	normals[idx + 1] *= fw;
	normals[idx + 2] *= fw;
}
