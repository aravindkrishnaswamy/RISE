//////////////////////////////////////////////////////////////////////
//
//  WeaveBRDF.h - The structured two-thread-family cloth response for
//    `weave_material` (docs/CLOTH_FABRIC_DESIGN.md Phase 2, slice
//    P2-A).
//
//  WHAT THIS IS FOR.  9.9 gate 9b measured `fabric_material`'s limit
//  and named it: 95 % (silk) / 99 % (satin) of an anisotropic GGX
//  substrate's highlight anisotropy survives being wrapped in the
//  isotropic Charlie sheen, so 9.5's delegation works almost
//  losslessly -- and the frame STILL reads as brushed metal, because a
//  single elliptical lobe with a painted rotation field has no PATTERN
//  SCALE.  Real satin's look is discrete floats, each a short
//  cylindrical highlight with its own orientation and its own
//  shadowing against its neighbours.  That is what this material makes,
//  and it is why it is a NEW material rather than a mode on
//  `fabric_material`: nothing here is a sheen lobe, and the fuzz layer
//  stacks ON TOP of it (`fabric_material`'s substrate allowlist accepts
//  `weave_material`, which is the physical stack -- fuzz over weave).
//
//  ============================================================
//  1.  THE MODEL
//  ============================================================
//
//  Two thread families k in {warp, weft}, each with its own tangent,
//  its own dye and its own two lobes.  At a shading point:
//
//    f(i, o) = available * SUM_k  ahat_k * M_k(i,o) *
//                            [ f_surf,k(i,o) + A_k * f_vol,k(i,o) ]
//
//  where `ahat_warp` is the weave's warp-coverage field a_warp(x) in
//  [0,1], `ahat_weft = 1 - ahat_warp`, and `available = 1 - gap`.
//
//  THE GAP IS AN ENERGY FACTOR, NOT A SAMPLING ONE, and the split is
//  deliberate.  Sadeghi 2013's Eq. 15 folds the gap into a
//  VIEW-DEPENDENT normaliser Q; that form is NOT RECIPROCAL (Q reads
//  omega_r and nothing else), and 9.9's reciprocity gate is
//  non-negotiable.  So the gap here is a direction-independent scalar
//  that darkens the response -- P2-A has no transmission lobe, so a
//  gap genuinely reflects nothing -- while the lobe-selection pmf
//  (`ahat_k`, which sums to 1) is what the sampler uses.  That keeps
//  `Pdf` normalised AND the BRDF reciprocal, which Sadeghi's own form
//  cannot do simultaneously.
//
//  1a.  THE FIBRE FRAME.  The warp tangent is the hit's shading-frame
//  u-axis rotated about the normal by `weave_rotation`
//  (`MicrofacetUtils::RotateTangent`, the same helper
//  `fabric_material`'s own weave rotation and GGX's `tangent_rotation`
//  use, so all three simply ADD).  The weft is the warp rotated a
//  further 90 deg + `weft_skew`.  Each family's tangent is then TILTED
//  out of the surface plane by its own `float_tilt` about the in-plane
//  perpendicular -- Sadeghi's tangent-offset reduced to one scalar per
//  family (P2_ZHU_READ 4.3's stated simplification of his
//  multi-segment tangent curve; the curve itself is Phase-3 material).
//
//  Given a family tangent `t`, the cross-section basis is
//  n_k = normalize(n - (n.t) t) and b_k = t x n_k, so azimuth is
//  measured FROM THE SURFACE NORMAL, which is Sadeghi's convention and
//  is what makes the masking term below mean anything.  For a direction
//  w (both directions point AWAY from the surface):
//
//      sin(theta) = w.t            (longitudinal, from the normal plane)
//      phi        = atan2(w.b_k, w.n_k)
//      theta_d    = (theta_i - theta_o) / 2
//      phi_d      = phi_i - phi_o, wrapped to [-pi, pi]
//
//  1b.  THE SURFACE LOBE (Sadeghi Eq. 2, on RISE's hair primitives):
//
//      f_surf,k = F(eta_k, cos(theta_d) cos(phi_d/2))
//               * Mp(theta_i, theta_o; beta_k^2)
//               * TrimmedLogistic(phi_d; s_k, -pi, pi)
//
//  The Fresnel argument is Kim 2002's genuine cylinder-geometry
//  effective incidence, not a naive half-angle: at exact backscatter in
//  the normal plane it is 1 (head-on retroreflection off the cylinder),
//  on the specular cone at 45 deg it is cos 45, and at phi_d = pi it is
//  0 (the light grazes the cylinder tangentially).
//
//  `Mp` is d'Eon 2011's energy-conserving longitudinal lobe, promoted
//  out of HairBSDF.cpp into FibreLobeMath.h for this material.  It
//  REPLACES Sadeghi's plain Gaussian g(gamma_s, theta_h) and is
//  strictly better: normalised at every width rather than only in the
//  narrow-angle limit, and it peaks on the specular cone
//  theta_i == -theta_o, which is a smooth cylinder's actual reflection
//  geometry.
//
//  `TrimmedLogistic` REPLACES Sadeghi's bare cos(phi_d/2).  His factor
//  has no independently normalised closed-form sampler -- his own
//  conclusion names importance sampling as open work -- and RISE
//  already owned a unimodal azimuthal shape with an exact CDF and an
//  exact inverse.  It is peaked at phi_d = 0 (backscatter) exactly as
//  cos(phi_d/2) is, and the authored `azimuth` gamma_k is its angular
//  WIDTH: the logistic scale is s = gamma * sqrt(3)/pi, so gamma is the
//  standard deviation and reads on the same scale as `width`.
//
//  SADEGHI'S EQ. 4 -- THE SHARED `1/cos^2(theta_d)` DIVISOR HE WRAPS
//  AROUND *BOTH* LOBES -- IS DELIBERATELY ABSENT, and that has to be
//  said explicitly because a reader who knows the paper and "restores"
//  it will silently break every preset.  Eq. 4 exists to compensate his
//  Gaussian `g`: it is un-normalised, so its longitudinal integral is
//  not 1 and the divisor is what makes the *lobe*, not `g` itself,
//  integrate correctly.  `Mp` is d'Eon's NORMALISED replacement --
//  `INT Mp cos(theta_i) dtheta_i == 1` by construction (1b above) -- so
//  the compensation the divisor exists to provide is already inside
//  `Mp`.  Adding `1/cos^2(theta_d)` on top applies it twice.  Measured
//  on the white satin preset (both dyes forced to 1) under the
//  corrected `C_v` below: as shipped the directional albedo is ~0.75
//  at theta_v = 0; reintroducing the divisor rises to ~0.95 at
//  theta_v = 30 and to a plainly over-unity 2.09 at theta_v = 60
//  (review scratch program `wv2.cpp`, this session).  The omission is
//  correct; this paragraph is what used to be missing.
//
//  THE SURFACE LOBE IS UNTINTED, AND THAT IS A MODELLING CLAIM.  A
//  dielectric's specular reflection preserves the incident spectrum;
//  only light that ENTERS the fibre picks up the dye.  P2_ZHU_READ 3.8
//  records this as one of the four places Sadeghi demonstrates
//  Irawan-Marschner failing (matching a two-tone shot fabric forced
//  Irawan into "physically incorrect" multiplication of the specular
//  coefficient by a colour).  `warp_color` / `weft_color` therefore tint
//  the VOLUME lobe only.
//
//  1c.  THE VOLUME LOBE (Sadeghi Eq. 3, with the brief's bracketing):
//
//      f_vol,k = (1 - F) * [ (1-k_d) Mp(theta_i,theta_o; (2 beta_k)^2) + k_d ]
//                        / ( (cos theta_i + cos theta_o) * C_v )
//
//  The second `Mp`'s width is TWICE the surface lobe's, which is not a
//  guess: Table II's gamma_v is ~2x gamma_s in EVERY one of its six
//  rows (5/2.5, 10/5, 32/18, 60/30, 8/4, 12/6 -- P2_ZHU_READ 3.4).  It
//  is therefore DERIVED here rather than authored, cutting a parameter.
//
//  `1/(cos theta_i + cos theta_o)` is Chandrasekhar 1960's
//  semi-infinite-medium diffuse-reflectance form, which Sadeghi adopts
//  empirically ("gave us better matches with our measured results").
//  It is symmetric under an i/o swap, so it cannot break reciprocity,
//  and it is floored away from zero -- the pair of directions that
//  drives it to zero is the one running along the yarn axis, where the
//  surface cosine is vanishing anyway.
//
//  `(1 - F)` uses the SAME effective-angle Fresnel as the surface lobe,
//  so the two lobes split one energy budget: what is not reflected
//  enters the fibre.  Sadeghi writes a product of two transmittances
//  (in then out); one factor is used here because the second would
//  double-count against `C_v`'s bound and because the exit Fresnel's
//  argument is not knowable without the internal path his model also
//  does not track.
//
//  1d.  MASKING (Sadeghi Eq. 7-9, verbatim).  Threads of the same
//  cardinal direction shadow each other:
//
//      M(t, w)      = max(cos phi(w), 0)
//      u(phi_d)     = exp( -phi_d^2 / (2 sigma^2) ),  sigma = 20 deg
//      M(t, i, o)   = (1-u) M(t,i) M(t,o) + u min( M(t,i), M(t,o) )
//
//  `u` is Ashikhmin et al. 2000's correlation blend: as the two
//  directions align, the naive independent-occlusion product is wrong
//  (you see exactly what you light) and the model slides to the
//  correlated `min`.  sigma is FIXED at 20 deg, the middle of the
//  paper's own 15-25 deg range, and is deliberately not authorable --
//  it is a property of how yarns occlude, not of a fabric.
//
//  `M` is symmetric under an i/o swap (the two arms exchange; `u` is
//  even in phi_d), so masking cannot break reciprocity -- which is
//  exactly why it is worth implementing and Sadeghi's Q is not.
//
//  CROSS-FAMILY MASKING IS NOT MODELLED, and that is the paper's own
//  scope: "we do not compute masking between different threads"
//  (P2_ZHU_READ 3.3).  Warp does not shadow weft here.
//
//  SADEGHI'S REWEIGHTING (his Eq. 11-15) IS ABSENT, and a prior revision
//  of this note claimed the reason was that it "degenerates to a
//  constant" under P2-A's one-tangent-per-family simplification -- THAT
//  CLAIM IS WRONG and is corrected here (P2R1 review, finding P2-3).
//  Eq. 15's normaliser sums the ONE-DIRECTION quantity `P(t, omega_r)`
//  over BOTH families:
//
//      Q = (a_warp/N_warp) SUM_t P(t,w_r) + (a_weft/N_weft) SUM_t P(t,w_r)
//        + (1 - a_warp - a_weft)(w_r . n)
//
//  while the numerator being reweighted carries the TWO-DIRECTION
//  quantity `P(t, omega_i, omega_r)` for the ONE family under
//  evaluation.  Those are different numbers -- even at N=1 segment per
//  family, Q still mixes in the OTHER family's one-direction term --
//  and they cancel only for a single-family weave, which P2-A never is.
//  So dropping Q is a REAL energy loss, concentrated at grazing views
//  along a family's own axis: for the shipped satin preset
//  (a_warp = 0.8) at a view 80 deg off the warp axis,
//  P(warp, w_r) = cos 80 = 0.17 and P(weft, w_r) = 1, so
//  Q = 0.8(0.17) + 0.2(1) = 0.336 and the undivided form is missing a
//  factor of 1/Q = 2.98 there.
//
//  Q IS STILL NOT IMPLEMENTED, and the reason is the one already given
//  above for masking's correlation blend: Q reads `omega_r` ALONE, so
//  any form that divides by it is not reciprocal, and 9.9's reciprocity
//  gate (SPFBSDFConsistencyTest, 1e-6) is non-negotiable.  A reciprocal
//  substitute -- the geometric mean `Q_sym = sqrt(Q(w_i) Q(w_o))`,
//  the same device `AzimuthalTrimBoost` and the volume lobe's `C_v`
//  latitude dependence both use elsewhere in this file -- was tried and
//  rejected: at the same grazing configuration `Q_sym` runs as low as
//  0.30-0.34 (both arms are individually small along a family axis, and
//  the geometric mean does not rescue that the way an arithmetic mean
//  would), so `1/Q_sym` reaches ~3x and pushes white presets past the
//  furnace's 1.05 ceiling at grazing.  Trading a bounded, reciprocal,
//  ~3x-under-energy model for an over-unity one to fix a grazing corner
//  is the wrong side of 9.9's gate.  So Q stays OUT and the resulting
//  grazing-only deficit is a recorded debt (docs/CLOTH_FABRIC_DESIGN.md
//  10.3, debt 7) rather than a silent gap.  All that survives of
//  Eq. 15 is the gap term, which is `available` above.
//
//  ============================================================
//  2.  ENERGY -- BOUNDED BY CONSTRUCTION, NOT BY A TABLE
//  ============================================================
//
//  `fabric_material` needed a baked directional-albedo table because
//  Charlie's albedo has no closed form.  This material needs none,
//  because both of its lobes inherit normalisations that make the
//  directional albedo provably bounded.  Writing a direction in the
//  fibre frame, dw = cos(theta) dtheta dphi and n.w <= 1:
//
//    rho_surf = INT f_surf (n.w_i) dw_i
//             <= INT INT Mp(theta_i,theta_o;v) N(phi_d) cos(theta_i) dtheta dphi
//             =  [ INT Mp cos(theta_i) dtheta_i ] [ INT N dphi_i ]
//             =  1 * 1  =  1
//
//  using F <= 1, `Mp`'s d'Eon normalisation and the trimmed logistic's
//  own.  The bound holds at ANY tilt, because it never assumed
//  n.w_i == cos(theta_i) cos(phi_i).  `ComputeThreadTerms` in fact
//  divides the surface lobe's azimuthal factor by the SAME visible
//  interval `SurfaceLobePdf` renormalises over (`AzimuthalTrimBoost`,
//  symmetrised so reciprocity survives) rather than leaving the loose
//  [-pi,pi] normaliser above -- that closes an ~8 % leak the untrimmed
//  form had (mass the horizon gate discards) and is why `value()` and
//  the sampler's density stay proportional.
//
//  THE VOLUME LOBE'S NORMALISER `C_v` IS THE EXACT INTEGRAL, NOT A
//  BOUND -- this is the one place P2-A's first cut got the arithmetic
//  wrong and it is worth being precise about what replaced it.
//  `cos(theta_i)/(cos theta_i + cos theta_o) <= 1` gives the LOOSE bound
//  `C_v <= 2(1-k_d) + 4k_d`, and an earlier revision shipped exactly
//  that bound (`C_v = 2(1+k_d)`) AS IF it were the normaliser -- it is
//  2.00-2.33x too large across the authorable k_d range, and because
//  the volume lobe carries ~90-96 % of a white weave's energy, that one
//  constant was the whole "every fabric renders 3-8x dark" defect
//  (REVIEW_P2R1.md P1-1, this session).  `WeaveBRDF.cpp`'s
//  `ComputeThreadTerms` now divides by the EXACT hemispherical integral
//  of the bracket at zero tilt,
//
//    C_v(k_d, theta_o) = (1 - k_d) + k_d * sqrt( J_d(theta_i) J_d(theta_o) )
//
//  where `J_d` is `VolumeKdIntegral` (the closed-form
//  `2[(2 - c pi) + c^2 (4/sqrt(1-c^2)) atan(sqrt((1-c)/(1+c)))]`,
//  `c = cos theta`) and the geometric mean of the two latitudes' `J_d`
//  is what keeps the result reciprocal -- the same device
//  `AzimuthalTrimBoost` and the (deliberately absent) `Q_sym` above use.
//  At theta_o = 0 this reduces exactly to the closed form derived at
//  `ComputeThreadTerms`'s definition site, `(1-k_d) + 2(4-pi)k_d`.  The
//  volume lobe therefore realises its budget `(1-F) A_k` almost exactly
//  at normal incidence and rises somewhat above it toward grazing (the
//  true integral grows as the Chandrasekhar denominator relaxes, and a
//  normaliser pinned at theta_o=0 under-divides there) -- BOUNDED, not
//  exactly conserving, which is why LayeredWhiteFurnaceTest's weave
//  rows stay `kPostureBounded` with an explicit <= 1.05 ceiling rather
//  than being asserted to reach exactly A_k.
//
//  THE REALISED (not merely bounded) NORMAL-INCIDENCE VALUES are exact
//  closed forms too, derived at `hemisphericalAlbedo`'s definition
//  site: `rho_vol(0) = A_k (1 - F_k(1)) pi/4` and
//  `rho_surf(0) = F_k(1) G(s_k)`, where `F_k(1)` is the Fresnel
//  reflectance AT NORMAL INCIDENCE (where the surface lobe's mass
//  actually sits), NOT the hemispherical average `F_bar` an earlier
//  revision budgeted it at -- `F_bar` is ~2.2x `F(1)` at eta 1.539, and
//  using it left a fitted constant to absorb the gap (REVIEW_P2R1.md
//  P2-4).  Nothing here is fitted any more; every factor is a named
//  integral.
//
//  Masking (<= 1), coverage (sums to 1) and `available` (<= 1) only
//  ever reduce, so the whole mixture stays bounded.
//
//  ============================================================
//  3.  WHAT THIS PHASE DOES NOT DO
//  ============================================================
//
//  NO TRANSMISSION.  `ScattersFullSphere` and `CouldLightPassThrough`
//  stay false; the backlit glow-through cue is slice P2-B, and claiming
//  it here would put the full-sphere NEE machinery behind a lobe that
//  does not exist.  Zhu 2023's delta-transmission term
//  `delta(i+o)/(i.n_s)` is the model for it when that slice lands.
//
//  NO PER-TEXEL ASG VISIBILITY BAKE.  Zhu 2023's headline contribution
//  is a per-texel Anisotropic-Spherical-Gaussian fit of the visibility
//  function (5 floats/texel) built by a radial horizon search -- real
//  infrastructure, a bake pass and a storage format, deliberately NOT
//  adopted (P2_ZHU_READ 1.4).  Sadeghi's cardinal-direction masking is
//  a closed form that needs neither, and it is the reciprocal one.
//
//  NO SGGX.  Zhu's reflection lobe is an SGGX microflake evaluation
//  with a Smith Lambda; RISE has no SGGX evaluator or VNDF sampler
//  (`grep -rni SGGX src/Library` is still empty), and the read's own
//  recommendation is to take Sadeghi's factorisation on RISE's existing
//  hair primitives precisely because it needs no new infrastructure.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WEAVE_BRDF_
#define WEAVE_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"
#include "WeavePresets.h"

