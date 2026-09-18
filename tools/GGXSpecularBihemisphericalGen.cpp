//////////////////////////////////////////////////////////////////////
//
//  GGXSpecularBihemisphericalGen.cpp - Offline tool that bakes the
//    quadrature-weight table consumed by
//    src/Library/Materials/GGXBRDF.cpp's hemisphericalAlbedo{,NM}
//    (debt ledger DL-123, docs/DEBT_LEDGER.md).
//
//  WHAT IS BAKED, AND WHY A SMALL FIXED-NODE QUADRATURE SUFFICES
//  -----------------------------------------------------------------
//  GGXBRDF::value's single-scatter specular term is
//
//      f_ss(wi,wo) = F(cosThetaH) * D(h)*G2(wi,wo) / (4*cosI*cosO)
//
//  where cosThetaH = dot(wo,h) = dot(wi,h) is the half-vector angle and
//  F is the interface Fresnel function (Schlick/conductor/thin-film),
//  a function of cosThetaH ALONE.  IBSDF's hemisphericalAlbedo contract
//  is the BIHEMISPHERICAL (white-sky) reflectance
//
//      R_ss = (1/pi) INT_H INT_H f_ss(wi,wo) (n.wi)(n.wo) dwi dwo
//           = (1/pi) INT_H INT_H F(cosThetaH) * D(h)*G2(wi,wo)/4 dwi dwo
//
//  (the (n.wi)(n.wo) factors cancel the f_ss denominator exactly, same
//  reduction OrenNayarHemisphericalAlbedoGen.cpp's header uses for its
//  own BRDF).  This can be written as a 1-D integral against a measure
//  in u = 1-cosThetaH alone:
//
//      R_ss = INT_0^1 F(1-u) * K(alpha, u) du
//
//  where K is a fixed (Fresnel-independent) kernel that depends only on
//  GGX geometry.  Rather than bake K itself, this tool bakes N+1
//  MOMENTS of K against the monomial basis {u^0, u^1, ..., u^N} and
//  then, via ONE alpha-independent linear solve, converts those moments
//  into per-node WEIGHTS w_i(alpha) at N+1 FIXED Chebyshev nodes u_i, so
//  that at runtime, for ANY Fresnel function F (Schlick, conductor,
//  thin-film -- F need not be known at bake time):
//
//      R_ss(alpha) ~= SUM_i F(1 - u_i) * w_i(alpha)
//
//  This is EXACT whenever F is a polynomial of degree <= N in u (in
//  particular it is algebraically EXACT, not merely a fit, for Schlick:
//  F(u) = F0 + (1-F0)*u^5 is degree 5, well inside N=7 below), and is a
//  degree-(N) polynomial-fit-quality approximation for conductor/thin-
//  film Fresnel curves, whose smooth, monotone shape over cosThetaH in
//  [0,1] is well within a degree-7 polynomial's reach (measured in
//  tests/GGXHemisphericalAlbedoTest.cpp).  This mirrors the same
//  "moment table -> closed-form combination" idiom
//  OrenNayarHemisphericalAlbedoGen.cpp uses for A1(sigma)/A2(sigma), and
//  the same "cheap fixed-node quadrature over a Fresnel function"
//  GGXInterfaceFresnel::Mean()/MicrofacetEnergyLUT::ComputeFresnelAvg
//  already use elsewhere in this file (GL_N=21 nodes there; this table
//  needs far fewer nodes because it is a BIHEMISPHERICAL, not
//  directional, quantity -- no dependence on an outgoing direction to
//  resolve).
//
//  NODE PLACEMENT: Chebyshev-Gauss-Lobatto nodes on [0,1],
//      u_i = 0.5*(1 - cos(pi*i/N)),  i = 0..N
//  clustered at both endpoints (u=0, i.e. cosThetaH=1, normal-incidence
//  half-vector; and u=1, i.e. cosThetaH=0, grazing half-vector) for a
//  well-conditioned degree-N Vandermonde solve (standard choice to avoid
//  Runge's phenomenon in polynomial interpolation).
//
//  MOMENT ESTIMATOR: a first cut at this generator used plain
//  deterministic midpoint quadrature over (mu_i,mu_o,deltaPhi), mirroring
//  OrenNayarHemisphericalAlbedoGen.cpp -- and it FAILED at low alpha
//  (measured moment_0 at alpha=0.01: 0.931 vs E_avg_TABLE_G2's 0.999,
//  a 7% deficit) because GGX's D is a sharply peaked function at low
//  roughness (angular width ~alpha) that a uniform angular grid at any
//  practical resolution cannot resolve.  Replaced with the SAME VNDF
//  (visible-normal-distribution) importance-sampling estimator
//  GenerateMicrofacetEnergyLUT.cpp's own E_ss/E_avg bake uses: for a
//  FIXED wi at polar angle theta_i, VNDF_Sample_Local(wi,alpha,u1,u2)
//  draws a microfacet normal m from the visible-normal distribution
//  D_visible(m|wi); m IS the half-vector h, so muH = dot(wi,m) falls out
//  of the sampler directly (no extra work), and reflecting wi about m
//  gives wo.  The height-correlated per-sample weight
//  G2(wi,wo)/G1(wi) (identical to the existing generator's own DL-63
//  `sumG2` line) turns the VNDF-sampled average of g(muH)*weight into an
//  unbiased estimator of INT_wo g(muH)*D*G2/(4*cosWi) dwo at FIXED wi --
//  this resolves the D peak automatically because VNDF sampling IS
//  importance sampling of D restricted to the visible microfacets, so a
//  low-alpha peak is sampled densely regardless of alpha.  The remaining
//  outer integral over wi (mu_i in [0,1], phi_i integrating out exactly
//  by isotropy) uses plain midpoint quadrature (NUM_MU_I bins), since
//  the alpha-independent 1/(1+cosWi) type smoothness there needs no
//  importance sampling.
//
//  CROSS-CHECK: moment 0 (u^0, i.e. F==1 identically) is EXACTLY the
//  bihemispherical albedo of the F=1 single-scatter BRDF, which is
//  E_avg_TABLE_G2's own definition (2*INT E_ss(alpha,mu)*mu dmu) --
//  algebraically the SAME quantity, computed by an estimator built the
//  same way (VNDF sampling, same per-sample G2/G1 weight) but through an
//  independent outer mu_i quadrature and independent RNG stream.
//  Printed to stderr for the paste-time reviewer; residual bake-vs-bake
//  disagreement additionally gated in
//  tests/GGXHemisphericalAlbedoTest.cpp.
//
//  ISOTROPIC ONLY.  For alphaX != alphaY, GGXBRDF.cpp evaluates this
//  table at alphaEff = sqrt(alphaX*alphaY) -- an approximation for the
//  single-scatter term specifically (the multiscatter term already uses
//  the exact anisotropic Eavg lookup, no approximation there).  Residual
//  tracked as a new debt row rather than extending this table to a
//  third (ratio) axis in this slice -- see the DL-123 closure entry.
//
//  PROVENANCE: this exact file reproduces byte-for-byte via the fixed
//  RNG seed baked into rng_state's initializer (1234567890123456789ULL,
//  the SAME constant GenerateMicrofacetEnergyLUT.cpp seeds with -- an
//  arbitrary but fixed choice, not a shared stream) with NUM_SAMPLES=
//  200000 samples per (alpha,mu_i) cell and NUM_MU_I=48 outer bins.
//
//  Build (from project root):
//      c++ -O3 -std=c++17 -o /tmp/GGXSpecularBihemisphericalGen \
//          tools/GGXSpecularBihemisphericalGen.cpp
//  Run:
//      /tmp/GGXSpecularBihemisphericalGen
//  and paste the printed `kGGXSpecularQuadNodes` / `kGGXSpecularQuadWeight`
//  arrays into src/Library/Materials/GGXBRDF.cpp's anonymous namespace,
//  replacing the previous ones verbatim.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cmath>
#include <vector>

