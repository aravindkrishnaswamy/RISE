//////////////////////////////////////////////////////////////////////
//
//  JakobHanikaLUTGen.cpp - Offline tool to generate the Jakob-Hanika
//    2019 sigmoid-uplift LUT for Landing 3 (RGB → spectrum upsampling).
//
//    Solves, per cell of a 64×64×64 RGB grid:
//      Find (c0, c1, c2) such that
//        sigmoid(c0·λ² + c1·λ + c2) integrated against the CIE 1931
//        2° observer and converted XYZ → ROMM RGB returns the cell's
//        target RGB.
//
//    Algorithm: Gauss-Newton iteration on the 3-coefficient residual
//    with finite-difference Jacobian (Jakob & Hanika 2019 §3).
//
//    Output: binary .coeff file at extlib/jakob-hanika-luts/romm.coeff
//    consumed by RISE's RGBToSpectrumTable runtime loader.
//
//    The output LUT is for ROMM RGB (RISE's internal linear RGB
//    space, D50 whitepoint), not sRGB.  All RISE painters return
//    RISEPel = ROMMRGBPel, so the uplift must operate in that
//    space natively.
//
//    Build (from project root):
//      c++ -O3 -std=c++17 -o bin/tools/JakobHanikaLUTGen \
//          tools/JakobHanikaLUTGen.cpp -lm
//    On Windows the VS2022 project file builds the same source.
//
//    Run:
//      bin/tools/JakobHanikaLUTGen.exe \
//        --output extlib/jakob-hanika-luts/romm.coeff \
//        --resolution 64
//
//    Author: Aravind Krishnaswamy
//    Date: 2026-05-08
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <string>

