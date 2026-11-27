//////////////////////////////////////////////////////////////////////
//
//  HyLIoSSkinMaterial.cpp - HyLIoS hyperspectral skin material
//    (P5 local baked response, supported camera proposal, shared
//    D65-weighted visible guide, and immutable asset cache).
//
//  Compiled with strict IEEE binary64 arithmetic to match
//  HyLIoSLocalBake / LoadLocalBake validation.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "HyLIoSSkinMaterial.h"
#include "HyLIoSLocalBake.h"
#include "HyLIoSResolvedExperiment.h"
#include "HyLIoSForwardExperiment.h"
#include "HyLIoSForwardTransport.h"
#include "HyLIoSOptics.h"
#include "HyLIoSReciprocalBoundary.h"
#include "HyLIoSSpatialGeometry.h"
#include "../../Agent/Json.h"
#include "../../Interfaces/ILog.h"
#include "../../Utilities/Color/ColorUtils.h"
#include "../../Utilities/Color/RGBSpectra.h"
#include "../../Utilities/GeometricUtilities.h"
#include "../../Utilities/ISampler.h"
#include "../../Utilities/RISECBOR64.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#if defined(__FAST_MATH__)
#error HyLIoSSkinMaterial must be compiled with strict floating point
#endif

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::HyLIoS;

namespace
{
	struct RightHandedSurfaceFrame
	{
		Vector3 T;
		Vector3 B;
		Vector3 N;
		Vector3 Ng;
		bool    valid = false;
	};

	//! Builds an unconditionally right-handed `(T, B, N)` shading basis
	//! (`T x B == +N`) with `N` and `Ng` oriented to oppose the incoming
	//! ray (`Dot(rayDir, N) < 0` and `Dot(rayDir, Ng) < 0`).
	//! Immune to negative-determinant mirrored transforms that flip `ri.onb.v()`.
	RightHandedSurfaceFrame BuildRightHandedFrame( const RayIntersectionGeometric& ri )
	{
		RightHandedSurfaceFrame f;
		const Vector3 rayDir = Vector3Ops::Normalize( ri.ray.Dir() );
		Vector3 N = ri.vNormal;
		if( Vector3Ops::SquaredModulus( N ) <= Scalar( 1e-24 ) ) {
			N = ri.onb.w();
		}
		const Scalar nLen = Vector3Ops::Magnitude( N );
		if( !( nLen > Scalar( 1e-12 ) ) ) {
			return f;
		}
		N = N * ( Scalar( 1 ) / nLen );
		if( Vector3Ops::Dot( rayDir, N ) > Scalar( 0 ) ) {
			N = -N;
		}

		Vector3 Ng = ri.vGeomNormal;
		const Scalar ngLen = Vector3Ops::Magnitude( Ng );
		if( ngLen > Scalar( 1e-12 ) ) {
			Ng = Ng * ( Scalar( 1 ) / ngLen );
		} else {
			Ng = N;
		}
		if( Vector3Ops::Dot( rayDir, Ng ) > Scalar( 0 ) ) {
			Ng = -Ng;
		}

		Vector3 T = ri.onb.u() - N * Vector3Ops::Dot( ri.onb.u(), N );
		Scalar tLen = Vector3Ops::Magnitude( T );
		if( !( tLen > Scalar( 1e-8 ) ) ) {
			const Vector3 axis = ( std::fabs( N.x ) < Scalar( 0.9 ) )
				? Vector3( 1, 0, 0 )
				: Vector3( 0, 1, 0 );
			T = axis - N * Vector3Ops::Dot( axis, N );
			tLen = Vector3Ops::Magnitude( T );
			if( !( tLen > Scalar( 1e-12 ) ) ) {
				return f;
			}
		}
		T = T * ( Scalar( 1 ) / tLen );
		Vector3 B = Vector3Ops::Cross( N, T );
		const Scalar bLen = Vector3Ops::Magnitude( B );
		if( !( bLen > Scalar( 1e-12 ) ) ) {
			return f;
		}
		B = B * ( Scalar( 1 ) / bLen );

		f.T = T;
		f.B = B;
		f.N = N;
		f.Ng = Ng;
		f.valid = true;
		return f;
	}

	inline bool WorldToLocalPoint(
		const RightHandedSurfaceFrame& frame,
		const Vector3& worldDir,
		LocalResponsePoint& outPoint
		)
	{
		if( !frame.valid ) {
			return false;
		}
		const Scalar len = Vector3Ops::Magnitude( worldDir );
		if( !( len > Scalar( 1e-12 ) ) ) {
			return false;
		}
		const Vector3 d = worldDir * ( Scalar( 1 ) / len );
		if( Vector3Ops::Dot( d, frame.Ng ) <= Scalar( 0 ) ) {
			return false;
		}
		const double z = static_cast<double>( Vector3Ops::Dot( d, frame.N ) );
		if( !( z > 0.0 ) || !std::isfinite( z ) ) {
			return false;
		}
		const double x = static_cast<double>( Vector3Ops::Dot( d, frame.T ) );
		const double y = static_cast<double>( Vector3Ops::Dot( d, frame.B ) );
		if( !std::isfinite( x ) || !std::isfinite( y ) ) {
			return false;
		}
		outPoint = HyLIoSLocalResponse::FromDirection( LocalResponseDirection{ x, y, z } );
		return outPoint.t > 0.0 && outPoint.t <= 1.0 && std::isfinite( outPoint.phi );
	}

	inline double EvalPiecewiseLinear(
		const std::vector<double>& xs,
		const std::vector<double>& ys,
		const double x
		)
	{
		if( xs.size() == 1 ) {
			return ys[0];
		}
		const auto it = std::lower_bound( xs.begin(), xs.end(), x );
		if( it == xs.begin() ) {
			return ys.front();
		}
		if( it == xs.end() ) {
			return ys.back();
		}
		const std::size_t hi = static_cast<std::size_t>( it - xs.begin() );
		if( xs[hi] == x ) {
			return ys[hi];
		}
		const std::size_t lo = hi - 1;
		const double t = ( x - xs[lo] ) / ( xs[hi] - xs[lo] );
		return ys[lo] * ( 1.0 - t ) + ys[hi] * t;
	}

	struct CachedAssetBundle
	{
		std::shared_ptr<const HyLIoSLocalResponseAsset> asset;
		std::shared_ptr<const HyLIoSSupportedCameraProposal> proposal;
		RISEPel guideRGB;
		bool hasVisibleGuide = false;
		std::uint64_t residentBytesBound = 0;
	};

	struct AssetCacheState
	{
		std::mutex mutex;
		std::unordered_map<std::string, std::weak_ptr<const CachedAssetBundle>> entries;
	};

	AssetCacheState& GlobalAssetCache()
	{
		static AssetCacheState state;
		return state;
	}

	std::string HashSemanticIdentity( const LocalAssetSemanticIdentity& id )
	{
		std::vector<unsigned char> buf;
		auto append = [&]( const LocalAssetRecord& r ) {
			const std::uint64_t n = static_cast<std::uint64_t>( r.size() );
			for( unsigned k = 0; k < 8; ++k ) {
				buf.push_back( static_cast<unsigned char>( n >> ( 8u * k ) ) );
			}
			buf.insert( buf.end(), r.begin(), r.end() );
		};
		append( id.model );
		append( id.placement );
		append( id.postscatter );
		append( id.boundary );
		append( id.lineage );
		append( id.data );
		append( id.specimen );
		append( id.exterior );
		append( id.approximation );
		return RISECBOR64::SHA256Hex( buf );
	}

	std::vector<double> ExtractSpecimenWavelengths( const std::string& sourceBytes )
	{
		RISE::Agent::JsonValue root;
		std::string err;
		if( !RISE::Agent::JsonParse( sourceBytes, root, err ) || !root.isObject() ) {
			throw std::runtime_error( "HyLIoSSkinMaterial: invalid specimen JSON: " + err );
		}
		const auto& grid = root.get( "wavelength_nm" );
		if( !grid.isArray() || grid.size() == 0 ) {
			throw std::runtime_error( "HyLIoSSkinMaterial: specimen JSON missing non-empty wavelength_nm array" );
		}
		std::vector<double> waves;
		waves.reserve( grid.size() );
		for( std::size_t i = 0; i < grid.size(); ++i ) {
			if( !grid.at( i ).isNumber() ) {
				throw std::runtime_error( "HyLIoSSkinMaterial: specimen wavelength_nm contains non-number" );
			}
			const double w = grid.at( i ).asNumber();
			if( !std::isfinite( w ) || !( w > 0.0 ) ) {
				throw std::runtime_error( "HyLIoSSkinMaterial: specimen wavelength_nm contains non-positive or non-finite value" );
			}
			if( !waves.empty() && !( w > waves.back() ) ) {
				throw std::runtime_error( "HyLIoSSkinMaterial: specimen wavelength_nm must be strictly increasing" );
			}
			waves.push_back( w );
		}
		return waves;
	}

