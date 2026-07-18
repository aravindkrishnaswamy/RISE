//////////////////////////////////////////////////////////////////////
//
//  InteractivePelRasterizer.cpp
//
//  All defaults are "minimum-cost preview": no GI, no path guiding,
//  no adaptive sampling, no live-drag OIDN denoiser, 1 SPP.  We rely on the
//  default ctors of PathGuidingConfig / AdaptiveSamplingConfig /
//  StabilityConfig, all of which produce "disabled" state.  The
//  zsobol flag is left false: low-discrepancy ordering buys little
//  at 1 SPP.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "InteractivePelRasterizer.h"
#include "BlockRasterizeSequence.h"
#include "RayCaster.h"
#include "MortonRasterizeSequence.h"
#include "ScanlineRasterizeSequence.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/ISampling2D.h"
#include "../Interfaces/IShader.h"
#include "../RasterImages/RasterImage.h"
#include "../RISE_API.h"
#include "../Utilities/Color/ColorMath.h"
#include "../Utilities/Reference.h"
#include <cstring>
#include "../Interfaces/IEnumCallback.h"
#include "../Interfaces/IObjectManager.h"
// GUI render modes P2a (docs/gui/RENDER_MODES.md §6): CreateBeautyVariantPipeline
// builds a production-class PT pipeline directly (mirroring
// Job::SetPathTracingPelRasterizer's minimal path) -- these config structs are
// its default-constructed ("disabled"/"off") arguments.
#include "../Utilities/PathGuidingField.h"
#include "../Utilities/AdaptiveSamplingConfig.h"
#include "../Utilities/StabilityConfig.h"
#include "../Utilities/OidnConfig.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace {

class InteractiveMaterialPreviewShader :
	public IShader,
	public Reference
{
public:
	InteractiveMaterialPreviewShader(
		const unsigned int aoSamples,
		const Scalar aoStrength,
		const Scalar aoRadiusScale,
		const Scalar aoMaxDistance,
		const unsigned int wideAOSamples,
		const Scalar wideAOStrength,
		const Scalar wideAORadiusScale,
		const Scalar wideAOMaxDistance )
	: mAOSamples( aoSamples )
	, mAOStrength( aoStrength )
	, mAORadiusScale( aoRadiusScale )
	, mAOMaxDistance( aoMaxDistance )
	, mWideAOSamples( wideAOSamples )
	, mWideAOStrength( wideAOStrength )
	, mWideAORadiusScale( wideAORadiusScale )
	, mWideAOMaxDistance( wideAOMaxDistance )
	{}

	void Shade( const RuntimeContext& rc,
				const RayIntersection& ri,
				const IRayCaster& caster,
				const IRayCaster::RAY_STATE& rs,
				RISEPel& c,
				const IORStack& ior_stack ) const override
	{
		(void)rc;
		(void)rs;
		(void)ior_stack;
		c = PreviewPel( ri, caster );
	}

	Scalar ShadeNM( const RuntimeContext& rc,
					const RayIntersection& ri,
					const IRayCaster& caster,
					const IRayCaster::RAY_STATE& rs,
					const Scalar nm,
					const IORStack& ior_stack ) const override
	{
		(void)rc;
		(void)rs;
		(void)nm;
		(void)ior_stack;
		const RISEPel c = PreviewPel( ri, caster );
		return ( c[0] + c[1] + c[2] ) / 3.0;
	}

	void ResetRuntimeData() const override {}

protected:
	~InteractiveMaterialPreviewShader() override {}

private:
	unsigned int mAOSamples;
	Scalar       mAOStrength;
	Scalar       mAORadiusScale;
	Scalar       mAOMaxDistance;
	unsigned int mWideAOSamples;
	Scalar       mWideAOStrength;
	Scalar       mWideAORadiusScale;
	Scalar       mWideAOMaxDistance;

	static Scalar Clamp01( const Scalar v )
	{
		return v < 0.0 ? 0.0 : ( v > 1.0 ? 1.0 : v );
	}

	static Scalar ClampRange( const Scalar v, const Scalar lo, const Scalar hi )
	{
		return v < lo ? lo : ( v > hi ? hi : v );
	}

	static RISEPel MaterialAlbedo( const RayIntersection& ri )
	{
		RISEPel albedo( 0.72, 0.72, 0.72 );
		if( ri.pMaterial ) {
			const IBSDF* bsdf = ri.pMaterial->GetBSDF();
			if( bsdf ) {
				albedo = bsdf->albedo( ri.geometric );
			}
		}
		ColorMath::Clamp( albedo, 0.08, 1.0 );
		return albedo;
	}

	static unsigned int Hash32( unsigned int x )
	{
		x ^= x >> 16;
		x *= 0x7feb352du;
		x ^= x >> 15;
		x *= 0x846ca68bu;
		x ^= x >> 16;
		return x;
	}

	static Scalar Random01(
		const unsigned int x,
		const unsigned int y,
		const unsigned int sample,
		const unsigned int dimension,
		const unsigned int salt )
	{
		const unsigned int h = Hash32(
			x * 1973u ^
			y * 9277u ^
			sample * 26699u ^
			dimension * 6271u ^
			salt * 3623u );
		return static_cast<Scalar>( h & 0x00ffffffu ) * ( 1.0 / 16777216.0 );
	}

	static void BuildBasis( const Vector3& n, Vector3& tangent, Vector3& bitangent )
	{
		tangent = Vector3Ops::Perpendicular( n );
		Vector3Ops::NormalizeMag( tangent );
		bitangent = Vector3Ops::Cross( n, tangent );
		Vector3Ops::NormalizeMag( bitangent );
	}

	static Vector3 HemisphereDirection(
		const Vector3& n,
		const Vector3& tangent,
		const Vector3& bitangent,
		const RayIntersection& ri,
		const unsigned int sample,
		const unsigned int sampleCount,
		const unsigned int salt )
	{
		const unsigned int strataX =
			static_cast<unsigned int>( ceil( sqrt( static_cast<double>( sampleCount ) ) ) );
		const unsigned int strataY = ( sampleCount + strataX - 1 ) / strataX;
		const unsigned int sx = sample % strataX;
		const unsigned int sy = sample / strataX;

		const Scalar jx = Random01( ri.geometric.rast.x, ri.geometric.rast.y, sample, 0, salt );
		const Scalar jy = Random01( ri.geometric.rast.x, ri.geometric.rast.y, sample, 1, salt );
		const Scalar u = ClampRange(
			( static_cast<Scalar>( sx ) + jx ) / static_cast<Scalar>( strataX ),
			0.0,
			1.0 - 1.0e-6 );
		const Scalar v = ClampRange(
			( static_cast<Scalar>( sy ) + jy ) / static_cast<Scalar>( strataY ),
			0.0,
			1.0 - 1.0e-6 );

		const Scalar r = sqrt( u );
		const Scalar phi = TWO_PI * v;
		const Scalar localX = r * cos( phi );
		const Scalar localY = r * sin( phi );
		const Scalar localZ = sqrt( 1.0 - u );
		Vector3 dir =
			tangent * localX +
			bitangent * localY +
			n * localZ;
		Vector3Ops::NormalizeMag( dir );
		return dir;
	}

	Scalar AmbientOcclusion(
		const RayIntersection& ri,
		const IRayCaster& caster,
		const Vector3& n,
		const unsigned int samples,
		const Scalar strength,
		const Scalar radiusScale,
		const Scalar maxDistance,
		const unsigned int salt ) const
	{
		if( samples == 0 || strength <= 0.0 ) {
			return 1.0;
		}

		Vector3 tangent;
		Vector3 bitangent;
		BuildBasis( n, tangent, bitangent );

		const Scalar radius = ClampRange(
			ri.geometric.range * radiusScale,
			Scalar( 0.03 ),
			maxDistance );
		const Scalar eps = ClampRange( radius * Scalar( 0.0005 ), Scalar( 1.0e-5 ), Scalar( 1.0e-3 ) );
		const Point3 origin = Point3Ops::mkPoint3( ri.geometric.ptIntersection, n * eps );

		unsigned int hits = 0;
		for( unsigned int i = 0; i < samples; ++i ) {
			const Vector3 dir = HemisphereDirection( n, tangent, bitangent, ri, i, samples, salt );
			if( caster.CastShadowRay( Ray( origin, dir ), radius ) ) {
				++hits;
			}
		}

		const Scalar occlusion = static_cast<Scalar>( hits ) / static_cast<Scalar>( samples );
		return Clamp01( 1.0 - strength * occlusion );
	}

	Scalar CombinedAmbientOcclusion( const RayIntersection& ri, const IRayCaster& caster, const Vector3& n ) const
	{
		const Scalar contact = AmbientOcclusion(
			ri,
			caster,
			n,
			mAOSamples,
			mAOStrength,
			mAORadiusScale,
			mAOMaxDistance,
			17u );
		const Scalar wide = AmbientOcclusion(
			ri,
			caster,
			n,
			mWideAOSamples,
			mWideAOStrength,
			mWideAORadiusScale,
			mWideAOMaxDistance,
			43u );
		return contact * wide;
	}

	static Scalar MaterialAOWeight( const RayIntersection& ri, const Vector3& n, const Vector3& v )
	{
		const IMaterial* material = ri.pMaterial;
		if( !material ) {
			return 1.0;
		}

		Scalar weight = 1.0;
		if( material->GetEmitter() ) {
			weight *= 0.18;
		}
		if( material->CouldLightPassThrough() ) {
			weight *= 0.28;
		}
		if( material->IsVolumetric() ||
			material->GetDiffusionProfile() ||
			material->GetRandomWalkSSSParams() ) {
			weight *= 0.62;
		}

		IORStack iorStack( 1.0 );
		if( ri.pObject ) {
			iorStack.SetCurrentObject( ri.pObject );
		}
		const SpecularInfo specularInfo = material->GetSpecularInfo( ri.geometric, iorStack );
		if( specularInfo.valid && specularInfo.isSpecular ) {
			weight *= specularInfo.canRefract ? 0.20 : 0.34;
		}

		const IBSDF* bsdf = material->GetBSDF();
		if( bsdf ) {
			RayIntersectionGeometric rig( ri.geometric );
			rig.vNormal = n;
			rig.onb.CreateFromW( n );

			const Vector3 mirror = n * ( 2.0 * Vector3Ops::Dot( n, v ) ) - v;
			const RISEPel normalResponse = bsdf->value( n, rig );
			const RISEPel mirrorResponse = bsdf->value( mirror, rig );
			const Scalar normalMax = ColorMath::MaxValue( normalResponse );
			const Scalar mirrorMax = ColorMath::MaxValue( mirrorResponse );
			const Scalar glossyBias = Clamp01(
				( mirrorMax - normalMax ) /
				( mirrorMax + normalMax + Scalar( 1.0e-4 ) ) );
			weight *= 1.0 - 0.55 * glossyBias;
		} else {
			weight *= 0.45;
		}

		return ClampRange( weight, 0.10, 1.0 );
	}

	static Scalar ApplyAOWeight( const Scalar ao, const Scalar weight )
	{
		return 1.0 - ( 1.0 - ao ) * weight;
	}

	static Scalar DirectionalTerm( const Vector3& n, const Vector3& direction )
	{
		return Clamp01( Vector3Ops::Dot( n, direction ) );
	}

	static RISEPel EvaluateStudioLight(
		const IBSDF* bsdf,
		const RayIntersection& ri,
		const Vector3& n,
		const Vector3& direction,
		const RISEPel& color,
		const Scalar intensity )
	{
		const Scalar nDotL = DirectionalTerm( n, direction );
		if( nDotL <= 0.0 ) {
			return RISEPel( 0, 0, 0 );
		}

		if( !bsdf ) {
			return color * ( nDotL * intensity );
		}

		RayIntersectionGeometric rig( ri.geometric );
		rig.vNormal = n;
		rig.onb.CreateFromW( n );

		RISEPel f = bsdf->value( direction, rig );
		ColorMath::Clamp( f, 0.0, 8.0 );
		return color * f * ( nDotL * intensity );
	}

	static RISEPel StudioLighting( const RayIntersection& ri, const Vector3& n, const Vector3& v, const RISEPel& base )
	{
		Vector3 key( -0.38, 0.56, 0.74 );
		Vector3 fill( 0.68, 0.18, 0.35 );
		Vector3 top( 0.0, 1.0, 0.0 );
		Vector3Ops::NormalizeMag( key );
		Vector3Ops::NormalizeMag( fill );
		Vector3Ops::NormalizeMag( top );

		const Scalar halfLambert = 0.5 + 0.5 * DirectionalTerm( n, v );
		const IBSDF* bsdf = ri.pMaterial ? ri.pMaterial->GetBSDF() : 0;

		RISEPel result = base * ( RISEPel( 0.11, 0.12, 0.14 ) + RISEPel( 0.10, 0.10, 0.10 ) * halfLambert );
		result = result + EvaluateStudioLight(
			bsdf,
			ri,
			n,
			key,
			RISEPel( 0.92, 0.86, 0.74 ),
			2.65 );
		result = result + EvaluateStudioLight(
			bsdf,
			ri,
			n,
			fill,
			RISEPel( 0.34, 0.40, 0.56 ),
			1.15 );
		result = result + EvaluateStudioLight(
			bsdf,
			ri,
			n,
			top,
			RISEPel( 0.38, 0.40, 0.43 ),
			0.85 );

		if( ColorMath::MaxValue( result ) < 0.035 ) {
			result = base * ( RISEPel( 0.20, 0.21, 0.24 ) + RISEPel( 0.18, 0.18, 0.18 ) * halfLambert );
		}
		return result;
	}

	RISEPel PreviewPel( const RayIntersection& ri, const IRayCaster& caster ) const
	{
		Vector3 n = ri.geometric.vNormal;
		Vector3 v = -ri.geometric.ray.Dir();
		Vector3Ops::NormalizeMag( n );
		Vector3Ops::NormalizeMag( v );

		Scalar ndvSigned = Vector3Ops::Dot( n, v );
		if( ndvSigned < 0.0 ) {
			n = -n;
			ndvSigned = -ndvSigned;
		}
		const Scalar ndv = Clamp01( ndvSigned );
		const Scalar rim = ( 1.0 - ndv ) * ( 1.0 - ndv );

		const RISEPel base = MaterialAlbedo( ri );
		const Scalar ao = ApplyAOWeight(
			CombinedAmbientOcclusion( ri, caster, n ),
			MaterialAOWeight( ri, n, v ) );
		RISEPel result = StudioLighting( ri, n, v, base ) * ao + RISEPel( 0.16, 0.18, 0.22 ) * rim;
		ColorMath::Clamp( result, 0.0, 1.0 );
		return result;
	}
};

