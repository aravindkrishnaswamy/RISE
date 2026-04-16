//////////////////////////////////////////////////////////////////////
//
//  VCMRecurrence.cpp - Pure-math MIS running quantities for VCM.
//
//    Straight translation of the SmallVCM formulas documented in
//    Georgiev's technical report "Implementing Vertex Connection
//    and Merging".  No scene or sampler state; the bodies here are
//    fully unit-testable against synthetic paths in
//    tests/VCMRecurrenceTest.cpp.
//
//    The SmallVCM recurrence inherently computes balance-heuristic
//    pdf ratio sums.  VCMMis() in VCMRecurrence.h is identity (x)
//    matching SmallVCM.  The power heuristic would require tracking
//    sum-of-squares running quantities — a deeper change.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 14, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "VCMRecurrence.h"

using namespace RISE;
using namespace RISE::Implementation;

//////////////////////////////////////////////////////////////////////
// ComputeNormalization
//
// Per-iteration constants used by every strategy's MIS weight.
// From SmallVCM (RunIteration):
//   mLightSubPathCount = float(resX * resY)
//   etaVCM             = PI * radius^2 * mLightSubPathCount
//   mMisVmWeightFactor = enableVM ? etaVCM       : 0
//   mMisVcWeightFactor = enableVC ? 1 / etaVCM   : 0
//   mVmNormalization   = 1 / etaVCM
//
// A merge radius of zero is treated as "no merging": mVmNormalization
// and mMisVmWeightFactor are left at zero so every merging term in
// the SmallVCM formulas collapses cleanly.  Callers must supply a
// positive radius when VM is active.
//////////////////////////////////////////////////////////////////////
VCMNormalization RISE::Implementation::ComputeNormalization(
	const unsigned int width,
	const unsigned int height,
	const Scalar mergeRadius,
	const bool enableVC,
	const bool enableVM
	)
{
	VCMNormalization n;
	n.mEnableVC = enableVC;
	n.mEnableVM = enableVM;
	n.mLightSubPathCount = static_cast<Scalar>( width ) * static_cast<Scalar>( height );
	n.mMergeRadius = mergeRadius;
	n.mMergeRadiusSq = mergeRadius * mergeRadius;

	if( mergeRadius > 0 && n.mLightSubPathCount > 0 )
	{
		const Scalar etaVCM = PI * n.mMergeRadiusSq * n.mLightSubPathCount;
		n.mMisVmWeightFactor = enableVM ? etaVCM : Scalar( 0 );
		n.mMisVcWeightFactor = enableVC ? ( Scalar( 1 ) / etaVCM ) : Scalar( 0 );
		n.mVmNormalization   = Scalar( 1 ) / etaVCM;
	}
	else
	{
		n.mMisVmWeightFactor = 0;
		n.mMisVcWeightFactor = 0;
		n.mVmNormalization   = 0;
	}

	return n;
}

//////////////////////////////////////////////////////////////////////
// ComputeNormalization (explicit light subpath count)
//////////////////////////////////////////////////////////////////////
VCMNormalization RISE::Implementation::ComputeNormalization(
	const unsigned int width,
	const unsigned int height,
	const Scalar mergeRadius,
	const bool enableVC,
	const bool enableVM,
	const Scalar effectiveLightSubpathCount
	)
{
	VCMNormalization n;
	n.mEnableVC = enableVC;
	n.mEnableVM = enableVM;
	n.mLightSubPathCount = effectiveLightSubpathCount;
	n.mMergeRadius = mergeRadius;
	n.mMergeRadiusSq = mergeRadius * mergeRadius;

	if( mergeRadius > 0 && n.mLightSubPathCount > 0 )
	{
		const Scalar etaVCM = PI * n.mMergeRadiusSq * n.mLightSubPathCount;
		n.mMisVmWeightFactor = enableVM ? etaVCM : Scalar( 0 );
		n.mMisVcWeightFactor = enableVC ? ( Scalar( 1 ) / etaVCM ) : Scalar( 0 );
		n.mVmNormalization   = Scalar( 1 ) / etaVCM;
	}
	else
	{
		n.mMisVmWeightFactor = 0;
		n.mMisVcWeightFactor = 0;
		n.mVmNormalization   = 0;
	}

	return n;
}

//////////////////////////////////////////////////////////////////////
// InitLight
//
// First vertex on a light subpath.  From SmallVCM GenerateLightSample:
//   dVCM = directPdfA / emissionPdfW
//   dVC  = !isDelta ? (finite ? cosLight : 1) / emissionPdfW : 0
//   dVM  = dVC * mMisVcWeightFactor   // NOTE: VC, not VM
//
// directPdfA is the AREA-measure pdf of selecting this light position
// for direct lighting (pdfSelect * pdfPosition).  emissionPdfW is the
// SmallVCM "combined area+solid-angle" emission pdf (the same value
// RISE's BDPT generator stores in BDPTVertex::emissionPdfW).
//////////////////////////////////////////////////////////////////////
VCMMisQuantities RISE::Implementation::InitLight(
	const Scalar directPdfA,
	const Scalar emissionPdfW,
	const Scalar cosLight,
	const bool isFiniteLight,
	const bool isDelta,
	const VCMNormalization& norm
	)
{
	VCMMisQuantities q;

	if( emissionPdfW > 0 )
	{
		q.dVCM = directPdfA / emissionPdfW;
		if( !isDelta )
		{
			const Scalar usedCosLight = isFiniteLight ? cosLight : Scalar( 1 );
			q.dVC = usedCosLight / emissionPdfW;
		}
		else
		{
			q.dVC = 0;
		}
	}
	else
	{
		// Degenerate light configuration: zero emission pdf.  Leave
		// the recurrence inert so downstream weights produce zero
		// contribution rather than NaNs.
		q.dVCM = 0;
		q.dVC = 0;
	}

	q.dVM = q.dVC * norm.mMisVcWeightFactor;

	return q;
}

