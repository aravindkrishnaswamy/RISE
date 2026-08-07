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
#include "../Utilities/Color/ColorUtils.h"
#include "../Volume/Volume.h"
#include "../Volume/VolumeAccessor_TRI.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <math.h>

using namespace RISE;

namespace
{
	static Scalar EvaluateCubic(
		const Scalar (&coeff)[4],
		const Scalar x
		)
	{
		return ((coeff[0]*x + coeff[1])*x + coeff[2])*x + coeff[3];
	}

	static bool CubicValueIsNumericalZero(
		const Scalar (&coeff)[4],
		const Scalar x,
		const Scalar value
		)
	{
		const Scalar ax = fabs(x);
		const Scalar scale = fabs(coeff[0])*ax*ax*ax +
			fabs(coeff[1])*ax*ax + fabs(coeff[2])*ax + fabs(coeff[3]);
		return fabs(value) <= 64.0*std::numeric_limits<Scalar>::epsilon()*scale;
	}

	static void AppendDerivativeRoots(
		const Scalar (&coeff)[4],
		std::vector<Scalar>& partition
		)
	{
		const Scalar a = 3.0*coeff[0];
		const Scalar b = 2.0*coeff[1];
		const Scalar c = coeff[2];
		if( a == 0.0 ) {
			if( b != 0.0 ) {
				const Scalar root = -c/b;
				if( root > 0.0 && root < 1.0 ) partition.push_back(root);
			}
			return;
		}

		Scalar discriminant = b*b - 4.0*a*c;
		const Scalar discriminantScale = b*b + fabs(4.0*a*c);
		if( discriminant < 0.0 &&
			-discriminant <= 64.0*std::numeric_limits<Scalar>::epsilon()*discriminantScale ) {
			discriminant = 0.0;
		}
		if( discriminant < 0.0 ) return;

		const Scalar rootDisc = sqrt(discriminant);
		const Scalar q = -0.5*(b + copysign(rootDisc,b));
		if( q == 0.0 ) {
			const Scalar root = -b/(2.0*a);
			if( root > 0.0 && root < 1.0 ) partition.push_back(root);
			return;
		}
		const Scalar root0 = q/a;
		const Scalar root1 = c/q;
		if( root0 > 0.0 && root0 < 1.0 ) partition.push_back(root0);
		if( root1 > 0.0 && root1 < 1.0 ) partition.push_back(root1);
	}

	static void SortUniqueUnitIntervalValues( std::vector<Scalar>& values )
	{
		std::sort( values.begin(), values.end() );
		std::vector<Scalar>::iterator out = values.begin();
		for( std::vector<Scalar>::const_iterator it = values.begin();
			it != values.end(); ++it ) {
			if( out == values.begin() || *it != *(out-1) ) {
				*out++ = *it;
			}
		}
		values.erase( out, values.end() );
	}

	static void AppendCubicLevelCrossings(
		const Scalar (&temperature)[4],
		const Scalar threshold,
		const Scalar tBegin,
		const Scalar tEnd,
		std::vector<Scalar>& breakpoints
		)
	{
		// Recover the cubic in u from four equally spaced values.  Using
		// the dimensionless cell parameter keeps the solve independent of
		// scene scale and avoids powers of a potentially large ray distance.
		const Scalar thirdDifference = temperature[3] - 3.0*temperature[2] +
			3.0*temperature[1] - temperature[0];
		const Scalar a = 4.5*thirdDifference;
		const Scalar secondDifference = temperature[2] -
			2.0*temperature[1] + temperature[0];
		const Scalar b = 4.5*secondDifference - a;
		const Scalar c = 3.0*(temperature[1]-temperature[0]) - a/9.0 - b/3.0;
		Scalar coeff[4] = { a, b, c, temperature[0] - threshold };

		std::vector<Scalar> partition;
		partition.reserve(4);
		partition.push_back(0.0);
		AppendDerivativeRoots( coeff, partition );
		partition.push_back(1.0);
		SortUniqueUnitIntervalValues(partition);

		for( size_t i = 0; i < partition.size(); ++i ) {
			const Scalar u = partition[i];
			const Scalar value = EvaluateCubic(coeff,u);
			if( u > 0.0 && u < 1.0 &&
				CubicValueIsNumericalZero(coeff,u,value) ) {
				breakpoints.push_back( tBegin + u*(tEnd-tBegin) );
			}
		}

		for( size_t i = 0; i + 1 < partition.size(); ++i ) {
			Scalar lo = partition[i];
			Scalar hi = partition[i+1];
			Scalar flo = EvaluateCubic(coeff,lo);
			const Scalar fhi = EvaluateCubic(coeff,hi);
			if( CubicValueIsNumericalZero(coeff,lo,flo) ||
				CubicValueIsNumericalZero(coeff,hi,fhi) ||
				(flo < 0.0) == (fhi < 0.0) ) continue;

			for( unsigned int iteration = 0; iteration < 64u; ++iteration ) {
				const Scalar middle = 0.5*(lo+hi);
				if( middle == lo || middle == hi ) break;
				const Scalar fmiddle = EvaluateCubic(coeff,middle);
				if( CubicValueIsNumericalZero(coeff,middle,fmiddle) ) {
					lo = hi = middle;
					break;
				}
				if( (flo < 0.0) != (fmiddle < 0.0) ) {
					hi = middle;
				} else {
					lo = middle;
					flo = fmiddle;
				}
			}
			const Scalar root = 0.5*(lo+hi);
			if( root > 0.0 && root < 1.0 ) {
				breakpoints.push_back( tBegin + root*(tEnd-tBegin) );
			}
		}
	}

