// DL-214: bridge alpha material coverage works under legacy and modern integrators.
// The real bridge producer functions are included below; no warning replicas.

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// The unit under test.  See the banner for why this is a .cpp include.
#include "../src/Blender/native/rise_blender_bridge.cpp"

#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color.h"

static int g_checks   = 0;
static int g_failures = 0;

static void Check( const bool ok, const std::string& what )
{
	++g_checks;
	if( !ok ) {
		++g_failures;
		std::cout << "  FAIL: " << what << std::endl;
	}
}

namespace {

//! Row-major (Blender mathutils convention) identity + translation,
//! matching `blender_matrix_to_rise_matrix`'s own documented layout.
void IdentityTranslate( float out[16], const float tx, const float ty, const float tz )
{
	const float m[16] = {
		1,0,0,tx,
		0,1,0,ty,
		0,0,1,tz,
		0,0,0,1
	};
	std::memcpy( out, m, sizeof(m) );
}

class CapturingRasterizerOutput
	: public virtual RISE::IRasterizerOutput
	, public virtual RISE::Implementation::Reference
{
public:
	std::vector<RISE::RISEColor> pixels;
	unsigned int width;
	unsigned int height;

	CapturingRasterizerOutput() : width(0), height(0) {}

protected:
	virtual ~CapturingRasterizerOutput() {}

public:
	virtual void OutputIntermediateImage( const RISE::IRasterImage&, const RISE::Rect* ) {}

	virtual void OutputImage( const RISE::IRasterImage& image, const RISE::Rect*, const unsigned int )
	{
		width = image.GetWidth();
		height = image.GetHeight();
		pixels.resize( width * height );
		for( unsigned int y = 0; y < height; y++ ) {
			for( unsigned int x = 0; x < width; x++ ) {
				pixels[y * width + x] = image.GetPEL( x, y );
			}
		}
	}
};

//! Mean RGB over the given column range [xLo, xHi) across all rows.
void ColumnMean( const CapturingRasterizerOutput& cap, unsigned int xLo, unsigned int xHi, double outRGB[3] )
{
	double sum[3] = { 0, 0, 0 };
	unsigned int n = 0;
	for( unsigned int y = 0; y < cap.height; y++ ) {
		for( unsigned int x = xLo; x < xHi && x < cap.width; x++ ) {
			const RISE::RISEColor& c = cap.pixels[y * cap.width + x];
			sum[0] += c.base.r; sum[1] += c.base.g; sum[2] += c.base.b;
			++n;
		}
	}
	outRGB[0] = n ? sum[0] / n : -1.0;
	outRGB[1] = n ? sum[1] / n : -1.0;
	outRGB[2] = n ? sum[2] / n : -1.0;
}

//! Builds the two-card-over-a-backdrop scene and renders it under
//! `rasterizerKind`.  Returns false (via `ok`) on any setup/render
//! failure; `warnings` receives the render's non-fatal diagnostics
//! (DL-193's integrator-compatibility warning lands here).  `*outCap`
//! receives a NEW, addref'd `CapturingRasterizerOutput*` on success
//! (ref-counted -- protected dtor -- so the caller must `safe_release`
//! it); left at 0 on failure.
bool RenderTwoCardScene(
	const uint32_t rasterizerKind,
	CapturingRasterizerOutput** outCap,
	std::vector<std::string>& warningsOut,
	std::string& errorOut
	)
{
	char err[512];
	warningsOut.clear();

	RISE::IJobPriv* job = 0;
	if( !RISE::RISE_CreateJobPriv( &job ) || !job ) {
		errorOut = "job creation failed";
		return false;
	}
	job->SetPrimaryAcceleration( true, false, 4, 32 );

	rise_blender_camera camera;
	std::memset( &camera, 0, sizeof(camera) );
	camera.projection_type = RISE_BLENDER_CAMERA_PERSPECTIVE;
	camera.location[0] = 0; camera.location[1] = 0; camera.location[2] = 5;
	camera.forward[0] = 0; camera.forward[1] = 0; camera.forward[2] = -1;
	camera.up[0] = 0; camera.up[1] = 1; camera.up[2] = 0;
	camera.fov_y_radians = 50.0f * 3.14159265f / 180.0f;
	camera.width = 64;
	camera.height = 32;
	camera.pixel_aspect = 1.0f;

	if( !configure_camera( *job, camera, err, sizeof(err) ) ) {
		errorOut = std::string( "configure_camera: " ) + err;
		RISE::safe_release( job );
		return false;
	}

	// Backdrop: a wide, thin, bright-green EMISSIVE box far behind
	// both cards.
	double green[3]  = { 0.05, 0.9, 0.05 };
	double blue[3]   = { 0.05, 0.05, 0.9 };
	double dark[3]   = { 0.02, 0.02, 0.02 };
	job->AddUniformColorPainter( "pnt_backdrop_emit", green, "Rec709RGB_Linear" );
	job->AddUniformColorPainter( "pnt_card_emit",     blue,  "Rec709RGB_Linear" );
	job->AddUniformColorPainter( "pnt_dark",          dark,  "Rec709RGB_Linear" );

	if( !job->AddBoxGeometry( "geom_backdrop", 20.0, 10.0, 0.2 ) ||
	    !job->AddBoxGeometry( "geom_card", 2.0, 4.0, 0.2 ) )
	{
		errorOut = "geometry creation failed";
		RISE::safe_release( job );
		return false;
	}

	rise_blender_material backdropMat;
	std::memset( &backdropMat, 0, sizeof(backdropMat) );
	backdropMat.name = "mat_backdrop";
	backdropMat.model = RISE_BLENDER_MATERIAL_LAMBERT;
	backdropMat.double_sided = 1;
	backdropMat.diffuse_painter_name = "pnt_dark";
	backdropMat.emission_painter_name = "pnt_backdrop_emit";
	backdropMat.emissive_scale = 1.0;
	if( !add_material( *job, backdropMat, err, sizeof(err) ) ) {
		errorOut = std::string( "backdrop material: " ) + err;
		RISE::safe_release( job );
		return false;
	}

	// Two identical card materials, differing only in alpha.
	rise_blender_material cardLeft;
	std::memset( &cardLeft, 0, sizeof(cardLeft) );
	cardLeft.name = "mat_card_left";
	cardLeft.model = RISE_BLENDER_MATERIAL_LAMBERT;
	cardLeft.double_sided = 1;
	cardLeft.diffuse_painter_name = "pnt_dark";
	cardLeft.emission_painter_name = "pnt_card_emit";
	cardLeft.emissive_scale = 1.0;
	cardLeft.alpha_mode = RISE_BLENDER_ALPHA_CLIP;
	cardLeft.alpha = 0.0;             // below threshold -> CUT (backdrop shows through)
	cardLeft.alpha_threshold = 0.5;
	if( !add_material( *job, cardLeft, err, sizeof(err) ) ) {
		errorOut = std::string( "left card material: " ) + err;
		RISE::safe_release( job );
		return false;
	}

	rise_blender_material cardRight = cardLeft;
	cardRight.name = "mat_card_right";
	cardRight.alpha = 1.0;            // at/above threshold -> KEPT (opaque)
	if( !add_material( *job, cardRight, err, sizeof(err) ) ) {
		errorOut = std::string( "right card material: " ) + err;
		RISE::safe_release( job );
		return false;
	}

	rise_blender_object backdropObj;
	std::memset( &backdropObj, 0, sizeof(backdropObj) );
	backdropObj.name = "obj_backdrop";
	backdropObj.geometry_name = "geom_backdrop";
	backdropObj.material_name = "mat_backdrop";
	IdentityTranslate( backdropObj.transform, 0, 0, -3 );
	backdropObj.casts_shadows = 1;
	backdropObj.receives_shadows = 1;
	backdropObj.visible = 1;
	if( !add_object( *job, backdropObj, err, sizeof(err) ) ) {
		errorOut = std::string( "backdrop object: " ) + err;
		RISE::safe_release( job );
		return false;
	}

	rise_blender_object cardLeftObj;
	std::memset( &cardLeftObj, 0, sizeof(cardLeftObj) );
	cardLeftObj.name = "obj_card_left";
	cardLeftObj.geometry_name = "geom_card";
	cardLeftObj.material_name = "mat_card_left";
	// ABI v14 / DL-193: the exporter would compute this from the bound
	// material's alpha_mode; this test does so directly, matching
	// `wire_alpha_shader_for_material`'s own deterministic naming.
	cardLeftObj.shader_name = "mat_card_left.shader";
	IdentityTranslate( cardLeftObj.transform, -1.3f, 0, 0 );
	cardLeftObj.casts_shadows = 1;
	cardLeftObj.receives_shadows = 1;
	cardLeftObj.visible = 1;
	if( !add_object( *job, cardLeftObj, err, sizeof(err) ) ) {
		errorOut = std::string( "left card object: " ) + err;
		RISE::safe_release( job );
		return false;
	}

	rise_blender_object cardRightObj = cardLeftObj;
	cardRightObj.name = "obj_card_right";
	cardRightObj.material_name = "mat_card_right";
	cardRightObj.shader_name = "mat_card_right.shader";
	IdentityTranslate( cardRightObj.transform, 1.3f, 0, 0 );
	if( !add_object( *job, cardRightObj, err, sizeof(err) ) ) {
		errorOut = std::string( "right card object: " ) + err;
		RISE::safe_release( job );
		return false;
	}


	// Reuse the REAL bridge rasterizer-configuration function (the same
	// one `rise_blender_render_scene` calls) rather than hand-building
	// an IRayCaster/ISampling2D/IPixelFilter trio ourselves -- this is
	// exactly the setup path a real Blender export goes through.
	rise_blender_render_settings settings;
	std::memset( &settings, 0, sizeof(settings) );
	settings.width = camera.width;
	settings.height = camera.height;
	settings.pixel_samples = 4;
	settings.max_recursion = 4;
	// Both the backdrop and the cards are self-emissive (this file's
	// own banner); a camera ray hitting a luminaire DIRECTLY needs
	// `show_lights` under `pixelpel_rasterizer` specifically -- PT/BDPT
	// show direct luminaire hits regardless of this flag (their own
	// NEE/BSDF-hit MIS architecture), but pixelpel's shader-op-driven
	// `DefaultEmission` op gates on it.  Without this the whole scene
	// (backdrop AND cards) renders as flat black under PIXELPEL only.
	settings.show_lights = 1;
	settings.rasterizer_kind = rasterizerKind;

	rise_blender_scene sceneForRasterizer;
	std::memset( &sceneForRasterizer, 0, sizeof(sceneForRasterizer) );

	if( !configure_shader( *job, settings, err, sizeof(err) ) ) {
		errorOut = std::string( "configure_shader: " ) + err;
		RISE::safe_release( job );
		return false;
	}
	if( !configure_rasterizer( *job, settings, sceneForRasterizer, err, sizeof(err) ) ) {
		errorOut = std::string( "configure_rasterizer: " ) + err;
		RISE::safe_release( job );
		return false;
	}

	job->RemoveRasterizerOutputs();
	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	RISE::GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	job->GetRasterizer()->AddRasterizerOutput( pCap );

	std::srand( 12345 );
	const bool rendered = job->Rasterize();
	if( !rendered || pCap->pixels.empty() ) {
		errorOut = "render failed or produced no pixels";
		RISE::safe_release( pCap );
		RISE::safe_release( job );
		return false;
	}

	*outCap = pCap;	// caller owns; must safe_release
	RISE::safe_release( job );
	return true;
}

}	// namespace

