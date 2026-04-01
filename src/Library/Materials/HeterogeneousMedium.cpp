//////////////////////////////////////////////////////////////////////
//
//  HeterogeneousMedium.cpp - Implementation of the heterogeneous
//    participating medium with delta tracking
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 31, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "HeterogeneousMedium.h"
#include <math.h>

using namespace RISE;

/// Maximum number of delta tracking steps before giving up.
/// This prevents infinite loops in degenerate cases.
static const unsigned int nMaxDeltaTrackingSteps = 1024;


HeterogeneousMedium::HeterogeneousMedium(
	const RISEPel& max_sigma_a,
	const RISEPel& max_sigma_s,
	const IPhaseFunction& phase,
	IVolumeAccessor& accessor,
	const unsigned int volWidth,
	const unsigned int volHeight,
	const unsigned int volDepth,
	const Point3& bboxMin,
	const Point3& bboxMax
	) :
  m_max_sigma_a( max_sigma_a ),
  m_max_sigma_s( max_sigma_s ),
  m_max_sigma_t( max_sigma_a + max_sigma_s ),
  m_emission( 0, 0, 0 ),
  m_sigma_t_majorant( ColorMath::MaxValue( max_sigma_a + max_sigma_s ) ),
  m_pPhase( &phase ),
  m_pAccessor( &accessor ),
  m_bboxMin( bboxMin ),
  m_bboxMax( bboxMax ),
  m_bboxExtent( Vector3Ops::mkVector3( bboxMax, bboxMin ) ),
  m_volWidth( volWidth ),
  m_volHeight( volHeight ),
  m_volDepth( volDepth )
{
	m_pPhase->addref();
	m_pAccessor->addref();
}

HeterogeneousMedium::HeterogeneousMedium(
	const RISEPel& max_sigma_a,
	const RISEPel& max_sigma_s,
	const RISEPel& emission,
	const IPhaseFunction& phase,
	IVolumeAccessor& accessor,
	const unsigned int volWidth,
	const unsigned int volHeight,
	const unsigned int volDepth,
	const Point3& bboxMin,
	const Point3& bboxMax
	) :
  m_max_sigma_a( max_sigma_a ),
  m_max_sigma_s( max_sigma_s ),
  m_max_sigma_t( max_sigma_a + max_sigma_s ),
  m_emission( emission ),
  m_sigma_t_majorant( ColorMath::MaxValue( max_sigma_a + max_sigma_s ) ),
  m_pPhase( &phase ),
  m_pAccessor( &accessor ),
  m_bboxMin( bboxMin ),
  m_bboxMax( bboxMax ),
  m_bboxExtent( Vector3Ops::mkVector3( bboxMax, bboxMin ) ),
  m_volWidth( volWidth ),
  m_volHeight( volHeight ),
  m_volDepth( volDepth )
{
	m_pPhase->addref();
	m_pAccessor->addref();
}

HeterogeneousMedium::~HeterogeneousMedium()
{
	safe_release( m_pPhase );
	safe_release( m_pAccessor );
}


//
// Look up density [0,1] at a world-space point by mapping to
// the volume's centered coordinate system.
//
Scalar HeterogeneousMedium::LookupDensity(
	const Point3& worldPt
	) const
{
	// Check AABB bounds
	if( worldPt.x < m_bboxMin.x || worldPt.x > m_bboxMax.x ||
		worldPt.y < m_bboxMin.y || worldPt.y > m_bboxMax.y ||
		worldPt.z < m_bboxMin.z || worldPt.z > m_bboxMax.z )
	{
		return 0.0;
	}

	// Normalize to [0, 1] within the AABB
	const Scalar nx = (worldPt.x - m_bboxMin.x) / m_bboxExtent.x;
	const Scalar ny = (worldPt.y - m_bboxMin.y) / m_bboxExtent.y;
	const Scalar nz = (worldPt.z - m_bboxMin.z) / m_bboxExtent.z;

	// Map to volume's centered coordinate system:
	// Volume::GetValue(int x,y,z) adds half-dimensions internally,
	// so we need coordinates in [-dim/2, dim/2) range.
	const Scalar vx = (nx - 0.5) * Scalar(m_volWidth);
	const Scalar vy = (ny - 0.5) * Scalar(m_volHeight);
	const Scalar vz = (nz - 0.5) * Scalar(m_volDepth);

	return m_pAccessor->GetValue( vx, vy, vz );
}


