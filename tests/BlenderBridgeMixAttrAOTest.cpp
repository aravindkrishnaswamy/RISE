//////////////////////////////////////////////////////////////////////
//
//  BlenderBridgeMixAttrAOTest.cpp - End-to-end ABI v21 contract test
//    for the Blender native bridge (`rise_blender_bridge.h/.cpp`):
//      1. ABI v21 version & append-only struct layout offsets
//      2. Named per-corner mesh attributes (`rise_blender_mesh_attribute`)
//         validation, attachment, and live `mesh_attr3`/`mesh_attr_alpha`
//      3. `RISE_BLENDER_MATERIAL_DIELECTRIC` spatially-varying `tau` via
//         registered IPainter (`PainterRGBScalarPainter` wrapper)
//      4. `RISE_BLENDER_MATERIAL_MIX` (`mix_children`, per-leaf modifier,
//         `mix_passthrough_painter_name`, `osl_ao` weight expression,
//         and error refusals on outer emission / empty / invalid weights /
//         containment-tracking child).
//
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "../src/Blender/native/rise_blender_bridge.cpp"

#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IGeometryManager.h"
#include "../src/Library/Interfaces/ITriangleMeshGeometry.h"
#include "../src/Library/Materials/MixMaterial.h"

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

static void CheckClose( const double a, const double b, const double tol, const std::string& what )
{
	const bool ok = std::isfinite( a ) && std::fabs( a - b ) <= tol;
	++g_checks;
	if( !ok ) {
		++g_failures;
		std::cout << "  FAIL: " << what << " (got " << a << ", expected " << b << " +/- " << tol << ")" << std::endl;
	}
}

namespace {

class JobHolder
{
public:
	JobHolder() : p( 0 ) { RISE::RISE_CreateJobPriv( &p ); }
	~JobHolder() { RISE::safe_release( p ); }
	RISE::IJobPriv& operator*() const { return *p; }
	bool Valid() const { return p != 0; }
private:
	JobHolder( const JobHolder& );
	JobHolder& operator=( const JobHolder& );
	RISE::IJobPriv* p;
};

struct QuadFixture
{
	float verts[12];
	float norms[12];
	float uvs[12];
	uint32_t vidx[6];
	uint32_t nidx[6];
	uint32_t uidx[6];

	explicit QuadFixture( const float z )
	{
		const float v[12] = {
			-2.0f, -2.0f, z,
			 2.0f, -2.0f, z,
			 2.0f,  2.0f, z,
			-2.0f,  2.0f, z
		};
		const float n[12] = {
			0,0,1, 0,0,1, 0,0,1, 0,0,1
		};
		const float u[12] = {
			0,0,0, 1,0,0, 1,1,0, 0,1,0
		};
		const uint32_t idx[6] = { 0,1,2, 0,2,3 };
		std::memcpy( verts, v, sizeof( v ) );
		std::memcpy( norms, n, sizeof( n ) );
		std::memcpy( uvs, u, sizeof( u ) );
		std::memcpy( vidx, idx, sizeof( idx ) );
		std::memcpy( nidx, idx, sizeof( idx ) );
		std::memcpy( uidx, idx, sizeof( idx ) );
	}

