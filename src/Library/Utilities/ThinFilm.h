//////////////////////////////////////////////////////////////////////
//
//  ThinFilm.h - Production thin-film interference reflectance for the
//    GGX microfacet conductor (heat-tint / anodization color).  This is
//    the lifted, renderer-side evaluator of the Phase-1 validated
//    single-film Airy oracle (tests/thinfilm/AiryReference.h) and the
//    N-layer characteristic-matrix TMM (tests/thinfilm/TmmReference.h),
//    re-expressed in RISE's Scalar (double) and std::complex<Scalar>.
//
//    Header-only, inline, allocation-free.  Called per-shade per-hero-
//    wavelength on the spectral path (see docs/THIN_FILM_INTERFERENCE.md
//    §7), so the hot path performs no heap allocation and a minimal
//    number of transcendentals.
//
//    The math is N-layer-capable internally (a fixed-capacity,
//    stack-allocated characteristic-matrix product), but the shipped
//    single-film entry point ThinFilm::ReflectanceConductor() serves the
//    air / oxide / metal stack, which is the Phase-2 material.  The Airy
//    closed form is used for the single-film case (one transcendental
//    complex exp), matching exactly what was validated in Phase 1.
//
//    PHYSICS / SIGN CONVENTION (docs/THIN_FILM_INTERFERENCE.md §5, as
//    corrected and validated by the Phase-1 reference -- reproduced here
//    EXACTLY so this code is bit-faithful to the oracle):
//
//      * Complex index  N = n + i*k,  k >= 0 for an absorbing medium.
//      * Time dependence e^{-iωt} (Born & Wolf / Macleod).
//      * cosθ in each medium is the FORWARD-TRAVELLING root:
//        Re(N cosθ) > 0, tie-broken by Im(N cosθ) > 0.  At normal
//        incidence this keeps cosθ = +1 even for an absorbing medium;
//        a naive "Im(N cosθ) >= 0 else negate" rule wrongly flips it.
//      * Per-polarization tilted admittance:  η_s = N cosθ ;  η_p = N/cosθ.
//      * Phase thickness  δ = (2π/λ) N d cosθ  (complex if the film
//        absorbs); the forward wave decays, Im(δ) >= 0.
//      * Single-film Airy:
//          r = ( r01 + r1s e^{+2iδ1} ) / ( 1 + r01 r1s e^{+2iδ1} )
//        with r_ab = (η_a - η_b)/(η_a + η_b).  The e^{+2iδ1} (NOT
//        e^{-2iδ1}) sign is the one that makes a thick absorbing film
//        decay to the bare top-interface reflectance r01.  Pairing the
//        opposite-sign factor with this forward-root branch produces a
//        GROWING wave and R >> 1 for absorbing films -- the bug the
//        Phase-1 Airy<->TMM cross-check is built to catch.
//      * N-layer characteristic (Abelès) matrix, e^{+iδ}-forward form:
//          Mj = [[ cosδj, -i sinδj/ηj ], [ -i ηj sinδj, cosδj ]]
//
//    UNITS:  wavelength and film thickness are both in nanometres (nm);
//    only their ratio enters δ, so any consistent length unit works, but
//    nm is the documented choice.
//
//    cosThetaI CONVENTION (the single most important call-site fact):
//    the input cosine is the cosine of the angle between the INCIDENT
//    direction and the MICROFACET HALF-VECTOR (the local micronormal
//    that GGX importance-samples), in [0,1].  It is NOT the cosine to the
//    geometric surface normal.  Thin-film interference is a property of
//    the local interface, whose normal is the half-vector; the GGX
//    conductor Fresnel it replaces is evaluated at the same dot(wi, h).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef THIN_FILM_
#define THIN_FILM_

#include <complex>
#include <cmath>

#include "Math3D/Math3D.h"		// RISE Scalar (double)
#include "Color/Color.h"		// RISEPel / XYZPel (RGB-path albedo basis)
#include "Color/ColorUtils.h"	// ColorUtils::XYZFromNM (renderer CMFs)
#include "MicrofacetEnergyLUT.h"	// GL_N / GL_nodes / GL_weights (shared Kulla-Conty F_avg quadrature)

namespace RISE
{
	namespace ThinFilm
	{
		//! Complex optical index / amplitude type used throughout, in
		//! RISE's Scalar precision (double).
		typedef std::complex<Scalar> Complex;