namespace RISE
{
	namespace Implementation
	{
		class WeaveBRDF :
			public virtual IBSDF,
			public virtual Reference
		{
		public:
			//! Clamps.  Every one of these bounds a quantity that would
			//! otherwise let a mis-authored painter produce a NaN or an
			//! unbounded lobe; see the notes at their use sites.
			static const Scalar kMinWidth;		///< 0.005 rad -- below this Mp's variance underflows
			static const Scalar kMaxWidth;		///< 1.0 rad
			static const Scalar kMinAzimuth;	///< 0.02 rad
			static const Scalar kMaxAzimuth;	///< 3.0 rad
			static const Scalar kMinIOR;		///< 1.001
			static const Scalar kMaxIOR;		///< 3.0
			static const Scalar kMaxGap;		///< 0.3
			//! 0.17 rad (~9.7 deg).  BOUNDED, not the earlier 0.6 rad
			//! (~34.4 deg), because the masking term's per-family azimuth
			//! has a coordinate pole at view latitude `90 - tilt_deg`
			//! (P2R4 review, finding P1-1): at the old clamp the pole sat
			//! at an ORDINARY, in-frame view angle (~55.6 deg) and
			//! produced a 48.9% brightness drop in a 0.5 deg step.  0.17
			//! keeps the pole at or beyond 80 deg for any in-range tilt,
			//! which -- combined with the pole-conditioning in
			//! `ProjectDir` and the smoothed masking gate below -- keeps
			//! it out of any ordinary framing even before the smoothing
			//! is counted.  See `kPoleBlendWidth` and `kMaskGateHalfWidth`.
			static const Scalar kMaxTilt;
			//! Masking correlation width, Sadeghi Eq. 9's `u`.  20 deg,
			//! the middle of the paper's stated 15-25 deg range.  NOT
			//! authorable on purpose.
			static const Scalar kMaskCorrelationSigma;
			//! `ProjectDir`'s pole-conditioning ramp width, in `h` (the
			//! in-plane projection magnitude, which EQUALS `cosTheta`
			//! measured from the fibre axis -- see the note at its use
			//! site).  `h` vanishes exactly at the fibre-axis pole where
			//! "front of thread" vs "back of thread" is undefined; below
			//! this width the raw `e1/h` ratio is blended toward 0 (the
			//! pole's own neutral value) rather than trusted, so `cosPhi`
			//! stays a smooth function of view direction THROUGH the
			//! pole instead of swinging through its full [-1,1] range in
			//! a fraction of a degree.  0.10 rad (~5.7 deg) of ramp on
			//! each side of the pole -- comfortably wider than any
			//! practical sampling step, confirmed by the 0.25 deg sweep
			//! in `WeaveMaterialChunkTest`.
			static const Scalar kPoleBlendWidth;
			//! The masking gate's own smoothing half-width, in `cosPhi`
			//! (dimensionless, range [-1,1]).  Replaces the hard
			//! `max(cosPhi, 0)` hinge with `SmoothedRamp`'s C1 quadratic
			//! mollification of the SAME hinge over
			//! `[-kMaskGateHalfWidth, +kMaskGateHalfWidth]` -- matching
			//! VALUE at both boundaries (0 and `kMaskGateHalfWidth`), so
			//! `mI`/`mO` stay the graded cosine-weighted occlusion factor
			//! Sadeghi's model is (equal to `cosPhi` itself once past the
			//! window) rather than becoming a saturating front/back
			//! gate -- independent of, and in addition to, the pole
			//! conditioning above (that fixes the INPUT to this gate;
			//! this fixes the gate's own kink).
			static const Scalar kMaskGateHalfWidth;

			//! `2(4 - pi)` = 1.71681, the k_d half of the volume lobe's
			//! normaliser `C_v = (1 - k_d) + 2(4 - pi) k_d`.  Named rather
			//! than inlined because the derivation at its use site refers
			//! to it by name and because it is a closed-form integral, not
			//! a tuned value.
			static const Scalar kVolumeKdNormaliser;

			//! Floor on either one-sided surviving azimuthal mass in
			//! `AzimuthalTrimBoost`, capping its boost at 20x.  See that
			//! function; the smallest value the shipped presets reach is
			//! 0.62, so this never binds on them.
			static const Scalar kMinAzimuthMass;



			//! One thread family, resolved at ONE shading point in ONE
			//! colour regime.
			struct ThreadParams
			{
				Vector3	tangent;	///< unit, tilted out of plane by `float_tilt`
				Scalar	eta;
				Scalar	vSurf;		///< beta^2, the surface Mp variance
				Scalar	vVol;		///< (2 beta)^2 -- Table II's recurring 2:1 ratio
				Scalar	s;			///< trimmed-logistic scale = gamma * sqrt(3)/pi
				Scalar	kd;
				RISEPel	tint;		///< A_k, RGB regime (undefined on the NM path)
				Scalar	tintNM;		///< A_k at the hero wavelength (undefined on the RGB path)
			};

			//! Everything the evaluator and the sampler both need,
			//! resolved once.  Shared so the two can never drift about
			//! which frame, which coverage or which selection weight
			//! they are using -- FabricBRDF/FabricSPF's own contract.
			struct WeaveParams
			{
				Vector3			n;			///< ray-facing shading normal
				Scalar			aWarp;		///< warp-coverage field in [0,1]; the lobe-selection pmf
				Scalar			available;	///< 1 - gap; an ENERGY factor only (see the banner)
				ThreadParams	warp;
				ThreadParams	weft;
			};

			//! The per-family geometry at one (i, o) pair.  Colour-free:
			//! one body serves the RGB and the spectral pipe, which is
			//! the structural half of
			//! docs/skills/audit-by-bug-pattern.md's twin discipline.
			struct ThreadTerms
			{
				bool	valid;		///< false => this family contributes nothing
				Scalar	surface;	///< F * Mp * N * mask   (UNTINTED)
				Scalar	volume;		///< (1-F) * [..] / ((cos+cos) Cv) * mask  (tinted by A_k)
			};

			WeaveBRDF(
				const WeavePatternKind pattern,
				const IScalarPainter& weaveScale,
				const IScalarPainter& weaveRotation,
				const IScalarPainter& weftSkew,
				const IScalarPainter* coverage,		///< NULL unless `pattern == eWeaveCustom`
				const IScalarPainter& gap,
				const IPainter& warpColor,
				const IScalarPainter& warpIOR,
				const IScalarPainter& warpWidth,
				const IScalarPainter& warpAzimuth,
				const IScalarPainter& warpKd,
				const IScalarPainter& warpTilt,
				const IPainter& weftColor,
				const IScalarPainter& weftIOR,
				const IScalarPainter& weftWidth,
				const IScalarPainter& weftAzimuth,
				const IScalarPainter& weftKd,
				const IScalarPainter& weftTilt
				);

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar  valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;
			virtual bool    hemisphericalAlbedo( const RayIntersectionGeometric& ri, RISEPel& out ) const;
			virtual bool    hemisphericalAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm, Scalar& out ) const;