class InteractiveMaterialPreviewRayCaster :
	public RayCaster
{
public:
	//! `showLuminaires` defaults FALSE so the material-preview / draft
	//! factory behaviour is byte-identical to before (a directly-visible
	//! emitter is suppressed to background, matching the studio-preview
	//! look).  The objectmap factory passes TRUE: an emissive object is a
	//! real, selectable, world-visible object that MUST appear in the
	//! identity segmentation (otherwise RayCaster forces bHit=false for
	//! eRayView rays hitting an emitter -- BEFORE the ObjectIdShader runs --
	//! and the object silently renders as background with a permanent
	//! pixelCount:0 legend entry).
	explicit InteractiveMaterialPreviewRayCaster( const IShader& shader,
	                                              const bool showLuminaires = false )
	: RayCaster( /*seeRadianceMap*/false,
				 /*maxR*/1,
				 shader,
				 /*showLuminaires*/showLuminaires )
	{}

protected:
	const IShader& SelectShader( const RayIntersection& /*ri*/ ) const override
	{
		return pDefaultShader;
	}
};

//! Toolkit slice 3a (objectmap): emits each hit object's flat identity
//! colour -- the LINEAR pre-image chosen so the sRGB encode truncates to
//! the object's reserved palette byte -- instead of shaded radiance.
//! Bumps the object's per-id atomic pixel tally exactly once per shaded
//! primary-ray hit (the single-ray, no-kernel IntegratePixel branch calls
//! Shade at most once per pixel; the tally is atomic because the block
//! workers run Shade concurrently).  A hit whose pObject is null or absent
//! from the registry decodes to the reserved UNKNOWN colour and tallies
//! into the last `counts` slot.  Borrows the palette (does NOT own it);
//! the caller (AgentSession) guarantees it outlives the render.
class ObjectIdShader :
	public IShader,
	public Reference
{
public:
	explicit ObjectIdShader( const ObjectMapPalette& palette )
	: mPalette( &palette )
	{}

	void Shade( const RuntimeContext& rc,
				const RayIntersection& ri,
				const IRayCaster& caster,
				const IRayCaster::RAY_STATE& rs,
				RISEPel& c,
				const IORStack& ior_stack ) const override
	{
		(void)rc;
		(void)caster;
		(void)rs;
		(void)ior_stack;
		c = LookupAndTally( ri, /*tally*/true );
	}

	Scalar ShadeNM( const RuntimeContext& rc,
					const RayIntersection& ri,
					const IRayCaster& caster,
					const IRayCaster::RAY_STATE& rs,
					const Scalar nm,
					const IORStack& ior_stack ) const override
	{
		(void)rc;
		(void)caster;
		(void)rs;
		(void)nm;
		(void)ior_stack;
		// The objectmap pipeline is strictly Pel (InteractivePelRasterizer
		// -> Shade), so ShadeNM never fires in practice.  Kept honest as a
		// pure read with NO tally, so a stray spectral caster could never
		// double-count against the Pel path's tally.
		const RISEPel c = LookupAndTally( ri, /*tally*/false );
		return ( c[0] + c[1] + c[2] ) / 3.0;
	}

	void ResetRuntimeData() const override {}

protected:
	~ObjectIdShader() override {}

private:
	const ObjectMapPalette* mPalette;

	RISEPel LookupAndTally( const RayIntersection& ri, const bool tally ) const
	{
		const IObject* obj = ri.pObject;
		if( obj ) {
			std::unordered_map<const IObject*, std::uint32_t>::const_iterator it =
				mPalette->registry.find( obj );
			if( it != mPalette->registry.end() ) {
				const std::uint32_t id = it->second;
				if( tally ) {
					mPalette->counts[id].fetch_add( 1u, std::memory_order_relaxed );
				}
				return mPalette->linearColors[id];
			}
		}
		// Unknown: null pObject, or an object not in the registry.  Its
		// tally lives in the last slot (index == names.size()).
		if( tally ) {
			mPalette->counts[mPalette->linearColors.size()].fetch_add( 1u, std::memory_order_relaxed );
		}
		return mPalette->unknownLinear;
	}
};

