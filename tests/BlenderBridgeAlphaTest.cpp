// DL-214: bridge alpha material coverage works under legacy and modern integrators.
// The real bridge producer functions are included below; no warning replicas.

#include <cmath>
#include <thread>
#include <stdexcept>
#include <memory>
#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// The unit under test.  See the banner for why this is a .cpp include.
#include "../src/Blender/native/rise_blender_bridge.cpp"

#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Rendering/FilteredFilm.h"
#include "../src/Library/Rendering/SplatFilm.h"
#include "../src/Library/Sampling/BoxPixelFilter.h"
#include "../src/Library/Sampling/MitchellNetravaliPixelFilter.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color.h"

static int g_checks   = 0;
static int g_failures = 0;

static void Check( const bool ok, const std::string& what )
{
	++g_checks;
	if( ok && std::getenv("RISE_RED_PROVE_MARKER") ) std::cout << "PASS: " << what << std::endl;
	if( !ok ) {
		++g_failures;
		std::cout << "  FAIL: " << what << std::endl;
	}
}

namespace {

void TestOutputBufferSizing()
{
	// Arithmetic-only large cases: never allocate these images.
	if( sizeof(size_t) > sizeof(uint32_t) ) {
		const auto boundary = checked_rgba_storage<float>( 32768, 32768 );
		Check( boundary.values == (std::uint64_t{1} << 32) &&
			boundary.bytes == (std::uint64_t{1} << 34),
			"BlenderBufferSize: 32768 squared RGBA storage does not wrap its channel count" );
		const auto larger = checked_rgba_storage<float>( 65535, 65535 );
		const auto preview = checked_rgba_storage<unsigned short>( 65535, 65535 );
		constexpr std::uint64_t values = 65535ULL * 65535ULL * 4ULL;
		const size_t last = rgba_value_offset( 65534, 65534, 65535 );
		Check( larger.values == values && preview.values == values &&
			last == values - 4 && last > std::numeric_limits<uint32_t>::max(),
			"BlenderBufferSize: final and preview indices preserve offsets above UINT_MAX" );
	} else {
		bool rejected = false;
		try { checked_rgba_storage<float>( 32768, 32768 ); }
		catch( const std::length_error& ) { rejected = true; }
		Check( rejected, "BlenderBufferSize: 32-bit hosts reject unrepresentable storage" );
	}
	bool rejected = false;
	try { checked_rgba_storage<float>( UINT32_MAX, UINT32_MAX ); }
	catch( const std::length_error& ) { rejected = true; }
	Check( rejected, "BlenderBufferSize: native byte/container overflow is rejected before allocation" );
	Check( !valid_image_region(2, 2, 1, 0, 0, 1) && !valid_image_region(2, 2, 0, 1, 1, 0) &&
		!valid_image_region(2, 2, 0, 0, 2, 1) && !valid_image_region(2, 2, 0, 0, 1, 2),
		"BlenderBufferSize: inverted and out-of-bounds rectangles are rejected" );

	struct Preview
	{
		std::vector<unsigned short> pixels;
		unsigned int calls = 0;
		bool dimensions = false;
	} preview;
	const auto capture = []( void* user, const unsigned short* pixels,
		uint32_t width, uint32_t height, uint32_t fullWidth, uint32_t fullHeight,
		uint32_t top, uint32_t left ) -> int {
		auto& observed = *static_cast<Preview*>( user );
		++observed.calls;
		observed.dimensions = width == 1 && height == 2 && fullWidth == 2 && fullHeight == 2 &&
			top == 0 && left == 1;
		const auto layout = checked_rgba_storage<unsigned short>( width, height );
		observed.pixels.assign( pixels, pixels + layout.values );
		return 1;
	};
	MemoryRasterizerOutput output( capture, &preview );
	const unsigned short image16[16] = {
		0, 0, 0, 65535, 100, 200, 300, 400,
		0, 0, 0, 65535, 500, 600, 700, 800
	};
	output.OutputImageRGBA16( image16, 2, 2, 0, 1, 1, 1 );
	rise_blender_render_result result{};
	const bool copied16 = output.CopyToResult( result );
	Check( copied16 && result.width == 2 && result.height == 2 &&
		result.rgba[4] == float(100) / 65535.0f && result.rgba[15] == float(800) / 65535.0f &&
		preview.calls == 1 && preview.dimensions && preview.pixels ==
			std::vector<unsigned short>({100, 200, 300, 400, 500, 600, 700, 800}),
		"BlenderBufferSize: legacy receiver and packed preview preserve rectangle channels" );
	rise_blender_free_render_result( &result );
	const float image32[16] = {
		0, 0, 0, 1, -1, 0.25f, 2, 1,
		0, 0, 0, 1, 0.5f, 0.75f, 0.125f, 0.5f
	};
	output.OutputImageRGBA32F( image32, 2, 2, 0, 1, 1, 1 );
	const bool copied32 = output.CopyToResult( result );
	Check( copied32 && result.rgba[4] == -1 && result.rgba[6] == 2 && result.rgba[15] == 0.5f &&
		preview.calls == 2 && preview.dimensions && preview.pixels ==
			std::vector<unsigned short>({0, 16384, 65535, 65535, 32768, 49151, 8192, 32768}),
		"BlenderBufferSize: float receiver retains HDR while preview clamps and packs its region" );
	rise_blender_free_render_result( &result );
	float nonfinite[16];
	std::memcpy( nonfinite, image32, sizeof(nonfinite) );
	const std::uint32_t nonfiniteBits[] = { 0x7fc00001U, 0x7f800000U, 0xff800000U };
	std::memcpy( nonfinite + 4, nonfiniteBits, sizeof(nonfiniteBits) );
	output.OutputImageRGBA32F( nonfinite, 2, 2, 0, 1, 1, 1 );
	const bool copiedNonfinite = output.CopyToResult( result );
	Check( copiedNonfinite && std::memcmp(result.rgba + 4, nonfiniteBits, sizeof(nonfiniteBits)) == 0 &&
		preview.calls == 3 && preview.pixels ==
			std::vector<unsigned short>({0, 65535, 0, 65535, 32768, 49151, 8192, 32768}),
		"BlenderBufferSize: preview bounds NaN and infinities while final float samples retain their bits" );
	rise_blender_free_render_result( &result );
	output.OutputImageRGBA32F( image32, 2, 2, 0, 0, UINT32_MAX, UINT32_MAX );
	output.OutputImageRGBA16( image16, 2, 2, 1, 1, 0, 0 );
	Check( preview.calls == 3 && output.CopyToResult(result) &&
		std::memcmp(result.rgba + 4, nonfiniteBits, sizeof(nonfiniteBits)) == 0,
		"BlenderBufferSize: invalid receiver rectangles neither invoke preview nor corrupt the image" );
	rise_blender_free_render_result( &result );
}

void TestExportCallbackFailure()
{
	struct State
	{
		rise_blender_camera camera{};
		rise_blender_painter painter{};
		rise_blender_material material{};
		rise_blender_mesh mesh{};
		rise_blender_object object{};
		rise_blender_scene scene{};
		rise_blender_render_settings settings{};
		rise_blender_render_result result{};
		float vertices[9] = {-10,-10,0, 10,-10,0, 0,10,0};
		uint32_t indices[3] = {0,1,2};
		char error[1024]{};
		std::atomic<bool> failCallback{true};
		std::atomic<unsigned int> failureCalls{0};
		std::atomic<unsigned int> healthyCalls{0};
		std::atomic<bool> sawCompleteRegion{false};
		std::atomic<bool> done{false};
		bool escaped = false;
		bool failedReturn = false;
		bool originalCause = false;
		bool clearedResult = false;
		bool healthyRetry = false;
		State()
		{
			camera.projection_type = RISE_BLENDER_CAMERA_PERSPECTIVE;
			camera.location[2] = 3;
			camera.forward[2] = -1;
			camera.up[1] = 1;
			camera.fov_y_radians = 0.7f;
			camera.width = camera.height = 2;
			camera.pixel_aspect = 1;
			painter.name = "bridge_callback_fixture_color";
			painter.kind = RISE_BLENDER_PAINTER_UNIFORM;
			painter.color_space = RISE_BLENDER_COLOR_LINEAR;
			painter.color[0] = 0.2f; painter.color[1] = 0.4f; painter.color[2] = 0.6f;
			material.name = "bridge_callback_fixture_material";
			material.model = RISE_BLENDER_MATERIAL_LAMBERT;
			material.diffuse_painter_name = painter.name;
			material.emission_painter_name = painter.name;
			mesh.name = "bridge_callback_fixture_mesh";
			mesh.vertices = vertices;
			mesh.vertex_indices = indices;
			mesh.num_vertices = 3; mesh.num_triangles = 1;
			mesh.use_face_normals = 1;
			mesh.double_sided = 1;
			object.name = "bridge_callback_fixture_object";
			object.geometry_name = mesh.name;
			object.material_name = material.name;
			object.visible = 1;
			for( unsigned int i = 0; i < 16; ++i ) object.transform[i] = i % 5 == 0 ? 1.0f : 0.0f;
			scene.camera = &camera;
			scene.painters = &painter; scene.num_painters = 1;
			scene.materials = &material; scene.num_materials = 1;
			scene.meshes = &mesh; scene.num_meshes = 1;
			scene.objects = &object; scene.num_objects = 1;
			settings.width = settings.height = 2;
			settings.pixel_samples = settings.light_samples = settings.max_recursion = 1;
			settings.show_lights = 1;
			settings.rasterizer_kind = RISE_BLENDER_RASTERIZER_PT_PEL;
			settings.pixel_filter = RISE_BLENDER_PIXEL_FILTER_NONE;
		}
		~State() { rise_blender_free_render_result( &result ); }
	};
	auto state = std::make_shared<State>();
	// The worker and C callback own/use only heap state. A broken exception
	// cleanup can time out normally without a dangling stack pointer.
	std::thread worker( [state] {
		const auto callback = []( void* user, const unsigned short* pixels,
			uint32_t width, uint32_t height, uint32_t fullWidth, uint32_t fullHeight,
			uint32_t top, uint32_t left ) -> int {
			auto& current = *static_cast<State*>( user );
			if( current.failCallback.load() ) {
				++current.failureCalls;
				throw std::runtime_error( "bridge callback fixture failure" );
			}
			++current.healthyCalls;
			if( pixels && width == 2 && height == 2 && fullWidth == 2 && fullHeight == 2 && top == 0 && left == 0 )
				current.sawCompleteRegion.store( true );
			return 1;
		};
		try {
			int returned = -1;
			try {
				returned = rise_blender_render_scene( &state->scene, &state->settings, nullptr,
					callback, state.get(), &state->result, state->error, sizeof(state->error) );
			} catch( ... ) {
				state->escaped = true;
			}
			state->failedReturn = returned == 0;
			state->originalCause = std::strstr( state->error, "bridge callback fixture failure" ) != nullptr;
			state->clearedResult = state->result.rgba == nullptr && state->result.width == 0 &&
				state->result.height == 0 && state->result.warnings[0] == 0;
			rise_blender_free_render_result( &state->result );
			state->failCallback.store( false );
			returned = rise_blender_render_scene( &state->scene, &state->settings, nullptr,
				callback, state.get(), &state->result, state->error, sizeof(state->error) );
			bool complete = returned == 1 && state->result.rgba && state->result.width == 2 &&
				state->result.height == 2 && state->error[0] == 0 && state->result.warnings[0] == 0;
			for( unsigned int i = 0; i < 4 && complete; ++i ) {
				const float* pixel = state->result.rgba + i * 4;
				complete &= pixel[0] > 0 && pixel[1] > pixel[0] && pixel[2] > pixel[1] &&
					std::isfinite(pixel[0]) && std::isfinite(pixel[1]) && std::isfinite(pixel[2]) &&
					std::isfinite(pixel[3]);
			}
			state->healthyRetry = complete && state->healthyCalls.load() > 0 && state->sawCompleteRegion.load();
			rise_blender_free_render_result( &state->result );
		} catch( ... ) {
			state->escaped = true;
		}
		state->done.store( true, std::memory_order_release );
	} );
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while( !state->done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline )
		std::this_thread::sleep_for( std::chrono::milliseconds(1) );
	const bool completed = state->done.load( std::memory_order_acquire );
	if( completed ) worker.join();
	else worker.detach(); // RED only; its capture owns every input/output object.
	Check( completed, "BlenderExportFailure: failing and healthy four-pixel renders complete within deadline" );
	Check( completed && !state->escaped && state->failureCalls.load() > 0 && state->failedReturn && state->originalCause,
		"BlenderExportFailure: C export contains callback exception and reports its original cause" );
	Check( completed && state->clearedResult,
		"BlenderExportFailure: failed C export clears its result buffer, dimensions and warnings" );
	Check( completed && state->healthyRetry,
		"BlenderExportFailure: healthy C export retry delivers all four emissive pixels and complete preview" );
	// A stuck render can own global pool workers. Do not let static pool
	// destruction turn the deadline's normal assertion failure into a hang.
	// All Check failures above use std::endl, so diagnostics are flushed.
	if( !completed ) std::_Exit(1); // RED only; GREEN joins and cleans up normally.
}

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
	std::cout << "Test: ABI version 20 and preserved append-only v14/v20 fields" << std::endl;

