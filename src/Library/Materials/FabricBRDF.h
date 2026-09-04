//////////////////////////////////////////////////////////////////////
//
//  FabricBRDF.h - The CLOSED-FORM combined response for
//    `fabric_material`: an energy-compensated Charlie sheen lobe over
//    a restricted substrate (docs/CLOTH_FABRIC_DESIGN.md 9.2).
//
//  THE MODEL (9.2 as amended by round 5, 2026-09-02): a Kulla-Conty
//  multiple-bounce coupling between the fuzz layer and the substrate.
//
//    f(l, v) = sheenColor * D_Charlie(a, n.h) * V_Charlie(a, n.l, n.v)
//            + f_base(l, v) * scale(l, v)
//
//    scale(l, v) = (1 - m*E(a, n.v)) * (1 - m*E(a, n.l))
//                  ---------------------------------------
//                            (1 - m*Ebar(a))
//
//  with  m = max3(sheenColor),  E  the BAKED directional albedo of the
//  bare Charlie lobe and  Ebar  its hemispherical mean
//  (SheenDirectionalAlbedo.h).  `scale` is symmetric under an l/v swap
//  -- the numerator's two arms simply exchange -- so the BRDF is
//  reciprocal (IBSDF.h:82-135; SPFBSDFConsistencyTest Part E is the
//  guard).
//
//  WHY THIS FORM AND NOT THE OTHER TWO.  Round 4 of the design doc
//  specified the glTF KHR_materials_sheen scaling with the two arms
//  combined by a `min`.  That was MEASURED at implementation time and
//  the measurement rejected it:
//
//    * glTF's OWN form is the single arm `1 - m*E(a, n.v)`.  It
//      conserves energy EXACTLY for a white Lambertian base
//      (rho = m*E(v) + (1 - m*E(v)) = 1) but it is NOT RECIPROCAL --
//      f(a->b) carries E(b) while f(b->a) carries E(a).
//    * `min(1 - m*E(v), 1 - m*E(l))` restores reciprocity but destroys
//      the energy identity: near normal incidence E(v) -> 0 while the
//      min still picks `1 - m*E(l)` for every l, so the base loses
//      Ebar's worth of energy that the sheen lobe never returns.
//      Measured on a white Lambertian base at m = 1: rho = 0.863 at
//      normal incidence for alpha = 0.5, against a required 1.000.
//
//  The PRODUCT form above is both.  It is the closed form of the
//  adding-doubling inter-reflection series between a lossless fuzz
//  layer and the base -- energy the fuzz intercepts on the way in is
//  not deleted, it is re-scattered onto the substrate -- and the
//  `1/(1 - m*EhatMean)` denominator is exactly that series' sum.  For a
//  white Lambertian base at ANY m and ANY alpha, wherever N == 1:
//
//    rho(v) = m*E(v) + (1 - m*E(v)) * (1 - m*Ehat)/(1 - m*Ehat) = 1
//
//  exactly.  tests/LayeredWhiteFurnaceTest.cpp's Lambertian fabric rows
//  are kPosturePass on the strength of that identity, not on a locked
//  curve.
//
//  THE DENOMINATOR BRIGHTENS THE BASE AWAY FROM GRAZING, AND THAT IS
//  THE POINT, NOT A BUG.  `scale` exceeds 1 wherever both directions
//  are far enough from the sheen lobe's grazing peak, because that is
//  where the intercepted energy is re-emitted.  Measured against the
//  SHIPPED tables (m = 1):
//
//      alpha   EhatMean   sup scale = 1/(1 - EhatMean)
//      0.08    0.07474    1.0808
//      0.20    0.13003    1.1495
//      0.50    0.22981    1.2983
//      1.00    0.30263    1.4339
//
//  (These moved slightly on 2026-09-02 when the E table's cosTheta axis
//  was warped toward grazing: the band the old uniform axis could not
//  resolve carries real weight in the mean.)
//
//  The supremum is approached only in the mu -> 0 corner where E -> 0
//  and the cosine weight vanishes.  It is exactly balanced by the
//  darkening at grazing -- the furnace's Lambertian rows landing on
//  1.000, INCLUDING its grazing check, is what proves the balance is
//  exact rather than merely plausible.
//
//  THE ENERGY BOUND NEEDS TWO THINGS, NOT ONE, AND THIS WAS A REAL BUG.
//
//  Estevez & Kulla's Charlie+Lambda fit is production-friendly, not
//  tightly energy-conserving: E exceeds 1 near grazing.  Until
//  2026-09-02 this header claimed the 0.04 roughness floor alone put
//  "every reachable cell at E <= 1".  That was true of the CELLS and
//  false of the LOBE -- the E table's cosTheta axis was uniform, so its
//  first interior node sat at mu = 0.0323 and the whole grazing band
//  where the lobe peaks was interpolated as a linear ramp from zero.
//  Since `value()` EMITS the true lobe while suppressing the base by
//  the TABLED E, the white-furnace identity broke by up to +1.05
//  ABSOLUTE (rho ~ 2.05) under grazing illumination, at every
//  roughness, and the furnace could not see it because its incident
//  angles stopped at 80 deg.  Both halves of the fix matter:
//
//    * the table's cosTheta axis is now WARPED toward grazing
//      (SheenDirectionalAlbedo.h), so E is resolved where the lobe
//      actually lives;
//    * and because the RESOLVED lobe still reaches E = 1.152 above the
//      floor, the symmetric normaliser `N` above bounds what the floor
//      cannot.
//
//  THE ROUGHNESS FLOOR IS 0.04, NOT sheen_material's 1e-3 -- and its
//  CRITERION is:
//
//      kMinSheenAlpha is the smallest alpha for which
//          max over mu >= 0.03 of E(alpha, mu)  <=  1.
//
//  It is NOT "max over ALL mu", which no alpha satisfies once the
//  grazing band is resolved.  Below mu = 0.03 the normaliser bounds the
//  energy; at and above it the product form must be exactly conserving,
//  which needs E <= 1.
//
//  tools/SheenDirectionalAlbedoGen.cpp prints the scan ("kMinSheenAlpha
//  scan") on every bake.  On the warped table the smallest baked alpha
//  meeting the criterion is 0.028289, with the next node up at 0.035350
//  (max E 0.928) -- so 0.04 clears it with BOTH bilinear-bracketing rows
//  already under 1, and the floor did not have to move when the axis was
//  re-baked.  Re-run that scan after any change to CharlieSheen or to
//  the bake extents; do not carry this number forward on trust.
//
//  EXACTNESS CLASS -- three bands, with the WORST case in each.
//
//  Fabric is NEARLY energy-conserving for n.v >= 0.0349 and
//  energy-BOUNDED below that.  Measured on a white Lambertian base at
//  m = 1, tint = 1, by deterministic quadrature over the WHOLE reachable
//  domain (alpha in [0.04, 1] x mu, warped grid):
//
//    n.v >= 0.0349          rho <= 1.0064  (+0.64 %)  at alpha 0.908
//    1/961 <= n.v < 0.0349  rho <= 1.0167  (+1.67 %)  at alpha 0.908,
//                                                     n.v ~ 0.0023
//    n.v < 1/961            rho <= 1.0067  (+0.67 %)  at alpha 0.908,
//                                                     n.v -> mu1
//
//  Global maximum anywhere: rho = 1.0166.  Nothing reaches 1.02.
//
//  EVERY ONE OF THESE NUMBERS WAS PREVIOUSLY UNDERSTATED, and the reason
//  is worth keeping.  Earlier revisions quoted 0.06 % / 1.1 % / "rho <=
//  1", all measured at ALPHA = 0.04 -- the roughness floor, where the
//  presets live.  The worst case is not there.  It is at alpha ~ 0.9,
//  in the LAST and widest log-alpha cell (0.800 -> 1.000), where E at
//  node 1 stops being monotone in alpha: it is CONCAVE with a peak near
//  0.9, so the log-alpha chord between the two bracketing rows
//  UNDER-reads the true lobe by up to 0.0067.  That under-read is what
//  drives all three bands' worst cases -- the lobe is emitted at its
//  true strength while the normaliser and the base suppression are
//  computed from the lower, interpolated value.
//
//  So: measure over the alpha range too, not just over mu.  A number
//  taken at the roughness floor is not the worst case for this model.
//
//  The middle band's residual is the E table's interpolation error
//  BETWEEN nodes; the bottom band is the floored-domain regime
//  (SheenDirectionalAlbedo.h's "THE DOMAIN IS [mu1, 1]").  Closing
//  either would mean more table resolution -- an alpha node near 0.9,
//  or more cosTheta nodes -- not an algebra change.  docs/
//  CLOTH_FABRIC_DESIGN.md 15 debt 18 tracks it.
//
//  Every preset in FabricPresets.h is above this floor -- velvet's 0.08
//  is the tightest.  `sheen_material`'s own 1e-3 floor is DELIBERATELY
//  LEFT ALONE (it has no E table to keep consistent and changing it
//  would move every existing sheen render); that asymmetry is a
//  recorded debt, not an oversight.
//
//  THE WEAVE ROTATION IS THE SUBSTRATE'S, NOT THE LOBE'S (9.5).
//  `weave_rotation` is an angle field in radians.  This BRDF hands the
//  substrate a hit record whose ONB has been rotated about `w` by that
//  angle (`MicrofacetUtils::RotateTangent`, the same helper GGX's own
//  `tangent_rotation` uses, so the two simply ADD: weave first, then
//  the substrate's own).  The Charlie lobe ignores it entirely, because
//  it is isotropic -- 9.5 gives four independent reasons why an
//  elliptical Charlie was withdrawn, the sharpest being that its `D`
//  normaliser and its Lambda visibility are both isotropic-only fits.
//
//  The rotated record must reach `value`, `valueNM`, `Pdf`, `PdfNM`,
//  `Scatter` and `ScatterNM` IDENTICALLY, or the sampler and the
//  density disagree about which frame they are in -- exactly the
//  Scatter<->Pdf mismatch SPFPdfConsistencyTest exists to catch.
//  `WeaveRotatedRI` below is the single mechanism all six use.
//
//  `hemisphericalAlbedo` is the deliberate EXCEPTION: it is
//  view-independent AND frame-independent (a rotation about `w` cannot
//  change a bihemispherical average), so it passes the caller's own
//  record and pays no copy.
//
//  ============================================================
//  TRANSMISSION THROUGH THE FUZZ LAYER (R8 P1.1,
//  docs/CLOTH_FABRIC_DESIGN.md 15 debt 22)
//  ============================================================
//
//  A `weave_material` substrate under `transmission thin` carries two
//  BELOW-HORIZON lobes (a delta gap pass-through and a Lambertian
//  back-face lobe, WeaveBRDF.h section 2a).  Until 2026-09-04 this BRDF
//  returned 0 for every opposite-hemisphere (l, v) pair, so wrapping a
//  sheer curtain in a sheen layer made it 100 % OPAQUE -- silently.
//  The fix is to FORWARD the substrate's transmission and MODULATE it:
//
//      f(l, v) = f_base(l, v) * scale(l, v)        for  (n.l)(n.v) < 0
//
//      scale(l, v) = (1 - m*Ehat(a, |n.v|)) * (1 - m*Ehat(a, |n.l|))
//                    -----------------------------------------------
//                              (1 - m*EhatMean(a))
//
//  -- the SAME Kulla-Conty product law the reflect branch uses, with
//  |n.l| in place of n.l, and NO sheen term (the Charlie lobe is
//  reflection-only: it has no transmission of its own to add).
//
//  WHY BOTH ARMS, not one.  `FabricBRDF` is TWO-SIDED: `RayFacingNormal`
//  flips the shading normal to whichever side the ray arrived on, so the
//  fuzz layer exists on BOTH faces of the cloth.  A transmitted path
//  therefore crosses a fuzz layer TWICE -- once entering on the light's
//  side, once leaving on the view's -- and each crossing costs the same
//  single-arm factor `1 - m*Ehat` the reflect branch charges per
//  direction.  A one-armed form would be non-reciprocal for exactly the
//  reason the banner's glTF discussion gives, and reciprocity across the
//  surface is measured (SPFBSDFConsistencyTest Part E2).
//
//  WHY THE SAME `1/(1 - m*EhatMean)` NORMALISER.  It is the sum of the
//  adding-doubling series between the fuzz layer and the substrate, and
//  that series is a property of the LAYER PAIR, not of which exit the
//  light eventually takes: energy the fuzz intercepts is re-scattered
//  onto the substrate, which then re-splits it between its own reflect
//  and transmit lobes in the substrate's own proportion.  Using a
//  different (or no) normaliser on the transmit branch would make the
//  two exits recycle at different rates from one interception.
//
//  THE ENERGY CLAIM THIS IMPLIES, stated as a CLOSED FORM so it can be
//  checked rather than assumed.  The weave's diffuse transmission lobe
//  is Lambertian-shaped -- `f_t = T/pi`, constant in l (WeaveBRDF.h
//  section 2a) -- so the wrapper's transmitted DIRECTIONAL share is
//
//    INT_below f_t * scale(v,l) |n.l| dl
//      = (T/pi) * (1 - m*Ehat(v))/(1 - m*Ebar) * INT (1-m*Ehat(mu)) mu dw
//      = (T/pi) * (1 - m*Ehat(v))/(1 - m*Ebar) * pi * (1 - m*Ebar)
//      = T * (1 - m*Ehat(v))
//
//  using `(1/pi) INT Ehat(mu) mu dw == Ebar` -- the SAME identity
//  `hemisphericalAlbedo`'s derivation uses twice.  The recycling
//  denominator CANCELS EXACTLY, and what is left is the single view-side
//  arm, which is <= 1.  So:
//
//    * the transmitted share is the BARE weave's times `1 - m*Ehat(n.v)`
//      -- an attenuation, never a gain, at every view angle;
//    * it is > 0 wherever the bare weave's is;
//    * and the total (reflect + transmit) stays under the furnace's 1.05
//      ceiling.
//
//  LayeredWhiteFurnaceTest rows 52/53 assert all three against that
//  closed form rather than against a locked curve.  (The DELTA gap lobe
//  is priced separately and deliberately differently -- see
//  FabricSPF.cpp: a measure-zero direction receives none of the
//  recycled series, so it carries the two arms and NOT the denominator,
//  giving `gap * (1 - m*Ehat(n.v))^2`.)
//
//  A SUBSTRATE THAT DOES NOT TRANSMIT IS BIT-IDENTICAL.  The branch is
//  gated on `BaseScattersFullSphere()`, captured at construction from
//  `IMaterial::ScattersFullSphere()` (the flag cannot change afterwards:
//  a weave's `transmission` enum is explicitly NOT rebindable, and
//  `base` is not rebindable on this material either).  With the flag
//  false the opposite-hemisphere early-out is the committed one, so
//  every Lambertian / Oren-Nayar / GGX / `transmission none` weave stack
//  answers exactly as before -- pinned, absolutely, by
//  FabricMaterialChunkTest's `TestReflectionOnlyUnchanged` value lock.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FABRIC_BRDF_
#define FABRIC_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"
#include "../Utilities/MicrofacetUtils.h"
#include <cmath>
#include <optional>