//! GUI render modes P1 (docs/gui/RENDER_MODES.md): world-space shading-
//! normal false colour -- c = 0.5*(N+1).  Deliberately NOT flipped
//! toward the viewer: a back-facing normal renders in the "negative"
//! half of the colour cube, which is exactly the orientation defect
//! the mode exists to reveal.
//!
//! X-ray axis (docs/gui/RENDER_MODES.md "X-ray axis"): resolution now
//! lives in the CASTER layer (RayCaster::ResolveXrayView_, enabled via
//! RayCaster::SetXrayViewResolve) rather than here -- by the time `ri`
//! reaches Shade/ShadeNM it is already the resolved hit, so this shader
//! is x-ray-agnostic.
class NormalsViewShader :
	public IShader,
	public Reference
{
public:
	NormalsViewShader() {}

	void Shade( const RuntimeContext& rc,
				const RayIntersection& ri,
				const IRayCaster& caster,
				const IRayCaster::RAY_STATE& rs,
				RISEPel& c,
				const IORStack& ior_stack ) const override
	{
		(void)rc;
		(void)caster;
		(void)rs;
		(void)ior_stack;
		c = NormalColor( ri );
	}

	Scalar ShadeNM( const RuntimeContext& rc,
					const RayIntersection& ri,
					const IRayCaster& caster,
					const IRayCaster::RAY_STATE& rs,
					const Scalar nm,
					const IORStack& ior_stack ) const override
	{
		(void)nm;
		RISEPel c;
		Shade( rc, ri, caster, rs, c, ior_stack );
		return ( c[0] + c[1] + c[2] ) / 3.0;
	}

	void ResetRuntimeData() const override {}

protected:
	~NormalsViewShader() override {}

private:
	static RISEPel NormalColor( const RayIntersection& ri )
	{
		Vector3 n = ri.geometric.vNormal;
		Vector3Ops::NormalizeMag( n );
		return RISEPel( 0.5 * ( n.x + 1.0 ), 0.5 * ( n.y + 1.0 ), 0.5 * ( n.z + 1.0 ) );
	}
};

//! Headlamp-shaded GEOMETRIC normal (flat face normal -- no Phong
//! smoothing, no bump/normal-map perturbation).  Reveals the actual
//! tessellation and any shading-normal divergence.
//! X-ray axis: resolution lives in the caster layer -- see
//! NormalsViewShader's class doc.
class FacetsViewShader :
	public IShader,
	public Reference
{
public:
	FacetsViewShader() {}

	void Shade( const RuntimeContext& rc,
				const RayIntersection& ri,
				const IRayCaster& caster,
				const IRayCaster::RAY_STATE& rs,
				RISEPel& c,
				const IORStack& ior_stack ) const override
	{
		(void)rc;
		(void)caster;
		(void)rs;
		(void)ior_stack;
		c = FacetShade( ri );
	}

	Scalar ShadeNM( const RuntimeContext& rc,
					const RayIntersection& ri,
					const IRayCaster& caster,
					const IRayCaster::RAY_STATE& rs,
					const Scalar nm,
					const IORStack& ior_stack ) const override
	{
		(void)rc;
		(void)caster;
		(void)rs;
		(void)nm;
		(void)ior_stack;
		const RISEPel c = FacetShade( ri );
		return ( c[0] + c[1] + c[2] ) / 3.0;
	}

	void ResetRuntimeData() const override {}

	//! Shared with WireframeViewShader (its non-edge base shade).
	static RISEPel FacetShade( const RayIntersection& ri )
	{
		Vector3 gn = ri.geometric.vGeomNormal;
		Vector3Ops::NormalizeMag( gn );
		Vector3 v = -ri.geometric.ray.Dir();
		Vector3Ops::NormalizeMag( v );
		Scalar ndv = Vector3Ops::Dot( gn, v );
		if( ndv < 0.0 ) {
			ndv = -ndv;
		}
		if( ndv > 1.0 ) {
			ndv = 1.0;
		}
		const Scalar shade = 0.12 + 0.88 * ndv;
		return RISEPel( 0.78 * shade, 0.78 * shade, 0.80 * shade );
	}

protected:
	~FacetsViewShader() override {}
};