	bool FailCreate( std::string* outError, const std::string& msg )
	{
		GlobalLog()->PrintEx( eLog_Error, "HyLIoSSkinMaterial:: %s", msg.c_str() );
		if( outError ) {
			*outError = msg;
		}
		return false;
	}
} // namespace

bool RISE::Implementation::ComputeHyLIoSVisibleGuideRGB(
	const std::vector<double>& wavelengthsNm,
	const std::vector<double>& hemisphericalReflectance,
	RISEPel& outGuideRGB
	)
{
	outGuideRGB = RISEPel( 0, 0, 0 );
	if( wavelengthsNm.empty() || wavelengthsNm.size() != hemisphericalReflectance.size() ) {
		return false;
	}
	for( std::size_t i = 0; i < wavelengthsNm.size(); ++i ) {
		if( !std::isfinite( wavelengthsNm[i] ) || !( wavelengthsNm[i] > 0.0 ) ||
			!std::isfinite( hemisphericalReflectance[i] ) || hemisphericalReflectance[i] < 0.0 ) {
			return false;
		}
		if( i > 0 && !( wavelengthsNm[i] > wavelengthsNm[i - 1] ) ) {
			return false;
		}
	}

	const double visLo = 380.0;
	const double visHi = 780.0;
	const double lo = std::max( visLo, wavelengthsNm.front() );
	const double hi = std::min( visHi, wavelengthsNm.back() );
	if( lo > hi ) {
		return false;
	}

	XYZPel numXYZ( 0, 0, 0 );
	double denY = 0.0;

	if( lo == hi ) {
		XYZPel obs( 0, 0, 0 );
		if( !ColorUtils::XYZFromNM( obs, Scalar( lo ) ) || !( double( obs.Y ) > 0.0 ) ) {
			return false;
		}
		const double r = std::clamp( EvalPiecewiseLinear( wavelengthsNm, hemisphericalReflectance, lo ), 0.0, 1.0 );
		const double d65 = static_cast<double>( RGBIlluminantSpectrum::ReferenceIlluminant( Scalar( lo ) ) );
		if( !( d65 > 0.0 ) ) {
			return false;
		}
		numXYZ = obs * Scalar( r * d65 );
		denY = double( obs.Y ) * d65;
	} else {
		// Deterministic 1 nm composite trapezoidal quadrature over [lo, hi].
		const unsigned int steps = std::max<unsigned int>( 1u, static_cast<unsigned int>( std::ceil( hi - lo ) ) );
		const double h = ( hi - lo ) / static_cast<double>( steps );
		for( unsigned int i = 0; i <= steps; ++i ) {
			const double nm = ( i == steps ) ? hi : ( lo + h * static_cast<double>( i ) );
			const double w = ( i == 0 || i == steps ) ? ( 0.5 * h ) : h;
			XYZPel obs( 0, 0, 0 );
			if( !ColorUtils::XYZFromNM( obs, Scalar( nm ) ) ) {
				continue;
			}
			const double d65 = static_cast<double>( RGBIlluminantSpectrum::ReferenceIlluminant( Scalar( nm ) ) );
			if( !( d65 > 0.0 ) ) {
				continue;
			}
			const double r = std::clamp( EvalPiecewiseLinear( wavelengthsNm, hemisphericalReflectance, nm ), 0.0, 1.0 );
			numXYZ = numXYZ + obs * Scalar( r * d65 * w );
			denY += double( obs.Y ) * d65 * w;
		}
	}

	if( !( denY > 0.0 ) || !std::isfinite( denY ) ) {
		return false;
	}

	const XYZPel normXYZ = numXYZ * Scalar( 1.0 / denY );
	RISEPel rgb( normXYZ );
	rgb.r = std::clamp( rgb.r, Scalar( 0 ), Scalar( 1 ) );
	rgb.g = std::clamp( rgb.g, Scalar( 0 ), Scalar( 1 ) );
	rgb.b = std::clamp( rgb.b, Scalar( 0 ), Scalar( 1 ) );
	outGuideRGB = rgb;
	return true;
}

//////////////////////////////////////////////////////////////////////
// HyLIoSLocalBSDF
//////////////////////////////////////////////////////////////////////

HyLIoSLocalBSDF::HyLIoSLocalBSDF(
	std::shared_ptr<const HyLIoSLocalResponse> response,
	const RISEPel& guideRGB_,
	const bool hasVisibleGuide_
	) :
  pResponse( std::move( response ) ),
  guideRGB( guideRGB_ ),
  hasVisibleGuide( hasVisibleGuide_ )
{
}

HyLIoSLocalBSDF::~HyLIoSLocalBSDF()
{
}

RISEPel HyLIoSLocalBSDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	if( !hasVisibleGuide ) {
		return RISEPel( 0, 0, 0 );
	}
	const RightHandedSurfaceFrame frame = BuildRightHandedFrame( ri );
	if( !frame.valid ) {
		return RISEPel( 0, 0, 0 );
	}
	const Vector3 wi = Vector3Ops::Normalize( vLightIn );
	const Vector3 wo = Vector3Ops::Normalize( -ri.ray.Dir() );
	if( Vector3Ops::Dot( wi, frame.N ) <= Scalar( 0 ) ||
		Vector3Ops::Dot( wi, frame.Ng ) <= Scalar( 0 ) ||
		Vector3Ops::Dot( wo, frame.N ) <= Scalar( 0 ) ||
		Vector3Ops::Dot( wo, frame.Ng ) <= Scalar( 0 ) ) {
		return RISEPel( 0, 0, 0 );
	}
	return guideRGB * INV_PI;
}

Scalar HyLIoSLocalBSDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	const double wavelength = static_cast<double>( nm );
	if( !pResponse || !std::isfinite( wavelength ) || !pResponse->Covers( wavelength, wavelength ) ) {
		return Scalar( 0 );
	}
	const RightHandedSurfaceFrame frame = BuildRightHandedFrame( ri );
	LocalResponsePoint wi{}, wo{};
	if( !WorldToLocalPoint( frame, vLightIn, wi ) ||
		!WorldToLocalPoint( frame, -ri.ray.Dir(), wo ) ) {
		return Scalar( 0 );
	}
	return Scalar( pResponse->EvaluateContinuousNM( wi, wo, wavelength ) );
}

RISEPel HyLIoSLocalBSDF::albedo( const RayIntersectionGeometric& ) const
{
	return hasVisibleGuide ? guideRGB : RISEPel( 0, 0, 0 );
}

//////////////////////////////////////////////////////////////////////
// HyLIoSLocalSPF
//////////////////////////////////////////////////////////////////////

HyLIoSLocalSPF::HyLIoSLocalSPF(
	std::shared_ptr<const HyLIoSSupportedCameraProposal> proposal,
	const RISEPel& guideRGB_,
	const bool hasVisibleGuide_
	) :
  pProposal( std::move( proposal ) ),
  guideRGB( guideRGB_ ),
  hasVisibleGuide( hasVisibleGuide_ )
{
}

HyLIoSLocalSPF::~HyLIoSLocalSPF()
{
}

void HyLIoSLocalSPF::Scatter(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	ScatteredRayContainer& scattered,
	const IORStack&
	) const
{
	if( !hasVisibleGuide ) {
		return;
	}
	const RightHandedSurfaceFrame frame = BuildRightHandedFrame( ri );
	if( !frame.valid ) {
		return;
	}
	const Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
	OrthonormalBasis3D onb;
	onb.CreateFromW( frame.N );
	const Vector3 dir = GeometricUtilities::CreateDiffuseVector( onb, ptrand );
	if( Vector3Ops::Dot( dir, frame.Ng ) <= Scalar( 0 ) ) {
		return;
	}
	const Scalar cosTheta = Vector3Ops::Dot( dir, frame.N );
	if( !( cosTheta > Scalar( 0 ) ) ) {
		return;
	}

	ScatteredRay sr;
	sr.type = ScatteredRay::eRayDiffuse;
	sr.kray = guideRGB;
	sr.ray.Set( ri.ptIntersection, dir );
	sr.pdf = cosTheta * INV_PI;
	sr.isDelta = false;
	scattered.AddScatteredRay( sr );
}

