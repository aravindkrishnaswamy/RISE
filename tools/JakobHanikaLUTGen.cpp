//////////////////////////////////////////////////////////////////////
//
//  JakobHanikaLUTGen.cpp - Offline tool to generate the Jakob-Hanika
//    2019 sigmoid-uplift LUT for Landing 3 (RGB → spectrum upsampling).
//
//    Solves, per cell of a 64×64×64 RGB grid (for each of 3 max-
//    channel sub-tables):
//
//      Find (c0, c1, c2) such that the REFLECTANCE
//        S(λ) = sigmoid(c0·λ̃² + c1·λ̃ + c2)
//      VIEWED UNDER THE TARGET'S REFERENCE ILLUMINANT I(λ),
//
//        rgb = M_XYZ→RGB · ( ∫ S(λ)·I(λ)·cmf(λ) dλ )
//                          / ( ∫ I(λ)·ȳ(λ) dλ )
//
//      reproduces the cell's RGB triple.  (Stage C, 2026-09-02 —
//      see docs/SPECTRAL_ILLUMINANT_CONVENTION.md.)
//
//    Algorithm: Gauss-Newton with finite-difference Jacobian and
//    Levenberg-Marquardt damping, line-search backtracking on
//    rejected steps.  Jakob & Hanika 2019 §3.
//
//    --target <rec709|romm|acescg> selects:
//      * the target RGB space's XYZ→RGB matrix (in the target's
//        whitepoint reference frame);
//      * the target's REFERENCE ILLUMINANT SPD (D65 for rec709).
//      Because the forward model integrates under the target's own
//      whitepoint SPD, the resulting XYZ is already in the target's
//      whitepoint reference frame — there is NO chromatic-adaptation
//      step any more (Stage C removed the Bradford stage; see
//      IntegrateToTarget's comment).  A target without a reference
//      SPD in this file is REFUSED rather than silently trained
//      under the wrong whitepoint.
//
//    --output is the binary `.coeff` path.  Companion script
//    tools/GenerateSpectrumLUTHeader.py bakes the binary into the
//    source tree as RGBToSpectrumTable_LUTData.cpp.
//
//    Build (from project root):
//      c++ -O3 -std=c++17 -o bin/tools/JakobHanikaLUTGen \
//          tools/JakobHanikaLUTGen.cpp -lm
//    On Windows the VS2022 project file builds the same source.
//
//    Run:
//      bin/tools/JakobHanikaLUTGen \
//        --target rec709 \
//        --output extlib/jakob-hanika-luts/rec709.coeff \
//        --resolution 64
//
//    Author: Aravind Krishnaswamy
//    Date: 2026-05-08 (parameterized 2026-05-24)
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

// CIE Standard Illuminant D65 SPD at 5nm spacing, 380-780nm (81
// entries), normalized so D65(560nm) = 100.  Source: CIE 015:2018
// Table A.1.
//
// Copied verbatim from `kD65` in
// src/Library/Utilities/Color/RGBSpectra.cpp so the LUT generator's
// forward model and the runtime's RGBIlluminantSpectrum reference the
// SAME SPD by construction.  This tool is deliberately a standalone
// single file (built with a bare `c++ -O3 -std=c++17`, no RISE
// headers), hence the duplication — keep the two tables in sync.
//
// The grid MATCHES the CMF grid above exactly (380-780 nm, 5 nm step,
// 81 samples), so IntegrateToTarget can index them in lockstep with no
// resampling.  A future SPD on a different grid MUST be resampled onto
// this one before use.
static const double kD65[ kNLambda ] = {
	 49.9755,  52.3118,  54.6482,  68.7015,  82.7549,  87.1204,  91.4860,  92.4589,  93.4318,  90.0570,
	 86.6823,  95.7736, 104.8650, 110.9360, 117.0080, 117.4100, 117.8120, 116.3360, 114.8610, 115.3920,
	115.9230, 112.3670, 108.8110, 109.0820, 109.3540, 108.5780, 107.8020, 106.2960, 104.7900, 106.2390,
	107.6890, 106.0470, 104.4050, 104.2250, 104.0460, 102.0230, 100.0000,  98.1671,  96.3342,  96.0611,
	 95.7880,  92.2368,  88.6856,  89.3459,  90.0062,  89.8026,  89.5991,  88.6489,  87.6987,  85.4936,
	 83.2886,  83.4939,  83.6992,  81.8630,  80.0268,  80.1207,  80.2146,  81.2462,  82.2778,  80.2810,
	 78.2842,  74.0027,  69.7213,  70.6652,  71.6091,  72.9790,  74.3490,  67.9765,  61.6040,  65.7448,
	 69.8856,  72.4863,  75.0870,  69.3398,  63.5927,  55.0054,  46.4182,  56.6118,  66.8054,  65.0941,
	 63.3828
};