namespace RISE
{
	namespace Implementation
	{
		//! The weave-rotated hit record the SUBSTRATE is evaluated
		//! against (docs/CLOTH_FABRIC_DESIGN.md 9.5).
		//!
		//! `Get()` returns the CALLER'S OWN record when the angle is a
		//! no-op -- the overwhelmingly common case, since an unset
		//! `weave_rotation` resolves to 0 -- so the ~0.5 KB
		//! RayIntersectionGeometric copy is paid only when there is
		//! genuinely a rotation to apply.  The threshold matches
		//! `MicrofacetUtils::RotateTangent`'s own fast-path epsilon, so
		//! "rotate the frame" and "copy the record" can never disagree
		//! about whether a rotation happened.
		//!
		//! The copy is REQUIRED, not an optimisation detail:
		//! `RayIntersectionGeometric` is the shared hit record and the
		//! scene is immutable during the parallel pass, so the rotation
		//! must never be written back into the caller's object.
		class WeaveRotatedRI
		{
		public:
			WeaveRotatedRI( const RayIntersectionGeometric& ri, const Scalar angle ) :
			  pRef( &ri )
			{
				if( std::fabs( angle ) >= Scalar( 1e-9 ) ) {
					storage.emplace( ri );
					storage->onb = MicrofacetUtils::RotateTangent( ri.onb, angle );
					pRef = &( *storage );
				}
			}