void HyLIoSLocalSPF::ScatterNM(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const Scalar nm,
	ScatteredRayContainer& scattered,
	const IORStack&
	) const
{
	const double wavelength = static_cast<double>( nm );
	if( !pProposal || !std::isfinite( wavelength ) || !pProposal->Response().Covers( wavelength, wavelength ) ) {
		return;
	}
	const RightHandedSurfaceFrame frame = BuildRightHandedFrame( ri );
	LocalResponsePoint wo{};
	if( !WorldToLocalPoint( frame, -ri.ray.Dir(), wo ) ) {
		return;
	}

	const std::size_t nCells = pProposal->Response().Storage().incidentCells;
	if( nCells == 0 ) {
		return;
	}

	auto clampUnit = []( const Scalar x ) -> double {
		const double d = static_cast<double>( x );
		if( !std::isfinite( d ) || d <= 0.0 ) return 0.0;
		if( d >= 1.0 ) return std::nextafter( 1.0, 0.0 );
		return d;
	};

	const double uEvent = clampUnit( sampler.Get1D() );
	const double uCell  = clampUnit( sampler.Get1D() );
	const double uT     = clampUnit( sampler.Get1D() );
	const double uPhi   = clampUnit( sampler.Get1D() );

	const unsigned event = ( uEvent < 0.5 ) ? 0u : 1u;
	const std::size_t cell = std::min<std::size_t>(
		static_cast<std::size_t>( std::floor( uCell * static_cast<double>( nCells ) ) ),
		nCells - 1 );
	const unsigned bits = pProposal->PrecisionBits();
	const double scale = std::ldexp( 1.0, static_cast<int>( bits ) );
	const std::uint64_t maxInt = ( std::uint64_t{1} << bits ) - 1ULL;
	const std::uint64_t ui = std::min<std::uint64_t>(
		static_cast<std::uint64_t>( std::floor( uT * scale ) ), maxInt );
	const std::uint64_t vi = std::min<std::uint64_t>(
		static_cast<std::uint64_t>( std::floor( uPhi * scale ) ), maxInt );

	const LocalResponseSample sample = pProposal->SampleCameraNM( wo, wavelength, event, cell, ui, vi );
	if( sample.event == LocalResponseSample::Event::None || !( sample.weight > 0.0 ) || !std::isfinite( sample.weight ) ) {
		return;
	}

	const LocalResponseDirection localWi = HyLIoSLocalResponse::ToDirection( sample.incident );
	const Vector3 wiWorld = Vector3Ops::Normalize(
		frame.T * Scalar( localWi.x ) +
		frame.B * Scalar( localWi.y ) +
		frame.N * Scalar( localWi.z ) );

	if( Vector3Ops::Dot( wiWorld, frame.Ng ) <= Scalar( 0 ) ||
		Vector3Ops::Dot( wiWorld, frame.N ) <= Scalar( 0 ) ) {
		return;
	}

	ScatteredRay sr;
	sr.ray.Set( ri.ptIntersection, wiWorld );
	sr.krayNM = Scalar( sample.weight );
	sr.pdf = Scalar( sample.pdf );
	sr.isDelta = ( sample.event == LocalResponseSample::Event::PlanarMirror );
	sr.type = sr.isDelta ? ScatteredRay::eRayReflection : ScatteredRay::eRayDiffuse;
	scattered.AddScatteredRay( sr );
}

Scalar HyLIoSLocalSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack&
	) const
{
	if( !hasVisibleGuide ) {
		return Scalar( 0 );
	}
	const RightHandedSurfaceFrame frame = BuildRightHandedFrame( ri );
	if( !frame.valid ) {
		return Scalar( 0 );
	}
	const Vector3 dir = Vector3Ops::Normalize( wo );
	if( Vector3Ops::Dot( dir, frame.Ng ) <= Scalar( 0 ) ) {
		return Scalar( 0 );
	}
	const Scalar cosTheta = Vector3Ops::Dot( dir, frame.N );
	return ( cosTheta > Scalar( 0 ) ) ? ( cosTheta * INV_PI ) : Scalar( 0 );
}

Scalar HyLIoSLocalSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& woIn,
	const Scalar nm,
	const IORStack&
	) const
{
	const double wavelength = static_cast<double>( nm );
	if( !pProposal || !std::isfinite( wavelength ) || !pProposal->Response().Covers( wavelength, wavelength ) ) {
		return Scalar( 0 );
	}
	const RightHandedSurfaceFrame frame = BuildRightHandedFrame( ri );
	LocalResponsePoint wi{}, wo{};
	if( !WorldToLocalPoint( frame, woIn, wi ) ||
		!WorldToLocalPoint( frame, -ri.ray.Dir(), wo ) ) {
		return Scalar( 0 );
	}
	return Scalar( pProposal->CameraPdfNM( wi, wo, wavelength ) );
}

//////////////////////////////////////////////////////////////////////
// P7 Nonlocal Spatial Runtime
//////////////////////////////////////////////////////////////////////

namespace RISE::Implementation
{
	struct HyLIoSNonlocalRuntime
	{
		std::vector<double> wavelengthsNm;
		std::vector<std::shared_ptr<const HyLIoSForwardTransport>> kernels;
		std::vector<double> scIORAtNode;
		double totalThicknessMetres = 0.0;
		double exteriorIOR = 1.0;
		double boundaryRoughness = 0.25;
		double physicalScale = 1.0;
		SpatialDomainKind domainKind = SpatialDomainKind::PlanarPolygon;
		double domainHalfWidthMetres = 0.05;
		double domainRadiusMetres = 0.10;
		std::uint64_t maxSubsurfaceSteps = 256;
		mutable std::atomic<std::uint64_t> domainBoundaryLosses{ 0 };

		bool InterpolateStratumCorneumIOR( const double nm, double& outEtaSC ) const noexcept
		{
			if( !std::isfinite( nm ) || wavelengthsNm.empty() || nm < wavelengthsNm.front() || nm > wavelengthsNm.back() ) {
				return false;
			}
			std::size_t lo = 0, hi = wavelengthsNm.size() - 1;
			if( wavelengthsNm.size() > 1 ) {
				auto it = std::lower_bound( wavelengthsNm.begin(), wavelengthsNm.end(), nm );
				if( it == wavelengthsNm.begin() ) {
					lo = hi = 0;
				} else if( it == wavelengthsNm.end() ) {
					lo = hi = wavelengthsNm.size() - 1;
				} else {
					hi = static_cast<std::size_t>( it - wavelengthsNm.begin() );
					lo = ( wavelengthsNm[hi] == nm ) ? hi : ( hi - 1 );
				}
			}
			const double t = ( hi > lo )
				? ( ( nm - wavelengthsNm[lo] ) / ( wavelengthsNm[hi] - wavelengthsNm[lo] ) )
				: 0.0;
			outEtaSC = ( 1.0 - t ) * scIORAtNode[lo] + t * scIORAtNode[hi];
			return std::isfinite( outEtaSC ) && outEtaSC > 0.0;
		}

		bool SelectKernelBracket(
			const double nm,
			std::size_t& outLo,
			std::size_t& outHi,
			double& outT
			) const noexcept
		{
			if( !std::isfinite( nm ) || wavelengthsNm.empty() || nm < wavelengthsNm.front() || nm > wavelengthsNm.back() ) {
				return false;
			}
			outLo = 0;
			outHi = wavelengthsNm.size() - 1;
			if( wavelengthsNm.size() > 1 ) {
				auto it = std::lower_bound( wavelengthsNm.begin(), wavelengthsNm.end(), nm );
				if( it == wavelengthsNm.begin() ) {
					outLo = outHi = 0;
				} else if( it == wavelengthsNm.end() ) {
					outLo = outHi = wavelengthsNm.size() - 1;
				} else {
					outHi = static_cast<std::size_t>( it - wavelengthsNm.begin() );
					outLo = ( wavelengthsNm[outHi] == nm ) ? outHi : ( outHi - 1 );
				}
			}
			outT = ( outHi > outLo )
				? ( ( nm - wavelengthsNm[outLo] ) / ( wavelengthsNm[outHi] - wavelengthsNm[outLo] ) )
				: 0.0;
			return true;
		}