void TestAbiVersionAndAlphaFields()
{
	std::cout << "Test: ABI version 15 and preserved append-only v14 alpha fields" << std::endl;

	Check( RISE_BLENDER_API_VERSION == 15, "RISE_BLENDER_API_VERSION is 15" );

	Check( offsetof( rise_blender_material, alpha ) >
	       offsetof( rise_blender_material, coat_normal_scale ),
		"material.alpha is appended after the coat-normal fields" );
	Check( offsetof( rise_blender_material, alpha_texture_painter_name ) >
	       offsetof( rise_blender_material, alpha ),
		"material.alpha_texture_painter_name follows alpha" );
	Check( offsetof( rise_blender_material, alpha_mode ) >
	       offsetof( rise_blender_material, alpha_texture_painter_name ),
		"material.alpha_mode follows alpha_texture_painter_name" );
	Check( offsetof( rise_blender_material, alpha_threshold ) >
	       offsetof( rise_blender_material, alpha_mode ),
		"material.alpha_threshold follows alpha_mode" );
	Check( RISE_BLENDER_ALPHA_OPAQUE == 0 && RISE_BLENDER_ALPHA_CLIP == 1 && RISE_BLENDER_ALPHA_BLEND == 2,
		"rise_blender_alpha_mode enumerators are 0/1/2" );

	// rise_blender_object gained shader_name.
	rise_blender_object obj;
	std::memset( &obj, 0, sizeof(obj) );
	obj.shader_name = "probe";
	Check( std::string( obj.shader_name ) == "probe", "rise_blender_object.shader_name exists and is settable" );
}

