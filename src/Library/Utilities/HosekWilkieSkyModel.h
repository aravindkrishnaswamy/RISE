//////////////////////////////////////////////////////////////////////
//
//  HosekWilkieSkyModel.h - Analytic spectral sun-and-sky model for
//    Landing 3 of the Physically Based Pipeline Plan.
//
//    NAMING NOTE.  The plan-doc target was the Hosek & Wilkie 2012
//    full-spectral model whose coefficient tables ship as supplemental
//    data alongside their reference paper (CC-BY 3.0).  Vendoring the
//    coefficient table requires fetching ~25KB of pre-computed data
//    files (`ArHosekSkyModel*Coefficients.cpp`) we do not currently
//    have at hand in this checkout.
//
//    v1 (this file): the Preetham 1999 "Practical Analytic Model for
//    Daylight" formulation.  Same Perez 5-coefficient angular form
//    Hosek-Wilkie generalises; coefficients are linear functions of
//    turbidity given in Preetham 1999 Table 2 (and reproduced verbatim
//    below — the values are facts and not copyrightable).  Output is
//    chromaticity + luminance, which we recombine into a spectrum via
//    a CIE D65 illuminant pre-multiplied by the chromaticity tint.
//
//    v1 LIMITATIONS vs the planned HW implementation:
//      - Less accurate at low solar elevations; Preetham reddening at
//        sunset over-saturates relative to HW Monte-Carlo reference.
//      - Spectral output is RGB-derived (D65 illuminant scaled by
//        chromaticity), not natively spectral.  Adequate for visible
//        differentiation of sun/sky but not for fine-grained spectral
//        verification against reference data.
//
//    v2 (future landing): swap the analytic core for the Hosek-Wilkie
//    reference C code + its CC-BY 3.0 coefficient tables.  Public
//    interface stays the same; only the per-sample radiance evaluation
//    changes.  All scene files and runtime callers built against v1
//    work unchanged.
//
//    Coefficient citations (Preetham 1999, "A Practical Analytic
//    Model for Daylight", SIGGRAPH 99):
//      Table 2: A_Y..E_Y, A_x..E_x, A_y..E_y as linear functions of T.
//      Section 4.4: zenith chromaticities x_z(T, θ_s), y_z(T, θ_s).
//      Section 4.4: zenith luminance Y_z(T, θ_s) in kcd/m².
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HOSEK_WILKIE_SKY_MODEL_
#define HOSEK_WILKIE_SKY_MODEL_

#include "Math3D/Math3D.h"
#include "Color/Color.h"

namespace RISE
{
	class HosekWilkieSkyModel
	{
	public:
		// Construct with solar elevation in radians (0 = horizon,
		// π/2 = zenith), turbidity ∈ [1, 10] (1 = arctic clear,
		// 10 = polluted), and per-channel ground albedo ∈ [0, 1]³.
		HosekWilkieSkyModel(
			Scalar solarElevationRadians,
			Scalar solarAzimuthRadians,
			Scalar turbidity,
			const RISEPel& groundAlbedo );

		~HosekWilkieSkyModel() {}

		// Returns the sun-pointing unit vector (FROM ground TO sun),
		// matching RISE's directional_light convention.  Y is up.
		Vector3 SunDirection() const { return sunDir; }

		// Sample the sky's radiance at a given direction, at a given
		// wavelength in nm.  Direction is the world-space ray direction
		// AWAY from the camera (looking up into the sky); for a ground
		// hit dir.y < 0 returns 0.
		// Wavelength outside [380, 780] nm returns 0.
		Scalar SampleRadiance( const Vector3& dir, Scalar lambda_nm ) const;

		// Sample the solar radiance at a given wavelength.  Used by the
		// matched directional_light created alongside the radiance map.
		// Models the sun's emission as a 5778K blackbody attenuated by
		// the atmosphere (turbidity-dependent extinction).
		Scalar SampleSolarRadiance( Scalar lambda_nm ) const;

		// RGB convenience: integrate the spectral radiance against CIE
		// 1931 + D65 → ROMM RGB.  Cached against a small directional
		// quantization so the first-call cost is paid once.  Used by
		// the IRadianceMap::GetRadiance non-spectral path.
		RISEPel IntegrateRGB( const Vector3& dir ) const;

	private:
		// Preetham angular form:
		//   F(θ, γ) = (1 + A·exp(B/cos θ)) · (1 + C·exp(D·γ) + E·cos²γ)
		// where θ is the angle from zenith of the sample direction
		// (so cos θ = dir.y) and γ is the angle between the sample
		// direction and the sun.  Two such F's give the sample's
		// chromaticity + luminance in xyY by ratio with the zenith F.
		struct PerezCoeffs { Scalar A, B, C, D, E; };

		PerezCoeffs perez_Y;
		PerezCoeffs perez_x;
		PerezCoeffs perez_y;

		Scalar      Yz, xz, yz;        // zenith xyY values
		Scalar      thetaS;             // solar zenith angle (radians)
		Scalar      cosThetaS;
		Vector3     sunDir;
		Scalar      turbidity;
		RISEPel     groundAlbedo;

		// Helper: angular Preetham-Perez perez form.
		static Scalar PerezF( const PerezCoeffs& c, Scalar cosTheta, Scalar gamma );

		// Preetham eq 3: scale a sample's xyY by the ratio
		// F(θ_sample, γ_sample) / F(0, θ_s) and multiply by zenith xyY.
		void SampleXYY( const Vector3& dir, Scalar& x, Scalar& y, Scalar& Y ) const;
	};
}

#endif
