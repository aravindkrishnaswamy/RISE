//////////////////////////////////////////////////////////////////////
//
//  OrenNayarHemisphericalAlbedoGen.cpp - Offline tool that bakes the
//    A1(sigma)/A2(sigma) bihemispherical-albedo tables consumed by
//    src/Library/Materials/OrenNayarBRDF.cpp's
//    hemisphericalAlbedo{,NM} (debt ledger DL-07,
//    docs/DEBT_LEDGER.md, CLOTH_FABRIC_DESIGN.md section 15 item 17 /
//    WETNESS_COAT_DESIGN.md section 12 item 13).
//
//  WHAT IS BAKED, AND WHY IT IS A 1-D TABLE
//  -----------------------------------------------------------------
//  OrenNayarBRDF's full (C1/C2/C3 + L2) BRDF is
//
//      f(wi,wo) = (L1/pi)*rho + (L2/pi)*rho^2
//
//  where L1, L2 depend only on (theta_i, theta_o, deltaPhi, sigma) --
//  never on rho.  `hemisphericalAlbedo` is IBSDF's "reflectance under
//  a UNIFORM incident field" (see IBSDF.h's doc comment: the `ri`
//  parameter is for painter sampling ONLY, `ri.ray` must not be read),
//  i.e. the BIHEMISPHERICAL (white-sky) albedo
//
//      R_bi = (1/pi) * INT_H R(wi) (n.wi) dwi
//           = (1/pi) * INT_H INT_H f(wi,wo) (n.wi)(n.wo) dwi dwo
//           = rho   * (1/pi^2) INT INT L1 (n.wi)(n.wo) dwi dwo
//           + rho^2 * (1/pi^2) INT INT L2 (n.wi)(n.wo) dwi dwo
//           = rho * A1(sigma) + rho^2 * A2(sigma)
//
//  So A1, A2 are pure functions of roughness alone (rho and the
//  incident/outgoing directions integrate out completely) -- a small
//  1-D table, exactly the same architectural shape as
//  SheenDirectionalAlbedo.h's kEHatMeanTable, not a 2-D one.
//
//  THE L1/L2 FORMULAS ARE TRANSCRIBED FROM
//  src/Library/Materials/OrenNayarBRDF.cpp's ComputeFactor (lines
//  89-107 as of this tool's authoring) -- copied rather than
//  #included so this tool stays a standalone, dependency-free
//  translation unit (same choice GenerateMicrofacetEnergyLUT.cpp
//  made, for the same reason: RayIntersectionGeometric's transitive
//  includes are not worth pulling into a bake tool).  If
//  ComputeFactor's algebra ever changes, this transcription AND the
//  regression test's independent copy (tests/OrenNayarHemisphericalAlbedoTest.cpp)
//  must both be re-derived and the table regenerated.
//
//  QUADRATURE
//  -----------------------------------------------------------------
//  Reduction used (exact, not an approximation): the double
//  hemisphere integral over (wi, wo) depends on wi and wo only
//  through (theta_i, theta_o, deltaPhi = phi_o - phi_i), so the
//  redundant common-azimuth integral factors out as an exact 2*pi,
//  leaving a 3-D integral in (mu_i = cos theta_i, mu_o = cos theta_o,
//  deltaPhi):
//
//      A(sigma) = (2/pi) * INT_0^1 INT_0^1 INT_0^{2pi}
//                   L(mu_i, mu_o, deltaPhi; sigma) * mu_i * mu_o
//                   dmu_i dmu_o dDeltaPhi
//
//  Evaluated by plain midpoint quadrature at kBakeNmu x kBakeNmu x
//  kBakeNphi -- no Monte Carlo, deterministic, byte-identical across
//  re-runs.  Convergence measured (scratch probe, not shipped):
//  Nmu=256/Nphi=512 agrees with Nmu=800/Nphi=1600 to 6 significant
//  figures at every sigma in [0,3], and the resulting 64-node table
//  (see AXIS below) interpolates the converged curve to within
//  2.3e-4 absolute (measured by an independent 300-point re-sampling
//  in a scratch probe) -- comfortably under the 2% DL-07 asked for
//  and the 0.5% this fix actually gates at.
//
//  AXIS: kNumSigmaBins nodes, POWER-2 WARPED on [0, kSigmaMax]:
//      sigma_i = kSigmaMax * (i/(N-1))^2
//  i.e. uniform in sqrt(sigma) -- inverse position is
//  sqrt(sigma/kSigmaMax)*(N-1), the same "warp the axis toward where
//  the curvature is" idiom SheenDirectionalAlbedo.h's cosTheta axis
//  uses (there for the opposite reason: grazing, not origin).  A1/A2
//  vary fastest for sigma in roughly [0.05, 0.5] because the model's
//  C1/C2/C3 coefficients are rational in sigma^2 = sqr_r and their
//  slope-in-sigma peaks there, not at sigma=0; a plain uniform-sigma
//  table needed 4x the nodes for the same worst-case error, and a
//  uniform-in-sigma^2 table was WORSE than uniform-in-sigma (measured
//  0.03-0.07 absolute error at N=32) because it under-resolves that
//  same peak-slope band. Both alternatives were measured in a scratch
//  probe before choosing this warp; see the DL-07 fix commit message
//  for the comparison table.
//
//  Beyond kSigmaMax the model plateaus (A1+A2 measured 0.6978 at
//  sigma=50 vs 0.7127 at sigma=3, a 2% residual drift) -- the runtime
//  lookup clamps to the table's last node (constant extrapolation),
//  which is the table's STATED DOMAIN, not a clamp of convenience;
//  scenes authoring sigma > 3 (unusual -- Oren-Nayar roughness is a
//  small-angle-variance parameter) get the sigma=3 value.
//
//  Build (from project root):
//      c++ -O3 -std=c++17 -o /tmp/OrenNayarHemisphericalAlbedoGen \
//          tools/OrenNayarHemisphericalAlbedoGen.cpp
//  Run:
//      /tmp/OrenNayarHemisphericalAlbedoGen
//  and paste the two printed `static const float` arrays into
//  src/Library/Materials/OrenNayarBRDF.cpp's anonymous namespace,
//  replacing the previous ones verbatim (the surrounding lookup code
//  is hand-written, not generated, so this tool does not overwrite a
//  whole file the way GenerateMicrofacetEnergyLUT.cpp does).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cmath>