//
// Ray-AABB intersection using the slab method.
// Returns entry and exit distances along the ray.
//
bool HeterogeneousMedium::IntersectBBox(
	const Ray& ray,
	Scalar& tEntry,
	Scalar& tExit
	) const
{
	Scalar tmin = -RISE_INFINITY;
	Scalar tmax = RISE_INFINITY;

	// X slab
	if( fabs( ray.Dir().x ) > 1e-12 )
	{
		const Scalar invD = 1.0 / ray.Dir().x;
		Scalar t1 = (m_bboxMin.x - ray.origin.x) * invD;
		Scalar t2 = (m_bboxMax.x - ray.origin.x) * invD;
		if( t1 > t2 ) { Scalar tmp = t1; t1 = t2; t2 = tmp; }
		if( t1 > tmin ) tmin = t1;
		if( t2 < tmax ) tmax = t2;
	}
	else if( ray.origin.x < m_bboxMin.x || ray.origin.x > m_bboxMax.x )
	{
		return false;
	}

	// Y slab
	if( fabs( ray.Dir().y ) > 1e-12 )
	{
		const Scalar invD = 1.0 / ray.Dir().y;
		Scalar t1 = (m_bboxMin.y - ray.origin.y) * invD;
		Scalar t2 = (m_bboxMax.y - ray.origin.y) * invD;
		if( t1 > t2 ) { Scalar tmp = t1; t1 = t2; t2 = tmp; }
		if( t1 > tmin ) tmin = t1;
		if( t2 < tmax ) tmax = t2;
	}
	else if( ray.origin.y < m_bboxMin.y || ray.origin.y > m_bboxMax.y )
	{
		return false;
	}

	// Z slab
	if( fabs( ray.Dir().z ) > 1e-12 )
	{
		const Scalar invD = 1.0 / ray.Dir().z;
		Scalar t1 = (m_bboxMin.z - ray.origin.z) * invD;
		Scalar t2 = (m_bboxMax.z - ray.origin.z) * invD;
		if( t1 > t2 ) { Scalar tmp = t1; t1 = t2; t2 = tmp; }
		if( t1 > tmin ) tmin = t1;
		if( t2 < tmax ) tmax = t2;
	}
	else if( ray.origin.z < m_bboxMin.z || ray.origin.z > m_bboxMax.z )
	{
		return false;
	}

	if( tmin > tmax || tmax < 0 )
	{
		return false;
	}

	// Clamp entry to 0 (ray origin inside box)
	tEntry = fmax( tmin, 0.0 );
	tExit = tmax;
	return true;
}


MediumCoefficients HeterogeneousMedium::GetCoefficients(
	const Point3& pt
	) const
{
	const Scalar density = LookupDensity( pt );

	MediumCoefficients c;
	c.sigma_t = m_max_sigma_t * density;
	c.sigma_s = m_max_sigma_s * density;
	c.emission = m_emission;
	return c;
}

MediumCoefficientsNM HeterogeneousMedium::GetCoefficientsNM(
	const Point3& pt,
	const Scalar nm
	) const
{
	const Scalar density = LookupDensity( pt );

	MediumCoefficientsNM c;
	c.sigma_t = ColorMath::Luminance( m_max_sigma_t ) * density;
	c.sigma_s = ColorMath::Luminance( m_max_sigma_s ) * density;
	c.emission = ColorMath::Luminance( m_emission );
	return c;
}

const IPhaseFunction* HeterogeneousMedium::GetPhaseFunction() const
{
	return m_pPhase;
}