//////////////////////////////////////////////////////////////////////
// InitCamera
//
// First vertex on an eye subpath.  From SmallVCM GenerateCameraSample:
//   dVCM = mLightSubPathCount / cameraPdfW
//   dVC  = 0
//   dVM  = 0
//
// cameraPdfW is the solid-angle directional importance pdf for this
// pixel's primary ray.
//////////////////////////////////////////////////////////////////////
VCMMisQuantities RISE::Implementation::InitCamera(
	const Scalar cameraPdfW,
	const VCMNormalization& norm
	)
{
	VCMMisQuantities q;
	if( cameraPdfW > 0 ) {
		q.dVCM = norm.mLightSubPathCount / cameraPdfW;
	} else {
		q.dVCM = 0;
	}
	q.dVC = 0;
	q.dVM = 0;
	return q;
}

//////////////////////////////////////////////////////////////////////
// ApplyGeometricUpdate
//
// Geometric update applied at each vertex AFTER the ray hits the next
// surface and BEFORE BSDF sampling chooses an onward direction.
// SmallVCM's inline sequence:
//   if (pathLength > 1 || isFiniteLight)
//       dVCM *= Sqr(distance)
//   dVCM /= |cosThetaFix|
//   dVC  /= |cosThetaFix|
//   dVM  /= |cosThetaFix|
//
// 'applyDistSqToDVCM' mirrors the "(pathLength > 1 || isFiniteLight)"
// gate: the first bounce off an infinite light skips the distance^2
// term because the emission point has no physical position.
//////////////////////////////////////////////////////////////////////
VCMMisQuantities RISE::Implementation::ApplyGeometricUpdate(
	const VCMMisQuantities& q,
	const Scalar distSq,
	const Scalar absCosThetaFix,
	const bool applyDistSqToDVCM
	)
{
	VCMMisQuantities r = q;

	if( applyDistSqToDVCM ) {
		r.dVCM *= distSq;
	}

	// Clamp grazing / degenerate cosines to zero rather than
	// dividing and producing infinities — the downstream weights
	// will naturally become zero-contribution.
	if( absCosThetaFix > 0 ) {
		const Scalar invCos = Scalar( 1 ) / absCosThetaFix;
		r.dVCM *= invCos;
		r.dVC  *= invCos;
		r.dVM  *= invCos;
	} else {
		r.dVCM = 0;
		r.dVC  = 0;
		r.dVM  = 0;
	}

	return r;
}

//////////////////////////////////////////////////////////////////////
// ApplyBsdfSamplingUpdate
//
// Update applied immediately after a non-endpoint vertex has its BSDF
// sampled to pick the onward direction.  Two branches from SmallVCM
// SampleScattering:
//
//   specular:
//     dVCM = 0
//     dVC *= cosThetaOut          (bsdfDirPdfW == bsdfRevPdfW so those
//     dVM *= cosThetaOut           factors cancel in the ratio)
//
//   non-specular:
//     dVC  = (cosThetaOut / bsdfDirPdfW) * (dVC  * bsdfRevPdfW
//                                           + dVCM
//                                           + mMisVmWeightFactor)
//     dVM  = (cosThetaOut / bsdfDirPdfW) * (dVM  * bsdfRevPdfW
//                                           + dVCM * mMisVcWeightFactor
//                                           + 1)
//     dVCM = 1 / bsdfDirPdfW
//
// The order matters: dVC and dVM read the *old* dVCM, so dVCM is
// assigned last.
//////////////////////////////////////////////////////////////////////
VCMMisQuantities RISE::Implementation::ApplyBsdfSamplingUpdate(
	const VCMMisQuantities& q,
	const Scalar cosThetaOut,
	const Scalar bsdfDirPdfW,
	const Scalar bsdfRevPdfW,
	const bool specular,
	const VCMNormalization& norm
	)
{
	VCMMisQuantities r;

	if( specular )
	{
		// Delta BSDF: dVCM collapses to zero because no finite
		// solid-angle pdf exists for this interaction; only the
		// chain from previous vertices propagates through the cosine.
		r.dVCM = 0;
		r.dVC  = q.dVC * cosThetaOut;
		r.dVM  = q.dVM * cosThetaOut;
		return r;
	}

	if( bsdfDirPdfW <= 0 ) {
		// Degenerate sample: kill the running values so the path
		// contributes nothing downstream.
		r.dVCM = 0;
		r.dVC  = 0;
		r.dVM  = 0;
		return r;
	}

	const Scalar invFwd = Scalar( 1 ) / bsdfDirPdfW;
	const Scalar factor = cosThetaOut * invFwd;

	// Read the old q.dVCM before we overwrite it.
	const Scalar oldDVCM = q.dVCM;

	r.dVC = factor * ( q.dVC * bsdfRevPdfW + oldDVCM + norm.mMisVmWeightFactor );
	r.dVM = factor * ( q.dVM * bsdfRevPdfW + oldDVCM * norm.mMisVcWeightFactor + Scalar( 1 ) );
	r.dVCM = invFwd;

	return r;
}
