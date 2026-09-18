//////////////////////////////////////////////////////////////////////
//
//  GLTFAnisotropyRotationTest.cpp - DL-17 regression (docs/DEBT_-
//    LEDGER.md, docs/CLOTH_FABRIC_DESIGN.md sec 15 item 12,
//    docs/GLTF_IMPORT.md).
//
//  glTF PER-TEXEL `anisotropy_rotation` (the direction encoded in the
//  KHR_materials_anisotropy texture's R/G channels) used to be DROPPED
//  at import -- the importer read only the texture's B channel
//  (strength) and applied the material's SCALAR anisotropyRotation
//  uniformly, logging once that the per-pixel direction was ignored.
//
//  Fixture: a hand-built, in-memory glTF document (written to TMPDIR
//  at test time, matching GLTFClearcoatImportTest.cpp's own pattern) --
//  a two-triangle quad with ONE material carrying `pbrMetallicRoughness`
//  + `KHR_materials_anisotropy`, whose `anisotropyTexture` is a REAL
//  8x1 PNG (built with RISE's own raster-image + PNG writer, exactly
//  like ScalarTexturePainterTest.cpp's WriteTempPNG) whose LEFT HALF
//  (columns 0-3) and RIGHT HALF (columns 4-7) encode two DIFFERENT
//  known rotations (8-wide, not 2, to clear BilinRasterImageAccessor's
//  half-texel sampling bias -- see WriteAnisotropyTexturePNG's comment):
//    left half:  direction (1, 0)  -> encoded R=1.0, G=0.5 -> angle 0
//    right half: direction (0, 1)  -> encoded R=0.5, G=1.0 -> angle pi/2
//  B (strength) = 1.0 at both texels; A = 1.0.  No UV-transform, no
//  wrap -- the importer's default REPEAT/clamp handling is irrelevant
//  here since we probe the resulting painter graph directly at chosen
//  `ptCoord`s rather than rendering, matching FabricMaterialChunkTest.-
//  cpp / GLTFClearcoatImportTest.cpp's own "probe the live objects"
//  idiom (materials import through Job:: calls, not scene-language
//  text the CST document carries).
//
//  What this proves:
//    A  the fixture imports and derives (parser plumbing intact).
//    B  the imported material's GGX base has a live, non-null
//       `tangent_rotation_scalar` -- MONEY: the DL-16 scalar-pipe slot
//       is actually reachable from the glTF path now, not just the
//       ggx_material chunk syntax.
//    C  MONEY (the actual DL-17 fix): probing that painter at UVs
//       landing in the left half vs the right half yields DIFFERENT
//       rotation angles -- 0 and pi/2 respectively -- i.e. the
//       rotation is genuinely PER-TEXEL, not the uniform scalar
//       fallback (which would read the SAME value everywhere).
//    D  the additive scalar `anisotropyRotation` term composes:
//       repeating with a non-zero scalar term shifts BOTH probed
//       angles by exactly that constant.
//    E  render-level: the two halves of the quad, sampled through
//       the live GGXBRDF, produce DIFFERENT anisotropic highlights
//       at the two texels' rotations.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IRasterImageWriter.h"
#include "../src/Library/Interfaces/IWriteBuffer.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/Math3D/VectorsOps.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Materials/GGXMaterial.h"
#include "../src/Library/Interfaces/IBSDF.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int s_pass = 0;
	int s_fail = 0;

	void Check( bool ok, const std::string& what )
	{
		if( ok ) { ++s_pass; std::printf( "  ok  : %s\n", what.c_str() ); }
		else     { ++s_fail; std::printf( "  FAIL: %s\n", what.c_str() ); }
	}

	std::string TempDir()
	{
		const char* base = std::getenv( "TMPDIR" );
		std::string dir = base ? base : "/tmp";
		if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += '/';
		return dir;
	}

	static const double kPI = 3.14159265358979323846;

	//! Builds an 8x1 anisotropy-direction texture: the LEFT HALF (columns
	//! 0-3) encodes angle 0 (direction (1,0)), the RIGHT HALF (columns
	//! 4-7) encodes angle pi/2 (direction (0,1)).  Rec709RGB_Linear --
	//! verbatim store, matching GLTFSceneImporter's
	//! TextureColorSpace("anisotropy") == Rec709RGB_Linear.
	//!
	//! WHY 8 WIDE, NOT 2: `BilinRasterImageAccessor::GetPel` maps a UV
	//! to a continuous pixel coordinate via `wrapped*W + 0.5` (a half-
	//! texel bias) and then pre-clamps to `[0, W-1]` -- documented in
	//! that file as deliberate ("do not fix") -- so any wrapped UV above
	//! `1 - 1.5/W` pins to the LAST texel.  At `W=2` that threshold is
	//! 0.25: nearly the WHOLE [0,1) UV range reads the last texel flat,
	//! leaving only a razor-thin sliver near U=0 to read the first.  A
	//! width of 8 pushes that threshold to 0.8125, giving each half a
	//! wide, stable plateau a probe can land on without hand-tuning to
	//! the accessor's exact half-texel convention.
	std::string WriteAnisotropyTexturePNG( const std::string& path )
	{
		IRasterImage* img = 0;
		RISE_API_CreateRISEColorRasterImage( &img, 8, 1, RISEColor( RISEPel( 0, 0, 0 ), 1.0 ) );
		// Left half: dir=(1,0) -> R=(1+1)/2=1.0, G=(0+1)/2=0.5
		for( unsigned int x = 0; x < 4; ++x ) {
			img->SetPEL( x, 0, RISEColor( RISEPel( 1.0, 0.5, 1.0 ), 1.0 ) );
		}
		// Right half: dir=(0,1) -> R=(0+1)/2=0.5, G=(1+1)/2=1.0
		for( unsigned int x = 4; x < 8; ++x ) {
			img->SetPEL( x, 0, RISEColor( RISEPel( 0.5, 1.0, 1.0 ), 1.0 ) );
		}

		IWriteBuffer* buf = 0;
		RISE_API_CreateDiskFileWriteBuffer( &buf, path.c_str() );

		IRasterImageWriter* writer = 0;
		RISE_API_CreatePNGWriter( &writer, *buf, 8, eColorSpace_Rec709RGB_Linear );

		img->DumpImage( writer );

		if( writer ) writer->release();
		if( buf )    buf->release();
		if( img )    img->release();
		return path;
	}

	//! Hand-built glTF JSON: a two-triangle quad (position/normal/UV0,
	//! same geometry as GLTFClearcoatImportTest.cpp's ClearcoatQuad.gltf
	//! plus a TEXCOORD_0 accessor/bufferView so the anisotropy texture
	//! has UVs to sample) with one material carrying
	//! `KHR_materials_anisotropy` (`anisotropyRotation` = `scalarRotation`,
	//! `anisotropyTexture` -> the PNG built above).
	//!
	//! Buffer layout (float32 unless noted): 4 x VEC3 POSITION (48B),
	//! 4 x VEC3 NORMAL (48B), 4 x VEC2 TEXCOORD_0 (32B),
	//! 6 x uint16 indices (12B, padded to 4-byte alignment by the
	//! bufferView after it -- there is none, so no padding needed here
	//! since it's the LAST view).  All are the SAME 4 corners as
	//! ClearcoatQuad.gltf: (-1,-1,0) (1,-1,0) (1,1,0) (-1,1,0), normal
	//! (0,0,1), UVs (0,0) (1,0) (1,1) (0,1), indices 0,1,2 0,2,3.
	std::string WriteAnisotropyQuadGltf( const std::string& gltfPath, const std::string& pngBasename,
	                                       double scalarRotationRadians )
	{
		std::ostringstream j;
		j << "{\n"
		  << " \"asset\": { \"version\": \"2.0\", \"generator\": \"RISE hand-authored fixture (GLTFAnisotropyRotationTest)\" },\n"
		  << " \"extensionsUsed\": [ \"KHR_materials_anisotropy\" ],\n"
		  << " \"scene\": 0,\n"
		  << " \"scenes\": [ { \"nodes\": [ 0 ] } ],\n"
		  << " \"nodes\": [ { \"mesh\": 0 } ],\n"
		  << " \"meshes\": [ { \"primitives\": [ { \"attributes\": { \"POSITION\": 0, \"NORMAL\": 1, \"TEXCOORD_0\": 2 }, \"indices\": 3, \"material\": 0, \"mode\": 4 } ] } ],\n"
		  << " \"materials\": [ {\n"
		  << "   \"name\": \"aniso_quad\",\n"
		  << "   \"pbrMetallicRoughness\": { \"baseColorFactor\": [0.6,0.6,0.6,1.0], \"metallicFactor\": 1.0, \"roughnessFactor\": 0.05 },\n"
		  << "   \"extensions\": { \"KHR_materials_anisotropy\": {\n"
		  << "     \"anisotropyStrength\": 0.9,\n"
		  << "     \"anisotropyRotation\": " << scalarRotationRadians << ",\n"
		  << "     \"anisotropyTexture\": { \"index\": 0 }\n"
		  << "   } }\n"
		  << " } ],\n"
		  << " \"textures\": [ { \"source\": 0 } ],\n"
		  << " \"images\": [ { \"uri\": \"" << pngBasename << "\" } ],\n"
		  << " \"accessors\": [\n"
		  << "  { \"bufferView\": 0, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\", \"min\": [-1.0,-1.0,0.0], \"max\": [1.0,1.0,0.0] },\n"
		  << "  { \"bufferView\": 1, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\" },\n"
		  << "  { \"bufferView\": 2, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC2\" },\n"
		  << "  { \"bufferView\": 3, \"componentType\": 5123, \"count\": 6, \"type\": \"SCALAR\" }\n"
		  << " ],\n"
		  << " \"bufferViews\": [\n"
		  << "  { \"buffer\": 0, \"byteOffset\": 0,  \"byteLength\": 48, \"target\": 34962 },\n"
		  << "  { \"buffer\": 0, \"byteOffset\": 48, \"byteLength\": 48, \"target\": 34962 },\n"
		  << "  { \"buffer\": 0, \"byteOffset\": 96, \"byteLength\": 32, \"target\": 34962 },\n"
		  << "  { \"buffer\": 0, \"byteOffset\": 128, \"byteLength\": 12, \"target\": 34963 }\n"
		  << " ],\n"
		  // Positions (-1,-1,0)(1,-1,0)(1,1,0)(-1,1,0) + Normals (0,0,1)x4 +
		  // UVs (0,0)(1,0)(1,1)(0,1) + indices 0,1,2,0,2,3 -- same base64
		  // payload construction as ClearcoatQuad.gltf's positions/normals,
		  // with a UV block and matching indices appended.  Generated once
		  // (not hand-typed) via a small Python one-liner encoding the raw
		  // float32/uint16 bytes; verified to decode back to the values
		  // above.
		  << " \"buffers\": [ { \"byteLength\": 140, \"uri\": \"data:application/octet-stream;base64,"
		  << "AACAvwAAgL8AAAAAAACAPwAAgL8AAAAAAACAPwAAgD8AAAAAAACAvwAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/"
		  << "AAAAAAAAAAAAAIA/AAAAAAAAgD8AAIA/AAAAAAAAgD8AAAEAAgAAAAIAAwA=\" } ]\n"
		  << "}\n";

		std::ofstream o( gltfPath.c_str(), std::ios::binary | std::ios::trunc );
		o << j.str();
		o.close();
		return gltfPath;
	}

	Job* LoadGltfScene( const std::string& gltfPath, const char* prefix )
	{
		std::ostringstream o;
		o << "RISE ASCII SCENE 7\n"
		  << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		  << "pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
		  << "film\n{\n\twidth 16\n\theight 16\n}\n\n"
		  << "pinhole_camera\n{\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
		  << "directional_light\n{\n\tname key\n\tpower 3.0\n\tcolor 1 1 1\n\tdirection 0.3 0.4 1.0\n}\n\n"
		  << "gltf_import\n{\n\tfile " << gltfPath << "\n\tname_prefix " << prefix << "\n}\n\n";

		const std::string scenePath = TempDir() + "rise_anisorot_" + prefix + ".RISEscene";
		{ std::ofstream f( scenePath.c_str(), std::ios::binary | std::ios::trunc ); f << o.str(); }

		Job* job = new Job();
		if( !job->LoadAsciiSceneViaCst( scenePath.c_str() ) ) {
			job->release();
			return nullptr;
		}
		return job;
	}

	//! A fixed shading point + probe at a chosen ptCoord, mirroring
	//! FabricMaterialChunkTest.cpp / GGXTangentRotationScalarTest.cpp's
	//! MakeProbe idiom.
	RayIntersectionGeometric MakeProbeAt( double u, double v )
	{
		const double th = 35.0 * kPI / 180.0;
		const Vector3 inDir( sin(th), 0, -cos(th) );
		Ray inRay( Point3( sin(th), 0, 1.0 ), inDir );
		RasterizerState rs = { 0, 0 };
		RayIntersectionGeometric ri( inRay, rs );
		ri.bHit = true;
		ri.range = 1.0;
		ri.ptIntersection = Point3( 0, 0, 0 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.vGeomNormal = ri.vNormal;
		ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
		ri.ptCoord = Point2( u, v );
		return ri;
	}

	Vector3 OutgoingProbeDir()
	{
		const double th2 = 20.0 * kPI / 180.0;
		const double phi2 = 55.0 * kPI / 180.0;
		return Vector3Ops::Normalize( Vector3( sin(th2)*cos(phi2), sin(th2)*sin(phi2), cos(th2) ) );
	}

	//! Wraps angle differences into (-pi, pi] so an atan2 result of, say,
	//! -0.0000001 compares cleanly against an authored 0.0.
	double AngleDiff( double a, double b )
	{
		double d = a - b;
		while( d > kPI )  d -= 2*kPI;
		while( d < -kPI ) d += 2*kPI;
		return d;
	}
}