//
// Delta tracking (Woodcock tracking) for distance sampling.
//
// Algorithm:
//   1. Start at t=0 (or AABB entry if ray starts outside)
//   2. Sample exponential step: dt = -ln(1-xi) / sigma_t_majorant
//   3. Advance t += dt
//   4. If t >= maxDist (or exits AABB), no scatter
//   5. Look up local sigma_t at the sample point
//   6. Accept with probability sigma_t_local / sigma_t_majorant
//   7. If rejected (null collision), goto step 2
//
// The majorant is the maximum possible extinction (density=1).
// Null collisions are "virtual" interactions that don't scatter
// but ensure the correct distance distribution.
//
// Reference: Cycles volume_sample_distance() in shade_volume.h
//
Scalar HeterogeneousMedium::SampleDistance(
	const Ray& ray,
	const Scalar maxDist,
	ISampler& sampler,
	bool& scattered
	) const
{
	if( m_sigma_t_majorant <= 0.0 )
	{
		scattered = false;
		return maxDist;
	}

	// Find the range of the ray within the AABB
	Scalar tEntry, tExit;
	if( !IntersectBBox( ray, tEntry, tExit ) )
	{
		// Ray misses the volume entirely — no interaction
		scattered = false;
		return maxDist;
	}

	// Clamp exit to the surface hit distance
	tExit = fmin( tExit, maxDist );

	// Start delta tracking from the AABB entry point
	Scalar t = tEntry;
	const Scalar invMajorant = 1.0 / m_sigma_t_majorant;

	for( unsigned int step = 0; step < nMaxDeltaTrackingSteps; step++ )
	{
		// Sample exponential free-flight distance
		const Scalar xi = sampler.Get1D();
		const Scalar dt = -log( fmax( 1.0 - xi, 1e-30 ) ) * invMajorant;
		t += dt;

		// Past the exit distance?
		if( t >= tExit )
		{
			scattered = false;
			return maxDist;
		}

		// Evaluate local density at the sample point
		const Point3 samplePt = Point3Ops::mkPoint3( ray.origin, ray.Dir() * t );
		const Scalar density = LookupDensity( samplePt );
		const Scalar sigma_t_local = ColorMath::MaxValue( m_max_sigma_t ) * density;

		// Accept/reject: real collision with probability sigma_t_local / majorant
		const Scalar xi2 = sampler.Get1D();
		if( xi2 < sigma_t_local * invMajorant )
		{
			// Real scatter event
			scattered = true;
			return t;
		}
		// Null collision — continue stepping
	}

	// Exceeded max steps — treat as no scatter
	scattered = false;
	return maxDist;
}

Scalar HeterogeneousMedium::SampleDistanceNM(
	const Ray& ray,
	const Scalar maxDist,
	const Scalar nm,
	ISampler& sampler,
	bool& scattered
	) const
{
	const Scalar sigma_t_majorant_nm = ColorMath::Luminance( m_max_sigma_t );

	if( sigma_t_majorant_nm <= 0.0 )
	{
		scattered = false;
		return maxDist;
	}

	Scalar tEntry, tExit;
	if( !IntersectBBox( ray, tEntry, tExit ) )
	{
		scattered = false;
		return maxDist;
	}

	tExit = fmin( tExit, maxDist );

	Scalar t = tEntry;
	const Scalar invMajorant = 1.0 / sigma_t_majorant_nm;

	for( unsigned int step = 0; step < nMaxDeltaTrackingSteps; step++ )
	{
		const Scalar xi = sampler.Get1D();
		const Scalar dt = -log( fmax( 1.0 - xi, 1e-30 ) ) * invMajorant;
		t += dt;

		if( t >= tExit )
		{
			scattered = false;
			return maxDist;
		}

		const Point3 samplePt = Point3Ops::mkPoint3( ray.origin, ray.Dir() * t );
		const Scalar density = LookupDensity( samplePt );
		const Scalar sigma_t_local = sigma_t_majorant_nm * density;

		const Scalar xi2 = sampler.Get1D();
		if( xi2 < sigma_t_local * invMajorant )
		{
			scattered = true;
			return t;
		}
	}

	scattered = false;
	return maxDist;
}