static const double PI = 3.14159265358979323846;
static const double TWO_PI = 2.0 * PI;

static inline double r_max( double a, double b ) { return a > b ? a : b; }
static inline double r_min( double a, double b ) { return a < b ? a : b; }

struct Vec3 { double x, y, z; };
static inline Vec3 normalize( const Vec3& v )
{
	double len = sqrt( v.x*v.x + v.y*v.y + v.z*v.z );
	if( len < 1e-20 ) return Vec3{ 0, 0, 1 };
	return Vec3{ v.x/len, v.y/len, v.z/len };
}
static inline double dot( const Vec3& a, const Vec3& b ) { return a.x*b.x + a.y*b.y + a.z*b.z; }

// Transcribed from MicrofacetUtils::GGX_Lambda_Aniso/GGX_G1_Aniso/
// GGX_G2_Aniso at alphaX=alphaY=alpha (isotropic reduction) -- same
// physics GenerateMicrofacetEnergyLUT.cpp's own GGX_Lambda/GGX_G1/
// GGX_G2_HeightCorrelated implement, re-transcribed here so this stays
// a standalone translation unit (matching that file's own rationale).
static double GGX_Lambda( double alpha, double cosTheta )
{
	if( cosTheta >= 1.0 - 1e-10 ) return 0.0;
	if( cosTheta < 1e-10 ) return 1e10;
	const double cos2 = cosTheta * cosTheta;
	const double tan2 = ( 1.0 - cos2 ) / cos2;
	return ( -1.0 + sqrt( 1.0 + alpha*alpha*tan2 ) ) * 0.5;
}
static double GGX_G1( double alpha, double cosTheta ) { return 1.0 / ( 1.0 + GGX_Lambda( alpha, cosTheta ) ); }
static double GGX_G2( double alpha, double cosWi, double cosWo )
{
	return 1.0 / ( 1.0 + GGX_Lambda( alpha, cosWi ) + GGX_Lambda( alpha, cosWo ) );
}

