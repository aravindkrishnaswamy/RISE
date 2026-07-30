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
#include "HenyeyGreensteinPhaseFunction.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Utilities/FiniteMath.h"
#include "../Utilities/PlanckRadiance.h"
#include "../Utilities/GaussLegendreQuadrature.h"
#include "../Utilities/RandomNumbers.h"
#include "../Volume/Volume.h"
#include "../Volume/VolumeAccessor_TRI.h"
#include <cmath>
#include <math.h>

using namespace RISE;

namespace
{
	static unsigned int HalfOpenBinIndex(
		const Scalar value,
		const Scalar minimum,
		const Scalar maximum,
		const Scalar binSize,
		const unsigned int binCount
		)
	{
		if( value >= maximum ) return binCount - 1u;
		unsigned int lower = 0u;
		unsigned int upper = binCount;
		while( lower + 1u < upper ) {
			const unsigned int middle = lower + (upper-lower)/2u;
			const Scalar boundary = minimum + Scalar(middle)*binSize;
			if( value < boundary ) upper = middle;
			else lower = middle;
		}
		return lower;
	}

	static Scalar SampleHalfOpenBinCoordinate(
		const Scalar minimum,
		const Scalar maximum,
		const Scalar binSize,
		const unsigned int binIndex,
		const unsigned int binCount,
		const Scalar sample
		)
	{
		const Scalar lower = minimum + Scalar(binIndex)*binSize;
		const Scalar upper = binIndex + 1u == binCount ? maximum :
			minimum + Scalar(binIndex+1u)*binSize;
		Scalar coordinate = lower + sample*(upper-lower);
		if( coordinate < lower ) coordinate = lower;
		if( binIndex + 1u < binCount && coordinate >= upper ) {
			coordinate = std::nextafter( upper, lower );
		}
		if( coordinate > maximum ) coordinate = maximum;
		return coordinate;
	}

	static bool HasRepresentableBinInteriors(
		const Scalar minimum,
		const Scalar maximum,
		const Scalar binSize,
		const unsigned int binCount
		)
	{
		Scalar previous = minimum;
		for( unsigned int i = 1u; i <= binCount; ++i ) {
			const Scalar boundary = i == binCount ? maximum :
				minimum + Scalar(i)*binSize;
			if( !(boundary > previous) ||
				!(std::nextafter(previous,boundary) < boundary) ) return false;
			previous = boundary;
		}
		return true;
	}

	static Scalar RepresentedBinVolume(
		const Point3& minimum,
		const Point3& maximum,
		const Vector3& binSize,
		const unsigned int x,
		const unsigned int y,
		const unsigned int z,
		const unsigned int nx,
		const unsigned int ny,
		const unsigned int nz
		)
	{
		const Scalar x0 = minimum.x + Scalar(x)*binSize.x;
		const Scalar y0 = minimum.y + Scalar(y)*binSize.y;
		const Scalar z0 = minimum.z + Scalar(z)*binSize.z;
		const Scalar x1 = x + 1u == nx ? maximum.x :
			minimum.x + Scalar(x+1u)*binSize.x;
		const Scalar y1 = y + 1u == ny ? maximum.y :
			minimum.y + Scalar(y+1u)*binSize.y;
		const Scalar z1 = z + 1u == nz ? maximum.z :
			minimum.z + Scalar(z+1u)*binSize.z;
		return (x1-x0)*(y1-y0)*(z1-z0);
	}

	class ConstituentHGPhaseClosure :
		public virtual IPhaseFunction,
		public virtual Implementation::Reference
	{
		const Scalar m_hotWeight;
		const Scalar m_coolWeight;
		const Scalar m_hotG;
		const Scalar m_coolG;

	protected:
		~ConstituentHGPhaseClosure() override = default;

	public:
		ConstituentHGPhaseClosure(
			const Scalar hotScattering,
			const Scalar coolScattering,
			const Scalar hotG,
			const Scalar coolG
			) :
		  m_hotWeight( hotScattering / (hotScattering + coolScattering) ),
		  m_coolWeight( coolScattering / (hotScattering + coolScattering) ),
		  m_hotG( hotG ),
		  m_coolG( coolG )
		{
		}

		Scalar Evaluate( const Vector3& wi, const Vector3& wo ) const override
		{
			const Scalar cosTheta = Vector3Ops::Dot( wi, wo );
			return m_hotWeight * HenyeyGreensteinPhaseFunction::EvaluateWithG(
				cosTheta, m_hotG ) +
				m_coolWeight * HenyeyGreensteinPhaseFunction::EvaluateWithG(
					cosTheta, m_coolG );
		}

		Vector3 Sample( const Vector3& wi, ISampler& sampler ) const override
		{
			const Scalar g = sampler.Get1D() < m_hotWeight ? m_hotG : m_coolG;
			return HenyeyGreensteinPhaseFunction::SampleWithG( wi, sampler, g );
		}

		Scalar Pdf( const Vector3& wi, const Vector3& wo ) const override
		{
			return Evaluate( wi, wo );
		}

		Scalar GetMeanCosine() const override
		{
			return m_hotWeight * m_hotG + m_coolWeight * m_coolG;
		}
	};

	static Scalar SmoothHotFraction( const Scalar temperature )
	{
		if( temperature <= 700.0 ) return 0.0;
		if( temperature >= 900.0 ) return 1.0;
		const Scalar x = (temperature - 700.0) / 200.0;
		return x * x * (3.0 - 2.0 * x);
	}

	static Scalar HotAbsorptionMass633(
		const Scalar sootEm,
		const Scalar sootDensity
		)
	{
		if( !RISE::IsFiniteDouble( sootEm ) ||
			!RISE::IsFiniteDouble( sootDensity ) ||
			sootEm < 0.0 || sootDensity <= 0.0 ) return 0.0;
		const Scalar lambdaMeters = 633.0e-9;
		return 6.0 * PI * sootEm * 1.0e-3 / (lambdaMeters * sootDensity);
	}

	static Scalar TrackingSigmaT633(
		const Scalar sceneUnitMeters,
		const Scalar sootEm,
		const Scalar sootDensity,
		const Scalar sootAlbedoHot,
		const Scalar smokeKmCarbon
		)
	{
		if( !RISE::IsFiniteDouble( sceneUnitMeters ) ||
			!RISE::IsFiniteDouble( sootEm ) ||
			!RISE::IsFiniteDouble( sootDensity ) ||
			!RISE::IsFiniteDouble( sootAlbedoHot ) ||
			!RISE::IsFiniteDouble( smokeKmCarbon ) ||
			sceneUnitMeters <= 0.0 || sootAlbedoHot < 0.0 ||
			sootAlbedoHot >= 1.0 || smokeKmCarbon < 0.0 ) {
			return 0.0;
		}
		const Scalar hotExtinction =
			HotAbsorptionMass633( sootEm, sootDensity ) / (1.0 - sootAlbedoHot);
		return sceneUnitMeters * fmax( hotExtinction, smokeKmCarbon );
	}