	Check( RISE_BLENDER_API_VERSION == 23, "RISE_BLENDER_API_VERSION is 23" );

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

	// rise_blender_object gained shader_name (v14) and object_color/index/random (v20).
	rise_blender_object obj;
	std::memset( &obj, 0, sizeof(obj) );
	obj.shader_name = "probe";
	Check( std::string( obj.shader_name ) == "probe", "rise_blender_object.shader_name exists and is settable" );
	Check( offsetof( rise_blender_object, object_color ) > offsetof( rise_blender_object, shader_name ),
		"object.object_color is appended after shader_name" );
	Check( offsetof( rise_blender_object, object_index ) > offsetof( rise_blender_object, object_color ),
		"object.object_index follows object_color" );
	Check( offsetof( rise_blender_object, object_random ) > offsetof( rise_blender_object, object_index ),
		"object.object_random follows object_index" );
	Check( RISE_BLENDER_MODIFIER_RELIEF == 2, "RISE_BLENDER_MODIFIER_RELIEF is 2" );
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

void TestFilteredAndSplatFilmTransparentAlpha()
{
	std::cout << "Test: FilteredFilm & SplatFilm transparent background alpha" << std::endl;

	// 1. BoxPixelFilter: opaque fast-path, miss=(0,0,0,0), partial coverage straight RGB + Unresolve round-trip.
	RISE::Implementation::BoxPixelFilter* pBox = new RISE::Implementation::BoxPixelFilter( 1.0, 1.0 );
	RISE::Implementation::FilteredFilm* pFilm = new RISE::Implementation::FilteredFilm( 4, 1 );
	RISE::IRasterImage* pImg = 0;
	Check( RISE::RISE_API_CreateRISEColorRasterImage( &pImg, 4, 1, RISE::RISEColor( 0, 0, 0, 0 ) ) && pImg,
		"FilteredFilmAlpha: image created" );
	if( pImg ) {
		// Pixel 0: opaque hit
		const RISE::RISEPel cOpaque( 0.4, 0.2, 0.8 );
		pFilm->Splat( 0.5, 0.5, RISE::XYZPel( cOpaque ), *pBox, 1.0 );
		// Pixel 1: transparent miss
		pFilm->Splat( 1.5, 0.5, RISE::XYZPel( 0, 0, 0 ), *pBox, 0.0 );
		// Pixel 2: 50% hit + 50% miss -> straight RGB = cEdge, alpha = 0.5
		const RISE::RISEPel cEdge( 0.6, 0.2, 0.1 );
		pFilm->Splat( 2.5, 0.5, RISE::XYZPel( cEdge ), *pBox, 1.0 );
		pFilm->Splat( 2.5, 0.5, RISE::XYZPel( 0, 0, 0 ), *pBox, 0.0 );

		pFilm->Resolve( *pImg );
		const RISE::RISEColor p0 = pImg->GetPEL( 0, 0 );
		const RISE::RISEColor p1 = pImg->GetPEL( 1, 0 );
		const RISE::RISEColor p2 = pImg->GetPEL( 2, 0 );
		Check( std::fabs( p0.a - 1.0 ) < 1e-9 && std::fabs( p0.base.r - 0.4 ) < 1e-6 &&
			std::fabs( p0.base.g - 0.2 ) < 1e-6 && std::fabs( p0.base.b - 0.8 ) < 1e-6,
			"FilteredFilmAlpha: opaque fast-path preserves exact color and alpha=1" );
		Check( p1.a == 0.0 && p1.base.r == 0.0 && p1.base.g == 0.0 && p1.base.b == 0.0,
			"FilteredFilmAlpha: all-miss pixel resolves to (0,0,0,0)" );
		Check( std::fabs( p2.a - 0.5 ) < 1e-9 && std::fabs( p2.base.r - 0.6 ) < 1e-5 &&
			std::fabs( p2.base.g - 0.2 ) < 1e-5 && std::fabs( p2.base.b - 0.1 ) < 1e-5,
			"FilteredFilmAlpha: 50% coverage resolves to straight RGB with alpha=0.5" );

		// SplatFilm on partial-coverage edge unpremultiplies splat so premultiplied output preserves energy.
		RISE::Implementation::SplatFilm* pSplat = new RISE::Implementation::SplatFilm( 4, 1 );
		pSplat->Splat( 2, 0, RISE::RISEPel( 0.1, 0.0, 0.0 ) );
		pSplat->FlushCallingThreadBuffer();
		pSplat->Resolve( *pImg, 1.0 );
		const RISE::RISEColor p2Splat = pImg->GetPEL( 2, 0 );
		Check( std::fabs( p2Splat.a - 0.5 ) < 1e-9 && std::fabs( p2Splat.base.r - 0.8 ) < 1e-5,
			"SplatFilmAlpha: partial-coverage splat unpremultiplies by alpha (0.6 + 0.1/0.5 = 0.8)" );
		pSplat->Unresolve( *pImg, 1.0 );
		const RISE::RISEColor p2Unsplat = pImg->GetPEL( 2, 0 );
		Check( std::fabs( p2Unsplat.base.r - 0.6 ) < 1e-5,
			"SplatFilmAlpha: Unresolve restores straight edge color" );
		RISE::safe_release( pSplat );

		// Unresolve cancels partial-coverage pixel back to zero.
		pFilm->Unresolve( *pImg );
		const RISE::RISEColor p2Zero = pImg->GetPEL( 2, 0 );
		Check( std::fabs( p2Zero.base.r ) < 1e-6 && std::fabs( p2Zero.base.g ) < 1e-6 &&
			std::fabs( p2Zero.base.b ) < 1e-6,
			"FilteredFilmAlpha: Unresolve cancels partial-coverage straight RGB" );

		RISE::safe_release( pImg );
	}
	RISE::safe_release( pFilm );
	RISE::safe_release( pBox );

	// 2. Mitchell-Netravali negative-lobe filter across a hit/miss edge clamps alpha to [0,1] cleanly.
	RISE::Implementation::MitchellNetravaliPixelFilter* pMitch = new RISE::Implementation::MitchellNetravaliPixelFilter( 1.0 / 3.0, 1.0 / 3.0 );
	RISE::Implementation::FilteredFilm* pMFilm = new RISE::Implementation::FilteredFilm( 6, 6 );
	RISE::IRasterImage* pMImg = 0;
	Check( RISE::RISE_API_CreateRISEColorRasterImage( &pMImg, 6, 6, RISE::RISEColor( 0, 0, 0, 0 ) ) && pMImg,
		"FilteredFilmAlpha: Mitchell image created" );
	if( pMImg ) {
		for( int y = 0; y < 6; ++y ) {
			for( int x = 0; x < 6; ++x ) {
				const bool hit = ( x < 3 );
				pMFilm->Splat( x + 0.5, y + 0.5,
					hit ? RISE::XYZPel( RISE::RISEPel( 0.9, 0.3, 0.2 ) ) : RISE::XYZPel( 0, 0, 0 ),
					*pMitch, hit ? 1.0 : 0.0 );
			}
		}
		pMFilm->Resolve( *pMImg );
		bool allFiniteAndClamped = true;
		for( unsigned int y = 0; y < 6; ++y ) {
			for( unsigned int x = 0; x < 6; ++x ) {
				const RISE::RISEColor c = pMImg->GetPEL( x, y );
				if( !std::isfinite( c.a ) || c.a < 0.0 || c.a > 1.0 ||
					!std::isfinite( c.base.r ) || !std::isfinite( c.base.g ) || !std::isfinite( c.base.b ) )
				{
					allFiniteAndClamped = false;
				}
			}
		}
		Check( allFiniteAndClamped,
			"FilteredFilmAlpha: Mitchell-Netravali negative lobes clamp alpha to [0,1] and remain finite" );
		RISE::safe_release( pMImg );
	}
	RISE::safe_release( pMFilm );
	RISE::safe_release( pMitch );
}

void TestBridgeTransparentFilmRegionAndExpression()
{
	std::cout << "Test: bridge ABI v19 transparent film, render region, and expression painter" << std::endl;

	const uint32_t W = 32;
	const uint32_t H = 24;

	rise_blender_camera cam{};
	cam.projection_type = RISE_BLENDER_CAMERA_PERSPECTIVE;
	cam.location[0] = 0.0f; cam.location[1] = 0.0f; cam.location[2] = 4.0f;
	cam.forward[0] = 0.0f; cam.forward[1] = 0.0f; cam.forward[2] = -1.0f;
	cam.up[0] = 0.0f; cam.up[1] = 1.0f; cam.up[2] = 0.0f;
	cam.fov_y_radians = 50.0f * 3.14159265f / 180.0f;
	cam.width = W;
	cam.height = H;
	cam.pixel_aspect = 1.0f;

	// Centered 2x2 quad in [-1,1]x[-1,1] at z=0: center hits quad, corners miss to world.
	const float positions[12] = {
		-1.0f, -1.0f, 0.0f,
		 1.0f, -1.0f, 0.0f,
		 1.0f,  1.0f, 0.0f,
		-1.0f,  1.0f, 0.0f,
	};
	const float normals[12] = {
		0.0f, 0.0f, 1.0f,
		0.0f, 0.0f, 1.0f,
		0.0f, 0.0f, 1.0f,
		0.0f, 0.0f, 1.0f,
	};
	const float uvs[8] = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
	const uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };

