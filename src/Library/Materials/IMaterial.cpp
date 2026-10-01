//////////////////////////////////////////////////////////////////////
//
//  IMaterial.cpp - Implementation of IMaterial default virtual methods
//  that depend on ISPF (avoiding circular include issues).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ISPF.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/ISampler.h"
#include <atomic>

using namespace RISE;

Scalar IMaterial::Pdf(
	const Vector3& wo,
	const RayIntersectionGeometric& ri,
	const IORStack& ior_stack
	) const
{
	const ISPF* pSPF = GetSPF();
	return pSPF ? pSPF->Pdf( ri, wo, ior_stack ) : 0;
}

Scalar IMaterial::PdfNM(
	const Vector3& wo,
	const RayIntersectionGeometric& ri,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	const ISPF* pSPF = GetSPF();
	return pSPF ? pSPF->PdfNM( ri, wo, nm, ior_stack ) : 0;
}

namespace { std::atomic<unsigned int> alphaMaterialCount(0); }
IMaterial::IMaterial() : alphaPainter_(0), alphaMode_(eAlphaOpaque), alphaCutoff_(0.5) {}
IMaterial::~IMaterial() { if (alphaMode_ != eAlphaOpaque) --alphaMaterialCount; if (alphaPainter_) alphaPainter_->release(); }
void IMaterial::SetAlpha(const IScalarPainter* painter, AlphaMode mode, Scalar cutoff)
{
    if (painter) painter->addref();
    if (alphaPainter_) alphaPainter_->release();
    if (mode != eAlphaOpaque && alphaMode_ == eAlphaOpaque) ++alphaMaterialCount;
    if (mode == eAlphaOpaque && alphaMode_ != eAlphaOpaque) --alphaMaterialCount;
    alphaPainter_ = painter; alphaMode_ = mode; alphaCutoff_ = cutoff;
}
Scalar IMaterial::AlphaCoverage(const RayIntersectionGeometric& ri) const
{
    if (alphaMode_ == eAlphaOpaque || !alphaPainter_) return 1;
    Scalar a = alphaPainter_->GetValuesAt(ri).v[ScalarPainterRGB::kSingleSampleChannel];
    if (!(a >= 0)) a = 0;
    if (a > 1) a = 1;
    return alphaMode_ == eAlphaMask ? (a >= alphaCutoff_ ? 1 : 0) : a;
}

bool IMaterial::AcceptAlpha(const RayIntersectionGeometric& ri, ISampler& sampler) const
{
    const Scalar a = AlphaCoverage(ri);
    return a >= 1 || (a > 0 && sampler.GetAlpha1D() < a);
}
bool IMaterial::AnyAlphaMaterials() { return alphaMaterialCount.load(std::memory_order_relaxed) != 0; }