//! Per-pass AUTO-WINDOWED hit distance, near = bright.  FIXES the earlier
//! fixed scene-diagonal normalization, which flattened staged scenes (a
//! subject scaled ~50 units, camera at ~95, background studio panels at
//! ~400) into a ~1% brightness band -- the whole subject read as a blank
//! near-uniform grey.
//!
//! Calibration: `mAccMin`/`mAccMax` are atomic accumulators (CAS-loop
//! min/max on std::atomic<double>, seeded with FINITE sentinels --
//! 1e300 / -1e300, NEVER infinity; an inf-seeded reduction miscompiles
//! under this repo's -ffast-math + LTO build, see CLAUDE.md) that every
//! Shade/ShadeNM call on the CURRENT pass folds its hit distance into.
//! `PreparePass`, called once per pass (single-threaded, BEFORE the
//! parallel block workers dispatch -- see InteractiveViewModeRayCaster::
//! AttachScene below) snapshots the PREVIOUS pass's accumulators into the
//! active window {mWinMin, mWinMax, mWinKind} that THIS pass shades
//! against, then resets the accumulators to sentinels for this pass to
//! fill.
//!
//! External review P2 fix (2026-07): the window is a THREE-state
//! `WindowKind` -- Unarmed / Ranged / Flat -- not a `bool mWinValid`.
//! The degenerate-span cutoff (max-min not more than 1e-9 of scale --
//! e.g. a single flat plane, or a wall, filling the frame) used to be a
//! REFUSAL: PreparePass left the window exactly as it was (Unarmed if
//! it had never armed, or the STALE previous Ranged window if it had).
//! That produced two bugs. (a) A view that NEVER arms (a scene that is
//! degenerate from the very first pass) stayed Unarmed forever, and
//! WindowStale()'s "still on the fallback" case is unconditionally true
//! for Unarmed -- so a permanently-flat scene re-ran the Part-B
//! self-calibration extra pass on EVERY single call, forever, not just
//! the first. (b) A jump from a genuinely-ranged view to a flat one (camera
//! swings to look straight at a wall) left the OLD Ranged window active
//! (PreparePass's refusal doesn't touch it), so DepthValue kept remapping
//! the new, single depth value through a window that no longer describes
//! the scene -- an arbitrary, usually-wrong clamp across the whole frame
//! instead of an honest "this view is flat" answer.
//!
//! The fix makes Flat a FIRST-CLASS armed state: PreparePass arms it
//! (mWinMin == mWinMax == the pass's single depth value) instead of
//! refusing, using the SAME 1e-9-relative cutoff as the classification
//! test rather than a reason to skip.  DepthValue shades a constant
//! mid-gray for a Flat window (see its own doc for why).  WindowStale
//! walks all four Ranged/Flat x Ranged/Flat transitions explicitly (see
//! its own doc) instead of only ever comparing against a Ranged window.
//!
//! With a Ranged window: t = 0.08 + 0.92 * clamp01((winMax-d)/(winMax-
//! winMin)), d outside the window clamps to the nearest edge.  With no
//! armed window yet (Unarmed -- first pass ever, and every pass has
//! remained degenerate since, until Flat arms on the SECOND such pass)
//! FALLS BACK to the original fixed scene-diagonal formula, keyed off
//! the scene bounding-box diagonal `mSceneDiagonal` (captured the same
//! way as before -- see AttachScene).
//!
//! X-ray axis (docs/gui/RENDER_MODES.md "X-ray axis"): resolution lives
//! in the CASTER layer (RayCaster::ResolveXrayView_, enabled via
//! RayCaster::SetXrayViewResolve) -- when it fires, the caster already
//! recomputes `ri.geometric.range` as the TOTAL distance from the
//! ORIGINAL ray's origin to the resolved hit point (NOT the resolved
//! hit's own last-segment range, and NOT a sum of per-segment ranges)
//! before this shader ever sees `ri`.  So Shade/ShadeNM simply
//! reads `ri.geometric.range` -- no x-ray-specific knowledge needed here.
class DepthViewShader :
	public IShader,
	public Reference
{
public:
	//! Three-state calibration window -- see the class doc's external-
	//! review P2 fix note.  Unarmed: no window has ever armed (or every
	//! pass since construction has been degenerate) -- DepthValue uses
	//! the flat scene-diagonal fallback.  Ranged: a genuine [min,max]
	//! depth extent is active.  Flat: the scene IS a single depth value
	//! (within the degenerate-span tolerance) -- DepthValue shades a
	//! constant mid-gray rather than remapping through a meaningless
	//! zero-width window.
	enum class WindowKind { Unarmed, Ranged, Flat };

	DepthViewShader()
	: mSceneDiagonal( 0.0 )
	, mWinKind( WindowKind::Unarmed )
	, mWinMin( 0.0 )
	, mWinMax( 0.0 )
	, mAccMin( kSentinelMin )
	, mAccMax( kSentinelMax )
	{}

	//! Per-pass calibration hook -- see the class doc.  Called once per
	//! RasterizeScene pass, single-threaded, BEFORE the parallel block
	//! workers dispatch (InteractiveViewModeRayCaster::AttachScene runs
	//! at the top of every pass).  `d` is the scene bounding-box diagonal
	//! (0 if unbounded/empty) -- the fallback formula's input.
	void PreparePass( const Scalar d )
	{
		mSceneDiagonal = d;

		const double accMin = mAccMin.load( std::memory_order_relaxed );
		const double accMax = mAccMax.load( std::memory_order_relaxed );
		// accMin <= kAlmostSentinel is the "did any sample land last pass"
		// test -- compared against a large FINITE threshold rather than
		// std::isfinite, per the class doc's -ffast-math note (value-level
		// isfinite guards are unreliable under -ffast-math/LTO in this repo).
		if( accMin <= kAlmostSentinel )
		{
			const double range = accMax - accMin;
			const double scale = accMax > 1.0 ? accMax : 1.0;
			if( range > 1.0e-9 * scale )
			{
				mWinMin = accMin;
				mWinMax = accMax;
				mWinKind = WindowKind::Ranged;
			}
			else
			{
				// External review P2 fix: ARM a first-class Flat window
				// (min==max, this pass's own single depth value) instead
				// of refusing to arm at all.  A permanently-degenerate
				// scene (a single flat plane filling the frame) now
				// settles into Flat on the pass immediately after the
				// one that discovers it, rather than staying Unarmed --
				// and therefore "still on the fallback" -- forever.  See
				// the class doc's bug (a).
				mWinMin = accMin;
				mWinMax = accMin;
				mWinKind = WindowKind::Flat;
			}
		}
		// else: no samples landed last pass -- leave whatever window was
		// already active (Unarmed / Ranged / Flat) unchanged.

		// Reset the accumulators for THIS pass to fill, regardless.
		mAccMin.store( kSentinelMin, std::memory_order_relaxed );
		mAccMax.store( kSentinelMax, std::memory_order_relaxed );
	}

	void Shade( const RuntimeContext& rc,
				const RayIntersection& ri,
				const IRayCaster& caster,
				const IRayCaster::RAY_STATE& rs,
				RISEPel& c,
				const IORStack& ior_stack ) const override
	{
		(void)rc;
		(void)caster;
		(void)rs;
		(void)ior_stack;
		const Scalar d = ri.geometric.range;
		RecordSample( d );
		const Scalar t = DepthValue( d );
		c = RISEPel( t, t, t );
	}

	Scalar ShadeNM( const RuntimeContext& rc,
					const RayIntersection& ri,
					const IRayCaster& caster,
					const IRayCaster::RAY_STATE& rs,
					const Scalar nm,
					const IORStack& ior_stack ) const override
	{
		(void)rc;
		(void)caster;
		(void)rs;
		(void)nm;
		(void)ior_stack;
		const Scalar d = ri.geometric.range;
		RecordSample( d );
		return DepthValue( d );
	}

	//! Query for the InteractivePelRasterizer's Part-B self-calibration
	//! re-run (docs/gui/RENDER_MODES.md "Depth axis").  True whenever a
	//! second pass would meaningfully change what got shaded THIS pass.
	//! At-most-one-re-run + cancellation behaviour (the caller site,
	//! InteractiveViewModeRayCaster::DepthWindowStale /
	//! InteractivePelRasterizer::RasterizeScene) is UNCHANGED by this
	//! fix -- only the classification below changed.
	//!
	//! External review P2 fix: walks all four WindowKind x new-pass-kind
	//! transitions explicitly (`mWinKind` is the window the pass that
	//! just ran was SHADED AGAINST; `newIsFlat` classifies the pass's
	//! OWN accumulated min/max -- not yet folded into mWinMin/mWinMax,
	//! since that only happens at the START of the NEXT pass via
	//! PreparePass -- using the identical 1e-9-relative cutoff
	//! PreparePass uses to classify Ranged vs Flat):
	//!
	//!   Unarmed -> *      : always stale.  The pass that just ran used
	//!                       the flat scene-diagonal fallback (no window
	//!                       armed yet); a second pass now shades
	//!                       against a real (Ranged or Flat) window.
	//!                       Mirrors PreparePass's "did any sample land"
	//!                       test so the two stay in lockstep.
	//!
	//!   Ranged -> Ranged  : stale if either (2a) a new endpoint lands
	//!                       outside the old window by more than 15% of
	//!                       the old window's span (the window no longer
	//!                       covers what is visible -- e.g. a camera
	//!                       jump/crop/edit revealed geometry the old
	//!                       window never saw), or (2b) the span itself
	//!                       changed by more than 30% (zooming from a
	//!                       wide shot into a close-up, or back out).
	//!                       Thresholds are picked so mild per-pass drift
	//!                       during smooth navigation (dolly, orbit)
	//!                       stays under them.
	//!
	//!   Ranged -> Flat    : always stale.  The view collapsed to a
	//!                       single depth (e.g. camera now looking
	//!                       straight at a wall) -- the pass that just
	//!                       ran shaded windowed against a range that no
	//!                       longer exists; the Flat window needs to arm
	//!                       before this pass's own output (a stale
	//!                       windowed clamp, the class doc's bug (b)) is
	//!                       trustworthy.
	//!
	//!   Flat -> Ranged    : always stale.  The view gained genuine
	//!                       depth extent (e.g. camera panned off the
	//!                       wall) -- the flat mid-gray this pass shaded
	//!                       is no longer an honest answer.
	//!
	//!   Flat -> Flat      : NOT stale as long as the new flat value sits
	//!                       within a small tolerance of the old one --
	//!                       same 15%-of-span SHAPE as the Ranged->Ranged
	//!                       2a check, but a Flat window's own span is
	//!                       ~0 by construction (mWinMin == mWinMax), so
	//!                       a literal `0.15 * oldSpan` tolerance would
	//!                       be zero and flag every Flat pass as "moved"
	//!                       from float noise alone.  Uses a NOMINAL span
	//!                       -- max(oldSpan, 1e-9 * scale), the same
	//!                       degenerate-cutoff scale PreparePass uses --
	//!                       as the tolerance base instead: float noise
	//!                       stays under it, a real shift (dolly along
	//!                       the flat wall, an edit that moves the plane)
	//!                       exceeds it and re-arms.
	bool WindowStale() const
	{
		const double accMin = mAccMin.load( std::memory_order_relaxed );
		const double accMax = mAccMax.load( std::memory_order_relaxed );
		const bool bHasSamples = accMin <= kAlmostSentinel;
		if( !bHasSamples ) {
			return false;   // nothing rendered this pass -- nothing to react to
		}

		if( mWinKind == WindowKind::Unarmed ) {
			return true;   // Unarmed -> *: still on the flat fallback
		}

		const double newSpan  = accMax - accMin;
		const double newScale = accMax > 1.0 ? accMax : 1.0;
		// Same degenerate-span classification PreparePass uses to pick
		// Ranged vs Flat when it next arms.
		const bool newIsFlat = !( newSpan > 1.0e-9 * newScale );

		if( mWinKind == WindowKind::Ranged )
		{
			const double oldSpan = mWinMax - mWinMin;
			if( oldSpan <= 0.0 ) {
				return false;   // shouldn't happen (Ranged implies span > eps) -- defensive
			}

			if( newIsFlat ) {
				return true;   // Ranged -> Flat: always stale
			}

			// Ranged -> Ranged: case (2a) new endpoints outside the OLD
			// window by > 15% of its span.
			const double tolExpand = 0.15 * oldSpan;
			if( accMin < mWinMin - tolExpand || accMax > mWinMax + tolExpand ) {
				return true;
			}

			// case (2b): span changed by > 30%.
			const double spanRatio = newSpan / oldSpan;
			if( spanRatio > 1.30 || spanRatio < 0.70 ) {
				return true;
			}

			return false;
		}

		// mWinKind == WindowKind::Flat
		if( !newIsFlat ) {
			return true;   // Flat -> Ranged: always stale
		}

		// Flat -> Flat: tolerance against a NOMINAL span -- see the
		// method doc's transition table for why a literal `oldSpan`
		// (~0 for a Flat window) can't be used directly.
		const double oldSpan = mWinMax - mWinMin;   // ~0 for a Flat window
		const double oldScale = mWinMax > 1.0 ? mWinMax : 1.0;
		const double nominalSpan = oldSpan > ( 1.0e-9 * oldScale ) ? oldSpan : ( 1.0e-9 * oldScale );
		const double tolExpand = 0.15 * nominalSpan;
		if( accMin < mWinMin - tolExpand || accMax > mWinMax + tolExpand ) {
			return true;
		}
		return false;
	}

	void ResetRuntimeData() const override {}

protected:
	~DepthViewShader() override {}

private:
	// CAS-loop atomic min/max seeds -- large FINITE sentinels, never
	// infinity (see the class doc's -ffast-math note).
	static constexpr double kSentinelMin    = 1.0e300;
	static constexpr double kSentinelMax    = -1.0e300;
	static constexpr double kAlmostSentinel = 1.0e299;

	Scalar                       mSceneDiagonal;
	WindowKind                   mWinKind;
	double                       mWinMin;
	double                       mWinMax;
	mutable std::atomic<double>  mAccMin;
	mutable std::atomic<double>  mAccMax;

	static void AtomicMin( std::atomic<double>& slot, const double v )
	{
		double cur = slot.load( std::memory_order_relaxed );
		while( v < cur && !slot.compare_exchange_weak( cur, v, std::memory_order_relaxed ) ) {}
	}
	static void AtomicMax( std::atomic<double>& slot, const double v )
	{
		double cur = slot.load( std::memory_order_relaxed );
		while( v > cur && !slot.compare_exchange_weak( cur, v, std::memory_order_relaxed ) ) {}
	}

	void RecordSample( const Scalar d ) const
	{
		AtomicMin( mAccMin, static_cast<double>( d ) );
		AtomicMax( mAccMax, static_cast<double>( d ) );
	}

	//! Ranged: t = 0.08 + 0.92 * clamp01((winMax-d)/(winMax-winMin)).
	//! Flat: a constant mid-gray (0.6) -- NOT the ranged formula's near
	//! edge (0.08, would read as "very far", wrong) or far edge (1.0,
	//! "very near", also wrong), and NOT the Unarmed fallback (which
	//! keys off the SCENE diagonal, not this view's actual, known-flat
	//! depth).  With mWinMin == mWinMax, (winMax-d)/(winMax-winMin) is a
	//! 0/0 that has no principled value -- there is no gradient to
	//! display when every visible pixel is (within tolerance) the same
	//! distance from the camera.  0.6 is a deliberately neutral value
	//! roughly mid-way up the Ranged formula's own [0.08, 1.0] range, so
	//! a Flat view reads as visually DISTINCT from both ends of the
	//! ranged palette -- "this view is uniform depth", not a guess at a
	//! near or far distance.
	Scalar DepthValue( const Scalar d ) const
	{
		if( mWinKind == WindowKind::Ranged )
		{
			Scalar t = ( mWinMax - mWinMin ) > 0.0 ? ( mWinMax - d ) / ( mWinMax - mWinMin ) : 1.0;
			if( t < 0.0 ) t = 0.0;
			if( t > 1.0 ) t = 1.0;
			return 0.08 + 0.92 * t;
		}
		if( mWinKind == WindowKind::Flat )
		{
			return 0.6;
		}
		if( mSceneDiagonal > 0.0 ) {
			return 1.0 / ( 1.0 + 3.0 * d / mSceneDiagonal );
		}
		return 1.0 / ( 1.0 + d );
	}
};