	rise_blender_mesh mesh{};
	mesh.name = "voronoi_quad_mesh";
	mesh.vertices = positions;
	mesh.normals = normals;
	mesh.uvs = uvs;
	mesh.vertex_indices = indices;
	mesh.normal_indices = indices;
	mesh.uv_indices = indices;
	mesh.num_vertices = 4;
	mesh.num_normals = 4;
	mesh.num_uvs = 4;
	mesh.num_triangles = 2;
	mesh.double_sided = 1;
	mesh.use_face_normals = 0;

	rise_blender_painter exprPainter{};
	exprPainter.name = "voronoi_expr_painter";
	exprPainter.kind = RISE_BLENDER_PAINTER_EXPRESSION;
	exprPainter.expression =
		"def _p ((Po - vec3(-1, -1, 0)) * vec3(0.5, 0.5, 0))\n"
		"osl_voronoi_color(_p, 0, 3, 0, 0, 4, 0, 0.5, 2, 1, 0.5, 1, 0)";

	rise_blender_painter roughPainter{};
	roughPainter.name = "rough_const";
	roughPainter.kind = RISE_BLENDER_PAINTER_UNIFORM;
	roughPainter.color[0] = 0.8f; roughPainter.color[1] = 0.8f; roughPainter.color[2] = 0.8f;