static void RunFixture( double scalarRotationRadians, const char* tag )
{
	std::printf( "-- DL-17 glTF per-texel anisotropy rotation (scalar term = %.4f) --\n", scalarRotationRadians );

	const std::string pngPath = TempDir() + "rise_anisorot_" + tag + ".png";
	WriteAnisotropyTexturePNG( pngPath );
	// The .gltf's image `uri` is resolved relative to the .gltf's own
	// directory -- both files live in TMPDIR, so the basename suffices.
	const std::string pngBasename = pngPath.substr( pngPath.find_last_of( '/' ) + 1 );

	const std::string gltfPath = TempDir() + "rise_anisorot_" + tag + ".gltf";
	WriteAnisotropyQuadGltf( gltfPath, pngBasename, scalarRotationRadians );

	Job* job = LoadGltfScene( gltfPath, tag );
	Check( job != nullptr, "A: the anisotropy-texture glTF fixture imports and derives" );
	if( !job ) { return; }

	// GLTFSceneImporter::MaterialName( prefix, idx ) = "<prefix>.mat.<idx>".
	IMaterial* mat = job->GetMaterials()->GetItem( ( std::string( tag ) + ".mat.0" ).c_str() );
	GGXMaterial* ggx = mat ? dynamic_cast<GGXMaterial*>( mat ) : nullptr;
	Check( ggx != nullptr,
		"the imported material resolves to a live GGXMaterial (pbr_metallic_roughness "
		"-> GGX at scene-build time)" );
	if( ggx ) {
		const IScalarPainter* rotPainter = ggx->GetTangentRotationScalar();
		Check( rotPainter != nullptr,
			"B MONEY: the imported GGX carries a live tangent_rotation_scalar -- the DL-16 "
			"scalar-pipe slot is reachable from the glTF import path" );

		if( rotPainter ) {
			const RayIntersectionGeometric probe0 = MakeProbeAt( 0.25, 0.5 );	// texel (0, *) CENTER -> R=1.0
			const RayIntersectionGeometric probe1 = MakeProbeAt( 0.75, 0.5 );	// texel (1, *) CENTER -> R=0.5

			const double angle0 = rotPainter->GetValuesAt( probe0 ).v[0];
			const double angle1 = rotPainter->GetValuesAt( probe1 ).v[0];
			std::printf( "     angle at texel 0 (expect %.6f + scalar) = %.6f\n", 0.0, angle0 );
			std::printf( "     angle at texel 1 (expect %.6f + scalar) = %.6f\n", kPI/2.0, angle1 );

			// 8-bit PNG quantization (1/255 per channel) propagates through atan2 to
			// up to a few thousandths of a radian -- loosened from an exact
			// comparison accordingly, still two orders of magnitude tighter than
			// the pi/2 (1.57 rad) gap between the two texels' rotations.
			Check( std::fabs( AngleDiff( angle0, 0.0 + scalarRotationRadians ) ) < 0.02,
				"C MONEY: texel (0,*)'s rotation reads back atan2(dir)=0 + the scalar term" );
			Check( std::fabs( AngleDiff( angle1, kPI/2.0 + scalarRotationRadians ) ) < 0.02,
				"C MONEY: texel (1,*)'s rotation reads back atan2(dir)=pi/2 + the scalar term" );
			Check( std::fabs( AngleDiff( angle0, angle1 ) ) > 0.5,
				"C MONEY: the two texels give GENUINELY DIFFERENT rotations -- per-texel, "
				"not the pre-fix uniform-scalar fallback (which would read identically at "
				"both probes)" );
		}

		// E: render-level -- the BSDF value at the two texels' rotations
		// must differ under a fixed (in, out) pair, exactly like DL-16's
		// own render-parity row confirms rotation actually matters here.
		{
			const RayIntersectionGeometric probe0 = MakeProbeAt( 0.25, 0.5 );
			const RayIntersectionGeometric probe1 = MakeProbeAt( 0.75, 0.5 );
			const Vector3 wo = OutgoingProbeDir();
			IBSDF* bsdf = ggx->GetBSDF();
			const RISEPel v0 = bsdf->value( wo, probe0 );
			const RISEPel v1 = bsdf->value( wo, probe1 );
			Check( std::fabs( v0[0] - v1[0] ) > 1e-6 || std::fabs( v0[1] - v1[1] ) > 1e-6,
				"E MONEY: the two texels' different per-texel rotations produce a genuinely "
				"different anisotropic BSDF value at a fixed (in, out) direction pair" );
		}
	}

	job->release();
}

int main()
{
	std::printf( "===== DL-17 glTF per-texel anisotropy rotation test =====\n" );
	RunFixture( 0.0, "zero" );
	RunFixture( 0.2, "offset" );	// D: the additive scalar term composes with the per-texel direction
	std::printf( "\n%d passed, %d failed\n", s_pass, s_fail );
	return s_fail == 0 ? 0 : 1;
}