		// Step-wise finite lateral side-wall check (workplan §7 item 1):
		// prevents any walk from leaving the finite footprint and re-entering unnoticed.
		bool IsInsideLateralBounds( const double xM, const double yM ) const noexcept
		{
			if( !std::isfinite( xM ) || !std::isfinite( yM ) ) {
				return false;
			}
			if( domainKind == SpatialDomainKind::PlanarPolygon ||
				domainKind == SpatialDomainKind::IntrinsicLayerVolume )
			{
				return std::fabs( xM ) <= domainHalfWidthMetres &&
					std::fabs( yM ) <= domainHalfWidthMetres;
			}
			if( domainKind == SpatialDomainKind::ConcentricCylinder ) {
				return std::fabs( yM ) <= domainHalfWidthMetres;
			}
			return true; // ConcentricSphere is closed laterally
		}
	};
}

namespace
{
	struct CachedNonlocalGuide
	{
		RISEPel guideRGB{ 0, 0, 0 };
		bool hasGuide = false;
	};

	struct NonlocalGuideCacheState
	{
		std::mutex mutex;
		std::unordered_map<std::string, CachedNonlocalGuide> entries;
	};

	NonlocalGuideCacheState& GlobalNonlocalGuideCache()
	{
		static NonlocalGuideCacheState state;
		return state;
	}

	std::shared_ptr<HyLIoSNonlocalRuntime> BuildNonlocalRuntime(
		const std::string& sourceBytes,
		const HyLIoSSkinMaterialConfig& config,
		const SpatialDomainKind domainKind,
		const ForwardPlacementPolicy placementPolicy,
		const ForwardPostScatterPolicy postScatterPolicy
		)
	{
		const std::vector<double> wls = ExtractSpecimenWavelengths( sourceBytes );
		if( wls.empty() ) {
			throw std::runtime_error( "nonlocal specimen_source has no wavelength_nm nodes" );
		}

		auto rt = std::make_shared<HyLIoSNonlocalRuntime>();
		rt->wavelengthsNm = wls;
		rt->kernels.reserve( wls.size() );
		rt->scIORAtNode.reserve( wls.size() );

		double totalThickness = 0.0;
		for( std::size_t k = 0; k < wls.size(); ++k ) {
			const double nm = wls[k];
			ResolveExperimentConfig rCfg;
			rCfg.wavelengthNm = nm;
			rCfg.boundary = ExperimentBoundary::Smooth;
			rCfg.explicitComplexPackV1 = config.explicitComplexPackV1;
			const ResolvedExperiment resolved = ResolveExperiment( sourceBytes, rCfg );

			if( k == 0 ) {
				for( const auto& L : resolved.Layers() ) {
					totalThickness += L.thicknessMetres;
				}
			}
			rt->scIORAtNode.push_back( resolved.Layers()[0].realIOR );

			const double fiberRate = RayleighRate(
				config.fiberRadiusMetres, nm * 1e-9,
				config.fiberFraction, config.fiberRelativeIOR );

			rt->kernels.emplace_back( std::make_shared<HyLIoSForwardTransport>(
				resolved.Layers(),
				nm,
				config.exteriorIOR,
				fiberRate,
				AbsorptionEstimator::Weighted,
				0.25,
				placementPolicy,
				postScatterPolicy ) );
		}

		rt->totalThicknessMetres  = totalThickness;
		rt->exteriorIOR           = config.exteriorIOR;
		rt->boundaryRoughness     = config.boundaryRoughness;
		rt->physicalScale         = config.physicalScale;
		rt->domainKind            = domainKind;
		rt->domainHalfWidthMetres = config.domainHalfWidthMetres;
		rt->domainRadiusMetres    = config.domainRadiusMetres;
		rt->maxSubsurfaceSteps    = std::max<std::uint64_t>( 16, config.maxSubsurfaceSteps );

		if( domainKind == SpatialDomainKind::ConcentricSphere ||
			domainKind == SpatialDomainKind::ConcentricCylinder )
		{
			if( !( rt->domainRadiusMetres > rt->totalThicknessMetres ) ) {
				throw std::invalid_argument( "nonlocal concentric domain radius must exceed total skin thickness" );
			}
		}
		return rt;
	}

	// Deterministic, cached preparation-time 1D Monte Carlo visible slab guide
	// driven by the actual P2 `HyLIoSForwardTransport` kernel (no response_asset needed).
	bool ComputeCachedNonlocalVisibleGuideRGB(
		const HyLIoSNonlocalRuntime& rt,
		const std::string& cacheKey,
		RISEPel& outGuideRGB
		)
	{
		{
			NonlocalGuideCacheState& cache = GlobalNonlocalGuideCache();
			std::lock_guard<std::mutex> lock( cache.mutex );
			auto it = cache.entries.find( cacheKey );
			if( it != cache.entries.end() ) {
				outGuideRGB = it->second.guideRGB;
				return it->second.hasGuide;
			}
		}

		static constexpr std::uint64_t kGuideSeed = 0x51AB601DE0000001ULL;
		static constexpr std::uint64_t kHistoriesPerNode = 64ULL;
		static constexpr std::uint64_t kMaxWorkPerHistory = 32768ULL;

		std::vector<double> refl( rt.wavelengthsNm.size(), 0.0 );
		for( std::size_t k = 0; k < rt.wavelengthsNm.size(); ++k ) {
			const double nm = rt.wavelengthsNm[k];
			if( nm < 360.0 || nm > 830.0 ) {
				continue;
			}
			const HyLIoSForwardTransport& kernel = *rt.kernels[k];
			const HyLIoSReciprocalBoundary boundary(
				rt.boundaryRoughness, rt.exteriorIOR, rt.scIORAtNode[k] );

			double sumReturned = 0.0;
			for( std::uint64_t h = 0; h < kHistoriesPerNode; ++h ) {
				ForwardExperimentRandom rng( kGuideSeed ^ ( static_cast<std::uint64_t>( k + 1 ) * 0x9e3779b97f4a7c15ULL ) ^ ( h * 0xbf58476d1ce4e5b9ULL ) );
				// Normal-incidence exterior ray: woLocal = (0, 0, +1)
				const ReciprocalBoundarySample entry = boundary.SampleFull(
					{ 0.0, 0.0, 1.0 }, rng.Get1D(), rng.Get1D(), rng.Get1D() );
				if( !entry.valid || !( entry.weight > 0.0 ) ) {
					continue;
				}
				if( entry.isReflection ) {
					sumReturned += entry.weight;
					continue;
				}
				// Power-domain entry weight into slab: undo radiance (etaExt/etaSC)^2 for power return
				const double etaRatioEntry = rt.scIORAtNode[k] / rt.exteriorIOR;
				const double powerEntryWeight = entry.weight * ( etaRatioEntry * etaRatioEntry );

				ForwardState state = kernel.Begin( { 0.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0 } );
				const SlabBoundaryEncounter initEnc = state.PendingBoundary();
				kernel.ResumeBoundary( state, initEnc.token, SlabBoundaryDecision{
					BoundaryAction::Transmit,
					SlabSide::Interior,
					{ entry.direction.x, -entry.direction.y, -entry.direction.z },
					powerEntryWeight,
					SlabBoundaryAngularLaw::Unclassified
				} );

				while( state.Termination() == ForwardTermination::None &&
					   state.Counters().workUnits < kMaxWorkPerHistory )
				{
					const ForwardAdvanceResult adv = kernel.Advance( state, rng, 32 );
					if( adv.kind == ForwardAdvanceKind::OuterBoundaryEncounter ) {
						const SlabBoundaryEncounter enc = state.PendingBoundary();
						const SlabVector woInt{ -enc.incidentDirection.x, enc.incidentDirection.y, enc.incidentDirection.z };
						const ReciprocalBoundarySample exitStep = boundary.SampleFull(
							woInt, rng.Get1D(), rng.Get1D(), rng.Get1D() );
						if( !exitStep.valid || !( exitStep.weight > 0.0 ) ) {
							kernel.StopForBudget( state );
							break;
						}
						if( exitStep.isReflection ) {
							kernel.ResumeBoundary( state, enc.token, SlabBoundaryDecision{
								BoundaryAction::Reflect,
								SlabSide::Interior,
								{ exitStep.direction.x, -exitStep.direction.y, -exitStep.direction.z },
								exitStep.weight,
								SlabBoundaryAngularLaw::Unclassified
							} );
						} else {
							const double etaRatioExit = rt.exteriorIOR / rt.scIORAtNode[k];
							const double powerExitWeight = exitStep.weight * ( etaRatioExit * etaRatioExit );
							sumReturned += std::min( 1.0, enc.weight * powerExitWeight );
							kernel.StopForBudget( state );
							break;
						}
					} else if( adv.kind == ForwardAdvanceKind::Terminal ) {
						break;
					}
				}
			}
			refl[k] = std::clamp( sumReturned / static_cast<double>( kHistoriesPerNode ), 0.0, 1.0 );
		}

		const bool hasGuide = ComputeHyLIoSVisibleGuideRGB( rt.wavelengthsNm, refl, outGuideRGB );
		{
			NonlocalGuideCacheState& cache = GlobalNonlocalGuideCache();
			std::lock_guard<std::mutex> lock( cache.mutex );
			cache.entries[cacheKey] = CachedNonlocalGuide{ outGuideRGB, hasGuide };
		}
		return hasGuide;
	}
}

