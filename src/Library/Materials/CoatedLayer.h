//////////////////////////////////////////////////////////////////////
//
//  CoatedLayer.h - Shared closed-form dielectric-layer math for
//    `coated_material` (CoatedBRDF / CoatedSPF / CoatedMaterial),
//    implementing docs/WETNESS_COAT_DESIGN.md §7.4.
//
//  Substrate-through-coat Saunderson / Lekner-Dorf form:
//
//     f_sub(wi,wo) = T(cos_i) * T(cos_o) * A_in * A_out
//                    ---------------------------------- * f_base(wi,wo)
//                         eta^2 * ( 1 - E_ret * R )
//
//  where T = 1 - F, A_* are Beer-Lambert pass transmittances, R is the
//  substrate albedo, and E_ret is the trapped diffuse field's round-trip
//  return fraction (r_i for clear coats; exact absorbing integral in
//  InteriorDiffuseTransport).
//
//  Energy conservation relies on `(1 - r_e)/eta^2 == 1 - r_i`, so `r_i`
//  is derived directly from 21-point Gauss-Legendre `r_e` over this
//  file's `Fresnel` rather than a polynomial fit (accuracy < 1e-5).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef COATED_LAYER_
#define COATED_LAYER_

#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/GaussLegendreQuadrature.h"
#include <array>
#include <cmath>
#include <limits>

namespace RISE
{
	namespace Implementation
	{
		//! Closed-form dielectric-layer helpers (stateless, allocation-free).
		namespace CoatedLayer
		{
			//! Floor on coat GGX alpha: coat is never delta, keeping coated
			//! surfaces visible to NEE/BDPT/VCM (CoatedMaterial does not override
			//! GetSpecularInfo).
			constexpr Scalar kMinCoatAlpha = Scalar( 1e-3 );

			//! Upper bound of the tabulated relative IOR range (clamped above).
			inline Scalar MaxRelativeIOR();

			//! Unpolarised smooth dielectric Fresnel reflectance for outer |cos|
			//! and relative IOR eta = n_coat / n_outer.
			inline Scalar Fresnel( Scalar cosThetaI, const Scalar eta )
			{
				if( eta <= Scalar(0) ) {
					return Scalar(0);
				}
				// Index-matched media: guard before sin2T >= 1 so grazing gives F = 0, not TIR.
				if( eta == Scalar(1) ) {
					return Scalar(0);
				}
				cosThetaI = r_min( r_max( cosThetaI, Scalar(0) ), Scalar(1) );

				const Scalar sin2T = ( Scalar(1) - cosThetaI * cosThetaI ) / ( eta * eta );
				if( sin2T >= Scalar(1) ) {
					return Scalar(1);			// total internal reflection (eta < 1 only)
				}
				const Scalar cosT = sqrt( r_max( Scalar(0), Scalar(1) - sin2T ) );

				const Scalar rs = ( cosThetaI - eta * cosT ) / ( cosThetaI + eta * cosT );
				const Scalar rp = ( eta * cosThetaI - cosT ) / ( eta * cosThetaI + cosT );

				return r_min( Scalar(1), Scalar(0.5) * ( rs * rs + rp * rp ) );
			}

			//! Refracted cosine inside the coat (0 under TIR).
			inline Scalar CosRefracted( Scalar cosThetaI, const Scalar eta )
			{
				if( eta <= Scalar(0) ) {
					return Scalar(1);
				}
				if( eta == Scalar(1) ) {
					// Undeviated ray; avoids grazing cos_t = 0 blowing up 1/cos_t.
					return r_min( r_max( cosThetaI, Scalar(0) ), Scalar(1) );
				}
				cosThetaI = r_min( r_max( cosThetaI, Scalar(0) ), Scalar(1) );
				const Scalar sin2T = ( Scalar(1) - cosThetaI * cosThetaI ) / ( eta * eta );
				if( sin2T >= Scalar(1) ) {
					return Scalar(0);
				}
				return sqrt( r_max( Scalar(0), Scalar(1) - sin2T ) );
			}

			//! Refract outer unit direction `d` (d . n > 0) into the coat.
			inline Vector3 RefractIntoCoat( const Vector3& d, const Vector3& n, const Scalar eta )
			{
				const Scalar c  = Vector3Ops::Dot( d, n );
				const Scalar mu = CosRefracted( c, eta );
				return Vector3Ops::Normalize( ( d - n * c ) * ( Scalar(1) / eta ) + n * mu );
			}

