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
//
// P3-c (debt-ggx3 review correction): the paragraph immediately below
// used to say "ANISO_ALPHA_SIZE/ANISO_COS_SIZE are coarser than LUT_SIZE
// (16 vs 32)" and "16x16=256 pairs ... grew 2x" -- accurate ONLY at the
// moment the reachability fix (below) first landed, when both sizes were
// still 16.  The very next pass in this same slice (the P2-2 residual
// pass, see the ANISO_PHI_SIZE paragraph further down) raised
// ANISO_ALPHA_SIZE to 24 and ANISO_COS_SIZE to 32, so the CURRENT sizes
// are: ANISO_ALPHA_SIZE=24 (still coarser than LUT_SIZE=32, per axis) and
// ANISO_COS_SIZE=32 (now MATCHES LUT_SIZE exactly, no longer coarser).
// 24x24=576 (alphaX,alphaY) pairs are baked (vs the old (ratio,alphaEff)
// parametrization's 8x16=128 pairs), kept smaller than a hypothetical
// full 32x32 to keep the total sample budget in a reasonable
// generation-time ballpark -- see the wall-clock time recorded in the
// DL-77 ledger row/DL62_DL64 doc for the actual bake cost at this size.
//
// P2-2 (debt-ggx3, reachability fix): this table used to be parametrized
// on (ratio=max(alphaX,alphaY)/min(alphaX,alphaY), alphaEff=sqrt(alphaX*
// alphaY)), with alphaX,alphaY RECOVERED as alphaEff/sqrt(ratio) and
// alphaEff*sqrt(ratio).  That coupling makes most (ratio,alphaEff) grid
// cells physically UNREACHABLE: both alphaX and alphaY must stay in
// [0.01,1.0], so at ratio=100 the only valid alphaEff is the single point
// 0.1 (alphaX=0.01, alphaY=1.0) -- every other alphaEff node at that
// ratio row was dead weight, and the reachable WINDOW shrinks continuously
// as ratio grows, wasting an increasing fraction of ANISO_ALPHA_SIZE's
// nominal resolution exactly where the compensation curve is steepest.
// The table now resolves alphaX and alphaY as two INDEPENDENT grid axes,
// each spanning the full isotropic LUT range [0.01,1.0] linearly (the
// SAME per-axis mapping AnisoAlphaIndex already used for alphaEff) --
// every (ix,iy) cell is a literal, always-reachable (alphaX,alphaY) pair.
// ANISO_ALPHA_SIZE is reused as the per-axis size for BOTH alphaX and
// alphaY -- at the CURRENT ANISO_ALPHA_SIZE=24 that is 24x24=576
// (alphaX,alphaY) pairs, vs the old (ratio,alphaEff) parametrization's
// 8x16=128 pairs -- more than 4x the WORKING resolution even though the
// nominal grid grew only 3x (24/8), since none of it is wasted now.
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
// 90, so one quarter-period with reflective boundaries suffices -- this
// symmetry is a property of the elliptical GGX distribution alone and
// holds for ANY alphaX,alphaY pair, not just a designated "smaller" axis)
// -- endpoint-inclusive specifically so phi=0 and phi=90 (the two azimuths
// TestSchlickSweep's anisotropic rows actually probe, i.e. wi aligned with
// one tangent axis or the other) are EXACT table entries, not
// interpolated.  Critically, phi=0 now means "wi aligned with the
// alphaX axis" LITERALLY (the table's ix index IS alphaX, not "the
// smaller of the pair" as under the old ratio parametrization) -- so
// LookupEssG2AnisoDirectional needs no axis-swap heuristic to stay
// consistent with the query's actual (localX,localY) tangent frame (see
// DL-77's P1 follow-up, debt-ggx3: the old ratio-based table silently
// assumed alphaX was always the smaller axis, which a caller passing
// alphaX>alphaY -- e.g. every glTF pbrmetallicroughness_material --
// violated, mirroring the azimuth).  E_ss_TABLE_G2_ANISO_PHI[alphaX]
// [alphaY][phi][cos] feeds the ENERGY-COMPENSATION lookup
// (LookupEssG2AnisoDirectional, used at GGXBRDF/GGXSPF's Ess_i/Ess_o call
// sites); E_ss_TABLE_G2_ANISO (no phi dimension) is DERIVED from it by
// trapezoidal-averaging over phi and continues to feed ONLY the H6
// multiscatter-lobe OUTGOING-DIRECTION SAMPLER (MSLobeZG2Aniso/
// SampleMSCosThetaG2Aniso/MSPdfG2Aniso), where an azimuth-averaged
// proposal shape costs importance-sampling efficiency, not correctness
// (MSPdfG2Aniso always reports the density of what SampleMSCosThetaG2Aniso
// actually samples, so the estimator stays unbiased regardless of how
// good the proposal shape is).
//
// P2-1 (debt-ggx3, continuity fix): the alphaX==alphaY diagonal (ix==iy)
// is no longer an independent Monte-Carlo bake at this table's own
// coarser NUM_SAMPLES_ANISO -- see main()'s DL-77 bake loop, which seeds
// it directly from the WELL-CONVERGED isotropic E_ss_G2/E_avg_G2 tables
// (NUM_SAMPLES samples/cell) via the exact bilinear scheme LookupEssG2/
// LookupEavgG2 use at runtime, so a query approaching the diagonal
// interpolates toward the SAME numbers the runtime alphaX==alphaY
// shortcut returns instead of an independently-noisy neighboring node.
// P2-2 residual pass (debt-ggx3): an independent 4000-point sweep (own
// mt19937_64 RNG stream, 200k VNDF samples/point) against a fresh
// quadrature of this same file's GGX_G2_Aniso_HeightCorrelated found the
// worst-case interpolation residual concentrated at LOW alphaY combined
// with EXTREME grazing (cosTheta near/inside the ANISO_COS_SIZE grid's
// first, widest bin) -- the same end-cap-flattening effect as the
// isotropic table's own tracked DL-86 residual, compounded by the aniso
// grid's coarser 16-step alpha/cos resolution (worst case at the
// original 8x16x7x16 (ratio,alphaEff,phi,cos) parametrization: 5.09% at
// alphaX=0.0114,alphaY=0.1464,cos=0.1377,phi=69.5, per the DL-77 P2-2
// ledger note).  At the reachability fix's initial 16x16x7x16 direct
// (alphaX,alphaY) grid: worst case 4.52% at alphaX=0.7684,alphaY=0.0626,
// cos=0.0254,phi=80.4 (mean residual 0.20% over the 4000 points, 84 >1%,
// 18 >2%, 0 >5%).  ANISO_ALPHA_SIZE 16->24 and ANISO_COS_SIZE 16->32
// (now matching the isotropic LUT_SIZE) directly narrow that end-cap and
// the low-alpha cell width: worst case dropped to 3.25% at
// alphaX=0.9353,alphaY=0.0752,cos=0.1211,phi=85.5 (mean 0.15%, 35 >1%,
// 7 >2%, 0 >5%).
//
// P2 root-cause correction (debt-ggx3, review round 2): the "3.25% worst
// case is the same grazing end-cap DL-86 tracks" attribution above was
// WRONG for the cited point.  A profile at that exact configuration
// (alphaX=0.9353, alphaY=0.0752, cos=0.1211 -- nowhere near the
// c0=0.5/32=0.0156 end-cap clamp) found table-vs-truth is 0.1-0.7% at
// every phi GRID NODE (the old grid's 60/75/90 degree nodes) but peaks
// 3.2-3.3% MID-INTERVAL (85-87.5 degrees, i.e. between the 75 and 90
// degree nodes): snapping phi alone to the nearest node dropped the
// residual 3.43%->0.79%, while snapping cos alone (keeping phi at the
// off-grid 85.5) left 3.24% unchanged -- so the dominant driver at this
// point is the 15-degree-coarse ANISO_PHI_SIZE=7 azimuth grid against
// strong curvature near phi=90 at high anisotropy, not the cosTheta
// end-cap.  A SEPARATE, secondary driver is the LINEAR alpha axis: node0
// (alpha=0.01) and node1 (alpha=0.053) are a 5.3x ratio apart in a single
// cell, so bilinear interpolation near the low-alpha diagonal mixes
// strongly anisotropic corner cells (also the source of the pre-existing
// 1.27e-3 E_ss seam and 3.6% MSLobeZ seam noted elsewhere).  The
// cosTheta end-cap (cos<0.0156) IS real and IS DL-86 -- it dominates
// OTHER sweep points (e.g. 2.79% at cos=0.0062) -- it just wasn't the
// driver at the specific point this comment used to cite.
//
// Fix: ANISO_PHI_SIZE 7->13 (7.5-degree steps instead of 15-degree,
// endpoint-inclusive grid and exact node mirror `phiDeg[N-1-pi] =
// 90-phiDeg[pi]` both preserved for any N).  The alpha axis was measured
// and left LINEAR (see AnisoAlphaIndex): a log-spaced axis would need
// re-deriving the P2-1 isotropic-diagonal seeding, the pass-2 mirror
// symmetry, and every runtime call site's index math, for a driver this
// pass measured as SECONDARY to the phi grid -- out of scope here, and
// recorded as a residual, not silently dropped.
//
// Re-measured post-fix (own mt19937_64 stream, 4000 points, 200k VNDF
// samples/point, seed 424242; independent scratch program, not checked
// in): the SAME cited point (alphaX=0.9353, alphaY=0.0752, cos=0.1211,
// phi=85.5), re-evaluated at 4e6 samples for a clean number, now reads
// 2.58% (was 3.25%).  Full 4000-point sweep: UNRESTRICTED worst case
// 17.89% at alphaX=0.0361,alphaY=0.9627,cos=0.0024,phi=5.0 -- this IS a
// DL-86 end-cap point (cos=0.0024 << the c0=0.0156 clamp), not a DL-77
// regression.  RESTRICTED to cos>=0.03 (isolates the phi-interpolation/
// low-alpha-grid residual this pass targets, per the root-cause
// correction above): worst case 2.69% at alphaX=0.0411,alphaY=0.6853,
// cos=0.0742,phi=2.9 (mean residual 0.118% over 3908 points, 6 >1%,
// 2 >2%, 0 >5%) -- down from the pre-fix 3.25%/0.15%/35/7/0.  NOT fully
// closed to the <=1% target -- the residual's root cause is now measured
// as (a) phi-interpolation curvature near the axis-aligned/45-degree
// boundary at extreme anisotropy ratios, which a 13-point grid narrows
// but does not eliminate, and (b) the low-alpha linear-grid coarseness
// noted above -- revisiting either further (a denser or non-uniform phi
// grid, or a log-spaced alpha axis) is out of scope for this pass.  The
// SEPARATE cosTheta end-cap (cos<0.0156) remains tracked as DL-86 and is
// unaffected by this fix, isotropic or anisotropic.  See
// docs/DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md's "DL-77" section, P2-2
// subsection, for the full sweep methodology and this correction.
static const int ANISO_ALPHA_SIZE = 24;
static const int ANISO_COS_SIZE = 32;
static const int ANISO_PHI_SIZE = 13;	// 0,7.5,15,...,90 degrees (7.5-degree steps)
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
	// height-correlated-G2 directional albedo, resolved DIRECTLY by
	// (alphaX,alphaY) as two independent grid axes (see the generator's
	// ANISO_ALPHA_SIZE comment, debt-ggx3 P2-2, for why this replaced an
	// earlier (ratio,alphaEff) parametrization: that coupling left most
	// nominal grid cells physically unreachable), AZIMUTHALLY AVERAGED
	// over the incident direction's azimuth relative to the tangent/
	// bitangent axes.
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
	// P1 follow-up (debt-ggx3): under the table's PRIOR (ratio,alphaEff)
	// parametrization, phi=0 was baked to always mean "wi aligned with
	// the SMALLER-alpha axis" (alphaX_table=alphaEff/sqrt(ratio) was
	// always <= alphaY_table=alphaEff*sqrt(ratio) by construction), but
	// LookupEssG2AnisoDirectional read phi directly off the caller's
	// (localX,localY) with NO swap when the caller's actual alphaX was
	// the LARGER of the pair -- mirroring the azimuth for every caller
	// whose alphaX>alphaY (e.g. every glTF pbrmetallicroughness_material,
	// Job.cpp's alphaX>=alphaY convention).  The direct (alphaX,alphaY)
	// grid below eliminates the ambiguity structurally: ix IS alphaX and
	// iy IS alphaY, so phi=0 means "aligned with the queried alphaX axis"
	// unconditionally and AnisoPhiIndex needs no swap.
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

	/// Map an alpha value (already clamped to [0.01,1.0]) to the DL-77
	/// aniso table's per-axis index pair (i0,i1,f) -- same linear mapping
	/// style as LookupEss's own alpha blend.  Used for BOTH the alphaX and
	/// alphaY axes (they share the same [0.01,1.0] range and grid size).
	inline void AnisoAlphaIndex( const Scalar alpha, int& i0, int& i1, Scalar& f )
	{
		Scalar a = r_max(0.0, r_min(1.0, (alpha - 0.01) / (1.0 - 0.01))) * (ANISO_ALPHA_SIZE - 1);
		i0 = (int)a;
		i1 = r_min(i0 + 1, ANISO_ALPHA_SIZE - 1);
		f = a - i0;
	}

	/// Map a local-space direction's (x,y) to the DL-77 aniso table's
	/// endpoint-inclusive phi index pair (pi0,pi1,pf), folded into the
	/// ellipse's quarter-period [0,90] degrees (see ANISO_PHI_SIZE's
	/// comment in the generator for the symmetry argument -- this holds
	/// for any alphaX,alphaY pair, so the caller does NOT need to swap
	/// localX/localY based on which of alphaX,alphaY is larger; phi=0 is
	/// simply "aligned with the queried alphaX axis").  Degenerate (x,y)
	/// near zero (a direction nearly along the normal) folds to an
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

		int xi0, xi1; Scalar xf;
		AnisoAlphaIndex( aX, xi0, xi1, xf );
		int yi0, yi1; Scalar yf;
		AnisoAlphaIndex( aY, yi0, yi1, yf );

		Scalar c = r_max(0.0, r_min(1.0, cosTheta)) * ANISO_COS_SIZE - 0.5;
		if( c < 0 ) c = 0;
		int ci0 = (int)c;
		int ci1 = r_min(ci0 + 1, ANISO_COS_SIZE - 1);
		Scalar cf = c - ci0;

		const Scalar v000 = E_ss_TABLE_G2_ANISO[xi0][yi0][ci0];
		const Scalar v001 = E_ss_TABLE_G2_ANISO[xi0][yi0][ci1];
		const Scalar v010 = E_ss_TABLE_G2_ANISO[xi0][yi1][ci0];
		const Scalar v011 = E_ss_TABLE_G2_ANISO[xi0][yi1][ci1];
		const Scalar v100 = E_ss_TABLE_G2_ANISO[xi1][yi0][ci0];
		const Scalar v101 = E_ss_TABLE_G2_ANISO[xi1][yi0][ci1];
		const Scalar v110 = E_ss_TABLE_G2_ANISO[xi1][yi1][ci0];
		const Scalar v111 = E_ss_TABLE_G2_ANISO[xi1][yi1][ci1];

		const Scalar v00 = (1-cf)*v000 + cf*v001;
		const Scalar v01 = (1-cf)*v010 + cf*v011;
		const Scalar v10 = (1-cf)*v100 + cf*v101;
		const Scalar v11 = (1-cf)*v110 + cf*v111;
		const Scalar v0 = (1-yf)*v00 + yf*v01;
		const Scalar v1 = (1-yf)*v10 + yf*v11;
		return (1-xf)*v0 + xf*v1;
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

		int xi0, xi1; Scalar xf;
		AnisoAlphaIndex( aX, xi0, xi1, xf );
		int yi0, yi1; Scalar yf;
		AnisoAlphaIndex( aY, yi0, yi1, yf );
		int pi0, pi1; Scalar pf;
		AnisoPhiIndex( localX, localY, pi0, pi1, pf );

		Scalar c = r_max(0.0, r_min(1.0, cosTheta)) * ANISO_COS_SIZE - 0.5;
		if( c < 0 ) c = 0;
		int ci0 = (int)c;
		int ci1 = r_min(ci0 + 1, ANISO_COS_SIZE - 1);
		Scalar cf = c - ci0;

		// Quadrilinear interpolation over (alphaX, alphaY, phi, cosTheta):
		// 16 corners, collapsed one axis at a time (cos, then phi, then
		// alphaY, then alphaX) -- same nested-lerp pattern as the
		// trilinear form above, one dimension deeper.
		Scalar vXY[2][2];	// [alphaX][alphaY], after collapsing phi and cos
		for( int xi = 0; xi < 2; xi++ )
		{
			const int xiv = (xi == 0) ? xi0 : xi1;
			for( int yi = 0; yi < 2; yi++ )
			{
				const int yiv = (yi == 0) ? yi0 : yi1;
				const Scalar vP0c0 = E_ss_TABLE_G2_ANISO_PHI[xiv][yiv][pi0][ci0];
				const Scalar vP0c1 = E_ss_TABLE_G2_ANISO_PHI[xiv][yiv][pi0][ci1];
				const Scalar vP1c0 = E_ss_TABLE_G2_ANISO_PHI[xiv][yiv][pi1][ci0];
				const Scalar vP1c1 = E_ss_TABLE_G2_ANISO_PHI[xiv][yiv][pi1][ci1];
				const Scalar vP0 = (1-cf)*vP0c0 + cf*vP0c1;
				const Scalar vP1 = (1-cf)*vP1c0 + cf*vP1c1;
				vXY[xi][yi] = (1-pf)*vP0 + pf*vP1;
			}
		}
		const Scalar v0 = (1-yf)*vXY[0][0] + yf*vXY[0][1];
		const Scalar v1 = (1-yf)*vXY[1][0] + yf*vXY[1][1];
		return (1-xf)*v0 + xf*v1;
	}

	/// DL-77: anisotropic twin of LookupEavgG2.
	inline Scalar LookupEavgG2Aniso( const Scalar alphaX, const Scalar alphaY )
	{
		if( fabs(alphaX - alphaY) < 1e-9 ) return LookupEavgG2( alphaX );

		const Scalar aX = r_max(0.01, r_min(1.0, alphaX));
		const Scalar aY = r_max(0.01, r_min(1.0, alphaY));

		int xi0, xi1; Scalar xf;
		AnisoAlphaIndex( aX, xi0, xi1, xf );
		int yi0, yi1; Scalar yf;
		AnisoAlphaIndex( aY, yi0, yi1, yf );

		const Scalar v00 = E_avg_TABLE_G2_ANISO[xi0][yi0];
		const Scalar v01 = E_avg_TABLE_G2_ANISO[xi0][yi1];
		const Scalar v10 = E_avg_TABLE_G2_ANISO[xi1][yi0];
		const Scalar v11 = E_avg_TABLE_G2_ANISO[xi1][yi1];
		const Scalar v0 = (1-yf)*v00 + yf*v01;
		const Scalar v1 = (1-yf)*v10 + yf*v11;
		return (1-xf)*v0 + xf*v1;
	}

	/// DL-77: anisotropic twin of MSLobeZG2.  Z is a LINEAR functional of
	/// the essRow (see MSLobeZ's own perf-note comment above), so the
	/// per-corner Z values can be precomputed once and then blended with
	/// the SAME bilinear (alphaX, alphaY) weights LookupEssG2Aniso/
	/// LookupEavgG2Aniso use -- exactly how MSLobeZG2 blends across alpha
	/// alone, just with a second (alphaY) dimension.
	inline Scalar MSLobeZG2Aniso( const Scalar alphaX, const Scalar alphaY )
	{
		if( fabs(alphaX - alphaY) < 1e-9 ) return MSLobeZG2( alphaX );

		static const std::array<std::array<Scalar, ANISO_ALPHA_SIZE>, ANISO_ALPHA_SIZE> rowZ = []() {
			std::array<std::array<Scalar, ANISO_ALPHA_SIZE>, ANISO_ALPHA_SIZE> z{};
			for( int xi = 0; xi < ANISO_ALPHA_SIZE; xi++ )
			{
				for( int yi = 0; yi < ANISO_ALPHA_SIZE; yi++ )
				{
					MSLobeDetail::Segment segs[ANISO_COS_SIZE + 1];
					int nSegs = 0;
					MSLobeDetail::BuildSegmentsFromRowN( E_ss_TABLE_G2_ANISO[xi][yi], ANISO_COS_SIZE, segs, nSegs );
					Scalar I = 0.0;
					for( int i = 0; i < nSegs; i++ )
						I += MSLobeDetail::SegTotal( segs[i] );
					z[xi][yi] = 2.0 * I;
				}
			}
			return z;
		}();

		const Scalar aX = r_max(0.01, r_min(1.0, alphaX));
		const Scalar aY = r_max(0.01, r_min(1.0, alphaY));

		int xi0, xi1; Scalar xf;
		AnisoAlphaIndex( aX, xi0, xi1, xf );
		int yi0, yi1; Scalar yf;
		AnisoAlphaIndex( aY, yi0, yi1, yf );

		const Scalar v0 = (1-yf)*rowZ[xi0][yi0] + yf*rowZ[xi0][yi1];
		const Scalar v1 = (1-yf)*rowZ[xi1][yi0] + yf*rowZ[xi1][yi1];
		return (1-xf)*v0 + xf*v1;
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

		int xi0, xi1; Scalar xf;
		AnisoAlphaIndex( aX, xi0, xi1, xf );
		int yi0, yi1; Scalar yf;
		AnisoAlphaIndex( aY, yi0, yi1, yf );

		Scalar essRow[ANISO_COS_SIZE];
		for( int k = 0; k < ANISO_COS_SIZE; k++ )
		{
			const Scalar v0 = (1-yf)*E_ss_TABLE_G2_ANISO[xi0][yi0][k] + yf*E_ss_TABLE_G2_ANISO[xi0][yi1][k];
			const Scalar v1 = (1-yf)*E_ss_TABLE_G2_ANISO[xi1][yi0][k] + yf*E_ss_TABLE_G2_ANISO[xi1][yi1][k];
			essRow[k] = (1-xf)*v0 + xf*v1;
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

	// DL-77: anisotropic height-correlated-G2 E_ss/E_avg, resolved DIRECTLY
	// by (alphaX, alphaY, phi, cosTheta) -- P2-2 (debt-ggx3) replaced the
	// old (ratio, alphaEff) coupling, which left most nominal grid cells
	// physically unreachable (see the ANISO_ALPHA_SIZE comment above), with
	// two independent alphaX/alphaY axes that are each always fully
	// reachable across [0.01,1.0].  E_ss_G2_ANISO_PHI is the primary
	// (phi-resolved) bake; E_ss_G2_ANISO (no phi) is DERIVED from it by
	// trapezoidal-averaging over phi, then E_avg_G2_ANISO is derived from
	// THAT exactly as the isotropic tables derive E_avg from E_ss.
	static double E_ss_G2_ANISO_PHI[ANISO_ALPHA_SIZE][ANISO_ALPHA_SIZE][ANISO_PHI_SIZE][ANISO_COS_SIZE];
	static double E_ss_G2_ANISO[ANISO_ALPHA_SIZE][ANISO_ALPHA_SIZE][ANISO_COS_SIZE];
	static double E_avg_G2_ANISO[ANISO_ALPHA_SIZE][ANISO_ALPHA_SIZE];

	// P2-1 (debt-ggx3): bilinear evaluation of the WELL-CONVERGED isotropic
	// E_ss_G2 table above, using the EXACT SAME interpolation scheme
	// LookupEssG2 uses at runtime (LUT_SIZE resolution, NUM_SAMPLES
	// samples/cell) -- used ONLY to seed the aniso table's alphaX==alphaY
	// diagonal below, so that boundary matches the isotropic curve exactly
	// instead of an independently-noisy NUM_SAMPLES_ANISO estimate, and a
	// query approaching the diagonal interpolates toward a consistent
	// limit instead of jumping.
	auto evalIsoEssG2 = [&](double alpha, double cosTheta) -> double {
		double a = fmax(0.0, fmin(1.0, (alpha - 0.01) / (1.0 - 0.01))) * (LUT_SIZE - 1);
		int ai0 = (int)a;
		int ai1 = (ai0 + 1 < LUT_SIZE - 1) ? ai0 + 1 : LUT_SIZE - 1;
		double af = a - ai0;
		double c = fmax(0.0, fmin(1.0, cosTheta)) * LUT_SIZE - 0.5;
		if(c < 0) c = 0;
		int ci0 = (int)c;
		int ci1 = (ci0 + 1 < LUT_SIZE - 1) ? ci0 + 1 : LUT_SIZE - 1;
		double cf = c - ci0;
		double v00 = E_ss_G2[ai0][ci0];
		double v01 = E_ss_G2[ai0][ci1];
		double v10 = E_ss_G2[ai1][ci0];
		double v11 = E_ss_G2[ai1][ci1];
		return (1-af) * ((1-cf)*v00 + cf*v01) + af * ((1-cf)*v10 + cf*v11);
	};

	// Pass 1: Monte-Carlo bake (or P2-1 iso-seed on the diagonal) ONLY the
	// canonical upper triangle ix<=iy.  Relabeling which axis is "X" and
	// which is "Y" is a pure coordinate swap of the SAME physical wi
	// (m.x^2/alphaX^2+m.y^2/alphaY^2 is invariant under x<->y with
	// alphaX<->alphaY), so Ess(alphaX=a,alphaY=b,phi) is EXACTLY
	// Ess(alphaX=b,alphaY=a,90-phi) for the identical physical direction
	// -- not an approximate symmetry to be independently re-sampled (that
	// would only agree up to Monte-Carlo noise between the two bakes, as
	// a naive full-square bake measured at ~1e-3..1e-4 relative
	// disagreement).  Pass 2 below fills the ix>iy half by MIRRORING pass
	// 1's data (same samples, zero extra noise), so the (alphaX,alphaY,
	// phi)<->(alphaY,alphaX,90-phi) relabel-symmetry is exact to the
	// bit, not just "close".  This also roughly halves the Monte-Carlo
	// work versus baking the full square independently.
	for(int ix = 0; ix < ANISO_ALPHA_SIZE; ix++) {
		const double alphaX = 0.01 + (1.0 - 0.01) * (double)ix / (double)(ANISO_ALPHA_SIZE - 1);

		for(int iy = ix; iy < ANISO_ALPHA_SIZE; iy++) {
			const double alphaY = 0.01 + (1.0 - 0.01) * (double)iy / (double)(ANISO_ALPHA_SIZE - 1);
			const bool isDiagonal = (ix == iy);

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

					if(isDiagonal) {
						// P2-1: alphaX==alphaY is isotropic -- no azimuthal
						// dependence, so every phi slot at this cell gets
						// the SAME converged isotropic value instead of an
						// independent (noisier) Monte-Carlo draw.
						E_ss_G2_ANISO_PHI[ix][iy][pi][ci] = evalIsoEssG2(alphaX, cosTheta);
						continue;
					}

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

					E_ss_G2_ANISO_PHI[ix][iy][pi][ci] = sumG2 / (double)NUM_SAMPLES_ANISO;
				}
			}

			fprintf(stderr, "aniso alphaX=%.4f alphaY=%.4f  (canonical bake)%s\n",
				alphaX, alphaY, isDiagonal ? "  [diag: iso-seeded]" : "");
		}
	}

	// Pass 2: mirror the ix>iy half from pass 1's ix<=iy data -- see the
	// exact-symmetry rationale above.  phi index pi mirrors to
	// ANISO_PHI_SIZE-1-pi because the endpoint-inclusive phi grid is
	// itself symmetric about 45 degrees (phiDeg[k] = 90 - phiDeg[N-1-k]).
	for(int ix = 1; ix < ANISO_ALPHA_SIZE; ix++) {
		for(int iy = 0; iy < ix; iy++) {
			for(int pi = 0; pi < ANISO_PHI_SIZE; pi++) {
				for(int ci = 0; ci < ANISO_COS_SIZE; ci++) {
					E_ss_G2_ANISO_PHI[ix][iy][pi][ci] = E_ss_G2_ANISO_PHI[iy][ix][ANISO_PHI_SIZE - 1 - pi][ci];
				}
			}
		}
	}

	// Pass 3: derive E_ss_G2_ANISO (phi-trapezoidal average) and
	// E_avg_G2_ANISO (cosTheta integral) for the FULL square -- the
	// endpoint-symmetric trapezoidal weights [0.5,1,1,...,1,0.5] read the
	// same forwards or reversed, so this comes out exactly symmetric
	// (E_ss_G2_ANISO[ix][iy]==E_ss_G2_ANISO[iy][ix]) as a consequence of
	// pass 2's mirror, with no special-casing needed here.
	for(int ix = 0; ix < ANISO_ALPHA_SIZE; ix++) {
		for(int iy = 0; iy < ANISO_ALPHA_SIZE; iy++) {
			// Derive the phi-averaged row via trapezoidal quadrature over
			// the endpoint-inclusive phi grid (half-weight at phi=0/90,
			// full weight interior, normalized by the number of
			// intervals) -- reproduces the same "uniform azimuth average"
			// quantity the pre-fix generator computed via random phi
			// draws, exploiting the ellipse's quarter-period symmetry.
			for(int ci = 0; ci < ANISO_COS_SIZE; ci++) {
				double trapz = 0.5 * E_ss_G2_ANISO_PHI[ix][iy][0][ci] + 0.5 * E_ss_G2_ANISO_PHI[ix][iy][ANISO_PHI_SIZE-1][ci];
				for(int pi = 1; pi < ANISO_PHI_SIZE - 1; pi++)
					trapz += E_ss_G2_ANISO_PHI[ix][iy][pi][ci];
				E_ss_G2_ANISO[ix][iy][ci] = trapz / (double)(ANISO_PHI_SIZE - 1);
			}

			double integralAniso = 0.0;
			for(int ci = 0; ci < ANISO_COS_SIZE; ci++) {
				const double mu = ((double)ci + 0.5) / ANISO_COS_SIZE;
				const double dmu = 1.0 / ANISO_COS_SIZE;
				integralAniso += E_ss_G2_ANISO[ix][iy][ci] * mu * dmu;
			}
			E_avg_G2_ANISO[ix][iy] = 2.0 * integralAniso;

			const double alphaX = 0.01 + (1.0 - 0.01) * (double)ix / (double)(ANISO_ALPHA_SIZE - 1);
			const double alphaY = 0.01 + (1.0 - 0.01) * (double)iy / (double)(ANISO_ALPHA_SIZE - 1);
			fprintf(stderr, "aniso alphaX=%.4f alphaY=%.4f  E_avg_G2_ANISO=%.6f\n",
				alphaX, alphaY, E_avg_G2_ANISO[ix][iy]);
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
	printf("//  DL-77 aniso LUT resolution: %d alphaX x %d alphaY x %d phi x %d cosTheta\n", ANISO_ALPHA_SIZE, ANISO_ALPHA_SIZE, ANISO_PHI_SIZE, ANISO_COS_SIZE);
	printf("//  DL-77 aniso samples per entry: %d (off-diagonal cells only --\n", NUM_SAMPLES_ANISO);
	printf("//  alphaX==alphaY cells are seeded from the isotropic tables, see\n");
	printf("//  the P2-1 comment above the DL-77 bake loop)\n");
	printf("//  DL-77 aniso alphaX,alphaY range: [0.01, 1.0] (uniform %d steps each)\n", ANISO_ALPHA_SIZE);
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
	printf("//  the DL-77 anisotropic twins (AnisoAlphaIndex/AnisoPhiIndex,\n");
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
	//
	// P3-e (debt-ggx3 review): all 7 large tables in this header are
	// `inline const`, not `inline constexpr` -- both give identical
	// C++17 external-linkage/one-definition-rule dedupe (an `inline`
	// variable has one shared definition across every including TU
	// regardless of the `const`/`constexpr` qualifier), but `constexpr`
	// additionally obligates the compiler to constant-evaluate the
	// initializer, and MSVC's default `/constexpr:steps 100000` step
	// budget cannot evaluate the largest table here (E_ss_TABLE_G2_ANISO_PHI,
	// 24*24*13*32 = 239,616 elements at the current ANISO_PHI_SIZE=13)
	// at compile time -- `inline const` keeps the same runtime data and
	// linkage without asking for compile-time evaluation at all.  Each
	// table below carries a one-line printed reminder of this.
	printf("\t/// Directional albedo E_ss(alpha, cosTheta) of GGX single-scatter BRDF with F=1.\n");
	printf("\t/// Indexed as E_ss_TABLE[alphaIdx][cosThetaIdx].\n");
	printf("\t/// Alpha mapped linearly from 0.01 to 1.0, cosTheta from cell centers.\n");
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_ss_TABLE[%d][%d] = {\n", LUT_SIZE, LUT_SIZE);
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
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_avg_TABLE[%d] = {\n\t\t", LUT_SIZE);
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
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_ss_TABLE_G2[%d][%d] = {\n", LUT_SIZE, LUT_SIZE);
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
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_avg_TABLE_G2[%d] = {\n\t\t", LUT_SIZE);
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
	printf("\tstatic const int ANISO_COS_SIZE = %d;\n", ANISO_COS_SIZE);
	printf("\tstatic const int ANISO_PHI_SIZE = %d;\n\n", ANISO_PHI_SIZE);

	printf("\t/// DL-77: per-azimuth (NOT azimuthally-averaged) height-\n");
	printf("\t/// correlated-G2 single-scatter directional albedo -- the\n");
	printf("\t/// primary bake; E_ss_TABLE_G2_ANISO below is DERIVED from this\n");
	printf("\t/// by trapezoidal-averaging over phi.  Indexed as\n");
	printf("\t/// E_ss_TABLE_G2_ANISO_PHI[alphaXIdx][alphaYIdx][phiIdx][cosThetaIdx],\n");
	printf("\t/// alphaX,alphaY each mapped linearly from 0.01 to 1.0 (P2-2,\n");
	printf("\t/// debt-ggx3: this used to be resolved by (ratio,alphaEff)\n");
	printf("\t/// instead, which left most nominal grid cells physically\n");
	printf("\t/// unreachable -- see the generator's ANISO_ALPHA_SIZE comment).\n");
	printf("\t/// phi is wi's azimuth relative to the alphaX axis, grid points\n");
	printf("\t/// at 0,15,...,90 degrees INCLUSIVE of both endpoints (ellipse\n");
	printf("\t/// quarter-period symmetry -- see the ANISO_PHI_SIZE comment in\n");
	printf("\t/// the generator).  Consumed by LookupEssG2AnisoDirectional,\n");
	printf("\t/// used ONLY at the energy-compensation Ess_i/Ess_o call sites\n");
	printf("\t/// in GGXBRDF.cpp/GGXSPF.cpp -- NOT by the H6 multiscatter-lobe\n");
	printf("\t/// sampler, which keeps using the phi-averaged\n");
	printf("\t/// E_ss_TABLE_G2_ANISO below (an importance-sampling proposal\n");
	printf("\t/// shape; exactness there is an efficiency concern, not a\n");
	printf("\t/// correctness one).  The alphaX==alphaY diagonal is seeded\n");
	printf("\t/// from the converged isotropic E_ss_TABLE_G2 (P2-1, debt-ggx3)\n");
	printf("\t/// rather than an independent Monte-Carlo bake, so it matches\n");
	printf("\t/// LookupEssG2's own curve exactly at the isotropic boundary.\n");
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_ss_TABLE_G2_ANISO_PHI[%d][%d][%d][%d] = {\n", ANISO_ALPHA_SIZE, ANISO_ALPHA_SIZE, ANISO_PHI_SIZE, ANISO_COS_SIZE);
	for(int ix = 0; ix < ANISO_ALPHA_SIZE; ix++) {
		printf("\t\t{\n");
		for(int iy = 0; iy < ANISO_ALPHA_SIZE; iy++) {
			printf("\t\t\t{\n");
			for(int pi = 0; pi < ANISO_PHI_SIZE; pi++) {
				printf("\t\t\t\t{ ");
				for(int ci = 0; ci < ANISO_COS_SIZE; ci++) {
					printf("%.8f", E_ss_G2_ANISO_PHI[ix][iy][pi][ci]);
					if(ci < ANISO_COS_SIZE - 1) printf(", ");
				}
				printf(" }");
				if(pi < ANISO_PHI_SIZE - 1) printf(",");
				printf("\n");
			}
			printf("\t\t\t}");
			if(iy < ANISO_ALPHA_SIZE - 1) printf(",");
			printf("\n");
		}
		printf("\t\t}");
		if(ix < ANISO_ALPHA_SIZE - 1) printf(",");
		printf("\n");
	}
	printf("\t};\n\n");

	printf("\t/// DL-77: azimuthally-averaged height-correlated-G2 single-\n");
	printf("\t/// scatter directional albedo.  Indexed as\n");
	printf("\t/// E_ss_TABLE_G2_ANISO[alphaXIdx][alphaYIdx][cosThetaIdx], both\n");
	printf("\t/// alpha axes mapped linearly from 0.01 to 1.0, cosTheta from\n");
	printf("\t/// cell centers.\n");
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_ss_TABLE_G2_ANISO[%d][%d][%d] = {\n", ANISO_ALPHA_SIZE, ANISO_ALPHA_SIZE, ANISO_COS_SIZE);
	for(int ix = 0; ix < ANISO_ALPHA_SIZE; ix++) {
		printf("\t\t{\n");
		for(int iy = 0; iy < ANISO_ALPHA_SIZE; iy++) {
			printf("\t\t\t{ ");
			for(int ci = 0; ci < ANISO_COS_SIZE; ci++) {
				printf("%.8f", E_ss_G2_ANISO[ix][iy][ci]);
				if(ci < ANISO_COS_SIZE - 1) printf(", ");
			}
			printf(" }");
			if(iy < ANISO_ALPHA_SIZE - 1) printf(",");
			printf("\n");
		}
		printf("\t\t}");
		if(ix < ANISO_ALPHA_SIZE - 1) printf(",");
		printf("\n");
	}
	printf("\t};\n\n");

	printf("\t/// DL-77: azimuthally-averaged hemisphere average of\n");
	printf("\t/// E_ss_TABLE_G2_ANISO per (alphaX, alphaY).\n");
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_avg_TABLE_G2_ANISO[%d][%d] = {\n", ANISO_ALPHA_SIZE, ANISO_ALPHA_SIZE);
	for(int ix = 0; ix < ANISO_ALPHA_SIZE; ix++) {
		printf("\t\t{ ");
		for(int iy = 0; iy < ANISO_ALPHA_SIZE; iy++) {
			printf("%.8f", E_avg_G2_ANISO[ix][iy]);
			if(iy < ANISO_ALPHA_SIZE - 1) printf(", ");
		}
		printf(" }");
		if(ix < ANISO_ALPHA_SIZE - 1) printf(",");
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