	rise_blender_mesh MakeMesh( const char* name ) const
	{
		rise_blender_mesh m;
		std::memset( &m, 0, sizeof( m ) );
		m.name = name;
		m.vertices = verts;
		m.normals = norms;
		m.uvs = uvs;
		m.vertex_indices = vidx;
		m.normal_indices = nidx;
		m.uv_indices = uidx;
		m.num_vertices = 4;
		m.num_normals = 4;
		m.num_uvs = 4;
		m.num_triangles = 2;
		m.double_sided = 1;
		m.use_face_normals = 0;
		return m;
	}
};

rise_blender_camera MakeCamera( const uint32_t w, const uint32_t h )
{
	rise_blender_camera cam;
	std::memset( &cam, 0, sizeof( cam ) );
	cam.projection_type = RISE_BLENDER_CAMERA_PERSPECTIVE;
	cam.location[0] = 0.0f; cam.location[1] = 0.0f; cam.location[2] = 3.0f;
	cam.forward[0]  = 0.0f; cam.forward[1]  = 0.0f; cam.forward[2]  = -1.0f;
	cam.up[0]       = 0.0f; cam.up[1]       = 1.0f; cam.up[2]       = 0.0f;
	cam.fov_y_radians = 0.9f;
	cam.width = w;
	cam.height = h;
	cam.pixel_aspect = 1.0f;
	cam.focal_length_mm = 50.0f;
	cam.sensor_width_mm = 36.0f;
	cam.sensor_height_mm = 24.0f;
	cam.aperture_ratio = 1.0f;
	cam.scene_unit_meters = 1.0f;
	return cam;
}

rise_blender_render_settings MakeSettings( const uint32_t w, const uint32_t h, const uint32_t spp )
{
	rise_blender_render_settings s;
	std::memset( &s, 0, sizeof( s ) );
	s.width = w;
	s.height = h;
	s.pixel_samples = spp;
	s.light_samples = 1;
	s.max_recursion = 4;
	s.use_path_tracing = 1;
	s.choose_one_light = 1;
	s.rasterizer_kind = RISE_BLENDER_RASTERIZER_PT_PEL;
	s.oidn_denoise = 0;
	s.use_zsobol = 1;
	s.pixel_filter = RISE_BLENDER_PIXEL_FILTER_BOX;
	s.pixel_filter_width = 1.0f;
	return s;
}

rise_blender_object MakeIdentityObject( const char* name, const char* geom, const char* mat )
{
	rise_blender_object o;
	std::memset( &o, 0, sizeof( o ) );
	o.name = name;
	o.geometry_name = geom;
	o.material_name = mat;
	o.transform[0] = 1.0f; o.transform[5] = 1.0f; o.transform[10] = 1.0f; o.transform[15] = 1.0f;
	o.casts_shadows = 1;
	o.receives_shadows = 1;
	o.visible = 1;
	o.object_color[0] = 1.0f; o.object_color[1] = 1.0f; o.object_color[2] = 1.0f; o.object_color[3] = 1.0f;
	return o;
}

void TestAbiVersionAndLayout()
{
	Check( RISE_BLENDER_API_VERSION == 23, "RISE_BLENDER_API_VERSION is 23" );
	Check( rise_blender_api_version() == 23, "rise_blender_api_version() == 23" );
	rise_blender_capabilities caps;
	std::memset( &caps, 0, sizeof( caps ) );
	Check( rise_blender_get_capabilities( &caps ) != 0 && caps.api_version == 23u,
		"rise_blender_get_capabilities reports api_version == 23" );
	Check( RISE_BLENDER_MATERIAL_MIX == 5, "RISE_BLENDER_MATERIAL_MIX == 5" );
	Check( offsetof( rise_blender_material, mix_children ) > offsetof( rise_blender_material, dispersion_abbe ),
		"mix_children is tail-appended after dispersion_abbe" );
	Check( offsetof( rise_blender_material, num_mix_children ) > offsetof( rise_blender_material, mix_children ),
		"num_mix_children follows mix_children" );
	Check( offsetof( rise_blender_material, mix_passthrough_painter_name ) > offsetof( rise_blender_material, num_mix_children ),
		"mix_passthrough_painter_name follows num_mix_children" );
	Check( offsetof( rise_blender_mesh, named_attributes ) > offsetof( rise_blender_mesh, tangent_is_shader_direction ),
		"named_attributes is tail-appended after tangent_is_shader_direction" );
	Check( offsetof( rise_blender_mesh, num_named_attributes ) > offsetof( rise_blender_mesh, named_attributes ),
		"num_named_attributes follows named_attributes" );
}

void TestNamedMeshAttributesAndValidation()
{
	JobHolder holder;
	Check( holder.Valid(), "create job" );
	if( !holder.Valid() ) return;

	QuadFixture q( 0.0f );
	char err[256] = {0};

	// Valid attribute attachment (2 triangles = 6 corners = 24 floats)
	const float cornerRGBA[24] = {
		1,0,0,0.25f,  0,1,0,0.50f,  0,0,1,0.75f,
		1,0,0,0.25f,  0,0,1,0.75f,  1,1,0,1.00f
	};
	rise_blender_mesh_attribute attr;
	attr.name = "distance";
	attr.rgba = cornerRGBA;
	attr.num_corners = 6;

	rise_blender_mesh m = q.MakeMesh( "m_attr_ok" );
	m.named_attributes = &attr;
	m.num_named_attributes = 1;
	Check( add_mesh( *holder, m, err, sizeof( err ) ), "add_mesh accepts valid named_attributes" );

	RISE::IGeometry* pGeom = (*holder).GetGeometries()->GetItem( "m_attr_ok" );
	RISE::ITriangleMeshGeometryIndexed5* pMesh5 = dynamic_cast<RISE::ITriangleMeshGeometryIndexed5*>( pGeom );
	Check( pMesh5 != 0, "geometry implements ITriangleMeshGeometryIndexed5" );
	if( pMesh5 ) {
		Check( pMesh5->numNamedCornerAttributes() == 1u, "named attribute 'distance' attached (count == 1)" );
	}

	// Invalid payloads must fail cleanly
	rise_blender_mesh badNull = q.MakeMesh( "m_bad_null" );
	badNull.named_attributes = 0;
	badNull.num_named_attributes = 1;
	Check( !add_mesh( *holder, badNull, err, sizeof( err ) ), "rejects null named_attributes array" );

	rise_blender_mesh_attribute badCount = attr;
	badCount.num_corners = 5;
	rise_blender_mesh mBadCount = q.MakeMesh( "m_bad_count" );
	mBadCount.named_attributes = &badCount;
	mBadCount.num_named_attributes = 1;
	Check( !add_mesh( *holder, mBadCount, err, sizeof( err ) ), "rejects mismatched num_corners" );

	rise_blender_mesh_attribute badEmpty = attr;
	badEmpty.name = "";
	rise_blender_mesh mBadEmpty = q.MakeMesh( "m_bad_empty" );
	mBadEmpty.named_attributes = &badEmpty;
	mBadEmpty.num_named_attributes = 1;
	Check( !add_mesh( *holder, mBadEmpty, err, sizeof( err ) ), "rejects empty attribute name" );

	// R1 A-P2-1: duplicate attribute names rejected
	rise_blender_mesh_attribute dupAttrs[2] = { attr, attr };
	rise_blender_mesh mDup = q.MakeMesh( "m_dup_attr" );
	mDup.named_attributes = dupAttrs;
	mDup.num_named_attributes = 2;
	Check( !add_mesh( *holder, mDup, err, sizeof( err ) ), "R1 A-P2-1: rejects duplicate attribute names" );
	Check( (*holder).GetGeometries()->GetItem( "m_dup_attr" ) == 0, "R1 A-P1-5: failed mesh not orphaned in manager" );

	// R1 A-P1-5 / R2 P2-1: num_triangles * 3 overflow rejected cleanly
	rise_blender_mesh mOverflow = q.MakeMesh( "m_tri_overflow" );
	mOverflow.num_triangles = std::numeric_limits<uint32_t>::max() / 3u + 1u;
	Check( !add_mesh( *holder, mOverflow, err, sizeof( err ) ), "R1 A-P1-5: rejects num_triangles * 3 overflow" );
}

void TestMixMaterialValidationAndRefusals()
{
	JobHolder holder;
	Check( holder.Valid(), "create job" );
	if( !holder.Valid() ) return;
	char err[256] = {0};

	// Register painters, a modifier, Lambert leaf, Dielectric leaf
	rise_blender_painter pWhite;
	std::memset( &pWhite, 0, sizeof( pWhite ) );
	pWhite.name = "p_white";
	pWhite.kind = RISE_BLENDER_PAINTER_UNIFORM;
	pWhite.color[0] = 0.8f; pWhite.color[1] = 0.8f; pWhite.color[2] = 0.8f;
	pWhite.color_space = RISE_BLENDER_COLOR_LINEAR;
	Check( add_painter( *holder, pWhite, err, sizeof( err ) ), "add uniform painter" );

	rise_blender_painter pAO;
	std::memset( &pAO, 0, sizeof( pAO ) );
	pAO.name = "p_ao_w";
	pAO.kind = RISE_BLENDER_PAINTER_EXPRESSION;
	pAO.expression = "osl_ao(0.2, 4, vec3(0, 0, 0), 1)";
	Check( add_painter( *holder, pAO, err, sizeof( err ) ), "add osl_ao expression painter" );

	rise_blender_modifier modRelief;
	std::memset( &modRelief, 0, sizeof( modRelief ) );
	modRelief.name = "mod_r";
	modRelief.kind = RISE_BLENDER_MODIFIER_RELIEF;
	modRelief.source_painter_name = "p_ao_w";
	modRelief.scale = 0.1f;
	modRelief.window = 0.0f;
	Check( add_modifier( *holder, modRelief, err, sizeof( err ) ), "add relief modifier" );

	rise_blender_material mLamb;
	std::memset( &mLamb, 0, sizeof( mLamb ) );
	mLamb.name = "m_lamb";
	mLamb.model = RISE_BLENDER_MATERIAL_LAMBERT;
	mLamb.diffuse_painter_name = "p_white";
	Check( add_material( *holder, mLamb, err, sizeof( err ) ), "add lambert leaf" );

	rise_blender_material mDiel;
	std::memset( &mDiel, 0, sizeof( mDiel ) );
	mDiel.name = "m_diel";
	mDiel.model = RISE_BLENDER_MATERIAL_DIELECTRIC;
	mDiel.tau_painter_name = "0.9 0.9 0.9";
	mDiel.ior_painter_name = "1.33";
	mDiel.scatter_painter_name = "100000";
	Check( add_material( *holder, mDiel, err, sizeof( err ) ), "add dielectric material" );

	// Valid mix with expression weight + absorber + per-leaf modifier + passthrough
	rise_blender_mix_child goodChildren[2];
	goodChildren[0].material_name = "m_lamb";
	goodChildren[0].weight_painter_name = "p_ao_w";
	goodChildren[0].modifier_name = "mod_r";
	goodChildren[1].material_name = 0; // absorber
	goodChildren[1].weight_painter_name = "0.25";
	goodChildren[1].modifier_name = 0;

	rise_blender_material mMix;
	std::memset( &mMix, 0, sizeof( mMix ) );
	mMix.name = "m_mix_ok";
	mMix.model = RISE_BLENDER_MATERIAL_MIX;
	mMix.mix_children = goodChildren;
	mMix.num_mix_children = 2;
	mMix.mix_passthrough_painter_name = "0.1";
	Check( add_material( *holder, mMix, err, sizeof( err ) ), "add_material accepts valid RISE_BLENDER_MATERIAL_MIX" );
	RISE::IMaterial* pReg = (*holder).GetMaterials()->GetItem( "m_mix_ok" );
	Check( pReg != 0 && pReg->IsUnresolvedMix(), "registered material is MixMaterial" );

	// Refusals: outer emission, empty children, unknown weight, dielectric child
	rise_blender_material badEmit = mMix;
	badEmit.name = "m_mix_bad_emit";
	badEmit.emission_painter_name = "p_white";
	Check( !add_material( *holder, badEmit, err, sizeof( err ) ), "refuses outer emission on MIX" );

	rise_blender_material badEmpty = mMix;
	badEmpty.name = "m_mix_bad_empty";
	badEmpty.num_mix_children = 0;
	badEmpty.mix_children = 0;
	Check( !add_material( *holder, badEmpty, err, sizeof( err ) ), "refuses empty MIX" );

	rise_blender_mix_child badWeightChild = goodChildren[0];
	badWeightChild.weight_painter_name = "not_a_painter_or_number";
	rise_blender_material badW = mMix;
	badW.name = "m_mix_bad_weight";
	badW.mix_children = &badWeightChild;
	badW.num_mix_children = 1;
	Check( !add_material( *holder, badW, err, sizeof( err ) ), "refuses unknown mix weight name" );

	rise_blender_mix_child badDielChild = { "m_diel", "1.0", 0 };
	rise_blender_material badDiel = mMix;
	badDiel.name = "m_mix_bad_diel";
	badDiel.mix_children = &badDielChild;
	badDiel.num_mix_children = 1;
	badDiel.mix_passthrough_painter_name = 0;
	Check( !add_material( *holder, badDiel, err, sizeof( err ) ), "refuses containment-tracking dielectric inside MIX" );
	Check( std::string( err ).find( ":" ) != std::string::npos, "R1 A-P2-2: propagates MixMaterial finalize diagnostic" );

	// R1 A-P1-1: "none" sentinel must NEVER resolve to Job's black IPainter in tau or mix weight!
	rise_blender_material badDielNone = mDiel;
	badDielNone.name = "m_diel_none_tau";
	badDielNone.tau_painter_name = "none";
	Check( !add_material( *holder, badDielNone, err, sizeof( err ) ), "R1 A-P1-1: dielectric refuses tau='none'" );

	rise_blender_mix_child badNoneWeight = { "m_lamb", "none", 0 };
	rise_blender_material badMixNone = mMix;
	badMixNone.name = "m_mix_none_w";
	badMixNone.mix_children = &badNoneWeight;
	badMixNone.num_mix_children = 1;
	badMixNone.mix_passthrough_painter_name = 0;
	Check( !add_material( *holder, badMixNone, err, sizeof( err ) ), "R1 A-P1-1: mix refuses weight='none'" );
}

void TestEndToEndDielectricNamedAttrRender()
{
	// Render a closed dielectric slab of thickness 1.0 (front face z=0, back face z=-1;
	// IOR = 1.0001 so rays pass straight through with ~0 Fresnel).  DielectricSPF applies
	// Beer's-law `pow(tau, distance)` on EXIT (`bFromInside == true`, distance = 1.0),
	// so `tau_painter_name = p_tint_expr` (`mesh_attr3("tint")`) transmits RED on the
	// bottom-right half (x > y) and BLUE on the top-left half (x < y) against white env!
	const uint32_t W = 24, H = 24;
	rise_blender_camera cam = MakeCamera( W, H );
	rise_blender_render_settings settings = MakeSettings( W, H, 8 );

	rise_blender_painter painters[2];
	std::memset( painters, 0, sizeof( painters ) );
	painters[0].name = "p_env";
	painters[0].kind = RISE_BLENDER_PAINTER_UNIFORM;
	painters[0].color[0] = 1.0f; painters[0].color[1] = 1.0f; painters[0].color[2] = 1.0f;
	painters[0].color_space = RISE_BLENDER_COLOR_LINEAR;

	painters[1].name = "p_tint_expr";
	painters[1].kind = RISE_BLENDER_PAINTER_EXPRESSION;
	painters[1].expression = "mesh_attr3(\"tint\")";

	rise_blender_material mat;
	std::memset( &mat, 0, sizeof( mat ) );
	mat.name = "m_water_attr";
	mat.model = RISE_BLENDER_MATERIAL_DIELECTRIC;
	mat.tau_painter_name = "p_tint_expr";
	mat.ior_painter_name = "1.0001";
	mat.scatter_painter_name = "1000000";
	mat.double_sided = 1;

	// Closed axis-aligned box [-2, 2] x [-2, 2] x [-1, 0] (12 triangles, 36 corners).
	// Outward normals; front (+Z at z=0) and back (-Z at z=-1) split along diagonal x=y
	// so bottom-right (+x, -y) carries RED (0.9, 0.1, 0.1) and top-left (-x, +y) carries BLUE (0.1, 0.1, 0.9).
	const float verts[24] = {
		-2.0f, -2.0f,  0.0f,   2.0f, -2.0f,  0.0f,   2.0f,  2.0f,  0.0f,  -2.0f,  2.0f,  0.0f, // 0..3 front z=0
		-2.0f, -2.0f, -1.0f,   2.0f, -2.0f, -1.0f,   2.0f,  2.0f, -1.0f,  -2.0f,  2.0f, -1.0f  // 4..7 back  z=-1
	};
	const float norms[18] = {
		 0, 0, 1,   0, 0,-1,   1, 0, 0,  -1, 0, 0,   0, 1, 0,   0,-1, 0
	};
	const float uvs[12] = { 0,0,0, 1,0,0, 1,1,0, 0,1,0 };
	const uint32_t vidx[36] = {
		// Front (+Z): tri0 BR (0,1,2), tri1 TL (0,2,3)
		0,1,2,  0,2,3,
		// Back (-Z): tri2 BR (4,6,5), tri3 TL (4,7,6)
		4,6,5,  4,7,6,
		// +X, -X, +Y, -Y sides to close the volume cleanly
		1,5,6,  1,6,2,
		4,0,3,  4,3,7,
		3,2,6,  3,6,7,
		4,5,1,  4,1,0
	};
	const uint32_t nidx[36] = {
		0,0,0, 0,0,0,
		1,1,1, 1,1,1,
		2,2,2, 2,2,2,
		3,3,3, 3,3,3,
		4,4,4, 4,4,4,
		5,5,5, 5,5,5
	};
	const uint32_t uidx[36] = {
		0,1,2, 0,2,3,  0,2,1, 0,3,2,
		0,1,2, 0,2,3,  0,1,2, 0,2,3,
		0,1,2, 0,2,3,  0,1,2, 0,2,3
	};
	float tintRGBA[36 * 4];
	for( int t = 0; t < 12; ++t ) {
		const bool isBlue = ( t == 1 || t == 3 );
		for( int k = 0; k < 3; ++k ) {
			const int base = ( t * 3 + k ) * 4;
			tintRGBA[base + 0] = isBlue ? 0.1f : 0.9f;
			tintRGBA[base + 1] = 0.1f;
			tintRGBA[base + 2] = isBlue ? 0.9f : 0.1f;
			tintRGBA[base + 3] = 1.0f;
		}
	}
	rise_blender_mesh_attribute attr = { "tint", tintRGBA, 36 };
	rise_blender_mesh mesh;
	std::memset( &mesh, 0, sizeof( mesh ) );
	mesh.name = "mesh_water";
	mesh.vertices = verts;
	mesh.normals = norms;
	mesh.uvs = uvs;
	mesh.vertex_indices = vidx;
	mesh.normal_indices = nidx;
	mesh.uv_indices = uidx;
	mesh.num_vertices = 8;
	mesh.num_normals = 6;
	mesh.num_uvs = 4;
	mesh.num_triangles = 12;
	mesh.double_sided = 1;
	mesh.use_face_normals = 0;
	mesh.named_attributes = &attr;
	mesh.num_named_attributes = 1;

	rise_blender_object obj = MakeIdentityObject( "obj_water", "mesh_water", "m_water_attr" );

	rise_blender_scene scene;
	std::memset( &scene, 0, sizeof( scene ) );
	scene.camera = &cam;
	scene.painters = painters;
	scene.num_painters = 2;
	scene.materials = &mat;
	scene.num_materials = 1;
	scene.meshes = &mesh;
	scene.num_meshes = 1;
	scene.objects = &obj;
	scene.num_objects = 1;
	scene.world_radiance_painter_name = "p_env";
	scene.world_radiance_scale = 1.0f;
	scene.world_radiance_is_background = 1;

	rise_blender_render_result res;
	std::memset( &res, 0, sizeof( res ) );
	char err[512] = {0};
	const int ok = rise_blender_render_scene( &scene, &settings, 0, 0, 0, &res, err, sizeof( err ) );
	Check( ok != 0, std::string( "dielectric named-attr render succeeded: " ) + err );
	if( ok && res.rgba ) {
		// RISE output image is top-down: (x=18, y=18) is bottom-right (+x, -y -> Tri 0 -> RED);
		// (x=5, y=5) is top-left (-x, +y -> Tri 1 -> BLUE).
		const size_t idxBR = ( size_t( 18 ) * W + 18 ) * 4;
		const size_t idxTL = ( size_t( 5 ) * W + 5 ) * 4;
		const float brR = res.rgba[idxBR + 0], brB = res.rgba[idxBR + 2];
		const float tlR = res.rgba[idxTL + 0], tlB = res.rgba[idxTL + 2];
		Check( brR > 0.6f && brR > brB * 3.0f, "bottom-right triangle transmits spatially-varying RED tau" );
		Check( tlB > 0.6f && tlB > tlR * 3.0f, "top-left triangle transmits spatially-varying BLUE tau" );
	}
	rise_blender_free_render_result( &res );
}

void TestEndToEndMixPassthroughRender()
{
	// Front quad: 0.5 Red(0.8,0,0) + 0.5 passthrough; Back quad: emissive Green (0, 0.8, 0) * pi -> radiance 0.8 G.
	// Uniform white env -> front reflects 0.5 * 0.8 = 0.40 R, back seen 50% -> 0.40 G!
	const uint32_t W = 24, H = 24;
	rise_blender_camera cam = MakeCamera( W, H );
	rise_blender_render_settings settings = MakeSettings( W, H, 64 );

	rise_blender_painter painters[5];
	std::memset( painters, 0, sizeof( painters ) );
	painters[0].name = "p_env";
	painters[0].kind = RISE_BLENDER_PAINTER_UNIFORM;
	painters[0].color[0] = 1.0f; painters[0].color[1] = 1.0f; painters[0].color[2] = 1.0f;
	painters[0].color_space = RISE_BLENDER_COLOR_LINEAR;

	painters[1].name = "p_red";
	painters[1].kind = RISE_BLENDER_PAINTER_UNIFORM;
	painters[1].color[0] = 0.8f; painters[1].color[1] = 0.0f; painters[1].color[2] = 0.0f;
	painters[1].color_space = RISE_BLENDER_COLOR_LINEAR;

	painters[2].name = "p_black";
	painters[2].kind = RISE_BLENDER_PAINTER_UNIFORM;
	painters[2].color_space = RISE_BLENDER_COLOR_LINEAR;

	painters[3].name = "p_emit_green";
	painters[3].kind = RISE_BLENDER_PAINTER_UNIFORM;
	painters[3].color[0] = 0.0f; painters[3].color[1] = 0.8f * 3.14159265f; painters[3].color[2] = 0.0f;
	painters[3].color_space = RISE_BLENDER_COLOR_LINEAR;

	painters[4].name = "p_half_expr";
	painters[4].kind = RISE_BLENDER_PAINTER_EXPRESSION;
	painters[4].expression = "0.5";

	rise_blender_mix_child child = { "m_red", "p_half_expr", 0 };

	rise_blender_material mats[3];
	std::memset( mats, 0, sizeof( mats ) );
	mats[0].name = "m_red";
	mats[0].model = RISE_BLENDER_MATERIAL_LAMBERT;
	mats[0].diffuse_painter_name = "p_red";
	mats[0].double_sided = 1;

	mats[1].name = "m_mix_front";
	mats[1].model = RISE_BLENDER_MATERIAL_MIX;
	mats[1].mix_children = &child;
	mats[1].num_mix_children = 1;
	mats[1].mix_passthrough_painter_name = "p_half_expr";
	mats[1].double_sided = 1;

	mats[2].name = "m_back_green";
	mats[2].model = RISE_BLENDER_MATERIAL_LAMBERT;
	mats[2].diffuse_painter_name = "p_black";
	mats[2].emission_painter_name = "p_emit_green";
	mats[2].double_sided = 1;

	QuadFixture qFront( 0.0f );
	QuadFixture qBack( -0.5f );
	rise_blender_mesh meshes[2] = { qFront.MakeMesh( "g_front" ), qBack.MakeMesh( "g_back" ) };
	rise_blender_object objs[2] = {
		MakeIdentityObject( "o_front", "g_front", "m_mix_front" ),
		MakeIdentityObject( "o_back", "g_back", "m_back_green" )
	};

	rise_blender_scene scene;
	std::memset( &scene, 0, sizeof( scene ) );
	scene.camera = &cam;
	scene.painters = painters;
	scene.num_painters = 5;
	scene.materials = mats;
	scene.num_materials = 3;
	scene.meshes = meshes;
	scene.num_meshes = 2;
	scene.objects = objs;
	scene.num_objects = 2;
	scene.world_radiance_painter_name = "p_env";
	scene.world_radiance_scale = 1.0f;
	scene.world_radiance_is_background = 1;

	rise_blender_render_result res;
	std::memset( &res, 0, sizeof( res ) );
	char err[512] = {0};
	const int ok = rise_blender_render_scene( &scene, &settings, 0, 0, 0, &res, err, sizeof( err ) );
	Check( ok != 0, std::string( "mix passthrough render succeeded: " ) + err );
	if( ok && res.rgba ) {
		double sumR = 0, sumG = 0;
		int count = 0;
		for( uint32_t y = 8; y < 16; ++y ) {
			for( uint32_t x = 8; x < 16; ++x ) {
				const size_t idx = ( size_t( y ) * W + x ) * 4;
				sumR += res.rgba[idx + 0];
				sumG += res.rgba[idx + 1];
				++count;
			}
		}
		CheckClose( sumR / count, 0.40, 0.05, "MIX front 0.5 Red reflects 0.40" );
		CheckClose( sumG / count, 0.40, 0.05, "MIX 0.5 passthrough reveals 0.40 Green background" );
	}
	rise_blender_free_render_result( &res );
}

// -----------------------------------------------------------------------------
// 6. ABI v22 progress_info_callback (RenderETAEstimator + stage titles + cancel)
// -----------------------------------------------------------------------------
void TestProgressInfoAndETA()
{
	struct ProgressRecord {
		float progress;
		double elapsed;
		double remaining;
		int has_remaining;
		std::string title;
	};
	struct Collector {
		std::vector<ProgressRecord> records;
		bool cancel_on_building = false;
		bool cancel_on_first_raster = false;
		bool cancel_on_mlt_progress = false;
		int delay_ms_per_raster_tick = 0;
		int delayed_ticks = 0;
	};

	auto on_progress_info = []( void* ud, const rise_blender_progress_info* info ) -> int {
		if( !ud || !info ) return 1;
		Collector* col = static_cast<Collector*>( ud );
		ProgressRecord rec;
		rec.progress = info->progress;
		rec.elapsed = info->elapsed_seconds;
		rec.remaining = info->remaining_seconds;
		rec.has_remaining = info->has_remaining;
		rec.title = info->title ? info->title : "";
		col->records.push_back( rec );
		if( col->cancel_on_building && rec.title == "Building scene" ) {
			return 0;
		}
		if( col->cancel_on_first_raster && rec.title == "Rasterizing Scene" && rec.progress >= 0.0f ) {
			return 0;
		}
		if( col->cancel_on_mlt_progress && rec.title.find( "MLT" ) != std::string::npos && rec.progress >= 0.0f ) {
			return 0;
		}
		if( col->delay_ms_per_raster_tick > 0 && rec.title == "Rasterizing Scene" &&
			rec.progress >= 0.0f && rec.progress < 0.9f && col->delayed_ticks < 10 ) {
			++col->delayed_ticks;
			std::this_thread::sleep_for( std::chrono::milliseconds( col->delay_ms_per_raster_tick ) );
		}
		return 1;
	};

	// (a) Direct ProgressBridge edge-input contract (NaN / Inf / total <= 0 / out-of-order / post-cancel SetTitle).
	{
		Collector col;
		ProgressBridge pb( 0, on_progress_info, &col );
		pb.SetTitle( "  Rasterizing Scene:  " );
		Check( !col.records.empty() && col.records.back().title == "Rasterizing Scene",
			"ProgressBridge trims leading/trailing whitespace and colon" );
		Check( pb.Progress( 5.0, 10.0 ), "Progress(5, 10) continues" );
		CheckClose( col.records.back().progress, 0.5, 1e-5, "Progress(5, 10) -> 0.5" );
		// total <= 0 and NaN / Inf must not reset pMax or emit 0%
		const size_t before_bad = col.records.size();
		Check( pb.Progress( 0.0, 0.0 ), "Progress(0, 0) ignored safely" );
		Check( pb.Progress( 1.0, -5.0 ), "Progress(1, -5) ignored safely" );
		Check( pb.Progress( std::numeric_limits<double>::quiet_NaN(), 10.0 ), "NaN progress ignored safely" );
		Check( pb.Progress( std::numeric_limits<double>::infinity(), 10.0 ), "Inf progress ignored safely" );
		Check( col.records.size() == before_bad, "degenerate progress inputs do not emit bogus callbacks" );
		// Small backward tile reordering keeps pMax at 0.5
		Check( pb.Progress( 4.5, 10.0 ), "out-of-order backward tile continues" );
		CheckClose( col.records.back().progress, 0.5, 1e-5, "out-of-order tile preserves pMax == 0.5" );
		// Repeated identical title does not reset progress/ETA
		const size_t before_dup_title = col.records.size();
		pb.SetTitle( "Rasterizing Scene: " );
		Check( col.records.size() == before_dup_title, "duplicate SetTitle is a no-op" );
		// Cancel then verify post-cancel SetTitle is suppressed (P2-1)
		col.cancel_on_first_raster = true;
		Check( !pb.Progress( 6.0, 10.0 ), "Progress returns false when callback cancels" );
		Check( pb.IsCancelled() && pb.Aborted(), "ProgressBridge marks cancelled" );
		const size_t after_cancel = col.records.size();
		pb.SetTitle( "Denoising: " );
		Check( col.records.size() == after_cancel, "post-cancel SetTitle does not emit Denoising" );
	}

	auto make_uniform = []( const char* name, float r, float g, float b ) {
		rise_blender_painter p;
		std::memset( &p, 0, sizeof( p ) );
		p.name = name;
		p.kind = RISE_BLENDER_PAINTER_UNIFORM;
		p.color[0] = r; p.color[1] = g; p.color[2] = b;
		return p;
	};

	const uint32_t W = 32, H = 32;
	rise_blender_camera cam = MakeCamera( W, H );
	rise_blender_render_settings settings = MakeSettings( W, H, 4 );
	settings.progressive_enabled = 1;
	settings.progressive_samples_per_pass = 1;
	rise_blender_capabilities caps;
	std::memset( &caps, 0, sizeof( caps ) );
	rise_blender_get_capabilities( &caps );
	settings.oidn_denoise = caps.supports_oidn ? 1 : 0;

	rise_blender_painter painters[2] = {
		make_uniform( "p_env", 1.0f, 1.0f, 1.0f ),
		make_uniform( "p_gray", 0.5f, 0.5f, 0.5f )
	};

	rise_blender_material mat;
	std::memset( &mat, 0, sizeof( mat ) );
	mat.name = "m_gray";
	mat.model = RISE_BLENDER_MATERIAL_LAMBERT;
	mat.diffuse_painter_name = "p_gray";

	QuadFixture quad( 0.0f );
	rise_blender_mesh mesh = quad.MakeMesh( "g_quad" );
	rise_blender_object obj = MakeIdentityObject( "o_quad", "g_quad", "m_gray" );

	rise_blender_scene scene;
	std::memset( &scene, 0, sizeof( scene ) );
	scene.camera = &cam;
	scene.painters = painters;
	scene.num_painters = 2;
	scene.materials = &mat;
	scene.num_materials = 1;
	scene.meshes = &mesh;
	scene.num_meshes = 1;
	scene.objects = &obj;
	scene.num_objects = 1;
	scene.world_radiance_painter_name = "p_env";
	scene.world_radiance_scale = 1.0f;
	scene.world_radiance_is_background = 1;

	// (b) Progressive multi-pass render with simulated tile latency crossing RenderETAEstimator's
	//     2.0 s warmup so we exercise has_remaining == 1 end-to-end, plus Denoising stage.
	{
		Collector col;
		col.delay_ms_per_raster_tick = 260;
		rise_blender_render_settings s_eta = settings;
		s_eta.pixel_samples = 16;
		s_eta.progressive_samples_per_pass = 1;
		rise_blender_render_result res;
		std::memset( &res, 0, sizeof( res ) );
		char err[512] = {0};
		const int ok = rise_blender_render_scene_ex(
			&scene, &s_eta, 0, on_progress_info, 0, &col, &res, err, sizeof( err ) );
		Check( ok != 0, std::string( "ABI v22 render_scene_ex succeeded: " ) + err );
		Check( !col.records.empty(), "ABI v22 emitted progress_info events" );
		if( !col.records.empty() ) {
			Check( col.records.front().title == "Building scene",
				"first progress_info stage is 'Building scene'" );
			Check( col.records.front().progress < 0.0f && col.records.front().has_remaining == 0,
				"Building scene stage is indeterminate with has_remaining == 0" );
		}

		int raster_title_resets = 0;
		int raster_progress_ticks = 0;
		int valid_eta_ticks = 0;
		float prev_raster_prog = -1.0f;
		bool raster_monotonic = true;
		bool all_finite_nonneg_elapsed = true;
		bool any_trailing_colon = false;
		bool saw_denoising = false;

		for( size_t i = 0; i < col.records.size(); ++i ) {
			const ProgressRecord& r = col.records[i];
			if( !r.title.empty() && ( r.title.back() == ':' || r.title.back() == ' ' ) ) {
				any_trailing_colon = true;
			}
			if( !std::isfinite( r.elapsed ) || r.elapsed < 0.0 ) {
				all_finite_nonneg_elapsed = false;
			}
			if( r.has_remaining ) {
				if( !std::isfinite( r.remaining ) || r.remaining < 0.0 ) {
					all_finite_nonneg_elapsed = false;
				}
				if( r.title == "Rasterizing Scene" && r.progress < 1.0f && r.remaining > 0.0 ) {
					++valid_eta_ticks;
				}
			}
			if( r.title == "Rasterizing Scene" ) {
				if( r.progress < 0.0f ) {
					++raster_title_resets;
				} else {
					++raster_progress_ticks;
					if( r.progress < prev_raster_prog - 1e-6f || r.progress > 1.0f + 1e-6f ) {
						raster_monotonic = false;
					}
					prev_raster_prog = r.progress;
				}
			} else if( r.title == "Denoising" ) {
				saw_denoising = true;
				Check( r.progress < 0.0f && r.has_remaining == 0,
					"Denoising stage clears ETA (indeterminate)" );
			}
		}

		Check( !any_trailing_colon, "progress_info titles strip trailing colon/whitespace" );
		Check( all_finite_nonneg_elapsed, "elapsed and remaining seconds are finite and non-negative" );
		Check( raster_title_resets == 1,
			"progressive multi-pass emits Rasterizing Scene title reset exactly once" );
		Check( raster_progress_ticks > 0, "Rasterizing Scene emitted progress ticks" );
		Check( valid_eta_ticks > 0, "RenderETAEstimator warmed up and reported positive remaining_seconds" );
		Check( raster_monotonic, "Rasterizing Scene progress is monotonic in [0, 1]" );
		if( caps.supports_oidn ) {
			Check( saw_denoising, "OIDN-enabled render emitted Denoising stage title" );
		}
		rise_blender_free_render_result( &res );
	}

	// (c) Cancel during Building scene aborts before rasterization starts.
	{
		Collector col;
		col.cancel_on_building = true;
		rise_blender_render_result res;
		std::memset( &res, 0, sizeof( res ) );
		char err[512] = {0};
		const int ok = rise_blender_render_scene_ex(
			&scene, &settings, 0, on_progress_info, 0, &col, &res, err, sizeof( err ) );
		Check( ok == 0, "cancelling on Building scene aborts cleanly" );
		Check( std::string( err ).find( "canceled" ) != std::string::npos,
			"Building scene cancel reports 'canceled'" );
		Check( col.records.size() == 1 && col.records[0].title == "Building scene",
			"Building scene cancel does not proceed to Rasterizing Scene" );
		rise_blender_free_render_result( &res );
	}

	// (d) Cancel during OIDN-enabled rasterization suppresses post-cancel Denoising title (P2-1).
	{
		Collector col;
		col.cancel_on_first_raster = true;
		rise_blender_render_result res;
		std::memset( &res, 0, sizeof( res ) );
		char err[512] = {0};
		const int ok = rise_blender_render_scene_ex(
			&scene, &settings, 0, on_progress_info, 0, &col, &res, err, sizeof( err ) );
		Check( ok == 0, "cancelling OIDN-enabled render returns 0" );
		Check( std::string( err ).find( "canceled" ) != std::string::npos,
			"canceled OIDN-enabled render reports 'canceled'" );
		bool saw_denoise_after_cancel = false;
		for( size_t i = 0; i < col.records.size(); ++i ) {
			if( col.records[i].title == "Denoising" ) saw_denoise_after_cancel = true;
		}
		Check( !saw_denoise_after_cancel, "canceled OIDN render does not emit Denoising stage title" );
		rise_blender_free_render_result( &res );
	}

	// (e) Cancel during MLT returns canceled (red-proves P1-2: MLT LastRenderCompleted() == true).
	{
		Collector col;
		col.cancel_on_mlt_progress = true;
		rise_blender_render_settings s_mlt = MakeSettings( W, H, 4 );
		s_mlt.rasterizer_kind = RISE_BLENDER_RASTERIZER_MLT_PEL;
		s_mlt.mlt_bootstrap = 500;
		s_mlt.mlt_chains = 2;
		s_mlt.mlt_mutations_per_pixel = 4;
		s_mlt.oidn_denoise = 0;
		rise_blender_render_result res;
		std::memset( &res, 0, sizeof( res ) );
		char err[512] = {0};
		const int ok = rise_blender_render_scene_ex(
			&scene, &s_mlt, 0, on_progress_info, 0, &col, &res, err, sizeof( err ) );
		Check( ok == 0, "cancelling MLT render returns 0 (P1-2)" );
		Check( std::string( err ).find( "canceled" ) != std::string::npos,
			"canceled MLT render reports 'canceled' rather than success or missing buffer (P1-2)" );
		rise_blender_free_render_result( &res );
	}

	// (f) Legacy rise_blender_render_scene with legacy progress_callback continues to work.
	{
		struct LegacyCol {
			int title_calls = 0;
			int prog_calls = 0;
		} lcol;
		auto legacy_cb = []( void* ud, float prog, const char* title ) -> int {
			LegacyCol* lc = static_cast<LegacyCol*>( ud );
			if( title && title[0] ) ++lc->title_calls;
			if( prog >= 0.0f ) ++lc->prog_calls;
			return 1;
		};
		rise_blender_render_settings s_legacy = MakeSettings( W, H, 4 );
		s_legacy.progressive_enabled = 1;
		s_legacy.progressive_samples_per_pass = 1;
		rise_blender_render_result res;
		std::memset( &res, 0, sizeof( res ) );
		char err[512] = {0};
		const int ok = rise_blender_render_scene(
			&scene, &s_legacy, legacy_cb, 0, &lcol, &res, err, sizeof( err ) );
		Check( ok != 0, std::string( "legacy rise_blender_render_scene succeeded: " ) + err );
		Check( lcol.title_calls > 0 && lcol.prog_calls > 0,
			"legacy progress_callback receives both title and progress events" );
		rise_blender_free_render_result( &res );
	}

	// (g) Non-progressive PT transitions from "Building scene" to "Rasterizing Scene"
	//     before emitting progress ticks (Round 2 P2-1).
	{
		Collector col;
		rise_blender_render_settings s_nonprog = MakeSettings( W, H, 2 );
		s_nonprog.progressive_enabled = 0;
		s_nonprog.oidn_denoise = 0;
		rise_blender_render_result res;
		std::memset( &res, 0, sizeof( res ) );
		char err[512] = {0};
		const int ok = rise_blender_render_scene_ex(
			&scene, &s_nonprog, 0, on_progress_info, 0, &col, &res, err, sizeof( err ) );
		Check( ok != 0, std::string( "non-progressive render_scene_ex succeeded: " ) + err );
		bool saw_raster_title = false;
		bool any_building_progress = false;
		for( size_t i = 0; i < col.records.size(); ++i ) {
			if( col.records[i].title == "Rasterizing Scene" ) {
				saw_raster_title = true;
			}
			if( col.records[i].title == "Building scene" && col.records[i].progress >= 0.0f ) {
				any_building_progress = true;
			}
		}
		Check( saw_raster_title, "non-progressive PT transitions to 'Rasterizing Scene'" );
		Check( !any_building_progress, "non-progressive PT never reports raster progress under 'Building scene'" );
		rise_blender_free_render_result( &res );
	}
}

void TestWrapModeAndExpressionAlpha()
{
	// Verify ABI v23 wrap_mode validation (0=Clamp, 1=Repeat, 2=Mirror, invalid clamps to 0)
	// and scalar ExpressionPainter alpha coverage (reads channel 0 instead of CHAN_A=1.0).
	const uint32_t W = 24, H = 24;
	rise_blender_camera cam = MakeCamera( W, H );
	rise_blender_render_settings settings = MakeSettings( W, H, 4 );
	settings.progressive_enabled = 0;
	settings.oidn_denoise = 0;

	rise_blender_painter painters[3];
	std::memset( painters, 0, sizeof( painters ) );
	painters[0].name = "p_env";
	painters[0].kind = RISE_BLENDER_PAINTER_UNIFORM;
	painters[0].color[0] = 0.0f; painters[0].color[1] = 0.0f; painters[0].color[2] = 1.0f; // blue sky
	painters[0].color_space = RISE_BLENDER_COLOR_LINEAR;

	painters[1].name = "p_red";
	painters[1].kind = RISE_BLENDER_PAINTER_UNIFORM;
	painters[1].color[0] = 3.14159f; painters[1].color[1] = 0.0f; painters[1].color[2] = 0.0f;
	painters[1].color_space = RISE_BLENDER_COLOR_LINEAR;

	// Scalar expression alpha: left half (u < 0.5) opaque (1.0), right half transparent (0.0)
	painters[2].name = "p_alpha_expr";
	painters[2].kind = RISE_BLENDER_PAINTER_EXPRESSION;
	painters[2].expression = "select(u < 0.5, 1, 0)";
	painters[2].wrap_mode = 99; // out-of-range wrap_mode must not crash or fail

	rise_blender_material mat;
	std::memset( &mat, 0, sizeof( mat ) );
	mat.name = "m_expr_alpha";
	mat.model = RISE_BLENDER_MATERIAL_PBR_METALLIC_ROUGHNESS;
	mat.base_color_painter_name = "p_red";
	mat.metallic_painter_name = "0.0";
	mat.roughness_painter_name = "1.0";
	mat.emission_painter_name = "p_red";
	mat.double_sided = 1;
	mat.alpha_mode = RISE_BLENDER_ALPHA_CLIP;
	mat.alpha = 1.0f;
	mat.alpha_threshold = 0.5f;
	mat.alpha_texture_painter_name = "p_alpha_expr";

	const float verts[12] = { -2.0f,-2.0f,0.0f, 2.0f,-2.0f,0.0f, 2.0f,2.0f,0.0f, -2.0f,2.0f,0.0f };
	const float norms[3]  = { 0.0f, 0.0f, 1.0f };
	const float uvs[12]   = { 0.0f,0.0f,0.0f, 1.0f,0.0f,0.0f, 1.0f,1.0f,0.0f, 0.0f,1.0f,0.0f };
	const uint32_t vidx[6] = { 0,1,2, 0,2,3 };
	const uint32_t nidx[6] = { 0,0,0, 0,0,0 };
	const uint32_t uidx[6] = { 0,1,2, 0,2,3 };

	rise_blender_mesh mesh;
	std::memset( &mesh, 0, sizeof( mesh ) );
	mesh.name = "quad_alpha";
	mesh.vertices = verts; mesh.num_vertices = 4;
	mesh.normals = norms; mesh.num_normals = 1;
	mesh.uvs = uvs; mesh.num_uvs = 4;
	mesh.vertex_indices = vidx; mesh.normal_indices = nidx; mesh.uv_indices = uidx;
	mesh.num_triangles = 2;
	mesh.double_sided = 1;

	rise_blender_object obj = MakeIdentityObject( "obj_alpha", "quad_alpha", "m_expr_alpha" );

	rise_blender_scene scene;
	std::memset( &scene, 0, sizeof( scene ) );
	scene.camera = &cam;
	scene.painters = painters; scene.num_painters = 3;
	scene.materials = &mat; scene.num_materials = 1;
	scene.meshes = &mesh; scene.num_meshes = 1;
	scene.objects = &obj; scene.num_objects = 1;
	scene.world_radiance_painter_name = "p_env";
	scene.world_radiance_scale = 1.0f;
	scene.world_radiance_is_background = 1;

	rise_blender_render_result res;
	std::memset( &res, 0, sizeof( res ) );
	char err[512] = {0};
	const int ok = rise_blender_render_scene( &scene, &settings, 0, 0, 0, &res, err, sizeof( err ) );
	Check( ok != 0, std::string( "expression alpha render succeeded: " ) + err );
	if( ok && res.rgba ) {
		const size_t leftIdx  = ( ( H / 2 ) * W + ( W / 4 ) ) * 4;
		const size_t rightIdx = ( ( H / 2 ) * W + ( 3 * W / 4 ) ) * 4;
		Check( res.rgba[leftIdx + 0] > 0.5f && res.rgba[leftIdx + 2] < 0.2f,
			"expression alpha left half (u<0.5) renders opaque red" );
		Check( res.rgba[rightIdx + 2] > 0.5f && res.rgba[rightIdx + 0] < 0.2f,
			"expression alpha right half (u>0.5) clips to blue sky" );
	}
	rise_blender_free_render_result( &res );
}

void TestPhase3BlendedModifierAndCameraRayGlass()
{
	// 1. Verify RISE_BLENDER_MODIFIER_STACK and RISE_BLENDER_MODIFIER_BLEND registration
	{
		JobHolder holder;
		char err[512] = {0};
		rise_blender_painter h1, h2, wExpr;
		std::memset( &h1, 0, sizeof( h1 ) );
		std::memset( &h2, 0, sizeof( h2 ) );
		std::memset( &wExpr, 0, sizeof( wExpr ) );
		h1.name = "p_h1"; h1.kind = RISE_BLENDER_PAINTER_EXPRESSION; h1.expression = "P.x";
		h2.name = "p_h2"; h2.kind = RISE_BLENDER_PAINTER_EXPRESSION; h2.expression = "P.y";
		wExpr.name = "p_bw"; wExpr.kind = RISE_BLENDER_PAINTER_EXPRESSION; wExpr.expression = "0.5";
		Check( add_painter( *holder, h1, err, sizeof( err ) ), "add relief height painter 1" );
		Check( add_painter( *holder, h2, err, sizeof( err ) ), "add relief height painter 2" );
		Check( add_painter( *holder, wExpr, err, sizeof( err ) ), "add blend weight painter" );

		rise_blender_modifier r1, r2, mStack, mBlend, mBlendExpr;
		std::memset( &r1, 0, sizeof( r1 ) );
		std::memset( &r2, 0, sizeof( r2 ) );
		std::memset( &mStack, 0, sizeof( mStack ) );
		std::memset( &mBlend, 0, sizeof( mBlend ) );
		std::memset( &mBlendExpr, 0, sizeof( mBlendExpr ) );
		r1.name = "mod_r1"; r1.kind = RISE_BLENDER_MODIFIER_RELIEF; r1.source_painter_name = "p_h1"; r1.scale = 0.1f;
		r2.name = "mod_r2"; r2.kind = RISE_BLENDER_MODIFIER_RELIEF; r2.source_painter_name = "p_h2"; r2.scale = 0.1f;
		Check( add_modifier( *holder, r1, err, sizeof( err ) ), "add relief modifier 1" );
		Check( add_modifier( *holder, r2, err, sizeof( err ) ), "add relief modifier 2" );

		const char* stackMembers[2] = { "mod_r1", "mod_r2" };
		mStack.name = "mod_stack";
		mStack.kind = RISE_BLENDER_MODIFIER_STACK;
		mStack.stack_member_names = stackMembers;
		mStack.num_stack_members = 2;
		Check( add_modifier( *holder, mStack, err, sizeof( err ) ), "add RISE_BLENDER_MODIFIER_STACK" );

		mBlend.name = "mod_blend_lit";
		mBlend.kind = RISE_BLENDER_MODIFIER_BLEND;
		mBlend.blend_a_name = "mod_r1";
		mBlend.blend_b_name = "mod_r2";
		mBlend.blend_weight_painter_name = "0.5";
		Check( add_modifier( *holder, mBlend, err, sizeof( err ) ), "add RISE_BLENDER_MODIFIER_BLEND (literal)" );

		mBlendExpr.name = "mod_blend_expr";
		mBlendExpr.kind = RISE_BLENDER_MODIFIER_BLEND;
		mBlendExpr.blend_a_name = "mod_r1";
		mBlendExpr.blend_b_name = 0; // null = unperturbed normal
		mBlendExpr.blend_weight_painter_name = "p_bw";
		Check( add_modifier( *holder, mBlendExpr, err, sizeof( err ) ), "add RISE_BLENDER_MODIFIER_BLEND (expression)" );
	}

	// 2. End-to-end architectural glass render:
	//    A window pane covering x in [-2, 0.2] at z = 0 uses
	//    Mix(Transparent, EmissiveGreen, Factor = osl_is_camera_ray()), in front
	//    of a full white diffuse backdrop at z = -1 under a black environment.
	//    A point light at (-1.5, 0, +1.0) illuminates the right half of the
	//    backdrop (x > 0.2, z = -1) along shadow rays that cross z = 0 inside
	//    quad_glass (x ~ -0.35):
	//    - Camera hitting left half (x < 0) sees GREEN glass (osl_is_camera_ray() == 1).
	//    - Camera hitting right half (x > 0.2) sees white diffuse backdrop lit
	//      through the glass pane (shadow ray sees osl_is_camera_ray() == 0 ->
	//      transparent passthrough == 1 -> unoccluded!).
	{
		const uint32_t W = 24, H = 24;
		rise_blender_camera cam = MakeCamera( W, H );
		rise_blender_render_settings settings = MakeSettings( W, H, 8 );
		settings.progressive_enabled = 0;
		settings.oidn_denoise = 0;

		rise_blender_painter painters[5];
		std::memset( painters, 0, sizeof( painters ) );
		painters[0].name = "p_env";
		painters[0].kind = RISE_BLENDER_PAINTER_UNIFORM;
		painters[0].color[0] = 0.0f; painters[0].color[1] = 0.0f; painters[0].color[2] = 0.0f;
		painters[1].name = "p_green";
		painters[1].kind = RISE_BLENDER_PAINTER_UNIFORM;
		painters[1].color[0] = 0.0f; painters[1].color[1] = 3.14159f; painters[1].color[2] = 0.0f;
		painters[2].name = "p_w_cam";
		painters[2].kind = RISE_BLENDER_PAINTER_EXPRESSION;
		painters[2].expression = "osl_is_camera_ray()";
		painters[3].name = "p_w_pass";
		painters[3].kind = RISE_BLENDER_PAINTER_EXPRESSION;
		painters[3].expression = "1 - osl_is_camera_ray()";
		painters[4].name = "p_white";
		painters[4].kind = RISE_BLENDER_PAINTER_UNIFORM;
		painters[4].color[0] = 1.0f; painters[4].color[1] = 1.0f; painters[4].color[2] = 1.0f;

		rise_blender_material mats[3];
		std::memset( mats, 0, sizeof( mats ) );
		mats[0].name = "m_glass_leaf";
		mats[0].model = RISE_BLENDER_MATERIAL_LAMBERT;
		mats[0].diffuse_painter_name = "p_env";
		mats[0].emission_painter_name = "p_green";
		mats[0].double_sided = 1;

		rise_blender_mix_child child = { "m_glass_leaf", "p_w_cam", 0 };
		mats[1].name = "m_arch_glass";
		mats[1].model = RISE_BLENDER_MATERIAL_MIX;
		mats[1].mix_children = &child;
		mats[1].num_mix_children = 1;
		mats[1].mix_passthrough_painter_name = "p_w_pass";
		mats[1].double_sided = 1;

		mats[2].name = "m_backdrop";
		mats[2].model = RISE_BLENDER_MATERIAL_LAMBERT;
		mats[2].diffuse_painter_name = "p_white";
		mats[2].double_sided = 1;

		// Left glass quad: x in [-2, 0.2], y in [-2, 2], z = 0
		const float glassVerts[12] = { -2.0f,-2.0f,0.0f, 0.2f,-2.0f,0.0f, 0.2f,2.0f,0.0f, -2.0f,2.0f,0.0f };
		// Full backdrop quad: x in [-2, 2], y in [-2, 2], z = -1
		const float backVerts[12]  = { -2.0f,-2.0f,-1.0f, 2.0f,-2.0f,-1.0f, 2.0f,2.0f,-1.0f, -2.0f,2.0f,-1.0f };
		const float norms[3]  = { 0.0f, 0.0f, 1.0f };
		const float uvs[12]   = { 0.0f,0.0f,0.0f, 1.0f,0.0f,0.0f, 1.0f,1.0f,0.0f, 0.0f,1.0f,0.0f };
		const uint32_t vidx[6] = { 0,1,2, 0,2,3 };
		const uint32_t nidx[6] = { 0,0,0, 0,0,0 };
		const uint32_t uidx[6] = { 0,1,2, 0,2,3 };

		rise_blender_mesh meshes[2];
		std::memset( meshes, 0, sizeof( meshes ) );
		meshes[0].name = "quad_glass";
		meshes[0].vertices = glassVerts; meshes[0].num_vertices = 4;
		meshes[0].normals = norms; meshes[0].num_normals = 1;
		meshes[0].uvs = uvs; meshes[0].num_uvs = 4;
		meshes[0].vertex_indices = vidx; meshes[0].normal_indices = nidx; meshes[0].uv_indices = uidx;
		meshes[0].num_triangles = 2;
		meshes[0].double_sided = 1;

		meshes[1].name = "quad_back";
		meshes[1].vertices = backVerts; meshes[1].num_vertices = 4;
		meshes[1].normals = norms; meshes[1].num_normals = 1;
		meshes[1].uvs = uvs; meshes[1].num_uvs = 4;
		meshes[1].vertex_indices = vidx; meshes[1].normal_indices = nidx; meshes[1].uv_indices = uidx;
		meshes[1].num_triangles = 2;
		meshes[1].double_sided = 1;

		rise_blender_object objs[2];
		objs[0] = MakeIdentityObject( "obj_glass", "quad_glass", "m_arch_glass" );
		objs[1] = MakeIdentityObject( "obj_back", "quad_back", "m_backdrop" );

		// Point light at (-1.5, 0, +1.0): shadow ray from right-side backdrop (+0.8, 0, -1.0)
		// to light crosses z=0 at x = -1.5 + 0.5*(0.8 - (-1.5)) = -0.35 (right through quad_glass!).
		rise_blender_light light;
		std::memset( &light, 0, sizeof( light ) );
		light.name = "pt_light";
		light.type = RISE_BLENDER_LIGHT_POINT;
		light.position[0] = -1.5f; light.position[1] = 0.0f; light.position[2] = 1.0f;
		light.color[0] = 1.0f; light.color[1] = 1.0f; light.color[2] = 1.0f;
		light.intensity = 150.0f;

		rise_blender_scene scene;
		std::memset( &scene, 0, sizeof( scene ) );
		scene.camera = &cam;
		scene.painters = painters; scene.num_painters = 5;
		scene.materials = mats; scene.num_materials = 3;
		scene.meshes = meshes; scene.num_meshes = 2;
		scene.objects = objs; scene.num_objects = 2;
		scene.lights = &light; scene.num_lights = 1;
		scene.world_radiance_painter_name = "p_env";
		scene.world_radiance_scale = 1.0f;
		scene.world_radiance_is_background = 1;

		rise_blender_render_result res;
		std::memset( &res, 0, sizeof( res ) );
		char err[512] = {0};
		const int ok = rise_blender_render_scene( &scene, &settings, 0, 0, 0, &res, err, sizeof( err ) );
		Check( ok != 0, std::string( "architectural glass IsCameraRay render succeeded: " ) + err );
		if( ok && res.rgba ) {
			const size_t leftIdx  = ( ( H / 2 ) * W + ( W / 4 ) ) * 4;
			const size_t rightIdx = ( ( H / 2 ) * W + ( 3 * W / 4 ) ) * 4;
			Check( res.rgba[leftIdx + 1] > 0.5f && res.rgba[leftIdx + 0] < 0.2f,
				"camera ray sees green m_arch_glass leaf (osl_is_camera_ray() == 1)" );
			Check( res.rgba[rightIdx + 0] > 0.2f && res.rgba[rightIdx + 2] > 0.2f,
				"shadow ray passes through m_arch_glass unoccluded (osl_is_camera_ray() == 0)" );
		}
		rise_blender_free_render_result( &res );
	}

	// 3. Camera near-clip (`clip_start`):
	//    Camera at (0, 0, 3) looking along -Z.  A red blocker wall sits at z = 2.5
	//    (distance 0.5) in front of a blue target quad at z = 0 (distance 3.0).
	//    - With clip_start = 0.0, primary rays hit the red blocker wall.
	//    - With clip_start = 0.8, primary rays start past the blocker wall and hit
	//      the blue target quad cleanly (matching Blender / Cycles camera.clip_start).
	{
		const uint32_t W = 16, H = 16;
		rise_blender_render_settings settings = MakeSettings( W, H, 4 );
		settings.progressive_enabled = 0;
		settings.oidn_denoise = 0;

		rise_blender_painter painters[3];
		std::memset( painters, 0, sizeof( painters ) );
		painters[0].name = "p_black";
		painters[0].kind = RISE_BLENDER_PAINTER_UNIFORM;
		painters[1].name = "p_red";
		painters[1].kind = RISE_BLENDER_PAINTER_UNIFORM;
		painters[1].color[0] = 3.14159f;
		painters[2].name = "p_blue";
		painters[2].kind = RISE_BLENDER_PAINTER_UNIFORM;
		painters[2].color[2] = 3.14159f;

		rise_blender_material mats[2];
		std::memset( mats, 0, sizeof( mats ) );
		mats[0].name = "m_red";
		mats[0].model = RISE_BLENDER_MATERIAL_LAMBERT;
		mats[0].diffuse_painter_name = "p_black";
		mats[0].emission_painter_name = "p_red";
		mats[0].double_sided = 1;
		mats[1].name = "m_blue";
		mats[1].model = RISE_BLENDER_MATERIAL_LAMBERT;
		mats[1].diffuse_painter_name = "p_black";
		mats[1].emission_painter_name = "p_blue";
		mats[1].double_sided = 1;

		const float wallVerts[12]   = { -2.0f,-2.0f,2.5f, 2.0f,-2.0f,2.5f, 2.0f,2.0f,2.5f, -2.0f,2.0f,2.5f };
		const float targetVerts[12] = { -2.0f,-2.0f,0.0f, 2.0f,-2.0f,0.0f, 2.0f,2.0f,0.0f, -2.0f,2.0f,0.0f };
		const float norms[3]        = { 0.0f, 0.0f, 1.0f };
		const float uvs[12]         = { 0.0f,0.0f,0.0f, 1.0f,0.0f,0.0f, 1.0f,1.0f,0.0f, 0.0f,1.0f,0.0f };
		const uint32_t vidx[6]      = { 0,1,2, 0,2,3 };
		const uint32_t nidx[6]      = { 0,0,0, 0,0,0 };
		const uint32_t uidx[6]      = { 0,1,2, 0,2,3 };

		rise_blender_mesh meshes[2];
		std::memset( meshes, 0, sizeof( meshes ) );
		meshes[0].name = "q_wall";
		meshes[0].vertices = wallVerts; meshes[0].num_vertices = 4;
		meshes[0].normals = norms; meshes[0].num_normals = 1;
		meshes[0].uvs = uvs; meshes[0].num_uvs = 4;
		meshes[0].vertex_indices = vidx; meshes[0].normal_indices = nidx; meshes[0].uv_indices = uidx;
		meshes[0].num_triangles = 2; meshes[0].double_sided = 1;

		meshes[1].name = "q_target";
		meshes[1].vertices = targetVerts; meshes[1].num_vertices = 4;
		meshes[1].normals = norms; meshes[1].num_normals = 1;
		meshes[1].uvs = uvs; meshes[1].num_uvs = 4;
		meshes[1].vertex_indices = vidx; meshes[1].normal_indices = nidx; meshes[1].uv_indices = uidx;
		meshes[1].num_triangles = 2; meshes[1].double_sided = 1;

		rise_blender_object objs[2];
		objs[0] = MakeIdentityObject( "o_wall", "q_wall", "m_red" );
		objs[1] = MakeIdentityObject( "o_target", "q_target", "m_blue" );

		auto renderWithClip = [&]( float clipStart, float& outR, float& outB ) -> bool {
			rise_blender_camera cam = MakeCamera( W, H );
			cam.clip_start = clipStart;
			rise_blender_scene scene;
			std::memset( &scene, 0, sizeof( scene ) );
			scene.camera = &cam;
			scene.painters = painters; scene.num_painters = 3;
			scene.materials = mats; scene.num_materials = 2;
			scene.meshes = meshes; scene.num_meshes = 2;
			scene.objects = objs; scene.num_objects = 2;
			scene.world_radiance_painter_name = "p_black";
			scene.world_radiance_scale = 1.0f;
			scene.world_radiance_is_background = 1;

			rise_blender_render_result res;
			std::memset( &res, 0, sizeof( res ) );
			char err[512] = {0};
			const int ok = rise_blender_render_scene( &scene, &settings, 0, 0, 0, &res, err, sizeof( err ) );
			if( ok && res.rgba ) {
				const size_t cIdx = ( ( H / 2 ) * W + ( W / 2 ) ) * 4;
				outR = res.rgba[cIdx + 0];
				outB = res.rgba[cIdx + 2];
			}
			rise_blender_free_render_result( &res );
			return ok != 0;
		};

		float r0 = 0, b0 = 0, r1 = 0, b1 = 0;
		Check( renderWithClip( 0.0f, r0, b0 ) && r0 > 0.5f && b0 < 0.1f,
			"clip_start = 0.0 hits the near red blocker wall" );
		Check( renderWithClip( 0.8f, r1, b1 ) && b1 > 0.5f && r1 < 0.1f,
			"clip_start = 0.8 clips past the near wall and sees the blue target" );
	}
}

} // namespace

int main()
{
	TestAbiVersionAndLayout();
	TestNamedMeshAttributesAndValidation();
	TestMixMaterialValidationAndRefusals();
	TestEndToEndDielectricNamedAttrRender();
	TestEndToEndMixPassthroughRender();
	TestProgressInfoAndETA();
	TestWrapModeAndExpressionAlpha();
	TestPhase3BlendedModifierAndCameraRayGlass();

	std::cout << "BlenderBridgeMixAttrAOTest: " << g_checks << " checks, " << g_failures << " failures" << std::endl;
	return g_failures == 0 ? 0 : 1;
}