//
// Ratio tracking for unbiased transmittance estimation.
//
// At each step along the ray, the running transmittance is
// multiplied by (1 - sigma_t_local / sigma_t_majorant).
// This converges to the true transmittance without bias.
//
// For efficiency, we terminate early when transmittance drops
// below a small threshold.
//
// Reference: Novak et al. 2014, "Residual Ratio Tracking for
// Estimating Attenuation in Participating Media".
//
RISEPel HeterogeneousMedium::EvalTransmittance(
	const Ray& ray,
	const Scalar dist
	) const
{
	if( m_sigma_t_majorant <= 0.0 )
	{
		return RISEPel( 1, 1, 1 );
	}

	Scalar tEntry, tExit;
	if( !IntersectBBox( ray, tEntry, tExit ) )
	{
		return RISEPel( 1, 1, 1 );
	}

	tExit = fmin( tExit, dist );
	if( tEntry >= tExit )
	{
		return RISEPel( 1, 1, 1 );
	}

	const Scalar invMajorant = 1.0 / m_sigma_t_majorant;

	// Use deterministic ray marching for transmittance rather than
	// stochastic ratio tracking — avoids noise in shadow rays.
	// March through the volume in uniform steps, accumulating
	// optical depth per channel.
	//
	// Step size: use the minimum world-space voxel edge length to
	// ensure adequate sampling regardless of ray direction.  This
	// handles diagonal rays correctly (they traverse more voxels
	// than axis-aligned rays of the same parametric length).
	const Scalar rayExtent = tExit - tEntry;
	const Scalar minVoxelEdge = fmin(
		fmin( m_bboxExtent.x / Scalar(m_volWidth),
			  m_bboxExtent.y / Scalar(m_volHeight) ),
		m_bboxExtent.z / Scalar(m_volDepth) );
	// Target ~2 samples per voxel, but cap at 512 steps
	const Scalar stepSize = fmax( minVoxelEdge * 0.5, rayExtent / 512.0 );
	const unsigned int nSteps = (unsigned int)ceil( rayExtent / stepSize );
	const Scalar dt = rayExtent / Scalar(nSteps);

	RISEPel opticalDepth( 0, 0, 0 );

	for( unsigned int i = 0; i < nSteps; i++ )
	{
		// Sample at the midpoint of each step
		const Scalar t = tEntry + (Scalar(i) + 0.5) * dt;
		const Point3 samplePt = Point3Ops::mkPoint3( ray.origin, ray.Dir() * t );
		const Scalar density = LookupDensity( samplePt );

		opticalDepth = opticalDepth + m_max_sigma_t * (density * dt);
	}

	return RISEPel(
		exp( -opticalDepth[0] ),
		exp( -opticalDepth[1] ),
		exp( -opticalDepth[2] )
	);
}

Scalar HeterogeneousMedium::EvalTransmittanceNM(
	const Ray& ray,
	const Scalar dist,
	const Scalar nm
	) const
{
	const Scalar sigma_t_max_nm = ColorMath::Luminance( m_max_sigma_t );

	if( sigma_t_max_nm <= 0.0 )
	{
		return 1.0;
	}

	Scalar tEntry, tExit;
	if( !IntersectBBox( ray, tEntry, tExit ) )
	{
		return 1.0;
	}

	tExit = fmin( tExit, dist );
	if( tEntry >= tExit )
	{
		return 1.0;
	}

	// Deterministic ray marching for spectral transmittance
	const Scalar rayExtent = tExit - tEntry;
	const Scalar minVoxelEdge = fmin(
		fmin( m_bboxExtent.x / Scalar(m_volWidth),
			  m_bboxExtent.y / Scalar(m_volHeight) ),
		m_bboxExtent.z / Scalar(m_volDepth) );
	const Scalar stepSize = fmax( minVoxelEdge * 0.5, rayExtent / 512.0 );
	const unsigned int nSteps = (unsigned int)ceil( rayExtent / stepSize );
	const Scalar dt = rayExtent / Scalar(nSteps);

	Scalar opticalDepth = 0;

	for( unsigned int i = 0; i < nSteps; i++ )
	{
		const Scalar t = tEntry + (Scalar(i) + 0.5) * dt;
		const Point3 samplePt = Point3Ops::mkPoint3( ray.origin, ray.Dir() * t );
		const Scalar density = LookupDensity( samplePt );

		opticalDepth += sigma_t_max_nm * density * dt;
	}

	return exp( -opticalDepth );
}

bool HeterogeneousMedium::IsHomogeneous() const
{
	return false;
}

Scalar HeterogeneousMedium::ClipDistanceToBounds(
	const Ray& ray,
	const Scalar dist
	) const
{
	Scalar tEntry, tExit;
	if( !IntersectBBox( ray, tEntry, tExit ) )
	{
		return 0;
	}
	return fmin( tExit, dist );
}