	rise_blender_painter metalPainter{};
	metalPainter.name = "metal_const";
	metalPainter.kind = RISE_BLENDER_PAINTER_UNIFORM;
	metalPainter.color[0] = 0.0f; metalPainter.color[1] = 0.0f; metalPainter.color[2] = 0.0f;

	rise_blender_painter worldPainter{};
	worldPainter.name = "world_sky";
	worldPainter.kind = RISE_BLENDER_PAINTER_UNIFORM;
	worldPainter.color[0] = 0.25f; worldPainter.color[1] = 0.45f; worldPainter.color[2] = 0.90f;

	rise_blender_painter painters[4] = { exprPainter, roughPainter, metalPainter, worldPainter };

	rise_blender_material mat{};
	mat.name = "voronoi_mat";
	mat.model = RISE_BLENDER_MATERIAL_PBR_METALLIC_ROUGHNESS;
	mat.base_color_painter_name = "voronoi_expr_painter";
	mat.roughness_painter_name = "rough_const";
	mat.metallic_painter_name = "metal_const";
	mat.double_sided = 1;
	mat.alpha = 1.0f;
	mat.alpha_mode = RISE_BLENDER_ALPHA_OPAQUE;

	rise_blender_object obj{};
	obj.name = "voronoi_quad";
	obj.geometry_name = "voronoi_quad_mesh";
	obj.material_name = "voronoi_mat";
	IdentityTranslate( obj.transform, 0.0f, 0.0f, 0.0f );
	obj.casts_shadows = 1;
	obj.receives_shadows = 1;
	obj.visible = 1;

	rise_blender_scene scene{};
	scene.camera = &cam;
	scene.painters = painters;
	scene.num_painters = 4;
	scene.materials = &mat;
	scene.num_materials = 1;
	scene.meshes = &mesh;
	scene.num_meshes = 1;
	scene.objects = &obj;
	scene.num_objects = 1;
	scene.world_color[0] = 0.25f;
	scene.world_color[1] = 0.45f;
	scene.world_color[2] = 0.90f;
	scene.world_strength = 1.5f;
	scene.world_radiance_painter_name = "world_sky";
	scene.world_radiance_scale = 1.5f;
	scene.world_radiance_is_background = 1;

	auto makeSettings = [&]( uint32_t rastKind, int transparent, int useRegion ) {
		rise_blender_render_settings s{};
		s.width = W;
		s.height = H;
		s.pixel_samples = 4;
		s.light_samples = 1;
		s.max_recursion = 3;
		s.rasterizer_kind = rastKind;
		s.use_path_tracing = 1;
		s.film_transparent = transparent;
		s.use_region = useRegion;
		return s;
	};

	char err[512] = {};

	// A. Control: film_transparent=0 -> corner has opaque blue world background.
	{
		rise_blender_render_settings s = makeSettings( RISE_BLENDER_RASTERIZER_PT_PEL, 0, 0 );
		rise_blender_render_result res{};
		const int ok = rise_blender_render_scene( &scene, &s, nullptr, nullptr, nullptr, &res, err, sizeof(err) );
		Check( ok == 1 && res.rgba && res.width == W && res.height == H,
			std::string( "ABI19 opaque control rendered: " ) + err );
		if( ok == 1 && res.rgba ) {
			const float* corner = res.rgba; // (0,0)
			Check( std::fabs( corner[3] - 1.0f ) < 1e-4f && corner[2] > 0.2f,
				"ABI19 opaque control: corner pixel has alpha=1 and visible blue world background" );
		}
		rise_blender_free_render_result( &res );
	}

