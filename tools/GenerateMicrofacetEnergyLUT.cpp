//////////////////////////////////////////////////////////////////////
//
//  GenerateMicrofacetEnergyLUT.cpp - Offline tool to generate the
//    precomputed directional albedo LUT for Kulla-Conty multiscattering
//    energy compensation.
//
//  Outputs: src/Library/Utilities/MicrofacetEnergyLUT.h
//
//  Build (from project root):
//    c++ -O3 -Isrc/Library -o tools/gen_lut tools/GenerateMicrofacetEnergyLUT.cpp -lm
//
//  Run:
//    tools/gen_lut > src/Library/Utilities/MicrofacetEnergyLUT.h
//
//  Author: Aravind Krishnaswamy
//  Date: March 28, 2026
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cmath>
#include <cstdlib>

// Inline the math we need rather than pulling in the full library
static const double PI = 3.14159265358979323846;
static const double TWO_PI = 2.0 * PI;

struct Vec3 {
	double x, y, z;
	Vec3() : x(0), y(0), z(0) {}
	Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
};

static Vec3 normalize(const Vec3& v) {
	double len = sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
	if(len < 1e-20) return Vec3(0,0,1);
	return Vec3(v.x/len, v.y/len, v.z/len);
}

static double dot(const Vec3& a, const Vec3& b) {
	return a.x*b.x + a.y*b.y + a.z*b.z;
}

// GGX G1 masking (Smith, separable-model form)
static double GGX_G1(double alpha, double cosTheta) {
	if(cosTheta < 1e-10) return 0;
	double a2 = alpha * alpha;
	double cos2 = cosTheta * cosTheta;
	return 2.0 * cosTheta / (cosTheta + sqrt(a2 + (1.0 - a2) * cos2));
}

// DL-63: height-correlated Smith masking-shadowing (Heitz 2014 JCGT
// 3(2) Sec. 5.2), matching MicrofacetUtils::GGX_Lambda/GGX_G2 -- the
// model GGXBRDF/GGXSPF/CoatedBRDF actually render with, as opposed to
// the separable G1(wi)*G1(wo) model CookTorranceBRDF renders with (see
// MicrofacetUtils::GGX_G's own doc comment).  Lambda(v) = (-1 +
// sqrt(1 + alpha^2*tan^2(theta))) / 2; G2 = 1/(1 + Lambda(wi) + Lambda(wo)).
static double GGX_Lambda(double alpha, double cosTheta) {
	if(cosTheta >= 1.0 - 1e-10) return 0.0;
	if(cosTheta < 1e-10) return 1e10;
	double cos2 = cosTheta * cosTheta;
	double tan2 = (1.0 - cos2) / cos2;
	return (-1.0 + sqrt(1.0 + alpha * alpha * tan2)) * 0.5;
}
static double GGX_G2_HeightCorrelated(double alpha, double cosWi, double cosWo) {
	return 1.0 / (1.0 + GGX_Lambda(alpha, cosWi) + GGX_Lambda(alpha, cosWo));
}

// DL-77: anisotropic Lambda/G1/G2 (Heitz 2014 Eq. 86, generalized per-axis),
// mirroring MicrofacetUtils::GGX_Lambda_Aniso/GGX_G1_Aniso/GGX_G2_Aniso --
// the ACTUAL formulas GGXBRDF/GGXSPF render single-scatter with.  v is a
// direction in local (tangent) space: x=u, y=v, z=n.
static double GGX_Lambda_Aniso(double alphaX, double alphaY, const Vec3& v) {
	double vz2 = v.z * v.z;
	if(vz2 < 1e-20) return 1e10;
	double ax = (alphaX < 1e-4) ? 1e-4 : alphaX;
	double ay = (alphaY < 1e-4) ? 1e-4 : alphaY;
	double a2 = ax * ax * v.x * v.x + ay * ay * v.y * v.y;
	return (-1.0 + sqrt(1.0 + a2 / vz2)) * 0.5;
}
static double GGX_G1_Aniso(double alphaX, double alphaY, const Vec3& v) {
	return 1.0 / (1.0 + GGX_Lambda_Aniso(alphaX, alphaY, v));
}
static double GGX_G2_Aniso_HeightCorrelated(double alphaX, double alphaY, const Vec3& wi, const Vec3& wo) {
	return 1.0 / (1.0 + GGX_Lambda_Aniso(alphaX, alphaY, wi) + GGX_Lambda_Aniso(alphaX, alphaY, wo));
}

// VNDF sampling (Dupuy-Benyoub spherical cap)
// wi must point away from surface (toward viewer), normal is (0,0,1)
// Returns micronormal in local space
static Vec3 VNDF_Sample_Local(const Vec3& wi, double alpha, double u1, double u2) {
	if(alpha < 1e-6) return Vec3(0, 0, 1);

	// Stretch wi
	Vec3 wi_h = normalize(Vec3(alpha * wi.x, alpha * wi.y, wi.z));

	// Sample spherical cap
	double phi = TWO_PI * u1;
	double z = (1.0 - u2) * (1.0 + wi_h.z) - wi_h.z;
	double sinTheta = sqrt(fmax(0.0, 1.0 - z * z));
	double x = sinTheta * cos(phi);
	double y = sinTheta * sin(phi);

	// Compute micronormal in hemisphere configuration
	Vec3 c(x + wi_h.x, y + wi_h.y, z + wi_h.z);

	// Unstretch
	return normalize(Vec3(alpha * c.x, alpha * c.y, c.z));
}

// DL-77: anisotropic twin of VNDF_Sample_Local above, mirroring
// MicrofacetUtils::VNDF_Sample_Aniso specialized to local coordinates
// (wi already expressed in tangent space, so no OrthonormalBasis3D is
// needed here -- same specialization VNDF_Sample_Local already is of
// MicrofacetUtils::VNDF_Sample*).
static Vec3 VNDF_Sample_Local_Aniso(const Vec3& wi, double alphaX, double alphaY, double u1, double u2) {
	Vec3 wi_h = normalize(Vec3(alphaX * wi.x, alphaY * wi.y, wi.z));

	double phi = TWO_PI * u1;
	double z = (1.0 - u2) * (1.0 + wi_h.z) - wi_h.z;
	double sinTheta = sqrt(fmax(0.0, 1.0 - z * z));
	double x = sinTheta * cos(phi);
	double y = sinTheta * sin(phi);

	Vec3 c(x + wi_h.x, y + wi_h.y, z + wi_h.z);

	return normalize(Vec3(alphaX * c.x, alphaY * c.y, c.z));
}

// Simple LCG random number generator
static unsigned long long rng_state = 1234567890123456789ULL;
static double rand01() {
	rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
	return (double)(rng_state >> 11) / (double)(1ULL << 53);
}

static const int LUT_SIZE = 32;
static const int NUM_SAMPLES = 1000000;

// DL-77: anisotropic Kulla-Conty compensation table dimensions.
// ANISO_ALPHA_SIZE/ANISO_COS_SIZE are coarser than LUT_SIZE (16 vs 32) to
// keep the total sample budget in the same generation-time ballpark as the
// isotropic bake.  ANISO_RATIO_MAX=100 spans the full ratio range reachable
// by alphaX,alphaY both clamped to the existing [0.01,1.0] LUT alpha range
// (max/min = 1.0/0.01 = 100); ratio is log-spaced since the compensation's
// sensitivity to ratio is itself roughly logarithmic (halving alphaX at
// fixed alphaY matters far more near ratio=1 than near ratio=50).
//
// ANISO_PHI_SIZE resolves the incident direction's AZIMUTH relative to the
// tangent/bitangent axes -- a first cut at this table (azimuthally
// AVERAGING wi's azimuth entirely) closed the DL-77 deficit on average but
// regressed an existing anisotropic TestSchlickSweep row (alphaX=.05/
// alphaY=.5, theta=80, az=90: measured true single-azimuth Ess spans
// 0.789 (az=0) to 0.879 (az=90) against a 0.841 azimuthal average -- a
// real ~10% per-azimuth spread the averaged table cannot represent,
// causing the compensation to OVER-shoot at az=90 once the average-case
// deficit was closed).  ANISO_PHI_SIZE grid points span [0,90] degrees
// INCLUSIVE of both endpoints (by the ellipse symmetry: Ess(alphaX,alphaY,
// mu,phi) has period 180 degrees and is mirror-symmetric about both 0 and
// 90, so one quarter-period with reflective boundaries suffices) --
// endpoint-inclusive specifically so phi=0 and phi=90 (the two azimuths
// TestSchlickSweep's anisotropic rows actually probe, i.e. wi aligned with
// one tangent axis or the other) are EXACT table entries, not
// interpolated.  E_ss_TABLE_G2_ANISO_PHI[ratio][alphaEff][phi][cos] feeds
// the ENERGY-COMPENSATION lookup (LookupEssG2AnisoDirectional, used at
// GGXBRDF/GGXSPF's Ess_i/Ess_o call sites); E_ss_TABLE_G2_ANISO (no phi
// dimension) is DERIVED from it by trapezoidal-averaging over phi and
// continues to feed ONLY the H6 multiscatter-lobe OUTGOING-DIRECTION
// SAMPLER (MSLobeZG2Aniso/SampleMSCosThetaG2Aniso/MSPdfG2Aniso), where an
// azimuth-averaged proposal shape costs importance-sampling efficiency,
// not correctness (MSPdfG2Aniso always reports the density of what
// SampleMSCosThetaG2Aniso actually samples, so the estimator stays
// unbiased regardless of how good the proposal shape is).
static const int ANISO_ALPHA_SIZE = 16;
static const int ANISO_RATIO_SIZE = 8;
static const int ANISO_COS_SIZE = 16;
static const int ANISO_PHI_SIZE = 7;	// 0,15,30,45,60,75,90 degrees
static const double ANISO_RATIO_MAX = 100.0;
static const int NUM_SAMPLES_ANISO = 150000;