			//! Resolve every weave parameter at `ri`.  `nm < 0` selects
			//! the RGB regime, `nm >= 0` the spectral one -- and ONLY the
			//! two tints differ between them.
			//!
			//! EVERY GEOMETRIC SCALAR IS READ ACHROMATICALLY (through
			//! `GetValuesAt(...).v[0]`, never `GetValueAtNM`), and that
			//! is the same rule `FabricBRDF::ResolveFabric` states at
			//! length: `IScalarPainter` permits a wavelength-varying
			//! scalar, and `requireSingle` rejects PER-CHANNEL painters
			//! rather than wavelength-varying ones.  A wavelength-varying
			//! width, azimuth, IOR, tilt, coverage, scale, rotation, skew
			//! or gap would put `Scatter`'s STORED hero-wavelength pdf
			//! out of step with a companion-wavelength `Pdf()` call --
			//! and for the frame-defining ones (rotation, skew, tilt) it
			//! would mean the sampler and the density disagree about the
			//! tangent basis itself.  One geometry, both pipes.
			void ResolveWeave( const RayIntersectionGeometric& ri, const Scalar nm, WeaveParams& out ) const;

			//! ONE family's fibre frame.  `t` is the tilted tangent, `nk`
			//! the component of the surface normal perpendicular to it
			//! (so azimuth is measured FROM THE NORMAL, Sadeghi's
			//! convention), and `bk = t x nk` completes it.
			//!
			//! `sinAlpha = n.t` and `cosAlpha = |n - (n.t)t|` record how
			//! far the tilt has pushed the tangent out of the surface
			//! plane; the sampler needs them to decide which azimuths at
			//! a given fibre latitude are above the horizon at all
			//! (`n.w = sinAlpha sin(theta) + cosAlpha cos(theta) cos(phi)`,
			//! which reduces to `cos(theta) cos(phi)` only when the tilt
			//! is zero).
			struct FibreFrame
			{
				Vector3	t, nk, bk;
				Scalar	sinAlpha, cosAlpha;
				bool	valid;		///< false iff the tangent is parallel to n
			};

