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
#include <limits>
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
	struct HyLIoSNonlocalLayerTable
	{
		double thicknessMetres = 0.0;
		std::vector<double> absorptionPerMetre;
		std::vector<double> ior;
		double scatteringScalePerMetre = 0.0;
	};

	struct HyLIoSNonlocalRuntime
	{
		std::vector<double> wavelengthsNm;
		std::array<HyLIoSNonlocalLayerTable, 6> layers;
		std::array<double, 7> layerPlanes{};
		double totalThicknessMetres = 0.0;
		double exteriorIOR = 1.0;
		double boundaryRoughness = 0.25;
		double physicalScale = 1.0;
		SpatialDomainKind domainKind = SpatialDomainKind::PlanarPolygon;
		double domainHalfWidthMetres = 0.05;
		double domainRadiusMetres = 0.10;
		std::uint64_t maxSubsurfaceSteps = 256;
		mutable std::atomic<std::uint64_t> domainBoundaryLosses{ 0 };

		bool InterpolateLayerAtNM(
			const int layerIdx,
			const double nm,
			double& outMuA,
			double& outMuS,
			double& outIOR
			) const noexcept
		{
			if( wavelengthsNm.empty() || layerIdx < 0 || layerIdx >= 6 ) {
				return false;
			}
			if( nm < wavelengthsNm.front() || nm > wavelengthsNm.back() ) {
				return false;
			}
			const auto& L = layers[static_cast<std::size_t>( layerIdx )];
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
			outMuA = ( 1.0 - t ) * L.absorptionPerMetre[lo] + t * L.absorptionPerMetre[hi];
			outIOR = ( 1.0 - t ) * L.ior[lo] + t * L.ior[hi];
			// Dermal collagen Rayleigh/Mie-like scattering scales smoothly with (550/nm)^1.5.
			const double wlFactor = std::pow( 550.0 / nm, 1.5 );
			outMuS = L.scatteringScalePerMetre * wlFactor;
			return std::isfinite( outMuA ) && std::isfinite( outMuS ) && std::isfinite( outIOR ) && outIOR > 0.0;
		}
	};
}