		//! Polarization selector.  Matches the Phase-1 reference.
		enum Polarization
		{
			ePolS = 0,		//!< s-polarized (TE): E perpendicular to plane of incidence
			ePolP = 1		//!< p-polarized (TM): E in the plane of incidence
		};

		//! The maximum number of films the stack-allocated N-layer path
		//! supports without heap.  Single film is the shipped material;
		//! this headroom keeps the math N-layer-capable at zero cost for
		//! the documented multi-oxide extension point (§3 generality).
		static const int kMaxFilms = 8;

		namespace detail
		{
			//! Builds a complex index from real n and extinction k, forcing
			//! k non-negative (the absorbing convention N = n + i|k|).
			inline Complex MakeIndex( Scalar n, Scalar k )
			{
				return Complex( n, k >= Scalar(0) ? k : -k );
			}

			//! Picks the FORWARD-TRAVELLING cosθ root.  A wave is forward if
			//! its tilted admittance η = N cosθ carries energy into the
			//! medium: Re(N cosθ) > 0, or (Re == 0 and Im(N cosθ) > 0) for a
			//! purely evanescent wave.  Otherwise the other root is taken.
			//! (Byrnes' `tmm` is_forward_angle / Born & Wolf / Macleod.)
			inline Complex PickForwardCos( const Complex& N, const Complex& cosCandidate )
			{
				const Complex eta = N * cosCandidate;
				const bool forward =
					( eta.real() > Scalar(0) ) ||
					( eta.real() == Scalar(0) && eta.imag() > Scalar(0) );
				return forward ? cosCandidate : -cosCandidate;
			}

			//! cosθ in a medium of index N given the Snell invariant
			//! s = N0 sinθ0 (preserved across all interfaces).  Returns the
			//! forward-travelling branch.   sinθ = s/N ; cosθ = sqrt(1-sin²θ).
			inline Complex CosThetaInMedium( const Complex& N, const Complex& sinTheta0TimesN0 )
			{
				const Complex sinTheta = sinTheta0TimesN0 / N;
				const Complex cos2 = Complex( Scalar(1), Scalar(0) ) - sinTheta * sinTheta;
				const Complex root = std::sqrt( cos2 );		// principal branch
				return PickForwardCos( N, root );
			}

			//! Tilted optical admittance for the chosen polarization.
			//!   η_s = N cosθ ;  η_p = N / cosθ
			inline Complex Admittance( const Complex& N, const Complex& cosTheta, Polarization pol )
			{
				if( pol == ePolS ) {
					return N * cosTheta;
				}
				return N / cosTheta;
			}

			//! The Snell invariant s = N0 sinθ0 from the ambient index and
			//! the incidence cosine.  The ambient is taken real for the
			//! shipped air/oxide/metal stack, but the math is uniform in
			//! Complex.  sinθ0 is recovered as sqrt(1 - cos²) (avoids an
			//! acos/sin round-trip on the hot path).
			//!
			//! cosThetaI is clamped to [kGrazingCosFloor, 1].  The lower
			//! bound is a small POSITIVE floor, not 0: at EXACTLY grazing
			//! (cosθ = 0) every medium's cosθ is 0, so the p-polarization
			//! admittance η_p = N/cosθ is infinite and the Fresnel
			//! coefficient r_ab = (η_a-η_b)/(η_a+η_b) evaluates to
			//! Inf/Inf = NaN.  That is the documented θ = 90° degeneracy of
			//! the oracle (TmmReference.h header), a non-physical input the
			//! renderer never produces (an edge-on microfacet has zero
			//! projected area and is rejected upstream).  Clamping the cosine
			//! to a small positive floor keeps cosθ nonzero in every medium,
			//! so the hot path returns the correct well-conditioned grazing
			//! LIMIT (R -> 1) instead of NaN, even if a caller passes cosθ=0.
			//!
			//! The floor is 1e-6, NOT NEARZERO (1e-12): the invariant is
			//! built from sqrt(1 - cos²), and for cos <~ sqrt(eps_double)
			//! (~1.5e-8) the term 1 - cos² rounds to exactly 1.0, collapsing
			//! sinθ0 to 1 and cosθ in the ambient back to 0 -- the very NaN
			//! we are avoiding.  1e-6 is comfortably above that
			//! representability threshold (so 1 - cos² stays < 1) and is
			//! still ~1 microradian from grazing, where R is already ~1.
			static const Scalar kGrazingCosFloor = Scalar(1e-6);