			//! Inverse of RefractIntoCoat for internal direction `dIn` (dIn . n > 0);
			//! returns false on TIR.
			inline bool RefractOutOfCoat( const Vector3& dIn, const Vector3& n, const Scalar eta, Vector3& out, Scalar& cosOut )
			{
				const Scalar mu = Vector3Ops::Dot( dIn, n );
				if( !( mu > Scalar(0) ) ) {
					return false;
				}
				const Scalar s2 = eta * eta * ( Scalar(1) - mu * mu );
				if( s2 >= Scalar(1) ) {
					return false;
				}
				cosOut = sqrt( Scalar(1) - s2 );
				out = Vector3Ops::Normalize( ( dIn - n * mu ) * eta + n * cosOut );
				return true;
			}

			//! Internal Fresnel reflectance at internal cosine `mu` (1 below TIR critical angle).
			inline Scalar FresnelInside( const Scalar mu, const Scalar eta )
			{
				if( !( eta > Scalar(1) ) ) {
					return Scalar(0);
				}
				const Scalar s2 = eta * eta * ( Scalar(1) - mu * mu );
				if( s2 >= Scalar(1) ) {
					return Scalar(1);
				}
				return Fresnel( sqrt( Scalar(1) - s2 ), eta );
			}

			namespace Detail
			{
				static const Scalar kEtaMin   = Scalar( 1.0 );
				static const Scalar kEtaMax   = Scalar( 3.0 );
				static const int    kEtaCount = 201;			// 0.01 steps

				inline const std::array<Scalar, 21>& GLNodes()
				{
					static const std::array<Scalar, 21> n = []() {
						std::array<Scalar, 21> a;
						for( unsigned int i = 0; i < GaussLegendre21::NodeCount; ++i ) a[i] = GaussLegendre21::Nodes[i];
						return a;
					}();
					return n;
				}

				inline const std::array<Scalar, 21>& GLWeights()
				{
					static const std::array<Scalar, 21> w = []() {
						std::array<Scalar, 21> a;
						for( unsigned int i = 0; i < GaussLegendre21::NodeCount; ++i ) a[i] = GaussLegendre21::Weights[i];
						return a;
					}();
					return w;
				}

				inline Scalar ExternalDiffuseFresnelExact( const Scalar eta )
				{
					const std::array<Scalar, 21>& nodes = GLNodes();
					const std::array<Scalar, 21>& wts   = GLWeights();
					Scalar sum = Scalar(0);
					for( std::size_t i = 0; i < nodes.size(); ++i ) {
						const Scalar mu = nodes[i];
						sum += Fresnel( mu, eta ) * ( Scalar(2) * mu * wts[i] );
					}
					return sum;
				}

				inline const std::array<Scalar, kEtaCount>& ReTable()
				{
					static const std::array<Scalar, kEtaCount> t = []() {
						std::array<Scalar, kEtaCount> a{};
						const Scalar step = ( kEtaMax - kEtaMin ) / Scalar( kEtaCount - 1 );
						for( int i = 0; i < kEtaCount; ++i ) {
							a[(std::size_t)i] = ExternalDiffuseFresnelExact( kEtaMin + step * Scalar(i) );
						}
						return a;
					}();
					return t;
				}
			}

			inline Scalar MaxRelativeIOR() { return Detail::kEtaMax; }

			//! External diffuse Fresnel r_e(eta) = 2 * integral_0^1 F(mu, eta) mu dmu
			//! via linearly interpolated table (error < 1e-5).
			inline Scalar ExternalDiffuseFresnel( const Scalar eta )
			{
				const std::array<Scalar, Detail::kEtaCount>& t = Detail::ReTable();
				const Scalar step = ( Detail::kEtaMax - Detail::kEtaMin ) / Scalar( Detail::kEtaCount - 1 );
				const Scalar x = ( eta - Detail::kEtaMin ) / step;

				if( x <= Scalar(0) )                            return t[0];
				if( x >= Scalar( Detail::kEtaCount - 1 ) )      return t[Detail::kEtaCount - 1];

				const int    i = (int)x;
				const Scalar f = x - Scalar(i);
				return t[(std::size_t)i] * ( Scalar(1) - f ) + t[(std::size_t)(i + 1)] * f;
			}