//! First-hit triangle edges over dim facet shading.  Consumes the
//! closest-edge point the mesh intersectors stamp when the caster
//! requests it (RayIntersectionGeometric::bWantsWireEdgeInfo); the
//! world-space distance |ptIntersection - ptWireNearestEdge| is exact
//! under any affine transform because Object::IntersectRay transforms
//! the edge point exactly like ptIntersection.  Line width is range-
//! proportional (approximately constant apparent width under
//! perspective).  Geometries without polygon edges (analytical
//! primitives, SDFs) never stamp the info -- they render as dim facet
//! shading with no lines, which is the honest answer.
//! X-ray axis: resolution lives in the caster layer -- see
//! NormalsViewShader's class doc.
class WireframeViewShader :
	public IShader,
	public Reference
{
public:
	WireframeViewShader() {}

	void Shade( const RuntimeContext& rc,
				const RayIntersection& ri,
				const IRayCaster& caster,
				const IRayCaster::RAY_STATE& rs,
				RISEPel& c,
				const IORStack& ior_stack ) const override
	{
		(void)rc;
		(void)caster;
		(void)rs;
		(void)ior_stack;
		c = WirePel( ri );
	}

	Scalar ShadeNM( const RuntimeContext& rc,
					const RayIntersection& ri,
					const IRayCaster& caster,
					const IRayCaster::RAY_STATE& rs,
					const Scalar nm,
					const IORStack& ior_stack ) const override
	{
		(void)rc;
		(void)caster;
		(void)rs;
		(void)nm;
		(void)ior_stack;
		const RISEPel c = WirePel( ri );
		return ( c[0] + c[1] + c[2] ) / 3.0;
	}

	void ResetRuntimeData() const override {}

protected:
	~WireframeViewShader() override {}

private:
	//! Fraction of the hit distance that reads as one line half-width;
	//! ~2.5 apparent pixels at 800 px / 40 degree fov.  A constant is a
	//! deliberate simplification -- the shader has no fov/raster-height
	//! access; thread the real pixel footprint through if that ever
	//! becomes worth it.
	static Scalar LineWidthScale() { return 0.0035; }

	static RISEPel WirePel( const RayIntersection& ri )
	{
		RISEPel base = FacetsViewShader::FacetShade( ri );
		base = base * 0.30;
		if( !ri.geometric.bHasWireEdgeInfo ) {
			return base;
		}
		const Scalar dist = Vector3Ops::Magnitude( Vector3Ops::mkVector3(
			ri.geometric.ptIntersection, ri.geometric.ptWireNearestEdge ) );
		const Scalar width = ri.geometric.range * LineWidthScale();
		if( width <= 0.0 || dist >= width ) {
			return base;
		}
		const Scalar t = 1.0 - dist / width;
		const RISEPel line( 0.92, 0.96, 1.0 );
		return base + ( line - base ) * t;
	}
};

//! Unions the world bounding boxes of all bounded objects; unbounded
//! objects (infinite planes report infinite boxes) are skipped by
//! threshold compare rather than isfinite (-ffast-math makes value-
//! level isfinite guards unreliable; see the repo FP notes).
class SceneExtentEnum : public IEnumCallback<IObject>
{
public:
	BoundingBox box;
	bool        any;

	SceneExtentEnum() : box( Point3( 0, 0, 0 ), Point3( 0, 0, 0 ) ), any( false ) {}

	bool operator()( const IObject& obj ) override
	{
		const BoundingBox b = obj.getBoundingBox();
		const Scalar kHuge = 1.0e30;
		if( fabs( b.ll.x ) > kHuge || fabs( b.ll.y ) > kHuge || fabs( b.ll.z ) > kHuge ||
			fabs( b.ur.x ) > kHuge || fabs( b.ur.y ) > kHuge || fabs( b.ur.z ) > kHuge ) {
			return true;
		}
		if( !any ) {
			box = b;
			any = true;
		} else {
			box.Include( b.ll );
			box.Include( b.ur );
		}
		return true;
	}
};

//! View-mode caster: material-preview caster behaviour with luminaires
//! visible (an emissive object is a real, world-visible object whose
//! normals / depth / facets / edges must render -- same rationale as
//! the objectmap caster), plus the wireframe edge-info request flag and
//! the scene-extent capture the depth shader normalizes against.
class InteractiveViewModeRayCaster :
	public InteractiveMaterialPreviewRayCaster
{
public:
	InteractiveViewModeRayCaster( const IShader& shader,
	                              const bool wantsWireEdges,
	                              DepthViewShader* pDepthListener )
	: InteractiveMaterialPreviewRayCaster( shader, /*showLuminaires*/true )
	, mpDepthListener( pDepthListener )
	{
		bWantsWireEdgeInfo = wantsWireEdges;
	}

	void AttachScene( const IScene* pScene_ ) override
	{
		InteractiveMaterialPreviewRayCaster::AttachScene( pScene_ );
		if( !mpDepthListener ) {
			return;
		}
		// AttachScene runs at the top of EVERY RasterizeScene pass (the
		// interactive hot path -- every live-drag frame), so the O(N)
		// extent walk is cached keyed on (scene, spatial generation):
		// GetSpatialStructureGeneration() advances exactly when object
		// bounds can have changed (spatial edits / TLAS invalidation) and
		// stays put for material/painter edits and camera motion.  The
		// diagonal is fed into DepthViewShader::PreparePass, which ALSO
		// runs the depth auto-window snapshot/reset -- see that method's
		// doc; this is the "top of every pass, single-threaded" call site
		// its doc refers to.
		Scalar diag = 0.0;
		if( pScene_ && pScene_->GetObjects() ) {
			const IObjectManager* objs = pScene_->GetObjects();
			const unsigned long long gen = objs->GetSpatialStructureGeneration();
			if( pScene_ == mExtentScene && gen == mExtentGeneration ) {
				mpDepthListener->PreparePass( mExtentDiagonal );
				return;
			}
			SceneExtentEnum extent;
			objs->EnumerateObjects( static_cast<IEnumCallback<IObject>&>( extent ) );
			if( extent.any ) {
				diag = Vector3Ops::Magnitude(
					Vector3Ops::mkVector3( extent.box.ur, extent.box.ll ) );
			}
			mExtentScene = pScene_;
			mExtentGeneration = gen;
			mExtentDiagonal = diag;
		}
		mpDepthListener->PreparePass( diag );
	}

	//! Part B (docs/gui/RENDER_MODES.md "Depth axis" self-calibration):
	//! forwards to the depth listener's WindowStale() query, or false
	//! when this caster has no depth listener (every non-Depth view
	//! mode).  See InteractivePelRasterizer::RasterizeScene.
	bool DepthWindowStale() const
	{
		return mpDepthListener && mpDepthListener->WindowStale();
	}

private:
	// Borrowed: the SAME object as the caster's default shader, kept
	// alive by the caster's own shader reference.
	DepthViewShader* mpDepthListener;
	// Extent cache -- see AttachScene.  mExtentScene is an identity key
	// only (never dereferenced), so a dangling pointer can at worst force
	// one spurious recompute after a scene teardown/rebuild.
	const IScene*             mExtentScene = nullptr;
	unsigned long long        mExtentGeneration = 0;
	Scalar                    mExtentDiagonal = 0.0;
};

}

bool RISE::Implementation::CreateInteractiveMaterialPreviewPipeline(
	IRasterizer** ppRasterizer,
	IRayCaster** ppPreviewCaster,
	IRayCaster** ppPolishCaster )
{
	if( ppRasterizer ) {
		*ppRasterizer = 0;
	}
	if( ppPreviewCaster ) {
		*ppPreviewCaster = 0;
	}
	if( ppPolishCaster ) {
		*ppPolishCaster = 0;
	}
	if( !ppRasterizer || !ppPreviewCaster || !ppPolishCaster ) {
		return false;
	}

	IShader* pPreviewShader = new InteractiveMaterialPreviewShader(
		/*aoSamples*/2,
		/*aoStrength*/0.42,
		/*aoRadiusScale*/0.045,
		/*aoMaxDistance*/1.75,
		/*wideAOSamples*/0,
		/*wideAOStrength*/0.0,
		/*wideAORadiusScale*/0.0,
		/*wideAOMaxDistance*/0.0 );
	IShader* pPolishPreviewShader = new InteractiveMaterialPreviewShader(
		/*aoSamples*/16,
		/*aoStrength*/0.52,
		/*aoRadiusScale*/0.055,
		/*aoMaxDistance*/2.25,
		/*wideAOSamples*/8,
		/*wideAOStrength*/0.28,
		/*wideAORadiusScale*/0.18,
		/*wideAOMaxDistance*/7.0 );

	IRayCaster* pCaster = new InteractiveMaterialPreviewRayCaster( *pPreviewShader );
	pPreviewShader->release();
	IRayCaster* pPolishCaster = new InteractiveMaterialPreviewRayCaster( *pPolishPreviewShader );
	pPolishPreviewShader->release();

	InteractivePelRasterizer::Config cfg;
	cfg.progressiveOnIdle = false;

	InteractivePelRasterizer* interactive = new InteractivePelRasterizer( pCaster, cfg );
	interactive->SetPolishRayCaster( pPolishCaster );

	*ppRasterizer = interactive;
	*ppPreviewCaster = pCaster;
	*ppPolishCaster = pPolishCaster;
	return true;
}

