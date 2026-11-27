//////////////////////////////////////////////////////////////////////
//
//  MixMaterial.cpp - Implementation of physically-based bounded N-ary
//    material mixing (`mix_material`).
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MixMaterial.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/ISPF.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/ILog.h"
#include "../Intersection/RayIntersection.h"
#include "../Utilities/ISampler.h"
#include "../Utilities/FiniteMath.h"
#include <cmath>
#include <cstdint>
#include <cstring>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	//! Deterministic, ASLR-free fallback uniform in [0, 1) used ONLY when a
	//! caller resolves a hit without an active `ISampler*` (e.g. bare
	//! `ObjectManager::IntersectRay`).  Every transport path supplies its
	//! `ISampler&` via `IntersectRaySampled` (Round 3 §3).
	Scalar DeterministicFallbackLeafDraw( const RayIntersectionGeometric& ri )
	{
		auto canonBits = []( double x ) -> std::uint64_t {
			std::uint64_t u = 0;
			std::memcpy( &u, &x, sizeof( u ) );
			if( ( u & 0x7FFFFFFFFFFFFFFFULL ) == 0ULL ) u = 0ULL;
			return u;
		};
		auto mix64 = []( std::uint64_t z ) -> std::uint64_t {
			z = ( z ^ ( z >> 30 ) ) * 0xbf58476d1ce4e5b9ULL;
			z = ( z ^ ( z >> 27 ) ) * 0x94d049bb133111ebULL;
			return z ^ ( z >> 31 );
		};
		// Dedicated salt distinct from osl_ao and VM hash builtins.
		std::uint64_t h = 0x517cc1b727220a95ULL;
		h = mix64( h ^ canonBits( static_cast<double>( ri.ptObjIntersec.x ) ) );
		h = mix64( h ^ canonBits( static_cast<double>( ri.ptObjIntersec.y ) ) );
		h = mix64( h ^ canonBits( static_cast<double>( ri.ptObjIntersec.z ) ) );
		h = mix64( h ^ canonBits( static_cast<double>( ri.ptIntersection.x ) ) );
		h = mix64( h ^ canonBits( static_cast<double>( ri.ptIntersection.y ) ) );
		h = mix64( h ^ canonBits( static_cast<double>( ri.ptIntersection.z ) ) );
		// Top 53 bits -> [0, 1)
		const double u = static_cast<double>( h >> 11 ) * ( 1.0 / 9007199254740992.0 );
		return static_cast<Scalar>( u );
	}
}

bool MixMaterial::ValidateChildren(
	const IMaterial* const* pChildren,
	const IScalarPainter* const* pWeights,
	const unsigned int numChildren
	)
{
	if( numChildren == 0 || !pChildren || !pWeights ) {
		GlobalLog()->PrintSourceError(
			"MixMaterial:: requires at least one child and non-null child/weight arrays",
			__FILE__, __LINE__ );
		return false;
	}
	for( unsigned int i = 0; i < numChildren; ++i ) {
		if( !pWeights[i] ) {
			GlobalLog()->PrintEx( eLog_Error,
				"MixMaterial:: child %u has a null weight scalar painter", i );
			return false;
		}
		const IMaterial* pMat = pChildren[i];
		if( !pMat ) {
			continue; // explicit `none` black absorber is valid
		}
		if( pMat->IsUnresolvedMix() ) {
			GlobalLog()->PrintEx( eLog_Error,
				"MixMaterial:: child %u is another MixMaterial; nested mixes are not allowed (flatten the mix tree instead)", i );
			return false;
		}
		if( pMat->TracksInteriorContainment() ) {
			GlobalLog()->PrintEx( eLog_Error,
				"MixMaterial:: child %u tracks interior IOR containment (dielectric/refractor/translucent/composite/null-boundary/tissue); "
				"containment-tracking materials cannot be children of a MixMaterial", i );
			return false;
		}
	}
	return true;
}

MixMaterial::MixMaterial(
	const IMaterial* const* pChildren,
	const IScalarPainter* const* pWeights,
	const IRayIntersectionModifier* const* pModifiers,
	const unsigned int numChildren,
	const IScalarPainter* pPassthroughWeight_
	) :
	pPassthroughWeight( pPassthroughWeight_ ),
	loggedInvalidWeight( false ),
	loggedSumOverOne( false )
{
	children.reserve( numChildren );
	for( unsigned int i = 0; i < numChildren; ++i ) {
		MixMaterialChild c;
		c.pMaterial = pChildren ? pChildren[i] : 0;
		c.pWeight   = pWeights ? pWeights[i] : 0;
		c.pModifier = pModifiers ? pModifiers[i] : 0;
		if( c.pMaterial ) {
			c.pMaterial->addref();
		}
		if( c.pWeight ) {
			c.pWeight->addref();
		}
		if( c.pModifier ) {
			c.pModifier->addref();
		}
		children.push_back( c );
	}
	if( pPassthroughWeight ) {
		pPassthroughWeight->addref();
		// Round 3 P1-2: register pass-through mix with the global alpha counter
		// so IntersectRaySampled / IntersectShadowRaySampled never bypass alpha testing.
		IMaterial::AdjustAlphaMaterialCount( +1 );
	}
	IMaterial::AdjustMixMaterialCount( +1 );
}