			//! One direction resolved in a fibre frame.  `cosPhi` is
			//! carried alongside `phi` because the masking term needs it
			//! and recomputing `cos(atan2(...))` loses the stability the
			//! direct ratio has near the axis.
			struct DirAngles
			{
				Scalar	sinTheta, cosTheta;		///< cosTheta >= 0 by construction
				Scalar	phi, cosPhi;
			};

			static FibreFrame MakeFibreFrame( const Vector3& tangent, const Vector3& n );
			static DirAngles  ProjectDir( const FibreFrame& f, const Vector3& w );

			//! The azimuth half-range visible above the surface at fibre
			//! latitude `theta`.  FALSE means nothing at this latitude is
			//! above the horizon at all -- reachable only under a
			//! non-zero tilt, where the whole latitude band beyond
			//! `pi/2 - |alpha|` is buried.
			static bool VisibleAzimuthHalfRange( const FibreFrame& f,
			                                     const Scalar sinTheta, const Scalar cosTheta,
			                                     Scalar& phiMax );

			//! The interval of `phi_d = phi_i - phi_o` the SAMPLER may
			//! draw from at fibre latitude `theta_i`, i.e. the visible
			//! azimuth range recentred on the view.
			//!
			//! THIS IS WHAT MAKES THE SURFACE LOBE'S DENSITY INTEGRATE TO
			//! 1 OVER THE HEMISPHERE rather than over the sphere.  The
			//! fibre-frame (theta, phi) parametrisation covers the whole
			//! SPHERE, so a naive draw would put a large fraction of its
			//! mass below the surface plane; rejecting those would leave
			//! `Pdf` un-normalised by a view-dependent factor with no
			//! closed form -- precisely the failure 9.4 rejected the
			//! Charlie D-sampler for.  Restricting the trimmed logistic's
			//! OWN interval instead is exact and free: the trimmed
			//! logistic is normalised over whatever [a, b] it is given
			//! and inverts over it exactly, so every draw lands above the
			//! horizon and the density still integrates to 1.
			static bool SurfaceAzimuthInterval( const FibreFrame& f,
			                                    const Scalar sinThetaI, const Scalar cosThetaI,
			                                    const Scalar phiO,
			                                    Scalar& lo, Scalar& hi );

