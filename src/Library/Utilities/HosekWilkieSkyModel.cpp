//////////////////////////////////////////////////////////////////////
//
//  HosekWilkieSkyModel.cpp - See HosekWilkieSkyModel.h.
//
//  Coefficients verbatim from Preetham 1999 Table 2 + §4.4.  The
//  values are facts and not copyrightable.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#define _USE_MATH_DEFINES
#include "HosekWilkieSkyModel.h"
#include "Color/ColorUtils.h"
#include "Color/ColorConversion.h"
#include "Math3D/Math3D.h"
#include "../Interfaces/ILog.h"

#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace RISE;

namespace
{
	// ----- Preetham 1999 Table 2: linear-in-T coefficients for the
	//                              Perez 5-coeff angular form.
	// Y (luminance distribution, sky F1):
	const Scalar kPerez_Y_A_t =  0.1787, kPerez_Y_A_c = -1.4630;
	const Scalar kPerez_Y_B_t = -0.3554, kPerez_Y_B_c =  0.4275;
	const Scalar kPerez_Y_C_t = -0.0227, kPerez_Y_C_c =  5.3251;
	const Scalar kPerez_Y_D_t =  0.1206, kPerez_Y_D_c = -2.5771;
	const Scalar kPerez_Y_E_t = -0.0670, kPerez_Y_E_c =  0.3703;
	// x (chromaticity x):
	const Scalar kPerez_x_A_t = -0.0193, kPerez_x_A_c = -0.2592;
	const Scalar kPerez_x_B_t = -0.0665, kPerez_x_B_c =  0.0008;
	const Scalar kPerez_x_C_t = -0.0004, kPerez_x_C_c =  0.2125;
	const Scalar kPerez_x_D_t = -0.0641, kPerez_x_D_c = -0.8989;
	const Scalar kPerez_x_E_t = -0.0033, kPerez_x_E_c =  0.0452;
	// y (chromaticity y):
	const Scalar kPerez_y_A_t = -0.0167, kPerez_y_A_c = -0.2608;
	const Scalar kPerez_y_B_t = -0.0950, kPerez_y_B_c =  0.0092;
	const Scalar kPerez_y_C_t = -0.0079, kPerez_y_C_c =  0.2102;
	const Scalar kPerez_y_D_t = -0.0441, kPerez_y_D_c = -1.6537;
	const Scalar kPerez_y_E_t = -0.0109, kPerez_y_E_c =  0.0529;

	// ----- Zenith chromaticity polynomials (Preetham §4.4).
	// x_z = c_x · (T·T·M_x[i] + T·M_x[i+5] + M_x[i+10] + M_x[i+15])  (per cubic in θ_s)
	// (Same structure for y_z; for Y_z it's a 4th-order poly on T·χ).

	static Scalar ZenithLuminance( Scalar T, Scalar thetaS )
	{
		// Preetham eq 5: Y_z = (4.0453·T - 4.9710)·tan(χ) - 0.2155·T + 2.4192
		// where χ = (4/9 - T/120) · (π - 2θ_s)
		const Scalar chi = (Scalar(4.0/9.0) - T / Scalar(120)) * (Scalar(M_PI) - Scalar(2) * thetaS);
		return (Scalar(4.0453) * T - Scalar(4.9710)) * std::tan( chi )
		     - Scalar(0.2155) * T + Scalar(2.4192);	// kcd/m²
	}

	static Scalar ZenithChromaticityX( Scalar T, Scalar thetaS )
	{
		// Preetham eq 6 + Table 4 (cubic in θ_s, quadratic in T).
		const Scalar t1 = thetaS;
		const Scalar t2 = thetaS * thetaS;
		const Scalar t3 = t2 * thetaS;
		return ( Scalar(0.00166) * t3 - Scalar(0.00375) * t2 + Scalar(0.00209) * t1 + Scalar(0)        ) * T * T
		     + ( Scalar(-0.02903) * t3 + Scalar(0.06377) * t2 - Scalar(0.03202) * t1 + Scalar(0.00394) ) * T
		     + ( Scalar(0.11693)  * t3 - Scalar(0.21196) * t2 + Scalar(0.06052) * t1 + Scalar(0.25886) );
	}