			inline Complex SnellInvariant( const Complex& N0, Scalar cosThetaI )
			{
				Scalar c = cosThetaI;
				if( c < kGrazingCosFloor ) c = kGrazingCosFloor;
				if( c > Scalar(1) )        c = Scalar(1);
				const Scalar s0 = std::sqrt( Scalar(1) - c * c );
				return N0 * s0;
			}

			//! Single-film Airy reflectance for one polarization.  This is
			//! the closed form lifted from AiryReference.h, the partner of
			//! the N-layer TMM (they agree to ~machine epsilon by design).
			inline Scalar AiryReflectanceForPol(
				const Complex& N0, const Complex& N1, const Complex& Ns,
				Scalar thickness_nm, Scalar lambda_nm,
				const Complex& sinInvariant,
				Polarization pol )
			{
				const Complex cos0 = CosThetaInMedium( N0, sinInvariant );
				const Complex cos1 = CosThetaInMedium( N1, sinInvariant );
				const Complex cosS = CosThetaInMedium( Ns, sinInvariant );

				const Complex eta0 = Admittance( N0, cos0, pol );
				const Complex eta1 = Admittance( N1, cos1, pol );
				const Complex etaS = Admittance( Ns, cosS, pol );

				// Fresnel amplitude reflection coefficients at each interface,
				// in admittance form r_ab = (η_a - η_b)/(η_a + η_b).
				const Complex r01 = ( eta0 - eta1 ) / ( eta0 + eta1 );
				const Complex r1s = ( eta1 - etaS ) / ( eta1 + etaS );

				// Phase thickness δ1 = (2π/λ) N1 d1 cosθ1.
				const Complex delta = Complex( TWO_PI * thickness_nm / lambda_nm, Scalar(0) ) * N1 * cos1;

				// e^{+2iδ1}: forward-decaying round-trip phase (see header).
				const Complex i( Scalar(0), Scalar(1) );
				const Complex phase = std::exp( Scalar(2) * i * delta );

				const Complex r = ( r01 + r1s * phase ) /
					( Complex( Scalar(1), Scalar(0) ) + r01 * r1s * phase );
				return std::norm( r );		// |r|²
			}

			//! N-layer characteristic-matrix reflectance for one
			//! polarization.  Allocation-free: the film indices/thicknesses
			//! are passed as fixed-capacity C arrays.  With nFilms == 1 this
			//! reduces algebraically to AiryReflectanceForPol (Phase-1
			//! cross-check).  Provided so the single tested evaluator stays
			//! N-layer-capable at zero cost for the multi-oxide extension.
			inline Scalar TmmReflectanceForPol(
				const Complex& N0,
				const Complex* filmIndex, const Scalar* filmThickness_nm, int nFilms,
				const Complex& Ns,
				Scalar lambda_nm,
				const Complex& sinInvariant,
				Polarization pol )
			{
				const Complex cos0 = CosThetaInMedium( N0, sinInvariant );
				const Complex eta0 = Admittance( N0, cos0, pol );

				// Characteristic matrix M = Π Mj, ambient -> substrate order.
				Complex m00( Scalar(1), Scalar(0) ), m01( Scalar(0), Scalar(0) );
				Complex m10( Scalar(0), Scalar(0) ), m11( Scalar(1), Scalar(0) );

				for( int j = 0; j < nFilms; ++j ) {
					const Complex Nj = filmIndex[j];
					const Scalar  dj = filmThickness_nm[j];

					const Complex cosj = CosThetaInMedium( Nj, sinInvariant );
					const Complex etaj = Admittance( Nj, cosj, pol );

					const Complex delta = Complex( TWO_PI * dj / lambda_nm, Scalar(0) ) * Nj * cosj;

					const Complex cosD = std::cos( delta );
					const Complex sinD = std::sin( delta );
					const Complex negI( Scalar(0), Scalar(-1) );

					// Mj = [[ cosδ, -i sinδ/η ], [ -i η sinδ, cosδ ]]
					const Complex a00 = cosD;
					const Complex a01 = negI * sinD / etaj;
					const Complex a10 = negI * etaj * sinD;
					const Complex a11 = cosD;

					// M <- M * Mj
					const Complex n00 = m00 * a00 + m01 * a10;
					const Complex n01 = m00 * a01 + m01 * a11;
					const Complex n10 = m10 * a00 + m11 * a10;
					const Complex n11 = m10 * a01 + m11 * a11;

					m00 = n00; m01 = n01;
					m10 = n10; m11 = n11;
				}

				const Complex cosS = CosThetaInMedium( Ns, sinInvariant );
				const Complex etaS = Admittance( Ns, cosS, pol );

				// [B; C] = M * [1; η_s] ;  Y = C/B.
				const Complex B = m00 * Complex( Scalar(1), Scalar(0) ) + m01 * etaS;
				const Complex C = m10 * Complex( Scalar(1), Scalar(0) ) + m11 * etaS;
				const Complex Y = C / B;

				const Complex r = ( eta0 - Y ) / ( eta0 + Y );
				return std::norm( r );		// |r|²
			}
		}