	// B. Transparent film across PT, BDPT, VCM (Pel & Spectral), Auto Pel, and PixelPel + Voronoi variation.
	const struct { uint32_t kind; const char* name; } rastCases[] = {
		{ RISE_BLENDER_RASTERIZER_PT_PEL, "PT_Pel" },
		{ RISE_BLENDER_RASTERIZER_PT_SPECTRAL, "PT_Spectral" },
		{ RISE_BLENDER_RASTERIZER_BDPT_PEL, "BDPT_Pel" },
		{ RISE_BLENDER_RASTERIZER_BDPT_SPECTRAL, "BDPT_Spectral" },
		{ RISE_BLENDER_RASTERIZER_VCM_PEL, "VCM_Pel" },
		{ RISE_BLENDER_RASTERIZER_VCM_SPECTRAL, "VCM_Spectral" },
		{ RISE_BLENDER_RASTERIZER_AUTO_PEL, "Auto_Pel" },
		{ RISE_BLENDER_RASTERIZER_PIXELPEL, "PixelPel" },
	};
	for( const auto& rc : rastCases ) {
		rise_blender_render_settings s = makeSettings( rc.kind, 1, 0 );
		rise_blender_render_result res{};
		err[0] = '\0';
		const int ok = rise_blender_render_scene( &scene, &s, nullptr, nullptr, nullptr, &res, err, sizeof(err) );
		Check( ok == 1 && res.rgba && res.width == W && res.height == H,
			std::string( "ABI19 transparent film (" ) + rc.name + ") rendered: " + err );
		if( ok == 1 && res.rgba ) {
			const float* c0 = res.rgba; // corner (0,0)
			const float* c1 = res.rgba + ( (H - 1) * W + (W - 1) ) * 4; // corner (W-1, H-1)
			Check( c0[0] == 0.0f && c0[1] == 0.0f && c0[2] == 0.0f && c0[3] == 0.0f &&
				c1[0] == 0.0f && c1[1] == 0.0f && c1[2] == 0.0f && c1[3] == 0.0f,
				std::string( "ABI19 transparent film (" ) + rc.name + "): corners are (0,0,0,0)" );

			// Center region inside quad: opaque, lit by IBL, and spatially varying Voronoi colors.
			float minR = 1e9f, maxR = -1e9f, minG = 1e9f, maxG = -1e9f;
			int opaqueCount = 0;
			for( uint32_t y = 8; y < 16; ++y ) {
				for( uint32_t x = 12; x < 20; ++x ) {
					const float* px = res.rgba + ( y * W + x ) * 4;
					if( std::fabs( px[3] - 1.0f ) < 1e-4f ) ++opaqueCount;
					if( px[0] < minR ) minR = px[0];
					if( px[0] > maxR ) maxR = px[0];
					if( px[1] < minG ) minG = px[1];
					if( px[1] > maxG ) maxG = px[1];
				}
			}
			Check( opaqueCount == 64 && maxR > 0.02f,
				std::string( "ABI19 transparent film (" ) + rc.name + "): center quad is opaque and world-IBL lit" );
			if( rc.kind == RISE_BLENDER_RASTERIZER_PT_PEL ) {
				Check( (maxR - minR) > 0.05f && (maxG - minG) > 0.05f,
					"ABI19 expression painter: Voronoi Color produces spatially varying RGB cells across quad" );
			}
		}
		rise_blender_free_render_result( &res );
	}

	// C. Render region (use_region=1): rasterizes only [8..23] x [6..17], leaves outside untouched.
	{
		rise_blender_render_settings s = makeSettings( RISE_BLENDER_RASTERIZER_PT_PEL, 0, 1 );
		s.region_left = 8;
		s.region_top = 6;
		s.region_right = 23;
		s.region_bottom = 17;
		rise_blender_render_result res{};
		err[0] = '\0';
		const int ok = rise_blender_render_scene( &scene, &s, nullptr, nullptr, nullptr, &res, err, sizeof(err) );
		Check( ok == 1 && res.rgba && res.width == W && res.height == H,
			std::string( "ABI19 render region rendered: " ) + err );
		if( ok == 1 && res.rgba ) {
			const float* outside = res.rgba; // (0,0) outside region
			const float* inside = res.rgba + ( 12 * W + 16 ) * 4; // center inside region
			Check( outside[0] == 0.0f && outside[1] == 0.0f && outside[2] == 0.0f && outside[3] == 0.0f,
				"ABI19 render region: pixels outside region remain unwritten (0,0,0,0)" );
			Check( std::fabs( inside[3] - 1.0f ) < 1e-4f && (inside[0] + inside[1] + inside[2]) > 0.05f,
				"ABI19 render region: pixels inside region are rasterized" );
		}
		rise_blender_free_render_result( &res );
	}

	// D. Expression painter validation: multiple root lines rejected; compiler diagnostic propagated.
	{
		RISE::IJobPriv* job = nullptr;
		Check( RISE::RISE_CreateJobPriv( &job ) && job, "ABI19 expr validation job created" );
		if( job ) {
			rise_blender_painter badMulti{};
			badMulti.name = "bad_multi";
			badMulti.kind = RISE_BLENDER_PAINTER_EXPRESSION;
			badMulti.expression = "vec3(1, 0, 0)\nvec3(0, 1, 0)";
			err[0] = '\0';
			Check( !add_painter( *job, badMulti, err, sizeof(err) ) &&
				std::strstr( err, "multiple root" ) != nullptr,
				"ABI19 expression painter rejects multiple non-def root lines" );

			rise_blender_painter badSyntax{};
			badSyntax.name = "bad_syntax";
			badSyntax.kind = RISE_BLENDER_PAINTER_EXPRESSION;
			badSyntax.expression = "unknown_nonexistent_fn(Po)";
			err[0] = '\0';
			Check( !add_painter( *job, badSyntax, err, sizeof(err) ) &&
				std::strstr( err, "unknown_nonexistent_fn" ) != nullptr,
				"ABI19 expression painter propagates RISE compiler diagnostic into error_message" );

			job->release();
		}
	}

