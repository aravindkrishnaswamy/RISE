//////////////////////////////////////////////////////////////////////
//
//  CoatedLayer.h - Shared closed-form layer math for the
//    `coated_material` triad (CoatedBRDF / CoatedSPF / CoatedMaterial).
//
//  Implements docs/WETNESS_COAT_DESIGN.md 7.4: a Weidlich-Wilkie
//  analytic core PLUS the REQUIRED coat<->substrate interreflection
//  compensation.  The compensation is NOT a Kulla-Conty LUT lookup --
//  7.4 is explicit that the quantity needed is the dielectric
//  interface's INTERNAL hemispherical reflectance `r_i`, which is
//  closed-form and needs no table.  (`MicrofacetEnergyLUT` stays
//  relevant to this material for the separate purpose of the coat
//  lobe's OWN multiple-scattering term; that lives in CoatedBRDF.)
//
//  The layer response this file supplies is the Saunderson /
//  Lekner-Dorf rational form of 2.1:
//
//     f_sub_through_coat(wi,wo)
//        =  T(cos_i) * T(cos_o) * A_in * A_out
//           ------------------------------------- * f_base(wi,wo)
//              eta^2 * ( 1 - E_ret * R )
//
//  where T(cos) = 1 - F(cos, eta) is the interface transmittance,
//  A_* are the coat's Beer-Lambert pass transmittances, R is the
//  substrate's directional-hemispherical albedo, and E_ret is the
//  fraction of the trapped diffuse field one round trip returns to the
//  substrate: r_i, the internal diffuse Fresnel reflectance, for a
//  clear coat, and its exact path-length average through an absorbing
//  one (InteriorDiffuseTransport below, DL-342).  The `1/(1 - E_ret R)`
//  factor IS the recycling series of 7.4; the `1/eta^2` is the radiance
//  compression on exit.  Same shape as Mitsuba's `plastic` /
//  `roughplastic` `nonlinear` mode and PBRT's coated diffuse; for a
//  smooth coat over a Lambertian it is EXACT, absorbing or not.
//
//  WHY THIS EXACTLY CONSERVES ENERGY (the 7.6 furnace gate).
//  For a Lambertian substrate of albedo R, the hemispherical integral
//  of the expression above is
//
//     (1 - F(cos_i)) * R * (1 - r_e) / ( eta^2 * (1 - r_i R) )
//
//  and the smooth-interface identity  (1 - r_e) / eta^2 == (1 - r_i)
//  collapses it to  (1 - F(cos_i)) * R * (1 - r_i) / (1 - r_i R),
//  which at R = 1 is exactly (1 - F(cos_i)) -- the complement of the
//  coat lobe.  rho == 1.  Drop the `1/(1 - r_i R)` factor (plain
//  Weidlich-Wilkie) and the same integral becomes
//  (1 - F) * (1 - r_i) ~= 0.53 at eta = 1.33: the ~45 % loss 7.4
//  predicts.  tests/LayeredWhiteFurnaceTest.cpp configs 11-15 measure
//  exactly this, and CoatedBRDF's `recyclingCompensation` constructor
//  flag (default TRUE; not reachable from the scene language)
//  reproduces the failing half on demand.
//
//  BECAUSE that identity is load-bearing, `r_i` here is DERIVED from
//  `r_e` rather than read from a published polynomial fit:
//
//     r_i(eta) = 1 - (1 - r_e(eta)) / eta^2
//
//  with r_e(eta) = 2 * integral_0^1 F(mu, eta) mu dmu evaluated by
//  21-point Gauss-Legendre against THIS FILE'S OWN `Fresnel`.  The
//  identity then holds to quadrature + table-interpolation accuracy
//  (< 1e-5) instead of to the ~1 % of a fit, so the furnace passes on
//  physics rather than on tolerance.  Cross-check: the derived
//  r_i(1.33) = 0.471949 and r_i(1.5) = 0.596346 match the classical
//  Egan-Hilgeman / d'Eon values (0.47, 0.596).  Note 0.471949 sits
//  JUST ABOVE the top of 2.1's quoted "r_i ~ 0.44-0.47" band rather
//  than inside it -- by 0.002, which is the rounding in the doc's own
//  figure, not a disagreement.
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
#include <array>
#include <cmath>
#include <limits>