MixMaterial::~MixMaterial()
{
	IMaterial::AdjustMixMaterialCount( -1 );
	if( pPassthroughWeight ) {
		IMaterial::AdjustAlphaMaterialCount( -1 );
		pPassthroughWeight->release();
		pPassthroughWeight = 0;
	}
	for( size_t i = 0; i < children.size(); ++i ) {
		if( children[i].pMaterial ) children[i].pMaterial->release();
		if( children[i].pWeight )   children[i].pWeight->release();
		if( children[i].pModifier ) children[i].pModifier->release();
	}
	children.clear();
}

bool MixMaterial::CouldLightPassThrough() const
{
	for( size_t i = 0; i < children.size(); ++i ) {
		if( children[i].pMaterial && children[i].pMaterial->CouldLightPassThrough() ) {
			return true;
		}
	}
	return false;
}

bool MixMaterial::HasDeltaPassThrough() const
{
	for( size_t i = 0; i < children.size(); ++i ) {
		if( children[i].pMaterial && children[i].pMaterial->HasDeltaPassThrough() ) {
			return true;
		}
	}
	return false;
}

bool MixMaterial::HasSubsurfaceLeaf() const
{
	for( size_t i = 0; i < children.size(); ++i ) {
		if( children[i].pMaterial && children[i].pMaterial->HasSubsurfaceLeaf() ) {
			return true;
		}
	}
	return false;
}

bool MixMaterial::HasRandomWalkSSSLeaf() const
{
	for( size_t i = 0; i < children.size(); ++i ) {
		if( children[i].pMaterial && children[i].pMaterial->HasRandomWalkSSSLeaf() ) {
			return true;
		}
	}
	return false;
}

bool MixMaterial::IsHyperspectralSkin() const
{
	for( size_t i = 0; i < children.size(); ++i ) {
		if( children[i].pMaterial && children[i].pMaterial->IsHyperspectralSkin() ) {
			return true;
		}
	}
	return false;
}

bool MixMaterial::HasSmoothNonlocalBoundary() const
{
	for( size_t i = 0; i < children.size(); ++i ) {
		if( children[i].pMaterial && children[i].pMaterial->HasSmoothNonlocalBoundary() ) {
			return true;
		}
	}
	return false;
}

bool MixMaterial::CoversWavelengthRange( const Scalar nmBegin, const Scalar nmEnd ) const
{
	for( size_t i = 0; i < children.size(); ++i ) {
		if( children[i].pMaterial && !children[i].pMaterial->CoversWavelengthRange( nmBegin, nmEnd ) ) {
			return false;
		}
	}
	return true;
}

bool MixMaterial::HasAlphaCoverage() const
{
	if( pPassthroughWeight || alphaMode_ != eAlphaOpaque ) {
		return true;
	}
	for( size_t i = 0; i < children.size(); ++i ) {
		if( children[i].pMaterial && children[i].pMaterial->HasAlphaCoverage() ) {
			return true;
		}
	}
	return false;
}

