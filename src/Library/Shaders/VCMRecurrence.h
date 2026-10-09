//////////////////////////////////////////////////////////////////////
//
//  VCMRecurrence.h - Pure-math MIS running quantities for VCM.
//
//    Georgiev et al. "Light Transport Simulation with Vertex
//    Connection and Merging" (SIGGRAPH Asia 2012) tracks three
//    running scalars per vertex that let the balance-heuristic MIS
//    weight for every connection and merging strategy be evaluated
//    in O(1) at connection/merge time:
//
//      dVCM — used by both vertex connections and vertex merging
//      dVC  — used by vertex connections only
//      dVM  — used by vertex merging only
//
//    This header is intentionally standalone: it depends only on
//    RISE's Scalar type and has no renderer / scene / sampler
//    dependencies so it can be covered by a synthetic-path unit
//    test and diffed line-for-line against SmallVCM's vertexcm.hxx.
//
//    Step 0 ships the interface and zero-initialized returns so the
//    build wiring compiles.  Step 2 populates the bodies with the
//    SmallVCM recurrence formulas.
//
//    Reference: http://www.iliyan.com/publications/ImplementingVCM
//                https://github.com/SmallVCM/SmallVCM
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 14, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef VCM_RECURRENCE_
#define VCM_RECURRENCE_