// ─────────────────────────────────────────────────────────────────
// Per-target colourspace tables.  Each Target supplies:
//   * mxXYZtoRGB[3][3]  — XYZ→RGB matrix in the target's whitepoint
//                         reference frame (e.g. XYZ(D65)→Rec709(D65),
//                         XYZ(D50)→ROMM(D50)).
//   * refIlluminant     — the target whitepoint's relative SPD,
//                         sampled on the CMF grid (kNLambda entries).
//                         nullptr = this target is NOT trainable and
//                         --target refuses it (see main()).
//
// Stage C (2026-09-02) removed the former `mxXYZD65toTW` Bradford
// stage.  Under the old flat-E forward model the CMF integral landed
// in a "flat-E-referred" frame that had to be adapted to the target's
// whitepoint; now the integral is taken under the target's OWN
// reference illuminant, so the resulting XYZ is already in that
// whitepoint's reference frame and any adaptation would be a second,
// erroneous whitepoint shift.  A new target therefore needs its
// whitepoint SPD, not a Bradford matrix.
//
// Adding a new target = add one entry to kAllTargets[] with the
// published XYZ→RGB constants, its reference SPD resampled onto the
// CMF grid, and a unique --target name string.
// ─────────────────────────────────────────────────────────────────

struct TargetSpec {
	const char*   name;
	const char*   description;
	double        mxXYZtoRGB[3][3];    // in target whitepoint reference frame
	const double* refIlluminant;       // kNLambda samples, or nullptr if unsupported
};

// Rec.709 / sRGB Linear (D65).  XYZ(D65)→Rec709(D65).  Copied from
// `mxXYZtoRec709` in src/Library/Utilities/Color/Color.cpp.
// Reference illuminant D65 — matches the space's whitepoint AND the
// runtime's RGBIlluminantSpectrum reference SPD.
static const TargetSpec kTarget_Rec709 = {
	"rec709",
	"Rec.709 / sRGB Linear (D65), reference illuminant D65.  Primaries "
	"inside the spectral locus; the white/grey axis is exactly "
	"representable (flat S=c integrates to (c,c,c) under D65).  Matches "
	"modern PBR pipelines and PBRT-v4's convention.",
	{
		{  3.240479, -1.537150, -0.498535 },
		{ -0.969256,  1.875992,  0.041556 },
		{  0.055648, -0.204043,  1.057311 }
	},
	kD65
};

// ROMM RGB Linear (D50).  XYZ(D50)→ROMM(D50).  Copied from
// `mxXYZD50toROMM` in src/Library/Utilities/Color/Color.cpp.
// Legacy default pre-2026-05; primaries OUTSIDE the spectral locus →
// ~22 % gamut-corner failures.
//
// UNSUPPORTED since Stage C: training it needs the D50 relative SPD
// on the CMF grid, which this standalone file does not carry (RISE has
// no vetted D50 table to copy from, and reconstructing it from the CIE
// daylight S0/S1/S2 basis would introduce unverified constants).  Left
// listed so the refusal message is specific.
static const TargetSpec kTarget_ROMM = {
	"romm",
	"ROMM RGB Linear / ProPhoto (D50).  UNSUPPORTED since Stage C: needs "
	"the CIE D50 relative SPD on the 380-780/5nm grid (not carried here). "
	"Also structurally poor: G & B primaries outside the spectral locus → "
	"~22 % gamut-corner failures (see docs/JH_LUT_GAMUT.md).  Legacy.",
	{
		{  1.3460, -0.2556, -0.0511 },
		{ -0.5446,  1.5082,  0.0205 },
		{  0.0,     0.0,     1.2123 }
	},
	nullptr
};