	class MultichannelExtinctionAccessor :
		public virtual IVolumeAccessor,
		public virtual Implementation::Reference
	{
	protected:
		const IVolumeAccessor* m_carbon;
		const IVolumeAccessor* m_temperature;
		Scalar m_hotExtinctionMass;
		Scalar m_coolExtinctionMass;
		Scalar m_massMajorant;

		virtual ~MultichannelExtinctionAccessor()
		{
			safe_release( m_carbon );
			safe_release( m_temperature );
		}

		Scalar Evaluate( const Scalar x, const Scalar y, const Scalar z ) const
		{
			if( m_massMajorant <= 0.0 ) return 0.0;
			const Scalar carbon = m_carbon->GetValue( x, y, z );
			const Scalar phi = SmoothHotFraction( m_temperature->GetValue( x, y, z ) );
			const Scalar massExtinction =
				phi * m_hotExtinctionMass + (1.0 - phi) * m_coolExtinctionMass;
			return carbon * massExtinction / m_massMajorant;
		}

	public:
		MultichannelExtinctionAccessor(
			const IVolumeAccessor& carbon,
			const IVolumeAccessor& temperature,
			const Scalar hotExtinctionMass,
			const Scalar coolExtinctionMass
			) :
		  m_carbon( &carbon ),
		  m_temperature( &temperature ),
		  m_hotExtinctionMass( hotExtinctionMass ),
		  m_coolExtinctionMass( coolExtinctionMass ),
		  m_massMajorant( fmax( hotExtinctionMass, coolExtinctionMass ) )
		{
			m_carbon->addref();
			m_temperature->addref();
		}

		Scalar GetValue( Scalar x, Scalar y, Scalar z ) const override
		{
			return Evaluate( x, y, z );
		}

		Scalar GetValue( int x, int y, int z ) const override
		{
			return Evaluate( Scalar(x), Scalar(y), Scalar(z) );
		}

		void BindVolume( const IVolume* ) override
		{
			// Derived from the two immutable baked channel accessors.
		}
	};

	static IVolumeAccessor* BakeScalarChannel(
		const IScalarPainter& painter,
		const unsigned int width,
		const unsigned int height,
		const unsigned int depth,
		const Point3& bboxMin,
		const Point3& bboxMax,
		const char* channelName,
		const bool requirePositive
		)
	{
		Volume<Scalar>* volume = new Volume<Scalar>( width, height, depth );
		if( volume->Width() != width || volume->Height() != height || volume->Depth() != depth ) {
			safe_release( volume );
			return 0;
		}

		const Vector3 extent = Vector3Ops::mkVector3( bboxMax, bboxMin );
		const int halfW = static_cast<int>( width ) / 2;
		const int halfH = static_cast<int>( height ) / 2;
		const int halfD = static_cast<int>( depth ) / 2;

		for( int z = -halfD; z < static_cast<int>( depth ) - halfD; ++z ) {
			const Scalar nz = (Scalar(z + halfD) + 0.5) / Scalar(depth);
			for( int y = -halfH; y < static_cast<int>( height ) - halfH; ++y ) {
				const Scalar ny = (Scalar(y + halfH) + 0.5) / Scalar(height);
				for( int x = -halfW; x < static_cast<int>( width ) - halfW; ++x ) {
					const Scalar nx = (Scalar(x + halfW) + 0.5) / Scalar(width);
					const Point3 worldPt(
						bboxMin.x + nx * extent.x,
						bboxMin.y + ny * extent.y,
						bboxMin.z + nz * extent.z );
					const Ray dummyRay( worldPt, Vector3( 0, 1, 0 ) );
					RayIntersectionGeometric ri( dummyRay, nullRasterizerState );
					ri.ptIntersection = worldPt;
					ri.ptObjIntersec = worldPt;
					ri.ptCoord = Point2( nx, ny );
					const Scalar value = painter.GetValuesAt( ri ).v[0];
					if( !RISE::IsFiniteDouble( value ) ||
						( requirePositive ? value <= 0.0 : value < 0.0 ) ) {
						GlobalLog()->PrintEx( eLog_Error,
							"MultichannelHeterogeneousMedium:: `%s` painter produced an out-of-domain or non-finite value at (%g,%g,%g)",
							channelName, worldPt.x, worldPt.y, worldPt.z );
						safe_release( volume );
						return 0;
					}
					volume->SetValue( x, y, z, value );
				}
			}
		}

		IVolumeAccessor* accessor = new VolumeAccessor_TRI();
		accessor->BindVolume( volume );
		safe_release( volume );
		return accessor;
	}
}


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
  m_pMajorantGrid( 0 ),
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
  m_pMajorantGrid( 0 ),
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
	const Scalar trackingSigmaT,
	const IPhaseFunction& phase,
	const unsigned int volWidth,
	const unsigned int volHeight,
	const unsigned int volDepth,
	const Point3& bboxMin,
	const Point3& bboxMax
	) :
  m_max_sigma_a( trackingSigmaT, trackingSigmaT, trackingSigmaT ),
  m_max_sigma_s( 0, 0, 0 ),
  m_max_sigma_t( trackingSigmaT, trackingSigmaT, trackingSigmaT ),
  m_emission( 0, 0, 0 ),
  m_sigma_t_majorant( trackingSigmaT ),
  m_pPhase( &phase ),
  m_pAccessor( 0 ),
  m_pMajorantGrid( 0 ),
  m_bboxMin( bboxMin ),
  m_bboxMax( bboxMax ),
  m_bboxExtent( Vector3Ops::mkVector3( bboxMax, bboxMin ) ),
  m_volWidth( volWidth ),
  m_volHeight( volHeight ),
  m_volDepth( volDepth )
{
	m_pPhase->addref();
}

void HeterogeneousMedium::InitializeTrackingAccessor( IVolumeAccessor& accessor )
{
	InitializeTrackingAccessor( accessor, accessor );
}