			//! NON-COPYABLE, deliberately.  The implicit copy ctor would
			//! copy `storage` but leave `pRef` pointing into the SOURCE
			//! object's `optional`, so a copy that outlived its source --
			//! or merely a source that moved -- would hand `Get()` a
			//! dangling reference.  Every use today is a named local, so
			//! this was latent rather than live; deleting the operations
			//! makes it impossible rather than merely unexercised, which
			//! is the right disposition for a header-public class whose
			//! next caller may well write `auto w = ...`.
			WeaveRotatedRI( const WeaveRotatedRI& ) = delete;
			WeaveRotatedRI& operator=( const WeaveRotatedRI& ) = delete;

			inline const RayIntersectionGeometric& Get() const { return *pRef; }

		private:
			std::optional<RayIntersectionGeometric>	storage;
			const RayIntersectionGeometric*			pRef;
		};

		class FabricBRDF :
			public virtual IBSDF,
			public virtual Reference
		{
		public:
			//! Sheen roughness clamp.  See the header banner for the
			//! measurement that pins the lower bound.
			static const Scalar kMinSheenAlpha;		///< 0.04
			static const Scalar kMaxSheenAlpha;		///< 1.0

			//! Fabric parameters resolved at ONE shading point in ONE
			//! colour regime.  Shared by FabricBRDF and FabricSPF so the
			//! sampler's alpha / selection weight can never drift from
			//! the evaluator's.
			struct FabricParams
			{
				Scalar	alpha;			///< sheen roughness, clamped to [kMinSheenAlpha, kMaxSheenAlpha]
				Scalar	m;				///< max3(sheenColor), clamped to [0,1] -- the ACHROMATIC energy split
				RISEPel	tint;			///< sheen colour, RGB regime (undefined on the NM path)
				Scalar	tintNM;			///< sheen colour at the hero wavelength (undefined on the RGB path)
				Scalar	weaveAngle;		///< radians, for the substrate's frame
			};

			FabricBRDF(
				const IBSDF& base,					///< [in] Substrate BSDF (allowlisted -- see FabricMaterial)
				const IPainter& sheenColor,
				const IScalarPainter& sheenRoughness,
				const IScalarPainter& weaveRotation,
				//! [in] The substrate material's `ScattersFullSphere()`.
				//! Passed in rather than derived because `IBSDF` carries
				//! no such flag -- it is an `IMaterial` property, and
				//! `FabricMaterial` is the only thing that holds both.
				//! Captured once: the only substrate that can set it is a
				//! `weave_material`, whose `transmission` enum is not
				//! rebindable, and `base` is not rebindable here either.
				const bool baseScattersFullSphere
				);

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar  valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;
			virtual bool    hemisphericalAlbedo( const RayIntersectionGeometric& ri, RISEPel& out ) const;
			virtual bool    hemisphericalAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm, Scalar& out ) const;