namespace JH {

// CIE 1931 2° observer at 5nm spacing, 380-780nm (81 entries each).
// Copied verbatim from src/Library/Utilities/Color/ColorUtils.cpp
// (CIE_DATA::x_2 / y_2 / z_2) so the LUT generator and runtime
// integrator agree by construction.
static const int    kLambdaMin  = 380;
static const int    kLambdaMax  = 780;
static const int    kLambdaStep = 5;
static const int    kNLambda    = (kLambdaMax - kLambdaMin) / kLambdaStep + 1;	// 81

static const double kCIE_x[ kNLambda ] = {
	0.0014, 0.0022, 0.0042, 0.0077, 0.0143, 0.0232, 0.0435, 0.0776, 0.1344, 0.2148,
	0.2839, 0.3285, 0.3483, 0.3481, 0.3362, 0.3187, 0.2908, 0.2511, 0.1954, 0.1421,
	0.0956, 0.0580, 0.0320, 0.0147, 0.0049, 0.0024, 0.0093, 0.0291, 0.0633, 0.1096,
	0.1655, 0.2257, 0.2904, 0.3597, 0.4334, 0.5121, 0.5945, 0.6784, 0.7621, 0.8425,
	0.9163, 0.9786, 1.0263, 1.0567, 1.0622, 1.0456, 1.0026, 0.9384, 0.8544, 0.7514,
	0.6424, 0.5419, 0.4479, 0.3608, 0.2835, 0.2187, 0.1649, 0.1212, 0.0874, 0.0636,
	0.0468, 0.0329, 0.0227, 0.0158, 0.0114, 0.0081, 0.0058, 0.0041, 0.0029, 0.0020,
	0.0014, 0.0010, 0.0007, 0.0005, 0.0003, 0.0002, 0.0002, 0.0001, 0.0001, 0.0001,
	0.0000
};
static const double kCIE_y[ kNLambda ] = {
	0.0000, 0.0001, 0.0001, 0.0002, 0.0004, 0.0006, 0.0012, 0.0022, 0.0040, 0.0073,
	0.0116, 0.0168, 0.0230, 0.0298, 0.0380, 0.0480, 0.0600, 0.0739, 0.0910, 0.1126,
	0.1390, 0.1693, 0.2080, 0.2586, 0.3230, 0.4073, 0.5030, 0.6082, 0.7100, 0.7932,
	0.8620, 0.9149, 0.9540, 0.9803, 0.9950, 1.0000, 0.9950, 0.9786, 0.9520, 0.9154,
	0.8700, 0.8163, 0.7570, 0.6949, 0.6310, 0.5668, 0.5030, 0.4412, 0.3810, 0.3210,
	0.2650, 0.2170, 0.1750, 0.1382, 0.1070, 0.0816, 0.0610, 0.0446, 0.0320, 0.0232,
	0.0170, 0.0119, 0.0082, 0.0057, 0.0041, 0.0029, 0.0021, 0.0015, 0.0010, 0.0007,
	0.0005, 0.0004, 0.0002, 0.0002, 0.0001, 0.0001, 0.0001, 0.0000, 0.0000, 0.0000,
	0.0000
};
static const double kCIE_z[ kNLambda ] = {
	0.0065, 0.0105, 0.0201, 0.0362, 0.0679, 0.1102, 0.2074, 0.3713, 0.6456, 1.0391,
	1.3856, 1.6230, 1.7471, 1.7826, 1.7721, 1.7441, 1.6692, 1.5281, 1.2876, 1.0419,
	0.8130, 0.6162, 0.4652, 0.3533, 0.2720, 0.2123, 0.1582, 0.1117, 0.0782, 0.0573,
	0.0422, 0.0298, 0.0203, 0.0134, 0.0087, 0.0057, 0.0039, 0.0027, 0.0021, 0.0018,
	0.0017, 0.0014, 0.0011, 0.0010, 0.0008, 0.0006, 0.0003, 0.0002, 0.0002, 0.0001,
	0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000,
	0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000,
	0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000,
	0.0000
};

// XYZ(D50) → ROMM RGB(D50).  Copied from src/Library/Utilities/Color/Color.cpp's
// `mxXYZD50toROMM`.  The LUT generator targets ROMM RGB so its output is
// directly consumable by RISE painters which return ROMMRGBPel.
//
// Why ROMM and not sRGB: RISE's RISEPel typedef = ROMMRGBPel
// (Color.h:45).  Texture loading converts source-space (sRGB / Rec709 /
// ROMM) to ROMM linear at decode time; painters operate in ROMM.  An
// sRGB-targeted LUT couldn't represent saturated values that fit in the
// wider ROMM gamut.
static const double kXYZtoROMM[3][3] = {
	{  1.3460, -0.2556, -0.0511 },
	{ -0.5446,  1.5082,  0.0205 },
	{  0.0,     0.0,     1.2123 }
};

// Bradford D65 → D50 chromatic adaptation, copied from
// `mxXYZD65toXYZD50` in src/Library/Utilities/Color/Color.cpp.  The
// runtime XYZtoROMMRGB pipeline applies this BEFORE kXYZtoROMM
// because the standard CIE 1931 observer XYZ values are commonly
// referred to a D65 whitepoint, while ROMM's whitepoint is D50.
// Keep this matrix in sync with Color.cpp's mxXYZD65toXYZD50 (any
// drift would re-introduce the BioSpec-vs-JH conflict that this
// generator was reworked to resolve).
static const double kXYZD65toD50[3][3] = {
	{  1.0479, 0.0229, -0.0502 },
	{  0.0296, 0.9904, -0.0171 },
	{ -0.0092, 0.0151,  0.7519 }
};

// CIE Standard Illuminant D50 SPD at 5nm spacing, 380-780nm.
// Used as the reference illuminant for albedo upsampling: when we
// integrate S(λ) against the observer, we multiply by D50 first
// because the input RGB triple represents reflectance under D50
// illumination (ROMM's whitepoint).
//
// Source: CIE 015:2018 Table A.5 — published reference.  Sampled at
// 5nm; values normalized so D50(560nm) = 100.
static const double kD50[ kNLambda ] = {
	 23.94,  25.45,  26.96,  25.72,  24.49,  27.18,  29.87,  39.59,  49.31,  52.91,
	 56.51,  58.27,  60.03,  58.93,  57.82,  66.32,  74.82,  81.04,  87.25,  88.93,
	 90.61,  90.99,  91.37,  93.24,  95.11,  93.54,  91.96,  93.84,  95.72,  96.17,
	 96.61,  96.87,  97.13,  99.61, 102.10, 101.43, 100.75, 101.54, 102.32, 101.16,
	100.00,  98.87,  97.74,  98.33,  98.92,  96.21,  93.50,  95.59,  97.69, 98.48,
	 99.27, 99.155, 99.04,  97.38,  95.72,  97.29,  98.86,  97.26,  95.67,  96.93,
	 98.19, 100.60, 103.00, 101.07,  99.13,  93.26,  87.38,  89.49,  91.60,  92.25,
	 92.89,  84.87,  76.85,  81.68,  86.51,  89.55,  92.58,  85.40,  78.23,  67.96,
	 57.69
};

// Stable sigmoid form (PBRT-v4): avoids overflow for large |x|.
//   sigmoid(x) = 0.5 + x / (2*sqrt(1 + x²))
static inline double Sigmoid( double x ) {
	return 0.5 + x / (2.0 * std::sqrt(1.0 + x * x));
}

// Wavelength normalization used by both this generator AND the runtime
// RGBSigmoidPolynomial::Eval.  Maps [380, 780] nm → [0, 1] so the
// three sigmoid coefficients (c0, c1, c2) all have natural O(1)
// magnitude.  Without this, c0 (the coefficient of λ²) would be
// ~1e-6 at midband and finite-difference-Jacobian-based solvers
// blow up trying to perturb it on the same scale as c2.
//
// Both the LUT file and the runtime loader assume this exact
// normalization.  Changing the constants here REQUIRES regenerating
// the LUT.
static const double kLambdaScale = 1.0 / (kLambdaMax - kLambdaMin);	// 1/400
static const double kLambdaShift = double(kLambdaMin);

static inline double NormalizeLambda( double lambda ) {
	return (lambda - kLambdaShift) * kLambdaScale;
}

// Evaluate S(λ) = sigmoid(c0·λ̃² + c1·λ̃ + c2) at one wavelength.
//   λ̃ = (λ_nm - 380) / 400 ∈ [0, 1]
static inline double EvalSigmoid( const double c[3], double lambda ) {
	const double t = NormalizeLambda( lambda );
	return Sigmoid( c[0] * t * t + c[1] * t + c[2] );
}

// Integrate S(c, λ) · CIE_obs(λ) over the visible range under a FLAT
// illuminant and convert XYZ → ROMM RGB via the same XYZ→ROMM
// pipeline the runtime uses (`ColorUtils::XYZtoROMMRGB`): D65 → D50
// Bradford adapt followed by `kXYZtoROMM` matrix.  This is the
// forward model whose inverse we solve.  Returns 3-vector in ROMM
// RGB space.
//
// Why flat illuminant and not D65:
//   The runtime spectral integrator computes `∫ S · L · CIE dλ` where
//   L is whatever the scene's lights emit per wavelength — there is
//   no fixed reference illuminant baked into the integrator itself.
//   Inverting that forward model would require the LUT to know L at
//   training time.  We instead train under L=1 (flat) and rely on
//   the runtime to multiply by L per sample; the resulting LUT round-
//   trips ROMM→sigmoid→XYZ→ROMM exactly when L=1, and stays close
//   for natural-daylight L (which most scenes use).
//
// Why D65 → D50 adapt now (vs the matrix-only convention this file
// used through 2026-04):
//   The matrix-only convention required the runtime to also use a
//   matrix-only conversion — `IntegratorXYZtoROMMRGB` — at film
//   resolve, which works for JH-uplifted spectra but is WRONG for
//   physically-grounded spectra (BioSpec skin under blackbody@6500K
//   integrated D65-reference XYZ that, matrix-multiplied without
//   adapt, produces lavender skin).  The fix is to align the
//   generator with the runtime's standard XYZtoROMMRGB pipeline
//   (adapt + matrix) and let both JH and physical spectra resolve
//   through the same path.  See the user-facing chat session
//   "Option B" discussion for the full derivation.
static void IntegrateToROMM( const double c[3], double romm[3] )
{
	double X = 0.0, Y = 0.0, Z = 0.0;
	double Y_norm = 0.0;

	for( int i = 0; i < kNLambda; ++i ) {
		const double lambda = double(kLambdaMin) + i * double(kLambdaStep);
		const double s = EvalSigmoid( c, lambda );

		X += s * kCIE_x[i];
		Y += s * kCIE_y[i];
		Z += s * kCIE_z[i];
		Y_norm += kCIE_y[i];	// flat illuminant ∫ ȳ dλ
	}

	// Normalize so a uniform reflectance S=1 returns Y=1.
	const double inv = 1.0 / Y_norm;
	X *= inv;
	Y *= inv;
	Z *= inv;

	// Bradford D65 → D50 chromatic adaptation.  Matches the
	// `mxXYZD65toXYZD50` step in `ColorUtils::XYZtoROMMRGB` so the
	// LUT trained here is consumed by the standard runtime pipeline.
	const double Xd = kXYZD65toD50[0][0]*X + kXYZD65toD50[0][1]*Y + kXYZD65toD50[0][2]*Z;
	const double Yd = kXYZD65toD50[1][0]*X + kXYZD65toD50[1][1]*Y + kXYZD65toD50[1][2]*Z;
	const double Zd = kXYZD65toD50[2][0]*X + kXYZD65toD50[2][1]*Y + kXYZD65toD50[2][2]*Z;

	// XYZ (D50 reference) → ROMM RGB via kXYZtoROMM matrix.
	romm[0] = kXYZtoROMM[0][0] * Xd + kXYZtoROMM[0][1] * Yd + kXYZtoROMM[0][2] * Zd;
	romm[1] = kXYZtoROMM[1][0] * Xd + kXYZtoROMM[1][1] * Yd + kXYZtoROMM[1][2] * Zd;
	romm[2] = kXYZtoROMM[2][0] * Xd + kXYZtoROMM[2][1] * Yd + kXYZtoROMM[2][2] * Zd;
}

// Solve for sigmoid coefficients matching `target` (ROMM RGB).
// Returns true if final residual < kAcceptTol; final coefficients
// (best-so-far) written to c[3].  Returns false if the solver could
// not improve from the seed at all.
//
// Gauss-Newton with finite-difference Jacobian.  Initial guess from
// caller (zero by default; previous-cell coefficients when seeded by
// the grid walker for warm-start).  Line-search backtracking on
// rejected steps.  See Jakob & Hanika 2019 §3 for the derivation.
static bool SolveCoefficients( const double target[3], double c[3], double* outResNorm = nullptr )
{
	// Acceptance: residual under 1e-4 in ROMM RGB units is well below
	// any visible difference (8-bit display quantum is ~4e-3).  Tight
	// solver tolerance under that just wastes iterations on cells
	// that are fundamentally near-singular at the gamut extremes.
	const double kAcceptTol = 1e-4;
	const int    kMaxIter   = 200;
	const double kFDStep    = 5e-4;

	double bestC[3] = { c[0], c[1], c[2] };
	double bestResNorm = std::numeric_limits<double>::infinity();

	{
		double romm[3];
		IntegrateToROMM( c, romm );
		const double r[3] = {
			romm[0] - target[0],
			romm[1] - target[1],
			romm[2] - target[2]
		};
		bestResNorm = std::sqrt( r[0]*r[0] + r[1]*r[1] + r[2]*r[2] );
	}

	for( int iter = 0; iter < kMaxIter; ++iter ) {
		double romm[3];
		IntegrateToROMM( c, romm );
		const double r[3] = {
			romm[0] - target[0],
			romm[1] - target[1],
			romm[2] - target[2]
		};
		const double resNorm = std::sqrt( r[0]*r[0] + r[1]*r[1] + r[2]*r[2] );
		if( resNorm < bestResNorm ) {
			bestResNorm = resNorm;
			bestC[0] = c[0]; bestC[1] = c[1]; bestC[2] = c[2];
		}
		if( resNorm < kAcceptTol ) {
			c[0] = bestC[0]; c[1] = bestC[1]; c[2] = bestC[2];
			if( outResNorm ) *outResNorm = bestResNorm;
			return true;
		}

		// Build Jacobian J[i][j] = ∂r[i] / ∂c[j] via central differences.
		double J[3][3];
		for( int j = 0; j < 3; ++j ) {
			double cp[3] = { c[0], c[1], c[2] };
			double cm[3] = { c[0], c[1], c[2] };
			cp[j] += kFDStep;
			cm[j] -= kFDStep;

			double rp[3], rm[3];
			IntegrateToROMM( cp, rp );
			IntegrateToROMM( cm, rm );

			J[0][j] = (rp[0] - rm[0]) / (2.0 * kFDStep);
			J[1][j] = (rp[1] - rm[1]) / (2.0 * kFDStep);
			J[2][j] = (rp[2] - rm[2]) / (2.0 * kFDStep);
		}

		// Solve J·delta = r via 3x3 inverse with a Levenberg-Marquardt
		// damping term to handle near-singular Jacobians at the
		// gamut extremes (where the sigmoid saturates and ∂S/∂c → 0).
		// The damped system is (JᵀJ + λI)·δ = Jᵀr.
		double JtJ[3][3];
		double Jtr[3];
		for( int i = 0; i < 3; ++i ) {
			Jtr[i] = J[0][i] * r[0] + J[1][i] * r[1] + J[2][i] * r[2];
			for( int j = 0; j < 3; ++j ) {
				JtJ[i][j] = J[0][i] * J[0][j] + J[1][i] * J[1][j] + J[2][i] * J[2][j];
			}
		}
		const double lm_lambda = 1e-6 * std::max( { JtJ[0][0], JtJ[1][1], JtJ[2][2] } );
		JtJ[0][0] += lm_lambda;
		JtJ[1][1] += lm_lambda;
		JtJ[2][2] += lm_lambda;

		const double det =
			  JtJ[0][0] * (JtJ[1][1] * JtJ[2][2] - JtJ[1][2] * JtJ[2][1])
			- JtJ[0][1] * (JtJ[1][0] * JtJ[2][2] - JtJ[1][2] * JtJ[2][0])
			+ JtJ[0][2] * (JtJ[1][0] * JtJ[2][1] - JtJ[1][1] * JtJ[2][0]);
		if( std::fabs( det ) < 1e-40 ) {
			break;	// give up on this seed; keep best-so-far
		}
		const double inv_det = 1.0 / det;

		double Minv[3][3];
		Minv[0][0] =  (JtJ[1][1] * JtJ[2][2] - JtJ[1][2] * JtJ[2][1]) * inv_det;
		Minv[0][1] = -(JtJ[0][1] * JtJ[2][2] - JtJ[0][2] * JtJ[2][1]) * inv_det;
		Minv[0][2] =  (JtJ[0][1] * JtJ[1][2] - JtJ[0][2] * JtJ[1][1]) * inv_det;
		Minv[1][0] = -(JtJ[1][0] * JtJ[2][2] - JtJ[1][2] * JtJ[2][0]) * inv_det;
		Minv[1][1] =  (JtJ[0][0] * JtJ[2][2] - JtJ[0][2] * JtJ[2][0]) * inv_det;
		Minv[1][2] = -(JtJ[0][0] * JtJ[1][2] - JtJ[0][2] * JtJ[1][0]) * inv_det;
		Minv[2][0] =  (JtJ[1][0] * JtJ[2][1] - JtJ[1][1] * JtJ[2][0]) * inv_det;
		Minv[2][1] = -(JtJ[0][0] * JtJ[2][1] - JtJ[0][1] * JtJ[2][0]) * inv_det;
		Minv[2][2] =  (JtJ[0][0] * JtJ[1][1] - JtJ[0][1] * JtJ[1][0]) * inv_det;

		double delta[3];
		for( int i = 0; i < 3; ++i ) {
			delta[i] = Minv[i][0] * Jtr[0] + Minv[i][1] * Jtr[1] + Minv[i][2] * Jtr[2];
		}

		// Line-search: try full step first; if it doesn't improve the
		// residual, halve and retry.  10 halvings (factor 1024 down).
		double step = 1.0;
		double newC[3] = { c[0], c[1], c[2] };
		bool   improved = false;
		for( int ls = 0; ls < 10; ++ls ) {
			newC[0] = c[0] - step * delta[0];
			newC[1] = c[1] - step * delta[1];
			newC[2] = c[2] - step * delta[2];
			double newRomm[3];
			IntegrateToROMM( newC, newRomm );
			const double nr[3] = {
				newRomm[0] - target[0],
				newRomm[1] - target[1],
				newRomm[2] - target[2]
			};
			const double newResNorm = std::sqrt( nr[0]*nr[0] + nr[1]*nr[1] + nr[2]*nr[2] );
			if( newResNorm < resNorm ) {
				improved = true;
				c[0] = newC[0];
				c[1] = newC[1];
				c[2] = newC[2];
				if( newResNorm < bestResNorm ) {
					bestResNorm = newResNorm;
					bestC[0] = c[0]; bestC[1] = c[1]; bestC[2] = c[2];
				}
				break;
			}
			step *= 0.5;
		}
		if( !improved ) break;
	}

	c[0] = bestC[0]; c[1] = bestC[1]; c[2] = bestC[2];
	if( outResNorm ) *outResNorm = bestResNorm;
	return bestResNorm < kAcceptTol;
}

// Jakob-Hanika cell parameterisation: 3 sub-tables (one per max-channel
// index) each indexed by (i_z, i_x, i_y) where:
//   z = max(R, G, B)        ∈ [0, 1]
//   x = mid_axis_value / z  ∈ [0, 1]
//   y = min_axis_value / z  ∈ [0, 1]
//
// At each (i_max, i_z, i_x, i_y) we solve for (c0, c1, c2) such that
// integrating sigmoid(c0·λ² + c1·λ + c2) reproduces the cell's RGB.
//
// Layout in memory (little-endian binary):
//   maxChannel ∈ {0=R, 1=G, 2=B}
//   for i_z in [0, RES):
//     for i_x in [0, RES):
//       for i_y in [0, RES):
//         float c0, c1, c2
//
// Indexing: data[((maxChannel * RES + i_z) * RES + i_x) * RES + i_y][0..2]
//
// The z-axis uses a sin² warp: i_z=0 → z=0, i_z=RES-1 → z=1, with finer
// resolution near z=1 (where most natural-image colours cluster).
//   z(i) = sin²(π/2 · i/(RES-1))
// This matches PBRT-v4 / Mitsuba 3 convention and prevents the
// gamut-edge sigmoid coefficients from exploding for nearly-saturated
// hues that would otherwise map to a sparse linear-z region.
static double GridZ( int i, int res ) {
	if( res <= 1 ) return 0.0;
	const double t = double(i) / double(res - 1);
	const double s = std::sin( 0.5 * M_PI * t );
	return s * s;
}
static double GridLin( int i, int res ) {
	if( res <= 1 ) return 0.0;
	return double(i) / double(res - 1);
}

// Build the cell's target ROMM RGB triple from (maxChannel, z, x, y).
// max channel = z; the next two channels in canonical (max+1)%3,
// (max+2)%3 order receive x*z and y*z respectively.
static void CellToRGB( int maxChannel, double z, double x, double y, double rgb[3] )
{
	rgb[ maxChannel             ] = z;
	rgb[ (maxChannel + 1) % 3   ] = x * z;
	rgb[ (maxChannel + 2) % 3   ] = y * z;
}

struct CoeffSet {
	float c0, c1, c2;
};

}	// namespace JH