void MixMaterial::EvaluateWeights(
	const RayIntersectionGeometric& ri,
	Scalar* outWeights,
	Scalar& outWClear
	) const
{
	const size_t n = children.size();
	Scalar sum = 0;
	bool sawInvalid = false;

	for( size_t i = 0; i < n; ++i ) {
		const Scalar raw = children[i].pWeight
			? children[i].pWeight->GetValuesAt( ri ).v[ScalarPainterRGB::kSingleSampleChannel]
			: Scalar( 0 );
		if( !IsFiniteDouble( static_cast<double>( raw ) ) || raw < Scalar( 0 ) ) {
			outWeights[i] = Scalar( 0 );
			sawInvalid = true;
		} else {
			outWeights[i] = raw;
			sum += raw;
		}
	}

	outWClear = Scalar( 0 );
	if( pPassthroughWeight ) {
		const Scalar rawClear = pPassthroughWeight->GetValuesAt( ri ).v[ScalarPainterRGB::kSingleSampleChannel];
		if( !IsFiniteDouble( static_cast<double>( rawClear ) ) || rawClear < Scalar( 0 ) ) {
			outWClear = Scalar( 0 );
			sawInvalid = true;
		} else {
			outWClear = rawClear;
			sum += rawClear;
		}
	}

	if( sawInvalid && !loggedInvalidWeight.exchange( true, std::memory_order_relaxed ) ) {
		GlobalLog()->PrintEasyWarning(
			"MixMaterial:: negative or non-finite weight encountered; clamped to 0" );
	}

	// Bounded physically-based mix (MIX §I.1): never normalize when sum < 1
	// (shortfall absorbs), and clamp by dividing by sum when sum > 1.
	if( sum > Scalar( 1 ) ) {
		if( !loggedSumOverOne.exchange( true, std::memory_order_relaxed ) ) {
			GlobalLog()->PrintEasyWarning(
				"MixMaterial:: weight sum exceeded 1.0; dividing weights by sum to preserve energy bounds" );
		}
		const Scalar invS = Scalar( 1 ) / sum;
		for( size_t i = 0; i < n; ++i ) {
			outWeights[i] *= invS;
		}
		outWClear *= invS;
	}

	if( outWClear > Scalar( 1 ) ) {
		outWClear = Scalar( 1 );
	}
}

Scalar MixMaterial::AlphaCoverage( const RayIntersectionGeometric& ri ) const
{
	const size_t n = children.size();
	Scalar stackW[kStackWeightCap];
	std::vector<Scalar> heapW;
	Scalar* w = stackW;
	if( n > kStackWeightCap ) {
		heapW.resize( n );
		w = heapW.data();
	}
	Scalar wClear = 0;
	EvaluateWeights( ri, w, wClear );
	Scalar aMix = Scalar( 1 ) - wClear;
	if( aMix < Scalar( 0 ) ) aMix = Scalar( 0 );
	if( aMix > Scalar( 1 ) ) aMix = Scalar( 1 );
	return aMix * IMaterial::AlphaCoverage( ri );
}

void MixMaterial::ResolveMixHit( RayIntersection& ri, ISampler* pSampler ) const
{
	const size_t n = children.size();
	Scalar stackW[kStackWeightCap];
	std::vector<Scalar> heapW;
	Scalar* w = stackW;
	if( n > kStackWeightCap ) {
		heapW.resize( n );
		w = heapW.data();
	}
	Scalar wClear = 0;
	// Weights are ALWAYS evaluated on the pre-modifier geometric hit (MIX §I.2).
	EvaluateWeights( ri.geometric, w, wClear );

	Scalar aMix = Scalar( 1 ) - wClear;
	if( aMix < Scalar( 0 ) ) aMix = Scalar( 0 );
	if( aMix > Scalar( 1 ) ) aMix = Scalar( 1 );

	const Scalar ownAlpha = IMaterial::AlphaCoverage( ri.geometric );
	const Scalar totalMixCoverage = aMix * ownAlpha;

	// Round 3 P1-6: retain unresolved mix & pre-mix modifier for noise-free expected-value denoiser guides.
	ri.pUnresolvedMix   = this;
	ri.pPreMixModifier  = ri.pModifier;
	ri.bMaterialFromMix = true;
	ri.mixCoverage      = totalMixCoverage;

	if( aMix <= Scalar( 0 ) || totalMixCoverage <= Scalar( 0 ) ) {
		ri.pMaterial      = 0;
		ri.mixConnectible = Scalar( 0 );
		return;
	}

	// Round 3 P1-1 (+ Slice 3 R1 A-P1-1): q = P(connectible & non-zero-alpha leaf | mix accepted).
	// Matches BDPTIntegrator::v.isConnectible iff (pMaterial && GetBSDF() && GetSPF()),
	// excluding zero-alpha cutout children (alpha_i == 0) whose hits are never deposited.
	Scalar qSum = 0;
	for( size_t i = 0; i < n; ++i ) {
		const IMaterial* pChild = children[i].pMaterial;
		if( w[i] > Scalar( 0 ) && pChild && pChild->GetBSDF() && pChild->GetSPF() &&
			( !pChild->HasAlphaCoverage() || pChild->AlphaCoverage( ri.geometric ) > Scalar( 0 ) ) )
		{
			qSum += w[i];
		}
	}
	Scalar q = qSum / aMix;
	if( q < Scalar( 0 ) ) q = Scalar( 0 );
	if( q > Scalar( 1 ) ) q = Scalar( 1 );
	ri.mixConnectible = q;

	// Round 3 §3: per-hit stochastic draw from active sampler (fallback hash only when null).
	Scalar u = pSampler ? pSampler->Get1D() : DeterministicFallbackLeafDraw( ri.geometric );
	if( !IsFiniteDouble( static_cast<double>( u ) ) || u < Scalar( 0 ) ) u = Scalar( 0 );
	if( u >= Scalar( 1 ) ) u = std::nextafter( Scalar( 1 ), Scalar( 0 ) );

	const Scalar xi = u * aMix;
	Scalar acc = 0;
	for( size_t i = 0; i < n; ++i ) {
		acc += w[i];
		if( xi < acc ) {
			ri.pMaterial = children[i].pMaterial; // nullptr when child is `none` absorber
			if( children[i].pModifier ) {
				ri.pModifier = children[i].pModifier;
			}
			return;
		}
	}

	// Shortfall (aMix - sum(w)) absorbs as `none`.
	ri.pMaterial = 0;
}