	static Scalar StablePositiveProduct(
		const std::initializer_list<Scalar>& factors
		)
	{
		Scalar mantissa = 1.0;
		int exponent = 0;
		for( const Scalar factor : factors ) {
			if( RISE::IsZeroDouble(factor) ) return 0.0;
			if( factor < 0.0 || !RISE::IsFiniteDouble(factor) ) {
				return -1.0;
			}
			int factorExponent = 0;
			const Scalar factorMantissa = std::frexp( factor, &factorExponent );
			int adjustment = 0;
			mantissa = std::frexp( mantissa*factorMantissa, &adjustment );
			exponent += factorExponent + adjustment;
		}
		const Scalar result = std::ldexp( mantissa, exponent );
		return RISE::IsZeroDouble(result) ? -1.0 : result;
	}

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
		return StablePositiveProduct( {x1-x0,y1-y0,z1-z0} );
	}

	class ConstituentHGPhaseClosure :
		public virtual IPhaseFunction,
		public virtual Implementation::Reference
	{
		const Scalar m_hotWeight;
		const Scalar m_coolWeight;
		const Scalar m_condWeight;
		const Scalar m_hotG;
		const Scalar m_coolG;
		const Scalar m_condG;

	protected:
		~ConstituentHGPhaseClosure() override = default;

	public:
		ConstituentHGPhaseClosure(
			const Scalar hotScattering,
			const Scalar coolScattering,
			const Scalar condScattering,
			const Scalar hotG,
			const Scalar coolG,
			const Scalar condG
			) :
		  m_hotWeight( hotScattering / (hotScattering + coolScattering + condScattering) ),
		  m_coolWeight( coolScattering / (hotScattering + coolScattering + condScattering) ),
		  m_condWeight( condScattering / (hotScattering + coolScattering + condScattering) ),
		  m_hotG( hotG ),
		  m_coolG( coolG ),
		  m_condG( condG )
		{
		}

		Scalar Evaluate( const Vector3& wi, const Vector3& wo ) const override
		{
			const Scalar cosTheta = Vector3Ops::Dot( wi, wo );
			return m_hotWeight * HenyeyGreensteinPhaseFunction::EvaluateWithG(
				cosTheta, m_hotG ) +
				m_coolWeight * HenyeyGreensteinPhaseFunction::EvaluateWithG(
					cosTheta, m_coolG ) +
				m_condWeight * HenyeyGreensteinPhaseFunction::EvaluateWithG(
					cosTheta, m_condG );
		}

		Vector3 Sample( const Vector3& wi, ISampler& sampler ) const override
		{
			const Scalar select = sampler.Get1D();
			const Scalar g = select < m_hotWeight ? m_hotG :
				(select < m_hotWeight + m_coolWeight ? m_coolG : m_condG);
			return HenyeyGreensteinPhaseFunction::SampleWithG( wi, sampler, g );
		}

		Scalar Pdf( const Vector3& wi, const Vector3& wo ) const override
		{
			return Evaluate( wi, wo );
		}

		Scalar GetMeanCosine() const override
		{
			return m_hotWeight * m_hotG + m_coolWeight * m_coolG +
				m_condWeight * m_condG;
		}
	};

	static RISEPel FirePelResponse( const Scalar nm )
	{
		XYZPel xyz;
		if( !ColorUtils::XYZFromNM( xyz, nm ) ) return RISEPel( 0.0 );
		static const Scalar yIntegral =
			ColorUtils::CIE_Y_Integral( 380.0, 780.0 );
		if( !RISE::IsFiniteDouble(yIntegral) || yIntegral <= 0.0 ) {
			return RISEPel( 0.0 );
		}
		return ColorUtils::XYZtoRec709RGBMatrixOnly( xyz ) * (1.0/yIntegral);
	}

	static RISEPel IntegrateFirePelResponsePower( const Scalar exponent )
	{
		RISEPel result( 0.0 );
		for( unsigned int channel = 0; channel < 3u; ++channel ) {
			result[channel] = GaussLegendre21::IntegrateVisibleBand(
				[exponent,channel]( const Scalar nm ) {
					return FirePelResponse(nm)[channel] *
						pow( Scalar(633.0)/nm, exponent );
				} );
		}
		return result;
	}

	static Scalar IntegrateFireSamplingPower( const Scalar exponent )
	{
		return GaussLegendre21::IntegrateVisibleBand(
			[exponent]( const Scalar nm ) {
				XYZPel xyz;
				if( !ColorUtils::XYZFromNM( xyz, nm ) ) return Scalar(0.0);
				const Scalar weight = xyz.X + xyz.Y + xyz.Z;
				return weight * pow( Scalar(633.0)/nm, exponent );
			} );
	}

	class ConstituentHGPelPhaseClosure :
		public virtual IPhaseFunction,
		public virtual Implementation::Reference
	{
		RISEPel m_hotWeight;
		RISEPel m_coolWeight;
		RISEPel m_condWeight;
		Scalar m_hotProposalWeight;
		Scalar m_coolProposalWeight;
		Scalar m_condProposalWeight;
		const Scalar m_hotG;
		const Scalar m_coolG;
		const Scalar m_condG;

	protected:
		~ConstituentHGPelPhaseClosure() override = default;

	public:
		ConstituentHGPelPhaseClosure(
			const RISEPel& hotScattering,
			const RISEPel& coolScattering,
			const RISEPel& condScattering,
			const Scalar hotProposal,
			const Scalar coolProposal,
			const Scalar condProposal,
			const Scalar hotG,
			const Scalar coolG,
			const Scalar condG
			) :
		  m_hotWeight( 0.0 ),
		  m_coolWeight( 0.0 ),
		  m_condWeight( 0.0 ),
		  m_hotProposalWeight( hotProposal/(hotProposal+coolProposal+condProposal) ),
		  m_coolProposalWeight( coolProposal/(hotProposal+coolProposal+condProposal) ),
		  m_condProposalWeight( condProposal/(hotProposal+coolProposal+condProposal) ),
		  m_hotG( hotG ),
		  m_coolG( coolG ),
		  m_condG( condG )
		{
			for( unsigned int channel = 0; channel < 3u; ++channel ) {
				const Scalar total = hotScattering[channel] +
					coolScattering[channel] + condScattering[channel];
				if( total > 0.0 ) {
					m_hotWeight[channel] = hotScattering[channel]/total;
					m_coolWeight[channel] = coolScattering[channel]/total;
					m_condWeight[channel] = condScattering[channel]/total;
				}
			}
		}

		Scalar Evaluate( const Vector3& wi, const Vector3& wo ) const override
		{
			return PdfProposal( wi, wo );
		}

		RISEPel EvaluatePel( const Vector3& wi, const Vector3& wo ) const override
		{
			const Scalar cosTheta = Vector3Ops::Dot( wi, wo );
			const Scalar hot = HenyeyGreensteinPhaseFunction::EvaluateWithG(
				cosTheta, m_hotG );
			const Scalar cool = HenyeyGreensteinPhaseFunction::EvaluateWithG(
				cosTheta, m_coolG );
			const Scalar cond = HenyeyGreensteinPhaseFunction::EvaluateWithG(
				cosTheta, m_condG );
			return m_hotWeight*hot + m_coolWeight*cool + m_condWeight*cond;
		}

		Vector3 Sample( const Vector3& wi, ISampler& sampler ) const override
		{
			const Scalar select = sampler.Get1D();
			const Scalar g = select < m_hotProposalWeight ? m_hotG :
				(select < m_hotProposalWeight + m_coolProposalWeight ?
					m_coolG : m_condG);
			return HenyeyGreensteinPhaseFunction::SampleWithG( wi, sampler, g );
		}

		Scalar Pdf( const Vector3& wi, const Vector3& wo ) const override
		{
			return PdfProposal( wi, wo );
		}

		Scalar PdfProposal( const Vector3& wi, const Vector3& wo ) const override
		{
			const Scalar cosTheta = Vector3Ops::Dot( wi, wo );
			return m_hotProposalWeight *
				HenyeyGreensteinPhaseFunction::EvaluateWithG(cosTheta,m_hotG) +
				m_coolProposalWeight *
				HenyeyGreensteinPhaseFunction::EvaluateWithG(cosTheta,m_coolG) +
				m_condProposalWeight *
				HenyeyGreensteinPhaseFunction::EvaluateWithG(cosTheta,m_condG);
		}

		Scalar GetMeanCosine() const override
		{
			return m_hotProposalWeight*m_hotG + m_coolProposalWeight*m_coolG +
				m_condProposalWeight*m_condG;
		}
	};

	static Scalar SmoothHotFraction(
		const Scalar temperature,
		const Scalar minimumTemperature,
		const Scalar maximumTemperature
		)
	{
		if( temperature <= minimumTemperature ) return 0.0;
		if( temperature >= maximumTemperature ) return 1.0;
		const Scalar x = (temperature-minimumTemperature)/
			(maximumTemperature-minimumTemperature);
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

	static Scalar TrackingSigmaTVisible(
		const Scalar sceneUnitMeters,
		const FireOpticsPreset& optics
		)
	{
		if( !RISE::IsFiniteDouble( sceneUnitMeters ) ||
			sceneUnitMeters <= 0.0 || !optics.IsValid() ) {
			return 0.0;
		}
		return sceneUnitMeters * optics.MaximumExtinctionMassVisible();
	}

	class MultichannelExtinctionAccessor :
		public virtual IVolumeAccessor,
		public virtual Implementation::Reference
	{
	protected:
		const IVolumeAccessor* m_carbon;
		const IVolumeAccessor* m_temperature;
		const IVolumeAccessor* m_condensed;
		Scalar m_hotExtinctionMass;
		Scalar m_coolExtinctionMass;
		Scalar m_condExtinctionMass;
		Scalar m_massMajorant;
		Scalar m_hotFractionMinK;
		Scalar m_hotFractionMaxK;

		virtual ~MultichannelExtinctionAccessor()
		{
			safe_release( m_carbon );
			safe_release( m_temperature );
			safe_release( m_condensed );
		}

		Scalar Evaluate( const Scalar x, const Scalar y, const Scalar z ) const
		{
			if( m_massMajorant <= 0.0 ) return 0.0;
			const Scalar carbon = m_carbon->GetValue( x, y, z );
			const Scalar phi = SmoothHotFraction(
				m_temperature->GetValue(x,y,z),m_hotFractionMinK,m_hotFractionMaxK );
			const Scalar massExtinction =
				phi * m_hotExtinctionMass + (1.0 - phi) * m_coolExtinctionMass;
			const Scalar condensed = m_condensed ? m_condensed->GetValue( x, y, z ) : 0.0;
			return (carbon * massExtinction + condensed * m_condExtinctionMass) /
				m_massMajorant;
		}

	public:
		MultichannelExtinctionAccessor(
			const IVolumeAccessor& carbon,
			const IVolumeAccessor& temperature,
			const IVolumeAccessor* condensed,
			const Scalar hotExtinctionMass,
			const Scalar coolExtinctionMass,
			const Scalar condExtinctionMass,
			const Scalar hotFractionMinK,
			const Scalar hotFractionMaxK
			) :
		  m_carbon( &carbon ),
		  m_temperature( &temperature ),
		  m_condensed( condensed ),
		  m_hotExtinctionMass( hotExtinctionMass ),
		  m_coolExtinctionMass( coolExtinctionMass ),
		  m_condExtinctionMass( condExtinctionMass ),
		  m_massMajorant( fmax( hotExtinctionMass,
			fmax( coolExtinctionMass, condExtinctionMass ) ) ),
		  m_hotFractionMinK( hotFractionMinK ),
		  m_hotFractionMaxK( hotFractionMaxK )
		{
			m_carbon->addref();
			m_temperature->addref();
			if( m_condensed ) m_condensed->addref();
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

	class SummedConcentrationAccessor :
		public virtual IVolumeAccessor,
		public virtual Implementation::Reference
	{
		const IVolumeAccessor* m_carbon;
		const IVolumeAccessor* m_condensed;

	protected:
		~SummedConcentrationAccessor() override
		{
			safe_release( m_carbon );
			safe_release( m_condensed );
		}

		Scalar Evaluate( const Scalar x, const Scalar y, const Scalar z ) const
		{
			return m_carbon->GetValue( x, y, z ) +
				(m_condensed ? m_condensed->GetValue( x, y, z ) : 0.0);
		}

	public:
		SummedConcentrationAccessor(
			const IVolumeAccessor& carbon,
			const IVolumeAccessor* condensed
			) :
		  m_carbon( &carbon ),
		  m_condensed( condensed )
		{
			m_carbon->addref();
			if( m_condensed ) m_condensed->addref();
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
			// Derived from immutable baked concentration accessors.
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
		const bool requirePositive,
		Scalar* maximumValue = 0
		)
	{
		if( maximumValue ) *maximumValue = 0.0;
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
					if( maximumValue ) *maximumValue = fmax(*maximumValue,value);
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

Scalar HeterogeneousMedium::PelTrackingMajorant() const
{
	return m_sigma_t_majorant;
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
	const Scalar pelMajorant = PelTrackingMajorant();
	const Scalar majorantRatio = m_sigma_t_majorant > 0.0 ?
		pelMajorant/m_sigma_t_majorant : 1.0;
	Scalar scatterDist = 0;
	bool didScatter = false;

	struct DeltaTrackingVisitor
	{
		const HeterogeneousMedium* self;
		const Ray* pRay;
		ISampler* pSampler;
		Scalar majorantRatio;
		Scalar* pScatterDist;
		bool* pDidScatter;

		bool operator()( Scalar tCellEntry, Scalar tCellExit, Scalar cellMajorantBase )
		{
			const Scalar cellMajorant = cellMajorantBase*majorantRatio;
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

				// Evaluate the actual projected local extinction.  For ordinary
				// heterogeneous media this reduces exactly to max_sigma_t*density;
				// fire media are not separable in one density channel.
				const Point3 samplePt = Point3Ops::mkPoint3(
					pRay->origin, pRay->Dir() * t );
				const Scalar sigma_t_local = ColorMath::MaxValue(
					self->GetCoefficients(samplePt).sigma_t );

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
	visitor.majorantRatio = majorantRatio;
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
	const Scalar pelMajorant = PelTrackingMajorant();
	const Scalar majorantRatio = m_sigma_t_majorant > 0.0 ?
		pelMajorant/m_sigma_t_majorant : 1.0;
	struct RatioTrackingVisitor
	{
		const HeterogeneousMedium* self;
		const Ray* pRay;
		RandomNumberGenerator* pRng;
		RISEPel* pW;
		Scalar majorantRatio;

		bool operator()( Scalar tCellEntry, Scalar tCellExit, Scalar cellMajorantBase )
		{
			const Scalar cellMajorant = cellMajorantBase*majorantRatio;
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
				const MediumCoefficients coefficients =
					self->GetCoefficients( samplePt );

				// Per-channel ratio tracking weight
				for( int ch = 0; ch < 3; ch++ )
				{
					const Scalar sigma_t_ch = coefficients.sigma_t[ch];
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
	visitor.majorantRatio = majorantRatio;

	m_pMajorantGrid->TraverseRay( ray, 0.0, dist, visitor );

	return w;
}

RISEPel HeterogeneousMedium::EvalDeterministicTransmittancePel(
	const Ray& ray,
	const Scalar dist
	) const
{
	RISEPel transmittance;
	for( unsigned int channel = 0; channel < 3u; ++channel ) {
		transmittance[channel] = exp(
			-EvalDeterministicOpticalDepthPelChannel(ray,dist,channel) );
	}
	return transmittance;
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
		ray, targetDist, sigma_t_eff, false, 0.0, -1 );
}

Scalar HeterogeneousMedium::EvalDeterministicOpticalDepthNM(
	const Ray& ray,
	const Scalar targetDist,
	const Scalar nm
	) const
{
	return EvalDeterministicOpticalDepthImpl(
		ray, targetDist, 1.0, true, nm, -1 );
}

Scalar HeterogeneousMedium::EvalDeterministicOpticalDepthPelChannel(
	const Ray& ray,
	const Scalar targetDist,
	const unsigned int channel
	) const
{
	return EvalDeterministicOpticalDepthImpl(
		ray,targetDist,1.0,false,0.0,static_cast<int>(channel) );
}

Scalar HeterogeneousMedium::InterpolationAccessorOffset(
	const unsigned int dimension
	) const
{
	return 0.5*Scalar(dimension);
}

void HeterogeneousMedium::AppendOpticalDepthBreakpoints(
	const Ray&,
	const Scalar,
	const Scalar,
	std::vector<Scalar>&
	) const
{
}

Scalar HeterogeneousMedium::EvalDeterministicOpticalDepthImpl(
	const Ray& ray,
	const Scalar targetDist,
	const Scalar,
	const bool spectral,
	const Scalar nm,
	const int pelChannel
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
	// consecutive knot planes, 7-point Gauss-Legendre quadrature
	// integrates the density exactly:
	//
	//   Nearest-neighbor ('n'): piecewise constant (degree 0)
	//   Trilinear ('t'):  cubic along ray per cell (degree 3)
	//   Tricubic Catmull-Rom ('c'):  degree 9 along ray per cell
	//     (3D tensor product of cubics, each axis linear in t)
	//
	// 7-point GL is exact for polynomials up to degree 2*7-1 = 13.
	// Fire media add the roots of T(t)-700/900 as sub-panel boundaries,
	// making carbon*phi(T) polynomial (degree at most 12) on each panel.
	//
	// COORDINATE MAPPING:
	//   Legacy LookupDensity maps world → accessor via:
	//     vx = ((world_x - bboxMin.x) / extent.x - 0.5) * volWidth
	//   Knot planes are at integer vx = n.  Solving for world_x:
	//     world_x = (n/volWidth + 0.5) * extent.x + bboxMin.x
	//             = (n + volWidth/2.0) * cellSzX + bboxMin.x
	//   For even volWidth, this coincides with bboxMin + k*cellSz.
	//   For odd volWidth, there is a half-voxel offset.  Painter-baked
	//   fire channels use floor(volWidth/2)+1/2 because integer samples
	//   denote voxel centres.  The virtual accessor offset keeps both
	//   mappings on their actual knot planes rather than naively splitting
	//   the AABB.
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

	// Accessor-coordinate offsets.  Ordinary volume accessors use dim/2;
	// painter-baked fire channels use floor(dim/2)+1/2 because their
	// integer samples denote voxel centres.
	const Scalar offsetW = InterpolationAccessorOffset(m_volWidth);
	const Scalar offsetH = InterpolationAccessorOffset(m_volHeight);
	const Scalar offsetD = InterpolationAccessorOffset(m_volDepth);

	// Starting point (nudge slightly inside to avoid landing on face)
	const Point3 startPt = ray.PointAtLength( tEntry + 1e-10 );

	// Map start point to accessor coordinates and take floor to get
	// the cell index.  Accessor coord:
	//   vx = (world_x - bboxMin.x) * invCellSzX - offsetW
	// Cell index = floor(vx).
	int cx = (int)floor( (startPt.x - m_bboxMin.x) * invCellSzX - offsetW );
	int cy = (int)floor( (startPt.y - m_bboxMin.y) * invCellSzY - offsetH );
	int cz = (int)floor( (startPt.z - m_bboxMin.z) * invCellSzZ - offsetD );

	// Valid accessor-space cells over [-offset, dimension-offset].
	const int minCx = (int)floor( -offsetW );
	const int maxCx = (int)ceil( Scalar(m_volWidth)-offsetW ) - 1;
	const int minCy = (int)floor( -offsetH );
	const int maxCy = (int)ceil( Scalar(m_volHeight)-offsetH ) - 1;
	const int minCz = (int)floor( -offsetD );
	const int maxCz = (int)ceil( Scalar(m_volDepth)-offsetD ) - 1;

	if( cx < minCx ) cx = minCx;  if( cx > maxCx ) cx = maxCx;
	if( cy < minCy ) cy = minCy;  if( cy > maxCy ) cy = maxCy;
	if( cz < minCz ) cz = minCz;  if( cz > maxCz ) cz = maxCz;

	// DDA step directions
	const int stepX = (ray.Dir().x >= 0) ? 1 : -1;
	const int stepY = (ray.Dir().y >= 0) ? 1 : -1;
	const int stepZ = (ray.Dir().z >= 0) ? 1 : -1;

	// tMax: ray parameter to reach the next knot plane in each axis.
	// Knot plane at accessor integer n maps to world:
	//   world_x = (n + offsetW) * cellSzX + bboxMin.x
	// Next face from cell cx in +x direction: n = cx + 1
	// Next face from cell cx in -x direction: n = cx
	Scalar tMaxX, tMaxY, tMaxZ;
	Scalar tDeltaX, tDeltaY, tDeltaZ;

	if( fabs( ray.Dir().x ) > 1e-20 )
	{
		const int nextN = (stepX > 0) ? cx + 1 : cx;
		const Scalar nextFaceX = m_bboxMin.x + (Scalar(nextN) + offsetW) * cellSzX;
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
		const Scalar nextFaceY = m_bboxMin.y + (Scalar(nextN) + offsetH) * cellSzY;
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
		const Scalar nextFaceZ = m_bboxMin.z + (Scalar(nextN) + offsetD) * cellSzZ;
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
			static const Scalar glNodes[7] = {
				-0.949107912342758524526189684048,
				-0.741531185599394439863864773281,
				-0.405845151377397166906606412077,
				 0.0,
				 0.405845151377397166906606412077,
				 0.741531185599394439863864773281,
				 0.949107912342758524526189684048 };
			static const Scalar glWeights[7] = {
				0.129484966168869693270611432679,
				0.279705391489276667901467771424,
				0.381830050505118944950369775489,
				0.417959183673469387755102040816,
				0.381830050505118944950369775489,
				0.279705391489276667901467771424,
				0.129484966168869693270611432679 };

			std::vector<Scalar> panels;
			panels.reserve(4);
			panels.push_back(t);
			AppendOpticalDepthBreakpoints( ray, t, tNext, panels );
			panels.push_back(tNext);
			std::sort( panels.begin(), panels.end() );
			for( size_t panel = 0; panel + 1 < panels.size(); ++panel ) {
				const Scalar panelBegin = panels[panel];
				const Scalar panelEnd = panels[panel+1];
				if( panelEnd <= panelBegin ) continue;
				const Scalar halfLen = 0.5*(panelEnd-panelBegin);
				const Scalar midPt = 0.5*(panelBegin+panelEnd);
				Scalar segIntegral = 0.0;
				for( unsigned int q = 0; q < 7u; ++q ) {
					const Scalar tq = midPt + halfLen*glNodes[q];
					const Point3 samplePt = Point3Ops::mkPoint3(
						ray.origin, ray.Dir()*tq );
					Scalar dq = 0.0;
					if( spectral ) {
						dq = GetCoefficientsNM( samplePt, nm ).sigma_t;
					} else {
						const RISEPel sigmaT = GetCoefficients(samplePt).sigma_t;
						dq = pelChannel >= 0 ? sigmaT[pelChannel] :
							ColorMath::MaxValue(sigmaT);
					}
					segIntegral += glWeights[q]*dq;
				}
				opticalDepth += halfLen*segIntegral;
			}
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
	// We compute T_real deterministically via knot-aligned, root-split
	// 7-point Gauss-Legendre panels (see EvalDeterministicOpticalDepth).
	// This avoids
	// the stochastic ratio-tracking path through EvalTransmittance,
	// which would randomize the MIS balance-heuristic denominator.
	//
	// sigma_t_eff is the scalar extinction used by the delta tracking
	// sampler: MaxValue(m_max_sigma_t) for RGB.
	//
	// Reference: Miller, Georgiev, Jarosz, SIGGRAPH 2019 §3.3
	const Scalar targetDist = scattered ? t : maxDist;
	const Scalar tau = EvalDeterministicOpticalDepth(
		ray, targetDist, PelTrackingMajorant() );
	const Scalar T_real = exp( -tau );

	if( scattered )
	{
		const Point3 pt = ray.PointAtLength( t );
		return ColorMath::MaxValue(GetCoefficients(pt).sigma_t) * T_real;
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
  MultichannelHeterogeneousMedium(
	  carbonPainter, temperaturePainter, 0,
	  volWidth, volHeight, volDepth, bboxMin, bboxMax, sceneUnitMeters,
	  sootEm, sootDensity, sootAlbedoHot, sootGHot,
	  smokeKmCarbon, smokeNCarbon, smokeAlbedoCarbon, smokeGCarbon,
	  0.0, 0.0, 0.0, 0.0, phase )
{
}

MultichannelHeterogeneousMedium::MultichannelHeterogeneousMedium(
	const IScalarPainter& carbonPainter,
	const IScalarPainter& temperaturePainter,
	const IScalarPainter* condensedPainter,
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
	const Scalar smokeKmCond,
	const Scalar smokeNCond,
	const Scalar smokeAlbedoCond,
	const Scalar smokeGCond,
	const IPhaseFunction& phase
	) :
	MultichannelHeterogeneousMedium(
		carbonPainter, temperaturePainter, condensedPainter,
		0, 0, 0, 0, 0, 0,
		0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
		volWidth, volHeight, volDepth, bboxMin, bboxMax, sceneUnitMeters,
		sootEm, sootDensity, sootAlbedoHot, sootGHot,
		smokeKmCarbon, smokeNCarbon, smokeAlbedoCarbon, smokeGCarbon,
		smokeKmCond, smokeNCond, smokeAlbedoCond, smokeGCond, phase )
{
}

MultichannelHeterogeneousMedium::MultichannelHeterogeneousMedium(
	const IScalarPainter& carbonPainter,
	const IScalarPainter& temperaturePainter,
	const IScalarPainter* condensedPainter,
	const IScalarPainter* chemCHPainter,
	const IScalarPainter* chemC2Painter,
	const IScalarPainter* chemCO2Painter,
	const IFunction1D* chemCHSPD,
	const IFunction1D* chemC2SPD,
	const IFunction1D* chemCO2SPD,
	const Scalar chemCHIntervalMin,
	const Scalar chemCHIntervalMax,
	const Scalar chemC2IntervalMin,
	const Scalar chemC2IntervalMax,
	const Scalar chemCO2IntervalMin,
	const Scalar chemCO2IntervalMax,
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
	const Scalar smokeKmCond,
	const Scalar smokeNCond,
	const Scalar smokeAlbedoCond,
	const Scalar smokeGCond,
	const IPhaseFunction& phase
	) :
	MultichannelHeterogeneousMedium(
		carbonPainter, temperaturePainter, condensedPainter,
		chemCHPainter, chemC2Painter, chemCO2Painter,
		chemCHSPD, chemC2SPD, chemCO2SPD,
		chemCHIntervalMin, chemCHIntervalMax,
		chemC2IntervalMin, chemC2IntervalMax,
		chemCO2IntervalMin, chemCO2IntervalMax,
		volWidth, volHeight, volDepth, bboxMin, bboxMax, sceneUnitMeters,
		FireOpticsPreset::CreateExplicitSyntheticFixture(
			sootEm, sootDensity, sootAlbedoHot, sootGHot,
			smokeKmCarbon, smokeNCarbon, smokeAlbedoCarbon, smokeGCarbon,
			smokeKmCond, smokeNCond, smokeAlbedoCond, smokeGCond ),
		phase )
{
}

MultichannelHeterogeneousMedium::MultichannelHeterogeneousMedium(
	const IScalarPainter& carbonPainter,
	const IScalarPainter& temperaturePainter,
	const IScalarPainter* condensedPainter,
	const IScalarPainter* chemCHPainter,
	const IScalarPainter* chemC2Painter,
	const IScalarPainter* chemCO2Painter,
	const IFunction1D* chemCHSPD,
	const IFunction1D* chemC2SPD,
	const IFunction1D* chemCO2SPD,
	const Scalar chemCHIntervalMin,
	const Scalar chemCHIntervalMax,
	const Scalar chemC2IntervalMin,
	const Scalar chemC2IntervalMax,
	const Scalar chemCO2IntervalMin,
	const Scalar chemCO2IntervalMax,
	const unsigned int volWidth,
	const unsigned int volHeight,
	const unsigned int volDepth,
	const Point3& bboxMin,
	const Point3& bboxMax,
	const Scalar sceneUnitMeters,
	const FireOpticsPreset& optics,
	const IPhaseFunction& phase
	) :
  HeterogeneousMedium(
	  TrackingSigmaTVisible( sceneUnitMeters, optics ),
	  phase, volWidth, volHeight, volDepth, bboxMin, bboxMax ),
  m_pCarbonAccessor( 0 ),
  m_pTemperatureAccessor( 0 ),
	  m_pCondensedAccessor( 0 ),
	  m_pChemAccessor{ 0, 0, 0 },
	  m_pChemSPD{ 0, 0, 0 },
	  m_chemIntervalMin{ chemCHIntervalMin, chemC2IntervalMin, chemCO2IntervalMin },
	  m_chemIntervalMax{ chemCHIntervalMax, chemC2IntervalMax, chemCO2IntervalMax },
	  m_chemSPDArea{ 0.0, 0.0, 0.0 },
	  m_optics( optics ),
	  m_hasNonzeroCondensedInventory( false ),
  m_sceneUnitMeters( sceneUnitMeters ),
  m_sootEm( optics.PelApproximateEffectiveAbsorption633() ),
  m_sootDensity( optics.SootDensityKgM3() ),
  m_sootAlbedoHot( optics.PelApproximateHotAlbedo() ),
  m_sootGHot( optics.PelApproximateHotG() ),
  m_smokeKmCarbon( optics.PelApproximateCoolKm633() ),
  m_smokeNCarbon( optics.PelApproximateCoolExponent() ),
  m_smokeAlbedoCarbon( optics.PelApproximateCoolAlbedo() ),
  m_smokeGCarbon( optics.PelApproximateCoolG() ),
	  m_smokeKmCond( optics.PelApproximateCondensedKm633() ),
	  m_smokeNCond( optics.PelApproximateCondensedExponent() ),
	  m_smokeAlbedoCond( optics.PelApproximateCondensedAlbedo() ),
	  m_smokeGCond( optics.PelApproximateCondensedG() ),
  m_hotAbsorptionMass633( optics.HotAbsorptionMass(633.0) ),
  m_hotExtinctionMass633( optics.HotExtinctionMass(633.0) ),
	  m_coolExtinctionMass633( optics.CoolExtinctionMass(633.0) ),
	  m_condExtinctionMass633( optics.CondensedExtinctionMass(633.0) ),
	  m_pelResponseMass( 0.0 ),
	  m_pelHotMean( 0.0 ),
	  m_pelCoolMean( 0.0 ),
	  m_pelCondMean( 0.0 ),
	  m_samplingHotMass( 0.0 ),
	  m_samplingCoolMass( 0.0 ),
	  m_samplingCondMass( 0.0 ),
	  m_effectiveAbsorptionAblation( NoEffectiveAbsorptionAblation ),
	  m_emissionBinSize( 0, 0, 0 ),
	  m_thermalEmissionImportance( 0.0 ),
  m_minPositiveThermalEmissionPdf( 0.0 ),
  m_valid( false )
{
	const IScalarPainter* chemPainters[3] = {
		chemCHPainter, chemC2Painter, chemCO2Painter };
	const IFunction1D* chemSPDs[3] = { chemCHSPD, chemC2SPD, chemCO2SPD };
	const bool hasAnyChem = chemPainters[0] || chemPainters[1] || chemPainters[2] ||
		chemSPDs[0] || chemSPDs[1] || chemSPDs[2];
	const bool hasAllChem = chemPainters[0] && chemPainters[1] && chemPainters[2] &&
		chemSPDs[0] && chemSPDs[1] && chemSPDs[2];
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
		sceneUnitMeters > 0.0 && optics.IsValid() &&
		m_sootEm >= 0.0 && m_sootDensity > 0.0 &&
		m_sootAlbedoHot >= 0.0 && m_sootAlbedoHot < 1.0 &&
		m_sootGHot > -1.0 && m_sootGHot < 1.0 &&
		m_smokeKmCarbon >= 0.0 && m_smokeNCarbon >= 0.0 &&
		m_smokeAlbedoCarbon >= 0.0 && m_smokeAlbedoCarbon <= 1.0 &&
		m_smokeGCarbon > -1.0 && m_smokeGCarbon < 1.0 &&
		(!condensedPainter ||
			(m_smokeKmCond >= 0.0 && m_smokeNCond >= 0.0 &&
			m_smokeAlbedoCond >= 0.0 && m_smokeAlbedoCond <= 1.0 &&
			m_smokeGCond > -1.0 && m_smokeGCond < 1.0)) &&
		(!hasAnyChem || hasAllChem);
	if( !validDimensions || !validBounds || !validOptics ||
		carbonPainter.HasPerChannelVariation() || temperaturePainter.HasPerChannelVariation() ||
		(condensedPainter && condensedPainter->HasPerChannelVariation()) ||
		(hasAllChem && (chemPainters[0]->HasPerChannelVariation() ||
			chemPainters[1]->HasPerChannelVariation() ||
			chemPainters[2]->HasPerChannelVariation())) ) {
		GlobalLog()->PrintEasyError(
			"MultichannelHeterogeneousMedium:: invalid lattice, bbox, scalar channel, or constituent optical parameter" );
		return;
	}

	m_pelResponseMass = IntegrateFirePelResponsePower( 0.0 );
	const RISEPel hotIntegral = IntegrateFirePelResponsePower( 1.0 );
	const RISEPel coolIntegral = IntegrateFirePelResponsePower( m_smokeNCarbon );
	const RISEPel condIntegral = condensedPainter ?
		IntegrateFirePelResponsePower( m_smokeNCond ) : m_pelResponseMass;
	bool validPelProjection = true;
	for( unsigned int channel = 0; channel < 3u; ++channel ) {
		const Scalar responseMass = m_pelResponseMass[channel];
		assert( RISE::IsFiniteDouble(responseMass) && responseMass > 0.0 );
		if( !RISE::IsFiniteDouble(responseMass) || responseMass <= 0.0 ) {
			validPelProjection = false;
			continue;
		}
		m_pelHotMean[channel] = hotIntegral[channel]/responseMass;
		m_pelCoolMean[channel] = coolIntegral[channel]/responseMass;
		m_pelCondMean[channel] = condIntegral[channel]/responseMass;
		const Scalar projectedHotSigmaA =
			m_hotAbsorptionMass633*m_pelHotMean[channel];
		const Scalar projectedHotSigmaS = projectedHotSigmaA *
			m_sootAlbedoHot/(1.0-m_sootAlbedoHot);
		const Scalar projectedCoolSigmaA = m_coolExtinctionMass633 *
			(1.0-m_smokeAlbedoCarbon)*m_pelCoolMean[channel];
		const Scalar projectedCoolSigmaS = m_coolExtinctionMass633 *
			m_smokeAlbedoCarbon*m_pelCoolMean[channel];
		const Scalar projectedCondSigmaA = m_condExtinctionMass633 *
			(1.0-m_smokeAlbedoCond)*m_pelCondMean[channel];
		const Scalar projectedCondSigmaS = m_condExtinctionMass633 *
			m_smokeAlbedoCond*m_pelCondMean[channel];
		assert( projectedHotSigmaA >= 0.0 && projectedHotSigmaS >= 0.0 &&
			projectedCoolSigmaA >= 0.0 && projectedCoolSigmaS >= 0.0 &&
			projectedCondSigmaA >= 0.0 && projectedCondSigmaS >= 0.0 );
		if( !RISE::IsFiniteDouble(projectedHotSigmaA) || projectedHotSigmaA < 0.0 ||
			!RISE::IsFiniteDouble(projectedHotSigmaS) || projectedHotSigmaS < 0.0 ||
			!RISE::IsFiniteDouble(projectedCoolSigmaA) || projectedCoolSigmaA < 0.0 ||
			!RISE::IsFiniteDouble(projectedCoolSigmaS) || projectedCoolSigmaS < 0.0 ||
			!RISE::IsFiniteDouble(projectedCondSigmaA) || projectedCondSigmaA < 0.0 ||
			!RISE::IsFiniteDouble(projectedCondSigmaS) || projectedCondSigmaS < 0.0 ) {
			validPelProjection = false;
		}
	}
	if( !validPelProjection ) {
		GlobalLog()->PrintEasyError(
			"MultichannelHeterogeneousMedium:: Pel response mass or projected extinction is non-positive" );
		return;
	}
	m_samplingHotMass = IntegrateFireSamplingPower( 1.0 );
	m_samplingCoolMass = IntegrateFireSamplingPower( m_smokeNCarbon );
	m_samplingCondMass = condensedPainter ?
		IntegrateFireSamplingPower( m_smokeNCond ) : 0.0;
	if( !RISE::IsFiniteDouble(m_samplingHotMass) || m_samplingHotMass <= 0.0 ||
		!RISE::IsFiniteDouble(m_samplingCoolMass) || m_samplingCoolMass <= 0.0 ||
		(condensedPainter &&
			(!RISE::IsFiniteDouble(m_samplingCondMass) || m_samplingCondMass <= 0.0)) ) {
		GlobalLog()->PrintEasyError(
			"MultichannelHeterogeneousMedium:: nonnegative Pel phase-sampling response is invalid" );
		return;
	}

	m_pCarbonAccessor = BakeScalarChannel(
		carbonPainter, volWidth, volHeight, volDepth, bboxMin, bboxMax, "carbon", false );
	if( !m_pCarbonAccessor ) return;

	m_pTemperatureAccessor = BakeScalarChannel(
		temperaturePainter, volWidth, volHeight, volDepth, bboxMin, bboxMax, "temperature", true );
	if( !m_pTemperatureAccessor ) return;
	if( condensedPainter ) {
		Scalar maximumCondensed = 0.0;
		m_pCondensedAccessor = BakeScalarChannel(
			*condensedPainter, volWidth, volHeight, volDepth,
			bboxMin, bboxMax, "condensed", false, &maximumCondensed );
		if( !m_pCondensedAccessor ) return;
		m_hasNonzeroCondensedInventory = maximumCondensed > 0.0;
	}
	if( hasAllChem ) {
		for( unsigned int band = 0; band < 3u; ++band ) {
			m_pChemSPD[band] = chemSPDs[band];
			m_pChemSPD[band]->addref();
		}
		for( unsigned int band = 0; band < 3u; ++band ) {
			m_chemSPDArea[band] = NormalizeChemSPD(
				*m_pChemSPD[band], m_chemIntervalMin[band],
				m_chemIntervalMax[band] );
			if( m_chemSPDArea[band] <= 0.0 ) {
				GlobalLog()->PrintEasyError(
					"MultichannelHeterogeneousMedium:: chem SPD interval or 1 nm trapezoid normalization is invalid" );
				return;
			}
		}
		static const char* chemNames[3] = { "chem_CH", "chem_C2", "chem_CO2" };
		for( unsigned int band = 0; band < 3u; ++band ) {
			m_pChemAccessor[band] = BakeScalarChannel(
				*chemPainters[band], volWidth, volHeight, volDepth,
				bboxMin, bboxMax, chemNames[band], false );
			if( !m_pChemAccessor[band] ) return;
		}
	}

	IVolumeAccessor* trackingAccessor = new MultichannelExtinctionAccessor(
		*m_pCarbonAccessor, *m_pTemperatureAccessor, m_pCondensedAccessor,
		m_hotExtinctionMass633, m_coolExtinctionMass633,
		m_condExtinctionMass633, m_optics.HotFractionMinK(),
		m_optics.HotFractionMaxK() );
	// The local tracking accessor evaluates carbon * k(phi(T)).  Building
	// a majorant from that nonlinear product's corner samples can miss an
	// interior maximum when carbon and temperature gradients oppose each
	// other.  Build the grid from carbon alone and scale it by the global
	// max of every constituent mass extinction (already in
	// m_sigma_t_majorant).  Carbon + condensed is trilinear on the shared
	// lattice, so its per-cell corner maximum is conservative.
	IVolumeAccessor* majorantAccessor = new SummedConcentrationAccessor(
		*m_pCarbonAccessor, m_pCondensedAccessor );
	InitializeTrackingAccessor( *trackingAccessor, *majorantAccessor );
	safe_release( trackingAccessor );
	safe_release( majorantAccessor );
	m_previewFidelity = m_optics.EvaluateFidelity(
		false, m_hasNonzeroCondensedInventory, hasAllChem );
	m_predictiveFidelity = m_optics.EvaluateFidelity(
		true, m_hasNonzeroCondensedInventory, hasAllChem );
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
	safe_release( m_pCondensedAccessor );
	for( unsigned int band = 0; band < 3u; ++band ) {
		safe_release( m_pChemAccessor[band] );
		safe_release( m_pChemSPD[band] );
	}
}

bool MultichannelHeterogeneousMedium::ForTest_SetEffectiveAbsorptionAblation(
	const EffectiveAbsorptionAblation ablation
	)
{
	if( ablation < NoEffectiveAbsorptionAblation || ablation > PresetTiltOnly ) {
		return false;
	}
	m_effectiveAbsorptionAblation = ablation;
	return true;
}

Scalar MultichannelHeterogeneousMedium::AblatedHotAbsorptionMass(
	const Scalar nm
	) const
{
	if( m_effectiveAbsorptionAblation == NoEffectiveAbsorptionAblation ) {
		return m_optics.HotAbsorptionMass(nm);
	}
	const FireOpticsPreset& predictive = FireOpticsPreset::PredictiveV1();
	const FireOpticsPreset& fixture = FireOpticsPreset::SyntheticRegressionV1();
	Scalar effectiveAbsorption = fixture.EffectiveAbsorption(nm);
	if( m_effectiveAbsorptionAblation == PresetMagnitudeOnly ) {
		effectiveAbsorption = predictive.EffectiveAbsorption(550.0);
	} else if( m_effectiveAbsorptionAblation == PresetTiltOnly ) {
		effectiveAbsorption = fixture.EffectiveAbsorption(550.0) *
			predictive.EffectiveAbsorption(nm) /
			predictive.EffectiveAbsorption(550.0);
	}
	const Scalar volumeFractionPerGM3 = 1.0e-3/m_optics.SootDensityKgM3();
	return 6.0*PI*effectiveAbsorption*volumeFractionPerGM3/(nm*1.0e-9);
}

Scalar MultichannelHeterogeneousMedium::NormalizeChemSPD(
	const IFunction1D& curve,
	const Scalar intervalMin,
	const Scalar intervalMax
	)
{
	if( !RISE::IsFiniteDouble(intervalMin) ||
		!RISE::IsFiniteDouble(intervalMax) || intervalMax <= intervalMin ) {
		return 0.0;
	}
	Scalar x = intervalMin;
	Scalar left = curve.Evaluate(x);
	if( !RISE::IsFiniteDouble(left) || left < 0.0 ) return 0.0;
	Scalar integral = 0.0;
	while( x < intervalMax ) {
		const Scalar next = fmin( x + 1.0, intervalMax );
		if( next <= x ) return 0.0;
		const Scalar right = curve.Evaluate(next);
		if( !RISE::IsFiniteDouble(right) || right < 0.0 ) return 0.0;
		integral += 0.5*(left+right)*(next-x);
		if( !RISE::IsFiniteDouble(integral) ) return 0.0;
		x = next;
		left = right;
	}
	return integral > 0.0 ? integral : 0.0;
}

Scalar MultichannelHeterogeneousMedium::LookupChemBand(
	const unsigned int band,
	const Point3& worldPt
	) const
{
	return band < 3u && m_pChemAccessor[band]
		? LookupChannel( *m_pChemAccessor[band], worldPt ) : 0.0;
}

Scalar MultichannelHeterogeneousMedium::NormalizedChemSPD(
	const unsigned int band,
	const Scalar nm
	) const
{
	if( band >= 3u || !m_pChemSPD[band] || m_chemSPDArea[band] <= 0.0 ||
		!RISE::IsFiniteDouble(nm) || nm < 380.0 || nm > 780.0 ||
		nm < m_chemIntervalMin[band] || nm > m_chemIntervalMax[band] ) {
		return 0.0;
	}
	const Scalar value = m_pChemSPD[band]->Evaluate(nm);
	assert( RISE::IsFiniteDouble(value) && value >= 0.0 );
	return RISE::IsFiniteDouble(value) && value > 0.0
		? value/m_chemSPDArea[band] : 0.0;
}

void MultichannelHeterogeneousMedium::AppendChemPanelBreakpoints(
	const Ray& ray,
	const Scalar segmentStart,
	const Scalar segmentEnd,
	std::vector<Scalar>& breakpoints
	) const
{
	const Scalar origins[3] = { ray.origin.x, ray.origin.y, ray.origin.z };
	const Scalar directions[3] = { ray.Dir().x, ray.Dir().y, ray.Dir().z };
	const Scalar bounds[3] = { m_bboxMin.x, m_bboxMin.y, m_bboxMin.z };
	const Scalar extents[3] = { m_bboxExtent.x, m_bboxExtent.y, m_bboxExtent.z };
	const unsigned int dimensions[3] = { m_volWidth, m_volHeight, m_volDepth };
	for( unsigned int axis = 0; axis < 3u; ++axis ) {
		if( directions[axis] == 0.0 ) continue;
		const Scalar scale = Scalar(dimensions[axis])/extents[axis];
		const Scalar offset = InterpolationAccessorOffset(dimensions[axis]);
		const Scalar base = (origins[axis]-bounds[axis])*scale-offset;
		const Scalar slope = directions[axis]*scale;
		const Scalar u0 = base+slope*segmentStart;
		const Scalar u1 = base+slope*segmentEnd;
		long long first = static_cast<long long>(ceil(fmin(u0,u1)));
		long long last = static_cast<long long>(floor(fmax(u0,u1)));
		const long long latticeFirst = static_cast<long long>(ceil(-offset));
		const long long latticeLast = static_cast<long long>(
			floor(Scalar(dimensions[axis])-offset) );
		first = std::max(first,latticeFirst);
		last = std::min(last,latticeLast);
		for( long long knot = first; knot <= last; ++knot ) {
			const Scalar t = (Scalar(knot)-base)/slope;
			if( t > segmentStart && t < segmentEnd ) breakpoints.push_back(t);
		}
	}
}

Scalar MultichannelHeterogeneousMedium::ChemPanelSupport(
	const Ray& ray,
	const Scalar panelStart,
	const Scalar panelEnd
	) const
{
	const Point3 middle = ray.PointAtLength(0.5*(panelStart+panelEnd));
	const Scalar nx = (middle.x-m_bboxMin.x)/m_bboxExtent.x;
	const Scalar ny = (middle.y-m_bboxMin.y)/m_bboxExtent.y;
	const Scalar nz = (middle.z-m_bboxMin.z)/m_bboxExtent.z;
	const int ix = static_cast<int>(floor(
		nx*Scalar(m_volWidth)-InterpolationAccessorOffset(m_volWidth)));
	const int iy = static_cast<int>(floor(
		ny*Scalar(m_volHeight)-InterpolationAccessorOffset(m_volHeight)));
	const int iz = static_cast<int>(floor(
		nz*Scalar(m_volDepth)-InterpolationAccessorOffset(m_volDepth)));
	Scalar maxima[3] = { 0.0, 0.0, 0.0 };
	for( int dz = 0; dz <= 1; ++dz ) {
		for( int dy = 0; dy <= 1; ++dy ) {
			for( int dx = 0; dx <= 1; ++dx ) {
				for( unsigned int band = 0; band < 3u; ++band ) {
					maxima[band] = fmax( maxima[band],
						m_pChemAccessor[band]->GetValue(ix+dx,iy+dy,iz+dz) );
				}
			}
		}
	}
	return maxima[0]+maxima[1]+maxima[2];
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
	Scalar maxCondensed = 0.0;
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
				if( m_pCondensedAccessor ) {
					maxCondensed = fmax( maxCondensed,
						m_pCondensedAccessor->GetValue( ix, iy, iz ) );
				}
			}
		}
	}
	if( (maxCarbon <= 0.0 && maxCondensed <= 0.0) || maxTemperature <= 0.0 ) return 0.0;

	const Scalar hotAbsorptionMax = m_optics.MaximumHotAbsorptionMassVisible();
	const Scalar coolAbsorptionMax = m_optics.MaximumCoolAbsorptionMassVisible();
	const Scalar carbonAbsorptionMax = fmax( hotAbsorptionMax, coolAbsorptionMax );
	const Scalar condAbsorptionMax = m_optics.MaximumCondensedAbsorptionMassVisible();

	// Wien's displacement gives the maximum of B_lambda(T) on the band.
	// Clamp that analytic peak to [380,780]; separating the absorption and
	// Planck maxima is intentionally conservative.
	const Scalar wienPeakNM = 2.897771955e6 / maxTemperature;
	const Scalar planckPeakNM = fmax( Scalar(380.0), fmin( Scalar(780.0), wienPeakNM ) );
	const Scalar planckMax = PlanckSpectralRadianceNM(
		planckPeakNM, maxTemperature );
	const Scalar binVolume = RepresentedBinVolume(
		m_bboxMin, m_bboxMax, m_emissionBinSize,
		x, y, z, m_volWidth, m_volHeight, m_volDepth );
	const Scalar carbonUpper = StablePositiveProduct( {
		binVolume, Scalar(400.0), m_sceneUnitMeters,
		maxCarbon, carbonAbsorptionMax, planckMax } );
	const Scalar condensedUpper = StablePositiveProduct( {
		binVolume, Scalar(400.0), m_sceneUnitMeters,
		maxCondensed, condAbsorptionMax, planckMax } );
	if( carbonUpper < 0.0 || condensedUpper < 0.0 ) return -1.0;
	return carbonUpper + condensedUpper;
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
				const Scalar binVolume = RepresentedBinVolume(
					m_bboxMin, m_bboxMax, m_emissionBinSize, x, y, z,
					m_volWidth, m_volHeight, m_volDepth );
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
				const Scalar proposal = proposalAverage > 0.0 ?
					StablePositiveProduct( {binVolume,proposalAverage} ) : 0.0;
				const Scalar upper = EmissionBinUpperBound( x, y, z );
				const Scalar weight = (Scalar(1.0) - supportMix) * proposal +
					supportMix * upper;
				if( !RISE::IsFiniteDouble( proposal ) || proposal < 0.0 ||
					!RISE::IsFiniteDouble( upper ) || upper < 0.0 ||
					!RISE::IsFiniteDouble( weight ) || weight < 0.0 ||
					((proposal > 0.0 || upper > 0.0) && RISE::IsZeroDouble(weight)) ) return false;

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
	return accessor.GetValue(
		nx*Scalar(m_volWidth) - InterpolationAccessorOffset(m_volWidth),
		ny*Scalar(m_volHeight) - InterpolationAccessorOffset(m_volHeight),
		nz*Scalar(m_volDepth) - InterpolationAccessorOffset(m_volDepth) );
}

Scalar MultichannelHeterogeneousMedium::InterpolationAccessorOffset(
	const unsigned int dimension
	) const
{
	return Scalar(dimension/2u) + 0.5;
}

void MultichannelHeterogeneousMedium::AppendOpticalDepthBreakpoints(
	const Ray& ray,
	const Scalar tBegin,
	const Scalar tEnd,
	std::vector<Scalar>& breakpoints
	) const
{
	Scalar temperature[4];
	for( unsigned int sample = 0; sample < 4u; ++sample ) {
		const Scalar u = Scalar(sample)/3.0;
		const Scalar t = tBegin + u*(tEnd-tBegin);
		const Point3 point = Point3Ops::mkPoint3( ray.origin, ray.Dir()*t );
		temperature[sample] = LookupTemperature(point);
	}
	AppendCubicLevelCrossings(
		temperature, m_optics.HotFractionMinK(), tBegin, tEnd, breakpoints );
	AppendCubicLevelCrossings(
		temperature, m_optics.HotFractionMaxK(), tBegin, tEnd, breakpoints );
	std::sort( breakpoints.begin(), breakpoints.end() );
	breakpoints.erase( std::unique(breakpoints.begin(),breakpoints.end()),
		breakpoints.end() );
}

Scalar MultichannelHeterogeneousMedium::LookupCarbon( const Point3& worldPt ) const
{
	return m_pCarbonAccessor ? LookupChannel( *m_pCarbonAccessor, worldPt ) : 0.0;
}

Scalar MultichannelHeterogeneousMedium::LookupTemperature( const Point3& worldPt ) const
{
	return m_pTemperatureAccessor ? LookupChannel( *m_pTemperatureAccessor, worldPt ) : 0.0;
}

Scalar MultichannelHeterogeneousMedium::LookupCondensed( const Point3& worldPt ) const
{
	return m_pCondensedAccessor ? LookupChannel( *m_pCondensedAccessor, worldPt ) : 0.0;
}

Scalar MultichannelHeterogeneousMedium::HotOpticsFraction( const Point3& worldPt ) const
{
	return m_optics.HotFraction( LookupTemperature(worldPt) );
}

Scalar MultichannelHeterogeneousMedium::HotSootVolumeFraction( const Point3& worldPt ) const
{
	return HotOpticsFraction( worldPt ) * 1.0e-3 * LookupCarbon( worldPt ) /
		m_optics.SootDensityKgM3();
}

const char* MultichannelHeterogeneousMedium::GetFireRenderFidelityStatus(
	const bool predictiveRequested
	) const
{
	return (predictiveRequested ? m_predictiveFidelity : m_previewFidelity).
		renderFidelityStatus.c_str();
}

unsigned int MultichannelHeterogeneousMedium::GetFireRenderReasonCodeCount(
	const bool predictiveRequested
	) const
{
	return static_cast<unsigned int>((predictiveRequested ?
		m_predictiveFidelity : m_previewFidelity).reasonCodes.size());
}

const char* MultichannelHeterogeneousMedium::GetFireRenderReasonCode(
	const bool predictiveRequested,
	const unsigned int index
	) const
{
	const std::vector<std::string>& reasons = (predictiveRequested ?
		m_predictiveFidelity : m_previewFidelity).reasonCodes;
	return index < reasons.size() ? reasons[index].c_str() : 0;
}

Scalar MultichannelHeterogeneousMedium::TrackingMajorantAt( const Point3& worldPt ) const
{
	if( !m_pMajorantGrid ) return 0.0;
	unsigned int x = 0, y = 0, z = 0;
	if( !m_pMajorantGrid->WorldToCell( worldPt, x, y, z ) ) return 0.0;
	return m_pMajorantGrid->GetCellMajorant( x, y, z );
}

Scalar MultichannelHeterogeneousMedium::TrackingMajorantAtPel(
	const Point3& worldPt
	) const
{
	const Scalar majorant633 = TrackingMajorantAt(worldPt);
	return m_sigma_t_majorant > 0.0 ?
		majorant633*PelTrackingMajorant()/m_sigma_t_majorant : 0.0;
}

Scalar MultichannelHeterogeneousMedium::SpectralTrackingMajorant(
	const Scalar
	) const
{
	return m_sigma_t_majorant;
}

Scalar MultichannelHeterogeneousMedium::PelTrackingMajorant() const
{
	Scalar projectedMassMaximum = 0.0;
	for( unsigned int channel = 0; channel < 3u; ++channel ) {
		projectedMassMaximum = fmax( projectedMassMaximum,
			m_hotExtinctionMass633*m_pelHotMean[channel] );
		projectedMassMaximum = fmax( projectedMassMaximum,
			m_coolExtinctionMass633*m_pelCoolMean[channel] );
		projectedMassMaximum = fmax( projectedMassMaximum,
			m_condExtinctionMass633*m_pelCondMean[channel] );
	}
	return m_sceneUnitMeters*projectedMassMaximum;
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
	const Scalar hotAbsorption633 = carbon * phi * m_hotAbsorptionMass633;
	const Scalar hotScattering633 = hotAbsorption633 *
		m_sootAlbedoHot / (1.0 - m_sootAlbedoHot);
	const Scalar coolExtinction633 = carbon * (1.0 - phi) *
		m_coolExtinctionMass633;
	const Scalar coolScattering633 = coolExtinction633 * m_smokeAlbedoCarbon;
	const Scalar condExtinction633 = LookupCondensed( pt ) * m_condExtinctionMass633;
	const Scalar condScattering633 = condExtinction633 * m_smokeAlbedoCond;

	const RISEPel sigmaA = m_sceneUnitMeters *
		(m_pelHotMean*hotAbsorption633 +
		 m_pelCoolMean*(coolExtinction633-coolScattering633) +
		 m_pelCondMean*(condExtinction633-condScattering633));
	const RISEPel sigmaS = m_sceneUnitMeters *
		(m_pelHotMean*hotScattering633 +
		 m_pelCoolMean*coolScattering633 +
		 m_pelCondMean*condScattering633);
	for( unsigned int channel = 0; channel < 3u; ++channel ) {
		assert( sigmaA[channel] >= 0.0 && sigmaS[channel] >= 0.0 );
	}

	MediumCoefficients c;
	c.sigma_t = sigmaA + sigmaS;
	c.sigma_s = sigmaS;
	// Thermal emission has its own collision-estimator source API, matching
	// GetThermalEmissionNM.  MediumCoefficients::emission remains the distinct
	// absorption-independent additive source contract.
	c.emission = RISEPel( 0.0 );
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
	if( !m_valid || !RISE::IsFiniteDouble( nm ) ||
		nm < m_optics.DomainMinNM() || nm > m_optics.DomainMaxNM() ) return c;

	const Scalar carbon = LookupCarbon( pt );
	const Scalar phi = HotOpticsFraction( pt );
	const Scalar hotAbsorption = carbon * phi * AblatedHotAbsorptionMass(nm);
	const Scalar hotExtinction = hotAbsorption/(1.0-m_optics.HotAlbedo(nm));
	const Scalar hotScattering = hotExtinction-hotAbsorption;
	const Scalar coolExtinction = carbon * (1.0 - phi) *
		m_optics.CoolExtinctionMass(nm);
	const Scalar coolScattering = coolExtinction * m_optics.CoolAlbedo(nm);
	const Scalar condExtinction = LookupCondensed( pt ) *
		m_optics.CondensedExtinctionMass(nm);
	const Scalar condScattering = condExtinction * m_optics.CondensedAlbedo(nm);
	const Scalar sigmaA = m_sceneUnitMeters *
		(hotAbsorption + coolExtinction - coolScattering +
		 condExtinction - condScattering);
	const Scalar sigmaS = m_sceneUnitMeters *
		(hotScattering + coolScattering + condScattering);
	c.sigma_t = sigmaA + sigmaS;
	c.sigma_s = sigmaS;
	return c;
}

const IPhaseFunction* MultichannelHeterogeneousMedium::MakePhaseClosure(
	const Point3& pt,
	const Scalar nm
	) const
{
	if( !m_valid || !RISE::IsFiniteDouble( nm ) ||
		nm < m_optics.DomainMinNM() || nm > m_optics.DomainMaxNM() ) return 0;

	const Scalar carbon = LookupCarbon( pt );
	const Scalar phi = HotOpticsFraction( pt );
	const Scalar hotExtinction = carbon * phi * AblatedHotAbsorptionMass(nm)/
		(1.0-m_optics.HotAlbedo(nm));
	const Scalar hotScattering = m_sceneUnitMeters * hotExtinction *
		m_optics.HotAlbedo(nm);
	const Scalar coolExtinction = carbon * (1.0 - phi) *
		m_optics.CoolExtinctionMass(nm);
	const Scalar coolScattering = m_sceneUnitMeters * coolExtinction *
		m_optics.CoolAlbedo(nm);
	const Scalar condExtinction = LookupCondensed( pt ) *
		m_optics.CondensedExtinctionMass(nm);
	const Scalar condScattering = m_sceneUnitMeters * condExtinction *
		m_optics.CondensedAlbedo(nm);
	const Scalar totalScattering = hotScattering + coolScattering + condScattering;
	if( !RISE::IsFiniteDouble( totalScattering ) || totalScattering <= 0.0 ) return 0;

	return new ConstituentHGPhaseClosure(
		hotScattering, coolScattering, condScattering,
		m_optics.HotG(nm), m_optics.CoolG(nm), m_optics.CondensedG(nm) );
}

const IPhaseFunction* MultichannelHeterogeneousMedium::MakePhaseClosurePel(
	const Point3& pt
	) const
{
	if( !m_valid ) return 0;

	const Scalar carbon = LookupCarbon( pt );
	const Scalar phi = HotOpticsFraction( pt );
	const Scalar hotScattering633 = carbon*phi*m_hotAbsorptionMass633 *
		m_sootAlbedoHot/(1.0-m_sootAlbedoHot);
	const Scalar coolScattering633 = carbon*(1.0-phi)*
		m_coolExtinctionMass633*m_smokeAlbedoCarbon;
	const Scalar condScattering633 = LookupCondensed(pt)*
		m_condExtinctionMass633*m_smokeAlbedoCond;
	const RISEPel hotScattering = m_pelHotMean*hotScattering633;
	const RISEPel coolScattering = m_pelCoolMean*coolScattering633;
	const RISEPel condScattering = m_pelCondMean*condScattering633;
	const Scalar hotProposal = hotScattering633*m_samplingHotMass;
	const Scalar coolProposal = coolScattering633*m_samplingCoolMass;
	const Scalar condProposal = condScattering633*m_samplingCondMass;
	const Scalar totalProposal = hotProposal+coolProposal+condProposal;
	if( !RISE::IsFiniteDouble(totalProposal) || totalProposal <= 0.0 ) return 0;
	return new ConstituentHGPelPhaseClosure(
		hotScattering,coolScattering,condScattering,
		hotProposal,coolProposal,condProposal,
		m_sootGHot,m_smokeGCarbon,m_smokeGCond );
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

RISEPel MultichannelHeterogeneousMedium::GetThermalEmissionPel(
	const Point3& pt
	) const
{
	if( !m_valid ) return RISEPel( 0.0 );
	RISEPel emission( 0.0 );
	for( unsigned int channel = 0; channel < 3u; ++channel ) {
		emission[channel] = GaussLegendre21::IntegrateVisibleBand(
			[this,&pt,channel]( const Scalar nm ) {
				const MediumCoefficientsNM coeff = GetCoefficientsNM(pt,nm);
				const Scalar sigmaA = coeff.sigma_t-coeff.sigma_s;
				return sigmaA > 0.0 ? FirePelResponse(nm)[channel]*sigmaA*
					PlanckSpectralRadianceNM(nm,LookupTemperature(pt)) : 0.0;
			} );
	}
	return emission;
}

Scalar MultichannelHeterogeneousMedium::GetChemEmissionNM(
	const Point3& pt,
	const Scalar nm
	) const
{
	if( !m_valid || !m_pChemAccessor[0] ) return 0.0;
	Scalar sourceSI = 0.0;
	for( unsigned int band = 0; band < 3u; ++band ) {
		sourceSI += LookupChemBand(band,pt)*NormalizedChemSPD(band,nm);
	}
	return sourceSI > 0.0
		? m_sceneUnitMeters*sourceSI/FOUR_PI : 0.0;
}

Scalar MultichannelHeterogeneousMedium::EstimateChemEmissionSegmentNM(
	const Ray& ray,
	const Scalar segmentStart,
	const Scalar segmentEnd,
	const Scalar nm,
	ISampler& sampler
	) const
{
	if( !m_valid || !m_pChemAccessor[0] ||
		!RISE::IsFiniteDouble(segmentStart) ||
		!RISE::IsFiniteDouble(segmentEnd) || segmentEnd <= segmentStart ) {
		return 0.0;
	}

	std::vector<Scalar> boundaries;
	boundaries.reserve(16);
	boundaries.push_back(segmentStart);
	AppendChemPanelBreakpoints(ray,segmentStart,segmentEnd,boundaries);
	boundaries.push_back(segmentEnd);
	std::sort(boundaries.begin(),boundaries.end());
	boundaries.erase(std::unique(boundaries.begin(),boundaries.end()),boundaries.end());
	if( boundaries.size() < 2u ) return 0.0;

	std::vector<Scalar> support;
	support.reserve(boundaries.size()-1u);
	Scalar totalWeight = 0.0;
	for( size_t panel = 0; panel+1u < boundaries.size(); ++panel ) {
		const Scalar length = boundaries[panel+1u]-boundaries[panel];
		const Scalar bound = length > 0.0
			? ChemPanelSupport(ray,boundaries[panel],boundaries[panel+1u]) : 0.0;
		const Scalar weight = bound > 0.0
			? StablePositiveProduct({length,bound}) : 0.0;
		if( weight < 0.0 ) return 0.0;
		support.push_back(weight);
		totalWeight += weight;
		if( !RISE::IsFiniteDouble(totalWeight) ) return 0.0;
	}

	const Scalar segmentLength = segmentEnd-segmentStart;
	Scalar sampleT = 0.0;
	if( totalWeight > 0.0 && sampler.Get1D() >= 0.5 ) {
		const Scalar target = sampler.Get1D()*totalWeight;
		Scalar cumulative = 0.0;
		size_t selected = support.size()-1u;
		for( size_t panel = 0; panel < support.size(); ++panel ) {
			cumulative += support[panel];
			if( target < cumulative ) {
				selected = panel;
				break;
			}
		}
		sampleT = boundaries[selected] + sampler.Get1D()*
			(boundaries[selected+1u]-boundaries[selected]);
	} else {
		sampleT = segmentStart+sampler.Get1D()*segmentLength;
	}

	Scalar reactionDensity = 0.0;
	if( totalWeight > 0.0 ) {
		for( size_t panel = 0; panel < support.size(); ++panel ) {
			const bool contains = sampleT >= boundaries[panel] &&
				(panel+1u == support.size()
					? sampleT <= boundaries[panel+1u]
					: sampleT < boundaries[panel+1u]);
			if( contains ) {
				const Scalar length = boundaries[panel+1u]-boundaries[panel];
				reactionDensity = length > 0.0
					? (support[panel]/totalWeight)/length : 0.0;
				break;
			}
		}
	}
	const Scalar proposalDensity = totalWeight > 0.0
		? 0.5/segmentLength+0.5*reactionDensity : 1.0/segmentLength;
	if( !RISE::IsFiniteDouble(proposalDensity) || proposalDensity <= 0.0 ) {
		return 0.0;
	}
	const Scalar epsilon = GetChemEmissionNM(ray.PointAtLength(sampleT),nm);
	if( epsilon <= 0.0 ) return 0.0;
	const Scalar tau = EvalDeterministicOpticalDepthNM(ray,sampleT,nm);
	return exp(-tau)*epsilon/proposalDensity;
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
