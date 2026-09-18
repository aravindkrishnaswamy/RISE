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

// DL-86: a SECOND, independently-seeded LCG stream, used exclusively by
// the DL-86 grazing sub-grid bakes below.  Deliberately separate from
// `rng_state` so that ADDING the sub-grid tables leaves every
// pre-existing table (E_ss_TABLE, E_ss_TABLE_G2, E_avg*, and all four
// DL-77 aniso tables) byte-for-byte identical -- drawing the sub-grid
// samples from the shared stream would shift every subsequent draw and
// perturb the whole file within Monte-Carlo noise, hiding the real
// change in a whole-file diff.
static unsigned long long rng_state_sub = 9876543210987654321ULL;
static double rand01_sub() {
	rng_state_sub = rng_state_sub * 6364136223846793005ULL + 1442695040888963407ULL;
	return (double)(rng_state_sub >> 11) / (double)(1ULL << 53);
}

// DL-105: a THIRD, independently-seeded LCG stream, used exclusively by
// the DL-105 low-alpha sub-grid bake.  Same isolation rationale as
// rng_state_sub above -- every pre-existing table (including the DL-86
// sub-grid tables, which are baked from rng_state_sub before this
// stream is ever touched) stays byte-for-byte identical.
static unsigned long long rng_state_lowalpha = 13091977ULL * 1000000007ULL + 42ULL;
static double rand01_lowalpha() {
	rng_state_lowalpha = rng_state_lowalpha * 6364136223846793005ULL + 1442695040888963407ULL;
	return (double)(rng_state_lowalpha >> 11) / (double)(1ULL << 53);
}

// DL-161: a FOURTH, independently-seeded LCG stream, used exclusively by
// the DL-161 aniso low-alpha sub-grid bake (ALPHALOW_X/ALPHALOW_XY and
// their grazing SUB twins).  Same isolation rationale as rng_state_sub/
// rng_state_lowalpha above -- every pre-existing table (isotropic AND
// the DL-77/DL-86 aniso ones, all fully baked before this stream is
// ever touched) stays byte-for-byte identical.
static unsigned long long rng_state_anisolow = 271828182845904523ULL;
static double rand01_anisolow() {
	rng_state_anisolow = rng_state_anisolow * 6364136223846793005ULL + 1442695040888963407ULL;
	return (double)(rng_state_anisolow >> 11) / (double)(1ULL << 53);
}

static const int LUT_SIZE = 32;
static const int NUM_SAMPLES = 1000000;

// DL-86: resolution of the grazing SUB-GRID that resolves Ess on
// [0, c0], where c0 = 0.5/N is the first ordinary cosTheta bin center
// of an N-bin table (c0 = 0.015625 at both LUT_SIZE and ANISO_COS_SIZE
// = 32, i.e. every incidence angle beyond ~89.1 degrees).
//
// Before DL-86 every lookup FLAT-CLAMPED that whole interval to the
// bin-0 value.  That is badly wrong at low roughness because
// E_ss(cosTheta) is NOT monotone there: at alpha=0.01 an independent
// 20M-sample VNDF quadrature reads 0.8920 at cosTheta=0.010, rising to
// 0.9796 at 0.001 and to the exact boundary value at 0 -- while bin 0
// (cosTheta=c0) reads 0.8993.  A single straight line from the boundary
// to bin 0 cannot follow that dip either, so the interval is BAKED at
// SUB_SIZE uniform sub-intervals instead (nodes at k*c0/SUB_SIZE,
// k=1..SUB_SIZE-1; the k=SUB_SIZE node IS bin 0 itself, so the model is
// continuous with the ordinary bilinear interior at c0 by construction,
// and the k=0 boundary is the exact cosTheta->0 limit -- 1 for the
// height-correlated G2 model, a baked per-alpha constant for the
// separable model, see E_ss_LIMIT_TABLE).
//
// SUB_SIZE=8 was chosen by measuring the reconstruction residual of the
// alpha-row-blended piecewise-linear model against an independent 2M-
// sample-per-point quadrature on the grid alpha in {0.01,0.015,0.02,
// 0.05,0.3,1.0} x cosTheta in {1e-4,5e-4,1e-3,2e-3,5e-3,8e-3,1e-2,
// 1.56e-2}: SUB_SIZE=4 leaves 0.41% worst-case at alpha=0.01, SUB_SIZE=8
// leaves 0.11%, SUB_SIZE=16 leaves 0.02%.  8 is the knee -- already an
// order of magnitude inside the 1% target, while 16 would double the
// DL-77 aniso sub-table's size for no measurable gain.  (The residual
// that remains at alpha=0.015-0.02, ~1.5%, is NOT a sub-grid artifact:
// it is the alpha axis's own coarseness between row 0 (alpha=0.01) and
// row 1 (alpha=0.0419) and does not move with SUB_SIZE at all -- see
// DL-105 in docs/DEBT_LEDGER.md.)
static const int SUB_SIZE = 8;

// DL-86 round 2: number of GEOMETRIC refinements of the LOWEST uniform
// sub-interval [0, c0/SUB_SIZE], applied to the DL-77 ANISOTROPIC sub-
// grid only (nodes at h*2^-j for j=1..ANISO_SUB_FINE, h = c0/SUB_SIZE).
//
// The uniform sub-grid above is enough for the ISOTROPIC tables because
// their approach to Ess(0)=1 is already essentially linear by the first
// sub-node: at alpha=0.01 the true curve reads 0.958 at h=1.953e-3 and
// 0.9796 at 1e-3 (deficit 0.042 vs 0.0204 -- a ratio of 2.06 across a
// factor of 1.953), so the straight ramp from the anchor is accurate to
// 0.016% at cos=1e-4 (measured, GGXHeightCorrelatedEnergyLUTTest).
//
// For an ANISOTROPIC pair it is not, and the reason is structural: the
// approach to 1 needs Lambda(wi) >> Lambda(wo), i.e. cos << alphaX*
// sinTheta for a wi aligned with the SMALL axis, while the scattered
// lobe is spread over the LARGE axis and keeps Lambda(wo) large.  At
// (alphaX=0.01, alphaY=1.0, phi=0) the true curve is still at 0.735 at
// that same first node, so the ramp from 1 spans a 0.265 drop over one
// interval and reads +2.5% at cos=1e-4, +4.3% at 2.5e-4, +5.9% at 5e-4
// and +5.9% at 1e-3 (round-2 review; reproduced verbatim in this
// slice's red proof).  Refining the interval geometrically instead puts
// a node at every octave down to h*2^-5 = 6.1e-5, below which no
// production shading direction lands (theta > 89.996 degrees).
//
// ANISO_SUB_FINE=5 was chosen by measuring the reconstruction residual
// of the node-blended piecewise-linear model against an independent 1M-
// sample-per-point quadrature at the node-exact configurations, over
// cos in {1e-4, 2.5e-4, 5e-4, 1e-3, 2e-3, 5e-3, 1e-2, 1.56e-2}: at
// (0.01,1.0,phi=0), J=3 leaves 0.81% (its anchor ramp still reaches up
// to 2.44e-4, above the smallest probe), J=4 leaves 0.60% and J=5
// leaves 0.54%.  From J=4 on, the worst residual is no longer in the
// end-cap at all -- it sits at cos=5e-3, inside the UNIFORM part of the
// grid, and does not move with further refinement.  J=5 is taken
// because it is the first value whose anchor ramp lies entirely below
// the smallest probe, so no probed cos is served by the one segment
// that is not bracketed by two baked nodes.
//
// Interpolation stays LINEAR IN COS on every interval.  Log-cos
// interpolation measured better on the uniform part (0.13% vs 0.54% at
// cos=5e-3, the curve being close to a power law there), but it cannot
// express the interval that touches cos=0, and every consumer of these
// nodes -- MSLobeDetail::SegTotal/SegInvert, which integrate and invert
// (1-Ess)*c in closed form for the H6 sampler -- is built on the
// piecewise-LINEAR model.  0.54% is already inside the 1% target.
static const int ANISO_SUB_FINE = 5;

// Total number of NODE INDICES on [0, c0] for the aniso sub-grid: index
// 0 is the exact cosTheta->0 anchor, indices 1..ANISO_SUB_FINE are the
// geometric octaves, indices ANISO_SUB_FINE+1..ANISO_SUB_TOTAL are the
// uniform nodes k*h (k=1..SUB_SIZE), and the last of those IS the first
// ordinary bin center c0.  So ANISO_SUB_TOTAL-1 = 12 values are stored
// per row (neither the anchor nor bin 0 is).
static const int ANISO_SUB_TOTAL = SUB_SIZE + ANISO_SUB_FINE;

// DL-105: the ALPHA axis is coarse at its low end in a way the DL-86
// cosTheta sub-grid does not touch at all -- alpha<0.01 clamps to row 0
// outright (GGXBRDF.cpp only floors authored roughness at 1e-4, so
// alpha=0.005 really reaches this table), and row 0 (alpha=0.01) to
// row 1 (alpha=0.0419) is a 4.2x ratio inside ONE linear interpolation
// cell.  Both are the SAME construction DL-86 used for the cosTheta
// end-cap: a baked sub-grid, not an extrapolation, anchored at a
// PROVABLE exact boundary.  Here the boundary is alpha->0 (a perfectly
// smooth surface): Smith Lambda(v) = (-1+sqrt(1+alpha^2*tan^2(theta)))/2
// -> 0 as alpha->0 for ANY FIXED cosTheta>0 (ordinary bin or existing
// DL-86 grazing sub-node), so G1->1 and G2->1 UNCONDITIONALLY -- i.e.
// Ess(alpha->0, cosTheta)=1 for BOTH the height-correlated G2 model and
// the separable model (unlike DL-86's cosTheta->0 case, the separable
// model needs no baked limit constant here).  Two zones are baked below
// this boundary and the existing row 0/row 1 data: ALPHA_SUB_FINE
// geometric octaves on (0, 0.01) (node j stores alpha=0.01/2^(8-j),
// j=1..7, i.e. 0.005 down to 7.8e-5 -- comfortably past GGXBRDF's 1e-4
// floor), and ALPHA_MID_SIZE-1 geometric nodes on (0.01, 0.0419]
// bridging the coarse first cell.  Interpolation is LINEAR IN ALPHA on
// every interval (matching ANISO_SUB_FINE's precedent: node PLACEMENT
// is geometric, the BLEND between two adjacent nodes is not).
static const int ALPHA_SUB_FINE = 7;
static const int ALPHA_MID_SIZE = 4;

// Total virtual node-index range on [0, A1] is [0, ALPHA_LOW_TOTAL]:
// index 0 is the exact alpha->0 boundary (not stored), 1..ALPHA_SUB_FINE
// are the geometric sub-nodes below A0 (stored), ALPHA_SUB_FINE+1 is A0
// itself (existing row 0, not re-baked), ALPHA_SUB_FINE+2..
// ALPHA_LOW_TOTAL-1 are the geometric mid-nodes between A0 and A1
// (stored), and ALPHA_LOW_TOTAL is A1 itself (existing row 1, not
// re-baked).  ALPHA_SUB_FINE + (ALPHA_MID_SIZE-1) = 10 values are
// stored per table.
static const int ALPHA_LOW_TOTAL = ALPHA_SUB_FINE + ALPHA_MID_SIZE;
static const int ALPHA_LOW_STORED = ALPHA_SUB_FINE + (ALPHA_MID_SIZE - 1);

// alpha value of virtual node `idx` (idx in [0, ALPHA_LOW_TOTAL]).  The
// emitted header carries a byte-identical twin, AlphaLowNode; the two
// MUST agree or the baked values land at the wrong abscissae.  A0/A1
// are the EXACT alpha values of the main table's row 0 / row 1 (see the
// alpha-mapping comment in main()'s bake loop).
static double alphaLowNode(int idx, double A0, double A1) {
	if(idx <= 0) return 0.0;
	if(idx <= ALPHA_SUB_FINE) return A0 * pow(2.0, (double)(idx - (ALPHA_SUB_FINE + 1)));
	if(idx == ALPHA_SUB_FINE + 1) return A0;
	if(idx < ALPHA_LOW_TOTAL) {
		const double t = (double)(idx - (ALPHA_SUB_FINE + 1)) / (double)ALPHA_MID_SIZE;
		return A0 * pow(A1 / A0, t);
	}
	return A1;
}

// Map a stored virtual index (1..ALPHA_SUB_FINE, or ALPHA_SUB_FINE+2..
// ALPHA_LOW_TOTAL-1) to its flat storage slot (0..ALPHA_LOW_STORED-1).
static int alphaLowSlot(int idx) {
	if(idx <= ALPHA_SUB_FINE) return idx - 1;
	return ALPHA_SUB_FINE + (idx - (ALPHA_SUB_FINE + 2));
}

// cosTheta of aniso sub-grid node `n` (n in [0, ANISO_SUB_TOTAL]).  The
// emitted header carries a byte-identical twin, AnisoSubNodeCos; the
// two MUST agree or the baked values land at the wrong abscissae.
static double anisoSubNodeCos(int n, double c0) {
	if(n <= 0) return 0.0;
	if(n >= ANISO_SUB_TOTAL) return c0;
	const double h = c0 / (double)SUB_SIZE;
	if(n <= ANISO_SUB_FINE) return h / (double)(1 << (ANISO_SUB_FINE + 1 - n));
	return (double)(n - ANISO_SUB_FINE) * h;
}