	// E. Regression test (Round 2 P1-1): direct SetTransparentBackground(true)
	//    with a bright (100.0) isBackground=true radiance map behind a pure-black
	//    quad must NOT leak sky radiance into partial-coverage silhouette edges.
	{
		const struct { uint32_t kind; int filter; float filterW; const char* name; } haloCases[] = {
			{ RISE_BLENDER_RASTERIZER_PT_PEL, RISE_BLENDER_PIXEL_FILTER_BOX, 1.0f, "PT_Pel_Box" },
			{ RISE_BLENDER_RASTERIZER_PT_PEL, RISE_BLENDER_PIXEL_FILTER_GAUSSIAN, 1.5f, "PT_Pel_Gaussian" },
			{ RISE_BLENDER_RASTERIZER_PT_SPECTRAL, RISE_BLENDER_PIXEL_FILTER_GAUSSIAN, 1.5f, "PT_Spectral" },
			{ RISE_BLENDER_RASTERIZER_BDPT_PEL, RISE_BLENDER_PIXEL_FILTER_GAUSSIAN, 1.5f, "BDPT_Pel" },
			{ RISE_BLENDER_RASTERIZER_BDPT_SPECTRAL, RISE_BLENDER_PIXEL_FILTER_GAUSSIAN, 1.5f, "BDPT_Spectral" },
			{ RISE_BLENDER_RASTERIZER_VCM_PEL, RISE_BLENDER_PIXEL_FILTER_GAUSSIAN, 1.5f, "VCM_Pel" },
			{ RISE_BLENDER_RASTERIZER_VCM_SPECTRAL, RISE_BLENDER_PIXEL_FILTER_GAUSSIAN, 1.5f, "VCM_Spectral" },
		};
		for( const auto& hc : haloCases ) {
			RISE::IJobPriv* job = nullptr;
			if( !RISE::RISE_CreateJobPriv( &job ) || !job ) {
				Check( false, std::string( "HaloRegression (" ) + hc.name + "): job creation failed" );
				continue;
			}
			job->SetPrimaryAcceleration( true, false, 4, 32 );
			// Slight subpixel offset on quad X edge guarantees fractional-alpha silhouette pixels.
			const float edgePos[12] = {
				-0.73f, -0.73f, 0.0f,
				 0.73f, -0.73f, 0.0f,
				 0.73f,  0.73f, 0.0f,
				-0.73f,  0.73f, 0.0f,
			};
			rise_blender_mesh edgeMesh = mesh;
			edgeMesh.name = "black_edge_quad_mesh";
			edgeMesh.vertices = edgePos;

			rise_blender_painter blackP{};
			blackP.name = "pure_black";
			blackP.kind = RISE_BLENDER_PAINTER_UNIFORM;
			blackP.color[0] = 0.0f; blackP.color[1] = 0.0f; blackP.color[2] = 0.0f;

			rise_blender_painter skyP{};
			skyP.name = "bright_sky";
			skyP.kind = RISE_BLENDER_PAINTER_UNIFORM;
			skyP.color[0] = 100.0f; skyP.color[1] = 100.0f; skyP.color[2] = 100.0f;

			rise_blender_painter hPainters[2] = { blackP, skyP };
			rise_blender_material hMat{};
			hMat.name = "pure_black_mat";
			hMat.model = RISE_BLENDER_MATERIAL_LAMBERT;
			hMat.diffuse_painter_name = "pure_black";
			hMat.double_sided = 1;
			hMat.alpha = 1.0f;
			hMat.alpha_mode = RISE_BLENDER_ALPHA_OPAQUE;

			rise_blender_object hObj = obj;
			hObj.name = "black_edge_quad";
			hObj.geometry_name = "black_edge_quad_mesh";
			hObj.material_name = "pure_black_mat";

			rise_blender_scene hScene{};
			hScene.camera = &cam;
			hScene.painters = hPainters;
			hScene.num_painters = 2;
			hScene.materials = &hMat;
			hScene.num_materials = 1;
			hScene.meshes = &edgeMesh;
			hScene.num_meshes = 1;
			hScene.objects = &hObj;
			hScene.num_objects = 1;
			hScene.world_radiance_painter_name = "bright_sky";
			hScene.world_radiance_scale = 1.0f;
			hScene.world_radiance_is_background = 1;

			// Configure rasterizer with film_transparent=0 so isBackground stays true,
			// then enable SetTransparentBackground(true) directly on IRasterizer.
			rise_blender_render_settings hs = makeSettings( hc.kind, 0, 0 );
			hs.pixel_samples = 16;
			hs.max_recursion = 2;
			hs.pixel_filter = hc.filter;
			hs.pixel_filter_width = hc.filterW;

			bool setupOk =
				configure_camera( *job, cam, err, sizeof(err) ) &&
				add_painter( *job, hPainters[0], err, sizeof(err) ) &&
				add_painter( *job, hPainters[1], err, sizeof(err) ) &&
				add_material( *job, hMat, err, sizeof(err) ) &&
				add_mesh( *job, edgeMesh, err, sizeof(err) ) &&
				add_object( *job, hObj, err, sizeof(err) ) &&
				configure_shader( *job, hs, err, sizeof(err) ) &&
				configure_rasterizer( *job, hs, hScene, err, sizeof(err) );
			Check( setupOk, std::string( "HaloRegression (" ) + hc.name + ") setup: " + err );
			if( setupOk ) {
				job->GetRasterizer()->SetTransparentBackground( true );
				CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
				job->GetRasterizer()->AddRasterizerOutput( cap );
				job->Rasterize();
				int partialEdges = 0;
				double maxEdgeRGB = 0.0;
				double maxInteriorRGB = 0.0;
				for( const RISE::RISEColor& c : cap->pixels ) {
					const double m = std::max( { std::fabs( c.base.r ), std::fabs( c.base.g ), std::fabs( c.base.b ) } );
					if( c.a > 0.05 && c.a < 0.95 ) {
						++partialEdges;
						if( m > maxEdgeRGB ) maxEdgeRGB = m;
					} else if( c.a >= 0.99 ) {
						if( m > maxInteriorRGB ) maxInteriorRGB = m;
					}
				}
				// Against a 100.0 sky, any missed-ray sky leakage blows up silhouette
				// edges to >> 10.0; spectral Jakob-Hanika uplift of (0,0,0) reflects
				// ~5e-5 of the 100.0 sky uniformly across both interior and edges (~0.005).
				const double edgeLimit = maxInteriorRGB * 2.0 + 0.01;
				Check( partialEdges > 0 && maxEdgeRGB <= edgeLimit && maxEdgeRGB < 0.05,
					std::string( "HaloRegression (" ) + hc.name + "): silhouette edges (" +
					std::to_string( partialEdges ) + ") have zero sky-halo leakage (maxEdge=" +
					std::to_string( maxEdgeRGB ) + ", maxInterior=" + std::to_string( maxInteriorRGB ) + ")" );
				RISE::safe_release( cap );
			}
			job->release();
		}
	}
}