static void PrintUsage() {
	std::fprintf( stderr,
		"Usage: JakobHanikaLUTGen --output <path> [--resolution N]\n"
		"  --output       output binary path (e.g. extlib/jakob-hanika-luts/romm.coeff)\n"
		"  --resolution   grid resolution per axis (default 64)\n"
		"  --quick        validate convergence on a small subset (debug)\n" );
}

int main( int argc, char** argv )
{
	std::string outputPath;
	int         resolution = 64;
	bool        quick      = false;

	for( int i = 1; i < argc; ++i ) {
		const std::string a = argv[i];
		if( a == "--output" && i + 1 < argc ) {
			outputPath = argv[++i];
		} else if( a == "--resolution" && i + 1 < argc ) {
			resolution = std::atoi( argv[++i] );
		} else if( a == "--quick" ) {
			quick = true;
		} else if( a == "--help" || a == "-h" ) {
			PrintUsage();
			return 0;
		} else {
			std::fprintf( stderr, "Unknown argument: %s\n", a.c_str() );
			PrintUsage();
			return 1;
		}
	}

	if( outputPath.empty() ) {
		std::fprintf( stderr, "ERROR: --output is required.\n" );
		PrintUsage();
		return 1;
	}
	if( resolution < 8 || resolution > 256 ) {
		std::fprintf( stderr, "ERROR: --resolution out of range [8, 256]\n" );
		return 1;
	}

	const int RES = resolution;
	const size_t totalCells = size_t( 3 ) * RES * RES * RES;
	std::vector<JH::CoeffSet> coeffs( totalCells );

	std::printf(
		"JakobHanikaLUTGen: target=ROMM RGB (D50), resolution=%d, "
		"cells=%zu\n", RES, totalCells );

	// Iteration order: max channel outermost, z next, then x, y.
	// Convergence is helped by seeding from the previous (i_x-1) cell's
	// coefficients.
	int    failureCount = 0;
	int    cellsDone    = 0;
	int    nextProgress = 0;

	double maxResNorm = 0.0;
	double sumResNorm = 0.0;
	int    convergedCount = 0;

	for( int maxC = 0; maxC < 3; ++maxC ) {
		// Warm-start cache: at the start of each (maxC, iz) plane, the
		// solver tackles the "neutral" cell (ix=0, iy=0; rgb on the max
		// channel only) which is well-behaved.  Subsequent cells in the
		// plane use the previously-solved cell's coefficients as the
		// seed.  Carries across ix → ix+1 boundaries via savedRowSeed.
		for( int iz = 0; iz < RES; ++iz ) {
			const double z = JH::GridZ( iz, RES );
			double savedRowSeed[3] = { 0.0, 0.0, 0.0 };
			for( int ix = 0; ix < RES; ++ix ) {
				const double x = JH::GridLin( ix, RES );
				double prevCoeff[3] = {
					savedRowSeed[0], savedRowSeed[1], savedRowSeed[2]
				};
				for( int iy = 0; iy < RES; ++iy ) {
					const double y = JH::GridLin( iy, RES );
					double rgb[3];
					JH::CellToRGB( maxC, z, x, y, rgb );

					double c[3] = { prevCoeff[0], prevCoeff[1], prevCoeff[2] };
					double resNorm = std::numeric_limits<double>::infinity();

					// z = 0 means the target RGB is (0, 0, 0): the
					// fundamentally-unrepresentable "perfect black"
					// reflectance.  No sigmoid coefficient triple
					// converges; use a far-negative c2 so the runtime
					// produces near-zero spectrum, which is the
					// closest representable analogue.
					if( z < 1e-9 ) {
						c[0] = c[1] = 0.0;
						c[2] = -100.0;
						resNorm = 0.0;	// don't count black as a failure
					} else {
						JH::SolveCoefficients( rgb, c, &resNorm );
						if( resNorm > 1e-4 ) {
							// Retry from cold start
							double cAlt[3] = { 0.0, 0.0, 0.0 };
							double altRes  = std::numeric_limits<double>::infinity();
							JH::SolveCoefficients( rgb, cAlt, &altRes );
							if( altRes < resNorm ) {
								c[0] = cAlt[0]; c[1] = cAlt[1]; c[2] = cAlt[2];
								resNorm = altRes;
							}
						}
					}

					if( resNorm < 1e-4 ) {
						++convergedCount;
					} else {
						++failureCount;
						if( quick ) {
							std::printf( "    fail @ maxC=%d, z=%.3f, x=%.3f, y=%.3f, "
								"target=(%.3f, %.3f, %.3f), residual=%.3e, "
								"c=(%.3f, %.3f, %.3f)\n",
								maxC, z, x, y, rgb[0], rgb[1], rgb[2],
								resNorm, c[0], c[1], c[2] );
						}
					}
					maxResNorm = std::max( maxResNorm, resNorm );
					sumResNorm += resNorm;

					const size_t idx =
						size_t(((maxC * RES + iz) * RES + ix) * RES + iy);
					coeffs[idx].c0 = float(c[0]);
					coeffs[idx].c1 = float(c[1]);
					coeffs[idx].c2 = float(c[2]);

					prevCoeff[0] = c[0];
					prevCoeff[1] = c[1];
					prevCoeff[2] = c[2];

					// Save the iy=0 cell's coefficients as the seed for
					// the NEXT ix iteration's iy=0 starting point.  This
					// gives a 2D warm-start instead of resetting to (0,0,0)
					// at each new column.
					if( iy == 0 ) {
						savedRowSeed[0] = c[0];
						savedRowSeed[1] = c[1];
						savedRowSeed[2] = c[2];
					}

					++cellsDone;
					if( cellsDone >= nextProgress ) {
						const double pct = 100.0 * double(cellsDone) / double(totalCells);
						std::printf( "  progress: %5.1f%% (%d/%zu cells, %d failures, "
							"max_residual=%.3e)\n",
							pct, cellsDone, totalCells, failureCount, maxResNorm );
						std::fflush( stdout );
						nextProgress = cellsDone + int(totalCells / 20);
					}

					if( quick && cellsDone >= 256 ) goto write_output;
				}
			}
		}
	}

write_output:
	std::printf( "JakobHanikaLUTGen: done.  converged=%d, failures=%d of %d cells.\n",
		convergedCount, failureCount, cellsDone );
	std::printf( "  mean residual = %.3e, max residual = %.3e\n",
		sumResNorm / std::max( 1, cellsDone ), maxResNorm );
	if( failureCount > cellsDone / 100 ) {
		std::fprintf( stderr, "WARNING: > 1%% of cells failed convergence; "
			"LUT quality may be poor.  Investigate before shipping.\n" );
	}

	// Binary file format (little-endian):
	//   char     magic[4]      = "RJHL"
	//   uint32_t version       = 0x00010000
	//   uint32_t resolution    (RES per axis)
	//   uint32_t numChannels   = 3 (R, G, B sub-tables)
	//   uint32_t numCoeffs     = 3 (c0, c1, c2)
	//   float    coeffs[ 3 · RES³ · 3 ]
	std::FILE* fp = std::fopen( outputPath.c_str(), "wb" );
	if( !fp ) {
		std::fprintf( stderr, "ERROR: cannot open '%s' for writing\n", outputPath.c_str() );
		return 1;
	}

	const char     magic[4]   = { 'R', 'J', 'H', 'L' };
	const uint32_t version    = 0x00010000;
	const uint32_t res32      = uint32_t(RES);
	const uint32_t nChannels  = 3;
	const uint32_t nCoeffs    = 3;

	std::fwrite( magic,        1, 4,                fp );
	std::fwrite( &version,     4, 1,                fp );
	std::fwrite( &res32,       4, 1,                fp );
	std::fwrite( &nChannels,   4, 1,                fp );
	std::fwrite( &nCoeffs,     4, 1,                fp );
	std::fwrite( coeffs.data(), sizeof(JH::CoeffSet), totalCells, fp );
	std::fclose( fp );

	std::printf( "JakobHanikaLUTGen: wrote %s (%zu bytes)\n",
		outputPath.c_str(),
		size_t(20) + totalCells * sizeof(JH::CoeffSet) );

	return failureCount == 0 ? 0 : 2;
}