bool RISE::Implementation::CreateInteractiveObjectMapPipeline(
	IRasterizer** ppRasterizer,
	IRayCaster** ppCaster,
	const ObjectMapPalette& palette )
{
	if( ppRasterizer ) {
		*ppRasterizer = 0;
	}
	if( ppCaster ) {
		*ppCaster = 0;
	}
	if( !ppRasterizer || !ppCaster ) {
		return false;
	}

	IShader* pShader = new ObjectIdShader( palette );
	// showLuminaires=true: emissive objects are world-visible, selectable
	// objects that MUST segment into the objectmap.  With the material-
	// preview default (false) RayCaster forces bHit=false for a primary ray
	// hitting an emitter BEFORE the ObjectIdShader runs, so the emissive
	// object would vanish into background and its legend entry would be a
	// permanent unexplained pixelCount:0.
	IRayCaster* pCaster = new InteractiveMaterialPreviewRayCaster( *pShader, /*showLuminaires*/true );
	pShader->release();

	// Default preview config, progressive OFF -- a single exact pass.  We
	// deliberately install NO polish caster and NO sampling kernel: the
	// freshly constructed instance has pSampling == null, so every pixel
	// takes IntegratePixel's single-ray (no jitter, no filter) branch --
	// the EXACTNESS INVARIANT documented at the factory in the header.
	InteractivePelRasterizer::Config cfg;
	cfg.progressiveOnIdle = false;

	InteractivePelRasterizer* interactive = new InteractivePelRasterizer( pCaster, cfg );

	*ppRasterizer = interactive;
	*ppCaster = pCaster;
	return true;
}

// ---------------------------------------------------------------------
// GUI render modes P1 (docs/gui/RENDER_MODES.md): the shared viewport
// render-mode registry + the ephemeral view-mode caster factory.
// ---------------------------------------------------------------------

namespace {

const RISE::Implementation::ViewportRenderModeInfo kViewportRenderModes[] =
{
	{ RISE::Implementation::ViewportRenderMode::Preview,   "preview",   "Shaded Preview",
	  "What does the scene roughly look like?",       true,  true,  false, 0, 0, 0 },
	{ RISE::Implementation::ViewportRenderMode::ObjectMap, "objectmap", "Object Map",
	  "Which object is where?",                       false, false, false, 0, 0, 0 },
	{ RISE::Implementation::ViewportRenderMode::Normals,   "normals",   "Normals",
	  "Which way do surfaces face?",                  false, true,  true,  0, 0, 0 },
	{ RISE::Implementation::ViewportRenderMode::Depth,     "depth",     "Depth",
	  "How far away is everything?",                  false, true,  true,  0, 0, 0 },
	{ RISE::Implementation::ViewportRenderMode::Facets,    "facets",    "Facets",
	  "What does the actual tessellation look like?", false, true,  true,  0, 0, 0 },
	{ RISE::Implementation::ViewportRenderMode::Wireframe, "wireframe", "Wireframe",
	  "Where are the polygon edges?",                 false, true,  true,  0, 0, 0 },
	// GUI render modes P2a (docs/gui/RENDER_MODES.md §3 Transport, §6): the
	// BeautyVariant rows.  casterFactory=false -- these route through
	// CreateBeautyVariantPipeline, not CreateInteractiveViewModeCaster.
	{ RISE::Implementation::ViewportRenderMode::DeepReflect, "deep_reflect", "Deep Reflections",
	  "What do reflections and refractions resolve to?", true, true, false, 4, 24, 16 },
	{ RISE::Implementation::ViewportRenderMode::Direct,      "direct",       "Direct Lighting",
	  "What does direct lighting alone contribute?",      true, true, false, 2, 1,  8  },
};

}

bool RISE::Implementation::IsBeautyVariantMode( ViewportRenderMode mode )
{
	const ViewportRenderModeInfo* info = FindViewportRenderModeInfo( mode );
	return info && info->variantSamplesPerPass > 0;
}

const RISE::Implementation::ViewportRenderModeInfo*
RISE::Implementation::GetViewportRenderModes( unsigned int& outCount )
{
	outCount = static_cast<unsigned int>(
		sizeof( kViewportRenderModes ) / sizeof( kViewportRenderModes[0] ) );
	return kViewportRenderModes;
}

const RISE::Implementation::ViewportRenderModeInfo*
RISE::Implementation::FindViewportRenderModeByName( const char* name )
{
	if( !name ) {
		return 0;
	}
	for( unsigned int i = 0; i < sizeof( kViewportRenderModes ) / sizeof( kViewportRenderModes[0] ); ++i ) {
		if( strcmp( kViewportRenderModes[i].name, name ) == 0 ) {
			return &kViewportRenderModes[i];
		}
	}
	return 0;
}

const RISE::Implementation::ViewportRenderModeInfo*
RISE::Implementation::FindViewportRenderModeInfo( ViewportRenderMode mode )
{
	for( unsigned int i = 0; i < sizeof( kViewportRenderModes ) / sizeof( kViewportRenderModes[0] ); ++i ) {
		if( kViewportRenderModes[i].mode == mode ) {
			return &kViewportRenderModes[i];
		}
	}
	return 0;
}

bool RISE::Implementation::CreateInteractiveViewModeCaster(
	ViewportRenderMode mode,
	IRayCaster** ppCaster,
	bool xray )
{
	if( ppCaster ) {
		*ppCaster = 0;
	}
	if( !ppCaster ) {
		return false;
	}

	IShader* pShader = 0;
	DepthViewShader* pDepthListener = 0;
	bool wantsWireEdges = false;
	switch( mode ) {
	case ViewportRenderMode::Normals:
		pShader = new NormalsViewShader();
		break;
	case ViewportRenderMode::Depth:
	{
		DepthViewShader* pDepth = new DepthViewShader();
		pShader = pDepth;
		pDepthListener = pDepth;
		break;
	}
	case ViewportRenderMode::Facets:
		pShader = new FacetsViewShader();
		break;
	case ViewportRenderMode::Wireframe:
		pShader = new WireframeViewShader();
		wantsWireEdges = true;
		break;
	default:
		// Preview restores the platform-installed casters; ObjectMap has
		// its own factory (palette lifecycle).  Neither is built here.
		return false;
	}

	InteractiveViewModeRayCaster* pCaster = new InteractiveViewModeRayCaster(
		*pShader, wantsWireEdges, pDepthListener );
	pShader->release();
	// GUI render modes (docs/gui/RENDER_MODES.md "X-ray axis"): x-ray
	// resolution now lives in the caster layer -- see RayCaster::
	// ResolveXrayView_.  This composes with all four data modes with no
	// per-shader plumbing.
	pCaster->SetXrayViewResolve( xray );
	*ppCaster = pCaster;
	return true;
}

bool RISE::Implementation::CreateInteractiveViewModePipeline(
	ViewportRenderMode mode,
	IRasterizer** ppRasterizer,
	IRayCaster** ppCaster,
	bool xray )
{
	if( ppRasterizer ) {
		*ppRasterizer = 0;
	}
	if( ppCaster ) {
		*ppCaster = 0;
	}
	if( !ppRasterizer || !ppCaster ) {
		return false;
	}

	IRayCaster* pCaster = 0;
	if( !CreateInteractiveViewModeCaster( mode, &pCaster, xray ) ) {
		// Preview / ObjectMap (own factories above) or an unrecognized mode.
		return false;
	}

	// Same "single exact pass" config as CreateInteractiveObjectMapPipeline:
	// progressiveOnIdle OFF, no polish caster, no sampling kernel installed --
	// every pixel takes IntegratePixel's single-ray (no jitter, no filter)
	// branch.  These are data/diagnostic images; a sampling kernel buys
	// nothing for P1.
	InteractivePelRasterizer::Config cfg;
	cfg.progressiveOnIdle = false;

	InteractivePelRasterizer* interactive = new InteractivePelRasterizer( pCaster, cfg );

	*ppRasterizer = interactive;
	*ppCaster = pCaster;
	return true;
}

// ---------------------------------------------------------------------
// GUI render modes P2a (docs/gui/RENDER_MODES.md §6): the BeautyVariant
// pipeline factory.
// ---------------------------------------------------------------------

namespace {

//! RayCaster's `pDefaultShader` constructor argument is only ever read by
//! RayCaster::SelectShader() (RayCaster.cpp), which nothing on the PT
//! transport path calls -- per-object shading is driven entirely by each
//! IObject's own IMaterial/BSDF (see RayCaster::AttachScene /
//! Job::SetPathTracingPelRasterizer, which passes an ordinary scene shader
//! for the SAME reason: the argument is structurally required but
//! practically inert for this rasterizer family).  This placeholder exists
//! only to satisfy that constructor requirement without pulling in a
//! Job/shader-manager dependency this factory otherwise has no need for.
class BeautyVariantPlaceholderShader :
	public IShader,
	public Reference
{
public:
	BeautyVariantPlaceholderShader() {}

	void Shade( const RuntimeContext&, const RayIntersection&,
	            const IRayCaster&, const IRayCaster::RAY_STATE&,
	            RISEPel& c, const IORStack& ) const override
	{
		c = RISEPel( 0, 0, 0 );
	}

	Scalar ShadeNM( const RuntimeContext&, const RayIntersection&,
	                const IRayCaster&, const IRayCaster::RAY_STATE&,
	                const Scalar, const IORStack& ) const override
	{
		return 0;
	}

	void ResetRuntimeData() const override {}

protected:
	~BeautyVariantPlaceholderShader() override {}
};

}

