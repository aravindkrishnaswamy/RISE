//////////////////////////////////////////////////////////////////////
//
//  WeaveSPF.h - Importance sampler for `weave_material`
//    (docs/CLOTH_FABRIC_DESIGN.md Phase 2 slice P2-A; Zhu et al. 2024
//    section 5.1's attenuation-proportional lobe-selection sampler).
//
//  ============================================================
//  THE ESTIMATOR
//  ============================================================
//
//  Zhu 2024 5.1, quoted from the primary source (P2_ZHU_READ 2.2):
//  compute an attenuation `A(p)` per lobe, select a lobe with pmf
//  `p_a = A(p) / SUM A(p)`, importance-sample the selected lobe for its
//  own density `p_l`, and return `w = f_p / (p_a p_l)`.  This material
//  has FOUR lobes -- {warp, weft} x {surface, volume} -- and the
//  selection is factored into two independent draws, which is the same
//  pmf written in two steps:
//
//      p_a(k, lobe)  =  ahat_k  *  ( w_k  or  1 - w_k )
//
//  `ahat_warp` is the weave's warp-coverage field and `ahat_weft` its
//  complement, so the FAMILY is chosen by which yarn is actually on top
//  at this point -- an attenuation in the most literal sense.  `w_k` is
//  `WeaveBRDF::SurfaceSelectWeight`, the Fresnel-vs-dye split.
//
//  RISE ADDS ONE THING TO ZHU'S RECIPE AND IT IS NOT OPTIONAL HERE: the
//  sample is REPRICED AGAINST THE FULL MIXTURE, not against the lobe
//  that drew it.  The emitted weight and density are
//
//      q(wo)  =  SUM_k ahat_k [ w_k p_surf,k(wo) + (1 - w_k) cos(wo)/pi ]
//      kray   =  f_weave(wi, wo) cos(wo) / q(wo)
//      pdf    =  q(wo)
//
//  Zhu's `f_p / (p_a p_l)` is the same estimator ONLY when the lobes
//  have disjoint support; these four overlap everywhere, and a
//  `Scatter` that reported its own branch's density would disagree with
//  an independent `Pdf(ri, wo)` call for the same direction -- exactly
//  the Scatter<->Pdf mismatch tests/SPFPdfConsistencyTest.cpp exists to
//  catch.  This is `FabricSPF`'s sample-then-reprice rule, and it is
//  the same rule for the same reason.  NEITHER LOBE IS DELTA, so the
//  delta convention (divide by the selection probability alone, report
//  pdf 0, route MIS through `GetSpecularInfo`) does not apply.
//
//  ============================================================
//  THE SURFACE BRANCH -- AND WHY ITS DENSITY IS EXACT
//  ============================================================
//
//  The surface lobe is sampled in the selected family's FIBRE FRAME, in
//  two independent pieces:
//
//    * LONGITUDINAL: d'Eon's exact inverse for `Mp`, the same three
//      lines PBRT's hair sampler uses --
//          cosT = 1 + v log( u + (1-u) exp(-2/v) )
//          sinThetaI = -cosT sinThetaO + sinT cos(2 pi u') cosThetaO
//      -- whose density in theta is exactly `Mp(theta_i,theta_o;v)
//      cos(theta_i)`, and whose centre `-theta_o` is the specular cone.
//    * AZIMUTHAL: `SampleTrimmedLogistic`, the exact inverse CDF.
//
//  With `dw = cos(theta) dtheta dphi` the solid-angle density is
//  therefore `Mp * N` exactly, with no Jacobian left over.
//
//  THE ONE REAL DIFFICULTY, AND ITS ANSWER.  A fibre frame's
//  (theta, phi) parametrisation covers the whole SPHERE, so a naive
//  draw puts a large fraction of its mass BELOW the surface plane.
//  Rejecting those draws is not available: it multiplies the accepted
//  sample's density by a view-dependent acceptance factor with no
//  closed form, and section 9.4 already rejected the Charlie
//  D-sampler for precisely that -- an un-normalised `Pdf` fails
//  SPFPdfConsistencyTest's hemisphere integral (part 2) and its
//  chi-squared histogram (part 3) independently.
//
//  The answer costs nothing, because the trimmed logistic is
//  normalised over WHATEVER interval it is given and inverts over it
//  exactly.  At fibre latitude theta_i the visible azimuths are
//  |phi_i| < phi_max(theta_i), so the sampler draws `phi_d` from the
//  logistic TRIMMED TO that interval, recentred on the view's azimuth.
//  Every draw lands above the horizon and
//
//      INT_hemisphere Mp(theta_i,theta_o;v) N_trimmed(phi_d) dw
//        = [ INT Mp cos dtheta ] [ INT N_trimmed dphi ]  =  1 * 1
//
//  exactly.  At ZERO TILT, phi_max is pi/2 at every latitude and the
//  whole construction is closed-form and lossless.
//
//  UNDER A NON-ZERO TILT IT LOSES A LITTLE, AND THE AMOUNT IS
//  MEASURED, NOT ASSERTED.  Two effects, both small:
//
//    * latitudes beyond `pi/2 - |alpha|` have NO visible azimuth at all
//      (the yarn leans far enough that the whole band is buried); the
//      longitudinal draw can still land there and is then discarded.
//    * where phi_max > pi/2 the recentred interval can run past the
//      logistic's own [-pi, pi] domain and is clipped, discarding the
//      FAR TAIL (density exp(-pi/s) of the peak, ~2e-3 at s = 0.5).
//
//  tests/SPFPdfConsistencyTest.cpp measures the resulting hemisphere
//  integral on the shipped presets and the numbers are recorded in
//  docs/CLOTH_FABRIC_DESIGN.md section 10 (0.99998 / 0.99983 at
//  theta_o = 30 deg for satin / denim, 0.99997 / 0.99868 at 60 deg --
//  i.e. the trimming is lossless to within two parts in ten thousand
//  where the presets actually live).  Both effects LOSE surface-lobe
//  density rather than adding any, and the cosine branch covers every
//  direction unconditionally, so the mixture never reports density for
//  a direction it cannot draw -- which is the direction of error that
//  would actually bias the estimator.
//
//  AT A GRAZING VIEW ALONG A TILTED YARN THE SURFACE BRANCH CAN FAIL
//  OUTRIGHT, AND THAT IS CORRECT.  When the view runs nearly along a
//  tilted family's axis, the specular cone theta_i == -theta_o lands in
//  the latitude band the tilt has buried, `SurfaceAzimuthInterval`
//  reports empty, and `Scatter` emits NOTHING for that draw.  Measured
//  at theta_o = 80 deg on the shipped presets: 51 % of draws on satin
//  (tilt 0.14, view along the warp), 19 % on silk, 0 % on the untilted
//  denim and linen -- and 0.1 % or less at every angle below 60 deg,
//  which is why it does not show in the pdf harness's 30/60 rows.
//
//  IT IS VARIANCE, NOT BIAS, and the argument is the one that always
//  applies to a sampler that sometimes produces nothing: the estimator
//  is E[f cos / q ; emitted], and integrating that against the sampler's
//  own (sub-normalised) density q gives INT f cos over the support of q
//  -- which is the whole hemisphere, because the cosine branch's
//  density is positive everywhere.  What is lost is effort, not energy:
//  the buried lobe genuinely carries no visible mass at that latitude,
//  and `Pdf` reports exactly zero surface density there too, so the two
//  agree.  tests/SPFBSDFConsistencyTest.cpp's furnace rows confirm it
//  numerically -- Monte Carlo against deterministic quadrature agrees to
//  0.05-0.56 % on both weave rows.
//
//  Recovering the effort would mean falling back to the cosine branch
//  when the interval is empty, which makes the branch probability
//  depend on the DRAWN latitude and destroys the closed-form mixture
//  density this whole design is built on.  Not worth it for a case that
//  only appears above 60 deg on a tilted preset.
//
//  ============================================================
//  THE VOLUME BRANCH
//  ============================================================
//
//  Cosine-hemisphere about the ray-facing shading normal, exactly as
//  `FabricSPF`'s sheen branch and `LambertianSPF` do.  Horizon-safe,
//  integrates to 1 by construction, density exact.
//
//  IT IS NOT A PERFECT MATCH, AND THE MISMATCH IS WORTH NAMING RATHER
//  THAN LEAVING TO BE REDISCOVERED.  The volume lobe is not purely
//  diffuse: its `(1 - k_d)` share carries a second `Mp` at twice the
//  surface width, which for a flat satin float is 5 degrees wide -- a
//  genuinely narrow forward cone, and deliberately so (it is Sadeghi's
//  model of scattering inside a twisted thread).  A cosine proposal
//  under-samples that cone, so the indirect bounces pay variance for it.
//
//  MEASURED, NOT ASSUMED: on a 2048-spp direct-lit sphere at the shipped
//  satin preset the peakiness ratio max/p99 over the lit disc is 1.5 --
//  identical to the Phase-1 sheen-over-GGX shape's 1.5 on the same
//  subject -- so at the shipped parameters this produces no fireflies.
//  The reason it does not is that a narrow lobe's energy arrives
//  overwhelmingly through NEE, which evaluates `value` directly rather
//  than sampling.  A scene lit only by indirect bounces off a narrow
//  volume lobe would be the case to re-measure.
//
//  Sampling the volume lobe by its own `Mp` would fix it and is a real
//  option; it is not taken here because it would make the mixture
//  density a THREE-term one per family, and the estimator's whole
//  tractability rests on the density staying closed-form and cheap.
//  docs/CLOTH_FABRIC_DESIGN.md 10.3 records it.
//
//  ============================================================
//  OTHER CONTRACTS
//  ============================================================
//
//  GEOMETRIC-HORIZON GATE, mirrored between sampler and density, so a
//  direction `Scatter` can no longer emit carries zero density.  A
//  GlintModifier tilt can otherwise send a continuation ray into the
//  solid (SheenSPF.cpp:86-99).
//
//  LOBE BUDGET: exactly ONE ray per `Scatter` call.
//
//  NO `EvaluateKrayNM` OVERRIDE, deliberately -- CoatedSPF.h point 3's
//  argument transfers verbatim.  The ISPF default (-1) routes
//  PathTracingIntegrator to `valueNM * cos / pS->pdf`, i.e. the pdf
//  stored ON the sampled ray: the true HERO-wavelength density that
//  actually drew it.  An override could only recompute the density at
//  the COMPANION wavelength, and dividing by the wrong density is a
//  bias, not an approximation.
//
//  NO `GetSpecularInfo` OVERRIDE.  Neither lobe is delta, so SMS
//  correctly ignores this material, on hair's and fabric's precedent.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WEAVE_SPF_
#define WEAVE_SPF_

