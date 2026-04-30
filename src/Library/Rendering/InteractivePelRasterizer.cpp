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
	InteractiveMaterialPreviewRayCaster( const IShader& shader )
	: RayCaster( /*seeRadianceMap*/false,
				 /*maxR*/1,
				 shader,
				 /*showLuminaires*/false )
	{}

protected:
	const IShader& SelectShader( const RayIntersection& /*ri*/ ) const override
	{
		return pDefaultShader;
	}
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

InteractivePelRasterizer::InteractivePelRasterizer( IRayCaster* pCaster, const Config& cfg )
: PixelBasedRasterizerHelper( pCaster )
, PixelBasedPelRasterizer(
    pCaster,
    PathGuidingConfig(),       // disabled by default
    AdaptiveSamplingConfig(),  // maxSamples=0 == disabled
    StabilityConfig(),         // default stability bounds
    /*useZSobol*/false
  )
, mCfg( cfg )
, mIdleMode( false )
, mPreviewDenoiseMode( PreviewDenoise_Off )
, mPolishKernel( 0 )
, mPolishCaster( 0 )
, mSavedPreviewCaster( 0 )
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
		if( mSavedPreviewCaster ) {
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
	if( mPolishCaster && !mSavedPreviewCaster ) {
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
	return PixelBasedPelRasterizer::ShouldDenoise() &&
		mPreviewDenoiseMode != PreviewDenoise_Off;
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
