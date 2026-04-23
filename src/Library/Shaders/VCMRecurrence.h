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
		struct VCMMisQuantities
		{
			Scalar dVCM;
			Scalar dVC;
			Scalar dVM;

			VCMMisQuantities() : dVCM( 0 ), dVC( 0 ), dVM( 0 ) {}
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

			VCMNormalization() :
				mLightSubPathCount( 0 ),
				mMisVmWeightFactor( 0 ),
				mMisVcWeightFactor( 0 ),
				mVmNormalization( 0 ),
				mMergeRadius( 0 ),
				mMergeRadiusSq( 0 ),
				mEnableVC( true ),
				mEnableVM( true )
			{}
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

		/// Initialize (dVCM, dVC, dVM) at the first vertex of a light
		/// subpath.  directPdfA is the light's area PDF of selecting
		/// this position (pdfSelect * pdfPosition).  emissionPdfW is
		/// the solid-angle direction PDF of the initial emission ray.
		/// cosLight is |n_light . direction_out|.  isFiniteLight is
		/// false for infinite lights (environment, directional) to
		/// suppress the distance-squared term in the first bounce
		/// update.
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
		VCMMisQuantities ApplyBsdfSamplingUpdate(
			const VCMMisQuantities& q,
			const Scalar cosThetaOut,
			const Scalar bsdfDirPdfW,
			const Scalar bsdfRevPdfW,
			const bool specular,
			const VCMNormalization& norm
			);
	}
}

#endif