namespace RISE
{
	namespace Implementation
	{
		//! Closed-form dielectric-layer quantities shared by CoatedBRDF
		//! and CoatedSPF.  Free functions, no state, no allocation.
		namespace CoatedLayer
		{
			//! Smallest coat GGX alpha the material will use.  The coat
			//! lobe is therefore NEVER a delta distribution, which is a
			//! deliberate design decision (docs/WETNESS_COAT_DESIGN.md
			//! 7.2 ranges coat_roughness at 0.01-0.05 for water and
			//! clearcoat sits at 0.03-0.1, so nothing in the intended
			//! parameter space wants a delta coat) with a correctness
			//! payoff: Phase 2 exists so NEE / BDPT / VCM connections can
			//! SEE the coated surface (7.1), and a delta lobe is
			//! invisible to every one of those strategies.  Consequence:
			//! CoatedMaterial does not override GetSpecularInfo -- see
			//! the note in CoatedMaterial.h.
			constexpr Scalar kMinCoatAlpha = Scalar( 1e-3 );

			//! Largest relative IOR the layer math is tabulated for.
			//! Covers every dielectric coat anyone authors (water 1.33,
			//! varnish 1.5, oil 1.4-1.6); a higher `coat_ior` is clamped
			//! here rather than extrapolated off the end of the table.
			//! Mirrored from Detail::kEtaMax so callers never have to
			//! reach into Detail.
			inline Scalar MaxRelativeIOR();