#include "../Interfaces/ISPF.h"
#include "../Utilities/Reference.h"
#include "WeaveBRDF.h"

namespace RISE
{
	namespace Implementation
	{
		class WeaveSPF : public virtual ISPF, public virtual Reference
		{
		public:
			//! @param brdf  the closed form this sampler is the importance
			//!              sampler FOR.  Held by reference (addref'd) so
			//!              `Scatter` and `value` can never disagree, and
			//!              so the editor's rebinds reach both through one
			//!              copy of the state.
			explicit WeaveSPF( const WeaveBRDF& brdf );

			void Scatter(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				ScatteredRayContainer& scattered,
				const IORStack& ior_stack
				) const;

			void ScatterNM(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				const Scalar nm,
				ScatteredRayContainer& scattered,
				const IORStack& ior_stack
				) const;

			Scalar Pdf(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const IORStack& ior_stack
				) const;

			Scalar PdfNM(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const Scalar nm,
				const IORStack& ior_stack
				) const;

			inline const WeaveBRDF& GetBRDF() const { return *pBRDF; }

		protected:
			virtual ~WeaveSPF();

			//! Shared body of Scatter / ScatterNM.  `nm < 0` selects the
			//! RGB regime.  ONE implementation is the structural defence
			//! against the RGB/NM-twin drift
			//! docs/skills/audit-by-bug-pattern.md catalogues.
			void ScatterImpl(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				const Scalar nm,
				ScatteredRayContainer& scattered,
				const IORStack& ior_stack
				) const;

			//! The mixture density with the parameters ALREADY resolved.
			//! `ScatterImpl` needs it once per emitted ray at the same
			//! shading point that produced the direction; routing that
			//! through the public entry point would re-read eighteen
			//! painters on the render's hottest path.
			static Scalar PdfWithParams(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const WeaveBRDF::WeaveParams& p
				);

			const WeaveBRDF*	pBRDF;
		};
	}
}

#endif