void TestBridgeObjectInfoAndReliefModifier()
{
	std::cout << "Test: ABI v20 ObjectInfo per-object attributes + RISE_BLENDER_MODIFIER_RELIEF" << std::endl;

	const uint32_t W = 32;
	const uint32_t H = 16;

	// Build two flat quads side by side (left x in [-1.8, -0.2], right x in [0.2, 1.8])
	// in the XZ plane facing -Y, viewed by an orthographic camera at (0, -5, 0) looking +Y
	// with up +Z (so -X is left, +X is right, +Z is up; blender_matrix_to_rise_matrix
	// transposes row-major to RISE column-major without swapping axes).
	rise_blender_camera cam;
	std::memset( &cam, 0, sizeof(cam) );
	cam.projection_type = RISE_BLENDER_CAMERA_ORTHOGRAPHIC;
	cam.location[0] = 0.0f; cam.location[1] = -5.0f; cam.location[2] = 0.0f;
	cam.forward[0]  = 0.0f; cam.forward[1]  =  1.0f; cam.forward[2]  = 0.0f;
	cam.up[0]       = 0.0f; cam.up[1]       =  0.0f; cam.up[2]       = 1.0f;
	cam.ortho_scale = 4.0f;
	cam.width = W;
	cam.height = H;
	cam.pixel_aspect = 1.0f;

	// Unit quad centered at origin in Blender XZ plane (facing -Y toward camera).
	const float verts[12] = {
		-0.8f, 0.0f, -0.8f,
		 0.8f, 0.0f, -0.8f,
		 0.8f, 0.0f,  0.8f,
		-0.8f, 0.0f,  0.8f
	};
	const float norms[12] = {
		0.0f, -1.0f, 0.0f,
		0.0f, -1.0f, 0.0f,
		0.0f, -1.0f, 0.0f,
		0.0f, -1.0f, 0.0f
	};
	const float uvs[8] = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
	const uint32_t idx[6] = { 0, 1, 2, 0, 2, 3 };

	rise_blender_mesh quadMesh;
	std::memset( &quadMesh, 0, sizeof(quadMesh) );
	quadMesh.name = "quad";
	quadMesh.vertices = verts;
	quadMesh.normals = norms;
	quadMesh.uvs = uvs;
	quadMesh.vertex_indices = idx;
	quadMesh.normal_indices = idx;
	quadMesh.uv_indices = idx;
	quadMesh.num_vertices = 4;
	quadMesh.num_normals = 4;
	quadMesh.num_uvs = 4;
	quadMesh.num_triangles = 2;
	quadMesh.double_sided = 1;

	// Part 1: Two objects sharing ONE emissive expression material reading ALL
	// 6 ObjectInfo fields (color.xyz, alpha, index, random), rendered under BOTH
	// PT and BDPT (also testing BDPTVertex::pObjectInfo + LightSampler::FillEmitterRecord!).
	for( int rk = 0; rk < 2; ++rk ) {
		const uint32_t kind = (rk == 0) ? RISE_BLENDER_RASTERIZER_PT_PEL : RISE_BLENDER_RASTERIZER_BDPT_PEL;
		const char* tag = (rk == 0) ? "PT" : "BDPT";

		rise_blender_painter exprP;
		std::memset( &exprP, 0, sizeof(exprP) );
		exprP.name = "p_objinfo";
		exprP.kind = RISE_BLENDER_PAINTER_EXPRESSION;
		exprP.expression = "vec3(osl_object_color().x + osl_object_color().y * 0.25, osl_object_random() + osl_object_color().z * 0.1, osl_object_index() * 0.1 + osl_object_alpha() * 0.5)";

		rise_blender_painter blackP;
		std::memset( &blackP, 0, sizeof(blackP) );
		blackP.name = "p_black";
		blackP.kind = RISE_BLENDER_PAINTER_UNIFORM;

		rise_blender_material mat;
		std::memset( &mat, 0, sizeof(mat) );
		mat.name = "m_objinfo";
		mat.model = RISE_BLENDER_MATERIAL_LAMBERT;
		mat.diffuse_painter_name = "p_black";
		mat.emission_painter_name = "p_objinfo";
		mat.double_sided = 1;

		rise_blender_object objs[2];
		std::memset( objs, 0, sizeof(objs) );
		// Left quad at x = -1.0: R=0.4+0.8*0.25=0.60, G=0.15+0.5*0.1=0.20, B=0.2+0.2=0.40
		objs[0].name = "quad_left";
		objs[0].geometry_name = "quad";
		objs[0].material_name = "m_objinfo";
		objs[0].transform[0] = 1.0f; objs[0].transform[5] = 1.0f;
		objs[0].transform[10] = 1.0f; objs[0].transform[15] = 1.0f;
		objs[0].transform[3] = -1.0f;
		objs[0].casts_shadows = 1; objs[0].receives_shadows = 1; objs[0].visible = 1;
		objs[0].object_color[0] = 0.4f; objs[0].object_color[1] = 0.8f;
		objs[0].object_color[2] = 0.5f; objs[0].object_color[3] = 0.4f;
		objs[0].object_index = 2.0f;
		objs[0].object_random = 0.15f;

		// Right quad at x = +1.0: R=0.1+0.4*0.25=0.20, G=0.80+1.0*0.1=0.90, B=0.5+0.5=1.00
		objs[1] = objs[0];
		objs[1].name = "quad_right";
		objs[1].transform[3] = 1.0f;
		objs[1].object_color[0] = 0.1f; objs[1].object_color[1] = 0.4f;
		objs[1].object_color[2] = 1.0f; objs[1].object_color[3] = 1.0f;
		objs[1].object_index = 5.0f;
		objs[1].object_random = 0.80f;

		rise_blender_painter painters[2] = { exprP, blackP };
		rise_blender_scene scene;
		std::memset( &scene, 0, sizeof(scene) );
		scene.camera = &cam;
		scene.painters = painters; scene.num_painters = 2;
		scene.materials = &mat; scene.num_materials = 1;
		scene.meshes = &quadMesh; scene.num_meshes = 1;
		scene.objects = objs; scene.num_objects = 2;

		rise_blender_render_settings settings;
		std::memset( &settings, 0, sizeof(settings) );
		settings.width = W; settings.height = H;
		settings.pixel_samples = 4; settings.max_recursion = 2;
		settings.show_lights = 1;
		settings.rasterizer_kind = kind;

		rise_blender_render_result res;
		std::memset( &res, 0, sizeof(res) );
		char err[512] = {0};
		const int ok = rise_blender_render_scene( &scene, &settings, 0, 0, 0, &res, err, sizeof(err) );
		Check( ok != 0 && res.rgba != 0, std::string( "ABI v20 ObjectInfo render (" ) + tag + ") ok: " + err );
		if( ok && res.rgba ) {
			const uint32_t cy = H / 2;
			const uint32_t lx = W / 4;
			const uint32_t rx = (3 * W) / 4;
			const float* pL = &res.rgba[(cy * W + lx) * 4];
			const float* pR = &res.rgba[(cy * W + rx) * 4];
			const float pi = 3.14159265358979323846f;
			const float lR = pL[0] * pi, lG = pL[1] * pi, lB = pL[2] * pi;
			const float rR = pR[0] * pi, rG = pR[1] * pi, rB = pR[2] * pi;
			Check( std::fabs( lR - 0.60f ) < 0.05f && std::fabs( lG - 0.20f ) < 0.05f && std::fabs( lB - 0.40f ) < 0.05f,
				std::string( "Left quad (" ) + tag + ") rendered all 6 ObjectInfo fields (got " +
				std::to_string( lR ) + "," + std::to_string( lG ) + "," + std::to_string( lB ) + ")" );
			Check( std::fabs( rR - 0.20f ) < 0.05f && std::fabs( rG - 0.90f ) < 0.05f && std::fabs( rB - 1.00f ) < 0.05f,
				std::string( "Right quad (" ) + tag + ") rendered all 6 ObjectInfo fields (got " +
				std::to_string( rR ) + "," + std::to_string( rG ) + "," + std::to_string( rB ) + ")" );
			rise_blender_free_render_result( &res );
		}
	}

	// Part 1b: Missing source painter for RISE_BLENDER_MODIFIER_RELIEF fails cleanly.
	{
		RISE::IJobPriv* errJob = 0;
		RISE::RISE_CreateJobPriv( &errJob );
		rise_blender_modifier badMod;
		std::memset( &badMod, 0, sizeof(badMod) );
		badMod.name = "mod_missing";
		badMod.kind = RISE_BLENDER_MODIFIER_RELIEF;
		badMod.source_painter_name = "does_not_exist";
		badMod.scale = 1.0f;
		char errBuf[256] = {0};
		const bool added = add_modifier( *errJob, badMod, errBuf, sizeof(errBuf) );
		Check( !added && std::string( errBuf ).find( "not found" ) != std::string::npos,
			"RISE_BLENDER_MODIFIER_RELIEF rejects missing source painter cleanly" );
		RISE::safe_release( errJob );
	}

	// Part 2: RISE_BLENDER_MODIFIER_RELIEF with procedural expression_painter height.
	// Height h(Po) = Po.x (rises along +X, so grad(h) = (+1, 0, 0)).
	// Point light on the -X side (-6, -2, 0):
	//   - positive scale (+1.0, Invert OFF): Blinn N' = N - scale * grad(h)
	//     tilts normal toward -X (TOWARD the light) -> BRIGHTER!
	//   - negative scale (-1.0, Invert ON): tilts normal toward +X (AWAY) -> DARKER!
	// Placing quad_pos farther (+1) and quad_neg closer (-1) to the -X light ensures
	// quad_pos only wins because the relief normal tilt really faces the light.
	{
		rise_blender_painter greyP;
		std::memset( &greyP, 0, sizeof(greyP) );
		greyP.name = "p_grey";
		greyP.kind = RISE_BLENDER_PAINTER_UNIFORM;
		greyP.color[0] = 0.8f; greyP.color[1] = 0.8f; greyP.color[2] = 0.8f;

		rise_blender_painter heightP;
		std::memset( &heightP, 0, sizeof(heightP) );
		heightP.name = "p_height_expr";
		heightP.kind = RISE_BLENDER_PAINTER_EXPRESSION;
		heightP.expression = "Po.x";

		rise_blender_modifier mods[2];
		std::memset( mods, 0, sizeof(mods) );
		mods[0].name = "mod_relief_pos";
		mods[0].kind = RISE_BLENDER_MODIFIER_RELIEF;
		mods[0].source_painter_name = "p_height_expr";
		mods[0].scale = 1.0f;
		mods[0].window = 0.0f;

		mods[1] = mods[0];
		mods[1].name = "mod_relief_neg";
		mods[1].scale = -1.0f;

		rise_blender_material mat;
		std::memset( &mat, 0, sizeof(mat) );
		mat.name = "m_grey";
		mat.model = RISE_BLENDER_MATERIAL_LAMBERT;
		mat.diffuse_painter_name = "p_grey";
		mat.double_sided = 1;

		rise_blender_light light;
		std::memset( &light, 0, sizeof(light) );
		light.name = "side_light";
		light.type = RISE_BLENDER_LIGHT_POINT;
		light.color[0] = 1.0f; light.color[1] = 1.0f; light.color[2] = 1.0f;
		light.intensity = 250.0f;
		light.position[0] = -6.0f; light.position[1] = -2.0f; light.position[2] = 0.0f;

		rise_blender_object objs[2];
		std::memset( objs, 0, sizeof(objs) );
		// quad_neg on left (-1, closer to -X light)
		objs[0].name = "quad_neg";
		objs[0].geometry_name = "quad";
		objs[0].material_name = "m_grey";
		objs[0].modifier_name = "mod_relief_neg";
		objs[0].transform[0] = 1.0f; objs[0].transform[5] = 1.0f;
		objs[0].transform[10] = 1.0f; objs[0].transform[15] = 1.0f;
		objs[0].transform[3] = -1.0f;
		objs[0].casts_shadows = 1; objs[0].receives_shadows = 1; objs[0].visible = 1;

		// quad_pos on right (+1, farther from -X light)
		objs[1] = objs[0];
		objs[1].name = "quad_pos";
		objs[1].modifier_name = "mod_relief_pos";
		objs[1].transform[3] = 1.0f;

		rise_blender_painter painters[2] = { greyP, heightP };
		rise_blender_scene scene;
		std::memset( &scene, 0, sizeof(scene) );
		scene.camera = &cam;
		scene.painters = painters; scene.num_painters = 2;
		scene.modifiers = mods; scene.num_modifiers = 2;
		scene.materials = &mat; scene.num_materials = 1;
		scene.meshes = &quadMesh; scene.num_meshes = 1;
		scene.objects = objs; scene.num_objects = 2;
		scene.lights = &light; scene.num_lights = 1;

		rise_blender_render_settings settings;
		std::memset( &settings, 0, sizeof(settings) );
		settings.width = W; settings.height = H;
		settings.pixel_samples = 4; settings.max_recursion = 2;
		settings.rasterizer_kind = RISE_BLENDER_RASTERIZER_PT_PEL;

		rise_blender_render_result res;
		std::memset( &res, 0, sizeof(res) );
		char err[512] = {0};
		const int ok = rise_blender_render_scene( &scene, &settings, 0, 0, 0, &res, err, sizeof(err) );
		Check( ok != 0 && res.rgba != 0, std::string( "ABI v20 ReliefModifier render ok: " ) + err );
		if( ok && res.rgba ) {
			const uint32_t cy = H / 2;
			const uint32_t lx = W / 4;
			const uint32_t rx = (3 * W) / 4;
			const float lumNeg = res.rgba[(cy * W + lx) * 4];
			const float lumPos = res.rgba[(cy * W + rx) * 4];
			Check( lumPos > lumNeg * 1.5f && lumPos > 0.05f,
				"RISE_BLENDER_MODIFIER_RELIEF tilts normal in Blinn elevation direction (pos=" +
				std::to_string( lumPos ) + " > neg=" + std::to_string( lumNeg ) + ")" );
			rise_blender_free_render_result( &res );
		}
	}
}

