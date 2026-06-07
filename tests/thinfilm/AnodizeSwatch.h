//////////////////////////////////////////////////////////////////////
//
//  AnodizeSwatch.h - Preview colorimetry for a thin-film anodize swatch:
//    turns a (metal, oxide) pair + film thickness into the colour the
//    surface shows to a daylight (D65) viewer, the way anodization charts
//    are photographed.  Part of the Phase-1 thin-film validation gate
//    (docs/THIN_FILM_INTERFERENCE.md §8/§9, piece P1-C).
//
//    This is the PREVIEW integration of §8, NOT the Phase-2 RGB-path
//    albedo integration:
//
//      XYZ = Σ R(λ)·S_D65(λ)·cmf(λ)·Δλ  /  Σ S_D65(λ)·ȳ(λ)·Δλ
//
//    The white-normalisation denominator (Σ S_D65·ȳ·Δλ) makes a perfect
//    white reflector R(λ) ≡ 1 map to the D65 white point — i.e. the
//    swatch of a hypothetical R≡1 surface is neutral grey, not tinted.
//    The CMFs come from ColorUtils::XYZFromNM (which returns the CIE 1931
//    2° x̄/ȳ/z̄ VALUES interpolated at a wavelength — verified by reading
//    its source: it is NOT pre-integrated), so the swatch shares the
//    renderer's colour observer.  XYZ -> linear Rec.709 via
//    ColorUtils::XYZtoRec709RGB.
//
//    R(λ) is the unpolarized reflectance of the air/oxide/metal stack at
//    NORMAL incidence, evaluated by the P1-A single-film Airy reference
//    (AiryReference.h) and cross-checked against the TMM
//    (TmmReference.h).
//
//    Wavelength sampling is 1 nm across [380, 780].  At the thickest
//    film here (d = 250 nm, n ~ 2.5) the round-trip optical phase sweeps
//    several 2π over the visible; the interference fringes in R(λ) have a
//    minimum spacing of order λ²/(2 n d) ~ tens of nm, so 1 nm steps are
//    comfortably below Nyquist and do not alias the fringes (a coarse
//    5/10 nm sampling would).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef THINFILM_ANODIZESWATCH_H
#define THINFILM_ANODIZESWATCH_H

#include <cmath>
#include <string>
#include <vector>

#include "OpticalConstants.h"
#include "AiryReference.h"
#include "TmmReference.h"

#include "../../src/Library/Utilities/Color/Color.h"
#include "../../src/Library/Utilities/Color/ColorUtils.h"

namespace RISE
{
	namespace ThinFilmReference
	{
		//! The full colorimetric description of one swatch, retained so
		//! the test can both render it and assert on its colour signature.
		struct SwatchColor
		{
			double		thickness_nm;	//!< film thickness this swatch represents

			// Tristimulus + chromaticity, computed from the UNCLAMPED XYZ
			// (before any gamut mapping) so the hue/chroma metrics reflect
			// the true colorimetry, not a gamut-clipped approximation.
			XYZPel		xyz;			//!< D65-normalised tristimulus
			double		x, y;			//!< CIE 1931 chromaticity
			double		Y;				//!< luminance (== xyz.Y; R≡1 -> ~1.0)

			// Hue / saturation descriptors used by the validation gate.
			double		dominantNm;		//!< dominant wavelength (nm); <0 if non-spectral (purple)
			double		hueAngleDeg;	//!< atan2 hue angle in CIELAB a*/b* [0,360)
			double		chroma;			//!< CIELAB C* = sqrt(a*^2 + b*^2)
			double		aStar, bStar;	//!< CIELAB a*, b*

			// Display colour for the swatch grid: LINEAR Rec.709 (the PNG
			// writer applies the sRGB OETF on write, so we must NOT gamma
			// here).
			RISEPel		linearRGB;
		};