			//! Resolve the fabric parameters at `ri`.  `nm < 0` selects
			//! the RGB regime, `nm >= 0` the spectral one -- but note
			//! that only the TINT actually differs between them.
			//!
			//! `alpha`, `m` AND `weaveAngle` are all read through the
			//! ACHROMATIC (RGB) path in both regimes, deliberately.  The
			//! `IScalarPainter` interface permits a wavelength-varying
			//! scalar (`GetValueAtNM`), and `requireSingle` on these
			//! slots rejects PER-CHANNEL painters, not wavelength-varying
			//! ones -- a scalar painter backed by a grey colour source
			//! has `HasPerChannelVariation() == false` yet a non-flat
			//! `GetValueAtNM`.  Fabric does not use that freedom for
			//! these two, for the same reason `m` does not use it:
			//!
			//!   * `alpha` feeds `E` -> `SheenSelectWeight` -> the
			//!     mixture density, so a wavelength-dependent alpha would
			//!     put `Scatter`'s STORED hero-wavelength pdf out of step
			//!     with a companion-wavelength `Pdf()` call -- the exact
			//!     Scatter<->Pdf mismatch gate 6 exists to catch;
			//!   * `weaveAngle` selects the FRAME the substrate's `Pdf`
			//!     is evaluated in, so a wavelength-dependent angle would
			//!     mean the sampler and the density disagree about the
			//!     tangent basis itself.
			//!
			//! `CoatedBRDF::ResolveCoat` reads its scalars per-wavelength
			//! and is the precedent, but that precedent predates this
			//! reasoning; fabric departs from it knowingly.
			//!
			//! `m` is taken from the RGB max3 in BOTH regimes, ON
			//! PURPOSE: the base-scaling factor and the lobe-selection
			//! weight are ACHROMATIC quantities (E depends on roughness
			//! and geometry only), so deriving `m` from a per-wavelength
			//! tint would make the substrate's suppression -- and
			//! therefore the mixture density -- wavelength-dependent for
			//! a coloured dye.  That would put `Scatter`'s hero-wavelength
			//! pdf out of step with a companion-wavelength `Pdf()` call
			//! and, worse, make the RGB and spectral pipes disagree about
			//! how much energy the base returns.  One achromatic split,
			//! both pipes.
			void ResolveFabric( const RayIntersectionGeometric& ri, const Scalar nm, FabricParams& out ) const;