static const double PI = 3.14159265358979323846;

static inline double r_max( double a, double b ) { return a > b ? a : b; }
static inline double r_min( double a, double b ) { return a < b ? a : b; }

// Transcribed from OrenNayarBRDF::ComputeFactor (OrenNayarBRDF.cpp
// lines 89-107).  muI = n.wi (light), muO = n.wo (view), cosPhiDiff =
// cos(phi_o - phi_i), sigma = roughness.  Both L1 and L2 are exactly
// what that function computes for a single (wi,wo) pair with rho
// factored out.
static void ComputeL1L2( double muI, double muO, double cosPhiDiff, double sigma, double& L1, double& L2 )
{
	const double theta_i = acos( r_min( 1.0, r_max( -1.0, muI ) ) );
	const double theta_r = acos( r_min( 1.0, r_max( -1.0, muO ) ) );
	const double alpha = r_max( theta_i, theta_r );
	const double beta  = r_min( theta_i, theta_r );
	const double sqr_r = sigma * sigma;

	const double C1 = 1.0 - 0.5 * ( sqr_r / ( sqr_r + 0.33 ) );
	const double C2 = 0.45 * ( sqr_r / ( sqr_r + 0.09 ) ) *
		( sin( alpha ) - ( ( cosPhiDiff >= 0 ) ? 0.0 : pow( 2.0 * beta / PI, 3.0 ) ) );
	const double t = ( 4.0 * alpha * beta / ( PI * PI ) );
	const double C3 = 0.125 * ( sqr_r / ( sqr_r + 0.09 ) ) * ( t * t );

	L1 = C1 + cosPhiDiff * C2 * tan( beta ) + ( 1.0 - fabs( cosPhiDiff ) ) * C3 * tan( ( alpha + beta ) / 2.0 );

	const double u = ( 2.0 * beta ) / PI;
	L2 = 0.17 * ( sqr_r / ( sqr_r + 0.13 ) ) * ( 1.0 - cosPhiDiff * ( u * u ) );
}

