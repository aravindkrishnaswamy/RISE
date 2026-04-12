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
#include "../Utilities/RandomNumbers.h"
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

	// Build the majorant grid for DDA-based delta/ratio tracking
	unsigned int gridX, gridY, gridZ;
	MajorantGrid::DefaultGridResolution( volWidth, volHeight, volDepth,
		gridX, gridY, gridZ );
	m_pMajorantGrid = new MajorantGrid(
		accessor, volWidth, volHeight, volDepth,
		bboxMin, bboxMax, m_sigma_t_majorant,
		gridX, gridY, gridZ );
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

	// Build the majorant grid for DDA-based delta/ratio tracking
	unsigned int gridX, gridY, gridZ;
	MajorantGrid::DefaultGridResolution( volWidth, volHeight, volDepth,
		gridX, gridY, gridZ );
	m_pMajorantGrid = new MajorantGrid(
		accessor, volWidth, volHeight, volDepth,
		bboxMin, bboxMax, m_sigma_t_majorant,
		gridX, gridY, gridZ );
}

HeterogeneousMedium::~HeterogeneousMedium()
{
	delete m_pMajorantGrid;
	m_pMajorantGrid = 0;
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

	// DDA-based delta tracking: walk through the majorant grid
	// cell by cell, using each cell's local majorant instead of
	// the global majorant.  This dramatically reduces null
	// collisions in volumes with spatially varying density.
	//
	// At each cell boundary we restart exponential sampling with
	// the new cell's majorant (standard approach, matches pbrt-v4).
	// This avoids numerical issues with residual rescaling.

	// Capture data needed by the DDA visitor
	const HeterogeneousMedium* self = this;
	const Ray& rayRef = ray;
	ISampler& samplerRef = sampler;
	const Scalar sigma_t_max_channel = ColorMath::MaxValue( m_max_sigma_t );
	Scalar scatterDist = 0;
	bool didScatter = false;
	unsigned int totalSteps = 0;

	struct DeltaTrackingVisitor
	{
		const HeterogeneousMedium* self;
		const Ray* pRay;
		ISampler* pSampler;
		Scalar sigma_t_max_channel;
		Scalar* pScatterDist;
		bool* pDidScatter;
		unsigned int* pTotalSteps;

		bool operator()( Scalar tCellEntry, Scalar tCellExit, Scalar cellMajorant )
		{
			// Skip empty cells (zero majorant)
			if( cellMajorant <= 0 )
				return true;  // Continue to next cell

			const Scalar invCellMaj = 1.0 / cellMajorant;
			Scalar t = tCellEntry;

			while( *pTotalSteps < nMaxDeltaTrackingSteps )
			{
				(*pTotalSteps)++;

				// Sample exponential free-flight distance with local majorant
				const Scalar xi = pSampler->Get1D();
				const Scalar dt = -log( fmax( 1.0 - xi, 1e-30 ) ) * invCellMaj;
				t += dt;

				// Past cell exit? Move to next cell
				if( t >= tCellExit )
					return true;

				// Evaluate local density at sample point
				const Point3 samplePt = Point3Ops::mkPoint3(
					pRay->origin, pRay->Dir() * t );
				const Scalar density = self->LookupDensity( samplePt );
				const Scalar sigma_t_local = sigma_t_max_channel * density;

				// Accept/reject with local majorant
				const Scalar xi2 = pSampler->Get1D();
				if( xi2 < sigma_t_local * invCellMaj )
				{
					// Real scatter event
					*pScatterDist = t;
					*pDidScatter = true;
					return false;  // Stop traversal
				}
				// Null collision — continue
			}
			return false;  // Exceeded max steps, stop
		}
	};

	DeltaTrackingVisitor visitor;
	visitor.self = self;
	visitor.pRay = &rayRef;
	visitor.pSampler = &samplerRef;
	visitor.sigma_t_max_channel = sigma_t_max_channel;
	visitor.pScatterDist = &scatterDist;
	visitor.pDidScatter = &didScatter;
	visitor.pTotalSteps = &totalSteps;

	m_pMajorantGrid->TraverseRay( ray, 0.0, maxDist, visitor );

	if( didScatter )
	{
		scattered = true;
		return scatterDist;
	}

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

	// DDA-based delta tracking (spectral variant).
	// Uses the same majorant grid as the RGB path.  The spectral
	// majorant per cell is scaled by the ratio of the spectral
	// to RGB majorant so that accept/reject uses the correct
	// spectral extinction.
	const Scalar majorantRatio = (m_sigma_t_majorant > 0)
		? sigma_t_majorant_nm / m_sigma_t_majorant : 1.0;

	const HeterogeneousMedium* self = this;
	const Ray& rayRef = ray;
	ISampler& samplerRef = sampler;
	Scalar scatterDist = 0;
	bool didScatter = false;
	unsigned int totalSteps = 0;

	struct DeltaTrackingVisitorNM
	{
		const HeterogeneousMedium* self;
		const Ray* pRay;
		ISampler* pSampler;
		Scalar sigma_t_majorant_nm;
		Scalar majorantRatio;
		Scalar* pScatterDist;
		bool* pDidScatter;
		unsigned int* pTotalSteps;

		bool operator()( Scalar tCellEntry, Scalar tCellExit, Scalar cellMajorant )
		{
			// Scale the RGB cell majorant to spectral
			const Scalar cellMajNM = cellMajorant * majorantRatio;
			if( cellMajNM <= 0 )
				return true;

			const Scalar invCellMaj = 1.0 / cellMajNM;
			Scalar t = tCellEntry;

			while( *pTotalSteps < nMaxDeltaTrackingSteps )
			{
				(*pTotalSteps)++;

				const Scalar xi = pSampler->Get1D();
				const Scalar dt = -log( fmax( 1.0 - xi, 1e-30 ) ) * invCellMaj;
				t += dt;

				if( t >= tCellExit )
					return true;

				const Point3 samplePt = Point3Ops::mkPoint3(
					pRay->origin, pRay->Dir() * t );
				const Scalar density = self->LookupDensity( samplePt );
				const Scalar sigma_t_local = sigma_t_majorant_nm * density;

				const Scalar xi2 = pSampler->Get1D();
				if( xi2 < sigma_t_local * invCellMaj )
				{
					*pScatterDist = t;
					*pDidScatter = true;
					return false;
				}
			}
			return false;
		}
	};

	DeltaTrackingVisitorNM visitor;
	visitor.self = self;
	visitor.pRay = &rayRef;
	visitor.pSampler = &samplerRef;
	visitor.sigma_t_majorant_nm = sigma_t_majorant_nm;
	visitor.majorantRatio = majorantRatio;
	visitor.pScatterDist = &scatterDist;
	visitor.pDidScatter = &didScatter;
	visitor.pTotalSteps = &totalSteps;

	m_pMajorantGrid->TraverseRay( ray, 0.0, maxDist, visitor );

	if( didScatter )
	{
		scattered = true;
		return scatterDist;
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

	// Ratio tracking with DDA-based majorant grid.
	//
	// For each cell traversed by the ray, we sample exponential
	// steps using the cell's local majorant.  At each sample point
	// the running per-channel transmittance weight is multiplied by:
	//   w[ch] *= 1 - sigma_t[ch] * density / cell_majorant
	//
	// This produces an unbiased estimate of the true per-channel
	// transmittance.  Using local majorants from the grid instead
	// of the global majorant ensures the ratio stays close to 1
	// in low-density cells, reducing variance.
	//
	// Reference: Novak et al. 2014, "Residual Ratio Tracking for
	// Estimating Attenuation in Participating Media".

	// Thread-local RNG for stochastic sampling.
	// Each thread gets its own Mersenne Twister to avoid contention.
	static thread_local RandomNumberGenerator tl_rng;

	RISEPel w( 1, 1, 1 );
	const HeterogeneousMedium* self = this;
	unsigned int totalSteps = 0;

	struct RatioTrackingVisitor
	{
		const HeterogeneousMedium* self;
		const Ray* pRay;
		RandomNumberGenerator* pRng;
		RISEPel* pW;
		RISEPel max_sigma_t;
		unsigned int* pTotalSteps;

		bool operator()( Scalar tCellEntry, Scalar tCellExit, Scalar cellMajorant )
		{
			if( cellMajorant <= 0 )
				return true;  // Skip empty cells

			const Scalar invCellMaj = 1.0 / cellMajorant;
			Scalar t = tCellEntry;

			while( *pTotalSteps < nMaxDeltaTrackingSteps )
			{
				(*pTotalSteps)++;

				const Scalar xi = pRng->CanonicalRandom();
				const Scalar dt = -log( fmax( 1.0 - xi, 1e-30 ) ) * invCellMaj;
				t += dt;

				if( t >= tCellExit )
					return true;  // Next cell

				const Point3 samplePt = Point3Ops::mkPoint3(
					pRay->origin, pRay->Dir() * t );
				const Scalar density = self->LookupDensity( samplePt );

				// Per-channel ratio tracking weight
				for( int ch = 0; ch < 3; ch++ )
				{
					const Scalar sigma_t_ch = max_sigma_t[ch] * density;
					(*pW)[ch] *= fmax( 0.0, 1.0 - sigma_t_ch * invCellMaj );
				}

				// Russian roulette for low transmittance.
				// Unlike a hard cutoff (which biases toward zero), RR
				// is unbiased: survivors are scaled by 1/pSurvive to
				// compensate for killed paths.
				const Scalar wMax = ColorMath::MaxValue( *pW );
				if( wMax < 0.1 )
				{
					const Scalar pSurvive = fmax( wMax, 1e-6 );
					if( pRng->CanonicalRandom() >= pSurvive )
					{
						*pW = RISEPel( 0, 0, 0 );
						return false;
					}
					*pW = *pW * (1.0 / pSurvive);
				}
			}
			return false;
		}
	};

	RatioTrackingVisitor visitor;
	visitor.self = self;
	visitor.pRay = &ray;
	visitor.pRng = &tl_rng;
	visitor.pW = &w;
	visitor.max_sigma_t = m_max_sigma_t;
	visitor.pTotalSteps = &totalSteps;

	m_pMajorantGrid->TraverseRay( ray, 0.0, dist, visitor );

	return w;
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

	// Ratio tracking (spectral variant) with DDA-based majorant grid.
	// Uses the RGB majorant grid scaled by the spectral/RGB ratio.
	const Scalar majorantRatio = (m_sigma_t_majorant > 0)
		? sigma_t_max_nm / m_sigma_t_majorant : 1.0;

	static thread_local RandomNumberGenerator tl_rng_nm;

	Scalar w = 1.0;
	const HeterogeneousMedium* self = this;
	unsigned int totalSteps = 0;

	struct RatioTrackingVisitorNM
	{
		const HeterogeneousMedium* self;
		const Ray* pRay;
		RandomNumberGenerator* pRng;
		Scalar* pW;
		Scalar sigma_t_max_nm;
		Scalar majorantRatio;
		unsigned int* pTotalSteps;

		bool operator()( Scalar tCellEntry, Scalar tCellExit, Scalar cellMajorant )
		{
			const Scalar cellMajNM = cellMajorant * majorantRatio;
			if( cellMajNM <= 0 )
				return true;

			const Scalar invCellMaj = 1.0 / cellMajNM;
			Scalar t = tCellEntry;

			while( *pTotalSteps < nMaxDeltaTrackingSteps )
			{
				(*pTotalSteps)++;

				const Scalar xi = pRng->CanonicalRandom();
				const Scalar dt = -log( fmax( 1.0 - xi, 1e-30 ) ) * invCellMaj;
				t += dt;

				if( t >= tCellExit )
					return true;

				const Point3 samplePt = Point3Ops::mkPoint3(
					pRay->origin, pRay->Dir() * t );
				const Scalar density = self->LookupDensity( samplePt );
				const Scalar sigma_t_local = sigma_t_max_nm * density;

				*pW *= fmax( 0.0, 1.0 - sigma_t_local * invCellMaj );

				// Russian roulette (see RGB variant for rationale)
				if( *pW < 0.1 )
				{
					const Scalar pSurvive = fmax( *pW, 1e-6 );
					if( pRng->CanonicalRandom() >= pSurvive )
					{
						*pW = 0;
						return false;
					}
					*pW = *pW * (1.0 / pSurvive);
				}
			}
			return false;
		}
	};

	RatioTrackingVisitorNM visitor;
	visitor.self = self;
	visitor.pRay = &ray;
	visitor.pRng = &tl_rng_nm;
	visitor.pW = &w;
	visitor.sigma_t_max_nm = sigma_t_max_nm;
	visitor.majorantRatio = majorantRatio;
	visitor.pTotalSteps = &totalSteps;

	m_pMajorantGrid->TraverseRay( ray, 0.0, dist, visitor );

	return w;
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


IMedium::DistanceSample HeterogeneousMedium::SampleDistanceWithPdf(
	const Ray& ray,
	const Scalar maxDist,
	ISampler& sampler
	) const
{
	DistanceSample ds;
	ds.t = SampleDistance( ray, maxDist, sampler, ds.scattered );
	ds.pdf = EvalDistancePdf( ray, ds.t, ds.scattered, maxDist );
	if( ds.pdf < 1e-30 ) ds.pdf = 1e-30;
	return ds;
}

IMedium::DistanceSample HeterogeneousMedium::SampleDistanceWithPdfNM(
	const Ray& ray,
	const Scalar maxDist,
	const Scalar nm,
	ISampler& sampler
	) const
{
	DistanceSample ds;
	ds.t = SampleDistanceNM( ray, maxDist, nm, sampler, ds.scattered );
	ds.pdf = EvalDistancePdfNM( ray, ds.t, ds.scattered, maxDist, nm );
	if( ds.pdf < 1e-30 ) ds.pdf = 1e-30;
	return ds;
}

Scalar HeterogeneousMedium::EvalDeterministicOpticalDepth(
	const Ray& ray,
	const Scalar targetDist,
	const Scalar sigma_t_eff
	) const
{
	// Deterministic optical depth via voxel-lattice DDA + Gauss-
	// Legendre quadrature.
	//
	// Amanatides-Woo DDA traversal splits the ray at every
	// accessor knot plane — the world-space positions where the
	// interpolation stencil changes.  All three supported accessors
	// (NNB, trilinear, tricubic Catmull-Rom) switch stencils at
	// integer accessor coordinates.  Within each interval between
	// consecutive knot planes, 5-point Gauss-Legendre quadrature
	// integrates the density exactly:
	//
	//   Nearest-neighbor ('n'): piecewise constant (degree 0)
	//   Trilinear ('t'):  cubic along ray per cell (degree 3)
	//   Tricubic Catmull-Rom ('c'):  degree 9 along ray per cell
	//     (3D tensor product of cubics, each axis linear in t)
	//
	// 5-point GL is exact for polynomials up to degree 2*5-1 = 9.
	//
	// COORDINATE MAPPING:
	//   LookupDensity maps world → accessor via:
	//     vx = ((world_x - bboxMin.x) / extent.x - 0.5) * volWidth
	//   Knot planes are at integer vx = n.  Solving for world_x:
	//     world_x = (n/volWidth + 0.5) * extent.x + bboxMin.x
	//             = (n + volWidth/2.0) * cellSzX + bboxMin.x
	//   For even volWidth, this coincides with bboxMin + k*cellSz.
	//   For odd volWidth, there is a half-voxel offset; the DDA
	//   must use the knot planes, not the naively subdivided AABB.
	//
	// DDA cell indices are in accessor space (floor of accessor
	// coordinate), not 0-based voxel indices.
	//
	// Fully deterministic: same (ray, targetDist) always returns
	// the same value.

	// Intersect ray with the volume AABB
	Scalar tEntry = 0, tExit = 0;
	if( !IntersectBBox( ray, tEntry, tExit ) )
		return 0;

	tEntry = fmax( tEntry, 0.0 );
	tExit = fmin( tExit, targetDist );
	if( tEntry >= tExit )
		return 0;

	// Voxel cell size in world space (same as before — the spacing
	// between knot planes equals extent/volDim regardless of parity)
	const Scalar cellSzX = m_bboxExtent.x / Scalar(m_volWidth);
	const Scalar cellSzY = m_bboxExtent.y / Scalar(m_volHeight);
	const Scalar cellSzZ = m_bboxExtent.z / Scalar(m_volDepth);
	const Scalar invCellSzX = Scalar(m_volWidth)  / m_bboxExtent.x;
	const Scalar invCellSzY = Scalar(m_volHeight) / m_bboxExtent.y;
	const Scalar invCellSzZ = Scalar(m_volDepth)  / m_bboxExtent.z;

	// Accessor-space half-widths for the centered coordinate mapping
	const Scalar halfW = Scalar(m_volWidth)  * 0.5;
	const Scalar halfH = Scalar(m_volHeight) * 0.5;
	const Scalar halfD = Scalar(m_volDepth)  * 0.5;

	// Starting point (nudge slightly inside to avoid landing on face)
	const Point3 startPt = ray.PointAtLength( tEntry + 1e-10 );

	// Map start point to accessor coordinates and take floor to get
	// the cell index.  Accessor coord:
	//   vx = (world_x - bboxMin.x) * invCellSzX - halfW
	// Cell index = floor(vx).
	int cx = (int)floor( (startPt.x - m_bboxMin.x) * invCellSzX - halfW );
	int cy = (int)floor( (startPt.y - m_bboxMin.y) * invCellSzY - halfH );
	int cz = (int)floor( (startPt.z - m_bboxMin.z) * invCellSzZ - halfD );

	// Valid accessor-space cell range: floor(-halfW) .. ceil(halfW)-1
	// (the AABB spans accessor coords [-halfW, halfW])
	const int minCx = (int)floor( -halfW );
	const int maxCx = (int)ceil( halfW ) - 1;
	const int minCy = (int)floor( -halfH );
	const int maxCy = (int)ceil( halfH ) - 1;
	const int minCz = (int)floor( -halfD );
	const int maxCz = (int)ceil( halfD ) - 1;

	if( cx < minCx ) cx = minCx;  if( cx > maxCx ) cx = maxCx;
	if( cy < minCy ) cy = minCy;  if( cy > maxCy ) cy = maxCy;
	if( cz < minCz ) cz = minCz;  if( cz > maxCz ) cz = maxCz;

	// DDA step directions
	const int stepX = (ray.Dir().x >= 0) ? 1 : -1;
	const int stepY = (ray.Dir().y >= 0) ? 1 : -1;
	const int stepZ = (ray.Dir().z >= 0) ? 1 : -1;

	// tMax: ray parameter to reach the next knot plane in each axis.
	// Knot plane at accessor integer n maps to world:
	//   world_x = (n + halfW) * cellSzX + bboxMin.x
	// Next face from cell cx in +x direction: n = cx + 1
	// Next face from cell cx in -x direction: n = cx
	Scalar tMaxX, tMaxY, tMaxZ;
	Scalar tDeltaX, tDeltaY, tDeltaZ;

	if( fabs( ray.Dir().x ) > 1e-20 )
	{
		const int nextN = (stepX > 0) ? cx + 1 : cx;
		const Scalar nextFaceX = m_bboxMin.x + (Scalar(nextN) + halfW) * cellSzX;
		tMaxX = (nextFaceX - ray.origin.x) / ray.Dir().x;
		tDeltaX = fabs( cellSzX / ray.Dir().x );
	}
	else
	{
		tMaxX = RISE_INFINITY;
		tDeltaX = RISE_INFINITY;
	}

	if( fabs( ray.Dir().y ) > 1e-20 )
	{
		const int nextN = (stepY > 0) ? cy + 1 : cy;
		const Scalar nextFaceY = m_bboxMin.y + (Scalar(nextN) + halfH) * cellSzY;
		tMaxY = (nextFaceY - ray.origin.y) / ray.Dir().y;
		tDeltaY = fabs( cellSzY / ray.Dir().y );
	}
	else
	{
		tMaxY = RISE_INFINITY;
		tDeltaY = RISE_INFINITY;
	}

	if( fabs( ray.Dir().z ) > 1e-20 )
	{
		const int nextN = (stepZ > 0) ? cz + 1 : cz;
		const Scalar nextFaceZ = m_bboxMin.z + (Scalar(nextN) + halfD) * cellSzZ;
		tMaxZ = (nextFaceZ - ray.origin.z) / ray.Dir().z;
		tDeltaZ = fabs( cellSzZ / ray.Dir().z );
	}
	else
	{
		tMaxZ = RISE_INFINITY;
		tDeltaZ = RISE_INFINITY;
	}

	// Walk the voxel lattice along accessor knot planes
	Scalar opticalDepth = 0;
	Scalar t = tEntry;

	while( t < tExit )
	{
		// Next knot-plane crossing or segment end
		Scalar tNext = fmin( fmin( tMaxX, tMaxY ), tMaxZ );
		tNext = fmin( tNext, tExit );

		if( tNext > t + 1e-15 )
		{
			// 5-point Gauss-Legendre: exact for polynomials up to
			// degree 9, covering all supported accessor types.
			//
			// Nodes on [-1,1]:  ±0.90618, ±0.53847, 0
			// Weights:          0.23693,  0.47863,  0.56889
			static const Scalar glNodes[5] = {
				-0.906179845938664, -0.538469310105683, 0.0,
				 0.538469310105683,  0.906179845938664 };
			static const Scalar glWeights[5] = {
				0.236926885056189, 0.478628670499366, 0.568888888888889,
				0.478628670499366, 0.236926885056189 };

			const Scalar halfLen = 0.5 * (tNext - t);
			const Scalar midPt  = 0.5 * (t + tNext);
			Scalar segIntegral = 0;
			for( int q = 0; q < 5; q++ )
			{
				const Scalar tq = midPt + halfLen * glNodes[q];
				const Scalar dq = LookupDensity(
					Point3Ops::mkPoint3( ray.origin, ray.Dir() * tq ) );
				segIntegral += glWeights[q] * dq;
			}
			opticalDepth += sigma_t_eff * halfLen * segIntegral;
		}

		// Advance DDA: step the axis with the smallest tMax
		if( tMaxX <= tMaxY && tMaxX <= tMaxZ )
		{
			cx += stepX;
			tMaxX += tDeltaX;
		}
		else if( tMaxY <= tMaxZ )
		{
			cy += stepY;
			tMaxY += tDeltaY;
		}
		else
		{
			cz += stepZ;
			tMaxZ += tDeltaZ;
		}

		t = tNext;

		// Out of accessor-space voxel range?  (Safety bound with
		// 1-cell margin; the tExit clamp is the true limiter.)
		if( cx < minCx - 1 || cx > maxCx + 1 ||
			cy < minCy - 1 || cy > maxCy + 1 ||
			cz < minCz - 1 || cz > maxCz + 1 )
			break;
	}

	return opticalDepth;
}


Scalar HeterogeneousMedium::EvalDistancePdf(
	const Ray& ray,
	const Scalar t,
	const bool scattered,
	const Scalar maxDist
	) const
{
	// Deterministic technique density for MIS weights.
	//
	// The true marginal PDF of delta tracking is:
	//   scatter:    sigma_t(t) * T_real(0,t)
	//   no scatter: T_real(0,tEnd)
	// where T_real = exp(-integral sigma_t ds) is the real transmittance.
	//
	// We compute T_real deterministically via Simpson quadrature over
	// the DDA cells (see EvalDeterministicOpticalDepth).  This avoids
	// the stochastic ratio-tracking path through EvalTransmittance,
	// which would randomize the MIS balance-heuristic denominator.
	//
	// sigma_t_eff is the scalar extinction used by the delta tracking
	// sampler: MaxValue(m_max_sigma_t) for RGB.
	//
	// Reference: Miller, Georgiev, Jarosz, SIGGRAPH 2019 §3.3
	const Scalar sigma_t_eff = m_sigma_t_majorant;  // = MaxValue(m_max_sigma_t)
	const Scalar targetDist = scattered ? t : maxDist;
	const Scalar tau = EvalDeterministicOpticalDepth( ray, targetDist, sigma_t_eff );
	const Scalar T_real = exp( -tau );

	if( scattered )
	{
		const Point3 pt = ray.PointAtLength( t );
		const Scalar density = LookupDensity( pt );
		return fmax( sigma_t_eff * density * T_real, 1e-30 );
	}
	else
	{
		return fmax( T_real, 1e-30 );
	}
}

Scalar HeterogeneousMedium::EvalDistancePdfNM(
	const Ray& ray,
	const Scalar t,
	const bool scattered,
	const Scalar maxDist,
	const Scalar nm
	) const
{
	// Deterministic technique density — see EvalDistancePdf comment.
	// sigma_t_eff for NM is Luminance(m_max_sigma_t), matching
	// the scalar majorant used by SampleDistanceNM.
	const Scalar sigma_t_eff = ColorMath::Luminance( m_max_sigma_t );
	const Scalar targetDist = scattered ? t : maxDist;
	const Scalar tau = EvalDeterministicOpticalDepth( ray, targetDist, sigma_t_eff );
	const Scalar T_real = exp( -tau );

	if( scattered )
	{
		const Point3 pt = ray.PointAtLength( t );
		const Scalar density = LookupDensity( pt );
		return fmax( sigma_t_eff * density * T_real, 1e-30 );
	}
	else
	{
		return fmax( T_real, 1e-30 );
	}
}

bool HeterogeneousMedium::GetBoundingBox(
	Point3& bbMin,
	Point3& bbMax
	) const
{
	bbMin = m_bboxMin;
	bbMax = m_bboxMax;
	return true;
}