			//! The surface lobe's solid-angle density at `iA`, given the
			//! view resolved at `oA`:  `Mp * TrimmedLogistic`, with the
			//! logistic trimmed to `SurfaceAzimuthInterval`.  Shared by
			//! `WeaveSPF::Scatter` and `WeaveSPF::Pdf` so the sampler and
			//! the density are literally the same expression.
			static Scalar SurfaceLobePdf( const ThreadParams& t, const FibreFrame& f,
			                              const DirAngles& oA, const DirAngles& iA );

			//! The FRACTION of the full [-pi, pi] azimuthal lobe that
			//! survives the horizon trim at latitude `thetaA` for a
			//! partner azimuth `phiB`.  0 when the whole latitude is
			//! buried.
			static Scalar VisibleAzimuthMass( const FibreFrame& f, const ThreadParams& t,
			                                  const Scalar sinThetaA, const Scalar cosThetaA,
			                                  const Scalar phiB );

			//! The RECIPROCAL renormalisation `value()` applies to the
			//! surface lobe so it agrees with the density the sampler
			//! draws from.  `1 / sqrt(Z_i * Z_o)` -- symmetric under an
			//! i/o swap by construction.  See the long note at the
			//! definition for why the sampler's own one-sided interval
			//! cannot be reused here.
			static Scalar AzimuthalTrimBoost( const FibreFrame& f, const ThreadParams& t,
			                                  const DirAngles& a, const DirAngles& b );