// H6 + DL-63: verbatim, hand-maintained multiscatter-lobe sampler/pdf
// machinery (the MSLobeDetail namespace, MSLobeZ/SampleMSCosTheta/MSPdf,
// and their DL-63 G2 twins).  Unlike the tables above, this code is not
// produced by the Monte-Carlo bake in main() below -- it is exact,
// closed-form machinery built analytically on top of the piecewise-
// linear Ess model the baked tables define.  It is embedded here
// verbatim (byte-for-byte, carried over from the hand-authored
// original) so that regenerating MicrofacetEnergyLUT.h reproduces it
// exactly instead of silently dropping it -- GGXSPF/CookTorranceSPF
// call MSLobeZ/SampleMSCosTheta/MSPdf and the G2 twins directly, so a
// generator that only emitted the tables would break the build.
static const char* const kHandMaintainedH6Block =
R"GGXH6BLOCK(	//////////////////////////////////////////////////////////////////
	// H6: multiscatter-lobe outgoing-direction sampler + matching pdf.
	//
	// The MS lobe's true energy in outgoing direction wo is proportional
	// to (1-Ess(cosWo,alpha))*cosWo -- the definition of E_avg above is
	// exactly its cosine-weighted hemisphere average:
	//   2 * integral_0^1 (1-Ess(alpha,mu)) * mu dmu == 1 - E_avg(alpha)
	// Sampling wo from THIS shape (instead of plain cosine-hemisphere)
	// makes the MS-lobe's importance-sampling weight collapse to a
	// bounded per-shading-point constant instead of blowing up at
	// grazing incidence (docs/... H6 design note; the old
	// ws*(1-Eavg) selection weight + cosine-sampled wo assumed
	// (1-Ess)=(1-Eavg) everywhere, which fails badly near cosTheta->0
	// on smooth surfaces).
	//
	// LookupEss(cosTheta,alpha) is PIECEWISE-LINEAR in cosTheta between
	// adjacent LUT bin centers (with flat clamping outside the first/
	// last centers) -- exactly what its bilinear-interpolation code
	// computes, and bilinear interpolation is separable, so blending the
	// two alpha rows first and then interpolating cosTheta (as done
	// below) is algebraically IDENTICAL to LookupEss's simultaneous
	// bilinear form for any (cosTheta, alpha).  That makes
	// shape(cos) = (1-LookupEss(cos,alpha))*cos a piecewise-QUADRATIC
	// function of cos with an EXACT closed-form integral and an exactly
	// invertible per-segment CDF (monotone; solved with a bracketed
	// Newton-bisection hybrid -- no numerical-quadrature approximation
	// anywhere).  MSPdf() below evaluates LookupEss directly, so the
	// density it reports for ANY cosWo is algebraically identical to the
	// shape MSLobeZ/SampleMSCosTheta integrate and sample: pdf and
	// sampler share the SAME LookupEss piecewise-linear model and cannot
	// drift apart.
	//////////////////////////////////////////////////////////////////

	namespace MSLobeDetail
	{
		// One [lo,hi] segment of the piecewise-linear-in-cosTheta Ess
		// model: either a flat end-cap (below the first / above the
		// last bin center) or the linear span between two adjacent bin
		// centers.  Ess(cos) = essLo + slope*(cos-lo) for cos in [lo,hi].
		struct Segment
		{
			Scalar lo, hi;
			Scalar essLo, slope;
		};

		// Build the LUT_SIZE+1 segments (33: 1 left cap + LUT_SIZE-1
		// interior spans + 1 right cap) from an already-resolved essRow
		// (either a single exact LUT row, or LookupEss's alpha-blended row).
		inline void BuildSegmentsFromRow( const Scalar essRow[LUT_SIZE], Segment segs[LUT_SIZE + 1], int& nSegs )
		{
			nSegs = 0;
			const Scalar c0 = 0.5 / Scalar(LUT_SIZE);
			const Scalar cLast = (Scalar(LUT_SIZE) - 0.5) / Scalar(LUT_SIZE);

			// Left flat end-cap [0, c0]: Ess clamped to row 0.
			segs[nSegs].lo = 0.0; segs[nSegs].hi = c0;
			segs[nSegs].essLo = essRow[0]; segs[nSegs].slope = 0.0;
			nSegs++;

			// Interior linear spans between adjacent bin centers.
			for( int k = 0; k < LUT_SIZE - 1; k++ )
			{
				const Scalar ck  = (k + 0.5) / Scalar(LUT_SIZE);
				const Scalar ck1 = (k + 1.5) / Scalar(LUT_SIZE);
				segs[nSegs].lo = ck; segs[nSegs].hi = ck1;
				segs[nSegs].essLo = essRow[k];
				segs[nSegs].slope = (essRow[k+1] - essRow[k]) / (ck1 - ck);
				nSegs++;
			}

			// Right flat end-cap [cLast, 1]: Ess clamped to row LUT_SIZE-1.
			segs[nSegs].lo = cLast; segs[nSegs].hi = 1.0;
			segs[nSegs].essLo = essRow[LUT_SIZE-1]; segs[nSegs].slope = 0.0;
			nSegs++;
		}

		// Build the segments for a fixed alphaEff, using EXACTLY LookupEss's
		// alpha-row blend.
		inline void BuildSegments( const Scalar alphaEff, Segment segs[LUT_SIZE + 1], int& nSegs )
		{
			Scalar a = r_max(0.0, r_min(1.0, (alphaEff - 0.01) / (1.0 - 0.01))) * (LUT_SIZE - 1);
			int ai0 = (int)a;
			int ai1 = r_min(ai0 + 1, LUT_SIZE - 1);
			Scalar af = a - ai0;

			Scalar essRow[LUT_SIZE];
			for( int k = 0; k < LUT_SIZE; k++ )
				essRow[k] = (1-af) * E_ss_TABLE[ai0][k] + af * E_ss_TABLE[ai1][k];

			BuildSegmentsFromRow( essRow, segs, nSegs );
		}

		// DL-63: height-correlated-G2 twin of BuildSegments above, using
		// EXACTLY LookupEssG2's alpha-row blend (E_ss_TABLE_G2 instead of
		// E_ss_TABLE).  Segment/SegShape/SegCDF/SegTotal/SegInvert and
		// BuildSegmentsFromRow are already table-agnostic (they only see
		// an already-resolved essRow), so only the alpha-row blend needs
		// a G2-specific twin.
		inline void BuildSegmentsG2( const Scalar alphaEff, Segment segs[LUT_SIZE + 1], int& nSegs )
		{
			Scalar a = r_max(0.0, r_min(1.0, (alphaEff - 0.01) / (1.0 - 0.01))) * (LUT_SIZE - 1);
			int ai0 = (int)a;
			int ai1 = r_min(ai0 + 1, LUT_SIZE - 1);
			Scalar af = a - ai0;

			Scalar essRow[LUT_SIZE];
			for( int k = 0; k < LUT_SIZE; k++ )
				essRow[k] = (1-af) * E_ss_TABLE_G2[ai0][k] + af * E_ss_TABLE_G2[ai1][k];

			BuildSegmentsFromRow( essRow, segs, nSegs );
		}

		// shape(cos) = (1-Ess(cos))*cos within a segment; y = cos - lo.
		inline Scalar SegShape( const Segment& s, const Scalar y )
		{
			const Scalar ess = s.essLo + s.slope * y;
			return r_max( 0.0, (1.0 - ess) * (s.lo + y) );
		}

		// Exact antiderivative F(y) = integral_0^y SegShape(t) dt.
		// shape(y) = P + Q*y + R*y^2 with P=(1-essLo)*lo, Q=(1-essLo)-slope*lo,
		// R=-slope (obtained by expanding (1-essLo-slope*y)*(lo+y)).
		inline Scalar SegCDF( const Segment& s, const Scalar y )
		{
			const Scalar P = (1.0 - s.essLo) * s.lo;
			const Scalar Q = (1.0 - s.essLo) - s.slope * s.lo;
			const Scalar R = -s.slope;
			return P * y + Q * y * y * 0.5 + R * y * y * y * (1.0 / 3.0);
		}

		inline Scalar SegTotal( const Segment& s )
		{
			return SegCDF( s, s.hi - s.lo );
		}

		// Invert F(y) = target for y in [0,w] via Newton-bisection (F is
		// monotone non-decreasing since SegShape >= 0 everywhere shape is
		// physical).  30 iterations is comfortably converged for a cubic
		// on a bounded interval; the bisection fallback guarantees
		// convergence even if a Newton step would leave the bracket.
		inline Scalar SegInvert( const Segment& s, const Scalar target, const Scalar w )
		{
			Scalar ylo = 0.0, yhi = w;
			Scalar y = w * 0.5;
			for( int it = 0; it < 30; it++ )
			{
				const Scalar Fy = SegCDF( s, y );
				const Scalar diff = Fy - target;
				if( fabs(diff) < 1e-13 ) break;
				if( diff > 0 ) yhi = y; else ylo = y;

				const Scalar deriv = SegShape( s, y );
				Scalar yNext = (deriv > 1e-12) ? (y - diff / deriv) : (0.5 * (ylo + yhi));
				if( !(yNext > ylo && yNext < yhi) ) yNext = 0.5 * (ylo + yhi);
				y = yNext;
			}
			return y;
		}
	}

	/// Exact normalization for the MS-lobe outgoing-direction shape:
	///   I(alpha) = integral_0^1 (1-Ess(alpha,mu))*mu dmu   (segment-exact)
	///   Z(alpha) = 2 * I(alpha)  (== 1-E_avg(alpha) in the continuum
	///   limit; computed here from the SAME piecewise-linear Ess model
	///   that LookupEss / SampleMSCosTheta use, so it cannot drift out of
	///   sync with either).
	///
	/// H6 perf note: this used to call BuildSegments() (O(LUT_SIZE) to
	/// build the blended row + O(LUT_SIZE) segment integrals) on EVERY
	/// call, and it is evaluated per-NEE-light-sample from GGXSPF /
	/// CookTorranceSPF's Pdf/PdfNM.  Per LookupEss's own alpha blend,
	/// essRow(af) = (1-af)*E_ss_TABLE[ai0] + af*E_ss_TABLE[ai1] is LINEAR
	/// in af for a fixed (ai0,ai1) pair; SegCDF/SegTotal are themselves
	/// linear in (essLo, slope) (see their definitions above), and
	/// (essLo, slope) are each linear in essRow -- so I(alpha), and hence
	/// Z(alpha) = 2*I(alpha), is an EXACT affine (linear) function of af
	/// within one alpha bin.  That means Z(alphaEff) can be obtained by
	/// precomputing the exact per-row Z (Z evaluated with essRow pinned
	/// to a single LUT row, i.e. af=0) ONCE per row and then doing the
	/// SAME (1-af)/af blend LookupEss uses for Ess itself -- an O(1)
	/// lookup that is algebraically IDENTICAL to re-running
	/// BuildSegments+SegTotal every call, not an approximation.
	inline Scalar MSLobeZ( const Scalar alphaEff )
	{
		// Per-alpha-row Z, computed once (C++11 magic-statics: thread-safe
		// initialization, no locking on the steady-state read path).
		static const std::array<Scalar, LUT_SIZE> rowZ = []() {
			std::array<Scalar, LUT_SIZE> z{};
			for( int row = 0; row < LUT_SIZE; row++ )
			{
				MSLobeDetail::Segment segs[LUT_SIZE + 1];
				int nSegs = 0;
				MSLobeDetail::BuildSegmentsFromRow( E_ss_TABLE[row], segs, nSegs );
				Scalar I = 0.0;
				for( int i = 0; i < nSegs; i++ )
					I += MSLobeDetail::SegTotal( segs[i] );
				z[row] = 2.0 * I;
			}
			return z;
		}();

		// Same alpha -> (ai0, ai1, af) mapping as LookupEss/BuildSegments.
		const Scalar a = r_max(0.0, r_min(1.0, (alphaEff - 0.01) / (1.0 - 0.01))) * (LUT_SIZE - 1);
		const int ai0 = (int)a;
		const int ai1 = r_min(ai0 + 1, LUT_SIZE - 1);
		const Scalar af = a - ai0;
		return (1.0 - af) * rowZ[ai0] + af * rowZ[ai1];
	}

	/// Sample cosWo ~ (1-Ess(alpha,cosWo))*cosWo / I(alpha) via exact
	/// per-segment CDF inversion.  u1 in [0,1). Returns cosWo in [0,1].
	/// Azimuth is NOT handled here (uniform; caller draws it separately).
	inline Scalar SampleMSCosTheta( const Scalar alphaEff, const Scalar u1 )
	{
		MSLobeDetail::Segment segs[LUT_SIZE + 1];
		int nSegs = 0;
		MSLobeDetail::BuildSegments( alphaEff, segs, nSegs );

		Scalar totals[LUT_SIZE + 1];
		Scalar I = 0.0;
		for( int i = 0; i < nSegs; i++ )
		{
			totals[i] = MSLobeDetail::SegTotal( segs[i] );
			I += totals[i];
		}
		if( I <= 1e-15 ) return r_max( 0.0, r_min( 1.0, u1 ) );	// degenerate fallback

		const Scalar target = r_max( 0.0, r_min( I, u1 * I ) );
		int seg = 0;
		Scalar running = 0.0;
		while( seg < nSegs - 1 && running + totals[seg] < target )
		{
			running += totals[seg];
			seg++;
		}
		const Scalar localTarget = r_max( 0.0, r_min( totals[seg], target - running ) );
		const Scalar y = MSLobeDetail::SegInvert( segs[seg], localTarget, segs[seg].hi - segs[seg].lo );
		return r_max( 0.0, r_min( 1.0, segs[seg].lo + y ) );
	}

	/// Solid-angle density of SampleMSCosTheta's output, expressed the
	/// same way as this codebase's other cosine-hemisphere pdfs (a
	/// function of cosTheta alone; azimuth is uniform and already folded
	/// in).  Uses LookupEss directly -- the SAME piecewise-linear model
	/// the sampler above is built from -- so this can never drift out of
	/// sync with what was actually sampled (see file-header comment).
	/// `Z` must be MSLobeZ(alphaEff) (callers compute it once per shading
	/// point and reuse it across the diffuse/specular/MS mixPdf sites).
	inline Scalar MSPdf( const Scalar cosTheta, const Scalar alphaEff, const Scalar Z )
	{
		if( Z <= 1e-15 ) return 0.0;
		const Scalar c = r_max( 0.0, r_min( 1.0, cosTheta ) );
		return r_max( 0.0, (1.0 - LookupEss( c, alphaEff )) * c / (RISE::PI * Z) );
	}

	//////////////////////////////////////////////////////////////////
	// DL-63: height-correlated-G2 twins of MSLobeZ/SampleMSCosTheta/
	// MSPdf above -- same H6 outgoing-direction shape/sampler/pdf
	// machinery, sourced from E_ss_TABLE_G2/LookupEssG2 instead of
	// E_ss_TABLE/LookupEss.  GGXSPF's multiscatter lobe (which renders
	// with height-correlated G2) uses these so its sampled direction,
	// reported density, and the F_ms/f_ms energy terms (LookupEavgG2/
	// LookupEssG2) all stay calibrated to the SAME masking-shadowing
	// model; CookTorranceSPF keeps using the separable-model MSLobeZ/
	// SampleMSCosTheta/MSPdf above, unchanged.
	//////////////////////////////////////////////////////////////////

	/// DL-63: height-correlated-G2 twin of MSLobeZ above.
	inline Scalar MSLobeZG2( const Scalar alphaEff )
	{
		static const std::array<Scalar, LUT_SIZE> rowZ = []() {
			std::array<Scalar, LUT_SIZE> z{};
			for( int row = 0; row < LUT_SIZE; row++ )
			{
				MSLobeDetail::Segment segs[LUT_SIZE + 1];
				int nSegs = 0;
				MSLobeDetail::BuildSegmentsFromRow( E_ss_TABLE_G2[row], segs, nSegs );
				Scalar I = 0.0;
				for( int i = 0; i < nSegs; i++ )
					I += MSLobeDetail::SegTotal( segs[i] );
				z[row] = 2.0 * I;
			}
			return z;
		}();

		const Scalar a = r_max(0.0, r_min(1.0, (alphaEff - 0.01) / (1.0 - 0.01))) * (LUT_SIZE - 1);
		const int ai0 = (int)a;
		const int ai1 = r_min(ai0 + 1, LUT_SIZE - 1);
		const Scalar af = a - ai0;
		return (1.0 - af) * rowZ[ai0] + af * rowZ[ai1];
	}

	/// DL-63: height-correlated-G2 twin of SampleMSCosTheta above.
	inline Scalar SampleMSCosThetaG2( const Scalar alphaEff, const Scalar u1 )
	{
		MSLobeDetail::Segment segs[LUT_SIZE + 1];
		int nSegs = 0;
		MSLobeDetail::BuildSegmentsG2( alphaEff, segs, nSegs );

		Scalar totals[LUT_SIZE + 1];
		Scalar I = 0.0;
		for( int i = 0; i < nSegs; i++ )
		{
			totals[i] = MSLobeDetail::SegTotal( segs[i] );
			I += totals[i];
		}
		if( I <= 1e-15 ) return r_max( 0.0, r_min( 1.0, u1 ) );	// degenerate fallback

		const Scalar target = r_max( 0.0, r_min( I, u1 * I ) );
		int seg = 0;
		Scalar running = 0.0;
		while( seg < nSegs - 1 && running + totals[seg] < target )
		{
			running += totals[seg];
			seg++;
		}
		const Scalar localTarget = r_max( 0.0, r_min( totals[seg], target - running ) );
		const Scalar y = MSLobeDetail::SegInvert( segs[seg], localTarget, segs[seg].hi - segs[seg].lo );
		return r_max( 0.0, r_min( 1.0, segs[seg].lo + y ) );
	}

	/// DL-63: height-correlated-G2 twin of MSPdf above.
	inline Scalar MSPdfG2( const Scalar cosTheta, const Scalar alphaEff, const Scalar Z )
	{
		if( Z <= 1e-15 ) return 0.0;
		const Scalar c = r_max( 0.0, r_min( 1.0, cosTheta ) );
		return r_max( 0.0, (1.0 - LookupEssG2( c, alphaEff )) * c / (RISE::PI * Z) );
	}
)GGXH6BLOCK";