			//! Internal diffuse Fresnel r_i(eta) = 1 - (1 - r_e(eta)) / eta^2
			//! (derived from r_e so the smooth-interface energy identity holds numerically).
			inline Scalar InternalDiffuseFresnel( const Scalar eta )
			{
				if( eta <= Scalar(1) ) {
					return Scalar(0);
				}
				const Scalar ri = Scalar(1) - ( Scalar(1) - ExternalDiffuseFresnel( eta ) ) / ( eta * eta );
				return r_min( r_max( ri, Scalar(0) ), Scalar( 0.999 ) );
			}

			//! Single-pass Beer-Lambert coat transmittance at outer `cosThetaI`.
			//! `applyTint` is decided once from un-uplifted RGB (not `tint < 1`) so
			//! untinted white is a bit-exact no-op on the spectral pipe despite
			//! asymptotic Jakob-Hanika uplift (see docs/SPECTRAL_ILLUMINANT_CONVENTION.md).
			inline Scalar PassTransmittance(
				const Scalar cosThetaI,
				const Scalar eta,
				const Scalar thickness,
				const Scalar absorption,
				const Scalar tint,
				const bool applyTint
				)
			{
				const bool absorbs = ( absorption > Scalar(0) && thickness > Scalar(0) );
				if( !absorbs && !applyTint ) {
					return Scalar(1);
				}

				const Scalar cosT  = r_max( CosRefracted( cosThetaI, eta ), Scalar(1e-4) );
				const Scalar obliq = Scalar(1) / cosT;

				Scalar a = Scalar(1);
				if( absorbs )   a *= exp( -absorption * thickness * obliq );
				if( applyTint ) a *= pow( r_max( tint, Scalar(0) ), obliq );
				return r_min( Scalar(1), r_max( Scalar(0), a ) );
			}

			//! RGB flavour of PassTransmittance (per-channel `tint`).
			inline RISEPel PassTransmittanceRGB(
				const Scalar cosThetaI,
				const Scalar eta,
				const Scalar thickness,
				const Scalar absorption,
				const RISEPel& tint,
				const bool applyTint
				)
			{
				return RISEPel(
					PassTransmittance( cosThetaI, eta, thickness, absorption, tint[0], applyTint ),
					PassTransmittance( cosThetaI, eta, thickness, absorption, tint[1], applyTint ),
					PassTransmittance( cosThetaI, eta, thickness, absorption, tint[2], applyTint ) );
			}

			//! Normal-incidence single-pass optical depth tau = absorption*thickness - ln(tint)
			//! (0 at defaults; +inf when applyTint && tint <= 0).
			inline Scalar OpticalDepth(
				const Scalar thickness,
				const Scalar absorption,
				const Scalar tint,
				const bool applyTint
				)
			{
				Scalar tau = Scalar(0);
				if( absorption > Scalar(0) && thickness > Scalar(0) ) {
					tau += absorption * thickness;
				}
				if( applyTint ) {
					if( !( tint > Scalar(0) ) ) {
						return std::numeric_limits<Scalar>::infinity();
					}
					tau -= log( tint );
				}
				return r_max( tau, Scalar(0) );
			}

			//! Exact diffuse hemispherical transport inside an absorbing coat
			//! (a(mu) = exp(-tau/mu); split at critical cosine muC with 21-pt GL on
			//! TIR and refracted arcs, evaluated as a deficit from tau = 0 so tau == 0
			//! is bit-identical and continuous; reciprocity gives escape == entry / eta^2).
			struct InteriorDiffuse
			{
				Scalar returned;		//!< E_ret = INT_0^1 2 mu a(mu)^2 F_in(mu) dmu (recycling factor)
				Scalar escape;			//!< INT_0^1 2 mu a(mu) (1 - F_in(mu)) dmu == entry / eta^2
				Scalar entry;			//!< INT_0^1 2 c (1 - F(c)) a(mu(c)) dc (white-sky entry)
			};