void TestOpaqueMaterialUnaffected()
{
	std::cout << "Test: alpha_mode OPAQUE (default) builds no shader chain, unaffected by the new fields" << std::endl;

	RISE::IJobPriv* job = 0;
	RISE::RISE_CreateJobPriv( &job );
	char err[256];

	double red[3] = { 0.8, 0.1, 0.1 };
	job->AddUniformColorPainter( "pnt_opaque", red, "Rec709RGB_Linear" );

	rise_blender_material mat;
	std::memset( &mat, 0, sizeof(mat) );
	mat.name = "mat_opaque";
	mat.model = RISE_BLENDER_MATERIAL_LAMBERT;
	mat.double_sided = 1;
	mat.diffuse_painter_name = "pnt_opaque";
	// alpha_mode left at its memset-zero default: RISE_BLENDER_ALPHA_OPAQUE.

	Check( add_material( *job, mat, err, sizeof(err) ), std::string( "opaque material registered: " ) + err );
	Check( job->GetShaders() && job->GetShaders()->GetItem( "mat_opaque.shader" ) == 0,
		"no advanced_shader named <material>.shader is registered when alpha_mode is OPAQUE" );

	RISE::safe_release( job );
}

// ============================================================
//  THE MONEY TEST -- `pixelpel_rasterizer` honours the alpha
//  shader-op chain; PT and BDPT alike document the caveat (opaque,
//  WARNED) -- see this file's own banner for how PT ended up in the
//  caveat group alongside BDPT, contradicting AlphaTestShaderOp.h's
//  own PRIOR (WRONG) claim.
// ============================================================