			//! Both lobes of family `t` at one direction pair, masking
			//! folded in.  `vLightIn` and the view are both taken to
			//! point AWAY from the surface.
			static ThreadTerms ComputeThreadTerms(
				const ThreadParams& t,
				const Vector3& n,
				const Vector3& wi,			///< the sampled / light direction
				const Vector3& wo			///< the view direction
				);

			//! The surface-vs-volume lobe selection weight for family
			//! `t` at view direction `wo` (Zhu 2024 5.1's
			//! attenuation-proportional pmf).  A cheap closed-form
			//! estimate of the two lobes' directional-albedo split:
			//!
			//!     w = F / ( F + (1-F) * max3(A) )
			//!
			//! clamped into [kMinLobeWeight, 1-kMinLobeWeight] so BOTH
			//! branches stay reachable.  The clamp costs variance and
			//! never bias: `Scatter` reprices every sample against the
			//! FULL mixture density, so `w` steers effort only -- the
			//! same argument `FabricBRDF::SheenSelectWeight` documents.
			//! The floor matters here in a way it does not there: a
			//! dielectric's F is ~0.04 near normal incidence while its
			//! surface lobe is the NARROWEST thing in the model, so an
			//! unclamped weight would starve exactly the lobe whose
			//! variance dominates.
			static Scalar SurfaceSelectWeight( const ThreadParams& t, const Vector3& n, const Vector3& wo );
			static const Scalar kMinLobeWeight;		///< 0.15

