//////////////////////////////////////////////////////////////////////
//
//  GLTFSceneImporter.cpp - Implementation.  See header for the
//  design.  Phase 2 of glTF support; see docs/GLTF_IMPORT.md.
//
//  Phase 2 scope summary -- what this importer does:
//   - Resolves the requested glTF scene (or the file's default scene).
//   - Walks the node tree depth-first, accumulating world matrices.
//   - For each node with a mesh: emits one gltfmesh_geometry per
//     primitive (re-using the Phase 1 loader), bound to a PBR material
//     + optional normal-map modifier through standard_object.
//   - For each glTF material (de-duplicated by index): emits a
//     pbr_metallic_roughness_material by feeding baseColor /
//     metallicRoughness / emissive textures (or factor-driven
//     uniformcolor painters when the slot is texture-less) into
//     Job::AddPBRMetallicRoughnessMaterial.
//   - For each glTF texture: emits one painter.  Embedded `.glb` images
//     and inline `data:` URIs go directly from the cgltf bufferView
//     bytes into a painter via Job::AddInMemoryPNGTexturePainter /
//     AddInMemoryJPEGTexturePainter -- no disk round-trip.  External
//     image URIs in the .gltf JSON form continue to use the existing
//     file-path painter APIs.  (Earlier revisions wrote a sidecar copy
//     under <glb_dir>/.gltf_cache/; that path is gone in Phase 3.)
//   - For each KHR_lights_punctual node: emits an omni_light /
//     spot_light / directional_light with the world-space placement
//     derived from the node's accumulated transform.
//   - For the first camera-bearing node: emits a pinhole_camera with
//     the world-space placement; subsequent camera nodes warn.
//
//  Scope summary as of Phase 3 -- what this importer DOES NOT do
//  (see docs/GLTF_IMPORT.md §13 + §6 for full rationale):
//   - Skinning / animation / morph targets -- import skips them with
//     a one-time warning per file.  Geometry renders at bind pose.
//   - alphaMode = MASK -- supported via alpha_test_shaderop, but
//     only honoured by integrators that route through IShader::Shade()
//     (path tracer + legacy direct shaders).  BDPT, VCM, MLT, and
//     photon tracers bypass the shader-op pipeline and treat MASK as
//     opaque.  No runtime warning is emitted for that mismatch.
//   - alphaMode = BLEND -- not supported; warn-and-treat-as-opaque.
//     glTF alphaCutoff = 0.5 is honoured for MASK; per-pixel alpha
//     uses max(R,G,B) of baseColor as a luminance proxy because the
//     painter system doesn't expose the A channel of an RGBA texture.
//   - KHR_draco_mesh_compression / EXT_meshopt_compression -- rejected
//     by the Phase 1 mesh loader; the importer surfaces that error.
//   - Per-mesh KHR_materials_* extensions beyond the core PBR shape
//     (clearcoat, sheen, transmission, etc.) -- ignored with one
//     warning per extension type.
//
//  Phase 3 retired the lossy Euler-decomposition node-transform path:
//  the importer now passes each node's column-major world matrix to
//  Job::AddObjectMatrix verbatim, eliminating gimbal-lock and shear
//  decomposition failure modes.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 30, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "GLTFSceneImporter.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/MediaPathLocator.h"