void MixMaterial::EvaluateExpectedGuides(
	const RayIntersection& ri,
	const bool bClayOverride,
	const IBSDF* pClayBRDF,
	RISEPel& outAlbedo,
	Vector3& outNormal
	) const
{
	// Reconstruct the FULL pre-modifier geometric tangent frame (preserving
	// coherent UV tangents, mirrored-instance FlipV, and normalMapOnb --
	// Slice 3 R1 A-P1-3) so weights and every leaf's modifier evaluate on the
	// true unmodified surface frame even after the chosen leaf's modifier ran.
	RayIntersectionGeometric preModGeom( ri.geometric );
	if( ri.geometric.bHasNormalMapFrame ) {
		// Keep `normalMapOnb` intact for NormalMap leaves; restore `onb` and
		// `vNormal` from `coatDecodeOnb` if recorded, else `normalMapOnb`.
		if( ri.geometric.bHasCoatDecodeOnb ) {
			preModGeom.onb = ri.geometric.coatDecodeOnb;
			preModGeom.bHasCoatDecodeOnb = false;
		} else {
			preModGeom.onb = ri.geometric.normalMapOnb;
		}
		preModGeom.vNormal = ri.geometric.normalMapOnb.w();
		if( !ri.geometric.bHasCoatDecodeOnb && ri.geometric.bHasShaderDirection ) {
			// Slice 3 R2 P2-2: match ModifierFrame::PromoteShaderDirection so
			// anisotropy/GlintModifier leaves on shader-direction meshes see
			// the exact pre-modifier shader-direction tangent frame.
			const Vector3 proj = ri.geometric.vShaderDirection -
				preModGeom.vNormal * Vector3Ops::Dot( ri.geometric.vShaderDirection, preModGeom.vNormal );
			if( Vector3Ops::SquaredModulus( proj ) > Scalar( 0 ) ) {
				preModGeom.onb.CreateFromWU( preModGeom.vNormal, proj );
			}
		}
	} else if( ri.geometric.bHasCoatDecodeOnb ) {
		preModGeom.onb = ri.geometric.coatDecodeOnb;
		preModGeom.vNormal = preModGeom.onb.w();
		preModGeom.bHasCoatDecodeOnb = false;
	}
	const Vector3 n0 = preModGeom.vNormal;

	const size_t n = children.size();
	Scalar stackW[kStackWeightCap];
	Scalar stackEff[kStackWeightCap];
	std::vector<Scalar> heapW;
	std::vector<Scalar> heapEff;
	Scalar* w = stackW;
	Scalar* effW = stackEff;
	if( n > kStackWeightCap ) {
		heapW.resize( n );
		heapEff.resize( n );
		w = heapW.data();
		effW = heapEff.data();
	}

	Scalar wClear = 0;
	EvaluateWeights( preModGeom, w, wClear );

	Scalar aMix = Scalar( 1 ) - wClear;
	if( aMix <= Scalar( 0 ) ) {
		outAlbedo = RISEPel( 0, 0, 0 );
		outNormal = n0;
		return;
	}

	const IRayIntersectionModifier* pObjMod = ri.pPreMixModifier;

	// Weight each leaf by its conditional probability given hit acceptance
	// (w[i] * leafAlpha[i], and shortfall * 1), normalized by the total
	// accepted mass so leaves with per-child alpha stay exact.
	Scalar sumW = 0;
	Scalar sumEff = 0;
	for( size_t i = 0; i < n; ++i ) {
		sumW += w[i];
		if( w[i] <= Scalar( 0 ) ) {
			effW[i] = Scalar( 0 );
			continue;
		}
		const Scalar leafAlpha = children[i].pMaterial
			? children[i].pMaterial->AlphaCoverage( preModGeom )
			: Scalar( 1 );
		effW[i] = w[i] * leafAlpha;
		sumEff += effW[i];
	}
	const Scalar wAbs = r_max( Scalar( 0 ), aMix - sumW );
	sumEff += wAbs;
	if( sumEff <= Scalar( 0 ) ) {
		outAlbedo = RISEPel( 0, 0, 0 );
		outNormal = n0;
		return;
	}

	RISEPel sumAlbedo( 0, 0, 0 );
	Vector3 sumNormal( 0, 0, 0 );

	for( size_t i = 0; i < n; ++i ) {
		if( effW[i] <= Scalar( 0 ) ) {
			continue;
		}
		const Scalar p = effW[i] / sumEff;

		RayIntersectionGeometric childGeom( preModGeom );
		const IRayIntersectionModifier* pMod = children[i].pModifier ? children[i].pModifier : pObjMod;
		if( pMod ) {
			pMod->Modify( childGeom );
		}

		RISEPel leafAlbedo( 0, 0, 0 );
		if( bClayOverride && pClayBRDF ) {
			leafAlbedo = pClayBRDF->albedo( childGeom );
		} else if( const IMaterial* pChild = children[i].pMaterial ) {
			if( pChild->EvaluateVisibleGuideAlbedo( childGeom, leafAlbedo ) ) {
				// Leaf supplied a dedicated visible guide albedo (e.g. HyLIoS).
			} else if( const IBSDF* pBSDF = pChild->GetBSDF() ) {
				leafAlbedo = pBSDF->albedo( childGeom );
			} else if( pChild->GetSPF() ) {
				// Pure delta / specular leaf (has SPF, no BSDF): white albedo per OIDN.
				leafAlbedo = RISEPel( 1, 1, 1 );
			} else {
				// Absorber leaf (no BSDF, no SPF): black albedo (Round 3 P1-1).
				leafAlbedo = RISEPel( 0, 0, 0 );
			}
		}

		sumAlbedo = sumAlbedo + leafAlbedo * p;
		sumNormal = sumNormal + childGeom.vNormal * p;
	}

	// Shortfall absorbs (black albedo; or clay albedo when clay_lights overrides all hits).
	if( wAbs > Scalar( 0 ) ) {
		const Scalar pAbs = wAbs / sumEff;
		RayIntersectionGeometric absGeom( preModGeom );
		if( pObjMod ) {
			pObjMod->Modify( absGeom );
		}
		if( bClayOverride && pClayBRDF ) {
			sumAlbedo = sumAlbedo + pClayBRDF->albedo( absGeom ) * pAbs;
		}
		sumNormal = sumNormal + absGeom.vNormal * pAbs;
	}

	const Scalar nLen = Vector3Ops::Magnitude( sumNormal );
	outAlbedo = sumAlbedo;
	outNormal = ( nLen > NEARZERO ) ? ( sumNormal * ( Scalar( 1 ) / nLen ) ) : n0;
}