			//! `value` / `valueNM` with the parameters already resolved.
			//! `WeaveSPF::ScatterImpl` needs the value AND the density at
			//! the same shading point and must not re-read eighteen
			//! painters to get them -- `FabricBRDF`'s own
			//! `ValueWithParams` note, with more painters at stake.
			RISEPel ValueWithParams( const Vector3& vLightIn,
			                         const RayIntersectionGeometric& ri,
			                         const WeaveParams& p ) const;
			Scalar  ValueNMWithParams( const Vector3& vLightIn,
			                           const RayIntersectionGeometric& ri,
			                           const Scalar nm,
			                           const WeaveParams& p ) const;

			inline WeavePatternKind GetPattern() const { return pattern; }

			//! Read-back for the interactive editor / snapshot clone.
			inline const IScalarPainter& GetWeaveScale()    const { return *pScale; }
			inline const IScalarPainter& GetWeaveRotation() const { return *pRotation; }
			inline const IScalarPainter& GetWeftSkew()      const { return *pSkew; }
			inline const IScalarPainter* GetCoverage()      const { return pCoverage; }
			inline const IScalarPainter& GetGap()           const { return *pGap; }
			inline const IPainter&       GetWarpColor()     const { return *pWarpColor; }
			inline const IScalarPainter& GetWarpIOR()       const { return *pWarpIOR; }
			inline const IScalarPainter& GetWarpWidth()     const { return *pWarpWidth; }
			inline const IScalarPainter& GetWarpAzimuth()   const { return *pWarpAzimuth; }
			inline const IScalarPainter& GetWarpKd()        const { return *pWarpKd; }
			inline const IScalarPainter& GetWarpTilt()      const { return *pWarpTilt; }
			inline const IPainter&       GetWeftColor()     const { return *pWeftColor; }
			inline const IScalarPainter& GetWeftIOR()       const { return *pWeftIOR; }
			inline const IScalarPainter& GetWeftWidth()     const { return *pWeftWidth; }
			inline const IScalarPainter& GetWeftAzimuth()   const { return *pWeftAzimuth; }
			inline const IScalarPainter& GetWeftKd()        const { return *pWeftKd; }
			inline const IScalarPainter& GetWeftTilt()      const { return *pWeftTilt; }