// DL-77: hand-derived anisotropic Kulla-Conty compensation machinery,
// embedded verbatim for the same reason kHandMaintainedH6Block above is --
// it is exact closed-form machinery built on top of the baked
// E_ss_TABLE_G2_ANISO/E_avg_TABLE_G2_ANISO tables below, not itself
// re-derived by the Monte-Carlo bake in main().
static const char* const kHandMaintainedDL77AnisoBlock =
R"DL77ANISOBLOCK(	//////////////////////////////////////////////////////////////////
	// DL-77: anisotropic Kulla-Conty compensation.
	//
	// LookupEssG2/LookupEavgG2/MSLobeZG2/SampleMSCosThetaG2/MSPdfG2 above
	// are calibrated to an ISOTROPIC Smith height-correlated model at a
	// single alphaEff=sqrt(alphaX*alphaY) -- exact only when alphaX==
	// alphaY.  GGXBRDF/GGXSPF's single-scatter term uses the direction-
	// dependent per-axis Lambda (MicrofacetUtils::GGX_G2_Aniso), so for
	// alphaX != alphaY the isotropized lookup under-compensates (measured
	// 11-44% E_ss deficit at F0=1 -- see the DL-77 ledger row and
	// docs/DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md "DL-77").
	//
	// E_ss_TABLE_G2_ANISO/E_avg_TABLE_G2_ANISO below tabulate the SAME
	// height-correlated-G2 directional albedo, additionally resolved by
	// anisotropy RATIO = max(alphaX,alphaY)/min(alphaX,alphaY) (log-
	// spaced [1, ANISO_RATIO_MAX]), AZIMUTHALLY AVERAGED over the incident
	// direction's azimuth relative to the tangent/bitangent axes.  By
	// symmetry (swapping alphaX<->alphaY is a 90-degree relabeling of the
	// tangent axes, which the full-2*pi azimuthal average is invariant
	// to), the table only needs RATIO >= 1, with alphaX,alphaY resolved
	// from (alphaEff,ratio) as alphaEff/sqrt(ratio) and alphaEff*
	// sqrt(ratio).
	//
	// A first cut at this fix used ONLY the azimuthally-averaged table for
	// EVERYTHING (energy compensation AND the H6 sampler) and closed the
	// DL-77 deficit on average, but regressed an existing anisotropic
	// TestSchlickSweep row: the true single-azimuth Ess at alphaX=.05/
	// alphaY=.5, theta=80 spans 0.789 (wi azimuth=0) to 0.879 (azimuth=90)
	// against a 0.841 azimuthal average -- once the average-case deficit
	// closed, the az=90 direction (whose true Ess is ABOVE the average, so
	// needs LESS compensation than the average-based lookup supplies)
	// over-shot into an energy GAIN.  E_ss_TABLE_G2_ANISO_PHI additionally
	// resolves wi/wo's azimuth (phi, endpoint-inclusive grid over [0,90]
	// degrees) and LookupEssG2AnisoDirectional (below) is the per-azimuth
	// lookup GGXBRDF.cpp/GGXSPF.cpp use for their Ess_i/Ess_o ENERGY terms
	// -- LookupEssG2Aniso itself (azimuth-averaged) remains in place ONLY
	// as the H6 multiscatter-lobe outgoing-direction SAMPLER's proposal
	// shape (MSLobeZG2Aniso/SampleMSCosThetaG2Aniso/MSPdfG2Aniso), where
	// precision is an efficiency concern, not a correctness one (MSPdf
	// always reports the density of what the sampler actually draws, so
	// the estimator stays unbiased regardless of proposal quality).
	//
	// LookupEssG2Aniso/LookupEssG2AnisoDirectional/LookupEavgG2Aniso/
	// MSLobeZG2Aniso/SampleMSCosThetaG2Aniso/MSPdfG2Aniso below take
	// alphaX,alphaY directly and are the drop-in replacements GGXBRDF.cpp/
	// GGXSPF.cpp use for their Kulla-Conty lookups.  Each one FORWARDS to
	// the exact pre-existing isotropic function (LookupEssG2/LookupEavgG2/
	// MSLobeZG2/SampleMSCosThetaG2) when alphaX and alphaY are equal, so
	// isotropic GGX materials (alphaX==alphaY, the common case) are
	// numerically BYTE-IDENTICAL to before this fix -- only genuinely
	// anisotropic configurations resolve through the new tables.
	//////////////////////////////////////////////////////////////////

	namespace MSLobeDetail
	{
		// Generic-N twin of BuildSegmentsFromRow above (identical algebra,
		// parameterized on row length instead of fixed at LUT_SIZE) --
		// needed because the DL-77 aniso table's cosTheta resolution
		// (ANISO_COS_SIZE) differs from the isotropic tables' LUT_SIZE.
		inline void BuildSegmentsFromRowN( const Scalar* essRow, const int N, Segment segs[], int& nSegs )
		{
			nSegs = 0;
			const Scalar c0 = 0.5 / Scalar(N);
			const Scalar cLast = (Scalar(N) - 0.5) / Scalar(N);

			segs[nSegs].lo = 0.0; segs[nSegs].hi = c0;
			segs[nSegs].essLo = essRow[0]; segs[nSegs].slope = 0.0;
			nSegs++;

			for( int k = 0; k < N - 1; k++ )
			{
				const Scalar ck  = (k + 0.5) / Scalar(N);
				const Scalar ck1 = (k + 1.5) / Scalar(N);
				segs[nSegs].lo = ck; segs[nSegs].hi = ck1;
				segs[nSegs].essLo = essRow[k];
				segs[nSegs].slope = (essRow[k+1] - essRow[k]) / (ck1 - ck);
				nSegs++;
			}

			segs[nSegs].lo = cLast; segs[nSegs].hi = 1.0;
			segs[nSegs].essLo = essRow[N-1]; segs[nSegs].slope = 0.0;
			nSegs++;
		}
	}

	/// Map alphaEff (already clamped to [0.01,1.0]) to the DL-77 aniso
	/// table's alpha index pair (ai0,ai1,af) -- same linear mapping style
	/// as LookupEssG2's own alpha blend.
	inline void AnisoAlphaIndex( const Scalar alphaEff, int& ai0, int& ai1, Scalar& af )
	{
		Scalar a = r_max(0.0, r_min(1.0, (alphaEff - 0.01) / (1.0 - 0.01))) * (ANISO_ALPHA_SIZE - 1);
		ai0 = (int)a;
		ai1 = r_min(ai0 + 1, ANISO_ALPHA_SIZE - 1);
		af = a - ai0;
	}

	/// Map anisotropy ratio (>=1) to the DL-77 aniso table's log-spaced
	/// ratio index pair (ri0,ri1,rf).  Clamped (not extrapolated) beyond
	/// ANISO_RATIO_MAX, matching how the generator baked the grid.
	inline void AnisoRatioIndex( const Scalar ratio, int& ri0, int& ri1, Scalar& rf )
	{
		const Scalar logRatio = log( r_max( Scalar(1.0), ratio ) ) / log( ANISO_RATIO_MAX );
		Scalar r = r_max(0.0, r_min(1.0, logRatio)) * (ANISO_RATIO_SIZE - 1);
		ri0 = (int)r;
		ri1 = r_min(ri0 + 1, ANISO_RATIO_SIZE - 1);
		rf = r - ri0;
	}

	/// Map a local-space direction's (x,y) to the DL-77 aniso table's
	/// endpoint-inclusive phi index pair (pi0,pi1,pf), folded into the
	/// ellipse's quarter-period [0,90] degrees (see ANISO_PHI_SIZE's
	/// comment in the generator for the symmetry argument).  Degenerate
	/// (x,y) near zero (a direction nearly along the normal) folds to an
	/// arbitrary phi -- harmless, since Ess is nearly phi-independent
	/// there (every phi bin agrees close to cosTheta=1).
	inline void AnisoPhiIndex( const Scalar localX, const Scalar localY, int& pi0, int& pi1, Scalar& pf )
	{
		Scalar phi = atan2( localY, localX );		// (-PI, PI]
		phi = fabs( phi );				// [0, PI] (even in sign flip)
		if( phi > (RISE::PI * 0.5) ) phi = RISE::PI - phi;	// fold to [0, PI/2]

		Scalar p = r_max(0.0, r_min(1.0, phi / (RISE::PI * 0.5))) * (ANISO_PHI_SIZE - 1);
		pi0 = (int)p;
		pi1 = r_min(pi0 + 1, ANISO_PHI_SIZE - 1);
		pf = p - pi0;
	}

	/// DL-77: anisotropic twin of LookupEssG2.  Falls back to the exact
	/// isotropic LookupEssG2 when alphaX==alphaY (see file-header note).
	inline Scalar LookupEssG2Aniso( const Scalar cosTheta, const Scalar alphaX, const Scalar alphaY )
	{
		if( fabs(alphaX - alphaY) < 1e-9 ) return LookupEssG2( cosTheta, alphaX );

		const Scalar aX = r_max(0.01, r_min(1.0, alphaX));
		const Scalar aY = r_max(0.01, r_min(1.0, alphaY));
		const Scalar alphaEff = sqrt( aX * aY );
		const Scalar ratio = r_max(aX, aY) / r_min(aX, aY);

		int ai0, ai1; Scalar af;
		AnisoAlphaIndex( alphaEff, ai0, ai1, af );
		int ri0, ri1; Scalar rf;
		AnisoRatioIndex( ratio, ri0, ri1, rf );

		Scalar c = r_max(0.0, r_min(1.0, cosTheta)) * ANISO_COS_SIZE - 0.5;
		if( c < 0 ) c = 0;
		int ci0 = (int)c;
		int ci1 = r_min(ci0 + 1, ANISO_COS_SIZE - 1);
		Scalar cf = c - ci0;

		const Scalar v000 = E_ss_TABLE_G2_ANISO[ri0][ai0][ci0];
		const Scalar v001 = E_ss_TABLE_G2_ANISO[ri0][ai0][ci1];
		const Scalar v010 = E_ss_TABLE_G2_ANISO[ri0][ai1][ci0];
		const Scalar v011 = E_ss_TABLE_G2_ANISO[ri0][ai1][ci1];
		const Scalar v100 = E_ss_TABLE_G2_ANISO[ri1][ai0][ci0];
		const Scalar v101 = E_ss_TABLE_G2_ANISO[ri1][ai0][ci1];
		const Scalar v110 = E_ss_TABLE_G2_ANISO[ri1][ai1][ci0];
		const Scalar v111 = E_ss_TABLE_G2_ANISO[ri1][ai1][ci1];

		const Scalar v00 = (1-cf)*v000 + cf*v001;
		const Scalar v01 = (1-cf)*v010 + cf*v011;
		const Scalar v10 = (1-cf)*v100 + cf*v101;
		const Scalar v11 = (1-cf)*v110 + cf*v111;
		const Scalar v0 = (1-af)*v00 + af*v01;
		const Scalar v1 = (1-af)*v10 + af*v11;
		return (1-rf)*v0 + rf*v1;
	}

	/// DL-77: per-azimuth (NOT azimuthally-averaged) twin of
	/// LookupEssG2Aniso, reading E_ss_TABLE_G2_ANISO_PHI instead of the
	/// phi-averaged E_ss_TABLE_G2_ANISO.  localX,localY are the queried
	/// direction's tangent-space x,y (e.g. wi_local.x/wi_local.y for
	/// Ess_i, wo_local.x/wo_local.y for Ess_o) -- the SAME tangent frame
	/// alphaX/alphaY are defined in.  This is the function GGXBRDF/GGXSPF
	/// use for the Kulla-Conty ENERGY term (Ess_i, Ess_o); the H6
	/// multiscatter-lobe sampler keeps using the azimuth-averaged
	/// LookupEssG2Aniso (see the ANISO_PHI_SIZE comment in the generator
	/// for why the sampler doesn't need this precision).  Falls back to
	/// the exact isotropic LookupEssG2 when alphaX==alphaY (phi is
	/// meaningless for an isotropic surface).
	inline Scalar LookupEssG2AnisoDirectional( const Scalar cosTheta, const Scalar localX, const Scalar localY, const Scalar alphaX, const Scalar alphaY )
	{
		if( fabs(alphaX - alphaY) < 1e-9 ) return LookupEssG2( cosTheta, alphaX );

		const Scalar aX = r_max(0.01, r_min(1.0, alphaX));
		const Scalar aY = r_max(0.01, r_min(1.0, alphaY));
		const Scalar alphaEff = sqrt( aX * aY );
		const Scalar ratio = r_max(aX, aY) / r_min(aX, aY);

		int ai0, ai1; Scalar af;
		AnisoAlphaIndex( alphaEff, ai0, ai1, af );
		int ri0, ri1; Scalar rf;
		AnisoRatioIndex( ratio, ri0, ri1, rf );
		int pi0, pi1; Scalar pf;
		AnisoPhiIndex( localX, localY, pi0, pi1, pf );

		Scalar c = r_max(0.0, r_min(1.0, cosTheta)) * ANISO_COS_SIZE - 0.5;
		if( c < 0 ) c = 0;
		int ci0 = (int)c;
		int ci1 = r_min(ci0 + 1, ANISO_COS_SIZE - 1);
		Scalar cf = c - ci0;

		// Quadrilinear interpolation over (ratio, alphaEff, phi, cosTheta):
		// 16 corners, collapsed one axis at a time (cos, then phi, then
		// alphaEff, then ratio) -- same nested-lerp pattern as the
		// trilinear form above, one dimension deeper.
		Scalar vRP[2][2];	// [ratio][alphaEff], after collapsing phi and cos
		for( int ri = 0; ri < 2; ri++ )
		{
			const int riv = (ri == 0) ? ri0 : ri1;
			for( int ai = 0; ai < 2; ai++ )
			{
				const int aiv = (ai == 0) ? ai0 : ai1;
				const Scalar vP0c0 = E_ss_TABLE_G2_ANISO_PHI[riv][aiv][pi0][ci0];
				const Scalar vP0c1 = E_ss_TABLE_G2_ANISO_PHI[riv][aiv][pi0][ci1];
				const Scalar vP1c0 = E_ss_TABLE_G2_ANISO_PHI[riv][aiv][pi1][ci0];
				const Scalar vP1c1 = E_ss_TABLE_G2_ANISO_PHI[riv][aiv][pi1][ci1];
				const Scalar vP0 = (1-cf)*vP0c0 + cf*vP0c1;
				const Scalar vP1 = (1-cf)*vP1c0 + cf*vP1c1;
				vRP[ri][ai] = (1-pf)*vP0 + pf*vP1;
			}
		}
		const Scalar v0 = (1-af)*vRP[0][0] + af*vRP[0][1];
		const Scalar v1 = (1-af)*vRP[1][0] + af*vRP[1][1];
		return (1-rf)*v0 + rf*v1;
	}

	/// DL-77: anisotropic twin of LookupEavgG2.
	inline Scalar LookupEavgG2Aniso( const Scalar alphaX, const Scalar alphaY )
	{
		if( fabs(alphaX - alphaY) < 1e-9 ) return LookupEavgG2( alphaX );

		const Scalar aX = r_max(0.01, r_min(1.0, alphaX));
		const Scalar aY = r_max(0.01, r_min(1.0, alphaY));
		const Scalar alphaEff = sqrt( aX * aY );
		const Scalar ratio = r_max(aX, aY) / r_min(aX, aY);

		int ai0, ai1; Scalar af;
		AnisoAlphaIndex( alphaEff, ai0, ai1, af );
		int ri0, ri1; Scalar rf;
		AnisoRatioIndex( ratio, ri0, ri1, rf );

		const Scalar v00 = E_avg_TABLE_G2_ANISO[ri0][ai0];
		const Scalar v01 = E_avg_TABLE_G2_ANISO[ri0][ai1];
		const Scalar v10 = E_avg_TABLE_G2_ANISO[ri1][ai0];
		const Scalar v11 = E_avg_TABLE_G2_ANISO[ri1][ai1];
		const Scalar v0 = (1-af)*v00 + af*v01;
		const Scalar v1 = (1-af)*v10 + af*v11;
		return (1-rf)*v0 + rf*v1;
	}

	/// DL-77: anisotropic twin of MSLobeZG2.  Z is a LINEAR functional of
	/// the essRow (see MSLobeZ's own perf-note comment above), so the
	/// per-corner Z values can be precomputed once and then blended with
	/// the SAME bilinear (alphaEff, ratio) weights LookupEssG2Aniso/
	/// LookupEavgG2Aniso use -- exactly how MSLobeZG2 blends across alpha
	/// alone, just with a second (ratio) dimension.
	inline Scalar MSLobeZG2Aniso( const Scalar alphaX, const Scalar alphaY )
	{
		if( fabs(alphaX - alphaY) < 1e-9 ) return MSLobeZG2( alphaX );

		static const std::array<std::array<Scalar, ANISO_ALPHA_SIZE>, ANISO_RATIO_SIZE> rowZ = []() {
			std::array<std::array<Scalar, ANISO_ALPHA_SIZE>, ANISO_RATIO_SIZE> z{};
			for( int rj = 0; rj < ANISO_RATIO_SIZE; rj++ )
			{
				for( int ai = 0; ai < ANISO_ALPHA_SIZE; ai++ )
				{
					MSLobeDetail::Segment segs[ANISO_COS_SIZE + 1];
					int nSegs = 0;
					MSLobeDetail::BuildSegmentsFromRowN( E_ss_TABLE_G2_ANISO[rj][ai], ANISO_COS_SIZE, segs, nSegs );
					Scalar I = 0.0;
					for( int i = 0; i < nSegs; i++ )
						I += MSLobeDetail::SegTotal( segs[i] );
					z[rj][ai] = 2.0 * I;
				}
			}
			return z;
		}();

		const Scalar aX = r_max(0.01, r_min(1.0, alphaX));
		const Scalar aY = r_max(0.01, r_min(1.0, alphaY));
		const Scalar alphaEff = sqrt( aX * aY );
		const Scalar ratio = r_max(aX, aY) / r_min(aX, aY);

		int ai0, ai1; Scalar af;
		AnisoAlphaIndex( alphaEff, ai0, ai1, af );
		int ri0, ri1; Scalar rf;
		AnisoRatioIndex( ratio, ri0, ri1, rf );

		const Scalar v0 = (1-af)*rowZ[ri0][ai0] + af*rowZ[ri0][ai1];
		const Scalar v1 = (1-af)*rowZ[ri1][ai0] + af*rowZ[ri1][ai1];
		return (1-rf)*v0 + rf*v1;
	}

	/// DL-77: anisotropic twin of SampleMSCosThetaG2.  Unlike Z (a linear
	/// functional, exactly interpolable), the CDF-inversion sampler needs
	/// the actual blended essRow at the query (alphaX,alphaY) -- blend the
	/// 4 corner rows with the SAME bilinear weights LookupEssG2Aniso uses,
	/// then invert exactly as SampleMSCosTheta does.
	inline Scalar SampleMSCosThetaG2Aniso( const Scalar alphaX, const Scalar alphaY, const Scalar u1 )
	{
		if( fabs(alphaX - alphaY) < 1e-9 ) return SampleMSCosThetaG2( alphaX, u1 );

		const Scalar aX = r_max(0.01, r_min(1.0, alphaX));
		const Scalar aY = r_max(0.01, r_min(1.0, alphaY));
		const Scalar alphaEff = sqrt( aX * aY );
		const Scalar ratio = r_max(aX, aY) / r_min(aX, aY);

		int ai0, ai1; Scalar af;
		AnisoAlphaIndex( alphaEff, ai0, ai1, af );
		int ri0, ri1; Scalar rf;
		AnisoRatioIndex( ratio, ri0, ri1, rf );

		Scalar essRow[ANISO_COS_SIZE];
		for( int k = 0; k < ANISO_COS_SIZE; k++ )
		{
			const Scalar v0 = (1-af)*E_ss_TABLE_G2_ANISO[ri0][ai0][k] + af*E_ss_TABLE_G2_ANISO[ri0][ai1][k];
			const Scalar v1 = (1-af)*E_ss_TABLE_G2_ANISO[ri1][ai0][k] + af*E_ss_TABLE_G2_ANISO[ri1][ai1][k];
			essRow[k] = (1-rf)*v0 + rf*v1;
		}

		MSLobeDetail::Segment segs[ANISO_COS_SIZE + 1];
		int nSegs = 0;
		MSLobeDetail::BuildSegmentsFromRowN( essRow, ANISO_COS_SIZE, segs, nSegs );

		Scalar totals[ANISO_COS_SIZE + 1];
		Scalar I = 0.0;
		for( int i = 0; i < nSegs; i++ )
		{
			totals[i] = MSLobeDetail::SegTotal( segs[i] );
			I += totals[i];
		}
		if( I <= 1e-15 ) return r_max( 0.0, r_min( 1.0, u1 ) );	// degenerate fallback

		const Scalar target = r_max( 0.0, r_min( I, u1 * I ) );
		int seg = 0;
		Scalar running = 0.0;
		while( seg < nSegs - 1 && running + totals[seg] < target )
		{
			running += totals[seg];
			seg++;
		}
		const Scalar localTarget = r_max( 0.0, r_min( totals[seg], target - running ) );
		const Scalar y = MSLobeDetail::SegInvert( segs[seg], localTarget, segs[seg].hi - segs[seg].lo );
		return r_max( 0.0, r_min( 1.0, segs[seg].lo + y ) );
	}

	/// DL-77: anisotropic twin of MSPdfG2.  Naturally reduces to the
	/// isotropic formula (and hence to byte-identical output) when
	/// alphaX==alphaY, since LookupEssG2Aniso itself forwards there.
	inline Scalar MSPdfG2Aniso( const Scalar cosTheta, const Scalar alphaX, const Scalar alphaY, const Scalar Z )
	{
		if( Z <= 1e-15 ) return 0.0;
		const Scalar c = r_max( 0.0, r_min( 1.0, cosTheta ) );
		return r_max( 0.0, (1.0 - LookupEssG2Aniso( c, alphaX, alphaY )) * c / (RISE::PI * Z) );
	}
)DL77ANISOBLOCK";