		//======================================================================
		//  Public hot-path API
		//======================================================================

		//! Unpolarized reflectance R = ½(R_s + R_p) of an
		//! ambient / single-film / substrate stack at a single hero
		//! wavelength, using the validated Airy closed form.
		//!
		//! \param cosThetaI    cosine of the angle between the incident
		//!                     direction and the microfacet half-vector,
		//!                     in [0,1] (NOT the geometric-normal cosine).
		//! \param wavelength_nm hero wavelength, nm.
		//! \param n0           ambient real index (N0 = n0 + i k0; air = 1+0i).
		//! \param k0           ambient extinction.
		//! \param n1           film real index (N1 = n1 + i k1; the oxide).
		//! \param k1           film extinction (k1>0 absorbing).
		//! \param thickness_nm physical film thickness, nm.
		//! \param n2           substrate real index (Ns = n2 + i k2; the metal).
		//! \param k2           substrate extinction.
		//! \return             unpolarized reflectance R in [0,1].
		//!
		//! No heap allocation; one complex exp on the hot path.
		inline Scalar ReflectanceConductor(
			Scalar cosThetaI,
			Scalar wavelength_nm,
			Scalar n0, Scalar k0,
			Scalar n1, Scalar k1,
			Scalar thickness_nm,
			Scalar n2, Scalar k2 )
		{
			const Complex N0 = detail::MakeIndex( n0, k0 );
			const Complex N1 = detail::MakeIndex( n1, k1 );
			const Complex Ns = detail::MakeIndex( n2, k2 );

			const Complex sinInv = detail::SnellInvariant( N0, cosThetaI );

			const Scalar Rs = detail::AiryReflectanceForPol(
				N0, N1, Ns, thickness_nm, wavelength_nm, sinInv, ePolS );
			const Scalar Rp = detail::AiryReflectanceForPol(
				N0, N1, Ns, thickness_nm, wavelength_nm, sinInv, ePolP );

			// R is mathematically in [0,1] for a passive stack; clamp only
			// to defend against FP round-off pushing it a hair past the
			// boundary (e.g. 1+1e-16), never to mask a real overflow.
			Scalar R = Scalar(0.5) * ( Rs + Rp );
			if( R < Scalar(0) ) R = Scalar(0);
			if( R > Scalar(1) ) R = Scalar(1);
			return R;
		}

		//! Convenience overload taking already-built complex indices.
		inline Scalar ReflectanceConductor(
			Scalar cosThetaI,
			Scalar wavelength_nm,
			const Complex& N0,
			const Complex& N1,
			Scalar thickness_nm,
			const Complex& Ns )
		{
			const Complex sinInv = detail::SnellInvariant( N0, cosThetaI );

			const Scalar Rs = detail::AiryReflectanceForPol(
				N0, N1, Ns, thickness_nm, wavelength_nm, sinInv, ePolS );
			const Scalar Rp = detail::AiryReflectanceForPol(
				N0, N1, Ns, thickness_nm, wavelength_nm, sinInv, ePolP );

			Scalar R = Scalar(0.5) * ( Rs + Rp );
			if( R < Scalar(0) ) R = Scalar(0);
			if( R > Scalar(1) ) R = Scalar(1);
			return R;
		}