bool RISE::Implementation::CreateBeautyVariantPipeline(
	ViewportRenderMode mode,
	IRasterizer** ppRasterizer,
	IRayCaster** ppCaster )
{
	if( ppRasterizer ) {
		*ppRasterizer = 0;
	}
	if( ppCaster ) {
		*ppCaster = 0;
	}
	if( !ppRasterizer || !ppCaster ) {
		return false;
	}
	if( !IsBeautyVariantMode( mode ) ) {
		return false;
	}
	const ViewportRenderModeInfo* info = FindViewportRenderModeInfo( mode );
	if( !info ) {
		return false;
	}

	// Minimal production-real caster -- mirrors Job::SetPathTracingPelRasterizer's
	// caster construction: seeRadianceMap=true, showLuminaires=true, maxR from
	// the mode's variantMaxBounces.  Not the InteractiveMaterialPreviewRayCaster
	// subclass -- a PLAIN RayCaster, so shading resolves through each object's
	// own real IMaterial exactly like a production render.
	IShader* pShader = new BeautyVariantPlaceholderShader();
	IRayCaster* pCaster = 0;
	RISE_API_CreateRayCaster( &pCaster, /*seeRadianceMap*/true, info->variantMaxBounces, *pShader, /*showLuminaires*/true );
	pShader->release();
	if( !pCaster ) {
		return false;
	}

	// Multijittered sampler at the mode's fixed spp + a box reconstruction
	// filter -- mirrors GetSamplingAndFilterElements' "multijittered"/"box"
	// branches (Job.cpp) at unit kernel dims (SetNumSamples supplies the
	// actual count).
	ISampling2D* pSampler = 0;
	RISE_API_CreateMultiJitteredSampling2D( &pSampler, 1.0, 1.0 );
	if( pSampler ) {
		pSampler->SetNumSamples( info->variantSamplesPerPass );
	}
	IPixelFilter* pFilter = 0;
	RISE_API_CreateBoxPixelFilter( &pFilter, 1.0, 1.0 );

	IRasterizer* pRaster = 0;
	RISE_API_CreatePathTracingPelRasterizer( &pRaster, pCaster, pSampler, pFilter,
		/*smsEnabled*/false, /*smsMaxIterations*/0, /*smsThreshold*/0.0, /*smsMaxChainDepth*/0,
		/*smsBiased*/false, /*smsBernoulliTrials*/0, /*smsMultiTrials*/1, /*smsPhotonCount*/0,
		/*smsTwoStage*/false, /*smsUseLevenbergMarquardt*/false, SMSSeedingMode::Snell, /*smsTargetBounces*/0,
		/*oidnDenoise*/true, OidnQuality::Auto, OidnDevice::Auto, OidnPrefilter::Fast,
		PathGuidingConfig(), AdaptiveSamplingConfig(), StabilityConfig(), /*useZSobol*/false );

	safe_release( pSampler );
	safe_release( pFilter );

	if( !pRaster ) {
		safe_release( pCaster );
		return false;
	}

	// `pCaster` is NOT released here -- unlike Job::SetPathTracingPelRasterizer
	// (which discards its own local ref once the rasterizer has addref'd it,
	// because Job never hands the caster pointer back to anyone), this
	// factory RETURNS pCaster as an owning reference via `*ppCaster`, exactly
	// like CreateInteractiveMaterialPreviewPipeline / CreateInteractiveObjectMapPipeline /
	// CreateInteractiveViewModePipeline above: the rasterizer holds its OWN
	// addref'd reference internally, so the caller's ref (this one) and the
	// rasterizer's internal one are independent and both must be released.
	*ppRasterizer = pRaster;
	*ppCaster = pCaster;
	return true;
}

InteractivePelRasterizer::InteractivePelRasterizer( IRayCaster* pCaster, const Config& cfg , RISE::Implementation::FrameStore* frameStore)
:
  Rasterizer( frameStore ),
  PixelBasedRasterizerHelper( pCaster , frameStore)
, PixelBasedPelRasterizer(
    pCaster,
    PathGuidingConfig(),       // disabled by default
    AdaptiveSamplingConfig(),  // maxSamples=0 == disabled
    StabilityConfig(),         // default stability bounds
    /*useZSobol*/false
  , frameStore)
, mCfg( cfg )
, mIdleMode( false )
, mPreviewDenoiseMode( PreviewDenoise_Off )
, mPolishKernel( 0 )
, mPolishCaster( 0 )
, mSavedPreviewCaster( 0 )
, mViewModeCasterInstalled( false )
, mViewModeCasterAllowsDenoise( false )
, mSavedPreviewCasterForViewMode( 0 )
, mXrayView( false )
{
}

InteractivePelRasterizer::~InteractivePelRasterizer()
{
	// If a polish pass was active when destroyed (shouldn't happen
	// in practice — the controller always restores after the pass —
	// but be defensive), make sure pCaster points back to the
	// preview caster before the base destructor releases it.
	if( mSavedPreviewCaster ) {
		safe_release( pCaster );
		pCaster = mSavedPreviewCaster;
		mSavedPreviewCaster = 0;
	}
	// GUI render modes P1: same defensive unwind for a still-installed
	// view-mode caster (shouldn't happen -- SceneEditController::
	// RebindEditorToJob resets to Preview on every scene load/reload, and
	// the controller resets before teardown -- but be defensive exactly
	// like the polish case above).
	if( mViewModeCasterInstalled ) {
		safe_release( pCaster );
		pCaster = mSavedPreviewCasterForViewMode;
		mSavedPreviewCasterForViewMode = 0;
		mViewModeCasterInstalled = false;
	}
	safe_release( mPolishKernel );
	safe_release( mPolishCaster );
}

void InteractivePelRasterizer::SetPreviewDenoiseMode( PreviewDenoiseMode mode )
{
	mPreviewDenoiseMode = mode;
#ifdef RISE_ENABLE_OIDN
	switch( mode )
	{
	case PreviewDenoise_Fast:
		SetDenoisingEnabled( true );
		SetDenoisingQuality( OidnQuality::Fast );
		SetDenoisingDevice( OidnDevice::Auto );
		break;
	case PreviewDenoise_Balanced:
		SetDenoisingEnabled( true );
		SetDenoisingQuality( OidnQuality::Balanced );
		SetDenoisingDevice( OidnDevice::Auto );
		break;
	case PreviewDenoise_Off:
	default:
		SetDenoisingEnabled( false );
		break;
	}
#else
	(void)mode;
#endif
}

void InteractivePelRasterizer::SetPolishRayCaster( IRayCaster* polishCaster )
{
	if( mPolishCaster == polishCaster ) return;
	safe_release( mPolishCaster );
	mPolishCaster = polishCaster;
	if( mPolishCaster ) {
		mPolishCaster->addref();
		ApplyXrayViewToCaster_( mPolishCaster );
	}
}

void InteractivePelRasterizer::ApplyXrayViewToCaster_( IRayCaster* c ) const
{
	if( RayCaster* rc = dynamic_cast<RayCaster*>( c ) ) {
		rc->SetXrayViewResolve( mXrayView );
	}
}

void InteractivePelRasterizer::SetXrayView( bool on )
{
	mXrayView = on;
	// Stamp every caster this rasterizer can currently reach -- the
	// active caster plus both saved-caster slots (whichever of the two
	// nesting swaps, polish or view-mode, is in effect right now) and
	// the polish caster itself.  A caster this rasterizer doesn't
	// currently hold a reference to (e.g. one built later by
	// CreateInteractiveViewModeCaster) picks up the flag at ITS OWN
	// install point via the same stamping this method uses -- see
	// SetPolishRayCaster / SetViewModeCaster / SetSampleCount.
	ApplyXrayViewToCaster_( pCaster );
	ApplyXrayViewToCaster_( mPolishCaster );
	ApplyXrayViewToCaster_( mSavedPreviewCaster );
	ApplyXrayViewToCaster_( mSavedPreviewCasterForViewMode );
}

void InteractivePelRasterizer::SetViewModeCaster( IRayCaster* p, bool allowsDenoise )
{
	if( p )
	{
		if( !mViewModeCasterInstalled )
		{
			// First install: capture the TRUE original preview caster to
			// restore to later.  If a polish-caster swap is currently in
			// effect (SetSampleCount's own mechanism -- mSavedPreviewCaster
			// non-null means pCaster currently holds mPolishCaster, not the
			// real preview caster), unwind it here so we save the REAL
			// original, not the transient polish caster.  The polish
			// reference pCaster held is simply dropped -- mPolishCaster's
			// own long-lived reference (held by the mPolishCaster member
			// itself, set up by SetPolishRayCaster) is untouched.
			if( mSavedPreviewCaster )
			{
				mSavedPreviewCasterForViewMode = mSavedPreviewCaster;
				mSavedPreviewCaster = 0;
				safe_release( pCaster );   // drop the transient polish reference
			}
			else
			{
				mSavedPreviewCasterForViewMode = pCaster;   // transfer pCaster's own reference
				pCaster = 0;
			}
			mViewModeCasterInstalled = true;
		}
		else
		{
			// Switching directly between two view modes: drop the
			// currently-installed one's pCaster reference.  The saved
			// original preview caster (mSavedPreviewCasterForViewMode)
			// is untouched -- it was captured on the FIRST install.
			safe_release( pCaster );
		}
		pCaster = p;
		pCaster->addref();
		ApplyXrayViewToCaster_( pCaster );
		mViewModeCasterAllowsDenoise = allowsDenoise;
		return;
	}

	// p == nullptr: restore.  No-op if no view-mode caster is installed.
	if( !mViewModeCasterInstalled ) return;
	safe_release( pCaster );
	pCaster = mSavedPreviewCasterForViewMode;
	mSavedPreviewCasterForViewMode = 0;
	mViewModeCasterInstalled = false;
	mViewModeCasterAllowsDenoise = false;
	ApplyXrayViewToCaster_( pCaster );
}