//////////////////////////////////////////////////////////////////////
// HyLIoSSkinMaterial
//////////////////////////////////////////////////////////////////////

HyLIoSSkinMaterial::HyLIoSSkinMaterial(
	std::shared_ptr<const void> cacheToken,
	std::shared_ptr<const HyLIoSLocalResponseAsset> asset,
	std::shared_ptr<const HyLIoSSupportedCameraProposal> proposal,
	std::shared_ptr<HyLIoSNonlocalRuntime> nonlocal,
	const RISEPel& guideRGB_,
	const HyLIoSSkinMaterialSummary& summary_
	) :
  pBSDF( 0 ),
  pSPF( 0 ),
  pCacheToken( std::move( cacheToken ) ),
  pAsset( std::move( asset ) ),
  pProposal( std::move( proposal ) ),
  pNonlocal( std::move( nonlocal ) ),
  guideRGB( guideRGB_ ),
  summary( summary_ )
{
	if( pAsset ) {
		pBSDF = new HyLIoSLocalBSDF( pAsset->ResponsePtr(), guideRGB, summary.hasVisibleGuide );
		GlobalLog()->PrintNew( pBSDF, __FILE__, __LINE__, "HyLIoSLocalBSDF" );
	}
	pSPF = new HyLIoSLocalSPF( pProposal, guideRGB, summary.hasVisibleGuide );
	GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "HyLIoSLocalSPF" );
}

HyLIoSSkinMaterial::~HyLIoSSkinMaterial()
{
	safe_release( pBSDF );
	safe_release( pSPF );
}

bool HyLIoSSkinMaterial::EvaluateVisibleGuideAlbedo(
	const RayIntersectionGeometric&,
	RISEPel& outAlbedo
	) const
{
	outAlbedo = summary.hasVisibleGuide ? guideRGB : RISEPel( 0, 0, 0 );
	return true;
}

bool HyLIoSSkinMaterial::CoversWavelengthRange( const Scalar nmBegin, const Scalar nmEnd ) const
{
	const double a = static_cast<double>( nmBegin );
	const double b = static_cast<double>( nmEnd );
	if( !std::isfinite( a ) || !std::isfinite( b ) || a > b ) {
		return false;
	}
	if( pAsset ) {
		return pAsset->Response().Covers( a, b );
	}
	if( pNonlocal ) {
		return a >= summary.coveredFirstNm && b <= summary.coveredLastNm;
	}
	return false;
}

std::uint64_t HyLIoSSkinMaterial::DomainBoundaryLossCount() const noexcept
{
	return pNonlocal ? pNonlocal->domainBoundaryLosses.load( std::memory_order_relaxed ) : 0ULL;
}

bool HyLIoSSkinMaterial::HasSmoothNonlocalBoundary() const
{
	return pNonlocal && pNonlocal->boundaryRoughness == 0.0;
}

bool HyLIoSSkinMaterial::GetNonlocalBoundaryParamsNM(
	const Scalar nm,
	double& outRoughness,
	double& outEtaExt,
	double& outEtaSC
	) const noexcept
{
	if( !pNonlocal ) {
		return false;
	}
	const double wavelength = static_cast<double>( nm );
	if( !std::isfinite( wavelength ) ||
		wavelength < summary.coveredFirstNm || wavelength > summary.coveredLastNm )
	{
		return false;
	}
	double etaSC = 1.4;
	if( !pNonlocal->InterpolateStratumCorneumIOR( wavelength, etaSC ) ) {
		return false;
	}
	outRoughness = pNonlocal->boundaryRoughness;
	outEtaExt = pNonlocal->exteriorIOR;
	outEtaSC = etaSC;
	return true;
}