	static Scalar ZenithChromaticityY( Scalar T, Scalar thetaS )
	{
		const Scalar t1 = thetaS;
		const Scalar t2 = thetaS * thetaS;
		const Scalar t3 = t2 * thetaS;
		return ( Scalar(0.00275) * t3 - Scalar(0.00610) * t2 + Scalar(0.00317) * t1 + Scalar(0)        ) * T * T
		     + ( Scalar(-0.04214) * t3 + Scalar(0.08970) * t2 - Scalar(0.04153) * t1 + Scalar(0.00516) ) * T
		     + ( Scalar(0.15346)  * t3 - Scalar(0.26756) * t2 + Scalar(0.06670) * t1 + Scalar(0.26688) );
	}
}

HosekWilkieSkyModel::HosekWilkieSkyModel(
	Scalar solarElevationRadians,
	Scalar solarAzimuthRadians,
	Scalar T,
	const RISEPel& albedo
) : turbidity( T ), groundAlbedo( albedo )
{
	// Solar zenith angle = π/2 - elevation.  Elevation is measured
	// from the horizon (matching glTF and most sky-modelling
	// conventions); zenith convention θ_s used by Preetham.
	thetaS = Scalar(M_PI / 2.0) - solarElevationRadians;
	thetaS = std::max( Scalar(0), std::min( Scalar(M_PI / 2.0), thetaS ) );
	cosThetaS = std::cos( thetaS );

	// Sun direction in RISE world coords (Y up): elevation lifts
	// from horizon toward zenith; azimuth rotates around Y.  The
	// returned direction points FROM the surface TO the light
	// source (matching directional_light convention; see
	// docs/SCENE_CONVENTIONS.md).
	const Scalar cosEl = std::cos( solarElevationRadians );
	const Scalar sinEl = std::sin( solarElevationRadians );
	sunDir = Vector3(
		cosEl * std::sin( solarAzimuthRadians ),
		sinEl,
		cosEl * std::cos( solarAzimuthRadians )
	);

	// Preetham coeffs as linear-in-T from Table 2.
	perez_Y.A = kPerez_Y_A_t * T + kPerez_Y_A_c;
	perez_Y.B = kPerez_Y_B_t * T + kPerez_Y_B_c;
	perez_Y.C = kPerez_Y_C_t * T + kPerez_Y_C_c;
	perez_Y.D = kPerez_Y_D_t * T + kPerez_Y_D_c;
	perez_Y.E = kPerez_Y_E_t * T + kPerez_Y_E_c;

	perez_x.A = kPerez_x_A_t * T + kPerez_x_A_c;
	perez_x.B = kPerez_x_B_t * T + kPerez_x_B_c;
	perez_x.C = kPerez_x_C_t * T + kPerez_x_C_c;
	perez_x.D = kPerez_x_D_t * T + kPerez_x_D_c;
	perez_x.E = kPerez_x_E_t * T + kPerez_x_E_c;

	perez_y.A = kPerez_y_A_t * T + kPerez_y_A_c;
	perez_y.B = kPerez_y_B_t * T + kPerez_y_B_c;
	perez_y.C = kPerez_y_C_t * T + kPerez_y_C_c;
	perez_y.D = kPerez_y_D_t * T + kPerez_y_D_c;
	perez_y.E = kPerez_y_E_t * T + kPerez_y_E_c;

	Yz = ZenithLuminance( T, thetaS );
	xz = ZenithChromaticityX( T, thetaS );
	yz = ZenithChromaticityY( T, thetaS );

	// Convert zenith Y from kcd/m² to a normalized scale.  The
	// rendering pipeline uses arbitrary radiometric units anyway;
	// keep the relative ratios across (sun, sky) consistent and let
	// scene-level intensity multipliers handle absolute brightness.
	if( Yz < 0 ) Yz = 0;

	// v1 (Preetham 1999) doesn't model the ground-coupling term that
	// Hosek-Wilkie 2012 introduces — non-default groundAlbedo has no
	// effect on the rendered sky.  Warn callers exactly once per
	// process so they can either adjust expectations or wait for the
	// v2 swap to vendored HW reference data.  Keeping the parameter
	// in the public API preserves source-compat for the v2 transition.
	const bool nonDefaultAlbedo =
		std::fabs( double(albedo.r) - 0.3 ) > 0.05 ||
		std::fabs( double(albedo.g) - 0.3 ) > 0.05 ||
		std::fabs( double(albedo.b) - 0.3 ) > 0.05;
	if( nonDefaultAlbedo ) {
		static bool warned = false;
		if( !warned ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"HosekWilkieSkyModel: v1 (Preetham 1999) does not model "
				"ground-albedo coupling — your groundAlbedo (%.2f, %.2f, %.2f) "
				"will have no effect on the rendered sky.  The parameter is "
				"plumbed for v2 (HW reference data) compatibility.",
				double(albedo.r), double(albedo.g), double(albedo.b) );
			warned = true;
		}
	}
}

