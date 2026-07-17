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
		Vector3 n = ri.geometric.vNormal;
		Vector3Ops::NormalizeMag( n );
		c = RISEPel( 0.5 * ( n.x + 1.0 ), 0.5 * ( n.y + 1.0 ), 0.5 * ( n.z + 1.0 ) );
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
};

//! Headlamp-shaded GEOMETRIC normal (flat face normal -- no Phong
//! smoothing, no bump/normal-map perturbation).  Reveals the actual
//! tessellation and any shading-normal divergence.
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

//! Scene-scale-normalized hit distance, near = bright:
//! t = 1 / (1 + 3 d / D) with D = the scene's world bounding-box
//! diagonal, captured ONCE per AttachScene (single-threaded, before the
//! parallel workers start) by InteractiveViewModeRayCaster below.  The
//! fixed scene-scale mapping is deliberate: brightness IS distance in
//! scene units, stable while navigating (an adaptive per-frame window
//! would re-mean the image every camera move).  D == 0 (no bounded
//! objects) falls back to t = 1 / (1 + d).
class DepthViewShader :
	public IShader,
	public Reference
{
public:
	DepthViewShader() : mSceneDiagonal( 0.0 ) {}

	//! Called from AttachScene only -- single-threaded by the render
	//! setup contract; read-only during the parallel render.
	void SetSceneDiagonal( const Scalar d ) { mSceneDiagonal = d; }

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
		const Scalar t = DepthValue( ri );
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
		return DepthValue( ri );
	}

	void ResetRuntimeData() const override {}

protected:
	~DepthViewShader() override {}

private:
	Scalar mSceneDiagonal;

	Scalar DepthValue( const RayIntersection& ri ) const
	{
		const Scalar d = ri.geometric.range;
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
		Scalar diag = 0.0;
		if( pScene_ && pScene_->GetObjects() ) {
			SceneExtentEnum extent;
			pScene_->GetObjects()->EnumerateObjects( static_cast<IEnumCallback<IObject>&>( extent ) );
			if( extent.any ) {
				diag = Vector3Ops::Magnitude(
					Vector3Ops::mkVector3( extent.box.ur, extent.box.ll ) );
			}
		}
		mpDepthListener->SetSceneDiagonal( diag );
	}

private:
	// Borrowed: the SAME object as the caster's default shader, kept
	// alive by the caster's own shader reference.
	DepthViewShader* mpDepthListener;
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
	  "What does the scene roughly look like?",       true,  true,  false },
	{ RISE::Implementation::ViewportRenderMode::ObjectMap, "objectmap", "Object Map",
	  "Which object is where?",                       false, false, false },
	{ RISE::Implementation::ViewportRenderMode::Normals,   "normals",   "Normals",
	  "Which way do surfaces face?",                  false, true,  true  },
	{ RISE::Implementation::ViewportRenderMode::Depth,     "depth",     "Depth",
	  "How far away is everything?",                  false, true,  true  },
	{ RISE::Implementation::ViewportRenderMode::Facets,    "facets",    "Facets",
	  "What does the actual tessellation look like?", false, true,  true  },
	{ RISE::Implementation::ViewportRenderMode::Wireframe, "wireframe", "Wireframe",
	  "Where are the polygon edges?",                 false, true,  true  },
};

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
	IRayCaster** ppCaster )
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

	IRayCaster* pCaster = new InteractiveViewModeRayCaster(
		*pShader, wantsWireEdges, pDepthListener );
	pShader->release();
	*ppCaster = pCaster;
	return true;
}

bool RISE::Implementation::CreateInteractiveViewModePipeline(
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

	IRayCaster* pCaster = 0;
	if( !CreateInteractiveViewModeCaster( mode, &pCaster ) ) {
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
	}
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