			//! ONE ARM of the product form: `1 - m*Ehat(alpha, cosTheta)`
			//! with `Ehat = min(E, 1)` -- the fraction of energy that
			//! reaches (or leaves) the substrate through the fuzz layer
			//! in ONE direction.
			//!
			//! Also the closed form of `albedo`'s base factor -- see the
			//! derivation at the definition: the `1/(1 - m*EhatMean)` in
			//! the joint form CANCELS against the l-integral of the other
			//! arm, so a DIRECTIONAL quantity carries the single arm and
			//! nothing else.
			static Scalar SheenTransmit( const Scalar alpha, const Scalar m, const Scalar cosTheta );

			//! The full Kulla-Conty base scaling of the banner:
			//!
			//!   (1 - m*Ehat(a, cosV)) * (1 - m*Ehat(a, cosL))
			//!   ---------------------------------------------
			//!               (1 - m*EhatMean(a))
			//!
			//! ONE definition, used by `value` and `valueNM`.
			//!
			//! Symmetric in cosV / cosL by inspection, which is the
			//! reciprocity argument.
			//!
			//! NOTE that tests deliberately do NOT call this: gate 3's
			//! furnace prediction and gate 5b's brute-force quadrature
			//! both re-derive the kernel from `SheenDirectionalAlbedo`
			//! directly, so a self-consistent error inside this function
			//! cannot hide by appearing on both sides of the comparison
			//! (M3 review, 2026-09-02).
			static Scalar BaseScaling( const Scalar alpha, const Scalar m,
			                           const Scalar cosThetaV, const Scalar cosThetaL );

