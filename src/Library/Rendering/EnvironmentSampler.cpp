//////////////////////////////////////////////////////////////////////
//
//  EnvironmentSampler.cpp - Implementation of EnvironmentSampler
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 30, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "EnvironmentSampler.h"
#include "../Utilities/Color/ColorMath.h"
#include "../Intersection/RayIntersectionGeometric.h"

using namespace RISE;
using namespace RISE::Implementation;

EnvironmentSampler::EnvironmentSampler(
	const IPainter& p,
	const Scalar scale,
	const Matrix4& transform,
	const unsigned int resolution
	) :
  painter( p ),
  dScale( scale ),
  mxtransform( transform ),
  width( resolution ),
  height( resolution ),
  totalLuminance( 0 )
{
	painter.addref();

	// Compute inverse transform (map-space -> world-space).
	// The transform is a rotation, so inverse = transpose.
	mxInvTransform = Matrix4Ops::Transpose( mxtransform );
}

EnvironmentSampler::~EnvironmentSampler()
{
	painter.release();
}

void EnvironmentSampler::DirectionToTexCoord(
	const Vector3& dir,
	Scalar& s,
	Scalar& t
	)
{
	const Scalar denom = sqrt( dir.x * dir.x + dir.y * dir.y );
	if( denom < 1e-12 )
	{
		// Direction is along +Z or -Z axis
		s = 0.5;
		t = 0.5;
		return;
	}
	const Scalar r = 0.159154943 * acos( r_max( Scalar(-1.0), r_min( Scalar(1.0), -dir.z ) ) ) / denom;
	s = 0.5 + dir.x * r;
	t = 0.5 - dir.y * r;
}

Vector3 EnvironmentSampler::TexCoordToDirection(
	const Scalar s,
	const Scalar t
	)
{
	const Scalar dx = s - 0.5;
	const Scalar dy = -(t - 0.5);
	const Scalar rTexel = sqrt( dx * dx + dy * dy );

	if( rTexel < 1e-12 )
	{
		// Center of probe: direction = (0, 0, -1)
		return Vector3( 0, 0, -1 );
	}

	// Inverse of light-probe mapping:
	// rTexel = theta / (2*PI), so theta = rTexel * 2*PI
	const Scalar theta = rTexel * TWO_PI;
	const Scalar sinTheta = sin( theta );
	const Scalar cosTheta = cos( theta );

	// Reconstruct direction from (theta, phi) encoded in (dx, dy)
	const Scalar vx = dx * sinTheta / rTexel;
	const Scalar vy = dy * sinTheta / rTexel;
	const Scalar vz = -cosTheta;

	return Vector3( vx, vy, vz );
}

unsigned int EnvironmentSampler::SampleCDF(
	const std::vector<Scalar>& cdf,
	const unsigned int n,
	const Scalar xi,
	Scalar& fractional
	)
{
	// Binary search: find largest index i such that cdf[i] <= xi
	unsigned int lo = 0;
	unsigned int hi = n;

	while( lo + 1 < hi )
	{
		const unsigned int mid = (lo + hi) / 2;
		if( cdf[mid] <= xi )
		{
			lo = mid;
		}
		else
		{
			hi = mid;
		}
	}

	// Fractional position within the bin [lo, lo+1)
	const Scalar binWidth = cdf[lo + 1] - cdf[lo];
	if( binWidth > 0 )
	{
		fractional = (xi - cdf[lo]) / binWidth;
	}
	else
	{
		fractional = 0;
	}

	return lo;
}

void EnvironmentSampler::Build()
{
	// Allocate CDF arrays
	conditionalCDF.resize( height );
	for( unsigned int v = 0; v < height; v++ )
	{
		conditionalCDF[v].resize( width + 1 );
	}
	marginalCDF.resize( height + 1 );

	// Create a dummy rasterizer state for painter queries
	const RasterizerState rast = nullRasterizerState;

	// For each row v, build the conditional CDF over columns u.
	// Weight each texel by L * sin(theta) / theta to account for
	// the solid angle Jacobian of the light-probe mapping.
	for( unsigned int v = 0; v < height; v++ )
	{
		conditionalCDF[v][0] = 0;

		for( unsigned int u = 0; u < width; u++ )
		{
			// Texel center in (s, t) space
			const Scalar s = (static_cast<Scalar>(u) + 0.5) / static_cast<Scalar>(width);
			const Scalar t_coord = (static_cast<Scalar>(v) + 0.5) / static_cast<Scalar>(height);

			// Query painter at this texture coordinate
			Ray dummyRay( Point3(0,0,0), Vector3(0,0,1) );
			RayIntersectionGeometric rig( dummyRay, rast );
			rig.ptCoord.x = s;
			rig.ptCoord.y = t_coord;

			const RISEPel color = painter.GetColor( rig ) * dScale;
			const Scalar luminance = ColorMath::MaxValue( color );

			// Compute theta from (s, t)
			const Scalar dx = s - 0.5;
			const Scalar dy = -(t_coord - 0.5);
			const Scalar rTexel = sqrt( dx * dx + dy * dy );
			const Scalar theta = rTexel * TWO_PI;

			// Solid-angle correction weight: sin(theta) / theta
			// When theta -> 0, the ratio -> 1 (L'Hopital).
			// When theta -> PI, sin(theta) -> 0 as expected.
			Scalar solidAngleWeight;
			if( theta < 1e-6 )
			{
				solidAngleWeight = 1.0;
			}
			else
			{
				solidAngleWeight = sin( theta ) / theta;
			}

			// Texels outside the probe disk (rTexel > 0.5 -> theta > PI)
			// should have zero weight.
			if( rTexel > 0.5 )
			{
				solidAngleWeight = 0;
			}

			const Scalar weight = luminance * solidAngleWeight;
			conditionalCDF[v][u + 1] = conditionalCDF[v][u] + weight;
		}
	}

	// Build marginal CDF from row totals
	marginalCDF[0] = 0;
	for( unsigned int v = 0; v < height; v++ )
	{
		marginalCDF[v + 1] = marginalCDF[v] + conditionalCDF[v][width];
	}

	totalLuminance = marginalCDF[height];

	if( totalLuminance <= 0 )
	{
		// Environment map is black — sampling will fail
		return;
	}

	// Normalize CDFs to [0, 1]
	for( unsigned int v = 0; v < height; v++ )
	{
		const Scalar rowTotal = conditionalCDF[v][width];
		if( rowTotal > 0 )
		{
			for( unsigned int u = 0; u <= width; u++ )
			{
				conditionalCDF[v][u] /= rowTotal;
			}
		}
		// Force the last entry to exactly 1.0
		conditionalCDF[v][width] = 1.0;
	}

	for( unsigned int v = 0; v <= height; v++ )
	{
		marginalCDF[v] /= totalLuminance;
	}
	marginalCDF[height] = 1.0;
}