// ACES AP1 (a.k.a. ACEScg).  XYZ(D60-ish)→AP1(D60-ish).  Source:
// AMPAS S-2014-004 (ACES Reference Image Capture Specification).
// Pre-staged so a future migration to AP1 is `--target acescg`.
//
// UNSUPPORTED since Stage C: needs the "ACES white" (~D60,
// xy = 0.32168, 0.33767) relative SPD on the CMF grid.  That is a
// daylight-locus reconstruction, not a published table RISE carries.
static const TargetSpec kTarget_ACEScg = {
	"acescg",
	"ACES AP1 / ACEScg (D60-ish).  UNSUPPORTED since Stage C: needs the "
	"ACES-white (~D60) relative SPD on the 380-780/5nm grid (not carried "
	"here).  Wide gamut, primaries inside the spectral locus; industry "
	"VFX standard.",
	{
		{  1.6410233797, -0.3248032942, -0.2364246952 },
		{ -0.6636628587,  1.6153315917,  0.0167563477 },
		{  0.0117218943, -0.0082844420,  0.9883948585 }
	},
	nullptr
};

static const TargetSpec* const kAllTargets[] = {
	&kTarget_Rec709, &kTarget_ROMM, &kTarget_ACEScg
};
static const int kNumTargets = int( sizeof(kAllTargets) / sizeof(kAllTargets[0]) );

// The currently active target.  Set by main() based on --target.
static const TargetSpec* gTarget = nullptr;

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

// Forward model: what RGB does the reflectance S(c, ·) produce when
// VIEWED UNDER the target's reference illuminant I and resolved by the
// film?
//
//   rgb = M_XYZ→RGB · ( ∫ S(λ)·I(λ)·cmf(λ) dλ ) / ( ∫ I(λ)·ȳ(λ) dλ )
//
// Why the reference illuminant is IN the model (Stage C, 2026-09-02):
//   The prior comment here argued for a FLAT (E) illuminant on the
//   grounds that "the runtime multiplies by whatever L the lights
//   emit, so the LUT must not bake one in".  That reasoning is wrong,
//   and it was the bug.  The LUT does not invert the runtime's
//   integral; it defines what a REFLECTANCE MEANS.  An RGB albedo is
//   authored as "what this surface looks like under white light" —
//   that statement is only well-posed once you say which white.  Under
//   flat E the CMFs give ΣX̄ = Σȳ = Σz̄, so a flat spectrum resolves to
//   rgb ≈ (1.2048, 0.9483, 0.9089) — NOT neutral.  The solver could
//   then only reach the white cell by fitting a NON-flat sigmoid whose
//   integral has D65 chromaticity, and since S ≤ 1 the only way to do
//   that is to SUPPRESS RED: authored white collapsed to 1.28e-5 at
//   660 nm, and every grey uplifted red-poor (0.5 grey → 0.36 at
//   660 nm), compounding ~19 % of the red channel per throughput
//   multiply in spectral renders.
//
//   Putting I in the model makes a flat S = c resolve to exactly
//   (c, c, c) (verified: S=1 → (0.99991, 1.00002, 1.00005), i.e. 9e-5
//   from neutral, the CMF/matrix rounding floor), so white is
//   representable at the sigmoid's asymptote and greys are flat.  This
//   is PBRT-v4's convention (RGBAlbedoSpectrum trained per
//   RGBColorSpace::illuminant).  The runtime is unchanged: light
//   SOURCES carry the illuminant shape (RGBIlluminantSpectrum =
//   sigmoid × D65), the film integrates radiance × CMF with a uniform
//   Y normalisation and applies the un-adapted D65 matrix, so a white
//   light on a white wall lands on (1, 1, 1).
//
// Why there is no chromatic-adaptation stage any more:
//   Integrating under I yields XYZ already referred to I's whitepoint,
//   which IS the target's whitepoint.  The old Bradford step existed
//   only to move the flat-E integral into that frame; applying one now
//   would be a second, erroneous whitepoint shift.  Targets whose
//   whitepoint ≠ D65 are handled by supplying THEIR SPD in
//   TargetSpec::refIlluminant, not by adapting a D65 integral.
static void IntegrateToTarget( const double c[3], double rgb[3] )
{
	const double* const I = gTarget->refIlluminant;

	double X = 0.0, Y = 0.0, Z = 0.0;
	double Y_norm = 0.0;

	for( int i = 0; i < kNLambda; ++i ) {
		const double lambda = double(kLambdaMin) + i * double(kLambdaStep);
		const double s  = EvalSigmoid( c, lambda );
		const double si = s * I[i];

		X += si * kCIE_x[i];
		Y += si * kCIE_y[i];
		Z += si * kCIE_z[i];
		Y_norm += I[i] * kCIE_y[i];	// ∫ I · ȳ dλ
	}

	// Normalize so a uniform reflectance S=1 returns Y=1 (a perfect
	// white diffuser under the reference illuminant).
	const double inv = 1.0 / Y_norm;
	X *= inv;
	Y *= inv;
	Z *= inv;

	// XYZ (already in the target's whitepoint reference frame) →
	// target RGB via the per-target matrix.
	rgb[0] = gTarget->mxXYZtoRGB[0][0] * X + gTarget->mxXYZtoRGB[0][1] * Y + gTarget->mxXYZtoRGB[0][2] * Z;
	rgb[1] = gTarget->mxXYZtoRGB[1][0] * X + gTarget->mxXYZtoRGB[1][1] * Y + gTarget->mxXYZtoRGB[1][2] * Z;
	rgb[2] = gTarget->mxXYZtoRGB[2][0] * X + gTarget->mxXYZtoRGB[2][1] * Y + gTarget->mxXYZtoRGB[2][2] * Z;
}