			//! The SYMMETRIC energy normaliser `max(1, E(v), E(l))`,
			//! evaluated on the RAW (unclamped) E.  Divides the sheen
			//! lobe so the total stays bounded inside the near-grazing
			//! sliver where E > 1; it is exactly 1 -- and therefore
			//! bit-identically absent -- everywhere else.  See the
			//! banner for why a symmetric form is required (an
			//! asymmetric one would break reciprocity, which is the
			//! whole reason the `min` form was adopted and then
			//! replaced).
			static Scalar SheenNormaliser( const Scalar alpha,
			                               const Scalar cosThetaV, const Scalar cosThetaL );

			//! The hemispherical mean of `SheenTransmit`, i.e.
			//! `1 - m*EhatMean(alpha)` -- the base factor in
			//! `hemisphericalAlbedo`'s closed form.  Retired the baked
			//! 2-D S(alpha, m) kernel table that round 4's `min` form
			//! needed (a `min` does not factor; a product does).
			static Scalar SheenTransmitMean( const Scalar alpha, const Scalar m );

			//! The sheen lobe's SELECTION weight / directional-albedo
			//! share, `m * Ehat(alpha, n.v)`, clamped to [0,1].
			//! FabricSPF uses it for both the branch pick and the mixture
			//! density, which is what makes the two agree by
			//! construction.  Uses `Ehat`, not raw E, so the weight
			//! cannot exceed 1 inside the grazing sliver.
			static Scalar SheenSelectWeight( const Scalar alpha, const Scalar m, const Scalar cosThetaV );