bool HyLIoSSkinMaterial::SampleNonlocalNM(
	const RayIntersectionGeometric& ri,
	const Scalar nm,
	ISampler& sampler,
	HyLIoSNonlocalSample& outSample,
	const std::function<void( const HyLIoSExitBoundaryEncounter& )>& onInteriorBoundaryEncounter
	) const
{
	outSample = HyLIoSNonlocalSample{};
	if( !pNonlocal ) {
		return false;
	}
	const double wavelength = static_cast<double>( nm );
	std::size_t wlLo = 0, wlHi = 0;
	double wlT = 0.0;
	if( !pNonlocal->SelectKernelBracket( wavelength, wlLo, wlHi, wlT ) ) {
		return false;
	}

	const RightHandedSurfaceFrame rhFrame = BuildRightHandedFrame( ri );
	if( !rhFrame.valid ) {
		return false;
	}

	const Vector3 woWorld = Vector3Ops::Normalize( -ri.ray.Dir() );
	const SlabVector woLocal{
		static_cast<double>( Vector3Ops::Dot( woWorld, rhFrame.T ) ),
		static_cast<double>( Vector3Ops::Dot( woWorld, rhFrame.B ) ),
		static_cast<double>( Vector3Ops::Dot( woWorld, rhFrame.N ) )
	};
	if( !( woLocal.z > 0.0 ) ) {
		return false;
	}

	// Unbiased stochastic wavelength-node selection between adjacent resolved nodes.
	const std::size_t wlIdx = ( wlHi > wlLo && static_cast<double>( sampler.Get1D() ) < wlT ) ? wlHi : wlLo;
	const HyLIoSForwardTransport& kernel = *pNonlocal->kernels[wlIdx];
	const double etaSC = pNonlocal->scIORAtNode[wlIdx];

	const HyLIoSReciprocalBoundary boundary(
		pNonlocal->boundaryRoughness, pNonlocal->exteriorIOR, etaSC );

	// Step 1: Outer boundary interaction at entry point.
	const ReciprocalBoundarySample entryStep = boundary.SampleFull(
		woLocal,
		static_cast<double>( sampler.Get1D() ),
		static_cast<double>( sampler.Get1D() ),
		static_cast<double>( sampler.Get1D() ) );
	if( !entryStep.valid || !( entryStep.weight > 0.0 ) ) {
		return false;
	}

	const SurfaceFrame3D entryFrame{ ri.ptIntersection, rhFrame.T, rhFrame.B, rhFrame.N };

	if( entryStep.isReflection ) {
		outSample.escaped = true;
		outSample.isExternalReflection = true;
		outSample.isDelta = entryStep.isDelta;
		outSample.exitPos = ri.ptIntersection;
		outSample.exitNormal = rhFrame.N;
		outSample.exitDir = Vector3Ops::Normalize(
			rhFrame.T * Scalar( entryStep.direction.x ) +
			rhFrame.B * Scalar( entryStep.direction.y ) +
			rhFrame.N * Scalar( entryStep.direction.z ) );
		outSample.throughput = Scalar( entryStep.weight );
		outSample.pdf = Scalar( entryStep.pdf );
		outSample.lateralDisplacementMetres = 0.0;
		return true;
	}

	// Step 2: Drive the P2 `HyLIoSForwardTransport` kernel one quantum at a time,
	// checking finite lateral side walls on EVERY step so no walk can leave and
	// re-enter the footprint unnoticed (workplan §7 item 1).
	const std::uint64_t seedLo = static_cast<std::uint64_t>(
		std::clamp( static_cast<double>( sampler.Get1D() ), 0.0, 1.0 ) * 4294967295.0 );
	const std::uint64_t seedHi = static_cast<std::uint64_t>(
		std::clamp( static_cast<double>( sampler.Get1D() ), 0.0, 1.0 ) * 4294967295.0 );
	ForwardExperimentRandom kernelRng( ( seedHi << 32 ) ^ seedLo ^ 0x9e3779b97f4a7c15ULL );

	ForwardState state = kernel.Begin( { 0.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0 } );
	const SlabBoundaryEncounter initEnc = state.PendingBoundary();
	kernel.ResumeBoundary( state, initEnc.token, SlabBoundaryDecision{
		BoundaryAction::Transmit,
		SlabSide::Interior,
		{ entryStep.direction.x, -entryStep.direction.y, -entryStep.direction.z },
		entryStep.weight,
		SlabBoundaryAngularLaw::Unclassified
	} );

	auto mapToDomain = [&]( const double xM, const double yM, const SlabVector& localOutDir ) -> SpatialExitMapping {
		if( pNonlocal->domainKind == SpatialDomainKind::PlanarPolygon ) {
			const PlanarPolygonDomain dom = PlanarPolygonDomain::MakeRectangle(
				pNonlocal->domainHalfWidthMetres, pNonlocal->domainHalfWidthMetres,
				pNonlocal->totalThicknessMetres, pNonlocal->physicalScale );
			return dom.MapExit( entryFrame, 0.0, 0.0, xM, yM, localOutDir );
		} else if( pNonlocal->domainKind == SpatialDomainKind::ConcentricSphere ) {
			const Point3 center = Point3Ops::mkPoint3(
				ri.ptIntersection,
				rhFrame.N * Scalar( -pNonlocal->domainRadiusMetres * pNonlocal->physicalScale ) );
			const ConcentricSphereDomain dom(
				center, pNonlocal->domainRadiusMetres,
				pNonlocal->totalThicknessMetres, pNonlocal->physicalScale );
			return dom.MapExit( entryFrame, xM, yM, localOutDir );
		} else if( pNonlocal->domainKind == SpatialDomainKind::ConcentricCylinder ) {
			const Point3 axisOrigin = Point3Ops::mkPoint3(
				ri.ptIntersection,
				rhFrame.N * Scalar( -pNonlocal->domainRadiusMetres * pNonlocal->physicalScale ) );
			const ConcentricCylinderDomain dom(
				axisOrigin, rhFrame.B, pNonlocal->domainRadiusMetres,
				pNonlocal->domainHalfWidthMetres, pNonlocal->totalThicknessMetres, pNonlocal->physicalScale );
			return dom.MapExit( entryFrame, xM, yM, localOutDir );
		}
		IntrinsicLayerChart chart;
		chart.uMin = 0.0; chart.uMax = 1.0;
		chart.vMin = 0.0; chart.vMax = 1.0;
		chart.metresPerU = 2.0 * pNonlocal->domainHalfWidthMetres;
		chart.metresPerV = 2.0 * pNonlocal->domainHalfWidthMetres;
		const IntrinsicLayerVolumeDomain dom(
			chart, pNonlocal->totalThicknessMetres, pNonlocal->physicalScale );
		return dom.MapExit( entryFrame, 0.5, 0.5, xM, yM, localOutDir );
	};

	const std::uint64_t workCap = std::max<std::uint64_t>( 64, pNonlocal->maxSubsurfaceSteps * 8ULL );
	const std::size_t advanceQuantum = ( pNonlocal->domainKind == SpatialDomainKind::ConcentricSphere ) ? 64u : 1u;
	while( state.Termination() == ForwardTermination::None &&
		   state.Counters().workUnits < workCap )
	{
		const ForwardAdvanceResult adv = kernel.Advance( state, kernelRng, advanceQuantum );

		// Step-wise absorbing side-wall check after every single quantum!
		const SlabVector posOracle = state.Position();
		const double curXM = posOracle.x;
		const double curYM = -posOracle.y;
		if( !pNonlocal->IsInsideLateralBounds( curXM, curYM ) ) {
			kernel.StopForBudget( state );
			pNonlocal->domainBoundaryLosses.fetch_add( 1ULL, std::memory_order_relaxed );
			outSample.escaped = false;
			outSample.domainBoundaryLoss = true;
			outSample.lateralDisplacementMetres = std::hypot( curXM, curYM );
			return false;
		}

		if( adv.kind == ForwardAdvanceKind::OuterBoundaryEncounter ) {
			const SlabBoundaryEncounter enc = state.PendingBoundary();
			const double xM = enc.position.x;
			const double yM = -enc.position.y;
			// Convert oracle upward arrival direction (z < 0) to boundary-frame inward `woInt` (z < 0).
			const SlabVector woInt{ -enc.incidentDirection.x, enc.incidentDirection.y, enc.incidentDirection.z };

			const SpatialExitMapping probeMap = mapToDomain( xM, yM, { 0.0, 0.0, 1.0 } );
			if( probeMap.outcome == DomainMapOutcome::DomainBoundaryLoss ) {
				kernel.StopForBudget( state );
				pNonlocal->domainBoundaryLosses.fetch_add( 1ULL, std::memory_order_relaxed );
				outSample.escaped = false;
				outSample.domainBoundaryLoss = true;
				outSample.lateralDisplacementMetres = probeMap.lateralDisplacementMetres;
				return false;
			}

			// Yield conditional exit boundary encounter to caller for `K_t` NEE/MIS
			// BEFORE sampling the Fresnel R/T branch.
			if( onInteriorBoundaryEncounter && !boundary.IsSmooth() ) {
				const SpatialExitMapping tanMap = mapToDomain( xM, yM, { 1.0, 0.0, 0.0 } );
				const Vector3 exitT = Vector3Ops::Normalize( tanMap.worldDir );
				const Vector3 exitB = Vector3Ops::Normalize( Vector3Ops::Cross( probeMap.worldNormal, exitT ) );
				HyLIoSExitBoundaryEncounter exitEnc;
				exitEnc.exitPos           = probeMap.worldPos;
				exitEnc.exitNormal        = probeMap.worldNormal;
				exitEnc.exitTangent       = exitT;
				exitEnc.exitBitangent     = exitB;
				exitEnc.interiorWoLocal   = woInt;
				exitEnc.interiorWeight    = enc.weight;
				exitEnc.boundaryRoughness = pNonlocal->boundaryRoughness;
				exitEnc.etaExt            = pNonlocal->exteriorIOR;
				exitEnc.etaSC             = etaSC;
				onInteriorBoundaryEncounter( exitEnc );
			}

			const ReciprocalBoundarySample exitStep = boundary.SampleFull(
				woInt, kernelRng.Get1D(), kernelRng.Get1D(), kernelRng.Get1D() );
			if( !exitStep.valid || !( exitStep.weight > 0.0 ) ) {
				kernel.StopForBudget( state );
				return false;
			}
			if( exitStep.isReflection ) {
				kernel.ResumeBoundary( state, enc.token, SlabBoundaryDecision{
					BoundaryAction::Reflect,
					SlabSide::Interior,
					{ exitStep.direction.x, -exitStep.direction.y, -exitStep.direction.z },
					exitStep.weight,
					SlabBoundaryAngularLaw::Unclassified
				} );
				continue;
			}

			const SpatialExitMapping mapped = mapToDomain( xM, yM, exitStep.direction );
			kernel.StopForBudget( state );
			if( mapped.outcome == DomainMapOutcome::DomainBoundaryLoss ) {
				pNonlocal->domainBoundaryLosses.fetch_add( 1ULL, std::memory_order_relaxed );
				outSample.escaped = false;
				outSample.domainBoundaryLoss = true;
				outSample.lateralDisplacementMetres = mapped.lateralDisplacementMetres;
				return false;
			}

			outSample.escaped = true;
			outSample.isExternalReflection = false;
			outSample.isDelta = exitStep.isDelta;
			outSample.exitPos = mapped.worldPos;
			outSample.exitNormal = mapped.worldNormal;
			outSample.exitDir = mapped.worldDir;
			outSample.throughput = Scalar( enc.weight * exitStep.weight );
			outSample.pdf = Scalar( exitStep.pdf );
			outSample.lateralDisplacementMetres = mapped.lateralDisplacementMetres;
			return true;
		}

		if( adv.kind == ForwardAdvanceKind::Terminal ) {
			return false;
		}
	}

	return false;
}