// Solve for sigmoid coefficients matching `target` (in target RGB).
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
	// Acceptance: residual under 1e-4 in target-RGB units is well below
	// any visible difference (8-bit display quantum is ~4e-3).  Tight
	// solver tolerance under that just wastes iterations on cells
	// that are fundamentally near-singular at the gamut extremes.
	const double kAcceptTol = 1e-4;
	const int    kMaxIter   = 200;
	const double kFDStep    = 5e-4;

	double bestC[3] = { c[0], c[1], c[2] };
	double bestResNorm = std::numeric_limits<double>::max();

	{
		double rgb[3];
		IntegrateToTarget( c, rgb );
		const double r[3] = {
			rgb[0] - target[0],
			rgb[1] - target[1],
			rgb[2] - target[2]
		};
		bestResNorm = std::sqrt( r[0]*r[0] + r[1]*r[1] + r[2]*r[2] );
	}

	for( int iter = 0; iter < kMaxIter; ++iter ) {
		double rgb[3];
		IntegrateToTarget( c, rgb );
		const double r[3] = {
			rgb[0] - target[0],
			rgb[1] - target[1],
			rgb[2] - target[2]
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
			IntegrateToTarget( cp, rp );
			IntegrateToTarget( cm, rm );

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
			double newRgb[3];
			IntegrateToTarget( newC, newRgb );
			const double nr[3] = {
				newRgb[0] - target[0],
				newRgb[1] - target[1],
				newRgb[2] - target[2]
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

// Build the cell's target RGB triple from (maxChannel, z, x, y).
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
		"Usage: JakobHanikaLUTGen --output <path> [--target NAME] [--resolution N]\n"
		"  --target       target colour space (default 'rec709'). Choices:\n" );
	for( int i = 0; i < JH::kNumTargets; ++i ) {
		std::fprintf( stderr, "                   %-8s  %s\n",
			JH::kAllTargets[i]->name, JH::kAllTargets[i]->description );
	}
	std::fprintf( stderr,
		"  --output       output binary path (e.g. extlib/jakob-hanika-luts/rec709.coeff)\n"
		"  --resolution   grid resolution per axis (default 64)\n"
		"  --quick        validate convergence on a small subset (debug)\n" );
}

int main( int argc, char** argv )
{
	std::string outputPath;
	std::string targetName = "rec709";	// default after 2026-05 colour-space migration
	int         resolution = 64;
	bool        quick      = false;

	for( int i = 1; i < argc; ++i ) {
		const std::string a = argv[i];
		if( a == "--output" && i + 1 < argc ) {
			outputPath = argv[++i];
		} else if( a == "--target" && i + 1 < argc ) {
			targetName = argv[++i];
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

	// Resolve target name → table.
	for( int i = 0; i < JH::kNumTargets; ++i ) {
		if( targetName == JH::kAllTargets[i]->name ) {
			JH::gTarget = JH::kAllTargets[i];
			break;
		}
	}
	if( !JH::gTarget ) {
		std::fprintf( stderr, "ERROR: unknown --target '%s'\n", targetName.c_str() );
		PrintUsage();
		return 1;
	}
	if( !JH::gTarget->refIlluminant ) {
		// Stage C: the forward model integrates under the target's own
		// reference illuminant.  Training a target without one would
		// silently reuse another whitepoint's SPD and bake a chromatic
		// error into every uplifted colour — refuse instead.
		std::fprintf( stderr,
			"ERROR: --target '%s' has no reference illuminant SPD in this tool, so it\n"
			"       cannot be trained under the Stage C forward model\n"
			"       (rgb = M · integral(S*I*cmf) / integral(I*ybar); see\n"
			"       docs/SPECTRAL_ILLUMINANT_CONVENTION.md).\n"
			"       To enable it: add that whitepoint's relative SPD, resampled onto\n"
			"       the %d-%d nm / %d nm CMF grid (%d samples), as a\n"
			"       `static const double kD??[kNLambda]` table next to kD65 and point\n"
			"       the target's `refIlluminant` at it.  Only 'rec709' (D65) ships one.\n",
			JH::gTarget->name,
			JH::kLambdaMin, JH::kLambdaMax, JH::kLambdaStep, JH::kNLambda );
		return 1;
	}

	const int RES = resolution;
	const size_t totalCells = size_t( 3 ) * RES * RES * RES;
	std::vector<JH::CoeffSet> coeffs( totalCells );

	std::printf(
		"JakobHanikaLUTGen: target=%s, resolution=%d, cells=%zu\n",
		JH::gTarget->name, RES, totalCells );

	// Iteration order: max channel outermost, z next, then x, y.
	// Convergence is helped by seeding from the previous (i_x-1) cell's
	// coefficients.
	int    failureCount = 0;
	int    cellsDone    = 0;
	int    nextProgress = 0;

	double maxResNorm = 0.0;
	double sumResNorm = 0.0;
	int    convergedCount = 0;

	// Failure clustering, printed at the end: counts per (max channel,
	// z band) where the bands are z < 1/3, z < 2/3, z >= 2/3.  Lets a
	// retrain see WHERE the residual failures live without a separate
	// diagnostic build (the analysis in docs/JH_LUT_GAMUT.md used a
	// since-reverted --residuals CSV mode).  Also remembers the single
	// worst cell's target RGB.
	int    failHist[3][3]  = { {0,0,0}, {0,0,0}, {0,0,0} };
	int    totalHist[3][3] = { {0,0,0}, {0,0,0}, {0,0,0} };
	double worstRGB[3]     = { 0.0, 0.0, 0.0 };

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
					double resNorm = std::numeric_limits<double>::max();

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
							double altRes  = std::numeric_limits<double>::max();
							JH::SolveCoefficients( rgb, cAlt, &altRes );
							if( altRes < resNorm ) {
								c[0] = cAlt[0]; c[1] = cAlt[1]; c[2] = cAlt[2];
								resNorm = altRes;
							}
						}
					}

					const int zBand = ( z < 1.0/3.0 ) ? 0 : ( z < 2.0/3.0 ? 1 : 2 );
					++totalHist[maxC][zBand];

					if( resNorm < 1e-4 ) {
						++convergedCount;
					} else {
						++failureCount;
						++failHist[maxC][zBand];
						if( quick ) {
							std::printf( "    fail @ maxC=%d, z=%.3f, x=%.3f, y=%.3f, "
								"target=(%.3f, %.3f, %.3f), residual=%.3e, "
								"c=(%.3f, %.3f, %.3f)\n",
								maxC, z, x, y, rgb[0], rgb[1], rgb[2],
								resNorm, c[0], c[1], c[2] );
						}
					}
					if( resNorm > maxResNorm ) {
						maxResNorm = resNorm;
						worstRGB[0] = rgb[0]; worstRGB[1] = rgb[1]; worstRGB[2] = rgb[2];
					}
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
	std::printf( "  mean residual = %.3e, max residual = %.3e (worst cell rgb = %.3f, %.3f, %.3f)\n",
		sumResNorm / std::max( 1, cellsDone ), maxResNorm,
		worstRGB[0], worstRGB[1], worstRGB[2] );
	{
		static const char* kBandName[3] = { "z<1/3 ", "z<2/3 ", "z>=2/3" };
		static const char* kChanName[3] = { "maxC=R", "maxC=G", "maxC=B" };
		std::printf( "  failure clustering (failed / total, %% of cells):\n" );
		for( int mc = 0; mc < 3; ++mc ) {
			for( int b = 0; b < 3; ++b ) {
				if( totalHist[mc][b] == 0 ) continue;
				std::printf( "    %s %s : %7d / %7d = %5.2f%%\n",
					kChanName[mc], kBandName[b],
					failHist[mc][b], totalHist[mc][b],
					100.0 * double(failHist[mc][b]) / double(totalHist[mc][b]) );
			}
		}
	}
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