#include "../../../extlib/cgltf/cgltf.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	// ---------- path utilities ----------

	// Returns the directory portion of a path (without trailing separator),
	// or "." if the path has no directory component.  Used only for
	// resolving external image URIs in glTF JSON form against the asset
	// directory; embedded `.glb` images go straight from buffer view to
	// in-memory painter without any disk path lookups.
	std::string DirOf( const std::string& path )
	{
		const size_t slash = path.find_last_of( "/\\" );
		if( slash == std::string::npos ) {
			return std::string( "." );
		}
		return path.substr( 0, slash );
	}

	// ---------- transform helpers (TRS or matrix to Euler XYZ degrees) ----------

	// 4x4 column-major matrix multiply (cgltf convention).
	void Mat4Mul( const cgltf_float A[16], const cgltf_float B[16], cgltf_float outM[16] )
	{
		for( int c = 0; c < 4; ++c ) {
			for( int r = 0; r < 4; ++r ) {
				cgltf_float v = 0;
				for( int k = 0; k < 4; ++k ) {
					v += A[ k*4 + r ] * B[ c*4 + k ];
				}
				outM[ c*4 + r ] = v;
			}
		}
	}

	void Mat4Identity( cgltf_float M[16] )
	{
		for( int i = 0; i < 16; ++i ) M[i] = 0;
		M[0] = M[5] = M[10] = M[15] = 1;
	}

	// Build a column-major 4x4 from glTF TRS components.  Quaternion is
	// (x, y, z, w) per glTF spec.
	void Mat4FromTRS(
		const cgltf_float t[3], const cgltf_float q[4], const cgltf_float s[3],
		cgltf_float outM[16] )
	{
		const cgltf_float x = q[0], y = q[1], z = q[2], w = q[3];
		const cgltf_float xx = x*x, yy = y*y, zz = z*z;
		const cgltf_float xy = x*y, xz = x*z, yz = y*z;
		const cgltf_float wx = w*x, wy = w*y, wz = w*z;

		// Row-major rotation; we'll fold scale + transpose into column-major below.
		const cgltf_float r00 = 1 - 2*(yy + zz);
		const cgltf_float r01 = 2*(xy - wz);
		const cgltf_float r02 = 2*(xz + wy);
		const cgltf_float r10 = 2*(xy + wz);
		const cgltf_float r11 = 1 - 2*(xx + zz);
		const cgltf_float r12 = 2*(yz - wx);
		const cgltf_float r20 = 2*(xz - wy);
		const cgltf_float r21 = 2*(yz + wx);
		const cgltf_float r22 = 1 - 2*(xx + yy);

		// Column-major: column c, row r at outM[c*4 + r]
		outM[0]  = r00 * s[0];	outM[4]  = r01 * s[1];	outM[8]  = r02 * s[2];	outM[12] = t[0];
		outM[1]  = r10 * s[0];	outM[5]  = r11 * s[1];	outM[9]  = r12 * s[2];	outM[13] = t[1];
		outM[2]  = r20 * s[0];	outM[6]  = r21 * s[1];	outM[10] = r22 * s[2];	outM[14] = t[2];
		outM[3]  = 0;			outM[7]  = 0;			outM[11] = 0;			outM[15] = 1;
	}

	// Read a node's local transform (from explicit matrix if has_matrix,
	// else from TRS) into a column-major matrix.
	void NodeLocalMatrix( const cgltf_node* node, cgltf_float outM[16] )
	{
		if( node->has_matrix ) {
			std::memcpy( outM, node->matrix, 16 * sizeof( cgltf_float ) );
		} else {
			cgltf_float t[3] = { 0, 0, 0 };
			cgltf_float q[4] = { 0, 0, 0, 1 };
			cgltf_float s[3] = { 1, 1, 1 };
			if( node->has_translation ) std::memcpy( t, node->translation, sizeof(t) );
			if( node->has_rotation )    std::memcpy( q, node->rotation,    sizeof(q) );
			if( node->has_scale )       std::memcpy( s, node->scale,       sizeof(s) );
			Mat4FromTRS( t, q, s, outM );
		}
	}

	// Phase 3 retired DecomposeAffine — the importer now passes the
	// node-world 4x4 verbatim to Job::AddObjectMatrix instead of going
	// through Euler XYZ.  Keeping the column-major matrix all the way
	// to the object eliminates gimbal-lock and shear-decomposition
	// failure modes.

	// ---------- embedded texture access ----------

	// Resolves a glTF image to either an existing on-disk file path or
	// an in-memory byte buffer.  The Phase 3 retirement of the
	// `.gltf_cache/` sidecar means embedded images go straight from the
	// `.glb` bufferView into a painter via Job::AddInMemoryPNGTexturePainter
	// / AddInMemoryJPEGTexturePainter -- no disk round-trip.
	//
	// Outputs: caller picks between two paths based on which is set:
	//   1. External URI to an existing sidecar file → outFilePath set,
	//      outBytes/outLen unset.  Use the file-path painter API.
	//   2. Embedded bufferView (any .glb image, plus .gltf `data:` URIs
	//      cgltf already decoded) → outBytes/outLen set, outFilePath
	//      empty.  Use the in-memory painter API.
	// outExt is the file extension (.png / .jpg / .jpeg) so the caller
	// knows which decoder to invoke; empty on failure.
	void ResolveImageBytes(
		const cgltf_data*    data,
		const std::string&   glbPath,
		size_t               imageIdx,
		std::string&         outExt,
		std::string&         outFilePath,
		const unsigned char*& outBytes,
		size_t&              outLen )
	{
		outExt.clear();
		outFilePath.clear();
		outBytes = NULL;
		outLen = 0;

		const cgltf_image& img = data->images[ imageIdx ];

		// Determine extension from mime type or URI suffix.
		if( img.mime_type ) {
			if(      std::strcmp( img.mime_type, "image/png"  ) == 0 ) outExt = ".png";
			else if( std::strcmp( img.mime_type, "image/jpeg" ) == 0 ) outExt = ".jpg";
		}
		if( outExt.empty() && img.uri ) {
			const char* dotp = std::strrchr( img.uri, '.' );
			if( dotp ) outExt = std::string( dotp );
		}

		// Path 1: external URI to an existing sidecar file (cheap case for
		// .gltf JSON form).  Skip "data:" URIs — those are inline base64
		// that cgltf has already decoded into a bufferView.
		if( img.uri && std::strncmp( img.uri, "data:", 5 ) != 0 ) {
			const std::string sideloaded = DirOf( glbPath ) + "/" + img.uri;
			struct stat s;
			if( stat( sideloaded.c_str(), &s ) == 0 ) {
				outFilePath = sideloaded;
				return;
			}
			// URI doesn't resolve; fall through to the bufferView path.
		}

		// Path 2: embedded image bytes (any .glb image, or a .gltf with a
		// data: URI that cgltf already decoded into a bufferView).
		if( !img.buffer_view ) {
			GlobalLog()->PrintEx( eLog_Error,
				"GLTFSceneImporter:: image %u has no buffer_view and no resolvable URI; "
				"texture will be missing", (unsigned)imageIdx );
			return;
		}
		const cgltf_buffer_view* bv = img.buffer_view;
		const cgltf_buffer*      buf = bv->buffer;
		if( !buf || !buf->data ) {
			GlobalLog()->PrintEx( eLog_Error,
				"GLTFSceneImporter:: image %u bufferView has no backing data; "
				"call cgltf_load_buffers first or check if the asset references a "
				"separate .bin that's missing on disk", (unsigned)imageIdx );
			return;
		}
		outBytes = static_cast<const unsigned char*>( buf->data ) + bv->offset;
		outLen   = static_cast<size_t>( bv->size );
	}

	// ---------- name composition ----------

	std::string MaterialName ( const std::string& prefix, size_t idx ) {
		std::ostringstream oss; oss << prefix << ".mat." << idx; return oss.str();
	}
	std::string ModifierName ( const std::string& prefix, size_t idx ) {
		std::ostringstream oss; oss << prefix << ".nm." << idx; return oss.str();
	}
	std::string GeomName     ( const std::string& prefix, size_t mesh, size_t prim ) {
		std::ostringstream oss; oss << prefix << ".geom.m" << mesh << ".p" << prim; return oss.str();
	}
	std::string ObjectName   ( const std::string& prefix, size_t nodeIdx, size_t prim ) {
		std::ostringstream oss; oss << prefix << ".obj.n" << nodeIdx << ".p" << prim; return oss.str();
	}
	std::string LightName    ( const std::string& prefix, size_t idx ) {
		std::ostringstream oss; oss << prefix << ".light." << idx; return oss.str();
	}
	std::string CameraName   ( const std::string& prefix, size_t idx ) {
		std::ostringstream oss; oss << prefix << ".cam." << idx; return oss.str();
	}
	std::string PainterName  ( const std::string& prefix, const char* role, size_t matIdx ) {
		std::ostringstream oss; oss << prefix << ".pnt." << role << "." << matIdx; return oss.str();
	}
	// Painter name MUST include the role because a single image can be
	// referenced by multiple materials in different semantic roles
	// (e.g., asset A as baseColor and asset B as normal map for the same
	// underlying PNG, which need different colour spaces).  Without the
	// role suffix `pPntManager->AddItem` rejects the second registration
	// and the second material silently inherits the first material's
	// colour-space encoding.
	std::string ImagePainterName( const std::string& prefix, const char* role, size_t imgIdx ) {
		std::ostringstream oss; oss << prefix << ".img." << role << "." << imgIdx; return oss.str();
	}

	// ---------- texture-painter creation ----------

	// Decide the right `color_space` argument for an image painter
	// based on its semantic role: baseColor / emissive want sRGB
	// (gamma-decoded for display); normal / metallicRoughness /
	// occlusion want a verbatim store with no colour conversion --
	// in today's RISE (RISEPel == ROMMRGB) that's "ROMMRGB_Linear".
	const char* TextureColorSpace( const char* role )
	{
		if( std::strcmp( role, "basecolor" ) == 0 ) return "sRGB";
		if( std::strcmp( role, "emissive"  ) == 0 ) return "sRGB";
		// normal / mr / occlusion -- verbatim store, no matrix conversion.
		return "ROMMRGB_Linear";
	}

	// Create the texture painter (png_painter or jpg_painter) for the
	// given role / texture index.  Returns the registered painter name
	// on success; empty string on failure (caller can swap in a
	// uniformcolor fallback).
	std::string CreateTexturePainter(
		IJob&               job,
		const std::string&  prefix,
		const cgltf_data*   data,
		const std::string&  glbPath,
		const cgltf_texture* tex,
		const char*         role )
	{
		if( !tex || !tex->image ) {
			return std::string();
		}
		const size_t imgIdx = (size_t)( tex->image - data->images );

		std::string ext, filePath;
		const unsigned char* bytes = NULL;
		size_t len = 0;
		ResolveImageBytes( data, glbPath, imgIdx, ext, filePath, bytes, len );

		if( filePath.empty() && bytes == NULL ) {
			return std::string();	// ResolveImageBytes already logged the cause
		}

		const std::string painterName = ImagePainterName( prefix, role, imgIdx );
		const char* colorSpace = TextureColorSpace( role );

		const double scale[3] = { 1.0, 1.0, 1.0 };
		const double shift[3] = { 0.0, 0.0, 0.0 };

		// `color_space` arg to the painter APIs is a char selector:
		// 0=Rec709 linear, 1=sRGB, 2=ROMM linear, 3=ProPhoto.
		char cs = 1;	// sRGB default
		if(      std::strcmp( colorSpace, "Rec709RGB_Linear" ) == 0 ) cs = 0;
		else if( std::strcmp( colorSpace, "sRGB"             ) == 0 ) cs = 1;
		else if( std::strcmp( colorSpace, "ROMMRGB_Linear"   ) == 0 ) cs = 2;
		else if( std::strcmp( colorSpace, "ProPhotoRGB"      ) == 0 ) cs = 3;

		bool ok = false;
		if( !filePath.empty() ) {
			// On-disk sidecar (the .gltf JSON form with external image files).
			if( ext == ".png" ) {
				ok = job.AddPNGTexturePainter(
					painterName.c_str(), filePath.c_str(),
					cs, /*filter*/ 1, /*lowmem*/ false, scale, shift );
			} else if( ext == ".jpg" || ext == ".jpeg" ) {
				ok = job.AddJPEGTexturePainter(
					painterName.c_str(), filePath.c_str(),
					cs, /*filter*/ 1, /*lowmem*/ false, scale, shift );
			}
		} else {
			// Embedded bytes (any .glb image; .gltf `data:` URIs cgltf already
			// decoded into a bufferView).  No disk round-trip.
			if( ext == ".png" ) {
				ok = job.AddInMemoryPNGTexturePainter(
					painterName.c_str(), bytes, len,
					cs, /*filter*/ 1, /*lowmem*/ false, scale, shift );
			} else if( ext == ".jpg" || ext == ".jpeg" ) {
				ok = job.AddInMemoryJPEGTexturePainter(
					painterName.c_str(), bytes, len,
					cs, /*filter*/ 1, /*lowmem*/ false, scale, shift );
			}
		}

		if( !ok ) {
			GlobalLog()->PrintEx( eLog_Error,
				"GLTFSceneImporter:: failed to register painter `%s` (ext=`%s`); "
				"only PNG and JPEG are wired up", painterName.c_str(), ext.c_str() );
			return std::string();
		}
		return painterName;
	}

	// ---------- material creation ----------

	// Builds a pbr_metallic_roughness_material from a glTF material.
	// Auto-creates per-role painters; falls back to uniformcolor when a
	// texture slot is empty (uses the glTF *Factor field).  Returns true
	// on success; on failure logs an error and the caller binds the
	// "none" material to the object (which renders as the default).
	bool CreateMaterial(
		IJob&               job,
		const std::string&  prefix,
		const cgltf_data*   data,
		const std::string&  glbPath,
		size_t              matIdx )
	{
		const cgltf_material& mat = data->materials[ matIdx ];

		// glTF baseColor = baseColorTexture x baseColorFactor (componentwise),
		// per Khronos spec §3.9.2.  Earlier revisions picked one or the other,
		// silently dropping the factor whenever a texture was present -- which
		// breaks any asset that tints a grayscale texture with a coloured
		// factor or gates alpha with factor.a.  This now always multiplies.
		//
		// The multiply uses BlendPainter's formula `a*c + b*(1-c)` with
		// b = zero, c = factorRGB, giving `texture * factorRGB`.
		//
		// The alpha component of the factor is captured separately into
		// `factorAlpha` and folded into the alphaMode = MASK cutoff below
		// (the painter system doesn't expose a painter's A channel, so per-
		// pixel `texture.a * factor.a` can't be assembled here directly).
		std::string baseColorPainter;
		bool baseColorIsTexture = false;	// false => uniform painter (alpha mask becomes pass/fail)
		double factorAlpha = 1.0;
		if( mat.has_pbr_metallic_roughness ) {
			const cgltf_pbr_metallic_roughness& pmr = mat.pbr_metallic_roughness;
			factorAlpha = (double)pmr.base_color_factor[3];

			std::string textureName;
			if( pmr.base_color_texture.texture ) {
				textureName = CreateTexturePainter(
					job, prefix, data, glbPath, pmr.base_color_texture.texture, "basecolor" );
			}

			const std::string nFactorRGB = PainterName( prefix, "basecolor_factor", matIdx );
			const double rgbFactor[3] = {
				(double)pmr.base_color_factor[0],
				(double)pmr.base_color_factor[1],
				(double)pmr.base_color_factor[2] };
			job.AddUniformColorPainter( nFactorRGB.c_str(), rgbFactor, "Rec709RGB_Linear" );

			if( !textureName.empty() ) {
				// Multiply: blend(texture, zero, factorRGB) = texture * factorRGB
				const std::string nZero   = PainterName( prefix, "basecolor_zero", matIdx );
				const double zero[3] = { 0.0, 0.0, 0.0 };
				job.AddUniformColorPainter( nZero.c_str(), zero, "Rec709RGB_Linear" );

				const std::string nProduct = PainterName( prefix, "basecolor", matIdx );
				job.AddBlendPainter( nProduct.c_str(),
					textureName.c_str(),
					nZero.c_str(),
					nFactorRGB.c_str() );
				baseColorPainter   = nProduct;
				baseColorIsTexture = true;
			} else {
				// No texture -- the factor IS the baseColor.
				baseColorPainter = nFactorRGB;
			}
		} else {
			// No PBR -- fallback to a mid-gray.
			const std::string n = PainterName( prefix, "basecolor", matIdx );
			const double pel[3] = { 0.5, 0.5, 0.5 };
			job.AddUniformColorPainter( n.c_str(), pel, "Rec709RGB_Linear" );
			baseColorPainter = n;
		}

		// Metallic / roughness -- channel-extract from the MR texture if
		// present, else use the glTF factor as a uniformcolor.
		std::string metallicPainter;
		std::string roughnessPainter;
		if( mat.has_pbr_metallic_roughness ) {
			const cgltf_pbr_metallic_roughness& pmr = mat.pbr_metallic_roughness;
			if( pmr.metallic_roughness_texture.texture ) {
				const std::string mrTex = CreateTexturePainter(
					job, prefix, data, glbPath, pmr.metallic_roughness_texture.texture,
					"metallic_roughness" );
				if( !mrTex.empty() ) {
					// glTF MR convention: G = roughness, B = metallic.
					const std::string mName = PainterName( prefix, "metallic", matIdx );
					const std::string rName = PainterName( prefix, "roughness", matIdx );
					job.AddChannelPainter( mName.c_str(), mrTex.c_str(), 2 /*B*/, pmr.metallic_factor,  0.0 );
					job.AddChannelPainter( rName.c_str(), mrTex.c_str(), 1 /*G*/, pmr.roughness_factor, 0.0 );
					metallicPainter  = mName;
					roughnessPainter = rName;
				}
			}
			if( metallicPainter.empty() ) {
				const std::string n = PainterName( prefix, "metallic", matIdx );
				const double v = pmr.metallic_factor;
				const double pel[3] = { v, v, v };
				job.AddUniformColorPainter( n.c_str(), pel, "Rec709RGB_Linear" );
				metallicPainter = n;
			}
			if( roughnessPainter.empty() ) {
				const std::string n = PainterName( prefix, "roughness", matIdx );
				const double v = pmr.roughness_factor;
				const double pel[3] = { v, v, v };
				job.AddUniformColorPainter( n.c_str(), pel, "Rec709RGB_Linear" );
				roughnessPainter = n;
			}
		} else {
			// No PBR section -- defaults that look like a plastic.
			const std::string nM = PainterName( prefix, "metallic",  matIdx );
			const std::string nR = PainterName( prefix, "roughness", matIdx );
			const double zero[3] = { 0.0, 0.0, 0.0 };
			const double mid[3]  = { 0.5, 0.5, 0.5 };
			job.AddUniformColorPainter( nM.c_str(), zero, "Rec709RGB_Linear" );
			job.AddUniformColorPainter( nR.c_str(), mid,  "Rec709RGB_Linear" );
			metallicPainter  = nM;
			roughnessPainter = nR;
		}

		// Emissive -- texture or factor.
		std::string emissivePainter = "none";
		if( mat.emissive_texture.texture ) {
			emissivePainter = CreateTexturePainter(
				job, prefix, data, glbPath, mat.emissive_texture.texture, "emissive" );
			if( emissivePainter.empty() ) emissivePainter = "none";
		}
		if( emissivePainter == "none" ) {
			const cgltf_float ef[3] = {
				mat.emissive_factor[0],
				mat.emissive_factor[1],
				mat.emissive_factor[2] };
			if( ef[0] > 0 || ef[1] > 0 || ef[2] > 0 ) {
				const std::string n = PainterName( prefix, "emissive", matIdx );
				const double pel[3] = { ef[0], ef[1], ef[2] };
				job.AddUniformColorPainter( n.c_str(), pel, "Rec709RGB_Linear" );
				emissivePainter = n;
			}
		}

		const std::string matName = MaterialName( prefix, matIdx );
		const bool ok = job.AddPBRMetallicRoughnessMaterial(
			matName.c_str(),
			baseColorPainter.c_str(),
			metallicPainter.c_str(),
			roughnessPainter.c_str(),
			/*ior*/ 1.5,
			emissivePainter.c_str(),
			/*emissive_scale*/ 1.0 );
		if( !ok ) {
			GlobalLog()->PrintEx( eLog_Error,
				"GLTFSceneImporter:: failed to register material `%s`", matName.c_str() );
			return false;
		}

		// Optional normal-map modifier.
		if( mat.normal_texture.texture ) {
			const std::string nmTex = CreateTexturePainter(
				job, prefix, data, glbPath, mat.normal_texture.texture, "normal" );
			if( !nmTex.empty() ) {
				const std::string modName = ModifierName( prefix, matIdx );
				job.AddNormalMapModifier( modName.c_str(), nmTex.c_str(), mat.normal_texture.scale );
			}
		}

		// Optional alpha-mask shader (glTF alphaMode = MASK).  For
		// alphaMode = OPAQUE we leave the global shader untouched; for
		// MASK we register a per-material standard_shader containing the
		// alpha_test_shaderop.  BLEND is documented as out-of-scope -- we
		// log once and treat it as opaque.  Per-pixel alpha currently
		// reads max(R,G,B) of baseColor as a luminance proxy because the
		// painter system doesn't expose the A channel; this matches the
		// MaskedAlphaMode test asset's behaviour where transparent pixels
		// are also dark, and the architectural hook is in place for a
		// future alpha-aware painter.
		if( mat.alpha_mode == cgltf_alpha_mode_mask ) {
			// When the baseColor painter is a uniform (no texture loaded),
			// the alpha test degenerates to a global pass/fail toggle for
			// the whole material — alpha < cutoff fully discards every hit
			// or alpha >= cutoff fully keeps every hit.  Warn so users
			// expecting per-pixel cutout know they got something coarser.
			std::string alphaSource;
			if( baseColorIsTexture ) {
				alphaSource = baseColorPainter;
			} else {
				const std::string n = PainterName( prefix, "alpha_factor", matIdx );
				const double pel[3] = { factorAlpha, factorAlpha, factorAlpha };
				job.AddUniformColorPainter( n.c_str(), pel, "Rec709RGB_Linear" );
				alphaSource = n;
				GlobalLog()->PrintEx( eLog_Warning,
					"GLTFSceneImporter:: material `%s` is alphaMode=MASK but has no baseColor "
					"texture; alpha test degraded to material-wide pass/fail at factor=%.3f",
					matName.c_str(), factorAlpha );
			}

			// Spec semantic: alpha < cutoff discards, where alpha = textureAlpha
			// * factorAlpha.  Painter system doesn't expose A, so we use
			// max(R,G,B) of (texture * factorRGB) as the alpha proxy already
			// captured in `alphaSource`.  factorAlpha (the alpha component of
			// baseColorFactor) modulates the threshold inversely: discard when
			// `proxy < cutoff / factorAlpha`.  factorAlpha == 0 → effective
			// cutoff is huge so nothing keeps (whole material transparent),
			// guarded explicitly to avoid divide-by-zero.
			double effCutoff;
			if( factorAlpha < 1e-6 ) {
				effCutoff = 1e30;	// always discard
			} else {
				effCutoff = (double)mat.alpha_cutoff / factorAlpha;
			}

			const std::string opName = matName + ".alphatest";
			const bool opOK = job.AddAlphaTestShaderOp(
				opName.c_str(), alphaSource.c_str(), effCutoff );

			// Compose [DefaultEmission, alpha_test, DefaultDirectLighting]
			// so emitters still light through the alpha and the BSDF only
			// runs for opaque hits.  If the op or shader registration fails
			// (typically a missing painter), skip the per-material shader so
			// the object falls back to the global shader and renders as
			// opaque -- preferable to silently dropping the mesh because the
			// shader name is left dangling for AddObjectMatrix to look up.
			bool shaderOK = false;
			if( opOK ) {
				const std::string shaderName = matName + ".shader";
				const char* ops[] = { "DefaultEmission", opName.c_str(), "DefaultDirectLighting" };
				shaderOK = job.AddStandardShader( shaderName.c_str(), 3, ops );
			}
			if( !shaderOK ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"GLTFSceneImporter:: failed to wire alpha-test shader for material `%s`; "
					"surface will render as opaque", matName.c_str() );
			}
		} else if( mat.alpha_mode == cgltf_alpha_mode_blend ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"GLTFSceneImporter:: material `%s` uses alphaMode = BLEND, which is out of "
				"Phase 3 scope; treating as OPAQUE", matName.c_str() );
		}

		return true;
	}

	// ---------- light + camera placement ----------

	bool CreateLightForNode(
		IJob&				job,
		const std::string&  prefix,
		size_t              lightIdx,
		const cgltf_light*  light,
		const cgltf_float   worldMatrix[16] )
	{
		// World-space position is column 3 of the world matrix.
		const double px = worldMatrix[12];
		const double py = worldMatrix[13];
		const double pz = worldMatrix[14];

		// glTF light shines down its local -Z axis.  Multiply (0,0,-1,0) by
		// the world matrix's upper-3x3 to get the world-space SHINE direction
		// (the direction the rays go after leaving the source).  Note this is
		// NOT the same as RISE's `direction` parameter on directional_light,
		// which points the OTHER way (from surface to light source) — see
		// the directional case below.
		const double dx = -worldMatrix[8];
		const double dy = -worldMatrix[9];
		const double dz = -worldMatrix[10];
		// Normalise (small drift on non-uniform-scale nodes).
		const double dlen = std::sqrt( dx*dx + dy*dy + dz*dz );
		const double dnx = (dlen > 1e-8) ? dx / dlen : 0;
		const double dny = (dlen > 1e-8) ? dy / dlen : 0;
		const double dnz = (dlen > 1e-8) ? dz / dlen : -1;

		const std::string name = LightName( prefix, lightIdx );
		// glTF light color is documented as linear sRGB (linear Rec.709).
		// RISE's `Add*Light` methods treat the supplied triple as sRGB,
		// gamma-decoding it on use -- so we'd lose ~58% of the intensity
		// without compensation.  Apply the sRGB OETF (linear -> sRGB
		// encoded) to the linear glTF color so RISE's downstream decode
		// recovers the original linear value.  This is the standard
		// Rec.709 OETF approximation; it deliberately matches RISE's
		// own sRGBPel <-> linear roundtrip.
		auto toSRGB = []( double linear ) -> double {
			if( linear <= 0.0031308 ) return 12.92 * linear;
			return 1.055 * std::pow( linear, 1.0 / 2.4 ) - 0.055;
		};
		const double color[3] = {
			toSRGB( (double)light->color[0] ),
			toSRGB( (double)light->color[1] ),
			toSRGB( (double)light->color[2] ) };
		// glTF intensity units: cd for point/spot, lm/m² for directional.
		// RISE's "power" is a multiplier on color; pass intensity directly.
		const double power = (double)light->intensity;
		const double pos[3] = { px, py, pz };
		const double dir[3] = { dnx, dny, dnz };

		switch( light->type ) {
			case cgltf_light_type_directional:
			{
				// RISE's DirectionalLight (DirectionalLight.cpp:48) treats
				// `direction` as the vector pointing FROM the surface TO the
				// light source -- a surface is lit when N · direction > 0.
				// The shine direction we computed above goes the opposite
				// way, so flip the sign before passing it through.
				const double dirArr[3] = { -dnx, -dny, -dnz };
				return job.AddDirectionalLight( name.c_str(), power, color, dirArr );
			}
			case cgltf_light_type_point:
			{
				const double posArr[3] = { px, py, pz };
				return job.AddPointOmniLight( name.c_str(), power, color, posArr,
					/*shootPhotons*/ false );
			}
			case cgltf_light_type_spot:
			{
				const double posArr[3] = { px, py, pz };
				// glTF spot direction is the local -Z axis transformed to
				// world; the spot's "focus point" is any point along that
				// direction beyond the source.  Use distance 1 -- the
				// inner/outer cone angles are what actually define the cone
				// shape; the focus point only sets the central axis.
				const double focArr[3] = { px + dnx, py + dny, pz + dnz };
				const double inner = (double)light->spot_inner_cone_angle;
				const double outer = (double)light->spot_outer_cone_angle;
				return job.AddPointSpotLight( name.c_str(), power, color,
					focArr, inner, outer, posArr,
					/*shootPhotons*/ false );
			}
			default:
				return false;
		}
	}
} // namespace