#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	namespace Implementation
	{
		/// MIS heuristic function applied to accumulated weight terms
		/// before summing in the weight denominator.
		///
		/// The SmallVCM recurrence computes dVCM/dVC/dVM as running
		/// sums of individual pdf ratios (balance heuristic).  The
		/// power heuristic (x²) cannot be applied to these accumulated
		/// sums because (Σx_i)² ≠ Σx_i²; it would require separate
		/// running quantities tracking the sum-of-squares, which is a
		/// deeper architectural change.  VCM therefore uses the balance
		/// heuristic, matching SmallVCM.  RISE BDPT uses the power
		/// heuristic via its walk-based MISWeight function.
		inline Scalar VCMMis( const Scalar x )
		{
			return x;	// balance heuristic (SmallVCM default)
		}

		/// The three per-vertex running MIS scalars from the
		/// SmallVCM formulation.  dVCM participates in both VC and
		/// VM strategies; dVC only in VC; dVM only in VM.
		/// The three running scalars alone -- what a stored light vertex
		/// keeps (VCMLightVertex.h), without VCMMisQuantities' DL-467 step.
		struct VCMMisCore
		{
			Scalar dVCM;
			Scalar dVC;
			Scalar dVM;

			VCMMisCore() : dVCM( 0 ), dVC( 0 ), dVM( 0 ) {}
		};

		struct VCMMisQuantities : public VCMMisCore
		{
			/// DL-467: the affine step that produced THIS record's (dVC,
			/// dVM) from the PREVIOUS record of the same subpath array
			/// (ConvertLightSubpath / ConvertEyeSubpath stamp every
			/// record):  dVC_i = xf * dVC_{i-1} + xgVC,  dVM_i = xf *
			/// dVM_{i-1} + xgVM.  dVC and dVM share the multiplier (every
			/// update scales them alike) and dVCM never depends on them,
			/// so replaying these steps from a record whose dVC / dVM are
			/// zeroed drops exactly the MIS terms of the strategies that
			/// lie past that record -- the depth-cap window
			/// (VCMIntegrator.cpp, "DL-467").  Not read by the ordinary
			/// weights; 0 on a record nobody stamped.
			Scalar xf;
			Scalar xgVC;
			Scalar xgVM;

			VCMMisQuantities() : VCMMisCore(), xf( 0 ), xgVC( 0 ), xgVM( 0 ) {}
		};

		/// Per-iteration constants shared by all subpaths.  Derived
		/// from the image resolution, the merge radius, and which
		/// strategy classes are enabled.
		struct VCMNormalization
		{
			Scalar	mLightSubPathCount;		///< = resX * resY
			Scalar	mMisVmWeightFactor;		///< VM ? PI*r^2*count : 0
			Scalar	mMisVcWeightFactor;		///< VC ? 1/(PI*r^2*count) : 0
			Scalar	mVmNormalization;		///< 1 / (PI*r^2*count)
			Scalar	mMergeRadius;			///< The resolved merge radius (units: world space)
			Scalar	mMergeRadiusSq;			///< mMergeRadius * mMergeRadius (KD-tree query uses squared distance)
			bool	mEnableVC;
			bool	mEnableVM;
			/// DL-469: the volume merge at MEDIUM vertices
			/// (VCMIntegrator.cpp, "volume merging") -- a 3-D ball kernel
			/// of its own radius r_v.  Its MIS factor eta_v plays the role
			/// mMisVmWeightFactor plays at a surface vertex (see
			/// VertexMergeFactorVC / VM).  All 0 = off.
			Scalar	mVolumeMergeRadius;
			Scalar	mVolumeMergeRadiusSq;
			Scalar	mVolumeNormalization;		///< 1 / eta_v
			Scalar	mMisVolumeWeightFactor;		///< eta_v = count * 4/3 pi r_v^3

			VCMNormalization() :
				mLightSubPathCount( 0 ),
				mMisVmWeightFactor( 0 ),
				mMisVcWeightFactor( 0 ),
				mVmNormalization( 0 ),
				mMergeRadius( 0 ),
				mMergeRadiusSq( 0 ),
				mEnableVC( true ),
				mEnableVM( true ),
				mVolumeMergeRadius( 0 ),
				mVolumeMergeRadiusSq( 0 ),
				mVolumeNormalization( 0 ),
				mMisVolumeWeightFactor( 0 )
			{}
		};

		/// DL-469: the merge strategy AT a vertex, as the recurrence
		/// counts it.  A surface vertex merges with the surface kernel
		/// (mMisVmWeightFactor); a MEDIUM vertex with the volume kernel
		/// (mMisVolumeWeightFactor, 0 when the volume merge is off --
		/// it then has no merge, and counting the surface factor there,
		/// as RISE did before DL-469, reserved MIS mass for a strategy
		/// that never ran: ~5 % low on multiple scattering).
		///   VC: the term the vertex adds to dVC (absolute eta).
		///   VM: the term it adds to dVM, which is dVC / eta_s (SmallVCM's
		///       dVM == dVC * mMisVcWeightFactor identity): 1 at a
		///       surface, eta_v / eta_s at a medium vertex.
		inline Scalar VertexMergeFactorVC( const VCMNormalization& n, const bool medium )
		{
			return medium ? n.mMisVolumeWeightFactor : n.mMisVmWeightFactor;
		}
		inline Scalar VertexMergeFactorVM( const VCMNormalization& n, const bool medium )
		{
			if( !medium ) {
				return Scalar( 1 );
			}
			return n.mMisVmWeightFactor > 0 ? n.mMisVolumeWeightFactor / n.mMisVmWeightFactor : Scalar( 0 );
		}

		/// DL-467: accumulates the affine (dVC, dVM) step between two
		/// records while the recurrence runs.  Each Compose* mirrors the
		/// corresponding Apply* below branch for branch.
		struct VCMTransfer
		{
			Scalar f;
			Scalar gVC;
			Scalar gVM;

			VCMTransfer() : f( 1 ), gVC( 0 ), gVM( 0 ) {}

			/// The state was overwritten by a value independent of the
			/// previous record (an entry barrier / onward update).
			void Reset( const VCMMisQuantities& q )
			{
				f = 0;
				gVC = q.dVC;
				gVM = q.dVM;
			}

			/// Mirrors ApplyGeometricUpdate.
			void ComposeGeometric( const Scalar absCosThetaFix )
			{
				if( absCosThetaFix > 0 ) {
					const Scalar invCos = Scalar( 1 ) / absCosThetaFix;
					f *= invCos;
					gVC *= invCos;
					gVM *= invCos;
				} else {
					f = 0;
					gVC = 0;
					gVM = 0;
				}
			}

			/// Mirrors ApplyBsdfSamplingUpdate; `qBefore` is the state the
			/// update is applied to (its dVCM enters the constant part).
			void ComposeBsdf(
				const VCMMisQuantities& qBefore,
				const Scalar cosThetaOut,
				const Scalar bsdfDirPdfW,
				const Scalar bsdfRevPdfW,
				const bool specular,
				const VCMNormalization& norm,
				const bool medium = false
				)
			{
				if( specular ) {
					f *= cosThetaOut;
					gVC *= cosThetaOut;
					gVM *= cosThetaOut;
					return;
				}
				if( bsdfDirPdfW <= 0 ) {
					f = 0;
					gVC = 0;
					gVM = 0;
					return;
				}
				const Scalar factor = cosThetaOut * ( Scalar( 1 ) / bsdfDirPdfW );
				const Scalar mul = factor * bsdfRevPdfW;
				f *= mul;
				gVC = mul * gVC + factor * ( qBefore.dVCM + VertexMergeFactorVC( norm, medium ) );
				gVM = mul * gVM + factor * ( qBefore.dVCM * norm.mMisVcWeightFactor + VertexMergeFactorVM( norm, medium ) );
			}

			/// Write the accumulated step into a record and start over.
			void StampAndRestart( VCMMisQuantities& q )
			{
				q.xf = f;
				q.xgVC = gVC;
				q.xgVM = gVM;
				f = 1;
				gVC = 0;
				gVM = 0;
			}
		};

		/// Compute the per-iteration normalization constants.
		///
		/// When VM is disabled, mMisVmWeightFactor == 0 collapses every
		/// SmallVCM weight expression back to the pure-BDPT form.  When
		/// VC is disabled, mMisVcWeightFactor == 0 likewise collapses
		/// the merge-only form.  Both disabled is a legal (zero-energy)
		/// configuration and is handled without divides by zero.
		///
		/// Step 0: returns a zeroed-out struct.  Step 2 fills this in.
		VCMNormalization ComputeNormalization(
			const unsigned int width,
			const unsigned int height,
			const Scalar mergeRadius,
			const bool enableVC,
			const bool enableVM
			);

		/// Overload with explicit light subpath count.  When the
		/// specular-only store filter discards non-caustic photons,
		/// the effective VM photon count is less than W*H.  Using
		/// the actual stored count for etaVCM keeps the MIS weights
		/// correctly calibrated.
		VCMNormalization ComputeNormalization(
			const unsigned int width,
			const unsigned int height,
			const Scalar mergeRadius,
			const bool enableVC,
			const bool enableVM,
			const Scalar effectiveLightSubpathCount
			);

		/// DL-469: set the volume-merge radius on `n` (after
		/// ComputeNormalization, against n.mLightSubPathCount).  A
		/// non-positive radius, or VC or VM disabled, turns the volume
		/// merge off (its weights rely on SmallVCM's dVM == dVC / eta_s
		/// identity, which needs both).
		void SetVolumeMergeRadius( VCMNormalization& n, const Scalar volumeRadius );

		/// Initialize (dVCM, dVC, dVM) at the first vertex of a light
		/// subpath.  directPdfA is the light's area PDF of selecting
		/// this position (pdfSelect * pdfPosition).  emissionPdfW is
		/// the joint emission PDF (pdfSelect * pdfPosition *
		/// pdfDirection).  cosLight is |n_light . direction_out|.
		/// isFiniteLight is false for infinite lights (environment,
		/// directional) to suppress the distance-squared term in the
		/// first bounce update.
		///
		/// Both densities include light selection. dVC must preserve
		/// 1/pdfSelect: its alternatives arrive from the camera rather
		/// than sampling this light. Infinite direct density is q*p_env
		/// in solid angle; joint emission is q*p_env*p_disc.
		///
		/// Step 0: returns a zeroed-out struct.  Step 2 fills this in.
		VCMMisQuantities InitLight(
			const Scalar directPdfA,
			const Scalar emissionPdfW,
			const Scalar cosLight,
			const bool isFiniteLight,
			const bool isDelta,
			const VCMNormalization& norm
			);

		/// Initialize (dVCM, dVC, dVM) at the camera vertex.
		/// cameraPdfW is the camera's directional importance PDF in
		/// solid-angle measure (typically read from
		/// BDPTCameraUtilities::PdfDirection).
		///
		/// Step 0: returns a zeroed-out struct.  Step 2 fills this in.
		VCMMisQuantities InitCamera(
			const Scalar cameraPdfW,
			const VCMNormalization& norm
			);

		/// Apply the post-intersection geometric update to the running
		/// quantities.  distSq is |prev -> current|^2.  absCosThetaFix
		/// is the receiving-side cosine at the current vertex.
		/// applyDistSqToDVCM gates the distance-squared term; the
		/// first bounce from an infinite light skips it.
		///
		/// Step 0: returns the input unchanged.  Step 2 fills this in.
		VCMMisQuantities ApplyGeometricUpdate(
			const VCMMisQuantities& q,
			const Scalar distSq,
			const Scalar absCosThetaFix,
			const bool applyDistSqToDVCM
			);

		/// Apply the BSDF-sampling update at a non-endpoint vertex.
		/// bsdfDirPdfW and bsdfRevPdfW are solid-angle BSDF sampling
		/// PDFs (forward: generating the next vertex; reverse: the
		/// opposite direction).  cosThetaOut is the outgoing cosine.
		/// Specular vertices take a simpler branch because their
		/// delta BSDF has no finite solid-angle PDF.
		///
		/// `medium` (DL-469): the scattering vertex is a MEDIUM vertex,
		/// whose merge (if any) is the volume merge -- see
		/// VertexMergeFactorVC / VM.
		VCMMisQuantities ApplyBsdfSamplingUpdate(
			const VCMMisQuantities& q,
			const Scalar cosThetaOut,
			const Scalar bsdfDirPdfW,
			const Scalar bsdfRevPdfW,
			const bool specular,
			const VCMNormalization& norm,
			const bool medium = false
			);
	}
}

#endif