int main() {
	double E_ss[LUT_SIZE][LUT_SIZE]; // [alphaIdx][cosThetaIdx]
	double E_avg[LUT_SIZE];
	// DL-63: height-correlated-G2 twin of E_ss/E_avg above, for the
	// height-correlated single-scatter consumers (GGXBRDF/GGXSPF,
	// CoatedBRDF) -- see GGX_G2_HeightCorrelated's doc comment.
	double E_ss_G2[LUT_SIZE][LUT_SIZE];
	double E_avg_G2[LUT_SIZE];

	// Compute E_ss for each (alpha, cosTheta) pair
	for(int ai = 0; ai < LUT_SIZE; ai++) {
		double alpha = (double)(ai + 0.5) / LUT_SIZE; // [0.015625, 0.984375]
		// Map to [0.01, 1.0] range
		alpha = 0.01 + (1.0 - 0.01) * (double)ai / (double)(LUT_SIZE - 1);

		for(int ci = 0; ci < LUT_SIZE; ci++) {
			double cosTheta = (double)(ci + 0.5) / LUT_SIZE;
			// Map to (0, 1] range
			cosTheta = (double)(ci + 0.5) / LUT_SIZE;

			// Construct wi in local coordinates (normal = z-axis)
			double sinTheta = sqrt(fmax(0.0, 1.0 - cosTheta * cosTheta));
			Vec3 wi(sinTheta, 0.0, cosTheta);

			double sum = 0.0;
			double sumG2 = 0.0;
			// cosWi is fixed for this row (= cosTheta); G1(wi) under the
			// height-correlated model is 1/(1+Lambda(wi)), needed by the
			// VNDF-sampling weight identity below.
			const double G1wi = 1.0 / (1.0 + GGX_Lambda(alpha, cosTheta));

			for(int s = 0; s < NUM_SAMPLES; s++) {
				double u1 = rand01();
				double u2 = rand01();

				Vec3 m = VNDF_Sample_Local(wi, alpha, u1, u2);
				double wiDotM = dot(wi, m);
				if(wiDotM <= 0) continue;

				// Reflect wi about m to get wo
				Vec3 wo(
					2.0 * wiDotM * m.x - wi.x,
					2.0 * wiDotM * m.y - wi.y,
					2.0 * wiDotM * m.z - wi.z
				);
				wo = normalize(wo);

				double cosWo = wo.z; // dot(wo, normal)
				if(cosWo > 0) {
					// With VNDF sampling and F=1, the estimator for E_ss
					// under the SEPARABLE model (G = G1(wi)*G1(wo)) is:
					// E_ss = (1/N) * sum G1(wo)   [G1(wi) cancels against
					// the VNDF pdf's own G1(wi) factor].
					sum += GGX_G1(alpha, cosWo);

					// DL-63: under the HEIGHT-CORRELATED model (Heitz
					// 2018, "Sampling the GGX Distribution of Visible
					// Normals", eq. for f*cosWo/pdf(wo) with VNDF
					// sampling), the analogous per-sample weight is
					// G2(wi,wo)/G1(wi) -- NOT G1(wo) -- because G2 does
					// not factor into G1(wi)*G1(wo).
					sumG2 += GGX_G2_HeightCorrelated(alpha, cosTheta, cosWo) / G1wi;
				}
			}

			E_ss[ai][ci] = sum / (double)NUM_SAMPLES;
			E_ss_G2[ai][ci] = sumG2 / (double)NUM_SAMPLES;
		}

		// Compute E_avg for this alpha using trapezoidal integration
		// E_avg = 2 * integral_0^1 E_ss(mu) * mu d_mu
		double integral = 0.0;
		double integralG2 = 0.0;
		for(int ci = 0; ci < LUT_SIZE; ci++) {
			double mu = (double)(ci + 0.5) / LUT_SIZE;
			double dmu = 1.0 / LUT_SIZE;
			integral += E_ss[ai][ci] * mu * dmu;
			integralG2 += E_ss_G2[ai][ci] * mu * dmu;
		}
		E_avg[ai] = 2.0 * integral;
		E_avg_G2[ai] = 2.0 * integralG2;

		fprintf(stderr, "alpha=%.4f  E_avg=%.6f  E_avg_G2=%.6f\n", alpha, E_avg[ai], E_avg_G2[ai]);
	}

	// DL-77: anisotropic height-correlated-G2 E_ss/E_avg, resolved by
	// (ratio, alphaEff, phi, cosTheta) -- see the ANISO_PHI_SIZE comment
	// above and the kHandMaintainedDL77AnisoBlock header comment below for
	// the phi-grid rationale and the alphaX,alphaY<->(alphaEff,ratio)
	// mapping.  E_ss_G2_ANISO_PHI is the primary (phi-resolved) bake;
	// E_ss_G2_ANISO (no phi) is DERIVED from it by trapezoidal-averaging
	// over phi, then E_avg_G2_ANISO is derived from THAT exactly as the
	// isotropic tables derive E_avg from E_ss.
	static double E_ss_G2_ANISO_PHI[ANISO_RATIO_SIZE][ANISO_ALPHA_SIZE][ANISO_PHI_SIZE][ANISO_COS_SIZE];
	static double E_ss_G2_ANISO[ANISO_RATIO_SIZE][ANISO_ALPHA_SIZE][ANISO_COS_SIZE];
	static double E_avg_G2_ANISO[ANISO_RATIO_SIZE][ANISO_ALPHA_SIZE];

	for(int rj = 0; rj < ANISO_RATIO_SIZE; rj++) {
		const double ratio = pow(ANISO_RATIO_MAX, (double)rj / (double)(ANISO_RATIO_SIZE - 1));
		const double sqrtRatio = sqrt(ratio);

		for(int ai = 0; ai < ANISO_ALPHA_SIZE; ai++) {
			const double alphaEff = 0.01 + (1.0 - 0.01) * (double)ai / (double)(ANISO_ALPHA_SIZE - 1);
			const double alphaX = alphaEff / sqrtRatio;
			const double alphaY = alphaEff * sqrtRatio;

			for(int pi = 0; pi < ANISO_PHI_SIZE; pi++) {
				// Endpoint-inclusive grid over [0,90] degrees -- phi=0 and
				// phi=90 (wi aligned with one tangent axis or the other)
				// are EXACT table entries, not interpolated, since those
				// are the azimuths TestSchlickSweep's anisotropic rows
				// actually probe.
				const double phiDeg = (double)pi * 90.0 / (double)(ANISO_PHI_SIZE - 1);
				const double phi = phiDeg * PI / 180.0;
				const double cosPhi = cos(phi);
				const double sinPhi = sin(phi);

				for(int ci = 0; ci < ANISO_COS_SIZE; ci++) {
					const double cosTheta = ((double)ci + 0.5) / ANISO_COS_SIZE;
					const double sinTheta = sqrt(fmax(0.0, 1.0 - cosTheta * cosTheta));

					// wi's azimuth is FIXED at this grid point (not
					// randomized) -- this is the fix for the averaged
					// table's per-azimuth over/under-shoot (see
					// ANISO_PHI_SIZE comment above).
					const Vec3 wi(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta);
					const double G1wi = GGX_G1_Aniso(alphaX, alphaY, wi);

					double sumG2 = 0.0;
					for(int s = 0; s < NUM_SAMPLES_ANISO; s++) {
						const double u1 = rand01();
						const double u2 = rand01();
						const Vec3 m = VNDF_Sample_Local_Aniso(wi, alphaX, alphaY, u1, u2);
						const double wiDotM = dot(wi, m);
						if(wiDotM <= 0) continue;

						Vec3 wo(
							2.0 * wiDotM * m.x - wi.x,
							2.0 * wiDotM * m.y - wi.y,
							2.0 * wiDotM * m.z - wi.z
						);
						wo = normalize(wo);

						const double cosWo = wo.z;
						if(cosWo > 0) {
							if(G1wi > 1e-12)
								sumG2 += GGX_G2_Aniso_HeightCorrelated(alphaX, alphaY, wi, wo) / G1wi;
						}
					}

					E_ss_G2_ANISO_PHI[rj][ai][pi][ci] = sumG2 / (double)NUM_SAMPLES_ANISO;
				}
			}

			// Derive the phi-averaged row via trapezoidal quadrature over
			// the endpoint-inclusive phi grid (half-weight at phi=0/90,
			// full weight interior, normalized by the number of
			// intervals) -- reproduces the same "uniform azimuth average"
			// quantity the pre-fix generator computed via random phi
			// draws, exploiting the ellipse's quarter-period symmetry.
			for(int ci = 0; ci < ANISO_COS_SIZE; ci++) {
				double trapz = 0.5 * E_ss_G2_ANISO_PHI[rj][ai][0][ci] + 0.5 * E_ss_G2_ANISO_PHI[rj][ai][ANISO_PHI_SIZE-1][ci];
				for(int pi = 1; pi < ANISO_PHI_SIZE - 1; pi++)
					trapz += E_ss_G2_ANISO_PHI[rj][ai][pi][ci];
				E_ss_G2_ANISO[rj][ai][ci] = trapz / (double)(ANISO_PHI_SIZE - 1);
			}

			double integralAniso = 0.0;
			for(int ci = 0; ci < ANISO_COS_SIZE; ci++) {
				const double mu = ((double)ci + 0.5) / ANISO_COS_SIZE;
				const double dmu = 1.0 / ANISO_COS_SIZE;
				integralAniso += E_ss_G2_ANISO[rj][ai][ci] * mu * dmu;
			}
			E_avg_G2_ANISO[rj][ai] = 2.0 * integralAniso;

			fprintf(stderr, "aniso ratio=%.4f alphaEff=%.4f (aX=%.4f aY=%.4f)  E_avg_G2_ANISO=%.6f\n",
				ratio, alphaEff, alphaX, alphaY, E_avg_G2_ANISO[rj][ai]);
		}
	}

	// Emit the header
	printf("//////////////////////////////////////////////////////////////////////\n");
	printf("//\n");
	printf("//  MicrofacetEnergyLUT.h - Precomputed directional albedo tables\n");
	printf("//    for Kulla-Conty multiscattering energy compensation.\n");
	printf("//\n");
	printf("//  AUTO-GENERATED by tools/GenerateMicrofacetEnergyLUT.cpp\n");
	printf("//  DO NOT EDIT BY HAND.\n");
	printf("//\n");
	printf("//  References:\n");
	printf("//    - Kulla & Conty, \"Revisiting Physically Based Shading at\n");
	printf("//      Imageworks\", SIGGRAPH 2017 Course\n");
	printf("//    - Turquin, \"Practical Multiple Scattering Compensation for\n");
	printf("//      Microfacet Models\", ILM 2019\n");
	printf("//\n");
	printf("//  LUT resolution: %d x %d\n", LUT_SIZE, LUT_SIZE);
	printf("//  Samples per entry: %d\n", NUM_SAMPLES);
	printf("//  Alpha range: [0.01, 1.0] (uniform %d steps)\n", LUT_SIZE);
	printf("//  CosTheta range: [0.5/%d, (%.1f)/%d] (cell centers)\n", LUT_SIZE, LUT_SIZE - 0.5, LUT_SIZE);
	printf("//  DL-77 aniso LUT resolution: %d ratio x %d alphaEff x %d phi x %d cosTheta\n", ANISO_RATIO_SIZE, ANISO_ALPHA_SIZE, ANISO_PHI_SIZE, ANISO_COS_SIZE);
	printf("//  DL-77 aniso samples per entry: %d\n", NUM_SAMPLES_ANISO);
	printf("//  DL-77 aniso ratio range: [1, %.0f] (log-spaced %d steps)\n", ANISO_RATIO_MAX, ANISO_RATIO_SIZE);
	printf("//  DL-77 aniso phi range: [0, 90] degrees (endpoint-inclusive %d steps)\n", ANISO_PHI_SIZE);
	printf("//\n");
	printf("//  Provenance (P2-4, debt-ggx2; extended DL-77, debt-ggx3): this\n");
	printf("//  exact file reproduces byte-for-byte via the fixed RNG seed baked\n");
	printf("//  into this generator's rng_state initializer (%lluULL) with %d\n", (unsigned long long)1234567890123456789ULL, NUM_SAMPLES);
	printf("//  samples/entry (isotropic tables) and %d samples/entry (DL-77\n", NUM_SAMPLES_ANISO);
	printf("//  aniso tables, drawn from the SAME rng_state stream immediately\n");
	printf("//  afterward).  Regenerate + verify with:\n");
	printf("//    c++ -O2 -Isrc/Library -std=c++11 -o tools/gen_lut \\\n");
	printf("//        tools/GenerateMicrofacetEnergyLUT.cpp -lm\n");
	printf("//    tools/gen_lut > /tmp/regen_MicrofacetEnergyLUT.h\n");
	printf("//    diff src/Library/Utilities/MicrofacetEnergyLUT.h /tmp/regen_MicrofacetEnergyLUT.h\n");
	printf("//  The hand-derived H6/DL-63 multiscatter-lobe sampler/pdf\n");
	printf("//  machinery (MSLobeDetail, MSLobeZ*, SampleMSCosTheta*, MSPdf*) and\n");
	printf("//  the DL-77 anisotropic twins (AnisoAlphaIndex/AnisoRatioIndex,\n");
	printf("//  LookupEssG2Aniso, LookupEavgG2Aniso, MSLobeZG2Aniso,\n");
	printf("//  SampleMSCosThetaG2Aniso, MSPdfG2Aniso) are embedded verbatim in\n");
	printf("//  this generator (kHandMaintainedH6Block, kHandMaintainedDL77AnisoBlock)\n");
	printf("//  and are NOT re-derived by the Monte-Carlo bake above.\n");
	printf("//\n");
	printf("//  Author: Aravind Krishnaswamy\n");
	printf("//  Date of Birth: March 28, 2026\n");
	printf("//  Tabs: 4\n");
	printf("//\n");
	printf("//  License Information: Please see the attached LICENSE.TXT file\n");
	printf("//\n");
	printf("//////////////////////////////////////////////////////////////////////\n\n");

	printf("#ifndef MICROFACET_ENERGY_LUT_\n");
	printf("#define MICROFACET_ENERGY_LUT_\n\n");
	printf("#include \"Math3D/Math3D.h\"\n");
	printf("#include \"Optics.h\"\n");
	printf("#include \"math_utils.h\"\n");
	printf("#include <array>\n\n");

	printf("namespace RISE\n{\n");
	printf("namespace MicrofacetEnergyLUT\n{\n");

	// Emit constants
	printf("\tstatic const int LUT_SIZE = %d;\n\n", LUT_SIZE);

	// Emit E_ss table
	printf("\t/// Directional albedo E_ss(alpha, cosTheta) of GGX single-scatter BRDF with F=1.\n");
	printf("\t/// Indexed as E_ss_TABLE[alphaIdx][cosThetaIdx].\n");
	printf("\t/// Alpha mapped linearly from 0.01 to 1.0, cosTheta from cell centers.\n");
	printf("\tstatic const Scalar E_ss_TABLE[%d][%d] = {\n", LUT_SIZE, LUT_SIZE);
	for(int ai = 0; ai < LUT_SIZE; ai++) {
		printf("\t\t{ ");
		for(int ci = 0; ci < LUT_SIZE; ci++) {
			printf("%.8f", E_ss[ai][ci]);
			if(ci < LUT_SIZE - 1) printf(", ");
		}
		printf(" }");
		if(ai < LUT_SIZE - 1) printf(",");
		printf("\n");
	}
	printf("\t};\n\n");

	// Emit E_avg table
	printf("\t/// Cosine-weighted hemisphere average of E_ss per roughness.\n");
	printf("\t/// E_avg(alpha) = 2 * integral_0^1 E_ss(alpha, mu) * mu d_mu\n");
	printf("\tstatic const Scalar E_avg_TABLE[%d] = {\n\t\t", LUT_SIZE);
	for(int ai = 0; ai < LUT_SIZE; ai++) {
		printf("%.8f", E_avg[ai]);
		if(ai < LUT_SIZE - 1) printf(", ");
		if((ai + 1) % 8 == 0 && ai < LUT_SIZE - 1) printf("\n\t\t");
	}
	printf("\n\t};\n\n");

	// DL-63: height-correlated-G2 twin tables.  E_ss_TABLE/E_avg_TABLE
	// above are calibrated to the SEPARABLE Smith model
	// (MicrofacetUtils::GGX_G, G1(wi)*G1(wo)) that CookTorranceBRDF/SPF
	// actually render with, and stay exactly as they are for that
	// consumer.  E_ss_TABLE_G2/E_avg_TABLE_G2 are calibrated to the
	// HEIGHT-CORRELATED Smith G2 model (MicrofacetUtils::GGX_G2/
	// GGX_G2_Aniso) that GGXBRDF/GGXSPF/CoatedBRDF actually render
	// with -- using a DIFFERENT compensation table for a DIFFERENT
	// masking-shadowing model is what the Kulla-Conty multiscatter
	// energy-conservation identity requires; see docs/DL62_DL64_GGX_
	// SAMPLE_EVAL_MISMATCH.md and the DL-63 ledger row.
	printf("\t/// DL-63: height-correlated-G2 twin of E_ss_TABLE above --\n");
	printf("\t/// directional albedo of GGX single-scatter BRDF with F=1,\n");
	printf("\t/// under Smith HEIGHT-CORRELATED G2 masking-shadowing\n");
	printf("\t/// (MicrofacetUtils::GGX_G2/GGX_G2_Aniso), NOT the separable\n");
	printf("\t/// G1(wi)*G1(wo) model E_ss_TABLE calibrates to.  Consumed by\n");
	printf("\t/// GGXBRDF/GGXSPF/CoatedBRDF, which render with height-\n");
	printf("\t/// correlated G2; CookTorranceBRDF/SPF (separable G) keep\n");
	printf("\t/// using E_ss_TABLE/LookupEss, unchanged.  Same indexing,\n");
	printf("\t/// resolution and sample count as E_ss_TABLE.\n");
	printf("\tstatic const Scalar E_ss_TABLE_G2[%d][%d] = {\n", LUT_SIZE, LUT_SIZE);
	for(int ai = 0; ai < LUT_SIZE; ai++) {
		printf("\t\t{ ");
		for(int ci = 0; ci < LUT_SIZE; ci++) {
			printf("%.8f", E_ss_G2[ai][ci]);
			if(ci < LUT_SIZE - 1) printf(", ");
		}
		printf(" }");
		if(ai < LUT_SIZE - 1) printf(",");
		printf("\n");
	}
	printf("\t};\n\n");

	printf("\t/// DL-63: height-correlated-G2 twin of E_avg_TABLE above.\n");
	printf("\tstatic const Scalar E_avg_TABLE_G2[%d] = {\n\t\t", LUT_SIZE);
	for(int ai = 0; ai < LUT_SIZE; ai++) {
		printf("%.8f", E_avg_G2[ai]);
		if(ai < LUT_SIZE - 1) printf(", ");
		if((ai + 1) % 8 == 0 && ai < LUT_SIZE - 1) printf("\n\t\t");
	}
	printf("\n\t};\n\n");

	// Emit lookup functions
	printf("\t/// Look up E_ss(cosTheta, alpha) with bilinear interpolation.\n");
	printf("\tinline Scalar LookupEss( const Scalar cosTheta, const Scalar alpha )\n");
	printf("\t{\n");
	printf("\t\t// Map alpha from [0.01, 1.0] to [0, LUT_SIZE-1]\n");
	printf("\t\tScalar a = r_max(0.0, r_min(1.0, (alpha - 0.01) / (1.0 - 0.01))) * (LUT_SIZE - 1);\n");
	printf("\t\tint ai0 = (int)a;\n");
	printf("\t\tint ai1 = r_min(ai0 + 1, LUT_SIZE - 1);\n");
	printf("\t\tScalar af = a - ai0;\n\n");
	printf("\t\t// Map cosTheta from cell centers: idx = cosTheta * LUT_SIZE - 0.5\n");
	printf("\t\tScalar c = r_max(0.0, r_min(1.0, cosTheta)) * LUT_SIZE - 0.5;\n");
	printf("\t\tif( c < 0 ) c = 0;\n");
	printf("\t\tint ci0 = (int)c;\n");
	printf("\t\tint ci1 = r_min(ci0 + 1, LUT_SIZE - 1);\n");
	printf("\t\tScalar cf = c - ci0;\n\n");
	printf("\t\t// Bilinear interpolation\n");
	printf("\t\tScalar v00 = E_ss_TABLE[ai0][ci0];\n");
	printf("\t\tScalar v01 = E_ss_TABLE[ai0][ci1];\n");
	printf("\t\tScalar v10 = E_ss_TABLE[ai1][ci0];\n");
	printf("\t\tScalar v11 = E_ss_TABLE[ai1][ci1];\n");
	printf("\t\treturn (1-af) * ((1-cf)*v00 + cf*v01) + af * ((1-cf)*v10 + cf*v11);\n");
	printf("\t}\n\n");

	printf("\t/// Look up E_avg(alpha) with linear interpolation.\n");
	printf("\tinline Scalar LookupEavg( const Scalar alpha )\n");
	printf("\t{\n");
	printf("\t\tScalar a = r_max(0.0, r_min(1.0, (alpha - 0.01) / (1.0 - 0.01))) * (LUT_SIZE - 1);\n");
	printf("\t\tint ai0 = (int)a;\n");
	printf("\t\tint ai1 = r_min(ai0 + 1, LUT_SIZE - 1);\n");
	printf("\t\tScalar af = a - ai0;\n");
	printf("\t\treturn (1-af) * E_avg_TABLE[ai0] + af * E_avg_TABLE[ai1];\n");
	printf("\t}\n\n");

	// DL-63: height-correlated-G2 twins of LookupEss/LookupEavg above.
	printf("\t/// DL-63: height-correlated-G2 twin of LookupEss above -- reads\n");
	printf("\t/// E_ss_TABLE_G2 instead of E_ss_TABLE.  Use for GGXBRDF/GGXSPF/\n");
	printf("\t/// CoatedBRDF (height-correlated G2 single-scatter); CookTorrance\n");
	printf("\t/// keeps using LookupEss (separable G).\n");
	printf("\tinline Scalar LookupEssG2( const Scalar cosTheta, const Scalar alpha )\n");
	printf("\t{\n");
	printf("\t\tScalar a = r_max(0.0, r_min(1.0, (alpha - 0.01) / (1.0 - 0.01))) * (LUT_SIZE - 1);\n");
	printf("\t\tint ai0 = (int)a;\n");
	printf("\t\tint ai1 = r_min(ai0 + 1, LUT_SIZE - 1);\n");
	printf("\t\tScalar af = a - ai0;\n\n");
	printf("\t\tScalar c = r_max(0.0, r_min(1.0, cosTheta)) * LUT_SIZE - 0.5;\n");
	printf("\t\tif( c < 0 ) c = 0;\n");
	printf("\t\tint ci0 = (int)c;\n");
	printf("\t\tint ci1 = r_min(ci0 + 1, LUT_SIZE - 1);\n");
	printf("\t\tScalar cf = c - ci0;\n\n");
	printf("\t\tScalar v00 = E_ss_TABLE_G2[ai0][ci0];\n");
	printf("\t\tScalar v01 = E_ss_TABLE_G2[ai0][ci1];\n");
	printf("\t\tScalar v10 = E_ss_TABLE_G2[ai1][ci0];\n");
	printf("\t\tScalar v11 = E_ss_TABLE_G2[ai1][ci1];\n");
	printf("\t\treturn (1-af) * ((1-cf)*v00 + cf*v01) + af * ((1-cf)*v10 + cf*v11);\n");
	printf("\t}\n\n");

	printf("\t/// DL-63: height-correlated-G2 twin of LookupEavg above.\n");
	printf("\tinline Scalar LookupEavgG2( const Scalar alpha )\n");
	printf("\t{\n");
	printf("\t\tScalar a = r_max(0.0, r_min(1.0, (alpha - 0.01) / (1.0 - 0.01))) * (LUT_SIZE - 1);\n");
	printf("\t\tint ai0 = (int)a;\n");
	printf("\t\tint ai1 = r_min(ai0 + 1, LUT_SIZE - 1);\n");
	printf("\t\tScalar af = a - ai0;\n");
	printf("\t\treturn (1-af) * E_avg_TABLE_G2[ai0] + af * E_avg_TABLE_G2[ai1];\n");
	printf("\t}\n\n");

	fputs( kHandMaintainedH6Block, stdout );
	printf("\n");

	// DL-77: anisotropic Kulla-Conty compensation tables + lookup/H6
	// machinery.  See kHandMaintainedDL77AnisoBlock's own header comment
	// (emitted below) for the full rationale.
	printf("\tstatic const int ANISO_ALPHA_SIZE = %d;\n", ANISO_ALPHA_SIZE);
	printf("\tstatic const int ANISO_RATIO_SIZE = %d;\n", ANISO_RATIO_SIZE);
	printf("\tstatic const int ANISO_COS_SIZE = %d;\n", ANISO_COS_SIZE);
	printf("\tstatic const int ANISO_PHI_SIZE = %d;\n", ANISO_PHI_SIZE);
	printf("\tstatic const Scalar ANISO_RATIO_MAX = %.1f;\n\n", ANISO_RATIO_MAX);

	printf("\t/// DL-77: per-azimuth (NOT azimuthally-averaged) height-\n");
	printf("\t/// correlated-G2 single-scatter directional albedo -- the\n");
	printf("\t/// primary bake; E_ss_TABLE_G2_ANISO below is DERIVED from this\n");
	printf("\t/// by trapezoidal-averaging over phi.  Indexed as\n");
	printf("\t/// E_ss_TABLE_G2_ANISO_PHI[ratioIdx][alphaEffIdx][phiIdx][cosThetaIdx].\n");
	printf("\t/// phi is wi's azimuth relative to the tangent/bitangent axes,\n");
	printf("\t/// grid points at 0,15,...,90 degrees INCLUSIVE of both\n");
	printf("\t/// endpoints (ellipse quarter-period symmetry -- see the\n");
	printf("\t/// ANISO_PHI_SIZE comment in the generator).  Consumed by\n");
	printf("\t/// LookupEssG2AnisoDirectional, used ONLY at the energy-\n");
	printf("\t/// compensation Ess_i/Ess_o call sites in GGXBRDF.cpp/\n");
	printf("\t/// GGXSPF.cpp -- NOT by the H6 multiscatter-lobe sampler, which\n");
	printf("\t/// keeps using the phi-averaged E_ss_TABLE_G2_ANISO below (an\n");
	printf("\t/// importance-sampling proposal shape; exactness there is an\n");
	printf("\t/// efficiency concern, not a correctness one).\n");
	printf("\tstatic const Scalar E_ss_TABLE_G2_ANISO_PHI[%d][%d][%d][%d] = {\n", ANISO_RATIO_SIZE, ANISO_ALPHA_SIZE, ANISO_PHI_SIZE, ANISO_COS_SIZE);
	for(int rj = 0; rj < ANISO_RATIO_SIZE; rj++) {
		printf("\t\t{\n");
		for(int ai = 0; ai < ANISO_ALPHA_SIZE; ai++) {
			printf("\t\t\t{\n");
			for(int pi = 0; pi < ANISO_PHI_SIZE; pi++) {
				printf("\t\t\t\t{ ");
				for(int ci = 0; ci < ANISO_COS_SIZE; ci++) {
					printf("%.8f", E_ss_G2_ANISO_PHI[rj][ai][pi][ci]);
					if(ci < ANISO_COS_SIZE - 1) printf(", ");
				}
				printf(" }");
				if(pi < ANISO_PHI_SIZE - 1) printf(",");
				printf("\n");
			}
			printf("\t\t\t}");
			if(ai < ANISO_ALPHA_SIZE - 1) printf(",");
			printf("\n");
		}
		printf("\t\t}");
		if(rj < ANISO_RATIO_SIZE - 1) printf(",");
		printf("\n");
	}
	printf("\t};\n\n");

	printf("\t/// DL-77: azimuthally-averaged height-correlated-G2 single-\n");
	printf("\t/// scatter directional albedo, resolved by anisotropy RATIO in\n");
	printf("\t/// addition to alphaEff and cosTheta.  Indexed as\n");
	printf("\t/// E_ss_TABLE_G2_ANISO[ratioIdx][alphaEffIdx][cosThetaIdx].\n");
	printf("\t/// Ratio mapped log-spaced from 1 to ANISO_RATIO_MAX, alphaEff\n");
	printf("\t/// linearly from 0.01 to 1.0, cosTheta from cell centers.  See\n");
	printf("\t/// the DL-77 hand-maintained block below for how alphaX,alphaY\n");
	printf("\t/// map to (ratio, alphaEff) and back.\n");
	printf("\tstatic const Scalar E_ss_TABLE_G2_ANISO[%d][%d][%d] = {\n", ANISO_RATIO_SIZE, ANISO_ALPHA_SIZE, ANISO_COS_SIZE);
	for(int rj = 0; rj < ANISO_RATIO_SIZE; rj++) {
		printf("\t\t{\n");
		for(int ai = 0; ai < ANISO_ALPHA_SIZE; ai++) {
			printf("\t\t\t{ ");
			for(int ci = 0; ci < ANISO_COS_SIZE; ci++) {
				printf("%.8f", E_ss_G2_ANISO[rj][ai][ci]);
				if(ci < ANISO_COS_SIZE - 1) printf(", ");
			}
			printf(" }");
			if(ai < ANISO_ALPHA_SIZE - 1) printf(",");
			printf("\n");
		}
		printf("\t\t}");
		if(rj < ANISO_RATIO_SIZE - 1) printf(",");
		printf("\n");
	}
	printf("\t};\n\n");

	printf("\t/// DL-77: azimuthally-averaged hemisphere average of\n");
	printf("\t/// E_ss_TABLE_G2_ANISO per (ratio, alphaEff).\n");
	printf("\tstatic const Scalar E_avg_TABLE_G2_ANISO[%d][%d] = {\n", ANISO_RATIO_SIZE, ANISO_ALPHA_SIZE);
	for(int rj = 0; rj < ANISO_RATIO_SIZE; rj++) {
		printf("\t\t{ ");
		for(int ai = 0; ai < ANISO_ALPHA_SIZE; ai++) {
			printf("%.8f", E_avg_G2_ANISO[rj][ai]);
			if(ai < ANISO_ALPHA_SIZE - 1) printf(", ");
		}
		printf(" }");
		if(rj < ANISO_RATIO_SIZE - 1) printf(",");
		printf("\n");
	}
	printf("\t};\n\n");

	fputs( kHandMaintainedDL77AnisoBlock, stdout );
	printf("\n");

	// Fresnel averaging via Gauss-Legendre quadrature
	printf("\t/// 21-point Gauss-Legendre quadrature nodes on [0,1].\n");
	printf("\tstatic const int GL_N = 21;\n");
	printf("\tstatic const Scalar GL_nodes[21] = {\n\t\t");
	// 21-point GL nodes on [0,1] (transformed from [-1,1])
	double gl_nodes_11[] = {
		-0.9937521706203895, -0.9672268385663063, -0.9200993341504008,
		-0.8533633645833173, -0.7684399634756779, -0.6671388041974123,
		-0.5516188358872198, -0.4243421202074388, -0.2880213168024011,
		-0.1455618541608951, 0.0,
		0.1455618541608951, 0.2880213168024011, 0.4243421202074388,
		0.5516188358872198, 0.6671388041974123, 0.7684399634756779,
		0.8533633645833173, 0.9200993341504008, 0.9672268385663063,
		0.9937521706203895
	};
	double gl_weights_11[] = {
		0.0160172282577743, 0.0369537897708525, 0.0571344254268572,
		0.0761001136283793, 0.0934444234560339, 0.1087972991671484,
		0.1218314160537285, 0.1322689386333375, 0.1398873947910732,
		0.1445244789050542, 0.1460811308764353,
		0.1445244789050542, 0.1398873947910732, 0.1322689386333375,
		0.1218314160537285, 0.1087972991671484, 0.0934444234560339,
		0.0761001136283793, 0.0571344254268572, 0.0369537897708525,
		0.0160172282577743
	};
	for(int i = 0; i < 21; i++) {
		double node_01 = 0.5 * (gl_nodes_11[i] + 1.0);
		printf("%.16f", node_01);
		if(i < 20) printf(", ");
		if((i+1) % 4 == 0) printf("\n\t\t");
	}
	printf("\n\t};\n\n");

	printf("\t/// 21-point Gauss-Legendre quadrature weights on [0,1].\n");
	printf("\tstatic const Scalar GL_weights[21] = {\n\t\t");
	for(int i = 0; i < 21; i++) {
		double weight_01 = 0.5 * gl_weights_11[i];
		printf("%.16f", weight_01);
		if(i < 20) printf(", ");
		if((i+1) % 4 == 0) printf("\n\t\t");
	}
	printf("\n\t};\n\n");

	// ComputeFresnelAvg template function
	printf("\t/// Compute the hemispherical Fresnel average for a conductor:\n");
	printf("\t///   F_avg = 2 * integral_0^1 F(mu) * mu d_mu\n");
	printf("\t/// Uses 21-point Gauss-Legendre quadrature.\n");
	printf("\ttemplate< class T >\n");
	printf("\tinline T ComputeFresnelAvg( const Vector3& n, const T& Ni, const T& Nt, const T& kt )\n");
	printf("\t{\n");
	printf("\t\t// Build a tangent vector for constructing directions\n");
	printf("\t\tVector3 t;\n");
	printf("\t\tif( fabs(n.x) < 0.9 )\n");
	printf("\t\t\tt = Vector3Ops::Normalize( Vector3Ops::Cross( n, Vector3(1,0,0) ) );\n");
	printf("\t\telse\n");
	printf("\t\t\tt = Vector3Ops::Normalize( Vector3Ops::Cross( n, Vector3(0,1,0) ) );\n\n");
	printf("\t\tT sum = 0.0;\n");
	printf("\t\tfor( int i = 0; i < GL_N; i++ )\n");
	printf("\t\t{\n");
	printf("\t\t\tconst Scalar mu = GL_nodes[i];\n");
	printf("\t\t\tconst Scalar sinTheta = sqrt( r_max(0.0, 1.0 - mu*mu) );\n");
	printf("\t\t\t// Direction at angle acos(mu) from normal\n");
	printf("\t\t\tconst Vector3 v = n * (-mu) + t * sinTheta;\n");
	printf("\t\t\tconst T F = Optics::CalculateConductorReflectance<T>( v, n, Ni, Nt, kt );\n");
	printf("\t\t\tsum = sum + F * (2.0 * mu * GL_weights[i]);\n");
	printf("\t\t}\n");
	printf("\t\treturn sum;\n");
	printf("\t}\n\n");

	// ComputeFms
	printf("\t/// Compute the multiscatter Fresnel compensation factor:\n");
	printf("\t///   F_ms = F_avg^2 * E_avg / (1 - F_avg * (1 - E_avg))\n");
	printf("\ttemplate< class T >\n");
	printf("\tinline T ComputeFms( const T& F_avg, const Scalar Eavg )\n");
	printf("\t{\n");
	printf("\t\tconst T denom = 1.0 - F_avg * (1.0 - Eavg);\n");
	printf("\t\tif( ColorMath::MaxValue(denom) < 1e-10 ) return T(0.0);\n");
	printf("\t\treturn F_avg * F_avg * Eavg / denom;\n");
	printf("\t}\n\n");

	// Scalar specialization of MaxValue for the denom check
	printf("\t/// Scalar overload for ComputeFms\n");
	printf("\ttemplate<>\n");
	printf("\tinline Scalar ComputeFms<Scalar>( const Scalar& F_avg, const Scalar Eavg )\n");
	printf("\t{\n");
	printf("\t\tconst Scalar denom = 1.0 - F_avg * (1.0 - Eavg);\n");
	printf("\t\tif( denom < 1e-10 ) return 0.0;\n");
	printf("\t\treturn F_avg * F_avg * Eavg / denom;\n");
	printf("\t}\n");

	printf("}\n}\n\n");
	printf("#endif\n");

	return 0;
}