		namespace detail
		{
			//! D65 white point tristimulus in the SAME normalisation as
			//! the swatch integral: XYZ_white = Σ S_D65·cmf·Δλ / Σ S_D65·ȳ·Δλ
			//! (i.e. R(λ) ≡ 1).  Y is 1 by construction.  The integration
			//! grid (lo/hi/step) must match the swatch grid so the
			//! normalisation is exact.
			//!
			//! Reuses the renderer CMFs (ColorUtils::XYZFromNM).
			inline XYZPel ComputeD65WhitePoint( int loNm, int hiNm, double stepNm )
			{
				double X = 0.0, Y = 0.0, Z = 0.0;
				double denom = 0.0;	// Σ S_D65·ȳ·Δλ
				for( double nm = loNm; nm <= hiNm + 1e-9; nm += stepNm ) {
					XYZPel cmf;
					if( !ColorUtils::XYZFromNM( cmf, Scalar( nm ) ) ) {
						continue;
					}
					const double s = D65_DATA::SampleAtNM( nm );
					X += s * cmf.X * stepNm;
					Y += s * cmf.Y * stepNm;
					Z += s * cmf.Z * stepNm;
					denom += s * cmf.Y * stepNm;
				}
				XYZPel w;
				if( denom > 0.0 ) {
					w.X = X / denom;
					w.Y = Y / denom;	// == 1 by construction
					w.Z = Z / denom;
				}
				return w;
			}

			//! CIELAB f() helper.
			inline double LabF( double t )
			{
				const double d = 6.0 / 29.0;
				if( t > d * d * d ) {
					return std::cbrt( t );
				}
				return t / ( 3.0 * d * d ) + 4.0 / 29.0;
			}

			//! Converts a (D65-normalised) XYZ to CIELAB a*, b*, C*, hue
			//! using the matching D65 white as reference.
			inline void XYZtoLabChroma(
				const XYZPel& xyz, const XYZPel& white,
				double& Lstar, double& aStar, double& bStar,
				double& chroma, double& hueDeg )
			{
				const double fx = LabF( white.X > 0 ? xyz.X / white.X : 0.0 );
				const double fy = LabF( white.Y > 0 ? xyz.Y / white.Y : 0.0 );
				const double fz = LabF( white.Z > 0 ? xyz.Z / white.Z : 0.0 );
				Lstar = 116.0 * fy - 16.0;
				aStar = 500.0 * ( fx - fy );
				bStar = 200.0 * ( fy - fz );
				chroma = std::sqrt( aStar * aStar + bStar * bStar );
				hueDeg = std::atan2( bStar, aStar ) * 180.0 / 3.14159265358979323846;
				if( hueDeg < 0.0 ) hueDeg += 360.0;
			}

			//! Dominant wavelength of a chromaticity (x,y) relative to the
			//! white point, by scanning the spectral locus (the CMF
			//! chromaticities at 1-nm steps) for the boundary point
			//! colinear with white->colour on the colour side.  Returns a
			//! negative value when the colour lies on the purple line
			//! (complement direction) — a deliberate "non-spectral" flag.
			inline double DominantWavelength(
				double cx, double cy, double wx, double wy )
			{
				const double dx = cx - wx;
				const double dy = cy - wy;
				const double len = std::sqrt( dx * dx + dy * dy );
				if( len < 1e-7 ) {
					return -1.0;	// achromatic: no dominant wavelength
				}
				const double ux = dx / len;
				const double uy = dy / len;

				double bestDom = -1.0;
				double bestProj = -1e30;
				double bestComp = -1.0;
				double bestCompProj = -1e30;

				double prevX = 0.0, prevY = 0.0, prevNm = 0.0;
				bool   havePrev = false;

				for( int nm = 360; nm <= 830; ++nm ) {
					XYZPel cmf;
					if( !ColorUtils::XYZFromNM( cmf, Scalar( nm ) ) ) {
						havePrev = false;
						continue;
					}
					const double sum = cmf.X + cmf.Y + cmf.Z;
					if( sum <= 0.0 ) {
						havePrev = false;
						continue;
					}
					const double lx = cmf.X / sum;
					const double ly = cmf.Y / sum;

					if( havePrev ) {
						// Parameterise the locus segment prev->cur and find
						// where the ray white + t*u crosses it.
						const double sx = lx - prevX;
						const double sy = ly - prevY;
						const double denom = ux * sy - uy * sx;
						if( std::fabs( denom ) > 1e-12 ) {
							// Solve white + t*u = prev + q*s.
							const double rx = prevX - wx;
							const double ry = prevY - wy;
							const double t = ( rx * sy - ry * sx ) / denom;
							const double q = ( rx * uy - ry * ux ) / denom;
							if( q >= -1e-6 && q <= 1.0 + 1e-6 ) {
								const double nmHit = prevNm + q * ( nm - prevNm );
								if( t > 0.0 ) {
									// Same side as the colour -> dominant.
									if( t > bestProj ) {
										bestProj = t;
										bestDom = nmHit;
									}
								} else if( t < 0.0 ) {
									// Opposite side -> complementary (purple).
									if( -t > bestCompProj ) {
										bestCompProj = -t;
										bestComp = nmHit;
									}
								}
							}
						}
					}
					prevX = lx; prevY = ly; prevNm = nm;
					havePrev = true;
				}

				if( bestDom > 0.0 ) {
					return bestDom;
				}
				// Purple line: no spectral dominant wavelength; report the
				// complementary as a negative flag value.
				if( bestComp > 0.0 ) {
					return -bestComp;
				}
				return -1.0;
			}
		}