namespace
{
	std::shared_ptr<HyLIoSNonlocalRuntime> BuildNonlocalRuntime(
		const std::string& sourceBytes,
		const HyLIoSSkinMaterialConfig& config,
		const SpatialDomainKind domainKind
		)
	{
		RISE::Agent::JsonValue root;
		std::string err;
		if( !RISE::Agent::JsonParse( sourceBytes, root, err ) || !root.isObject() ) {
			throw std::runtime_error( "nonlocal specimen_source JSON parse error: " + err );
		}
		const RISE::Agent::JsonValue& wlArr = root.get( "wavelength_nm" );
		const RISE::Agent::JsonValue& layerArr = root.get( "layers" );
		if( !wlArr.isArray() || wlArr.size() == 0 || !layerArr.isArray() || layerArr.size() != 6 ) {
			throw std::runtime_error( "nonlocal specimen_source requires non-empty wavelength_nm and 6 layers" );
		}

		auto rt = std::make_shared<HyLIoSNonlocalRuntime>();
		rt->wavelengthsNm.reserve( wlArr.size() );
		double prevNm = 0.0;
		for( std::size_t i = 0; i < wlArr.size(); ++i ) {
			const double nm = wlArr.at( i ).asNumber();
			if( !std::isfinite( nm ) || nm <= prevNm ) {
				throw std::runtime_error( "nonlocal specimen_source wavelength_nm must be strictly increasing" );
			}
			prevNm = nm;
			rt->wavelengthsNm.push_back( nm );
		}

		rt->layerPlanes[0] = 0.0;
		for( std::size_t li = 0; li < 6; ++li ) {
			const RISE::Agent::JsonValue& L = layerArr.at( li );
			const double th = L.get( "thickness_m" ).asNumber();
			const RISE::Agent::JsonValue& absArr = L.get( "host_absorption_m_inv" );
			const RISE::Agent::JsonValue& iorArr = L.get( "real_refractive_index" );
			if( !std::isfinite( th ) || !( th > 0.0 ) ||
				!absArr.isArray() || absArr.size() != rt->wavelengthsNm.size() ||
				!iorArr.isArray() || iorArr.size() != rt->wavelengthsNm.size() )
			{
				throw std::runtime_error( "nonlocal specimen_source layer array length or thickness invalid" );
			}
			rt->layers[li].thicknessMetres = th;
			rt->layers[li].absorptionPerMetre.resize( rt->wavelengthsNm.size() );
			rt->layers[li].ior.resize( rt->wavelengthsNm.size() );
			for( std::size_t k = 0; k < rt->wavelengthsNm.size(); ++k ) {
				const double a = absArr.at( k ).asNumber();
				const double n = iorArr.at( k ).asNumber();
				if( !std::isfinite( a ) || a < 0.0 || !std::isfinite( n ) || !( n > 0.0 ) ) {
					throw std::runtime_error( "nonlocal specimen_source contains invalid absorption or IOR" );
				}
				rt->layers[li].absorptionPerMetre[k] = a;
				rt->layers[li].ior[k] = n;
			}
			// Epidermal layers (0..3) carry moderate cell scattering; dermal layers (4..5)
			// carry collagen fiber scattering scaled by authored fiberFraction.
			rt->layers[li].scatteringScalePerMetre = ( li < 4 )
				? 2000.0
				: ( 12000.0 * std::max( 0.05, config.fiberFraction ) );
			rt->layerPlanes[li + 1] = rt->layerPlanes[li] + th;
		}
		rt->totalThicknessMetres = rt->layerPlanes[6];
		rt->exteriorIOR = config.exteriorIOR;
		rt->boundaryRoughness = config.boundaryRoughness;
		rt->physicalScale = config.physicalScale;
		rt->domainKind = domainKind;
		rt->domainHalfWidthMetres = config.domainHalfWidthMetres;
		rt->domainRadiusMetres = config.domainRadiusMetres;
		rt->maxSubsurfaceSteps = std::max<std::uint64_t>( 16, config.maxSubsurfaceSteps );

		// Validate domain parameters upfront.
		if( domainKind == SpatialDomainKind::ConcentricSphere ||
			domainKind == SpatialDomainKind::ConcentricCylinder )
		{
			if( !( rt->domainRadiusMetres > rt->totalThicknessMetres ) ) {
				throw std::invalid_argument( "nonlocal concentric domain radius must exceed total skin thickness" );
			}
		}
		return rt;
	}

