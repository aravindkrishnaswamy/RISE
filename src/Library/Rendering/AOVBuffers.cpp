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

using namespace RISE;
using namespace RISE::Implementation;

AOVBuffers::AOVBuffers( unsigned int w, unsigned int h ) :
  width( w ),
  height( h ),
  albedo( w * h * 3, 0.0f ),
  normals( w * h * 3, 0.0f )
{
}

void AOVBuffers::AccumulateAlbedo(
	unsigned int x,
	unsigned int y,
	const RISEPel& c,
	Scalar weight
	)
{
	const unsigned int idx = (y * width + x) * 3;
	albedo[idx + 0] += static_cast<float>( c.r * weight );
	albedo[idx + 1] += static_cast<float>( c.g * weight );
	albedo[idx + 2] += static_cast<float>( c.b * weight );
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