std::size_t HyLIoSSkinMaterial::ActiveCachedAssetCount()
{
	AssetCacheState& cache = GlobalAssetCache();
	std::lock_guard<std::mutex> lock( cache.mutex );
	std::size_t live = 0;
	for( auto it = cache.entries.begin(); it != cache.entries.end(); ) {
		if( it->second.expired() ) {
			it = cache.entries.erase( it );
		} else {
			++live;
			++it;
		}
	}
	return live;
}

HyLIoSSkinMaterial* HyLIoSSkinMaterial::Create(
	const HyLIoSSkinMaterialConfig& config,
	std::string* outError
	)
{
	if( config.structVersion != HyLIoSSkinMaterialConfig::kCurrentVersion ||
		config.structSize != sizeof( HyLIoSSkinMaterialConfig ) ) {
		FailCreate( outError, "invalid HyLIoSSkinMaterialConfig version or structSize" );
		return nullptr;
	}

	const std::string modelRev    = config.modelRevision     ? config.modelRevision     : "";
	const std::string dataRev     = config.dataRevision      ? config.dataRevision      : "";
	const std::string transport   = config.transportMode     ? config.transportMode     : "";
	const std::string boundary    = config.boundaryMode      ? config.boundaryMode      : "";
	const std::string boundaryRev = config.boundaryRevision  ? config.boundaryRevision  : "";
	const std::string placement   = config.placementPolicy   ? config.placementPolicy   : "";
	const std::string postScatter = config.postScatterPolicy ? config.postScatterPolicy : "";
	const std::string preset      = config.preset            ? config.preset            : "";
	const std::string assetPath   = config.responseAssetPath ? config.responseAssetPath : "";
	const std::string specPath    = config.specimenSourcePath ? config.specimenSourcePath : "";

	if( !std::isfinite( config.exteriorIOR ) || !( config.exteriorIOR > 0.0 ) ) {
		FailCreate( outError, "exterior_ior must be finite and > 0" );
		return nullptr;
	}
	if( !std::isfinite( config.physicalScale ) || !( config.physicalScale > 0.0 ) ) {
		FailCreate( outError, "physical_scale must be finite and > 0" );
		return nullptr;
	}
	if( !std::isfinite( config.fiberRadiusMetres ) || !( config.fiberRadiusMetres > 0.0 ) ||
		!std::isfinite( config.fiberFraction ) || config.fiberFraction < 0.0 || config.fiberFraction > 1.0 ||
		!std::isfinite( config.fiberRelativeIOR ) || !( config.fiberRelativeIOR > 0.0 ) ) {
		FailCreate( outError, "fiber parameters outside finite supported range" );
		return nullptr;
	}

	if( transport == "nonlocal" ) {
		if( !assetPath.empty() ) {
			FailCreate( outError, "nonlocal transport_mode rejects `response_asset` (only `baked_local` uses response_asset)" );
			return nullptr;
		}
		if( specPath.empty() ) {
			FailCreate( outError, "nonlocal transport_mode requires a non-empty `specimen_source` path" );
			return nullptr;
		}
		ForwardPlacementPolicy nlPlacement = ForwardPlacementPolicy::NullCollisionPlacementV1;
		if( placement == "null_collision_placement_v1" ) {
			nlPlacement = ForwardPlacementPolicy::NullCollisionPlacementV1;
		} else if( placement == "fixed_event_diagnostic_v1" ) {
			nlPlacement = ForwardPlacementPolicy::FixedEventDiagnosticV1;
		} else {
			FailCreate( outError, "unsupported placement_policy `" + placement + "`" );
			return nullptr;
		}
		ForwardPostScatterPolicy nlPostScatter = ForwardPostScatterPolicy::DiscardAfterScatterV1;
		if( postScatter == "discard_after_scatter_v1" ) {
			nlPostScatter = ForwardPostScatterPolicy::DiscardAfterScatterV1;
		} else if( postScatter == "diagnostic_retain_particle_v1" ) {
			nlPostScatter = ForwardPostScatterPolicy::DiagnosticRetainParticleV1;
		} else {
			FailCreate( outError, "unsupported post_scatter_policy `" + postScatter + "`" );
			return nullptr;
		}
		const std::string domStr = config.spatialDomain ? config.spatialDomain : "planar_polygon";
		SpatialDomainKind domKind = SpatialDomainKind::PlanarPolygon;
		if( domStr == "planar_polygon" ) {
			domKind = SpatialDomainKind::PlanarPolygon;
		} else if( domStr == "concentric_sphere" ) {
			domKind = SpatialDomainKind::ConcentricSphere;
		} else if( domStr == "concentric_cylinder" ) {
			domKind = SpatialDomainKind::ConcentricCylinder;
		} else if( domStr == "intrinsic_layer" ) {
			domKind = SpatialDomainKind::IntrinsicLayerVolume;
		} else {
			FailCreate( outError, "unsupported spatial_domain `" + domStr + "`" );
			return nullptr;
		}
		try {
			const HyLIoSReciprocalBoundary validateBoundary( config.boundaryRoughness, config.exteriorIOR, 1.4 );
			(void)validateBoundary;

			const LocalAssetRecord rawSpec = LocalAssetDetail::ReadRegularFile( specPath, std::uint64_t{64} * 1024 * 1024 );
			const std::string sourceBytes( reinterpret_cast<const char*>( rawSpec.data() ), rawSpec.size() );
			auto rt = BuildNonlocalRuntime( sourceBytes, config, domKind, nlPlacement, nlPostScatter );

			const std::string specSHA = RISE::RISECBOR64::SHA256Hex( rawSpec );
			std::ostringstream guideKey;
			guideKey.imbue( std::locale::classic() );
			guideKey << std::setprecision( 17 )
			         << specSHA << "|" << config.exteriorIOR << "|" << config.boundaryRoughness
			         << "|" << config.fiberRadiusMetres << "|" << config.fiberFraction
			         << "|" << config.fiberRelativeIOR << "|" << placement << "|" << postScatter
			         << "|" << ( config.explicitComplexPackV1 ? 1 : 0 );

			RISEPel computedGuide( 0, 0, 0 );
			const bool hasGuide = ComputeCachedNonlocalVisibleGuideRGB( *rt, guideKey.str(), computedGuide );

			HyLIoSSkinMaterialSummary sum;
			sum.modelRevision      = modelRev;
			sum.dataRevision       = dataRev;
			sum.transportMode      = transport;
			sum.boundaryMode       = boundary.empty() ? std::string( ReciprocalBoundaryVersion ) : boundary;
			sum.boundaryRevision   = ReciprocalBoundaryVersion;
			sum.placementPolicy    = placement;
			sum.postScatterPolicy  = postScatter;
			sum.preset             = preset;
			sum.specimenSourcePath = specPath;
			sum.semanticSHA256     = specSHA;
			sum.exteriorIOR        = config.exteriorIOR;
			sum.physicalScale      = config.physicalScale;
			sum.coveredFirstNm     = rt->wavelengthsNm.front();
			sum.coveredLastNm      = rt->wavelengthsNm.back();
			sum.hasVisibleGuide    = hasGuide;

			HyLIoSSkinMaterial* pMat = new HyLIoSSkinMaterial(
				nullptr, nullptr, nullptr, std::move( rt ), computedGuide, sum );
			GlobalLog()->PrintNew( pMat, __FILE__, __LINE__, "HyLIoSSkinMaterial (nonlocal)" );
			return pMat;
		} catch( const std::exception& ex ) {
			FailCreate( outError, ex.what() );
			return nullptr;
		}
	}

	if( transport != "baked_local" ) {
		FailCreate( outError, "transport_mode `" + transport + "` is not supported (expected `baked_local` or `nonlocal`)" );
		return nullptr;
	}
	if( assetPath.empty() ) {
		FailCreate( outError, "baked_local transport_mode requires a non-empty `response_asset` path" );
		return nullptr;
	}
	if( config.maximumFileBytes == 0 || config.maximumResidentBytes == 0 || config.aggregateResidentCap == 0 ) {
		FailCreate( outError, "memory/file safety budgets must be positive" );
		return nullptr;
	}

	ExperimentBoundary expBoundary = ExperimentBoundary::Smooth;
	if( boundary == "smooth" ) {
		expBoundary = ExperimentBoundary::Smooth;
	} else if( boundary == "hylios2015" ) {
		expBoundary = ExperimentBoundary::HyLIoS2015;
	} else {
		FailCreate( outError, "unsupported boundary_mode `" + boundary + "` (expected `smooth` or `hylios2015`)" );
		return nullptr;
	}

	ForwardPlacementPolicy expPlacement = ForwardPlacementPolicy::NullCollisionPlacementV1;
	if( placement == "null_collision_placement_v1" ) {
		expPlacement = ForwardPlacementPolicy::NullCollisionPlacementV1;
	} else if( placement == "fixed_event_diagnostic_v1" ) {
		expPlacement = ForwardPlacementPolicy::FixedEventDiagnosticV1;
	} else {
		FailCreate( outError, "unsupported placement_policy `" + placement + "`" );
		return nullptr;
	}

	ForwardPostScatterPolicy expPostScatter = ForwardPostScatterPolicy::DiscardAfterScatterV1;
	if( postScatter == "discard_after_scatter_v1" ) {
		expPostScatter = ForwardPostScatterPolicy::DiscardAfterScatterV1;
	} else if( postScatter == "diagnostic_retain_particle_v1" ) {
		expPostScatter = ForwardPostScatterPolicy::DiagnosticRetainParticleV1;
	} else {
		FailCreate( outError, "unsupported post_scatter_policy `" + postScatter + "`" );
		return nullptr;
	}

	try {
		LocalAssetLimits limits;
		limits.maximumFileBytes = config.maximumFileBytes;
		limits.maximumResidentBytes = config.maximumResidentBytes;
		limits.response.maximumStorageBytes = static_cast<std::size_t>(
			std::min<std::uint64_t>( config.maximumResidentBytes, std::numeric_limits<std::size_t>::max() ) );

		LocalAssetSemanticIdentity expectedSemantic;
		if( config.useStandaloneAssetCodec ) {
			if( !config.explicitSemanticIdentity ) {
				FailCreate( outError, "standalone asset codec requires an explicit LocalAssetSemanticIdentity" );
				return nullptr;
			}
			expectedSemantic = *static_cast<const LocalAssetSemanticIdentity*>( config.explicitSemanticIdentity );
		} else {
			if( specPath.empty() ) {
				FailCreate( outError, "baked_local mode requires `specimen_source` to independently derive and verify the asset semantic identity" );
				return nullptr;
			}
			const LocalAssetRecord rawSpec = LocalAssetDetail::ReadRegularFile( specPath, std::uint64_t{64} * 1024 * 1024 );
			const std::string sourceBytes( reinterpret_cast<const char*>( rawSpec.data() ), rawSpec.size() );

			LocalBakeConfig bakeCfg;
			bakeCfg.boundary = expBoundary;
			bakeCfg.placement = expPlacement;
			bakeCfg.postScatter = expPostScatter;
			bakeCfg.explicitComplexPackV1 = config.explicitComplexPackV1;
			bakeCfg.exteriorIOR = config.exteriorIOR;
			bakeCfg.fiberRadiusMetres = config.fiberRadiusMetres;
			bakeCfg.fiberFraction = config.fiberFraction;
			bakeCfg.fiberRelativeIOR = config.fiberRelativeIOR;
			bakeCfg.limits = limits;
			if( config.wavelengthsNm && config.wavelengthCount > 0 ) {
				bakeCfg.wavelengthsNm.assign( config.wavelengthsNm, config.wavelengthsNm + config.wavelengthCount );
			} else {
				bakeCfg.wavelengthsNm = ExtractSpecimenWavelengths( sourceBytes );
			}
			expectedSemantic = PrepareLocalBakeSemantic( sourceBytes, bakeCfg );
		}

		const std::string semanticHash = HashSemanticIdentity( expectedSemantic );
		std::ostringstream keyStream;
		keyStream << ( config.useStandaloneAssetCodec ? "standalone:" : "bake:" )
		          << assetPath << "|" << semanticHash << "|"
		          << limits.maximumFileBytes << "|" << limits.maximumResidentBytes;
		const std::string cacheKey = keyStream.str();

		std::shared_ptr<const CachedAssetBundle> bundle;
		{
			AssetCacheState& cache = GlobalAssetCache();
			std::lock_guard<std::mutex> lock( cache.mutex );
			auto it = cache.entries.find( cacheKey );
			if( it != cache.entries.end() ) {
				bundle = it->second.lock();
			}
		}

		if( !bundle ) {
			std::shared_ptr<const HyLIoSLocalResponseAsset> loadedAsset = config.useStandaloneAssetCodec
				? LoadLocalResponseAsset( assetPath, expectedSemantic, limits )
				: LoadLocalBake( assetPath, expectedSemantic, limits );
			std::shared_ptr<const HyLIoSSupportedCameraProposal> loadedProposal =
				HyLIoSSupportedCameraProposal::Create( loadedAsset->ResponsePtr(), LocalResponseSamplingGrid{ 32u } );

			RISEPel computedGuide( 0, 0, 0 );
			const bool hasGuide = ComputeHyLIoSVisibleGuideRGB(
				loadedAsset->WavelengthsNm(),
				loadedAsset->NormalIncidenceGuide(),
				computedGuide );

			auto fresh = std::make_shared<CachedAssetBundle>();
			fresh->asset = std::move( loadedAsset );
			fresh->proposal = std::move( loadedProposal );
			fresh->guideRGB = computedGuide;
			fresh->hasVisibleGuide = hasGuide;
			fresh->residentBytesBound = fresh->asset->PeakResidentBytesBound();

			AssetCacheState& cache = GlobalAssetCache();
			std::lock_guard<std::mutex> lock( cache.mutex );
			// Check aggregate resident budget across all currently live distinct assets.
			std::uint64_t liveBytes = 0;
			for( auto it = cache.entries.begin(); it != cache.entries.end(); ) {
				if( auto existing = it->second.lock() ) {
					if( it->first == cacheKey ) {
						bundle = existing;
						break;
					}
					if( existing->residentBytesBound > std::numeric_limits<std::uint64_t>::max() - liveBytes ) {
						FailCreate( outError, "aggregate HyLIoS resident memory budget overflow" );
						return nullptr;
					}
					liveBytes += existing->residentBytesBound;
					++it;
				} else {
					it = cache.entries.erase( it );
				}
			}
			if( !bundle ) {
				if( fresh->residentBytesBound > config.aggregateResidentCap ||
					liveBytes > config.aggregateResidentCap - fresh->residentBytesBound ) {
					FailCreate( outError, "aggregate HyLIoS resident memory budget exceeded; raise aggregate_resident_budget or reduce asset count" );
					return nullptr;
				}
				cache.entries[cacheKey] = fresh;
				bundle = std::move( fresh );
			}
		}

		HyLIoSSkinMaterialSummary sum;
		sum.modelRevision      = modelRev;
		sum.dataRevision       = dataRev;
		sum.transportMode      = transport;
		sum.boundaryMode       = boundary;
		sum.boundaryRevision   = boundaryRev;
		sum.placementPolicy    = placement;
		sum.postScatterPolicy  = postScatter;
		sum.preset             = preset;
		sum.responseAssetPath  = assetPath;
		sum.specimenSourcePath = specPath;
		sum.contentSHA256      = bundle->asset->ContentSHA256();
		sum.semanticSHA256     = bundle->asset->SemanticSHA256();
		sum.productionSHA256   = bundle->asset->ProductionSHA256();
		sum.exteriorIOR        = config.exteriorIOR;
		sum.physicalScale      = config.physicalScale;
		sum.coveredFirstNm     = bundle->asset->Semantic().coveredFirstNm;
		sum.coveredLastNm      = bundle->asset->Semantic().coveredLastNm;
		sum.residentBytesBound = bundle->residentBytesBound;
		sum.hasVisibleGuide    = bundle->hasVisibleGuide;

		HyLIoSSkinMaterial* pMat = new HyLIoSSkinMaterial(
			bundle,
			bundle->asset,
			bundle->proposal,
			nullptr,
			bundle->guideRGB,
			sum );
		GlobalLog()->PrintNew( pMat, __FILE__, __LINE__, "HyLIoSSkinMaterial" );
		return pMat;
	} catch( const std::exception& ex ) {
		FailCreate( outError, ex.what() );
		return nullptr;
	}
}