		//! General N-layer unpolarized reflectance (the documented
		//! multi-oxide extension point).  `nFilms` must be in [0, kMaxFilms];
		//! `filmIndex` / `filmThickness_nm` are caller-owned arrays of that
		//! length (ambient -> substrate order).  With nFilms == 1 this is
		//! algebraically the Airy single-film result.  Allocation-free.
		inline Scalar ReflectanceConductorStack(
			Scalar cosThetaI,
			Scalar wavelength_nm,
			const Complex& N0,
			const Complex* filmIndex,
			const Scalar* filmThickness_nm,
			int nFilms,
			const Complex& Ns )
		{
			if( nFilms < 0 ) nFilms = 0;
			if( nFilms > kMaxFilms ) nFilms = kMaxFilms;

			const Complex sinInv = detail::SnellInvariant( N0, cosThetaI );

			const Scalar Rs = detail::TmmReflectanceForPol(
				N0, filmIndex, filmThickness_nm, nFilms, Ns, wavelength_nm, sinInv, ePolS );
			const Scalar Rp = detail::TmmReflectanceForPol(
				N0, filmIndex, filmThickness_nm, nFilms, Ns, wavelength_nm, sinInv, ePolP );

			Scalar R = Scalar(0.5) * ( Rs + Rp );
			if( R < Scalar(0) ) R = Scalar(0);
			if( R > Scalar(1) ) R = Scalar(1);
			return R;
		}

		//======================================================================
		//  RGB-path albedo-basis integration (PREVIEW — see §8)
		//======================================================================
		//
		//  The RGB rendering path has no wavelength, so the spectral
		//  interference reflectance R(λ) of the air/oxide/metal stack is
		//  pre-integrated against the renderer's CIE colour-matching
		//  functions in the ALBEDO BASIS (docs/THIN_FILM_INTERFERENCE.md §8,
		//  second bullet):
		//
		//      XYZ = Σ R(λ)·cmf(λ)·Δλ  /  Σ ȳ(λ)·Δλ
		//
		//  This is WHITE-NORMALIZED and ILLUMINANT-INDEPENDENT: a perfect
		//  reflector R(λ) ≡ 1 maps to XYZ = (white point of the equal-energy
		//  illuminant), and after XYZ→Rec.709 to neutral RGB (1,1,1) — NOT a
		//  D65-tinted colour.  It is deliberately DIFFERENT from the Phase-1
		//  swatch's D65 preview (§8, first bullet), which weights by S_D65(λ)
		//  to predict "what the surface looks like under a daylight viewer".
		//  Here the renderer multiplies this reflectance by the incident RGB
		//  radiance, so the normaliser must be illuminant-free.
		//
		//  The result is a linear Rec.709 reflectance (RISEPel).  It is
		//  PREVIEW-GRADE: three RGB point-samples cannot represent an
		//  interference integral, which is exactly why this offline
		//  pre-integration replaces a naive per-channel evaluation.  The
		//  SPECTRAL path (ReflectanceConductor per hero wavelength) carries
		//  no such approximation and is the authoritative result.
		//
		//  WHITE NORMALIZATION (the §8 subtlety that bites colour-space
		//  work).  The renderer's working space (RISEPel == Rec.709) has a
		//  D65 whitepoint, so "R(λ)≡1 → neutral RGB(1,1,1)" requires the
		//  flat reflector to integrate to the D65 white point — NOT the
		//  equal-energy point E that a bare Σ R·cmf / Σ ȳ would give (E maps
		//  to a BLUISH-TINTED Rec.709 triple, the bug an illuminant-naive
		//  normaliser introduces).  We therefore (1) form the equal-energy
		//  relative tristimulus Σ R·cmf / Σ cmf per channel (so R≡1 → E),
		//  then (2) von-Kries scale E → the matrix's exact D65 white
		//  Rec709RGBtoXYZ(1,1,1) (so R≡1 → D65 white → neutral RGB).  The
		//  scene illuminant never enters — the basis is reflectance, the
		//  renderer multiplies incident RGB radiance afterwards.
		//
		//  Per the locked design (§7/§13) this integral is the GENERATOR for
		//  a per-material (cosθ × thickness) 2D LUT; the LUT itself is a
		//  documented follow-up (it requires the substrate/film complex
		//  indices to be CONSTANT across the material, which cannot be
		//  guaranteed at the GGX layer because they arrive as possibly
		//  spatially-varying IScalarPainters).  Calling this per shade is
		//  always correct regardless of spatial variation.

		//! Number of wavelength strata for the RGB albedo-basis integral.
		//! 32 uniform samples across the visible band: above the Nyquist
		//! limit of the interference fringes for the shipped oxide thickness
		//! range (a 250 nm, n≈2.5 film has fringe spacing ~tens of nm), and
		//! a fixed compile-time count keeps the hot loop allocation-free.
		static const int kRGBIntegrationSamples = 32;