	// Deterministic preparation-time 1D slab visible reflectance guide (no response_asset needed).
	bool ComputeNonlocalVisibleGuideRGB(
		const HyLIoSNonlocalRuntime& rt,
		RISEPel& outGuideRGB
		)
	{
		std::vector<double> refl( rt.wavelengthsNm.size(), 0.0 );
		for( std::size_t k = 0; k < rt.wavelengthsNm.size(); ++k ) {
			const double nm = rt.wavelengthsNm[k];
			double muA0 = 0, muS0 = 0, etaSC = 1.4;
			rt.InterpolateLayerAtNM( 0, nm, muA0, muS0, etaSC );
			const double F0 = HyLIoSReciprocalBoundary::FresnelDielectric( 1.0, rt.exteriorIOR, etaSC );
			// Two-way Kubelka-Munk / Eddington 1D slab reflectance across the 6 layers.
			double totalTauAbs = 0.0;
			double totalTauScat = 0.0;
			for( int li = 0; li < 6; ++li ) {
				double a = 0, s = 0, n = 1.4;
				rt.InterpolateLayerAtNM( li, nm, a, s, n );
				totalTauAbs  += a * rt.layers[static_cast<std::size_t>( li )].thicknessMetres;
				totalTauScat += s * rt.layers[static_cast<std::size_t>( li )].thicknessMetres;
			}
			const double omega = ( totalTauScat + totalTauAbs > 0.0 )
				? ( totalTauScat / ( totalTauScat + totalTauAbs ) )
				: 0.0;
			const double rDiff = omega * ( 1.0 - std::exp( -( totalTauScat + totalTauAbs ) ) ) * std::exp( -0.5 * totalTauAbs );
			refl[k] = std::clamp( F0 + ( 1.0 - F0 ) * ( 1.0 - F0 ) * rDiff, 0.0, 1.0 );
		}
		return ComputeHyLIoSVisibleGuideRGB( rt.wavelengthsNm, refl, outGuideRGB );
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

bool HyLIoSSkinMaterial::SampleNonlocalNM(
	const RayIntersectionGeometric& ri,
	const Scalar nm,
	ISampler& sampler,
	HyLIoSNonlocalSample& outSample
	) const
{
	outSample = HyLIoSNonlocalSample{};
	if( !pNonlocal ) {
		return false;
	}
	const double wavelength = static_cast<double>( nm );
	if( !std::isfinite( wavelength ) ||
		wavelength < summary.coveredFirstNm || wavelength > summary.coveredLastNm )
	{
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

	double muA0 = 0, muS0 = 0, etaSC = 1.4;
	if( !pNonlocal->InterpolateLayerAtNM( 0, wavelength, muA0, muS0, etaSC ) ) {
		return false;
	}

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

	// Step 2: Bounded iterative 6-layer subsurface slab walk.
	// Local slab coordinates: z_m in [0, totalThicknessMetres] increasing INWARD (-N).
	double xM = 0.0, yM = 0.0, zM = 1e-9;
	// Propagation direction inside slab: d.z > 0 goes DEEPER into skin (-N).
	SlabVector dir{ entryStep.direction.x, entryStep.direction.y, -entryStep.direction.z };
	double weight = entryStep.weight;
	int layer = 0;

	for( std::uint64_t step = 0; step < pNonlocal->maxSubsurfaceSteps; ++step ) {
		double muA = 0, muS = 0, etaCur = 1.4;
		if( !pNonlocal->InterpolateLayerAtNM( layer, wavelength, muA, muS, etaCur ) ) {
			return false;
		}
		const double muT = muA + muS;
		const double zTop = pNonlocal->layerPlanes[static_cast<std::size_t>( layer )];
		const double zBot = pNonlocal->layerPlanes[static_cast<std::size_t>( layer + 1 )];

		// Distance to nearest layer boundary along `dir.z`.
		double distBoundary = 1e30;
		bool hitsTop = false;
		if( dir.z < -1e-12 ) {
			distBoundary = ( zTop - zM ) / dir.z;
			hitsTop = true;
		} else if( dir.z > 1e-12 ) {
			distBoundary = ( zBot - zM ) / dir.z;
			hitsTop = false;
		}
		distBoundary = std::max( 0.0, distBoundary );

		// Free-flight distance to scattering/absorption event.
		const double uDist = std::clamp( static_cast<double>( sampler.Get1D() ), 1e-12, 1.0 - 1e-12 );
		const double distScatter = ( muT > 1e-12 ) ? ( -std::log( uDist ) / muT ) : 1e30;

		if( distScatter < distBoundary ) {
			// Advance to interaction point inside current layer.
			xM += dir.x * distScatter;
			yM += dir.y * distScatter;
			zM = std::clamp( zM + dir.z * distScatter, zTop, zBot );

			const double albedo = ( muT > 0.0 ) ? ( muS / muT ) : 0.0;
			weight *= albedo;
			if( !( weight > 1e-8 ) ) {
				return false;
			}
			// Russian roulette after 4 steps.
			if( step >= 4 ) {
				const double q = std::min( 0.95, std::max( 0.05, weight ) );
				if( static_cast<double>( sampler.Get1D() ) >= q ) {
					return false;
				}
				weight /= q;
			}

			// Isotropic / forward-peaked Henyey-Greenstein scatter (g = 0.75 for skin tissue).
			const double g = 0.75;
			const double uCos = static_cast<double>( sampler.Get1D() );
			const double uPhi = static_cast<double>( sampler.Get1D() );
			const double sqrTerm = ( 1.0 - g * g ) / ( 1.0 - g + 2.0 * g * uCos );
			const double cosTheta = std::clamp( ( 1.0 + g * g - sqrTerm * sqrTerm ) / ( 2.0 * g ), -1.0, 1.0 );
			const double sinTheta = std::sqrt( std::max( 0.0, 1.0 - cosTheta * cosTheta ) );
			const double phi = 2.0 * 3.14159265358979323846 * uPhi;

			// Build local frame around `dir`.
			const SlabVector up = ( std::fabs( dir.z ) < 0.9 ) ? SlabVector{ 0, 0, 1 } : SlabVector{ 1, 0, 0 };
			const SlabVector uAxRaw{
				up.y * dir.z - up.z * dir.y,
				up.z * dir.x - up.x * dir.z,
				up.x * dir.y - up.y * dir.x
			};
			const double uLen = std::sqrt( uAxRaw.x * uAxRaw.x + uAxRaw.y * uAxRaw.y + uAxRaw.z * uAxRaw.z );
			const SlabVector uAx{ uAxRaw.x / uLen, uAxRaw.y / uLen, uAxRaw.z / uLen };
			const SlabVector vAx{
				dir.y * uAx.z - dir.z * uAx.y,
				dir.z * uAx.x - dir.x * uAx.z,
				dir.x * uAx.y - dir.y * uAx.x
			};
			const double cPhi = std::cos( phi );
			const double sPhi = std::sin( phi );
			dir = {
				sinTheta * ( cPhi * uAx.x + sPhi * vAx.x ) + cosTheta * dir.x,
				sinTheta * ( cPhi * uAx.y + sPhi * vAx.y ) + cosTheta * dir.y,
				sinTheta * ( cPhi * uAx.z + sPhi * vAx.z ) + cosTheta * dir.z
			};
			const double dLen = std::sqrt( dir.x * dir.x + dir.y * dir.y + dir.z * dir.z );
			if( !( dLen > 0.0 ) ) return false;
			dir = { dir.x / dLen, dir.y / dLen, dir.z / dLen };
		} else {
			// Advance to layer boundary.
			xM += dir.x * distBoundary;
			yM += dir.y * distBoundary;
			zM = hitsTop ? zTop : zBot;

			if( hitsTop && layer == 0 ) {
				// Reached outer stratum corneum boundary from inside!
				// Incoming interior direction pointing AWAY from boundary (into -Z interior):
				// arrival is `dir` with `dir.z < 0` (upward), so `woInt` pointing away into interior
				// has `woInt = { -dir.x, -dir.y, dir.z }` with `woInt.z < 0`!
				const SlabVector woInt{ -dir.x, -dir.y, dir.z };
				const ReciprocalBoundarySample exitStep = boundary.SampleFull(
					woInt,
					static_cast<double>( sampler.Get1D() ),
					static_cast<double>( sampler.Get1D() ),
					static_cast<double>( sampler.Get1D() ) );
				if( !exitStep.valid || !( exitStep.weight > 0.0 ) ) {
					return false;
				}
				if( exitStep.isReflection ) {
					// Reflected back into stratum corneum (`exitStep.direction.z < 0`).
					weight *= exitStep.weight;
					dir = { exitStep.direction.x, exitStep.direction.y, -exitStep.direction.z };
					zM = 1e-9;
					continue;
				}

				// Transmitted out to exterior (`exitStep.direction.z > 0`)!
				// Map lateral displacement `(xM, yM)` through the configured finite spatial domain.
				SpatialExitMapping mapped;
				if( pNonlocal->domainKind == SpatialDomainKind::PlanarPolygon ) {
					const PlanarPolygonDomain dom = PlanarPolygonDomain::MakeRectangle(
						pNonlocal->domainHalfWidthMetres, pNonlocal->domainHalfWidthMetres,
						pNonlocal->totalThicknessMetres, pNonlocal->physicalScale );
					mapped = dom.MapExit( entryFrame, 0.0, 0.0, xM, yM, exitStep.direction );
				} else if( pNonlocal->domainKind == SpatialDomainKind::ConcentricSphere ) {
					const Point3 center = Point3Ops::mkPoint3(
						ri.ptIntersection,
						rhFrame.N * Scalar( -pNonlocal->domainRadiusMetres * pNonlocal->physicalScale ) );
					const ConcentricSphereDomain dom(
						center, pNonlocal->domainRadiusMetres,
						pNonlocal->totalThicknessMetres, pNonlocal->physicalScale );
					mapped = dom.MapExit( entryFrame, xM, yM, exitStep.direction );
				} else if( pNonlocal->domainKind == SpatialDomainKind::ConcentricCylinder ) {
					const Point3 axisOrigin = Point3Ops::mkPoint3(
						ri.ptIntersection,
						rhFrame.N * Scalar( -pNonlocal->domainRadiusMetres * pNonlocal->physicalScale ) );
					const ConcentricCylinderDomain dom(
						axisOrigin, rhFrame.B, pNonlocal->domainRadiusMetres,
						pNonlocal->domainHalfWidthMetres, pNonlocal->totalThicknessMetres, pNonlocal->physicalScale );
					mapped = dom.MapExit( entryFrame, xM, yM, exitStep.direction );
				} else {
					IntrinsicLayerChart chart;
					chart.uMin = 0.0; chart.uMax = 1.0;
					chart.vMin = 0.0; chart.vMax = 1.0;
					chart.metresPerU = 2.0 * pNonlocal->domainHalfWidthMetres;
					chart.metresPerV = 2.0 * pNonlocal->domainHalfWidthMetres;
					const IntrinsicLayerVolumeDomain dom(
						chart, pNonlocal->totalThicknessMetres, pNonlocal->physicalScale );
					mapped = dom.MapExit( entryFrame, 0.5, 0.5, xM, yM, exitStep.direction );
				}

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
				outSample.throughput = Scalar( weight * exitStep.weight );
				outSample.pdf = Scalar( exitStep.pdf );
				outSample.lateralDisplacementMetres = mapped.lateralDisplacementMetres;
				return true;
			} else if( !hitsTop && layer == 5 ) {
				// Reached deep hypodermis boundary: absorbed.
				return false;
			} else {
				// Internal layer interface between `layer` and `nextLayer`.
				const int nextLayer = hitsTop ? ( layer - 1 ) : ( layer + 1 );
				double nA = 0, nS = 0, etaNext = etaCur;
				pNonlocal->InterpolateLayerAtNM( nextLayer, wavelength, nA, nS, etaNext );
				const double cosI = std::fabs( dir.z );
				const double F = HyLIoSReciprocalBoundary::FresnelDielectric( cosI, etaCur, etaNext );
				if( static_cast<double>( sampler.Get1D() ) < F ) {
					dir.z = -dir.z;
				} else {
					const double ratio = etaCur / etaNext;
					const double sinT2 = ratio * ratio * std::max( 0.0, 1.0 - cosI * cosI );
					if( sinT2 >= 1.0 ) {
						dir.z = -dir.z;
					} else {
						const double cosT = std::sqrt( std::max( 0.0, 1.0 - sinT2 ) );
						dir = { ratio * dir.x, ratio * dir.y, ( dir.z >= 0.0 ? cosT : -cosT ) };
						layer = nextLayer;
					}
				}
			}
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
		if( specPath.empty() ) {
			FailCreate( outError, "nonlocal transport_mode requires a non-empty `specimen_source` path" );
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
			// Validate boundary roughness upfront via HyLIoSReciprocalBoundary.
			const HyLIoSReciprocalBoundary validateBoundary( config.boundaryRoughness, config.exteriorIOR, 1.4 );
			(void)validateBoundary;

			const LocalAssetRecord rawSpec = LocalAssetDetail::ReadRegularFile( specPath, std::uint64_t{64} * 1024 * 1024 );
			const std::string sourceBytes( reinterpret_cast<const char*>( rawSpec.data() ), rawSpec.size() );
			auto rt = BuildNonlocalRuntime( sourceBytes, config, domKind );

			RISEPel computedGuide( 0, 0, 0 );
			const bool hasGuide = ComputeNonlocalVisibleGuideRGB( *rt, computedGuide );

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
			sum.semanticSHA256     = RISE::RISECBOR64::SHA256Hex( rawSpec );
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
