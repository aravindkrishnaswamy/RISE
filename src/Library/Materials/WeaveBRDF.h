//////////////////////////////////////////////////////////////////////
//
//  WeaveBRDF.h - Structured two-thread-family (warp/weft) cloth BRDF
//  for `weave_material` (docs/CLOTH_FABRIC_DESIGN.md).
//
//  Model: Sadeghi et al. 2013 factorisation on RISE's d'Eon 2011 `Mp`
//  and `TrimmedLogistic` primitives (no per-texel ASG bake, no SGGX):
//    f(i,o) = (1 - gap) * sum_k ahat_k * M_k(i,o) * [f_surf,k + A_k * f_vol,k]
//    f_surf,k = F(eta_k, cos(theta_d) cos(phi_d/2)) * Mp(beta_k^2) * N(phi_d; s_k)
//    f_vol,k  = (1 - F) * [(1 - k_d) Mp((2 beta_k)^2) + k_d]
//               / ((cos theta_i + cos theta_o) * C_v)
//
//  Load-bearing invariants:
//  - Fibre frame: warp tangent rotates `u` by `weave_rotation`; weft adds
//    90 deg + `weft_skew`; each tilts out of plane by `float_tilt`.
//    n_k = normalize(n - (n.t)t), b_k = t x n_k, sin(theta) = w.t,
//    phi = atan2(w.b_k, w.n_k) (azimuth measured from surface normal).
//  - Sadeghi Eq. 4 (`1/cos^2(theta_d)`) is DELIBERATELY ABSENT: d'Eon's
//    `Mp` is already normalized (`int Mp cos(theta_i) dtheta_i == 1`), so
//    restoring Eq. 4 double-compensates and breaks energy conservation (> 2x).
//  - Sadeghi Eq. 11-15 reweighting `Q` is DELIBERATELY ABSENT: `Q(w_r)`
//    breaks reciprocity and a symmetrized `sqrt(Q_i Q_o)` pushes white
//    presets over the 1.05 furnace ceiling at grazing. Gap stays a
//    direction-independent energy factor `(1 - gap)` so BRDF stays
//    reciprocal and sampling PDF stays normalized.
//  - Surface lobe is untinted (dielectric); `warp_color`/`weft_color` tint
//    volume lobe only. Masking M_k uses Sadeghi Eq. 7-9 (Ashikhmin 2000
//    correlation, fixed sigma = 20 deg, reciprocal; no cross-family masking).
//  - Energy bounded by construction via symmetrized `AzimuthalTrimBoost`
//    and exact symmetrized `C_v` (kVolumeKdNormaliser = 2(4 - pi)).
//  - Thin transmission (`transmission thin`, Zhu 2023): adds delta gap
//    pass-through (`gap * delta(i+o)/(i.n)`, excluded from `Pdf()`, no SMS
//    `GetSpecularInfo`) and Lambertian back-face lobe `(1-gap)*transmit_k*T_k/pi`
//    split from the volume budget `(1-transmit_k)` reflect / `transmit_k`
//    transmit. `transmission none` is bit-identical to reflect-only.
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
			//! Clamped to 0.17 rad (~9.7 deg) so the masking coordinate pole
			//! stays at >= 80 deg view latitude (combined with `kPoleBlendWidth`
			//! and `kMaskGateHalfWidth`).
			static const Scalar kMaxTilt;
			//! Masking correlation width, Sadeghi Eq. 9's `u`.  20 deg,
			//! the middle of the paper's stated 15-25 deg range.  NOT
			//! authorable on purpose.
			static const Scalar kMaskCorrelationSigma;
			//! `ProjectDir` pole-conditioning ramp width in `h` (cosTheta, 0.10 rad);
			//! blends e1/h toward 0 near the fibre-axis pole to keep `cosPhi` smooth.
			static const Scalar kPoleBlendWidth;
			//! C1 quadratic mollification half-width for `max(cosPhi, 0)` over
			//! `[-kMaskGateHalfWidth, +kMaskGateHalfWidth]`; matches value at both
			//! ends so `mI`/`mO` stay graded cosine occlusion past the window.
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
				Scalar	transmit;	///< P2-B: k_t in [0,1]; 0 unless `transmission thin`
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
				bool			thin;		///< P2-B: `transmission thin`?  Gates every new lobe.
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
				const WeaveTransmissionKind transmission,		///< P2-B: `none` or `thin`
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
				const IScalarPainter& warpTransmit,			///< P2-B
				const IPainter& weftColor,
				const IScalarPainter& weftIOR,
				const IScalarPainter& weftWidth,
				const IScalarPainter& weftAzimuth,
				const IScalarPainter& weftKd,
				const IScalarPainter& weftTilt,
				const IScalarPainter& weftTransmit				///< P2-B
				);

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar  valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;
			virtual bool    hemisphericalAlbedo( const RayIntersectionGeometric& ri, RISEPel& out ) const;
			virtual bool    hemisphericalAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm, Scalar& out ) const;

			//! Resolves weave parameters at `ri` (`nm < 0` RGB, `nm >= 0` NM).
			//! All geometric scalars are read achromatically (`GetValuesAt().v[0]`)
			//! so hero/companion PDFs and tangent frames agree; only tints vary
			//! spectrally.
			void ResolveWeave( const RayIntersectionGeometric& ri, const Scalar nm, WeaveParams& out ) const;

			//! The clamped gap fraction `gap(x)` at @a ri -- the ONE
			//! expression `ResolveWeave` stores as `1 - available`, lifted
			//! out so DL-05's shadow-walk query (`WeaveSPF::
			//! DeltaPassThroughTransmittance`) reads the identical number
			//! without resolving the other eighteen painters.  Achromatic,
			//! like every geometric scalar above.
			Scalar ResolveGap( const RayIntersectionGeometric& ri ) const;

			//! DL-05: false only when the gap is PROVABLY zero everywhere
			//! (a uniform painter clamping to 0 -- the `silk` / `satin`
			//! presets), so the SPF can never draw its gap lobe.  Read by
			//! WeaveMaterial::HasDeltaPassThrough; kept current by the
			//! constructor and SetGap.
			inline bool GapCanOpen() const { return bGapCanOpen; }

			//! One family's fibre frame: tilted tangent `t`, normal projection `nk`
			//! (azimuth measured from normal, Sadeghi convention), `bk = t x nk`,
			//! and tilt cosines `sinAlpha`/`cosAlpha` for horizon visibility.
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

			//! Visible `phi_d = phi_i - phi_o` interval at latitude `theta_i`.
			//! Trimming the logistic to this interval keeps the surface-lobe PDF
			//! exact and normalized over the upper hemisphere without rejection.
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

			//! Surface-vs-volume selection weight for family `t` at `wo`:
			//! `F / (F + (1 - F) * max3(A))`, clamped to [kMinLobeWeight, 1 - kMinLobeWeight]
			//! so narrow dielectric surface lobes are never starved (unbiased because
			//! Scatter reprices against the full mixture).
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

			inline WeavePatternKind      GetPattern()      const { return pattern; }
			inline WeaveTransmissionKind GetTransmission() const { return transmission; }

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
			inline const IScalarPainter& GetWarpTransmit()  const { return *pWarpTransmit; }
			inline const IPainter&       GetWeftColor()     const { return *pWeftColor; }
			inline const IScalarPainter& GetWeftIOR()       const { return *pWeftIOR; }
			inline const IScalarPainter& GetWeftWidth()     const { return *pWeftWidth; }
			inline const IScalarPainter& GetWeftAzimuth()   const { return *pWeftAzimuth; }
			inline const IScalarPainter& GetWeftKd()        const { return *pWeftKd; }
			inline const IScalarPainter& GetWeftTilt()      const { return *pWeftTilt; }
			inline const IScalarPainter& GetWeftTransmit()  const { return *pWeftTransmit; }

			//! Interactive editor rebinds (addref-before-release; WeaveSPF reads
			//! shared state directly from this object, so no SPF forwarder needed).
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
			void SetWarpTransmit( const IScalarPainter& v );
			void SetWeftColor( const IPainter& v );
			void SetWeftIOR( const IScalarPainter& v );
			void SetWeftWidth( const IScalarPainter& v );
			void SetWeftAzimuth( const IScalarPainter& v );
			void SetWeftKd( const IScalarPainter& v );
			void SetWeftTilt( const IScalarPainter& v );
			void SetWeftTransmit( const IScalarPainter& v );

		protected:
			virtual ~WeaveBRDF();

			WeavePatternKind		pattern;
			WeaveTransmissionKind	transmission;	///< P2-B; NOT rebindable (re-author the chunk -- same call as `pattern`)

			const IScalarPainter*	pScale;
			const IScalarPainter*	pRotation;
			const IScalarPainter*	pSkew;
			const IScalarPainter*	pCoverage;		///< may be NULL
			const IScalarPainter*	pGap;
			bool					bGapCanOpen;	///< DL-05: see GapCanOpen()

			//! DL-05: the rule behind bGapCanOpen (WeaveBRDF.cpp).
			static bool GapPainterCanOpen( const IScalarPainter& gap );

			const IPainter*			pWarpColor;
			const IScalarPainter*	pWarpIOR;
			const IScalarPainter*	pWarpWidth;
			const IScalarPainter*	pWarpAzimuth;
			const IScalarPainter*	pWarpKd;
			const IScalarPainter*	pWarpTilt;
			const IScalarPainter*	pWarpTransmit;

			const IPainter*			pWeftColor;
			const IScalarPainter*	pWeftIOR;
			const IScalarPainter*	pWeftWidth;
			const IScalarPainter*	pWeftAzimuth;
			const IScalarPainter*	pWeftKd;
			const IScalarPainter*	pWeftTilt;
			const IScalarPainter*	pWeftTransmit;
		};
	}
}

#endif