			//! The two lobe terms at one (l, v) pair, with the sheen's
			//! normaliser already folded in.  Colour-free: everything
			//! here depends on geometry and roughness only, so ONE body
			//! serves both the RGB and the spectral pipe and the two
			//! cannot drift (docs/skills/audit-by-bug-pattern.md's
			//! RGB/NM-twin discipline).
			struct FabricTerms
			{
				bool	valid;		///< false => the direction pair is gated off; both terms are 0
				//! For a TRANSMISSION pair (`l` and `v` on opposite
				//! sides of the shading normal -- only reachable when the
				//! substrate reports `ScattersFullSphere()`) `sheen` is
				//! 0 -- the Charlie lobe is reflection-only and has no
				//! transmission to contribute -- and `scaling` carries
				//! the two-crossing attenuation the header derives.  No
				//! flag marks the case and the two consumers
				//! (`ValueWithParams` / `ValueNMWithParams`) never branch
				//! on it: `tint * sheen + f_base * scaling` is the right
				//! sum on both sides of the surface precisely because
				//! `sheen` is structurally 0 below the horizon.
				Scalar	sheen;		///< D * V / max(1, E(v), E(l))
				Scalar	scaling;	///< the base scaling factor
			};

			//! Resolve the geometry + both lobe factors at `vLightIn`.
			//! Shared by `value`, `valueNM` and their `*WithParams`
			//! forms.
			FabricTerms ComputeTerms( const Vector3& vLightIn,
			                          const RayIntersectionGeometric& ri,
			                          const FabricParams& p ) const;

			//! `value` / `valueNM` with the parameters and the
			//! weave-rotated record ALREADY resolved by the caller.
			//!
			//! WHY THESE EXIST.  `FabricSPF::ScatterImpl` needs the
			//! parameters, the rotated record, the mixture pdf and the
			//! BRDF value for the SAME shading point; going through the
			//! public entry points re-resolved three painters and rebuilt
			//! a ~600-byte `RayIntersectionGeometric` three to four times
			//! per `Scatter` call, on the renderer's hottest path and
			//! precisely for the scenes that set `weave_rotation` (the
			//! denim / silk / satin presets).  Correctness was never
			//! affected -- painters are deterministic -- but it was ~4x
			//! the layered-material overhead `CoatedSPF` pays.
			RISEPel ValueWithParams( const Vector3& vLightIn,
			                         const RayIntersectionGeometric& ri,
			                         const FabricParams& p,
			                         const RayIntersectionGeometric& weaveRi ) const;
			Scalar  ValueNMWithParams( const Vector3& vLightIn,
			                           const RayIntersectionGeometric& ri,
			                           const Scalar nm,
			                           const FabricParams& p,
			                           const RayIntersectionGeometric& weaveRi ) const;

			//! The substrate as seen by the layer.  FabricSPF needs it to
			//! evaluate the full mixture after sampling either branch.
			inline const IBSDF& GetBase() const { return *pBase; }

			//! Does the SUBSTRATE scatter over the full sphere (R8 P1.1)?
			//! `FabricSPF` reads it back through here so the sampler and
			//! the evaluator are gated on literally the same bit -- one
			//! copy of the state, `FabricSPF`'s standing contract.
			inline bool BaseScattersFullSphere() const { return bBaseFullSphere; }

			//! Read-back for the interactive editor / snapshot clone.
			inline const IPainter&       GetSheenColor()     const { return *pSheenColor; }
			inline const IScalarPainter& GetSheenRoughness() const { return *pSheenRoughness; }
			inline const IScalarPainter& GetWeaveRotation()  const { return *pWeaveRotation; }

			//! Rebind for the interactive editor's MaterialIntrospection.
			//! Like CoatedBRDF -- and UNLIKE the GGX / Sheen triads --
			//! these need no matching forwarder into the SPF: FabricSPF
			//! holds a reference to THIS object and reads every fabric
			//! parameter back through `ResolveFabric`, so evaluator and
			//! sampler cannot drift; there is only one copy of the state.
			//!
			//! addref-before-release throughout, so a self-rebind
			//! (Set(X) where X is already bound) cannot destroy the
			//! painter mid-swap -- LambertianBRDF::SetReflectance's
			//! documented contract.
			void SetSheenColor( const IPainter& v );
			void SetSheenRoughness( const IScalarPainter& v );
			void SetWeaveRotation( const IScalarPainter& v );

		protected:
			virtual ~FabricBRDF();

			const IBSDF*			pBase;
			const IPainter*			pSheenColor;
			const IScalarPainter*	pSheenRoughness;
			const IScalarPainter*	pWeaveRotation;
			bool					bBaseFullSphere;	///< see the ctor parameter and the header's transmission section
		};
	}
}

#endif