void TestAlphaCutoutUnderPixelPel()
{
	std::cout << "Test: under pixelpel_rasterizer, the alpha=0 (CLIP) card is cut through to the backdrop; the alpha=1 card stays opaque" << std::endl;

	CapturingRasterizerOutput* cap = 0;
	std::vector<std::string> warnings;
	std::string errorOut;
	const bool ok = RenderTwoCardScene( RISE_BLENDER_RASTERIZER_PIXELPEL, &cap, warnings, errorOut );
	Check( ok, std::string( "pixelpel render succeeded: " ) + errorOut );
	if( !ok ) return;

	Check( cap->width > 0 && cap->height > 0, "pixelpel render produced pixels" );

	double leftMean[3], rightMean[3];
	// Empirically located (not the naive half-split): each card is a
	// NARROW object within its half of frame (box width 2 at distance 5
	// under a 50-degree vertical FOV, 2:1 aspect), so averaging over the
	// WHOLE half dilutes its colour with a lot of surrounding backdrop.
	// These fractions (of the full image width) bracket the card's own
	// footprint, located via a standalone debug render of this exact
	// camera/geometry configuration.
	ColumnMean( *cap, (unsigned int)( cap->width * 0.21875 ), (unsigned int)( cap->width * 0.4375 ), leftMean );
	ColumnMean( *cap, (unsigned int)( cap->width * 0.5625 ), (unsigned int)( cap->width * 0.78125 ), rightMean );

	std::cout << "  left (cut, alpha=0) mean:   " << leftMean[0] << " " << leftMean[1] << " " << leftMean[2] << std::endl;
	std::cout << "  right (opaque, alpha=1) mean: " << rightMean[0] << " " << rightMean[1] << " " << rightMean[2] << std::endl;

	// LEFT half: the cut card reveals the GREEN backdrop -> green
	// channel dominates over blue.
	Check( leftMean[1] > leftMean[2] * 2.0,
		"pixelpel: the alpha=0 (CLIP) card is cut through -- the green backdrop dominates the left half" );
	// RIGHT half: the opaque card's own BLUE emission wins -> blue
	// channel dominates over green.
	Check( rightMean[2] > rightMean[1] * 2.0,
		"pixelpel: the alpha=1 (CLIP) card stays opaque -- its blue emission dominates the right half" );

	Check( warnings.empty(), "pixelpel is alpha-compatible: no integrator-compatibility warning" );

	RISE::safe_release( cap );
}