GLTFSceneImporter::GLTFSceneImporter( const char* glbPath ) :
  szFilename( GlobalMediaPathLocator().Find( glbPath ).c_str() )
{
}

GLTFSceneImporter::~GLTFSceneImporter()
{
}

bool GLTFSceneImporter::Import( IJob& job, const GLTFImportOptions& opts )
{
	cgltf_options copts = {};
	cgltf_data*   data  = NULL;

	cgltf_result r = cgltf_parse_file( &copts, szFilename.c_str(), &data );
	if( r != cgltf_result_success ) {
		GlobalLog()->PrintEx( eLog_Error,
			"GLTFSceneImporter:: cgltf_parse_file failed for `%s` (cgltf_result=%d)",
			szFilename.c_str(), (int)r );
		return false;
	}
	r = cgltf_load_buffers( &copts, data, szFilename.c_str() );
	if( r != cgltf_result_success ) {
		GlobalLog()->PrintEx( eLog_Error,
			"GLTFSceneImporter:: cgltf_load_buffers failed for `%s` (cgltf_result=%d)",
			szFilename.c_str(), (int)r );
		cgltf_free( data );
		return false;
	}
	r = cgltf_validate( data );
	if( r != cgltf_result_success ) {
		GlobalLog()->PrintEx( eLog_Error,
			"GLTFSceneImporter:: cgltf_validate failed for `%s` (cgltf_result=%d)",
			szFilename.c_str(), (int)r );
		cgltf_free( data );
		return false;
	}

	// Skin / animation / morph-target features are out of scope for the
	// importer; warn (loudly, once) so users with animated assets see why
	// their character renders at bind pose with no motion.
	if( data->skins_count > 0 ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"GLTFSceneImporter:: %u skin(s) ignored in `%s`; geometry rendered at bind pose "
			"(skinning support is Phase 4+)",
			(unsigned)data->skins_count, szFilename.c_str() );
	}
	if( data->animations_count > 0 ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"GLTFSceneImporter:: %u animation(s) ignored in `%s` (animation support is Phase 4+)",
			(unsigned)data->animations_count, szFilename.c_str() );
	}
	for( size_t mi = 0; mi < data->meshes_count; ++mi ) {
		const cgltf_mesh& m = data->meshes[ mi ];
		for( size_t pi = 0; pi < m.primitives_count; ++pi ) {
			if( m.primitives[ pi ].targets_count > 0 ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"GLTFSceneImporter:: morph targets on mesh %u primitive %u in `%s` "
					"are ignored (morph-target support is Phase 4+)",
					(unsigned)mi, (unsigned)pi, szFilename.c_str() );
				goto morphWarned;	// once-per-file is enough
			}
		}
	}
	morphWarned:;

	const std::string& prefix = opts.namePrefix;

	// Materials -- create up front (de-duplicates across primitives).
	if( opts.importMaterials ) {
		for( size_t mi = 0; mi < data->materials_count; ++mi ) {
			CreateMaterial( job, prefix, data, szFilename, mi );
		}
	}

	// Pick the scene to walk.  `kSceneIndexDefault` (the option's default
	// value) means "use the file's default scene", which is the cgltf
	// `data->scene` pointer (parsed from the glTF JSON's top-level `"scene"`
	// field) if the file declares one; falls back to scenes[0] otherwise.
	// Any other sceneIndex value is treated as an explicit array index and
	// validated against `scenes_count`.
	const cgltf_scene* scene = NULL;
	if( opts.sceneIndex == GLTFImportOptions::kSceneIndexDefault ) {
		if( data->scene ) {
			scene = data->scene;
		} else if( data->scenes_count > 0 ) {
			scene = &data->scenes[0];
		}
	} else if( opts.sceneIndex < data->scenes_count ) {
		scene = &data->scenes[ opts.sceneIndex ];
	} else {
		GlobalLog()->PrintEx( eLog_Warning,
			"GLTFSceneImporter:: scene_index %u out of range "
			"(file has %u scene(s)); falling back to default",
			opts.sceneIndex, (unsigned)data->scenes_count );
		if( data->scene ) {
			scene = data->scene;
		} else if( data->scenes_count > 0 ) {
			scene = &data->scenes[0];
		}
	}
	if( !scene ) {
		GlobalLog()->PrintEasyError(
			"GLTFSceneImporter:: file has no scene to import" );
		cgltf_free( data );
		return false;
	}

	// Recursive node walk (column-major matrix accumulation).
	bool sawCamera = false;

	struct Walker
	{
		IJob&                    job;
		const std::string&       prefix;
		const cgltf_data*        data;
		const std::string&       glbPath;
		const GLTFImportOptions& opts;
		bool&                    sawCamera;

		void Walk( const cgltf_node* node, const cgltf_float parentWorld[16] )
		{
			cgltf_float local[16]; NodeLocalMatrix( node, local );
			cgltf_float world[16]; Mat4Mul( parentWorld, local, world );

			const size_t nodeIdx = (size_t)( node - data->nodes );

			// Mesh
			if( opts.importMeshes && node->mesh ) {
				const size_t meshIdx = (size_t)( node->mesh - data->meshes );

				// Phase 3: pass the column-major node-world matrix to
				// AddObjectMatrix verbatim.  No more lossy Euler decomposition.
				double matD[16];
				for( int i = 0; i < 16; ++i ) matD[i] = (double)world[i];

				for( size_t pi = 0; pi < node->mesh->primitives_count; ++pi ) {
					const cgltf_primitive& prim = node->mesh->primitives[ pi ];
					const std::string geom = GeomName( prefix, meshIdx, pi );

					// Forward material.double_sided so thin cards / leaves /
					// cloth / decals render with backfaces.  Default to false
					// (opaque single-sided) when no material is bound.
					const bool doubleSided = ( prim.material && prim.material->double_sided );

					// Phase 1 loader; flip_v=TRUE because glTF V-up vs RISE V-down.
					const bool gOK = job.AddGLTFTriangleMeshGeometry(
						geom.c_str(), glbPath.c_str(),
						(unsigned int)meshIdx, (unsigned int)pi,
						doubleSided,
						/*face_normals*/ false,
						/*flip_v*/ true );
					if( !gOK ) {
						continue;
					}

					std::string matName = "none";
					std::string modName = "none";
					std::string shaderName = "none";
					if( opts.importMaterials && prim.material ) {
						const size_t mi = (size_t)( prim.material - data->materials );
						matName = MaterialName( prefix, mi );
						if( opts.importNormalMaps && prim.material->normal_texture.texture ) {
							modName = ModifierName( prefix, mi );
						}
						// Per-material shader override exists only when the
						// material was imported with alphaMode = MASK; CreateMaterial
						// registers <matName>.shader in that case.
						if( prim.material->alpha_mode == cgltf_alpha_mode_mask ) {
							shaderName = matName + ".shader";
						}
					}

					RadianceMapConfig rmc;
					const std::string objName = ObjectName( prefix, nodeIdx, pi );
					job.AddObjectMatrix(
						objName.c_str(), geom.c_str(),
						matName == "none" ? NULL : matName.c_str(),
						modName == "none" ? NULL : modName.c_str(),
						shaderName == "none" ? NULL : shaderName.c_str(),
						rmc, matD,
						/*casts*/ true, /*receives*/ true );
				}
			}

			// Light
			if( opts.importLights && node->light ) {
				const size_t lightIdx = (size_t)( node->light - data->lights );
				CreateLightForNode( job, prefix, lightIdx, node->light, world );
			}

			// Camera (first only)
			if( opts.importCameras && node->camera && !sawCamera ) {
				const cgltf_camera* cam = node->camera;
				if( cam->type == cgltf_camera_type_perspective ) {
					// glTF cameras look down -Z in their local frame; up is +Y.
					// Transform those local axes via the world matrix to get
					// the world-space placement.
					const double cx = world[12], cy = world[13], cz = world[14];
					const double lx = cx - world[8],  ly = cy - world[9],  lz = cz - world[10];
					const double ux = world[4], uy = world[5], uz = world[6];

					const double location[3] = { cx, cy, cz };
					const double lookat[3]   = { lx, ly, lz };
					const double up[3]       = { ux, uy, uz };
					const double orientation[3] = { 0, 0, 0 };
					const double target_orient[2] = { 0, 0 };

					// glTF stores yfov in radians; SetPinholeCamera takes fov
					// in radians too (despite the chunk's "degrees" hint).
					const double yfov_rad = (double)cam->data.perspective.yfov;

					// Resolution / pixel rate / scanning rate are RISE
					// internals; pick benign defaults.  Users override via a
					// pinhole_camera chunk after the import.
					job.SetPinholeCamera(
						location, lookat, up,
						yfov_rad,
						/*xres*/ 800, /*yres*/ 600,
						/*pixelAR*/ 1.0,
						/*exposure*/ 1.0,
						/*scanningRate*/ 0.0,
						/*pixelRate*/ 0.0,
						orientation, target_orient );
					sawCamera = true;
				} else {
					GlobalLog()->PrintEasyWarning(
						"GLTFSceneImporter:: orthographic glTF cameras are not yet "
						"wired up; skipping" );
				}
			}

			for( cgltf_size c = 0; c < node->children_count; ++c ) {
				Walk( node->children[c], world );
			}
		}
	};

	cgltf_float identity[16]; Mat4Identity( identity );
	Walker w = { job, prefix, data, szFilename, opts, sawCamera };
	for( size_t r = 0; r < scene->nodes_count; ++r ) {
		w.Walk( scene->nodes[r], identity );
	}

	cgltf_free( data );
	return true;
}