// VNDF sampling (Dupuy-Benyoub spherical cap), transcribed verbatim from
// GenerateMicrofacetEnergyLUT.cpp's own VNDF_Sample_Local.  wi must
// point away from the surface (toward the viewer); normal is (0,0,1).
// Returns the sampled microfacet normal (== half-vector h) in local space.
static Vec3 VNDF_Sample_Local( const Vec3& wi, double alpha, double u1, double u2 )
{
	if( alpha < 1e-6 ) return Vec3{ 0, 0, 1 };
	Vec3 wi_h = normalize( Vec3{ alpha * wi.x, alpha * wi.y, wi.z } );
	double phi = TWO_PI * u1;
	double z = ( 1.0 - u2 ) * ( 1.0 + wi_h.z ) - wi_h.z;
	double sinTheta = sqrt( r_max( 0.0, 1.0 - z*z ) );
	double x = sinTheta * cos( phi );
	double y = sinTheta * sin( phi );
	Vec3 c{ x + wi_h.x, y + wi_h.y, z + wi_h.z };
	return normalize( Vec3{ alpha * c.x, alpha * c.y, c.z } );
}

// Simple LCG random number generator -- identical form to
// GenerateMicrofacetEnergyLUT.cpp's own rand01, independent stream
// (this tool's own rng_state instance).
static unsigned long long rng_state = 1234567890123456789ULL;
static double rand01()
{
	rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
	return (double)( rng_state >> 11 ) / (double)( 1ULL << 53 );
}

// Solve the (N+1)x(N+1) linear system V*w = m via Gauss-Jordan with
// partial pivoting.  V is the SAME fixed matrix for every alpha
// (V[k][i] = u_i^k), so this is called once per alpha with the same V.
static std::vector<double> SolveVandermonde( const std::vector<std::vector<double>>& V, const std::vector<double>& rhs )
{
	const int n = (int)rhs.size();
	std::vector<std::vector<double>> A( n, std::vector<double>( n + 1 ) );
	for( int r = 0; r < n; r++ ) {
		for( int c = 0; c < n; c++ ) A[r][c] = V[r][c];
		A[r][n] = rhs[r];
	}
	for( int col = 0; col < n; col++ ) {
		int piv = col;
		double best = fabs( A[col][col] );
		for( int r = col + 1; r < n; r++ ) {
			if( fabs( A[r][col] ) > best ) { best = fabs( A[r][col] ); piv = r; }
		}
		std::swap( A[col], A[piv] );
		const double d = A[col][col];
		for( int c = col; c <= n; c++ ) A[col][c] /= d;
		for( int r = 0; r < n; r++ ) {
			if( r == col ) continue;
			const double f = A[r][col];
			if( f == 0.0 ) continue;
			for( int c = col; c <= n; c++ ) A[r][c] -= f * A[col][c];
		}
	}
	std::vector<double> w( n );
	for( int r = 0; r < n; r++ ) w[r] = A[r][n];
	return w;
}

// Moments_k(alpha) = R_ss[u^k] for k=0..numMoments-1, via the VNDF
// estimator described in the file header "MOMENT ESTIMATOR".
static void Moments( double alpha, int numMuI, int numSamples, int numMoments, std::vector<double>& outMoments )
{
	outMoments.assign( numMoments, 0.0 );
	const double dmuI = 1.0 / numMuI;

	for( int bi = 0; bi < numMuI; bi++ ) {
		const double muI = ( bi + 0.5 ) * dmuI;
		const double sinI = sqrt( r_max( 0.0, 1.0 - muI*muI ) );
		const Vec3 wi{ sinI, 0.0, muI };
		const double G1wi = GGX_G1( alpha, muI );

		std::vector<double> innerSum( numMoments, 0.0 );
		for( int s = 0; s < numSamples; s++ ) {
			const double u1 = rand01();
			const double u2 = rand01();
			const Vec3 m = VNDF_Sample_Local( wi, alpha, u1, u2 );
			const double wiDotM = dot( wi, m );
			if( wiDotM <= 0 ) continue;

			const Vec3 wo{ 2.0*wiDotM*m.x - wi.x, 2.0*wiDotM*m.y - wi.y, 2.0*wiDotM*m.z - wi.z };
			const double cosWo = wo.z; // reflect() preserves unit length exactly for a unit m,wi
			if( cosWo <= 0 ) continue;

			const double u = r_max( 0.0, r_min( 1.0, 1.0 - wiDotM ) ); // muH == wiDotM == dot(wo,m)
			const double perSampleWeight = GGX_G2( alpha, muI, cosWo ) / G1wi;

			double upow = 1.0;
			for( int k = 0; k < numMoments; k++ ) {
				innerSum[k] += upow * perSampleWeight;
				upow *= u;
			}
		}

		// innerSum[k]/numSamples estimates INT_wo u^k*D*G2/(4*cosWi) dwo
		// (see header derivation); weight by cosWi*dmuI (outer 1-D
		// midpoint quadrature; the redundant phi_i integral supplies an
		// exact extra 2*pi, and the leading (1/pi) bihemispherical
		// normalization cancels it to a factor of 2 overall).
		for( int k = 0; k < numMoments; k++ ) {
			outMoments[k] += ( innerSum[k] / (double)numSamples ) * muI * dmuI;
		}
	}
	for( int k = 0; k < numMoments; k++ ) outMoments[k] *= 2.0;
}