void EnvironmentSampler::Sample(
	const Scalar u1,
	const Scalar u2,
	Vector3& direction,
	Scalar& pdf
	) const
{
	if( totalLuminance <= 0 )
	{
		direction = Vector3( 0, 0, 1 );
		pdf = 0;
		return;
	}

	// Sample row (marginal)
	Scalar fracV;
	const unsigned int v = SampleCDF( marginalCDF, height, u2, fracV );

	// Sample column (conditional) within selected row
	Scalar fracU;
	const unsigned int u = SampleCDF( conditionalCDF[v], width, u1, fracU );

	// Continuous (s, t) coordinates with fractional offset
	const Scalar s = (static_cast<Scalar>(u) + fracU) / static_cast<Scalar>(width);
	const Scalar t_coord = (static_cast<Scalar>(v) + fracV) / static_cast<Scalar>(height);

	// Convert to direction in map space, then transform to world space
	const Vector3 mapDir = TexCoordToDirection( s, t_coord );
	direction = Vector3Ops::Transform( mxInvTransform, mapDir );

	// Compute the PDF
	// pdf_st = marginal_pdf(v) * conditional_pdf(u|v)
	// where marginal_pdf(v) = (marginalCDF[v+1] - marginalCDF[v]) * height
	// and   conditional_pdf(u|v) = (conditionalCDF[v][u+1] - conditionalCDF[v][u]) * width

	const Scalar marginalPdf = (marginalCDF[v + 1] - marginalCDF[v]) * static_cast<Scalar>(height);
	const Scalar conditionalPdf = (conditionalCDF[v][u + 1] - conditionalCDF[v][u]) * static_cast<Scalar>(width);
	const Scalar pdfST = marginalPdf * conditionalPdf;

	// Convert from (s,t) density to solid-angle density.
	// pdf_omega = pdf_st * theta / (4 * PI^2 * sin(theta))
	const Scalar dx = s - 0.5;
	const Scalar dy = -(t_coord - 0.5);
	const Scalar rTexel = sqrt( dx * dx + dy * dy );
	const Scalar theta = rTexel * TWO_PI;

	if( theta < 1e-6 || rTexel > 0.5 )
	{
		// Near the pole or outside the disk: use limiting value
		// theta/sin(theta) -> 1 as theta -> 0
		pdf = pdfST / ((4.0 * PI));
	}
	else
	{
		const Scalar sinTheta = sin( theta );
		if( sinTheta < 1e-10 )
		{
			pdf = 0;
			return;
		}
		pdf = pdfST * theta / ((4.0 * PI) * PI * sinTheta);
	}
}

Scalar EnvironmentSampler::Pdf(
	const Vector3& direction
	) const
{
	if( totalLuminance <= 0 )
	{
		return 0;
	}

	// Transform world direction to map space
	const Vector3 mapDir = Vector3Ops::Transform( mxtransform, direction );

	// Convert to (s, t) texture coordinates
	Scalar s, t_coord;
	DirectionToTexCoord( mapDir, s, t_coord );

	// Clamp to [0, 1)
	s = r_max( Scalar(0), r_min( Scalar(0.999999), s ) );
	t_coord = r_max( Scalar(0), r_min( Scalar(0.999999), t_coord ) );

	// Find the texel indices
	const unsigned int u = r_min( static_cast<unsigned int>( s * width ), width - 1 );
	const unsigned int v = r_min( static_cast<unsigned int>( t_coord * height ), height - 1 );

	// Compute pdf_st = marginal_pdf * conditional_pdf
	const Scalar marginalPdf = (marginalCDF[v + 1] - marginalCDF[v]) * static_cast<Scalar>(height);
	const Scalar conditionalPdf = (conditionalCDF[v][u + 1] - conditionalCDF[v][u]) * static_cast<Scalar>(width);
	const Scalar pdfST = marginalPdf * conditionalPdf;

	// Convert to solid-angle density
	const Scalar dx = s - 0.5;
	const Scalar dy = -(t_coord - 0.5);
	const Scalar rTexel = sqrt( dx * dx + dy * dy );
	const Scalar theta = rTexel * TWO_PI;

	if( theta < 1e-6 || rTexel > 0.5 )
	{
		return pdfST / ((4.0 * PI));
	}

	const Scalar sinTheta = sin( theta );
	if( sinTheta < 1e-10 )
	{
		return 0;
	}

	return pdfST * theta / ((4.0 * PI) * PI * sinTheta);
}