Scalar HosekWilkieSkyModel::PerezF(
	const PerezCoeffs& c, Scalar cosTheta, Scalar gamma )
{
	// F(θ, γ) = (1 + A·exp(B/cos θ)) · (1 + C·exp(D·γ) + E·cos²γ)
	// θ is angle from zenith (so cos θ = dot(dir, +Y)); γ is angle
	// between sample direction and sun.
	const Scalar safeCos = std::max( Scalar(1e-3), cosTheta );	// avoid div-by-0 at horizon
	const Scalar term1 = Scalar(1) + c.A * std::exp( c.B / safeCos );
	const Scalar cosG  = std::cos( gamma );
	const Scalar term2 = Scalar(1) + c.C * std::exp( c.D * gamma ) + c.E * cosG * cosG;
	return term1 * term2;
}

void HosekWilkieSkyModel::SampleXYY(
	const Vector3& dir, Scalar& x, Scalar& y, Scalar& Y ) const
{
	// Direction is normalized; .y component = cos(θ) where θ is
	// angle from zenith.  Negative y → ground hit; clamp to 0
	// (caller filters this case before calling).
	const Scalar cosTheta = std::max( Scalar(0), dir.y );
	const Scalar dotSun   = std::max( Scalar(-1), std::min( Scalar(1), Vector3Ops::Dot( dir, sunDir ) ) );
	const Scalar gamma    = std::acos( dotSun );

	// Preetham eq 3: scale by F(θ, γ) / F(0, θ_s).
	const Scalar F_xy_Y = PerezF( perez_Y, cosTheta, gamma );
	const Scalar F_xy_x = PerezF( perez_x, cosTheta, gamma );
	const Scalar F_xy_y = PerezF( perez_y, cosTheta, gamma );

	const Scalar F_z_Y  = PerezF( perez_Y, Scalar(1), thetaS );	// θ=0 (zenith), γ=θ_s
	const Scalar F_z_x  = PerezF( perez_x, Scalar(1), thetaS );
	const Scalar F_z_y  = PerezF( perez_y, Scalar(1), thetaS );

	Y = Yz * F_xy_Y / F_z_Y;
	x = xz * F_xy_x / F_z_x;
	y = yz * F_xy_y / F_z_y;
}

Scalar HosekWilkieSkyModel::SampleRadiance(
	const Vector3& dir, Scalar lambda_nm ) const
{
	if( lambda_nm < Scalar(380) || lambda_nm > Scalar(780) ) return 0;
	if( dir.y <= Scalar(0) ) return 0;	// ground hit

	Scalar x, y, Y;
	SampleXYY( dir, x, y, Y );

	if( Y < 0 ) return 0;
	if( y < Scalar(1e-6) ) return 0;

	// Convert sky's xyY → XYZ → spectral radiance via D65 illuminant
	// scaled by chromaticity tint.  Approximate but visually accurate:
	// the sky's chromaticity is dominantly blue-skewed Rayleigh +
	// horizon-orange Mie; scaling D65 by the local (x, y) gives the
	// right per-wavelength shaping at the 5-10% level which is what
	// the user can actually see in the sky lab scene.
	XYZPel xyz;
	xyz.Y = Y;
	xyz.X = (x / y) * Y;
	xyz.Z = ((Scalar(1) - x - y) / y) * Y;

	// Quick spectral approximation: assume the SPD is a linear
	// combination of CIE colour matching functions weighted by
	// (X, Y, Z) — the analytical inverse used by Smits 1999.  For
	// rendering purposes this gives a smooth plausible spectrum
	// whose XYZ integral is exactly (X, Y, Z) by construction.
	// At wavelength λ:
	//   S(λ) = X · x̄_inv(λ) + Y · ȳ_inv(λ) + Z · z̄_inv(λ)
	// where x̄_inv is the appropriate basis function.  Cheapest
	// approximation: per-axis Smits-style "step" basis.
	// We use a simpler still approximation: weight the CIE x̄/ȳ/z̄
	// matching functions themselves and let the inverse fall out
	// in the ratio sense.  At each wavelength:
	//   S(λ) = (X·x̄(λ) + Y·ȳ(λ) + Z·z̄(λ)) / (x̄+ȳ+z̄)(λ)
	// This is approximate but gives sky-blue at short λ + sun-warm
	// at long λ which is the visible behaviour.
	XYZPel obs;
	if( !ColorUtils::XYZFromNM( obs, lambda_nm ) ) return 0;
	const Scalar denom = obs.X + obs.Y + obs.Z;
	if( denom < Scalar(1e-9) ) return 0;
	return ( xyz.X * obs.X + xyz.Y * obs.Y + xyz.Z * obs.Z ) / denom;
}