// DL-86: the cosTheta at which the cosTheta->0 boundary value is probed
// for the SEPARABLE model (the height-correlated G2 model's boundary is
// exactly 1 and is not probed at all -- see the LookupEssG2 comment
// emitted below for the proof).  Converged: an independent quadrature
// reads 0.93436251 at 1e-6 and 0.93436415 at 1e-7 for alpha=0.05, i.e.
// stable to ~2e-6, far inside this bake's own Monte-Carlo error.
static const double SUB_LIMIT_COS = 1e-7;

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
	// adjacent LUT bin centers (DL-86: between the grazing sub-grid's
	// own nodes below the first bin center c0, and flat-clamped above
	// the last center) -- exactly what its bilinear-interpolation code
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
		// model: a DL-86 grazing sub-interval (below the first bin
		// center), the linear span between two adjacent bin centers, or
		// the flat right end-cap above the last center.
		// Ess(cos) = essLo + slope*(cos-lo) for cos in [lo,hi].
		struct Segment
		{
			Scalar lo, hi;
			Scalar essLo, slope;
		};

		// Build the LUT_SIZE+SUB_SIZE segments (40 at SUB_SIZE=8:
		// SUB_SIZE grazing sub-intervals + LUT_SIZE-1 interior spans +
		// 1 right cap) from an already-resolved essRow / subRow / essLimit
		// (either a single exact LUT row, or LookupEss's alpha-blended one).
		//
		// DL-86: the left end-cap [0, c0] used to be a single FLAT
		// segment (Ess clamped to row 0).  It is now SUB_SIZE linear
		// pieces through the baked grazing sub-grid, reading exactly the
		// nodes SubNodeEss/SubNodeEssG2 give LookupEss/LookupEssG2 -- this
		// is load-bearing, not cosmetic: MSPdf/MSPdfG2 call
		// LookupEss/LookupEssG2 directly while SampleMSCosTheta/
		// SampleMSCosThetaG2 sample from THESE segments, and the file's
		// own header comment guarantees the two "cannot drift apart".
		// `essLimit` is the cosTheta=0 boundary value (1 for the
		// height-correlated G2 model, E_ss_LIMIT_TABLE's blended row for
		// the separable one); `subRow` holds the SUB_SIZE-1 interior
		// nodes.  All three inputs are LINEAR in the alpha blend, so this
		// preserves MSLobeZ/MSLobeZG2's documented "Z is an exact affine
		// function of af" per-row-then-blend optimization unchanged.
		inline void BuildSegmentsFromRow( const Scalar essRow[LUT_SIZE], const Scalar subRow[SUB_SIZE-1], const Scalar essLimit, Segment segs[LUT_SIZE + SUB_SIZE], int& nSegs )
		{
			nSegs = 0;
			const Scalar c0 = 0.5 / Scalar(LUT_SIZE);
			const Scalar cLast = (Scalar(LUT_SIZE) - 0.5) / Scalar(LUT_SIZE);

			// DL-86 grazing sub-intervals covering [0, c0].
			const Scalar h = c0 / Scalar(SUB_SIZE);
			for( int k = 0; k < SUB_SIZE; k++ )
			{
				const Scalar vLo = (k == 0) ? essLimit : subRow[k-1];
				const Scalar vHi = (k == SUB_SIZE - 1) ? essRow[0] : subRow[k];
				segs[nSegs].lo = Scalar(k) * h; segs[nSegs].hi = Scalar(k + 1) * h;
				segs[nSegs].essLo = vLo;
				segs[nSegs].slope = (vHi - vLo) / h;
				nSegs++;
			}

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
		//
		// DL-105: below ALPHA_LOW_A1, source the row from the low-alpha
		// sub-grid instead -- MUST stay in sync with LookupEss's own
		// low-alpha branch, which is why both call BuildLowAlphaRow.
		inline void BuildSegments( const Scalar alphaEff, Segment segs[LUT_SIZE + SUB_SIZE], int& nSegs )
		{
			if( alphaEff < ALPHA_LOW_A1 )
			{
				Scalar essRow[LUT_SIZE]; Scalar subRow[SUB_SIZE-1]; Scalar essLimit;
				BuildLowAlphaRow( alphaEff, essRow, subRow, essLimit );
				BuildSegmentsFromRow( essRow, subRow, essLimit, segs, nSegs );
				return;
			}

			Scalar a = r_max(0.0, r_min(1.0, (alphaEff - 0.01) / (1.0 - 0.01))) * (LUT_SIZE - 1);
			int ai0 = (int)a;
			int ai1 = r_min(ai0 + 1, LUT_SIZE - 1);
			Scalar af = a - ai0;

			Scalar essRow[LUT_SIZE];
			for( int k = 0; k < LUT_SIZE; k++ )
				essRow[k] = (1-af) * E_ss_TABLE[ai0][k] + af * E_ss_TABLE[ai1][k];

			// DL-86: the same blend on the grazing sub-grid and its
			// cosTheta=0 boundary constant.
			Scalar subRow[SUB_SIZE-1];
			for( int k = 0; k < SUB_SIZE - 1; k++ )
				subRow[k] = (1-af) * E_ss_SUB_TABLE[ai0][k] + af * E_ss_SUB_TABLE[ai1][k];
			const Scalar essLimit = (1-af) * E_ss_LIMIT_TABLE[ai0] + af * E_ss_LIMIT_TABLE[ai1];

			BuildSegmentsFromRow( essRow, subRow, essLimit, segs, nSegs );
		}

		// DL-63: height-correlated-G2 twin of BuildSegments above, using
		// EXACTLY LookupEssG2's alpha-row blend (E_ss_TABLE_G2 instead of
		// E_ss_TABLE).  Segment/SegShape/SegCDF/SegTotal/SegInvert and
		// BuildSegmentsFromRow are already table-agnostic (they only see
		// an already-resolved essRow), so only the alpha-row blend needs
		// a G2-specific twin.
		//
		// DL-105: below ALPHA_LOW_A1, source the row from the low-alpha
		// sub-grid instead -- MUST stay in sync with LookupEssG2's own
		// low-alpha branch, which is why both call BuildLowAlphaRowG2.
		inline void BuildSegmentsG2( const Scalar alphaEff, Segment segs[LUT_SIZE + SUB_SIZE], int& nSegs )
		{
			if( alphaEff < ALPHA_LOW_A1 )
			{
				Scalar essRow[LUT_SIZE]; Scalar subRow[SUB_SIZE-1];
				BuildLowAlphaRowG2( alphaEff, essRow, subRow );
				BuildSegmentsFromRow( essRow, subRow, Scalar(1.0), segs, nSegs );
				return;
			}

			Scalar a = r_max(0.0, r_min(1.0, (alphaEff - 0.01) / (1.0 - 0.01))) * (LUT_SIZE - 1);
			int ai0 = (int)a;
			int ai1 = r_min(ai0 + 1, LUT_SIZE - 1);
			Scalar af = a - ai0;

			Scalar essRow[LUT_SIZE];
			for( int k = 0; k < LUT_SIZE; k++ )
				essRow[k] = (1-af) * E_ss_TABLE_G2[ai0][k] + af * E_ss_TABLE_G2[ai1][k];

			// DL-86: this model's cosTheta=0 boundary is exactly 1, so
			// only the interior sub-nodes need blending.
			Scalar subRow[SUB_SIZE-1];
			for( int k = 0; k < SUB_SIZE - 1; k++ )
				subRow[k] = (1-af) * E_ss_SUB_TABLE_G2[ai0][k] + af * E_ss_SUB_TABLE_G2[ai1][k];

			BuildSegmentsFromRow( essRow, subRow, Scalar(1.0), segs, nSegs );
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
	///
	/// DL-105: below ALPHA_LOW_A1 this does NOT use the cached-affine
	/// row shortcut above -- it recomputes segments+SegTotal directly
	/// from BuildLowAlphaRow every call, a deliberate perf/simplicity
	/// trade-off for a rare regime (very smooth surfaces, alpha<0.0419)
	/// rather than precomputing and affine-blending Z at the 13 low-
	/// alpha virtual nodes too.  Correctness does not depend on the
	/// cache: MSPdf calls LookupEss (also low-alpha-aware) and
	/// SampleMSCosTheta calls BuildSegments (also low-alpha-aware) --
	/// as long as this function's Z integrates the SAME row those two
	/// use, which it does (BuildLowAlphaRow is the single source both
	/// this function and BuildSegments read).
	inline Scalar MSLobeZ( const Scalar alphaEff )
	{
		if( alphaEff < ALPHA_LOW_A1 )
		{
			Scalar essRow[LUT_SIZE]; Scalar subRow[SUB_SIZE-1]; Scalar essLimit;
			BuildLowAlphaRow( alphaEff, essRow, subRow, essLimit );
			MSLobeDetail::Segment segs[LUT_SIZE + SUB_SIZE];
			int nSegs = 0;
			MSLobeDetail::BuildSegmentsFromRow( essRow, subRow, essLimit, segs, nSegs );
			Scalar I = 0.0;
			for( int i = 0; i < nSegs; i++ )
				I += MSLobeDetail::SegTotal( segs[i] );
			return 2.0 * I;
		}

		// Per-alpha-row Z, computed once (C++11 magic-statics: thread-safe
		// initialization, no locking on the steady-state read path).
		static const std::array<Scalar, LUT_SIZE> rowZ = []() {
			std::array<Scalar, LUT_SIZE> z{};
			for( int row = 0; row < LUT_SIZE; row++ )
			{
				MSLobeDetail::Segment segs[LUT_SIZE + SUB_SIZE];
				int nSegs = 0;
				MSLobeDetail::BuildSegmentsFromRow( E_ss_TABLE[row], E_ss_SUB_TABLE[row], E_ss_LIMIT_TABLE[row], segs, nSegs );
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
		MSLobeDetail::Segment segs[LUT_SIZE + SUB_SIZE];
		int nSegs = 0;
		MSLobeDetail::BuildSegments( alphaEff, segs, nSegs );

		Scalar totals[LUT_SIZE + SUB_SIZE];
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
	///
	/// DL-105: below ALPHA_LOW_A1, recompute directly from
	/// BuildLowAlphaRowG2 instead of the cached-affine row shortcut --
	/// see MSLobeZ's own comment for the rationale.
	inline Scalar MSLobeZG2( const Scalar alphaEff )
	{
		if( alphaEff < ALPHA_LOW_A1 )
		{
			Scalar essRow[LUT_SIZE]; Scalar subRow[SUB_SIZE-1];
			BuildLowAlphaRowG2( alphaEff, essRow, subRow );
			MSLobeDetail::Segment segs[LUT_SIZE + SUB_SIZE];
			int nSegs = 0;
			MSLobeDetail::BuildSegmentsFromRow( essRow, subRow, Scalar(1.0), segs, nSegs );
			Scalar I = 0.0;
			for( int i = 0; i < nSegs; i++ )
				I += MSLobeDetail::SegTotal( segs[i] );
			return 2.0 * I;
		}

		static const std::array<Scalar, LUT_SIZE> rowZ = []() {
			std::array<Scalar, LUT_SIZE> z{};
			for( int row = 0; row < LUT_SIZE; row++ )
			{
				MSLobeDetail::Segment segs[LUT_SIZE + SUB_SIZE];
				int nSegs = 0;
				MSLobeDetail::BuildSegmentsFromRow( E_ss_TABLE_G2[row], E_ss_SUB_TABLE_G2[row], Scalar(1.0), segs, nSegs );
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
		MSLobeDetail::Segment segs[LUT_SIZE + SUB_SIZE];
		int nSegs = 0;
		MSLobeDetail::BuildSegmentsG2( alphaEff, segs, nSegs );

		Scalar totals[LUT_SIZE + SUB_SIZE];
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

	/// DL-86 round 2: cosTheta of aniso sub-grid node `n`, n in
	/// [0, ANISO_SUB_TOTAL].  n=0 is the exact cosTheta->0 anchor;
	/// n=1..ANISO_SUB_FINE are the geometric octaves h*2^-(FINE+1-n)
	/// (so n=ANISO_SUB_FINE is h/2); n=ANISO_SUB_FINE+k for k=1..SUB_SIZE
	/// are the uniform nodes k*h, the last of which IS c0 = bin 0.
	/// h = c0/SUB_SIZE.
	///
	/// `c0` is passed in rather than derived because the isotropic and
	/// aniso tables could in principle differ in cosTheta resolution
	/// (they are both 32 today); every caller passes
	/// 0.5/ANISO_COS_SIZE.  The generator has a byte-identical twin,
	/// anisoSubNodeCos -- they MUST agree or the baked values land at
	/// the wrong abscissae.
	inline Scalar AnisoSubNodeCos( const int n, const Scalar c0 )
	{
		if( n <= 0 ) return Scalar(0.0);
		if( n >= ANISO_SUB_TOTAL ) return c0;
		const Scalar h = c0 / Scalar(SUB_SIZE);
		if( n <= ANISO_SUB_FINE ) return h / Scalar( 1 << (ANISO_SUB_FINE + 1 - n) );
		return Scalar( n - ANISO_SUB_FINE ) * h;
	}

	/// DL-86 round 2: locate cc in [0, c0) on the aniso sub-grid --
	/// node index n0 in [0, ANISO_SUB_TOTAL-1] and the fraction nf of
	/// the way from node n0 to node n0+1, LINEAR IN COS (see the
	/// generator's ANISO_SUB_FINE comment for why not log-cos).  The
	/// non-uniform twin of SubNodeIndex; the geometric part is resolved
	/// by a bounded descending scan (ANISO_SUB_FINE iterations at
	/// worst) rather than a log2, which keeps the node abscissae
	/// bit-identical to AnisoSubNodeCos's own.
	inline void AnisoSubNodeIndex( const Scalar cc, const Scalar c0, int& n0, Scalar& nf )
	{
		const Scalar h = c0 / Scalar(SUB_SIZE);
		if( cc >= h )
		{
			int k = (int)( cc / h );
			if( k < 1 ) k = 1;
			if( k > SUB_SIZE - 1 ) k = SUB_SIZE - 1;
			n0 = ANISO_SUB_FINE + k;
			nf = ( cc - Scalar(k) * h ) / h;
		}
		else
		{
			n0 = 0;
			for( int n = ANISO_SUB_FINE; n >= 1; n-- )
			{
				if( cc >= AnisoSubNodeCos( n, c0 ) ) { n0 = n; break; }
			}
			const Scalar lo = AnisoSubNodeCos( n0, c0 );
			const Scalar hi = AnisoSubNodeCos( n0 + 1, c0 );
			nf = ( hi > lo ) ? ( ( cc - lo ) / ( hi - lo ) ) : Scalar(0.0);
		}
		if( nf < 0 ) nf = 0;
		if( nf > 1 ) nf = 1;
	}

	namespace MSLobeDetail
	{
		// Generic-N twin of BuildSegmentsFromRow above (identical algebra,
		// parameterized on row length instead of fixed at LUT_SIZE) --
		// needed because the DL-77 aniso table's cosTheta resolution
		// (ANISO_COS_SIZE) differs from the isotropic tables' LUT_SIZE.
		//
		// DL-86: `subRow` carries the ANISO_SUB_TOTAL-1 grazing sub-grid
		// nodes -- NOT the isotropic SUB_SIZE-1 set BuildSegmentsFromRow
		// takes, since round 2 refines the aniso grid's lowest interval
		// geometrically (see ANISO_SUB_FINE).  This twin is only ever
		// used for the height-correlated G2 model, whose cosTheta=0
		// boundary is the exact constant 1, so there is no essLimit
		// parameter.  Keeping this end-cap in step with
		// LookupEssG2Aniso's own below-c0 branch is load-bearing for the
		// same reason as in the isotropic pair: MSPdfG2Aniso calls
		// LookupEssG2Aniso directly while SampleMSCosThetaG2Aniso
		// inverts THESE segments.  `segs` must hold N+ANISO_SUB_TOTAL.
		inline void BuildSegmentsFromRowN( const Scalar* essRow, const Scalar subRow[ANISO_SUB_TOTAL-1], const int N, Segment segs[], int& nSegs )
		{
			nSegs = 0;
			const Scalar c0 = 0.5 / Scalar(N);
			const Scalar cLast = (Scalar(N) - 0.5) / Scalar(N);

			for( int n = 0; n < ANISO_SUB_TOTAL; n++ )
			{
				const Scalar lo = AnisoSubNodeCos( n, c0 );
				const Scalar hi = AnisoSubNodeCos( n + 1, c0 );
				const Scalar vLo = (n == 0) ? Scalar(1.0) : subRow[n-1];
				const Scalar vHi = (n == ANISO_SUB_TOTAL - 1) ? essRow[0] : subRow[n];
				segs[nSegs].lo = lo; segs[nSegs].hi = hi;
				segs[nSegs].essLo = vLo;
				segs[nSegs].slope = (vHi - vLo) / (hi - lo);
				nSegs++;
			}

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

	/// DL-161: alpha value of the ANISO grid's own row 0 / row 1 -- the
	/// two endpoints the aniso low-alpha sub-grid below is anchored to.
	/// Distinct from the isotropic ALPHA_LOW_A0/A1 (defined above): the
	/// aniso grid has ANISO_ALPHA_SIZE=24 nodes (not LUT_SIZE=32), so its
	/// own row1 sits at a coarser ~5.3x ratio from row0 (isotropic:
	/// ~4.2x).  ALPHA_LOW_A0 is numerically identical (both are 0.01),
	/// reused as-is.
	static const Scalar ANISO_ALPHA_LOW_A1 = Scalar(0.01) + Scalar(1.0 - 0.01) * Scalar(1.0) / Scalar(ANISO_ALPHA_SIZE - 1);

	/// DL-161: alpha value of aniso low-alpha virtual node `idx` (idx in
	/// [0, ALPHA_LOW_TOTAL]) -- the SAME node-count/placement
	/// construction (ALPHA_SUB_FINE geometric octaves below A0,
	/// ALPHA_MID_SIZE-1 geometric nodes between A0 and A1) DL-105 used
	/// for the isotropic axis, re-anchored to ANISO_ALPHA_LOW_A1 instead
	/// of ALPHA_LOW_A1.  The below-A0 octaves (idx 1..ALPHA_SUB_FINE)
	/// depend only on A0=0.01, shared with the isotropic axis, so those
	/// 7 alpha VALUES are numerically identical to AlphaLowNode's -- only
	/// the mid-section (between A0 and A1) differs, since A1 differs.
	inline Scalar AnisoAlphaLowNode( const int idx )
	{
		if( idx <= 0 ) return Scalar(0.0);
		if( idx <= ALPHA_SUB_FINE ) return ALPHA_LOW_A0 * pow( Scalar(2.0), Scalar(idx - (ALPHA_SUB_FINE + 1)) );
		if( idx == ALPHA_SUB_FINE + 1 ) return ALPHA_LOW_A0;
		if( idx < ALPHA_LOW_TOTAL )
		{
			const Scalar t = Scalar(idx - (ALPHA_SUB_FINE + 1)) / Scalar(ALPHA_MID_SIZE);
			return ALPHA_LOW_A0 * pow( ANISO_ALPHA_LOW_A1 / ALPHA_LOW_A0, t );
		}
		return ANISO_ALPHA_LOW_A1;
	}

	/// DL-161: bracket a query alpha in [0, ANISO_ALPHA_LOW_A1) against
	/// the AnisoAlphaLowNode list -- twin of AlphaLowIndex, reusing the
	/// SAME AlphaLowSlot storage-index mapping (layout-only, independent
	/// of the node VALUES, so no aniso-specific twin of it is needed).
	/// idx0==0 (the exact alpha->0 virtual node) is a value this scan can
	/// return; every *Ordinary/*Grazing/*Eavg accessor below folds it to
	/// idx=1's own baked row instead of dereferencing it, because unlike
	/// the isotropic axis the anisotropic alpha->0 boundary is NOT simply
	/// Ess=1 (Lambda_Aniso stays finite off-axis) and has no closed form
	/// -- and because it is PROVABLY UNREACHABLE from any production
	/// roughness (GGXBRDF.cpp/GGXSPF.cpp floor authored alpha at 1e-4,
	/// comfortably inside the [AnisoAlphaLowNode(1)=7.8e-5,
	/// AnisoAlphaLowNode(2)~1.6e-4) octave).  See
	/// docs/DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md "DL-161".
	inline void AnisoAlphaLowIndex( const Scalar alpha, int& idx0, int& idx1, Scalar& frac )
	{
		const Scalar a = r_max( Scalar(0.0), alpha );
		idx0 = 0;
		for( int j = 0; j < ALPHA_LOW_TOTAL; j++ )
		{
			if( a >= AnisoAlphaLowNode( j + 1 ) ) idx0 = j + 1; else break;
		}
		if( idx0 > ALPHA_LOW_TOTAL - 1 ) idx0 = ALPHA_LOW_TOTAL - 1;
		idx1 = idx0 + 1;
		const Scalar lo = AnisoAlphaLowNode(idx0), hi = AnisoAlphaLowNode(idx1);
		frac = (hi > lo) ? r_max( Scalar(0.0), r_min( Scalar(1.0), (a - lo) / (hi - lo) ) ) : Scalar(0.0);
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

	/// DL-86: value of the azimuth-AVERAGED aniso grazing model at
	/// sub-node n (n in [0, ANISO_SUB_TOTAL]) of cell (xi,yi).  Same
	/// contract as the isotropic SubNodeEssG2: n==0 is the exact boundary
	/// 1, n==ANISO_SUB_TOTAL is the cell's own first ordinary bin.  The
	/// node's cosTheta is AnisoSubNodeCos(n, 0.5/ANISO_COS_SIZE) -- the
	/// grid is NOT uniform (round 2, see ANISO_SUB_FINE).
	inline Scalar AnisoSubNodeEssG2( const int xi, const int yi, const int n )
	{
		if( n <= 0 ) return Scalar(1.0);
		if( n >= ANISO_SUB_TOTAL ) return E_ss_TABLE_G2_ANISO[xi][yi][0];
		return E_ss_TABLE_G2_ANISO_SUB[xi][yi][n-1];
	}

	/// DL-86: per-azimuth twin of AnisoSubNodeEssG2.
	inline Scalar AnisoPhiSubNodeEssG2( const int xi, const int yi, const int pi, const int n )
	{
		if( n <= 0 ) return Scalar(1.0);
		if( n >= ANISO_SUB_TOTAL ) return E_ss_TABLE_G2_ANISO_PHI[xi][yi][pi][0];
		return E_ss_TABLE_G2_ANISO_PHI_SUB[xi][yi][pi][n-1];
	}

	/// DL-161: per-azimuth aniso E_ss_G2 at a cell whose X axis is EITHER
	/// an ordinary grid index (xLow==false, xIdx in [0,ANISO_ALPHA_SIZE))
	/// or a low-alpha virtual node index (xLow==true, an
	/// AnisoAlphaLowIndex idx0/idx1 value), paired against an ORDINARY Y
	/// grid index yi.  See AnisoAlphaLowIndex's own comment for why
	/// idx<=0 folds to idx=1's baked row instead of a fabricated anchor.
	inline Scalar AnisoPhiCellMixedX( const bool xLow, const int xIdx, const int yi, const int pi, const int ci )
	{
		if( !xLow ) return E_ss_TABLE_G2_ANISO_PHI[xIdx][yi][pi][ci];
		const int idx = (xIdx <= 0) ? 1 : xIdx;
		if( idx == ALPHA_SUB_FINE + 1 ) return E_ss_TABLE_G2_ANISO_PHI[0][yi][pi][ci];
		if( idx >= ALPHA_LOW_TOTAL ) return E_ss_TABLE_G2_ANISO_PHI[1][yi][pi][ci];
		return E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_X[ AlphaLowSlot(idx) ][yi][pi][ci];
	}

	/// DL-161: grazing-sub-grid twin of AnisoPhiCellMixedX above.  Same
	/// n<=0 / n>=ANISO_SUB_TOTAL boundary contract as the pre-existing
	/// AnisoPhiSubNodeEssG2 (n<=0 is the exact cosTheta->0 boundary,
	/// exactly 1 for the G2 model; n>=ANISO_SUB_TOTAL IS the cell's own
	/// ordinary bin 0, read via AnisoPhiCellMixedX rather than the SUB
	/// table, which only stores the ANISO_SUB_TOTAL-1 INTERIOR nodes) --
	/// AnisoSubNodeIndex's own k0/k0+1 pair reaches both ends of this
	/// range (verified: n=ANISO_SUB_TOTAL is reachable at cosTheta values
	/// close to c0, e.g. cos=0.0156).
	inline Scalar AnisoPhiSubCellMixedX( const bool xLow, const int xIdx, const int yi, const int pi, const int k )
	{
		if( k <= 0 ) return Scalar(1.0);
		if( k >= ANISO_SUB_TOTAL ) return AnisoPhiCellMixedX( xLow, xIdx, yi, pi, 0 );
		if( !xLow ) return E_ss_TABLE_G2_ANISO_PHI_SUB[xIdx][yi][pi][k-1];
		const int idx = (xIdx <= 0) ? 1 : xIdx;
		if( idx == ALPHA_SUB_FINE + 1 ) return E_ss_TABLE_G2_ANISO_PHI_SUB[0][yi][pi][k-1];
		if( idx >= ALPHA_LOW_TOTAL ) return E_ss_TABLE_G2_ANISO_PHI_SUB[1][yi][pi][k-1];
		return E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_X_SUB[ AlphaLowSlot(idx) ][yi][pi][k-1];
	}

	/// DL-161: per-azimuth aniso E_ss_G2 at a cell where BOTH axes are
	/// low-alpha virtual node indices (or a boundary forward to A0/A1).
	/// Dispatches to the dedicated both-low corner table only when
	/// NEITHER index is a boundary forward; a boundary-vs-interior pair
	/// reduces to the ALPHALOW_X table (with a phi flip when it is the Y
	/// index that is interior-low, by the X<->Y relabel symmetry -- see
	/// AnisoPhiIndex's own comment), and a boundary-vs-boundary pair
	/// reduces to the main ordinary table.
	inline Scalar AnisoPhiCellBothLow( const int xIdx, const int yIdx, const int pi, const int ci )
	{
		const int ix = (xIdx <= 0) ? 1 : xIdx;
		const int iy = (yIdx <= 0) ? 1 : yIdx;
		const bool ixBoundary = (ix == ALPHA_SUB_FINE + 1) || (ix >= ALPHA_LOW_TOTAL);
		const bool iyBoundary = (iy == ALPHA_SUB_FINE + 1) || (iy >= ALPHA_LOW_TOTAL);
		if( ixBoundary && iyBoundary )
		{
			const int xo = (ix == ALPHA_SUB_FINE + 1) ? 0 : 1;
			const int yo = (iy == ALPHA_SUB_FINE + 1) ? 0 : 1;
			return E_ss_TABLE_G2_ANISO_PHI[xo][yo][pi][ci];
		}
		if( iyBoundary )
		{
			const int yo = (iy == ALPHA_SUB_FINE + 1) ? 0 : 1;
			return E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_X[ AlphaLowSlot(ix) ][yo][pi][ci];
		}
		if( ixBoundary )
		{
			const int xo = (ix == ALPHA_SUB_FINE + 1) ? 0 : 1;
			return E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_X[ AlphaLowSlot(iy) ][xo][ ANISO_PHI_SIZE - 1 - pi ][ci];
		}
		return E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_XY[ AlphaLowSlot(ix) ][ AlphaLowSlot(iy) ][pi][ci];
	}

	/// DL-161: grazing-sub-grid twin of AnisoPhiCellBothLow above.  Same
	/// n<=0 / n>=ANISO_SUB_TOTAL boundary contract as AnisoPhiSubCellMixedX
	/// above -- see its own comment.
	inline Scalar AnisoPhiSubCellBothLow( const int xIdx, const int yIdx, const int pi, const int k )
	{
		if( k <= 0 ) return Scalar(1.0);
		if( k >= ANISO_SUB_TOTAL ) return AnisoPhiCellBothLow( xIdx, yIdx, pi, 0 );
		const int ix = (xIdx <= 0) ? 1 : xIdx;
		const int iy = (yIdx <= 0) ? 1 : yIdx;
		const bool ixBoundary = (ix == ALPHA_SUB_FINE + 1) || (ix >= ALPHA_LOW_TOTAL);
		const bool iyBoundary = (iy == ALPHA_SUB_FINE + 1) || (iy >= ALPHA_LOW_TOTAL);
		if( ixBoundary && iyBoundary )
		{
			const int xo = (ix == ALPHA_SUB_FINE + 1) ? 0 : 1;
			const int yo = (iy == ALPHA_SUB_FINE + 1) ? 0 : 1;
			return E_ss_TABLE_G2_ANISO_PHI_SUB[xo][yo][pi][k-1];
		}
		if( iyBoundary )
		{
			const int yo = (iy == ALPHA_SUB_FINE + 1) ? 0 : 1;
			return E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_X_SUB[ AlphaLowSlot(ix) ][yo][pi][k-1];
		}
		if( ixBoundary )
		{
			const int xo = (ix == ALPHA_SUB_FINE + 1) ? 0 : 1;
			return E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_X_SUB[ AlphaLowSlot(iy) ][xo][ ANISO_PHI_SIZE - 1 - pi ][k-1];
		}
		return E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_XY_SUB[ AlphaLowSlot(ix) ][ AlphaLowSlot(iy) ][pi][k-1];
	}

	/// DL-77: anisotropic twin of LookupEssG2.  Falls back to the exact
	/// isotropic LookupEssG2 when alphaX==alphaY (see file-header note).
	///
	/// DL-86: this azimuth-AVERAGED table feeds ONLY the H6
	/// multiscatter-lobe SAMPLER's proposal shape (MSLobeZG2Aniso/
	/// SampleMSCosThetaG2Aniso/MSPdfG2Aniso) -- never a direct
	/// ENERGY-COMPENSATION call site, which all route through the
	/// per-azimuth LookupEssG2AnisoDirectional below.  It still gets the
	/// grazing sub-grid (round 2: including its geometric refinement),
	/// for two reasons that are about consistency, not about proposal
	/// precision: (a) leaving it flat while its own alphaX==alphaY
	/// fallback (LookupEssG2) is not would put a step in the proposal
	/// shape as a surface approaches isotropy, and (b) the sub-table is
	/// DERIVED from the phi-resolved bake by the same trapezoidal average
	/// that already produces E_ss_TABLE_G2_ANISO, so it costs nothing
	/// extra to bake.  MSPdfG2Aniso reads THIS function while
	/// SampleMSCosThetaG2Aniso inverts MSLobeDetail's segments over the
	/// same nodes, so the two cannot describe different densities.
	inline Scalar LookupEssG2Aniso( const Scalar cosTheta, const Scalar alphaX, const Scalar alphaY )
	{
		if( fabs(alphaX - alphaY) < 1e-9 ) return LookupEssG2( cosTheta, alphaX );

		const Scalar aX = r_max(0.01, r_min(1.0, alphaX));
		const Scalar aY = r_max(0.01, r_min(1.0, alphaY));

		int xi0, xi1; Scalar xf;
		AnisoAlphaIndex( aX, xi0, xi1, xf );
		int yi0, yi1; Scalar yf;
		AnisoAlphaIndex( aY, yi0, yi1, yf );

		const Scalar cc = r_max(0.0, r_min(1.0, cosTheta));
		const Scalar cSub0 = Scalar(0.5) / Scalar(ANISO_COS_SIZE);
		if( cc < cSub0 )
		{
			int k0; Scalar kf;
			AnisoSubNodeIndex( cc, cSub0, k0, kf );
			Scalar vXY[2][2];
			for( int xi = 0; xi < 2; xi++ )
			{
				const int xiv = (xi == 0) ? xi0 : xi1;
				for( int yi = 0; yi < 2; yi++ )
				{
					const int yiv = (yi == 0) ? yi0 : yi1;
					vXY[xi][yi] = (1-kf) * AnisoSubNodeEssG2(xiv, yiv, k0) + kf * AnisoSubNodeEssG2(xiv, yiv, k0 + 1);
				}
			}
			const Scalar s0 = (1-yf)*vXY[0][0] + yf*vXY[0][1];
			const Scalar s1 = (1-yf)*vXY[1][0] + yf*vXY[1][1];
			return r_max( Scalar(0.0), r_min( Scalar(1.0), (1-xf)*s0 + xf*s1 ) );
		}

		Scalar c = cc * ANISO_COS_SIZE - 0.5;
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
	///
	/// DL-86: below the first cosTheta bin center c0=0.5/ANISO_COS_SIZE
	/// this shared the isotropic flat-clamp pattern, and this IS a direct
	/// energy-compensation call site.  It now interpolates the baked
	/// per-azimuth grazing sub-grid (E_ss_TABLE_G2_ANISO_PHI_SUB, via
	/// AnisoPhiSubNodeEssG2), anchored at the same exact boundary 1 --
	/// the height-correlated Smith argument holds PER DIRECTION
	/// (Lambda_Aniso(wi) -> infinity as cosWi -> 0 at every azimuth), so
	/// the boundary is azimuth-independent.
	///
	/// Round 2 made that sub-grid NON-UNIFORM (ANISO_SUB_FINE): the
	/// uniform version's lowest interval was a single straight ramp from
	/// the anchor across a drop of 0.265 at (0.01,1.0,phi=0), and read
	/// +2.5% at cos=1e-4 rising to +5.9% at 1e-3.
	///
	/// Measured against an independent 20M-sample per-azimuth quadrature
	/// over cos in {1e-4, 2.5e-4, 5e-4, 1e-3, 2e-3, 5e-3, 1e-2, 1.56e-2}:
	/// <=0.34% relative at EXACT (alphaX, alphaY, phi) grid nodes -- the
	/// configurations where cosTheta is the only interpolated axis, i.e.
	/// where this end-cap is the only thing being measured -- against up
	/// to 18% for the pre-DL-86 flat clamp and up to 5.9% for round 1's
	/// uniform sub-grid.  OFF-node the residual is the ANISO GRID's own
	/// interpolation error, not this end-cap's: <=0.57% with one alpha
	/// off-node, 3-4% off-node on all three of alphaX, alphaY and phi
	/// (DL-105 / DL-77's tracked ANISO_PHI and low-alpha residual).  See
	/// docs/DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md "DL-86" for the full
	/// before/after table and the probe protocol.
	///
	/// DL-161: LookupEssG2AnisoDirectionalLowX/LowXY below handle the
	/// case where alphaX and/or alphaY fall below ANISO_ALPHA_LOW_A1 --
	/// see this function's own dispatch and docs/DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md "DL-161".
	inline Scalar LookupEssG2AnisoDirectionalLowX( const Scalar cosTheta, const Scalar localX, const Scalar localY, const Scalar aXLow, const Scalar aYOrd )
	{
		int xi0, xi1; Scalar xf;
		AnisoAlphaLowIndex( aXLow, xi0, xi1, xf );
		int yi0, yi1; Scalar yf;
		AnisoAlphaIndex( aYOrd, yi0, yi1, yf );
		int pi0, pi1; Scalar pf;
		AnisoPhiIndex( localX, localY, pi0, pi1, pf );

		const Scalar cc = r_max(0.0, r_min(1.0, cosTheta));
		const Scalar cSub0 = Scalar(0.5) / Scalar(ANISO_COS_SIZE);
		const bool belowC0 = ( cc < cSub0 );

		int k0 = 0; Scalar kf = 0;
		int ci0 = 0, ci1 = 0; Scalar cf = 0;
		if( belowC0 ) { AnisoSubNodeIndex( cc, cSub0, k0, kf ); }
		else
		{
			Scalar c = cc * ANISO_COS_SIZE - 0.5;
			ci0 = (int)c;
			ci1 = r_min(ci0 + 1, ANISO_COS_SIZE - 1);
			cf = c - ci0;
		}

		Scalar vXY[2][2];
		for( int xi = 0; xi < 2; xi++ )
		{
			const int xiv = (xi == 0) ? xi0 : xi1;
			for( int yi = 0; yi < 2; yi++ )
			{
				const int yiv = (yi == 0) ? yi0 : yi1;
				Scalar vP0, vP1;
				if( belowC0 )
				{
					vP0 = (1-kf) * AnisoPhiSubCellMixedX(true, xiv, yiv, pi0, k0) + kf * AnisoPhiSubCellMixedX(true, xiv, yiv, pi0, k0 + 1);
					vP1 = (1-kf) * AnisoPhiSubCellMixedX(true, xiv, yiv, pi1, k0) + kf * AnisoPhiSubCellMixedX(true, xiv, yiv, pi1, k0 + 1);
				}
				else
				{
					vP0 = (1-cf) * AnisoPhiCellMixedX(true, xiv, yiv, pi0, ci0) + cf * AnisoPhiCellMixedX(true, xiv, yiv, pi0, ci1);
					vP1 = (1-cf) * AnisoPhiCellMixedX(true, xiv, yiv, pi1, ci0) + cf * AnisoPhiCellMixedX(true, xiv, yiv, pi1, ci1);
				}
				vXY[xi][yi] = (1-pf)*vP0 + pf*vP1;
			}
		}
		const Scalar v0 = (1-yf)*vXY[0][0] + yf*vXY[0][1];
		const Scalar v1 = (1-yf)*vXY[1][0] + yf*vXY[1][1];
		const Scalar v = (1-xf)*v0 + xf*v1;
		return belowC0 ? r_max( Scalar(0.0), r_min( Scalar(1.0), v ) ) : v;
	}

	/// DL-161: LookupEssG2AnisoDirectional's both-low branch -- SAME
	/// quadrilinear-interpolation shape as LookupEssG2AnisoDirectionalLowX,
	/// resolving BOTH axes via AnisoAlphaLowIndex/AnisoPhiCellBothLow.
	inline Scalar LookupEssG2AnisoDirectionalLowXY( const Scalar cosTheta, const Scalar localX, const Scalar localY, const Scalar aXLow, const Scalar aYLow )
	{
		int xi0, xi1; Scalar xf;
		AnisoAlphaLowIndex( aXLow, xi0, xi1, xf );
		int yi0, yi1; Scalar yf;
		AnisoAlphaLowIndex( aYLow, yi0, yi1, yf );
		int pi0, pi1; Scalar pf;
		AnisoPhiIndex( localX, localY, pi0, pi1, pf );

		const Scalar cc = r_max(0.0, r_min(1.0, cosTheta));
		const Scalar cSub0 = Scalar(0.5) / Scalar(ANISO_COS_SIZE);
		const bool belowC0 = ( cc < cSub0 );

		int k0 = 0; Scalar kf = 0;
		int ci0 = 0, ci1 = 0; Scalar cf = 0;
		if( belowC0 ) { AnisoSubNodeIndex( cc, cSub0, k0, kf ); }
		else
		{
			Scalar c = cc * ANISO_COS_SIZE - 0.5;
			ci0 = (int)c;
			ci1 = r_min(ci0 + 1, ANISO_COS_SIZE - 1);
			cf = c - ci0;
		}

		Scalar vXY[2][2];
		for( int xi = 0; xi < 2; xi++ )
		{
			const int xiv = (xi == 0) ? xi0 : xi1;
			for( int yi = 0; yi < 2; yi++ )
			{
				const int yiv = (yi == 0) ? yi0 : yi1;
				Scalar vP0, vP1;
				if( belowC0 )
				{
					vP0 = (1-kf) * AnisoPhiSubCellBothLow(xiv, yiv, pi0, k0) + kf * AnisoPhiSubCellBothLow(xiv, yiv, pi0, k0 + 1);
					vP1 = (1-kf) * AnisoPhiSubCellBothLow(xiv, yiv, pi1, k0) + kf * AnisoPhiSubCellBothLow(xiv, yiv, pi1, k0 + 1);
				}
				else
				{
					vP0 = (1-cf) * AnisoPhiCellBothLow(xiv, yiv, pi0, ci0) + cf * AnisoPhiCellBothLow(xiv, yiv, pi0, ci1);
					vP1 = (1-cf) * AnisoPhiCellBothLow(xiv, yiv, pi1, ci0) + cf * AnisoPhiCellBothLow(xiv, yiv, pi1, ci1);
				}
				vXY[xi][yi] = (1-pf)*vP0 + pf*vP1;
			}
		}
		const Scalar v0 = (1-yf)*vXY[0][0] + yf*vXY[0][1];
		const Scalar v1 = (1-yf)*vXY[1][0] + yf*vXY[1][1];
		const Scalar v = (1-xf)*v0 + xf*v1;
		return belowC0 ? r_max( Scalar(0.0), r_min( Scalar(1.0), v ) ) : v;
	}

	inline Scalar LookupEssG2AnisoDirectional( const Scalar cosTheta, const Scalar localX, const Scalar localY, const Scalar alphaX, const Scalar alphaY )
	{
		if( fabs(alphaX - alphaY) < 1e-9 ) return LookupEssG2( cosTheta, alphaX );

		// DL-161: the low-alpha dispatch MUST see the RAW alpha (floored
		// only at 0, not at 0.01) -- the r_max(0.01,...) clamp two lines
		// below exists for the ORDINARY grid's own domain and is applied
		// AFTER this check.  Clamping first would silently round every
		// alphaX/alphaY below 0.01 up to exactly 0.01 before the low-alpha
		// branch ever saw it, so e.g. alphaX=0.002 and alphaX=0.005 would
		// both dispatch as if alphaX==0.01 and return IDENTICAL results --
		// exactly the regression GGXHeightCorrelatedEnergyLUTTest's DL-161
		// rows caught (both then read the alpha=0.01 boundary row's own
		// value, discarding the true low alpha's sharper masking).  The
		// X<->Y relabel symmetry (Ess(a,b,phi) == Ess(b,a,90-phi)) lets a
		// single "low X" bake (LookupEssG2AnisoDirectionalLowX) cover the
		// "low Y" case too, by swapping (alphaX,alphaY,localX,localY).
		const Scalar rawX = r_max( Scalar(0.0), alphaX );
		const Scalar rawY = r_max( Scalar(0.0), alphaY );
		const bool xLow = rawX < ANISO_ALPHA_LOW_A1;
		const bool yLow = rawY < ANISO_ALPHA_LOW_A1;
		if( xLow && !yLow ) return LookupEssG2AnisoDirectionalLowX( cosTheta, localX, localY, rawX, rawY );
		if( yLow && !xLow ) return LookupEssG2AnisoDirectionalLowX( cosTheta, localY, localX, rawY, rawX );
		if( xLow && yLow )  return LookupEssG2AnisoDirectionalLowXY( cosTheta, localX, localY, rawX, rawY );

		const Scalar aX = r_max(0.01, r_min(1.0, alphaX));
		const Scalar aY = r_max(0.01, r_min(1.0, alphaY));

		int xi0, xi1; Scalar xf;
		AnisoAlphaIndex( aX, xi0, xi1, xf );
		int yi0, yi1; Scalar yf;
		AnisoAlphaIndex( aY, yi0, yi1, yf );
		int pi0, pi1; Scalar pf;
		AnisoPhiIndex( localX, localY, pi0, pi1, pf );

		const Scalar cc = r_max(0.0, r_min(1.0, cosTheta));
		const Scalar cSub0 = Scalar(0.5) / Scalar(ANISO_COS_SIZE);
		const bool belowC0 = ( cc < cSub0 );

		// Below c0 the cosTheta axis is resolved on the DL-86 sub-grid
		// (k0/kf); at or above it, on the ordinary bin centers (ci0/ci1/
		// cf).  Only the innermost cosTheta collapse differs -- the phi,
		// alphaY and alphaX collapses below are shared verbatim.
		int k0 = 0; Scalar kf = 0;
		int ci0 = 0, ci1 = 0; Scalar cf = 0;
		if( belowC0 )
		{
			AnisoSubNodeIndex( cc, cSub0, k0, kf );
		}
		else
		{
			Scalar c = cc * ANISO_COS_SIZE - 0.5;
			ci0 = (int)c;
			ci1 = r_min(ci0 + 1, ANISO_COS_SIZE - 1);
			cf = c - ci0;
		}

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
				Scalar vP0, vP1;
				if( belowC0 )
				{
					vP0 = (1-kf) * AnisoPhiSubNodeEssG2(xiv, yiv, pi0, k0) + kf * AnisoPhiSubNodeEssG2(xiv, yiv, pi0, k0 + 1);
					vP1 = (1-kf) * AnisoPhiSubNodeEssG2(xiv, yiv, pi1, k0) + kf * AnisoPhiSubNodeEssG2(xiv, yiv, pi1, k0 + 1);
				}
				else
				{
					vP0 = (1-cf) * E_ss_TABLE_G2_ANISO_PHI[xiv][yiv][pi0][ci0] + cf * E_ss_TABLE_G2_ANISO_PHI[xiv][yiv][pi0][ci1];
					vP1 = (1-cf) * E_ss_TABLE_G2_ANISO_PHI[xiv][yiv][pi1][ci0] + cf * E_ss_TABLE_G2_ANISO_PHI[xiv][yiv][pi1][ci1];
				}
				vXY[xi][yi] = (1-pf)*vP0 + pf*vP1;
			}
		}
		const Scalar v0 = (1-yf)*vXY[0][0] + yf*vXY[0][1];
		const Scalar v1 = (1-yf)*vXY[1][0] + yf*vXY[1][1];
		const Scalar v = (1-xf)*v0 + xf*v1;
		return belowC0 ? r_max( Scalar(0.0), r_min( Scalar(1.0), v ) ) : v;
	}

	/// DL-161: E_avg_G2_ANISO twin of AnisoPhiCellMixedX -- no phi axis
	/// (Eavg is already azimuth-averaged by construction), so no flip is
	/// needed for the X<->Y relabel: Eavg(alphaX,alphaY) ==
	/// Eavg(alphaY,alphaX) identically.
	inline Scalar AnisoEavgCellMixedX( const bool xLow, const int xIdx, const int yi )
	{
		if( !xLow ) return E_avg_TABLE_G2_ANISO[xIdx][yi];
		const int idx = (xIdx <= 0) ? 1 : xIdx;
		if( idx == ALPHA_SUB_FINE + 1 ) return E_avg_TABLE_G2_ANISO[0][yi];
		if( idx >= ALPHA_LOW_TOTAL ) return E_avg_TABLE_G2_ANISO[1][yi];
		return E_avg_TABLE_G2_ANISO_ALPHALOW_X[ AlphaLowSlot(idx) ][yi];
	}

	/// DL-161: E_avg_G2_ANISO twin of AnisoPhiCellBothLow.
	inline Scalar AnisoEavgCellBothLow( const int xIdx, const int yIdx )
	{
		const int ix = (xIdx <= 0) ? 1 : xIdx;
		const int iy = (yIdx <= 0) ? 1 : yIdx;
		const bool ixBoundary = (ix == ALPHA_SUB_FINE + 1) || (ix >= ALPHA_LOW_TOTAL);
		const bool iyBoundary = (iy == ALPHA_SUB_FINE + 1) || (iy >= ALPHA_LOW_TOTAL);
		if( ixBoundary && iyBoundary )
		{
			const int xo = (ix == ALPHA_SUB_FINE + 1) ? 0 : 1;
			const int yo = (iy == ALPHA_SUB_FINE + 1) ? 0 : 1;
			return E_avg_TABLE_G2_ANISO[xo][yo];
		}
		if( iyBoundary )
		{
			const int yo = (iy == ALPHA_SUB_FINE + 1) ? 0 : 1;
			return E_avg_TABLE_G2_ANISO_ALPHALOW_X[ AlphaLowSlot(ix) ][yo];
		}
		if( ixBoundary )
		{
			const int xo = (ix == ALPHA_SUB_FINE + 1) ? 0 : 1;
			return E_avg_TABLE_G2_ANISO_ALPHALOW_X[ AlphaLowSlot(iy) ][xo];
		}
		return E_avg_TABLE_G2_ANISO_ALPHALOW_XY[ AlphaLowSlot(ix) ][ AlphaLowSlot(iy) ];
	}

	/// DL-161: LookupEavgG2Aniso's low-alpha-X (ordinary-Y) branch.
	inline Scalar LookupEavgG2AnisoLowX( const Scalar aXLow, const Scalar aYOrd )
	{
		int xi0, xi1; Scalar xf;
		AnisoAlphaLowIndex( aXLow, xi0, xi1, xf );
		int yi0, yi1; Scalar yf;
		AnisoAlphaIndex( aYOrd, yi0, yi1, yf );

		const Scalar v00 = AnisoEavgCellMixedX(true, xi0, yi0);
		const Scalar v01 = AnisoEavgCellMixedX(true, xi0, yi1);
		const Scalar v10 = AnisoEavgCellMixedX(true, xi1, yi0);
		const Scalar v11 = AnisoEavgCellMixedX(true, xi1, yi1);
		const Scalar v0 = (1-yf)*v00 + yf*v01;
		const Scalar v1 = (1-yf)*v10 + yf*v11;
		return (1-xf)*v0 + xf*v1;
	}

	/// DL-161: LookupEavgG2Aniso's both-low branch.
	inline Scalar LookupEavgG2AnisoLowXY( const Scalar aXLow, const Scalar aYLow )
	{
		int xi0, xi1; Scalar xf;
		AnisoAlphaLowIndex( aXLow, xi0, xi1, xf );
		int yi0, yi1; Scalar yf;
		AnisoAlphaLowIndex( aYLow, yi0, yi1, yf );

		const Scalar v00 = AnisoEavgCellBothLow(xi0, yi0);
		const Scalar v01 = AnisoEavgCellBothLow(xi0, yi1);
		const Scalar v10 = AnisoEavgCellBothLow(xi1, yi0);
		const Scalar v11 = AnisoEavgCellBothLow(xi1, yi1);
		const Scalar v0 = (1-yf)*v00 + yf*v01;
		const Scalar v1 = (1-yf)*v10 + yf*v11;
		return (1-xf)*v0 + xf*v1;
	}

	/// DL-77: anisotropic twin of LookupEavgG2.
	inline Scalar LookupEavgG2Aniso( const Scalar alphaX, const Scalar alphaY )
	{
		if( fabs(alphaX - alphaY) < 1e-9 ) return LookupEavgG2( alphaX );

		// DL-161: same "check the raw alpha before the ordinary [0.01,1.0]
		// clamp" fix as LookupEssG2AnisoDirectional's own dispatch --
		// see its comment for why clamping first breaks the low-alpha
		// branch (every alpha below 0.01 would alias to exactly 0.01).
		const Scalar rawX = r_max( Scalar(0.0), alphaX );
		const Scalar rawY = r_max( Scalar(0.0), alphaY );
		const bool xLow = rawX < ANISO_ALPHA_LOW_A1;
		const bool yLow = rawY < ANISO_ALPHA_LOW_A1;
		if( xLow && !yLow ) return LookupEavgG2AnisoLowX( rawX, rawY );
		if( yLow && !xLow ) return LookupEavgG2AnisoLowX( rawY, rawX );
		if( xLow && yLow )  return LookupEavgG2AnisoLowXY( rawX, rawY );

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
					MSLobeDetail::Segment segs[ANISO_COS_SIZE + ANISO_SUB_TOTAL];
					int nSegs = 0;
					MSLobeDetail::BuildSegmentsFromRowN( E_ss_TABLE_G2_ANISO[xi][yi], E_ss_TABLE_G2_ANISO_SUB[xi][yi], ANISO_COS_SIZE, segs, nSegs );
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

		// DL-86: the same bilinear blend on the grazing sub-grid.
		Scalar subRow[ANISO_SUB_TOTAL-1];
		for( int k = 0; k < ANISO_SUB_TOTAL - 1; k++ )
		{
			const Scalar s0 = (1-yf)*E_ss_TABLE_G2_ANISO_SUB[xi0][yi0][k] + yf*E_ss_TABLE_G2_ANISO_SUB[xi0][yi1][k];
			const Scalar s1 = (1-yf)*E_ss_TABLE_G2_ANISO_SUB[xi1][yi0][k] + yf*E_ss_TABLE_G2_ANISO_SUB[xi1][yi1][k];
			subRow[k] = (1-xf)*s0 + xf*s1;
		}

		MSLobeDetail::Segment segs[ANISO_COS_SIZE + ANISO_SUB_TOTAL];
		int nSegs = 0;
		MSLobeDetail::BuildSegmentsFromRowN( essRow, subRow, ANISO_COS_SIZE, segs, nSegs );

		Scalar totals[ANISO_COS_SIZE + ANISO_SUB_TOTAL];
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

	// DL-86: grazing sub-grid bake for the two ISOTROPIC tables, plus the
	// separable model's cosTheta->0 boundary constant.  Identical
	// estimator to the main loop above -- only the cosTheta values and
	// the RNG stream differ (rand01_sub, so every table above stays
	// byte-for-byte identical to the pre-DL-86 bake).
	double E_ss_SUB[LUT_SIZE][SUB_SIZE-1];
	double E_ss_SUB_G2[LUT_SIZE][SUB_SIZE-1];
	double E_ss_LIMIT[LUT_SIZE];
	{
		const double c0 = 0.5 / (double)LUT_SIZE;
		for(int ai = 0; ai < LUT_SIZE; ai++) {
			const double alpha = 0.01 + (1.0 - 0.01) * (double)ai / (double)(LUT_SIZE - 1);

			for(int k = 0; k < SUB_SIZE; k++) {
				// k==0 probes the cosTheta->0 boundary (consumed for the
				// SEPARABLE model only -- the G2 model's boundary is
				// exactly 1 analytically); k>=1 is sub-node k, at
				// cosTheta = k*c0/SUB_SIZE.
				const double cosTheta = (k == 0) ? SUB_LIMIT_COS : (double)k * c0 / (double)SUB_SIZE;
				const double sinTheta = sqrt(fmax(0.0, 1.0 - cosTheta * cosTheta));
				const Vec3 wi(sinTheta, 0.0, cosTheta);
				const double G1wi = 1.0 / (1.0 + GGX_Lambda(alpha, cosTheta));

				double sum = 0.0;
				double sumG2 = 0.0;
				for(int s = 0; s < NUM_SAMPLES; s++) {
					const double u1 = rand01_sub();
					const double u2 = rand01_sub();

					const Vec3 m = VNDF_Sample_Local(wi, alpha, u1, u2);
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
						sum += GGX_G1(alpha, cosWo);
						sumG2 += GGX_G2_HeightCorrelated(alpha, cosTheta, cosWo) / G1wi;
					}
				}

				if(k == 0) {
					E_ss_LIMIT[ai] = sum / (double)NUM_SAMPLES;
				} else {
					E_ss_SUB[ai][k-1] = sum / (double)NUM_SAMPLES;
					E_ss_SUB_G2[ai][k-1] = sumG2 / (double)NUM_SAMPLES;
				}
			}

			fprintf(stderr, "DL-86 sub-grid alpha=%.4f  E_ss_LIMIT=%.6f  E_ss_SUB_G2[0]=%.6f\n",
				alpha, E_ss_LIMIT[ai], E_ss_SUB_G2[ai][0]);
		}
	}

	// DL-105: low-alpha sub-grid bake -- ALPHA_LOW_STORED (10) virtual
	// alpha rows below the main table's row 1, covering the alpha<0.01
	// clamp-to-row-0 range AND the coarse row0->row1 (0.01->0.0419)
	// cell.  Each stored row gets its OWN full ordinary cosTheta row
	// (matching E_ss_TABLE/E_ss_TABLE_G2) plus its own DL-86-style
	// grazing sub-row and separable-model limit constant (matching
	// E_ss_SUB_TABLE/E_ss_SUB_TABLE_G2/E_ss_LIMIT_TABLE), so a query
	// combining a low alpha AND a grazing cosTheta (the exact
	// configuration the DL-105 furnace rows probe -- theta=89.4..89.89
	// is already inside the DL-86 grazing sub-grid) reads a value baked
	// AT that alpha, not one blended in from row 0/row 1.  Drawn from
	// rand01_lowalpha, a THIRD independently-seeded stream, so every
	// table above (including the DL-86 sub-grid, already fully baked at
	// this point) stays byte-for-byte identical.
	double E_ss_ALPHA_LOW[ALPHA_LOW_STORED][LUT_SIZE];
	double E_ss_ALPHA_LOW_G2[ALPHA_LOW_STORED][LUT_SIZE];
	double E_ss_ALPHA_LOW_SUB[ALPHA_LOW_STORED][SUB_SIZE-1];
	double E_ss_ALPHA_LOW_SUB_G2[ALPHA_LOW_STORED][SUB_SIZE-1];
	double E_ss_ALPHA_LOW_LIMIT[ALPHA_LOW_STORED];
	double E_avg_ALPHA_LOW[ALPHA_LOW_STORED];
	double E_avg_ALPHA_LOW_G2[ALPHA_LOW_STORED];
	{
		const double A0 = 0.01 + (1.0 - 0.01) * 0.0 / (double)(LUT_SIZE - 1);
		const double A1 = 0.01 + (1.0 - 0.01) * 1.0 / (double)(LUT_SIZE - 1);
		const double c0 = 0.5 / (double)LUT_SIZE;

		auto bakeIdx = [&](int idx) {
			const int slot = alphaLowSlot(idx);
			const double alpha = alphaLowNode(idx, A0, A1);

			// Ordinary row (LUT_SIZE cells) -- identical estimator to
			// the main E_ss/E_ss_G2 bake loop above.
			for(int ci = 0; ci < LUT_SIZE; ci++) {
				const double cosTheta = (double)(ci + 0.5) / LUT_SIZE;
				const double sinTheta = sqrt(fmax(0.0, 1.0 - cosTheta * cosTheta));
				const Vec3 wi(sinTheta, 0.0, cosTheta);
				const double G1wi = 1.0 / (1.0 + GGX_Lambda(alpha, cosTheta));

				double sum = 0.0, sumG2 = 0.0;
				for(int s = 0; s < NUM_SAMPLES; s++) {
					const double u1 = rand01_lowalpha();
					const double u2 = rand01_lowalpha();
					const Vec3 m = VNDF_Sample_Local(wi, alpha, u1, u2);
					const double wiDotM = dot(wi, m);
					if(wiDotM <= 0) continue;
					Vec3 wo(2.0*wiDotM*m.x - wi.x, 2.0*wiDotM*m.y - wi.y, 2.0*wiDotM*m.z - wi.z);
					wo = normalize(wo);
					const double cosWo = wo.z;
					if(cosWo > 0) {
						sum += GGX_G1(alpha, cosWo);
						sumG2 += GGX_G2_HeightCorrelated(alpha, cosTheta, cosWo) / G1wi;
					}
				}
				E_ss_ALPHA_LOW[slot][ci] = sum / (double)NUM_SAMPLES;
				E_ss_ALPHA_LOW_G2[slot][ci] = sumG2 / (double)NUM_SAMPLES;
			}

			// Grazing sub-row + separable limit -- identical estimator
			// to the DL-86 sub-grid bake loop above, at this alpha.
			for(int k = 0; k < SUB_SIZE; k++) {
				const double cosTheta = (k == 0) ? SUB_LIMIT_COS : (double)k * c0 / (double)SUB_SIZE;
				const double sinTheta = sqrt(fmax(0.0, 1.0 - cosTheta * cosTheta));
				const Vec3 wi(sinTheta, 0.0, cosTheta);
				const double G1wi = 1.0 / (1.0 + GGX_Lambda(alpha, cosTheta));

				double sum = 0.0, sumG2 = 0.0;
				for(int s = 0; s < NUM_SAMPLES; s++) {
					const double u1 = rand01_lowalpha();
					const double u2 = rand01_lowalpha();
					const Vec3 m = VNDF_Sample_Local(wi, alpha, u1, u2);
					const double wiDotM = dot(wi, m);
					if(wiDotM <= 0) continue;
					Vec3 wo(2.0*wiDotM*m.x - wi.x, 2.0*wiDotM*m.y - wi.y, 2.0*wiDotM*m.z - wi.z);
					wo = normalize(wo);
					const double cosWo = wo.z;
					if(cosWo > 0) {
						sum += GGX_G1(alpha, cosWo);
						sumG2 += GGX_G2_HeightCorrelated(alpha, cosTheta, cosWo) / G1wi;
					}
				}
				if(k == 0) {
					E_ss_ALPHA_LOW_LIMIT[slot] = sum / (double)NUM_SAMPLES;
				} else {
					E_ss_ALPHA_LOW_SUB[slot][k-1] = sum / (double)NUM_SAMPLES;
					E_ss_ALPHA_LOW_SUB_G2[slot][k-1] = sumG2 / (double)NUM_SAMPLES;
				}
			}

			// E_avg via the SAME midpoint rule over the 32 ordinary
			// bins the main E_avg_TABLE/E_avg_TABLE_G2 bake uses
			// (deliberately NOT including the grazing sub-grid, for
			// consistency with those pre-existing tables).
			double integral = 0.0, integralG2 = 0.0;
			for(int ci = 0; ci < LUT_SIZE; ci++) {
				const double mu = (double)(ci + 0.5) / LUT_SIZE;
				const double dmu = 1.0 / LUT_SIZE;
				integral += E_ss_ALPHA_LOW[slot][ci] * mu * dmu;
				integralG2 += E_ss_ALPHA_LOW_G2[slot][ci] * mu * dmu;
			}
			E_avg_ALPHA_LOW[slot] = 2.0 * integral;
			E_avg_ALPHA_LOW_G2[slot] = 2.0 * integralG2;

			fprintf(stderr, "DL-105 low-alpha idx=%d alpha=%.8f E_avg=%.6f E_avg_G2=%.6f\n",
				idx, alpha, E_avg_ALPHA_LOW[slot], E_avg_ALPHA_LOW_G2[slot]);
		};

		for(int idx = 1; idx <= ALPHA_SUB_FINE; idx++) bakeIdx(idx);
		for(int idx = ALPHA_SUB_FINE + 2; idx < ALPHA_LOW_TOTAL; idx++) bakeIdx(idx);
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
	// DL-86: the aniso twins of E_ss_SUB_G2 above -- the phi-resolved
	// grazing sub-grid (primary bake, consumed by
	// LookupEssG2AnisoDirectional) and its phi-averaged derivative
	// (consumed by LookupEssG2Aniso / the H6 aniso sampler, exactly as
	// E_ss_G2_ANISO is derived from E_ss_G2_ANISO_PHI).  No aniso
	// E_ss_LIMIT twin exists: the height-correlated boundary is exactly 1
	// per-direction, azimuth included (Lambda_Aniso(wi)->infinity as
	// cosWi->0 at EVERY azimuth), so there is nothing to bake.
	//
	// Round 2: ANISO_SUB_TOTAL-1 nodes per row, not SUB_SIZE-1 -- the
	// lowest uniform interval is refined geometrically (see
	// ANISO_SUB_FINE).  The ISOTROPIC sub tables above keep the plain
	// uniform grid and are byte-for-byte unchanged by this.
	static double E_ss_G2_ANISO_PHI_SUB[ANISO_ALPHA_SIZE][ANISO_ALPHA_SIZE][ANISO_PHI_SIZE][ANISO_SUB_TOTAL-1];
	static double E_ss_G2_ANISO_SUB[ANISO_ALPHA_SIZE][ANISO_ALPHA_SIZE][ANISO_SUB_TOTAL-1];

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

	// DL-86 twin of evalIsoEssG2 for the grazing sub-grid: a byte-exact
	// re-implementation of LookupEssG2's OWN below-c0 branch (uniform
	// SUB_SIZE grid, anchor 1 at cosTheta=0, alpha-row blend), evaluated
	// at an arbitrary cosTheta.
	//
	// Round 2 note: this used to take a NODE INDEX k, because the aniso
	// sub-nodes sat at exactly the isotropic ones.  They no longer do --
	// the aniso grid is refined below c0/SUB_SIZE and the isotropic one
	// is not -- so the diagonal seed asks for the isotropic MODEL's value
	// at the aniso node's cosTheta instead.  At the nodes the two grids
	// still share (the uniform ones) this returns exactly what the
	// index-based version returned, since SubNodeIndex lands on kf=0
	// there; at the new geometric nodes it returns the isotropic ramp's
	// own value, which is the point: alphaX==alphaY must keep agreeing
	// with LookupEssG2, whose end-cap this slice deliberately leaves
	// alone (it is accurate to 0.016% there).
	auto evalIsoEssG2SubAt = [&](double alpha, double cosTheta) -> double {
		double a = fmax(0.0, fmin(1.0, (alpha - 0.01) / (1.0 - 0.01))) * (LUT_SIZE - 1);
		int ai0 = (int)a;
		int ai1 = (ai0 + 1 < LUT_SIZE - 1) ? ai0 + 1 : LUT_SIZE - 1;
		double af = a - ai0;

		const double c0iso = 0.5 / (double)LUT_SIZE;
		double t = cosTheta / c0iso * (double)SUB_SIZE;
		int k0 = (int)t;
		if(k0 < 0) k0 = 0;
		if(k0 > SUB_SIZE - 1) k0 = SUB_SIZE - 1;
		const double kf = t - (double)k0;

		auto node = [&](int ai, int k) -> double {
			if(k <= 0) return 1.0;
			if(k >= SUB_SIZE) return E_ss_G2[ai][0];
			return E_ss_SUB_G2[ai][k-1];
		};
		const double s0 = (1-af) * node(ai0, k0)     + af * node(ai1, k0);
		const double s1 = (1-af) * node(ai0, k0 + 1) + af * node(ai1, k0 + 1);
		return (1-kf) * s0 + kf * s1;
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

				// DL-86: the same per-azimuth estimator on the grazing
				// sub-grid (nodes anisoSubNodeCos(n), n=1..
				// ANISO_SUB_TOTAL-1, with c0 = 0.5/ANISO_COS_SIZE --
				// ANISO_SUB_FINE geometric octaves below c0/SUB_SIZE then
				// the uniform k*c0/SUB_SIZE nodes).  Drawn from rand01_sub
				// so the ci loop above stays byte-identical to the
				// pre-DL-86 bake.  The n=0 boundary is exactly 1
				// analytically and the n=ANISO_SUB_TOTAL node IS ci=0, so
				// neither is stored.
				for(int n = 1; n < ANISO_SUB_TOTAL; n++) {
					const double cosTheta = anisoSubNodeCos(n, 0.5 / (double)ANISO_COS_SIZE);

					if(isDiagonal) {
						E_ss_G2_ANISO_PHI_SUB[ix][iy][pi][n-1] = evalIsoEssG2SubAt(alphaX, cosTheta);
						continue;
					}

					const double sinTheta = sqrt(fmax(0.0, 1.0 - cosTheta * cosTheta));
					const Vec3 wi(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta);
					const double G1wi = GGX_G1_Aniso(alphaX, alphaY, wi);

					double sumG2 = 0.0;
					for(int s = 0; s < NUM_SAMPLES_ANISO; s++) {
						const double u1 = rand01_sub();
						const double u2 = rand01_sub();
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

					E_ss_G2_ANISO_PHI_SUB[ix][iy][pi][n-1] = sumG2 / (double)NUM_SAMPLES_ANISO;
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
				// DL-86: the grazing sub-grid mirrors identically -- the
				// relabel symmetry is a property of the direction, not of
				// which cosTheta node it is evaluated at.
				for(int k = 0; k < ANISO_SUB_TOTAL - 1; k++) {
					E_ss_G2_ANISO_PHI_SUB[ix][iy][pi][k] = E_ss_G2_ANISO_PHI_SUB[iy][ix][ANISO_PHI_SIZE - 1 - pi][k];
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

			// DL-86: the grazing sub-grid's phi-average, derived by the
			// identical trapezoidal rule.
			for(int k = 0; k < ANISO_SUB_TOTAL - 1; k++) {
				double trapz = 0.5 * E_ss_G2_ANISO_PHI_SUB[ix][iy][0][k] + 0.5 * E_ss_G2_ANISO_PHI_SUB[ix][iy][ANISO_PHI_SIZE-1][k];
				for(int pi = 1; pi < ANISO_PHI_SIZE - 1; pi++)
					trapz += E_ss_G2_ANISO_PHI_SUB[ix][iy][pi][k];
				E_ss_G2_ANISO_SUB[ix][iy][k] = trapz / (double)(ANISO_PHI_SIZE - 1);
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

	// DL-161: aniso low-alpha sub-grid bake.  Reuses alphaLowNode/
	// alphaLowSlot (both already parameterized by A0,A1 -- no new
	// generator-side node function is needed) re-anchored to the ANISO
	// grid's own row0/row1 (anisoAlphaLowA0=0.01, anisoAlphaLowA1=
	// 0.01+0.99/(ANISO_ALPHA_SIZE-1)=0.053043...), NOT the isotropic
	// axis's A1 (0.041935...) -- the aniso grid's first cell is a 5.3x
	// ratio (isotropic: 4.2x), so the low-alpha MID nodes (between A0
	// and A1) sit at different alpha values.  The below-0.01 geometric
	// octaves (which depend only on A0=0.01, shared with the isotropic
	// axis) are numerically identical to alphaLowNode's isotropic call
	// sites' own octave values -- confirmed by construction, not
	// re-measured.
	//
	// Two families of tables: ALPHALOW_X (low alphaX paired against the
	// FULL ordinary 24-node alphaY grid -- no symmetry between a
	// low-alpha virtual index and an ordinary grid index, so this is a
	// genuine rectangular bake) and ALPHALOW_XY (BOTH axes low -- itself
	// relabel-symmetric like the main DL-77 table, so baked
	// upper-triangle + mirrored).  The "low alphaY, ordinary alphaX" and
	// "boundary-vs-interior" cases are NOT separately baked --
	// LookupEssG2AnisoDirectional's runtime dispatch derives them from
	// ALPHALOW_X via the X<->Y relabel symmetry (phi -> 90-phi) and from
	// the main ordinary table where a low index forwards to A0/A1
	// (idx==ALPHA_SUB_FINE+1 or idx>=ALPHA_LOW_TOTAL) -- see the emitted
	// AnisoPhiCellBothLow's own comment.
	//
	// The H6 multiscatter-lobe SAMPLER (E_ss_G2_ANISO/_SUB,
	// MSLobeZG2Aniso, SampleMSCosThetaG2Aniso, MSPdfG2Aniso,
	// BuildSegmentsFromRowN) is DELIBERATELY NOT extended here -- it
	// keeps reading the pre-existing AnisoAlphaIndex clamp-to-row-0
	// behavior below 0.01, matching the DL-86 precedent that the
	// sampler's own precision is an efficiency concern (MSPdfG2Aniso
	// always reports the density of what SampleMSCosThetaG2Aniso
	// actually draws, so the estimator stays unbiased regardless of the
	// proposal's accuracy), not a correctness one -- only the two
	// ENERGY-COMPENSATION call sites (LookupEssG2AnisoDirectional,
	// LookupEavgG2Aniso) are fixed.  See docs/DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md "DL-161".
	const double anisoAlphaLowA0 = 0.01;
	const double anisoAlphaLowA1 = 0.01 + (1.0 - 0.01) * 1.0 / (double)(ANISO_ALPHA_SIZE - 1);

	static double E_ss_G2_ANISO_PHI_ALPHALOW_X[ALPHA_LOW_STORED][ANISO_ALPHA_SIZE][ANISO_PHI_SIZE][ANISO_COS_SIZE];
	static double E_ss_G2_ANISO_PHI_ALPHALOW_X_SUB[ALPHA_LOW_STORED][ANISO_ALPHA_SIZE][ANISO_PHI_SIZE][ANISO_SUB_TOTAL-1];
	static double E_ss_G2_ANISO_PHI_ALPHALOW_XY[ALPHA_LOW_STORED][ALPHA_LOW_STORED][ANISO_PHI_SIZE][ANISO_COS_SIZE];
	static double E_ss_G2_ANISO_PHI_ALPHALOW_XY_SUB[ALPHA_LOW_STORED][ALPHA_LOW_STORED][ANISO_PHI_SIZE][ANISO_SUB_TOTAL-1];
	static double E_avg_G2_ANISO_ALPHALOW_X[ALPHA_LOW_STORED][ANISO_ALPHA_SIZE];
	static double E_avg_G2_ANISO_ALPHALOW_XY[ALPHA_LOW_STORED][ALPHA_LOW_STORED];

	// -- ALPHALOW_X: low alphaX (ALPHA_LOW_STORED nodes) x ordinary
	// alphaY (ANISO_ALPHA_SIZE nodes), full phi x cosTheta resolution
	// (ordinary bins) plus the grazing sub-grid twin. --
	auto bakeAnisoLowXRow = [&](int lowIdx) {
		const int slot = alphaLowSlot(lowIdx);
		const double alphaX = alphaLowNode(lowIdx, anisoAlphaLowA0, anisoAlphaLowA1);

		for(int iy = 0; iy < ANISO_ALPHA_SIZE; iy++) {
			const double alphaY = 0.01 + (1.0 - 0.01) * (double)iy / (double)(ANISO_ALPHA_SIZE - 1);

			for(int pi = 0; pi < ANISO_PHI_SIZE; pi++) {
				const double phiDeg = (double)pi * 90.0 / (double)(ANISO_PHI_SIZE - 1);
				const double phi = phiDeg * PI / 180.0;
				const double cosPhi = cos(phi);
				const double sinPhi = sin(phi);

				for(int ci = 0; ci < ANISO_COS_SIZE; ci++) {
					const double cosTheta = ((double)ci + 0.5) / ANISO_COS_SIZE;
					const double sinTheta = sqrt(fmax(0.0, 1.0 - cosTheta * cosTheta));
					const Vec3 wi(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta);
					const double G1wi = GGX_G1_Aniso(alphaX, alphaY, wi);

					double sumG2 = 0.0;
					for(int s = 0; s < NUM_SAMPLES_ANISO; s++) {
						const double u1 = rand01_anisolow();
						const double u2 = rand01_anisolow();
						const Vec3 m = VNDF_Sample_Local_Aniso(wi, alphaX, alphaY, u1, u2);
						const double wiDotM = dot(wi, m);
						if(wiDotM <= 0) continue;
						Vec3 wo(2.0*wiDotM*m.x - wi.x, 2.0*wiDotM*m.y - wi.y, 2.0*wiDotM*m.z - wi.z);
						wo = normalize(wo);
						const double cosWo = wo.z;
						if(cosWo > 0) {
							if(G1wi > 1e-12)
								sumG2 += GGX_G2_Aniso_HeightCorrelated(alphaX, alphaY, wi, wo) / G1wi;
						}
					}
					E_ss_G2_ANISO_PHI_ALPHALOW_X[slot][iy][pi][ci] = sumG2 / (double)NUM_SAMPLES_ANISO;
				}

				for(int n = 1; n < ANISO_SUB_TOTAL; n++) {
					const double cosTheta = anisoSubNodeCos(n, 0.5 / (double)ANISO_COS_SIZE);
					const double sinTheta = sqrt(fmax(0.0, 1.0 - cosTheta * cosTheta));
					const Vec3 wi(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta);
					const double G1wi = GGX_G1_Aniso(alphaX, alphaY, wi);

					double sumG2 = 0.0;
					for(int s = 0; s < NUM_SAMPLES_ANISO; s++) {
						const double u1 = rand01_anisolow();
						const double u2 = rand01_anisolow();
						const Vec3 m = VNDF_Sample_Local_Aniso(wi, alphaX, alphaY, u1, u2);
						const double wiDotM = dot(wi, m);
						if(wiDotM <= 0) continue;
						Vec3 wo(2.0*wiDotM*m.x - wi.x, 2.0*wiDotM*m.y - wi.y, 2.0*wiDotM*m.z - wi.z);
						wo = normalize(wo);
						const double cosWo = wo.z;
						if(cosWo > 0) {
							if(G1wi > 1e-12)
								sumG2 += GGX_G2_Aniso_HeightCorrelated(alphaX, alphaY, wi, wo) / G1wi;
						}
					}
					E_ss_G2_ANISO_PHI_ALPHALOW_X_SUB[slot][iy][pi][n-1] = sumG2 / (double)NUM_SAMPLES_ANISO;
				}
			}

			// E_avg via phi-trapezoidal average (endpoint-half-weight,
			// matching the main table's own derivation) THEN the
			// midpoint-rule cosTheta integral over the ordinary bins --
			// identical two-step derivation to E_avg_G2_ANISO's own,
			// just sourced from this row's freshly-baked PHI table
			// instead of the main one.  The phi average is an EPHEMERAL
			// scratch quantity here (not separately stored in the
			// header): only the final per-(alphaX,alphaY) E_avg needs a
			// table entry, matching DL-77's economy for the main table.
			double integral = 0.0;
			for(int ci = 0; ci < ANISO_COS_SIZE; ci++) {
				double trapz = 0.5 * E_ss_G2_ANISO_PHI_ALPHALOW_X[slot][iy][0][ci] + 0.5 * E_ss_G2_ANISO_PHI_ALPHALOW_X[slot][iy][ANISO_PHI_SIZE-1][ci];
				for(int pi = 1; pi < ANISO_PHI_SIZE - 1; pi++)
					trapz += E_ss_G2_ANISO_PHI_ALPHALOW_X[slot][iy][pi][ci];
				const double essAvgPhi = trapz / (double)(ANISO_PHI_SIZE - 1);
				const double mu = ((double)ci + 0.5) / ANISO_COS_SIZE;
				const double dmu = 1.0 / ANISO_COS_SIZE;
				integral += essAvgPhi * mu * dmu;
			}
			E_avg_G2_ANISO_ALPHALOW_X[slot][iy] = 2.0 * integral;
		}

		fprintf(stderr, "DL-161 aniso low-X idx=%d alphaX=%.8f  (canonical bake)\n", lowIdx, alphaX);
	};
	for(int idx = 1; idx <= ALPHA_SUB_FINE; idx++) bakeAnisoLowXRow(idx);
	for(int idx = ALPHA_SUB_FINE + 2; idx < ALPHA_LOW_TOTAL; idx++) bakeAnisoLowXRow(idx);

	// -- ALPHALOW_XY: BOTH axes low (ALPHA_LOW_STORED x ALPHA_LOW_STORED)
	// -- relabel-symmetric like the main DL-77 table, so only the
	// canonical ix<=iy half is Monte-Carlo baked; the ix>iy half is
	// filled by MIRRORING (phi -> 90-phi), zero extra noise, exactly
	// the main table's own Pass-1/Pass-2 construction. --
	for(int ixRaw = 0; ixRaw < ALPHA_LOW_STORED; ixRaw++) {
		const int ix = (ixRaw < ALPHA_SUB_FINE) ? (ixRaw + 1) : (ixRaw + 2);	// virtual idx skipping the unstored A0/A1 slots
		const double alphaX = alphaLowNode(ix, anisoAlphaLowA0, anisoAlphaLowA1);

		for(int iyRaw = ixRaw; iyRaw < ALPHA_LOW_STORED; iyRaw++) {
			const int iy = (iyRaw < ALPHA_SUB_FINE) ? (iyRaw + 1) : (iyRaw + 2);
			const double alphaY = alphaLowNode(iy, anisoAlphaLowA0, anisoAlphaLowA1);
			const bool isDiagonal = (ixRaw == iyRaw);

			for(int pi = 0; pi < ANISO_PHI_SIZE; pi++) {
				const double phiDeg = (double)pi * 90.0 / (double)(ANISO_PHI_SIZE - 1);
				const double phi = phiDeg * PI / 180.0;
				const double cosPhi = cos(phi);
				const double sinPhi = sin(phi);

				for(int ci = 0; ci < ANISO_COS_SIZE; ci++) {
					const double cosTheta = ((double)ci + 0.5) / ANISO_COS_SIZE;

					if(isDiagonal) {
						E_ss_G2_ANISO_PHI_ALPHALOW_XY[ixRaw][iyRaw][pi][ci] = evalIsoEssG2(alphaX, cosTheta);
						continue;
					}

					const double sinTheta = sqrt(fmax(0.0, 1.0 - cosTheta * cosTheta));
					const Vec3 wi(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta);
					const double G1wi = GGX_G1_Aniso(alphaX, alphaY, wi);

					double sumG2 = 0.0;
					for(int s = 0; s < NUM_SAMPLES_ANISO; s++) {
						const double u1 = rand01_anisolow();
						const double u2 = rand01_anisolow();
						const Vec3 m = VNDF_Sample_Local_Aniso(wi, alphaX, alphaY, u1, u2);
						const double wiDotM = dot(wi, m);
						if(wiDotM <= 0) continue;
						Vec3 wo(2.0*wiDotM*m.x - wi.x, 2.0*wiDotM*m.y - wi.y, 2.0*wiDotM*m.z - wi.z);
						wo = normalize(wo);
						const double cosWo = wo.z;
						if(cosWo > 0) {
							if(G1wi > 1e-12)
								sumG2 += GGX_G2_Aniso_HeightCorrelated(alphaX, alphaY, wi, wo) / G1wi;
						}
					}
					E_ss_G2_ANISO_PHI_ALPHALOW_XY[ixRaw][iyRaw][pi][ci] = sumG2 / (double)NUM_SAMPLES_ANISO;
				}

				for(int n = 1; n < ANISO_SUB_TOTAL; n++) {
					const double cosTheta = anisoSubNodeCos(n, 0.5 / (double)ANISO_COS_SIZE);

					if(isDiagonal) {
						E_ss_G2_ANISO_PHI_ALPHALOW_XY_SUB[ixRaw][iyRaw][pi][n-1] = evalIsoEssG2SubAt(alphaX, cosTheta);
						continue;
					}

					const double sinTheta = sqrt(fmax(0.0, 1.0 - cosTheta * cosTheta));
					const Vec3 wi(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta);
					const double G1wi = GGX_G1_Aniso(alphaX, alphaY, wi);

					double sumG2 = 0.0;
					for(int s = 0; s < NUM_SAMPLES_ANISO; s++) {
						const double u1 = rand01_anisolow();
						const double u2 = rand01_anisolow();
						const Vec3 m = VNDF_Sample_Local_Aniso(wi, alphaX, alphaY, u1, u2);
						const double wiDotM = dot(wi, m);
						if(wiDotM <= 0) continue;
						Vec3 wo(2.0*wiDotM*m.x - wi.x, 2.0*wiDotM*m.y - wi.y, 2.0*wiDotM*m.z - wi.z);
						wo = normalize(wo);
						const double cosWo = wo.z;
						if(cosWo > 0) {
							if(G1wi > 1e-12)
								sumG2 += GGX_G2_Aniso_HeightCorrelated(alphaX, alphaY, wi, wo) / G1wi;
						}
					}
					E_ss_G2_ANISO_PHI_ALPHALOW_XY_SUB[ixRaw][iyRaw][pi][n-1] = sumG2 / (double)NUM_SAMPLES_ANISO;
				}
			}

			fprintf(stderr, "DL-161 aniso low-XY ix=%d iy=%d alphaX=%.6f alphaY=%.6f  (canonical bake)%s\n",
				ixRaw, iyRaw, alphaX, alphaY, isDiagonal ? "  [diag: iso-seeded]" : "");
		}
	}
	// Mirror the ixRaw>iyRaw half.
	for(int ixRaw = 1; ixRaw < ALPHA_LOW_STORED; ixRaw++) {
		for(int iyRaw = 0; iyRaw < ixRaw; iyRaw++) {
			for(int pi = 0; pi < ANISO_PHI_SIZE; pi++) {
				for(int ci = 0; ci < ANISO_COS_SIZE; ci++) {
					E_ss_G2_ANISO_PHI_ALPHALOW_XY[ixRaw][iyRaw][pi][ci] = E_ss_G2_ANISO_PHI_ALPHALOW_XY[iyRaw][ixRaw][ANISO_PHI_SIZE - 1 - pi][ci];
				}
				for(int k = 0; k < ANISO_SUB_TOTAL - 1; k++) {
					E_ss_G2_ANISO_PHI_ALPHALOW_XY_SUB[ixRaw][iyRaw][pi][k] = E_ss_G2_ANISO_PHI_ALPHALOW_XY_SUB[iyRaw][ixRaw][ANISO_PHI_SIZE - 1 - pi][k];
				}
			}
		}
	}
	// E_avg_G2_ANISO_ALPHALOW_XY: phi-trapezoidal average (ephemeral)
	// then cosTheta midpoint-rule integral, over the FULL (mirrored)
	// square -- comes out symmetric automatically, same argument as the
	// main E_avg_G2_ANISO's own derivation.
	for(int ixRaw = 0; ixRaw < ALPHA_LOW_STORED; ixRaw++) {
		for(int iyRaw = 0; iyRaw < ALPHA_LOW_STORED; iyRaw++) {
			double integral = 0.0;
			for(int ci = 0; ci < ANISO_COS_SIZE; ci++) {
				double trapz = 0.5 * E_ss_G2_ANISO_PHI_ALPHALOW_XY[ixRaw][iyRaw][0][ci] + 0.5 * E_ss_G2_ANISO_PHI_ALPHALOW_XY[ixRaw][iyRaw][ANISO_PHI_SIZE-1][ci];
				for(int pi = 1; pi < ANISO_PHI_SIZE - 1; pi++)
					trapz += E_ss_G2_ANISO_PHI_ALPHALOW_XY[ixRaw][iyRaw][pi][ci];
				const double essAvgPhi = trapz / (double)(ANISO_PHI_SIZE - 1);
				const double mu = ((double)ci + 0.5) / ANISO_COS_SIZE;
				const double dmu = 1.0 / ANISO_COS_SIZE;
				integral += essAvgPhi * mu * dmu;
			}
			E_avg_G2_ANISO_ALPHALOW_XY[ixRaw][iyRaw] = 2.0 * integral;
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
	printf("//  DL-86 grazing sub-grid: %d uniform sub-intervals on\n", SUB_SIZE);
	printf("//  [0, 0.5/%d], i.e. %d stored nodes per row, for the isotropic\n", LUT_SIZE, SUB_SIZE - 1);
	printf("//  E_ss/E_ss_G2 tables; the DL-77 aniso tables refine the LOWEST\n");
	printf("//  of those intervals by %d further geometric octaves (down to\n", ANISO_SUB_FINE);
	printf("//  0.5/%d/%d/%d), i.e. %d stored nodes per aniso row -- see\n", LUT_SIZE, SUB_SIZE, 1 << ANISO_SUB_FINE, ANISO_SUB_TOTAL - 1);
	printf("//  ANISO_SUB_FINE.  The cosTheta->0 boundary is exactly 1 for the\n");
	printf("//  height-correlated G2 model and a baked per-alpha constant\n");
	printf("//  (E_ss_LIMIT_TABLE, probed at cosTheta=%g) for the separable model\n", SUB_LIMIT_COS);
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
	printf("//  afterward).  The DL-86 grazing sub-grid tables (E_ss_SUB_TABLE,\n");
	printf("//  E_ss_SUB_TABLE_G2, E_ss_LIMIT_TABLE, E_ss_TABLE_G2_ANISO_PHI_SUB,\n");
	printf("//  E_ss_TABLE_G2_ANISO_SUB) are drawn from a SECOND, independently\n");
	printf("//  seeded stream (rng_state_sub, %lluULL) at the same per-entry\n", (unsigned long long)9876543210987654321ULL);
	printf("//  sample counts, specifically so that adding them left every\n");
	printf("//  pre-existing table byte-for-byte unchanged.  Regenerate + verify with:\n");
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

	printf("\t/// DL-86: number of uniform sub-intervals the grazing interval\n");
	printf("\t/// [0, c0] is resolved at, where c0 = 0.5/N is the first ordinary\n");
	printf("\t/// cosTheta bin center of an N-bin table (N = LUT_SIZE for the\n");
	printf("\t/// isotropic tables, ANISO_COS_SIZE for the DL-77 aniso ones;\n");
	printf("\t/// both are %d, so c0 = %g -- every incidence beyond ~89.1 degrees).\n", LUT_SIZE, 0.5 / (double)LUT_SIZE);
	printf("\t/// Every lookup used to FLAT-CLAMP that whole interval to the\n");
	printf("\t/// bin-0 value; E_ss is not even monotone there at low roughness\n");
	printf("\t/// (at alpha=0.01 an independent 20M-sample VNDF quadrature reads\n");
	printf("\t/// 0.8920 at cosTheta=0.010, 0.9796 at 0.001 and the exact\n");
	printf("\t/// boundary value at 0, while bin 0 reads 0.8993), so neither a\n");
	printf("\t/// flat cap nor a single straight line to the boundary can follow\n");
	printf("\t/// it -- the interval is BAKED instead.  %d nodes are stored per\n", SUB_SIZE - 1);
	printf("\t/// row (k*c0/SUB_SIZE for k=1..SUB_SIZE-1); k=SUB_SIZE IS bin 0\n");
	printf("\t/// itself (so the model is continuous with the ordinary bilinear\n");
	printf("\t/// interior at c0 by construction) and k=0 is the exact\n");
	printf("\t/// cosTheta->0 boundary.  See the generator's SUB_SIZE comment for\n");
	printf("\t/// the residual measurements that picked this value.\n");
	printf("\tstatic const int SUB_SIZE = %d;\n\n", SUB_SIZE);

	printf("\t/// DL-86 round 2: the DL-77 ANISOTROPIC sub-grid refines the\n");
	printf("\t/// LOWEST uniform sub-interval [0, c0/SUB_SIZE] by this many\n");
	printf("\t/// GEOMETRIC octaves (nodes at h*2^-j for j=1..ANISO_SUB_FINE,\n");
	printf("\t/// h = c0/SUB_SIZE), so the straight ramp from the exact\n");
	printf("\t/// cosTheta->0 anchor spans only [0, h*2^-%d] = [0, %.3g].\n", ANISO_SUB_FINE, (0.5/(double)LUT_SIZE)/(double)SUB_SIZE/(double)(1 << ANISO_SUB_FINE));
	printf("\t///\n");
	printf("\t/// The uniform grid suffices for the ISOTROPIC tables (their\n");
	printf("\t/// approach to Ess=1 is already linear by the first sub-node:\n");
	printf("\t/// 0.016%% residual at alpha=0.01, cos=1e-4, measured).  For an\n");
	printf("\t/// ANISOTROPIC pair it does not: the approach to 1 needs\n");
	printf("\t/// Lambda(wi) >> Lambda(wo), i.e. cos << alphaX*sinTheta for a wi\n");
	printf("\t/// aligned with the SMALL axis, while the scattered lobe spread\n");
	printf("\t/// over the LARGE axis keeps Lambda(wo) large.  At\n");
	printf("\t/// (alphaX=0.01, alphaY=1.0, phi=0) the true curve is still at\n");
	printf("\t/// 0.735 at the first uniform sub-node, so a ramp from 1 across\n");
	printf("\t/// that one interval read +2.5%% at cos=1e-4 and +5.9%% at 1e-3.\n");
	printf("\t/// See the generator's ANISO_SUB_FINE comment for the residual\n");
	printf("\t/// measurements that picked this value and for why the\n");
	printf("\t/// interpolation stays LINEAR IN COS on every interval.\n");
	printf("\tstatic const int ANISO_SUB_FINE = %d;\n\n", ANISO_SUB_FINE);

	printf("\t/// DL-86 round 2: total number of aniso sub-grid NODE INDICES on\n");
	printf("\t/// [0, c0].  Index 0 is the exact cosTheta->0 anchor, 1..\n");
	printf("\t/// ANISO_SUB_FINE are the geometric octaves, ANISO_SUB_FINE+1..\n");
	printf("\t/// ANISO_SUB_TOTAL are the uniform nodes k*c0/SUB_SIZE\n");
	printf("\t/// (k=1..SUB_SIZE), and the last of those IS bin 0 -- so\n");
	printf("\t/// ANISO_SUB_TOTAL-1 = %d values are stored per row.\n", ANISO_SUB_TOTAL - 1);
	printf("\tstatic const int ANISO_SUB_TOTAL = SUB_SIZE + ANISO_SUB_FINE;\n\n");

	printf("\t/// DL-105: the ALPHA axis is coarse at its low end in a way the\n");
	printf("\t/// DL-86 cosTheta sub-grid does not touch -- alpha<0.01 clamps to\n");
	printf("\t/// row 0 outright (GGXBRDF.cpp only floors authored roughness at\n");
	printf("\t/// 1e-4), and row 0 (alpha=0.01) to row 1 (alpha=%.4f) is a\n", 0.01 + 0.99/(double)(LUT_SIZE-1));
	printf("\t/// %.1fx ratio inside ONE linear interpolation cell.  Same\n", (0.01 + 0.99/(double)(LUT_SIZE-1)) / 0.01);
	printf("\t/// construction as DL-86's cosTheta end-cap: a baked sub-grid, not\n");
	printf("\t/// an extrapolation, anchored at a PROVABLE exact boundary --\n");
	printf("\t/// alpha->0 (a perfectly smooth surface) makes Smith\n");
	printf("\t/// Lambda(v)->0 for ANY fixed cosTheta>0, so G1->1 and G2->1\n");
	printf("\t/// unconditionally: Ess(alpha->0,cosTheta)=1 for BOTH models (no\n");
	printf("\t/// baked limit constant needed here, unlike DL-86's cosTheta->0\n");
	printf("\t/// case).  %d geometric octaves resolve (0, 0.01) (node j stores\n", ALPHA_SUB_FINE);
	printf("\t/// alpha=0.01/2^(8-j)); %d geometric nodes resolve (0.01, %.4f]\n", ALPHA_MID_SIZE - 1, 0.01 + 0.99/(double)(LUT_SIZE-1));
	printf("\t/// bridging the coarse first cell.  See the generator's\n");
	printf("\t/// ALPHA_SUB_FINE/ALPHA_MID_SIZE comment for the derivation.\n");
	printf("\t/// Interpolation is LINEAR IN ALPHA on every interval (node\n");
	printf("\t/// PLACEMENT is geometric, the BLEND between two adjacent nodes\n");
	printf("\t/// is not -- matching ANISO_SUB_FINE's precedent).\n");
	printf("\tstatic const int ALPHA_SUB_FINE = %d;\n", ALPHA_SUB_FINE);
	printf("\tstatic const int ALPHA_MID_SIZE = %d;\n\n", ALPHA_MID_SIZE);

	printf("\t/// DL-105: total virtual node-index range on [0, A1] (A1 = row\n");
	printf("\t/// 1's alpha).  Index 0 is the exact alpha->0 boundary (not\n");
	printf("\t/// stored), 1..ALPHA_SUB_FINE are the geometric sub-nodes below\n");
	printf("\t/// A0=0.01 (stored), ALPHA_SUB_FINE+1 is A0 itself (row 0, not\n");
	printf("\t/// re-baked), ALPHA_SUB_FINE+2..ALPHA_LOW_TOTAL-1 are the\n");
	printf("\t/// geometric mid-nodes between A0 and A1 (stored), and\n");
	printf("\t/// ALPHA_LOW_TOTAL is A1 itself (row 1, not re-baked).\n");
	printf("\tstatic const int ALPHA_LOW_TOTAL = ALPHA_SUB_FINE + ALPHA_MID_SIZE;\n");
	printf("\t/// %d values (ALPHA_SUB_FINE + (ALPHA_MID_SIZE-1)) are stored.\n", ALPHA_LOW_STORED);
	printf("\tstatic const int ALPHA_LOW_STORED = ALPHA_SUB_FINE + (ALPHA_MID_SIZE - 1);\n\n");

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

	// DL-86: grazing sub-grid twin of E_ss_TABLE, plus the separable
	// model's cosTheta->0 boundary constant.
	printf("\t/// DL-86: grazing sub-grid twin of E_ss_TABLE above -- the SAME\n");
	printf("\t/// separable-model directional albedo, sampled at the SUB_SIZE-1\n");
	printf("\t/// interior nodes of [0, c0] (cosTheta = k*c0/SUB_SIZE for\n");
	printf("\t/// k=1..SUB_SIZE-1, c0 = 0.5/LUT_SIZE).  Indexed as\n");
	printf("\t/// E_ss_SUB_TABLE[alphaIdx][k-1].  Consumed by LookupEss and by\n");
	printf("\t/// MSLobeDetail::BuildSegmentsFromRow -- which MUST agree, since\n");
	printf("\t/// MSPdf evaluates LookupEss directly while SampleMSCosTheta\n");
	printf("\t/// inverts those segments (see the H6 header comment); sharing\n");
	printf("\t/// one baked table is what makes them agree by construction\n");
	printf("\t/// rather than by two matching formulas.\n");
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_ss_SUB_TABLE[%d][%d] = {\n", LUT_SIZE, SUB_SIZE - 1);
	for(int ai = 0; ai < LUT_SIZE; ai++) {
		printf("\t\t{ ");
		for(int k = 0; k < SUB_SIZE - 1; k++) {
			printf("%.8f", E_ss_SUB[ai][k]);
			if(k < SUB_SIZE - 2) printf(", ");
		}
		printf(" }");
		if(ai < LUT_SIZE - 1) printf(",");
		printf("\n");
	}
	printf("\t};\n\n");

	printf("\t/// DL-86: the SEPARABLE model's limiting cosTheta->0 directional\n");
	printf("\t/// albedo, per alpha row.  Unlike the height-correlated G2 model\n");
	printf("\t/// (whose limit is provably exactly 1 -- see LookupEssG2's own\n");
	printf("\t/// comment), the separable model's per-sample VNDF weight is\n");
	printf("\t/// G1(wo), which has NO cancellation against the sampling pdf's\n");
	printf("\t/// G1(wi), so its limit is a finite alpha-dependent constant with\n");
	printf("\t/// no closed form here.  It is therefore BAKED, by evaluating the\n");
	printf("\t/// same estimator at cosTheta=%g (converged: an independent\n", SUB_LIMIT_COS);
	printf("\t/// quadrature reads 0.93436251 at 1e-6 vs 0.93436415 at 1e-7 for\n");
	printf("\t/// alpha=0.05).  This is the k=0 node of the sub-grid above.\n");
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_ss_LIMIT_TABLE[%d] = {\n\t\t", LUT_SIZE);
	for(int ai = 0; ai < LUT_SIZE; ai++) {
		printf("%.8f", E_ss_LIMIT[ai]);
		if(ai < LUT_SIZE - 1) printf(", ");
		if((ai + 1) % 8 == 0 && ai < LUT_SIZE - 1) printf("\n\t\t");
	}
	printf("\n\t};\n\n");

	// DL-105: low-alpha sub-grid tables.  Indexed by the STORED slot
	// (0..ALPHA_LOW_STORED-1, via AlphaLowSlot), not the virtual node
	// index -- slots 0..ALPHA_SUB_FINE-1 are the geometric sub-nodes
	// below A0=0.01, slots ALPHA_SUB_FINE..ALPHA_LOW_STORED-1 are the
	// geometric mid-nodes between A0 and A1 (row 1's alpha).
	printf("\t/// DL-105: low-alpha twin of E_ss_TABLE above, resolved at the\n");
	printf("\t/// ALPHA_LOW_STORED geometric alpha nodes below/between the main\n");
	printf("\t/// table's row 0 and row 1 (see ALPHA_SUB_FINE/ALPHA_MID_SIZE).\n");
	printf("\t/// Indexed as E_ss_ALPHA_LOW_TABLE[AlphaLowSlot(idx)][cosThetaIdx].\n");
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_ss_ALPHA_LOW_TABLE[%d][%d] = {\n", ALPHA_LOW_STORED, LUT_SIZE);
	for(int s = 0; s < ALPHA_LOW_STORED; s++) {
		printf("\t\t{ ");
		for(int ci = 0; ci < LUT_SIZE; ci++) {
			printf("%.8f", E_ss_ALPHA_LOW[s][ci]);
			if(ci < LUT_SIZE - 1) printf(", ");
		}
		printf(" }");
		if(s < ALPHA_LOW_STORED - 1) printf(",");
		printf("\n");
	}
	printf("\t};\n\n");

	printf("\t/// DL-105: low-alpha twin of E_avg_TABLE above (SAME midpoint-\n");
	printf("\t/// rule discretization over the 32 ordinary cosTheta bins).\n");
	printf("\tinline const Scalar E_avg_ALPHA_LOW_TABLE[%d] = {\n\t\t", ALPHA_LOW_STORED);
	for(int s = 0; s < ALPHA_LOW_STORED; s++) {
		printf("%.8f", E_avg_ALPHA_LOW[s]);
		if(s < ALPHA_LOW_STORED - 1) printf(", ");
	}
	printf("\n\t};\n\n");

	printf("\t/// DL-105: low-alpha twin of E_ss_SUB_TABLE above (the DL-86\n");
	printf("\t/// grazing sub-grid, baked AT each low-alpha node instead of\n");
	printf("\t/// blended in from row 0/row 1).\n");
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_ss_ALPHA_LOW_SUB_TABLE[%d][%d] = {\n", ALPHA_LOW_STORED, SUB_SIZE - 1);
	for(int s = 0; s < ALPHA_LOW_STORED; s++) {
		printf("\t\t{ ");
		for(int k = 0; k < SUB_SIZE - 1; k++) {
			printf("%.8f", E_ss_ALPHA_LOW_SUB[s][k]);
			if(k < SUB_SIZE - 2) printf(", ");
		}
		printf(" }");
		if(s < ALPHA_LOW_STORED - 1) printf(",");
		printf("\n");
	}
	printf("\t};\n\n");

	printf("\t/// DL-105: low-alpha twin of E_ss_LIMIT_TABLE above (the\n");
	printf("\t/// SEPARABLE model's cosTheta->0 boundary, baked at this alpha --\n");
	printf("\t/// the G2 model's boundary is exactly 1 at every alpha, so no G2\n");
	printf("\t/// twin of this table exists, same as E_ss_LIMIT_TABLE itself).\n");
	printf("\tinline const Scalar E_ss_ALPHA_LOW_LIMIT_TABLE[%d] = {\n\t\t", ALPHA_LOW_STORED);
	for(int s = 0; s < ALPHA_LOW_STORED; s++) {
		printf("%.8f", E_ss_ALPHA_LOW_LIMIT[s]);
		if(s < ALPHA_LOW_STORED - 1) printf(", ");
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

	printf("\t/// DL-86: height-correlated-G2 twin of E_ss_SUB_TABLE above.\n");
	printf("\t/// No E_ss_LIMIT_TABLE twin exists for this model: its\n");
	printf("\t/// cosTheta->0 boundary is exactly 1 analytically.\n");
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_ss_SUB_TABLE_G2[%d][%d] = {\n", LUT_SIZE, SUB_SIZE - 1);
	for(int ai = 0; ai < LUT_SIZE; ai++) {
		printf("\t\t{ ");
		for(int k = 0; k < SUB_SIZE - 1; k++) {
			printf("%.8f", E_ss_SUB_G2[ai][k]);
			if(k < SUB_SIZE - 2) printf(", ");
		}
		printf(" }");
		if(ai < LUT_SIZE - 1) printf(",");
		printf("\n");
	}
	printf("\t};\n\n");

	// DL-105: G2 twins of the low-alpha tables above.
	printf("\t/// DL-105: height-correlated-G2 twin of E_ss_ALPHA_LOW_TABLE above.\n");
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_ss_ALPHA_LOW_TABLE_G2[%d][%d] = {\n", ALPHA_LOW_STORED, LUT_SIZE);
	for(int s = 0; s < ALPHA_LOW_STORED; s++) {
		printf("\t\t{ ");
		for(int ci = 0; ci < LUT_SIZE; ci++) {
			printf("%.8f", E_ss_ALPHA_LOW_G2[s][ci]);
			if(ci < LUT_SIZE - 1) printf(", ");
		}
		printf(" }");
		if(s < ALPHA_LOW_STORED - 1) printf(",");
		printf("\n");
	}
	printf("\t};\n\n");

	printf("\t/// DL-105: height-correlated-G2 twin of E_avg_ALPHA_LOW_TABLE above.\n");
	printf("\tinline const Scalar E_avg_ALPHA_LOW_TABLE_G2[%d] = {\n\t\t", ALPHA_LOW_STORED);
	for(int s = 0; s < ALPHA_LOW_STORED; s++) {
		printf("%.8f", E_avg_ALPHA_LOW_G2[s]);
		if(s < ALPHA_LOW_STORED - 1) printf(", ");
	}
	printf("\n\t};\n\n");

	printf("\t/// DL-105: height-correlated-G2 twin of E_ss_ALPHA_LOW_SUB_TABLE\n");
	printf("\t/// above.  No G2 twin of E_ss_ALPHA_LOW_LIMIT_TABLE exists: this\n");
	printf("\t/// model's cosTheta->0 boundary is exactly 1 at every alpha.\n");
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_ss_ALPHA_LOW_SUB_TABLE_G2[%d][%d] = {\n", ALPHA_LOW_STORED, SUB_SIZE - 1);
	for(int s = 0; s < ALPHA_LOW_STORED; s++) {
		printf("\t\t{ ");
		for(int k = 0; k < SUB_SIZE - 1; k++) {
			printf("%.8f", E_ss_ALPHA_LOW_SUB_G2[s][k]);
			if(k < SUB_SIZE - 2) printf(", ");
		}
		printf(" }");
		if(s < ALPHA_LOW_STORED - 1) printf(",");
		printf("\n");
	}
	printf("\t};\n\n");

	// Emit lookup functions
	printf("\t/// DL-86: value of the grazing sub-grid model at node k of alpha\n");
	printf("\t/// row `ai`, for k in [0, SUB_SIZE].  k==0 is the cosTheta->0\n");
	printf("\t/// boundary (exactly 1 for the G2 model, a baked per-alpha\n");
	printf("\t/// constant for the separable one), k==SUB_SIZE is the first bin\n");
	printf("\t/// center c0 itself.  Every below-c0 consumer -- LookupEss,\n");
	printf("\t/// LookupEssG2 and MSLobeDetail::BuildSegmentsFromRow -- reads the\n");
	printf("\t/// model through these two accessors, so the lookup and the\n");
	printf("\t/// sampler's segments describe the SAME curve by construction.\n");
	printf("\t///\n");
	printf("\t/// Every node value is a baked directional albedo in [0,1] (and\n");
	printf("\t/// the k==0 G2 boundary is exactly 1), so a convex combination of\n");
	printf("\t/// them is automatically in [0,1]: LookupEss/LookupEssG2's\n");
	printf("\t/// defensive r_max/r_min around the blend below cannot fire, and\n");
	printf("\t/// BuildSegmentsFromRow's un-clamped segment form is therefore\n");
	printf("\t/// describing the identical function, not a laxer one.\n");
	printf("\tinline Scalar SubNodeEss( const int ai, const int k )\n");
	printf("\t{\n");
	printf("\t\tif( k <= 0 ) return E_ss_LIMIT_TABLE[ai];\n");
	printf("\t\tif( k >= SUB_SIZE ) return E_ss_TABLE[ai][0];\n");
	printf("\t\treturn E_ss_SUB_TABLE[ai][k-1];\n");
	printf("\t}\n\n");

	printf("\t/// DL-86: height-correlated-G2 twin of SubNodeEss above.  The\n");
	printf("\t/// k==0 boundary is the PROVABLE exact value 1: under the\n");
	printf("\t/// height-correlated Smith model the per-sample VNDF weight is\n");
	printf("\t/// G2(wi,wo)/G1(wi) = (1+Lambda(wi)) / (1+Lambda(wi)+Lambda(wo)),\n");
	printf("\t/// and Lambda(wi) ~ alpha/(2*cosWi) -> infinity as cosWi -> 0, so\n");
	printf("\t/// that ratio -> 1 for ANY finite Lambda(wo) -- i.e. every\n");
	printf("\t/// VNDF-sampled wo, however distributed, contributes weight -> 1.\n");
	printf("\tinline Scalar SubNodeEssG2( const int ai, const int k )\n");
	printf("\t{\n");
	printf("\t\tif( k <= 0 ) return Scalar(1.0);\n");
	printf("\t\tif( k >= SUB_SIZE ) return E_ss_TABLE_G2[ai][0];\n");
	printf("\t\treturn E_ss_SUB_TABLE_G2[ai][k-1];\n");
	printf("\t}\n\n");

	printf("\t/// DL-86: map a cosTheta strictly below c0 onto the grazing\n");
	printf("\t/// sub-grid -- sub-interval index k0 in [0, SUB_SIZE-1] and the\n");
	printf("\t/// fraction within it.  Shared by LookupEss/LookupEssG2 and the\n");
	printf("\t/// DL-77 aniso lookups so all of them bin identically.\n");
	printf("\tinline void SubNodeIndex( const Scalar cc, const Scalar c0, int& k0, Scalar& kf )\n");
	printf("\t{\n");
	printf("\t\tconst Scalar t = cc / c0 * Scalar(SUB_SIZE);\n");
	printf("\t\tk0 = (int)t;\n");
	printf("\t\tif( k0 < 0 ) k0 = 0;\n");
	printf("\t\tif( k0 > SUB_SIZE - 1 ) k0 = SUB_SIZE - 1;\n");
	printf("\t\tkf = t - Scalar(k0);\n");
	printf("\t}\n\n");

	// DL-105: low-alpha sub-grid machinery.  See the ALPHA_SUB_FINE/
	// ALPHA_MID_SIZE/ALPHA_LOW_TOTAL constant comments above for the
	// derivation; this block is the runtime counterpart of the
	// generator's alphaLowNode/alphaLowSlot/BuildLowAlphaRow helpers,
	// and MUST agree with them or the baked values land at the wrong
	// abscissae (same discipline as AnisoSubNodeCos/anisoSubNodeCos).
	printf("\t/// DL-105: alpha value of the main table's row 0 / row 1 -- the\n");
	printf("\t/// two endpoints the low-alpha sub-grid below is anchored to.\n");
	printf("\tstatic const Scalar ALPHA_LOW_A0 = Scalar(0.01);\n");
	printf("\tstatic const Scalar ALPHA_LOW_A1 = Scalar(0.01) + Scalar(1.0 - 0.01) * Scalar(1.0) / Scalar(LUT_SIZE - 1);\n\n");

	printf("\t/// DL-105: alpha value of low-alpha virtual node `idx` (idx in\n");
	printf("\t/// [0, ALPHA_LOW_TOTAL]).  idx==0 is the exact alpha->0 boundary,\n");
	printf("\t/// idx==ALPHA_SUB_FINE+1 is A0 (row 0), idx==ALPHA_LOW_TOTAL is A1\n");
	printf("\t/// (row 1) -- see the ALPHA_SUB_FINE/ALPHA_MID_SIZE comment above.\n");
	printf("\tinline Scalar AlphaLowNode( const int idx )\n");
	printf("\t{\n");
	printf("\t\tif( idx <= 0 ) return Scalar(0.0);\n");
	printf("\t\tif( idx <= ALPHA_SUB_FINE ) return ALPHA_LOW_A0 * pow( Scalar(2.0), Scalar(idx - (ALPHA_SUB_FINE + 1)) );\n");
	printf("\t\tif( idx == ALPHA_SUB_FINE + 1 ) return ALPHA_LOW_A0;\n");
	printf("\t\tif( idx < ALPHA_LOW_TOTAL )\n");
	printf("\t\t{\n");
	printf("\t\t\tconst Scalar t = Scalar(idx - (ALPHA_SUB_FINE + 1)) / Scalar(ALPHA_MID_SIZE);\n");
	printf("\t\t\treturn ALPHA_LOW_A0 * pow( ALPHA_LOW_A1 / ALPHA_LOW_A0, t );\n");
	printf("\t\t}\n");
	printf("\t\treturn ALPHA_LOW_A1;\n");
	printf("\t}\n\n");

	printf("\t/// DL-105: map a STORED virtual index (1..ALPHA_SUB_FINE, or\n");
	printf("\t/// ALPHA_SUB_FINE+2..ALPHA_LOW_TOTAL-1) to its flat storage slot.\n");
	printf("\tinline int AlphaLowSlot( const int idx )\n");
	printf("\t{\n");
	printf("\t\tif( idx <= ALPHA_SUB_FINE ) return idx - 1;\n");
	printf("\t\treturn ALPHA_SUB_FINE + ( idx - (ALPHA_SUB_FINE + 2) );\n");
	printf("\t}\n\n");

	printf("\t/// DL-105: bracket a query alpha in [0, ALPHA_LOW_A1) against the\n");
	printf("\t/// 13-position AlphaLowNode list.  A plain linear scan (small,\n");
	printf("\t/// fixed size) rather than a closed-form index -- the geometric\n");
	printf("\t/// spacing makes a closed form awkward across the alpha->0\n");
	printf("\t/// boundary, and this runs only for alpha<ALPHA_LOW_A1 (very\n");
	printf("\t/// smooth surfaces), not the majority-path alpha>=ALPHA_LOW_A1\n");
	printf("\t/// case below.\n");
	printf("\tinline void AlphaLowIndex( const Scalar alpha, int& idx0, int& idx1, Scalar& frac )\n");
	printf("\t{\n");
	printf("\t\tconst Scalar a = r_max( Scalar(0.0), alpha );\n");
	printf("\t\tidx0 = 0;\n");
	printf("\t\tfor( int j = 0; j < ALPHA_LOW_TOTAL; j++ )\n");
	printf("\t\t{\n");
	printf("\t\t\tif( a >= AlphaLowNode( j + 1 ) ) idx0 = j + 1; else break;\n");
	printf("\t\t}\n");
	printf("\t\tif( idx0 > ALPHA_LOW_TOTAL - 1 ) idx0 = ALPHA_LOW_TOTAL - 1;\n");
	printf("\t\tidx1 = idx0 + 1;\n");
	printf("\t\tconst Scalar lo = AlphaLowNode(idx0), hi = AlphaLowNode(idx1);\n");
	printf("\t\tfrac = (hi > lo) ? r_max( Scalar(0.0), r_min( Scalar(1.0), (a - lo) / (hi - lo) ) ) : Scalar(0.0);\n");
	printf("\t}\n\n");

	printf("\t/// DL-105: ordinary-row / grazing-subrow / limit accessors for a\n");
	printf("\t/// low-alpha virtual node, dispatching between the exact\n");
	printf("\t/// alpha->0 boundary (idx<=0), the reused main-table rows\n");
	printf("\t/// (idx==A0's or A1's index), and the new baked sub/mid tables.\n");
	printf("\tinline Scalar AlphaLowOrdinary( const int idx, const int k )\n");
	printf("\t{\n");
	printf("\t\tif( idx <= 0 ) return Scalar(1.0);\n");
	printf("\t\tif( idx == ALPHA_SUB_FINE + 1 ) return E_ss_TABLE[0][k];\n");
	printf("\t\tif( idx >= ALPHA_LOW_TOTAL ) return E_ss_TABLE[1][k];\n");
	printf("\t\treturn E_ss_ALPHA_LOW_TABLE[ AlphaLowSlot(idx) ][k];\n");
	printf("\t}\n\n");
	printf("\tinline Scalar AlphaLowOrdinaryG2( const int idx, const int k )\n");
	printf("\t{\n");
	printf("\t\tif( idx <= 0 ) return Scalar(1.0);\n");
	printf("\t\tif( idx == ALPHA_SUB_FINE + 1 ) return E_ss_TABLE_G2[0][k];\n");
	printf("\t\tif( idx >= ALPHA_LOW_TOTAL ) return E_ss_TABLE_G2[1][k];\n");
	printf("\t\treturn E_ss_ALPHA_LOW_TABLE_G2[ AlphaLowSlot(idx) ][k];\n");
	printf("\t}\n\n");
	printf("\tinline Scalar AlphaLowGrazing( const int idx, const int k )\n");
	printf("\t{\n");
	printf("\t\tif( idx <= 0 ) return Scalar(1.0);\n");
	printf("\t\tif( idx == ALPHA_SUB_FINE + 1 ) return E_ss_SUB_TABLE[0][k];\n");
	printf("\t\tif( idx >= ALPHA_LOW_TOTAL ) return E_ss_SUB_TABLE[1][k];\n");
	printf("\t\treturn E_ss_ALPHA_LOW_SUB_TABLE[ AlphaLowSlot(idx) ][k];\n");
	printf("\t}\n\n");
	printf("\tinline Scalar AlphaLowGrazingG2( const int idx, const int k )\n");
	printf("\t{\n");
	printf("\t\tif( idx <= 0 ) return Scalar(1.0);\n");
	printf("\t\tif( idx == ALPHA_SUB_FINE + 1 ) return E_ss_SUB_TABLE_G2[0][k];\n");
	printf("\t\tif( idx >= ALPHA_LOW_TOTAL ) return E_ss_SUB_TABLE_G2[1][k];\n");
	printf("\t\treturn E_ss_ALPHA_LOW_SUB_TABLE_G2[ AlphaLowSlot(idx) ][k];\n");
	printf("\t}\n\n");
	printf("\t/// SEPARABLE-model cosTheta->0 boundary at a low-alpha node.  No\n");
	printf("\t/// G2 twin exists: that model's boundary is exactly 1 (returned\n");
	printf("\t/// by AlphaLowOrdinaryG2/AlphaLowGrazingG2's own idx<=0 branch).\n");
	printf("\tinline Scalar AlphaLowLimit( const int idx )\n");
	printf("\t{\n");
	printf("\t\tif( idx <= 0 ) return Scalar(1.0);\n");
	printf("\t\tif( idx == ALPHA_SUB_FINE + 1 ) return E_ss_LIMIT_TABLE[0];\n");
	printf("\t\tif( idx >= ALPHA_LOW_TOTAL ) return E_ss_LIMIT_TABLE[1];\n");
	printf("\t\treturn E_ss_ALPHA_LOW_LIMIT_TABLE[ AlphaLowSlot(idx) ];\n");
	printf("\t}\n\n");
	printf("\tinline Scalar AlphaLowEavg( const int idx )\n");
	printf("\t{\n");
	printf("\t\tif( idx <= 0 ) return Scalar(1.0);\n");
	printf("\t\tif( idx == ALPHA_SUB_FINE + 1 ) return E_avg_TABLE[0];\n");
	printf("\t\tif( idx >= ALPHA_LOW_TOTAL ) return E_avg_TABLE[1];\n");
	printf("\t\treturn E_avg_ALPHA_LOW_TABLE[ AlphaLowSlot(idx) ];\n");
	printf("\t}\n\n");
	printf("\tinline Scalar AlphaLowEavgG2( const int idx )\n");
	printf("\t{\n");
	printf("\t\tif( idx <= 0 ) return Scalar(1.0);\n");
	printf("\t\tif( idx == ALPHA_SUB_FINE + 1 ) return E_avg_TABLE_G2[0];\n");
	printf("\t\tif( idx >= ALPHA_LOW_TOTAL ) return E_avg_TABLE_G2[1];\n");
	printf("\t\treturn E_avg_ALPHA_LOW_TABLE_G2[ AlphaLowSlot(idx) ];\n");
	printf("\t}\n\n");

	printf("\t/// DL-105: build an alpha-blended (essRow, subRow, essLimit)\n");
	printf("\t/// triple for a query alpha < ALPHA_LOW_A1, the SEPARABLE model.\n");
	printf("\t/// Callers feed this straight into MSLobeDetail::BuildSegmentsFromRow\n");
	printf("\t/// (the SAME function the main alpha>=ALPHA_LOW_A1 path uses), so\n");
	printf("\t/// LookupEss/BuildSegments/MSLobeZ cannot drift apart here either.\n");
	printf("\tinline void BuildLowAlphaRow( const Scalar alpha, Scalar essRow[LUT_SIZE], Scalar subRow[SUB_SIZE-1], Scalar& essLimit )\n");
	printf("\t{\n");
	printf("\t\tint idx0, idx1; Scalar f;\n");
	printf("\t\tAlphaLowIndex( alpha, idx0, idx1, f );\n");
	printf("\t\tfor( int k = 0; k < LUT_SIZE; k++ )\n");
	printf("\t\t\tessRow[k] = (1-f) * AlphaLowOrdinary(idx0,k) + f * AlphaLowOrdinary(idx1,k);\n");
	printf("\t\tfor( int k = 0; k < SUB_SIZE - 1; k++ )\n");
	printf("\t\t\tsubRow[k] = (1-f) * AlphaLowGrazing(idx0,k) + f * AlphaLowGrazing(idx1,k);\n");
	printf("\t\tessLimit = (1-f) * AlphaLowLimit(idx0) + f * AlphaLowLimit(idx1);\n");
	printf("\t}\n\n");

	printf("\t/// DL-105: height-correlated-G2 twin of BuildLowAlphaRow above.\n");
	printf("\t/// No essLimit output: this model's cosTheta->0 boundary is\n");
	printf("\t/// exactly 1 at every alpha (matching BuildSegmentsG2's own\n");
	printf("\t/// Scalar(1.0) literal passed to BuildSegmentsFromRow).\n");
	printf("\tinline void BuildLowAlphaRowG2( const Scalar alpha, Scalar essRow[LUT_SIZE], Scalar subRow[SUB_SIZE-1] )\n");
	printf("\t{\n");
	printf("\t\tint idx0, idx1; Scalar f;\n");
	printf("\t\tAlphaLowIndex( alpha, idx0, idx1, f );\n");
	printf("\t\tfor( int k = 0; k < LUT_SIZE; k++ )\n");
	printf("\t\t\tessRow[k] = (1-f) * AlphaLowOrdinaryG2(idx0,k) + f * AlphaLowOrdinaryG2(idx1,k);\n");
	printf("\t\tfor( int k = 0; k < SUB_SIZE - 1; k++ )\n");
	printf("\t\t\tsubRow[k] = (1-f) * AlphaLowGrazingG2(idx0,k) + f * AlphaLowGrazingG2(idx1,k);\n");
	printf("\t}\n\n");

	printf("\t/// Look up E_ss(cosTheta, alpha) with bilinear interpolation.\n");
	printf("\t///\n");
	printf("\t/// DL-86: below the first bin center c0 = 0.5/LUT_SIZE this used\n");
	printf("\t/// to flat-clamp to the c0 row, which mis-reads the true, sharply\n");
	printf("\t/// varying Ess right at the grazing limit (measured: alpha=0.01,\n");
	printf("\t/// cosTheta=0.002, independent 20M-sample VNDF quadrature reads\n");
	printf("\t/// 0.9174 for the SEPARABLE model against the flat clamp's\n");
	printf("\t/// 0.8947, a 2.5%% under-read that grows the compensation weight\n");
	printf("\t/// (1-Ess) by 27.5%%).  It now interpolates the BAKED sub-grid\n");
	printf("\t/// (E_ss_SUB_TABLE + E_ss_LIMIT_TABLE, via SubNodeEss) -- an\n");
	printf("\t/// extrapolated bin0->bin1 secant was tried first and is WORSE\n");
	printf("\t/// than the flat clamp here, because row 0 RISES with cosTheta\n");
	printf("\t/// (0.8947 at c0 vs 0.9731 at the next bin) so the secant\n");
	printf("\t/// extrapolates DOWNWARD, away from the true limit.\n");
	printf("\t/// MUST stay in sync with MSLobeDetail::BuildSegmentsFromRow's\n");
	printf("\t/// left end-cap -- MSPdf calls this function directly while\n");
	printf("\t/// SampleMSCosTheta inverts that segment's shape; both now read\n");
	printf("\t/// the same nodes through SubNodeEss, so they cannot disagree.\n");
	printf("\t///\n");
	printf("\t/// DL-105: below ALPHA_LOW_A1 (row 1's alpha), source the row\n");
	printf("\t/// from the low-alpha sub-grid (BuildLowAlphaRow) instead of the\n");
	printf("\t/// main table -- this covers BOTH the alpha<0.01 clamp-to-row-0\n");
	printf("\t/// range and the coarse row0->row1 cell; see ALPHA_SUB_FINE/\n");
	printf("\t/// ALPHA_MID_SIZE above.\n");
	printf("\tinline Scalar LookupEss( const Scalar cosTheta, const Scalar alpha )\n");
	printf("\t{\n");
	printf("\t\tif( alpha < ALPHA_LOW_A1 )\n");
	printf("\t\t{\n");
	printf("\t\t\tScalar essRow[LUT_SIZE]; Scalar subRow[SUB_SIZE-1]; Scalar essLimit;\n");
	printf("\t\t\tBuildLowAlphaRow( alpha, essRow, subRow, essLimit );\n");
	printf("\t\t\tconst Scalar cc2 = r_max(0.0, r_min(1.0, cosTheta));\n");
	printf("\t\t\tconst Scalar c02 = Scalar(0.5) / Scalar(LUT_SIZE);\n");
	printf("\t\t\tif( cc2 < c02 )\n");
	printf("\t\t\t{\n");
	printf("\t\t\t\tint k0; Scalar kf;\n");
	printf("\t\t\t\tSubNodeIndex( cc2, c02, k0, kf );\n");
	printf("\t\t\t\tconst Scalar s0 = (k0 <= 0) ? essLimit : subRow[k0-1];\n");
	printf("\t\t\t\tconst Scalar s1 = (k0+1 >= SUB_SIZE) ? essRow[0] : subRow[k0];\n");
	printf("\t\t\t\treturn r_max( Scalar(0.0), r_min( Scalar(1.0), (1-kf) * s0 + kf * s1 ) );\n");
	printf("\t\t\t}\n");
	printf("\t\t\tconst Scalar c2 = cc2 * LUT_SIZE - 0.5;\n");
	printf("\t\t\tconst int ci02 = (int)c2;\n");
	printf("\t\t\tconst int ci12 = r_min(ci02 + 1, LUT_SIZE - 1);\n");
	printf("\t\t\tconst Scalar cf2 = c2 - ci02;\n");
	printf("\t\t\treturn (1-cf2) * essRow[ci02] + cf2 * essRow[ci12];\n");
	printf("\t\t}\n\n");
	printf("\t\t// Map alpha from [0.01, 1.0] to [0, LUT_SIZE-1]\n");
	printf("\t\tScalar a = r_max(0.0, r_min(1.0, (alpha - 0.01) / (1.0 - 0.01))) * (LUT_SIZE - 1);\n");
	printf("\t\tint ai0 = (int)a;\n");
	printf("\t\tint ai1 = r_min(ai0 + 1, LUT_SIZE - 1);\n");
	printf("\t\tScalar af = a - ai0;\n\n");
	printf("\t\tconst Scalar cc = r_max(0.0, r_min(1.0, cosTheta));\n");
	printf("\t\tconst Scalar c0 = Scalar(0.5) / Scalar(LUT_SIZE);\n");
	printf("\t\tif( cc < c0 )\n");
	printf("\t\t{\n");
	printf("\t\t\tint k0; Scalar kf;\n");
	printf("\t\t\tSubNodeIndex( cc, c0, k0, kf );\n");
	printf("\t\t\tconst Scalar s0 = (1-af) * SubNodeEss(ai0, k0)     + af * SubNodeEss(ai1, k0);\n");
	printf("\t\t\tconst Scalar s1 = (1-af) * SubNodeEss(ai0, k0 + 1) + af * SubNodeEss(ai1, k0 + 1);\n");
	printf("\t\t\treturn r_max( Scalar(0.0), r_min( Scalar(1.0), (1-kf) * s0 + kf * s1 ) );\n");
	printf("\t\t}\n\n");
	printf("\t\t// Map cosTheta from cell centers: idx = cosTheta * LUT_SIZE - 0.5\n");
	printf("\t\tScalar c = cc * LUT_SIZE - 0.5;\n");
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
	printf("\t///\n");
	printf("\t/// DL-105: below ALPHA_LOW_A1, blend the low-alpha Eavg sub-grid\n");
	printf("\t/// instead -- see LookupEss's own DL-105 comment.\n");
	printf("\tinline Scalar LookupEavg( const Scalar alpha )\n");
	printf("\t{\n");
	printf("\t\tif( alpha < ALPHA_LOW_A1 )\n");
	printf("\t\t{\n");
	printf("\t\t\tint idx0, idx1; Scalar f;\n");
	printf("\t\t\tAlphaLowIndex( alpha, idx0, idx1, f );\n");
	printf("\t\t\treturn (1-f) * AlphaLowEavg(idx0) + f * AlphaLowEavg(idx1);\n");
	printf("\t\t}\n");
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
	printf("\t///\n");
	printf("\t/// DL-86: below the first bin center c0 = 0.5/LUT_SIZE this used\n");
	printf("\t/// to flat-clamp to the c0 row, under-reading the true Ess right\n");
	printf("\t/// at the grazing limit and over-stating the Kulla-Conty\n");
	printf("\t/// multiscatter compensation there by up to ~9.9%% relative\n");
	printf("\t/// (measured against an independent 20M-sample VNDF quadrature:\n");
	printf("\t/// alpha=0.01, cosTheta=0.0001, truth 0.99789 vs the flat clamp's\n");
	printf("\t/// 0.89927) -- an isotropic furnace GAIN confined to incidence\n");
	printf("\t/// beyond ~89.1 degrees.  It now interpolates the BAKED grazing\n");
	printf("\t/// sub-grid (E_ss_SUB_TABLE_G2, via SubNodeEssG2), anchored at\n");
	printf("\t/// the provable exact boundary Ess_G2(cosTheta=0)=1 (proof in\n");
	printf("\t/// SubNodeEssG2's own comment).  A single straight line from that\n");
	printf("\t/// boundary to bin 0 was tried first and is NOT sufficient: at\n");
	printf("\t/// alpha=0.01 the true curve DIPS to 0.8920 near cosTheta=0.010\n");
	printf("\t/// and only climbs to 1 as cosTheta itself goes to 0, so a\n");
	printf("\t/// straight line from the boundary over-reads by up to\n");
	printf("\t/// 5.7%% -- a larger error, of the opposite sign, than the flat\n");
	printf("\t/// clamp it replaced.  Residual after the baked sub-grid\n");
	printf("\t/// (independent 20M-sample-per-point quadrature, alpha-row blend\n");
	printf("\t/// included, cosTheta in [1e-4, c0]): <=0.26%% at every tested\n");
	printf("\t/// alpha in [0.01,1.0] EXCEPT inside the FIRST alpha cell (row 0\n");
	printf("\t/// alpha=0.01 to row 1 alpha=0.0419, a 4.2x ratio), where the\n");
	printf("\t/// ALPHA axis alone still contributes up to 1.6%% -- and up to\n");
	printf("\t/// 5.1%% below alpha=0.01, which the table clamps to row 0\n");
	printf("\t/// outright.  Both are DL-105, not this end-cap: neither moves\n");
	printf("\t/// with SUB_SIZE.  MUST stay in sync with\n");
	printf("\t/// MSLobeDetail::BuildSegmentsFromRow's left end-cap -- see\n");
	printf("\t/// LookupEss's own comment on why; both read the same nodes\n");
	printf("\t/// through SubNodeEssG2.\n");
	printf("\t///\n");
	printf("\t/// DL-105: below ALPHA_LOW_A1, source the row from the low-alpha\n");
	printf("\t/// sub-grid (BuildLowAlphaRowG2) instead -- see LookupEss's own\n");
	printf("\t/// DL-105 comment.\n");
	printf("\tinline Scalar LookupEssG2( const Scalar cosTheta, const Scalar alpha )\n");
	printf("\t{\n");
	printf("\t\tif( alpha < ALPHA_LOW_A1 )\n");
	printf("\t\t{\n");
	printf("\t\t\tScalar essRow[LUT_SIZE]; Scalar subRow[SUB_SIZE-1];\n");
	printf("\t\t\tBuildLowAlphaRowG2( alpha, essRow, subRow );\n");
	printf("\t\t\tconst Scalar cc2 = r_max(0.0, r_min(1.0, cosTheta));\n");
	printf("\t\t\tconst Scalar c02 = Scalar(0.5) / Scalar(LUT_SIZE);\n");
	printf("\t\t\tif( cc2 < c02 )\n");
	printf("\t\t\t{\n");
	printf("\t\t\t\tint k0; Scalar kf;\n");
	printf("\t\t\t\tSubNodeIndex( cc2, c02, k0, kf );\n");
	printf("\t\t\t\tconst Scalar s0 = (k0 <= 0) ? Scalar(1.0) : subRow[k0-1];\n");
	printf("\t\t\t\tconst Scalar s1 = (k0+1 >= SUB_SIZE) ? essRow[0] : subRow[k0];\n");
	printf("\t\t\t\treturn r_max( Scalar(0.0), r_min( Scalar(1.0), (1-kf) * s0 + kf * s1 ) );\n");
	printf("\t\t\t}\n");
	printf("\t\t\tconst Scalar c2 = cc2 * LUT_SIZE - 0.5;\n");
	printf("\t\t\tconst int ci02 = (int)c2;\n");
	printf("\t\t\tconst int ci12 = r_min(ci02 + 1, LUT_SIZE - 1);\n");
	printf("\t\t\tconst Scalar cf2 = c2 - ci02;\n");
	printf("\t\t\treturn (1-cf2) * essRow[ci02] + cf2 * essRow[ci12];\n");
	printf("\t\t}\n\n");
	printf("\t\tScalar a = r_max(0.0, r_min(1.0, (alpha - 0.01) / (1.0 - 0.01))) * (LUT_SIZE - 1);\n");
	printf("\t\tint ai0 = (int)a;\n");
	printf("\t\tint ai1 = r_min(ai0 + 1, LUT_SIZE - 1);\n");
	printf("\t\tScalar af = a - ai0;\n\n");
	printf("\t\tconst Scalar cc = r_max(0.0, r_min(1.0, cosTheta));\n");
	printf("\t\tconst Scalar c0 = Scalar(0.5) / Scalar(LUT_SIZE);\n");
	printf("\t\tif( cc < c0 )\n");
	printf("\t\t{\n");
	printf("\t\t\tint k0; Scalar kf;\n");
	printf("\t\t\tSubNodeIndex( cc, c0, k0, kf );\n");
	printf("\t\t\tconst Scalar s0 = (1-af) * SubNodeEssG2(ai0, k0)     + af * SubNodeEssG2(ai1, k0);\n");
	printf("\t\t\tconst Scalar s1 = (1-af) * SubNodeEssG2(ai0, k0 + 1) + af * SubNodeEssG2(ai1, k0 + 1);\n");
	printf("\t\t\treturn r_max( Scalar(0.0), r_min( Scalar(1.0), (1-kf) * s0 + kf * s1 ) );\n");
	printf("\t\t}\n\n");
	printf("\t\tScalar c = cc * LUT_SIZE - 0.5;\n");
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
	printf("\t///\n");
	printf("\t/// DL-105: below ALPHA_LOW_A1, blend the low-alpha Eavg sub-grid\n");
	printf("\t/// instead -- see LookupEss's own DL-105 comment.\n");
	printf("\tinline Scalar LookupEavgG2( const Scalar alpha )\n");
	printf("\t{\n");
	printf("\t\tif( alpha < ALPHA_LOW_A1 )\n");
	printf("\t\t{\n");
	printf("\t\t\tint idx0, idx1; Scalar f;\n");
	printf("\t\t\tAlphaLowIndex( alpha, idx0, idx1, f );\n");
	printf("\t\t\treturn (1-f) * AlphaLowEavgG2(idx0) + f * AlphaLowEavgG2(idx1);\n");
	printf("\t\t}\n");
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

	printf("\t/// DL-86: grazing sub-grid twin of E_ss_TABLE_G2_ANISO_PHI\n");
	printf("\t/// above -- the SAME per-azimuth height-correlated-G2\n");
	printf("\t/// directional albedo, at the ANISO_SUB_TOTAL-1 interior nodes\n");
	printf("\t/// of [0, 0.5/ANISO_COS_SIZE] -- ANISO_SUB_FINE geometric\n");
	printf("\t/// octaves below c0/SUB_SIZE, then the uniform k*c0/SUB_SIZE\n");
	printf("\t/// nodes (see AnisoSubNodeCos).  Indexed as\n");
	printf("\t/// E_ss_TABLE_G2_ANISO_PHI_SUB[alphaXIdx][alphaYIdx][phiIdx][n-1].\n");
	printf("\t/// Consumed by LookupEssG2AnisoDirectional (an energy-\n");
	printf("\t/// compensation call site).  The cosTheta->0 boundary is\n");
	printf("\t/// exactly 1 at every azimuth, so it is not stored.\n");
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_ss_TABLE_G2_ANISO_PHI_SUB[%d][%d][%d][%d] = {\n", ANISO_ALPHA_SIZE, ANISO_ALPHA_SIZE, ANISO_PHI_SIZE, ANISO_SUB_TOTAL - 1);
	for(int ix = 0; ix < ANISO_ALPHA_SIZE; ix++) {
		printf("\t\t{\n");
		for(int iy = 0; iy < ANISO_ALPHA_SIZE; iy++) {
			printf("\t\t\t{\n");
			for(int pi = 0; pi < ANISO_PHI_SIZE; pi++) {
				printf("\t\t\t\t{ ");
				for(int k = 0; k < ANISO_SUB_TOTAL - 1; k++) {
					printf("%.8f", E_ss_G2_ANISO_PHI_SUB[ix][iy][pi][k]);
					if(k < ANISO_SUB_TOTAL - 2) printf(", ");
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

	printf("\t/// DL-86: grazing sub-grid twin of E_ss_TABLE_G2_ANISO above,\n");
	printf("\t/// DERIVED from E_ss_TABLE_G2_ANISO_PHI_SUB by the same\n");
	printf("\t/// trapezoidal phi average.  Consumed by LookupEssG2Aniso and\n");
	printf("\t/// MSLobeDetail::BuildSegmentsFromRowN -- i.e. by the H6 aniso\n");
	printf("\t/// sampler/pdf pair, which must read one shared model.\n");
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_ss_TABLE_G2_ANISO_SUB[%d][%d][%d] = {\n", ANISO_ALPHA_SIZE, ANISO_ALPHA_SIZE, ANISO_SUB_TOTAL - 1);
	for(int ix = 0; ix < ANISO_ALPHA_SIZE; ix++) {
		printf("\t\t{\n");
		for(int iy = 0; iy < ANISO_ALPHA_SIZE; iy++) {
			printf("\t\t\t{ ");
			for(int k = 0; k < ANISO_SUB_TOTAL - 1; k++) {
				printf("%.8f", E_ss_G2_ANISO_SUB[ix][iy][k]);
				if(k < ANISO_SUB_TOTAL - 2) printf(", ");
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

	printf("\t/// DL-161: per-azimuth aniso E_ss_G2 twin of\n");
	printf("\t/// E_ss_TABLE_G2_ANISO_PHI, resolved at ALPHA_LOW_STORED low\n");
	printf("\t/// alphaX virtual nodes (see AlphaLowNode/AnisoAlphaLowIndex)\n");
	printf("\t/// paired against the FULL ordinary alphaY grid.  Indexed as\n");
	printf("\t/// E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_X[AlphaLowSlot(lowIdx)][alphaYIdx][phiIdx][cosThetaIdx].\n");
	printf("\t/// Consumed (via AnisoPhiCellMixedX) by LookupEssG2AnisoDirectional's\n");
	printf("\t/// low-alphaX branch; the low-alphaY case is derived at\n");
	printf("\t/// lookup time by the X<->Y relabel symmetry (phi -> 90-phi),\n");
	printf("\t/// not separately baked.\n");
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_X[%d][%d][%d][%d] = {\n", ALPHA_LOW_STORED, ANISO_ALPHA_SIZE, ANISO_PHI_SIZE, ANISO_COS_SIZE);
	for(int s = 0; s < ALPHA_LOW_STORED; s++) {
		printf("\t\t{\n");
		for(int iy = 0; iy < ANISO_ALPHA_SIZE; iy++) {
			printf("\t\t\t{\n");
			for(int pi = 0; pi < ANISO_PHI_SIZE; pi++) {
				printf("\t\t\t\t{ ");
				for(int ci = 0; ci < ANISO_COS_SIZE; ci++) {
					printf("%.8f", E_ss_G2_ANISO_PHI_ALPHALOW_X[s][iy][pi][ci]);
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
		if(s < ALPHA_LOW_STORED - 1) printf(",");
		printf("\n");
	}
	printf("\t};\n\n");

	printf("\t/// DL-161: grazing sub-grid twin of\n");
	printf("\t/// E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_X above, at the\n");
	printf("\t/// ANISO_SUB_TOTAL-1 interior nodes of [0, 0.5/ANISO_COS_SIZE]\n");
	printf("\t/// (see AnisoSubNodeCos).  Indexed as\n");
	printf("\t/// E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_X_SUB[AlphaLowSlot(lowIdx)][alphaYIdx][phiIdx][n-1].\n");
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_X_SUB[%d][%d][%d][%d] = {\n", ALPHA_LOW_STORED, ANISO_ALPHA_SIZE, ANISO_PHI_SIZE, ANISO_SUB_TOTAL - 1);
	for(int s = 0; s < ALPHA_LOW_STORED; s++) {
		printf("\t\t{\n");
		for(int iy = 0; iy < ANISO_ALPHA_SIZE; iy++) {
			printf("\t\t\t{\n");
			for(int pi = 0; pi < ANISO_PHI_SIZE; pi++) {
				printf("\t\t\t\t{ ");
				for(int k = 0; k < ANISO_SUB_TOTAL - 1; k++) {
					printf("%.8f", E_ss_G2_ANISO_PHI_ALPHALOW_X_SUB[s][iy][pi][k]);
					if(k < ANISO_SUB_TOTAL - 2) printf(", ");
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
		if(s < ALPHA_LOW_STORED - 1) printf(",");
		printf("\n");
	}
	printf("\t};\n\n");

	printf("\t/// DL-161: per-azimuth aniso E_ss_G2 twin of\n");
	printf("\t/// E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_X above, for the corner\n");
	printf("\t/// where BOTH alphaX and alphaY are low-alpha virtual nodes.\n");
	printf("\t/// Relabel-symmetric (baked upper-triangle, mirrored) exactly\n");
	printf("\t/// like E_ss_TABLE_G2_ANISO_PHI itself.  Indexed as\n");
	printf("\t/// E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_XY[AlphaLowSlot(lowIdxX)][AlphaLowSlot(lowIdxY)][phiIdx][cosThetaIdx].\n");
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_XY[%d][%d][%d][%d] = {\n", ALPHA_LOW_STORED, ALPHA_LOW_STORED, ANISO_PHI_SIZE, ANISO_COS_SIZE);
	for(int sx = 0; sx < ALPHA_LOW_STORED; sx++) {
		printf("\t\t{\n");
		for(int sy = 0; sy < ALPHA_LOW_STORED; sy++) {
			printf("\t\t\t{\n");
			for(int pi = 0; pi < ANISO_PHI_SIZE; pi++) {
				printf("\t\t\t\t{ ");
				for(int ci = 0; ci < ANISO_COS_SIZE; ci++) {
					printf("%.8f", E_ss_G2_ANISO_PHI_ALPHALOW_XY[sx][sy][pi][ci]);
					if(ci < ANISO_COS_SIZE - 1) printf(", ");
				}
				printf(" }");
				if(pi < ANISO_PHI_SIZE - 1) printf(",");
				printf("\n");
			}
			printf("\t\t\t}");
			if(sy < ALPHA_LOW_STORED - 1) printf(",");
			printf("\n");
		}
		printf("\t\t}");
		if(sx < ALPHA_LOW_STORED - 1) printf(",");
		printf("\n");
	}
	printf("\t};\n\n");

	printf("\t/// DL-161: grazing sub-grid twin of\n");
	printf("\t/// E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_XY above.  Indexed as\n");
	printf("\t/// E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_XY_SUB[AlphaLowSlot(lowIdxX)][AlphaLowSlot(lowIdxY)][phiIdx][n-1].\n");
	printf("\t/// inline const, not inline constexpr: MSVC's default /constexpr:steps 100000 can't evaluate a table this large.\n");
	printf("\tinline const Scalar E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_XY_SUB[%d][%d][%d][%d] = {\n", ALPHA_LOW_STORED, ALPHA_LOW_STORED, ANISO_PHI_SIZE, ANISO_SUB_TOTAL - 1);
	for(int sx = 0; sx < ALPHA_LOW_STORED; sx++) {
		printf("\t\t{\n");
		for(int sy = 0; sy < ALPHA_LOW_STORED; sy++) {
			printf("\t\t\t{\n");
			for(int pi = 0; pi < ANISO_PHI_SIZE; pi++) {
				printf("\t\t\t\t{ ");
				for(int k = 0; k < ANISO_SUB_TOTAL - 1; k++) {
					printf("%.8f", E_ss_G2_ANISO_PHI_ALPHALOW_XY_SUB[sx][sy][pi][k]);
					if(k < ANISO_SUB_TOTAL - 2) printf(", ");
				}
				printf(" }");
				if(pi < ANISO_PHI_SIZE - 1) printf(",");
				printf("\n");
			}
			printf("\t\t\t}");
			if(sy < ALPHA_LOW_STORED - 1) printf(",");
			printf("\n");
		}
		printf("\t\t}");
		if(sx < ALPHA_LOW_STORED - 1) printf(",");
		printf("\n");
	}
	printf("\t};\n\n");

	printf("\t/// DL-161: E_avg_G2_ANISO twin of\n");
	printf("\t/// E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_X -- low alphaX (rows)\n");
	printf("\t/// against the ordinary alphaY grid (columns), derived by the\n");
	printf("\t/// SAME phi-trapezoidal-average-then-cosTheta-integral rule\n");
	printf("\t/// E_avg_TABLE_G2_ANISO itself uses.  Indexed as\n");
	printf("\t/// E_avg_TABLE_G2_ANISO_ALPHALOW_X[AlphaLowSlot(lowIdx)][alphaYIdx].\n");
	printf("\tinline const Scalar E_avg_TABLE_G2_ANISO_ALPHALOW_X[%d][%d] = {\n", ALPHA_LOW_STORED, ANISO_ALPHA_SIZE);
	for(int s = 0; s < ALPHA_LOW_STORED; s++) {
		printf("\t\t{ ");
		for(int iy = 0; iy < ANISO_ALPHA_SIZE; iy++) {
			printf("%.8f", E_avg_G2_ANISO_ALPHALOW_X[s][iy]);
			if(iy < ANISO_ALPHA_SIZE - 1) printf(", ");
		}
		printf(" }");
		if(s < ALPHA_LOW_STORED - 1) printf(",");
		printf("\n");
	}
	printf("\t};\n\n");

	printf("\t/// DL-161: E_avg_G2_ANISO twin of\n");
	printf("\t/// E_ss_TABLE_G2_ANISO_PHI_ALPHALOW_XY -- BOTH alphaX and\n");
	printf("\t/// alphaY low.  Symmetric by construction (Eavg has no phi\n");
	printf("\t/// axis to break the X<->Y relabel symmetry).  Indexed as\n");
	printf("\t/// E_avg_TABLE_G2_ANISO_ALPHALOW_XY[AlphaLowSlot(lowIdxX)][AlphaLowSlot(lowIdxY)].\n");
	printf("\tinline const Scalar E_avg_TABLE_G2_ANISO_ALPHALOW_XY[%d][%d] = {\n", ALPHA_LOW_STORED, ALPHA_LOW_STORED);
	for(int sx = 0; sx < ALPHA_LOW_STORED; sx++) {
		printf("\t\t{ ");
		for(int sy = 0; sy < ALPHA_LOW_STORED; sy++) {
			printf("%.8f", E_avg_G2_ANISO_ALPHALOW_XY[sx][sy]);
			if(sy < ALPHA_LOW_STORED - 1) printf(", ");
		}
		printf(" }");
		if(sx < ALPHA_LOW_STORED - 1) printf(",");
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