unsigned int MixMaterial::GetPathDependence() const
{
	unsigned int mask = IMaterial::GetPathDependence();
	bool anyVaryingWeight = false;
	if( pPassthroughWeight ) {
		mask |= pPassthroughWeight->GetPathDependence();
		if( !pPassthroughWeight->IsPositionIndependent() ) {
			anyVaryingWeight = true;
		}
	}
	bool hasSSSChild = false;
	for( size_t i = 0; i < children.size(); ++i ) {
		const MixMaterialChild& ch = children[i];
		if( ch.pMaterial ) {
			mask |= ch.pMaterial->GetPathDependence();
			if( ch.pMaterial->HasSubsurfaceLeaf() || ch.pMaterial->HasRandomWalkSSSLeaf() ) {
				hasSSSChild = true;
			}
		}
		if( ch.pWeight ) {
			mask |= ch.pWeight->GetPathDependence();
			if( !ch.pWeight->IsPositionIndependent() ) {
				anyVaryingWeight = true;
			}
		}
		if( ch.pModifier ) {
			mask |= ch.pModifier->GetPathDependence();
		}
	}
	// If any SSS leaf is present and any mix/passthrough weight varies spatially
	// (including sibling/passthrough weights that can normalize the SSS leaf via
	// sum > 1 clamping), flag kVaryingSSSMix so BDPT/VCM refuse it cleanly.
	if( hasSSSChild && anyVaryingWeight ) {
		mask |= PathDependence::kVaryingSSSMix;
	}
	return mask;
}