			//! Unpolarised Fresnel reflectance of a smooth dielectric
			//! interface.
			//! @param cosThetaI  |cos| of the incidence angle measured
			//!                   in the OUTER medium.
			//! @param eta        relative IOR, n_coat / n_outer.
			inline Scalar Fresnel( Scalar cosThetaI, const Scalar eta )
			{
				if( eta <= Scalar(0) ) {
					return Scalar(0);
				}
				// eta == 1 is NO INTERFACE: index-matched media reflect
				// nothing at any angle, so F == 0 identically.  Handled
				// before the sin2T test below because that test would
				// otherwise take the total-internal-reflection branch
				// at exactly grazing (cos = 0 gives sin2T = 1) and
				// return F = 1 -- the opposite of the correct limit.
				// Reachable: CoatedBRDF clamps eta to >= 1, so a
				// coat_ior equal to the ambient IOR lands exactly here.
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

			//! Cosine of the refracted angle INSIDE the coat, for a ray
			//! arriving at `cosThetaI` in the outer medium.  Returns 0
			//! under total internal reflection.
			inline Scalar CosRefracted( Scalar cosThetaI, const Scalar eta )
			{
				if( eta <= Scalar(0) ) {
					return Scalar(1);
				}
				if( eta == Scalar(1) ) {
					// No interface: the ray is undeviated, so the
					// "refracted" cosine is the incident one.  Same
					// edge as Fresnel() above; without it, grazing
					// would report cos_t = 0 and blow the obliquity
					// factor 1/cos_t up to its 1e4 clamp.
					return r_min( r_max( cosThetaI, Scalar(0) ), Scalar(1) );
				}
				cosThetaI = r_min( r_max( cosThetaI, Scalar(0) ), Scalar(1) );
				const Scalar sin2T = ( Scalar(1) - cosThetaI * cosThetaI ) / ( eta * eta );
				if( sin2T >= Scalar(1) ) {
					return Scalar(0);
				}
				return sqrt( r_max( Scalar(0), Scalar(1) - sin2T ) );
			}

			namespace Detail
			{
				//! eta domain of the cached r_e table.  1.0 (no
				//! interface) through 3.0 covers every dielectric coat
				//! anyone authors; outside, the endpoints are clamped.
				static const Scalar kEtaMin   = Scalar( 1.0 );
				static const Scalar kEtaMax   = Scalar( 3.0 );
				static const int    kEtaCount = 201;			// 0.01 steps

				//! 21-point Gauss-Legendre nodes/weights on [0,1].
				//! Same set MicrofacetEnergyLUT uses for its conductor
				//! Fresnel average; duplicated here (rather than
				//! included) so this header stays free of the 32x32
				//! GGX energy tables it would otherwise drag in.
				inline const std::array<Scalar, 21>& GLNodes()
				{
					static const std::array<Scalar, 21> n = { {
						0.0031239146898053, 0.0163865807168468, 0.0399503329247996, 0.0733183177083414,
						0.1157800182621611, 0.1664305979012938, 0.2241905820563901, 0.2878289398962806,
						0.3559893415987995, 0.4272190729195525, 0.5000000000000000, 0.5727809270804476,
						0.6440106584012005, 0.7121710601037194, 0.7758094179436099, 0.8335694020987061,
						0.8842199817378389, 0.9266816822916586, 0.9600496670752003, 0.9836134192831532,
						0.9968760853101948
					} };
					return n;
				}

				inline const std::array<Scalar, 21>& GLWeights()
				{
					static const std::array<Scalar, 21> w = { {
						0.0080086141288871, 0.0184768948854263, 0.0285672127134286, 0.0380500568141897,
						0.0467222117280170, 0.0543986495835742, 0.0609157080268643, 0.0661344693166687,
						0.0699436973955366, 0.0722622394525271, 0.0730405654382176, 0.0722622394525271,
						0.0699436973955366, 0.0661344693166687, 0.0609157080268643, 0.0543986495835742,
						0.0467222117280170, 0.0380500568141897, 0.0285672127134286, 0.0184768948854263,
						0.0080086141288871
					} };
					return w;
				}

				//! r_e(eta) by direct quadrature (table build only).
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

				//! Lazily-built, immutable r_e table.  C++11 magic
				//! statics make the initialisation thread-safe and
				//! once-only -- same pattern as
				//! MicrofacetEnergyLUT::MSLobeZ's per-alpha row cache.
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

			//! Cosine-weighted hemispherical average of the EXTERNAL
			//! Fresnel reflectance at an outer->coat interface:
			//!   r_e(eta) = 2 * integral_0^1 F(mu, eta) mu dmu.
			//! Table lookup with linear interpolation; error < 1e-5
			//! against the quadrature it was built from.
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

			//! Hemispherically averaged INTERNAL reflectance at the
			//! coat->outer boundary -- 7.4's `r_i`, the recycling
			//! coefficient and "the single most important number in
			//! this document" (2.1).
			//!
			//! DERIVED from r_e via the exact smooth-interface identity
			//!   (1 - r_e) / eta^2 == 1 - r_i
			//! rather than read from a fit, so the furnace identity in
			//! this file's header holds numerically.  For eta = 1.33
			//! this returns 0.4712 (2.1 quotes 0.44-0.47, of which the
			//! pure-TIR fraction 1 - 1/n^2 alone is 0.435).
			inline Scalar InternalDiffuseFresnel( const Scalar eta )
			{
				if( eta <= Scalar(1) ) {
					return Scalar(0);
				}
				const Scalar ri = Scalar(1) - ( Scalar(1) - ExternalDiffuseFresnel( eta ) ) / ( eta * eta );
				return r_min( r_max( ri, Scalar(0) ), Scalar( 0.999 ) );
			}

			//! Beer-Lambert transmittance for ONE traversal of the coat
			//! film by a ray arriving at `cosThetaI` in the outer medium.
			//!
			//! Two independent, separately-neutral terms:
			//!   - `absorption` (1/length) x `thickness` is the grey
			//!     optical depth for one NORMAL-incidence traversal;
			//!   - `tint` is the colour transmitted by one NORMAL-
			//!     incidence traversal, applied only when `applyTint`.
			//! Both are raised to the obliquity factor 1/cos(theta_t),
			//! so an oblique path darkens correctly.  At the shipping
			//! defaults (absorption 0, thickness 0, untinted) the result
			//! is exactly 1 and the whole term is a no-op.
			//!
			//! WHY `applyTint` IS A FLAG AND NOT `tint < 1`.
			//! An earlier revision decided "is this coat tinted?" by
			//! comparing the sampled value against 1.  That is sound on
			//! the RGB pipe and WRONG on the spectral one, because a
			//! white `uniformcolor_painter` does NOT sample to exactly
			//! 1.0 at every wavelength: `GetColorNM` runs the
			//! Jakob-Hanika uplift, whose sigmoid reaches white only
			//! asymptotically.
			//!
			//! HISTORICAL (pre-Stage-C, i.e. LUT trained under a flat E
			//! illuminant; superseded 2026-09-02 by
			//! docs/SPECTRAL_ILLUMINANT_CONVENTION.md): white was then
			//! not representable AT ALL and collapsed off the red end --
			//! MEASURED on that tree at 5 nm steps over 380-780 nm:
			//! 620 nm -> 0.99998, 640 nm -> 0.5189, 660 nm -> 1.28e-5,
			//! 780 nm -> 1.43e-7 (min 1.4e-7, mean |1-v| = 0.35).  Under
			//! the Stage C D65-referenced LUT the same samples are
			//! >= 0.99999 (660 nm -> 0.9999997).  The collapse is gone;
			//! the FLAG STAYS, because a multiplicative slot at authored
			//! white must be a BIT-EXACT no-op so the NM path matches the
			//! RGB path, and 1 - epsilon is not 1.
			//!
			//! Under the old test AND the old LUT, an UNTINTED coat took
			//! the tint branch at every wavelength and multiplied in
			//! pow(1.3e-5, 1/cos) ~ 0 above 660 nm -- i.e. the default
			//! coat went opaque across the red end of every spectral
			//! render, silently, while the RGB render stayed perfect.
			//! No epsilon rescued that: 1.3e-5 is nowhere near 1.
			//!
			//! "Is this coat tinted at all?" is a property of the
			//! AUTHORED COLOUR, not of a wavelength, so CoatedBRDF
			//! decides it once from the un-uplifted RGB triple and
			//! passes the answer down.  When the coat genuinely IS
			//! tinted, `GetColorNM` is then the correct per-wavelength
			//! magnitude and its red-end behaviour is the engine's
			//! documented treatment of a colour the author asked for.
			//!
			//! KNOWN RESIDUAL (HISTORICAL -- LARGELY CLOSED BY STAGE C),
			//! AND A NORMALIZATION THAT DID NOT FIX IT.
			//! The flag leaves a DISCONTINUITY at the near-white
			//! boundary: an authored tint of exactly 1 is clear, while
			//! 0.99 takes the tint branch.  Pre-Stage-C that branch went
			//! opaque in the red and a textured tint straddling white
			//! showed a per-pixel edge on the spectral pipe.  Post
			//! Stage C the tint branch samples ~the authored value at
			//! every wavelength (0.95 grey -> 0.9501 at 660 nm, was
			//! 0.973 under flat-E training; 0.5 grey -> 0.50005, was
			//! 0.36), so the step across the boundary is now ~1e-5
			//! instead of catastrophic.  The paragraphs below record why
			//! the divide-by-white repair was rejected at the time; they
			//! describe the OLD LUT and no longer apply numerically.
			//!
			//! The obvious repair -- divide the tint's uplifted spectrum
			//! by WHITE's, so untinted is 1 by construction and "the
			//! shared gamut-edge shape divides out" -- was implemented
			//! as a measurement and REFUTED.  The shape is not shared:
			//! the collapse is specific to the degenerate white corner,
			//! and a 0.95 grey does not collapse at all (0.973 at
			//! 660 nm).  So the divisor is ~0 exactly where the
			//! numerator is not, and the ratio explodes.  MEASURED
			//! tintNM/whiteNM at 660 nm / 780 nm:
			//!     white     1.0000    / 1.0000     (exact, as designed)
			//!     0.99 grey 1.53      / 1.64
			//!     0.95 grey 7.6e+04   / 2.7e+04
			//!     amber     5.7e+04   / 2.5e+06
			//! Clamping the ratio to [0,1] tames the blow-up but buys
			//! the near-white boundary at the cost of every SATURATED
			//! tint, which then clamps to 1.0 across the whole red end
			//! and stops tinting there -- a worse trade, since a tinted
			//! lacquer is the slot's actual use case.
			//!
			//! The residual was upstream of this material: the JH ALBEDO
			//! uplift is a reflectance round-trip (it preserves the
			//! CIE-integrated colour, not spectral flatness -- under
			//! flat-E training a 0.5 grey uplifted to 0.36 at 660 nm),
			//! and `coat_tint` is used as a per-wavelength
			//! multiplicative TRANSMITTANCE.  Stage C fixed it upstream
			//! exactly as that diagnosis predicted: with the reference
			//! illuminant in the LUT's forward model, a neutral grey
			//! uplifts FLAT, so a grey tint is now a faithful grey
			//! transmittance.  A saturated tint is still a reflectance-
			//! shaped spectrum used as a transmittance -- the design
			//! question for docs/WETNESS_COAT_DESIGN.md 7.2 -- but the
			//! near-white artifact this paragraph was written about is
			//! closed.  The DEFAULT (untinted) is exactly correct via
			//! the flag, as always.
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

			//! NORMAL-INCIDENCE optical depth of ONE traversal of the
			//! coat film, for one channel: the exponent PassTransmittance
			//! raises to the obliquity 1/cos(theta_t), i.e.
			//!   PassTransmittance(...) == exp( -tau / cos(theta_t) )
			//! with  tau = absorption * thickness - ln(tint)  (the tint
			//! term only when `applyTint`), clamped at 0 exactly where
			//! PassTransmittance clamps its result at 1.  +infinity for a
			//! zero tint (an opaque coat).  Exactly 0 at the shipping
			//! defaults, which is what keeps every non-absorbing result
			//! on the pre-DL-342 code path.
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

			//! Hemispherical transport of the DIFFUSE field trapped
			//! inside an absorbing coat (DL-342).  Three numbers, all
			//! exact for a smooth interface over a Lambertian substrate:
			//!
			//!   returned  E_ret = INT_0^1 2 mu a(mu)^2 F_in(mu) dmu
			//!             the fraction of a cosine-distributed upwelling
			//!             field that comes back to the substrate after one
			//!             round trip (up the film, Fresnel/TIR at the
			//!             underside, down the film);
			//!   escape    INT_0^1 2 mu a(mu) (1 - F_in(mu)) dmu
			//!             the fraction of the same field that leaves;
			//!   entry     INT_0^1 2 c (1 - F(c)) a(mu(c)) dc
			//!             the fraction of a uniform OUTSIDE field that
			//!             reaches the substrate (white-sky entry);
			//!
			//! with a(mu) = exp(-tau/mu) one traversal at the INTERNAL
			//! cosine mu.  Reciprocity makes escape == entry / eta^2.
			//!
			//! WHY NOT ONE BEER FACTOR.  The pre-DL-342 model wrote
			//! E_ret = r_i * a(mu_bar)^2 with mu_bar the refraction of an
			//! outer cosine 0.5.  But the trapped field's reflected part
			//! is DOMINATED by total internal reflection, which happens
			//! exactly at the internal cosines below the critical one
			//! (mu < sqrt(1 - 1/eta^2), 0.745 at eta 1.5) -- the
			//! LONGEST paths, which absorb most.  E_ret is therefore far
			//! below r_i * a(mu_bar)^2: 0.2583 against 0.3654 at eta 1.5,
			//! tau 0.2.  No single mean cosine fixes that, because the
			//! weighting by F_in is what moves the answer.
			//!
			//! QUADRATURE.  Split at the critical cosine.  Below it F_in
			//! is 1 and the integrand is mu exp(-2 tau/mu); above it the
			//! substitution mu = mu(c) (c the OUTER cosine Snell pairs
			//! with mu, mu dmu = c dc / eta^2) turns F_in(mu) into the
			//! external F(c) and removes the square-root kink at the
			//! critical angle, so both pieces are smooth and 21-point
			//! Gauss-Legendre on each is accurate to ~1e-6 for every
			//! eta in [1, 3] and tau in [0, 10] (measured against a
			//! 4e5-point midpoint rule).  Each piece is evaluated as a
			//! DEFICIT against its tau = 0 value (r_i and 1 - r_e, which
			//! the rest of this file already holds), so the result is
			//! continuous into tau = 0 instead of jumping by the
			//! quadrature's own error, and tau == 0 returns those values
			//! untouched (bit-identical to the pre-DL-342 model).
			struct InteriorDiffuse
			{
				Scalar returned;		//!< E_ret, multiplies R in the recycling denominator
				Scalar escape;			//!< hemispherical exit fraction of the trapped field
				Scalar entry;			//!< white-sky entry fraction to the substrate
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

			//! 7.4's recycling factor  1 / ( 1 - E_ret * R ).
			//! `R` is the substrate's directional-hemispherical albedo
			//! and `returned` the round trip's returned fraction --
			//! r_i for a clear coat, InteriorDiffuse::returned for an
			//! absorbing one (DL-342).  `roundTrip` multiplies
			//! `returned` and is 1 at every in-tree call site; it is
			//! kept so callers holding the clear-coat form
			//! (r_i, round trip 1) read unchanged.  Clamped so a
			//! substrate reporting an albedo slightly over unity
			//! (GGXBRDF::albedo is an estimate, not a bound) cannot
			//! drive the denominator to zero.
			inline Scalar Recycling( const Scalar returned, Scalar R, const Scalar roundTrip )
			{
				R = r_min( r_max( R, Scalar(0) ), Scalar(1) );
				const Scalar denom = Scalar(1) - returned * R * roundTrip;
				return ( denom > Scalar(1e-6) ) ? ( Scalar(1) / denom ) : Scalar(1e6);
			}

			//! RGB flavour: the per-channel form is what produces the
			//! wet CHROMA BOOST (2.1) -- the high-albedo channels are
			//! amplified more than the dark ones, which is precisely
			//! what a single grey recycling factor would destroy.
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