void HeterogeneousMedium::InitializeTrackingAccessor(
	IVolumeAccessor& accessor,
	IVolumeAccessor& majorantAccessor
	)
{
	delete m_pMajorantGrid;
	m_pMajorantGrid = 0;
	safe_release( m_pAccessor );
	m_pAccessor = &accessor;
	m_pAccessor->addref();

	unsigned int gridX, gridY, gridZ;
	MajorantGrid::DefaultGridResolution( m_volWidth, m_volHeight, m_volDepth,
		gridX, gridY, gridZ );
	m_pMajorantGrid = new MajorantGrid(
		majorantAccessor, m_volWidth, m_volHeight, m_volDepth,
		m_bboxMin, m_bboxMax, m_sigma_t_majorant,
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

Scalar HeterogeneousMedium::SpectralTrackingMajorant( const Scalar ) const
{
	return ColorMath::Luminance( m_max_sigma_t );
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

	struct DeltaTrackingVisitor
	{
		const HeterogeneousMedium* self;
		const Ray* pRay;
		ISampler* pSampler;
		Scalar sigma_t_max_channel;
		Scalar* pScatterDist;
		bool* pDidScatter;

		bool operator()( Scalar tCellEntry, Scalar tCellExit, Scalar cellMajorant )
		{
			// Skip empty cells (zero majorant)
			if( cellMajorant <= 0 )
				return true;  // Continue to next cell

			const Scalar invCellMaj = 1.0 / cellMajorant;
			Scalar t = tCellEntry;

			for( ;; )
			{
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
		}
	};

	DeltaTrackingVisitor visitor;
	visitor.self = self;
	visitor.pRay = &rayRef;
	visitor.pSampler = &samplerRef;
	visitor.sigma_t_max_channel = sigma_t_max_channel;
	visitor.pScatterDist = &scatterDist;
	visitor.pDidScatter = &didScatter;

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
	const Scalar sigma_t_majorant_nm = SpectralTrackingMajorant( nm );

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

	struct DeltaTrackingVisitorNM
	{
		const HeterogeneousMedium* self;
		const Ray* pRay;
		ISampler* pSampler;
		Scalar majorantRatio;
		Scalar nm;
		Scalar* pScatterDist;
		bool* pDidScatter;

		bool operator()( Scalar tCellEntry, Scalar tCellExit, Scalar cellMajorant )
		{
			// Scale the RGB cell majorant to spectral
			const Scalar cellMajNM = cellMajorant * majorantRatio;
			if( cellMajNM <= 0 )
				return true;

			const Scalar invCellMaj = 1.0 / cellMajNM;
			Scalar t = tCellEntry;

			for( ;; )
			{
				const Scalar xi = pSampler->Get1D();
				const Scalar dt = -log( fmax( 1.0 - xi, 1e-30 ) ) * invCellMaj;
				t += dt;

				if( t >= tCellExit )
					return true;

				const Point3 samplePt = Point3Ops::mkPoint3(
					pRay->origin, pRay->Dir() * t );
				const Scalar sigma_t_local = self->GetCoefficientsNM( samplePt, nm ).sigma_t;

				const Scalar xi2 = pSampler->Get1D();
				if( xi2 < sigma_t_local * invCellMaj )
				{
					*pScatterDist = t;
					*pDidScatter = true;
					return false;
				}
			}
		}
	};

	DeltaTrackingVisitorNM visitor;
	visitor.self = self;
	visitor.pRay = &rayRef;
	visitor.pSampler = &samplerRef;
	visitor.majorantRatio = majorantRatio;
	visitor.nm = nm;
	visitor.pScatterDist = &scatterDist;
	visitor.pDidScatter = &didScatter;

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
	struct RatioTrackingVisitor
	{
		const HeterogeneousMedium* self;
		const Ray* pRay;
		RandomNumberGenerator* pRng;
		RISEPel* pW;
		RISEPel max_sigma_t;

		bool operator()( Scalar tCellEntry, Scalar tCellExit, Scalar cellMajorant )
		{
			if( cellMajorant <= 0 )
				return true;  // Skip empty cells

			const Scalar invCellMaj = 1.0 / cellMajorant;
			Scalar t = tCellEntry;

			for( ;; )
			{
				Scalar xi = pRng->CanonicalRandom();
				if( xi <= 0.0 ) {
					// The continuous exponential law has no atom at zero.  Retry
					// the discrete RNG endpoint instead of creating a zero step.
					continue;
				}
				xi = std::fmin( xi, std::nextafter( Scalar(1.0), Scalar(0.0) ) );
				const Scalar dt = -std::log1p( -xi ) * invCellMaj;
				const Scalar nextT = t + dt;
				if( !(nextT > t) ) {
					GlobalLog()->PrintEasyError(
						"HeterogeneousMedium::EvalTransmittance: finite ratio-tracking step cannot advance the ray parameter" );
					*pW = RISEPel( -1.0, -1.0, -1.0 );
					return false;
				}
				t = nextT;

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
		}
	};

	RatioTrackingVisitor visitor;
	visitor.self = self;
	visitor.pRay = &ray;
	visitor.pRng = &tl_rng;
	visitor.pW = &w;
	visitor.max_sigma_t = m_max_sigma_t;

	m_pMajorantGrid->TraverseRay( ray, 0.0, dist, visitor );

	return w;
}

Scalar HeterogeneousMedium::EvalTransmittanceNM(
	const Ray& ray,
	const Scalar dist,
	const Scalar nm
	) const
{
	const Scalar sigma_t_max_nm = SpectralTrackingMajorant( nm );

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

	struct RatioTrackingVisitorNM
	{
		const HeterogeneousMedium* self;
		const Ray* pRay;
		RandomNumberGenerator* pRng;
		Scalar* pW;
		Scalar majorantRatio;
		Scalar nm;

		bool operator()( Scalar tCellEntry, Scalar tCellExit, Scalar cellMajorant )
		{
			const Scalar cellMajNM = cellMajorant * majorantRatio;
			if( cellMajNM <= 0 )
				return true;

			const Scalar invCellMaj = 1.0 / cellMajNM;
			Scalar t = tCellEntry;

			for( ;; )
			{
				Scalar xi = pRng->CanonicalRandom();
				if( xi <= 0.0 ) {
					continue;
				}
				xi = std::fmin( xi, std::nextafter( Scalar(1.0), Scalar(0.0) ) );
				const Scalar dt = -std::log1p( -xi ) * invCellMaj;
				const Scalar nextT = t + dt;
				if( !(nextT > t) ) {
					GlobalLog()->PrintEasyError(
						"HeterogeneousMedium::EvalTransmittanceNM: finite ratio-tracking step cannot advance the ray parameter" );
					*pW = -1.0;
					return false;
				}
				t = nextT;

				if( t >= tCellExit )
					return true;

				const Point3 samplePt = Point3Ops::mkPoint3(
					pRay->origin, pRay->Dir() * t );
				const Scalar sigma_t_local = self->GetCoefficientsNM( samplePt, nm ).sigma_t;

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
	visitor.majorantRatio = majorantRatio;
	visitor.nm = nm;

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
	return ds;
}

Scalar HeterogeneousMedium::EvalDeterministicOpticalDepth(
	const Ray& ray,
	const Scalar targetDist,
	const Scalar sigma_t_eff
	) const
{
	return EvalDeterministicOpticalDepthImpl(
		ray, targetDist, sigma_t_eff, false, 0.0 );
}

Scalar HeterogeneousMedium::EvalDeterministicOpticalDepthNM(
	const Ray& ray,
	const Scalar targetDist,
	const Scalar nm
	) const
{
	return EvalDeterministicOpticalDepthImpl(
		ray, targetDist, 1.0, true, nm );
}

Scalar HeterogeneousMedium::EvalDeterministicOpticalDepthImpl(
	const Ray& ray,
	const Scalar targetDist,
	const Scalar sigma_t_eff,
	const bool spectral,
	const Scalar nm
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
				const Point3 samplePt = Point3Ops::mkPoint3(
					ray.origin, ray.Dir() * tq );
				const Scalar dq = spectral
					? GetCoefficientsNM( samplePt, nm ).sigma_t
					: sigma_t_eff * LookupDensity( samplePt );
				segIntegral += glWeights[q] * dq;
			}
			opticalDepth += halfLen * segIntegral;
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
		return sigma_t_eff * density * T_real;
	}
	else
	{
		return T_real;
	}
}

Scalar HeterogeneousMedium::EvalLogDistancePdfNM(
	const Ray& ray,
	const Scalar t,
	const bool scattered,
	const Scalar maxDist,
	const Scalar nm
	) const
{
	const Scalar targetDist = scattered ? t : maxDist;
	const Scalar tau = EvalDeterministicOpticalDepthNM( ray, targetDist, nm );
	if( !scattered ) return -tau;

	const Scalar localSigmaT = GetCoefficientsNM(
		ray.PointAtLength( t ), nm ).sigma_t;
	return localSigmaT > 0.0 ? log( localSigmaT ) - tau : -RISE_INFINITY;
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
	const Scalar targetDist = scattered ? t : maxDist;
	const Scalar tau = EvalDeterministicOpticalDepthNM( ray, targetDist, nm );
	const Scalar T_real = exp( -tau );

	if( scattered )
	{
		const Point3 pt = ray.PointAtLength( t );
		return GetCoefficientsNM( pt, nm ).sigma_t * T_real;
	}
	else
	{
		return T_real;
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


Scalar MultichannelHeterogeneousMedium::ComputeHotAbsorptionMass633(
	Scalar sootEm,
	Scalar sootDensity
	)
{
	return HotAbsorptionMass633( sootEm, sootDensity );
}

MultichannelHeterogeneousMedium::MultichannelHeterogeneousMedium(
	const IScalarPainter& carbonPainter,
	const IScalarPainter& temperaturePainter,
	const unsigned int volWidth,
	const unsigned int volHeight,
	const unsigned int volDepth,
	const Point3& bboxMin,
	const Point3& bboxMax,
	const Scalar sceneUnitMeters,
	const Scalar sootEm,
	const Scalar sootDensity,
	const Scalar sootAlbedoHot,
	const Scalar sootGHot,
	const Scalar smokeKmCarbon,
	const Scalar smokeNCarbon,
	const Scalar smokeAlbedoCarbon,
	const Scalar smokeGCarbon,
	const IPhaseFunction& phase
	) :
  HeterogeneousMedium(
	  TrackingSigmaT633( sceneUnitMeters, sootEm, sootDensity,
		  sootAlbedoHot, smokeKmCarbon ),
	  phase, volWidth, volHeight, volDepth, bboxMin, bboxMax ),
  m_pCarbonAccessor( 0 ),
  m_pTemperatureAccessor( 0 ),
  m_sceneUnitMeters( sceneUnitMeters ),
  m_sootEm( sootEm ),
  m_sootDensity( sootDensity ),
  m_sootAlbedoHot( sootAlbedoHot ),
  m_sootGHot( sootGHot ),
  m_smokeKmCarbon( smokeKmCarbon ),
  m_smokeNCarbon( smokeNCarbon ),
  m_smokeAlbedoCarbon( smokeAlbedoCarbon ),
  m_smokeGCarbon( smokeGCarbon ),
  m_hotAbsorptionMass633( ComputeHotAbsorptionMass633( sootEm, sootDensity ) ),
  m_hotExtinctionMass633(
	  sootAlbedoHot >= 0.0 && sootAlbedoHot < 1.0
		  ? m_hotAbsorptionMass633 / (1.0 - sootAlbedoHot)
		  : 0.0 ),
	  m_coolExtinctionMass633( smokeKmCarbon ),
	  m_emissionBinSize( 0, 0, 0 ),
	  m_thermalEmissionImportance( 0.0 ),
  m_minPositiveThermalEmissionPdf( 0.0 ),
  m_valid( false )
{
	const Vector3 extent = Vector3Ops::mkVector3( bboxMax, bboxMin );
	const bool validDimensions = volWidth >= 2 && volHeight >= 2 && volDepth >= 2 &&
		volWidth <= 0x7fffffffu && volHeight <= 0x7fffffffu && volDepth <= 0x7fffffffu;
	const bool validBounds =
		RISE::IsFiniteDouble( bboxMin.x ) && RISE::IsFiniteDouble( bboxMin.y ) &&
		RISE::IsFiniteDouble( bboxMin.z ) && RISE::IsFiniteDouble( bboxMax.x ) &&
		RISE::IsFiniteDouble( bboxMax.y ) && RISE::IsFiniteDouble( bboxMax.z ) &&
		RISE::IsFiniteDouble( extent.x ) && RISE::IsFiniteDouble( extent.y ) &&
		RISE::IsFiniteDouble( extent.z ) &&
		extent.x > 0.0 && extent.y > 0.0 && extent.z > 0.0;
	const bool validOptics =
		RISE::IsFiniteDouble( sceneUnitMeters ) &&
		RISE::IsFiniteDouble( sootEm ) && RISE::IsFiniteDouble( sootDensity ) &&
		RISE::IsFiniteDouble( sootAlbedoHot ) && RISE::IsFiniteDouble( sootGHot ) &&
		RISE::IsFiniteDouble( smokeKmCarbon ) && RISE::IsFiniteDouble( smokeNCarbon ) &&
		RISE::IsFiniteDouble( smokeAlbedoCarbon ) && RISE::IsFiniteDouble( smokeGCarbon ) &&
		sceneUnitMeters > 0.0 && sootEm >= 0.0 && sootDensity > 0.0 &&
		sootAlbedoHot >= 0.0 && sootAlbedoHot < 1.0 &&
		sootGHot > -1.0 && sootGHot < 1.0 &&
		smokeKmCarbon >= 0.0 && smokeNCarbon >= 0.0 &&
		smokeAlbedoCarbon >= 0.0 && smokeAlbedoCarbon <= 1.0 &&
		smokeGCarbon > -1.0 && smokeGCarbon < 1.0;
	if( !validDimensions || !validBounds || !validOptics ||
		carbonPainter.HasPerChannelVariation() || temperaturePainter.HasPerChannelVariation() ) {
		GlobalLog()->PrintEasyError(
			"MultichannelHeterogeneousMedium:: invalid lattice, bbox, scalar channel, or carbon optical parameter" );
		return;
	}

	m_pCarbonAccessor = BakeScalarChannel(
		carbonPainter, volWidth, volHeight, volDepth, bboxMin, bboxMax, "carbon", false );
	if( !m_pCarbonAccessor ) return;

	m_pTemperatureAccessor = BakeScalarChannel(
		temperaturePainter, volWidth, volHeight, volDepth, bboxMin, bboxMax, "temperature", true );
	if( !m_pTemperatureAccessor ) return;

	IVolumeAccessor* trackingAccessor = new MultichannelExtinctionAccessor(
		*m_pCarbonAccessor, *m_pTemperatureAccessor,
		m_hotExtinctionMass633, m_coolExtinctionMass633 );
	// The local tracking accessor evaluates carbon * k(phi(T)).  Building
	// a majorant from that nonlinear product's corner samples can miss an
	// interior maximum when carbon and temperature gradients oppose each
	// other.  Build the grid from carbon alone and scale it by the global
	// max of the hot/cool mass extinction (already in m_sigma_t_majorant):
	// this is the required phi-sup bound.
	InitializeTrackingAccessor( *trackingAccessor, *m_pCarbonAccessor );
	safe_release( trackingAccessor );
	m_valid = true;
	if( !BuildThermalEmissionImportance() ) {
		m_valid = false;
		GlobalLog()->PrintEasyError(
			"MultichannelHeterogeneousMedium:: failed to build finite thermal-emission importance" );
	}
}

MultichannelHeterogeneousMedium::~MultichannelHeterogeneousMedium()
{
	safe_release( m_pCarbonAccessor );
	safe_release( m_pTemperatureAccessor );
}

unsigned int MultichannelHeterogeneousMedium::EmissionBinIndex(
	const unsigned int x,
	const unsigned int y,
	const unsigned int z
	) const
{
	return z * m_volWidth * m_volHeight + y * m_volWidth + x;
}

bool MultichannelHeterogeneousMedium::EmissionBinAtPoint(
	const Point3& point,
	unsigned int& x,
	unsigned int& y,
	unsigned int& z
	) const
{
	if( !RISE::IsFiniteDouble( point.x ) || !RISE::IsFiniteDouble( point.y ) ||
		!RISE::IsFiniteDouble( point.z ) ) return false;
	if( point.x < m_bboxMin.x || point.x > m_bboxMax.x ||
		point.y < m_bboxMin.y || point.y > m_bboxMax.y ||
		point.z < m_bboxMin.z || point.z > m_bboxMax.z ) return false;

	x = HalfOpenBinIndex( point.x, m_bboxMin.x, m_bboxMax.x,
		m_emissionBinSize.x, m_volWidth );
	y = HalfOpenBinIndex( point.y, m_bboxMin.y, m_bboxMax.y,
		m_emissionBinSize.y, m_volHeight );
	z = HalfOpenBinIndex( point.z, m_bboxMin.z, m_bboxMax.z,
		m_emissionBinSize.z, m_volDepth );
	return true;
}

Scalar MultichannelHeterogeneousMedium::EmissionBinUpperBound(
	const unsigned int x,
	const unsigned int y,
	const unsigned int z
	) const
{
	Scalar maxCarbon = 0.0;
	Scalar maxTemperature = 0.0;
	const int halfW = static_cast<int>(m_volWidth / 2u);
	const int halfH = static_cast<int>(m_volHeight / 2u);
	const int halfD = static_cast<int>(m_volDepth / 2u);
	for( int dz = -1; dz <= 1; ++dz ) {
		for( int dy = -1; dy <= 1; ++dy ) {
			for( int dx = -1; dx <= 1; ++dx ) {
				const int ix = static_cast<int>(x) + dx - halfW;
				const int iy = static_cast<int>(y) + dy - halfH;
				const int iz = static_cast<int>(z) + dz - halfD;
				maxCarbon = fmax( maxCarbon,
					m_pCarbonAccessor->GetValue( ix, iy, iz ) );
				maxTemperature = fmax( maxTemperature,
					m_pTemperatureAccessor->GetValue( ix, iy, iz ) );
			}
		}
	}
	if( maxCarbon <= 0.0 || maxTemperature <= 0.0 ) return 0.0;

	const Scalar blueScale = 633.0 / 380.0;
	const Scalar hotAbsorptionMax = m_hotAbsorptionMass633 * blueScale;
	const Scalar coolAbsorptionMax = m_coolExtinctionMass633 *
		(1.0 - m_smokeAlbedoCarbon) * pow( blueScale, m_smokeNCarbon );
	const Scalar absorptionMax = fmax( hotAbsorptionMax, coolAbsorptionMax );

	// Wien's displacement gives the maximum of B_lambda(T) on the band.
	// Clamp that analytic peak to [380,780]; separating the absorption and
	// Planck maxima is intentionally conservative.
	const Scalar wienPeakNM = 2.897771955e6 / maxTemperature;
	const Scalar planckPeakNM = fmax( Scalar(380.0), fmin( Scalar(780.0), wienPeakNM ) );
	const Scalar planckMax = PlanckSpectralRadianceNM(
		planckPeakNM, maxTemperature );
	return RepresentedBinVolume( m_bboxMin, m_bboxMax, m_emissionBinSize,
		x, y, z, m_volWidth, m_volHeight, m_volDepth ) *
		Scalar(400.0) * m_sceneUnitMeters *
		maxCarbon * absorptionMax * planckMax;
}

bool MultichannelHeterogeneousMedium::BuildThermalEmissionImportance()
{
	if( !m_pMajorantGrid || !m_pCarbonAccessor || !m_pTemperatureAccessor ) {
		return false;
	}
	m_emissionBinSize = Vector3(
		m_bboxExtent.x / Scalar(m_volWidth),
		m_bboxExtent.y / Scalar(m_volHeight),
		m_bboxExtent.z / Scalar(m_volDepth) );
	m_thermalEmissionImportance = 0.0;
	m_minPositiveThermalEmissionPdf = 0.0;
	if( !RISE::IsFiniteDouble(m_emissionBinSize.x) || m_emissionBinSize.x <= 0.0 ||
		!RISE::IsFiniteDouble(m_emissionBinSize.y) || m_emissionBinSize.y <= 0.0 ||
		!RISE::IsFiniteDouble(m_emissionBinSize.z) || m_emissionBinSize.z <= 0.0 ) {
		return false;
	}
	if( !HasRepresentableBinInteriors( m_bboxMin.x, m_bboxMax.x,
		m_emissionBinSize.x, m_volWidth ) ||
		!HasRepresentableBinInteriors( m_bboxMin.y, m_bboxMax.y,
		m_emissionBinSize.y, m_volHeight ) ||
		!HasRepresentableBinInteriors( m_bboxMin.z, m_bboxMax.z,
		m_emissionBinSize.z, m_volDepth ) ) return false;

	const unsigned int binCount = m_volWidth * m_volHeight * m_volDepth;
	m_emissionBinWeights.assign( binCount, 0.0 );
	m_emissionBinProbabilities.assign( binCount, 0.0 );
	const unsigned int cellCount = m_pMajorantGrid->GetGridX() *
		m_pMajorantGrid->GetGridY() * m_pMajorantGrid->GetGridZ();
	m_emissionCells.clear();
	m_emissionCells.resize( cellCount );
	std::vector<std::vector<double> > cellWeights( cellCount );

	static const Scalar spatialNodes[2] = {
		Scalar(0.5) * (Scalar(1.0) - Scalar(1.0) / sqrt(Scalar(3.0))),
		Scalar(0.5) * (Scalar(1.0) + Scalar(1.0) / sqrt(Scalar(3.0)))
	};
	static const Scalar supportMix = Scalar(1.0) / Scalar(1024.0);

	for( unsigned int z = 0; z < m_volDepth; ++z ) {
		for( unsigned int y = 0; y < m_volHeight; ++y ) {
			for( unsigned int x = 0; x < m_volWidth; ++x ) {
				const Point3 binMin(
					m_bboxMin.x + Scalar(x) * m_emissionBinSize.x,
					m_bboxMin.y + Scalar(y) * m_emissionBinSize.y,
					m_bboxMin.z + Scalar(z) * m_emissionBinSize.z );
				const Point3 binMax(
					x + 1u == m_volWidth ? m_bboxMax.x :
						m_bboxMin.x + Scalar(x+1u) * m_emissionBinSize.x,
					y + 1u == m_volHeight ? m_bboxMax.y :
						m_bboxMin.y + Scalar(y+1u) * m_emissionBinSize.y,
					z + 1u == m_volDepth ? m_bboxMax.z :
						m_bboxMin.z + Scalar(z+1u) * m_emissionBinSize.z );
				const Vector3 representedSize = Vector3Ops::mkVector3( binMax, binMin );
				const Scalar binVolume = representedSize.x * representedSize.y *
					representedSize.z;
				if( !RISE::IsFiniteDouble(binVolume) || binVolume <= 0.0 ) return false;
				Scalar proposalAverage = 0.0;
				for( unsigned int gz = 0; gz < 2; ++gz ) {
					for( unsigned int gy = 0; gy < 2; ++gy ) {
						for( unsigned int gx = 0; gx < 2; ++gx ) {
							const Point3 samplePoint(
								binMin.x + spatialNodes[gx] * representedSize.x,
								binMin.y + spatialNodes[gy] * representedSize.y,
								binMin.z + spatialNodes[gz] * representedSize.z );
							proposalAverage += GaussLegendre21::IntegrateVisibleBand(
								[this, &samplePoint]( const Scalar nm ) {
									return GetThermalEmissionNM( samplePoint, nm );
								} ) / Scalar(8.0);
						}
					}
				}
				const Scalar proposal = binVolume * proposalAverage;
				const Scalar upper = EmissionBinUpperBound( x, y, z );
				const Scalar weight = (Scalar(1.0) - supportMix) * proposal +
					supportMix * upper;
				if( !RISE::IsFiniteDouble( proposal ) || proposal < 0.0 ||
					!RISE::IsFiniteDouble( upper ) || upper < 0.0 ||
					!RISE::IsFiniteDouble( weight ) || weight < 0.0 ) return false;

				const unsigned int binIndex = EmissionBinIndex( x, y, z );
				m_emissionBinWeights[binIndex] = static_cast<double>( weight );
				const Point3 center(
					binMin.x + Scalar(0.5) * representedSize.x,
					binMin.y + Scalar(0.5) * representedSize.y,
					binMin.z + Scalar(0.5) * representedSize.z );
				unsigned int cx = 0, cy = 0, cz = 0;
				if( !m_pMajorantGrid->WorldToCell( center, cx, cy, cz ) ) return false;
				const unsigned int cellIndex = cz * m_pMajorantGrid->GetGridX() *
					m_pMajorantGrid->GetGridY() + cy * m_pMajorantGrid->GetGridX() + cx;
				m_emissionCells[cellIndex].binIndices.push_back( binIndex );
				cellWeights[cellIndex].push_back( static_cast<double>( weight ) );
			}
		}
	}

	std::vector<double> topWeights( cellCount, 0.0 );
	for( unsigned int i = 0; i < cellCount; ++i ) {
		m_emissionCells[i].binAlias.Build( cellWeights[i] );
		topWeights[i] = m_emissionCells[i].binAlias.TotalWeight();
	}
	m_emissionCellAlias.Build( topWeights );
	m_thermalEmissionImportance = static_cast<Scalar>(
		m_emissionCellAlias.TotalWeight() );
	if( !RISE::IsFiniteDouble( m_thermalEmissionImportance ) ||
		m_thermalEmissionImportance < 0.0 ) return false;
	if( m_thermalEmissionImportance > 0.0 ) {
		for( unsigned int i = 0; i < binCount; ++i ) {
			m_emissionBinProbabilities[i] = m_emissionBinWeights[i] /
				static_cast<double>(m_thermalEmissionImportance);
			if( m_emissionBinWeights[i] > 0.0 &&
				m_emissionBinProbabilities[i] <= 0.0 ) return false;
			const unsigned int xy = m_volWidth * m_volHeight;
			const unsigned int z = i / xy;
			const unsigned int rem = i - z*xy;
			const unsigned int y = rem / m_volWidth;
			const unsigned int x = rem - y*m_volWidth;
			const Scalar binVolume = RepresentedBinVolume(
				m_bboxMin, m_bboxMax, m_emissionBinSize, x, y, z,
				m_volWidth, m_volHeight, m_volDepth );
			const Scalar density = static_cast<Scalar>(
				m_emissionBinProbabilities[i] ) / binVolume;
			if( !RISE::IsFiniteDouble(density) || density < 0.0 ||
				(m_emissionBinProbabilities[i] > 0.0 && density <= 0.0) ) return false;
			if( density > 0.0 &&
				(m_minPositiveThermalEmissionPdf <= 0.0 ||
				density < m_minPositiveThermalEmissionPdf) ) {
				m_minPositiveThermalEmissionPdf = density;
			}
		}
		if( m_minPositiveThermalEmissionPdf <= 0.0 ) return false;
	}
	return true;
}

Scalar MultichannelHeterogeneousMedium::LookupChannel(
	const IVolumeAccessor& accessor,
	const Point3& worldPt
	) const
{
	if( worldPt.x < m_bboxMin.x || worldPt.x > m_bboxMax.x ||
		worldPt.y < m_bboxMin.y || worldPt.y > m_bboxMax.y ||
		worldPt.z < m_bboxMin.z || worldPt.z > m_bboxMax.z ) {
		return 0.0;
	}

	const Scalar nx = (worldPt.x - m_bboxMin.x) / m_bboxExtent.x;
	const Scalar ny = (worldPt.y - m_bboxMin.y) / m_bboxExtent.y;
	const Scalar nz = (worldPt.z - m_bboxMin.z) / m_bboxExtent.z;
	const Scalar halfW = Scalar(m_volWidth / 2u);
	const Scalar halfH = Scalar(m_volHeight / 2u);
	const Scalar halfD = Scalar(m_volDepth / 2u);
	return accessor.GetValue(
		nx * Scalar(m_volWidth) - 0.5 - halfW,
		ny * Scalar(m_volHeight) - 0.5 - halfH,
		nz * Scalar(m_volDepth) - 0.5 - halfD );
}

Scalar MultichannelHeterogeneousMedium::LookupCarbon( const Point3& worldPt ) const
{
	return m_pCarbonAccessor ? LookupChannel( *m_pCarbonAccessor, worldPt ) : 0.0;
}

Scalar MultichannelHeterogeneousMedium::LookupTemperature( const Point3& worldPt ) const
{
	return m_pTemperatureAccessor ? LookupChannel( *m_pTemperatureAccessor, worldPt ) : 0.0;
}

Scalar MultichannelHeterogeneousMedium::HotOpticsFraction( const Point3& worldPt ) const
{
	return SmoothHotFraction( LookupTemperature( worldPt ) );
}

Scalar MultichannelHeterogeneousMedium::HotSootVolumeFraction( const Point3& worldPt ) const
{
	return HotOpticsFraction( worldPt ) * 1.0e-3 * LookupCarbon( worldPt ) / m_sootDensity;
}

Scalar MultichannelHeterogeneousMedium::TrackingMajorantAt( const Point3& worldPt ) const
{
	if( !m_pMajorantGrid ) return 0.0;
	unsigned int x = 0, y = 0, z = 0;
	if( !m_pMajorantGrid->WorldToCell( worldPt, x, y, z ) ) return 0.0;
	return m_pMajorantGrid->GetCellMajorant( x, y, z );
}

Scalar MultichannelHeterogeneousMedium::SpectralTrackingMajorant(
	const Scalar
	) const
{
	// Locked Phase-A policy: one conservative max over the full visible
	// interval.  Both admitted extinction laws are monotone toward blue, so
	// their maxima occur at 380 nm; phi(T) is a convex hot/cool blend.
	const Scalar visibleBlueScale = 633.0 / 380.0;
	const Scalar hotMax = m_hotExtinctionMass633 * visibleBlueScale;
	const Scalar coolMax = m_coolExtinctionMass633 *
		pow( visibleBlueScale, m_smokeNCarbon );
	return m_sceneUnitMeters * fmax( hotMax, coolMax );
}

Scalar MultichannelHeterogeneousMedium::TrackingMajorantAtNM(
	const Point3& worldPt,
	const Scalar nm
	) const
{
	const Scalar majorant633 = TrackingMajorantAt( worldPt );
	if( majorant633 <= 0.0 || m_sigma_t_majorant <= 0.0 ) return 0.0;
	return majorant633 * SpectralTrackingMajorant( nm ) / m_sigma_t_majorant;
}

MediumCoefficients MultichannelHeterogeneousMedium::GetCoefficients(
	const Point3& pt
	) const
{
	const Scalar carbon = LookupCarbon( pt );
	const Scalar phi = HotOpticsFraction( pt );
	const Scalar hotAbsorption = carbon * phi * m_hotAbsorptionMass633;
	const Scalar hotScattering = hotAbsorption * m_sootAlbedoHot / (1.0 - m_sootAlbedoHot);
	const Scalar coolExtinction = carbon * (1.0 - phi) * m_coolExtinctionMass633;
	const Scalar coolScattering = coolExtinction * m_smokeAlbedoCarbon;

	const Scalar sigmaA = m_sceneUnitMeters *
		(hotAbsorption + coolExtinction - coolScattering);
	const Scalar sigmaS = m_sceneUnitMeters *
		(hotScattering + coolScattering);

	MediumCoefficients c;
	c.sigma_t = RISEPel( sigmaA + sigmaS, sigmaA + sigmaS, sigmaA + sigmaS );
	c.sigma_s = RISEPel( sigmaS, sigmaS, sigmaS );
	c.emission = RISEPel( 0, 0, 0 );
	return c;
}

MediumCoefficientsNM MultichannelHeterogeneousMedium::GetCoefficientsNM(
	const Point3& pt,
	const Scalar nm
	) const
{
	MediumCoefficientsNM c;
	c.sigma_t = 0.0;
	c.sigma_s = 0.0;
	c.emission = 0.0;
	if( !m_valid || !RISE::IsFiniteDouble( nm ) || nm <= 0.0 ) return c;

	const Scalar carbon = LookupCarbon( pt );
	const Scalar phi = HotOpticsFraction( pt );
	const Scalar wavelengthScale = 633.0 / nm;
	const Scalar hotAbsorption = carbon * phi * m_hotAbsorptionMass633 * wavelengthScale;
	const Scalar hotScattering = hotAbsorption *
		m_sootAlbedoHot / (1.0 - m_sootAlbedoHot);
	const Scalar coolExtinction = carbon * (1.0 - phi) *
		m_coolExtinctionMass633 * pow( wavelengthScale, m_smokeNCarbon );
	const Scalar coolScattering = coolExtinction * m_smokeAlbedoCarbon;
	const Scalar sigmaA = m_sceneUnitMeters *
		(hotAbsorption + coolExtinction - coolScattering);
	const Scalar sigmaS = m_sceneUnitMeters *
		(hotScattering + coolScattering);
	c.sigma_t = sigmaA + sigmaS;
	c.sigma_s = sigmaS;
	return c;
}

const IPhaseFunction* MultichannelHeterogeneousMedium::MakePhaseClosure(
	const Point3& pt,
	const Scalar nm
	) const
{
	if( !m_valid || !RISE::IsFiniteDouble( nm ) || nm <= 0.0 ) return 0;

	const Scalar carbon = LookupCarbon( pt );
	const Scalar phi = HotOpticsFraction( pt );
	const Scalar wavelengthScale = 633.0 / nm;
	const Scalar hotAbsorption = carbon * phi * m_hotAbsorptionMass633 * wavelengthScale;
	const Scalar hotScattering = m_sceneUnitMeters * hotAbsorption *
		m_sootAlbedoHot / (1.0 - m_sootAlbedoHot);
	const Scalar coolExtinction = carbon * (1.0 - phi) *
		m_coolExtinctionMass633 * pow( wavelengthScale, m_smokeNCarbon );
	const Scalar coolScattering = m_sceneUnitMeters * coolExtinction *
		m_smokeAlbedoCarbon;
	const Scalar totalScattering = hotScattering + coolScattering;
	if( !RISE::IsFiniteDouble( totalScattering ) || totalScattering <= 0.0 ) return 0;

	return new ConstituentHGPhaseClosure(
		hotScattering, coolScattering, m_sootGHot, m_smokeGCarbon );
}

Scalar MultichannelHeterogeneousMedium::GetThermalEmissionNM(
	const Point3& pt,
	const Scalar nm
	) const
{
	if( !m_valid ) return 0.0;
	const MediumCoefficientsNM coeff = GetCoefficientsNM( pt, nm );
	const Scalar sigmaA = coeff.sigma_t - coeff.sigma_s;
	return sigmaA > 0.0
		? sigmaA * PlanckSpectralRadianceNM( nm, LookupTemperature( pt ) )
		: 0.0;
}

Scalar MultichannelHeterogeneousMedium::GetThermalEmissionPowerProxy() const
{
	if( m_sceneUnitMeters <= 0.0 || m_thermalEmissionImportance <= 0.0 ) return 0.0;
	int sceneExponent = 0;
	int importanceExponent = 0;
	const Scalar sceneMantissa = std::frexp( m_sceneUnitMeters, &sceneExponent );
	const Scalar importanceMantissa = std::frexp(
		m_thermalEmissionImportance, &importanceExponent );
	int productExponent = 0;
	const Scalar productMantissa = std::frexp(
		FOUR_PI * sceneMantissa * sceneMantissa * importanceMantissa,
		&productExponent );
	return std::ldexp( productMantissa,
		2*sceneExponent + importanceExponent + productExponent );
}

bool MultichannelHeterogeneousMedium::SampleThermalEmission(
	ISampler& sampler,
	Point3& point,
	Scalar& pdf
	) const
{
	pdf = 0.0;
	if( !m_valid || !m_emissionCellAlias.IsValid() ||
		m_thermalEmissionImportance <= 0.0 ) return false;

	const unsigned int cellIndex = m_emissionCellAlias.Sample( sampler.Get1D() );
	if( cellIndex >= m_emissionCells.size() ) return false;
	const EmissionCell& cell = m_emissionCells[cellIndex];
	if( !cell.binAlias.IsValid() || cell.binIndices.empty() ) return false;
	const unsigned int localBin = cell.binAlias.Sample( sampler.Get1D() );
	if( localBin >= cell.binIndices.size() ) return false;
	const unsigned int binIndex = cell.binIndices[localBin];
	const unsigned int xy = m_volWidth * m_volHeight;
	const unsigned int z = binIndex / xy;
	const unsigned int rem = binIndex - z * xy;
	const unsigned int y = rem / m_volWidth;
	const unsigned int x = rem - y * m_volWidth;
	point = Point3(
		SampleHalfOpenBinCoordinate( m_bboxMin.x, m_bboxMax.x,
			m_emissionBinSize.x, x, m_volWidth, sampler.Get1D() ),
		SampleHalfOpenBinCoordinate( m_bboxMin.y, m_bboxMax.y,
			m_emissionBinSize.y, y, m_volHeight, sampler.Get1D() ),
		SampleHalfOpenBinCoordinate( m_bboxMin.z, m_bboxMax.z,
			m_emissionBinSize.z, z, m_volDepth, sampler.Get1D() ) );
	pdf = ThermalEmissionPdf( point );
	return pdf > 0.0 && RISE::IsFiniteDouble( pdf );
}

Scalar MultichannelHeterogeneousMedium::ThermalEmissionPdf(
	const Point3& point
	) const
{
	if( !m_valid || m_thermalEmissionImportance <= 0.0 ) return 0.0;
	unsigned int x = 0, y = 0, z = 0;
	if( !EmissionBinAtPoint( point, x, y, z ) ) return 0.0;
	// Divide the pre-normalized q_v by V_v.  Keeping q_v as constructed state
	// prevents fast-math reassociation back into weight/(W_m*V_v), whose
	// denominator can underflow for a tiny but otherwise valid finite bbox.
	const Scalar binVolume = RepresentedBinVolume(
		m_bboxMin, m_bboxMax, m_emissionBinSize, x, y, z,
		m_volWidth, m_volHeight, m_volDepth );
	return static_cast<Scalar>(
		m_emissionBinProbabilities[EmissionBinIndex( x, y, z )] ) / binVolume;
}

Scalar MultichannelHeterogeneousMedium::GetThermalEmissionBinProbability(
	const unsigned int x,
	const unsigned int y,
	const unsigned int z
	) const
{
	if( x >= m_volWidth || y >= m_volHeight || z >= m_volDepth ||
		m_thermalEmissionImportance <= 0.0 ) return 0.0;
	return static_cast<Scalar>(
		m_emissionBinProbabilities[EmissionBinIndex( x, y, z )] );
}