void InteractivePelRasterizer::SetIdleMode( bool idle ) const
{
	mIdleMode = idle;
	// The actual switch from 1-pass to multi-pass progressive lives
	// in the SceneEditController's render loop driver (Phase 2): it
	// inspects IsIdleMode() before each RasterizeScene call and
	// configures the rasterizer's progressiveConfig accordingly.
	// Doing it here would be order-dependent on the controller's
	// own state and risk thrashing.
}

void InteractivePelRasterizer::SetSampleCount( unsigned int n )
{
	if( n <= 1 ) {
		// 1-SPP — clear pSampling so per-pixel integration falls
		// back to the single-ray path.  Leave progressiveConfig
		// alone; without pSampling the progressive path is skipped.
		safe_release( pSampling );
		pSampling = 0;

		// If we'd swapped to the polish caster, restore the preview
		// caster.  pCaster currently holds an addref'd polish caster;
		// release that and adopt the saved preview caster (whose
		// refcount we preserved at swap time).
		//
		// GUI render modes P1: suppressed entirely while a view-mode
		// caster is installed -- pCaster holds the view-mode caster,
		// not the polish caster, and mSavedPreviewCaster is guaranteed
		// null in that window (SetViewModeCaster's install unwinds any
		// in-progress polish swap before installing) -- so this branch
		// would never fire anyway; the guard documents the invariant.
		if( !mViewModeCasterInstalled && mSavedPreviewCaster ) {
			safe_release( pCaster );
			pCaster = mSavedPreviewCaster;
			mSavedPreviewCaster = 0;
			ApplyXrayViewToCaster_( pCaster );
		}
		return;
	}

	// Multi-SPP polish.  Lazy-init a 2D sampling kernel; reuse on
	// subsequent polish calls (just reset numSamples in case the
	// caller asked for a different count).
	if( !mPolishKernel ) {
		// MultiJittered sample dimensions are the kernel's spatial
		// extent (pixels); 1.0 × 1.0 means the samples spread over
		// one full pixel.  Sample COUNT is independent and set via
		// SetNumSamples below.
		RISE_API_CreateMultiJitteredSampling2D( &mPolishKernel, 1.0, 1.0 );
	}
	if( mPolishKernel ) {
		mPolishKernel->SetNumSamples( n );
	}

	safe_release( pSampling );
	pSampling = mPolishKernel;
	if( pSampling ) {
		pSampling->addref();
	}

	// Disable progressive so we get exactly ONE pass at n SPP rather
	// than splitting into multiple progressive sub-passes.
	ProgressiveConfig cfg;
	cfg.enabled = false;
	cfg.samplesPerPass = n;
	SetProgressiveConfig( cfg );

	// Swap to the polish ray caster.  The shared material-preview
	// pipeline uses this to spend extra work on pointer-up AO while
	// keeping live drag cheap.  Idempotent: if we've already swapped,
	// leave the existing state in place.
	//
	// GUI render modes P1: suppressed entirely while a view-mode caster
	// is installed -- swapping pCaster to mPolishCaster here would
	// silently flip the image back to clay-preview shading mid-polish
	// while the user is looking at a normals/depth/facets/wireframe
	// view (docs/gui/RENDER_MODES.md §4 "Denoise / display policy" and
	// SetViewModeCaster's own doc).  The sampling-kernel install above
	// (pSampling / ProgressiveConfig) is UNAFFECTED by this guard --
	// only the pCaster swap is suppressed.
	if( !mViewModeCasterInstalled && mPolishCaster && !mSavedPreviewCaster ) {
		mSavedPreviewCaster = pCaster;   // keep its refcount on the saved slot
		pCaster = mPolishCaster;
		pCaster->addref();
		ApplyXrayViewToCaster_( pCaster );
	}
}

void InteractivePelRasterizer::PrepareImageForNewRender( IRasterImage& /*img*/, const Rect* /*pRect*/ ) const
{
	// Intentionally empty.  The default impl clears to a random
	// pastel and fires OutputIntermediateImage; both produce visible
	// flashes during the interactive cancel-restart loop.  We want
	// the previous frame's pixels to stay on screen until the new
	// tiles overwrite them, so we skip the clear.  Our viewport sink
	// also ignores OutputIntermediateImage (it only dispatches at
	// end-of-pass), so skipping the notification is harmless.
}

void InteractivePelRasterizer::RasterizeScene(
	const IScene& pScene, const Rect* pRect, IRasterizeSequence* pRasterSequence ) const
{
	PixelBasedRasterizerHelper::RasterizeScene( pScene, pRect, pRasterSequence );

	// Part B (docs/gui/RENDER_MODES.md "Depth axis" self-calibration): a
	// Depth view-mode caster's auto-window arms from the PREVIOUS pass's
	// accumulated min/max (DepthViewShader::PreparePass, called from
	// InteractiveViewModeRayCaster::AttachScene at the top of every
	// pass).  The interactive live-drag/idle-refine loop calls
	// RasterizeScene repeatedly, so mild per-pass drift self-corrects
	// within a couple of repaints -- but a single SETTLED call (a GUI
	// mode switch, or any one-shot render like the agent view-mode path)
	// only gets ONE pass.  Two distinct staleness cases both need that
	// one pass to self-correct rather than ship a stale result:
	//   (a) no window has ever armed (first pass ever) -- shows the flat
	//       scene-diagonal fallback;
	//   (b) a window WAS armed from an earlier pass, but a camera jump /
	//       crop / scene edit between calls means THIS pass's actual
	//       accumulated min/max no longer match it -- shades against a
	//       window calibrated for a different view.
	// DepthWindowStale() (see InteractiveViewModeRayCaster /
	// DepthViewShader::WindowStale) detects both.  If it fires, re-run
	// the base pass exactly once more: AttachScene's PreparePass call at
	// the top of that second pass snapshots the just-finished pass's
	// accumulators into a freshly-valid window, and the second pass
	// shades against it -- so THIS SAME RasterizeScene call returns a
	// correctly-windowed image.  Calling the base method directly (not
	// through the vtable) means there is no re-entrancy into this
	// override, so at most one extra pass ever runs per call -- the
	// second pass's own possible staleness is not re-checked, which
	// bounds the cost and respects cancellation exactly as before.
	InteractiveViewModeRayCaster* pViewCaster =
		dynamic_cast<InteractiveViewModeRayCaster*>( pCaster );
	const bool bCancelled = pProgressFunc && pProgressFunc->IsCancelled();
	if( pViewCaster && !bCancelled && pViewCaster->DepthWindowStale() )
	{
		PixelBasedRasterizerHelper::RasterizeScene( pScene, pRect, pRasterSequence );
	}
}

void InteractivePelRasterizer::PrepareRuntimeContext( RuntimeContext& rc ) const
{
	// Inherit everything the production base configures (stability,
	// optimal MIS, path guiding, etc.) so the fast-preview render
	// produces images consistent with what production would produce
	// for non-expensive shader ops.
	PixelBasedPelRasterizer::PrepareRuntimeContext( rc );

	// Then signal "this is the interactive preview path" so shader
	// ops that have a fast-preview branch take it.  See
	// RuntimeContext::bFastPreview for the contract.
	rc.bFastPreview = true;
}

#ifdef RISE_ENABLE_OIDN
bool InteractivePelRasterizer::ShouldDenoise() const
{
	// Stack the preview-mode toggle on top of the base predicate.
	// Cancellation is intentionally NOT consulted (see base class doc):
	// in interactive workflows the cancel-restart loop benefits from
	// denoising the partial image so the user sees a smoothed preview
	// of whatever samples landed before the next restart, instead of
	// raw MC noise.
	//
	// GUI render modes P1: denoise policy follows the installed mode's
	// registry wantsDenoise (plumbed through SetViewModeCaster) -- false
	// for every P1 data mode, so normals/depth/facets/wireframe never
	// OIDN (docs/gui/RENDER_MODES.md §4 "Denoise / display policy");
	// a future beauty-variant mode that wants denoise just declares it.
	return PixelBasedPelRasterizer::ShouldDenoise() &&
		mPreviewDenoiseMode != PreviewDenoise_Off &&
		( !mViewModeCasterInstalled || mViewModeCasterAllowsDenoise );
}

unsigned int InteractivePelRasterizer::GetDenoiseAOVSamplesPerPixel() const
{
	return 1;
}
#endif

IRasterizeSequence* InteractivePelRasterizer::CreateDefaultRasterSequence( unsigned int tileEdge ) const
{
	// BlockRasterizeSequence's `type` argument:
	//   0 = centre-out (sort by distance from image centre)
	//   1 = random shuffle
	//   2 = top-left
	switch( mCfg.tileOrder )
	{
	case TileOrder_Random:
		return new BlockRasterizeSequence( tileEdge, tileEdge, 1 );
	case TileOrder_Scanline:
		// Scanline goes left-to-right, top-to-bottom — closest
		// available is BlockRasterizeSequence type 2 (top-left
		// distance), which gives a roughly scanline-ish order.
		return new BlockRasterizeSequence( tileEdge, tileEdge, 2 );
	case TileOrder_CenterOut:
	default:
		return new BlockRasterizeSequence( tileEdge, tileEdge, 0 );
	}
}