		//! RGB albedo-basis reflectance of an ambient / single-film /
		//! substrate stack at one half-vector cosine (the §8 preview
		//! integration).  See the block comment above for the convention.
		//!
		//! \param cosThetaI    cosine of the angle between the incident
		//!                     direction and the microfacet half-vector,
		//!                     in [0,1] (NOT the geometric-normal cosine) —
		//!                     identical to ReflectanceConductor.
		//! \param n0           ambient real index (air = 1+0i).
		//! \param k0           ambient extinction.
		//! \param n1           film (oxide) real index.
		//! \param k1           film (oxide) extinction.
		//! \param thickness_nm physical film thickness, nm.
		//! \param n2           substrate (metal) real index.
		//! \param k2           substrate (metal) extinction.
		//! \return             linear Rec.709 reflectance (RISEPel); a
		//!                     perfect reflector → neutral (1,1,1).
		//!
		//! Allocation-free; kRGBIntegrationSamples Airy evaluations + CMF
		//! lookups.  Only ever called on the RGB preview path of a thin-film
		//! GGX material, so it adds zero cost to existing render paths.
		inline RISEPel ReflectanceConductorRGB(
			Scalar cosThetaI,
			Scalar n0, Scalar k0,
			Scalar n1, Scalar k1,
			Scalar thickness_nm,
			Scalar n2, Scalar k2 )
		{
			// Integration band matches the renderer's CIE table support.
			const Scalar loNm = Scalar( 380 );
			const Scalar hiNm = Scalar( 780 );
			const Scalar step = ( hiNm - loNm ) / Scalar( kRGBIntegrationSamples );

			const Complex N0 = detail::MakeIndex( n0, k0 );
			const Complex N1 = detail::MakeIndex( n1, k1 );
			const Complex Ns = detail::MakeIndex( n2, k2 );
			const Complex sinInv = detail::SnellInvariant( N0, cosThetaI );

			// Numerator Σ R·cmf·Δλ and the PER-CHANNEL equal-energy basis
			// sums Σ cmf·Δλ.  Sharing the SAME midpoint quadrature for both
			// means a flat reflector R≡1 lands EXACTLY on the equal-energy
			// white E (Xn/Xe = Yn/Ye = Zn/Ze = 1) to quadrature error, with
			// no normaliser mismatch.
			Scalar Xn = Scalar( 0 ), Yn = Scalar( 0 ), Zn = Scalar( 0 );
			Scalar Xe = Scalar( 0 ), Ye = Scalar( 0 ), Ze = Scalar( 0 );

			for( int s = 0; s < kRGBIntegrationSamples; ++s ) {
				const Scalar nm = loNm + ( Scalar( s ) + Scalar( 0.5 ) ) * step;

				XYZPel cmf;
				if( !ColorUtils::XYZFromNM( cmf, nm ) ) {
					continue;
				}

				const Scalar Rs = detail::AiryReflectanceForPol(
					N0, N1, Ns, thickness_nm, nm, sinInv, ePolS );
				const Scalar Rp = detail::AiryReflectanceForPol(
					N0, N1, Ns, thickness_nm, nm, sinInv, ePolP );
				Scalar R = Scalar( 0.5 ) * ( Rs + Rp );
				if( R < Scalar( 0 ) ) R = Scalar( 0 );
				if( R > Scalar( 1 ) ) R = Scalar( 1 );

				Xn += R * cmf.X * step;  Yn += R * cmf.Y * step;  Zn += R * cmf.Z * step;
				Xe += cmf.X * step;      Ye += cmf.Y * step;      Ze += cmf.Z * step;
			}

			// Equal-energy relative tristimulus: R≡1 → (1,1,1) (white E).
			const Scalar relX = ( Xe > Scalar( 0 ) ) ? Xn / Xe : Scalar( 0 );
			const Scalar relY = ( Ye > Scalar( 0 ) ) ? Yn / Ye : Scalar( 0 );
			const Scalar relZ = ( Ze > Scalar( 0 ) ) ? Zn / Ze : Scalar( 0 );

			// von-Kries scale E → the matrix's exact D65 white so that R≡1
			// maps to neutral Rec.709.  whiteXYZ = Rec709RGBtoXYZ(1,1,1) is
			// the D65 whitepoint baked into the conversion matrix, so the
			// neutrality holds to the matrix's own precision (not an
			// approximate literal).
			const Rec709RGBPel whiteRGB( Scalar( 1 ), Scalar( 1 ), Scalar( 1 ) );
			const XYZPel whiteXYZ = ColorUtils::Rec709RGBtoXYZ( whiteRGB );

			XYZPel xyz;
			xyz.X = relX * whiteXYZ.X;
			xyz.Y = relY * whiteXYZ.Y;
			xyz.Z = relZ * whiteXYZ.Z;

			// XYZ → linear Rec.709 (RISEPel).  XYZtoRec709RGB gamut-maps,
			// keeping the preview displayable; a passive R(λ)∈[0,1] stack
			// stays inside or near the gamut so the clip is negligible.
			RISEPel rgb = ColorUtils::XYZtoRec709RGB( xyz );

			// Defend against a hair of negative from gamut mapping / FP;
			// reflectance is physically non-negative.
			if( rgb.r < Scalar( 0 ) ) rgb.r = Scalar( 0 );
			if( rgb.g < Scalar( 0 ) ) rgb.g = Scalar( 0 );
			if( rgb.b < Scalar( 0 ) ) rgb.b = Scalar( 0 );
			return rgb;
		}
		