		//! Computes the preview-colorimetry swatch for one (metal, oxide)
		//! pair at one thickness.  `whiteRef` is the D65 white point on
		//! the SAME integration grid (so the normalisation is exact and
		//! the chroma metrics are referenced to the same white).
		inline SwatchColor ComputeSwatch(
			const MetalOxidePair& pair,
			double thickness_nm,
			const XYZPel& whiteRef,
			int loNm = 380, int hiNm = 780, double stepNm = 1.0 )
		{
			SwatchColor out;
			out.thickness_nm = thickness_nm;

			double X = 0.0, Y = 0.0, Z = 0.0;
			double denom = 0.0;	// Σ S_D65·ȳ·Δλ — the white normaliser

			for( double nm = loNm; nm <= hiNm + 1e-9; nm += stepNm ) {
				XYZPel cmf;
				if( !ColorUtils::XYZFromNM( cmf, Scalar( nm ) ) ) {
					continue;
				}
				const double s = D65_DATA::SampleAtNM( nm );

				// Unpolarized reflectance at normal incidence (θ = 0).
				const Stack stack = pair.BuildStack( thickness_nm, nm );
				const double R = AiryReflectanceUnpolarized( stack, nm, 0.0 );

				X += R * s * cmf.X * stepNm;
				Y += R * s * cmf.Y * stepNm;
				Z += R * s * cmf.Z * stepNm;
				denom += s * cmf.Y * stepNm;
			}

			XYZPel xyz;
			if( denom > 0.0 ) {
				xyz.X = X / denom;
				xyz.Y = Y / denom;
				xyz.Z = Z / denom;
			}
			out.xyz = xyz;
			out.Y = xyz.Y;

			const double sum = xyz.X + xyz.Y + xyz.Z;
			if( sum > 0.0 ) {
				out.x = xyz.X / sum;
				out.y = xyz.Y / sum;
			} else {
				out.x = out.y = 0.0;
			}

			// Hue / chroma from the UNCLAMPED tristimulus.
			double Lstar = 0.0;
			detail::XYZtoLabChroma( xyz, whiteRef,
				Lstar, out.aStar, out.bStar, out.chroma, out.hueAngleDeg );

			// Dominant wavelength relative to the matching white.
			const double wsum = whiteRef.X + whiteRef.Y + whiteRef.Z;
			const double wx = wsum > 0 ? whiteRef.X / wsum : 0.3127;
			const double wy = wsum > 0 ? whiteRef.Y / wsum : 0.3290;
			out.dominantNm = detail::DominantWavelength( out.x, out.y, wx, wy );

			// Display colour: XYZ -> linear Rec.709.  (Gamut-mapped inside
			// XYZtoRec709RGB so it stays displayable; the metrics above use
			// the unclamped XYZ instead.)
			out.linearRGB = ColorUtils::XYZtoRec709RGB( xyz );

			return out;
		}

		//! Convenience: the matching D65 white point on a given grid.
		inline XYZPel D65WhitePointForGrid( int loNm = 380, int hiNm = 780, double stepNm = 1.0 )
		{
			return detail::ComputeD65WhitePoint( loNm, hiNm, stepNm );
		}
	}
}

#endif
