//////////////////////////////////////////////////////////////////////
//
//  HosekWilkieSkyModel.h - Analytic spectral sun-and-sky model.
//
//  Currently implements the Preetham (1999) Perez 5-coefficient model
//  (Table 2 linear turbidity fits, §4.4 zenith xyY) recombined with a
//  D65 illuminant tint; public API is ready to swap in Hosek & Wilkie
//  (2012) full-spectral tables without breaking callers.
//  Known v1 limits: Preetham over-reddens at low solar elevation, and
//  spectral output is RGB/D65-derived rather than natively spectral.
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

        // Analytic xyY -> XYZ(D65) -> linear Rec.709 RISEPel (does not
        // integrate SampleRadiance).  Stateless and thread-safe; used by
        // IRadianceMap::GetRadiance and its legacy painter proxy.
		RISEPel IntegrateRGB( const Vector3& dir ) const;

	private:
		// Preetham-Perez form: F(theta, gamma) = (1 + A*exp(B/cos theta)) * (1 + C*exp(D*gamma) + E*cos^2 gamma),
		// with cos theta = dir.y and gamma = angle between sample direction and sun.
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