			inline InteriorDiffuse InteriorDiffuseTransport(
				const Scalar eta,
				const Scalar tau,
				const Scalar re,			//!< ExternalDiffuseFresnel( eta )
				const Scalar ri				//!< InternalDiffuseFresnel( eta )
				)
			{
				InteriorDiffuse out;
				if( !( tau > Scalar(0) ) ) {
					out.returned = ri;
					out.escape   = Scalar(1) - ri;
					out.entry    = Scalar(1) - re;
					return out;
				}
				if( !( tau < std::numeric_limits<Scalar>::infinity() ) ) {
					out.returned = Scalar(0);
					out.escape   = Scalar(0);
					out.entry    = Scalar(0);
					return out;
				}

				const std::array<Scalar, 21>& nodes = Detail::GLNodes();
				const std::array<Scalar, 21>& wts   = Detail::GLWeights();
				const Scalar invEta2 = ( eta > Scalar(1) ) ? Scalar(1) / ( eta * eta ) : Scalar(1);
				const Scalar muC2 = ( eta > Scalar(1) ) ? ( Scalar(1) - invEta2 ) : Scalar(0);
				const Scalar muC  = sqrt( r_max( muC2, Scalar(0) ) );

				Scalar dTIR = 0;		// INT_0^muC 2 mu (1 - a^2) dmu
				Scalar dRefl = 0;		// INT_0^1 2 c F(c) (1 - a^2) dc   (times 1/eta^2 below)
				Scalar dEntry = 0;		// INT_0^1 2 c (1 - F(c)) (1 - a) dc
				for( std::size_t i = 0; i < nodes.size(); ++i )
				{
					const Scalar x = nodes[i];
					const Scalar w = wts[i];

					if( muC > Scalar(0) ) {
						const Scalar mu = x * muC;
						dTIR += w * muC * Scalar(2) * mu * ( -expm1( Scalar(-2) * tau / mu ) );
					}

					const Scalar c  = x;
					const Scalar mu = sqrt( r_max( Scalar(1) - ( Scalar(1) - c * c ) * invEta2, Scalar(1e-300) ) );
					const Scalar F  = Fresnel( c, eta );
					const Scalar oneMinusA  = -expm1( -tau / mu );
					const Scalar oneMinusA2 = -expm1( Scalar(-2) * tau / mu );
					dRefl  += w * Scalar(2) * c * F * oneMinusA2;
					dEntry += w * Scalar(2) * c * ( Scalar(1) - F ) * oneMinusA;
				}

				out.returned = r_max( Scalar(0), ri - dTIR - dRefl * invEta2 );
				out.entry    = r_max( Scalar(0), ( Scalar(1) - re ) - dEntry );
				out.escape   = out.entry * invEta2;
				return out;
			}

			//! 4-entry per-thread cache over InteriorDiffuseTransport keyed on (eta, tau).
			inline InteriorDiffuse InteriorDiffuseTransportCached(
				const Scalar eta,
				const Scalar tau,
				const Scalar re,
				const Scalar ri
				)
			{
				if( !( tau > Scalar(0) ) ) {
					return InteriorDiffuseTransport( eta, tau, re, ri );
				}
				struct Entry { Scalar eta; Scalar tau; InteriorDiffuse value; bool valid; };
				static thread_local Entry cache[4] = {};
				static thread_local unsigned int next = 0;
				for( unsigned int i = 0; i < 4; ++i ) {
					if( cache[i].valid && cache[i].eta == eta && cache[i].tau == tau ) {
						return cache[i].value;
					}
				}
				const InteriorDiffuse v = InteriorDiffuseTransport( eta, tau, re, ri );
				Entry& e = cache[next];
				next = ( next + 1 ) & 3u;
				e.eta = eta;
				e.tau = tau;
				e.value = v;
				e.valid = true;
				return v;
			}

			//! Saunderson recycling factor 1 / (1 - returned * R * roundTrip),
			//! clamped against substrate albedos slightly > 1.
			inline Scalar Recycling( const Scalar returned, Scalar R, const Scalar roundTrip )
			{
				R = r_min( r_max( R, Scalar(0) ), Scalar(1) );
				const Scalar denom = Scalar(1) - returned * R * roundTrip;
				return ( denom > Scalar(1e-6) ) ? ( Scalar(1) / denom ) : Scalar(1e6);
			}

			//! Per-channel RGB recycling (produces wet chroma boost).
			inline RISEPel RecyclingRGB( const RISEPel& returned, const RISEPel& R )
			{
				return RISEPel(
					Recycling( returned[0], R[0], Scalar(1) ),
					Recycling( returned[1], R[1], Scalar(1) ),
					Recycling( returned[2], R[2], Scalar(1) ) );
			}
		}
	}
}

#endif