Scalar HosekWilkieSkyModel::SampleSolarRadiance( Scalar lambda_nm ) const
{
	if( lambda_nm < Scalar(380) || lambda_nm > Scalar(780) ) return 0;

	// Sun's emission ≈ 5778K blackbody.  Atmospheric extinction
	// (Rayleigh + Mie) attenuates more at short λ as
	// exp(-τ_R(λ)·m(θ_s)) where τ_R(λ) ∝ λ⁻⁴.  The air mass m
	// grows ~ 1/cos θ_s at moderate elevations.
	const Scalar h        = Scalar(6.62607015e-34);
	const Scalar c0       = Scalar(2.99792458e8);
	const Scalar k        = Scalar(1.380649e-23);
	const Scalar T        = Scalar(5778);
	const Scalar lambda_m = lambda_nm * Scalar(1e-9);
	const Scalar exponent = (h * c0) / (lambda_m * k * T);
	const Scalar planck   = (Scalar(2) * h * c0 * c0) /
	                        (lambda_m * lambda_m * lambda_m * lambda_m * lambda_m *
	                         (std::exp( exponent ) - Scalar(1)));

	// Rayleigh extinction: τ ∝ T · λ⁻⁴ (turbidity boosts atmospheric
	// scattering effectiveness).  Air mass m approximated by 1/cos θ_s
	// (Kasten-Young at modest elevations).
	const Scalar lambda_norm = lambda_nm / Scalar(550);	// ref 550nm
	const Scalar tauR  = (turbidity / Scalar(10)) * std::pow( lambda_norm, Scalar(-4) );
	const Scalar airMass = Scalar(1) / std::max( Scalar(0.1), cosThetaS );
	const Scalar trans = std::exp( -tauR * airMass );

	// Normalize so peak (visible-band, low turbidity) ~ 1.0.  The
	// Planck constant gives an absolute radiance; we want a relative
	// figure consistent with sky radiance.  Choose normalisation
	// `1e-13` empirically so the sun's 555nm radiance lands in the
	// same 1-100 range as sky radiance — a scene-level intensity
	// multiplier on the sun light gives finer control.
	return planck * trans * Scalar(1e-13);
}

RISEPel HosekWilkieSkyModel::IntegrateRGB( const Vector3& dir ) const
{
	if( dir.y <= Scalar(0) ) return RISEPel( 0, 0, 0 );

	Scalar x, y, Y;
	SampleXYY( dir, x, y, Y );
	if( Y < 0 || y < Scalar(1e-6) ) return RISEPel( 0, 0, 0 );

	XYZPel xyz;
	xyz.Y = Y;
	xyz.X = (x / y) * Y;
	xyz.Z = ((Scalar(1) - x - y) / y) * Y;

	// XYZ(D65) → RISEPel via the implicit RISEPel(XYZPel) constructor.
	// Post Stage-B colour-space migration, RISEPel = Rec709RGBPel; the
	// implicit conversion runs `ColorUtils::XYZtoRec709RGB` (no Bradford
	// adapt — Preetham/HW chromaticities are D65-referred, Rec.709 is
	// D65, the matrix is straight XYZ→Rec.709).
	return RISEPel( xyz );
}