			//! Rebind for the interactive editor's MaterialIntrospection.
			//! Like `FabricBRDF` -- and UNLIKE the GGX / Sheen triads --
			//! these need no matching forwarder into the SPF: `WeaveSPF`
			//! holds a reference to THIS object and reads every parameter
			//! back through `ResolveWeave`, so there is only one copy of
			//! the state.  addref-before-release throughout, so a
			//! self-rebind cannot destroy the painter mid-swap.
			void SetWeaveScale( const IScalarPainter& v );
			void SetWeaveRotation( const IScalarPainter& v );
			void SetWeftSkew( const IScalarPainter& v );
			void SetCoverage( const IScalarPainter& v );
			void SetGap( const IScalarPainter& v );
			void SetWarpColor( const IPainter& v );
			void SetWarpIOR( const IScalarPainter& v );
			void SetWarpWidth( const IScalarPainter& v );
			void SetWarpAzimuth( const IScalarPainter& v );
			void SetWarpKd( const IScalarPainter& v );
			void SetWarpTilt( const IScalarPainter& v );
			void SetWeftColor( const IPainter& v );
			void SetWeftIOR( const IScalarPainter& v );
			void SetWeftWidth( const IScalarPainter& v );
			void SetWeftAzimuth( const IScalarPainter& v );
			void SetWeftKd( const IScalarPainter& v );
			void SetWeftTilt( const IScalarPainter& v );

		protected:
			virtual ~WeaveBRDF();

			WeavePatternKind		pattern;

			const IScalarPainter*	pScale;
			const IScalarPainter*	pRotation;
			const IScalarPainter*	pSkew;
			const IScalarPainter*	pCoverage;		///< may be NULL
			const IScalarPainter*	pGap;

			const IPainter*			pWarpColor;
			const IScalarPainter*	pWarpIOR;
			const IScalarPainter*	pWarpWidth;
			const IScalarPainter*	pWarpAzimuth;
			const IScalarPainter*	pWarpKd;
			const IScalarPainter*	pWarpTilt;

			const IPainter*			pWeftColor;
			const IScalarPainter*	pWeftIOR;
			const IScalarPainter*	pWeftWidth;
			const IScalarPainter*	pWeftAzimuth;
			const IScalarPainter*	pWeftKd;
			const IScalarPainter*	pWeftTilt;
		};
	}
}

#endif
