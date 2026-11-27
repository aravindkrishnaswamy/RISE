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
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Intersection/RayIntersection.h"
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

namespace {
	std::atomic<unsigned int> alphaMaterialCount(0);
	std::atomic<unsigned int> mixMaterialCount(0);
}
IMaterial::IMaterial() : alphaPainter_(0), alphaMode_(eAlphaOpaque), alphaCutoff_(0.5) {}
IMaterial::~IMaterial() { if (alphaMode_ != eAlphaOpaque) --alphaMaterialCount; if (alphaPainter_) alphaPainter_->release(); }
void IMaterial::AdjustAlphaMaterialCount( int delta )
{
	if( delta > 0 ) alphaMaterialCount.fetch_add( static_cast<unsigned int>( delta ), std::memory_order_relaxed );
	else if( delta < 0 ) alphaMaterialCount.fetch_sub( static_cast<unsigned int>( -delta ), std::memory_order_relaxed );
}
void IMaterial::AdjustMixMaterialCount( int delta )
{
	if( delta > 0 ) mixMaterialCount.fetch_add( static_cast<unsigned int>( delta ), std::memory_order_relaxed );
	else if( delta < 0 ) mixMaterialCount.fetch_sub( static_cast<unsigned int>( -delta ), std::memory_order_relaxed );
}
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
bool IMaterial::AnyMixMaterials() { return mixMaterialCount.load(std::memory_order_relaxed) != 0; }

unsigned int IMaterial::GetPathDependence() const
{
	return alphaPainter_ ? alphaPainter_->GetPathDependence() : 0u;
}

void RISE::EvaluateMaterialAOVGuides(
	const RayIntersection& ri,
	const RayIntersectionGeometric& geom,
	const bool bClayOverride,
	const IBSDF* pClayBRDF,
	RISEPel& outAlbedo,
	Vector3& outNormal
	)
{
	if( ri.pUnresolvedMix ) {
		ri.pUnresolvedMix->EvaluateExpectedGuides( ri, bClayOverride, pClayBRDF, outAlbedo, outNormal );
		return;
	}
	outNormal = geom.vNormal;
	if( bClayOverride && pClayBRDF ) {
		outAlbedo = pClayBRDF->albedo( geom );
	} else if( !ri.pMaterial ) {
		// Round 3 P1-1: black absorber (no material) reports black albedo, not white.
		outAlbedo = RISEPel( 0, 0, 0 );
	} else if( ri.pMaterial->EvaluateVisibleGuideAlbedo( geom, outAlbedo ) ) {
		// Material supplied a dedicated visible guide albedo (e.g. HyLIoS).
	} else if( const IBSDF* pBSDF = ri.pMaterial->GetBSDF() ) {
		outAlbedo = pBSDF->albedo( geom );
	} else if( ri.pMaterial->GetSPF() ) {
		// Delta / specular / transparent surface (has SPF, no BSDF): white albedo per OIDN.
		outAlbedo = RISEPel( 1, 1, 1 );
	} else {
		// NullMaterial / absorber (neither BSDF nor SPF): black albedo.
		outAlbedo = RISEPel( 0, 0, 0 );
	}
}