int main()
{
	// E_avg_TABLE_G2's own alpha axis: uniform 32 nodes, [0.01, 1.0].
	const int kNumAlphaBins = 32;
	const double kAlphaMin = 0.01;
	const double kAlphaMax = 1.0;

	// Chebyshev-Gauss-Lobatto nodes in u=1-cosThetaH on [0,1], N=7 (8
	// nodes) -- see header "NODE PLACEMENT".
	const int N = 7;
	const int numNodes = N + 1;
	std::vector<double> u( numNodes );
	for( int i = 0; i <= N; i++ ) {
		u[i] = 0.5 * ( 1.0 - cos( PI * i / (double)N ) );
	}

	// Fixed Vandermonde matrix V[k][i] = u_i^k, k,i = 0..N.
	std::vector<std::vector<double>> V( numNodes, std::vector<double>( numNodes ) );
	for( int k = 0; k < numNodes; k++ ) {
		for( int i = 0; i < numNodes; i++ ) {
			V[k][i] = pow( u[i], k );
		}
	}

	const int numMuI = 48;
	const int numSamples = 200000;

	printf( "// Generated by tools/GGXSpecularBihemisphericalGen.cpp -- see\n" );
	printf( "// that file's header for the derivation (moment-matched fixed-\n" );
	printf( "// node quadrature via VNDF importance sampling) and the alpha\n" );
	printf( "// axis (uniform, matches E_avg_TABLE_G2).  DO NOT HAND-EDIT;\n" );
	printf( "// re-run the generator and paste both arrays back in.\n" );
	printf( "// numNodes=%d (Chebyshev-Gauss-Lobatto in u=1-cosThetaH), kNumAlphaBins=%d,\n", numNodes, kNumAlphaBins );
	printf( "// alpha_i = %.2f + i*(%.2f)/(N-1), baked at numMuI=%d numSamples=%d VNDF MC.\n",
		kAlphaMin, kAlphaMax - kAlphaMin, numMuI, numSamples );

	printf( "static const int kGGXSpecularQuadNumNodes = %d;\n\n", numNodes );

	printf( "// mu_i = 1 - u_i (cosThetaH nodes, normal incidence first).\n" );
	printf( "static const Scalar kGGXSpecularQuadNodes[ kGGXSpecularQuadNumNodes ] =\n{\n\t" );
	for( int i = 0; i < numNodes; i++ ) {
		printf( "%.10ff%s", 1.0 - u[i], ( i + 1 < numNodes ) ? ", " : "" );
	}
	printf( "\n};\n\n" );

	printf( "static const Scalar kGGXSpecularQuadWeight[ %d ][ kGGXSpecularQuadNumNodes ] =\n{\n", kNumAlphaBins );

	fprintf( stderr, "Spot checks (alpha, N0=moment0, should track E_avg_TABLE_G2):\n" );

	for( int a = 0; a < kNumAlphaBins; a++ ) {
		const double frac = (double)a / (double)( kNumAlphaBins - 1 );
		const double alpha = kAlphaMin + frac * ( kAlphaMax - kAlphaMin );

		std::vector<double> moments;
		Moments( alpha, numMuI, numSamples, numNodes, moments );

		const std::vector<double> w = SolveVandermonde( V, moments );

		printf( "\t{ " );
		for( int i = 0; i < numNodes; i++ ) {
			printf( "%.10ff%s", w[i], ( i + 1 < numNodes ) ? ", " : "" );
		}
		printf( " },\n" );

		fprintf( stderr, "  alpha=%.4f  N0=%.6f\n", alpha, moments[0] );
	}

	printf( "};\n" );

	return 0;
}