// A(sigma) = (2/pi) INT_0^1 INT_0^1 INT_0^{2pi} L(muI,muO,phi;sigma) muI muO dmuI dmuO dphi
// via plain midpoint quadrature -- see file header "QUADRATURE".
static void Bihemispherical( double sigma, int Nmu, int Nphi, double& A1, double& A2 )
{
	double sum1 = 0, sum2 = 0;
	for( int i = 0; i < Nmu; i++ ) {
		const double muI = ( i + 0.5 ) / Nmu;
		for( int k = 0; k < Nmu; k++ ) {
			const double muO = ( k + 0.5 ) / Nmu;
			for( int j = 0; j < Nphi; j++ ) {
				const double phi = 2.0 * PI * ( j + 0.5 ) / Nphi;
				const double cosPhi = cos( phi );
				double L1, L2;
				ComputeL1L2( muI, muO, cosPhi, sigma, L1, L2 );
				sum1 += L1 * muI * muO;
				sum2 += L2 * muI * muO;
			}
		}
	}
	const double dmu = 1.0 / Nmu;
	const double dphi = 2.0 * PI / Nphi;
	A1 = sum1 * dmu * dmu * dphi * ( 2.0 / PI );
	A2 = sum2 * dmu * dmu * dphi * ( 2.0 / PI );
}

int main()
{
	const int kNumSigmaBins = 64;
	const double kSigmaMax = 3.0;
	const int Nmu = 256;
	const int Nphi = 512;

	printf( "// Generated by tools/OrenNayarHemisphericalAlbedoGen.cpp -- see that\n" );
	printf( "// file's header for the derivation, quadrature and axis warp.  DO NOT\n" );
	printf( "// HAND-EDIT; re-run the generator and paste both arrays back in.\n" );
	printf( "// kNumSigmaBins=%d nodes, kSigmaMax=%.1f, sigma_i = kSigmaMax*(i/(N-1))^2,\n", kNumSigmaBins, kSigmaMax );
	printf( "// baked at Nmu=%d Nphi=%d midpoint quadrature.\n", Nmu, Nphi );

	double a1[64], a2[64];
	for( int i = 0; i < kNumSigmaBins; i++ ) {
		const double frac = (double)i / (double)( kNumSigmaBins - 1 );
		const double sigma = kSigmaMax * frac * frac;
		double A1, A2;
		Bihemispherical( sigma, Nmu, Nphi, A1, A2 );
		a1[i] = (float)A1;
		a2[i] = (float)A2;
	}

	printf( "static const float kOrenNayarA1Table[ kNumSigmaBins ] =\n{\n" );
	for( int i = 0; i < kNumSigmaBins; i++ ) {
		printf( "\t%.8ff,%s", a1[i], ( (i+1) % 4 == 0 ) ? "\n" : " " );
	}
	printf( "\n};\n\n" );

	printf( "static const float kOrenNayarA2Table[ kNumSigmaBins ] =\n{\n" );
	for( int i = 0; i < kNumSigmaBins; i++ ) {
		printf( "\t%.8ff,%s", a2[i], ( (i+1) % 4 == 0 ) ? "\n" : " " );
	}
	printf( "\n};\n\n" );

	// Spot-check printout matching the derivation's cross-reference
	// values, for a human reviewing the paste to sanity-check against.
	fprintf( stderr, "Spot checks (sigma, A1, A2, A1+A2):\n" );
	double checkSigmas[] = { 0.0, 0.1, 0.2, 0.3, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0 };
	for( double s : checkSigmas ) {
		double A1, A2;
		Bihemispherical( s, Nmu, Nphi, A1, A2 );
		fprintf( stderr, "  sigma=%.3f  A1=%.6f  A2=%.6f  sum=%.6f\n", s, A1, A2, A1 + A2 );
	}

	return 0;
}
