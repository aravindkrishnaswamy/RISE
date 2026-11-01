//////////////////////////////////////////////////////////////////////
//
//  MaterialPreview.cpp - See MaterialPreview.h.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MaterialPreview.h"
#include "../Interfaces/IMaterialManager.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IEmitter.h"
#include "../Interfaces/ISPF.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/ColorMath.h"
#include "../Utilities/Color/ColorUtils.h"
#include "../Utilities/IORStack.h"
#include "../Utilities/Optics.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace RISE
{

namespace
{
	const int kSpectralSamples = 12;
	const Scalar kLoNm = Scalar( 390.0 );
	const Scalar kHiNm = Scalar( 710.0 );
	const Scalar kSphereRadius = Scalar( 0.84 );
	const Scalar kCameraZ = Scalar( 2.6 );
	const Scalar kTanHalfFov = Scalar( 0.38 );

	inline Scalar ClampScalar( Scalar v, Scalar lo, Scalar hi )
	{
		if( !( v > lo ) ) return lo;
		if( v > hi ) return hi;
		return v;
	}

	inline RISEPel SanitizePel( const RISEPel& c )
	{
		return RISEPel(
			( c[0] > Scalar( 0 ) && std::isfinite( c[0] ) ) ? c[0] : Scalar( 0 ),
			( c[1] > Scalar( 0 ) && std::isfinite( c[1] ) ) ? c[1] : Scalar( 0 ),
			( c[2] > Scalar( 0 ) && std::isfinite( c[2] ) ) ? c[2] : Scalar( 0 ) );
	}

	//! Soft filmic/Reinhard shoulder above 0.85 so bright specular highlights
	//! and luminaire materials compress smoothly into [0, 1] before gamma 2.2.
	unsigned char EncodePreviewChannel( Scalar linear )
	{
		Scalar c = ( linear > Scalar( 0 ) && std::isfinite( linear ) ) ? linear : Scalar( 0 );
		if( c > Scalar( 0.80 ) ) {
			const Scalar excess = c - Scalar( 0.80 );
			c = Scalar( 0.80 ) + Scalar( 0.20 ) * ( excess / ( excess + Scalar( 0.35 ) ) );
		}
		c = ClampScalar( c, Scalar( 0 ), Scalar( 1 ) );
		const double enc = std::pow( static_cast<double>( c ), 1.0 / 2.2 );
		int v = static_cast<int>( enc * 255.0 + 0.5 );
		if( v < 0 ) v = 0;
		if( v > 255 ) v = 255;
		return static_cast<unsigned char>( v );
	}

	void WriteRGBA( std::vector<unsigned char>& rgba, std::size_t idx, const RISEPel& c )
	{
		rgba[idx + 0] = EncodePreviewChannel( c[0] );
		rgba[idx + 1] = EncodePreviewChannel( c[1] );
		rgba[idx + 2] = EncodePreviewChannel( c[2] );
		rgba[idx + 3] = 255;
	}

	bool ValidDims( unsigned int w, unsigned int h, std::string& err )
	{
		if( w == 0 || h == 0 ) {
			err = "width and height must both be > 0";
			return false;
		}
		if( w > MaterialPreview::kMaxDim || h > MaterialPreview::kMaxDim ) {
			err = "width/height exceeds the " + std::to_string( MaterialPreview::kMaxDim ) + "-pixel preview cap";
			return false;
		}
		return true;
	}

	void ComputeDecimatedGrid(
		unsigned int w,
		unsigned int h,
		unsigned int budget,
		unsigned int& gw,
		unsigned int& gh )
	{
		const std::uint64_t total = static_cast<std::uint64_t>( w ) * static_cast<std::uint64_t>( h );
		if( total <= static_cast<std::uint64_t>( budget ) ) {
			gw = w;
			gh = h;
			return;
		}
		const double s = std::sqrt( static_cast<double>( budget ) / static_cast<double>( total ) );
		gw = static_cast<unsigned int>( std::max( 1.0, std::floor( static_cast<double>( w ) * s ) ) );
		gh = static_cast<unsigned int>( std::max( 1.0, std::floor( static_cast<double>( h ) * s ) ) );
		if( gw > w ) gw = w;
		if( gh > h ) gh = h;
		while( static_cast<std::uint64_t>( gw ) * static_cast<std::uint64_t>( gh ) > static_cast<std::uint64_t>( budget ) && ( gw > 1 || gh > 1 ) ) {
			if( gw >= gh && gw > 1 ) --gw;
			else if( gh > 1 ) --gh;
			else --gw;
		}
	}

	void UpscaleRGBANearest(
		const std::vector<unsigned char>& src,
		unsigned int sw,
		unsigned int sh,
		std::vector<unsigned char>& dst,
		unsigned int dw,
		unsigned int dh )
	{
		if( sw == dw && sh == dh ) {
			dst = src;
			return;
		}
		dst.assign( static_cast<std::size_t>( dw ) * dh * 4, 0 );
		for( unsigned int y = 0; y < dh; ++y ) {
			unsigned int sy = static_cast<unsigned int>( ( static_cast<std::uint64_t>( y ) * sh ) / dh );
			if( sy >= sh ) sy = sh - 1;
			for( unsigned int x = 0; x < dw; ++x ) {
				unsigned int sx = static_cast<unsigned int>( ( static_cast<std::uint64_t>( x ) * sw ) / dw );
				if( sx >= sw ) sx = sw - 1;
				const std::size_t si = ( static_cast<std::size_t>( sy ) * sw + sx ) * 4;
				const std::size_t di = ( static_cast<std::size_t>( y ) * dw + x ) * 4;
				dst[di + 0] = src[si + 0];
				dst[di + 1] = src[si + 1];
				dst[di + 2] = src[si + 2];
				dst[di + 3] = src[si + 3];
			}
		}
	}

	//! Neutral studio checker backdrop evaluated along a world ray `(orig, dir)`.
	//! Gives refractive/transmissive spheres a crisp 3D spatial pattern to bend,
	//! while staying dark and neutral outside the sphere silhouette.
	RISEPel BackdropCheckerRGB( const Point3& orig, const Vector3& dir )
	{
		// Intersect back plane at z = -1.35 when dir.z < -1e-4
		Scalar bx = dir.x * Scalar( 2.5 );
		Scalar by = dir.y * Scalar( 2.5 );
		if( dir.z < Scalar( -1e-4 ) ) {
			const Scalar t = ( Scalar( -1.35 ) - orig.z ) / dir.z;
			if( t > Scalar( 0 ) ) {
				bx = orig.x + t * dir.x;
				by = orig.y + t * dir.y;
			}
		}
		const int cx = static_cast<int>( std::floor( bx * 3.2 ) );
		const int cy = static_cast<int>( std::floor( by * 3.2 ) );
		const bool even = ( ( cx + cy ) & 1 ) == 0;
		const Scalar r2 = bx * bx + by * by;
		const Scalar vignette = Scalar( 1.0 ) / ( Scalar( 1.0 ) + Scalar( 0.18 ) * r2 );
		const RISEPel tile = even
			? RISEPel( 0.055, 0.058, 0.066 )
			: RISEPel( 0.028, 0.030, 0.035 );
		return tile * vignette;
	}

	//! Brighter transmissive-backdrop checker used behind the sphere when a ray
	//! refracts through a dielectric or transmissive material, so glass and
	//! dispersive materials read unmistakably as transparent lenses.
	RISEPel RefractedBackdropRGB( const Point3& orig, const Vector3& dir )
	{
		Scalar bx = dir.x * Scalar( 2.2 );
		Scalar by = dir.y * Scalar( 2.2 );
		if( dir.z < Scalar( -1e-4 ) ) {
			const Scalar t = ( Scalar( -1.35 ) - orig.z ) / dir.z;
			if( t > Scalar( 0 ) ) {
				bx = orig.x + t * dir.x;
				by = orig.y + t * dir.y;
			}
		}
		const int cx = static_cast<int>( std::floor( bx * 3.6 ) );
		const int cy = static_cast<int>( std::floor( by * 3.6 ) );
		const bool even = ( ( cx + cy ) & 1 ) == 0;
		const Scalar skyMix = ClampScalar( Scalar( 0.5 ) + Scalar( 0.5 ) * dir.y, Scalar( 0 ), Scalar( 1 ) );
		const RISEPel sky = RISEPel( 0.18, 0.20, 0.24 ) * ( Scalar( 1 ) - skyMix )
			+ RISEPel( 0.58, 0.64, 0.74 ) * skyMix;
		const Scalar checkerFactor = even ? Scalar( 1.15 ) : Scalar( 0.42 );
		return sky * checkerFactor;
	}

	//! Studio environment dome evaluated along reflection direction `r`.
	RISEPel StudioEnvironmentRGB( const Vector3& r )
	{
		const Vector3 keyDir = Vector3Ops::Normalize( Vector3( -0.42, 0.58, 0.70 ) );
		const Vector3 fillDir = Vector3Ops::Normalize( Vector3( 0.65, 0.22, 0.55 ) );
		const Vector3 rimDir = Vector3Ops::Normalize( Vector3( 0.10, 0.88, -0.45 ) );

		const Scalar skyT = ClampScalar( Scalar( 0.5 ) + Scalar( 0.5 ) * r.y, Scalar( 0 ), Scalar( 1 ) );
		RISEPel env = RISEPel( 0.07, 0.08, 0.10 ) * ( Scalar( 1 ) - skyT )
			+ RISEPel( 0.30, 0.34, 0.42 ) * skyT;

		// Horizon studio band
		const Scalar horizon = std::exp( -Scalar( 10.0 ) * r.y * r.y );
		env = env + RISEPel( 0.14, 0.15, 0.18 ) * horizon;

		// Softbox reflections in the environment dome
		const Scalar keyCos = ClampScalar( Vector3Ops::Dot( r, keyDir ), Scalar( 0 ), Scalar( 1 ) );
		const Scalar fillCos = ClampScalar( Vector3Ops::Dot( r, fillDir ), Scalar( 0 ), Scalar( 1 ) );
		const Scalar rimCos = ClampScalar( Vector3Ops::Dot( r, rimDir ), Scalar( 0 ), Scalar( 1 ) );
		env = env + RISEPel( 1.35, 1.28, 1.18 ) * std::pow( keyCos, Scalar( 18.0 ) );
		env = env + RISEPel( 0.42, 0.48, 0.62 ) * std::pow( fillCos, Scalar( 10.0 ) );
		env = env + RISEPel( 0.55, 0.58, 0.64 ) * std::pow( rimCos, Scalar( 14.0 ) );
		return env;
	}

	//! Integrate a per-wavelength scalar response `evalAtNM(nm)` across the visible
	//! band [390, 710] nm into D65-normalized linear Rec.709 RGB (the same white-
	//! balanced CMF quadrature used by ThinFilm::ReflectanceConductorRGBSpectral).
	template<class Fn>
	RISEPel IntegrateSpectralToRGB( const Fn& evalAtNM )
	{
		const Scalar step = ( kHiNm - kLoNm ) / Scalar( kSpectralSamples );
		Scalar Xn = 0, Yn = 0, Zn = 0;
		Scalar Xe = 0, Ye = 0, Ze = 0;
		for( int s = 0; s < kSpectralSamples; ++s ) {
			const Scalar nm = kLoNm + ( Scalar( s ) + Scalar( 0.5 ) ) * step;
			XYZPel cmf;
			if( !ColorUtils::XYZFromNM( cmf, nm ) ) continue;
			Scalar val = evalAtNM( nm );
			if( !( val > Scalar( 0 ) ) || !std::isfinite( val ) ) val = Scalar( 0 );
			if( val > Scalar( 12.0 ) ) val = Scalar( 12.0 );
			Xn += val * cmf.X * step;
			Yn += val * cmf.Y * step;
			Zn += val * cmf.Z * step;
			Xe += cmf.X * step;
			Ye += cmf.Y * step;
			Ze += cmf.Z * step;
		}
		const Scalar relX = ( Xe > Scalar( 0 ) ) ? ( Xn / Xe ) : Scalar( 0 );
		const Scalar relY = ( Ye > Scalar( 0 ) ) ? ( Yn / Ye ) : Scalar( 0 );
		const Scalar relZ = ( Ze > Scalar( 0 ) ) ? ( Zn / Ze ) : Scalar( 0 );
		const XYZPel whiteXYZ = ColorUtils::Rec709RGBtoXYZ( Rec709RGBPel( Scalar( 1 ), Scalar( 1 ), Scalar( 1 ) ) );
		XYZPel xyz( relX * whiteXYZ.X, relY * whiteXYZ.Y, relZ * whiteXYZ.Z );
		return SanitizePel( ColorUtils::XYZtoRec709RGB( xyz ) );
	}

	//! Construct a seam-free spherical RayIntersectionGeometric for a point on
	//! the preview sphere with unit normal `N` and incoming camera ray `ray`.
	RayIntersectionGeometric MakeSphereIntersection(
		const Ray& ray,
		const Point3& hitPt,
		const Vector3& N,
		unsigned int px,
		unsigned int py )
	{
		RayIntersectionGeometric ri( ray, nullRasterizerState );
		ri.bHit = true;
		ri.rast.x = px;
		ri.rast.y = py;
		ri.range = Point3Ops::Distance( ray.origin, hitPt );

		// Spherical UV mapping: front hemisphere sweeps u in [0, 1], v in [0, 1].
		const Scalar phi = std::atan2( N.x, N.z );
		Scalar u = Scalar( 0.5 ) + phi / PI;
		u = u - std::floor( u );
		const Scalar theta = std::acos( ClampScalar( N.y, Scalar( -1 ), Scalar( 1 ) ) );
		const Scalar v = ClampScalar( Scalar( 1.0 ) - theta / PI, Scalar( 0 ), Scalar( 1 ) );
		ri.ptCoord = Point2( u, v );

		// Unit-scale world/object coordinates on the sphere surface for 3D painters.
		ri.ptIntersection = Point3( N.x * Scalar( 0.5 ), N.y * Scalar( 0.5 ), N.z * Scalar( 0.5 ) );
		ri.ptObjIntersec  = ri.ptIntersection;
		ri.vNormal        = N;
		ri.vGeomNormal    = N;

		// Continuous latitude/longitude tangent frame (U = east longitude, V = north latitude, W = N)
		// so anisotropic materials (GGX, Ward, Ashikhmin-Shirley) never hit aCreateFromW seam.
		Vector3 U( N.z, Scalar( 0 ), -N.x );
		const Scalar uLenSq = Vector3Ops::SquaredModulus( U );
		if( uLenSq > Scalar( 1e-8 ) ) {
			U = U * ( Scalar( 1.0 ) / std::sqrt( uLenSq ) );
		} else {
			U = Vector3( Scalar( 1 ), Scalar( 0 ), Scalar( 0 ) );
		}
		const Vector3 V = Vector3Ops::Normalize( Vector3Ops::Cross( N, U ) );
		ri.onb = OrthonormalBasis3D( U, V, N );

		ri.txFootprint.widthValid  = true;
		ri.txFootprint.worldWidth  = Scalar( 0 );
		ri.txFootprint.objectWidth = Scalar( 0 );
		// Finite studio softbox angular filter width so ultra-smooth microfacet
		// materials (alpha ~ 0.001) still catch an anti-aliased highlight on a
		// 40x40 or 56x56 thumbnail.
		ri.glossyFilterWidth = Scalar( 0.035 );
		return ri;
	}

	//! Shade a delta/specular-only material (Dielectric, PerfectReflector,
	//! PerfectRefractor) using per-wavelength GetSpecularInfoNM so dispersive
	//! Sellmeier glasses bend each wavelength at its true refractive index.
	RISEPel ShadeDeltaSphereHit(
		const IMaterial& mat,
		const RayIntersectionGeometric& ri,
		const Point3& hitPt,
		const Vector3& N )
	{
		const Vector3 d = ri.ray.Dir();
		const Vector3 reflDir = Vector3Ops::Normalize( d - N * ( Scalar( 2.0 ) * Vector3Ops::Dot( d, N ) ) );
		const RISEPel envRefl = StudioEnvironmentRGB( reflDir );
		const Scalar cosI = ClampScalar( -Vector3Ops::Dot( d, N ), Scalar( 0 ), Scalar( 1 ) );
		const IORStack iorStack( Scalar( 1.0 ) );

		return IntegrateSpectralToRGB( [&]( Scalar nm ) -> Scalar {
			const SpecularInfo info = mat.GetSpecularInfoNM( ri, iorStack, nm );
			const Scalar atten = ClampScalar( info.attenuationNM, Scalar( 0 ), Scalar( 4 ) );
			// Map the RGB environment/backdrop to a wavelength proxy via luminance + chromatic channel selection
			const int band = ( nm < Scalar( 495.0 ) ) ? 2 : ( ( nm < Scalar( 585.0 ) ) ? 1 : 0 );
			const Scalar reflEnv = envRefl[band];

			if( !info.canRefract ) {
				const Scalar reflWeight = info.attenuationAppliesToReflection ? atten : Scalar( 1.0 );
				return reflEnv * reflWeight;
			}

			const Scalar eta = std::max( Scalar( 0.05 ), std::isfinite( info.ior ) ? info.ior : Scalar( 1.5 ) );
			Vector3 t1 = d;
			Scalar F = Scalar( 1.0 );
			const bool refractedIn = Optics::CalculateRefractedRay( N, Scalar( 1.0 ), eta, t1 );
			if( refractedIn ) {
				F = Optics::CalculateDielectricReflectance( d, t1, N, Scalar( 1.0 ), eta );
			}
			// PerfectRefractorMaterial is non-Fresnel in transport; keep a subtle Schlick rim so the sphere form reads.
			if( !info.attenuationAppliesToReflection && !info.attenuationIsInteriorTransmittance ) {
				F = Scalar( 0.06 ) + Scalar( 0.25 ) * std::pow( Scalar( 1.0 ) - cosI, Scalar( 5.0 ) );
			}
			F = ClampScalar( F, Scalar( 0 ), Scalar( 1 ) );

			Scalar result = F * reflEnv * ( info.attenuationAppliesToReflection ? atten : Scalar( 1.0 ) );
			if( refractedIn && F < Scalar( 1.0 ) ) {
				t1 = Vector3Ops::Normalize( t1 );
				const Scalar cosT = ClampScalar( -Vector3Ops::Dot( t1, N ), Scalar( 0.05 ), Scalar( 1.0 ) );
				const Scalar chord = Scalar( 2.0 ) * kSphereRadius * cosT;
				const Point3 exitPt = Point3Ops::mkPoint3( hitPt, t1 * chord );
				const Vector3 exitN = Vector3Ops::Normalize( Vector3( exitPt.x, exitPt.y, exitPt.z ) );
				Vector3 t2 = t1;
				if( !Optics::CalculateRefractedRay( -exitN, eta, Scalar( 1.0 ), t2 ) ) {
					t2 = t1;
				}
				t2 = Vector3Ops::Normalize( t2 );
				const RISEPel bgRGB = RefractedBackdropRGB( exitPt, t2 );
				const Scalar T = info.attenuationIsInteriorTransmittance
					? std::pow( ClampScalar( atten, Scalar( 0 ), Scalar( 1 ) ), chord )
					: atten;
				result += ( Scalar( 1.0 ) - F ) * T * bgRGB[band];
			}
			return result;
		} );
	}

	//! Shade any hit point on the preview sphere.
	RISEPel ShadeSphereHit(
		const IMaterial& mat,
		const Ray& ray,
		const Point3& hitPt,
		const Vector3& N,
		unsigned int px,
		unsigned int py )
	{
		const RayIntersectionGeometric ri = MakeSphereIntersection( ray, hitPt, N, px, py );
		const Vector3 V = Vector3Ops::Normalize( -ray.Dir() );
		const Scalar ndv = ClampScalar( Vector3Ops::Dot( N, V ), Scalar( 0 ), Scalar( 1 ) );

		RISEPel total( 0, 0, 0 );

		// 1. Emissive contribution (lambertian_luminaire_material, phong_luminaire_material)
		if( const IEmitter* emitter = mat.GetEmitter() ) {
			RISEPel Le = SanitizePel( emitter->emittedRadiance( ri, V, N ) );
			if( ColorMath::MaxValue( Le ) <= Scalar( 0 ) ) {
				Le = SanitizePel( emitter->radiantExitanceAt( ri ) * INV_PI );
			}
			total = total + Le;
		}

		const IBSDF* bsdf = mat.GetBSDF();
		const IORStack iorStack( Scalar( 1.0 ) );
		const SpecularInfo specInfo = mat.GetSpecularInfo( ri, iorStack );

		// 2. Pure delta materials (Dielectric, PerfectReflector, PerfectRefractor)
		if( !bsdf && specInfo.valid && specInfo.isSpecular ) {
			return total + ShadeDeltaSphereHit( mat, ri, hitPt, N );
		}

		// 3. BSDF materials (evaluated spectrally across [390, 710] nm)
		if( bsdf ) {
			const Vector3 keyDir  = Vector3Ops::Normalize( Vector3( -0.42, 0.58, 0.70 ) );
			const Vector3 fillDir = Vector3Ops::Normalize( Vector3( 0.65, 0.22, 0.55 ) );
			const Vector3 rimDir  = Vector3Ops::Normalize( Vector3( 0.10, 0.88, -0.45 ) );
			const Vector3 backDir = Vector3Ops::Normalize( Vector3( 0.15, 0.20, -0.96 ) );

			const Scalar nDotKey  = ClampScalar( Vector3Ops::Dot( N, keyDir ),  Scalar( 0 ), Scalar( 1 ) );
			const Scalar nDotFill = ClampScalar( Vector3Ops::Dot( N, fillDir ), Scalar( 0 ), Scalar( 1 ) );
			const Scalar nDotRim  = ClampScalar( Vector3Ops::Dot( N, rimDir ),  Scalar( 0 ), Scalar( 1 ) );
			const bool canTransmit = mat.CouldLightPassThrough();

			const RISEPel keyTint( 1.04, 0.99, 0.92 );
			const RISEPel fillTint( 0.82, 0.90, 1.05 );
			const RISEPel rimTint( 0.92, 0.95, 1.00 );

			RISEPel keySpec( 0, 0, 0 );
			if( nDotKey > Scalar( 0 ) ) {
				keySpec = IntegrateSpectralToRGB( [&]( Scalar nm ) {
					return bsdf->valueStatefulNM( keyDir, ri, nm, nullptr ) * nDotKey * Scalar( 2.55 );
				} );
			}
			RISEPel fillSpec( 0, 0, 0 );
			if( nDotFill > Scalar( 0 ) ) {
				fillSpec = IntegrateSpectralToRGB( [&]( Scalar nm ) {
					return bsdf->valueStatefulNM( fillDir, ri, nm, nullptr ) * nDotFill * Scalar( 1.10 );
				} );
			}
			RISEPel rimSpec( 0, 0, 0 );
			if( nDotRim > Scalar( 0 ) ) {
				rimSpec = IntegrateSpectralToRGB( [&]( Scalar nm ) {
					return bsdf->valueStatefulNM( rimDir, ri, nm, nullptr ) * nDotRim * Scalar( 0.85 );
				} );
			}

			total = total + keySpec * keyTint + fillSpec * fillTint + rimSpec * rimTint;

			if( canTransmit ) {
				const Scalar nDotBack = ClampScalar( -Vector3Ops::Dot( N, backDir ), Scalar( 0 ), Scalar( 1 ) );
				if( nDotBack > Scalar( 0 ) ) {
					const RISEPel transSpec = IntegrateSpectralToRGB( [&]( Scalar nm ) {
						return bsdf->valueStatefulNM( backDir, ri, nm, nullptr ) * nDotBack * Scalar( 1.65 );
					} );
					total = total + transSpec;
				}
			}

			// Studio hemisphere ambient + directional albedo reflection so smooth
			// conductors and thin-film materials display their angle-dependent
			// interference/Fresnel color across the entire sphere face.
			const RISEPel dirAlbedo = SanitizePel( bsdf->albedo( ri ) );
			const Vector3 reflDir = Vector3Ops::Normalize( V * Scalar( -1.0 ) + N * ( Scalar( 2.0 ) * ndv ) );
			const RISEPel studioEnv = StudioEnvironmentRGB( reflDir );
			const Scalar hemiWrap = Scalar( 0.5 ) + Scalar( 0.5 ) * N.y;
			const RISEPel hemiAmb = RISEPel( 0.08, 0.09, 0.11 ) * ( Scalar( 1 ) - hemiWrap )
				+ RISEPel( 0.18, 0.19, 0.22 ) * hemiWrap;
			total = total + dirAlbedo * ( hemiAmb + studioEnv * Scalar( 0.22 ) );
		}

		// 4. Subsurface scattering wrap term (RandomWalkSSS / BSSRDF / BioSpecSkin / GenericHumanTissue)
		if( mat.IsVolumetric() || mat.GetDiffusionProfile() != nullptr || mat.GetRandomWalkSSSParams() != nullptr ) {
			RISEPel sssTint( 0.65, 0.38, 0.28 );
			if( const RandomWalkSSSParams* rw = mat.GetRandomWalkSSSParams() ) {
				for( int c = 0; c < 3; ++c ) {
					const Scalar st = rw->sigma_t[c];
					sssTint[c] = ( st > Scalar( 1e-6 ) )
						? ClampScalar( rw->sigma_s[c] / st, Scalar( 0.05 ), Scalar( 0.98 ) )
						: Scalar( 0.5 );
				}
			} else if( bsdf ) {
				sssTint = SanitizePel( bsdf->albedo( ri ) );
			}
			const Vector3 keyDir = Vector3Ops::Normalize( Vector3( -0.42, 0.58, 0.70 ) );
			const Scalar wrap = ClampScalar( ( Vector3Ops::Dot( N, keyDir ) + Scalar( 0.55 ) ) / Scalar( 1.55 ), Scalar( 0 ), Scalar( 1 ) );
			const Scalar edgeScatter = std::pow( Scalar( 1.0 ) - ndv, Scalar( 2.0 ) );
			total = total + sssTint * ( Scalar( 0.22 ) * wrap + Scalar( 0.16 ) * edgeScatter );
		}

		// 5. Fallback if material has neither BSDF, Emitter, nor SpecularInfo (e.g. PolishedMaterial with null BSDF or custom SPF)
		if( !bsdf && !mat.GetEmitter() && !( specInfo.valid && specInfo.isSpecular ) ) {
			const Scalar halfLambert = Scalar( 0.35 ) + Scalar( 0.65 ) * ndv;
			total = RISEPel( 0.45, 0.47, 0.52 ) * halfLambert;
		}

		return SanitizePel( total );
	}
}

MaterialPreview::Result MaterialPreview::RenderMaterialPreview(
	IJobPriv& job,
	const String& materialName,
	unsigned int w,
	unsigned int h )
{
	Result out;
	if( !ValidDims( w, h, out.refusalReason ) ) return out;
	if( materialName.size() <= 1 || std::strcmp( materialName.c_str(), "none" ) == 0 ) {
		out.refusalReason = "materialName is empty or 'none'";
		return out;
	}

	IMaterialManager* pMatMgr = job.GetMaterials();
	IMaterial* pMat = pMatMgr ? pMatMgr->GetItem( materialName.c_str() ) : nullptr;
	if( !pMat ) {
		out.refusalReason = "no material named `" + std::string( materialName.c_str() ) + "`";
		return out;
	}

	unsigned int gw = w, gh = h;
	ComputeDecimatedGrid( w, h, kMaxSampleBudget, gw, gh );

	const double aspect = static_cast<double>( gw ) / static_cast<double>( gh );
	const Point3 camOrigin( Scalar( 0 ), Scalar( 0 ), kCameraZ );
	// Anti-aliasing band width in normalized screen space
	const Scalar pixelPitch = Scalar( 2.0 ) / static_cast<Scalar>( std::min( gw, gh ) );

	std::vector<unsigned char> gridRGBA( static_cast<std::size_t>( gw ) * gh * 4, 0 );

	for( unsigned int y = 0; y < gh; ++y ) {
		const Scalar ny = Scalar( 1.0 ) - Scalar( 2.0 ) * ( static_cast<Scalar>( y ) + Scalar( 0.5 ) ) / static_cast<Scalar>( gh );
		for( unsigned int x = 0; x < gw; ++x ) {
			const Scalar nx = ( Scalar( 2.0 ) * ( static_cast<Scalar>( x ) + Scalar( 0.5 ) ) / static_cast<Scalar>( gw ) - Scalar( 1.0 ) ) * static_cast<Scalar>( aspect );

			const Vector3 rayDir = Vector3Ops::Normalize( Vector3( nx * kTanHalfFov, ny * kTanHalfFov, Scalar( -1.0 ) ) );
			const Ray ray( camOrigin, rayDir );
			const RISEPel bg = BackdropCheckerRGB( camOrigin, rayDir );

			// Ray-sphere intersection: |camOrigin + t * rayDir|^2 = R^2
			const Scalar b = Vector3Ops::Dot( Vector3( camOrigin.x, camOrigin.y, camOrigin.z ), rayDir );
			const Scalar c = camOrigin.z * camOrigin.z - kSphereRadius * kSphereRadius;
			const Scalar disc = b * b - c;

			//Closest approach distance from sphere center to ray for silhouette anti-aliasing
			const Scalar rClosestSq = std::max( Scalar( 0 ), camOrigin.z * camOrigin.z - b * b );
			const Scalar rClosest = std::sqrt( rClosestSq );
			const Scalar aaHalfWidth = kSphereRadius * pixelPitch * Scalar( 0.75 );
			const Scalar coverage = ClampScalar( ( kSphereRadius + aaHalfWidth - rClosest ) / ( Scalar( 2.0 ) * aaHalfWidth ), Scalar( 0 ), Scalar( 1 ) );

			RISEPel pixelColor = bg;
			if( coverage > Scalar( 0 ) ) {
				Point3 hitPt;
				Vector3 N;
				if( disc >= Scalar( 0 ) ) {
					const Scalar t = -b - std::sqrt( disc );
					hitPt = Point3Ops::mkPoint3( camOrigin, rayDir * t );
					N = Vector3Ops::Normalize( Vector3( hitPt.x, hitPt.y, hitPt.z ) );
				} else {
					// Just outside the exact silhouette within the AA fringe: clamp onto rim
					const Point3 closestPt = Point3Ops::mkPoint3( camOrigin, rayDir * ( -b ) );
					N = Vector3Ops::Normalize( Vector3( closestPt.x, closestPt.y, Scalar( 0.05 ) ) );
					hitPt = Point3( N.x * kSphereRadius, N.y * kSphereRadius, N.z * kSphereRadius );
				}
				const RISEPel sphereColor = ShadeSphereHit( *pMat, ray, hitPt, N, x, y );
				pixelColor = bg * ( Scalar( 1.0 ) - coverage ) + sphereColor * coverage;
			}

			const std::size_t idx = ( static_cast<std::size_t>( y ) * gw + x ) * 4;
			WriteRGBA( gridRGBA, idx, pixelColor );
		}
	}

	out.status = Status::Ok;
	out.width  = w;
	out.height = h;
	UpscaleRGBANearest( gridRGBA, gw, gh, out.rgba, w, h );
	return out;
}

} // namespace RISE