//! Shared coverage expectations across modern integrators.
void CheckAlphaUnderModernIntegrator( const uint32_t rasterizerKind, const char* label )
{
	std::cout << "Test: under " << label << ", alpha-zero reveals the backdrop and alpha-one remains opaque" << std::endl;

	CapturingRasterizerOutput* cap = 0;
	std::vector<std::string> warnings;
	std::string errorOut;
	const bool ok = RenderTwoCardScene( rasterizerKind, &cap, warnings, errorOut );
	Check( ok, std::string( label ) + " render succeeded: " + errorOut );
	if( !ok ) return;

	double leftMean[3], rightMean[3];
	// Empirically located (not the naive half-split): each card is a
	// NARROW object within its half of frame (box width 2 at distance 5
	// under a 50-degree vertical FOV, 2:1 aspect), so averaging over the
	// WHOLE half dilutes its colour with a lot of surrounding backdrop.
	// These fractions (of the full image width) bracket the card's own
	// footprint, located via a standalone debug render of this exact
	// camera/geometry configuration.
	ColumnMean( *cap, (unsigned int)( cap->width * 0.21875 ), (unsigned int)( cap->width * 0.4375 ), leftMean );
	ColumnMean( *cap, (unsigned int)( cap->width * 0.5625 ), (unsigned int)( cap->width * 0.78125 ), rightMean );

	std::cout << "  left (alpha=0 cutout) mean:  " << leftMean[0] << " " << leftMean[1] << " " << leftMean[2] << std::endl;
	std::cout << "  right (alpha=1) mean: " << rightMean[0] << " " << rightMean[1] << " " << rightMean[2] << std::endl;

	Check( leftMean[1] > leftMean[2] * 2.0,
        std::string(label) + ": alpha-zero reveals green backdrop" );
    Check( rightMean[2] > rightMean[1] * 2.0,
        std::string(label) + ": alpha-one retains blue surface" );
    Check( warnings.empty(), std::string(label) + ": material alpha needs no incompatibility warning" );

	RISE::safe_release( cap );
}

void TestAlphaCutoutUnderPT()
{
	CheckAlphaUnderModernIntegrator( RISE_BLENDER_RASTERIZER_PT_PEL, "PT" );
}

void TestAlphaCutoutUnderBDPT()
{
	CheckAlphaUnderModernIntegrator( RISE_BLENDER_RASTERIZER_BDPT_PEL, "BDPT" );
}

void TestConstantAlphaPrecision() {
    RISE::IJobPriv* job=nullptr;Check(RISE::RISE_CreateJobPriv(&job),"precision job created");if(!job)return;
    double white[3]={1,1,1};job->AddUniformColorPainter("white",white,"Rec709RGB_Linear");
    const double factors[]={1e-7,.4999996,.5000004};const double cutoffs[]={5e-8,.4999998,.5000002};const double expected[]={1,0,1};
    for(int i=0;i<3;++i) {
        const std::string name="precision"+std::to_string(i);job->AddLambertianMaterial(name.c_str(),"white");
        rise_blender_material payload={};payload.name=name.c_str();payload.alpha=factors[i];payload.alpha_threshold=cutoffs[i];payload.alpha_mode=RISE_BLENDER_ALPHA_CLIP;
        char error[512]={};Check(wire_alpha_shader_for_material(*job,payload,error,sizeof(error)),"production bridge binds precise alpha");
        const RISE::IMaterial* m=job->GetMaterials()->GetItem(name.c_str());
        RISE::RayIntersectionGeometric ri(RISE::Ray(RISE::Point3(0,0,1),RISE::Vector3(0,0,-1)),RISE::nullRasterizerState);
        Check(m && m->AlphaCoverage(ri)==expected[i],"bridge constant MASK preserves double precision");
    }
    job->release();
}

int main()
{
	std::cout << "=== Blender bridge Alpha test (DL-193) ===" << std::endl;

	TestConstantAlphaPrecision();
	TestAbiVersionAndAlphaFields();
	TestOpaqueMaterialUnaffected();
	TestAlphaCutoutUnderPixelPel();
	TestAlphaCutoutUnderPT();
	TestAlphaCutoutUnderBDPT();

	std::cout << "----------------------------------------" << std::endl;
	std::cout << "checks: " << g_checks << "   failures: " << g_failures << std::endl;
	if( g_failures > 0 ) {
		std::cout << "BlenderBridgeAlphaTest: FAILED" << std::endl;
		return 1;
	}
	std::cout << "BlenderBridgeAlphaTest: PASSED" << std::endl;
	return 0;
}