int main( int argc, char** argv )
{
	std::cout << "=== Blender bridge Alpha test (DL-193) ===" << std::endl;

	const bool bufferOnly = argc == 2 && std::strcmp(argv[1], "--buffer-only") == 0;
	const bool exportOnly = argc == 2 && std::strcmp(argv[1], "--export-failure-only") == 0;
	if( !exportOnly ) TestOutputBufferSizing();
	if( !bufferOnly ) TestExportCallbackFailure();
	if( !bufferOnly && !exportOnly ) {
		TestConstantAlphaPrecision();
		TestAbiVersionAndAlphaFields();
		TestOpaqueMaterialUnaffected();
		TestAlphaCutoutUnderPixelPel();
		TestAlphaCutoutUnderPT();
		TestAlphaCutoutUnderBDPT();
		TestFilteredAndSplatFilmTransparentAlpha();
		TestBridgeTransparentFilmRegionAndExpression();
		TestBridgeObjectInfoAndReliefModifier();
	}

	std::cout << "----------------------------------------" << std::endl;
	std::cout << "checks: " << g_checks << "   failures: " << g_failures << std::endl;
	if( g_failures > 0 ) {
		std::cout << "BlenderBridgeAlphaTest: FAILED" << std::endl;
		return 1;
	}
	std::cout << "BlenderBridgeAlphaTest: PASSED" << std::endl;
	return 0;
}