		//! Hemispherical Fresnel average  F_avg = 2 * integral_0^1 R(mu) mu d_mu
		//! of the air/film/substrate stack at one wavelength.  Feeds the
		//! Kulla-Conty multiscatter tail of the thin-film GGX mode: the film
		//! shifts the average reflectance by up to ~0.5 vs the bare substrate
		//! (tests/ThinFilmFurnaceTest.cpp), so the substrate average is unusable
		//! there.  Uses the SAME 21-point Gauss-Legendre rule as the conductor's
		//! MicrofacetEnergyLUT::ComputeFresnelAvg (same GL_nodes / GL_weights /
		//! 2*mu*w weighting), so at film==air this average is byte-identical to
		//! the conductor's — the additive invariant (ThinFilmBRDFTest Test D)
		//! holds in the multiscatter tail, not just the single-scatter lobe.
		//! Gauss-Legendre is also more accurate than the old midpoint rule
		//! (matches the 256-pt reference in ThinFilmFurnaceTest Gate 2 to
		//! < 2e-3).  A per-material (nm x thickness) LUT is the deferred
		//! optimisation for spatially-constant stacks; this per-shade quadrature
		//! is always correct.  docs/THIN_FILM_INTERFERENCE.md section 7.
		inline Scalar FresnelAvgConductor(
			Scalar wavelength_nm,
			Scalar n0, Scalar k0,
			Scalar n1, Scalar k1,
			Scalar thickness_nm,
			Scalar n2, Scalar k2 )
		{
			Scalar sum = Scalar( 0 );
			for( int i = 0; i < MicrofacetEnergyLUT::GL_N; ++i ) {
				const Scalar mu = MicrofacetEnergyLUT::GL_nodes[i];
				sum += ReflectanceConductor( mu, wavelength_nm, n0, k0, n1, k1, thickness_nm, n2, k2 )
					* ( Scalar( 2 ) * mu * MicrofacetEnergyLUT::GL_weights[i] );
			}
			return sum;
		}
		
		//! RGB (no-wavelength) hemispherical Fresnel average for the thin-film
		//! multiscatter tail.  PREVIEW-grade and the costliest thin-film call
		//! (GL_N angles x the internal kRGBIntegrationSamples-lambda CMF
		//! integral); the K-C tail is a high-roughness-only correction.  Uses
		//! the SAME 21-point Gauss-Legendre rule as the spectral
		//! FresnelAvgConductor above (and the conductor ComputeFresnelAvg).
		inline RISEPel FresnelAvgConductorRGB(
			Scalar n0, Scalar k0,
			Scalar n1, Scalar k1,
			Scalar thickness_nm,
			Scalar n2, Scalar k2 )
		{
			RISEPel sum( Scalar( 0 ), Scalar( 0 ), Scalar( 0 ) );
			for( int i = 0; i < MicrofacetEnergyLUT::GL_N; ++i ) {
				const Scalar mu = MicrofacetEnergyLUT::GL_nodes[i];
				sum = sum + ReflectanceConductorRGB( mu, n0, k0, n1, k1, thickness_nm, n2, k2 )
					* ( Scalar( 2 ) * mu * MicrofacetEnergyLUT::GL_weights[i] );
			}
			return sum;
		}
	}
}

#endif
